// engine.cpp — your submission. This is the only file you write.
//
// It is also the only file the server compiles: one translation unit, against
// the read-only headers in include/mebench/. Everything below the line marked
// YOUR CODE STARTS HERE is yours to restructure completely — the data
// structures here are a starting point, not a required design.
//
// Read SPEC.md first. Every rule referenced in a // TODO below is numbered
// there, and the numbers are the contract.
//
// Run     = the ~30 visible tests in tests/. Fast, unlimited, teaches the rules.
// Submit  = hidden-seed differential fuzzing against the reference. That is the
//           gate. Passing the visible tests does not mean passing it.

#include <algorithm>
#include <cstdint>
#include <map>
#include <vector>

#include "mebench/engine.h"
#include "mebench/order.h"
#include "mebench/out.h"

using namespace mebench;

namespace {

// A resting order is the original order plus how much of it is left. An order
// that is partially filled keeps its place in the queue (price-time priority is
// on arrival sequence, and a partial fill does not change arrival time).
struct RestingOrder {
  Order o;
  uint32_t remaining;
};

// One price level: orders in arrival order, oldest at the front.
//
// FIFO is not decoration. The snapshot exposes front_seq precisely so that a
// LIFO queue is detectable — the aggregate quantity and order count look
// identical either way, so nothing else would catch it.
struct PriceLevel {
  std::vector<RestingOrder> orders;  // front() is the head of the queue
  uint64_t total_qty = 0;

  void push(const Order& o, uint32_t qty) {
    orders.push_back(RestingOrder{o, qty});
    total_qty += qty;
  }

  bool empty() const { return orders.empty(); }

  // Remove by index, preserving arrival order of everything behind it.
  void erase_at(size_t i) {
    total_qty -= orders[i].remaining;
    orders.erase(orders.begin() + static_cast<long>(i));
  }

  void reduce_at(size_t i, uint32_t by) {
    orders[i].remaining -= by;
    total_qty -= by;
  }
};

// Bids are best-first at the HIGHEST price, asks at the LOWEST. Each map's own
// comparator makes begin() the touch.
using Bids = std::map<int32_t, PriceLevel, std::greater<int32_t>>;
using Asks = std::map<int32_t, PriceLevel, std::less<int32_t>>;

// Cancel identity is the (session_id, client_order_id) PAIR, never the id
// alone: client order ids are unique only within a session, and the stream
// deliberately contains two sessions using the same value at the same time.
// Keying on the id alone will cancel the wrong order and look correct for a
// long time first.
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
    // ---- FOK is checked BEFORE anything is mutated and BEFORE the Ack -------
    // An unfillable FOK emits only the Reject, with no Ack at all (SPEC A3/F2).
    if (o.tif == TIF::FOK) {
      // TODO (SPEC F1): read-only walk of the opposite side, best price first,
      // bounded by o.px, summing the remaining quantity of resting orders whose
      // participant_id DIFFERS from o.participant_id.
      //
      // The same-firm exclusion is the subtle part and it is not industry
      // practice — see the worked 60/50/100 example in SPEC section 3.6.
      // Counting your own resting liquidity makes a FOK commit and then
      // partially fill, which F4 forbids.
      //
      //   if (fillable < o.qty) {
      //     out.emit(out::reject(o.seq, o.ref(), RejectReason::FokUnfillable, o.side));
      //     return;                       // book untouched: no trades, no STP
      //   }
    }

    // ---- every accepted order acks first (SPEC A1) --------------------------
    out.emit(out::ack(o.seq, o.ref(), o.px, o.side));

    uint32_t remaining = o.qty;

    // ---- match against the opposite side -----------------------------------
    // TODO (SPEC section 3.1): walk the opposite book best price first, and
    // within each level in arrival order, while `remaining > 0` and the level's
    // price is acceptable:
    //
    //   a market order (TIF::Market) accepts every price
    //   a buy accepts levels priced <= o.px
    //   a sell accepts levels priced >= o.px
    //
    // For each resting order reached:
    //
    //   same participant_id  -> SPEC S2: remove the RESTING order, emit
    //                           out::stp_cancel_ack(...) carrying its remaining
    //                           quantity, and CONTINUE matching with the
    //                           aggressor's quantity unchanged. No trade.
    //   otherwise            -> trade min(remaining, resting.remaining) at the
    //                           RESTING order's price, never the aggressor's.
    //                           This is the most common first-submission bug.
    //
    // Emission is book-walk order, so an STP CancelAck appears interleaved
    // exactly where the resting order sat (SPEC S5).
    (void)remaining;

    // ---- what happens to the remainder -------------------------------------
    // TODO: dispatch on time-in-force. Every branch is a numbered rule.
    switch (o.tif) {
      case TIF::Day:
        // TODO: rest the remainder silently — no second Ack, no Expired. The
        // Ack already covered acceptance (SPEC A4). Remember to record it in
        // the cancel index.
        break;

      case TIF::IOC:
      case TIF::Market:
        // TODO (SPEC M3 / I1): emit exactly one Expired carrying the
        // remainder, if any. This INCLUDES quantity that met only same-firm
        // liquidity which STP just cancelled. A fully filled IOC emits no
        // Expired at all.
        break;

      case TIF::FOK:
        // Nothing to do: F3 guarantees an accepted FOK filled completely, and
        // F4 forbids it from ever emitting Expired.
        break;
    }
  }

  void on_cancel(OrderRef ref, uint64_t seq, OutSink& out) noexcept override {
    // TODO (SPEC section 3.4):
    //
    //   found     -> remove it and emit out::cancel_ack(seq, ref, REMAINING,
    //                side). The remaining quantity, not the original: this is
    //                the only case where a cancel carries a meaningful number.
    //   not found -> out::reject(seq, ref, RejectReason::UnknownOrder,
    //                Side::Buy). "Already fully filled" and "never existed" are
    //                indistinguishable, and you are not asked to tell them
    //                apart. An order removed earlier by STP is also gone.
    //
    // Look it up by the (session_id, client_order_id) pair. See the note on
    // OrderKey above.
    (void)ref;
    (void)seq;
    (void)out;
  }

  // Called OUTSIDE the timed region — correctness snapshots only. It costs you
  // nothing at benchmark time, so write it straightforwardly. It is what
  // catches a silently dropped resting order, and a book that disagrees with
  // your own output fails the gate.
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

    // These cover the WHOLE book, not just the levels written above.
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

// The server dlopen()s your compiled engine and looks up this symbol, so it
// must keep C linkage and this exact name.
extern "C" mebench::IMatchingEngine* create_engine() { return new Engine(); }
