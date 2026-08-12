#!/usr/bin/env bash
# pause.sh — stop the worker nodes between sessions, and bring them back.
#
#   ops/aws/pause.sh              # stop pool and bench
#   ops/aws/pause.sh --all        # stop the web node too
#   ops/aws/pause.sh --resume     # start whatever is stopped, and re-arm it
#
# Stopping is not destroying. The instances keep their volumes, their
# provisioning, their toolchain, the bench node's kernel parameters and its
# masked units — everything survives, and they are usable again about a minute
# after --resume. That is the difference from destroy.sh, which is for when you
# are finished rather than paused.
#
# Costs while stopped: the EBS volumes, and nothing else. Roughly $3.65/month
# per 40 GB volume. Stopping the two workers takes the deployment from about
# $19.50/day to $2.90/day.
#
# THE PRIVATE IP SURVIVES, WHICH IS WHY THIS IS SAFE. Both workers hold web's
# private address in /opt/mebench/worker.env, and operators reach them on their
# own private addresses through the web jump host. A stop/start reassigns the
# PUBLIC address, which nothing here depends on. Terminating would change the
# private one too, and that would need worker.env rewritten.

source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

RESUME=0
INCLUDE_WEB=0
ASSUME_YES=0

for a in "$@"; do
  case "$a" in
    --resume)  RESUME=1 ;;
    --all)     INCLUDE_WEB=1 ;;
    --yes|-y)  ASSUME_YES=1 ;;
    -h|--help) sed -n '2,25p' "${BASH_SOURCE[0]}" | sed 's|^# \?||'; exit 0 ;;
    *) die "unknown argument: $a" ;;
  esac
done
export ASSUME_YES

need_cmd aws; need_cmd jq
load_outputs
export AWS_REGION="$AWS_REGION_TF" AWS_DEFAULT_REGION="$AWS_REGION_TF"
if profile="$(terraform -chdir="$INFRA_DIR" console <<<'var.aws_profile' 2>/dev/null | tr -d '"')"; then
  [[ -n "$profile" && "$profile" != "null" && "$profile" != "tostring(null)" ]] && export AWS_PROFILE="$profile"
fi

ids_for() {
  local names="$1"
  aws ec2 describe-instances \
    --filters "Name=tag:Project,Values=flashmatch" "Name=tag:Name,Values=$names" \
              "Name=instance-state-name,Values=running,stopped,stopping,pending" \
    --query 'Reservations[].Instances[].InstanceId' --output text 2>/dev/null
}

TARGETS="flashmatch-pool,flashmatch-bench"
(( INCLUDE_WEB )) && TARGETS="flashmatch-web,$TARGETS"

# ------------------------------------------------------------------- resume

