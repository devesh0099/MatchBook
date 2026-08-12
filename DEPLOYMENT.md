# Deployment

Three EC2 instances in one VPC. Only the web node is reachable by participants.

| Node | Type | vCPU / RAM | Public IP |
|---|---|---|---|
| web | `m6i.large` | 2 / 8 GB | depends on the TLS option in §B3 |
| pool | `c6i.2xlarge` | 8 / 16 GB | no |
| bench | `c6i.2xlarge`, SMT off | 4 cores presented | no |

Ubuntu 24.04 LTS, **x86_64** — everything is compiled `-march=x86-64-v3`.

This whole sequence has been run end to end on real AWS. What follows is what
worked, not what should work. Where a step exists because something failed
silently the first time, it says so.

**Part A is for whoever owns the AWS account.** It is written to be forwarded.
**Part B is for the operator** running the event.

---

# Part A — for whoever owns the AWS account

## A1. Create three things

Nothing in the platform creates these, and without them every `/run` and
`/submit` returns an opaque `internal error`.

### 1. An S3 bucket

Private, default encryption. Any name; called `<bucket>` below. It holds
submission source and compiled binaries, keyed on sha256.

### 2. An IAM role + instance profile named `me-platform-node`

Trusted by `ec2.amazonaws.com`, attached to all three instances, with **exactly
this policy and nothing else**:

```json
{
  "Version": "2012-10-17",
  "Statement": [{
    "Effect": "Allow",
    "Action": ["s3:GetObject", "s3:PutObject", "s3:ListBucket"],
    "Resource": ["arn:aws:s3:::<bucket>", "arn:aws:s3:::<bucket>/*"]
  }]
}
```

This is the entire AWS surface of the running platform. It makes no other API
call: no EC2, no Secrets Manager, no CloudWatch, no SSM.

### 3. An IAM user for the operator, with this policy

```json
{
  "Version": "2012-10-17",
  "Statement": [
    {
      "Sid": "ReadEverythingEC2",
      "Effect": "Allow",
      "Action": ["ec2:Describe*"],
      "Resource": "*"
    },
    {
      "Sid": "ManageOurInstancesInOneRegion",
      "Effect": "Allow",
      "Action": [
        "ec2:RunInstances", "ec2:TerminateInstances",
        "ec2:StartInstances", "ec2:StopInstances",
        "ec2:CreateTags", "ec2:ModifyInstanceAttribute",
        "ec2:CreateSecurityGroup", "ec2:DeleteSecurityGroup",
        "ec2:AuthorizeSecurityGroupIngress", "ec2:RevokeSecurityGroupIngress",
        "ec2:AuthorizeSecurityGroupEgress", "ec2:RevokeSecurityGroupEgress",
        "ec2:CreateKeyPair", "ec2:ImportKeyPair", "ec2:DeleteKeyPair"
      ],
      "Resource": "*",
      "Condition": { "StringEquals": { "aws:RequestedRegion": "<region>" } }
    },
    {
      "Sid": "AttachOnlyOurRoleToInstances",
      "Effect": "Allow",
      "Action": ["iam:PassRole"],
      "Resource": "arn:aws:iam::<account>:role/me-platform-node",
      "Condition": { "StringEquals": { "iam:PassedToService": "ec2.amazonaws.com" } }
    },
    {
      "Sid": "OurBucketOnly",
      "Effect": "Allow",
      "Action": ["s3:GetObject", "s3:PutObject", "s3:ListBucket", "s3:GetBucketLocation"],
      "Resource": ["arn:aws:s3:::<bucket>", "arn:aws:s3:::<bucket>/*"]
    }
  ]
}
```

**No IAM write. One region. One bucket.** This exact policy has been tested by
running the deployment under it — `RunInstances` with the instance profile
attached, `CreateSecurityGroup` and `ImportKeyPair` all pass, the same calls in
another region are denied, and `iam:ListUsers` is denied.

Three notes for whoever reviews it:

- **`iam:PassRole` is the one people cut.** Launching an instance *with* an
  instance profile is handing a role to EC2, and AWS treats that as a privilege
  separate from creating the role. Without it `RunInstances` fails complaining
  about passing a role, which reads like a malformed command rather than a
  missing permission.
