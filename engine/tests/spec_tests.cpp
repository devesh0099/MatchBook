// tests/spec_tests.cpp — one case per SPEC rule.
//
// Expected outputs are built with the out:: helpers from the frozen header, so
// every field is compared, including the ones that must be zeroed. That is
// deliberate: the benchmark digest folds every field too (SPEC §4 Contest rules), so a test
// that ignored `maker` on an Ack would pass code that fails the ranked run.

#include "tests/spec_tests.h"

#include <cstring>
#include <sstream>

#include "mebench/order.h"
#include "mebench/out.h"

namespace mebench::tests {
namespace {

// ---------------------------------------------------------------- plumbing

struct Rec : OutSink {
  std::vector<OutEvent> ev;
  Rec() { ev.reserve(256); }
  void emit(const OutEvent& e) noexcept override { ev.push_back(e); }
  void clear() { ev.clear(); }
};

Order ord(uint64_t seq, Side side, int32_t price, uint32_t qty, uint16_t sess, uint64_t coid,
          uint16_t firm, TIF tif = TIF::GTC) {
  Order o{};
  o.seq = seq;
  o.client_order_id = coid;
  o.price = price;
  o.qty = qty;
  o.session_id = sess;
  o.participant_id = firm;
  o.side = side;
  o.tif = tif;
  return o;
}

OrderRef rf(uint16_t sess, uint64_t coid, uint16_t firm) { return OrderRef{coid, sess, firm, 0}; }

bool same(const OutEvent& a, const OutEvent& b) {
  return a.in_seq == b.in_seq && a.maker == b.maker && a.taker == b.taker && a.price == b.price &&
         a.qty == b.qty && a.type == b.type && a.aggressor_side == b.aggressor_side &&
         a.reason == b.reason;
}

const char* type_name(OutType t) {
  switch (t) {
    case OutType::Trade: return "Trade";
    case OutType::Ack: return "Ack";
    case OutType::Reject: return "Reject";
    case OutType::CancelAck: return "CancelAck";
    case OutType::Expired: return "Expired";
  }
  return "?";
}

const char* reason_name(RejectReason r) {
  switch (r) {
    case RejectReason::None: return "None";
    case RejectReason::UnknownOrder: return "UnknownOrder";
    case RejectReason::FokUnfillable: return "FokUnfillable";
  }
  return "?";
}

std::string describe(const OutEvent& e) {
  std::ostringstream s;
  s << type_name(e.type) << " in_seq=" << e.in_seq << " price=" << e.price << " qty=" << e.qty
    << " maker=(s" << e.maker.session_id << ",c" << e.maker.client_order_id << ",f"
    << e.maker.participant_id << ")"
    << " taker=(s" << e.taker.session_id << ",c" << e.taker.client_order_id << ",f"
    << e.taker.participant_id << ")"
    << " side=" << (e.aggressor_side == Side::Buy ? "Buy" : "Sell");
  if (e.reason != RejectReason::None) s << " reason=" << reason_name(e.reason);
  return s.str();
}

std::string diff(const std::vector<OutEvent>& got, const std::vector<OutEvent>& want) {
  bool ok = got.size() == want.size();
  for (size_t i = 0; ok && i < got.size(); ++i) ok = same(got[i], want[i]);
  if (ok) return {};

  std::ostringstream s;
  s << "output mismatch (" << want.size() << " expected, " << got.size() << " emitted)\n";
  const size_t n = got.size() > want.size() ? got.size() : want.size();
  for (size_t i = 0; i < n; ++i) {
    const bool has_g = i < got.size(), has_w = i < want.size();
    const bool row_ok = has_g && has_w && same(got[i], want[i]);
    s << (row_ok ? "    " : "  > ") << "#" << i << "\n";
    s << "      expected: " << (has_w ? describe(want[i]) : std::string("<nothing>")) << "\n";
    s << "      actual:   " << (has_g ? describe(got[i]) : std::string("<nothing>")) << "\n";
  }
  return s.str();
}

std::string expect_book(IMatchingEngine& e, uint32_t n_bids, uint32_t n_asks, uint64_t total_qty,
                        uint32_t total_orders) {
  BookSnapshot s{};
  build_book(e, s);
  if (s.n_bids == n_bids && s.n_asks == n_asks && s.resting_qty_total == total_qty &&
      s.resting_order_count == total_orders) {
    return {};
  }
  std::ostringstream o;
  o << "book mismatch\n"
    << "      expected: n_bids=" << n_bids << " n_asks=" << n_asks << " qty=" << total_qty
    << " orders=" << total_orders << "\n"
    << "      actual:   n_bids=" << s.n_bids << " n_asks=" << s.n_asks
    << " qty=" << s.resting_qty_total << " orders=" << s.resting_order_count;
  return o.str();
}

using Fn = std::string (*)(IMatchingEngine&);

struct Case {
  const char* name;
  const char* rule;
  Fn fn;
};

// ---------------------------------------------------------------- A: acks

// A1/A4: a non-marketable limit emits exactly one Ack and nothing else.
std::string t_ack_only(IMatchingEngine& e) {
  Rec r;
  e.on_new(ord(1, Side::Buy, 10000, 100, 1, 1, 1), r);
  return diff(r.ev, {out::ack(1, rf(1, 1, 1), 10000, Side::Buy)});
}

// A1: the Ack precedes every trade for the same event.
std::string t_ack_before_trades(IMatchingEngine& e) {
  Rec r;
  e.on_new(ord(1, Side::Sell, 10000, 100, 1, 1, 1), r);
  r.clear();
  e.on_new(ord(2, Side::Buy, 10000, 100, 2, 2, 2), r);
  if (r.ev.empty() || r.ev[0].type != OutType::Ack) return "first output for a New must be the Ack";
  return diff(r.ev, {out::ack(2, rf(2, 2, 2), 10000, Side::Buy),
                     out::trade(2, rf(1, 1, 1), rf(2, 2, 2), 10000, 100, Side::Buy)});
}

// ---------------------------------------------------------------- core matching

// §1.1 Core: the trade price is the RESTING order's price, never the aggressor's.
// The single most common first-submission failure.
std::string t_trade_price_is_resting(IMatchingEngine& e) {
  Rec r;
  e.on_new(ord(1, Side::Sell, 9999, 100, 7, 12, 7), r);  // resting ask at 9999
  r.clear();
  e.on_new(ord(2, Side::Buy, 10005, 100, 3, 77, 3), r);  // aggressor willing to pay 10005
  return diff(r.ev, {out::ack(2, rf(3, 77, 3), 10005, Side::Buy),
                     out::trade(2, rf(7, 12, 7), rf(3, 77, 3), 9999, 100, Side::Buy)});
}

// §1.1 Core: best price first.
std::string t_price_priority(IMatchingEngine& e) {
  Rec r;
  e.on_new(ord(1, Side::Sell, 10002, 50, 1, 1, 1), r);
  e.on_new(ord(2, Side::Sell, 10000, 50, 1, 2, 1), r);  // better price, arrived later
  r.clear();
  e.on_new(ord(3, Side::Buy, 10002, 100, 2, 3, 2), r);
  return diff(r.ev, {out::ack(3, rf(2, 3, 2), 10002, Side::Buy),
                     out::trade(3, rf(1, 2, 1), rf(2, 3, 2), 10000, 50, Side::Buy),
                     out::trade(3, rf(1, 1, 1), rf(2, 3, 2), 10002, 50, Side::Buy)});
}

// §1.1 Core: within a level, lowest seq first. "Time" means arrival sequence.
std::string t_time_priority_fifo(IMatchingEngine& e) {
  Rec r;
  e.on_new(ord(1, Side::Sell, 10000, 30, 1, 1, 1), r);
  e.on_new(ord(2, Side::Sell, 10000, 30, 1, 2, 1), r);
  e.on_new(ord(3, Side::Sell, 10000, 30, 1, 3, 1), r);
  r.clear();
  e.on_new(ord(4, Side::Buy, 10000, 90, 2, 9, 2), r);
  return diff(r.ev, {out::ack(4, rf(2, 9, 2), 10000, Side::Buy),
                     out::trade(4, rf(1, 1, 1), rf(2, 9, 2), 10000, 30, Side::Buy),
                     out::trade(4, rf(1, 2, 1), rf(2, 9, 2), 10000, 30, Side::Buy),
                     out::trade(4, rf(1, 3, 1), rf(2, 9, 2), 10000, 30, Side::Buy)});
}

// §1.1 Core: an aggressor smaller than the resting order takes part of it; the
// resting order stays at the head of its level with the remainder.
std::string t_partial_fill_resting_survives(IMatchingEngine& e) {
  Rec r;
  e.on_new(ord(1, Side::Sell, 10000, 100, 1, 1, 1), r);
  r.clear();
  e.on_new(ord(2, Side::Buy, 10000, 40, 2, 2, 2), r);
  if (auto d = diff(r.ev, {out::ack(2, rf(2, 2, 2), 10000, Side::Buy),
                           out::trade(2, rf(1, 1, 1), rf(2, 2, 2), 10000, 40, Side::Buy)});
      !d.empty()) {
    return d;
  }
  return expect_book(e, 0, 1, 60, 1);
}

// A4: a marketable limit that only partially fills rests its remainder
// silently — no second Ack, no Expired.
std::string t_marketable_limit_rests_remainder(IMatchingEngine& e) {
  Rec r;
  e.on_new(ord(1, Side::Sell, 10000, 40, 1, 1, 1), r);
  r.clear();
  e.on_new(ord(2, Side::Buy, 10000, 100, 2, 2, 2), r);
  if (auto d = diff(r.ev, {out::ack(2, rf(2, 2, 2), 10000, Side::Buy),
                           out::trade(2, rf(1, 1, 1), rf(2, 2, 2), 10000, 40, Side::Buy)});
      !d.empty()) {
    return d;
  }
  return expect_book(e, 1, 0, 60, 1);  // 60 resting as a bid, asks empty
}

// §1.1 Core: a limit order never trades through its own limit price.
std::string t_limit_price_bound(IMatchingEngine& e) {
  Rec r;
  e.on_new(ord(1, Side::Sell, 10000, 50, 1, 1, 1), r);
  e.on_new(ord(2, Side::Sell, 10001, 50, 1, 2, 1), r);
  r.clear();
  e.on_new(ord(3, Side::Buy, 10000, 100, 2, 3, 2), r);  // may not pay 10001
  if (auto d = diff(r.ev, {out::ack(3, rf(2, 3, 2), 10000, Side::Buy),
                           out::trade(3, rf(1, 1, 1), rf(2, 3, 2), 10000, 50, Side::Buy)});
      !d.empty()) {
    return d;
  }
  return expect_book(e, 1, 1, 100, 2);  // 50 rests as bid @10000, ask @10001 untouched
}

// Invariant (book invariant): the remainder rests at its own limit price, so the book is
// never left crossed.
std::string t_book_never_crossed(IMatchingEngine& e) {
  Rec r;
  e.on_new(ord(1, Side::Buy, 10000, 100, 1, 1, 1), r);
  r.clear();
  e.on_new(ord(2, Side::Sell, 9999, 150, 2, 2, 2), r);
  if (auto d = diff(r.ev, {out::ack(2, rf(2, 2, 2), 9999, Side::Sell),
                           out::trade(2, rf(1, 1, 1), rf(2, 2, 2), 10000, 100, Side::Sell)});
      !d.empty()) {
    return d;
  }
  BookSnapshot s{};
  build_book(e, s);
  if (s.n_bids != 0 || s.n_asks != 1 || s.asks.front().first != 9999) {
    return "remainder must rest at its own price (9999) with the bid fully consumed";
  }
  return {};
}

// §1.1 Core: a partial fill does not change arrival time — the partially
// filled order keeps its place at the head of the level. The snapshot no
// longer exposes per-order queues, so this is proven behaviorally: the next
// aggressor must trade the partially filled order's REMAINDER first.
std::string t_partial_fill_keeps_queue_head(IMatchingEngine& e) {
  Rec r;
  e.on_new(ord(1, Side::Sell, 10000, 100, 1, 1, 1), r);
  e.on_new(ord(2, Side::Sell, 10000, 50, 1, 2, 1), r);
  e.on_new(ord(3, Side::Buy, 10000, 40, 2, 3, 2), r);  // fills 40 of order 1
  r.clear();
  e.on_new(ord(4, Side::Buy, 10000, 70, 2, 4, 2), r);
  // Head kept: 60 remaining of order 1 first, then 10 of order 2. A
  // pop-and-reappend engine would trade order 2's 50 first instead.
  return diff(r.ev, {out::ack(4, rf(2, 4, 2), 10000, Side::Buy),
                     out::trade(4, rf(1, 1, 1), rf(2, 4, 2), 10000, 60, Side::Buy),
                     out::trade(4, rf(1, 2, 1), rf(2, 4, 2), 10000, 10, Side::Buy)});
}

// ---------------------------------------------------------------- M: market orders

// M1/M4: a market order acks on receipt like every other order.
std::string t_market_acks(IMatchingEngine& e) {
  Rec r;
  e.on_new(ord(1, Side::Sell, 10000, 100, 1, 1, 1), r);
  r.clear();
  e.on_new(ord(2, Side::Buy, 0, 100, 2, 2, 2, TIF::Market), r);
  return diff(r.ev, {out::ack(2, rf(2, 2, 2), 0, Side::Buy),
                     out::trade(2, rf(1, 1, 1), rf(2, 2, 2), 10000, 100, Side::Buy)});
}

// M2: matches at EACH LEVEL's resting price, sweeping best price first.
std::string t_market_sweeps_at_resting_prices(IMatchingEngine& e) {
  Rec r;
  e.on_new(ord(1, Side::Sell, 10000, 40, 1, 1, 1), r);
  e.on_new(ord(2, Side::Sell, 10001, 40, 1, 2, 1), r);
  e.on_new(ord(3, Side::Sell, 10002, 40, 1, 3, 1), r);
  r.clear();
  e.on_new(ord(4, Side::Buy, 0, 100, 2, 9, 2, TIF::Market), r);
  return diff(r.ev, {out::ack(4, rf(2, 9, 2), 0, Side::Buy),
                     out::trade(4, rf(1, 1, 1), rf(2, 9, 2), 10000, 40, Side::Buy),
                     out::trade(4, rf(1, 2, 1), rf(2, 9, 2), 10001, 40, Side::Buy),
                     out::trade(4, rf(1, 3, 1), rf(2, 9, 2), 10002, 20, Side::Buy)});
}

// M3/M4: into an empty opposite side — Ack, Expired, nothing else. Never a
// Reject for lack of liquidity.
std::string t_market_into_empty_book(IMatchingEngine& e) {
  Rec r;
  e.on_new(ord(1, Side::Buy, 0, 100, 1, 1, 1, TIF::Market), r);
  if (auto d = diff(r.ev, {out::ack(1, rf(1, 1, 1), 0, Side::Buy),
                           out::expired(1, rf(1, 1, 1), 100, Side::Buy)});
      !d.empty()) {
    return d;
  }
  return expect_book(e, 0, 0, 0, 0);
}

// M3: partial fill leaves exactly one Expired carrying the remainder.
std::string t_market_partial_expires_remainder(IMatchingEngine& e) {
  Rec r;
  e.on_new(ord(1, Side::Sell, 10000, 30, 1, 1, 1), r);
  r.clear();
  e.on_new(ord(2, Side::Buy, 0, 100, 2, 2, 2, TIF::Market), r);
  return diff(r.ev, {out::ack(2, rf(2, 2, 2), 0, Side::Buy),
                     out::trade(2, rf(1, 1, 1), rf(2, 2, 2), 10000, 30, Side::Buy),
                     out::expired(2, rf(2, 2, 2), 70, Side::Buy)});
}

// M1: a market order is never rested, not even its unfilled remainder.
std::string t_market_never_rests(IMatchingEngine& e) {
  Rec r;
  e.on_new(ord(1, Side::Sell, 10000, 30, 1, 1, 1), r);
  e.on_new(ord(2, Side::Buy, 0, 100, 2, 2, 2, TIF::Market), r);
  return expect_book(e, 0, 0, 0, 0);
}

// ---------------------------------------------------------------- IOC

// A4: nothing matched — Ack, Expired for the whole quantity.
std::string t_ioc_no_liquidity(IMatchingEngine& e) {
  Rec r;
  e.on_new(ord(1, Side::Buy, 10000, 100, 1, 1, 1, TIF::IOC), r);
  if (auto d = diff(r.ev, {out::ack(1, rf(1, 1, 1), 10000, Side::Buy),
                           out::expired(1, rf(1, 1, 1), 100, Side::Buy)});
      !d.empty()) {
    return d;
  }
  return expect_book(e, 0, 0, 0, 0);
}

// A2/A4: trades then exactly one Expired.
std::string t_ioc_partial(IMatchingEngine& e) {
  Rec r;
  e.on_new(ord(1, Side::Sell, 10000, 40, 1, 1, 1), r);
  r.clear();
  e.on_new(ord(2, Side::Buy, 10000, 100, 2, 2, 2, TIF::IOC), r);
  return diff(r.ev, {out::ack(2, rf(2, 2, 2), 10000, Side::Buy),
                     out::trade(2, rf(1, 1, 1), rf(2, 2, 2), 10000, 40, Side::Buy),
                     out::expired(2, rf(2, 2, 2), 60, Side::Buy)});
}

// A4: a fully filled IOC emits NO Expired.
std::string t_ioc_full_fill_no_expired(IMatchingEngine& e) {
  Rec r;
  e.on_new(ord(1, Side::Sell, 10000, 100, 1, 1, 1), r);
  r.clear();
  e.on_new(ord(2, Side::Buy, 10000, 100, 2, 2, 2, TIF::IOC), r);
  return diff(r.ev, {out::ack(2, rf(2, 2, 2), 10000, Side::Buy),
                     out::trade(2, rf(1, 1, 1), rf(2, 2, 2), 10000, 100, Side::Buy)});
}

// ---------------------------------------------------------------- cancel

// §1.4 Cancel: cancel of a live order — CancelAck carrying its remaining quantity.
std::string t_cancel_live(IMatchingEngine& e) {
  Rec r;
  e.on_new(ord(1, Side::Buy, 10000, 100, 4, 55, 4), r);
  r.clear();
  e.on_cancel(rf(4, 55, 4), 2, r);
  if (auto d = diff(r.ev, {out::cancel_ack(2, rf(4, 55, 4), 100, Side::Buy)}); !d.empty()) return d;
  return expect_book(e, 0, 0, 0, 0);
}

// §1.4 Cancel: the CancelAck carries the REMAINDER, not the original quantity. This is
// the only case where a cancel carries a meaningful quantity.
std::string t_cancel_partially_filled_carries_remainder(IMatchingEngine& e) {
  Rec r;
  e.on_new(ord(1, Side::Sell, 10000, 100, 1, 1, 1), r);
  e.on_new(ord(2, Side::Buy, 10000, 30, 2, 2, 2), r);  // consumes 30 of it
  r.clear();
  e.on_cancel(rf(1, 1, 1), 3, r);
  return diff(r.ev, {out::cancel_ack(3, rf(1, 1, 1), 70, Side::Sell)});
}

// §1.4 Cancel: unknown id — Reject/UnknownOrder. Not silent, not fatal.
std::string t_cancel_unknown(IMatchingEngine& e) {
  Rec r;
  e.on_cancel(rf(9, 999, 9), 1, r);
  return diff(r.ev, {out::reject(1, rf(9, 999, 9), RejectReason::UnknownOrder, Side::Buy)});
}

// §1.4 Cancel: already fully filled is indistinguishable from never existed.
std::string t_cancel_already_filled(IMatchingEngine& e) {
  Rec r;
  e.on_new(ord(1, Side::Sell, 10000, 100, 1, 1, 1), r);
  e.on_new(ord(2, Side::Buy, 10000, 100, 2, 2, 2), r);  // fully consumes it
  r.clear();
  e.on_cancel(rf(1, 1, 1), 3, r);
  return diff(r.ev, {out::reject(3, rf(1, 1, 1), RejectReason::UnknownOrder, Side::Buy)});
}

// §1.4 Cancel: the second cancel of the same order is an unknown-order reject.
std::string t_double_cancel(IMatchingEngine& e) {
  Rec r;
  e.on_new(ord(1, Side::Buy, 10000, 100, 1, 1, 1), r);
  e.on_cancel(rf(1, 1, 1), 2, r);
  r.clear();
  e.on_cancel(rf(1, 1, 1), 3, r);
  return diff(r.ev, {out::reject(3, rf(1, 1, 1), RejectReason::UnknownOrder, Side::Buy)});
}

// Terminology: identity is the (session_id, client_order_id) PAIR. Two sessions using
// client_order_id = 1 concurrently is a deliberate adversarial case; keying a
// map on the id alone cancels the wrong order here.
std::string t_cancel_identity_is_the_pair(IMatchingEngine& e) {
  Rec r;
  e.on_new(ord(1, Side::Buy, 10000, 100, 3, 1, 3), r);  // session 3, coid 1
  e.on_new(ord(2, Side::Buy, 9999, 50, 7, 1, 7), r);    // session 7, coid 1 — same id!
  r.clear();
  // Cancel BOTH, first-inserted first. A map keyed on coid alone fails one of
  // the two whichever insert "won": overwrite semantics cancel the wrong order
  // here; keep-first semantics reject session 7's cancel below.
  e.on_cancel(rf(3, 1, 3), 3, r);
  if (auto d = diff(r.ev, {out::cancel_ack(3, rf(3, 1, 3), 100, Side::Buy)}); !d.empty()) return d;
  r.clear();
  e.on_cancel(rf(7, 1, 7), 4, r);
  if (auto d = diff(r.ev, {out::cancel_ack(4, rf(7, 1, 7), 50, Side::Buy)}); !d.empty()) return d;
  return expect_book(e, 0, 0, 0, 0);
}

// §1.1 Core: cancelling the last order at a level removes the level.
std::string t_cancel_last_order_at_level(IMatchingEngine& e) {
  Rec r;
  e.on_new(ord(1, Side::Sell, 10000, 100, 1, 1, 1), r);
  e.on_new(ord(2, Side::Sell, 10001, 100, 1, 2, 1), r);
  e.on_cancel(rf(1, 1, 1), 3, r);
  BookSnapshot s{};
  build_book(e, s);
  if (s.n_asks != 1 || s.asks.front().first != 10001)
    return "empty level must disappear from the book";
  return {};
}

// engine.h: bid_levels()/ask_levels() return (price, total_qty) BEST price
// first — bids highest first, asks lowest first — with per-level quantities
// that track fills and cancels.
std::string t_levels_best_first(IMatchingEngine& e) {
  Rec r;
  e.on_new(ord(1, Side::Sell, 10002, 10, 1, 1, 1), r);
  e.on_new(ord(2, Side::Sell, 10000, 20, 1, 2, 1), r);
  e.on_new(ord(3, Side::Sell, 10000, 5, 1, 3, 1), r);
  e.on_new(ord(4, Side::Buy, 9999, 30, 1, 4, 1), r);
  e.on_new(ord(5, Side::Buy, 9997, 40, 1, 5, 1), r);
  BookSnapshot s{};
  build_book(e, s);
  const LevelVec want_asks{{10000, 25}, {10002, 10}};  // lowest first, 20+5 merged
  const LevelVec want_bids{{9999, 30}, {9997, 40}};    // highest first
  if (s.asks != want_asks) return "asks must be [(10000,25),(10002,10)], lowest price first";
  if (s.bids != want_bids) return "bids must be [(9999,30),(9997,40)], highest price first";
  e.on_cancel(rf(1, 2, 1), 6, r);  // remove 20 of the 25 at 10000
  build_book(e, s);
  if (s.asks != LevelVec{{10000, 5}, {10002, 10}}) {
    return "cancelling one order must leave (10000,5) at the touch";
  }
  return {};
}

// §2 Input guarantees: a (session_id, client_order_id) pair may be reused once
// the earlier order is gone. A stale map entry left behind by the first order
// would make this cancel miss the NEW order or report the wrong side.
std::string t_id_reuse_after_death(IMatchingEngine& e) {
  Rec r;
  e.on_new(ord(1, Side::Buy, 10000, 100, 1, 7, 1), r);
  e.on_cancel(rf(1, 7, 1), 2, r);                       // pair (1,7) now dead
  e.on_new(ord(3, Side::Sell, 10005, 40, 1, 7, 1), r);  // same pair, reused
  r.clear();
  e.on_cancel(rf(1, 7, 1), 4, r);
  // Side::Sell proves the cancel found the new order, not the dead buy.
  if (auto d = diff(r.ev, {out::cancel_ack(4, rf(1, 7, 1), 40, Side::Sell)}); !d.empty()) return d;
  return expect_book(e, 0, 0, 0, 0);
}

// ---------------------------------------------------------------- S: self-trade prevention

// S1: STP keys on participant_id (firm). Different sessions of the same firm
// still self-trade — session_id is irrelevant to STP.
//
// The aggressor here is a GTC order, so after the STP cancellation its
// untouched quantity rests silently (A4). There is no Expired: that is an
// IOC/market ending, not a GTC one.
std::string t_stp_across_sessions_same_firm(IMatchingEngine& e) {
  Rec r;
  e.on_new(ord(1, Side::Sell, 10000, 100, 1, 1, 5), r);  // firm 5, session 1
  r.clear();
  e.on_new(ord(2, Side::Buy, 10000, 100, 2, 2, 5), r);  // firm 5, session 2
  if (auto d = diff(r.ev, {out::ack(2, rf(2, 2, 5), 10000, Side::Buy),
                           out::stp_cancel_ack(2, rf(1, 1, 5), rf(2, 2, 5), 100, Side::Buy)});
      !d.empty()) {
    return d;
  }
  // The resting sell is gone and the aggressor's full 100 now rests as a bid.
  return expect_book(e, 1, 0, 100, 1);
}

// S2/S3: the RESTING order is cancelled, the aggressor's quantity is unchanged
// and it keeps matching past the cancelled order.
std::string t_stp_cancels_resting_aggressor_continues(IMatchingEngine& e) {
  Rec r;
  e.on_new(ord(1, Side::Sell, 10000, 40, 1, 1, 5), r);  // own (firm 5)
  e.on_new(ord(2, Side::Sell, 10000, 60, 2, 2, 6), r);  // other firm
  r.clear();
  e.on_new(ord(3, Side::Buy, 10000, 60, 3, 3, 5), r);  // firm 5 aggresses
  if (auto d = diff(r.ev, {out::ack(3, rf(3, 3, 5), 10000, Side::Buy),
                           out::stp_cancel_ack(3, rf(1, 1, 5), rf(3, 3, 5), 40, Side::Buy),
                           out::trade(3, rf(2, 2, 6), rf(3, 3, 5), 10000, 60, Side::Buy)});
      !d.empty()) {
    return d;
  }
  return expect_book(e, 0, 0, 0, 0);  // full 60 filled: STP did not reduce the aggressor
}

// S5: emission is book-walk order — the STP CancelAck appears exactly where the
// resting order sat, interleaved between trades.
std::string t_stp_emission_order_interleaved(IMatchingEngine& e) {
  Rec r;
  e.on_new(ord(1, Side::Sell, 10000, 30, 1, 1, 6), r);  // other firm  (B)
  e.on_new(ord(2, Side::Sell, 10000, 30, 2, 2, 5), r);  // own         (STP)
  e.on_new(ord(3, Side::Sell, 10000, 30, 3, 3, 7), r);  // other firm  (C)
  r.clear();
  e.on_new(ord(4, Side::Buy, 10000, 60, 4, 4, 5), r);
  return diff(r.ev, {out::ack(4, rf(4, 4, 5), 10000, Side::Buy),
                     out::trade(4, rf(1, 1, 6), rf(4, 4, 5), 10000, 30, Side::Buy),
                     out::stp_cancel_ack(4, rf(2, 2, 5), rf(4, 4, 5), 30, Side::Buy),
                     out::trade(4, rf(3, 3, 7), rf(4, 4, 5), 10000, 30, Side::Buy)});
}

// §1.4 Cancel: an order removed by STP is gone; cancelling it later is an
// unknown-order reject like any other dead order.
std::string t_cancel_after_stp_removal(IMatchingEngine& e) {
  Rec r;
  e.on_new(ord(1, Side::Sell, 10000, 40, 1, 1, 5), r);
  e.on_new(ord(2, Side::Buy, 10000, 40, 2, 2, 5), r);  // STP removes the resting order
  r.clear();
  e.on_cancel(rf(1, 1, 5), 3, r);
  return diff(r.ev, {out::reject(3, rf(1, 1, 5), RejectReason::UnknownOrder, Side::Buy)});
}

// I1: an IOC that met only same-firm liquidity expires its FULL quantity —
// the STP cancellation did not fill anything.
std::string t_ioc_stp_expires_full_qty(IMatchingEngine& e) {
  Rec r;
  e.on_new(ord(1, Side::Sell, 10000, 50, 1, 1, 5), r);
  r.clear();
  e.on_new(ord(2, Side::Buy, 10000, 100, 2, 2, 5, TIF::IOC), r);
  return diff(r.ev, {out::ack(2, rf(2, 2, 5), 10000, Side::Buy),
                     out::stp_cancel_ack(2, rf(1, 1, 5), rf(2, 2, 5), 50, Side::Buy),
                     out::expired(2, rf(2, 2, 5), 100, Side::Buy)});
}

// S2: the STP CancelAck carries the resting order's REMAINING quantity, not
// its original quantity — the same remainder rule a plain cancel obeys. Every
// other STP case above uses an untouched resting order, so an original-qty bug
// would pass them all.
std::string t_stp_cancelack_carries_remaining(IMatchingEngine& e) {
  Rec r;
  e.on_new(ord(1, Side::Sell, 10000, 100, 1, 1, 5), r);  // firm 5 rests 100
  e.on_new(ord(2, Side::Buy, 10000, 30, 2, 2, 6), r);    // other firm fills 30
  r.clear();
  e.on_new(ord(3, Side::Buy, 10000, 10, 3, 3, 5), r);  // firm 5 aggresses
  if (auto d = diff(r.ev, {out::ack(3, rf(3, 3, 5), 10000, Side::Buy),
                           out::stp_cancel_ack(3, rf(1, 1, 5), rf(3, 3, 5), 70, Side::Buy)});
      !d.empty()) {
    return d;
  }
  return expect_book(e, 1, 0, 10, 1);  // aggressor's untouched 10 rests as a bid
}

// A2/I1 end-to-end: one IOC event producing the full canonical sequence —
// Ack, trade, interleaved STP CancelAck, trade at the next level, then exactly
// one Expired for the remainder.
std::string t_ioc_full_sequence_trades_stp_expired(IMatchingEngine& e) {
  Rec r;
  e.on_new(ord(1, Side::Sell, 10000, 30, 1, 1, 6), r);  // other firm
  e.on_new(ord(2, Side::Sell, 10000, 20, 2, 2, 5), r);  // own — STP target
  e.on_new(ord(3, Side::Sell, 10001, 25, 3, 3, 7), r);  // other firm, next level
  r.clear();
  e.on_new(ord(4, Side::Buy, 10001, 100, 4, 4, 5, TIF::IOC), r);
  return diff(r.ev, {out::ack(4, rf(4, 4, 5), 10001, Side::Buy),
                     out::trade(4, rf(1, 1, 6), rf(4, 4, 5), 10000, 30, Side::Buy),
                     out::stp_cancel_ack(4, rf(2, 2, 5), rf(4, 4, 5), 20, Side::Buy),
                     out::trade(4, rf(3, 3, 7), rf(4, 4, 5), 10001, 25, Side::Buy),
                     out::expired(4, rf(4, 4, 5), 45, Side::Buy)});
}

// ---------------------------------------------------------------- F: fill-or-kill

// F4: a fillable FOK is Ack + trades, never an Expired.
std::string t_fok_fillable(IMatchingEngine& e) {
  Rec r;
  e.on_new(ord(1, Side::Sell, 10000, 60, 1, 1, 6), r);
  e.on_new(ord(2, Side::Sell, 10000, 40, 2, 2, 7), r);
  r.clear();
  e.on_new(ord(3, Side::Buy, 10000, 100, 3, 3, 5, TIF::FOK), r);
  return diff(r.ev, {out::ack(3, rf(3, 3, 5), 10000, Side::Buy),
                     out::trade(3, rf(1, 1, 6), rf(3, 3, 5), 10000, 60, Side::Buy),
                     out::trade(3, rf(2, 2, 7), rf(3, 3, 5), 10000, 40, Side::Buy)});
}

// F2/A3: unfillable — exactly one Reject, no Ack, and the book is untouched.
std::string t_fok_unfillable_rejects_without_ack(IMatchingEngine& e) {
  Rec r;
  e.on_new(ord(1, Side::Sell, 10000, 60, 1, 1, 6), r);
  r.clear();
  e.on_new(ord(2, Side::Buy, 10000, 100, 2, 2, 5, TIF::FOK), r);
  if (auto d = diff(r.ev, {out::reject(2, rf(2, 2, 5), RejectReason::FokUnfillable, Side::Buy)});
      !d.empty()) {
    return d;
  }
  return expect_book(e, 0, 1, 60, 1);  // the 60 must still be resting, untouched
}

// F1: fillability is bounded by the FOK order's limit price — liquidity above
// it does not count.
std::string t_fok_fillability_bounded_by_limit(IMatchingEngine& e) {
  Rec r;
  e.on_new(ord(1, Side::Sell, 10000, 60, 1, 1, 6), r);
  e.on_new(ord(2, Side::Sell, 10001, 60, 2, 2, 6), r);  // outside the limit
  r.clear();
  e.on_new(ord(3, Side::Buy, 10000, 100, 3, 3, 5, TIF::FOK), r);
  if (auto d = diff(r.ev, {out::reject(3, rf(3, 3, 5), RejectReason::FokUnfillable, Side::Buy)});
      !d.empty()) {
    return d;
  }
  return expect_book(e, 0, 2, 120, 2);
}

// F1, THE worked example from the spec. Firm A sends FOK buy 100; the ask side
// holds 60 from firm B and 50 from firm A.
//
//   naive (count all 110) -> commit -> STP cancels A's 50 -> only 60 fills:
//                            a partially filled FOK, violating F4
//   correct (exclude same firm) -> 60 < 100 -> clean Reject, book untouched
//
// This is the one case that separates F1 from the obvious implementation, and
// the hidden stream contains an injected instance of it.
std::string t_fok_same_firm_exclusion_60_50_100(IMatchingEngine& e) {
  Rec r;
  e.on_new(ord(1, Side::Sell, 10000, 60, 1, 1, 6), r);  // firm B: 60
  e.on_new(ord(2, Side::Sell, 10000, 50, 2, 2, 5), r);  // firm A: 50
  r.clear();
  e.on_new(ord(3, Side::Buy, 10000, 100, 3, 3, 5, TIF::FOK), r);  // firm A wants 100
  if (auto d = diff(r.ev, {out::reject(3, rf(3, 3, 5), RejectReason::FokUnfillable, Side::Buy)});
      !d.empty()) {
    return d;
  }
  return expect_book(e, 0, 1, 110, 2);  // both still resting: no partial effects at all
}

// F3: same-firm liquidity is excluded from the count but still STP-cancelled
// during the commit — and because it was never counted, the commit cannot fall
// short. No rollback path is ever needed.
std::string t_fok_commits_with_stp_interleaved(IMatchingEngine& e) {
  Rec r;
  e.on_new(ord(1, Side::Sell, 10000, 50, 1, 1, 5), r);   // own — excluded from the count
  e.on_new(ord(2, Side::Sell, 10000, 100, 2, 2, 6), r);  // other firm — 100 counted
  r.clear();
  e.on_new(ord(3, Side::Buy, 10000, 100, 3, 3, 5, TIF::FOK), r);
  if (auto d = diff(r.ev, {out::ack(3, rf(3, 3, 5), 10000, Side::Buy),
                           out::stp_cancel_ack(3, rf(1, 1, 5), rf(3, 3, 5), 50, Side::Buy),
                           out::trade(3, rf(2, 2, 6), rf(3, 3, 5), 10000, 100, Side::Buy)});
      !d.empty()) {
    return d;
  }
  return expect_book(e, 0, 0, 0, 0);
}

// F1/F2: fillability counts REMAINING quantity, not original quantity.
std::string t_fok_counts_remaining_not_original(IMatchingEngine& e) {
  Rec r;
  e.on_new(ord(1, Side::Sell, 10000, 100, 1, 1, 6), r);
  e.on_new(ord(2, Side::Buy, 10000, 70, 2, 2, 7), r);  // leaves 30 remaining
  r.clear();
  e.on_new(ord(3, Side::Buy, 10000, 50, 3, 3, 5, TIF::FOK), r);  // 30 < 50
  return diff(r.ev, {out::reject(3, rf(3, 3, 5), RejectReason::FokUnfillable, Side::Buy)});
}

// F4: an exactly-fillable FOK fills completely with no Expired and no leftover.
std::string t_fok_exact_fill(IMatchingEngine& e) {
  Rec r;
  e.on_new(ord(1, Side::Sell, 10000, 100, 1, 1, 6), r);
  r.clear();
  e.on_new(ord(2, Side::Buy, 10000, 100, 2, 2, 5, TIF::FOK), r);
  if (auto d = diff(r.ev, {out::ack(2, rf(2, 2, 5), 10000, Side::Buy),
                           out::trade(2, rf(1, 1, 6), rf(2, 2, 5), 10000, 100, Side::Buy)});
      !d.empty()) {
    return d;
  }
  return expect_book(e, 0, 0, 0, 0);
}

// F2: an unfillable FOK against an empty book is still a Reject, not an Expired.
std::string t_fok_empty_book_rejects(IMatchingEngine& e) {
  Rec r;
  e.on_new(ord(1, Side::Buy, 10000, 100, 1, 1, 5, TIF::FOK), r);
  return diff(r.ev, {out::reject(1, rf(1, 1, 5), RejectReason::FokUnfillable, Side::Buy)});
}

// F1: the fillability walk sweeps multiple levels, all bounded by the limit.
std::string t_fok_multi_level_fill(IMatchingEngine& e) {
  Rec r;
  e.on_new(ord(1, Side::Sell, 10000, 40, 1, 1, 6), r);
  e.on_new(ord(2, Side::Sell, 10001, 40, 2, 2, 6), r);
  e.on_new(ord(3, Side::Sell, 10002, 40, 3, 3, 6), r);
  r.clear();
  e.on_new(ord(4, Side::Buy, 10002, 100, 4, 4, 5, TIF::FOK), r);
  return diff(r.ev, {out::ack(4, rf(4, 4, 5), 10002, Side::Buy),
                     out::trade(4, rf(1, 1, 6), rf(4, 4, 5), 10000, 40, Side::Buy),
                     out::trade(4, rf(2, 2, 6), rf(4, 4, 5), 10001, 40, Side::Buy),
                     out::trade(4, rf(3, 3, 6), rf(4, 4, 5), 10002, 20, Side::Buy)});
}

// ---------------------------------------------------------------- sell-side mirrors

// Everything above is buy-side. The sell side is not a special case, and this
// catches an engine that hardcoded one direction.
std::string t_sell_aggressor_mirrors(IMatchingEngine& e) {
  Rec r;
  e.on_new(ord(1, Side::Buy, 10000, 50, 1, 1, 6), r);
  e.on_new(ord(2, Side::Buy, 9999, 50, 2, 2, 6), r);
  r.clear();
  e.on_new(ord(3, Side::Sell, 9999, 100, 3, 3, 5), r);
  return diff(r.ev, {out::ack(3, rf(3, 3, 5), 9999, Side::Sell),
                     out::trade(3, rf(1, 1, 6), rf(3, 3, 5), 10000, 50, Side::Sell),
                     out::trade(3, rf(2, 2, 6), rf(3, 3, 5), 9999, 50, Side::Sell)});
}

// M2 on the sell side: best bid first means HIGHEST price first.
std::string t_sell_market_sweeps_bids_high_first(IMatchingEngine& e) {
  Rec r;
  e.on_new(ord(1, Side::Buy, 9998, 40, 1, 1, 6), r);
  e.on_new(ord(2, Side::Buy, 10000, 40, 2, 2, 6), r);
  r.clear();
  e.on_new(ord(3, Side::Sell, 0, 60, 3, 3, 5, TIF::Market), r);
  return diff(r.ev, {out::ack(3, rf(3, 3, 5), 0, Side::Sell),
                     out::trade(3, rf(2, 2, 6), rf(3, 3, 5), 10000, 40, Side::Sell),
                     out::trade(3, rf(1, 1, 6), rf(3, 3, 5), 9998, 20, Side::Sell)});
}

// F2/M3 on the sell side: Reject and Expired must carry the order's real side.
// Every other Reject and Expired in this suite is buy-side, so an engine
// hardcoding Side::Buy on those event types would pass them all.
std::string t_sell_side_reject_and_expired(IMatchingEngine& e) {
  Rec r;
  e.on_new(ord(1, Side::Sell, 10000, 50, 1, 1, 5, TIF::FOK), r);  // empty bid side
  if (auto d = diff(r.ev, {out::reject(1, rf(1, 1, 5), RejectReason::FokUnfillable, Side::Sell)});
      !d.empty()) {
    return d;
  }
  r.clear();
  e.on_new(ord(2, Side::Sell, 0, 30, 1, 2, 5, TIF::Market), r);  // still empty
  return diff(r.ev, {out::ack(2, rf(1, 2, 5), 0, Side::Sell),
                     out::expired(2, rf(1, 2, 5), 30, Side::Sell)});
}

// ---------------------------------------------------------------- registry

const Case kCases[] = {
    {"ack_only_for_non_marketable_limit", "A1, A4", t_ack_only},
    {"ack_precedes_trades", "A1", t_ack_before_trades},
    {"trade_price_is_the_resting_price", "§1.1 Core", t_trade_price_is_resting},
    {"price_priority_best_first", "§1.1 Core", t_price_priority},
    {"time_priority_fifo_within_level", "§1.1 Core", t_time_priority_fifo},
    {"partial_fill_resting_order_survives", "§1.1 Core", t_partial_fill_resting_survives},
    {"marketable_limit_rests_remainder_silently", "A4", t_marketable_limit_rests_remainder},
    {"limit_order_respects_its_limit_price", "§1.1 Core", t_limit_price_bound},
    {"book_is_never_left_crossed", "invariants", t_book_never_crossed},
    {"market_order_acks_on_receipt", "M1, M4", t_market_acks},
    {"market_sweeps_at_each_resting_price", "M2", t_market_sweeps_at_resting_prices},
    {"market_into_empty_book_expires", "M3, M4", t_market_into_empty_book},
    {"market_partial_expires_remainder", "M3", t_market_partial_expires_remainder},
    {"market_order_never_rests", "M1", t_market_never_rests},
    {"ioc_with_no_liquidity_expires", "A4", t_ioc_no_liquidity},
    {"ioc_partial_fill_expires_remainder", "A2, A4", t_ioc_partial},
    {"ioc_full_fill_emits_no_expired", "A4", t_ioc_full_fill_no_expired},
    {"cancel_live_order", "§1.4 Cancel", t_cancel_live},
    {"cancel_carries_remaining_not_original", "§1.4 Cancel", t_cancel_partially_filled_carries_remainder},
    {"cancel_unknown_id_rejects", "§1.4 Cancel", t_cancel_unknown},
    {"cancel_already_filled_rejects", "§1.4 Cancel", t_cancel_already_filled},
    {"second_cancel_of_same_order_rejects", "§1.4 Cancel", t_double_cancel},
    {"cancel_identity_is_session_plus_coid", "identity", t_cancel_identity_is_the_pair},
    {"cancelling_last_order_removes_level", "§1.1 Core", t_cancel_last_order_at_level},
    {"levels_are_best_price_first", "engine.h", t_levels_best_first},
    {"stp_keys_on_firm_not_session", "S1", t_stp_across_sessions_same_firm},
    {"stp_cancels_resting_aggressor_continues", "S2, S3",
     t_stp_cancels_resting_aggressor_continues},
    {"stp_cancelack_interleaved_in_walk_order", "S5", t_stp_emission_order_interleaved},
    {"cancel_after_stp_removal_rejects", "§1.4 Cancel", t_cancel_after_stp_removal},
    {"ioc_meeting_only_own_liquidity_expires_full", "I1", t_ioc_stp_expires_full_qty},
    {"fok_fillable_commits", "F3, F4", t_fok_fillable},
    {"fok_unfillable_rejects_with_no_ack", "F2, A3", t_fok_unfillable_rejects_without_ack},
    {"fok_fillability_bounded_by_limit_price", "F1", t_fok_fillability_bounded_by_limit},
    {"fok_excludes_same_firm_liquidity_60_50_100", "F1", t_fok_same_firm_exclusion_60_50_100},
    {"fok_commits_with_stp_cancels_interleaved", "F3", t_fok_commits_with_stp_interleaved},
    {"fok_counts_remaining_not_original_qty", "F1", t_fok_counts_remaining_not_original},
    {"fok_exact_fill", "F4", t_fok_exact_fill},
    {"fok_against_empty_book_rejects", "F2", t_fok_empty_book_rejects},
    {"fok_fills_across_multiple_levels", "F1, F3", t_fok_multi_level_fill},
    {"sell_side_aggressor_mirrors_buy_side", "§1.1 Core", t_sell_aggressor_mirrors},
    {"sell_market_sweeps_bids_highest_first", "M2", t_sell_market_sweeps_bids_high_first},
    {"partial_fill_keeps_queue_head", "§1.1 Core", t_partial_fill_keeps_queue_head},
    {"id_reuse_after_death_targets_new_order", "§2", t_id_reuse_after_death},
    {"stp_cancelack_carries_remaining_qty", "S2", t_stp_cancelack_carries_remaining},
    {"ioc_full_sequence_trade_stp_trade_expired", "A2, I1", t_ioc_full_sequence_trades_stp_expired},
    {"sell_side_reject_and_expired_carry_side", "F2, M3", t_sell_side_reject_and_expired},
};

}  // namespace

std::vector<TestResult> run_all_spec_tests(const EngineFactory& make_engine) {
  std::vector<TestResult> results;
  results.reserve(sizeof(kCases) / sizeof(kCases[0]));
  for (const Case& c : kCases) {
    IMatchingEngine* engine = make_engine();
    std::string detail = c.fn(*engine);
    delete engine;
    results.push_back(TestResult{c.name, c.rule, detail.empty(), std::move(detail)});
  }
  return results;
}

}  // namespace mebench::tests
