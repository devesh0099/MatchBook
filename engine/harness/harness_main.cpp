// harness/harness_main.cpp — the `harness` binary.
//
//   harness verify --stream F --engine ./sub.so [--oracle builtin] [--wall-time 45]
//   harness verify --seed S --profile P --events N --engine ./sub.so
//
// A stream can be read from a file or generated in process from its seed. The
// correctness pool uses the second form: the seed IS the stream, so there is no
// reason to round-trip 240MB through a disk.

#include <signal.h>
#include <unistd.h>

#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "generator/generator.h"
#include "harness/engine_loader.h"
#include "harness/verify.h"
#include "mebench/wire.h"

namespace {

// Exit codes. The worker keys off these rather than parsing prose, so a timeout
// stays distinguishable from a wrong answer all the way up to the participant's
// results panel (plan section 2).
constexpr int kExitPassed = 0;
constexpr int kExitFailed = 1;
constexpr int kExitUsage = 2;
constexpr int kExitTimeout = 3;

bool g_timeout_json = false;

// The in-loop deadline in verify() only catches an engine that is slow. An
// engine stuck inside on_new never returns control to the harness at all, so
// the limit has to be enforced asynchronously. Everything here is
// async-signal-safe: no printf, no allocation.
extern "C" void on_alarm(int) {
  static const char kText[] =
      "TIMEOUT\n\nThe engine stopped returning. This is a liveness failure, not a wrong\n"
      "answer: look for an unbounded loop, not for a matching bug.\n";
  static const char kJson[] = "{\"outcome\":\"timeout\",\"passed\":0}\n";
  const char* msg = g_timeout_json ? kJson : kText;
  const size_t len = g_timeout_json ? sizeof(kJson) - 1 : sizeof(kText) - 1;
  ssize_t written = write(STDOUT_FILENO, msg, len);
  (void)written;
  _exit(kExitTimeout);
}

void usage() {
  std::fprintf(
      stderr,
      "usage:\n"
      "  harness verify --engine ENGINE [stream options] [--wall-time S] [--json]\n"
      "\n"
      "  --engine ENGINE      path to a submission .so, or 'builtin' for the reference\n"
      "  --oracle ENGINE      what to compare against (default: builtin)\n"
      "\n"
      "  stream options, either:\n"
      "    --stream FILE      a stream written by `gen`\n"
      "  or:\n"
      "    --seed S --profile P --events N    generate it in process\n"
      "\n"
      "  --wall-time S        liveness limit in seconds (default 45, 0 disables).\n"
      "                       Hitting it is reported as a timeout, never as a wrong answer\n"
      "  --snapshot-every N   diff the two books every N events (default 10000, 0 disables)\n"
      "  --no-shrink          skip binary-searching the counterexample\n"
      "  --json               machine-readable result\n");
}

bool parse_u64(const char* s, uint64_t& out) {
  char* end = nullptr;
  const unsigned long long v = std::strtoull(s, &end, 10);
  if (!end || *end != '\0') return false;
  out = v;
  return true;
}

int run_verify(int argc, char** argv) {
  std::string engine_spec, oracle_spec = "builtin", stream_path, profile_name = "balanced";
  uint64_t seed = 0, events = 0;
  bool have_seed = false;
  mebench::harness::VerifyOptions opts;
  bool as_json = false;

  for (int i = 0; i < argc; ++i) {
    const std::string a = argv[i];
    const bool has_next = (i + 1) < argc;
    if (a == "--engine" && has_next) {
      engine_spec = argv[++i];
    } else if (a == "--oracle" && has_next) {
      oracle_spec = argv[++i];
    } else if (a == "--stream" && has_next) {
      stream_path = argv[++i];
    } else if (a == "--seed" && has_next) {
      if (!parse_u64(argv[++i], seed)) return usage(), kExitUsage;
      have_seed = true;
    } else if (a == "--events" && has_next) {
      if (!parse_u64(argv[++i], events)) return usage(), kExitUsage;
    } else if (a == "--profile" && has_next) {
      profile_name = argv[++i];
    } else if (a == "--wall-time" && has_next) {
      opts.wall_time_s = std::strtod(argv[++i], nullptr);
    } else if (a == "--snapshot-every" && has_next) {
      uint64_t n = 0;
      if (!parse_u64(argv[++i], n)) return usage(), kExitUsage;
      opts.snapshot_every = n;
    } else if (a == "--no-shrink") {
      opts.shrink = false;
    } else if (a == "--json") {
      as_json = true;
    } else {
      std::fprintf(stderr, "unknown argument: %s\n", a.c_str());
      usage();
      return 2;
    }
  }

  if (engine_spec.empty()) {
    std::fprintf(stderr, "--engine is required\n");
    usage();
    return 2;
  }

  std::vector<mebench::WireEvent> stream;
  if (!stream_path.empty()) {
    mebench::StreamHeader header{};
    std::string err;
    if (!mebench::generator::read_stream(stream_path, header, stream, err)) {
      std::fprintf(stderr, "%s\n", err.c_str());
      return 2;
    }
  } else if (have_seed && events > 0) {
    mebench::Profile profile{};
    if (!mebench::generator::parse_profile(profile_name, profile)) {
      std::fprintf(stderr, "unknown profile: %s\n", profile_name.c_str());
      return 2;
    }
    mebench::generator::Generator gen(seed, profile);
    stream = gen.generate(events);
  } else {
    std::fprintf(stderr, "need either --stream FILE or --seed S --events N\n");
    usage();
    return 2;
  }

  std::string err;
  auto submission = mebench::harness::EngineSource::open(engine_spec, err);
  if (!submission) {
    // A load failure is the participant's problem to see, but it is not a
    // divergence — keep the two outcomes distinguishable.
    if (as_json) {
      std::printf("{\"outcome\":\"load_failed\",\"passed\":0,\"message\":\"%s\"}\n", err.c_str());
    } else {
      std::fprintf(stderr, "%s\n", err.c_str());
    }
    return 2;
  }
  auto oracle = mebench::harness::EngineSource::open(oracle_spec, err);
  if (!oracle) {
    std::fprintf(stderr, "%s\n", err.c_str());
    return 2;
  }

  // Arm the asynchronous limit a little above the harness's own soft deadline,
  // so a merely slow engine still gets the readable report from verify() and
  // only a genuinely stuck one is killed here.
  if (opts.wall_time_s > 0) {
    g_timeout_json = as_json;
    struct sigaction sa {};
    sa.sa_handler = on_alarm;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGALRM, &sa, nullptr);
    alarm(static_cast<unsigned>(std::ceil(opts.wall_time_s)) + 5u);
  }

  const auto result = mebench::harness::verify(stream, *submission, *oracle, opts);
  alarm(0);

  if (as_json) {
    std::printf("%s\n", mebench::harness::format_json(result).c_str());
  } else {
    std::printf("%s", mebench::harness::format_report(result).c_str());
  }
  if (result.outcome == mebench::harness::VerifyOutcome::Timeout) return kExitTimeout;
  return result.passed() ? kExitPassed : kExitFailed;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    usage();
    return 2;
  }
  const std::string mode = argv[1];
  if (mode == "verify") return run_verify(argc - 2, argv + 2);
  if (mode == "-h" || mode == "--help") {
    usage();
    return 0;
  }
  std::fprintf(stderr, "unknown mode: %s\n", mode.c_str());
  usage();
  return 2;
}
