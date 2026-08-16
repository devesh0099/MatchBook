// Thin JSON client. The frontend owns every pixel; the API owns state and the
// queue, and returns nothing but JSON.

export const API_BASE = process.env.NEXT_PUBLIC_API_BASE ?? '/api';

export type SubState =
  | 'received'
  | 'compiling'
  | 'compile_failed'
  | 'testing'
  | 'tests_failed'
  | 'verifying'
  | 'verify_failed'
  | 'verify_timeout'
  | 'phase1'
  | 'phase1_failed'
  | 'phase2'
  | 'done'
  | 'error';

export interface LadderLevel {
  level: number;
  outcome: string;
  events: number;
  deadline_ms: number;
  p50_ns: number;
  p95_ns: number;
  p99_ns: number;
  wall_s?: number | null;
  events_processed?: number;
  checked_events?: number | null;
  notes?: string | null;
}

export interface Phase1Result {
  outcome: string;
  p50_ns: number;
  p95_ns: number;
  p99_ns: number;
  events: number;
  deadline_ms: number;
  events_processed?: number;
  runs?: unknown[] | null;
  percentiles?: { p: number; ns: number; count?: number; inv?: number }[] | null;
  notes?: string | null;
}

export interface Submission {
  id: number;
  participant_id: number;
  source_hash: string;
  state: SubState;
  created_at: string | null;
  updated_at: string | null;
  /// One stable blob per phase — the same contract the dashboard reads.
  phase0: {
    compile?: { stderr?: string };
    tests?: { total: number; passed: number; tests: { name: string; rule: string; passed: boolean; detail: string }[] };
    verify?: VerifyDetail;
  } | null;
  phase1: Phase1Result | null;
  phase2: { levels: LadderLevel[] } | null;
  max_level: number | null;
  top_p50_ns: number | null;
  top_p95_ns: number | null;
  top_p99_ns: number | null;
}

export interface OutEventView {
  type: string;
  in_seq: number;
  price: number;
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
  price: number;
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
  /// Set when correctness passed but the benchmark lane declined the job.
  /// Never left silent: a submission sitting at verify_passed with no
  /// explanation is the failure this field exists to prevent.
  bench_held?: string;
  events_processed?: number;
  total_events?: number;
}

export interface SubmitResponse {
  submission_id: number;
  source_hash: string;
}

export interface LeaderboardEntry {
  handle: string;
  submission_id: number | null;
  /// Highest ladder level cleared; null for a contestant who has not ranked yet
  /// (still on correctness).
  max_level: number | null;
  /// The scoring chain at that level: p95, then p50, then p99. Null until ranked.
  chain_p95_ns: number | null;
  chain_p50_ns: number | null;
  chain_p99_ns: number | null;
  /// Best visible-test pass count (progress board); 0 if never compiled.
  tests_passed?: number;
  /// Visible-test total; null until the tests have been run at all.
  tests_total?: number | null;
}

export interface FinalEntry {
  handle: string;
  submission_id: number;
  finished: boolean | null;
  p95_ns: number | null;
  p50_ns: number | null;
  p99_ns: number | null;
  events_processed: number | null;
}

export interface RunResult {
  compiled: boolean;
  stderr?: string;
  tests?: {
    total: number;
    passed: number;
    tests: { name: string; rule: string; passed: boolean; detail: string }[];
  };
  /// Phase I feedback — the exact instrument that judges, on every Run.
  phase1?: Phase1Result;
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
  // Identity: the ONLY time credentials cross, and the cookie does the rest.
  login: (handle: string, password: string) =>
    call<{ participant_id: number; handle: string }>('/login', {
      method: 'POST',
      body: JSON.stringify({ handle, password }),
    }),
  logout: () => call<{ signed_out: boolean }>('/logout', { method: 'POST' }),
  session: () => call<{ participant_id: number; handle: string }>('/session'),

  // Everything below is implicitly "mine" — the server derives the
  // participant from the session cookie; no id is ever sent.
  getDraft: () => call<{ source: string | null }>('/draft'),

  saveDraft: (source: string) =>
    call<{ saved: boolean }>('/draft', {
      method: 'PUT',
      body: JSON.stringify({ source }),
    }),

  run: (source: string) =>
    call<{ run_id: number; source_hash: string }>('/run', {
      method: 'POST',
      body: JSON.stringify({ source }),
    }),

  submit: (source: string) =>
    call<SubmitResponse>('/submit', {
      method: 'POST',
      body: JSON.stringify({ source }),
    }),

  runResult: (id: number) =>
    call<{ run: { id: number; state: string; result: RunResult | null }; terminal: boolean }>(
      `/runs/${id}`,
    ),

  submission: (id: number) =>
    call<{ submission: Submission; terminal: boolean }>(`/submissions/${id}`),

  mine: () => call<Submission[]>('/me'),

  leaderboard: () => call<{ frozen: boolean; entries: LeaderboardEntry[] }>('/leaderboard'),

  /// Golden-box numbers on the sealed seed — the standings that decide the
  /// contest. Empty until the operator publishes them.
  final: () => call<{ published: boolean; entries: FinalEntry[] }>('/final'),

  /// The provisional standings as they stood at the start of Phase III.
  provisional: () => call<{ entries: LeaderboardEntry[] }>('/provisional'),

  /// The one-at-a-time rule, by exactly the check submit() enforces — so the
  /// editor's message can never promise a submission the server would refuse.
  queue: () =>
    call<{ evaluating: boolean; submission_id: number | null; reason: string }>('/queue'),
};

