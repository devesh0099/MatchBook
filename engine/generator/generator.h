// generator/generator.h — deterministic stream generator.
//
// Determinism across platforms is the whole point: participants get the
// generator, not 240MB of files, and "same seed, same bytes" has to hold on
// Linux, macOS and the server.
//
// std::mt19937_64 is portable. THE DISTRIBUTIONS ARE NOT — libstdc++ and libc++
// disagree, so std::uniform_int_distribution, std::shuffle and std::sample are
// banned everywhere in this file. Everything is built from Rng::below().

#pragma once

#include <cstdint>
#include <deque>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

#include "mebench/wire.h"
#include "reference/reference.h"

namespace mebench::generator {

struct Rng {
  std::mt19937_64 r;

  explicit Rng(uint64_t seed) : r(seed) {}

  uint64_t next() { return r(); }

  // Unbiased enough and, more importantly, identical everywhere: take the high
  // 32 bits, multiply, take the high half.
  uint32_t below(uint32_t n) {
    return static_cast<uint32_t>(((next() >> 32) * static_cast<uint64_t>(n)) >> 32);
  }

  // Inclusive on both ends.
  uint32_t range(uint32_t lo, uint32_t hi) { return lo + below(hi - lo + 1); }

  bool chance(uint32_t pct) { return below(100) < pct; }
};

enum class AgentKind : uint8_t {
  MarketMaker,   // quotes both sides at the touch, cancels and re-quotes constantly
  Taker,         // bursts of marketable orders, drives sweeps
  Noise,         // deep orders that mostly rest, grows the book
  LevelStacker,  // repeated same-price adds, stresses one level's queue
};

struct ProfileParams {
  const char* name;
  uint32_t n_sessions;
  uint32_t n_firms;

  // Agent mix, as session counts drawn in round-robin order.
  uint32_t w_market_maker, w_taker, w_noise, w_stacker;

  // Share of events that are cancels. Real books are 90%+ cancels; a generator
  // dominated by inserts never exercises the path where the interesting
  // data-structure decisions live.
  uint32_t cancel_pct;

  // Placement weights, must sum to 100 (SPEC-side plan section 5.2).
  uint32_t p_marketable, p_at_touch, p_near, p_deep;

  // TIF mix among New events, as percentages; the remainder is GTC.
  uint32_t tif_ioc_pct, tif_fok_pct, tif_market_pct;

  // How far out, in ticks, a "deep" placement can sit. This is what spreads
  // depth across PRICE LEVELS rather than piling it into a handful of them.
  //
  // It used to be fixed at 50, which was fine at a few thousand resting orders
  // and wrong at a few hundred thousand: the whole book lived in ~100 levels,
  // every level held thousands of orders, and matching degenerated into walking
  // enormous FIFO queues. That measures list traversal, not the price index —
  // and a real book with this much size behind it is wide, not tall.
  //
  // Orders this far out never fill, because mid random-walks in a +/-50 band.
  // That is realistic and, since the generator now tracks resting orders
  // exactly, they stay cancellable rather than leaking depth forever.
  uint32_t deep_ticks;

  // How many orders a session is content to have live before it cancels in
  // preference to adding. This is the knob that sets book depth.
  //
  // Since the generator started running its own reference book, this number
  // means what it says: a session's live set holds only orders that are
  // genuinely resting, so back-pressure binds on real depth and the book
  // settles at roughly `n_sessions * live_target`. It used to bind on a list
  // that was ~96% stale, which is why depth was a small and unpredictable
  // fraction of that product.
  uint32_t live_target;
};

const ProfileParams& profile_params(Profile p);
bool parse_profile(const std::string& name, Profile& out);
const char* profile_name(Profile p);

// Hand-placed adversarial cases. Random generation essentially never produces
// these interleavings, so they are injected at known sequence numbers and
// asserted present afterwards.
enum class InjectionKind : uint8_t {
  CancelOfConsumedOrder,     // cancel at seq N for an order consumed at seq N-1
  DuplicateCoidAcrossSessions,  // two sessions using the same client_order_id
  SelfCrossAcrossSessions,   // one firm crossing itself through two sessions
  FokSameFirmExclusion,      // the 60/50/100 case that separates F1 from the naive count
  SweepExactlyFiveLevels,    // aggressor sized to clear 5 levels and rest the remainder
  ConsumeDeepLevelQueue,     // a level with 200 resting orders, then one order taking all
  CancelLastOrderAtLevel,    // level cleanup
};

struct InjectionRecord {
  InjectionKind kind;
  uint64_t payoff_seq;  // the seq whose OUTPUT proves the case fired
  uint32_t detail;      // kind-specific expected count (trades, qty, session id)
};

const char* injection_name(InjectionKind k);

// A session's genuinely-resting orders, in age order, with O(1) removal.
//
// Two requirements pull against each other. Removal is driven by the reference
// book's output and names an arbitrary order, so it has to be O(1) — a session
// holding a thousand live orders across a ten-million-event stream cannot
// afford a linear erase. Selection is age-biased, because re-quoting cancels the
// order you just placed, so the order of the list has to survive removal, which
// rules out swap-remove.
//
// Tombstones satisfy both: removal blanks a slot, and compaction runs only once
// the dead outnumber the living, so it is O(1) amortised. `slots` is the only
// thing ever iterated; the map is lookup-only, so no iteration order of an
// unordered container can leak into the stream.
class LiveSet {
 public:
  void add(uint64_t coid);
  void remove(uint64_t coid);

