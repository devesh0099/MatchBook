// harness/invariants.h — layer 3 of correctness (SPEC section 4).
//
// These hold regardless of what the oracle says, so they catch bugs the
// reference itself might share. Nothing here consults the reference: the shadow
// book is rebuilt purely from the engine's OWN output stream, then cross-checked
// against the engine's own snapshot. An engine whose output does not describe
// the book it claims to hold fails here even if the reference agrees with it.

#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "common/decode.h"
#include "mebench/engine.h"
#include "mebench/out.h"

namespace mebench::harness {

class InvariantChecker {
 public:
  // Feed the input event and every output the engine produced for it, in order.
  // Returns false and fills `err` on the first violation.
  bool on_event(const DecodedEvent& d, const std::vector<OutEvent>& outs, std::string& err);

  // Cross-check the engine's own view of the book against the one implied by
  // its output so far.
  bool on_snapshot(const BookSnapshot& snap, std::string& err);

  uint64_t resting_orders() const { return live_.size(); }
  uint64_t resting_qty() const { return resting_qty_; }

 private:
  struct Shadow {
    int32_t price;
    uint64_t seq;
    uint32_t remaining;
    Side side;
    uint16_t firm;
  };

  using Key = std::pair<uint16_t, uint64_t>;
  static Key key_of(const OrderRef& r) { return {r.session_id, r.client_order_id}; }

  bool check_new(const DecodedEvent& d, const std::vector<OutEvent>& outs, std::string& err);
  bool check_cancel(const DecodedEvent& d, const std::vector<OutEvent>& outs, std::string& err);

  std::map<Key, Shadow> live_;
  uint64_t resting_qty_ = 0;
};

}  // namespace mebench::harness
