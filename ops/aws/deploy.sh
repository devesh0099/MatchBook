#!/usr/bin/env bash
# deploy.sh — launch and provision the platform, fleet-shaped
# (PLAN-measurement-redesign M4): one web node plus one box per participant.
#
#   ops/aws/deploy.sh                          # everything
#   ops/aws/deploy.sh --site-address matcher.example.com
#   ops/aws/deploy.sh --from build             # resume after a failed node
#   ops/aws/deploy.sh --only agent2            # re-provision one box
#   ops/aws/deploy.sh --seed1 N --seed2 N      # the event's fixed seeds
#
# Phases, in order:
#
#   infra    terraform apply — web + fleet_size agent boxes
#   wait     cloud-init: Docker, the build toolchain, rustup, the repo
#   build    web stack and every agent box — IN PARALLEL, each ~5-10 min;
#            agents also reboot once for kernel core isolation
#   connect  per-box worker.env with the participant binding, units started
#   verify   health, box registry, per-box reference acceptance number
#
# Everything is idempotent: re-running a phase on a healthy deployment
# converges rather than duplicating. The one thing that is NOT idempotent is
# POSTGRES_PASSWORD, generated once into ops/aws/.state/secrets.env.

source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

SITE_ADDRESS="${SITE_ADDRESS:-:80}"
FROM_PHASE="infra"
ONLY_PHASE=""
ASSUME_YES=0
SKIP_PREFLIGHT=0
# The fixed measurement seeds (PLAN-measurement-redesign §4). Dev defaults;
# choose the event's for the day and never publish them. SEED3 is M6's and
# never reaches an agent box.
SEED1="${SEED1:-101}"
SEED2="${SEED2:-202}"

PHASES=(infra wait build connect verify)

usage() { sed -n '2,25p' "${BASH_SOURCE[0]}" | sed 's|^# \?||'; exit "${1:-0}"; }

while (( $# )); do
  case "$1" in
    --site-address) SITE_ADDRESS="$2"; shift 2 ;;
    --from)         FROM_PHASE="$2"; shift 2 ;;
    --only)         ONLY_PHASE="$2"; shift 2 ;;
    --seed1)        SEED1="$2"; shift 2 ;;
    --seed2)        SEED2="$2"; shift 2 ;;
    --skip-preflight) SKIP_PREFLIGHT=1; shift ;;
    --yes|-y)       ASSUME_YES=1; shift ;;
    -h|--help)      usage 0 ;;
    *) die "unknown argument: $1 (try --help)" ;;
  esac
done
export ASSUME_YES

should_run() {
  local phase="$1"
  if [[ -n "$ONLY_PHASE" ]]; then
    [[ "$phase" == "$ONLY_PHASE" ]] && return 0
    # A single node name is shorthand for "build and connect, for that node".
    [[ "$ONLY_PHASE" =~ ^(web|agent[0-9]+)$ && "$phase" =~ ^(build|connect)$ ]] && return 0
    return 1
  fi
  local seen=0 p
  for p in "${PHASES[@]}"; do
    [[ "$p" == "$FROM_PHASE" ]] && seen=1
    [[ "$p" == "$phase" ]] && return $(( seen ? 0 : 1 ))
  done
  return 1
}

node_selected() {
  [[ -z "$ONLY_PHASE" || ! "$ONLY_PHASE" =~ ^(web|agent[0-9]+)$ ]] && return 0
  [[ "$1" == "$ONLY_PHASE" ]]
}

START_TS=$SECONDS
LOG_DIR="$STATE_DIR/logs"
mkdir -p "$LOG_DIR"

# A stopped instance reports associate_public_ip_address = false; Terraform
# reads that as drift and REPLACES the instance, discarding all provisioning.
assert_nothing_stopped() {
  command -v aws >/dev/null 2>&1 || return 0
  local region stopped
  region="$(terraform -chdir="$INFRA_DIR" output -raw aws_region 2>/dev/null || true)"
  [[ -n "$region" ]] || return 0
  stopped="$(aws ec2 describe-instances --region "$region" \
    --filters "Name=tag:Project,Values=flashmatch" "Name=instance-state-name,Values=stopped,stopping" \
    --query 'Reservations[].Instances[].Tags[?Key==`Name`].Value' --output text 2>/dev/null | tr '\n' ' ')"
  [[ -z "${stopped// /}" ]] && return 0
  die "these instances are STOPPED: ${stopped}
    Applying now would replace them rather than update them. Bring them back
    first:  ops/aws/pause.sh --resume"
}
assert_nothing_stopped

# ============================================================ phase: infra