  // A live client_order_id, biased toward recent ones, or 0 if empty.
  uint64_t pick(Rng& rng, uint32_t recent_pct);

  uint32_t size() const { return alive_; }
  bool empty() const { return alive_ == 0; }

 private:
  void compact();

  std::vector<uint64_t> slots_;  // age-ordered; 0 means a tombstone
  std::unordered_map<uint64_t, uint32_t> pos_;
  uint32_t alive_ = 0;
};

class Generator {
 public:
  // `live_target_override` replaces the profile's live_target when non-zero.
  // Depth is now proportional to it, so the whole depth/latency sweep is this
  // one integer rather than a family of near-duplicate profiles.
  Generator(uint64_t seed, Profile profile, uint32_t live_target_override = 0);

  // Produces exactly `count` events. Injections are scheduled inside that
  // budget; they need roughly 250 events of room, so a stream under
  // kMinEventsForInjections carries none and validation says so.
  std::vector<WireEvent> generate(uint64_t count);

  const std::vector<InjectionRecord>& injections() const { return injections_; }
  uint64_t seed() const { return seed_; }
  Profile profile() const { return profile_; }
  uint32_t live_target() const { return live_target_; }

  // Resting orders in the generator's own book at the end of generate().
  uint32_t final_depth() const { return book_.resting_order_count(); }
  uint32_t final_levels() const { return book_.level_count(); }

  static constexpr uint64_t kMinEventsForInjections = 5000;

  // Injections rest far above the organic price band, where no organic buy can
  // reach them, and any injected aggressor is preceded by a sweep that clears
  // the side it needs empty. Both are required: the band protects injected
  // resting orders from organic aggressors, the sweep protects injected
  // aggressors from organic resting orders.
  static constexpr int32_t kInjectionPriceBase = 20000;

  // The reference price random-walks inside a narrow band. A wide band lets mid
  // wander permanently away from orders resting 9-50 ticks out, which then can
  // never fill and can only be cancelled while their session still remembers
  // them — a slow, unbounded depth leak. Keeping mid near the resting liquidity
  // keeps the whole book reachable.
  static constexpr int32_t kMidLow = 9950;
  static constexpr int32_t kMidHigh = 10050;

  // Every injection session id is at or above this. The organic mix uses ids
  // from 1 upward, so the two can never collide, and the stream statistics
  // exclude injection traffic — those events are synthetic and would otherwise
  // swamp the fill rate the checks are actually about.
  static constexpr uint16_t kFirstInjectionSession = 899;

 private:
  struct Session {
    uint16_t session_id;
    uint16_t participant_id;
    AgentKind kind;
    Rng rng;
    LiveSet live;  // client_order_ids the reference book says are resting
    uint64_t next_coid = 1;
    int32_t stack_px = 0;  // LevelStacker's chosen level
    uint32_t weight = 1;
  };

  void build_sessions();
  void step_mid();
  uint32_t pick_session();
  WireEvent emit_from(Session& s);
  WireEvent make_new(Session& s, Side side, int32_t price, uint32_t qty, TIF tif);
  WireEvent make_cancel(uint16_t session_id, uint16_t firm, uint64_t coid);
  int32_t place_price(Session& s, Side side, bool& marketable);

  // Feed one emitted event through the private book and fold the resulting
  // output back into the live sets.
  void apply(const WireEvent& e, uint64_t seq);
  Session* session_for(uint16_t session_id);

  void schedule_injections(uint64_t count);
  void enqueue_injection(InjectionKind kind, uint64_t at_seq);

  uint64_t seed_;
  Profile profile_;
  const ProfileParams* pp_;
  uint32_t live_target_;
  Rng rng_;  // sequencer + price walk only; sessions have their own streams
  int32_t mid_ = 10000;
  uint64_t seq_ = 0;

  std::vector<Session> sessions_;
  uint32_t total_weight_ = 0;

  // The generator's own copy of the book. EVERY emitted event goes through it,
  // injections included: the clearing sweeps consume thousands of organic
  // resting orders, and a book that did not see them would hand back exactly
  // the staleness this exists to remove.
  reference::ReferenceEngine book_;

  std::deque<WireEvent> pending_;  // an in-flight injection script
  std::vector<std::pair<uint64_t, InjectionKind>> schedule_;
  size_t next_scheduled_ = 0;
  std::vector<InjectionRecord> injections_;
};

// Stream file I/O. Format is StreamHeader followed by `event_count` WireEvents.
bool write_stream(const std::string& path, uint64_t seed, Profile profile, uint32_t live_target,
                  const std::vector<WireEvent>& events, std::string& err);
bool read_stream(const std::string& path, StreamHeader& header, std::vector<WireEvent>& events,
                 std::string& err);

// Read a stream from an already-open descriptor. Used for hidden streams: the
// parent opens the file and passes the fd before dropping privileges, so the
// sandboxed submission never sees a path it could open and no seed ever
// appears on the command line (plan section 10).
bool read_stream_fd(int fd, StreamHeader& header, std::vector<WireEvent>& events, std::string& err);

}  // namespace mebench::generator
