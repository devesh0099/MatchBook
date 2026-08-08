# Matching Engine Challenge — local kit

Optional. The **web editor is the only submission path** — there is no CLI
submit, deliberately, so there is exactly one thing to support at hour four.

This kit exists so you can iterate in your own editor and measure locally
without touching the shared queue. When you want a result that counts, paste the
buffer back into the web editor and press Submit.

## Layout

```
include/mebench/   the frozen headers — read-only, identical to the server's
src/engine.cpp     your engine; identical to the editor's starting buffer
tests/             the ~30 visible tests the Run button executes
tools/gen          the stream generator
tools/bench        the local harness: verify and bench modes
SPEC.md            the specification. It is normative.
```

## Build and run

```sh
cmake -S . -B build
cmake --build build -j

./build/run_tests                  # the visible tests
```

Verify against the reference on a stream of your choosing:

```sh
./tools/gen --seed 1 --profile balanced --events 200000 -o stream.bin
./tools/bench verify --stream stream.bin --engine ./build/libengine.so
```

Measure:

```sh
./tools/gen --seed 1 --profile cancel_heavy --events 10000000 -o bench.bin
./tools/bench bench --stream bench.bin --engine ./build/libengine.so --runs 5
```

## Reading local numbers honestly

Local numbers **will not match server numbers**, and are not meant to. Your
laptop has frequency scaling, turbo, other processes, and a different
microarchitecture; the benchmark node has none of those. What local measurement
is good for is **ranking your own changes against each other**, which is what
iteration actually needs.

Two things to keep in mind when you read them:

- The **probe cost** printed alongside your p50 is the fixed overhead of the two
  `rdtscp` reads bracketing every event. It is added to every sample, so
  ordering is preserved but gaps are compressed. If it is a large fraction of
  your p50, a 2x improvement in your engine will move the ranked number by much
  less than 2x.
- Run the benchmark **more than once**. A single run measures one memory layout
  on one thermal state. The server takes the median of 7–10 runs for exactly
  this reason.

## Ground rules

- One file: `src/engine.cpp`. It may include the frozen headers and the C++20
  standard library, nothing else.
- Integer prices only. No floating point in matching logic.
- No threads.
- Reading harness memory other than the events passed to your engine is
  **disqualifying**. The decoded stream lives in the same address space as your
  code; this is handled by rule, and enforced by source review of the top
  finishers.
- Submissions are plagiarism-checked across the cohort.

Read `SPEC.md` before writing code. Every rule referenced by a `// TODO` in the
skeleton is numbered there, and the numbers are the contract.
