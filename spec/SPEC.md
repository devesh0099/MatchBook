# Matching Engine Challenge — Specification

Version 1.0. This document is normative. Where the reference implementation and this
document disagree, this document wins and the reference is a bug.

You implement `create_engine()` in a single translation unit, `engine.cpp`. Your engine
receives decoded order events and emits output events into a sink. You are graded in two
stages: a **correctness gate** (binary — pass or fail) and, once passed, a **latency
benchmark** (ranked on p50 per-order latency).

---

## 1. Terminology

**Cancel, never delete.** This specification uses *cancel* for removing a resting order
from the book, matching `EvType::Cancel` and `OutType::CancelAck`. If you have read
Nasdaq ITCH or similar exchange documentation, you may know a distinction between *Order
Delete* (full removal) and *Order Cancel* (partial quantity reduction). **That
distinction does not exist here.** There is one removal operation and it is called
cancel. It always removes the entire remaining quantity.

**Cancel identity is a pair, not an id.**

> Cancel removes a resting order identified by the `(session_id, client_order_id)` pair.
> Client order IDs are unique only within a session; two sessions may use the same value
> concurrently.

This matters. An `unordered_map<uint64_t, Order*>` keyed on `client_order_id` alone will
appear to work and then cancel the wrong order. The stream deliberately contains
concurrent duplicate `client_order_id` values across sessions.

**Firm vs session.** `participant_id` identifies the *firm*. `session_id` identifies the
*connection*. One firm may have several sessions. Self-trade prevention keys on
`participant_id`; cancel identity keys on `session_id`. They are different fields with
different jobs, and neither substitutes for the other.

---

## 2. Data contracts

### 2.1 Hot-path structs

These are what your engine sees. They are frozen; the headers are read-only and shipped
with the boilerplate verbatim.

```cpp
struct OrderRef {                    // 16 bytes, pass by value
  uint64_t client_order_id;
  uint16_t session_id;
  uint16_t participant_id;
  uint32_t _pad;
  bool operator==(const OrderRef&) const = default;
};

struct Order {                       // 32 bytes, naturally aligned
  uint64_t seq;                      // global sequence — use for time priority
  uint64_t client_order_id;
  int32_t  px;
  uint32_t qty;
  uint16_t session_id;
  uint16_t participant_id;
  Side     side;
  TIF      tif;
  uint16_t _pad;

  OrderRef ref() const { return {client_order_id, session_id, participant_id, 0}; }
};
static_assert(sizeof(Order) == 32 && alignof(Order) == 8);
```

`seq` is the global arrival sequence number. **Time priority is defined on `seq`, not on
any clock.** There is no timestamp anywhere in this contest.

Prices are `int32_t` **integer ticks**. There are no floating-point prices. Do not
introduce any; `double` arithmetic with FMA contraction produces build-dependent results
and would make your submission's correctness depend on the compiler's mood.

### 2.2 Enumerations

```cpp
enum class EvType : uint8_t { New = 0, Cancel = 1 };
enum class Side   : uint8_t { Buy = 0, Sell = 1 };
enum class TIF    : uint8_t { Day = 0, IOC = 1, FOK = 2, Market = 3 };

enum class OutType : uint8_t {
  Trade = 0, Ack = 1, Reject = 2, CancelAck = 3, Expired = 4
};

enum class RejectReason : uint8_t {
  None = 0, UnknownOrder = 1, FokUnfillable = 2
};
```

There is **no `Modify`** event. There is no cancel-replace. Do not implement one.

`RejectReason` has exactly three values and every one is reachable by a rule below. There
is no `SelfTrade` reason (STP never rejects — see S4), no `DuplicateId` and no
`InvalidQty` (the generator cannot produce those inputs — see §2.5).

### 2.3 Output events

```cpp
struct OutEvent {                    // 56 bytes
  uint64_t in_seq;                   // which input event caused this
  OrderRef maker;                    // zeroed for non-trade events
  OrderRef taker;
  int32_t  px;
  uint32_t qty;
  OutType  type;
  Side     aggressor_side;
  RejectReason reason;
  uint8_t  _pad[5];
};
static_assert(sizeof(OutEvent) == 56);

class OutSink {
public:
  virtual void emit(const OutEvent&) noexcept = 0;
  virtual ~OutSink() = default;
};
```

