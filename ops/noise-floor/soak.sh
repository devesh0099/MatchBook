#!/usr/bin/env bash
# soak.sh — the drift half of the hardware decision.
#
# The spread test says how much the machine moves between adjacent runs. It says
# nothing about whether the machine is the SAME machine six hours later, which
# is the property the event actually depends on: submissions are measured hours
# apart and compared against each other.
#
# Runs unattended. Start it, leave it, analyse in the morning.
#
#   ops/noise-floor/soak.sh [hours] [out.jsonl]

set -euo pipefail

HOURS="${1:-6}"
OUT="${2:-soak.jsonl}"
PREFIX="${PREFIX:-/opt/mebench}"
HARNESS="${MEBENCH_HARNESS:-$PREFIX/bin/harness}"
REFERENCE="${MEBENCH_REFERENCE_SO:-$PREFIX/lib/libreference_engine.so}"
EVENTS="${EVENTS:-10000000}"
SEED="${SEED:-20260808}"
# Load between measurements, so this measures a machine under the thermal
# conditions of a real event rather than an idle one.
LOAD="${LOAD:-1}"

STREAM=$(mktemp /tmp/soak-XXXX.bin)
trap 'rm -f "$STREAM"; [[ -n "${load_pid:-}" ]] && kill $load_pid 2>/dev/null' EXIT
"${MEBENCH_GEN:-$PREFIX/bin/gen}" --seed "$SEED" --profile cancel_heavy --events "$EVENTS" -o "$STREAM" >/dev/null

if [[ "$LOAD" == "1" ]]; then
  # Keep the package warm on the NON-isolated cores. Thermal steady state is
  # the condition the event runs in; measuring a cold package flatters it.
  ( while :; do :; done ) & load_pid=$!
  taskset -cp 0-3 $load_pid >/dev/null 2>&1 || true
fi

: > "$OUT"
END=$(( $(date +%s) + HOURS * 3600 ))
i=0
echo "soaking for ${HOURS}h; writing $OUT"
while [[ $(date +%s) -lt $END ]]; do
  i=$((i + 1))
  json=$("$HARNESS" bench --stream "$STREAM" --engine "$REFERENCE" \
          --runs 1 --warmup 200000 --json 2>/dev/null || echo '{}')
  echo "{\"i\":$i,\"at\":$(date +%s),\"result\":$json}" >> "$OUT"
  sleep 60
done
echo "done: $i samples over ${HOURS}h"
echo "now: ops/noise-floor/analyze.py <spread.jsonl> $OUT"
