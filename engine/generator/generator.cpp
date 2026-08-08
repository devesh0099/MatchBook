#include "generator/generator.h"

#include <cstdio>
#include <cstring>

namespace mebench::generator {
namespace {

// Profiles differ by who is in the room (agent mix), how cancel-heavy they are,
// and the TIF mix takers use. Placement is the product of the profile's
// baseline weights and the agent kind's own bias, below.
constexpr ProfileParams kProfiles[] = {
    // name         sess firms  mm tk ns st  cancel  mkt touch near deep  ioc fok mkt  live
    {"balanced", 24, 10, 6, 6, 6, 6, 45, 25, 35, 30, 10, 12, 8, 6, 150},
    // The ranked benchmark profile. live_target is deliberately large: this is
    // the one profile whose book depth decides what the contest can resolve. A
    // few hundred resting orders fit in L1 and every data structure looks
    // identical; a few thousand is where the cancel index and the level layout
    // start to cost real cache misses.
    {"cancel_heavy", 32, 12, 16, 6, 6, 4, 68, 22, 40, 28, 10, 10, 6, 5, 2600},
    // Heavy on takers, but liquidity providers still have to outnumber them:
    // takers with nothing to sweep leave a book a handful of orders deep, which
    // stresses nothing at all.
    {"sweep_heavy", 38, 14, 20, 6, 10, 2, 40, 25, 40, 30, 5, 18, 10, 12, 200},
    {"deep_book", 28, 10, 6, 4, 14, 4, 35, 15, 25, 30, 30, 8, 5, 4, 600},
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

}  // namespace

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

Generator::Generator(uint64_t seed, Profile profile)
    : seed_(seed), profile_(profile), pp_(&profile_params(profile)), rng_(seed) {
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
      s.live.reserve(256);
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
    offset = static_cast<int32_t>(s.rng.range(9, 50));
    through_touch = false;
  }

  // A buy is more aggressive at a HIGHER price; a sell at a lower one.
  marketable = through_touch;
  const int32_t sign = (side == Side::Buy) ? 1 : -1;
  return mid_ + sign * (through_touch ? offset : -offset);
}

// ---------------------------------------------------------------- emission

WireEvent Generator::make_new(Session& s, Side side, int32_t px, uint32_t qty, TIF tif,
                              bool likely_rests) {
  WireEvent e{};
  e.client_order_id = s.next_coid++;
  e.px = (tif == TIF::Market) ? 0 : px;
  e.qty = qty;
  e.session_id = s.session_id;
  e.participant_id = s.participant_id;
  e.type = EvType::New;
  e.side = side;
  e.tif = tif;

  // "Believed resting" is intentional: the generator does not run a matching
  // engine, so this list still accumulates orders that were actually filled —
  // which is exactly how cancels for already-filled orders get produced.
  //
  // But an order placed THROUGH the touch is filled almost immediately, and
  // tracking those made the list mostly garbage: cancels then overwhelmingly
  // hit dead orders and stop removing real resting liquidity, so the book grows
  // without bound and the stream drifts away from anything realistic. Excluding
  // them keeps the list approximately right while leaving plenty of stale
  // entries from orders that were filled later.
  if (tif == TIF::Day && likely_rests) {
    // Backstop only. Eviction drops an order nobody can ever cancel again, so
    // it must stay well clear of live_target, which the cancel back-pressure in
    // emit_from() already holds the list at.
    if (s.live.size() >= pp_->live_target * 4u) s.live.erase(s.live.begin());
    s.live.push_back(e.client_order_id);
  }
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
  uint32_t cancel_pct = clamp_pct(pp_->cancel_pct * b.cancel_scale_pct / 100);

  // A real quoting desk holds a bounded number of live orders: once its book of
  // quotes gets long it cancels before it adds.
  //
  // This has to be hard back-pressure, not a nudge. A session whose add rate
  // beats its cancel rate grows its live list until the cap below evicts the
  // oldest entry — and an evicted order is one nobody will ever cancel again,
  // so it rests forever. That leak is linear in stream length: deep_book went
  // from 5.7k to 35.6k resting orders over 3M events before this.
  if (s.live.size() > pp_->live_target) cancel_pct = clamp_pct(cancel_pct < 85 ? 85 : cancel_pct);

  if (s.rng.chance(cancel_pct)) {
    // ~2% of cancels target an id that never existed at all.
    if (s.live.empty() || s.rng.chance(2)) {
      const uint64_t ghost = s.next_coid + 1'000'000 + s.rng.below(1000);
      return make_cancel(s.session_id, s.participant_id, ghost);
    }
    const uint32_t idx = s.rng.below(static_cast<uint32_t>(s.live.size()));
    const uint64_t coid = s.live[idx];
    s.live[idx] = s.live.back();
    s.live.pop_back();
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

  const uint32_t qty = s.rng.range(b.qty_lo, b.qty_hi);
  return make_new(s, side, px, qty, tif, !marketable);
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
  for (uint64_t i = 0; i < n; ++i) {
    // Spread them through the stream rather than bunching at the start, so a
    // divergence that only appears deep into a run still gets caught.
    const uint64_t at = count * (i + 1) / (n + 2);
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
      continue;
    }

    step_mid();
    events.push_back(emit_from(sessions_[pick_session()]));
  }

  return events;
}

// ---------------------------------------------------------------- stream I/O

bool write_stream(const std::string& path, uint64_t seed, Profile profile,
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

  bool ok = std::fwrite(&h, sizeof(h), 1, f) == 1;
  if (ok && !events.empty()) {
    ok = std::fwrite(events.data(), sizeof(WireEvent), events.size(), f) == events.size();
  }
  std::fclose(f);
  if (!ok) err = "short write to " + path;
  return ok;
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
