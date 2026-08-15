//! The agent role (PLAN-measurement-redesign §7).
//!
//! One agent per box. It polls Postgres directly — same claim-and-commit as
//! ever: the claim IS the state transition, so crash recovery stays a single
//! janitor statement per stage. The per-box queue is a mailbox of depth <= 1;
//! the API enforces one evaluation at a time per participant.
//!
//! The pipeline (one claimed job end to end):
//!
//!   Run    = compile -> visible tests -> Phase I          (feedback only)
//!   Submit = compile -> visible tests -> hidden verify
//!            -> Phase I -> Phase II ladder -> done        (leaderboard)
//!
//! One scoring chain everywhere: p95, then p50, then p99. Deadlines are
//! gates, never scores. Streams and their baked solutions come from the bake
//! cache — the generator and the reference run at bake time, not judge time
//! (on the real fleet they are baked into the AMI; here the agent bakes
//! lazily on first use, which is the dev stand-in).

use anyhow::{Context, Result};
use common::{harness_exit, SubState};
use sqlx::PgPool;
use std::path::PathBuf;
use std::time::Duration;

use crate::sandbox::{self, RunOpts, Sandbox};
use crate::Config;

/// Every phase stream is cancel_heavy — the ranked profile. Depth per level
/// comes from the level table's live_target.
const PROFILE: &str = "cancel_heavy";

/// The hidden verify runs on this prefix of the Phase I stream — same baked
/// bytes, truncated by the harness (`--events`), never regenerated. Small on
/// purpose: it catches essentially every bug a longer stream catches and the
/// verify lane must feel fast.
const VERIFY_PREFIX_EVENTS: u64 = 50_000;
const VERIFY_WALL_TIME_S: f64 = 45.0;

/// Wall-time ceiling handed to isolate for a single gated bench invocation.
/// Generous by design: the DEADLINE inside the harness is the real gate, and
/// this only bounds a pathological run the SIGALRM backstop somehow missed.
const BENCH_BOX_WALL_S: f64 = 600.0;

// ---------------------------------------------------------------- config

#[derive(serde::Deserialize, Clone)]
pub struct Phase1Cfg {
    pub events: u64,
    #[serde(default)]
    pub live_target: u64,
    pub deadline_ms: f64,
    #[serde(default = "default_runs")]
    pub runs: u32,
}
fn default_runs() -> u32 {
    3
}

#[derive(serde::Deserialize, Clone)]
pub struct LevelCfg {
    pub level: u32,
    pub events: u64,
    #[serde(default)]
    pub live_target: u64,
    pub deadline_ms: f64,
}

/// Workload numbers live in `settings`, not in code: M5's calibration writes
/// the real level table there, and an operator can adjust a deadline without
/// a deploy. Read per job — it is one row, and it means an edit takes effect
/// on the next evaluation.
async fn phase1_cfg(db: &PgPool) -> Result<Phase1Cfg> {
    let (v,): (serde_json::Value,) =
        sqlx::query_as("SELECT value FROM settings WHERE key = 'phase1'")
            .fetch_one(db)
            .await
            .context("settings.phase1 missing")?;
    Ok(serde_json::from_value(v).context("settings.phase1 malformed")?)
}

async fn level_table(db: &PgPool) -> Result<Vec<LevelCfg>> {
    let (v,): (serde_json::Value,) =
        sqlx::query_as("SELECT value FROM settings WHERE key = 'level_table'")
            .fetch_one(db)
            .await
            .context("settings.level_table missing")?;
    let mut t: Vec<LevelCfg> = serde_json::from_value(v).context("settings.level_table malformed")?;
    t.sort_by_key(|l| l.level);
    Ok(t)
}

// ---------------------------------------------------------------- agent role

pub async fn run_agent(db: PgPool, cfg: Config) -> Result<()> {
    let sandbox = Sandbox::new();
    // A killed predecessor never ran its BoxGuard, and a stale isolate box
    // wedges every later init for this box id. A fresh agent owns its box id
    // outright, so an unconditional cleanup at startup is always correct —
    // the companion of the startup reclaim in main.rs.
    sandbox.cleanup_blocking(cfg.box_id);
    loop {
        // Run jobs first, always: Run turnaround IS the iteration loop, and a
        // Submit can wait the extra seconds.
        if claim_run_job(&db, &cfg, &sandbox).await? {
            continue;
        }
        if claim_submission(&db, &cfg, &sandbox).await? {
            continue;
        }
        tokio::time::sleep(Duration::from_millis(400)).await;
    }
}

