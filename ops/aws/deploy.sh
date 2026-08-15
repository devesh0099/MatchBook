#!/usr/bin/env bash
# deploy.sh — launch and provision the whole platform, end to end.
#
#   ops/aws/deploy.sh                          # everything
#   ops/aws/deploy.sh --site-address matcher.example.com
#   ops/aws/deploy.sh --from build             # resume after a failed node
#   ops/aws/deploy.sh --only bench             # re-provision one node
#   ops/aws/deploy.sh --bench-seed 424242      # fix the ranked seed
#
# Phases, in order:
#
#   infra    terraform apply — the three instances and their security groups
#   wait     cloud-init: Docker, the build toolchain, rustup, the repo
#   build    web stack, pool node and bench node — IN PARALLEL, each ~5-10 min
#   connect  worker.env on both workers, then start the systemd units
#   verify   the acceptance checks from DEPLOYMENT.md 10
#
# Everything is idempotent: re-running a phase on a healthy deployment converges
# rather than duplicating. The one thing that is NOT idempotent is
# POSTGRES_PASSWORD, which Postgres reads only when the cluster is first
# initialised — so it is generated once into ops/aws/.state/secrets.env and
# reused forever after.

source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

SITE_ADDRESS="${SITE_ADDRESS:-:80}"
FROM_PHASE="infra"
ONLY_PHASE=""
ASSUME_YES=0
SKIP_PREFLIGHT=0
POOL_BOXES="${POOL_BOXES:-8}"
# Fixed seed for ranked runs. Empty means a fresh random seed per submission,
# which is the original behaviour and keeps the hidden stream ungameable — at
# the cost that two submissions are measured on different workloads. One
# identical engine spread 11% on p50 across eleven random seeds, so the live
# leaderboard compares submissions only loosely. A rejudge always overrides it.
BENCH_SEED="${BENCH_SEED:-}"

PHASES=(infra wait build connect verify)

usage() { sed -n '2,30p' "${BASH_SOURCE[0]}" | sed 's|^# \?||'; exit "${1:-0}"; }

while (( $# )); do
  case "$1" in
    --site-address) SITE_ADDRESS="$2"; shift 2 ;;
    --from)         FROM_PHASE="$2"; shift 2 ;;
    --only)         ONLY_PHASE="$2"; shift 2 ;;
    --pool-boxes)   POOL_BOXES="$2"; shift 2 ;;
    --bench-seed)   BENCH_SEED="$2"; shift 2 ;;
    --skip-preflight) SKIP_PREFLIGHT=1; shift ;;
    --yes|-y)       ASSUME_YES=1; shift ;;
    -h|--help)      usage 0 ;;
    *) die "unknown argument: $1 (try --help)" ;;
  esac
done
export ASSUME_YES

# `--only bench` means bench and nothing else; `--from build` means build onward.
should_run() {
  local phase="$1"
  if [[ -n "$ONLY_PHASE" ]]; then
    [[ "$phase" == "$ONLY_PHASE" ]] && return 0
    # A single node name is shorthand for "the build and connect phases, for
    # that node only" — the common case after one node fails.
    [[ "$ONLY_PHASE" =~ ^(web|pool|bench)$ && "$phase" =~ ^(build|connect)$ ]] && return 0
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
  [[ -z "$ONLY_PHASE" || ! "$ONLY_PHASE" =~ ^(web|pool|bench)$ ]] && return 0
  [[ "$1" == "$ONLY_PHASE" ]]
}

START_TS=$SECONDS
LOG_DIR="$STATE_DIR/logs"
mkdir -p "$LOG_DIR"

# A stopped instance reports associate_public_ip_address = false, because its
# address really was released. Terraform reads that as drift from the configured
# true, and that attribute forces replacement — so applying against a paused
# deployment silently DESTROYS the workers and rebuilds them from scratch,
# discarding the toolchain, the isolate build and the bench node's kernel
# parameters. The plan does say "must be replaced", but it is easy to skim past
# when you were expecting a no-op.
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
    Applying now would replace them rather than update them, discarding all
    provisioning. Bring them back first:  ops/aws/pause.sh --resume"
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
  confirm "Apply this plan? This starts billing for three EC2 instances (~\$0.78/hr)." \
    || die "aborted; nothing was created"
  tf "$INFRA_DIR" apply -input=false /tmp/infra.tfplan
  rm -f /tmp/infra.tfplan
fi

load_outputs
require_public_ips
load_secrets

log "web   $WEB_IP (private $WEB_PRIVATE_IP)"
log "pool  $POOL_IP"
log "bench $BENCH_IP"

# ============================================================= phase: wait

if should_run wait; then
  step "waiting for cloud-init on all three nodes"
  # They boot concurrently, so these waits overlap in wall-clock terms even
  # though they are checked in sequence.
  for role in web pool bench; do
    wait_ready "$role" 1200
  done
