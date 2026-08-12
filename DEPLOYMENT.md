# Deploying the matching-engine contest platform

A complete deployment, start to finish. Every step here has been run on real
AWS; where a step exists because something failed silently the first time, it
says so.

Three EC2 instances in one VPC. Only the web node is reachable by participants.

| Node | Type | vCPU / RAM | Public IP | Runs |
|---|---|---|---|---|
| web | `m6i.large` | 2 / 8 GB | see §5 | Caddy, Next.js, axum API, Postgres, Redis |
| pool | `c6i.2xlarge` | 8 / 16 GB | no | 6 correctness workers |
| bench | `c6i.2xlarge`, SMT off | 4 cores presented | no | 1 benchmark worker |

Ubuntu 24.04 LTS, **x86_64** — everything is compiled `-march=x86-64-v3`, so
arm64 is not an option.

Roughly **$0.78/hour** for all three. Under **$20** covers calibration, a
rehearsal and the event day, provided the instances are destroyed afterwards.
The real cost risk is forgetting to: a month of idle instances is ~$560.

---

## What you need before starting

- An AWS account, and a region to deploy into.
- A **VPC id** and **subnet id**. The subnet must have a route to the internet —
  setup pulls apt packages, the Rust toolchain and GitHub.
- **Terraform ≥ 1.5** and the **AWS CLI**, configured with a profile.
- A decision on how participants reach the site: a **hostname you control**, or
  a **slot behind an existing proxy or load balancer**. See §5. Without either,
  the platform serves plain HTTP on a bare IP — which works, but every
  participant sees a "not secure" badge.

Permissions needed: creating an S3 bucket, an IAM role and instance profile,
security groups, and EC2 instances. §2 gives the exact policy if you want to
scope it tightly rather than deploying as an administrator.

---

## 1. SSH key

Generate a key dedicated to this deployment. Revoking it later then affects
nothing else.

```sh
ssh-keygen -t ed25519 -f ~/.ssh/me-platform -C me-platform
```

**Verify both halves match before launching anything.** A mismatched key pair
produces three healthy instances nobody can log into, and the only recovery is
detaching the root volume and mounting it elsewhere.

```sh
diff <(ssh-keygen -y -f ~/.ssh/me-platform) <(cat ~/.ssh/me-platform.pub) \
  && echo "key pair is consistent"
```

---

## 2. Bucket and IAM

Nothing in the platform creates these. Without them every `/run` and `/submit`
returns an opaque `internal error`, because the API stores submission source in
S3 before it does anything else — this is the one provisioning step with no
error message pointing at it.

```sh
terraform -chdir=infra/bootstrap init
terraform -chdir=infra/bootstrap apply
```

That creates three things and prints the values the next step needs.

**An S3 bucket**, private and encrypted. Holds submission source and compiled
binaries, keyed on sha256.

**`me-platform-node`** — an IAM role and instance profile attached to all three
instances. This is the entire AWS surface of the running platform:

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

No EC2, no Secrets Manager, no CloudWatch, no SSM.

**`me-platform-deployer`** — a role carrying the deployment permissions. Use it
if the rest of the deployment should run under a scoped policy rather than as an
administrator; set `deployer_role_arn` in §3. The same document works attached
to a user instead, if you would rather issue credentials directly:

```json
{
  "Version": "2012-10-17",
  "Statement": [
    { "Sid": "ReadEverythingEC2", "Effect": "Allow",
      "Action": ["ec2:Describe*"], "Resource": "*" },

    { "Sid": "ManageOurInstancesInOneRegion", "Effect": "Allow",
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
      "Condition": { "StringEquals": { "aws:RequestedRegion": "<region>" } } },

    { "Sid": "AttachOnlyOurRoleToInstances", "Effect": "Allow",
      "Action": ["iam:PassRole"],
      "Resource": "arn:aws:iam::<account>:role/me-platform-node",
      "Condition": { "StringEquals": { "iam:PassedToService": "ec2.amazonaws.com" } } },

    { "Sid": "OurBucketOnly", "Effect": "Allow",
      "Action": ["s3:GetObject", "s3:PutObject", "s3:ListBucket", "s3:GetBucketLocation"],
      "Resource": ["arn:aws:s3:::<bucket>", "arn:aws:s3:::<bucket>/*"] }
  ]
}
```

