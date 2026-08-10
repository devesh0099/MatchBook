#include "generator/generator.h"

#include <cstdio>
#include <cstring>

#include "common/decode.h"
#include "mebench/out.h"

namespace mebench::generator {
namespace {

// Profiles differ by who is in the room (agent mix), how cancel-heavy they are,
// and the TIF mix takers use. Placement is the product of the profile's
// baseline weights and the agent kind's own bias, below.
constexpr ProfileParams kProfiles[] = {
    // name         sess firms  mm tk ns st  cancel  mkt touch near deep  ioc fok mkt  deep_ticks live
    {"balanced", 24, 10, 6, 6, 6, 6, 45, 25, 35, 30, 10, 12, 8, 6, 42, 150},
    // The ranked benchmark profile. Retuned after measuring what it was
    // actually grading: with 68% of events being cancels there were 2.3 cancel
    // messages per order created, and since an order can only be cancelled
    // once, 93% of them were bound to hit nothing. The median ranked event was
    // therefore a FAILED index lookup — the cheapest operation in the engine —
    // and the contest was ranking how fast you can not find something.
    //
    // "Real books are 90%+ cancels" means 90% of orders END in cancellation
    // rather than execution; it does not mean 90% of messages are cancels. At
    // roughly one cancel per order the successful-cancel path — find, erase,
    // clean up the level — becomes the common case, which is the work the
    // cancel index was chosen to expose.
    //
    // Depth is set to ~750k resting orders (320 sessions x 3780, times the
    // controller's ~0.62 settling ratio). That is the point of the profile.
    //
    // 750k targets a ~54 MB L3 -- an Ice Lake Xeon, which is what this will be
    // measured on. The window where a naive book is RAM-bound and a
    // pool-allocated one is not runs from about 400k to 1M orders there, and
    // 750k sits in the middle of it with margin on both sides: naive ~102 MB
    // (1.9x L3), optimised ~39 MB (0.7x). The margin is deliberate, because a
    // shared instance's EFFECTIVE cache is smaller than its nominal one and
    // varies with whatever the co-tenant is doing.
    //
    // On a 16 MB L3 this is the WRONG number and will read worse than 300k did:
    // at 750k both engines are RAM-bound, so the gap compresses instead of
    // widening. Depth follows the cache it is measured against, and 300k was
    // right for a 16 MB box.
    //
    // The sizing arithmetic: a reference-shaped book costs ~136 B/order and a
    // pool-allocated one ~48 B + 3.1 MB of fixed level array. Against 54 MB
    // that puts the naive engine out of cache above ~400k and the optimised one
    // still inside it below ~1M, which is the window.
    //
    // Measured on a Zen 4 box with a 16 MB L3, reference against the
    // pool/flat-array engine in tests/engines/optimized.cpp, 6M timed events —
    // the evidence that depth does anything at all, since at 23k it did not:
    //
    //     depth    reference   optimized   ratio
    //      23k       130 ns       40 ns    3.25x   <- both fit in L3
    //      50k       220 ns       50 ns    4.40x
    //     150k       281 ns       60 ns    4.67x
    //     300k       341 ns       70 ns    4.86x   <- best on 16 MB
    //     750k       421 ns      120 ns    3.50x   <- both RAM-bound; see below
    //
    // The last row is the point, not a failure. At 750k the optimised book is
    // ~36 MB and leaves a 16 MB L3 as well, so both engines go to memory and the
    // gap COMPRESSES. On a 54 MB L3 it is ~39 MB and still fits, which is where
    // the same depth should widen the gap instead. The shipped number is a bet
    // on the target hardware and section 9 of AWS.md is what settles it: if the
    // bench node's L3 turns out closer to 16 MB than 54 MB, 300k is the better
    // setting and this should go back.
    //
    // Two caveats on the earlier rows. Most of the discrimination is already
    // there by ~50-100k; and that box's rdtscp advances in 38-tick (10 ns)
    // steps, so the optimized column at 300k and below is 5, 6 and 7 quanta and
    // the 150k->300k increment is under its resolution.
    //
    // deep_ticks is 7,500 so the book stays WIDE at this depth: 15,000 levels
    // holds 750k orders at ~50 per level, the same shape 3,000 gave at 300k.
    // Depth without width measures FIFO-queue traversal instead of the price
    // index, which is not the thing being graded.
    //
    // It is close to its ceiling. Asks rest at mid + offset with mid <= 10,050,
    // so 7,500 puts the top of the book at 17,559 against an injection band
    // starting at 20,000 -- about 2,400 ticks of headroom. Beyond ~9,900 they
    // collide and the injection price base has to move first.
    {"cancel_heavy", 320, 90, 200, 12, 78, 30, 42, 2, 20, 33, 45, 3, 2, 1, 7500, 3780},
    // Heavy on takers, but liquidity providers still have to outnumber them:
    // takers with nothing to sweep leave a book a handful of orders deep, which
    // stresses nothing at all.
    {"sweep_heavy", 38, 14, 20, 6, 10, 2, 40, 25, 40, 30, 5, 18, 10, 12, 42, 200},
    {"deep_book", 28, 10, 6, 4, 14, 4, 35, 15, 25, 30, 30, 8, 5, 4, 400, 600},
};

struct KindBias {
  uint32_t marketable, at_touch, near_book, deep;
  uint32_t qty_lo, qty_hi;
  uint32_t cancel_scale_pct;  // scales the profile's cancel_pct
  uint32_t weight;            // sequencer activity weight
};

// A market maker quotes the touch and is the source of ~80% of cancel traffic;
// noise rests deep and rarely cancels.
constexpr KindBias kBias[] = {
    /* MarketMaker  */ {1, 6, 2, 1, 20, 200, 130, 4},
    /* Taker        */ {8, 2, 1, 1, 50, 500, 35, 3},
    /* Noise        */ {1, 1, 3, 8, 10, 100, 40, 1},
    /* LevelStacker */ {2, 5, 3, 1, 5, 50, 70, 1},
};

const KindBias& bias_of(AgentKind k) { return kBias[static_cast<uint8_t>(k)]; }

uint32_t clamp_pct(uint32_t v) { return v > 100 ? 100 : v; }

/// How often a cancel targets a recently placed order rather than any live one.
constexpr uint32_t kRecentCancelPct = 85;

/// Compaction threshold: rebuild once fewer than this fraction of slots are
/// live. At one half, every compaction is paid for by at least as many
/// removals, so the amortised cost per removal is constant.
constexpr uint32_t kCompactWhenAliveBelowPct = 50;

/// Below this many slots, compaction is not worth thinking about.
constexpr size_t kCompactMinSlots = 64;

/// Cancel rate for a session that has reached its resting-order target. Well
/// above any kind's resting-add rate, so depth cannot climb past the target.
constexpr uint32_t kBackPressurePct = 95;

}  // namespace

// ---------------------------------------------------------------- LiveSet

void LiveSet::add(uint64_t coid) {
  if (slots_.size() >= kCompactMinSlots &&
      alive_ * 100u < slots_.size() * kCompactWhenAliveBelowPct) {
    compact();
  }
  pos_[coid] = static_cast<uint32_t>(slots_.size());
  slots_.push_back(coid);
  ++alive_;
}

void LiveSet::remove(uint64_t coid) {
  auto it = pos_.find(coid);
  if (it == pos_.end()) return;  // never rested, or already gone
  slots_[it->second] = 0;
  pos_.erase(it);
  --alive_;
}

void LiveSet::compact() {
  std::vector<uint64_t> kept;
  kept.reserve(alive_);
  // Iterate the age-ordered vector, never the map: the map's order is
  // unspecified and would make the stream depend on the standard library.
  for (uint64_t c : slots_) {
    if (c != 0) kept.push_back(c);
  }
  slots_.swap(kept);
  pos_.clear();
  for (uint32_t i = 0; i < slots_.size(); ++i) pos_[slots_[i]] = i;
  alive_ = static_cast<uint32_t>(slots_.size());
}

uint64_t LiveSet::pick(Rng& rng, uint32_t recent_pct) {
  if (alive_ == 0) return 0;
  const uint32_t n = static_cast<uint32_t>(slots_.size());

  // One probe, biased toward the recent end. Re-quoting cancels the order you
  // just placed because the price moved, not one from ten thousand events ago.
  uint32_t idx;
  if (rng.chance(recent_pct)) {
    const uint32_t window = n / 8 > 0 ? n / 8 : 1;
    idx = n - 1 - rng.below(window);
  } else {
    idx = rng.below(n);
  }

  // Compaction holds at least half the slots live, so walking back to the
  // nearest survivor terminates in ~1 step on average. It consumes no further
  // randomness, which keeps the stream stable against changes in tombstone
  // layout that do not change which orders are live.
  uint32_t scanned = 0;
  while (slots_[idx] == 0) {
    idx = idx == 0 ? n - 1 : idx - 1;
    if (++scanned > n) return 0;  // unreachable while alive_ > 0
  }
  return slots_[idx];
}

const ProfileParams& profile_params(Profile p) { return kProfiles[static_cast<uint32_t>(p)]; }

const char* profile_name(Profile p) { return kProfiles[static_cast<uint32_t>(p)].name; }

bool parse_profile(const std::string& name, Profile& out) {
  for (uint32_t i = 0; i < sizeof(kProfiles) / sizeof(kProfiles[0]); ++i) {
    if (name == kProfiles[i].name) {
      out = static_cast<Profile>(i);
      return true;
    }
  }
  return false;
}

const char* injection_name(InjectionKind k) {
  switch (k) {
    case InjectionKind::CancelOfConsumedOrder: return "cancel_of_consumed_order";
    case InjectionKind::DuplicateCoidAcrossSessions: return "duplicate_coid_across_sessions";
    case InjectionKind::SelfCrossAcrossSessions: return "self_cross_across_sessions";
    case InjectionKind::FokSameFirmExclusion: return "fok_same_firm_exclusion";
    case InjectionKind::SweepExactlyFiveLevels: return "sweep_exactly_five_levels";
    case InjectionKind::ConsumeDeepLevelQueue: return "consume_deep_level_queue";
    case InjectionKind::CancelLastOrderAtLevel: return "cancel_last_order_at_level";
  }
  return "?";
}

// ---------------------------------------------------------------- construction

Generator::Generator(uint64_t seed, Profile profile, uint32_t live_target_override)
    : seed_(seed),
      profile_(profile),
      pp_(&profile_params(profile)),
      live_target_(live_target_override ? live_target_override : profile_params(profile).live_target),
      rng_(seed) {
  build_sessions();
}

void Generator::build_sessions() {
  const uint32_t mix[4] = {pp_->w_market_maker, pp_->w_taker, pp_->w_noise, pp_->w_stacker};
  uint16_t sid = 1;
  for (uint32_t k = 0; k < 4; ++k) {
    for (uint32_t i = 0; i < mix[k]; ++i) {
      Session s{sid,
                // Several sessions share one firm — that is what makes
                // self-trade prevention meaningful rather than decorative.
                static_cast<uint16_t>(1 + (sid % pp_->n_firms)),
                static_cast<AgentKind>(k),
                // Each session gets its own RNG stream, so a session's
                // behaviour does not shift when another session's does.
                Rng(seed_ ^ (static_cast<uint64_t>(sid) * 0x9E3779B97F4A7C15ull)),
                {},
                1,
                0,
                bias_of(static_cast<AgentKind>(k)).weight};
      total_weight_ += s.weight;
      sessions_.push_back(std::move(s));
      ++sid;
    }
  }
}

// ---------------------------------------------------------------- price model

void Generator::step_mid() {
  // A reference price doing a slow random walk. Uniform random prices almost
  // never cross and would test nothing.
  if (rng_.below(100) < 5) {
    mid_ += (rng_.next() & 1) ? 1 : -1;
    // Reflect at the band edges. The walk is bounded so the organic book can
    // never drift up into the injection price band.
    if (mid_ < kMidLow) mid_ = kMidLow + 1;
    if (mid_ > kMidHigh) mid_ = kMidHigh - 1;
  }
}

int32_t Generator::place_price(Session& s, Side side, bool& marketable) {
  const KindBias& b = bias_of(s.kind);
  const uint32_t w0 = pp_->p_marketable * b.marketable;
  const uint32_t w1 = pp_->p_at_touch * b.at_touch;
  const uint32_t w2 = pp_->p_near * b.near_book;
  const uint32_t w3 = pp_->p_deep * b.deep;

  uint32_t roll = s.rng.below(w0 + w1 + w2 + w3);
  int32_t offset;
  bool through_touch;
  if (roll < w0) {
    offset = static_cast<int32_t>(s.rng.range(1, 4));  // through the touch
    through_touch = true;
  } else if (roll < w0 + w1) {
    offset = static_cast<int32_t>(s.rng.range(0, 1));
    through_touch = false;
  } else if (roll < w0 + w1 + w2) {
    offset = static_cast<int32_t>(s.rng.range(2, 8));
    through_touch = false;
  } else {
    // Density decays with distance from the touch, which is the shape a real
    // book has. The minimum of two uniform draws is a triangular distribution
    // — one extra RNG call, no floating point, identical on every platform.
    const uint32_t a = s.rng.below(pp_->deep_ticks);
    const uint32_t b = s.rng.below(pp_->deep_ticks);
    offset = static_cast<int32_t>(9 + (a < b ? a : b));
    through_touch = false;
  }

  // A buy is more aggressive at a HIGHER price; a sell at a lower one.
  marketable = through_touch;
  const int32_t sign = (side == Side::Buy) ? 1 : -1;
  return mid_ + sign * (through_touch ? offset : -offset);
}

// ---------------------------------------------------------------- emission

WireEvent Generator::make_new(Session& s, Side side, int32_t px, uint32_t qty, TIF tif) {
  WireEvent e{};
  e.client_order_id = s.next_coid++;
  e.px = (tif == TIF::Market) ? 0 : px;
  e.qty = qty;
  e.session_id = s.session_id;
  e.participant_id = s.participant_id;
  e.type = EvType::New;
  e.side = side;
  e.tif = tif;
  // Nothing is added to the live set here. apply() asks the book afterwards
  // whether this order actually rested, which is the only way to be right about
  // a marketable Day order that partially filled and rested the remainder.
  return e;
}

WireEvent Generator::make_cancel(uint16_t session_id, uint16_t firm, uint64_t coid) {
  WireEvent e{};
  e.client_order_id = coid;
  e.px = 0;
  e.qty = 0;
  e.session_id = session_id;
  e.participant_id = firm;
  e.type = EvType::Cancel;
  e.side = Side::Buy;  // ignored for cancels, but must be deterministic
  e.tif = TIF::Day;
  return e;
}

WireEvent Generator::emit_from(Session& s) {
  const KindBias& b = bias_of(s.kind);

  // Cancel pressure rises with how full the session's quote book already is.
  //
  // A fixed cancel percentage cannot work now that the live set is exact. In
  // steady state every order that rests must leave, by cancel or by fill, so
  // the cancel SHARE of the stream is not a free parameter — it is pinned near
  // `r / (1 + r)` for a resting fraction r, about 37%. Setting it higher than
  // that drains every session's live set to empty and the cancels become ghost
  // lookups again; setting it lower grows the book without bound. The old code
  // set it to 55% and got away with it only because the list it drew from was
  // ~96% stale, so most of those cancels named nothing.
  //
  // Ramping to the profile's rate as the session approaches its target makes
  // depth the controlled variable and cancel share the emergent one, which is
  // the right way round: depth is what the benchmark is about, and a session
  // far below target should be building quotes, not churning them.
  const uint32_t base = clamp_pct(pp_->cancel_pct * b.cancel_scale_pct / 100);
  const uint32_t n = s.live.size();
  uint32_t cancel_pct;
  if (n >= live_target_) {
    // Hard back-pressure. Without it a kind whose resting fraction is high and
    // whose cancel bias is low — Noise, which rests deep and rarely cancels —
    // never stops growing.
    cancel_pct = kBackPressurePct;
  } else {
    cancel_pct = static_cast<uint32_t>(static_cast<uint64_t>(base) * n / live_target_);
  }

  if (s.rng.chance(cancel_pct)) {
    // ~2% of cancels target an id that never existed at all. The engine cannot
    // tell "never existed" from "already filled" and is not asked to, but the
    // lookup-miss path still has to be exercised.
    if (s.live.empty() || s.rng.chance(2)) {
      const uint64_t ghost = s.next_coid + 1'000'000 + s.rng.below(1000);
      return make_cancel(s.session_id, s.participant_id, ghost);
    }
    // The live set holds only orders the book says are resting, so this hits.
    // It did not used to: the old list tracked "believed resting" and drifted
    // ~96% stale, so 93% of cancels named an order that was already gone. The
    // median ranked event was a FAILED index lookup — the cheapest operation in
    // the engine — and the contest was ranking how fast you can not find
    // something.
    const uint64_t coid = s.live.pick(s.rng, kRecentCancelPct);
    // Not removed here. The cancel might still be rejected — a race the
    // generator does not model but the book does — so apply() removes it when
    // the book actually acknowledges it.
    return make_cancel(s.session_id, s.participant_id, coid);
  }

  Side side = (s.rng.next() & 1) ? Side::Buy : Side::Sell;
  TIF tif = TIF::Day;

  // Only takers use non-Day time-in-force. A market maker sending FOK would be
  // noise for its own sake.
  if (s.kind == AgentKind::Taker) {
    const uint32_t roll = s.rng.below(100);
    if (roll < pp_->tif_ioc_pct) {
      tif = TIF::IOC;
    } else if (roll < pp_->tif_ioc_pct + pp_->tif_fok_pct) {
      tif = TIF::FOK;
    } else if (roll < pp_->tif_ioc_pct + pp_->tif_fok_pct + pp_->tif_market_pct) {
      tif = TIF::Market;
    }
  }

  int32_t px;
  bool marketable = false;
  if (s.kind == AgentKind::LevelStacker) {
    // Repeated same-price adds, to stress one level's queue.
    side = (s.session_id & 1) ? Side::Buy : Side::Sell;
    if (s.stack_px == 0 || s.rng.chance(2)) {
      s.stack_px = mid_ + (side == Side::Buy ? -1 : 1);
    }
    px = s.stack_px;
  } else {
    px = place_price(s, side, marketable);
  }

  // A FOK priced at or through the touch is the interesting case; leave the
  // price as placed, but never let a FOK or a limit carry price 0, which is
  // reserved for market orders (SPEC M1).
  if (tif != TIF::Market && px <= 0) px = 1;

  (void)marketable;  // the book decides what rests now, not a guess
  const uint32_t qty = s.rng.range(b.qty_lo, b.qty_hi);
  return make_new(s, side, px, qty, tif);
}

// ---------------------------------------------------------------- book feedback

namespace {

// Collects one event's output so apply() can walk it. Deliberately not the
// harness's sink: this one keeps nothing but the current event.
class GenSink final : public OutSink {
 public:
  void clear() { evs_.clear(); }
  const std::vector<OutEvent>& events() const { return evs_; }
  void emit(const OutEvent& e) noexcept override { evs_.push_back(e); }