if should_run infra; then
  if (( ! SKIP_PREFLIGHT )); then
    step "preflight"
    "$AWS_DIR/preflight.sh" || die "preflight failed — nothing was created"
  fi

  step "terraform apply"
  tf "$INFRA_DIR" init -input=false
  tf "$INFRA_DIR" plan -input=false -out=/tmp/infra.tfplan
  echo >&2
  confirm "Apply this plan? This starts billing for the web node and the agent fleet." \
    || die "aborted; nothing was created"
  tf "$INFRA_DIR" apply -input=false /tmp/infra.tfplan
  rm -f /tmp/infra.tfplan
fi

load_outputs
require_public_ips
load_secrets

log "web    $WEB_IP (private $WEB_PRIVATE_IP)"
for (( i = 0; i < AGENT_COUNT; i++ )); do
  log "agent$((i + 1)) ${AGENT_IPS[$i]} (participant $((i + 1)))"
done

AGENT_ROLES=()
for (( i = 1; i <= AGENT_COUNT; i++ )); do AGENT_ROLES+=("agent$i"); done

# ============================================================= phase: wait

if should_run wait; then
  step "waiting for cloud-init on every node"
  for role in web "${AGENT_ROLES[@]}"; do
    wait_ready "$role" 1200
  done
fi

# ============================================================ phase: build

provision_web() {
  node web "cat > /opt/flashmatch/platform/.env <<'ENVEOF'
DB_BIND=$WEB_PRIVATE_IP
POSTGRES_PASSWORD=$POSTGRES_PASSWORD
AWS_REGION=$AWS_REGION_TF
S3_BUCKET=$S3_BUCKET_TF
SITE_ADDRESS=$SITE_ADDRESS
ENVEOF
chmod 600 /opt/flashmatch/platform/.env"

  node web "cd /opt/flashmatch && docker compose -f platform/compose.yaml up -d --build"

  local deadline=$(( SECONDS + 180 ))
  while (( SECONDS < deadline )); do
    if node web "curl -fsS --max-time 5 http://127.0.0.1/api/health" >/dev/null 2>&1; then
      return 0
    fi
    sleep 5
  done
  echo "api/health never answered; container state:" >&2
  node web "cd /opt/flashmatch && docker compose -f platform/compose.yaml ps" >&2 || true
  node web "cd /opt/flashmatch && docker compose -f platform/compose.yaml logs --tail=50 api" >&2 || true
  return 1
}

provision_agent() {
  local role="$1"
  # ISOLATED_CPUS / BENCH_CPU / COMPILE_CPUS derived from what the box actually
  # presents. With SMT off a c6i.2xlarge presents 4: OS on 0, measurement on 1,
  # compiles on the rest.
  local ncpu
  ncpu="$(node "$role" nproc --all)"
  ncpu="${ncpu//[!0-9]/}"
  (( ncpu >= 2 )) || { echo "$role reports $ncpu CPUs" >&2; return 1; }
  local compile_cpus="2,3"
  (( ncpu < 4 )) && compile_cpus="1"

  node "$role" "cd /opt/flashmatch && sudo ISOLATED_CPUS=1-$((ncpu - 1)) BENCH_CPU=1 \
    COMPILE_CPUS=$compile_cpus SEED1=$SEED1 SEED2=$SEED2 ops/agent-node-setup.sh"

  # isolcpus/nohz_full/rcu_nocbs only take effect on the next boot, so the
  # reboot is part of provisioning rather than a note for someone to action.
  if node "$role" 'test -f /var/lib/mebench-reboot-required' 2>/dev/null; then
    echo "rebooting $role to activate CPU isolation"
    node "$role" 'sudo systemctl reboot' 2>/dev/null || true
    sleep 20
    local deadline=$(( SECONDS + 300 ))
    while (( SECONDS < deadline )); do
      if node "$role" 'test -f /var/lib/cloud/flashmatch-ready' 2>/dev/null; then break; fi
      sleep 10
    done
    (( SECONDS < deadline )) || { echo "$role did not come back after reboot" >&2; return 1; }

    local cmdline
    cmdline="$(node "$role" 'cat /proc/cmdline')"
    for want in isolcpus nohz_full rcu_nocbs; do
      grep -q "$want=" <<<"$cmdline" || {
        echo "after reboot, $want is STILL not on the kernel cmdline: $cmdline" >&2
        return 1
      }
    done
    echo "CPU isolation active: $(grep -o 'isolcpus=[^ ]*' <<<"$cmdline")"

    node "$role" 'cd /opt/flashmatch && sudo BENCH_CPU=1 ops/bench-hygiene.sh' \
      || { echo "bench-hygiene.sh failed on $role after reboot" >&2; return 1; }
  fi
}

