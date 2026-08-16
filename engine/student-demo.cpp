// engine.cpp - a matching engine, explained top to bottom.
//
// Read this file straight through and you will have the whole correctness model
// in your head. It is a COMPLETE, correct engine: submit it unchanged and every
// correctness check passes. It is also deliberately simple and slow, so once you
// understand it, your job is to make it fast without breaking any of the rules
// narrated below.
//
// ------------------------------------------------------------------------
// THE JOB
// ------------------------------------------------------------------------
// A matching engine keeps an order book and matches incoming orders against it.
// You receive two kinds of input event:
//
//   New     an order arriving (a price, a quantity, a side, a time-in-force)
//   Cancel  a request to remove a still-resting order
//
// and you emit output events describing what happened:
//
//   Ack        the order was accepted (always first, for every accepted order)
//   Trade      a match: some quantity crossed at a price
//   CancelAck  a resting order was removed (by a cancel, or by self-trade)
//   Reject     the order was refused (unknown cancel, or an unfillable FOK)
//   Expired    an unfilled remainder was thrown away (IOC / market)
//
// The grader replays a stream through your engine and compares your output,
// event by event, against a correct reference. It also asks your engine for a
// book snapshot from time to time and checks that too. Match the reference
// exactly and you pass.
//
// ------------------------------------------------------------------------
// THE TWO RULES EVERYTHING RESTS ON
// ------------------------------------------------------------------------
// 1. PRICE-TIME PRIORITY. An aggressor takes the best price on the opposite
//    side first (highest bid / lowest ask). Within one price level, the order
//    that arrived earliest is filled first (first in, first out).
//
// 2. TRADE PRICE IS THE RESTING ORDER'S PRICE. When two orders match, the trade
//    happens at the price of the order that was already sitting in the book, not
//    the incoming one.
//
// ------------------------------------------------------------------------
// THE FOUR ORDER TYPES (time-in-force)
// ------------------------------------------------------------------------
//   GTC     a normal limit order. Matches what it can, then RESTS the rest in
//           the book to wait for future orders.
//   IOC     "immediate or cancel": matches what it can right now, then the
//           unfilled remainder EXPIRES. Never rests.
//   Market  like IOC but with no price limit: it takes any price. Never rests.
//   FOK     "fill or kill": either the WHOLE quantity can fill immediately, or
//           none of it does. Checked BEFORE touching the book (see below).
//
// ------------------------------------------------------------------------
// SELF-TRADE PREVENTION
// ------------------------------------------------------------------------
// A firm must never trade with itself (firms are identified by participant_id,
// not by session). When an aggressor meets a resting order from its own firm, we
// do NOT trade: we remove that resting order (emitting a CancelAck for it) and
// the aggressor keeps matching against the next order, unchanged.

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
  // ======================================================================
  // THE BOOK  (the state we keep between events)
  // ======================================================================
  // One resting order is the order itself plus how much of it is still live
  // (a partial fill leaves a smaller remainder resting, keeping its place).
  struct Resting {
    Order o;
    uint32_t remaining;
  };

  // A price LEVEL is all the orders resting at one price, oldest at the front.
  // A list keeps that first-in-first-out order and lets us remove from the
  // middle cheaply (a cancel can hit any order at the level).
  using Level = std::list<Resting>;

  // Each side is a map from price to level. The comparator puts the BEST price
  // at begin(): highest first for bids, lowest first for asks. So "walk the
  // opposite book best price first" is just iterating from begin().
  using BidsBook = std::map<int32_t, Level, std::greater<int32_t>>;
  using AsksBook = std::map<int32_t, Level, std::less<int32_t>>;

  // A cancel names an order by the (session_id, client_order_id) PAIR - the id
  // alone is not unique across sessions. This index lets a cancel find its
  // order without scanning the whole book.
  using Key = std::pair<uint16_t, uint64_t>;
  struct Locator {
    Side side;
    int32_t price;
    Level::iterator it;
  };

  BidsBook bids_;
  AsksBook asks_;
  std::map<Key, Locator> index_;

  // Running totals, so the book snapshot is cheap to produce.
  uint64_t last_seq_ = 0;
  uint64_t resting_qty_total_ = 0;
  uint32_t resting_order_count_ = 0;

  static Key key_of(const OrderRef& r) { return {r.session_id, r.client_order_id}; }

 public:
  // ======================================================================
  // A NEW ORDER ARRIVES  (the heart of the engine)
  // ======================================================================
  void on_new(const Order& o, OutSink& sink) noexcept override {
    last_seq_ = o.seq;
    uint32_t remaining = o.qty;

    // STEP 1 - FILL-OR-KILL is decided BEFORE anything else changes.
    // A FOK must fill completely or not at all, and a killed FOK emits ONLY a
    // Reject (no Ack). So we look before we leap: sum up how much OTHER firms'
    // liquidity is available (our own does not count, because self-trade would
    // cancel it, not fill it). If that is less than the whole order, reject and
    // leave the book untouched.
    if (o.tif == TIF::FOK) {
      const uint32_t available =
          o.side == Side::Buy ? fillable_qty(asks_, o) : fillable_qty(bids_, o);
      if (available < o.qty) {
        sink.emit(out::reject(o.seq, o.ref(), RejectReason::FokUnfillable, o.side));
        return;  // untouched book, no Ack
      }
    }

    // STEP 2 - ACK. Every accepted order is acknowledged exactly once, and the
    // Ack comes before any Trade or Expired for this event.
    sink.emit(out::ack(o.seq, o.ref(), o.price, o.side));

    // STEP 3 - MATCH against the opposite side (buys hit asks, sells hit bids).
    // This is where trades and self-trade cancellations happen; see match().
    if (o.side == Side::Buy) {
      match(asks_, o, remaining, sink);
    } else {
      match(bids_, o, remaining, sink);
    }

    // STEP 4 - THE REMAINDER. If nothing is left, we are done. Otherwise what
    // happens to the leftover depends on the order type.
    if (remaining == 0) return;
    switch (o.tif) {
      case TIF::GTC:
        // A limit order rests its remainder in the book, silently (the Ack in
        // step 2 already reported acceptance). It now waits for future orders.
        if (o.side == Side::Buy) rest(bids_, o, remaining);
        else rest(asks_, o, remaining);
        break;

      case TIF::IOC:
      case TIF::Market:
        // "Immediate" orders never rest: the unfilled remainder expires. Note
        // this fires even when the remainder only failed to fill because it met
        // its own firm's liquidity (which we cancelled during the match).
        sink.emit(out::expired(o.seq, o.ref(), remaining, o.side));
        break;

      case TIF::FOK:
        // Unreachable: step 1 proved a FOK either fully fills or was rejected.
        break;
    }
  }

  // ======================================================================
  // THE MATCH  (price-time priority, resting-price trades, self-trade)
  // ======================================================================
  // Walk the opposite book from the best price. At each level, take orders in
  // arrival order. Stop when the incoming order is exhausted, the book runs out,
  // or the next price is no longer acceptable (worse than a limit order's price;
  // a market order accepts any price).
  template <class Book>
  void match(Book& book, const Order& o, uint32_t& remaining, OutSink& sink) noexcept {
    auto lit = book.begin();
    while (remaining > 0 && lit != book.end()) {
      if (!acceptable(o.side, o.tif, o.price, lit->first)) break;

      Level& level = lit->second;
      auto oit = level.begin();
      while (remaining > 0 && oit != level.end()) {
        Resting& r = *oit;

        // SELF-TRADE: same firm. We do not trade; we remove the resting order
        // (CancelAck for its remaining quantity) and the aggressor moves on with
        // its own quantity unchanged. The CancelAck appears right here, in the
        // order the walk reached it.
        if (r.o.participant_id == o.participant_id) {
          sink.emit(out::stp_cancel_ack(o.seq, r.o.ref(), o.ref(), r.remaining, o.side));
          resting_qty_total_ -= r.remaining;
          --resting_order_count_;
          index_.erase(key_of(r.o.ref()));
          oit = level.erase(oit);
          continue;
        }

        // A TRADE. The quantity is whatever both sides can support, and the
        // price is the RESTING order's price (rule 2 above).
        const uint32_t q = remaining < r.remaining ? remaining : r.remaining;
        sink.emit(out::trade(o.seq, r.o.ref(), o.ref(), r.o.price, q, o.side));
        remaining -= q;
        r.remaining -= q;
        resting_qty_total_ -= q;

        if (r.remaining == 0) {
          // The resting order was fully consumed: remove it and move on.
          --resting_order_count_;
          index_.erase(key_of(r.o.ref()));
          oit = level.erase(oit);
        } else {
          // The resting order survived, which means the aggressor is spent; the
          // outer while will now see remaining == 0 and stop.
          ++oit;
        }
      }

      // An emptied price level is removed so the book only holds live prices.
      if (level.empty()) lit = book.erase(lit);
      else ++lit;
    }
  }

  // Is this level's price acceptable to the aggressor? A buy can trade at asks
  // up to its limit; a sell at bids down to its limit; a market order anywhere.
  static bool acceptable(Side side, TIF tif, int32_t aggr_price, int32_t level_price) {
    if (tif == TIF::Market) return true;
    return side == Side::Buy ? level_price <= aggr_price : level_price >= aggr_price;
  }

  // The FOK look-ahead: a READ-ONLY walk (it changes nothing) that sums other
  // firms' resting quantity, best price first, up to the limit price. It stops
  // early once it has seen enough. Skipping same-firm quantity is what makes FOK
  // and self-trade consistent: liquidity that would be cancelled cannot count.
  template <class Book>
  uint32_t fillable_qty(const Book& book, const Order& o) const {
    uint64_t sum = 0;
    for (const auto& [price, level] : book) {
      if (!acceptable(o.side, o.tif, o.price, price)) break;
      for (const auto& r : level) {
        if (r.o.participant_id == o.participant_id) continue;
        sum += r.remaining;
        if (sum >= o.qty) return o.qty;
      }
    }
    return static_cast<uint32_t>(sum);
  }

  // Rest a remainder: append to its price level (so it is last in FIFO order)
  // and record where it lives so a future cancel finds it in one lookup.
  template <class Book>
  void rest(Book& book, const Order& o, uint32_t remaining) noexcept {
    Level& level = book[o.price];
    level.push_back(Resting{o, remaining});
    index_[key_of(o.ref())] = Locator{o.side, o.price, std::prev(level.end())};
    resting_qty_total_ += remaining;
    ++resting_order_count_;
  }

  // ======================================================================
  // A CANCEL ARRIVES
  // ======================================================================
  void on_cancel(OrderRef ref, uint64_t seq, OutSink& sink) noexcept override {
    last_seq_ = seq;

    // Find the order by its (session, client_order_id) pair. If it is not in the
    // index it is either unknown or already fully filled - we cannot tell which,
    // and we are not required to: both get the same Reject.
    auto it = index_.find(key_of(ref));
    if (it == index_.end()) {
      sink.emit(out::reject(seq, ref, RejectReason::UnknownOrder, Side::Buy));
      return;
    }

    // Found: remove it and acknowledge with its REMAINING quantity (a partial
    // fill means the cancel carries the leftover, not the original size).
    const Locator loc = it->second;
    const uint32_t remaining = loc.it->remaining;
    const OrderRef resting_ref = loc.it->o.ref();
    const Side side = loc.it->o.side;

    index_.erase(it);
    if (side == Side::Buy) erase_at(bids_, loc.price, loc.it);
    else erase_at(asks_, loc.price, loc.it);
    resting_qty_total_ -= remaining;
    --resting_order_count_;

    sink.emit(out::cancel_ack(seq, resting_ref, remaining, side));
  }

  // Remove one order from its level, and drop the level if it becomes empty.
  template <class Book>
  void erase_at(Book& book, int32_t price, Level::iterator it) noexcept {
    auto lit = book.find(price);
    if (lit == book.end()) return;
    lit->second.erase(it);
    if (lit->second.empty()) book.erase(lit);
  }

  // ======================================================================
  // INSPECTING THE BOOK  (how the grader checks your state, not just output)
  // ======================================================================
  // Between events the grader may ask for the book's shape and totals and
  // compare them to the reference. This is what catches a silently dropped or
  // mis-priced resting order even when your output events looked fine.
  void snapshot(BookSnapshot& snap) const override {
    snap = BookSnapshot{};
    snap.at_seq = last_seq_;
    snap.n_bids = static_cast<uint32_t>(bids_.size());
    snap.n_asks = static_cast<uint32_t>(asks_.size());
    snap.resting_qty_total = resting_qty_total_;
    snap.resting_order_count = resting_order_count_;
  }

  // One (price, total resting quantity) pair per occupied level, best price
  // first - the same order the book is stored in.
  void bid_levels(LevelVec& out) const override { fill_levels(bids_, out); }
  void ask_levels(LevelVec& out) const override { fill_levels(asks_, out); }

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
};

}  // namespace

// The server loads your compiled engine and calls this once to create it. Keep
// the name and the extern "C" linkage.
extern "C" mebench::IMatchingEngine* create_engine() { return new Engine(); }
