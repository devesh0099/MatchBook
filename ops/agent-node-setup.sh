#!/usr/bin/env bash
# agent-node-setup.sh — provision one participant's box (PLAN-measurement-redesign §7, M4).
#
# Every box measures, so every box gets the discipline the old bench node had:
# timers masked, swap off, kernel core isolation, IRQs steered, turbo pinned.
# On a c6i.2xlarge with SMT off (4 cores): core 0 runs the OS and the agent,
# core 1 is the isolated measurement core, cores 2-3 take compiles — reachable
# only by explicit affinity, which is exactly how the worker steers them.
#
#   sudo SEED1=... SEED2=... ops/agent-node-setup.sh
#
# The participant binding does NOT live here — deploy.sh writes it into
# worker.env per box, so one provisioning script serves every box and the AMI
# baked from one box serves the whole fleet.
#
# The script ends by running the hygiene assertions and REFUSES to mark the
# node healthy if any fail (unless a reboot for the kernel parameters is still
# pending, which deploy.sh performs and re-checks).

set -euo pipefail

PREFIX="${PREFIX:-/opt/mebench}"
REPO="${REPO:-/opt/flashmatch}"
ISOLATED_CPUS="${ISOLATED_CPUS:-1-3}"
export BENCH_CPU="${BENCH_CPU:-1}"
COMPILE_CPUS="${COMPILE_CPUS:-2,3}"
# The fixed measurement seeds (§4). Dev defaults; the event's real seeds are
# chosen before kickoff and passed in. SEED3 never reaches an agent box.
SEED1="${SEED1:-101}"
SEED2="${SEED2:-202}"
# The bake list must mirror settings.phase1 and settings.level_table —
# "events:live_target" pairs, phase 1 first. Baking at provision time is what
# makes the first Submit as fast as the tenth; a missing entry only costs a
# lazy bake on first use, never a wrong result.
BAKE_LIST="${BAKE_LIST:-500000:1000 1000000:1000 2000000:1000 4000000:1000}"

[[ $EUID -eq 0 ]] || { echo "run as root" >&2; exit 1; }

# --------------------------------------------------------------------------
# Housekeeping CPUs, derived exactly as bench-node-setup.sh does (see the
# reasoning there): --all because isolcpus hides CPUs from plain nproc.
NCPU="$(nproc --all)"
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
_mask=0
for _c in "${_housekeeping[@]}"; do _mask=$(( _mask | (1 << _c) )); done
HOUSEKEEPING_MASK="$(printf '%x' "$_mask")"

[[ -n "$HOUSEKEEPING_CPUS" ]] || { echo "ISOLATED_CPUS=$ISOLATED_CPUS leaves no CPU for the OS" >&2; exit 1; }
[[ -n "${_isolated[$BENCH_CPU]:-}" ]] || {
  echo "BENCH_CPU=$BENCH_CPU is not inside ISOLATED_CPUS=$ISOLATED_CPUS" >&2; exit 1; }
(( BENCH_CPU < NCPU )) || { echo "BENCH_CPU=$BENCH_CPU does not exist on a $NCPU-CPU box" >&2; exit 1; }

echo "==> $NCPU CPUs: $HOUSEKEEPING_CPUS for the OS+agent, $ISOLATED_CPUS isolated, timing on $BENCH_CPU, compiles on $COMPILE_CPUS"

echo "==> removing everything that could wake up mid-measurement"
# Same list and same mask-then-disable dance as bench-node-setup.sh; see the
# comments there for why each line is the way it is.
for svc in amazon-cloudwatch-agent amazon-ssm-agent unattended-upgrades \
           snapd snapd.socket snapd.seeded.service \
           snap.amazon-ssm-agent.amazon-ssm-agent.service \
           cron apt-daily.timer apt-daily-upgrade.timer motd-news.timer \
           fstrim.timer man-db.timer logrotate.timer e2scrub_all.timer \
           systemd-tmpfiles-clean.timer dpkg-db-backup.timer \
           sysstat-collect.timer sysstat-summary.timer \
           fwupd-refresh.timer update-notifier-download.timer \
           update-notifier-motd.timer; do
  if systemctl mask --now "$svc" 2>/dev/null; then
    echo "    masked $svc"
  elif systemctl disable --now "$svc" 2>/dev/null; then
    echo "    disabled $svc (mask refused: a real unit file occupies the mask path)"
  fi
