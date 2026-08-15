# Matching Engine Challenge — Specification

Version 1.1. Normative. Where the reference implementation and this document
disagree, this document wins and the reference is a bug.

You implement `create_engine()` in one translation unit, `engine.cpp`. Your engine
receives decoded order events and emits output events into a sink. Grading is two
stages: a **correctness gate** (pass or fail) and, once passed, a **latency benchmark**
ranked on p50 per-event latency.

Three definitions the rules depend on:

| Term | Meaning |
|---|---|
| **Cancel** | The only removal operation. Always removes the entire remaining quantity. There is no partial cancel and no cancel-replace. If you know the ITCH *Order Delete* / *Order Cancel* distinction, it does not exist here. |
| **Order identity** | The pair `(session_id, client_order_id)`. `client_order_id` is unique only within a session, and the stream deliberately reuses the same value across sessions concurrently. A map keyed on `client_order_id` alone will cancel the wrong order. |
| **Firm vs session** | `participant_id` is the firm; `session_id` is the connection. One firm may hold several sessions. Self-trade prevention keys on `participant_id`. Order identity keys on `session_id`. Neither substitutes for the other. |

---

## 1. Matching rules

### 1.1 Core

- **Trade price is the resting order's price. Always.** Not the aggressor's, not a
  midpoint.
- **Priority is strict price-time FIFO.** Best price first; within a price level, lowest
  `seq` first. "Time" means arrival sequence, not a clock — there is no timestamp
  anywhere in this contest.
- **Emission order is book-walk order:** one `Trade` per maker consumed, in the order the
  makers are consumed.
- A marketable limit order matches as far as its limit allows, then **rests its remainder
  silently** — the `Ack` already covered acceptance.
- A non-marketable limit order rests entirely and emits only its `Ack`.

### 1.2 Acknowledgement — ack on receipt (A1–A4)

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

### 1.3 Market orders (M1–M5)

1. **M1.** A market order is `price = 0`, `TIF::Market`, and is **never rested**. There are
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

### 1.4 Cancel

- **Cancel of a live order** removes it entirely and emits `CancelAck` carrying its
  **remaining** quantity. For a partially filled order that is the *remainder*, not the
  original quantity. This is the only case where a cancel carries a meaningful quantity.
- **Cancel of an unknown or already-filled order** emits `Reject` with `UnknownOrder`.
  Not silent, not fatal. Distinguishing "never existed" from "already filled" would need a
  graveyard of every order ever seen; this specification does not require one.
- Cancel of an order that was removed by self-trade prevention behaves the same way: it is
  no longer live, so `Reject`/`UnknownOrder`.

### 1.5 Self-trade prevention — cancel resting (S1–S5)

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

### 1.6 Fill-or-kill (F1–F4)

FOK requires checking fillability **before mutating anything**: a greedy implementation
that matches and then finds it cannot finish has no way to undo.

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
variants under which this corner never arises, so exchange experience will not tell you
this rule. It is what cancel-resting STP plus FOK atomicity force; the alternatives are
partial FOK fills or rollback machinery. The hidden stream contains an injected case that
separates F1 from the naive count.

### 1.7 IOC × STP (I1)

1. **I1.** IOC matches under S1–S5 like any aggressor; whatever remains after the walk —
   **including** quantity that met only same-firm liquidity, now cancelled — emits one
   `Expired`.

---

---

## 2. Input guarantees

The stream generator guarantees the following. You need not defend against these cases,
and there is no reject reason for them:

- A `New` never carries a `(session_id, client_order_id)` that duplicates a **currently
  live** order in the same session. (The same pair may be reused after the earlier order
  is gone, and the same `client_order_id` **will** appear concurrently in *different*
  sessions.)
- A `New` never carries zero quantity.
- A market order (`TIF::Market`) always carries `price == 0`.

Everything else is fair game, including cancels for orders that were already fully filled
and cancels for orders that never existed.

---

---

## 3. Benchmark and scoring

### 3.1 What is measured

```
Generate (offline)  →  Load + decode + warm up  →  [ TIMED LOOP ]  →  Verify + flush
```

Only the timed loop counts. Loading, decoding, page-touching, and warm-up all happen
before it. Verification and flushing happen after.

Warm-up is sized from the stream's own profile: the ranked profile builds a book of
roughly **750,000 resting orders across ~14,800 price levels**, and it takes about 9.7M
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

### 3.2 How you are scored

- **Within one run:** ~10.3M per-event latencies → HdrHistogram → **p50 is your score for
  that run**. p99 and p99.9 are reported but not ranked.
- **Across runs:** the run is repeated 9 times → **your score is the median of the nine
  per-run p50s**.

Per-event latencies are never averaged across runs.

Positions are exact, **sorted by score ascending**. You are ranked on your **best**
submission, not your latest — a later experiment that measures worse costs you nothing.
Identical scores break on the **earlier submission**: if two engines measure the same, the
one that got there first is placed above.

A confidence interval across your nine per-run medians is published beside each score. It
shows how separated a close pair really is; it does **not** affect rank.

## 4. Contest rules

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
| **Submit** → benchmark | 20M events, `cancel_heavy` (9.7M warm-up, 10.3M timed) | ~2 min | unlimited; at most 1 of yours queued at a time |

Rate limiting is checked **at enqueue**: correctness is always accepted, and the submit
response tells you immediately whether the benchmark will auto-queue and how long the
remaining wait is. Nothing is ever silently dropped.

What happens when you submit again while a benchmark of yours is outstanding depends on
what that job is doing:

- **Waiting in the queue** — the new submission replaces it and **keeps its place in the
  queue**. The old one is marked *superseded*: correct, but never timed.
- **Already running** — it is never interrupted. Your new submission is held and queues
  itself automatically when the running job finishes.

Either way you always end up with your newest correct code ranked, without resubmitting.
If several of yours are held, only the newest queues; the rest are superseded. Correctness
is never affected by any of this — it always runs, immediately, on every submission.

The boilerplate ships the **generator** and a **local benchmark runner** so you can
measure locally without touching the queue. Local numbers will not match server numbers
in absolute terms — different machine — but they rank your own changes correctly, which
is what iteration needs.

---

---

## 5. The environment

The benchmark node is tuned to be unrealistically quiet: isolated cores, no scheduler
tick, fixed frequency, no turbo, one job at a time. This is so the measurement is of
*your code* rather than of the room.

Real matching engines fight jitter, not just cycles. Do not leave this event thinking
production p99 looks like this. It does not.
