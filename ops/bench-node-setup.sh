#!/usr/bin/env bash
# bench-node-setup.sh — provision the benchmark node.
#
# This is the only node whose numbers mean anything, so it is the only node
# where these details matter. Nothing runs here except the current job.
#
#   ops/bench-node-setup.sh --dedicated    # dedicated instance: runtime tuning only
#   ops/bench-node-setup.sh --metal        # bare metal: also writes boot parameters
#
# WHICH BRANCH TO USE IS AN OPEN DECISION (plan section 16 q2) and is settled by
# the noise floor measurement in ops/noise-floor/, not by argument. Run that
# first; the spread it reports tells you which of these you needed.
#
# The script ends by running the hygiene assertions and REFUSES to mark the node
# healthy if any fail — a node that quietly half-applied its tuning produces
# numbers that look fine and rank people wrongly.

set -euo pipefail

MODE=""
case "${1:-}" in
  --dedicated) MODE=dedicated ;;
  --metal)     MODE=metal ;;
  *) echo "usage: $0 --dedicated|--metal" >&2; exit 2 ;;
esac

PREFIX="${PREFIX:-/opt/mebench}"
REPO="${REPO:-/opt/flashmatch}"
ISOLATED_CPUS="${ISOLATED_CPUS:-4-7}"
# Exported, not just set: this script runs bench-hygiene.sh as a child, and that
# script has its own BENCH_CPU default of 4. Without the export a node
# configured for a smaller core count pins to the right CPU and is then checked
# against CPU 4 — so a correct setup fails its own gate, on a box where CPU 4
# may not exist at all.
export BENCH_CPU="${BENCH_CPU:-4}"

[[ $EUID -eq 0 ]] || { echo "run as root" >&2; exit 1; }

