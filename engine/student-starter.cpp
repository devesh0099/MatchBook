// engine.cpp - your starting engine.
//
// This is a COMPLETE, correct matching engine. Submit it unchanged and it
// passes every correctness check. It is written for clarity, not speed: a
// std::map of prices, a std::list of orders per price level, and a std::map
// index for cancels. That is easy to read and slow to run.
//
// Your job is to make it fast. The benchmark ranks you on p95 latency per
// event, and the ladder only lets you climb as your engine gets quicker, so
// every improvement to the hot paths (on_new / on_cancel) and to these data
// structures earns you levels. Correctness is a gate you have already cleared -
// keep it green while you optimize.
//
// You may restructure this file completely. The only fixed points are the
// frozen headers in mebench/ and the create_engine() entry point at the bottom.

#include <cstdint>
#include <list>
#include <map>
#include <utility>

#include "mebench/engine.h"
#include "mebench/order.h"
#include "mebench/out.h"

using namespace mebench;

namespace {

class Engine final : public IMatchingEngine {
 public:
  void on_new(const Order& o, OutSink& sink) noexcept override {
    last_seq_ = o.seq;
    uint32_t remaining = o.qty;

    // >>> ANSWER [F1/F2] - FOK fillability: check BEFORE any change to the book
    // and before the Ack; an unfillable FOK emits only the Reject.
    if (o.tif == TIF::FOK) {
      const uint32_t available =
          o.side == Side::Buy ? fillable_qty(asks_, o) : fillable_qty(bids_, o);
      if (available < o.qty) {
        sink.emit(out::reject(o.seq, o.ref(), RejectReason::FokUnfillable, o.side));
        return;  // book untouched: no trades, no cancellations, no Ack
      }
    }

    // Every accepted order acks exactly once, before any other output.
    sink.emit(out::ack(o.seq, o.ref(), o.price, o.side));

    // >>> ANSWER [1.1 Core] - match against the opposite side, best price first
    // (the loop body lives in match() below).
    if (o.side == Side::Buy) {
      match(asks_, o, remaining, sink);
    } else {
      match(bids_, o, remaining, sink);
    }

    if (remaining == 0) return;

    switch (o.tif) {
      case TIF::GTC:
        // >>> ANSWER [A4] - rest the remainder silently.
        if (o.side == Side::Buy) rest(bids_, o, remaining);
        else rest(asks_, o, remaining);
        break;

      case TIF::IOC:
      case TIF::Market:
        // >>> ANSWER [M3/I1] - one Expired carrying whatever did not fill.
        sink.emit(out::expired(o.seq, o.ref(), remaining, o.side));
        break;

      case TIF::FOK:
        // A FOK that reached here was fully fillable, so nothing is left over.
        break;
    }
  }

  void on_cancel(OrderRef ref, uint64_t seq, OutSink& sink) noexcept override {
    last_seq_ = seq;

    // >>> ANSWER [1.4 Cancel] - look up the (session_id, client_order_id) pair;
    // found: remove it and cancel-ack the REMAINING qty; not found: reject.
    auto it = index_.find(key_of(ref));
    if (it == index_.end()) {
      // Unknown, or already fully filled - either way it is no longer live.
      sink.emit(out::reject(seq, ref, RejectReason::UnknownOrder, Side::Buy));
      return;
    }

    const Locator loc = it->second;
    const uint32_t remaining = loc.it->remaining;  // the remainder, not the original qty
    const OrderRef resting_ref = loc.it->o.ref();
    const Side side = loc.it->o.side;

    index_.erase(it);
    if (side == Side::Buy) {
      erase_at(bids_, loc.price, loc.it);
    } else {
      erase_at(asks_, loc.price, loc.it);
    }
    resting_qty_total_ -= remaining;
    --resting_order_count_;

    sink.emit(out::cancel_ack(seq, resting_ref, remaining, side));
  }

  void snapshot(BookSnapshot& snap) const override {
    snap = BookSnapshot{};
    snap.at_seq = last_seq_;
    // >>> ANSWER [snapshot] - occupied level counts and whole-book totals.
    snap.n_bids = static_cast<uint32_t>(bids_.size());
    snap.n_asks = static_cast<uint32_t>(asks_.size());
    snap.resting_qty_total = resting_qty_total_;
    snap.resting_order_count = resting_order_count_;
  }

  // >>> ANSWER [levels] - one (price, total_qty) pair per occupied level, best
  // price first (bodies in fill_levels below).
  void bid_levels(LevelVec& out) const override { fill_levels(bids_, out); }
  void ask_levels(LevelVec& out) const override { fill_levels(asks_, out); }

 private:
  struct Resting {
    Order o;
    uint32_t remaining;
  };
  using Level = std::list<Resting>;
  // Best price first: highest first for bids, lowest first for asks.
  using BidsBook = std::map<int32_t, Level, std::greater<int32_t>>;
  using AsksBook = std::map<int32_t, Level, std::less<int32_t>>;

