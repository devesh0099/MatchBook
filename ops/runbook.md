# Event-day runbook

Everything here is meant to be executable under pressure by someone who did not
write it. Commands first, reasoning second.

Nodes: **web** (Next.js, API, Postgres, Redis, all in compose) · **pool**
(compiles + verifies, several isolate boxes) · **bench** (ranked runs, exactly
one at a time, never scaled).

---

## Before the day

### Decide the hardware — this is a measurement, not an opinion

```sh
ops/noise-floor/measure.sh 200 spread.jsonl     # ~20 min
ops/noise-floor/soak.sh 6 soak.jsonl            # overnight, unattended
ops/noise-floor/analyze.py spread.jsonl soak.jsonl
```

`analyze.py` prints the decision for §16 q1 (drift correction) and q2 (metal vs
dedicated). Run it **before** provisioning the bench node for real — the boot
parameters only matter if metal is what the numbers call for.

### Provision

```sh
ops/pool-node-setup.sh                       # on the pool node
ops/bench-node-setup.sh --dedicated|--metal  # on the bench node; reboot if metal
ops/bench-hygiene.sh                         # must exit 0
docker compose -f platform/compose.yaml up -d   # on the web node
```

The bench setup **refuses to mark the node healthy** if hygiene fails. Do not
override it; a half-tuned node produces numbers that look fine and rank people
wrongly.

### Calibrate the bands

The band thresholds in `platform/api/src/state.rs` (`BANDS`) must be calibrated
against the reference on the **actual** bench node. Take the reference p50 from
the noise-floor run and set thresholds relative to it. Shipping the defaults
unchanged means the bands mean nothing.

### Publish the kit

```sh
ops/make-boilerplate.sh          # verifies the skeleton hash matches the editor
```

Publish `dist/me-boilerplate.zip` at 0:30 with the spec.

### Load the roster

```sql
INSERT INTO participants (handle) VALUES ('...'), ('...');
```

---

## Morning of

- [ ] Warm the bench node for **30 minutes under load** — thermal steady state.
      A cold package flatters the first submissions and nobody else.
- [ ] `ops/bench-hygiene.sh` → exits 0
- [ ] 20 reference runs; spread within what the noise floor predicted. **Record
      the median — this is the morning baseline** every later spot check is
      compared against.
- [ ] Confirm no agents or timers are active (hygiene covers this)
- [ ] `curl localhost:8081/admin/queue` through the SSH tunnel → workers healthy
- [ ] **Freeze all deploys.** From here the platform does not change.

Operator access is an SSH tunnel; there is no admin auth because the admin
listener is not reachable from the network:

```sh
ssh -N -L 8081:127.0.0.1:8081 web-node
```

---

## Timeline

| Time | Action |
|---|---|
| 0:00 | Kickoff, spec walkthrough (30 min, mandatory). Walk through the FOK 60/50/100 example — nobody arrives knowing that rule. |
| 0:30 | Publish the zip. Coding opens; correctness lane live. |
| 1:30 | Benchmark lane opens. |
| 5:15 | `POST /admin/freeze` |
| 6:00 | Submissions close. Rejudge block. Reveal. |

---

## During

Watch, in order of how early they warn you:

```sh
curl -s localhost:8081/admin/discards | jq    # machine-signal discard rate
curl -s localhost:8081/admin/queue | jq       # depth per state, worker health
curl -s localhost:8081/admin/events | jq      # janitor actions, alerts
```

**The discard rate is the earliest signal the bench node has become unstable.**
A rising rate means look now, not later.

Every ~20 minutes the bench worker re-runs the reference by itself and logs
`reference_spot_check`. Compare against the morning baseline; **>2% deviation is
an alert**, and it is contamination detection by measurement rather than by
inference — throttling, frequency drift and a mystery daemon all show up here
without any per-run classification logic.

### When something goes wrong

| Symptom | Action |
|---|---|
| A submission stuck in a working state | The janitor resets it within 3 min (15 for bench). If not, `POST /admin/requeue/{id}`. |
| Bench node sick | `POST /admin/bench/unhealthy`. Queued jobs park as `pending_benchmark` — they are held, not lost. Correctness keeps working. |
| Bench node back | `POST /admin/bench/healthy`. The janitor unparks on its next sweep. |
| Leaderboard looks wrong | `POST /admin/leaderboard/rebuild`. Redis is a disposable read model; Postgres is the truth, and the rebuild is 18 rows. |
| Same submission discarded 3× on steal time | The worker already stopped requeueing and marked the node unhealthy. A human should look before the queue churns. |
| Someone claims a lost submission | `events_log` has every janitor action and every state change. |

**Do not deploy anything.** If a fix seems urgent, it is almost certainly less
urgent than the risk of changing the thing that is currently working.

---

## After

The live leaderboard is indicative. **The rejudge is authoritative.**

1. Run the reference once, immediately before the block. Record it.
2. Rejudge every participant's final submission in **one continuous block**, on
   fresh seeds, using the **same seed set for everyone**.
3. Run the reference again immediately after.
4. **If the two reference runs disagree, rerun the block.** Two minutes of
   insurance on the only measurement that decides ranking.

This is what makes absolute scoring defensible without drift normalisation: all
final numbers come from the same short window on the same machine, so
cross-time comparability stops being required.

Then:

- [ ] Source-review the top finishers — the memory-scanning rule is enforced by
      review, not by code
- [ ] Plagiarism check across submissions
- [ ] Publish all seeds, the generator, and the reference implementation
- [ ] Publish per-participant flamegraphs
- [ ] `pg_dump` to S3 before tearing anything down

---

## Things worth saying out loud at the kickoff

- **Trade price is the resting order's price.** The most common first-submission
  failure, every time.
- **Cancel identity is the `(session_id, client_order_id)` pair.** The stream
  deliberately contains two sessions using the same id at once. An engine keyed
  on the id alone works for a long time and then cancels the wrong order.
- **The FOK same-firm exclusion (F1)** is a spec invention, not industry
  practice. Walk through 60/50/100 on the board.
- **The environment is unrealistically quiet.** Real matching engines fight
  jitter, not just cycles. Nobody should leave thinking production p99 looks
  like this.
- **The probe cost is published with every result.** If it is a large fraction
  of the p50, ranked gaps are much smaller than the real differences — say so
  before the reveal rather than after.