# ---------------------------------------------------------------------------
# Which CPUs are left for the OS. Everything below that steers work AWAY from
# the measured cores needs this, and it MUST be derived rather than written
# down: the defaults above describe an 8-CPU box, but a c6i.2xlarge with
# threads_per_core=1 presents 4, and DEPLOYMENT.md accordingly tells you to pass
# ISOLATED_CPUS=2-3. A hardcoded housekeeping list of 0-3 then covers every CPU
# on the box, so IRQs land on the isolated cores and the tuning that the ranked
# numbers depend on quietly does nothing.
NCPU="$(nproc)"
declare -A _isolated=()
IFS=',' read -ra _ranges <<< "$ISOLATED_CPUS"
for _r in "${_ranges[@]}"; do
  if [[ "$_r" == *-* ]]; then
    for ((_c = ${_r%-*}; _c <= ${_r#*-}; _c++)); do _isolated[$_c]=1; done
  else
    _isolated[$_r]=1
  fi
done

_housekeeping=()
for ((_c = 0; _c < NCPU; _c++)); do
  [[ -z "${_isolated[$_c]:-}" ]] && _housekeeping+=("$_c")
done
HOUSEKEEPING_CPUS="$(IFS=,; echo "${_housekeeping[*]}")"

# Two spellings, because the two files disagree. /proc/irq/N/smp_affinity_list
# takes a CPU list ("0,1"); /proc/irq/default_smp_affinity takes a HEX BITMASK
# and silently rejects a list — which is why the previous `echo 0-3 >
# default_smp_affinity` never did anything, on any core count.
_mask=0
for _c in "${_housekeeping[@]}"; do _mask=$(( _mask | (1 << _c) )); done
HOUSEKEEPING_MASK="$(printf '%x' "$_mask")"

if [[ -z "$HOUSEKEEPING_CPUS" ]]; then
  echo "ISOLATED_CPUS=$ISOLATED_CPUS leaves no CPU for the OS on a $NCPU-CPU box" >&2
  exit 1
fi
# Pinning the worker to a CPU that was never isolated is the same failure with
# no symptom: the run is measured on a core the scheduler is still using.
if [[ -z "${_isolated[$BENCH_CPU]:-}" ]]; then
  echo "BENCH_CPU=$BENCH_CPU is not inside ISOLATED_CPUS=$ISOLATED_CPUS." >&2
  echo "The timed runs would share a core with everything else on the box." >&2
  exit 1
fi
if (( BENCH_CPU >= NCPU )); then
  echo "BENCH_CPU=$BENCH_CPU does not exist on a $NCPU-CPU box" >&2
  exit 1
fi

echo "==> $NCPU CPUs: $HOUSEKEEPING_CPUS for the OS, $ISOLATED_CPUS isolated, timing on $BENCH_CPU"

echo "==> removing everything that could wake up mid-measurement"
# A default Ubuntu AMI has several timers that WILL fire during a run. Pull
# metrics between runs; never let an agent push continuously.
# amazon-ssm-agent appears TWICE on purpose. Ubuntu's AWS AMI ships it as a
# SNAP, so the plain name is not a unit at all and `systemctl is-active
# amazon-ssm-agent` cheerfully answers "inactive" while the agent runs happily
# under snap.amazon-ssm-agent.amazon-ssm-agent.service. Checking the wrong name
# is worse than not checking: it reports ok for the exact thing it exists to
# catch.
for svc in amazon-cloudwatch-agent amazon-ssm-agent unattended-upgrades snapd \
           snap.amazon-ssm-agent.amazon-ssm-agent.service \
           cron apt-daily.timer apt-daily-upgrade.timer motd-news.timer \
           fstrim.timer man-db.timer logrotate.timer e2scrub_all.timer \
           systemd-tmpfiles-clean.timer dpkg-db-backup.timer \
           sysstat-collect.timer sysstat-summary.timer \
           fwupd-refresh.timer update-notifier-download.timer \
           update-notifier-motd.timer; do
  systemctl disable --now "$svc" 2>/dev/null && echo "    disabled $svc" || true
done
systemctl mask apt-daily.service apt-daily-upgrade.service 2>/dev/null || true

# Belt and braces. The list above is what a 24.04 EC2 AMI shipped on the day
# this was written, and the hygiene check fails on ANY armed timer — so a timer
# added by a future image, or pulled in as a dependency of something we
# install, would fail the node with no clue as to which. Disable whatever is
# still armed, and say what was caught, so the list above can be updated rather
# than silently drifting.
#
# Nothing here is load-bearing for a machine whose whole job is to run one
# benchmark at a time: they are log rotation, metrics collection and update
# checks. sysstat-collect is the worst of them — every 10 minutes, straight
# through a timed run.
remaining=$(systemctl list-timers --no-pager --no-legend 2>/dev/null | awk '{print $NF}' | grep -v '^$' || true)
for t in $remaining; do
  unit="${t%.service}.timer"
  systemctl disable --now "$unit" 2>/dev/null && echo "    disabled $unit (not in the explicit list — consider adding it)" || true
done

echo "==> swap off"
# Memory limits do not affect swapped-out data, and a swap-in inside a timed
# region is not a measurement of anyone's engine.
swapoff -a || true
sed -i '/\sswap\s/s/^/#/' /etc/fstab || true

echo "==> toolchain (must match the correctness pool exactly)"
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq || echo "  (apt-get update reported errors; continuing)"
apt-get install -y --no-install-recommends \
  build-essential g++ cmake git pkg-config linux-tools-common linux-tools-generic \
  numactl util-linux curl ca-certificates \
  libcap-dev libseccomp-dev libsystemd-dev libssl-dev

# Rust, for the worker. Not in the apt line above because Ubuntu's packaged
# toolchain is far older than what Cargo.lock resolves against — the API image
# needed 1.96 — so this uses rustup and takes stable. Neither setup script did
# this before, and both then failed at `cargo build` on a fresh AMI.
if ! command -v cargo >/dev/null 2>&1; then
  echo "==> rust toolchain (rustup)"
  curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y --profile minimal
fi
export PATH="$HOME/.cargo/bin:$PATH"
mkdir -p "$PREFIX"
g++ --version | head -1 | tee "$PREFIX/TOOLCHAIN"

echo "==> isolate"
"$(dirname "${BASH_SOURCE[0]}")/install-isolate.sh"

if [[ "$MODE" == "metal" ]]; then
  echo "==> boot parameters (metal)"
  # isolcpus     removes cores from the load balancer — the biggest single win
  # nohz_full    stops the scheduler tick; without it a timer interrupt every
  #              1-4ms lands squarely in p99
  # rcu_nocbs    offloads bursty RCU callbacks
  # max_cstate=1 a core waking from C6 has a cold L1
  # nosmt        no sibling contending for L1 or execution ports
  # mitigations  removes workload-dependent indirect-branch overhead
  PARAMS="isolcpus=$ISOLATED_CPUS nohz_full=$ISOLATED_CPUS rcu_nocbs=$ISOLATED_CPUS"
  PARAMS="$PARAMS intel_pstate=disable processor.max_cstate=1 intel_idle.max_cstate=0"
  PARAMS="$PARAMS mitigations=off nosmt audit=0 nmi_watchdog=0"
  PARAMS="$PARAMS hugepagesz=1G hugepages=8"

  cp /etc/default/grub "/etc/default/grub.bak.$(date +%s)"
  if grep -q '^GRUB_CMDLINE_LINUX_DEFAULT=' /etc/default/grub; then
    sed -i "s|^GRUB_CMDLINE_LINUX_DEFAULT=.*|GRUB_CMDLINE_LINUX_DEFAULT=\"$PARAMS\"|" /etc/default/grub
  else
    echo "GRUB_CMDLINE_LINUX_DEFAULT=\"$PARAMS\"" >> /etc/default/grub
  fi
  update-grub
  echo "    boot parameters written — A REBOOT IS REQUIRED before they take effect."
  echo "    $PARAMS"
else
  echo "==> boot parameters (dedicated / virtualised)"
  # An earlier version of this script said isolcpus and nohz_full "cannot be set
  # here". That was wrong, and it cost the thing the node exists for: they are
  # GUEST KERNEL scheduler features and the hypervisor has no say in them. On a
  # stock Ubuntu AWS kernel CONFIG_NO_HZ_FULL, CONFIG_RCU_NOCB_CPU and
  # CONFIG_CPUSETS are all enabled. What they need is a reboot, and skipping
  # them left CPUs that were "isolated" in name only — with taskset and IRQ
  # affinity doing all the work while the scheduler, RCU callbacks and the
  # timer tick still landed on the measured cores.
  #
  # This is a deliberately SMALLER set than --metal writes, because most of the
  # rest is a no-op or actively harmful in a guest:
  #
  #   intel_pstate=disable, processor.max_cstate, intel_idle.max_cstate
  #       no such drivers inside an EC2 guest; the hypervisor owns frequency
  #       and C-states. Setting them changes nothing.
  #   nosmt
  #       redundant — cpu_options.threads_per_core = 1 already did it at launch.
  #   hugepagesz=1G hugepages=8
  #       reserves 8 GB on a 16 GB node for a harness buffer of about 240 MB,
  #       and mlockall fails inside isolate regardless.
  #   mitigations=off
  #       DELIBERATELY OMITTED. It works in a guest, and it is a security
  #       decision, not a tuning one: this node runs participant-submitted C++,
  #       and disabling speculative-execution mitigations opens side channels
  #       against other people's submissions and the instance credentials on a
  #       box where the source IS the prize. It also changes measured
  #       performance in ways that favour some code over others.
  PARAMS="isolcpus=$ISOLATED_CPUS nohz_full=$ISOLATED_CPUS rcu_nocbs=$ISOLATED_CPUS"
  PARAMS="$PARAMS nmi_watchdog=0"

  cp /etc/default/grub "/etc/default/grub.bak.$(date +%s)"
  if grep -q '^GRUB_CMDLINE_LINUX_DEFAULT=' /etc/default/grub; then
    sed -i "s|^GRUB_CMDLINE_LINUX_DEFAULT=.*|GRUB_CMDLINE_LINUX_DEFAULT=\"$PARAMS\"|" /etc/default/grub
  else
    echo "GRUB_CMDLINE_LINUX_DEFAULT=\"$PARAMS\"" >> /etc/default/grub
  fi
  update-grub

  # A marker rather than a printed instruction: ops/aws/deploy.sh reboots the
  # node and re-runs the hygiene assertions afterwards, so the isolation is
  # actually in effect before any submission is measured. A message in a log
  # nobody reads is how this got skipped in the first place.
  if ! grep -q "isolcpus=$ISOLATED_CPUS" /proc/cmdline; then
    touch /var/lib/mebench-reboot-required
    echo "    boot parameters written — REBOOT REQUIRED before they take effect"
    echo "    $PARAMS"
  else
    rm -f /var/lib/mebench-reboot-required
    echo "    already active on the running kernel"
  fi
fi

echo "==> runtime tuning"
cat > /usr/local/sbin/mebench-tune <<EOF
#!/usr/bin/env bash
# Re-applied on every boot: none of this survives a reboot by itself.
set -u

# Lock to base clock. Turbo is thermally opportunistic — 3.5GHz on a cold
# package at 9am, 3.0GHz at hour five — so locking it is what makes numbers
# taken hours apart comparable.
if command -v cpupower >/dev/null; then
  cpupower frequency-set -g performance >/dev/null 2>&1 || true
fi
[[ -w /sys/devices/system/cpu/intel_pstate/no_turbo ]] && echo 1 > /sys/devices/system/cpu/intel_pstate/no_turbo

# Keep interrupts off the measured cores. The list is the housekeeping CPUs
# computed at setup time, NOT a constant — see the derivation in
# bench-node-setup.sh. Some IRQs refuse to be moved (per-CPU timers, and on EC2
# some ENA queues); those failures are expected and ignored.
echo $HOUSEKEEPING_MASK > /proc/irq/default_smp_affinity 2>/dev/null || true
for irq in /proc/irq/[0-9]*; do
  echo $HOUSEKEEPING_CPUS > "\$irq/smp_affinity_list" 2>/dev/null || true
done

# ASLR off removes cache-set-conflict luck, worth about 3%.
echo 0 > /proc/sys/kernel/randomize_va_space 2>/dev/null || true
echo 1 > /proc/sys/fs/protected_hardlinks 2>/dev/null || true
EOF
chmod +x /usr/local/sbin/mebench-tune

cat > /etc/systemd/system/mebench-tune.service <<'EOF'
[Unit]
Description=mebench benchmark node tuning
After=multi-user.target

[Service]
Type=oneshot
ExecStart=/usr/local/sbin/mebench-tune
RemainAfterExit=yes

[Install]
WantedBy=multi-user.target
EOF
systemctl daemon-reload
systemctl enable --now mebench-tune.service
echo "    tuning applied and enabled at boot"

echo "==> building and installing"
cmake -S "$REPO/engine" -B "$REPO/engine/build/contest" -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$REPO/engine/build/contest" -j "$(nproc)" >/dev/null
mkdir -p "$PREFIX"/{bin,lib,include,tests}
install -m755 "$REPO/engine/build/contest/harness" "$PREFIX/bin/harness"
install -m755 "$REPO/engine/build/contest/gen"     "$PREFIX/bin/gen"
install -m755 "$REPO/engine/build/contest/libreference_engine.so" "$PREFIX/lib/"
cp -r "$REPO/engine/include/mebench" "$PREFIX/include/"
cargo build --release --manifest-path "$REPO/platform/Cargo.toml" --bin worker
install -m755 "$REPO/platform/target/release/worker" "$PREFIX/bin/worker"

echo "==> systemd unit (exactly one, never scaled)"
# Two submissions on different physical cores still share L3, memory bandwidth
# and the memory controller. An order book is memory-latency-bound, so a
# neighbour thrashing L3 reintroduces exactly the noisy-neighbour problem this
# node was bought to avoid. Serialisation IS the product.
cat > /etc/systemd/system/mebench-bench.service <<EOF
[Unit]
Description=mebench benchmark worker (never run more than one)
After=network-online.target isolate.service mebench-tune.service
Wants=isolate.service

[Service]
Type=simple
Environment=BOX_ID=0
Environment=MEBENCH_INCLUDE=$PREFIX/include
Environment=MEBENCH_HARNESS=$PREFIX/bin/harness
Environment=MEBENCH_GEN=$PREFIX/bin/gen
Environment=MEBENCH_REFERENCE_SO=$PREFIX/lib/libreference_engine.so
Environment=MEBENCH_CXX=/usr/bin/g++
Environment=MEBENCH_MARCH=x86-64-v3
EnvironmentFile=$PREFIX/worker.env
ExecStart=/usr/bin/numactl --cpunodebind=0 --membind=0 \\
          /usr/bin/setarch -R /usr/bin/chrt -f 99 /usr/bin/taskset -c $BENCH_CPU \\
          $PREFIX/bin/worker --role bench
Restart=always
RestartSec=5

[Install]
WantedBy=multi-user.target
EOF

if [[ ! -f "$PREFIX/worker.env" ]]; then
  cat > "$PREFIX/worker.env" <<'EOF'
# `web-node` is a placeholder and resolves to NOTHING. Use the web node's
# PRIVATE VPC address, and check it brought Postgres up with DB_BIND set.
DATABASE_URL=postgres://mebench:CHANGEME@web-node:5432/mebench
S3_BUCKET=flashmatch-artifacts
AWS_REGION=eu-west-1
EOF
  chmod 600 "$PREFIX/worker.env"
fi
systemctl daemon-reload

echo
echo "==> hygiene assertions"
# The node does not get to call itself ready if it is not.
if "$(dirname "${BASH_SOURCE[0]}")/bench-hygiene.sh"; then
  echo
  echo "benchmark node ready. Start it with:"
  echo "  systemctl enable --now mebench-bench"
else
  echo
  echo "REFUSING to mark this node healthy: hygiene assertions failed above." >&2
  [[ "$MODE" == "metal" ]] && echo "If you have not rebooted since writing boot parameters, do that first." >&2
  exit 1
fi
