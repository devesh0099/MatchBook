# AWS deployment scripts

Wrappers around the deployment [DEPLOYMENT.md](../../DEPLOYMENT.md) describes by
hand. The document remains the explanation of *why*; these are the *how*, run
often enough that they stay correct.

Nothing here takes a hostname or an IP as an argument. Everything derives from
`terraform output`, because the failure that costs an event is a stale address
pasted from yesterday into a command that still looks right.

## First deployment

```sh
ops/aws/bootstrap.sh          # once per account: bucket, IAM, and a tfvars file
$EDITOR infra/terraform.tfvars  # vpc_id, subnet_id, ami_id
ops/aws/preflight.sh          # checks all of it against the real account
ops/aws/deploy.sh             # launch, provision, wire up, verify
```

`preflight.sh` creates nothing and is the one to run twice. It catches the
failures that are expensive or silent: a key pair whose halves do not match
(three healthy instances nobody can log into), a subnet with no route to the
internet (cloud-init hangs and the readiness marker never appears), a vCPU quota
too low for the third instance, an AMI that is arm64 or from another region, and
a `bench_core_count` the instance type will reject at launch.

## Day to day

| | |
|---|---|
| `status.sh` | what is running, worker health, queue depth, disk, and cost so far |
| `verify.sh` | the acceptance checks from DEPLOYMENT.md §10; exits non-zero on failure |
| `ssh.sh <role> [cmd]` | connect to `web`, `pool` or `bench` |
| `tunnel.sh` | forward the operator API to `localhost:8081` |
| `logs.sh <role> <what>` | `api`, `worker`, `cloud-init`, `tune`, `all`; `-f` to follow |
| `backup.sh` | `pg_dump` to local disk and to the artifact bucket |
| `destroy.sh` | back up, then stop the meter |

`verify.sh` is worth running on the event morning as well as after a deploy — it
checks that the pool and bench toolchains are byte-identical, that swap is off on
the bench node, that exactly one bench worker is registered, and that Postgres
and the unauthenticated operator API both refuse connections from the public IP.

## Resuming a partial deploy

`deploy.sh` runs five phases: `infra`, `wait`, `build`, `connect`, `verify`. The
three nodes are provisioned **in parallel** during `build` — each takes five to
ten minutes on its own, and doing them in sequence is most of half an hour.
Per-node output goes to `.state/logs/<role>.log`, and a failure prints the tail
of the one that failed.

```sh
ops/aws/deploy.sh --only bench    # re-provision one node after fixing something
ops/aws/deploy.sh --from connect  # skip straight to wiring up the workers
```

Every phase is idempotent. The one thing that is not is `POSTGRES_PASSWORD`,
which Postgres reads only when the cluster is first initialised — so it is
generated once into `.state/secrets.env` and reused from then on. Changing it
afterwards means deleting the `pgdata` volume.

## What deploy.sh does that the manual steps do not

**Writes `platform/.env` instead of exporting variables.** Compose reads that
file on *every* invocation. Exported variables have to be re-exported on every
later `docker compose` call, and forgetting `POSTGRES_PASSWORD` on a subsequent
`up` restarts the API against credentials it cannot use, where it crash-loops on
`password authentication failed for user "mebench"`.

**Derives the bench node's CPU layout from `nproc`.** `bench-node-setup.sh`
defaults to `BENCH_CPU=4 ISOLATED_CPUS=4-7`, which describes an 8-CPU box; a
`c6i.2xlarge` with `threads_per_core = 1` presents 4, so those defaults name CPUs
that do not exist. The script now rejects that instead of half-applying it.

**Enables the pool units that exist** rather than a hardcoded `{0..5}`. The count
is `min(nproc - 2, POOL_BOXES)`, so changing either the instance type or
`POOL_BOXES` otherwise leaves workers unstarted or fails on units that were never
created.

**Waits for workers to register.** If `DB_BIND` or the 5432 security group rule
is wrong, workers do not report an error — they simply never appear in the
`workers` table, which is why registration is the check rather than the logs.

## `.state/`

Gitignored, and holds the generated Postgres password, a `known_hosts` scoped to
this deployment (AWS recycles public IPs, so a shared `known_hosts` produces a
frightening and irrelevant host-key warning), per-node provisioning logs, and
database backups. `destroy.sh` clears the password and host keys but deliberately
keeps the backups — after teardown they are the only remaining record of the
event.