Field rules for every emitted event:

| Field | Rule |
|---|---|
| `in_seq` | The `seq` of the input event being processed. Always set. |
| `maker` | The resting order, on `Trade` and on STP `CancelAck`. Zeroed otherwise. |
| `taker` | The order named by the input event: the aggressor on `Trade`, the subject order on `Ack` / `Reject` / `Expired`, and the cancelled order on an ordinary `CancelAck`. |
| `px` | `Trade`: the **resting** order's price. `Ack`: the order's own price (`0` for market). `Expired` / `CancelAck` / `Reject`: `0`. |
| `qty` | `Trade`: quantity traded. `Expired`: quantity expiring. `CancelAck`: the **remaining** quantity of the cancelled order. `Ack` / `Reject`: `0`. |
| `aggressor_side` | The side of the order named by the input event. On a `Cancel` input, the side of the resting order being cancelled; on a `Reject`/`UnknownOrder` where no such order exists, `Side::Buy`. |
| `reason` | `RejectReason::None` on everything except `Reject`. |
| `_pad` | Zeroed. |

"Zeroed" means all bytes zero. The digest that verifies your run folds every field, so a
garbage `maker` on an `Ack` is a correctness failure like any other.

### 2.4 Engine interface

```cpp
class IMatchingEngine {
public:
  virtual void on_new(const Order& o, OutSink& out) noexcept = 0;
  virtual void on_cancel(OrderRef ref, uint64_t seq, OutSink& out) noexcept = 0;

  // Called outside the timed region — correctness snapshots only.
  virtual void snapshot(BookSnapshot& out) const = 0;
  virtual ~IMatchingEngine() = default;
};

IMatchingEngine* create_engine();    // you implement this
```

`on_new` and `on_cancel` are `noexcept`. Do not throw. Do not allocate in a way that can
throw `std::bad_alloc` on the hot path if you can avoid it — but note that if you do
throw, `noexcept` terminates the process and your run fails.

`snapshot` is called **outside** the timed region. It costs you nothing at benchmark
time, so implement it straightforwardly; it is what catches a silently dropped resting
order.

```cpp
struct LevelSnapshot {
  int32_t  px;
  uint64_t total_qty;
  uint32_t order_count;
  uint64_t front_seq;      // seq of the order at the head of the queue
};

struct BookSnapshot {
  uint64_t at_seq;
  uint32_t n_bids, n_asks;
  LevelSnapshot bids[16];  // best first
  LevelSnapshot asks[16];
  uint64_t resting_qty_total;
  uint32_t resting_order_count;
};
```

Fill at most 16 levels per side, **best price first** (highest price first for bids,
lowest first for asks). Set `n_bids`/`n_asks` to the number of levels actually written,
capped at 16. `resting_qty_total` and `resting_order_count` cover the **whole** book, not
just the levels written. `front_seq` is the `seq` of the order at the head of that level's
queue — this is what proves your level queue is FIFO and not LIFO.

### 2.5 Input guarantees

The stream generator guarantees the following. You need not defend against these cases,
and there is no reject reason for them:

- A `New` never carries a `(session_id, client_order_id)` that duplicates a **currently
  live** order in the same session. (The same pair may be reused after the earlier order
  is gone, and the same `client_order_id` **will** appear concurrently in *different*
  sessions.)
- A `New` never carries zero quantity.
- A market order (`TIF::Market`) always carries `px == 0`.

Everything else is fair game, including cancels for orders that were already fully filled
and cancels for orders that never existed.

---

## 3. Matching rules

### 3.1 Core

- **Trade price is the resting order's price. Always.** Not the aggressor's, not a
  midpoint. This is the single most common first-submission failure.
- **Priority is strict price-time FIFO.** Best price first; within a price level, lowest
  `seq` first. "Time" means arrival sequence, not a timestamp.