Three things worth knowing if you trim it:

- **`iam:PassRole` is the one people cut.** Launching an instance *with* an
  instance profile is handing a role to EC2, and AWS treats that as a privilege
  separate from creating the role. Without it `RunInstances` fails complaining
  about passing a role, which reads like a malformed command rather than a
  missing permission.
- **`ec2:Describe*` must be `"Resource": "*"`.** Describe actions do not support
  resource-level permissions; the region condition on the write actions is where
  the scoping lives.
- **The `s3` block is not used by Terraform.** It is there so a human can inspect
  artifacts without a second credential.

If a Service Control Policy or permission boundary applies to the account, it
sits on top of all of this. Restrictions on regions, instance types or public IPs
surface as an `AccessDenied` naming the action.

---

## 3. Launch the instances

```sh
cp infra/terraform.tfvars.example infra/terraform.tfvars
```

Fill in the region, `vpc_id`, `subnet_id`, `ami_id` (Ubuntu 24.04 LTS x86_64),
the bucket and instance profile from §2, and `public_key_path`. Set
`deployer_role_arn` only if Terraform should assume a role rather than use the
profile's own credentials.

```sh
terraform -chdir=infra init
terraform -chdir=infra plan          # creates nothing
terraform -chdir=infra apply
```

Two settings are **launch-time only** and cannot be changed on a running
instance:

- **`cpu_options { threads_per_core = 1 }`** — SMT off. A sibling thread sharing
  a core with a timed run is exactly the contamination the design refuses. With
  SMT off a `c6i.2xlarge` presents **4 cores**, which is what `BENCH_CPU` and
  `ISOLATED_CPUS` must be set against in §7.
- **`tenancy`** — shared by default, which is correct. See §11 before changing it.

Security groups are created for you. The rule that matters is Postgres on the
web node accepting **from the worker security group** rather than from a CIDR,
so membership is dynamic and replacing an instance needs no rule change.

Wait for cloud-init before connecting. It installs Docker, the build toolchain
and rustup, then clones the repo to `/opt/me-platform`:

```sh
until ssh -i ~/.ssh/me-platform ubuntu@<web-ip> \
      'test -f /var/lib/cloud/me-platform-ready'; do sleep 10; done
```

`terraform output` gives the SSH commands, the web node's **private IP**, and
ready-to-paste environment blocks with that IP already substituted. Use them —
a wrong private IP is the most common way a three-node deployment fails
silently.

---

## 4. Web node

```sh
ssh -i ~/.ssh/me-platform ubuntu@<web-ip>
cd /opt/me-platform

export DB_BIND='<web private IP>'        # NOT loopback — workers cannot reach that
export POSTGRES_PASSWORD='<something long>'
export AWS_REGION='<region>'
export S3_BUCKET='<bucket>'
export SITE_ADDRESS='...'                # decided in §5 — set it before running

docker compose -f platform/compose.yaml up -d --build
```

The first build compiles the Rust API in release mode on the node. Expect
several minutes.

Two things that bite:

- **`POSTGRES_PASSWORD` is read only when the database is first initialised.**
  Changing it later means deleting the `pgdata` volume.
- **Export all of these on every later `docker compose` call**, not just the
  first. Omitting `POSTGRES_PASSWORD` on a subsequent `up` restarts the API
  against the wrong credentials, and it crash-loops on
  `password authentication failed for user "mebench"`.

---

## 5. How participants reach the site

