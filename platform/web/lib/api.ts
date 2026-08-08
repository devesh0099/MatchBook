// Thin JSON client. The frontend owns every pixel; the API owns state and the
// queue, and returns nothing but JSON.

export const API_BASE = process.env.NEXT_PUBLIC_API_BASE ?? '/api';

export type SubState =
  | 'received'
  | 'compiling'
  | 'compile_failed'
  | 'verifying'
  | 'verify_failed'
  | 'verify_timeout'
  | 'verify_passed'
  | 'bench_queued'
  | 'pending_benchmark'
  | 'benchmarking'
  | 'bench_verify_failed'
  | 'done'
  | 'error';

export interface Participant {
  id: number;
  handle: string;
}

export interface Submission {
  id: number;
  participant_id: number;
  source_hash: string;
  state: SubState;
  created_at: string | null;
  updated_at: string | null;
  verify_detail: VerifyDetail | null;
  p50_ns: number | null;
  p99_ns: number | null;
  probe_cost_ns: number | null;
  run_p50s_ns: number[] | null;
  discard_count: number | null;
  histogram_s3: string | null;
  flamegraph_s3: string | null;
}

export interface OutEventView {
  type: string;
  in_seq: number;
  px: number;
  qty: number;
  maker_session: number;
  maker_coid: number;
  maker_firm: number;
  taker_session: number;
  taker_coid: number;
  taker_firm: number;
  reason: string;
}

export interface ReproEvent {
  seq: number;
  type: string;
  side: string;
  tif: string;
  px: number;
  qty: number;
  session: number;
  coid: number;
  firm: number;
}

export interface VerifyDetail {
  outcome?: string;
  passed?: number;
  in_seq?: number;
  output_index?: number;
  message?: string;
  expected?: OutEventView;
  actual?: OutEventView;
  reproducer?: ReproEvent[];
  progress?: { area: string; exercised: number; failed: number }[];
  stderr?: string;
  hint?: string;
  /// isolate's own verdict when the engine died on a signal (SG).
  status?: string;
  events_processed?: number;
  total_events?: number;
}

export interface SubmitResponse {
  submission_id: number;
  source_hash: string;
  bench_will_queue: boolean;
  bench_reason: string;
  bench_wait_secs: number;
}

export interface LeaderboardEntry {
  handle: string;
  band: string;
  p50_ns: number;
  p99_ns: number | null;
  probe_cost_ns: number | null;
  ci_low_ns: number;
  ci_high_ns: number;
  submission_id: number;
}

export interface RunResult {
  compiled: boolean;
  stderr?: string;
  tests?: {
    total: number;
    passed: number;
    tests: { name: string; rule: string; passed: boolean; detail: string }[];
  };
  error?: string;
}

async function call<T>(path: string, init?: RequestInit): Promise<T> {
  const res = await fetch(`${API_BASE}${path}`, {
    ...init,
    headers: { 'content-type': 'application/json', ...(init?.headers ?? {}) },
    cache: 'no-store',
  });
  if (!res.ok) {
    let detail = `${res.status}`;
    try {
      const body = await res.json();
      if (body?.error) detail = body.error;
    } catch {
      /* the status alone will have to do */
    }
    throw new Error(detail);
  }
  return res.json() as Promise<T>;
}

export const api = {
  participants: () => call<Participant[]>('/participants'),

  getDraft: (participantId: number) =>
    call<{ source: string | null }>(`/draft?participant_id=${participantId}`),

  saveDraft: (participantId: number, source: string) =>
    call<{ saved: boolean }>('/draft', {
      method: 'PUT',
      body: JSON.stringify({ participant_id: participantId, source }),
    }),

  run: (participantId: number, source: string) =>
    call<{ run_id: number; source_hash: string }>('/run', {
      method: 'POST',
      body: JSON.stringify({ participant_id: participantId, source }),
    }),

  submit: (participantId: number, source: string) =>
    call<SubmitResponse>('/submit', {
      method: 'POST',
      body: JSON.stringify({ participant_id: participantId, source }),
    }),

  runResult: (id: number) =>
    call<{ run: { id: number; state: string; result: RunResult | null }; terminal: boolean }>(
      `/runs/${id}`,
    ),

  submission: (id: number) =>
    call<{ submission: Submission; terminal: boolean }>(`/submissions/${id}`),

  mine: (participantId: number) => call<Submission[]>(`/me?participant_id=${participantId}`),

  leaderboard: () => call<{ frozen: boolean; entries: LeaderboardEntry[] }>('/leaderboard'),

  queue: (participantId: number) =>
    call<{ depth: number; ahead: number; eta_secs: number }>(
      `/queue?participant_id=${participantId}`,
    ),
};

/// Which states are still moving. The UI polls every 2s while non-terminal —
/// plain JSON polling, no websockets.
export function isTerminal(state: SubState): boolean {
  return [
    'compile_failed',
    'verify_failed',
    'verify_timeout',
    'bench_verify_failed',
    'done',
    'error',
  ].includes(state);
}

export function stateLabel(state: SubState): string {
  switch (state) {
    case 'received':
      return 'Queued';
    case 'compiling':
      return 'Compiling';
    case 'compile_failed':
      return 'Compile failed';
    case 'verifying':
      return 'Checking correctness';
    case 'verify_failed':
      return 'Wrong output';
    // A liveness failure, never shown as a wrong answer: they are different
    // bugs and conflating them wastes debugging time.
    case 'verify_timeout':
      return 'Timed out';
    case 'verify_passed':
      return 'Correct';
    case 'bench_queued':
      return 'Waiting for the benchmark node';
    case 'pending_benchmark':
      return 'Benchmark node unavailable — held, not lost';
    case 'benchmarking':
      return 'Benchmarking';
    case 'bench_verify_failed':
      return 'Diverged during the benchmark run';
    case 'done':
      return 'Done';
    case 'error':
      return 'Platform error';
  }
}

export function stateTone(state: SubState): 'good' | 'bad' | 'warn' | 'busy' {
  switch (state) {
    case 'done':
    case 'verify_passed':
      return 'good';
    case 'compile_failed':
    case 'verify_failed':
    case 'bench_verify_failed':
    case 'error':
      return 'bad';
    case 'verify_timeout':
    case 'pending_benchmark':
      return 'warn';
    default:
      return 'busy';
  }
}