async fn claim_run_job(db: &PgPool, cfg: &Config, sandbox: &Sandbox) -> Result<bool> {
    // The participant binding IS the routing: a bound agent's claim matches
    // only its own participant's rows, so nothing of anyone else's can ever
    // run here. NULL binding (dev) matches everything.
    let row: Option<(i64, String)> = sqlx::query_as(
        "UPDATE run_jobs SET state = 'running', claimed_by = $1, updated_at = now() \
         WHERE id = (SELECT id FROM run_jobs WHERE state = 'received' \
                     AND ($2::int IS NULL OR participant_id = $2) \
                     ORDER BY id LIMIT 1 FOR UPDATE SKIP LOCKED) \
         RETURNING id, source_hash",
    )
    .bind(&cfg.worker_id)
    .bind(cfg.participant_id)
    .fetch_optional(db)
    .await?;

    let Some((id, hash)) = row else { return Ok(false) };
    tracing::info!("run job {id} ({hash})");
    if let Ok(mut j) = cfg.current_job.lock() { *j = format!("run #{id}"); }

    let result = match execute_run(db, cfg, sandbox, &hash).await {
        Ok(v) => v,
        Err(e) => serde_json::json!({ "error": e.to_string() }),
    };
    sqlx::query("UPDATE run_jobs SET state = 'done', result = $2, updated_at = now() WHERE id = $1")
        .bind(id)
        .bind(result)
        .execute(db)
        .await?;
    if let Ok(mut j) = cfg.current_job.lock() { *j = "idle".into(); }
    Ok(true)
}

/// Run = Phase 0's visible half + Phase I, via EXACTLY the code path Submit
/// uses. The lanes must not drift: "Run said 1.9s, Submit said fail" is a bug
/// class this sharing removes by construction.
async fn execute_run(
    db: &PgPool,
    cfg: &Config,
    sandbox: &Sandbox,
    hash: &str,
) -> Result<serde_json::Value> {
    let source = cfg.storage.get_source(hash).await?;
    let b = sandbox.init(cfg.box_id).await?;
    let _guard = BoxGuard { sandbox, id: cfg.box_id };
    sandbox::place(&b, "engine.cpp", &source).await?;

    // One compile covers both artifacts: the tests binary embeds engine.cpp,
    // and phase 1 needs the .so the harness dlopens.
    let compile = compile_in_box(cfg, sandbox, &b, "engine.cpp", "engine").await?;
    if compile.exit_code != 0 {
        return Ok(serde_json::json!({
            "compiled": false,
            "stderr": first_lines(&compile.stderr, 100),
        }));
    }
    let tests = run_visible_tests(cfg, sandbox, &b).await?;

    let p1cfg = phase1_cfg(db).await?;
    let phase1 = run_phase1(cfg, sandbox, &b, &p1cfg).await?;

    Ok(serde_json::json!({ "compiled": true, "tests": tests, "phase1": phase1 }))
}

async fn claim_submission(db: &PgPool, cfg: &Config, sandbox: &Sandbox) -> Result<bool> {
    let row: Option<(i64, i32, String)> = sqlx::query_as(
        "UPDATE submissions SET state = 'compiling', claimed_by = $1, updated_at = now() \
         WHERE id = (SELECT id FROM submissions WHERE state = 'received' \
                     AND ($2::int IS NULL OR participant_id = $2) \
                     ORDER BY id LIMIT 1 FOR UPDATE SKIP LOCKED) \
         RETURNING id, participant_id, source_hash",
    )
    .bind(&cfg.worker_id)
    .bind(cfg.participant_id)
    .fetch_optional(db)
    .await?;

    let Some((id, participant_id, hash)) = row else { return Ok(false) };
    let _ = participant_id;
    tracing::info!("submission {id} ({hash})");
    if let Ok(mut j) = cfg.current_job.lock() { *j = format!("submission #{id}"); }

    match process_submission(db, cfg, sandbox, id, &hash).await {
        Ok(()) => {}
        Err(e) => {
            tracing::error!("submission {id} errored: {e}");
            set_state(db, id, SubState::Error).await?;
            log_event(db, Some(id), "worker_error", serde_json::json!({ "error": e.to_string() }))
                .await?;
        }
    }
    if let Ok(mut j) = cfg.current_job.lock() { *j = "idle".into(); }
    Ok(true)
}