Both options put the frontend and the API on **one origin** — Caddy owns the
`/api/*` split. Keep it in the path either way, or the browser starts making
cross-origin requests and CORS breaks.

### Option A — a hostname, Caddy terminates TLS

Caddy holds the certificate. One hop, nothing else to configure.

```
browser ──https──▶ Caddy (web node) ──▶ Next.js  /*
                                    └──▶ axum    /api/*  (prefix stripped)
```

1. **A record:** `matcher.example.com` → the web node's **public** IP.
2. On the web node, before `docker compose up`:
   ```sh
   export SITE_ADDRESS='matcher.example.com'
   ```
3. Open **80 and 443** on the `me-web` security group.

Caddy requests a Let's Encrypt certificate on first boot and renews it itself.

> **Port 80 must be reachable from the public internet for issuance.** Let's
> Encrypt validates over HTTP-01 from its own servers. If 80 is restricted to
> the venue's IP, issuance fails with a challenge error rather than a firewall
> error, which sends you looking in the wrong place. Open it, confirm the
> certificate, then narrow the rule if you want.

Use an **Elastic IP** if DNS points at the instance. EC2 public IPs change on
stop/start and the A record would break silently.

### Option B — behind an existing proxy or load balancer

Your edge holds the certificate. The web node needs **no public IP**.

```
browser ──https──▶ your proxy ──http──▶ Caddy (web node) ──▶ Next.js  /*
                                                          └──▶ axum    /api/*
```

1. On the web node, before `docker compose up`:
   ```sh
   export SITE_ADDRESS=':80'      # plain HTTP, no ACME, no redirect to 443
   ```
2. Allow **80 from the proxy's security group** on `me-web`; remove any public
   80/443 rules and the public IP.
3. Forward the hostname to the web node's **private IP, port 80, path
   unchanged**:

   ```nginx
   location / {
       proxy_pass http://172.31.9.101:80;
       proxy_set_header Host              $host;
       proxy_set_header X-Forwarded-Proto $scheme;
   }
   ```

   ALB equivalent: target group → web node, port 80, protocol HTTP.
4. Health check: **`/api/health`**.

Constraints for whoever owns the proxy:

- **Do not strip a path prefix.** Host-based routing only. Serving under
  `example.com/matcher` requires a Next.js `basePath` set at build time and is
  not supported by this configuration.
- **Do not proxy `/admin`.** Operator routes listen on loopback `:8081` and Caddy
  does not route them. Reaching the API container directly exposes every
  operator route, with no authentication anywhere.
- No WebSockets, no streaming, no sticky sessions. JSON polling only; the API is
  stateless.

### If neither

Leave `SITE_ADDRESS=':80'` and give participants the public IP over HTTP. One
thing degrades: the "copy reproducer" button on a failed submission uses a
clipboard API that browsers disable outside a secure context, so it silently
does nothing. The text remains selectable.

### Verify, either way

```sh
curl -s http://<host>/api/health          # ok
curl -s http://<host>/api/participants    # JSON, not 500
```

> `SITE_ADDRESS` reaches Caddy through an `environment:` block in
> `compose.yaml`. If a real domain is being served as `localhost`, or `:80`
> answers `308` redirecting to HTTPS with a self-signed certificate, that
> variable is not reaching the container — check with
> `docker exec me-platform-caddy-1 printenv SITE_ADDRESS`.

---

## 6. Pool node

```sh
ssh -i ~/.ssh/me-platform ubuntu@<pool-ip>
cd /opt/me-platform
sudo ops/pool-node-setup.sh
```

Worker count is `min(nproc - 2, POOL_BOXES)`, and `POOL_BOXES` defaults to 8 —
so **6 workers on a `c6i.2xlarge`**. Raise it with
`sudo POOL_BOXES=14 ops/pool-node-setup.sh`.

Six slots is ample: a Run job is about two seconds of compile, so 18 people
iterating cannot saturate it. The slots are shared between both lanes and Run
jobs are claimed first, so under sustained Run traffic, Submit verification
queues behind them by design.

