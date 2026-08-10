-- dev-seed.sql — local development data. NEVER run against the event database.
--
-- It lives in ops/ rather than platform/db/ for exactly that reason: compose
-- mounts platform/db into /docker-entrypoint-initdb.d, and Postgres runs every
-- .sql it finds there, in name order, the first time a cluster comes up. A file
-- whose header says "never run this" would have been run automatically on the
-- event's own database. Run it by hand:
--
--   docker exec -i me-postgres psql -U mebench -d mebench < ops/dev-seed.sql
--
-- Enough submissions to render every state the UI has a panel for, plus a few
-- other people so the leaderboard is not a single row. The percentile curve and
-- the within-run timeline are generated here rather than pasted, so this file
-- stays readable and the shapes match what the harness actually emits.

BEGIN;

DELETE FROM submissions;
DELETE FROM bench_slots;

-- The HdrHistogram percentile distribution: p50 and then successive halvings of
-- the remaining distance to 100, which is what the library's CLASSIC output
-- does. `inv` is 1/(1-p), the x axis of the standard plot.
CREATE TEMP VIEW pctl AS
WITH s(i) AS (SELECT generate_series(0, 24)),
     p AS (SELECT i, 100.0 - 50.0 / power(2, i) AS pct FROM s)
SELECT jsonb_agg(jsonb_build_object(
         'p', round(pct::numeric, 5),
         'ns', round((330 + 26 * power(greatest(0, log(1.0 / (1.0 - pct / 100)) - 0.3), 2.35))::numeric, 1),
         'count', floor(6108800 * pct / 100),
         'inv', round((1.0 / (1.0 - pct / 100))::numeric, 3))
       ORDER BY pct) AS v
FROM p WHERE pct < 100;

-- Sixty windows through the timed region: a flat p50, a mildly wandering p95,
-- and a p99 that drifts upward — which is what a book filling up looks like.
CREATE TEMP VIEW tline AS
WITH s(i) AS (SELECT generate_series(0, 59))
SELECT jsonb_agg(jsonb_build_object(
         't', round((i / 59.0)::numeric, 4),
         'event', i * 101813,
         'p50_ns', round((340 + 8 * sin(i / 5.0))::numeric, 1),
         'p95_ns', round((610 + 25 * sin(i / 3.0 + 1))::numeric, 1),
         'p99_ns', round((1780 + 90 * sin(i / 7.0) + i * 3)::numeric, 1))
       ORDER BY i) AS v
FROM s;

-- The first-divergence payload, shaped exactly as verify.cpp writes it.
CREATE TEMP VIEW divergence AS
SELECT jsonb_build_object(
  'outcome', 'diverged',
  'in_seq', 41827,
  'output_index', 2,
  'message', 'The two trades match. You rested the remainder of an IOC order instead of expiring it — an IOC never joins the book, so everything after this diverges.',
  'expected', jsonb_build_object('type','EXPIRED','in_seq',41827,'px',0,'qty',100,
     'maker_session',0,'maker_coid',0,'maker_firm',0,
     'taker_session',12,'taker_coid',51203,'taker_firm',4,'reason','none'),
  'actual', jsonb_build_object('type','ACK','in_seq',41827,'px',10042,'qty',0,
     'maker_session',0,'maker_coid',0,'maker_firm',0,
     'taker_session',12,'taker_coid',51203,'taker_firm',4,'reason','none'),
  'reproducer', jsonb_build_array(
     jsonb_build_object('seq',1,'type','NEW','side','SELL','tif','DAY','px',10042,'qty',120,'session',8,'coid',50988,'firm',3),
     jsonb_build_object('seq',2,'type','NEW','side','SELL','tif','DAY','px',10042,'qty',80,'session',9,'coid',51044,'firm',5),
     jsonb_build_object('seq',3,'type','NEW','side','SELL','tif','DAY','px',10043,'qty',500,'session',8,'coid',51100,'firm',3),
     jsonb_build_object('seq',4,'type','NEW','side','BUY','tif','IOC','px',10042,'qty',300,'session',12,'coid',51203,'firm',4)),
  'progress', jsonb_build_array(
     jsonb_build_object('area','price-time priority','exercised',812004,'failed',0),
     jsonb_build_object('area','partial fills','exercised',604112,'failed',0),
     jsonb_build_object('area','cancel','exercised',441987,'failed',0),
     jsonb_build_object('area','IOC','exercised',2881,'failed',1),
     jsonb_build_object('area','FOK','exercised',1197,'failed',0),
     jsonb_build_object('area','self-trade prevention','exercised',0,'failed',0))) AS v;

CREATE TEMP VIEW passed_progress AS
SELECT jsonb_build_array(
   jsonb_build_object('area','price-time priority','exercised',812004,'failed',0),
   jsonb_build_object('area','partial fills','exercised',604112,'failed',0),
   jsonb_build_object('area','cancel','exercised',441987,'failed',0),
   jsonb_build_object('area','IOC','exercised',2881,'failed',0),
   jsonb_build_object('area','FOK','exercised',1197,'failed',0),
   jsonb_build_object('area','self-trade prevention','exercised',9044,'failed',0)) AS v;

-- ── participant 1: one submission per state, oldest first ───────────────