/// The whole gauntlet, one claimed job: compile -> tests -> hidden verify ->
/// Phase I -> Phase II -> done. Every phase writes its blob as it completes,
/// so the student page can follow the climb live on its 2s poll.
async fn process_submission(
    db: &PgPool,
    cfg: &Config,
    sandbox: &Sandbox,
    id: i64,
    hash: &str,
) -> Result<()> {
    let source = cfg.storage.get_source(hash).await?;
    let b = sandbox.init(cfg.box_id).await?;
    let _guard = BoxGuard { sandbox, id: cfg.box_id };
    sandbox::place(&b, "engine.cpp", &source).await?;

    // ---- compile
    let compile = compile_in_box(cfg, sandbox, &b, "engine.cpp", "engine").await?;
    if compile.exit_code != 0 {
        let blob = serde_json::json!({ "compile": { "stderr": first_lines(&compile.stderr, 100) } });
        finish_phase0(db, id, SubState::CompileFailed, &blob).await?;
        return Ok(());
    }
    if let Ok(so) = sandbox::read_out(&b, "engine.so").await {
        cfg.storage.put_binary(hash, so).await?;
    }

    // ---- phase 0a: the visible tests. All of them.
    set_state(db, id, SubState::Testing).await?;
    let tests = run_visible_tests(cfg, sandbox, &b).await?;
    let all_passed = tests.get("passed").and_then(|v| v.as_u64())
        == tests.get("total").and_then(|v| v.as_u64())
        && tests.get("total").and_then(|v| v.as_u64()).unwrap_or(0) > 0;
    if !all_passed {
        let blob = serde_json::json!({ "tests": tests });
        finish_phase0(db, id, SubState::TestsFailed, &blob).await?;
        return Ok(());
    }

    // ---- phase 0b: the hidden differential verify, on a fixed 50k prefix of
    // the Phase I stream. Same baked bytes Phase I times, truncated by the
    // harness; the stream crosses as an inherited descriptor, never a path.
    set_state(db, id, SubState::Verifying).await?;
    let p1cfg = phase1_cfg(db).await?;
    let baked = ensure_baked(cfg, cfg.seed1, p1cfg.events, p1cfg.live_target).await?;
    stage_harness(cfg, &b).await?;

    let verify = sandbox
        .run_with_stream(
            &b,
            &RunOpts::for_verify(VERIFY_WALL_TIME_S + 10.0),
            &[
                "./harness",
                "verify",
                "--stream-fd",
                &sandbox::STREAM_FD.to_string(),
                "--events",
                &VERIFY_PREFIX_EVENTS.to_string(),
                "--engine",
                "./engine.so",
                "--wall-time",
                &VERIFY_WALL_TIME_S.to_string(),
                "--json",
            ],
            Some(baked.open_stream()?),
        )
        .await?;

    let mut verify_detail: serde_json::Value =
        serde_json::from_str(verify.stdout.trim()).unwrap_or_else(|_| {
            serde_json::json!({ "outcome": "error", "stderr": first_lines(&verify.stderr, 40) })
        });
    let known = [
        harness_exit::PASSED,
        harness_exit::FAILED,
        harness_exit::USAGE,
        harness_exit::TIMEOUT,
    ];
    if verify.crashed() || verify.unexpected_exit(&known) {
        verify_detail = serde_json::json!({
            "outcome": "crashed",
            "status": verify.status,
            "hint": "the engine died before finishing: signal or runtime error, not a diff",
            "stderr": first_lines(&verify.stderr, 40),
        });
    }
    let blob = serde_json::json!({ "tests": tests, "verify": verify_detail });

    if verify.timed_out() || verify.exit_code == harness_exit::TIMEOUT {
        finish_phase0(db, id, SubState::VerifyTimeout, &blob).await?;
        return Ok(());
    }
    if verify.exit_code != harness_exit::PASSED {
        finish_phase0(db, id, SubState::VerifyFailed, &blob).await?;
        return Ok(());
    }
    sqlx::query("UPDATE submissions SET phase0 = $2, updated_at = now() WHERE id = $1")
        .bind(id)
        .bind(&blob)
        .execute(db)
        .await?;

    // ---- phase I: the bulk run. The gate into the ladder.
    set_state(db, id, SubState::Phase1).await?;
    let phase1 = run_phase1(cfg, sandbox, &b, &p1cfg).await?;
    let p1_ok = phase1.get("outcome").and_then(|v| v.as_str()) == Some("ok");
    sqlx::query("UPDATE submissions SET phase1 = $2, updated_at = now() WHERE id = $1")
        .bind(id)
        .bind(&phase1)
        .execute(db)
        .await?;
    if !p1_ok {
        set_state(db, id, SubState::Phase1Failed).await?;
        return Ok(());
    }

    // ---- phase II: the ladder. One attempt per level, in order; the climb
    // ends at the first miss — and a stopped climb is a RESULT, not an error.
    set_state(db, id, SubState::Phase2).await?;
    let table = level_table(db).await?;
    let mut levels = Vec::new();
    let mut max_level: i32 = 0;
    let mut top: Option<(f64, f64, f64)> = None;

    for lv in &table {
        let entry = run_level(cfg, sandbox, &b, lv).await?;
        let ok = entry.get("outcome").and_then(|v| v.as_str()) == Some("ok");
        if ok {
            max_level = lv.level as i32;
            top = Some((
                entry.get("p50_ns").and_then(|v| v.as_f64()).unwrap_or(0.0),
                entry.get("p95_ns").and_then(|v| v.as_f64()).unwrap_or(0.0),
                entry.get("p99_ns").and_then(|v| v.as_f64()).unwrap_or(0.0),
            ));
        }
        levels.push(entry);
        // Progress is visible mid-climb, and updated_at doubles as the
        // heartbeat that keeps the janitor's hands off a live ladder.
        sqlx::query(
            "UPDATE submissions SET phase2 = $2, updated_at = now() WHERE id = $1",
        )
        .bind(id)
        .bind(serde_json::json!({ "levels": levels }))
        .execute(db)
        .await?;
        if !ok {
            break;
        }
    }

    let (p50, p95, p99) = top.unwrap_or((0.0, 0.0, 0.0));
    sqlx::query(
        "UPDATE submissions SET state = 'done', max_level = $2, top_p50_ns = $3, \
         top_p95_ns = $4, top_p99_ns = $5, updated_at = now() WHERE id = $1",
    )
    .bind(id)
    .bind(max_level)
    .bind(if max_level > 0 { Some(p50) } else { None })
    .bind(if max_level > 0 { Some(p95) } else { None })
    .bind(if max_level > 0 { Some(p99) } else { None })
    .execute(db)
    .await?;
    Ok(())
}

