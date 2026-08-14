// mebench/out.h — output events and the sink your engine emits into.
//
// FROZEN.

#pragma once

#include <cstdint>

#include "mebench/order.h"

namespace mebench {

enum class OutType : uint8_t { Trade = 0, Ack = 1, Reject = 2, CancelAck = 3, Expired = 4 };

// Every value here is reachable by a spec rule.
//
// There is no SelfTrade reason: STP cancels the resting order and continues
// matching (SPEC S2/S4), so no rule ever rejects on self-trade.
//
// There is no DuplicateId or InvalidQty: the generator never emits a New that
// duplicates a live (session_id, client_order_id) in the same session, and never
// emits a zero quantity (SPEC §2 Input guarantees).
enum class RejectReason : uint8_t { None = 0, UnknownOrder = 1, FokUnfillable = 2 };

// 56 bytes.
//
// in_seq is what makes counterexample shrinking possible: every output points
// back at the input event that caused it.
//
// Field rules (SPEC the field rules above):
//   in_seq          always the seq of the input event being processed
//   maker           the resting order on Trade and on an STP CancelAck; zeroed otherwise
//   taker           the order named by the input event: the aggressor on Trade,
//                   the subject order on Ack / Reject / Expired, and the
//                   cancelled order on an ordinary CancelAck
//   px              Trade: the RESTING order's price. Ack: the order's own price
//                   (0 for a market order). Expired / CancelAck / Reject: 0
//   qty             Trade: quantity traded. Expired: quantity expiring.
//                   CancelAck: the REMAINING quantity. Ack / Reject: 0
//   aggressor_side  side of the order named by the input event. On a Cancel
//                   input that is the side of the resting order being
//                   cancelled; on a Reject/UnknownOrder, where no such order
//                   exists, it is Side::Buy
//   reason          None on everything except Reject
//   _pad            zeroed
//
// "Zeroed" means every byte zero. The digest that verifies a run folds EVERY
// field, so a garbage maker on an Ack is a correctness failure like any other.
struct OutEvent {
  uint64_t in_seq;
  OrderRef maker;
  OrderRef taker;
  int32_t px;
  uint32_t qty;
  OutType type;
  Side aggressor_side;
  RejectReason reason;
  uint8_t _pad[5];
};
static_assert(sizeof(OutEvent) == 56 && alignof(OutEvent) == 8);

class OutSink {
 public:
  virtual void emit(const OutEvent&) noexcept = 0;
  virtual ~OutSink() = default;
};

// Convenience constructors. These are ordinary helpers, not part of the
// measured contract — use them or build OutEvents by hand, whichever you prefer.
namespace out {

inline OutEvent ack(uint64_t in_seq, OrderRef taker, int32_t px, Side side) {
  return OutEvent{in_seq, OrderRef{}, taker,        px, 0, OutType::Ack, side,
                  RejectReason::None, {0, 0, 0, 0, 0}};
}

inline OutEvent trade(uint64_t in_seq, OrderRef maker, OrderRef taker, int32_t px, uint32_t qty,
                      Side aggressor_side) {
  return OutEvent{in_seq, maker, taker,        px, qty, OutType::Trade, aggressor_side,
                  RejectReason::None, {0, 0, 0, 0, 0}};
}

inline OutEvent reject(uint64_t in_seq, OrderRef taker, RejectReason reason, Side side) {
  return OutEvent{in_seq, OrderRef{}, taker, 0, 0, OutType::Reject, side, reason, {0, 0, 0, 0, 0}};
}

// Ordinary cancel: maker is zeroed, taker is the cancelled order, qty is the
// remaining quantity.
inline OutEvent cancel_ack(uint64_t in_seq, OrderRef taker, uint32_t remaining_qty, Side side) {
  return OutEvent{in_seq,
                  OrderRef{},
                  taker,
                  0,
                  remaining_qty,
                  OutType::CancelAck,
                  side,
                  RejectReason::None,
                  {0, 0, 0, 0, 0}};
}

// STP cancel: maker is the removed resting order, taker is the aggressor that
// triggered it, side is the aggressor's side (SPEC S2/S5).
inline OutEvent stp_cancel_ack(uint64_t in_seq, OrderRef maker, OrderRef taker,
                               uint32_t remaining_qty, Side aggressor_side) {
  return OutEvent{in_seq,
                  maker,
                  taker,
                  0,
                  remaining_qty,
                  OutType::CancelAck,
                  aggressor_side,
                  RejectReason::None,
                  {0, 0, 0, 0, 0}};
}

inline OutEvent expired(uint64_t in_seq, OrderRef taker, uint32_t remaining_qty, Side side) {
  return OutEvent{in_seq,
                  OrderRef{},
                  taker,
                  0,
                  remaining_qty,
                  OutType::Expired,
                  side,
                  RejectReason::None,
                  {0, 0, 0, 0, 0}};
}

}  // namespace out
}  // namespace mebench
