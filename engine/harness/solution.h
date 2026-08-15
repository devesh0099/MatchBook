// harness/solution.h — the baked answer key for a fixed stream.
//
// With the contest seeds fixed, the reference runs ONCE, at bake time, and
// what it produced is cached: the output digest at regular checkpoints through
// the stream, plus the final digest. At judge time correctness is a lookup,
// not a reference run — and because the table has an entry every
// `checkpoint_interval` events, the check is valid at ANY cut point: a
// completed run, a deadline-cut ladder level, and a rejudge non-finisher's
// progress prefix are all validated by the same mechanism. A fast-wrong
// engine cannot buy a percentile number with garbage output
// (PLAN-measurement-redesign §4).
//
// The digest is the existing field-wise FNV fold (hash_sink.h) over the
// engine's cumulative output after the first N input events, warm-up included.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace mebench::harness {

struct Solution {
  uint64_t seed = 0;
  uint32_t profile_id = 0;
  uint64_t events = 0;               // input events the table describes
  uint64_t checkpoint_interval = 0;  // input events between table entries
  // digest after (k+1)*checkpoint_interval input events, k = 0..n-1.
  std::vector<uint64_t> checkpoints;
  uint64_t final_digest = 0;  // digest after all `events` inputs

  // The last table entry at or below `processed` input events.
  // Returns false when `processed` is short of even the first checkpoint —
  // nothing can be validated in that case, which callers must treat as
  // "unchecked", never as "passed".
  bool checkpoint_at(uint64_t processed, uint64_t& index_out, uint64_t& digest_out) const;
};

bool write_solution(const std::string& path, const Solution& s, std::string& err);
bool read_solution(const std::string& path, Solution& s, std::string& err);

}  // namespace mebench::harness
