#!/usr/bin/env bash
# pool-node-setup.sh — provision a correctness pool node (c6i.4xlarge or similar).
#
# This node compiles and verifies. It is the only node that runs a compiler, and
# nothing measured happens here, so shared tenancy is fine and its only real
# requirement is that its TOOLCHAIN IS IDENTICAL to the benchmark node's.
# Binaries move between the two: a binary verified here is the binary measured
# there, and if the compilers differ, verification has proved nothing about what
# was benchmarked.
#
# Run as root.

set -euo pipefail

PREFIX="${PREFIX:-/opt/mebench}"
REPO="${REPO:-/opt/me-platform}"
POOL_BOXES="${POOL_BOXES:-8}"

if [[ $EUID -ne 0 ]]; then
  echo "run as root" >&2
  exit 1
fi

echo "==> toolchain"
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq || echo "  (apt-get update reported errors; continuing)"
apt-get install -y --no-install-recommends \
  build-essential g++ cmake git pkg-config zip \
  libcap-dev libseccomp-dev libsystemd-dev

# The pinned compiler. Record it: the benchmark node asserts the same string,
# and a silent divergence here is the kind of thing nobody notices until the
# ranked numbers stop making sense.
CXX_VERSION="$(g++ --version | head -1)"
echo "    $CXX_VERSION"
mkdir -p "$PREFIX"
echo "$CXX_VERSION" > "$PREFIX/TOOLCHAIN"

echo "==> isolate"
"$(dirname "${BASH_SOURCE[0]}")/install-isolate.sh"

echo "==> building the engine at the contest preset"
cmake -S "$REPO/engine" -B "$REPO/engine/build/contest" \
      -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$REPO/engine/build/contest" -j "$(nproc)" >/dev/null

echo "==> installing to $PREFIX"
mkdir -p "$PREFIX"/{bin,lib,include,tests}
install -m755 "$REPO/engine/build/contest/harness" "$PREFIX/bin/harness"
install -m755 "$REPO/engine/build/contest/gen"     "$PREFIX/bin/gen"
install -m755 "$REPO/engine/build/contest/libreference_engine.so" "$PREFIX/lib/"
cp -r "$REPO/engine/include/mebench" "$PREFIX/include/"
cp "$REPO/engine/tests/spec_tests.h" "$REPO/engine/tests/spec_tests.cpp" \
   "$REPO/engine/tests/run_tests_main.cpp" "$PREFIX/tests/"

echo "==> building the worker"
cargo build --release --manifest-path "$REPO/platform/Cargo.toml" --bin worker
install -m755 "$REPO/platform/target/release/worker" "$PREFIX/bin/worker"

echo "==> systemd units"
# Compile concurrency is capped at ncores-2: compiles are bursty and CPU-heavy,
# and leaving headroom is what keeps the node responsive when eighteen people
# press Submit at once.
CONCURRENCY=$(( $(nproc) - 2 ))
[[ $CONCURRENCY -lt 1 ]] && CONCURRENCY=1
[[ $CONCURRENCY -gt $POOL_BOXES ]] && CONCURRENCY=$POOL_BOXES

for i in $(seq 0 $((CONCURRENCY - 1))); do
  cat > "/etc/systemd/system/mebench-pool@$i.service" <<EOF
[Unit]
Description=mebench correctness pool worker (box $i)
After=network-online.target isolate.service
Wants=isolate.service

[Service]
Type=simple
Environment=BOX_ID=$i
Environment=MEBENCH_INCLUDE=$PREFIX/include
Environment=MEBENCH_TESTS=$PREFIX/tests
Environment=MEBENCH_HARNESS=$PREFIX/bin/harness
Environment=MEBENCH_GEN=$PREFIX/bin/gen
Environment=MEBENCH_REFERENCE_SO=$PREFIX/lib/libreference_engine.so
Environment=MEBENCH_CXX=/usr/bin/g++
Environment=MEBENCH_MARCH=x86-64-v3
EnvironmentFile=$PREFIX/worker.env
ExecStart=$PREFIX/bin/worker --role pool
Restart=always
RestartSec=2

[Install]
WantedBy=multi-user.target
EOF
done

if [[ ! -f "$PREFIX/worker.env" ]]; then
  cat > "$PREFIX/worker.env" <<'EOF'
# Fill these in before starting the workers.
#
# `web-node` is a placeholder and resolves to NOTHING. Use the web node's
# PRIVATE VPC address (172.31.x.x), never a public one, and make sure that
# node brought Postgres up with DB_BIND set to the same address — the compose
# default publishes it on loopback, where this cannot reach it.
DATABASE_URL=postgres://mebench:CHANGEME@web-node:5432/mebench
S3_BUCKET=me-platform-artifacts
AWS_REGION=eu-west-1
EOF
  chmod 600 "$PREFIX/worker.env"
  echo "    wrote $PREFIX/worker.env — FILL IT IN, then:"
  echo "      systemctl enable --now mebench-pool@{0..$((CONCURRENCY - 1))}"
else
  systemctl daemon-reload
  echo "    systemctl enable --now mebench-pool@{0..$((CONCURRENCY - 1))}"
fi

echo
echo "pool node ready: $CONCURRENCY boxes, toolchain $CXX_VERSION"