- **`ec2:Describe*` must be `"Resource": "*"`.** Describe actions do not support
  resource-level permissions. That is an AWS constraint, not laziness — the
  region condition on the write actions is where the scoping lives.
- **`s3` on the operator policy is not used by the tooling.** It is there so a
  human can inspect artifacts without a second credential. Cut it if you prefer.

`infra/bootstrap/` in this repo creates all three as Terraform, if that is
easier than clicking. `infra/bootstrap/deployer-policy.json.tftpl` is the
policy above, templated.

## A2. Tell us four things

- **Region**, and the **VPC id** and **subnet id** to launch into. The subnet
  must have a route to the internet — the setup pulls apt packages, the Rust
  toolchain and GitHub.
- **May the web node have a public IP**, or must it sit behind your load
  balancer? This decides §B3 and who terminates TLS.
- **A hostname**, if you want HTTPS. Any subdomain of something you already own.
  Without one the platform serves plain HTTP on a bare IP, which works but shows
  a "not secure" badge to every participant.
- **How we reach the machines**: an SSH key pair, or SSM Session Manager. If
  SSM, the `me-platform-node` role also needs `AmazonSSMManagedInstanceCore` —
  worth deciding now rather than editing the role after launch.

## A3. Things that can block us, that we cannot test for you

- **Service Control Policies.** An org-level deny sits on top of the user policy
  and no test in another account reveals it. If your org restricts regions,
  instance types, or public IPs, say so.
- **Permission boundaries** on the user you create — same reasoning.
- **Dedicated tenancy**, *only if we ask*. We measured a shared instance and it
  is fine (§C2), so this is unlikely. If it comes up it needs no quota increase
  — Dedicated *Instances* count against the ordinary On-Demand vCPU quota.
  Dedicated **Hosts** are a different product whose quota is often zero; do not
  let the two be conflated.

## A4. Cost

Roughly **$0.78/hour** for all three nodes, so **under $20** for calibration, a
rehearsal and the event day combined, if the instances are destroyed afterwards.
The real cost risk is leaving them running: a forgotten week is ~$130.

---

# Part B — for the operator

## B0. Before you start

You need the account details from §A2, an SSH key pair, and Terraform ≥ 1.5.

Generate a key dedicated to this rather than reusing one — revoking it later
then touches nothing else:

```sh
ssh-keygen -t ed25519 -f ~/.ssh/me-platform -C me-platform
```

**Verify the halves match before launching.** A mismatched key pair produces
three healthy instances you cannot log into:

```sh
diff <(ssh-keygen -y -f ~/.ssh/me-platform) <(cat ~/.ssh/me-platform.pub) \
  && echo "key pair is consistent"
```

## B1. Provision

```sh
cp infra/terraform.tfvars.example infra/terraform.tfvars   # fill in from §A2
terraform -chdir=infra init
terraform -chdir=infra plan          # creates nothing
terraform -chdir=infra apply
```

If you own the account, run `terraform -chdir=infra/bootstrap apply` first — it
creates the three things in §A1 and prints the values for `terraform.tfvars`.

Two settings in `infra/main.tf` are **launch-time only** and cannot be changed
on a running instance:

- `tenancy` — shared by default. See §C2 before changing it.
- `cpu_options { threads_per_core = 1 }` — SMT off. A sibling thread sharing the
  core with a timed run is exactly the contamination the design refuses. With
  SMT off, `c6i.2xlarge` presents **4 cores**, which is what `BENCH_CPU` and
  `ISOLATED_CPUS` must be set against.

Wait for cloud-init before connecting — `user_data` installs Docker, the build
toolchain and rustup, then clones the repo:

```sh
until ssh -i ~/.ssh/me-platform ubuntu@<web-ip> \
      'test -f /var/lib/cloud/me-platform-ready'; do sleep 10; done
```

`terraform output` then gives you the SSH commands, the web node's **private
IP**, and ready-to-paste `web_env` / `worker_env` blocks with that IP already
substituted. Use them — a wrong private IP here is the most common way a
three-node deployment fails silently.

