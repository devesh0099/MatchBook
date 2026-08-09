#include "harness/bench.h"

#include <sys/mman.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <memory>
#include <random>
#include <sstream>

#include "common/decode.h"
#include "harness/hash_sink.h"
#include "harness/timing.h"

extern "C" {
#include "hdr/hdr_histogram.h"
}

#ifndef MEBENCH_BUILD_FINGERPRINT
#define MEBENCH_BUILD_FINGERPRINT "unknown"
#endif

namespace mebench::harness {
namespace {

// Latencies are recorded in raw TSC ticks and converted afterwards. Three
// significant figures over a 1..10,000,000 tick range is far finer than any
// difference this contest can resolve.
constexpr int64_t kHistLowest = 1;
constexpr int64_t kHistHighest = 10'000'000;
constexpr int kHistSigFigs = 3;

// An engine that does nothing, used to measure what the rdtscp pair and the
// virtual dispatch cost on THIS node. Whatever it reports is pure probe
// overhead: a constant added to every sample (SPEC section 5.1).
class EmptyEngine final : public IMatchingEngine {
 public:
  void on_new(const Order&, OutSink&) noexcept override {}
  void on_cancel(OrderRef, uint64_t, OutSink&) noexcept override {}
  void snapshot(BookSnapshot& out) const override { std::memset(&out, 0, sizeof(out)); }
};

class Histogram {
 public:
  Histogram() {
    // hdr_init returns 0 on success (verified against the library, not recalled).
    if (hdr_init(kHistLowest, kHistHighest, kHistSigFigs, &h_) != 0) h_ = nullptr;
  }
  ~Histogram() {
    if (h_) hdr_close(h_);
  }
  Histogram(const Histogram&) = delete;
  Histogram& operator=(const Histogram&) = delete;

  bool valid() const { return h_ != nullptr; }
  hdr_histogram* raw() { return h_; }

  // Percentiles are on a 0..100 scale, not 0..1 (verified empirically).
  double at(double pct) const { return static_cast<double>(hdr_value_at_percentile(h_, pct)); }
  double mean() const { return hdr_mean(h_); }
  double max() const { return static_cast<double>(hdr_max(h_)); }
  int64_t count() const { return h_->total_count; }