async fn finish_phase0(db: &PgPool, id: i64, state: SubState, blob: &serde_json::Value) -> Result<()> {
    sqlx::query("UPDATE submissions SET state = $2, phase0 = $3, updated_at = now() WHERE id = $1")
        .bind(id)
        .bind(state)
        .bind(blob)
        .execute(db)
        .await?;
    Ok(())
}

// ---------------------------------------------------------------- phases

async fn run_visible_tests(
    cfg: &Config,
    sandbox: &Sandbox,
    b: &sandbox::Box_,
) -> Result<serde_json::Value> {
    let compile = compile_in_box(cfg, sandbox, b, "engine.cpp", "tests").await?;
    if compile.exit_code != 0 {
        // engine.so compiled but the tests harness did not — a worker-side
        // problem worth surfacing distinctly, not a participant failure.
        anyhow::bail!("tests harness failed to compile: {}", first_lines(&compile.stderr, 20));
    }
    let out = sandbox.run(b, &RunOpts::for_verify(30.0), &["./run_tests", "--json"]).await?;
    Ok(serde_json::from_str(out.stdout.trim())
        .unwrap_or_else(|_| serde_json::json!({ "error": first_lines(&out.stderr, 20) })))
}

/// One gated bench invocation: deadline + baked solution, stream on the
/// inherited descriptor, chain out. Shared by Phase I (runs=N) and each
/// ladder level (runs=1).
async fn gated_bench(
    cfg: &Config,
    sandbox: &Sandbox,
    b: &sandbox::Box_,
    baked: &Baked,
    runs: u32,
    deadline_ms: f64,
    sol_name: &str,
) -> Result<(i32, serde_json::Value)> {
    let sol_bytes = tokio::fs::read(&baked.solution)
        .await
        .with_context(|| format!("reading solution {}", baked.solution.display()))?;
    sandbox::place(b, sol_name, &sol_bytes).await?;

    // Measured runs pin to the isolated core (§7); everything else on the box
    // steers clear of it, so nothing shares the core with a timed event.
    let mut opts = RunOpts::for_bench(BENCH_BOX_WALL_S);
    opts.cpus = cfg.bench_cpus.clone();

    let out = sandbox
        .run_with_stream(
            b,
            &opts,
            &[
                "./harness",
                "bench",
                "--stream-fd",
                &sandbox::STREAM_FD.to_string(),
                "--engine",
                "./engine.so",
                "--runs",
                &runs.to_string(),
                "--deadline-ms",
                &deadline_ms.to_string(),
                "--solution",
                sol_name,
                "--json",
            ],
            Some(baked.open_stream()?),
        )
        .await?;

    let parsed: serde_json::Value =
        serde_json::from_str(out.stdout.trim()).unwrap_or_else(|_| serde_json::json!({}));
    Ok((out.exit_code, parsed))
}

