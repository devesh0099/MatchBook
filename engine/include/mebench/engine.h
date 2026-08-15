// mebench/engine.h — the interface you implement.
//
// FROZEN.

#pragma once

#include <cstdint>
#include <utility>
#include <vector>

#include "mebench/order.h"
#include "mebench/out.h"

namespace mebench {

// One side of the book as (price, total_qty) aggregates, one entry per occupied
// price level, BEST PRICE FIRST: bids highest price first, asks lowest first.
// total_qty is the sum of every resting order's remaining quantity at that
// level. No empty levels, no duplicate prices.
using LevelVec = std::vector<std::pair<int32_t, uint64_t>>;

// The book as the harness sees it. Never timed.
struct BookSnapshot {
  uint64_t at_seq;          // seq of the last event you saw
  uint32_t n_bids, n_asks;  // must equal bids.size() / asks.size() once filled
  LevelVec bids;            // filled via bid_levels(), highest price first
  LevelVec asks;            // filled via ask_levels(), lowest price first
  uint64_t resting_qty_total;    // whole book
  uint32_t resting_order_count;  // whole book
};

class IMatchingEngine {
 public:
  // noexcept on the hot path lets the compiler skip unwind-table setup and
  // keeps exception machinery out of the timing. Do not throw: noexcept means a
  // throw terminates the process outright and the run fails.
  virtual void on_new(const Order& o, OutSink& out) noexcept = 0;
  virtual void on_cancel(OrderRef ref, uint64_t seq, OutSink& out) noexcept = 0;

  // Never timed. Fills the SCALAR fields only — at_seq, n_bids/n_asks and the
  // whole-book totals. The level vectors are filled separately by
  // bid_levels()/ask_levels(); the harness composes all three via build_book().
  virtual void snapshot(BookSnapshot& out) const = 0;

  // Never timed. Clear `out`, then append one (price, total_qty) pair per
  // occupied level, best price first — bids highest first, asks lowest first.
  // Write them the simple way, whatever your book looks like internally: an
  // O(n log n) collect-and-sort is fine.
  virtual void bid_levels(LevelVec& out) const = 0;
  virtual void ask_levels(LevelVec& out) const = 0;

  virtual ~IMatchingEngine() = default;
};

// How every consumer builds a complete book view. Not part of what you
// implement — it just calls the three virtuals above in order.
inline void build_book(const IMatchingEngine& e, BookSnapshot& snap) {
  e.snapshot(snap);
  e.bid_levels(snap.bids);
  e.ask_levels(snap.asks);
}

}  // namespace mebench

// Participants implement this. The harness dlopen()s the compiled engine and
// looks up this symbol, so it must have C linkage.
extern "C" mebench::IMatchingEngine* create_engine();
