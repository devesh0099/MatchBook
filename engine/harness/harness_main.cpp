// harness/harness_main.cpp — the `harness` binary.
//
//   harness verify --stream F --engine ./sub.so [--oracle builtin] [--wall-time 45]
//   harness verify --seed S --profile P --events N --engine ./sub.so
//
// A stream can be read from a file, from an inherited file descriptor, or
// generated in process from its seed.
//
// SEED HANDLING IS A SECURITY BOUNDARY. The submission is dlopen()ed into this
// process, so it can read /proc/self/cmdline. The generator is published, so a
// seed on the command line is complete knowledge of every future event —
// enough to precompute output or prefetch the stream and post a latency that
// measures nothing. That invalidates the leaderboard.
//
//   --seed      ONLY for visible lanes: Run, local iteration, published seeds.
//   --stream-fd for hidden streams. The parent opens the stream, passes the fd,
//               and drops privileges; the box UID never gets a readable path
//               and the seed never appears in argv (plan section 10).
//
// The correctness lane is the softer case — its seed is fresh per submission
// and knowing it does not make a wrong engine right — but the ranked lane must
// always use --stream-fd.

#include <signal.h>
#include <unistd.h>

#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "generator/generator.h"
#include "generator/validate.h"
#include "harness/engine_loader.h"
#include <memory>

#include "common/decode.h"
#include "harness/bench.h"
#include "harness/hash_sink.h"
#include "harness/solution.h"
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
constexpr int kExitUnhealthy = 4;  // the node, not the submission
constexpr int kExitDeadline = 5;   // the gate: correct so far, too slow to finish

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
      "  harness bench  --engine ENGINE [stream options] [--runs N] [--warmup N]\n"
      "                 [--digest HEX] [-o FILE] [--json]\n"
      "  harness digest --engine ENGINE [stream options]\n"
      "\n"
      "  --engine ENGINE      path to a submission .so, or 'builtin' for the reference\n"
      "  --oracle ENGINE      what to compare against (default: builtin)\n"
      "\n"
      "  stream options, either:\n"
      "    --stream FILE      a stream written by `gen`\n"
      "    --stream-fd N      an inherited fd holding the stream. USE THIS for hidden\n"
      "                       streams: --seed puts the seed in argv, and the loaded\n"
      "                       submission can read /proc/self/cmdline\n"
      "  or:\n"
      "    --seed S --profile P --events N    generate it in process\n"
      "\n"
      "  --wall-time S        liveness limit in seconds (default 45, 0 disables).\n"
      "                       Hitting it is reported as a timeout, never as a wrong answer\n"
      "  --snapshot-every N   diff the two books every N events (default 10000, 0 disables)\n"
      "  --no-shrink          skip binary-searching the counterexample\n"
      "\n"
      "  bench only:\n"
      "  --runs N             ranked runs; the score is the median of their p50s (default 9)\n"
      "  --warmup N           untimed events before measuring; defaults to what the\n"
      "                       stream's own profile needs to fill its book\n"
      "  --digest HEX         the correctness run's digest; a mismatch is its own outcome,\n"
      "                       not a generic failure\n"
      "  --deadline-ms N      per-run wall budget over the whole run, warm-up included.\n"
      "                       A miss is a gate failure (exit 5), reported with percentiles\n"
      "                       up to the cut. 0 disables (default)\n"
      "  --solution FILE      baked answer key from `harness solve`; validates output at\n"
      "                       every checkpoint, so a deadline-cut run is still checked\n"
      "  -o FILE              write the JSON result to FILE\n"
      "  --json               machine-readable result\n"
      "\n"
      "  harness solve  [stream options] -o FILE [--checkpoint-every N]\n"
      "                       run the reference once and bake its answer key: checkpoint\n"
      "                       digests every N events (default 100000) plus the final digest\n"
      "\n"
      "  harness ladder --engine ENGINE --levels FILE [--json]\n"
      "                       climb a level table: each line is\n"
      "                         ID STREAM SOLUTION DEADLINE_MS\n"
      "                       ('#' comments allowed). One attempt per level, in order;\n"
      "                       the climb ends at the first miss. Results stream one line\n"
      "                       per level as it finishes, then a summary\n");
}

