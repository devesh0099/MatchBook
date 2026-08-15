-- 001_schema.sql — the whole data model, v2 (PLAN-measurement-redesign).
--
-- The queue as a contention point is gone — every participant has their own
-- box — but the state machine stays: jobs are rows, the claim is the state
-- transition on FOR UPDATE SKIP LOCKED, and crash recovery stays one janitor
-- statement per stage. Each box's agent polls only its own participant's
-- jobs, so the per-box "queue" is a mailbox of depth <= 1.
--
-- One scoring chain judges everything: p95, then p50, then p99. Deadlines are
-- gates, never scores. The phase blobs below carry one stable JSON schema per
-- phase — the student page, the leaderboard, and the future admin dashboard
-- all read the same blobs.

CREATE TYPE sub_state AS ENUM (
  'received','compiling','compile_failed',
  'testing','tests_failed',            -- phase 0: the visible spec tests
  'verifying','verify_failed','verify_timeout',  -- phase 0: hidden differential verify
  'phase1','phase1_failed',            -- the bulk run: 3 gated runs, median chain
  'phase2',                            -- the ladder; a stopped climb is still 'done'
  'done','error');

CREATE TABLE participants (
  id          serial PRIMARY KEY,
  handle      text UNIQUE NOT NULL,
  -- Argon2id hash of the issued password (ops/issue-credentials.sh). NULL
  -- means no credentials issued yet, and login is refused — there is no
  -- anonymous path.
  credential_hash text,
  -- The participant -> box binding (M4). Informational here; the agent's own
  -- config is what actually scopes its job claims.
  box_id      text,
  created_at  timestamptz DEFAULT now()
);

-- Login sessions. The cookie carries a random token; only its sha256 is
-- stored, so a leaked table replays nothing. One-day event, so expiry is a
-- fixed window enforced at lookup rather than a refresh protocol.
CREATE TABLE sessions (
  token_hash     text PRIMARY KEY,
  participant_id int REFERENCES participants(id) NOT NULL,
  created_at     timestamptz DEFAULT now(),
  last_seen      timestamptz DEFAULT now()
);

CREATE TABLE submissions (
  id            bigserial PRIMARY KEY,
  participant_id int REFERENCES participants(id) NOT NULL,
  source_hash   text NOT NULL,             -- sha256 of source; S3 + compile-cache key
  state         sub_state NOT NULL DEFAULT 'received',
  claimed_by    text,                      -- agent id, audit only; cleared by janitor
  created_at    timestamptz DEFAULT now(),
  updated_at    timestamptz DEFAULT now(),
  -- One blob per phase, stable schema each:
  --   phase0: { compile: {stderr?}, tests: {total,passed,tests[]},
  --             verify: {outcome, reproducer?, ...} }
  --   phase1: { outcome, p50_ns, p95_ns, p99_ns, runs[], deadline_ms, events }
  --   phase2: { levels: [{level, outcome, events, deadline_ms, p50_ns, p95_ns,
  --             p99_ns, wall_s, events_processed, checked_events}] }
  phase0        jsonb,
  phase1        jsonb,
  phase2        jsonb,
  -- The leaderboard sort keys, denormalized from phase2's last cleared level.
  -- max_level 0 with state 'done' means the climb ended at rung 1 — still
  -- ranked, sub-ordered by the phase1 chain (the CASE in the view below).
  max_level     int,
  top_p50_ns    double precision,
  top_p95_ns    double precision,
  top_p99_ns    double precision
);
CREATE INDEX ON submissions (participant_id, created_at DESC);
CREATE INDEX ON submissions (state) WHERE state IN ('received');

CREATE TABLE drafts (
  participant_id int PRIMARY KEY REFERENCES participants(id),
  source        text NOT NULL,
  updated_at    timestamptz DEFAULT now()
);

CREATE TABLE run_jobs (                     -- "Run": phase 0 tests + phase 1, feedback only
  id bigserial PRIMARY KEY,
  participant_id int REFERENCES participants(id) NOT NULL,
  source_hash text NOT NULL,
  state text NOT NULL DEFAULT 'received',   -- received|running|done|failed
  result jsonb,                             -- {compiled, tests, phase1}
  claimed_by text,
  created_at timestamptz DEFAULT now(),
  updated_at timestamptz DEFAULT now()
);
CREATE INDEX ON run_jobs (state, id);

CREATE TABLE events_log (                   -- operator audit trail
  id bigserial PRIMARY KEY, at timestamptz DEFAULT now(),
  submission_id bigint, kind text, detail jsonb
);

