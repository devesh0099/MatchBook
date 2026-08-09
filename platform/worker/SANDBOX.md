# Sandbox limits and CPU pinning

What a submission actually gets, and which of it comes from isolate versus from
outside it. The second half matters: **isolate has no cpuset option**, so every
CPU guarantee this platform makes is enforced by the systemd unit, not by the
sandbox.

Implementation: `src/sandbox.rs` (`RunOpts`), `ops/bench-node-setup.sh` (unit).

---

## 1. What isolate enforces

Passed on every `--run`:

| Stage | `--cg-mem` | `--processes` | `--wall-time` | `--time` |
|---|---|---|---|---|
| compile | 4 GB | 16 | 60 s | 50 s |
| verify | 4 GB | **1** | 45 s (+10 s outer) | 45 s |
| bench | 8 GB | **1** | 600 s | 600 s |
| (default) | 2 GB | 1 | 60 s | 50 s |

### Memory

`--cg-mem` is a **whole-cgroup** limit on actual usage, not a per-process
address-space cap. That is the right knob here: it catches a runaway across
every process in the box, and it is what turns a runaway allocation into a clean
`SG` (OOM kill) rather than a machine-wide problem.

`--mem`, which caps a single process's address space, is deliberately **not**
set. One consequence worth knowing: a submission can `mmap` an enormous
*reservation* without being stopped, because nothing is charged until pages are
touched. It dies on first touch instead. Harmless, but not what a reader of the
limits table might assume.

Headroom: a ranked run measured **707 MB peak RSS** against the 8 GB limit — the
240 MB decoded event buffer plus a ~300k-order book.

### `RLIMIT_MEMLOCK` is 0, and cannot be raised

isolate sets the memlock limit to **zero** inside the box:

```
$ isolate --cg --box-id 32 --run -- /bin/sh -c 'ulimit -l'
0
$ ulimit -l
1953772
```

There is no option to change it — no `--memlock`, nothing in the option table.
So `mlockall(MCL_CURRENT | MCL_FUTURE)` in the harness **always fails in a
ranked run**, and every ranked result reports `memory_locked: false`.

This matters because the mitigation previously written down — add
`LimitMEMLOCK=infinity` to the systemd unit — **would not have worked**. rlimits
are inherited across `fork`/`exec`, so it looks like it should; isolate
overrides it afterwards.

What actually keeps a page fault out of the timed region:

- **swap is off**, so anonymous pages cannot be reclaimed at all — there is
  nowhere to put them. This makes swap-off load-bearing rather than tidiness,
  which is why `ops/bench-hygiene.sh` fails the node outright when it is on.
- the harness **touches the whole buffer** in the untimed load phase, so every
  page is already faulted in before measurement starts.

`mlockall` was belt and braces on top of those two. It is still attempted, and
still reported, because `no` *outside* the sandbox would mean something
different and worth seeing.

### Processes

`--processes=1` on verify and bench does two jobs. It contains fork bombs — one
of the near-certain accidents — and it blocks `clone`, so thread-based gaming of
the wall clock is not available. Compile needs 16 because `g++` genuinely forks
`cc1plus` and `as`.

> `--processes` takes an **optional** argument, so it must be written attached:
> `--processes=1`. The separated form leaves the number unconsumed, getopt stops
> at the first non-option, and isolate tries to `exec` it. It is the only option
> in isolate 2.6 declared with an optional argument.

### Time

Two limits, both real: `--wall-time` catches a sleeping or blocked process,
`--time` catches a spinning one. The harness also arms its own `SIGALRM` a few
seconds inside the isolate limit, so a hang produces a readable timeout and exit
code 3 rather than an opaque kill.

### What isolate gives without being asked

No network (a fresh network namespace), a private mount namespace, a distinct
unprivileged UID per `--box-id`, and process-group kill on timeout. The stream
is handed in on **fd 9** via `--inherit-fds`; it is never a path the box can
open.

---

## 2. What isolate does NOT do: CPU

**isolate 2.6 has no cpuset, affinity or NUMA option.** Its full long-option
table contains no `--cpus`, no `--cg-cpuset`, nothing equivalent. So none of the
following is enforced by the sandbox:

- which core a submission runs on
- which NUMA node its memory comes from
- its scheduling policy or priority
- address-space randomisation

All of it is applied **outside** isolate, in the systemd unit, and inherited by
the sandboxed process across `fork`/`exec` — affinity, NUMA policy, scheduling
class and the ASLR personality all survive `exec` unless something resets them,
and isolate resets none of them.

```
ExecStart=/usr/bin/numactl --cpunodebind=0 --membind=0 \
          /usr/bin/setarch -R /usr/bin/chrt -f 99 /usr/bin/taskset -c 4 \
          /opt/mebench/bin/worker --role bench
```

Read outward-in, that is what a ranked submission runs under:

| Wrapper | Effect | Why |
|---|---|---|
| `taskset -c 4` | one core | matches `isolcpus=4-7`; the core is off the scheduler's load balancer |
| `numactl --cpunodebind=0 --membind=0` | CPU and memory on one NUMA node | a cross-node access is a different measurement |
| `chrt -f 99` | `SCHED_FIFO` 99 | nothing else preempts a ranked run |
| `setarch -R` | ASLR off | removes cache-set-conflict luck, worth about 3% |

### The pool node is deliberately unpinned

Nothing on the correctness pool is measured, and compiles want every core they
can get. Only the bench node pins.

### Consequence worth stating plainly

**Every CPU guarantee in this platform is an inheritance assumption.** If a
future change puts something between the unit and the worker that resets
affinity — a shell wrapper, a supervisor, a container runtime — the limits table
above still looks correct while the ranked runs quietly spread across cores.

That assumption is asserted rather than trusted: `ops/bench-hygiene.sh` checks
the sandboxed process actually lands on the pinned core, and the setup script
refuses to mark the node healthy if it does not.

---

## 3. Why not just containerise it

Docker was rejected for the measurement path (plan §10) and the CPU story is
part of the reason. A container runtime does background work on the machine
during a benchmark, and its cgroup and cpuset handling is another layer between
the intent and the kernel. isolate is namespaces plus cgroups with no daemon:
`--init`, copy in, `--run`, read the meta file, `--cleanup`.

Nothing containerised runs on the benchmark node at all — the workers are plain
systemd units. Compose exists only on the web node, where nothing is measured.
