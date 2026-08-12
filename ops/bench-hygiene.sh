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
# The snap unit is listed alongside the plain name because Ubuntu's AWS AMI
# ships the SSM agent as a snap: `systemctl is-active amazon-ssm-agent` returns
# "inactive" for a unit that does not exist, so this check passed while the
# agent was running through every measurement.
for svc in amazon-cloudwatch-agent amazon-ssm-agent unattended-upgrades cron crond \
           snap.amazon-ssm-agent.amazon-ssm-agent.service \
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
# Count only timers with a NEXT elapse time. A masked timer stays loaded in a
# failed state and `list-timers` keeps printing a row for it, with "-" in every
# time column — counting that row reported an armed timer that can never fire,
# and no amount of masking would clear it. The first field is NEXT.
n_timers=$(systemctl list-timers --no-pager --no-legend 2>/dev/null \
  | awk 'NF && $1 != "-"' | grep -vc '^$' || true)
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

# Frequency control is not the guest's to hold on a virtual machine. There is
# no cpufreq driver inside an EC2 instance — the hypervisor owns the P-states —
# so scaling_governor does not exist and this can never be satisfied, no matter
# how the node is provisioned. Failing on it condemns every VM permanently,
# which is not a verdict about the node.
#
# The turbo check immediately above already degrades to a warning when its file
# is absent; this one hard-failed on the identical situation. On metal, where
# the governor genuinely is ours to set, it stays fatal.
virt=$(systemd-detect-virt 2>/dev/null || echo none)
gov=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo unknown)
if [[ "$gov" == "performance" ]]; then
  pass "governor=performance"
elif [[ "$virt" != "none" ]]; then
  warn "governor=$gov — no cpufreq inside a $virt guest; the hypervisor owns the clock"
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
  pass "swap off (the only thing keeping ranked pages resident — see below)"
else
  # This is not tidiness, it is the guarantee. isolate sets RLIMIT_MEMLOCK to 0
  # inside the box and offers no way to raise it, so the harness's mlockall
  # ALWAYS fails in a ranked run. With swap off, anonymous pages cannot be
  # reclaimed at all and the point is moot; with swap on, a ~700MB working set
  # is evictable and a swap-in inside a timed region is a five-figure-nanosecond
  # event that no memory limit prevents.
  fail "swap is on (${swap} kB) — turn it off; mlockall cannot cover for it inside isolate"
fi

if [[ -r /sys/kernel/mm/transparent_hugepage/enabled ]]; then
  thp=$(sed -n 's/.*\[\(.*\)\].*/\1/p' /sys/kernel/mm/transparent_hugepage/enabled)
  pass "transparent hugepages: $thp"
fi
hp=$(awk '/^HugePages_Total:/ {print $2}' /proc/meminfo)
[[ "${hp:-0}" -gt 0 ]] && pass "explicit hugepages reserved: $hp" || warn "no explicit hugepages reserved"

echo
echo "== CPU pinning reaches inside the sandbox =="
# isolate has no cpuset option: every CPU guarantee this platform makes is
# applied OUTSIDE it and inherited across fork/exec. That inheritance is an
# assumption, so it is asserted rather than trusted — anything landing between
# the unit and the worker that resets affinity would leave the limits looking
# correct while ranked runs quietly spread across cores.
BENCH_CPU="${BENCH_CPU:-4}"
if command -v isolate >/dev/null 2>&1; then
  isolate --cg --box-id 30 --cleanup >/dev/null 2>&1 || true
  if isolate --cg --box-id 30 --init >/dev/null 2>&1; then
    inside=$(taskset -c "$BENCH_CPU" isolate --cg --box-id 30 --processes=2 \
             --run -- /bin/sh -c 'grep Cpus_allowed_list /proc/self/status' 2>/dev/null \
             | awk '{print $2}')
    if [[ "$inside" == "$BENCH_CPU" ]]; then
      pass "sandboxed process inherits the pinned core ($inside)"
    else
      fail "pinned to CPU $BENCH_CPU outside, box reports '${inside:-none}' — affinity is NOT reaching the submission"
    fi
    isolate --cg --box-id 30 --cleanup >/dev/null 2>&1 || true
  else
    warn "could not init a box to check affinity inheritance"
  fi
else
  warn "isolate not installed; cannot check affinity inheritance"
fi

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
