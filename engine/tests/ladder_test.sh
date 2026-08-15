#!/usr/bin/env bash
# M1 acceptance: solve / gated bench / ladder, driven end to end with real
# binaries. Deadlines here are either enormous (must pass on any machine) or
# physically impossible (1 ms for 200k events needs <5 ns/event against a
# ~10 ns rdtscp floor), so the test is deterministic across hardware.
set -euo pipefail

GEN=$1 HARNESS=$2 REF=$3 MUTANT=$4 DIR=$5
rm -rf "$DIR" && mkdir -p "$DIR"

$GEN --seed 7 --profile balanced --events 100000 -o "$DIR/l1.bin" >/dev/null
$GEN --seed 8 --profile balanced --events 200000 -o "$DIR/l2.bin" >/dev/null
$HARNESS solve --stream "$DIR/l1.bin" -o "$DIR/l1.sol" --checkpoint-every 20000 >/dev/null
$HARNESS solve --stream "$DIR/l2.bin" -o "$DIR/l2.sol" --checkpoint-every 20000 >/dev/null

# -- gated bench: a correct engine under a generous deadline passes, and the
#    baked solution's final digest is what checked it.
out=$($HARNESS bench --stream "$DIR/l1.bin" --runs 1 --warmup 10000 \
      --deadline-ms 30000 --solution "$DIR/l1.sol" --engine "$REF" --json)
grep -q '"outcome":"ok"' <<<"$out"

# -- gate failure: an impossible deadline exits 5 with progress + percentiles.
set +e
out=$($HARNESS bench --stream "$DIR/l2.bin" --runs 1 --warmup 10000 \
      --deadline-ms 1 --solution "$DIR/l2.sol" --engine "$REF" --json)
rc=$?
set -e
[ "$rc" -eq 5 ]
grep -q '"outcome":"deadline_missed"' <<<"$out"
grep -q '"events_processed":' <<<"$out"

# -- wrong engine: the baked solution catches a wrong trade price mid-stream.
set +e
out=$(MEBENCH_MUTANT=trade_price $HARNESS bench --stream "$DIR/l1.bin" --runs 1 --warmup 10000 \
      --solution "$DIR/l1.sol" --engine "$MUTANT" --json)
rc=$?
set -e
[ "$rc" -ne 0 ]
grep -q '"outcome":"digest_mismatch"' <<<"$out"

# -- stuck engine: the SIGALRM backstop fires (exit 3), because the in-loop
#    check never runs when on_new never returns.
set +e
MEBENCH_MUTANT=hang \
timeout 60 $HARNESS bench --stream "$DIR/l1.bin" --runs 1 --warmup 10000 \
      --deadline-ms 2000 --engine "$MUTANT" --json >/dev/null
rc=$?
set -e
[ "$rc" -eq 3 ]

# -- the ladder: passes the generous level, stops at the impossible one, and
#    reports the climb as data.
cat > "$DIR/levels.txt" <<EOF
# id stream solution deadline_ms
1 $DIR/l1.bin $DIR/l1.sol 30000
2 $DIR/l2.bin $DIR/l2.sol 1
EOF
out=$($HARNESS ladder --engine "$REF" --levels "$DIR/levels.txt" --json)
grep -q '"level":1,"outcome":"ok"' <<<"$out"
grep -q '"level":2,"outcome":"deadline_missed"' <<<"$out"
grep -q '"max_level":1' <<<"$out"
grep -q '"climb_complete":0' <<<"$out"

echo "gate/solution/ladder: all checks passed"