- **Emission order is book-walk order:** one `Trade` per maker consumed, in the order the
  makers are consumed.
- A marketable limit order matches as far as its limit price allows, then **rests its
  remainder silently** (no extra output — the `Ack` already covered acceptance).
- A non-marketable limit order rests entirely and emits only its `Ack`.

### 3.2 Acknowledgement — ack on receipt (A1–A4)

`Ack` means exactly one thing everywhere in this specification: **the order was
accepted.** This matches FIX-style venue behaviour (`ExecType=New` before any fills).

1. **A1.** Every accepted `New` emits exactly one `Ack`, **before any other output for
   that event** — limit, IOC, FOK (accepted path), and market alike.
2. **A2.** After the `Ack`: trades in match order, interleaved STP `CancelAck`s per S5,
   then one `Expired` for any IOC or market remainder.
3. **A3.** A rejected order emits **only** the `Reject` — never an `Ack`. Ack means
   accepted; rejection is the opposite of acceptance. Concretely: an unfillable FOK emits
   exactly one `Reject`/`FokUnfillable` and nothing else.
4. **A4.** Canonical sequences:

   | Input | Output sequence |
   |---|---|
   | Non-marketable limit | `Ack` |
   | Marketable limit, fully filled | `Ack, Trade…` |
   | Marketable limit, partial (remainder rests) | `Ack, Trade…` |
   | IOC, some liquidity | `Ack, Trade…, Expired` |
   | IOC, nothing matched | `Ack, Expired` |
   | Market | same shapes as IOC |
   | FOK, fillable | `Ack, Trade…` |
   | FOK, unfillable | `Reject` (reason `FokUnfillable`) |
   | Cancel of a live order | `CancelAck` |
   | Cancel of an unknown or already-filled order | `Reject` (reason `UnknownOrder`) |

### 3.3 Market orders (M1–M5)

1. **M1.** A market order is `px = 0`, `TIF::Market`, and is **never rested**. There are
   no `INT32_MIN`/`INT32_MAX` price sentinels; do not invent any, they invite overflow
   bugs in comparison code.
2. **M2.** It matches against the opposite side at **each level's resting price**, best
   price first, price-time order within a level, until its quantity is exhausted or the
   opposite side is empty.
3. **M3.** Any unfilled remainder emits exactly one `Expired` carrying the remaining
   quantity — including the fully-unfilled case: a market order into an empty opposite
   side emits `Ack, Expired` and nothing else.
4. **M4.** A market order acks on receipt like every order (A1). It never rests and never
   emits `Reject` for lack of liquidity — an empty opposite side yields `Ack, Expired`.
5. **M5.** Net effect: market ≡ IOC at unlimited price. The event sequences are identical
   in shape.

### 3.4 Cancel

- **Cancel of a live order** removes it entirely and emits `CancelAck` carrying its
  **remaining** quantity. For a partially filled order that is the *remainder*, not the
  original quantity. This is the only case where a cancel carries a meaningful quantity.
- **Cancel of an unknown or already-filled `(session_id, client_order_id)`** emits
  `Reject` with `UnknownOrder`. Not silent, not fatal. The engine cannot distinguish
  "never existed" from "already fully filled" without keeping a graveyard of every order
  it has ever seen, and this specification does not require one.
- Cancel of an order that was removed by self-trade prevention behaves the same way: it is
  no longer live, so `Reject`/`UnknownOrder`.

### 3.5 Self-trade prevention — cancel resting (S1–S5)

1. **S1.** Self-trade is detected on `participant_id` equality between the aggressor and
   the resting order. `session_id` is irrelevant to STP: a firm crossing itself across two
   different sessions is still a self-trade.
2. **S2.** When the aggressor reaches a resting order with the same `participant_id`: the
   resting order is removed from the book, a `CancelAck` is emitted carrying the resting
   order's **remaining** quantity, and the aggressor **continues matching** at the same
   level and beyond with its quantity unchanged. No trade occurs between the pair.
