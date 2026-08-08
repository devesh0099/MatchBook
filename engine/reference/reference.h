// reference/reference.h — the reference matching engine.
//
// Clarity first, speed never. This is the oracle the differential fuzzer
// compares submissions against, the baseline the noise floor is measured on,
// and what the leaderboard bands are calibrated against. It is std::map of
// std::list on purpose: every rule in SPEC.md should be readable straight off
// the page here.

#pragma once

#include <cstdint>
#include <list>
#include <map>

#include "mebench/engine.h"
#include "mebench/order.h"
#include "mebench/out.h"

namespace mebench::reference {

class ReferenceEngine final : public IMatchingEngine {
 public:
  void on_new(const Order& o, OutSink& out) noexcept override;
  void on_cancel(OrderRef ref, uint64_t seq, OutSink& out) noexcept override;
  void snapshot(BookSnapshot& out) const override;

 private:
  struct Resting {
    Order o;
    uint32_t remaining;
  };

  using Level = std::list<Resting>;
  // Bids best-first is highest price first; asks best-first is lowest price
  // first. Each map's own comparator makes begin() the touch.
  using Bids = std::map<int32_t, Level, std::greater<int32_t>>;
  using Asks = std::map<int32_t, Level, std::less<int32_t>>;

  // Cancel identity is the (session_id, client_order_id) PAIR — never the
  // client_order_id alone (SPEC §1). std::pair's ordering gives us that for
  // free.
  using Key = std::pair<uint16_t, uint64_t>;

  struct Locator {
    Side side;
    int32_t px;
    Level::iterator it;
  };

  static Key key_of(const OrderRef& r) { return {r.session_id, r.client_order_id}; }

  // Would an aggressor at aggr_px trade against a level resting at level_px?
  // A market order (SPEC M2) accepts every price.
  static bool acceptable(Side aggressor_side, TIF tif, int32_t aggr_px, int32_t level_px) {
    if (tif == TIF::Market) return true;
    return aggressor_side == Side::Buy ? level_px <= aggr_px : level_px >= aggr_px;
  }

  // SPEC F1: read-only walk, best price first, bounded by the aggressor's limit
  // price, summing ONLY resting quantity from a different firm. Mutates
  // nothing. Stops early once the target is reached.
  template <class Book>
  uint32_t fillable_qty(const Book& book, const Order& o) const;

  // Walks the opposite book consuming `remaining`, emitting Trades and STP
  // CancelAcks in book-walk order (SPEC S5).
  template <class Book>
  void match(Book& book, const Order& o, uint32_t& remaining, OutSink& out) noexcept;

  template <class Book>
  void rest(Book& book, const Order& o, uint32_t remaining) noexcept;

  template <class Book>
  void erase_at(Book& book, int32_t px, Level::iterator it) noexcept;

  template <class Book>
  void fill_side(const Book& book, LevelSnapshot* dst, uint32_t& n) const;

  Bids bids_;
  Asks asks_;
  std::map<Key, Locator> index_;
  uint64_t last_seq_ = 0;
  uint64_t resting_qty_total_ = 0;
  uint32_t resting_order_count_ = 0;
};

// Factory used by the harness for `--engine builtin`. The .so build of the
// reference (used to exercise the dlopen path) wraps this in create_engine().
IMatchingEngine* make_reference_engine();

}  // namespace mebench::reference
