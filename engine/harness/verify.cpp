#include "harness/verify.h"

#include <chrono>
#include <cstdio>
#include <memory>
#include <sstream>

#include "common/decode.h"
#include "harness/hash_sink.h"
#include "harness/invariants.h"

namespace mebench::harness {
namespace {

using Clock = std::chrono::steady_clock;

class CaptureSink final : public OutSink {
 public:
  explicit CaptureSink(std::vector<OutEvent>& into) : into_(into) {}
  void emit(const OutEvent& e) noexcept override { into_.push_back(e); }

 private:
  std::vector<OutEvent>& into_;
};

bool same(const OutEvent& a, const OutEvent& b) {
  return a.in_seq == b.in_seq && a.maker == b.maker && a.taker == b.taker && a.px == b.px &&
         a.qty == b.qty && a.type == b.type && a.aggressor_side == b.aggressor_side &&
         a.reason == b.reason;
}

bool same(const BookSnapshot& a, const BookSnapshot& b, std::string& what) {
  auto levels_equal = [](const LevelSnapshot& x, const LevelSnapshot& y) {
    return x.px == y.px && x.total_qty == y.total_qty && x.order_count == y.order_count &&
           x.front_seq == y.front_seq;
  };
  if (a.n_bids != b.n_bids) return what = "number of bid levels", false;
  if (a.n_asks != b.n_asks) return what = "number of ask levels", false;
  if (a.resting_qty_total != b.resting_qty_total) return what = "total resting quantity", false;
  if (a.resting_order_count != b.resting_order_count) return what = "resting order count", false;
  for (uint32_t i = 0; i < a.n_bids; ++i) {
    if (!levels_equal(a.bids[i], b.bids[i])) {
      return what = "bid level " + std::to_string(i), false;
    }
  }
  for (uint32_t i = 0; i < a.n_asks; ++i) {
    if (!levels_equal(a.asks[i], b.asks[i])) {
      return what = "ask level " + std::to_string(i), false;
    }
  }
  return true;
}

const char* type_name(OutType t) {
  switch (t) {
    case OutType::Trade: return "TRADE";
    case OutType::Ack: return "ACK";
    case OutType::Reject: return "REJECT";
    case OutType::CancelAck: return "CANCEL_ACK";
    case OutType::Expired: return "EXPIRED";
  }
  return "?";
}

const char* reason_name(RejectReason r) {
  switch (r) {
    case RejectReason::None: return "none";
    case RejectReason::UnknownOrder: return "UnknownOrder";
    case RejectReason::FokUnfillable: return "FokUnfillable";
  }
  return "?";
}

const char* tif_name(TIF t) {
  switch (t) {
    case TIF::Day: return "DAY";
    case TIF::IOC: return "IOC";
    case TIF::FOK: return "FOK";
    case TIF::Market: return "MARKET";
  }
  return "?";
}

// The firm is always shown. It is not decoration: self-trade prevention keys on
// it, so a divergence can live entirely in participant_id — and a report that
// omitted it would print an "expected" and an "actual" line that read
// identically.
std::string short_ref(const OrderRef& r) {
  if (r.session_id == 0 && r.client_order_id == 0 && r.participant_id == 0) return "-";
  std::ostringstream s;
  s << "(s" << r.session_id << ",c" << r.client_order_id << ",f" << r.participant_id << ")";
  return s.str();
}

std::string describe_out(const OutEvent& e) {
  std::ostringstream s;
  s << type_name(e.type);
  if (e.type == OutType::Trade) {
    s << " " << e.qty << " @ " << e.px;
  } else if (e.type == OutType::CancelAck || e.type == OutType::Expired) {
    s << " " << e.qty;
  }
  s << "  maker=" << short_ref(e.maker) << "  taker=" << short_ref(e.taker);
  if (e.reason != RejectReason::None) s << "  reason=" << reason_name(e.reason);
  return s.str();
}

std::string describe_in(const WireEvent& w, uint64_t seq) {
  std::ostringstream s;
  s << "seq=" << seq << "  ";
  if (w.type == EvType::Cancel) {
    s << "CANCEL session=" << w.session_id << " coid=" << w.client_order_id;
    return s.str();
  }
  s << "NEW " << (w.side == Side::Buy ? "buy " : "sell ") << w.qty;
  if (w.tif == TIF::Market) {
    s << " @ market";
  } else {
    s << " @ " << w.px;
  }
  s << "  session=" << w.session_id << " coid=" << w.client_order_id << " firm=" << w.participant_id
    << " tif=" << tif_name(w.tif);
  return s.str();
}

struct RunFailure {
  bool failed = false;
  VerifyOutcome outcome = VerifyOutcome::Passed;
  uint64_t event_index = 0;  // index within the replayed range
  uint64_t in_seq = 0;
  uint64_t output_index = 0;
  bool have_expected = false;
  bool have_actual = false;
  OutEvent expected{};
  OutEvent actual{};
  std::string message;
  uint64_t events_processed = 0;
  uint64_t digest = 0;
};

// Which rule areas this event exercised, judged from the ORACLE's output so a
// broken submission cannot talk its way out of a category.
void classify(const DecodedEvent& d, const std::vector<OutEvent>& oracle_outs, uint64_t* exercised,
              Category* cats, uint32_t& n_cats) {
  n_cats = 0;
  auto add = [&](Category c) {
    exercised[static_cast<uint32_t>(c)]++;
    cats[n_cats++] = c;
  };

  if (d.type == EvType::Cancel) {
    add(Category::Cancels);
    return;
  }

  uint64_t traded = 0;
  bool stp = false;
  for (const auto& e : oracle_outs) {
    if (e.type == OutType::Trade) traded += e.qty;
    if (e.type == OutType::CancelAck && e.maker.session_id != 0) stp = true;
  }

  switch (d.o.tif) {
    case TIF::Market: add(Category::MarketOrders); break;
    case TIF::IOC: add(Category::Ioc); break;
    case TIF::FOK: add(Category::Fok); break;
    case TIF::Day: break;
  }
  if (traded > 0) add(Category::PriceTimePriority);
  if (traded > 0 && traded < d.o.qty) add(Category::PartialFills);
  if (stp) add(Category::SelfTradePrevention);
}

struct ReplayCounters {
  uint64_t exercised[static_cast<uint32_t>(Category::Count)] = {};
  bool failed_in[static_cast<uint32_t>(Category::Count)] = {};
};

// Replays events[begin, end) on fresh engines, renumbering seq from zero so the
// range stands alone as a reproducer.
RunFailure replay(const std::vector<WireEvent>& events, uint64_t begin, uint64_t end,
                  const EngineSource& submission, const EngineSource& oracle,
                  const VerifyOptions& opts, ReplayCounters* counters,
                  const Clock::time_point* deadline) {
  RunFailure fail;
  std::unique_ptr<IMatchingEngine> sub(submission.create());
  std::unique_ptr<IMatchingEngine> ref(oracle.create());

  std::vector<OutEvent> sub_out, ref_out;
  sub_out.reserve(512);
  ref_out.reserve(512);
  CaptureSink sub_sink(sub_out), ref_sink(ref_out);

  InvariantChecker invariants;
  // The oracle is held to the same laws as the submission. If the reference
  // ever violates one, that is a PLATFORM bug and must be shouted about — the
  // alternative is reporting it as the participant's failure, or not at all.
  // This is also what makes "layer 3 catches bugs the reference shares" a claim
  // rather than an aspiration.
  InvariantChecker oracle_invariants;
  HashSink digest_sink;
  uint64_t output_index = 0;
  Category cats[8];
  uint32_t n_cats = 0;

  for (uint64_t i = begin; i < end; ++i) {
    if (deadline && (i - begin) % 4096 == 0 && Clock::now() > *deadline) {
      fail.failed = true;
      fail.outcome = VerifyOutcome::Timeout;
      fail.events_processed = i - begin;
      return fail;
    }

    const DecodedEvent d = decode(events[i], i - begin);
    sub_out.clear();
    ref_out.clear();
    dispatch(*ref, d, ref_sink);
    dispatch(*sub, d, sub_sink);

    if (counters) classify(d, ref_out, counters->exercised, cats, n_cats);

    const size_t n = sub_out.size() < ref_out.size() ? sub_out.size() : ref_out.size();
    for (size_t j = 0; j < n; ++j) {
      if (!same(ref_out[j], sub_out[j])) {
        fail.failed = true;
        fail.outcome = VerifyOutcome::OutputDiverged;
        fail.have_expected = fail.have_actual = true;
        fail.expected = ref_out[j];
        fail.actual = sub_out[j];
        fail.output_index = output_index + j;
        break;
      }
    }
    if (!fail.failed && sub_out.size() != ref_out.size()) {
      fail.failed = true;
      fail.outcome = VerifyOutcome::OutputDiverged;
      fail.output_index = output_index + n;
      if (ref_out.size() > n) {
        fail.have_expected = true;
        fail.expected = ref_out[n];
        fail.message = "the submission emitted nothing more for this event";
      } else {
        fail.have_actual = true;
        fail.actual = sub_out[n];
        fail.message = "the submission emitted an output the spec does not call for";
      }
    }

    if (fail.failed) {
      fail.event_index = i;
      fail.in_seq = d.o.seq;
      fail.events_processed = i - begin;
      if (counters) {
        for (uint32_t c = 0; c < n_cats; ++c) {
          counters->failed_in[static_cast<uint32_t>(cats[c])] = true;
        }
      }
      return fail;
    }

    if (std::string err; !oracle_invariants.on_event(d, ref_out, err)) {
      fail.failed = true;
      fail.outcome = VerifyOutcome::OracleViolatedInvariant;
      fail.event_index = i;
      fail.in_seq = d.o.seq;
      fail.message = "THE REFERENCE violated an invariant: " + err +
                     " — this is a platform bug, not a submission bug";
      fail.events_processed = i - begin;
      return fail;
    }

    // Layer 3 runs on the submission's own output, independent of the oracle.
    if (std::string err; !invariants.on_event(d, sub_out, err)) {
      fail.failed = true;
      fail.outcome = VerifyOutcome::InvariantViolated;
      fail.event_index = i;
      fail.in_seq = d.o.seq;
      fail.output_index = output_index;
      fail.message = err;
      fail.events_processed = i - begin;
      if (counters) {
        for (uint32_t c = 0; c < n_cats; ++c) {
          counters->failed_in[static_cast<uint32_t>(cats[c])] = true;
        }
      }
      return fail;
    }

    for (const auto& e : sub_out) digest_sink.emit(e);
    output_index += sub_out.size();

    const bool at_snapshot = opts.snapshot_every && ((i - begin + 1) % opts.snapshot_every == 0);
    if (at_snapshot || i + 1 == end) {
      BookSnapshot a{}, b{};
      ref->snapshot(a);
      sub->snapshot(b);
      if (std::string what; !same(a, b, what)) {
        fail.failed = true;
        fail.outcome = VerifyOutcome::SnapshotDiverged;
        fail.event_index = i;
        fail.in_seq = d.o.seq;
        fail.output_index = output_index;
        fail.message = "book snapshots disagree on " + what +
                       " — trades matched, but the books behind them did not";
        fail.events_processed = i - begin;
        return fail;
      }
      if (std::string err; !oracle_invariants.on_snapshot(a, err)) {
        fail.failed = true;
        fail.outcome = VerifyOutcome::OracleViolatedInvariant;
        fail.event_index = i;
        fail.in_seq = d.o.seq;
        fail.message = "THE REFERENCE's book disagrees with its own output: " + err;
        fail.events_processed = i - begin;
        return fail;
      }
      if (std::string err; !invariants.on_snapshot(b, err)) {
        fail.failed = true;
        fail.outcome = VerifyOutcome::InvariantViolated;
        fail.event_index = i;
        fail.in_seq = d.o.seq;
        fail.output_index = output_index;
        fail.message = err;
        fail.events_processed = i - begin;
        return fail;
      }
    }
  }

  fail.events_processed = end - begin;
  fail.digest = digest_sink.digest();
  return fail;
}

const char* category_name(Category c) {
  switch (c) {
    case Category::PriceTimePriority: return "price-time priority";
    case Category::PartialFills: return "partial fills";
    case Category::Cancels: return "cancels";
    case Category::MarketOrders: return "market orders";
    case Category::Ioc: return "IOC";
    case Category::Fok: return "FOK";
    case Category::SelfTradePrevention: return "self-trade prevention";
    case Category::Count: break;
  }
  return "?";
}

}  // namespace

const char* outcome_name(VerifyOutcome o) {
  switch (o) {
    case VerifyOutcome::Passed: return "passed";
    case VerifyOutcome::OutputDiverged: return "output_diverged";
    case VerifyOutcome::SnapshotDiverged: return "snapshot_diverged";
    case VerifyOutcome::InvariantViolated: return "invariant_violated";
    case VerifyOutcome::Timeout: return "timeout";
    case VerifyOutcome::OracleViolatedInvariant: return "oracle_violated_invariant";
  }
  return "?";
}

VerifyResult verify(const std::vector<WireEvent>& events, const EngineSource& submission,
                    const EngineSource& oracle, const VerifyOptions& opts) {
  const auto started = Clock::now();
  const Clock::time_point deadline =
      started + std::chrono::milliseconds(static_cast<int64_t>(opts.wall_time_s * 1000.0));
  const Clock::time_point* deadline_p = opts.wall_time_s > 0 ? &deadline : nullptr;

  VerifyResult r;
  r.total_events = events.size();

  ReplayCounters counters;
  RunFailure f = replay(events, 0, events.size(), submission, oracle, opts, &counters, deadline_p);
  r.events_processed = f.events_processed;

  for (uint32_t c = 0; c < static_cast<uint32_t>(Category::Count); ++c) {
    r.progress.push_back(CategoryProgress{category_name(static_cast<Category>(c)),
                                          counters.exercised[c], counters.failed_in[c]});
  }

  r.digest = f.digest;
  if (!f.failed) {
    r.elapsed_s = std::chrono::duration<double>(Clock::now() - started).count();
    return r;
  }

  r.outcome = f.outcome;
  r.in_seq = f.in_seq;
  r.output_index = f.output_index;
  r.have_expected = f.have_expected;
  r.have_actual = f.have_actual;
  r.expected = f.expected;
  r.actual = f.actual;
  r.message = f.message;

  if (f.outcome == VerifyOutcome::Timeout) {
    r.elapsed_s = std::chrono::duration<double>(Clock::now() - started).count();
    return r;
  }

  uint64_t begin = 0;
  uint64_t end = f.event_index + 1;

  if (opts.shrink) {
    // Shrinking replays prefixes repeatedly, so give it its own budget rather
    // than letting it eat the whole wall-clock limit.
    const Clock::time_point shrink_deadline = Clock::now() + std::chrono::seconds(10);
    auto diverges = [&](uint64_t b, uint64_t e) {
      return replay(events, b, e, submission, oracle, opts, nullptr, &shrink_deadline).failed;
    };

    // Smallest prefix that still fails. Usually collapses hard.
    uint64_t lo = 1, hi = end;
    while (lo < hi && Clock::now() < shrink_deadline) {
      const uint64_t mid = lo + (hi - lo) / 2;
      if (diverges(0, mid)) {
        hi = mid;
      } else {
        lo = mid + 1;
      }
    }
    if (Clock::now() < shrink_deadline) end = hi;

    // Then drop as much of the front as the failure survives. Renumbering the
    // seqs changes time priority, so this can stop reproducing — that is why it
    // is a search and not an assumption.
    uint64_t blo = 0, bhi = end - 1;
    while (blo < bhi && Clock::now() < shrink_deadline) {
      const uint64_t mid = blo + (bhi - blo + 1) / 2;
      if (diverges(mid, end)) {
        blo = mid;
      } else {
        bhi = mid - 1;
      }
    }
    begin = blo;
  }

  r.reproducer_begin = begin;
  r.reproducer_end = end;
  r.reproducer.assign(events.begin() + static_cast<int64_t>(begin),
                      events.begin() + static_cast<int64_t>(end));
  r.elapsed_s = std::chrono::duration<double>(Clock::now() - started).count();
  return r;
}

std::string format_report(const VerifyResult& r) {
  std::ostringstream s;

  if (r.outcome == VerifyOutcome::Timeout) {
    s << "TIMEOUT after " << r.events_processed << " of " << r.total_events << " events ("
      << r.elapsed_s << "s)\n\n"
      << "This is a liveness failure, not a wrong answer: the engine stopped making\n"
      << "progress. Look for an unbounded loop, not for a matching bug.\n";
    return s.str();
  }

  if (r.passed()) {
    s << "PASSED  " << r.total_events << " events, " << r.elapsed_s << "s\n\n";
  } else {
    switch (r.outcome) {
      case VerifyOutcome::OutputDiverged:
        s << "divergence at output #" << r.output_index << " (input seq " << r.in_seq << ")\n\n";
        break;
      case VerifyOutcome::SnapshotDiverged:
        s << "book divergence after input seq " << r.in_seq << "\n\n";
        break;
      case VerifyOutcome::InvariantViolated:
        s << "invariant violated at input seq " << r.in_seq << "\n\n";
        break;
      default: break;
    }

    if (!r.reproducer.empty()) {
      const uint64_t at = r.in_seq >= r.reproducer_begin ? r.in_seq - r.reproducer_begin : 0;
      if (at < r.reproducer.size()) {
        s << "  input:    " << describe_in(r.reproducer[at], at) << "\n";
      }
    }
    if (r.have_expected) s << "  expected: " << describe_out(r.expected) << "\n";
    if (r.have_actual) s << "  actual:   " << describe_out(r.actual) << "\n";
    if (!r.message.empty()) s << "  " << r.message << "\n";
    s << "\n";

    if (!r.reproducer.empty()) {
      s << "reproducer: " << r.reproducer.size() << " events (original stream index "
        << r.reproducer_begin << ".." << r.reproducer_end << ")\n";
      const size_t limit = 20;
      const size_t n = r.reproducer.size();
      for (size_t i = 0; i < n; ++i) {
        if (n > limit && i == limit / 2) {
          s << "    ... " << (n - limit) << " more ...\n";
          i = n - limit / 2 - 1;
          continue;
        }
        s << "    " << describe_in(r.reproducer[i], i) << "\n";
      }
      s << "\n";
    }
  }

  s << "progress:\n";
  for (const auto& p : r.progress) {
    const char* mark = p.failed ? "X" : (p.exercised ? "OK" : "-");
    s << "  " << (p.failed ? "[X ]" : (p.exercised ? "[OK]" : "[  ]")) << "  " << p.name << "  ("
      << p.exercised << " events)";
    (void)mark;
    s << "\n";
  }
  return s.str();
}

namespace {

std::string json_escape(const std::string& in) {
  std::string out;
  for (char c : in) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      default: out += c;
    }
  }
  return out;
}