3. **S3.** STP never rejects the aggressor and never reduces the aggressor's quantity.
4. **S4.** No rule emits a self-trade rejection; there is no such `RejectReason`.
5. **S5.** Emission ordering is book-walk order. If the walk is `Trade(B), CancelAck(own),
   Trade(C)`, that is the exact output order — the STP `CancelAck` appears interleaved at
   the position where the resting order was encountered.

On an STP `CancelAck`, `maker` is the removed resting order and `taker` is the aggressor
that triggered it; `in_seq` is the aggressor's `seq`.

### 3.6 Fill-or-kill (F1–F4)

FOK is the heavy item in this contest. It requires walking the book to check fillability
**before mutating anything**. A greedy implementation that matches and then discovers it
cannot finish has no way to undo. Build a dry-run pass.

1. **F1.** Fillability is computed by a **read-only walk of the opposite side before any
   mutation**: sum the remaining quantity of resting orders whose `participant_id`
   **differs** from the aggressor's, best price first, bounded by the FOK order's limit
   price.
2. **F2.** If that sum < the FOK order's quantity: emit exactly one `Reject` with
   `FokUnfillable`. The book is untouched — no trades, no STP cancellations, no partial
   effects of any kind, and no `Ack`.
3. **F3.** If that sum ≥ the FOK order's quantity: commit. Same-firm resting orders
   encountered during the commit are handled by S1–S5 (removed with `CancelAck`, no
   trade). Because they were excluded from the fillability count, STP cancellations can
   never cause a shortfall — "check then commit" stays atomic without a rollback path.
4. **F4.** A FOK order never emits `Expired` and never partially fills. Its only outcomes
   are: accepted and fully filled (`Ack, Trade…`, possibly with interleaved STP
   `CancelAck`s per S5), or a single `Reject`/`FokUnfillable` with no `Ack`.

**Worked example for F1.** Firm A sends FOK buy 100 @ 10000. The ask side holds 60 from
firm B and 50 from firm A, both at 10000.

- Counting all 110 available → commit → STP cancels A's own 50 mid-match → only 60 fills.
  A partially filled FOK. This violates FOK's entire promise.
- Excluding same-firm liquidity → 60 available < 100 → clean `Reject`/`FokUnfillable`,
  book untouched, A's resting 50 still there.

The second is correct.

**F1 is a specification invention, not industry practice.** Real venues offer several STP
variants (cancel-newest, cancel-both, decrement-and-cancel), and under the common
cancel-aggressor policies this corner never arises. Nobody arrives knowing this rule from
exchange experience. It is the resolution forced by cancel-resting STP plus FOK
atomicity; the alternatives are partial FOK fills or mandatory rollback machinery. The
hidden stream contains an injected case that distinguishes F1 from the naive count.

### 3.7 IOC × STP (I1)

1. **I1.** IOC matches under S1–S5 like any aggressor; whatever remains after the walk —
   **including** quantity that met only same-firm liquidity, now cancelled — emits one
   `Expired`.

---

## 4. Correctness

Three layers. Only layer 2 and layer 3 gate the leaderboard.

**Layer 1 — visible unit tests.** ~30 hand-written cases, one per rule above, shipped in
the boilerplate and run by the editor's **Run** button. These teach; they do not grade.
Passing all of them does not mean you pass the gate.

**Layer 2 — differential fuzzing (the gate).** A seeded stream is run through your engine
and the reference side by side and compared event by event. **The seed is fresh on every
submission**, so it cannot be reverse-engineered. The book is also snapshotted every N
events and diffed — a silently dropped resting order produces identical trades until it
suddenly does not, possibly millions of events later.

**Layer 3 — invariants.** These hold regardless of the oracle and catch bugs the
reference might share:

- The book is never crossed: `best_bid < best_ask`
- `Σ traded + Σ resting + Σ cancelled == Σ submitted`
- Every trade price lies between both orders' limit prices
- No zero-quantity or negative-quantity resting order
- No self-trade ever: `maker.participant_id != taker.participant_id` on every `Trade`
- Within a level, `front_seq` ordering matches arrival order

**Correctness is binary for leaderboard eligibility.** You will still see a progress
breakdown (`price-time ✓  partial fills ✓  IOC ✗`) so you know where you stand.

