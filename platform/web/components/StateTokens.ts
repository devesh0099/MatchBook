// One mapping from submission state to the design's five state colours, used by
// every view that renders a state.
//
// The states stay distinct on purpose. "Wrong output", "timed out" and
// "missed the Phase I gate" are different problems with different next
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
      return 'pass';
    case 'compile_failed':
    case 'tests_failed':
    case 'verify_failed':
    case 'phase1_failed':
    case 'error':
      return 'fail';
    case 'verify_timeout':
      return 'slow';
    case 'phase1':
    case 'phase2':
      return 'bench';
    default:
      return 'idle';
  }
}

export const inkOf = (s: SubState) => TONE_INK[toneOf(s)];
export const bgOf = (s: SubState) => TONE_BG[toneOf(s)];

/// A short label for tight spaces — the editor's submission rail and the list
/// row, both of which are too narrow for the full sentence.
export function stateShort(state: SubState): string {
  switch (state) {
    case 'received':
      return 'queued';
    case 'compiling':
      return 'compiling';
    case 'compile_failed':
      return 'compile failed';
    case 'testing':
      return 'spec tests';
    case 'tests_failed':
      return 'tests failed';
    case 'verifying':
      return 'verifying';
    case 'verify_failed':
      return 'wrong output';
    case 'verify_timeout':
      return 'timed out';
    case 'phase1':
      return 'phase I';
    case 'phase1_failed':
      return 'phase I failed';
    case 'phase2':
      return 'climbing';
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
      return 'Queued for your box. Compilation starts within a couple of seconds.';
    case 'compiling':
      return 'Building your translation unit against the frozen headers.';
    case 'compile_failed':
      return 'The submission did not build, so nothing ran. Fix it and submit again immediately.';
    case 'testing':
      return 'Running the visible spec tests — all of them must pass.';
    case 'tests_failed':
      return 'One or more spec tests failed. The failing rules are listed below; Run reproduces them locally in seconds.';
    case 'verifying':
      return 'Comparing your engine against the reference over a hidden stream, event by event.';
    case 'verify_failed':
      return 'Your engine passed the visible tests but diverged on the hidden stream. The divergence has been shrunk to a minimal reproducer below.';
    case 'verify_timeout':
      return 'Your engine stopped producing output before the stream ended. This is a liveness bug, not a wrong answer — the output matched up to the point it stalled.';
    case 'phase1':
      return 'Phase I: three gated runs over the bulk stream, measuring the scoring chain (p95, then p50, then p99). The deadline is a gate, never a score.';
    case 'phase1_failed':
      return 'The Phase I gate was missed — too slow to finish the bulk stream in its budget, or the output diverged mid-stream. The details below say which, and where.';
    case 'phase2':
      return 'Phase II: climbing the ladder, one attempt per level. Each cleared level records your chain; the climb ends at the first missed deadline. This page updates itself.';
    case 'done':
      return 'The climb is recorded. You rank on the highest level cleared, tie-broken by the chain at that level. Live numbers come from your own box; the final standings are re-measured on the golden box after the contest.';
    case 'error':
      return 'The platform failed, not your code. This is being looked at; the submission can be re-evaluated.';
  }
}