// ---------------------------------------------------------------- operator

export interface OpBoxRow {
  id: string;
  role: string;
  participant_id: number | null;
  healthy: boolean;
  last_seen_secs_ago: number;
  /// Live telemetry, carried on every heartbeat.
  job: string | null;
  load1: number | null;
  mem_avail_mb: number | null;
  steal_total: number | null;
}

export interface OpHealth {
  ok: boolean;
  flags: Record<string, unknown>[];
  boxes: { total: number; unhealthy: number; rows: OpBoxRow[] };
  submissions: { state: string; count: number }[];
  run_jobs_pending: number;
  rejudge: { state: string; count: number }[];
  recent_self_healing: { kind: string; at: string }[];
}

export interface OpParticipantRow {
  participant_id: number;
  handle: string;
  box: string | null;
  /// Lifecycle: none | deploying | ready | redeploying | failed.
  box_state: string;
  /// Live activity string: 'no box' | 'deploying…' | 'IDLE' | 'Phase I' |
  /// 'Phase II · L3' | 'box unhealthy' | …
  activity: string;
  latest_submission: number | null;
  best_level: number | null;
  /// When the in-flight box request was enqueued (ISO), for the elapsed clock
  /// while a box is deploying/redeploying/removing. Null when nothing's in
  /// flight.
  busy_since: string | null;
}

/// The /admin dashboard's surface: the same operator actions the loopback
/// listener offers, behind the operator session cookie.
export const op = {
  login: (email: string, password: string) =>
    call<{ operator: boolean }>('/op/login', {
      method: 'POST',
      body: JSON.stringify({ email, password }),
    }),
  logout: () => call<{ signed_out: boolean }>('/op/logout', { method: 'POST' }),
  session: () => call<{ operator: boolean }>('/op/session'),
  health: () => call<OpHealth>('/op/health'),
  participants: () => call<{ participants: OpParticipantRow[] }>('/op/participants/status'),
  events: (limit = 40) =>
    call<{ events: { id: number; at: string; submission_id: number | null; kind: string; detail: unknown }[] }>(
      `/op/events?limit=${limit}`,
    ),
  issue: (handle: string) =>
    call<{ participant_id: number; handle: string; password: string }>('/op/participants', {
      method: 'POST',
      body: JSON.stringify({ handle }),
    }),
  act: (path: string) => call<Record<string, unknown>>(`/op/${path}`, { method: 'POST' }),
  // Box lifecycle (M8): the dashboard's Deploy / Redeploy / Terminate.
  deployBox: (id: number) => call<Record<string, unknown>>(`/op/participants/${id}/deploy`, { method: 'POST' }),
  redeployBox: (id: number) => call<Record<string, unknown>>(`/op/participants/${id}/redeploy`, { method: 'POST' }),
  terminateBox: (id: number) => call<Record<string, unknown>>(`/op/participants/${id}/terminate`, { method: 'POST' }),
  removeParticipant: (id: number) => call<Record<string, unknown>>(`/op/participants/${id}/remove`, { method: 'POST' }),
  rejudgeStatus: () => call<RejudgeStatus>('/op/rejudge/status'),
  // Student monitoring: drill into a participant's submissions, then one
  // submission's full analytics + source.
  studentSubmissions: (id: number) =>
    call<{ participant_id: number; handle: string | null; submissions: Submission[] }>(
      `/op/participants/${id}/submissions`,
    ),
  submissionDetail: (id: number) =>
    call<{ submission: Submission; source: string }>(`/op/submissions/${id}`),
};

export interface RejudgeStatus {
  golden: { id: string; healthy: boolean; last_seen_secs_ago: number; detail: Record<string, unknown> | null } | null;
  progress: { total: number; done: number; running: number; pending: number; errored: number };
  current: string | null;
  results: {
    handle: string; finished: boolean | null;
    p95_ns: number | null; p50_ns: number | null; p99_ns: number | null; events_processed: number | null;
  }[];
}

/// Which states are still moving. The UI polls every 2s while non-terminal —
/// plain JSON polling, no websockets.
export function isTerminal(state: SubState): boolean {
  return [
    'compile_failed',
    'tests_failed',
    'verify_failed',
    'verify_timeout',
    'phase1_failed',
    'done',
    'error',
  ].includes(state);
}

export function stateLabel(state: SubState): string {
  switch (state) {
    case 'received':
      return 'Waiting for your box';
    case 'compiling':
      return 'Compiling';
    case 'compile_failed':
      return 'Compile failed';
    case 'testing':
      return 'Running the spec tests';
    case 'tests_failed':
      return 'Spec tests failed';
    case 'verifying':
      return 'Checking correctness';
    case 'verify_failed':
      return 'Wrong output';
    // A liveness failure, never shown as a wrong answer: they are different
    // bugs and conflating them wastes debugging time.
    case 'verify_timeout':
      return 'Timed out';
    case 'phase1':
      return 'Phase I — bulk run';
    case 'phase1_failed':
      return 'Phase I failed';
    case 'phase2':
      return 'Phase II — climbing the ladder';
    case 'done':
      return 'Done';
    case 'error':
      return 'Platform error';
  }
}

export function stateTone(state: SubState): 'good' | 'bad' | 'warn' | 'busy' {
  switch (state) {
    case 'done':
      return 'good';
    case 'compile_failed':
    case 'tests_failed':
    case 'verify_failed':
    case 'phase1_failed':
    case 'error':
      return 'bad';
    case 'verify_timeout':
      return 'warn';
    default:
      return 'busy';
  }
}