/// Map a gated bench's exit + JSON to the stable per-phase schema. The chain
/// (p95, p50, p99) is present for EVERY outcome that measured anything — a
/// deadline-cut run's chain covers what it processed, which is exactly what
/// the rejudge ranks non-finishers on.
fn bench_outcome(exit_code: i32, r: &serde_json::Value, crashed: bool) -> serde_json::Value {
    let outcome = if crashed {
        "crashed"
    } else {
        match exit_code {
            harness_exit::PASSED => "ok",
            harness_exit::DEADLINE => "deadline_missed",
            harness_exit::TIMEOUT => "timeout",
            harness_exit::UNHEALTHY => "node_unhealthy",
            _ => r.get("outcome").and_then(|v| v.as_str()).unwrap_or("failed"),
        }
    };
    let f = |k: &str| r.get(k).and_then(|v| v.as_f64()).unwrap_or(0.0);
    serde_json::json!({
        "outcome": outcome,
        "p50_ns": f("p50_ns"),
        "p95_ns": f("p95_ns"),
        "p99_ns": f("p99_ns"),
        "events_processed": r.get("events_processed").and_then(|v| v.as_u64()).unwrap_or(0),
        "notes": r.get("notes").cloned().unwrap_or(serde_json::Value::Null),
    })
}

async fn run_phase1(
    cfg: &Config,
    sandbox: &Sandbox,
    b: &sandbox::Box_,
    p1: &Phase1Cfg,
) -> Result<serde_json::Value> {
    let baked = ensure_baked(cfg, cfg.seed1, p1.events, p1.live_target).await?;
    stage_harness(cfg, b).await?;
    let (exit, r) = gated_bench(cfg, sandbox, b, &baked, p1.runs, p1.deadline_ms, "phase1.sol").await?;

    let mut out = bench_outcome(exit, &r, false);
    let o = out.as_object_mut().unwrap();
    o.insert("events".into(), serde_json::json!(p1.events));
    o.insert("deadline_ms".into(), serde_json::json!(p1.deadline_ms));
    o.insert("runs".into(), r.get("runs").cloned().unwrap_or(serde_json::Value::Null));
    o.insert(
        "percentiles".into(),
        r.get("percentiles").cloned().unwrap_or(serde_json::Value::Null),
    );
    Ok(out)
}

async fn run_level(
    cfg: &Config,
    sandbox: &Sandbox,
    b: &sandbox::Box_,
    lv: &LevelCfg,
) -> Result<serde_json::Value> {
    let baked = ensure_baked(cfg, cfg.seed2, lv.events, lv.live_target).await?;
    let (exit, r) = gated_bench(cfg, sandbox, b, &baked, 1, lv.deadline_ms, "level.sol").await?;

    let crashed = !matches!(
        exit,
        harness_exit::PASSED
            | harness_exit::FAILED
            | harness_exit::TIMEOUT
            | harness_exit::UNHEALTHY
            | harness_exit::DEADLINE
    );
    let mut out = bench_outcome(exit, &r, crashed);
    let o = out.as_object_mut().unwrap();
    o.insert("level".into(), serde_json::json!(lv.level));
    o.insert("events".into(), serde_json::json!(lv.events));
    o.insert("deadline_ms".into(), serde_json::json!(lv.deadline_ms));
    o.insert(
        "wall_s".into(),
        r.get("runs")
            .and_then(|v| v.as_array())
            .and_then(|a| a.last())
            .and_then(|run| run.get("wall_s"))
            .cloned()
            .unwrap_or(serde_json::Value::Null),
    );
    o.insert(
        "checked_events".into(),
        r.get("runs")
            .and_then(|v| v.as_array())
            .and_then(|a| a.last())
            .and_then(|run| run.get("checked_events"))
            .cloned()
            .unwrap_or(serde_json::Value::Null),
    );
    Ok(out)
}

// ---------------------------------------------------------------- golden role

