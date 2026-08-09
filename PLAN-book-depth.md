# Plan: make book depth actually stress the memory hierarchy

For review. Nothing here is implemented yet.

---

## 1. What we know, measured

A cache-conscious engine now exists (`engine/tests/engines/optimized.cpp`) — pool
allocation, flat level array, open-addressed cancel index — and it passes the
gate. So the ranked profile can finally be asked whether it discriminates.

| depth | naive size | reference p50 | optimized p50 | ratio |
|---|---|---|---|---|
| 23k orders | 3.1 MB | 130.3 ns | 40.1 ns | **3.25×** |
| 49k orders | 6.7 MB | 130.3 ns | 40.1 ns | **3.25×** |

Two facts, and the second is the problem:

- **It does discriminate.** 3.25× on the ranked number, 4× net of probe. The
  profile is not broken.
- **Depth is currently doing nothing.** Doubling the book changed neither engine
  by a single tick. Both books sit inside this box's 16 MB L3, so both pay an
  L3 hit and the difference is structural (12 dependent loads vs 1–2), not
  memory-hierarchy.

The conclusion you drew is right: **while the book fits in cache, a
cache-optimised layout and a naive one are measured on everything except the
thing that makes them different.** The 3.25× we see is the tree-walk penalty. The
locality penalty is not being charged at all.

## 2. Why going deeper is currently expensive

Book depth is not a free parameter today. Measured relationship:

```
depth  ≈  sessions × live_target × 0.039
warmup ≈  sessions × live_target × 5 events
```

That **0.039** is the problem. A session's `live` list is what it *believes* is
resting, and 96% of it is stale — orders that were filled, which the generator
cannot observe because it runs no matching engine. So we carry 25× more tracked
entries than resting orders, and the warm-up scales with the tracked count, not
the useful one.

Cost of depth today:

| target depth | naive size | warm-up needed | stream / buffer |
|---|---|---|---|
| 23k (current) | 3.1 MB | 2.9M events | 10M / 0.4 GB |
| 120k | 16 MB | **15.4M events** | 35M / 1.4 GB |
| 300k | 41 MB | **38.5M events** | 60M / 2.4 GB |

A 40M-event attempt timed out on this box. At that scale a bench job goes from
~15 s to ~90 s, and with 18 participants against a strictly serialised node the
queue moves from comfortable headroom to saturated.

**So the blocker is not memory bandwidth — you are right that we have plenty. It
is that depth is coupled to warm-up through a 25× waste factor.**

## 3. Proposal: give the generator an exact book

Run the reference engine *inside* the generator, and drive `live` from its
output instead of guessing.

The generator already links the reference (`--validate` runs it). Feeding each
emitted event through an internal book and reading the resulting `Trade` /
`CancelAck` / `Expired` tells it exactly which orders are resting. Then:

- `live` contains only genuinely live orders → **P(live) ≈ 1**
- `depth ≈ sessions × live_target` **directly**
- warm-up collapses from `depth / 0.039 × 5` to roughly `depth × 2` events

| target depth | warm-up now | warm-up after | improvement |
|---|---|---|---|
| 120k | 15.4M events | ~0.24M | **64×** |
| 300k | 38.5M events | ~0.6M | **64×** |

A 300k-order book becomes reachable inside a 10M-event stream with the timed
region intact, at today's job cost.

Secondary benefits, all of which are things we currently cannot control:

- **cancel-hit rate becomes a dial**, not an emergent 45%. Real books cancel
  nearly every order they don't fill; we could set it deliberately and keep a
  small deliberate miss rate for the unknown-order path.
- **fill rate becomes independent of depth**, instead of both falling out of the
  same aggression knob.
- the "believed live" staleness that produced the original 93%-miss bug stops
  being a mechanism at all.

Costs and risks, stated plainly:

- **Generation gets ~2× slower.** The reference costs ~1.3 s per 10M events. It
  is offline and once per stream; the bench worker already generates a stream
  per job in ~1 s. Acceptable.
- **The generator depends on the reference being correct.** It already does for
  `--validate`. A reference bug would change stream *shape*, not stream
  *validity* — the stream is still a legal input sequence whatever the book does.
- **Determinism is preserved.** The reference is deterministic; same seed still
  produces the same bytes. This is a `--validate` assertion already and stays one.

## 4. Choosing the depth

Per-order footprint differs by engine, and that is exactly the lever:

| | per order | 120k orders | 300k orders |
|---|---|---|---|
| reference (map + list nodes) | ~136 B | 16 MB | **41 MB** |
| optimized (pool + flat levels) | ~48 B + 1.6 MB fixed | 7.4 MB | **16 MB** |

The discriminating window is where **the naive engine is RAM-bound and the
optimised one is not**. On a 16 MB L3 that is roughly **150k–300k resting
orders**. Below it, both are cached and depth is free. Above it, both miss and
the advantage compresses again.

The bench node's real L3 is unknown until it exists, so the target should be
**set from the noise-floor run**, not fixed now: measure L3, pick a depth that
puts ~2× L3 of naive footprint in the book.

### The realism objection, and my answer

150k–300k resting orders is deeper than a real single-instrument book (a liquid
US equity shows maybe 1k–20k). I do not think that sinks it, but it should be a
conscious trade:

- The contest's stated purpose is the memory hierarchy — plan §16 rejects
  Callgrind precisely because instruction counts "ignore the memory hierarchy,
  the dimension an order-book contest is about". A book that never leaves cache
  cannot serve that purpose.
- The alternative framings are worse: shrink per-order footprint (we do not
  control the submission's), or add instruments (out of scope).
- Precedent exists in the plan itself — `deep_book` is a profile whose entire
  stated purpose is "stresses insertion and memory layout".

If realism wins, the honest consequence is that **cache optimisation stops being
the differentiator** and the contest ranks structural choices (hash vs tree)
only — which is still a real contest, just a different one. That is the decision
in front of you.

## 5. Sequence

1. **Exact live-tracking in the generator.** Internal reference book, `live`
   driven by observed output. Re-validate all four profiles; determinism check.
2. **Re-measure the depth/latency curve** with both engines at 50k / 150k / 300k.
   Confirm the gap widens past the L3 crossover — if it does not, stop, and the
   current profile stands.
3. **Pick the ranked depth from the bench node's real L3**, during the
   noise-floor run rather than now.
4. **Re-tune `cancel_heavy`** with cancel-hit and fill rate set explicitly rather
   than emergent.
5. **Re-run the three-engine comparison** (naive / reference / optimized) as the
   acceptance test: the ranked profile must separate them reproducibly.

Step 2 is the gate on the rest. If the gap does not widen, steps 3–5 are wasted
and 23k stands.

## 6. What I would not do

- **Push depth by growing the stream.** 40M-event streams cost 90 s jobs and a
  saturated queue for the same result the generator fix gets in 10M.
- **Fix the ranked depth now.** It depends on the bench node's L3, which does not
  exist yet.
- **Drop the 3.25× we already have.** The current profile discriminates. This is
  about widening a real gap, not rescuing a broken one — so it should not block
  the event if it runs out of time.
