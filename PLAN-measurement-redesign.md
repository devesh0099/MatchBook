# Plan: the phased gauntlet — redesigning measurement, and the fleet under it

For review. Nothing here is implemented. Supersedes the queue-and-bench-node
model described in STATUS §3 and parks PLAN-ratio-scoring.md (§10 explains why).

Two decisions drive everything else:

1. **Scoring moves from nanoseconds to a ladder.** A submission is graded on
   how far it climbs through ever-larger workloads inside per-level time
   budgets, not on a per-event p50. Fine-grained separation happens once, after
   the contest, on one controlled machine.
2. **The shared bench node and its queue are replaced by one box per
   participant.** Auth maps each student to their own AWS instance; their Runs
   and Submits execute there and nowhere else. Nobody waits behind anybody.

The measurement pipeline becomes four phases:

| | Phase | What | Where | When |
|---|---|---|---|---|
| **0** | Correctness | visible spec tests; hidden differential verify on Submit | student's box | Run + Submit |
| **I** | Bulk run | ~500k events × 3 runs, percentile scoring, per-run deadline | student's box | Run + Submit |
| **II** | Ladder | increasing workloads, each gated by a deadline, single attempt, climb until a miss | student's box | Submit only |
| **III** | Rejudge | every finalist's last-submitted code, X events under a deadline, sealed seed | golden box, sequential | after contest end |

**Run** = Phase 0 + I. Unlimited, feedback only, never touches the leaderboard.
**Submit** = Phase 0 + I + II. **One scoring chain judges everything, in every
phase: p95, then p50, then p99** — per-event latency measured while the
workload runs. Wall-clock never ranks anyone; deadlines exist only as gates so
nobody too slow continues. The live leaderboard ranks on the highest ladder
level cleared, tie-broken by the chain at the highest level both cleared, then
earlier submission. Phase III produces the final standings.

---

## 1. Why the ladder, stated honestly

The current design ranks on a median-of-9 per-event p50 measured with `rdtscp`.
It works — STATUS §4 shows it working — but it concentrates all its demands in
one place: a single hardened node whose nanoseconds must be comparable across
six hours, which is why the steal-time discards, the spot check, the drift
question, and the entire ratio-scoring plan exist. The measurement is exquisite
and everything around it exists to protect it.

The ladder inverts the tolerance. A pass/fail budget with real headroom absorbs
the box-to-box and hour-to-hour variance that nanosecond ranking cannot: two
instances that differ by 3% will produce the *same* ladder result unless a
submission was already within 3% of a budget line, and §5 sets budgets so that
sitting near a line is itself the discriminating event. That is what makes
per-student boxes affordable as a fairness proposition — the metric is coarse
where the hardware is uncontrolled, and the one fine-grained measurement left
(Phase III) runs where the hardware is controlled.

What is deliberately given up: mid-contest ranks are provisional. The live
chain numbers come from each student's own box; only the golden box's are
final. Announced, not discovered: the leaderboard shows "Level 6 · p95
210 ns", and the spec says final standings come from the rejudge.

---

## 2. What survives — most of it

This is a re-plumbing, not a rebuild. Explicitly carried forward:

- **The whole verify path**: first-divergence stop, binary-searched minimal
  reproducer, the invariant shadow book, `front_seq`. One change of plumbing:
  the comparison runs against cached oracle output (§4) instead of a live
  side-by-side oracle, which only the reproducer step still needs.
- **The generator** and its determinism discipline (one `below()` helper, same
  seed same bytes). Extended, not changed (§4).
- **The sandbox**: isolate, subordinate uids, fd-9 stream passing, no seed in
  argv, `SIGALRM` timeout enforcement. Every phase runs inside it.
- **The worker binary** — re-roled from pool/bench to a per-box agent (§7),
  keeping claim-and-commit, heartbeats, and the janitor.
