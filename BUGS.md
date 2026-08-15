# Bug & punch list — from the 10-agent scale test (2026-08-15)

Found by running a real contest against the live fleet: 10 Sonnet subagents each
wrote their own C++ matching engine and competed, 2 human accounts, a 12-box
`c6i.xlarge` fleet, and a golden-box Phase III rejudge. Eight of ten agents
cleared the full ladder; the exercise surfaced the items below.

Severity: **BLOCKER** (fix before any real event) · **IMPORTANT** (real defect)
· **MINOR** (UX / event-day gotcha) · **VALIDATED** (looked like a bug, was
correct behaviour) · **TOOLING** (operator-side, not the platform).

---

## BLOCKER

### B1 — Internal calibration engine ships in participant-readable materials
`engine/tests/engines/optimized.cpp` (the near-optimal reference the platform
uses to validate ladder discrimination) is in the repo tree a contestant can
read. **Three independent expert agents each found it and used its architecture
as their starting design.** On a real event this hands out a top-tier engine.
- Fix: exclude `tests/` internals (and anything non-boilerplate) from the
  published kit; audit `ops/make-boilerplate.sh` output.

### B2 — systemd ordering cycle: the agent unit cannot start via systemd
`mebench-agent → mebench-tune → multi-user.target → mebench-agent` is a
dependency cycle; `systemctl start mebench-agent` fails with "Unable to break
cycle." Every contest box happened to start pre-reboot, so it only surfaced on
the golden box — which is exactly the event-critical one.
- Fix: correct the `After=`/`Before=`/`Wants=` graph in
  `ops/agent-node-setup.sh` so `mebench-tune` and `mebench-agent` don't order
  through `multi-user.target` back onto the agent.

### B3 — No single-instance guard on the worker
A single launch produced **two worker processes on the same `box_id`**, which
collided on isolate sandbox box 0 and errored 5 of 9 rejudge jobs.
- Fix: a PID/flock guard in the worker, or a systemd unit that guarantees one
  instance per box_id; refuse to start if another worker holds the box.

---

## IMPORTANT

### I1 — `coid` truncation in optimized.cpp
Cancel-index key packs as `session<<48 | (client_order_id & 48 bits)` — silently
truncates order ids above 48 bits. Latent (the generator stays in range) but
this is the *calibration* engine, so its correctness matters. (Found by an agent
studying it.)

### I2 — Stale isolate box not cleaned across worker restarts
A killed worker leaves box 0 locked; startup `cleanup_blocking` didn't clear it
when a different process had claimed it. Compounds B3.

### I3 — boxes.participant_id FK crash-loop  *(fixed in-session)*
The first box to register before the roster was loaded crash-looped on the
foreign key. Fixed by dropping the FK (registry records a *claim*, validated
elsewhere).

### I4 — HOSTNAME not inherited by systemd → identity collision  *(fixed in-session)*
Every agent computed worker id `agent-worker-0` and fought over one registry
row. Fixed with `Environment=HOSTNAME=%H` + `/etc/hostname` fallback in the
worker.

### I5 — agent-node-setup.sh committed non-executable (644)  *(fixed in-session)*
Fleet deploy failed with "command not found." Fixed to 755.

### I6 — docker-compose `$$` interpolation eats a dollar  *(worked around)*
`ADMIN_PASSWORD` containing `$$` reached the container as `$`, so operator login
rejected the correct password. `.env` needs the value doubled (`$$$$`); document
this for event-day credential setup.

---

## MINOR / event-day

### M1 — p50 shown, p95 ranks: contestant confusion
The submission and leaderboard pages surface p50 prominently, but ranking is
**p95-first**. Three agents optimized p50 and were confused when their "better"
submission ranked lower. The ranking is correct; the page just never says what
it ranks on.
- Fix: label the board/submission page "ranked on p95".

### M2 — Shipped bundle leaks workload internals
An agent read `generator.cpp` to derive exact price bands and pre-size arrays.
Audit what goes in the participant kit.

### M3 — AMI carries stale state
The image (baked from a 4-core box) hardcodes `isolcpus=1-3` (wrong but harmless
on the 2-core xlarge) and retains the source box's journal, which made golden
diagnostics misleading. Re-bake the AMI from a 2-core box; scrub journald before
imaging.

### M4 — Dedicated-tenancy vCPU quota
Golden box on dedicated tenancy needs an AWS vCPU limit ≥257 (account is at
256). File the limit-increase before the event if dedicated tenancy is wanted
for the rejudge hour.

### M5 — 30-minute recovery window  *(fixed in-session)*
Too slow for a 6-hour contest. Fixed: startup reclaim (~10 s) + dead-box
heartbeat sweep (~3 min); the 30-min staleness rule is now only the deep
backstop.

---

## VALIDATED — the platform working (no fix)

- **Hidden differential verify caught a real STP double-bookkeeping bug** in an
  agent's engine that the 41 visible tests missed — the invariant/snapshot layer
  doing its job.
- **The one-evaluation-at-a-time 409 fired** on a live racing double-submit.
- **"Stale leaderboard score"** (an agent's complaint) was correct p95-first
  ranking keeping the better-p95 submission.
- **The ladder discriminated the full skill range** (L2 novice → L8 experts);
  an 8-at-L8 field resolved on p95 alone, no ties.
- **Phase III re-ranked the field** — bob went #3 (own box) → #1 (golden box,
  sealed seed), demonstrating the golden box removes per-box variance.

---

## TOOLING — operator-side, not the platform

- CGNAT link broke long SSH sessions and IP allowlists (worked around by running
  orchestration on the web node).
- Monitor's `ec2 describe-instance-status` used an invalid `tag:` filter and
  silently errored — health heartbeats never posted. (Resolve instance IDs by
  tag first.)
- Several rejudge failures were operator orchestration errors (competing
  scripts; stopping a box the golden setup depended on), not platform defects.