 private:
  hdr_histogram* h_ = nullptr;
};

// The decoded stream is ~400MB at 10M events. Backing it with huge pages keeps
// TLB misses out of the timed region; falling back is fine, but say so rather
// than silently measuring something else.
class EventBuffer {
 public:
  explicit EventBuffer(size_t count) : count_(count), bytes_(count * sizeof(DecodedEvent)) {
    void* p = mmap(nullptr, bytes_, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);
    if (p != MAP_FAILED) {
      huge_ = true;
    } else {
      p = mmap(nullptr, bytes_, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
      if (p != MAP_FAILED) {
#ifdef MADV_HUGEPAGE
        madvise(p, bytes_, MADV_HUGEPAGE);  // transparent huge pages as second best
#endif
      }
    }
    data_ = (p == MAP_FAILED) ? nullptr : static_cast<DecodedEvent*>(p);
  }
  ~EventBuffer() {
    if (data_) munmap(data_, bytes_);
  }
  EventBuffer(const EventBuffer&) = delete;
  EventBuffer& operator=(const EventBuffer&) = delete;

  DecodedEvent* data() const { return data_; }
  size_t size() const { return count_; }
  bool explicit_huge_pages() const { return huge_; }

 private:
  DecodedEvent* data_ = nullptr;
  size_t count_;
  size_t bytes_;
  bool huge_ = false;
};

// Bootstrap a confidence interval across the per-run medians (plan section 8).
// This quantifies sampling noise across runs; it says nothing about systematic
// bias — a node that is 8% slow yields a tight interval around a wrong number.
void bootstrap_ci(const std::vector<double>& run_p50s, double& lo, double& hi) {
  if (run_p50s.empty()) return;
  if (run_p50s.size() == 1) {
    lo = hi = run_p50s[0];
    return;
  }
  constexpr int kResamples = 2000;
  std::mt19937_64 rng(0x5EED5EEDull);  // fixed: the reported interval is reproducible
  std::vector<double> medians;
  medians.reserve(kResamples);
  std::vector<double> sample(run_p50s.size());

  for (int b = 0; b < kResamples; ++b) {
    for (size_t i = 0; i < run_p50s.size(); ++i) {
      const uint32_t pick =
          static_cast<uint32_t>(((rng() >> 32) * run_p50s.size()) >> 32);
      sample[i] = run_p50s[pick];
    }
    std::sort(sample.begin(), sample.end());
    medians.push_back(sample[sample.size() / 2]);
  }
  std::sort(medians.begin(), medians.end());
  lo = medians[static_cast<size_t>(kResamples * 0.025)];
  hi = medians[static_cast<size_t>(kResamples * 0.975)];
}

double median_of(std::vector<double> v) {
  if (v.empty()) return 0.0;
  std::sort(v.begin(), v.end());
  return v[v.size() / 2];
}

}  // namespace

const char* bench_outcome_name(BenchOutcome o) {
  switch (o) {
    case BenchOutcome::Ok: return "ok";
    case BenchOutcome::DigestMismatch: return "digest_mismatch";
    case BenchOutcome::NodeUnhealthy: return "node_unhealthy";
  }
  return "?";
}

double measure_probe_cost_ns(const std::vector<WireEvent>& events, uint64_t sample_events,
                            double ticks_per_ns) {
  const uint64_t n = std::min<uint64_t>(sample_events, events.size());
  if (n == 0 || ticks_per_ns <= 0.0) return 0.0;

  EmptyEngine empty;
  HashSink sink;

  std::vector<DecodedEvent> decoded;
  decoded.reserve(n);
  for (uint64_t i = 0; i < n; ++i) decoded.push_back(decode(events[i], i));

  volatile uint64_t touch = 0;
  for (const auto& d : decoded) touch += d.o.px;
  (void)touch;

  // Measured under the SAME discipline as a ranked run: warm up first, then
  // take the median of several passes.
  //
  // Measuring it once, cold, reported a probe cost LARGER than the p50 of the
  // engine it was supposed to be a component of — which is impossible, since
  // the probe is inside every sample. The first ranked run shows the same cold
  // inflation (110ns against a 40ns steady state); the median across runs
  // absorbs it there, and this now does the equivalent here.
  constexpr int kProbePasses = 5;
  std::vector<double> passes;
  passes.reserve(kProbePasses);

  for (int pass = 0; pass < kProbePasses + 1; ++pass) {
    Histogram hist;
    if (!hist.valid()) return 0.0;
    for (uint64_t i = 0; i < n; ++i) {
      const uint64_t t0 = rdtscp();
      dispatch(empty, decoded[i], sink);
      const uint64_t t1 = rdtscp();
      hdr_record_value(hist.raw(), static_cast<int64_t>(t1 - t0));
    }
    if (pass == 0) continue;  // discard the cold pass
    passes.push_back(hist.at(50.0) / ticks_per_ns);
  }

  std::sort(passes.begin(), passes.end());
  return passes[passes.size() / 2];
}

BenchResult bench(const std::vector<WireEvent>& events, const EngineSource& engine,
                  const BenchOptions& opts) {
  BenchResult r;
  r.events_total = events.size();
  r.build_fingerprint = MEBENCH_BUILD_FINGERPRINT;
  r.expected_digest = opts.expected_digest;
  r.digest_checked = opts.have_expected_digest;

  if (opts.lock_memory) {
    // Keep page faults out of the timed region entirely.
    r.memory_locked = mlockall(MCL_CURRENT | MCL_FUTURE) == 0;
  }

  r.tsc_ticks_per_ns = calibrate_tsc_ticks_per_ns();

  // ---- load phase, untimed (plan section 7)
  EventBuffer buf(events.size());
  if (!buf.data()) {
    r.notes = "could not allocate the decoded event buffer";
    r.outcome = BenchOutcome::NodeUnhealthy;
    return r;
  }
  for (uint64_t i = 0; i < events.size(); ++i) buf.data()[i] = decode(events[i], i);

  // Touch every page so no fault lands in the timed region.
  volatile uint64_t touch = 0;
  for (size_t i = 0; i < buf.size(); ++i) touch += buf.data()[i].o.px;
  (void)touch;

  if (!buf.explicit_huge_pages()) {
    r.notes = "explicit huge pages unavailable; fell back to transparent huge pages";
  }

  const uint64_t warmup = std::min<uint64_t>(opts.warmup, events.size());
  r.events_timed = events.size() - warmup;

  r.probe_cost_ns = measure_probe_cost_ns(events, std::min<uint64_t>(events.size(), 2'000'000),
                                          r.tsc_ticks_per_ns);

  uint32_t consecutive_discards = 0;
  uint32_t completed = 0;

  while (completed < opts.runs) {
    uint64_t steal_before = 0, steal_after = 0;
    const bool have_steal = read_steal_time(steal_before);

    std::unique_ptr<IMatchingEngine> e(engine.create());
    HashSink sink;
    Histogram hist;
    if (!hist.valid()) {
      r.notes = "histogram allocation failed";
      r.outcome = BenchOutcome::NodeUnhealthy;
      return r;
    }

    // Warm the branch predictor, caches and the book itself. The digest covers
    // the WHOLE stream, warmup included, so it stays comparable with the
    // correctness run's digest over the same seed.
    for (uint64_t i = 0; i < warmup; ++i) dispatch(*e, buf.data()[i], sink);

    const auto wall0 = std::chrono::steady_clock::now();
    for (uint64_t i = warmup; i < buf.size(); ++i) {
      const uint64_t t0 = rdtscp();
      dispatch(*e, buf.data()[i], sink);
      const uint64_t t1 = rdtscp();
      hdr_record_value(hist.raw(), static_cast<int64_t>(t1 - t0));
    }
    const auto wall1 = std::chrono::steady_clock::now();

    if (have_steal) read_steal_time(steal_after);

    RunResult run;
    run.digest = sink.digest();
    run.steal_delta = have_steal ? (steal_after - steal_before) : 0;
    const double per_ns = r.tsc_ticks_per_ns > 0 ? r.tsc_ticks_per_ns : 1.0;
    run.p50_ns = hist.at(50.0) / per_ns;
    run.p95_ns = hist.at(95.0) / per_ns;
    run.p99_ns = hist.at(99.0) / per_ns;
    run.p999_ns = hist.at(99.9) / per_ns;
    run.mean_ns = hist.mean() / per_ns;
    run.max_ns = hist.max() / per_ns;
    run.wall_s = std::chrono::duration<double>(wall1 - wall0).count();

    // The one contamination signal a submission cannot cause itself. Non-zero
    // means a co-tenant took CPU from this run; discard it and try again.
    if (run.steal_delta != 0) {
      run.discarded = true;
      r.runs.push_back(run);
      ++r.discard_count;
      if (++consecutive_discards >= opts.max_discards) {
        r.outcome = BenchOutcome::NodeUnhealthy;
        std::ostringstream s;
        s << consecutive_discards << " consecutive runs discarded on steal time; a human should "
                                     "look before the queue churns silently";
        r.notes = s.str();
        return r;
      }
      continue;
    }

    static const double kPcts[] = {50.0, 75.0, 90.0, 95.0, 99.0, 99.9, 99.99, 100.0};
    for (double p : kPcts) run.percentiles.push_back({p, hist.at(p) / per_ns});

    consecutive_discards = 0;
    r.runs.push_back(run);
    r.run_p50s_ns.push_back(run.p50_ns);
    r.digest = run.digest;
    ++completed;
  }

  // Median of the per-run p50s. Never an average of per-event latencies across
  // runs — that would destroy the tail.
  r.p50_ns = median_of(r.run_p50s_ns);
  std::vector<double> p95s, p99s, p999s;
  for (const auto& run : r.runs) {
    if (run.discarded) continue;
    p95s.push_back(run.p95_ns);
    p99s.push_back(run.p99_ns);
    p999s.push_back(run.p999_ns);
  }
  r.p95_ns = median_of(p95s);
  r.p99_ns = median_of(p99s);
  r.p999_ns = median_of(p999s);
  bootstrap_ci(r.run_p50s_ns, r.ci_low_ns, r.ci_high_ns);

  // Publish the distribution of the run that DECIDED the score. Reporting the
  // last run's curve alongside the median run's headline number would be two
  // different runs wearing one label.
  {
    double best = 1e300;
    for (uint32_t i = 0; i < r.runs.size(); ++i) {
      if (r.runs[i].discarded) continue;
      const double d = std::abs(r.runs[i].p50_ns - r.p50_ns);
      if (d < best) {
        best = d;
        r.median_run_index = i;
        r.percentiles = r.runs[i].percentiles;
      }
    }
  }

  // Verification inside the timed run is what stops anyone winning by doing
  // less work. A mismatch here is its own outcome: passed correctness, diverged
  // on the benchmark stream.
  if (opts.have_expected_digest && r.digest != opts.expected_digest) {
    r.outcome = BenchOutcome::DigestMismatch;
  }
  for (const auto& run : r.runs) {
    if (!run.discarded && run.digest != r.digest) {
      r.outcome = BenchOutcome::DigestMismatch;
      r.notes = "runs of the same stream produced different digests; the engine is not "
                "deterministic";
    }
  }
  return r;
}

std::string format_bench_report(const BenchResult& r) {
  std::ostringstream s;
  s.setf(std::ios::fixed);
  s.precision(1);

  if (r.outcome == BenchOutcome::NodeUnhealthy) {
    s << "NODE UNHEALTHY\n  " << r.notes << "\n";
    return s.str();
  }
  if (r.outcome == BenchOutcome::DigestMismatch) {
    s << "BENCH VERIFICATION FAILED\n\n"
      << "  This submission passed the correctness lane but diverged on the benchmark\n"
      << "  stream. That is a different failure from a wrong answer during verification.\n";
    if (r.digest_checked) {
      s << "  expected digest " << std::hex << r.expected_digest << ", got " << r.digest << std::dec
        << "\n";
    }
    if (!r.notes.empty()) s << "  " << r.notes << "\n";
    return s.str();
  }

  const double net = r.p50_ns - r.probe_cost_ns;
  s << "p50   " << r.p50_ns << " ns   <- ranked\n"
    << "p99   " << r.p99_ns << " ns      (reported, unranked)\n"
    << "p99.9 " << r.p999_ns << " ns      (reported, unranked)\n\n"
    << "probe cost " << r.probe_cost_ns
    << " ns/event, measured on this node with an empty engine.\n"
    << "It is a constant added to every sample: ordering is preserved, gaps are\n"
    << "compressed. Engine work net of the probe is about " << (net > 0 ? net : 0.0)
    << " ns (informational, not ranked).\n";

  // If the instrument costs more than the thing being measured, say so loudly
  // rather than letting a compressed leaderboard look like a close contest.
  if (r.p50_ns > 0 && r.probe_cost_ns / r.p50_ns > 0.25) {
    s.precision(0);
    s << "\nWARNING: the probe is " << (100.0 * r.probe_cost_ns / r.p50_ns)
      << "% of the ranked number. A 2x engine improvement moves p50 by far less\n"
      << "than 2x, so ranked gaps are much smaller than the underlying differences.\n";
    s.precision(1);
  }
  s << "\n"
    << "bootstrap 95% CI on the score: " << r.ci_low_ns << " .. " << r.ci_high_ns << " ns\n"
    << "(overlapping intervals mean tied rank)\n\n"
    << "per-run p50s (ns):";
  for (double p : r.run_p50s_ns) s << " " << p;
  s << "\n";
  if (r.discard_count) s << r.discard_count << " run(s) discarded on steal time\n";
  s << "\n" << r.events_timed << " events timed per run, " << r.run_p50s_ns.size() << " runs, digest "
    << std::hex << r.digest << std::dec << "\n";
  s << "memory locked: " << (r.memory_locked ? "yes" : "no")
    << ", tsc " << r.tsc_ticks_per_ns << " ticks/ns\n";
  if (!r.notes.empty()) s << "note: " << r.notes << "\n";
  return s.str();
}

std::string format_bench_json(const BenchResult& r) {
  std::ostringstream s;
  s << "{\"outcome\":\"" << bench_outcome_name(r.outcome) << "\",\"p50_ns\":" << r.p50_ns
    << ",\"p95_ns\":" << r.p95_ns << ",\"p99_ns\":" << r.p99_ns << ",\"p999_ns\":" << r.p999_ns
    << ",\"probe_cost_ns\":" << r.probe_cost_ns
    << ",\"p50_net_of_probe_ns\":" << (r.p50_ns > r.probe_cost_ns ? r.p50_ns - r.probe_cost_ns : 0.0) << ",\"ci_low_ns\":" << r.ci_low_ns
    << ",\"ci_high_ns\":" << r.ci_high_ns << ",\"discard_count\":" << r.discard_count
    << ",\"events_timed\":" << r.events_timed << ",\"events_total\":" << r.events_total
    << ",\"tsc_ticks_per_ns\":" << r.tsc_ticks_per_ns
    << ",\"memory_locked\":" << (r.memory_locked ? 1 : 0) << ",\"digest\":\"" << std::hex
    << r.digest << std::dec << "\",\"build_fingerprint\":\"" << r.build_fingerprint << "\"";

  s << ",\"run_p50s_ns\":[";
  for (size_t i = 0; i < r.run_p50s_ns.size(); ++i) s << (i ? "," : "") << r.run_p50s_ns[i];
  s << "],\"runs\":[";
  for (size_t i = 0; i < r.runs.size(); ++i) {
    const auto& run = r.runs[i];
    s << (i ? "," : "") << "{\"p50_ns\":" << run.p50_ns << ",\"p99_ns\":" << run.p99_ns
      << ",\"p95_ns\":" << run.p95_ns << ",\"p999_ns\":" << run.p999_ns << ",\"mean_ns\":" << run.mean_ns
      << ",\"max_ns\":" << run.max_ns << ",\"wall_s\":" << run.wall_s
      << ",\"steal_delta\":" << run.steal_delta
      << ",\"discarded\":" << (run.discarded ? 1 : 0) << "}";
  }
  s << "],\"median_run_index\":" << r.median_run_index << ",\"percentiles\":[";
  for (size_t i = 0; i < r.percentiles.size(); ++i) {
    s << (i ? "," : "") << "{\"p\":" << r.percentiles[i].first
      << ",\"ns\":" << r.percentiles[i].second << "}";
  }
  s << "]";
  if (!r.notes.empty()) s << ",\"notes\":\"" << r.notes << "\"";
  s << "}";
  return s.str();
}

}  // namespace mebench::harness