  // A cancel names an order by the (session_id, client_order_id) pair.
  using Key = std::pair<uint16_t, uint64_t>;

  struct Locator {
    Side side;
    int32_t price;
    Level::iterator it;
  };

  static Key key_of(const OrderRef& r) { return {r.session_id, r.client_order_id}; }

  static bool acceptable(Side aggressor_side, TIF tif, int32_t aggr_price, int32_t level_price) {
    if (tif == TIF::Market) return true;
    return aggressor_side == Side::Buy ? level_price <= aggr_price : level_price >= aggr_price;
  }

  // >>> ANSWER [F1] - read-only walk, best price first, bounded by the limit
  // price, summing only OTHER firms' resting quantity. Changes nothing.
  template <class Book>
  uint32_t fillable_qty(const Book& book, const Order& o) const {
    uint64_t sum = 0;
    for (const auto& [price, level] : book) {
      if (!acceptable(o.side, o.tif, o.price, price)) break;
      for (const auto& r : level) {
        if (r.o.participant_id == o.participant_id) continue;  // same firm never fills you
        sum += r.remaining;
        if (sum >= o.qty) return o.qty;
      }
    }
    return static_cast<uint32_t>(sum);
  }

  // >>> ANSWER [1.1 Core + S1/S2/S5] - walk the opposite book best price first,
  // arrival order within a level, emitting Trades (at the RESTING price) and
  // self-trade CancelAcks in book-walk order.
  template <class Book>
  void match(Book& book, const Order& o, uint32_t& remaining, OutSink& sink) noexcept {
    auto lit = book.begin();
    while (remaining > 0 && lit != book.end()) {
      if (!acceptable(o.side, o.tif, o.price, lit->first)) break;

      Level& level = lit->second;
      auto oit = level.begin();
      while (remaining > 0 && oit != level.end()) {
        Resting& r = *oit;

        // Same firm: remove the resting order, cancel-ack its remaining
        // quantity, and keep matching. The two never trade with each other.
        if (r.o.participant_id == o.participant_id) {
          sink.emit(out::stp_cancel_ack(o.seq, r.o.ref(), o.ref(), r.remaining, o.side));
          resting_qty_total_ -= r.remaining;
          --resting_order_count_;
          index_.erase(key_of(r.o.ref()));
          oit = level.erase(oit);
          continue;
        }

        // Trade price is the RESTING order's price, always.
        const uint32_t q = remaining < r.remaining ? remaining : r.remaining;
        sink.emit(out::trade(o.seq, r.o.ref(), o.ref(), r.o.price, q, o.side));
        remaining -= q;
        r.remaining -= q;
        resting_qty_total_ -= q;

        if (r.remaining == 0) {
          --resting_order_count_;
          index_.erase(key_of(r.o.ref()));
          oit = level.erase(oit);
        } else {
          ++oit;  // resting order survived, so the aggressor is exhausted
        }
      }

      if (level.empty()) {
        lit = book.erase(lit);
      } else {
        ++lit;
      }
    }
  }

  // >>> ANSWER [A4] - push the remainder onto its price level and index it so a
  // later cancel is O(log n), not a book scan.
  template <class Book>
  void rest(Book& book, const Order& o, uint32_t remaining) noexcept {
    Level& level = book[o.price];
    level.push_back(Resting{o, remaining});
    index_[key_of(o.ref())] = Locator{o.side, o.price, std::prev(level.end())};
    resting_qty_total_ += remaining;
    ++resting_order_count_;
  }

  // >>> ANSWER [1.4 Cancel] - remove a resting order from its level (drops the
  // level when it empties).
  template <class Book>
  void erase_at(Book& book, int32_t price, Level::iterator it) noexcept {
    auto lit = book.find(price);
    if (lit == book.end()) return;
    lit->second.erase(it);
    if (lit->second.empty()) book.erase(lit);
  }

  // >>> ANSWER [levels] - one (price, total_qty) pair per occupied level, in the
  // map's best-first order.
  template <class Book>
  void fill_levels(const Book& book, LevelVec& dst) const {
    dst.clear();
    dst.reserve(book.size());
    for (const auto& [price, level] : book) {
      uint64_t qty = 0;
      for (const auto& r : level) qty += r.remaining;
      dst.emplace_back(price, qty);
    }
  }

  BidsBook bids_;
  AsksBook asks_;
  std::map<Key, Locator> index_;
  uint64_t last_seq_ = 0;
  uint64_t resting_qty_total_ = 0;
  uint32_t resting_order_count_ = 0;
};

}  // namespace

// The server loads your compiled engine and calls this to create it. Keep the
// name and the C linkage.
extern "C" mebench::IMatchingEngine* create_engine() { return new Engine(); }
