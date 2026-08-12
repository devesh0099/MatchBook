# Status

Matching Engine Challenge platform. 38 commits, ~13,900 lines across C++20, Rust,
TypeScript and SQL, plus 2,190 lines of vendored HdrHistogram and ~2,500 lines of
prose (spec, runbook, subsystem docs).

**Every build-order item is complete, the benchmark measures what it was meant
to, and a submission has been driven through the whole platform on the real
path.** What remains is provisioning and calibration on the real bench node
(§6), plus the short list in §7 that needs no hardware.

---

## 1. What exists

```
flashmatch/
├── spec/SPEC.md                   559   the published, normative specification
├── engine/                             C++20 — the contest itself
│   ├── include/mebench/           286   frozen headers: the participant contract
│   ├── reference/                 341   the oracle (std::map + std::list)
│   ├── generator/                1692   deterministic streams; runs its own book
│   ├── harness/                  2947   verify + bench + digest, and HARNESS.md
│   ├── tests/                    1667   41 visible tests, 5 mutants, 1 fast engine
│   ├── boilerplate/               316   the skeleton participants start from
│   └── third_party/hdr/          2190   vendored HdrHistogram, pinned df64f85
├── platform/
│   ├── db/001_schema.sql          126   whole data model; the queue is a table
│   ├── common/                     85   types shared across the process boundary
│   ├── api/                      1078   axum: two routers, two listeners
│   ├── worker/                   1677   pool + bench roles, isolate, SANDBOX.md
│   └── web/                      1963   Next.js 16: editor, results, board, spec
└── ops/                          1132   provisioning, noise floor, runbook
```

---

## 2. The C++ engine

### `include/mebench/` — the frozen headers

Four headers, transcribed from the plan's data contracts with their size asserts
intact: `WireEvent` 24 B packed, `Order` 32 B aligned, `OutEvent` 56 B,
`OrderRef` 16 B. Packed structs are for files, aligned structs for hot loops, and
neither is reused for the other.

There is no `Modify` anywhere, and `RejectReason` carries exactly the three
values a rule can actually produce. An enum value for an input the generator
cannot emit is untested dead code that every participant has to guess about.

### `reference/` — the oracle

`std::map<Price, std::list<Order>>`, clarity over speed. It is the fuzzing
oracle and the noise-floor baseline, so
every branch is written to map onto a numbered spec rule. The two rules most
likely to be got wrong are the ones most heavily commented: trade price is the
**resting** order's price, and FOK fillability **excludes the aggressor's own
firm**.

### `generator/` — deterministic streams

`std::mt19937_64` is portable; its distributions are not. `uniform_int_distribution`,
`shuffle` and `sample` are banned in this directory, and everything derives from
one `below()` helper. Same seed, same bytes, verified.

Four profiles differ by agent mix, cancel share, TIF mix, and book depth.
`cancel_heavy` is the ranked one and carries **~300k resting orders across
~5,900 price levels**.

The generator runs its own `ReferenceEngine` and drives each session's live set
from what that book emits, rather than guessing which of its orders are still
resting. That guess used to drift 96% stale, with two consequences: 93% of
cancels named orders that were already gone — so the median ranked event was a
*failed index lookup*, the cheapest operation in an engine — and depth came out
at 23k where the parameters implied 512k. At 23k the book never leaves L3, and
23k and 49k measured **byte-identically** on both a naive and a cache-conscious
engine. Depth was decorative.

It is not now. Measured against `tests/engines/optimized.cpp`, 6M timed events:

| depth | reference | optimized | ratio |
|---|--:|--:|--:|
| 23k *(old)* | 130 ns | 40 ns | 3.25× |
| 50k | 220 ns | 50 ns | 4.40× |
| 150k | 281 ns | 60 ns | 4.67× |
| **300k** | **341 ns** | **70 ns** | **4.86×** |

Full detail, including why most of the gain lands by ~100k and why this box
cannot resolve the last step, is in `PLAN-book-depth.md`.

Seven adversarial injections, each asserted to fire **and to have its intended
effect**. Three non-obvious things were needed:

- A far price band isolates injected *resting* orders from organic aggressors
  but cannot isolate an injected *aggressor* from organic liquidity — an
  aggressor always takes the best price first. Each injection with an aggressor
  now opens with a sweep that clears the side it needs empty. Without it the FOK
  case counted organic liquidity and **committed instead of rejecting**,
  silently destroying the one case that separates rule F1 from the naive check.
- That sweep is a market order, and a market order accepts every price, so it
  takes the *entire* organic ask side. Invisible at 3k resting orders; half the
  book at 300k. Injections are now scheduled across the stream's first third,
  where the book is filling anyway.