/// Phase III (PLAN-measurement-redesign §3): the golden box, idle all
/// contest, claims rejudge jobs after the freeze and re-measures every
/// finalist's LAST submitted code on the sealed seed — one heavy constrained
/// run, repeated, under maximum isolation. The seed arrives as MEBENCH_SEED3
/// only when the operator starts this role: sealed means it exists nowhere
/// executable until the rejudge begins, and the streams are baked here, then.
pub async fn run_golden(db: PgPool, cfg: Config) -> Result<()> {
    let seed3 = cfg.seed3.context("MEBENCH_SEED3 must be set for the golden role")?;
    let sandbox = Sandbox::new();
    sandbox.cleanup_blocking(cfg.box_id);
    tracing::info!("golden box up; rejudge seed loaded, streams bake on first job");
    loop {
        let row: Option<(i64, i32, i64, String)> = sqlx::query_as(
            "UPDATE rejudge_jobs SET state = 'running', claimed_by = $1, updated_at = now() \
             WHERE id = (SELECT id FROM rejudge_jobs WHERE state = 'received' \
                         ORDER BY id LIMIT 1 FOR UPDATE SKIP LOCKED) \
             RETURNING id, participant_id, submission_id, source_hash",
        )
        .bind(&cfg.worker_id)
        .fetch_optional(&db)
        .await?;

        let Some((job_id, participant_id, submission_id, hash)) = row else {
            tokio::time::sleep(Duration::from_millis(1000)).await;
            continue;
        };
        tracing::info!("rejudge job {job_id}: participant {participant_id}, submission {submission_id}");
        // Report which contestant is being measured, so the dashboard's Phase
        // III panel shows the golden box's live progress.
        if let Ok(mut j) = cfg.current_job.lock() {
            *j = format!("rejudge · participant {participant_id} · submission {submission_id}");
        }
        match rejudge_one(&db, &cfg, &sandbox, seed3, participant_id, submission_id, &hash).await {
            Ok(()) => {
                sqlx::query("UPDATE rejudge_jobs SET state = 'done', updated_at = now() WHERE id = $1")
                    .bind(job_id)
                    .execute(&db)
                    .await?;
            }
            Err(e) => {
                tracing::error!("rejudge job {job_id} errored: {e}");
                sqlx::query("UPDATE rejudge_jobs SET state = 'error', updated_at = now() WHERE id = $1")
                    .bind(job_id)
                    .execute(&db)
                    .await?;
                log_event(&db, Some(submission_id), "rejudge_error",
                          serde_json::json!({ "error": e.to_string() })).await?;
            }
        }
        if let Ok(mut j) = cfg.current_job.lock() { *j = "idle".into(); }
    }
}

#[derive(serde::Deserialize, Clone)]
struct Phase3Cfg {
    events: u64,
    #[serde(default)]
    live_target: u64,
    deadline_ms: f64,
    #[serde(default = "default_runs")]
    runs: u32,
}