- **The bench harness** (`rdtscp`, HdrHistogram, steal-time discards,
  median-of-9, probe calibration) — retired from the live path, re-employed
  wholesale as Phase III's instrument.
- **`bench-hygiene.sh`**, run at box provisioning and re-runnable from the
  admin surface.

Retired from the live path: the bench queue and `bench_slots` (nothing is
shared, so nothing queues), the spot-check-driven node-unhealthy machinery
(replaced by per-box health in the registry, §8), and ratio scoring (§10).

---

## 3. The phases, precisely

### Phase 0 — correctness

Compile, then the visible spec tests (41 today; strengthening
`market_order_never_rests` per STATUS §7 would make it a more honest 42).
On **Submit**, additionally the hidden differential verify against the oracle
on a **fixed 50k prefix of SEED1** (§4), with the invariant layer on. The hidden half stays
Submit-only so Run keeps its ~2s iteration loop, and stays present at all so an
engine hard-coded against the visible tests cannot reach the leaderboard.

Failure output: compiler stderr verbatim; failing test names with expected/
actual; for hidden-verify divergence, the existing minimal reproducer
(1–19 events, participant-visible, seed withheld).

### Phase I — the bulk run

One stream of **N₁ ≈ 500k events** from the fixed SEED1 (§4), run **3 times**,
each run under a per-run deadline (placeholder 5 s — deliberately loose; a
deadline is a gate, never a score). Each run measures **per-event latency**
with the existing bench instrument (`rdtscp` into HdrHistogram, warm-up
derived from the stream header exactly as today, so the book-filling phase is
never timed), and the reported result is the **median-across-runs p95 / p50 /
p99** — the same chain every phase judges by.

On Run, Phase I is the student's feedback: the percentile chain, throughput,
and the spread of the three runs — the exact instrument that will judge them,
seen on every iteration. On Submit it re-runs (Submit grades the submitted
source, not a remembered Run) and gates entry to Phase II.

Three runs, not one, because a single 500k-event sample is the noisiest
measurement in this design; the median of three is cheap (~1 s total for any
plausible engine) and stable.

### Phase II — the ladder

A sequence of levels, each a deterministic stream from **SEED2**
(§4), each gated by a **deadline** — a gate, never a score. One attempt per
level, in order; the climb ends at the first missed deadline, a crash, or a
divergence from the cached expected output (§4 — a wrong answer at level 5 is
a failure at level 5, so speed cannot be bought with wrongness). While a level
runs, the bench instrument records per-event percentiles; every cleared level
stores its p95/p50/p99, and the **highest cleared level's chain is the live
tie-break**.

Levels scale on **two axes**, not one:

- **Event count** — 500k, 1M, 2M, 4M… (geometric; placeholders).
- **Resting-book depth** — ramping toward and past the 750k-order profile at
  the top levels.

The second axis is what makes the ladder measure something. Deeper books are
qualitatively harder (the book leaves L3 — the entire finding of
PLAN-book-depth.md), whereas more events at the same depth only measures
sustained throughput. With depth ramping, each level asks a different question,
and the level where an engine stops climbing says which question it failed.

The ceiling is **finite and published** — streams are baked ahead, so an
open-ended ladder is not physically possible anyway. §5's calibration picks
the top rung, sized above where any plausible one-day engine reaches; and if
two leaders do top out level-tied, Phase III's percentile chain separates
them, so the ceiling is a bake-time bound, not a ranking risk.

Failure output per level: events processed before the deadline (the harness
counts them; `SIGALRM` fires asynchronously exactly as today), achieved
throughput, the budget it missed and by how much, and the per-level table of
everything cleared so far. A timeout at level k with 61% of events processed
tells a student more than any p50 ever did.

### Mechanics, decided

- **One evaluation at a time per student.** A Submit while any evaluation of
  theirs is running is **rejected** with a clear message — not queued, not
  cancel-and-replace; a Run while a Submit holds the box is rejected the same
  way. The box is theirs, but the measurement core is single-file.
