# Book depth: plan, and what happened

Implemented. Section 7 is the result.

---

## 1. What we knew, measured

A cache-conscious engine exists (`engine/tests/engines/optimized.cpp`) — pool
allocation, flat level array, open-addressed cancel index — and it passes the
gate. So the ranked profile could finally be asked whether it discriminates.

| depth | naive size | reference p50 | optimized p50 | ratio |
|---|---|---|---|---|
| 23k orders | 3.1 MB | 130.3 ns | 40.1 ns | **3.25×** |
| 49k orders | 6.7 MB | 130.3 ns | 40.1 ns | **3.25×** |

Two facts, and the second was the problem:

- **It did discriminate.** 3.25× on the ranked number. The profile was not broken.
- **Depth was doing nothing.** Doubling the book changed neither engine by a
  single tick. Both sat inside this box's 16 MB L3, so both paid an L3 hit and
  the difference was structural (12 dependent loads vs 1–2), not
  memory-hierarchy.

While the book fits in cache, a cache-optimised layout and a naive one are
measured on everything except the thing that makes them different.

## 2. Why going deeper was expensive

Depth was not a free parameter. Measured relationship:

```
depth  ≈  sessions × live_target × 0.039
warmup ≈  sessions × live_target × 5 events
```

That **0.039** was the problem. A session's `live` list was what it *believed*
was resting, and 96% of it was stale — orders that had been filled, which the
generator could not observe because it ran no matching engine. So it carried 25×
more tracked entries than resting orders, and warm-up scaled with the tracked
count rather than the useful one. A 40M-event attempt timed out on this box.

**The blocker was never memory bandwidth. It was that depth was coupled to
warm-up through a 25× waste factor.**

## 3. What was built

The generator runs a private `ReferenceEngine` and drives each session's live
set from what that book actually emits:

- **Trade** → if the maker is no longer resting, remove it. Cross-session: a
  trade takes liquidity from whoever was on the other side, not from the sender.
- **CancelAck** → remove the order it names. Ordinary cancels name it in
  `taker`, STP cancels in `maker`.
- **New** → enters the set only if the book says it rested, which is the only
  way to be right about a marketable Day order that partly filled.

Every emitted event goes through the book, injections included — their clearing
sweeps consume thousands of organic orders, and a book that had not seen them
would recreate exactly the staleness this removes.

The live set is a tombstone vector: O(1) removal (driven by arbitrary orders
from the book) with age order preserved (needed for the recency-biased cancel
selection), compacted once the dead outnumber the living. Only the age-ordered
vector is ever iterated, so no unordered-container iteration order can leak into
the stream.

## 4. Three things this exposed that the plan did not predict

**Cancel percentage stopped being a free parameter.** In steady state every
resting order leaves by cancel or by fill, so cancel share is pinned near
`r/(1+r)` for a resting fraction r — about 37%. The profiles asked for 55%.
First run after the change: every live set drained to empty, hit rate 36%, depth
444. It had only ever "worked" because the list was 96% stale, so the excess
named nothing.

Cancel pressure now ramps with how full a session's live set is. **Depth is the
controlled variable and cancel share is the emergent one** — the right way
round, since depth is what the benchmark is about.

**Depth went tall rather than wide.** 200k orders in ~100 price levels means
2000-deep FIFO queues. That measures list traversal, not the price index, and no
real book has that shape. Deep placements now spread over a per-profile tick
range with density decaying from the touch (minimum of two draws — triangular,
no floating point, identical on every platform).

**The injections' clearing sweeps are market orders**, and a market order
accepts every price (SPEC M2), so each takes the *entire* organic ask side.
Invisible at 3k resting orders; half the book at 300k. They are now scheduled
across the stream's first third, where the book is filling anyway.

## 5. Result of the generator change

| | before | after |
|---|--:|--:|
| cancel-hit rate | 45% | **98%** |
| depth, 10M events | 23k | **12k–300k, set by one flag** |
| depth vs `live_target` | 0.039 × sessions × target | **0.62 × sessions × target** |
| warm-up per resting order | ~130 events | **~12 events** |

