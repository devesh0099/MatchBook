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
};

const char* bench_outcome_name(BenchOutcome o);

struct RunResult {
  double p50_ns = 0.0;
  double p99_ns = 0.0;
  double p999_ns = 0.0;
  double mean_ns = 0.0;
  double max_ns = 0.0;
  double wall_s = 0.0;  // so the real node measures job cost instead of us estimating it
  uint64_t steal_delta = 0;
  bool discarded = false;
  uint64_t digest = 0;
};

struct BenchResult {
  BenchOutcome outcome = BenchOutcome::Ok;

  // The ranked number.
  double p50_ns = 0.0;
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
  double tsc_ticks_per_ns = 0.0;
  bool memory_locked = false;
  std::string build_fingerprint;
  std::string notes;

  // Percentile table for the histogram artifact.
  std::vector<std::pair<double, double>> percentiles;  // (percentile, ns)
};

struct BenchOptions {
  uint32_t runs = 9;
  uint64_t warmup = 200000;
  uint64_t expected_digest = 0;
  bool have_expected_digest = false;
  uint32_t max_discards = 3;  // three in a row means the node, not the submission
  bool lock_memory = true;
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