---

## 7. Bench node

```sh
ssh -i ~/.ssh/me-platform ubuntu@<bench-ip>
cd /opt/me-platform
sudo BENCH_CPU=2 ISOLATED_CPUS=2-3 ops/bench-node-setup.sh --dedicated
```

`BENCH_CPU` and `ISOLATED_CPUS` must match the cores the instance actually
presents — 4 on a `c6i.2xlarge` with SMT off, so two for the OS and two isolated
for timed runs.

`--dedicated` is the script's **tuning mode** — runtime tuning only, no boot
parameters — and is unrelated to AWS tenancy. Use `--metal` only on bare metal,
which also requires a reboot.

The script runs its hygiene assertions and **refuses to mark the node healthy if
any fail**. Do not override that; a half-tuned node produces numbers that look
fine and rank people wrongly.

On a VM these remain warnings and are expected: `isolcpus`, `nohz_full`,
`rcu_nocbs`, `mitigations=off`, reserved hugepages, and the CPU governor —
there is no cpufreq driver inside an EC2 guest, because the hypervisor owns
frequency scaling.

---

## 8. Connect the workers

On **both** worker nodes, edit `/opt/mebench/worker.env`. The generated file
contains a placeholder host that resolves to nothing:

```
DATABASE_URL=postgres://mebench:<password>@<web private IP>:5432/mebench
S3_BUCKET=<bucket>
AWS_REGION=<region>
```

```sh
sudo systemctl enable --now mebench-pool@{0..5}   # pool node, 0..N-1 from §6
sudo systemctl enable --now mebench-bench         # bench node
```

Confirm they registered. This is the step that proves the security group rule
and `DB_BIND` are both right — if either is wrong the workers simply never
appear:

```sh
docker exec -i me-platform-postgres-1 psql -U mebench -d mebench \
  -c 'SELECT id, role, healthy, now()-last_seen FROM workers ORDER BY role;'
```

Seven rows, all `healthy = t`.

---

## 9. Load the roster

There is no signup. Identity is a name picked from this list.

```sh
docker exec -i me-platform-postgres-1 psql -U mebench -d mebench <<'SQL'
INSERT INTO participants (handle)
VALUES ('a.mehra'), ('b.kulkarni') /* … one row per participant … */
ON CONFLICT (handle) DO NOTHING;
SQL
```

---

## 10. Verify the deployment

**The instance role is attached.** The AWS CLI is deliberately not installed on
the nodes and is not needed — the SDK reads credentials from the metadata
service:

```sh
T=$(curl -s -X PUT http://169.254.169.254/latest/api/token \
      -H "X-aws-ec2-metadata-token-ttl-seconds: 60")
curl -s -H "X-aws-ec2-metadata-token: $T" \
  http://169.254.169.254/latest/meta-data/iam/security-credentials/
```

Prints `me-platform-node` on each instance.

**Postgres and the operator API are not exposed.** Both must refuse:

```sh
nc -zv <web-public-ip> 5432
nc -zv <web-public-ip> 8081
```

**A submission goes through.** Open the site, pick a handle, paste a reference
engine:

- **Run** → `41/41`
- **Submit** → `received → compiling → verifying → verify_passed → bench_queued
  → benchmarking → done`, about 70 seconds

---

## 11. Bench node tenancy

Shared tenancy is enough, and this was measured rather than assumed. 200
independent runs of the reference on a shared `c6i.2xlarge`:

| | |
|---|---|
| single-run spread | 4.46% (max−min) |
| single-run IQR | 1.05% |
| **median-of-9 stability** | **±0.6%** |
| steal-time discards | **0 / 200** |

A score is the median of nine runs, so the IQR predicts stability, not max−min.
Two engines 2% apart are ranked correctly 100% of the time; 1% apart, 98.8%.