## B2. Web node

```sh
cd /opt/me-platform

export DB_BIND='<web private IP>'        # NOT loopback; workers cannot reach that
export POSTGRES_PASSWORD='<something long>'
export AWS_REGION='<region>'
export S3_BUCKET='<bucket>'
export SITE_ADDRESS='...'                # value depends on §B3 — set it first

docker compose -f platform/compose.yaml up -d --build
```

- `POSTGRES_PASSWORD` is read **only when the cluster is first initialised**.
  Changing it later means deleting the `pgdata` volume.
- Every one of these must be exported on **every** later `docker compose` call
  in this directory. Omitting `POSTGRES_PASSWORD` on a subsequent `up` restarts
  the API against the wrong credentials and it crash-loops on
  `password authentication failed`.

The first build compiles the Rust API in release mode on the node. Expect
several minutes.

## B3. TLS and hosting

Both options put the frontend and API on **one origin**; Caddy owns the `/api/*`
split. Keep it in the path either way or CORS breaks.

### Option A — a hostname, Caddy terminates TLS

```
browser ──https──▶ Caddy (web node) ──▶ Next.js  /*
                                    └──▶ axum    /api/*  (prefix stripped)
```

1. A record: `matcher.example.com` → the web node's **public** IP.
2. `export SITE_ADDRESS='matcher.example.com'` before `docker compose up`.
3. Open **80 and 443** on `me-web`.

Caddy gets a Let's Encrypt certificate on first boot and renews it itself.

> **Port 80 must be reachable from the public internet for issuance.** Let's
> Encrypt validates over HTTP-01 from its own servers. Restricted to the room's
> IP, issuance fails with a challenge error rather than a firewall error.

Use an **Elastic IP** if you point DNS at it. EC2 public IPs change on
stop/start, and the A record would break silently.

### Option B — behind an existing proxy or ALB

```
browser ──https──▶ their proxy ──http──▶ Caddy (web node) ──▶ Next.js  /*
                                                          └──▶ axum    /api/*
```

1. `export SITE_ADDRESS=':80'` — plain HTTP, no ACME, no redirect to 443.
2. Open **80 from their proxy's security group**; drop the public IP.
3. Forward the hostname to the web node's **private IP, port 80, path
   unchanged**. Health check `/api/health`.

Constraints to pass to whoever owns the proxy:

- **Do not strip a path prefix.** Host-based routing only. Serving under
  `example.com/matcher` needs a Next.js `basePath` at build time and is not
  supported here.
- **Do not proxy `/admin`.** Operator routes are on loopback `:8081`; Caddy does
  not route them. Reaching the API container directly exposes every operator
  route with no authentication anywhere.
- No WebSockets, no streaming, no sticky sessions. JSON polling only.

> `SITE_ADDRESS` reaches Caddy through an `environment:` block in
> `compose.yaml`. Exporting it on the host alone used to do nothing, and both
> options above failed in confusing ways — a real domain served as `localhost`,
> or `:80` answering `308` to HTTPS with a self-signed certificate. Fixed, but
> if you see either symptom, that variable is not reaching the container.

### Verify

```sh
curl -s http://<host>/api/health          # ok
curl -s http://<host>/api/participants    # JSON, not 500
```

## B4. Pool node

```sh
sudo ops/pool-node-setup.sh
```

Worker count is `min(nproc - 2, POOL_BOXES)`, `POOL_BOXES` defaults to 8 — so
**6 on a `c6i.2xlarge`**. Raise with `sudo POOL_BOXES=14 ops/pool-node-setup.sh`.

A Run job is ~2s of compile, so 6 slots is far more than 18 people can
saturate. Note the slots are shared between both lanes and Run is claimed first;
under sustained Run traffic, Submit verification queues behind it.

## B5. Bench node

```sh
sudo BENCH_CPU=2 ISOLATED_CPUS=2-3 ops/bench-node-setup.sh --dedicated
```

`--dedicated` is the script's **tuning mode** (runtime tuning only, no boot
parameters) and is unrelated to AWS tenancy. Use `--metal` only on bare metal.

