// mebench/wire.h — on-disk stream format.
//
// FROZEN. This header is read-only for participants and identical on the server
// and in the boilerplate.
//
// Participant engines never see WireEvent. The harness decodes it into Order
// (order.h) outside the timed region. It is documented here only so the local
// generator and the stream format are inspectable.

#pragma once

#include <cstdint>

// EvType, Side and TIF live in order.h: an engine needs them and never needs
// anything else from this file.
#include "mebench/order.h"

namespace mebench {

// Exactly 24 bytes. Packed: this is a file layout, not a hot-path struct.
struct __attribute__((packed)) WireEvent {
  uint64_t client_order_id;  // unique per session only
  int32_t px;                // integer ticks; 0 for market orders
  uint32_t qty;
  uint16_t session_id;
  uint16_t participant_id;  // firm; several sessions may share one
  EvType type;
  Side side;
  TIF tif;
  uint8_t _pad;
};
static_assert(sizeof(WireEvent) == 24);

// No seq field: the sequence number is the record index, so the two can never
// disagree. No timestamp field: priority is defined on arrival sequence and no
// order type in scope keys on time.
struct __attribute__((packed)) StreamHeader {
  char magic[8];  // "MEBENCH1"
  uint64_t seed;
  uint64_t event_count;
  uint32_t profile_id;
  uint32_t tick_size;
  // The per-session resting-order target this stream was built with, or 0 for
  // the profile's own. Book depth is proportional to it, and so is the untimed
  // warm-up a benchmark needs, so a stream that did not carry it could not say
  // how much of itself is warm-up. Taken from `reserved`, so the layout is
  // unchanged and a stream written before this field reads 0 — which is exactly
  // the "use the profile default" case.
  uint64_t live_target;
  uint64_t reserved[3];
};
static_assert(sizeof(StreamHeader) == 64);

inline constexpr char kStreamMagic[8] = {'M', 'E', 'B', 'E', 'N', 'C', 'H', '1'};

// Profile ids as written into StreamHeader::profile_id.
enum class Profile : uint32_t {
  Balanced = 0,     // correctness runs
  CancelHeavy = 1,  // benchmark runs (realistic)
  SweepHeavy = 2,   // stresses the matching loop
  DeepBook = 3,     // stresses insertion and memory layout
};

}  // namespace mebench