On failure you get the **first** divergence only, shrunk by binary search to a reproducer
usually under ten events:

```
divergence at output #8,341 (input seq 4,182)

  input:    NEW sell 100 @ 9998  session=3 coid=77
  expected: TRADE 100 @ 9999  maker=(s7,c12)  taker=(s3,c77)
  actual:   TRADE 100 @ 9998  maker=(s7,c12)  taker=(s3,c77)
                          ^^^ trade price should be the resting order's
```

Only the first is shown, deliberately. Fixing by pattern-matching against a list of 400
failures is not debugging.

A **timeout** is reported distinctly from a wrong-output failure. They are different bugs.

---

## 5. Benchmark and scoring

### 5.1 What is measured

```
Generate (offline)  →  Load + decode + warm up  →  [ TIMED LOOP ]  →  Verify + flush
```

Only the timed loop counts. Loading, decoding, page-touching, and warm-up all happen
before it. Verification and flushing happen after.

Warm-up is sized from the stream's own profile: the ranked profile builds a book of
roughly **300,000 resting orders across ~5,900 price levels**, and it takes about 3.9M
events to get there. Those events run through your engine — the digest covers them —
but they are not measured, so every ranked sample is taken against a book at full
depth. That book is far larger than any L3, which is the point: a layout that keeps
per-order footprint small is measurably faster here and was not at shallower depths.

```cpp
for (uint64_t i = 0; i < n; ++i) {
    uint64_t t0 = rdtscp();
    dispatch(engine, events[i], out);
    uint64_t t1 = rdtscp();
    hist.record(t1 - t0);
}
```

Two consequences of per-event timing are **deliberate** and stated so the measurement
model is explicit:

- **Constant probe overhead.** Each `rdtscp` pair adds a fixed cost of order tens of
  cycles. It is measured on the actual benchmark node with an empty engine and
  **published alongside your results**. A constant added to every sample preserves
  ordering; it slightly compresses relative gaps.
- **Per-event serialization.** `rdtscp` waits for prior instructions to retire, so the CPU
  cannot overlap event N's tail with event N+1's head. This contest measures **isolated
  per-event latency, not overlapped stream throughput.** Cross-event memory-level-
  parallelism tricks earn no credit — by definition of the metric.

### 5.2 Sink

The benchmark run uses a **checksum-only sink** costing a few nanoseconds, with no
volume-dependent memory traffic, so sink cost is identical for everyone and cancels in the
ranking. The checksum folds **every field** of every `OutEvent`, field by field:

```cpp
class HashSink : public OutSink {
  uint64_t h_ = 0xcbf29ce484222325ull;
public:
  void emit(const OutEvent& e) noexcept override {
    auto mix = [&](uint64_t v) { h_ = (h_ ^ v) * 0x100000001b3ull; };
    mix(e.in_seq);
    mix(e.maker.client_order_id);
    mix((uint64_t)e.maker.session_id << 16 | e.maker.participant_id);
    mix(e.taker.client_order_id);
    mix((uint64_t)e.taker.session_id << 16 | e.taker.participant_id);
    mix((uint64_t)(uint32_t)e.px << 32 | e.qty);
    mix((uint64_t)e.type << 16 | (uint64_t)e.aggressor_side << 8
        | (uint64_t)e.reason);
  }
  uint64_t digest() const noexcept { return h_; }
};
```

The digest from your benchmark run must equal the digest from your correctness run on the
same stream. **Verification happens inside the timed run**, which is what makes it
pointless to go faster by doing less work.

### 5.3 Metric

Two nested distributions:

- **Within one run:** ~10M per-event latencies → HdrHistogram → **p50 is ranked**; p99 and
  p99.9 are reported but unranked.
- **Across runs:** the run is repeated 7–10 times → **score = median of the per-run p50s**.

Per-event latencies are never averaged across runs; that would destroy the tail.

Ranking is on **p50, not p99**: sporadic hypervisor or thermal interference contaminates
tails but barely moves a median over 10M events. Mean is not used — a single 50 µs page
fault vanishes into it.

