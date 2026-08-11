# Deployment

Commands to get the platform running on AWS. Reasoning, sizing rationale and
calibration are elsewhere; this is the sequence.

Three EC2 instances in one VPC. Only the web node is reachable by participants.

| Node | Type | Public IP |
|---|---|---|
| web | `m6i.large` | depends on option A or B below |
| pool | `c6i.2xlarge` | no |
| bench | `c6i.2xlarge`, dedicated tenancy, SMT off | no |

Ubuntu 24.04 LTS, x86_64. Clone the repo to `/opt/me-platform` on all three.

---

## 1. Before launching

**Dedicated tenancy and SMT-off are launch-time only.** They cannot be changed
on a running instance.

```sh
aws ec2 run-instances --instance-type c6i.2xlarge \
  --placement Tenancy=dedicated \
  --cpu-options CoreCount=4,ThreadsPerCore=1 \
  --iam-instance-profile Name=me-platform-node \
  ...
```

### S3 bucket and instance role

Nothing in the platform creates these. Without them every `/run` and `/submit`
returns `internal error`.

```sh
aws s3 mb s3://me-platform-artifacts --region <region>
```

Role `me-platform-node`, trusted by `ec2.amazonaws.com`, attached to **all
three** instances:

```json
{
  "Version": "2012-10-17",
  "Statement": [{
    "Effect": "Allow",
    "Action": ["s3:GetObject", "s3:PutObject", "s3:ListBucket"],
    "Resource": [
      "arn:aws:s3:::me-platform-artifacts",
      "arn:aws:s3:::me-platform-artifacts/*"
    ]
  }]
}
```

Verify on each instance once it is up:

```sh
aws sts get-caller-identity          # shows the ROLE, not a user
aws s3 ls s3://me-platform-artifacts
```

### Security groups

| Group | Port | Source |
|---|---|---|
| `me-web` | 5432 | **`me-worker`** (the group, not a CIDR) |
| `me-web` | 22 | your IP |
| `me-worker` | 22 | your IP |

Ports 80/443 on `me-web` depend on the hosting option — see §3.

---

## 2. Web node

```sh
cd /opt/me-platform

export DB_BIND=$(ec2-metadata --local-ipv4 | cut -d' ' -f2)   # e.g. 172.31.8.21
export POSTGRES_PASSWORD='<something long>'
export AWS_REGION='<region>'
export S3_BUCKET='me-platform-artifacts'
export SITE_ADDRESS='...'          # value depends on §3 — set it before running
```

- `DB_BIND` must be the **private** IP. It defaults to loopback, and the workers
  then cannot reach Postgres.
- `POSTGRES_PASSWORD` is read **only when the cluster is first initialised**.
  Setting it later requires deleting the `pgdata` volume.

```sh
docker compose -f platform/compose.yaml up -d --build
```

Load the roster:

```sh
docker exec -i me-platform-postgres-1 psql -U mebench -d mebench <<'SQL'
INSERT INTO participants (handle)
VALUES ('a.mehra'), ('b.kulkarni') /* … 18 rows … */
ON CONFLICT (handle) DO NOTHING;
SQL
```

---

## 3. Hosting the frontend

Both options put the frontend and the API on **one origin**. Caddy owns the
`/api/*` split; keep it in the path either way, or CORS breaks.

### Option A — subdomain, Caddy terminates TLS

Caddy holds the certificate. One hop.

```
browser ──https──▶ Caddy (web node) ──▶ Next.js  /*
                                    └──▶ axum    /api/*  (prefix stripped)
```

1. **A record:** `matcher.example.com` → the web node's **public** IP.
2. On the web node, before `docker compose up`:
   ```sh
   export SITE_ADDRESS='matcher.example.com'
   ```
3. Security group `me-web`: open **80 and 443**.

Caddy requests a Let's Encrypt certificate on first boot and renews it itself.

> **Port 80 must be reachable from the public internet for certificate
> issuance.** Let's Encrypt validates over HTTP-01 from its own servers. If 80 is
> restricted to the room's IP, issuance fails with a challenge error rather than
> a firewall error. Open 80 to `0.0.0.0/0`, confirm the certificate, then narrow
> the rule.

### Option B — behind an existing proxy or ALB

Their edge holds the certificate. The web node needs **no public IP**.

```
browser ──https──▶ their proxy ──http──▶ Caddy (web node) ──▶ Next.js  /*
                                                          └──▶ axum    /api/*
```

1. On the web node, before `docker compose up`:
   ```sh
   export SITE_ADDRESS=':80'      # plain HTTP, no ACME, no redirect to 443
   ```
2. Security group `me-web`: open **80 from their proxy's security group**.
   Remove 80/443 public rules; remove the public IP.