- **Submits are otherwise unlimited.** No cooldown, no count limit;
  one-at-a-time is the only throttle.
- **Freeze is an operator toggle** on the admin surface (the existing
  `settings` switch, surfaced on the dashboard when it exists): frozen hides
  live ranks; evaluations keep running.
- **A student who clears no rung still ranks**: level-0 rows are sub-ordered
  by their Phase I chain (p95, then p50, then p99), so the board stays
  total-ordered and first-hour movement is visible.
- **Contest close is operational, not architectural**: submissions cut off at
  the announced time, students are asked to stop, and a ~10-minute drain
  window lets in-flight evaluations finish before the freeze and rejudge.
  The last submission stored by the cutoff is the rejudge target.

### Phase III — the rejudge

After contest end, on **one golden box**, **sequentially**: every
participant's **last submission stored to S3 via the Submit button** —
latest, not best, and latest **regardless of how it fared live**. If that
code fails correctness at rejudge, it scores as failed; the fallback to
"last *verified* submission" was considered and declined, so the rule the
spec announces is blunt: make your final Submit one that works. The
workload, on the **sealed SEED3** (§4), never used live: **one heavy
constrained run — X events under a deadline of Y** (both TBD at M5
calibration, sized so the field separates), repeated, measured by the
existing deep-bench instrument (`rdtscp` per event into HdrHistogram,
median across repeats for every metric).

**Final ranking, recorded:** engines that complete the X events under the
deadline rank above engines that do not — the deadline gates, it never
scores. Among finishers — **p95**, then p50, then p99, then earlier
submission. Among non-finishers — **p99** over the events they processed
(validated against the cached expected output up to that point, §4, so a
fast-wrong engine cannot buy a tail number with garbage), then events
processed, then earlier submission. p95 leads deliberately: it grades tail
control, which a median hides. The live board and the final standings speak
the same language — the chain is identical everywhere; only the venue and
the workload change. One consequence to know: with wall-clock nowhere in
the chain and `rdtscp` stepping in ~10 ns quanta, exact full-chain ties are
possible; they fall to the earlier submission — the same rule the current
platform uses, made deliberate.

Sequential on one box is the point: it collapses every ranking-relevant number
into one short window on one machine — the same defence against drift the
current design uses (STATUS §6.2), now applied to the only numbers that are
final. Under 20 finalists × a few minutes each fits comfortably in an hour.
The sealed seed also bounds what overfitting to the live seeds can buy (§9).

---

## 4. Streams: three fixed seeds, pre-generated, prefix-consistent

Everything below 20M events generated per-job was tolerable; a ladder is not —
generation ran 16 s for 10M events, and a full climb would spend minutes
generating and seconds measuring. So:

- **Three fixed seeds, one per timed phase**: SEED1 for the Phase I bulk stream,
  SEED2 for every ladder level, SEED3 for the rejudge. SEED1 ≠ SEED2 so the unlimited Run
  lane is not a free timing probe of ladder rung 1. **SEED3 is sealed**: chosen
  before the contest, its artifacts (stream + expected output) uploaded to a
  private storage bucket and pulled by the golden box only when the rejudge
  begins — they exist on no running machine until then. The `MEBENCH_BENCH_SEED` mechanism (already
  shipped) generalises to a per-phase setting.
- **Every stream, its reference-engine expected output, and digest
  checkpoints are generated once, at image-bake time**, and baked into the
  box image. The reference runs **once per stream, at bake time**; at judge
  time, correctness is a streaming comparison against the cached expected
  output — valid at any cut point, so a completed level, a timed-out level,
  and a Phase III non-finisher's progress prefix are all checked by the same
  mechanism. Identical bytes on every box, by construction and by hash. The
  generator and the reference both leave the measurement path; the reference
  ships on boxes for exactly one remaining job — shrinking the minimal
  reproducer after a divergence is found.
- Streams still reach the harness on **fd 9** and never exist as a path inside
  the isolate box. Nothing about caching changes the exposure story.