Depth is linear and flat across the whole stream. Same seed still produces the
same bytes, verified across `-O2` and `-O1 + ASan`.

## 6. Choosing the depth

Per-order footprint differs by engine, and that is the lever:

| | per order | 150k orders | 300k orders |
|---|---|---|---|
| reference (map + list nodes) | ~136 B | 20 MB | **41 MB** |
| optimized (pool + flat levels) | ~48 B + 3.1 MB fixed | 10 MB | **17 MB** |

This box: Zen 4, **16 MB L3**, L2 TLB **3584 × 4K = 14 MB of reach**. A 41 MB
naive book is 2.6× L3 and 2.9× TLB reach; the optimized one is borderline.

## 7. The gate: does the gap widen?

`cancel_heavy`, 6M timed events after a full warm-up, three independent passes
including one that interleaves the engines (this box has no fixed governor, so
back-to-back runs could manufacture a gap from clock drift).

| depth | naive size | reference p50 | optimized p50 | ratio |
|---|---|--:|--:|--:|
| 23k *(old profile)* | 3.1 MB | 130.3 ns | 40.1 ns | 3.25× |
| **50k** | 6.8 MB | 220.4 ns | 50.1 ns | **4.40×** |
| **150k** | 20 MB | 280.5 ns | 60.1 ns | **4.67×** |
| **300k** | 41 MB | 340.6 ns | 70.1 ns | **4.86×** |

Optimized read identically in all three passes at every depth; reference varied
by one quantum only at 50k. Reference grows **+55%** across the range, optimized
**+40%**; the absolute gap goes 170 ns → 270 ns.

**Verdict: pass.** Depth now does real work — it previously did nothing at all.

### Two caveats, stated rather than buried

**This box's `rdtscp` advances in 38-tick steps — exactly 10 ns.** Every value
above is an exact multiple: 50.1 ns is 5 quanta, 220.4 ns is 22, 340.6 ns is 34.
So the optimized engine's whole depth response is 5 → 6 → 7 quanta. That
resolves the direction but not the increment: gross ratios say 150k→300k widens
(4.67 → 4.86), net-of-probe says it narrows (5.40 → 5.33), and both differences
are inside one quantum. **Most of the discrimination is already present by
~50–100k.** The bench node — different L3, passes hygiene, possibly finer TSC —
settles the increment.

**`deep_ticks` cannot keep growing.** Levels saturate near 5,900, so past ~100k
the book thickens levels rather than adding them (3 orders/level at 12k, 49 at
291k). Asks rest at `mid + offset`, so a band beyond ~9,900 ticks collides with
the injection price base at 20,000. Going deeper than 300k would need the
injection band moved first.

## 8. What shipped

- `cancel_heavy` `live_target` = 1520 → **~300k resting orders across ~5,900
  price levels**.
- Warm-up is derived from the stream's own profile inside the harness, read from
  the stream header. A caller carrying its own constant would have timed ~3.9M
  events of book-filling in every ranked run; deriving it means the profile and
  the warm-up cannot drift apart, because there is only one of them.
- Depth is sampled 100 times per stream rather than 10 — a decile averages away
  a dip lasting a few percent of the run — and asserted from **both** sides,
  since bounded depth cannot see a collapse.
- `--live-target N` on `gen`, so the depth sweep is one integer rather than a
  family of near-duplicate profiles.

## 9. Open, for the bench node

- **The exact depth.** 300k is chosen; the L3 that justifies it is this box's.
  Re-run the sweep during the noise-floor run and confirm, or move it.
- **Measurement resolution.** If the bench node's TSC also quantises at 10 ns,
  the ranked p50 is a count of quanta and the ranking question (bands vs plain
  ranks vs ties) is settled by that, not by preference.
- **`kRecentCancelPct = 85`** means most timed cancels target recently placed —
  and therefore cache-warm — orders. The knob that fixed the hit rate also bounds
  how much of the memory hierarchy the cancel path grades. Worth measuring
  against a lower value on the real node.