done
systemctl mask apt-daily.service apt-daily-upgrade.service 2>/dev/null || true
remaining=$(systemctl list-timers --no-pager --no-legend 2>/dev/null | awk '{print $NF}' | grep -v '^$' || true)
for t in $remaining; do
  unit="${t%.service}.timer"
  if systemctl mask --now "$unit" 2>/dev/null; then
    echo "    masked $unit (not in the explicit list — consider adding it)"
  elif systemctl disable --now "$unit" 2>/dev/null; then
    echo "    disabled $unit (mask refused; not in the explicit list)"
  fi
done
systemctl reset-failed 2>/dev/null || true
systemctl daemon-reload 2>/dev/null || true

echo "==> swap off"
swapoff -a || true
sed -i '/\sswap\s/s/^/#/' /etc/fstab || true

echo "==> toolchain"
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq || echo "  (apt-get update reported errors; continuing)"
apt-get install -y --no-install-recommends \
  build-essential g++ cmake git pkg-config linux-tools-common linux-tools-generic \
  numactl util-linux curl ca-certificates \
  libcap-dev libseccomp-dev libsystemd-dev libssl-dev
if ! command -v cargo >/dev/null 2>&1; then
  echo "==> rust toolchain (rustup)"
  curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y --profile minimal
fi
export PATH="$HOME/.cargo/bin:$PATH"
mkdir -p "$PREFIX"
g++ --version | head -1 | tee "$PREFIX/TOOLCHAIN"

echo "==> isolate"
"$(dirname "${BASH_SOURCE[0]}")/install-isolate.sh"

# Kernel parameters via a grub.d DROP-IN — see bench-node-setup.sh for why the
# main file silently loses. The guest set only: isolation is a guest-kernel
# scheduler feature; frequency and C-states belong to the hypervisor, and
# mitigations stay ON because this box runs participant-submitted C++.
echo "==> boot parameters"
install -d /etc/default/grub.d
cat > /etc/default/grub.d/99-mebench.cfg <<EOF
# Written by ops/agent-node-setup.sh. Appends to whatever the cloud image set.
GRUB_CMDLINE_LINUX_DEFAULT="\$GRUB_CMDLINE_LINUX_DEFAULT isolcpus=$ISOLATED_CPUS nohz_full=$ISOLATED_CPUS rcu_nocbs=$ISOLATED_CPUS nmi_watchdog=0"
EOF
update-grub
if ! grep -q "isolcpus=$ISOLATED_CPUS" /proc/cmdline; then
  touch /var/lib/mebench-reboot-required
  echo "    boot parameters written — REBOOT REQUIRED before they take effect"
else
  rm -f /var/lib/mebench-reboot-required
  echo "    already active on the running kernel"
fi

echo "==> runtime tuning"
cat > /usr/local/sbin/mebench-tune <<EOF
#!/usr/bin/env bash
# Re-applied on every boot: none of this survives a reboot by itself.
set -u
if command -v cpupower >/dev/null; then
  cpupower frequency-set -g performance >/dev/null 2>&1 || true
fi
[[ -w /sys/devices/system/cpu/intel_pstate/no_turbo ]] && echo 1 > /sys/devices/system/cpu/intel_pstate/no_turbo
echo $HOUSEKEEPING_MASK > /proc/irq/default_smp_affinity 2>/dev/null || true
for irq in /proc/irq/[0-9]*; do
  echo $HOUSEKEEPING_CPUS > "\$irq/smp_affinity_list" 2>/dev/null || true
done
echo 0 > /proc/sys/kernel/randomize_va_space 2>/dev/null || true
echo 1 > /proc/sys/fs/protected_hardlinks 2>/dev/null || true
EOF
chmod +x /usr/local/sbin/mebench-tune
cat > /etc/systemd/system/mebench-tune.service <<'EOF'
[Unit]
Description=mebench box tuning
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
cmake --build "$REPO/engine/build/contest" -j "$(nproc --all)" >/dev/null
mkdir -p "$PREFIX"/{bin,lib,include,tests,bake}
install -m755 "$REPO/engine/build/contest/harness" "$PREFIX/bin/harness"
install -m755 "$REPO/engine/build/contest/gen"     "$PREFIX/bin/gen"
install -m755 "$REPO/engine/build/contest/libreference_engine.so" "$PREFIX/lib/"
cp -r "$REPO/engine/include/mebench" "$PREFIX/include/"
cp "$REPO/engine/tests/spec_tests.h" "$REPO/engine/tests/spec_tests.cpp" \
   "$REPO/engine/tests/run_tests_main.cpp" "$PREFIX/tests/"
