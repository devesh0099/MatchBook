# Status

Matching Engine Challenge platform. 12 commits, ~16,600 lines across C++20, Rust,
TypeScript and SQL, plus 2,190 lines of vendored HdrHistogram.

**All twelve build-order items from the implementation spec are complete.** What
remains is calibration that needs real hardware, and four features listed at the
bottom that were deliberately not built.

---

## 1. What exists

```
me-platform/
├── spec/SPEC.md                   552   the published, normative specification
├── engine/                             C++20 — the contest itself
│   ├── include/mebench/           279   frozen headers: the participant contract
│   ├── reference/                 321   the oracle (std::map + std::list)
│   ├── generator/                1237   deterministic stream generator
│   ├── harness/                  2205   verify + bench + digest
│   ├── tests/                     952   41 visible tests, 5 mutant engines
│   ├── boilerplate/               350   the skeleton participants start from
│   └── third_party/hdr/          2190   vendored HdrHistogram, pinned df64f85
├── platform/
│   ├── db/001_schema.sql           112   whole data model; the queue is a table
│   ├── common/                      95   types shared across the process boundary
│   ├── api/                       1109   axum: two routers, two listeners
│   ├── worker/                    1474   pool + bench roles, isolate integration
│   └── web/                       1551   Next.js 16: editor, results, board, spec
└── ops/                           1099   provisioning, noise floor, runbook
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
oracle, the noise-floor baseline and what the bands are calibrated against, so
every branch is written to map onto a numbered spec rule. The two rules most
likely to be got wrong are the ones most heavily commented: trade price is the
**resting** order's price, and FOK fillability **excludes the aggressor's own
firm**.

### `generator/` — deterministic streams

`std::mt19937_64` is portable; its distributions are not. `uniform_int_distribution`,
`shuffle` and `sample` are banned in this directory, and everything derives from
one `below()` helper. Same seed, same bytes, verified.

Four profiles differ by agent mix, cancel share, TIF mix, and book depth.
`cancel_heavy` is the ranked one and deliberately carries ~2,600 resting orders:
a few hundred fit in L1 and every data structure measures identically, which
would quietly defeat ranking on per-order latency.

Seven adversarial injections, each asserted to fire **and to have its intended
effect**. Two non-obvious things were needed to make them fire at all:

- A far price band isolates injected *resting* orders from organic aggressors
  but cannot isolate an injected *aggressor* from organic liquidity — an
  aggressor always takes the best price first. Each injection with an aggressor
  now opens with a sweep that clears the side it needs empty. Without it the FOK
  case counted organic liquidity and **committed instead of rejecting**,
  silently destroying the one case that separates rule F1 from the naive check.
- Book depth grew without bound from two independent causes: orders placed
  through the touch were tracked as "believed resting" although they fill almost
  at once, and each session's live list hit its cap and evicted entries — an
  evicted order being one nobody can ever cancel again. Depth is now flat over
  10M events on every profile.

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
| `ctest` (C++) | **13/13** |
| Sandbox integration vs real isolate | **4/4** |
| Rust workspace | 0 errors, 0 warnings |
| Frontend | typecheck clean, builds, image serves |
| Fresh `git clone` → build | ✅ |

**End-to-end, on real infrastructure** (Postgres 16, MinIO, Redis, isolate 2.6):

```
submitted #6
  t+5s   received
  t+10s  benchmarking
  t+25s  done      p50 40.1 ns · probe 20 ns · 9 runs · 0 discards
```

Submit lane, Run lane (`2/41` for the skeleton) and the rejudge block have all
been driven end to end. With no bench worker running, a submission held at
`pending_benchmark` and resumed by itself when one appeared — the failure
isolation working without being prompted.

**Security properties confirmed rather than assumed:** the stream path does not
resolve inside the box, the box runs as a subordinate uid, no seed appears in
argv, the harness still receives the stream on fd 9. Infinite loop reports `TO`,
runaway allocation is OOM-killed as `SG`, a fork bomb is contained by the process
cap, and no process outlives the box.

---

## 5. Measured

- **Reference engine: ~60 ns/event mean**, ~0.8 s per timed run. Nine runs ≈ 8 s,
  not the 30–60 s the plan budgeted — the queue has far more headroom than
  projected. Bench mode records per-run wall time so the real node measures this
  rather than us estimating it.
- **Probe cost is a large fraction of the ranked number.** On this box, 20 ns
  against a 40 ns p50. It is a constant added to every sample, so ordering holds,
  but a 2× engine improvement moves p50 by much less than 2×. Reported alongside
  every result, with a warning above 25%. **Left as-is by decision**; the §9
  noise-floor run on real hardware is the designated place to settle it.

---

## 6. Open — needs hardware

- **Noise floor + soak** → settles §16 q1 (drift correction) and q2 (metal vs
  dedicated). Scripts exist; `analyze.py` maps spread and drift onto the plan's
  decision table and was validated against synthetic tight and noisy machines.
- **Band calibration.** Thresholds live in the `settings` table, so this is a SQL
  statement on the morning, not a deploy. The defaults are invented numbers and
  mean nothing until calibrated against the reference on the actual bench node.

## 7. Deliberately not built

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

One known weak test: `market_order_never_rests` asserts only that the book is
empty afterwards, so it would also pass for an engine that does nothing. M2/M3
are covered properly by three other tests; the `2/41` the skeleton scores is
slightly flattering for this reason.