### 5.4 Presentation and ties

Results are presented as **bands against fixed thresholds**, not exact positions. A
bootstrap confidence interval is computed across your per-run medians; **overlapping
intervals means tied rank.** Everyone past the correctness gate wrote a working order
book, spreads are often under 2×, and defending the difference between rank 7 and rank 9
is not honestly possible.

The leaderboard **freezes at 5:15** (ICPC style) and is revealed at the end.

### 5.5 Rejudge

Every participant's final submission is **rejudged in one continuous block** on the same
machine in the same short window before results are announced. The live leaderboard during
the event is indicative; **the rejudge is authoritative.** A reference run brackets the
block on both sides; if the two disagree, the block is rerun.

### 5.6 Machine health

Two checks, both on signals your code cannot cause:

1. **Steal time** — `/proc/stat` delta across the run. Non-zero → the run is discarded and
   requeued **at the front** of the queue, not the back. You already waited once and it
   was not your fault.
2. **Reference spot check** — the reference implementation is run every ~20 minutes;
   >2% deviation from the morning baseline raises an alert.

Checks on context switches, page faults, and frequency are deliberately **not** applied:
your own engine can legitimately cause those (a heap-growing engine page-faults — that is
its latency, not contamination), and discarding on them would put you in an infinite
requeue loop.

---

## 6. Rules

- **You submit one file**: `engine.cpp`, a single translation unit. It may include the
  frozen headers and the C++20 standard library. Nothing else.
- **Compiler and flags are fixed and published**: pinned `g++`, `-O2 -march=<explicit>
  -fno-omit-frame-pointer`. Never `-march=native`. You cannot change flags, add pragmas
  that change optimization level, or ship a build system.
- **Integer prices only.** No floating point in matching logic.
- **Reading harness memory other than the events passed to your engine is
  disqualifying.** The decoded stream lives in the same address space as your engine; a
  determined participant could scan for it and prefetch future events. This is handled by
  rule, and enforced by source review of the top finishers.
- **No threads.** `clone` is blocked by the sandbox; thread-based gaming of wall clock is
  not a strategy here.
- **Submissions are plagiarism-checked** across the cohort.
- Your engine runs sandboxed (`ioi/isolate`) with wall-time, memory, and pid limits. An
  infinite loop, a runaway allocation, or a fork bomb fails your run cleanly and does not
  stall anyone else's.

### Rate limits

| Lane | Stream | Turnaround | Limit |
|---|---|---|---|
| **Run** (visible tests) | ~100k events | seconds | unlimited |
| **Submit** → correctness | 100k–500k events, fresh seed | seconds | unlimited |
| **Submit** → benchmark | 10M events, `cancel_heavy` (3.9M warm-up, 6.1M timed) | ~60 s | unlimited; at most 1 of yours queued at a time |

Rate limiting is checked **at enqueue**: correctness is always accepted, and the submit
response tells you immediately whether the benchmark will auto-queue and how long the
remaining wait is. Nothing is ever silently dropped.

The boilerplate ships the **generator** and a **local benchmark runner** so you can
measure locally without touching the queue. Local numbers will not match server numbers
in absolute terms — different machine — but they rank your own changes correctly, which
is what iteration needs.

---

## 7. Timeline

| Time | Phase |
|---|---|
| 0:00 | Kickoff, spec walkthrough (30 min, mandatory) |
| 0:30 | Coding opens, correctness lane live |
| 1:30 | Benchmark lane opens |
| 5:15 | Leaderboard freezes |
| 6:00 | Submissions close, final rejudge, reveal |

The benchmark lane opens an hour after coding does. Get it correct first; the leaderboard
is not accepting anything that has not passed the gate anyway.

---

## 8. A note on the environment

The benchmark node is tuned to be unrealistically quiet: isolated cores, no scheduler
tick, fixed frequency, no turbo, one job at a time. This is so the measurement is of
*your code* rather than of the room.

Real matching engines fight jitter, not just cycles. Do not leave this event thinking
production p99 looks like this. It does not.
