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

// An order plus how much of it is left. A partial fill does not change arrival
// time, so a partially filled order keeps its place in the queue.
struct RestingOrder {
  Order o;
  uint32_t remaining;
};

// One price level, oldest at the front.
struct PriceLevel {
  std::vector<RestingOrder> orders;
  uint64_t total_qty = 0;

  void push(const Order& o, uint32_t qty) {
    orders.push_back(RestingOrder{o, qty});
    total_qty += qty;
  }

  bool empty() const { return orders.empty(); }

  // Removal preserves the arrival order of everything behind it.
  void erase_at(size_t i) {
    total_qty -= orders[i].remaining;
    orders.erase(orders.begin() + static_cast<long>(i));
  }

  void reduce_at(size_t i, uint32_t by) {
    orders[i].remaining -= by;
    total_qty -= by;
  }
};

// Each map's own comparator makes begin() the touch: bids highest price first,
// asks lowest first.
using Bids = std::map<int32_t, PriceLevel, std::greater<int32_t>>;
using Asks = std::map<int32_t, PriceLevel, std::less<int32_t>>;

// Keyed on the PAIR, never client_order_id alone — see the note on OrderRef in
// order.h. Keying on the id alone cancels the wrong order and looks correct for
// a long time first.
//
// This index is where most of the latency spread in this contest comes from.
// std::map is a correct starting point and a slow one.
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
  int32_t px;
};

// ============================================================================
//                            YOUR CODE STARTS HERE
// ============================================================================

class Engine final : public IMatchingEngine {
 public:
  void on_new(const Order& o, OutSink& out) noexcept override {
    if (o.tif == TIF::FOK) {
      // TODO [F1]: read-only walk of the opposite side summing only OTHER
      // firms' resting quantity. If it is short of o.qty, emit the Reject and
      // return with the book untouched — no Ack. [F2, A3]
    }

    // [A1] every accepted order acks first, before any other output.
    out.emit(out::ack(o.seq, o.ref(), o.px, o.side));

    uint32_t remaining = o.qty;

    // TODO [1.1 Core]: walk the opposite book best price first, arrival order
    // within a level, while remaining > 0 and the level price is acceptable.
    // Same participant_id is [S2], not a trade. Trade price is the RESTING
    // order's price.
    (void)remaining;

    switch (o.tif) {
      case TIF::Day:
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
    // TODO [1.4 Cancel]: look up the (session_id, client_order_id) pair.
    // Found -> remove it and cancel_ack the REMAINING quantity. Not found ->
    // reject with UnknownOrder.
    (void)ref;
    (void)seq;
    (void)out;
  }

  // Outside the timed region, so it costs nothing at benchmark time. Write it
  // straightforwardly: it is what catches a silently dropped resting order.
  void snapshot(BookSnapshot& snap) const override {
    snap = BookSnapshot{};
    snap.at_seq = last_seq_;

    uint32_t n = 0;
    for (const auto& [px, level] : bids_) {
      if (n == kSnapshotLevels) break;
      snap.bids[n++] = make_level(px, level);
    }
    snap.n_bids = n;

    n = 0;
    for (const auto& [px, level] : asks_) {
      if (n == kSnapshotLevels) break;
      snap.asks[n++] = make_level(px, level);
    }
    snap.n_asks = n;

    snap.resting_qty_total = resting_qty_total_;
    snap.resting_order_count = resting_order_count_;
  }

 private:
  static LevelSnapshot make_level(int32_t px, const PriceLevel& level) {
    LevelSnapshot s{};
    s.px = px;
    s.total_qty = level.total_qty;
    s.order_count = static_cast<uint32_t>(level.orders.size());
    s.front_seq = level.orders.empty() ? 0 : level.orders.front().o.seq;
    return s;
  }

  Bids bids_;
  Asks asks_;
  std::map<OrderKey, Location> index_;

  uint64_t last_seq_ = 0;
  uint64_t resting_qty_total_ = 0;
  uint32_t resting_order_count_ = 0;
};

}  // namespace

// The server dlopen()s your engine and looks up this symbol, so it must keep C
// linkage and this exact name.
extern "C" mebench::IMatchingEngine* create_engine() { return new Engine(); }
