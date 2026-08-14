// reference/reference.cpp — the oracle.
//
// Every branch here should map onto a numbered rule in SPEC.md. Where it does
// not, either the spec is missing a rule or this file has a bug; those are the
// only two options.

#include "reference/reference.h"

#include <cassert>
#include <cstring>

namespace mebench::reference {

// ---------------------------------------------------------------- FOK check

// SPEC F1. Read-only. Sums only resting quantity whose participant_id DIFFERS
// from the aggressor's, best price first, bounded by the aggressor's limit
// price. Excluding same-firm liquidity is what makes "check then commit" atomic
// without a rollback path (F3): STP cancellations during the commit can never
// cause a shortfall, because their quantity was never counted.
template <class Book>
uint32_t ReferenceEngine::fillable_qty(const Book& book, const Order& o) const {
  uint64_t sum = 0;
  for (const auto& [px, level] : book) {
    if (!acceptable(o.side, o.tif, o.px, px)) break;
    for (const auto& r : level) {
      if (r.o.participant_id == o.participant_id) continue;  // F1 exclusion
      sum += r.remaining;
      if (sum >= o.qty) return o.qty;  // enough is enough; stop walking
    }
  }
  return static_cast<uint32_t>(sum);
}

// ---------------------------------------------------------------- matching

// Walks the opposite book best price first, FIFO within each level (SPEC §1.1 Core),
// emitting Trades and STP CancelAcks in book-walk order (S5).
template <class Book>
void ReferenceEngine::match(Book& book, const Order& o, uint32_t& remaining,
                            OutSink& sink) noexcept {
  auto lit = book.begin();
  while (remaining > 0 && lit != book.end()) {
    const int32_t level_px = lit->first;
    if (!acceptable(o.side, o.tif, o.px, level_px)) break;  // no better prices behind us

    Level& level = lit->second;
    auto oit = level.begin();
    while (remaining > 0 && oit != level.end()) {
      Resting& r = *oit;

      // SPEC S1/S2: same firm — remove the RESTING order, CancelAck its
      // remaining quantity, and continue matching with the aggressor's quantity
      // unchanged. No trade between the pair, no reject, no reduction (S3).
      if (r.o.participant_id == o.participant_id) {
        sink.emit(out::stp_cancel_ack(o.seq, r.o.ref(), o.ref(), r.remaining, o.side));
        resting_qty_total_ -= r.remaining;
        --resting_order_count_;
        index_.erase(key_of(r.o.ref()));
        oit = level.erase(oit);
        continue;
      }

      // SPEC §1.1 Core: trade price is the RESTING order's price, always.
      const uint32_t q = remaining < r.remaining ? remaining : r.remaining;
      sink.emit(out::trade(o.seq, r.o.ref(), o.ref(), r.o.px, q, o.side));
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
      lit = book.erase(lit);  // level cleanup
    } else {
      ++lit;
    }
  }
}

template <class Book>
void ReferenceEngine::rest(Book& book, const Order& o, uint32_t remaining) noexcept {
  Level& level = book[o.px];
  level.push_back(Resting{o, remaining});
  auto it = std::prev(level.end());
  index_[key_of(o.ref())] = Locator{o.side, o.px, it};
  resting_qty_total_ += remaining;
  ++resting_order_count_;
}

template <class Book>
void ReferenceEngine::erase_at(Book& book, int32_t px, Level::iterator it) noexcept {
  auto lit = book.find(px);
  assert(lit != book.end());
  lit->second.erase(it);
  if (lit->second.empty()) book.erase(lit);
}

// ---------------------------------------------------------------- on_new

void ReferenceEngine::on_new(const Order& o, OutSink& sink) noexcept {
  last_seq_ = o.seq;
  uint32_t remaining = o.qty;

  // SPEC F2: the fillability check happens BEFORE any mutation and before the
  // Ack, because an unfillable FOK emits only the Reject (A3) — no Ack at all.
  if (o.tif == TIF::FOK) {
    const uint32_t available =
        o.side == Side::Buy ? fillable_qty(asks_, o) : fillable_qty(bids_, o);
    if (available < o.qty) {
      sink.emit(out::reject(o.seq, o.ref(), RejectReason::FokUnfillable, o.side));
      return;  // book untouched: no trades, no STP cancellations, no Ack
    }
  }

  // SPEC A1: exactly one Ack, before any other output for this event.
  sink.emit(out::ack(o.seq, o.ref(), o.px, o.side));

  if (o.side == Side::Buy) {
    match(asks_, o, remaining, sink);
  } else {
    match(bids_, o, remaining, sink);
  }

  if (remaining == 0) return;

  switch (o.tif) {
    case TIF::Day:
      // Marketable-and-partial or plain non-marketable: the remainder rests
      // silently. The Ack already covered acceptance (SPEC A4).
      if (o.side == Side::Buy) {
        rest(bids_, o, remaining);
      } else {
        rest(asks_, o, remaining);
      }
      break;

    case TIF::IOC:
    case TIF::Market:
      // SPEC M3 / I1: exactly one Expired carrying the remainder — including
      // quantity that met only same-firm liquidity, now cancelled.
      sink.emit(out::expired(o.seq, o.ref(), remaining, o.side));
      break;

    case TIF::FOK:
      // Unreachable by F3: we counted at least o.qty of other-firm liquidity
      // above, and STP cancellations cannot eat into it. A remainder here means
      // the fillability walk and the commit walk disagree — a reference bug.
      assert(false && "FOK committed but did not fully fill");
      break;
  }
}

// ---------------------------------------------------------------- on_cancel

void ReferenceEngine::on_cancel(OrderRef ref, uint64_t seq, OutSink& sink) noexcept {
  last_seq_ = seq;

  auto it = index_.find(key_of(ref));
  if (it == index_.end()) {
    // SPEC §1.4 Cancel: unknown or already-filled — the engine cannot tell the two
    // apart without a graveyard, and is not asked to.
    sink.emit(out::reject(seq, ref, RejectReason::UnknownOrder, Side::Buy));
    return;
  }

  const Locator loc = it->second;
  const uint32_t remaining = loc.it->remaining;  // the REMAINDER, not the original qty
  const OrderRef resting_ref = loc.it->o.ref();
  const Side side = loc.it->o.side;

  index_.erase(it);
  if (side == Side::Buy) {
    erase_at(bids_, loc.px, loc.it);
  } else {
    erase_at(asks_, loc.px, loc.it);
  }
  resting_qty_total_ -= remaining;
  --resting_order_count_;

  sink.emit(out::cancel_ack(seq, resting_ref, remaining, side));
}

// ---------------------------------------------------------------- snapshot

template <class Book>
void ReferenceEngine::fill_side(const Book& book, LevelSnapshot* dst, uint32_t& n) const {
  n = 0;
  for (const auto& [px, level] : book) {
    if (n == kSnapshotLevels) break;
    uint64_t total = 0;
    for (const auto& r : level) total += r.remaining;
    dst[n++] = LevelSnapshot{px, total, static_cast<uint32_t>(level.size()),
                             level.empty() ? 0 : level.front().o.seq};
  }
}

void ReferenceEngine::snapshot(BookSnapshot& snap) const {
  std::memset(&snap, 0, sizeof(snap));
  snap.at_seq = last_seq_;
  fill_side(bids_, snap.bids, snap.n_bids);
  fill_side(asks_, snap.asks, snap.n_asks);
  // Whole-book totals, not just the levels written above.
  snap.resting_qty_total = resting_qty_total_;
  snap.resting_order_count = resting_order_count_;
}

IMatchingEngine* make_reference_engine() { return new ReferenceEngine(); }

}  // namespace mebench::reference