- Cancel share stopped being a free parameter once tracking became exact. In
  steady state every resting order leaves by cancel or by fill, so cancel share
  is pinned near 37%; the profiles asked for 55% and drained every live set to
  empty. Cancel pressure now ramps with how full a session's set is, making
  depth the controlled variable and cancel share the emergent one.

### `harness/` — how submissions are run

Participants never write a `main()`. They implement `create_engine()`, it is
compiled to a `.so`, and the harness `dlopen`s it and drives it. That inversion
is why CMS and DOMjudge were unusable: they diff a process's stdout, whereas here
the submission is linked into the measuring program.

**verify** streams the submission and the oracle side by side, stops at the first
divergence, and binary-searches a reproducer — the mutants shrink to 1–19 events
from a 50k stream. The invariant layer rebuilds a shadow book purely from the
engine's *own* output and cross-checks it against the engine's own snapshot, so
it catches bugs the reference might share. `front_seq` is what catches a LIFO
level queue; aggregates alone look identical.

**bench** does `rdtscp` per-event timing into HdrHistogram, with a checksum-only
sink, per-run steal-time checks, empty-engine probe calibration, and the median
of per-run p50s with a bootstrap interval.

**digest** folds the field-wise hash for a stream. The bench node compares every
ranked run against the *oracle's* digest — a submission's own correctness-run
digest could not serve, because the two lanes run different streams.

Two bugs the harness's own tests found:

- The divergence report omitted `participant_id`, so a forged self-trade printed
  "expected" and "actual" lines that read **identically**. STP keys on that
  field, so a divergence can live entirely in it.
- The wall-clock limit **could never fire for a real infinite loop**: the in-loop
  deadline only runs between events, and an engine stuck inside `on_new` never
  returns control. Enforcement is now asynchronous via `SIGALRM`.

### `tests/` — including engines built to fail

41 visible tests, one per spec rule, written against `IMatchingEngine` so the
same file runs server-side against the reference and in the boilerplate against a
participant's engine. Expected outputs are built with the frozen `out::` helpers
so **every** field is compared — matching what the benchmark digest folds.

Five deliberately broken engines, one per layer of the gate: wrong trade price,
dropped `Expired`, forged self-trade, a snapshot the output does not support, and
an unbounded loop. All five are ctest cases, because a gate that has only ever
seen correct engines has not been tested.

---

## 3. The platform

### `db/001_schema.sql`

The queue **is** the submissions table: claim-and-commit on
`FOR UPDATE SKIP LOCKED`. The claim is the state transition, so crash recovery is
one janitor statement per stage rather than a protocol. At ~200 submissions
across the event, a table is a perfectly good queue.

### `api/` — axum

Two `Router`s on two listeners. Participant routes on `0.0.0.0:8080`; operator
routes on `127.0.0.1:8081`, reached over an SSH tunnel. **That is the entire auth
story** — not a prefix, not middleware, not a token. Verified refused from the
host's LAN address.

The janitor's timeouts sit well above each stage's isolate wall-time, so it can
only ever catch a dead worker; every action it takes lands in `events_log`. A
dead bench job requeues at the **front**, since the participant already waited
once and it was not their fault.

Queries are runtime-checked rather than compile-time-checked. The `query!` macros
need a live database or a checked-in offline cache *at build time*, and the API
image builds on the web node from a clean checkout. Stated in `main.rs` so the
trade-off is visible rather than looking like an oversight.

### `worker/` — pool and bench

One binary, two roles. Pool claims `run_jobs` ahead of submissions because Run is
the iteration loop and a Submit can wait three seconds. Bench claims strictly one
at a time, ordered by requeue priority so steal-time discards go to the front.

The hidden stream reaches the harness on an **inherited descriptor**, never as a
file inside the box. The first cut wrote `stream.bin` into the box directory —
which is the submission's own directory — and would have handed the stream to the
code being measured. Same exposure as the earlier `--seed`-in-argv leak,
arriving by a different route, which is the argument for fd-passing being the
default rather than an option.

### `web/` — Next.js 16

Four views. The editor is the sole submission path, so it got the attention: one
editable `engine.cpp` buffer, four read-only header tabs, Run/Submit, debounced
server-side autosave, 2s polling while non-terminal.

The starting buffer and header tabs are **generated from the real files in
`engine/` at build time**, and the boilerplate zip is assembled from the same
sources. Verified byte-identical by hash — hand-copying is exactly how those
drift, and a participant pasting between them would be the one to find out.

Monaco is vendored and served from our own origin. The default pulls it from a
CDN, which would mean the only way to submit breaks if the room's network does.

Next was bumped 14 → 16 (and React 18 → 19) after npm flagged high-severity
advisories with no fix inside the 14.x line. This deviates from the impl spec's
"Next.js 14" — a deliberate, recorded call. `npm audit` is clean.

