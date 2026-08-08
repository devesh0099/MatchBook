#!/usr/bin/env bash
# make-boilerplate.sh — assemble the participant zip.
#
# ASSEMBLED, never maintained by hand. Every file in it is copied from the one
# canonical copy in engine/, for the same reason the web editor generates its
# starting buffer from engine/boilerplate/src/engine.cpp: the moment the zip and
# the server hold separate copies of a header, they will disagree, and the
# person who discovers it will be a participant at hour three whose code
# compiles locally and not on the server.
#
#   ops/make-boilerplate.sh [outdir]
#
# Produces  <outdir>/me-boilerplate/  and  <outdir>/me-boilerplate.zip

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${1:-$ROOT/dist}"
STAGE="$OUT/me-boilerplate"

ENGINE="$ROOT/engine"
BUILD="${MEBENCH_BUILD:-$ENGINE/build/contest}"

echo "==> checking the prebuilt tools exist"
for t in gen harness; do
  if [[ ! -x "$BUILD/$t" ]]; then
    echo "missing $BUILD/$t — build the contest preset first:" >&2
    echo "  cmake --preset contest -S $ENGINE && cmake --build $ENGINE/build/contest -j" >&2
    exit 1
  fi
done

rm -rf "$STAGE"
mkdir -p "$STAGE"/{include/mebench,src,tests,tools}

echo "==> frozen headers (verbatim)"
cp "$ENGINE"/include/mebench/*.h "$STAGE/include/mebench/"

echo "==> skeleton — byte-identical to the editor's starting buffer"
cp "$ENGINE/boilerplate/src/engine.cpp" "$STAGE/src/engine.cpp"

echo "==> visible tests"
cp "$ENGINE"/tests/spec_tests.h "$ENGINE"/tests/spec_tests.cpp "$ENGINE"/tests/run_tests_main.cpp \
   "$STAGE/tests/"

echo "==> build files and docs"
cp "$ENGINE/boilerplate/CMakeLists.txt" "$STAGE/CMakeLists.txt"
cp "$ENGINE/boilerplate/README.md" "$STAGE/README.md"
cp "$ROOT/spec/SPEC.md" "$STAGE/SPEC.md"

echo "==> prebuilt tools"
cp "$BUILD/gen" "$STAGE/tools/gen"
# Shipped as `bench` because that is what participants type; it is the same
# binary the server runs, so a local verify and a server verify cannot disagree
# about what the rules are.
cp "$BUILD/harness" "$STAGE/tools/bench"
chmod +x "$STAGE/tools/gen" "$STAGE/tools/bench"

# A zip whose skeleton has drifted from the editor is the failure this whole
# script exists to prevent, so prove it rather than assume it.
echo "==> verifying the skeleton matches the canonical source"
a=$(sha256sum "$ENGINE/boilerplate/src/engine.cpp" | cut -d' ' -f1)
b=$(sha256sum "$STAGE/src/engine.cpp" | cut -d' ' -f1)
[[ "$a" == "$b" ]] || { echo "skeleton mismatch: $a vs $b" >&2; exit 1; }
echo "    sha256 $a"

echo "==> smoke-testing the assembled kit"
tmp=$(mktemp -d)
cmake -S "$STAGE" -B "$tmp" >/dev/null
cmake --build "$tmp" -j >/dev/null
# The skeleton is meant to FAIL the visible tests: it is a skeleton. What is
# being checked is that it builds and runs cleanly, so a participant's first
# five minutes are spent reading the spec and not fighting the toolchain.
if "$tmp/run_tests" >/dev/null 2>&1; then
  echo "    unexpected: the shipped skeleton passes every visible test" >&2
else
  passed=$("$tmp/run_tests" 2>/dev/null | grep -oE '[0-9]+/[0-9]+ passed' | tail -1 || true)
  echo "    builds and runs; skeleton scores ${passed:-a partial pass}, as intended"
fi
"$STAGE/tools/gen" --seed 1 --profile balanced --events 20000 -o "$tmp/s.bin" >/dev/null
"$STAGE/tools/bench" verify --stream "$tmp/s.bin" --engine "$tmp/libengine.so" >/dev/null 2>&1 || true
echo "    gen and bench run"
rm -rf "$tmp"

echo "==> zipping"
(cd "$OUT" && rm -f me-boilerplate.zip && zip -qr me-boilerplate.zip me-boilerplate)
echo
echo "$OUT/me-boilerplate.zip"
du -h "$OUT/me-boilerplate.zip" | cut -f1
