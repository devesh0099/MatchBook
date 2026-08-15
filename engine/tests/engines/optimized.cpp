// tests/engines/optimized.cpp — a cache-conscious engine.
//
// NOT shipped to participants. It exists to answer one question the platform
// cannot answer about itself: does the ranked profile actually separate a good
// data structure from a naive one? A gate that has only ever measured the
// reference knows nothing about its own discriminating power.
//
// The three design choices, each aimed at the thing the reference pays for:
//
//   1. Orders live in one contiguous pool with a free list. std::list allocates
//      a node per order, scattered across the heap; every walk is a pointer
//      chase into a different cache line.
//   2. Levels are a flat array indexed by price, holding intrusive list heads
//      as pool indices. std::map<Price, ...> is a red-black tree: ~12 pointer
//      hops to find a level, each one a potential miss.
//   3. The cancel index is open-addressed with linear probing. std::map for the
//      same job is another ~12-hop tree walk, and the cancel path is the most
//      common event in the ranked profile.
//
// Indices, not pointers, throughout: 32-bit indices halve the footprint of
// every link and keep more of the book in cache.

#include <cstdint>
#include <cstring>
#include <vector>

#include "mebench/engine.h"
#include "mebench/order.h"
#include "mebench/out.h"

using namespace mebench;

namespace {

constexpr uint32_t kNil = 0xFFFFFFFFu;

// Prices are integer ticks in a bounded band (the generator walks mid inside
// [9950, 10050] and injects at 20000+). A flat array over the whole plausible
// range costs a few hundred KB of heads and turns level lookup into one index.
constexpr int32_t kMinPx = 0;
constexpr int32_t kMaxPx = 65535;
constexpr uint32_t kPxSlots = static_cast<uint32_t>(kMaxPx - kMinPx + 1);

struct PoolOrder {
  uint64_t seq;
  uint64_t coid;
  int32_t price;
  uint32_t remaining;
  uint16_t session;
  uint16_t firm;
  uint32_t next;  // intrusive level list, pool indices
  uint32_t prev;
  Side side;
  bool live;
};

struct Level {
  uint32_t head = kNil;
  uint32_t tail = kNil;
  uint32_t count = 0;
  uint64_t total_qty = 0;
};

/// Open-addressed (session, coid) -> pool index, linear probing.
///
/// The reference does this with a red-black tree. In the ranked profile the
/// successful-cancel path is the most common event, so this lookup is the
/// hottest thing in the engine — one or two cache lines here against a dozen
/// dependent loads there.
class CancelIndex {
 public:
  CancelIndex() { rehash(1u << 16); }

  void insert(uint16_t session, uint64_t coid, uint32_t slot) {
    if ((used_ + 1) * 10 >= cap_ * 7) rehash(cap_ * 2);  // keep load under 0.7
    const uint64_t k = key(session, coid);
    uint32_t i = hash(k) & mask_;
    while (keys_[i] != kEmpty) {
      if (keys_[i] == k) {
        vals_[i] = slot;
        return;
      }
      i = (i + 1) & mask_;
    }
    keys_[i] = k;
    vals_[i] = slot;
    ++used_;
  }

  uint32_t find(uint16_t session, uint64_t coid) const {
    const uint64_t k = key(session, coid);
    uint32_t i = hash(k) & mask_;
    while (keys_[i] != kEmpty) {
      if (keys_[i] == k) return vals_[i];
      i = (i + 1) & mask_;
    }
    return kNil;
  }

  // Backward-shift deletion: tombstones would let probe chains grow without
  // bound across an event stream that is mostly insert/erase churn.
  void erase(uint16_t session, uint64_t coid) {
    const uint64_t k = key(session, coid);
    uint32_t i = hash(k) & mask_;
    while (keys_[i] != kEmpty && keys_[i] != k) i = (i + 1) & mask_;
    if (keys_[i] == kEmpty) return;

    keys_[i] = kEmpty;
    --used_;
    uint32_t j = (i + 1) & mask_;
    while (keys_[j] != kEmpty) {
      const uint32_t home = hash(keys_[j]) & mask_;
      // Is j's home position at or before the hole, walking forward?
      const bool movable = (j > i) ? (home <= i || home > j) : (home <= i && home > j);
      if (movable) {
        keys_[i] = keys_[j];
        vals_[i] = vals_[j];
        keys_[j] = kEmpty;
        i = j;
      }
      j = (j + 1) & mask_;
    }
  }

 private:
  static constexpr uint64_t kEmpty = 0xFFFFFFFFFFFFFFFFull;

  static uint64_t key(uint16_t session, uint64_t coid) {
    return (static_cast<uint64_t>(session) << 48) ^ (coid & 0x0000FFFFFFFFFFFFull);
  }
  static uint32_t hash(uint64_t k) {
    k *= 0x9E3779B97F4A7C15ull;
    return static_cast<uint32_t>(k >> 32);
  }