The script runs the hygiene assertions itself and **refuses to mark the node
healthy if they fail**. Do not override that. On a VM these remain warnings and
are expected: `isolcpus`, `nohz_full`, `rcu_nocbs`, `mitigations=off`, hugepages,
and the CPU governor — there is no cpufreq driver inside an EC2 guest.

## B6. Wire the workers

On **both** worker nodes, `/opt/mebench/worker.env` — the generated file has a
placeholder host that resolves to nothing:

```
DATABASE_URL=postgres://mebench:<password>@<web private IP>:5432/mebench
S3_BUCKET=<bucket>
AWS_REGION=<region>
```

```sh
systemctl enable --now mebench-pool@{0..5}    # 0..N-1, N from §B4
systemctl enable --now mebench-bench          # bench node
```

Confirm they registered — this is the step that proves the security group rule
and `DB_BIND` are both right:

```sh
docker exec -i me-platform-postgres-1 psql -U mebench -d mebench \
  -c 'SELECT id, role, healthy, now()-last_seen FROM workers ORDER BY role;'
```

Seven rows, all `healthy = t`.

## B7. Roster

There is no signup — identity is a name picked from this list.

```sh
docker exec -i me-platform-postgres-1 psql -U mebench -d mebench <<'SQL'
INSERT INTO participants (handle)
VALUES ('a.mehra'), ('b.kulkarni') /* … 18 rows … */
ON CONFLICT (handle) DO NOTHING;
SQL
```

## B8. Verify the deployment

```sh
# instance role is attached (the AWS CLI is NOT installed and is not needed —
# the SDK reads credentials from the metadata service)
T=$(curl -s -X PUT http://169.254.169.254/latest/api/token \
      -H "X-aws-ec2-metadata-token-ttl-seconds: 60")
curl -s -H "X-aws-ec2-metadata-token: $T" \
  http://169.254.169.254/latest/meta-data/iam/security-credentials/

# Postgres and the operator API must NOT be reachable from outside
nc -zv <web-public-ip> 5432        # refused
nc -zv <web-public-ip> 8081        # refused
```

Then drive one submission through the frontend: **Run** should give `41/41`, and
**Submit** should walk `received → compiling → verifying → verify_passed →
bench_queued → benchmarking → done` in about 70 seconds.

## B9. Operator access

Nine `/admin/*` routes on loopback `:8081`. **No token, no password** — access to
the admin API *is* SSH access to the web node. That is the entire auth story;
unreachability is the mechanism.

```sh
ssh -i ~/.ssh/me-platform -N -L 8081:127.0.0.1:8081 ubuntu@<web-ip>

curl -s  localhost:8081/admin/queue | jq       # workers, queue depth
curl -X POST localhost:8081/admin/freeze       # freeze the standings
curl -X POST localhost:8081/admin/bench/healthy
curl -X POST localhost:8081/admin/bench/unhealthy
```

If SSH keys are not permitted, the same forward over SSM:

```sh
aws ssm start-session --target <instance-id> \
  --document-name AWS-StartPortForwardingSession \
  --parameters '{"portNumber":["8081"],"localPortNumber":["8081"]}'
```

## B10. Before the event

```sh
ops/make-boilerplate.sh          # publish dist/me-boilerplate.zip with the spec
```

- Drive one submission through all three nodes (§B8).
- Stop the bench worker mid-queue and confirm jobs park at `pending_benchmark`
  and unpark when it returns.
- Warm the bench node **30 minutes under load** on the morning — a cold package
  flatters the first submissions and nobody else.

Event-day operations are in [ops/runbook.md](ops/runbook.md).

## B11. Teardown

```sh
terraform -chdir=infra destroy             # stops the meter
terraform -chdir=infra/bootstrap destroy   # only if you own the account
```

`pg_dump` to S3 first if the results matter. Note `force_destroy_bucket` is
`true` by default, which is right for a test and wrong for the real event — the
bucket holds every submission's source.

## Checklist