async fn rejudge_one(
    db: &PgPool,
    cfg: &Config,
    sandbox: &Sandbox,
    seed3: u64,
    participant_id: i32,
    submission_id: i64,
    hash: &str,
) -> Result<()> {
    let (p3,): (serde_json::Value,) =
        sqlx::query_as("SELECT value FROM settings WHERE key = 'phase3'")
            .fetch_one(db)
            .await
            .context("settings.phase3 missing")?;
    let p3: Phase3Cfg = serde_json::from_value(p3).context("settings.phase3 malformed")?;

    let source = cfg.storage.get_source(hash).await?;
    let b = sandbox.init(cfg.box_id).await?;
    let _guard = BoxGuard { sandbox, id: cfg.box_id };
    sandbox::place(&b, "engine.cpp", &source).await?;

    // "Latest regardless of how it fared live" is blunt on purpose: a final
    // submit that does not compile scores as failed, and the spec says so.
    let compile = compile_in_box(cfg, sandbox, &b, "engine.cpp", "engine").await?;
    let detail;
    let (finished, wall_s, events_processed, chain);
    if compile.exit_code != 0 {
        finished = false;
        wall_s = 0.0;
        events_processed = 0i64;
        chain = (0.0, 0.0, 0.0);
        detail = serde_json::json!({ "outcome": "compile_failed",
                                     "stderr": first_lines(&compile.stderr, 40) });
    } else {
        stage_harness(cfg, &b).await?;
        let baked = ensure_baked(cfg, seed3, p3.events, p3.live_target).await?;
        let (exit, r) = gated_bench(cfg, sandbox, &b, &baked, p3.runs, p3.deadline_ms, "phase3.sol").await?;
        let out = bench_outcome(exit, &r, false);
        let f = |k: &str| out.get(k).and_then(|v| v.as_f64()).unwrap_or(0.0);
        finished = out.get("outcome").and_then(|v| v.as_str()) == Some("ok");
        chain = (f("p50_ns"), f("p95_ns"), f("p99_ns"));
        events_processed =
            out.get("events_processed").and_then(|v| v.as_u64()).unwrap_or(0) as i64;
        wall_s = r
            .get("runs")
            .and_then(|v| v.as_array())
            .map(|a| {
                let kept: Vec<f64> = a
                    .iter()
                    .filter(|run| run.get("discarded").and_then(|d| d.as_i64()) != Some(1))
                    .filter_map(|run| run.get("wall_s").and_then(|w| w.as_f64()))
                    .collect();
                if kept.is_empty() { 0.0 } else { kept.iter().sum::<f64>() / kept.len() as f64 }
            })
            .unwrap_or(0.0);
        detail = serde_json::json!({
            "outcome": out.get("outcome"),
            "notes": out.get("notes"),
            "runs": r.get("runs"),
            "percentiles": r.get("percentiles"),
        });
    }

    sqlx::query(
        "INSERT INTO rejudge_results (participant_id, submission_id, seed, finished, wall_s, \
         events_processed, p50_ns, p95_ns, p99_ns, detail) \
         VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10)",
    )
    .bind(participant_id)
    .bind(submission_id)
    .bind(seed3 as i64)
    .bind(finished)
    .bind(wall_s)
    .bind(events_processed)
    .bind(chain.0)
    .bind(chain.1)
    .bind(chain.2)
    .bind(&detail)
    .execute(db)
    .await?;
    Ok(())
}

// ---------------------------------------------------------------- bake cache

/// A baked stream + its solution. On the real fleet these are AMI contents;
/// in dev the agent bakes lazily into MEBENCH_BAKE on first use. Filenames
/// carry every generation parameter, so an operator editing the level table
/// mid-event just causes a fresh bake for the new shape.
pub struct Baked {
    pub stream: PathBuf,
    pub solution: PathBuf,
}

impl Baked {
    /// The stream crosses into the box as an inherited descriptor: no path
    /// resolves inside, and no seed appears in argv.
    fn open_stream(&self) -> Result<std::fs::File> {
        Ok(std::fs::File::open(&self.stream)?)
    }
}

async fn ensure_baked(cfg: &Config, seed: u64, events: u64, live_target: u64) -> Result<Baked> {
    tokio::fs::create_dir_all(&cfg.bake_dir).await?;
    let base = format!("{PROFILE}-{seed}-{events}-{live_target}");
    let stream = PathBuf::from(&cfg.bake_dir).join(format!("{base}.bin"));
    let solution = PathBuf::from(&cfg.bake_dir).join(format!("{base}.sol"));

    if !stream.exists() {
        tracing::info!("baking stream {base}");
        let mut args = vec![
            "--seed".to_string(),
            seed.to_string(),
            "--profile".to_string(),
            PROFILE.to_string(),
            "--events".to_string(),
            events.to_string(),
            "-o".to_string(),
            stream.to_string_lossy().into_owned(),
        ];
        if live_target > 0 {
            args.extend(["--live-target".to_string(), live_target.to_string()]);
        }
        let out = tokio::process::Command::new(&cfg.gen_bin).args(&args).output().await?;
        if !out.status.success() {
            anyhow::bail!("gen failed: {}", String::from_utf8_lossy(&out.stderr));
        }
        use std::os::unix::fs::PermissionsExt;
        std::fs::set_permissions(&stream, std::fs::Permissions::from_mode(0o600))?;
    }
    if !solution.exists() {
        tracing::info!("baking solution {base}");
        let out = tokio::process::Command::new(&cfg.harness_bin)
            .args([
                "solve",
                "--stream",
                &stream.to_string_lossy(),
                "-o",
                &solution.to_string_lossy(),
            ])
            .output()
            .await?;
        if !out.status.success() {
            anyhow::bail!("solve failed: {}", String::from_utf8_lossy(&out.stderr));
        }
    }
    Ok(Baked { stream, solution })
}

// ---------------------------------------------------------------- helpers

struct BoxGuard<'a> {
    sandbox: &'a Sandbox,
    id: u32,
}

