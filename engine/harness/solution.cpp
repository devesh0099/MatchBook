#include "harness/solution.h"

#include <cstdio>
#include <cstring>

namespace mebench::harness {

namespace {

// "MBSOL" + version byte, little-endian in a u64 so the whole header is
// fixed-width integers and fread/fwrite of the struct below is portable
// across the machines this contest actually uses (x86-64 Linux everywhere).
constexpr uint64_t kMagic = 0x314c4f53424dull;  // "MBSOL1"

struct FileHeader {
  uint64_t magic;
  uint64_t seed;
  uint64_t profile_id;
  uint64_t events;
  uint64_t checkpoint_interval;
  uint64_t checkpoint_count;
  uint64_t final_digest;
};

}  // namespace

bool Solution::checkpoint_at(uint64_t processed, uint64_t& index_out,
                             uint64_t& digest_out) const {
  if (checkpoint_interval == 0 || checkpoints.empty()) return false;
  const uint64_t k = processed / checkpoint_interval;  // entries fully covered
  if (k == 0) return false;
  const uint64_t entry = std::min<uint64_t>(k, checkpoints.size()) - 1;
  index_out = (entry + 1) * checkpoint_interval;
  digest_out = checkpoints[entry];
  return true;
}

bool write_solution(const std::string& path, const Solution& s, std::string& err) {
  std::FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) {
    err = "could not open " + path + " for writing";
    return false;
  }
  FileHeader h{kMagic,  s.seed, s.profile_id, s.events, s.checkpoint_interval,
               s.checkpoints.size(), s.final_digest};
  bool ok = std::fwrite(&h, sizeof h, 1, f) == 1;
  if (ok && !s.checkpoints.empty()) {
    ok = std::fwrite(s.checkpoints.data(), sizeof(uint64_t), s.checkpoints.size(), f) ==
         s.checkpoints.size();
  }
  ok = std::fclose(f) == 0 && ok;
  if (!ok) err = "short write to " + path;
  return ok;
}

bool read_solution(const std::string& path, Solution& s, std::string& err) {
  std::FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) {
    err = "could not open " + path;
    return false;
  }
  FileHeader h{};
  if (std::fread(&h, sizeof h, 1, f) != 1 || h.magic != kMagic) {
    err = path + " is not a solution file";
    std::fclose(f);
    return false;
  }
  s.seed = h.seed;
  s.profile_id = static_cast<uint32_t>(h.profile_id);
  s.events = h.events;
  s.checkpoint_interval = h.checkpoint_interval;
  s.final_digest = h.final_digest;
  s.checkpoints.resize(h.checkpoint_count);
  if (h.checkpoint_count &&
      std::fread(s.checkpoints.data(), sizeof(uint64_t), h.checkpoint_count, f) !=
          h.checkpoint_count) {
    err = path + " is truncated";
    std::fclose(f);
    return false;
  }
  std::fclose(f);
  return true;
}

}  // namespace mebench::harness
