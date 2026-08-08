// harness/timing.h — the measurement primitives.
//
// Everything here is deliberate and documented in SPEC section 5.1, because the
// measurement model IS the contest: change any of it and you are ranking a
// different quantity.

#pragma once

#include <time.h>

#include <cstdint>

namespace mebench::harness {

// rdtscp, not rdtsc: it waits for prior instructions to retire, so event N's
// tail cannot overlap event N+1's head. That makes this a measurement of
// ISOLATED per-event latency rather than overlapped stream throughput — an
// accepted property of the design, not an oversight (SPEC 5.1).
inline uint64_t rdtscp() {
  uint32_t lo, hi, aux;
  __asm__ __volatile__("rdtscp" : "=a"(lo), "=d"(hi), "=c"(aux));
  return (static_cast<uint64_t>(hi) << 32) | lo;
}

// The TSC is invariant on every CPU this runs on, so one calibration against a
// real clock converts ticks to nanoseconds for the whole run.
inline double calibrate_tsc_ticks_per_ns(uint32_t settle_ms = 50) {
  timespec t0{}, t1{};
  clock_gettime(CLOCK_MONOTONIC, &t0);
  const uint64_t c0 = rdtscp();

  const timespec sleep_for{0, static_cast<long>(settle_ms) * 1000L * 1000L};
  timespec remaining = sleep_for;
  while (nanosleep(&remaining, &remaining) == -1) {
  }

  const uint64_t c1 = rdtscp();
  clock_gettime(CLOCK_MONOTONIC, &t1);

  const double elapsed_ns = static_cast<double>(t1.tv_sec - t0.tv_sec) * 1e9 +
                            static_cast<double>(t1.tv_nsec - t0.tv_nsec);
  if (elapsed_ns <= 0.0) return 1.0;
  return static_cast<double>(c1 - c0) / elapsed_ns;
}

// Steal time from /proc/stat, in USER_HZ ticks.
//
// The one contamination signal a submission cannot cause itself, which is
// exactly why it is the only per-run machine check (plan section 8). On
// dedicated tenancy it should always read zero, making it a free tripwire.
// Returns false if /proc/stat could not be read.
bool read_steal_time(uint64_t& out);

}  // namespace mebench::harness