fi

# ============================================================ phase: build
#
# The three nodes are independent here and each takes several minutes — the web
# node compiles the API in release mode and builds the Next.js image, and both
# workers build the engine and the Rust worker. Serially that is ~25 minutes of
# waiting; in parallel it is the slowest one.

provision_web() {
  # A .env file beside compose.yaml rather than exported variables. compose
  # reads it automatically on EVERY invocation, which removes the footgun
  # DEPLOYMENT.md 4 warns about: exporting POSTGRES_PASSWORD for the first `up`
  # and forgetting it on a later one restarts the API against credentials it
  # cannot use, and it crash-loops on "password authentication failed".
  node web "cat > /opt/flashmatch/platform/.env <<'ENVEOF'
DB_BIND=$WEB_PRIVATE_IP
POSTGRES_PASSWORD=$POSTGRES_PASSWORD
AWS_REGION=$AWS_REGION_TF
S3_BUCKET=$S3_BUCKET_TF
SITE_ADDRESS=$SITE_ADDRESS
ENVEOF
chmod 600 /opt/flashmatch/platform/.env"

  node web "cd /opt/flashmatch && docker compose -f platform/compose.yaml up -d --build"

  # compose returns as soon as the containers are created; Postgres still has to
  # initialise and the API has to connect.
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

provision_pool() {
  # `node`, not `node_tty`: this runs as a background job with its output going
  # to a log file, and `ssh -t` there warns about the missing tty and fights the
  # redirection for nothing.
  node pool "cd /opt/flashmatch && sudo POOL_BOXES=$POOL_BOXES ops/pool-node-setup.sh"
}

provision_bench() {
  # BENCH_CPU and ISOLATED_CPUS must match the cores the instance actually
  # presents, and with SMT off that is half of what the instance type advertises.
  # The script's own defaults (BENCH_CPU=4, ISOLATED_CPUS=4-7) assume an 8-CPU
  # box; on the 4-CPU box a c6i.2xlarge presents with threads_per_core=1 they
  # name a CPU that does not exist. Derive them instead of hardcoding.
  local ncpu half
  ncpu="$(node bench nproc --all)"  # --all: isolcpus hides CPUs from plain nproc
  ncpu="${ncpu//[!0-9]/}"
  (( ncpu >= 2 )) || die "bench node reports $ncpu CPUs"
  half=$(( ncpu / 2 ))

  log "bench node presents $ncpu CPUs: 0-$((half - 1)) for the OS, $half-$((ncpu - 1)) isolated"
  node bench "cd /opt/flashmatch && sudo BENCH_CPU=$half ISOLATED_CPUS=$half-$((ncpu - 1)) \
    BENCH_SEED='$BENCH_SEED' ops/bench-node-setup.sh --dedicated"

  # isolcpus, nohz_full and rcu_nocbs only take effect on the next boot, so the
  # reboot is part of provisioning rather than a note for someone to action
  # later. Without it the node measures on cores the scheduler is still using
  # and reports success either way.
  if node bench 'test -f /var/lib/mebench-reboot-required' 2>/dev/null; then
    echo "rebooting bench to activate CPU isolation"
    node bench 'sudo systemctl reboot' 2>/dev/null || true

    # Wait for it to go away and come back; sshd answering immediately would
    # just be the pre-reboot session.
    sleep 20
    local deadline=$(( SECONDS + 300 ))
    while (( SECONDS < deadline )); do
      if node bench 'test -f /var/lib/cloud/flashmatch-ready' 2>/dev/null; then break; fi
      sleep 10
    done
    (( SECONDS < deadline )) || { echo "bench did not come back after reboot" >&2; return 1; }

    # Assert the parameters are actually on the running kernel, rather than
    # trusting that update-grub plus a reboot did what it should.
    local cmdline
    cmdline="$(node bench 'cat /proc/cmdline')"
    for want in isolcpus nohz_full rcu_nocbs; do
      grep -q "$want=" <<<"$cmdline" || {
        echo "after reboot, $want is STILL not on the kernel cmdline:" >&2
        echo "  $cmdline" >&2
        return 1
      }
    done
    echo "CPU isolation active: $(grep -o 'isolcpus=[^ ]*' <<<"$cmdline")"

    node bench 'cd /opt/flashmatch && sudo BENCH_CPU=$(( $(nproc) / 2 )) ops/bench-hygiene.sh' \
      || { echo "bench-hygiene.sh failed after reboot" >&2; return 1; }
  fi
}

if should_run build; then
  step "provisioning nodes in parallel (logs in ${LOG_DIR/#$REPO_ROOT\//})"

  declare -A PIDS=()
  for role in web pool bench; do
    node_selected "$role" || continue
    log "starting $role → ${role}.log"
    ( "provision_$role" ) >"$LOG_DIR/$role.log" 2>&1 &
    PIDS[$role]=$!
  done

  # Report each as it lands rather than after all three, so a fast failure is
  # visible immediately instead of behind the slowest node.
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
#
# Both workers need the same three values, and the placeholder host the setup
# scripts write ("web-node") resolves to nothing on purpose — so that a worker
# that was never configured fails loudly rather than connecting somewhere
# unexpected.

if should_run connect; then
  step "connecting the workers to Postgres"

  write_worker_env() {
    local role="$1"
    node "$role" "sudo tee /opt/mebench/worker.env >/dev/null <<'ENVEOF'
DATABASE_URL=postgres://mebench:$POSTGRES_PASSWORD@$WEB_PRIVATE_IP:5432/mebench
S3_BUCKET=$S3_BUCKET_TF
AWS_REGION=$AWS_REGION_TF
ENVEOF
sudo chmod 600 /opt/mebench/worker.env"
    ok "$role: worker.env → $WEB_PRIVATE_IP"
  }

  if node_selected pool; then
    write_worker_env pool
    # Enable exactly the units the setup script decided to create, rather than a
    # hardcoded {0..5}: the count is min(nproc-2, POOL_BOXES) and changing either
    # would silently leave workers unstarted or fail on units that do not exist.
    # `enable --now` starts a STOPPED unit and does nothing to a running one, so
    # re-provisioning installed a freshly compiled worker and left the old
    # process running from the deleted inode — `readlink /proc/PID/exe` showed
    # "(deleted)" while the binary on disk was an hour newer. No worker code
    # change ever took effect through this path. `restart` after enabling is
    # what actually picks up a new binary, and is a no-op on first deploy.
    node pool 'sudo systemctl daemon-reload
      units=$(cd /etc/systemd/system && ls mebench-pool@*.service 2>/dev/null | tr "\n" " ")
      [ -n "$units" ] || { echo "no mebench-pool units found — did pool-node-setup.sh run?" >&2; exit 1; }
      echo "enabling: $units"
      sudo systemctl enable $units
      sudo systemctl restart $units'
    ok "pool workers started (restarted onto the current binary)"
  fi

  if node_selected bench; then
    write_worker_env bench
    node bench 'sudo systemctl daemon-reload \
      && sudo systemctl enable mebench-bench \
      && sudo systemctl restart mebench-bench'
    ok "bench worker started (restarted onto the current binary)"
  fi

  # Assert it, rather than trusting the restart. A worker still running a
  # replaced binary is invisible until its behaviour silently disagrees with the
  # code you think you deployed.
  for r in pool bench; do
    node_selected "$r" || continue
    if node "$r" 'for p in $(pgrep -x worker); do sudo readlink /proc/$p/exe | grep -q "(deleted)" && exit 1; done; exit 0' 2>/dev/null; then
      ok "$r: running the installed binary"
    else
      warn "$r: a worker is still executing a DELETED binary — the restart did not take"
    fi
  done

  # Registration is the proof that the security group rule and DB_BIND are both
  # right. If either is wrong the workers do not error visibly — they simply
  # never appear in this table.
  step "waiting for workers to register"
  deadline=$(( SECONDS + 120 ))
  while (( SECONDS < deadline )); do
    count="$(web_psql "-tAc 'SELECT count(*) FROM workers WHERE healthy;'" 2>/dev/null | tr -d '[:space:]')"
    if [[ "$count" =~ ^[0-9]+$ ]] && (( count > 0 )); then
      ok "$count healthy worker(s) registered"
      break
    fi
    sleep 5
  done
  if [[ ! "${count:-0}" =~ ^[0-9]+$ ]] || (( ${count:-0} == 0 )); then
    warn "no workers registered after 2 minutes"
    warn "check: ops/aws/logs.sh pool worker    (and: ops/aws/status.sh)"
  fi
fi

# =========================================================== phase: verify

if should_run verify; then
  "$AWS_DIR/verify.sh" || warn "verification reported problems — see above"
fi

# ------------------------------------------------------------------- done

ELAPSED=$(( SECONDS - START_TS ))
cat >&2 <<EOF

${C_GREEN}${C_BOLD}deployment complete${C_RESET} in $(human_duration $ELAPSED)

  site       http://$WEB_IP/          (SITE_ADDRESS=$SITE_ADDRESS)
  admin      ops/aws/tunnel.sh        then curl localhost:8081/admin/queue
  status     ops/aws/status.sh
  ssh        ops/aws/ssh.sh web|pool|bench
  teardown   ops/aws/destroy.sh       ${C_DIM}# three instances cost ~\$560/month idle${C_RESET}

Still to do by hand:
  - load the participant roster       (DEPLOYMENT.md 9)
  - warm the bench node 30 min under load on the morning
EOF