---

## 4. Verification

| Suite | Result |
|---|---|
| `ctest` (C++) | **13/13** dev · **14/14** contest |
| Sandbox integration vs real isolate | **4/4** |
| Rust workspace | 0 errors, 0 warnings |
| Frontend | typecheck clean, builds, image serves |
| Fresh `git clone` → build | ✅ |
| `docker compose up` as a stack | ✅ all five services |
| A submission through the whole platform | ✅ `received` → `done` |

**The whole platform, on the real path.** One submission — the reference engine
pasted in as `engine.cpp` — POSTed through Caddy, compiled and verified by a
pool worker in an isolate box, measured by a bench worker, read back through the
API:

```
received → compiling → verifying → verify_passed
        → pending_benchmark → bench_queued → benchmarking → done

p50 320.4 ns · p99 1542.8 ns · probe 10.0 ns (3.1%)
9 runs · 0 discards · 6,108,800 timed events
116 percentile points · 61 timeline windows, both rendering
```

The number is meaningless — one unpinned laptop running Postgres, MinIO, Caddy,
Next and both workers at once — but every stage moved and every field arrived.

Two behaviours confirmed rather than asserted. The pool worker enqueued eight
seconds before the bench worker registered, so the job **parked at
`pending_benchmark` instead of erroring**, and the janitor unparked it by itself
once the node appeared. Neither was staged; it happened because of the timing.

Submit lane, Run lane (`2/41` for the skeleton) and the rejudge block have all
been driven end to end.

**Security properties confirmed rather than assumed:** the stream path does not
resolve inside the box, the box runs as a subordinate uid, no seed appears in
argv, the harness still receives the stream on fd 9. Infinite loop reports `TO`,
runaway allocation is OOM-killed as `SG`, a fork bomb is contained by the process
cap, and no process outlives the box.

**A ranked job in its production configuration** — real isolate box, the 10M
stream on fd 9 with no path inside, bench-stage limits:

```
p50 360.6 ns · p99 1884 ns · probe 10 ns (2.8%)
6,108,800 timed of 10,000,000 · warm-up 3,891,200 derived from the stream header
digest matched · 0 discards · 707 MB peak RSS of 8 GB · 43 s for 9 runs
```

That run also surfaced the `mlockall` finding in §6.

---

## 5. Measured

A full ranked job on this box, at the shipped 300k-order profile:

| stage | cost |
|---|--:|
| generate the 10M-event stream | 16 s |
| bench, 9 runs (3.9M warm-up + 6.1M timed each) | 40 s |
| peak RSS | 710 MB (against an 8 GB cgroup limit) |

- **Reference engine: p50 341 ns** at 300k resting orders, against 40 ns at the
  23k the profile used to carry. Most of that is the book leaving cache, which is
  the point. Warm-up is derived from the stream's own profile inside the harness,
  so a caller cannot silently time the book-filling phase.
- **Probe cost is no longer a large fraction of the ranked number.** 10 ns
  against a 341 ns p50 — 3%, where it used to be 50%. The depth change fixed this
  as a side effect; it was previously the largest known distortion in the score.
- **This box's `rdtscp` advances in 38-tick steps — exactly 10 ns.** Every p50 it
  reports is a whole number of quanta. That is a property of the hardware, not
  the harness, and it caps how finely any two engines can be separated here.
  Whether the bench node does the same is a question for the noise-floor run, and
  it also settles the ranking-presentation question, which has been open on
  preference and should be settled on resolution.

---

## 6. Remaining — on AWS

**The deployment has been run end to end on real AWS and the calibration
questions are answered.** `DEPLOYMENT.md` Part C carries the numbers; this is
what is left.

Measured on `c6i.2xlarge` (Ice Lake Xeon 8375C, **54 MB L3**): a full submission
`done` in 72 s at p50 96.6 ns with 0 discards, `41/41` on the Run lane, and
seven workers heartbeating across three machines.

### Settled

| | What | Answer |
|---|---|---|
| **Noise floor** | 200 runs on a **shared** instance | 4.46% single-run spread but **1.05% IQR**, median-of-9 stable to **±0.6%**, **0/200** steal-time discards. Engines 2% apart rank correctly 100% of the time. **Shared tenancy is enough** — dedicated adds a flat $2/hr per region and roughly triples the bill. |
| **Ranked depth** | sweep at 20M events, both engines | **3780 → 732,279 orders, 5.98×** discrimination — the best of any depth measured, against 3.50× for the same config on a 16 MB laptop. The 750k bet was right. Ship it. |
| **S3 bucket + instance role** | `infra/bootstrap/` | Terraform, tested. `flashmatch-node` is the platform's entire AWS surface. |
| **`bench-hygiene.sh` exits 0** | on a shared EC2 VM | Passes. Two checks were wrong for a VM and are fixed: the governor check hard-failed where no cpufreq driver exists, and the SSM check tested a unit name Ubuntu does not use, so it reported `ok` while the agent ran through every measurement. |

