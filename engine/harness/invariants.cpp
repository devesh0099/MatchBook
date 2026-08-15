#include "harness/invariants.h"

#include <algorithm>
#include <sstream>

namespace mebench::harness {
namespace {

std::string ref_str(const OrderRef& r) {
  std::ostringstream s;
  s << "(s" << r.session_id << ",c" << r.client_order_id << ",f" << r.participant_id << ")";
  return s.str();
}

bool fail(std::string& err, const std::string& msg) {
  err = msg;
  return false;
}

}  // namespace

bool InvariantChecker::on_event(const DecodedEvent& d, const std::vector<OutEvent>& outs,
                                std::string& err) {
  for (const auto& e : outs) {
    if (e.in_seq != d.o.seq) {
      std::ostringstream s;
      s << "output carries in_seq=" << e.in_seq << " while processing input seq " << d.o.seq;
      return fail(err, s.str());
    }
  }
  return d.type == EvType::New ? check_new(d, outs, err) : check_cancel(d, outs, err);
}

bool InvariantChecker::check_new(const DecodedEvent& d, const std::vector<OutEvent>& outs,
                                 std::string& err) {
  const Order& o = d.o;
  uint64_t traded = 0;
  uint64_t expired = 0;
  bool acked = false;
  bool rejected = false;

  for (const auto& e : outs) {
    switch (e.type) {
      case OutType::Ack:
        if (acked) return fail(err, "more than one Ack for one New");
        if (rejected) return fail(err, "Ack emitted after a Reject (SPEC A3)");
        if (!outs.empty() && &e != &outs.front()) {
          return fail(err, "the Ack must be the first output for a New (SPEC A1)");
        }
        acked = true;
        break;

      case OutType::Reject:
        rejected = true;
        if (acked) return fail(err, "a rejected order must not also be acked (SPEC A3)");
        break;

      case OutType::Trade: {
        // No self-trade, ever. Checkable straight from output, which is why
        // OrderRef carries the firm.
        if (e.maker.participant_id == e.taker.participant_id) {
          return fail(err, "self-trade: maker and taker are both firm " +
                               std::to_string(e.maker.participant_id) + " " + ref_str(e.maker));
        }
        auto it = live_.find(key_of(e.maker));
        if (it == live_.end()) {
          return fail(err, "trade against a maker " + ref_str(e.maker) +
                               " that is not resting according to this engine's own output");
        }
        Shadow& m = it->second;
        // SPEC section 3.1: the trade price is the resting order's price.
        if (e.price != m.price) {
          std::ostringstream s;
          s << "trade price " << e.price << " is not the resting order's price " << m.price << " "
            << ref_str(e.maker);
          return fail(err, s.str());
        }
        // And it must lie within the aggressor's limit.
        if (o.tif != TIF::Market) {
          const bool within = (o.side == Side::Buy) ? (e.price <= o.price) : (e.price >= o.price);
          if (!within) {
            std::ostringstream s;
            s << "trade price " << e.price << " is outside the aggressor's limit " << o.price;
            return fail(err, s.str());
          }
        }
        if (e.qty == 0) return fail(err, "zero-quantity trade");
        if (e.qty > m.remaining) {
          std::ostringstream s;
          s << "trade of " << e.qty << " exceeds the maker's remaining " << m.remaining << " "
            << ref_str(e.maker);
          return fail(err, s.str());
        }
        if (m.side == o.side) return fail(err, "trade between two orders on the same side");

        m.remaining -= e.qty;
        resting_qty_ -= e.qty;
        traded += e.qty;
        if (m.remaining == 0) live_.erase(it);
        break;
      }

      case OutType::CancelAck: {
        // On a New, the only cancel that can appear is an STP cancellation of a
        // resting same-firm order (SPEC S2/S5).
        if (e.maker.session_id == 0) {
          return fail(err, "a New emitted an ordinary CancelAck; only STP cancels are possible");
        }
        if (e.maker.participant_id != o.participant_id) {
          return fail(err, "STP cancelled a resting order belonging to firm " +
                               std::to_string(e.maker.participant_id) + ", not the aggressor's " +
                               std::to_string(o.participant_id) + " (SPEC S1)");
        }
        auto it = live_.find(key_of(e.maker));
        if (it == live_.end()) {
          return fail(err, "STP cancelled " + ref_str(e.maker) + " which is not resting");
        }
        if (e.qty != it->second.remaining) {
          std::ostringstream s;
          s << "STP CancelAck carries " << e.qty << " but the resting remainder is "
            << it->second.remaining << " (SPEC S2)";
          return fail(err, s.str());
        }
        resting_qty_ -= it->second.remaining;
        live_.erase(it);
        break;
      }

      case OutType::Expired:
        if (expired) return fail(err, "more than one Expired for one New");
        expired = e.qty;
        break;
    }
  }

  if (rejected) {
    if (!outs.empty() && outs.size() != 1) {
      return fail(err, "a rejected order must emit only the Reject (SPEC A3/F2)");
    }
    return true;  // book untouched
  }
  if (!acked) return fail(err, "an accepted New must emit exactly one Ack (SPEC A1)");

  const uint64_t rested = static_cast<uint64_t>(o.qty) - traded - expired;
  if (traded + expired > o.qty) {
    std::ostringstream s;
    s << "order filled " << traded << " and expired " << expired << ", more than its quantity "
      << o.qty;
    return fail(err, s.str());
  }

  switch (o.tif) {
    case TIF::GTC:
      if (expired) return fail(err, "a GTC order must never emit Expired");
      break;
    case TIF::IOC:
    case TIF::Market:
      // Every unit must be accounted for: filled or expired, never rested.
      if (rested != 0) {
        std::ostringstream s;
        s << (o.tif == TIF::Market ? "a market" : "an IOC") << " order left " << rested
          << " unaccounted for; it must expire the remainder (SPEC M3/I1)";
        return fail(err, s.str());
      }
      break;
    case TIF::FOK:
      if (expired) return fail(err, "a FOK order must never emit Expired (SPEC F4)");
      if (traded != o.qty) {
        std::ostringstream s;
        s << "an accepted FOK filled " << traded << " of " << o.qty
          << "; FOK fills entirely or rejects (SPEC F4)";
        return fail(err, s.str());
      }
      break;
  }

  if (o.tif == TIF::GTC && rested > 0) {
    const Key k = key_of(o.ref());
    if (live_.count(k)) {
      return fail(err, "two live orders share " + ref_str(o.ref()));
    }
    live_[k] = Shadow{o.price, o.seq, static_cast<uint32_t>(rested), o.side, o.participant_id};
    resting_qty_ += rested;
  }
  return true;
}

bool InvariantChecker::check_cancel(const DecodedEvent& d, const std::vector<OutEvent>& outs,
                                    std::string& err) {
  const OrderRef ref = d.o.ref();
  const Key k = key_of(ref);
  auto it = live_.find(k);

  if (outs.size() != 1) {
    std::ostringstream s;
    s << "a Cancel must emit exactly one output, got " << outs.size();
    return fail(err, s.str());
  }
  const OutEvent& e = outs[0];

  if (e.type == OutType::Reject) {
    if (e.reason != RejectReason::UnknownOrder) {
      return fail(err, "a Cancel can only be rejected with UnknownOrder");
    }
    if (it != live_.end()) {
      return fail(err, "cancel of " + ref_str(ref) +
                           " was rejected as unknown, but it is resting (SPEC section 3.4)");
    }
    return true;
  }

  if (e.type != OutType::CancelAck) {
    return fail(err, "a Cancel must emit CancelAck or Reject");
  }
  if (it == live_.end()) {
    return fail(err, "cancel of " + ref_str(ref) + " was acked, but no such order is resting");
  }
  if (e.qty != it->second.remaining) {
    std::ostringstream s;
    s << "CancelAck carries " << e.qty << " but the order's remainder is " << it->second.remaining
      << " (SPEC section 3.4: the remainder, not the original quantity)";
    return fail(err, s.str());
  }
  resting_qty_ -= it->second.remaining;
  live_.erase(it);
  return true;
}

bool InvariantChecker::on_snapshot(const BookSnapshot& snap, std::string& err) {
  if (snap.n_bids != snap.bids.size() || snap.n_asks != snap.asks.size()) {
    return fail(err, "snapshot's n_bids / n_asks disagree with the level vectors they count");
  }

  // The engine's own totals must match the book implied by its output.
  if (snap.resting_order_count != live_.size()) {
    std::ostringstream s;
    s << "snapshot says " << snap.resting_order_count << " resting orders, but this engine's own "
      << "output accounts for " << live_.size()
      << " — a resting order was dropped or invented silently";
    return fail(err, s.str());
  }
  if (snap.resting_qty_total != resting_qty_) {
    std::ostringstream s;
    s << "snapshot says " << snap.resting_qty_total << " resting quantity, output accounts for "
      << resting_qty_;
    return fail(err, s.str());
  }

  // Rebuild the shadow book as per-level quantity aggregates so the snapshot
  // can be compared level for level rather than by summary.
  std::map<int32_t, uint64_t> bid_want, ask_want;
  for (const auto& [k, sh] : live_) {
    (void)k;
    auto& levels = (sh.side == Side::Buy) ? bid_want : ask_want;
    levels[sh.price] += sh.remaining;
  }

  if (!bid_want.empty() && !ask_want.empty()) {
    const int32_t best_bid = bid_want.rbegin()->first;
    const int32_t best_ask = ask_want.begin()->first;
    if (best_bid >= best_ask) {
      std::ostringstream s;
      s << "book is crossed: best bid " << best_bid << " >= best ask " << best_ask;
      return fail(err, s.str());
    }
  }

  // The old map-of-levels snapshot made "best first, no duplicates" structural;
  // a vector makes wrong ordering a submittable bug, so it is checked here.
  auto check_side = [&](const LevelVec& got, bool bids,
                        const std::map<int32_t, uint64_t>& want) -> bool {
    const char* what = bids ? "bid" : "ask";
    if (got.size() != want.size()) {
      std::ostringstream s;
      s << "snapshot reports " << got.size() << " " << what << " levels; this engine's own output "
        << "accounts for " << want.size();
      return fail(err, s.str());
    }
    for (size_t i = 0; i < got.size(); ++i) {
      const auto [price, qty] = got[i];
      if (i > 0) {
        const int32_t prev = got[i - 1].first;
        const bool ordered = bids ? price < prev : price > prev;
        if (!ordered) {
          std::ostringstream s;
          s << what << " levels are not best-price-first: depth " << i - 1 << " is " << prev
            << ", depth " << i << " is " << price
            << (price == prev ? " (duplicate level)" : "");
          return fail(err, s.str());
        }
      }
      if (qty == 0) {
        return fail(err, std::string("snapshot contains an empty ") + what + " level at price " +
                             std::to_string(price));
      }
      auto e = want.find(price);
      if (e == want.end()) {
        return fail(err, std::string("snapshot reports a ") + what + " level at " +
                             std::to_string(price) +
                             " with no orders behind it in this engine's own output");
      }
      if (qty != e->second) {
        std::ostringstream s;
        s << what << " level " << price << ": snapshot says total_qty " << qty
          << " but this engine's own output accounts for " << e->second;
        return fail(err, s.str());
      }
    }
    return true;
  };

  if (!check_side(snap.bids, true, bid_want)) return false;
  if (!check_side(snap.asks, false, ask_want)) return false;

  return true;
}

}  // namespace mebench::harness
