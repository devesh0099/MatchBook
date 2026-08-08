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

  bool ok() const {
    for (const auto& c : checks) {
      if (!c.passed) return false;
    }
    return true;
  }
};

ValidationReport validate_stream(const std::vector<WireEvent>& events,
                                 const std::vector<InjectionRecord>& injections);

}  // namespace mebench::generator
