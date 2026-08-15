// common/decode.h — wire format to hot-path structs.
//
// Decoding happens in the LOAD phase, never in the timed region (plan section
// 7). Participants never see WireEvent.

#pragma once

#include <cstdint>

#include "mebench/engine.h"
#include "mebench/order.h"
#include "mebench/wire.h"

namespace mebench {

// 40 bytes: the Order the engine sees, plus which entry point to call. Kept as
// one array rather than parallel arrays so the timed loop streams a single
// cache line sequence. The cost is identical for every participant.
struct DecodedEvent {
  Order o;
  EvType type;
  uint8_t _pad[7];
};
static_assert(sizeof(DecodedEvent) == 40 && alignof(DecodedEvent) == 8);

// The sequence number IS the record index — that is why there is no seq field
// on the wire, and why the two can never disagree.
inline DecodedEvent decode(const WireEvent& w, uint64_t seq) {
  DecodedEvent d{};
  d.o.seq = seq;
  d.o.client_order_id = w.client_order_id;
  d.o.price = w.price;
  d.o.qty = w.qty;
  d.o.session_id = w.session_id;
  d.o.participant_id = w.participant_id;
  d.o.side = w.side;
  d.o.tif = w.tif;
  d.type = w.type;
  return d;
}

inline void dispatch(IMatchingEngine& engine, const DecodedEvent& d, OutSink& out) noexcept {
  if (d.type == EvType::New) {
    engine.on_new(d.o, out);
  } else {
    engine.on_cancel(d.o.ref(), d.o.seq, out);
  }
}

}  // namespace mebench
