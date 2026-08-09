// generator/validate.h — the checks that must pass before the generator is
// trusted (plan section 5.8).
//
// Run the reference over a generated stream and assert the stream is actually
// exercising the engine: crossing enough to produce fills, bounded in depth,
// and containing every hand-placed adversarial case with its intended effect.
// A generator that quietly stopped crossing would still produce a leaderboard
// — of engines that were never tested.

#pragma once

#include <string>
#include <vector>

#include "generator/generator.h"
#include "mebench/wire.h"

namespace mebench::generator {

struct Check {
  std::string name;
  bool passed;
  std::string detail;
  // A check that could not be run meaningfully — not a pass, and not a failure
  // either. Reporting one as the other would be lying in a tool whose whole job
  // is to say whether a stream can be trusted.
  bool skipped = false;
};

struct ValidationReport {
  std::vector<Check> checks;

  uint64_t event_count = 0;
  uint64_t new_count = 0;
  uint64_t cancel_count = 0;
  uint64_t trade_count = 0;
  uint64_t ack_count = 0;
  uint64_t reject_unknown_count = 0;
  uint64_t reject_fok_count = 0;
  uint64_t expired_count = 0;
  uint64_t cancel_ack_count = 0;
  uint64_t stp_cancel_count = 0;

  uint64_t submitted_qty = 0;
  uint64_t traded_qty = 0;
  double fill_rate = 0.0;  // traded_qty / submitted_qty

  std::vector<uint32_t> depth_samples;  // resting order count at each decile

  // Cancels that named a resting order, versus cancels that hit nothing. The
  // second number is the one that made the benchmark grade the wrong thing when
  // it was 93%, so it is reported rather than left to be rediscovered.
  uint64_t cancel_hit_count = 0;
  double cancel_hit_rate = 0.0;

  bool ok() const {
    for (const auto& c : checks) {
      if (!c.passed && !c.skipped) return false;
    }
    return true;
  }
};

// How many events a profile needs before its book reaches steady state. Below
// this the book is still filling up, and "depth is growing" says nothing.
//
// `live_target` overrides the profile's own when non-zero, since the sweep
// drives depth through that knob and warm-up scales with it.
uint64_t warmup_events(Profile p, uint32_t live_target = 0);

ValidationReport validate_stream(const std::vector<WireEvent>& events,
                                 const std::vector<InjectionRecord>& injections, Profile profile,
                                 uint32_t live_target = 0);

}  // namespace mebench::generator