if (( RESUME )); then
  step "starting instances"
  ids="$(ids_for "$TARGETS")"
  [[ -n "$ids" ]] || die "no flashmatch instances found — has it been destroyed?"
  # shellcheck disable=SC2086
  aws ec2 start-instances --instance-ids $ids --output text >/dev/null
  # shellcheck disable=SC2086
  aws ec2 wait instance-running --instance-ids $ids
  ok "instances running"

  # Addresses were read BEFORE the start, when a stopped web node had no public
  # IP at all — so every later `node` call would try to ssh to an empty host.
  # A stop/start also assigns a DIFFERENT public address, so even a previously
  # good value is wrong now. Refresh state, then re-read.
  #
  # -refresh-only is the safe form: it updates state to match reality and
  # applies no configuration changes, so it cannot trip the
  # associate_public_ip_address replacement that a normal apply would.
  step "refreshing Terraform state for the new addresses"
  tf "$INFRA_DIR" apply -refresh-only -auto-approve -input=false >/dev/null 2>&1 \
    || warn "refresh failed; addresses below may be stale"
  _TF_JSON=""
  load_outputs
  ok "web is now $WEB_IP"
  warn "the public IP changed — update the DNS A record or the site stays dark"

  # The web node is the jump host, so it has to answer before the others can be
  # reached at all.
  step "waiting for SSH"
  for role in web pool bench; do
    local_deadline=$(( SECONDS + 300 ))
    while (( SECONDS < local_deadline )); do
      node "$role" true 2>/dev/null && { ok "$role reachable"; break; }
      sleep 5
    done
  done

  # A stop/start lands the instance on DIFFERENT PHYSICAL HARDWARE. The stored
  # reference baseline was measured on the old host, so the next spot check
  # compares against a number from a machine that no longer exists, exceeds the
  # 5% tolerance, marks the node unhealthy and parks the whole bench queue —
  # exactly the failure DEPLOYMENT.md 11 describes for a replaced bench node.
  # Deleting it makes the next check re-establish it.
  step "clearing the bench reference baseline"
  if web_psql "-tAc \"DELETE FROM settings WHERE key = 'bench_reference_baseline_ns';\"" >/dev/null 2>&1; then
    ok "baseline cleared — the next spot check re-establishes it on this host"
  else
    warn "could not clear the baseline; do it by hand or the bench queue may park"
  fi

  step "workers"
  deadline=$(( SECONDS + 180 ))
  while (( SECONDS < deadline )); do
    n="$(web_psql "-tAc 'SELECT count(*) FROM workers WHERE healthy;'" 2>/dev/null | tr -d '[:space:]')"
    [[ "$n" =~ ^[0-9]+$ ]] && (( n >= 7 )) && { ok "$n workers healthy"; break; }
    sleep 5
  done
  [[ "${n:-0}" =~ ^[0-9]+$ ]] && (( n < 7 )) && \
    warn "only ${n:-0} workers healthy — units start at boot, give them a moment or check ops/aws/logs.sh pool worker"

  # The kernel parameters live in a grub drop-in and survive a reboot, but
  # assert rather than assume: a bench node measuring on unisolated cores looks
  # completely healthy.
  step "bench node still isolated?"
  if node bench 'grep -q isolcpus /proc/cmdline' 2>/dev/null; then
    ok "isolcpus active: $(node bench 'cat /sys/devices/system/cpu/isolated' 2>/dev/null)"
  else
    bad "isolcpus is NOT on the kernel cmdline — do not rank anything until this is fixed"
  fi

  echo >&2
  printf '%sresumed.%s Confirm with: ops/aws/verify.sh\n\n' "$C_GREEN$C_BOLD" "$C_RESET" >&2
  exit 0
fi

# -------------------------------------------------------------------- pause

step "stopping: ${TARGETS//,/ }"
ids="$(ids_for "$TARGETS")"
[[ -n "$ids" ]] || die "no matching instances found"

if (( INCLUDE_WEB )); then
  warn "this includes the WEB node: the site goes down and the operator API with it"
fi
log "volumes keep billing (~\$3.65/month per 40GB); everything else stops"
confirm "Stop these instances?" || die "aborted; nothing stopped"

# shellcheck disable=SC2086
aws ec2 stop-instances --instance-ids $ids --output text >/dev/null
# shellcheck disable=SC2086
aws ec2 wait instance-stopped --instance-ids $ids
ok "stopped"

cat >&2 <<EOF

  Resume with:  ops/aws/pause.sh --resume

  Note the workers will show as unhealthy in the admin queue while stopped;
  that is expected, not a fault. The public IPs are released on stop and
  reassigned on start, which nothing here depends on — operators reach the
  workers on their PRIVATE addresses through the web node.

  ${C_RED}${C_BOLD}DO NOT RUN terraform apply (or deploy.sh) WHILE STOPPED.${C_RESET}

  A stopped instance reports associate_public_ip_address = false, because the
  address really was released. Terraform reads that as drift from the
  configured true, and that attribute forces replacement — so an apply would
  DESTROY AND RECREATE both workers, discarding the toolchain, the isolate
  build, the bench node's kernel parameters and its masked units. The plan says
  "must be replaced", which is easy to skim past when you expected a no-op.

  Resume first, then apply. ops/aws/deploy.sh refuses to run while any
  instance is stopped, for this reason.
EOF