- [ ] Bucket, `me-platform-node` role, operator user all exist (§A1)
- [ ] `terraform plan` succeeds under the operator's own credentials
- [ ] SSH key halves verified to match **before** launch
- [ ] Bench instance launched with `threads_per_core = 1`
- [ ] `DB_BIND` is the web node's **private** IP
- [ ] `POSTGRES_PASSWORD` exported before the first `up`, and on every later one
- [ ] `SITE_ADDRESS` reaches the Caddy container (`docker exec … printenv`)
- [ ] `me-web` allows 5432 from the `me-worker` **security group**
- [ ] Seven workers registered and healthy
- [ ] Roster loaded
- [ ] `ops/bench-hygiene.sh` exits 0 with `BENCH_CPU` passed explicitly
- [ ] Postgres and `:8081` refused from the public IP
- [ ] One submission end to end across all three nodes

---

# Part C — what has been measured

Numbers from a real deployment on `c6i.2xlarge` (Ice Lake Xeon 8375C, **54 MB
L3**), not estimates.

## C1. It works

Full submission `done` in **72 s**: p50 96.6 ns, p99 360 ns, probe cost 12.4 ns,
9 runs, **0 discards**. Run lane `41/41`.

## C2. Shared tenancy is enough

200 independent runs of the reference on a shared instance:

| | |
|---|---|
| single-run spread | 4.46% (max−min) |
| single-run IQR | 1.05% |
| **median-of-9 stability** | **±0.6%** |
| steal-time discards | **0 / 200** |

Two engines 2% apart are ranked correctly **100%** of the time; 1% apart, 98.8%.

`analyze.py` judges on single-run `max−min` and will report the middle band —
but a score is the median of nine runs, so IQR is the number that predicts
stability. **Do not pay for dedicated tenancy for the live event.** It adds a
flat $2/hr per region — about 59× the per-instance premium — and roughly triples
the bill.

The cheap insurance is dedicated tenancy **for the rejudge block only**: launch
a second bench node of the same instance type at the end, ~2 hours, ~$5. If you
do, three things are mandatory:

1. **Stop the old bench worker first.** Nothing prevents two bench workers
   running at once, and `SKIP LOCKED` means they would take different jobs and
   benchmark concurrently — the exact contamination the single-node design
   exists to prevent.
2. **`DELETE FROM settings WHERE key = 'bench_reference_baseline_ns';`** The
   baseline is global, not per-node, so a fresh node compares against the old
   node's number, exceeds the 5% tolerance, and parks the whole queue.
3. **Same instance type.** The ranked depth is calibrated to this L3.

## C3. The ranked depth is correct

Measured at 20M events, both engines, on 54 MB L3:

| `live_target` | depth | levels | reference | optimized | ratio |
|---|---|---|---|---|---|
| 252 | 53,116 | 12,363 | 314.5 ns | 70.3 ns | 4.47× |
| 756 | 153,390 | 14,071 | 422.1 ns | 78.6 ns | 5.37× |
| 1512 | 301,666 | 14,462 | 491.7 ns | 85.5 ns | 5.75× |
| **3780** | **732,279** | **14,794** | **615.2 ns** | **102.8 ns** | **5.98×** |

**Ship 3780.** On a 16 MB laptop the same config collapsed to 3.50× because both
engines left cache; on 54 MB it is the best-discriminating depth measured.

Do not raise it without also raising `BENCH_EVENTS`. Warm-up is
`n_sessions × live_target × 8` and the harness clips it to half the stream, so
`live_target 5670` needs 14.5M warm-up events against a 10M cap and times a
book at ~69% of target. 3780 needs 9.68M against that cap — the deepest
configuration 20M events can measure honestly.

Levels are **98.6% saturated** (14,794 of ~15,000), so added depth goes vertical
— 49 orders per level at 3780, 73 at 5670 — rather than wider.

## C4. Still not measured

- **Drift across a full day.** `ops/noise-floor/soak.sh` runs 6 hours and has
  not been run. The rejudge block is the designed defence: it collapses every
  final number into one short window on one machine.
- **A cache-sensitive submission against a noisy neighbour.** The noise floor
  measures the reference against itself. A co-tenant evicting L3 is invisible to
  every counter available inside the guest — see `PLAN-ratio-scoring.md`.
- **18 people at once.** Everything here was measured with one submitter.