- **The hidden verify (Phase 0) runs on a fixed 50k prefix of SEED1** — the
  recorded decision is three seeds and no fourth. The cost, stated so it is
  accepted rather than discovered: the verify failure report shows the
  student a minimal reproducer of 1–19 real events, so repeated
  deliberately-diverging submissions can mine the early stream through it.
  Accepted because mining is slow (≤19 events per submission), what it
  reveals is the stream Phase I already times, and the sealed SEED3 keeps the
  final standings beyond anything mined. With verify on baked bytes too,
  **nothing generates streams at runtime anywhere** — student boxes carry no
  generator at all.
- Levels that share a depth profile should be **prefixes of one stream**
  (generate the longest, cut) so a level-4 engine saw exactly the first half
  of what level 5 replays. Where the depth axis steps, prefix consistency
  breaks by design — a new depth is a new stream — and that is fine: the
  discontinuity is the level's content.

The generator needs two extensions, both small against its current shape: emit
a stream at a requested depth-ramp target (profiles already parameterise
depth), and emit the cached expected output with digest checkpoints per level,
so judge-time checking is one streaming comparison.

---

## 5. The level table: absolute numbers, chosen by measurement

Levels are published as **absolute tuples** — `(events, depth, deadline)`:
"level 4 — 2M events, 300k depth, 1.2 s". The deadline is a gate — miss it
and the climb ends; it never scores. No formula in the spec, nothing
reference-relative: a student reasons directly in events per second, and
difficulty climbs by varying the two knobs — **more events against a budget
that grows slower than they do** (the implied throughput bar rises each rung),
and **deeper books** where the rung's question changes in kind.

The published numbers are absolute, but they cannot be *picked* by instinct.
The illustrative "500k in 3 s" fails on contact with the measured numbers: the
*deliberately slow* reference engine sustains roughly 480 ns/event wall
(STATUS §4 — 43 s / 9 runs / 10M events), so it clears 500k events in ~0.25 s,
and a 3-second budget passes every engine that compiles, `std::map` included.
The table therefore comes out of one calibration afternoon on real instances —
run the skeleton, the reference, and `tests/engines/optimized.cpp` across
candidate rungs, then choose the tuples so that:

- **The skeleton fails at level 1** (it does nothing and times out or diverges
  on digest — either way, honestly zero).
- **The reference falls off at a known mid-rung**, giving every participant a
  visible, beatable milestone: "you out-climbed the oracle." Beatable is a
  measured fact, not a hope — the optimized test engine runs 4.9–6.0× faster
  than the reference at ranked depth, because the oracle is deliberately
  naive (`std::map` + `std::list`); any flat, cache-conscious book beats it.
- **The optimized engine clears several rungs past the reference**, or the
  upper ladder is measuring nothing — this is the acceptance test.
- **Budget margins sit ≥ 5× the measured box-to-box spread** at every rung,
  and comfortably above same-box run-to-run spread (bounded further by the
  isolated core, §7), so no rung is decided by variance. The noise-floor
  tooling (`ops/noise-floor/`) re-runs on the real fleet to put numbers on
  both; the current measured 1.05% IQR suggests margins are easy to afford.

The same afternoon fixes N₁, the Phase I timeout, and the ceiling, and the
finished table is recorded in the spec and in `settings`. Every number in
this document marked "placeholder" is an output of that run.

---

## 6. Auth: credentials, and a box binding

The `localStorage` roster dies. Each participant gets issued credentials
(**username + password, issued as printed slips at check-in** — recorded;
under 20 users means no self-service flows, no email, no reset UX — an
operator table and a printout). Login yields a session token; every participant API call carries
it; the session resolves to `participant_id`, and `participant_id` resolves to
**their assigned box**. That binding is the entire routing story: a Run or
Submit from student *k* is only ever claimable by the agent on box *k*.

