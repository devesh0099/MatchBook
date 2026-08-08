#include "harness/timing.h"

#include <cstdio>
#include <cstring>

namespace mebench::harness {

bool read_steal_time(uint64_t& out) {
  std::FILE* f = std::fopen("/proc/stat", "rb");
  if (!f) return false;

  char line[512];
  bool ok = false;
  if (std::fgets(line, sizeof(line), f)) {
    // "cpu  user nice system idle iowait irq softirq steal guest guest_nice"
    // Aggregate line first; steal is the 8th value.
    unsigned long long v[10] = {};
    const int n = std::sscanf(line, "cpu %llu %llu %llu %llu %llu %llu %llu %llu", &v[0], &v[1],
                              &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]);
    if (n >= 8) {
      out = v[7];
      ok = true;
    }
  }
  std::fclose(f);
  return ok;
}

}  // namespace mebench::harness