bool parse_u64(const char* s, uint64_t& out) {
  char* end = nullptr;
  const unsigned long long v = std::strtoull(s, &end, 10);
  if (!end || *end != '\0') return false;
  out = v;
  return true;
}

bool load_stream(const std::string& stream_path, int stream_fd, bool have_seed, uint64_t seed,
                 const std::string& profile_name, uint64_t events,
                 std::vector<mebench::WireEvent>& out, mebench::Profile& profile_out,
                 uint32_t& live_target_out);

int run_verify(int argc, char** argv) {
  std::string engine_spec, oracle_spec = "builtin", stream_path, profile_name = "balanced";
  uint64_t seed = 0, events = 0;
  int stream_fd = -1;
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
    } else if (a == "--stream-fd" && has_next) {
      stream_fd = std::atoi(argv[++i]);
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
  mebench::Profile stream_profile{};
  uint32_t stream_live_target = 0;
  (void)stream_profile;  // only bench derives anything from these
  (void)stream_live_target;
  if (!load_stream(stream_path, stream_fd, have_seed, seed, profile_name, events, stream,
                   stream_profile, stream_live_target))
    return kExitUsage;

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

// Shared by both modes: a stream is either read from a file or generated in
// process from its seed.
bool load_stream(const std::string& stream_path, int stream_fd, bool have_seed, uint64_t seed,
                 const std::string& profile_name, uint64_t events,
                 std::vector<mebench::WireEvent>& out, mebench::Profile& profile_out,
                 uint32_t& live_target_out) {
  // The profile comes back out because bench derives its default warm-up from
  // it. Reading it off the stream rather than taking it as a flag is what stops
  // the caller's idea of the profile drifting from the stream's.
  mebench::StreamHeader header{};
  if (stream_fd >= 0) {
    // The hidden-stream path: the fd was inherited before privileges dropped,
    // so the submission never sees a path it could open itself, and no seed
    // ever reaches argv.
    std::string err;
    if (!mebench::generator::read_stream_fd(stream_fd, header, out, err)) {
      std::fprintf(stderr, "%s\n", err.c_str());
      return false;
    }
    profile_out = static_cast<mebench::Profile>(header.profile_id);
    live_target_out = static_cast<uint32_t>(header.live_target);
    return true;
  }
  if (!stream_path.empty()) {
    std::string err;
    if (!mebench::generator::read_stream(stream_path, header, out, err)) {
      std::fprintf(stderr, "%s\n", err.c_str());
      return false;
    }
    profile_out = static_cast<mebench::Profile>(header.profile_id);
    live_target_out = static_cast<uint32_t>(header.live_target);
    return true;
  }
  if (have_seed && events > 0) {
    mebench::Profile profile{};
    if (!mebench::generator::parse_profile(profile_name, profile)) {
      std::fprintf(stderr, "unknown profile: %s\n", profile_name.c_str());
      return false;
    }
    mebench::generator::Generator gen(seed, profile);
    out = gen.generate(events);
    profile_out = profile;
    live_target_out = gen.live_target();
    return true;
  }
  std::fprintf(stderr, "need either --stream FILE or --seed S --events N\n");
  return false;
}

// `harness digest` — fold the field-wise hash over an engine's output for a
// stream and print it.
//
// The benchmark node compares every ranked run against the ORACLE's digest for
// that stream, computed once per seed and cached. That is what makes the check
// meaningful across lanes: the correctness lane and the benchmark lane run
// different streams, so a submission's own correctness-run digest could never
// be the thing a 10M cancel_heavy run has to reproduce.
int run_digest(int argc, char** argv) {
  std::string engine_spec = "builtin", stream_path, profile_name = "cancel_heavy";
  uint64_t seed = 0, events = 0;
  int stream_fd = -1;
  bool have_seed = false;

  for (int i = 0; i < argc; ++i) {
    const std::string a = argv[i];
    const bool has_next = (i + 1) < argc;
    if (a == "--engine" && has_next) {
      engine_spec = argv[++i];
    } else if (a == "--stream" && has_next) {
      stream_path = argv[++i];
    } else if (a == "--stream-fd" && has_next) {
      stream_fd = std::atoi(argv[++i]);
    } else if (a == "--seed" && has_next) {
      if (!parse_u64(argv[++i], seed)) return usage(), kExitUsage;
      have_seed = true;
    } else if (a == "--events" && has_next) {
      if (!parse_u64(argv[++i], events)) return usage(), kExitUsage;
    } else if (a == "--profile" && has_next) {
      profile_name = argv[++i];
    } else {
      std::fprintf(stderr, "unknown argument: %s\n", a.c_str());
      return kExitUsage;
    }
  }

  std::vector<mebench::WireEvent> stream;
  mebench::Profile stream_profile{};
  uint32_t stream_live_target = 0;
  (void)stream_profile;  // only bench derives anything from these
  (void)stream_live_target;
  if (!load_stream(stream_path, stream_fd, have_seed, seed, profile_name, events, stream,
                   stream_profile, stream_live_target))
    return kExitUsage;

  std::string err;
  auto engine = mebench::harness::EngineSource::open(engine_spec, err);
  if (!engine) {
    std::fprintf(stderr, "%s\n", err.c_str());
    return kExitUsage;
  }

  std::unique_ptr<mebench::IMatchingEngine> e(engine->create());
  mebench::harness::HashSink sink;
  for (uint64_t i = 0; i < stream.size(); ++i) {
    const mebench::DecodedEvent d = mebench::decode(stream[i], i);
    mebench::dispatch(*e, d, sink);
  }
  std::printf("%016llx\n", static_cast<unsigned long long>(sink.digest()));
  return kExitPassed;
}

int run_bench(int argc, char** argv) {
  std::string engine_spec, stream_path, profile_name = "cancel_heavy", json_path, solution_path;
  uint64_t seed = 0, events = 0;
  int stream_fd = -1;
  bool have_seed = false, as_json = false, warmup_given = false;
  mebench::harness::BenchOptions opts;

  for (int i = 0; i < argc; ++i) {
    const std::string a = argv[i];
    const bool has_next = (i + 1) < argc;
    if (a == "--engine" && has_next) {
      engine_spec = argv[++i];
    } else if (a == "--stream" && has_next) {
      stream_path = argv[++i];
    } else if (a == "--stream-fd" && has_next) {
      stream_fd = std::atoi(argv[++i]);
    } else if (a == "--seed" && has_next) {
      if (!parse_u64(argv[++i], seed)) return usage(), kExitUsage;
      have_seed = true;
    } else if (a == "--events" && has_next) {
      if (!parse_u64(argv[++i], events)) return usage(), kExitUsage;
    } else if (a == "--profile" && has_next) {
      profile_name = argv[++i];
    } else if (a == "--runs" && has_next) {
      uint64_t n = 0;
      if (!parse_u64(argv[++i], n)) return usage(), kExitUsage;
      opts.runs = static_cast<uint32_t>(n);
    } else if (a == "--warmup" && has_next) {
      if (!parse_u64(argv[++i], opts.warmup)) return usage(), kExitUsage;
      warmup_given = true;
    } else if (a == "--digest" && has_next) {
      opts.expected_digest = std::strtoull(argv[++i], nullptr, 16);
      opts.have_expected_digest = true;
    } else if (a == "--deadline-ms" && has_next) {
      opts.deadline_s = std::strtod(argv[++i], nullptr) / 1000.0;
    } else if (a == "--solution" && has_next) {
      solution_path = argv[++i];
    } else if (a == "--no-mlock") {
      opts.lock_memory = false;
    } else if ((a == "--json" || a == "-o") && has_next && a == "-o") {
      json_path = argv[++i];
    } else if (a == "--json") {
      as_json = true;
    } else {
      std::fprintf(stderr, "unknown argument: %s\n", a.c_str());
      usage();
      return kExitUsage;
    }
  }

  if (engine_spec.empty()) {
    std::fprintf(stderr, "--engine is required\n");
    return kExitUsage;
  }

  std::vector<mebench::WireEvent> stream;
  mebench::Profile stream_profile{};
  uint32_t stream_live_target = 0;
  if (!load_stream(stream_path, stream_fd, have_seed, seed, profile_name, events, stream,
                   stream_profile, stream_live_target))
    return kExitUsage;
  // Warm-up defaults to whatever THIS stream's profile needs to fill its book,
  // read from the stream header rather than passed in.
  //
  // A fixed default is wrong by construction: warm-up scales with depth, and
  // cancel_heavy now builds ~300k resting orders, which takes ~3.9M events. Any
  // caller carrying its own constant would silently time several million events
  // of book-filling in every ranked run and report the average of a shallow
  // book and a deep one. Deriving it here means the profile and the warm-up
  // cannot drift apart, because there is only one of them.
  if (!warmup_given) {
    opts.warmup = std::min<uint64_t>(
        mebench::generator::warmup_events(stream_profile, stream_live_target), stream.size() / 2);
  }


  std::string err;
  mebench::harness::Solution sol;
  if (!solution_path.empty()) {
    if (!mebench::harness::read_solution(solution_path, sol, err)) {
      std::fprintf(stderr, "%s\n", err.c_str());
      return kExitUsage;
    }
    if (sol.events != stream.size()) {
      std::fprintf(stderr, "solution describes %" PRIu64 " events, stream has %zu\n", sol.events,
                   stream.size());
      return kExitUsage;
    }
    opts.solution = &sol;
  }

  auto engine = mebench::harness::EngineSource::open(engine_spec, err);
  if (!engine) {
    std::fprintf(stderr, "%s\n", err.c_str());
    return kExitUsage;
  }

  // The asynchronous backstop for an engine stuck inside a callback; bench()
  // arms the per-run alarm, this installs where it lands.
  if (opts.deadline_s > 0) {
    g_timeout_json = as_json;
    struct sigaction sa {};
    sa.sa_handler = on_alarm;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGALRM, &sa, nullptr);
  }

  const auto r = mebench::harness::bench(stream, *engine, opts);

  if (!json_path.empty()) {
    if (std::FILE* f = std::fopen(json_path.c_str(), "wb")) {
      std::fprintf(f, "%s\n", mebench::harness::format_bench_json(r).c_str());
      std::fclose(f);
    }
  }
  if (as_json) {
    std::printf("%s\n", mebench::harness::format_bench_json(r).c_str());
  } else {
    std::printf("%s", mebench::harness::format_bench_report(r).c_str());
  }

  switch (r.outcome) {
    case mebench::harness::BenchOutcome::Ok: return kExitPassed;
    case mebench::harness::BenchOutcome::DigestMismatch: return kExitFailed;
    case mebench::harness::BenchOutcome::NodeUnhealthy: return kExitUnhealthy;
    case mebench::harness::BenchOutcome::DeadlineMissed: return kExitDeadline;
  }
  return kExitFailed;
}