The operator surface stays loopback-behind-SSH for now — it worked, and the
admin dashboard (§8) is designed against tables, not against a rushed operator
auth. With real participant auth in place, the web node faces the open
internet with no IP allowlist — recorded: auth is the lock, and the
instance is not worth hunting for.

---

## 7. The control plane: the queue dies, the state machine stays

"Remove the queue" is true as the participant experiences it — nothing of
theirs ever waits behind anyone else's work — but something must still
dispatch a job to the right box, record results, survive a box dying
mid-climb, and expose state to a dashboard. The cheapest correct shape is the
one already built:

- **Postgres on the control node stays the system of record.** Jobs are still
  rows; the claim is still `FOR UPDATE SKIP LOCKED`; the claim is still the
  state transition.
- **Each student box runs one agent** (the existing worker binary, new role)
  that polls for jobs `WHERE participant_id = <its own>`. The queue per box
  has depth ≤ 1 — it is not a queue, it is a mailbox — but claim-and-commit
  means a box that dies mid-job leaves a row the janitor can see and reset,
  exactly as today. No new crash-consistency story has to be invented.
- **The golden box runs the same agent** with role `golden`, idle all contest,
  claiming only rejudge jobs.
- **Push-RPC from the web node to boxes was considered and rejected**: it
  needs connection management, retry semantics, and a partial-failure story —
  all things claim-and-commit already provides — and it takes the audit trail
  out of the database the dashboard reads.

Per-student boxes also dissolve the pool/bench split: the same box runs
Phase 0's tests and Phase II's ladder. The hygiene script runs at provisioning
on every box; the steal-time check stays in the harness (it is nearly free and
`detail` from it feeds the dashboard), but a steal event on a student's own
box **warns rather than discards** — there is no co-tenant queue to protect,
and the golden box is where discards still mean discard.

**Every box pins measurement to an isolated physical core.** The fleet
instance is **`c6i.2xlarge` launched with `CpuOptions { ThreadsPerCore = 1 }`**
— SMT off from boot, leaving 4 whole physical cores: core 0 runs the OS and
the agent, core 1 is isolated (`isolcpus` + `nohz_full` + `rcu_nocbs`, IRQ
affinity steered to core 0) and runs nothing but the pinned harness, and
cores 2–3 take compiles, so the ~2 s Run loop stays fast without ever
touching the measurement core. With SMT disabled there is no sibling thread
to share L1/L2 or execution ports, no scheduler migration, no timer tick and
no IRQ inside a timed run — which is what bounds same-code run-to-run
variance. Two cautions that shaped the choice: a bare 2-vCPU instance
cannot deliver this (its two vCPUs are hyperthread siblings of **one**
physical core, so an "isolated" thread still shares the core with the OS),
and the natively SMT-free x86 families (`c7a`/`m7a`) are not offered in
ap-south-1. `c6i.2xlarge` is also the instance every existing calibration
number was measured on — the 750k depth sweep, the noise floor, the
end-to-end AWS run — and its 54 MB Ice Lake L3 is what the depth ladder is
sized against. At $0.34/hr in ap-south-1, a 20-box fleet for an 8-hour
event is ~$55 of compute. `bench-hygiene.sh` grows a check that the
isolation actually holds — kernel cmdline, SMT state, IRQ affinity, nothing
scheduled on the isolated core. The **golden box** gets everything the
current bench node has, and then everything the fleet box adds, and then
what only a single decisive box can afford: **dedicated tenancy** for the
rejudge window (no co-tenant exists to evict L3 or steal bandwidth — the
flat regional fee that was too expensive fleet-wide is nothing for one box
for one hour), SMT off, the isolated measurement core, full
`bench-hygiene.sh`, swap off standing in for the `mlockall`-in-isolate
limitation as today, the agent quiescing its own polling and heartbeats for
the duration of each timed run, and a fresh reference baseline recorded at
the start of the rejudge window so any drift inside it is visible. It is
the most isolated machine in the design because it is the only one whose
numbers are final.

