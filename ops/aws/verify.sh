#!/usr/bin/env bash
# verify.sh — the acceptance checks from DEPLOYMENT.md 10, plus the ones that
# only a script will bother to do.
#
#   ops/aws/verify.sh
#
# Read-only. Exits non-zero if anything fails, so it can gate the event morning.

source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

need_cmd jq
load_outputs
require_public_ips

# ------------------------------------------------------------ instance role

step "instance role attached to every node"
for role in web pool bench; do
  # The AWS CLI is deliberately not installed on the nodes; the SDK reads
  # credentials straight from IMDS, so that is what to ask.
  got="$(node "$role" '
    T=$(curl -sS -X PUT --max-time 5 http://169.254.169.254/latest/api/token \
          -H "X-aws-ec2-metadata-token-ttl-seconds: 60") || exit 1
    curl -sS --max-time 5 -H "X-aws-ec2-metadata-token: $T" \
      http://169.254.169.254/latest/meta-data/iam/security-credentials/' 2>/dev/null || true)"
  if [[ -n "$got" ]]; then
    ok "$role: $got"
  else
    check_fail "$role: no instance role visible via IMDS — S3 reads and writes will fail"
  fi
done

# --------------------------------------------------------------- exposure

step "nothing sensitive is reachable from the public internet"
# Postgres holds every submission; the admin API has no authentication at all,
# so unreachability IS its auth story.
for port in 5432:Postgres 8081:"operator API"; do
  p="${port%%:*}"; what="${port#*:}"
  if timeout 6 bash -c "</dev/tcp/$WEB_IP/$p" 2>/dev/null; then
    check_fail "$what is OPEN on $WEB_IP:$p — this must refuse"
  else
    ok "$what refused on :$p"
  fi
done

# The worker nodes keep a public IP for egress, but accept SSH only from the web
# security group — so from out here every port must be dead, port 22 included.
# Probing the PUBLIC address is the whole point: the private one is unroutable
# from a laptop and would pass this check without testing anything.
for pair in "pool:$POOL_PUBLIC_IP" "bench:$BENCH_PUBLIC_IP"; do
  role="${pair%%:*}"; ip="${pair#*:}"
  [[ -z "$ip" ]] && continue
  for port in 22 80; do
    if timeout 6 bash -c "</dev/tcp/$ip/$port" 2>/dev/null; then
      check_fail "$role is REACHABLE on $ip:$port from the internet — it must not be"
    fi
  done
  ok "$role sealed from the internet ($ip: 22 and 80 both dead)"
done

# And prove the jump still works, or the seal has locked the operator out too.
if node pool true 2>/dev/null; then
  ok "operator access to workers via jump through web works"
else
  check_fail "cannot reach the pool node through the web jump host — operator access is broken"
fi

# ------------------------------------------------------------------ the app

step "the site answers"
if health="$(curl -fsS --max-time 10 "http://$WEB_IP/api/health" 2>/dev/null)"; then
  ok "GET /api/health → ${health:0:60}"
else
  check_fail "GET http://$WEB_IP/api/health failed — Caddy or the API is not up"
fi

if curl -fsS --max-time 10 "http://$WEB_IP/api/participants" >/dev/null 2>&1; then
  ok "GET /api/participants → 200 (not a 500, so the DB and S3 wiring hold)"
else
  check_fail "GET /api/participants failed — usually the API cannot reach Postgres or S3"
fi

# SITE_ADDRESS reaching the container is the difference between a real domain
# being served correctly and it being served as localhost.
sa="$(node web 'docker exec flashmatch-caddy-1 printenv SITE_ADDRESS' 2>/dev/null || true)"
if [[ -n "$sa" ]]; then
  ok "Caddy sees SITE_ADDRESS=$sa"
else
  check_fail "SITE_ADDRESS is not set inside the Caddy container — a real domain would be served as localhost"
fi

# ----------------------------------------------------------------- workers

step "workers"
if rows="$(web_psql "-tAc \"SELECT role, count(*) FILTER (WHERE healthy), count(*) FROM workers GROUP BY role ORDER BY role;\"" 2>/dev/null)"; then
  total=0
  while IFS='|' read -r wrole healthy all; do
    [[ -z "$wrole" ]] && continue
    total=$(( total + all ))
    if [[ "$healthy" == "$all" ]]; then
      ok "$wrole: $healthy/$all healthy"
    else
      check_fail "$wrole: only $healthy of $all healthy"
    fi
  done <<<"$rows"
  (( total == 0 )) && check_fail "no workers registered at all — check DB_BIND and the 5432 security group rule"

  # Exactly one bench worker, always. Two would claim different jobs with
  # SKIP LOCKED and benchmark concurrently, which is the contamination a single
  # bench node exists to prevent.
  nbench="$(web_psql "-tAc \"SELECT count(*) FROM workers WHERE role='bench';\"" 2>/dev/null | tr -d '[:space:]')"
  if [[ "$nbench" =~ ^[0-9]+$ ]] && (( nbench > 1 )); then
    check_fail "$nbench bench workers registered — there must be exactly one"
  fi
else
  check_fail "could not query the workers table"
fi

# --------------------------------------------------------------- toolchain
# A binary verified on the pool is the binary measured on the bench. If the
# compilers differ, verification proved nothing about what was benchmarked.

step "toolchain identical across pool and bench"
pool_tc="$(node pool 'cat /opt/mebench/TOOLCHAIN' 2>/dev/null || true)"
bench_tc="$(node bench 'cat /opt/mebench/TOOLCHAIN' 2>/dev/null || true)"
if [[ -z "$pool_tc" || -z "$bench_tc" ]]; then
  check_fail "TOOLCHAIN missing on pool and/or bench — did the setup scripts finish?"
elif [[ "$pool_tc" != "$bench_tc" ]]; then
  check_fail "toolchain MISMATCH
      pool:  $pool_tc
      bench: $bench_tc"
else
  ok "$pool_tc"
fi

# ------------------------------------------------------------ bench hygiene

step "bench node hygiene"
if out="$(node bench 'cd /opt/flashmatch && sudo BENCH_CPU=$(( $(nproc) / 2 )) ops/bench-hygiene.sh' 2>&1)"; then
  ok "bench-hygiene.sh exits 0"
else
  check_fail "bench-hygiene.sh FAILED — do not rank anything on this node:"
  printf '%s\n' "$out" | sed 's/^/        /' >&2
fi

# swap is the load-bearing one: mlockall always fails inside isolate, so what
# actually keeps a page fault out of the timed region is that swap is off.
if node bench 'test -z "$(swapon --show)"' 2>/dev/null; then
  ok "swap is off on the bench node"
else
  check_fail "SWAP IS ON on the bench node — a swap-in inside a timed region is not a measurement"
fi

# ------------------------------------------------------------------ verdict

echo >&2
if (( FAILURES )); then
  printf '%s%d check(s) failed.%s\n\n' "$C_RED$C_BOLD" "$FAILURES" "$C_RESET" >&2
  exit 1
fi
printf '%sall checks passed%s\n\n' "$C_GREEN$C_BOLD" "$C_RESET" >&2