 private:
  std::vector<OutEvent> evs_;
};

}  // namespace

Generator::Session* Generator::session_for(uint16_t session_id) {
  // Injection sessions start at kFirstInjectionSession and have no live set —
  // their orders are scripted and never cancelled by the organic mix.
  if (session_id == 0 || session_id >= kFirstInjectionSession) return nullptr;
  const uint32_t idx = session_id - 1u;  // build_sessions() numbers from 1
  return idx < sessions_.size() ? &sessions_[idx] : nullptr;
}

void Generator::apply(const WireEvent& w, uint64_t seq) {
  static thread_local GenSink sink;
  sink.clear();
  const DecodedEvent d = decode(w, seq);
  dispatch(book_, d, sink);

  // Removals are cross-session. A trade takes liquidity out of whichever
  // session was resting on the other side, and an STP cancel removes a resting
  // order belonging to a session that did not send this event at all — so the
  // emitting session is the wrong place to look.
  for (const OutEvent& e : sink.events()) {
    switch (e.type) {
      case OutType::Trade: {
        // The maker is gone only if it was fully consumed. One query is cheaper
        // and more honest than tracking remaining quantity in a second place.
        if (Session* m = session_for(e.maker.session_id)) {
          if (!book_.is_resting(e.maker.session_id, e.maker.client_order_id)) {
            m->live.remove(e.maker.client_order_id);
          }
        }
        break;
      }
      case OutType::CancelAck: {
        // Ordinary cancel zeroes `maker` and names the order in `taker`; an STP
        // cancel puts the removed resting order in `maker` (SPEC S2/S5).
        const OrderRef& gone = e.maker.session_id != 0 ? e.maker : e.taker;
        if (Session* g = session_for(gone.session_id)) g->live.remove(gone.client_order_id);
        break;
      }
      case OutType::Ack:
      case OutType::Reject:
      case OutType::Expired:
        // Nothing enters the live set on these. A New's membership is decided
        // once, below, from the book itself.
        break;
    }
  }

  if (w.type == EvType::New) {
    if (Session* s = session_for(w.session_id)) {
      if (book_.is_resting(w.session_id, w.client_order_id)) s->live.add(w.client_order_id);
    }
  }
}

uint32_t Generator::pick_session() {
  uint32_t r = rng_.below(total_weight_);
  for (uint32_t i = 0; i < sessions_.size(); ++i) {
    if (r < sessions_[i].weight) return i;
    r -= sessions_[i].weight;
  }
  return 0;
}

// ---------------------------------------------------------------- injections

namespace {

// Each script's length, so the scheduler can refuse to start one it cannot
// finish inside the event budget.
uint64_t script_length(InjectionKind k) {
  switch (k) {
    case InjectionKind::CancelOfConsumedOrder: return 4;
    case InjectionKind::DuplicateCoidAcrossSessions: return 4;
    case InjectionKind::SelfCrossAcrossSessions: return 3;
    case InjectionKind::FokSameFirmExclusion: return 6;
    case InjectionKind::SweepExactlyFiveLevels: return 8;
    case InjectionKind::ConsumeDeepLevelQueue: return 202;
    case InjectionKind::CancelLastOrderAtLevel: return 2;
  }
  return 0;
}

WireEvent inj_new(uint16_t sess, uint16_t firm, uint64_t coid, Side side, int32_t px, uint32_t qty,
                  TIF tif) {
  WireEvent e{};
  e.client_order_id = coid;
  e.px = (tif == TIF::Market) ? 0 : px;
  e.qty = qty;
  e.session_id = sess;
  e.participant_id = firm;
  e.type = EvType::New;
  e.side = side;
  e.tif = tif;
  return e;
}

// Clears the ask side so the case that follows runs against known-empty
// liquidity.
//
// Without this, an injected buy priced up in the injection band crosses every
// organic ask below it, sweeps the organic book, and never reaches its own
// resting orders — the case silently does not fire. A far price band isolates
// resting orders from organic aggressors, but it cannot isolate an injected
// aggressor from organic liquidity: an aggressor always takes the best price
// first, and organic prices are always better than the band.
//
// Injection scripts drain with no organic events interleaved, so the side stays
// clear for exactly as long as the case needs it. The organic book rebuilds
// immediately afterwards, and these seven sweeps are excluded from the stream
// statistics in validate.cpp.
WireEvent inj_clear_asks() {
  WireEvent e{};
  e.client_order_id = 1;
  e.px = 0;
  e.qty = 1'000'000'000;
  e.session_id = 899;
  e.participant_id = 899;
  e.type = EvType::New;
  e.side = Side::Buy;
  e.tif = TIF::Market;
  return e;
}

WireEvent inj_cancel(uint16_t sess, uint16_t firm, uint64_t coid) {
  WireEvent e{};
  e.client_order_id = coid;
  e.session_id = sess;
  e.participant_id = firm;
  e.type = EvType::Cancel;
  e.side = Side::Buy;
  e.tif = TIF::Day;
  return e;
}

}  // namespace

void Generator::enqueue_injection(InjectionKind kind, uint64_t at_seq) {
  const int32_t P = kInjectionPriceBase;
  uint64_t payoff = at_seq;
  uint32_t detail = 0;

  switch (kind) {
    case InjectionKind::CancelOfConsumedOrder: {
      // The cancel at seq N targets an order the aggressor at seq N-1 consumed.
      pending_.push_back(inj_clear_asks());
      pending_.push_back(inj_new(900, 900, 1, Side::Sell, P + 0, 100, TIF::Day));
      pending_.push_back(inj_new(901, 901, 1, Side::Buy, P + 0, 100, TIF::Day));
      pending_.push_back(inj_cancel(900, 900, 1));
      payoff = at_seq + 3;
      break;
    }
    case InjectionKind::DuplicateCoidAcrossSessions: {
      // Both sessions use client_order_id 7 at the same time. An engine keyed on
      // the id alone cancels the wrong one here.
      //
      // Both rest as asks up in the injection band, where no organic buy can
      // reach them, so no clearing sweep is needed: this case has no aggressor.
      pending_.push_back(inj_new(902, 902, 7, Side::Sell, P + 10, 50, TIF::Day));
      pending_.push_back(inj_new(903, 903, 7, Side::Sell, P + 11, 60, TIF::Day));
      pending_.push_back(inj_cancel(903, 903, 7));
      pending_.push_back(inj_cancel(902, 902, 7));
      payoff = at_seq + 2;
      detail = 60;  // must cancel session 903's order, not session 902's 50
      break;
    }
    case InjectionKind::SelfCrossAcrossSessions: {
      // One firm (904) crossing itself through two different sessions. The
      // aggressor is IOC so nothing is left resting up in the injection band
      // for organic traffic to trade against afterwards.
      pending_.push_back(inj_clear_asks());
      pending_.push_back(inj_new(904, 904, 1, Side::Sell, P + 20, 80, TIF::Day));
      pending_.push_back(inj_new(905, 904, 1, Side::Buy, P + 20, 80, TIF::IOC));
      payoff = at_seq + 2;
      detail = 80;
      break;
    }
    case InjectionKind::FokSameFirmExclusion: {
      // THE case: opposite side holds 60 from firm 906 and 50 from firm 907,
      // and firm 907 sends FOK 100. Counting all 110 commits and partially
      // fills; excluding same-firm liquidity rejects cleanly (SPEC F1).
      //
      // The clear matters most here: any organic ask left inside the FOK's
      // limit would count toward fillability and turn the intended Reject into
      // a commit, quietly destroying the one case that separates F1 from the
      // naive fillability check.
      pending_.push_back(inj_clear_asks());
      pending_.push_back(inj_new(906, 906, 1, Side::Sell, P + 30, 60, TIF::Day));
      pending_.push_back(inj_new(907, 907, 1, Side::Sell, P + 30, 50, TIF::Day));
      pending_.push_back(inj_new(908, 907, 1, Side::Buy, P + 30, 100, TIF::FOK));
      pending_.push_back(inj_cancel(906, 906, 1));
      pending_.push_back(inj_cancel(907, 907, 1));
      payoff = at_seq + 3;
      break;
    }
    case InjectionKind::SweepExactlyFiveLevels: {
      pending_.push_back(inj_clear_asks());
      for (uint32_t k = 0; k < 5; ++k) {
        pending_.push_back(
            inj_new(909, 909, k + 1, Side::Sell, P + 40 + static_cast<int32_t>(k), 20, TIF::Day));
      }
      pending_.push_back(inj_new(910, 910, 1, Side::Buy, P + 44, 150, TIF::Day));
      pending_.push_back(inj_cancel(910, 910, 1));
      payoff = at_seq + 6;
      detail = 5;  // exactly five trades, then 50 rests
      break;
    }
    case InjectionKind::ConsumeDeepLevelQueue: {
      pending_.push_back(inj_clear_asks());
      for (uint32_t k = 0; k < 200; ++k) {
        pending_.push_back(inj_new(911, 911, k + 1, Side::Sell, P + 50, 1, TIF::Day));
      }
      pending_.push_back(inj_new(912, 912, 1, Side::Buy, P + 50, 200, TIF::Day));
      payoff = at_seq + 201;
      detail = 200;  // exactly 200 trades from one event
      break;
    }
    case InjectionKind::CancelLastOrderAtLevel: {
      pending_.push_back(inj_new(913, 913, 1, Side::Sell, P + 60, 25, TIF::Day));
      pending_.push_back(inj_cancel(913, 913, 1));
      payoff = at_seq + 1;
      detail = 25;
      break;
    }
  }

  injections_.push_back(InjectionRecord{kind, payoff, detail});
}

void Generator::schedule_injections(uint64_t count) {
  if (count < kMinEventsForInjections) return;

  const InjectionKind order[] = {
      InjectionKind::CancelOfConsumedOrder,      InjectionKind::DuplicateCoidAcrossSessions,
      InjectionKind::SelfCrossAcrossSessions,    InjectionKind::FokSameFirmExclusion,
      InjectionKind::SweepExactlyFiveLevels,     InjectionKind::ConsumeDeepLevelQueue,
      InjectionKind::CancelLastOrderAtLevel,
  };
  const uint64_t n = sizeof(order) / sizeof(order[0]);

  // Spread across the stream's first third rather than its whole length.
  //
  // Five of the seven scripts open with a market order that consumes the entire
  // organic ask side (SPEC M2: a market order accepts every price). That is the
  // only way to give an injected aggressor a known-empty book, since an
  // aggressor always takes the best price first and organic prices are always
  // better than the injection band — but at a few hundred thousand resting
  // orders it removes half the book, and the refill costs hundreds of thousands
  // of events.
  //
  // Keeping them early puts the damage where the book is filling up anyway, and
  // leaves the back two thirds — the part the benchmark's steady state is
  // measured on — undisturbed. They still run against a book with real depth,
  // and the correctness lane still gets them spread over its whole warm-up.
  const uint64_t window = count / 3;
  for (uint64_t i = 0; i < n; ++i) {
    const uint64_t at = window * (i + 1) / (n + 1);
    schedule_.push_back({at, order[i]});
  }
}

// ---------------------------------------------------------------- generate

std::vector<WireEvent> Generator::generate(uint64_t count) {
  std::vector<WireEvent> events;
  events.reserve(count);
  schedule_injections(count);

  for (seq_ = 0; seq_ < count; ++seq_) {
    if (pending_.empty() && next_scheduled_ < schedule_.size() &&
        seq_ >= schedule_[next_scheduled_].first) {
      const auto [at, kind] = schedule_[next_scheduled_];
      ++next_scheduled_;
      if (seq_ + script_length(kind) <= count) enqueue_injection(kind, seq_);
    }

    if (!pending_.empty()) {
      events.push_back(pending_.front());
      pending_.pop_front();
    } else {
      step_mid();
      events.push_back(emit_from(sessions_[pick_session()]));
    }

    // Every emitted event, injections included. An injection's clearing sweep
    // consumes thousands of organic resting orders; a book that had not seen it
    // would leave those orders in their sessions' live sets forever, which is
    // precisely the staleness this loop exists to remove.
    apply(events.back(), seq_);
  }

  return events;
}

// ---------------------------------------------------------------- stream I/O

bool write_stream(const std::string& path, uint64_t seed, Profile profile, uint32_t live_target,
                  const std::vector<WireEvent>& events, std::string& err) {
  std::FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) {
    err = "cannot open " + path + " for writing";
    return false;
  }