---

## 8. Schema delta, and the dashboard it feeds

The future admin dashboard is bought now by keeping all state in tables it can
read, not by building it. The delta from `001_schema.sql`:

- **`participants`** += `credential_hash`, `box_id`.
- **`boxes`** (new registry): `id`, `instance_id`, `ip`, `role`
  (`student|golden|web`), `participant_id` nullable, `healthy`, `last_seen`,
  `detail jsonb` (hygiene report, last steal event, agent version, current
  job). Replaces the `workers` table's role; the agent heartbeats here.
- **`submissions`**: the bench-result columns (`p50_ns` … `percentiles`) move
  out of the live path; in their place `phase0 jsonb`, `phase1 jsonb`
  (the percentile chain per run and its median), `phase2 jsonb` (per-level
  table: events, deadline_ms, outcome, p50/p95/p99), `max_level int`, and
  the top cleared level's percentile columns for the leaderboard sort. The
  state enum becomes
  `received → compiling → testing → verifying → phase1 → phase2 → done`
  with one failure state per phase, each carrying its diagnostic in the phase
  blob. **One stable result-JSON schema per phase** is the dashboard contract:
  the student page, the leaderboard, and the future dashboard all read the
  same blobs.
- **`rejudge_results`** (new): `participant_id`, `submission_id`, the
  constrained-run outcome (finished, wall time, events processed) and the
  percentile set p50/p95/p99 (the retired percentile columns live here),
  seed, `judged_at`. Final standings are a view over this table implementing
  the §3 ranking chain; the live leaderboard view reorders to `max_level
  DESC`, then the top level's chain (p95, p50, p99), then `created_at ASC`,
  with level-0 rows sub-ordered by their Phase I chain.
- **`run_jobs`** gains the phase0/phase1 blobs; `bench_slots` is dropped.

The dashboard itself stays future work, but its first version is now four
`SELECT`s: boxes with health and current job, submissions by state, per-box
event log, live leaderboard.

---

## 9. Gaming and fairness, examined

- **Fixed contest seed** means a determined participant could tune constants
  to the one stream they are timed on. They never see the stream (fd 9, no
  path in the box), but repeated submits are a timing oracle. Accepted:
  everyone has the same oracle, tuning-to-workload is half of what real HFT
  work is, and the **sealed SEED3** bounds what pure overfitting is worth
  exactly where it matters — the final standings.
- **Correctness cannot be traded for speed** at any rung: every ladder level
  digest-checks against the oracle, and the hidden verify already gated entry.
- **Box lottery**: a student assigned a slightly slow instance loses nothing
  at pass/fail rungs with §5's margins, and nothing at all in the final
  standings, which come from the golden box. The one residual: live tie-break
  percentiles compare across boxes. Accepted as provisional-by-design; if the
  measured fleet spread says otherwise, tighten by re-running noise-floor per
  box and publishing per-box reference numbers alongside.
- **Latest-not-best in Phase III** is today's rule kept: it rewards the code
  you stand behind at the bell, and it is announced.

## 10. What happens to the open plans

- **PLAN-ratio-scoring.md is parked, not refuted.** Its problem — undetectable
  co-tenant interference contaminating a ranked nanosecond — mostly dissolves:
  ladder budgets carry margins far above interference amplitudes, and the
  golden box can simply be dedicated-tenancy for its one decisive hour
  (the flat $2/hr that was too expensive for a fleet is nothing for a box).
  Its §4 interleave idea remains the right tool **if** Phase III's deep bench
  shows instability on the day; keep the doc.
- **PLAN-book-depth.md is promoted.** The depth axis of the ladder (§3) is its
  crossover finding turned into the grading instrument itself.

## 11. What I would not do

- **Treat live tie-break gaps as precise.** The live chain compares
  percentiles measured on different machines; the deadline margins, the box
  acceptance band, and the golden box are what keep that honest. Present
  small live gaps as provisional, and never let a mid-contest dispute be
  settled by a cross-box nanosecond.
