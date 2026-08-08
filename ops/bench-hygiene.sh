#!/usr/bin/env bash
# bench-hygiene.sh — assert the benchmark node is in a state worth measuring on.
#
# Run before the event, and any time a number looks wrong. Exits non-zero if any
# assertion fails, so the setup script can refuse to mark the node healthy and
# the runbook can gate on it.
#
# Every check here corresponds to something that has quietly corrupted a
# measurement somewhere: a monitoring agent waking up mid-run, turbo giving
# 3.5GHz at 9am and 3.0GHz at hour five, a scheduler tick landing in p99.
#
#   ops/bench-hygiene.sh            report and exit non-zero on failure
#   ops/bench-hygiene.sh --warn     report only, always exit 0

set -uo pipefail

WARN_ONLY=0
[[ "${1:-}" == "--warn" ]] && WARN_ONLY=1

FAIL=0
pass() { printf '  \033[32mok  \033[0m %s\n' "$1"; }
fail() { printf '  \033[31mFAIL\033[0m %s\n' "$1"; FAIL=$((FAIL + 1)); }
warn() { printf '  \033[33mwarn\033[0m %s\n' "$1"; }

echo "== processes that must not exist =="
# "Pull metrics between runs; never let an agent push continuously." A default
# Ubuntu AMI has several timers that will fire mid-measurement.
for svc in amazon-cloudwatch-agent amazon-ssm-agent unattended-upgrades cron crond \
           snapd apt-daily.timer apt-daily-upgrade.timer motd-news.timer \
           fstrim.timer man-db.timer logrotate.timer filebeat fluentd; do
  if systemctl is-active --quiet "$svc" 2>/dev/null; then
    fail "$svc is active"
  else
    pass "$svc inactive"
  fi
done

echo
echo "== systemd timers =="
n_timers=$(systemctl list-timers --no-pager --no-legend 2>/dev/null | grep -vc '^$' || true)
if [[ "${n_timers:-0}" -gt 0 ]]; then
  fail "$n_timers systemd timer(s) armed — each one is a scheduled interruption"
  systemctl list-timers --no-pager --no-legend 2>/dev/null | awk '{print "       " $NF}' | head -8
else
  pass "no armed timers"
fi

echo
echo "== CPU =="
if [[ -r /sys/devices/system/cpu/intel_pstate/no_turbo ]]; then
  if [[ "$(cat /sys/devices/system/cpu/intel_pstate/no_turbo)" == "1" ]]; then
    pass "turbo disabled"
  else
    # Disabling turbo feels backwards and is the point: turbo is thermally
    # opportunistic, so it hands out a different clock at 9am than at hour five.
    fail "turbo is ENABLED — everyone must be measured against the same clock"
  fi
else
  warn "intel_pstate/no_turbo not present (not an Intel P-state system?)"
fi

gov=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo unknown)
if [[ "$gov" == "performance" ]]; then
  pass "governor=performance"
else
  fail "governor=$gov (want performance)"
fi

if grep -qE '^flags.*\bht\b' /proc/cpuinfo 2>/dev/null; then
  siblings=$(lscpu | awk -F: '/Thread\(s\) per core/ {gsub(/ /,"",$2); print $2}')
  if [[ "$siblings" == "1" ]]; then
    pass "SMT off (no sibling contending for L1 or execution ports)"
  else
    fail "SMT is ON — a sibling thread shares L1 and execution ports"
  fi
fi

echo
echo "== kernel command line =="
CMDLINE=$(cat /proc/cmdline)
for want in isolcpus nohz_full rcu_nocbs; do
  if grep -q "$want" <<<"$CMDLINE"; then
    pass "$want present"
  else
    # Only meaningful on metal. On a dedicated instance these cannot be set and
    # their absence is expected, not a failure of the operator.
    warn "$want absent (expected unless this is metal — see plan section 9)"
  fi
done
if grep -q "mitigations=off" <<<"$CMDLINE"; then
  pass "mitigations=off"
else
  warn "mitigations=off absent — indirect-branch overhead is workload-dependent"
fi

echo
echo "== memory =="
swap=$(awk '/^SwapTotal:/ {print $2}' /proc/meminfo)
if [[ "${swap:-0}" -eq 0 ]]; then
  pass "swap off"
else
  # Memory limits do not affect swapped-out data, and a swap-in inside a timed
  # region is a five-figure-nanosecond event.
  fail "swap is on (${swap} kB) — turn it off"
fi

if [[ -r /sys/kernel/mm/transparent_hugepage/enabled ]]; then
  thp=$(sed -n 's/.*\[\(.*\)\].*/\1/p' /sys/kernel/mm/transparent_hugepage/enabled)
  pass "transparent hugepages: $thp"
fi
hp=$(awk '/^HugePages_Total:/ {print $2}' /proc/meminfo)
[[ "${hp:-0}" -gt 0 ]] && pass "explicit hugepages reserved: $hp" || warn "no explicit hugepages reserved"

echo
echo "== steal time =="
# On dedicated tenancy this should always read zero, which makes it a free
# tripwire for the co-tenant problem.
steal=$(awk '/^cpu /{print $9}' /proc/stat)
if [[ "${steal:-0}" -eq 0 ]]; then
  pass "cumulative steal time is zero"
else
  warn "cumulative steal time is $steal (non-zero since boot; per-run deltas are what matter)"
fi

echo
if [[ $FAIL -gt 0 ]]; then
  echo "$FAIL assertion(s) failed — this node is not fit to produce ranked numbers."
  [[ $WARN_ONLY -eq 1 ]] && exit 0
  exit 1
fi
echo "hygiene checks passed."
