#!/usr/bin/env bash
# status.sh — what is running, is it healthy, and what has it cost so far.
#
#   ops/aws/status.sh
#
# Read-only, and safe to run during the event. The cost line exists because the
# real risk with this deployment is not the hourly rate, it is forgetting to
# destroy it: three idle instances are about $560 a month.

source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

need_cmd aws; need_cmd jq

if ! have_state "$INFRA_DIR"; then
  printf '\n%snothing deployed%s — infra/ has no Terraform state.\n\n' "$C_BOLD" "$C_RESET"
  exit 0
fi

load_outputs
export AWS_REGION="$AWS_REGION_TF" AWS_DEFAULT_REGION="$AWS_REGION_TF"
if profile="$(terraform -chdir="$INFRA_DIR" console <<<'var.aws_profile' 2>/dev/null | tr -d '"')"; then
  [[ -n "$profile" && "$profile" != "null" ]] && export AWS_PROFILE="$profile"
fi

# ------------------------------------------------------------------ instances

step "instances ($AWS_REGION_TF)"

instances="$(aws ec2 describe-instances \
  --filters "Name=tag:Project,Values=flashmatch" \
            "Name=instance-state-name,Values=pending,running,stopping,stopped" \
  --output json 2>/dev/null)" || die "could not describe instances — check credentials"

# Approximate on-demand rates for ap-south-1 (Mumbai), which is where this is
# deployed. Deliberately a table rather than a Pricing API call: that API needs
# a permission this deployment does not otherwise grant, and is only reachable
# from us-east-1. Rates in other regions differ by a few percent for m6i and are
# identical for c6i, so treat the total as an estimate either way.
rate_for() {
  case "$1" in
    m6i.large)    echo 0.101 ;;
    c6i.2xlarge)  echo 0.340 ;;
    c6i.4xlarge)  echo 0.680 ;;
    m6i.xlarge)   echo 0.202 ;;
    t3.medium)    echo 0.0448 ;;
    *)            echo 0 ;;
  esac
}

# Charged per running instance regardless of type, and easy to forget because it
# is not part of the instance rate: every public IPv4 address costs $0.005/hr,
# and a 40 GB gp3 root volume is $0.0912/GB-month whether the instance is
# running or stopped. Stopping the nodes does NOT stop the volume charge.
IPV4_HOURLY=0.005
EBS_HOURLY=$(awk -v gb=40 'BEGIN{printf "%.5f", gb*0.0912/730}')

now=$(date +%s)
total_rate=0
total_cost=0
printf '    %-8s %-14s %-10s %-16s %-10s %s\n' NAME TYPE STATE "PUBLIC IP" UPTIME "EST. COST" >&2

while IFS=$'\t' read -r name itype state pubip launch; do
  [[ -z "$name" ]] && continue
  launch_s=$(date -d "$launch" +%s 2>/dev/null || echo "$now")
  up=$(( now - launch_s ))
  rate="$(rate_for "$itype")"
  if [[ "$state" == "running" ]]; then
    # Instance + its public IPv4 + its root volume.
    rate=$(awk -v r="$rate" -v i="$IPV4_HOURLY" -v e="$EBS_HOURLY" 'BEGIN{printf "%.4f", r+i+e}')
    cost=$(awk -v r="$rate" -v u="$up" 'BEGIN{printf "%.2f", r*u/3600}')
    total_rate=$(awk -v a="$total_rate" -v b="$rate" 'BEGIN{printf "%.4f", a+b}')
    total_cost=$(awk -v a="$total_cost" -v b="$cost" 'BEGIN{printf "%.2f", a+b}')
  else
    # Stopped still bills the volume, which is the trap: the instance list looks
    # harmless and the bill does not stop.
    rate="$EBS_HOURLY"
    cost=$(awk -v r="$rate" -v u="$up" 'BEGIN{printf "%.2f", r*u/3600}')
    total_rate=$(awk -v a="$total_rate" -v b="$rate" 'BEGIN{printf "%.4f", a+b}')
    total_cost=$(awk -v a="$total_cost" -v b="$cost" 'BEGIN{printf "%.2f", a+b}')
  fi
  colour="$C_GREEN"; [[ "$state" != "running" ]] && colour="$C_YELLOW"
  printf '    %-8s %-14s %s%-10s%s %-16s %-10s $%s\n' \
    "$name" "$itype" "$colour" "$state" "$C_RESET" "${pubip:--}" "$(human_duration $up)" "$cost" >&2
done < <(jq -r '.Reservations[].Instances[]
  | [ (.Tags[]?|select(.Key=="Name")|.Value), .InstanceType, .State.Name,
      (.PublicIpAddress // "-"), .LaunchTime ] | @tsv' <<<"$instances")

echo >&2
printf '    running rate ~$%s/hr · accrued ~$%s · a month idle would be ~$%s\n' \
  "$total_rate" "$total_cost" \
  "$(awk -v r="$total_rate" 'BEGIN{printf "%.0f", r*730}')" >&2
printf '    %s(estimate: us-east-1 on-demand, excludes EBS, data transfer and dedicated tenancy)%s\n' \
  "$C_DIM" "$C_RESET" >&2

# ------------------------------------------------------------------- platform

if [[ -z "$WEB_IP" ]]; then
  warn "web node has no public IP — skipping platform checks"
  exit 0
fi

step "platform"

if health="$(curl -fsS --max-time 8 "http://$WEB_IP/api/health" 2>/dev/null)"; then
  ok "http://$WEB_IP/api/health → ${health:0:50}"
else
  bad "http://$WEB_IP/api/health unreachable"
fi

if containers="$(node web 'cd /opt/flashmatch && docker compose -f platform/compose.yaml ps --format "{{.Service}}\t{{.State}}"' 2>/dev/null)"; then
  while IFS=$'\t' read -r svc st; do
    [[ -z "$svc" ]] && continue
    if [[ "$st" == running* ]]; then ok "$svc: $st"; else bad "$svc: $st"; fi
  done <<<"$containers"
fi

# --------------------------------------------------------------------- queue

step "workers and queue"

if rows="$(web_psql "-tAc \"SELECT role, id, healthy, round(extract(epoch from now()-last_seen)) FROM workers ORDER BY role, id;\"" 2>/dev/null)"; then
  while IFS='|' read -r wrole wid whealthy wseen; do
    [[ -z "$wrole" ]] && continue
    if [[ "$whealthy" == "t" ]]; then
      ok "$(printf '%-6s %-24s last seen %ss ago' "$wrole" "$wid" "$wseen")"
    else
      bad "$(printf '%-6s %-24s UNHEALTHY, last seen %ss ago' "$wrole" "$wid" "$wseen")"
    fi
  done <<<"$rows"
else
  bad "could not reach Postgres on the web node"
fi

if q="$(admin_curl GET /admin/queue 2>/dev/null)"; then
  printf '%s\n' "$q" | jq . >&2 2>/dev/null || printf '    %s\n' "$q" >&2
else
  warn "operator API on :8081 did not answer (it is loopback-only; this queries it from the web node)"
fi

# ---------------------------------------------------------------------- disk
# A full root volume on the web node stops Postgres accepting writes, and it is
# the kind of thing that is obvious afterwards and invisible before.

step "disk"
for role in web pool bench; do
  ip="$(_ip_for_role "$role")"
  [[ -z "$ip" ]] && continue
  if use="$(node "$role" "df -h / | awk 'NR==2{print \$5\" of \"\$2\" used\"}'" 2>/dev/null)"; then
    pct="${use%%%*}"
    if (( pct >= 85 )); then bad "$role: $use"; else ok "$role: $use"; fi
  fi
done

echo >&2
