#include "generator/validate.h"

#include <sstream>
#include <unordered_map>

#include "common/decode.h"
#include "mebench/out.h"
#include "reference/reference.h"

namespace mebench::generator {
namespace {

// The generator's own bar. Below the floor the stream has stopped crossing and
// is testing nothing; above the ceiling the book never builds up and the cancel
// path — where the interesting data-structure decisions live — goes unexercised.
constexpr double kFillRateFloor = 0.15;
constexpr double kFillRateCeiling = 0.40;
constexpr double kTradeShareFloor = 0.05;

class ValidatingSink final : public OutSink {
 public:
  explicit ValidatingSink(const std::vector<InjectionRecord>& injections) {
    for (const auto& i : injections) watched_[i.payoff_seq] = {};
  }

  void emit(const OutEvent& e) noexcept override {
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
      case OutType::Trade: s << " Trade(" << e.qty << "@" << e.px << ")"; break;
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

ValidationReport validate_stream(const std::vector<WireEvent>& events,
                                 const std::vector<InjectionRecord>& injections) {
  ValidationReport rep;
  rep.event_count = events.size();

  reference::ReferenceEngine engine;
  ValidatingSink sink(injections);
  uint64_t organic_events = 0;

  const uint64_t sample_every = events.empty() ? 1 : (events.size() + 9) / 10;

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
    dispatch(engine, d, sink);

    if ((i + 1) % sample_every == 0) {
      BookSnapshot snap{};
      engine.snapshot(snap);
      rep.depth_samples.push_back(snap.resting_order_count);
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
    // Depth must stay bounded, not grow monotonically. Compare the final decile
    // against the median of the settled deciles: a book that only ever grows
    // means the cancel path is not keeping up and the stream is drifting away
    // from anything realistic.
    bool ok = rep.depth_samples.size() >= 4;
    std::ostringstream s;
    if (ok) {
      std::vector<uint32_t> settled(rep.depth_samples.begin() + 2, rep.depth_samples.end());
      std::vector<uint32_t> sorted = settled;
      for (size_t a = 0; a + 1 < sorted.size(); ++a) {
        for (size_t b = a + 1; b < sorted.size(); ++b) {
          if (sorted[b] < sorted[a]) std::swap(sorted[a], sorted[b]);
        }
      }
      const uint32_t median = sorted[sorted.size() / 2];
      const uint32_t last = rep.depth_samples.back();
      ok = median > 0 && last <= median * 3 / 2;
      s << "depth by decile:";
      for (uint32_t d : rep.depth_samples) s << " " << d;
      s << "; final " << last << " vs settled median " << median;
    } else {
      s << "too few samples";
    }
    rep.checks.push_back({"book depth bounded", ok, s.str()});
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
