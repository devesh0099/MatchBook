// One mapping from submission state to the design's five state colours, used by
// every view that renders a state.
//
// The states stay distinct on purpose. "Wrong output", "timed out" and "the
// bench node is down" are three different problems with three different next
// actions, and a single red X for all of them costs a participant the time it
// takes to work out which one they have.

import type { SubState } from '@/lib/api';

export type Tone = 'pass' | 'fail' | 'slow' | 'bench' | 'idle';

export const TONE_INK: Record<Tone, string> = {
  pass: 'var(--s-pass)',
  fail: 'var(--s-fail)',
  slow: 'var(--s-slow)',
  bench: 'var(--s-bench)',
  idle: 'var(--s-idle)',
};

export const TONE_BG: Record<Tone, string> = {
  pass: 'var(--s-pass-bg)',
  fail: 'var(--s-fail-bg)',
  slow: 'var(--s-slow-bg)',
  bench: 'var(--s-bench-bg)',
  idle: 'var(--s-idle-bg)',
};

export function toneOf(state: SubState): Tone {
  switch (state) {
    case 'done':
    case 'verify_passed':
      return 'pass';
    case 'compile_failed':
    case 'verify_failed':
    case 'bench_verify_failed':
    case 'error':
      return 'fail';
    case 'verify_timeout':
      return 'slow';
    case 'pending_benchmark':
    case 'superseded':
      return 'idle';
    case 'benchmarking':
    case 'bench_queued':
      return 'bench';
    default:
      return 'idle';
  }
}

export const inkOf = (s: SubState) => TONE_INK[toneOf(s)];
export const bgOf = (s: SubState) => TONE_BG[toneOf(s)];

/// A short label for tight spaces — the editor's submission rail and the list
/// row, both of which are too narrow for the full sentence.
///
/// `stateLabel` stays long on purpose: on the detail page it is the headline
/// and has room to say "Benchmark node unavailable — held, not lost", which is
/// the whole point of distinguishing that state. Wrapped across two lines in a
/// 336px rail it just looks broken.
export function stateShort(state: SubState): string {
  switch (state) {
    case 'received':
      return 'queued';
    case 'compiling':
      return 'compiling';
    case 'compile_failed':
      return 'compile failed';
    case 'verifying':
      return 'verifying';
    case 'verify_failed':
      return 'wrong output';
    case 'verify_timeout':
      return 'timed out';
    case 'verify_passed':
      return 'correct';
    case 'bench_queued':
      return 'queued for bench';
    case 'pending_benchmark':
      return 'held';
    case 'benchmarking':
      return 'benchmarking';
    case 'bench_verify_failed':
      return 'diverged on bench';
    case 'superseded':
      return 'superseded';
    case 'done':
      return 'done';
    case 'error':
      return 'platform error';
  }
}

/// The one-line explanation under the state title. Written to say what happened
/// and what to do next, because that is what someone reads at hour four.
export function stateBlurb(state: SubState): string {
  switch (state) {
    case 'received':
      return 'Queued for a pool worker. Compilation starts within a couple of seconds.';
    case 'compiling':
      return 'Building your translation unit against the frozen headers.';
    case 'compile_failed':
      return 'The submission did not build, so nothing ran. Your benchmark slot was not consumed — fix it and submit again immediately.';
    case 'verifying':
      return 'Running your engine and the reference side by side over a stream you have never seen.';
    case 'verify_failed':
      return 'Your engine built and ran, but diverged from the reference. The divergence has been shrunk to a minimal reproducer below.';
    case 'verify_timeout':
      return 'Your engine stopped producing output before the stream ended. This is a liveness bug, not a wrong answer — the output matched up to the point it stalled.';
    case 'verify_passed':
      return 'Correct on every rule. The timing run is queued, unless a benchmark of yours is already running — in that case this one takes the slot automatically when that finishes.';
    case 'bench_queued':
      return 'Waiting for the benchmark node, which runs exactly one submission at a time.';
    case 'pending_benchmark':
      return 'Correctness passed and is recorded. The bench node is unavailable, so the timing run is held and will start on its own.';
    case 'benchmarking':
      return 'Nine timed runs against a 300,000-order book on an isolated core. This page updates itself.';
    case 'bench_verify_failed':
      return 'Your engine passed correctness but produced different output while being timed. A ranked run is verified as it runs, so this cannot be scored.';
    case 'superseded':
      return 'Correct, but never timed: you submitted newer code that passed while this one was still waiting for the benchmark node, and it took this slot. Nothing was lost — the newer submission is the one being ranked.';
    case 'done':
      return 'Median of the nine per-run p50s. The interval below is the spread across runs, not a confidence bound.';
    case 'error':
      return 'The platform failed, not your code. This is being looked at; the submission can be rejudged.';
  }
}
