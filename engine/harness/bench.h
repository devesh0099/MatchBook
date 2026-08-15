// harness/bench.h — the ranked measurement (SPEC section 5).
//
// score = median of per-run p50s, across 7-10 runs.
//
// p50 is ranked; p99 and p99.9 are reported unranked. Sporadic hypervisor or
// thermal interference contaminates tails but barely moves a median over 10M
// events. Per-event latencies are never averaged across runs — that would
// destroy the tail.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "harness/engine_loader.h"
#include "mebench/wire.h"

namespace mebench::harness {

enum class BenchOutcome {
  Ok,
  DigestMismatch,   // passed correctness, diverged on the benchmark stream
  NodeUnhealthy,    // repeated steal-time discards; a human should look
  DeadlineMissed,   // the gate: too slow to finish the stream in its budget
};

const char* bench_outcome_name(BenchOutcome o);

/// One row of the HdrHistogram percentile distribution — the CLASSIC columns
/// that hdr_percentiles_print emits and that Gil Tene's plotter consumes.
struct HdrPoint {
  double percentile;         // 0..100
  double value_ns;           // highest_equivalent_value, converted
  uint64_t cumulative_count; // samples at or below this value
};

/// One window of a run: the percentiles of the events that fell inside it.
/// This is what makes a within-run time series possible — how latency moved as
/// the run progressed, rather than one number for the whole thing.
struct WindowPoint {
  double elapsed_frac;  // 0..1 through the timed region
  uint64_t first_event;
  double p50_ns, p95_ns, p99_ns;
};

struct RunResult {
  double p50_ns = 0.0;
  double p95_ns = 0.0;
  double p99_ns = 0.0;
  double p999_ns = 0.0;
  double mean_ns = 0.0;
  double max_ns = 0.0;
  double wall_s = 0.0;  // so the real node measures job cost instead of us estimating it
  uint64_t steal_delta = 0;
  bool discarded = false;
  // Deadline-cut runs keep everything measured up to the cut: the percentile
  // chain judges non-finishers too (PLAN-measurement-redesign §3), so the
  // histogram to the cut is a result, not debris.
  bool deadline_missed = false;
  uint64_t events_processed = 0;  // input events dispatched before the cut (= all when it finished)
  uint64_t checked_events = 0;    // events validated against the solution table (0 = unchecked)
  uint64_t digest = 0;
  /// This run's own percentile distribution. Kept per run so the published
  /// curve can come from the run that actually decided the score.
  std::vector<HdrPoint> percentiles;
  std::vector<WindowPoint> timeline;
};

struct BenchResult {
  BenchOutcome outcome = BenchOutcome::Ok;

  // The ranked number.
  double p50_ns = 0.0;
  double p95_ns = 0.0;
  double p99_ns = 0.0;
  double p999_ns = 0.0;

  // Published alongside the score so the numbers can be read honestly: a
  // constant added to every sample preserves ordering but compresses gaps.
  double probe_cost_ns = 0.0;

  std::vector<double> run_p50s_ns;  // every kept run, for the bootstrap CI
  double ci_low_ns = 0.0;
  double ci_high_ns = 0.0;

  std::vector<RunResult> runs;  // including discarded ones, for the audit trail
  uint32_t discard_count = 0;

  uint64_t digest = 0;
  uint64_t expected_digest = 0;
  bool digest_checked = false;

  uint64_t events_total = 0;
  uint64_t events_timed = 0;
  double deadline_s = 0.0;        // echoed from the options; 0 = no gate
  uint64_t events_processed = 0;  // from the run that decided a missed gate
  double tsc_ticks_per_ns = 0.0;
  bool memory_locked = false;
  std::string build_fingerprint;
  std::string notes;

  // Percentile table for the published curve, taken from the MEDIAN run — the
  // one whose p50 is the score. The last run's table would describe a run that
  // decided nothing.
  std::vector<HdrPoint> percentiles;
  /// Windowed percentiles from the same median run the curve comes from.
  std::vector<WindowPoint> timeline;
  uint32_t median_run_index = 0;
};

struct BenchOptions {
  uint32_t runs = 9;
  uint64_t warmup = 200000;
  uint64_t expected_digest = 0;
  bool have_expected_digest = false;
  uint32_t max_discards = 3;  // three in a row means the node, not the submission
  bool lock_memory = true;

  // The gate (PLAN-measurement-redesign §3): a per-run wall budget over the
  // WHOLE run, warm-up included. 0 disables. A miss stops the job — the
  // remaining runs would only re-measure a failed gate — but the cut run's
  // percentiles and events_processed are kept and reported.
  double deadline_s = 0.0;

  // The baked answer key (solution.h). When set, every run's cumulative
  // digest is recorded at the table's checkpoints and compared: a completed
  // run must match the final digest, a cut run must match every checkpoint it
  // passed. Replaces --digest for streams that have a baked solution.
  const struct Solution* solution = nullptr;
};

BenchResult bench(const std::vector<WireEvent>& events, const EngineSource& engine,
                  const BenchOptions& opts);

// The per-event cost of the rdtscp pair itself, measured on this exact node
// with an engine that does nothing (plan section 13).
double measure_probe_cost_ns(const std::vector<WireEvent>& events, uint64_t sample_events,
                             double ticks_per_ns);

std::string format_bench_report(const BenchResult& r);
std::string format_bench_json(const BenchResult& r);

}  // namespace mebench::harness