// `harness solve` — bake the answer key for a fixed stream. The reference runs
// ONCE, here, at bake time; judge time is a table lookup (solution.h).
int run_solve(int argc, char** argv) {
  std::string stream_path, out_path, profile_name = "cancel_heavy";
  uint64_t seed = 0, events = 0, checkpoint_every = 100000;
  int stream_fd = -1;
  bool have_seed = false;

  for (int i = 0; i < argc; ++i) {
    const std::string a = argv[i];
    const bool has_next = (i + 1) < argc;
    if (a == "--stream" && has_next) {
      stream_path = argv[++i];
    } else if (a == "--stream-fd" && has_next) {
      stream_fd = std::atoi(argv[++i]);
    } else if (a == "--seed" && has_next) {
      if (!parse_u64(argv[++i], seed)) return usage(), kExitUsage;
      have_seed = true;
    } else if (a == "--events" && has_next) {
      if (!parse_u64(argv[++i], events)) return usage(), kExitUsage;
    } else if (a == "--profile" && has_next) {
      profile_name = argv[++i];
    } else if (a == "--checkpoint-every" && has_next) {
      if (!parse_u64(argv[++i], checkpoint_every) || checkpoint_every == 0)
        return usage(), kExitUsage;
    } else if ((a == "-o" || a == "--out") && has_next) {
      out_path = argv[++i];
    } else {
      std::fprintf(stderr, "unknown argument: %s\n", a.c_str());
      return kExitUsage;
    }
  }
  if (out_path.empty()) {
    std::fprintf(stderr, "-o FILE is required\n");
    return kExitUsage;
  }

  std::vector<mebench::WireEvent> stream;
  mebench::Profile stream_profile{};
  uint32_t stream_live_target = 0;
  if (!load_stream(stream_path, stream_fd, have_seed, seed, profile_name, events, stream,
                   stream_profile, stream_live_target))
    return kExitUsage;

  std::string err;
  auto engine = mebench::harness::EngineSource::open("builtin", err);
  if (!engine) {
    std::fprintf(stderr, "%s\n", err.c_str());
    return kExitUsage;
  }

  mebench::harness::Solution sol;
  sol.seed = seed;
  sol.profile_id = static_cast<uint32_t>(stream_profile);
  sol.events = stream.size();
  sol.checkpoint_interval = checkpoint_every;

  std::unique_ptr<mebench::IMatchingEngine> e(engine->create());
  mebench::harness::HashSink sink;
  for (uint64_t i = 0; i < stream.size(); ++i) {
    const mebench::DecodedEvent d = mebench::decode(stream[i], i);
    mebench::dispatch(*e, d, sink);
    if ((i + 1) % checkpoint_every == 0) sol.checkpoints.push_back(sink.digest());
  }
  sol.final_digest = sink.digest();

  if (!mebench::harness::write_solution(out_path, sol, err)) {
    std::fprintf(stderr, "%s\n", err.c_str());
    return kExitFailed;
  }
  std::printf("wrote %s: %" PRIu64 " events, %zu checkpoints every %" PRIu64
              ", final digest %016llx\n",
              out_path.c_str(), sol.events, sol.checkpoints.size(), sol.checkpoint_interval,
              static_cast<unsigned long long>(sol.final_digest));
  return kExitPassed;
}