Dedicated tenancy adds a **flat $2/hour per region** on top of a ~10%
per-instance premium — about 59× the instance premium — and roughly triples the
bill. It is not justified for the live event.

If you want it for the final rejudge block only, launch a second bench node of
the **same instance type** at the end (~2 hours, ~$5). Three things are then
mandatory:

1. **Stop the old bench worker first.** Nothing prevents two bench workers
   running at once, and the queue claim uses `SKIP LOCKED` — they would take
   different jobs and benchmark concurrently, which is exactly the contamination
   a single bench node exists to prevent.
2. **`DELETE FROM settings WHERE key = 'bench_reference_baseline_ns';`** The
   spot-check baseline is global, not per-node. A fresh node compares against
   the old node's number, exceeds the 5% tolerance, and parks the whole queue.
3. **Same instance type.** The ranked workload is calibrated to this L3.

---

## 12. Operator access

Nine `/admin/*` routes on loopback `:8081`. **No token, no password** — access to
the admin API *is* SSH access to the web node. That is the entire auth story;
unreachability is the mechanism, which is why §5 says never to proxy `/admin`.

```sh
ssh -i ~/.ssh/me-platform -N -L 8081:127.0.0.1:8081 ubuntu@<web-ip>

curl -s  localhost:8081/admin/queue | jq       # workers, queue depth
curl -X POST localhost:8081/admin/freeze       # freeze the standings
curl -X POST localhost:8081/admin/bench/healthy
curl -X POST localhost:8081/admin/bench/unhealthy
```

If SSH keys are not permitted, the same port forward over SSM — note this
requires adding `AmazonSSMManagedInstanceCore` to the `me-platform-node` role:

```sh
aws ssm start-session --target <instance-id> \
  --document-name AWS-StartPortForwardingSession \
  --parameters '{"portNumber":["8081"],"localPortNumber":["8081"]}'
```

---

## 13. Before the event

```sh
ops/make-boilerplate.sh          # publishes dist/me-boilerplate.zip with the spec
```

- Drive one submission through all three nodes (§10).
- Stop the bench worker mid-queue and confirm jobs park at `pending_benchmark`
  and unpark when it returns.
- **Warm the bench node for 30 minutes under load** on the morning. A cold
  package flatters the first few submissions and nobody else.

The ranked workload needs no calibration. `live_target 3780` produces 732,279
resting orders across 14,794 price levels, and on a 54 MB L3 it separates a
cache-conscious engine from a naive one by **5.98×** — the best of any depth
measured. Do not raise it without also raising `BENCH_EVENTS`: warm-up is
`n_sessions × live_target × 8` and the harness clips it to half the stream, so
the next step up would time a book at 69% of its target.

Event-day operations are in [ops/runbook.md](ops/runbook.md).

---

## 14. Teardown

```sh
terraform -chdir=infra destroy             # stops the meter
terraform -chdir=infra/bootstrap destroy   # bucket and IAM
```

`pg_dump` to S3 first if the results matter. Note `force_destroy_bucket`
defaults to `true`, which is right for a test deployment and wrong for the real
event — the bucket holds every submission's source.

---

## Checklist

- [ ] Bucket, `me-platform-node` role and instance profile exist
- [ ] SSH key halves verified to match **before** launch
- [ ] Bench instance launched with `threads_per_core = 1`
- [ ] `DB_BIND` is the web node's **private** IP
- [ ] `POSTGRES_PASSWORD` exported before the first `up`, and on every later one
- [ ] `SITE_ADDRESS` confirmed inside the Caddy container
- [ ] `me-web` allows 5432 from the `me-worker` **security group**
- [ ] Seven workers registered and `healthy = t`
- [ ] Roster loaded
- [ ] `ops/bench-hygiene.sh` exits 0 with `BENCH_CPU` passed explicitly
- [ ] Postgres and `:8081` refused from the public IP
- [ ] One submission driven end to end across all three nodes