impl Drop for BoxGuard<'_> {
    fn drop(&mut self) {
        // A leaked box wedges that box id for the rest of the event, so this
        // has to happen on every path out — including the error paths.
        self.sandbox.cleanup_blocking(self.id);
    }
}

async fn compile_in_box(
    cfg: &Config,
    sandbox: &Sandbox,
    b: &sandbox::Box_,
    src: &str,
    kind: &str,
) -> Result<sandbox::RunOutcome> {
    // Pinned compiler, published flags, explicit -march. Never -march=native:
    // every box is the same instance type by design, but a binary must mean
    // the same thing on every one of them.
    let mut argv: Vec<String> = vec![
        cfg.cxx.clone(),
        "-std=c++20".into(),
        "-O2".into(),
        format!("-march={}", cfg.march),
        "-fno-omit-frame-pointer".into(),
        "-fPIC".into(),
        "-Iinclude".into(),
    ];
    if kind == "tests" {
        argv.extend([
            "-I.".into(),
            "-o".into(),
            "run_tests".into(),
            src.into(),
            "tests/spec_tests.cpp".into(),
            "tests/run_tests_main.cpp".into(),
        ]);
    } else {
        argv.extend(["-shared".into(), "-o".into(), "engine.so".into(), src.into()]);
    }
    let refs: Vec<&str> = argv.iter().map(String::as_str).collect();

    // Headers go INTO the box rather than being bind-mounted from the host.
    stage_headers(cfg, b, kind).await?;

    let mut opts = RunOpts::for_compile();
    opts.env.push(("PATH".into(), "/usr/bin:/bin".into()));
    // Compiles go to the spare cores, never the isolated one: a cc1plus
    // sharing L3 is tolerable, one sharing the measurement core is not.
    opts.cpus = cfg.compile_cpus.clone();
    sandbox.run(b, &opts, &refs).await
}

/// Copy the harness into the box. It runs as ./harness inside the sandbox, so
/// it has to be there — the box sees almost nothing of the host filesystem.
async fn stage_harness(cfg: &Config, b: &sandbox::Box_) -> Result<()> {
    let bytes = tokio::fs::read(&cfg.harness_bin)
        .await
        .with_context(|| format!("reading harness from {}", cfg.harness_bin))?;
    let dest = b.root.join("harness");
    tokio::fs::write(&dest, bytes).await?;
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        tokio::fs::set_permissions(&dest, std::fs::Permissions::from_mode(0o755)).await?;
    }
    Ok(())
}

/// Copy the frozen headers into the box, plus the visible tests when needed.
async fn stage_headers(cfg: &Config, b: &sandbox::Box_, kind: &str) -> Result<()> {
    let inc = b.root.join("include/mebench");
    tokio::fs::create_dir_all(&inc).await?;
    for name in ["wire.h", "order.h", "out.h", "engine.h"] {
        let bytes = tokio::fs::read(std::path::Path::new(&cfg.include_dir).join("mebench").join(name))
            .await
            .with_context(|| format!("reading frozen header {name}"))?;
        tokio::fs::write(inc.join(name), bytes).await?;
    }
    if kind == "tests" {
        let tests = b.root.join("tests");
        tokio::fs::create_dir_all(&tests).await?;
        for name in ["spec_tests.h", "spec_tests.cpp", "run_tests_main.cpp"] {
            let bytes = tokio::fs::read(std::path::Path::new(&cfg.tests_dir).join(name))
                .await
                .with_context(|| format!("reading visible test file {name}"))?;
            tokio::fs::write(tests.join(name), bytes).await?;
        }
    }
    Ok(())
}

async fn set_state(db: &PgPool, id: i64, state: SubState) -> Result<()> {
    sqlx::query("UPDATE submissions SET state = $2, updated_at = now() WHERE id = $1")
        .bind(id)
        .bind(state)
        .execute(db)
        .await?;
    Ok(())
}

async fn log_event(
    db: &PgPool,
    submission_id: Option<i64>,
    kind: &str,
    detail: serde_json::Value,
) -> Result<()> {
    sqlx::query("INSERT INTO events_log (submission_id, kind, detail) VALUES ($1, $2, $3)")
        .bind(submission_id)
        .bind(kind)
        .bind(detail)
        .execute(db)
        .await?;
    Ok(())
}

fn first_lines(s: &str, n: usize) -> String {
    s.lines().take(n).collect::<Vec<_>>().join("\n")
}