// `harness ladder` — Phase II (PLAN-measurement-redesign §3): walk the level
// table in order, one attempt per level, stop at the first miss. Each level's
// result is printed AS IT FINISHES: if a later level's stuck engine dies on
// the SIGALRM backstop, everything already climbed is on stdout.
int run_ladder(int argc, char** argv) {
  std::string engine_spec, levels_path;
  bool as_json = false;

  for (int i = 0; i < argc; ++i) {
    const std::string a = argv[i];
    const bool has_next = (i + 1) < argc;
    if (a == "--engine" && has_next) {
      engine_spec = argv[++i];
    } else if (a == "--levels" && has_next) {
      levels_path = argv[++i];
    } else if (a == "--json") {
      as_json = true;
    } else {
      std::fprintf(stderr, "unknown argument: %s\n", a.c_str());
      return kExitUsage;
    }
  }
  if (engine_spec.empty() || levels_path.empty()) {
    std::fprintf(stderr, "--engine and --levels are required\n");
    return kExitUsage;
  }

  struct Level {
    uint64_t id;
    std::string stream, solution;
    double deadline_ms;
  };
  std::vector<Level> levels;
  {
    std::FILE* f = std::fopen(levels_path.c_str(), "r");
    if (!f) {
      std::fprintf(stderr, "could not open %s\n", levels_path.c_str());
      return kExitUsage;
    }
    char line[4096];
    while (std::fgets(line, sizeof line, f)) {
      char s1[2048], s2[2048];
      unsigned long long id = 0;
      double dl = 0;
      if (line[0] == '#' || line[0] == '\n') continue;
      if (std::sscanf(line, "%llu %2047s %2047s %lf", &id, s1, s2, &dl) != 4) {
        std::fprintf(stderr, "bad level line: %s", line);
        std::fclose(f);
        return kExitUsage;
      }
      levels.push_back(Level{id, s1, s2, dl});
    }
    std::fclose(f);
  }

  std::string err;
  auto engine = mebench::harness::EngineSource::open(engine_spec, err);
  if (!engine) {
    std::fprintf(stderr, "%s\n", err.c_str());
    return kExitUsage;
  }

  // Backstop for an engine stuck inside a callback; bench() arms it per run.
  g_timeout_json = as_json;
  struct sigaction sa {};
  sa.sa_handler = on_alarm;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGALRM, &sa, nullptr);

  uint64_t max_level = 0;
  double top_p50 = 0, top_p95 = 0, top_p99 = 0;
  bool stopped = false;

  for (const auto& lv : levels) {
    if (stopped) break;

    // Streams load per level and free before the next: the top rungs are the
    // big ones, and two of them resident at once would double peak RSS.
    std::vector<mebench::WireEvent> stream;
    mebench::Profile profile{};
    uint32_t live_target = 0;
    if (!load_stream(lv.stream, -1, false, 0, "", 0, stream, profile, live_target))
      return kExitUsage;

    mebench::harness::Solution sol;
    if (!mebench::harness::read_solution(lv.solution, sol, err)) {
      std::fprintf(stderr, "%s\n", err.c_str());
      return kExitUsage;
    }
    if (sol.events != stream.size()) {
      std::fprintf(stderr, "level %" PRIu64 ": solution/stream event count mismatch\n", lv.id);
      return kExitUsage;
    }

    mebench::harness::BenchOptions opts;
    opts.runs = 1;  // one attempt per level, by design
    opts.deadline_s = lv.deadline_ms / 1000.0;
    opts.solution = &sol;
    opts.warmup = std::min<uint64_t>(mebench::generator::warmup_events(profile, live_target),
                                     stream.size() / 2);

    const auto r = mebench::harness::bench(stream, *engine, opts);
    const char* outcome = mebench::harness::bench_outcome_name(r.outcome);
    const auto& run = r.runs.empty() ? mebench::harness::RunResult{} : r.runs.back();

    if (as_json) {
      std::printf("{\"level\":%" PRIu64 ",\"outcome\":\"%s\",\"events\":%zu,\"deadline_ms\":%g"
                  ",\"p50_ns\":%g,\"p95_ns\":%g,\"p99_ns\":%g,\"wall_s\":%g"
                  ",\"events_processed\":%" PRIu64 ",\"checked_events\":%" PRIu64 "}\n",
                  lv.id, outcome, stream.size(), lv.deadline_ms, run.p50_ns, run.p95_ns,
                  run.p99_ns, run.wall_s, run.events_processed, run.checked_events);
    } else {
      if (r.outcome == mebench::harness::BenchOutcome::Ok) {
        std::printf("level %-3" PRIu64 " PASS      p95 %.1f ns · p50 %.1f ns · p99 %.1f ns · "
                    "%.2f s\n",
                    lv.id, run.p95_ns, run.p50_ns, run.p99_ns, run.wall_s);
      } else {
        std::printf("level %-3" PRIu64 " %-9s %" PRIu64 " of %zu events · %s\n", lv.id, outcome,
                    run.events_processed, stream.size(), r.notes.c_str());
      }
    }
    std::fflush(stdout);

    if (r.outcome == mebench::harness::BenchOutcome::Ok) {
      max_level = lv.id;
      top_p50 = run.p50_ns;
      top_p95 = run.p95_ns;
      top_p99 = run.p99_ns;
    } else {
      stopped = true;
      if (r.outcome == mebench::harness::BenchOutcome::NodeUnhealthy) return kExitUnhealthy;
    }
  }

  if (as_json) {
    std::printf("{\"summary\":1,\"max_level\":%" PRIu64 ",\"top_p95_ns\":%g,\"top_p50_ns\":%g"
                ",\"top_p99_ns\":%g,\"climb_complete\":%d}\n",
                max_level, top_p95, top_p50, top_p99, stopped ? 0 : 1);
  } else {
    std::printf("\nclimbed to level %" PRIu64 "%s\n", max_level,
                stopped ? "" : " — cleared the whole table");
  }
  return kExitPassed;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    usage();
    return 2;
  }
  const std::string mode = argv[1];
  if (mode == "verify") return run_verify(argc - 2, argv + 2);
  if (mode == "bench") return run_bench(argc - 2, argv + 2);
  if (mode == "digest") return run_digest(argc - 2, argv + 2);
  if (mode == "solve") return run_solve(argc - 2, argv + 2);
  if (mode == "ladder") return run_ladder(argc - 2, argv + 2);
  if (mode == "-h" || mode == "--help") {
    usage();
    return 0;
  }
  std::fprintf(stderr, "unknown mode: %s\n", mode.c_str());
  usage();
  return 2;
}
