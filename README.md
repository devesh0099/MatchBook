# Matchbook

A contest platform for a one-day matching-engine challenge. Participants
implement the order-matching logic of a single-instrument exchange in C++20;
submissions are **gated on correctness** and then **ranked on p50 latency per
event**.

The rules participants are held to are in [spec/SPEC.md](spec/SPEC.md), which is
normative. [WALKTHROUGH.md](WALKTHROUGH.md) is a tour of the code and
[STATUS.md](STATUS.md) says what is built and what is left.

---

## How it fits together

```
                 ┌────────── web node ──────────┐
  participants → │ Caddy → Next.js + axum API   │
                 │ Postgres 16 · Redis          │
                 └──────────────┬───────────────┘
                                │  claim-and-commit, FOR UPDATE SKIP LOCKED
                 ┌──────────────┴───────────────┐
        pool node (compile + verify, parallel)  bench node (one run at a time)
```

Three nodes that **never talk to each other** — they all poll Postgres. No
broker, no service discovery, no RPC. Workers are one binary with two roles.

Two lanes, and the difference matters:

- **Run** — compiles against 41 visible tests, returns `N/41`. Unlimited,
  seconds. Teaches the rules; does not grade.
- **Submit** — compiles to a `.so`, runs a **hidden** differential check against
  a reference engine on a fresh seed, and if that passes queues a ranked
  benchmark. This is the gate.

Participants never write a `main()`. They implement `create_engine()`, and the
harness `dlopen`s it and drives it — which is why the sandbox is mandatory and
why the timing is per-event `rdtscp` rather than process wall time.

---

## Local testing

Brings the whole platform up on one machine and puts a real submission through
it. Roughly 15 minutes, most of it compiling. Verified end to end from an empty
checkout: the last submission came back `done` 57 seconds after Submit.

### 1. Prerequisites

```sh
sudo apt-get install -y build-essential g++ cmake git pkg-config zip curl \
     libcap-dev libseccomp-dev libsystemd-dev libssl-dev
```

Also needed: **Docker** with the compose plugin, and **Rust** (via
[rustup](https://rustup.rs) — the packaged toolchain is older than
`Cargo.lock` needs).

`g++` must support C++20. Everything is compiled `-march=x86-64-v3`, so this is
an **x86-64** platform.

### 2. Install isolate

The sandbox every submission runs inside. It is setuid root, so this needs
sudo and cannot be skipped — the workers shell out to it.

```sh
sudo ops/install-isolate.sh
isolate --version          # expect: The process isolator 2.6
```

### 3. Build

```sh
cmake -S engine -B engine/build/contest -DCMAKE_BUILD_TYPE=Release
cmake --build engine/build/contest -j$(nproc)
ctest --test-dir engine/build/contest        # expect 14/14

cargo build --release --manifest-path platform/Cargo.toml
```

### 4. Start the stack

```sh
docker compose -f platform/compose.yaml -f platform/compose.local.yaml up -d --build
```

Five services plus MinIO. The local override moves ports off 80/443/5432, serves
plain HTTP, and supplies MinIO in place of S3.

Create the bucket the API writes submissions into — nothing does this for you,
and without it every `/run` and `/submit` returns an opaque `internal error`:

```sh
docker run --rm --network me-platform_default --entrypoint sh minio/mc -c \
  "mc alias set l http://minio:9000 mebench mebench123 >/dev/null && \
   mc mb --ignore-existing l/me-platform-artifacts"
```

Load a roster — there is no signup, identity is a name picked from this list:

```sh
docker exec -i me-platform-postgres-1 psql -U mebench -d mebench <<'SQL'
INSERT INTO participants (handle)
VALUES ('a.mehra'), ('b.kulkarni'), ('c.natarajan')
ON CONFLICT (handle) DO NOTHING;
SQL
```

Check it is serving:

```sh
curl -s http://localhost:8088/api/participants
```

### 5. Start the workers

Two processes, same binary. On AWS these are systemd units on separate nodes;
locally they are just two shells.

```sh
cat > /tmp/worker.env <<EOF
DATABASE_URL=postgres://mebench:mebench@127.0.0.1:15432/mebench
REDIS_URL=redis://127.0.0.1:6379
S3_ENDPOINT=http://127.0.0.1:19000
S3_BUCKET=me-platform-artifacts
AWS_REGION=us-east-1
AWS_ACCESS_KEY_ID=mebench
AWS_SECRET_ACCESS_KEY=mebench123
MEBENCH_INCLUDE=$PWD/engine/include
MEBENCH_TESTS=$PWD/engine/tests
MEBENCH_HARNESS=$PWD/engine/build/contest/harness
MEBENCH_GEN=$PWD/engine/build/contest/gen
MEBENCH_REFERENCE_SO=$PWD/engine/build/contest/libreference_engine.so
MEBENCH_CXX=/usr/bin/g++
MEBENCH_MARCH=x86-64-v3
EOF

# terminal 1
set -a; . /tmp/worker.env; set +a
BOX_ID=0 HOSTNAME=poolnode ./platform/target/release/worker --role pool

# terminal 2
set -a; . /tmp/worker.env; set +a
BOX_ID=20 HOSTNAME=benchnode ./platform/target/release/worker --role bench
```

`BOX_ID` differs only so their isolate sandboxes do not collide.

### 6. Submit something

Open **http://localhost:8088/editor**, pick a handle in the top bar, and either
write an engine or paste the reference one:

```sh
cat dist/engine.cpp        # a correct, cache-conscious engine
```

**Run** should give `41/41`. **Submit** runs the hidden gate and then the
benchmark. Expect roughly:

```
received → compiling → verifying → verify_passed → bench_queued
        → benchmarking → done
```

A ranked job takes about a minute here: it generates a 20M-event stream, then
times nine runs of it. Watch it move on the submission page, which polls every
two seconds.

### The numbers you get are not measurements

A p50 from a laptop running six containers, a browser and both workers is real
arithmetic over real samples, but it is not a benchmark. There is no core
pinning, no isolated CPU, turbo is on and something else is always running.

You will see this directly: the bench node re-measures a reference engine every
twenty minutes and marks itself **unhealthy** if the result moves more than 2%.
On a laptop it will. When that happens, submissions park at `pending_benchmark`
rather than failing — which is the designed behaviour, not a bug. Clear it with:

```sh
curl -X POST http://127.0.0.1:8081/admin/bench/healthy
```

That port is loopback-only and is the entire operator API. There is no
authentication anywhere in this platform: on a real deployment you reach it over
an SSH tunnel, and access to it *is* SSH access to the web node.

### Tear down

```sh
docker compose -f platform/compose.yaml -f platform/compose.local.yaml down -v
```

`-v` deletes the database volume, so the next `up` starts from an empty schema.

---

## Layout

| | |
|---|---|
| `engine/` | C++20 — frozen headers, reference engine, stream generator, harness, visible tests |
| `platform/api/` | axum. Two routers on two listeners: participants on `:8080`, operators on loopback `:8081` |
| `platform/worker/` | one binary, two roles, isolate integration |
| `platform/web/` | Next.js 16 — editor, results, leaderboard, spec |
| `platform/db/` | the whole data model; the queue **is** the submissions table |
| `ops/` | provisioning, bench-node tuning, noise floor, event runbook |
| `spec/SPEC.md` | the normative specification, published to participants |
