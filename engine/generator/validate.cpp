#include "generator/validate.h"

#include <algorithm>
#include <sstream>
#include <unordered_map>

#include "common/decode.h"
#include "harness/invariants.h"
#include "mebench/out.h"
#include "reference/reference.h"

namespace mebench::generator {
namespace {

// The generator's own bar. Below the floor the stream has stopped crossing and
// is testing nothing.
//
// The ceiling used to stand in for "the book actually builds up", which it can
// no longer do well: fill rates rose across every profile once orders reliably
// rested and were matched later instead of being forgotten. Depth is now
// measured directly, twice, so the ceiling only has to catch a stream that has
// turned into pure crossing — hence 45% rather than the old 40%, which
// sweep_heavy, the profile whose entire purpose is aggression, now sits just
// above.
constexpr double kFillRateFloor = 0.15;
constexpr double kFillRateCeiling = 0.45;
constexpr double kTradeShareFloor = 0.05;

// Cancels that name a genuinely resting order. The generator's live sets are
// driven by its own reference book, so the only intended misses are the ~2%
// deliberate ghost ids; anything much below this means the feedback loop has
// broken and the benchmark is back to grading failed lookups.
constexpr double kCancelHitFloor = 0.85;

// The shallowest settled sample, as a percentage of the median settled sample.
constexpr uint32_t kDepthFloorPctOfMedian = 60;

// How often depth is sampled across the stream, and how many of those samples
// are printed. Sampling coarsely enough to report is sampling too coarsely to
// catch a dip that lasts a few percent of the run.
constexpr uint64_t kDepthSamples = 100;
constexpr size_t kDepthReportEvery = 10;

// Floor on how many samples are treated as warm-up. The real cut comes from
// warmup_events(), but a deep profile whose warm-up is a small fraction of a
// very long stream still deserves a few samples of margin.
constexpr size_t kMinWarmupSamples = 5;

class ValidatingSink final : public OutSink {
 public:
  explicit ValidatingSink(const std::vector<InjectionRecord>& injections) {
    for (const auto& i : injections) watched_[i.payoff_seq] = {};
  }

  /// Start collecting a new event's outputs, so the invariant layer can be fed
  /// per event exactly as the harness feeds it.
  void begin_event() { current_.clear(); }
  const std::vector<OutEvent>& last_event() const { return current_; }

  void emit(const OutEvent& e) noexcept override {
    current_.push_back(e);
    // Injected traffic is synthetic — a clearing sweep alone submits a billion
    // units. Counting it would drown the statistics that say whether the
    // ORGANIC stream is worth trusting.
    const bool organic = e.maker.session_id < Generator::kFirstInjectionSession &&
                         e.taker.session_id < Generator::kFirstInjectionSession;

    switch (e.type) {
      case OutType::Trade:
        if (organic) {
          ++trades;
          traded_qty += e.qty;
        }
        break;
      case OutType::Ack: ++acks; break;
      case OutType::Reject:
        if (e.reason == RejectReason::UnknownOrder) {
          ++reject_unknown;
        } else {
          ++reject_fok;
        }
        break;
      case OutType::CancelAck:
        ++cancel_acks;
        // An STP cancel carries the removed resting order in `maker`; an
        // ordinary cancel leaves it zeroed. Session ids start at 1.
        if (e.maker.session_id != 0) ++stp_cancels;
        break;
      case OutType::Expired: ++expired; break;
    }

    auto it = watched_.find(e.in_seq);
    if (it != watched_.end()) it->second.push_back(e);
  }

  const std::vector<OutEvent>& at(uint64_t seq) const {
    static const std::vector<OutEvent> kEmpty;
    auto it = watched_.find(seq);
    return it == watched_.end() ? kEmpty : it->second;
  }

  uint64_t trades = 0, acks = 0, reject_unknown = 0, reject_fok = 0;
  uint64_t cancel_acks = 0, stp_cancels = 0, expired = 0, traded_qty = 0;
  std::vector<OutEvent> current_;