- **Pick the level table by instinct.** The published numbers are absolute,
  but every one of them comes out of the §5 calibration or it is wrong. The
  3-seconds-for-500k instinct was off by 12×, which is the argument.
- **Build the admin dashboard before the tables settle.** §8's contract-first
  approach means the dashboard is cheap later; building UI against a moving
  schema is how it becomes expensive now.
- **Invent a new transport for job dispatch.** The mailbox-row pattern is the
  queue machinery minus contention; keep it.
- **Let Run and Submit drift apart.** Both lanes must execute Phase 0 + I via
  the same code path with a flag, or the "Run said 1.9 s, Submit said fail"
  bug class appears.

---

## 12. Implementation, in phases

Ordered so every milestone is independently testable on the current
single-node compose stack before the fleet exists.

**M1 — the gated bench and the ladder driver.** The bench instrument gains
a hard deadline (events-processed counter on `SIGALRM`, percentiles kept up
to the cut) and cached-expected-output comparison; a ladder driver walks a
level table; bake tooling emits stream + expected output + digest
checkpoints per level. Acceptance: skeleton / reference / optimized produce
the §5 ordering on a laptop, with a hand-written level table.

**M2 — schema v2 and the phased lanes.** New enum, phase blobs, `boxes`,
`rejudge_results`; worker executes Phase 0 → I → II as one claimed job;
Run = 0+I, Submit = 0+I+II; leaderboard view reordered. All on the existing
compose stack with the existing pool worker standing in as the agent.
Acceptance: a submission climbs, a broken one fails each phase with the right
diagnostic blob, ctest and the mutants still gate.

**M3 — auth.** Credential issue (operator script), login route, session →
participant → box resolution, `localStorage` roster removed, web UI login.
Acceptance: an unauthenticated request can do nothing; student A cannot see
or trigger anything of student B's.

**M4 — the fleet.** Terraform for N student boxes + golden box from one
image (toolchain, isolate, agent, hygiene, baked SEED1/SEED2 streams and expected
outputs; SEED3 artifacts in a private bucket, pulled by the golden box at
rejudge start); kernel core isolation and harness pinning per §7;
agent role with `participant_id` binding; janitor timeouts revisited for
ladder duration; box registry heartbeats. Acceptance: two real boxes, two
students submitting simultaneously, zero interaction; kill an agent
mid-climb and watch the janitor recover the row; hygiene confirms isolation
on every box; a **box acceptance test** — each box runs the reference on a
fixed workload and any box outside a tight band around the fleet median is
replaced, not compensated for. Replacement is minutes by design: the fleet
image is one baked AMI, so a dead or outlier box — before or during the
contest — is a fresh launch plus re-registration, never a rebuild.

**M5 — calibration.** The §5 afternoon on real instances: fix N₁, the level
table (events, depth, budget per rung), timeouts, ceiling; re-run noise-floor
across the fleet for the margin check; write the numbers into the spec and
the level table into `settings`. Acceptance: the
optimized/reference/skeleton separation criterion, measured on the real
fleet.

**M6 — Phase III + frontend.** Rejudge block against the golden box
(sequential, sealed SEED3, the constrained run + percentile ranking chain);
results into `rejudge_results`;
final-standings view. Web: ladder progress on the submission page (per-level
table is already in the blob), leaderboard v2, per-phase failure rendering.
Acceptance: full dress rehearsal — three participants, one contest hour
compressed, freeze, rejudge, final board.

**M7 — dashboard readiness (stub only).** Read-only admin endpoints over the
§8 tables and a health summary; the UI remains future work by design.

Rough sequencing: M1–M2 are the bulk of the engine/platform work and need no
AWS. M3 is small. M4–M5 are the AWS week. M6 closes the loop. Nothing in
M1–M3 blocks on fleet decisions, so implementation can start while instance
choices are still open.
