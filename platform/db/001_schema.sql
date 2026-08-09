-- 001_schema.sql — the whole data model.
--
-- The queue IS the submissions table: claim-and-commit on FOR UPDATE SKIP
-- LOCKED. At ~200 submissions across the event a Postgres table is a perfectly
-- good queue, and because the claim IS the state transition, a whole
-- crash-consistency class disappears.

CREATE TYPE sub_state AS ENUM (
  'received','compiling','compile_failed',
  'verifying','verify_failed','verify_timeout','verify_passed',
  'bench_queued','pending_benchmark',      -- pending_benchmark = bench node unhealthy
  'benchmarking','bench_verify_failed',    -- passed correctness, diverged on bench stream
  'done','error');

CREATE TABLE participants (                 -- preloaded roster of 18; no credentials
  id          serial PRIMARY KEY,
  handle      text UNIQUE NOT NULL,
  created_at  timestamptz DEFAULT now()
);

CREATE TABLE submissions (
  id            bigserial PRIMARY KEY,
  participant_id int REFERENCES participants(id) NOT NULL,
  source_hash   text NOT NULL,             -- sha256 of source; S3 + compile-cache key
  state         sub_state NOT NULL DEFAULT 'received',
  claimed_by    text,                      -- worker id, audit only; cleared by janitor
  requeue_priority int NOT NULL DEFAULT 0, -- 1 = front of bench queue (steal-time requeue)
  created_at    timestamptz DEFAULT now(),
  updated_at    timestamptz DEFAULT now(),
  -- correctness results
  verify_seed       bigint,
  verify_detail     jsonb,   -- progress breakdown, first-divergence reproducer, timeout flag
  verify_digest     text,    -- field-wise digest of the submission's output
  -- benchmark results
  bench_seed_set    bigint[],
  p50_ns            double precision,      -- median of per-run p50s
  p95_ns            double precision,      -- reported, unranked
  p99_ns            double precision,      -- reported, unranked
  probe_cost_ns     double precision,      -- published alongside
  run_p50s_ns       double precision[],    -- all 7-10 per-run p50s, for the bootstrap CI
  discard_count     int DEFAULT 0,         -- steal-time discards (3 => operator alert)
  -- Percentile curve from the run whose p50 IS the score, so the published
  -- distribution and the published headline describe the same run.
  percentiles       jsonb,
  histogram_s3      text,
  flamegraph_s3     text
);
CREATE INDEX ON submissions (participant_id, created_at DESC);
CREATE INDEX ON submissions (state) WHERE state IN ('received','verify_passed','bench_queued');

CREATE TABLE bench_slots (                  -- rate limiting, one row per participant
  participant_id int PRIMARY KEY REFERENCES participants(id),
  last_bench_at  timestamptz,
  pending_sub    bigint REFERENCES submissions(id)   -- max one pending bench job
);

CREATE TABLE drafts (
  participant_id int PRIMARY KEY REFERENCES participants(id),
  source        text NOT NULL,
  updated_at    timestamptz DEFAULT now()
);

CREATE TABLE run_jobs (                     -- LeetCode "Run": ephemeral, not submissions
  id bigserial PRIMARY KEY,
  participant_id int REFERENCES participants(id) NOT NULL,
  source_hash text NOT NULL,
  state text NOT NULL DEFAULT 'received',   -- received|running|done|failed
  result jsonb,                             -- per-test pass/fail + compiler stderr
  claimed_by text,
  created_at timestamptz DEFAULT now(),
  updated_at timestamptz DEFAULT now()
);
CREATE INDEX ON run_jobs (state, id);

CREATE TABLE events_log (                   -- operator audit trail
  id bigserial PRIMARY KEY, at timestamptz DEFAULT now(),
  submission_id bigint, kind text, detail jsonb
);

-- Worker liveness. The API marks the bench node unhealthy after 90s of silence,
-- and jobs then park in pending_benchmark rather than erroring (plan section 2).
CREATE TABLE workers (
  id          text PRIMARY KEY,
  role        text NOT NULL,                -- pool | bench
  last_seen   timestamptz NOT NULL DEFAULT now(),
  healthy     boolean NOT NULL DEFAULT true,
  detail      jsonb
);

-- Operator-controlled switches, so freezing the leaderboard or parking the
-- bench node does not need a deploy mid-event.
CREATE TABLE settings (
  key   text PRIMARY KEY,
  value jsonb NOT NULL
);
INSERT INTO settings (key, value) VALUES
  ('leaderboard_frozen', 'false'::jsonb),
  ('bench_lane_open',    'true'::jsonb);

-- Postgres stays the leaderboard's source of truth. Redis holds a disposable
-- serving copy that is rebuilt from this view whenever it is suspect — which is
-- cheap, because it is 18 rows.
CREATE VIEW leaderboard AS
SELECT DISTINCT ON (s.participant_id)
       p.handle,
       s.participant_id,
       s.id AS submission_id,
       s.p50_ns,
       s.p99_ns,
       s.probe_cost_ns,
       s.run_p50s_ns,
       s.updated_at
FROM submissions s
JOIN participants p ON p.id = s.participant_id
WHERE s.state = 'done' AND s.p50_ns IS NOT NULL
ORDER BY s.participant_id, s.created_at DESC;
