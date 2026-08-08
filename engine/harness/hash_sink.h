// harness/hash_sink.h — the digest that ties the ranked run to the verified one.
//
// Verifying the checksum inside the timed run is what stops anyone winning by
// doing less work (SPEC section 5.2). The correctness run folds this hash over
// its full output and stores the digest; the benchmark run's digest must equal
// it exactly. Same stream, same seed, so any divergence in ANY field fails the
// ranked run.

#pragma once

#include <cstdint>

#include "mebench/out.h"

namespace mebench::harness {

class HashSink final : public OutSink {
 public:
  void emit(const OutEvent& e) noexcept override {
    // FNV-1a folded field by field — never over raw struct bytes, which would
    // make the digest depend on padding contents.
    //
    // Every field is covered on purpose. A partial hash would leave a hole: a
    // submission emitting the wrong OutType, the wrong RejectReason or the
    // wrong taker would produce an identical checksum, and this is the only
    // in-timed-run defence against deleting checks the hidden tests miss.
    auto mix = [&](uint64_t v) { h_ = (h_ ^ v) * 0x100000001b3ull; };
    mix(e.in_seq);
    mix(e.maker.client_order_id);
    mix(static_cast<uint64_t>(e.maker.session_id) << 16 | e.maker.participant_id);
    mix(e.taker.client_order_id);
    mix(static_cast<uint64_t>(e.taker.session_id) << 16 | e.taker.participant_id);
    mix(static_cast<uint64_t>(static_cast<uint32_t>(e.px)) << 32 | e.qty);
    mix(static_cast<uint64_t>(e.type) << 16 | static_cast<uint64_t>(e.aggressor_side) << 8 |
        static_cast<uint64_t>(e.reason));
  }

  uint64_t digest() const noexcept { return h_; }
  void reset() noexcept { h_ = 0xcbf29ce484222325ull; }

 private:
  uint64_t h_ = 0xcbf29ce484222325ull;
};

}  // namespace mebench::harness