INSERT INTO submissions (participant_id, source_hash, state, created_at, updated_at, verify_detail)
-- Dollar-quoted: the payload is real compiler output, full of quotes and
-- backslashes, and psql reads backslash sequences inside ordinary literals.
VALUES (1, 'c0mp1lefa11', 'compile_failed', now() - interval '240 minutes', now() - interval '239 minutes',
  jsonb_build_object('outcome','compile_failed','stderr', $stderr$engine.cpp: In member function 'void Engine::on_new(const mebench::Order&, mebench::OutSink&)':
engine.cpp:58:24: error: no matching function for call to 'mebench::OutSink::emit(uint64_t&, uint64_t&, int32_t&, uint32_t&)'
   58 |       out.emit(o.seq, r.o.client_order_id, r.o.px, take);
      |       ~~~~~~~~^~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
In file included from engine.cpp:3:
include/mebench/out.h:56:16: note: candidate: 'virtual void mebench::OutSink::emit(const mebench::OutEvent&)'
   56 |   virtual void emit(const OutEvent&) noexcept = 0;
      |                ^~~~
include/mebench/out.h:56:16: note:   candidate expects 1 argument, 4 provided$stderr$));

INSERT INTO submissions (participant_id, source_hash, state, created_at, updated_at, verify_detail)
SELECT 1, 'd1verged01', 'verify_failed', now() - interval '200 minutes', now() - interval '199 minutes', v FROM divergence;

INSERT INTO submissions (participant_id, source_hash, state, created_at, updated_at, verify_detail)
VALUES (1, 't1meout001', 'verify_timeout', now() - interval '170 minutes', now() - interval '169 minutes',
  jsonb_build_object('outcome','timeout','events_processed',213447,'total_events',300000,
    'message','No output for 10 seconds; stopped at the wall-clock limit.'));

INSERT INTO submissions (participant_id, source_hash, state, created_at, updated_at, verify_detail)
SELECT 1, 'he1d000001', 'pending_benchmark', now() - interval '140 minutes', now() - interval '139 minutes',
  jsonb_build_object('progress', v) FROM passed_progress;

INSERT INTO submissions (participant_id, source_hash, state, created_at, updated_at,
  verify_detail, p50_ns, p95_ns, p99_ns, probe_cost_ns, run_p50s_ns, discard_count, percentiles, timeline)
SELECT 1, 'd0ne000001', 'done', now() - interval '110 minutes', now() - interval '109 minutes',
  jsonb_build_object('progress', pp.v), 498.3, 820.0, 2510.5, 10.0,
  ARRAY[496.1,498.3,500.2,497.4,499.9,498.3,501.0,497.0,502.2]::double precision[], 0, pc.v, tl.v
FROM passed_progress pp, pctl pc, tline tl;

INSERT INTO submissions (participant_id, source_hash, state, created_at, updated_at,
  verify_detail, p50_ns, p95_ns, p99_ns, probe_cost_ns, run_p50s_ns, discard_count, percentiles, timeline)
SELECT 1, 'd0ne000002', 'done', now() - interval '70 minutes', now() - interval '69 minutes',
  jsonb_build_object('progress', pp.v), 411.2, 702.1, 2104.0, 10.0,
  ARRAY[409.0,411.2,412.8,410.1,413.9,411.2,410.6,412.0,414.2]::double precision[], 1, pc.v, tl.v
FROM passed_progress pp, pctl pc, tline tl;

INSERT INTO submissions (participant_id, source_hash, state, created_at, updated_at, run_p50s_ns)
VALUES (1, 'b3nch00001', 'benchmarking', now() - interval '30 minutes', now() - interval '29 minutes',
  ARRAY[352.1,349.8,351.0,350.2,352.9,350.6]::double precision[]);

INSERT INTO submissions (participant_id, source_hash, state, created_at, updated_at,
  verify_detail, p50_ns, p95_ns, p99_ns, probe_cost_ns, run_p50s_ns, discard_count, percentiles, timeline)
SELECT 1, 'd0ne000003', 'done', now() - interval '12 minutes', now() - interval '11 minutes',
  jsonb_build_object('progress', pp.v), 360.6, 612.4, 1884.2, 10.0,
  ARRAY[358.2,360.6,359.1,362.0,360.6,361.4,359.8,360.6,363.1]::double precision[], 0, pc.v, tl.v
FROM passed_progress pp, pctl pc, tline tl;

-- ── other people, so the leaderboard has a field ────────────────────────

INSERT INTO submissions (participant_id, source_hash, state, created_at, updated_at,
  p50_ns, p95_ns, p99_ns, probe_cost_ns, run_p50s_ns, discard_count, percentiles, timeline)
SELECT p.id,
       'seed' || lpad(p.id::text, 6, '0'),
       'done',
       now() - (p.id * 7 || ' minutes')::interval,
       now() - (p.id * 7 - 1 || ' minutes')::interval,
       s.p50, s.p50 * 1.7, s.p50 * 2.6, 10.0,
       ARRAY[s.p50 - 3, s.p50, s.p50 + 4]::double precision[], 0, pc.v, tl.v
FROM participants p
CROSS JOIN LATERAL (SELECT 341 + round(power(p.id, 1.9) * 4)::double precision AS p50) s,
     pctl pc, tline tl
WHERE p.handle IN ('r.venkatesh','m.oduya','k.lindqvist','t.becker','s.roychoudhury',
                   'j.park','n.almeida','p.ibarra','l.novak');

-- last_bench_at is recorded but gates nothing; Submit is blocked only while a
-- benchmark of yours is still queued, which submission 7 is not (it is held on
-- the bench node being down, which is a different thing).
INSERT INTO bench_slots (participant_id, last_bench_at)
VALUES (1, now() - interval '6 minutes')
ON CONFLICT (participant_id) DO UPDATE SET last_bench_at = EXCLUDED.last_bench_at;

COMMIT;

SELECT state, count(*) FROM submissions GROUP BY state ORDER BY 1;