void json_out_event(std::ostringstream& s, const OutEvent& e) {
  s << "{\"type\":\"" << type_name(e.type) << "\",\"in_seq\":" << e.in_seq << ",\"px\":" << e.px
    << ",\"qty\":" << e.qty << ",\"maker_session\":" << e.maker.session_id
    << ",\"maker_coid\":" << e.maker.client_order_id
    << ",\"maker_firm\":" << e.maker.participant_id
    << ",\"taker_session\":" << e.taker.session_id
    << ",\"taker_coid\":" << e.taker.client_order_id
    << ",\"taker_firm\":" << e.taker.participant_id << ",\"reason\":\"" << reason_name(e.reason)
    << "\"}";
}

}  // namespace

std::string format_json(const VerifyResult& r) {
  std::ostringstream s;
  s << "{\"outcome\":\"" << outcome_name(r.outcome) << "\",\"passed\":" << (r.passed() ? 1 : 0)
    << ",\"digest\":\"" << std::hex << r.digest << std::dec << "\""
    << ",\"events_processed\":" << r.events_processed << ",\"total_events\":" << r.total_events
    << ",\"elapsed_s\":" << r.elapsed_s;

  if (!r.passed() && r.outcome != VerifyOutcome::Timeout) {
    s << ",\"in_seq\":" << r.in_seq << ",\"output_index\":" << r.output_index
      << ",\"message\":\"" << json_escape(r.message) << "\"";
    if (r.have_expected) {
      s << ",\"expected\":";
      json_out_event(s, r.expected);
    }
    if (r.have_actual) {
      s << ",\"actual\":";
      json_out_event(s, r.actual);
    }
    s << ",\"reproducer_begin\":" << r.reproducer_begin
      << ",\"reproducer_end\":" << r.reproducer_end << ",\"reproducer\":[";
    for (size_t i = 0; i < r.reproducer.size(); ++i) {
      const WireEvent& w = r.reproducer[i];
      s << (i ? "," : "") << "{\"seq\":" << i << ",\"type\":\""
        << (w.type == EvType::New ? "NEW" : "CANCEL") << "\",\"side\":\""
        << (w.side == Side::Buy ? "buy" : "sell") << "\",\"tif\":\"" << tif_name(w.tif)
        << "\",\"px\":" << w.px << ",\"qty\":" << w.qty << ",\"session\":" << w.session_id
        << ",\"coid\":" << w.client_order_id << ",\"firm\":" << w.participant_id << "}";
    }
    s << "]";
  }

  s << ",\"progress\":[";
  for (size_t i = 0; i < r.progress.size(); ++i) {
    s << (i ? "," : "") << "{\"area\":\"" << json_escape(r.progress[i].name)
      << "\",\"exercised\":" << r.progress[i].exercised
      << ",\"failed\":" << (r.progress[i].failed ? 1 : 0) << "}";
  }
  s << "]}";
  return s.str();
}

}  // namespace mebench::harness
