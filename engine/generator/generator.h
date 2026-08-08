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
#include <vector>

#include "mebench/wire.h"

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

  // TIF mix among New events, as percentages; the remainder is Day.
  uint32_t tif_ioc_pct, tif_fok_pct, tif_market_pct;

  // How many orders a session is content to have live before it cancels in
  // preference to adding. This is the knob that sets book depth: a session
  // that never pushes back adds faster than it cancels, and the book grows for
  // the whole run.
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

class Generator {
 public:
  Generator(uint64_t seed, Profile profile);

  // Produces exactly `count` events. Injections are scheduled inside that
  // budget; they need roughly 250 events of room, so a stream under
  // kMinEventsForInjections carries none and validation says so.
  std::vector<WireEvent> generate(uint64_t count);

  const std::vector<InjectionRecord>& injections() const { return injections_; }
  uint64_t seed() const { return seed_; }
  Profile profile() const { return profile_; }

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
    std::vector<uint64_t> live;  // client_order_ids BELIEVED resting
    uint64_t next_coid = 1;
    int32_t stack_px = 0;  // LevelStacker's chosen level
    uint32_t weight = 1;
  };

  void build_sessions();
  void step_mid();
  uint32_t pick_session();
  WireEvent emit_from(Session& s);
  WireEvent make_new(Session& s, Side side, int32_t px, uint32_t qty, TIF tif, bool likely_rests);
  WireEvent make_cancel(uint16_t session_id, uint16_t firm, uint64_t coid);
  int32_t place_price(Session& s, Side side, bool& marketable);

  void schedule_injections(uint64_t count);
  void enqueue_injection(InjectionKind kind, uint64_t at_seq);

  uint64_t seed_;
  Profile profile_;
  const ProfileParams* pp_;
  Rng rng_;  // sequencer + price walk only; sessions have their own streams
  int32_t mid_ = 10000;
  uint64_t seq_ = 0;

  std::vector<Session> sessions_;
  uint32_t total_weight_ = 0;

  std::deque<WireEvent> pending_;  // an in-flight injection script
  std::vector<std::pair<uint64_t, InjectionKind>> schedule_;
  size_t next_scheduled_ = 0;
  std::vector<InjectionRecord> injections_;
};

// Stream file I/O. Format is StreamHeader followed by `event_count` WireEvents.
bool write_stream(const std::string& path, uint64_t seed, Profile profile,
                  const std::vector<WireEvent>& events, std::string& err);
bool read_stream(const std::string& path, StreamHeader& header, std::vector<WireEvent>& events,
                 std::string& err);

// Read a stream from an already-open descriptor. Used for hidden streams: the
// parent opens the file and passes the fd before dropping privileges, so the
// sandboxed submission never sees a path it could open and no seed ever
// appears on the command line (plan section 10).
bool read_stream_fd(int fd, std::vector<WireEvent>& events, std::string& err);

}  // namespace mebench::generator