if should_run build; then
  step "provisioning nodes in parallel (logs in ${LOG_DIR/#$REPO_ROOT\//})"

  declare -A PIDS=()
  for role in web "${AGENT_ROLES[@]}"; do
    node_selected "$role" || continue
    log "starting $role → ${role}.log"
    if [[ "$role" == web ]]; then
      ( provision_web ) >"$LOG_DIR/$role.log" 2>&1 &
    else
      ( provision_agent "$role" ) >"$LOG_DIR/$role.log" 2>&1 &
    fi
    PIDS[$role]=$!
  done

  FAILED=()
  for role in "${!PIDS[@]}"; do
    if wait "${PIDS[$role]}"; then
      ok "$role provisioned"
    else
      bad "$role FAILED — last 30 lines of $LOG_DIR/$role.log:"
      tail -30 "$LOG_DIR/$role.log" >&2
      FAILED+=("$role")
    fi
  done

  if (( ${#FAILED[@]} )); then
    die "provisioning failed on: ${FAILED[*]}
    Fix the cause, then re-run just that node:  ops/aws/deploy.sh --only ${FAILED[0]}"
  fi
fi

# ========================================================== phase: connect

if should_run connect; then
  step "binding each box to its participant and starting the agents"

  for (( i = 1; i <= AGENT_COUNT; i++ )); do
    role="agent$i"
    node_selected "$role" || continue
    # The participant binding lives HERE, in worker.env, and nowhere else on
    # the box: one provisioning script (and one AMI) serves the whole fleet.
    node "$role" "sudo tee /opt/mebench/worker.env >/dev/null <<'ENVEOF'
DATABASE_URL=postgres://mebench:$POSTGRES_PASSWORD@$WEB_PRIVATE_IP:5432/mebench
S3_BUCKET=$S3_BUCKET_TF
AWS_REGION=$AWS_REGION_TF
MEBENCH_PARTICIPANT_ID=$i
ENVEOF
sudo chmod 600 /opt/mebench/worker.env"
    # `restart`, not just `enable --now`: a re-provision installs a fresh
    # binary and a running unit would keep executing the deleted inode.
    node "$role" 'sudo systemctl daemon-reload \
      && sudo systemctl enable mebench-agent \
      && sudo systemctl restart mebench-agent'
    ok "$role bound to participant $i and started"
  done

  step "waiting for boxes to register"
  deadline=$(( SECONDS + 120 ))
  while (( SECONDS < deadline )); do
    count="$(web_psql "-tAc 'SELECT count(*) FROM boxes WHERE healthy;'" 2>/dev/null | tr -d '[:space:]')"
    if [[ "$count" =~ ^[0-9]+$ ]] && (( count >= AGENT_COUNT )); then
      ok "$count healthy box(es) in the registry"
      break
    fi
    sleep 5
  done
  if [[ ! "${count:-0}" =~ ^[0-9]+$ ]] || (( ${count:-0} < AGENT_COUNT )); then
    warn "only ${count:-0}/$AGENT_COUNT boxes registered after 2 minutes"
    warn "check: ops/aws/logs.sh agent1 agent    (and: ops/aws/status.sh)"
  fi
fi

# =========================================================== phase: verify

if should_run verify; then
  step "verify: health, registry, and the box acceptance number"

  node web "curl -fsS --max-time 5 http://127.0.0.1/api/health" >/dev/null \
    && ok "api healthy" || check_fail "api /health failed"

  web_psql "-c 'SELECT id, role, participant_id, healthy FROM boxes ORDER BY id;'" || true

  # The box acceptance test (M4): every box runs the reference on one fixed
  # workload; a box far from the fleet's band gets REPLACED, not compensated
  # for. Two boxes give a spread, twenty give a band — either way the numbers
  # land here and in the log for the record.
  step "box acceptance: reference wall on each box (fixed seed, 1M events)"
  for (( i = 1; i <= AGENT_COUNT; i++ )); do
    p50="$(node "agent$i" \
      'taskset -c 1 /opt/mebench/bin/harness bench --seed 9 --profile cancel_heavy \
       --events 1000000 --live-target 1000 --runs 3 --engine /opt/mebench/lib/libreference_engine.so \
       --json 2>/dev/null' | jq -r '.p50_ns' 2>/dev/null || echo '?')"
    log "agent$i reference p50: ${p50} ns"
  done
  (( FAILURES == 0 )) && ok "verification passed" || warn "$FAILURES verification failure(s)"
fi

# ------------------------------------------------------------------- done

ELAPSED=$(( SECONDS - START_TS ))
cat >&2 <<EOF

${C_GREEN}${C_BOLD}deployment complete${C_RESET} in $(human_duration $ELAPSED)

  site       http://$WEB_IP/          (SITE_ADDRESS=$SITE_ADDRESS)
  admin      ops/aws/tunnel.sh        then curl localhost:8081/admin/queue
  ssh        ops/aws/ssh.sh web|agent1..agent$AGENT_COUNT
  teardown   ops/aws/destroy.sh

Still to do by hand:
  - issue credentials: ops/aws/tunnel.sh, then
      ops/issue-credentials.sh roster.txt | tee slips.txt
    (participant N is served by agent-N: roster order IS the box binding)
EOF
