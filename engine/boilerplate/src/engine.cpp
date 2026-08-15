// engine.cpp — your submission. The only file you write, and the only file the
// server compiles: one translation unit against the read-only headers in
// include/mebench/.
//
// Everything below YOUR CODE STARTS HERE is yours to restructure completely.
// The data structures above it are a starting point, not a required design.
//
// Each TODO names the rule it implements — [A1], [F1], [S2] and so on. Those
// ids are in the spec, one click away in the editor. The headers themselves
// document every struct and field.
//
//   Run     the visible tests in tests/. Fast, unlimited, teaches the rules.
//   Submit  hidden-seed differential fuzzing against a reference engine. That
//           is the gate; passing the visible tests does not mean passing it.

#include <algorithm>
#include <cstdint>
#include <map>
#include <vector>

#include "mebench/engine.h"
#include "mebench/order.h"
#include "mebench/out.h"

using namespace mebench;

namespace {

struct OrderKey {
  uint16_t session_id;
  uint64_t client_order_id;

  bool operator<(const OrderKey& other) const {
    if (session_id != other.session_id) return session_id < other.session_id;
    return client_order_id < other.client_order_id;
  }
};

inline OrderKey key_of(const OrderRef& r) { return OrderKey{r.session_id, r.client_order_id}; }

// Where a resting order lives, so a cancel does not have to search the book.
struct Location {
  Side side;
  int32_t price;
};

// ============================================================================
//                            YOUR CODE STARTS HERE
// ============================================================================

class Engine final : public IMatchingEngine {
 public:
  void on_new(const Order& o, OutSink& out) noexcept override {
    last_seq_ = o.seq;  // every event, even a rejected one

    if (o.tif == TIF::FOK) {
      // TODO [F1]: read-only walk of the opposite side summing only OTHER
      // firms' resting quantity. If it is short of o.qty, emit the Reject and
      // return with the book untouched — no Ack. [F2, A3]
    }

    // [A1] every accepted order acks first, before any other output.
    out.emit(out::ack(o.seq, o.ref(), o.price, o.side));

    uint32_t remaining = o.qty;

    // TODO [1.1 Core]: walk the opposite book best price first, arrival order
    // within a level, while remaining > 0 and the level price is acceptable.
    // Same participant_id is [S2], not a trade. Trade price is the RESTING
    // order's price.
    (void)remaining;

    switch (o.tif) {
      case TIF::GTC:
        // TODO: rest the remainder silently and record it in the index. [A4]
        break;

      case TIF::IOC:
      case TIF::Market:
        // TODO: one Expired carrying the remainder, if any. [M3, I1]
        break;

      case TIF::FOK:
        // Nothing to do. [F3, F4]
        break;
    }
  }

  void on_cancel(OrderRef ref, uint64_t seq, OutSink& out) noexcept override {
    last_seq_ = seq;

    // TODO [1.4 Cancel]: look up the (session_id, client_order_id) pair.
    // Found -> remove it and cancel_ack the REMAINING quantity. Not found ->
    // reject with UnknownOrder.
    (void)ref;
    (void)seq;
    (void)out;
  }

  void snapshot(BookSnapshot& snap) const override {
    snap = BookSnapshot{};
    snap.at_seq = last_seq_;

    // TODO: fill n_bids / n_asks (occupied level counts) and the whole-book
    // totals from your book. The level vectors are NOT filled here — they come
    // from bid_levels() / ask_levels() below.
    snap.resting_qty_total = resting_qty_total_;
    snap.resting_order_count = resting_order_count_;
  }

  void bid_levels(LevelVec& out) const override {
    out.clear();
    // TODO: one (price, total_qty) pair per occupied bid level, HIGHEST price
    // first. total_qty is the sum of remaining quantity resting at that level.
  }

  void ask_levels(LevelVec& out) const override {
    out.clear();
    // TODO: one (price, total_qty) pair per occupied ask level, LOWEST price
    // first.
  }

 private:
  uint64_t last_seq_ = 0;
  uint64_t resting_qty_total_ = 0;
  uint32_t resting_order_count_ = 0;
};

}  // namespace

// The server dlopen()s your engine and looks up this symbol, so it must keep C
// linkage and this exact name.
extern "C" mebench::IMatchingEngine* create_engine() { return new Engine(); }