cargo build --release --manifest-path "$REPO/platform/Cargo.toml" --bin worker
install -m755 "$REPO/platform/target/release/worker" "$PREFIX/bin/worker"

echo "==> baking streams and solutions (fixed seeds: everything judges by lookup)"
# File naming must match the worker's ensure_baked() exactly:
#   {profile}-{seed}-{events}-{live_target}.bin/.sol
bake_one() {
  local seed="$1" events="$2" lt="$3"
  local base="cancel_heavy-$seed-$events-$lt"
  if [[ ! -f "$PREFIX/bake/$base.bin" ]]; then
    "$PREFIX/bin/gen" --seed "$seed" --profile cancel_heavy --events "$events" \
      --live-target "$lt" -o "$PREFIX/bake/$base.bin" >/dev/null
    chmod 600 "$PREFIX/bake/$base.bin"
  fi
  [[ -f "$PREFIX/bake/$base.sol" ]] || \
    "$PREFIX/bin/harness" solve --stream "$PREFIX/bake/$base.bin" -o "$PREFIX/bake/$base.sol"
}
read -ra _first <<< "$BAKE_LIST"
# Phase 1 (SEED1) uses the first entry's shape; the ladder (SEED2) uses all.
IFS=':' read -r _ev _lt <<< "${_first[0]}"
bake_one "$SEED1" "$_ev" "$_lt"
for pair in $BAKE_LIST; do
  IFS=':' read -r _ev _lt <<< "$pair"
  bake_one "$SEED2" "$_ev" "$_lt"
done
echo "    $(ls "$PREFIX/bake" | wc -l) baked artifacts in $PREFIX/bake"

echo "==> systemd unit (one agent, one participant)"
# The agent itself lives on the housekeeping core. Measured runs and compiles
# are steered per-invocation by the worker (MEBENCH_BENCH_CPUS /
# MEBENCH_COMPILE_CPUS wrap the sandbox in taskset), so nothing shares the
# isolated core with a timed event — and the agent's own polling never
# touches it at all.
cat > /etc/systemd/system/mebench-agent.service <<EOF
[Unit]
Description=mebench per-participant agent
After=network-online.target isolate.service mebench-tune.service
Wants=isolate.service

[Service]
Type=simple
Environment=BOX_ID=0
Environment=MEBENCH_INCLUDE=$PREFIX/include
Environment=MEBENCH_TESTS=$PREFIX/tests
Environment=MEBENCH_HARNESS=$PREFIX/bin/harness
Environment=MEBENCH_GEN=$PREFIX/bin/gen
Environment=MEBENCH_REFERENCE_SO=$PREFIX/lib/libreference_engine.so
Environment=MEBENCH_CXX=/usr/bin/g++
Environment=MEBENCH_MARCH=x86-64-v3
Environment=MEBENCH_BAKE=$PREFIX/bake
Environment=MEBENCH_SEED1=$SEED1
Environment=MEBENCH_SEED2=$SEED2
Environment=MEBENCH_BENCH_CPUS=$BENCH_CPU
Environment=MEBENCH_COMPILE_CPUS=$COMPILE_CPUS
EnvironmentFile=$PREFIX/worker.env
ExecStart=/usr/bin/taskset -c $HOUSEKEEPING_CPUS $PREFIX/bin/worker --role agent
Restart=always
RestartSec=5

[Install]
WantedBy=multi-user.target
EOF

if [[ ! -f "$PREFIX/worker.env" ]]; then
  cat > "$PREFIX/worker.env" <<'EOF'
# Written by ops/aws/deploy.sh per box. `web-node` resolves to NOTHING on
# purpose: an unconfigured agent fails loudly rather than connecting somewhere
# unexpected. MEBENCH_PARTICIPANT_ID is the participant -> box binding.
DATABASE_URL=postgres://mebench:CHANGEME@web-node:5432/mebench
S3_BUCKET=flashmatch-artifacts
AWS_REGION=ap-south-1
MEBENCH_PARTICIPANT_ID=
EOF
  chmod 600 "$PREFIX/worker.env"
fi
systemctl daemon-reload

echo
echo "==> hygiene assertions"
if [[ -f /var/lib/mebench-reboot-required ]]; then
  echo "    deferred: kernel isolation needs the reboot deploy.sh performs; hygiene runs after it."
elif "$(dirname "${BASH_SOURCE[0]}")/bench-hygiene.sh"; then
  echo
  echo "agent box ready. Start it with:  systemctl enable --now mebench-agent"
else
  echo
  echo "REFUSING to mark this box healthy: hygiene assertions failed above." >&2
  exit 1
fi