  void rehash(uint32_t cap) {
    std::vector<uint64_t> ok = std::move(keys_);
    std::vector<uint32_t> ov = std::move(vals_);
    keys_.assign(cap, kEmpty);
    vals_.assign(cap, kNil);
    cap_ = cap;
    mask_ = cap - 1;
    used_ = 0;
    for (size_t i = 0; i < ok.size(); ++i) {
      if (ok[i] == kEmpty) continue;
      uint32_t j = hash(ok[i]) & mask_;
      while (keys_[j] != kEmpty) j = (j + 1) & mask_;
      keys_[j] = ok[i];
      vals_[j] = ov[i];
      ++used_;
    }
  }

  std::vector<uint64_t> keys_;
  std::vector<uint32_t> vals_;
  uint32_t cap_ = 0, mask_ = 0, used_ = 0;
};

class OptimizedEngine final : public IMatchingEngine {
 public:
  OptimizedEngine() {
    bids_.resize(kPxSlots);
    asks_.resize(kPxSlots);
    pool_.reserve(1u << 20);
  }

  void on_new(const Order& o, OutSink& out) noexcept override {
    last_seq_ = o.seq;
    uint32_t remaining = o.qty;

    if (o.tif == TIF::FOK) {
      const uint32_t have = o.side == Side::Buy ? fillable<false>(o) : fillable<true>(o);
      if (have < o.qty) {
        out.emit(out::reject(o.seq, o.ref(), RejectReason::FokUnfillable, o.side));
        return;
      }
    }

    out.emit(out::ack(o.seq, o.ref(), o.price, o.side));

    if (o.side == Side::Buy) {
      match<false>(o, remaining, out);
    } else {
      match<true>(o, remaining, out);
    }

    if (remaining == 0) return;
    switch (o.tif) {
      case TIF::GTC: rest(o, remaining); break;
      case TIF::IOC:
      case TIF::Market: out.emit(out::expired(o.seq, o.ref(), remaining, o.side)); break;
      case TIF::FOK: break;  // F3 guarantees a full fill
    }
  }

  void on_cancel(OrderRef ref, uint64_t seq, OutSink& out) noexcept override {
    last_seq_ = seq;
    const uint32_t slot = index_.find(ref.session_id, ref.client_order_id);
    if (slot == kNil || !pool_[slot].live) {
      out.emit(out::reject(seq, ref, RejectReason::UnknownOrder, Side::Buy));
      return;
    }
    PoolOrder& p = pool_[slot];
    const uint32_t remaining = p.remaining;
    const Side side = p.side;
    const OrderRef r{p.coid, p.session, p.firm, 0};
    unlink(slot);
    out.emit(out::cancel_ack(seq, r, remaining, side));
  }

  // Never timed: scan the price band and count occupied levels.
  void snapshot(BookSnapshot& snap) const override {
    snap = BookSnapshot{};  // not memset: BookSnapshot owns containers
    snap.at_seq = last_seq_;
    uint32_t n_bids = 0, n_asks = 0;
    for (int32_t price = best_bid_; price >= kMinPx; --price) {
      if (bids_[static_cast<uint32_t>(price)].count) ++n_bids;
    }
    for (int32_t price = best_ask_; price <= kMaxPx; ++price) {
      if (asks_[static_cast<uint32_t>(price)].count) ++n_asks;
    }
    snap.n_bids = n_bids;
    snap.n_asks = n_asks;
    snap.resting_qty_total = resting_qty_;
    snap.resting_order_count = resting_count_;
  }

  // Best first: bids scan downward from the touch, asks upward.
  void bid_levels(LevelVec& out) const override {
    out.clear();
    for (int32_t price = best_bid_; price >= kMinPx; --price) {
      const Level& l = bids_[static_cast<uint32_t>(price)];
      if (l.count) out.emplace_back(price, l.total_qty);
    }
  }

  void ask_levels(LevelVec& out) const override {
    out.clear();
    for (int32_t price = best_ask_; price <= kMaxPx; ++price) {
      const Level& l = asks_[static_cast<uint32_t>(price)];
      if (l.count) out.emplace_back(price, l.total_qty);
    }
  }

 private:
  // Sell == true means walk the BID side (a sell aggressor), high price first.
  template <bool Sell>
  uint32_t fillable(const Order& o) const {
    uint64_t sum = 0;
    const auto& book = Sell ? bids_ : asks_;
    int32_t price = Sell ? best_bid_ : best_ask_;
    while (price >= kMinPx && price <= kMaxPx) {
      if (!acceptable<Sell>(o, price)) break;
      const Level& l = book[static_cast<uint32_t>(price)];
      for (uint32_t s = l.head; s != kNil; s = pool_[s].next) {
        if (pool_[s].firm == o.participant_id) continue;
        sum += pool_[s].remaining;
        if (sum >= o.qty) return o.qty;
      }
      price += Sell ? -1 : 1;
    }
    return static_cast<uint32_t>(sum);
  }