### Open

| | What | Why it still matters |
|---|---|---|
| **1** | **Measure the TSC granularity.** This box steps in 38 ticks — exactly 10 ns. | If the bench node does the same, a ranked p50 is a *count of quanta*, so **exact ties are expected, not rare**. The presentation is settled — sorted p50, earlier submission above — but this says how often the tiebreak decides a position, and whether the quantum is coarse enough that the ranking measures less than it appears to. |
| **2** | **Soak.** `ops/noise-floor/soak.sh` runs 6 hours and has not been run. | Catches drift across a day, which the 25-minute spread test cannot see. Lower stakes than it looks: the rejudge block collapses every final number into one short window on one machine, which is the designed defence against drift. |
| **3** | **18 people at once.** Everything measured so far used one submitter. | Six pool slots against 18 iterating participants is far from saturation on paper, and a Run is ~2s — but it is arithmetic, not a measurement. |
| **4** | **A dedicated bench node for the rejudge block**, if you want it. ~2 h, ~$5. | Cheap insurance on the numbers that decide ranking. Three hard requirements in `DEPLOYMENT.md` §C2: stop the old bench worker first (nothing prevents two running concurrently), delete the global spot-check baseline, and use the same instance type. |

## 7. Remaining — does not need hardware

Short, and none of it blocks the event.

- **`market_order_never_rests` is a weak test.** It asserts only that the book is
  empty afterwards, so it would also pass an engine that does nothing. M2/M3 are
  properly covered by three other tests — but it means the `2/41` the skeleton
  scores is slightly flattering.
- **Publish the kit.** `ops/make-boilerplate.sh` → `dist/me-boilerplate.zip`,
  published with the spec at kickoff. It embeds prebuilt `gen` and `bench`, so it
  must be rebuilt after any engine change or "same seed, same bytes" breaks
  between a laptop and the server.
- **The Rust image pin will rot again.** `platform/api/Dockerfile` has to be at
  least as new as the toolchain `Cargo.lock` was resolved with, and nothing in CI
  builds that image — it silently stopped building once already and was only
  found by running `docker compose up` for the first time. Build it after any
  `cargo update`.

## 8. Open plans, for review

- **[PLAN-ratio-scoring.md](PLAN-ratio-scoring.md)** — score
  `submission_p50 / reference_p50`, measured in the same job on the same core,
  instead of raw nanoseconds. Cancels machine contamination rather than trying
  to detect it, which matters because a co-tenant evicting L3 is *invisible*
  from inside the guest — it looks exactly like a slow submission. Costs 2×
  bench time. **Only worth doing if the noise floor says shared tenancy is
  marginal**; measure before building.

## 9. Future — worth doing, out of scope for one event

- **Deliberate cancel-miss tuning.** Cancels now hit 98%; the ~2% misses are
  ghost ids that never existed. A *recently dead* order would exercise the
  deleted-key and tombstone-probe paths in a submission's index, which a
  never-existed id does not. `PLAN-book-depth.md` §9.
- **`kRecentCancelPct = 85`** means most timed cancels target recently placed,
  and therefore cache-warm, orders. The knob that fixed the hit rate also bounds
  how much of the memory hierarchy the cancel path grades. Worth sweeping.
- **Multi-instrument books**, which is the honest way to reach realistic depth
  without a single book 15× deeper than any real one.
- **A build fingerprint embedded in the submission `.so`**, which would let the
  bench node reuse the pool's binary instead of recompiling. Currently the check
  has no evidence to work from and would pass vacuously, so the bench node always
  recompiles — ~1 s against a 60 s job, so this is a nicety.

## 10. Deliberately not built

Recorded in the runbook's Known Limitations so they are read in the morning
rather than discovered at 5pm.

- **Flamegraphs and plagiarism checking** — dropped by decision, not pending.
  Neither column nor check exists.
- **A bench-lane time gate** — dropped by decision. Benchmarking is live from
  the start: the correctness gate already prevents premature optimisation, since
  nothing reaches the bench queue until it has passed verification.
- **Redis as the serving read model** — the leaderboard is computed from Postgres
  per request, which is fine at 18 rows. Redis holds the freeze snapshot only,
  and that *is* on the serving path.
- **Authentication.** Participants pick a handle from a roster in `localStorage`;
  the operator API is loopback-only over an SSH tunnel. Fine for 18 people on one
  network for six hours. **Not fine on the open internet** — put the web node
  behind a security group admitting only the room's egress IP.