  StreamHeader h{};
  std::memcpy(h.magic, kStreamMagic, sizeof(h.magic));
  h.seed = seed;
  h.event_count = events.size();
  h.profile_id = static_cast<uint32_t>(profile);
  h.tick_size = 1;
  h.live_target = live_target;

  bool ok = std::fwrite(&h, sizeof(h), 1, f) == 1;
  if (ok && !events.empty()) {
    ok = std::fwrite(events.data(), sizeof(WireEvent), events.size(), f) == events.size();
  }
  std::fclose(f);
  if (!ok) err = "short write to " + path;
  return ok;
}

bool read_stream_fd(int fd, StreamHeader& header, std::vector<WireEvent>& events,
                    std::string& err) {
  std::FILE* f = fdopen(fd, "rb");
  if (!f) {
    err = "cannot read stream from inherited fd";
    return false;
  }
  if (std::fread(&header, sizeof(header), 1, f) != 1) {
    err = "truncated header on inherited fd";
    std::fclose(f);
    return false;
  }
  if (std::memcmp(header.magic, kStreamMagic, sizeof(header.magic)) != 0) {
    err = "inherited fd does not carry a MEBENCH1 stream";
    std::fclose(f);
    return false;
  }
  events.resize(header.event_count);
  const size_t got =
      header.event_count ? std::fread(events.data(), sizeof(WireEvent), header.event_count, f) : 0;
  std::fclose(f);
  if (got != header.event_count) {
    err = "truncated stream body on inherited fd";
    return false;
  }
  return true;
}

bool read_stream(const std::string& path, StreamHeader& header, std::vector<WireEvent>& events,
                 std::string& err) {
  std::FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) {
    err = "cannot open " + path;
    return false;
  }
  if (std::fread(&header, sizeof(header), 1, f) != 1) {
    err = "truncated header in " + path;
    std::fclose(f);
    return false;
  }
  if (std::memcmp(header.magic, kStreamMagic, sizeof(header.magic)) != 0) {
    err = path + " is not a MEBENCH1 stream";
    std::fclose(f);
    return false;
  }
  events.resize(header.event_count);
  const size_t got =
      header.event_count ? std::fread(events.data(), sizeof(WireEvent), header.event_count, f) : 0;
  std::fclose(f);
  if (got != header.event_count) {
    err = "truncated stream body in " + path;
    return false;
  }
  return true;
}

}  // namespace mebench::generator
