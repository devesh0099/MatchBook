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
REPO="${REPO:-/opt/me-platform}"
ISOLATED_CPUS="${ISOLATED_CPUS:-4-7}"
BENCH_CPU="${BENCH_CPU:-4}"

[[ $EUID -eq 0 ]] || { echo "run as root" >&2; exit 1; }

echo "==> removing everything that could wake up mid-measurement"
# A default Ubuntu AMI has several timers that WILL fire during a run. Pull
# metrics between runs; never let an agent push continuously.
for svc in amazon-cloudwatch-agent amazon-ssm-agent unattended-upgrades snapd \
           cron apt-daily.timer apt-daily-upgrade.timer motd-news.timer \
           fstrim.timer man-db.timer logrotate.timer e2scrub_all.timer; do
  systemctl disable --now "$svc" 2>/dev/null && echo "    disabled $svc" || true
done
systemctl mask apt-daily.service apt-daily-upgrade.service 2>/dev/null || true

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
  numactl util-linux libcap-dev libseccomp-dev libsystemd-dev
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

  cp /etc/default/grub /etc/default/grub.bak.$(date +%s)
  if grep -q '^GRUB_CMDLINE_LINUX_DEFAULT=' /etc/default/grub; then
    sed -i "s|^GRUB_CMDLINE_LINUX_DEFAULT=.*|GRUB_CMDLINE_LINUX_DEFAULT=\"$PARAMS\"|" /etc/default/grub
  else
    echo "GRUB_CMDLINE_LINUX_DEFAULT=\"$PARAMS\"" >> /etc/default/grub
  fi
  update-grub
  echo "    boot parameters written — A REBOOT IS REQUIRED before they take effect."
  echo "    $PARAMS"
else
  echo "==> dedicated instance: skipping boot parameters"
  echo "    isolcpus/nohz_full/nosmt cannot be set here. Runtime tuning below is"
  echo "    what you get; the noise floor measurement tells you if it is enough."
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

# Keep interrupts off the measured cores.
echo 0-3 > /proc/irq/default_smp_affinity 2>/dev/null || true
for irq in /proc/irq/[0-9]*; do
  echo 0-3 > "\$irq/smp_affinity_list" 2>/dev/null || true
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
S3_BUCKET=me-platform-artifacts
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