  template <bool Sell>
  bool acceptable(const Order& o, int32_t level_px) const {
    if (o.tif == TIF::Market) return true;
    return Sell ? level_px >= o.price : level_px <= o.price;
  }

  template <bool Sell>
  void match(const Order& o, uint32_t& remaining, OutSink& out) noexcept {
    auto& book = Sell ? bids_ : asks_;
    int32_t price = Sell ? best_bid_ : best_ask_;
    while (remaining > 0 && price >= kMinPx && price <= kMaxPx) {
      if (!acceptable<Sell>(o, price)) break;
      Level& l = book[static_cast<uint32_t>(price)];
      uint32_t s = l.head;
      while (remaining > 0 && s != kNil) {
        const uint32_t next = pool_[s].next;
        PoolOrder& p = pool_[s];
        const OrderRef mref{p.coid, p.session, p.firm, 0};

        if (p.firm == o.participant_id) {
          out.emit(out::stp_cancel_ack(o.seq, mref, o.ref(), p.remaining, o.side));
          unlink(s);
          s = next;
          continue;
        }
        const uint32_t q = remaining < p.remaining ? remaining : p.remaining;
        out.emit(out::trade(o.seq, mref, o.ref(), p.price, q, o.side));
        remaining -= q;
        p.remaining -= q;
        l.total_qty -= q;
        resting_qty_ -= q;
        if (p.remaining == 0) {
          unlink(s);
          s = next;
        } else {
          break;  // resting order survived, so the aggressor is exhausted
        }
      }
      if (l.count == 0) price += Sell ? -1 : 1;
      else if (remaining == 0) break;
      else price += Sell ? -1 : 1;
    }
    retouch();
  }

  void rest(const Order& o, uint32_t remaining) noexcept {
    const uint32_t slot = alloc();
    PoolOrder& p = pool_[slot];
    p.seq = o.seq;
    p.coid = o.client_order_id;
    p.price = o.price;
    p.remaining = remaining;
    p.session = o.session_id;
    p.firm = o.participant_id;
    p.side = o.side;
    p.live = true;
    p.next = kNil;

    auto& book = o.side == Side::Buy ? bids_ : asks_;
    Level& l = book[static_cast<uint32_t>(o.price)];
    p.prev = l.tail;
    if (l.tail != kNil) pool_[l.tail].next = slot;
    l.tail = slot;
    if (l.head == kNil) l.head = slot;
    ++l.count;
    l.total_qty += remaining;

    index_.insert(o.session_id, o.client_order_id, slot);
    resting_qty_ += remaining;
    ++resting_count_;

    if (o.side == Side::Buy) {
      if (o.price > best_bid_) best_bid_ = o.price;
    } else {
      if (o.price < best_ask_) best_ask_ = o.price;
    }
  }

  void unlink(uint32_t slot) noexcept {
    PoolOrder& p = pool_[slot];
    auto& book = p.side == Side::Buy ? bids_ : asks_;
    Level& l = book[static_cast<uint32_t>(p.price)];
    if (p.prev != kNil) pool_[p.prev].next = p.next;
    else l.head = p.next;
    if (p.next != kNil) pool_[p.next].prev = p.prev;
    else l.tail = p.prev;
    --l.count;
    l.total_qty -= p.remaining;
    resting_qty_ -= p.remaining;
    --resting_count_;
    p.live = false;
    index_.erase(p.session, p.coid);
    free_.push_back(slot);
  }

  // The touch pointers are hints: they only ever move outward on insert, so a
  // cheap scan pulls them back after a level empties.
  void retouch() noexcept {
    while (best_bid_ >= kMinPx && bids_[static_cast<uint32_t>(best_bid_)].count == 0) --best_bid_;
    while (best_ask_ <= kMaxPx && asks_[static_cast<uint32_t>(best_ask_)].count == 0) ++best_ask_;
  }

  uint32_t alloc() {
    if (!free_.empty()) {
      const uint32_t s = free_.back();
      free_.pop_back();
      return s;
    }
    pool_.push_back(PoolOrder{});
    return static_cast<uint32_t>(pool_.size() - 1);
  }

  std::vector<PoolOrder> pool_;
  std::vector<uint32_t> free_;
  std::vector<Level> bids_, asks_;
  CancelIndex index_;
  int32_t best_bid_ = kMinPx - 1;
  int32_t best_ask_ = kMaxPx + 1;
  uint64_t last_seq_ = 0;
  uint64_t resting_qty_ = 0;
  uint32_t resting_count_ = 0;
};

}  // namespace

extern "C" mebench::IMatchingEngine* create_engine() { return new OptimizedEngine(); }
