// generator/gen_main.cpp — the `gen` binary.
//
//   gen --seed S --profile P --events N -o stream.bin [--validate] [--live-target N]
//
// Participants get this binary, not 240MB of stream files: same seed, same
// bytes, everywhere. Hidden streams are simply unpublished seeds.

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "generator/generator.h"
#include "generator/validate.h"

namespace {

void usage() {
  std::fprintf(stderr,
               "usage: gen --seed S --profile P --events N -o FILE [--validate]\n"
               "\n"
               "  --seed S       stream seed; the same seed produces the same bytes on\n"
               "                 Linux, macOS and the server\n"
               "  --profile P    balanced | cancel_heavy | sweep_heavy | deep_book\n"
               "                 (balanced is used for correctness, cancel_heavy for the\n"
               "                  ranked benchmark)\n"
               "  --events N     number of events; 100k iterates fast, 10M is the real run\n"
               "  -o FILE        output path; omit with --validate to generate in memory\n"
               "  --validate     run the reference over the stream and check it is worth\n"
               "                 trusting (fill rate, bounded depth, every adversarial\n"
               "                 injection present and having its intended effect)\n"
               "  --live-target N  override the profile's per-session resting-order target.\n"
               "                 Book depth is roughly 0.62 x sessions x this, so it is the\n"
               "                 single knob for the depth/latency sweep. It is recorded in\n"
               "                 the stream header, so `bench` still derives the right\n"
               "                 untimed warm-up for the stream it was given\n");
}

bool parse_u64(const char* s, uint64_t& out) {
  char* end = nullptr;
  const unsigned long long v = std::strtoull(s, &end, 10);
  if (!end || *end != '\0') return false;
  out = v;
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  uint64_t seed = 0;
  uint64_t events = 1'000'000;
  std::string profile_name = "balanced";
  std::string out_path;
  uint64_t live_target = 0;
  bool do_validate = false;

  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    const bool has_next = (i + 1) < argc;
    if (a == "--seed" && has_next) {
      if (!parse_u64(argv[++i], seed)) return usage(), 2;
    } else if (a == "--events" && has_next) {
      if (!parse_u64(argv[++i], events)) return usage(), 2;
    } else if (a == "--profile" && has_next) {
      profile_name = argv[++i];
    } else if ((a == "-o" || a == "--out") && has_next) {
      out_path = argv[++i];
    } else if (a == "--live-target" && has_next) {
      if (!parse_u64(argv[++i], live_target)) return usage(), 2;
    } else if (a == "--validate") {
      do_validate = true;
    } else if (a == "-h" || a == "--help") {
      usage();
      return 0;
    } else {
      std::fprintf(stderr, "unknown argument: %s\n", a.c_str());
      usage();
      return 2;
    }
  }

  mebench::Profile profile{};
  if (!mebench::generator::parse_profile(profile_name, profile)) {
    std::fprintf(stderr, "unknown profile: %s\n", profile_name.c_str());
    usage();
    return 2;
  }
  if (out_path.empty() && !do_validate) {
    std::fprintf(stderr, "nothing to do: pass -o FILE, --validate, or both\n");
    return 2;
  }

  mebench::generator::Generator gen(seed, profile, static_cast<uint32_t>(live_target));
  const auto stream = gen.generate(events);

  if (!out_path.empty()) {
    std::string err;
    if (!mebench::generator::write_stream(out_path, seed, profile, gen.live_target(), stream, err)) {
      std::fprintf(stderr, "%s\n", err.c_str());
      return 1;
    }
    std::printf("wrote %s: %" PRIu64 " events, profile=%s, seed=%" PRIu64 "\n", out_path.c_str(),
                static_cast<uint64_t>(stream.size()), mebench::generator::profile_name(profile),
                seed);
  }

  if (!do_validate) return 0;

  if (events < mebench::generator::Generator::kMinEventsForInjections) {
    std::printf(
        "note: %" PRIu64 " events is below the %" PRIu64
        " needed to fit the adversarial injections; validating stream statistics only\n",
        events, mebench::generator::Generator::kMinEventsForInjections);
  }

  const auto rep =
      mebench::generator::validate_stream(stream, gen.injections(), profile, gen.live_target());

  std::printf("\nstream: %" PRIu64 " events (%" PRIu64 " new, %" PRIu64 " cancel), profile=%s\n",
              rep.event_count, rep.new_count, rep.cancel_count,
              mebench::generator::profile_name(profile));
  std::printf("output: %" PRIu64 " trades, %" PRIu64 " acks, %" PRIu64 " cancel-acks (%" PRIu64
              " via STP), %" PRIu64 " expired\n",
              rep.trade_count, rep.ack_count, rep.cancel_ack_count, rep.stp_cancel_count,
              rep.expired_count);
  std::printf("        %" PRIu64 " unknown-order rejects, %" PRIu64 " FOK rejects\n",
              rep.reject_unknown_count, rep.reject_fok_count);
  std::printf("fill:   %.1f%% of submitted quantity traded\n", rep.fill_rate * 100.0);
  std::printf("cancel: %.1f%% of cancels found their order (%" PRIu64 " hit, %" PRIu64 " missed)\n",
              rep.cancel_hit_rate * 100.0, rep.cancel_hit_count,
              rep.cancel_count - rep.cancel_hit_count);
  std::printf("depth:  %u resting orders across %u price levels (%.0f per level)\n\n",
              gen.final_depth(), gen.final_levels(),
              gen.final_levels() ? static_cast<double>(gen.final_depth()) / gen.final_levels() : 0.0);

  for (const auto& c : rep.checks) {
    const char* tag = c.skipped ? "SKIP" : (c.passed ? "PASS" : "FAIL");
    std::printf("%s  %-46s  %s\n", tag, c.name.c_str(), c.detail.c_str());
  }

  if (!rep.ok()) {
    std::printf("\nvalidation FAILED — do not trust this stream\n");
    return 1;
  }
  std::printf("\nvalidation passed\n");
  return 0;
}
