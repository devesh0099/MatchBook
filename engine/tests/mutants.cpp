// tests/mutants.cpp — deliberately broken engines.
//
// Pre-event validation (plan section 13) requires proving that a broken engine
// is caught by every layer, not just that a correct one passes. Each mutation
// below is a bug a participant could plausibly ship, and each is aimed at a
// different layer of the gate:
//
//   trade_price     wrong trade price      -> output diff AND invariant layer
//   swallow_expired missing output         -> output diff AND invariant layer
//   self_trade      trades a firm with itself -> invariant layer
//   lying_snapshot  book that output does not support -> snapshot diff
//   hang            unbounded loop         -> wall-clock timeout, reported distinctly
//
// Selected by the MEBENCH_MUTANT environment variable so one shared object
// covers them all. Nothing here is ever shipped to participants.

#include <cstdlib>
#include <cstring>
#include <memory>

#include "mebench/engine.h"
#include "reference/reference.h"

namespace {

enum class Mutation {
  None,
  TradeAtAggressorPrice,
  SwallowExpired,
  SelfTrade,
  LyingSnapshot,
  Hang,
};

Mutation selected() {
  const char* m = std::getenv("MEBENCH_MUTANT");
  if (!m) return Mutation::None;
  if (std::strcmp(m, "trade_price") == 0) return Mutation::TradeAtAggressorPrice;
  if (std::strcmp(m, "swallow_expired") == 0) return Mutation::SwallowExpired;
  if (std::strcmp(m, "self_trade") == 0) return Mutation::SelfTrade;
  if (std::strcmp(m, "lying_snapshot") == 0) return Mutation::LyingSnapshot;
  if (std::strcmp(m, "hang") == 0) return Mutation::Hang;
  return Mutation::None;
}

using namespace mebench;

class FilterSink final : public OutSink {
 public:
  FilterSink(OutSink& inner, Mutation m, int32_t aggressor_px)
      : inner_(inner), m_(m), aggressor_px_(aggressor_px) {}

  void emit(const OutEvent& e) noexcept override {
    OutEvent out = e;
    switch (m_) {
      case Mutation::TradeAtAggressorPrice:
        // The most common first-submission bug: paying the aggressor's price
        // instead of the resting order's.
        if (out.type == OutType::Trade && aggressor_px_ != 0) out.price = aggressor_px_;
        break;
      case Mutation::SwallowExpired:
        if (out.type == OutType::Expired) return;  // silently drop it
        break;
      case Mutation::SelfTrade:
        if (out.type == OutType::Trade) out.maker.participant_id = out.taker.participant_id;
        break;
      default: break;
    }
    inner_.emit(out);
  }

 private:
  OutSink& inner_;
  Mutation m_;
  int32_t aggressor_px_;
};

class MutantEngine final : public IMatchingEngine {
 public:
  explicit MutantEngine(Mutation m) : m_(m), inner_(reference::make_reference_engine()) {}

  void on_new(const Order& o, OutSink& out) noexcept override {
    if (m_ == Mutation::Hang && o.seq >= 20000) {
      // A runaway engine: no output, no progress, no exit.
      static volatile uint64_t spin = 0;
      for (;;) spin = spin + 1;
    }
    FilterSink f(out, m_, o.price);
    inner_->on_new(o, f);
  }

  void on_cancel(OrderRef ref, uint64_t seq, OutSink& out) noexcept override {
    FilterSink f(out, m_, 0);
    inner_->on_cancel(ref, seq, f);
  }

  void snapshot(BookSnapshot& out) const override {
    inner_->snapshot(out);
    if (m_ == Mutation::LyingSnapshot) {
      // Paired with the bid_levels() lie below so the book stays internally
      // consistent. Trades still match the oracle; only the book disagrees.
      out.resting_qty_total += 1;
    }
  }

  void bid_levels(LevelVec& out) const override {
    inner_->bid_levels(out);
    if (m_ == Mutation::LyingSnapshot && !out.empty()) {
      // Claim quantity that the emitted output never put an order on.
      out[0].second += 1;
    }
  }

  void ask_levels(LevelVec& out) const override { inner_->ask_levels(out); }

 private:
  Mutation m_;
  std::unique_ptr<IMatchingEngine> inner_;
};

}  // namespace

extern "C" mebench::IMatchingEngine* create_engine() { return new MutantEngine(selected()); }
