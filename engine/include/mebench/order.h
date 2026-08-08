// mebench/order.h — hot-path structs.
//
// FROZEN. Packed structs are for files; aligned structs are for hot loops.
// Do not reuse one for both.

#pragma once

#include <cstdint>

#include "mebench/wire.h"

namespace mebench {

// 16 bytes, pass by value.
//
// Identity for cancel is the (session_id, client_order_id) PAIR. client_order_id
// is unique only within a session; two sessions may use the same value
// concurrently, and the stream deliberately contains that case.
//
// participant_id is the firm, and is carried on every OrderRef so that
// self-trade prevention is checkable directly from output.
struct OrderRef {
  uint64_t client_order_id;
  uint16_t session_id;
  uint16_t participant_id;
  uint32_t _pad;

  bool operator==(const OrderRef&) const = default;
};
static_assert(sizeof(OrderRef) == 16 && alignof(OrderRef) == 8);

// 32 bytes, naturally aligned.
struct Order {
  uint64_t seq;  // global sequence — use for time priority
  uint64_t client_order_id;
  int32_t px;  // integer ticks; 0 for market orders
  uint32_t qty;
  uint16_t session_id;
  uint16_t participant_id;
  Side side;
  TIF tif;
  uint16_t _pad;

  OrderRef ref() const { return {client_order_id, session_id, participant_id, 0}; }
};
static_assert(sizeof(Order) == 32 && alignof(Order) == 8);

}  // namespace mebench
