// harness/verify.h — the correctness gate (SPEC section 4, layers 2 and 3).
//
// Streams the submission and the oracle side by side over the same seeded
// stream, stops at the FIRST divergence, and binary-searches the stream prefix
// down to a reproducer small enough to read.
//
// Only the first failure is reported, deliberately: showing all 400 encourages
// fixing by pattern-matching against output instead of thinking.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "harness/engine_loader.h"
#include "mebench/out.h"
#include "mebench/wire.h"

namespace mebench::harness {

enum class VerifyOutcome {
  Passed,
  OutputDiverged,     // submission disagreed with the oracle
  SnapshotDiverged,   // books disagreed even though output matched so far
  InvariantViolated,  // submission's output is not self-consistent
  Timeout,            // liveness limit hit; reported distinctly from a wrong answer
  /// The REFERENCE broke one of its own laws. A platform bug, never the
  /// participant's — reported separately so it can never be scored as theirs.
  OracleViolatedInvariant,
};

const char* outcome_name(VerifyOutcome o);

// The rule areas a participant is shown progress against. Correctness is binary
// for eligibility, but "price-time OK, IOC not yet" is what makes a red X
// actionable.
enum class Category : uint8_t {
  PriceTimePriority,
  PartialFills,
  Cancels,
  MarketOrders,
  Ioc,
  Fok,
  SelfTradePrevention,
  Count,
};

struct CategoryProgress {
  const char* name;
  uint64_t exercised;   // events of this kind seen before the run stopped
  bool failed;          // the divergence happened on an event of this kind
};

struct VerifyResult {
  VerifyOutcome outcome = VerifyOutcome::Passed;
  uint64_t events_processed = 0;
  uint64_t total_events = 0;
  double elapsed_s = 0.0;

  // Field-wise digest of the submission's output over the whole stream. The
  // ranked run must reproduce this exactly, which is what makes verification
  // happen INSIDE the timed loop (SPEC 5.2).
  uint64_t digest = 0;

  // Populated unless the run passed or timed out.
  uint64_t in_seq = 0;
  uint64_t output_index = 0;  // which output number, across the whole run
  bool have_expected = false;
  bool have_actual = false;
  OutEvent expected{};
  OutEvent actual{};
  std::string message;

  // Shrunk counterexample: replaying exactly these events, numbered from zero,
  // reproduces the failure.
  std::vector<WireEvent> reproducer;
  uint64_t reproducer_begin = 0;  // index in the original stream
  uint64_t reproducer_end = 0;

  std::vector<CategoryProgress> progress;

  bool passed() const { return outcome == VerifyOutcome::Passed; }
};

struct VerifyOptions {
  uint64_t snapshot_every = 10000;  // 0 disables snapshot diffing
  double wall_time_s = 45.0;        // liveness only; 0 disables
  bool shrink = true;
};

VerifyResult verify(const std::vector<WireEvent>& events, const EngineSource& submission,
                    const EngineSource& oracle, const VerifyOptions& opts);

// Human-readable first-divergence report, in the shape SPEC section 4 shows.
std::string format_report(const VerifyResult& r);
std::string format_json(const VerifyResult& r);

}  // namespace mebench::harness
