# Plan: score the ratio, not the nanoseconds

For review. Nothing here is implemented.

The question behind it: **if we cannot get a machine to ourselves, can we detect
a contaminated run and throw it away?** Partly — and the platform already does
the detectable part. This is about the part that is not detectable, and about
scoring in a way that does not need it to be.

---

## 1. What is already there

Two mechanisms, both real and both working:

- **Per-run steal time.** The harness reads `/proc/stat` before and after each
  timed run and records `steal_delta`. A run with steal is discarded; three
  consecutive discards exit `UNHEALTHY` and the node stops taking work.
- **The reference spot check.** Every ~20 minutes, between jobs, the bench worker
  re-runs the reference engine and compares its p50 against a baseline recorded
  when the node came up. More than **5%** deviation marks the node unhealthy.
  Verified firing in practice at 7.1% on a contended laptop.

The second is the right shape: it measures a **known** workload and asks whether
the machine changed, without needing to know why.

## 2. What cannot be detected, at all

Steal time catches the hypervisor descheduling your vCPU. It does not catch:

- **A co-tenant evicting L3.** No counter reports "someone else took your cache
  lines". On a shared EC2 instance most PMU counters are not exposed to the
  guest either, so you cannot even watch your own miss rate to infer it.
- **Memory bandwidth contention.**
- **Socket-wide frequency drops** caused by a neighbour's AVX-heavy work.

All three arrive as the same observation: *this code was slower*. Which is
exactly what a slower submission looks like. **Contamination and a bad engine are
not distinguishable from inside the guest**, so no amount of watching our own CPU
or memory tells us which we have.

## 3. The gap the spot check leaves

It runs **between jobs**. A neighbour that starts and stops inside one 60-second
ranked job contaminates that job and the next spot check sees nothing.

So the dangerous case is not the obvious one. It is interference large enough to
move a rank but too brief, or too smooth, to trip the gate twenty minutes later.

## 4. Proposal: bracket every ranked run with the reference

Run both engines **in the same job, on the same stream, on the same core**,
interleaved, and score the ratio:

```
score = median(submission per-run p50) / median(reference per-run p50)
```

This stops trying to detect contamination and cancels it instead. A neighbour
thrashing L3 during the job slows both engines, and the ratio holds.

Three things fall out of it:

- **Drift is answered by construction.** A 9am result and a 3pm result are
  comparable without a correction model, which settles plan §16 q1 rather than
  implementing it. A correction is a model of the machine's behaviour and a model
  can be wrong; a ratio measured thirty seconds apart is not a model.
- **The node-unhealthy path gets quieter.** Today a wandering machine parks the
  queue. With a ratio, mild wander is divided out and only genuine instability —
  where the reference itself is inconsistent run to run — needs to stop the node.
- **It is what you would do on any untrusted machine.** This is not a workaround
  invented for us.

The reference is already on the bench node (`cfg.reference_so`) and the worker
already runs it once per job for the oracle digest, so there is no new artefact
to build or ship.

### Costs, stated plainly

- **Roughly 2× bench wall time**, ~60 s to ~120 s per job. Against one bench node
  and 18 participants that is still ample headroom — capacity goes from about 60
  submissions an hour to about 30, against a realistic load well under 10.
- **The leaderboard stops showing nanoseconds.** A unitless ratio is more honest
  and less satisfying to read. Mitigation: rank on the ratio, display both, and
  keep the ns column as the headline number people care about.
- **A good submission scores below 1.** The reference is ~341 ns and a strong
  engine ~70 ns, so ratios land around 0.2. Presenting it as "× faster than the
  reference" (5.0×) reads better than 0.2 and is the same number inverted.
- **It does not fix bursty interference inside one pair.** If a neighbour lands
  during the submission's run and not the reference's, the ratio is wrong. The
  interleave should therefore alternate rather than run all of one then all of
  the other, so the two engines see the same minute.

## 5. Cheaper alternative, and why it is weaker

Normalise against the existing 20-minute spot check instead of running the
reference per job: `score = p50 / most_recent_spot_check_p50`. No extra bench
time at all.

It corrects slow drift, which is most of what a shared instance does. It does
nothing about interference inside a single job — the exact gap in §3 — because
the reference measurement is up to twenty minutes away from the run it is
normalising.

Worth having as a fallback if the 2× cost is refused. Not worth choosing if it
is not.

## 6. Does any of this change the book depth?

**No. Depth is a function of L3 size, not of tenancy.** A `c6i.2xlarge` sees the
same socket cache whether or not a neighbour is on the box, so the crossover
point where a naive book leaves cache is unchanged.

Shared tenancy does one thing to it: a co-tenant's working set occupies part of
that cache, so the *effective* L3 is smaller and varies with what they are doing.
That pushes the naive engine out of cache **sooner**, which helps discrimination
— but it makes the boundary wobble.

The implication is not a different depth, it is a **safer** one: pick a depth
comfortably past the crossover rather than near it, so the measured gap does not
depend on how much cache a stranger happens to be using. The shipped sizing does
that — naive at ~1.9× L3, optimized at ~0.7× — and shared tenancy is another
argument for that margin rather than the tightest depth that technically works.

What *does* change depth is the hardware. The profile now ships at **750k**,
sized for a ~54 MB Ice Lake L3, where the window runs from about 400k to 1M. On
the 16 MB laptop it was tuned on, 300k was the better number — and 750k measures
*worse* there (3.50× against 4.86×), because both engines leave a 16 MB cache.
Re-run the sweep on the real node and take the widest ratio.

## 7. Sequence

1. **Measure first.** The noise floor (`ops/noise-floor/`) says how bad shared
   tenancy actually is on the instance we get. If the spread is inside the plan's bar,
   none of this is needed.
2. If it is not: implement §4 behind a setting, so a single ranked run reports
   both the raw p50 and the ratio.
3. Re-run the three-engine comparison. **The acceptance test is that the ratio
   separates naive from cache-conscious as cleanly as the raw number does, and
   is more stable across the day.** If it is not more stable, it has bought
   nothing and costs 2×.
4. Only then decide what the leaderboard displays.

## 8. What I would not do

- **Infer contamination from our own CPU or memory usage.** The interference is
  on the other side of the hypervisor; our processes look identical either way.
- **Tighten the spot-check tolerance.** It would fire more often on a shared
  instance and park the queue more, which is a worse failure than a slightly
  noisy number — and it still would not see inside a job. It has since gone the
  other way, 2% to 5%, for exactly this reason.
- **Implement this before the noise floor.** If dedicated tenancy is affordable
  and the instance is quiet, this is complexity for nothing.