-- The box registry — what the future admin dashboard watches. Replaces the
-- old `workers` table: a row per machine-resident agent, bound to its
-- participant. `detail` carries the hygiene report, steal events, agent
-- version, current job.
CREATE TABLE boxes (
  id             text PRIMARY KEY,
  role           text NOT NULL,             -- agent | golden
  participant_id int REFERENCES participants(id),
  instance_id    text,
  ip             text,
  healthy        boolean NOT NULL DEFAULT true,
  last_seen      timestamptz NOT NULL DEFAULT now(),
  detail         jsonb
);

-- Phase III, written by the golden box during the post-contest rejudge (M6).
-- Final standings are a view over this table; nothing live writes here.
CREATE TABLE rejudge_results (
  id              bigserial PRIMARY KEY,
  participant_id  int REFERENCES participants(id) NOT NULL,
  submission_id   bigint REFERENCES submissions(id) NOT NULL,
  seed            bigint,
  finished        boolean,                  -- completed X events under the deadline
  wall_s          double precision,
  events_processed bigint,
  p50_ns          double precision,
  p95_ns          double precision,
  p99_ns          double precision,
  detail          jsonb,
  judged_at       timestamptz DEFAULT now()
);

-- Operator-controlled switches and the workload configuration, so changing a
-- deadline or freezing the board does not need a deploy mid-event.
CREATE TABLE settings (
  key   text PRIMARY KEY,
  value jsonb NOT NULL
);
-- The numbers below are DEV PLACEHOLDERS sized for a laptop. M5's calibration
-- afternoon replaces them with the real level table and writes it into the
-- spec; every deadline is a gate, never a score.
INSERT INTO settings (key, value) VALUES
  ('leaderboard_frozen', 'false'::jsonb),
  ('phase1', '{"events": 500000, "live_target": 1000, "deadline_ms": 5000, "runs": 3}'::jsonb),
  ('level_table', '[
    {"level": 1, "events":  500000, "live_target": 1000, "deadline_ms": 3000},
    {"level": 2, "events": 1000000, "live_target": 1000, "deadline_ms": 1000},
    {"level": 3, "events": 2000000, "live_target": 1000, "deadline_ms": 1000},
    {"level": 4, "events": 4000000, "live_target": 1000, "deadline_ms": 1300}
   ]'::jsonb);

-- The live ranking: highest ladder level, then the scoring chain (p95, p50,
-- p99) at that level, then the earlier submission. Level-0 rows rank on their
-- Phase I chain so the board stays total-ordered from the first hour.
--
-- BEST per participant, not latest: DISTINCT ON keeps the first row of each
-- participant's group under this ORDER BY. The rejudge (Phase III) does not
-- read this view — final standings come from rejudge_results.
CREATE VIEW leaderboard AS
SELECT DISTINCT ON (s.participant_id)
       p.handle,
       s.participant_id,
       s.id AS submission_id,
       s.max_level,
       CASE WHEN s.max_level > 0 THEN s.top_p95_ns
            ELSE (s.phase1->>'p95_ns')::double precision END AS chain_p95_ns,
       CASE WHEN s.max_level > 0 THEN s.top_p50_ns
            ELSE (s.phase1->>'p50_ns')::double precision END AS chain_p50_ns,
       CASE WHEN s.max_level > 0 THEN s.top_p99_ns
            ELSE (s.phase1->>'p99_ns')::double precision END AS chain_p99_ns,
       s.updated_at,
       -- The final deterministic tie-break: with rdtscp stepping in ~10 ns
       -- quanta, an exact full-chain tie is possible, and it falls to the
       -- earlier submission. created_at, never updated_at — requeues rewrite
       -- updated_at into arbitrary order.
       s.created_at
FROM submissions s
JOIN participants p ON p.id = s.participant_id
WHERE s.state = 'done' AND s.max_level IS NOT NULL
ORDER BY s.participant_id,
         s.max_level DESC,
         CASE WHEN s.max_level > 0 THEN s.top_p95_ns
              ELSE (s.phase1->>'p95_ns')::double precision END ASC NULLS LAST,
         CASE WHEN s.max_level > 0 THEN s.top_p50_ns
              ELSE (s.phase1->>'p50_ns')::double precision END ASC NULLS LAST,
         CASE WHEN s.max_level > 0 THEN s.top_p99_ns
              ELSE (s.phase1->>'p99_ns')::double precision END ASC NULLS LAST,
         s.created_at ASC;
