// mebench/engine.h — the interface you implement.
//
// FROZEN.

#pragma once

#include <cstdint>

#include "mebench/order.h"
#include "mebench/out.h"

namespace mebench {

struct LevelSnapshot {
  int32_t px;
  uint64_t total_qty;
  uint32_t order_count;
  uint64_t front_seq;  // seq of the order at the head of the queue
};

// front_seq is the only cheap way to catch a LIFO level queue — aggregates
// alone would look identical.
struct BookSnapshot {
  uint64_t at_seq;
  uint32_t n_bids, n_asks;
  LevelSnapshot bids[16];  // best first: highest price first
  LevelSnapshot asks[16];  // best first: lowest price first
  uint64_t resting_qty_total;
  uint32_t resting_order_count;
};

inline constexpr uint32_t kSnapshotLevels = 16;

class IMatchingEngine {
 public:
  // noexcept on the hot path lets the compiler skip unwind-table setup and
  // keeps exception machinery out of the timing. Do not throw: noexcept means a
  // throw terminates the process outright and the run fails.
  virtual void on_new(const Order& o, OutSink& out) noexcept = 0;
  virtual void on_cancel(OrderRef ref, uint64_t seq, OutSink& out) noexcept = 0;

  // Called outside the timed region — correctness snapshots only. It costs
  // nothing at benchmark time, so implement it straightforwardly: this is what
  // catches a silently dropped resting order.
  //
  // Fill at most kSnapshotLevels per side, BEST PRICE FIRST — highest price
  // first for bids, lowest first for asks. Set n_bids / n_asks to the number of
  // levels actually written, capped at kSnapshotLevels. resting_qty_total and
  // resting_order_count cover the WHOLE book, not just the levels written.
  virtual void snapshot(BookSnapshot& out) const = 0;

  virtual ~IMatchingEngine() = default;
};

}  // namespace mebench

// Participants implement this. The harness dlopen()s the compiled engine and
// looks up this symbol, so it must have C linkage.
extern "C" mebench::IMatchingEngine* create_engine();