 private:
  std::unordered_map<uint64_t, std::vector<OutEvent>> watched_;
};

uint64_t count_type(const std::vector<OutEvent>& evs, OutType t) {
  uint64_t n = 0;
  for (const auto& e : evs) n += (e.type == t) ? 1 : 0;
  return n;
}

std::string summarize(const std::vector<OutEvent>& evs) {
  std::ostringstream s;
  s << evs.size() << " output(s):";
  for (const auto& e : evs) {
    switch (e.type) {
      case OutType::Trade: s << " Trade(" << e.qty << "@" << e.price << ")"; break;
      case OutType::Ack: s << " Ack"; break;
      case OutType::Reject:
        s << " Reject("
          << (e.reason == RejectReason::UnknownOrder ? "UnknownOrder" : "FokUnfillable") << ")";
        break;
      case OutType::CancelAck:
        s << (e.maker.session_id != 0 ? " StpCancelAck(" : " CancelAck(") << e.qty << ")";
        break;
      case OutType::Expired: s << " Expired(" << e.qty << ")"; break;
    }
  }
  return s.str();
}

// Each injection's intended EFFECT, not merely its presence. "The event is in
// the stream" is worth very little; "the reference reacted to it the way the
// spec says" is the check that matters.
Check check_injection(const InjectionRecord& inj, const ValidatingSink& sink) {
  const std::vector<OutEvent>& evs = sink.at(inj.payoff_seq);
  const std::string name = std::string("injection: ") + injection_name(inj.kind);
  std::ostringstream why;

  switch (inj.kind) {
    case InjectionKind::CancelOfConsumedOrder: {
      const bool ok = evs.size() == 1 && evs[0].type == OutType::Reject &&
                      evs[0].reason == RejectReason::UnknownOrder;
      if (!ok) why << "expected one Reject/UnknownOrder, got " << summarize(evs);
      return {name, ok, why.str()};
    }
    case InjectionKind::DuplicateCoidAcrossSessions: {
      const bool ok = evs.size() == 1 && evs[0].type == OutType::CancelAck &&
                      evs[0].taker.session_id == 903 && evs[0].qty == inj.detail;
      if (!ok) {
        why << "expected one CancelAck for session 903 carrying qty " << inj.detail
            << " (cancelling session 902's identical client_order_id would show qty 50), got "
            << summarize(evs);
      }
      return {name, ok, why.str()};
    }
    case InjectionKind::SelfCrossAcrossSessions: {
      bool stp = false;
      for (const auto& e : evs) {
        if (e.type == OutType::CancelAck && e.maker.session_id != 0 &&
            e.maker.participant_id == e.taker.participant_id && e.qty == inj.detail) {
          stp = true;
        }
      }
      const bool ok = stp && count_type(evs, OutType::Trade) == 0;
      if (!ok) why << "expected an STP CancelAck of " << inj.detail << " and no trade, got "
                   << summarize(evs);
      return {name, ok, why.str()};
    }
    case InjectionKind::FokSameFirmExclusion: {
      const bool ok = evs.size() == 1 && evs[0].type == OutType::Reject &&
                      evs[0].reason == RejectReason::FokUnfillable;
      if (!ok) {
        why << "expected exactly one Reject/FokUnfillable (SPEC F1: the 50 from the "
               "aggressor's own firm must not count toward fillability), got "
            << summarize(evs);
      }
      return {name, ok, why.str()};
    }
    case InjectionKind::SweepExactlyFiveLevels: {
      const uint64_t trades = count_type(evs, OutType::Trade);
      const bool ok = trades == inj.detail;
      if (!ok) why << "expected exactly " << inj.detail << " trades, got " << trades;
      return {name, ok, why.str()};
    }
    case InjectionKind::ConsumeDeepLevelQueue: {
      const uint64_t trades = count_type(evs, OutType::Trade);
      const bool ok = trades == inj.detail;
      if (!ok) why << "expected exactly " << inj.detail << " trades from one event, got " << trades;
      return {name, ok, why.str()};
    }
    case InjectionKind::CancelLastOrderAtLevel: {
      const bool ok =
          evs.size() == 1 && evs[0].type == OutType::CancelAck && evs[0].qty == inj.detail;
      if (!ok) why << "expected one CancelAck of " << inj.detail << ", got " << summarize(evs);
      return {name, ok, why.str()};
    }
  }
  return {name, false, "unknown injection kind"};
}

}  // namespace

uint64_t warmup_events(Profile p, uint32_t live_target) {
  const ProfileParams& pp = profile_params(p);
  const uint64_t target = live_target ? live_target : pp.live_target;
  // Warm-up is proportional to the depth actually wanted, because the live set
  // holds only resting orders. Measured at ~12 events per resting order on
  // cancel_heavy, and depth settles near 0.62 * sessions * live_target, so the
  // factor below is 12 * 0.62 rounded up. It is not simply "one New per order":
  // cancels, fills and non-resting adds are interleaved throughout, and the
  // cancel controller ramps up as the book fills, so the net accumulation rate
  // falls as the target is approached.
  //
  // It used to be 5x a target that was ~96% stale, so the same book cost 25x
  // more events to build than it needed to — which is what made deep books
  // unaffordable and kept the benchmark inside L3.
  return static_cast<uint64_t>(pp.n_sessions) * target * 8ull;
}

ValidationReport validate_stream(const std::vector<WireEvent>& events,
                                 const std::vector<InjectionRecord>& injections, Profile profile,
                                 uint32_t live_target) {
  ValidationReport rep;
  rep.event_count = events.size();

  reference::ReferenceEngine engine;
  ValidatingSink sink(injections);
  uint64_t organic_events = 0;

  // The reference is held to the same laws a submission is. Plan section 13
  // lists "reference passes all invariants on all four profiles" as a
  // pre-event check; running it here is what makes that a fact rather than an
  // assumption, and a stream is only worth trusting if the oracle behaved on it.
  harness::InvariantChecker invariants;
  std::string invariant_error;
  uint64_t invariant_failed_at = 0;

  // Sampled finely, reported coarsely. Ten samples average right over the thing
  // the floor check is looking for: an injection's clearing sweep empties one
  // whole side of the book, and the refill takes a few hundred thousand events
  // — long enough to matter, short enough for a decile to hide it completely.
  const uint64_t sample_every =
      events.empty() ? 1 : (events.size() + kDepthSamples - 1) / kDepthSamples;

  for (uint64_t i = 0; i < events.size(); ++i) {
    const WireEvent& w = events[i];
    const bool organic = w.session_id < Generator::kFirstInjectionSession;
    if (w.type == EvType::New) {
      ++rep.new_count;
      if (organic) rep.submitted_qty += w.qty;
    } else {
      ++rep.cancel_count;
    }
    if (organic) ++organic_events;

    const DecodedEvent d = decode(w, i);
    sink.begin_event();
    dispatch(engine, d, sink);

    if (invariant_error.empty() && !invariants.on_event(d, sink.last_event(), invariant_error)) {
      invariant_failed_at = i;
    }

    if ((i + 1) % sample_every == 0) {
      BookSnapshot snap{};
      build_book(engine, snap);
      rep.depth_samples.push_back(snap.resting_order_count);
      if (invariant_error.empty() && !invariants.on_snapshot(snap, invariant_error)) {
        invariant_failed_at = i;
      }
    }
  }

  rep.trade_count = sink.trades;
  rep.ack_count = sink.acks;
  rep.reject_unknown_count = sink.reject_unknown;
  rep.reject_fok_count = sink.reject_fok;
  rep.expired_count = sink.expired;
  rep.cancel_ack_count = sink.cancel_acks;
  rep.stp_cancel_count = sink.stp_cancels;
  rep.traded_qty = sink.traded_qty;
  rep.fill_rate = rep.submitted_qty ? static_cast<double>(rep.traded_qty) /
                                          static_cast<double>(rep.submitted_qty)
                                    : 0.0;
  // A cancel either found its order (CancelAck) or did not (UnknownOrder
  // reject). STP cancel-acks are removals the book chose, not cancel messages,
  // so they are excluded.
  rep.cancel_hit_count = rep.cancel_ack_count - rep.stp_cancel_count;
  rep.cancel_hit_rate = rep.cancel_count ? static_cast<double>(rep.cancel_hit_count) /
                                               static_cast<double>(rep.cancel_count)
                                         : 0.0;

  {
    std::ostringstream s;
    if (invariant_error.empty()) {
      s << "the reference obeyed every invariant across all " << rep.event_count << " events";
    } else {
      s << "at event " << invariant_failed_at << ": " << invariant_error;
    }
    rep.checks.push_back({"reference passes the invariant layer", invariant_error.empty(), s.str()});
  }

  {
    std::ostringstream s;
    s << "fill rate " << rep.fill_rate << " (traded " << rep.traded_qty << " of " << rep.submitted_qty
      << " submitted); want " << kFillRateFloor << "-" << kFillRateCeiling;
    rep.checks.push_back(
        {"fill rate in range", rep.fill_rate >= kFillRateFloor && rep.fill_rate <= kFillRateCeiling,
         s.str()});
  }

  {
    const double share = organic_events ? static_cast<double>(rep.trade_count) /
                                              static_cast<double>(organic_events)
                                        : 0.0;
    std::ostringstream s;
    s << rep.trade_count << " trades = " << share << " of events; want > " << kTradeShareFloor;
    rep.checks.push_back({"trade count above floor", share > kTradeShareFloor, s.str()});
  }

  {
    std::ostringstream s;
    s << rep.cancel_hit_count << " of " << rep.cancel_count << " cancels found their order ("
      << rep.cancel_hit_rate * 100.0 << "%); want > " << kCancelHitFloor * 100.0
      << "% so the median ranked event is a real erase and not a failed lookup";
    rep.checks.push_back(
        {"cancels mostly hit", rep.cancel_hit_rate > kCancelHitFloor, s.str(),
         /*skipped=*/rep.cancel_count == 0});
  }

  if (events.size() < warmup_events(profile, live_target)) {
    std::ostringstream s;
    s << "stream is shorter than the ~" << warmup_events(profile, live_target)
      << " events this profile needs to reach steady state; depth is still filling up, so "
         "growth here means nothing";
    rep.checks.push_back({"book depth bounded", true, s.str(), /*skipped=*/true});
    rep.checks.push_back({"book depth holds up", true, s.str(), /*skipped=*/true});
  } else {
    // Warm-up is excluded from both depth checks, and it is also where the
    // injections live: an injection's clearing sweep is a market order that
    // takes the whole organic ask side, and the book is still filling anyway.
    //
    // The window runs to whichever is later: the end of warm-up, or the end of
    // the injection window (the stream's first third), because the sweeps are a
    // known deliberate disturbance and this check is about whether the ORGANIC
    // steady state holds depth.
    const size_t warm_samples = std::max<size_t>(
        {kMinWarmupSamples, kDepthSamples / 3 + 1,
         static_cast<size_t>(warmup_events(profile, live_target) * kDepthSamples /
                             std::max<uint64_t>(events.size(), 1))});
    const bool enough = rep.depth_samples.size() > warm_samples + 4;
    std::vector<uint32_t> settled;
    uint32_t median = 0, worst = 0;
    if (enough) {
      settled.assign(rep.depth_samples.begin() + warm_samples, rep.depth_samples.end());
      std::vector<uint32_t> sorted = settled;
      std::sort(sorted.begin(), sorted.end());
      median = sorted[sorted.size() / 2];
      worst = sorted.front();
    }

    // Depth must stay bounded, not grow monotonically. A book that only ever
    // grows means the cancel path is not keeping up and the stream is drifting
    // away from anything realistic.
    {
      std::ostringstream s;
      bool ok = enough && median > 0 && rep.depth_samples.back() <= median * 3 / 2;
      if (enough) {
        s << "depth across the stream:";
        for (size_t i = 0; i < rep.depth_samples.size(); i += kDepthReportEvery) {
          s << " " << rep.depth_samples[i];
        }
        s << "; final " << rep.depth_samples.back() << " vs settled median " << median;
      } else {
        s << "too few samples";
      }
      rep.checks.push_back({"book depth bounded", ok, s.str()});
    }

    // The other direction, and the one bounded-ness cannot see: depth must not
    // COLLAPSE either.
    //
    // Five of the seven injections open with a MARKET order, and a market order
    // accepts every price (SPEC M2), so it does not clear the near book — it
    // consumes the entire organic ask side. At a few thousand resting orders
    // that is invisible and refills almost at once. At a few hundred thousand it
    // is half the book, and the refill takes long enough that a large slice of
    // the timed region would run against a book far shallower than the profile
    // claims to build. Depth is the whole point of the deep profiles, so it is
    // asserted rather than assumed — and sampled finely enough to see it.
    {
      std::ostringstream f;
      bool ok = enough && median > 0 && worst * 100u >= median * kDepthFloorPctOfMedian;
      if (enough) {
        f << "shallowest settled sample " << worst << " vs median " << median << " ("
          << (median ? worst * 100.0 / median : 0.0) << "%); want >= " << kDepthFloorPctOfMedian
          << "%";
      } else {
        f << "too few samples";
      }
      rep.checks.push_back({"book depth holds up", ok, f.str()});
    }
  }

  if (events.size() >= Generator::kMinEventsForInjections) {
    std::ostringstream s;
    s << injections.size() << " scheduled";
    rep.checks.push_back({"all adversarial injections scheduled", injections.size() == 7, s.str()});
  }
  for (const auto& inj : injections) rep.checks.push_back(check_injection(inj, sink));

  return rep;
}

}  // namespace mebench::generator
