#!/usr/bin/env bash
# measure.sh — the measurement that decides the hardware.
#
# Run the reference implementation many times on a candidate instance and look
# at the spread of per-run medians. This is settled by measurement, not by
# argument, and it must be run BEFORE building the benchmark node's provisioning
# for real — the boot parameters in bench-node-setup.sh only matter if metal is
# what the numbers call for.
#
#   ops/noise-floor/measure.sh [runs] [out.jsonl]
#
# Twenty minutes for the spread test. Feed the result to analyze.py.

set -euo pipefail

RUNS="${1:-200}"
OUT="${2:-noise-floor.jsonl}"
PREFIX="${PREFIX:-/opt/mebench}"
HARNESS="${MEBENCH_HARNESS:-$PREFIX/bin/harness}"
REFERENCE="${MEBENCH_REFERENCE_SO:-$PREFIX/lib/libreference_engine.so}"
# 20M, matching the ranked configuration, NOT 10M. The harness derives warm-up
# from the profile's depth and then clips it to half the stream: cancel_heavy
# needs ~9.7M warm-up events, so a 10M stream is clipped to 5M and the timed
# region runs against a book at roughly half fill. The spread measured that way
# is still a valid stability signal, but it is not the workload being ranked.
EVENTS="${EVENTS:-20000000}"
PROFILE="${PROFILE:-cancel_heavy}"
SEED="${SEED:-20260808}"

for f in "$HARNESS" "$REFERENCE"; do
  [[ -x "$f" || -f "$f" ]] || { echo "missing $f" >&2; exit 1; }
done

echo "==> hygiene first: a spread measured on an untuned node measures the node,"
echo "    not the hardware."
"$(dirname "${BASH_SOURCE[0]}")/../bench-hygiene.sh" --warn || true

STREAM=$(mktemp /tmp/noise-floor-XXXX.bin)
trap 'rm -f "$STREAM"' EXIT
echo "==> generating the stream once (same bytes for every run)"
"${MEBENCH_GEN:-$PREFIX/bin/gen}" --seed "$SEED" --profile "$PROFILE" --events "$EVENTS" -o "$STREAM" >/dev/null

: > "$OUT"
echo "==> $RUNS runs of the reference, one ranked run each"
for i in $(seq 1 "$RUNS"); do
  ts=$(date +%s)
  # --runs 1: this is measuring RUN-TO-RUN spread, so each sample must be one
  # independent run. Taking a median of nine here would measure the thing that
  # is supposed to be smoothing the spread out.
  json=$("$HARNESS" bench --stream "$STREAM" --engine "$REFERENCE" \
          --runs 1 --warmup 200000 --json 2>/dev/null || echo '{}')
  echo "{\"i\":$i,\"at\":$ts,\"result\":$json}" >> "$OUT"
  p50=$(python3 -c "import json,sys; print(json.loads('''$json''').get('p50_ns','?'))" 2>/dev/null || echo '?')
  printf '\r  run %4d/%d  p50=%s ns   ' "$i" "$RUNS" "$p50"
done
echo
echo
echo "wrote $OUT"
echo "now: ops/noise-floor/analyze.py $OUT"