3. Their proxy: forward the hostname to the web node's **private IP, port 80**,
   **path unchanged**.

   ```nginx
   location / {
       proxy_pass http://172.31.8.21:80;
       proxy_set_header Host              $host;
       proxy_set_header X-Forwarded-Proto $scheme;
   }
   ```

   ALB equivalent: target group → web node, port 80, protocol HTTP.
4. Health check: **`/api/health`**.

Constraints to pass on to whoever owns the proxy:

- **Do not strip a path prefix.** Host-based routing only. Serving under
  `example.com/matcher` requires a Next.js `basePath` set at build time and is
  not supported by this configuration.
- **Do not proxy `/admin`.** Operator routes are on loopback `:8081` and Caddy
  does not route them. Bypassing Caddy to reach the API container directly
  exposes every operator route, with no authentication anywhere.
- No WebSockets, no streaming, no sticky sessions. JSON polling only; the API is
  stateless.

### Verify either option

```sh
curl -s https://matcher.example.com/api/participants     # JSON, not 500
curl -s https://matcher.example.com/api/health           # ok
```

---

## 4. Pool node

```sh
sudo ops/pool-node-setup.sh
```

Fill in `/opt/mebench/worker.env`. `web-node` in the generated file is a
placeholder that resolves to nothing:

```
DATABASE_URL=postgres://mebench:<password>@172.31.8.21:5432/mebench
S3_BUCKET=me-platform-artifacts
AWS_REGION=<region>
```

```sh
systemctl enable --now mebench-pool@{0..7}     # 0..N-1 for however many units it wrote
journalctl -u 'mebench-pool@*' -f
```

Worker count is `min(nproc - 2, POOL_BOXES)` and `POOL_BOXES` defaults to 8.
Raise it with `sudo POOL_BOXES=14 ops/pool-node-setup.sh`.

---

## 5. Bench node

`BENCH_CPU` and `ISOLATED_CPUS` must match the core count the instance actually
presents. With SMT off, `c6i.2xlarge` presents 4.

```sh
sudo BENCH_CPU=2 ISOLATED_CPUS=2-3 ops/bench-node-setup.sh --dedicated
```

The setup script runs the hygiene checks itself and refuses to mark the node
healthy if they fail. Do not override that. To re-run them later, pass the same
`BENCH_CPU` — the script's own default is 4:

```sh
sudo BENCH_CPU=2 ops/bench-hygiene.sh          # must exit 0
```

Same `worker.env` as the pool node, then:

```sh
systemctl enable --now mebench-bench
```

---

## 6. Operator access

Nine `/admin/*` routes on loopback `:8081`. No token, no password — access to
the admin API is SSH access to the web node.

```sh
ssh -N -L 8081:127.0.0.1:8081 web-node

curl -s  localhost:8081/admin/queue | jq       # workers, queue depth
curl -X POST localhost:8081/admin/freeze       # freeze the standings
curl -X POST localhost:8081/admin/bench/healthy
curl -X POST localhost:8081/admin/bench/unhealthy
```

If SSH keys are not permitted, the same port forward over SSM:

```sh
aws ssm start-session --target <instance-id> \
  --document-name AWS-StartPortForwardingSession \
  --parameters '{"portNumber":["8081"],"localPortNumber":["8081"]}'
```

Confirm it is not exposed:

```sh
curl http://<web-private-ip>:8081/admin/queue     # must be REFUSED
```

---

## 7. Before the event

```sh
ops/make-boilerplate.sh          # publish dist/me-boilerplate.zip with the spec
```

- Calibrate the ranked depth against the bench node's real L3. The shipped 750k
  is sized for ~54 MB; on a smaller cache 300k scores better.
- Set band thresholds in the `settings` table relative to the reference p50
  measured on that node.
- Drive one submission through all three nodes.
- Stop the bench worker mid-queue and confirm jobs park at `pending_benchmark`
  and unpark when it returns.

---

## Checklist

- [ ] Bucket created; role attached to all three instances; `aws s3 ls` works from each
- [ ] Bench instance launched with `Tenancy=dedicated` and `ThreadsPerCore=1`
- [ ] `DB_BIND` set to the web node's private IP
- [ ] `POSTGRES_PASSWORD` set before the first `docker compose up`
- [ ] `SITE_ADDRESS` matches the chosen hosting option
- [ ] `me-web` allows 5432 from the `me-worker` security group
- [ ] Roster loaded
- [ ] `ops/bench-hygiene.sh` exits 0
- [ ] `BENCH_CPU` / `ISOLATED_CPUS` match the instance's core count
- [ ] Admin port refused from the private IP, reachable over the tunnel
- [ ] One submission driven end to end across all three nodes
