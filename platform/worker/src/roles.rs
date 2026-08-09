//! The two worker roles. Both poll Postgres directly — same VPC, no broker.
//!
//! Claim-and-commit: the claim commits immediately (no transaction held across
//! the job), the worker does the work, then commits the terminal state. The
//! claim IS the state transition, which is what makes crash recovery a single
//! janitor statement rather than a protocol.

use anyhow::{Context, Result};
use common::{harness_exit, SubState};
use sqlx::PgPool;
use std::time::Duration;

use crate::sandbox::{self, RunOpts, Sandbox};
use crate::Config;

/// The correctness lane's stream. Short on purpose: it catches essentially
/// every bug the 10M stream catches, in a fraction of the time, and lane
/// throughput is what makes iteration feel fast.
const VERIFY_EVENTS: u64 = 300_000;
const VERIFY_PROFILE: &str = "balanced";
const VERIFY_WALL_TIME_S: f64 = 45.0;

/// The ranked stream.
const BENCH_EVENTS: u64 = 10_000_000;
const BENCH_PROFILE: &str = "cancel_heavy";
const BENCH_RUNS: u32 = 9;

// ---------------------------------------------------------------- pool role

pub async fn run_pool(db: PgPool, cfg: Config) -> Result<()> {
    let sandbox = Sandbox::new();
    loop {
        // Run jobs come first, always. Run turnaround IS the iteration loop and
        // must feel instant; a Submit can wait three extra seconds.
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
    let row: Option<(i64, String)> = sqlx::query_as(
        "UPDATE run_jobs SET state = 'running', claimed_by = $1, updated_at = now() \
         WHERE id = (SELECT id FROM run_jobs WHERE state = 'received' \
                     ORDER BY id LIMIT 1 FOR UPDATE SKIP LOCKED) \
         RETURNING id, source_hash",
    )
    .bind(&cfg.worker_id)
    .fetch_optional(db)
    .await?;

    let Some((id, hash)) = row else { return Ok(false) };
    tracing::info!("run job {id} ({hash})");

    let result = match execute_run(cfg, sandbox, &hash).await {
        Ok(v) => v,
        Err(e) => serde_json::json!({ "error": e.to_string() }),
    };
    sqlx::query("UPDATE run_jobs SET state = 'done', result = $2, updated_at = now() WHERE id = $1")
        .bind(id)
        .bind(result)
        .execute(db)
        .await?;
    Ok(true)
}

/// Compile, then run the ~30 visible tests. These teach; they do not grade, so
/// nothing here touches the submissions table.
async fn execute_run(cfg: &Config, sandbox: &Sandbox, hash: &str) -> Result<serde_json::Value> {
    let source = cfg.storage.get_source(hash).await?;
    let b = sandbox.init(cfg.box_id).await?;
    let _guard = BoxGuard { sandbox, id: cfg.box_id };

    sandbox::place(&b, "engine.cpp", &source).await?;
    let compile = compile_in_box(cfg, sandbox, &b, "engine.cpp", "tests").await?;
    if compile.exit_code != 0 {
        return Ok(serde_json::json!({
            "compiled": false,
            "stderr": first_lines(&compile.stderr, 100),
        }));
    }

    let out = sandbox
        .run(
            &b,
            &RunOpts::for_verify(30.0),
            &["./run_tests", "--json"],
        )
        .await?;
    let parsed: serde_json::Value =
        serde_json::from_str(out.stdout.trim()).unwrap_or_else(|_| serde_json::json!({}));
    Ok(serde_json::json!({ "compiled": true, "tests": parsed }))
}

async fn claim_submission(db: &PgPool, cfg: &Config, sandbox: &Sandbox) -> Result<bool> {
    let row: Option<(i64, i32, String)> = sqlx::query_as(
        "UPDATE submissions SET state = 'compiling', claimed_by = $1, updated_at = now() \
         WHERE id = (SELECT id FROM submissions WHERE state = 'received' \
                     ORDER BY id LIMIT 1 FOR UPDATE SKIP LOCKED) \
         RETURNING id, participant_id, source_hash",
    )
    .bind(&cfg.worker_id)
    .fetch_optional(db)
    .await?;

    let Some((id, participant_id, hash)) = row else { return Ok(false) };
    tracing::info!("submission {id} ({hash})");

    match process_submission(db, cfg, sandbox, id, participant_id, &hash).await {
        Ok(()) => {}
        Err(e) => {
            tracing::error!("submission {id} errored: {e}");
            set_state(db, id, SubState::Error).await?;
            log_event(db, Some(id), "worker_error", serde_json::json!({ "error": e.to_string() }))
                .await?;
        }
    }
    Ok(true)
}

async fn process_submission(
    db: &PgPool,
    cfg: &Config,
    sandbox: &Sandbox,
    id: i64,
    participant_id: i32,
    hash: &str,
) -> Result<()> {
    let source = cfg.storage.get_source(hash).await?;
    let b = sandbox.init(cfg.box_id).await?;
    let _guard = BoxGuard { sandbox, id: cfg.box_id };
    sandbox::place(&b, "engine.cpp", &source).await?;

    // ---- compile
    let compile = compile_in_box(cfg, sandbox, &b, "engine.cpp", "engine").await?;
    if compile.exit_code != 0 {
        sqlx::query(
            "UPDATE submissions SET state = 'compile_failed', verify_detail = $2, \
             updated_at = now() WHERE id = $1",
        )
        .bind(id)
        .bind(serde_json::json!({ "stderr": first_lines(&compile.stderr, 100) }))
        .execute(db)
        .await?;
        return Ok(());
    }

    // The binary is cached on the source hash: participants resubmit unchanged
    // code more often than expected, especially when chasing a fresh bench slot.
    if let Ok(so) = sandbox::read_out(&b, "engine.so").await {
        cfg.storage.put_binary(hash, so).await?;
    }

    // ---- verify, on a seed this submission has never seen
    set_state(db, id, SubState::Verifying).await?;
    let seed: i64 = rand_seed();

    // The seed reaches the harness as a generated stream file passed by fd, not
    // as --seed on argv: the submission is dlopen'ed into the harness process
    // and can read /proc/self/cmdline.
    let stream = generate_stream(cfg, seed as u64, VERIFY_PROFILE, VERIFY_EVENTS).await?;
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
                "--engine",
                "./engine.so",
                "--wall-time",
                &VERIFY_WALL_TIME_S.to_string(),
                "--json",
            ],
            Some(stream.dup_fd()?),
        )
        .await?;

    let detail: serde_json::Value =
        serde_json::from_str(verify.stdout.trim()).unwrap_or_else(|_| {
            serde_json::json!({ "outcome": "error", "stderr": first_lines(&verify.stderr, 40) })
        });

    // A crashed engine is not a wrong answer either. Saying "segfault" beats
    // showing a diff that never got produced.
    let known_codes = [
        harness_exit::PASSED,
        harness_exit::FAILED,
        harness_exit::USAGE,
        harness_exit::TIMEOUT,
    ];
    let detail = if verify.crashed() || verify.unexpected_exit(&known_codes) {
        serde_json::json!({
            "outcome": "crashed",
            "status": verify.status,
            "hint": "the engine died before finishing: signal or runtime error, not a diff",
            "stderr": first_lines(&verify.stderr, 40),
        })
    } else {
        detail
    };

    // A timeout is a liveness failure, reported distinctly from a wrong answer:
    // they are different bugs and conflating them wastes participant time.
    let state = if verify.timed_out() || verify.exit_code == harness_exit::TIMEOUT {
        SubState::VerifyTimeout
    } else if verify.exit_code == harness_exit::PASSED {
        SubState::VerifyPassed
    } else {
        SubState::VerifyFailed
    };

    let digest = detail.get("digest").and_then(|d| d.as_str()).map(str::to_string);
    sqlx::query(
        "UPDATE submissions SET state = $2, verify_seed = $3, verify_detail = $4, \
         verify_digest = $5, updated_at = now() WHERE id = $1",
    )
    .bind(id)
    .bind(state)
    .bind(seed)
    .bind(&detail)
    .bind(digest)
    .execute(db)
    .await?;

    if state != SubState::VerifyPassed {
        return Ok(());
    }

    try_enqueue_bench(db, id, participant_id).await
}

/// The queue rule is applied here, at enqueue, in one transaction: at most one
/// pending bench job per participant. That alone makes the queue
/// self-throttling — worst-case depth is the number of participants, whatever
/// anyone does — which is why the fifteen-minute cooldown that used to sit
/// beside it was removed. It throttled people who were already blocked behind
/// their own job, and protected nothing the pending rule was not already
/// protecting.
async fn try_enqueue_bench(db: &PgPool, id: i64, participant_id: i32) -> Result<()> {
    let mut tx = db.begin().await?;

    let slot: Option<(Option<chrono::DateTime<chrono::Utc>>, Option<i64>)> = sqlx::query_as(
        "SELECT last_bench_at, pending_sub FROM bench_slots WHERE participant_id = $1 FOR UPDATE",
    )
    .bind(participant_id)
    .fetch_optional(&mut *tx)
    .await?;

    let eligible = !matches!(&slot, Some((_, Some(_))));

    if !eligible {
        // Say so, on the submission.
        //
        // /submit answers the rate question at request time, but the slot is
        // only TAKEN here, when verification passes. Between the two, an
        // earlier submission can pass and claim it — so a participant told
        // "will queue for the benchmark" can end up at verify_passed forever
        // with nothing explaining why. That is exactly the silently-dropped
        // job plan section 2 designs against, arriving by a different route.
        let reason = match &slot {
            Some((_, Some(pending))) => format!(
                "held: submission #{pending} is already queued for the benchmark. \
                 One pending benchmark job per participant."
            ),
            _ => "held: not eligible for the benchmark lane".to_string(),
        };
        sqlx::query(
            "UPDATE submissions SET verify_detail = \
             COALESCE(verify_detail, '{}'::jsonb) || jsonb_build_object('bench_held', $2::text), \
             updated_at = now() WHERE id = $1",
        )
        .bind(id)
        .bind(&reason)
        .execute(&mut *tx)
        .await?;
        tx.commit().await?;
        log_event(db, Some(id), "bench_enqueue_declined", serde_json::json!({ "reason": reason }))
            .await?;
        return Ok(());
    }

    // If no bench worker is healthy, park rather than error: correctness keeps
    // working and the queue is rejudged when the node returns.
    let (healthy,): (i64,) =
        sqlx::query_as("SELECT count(*) FROM workers WHERE role = 'bench' AND healthy = true")
            .fetch_one(&mut *tx)
            .await?;
    let target = if healthy > 0 { SubState::BenchQueued } else { SubState::PendingBenchmark };

    sqlx::query("UPDATE submissions SET state = $2, updated_at = now() WHERE id = $1")
        .bind(id)
        .bind(target)
        .execute(&mut *tx)
        .await?;

    // last_bench_at no longer gates anything — it is kept as a record of when a
    // participant last took the bench slot, which is worth having in the
    // events log's company when a queue question comes up on the day.
    sqlx::query(
        "INSERT INTO bench_slots (participant_id, last_bench_at, pending_sub) \
         VALUES ($1, now(), $2) ON CONFLICT (participant_id) \
         DO UPDATE SET last_bench_at = now(), pending_sub = $2",
    )
    .bind(participant_id)
    .bind(id)
    .execute(&mut *tx)
    .await?;

    tx.commit().await?;
    Ok(())
}

// ---------------------------------------------------------------- bench role

pub async fn run_bench(db: PgPool, cfg: Config) -> Result<()> {
    let sandbox = Sandbox::new();
    let mut last_spot_check = std::time::Instant::now();

    loop {
        // Steal-time discards requeue at the FRONT, so priority leads the sort.
        let row: Option<(i64, i32, String, Option<Vec<i64>>)> = sqlx::query_as(
            "UPDATE submissions SET state = 'benchmarking', claimed_by = $1, updated_at = now() \
             WHERE id = (SELECT id FROM submissions WHERE state = 'bench_queued' \
                         ORDER BY requeue_priority DESC, id LIMIT 1 FOR UPDATE SKIP LOCKED) \
             RETURNING id, participant_id, source_hash, bench_seed_set",
        )
        .bind(&cfg.worker_id)
        .fetch_optional(&db)
        .await?;

        if let Some((id, participant_id, hash, pinned)) = row {
            // A pinned seed means this is a rejudge: every participant's final
            // submission is measured on the SAME seed set, in one continuous
            // block, so the numbers that decide ranking are comparable by
            // construction rather than by assumption.
            let pinned_seed = pinned.and_then(|v| v.first().copied()).map(|s| s as u64);
            tracing::info!("bench job {id} ({hash}){}", if pinned_seed.is_some() { " [rejudge]" } else { "" });
            if let Err(e) = run_bench_job(&db, &cfg, &sandbox, id, participant_id, &hash, pinned_seed).await {
                tracing::error!("bench job {id} errored: {e}");
                set_state(&db, id, SubState::Error).await?;
                clear_pending(&db, participant_id).await?;
            }
            continue;
        }

        // Between jobs, roughly every 20 minutes, re-measure the reference.
        // This is contamination detection by MEASUREMENT rather than inference:
        // throttling, frequency drift or a mystery daemon all show up here
        // without any per-run classification logic.
        if last_spot_check.elapsed() > Duration::from_secs(20 * 60) {
            last_spot_check = std::time::Instant::now();
            if let Err(e) = reference_spot_check(&db, &cfg, &sandbox).await {
                tracing::error!("reference spot check failed: {e}");
            }
        }

        tokio::time::sleep(Duration::from_millis(500)).await;
    }
}

async fn run_bench_job(
    db: &PgPool,
    cfg: &Config,
    sandbox: &Sandbox,
    id: i64,
    participant_id: i32,
    hash: &str,
    pinned_seed: Option<u64>,
) -> Result<()> {
    let b = sandbox.init(cfg.box_id).await?;
    let _guard = BoxGuard { sandbox, id: cfg.box_id };

    // ALWAYS recompile on the benchmark node.
    //
    // The plan allows reusing the pool's cached binary when its build
    // fingerprint matches this node's — but nothing embeds a fingerprint into a
    // submission .so, so that check has no evidence to work from and would
    // pass vacuously. Rather than keep a check that looks like it verifies
    // something, take the branch it was there to guarantee: recompile locally
    // rather than measuring a binary this node cannot vouch for. It costs about
    // a second against a job of tens of seconds.
    let source = cfg.storage.get_source(hash).await?;
    sandbox::place(&b, "engine.cpp", &source).await?;
    let compile = compile_in_box(cfg, sandbox, &b, "engine.cpp", "engine").await?;
    if compile.exit_code != 0 {
        set_state(db, id, SubState::CompileFailed).await?;
        clear_pending(db, participant_id).await?;
        return Ok(());
    }

    let seed = pinned_seed.unwrap_or_else(|| rand_seed() as u64);
    let stream = generate_stream(cfg, seed, BENCH_PROFILE, BENCH_EVENTS).await?;

    // The expected digest is the ORACLE's for this stream, computed outside the
    // box. Verification inside the timed run is what stops anyone winning by
    // doing less work.
    let oracle_digest = oracle_digest(cfg, &stream.path).await?;
    stage_harness(cfg, &b).await?;

    let out = sandbox
        .run_with_stream(
            &b,
            &RunOpts::for_bench(600.0),
            &[
                "./harness",
                "bench",
                "--stream-fd",
                &sandbox::STREAM_FD.to_string(),
                "--engine",
                "./engine.so",
                "--runs",
                &BENCH_RUNS.to_string(),
                "--digest",
                &oracle_digest,
                "--json",
            ],
            Some(stream.dup_fd()?),
        )
        .await?;

    let result: serde_json::Value =
        serde_json::from_str(out.stdout.trim()).unwrap_or_else(|_| serde_json::json!({}));

    if out.exit_code == harness_exit::UNHEALTHY {
        // Three consecutive steal-time discards: stop requeueing and alert.
        // The node is unhealthy and a human should look before the queue churns
        // silently.
        tracing::error!("bench node reports unhealthy; parking job {id}");
        sqlx::query("UPDATE workers SET healthy = false WHERE id = $1")
            .bind(&cfg.worker_id)
            .execute(db)
            .await?;
        sqlx::query(
            "UPDATE submissions SET state = 'pending_benchmark', requeue_priority = 1, \
             updated_at = now() WHERE id = $1",
        )
        .bind(id)
        .execute(db)
        .await?;
        log_event(db, Some(id), "bench_node_unhealthy", result).await?;
        return Ok(());
    }

    let state = if out.exit_code == harness_exit::PASSED {
        SubState::Done
    } else {
        // Passed correctness, diverged on the benchmark stream — its own
        // outcome, not a generic failure.
        SubState::BenchVerifyFailed
    };

    let f = |k: &str| result.get(k).and_then(|v| v.as_f64());
    let runs: Vec<f64> = result
        .get("run_p50s_ns")
        .and_then(|v| v.as_array())
        .map(|a| a.iter().filter_map(|x| x.as_f64()).collect())
        .unwrap_or_default();

    sqlx::query(
        "UPDATE submissions SET state = $2, bench_seed_set = ARRAY[$3::bigint], p50_ns = $4, \
         p95_ns = $5, p99_ns = $6, probe_cost_ns = $7, run_p50s_ns = $8, discard_count = $9, \
         percentiles = $10, runs = $11, timeline = $12, updated_at = now() WHERE id = $1",
    )
    .bind(id)
    .bind(state)
    .bind(seed as i64)
    .bind(f("p50_ns"))
    .bind(f("p95_ns"))
    .bind(f("p99_ns"))
    .bind(f("probe_cost_ns"))
    .bind(&runs)
    .bind(result.get("discard_count").and_then(|v| v.as_i64()).unwrap_or(0) as i32)
    .bind(result.get("percentiles").cloned())
    .bind(result.get("runs").cloned())
    .bind(result.get("timeline").cloned())
    .execute(db)
    .await?;

    clear_pending(db, participant_id).await?;
    Ok(())
}

/// Contamination detection by MEASUREMENT rather than inference: throttling,
/// frequency drift and a mystery daemon all show up here without any per-run
/// classification logic.
const SPOT_CHECK_TOLERANCE: f64 = 0.02;

async fn reference_spot_check(db: &PgPool, cfg: &Config, _sandbox: &Sandbox) -> Result<()> {
    // Long enough that the harness's derived warm-up (~3.9M events for this
    // profile) still leaves a timed region, so this measures the reference
    // against a SETTLED 300k-order book — the same memory-bound work a ranked
    // run does. At 2M it would time the book still filling from 50k to 130k:
    // deterministic, so the baseline would still be stable, but a number whose
    // meaning changes with any depth retune, compared against a 2% tolerance.
    let out = tokio::process::Command::new(&cfg.harness_bin)
        .args([
            "bench", "--seed", "1", "--profile", BENCH_PROFILE, "--events", "5000000", "--runs",
            "3", "--engine", &cfg.reference_so, "--json",
        ])
        .output()
        .await?;
    let v: serde_json::Value =
        serde_json::from_str(String::from_utf8_lossy(&out.stdout).trim()).unwrap_or_default();

    let Some(p50) = v.get("p50_ns").and_then(|x| x.as_f64()) else {
        log_event(db, None, "reference_spot_check_failed", v).await?;
        return Ok(());
    };

    // The morning baseline, recorded by the first spot check after the node
    // comes up. Every later one is compared against it — a measurement that is
    // never compared is just a log line.
    let baseline: Option<(serde_json::Value,)> =
        sqlx::query_as("SELECT value FROM settings WHERE key = 'bench_reference_baseline_ns'")
            .fetch_optional(db)
            .await?;

    let baseline = baseline.and_then(|b| b.0.as_f64());
    let Some(base) = baseline else {
        sqlx::query(
            "INSERT INTO settings (key, value) VALUES ('bench_reference_baseline_ns', $1) \
             ON CONFLICT (key) DO UPDATE SET value = $1",
        )
        .bind(serde_json::json!(p50))
        .execute(db)
        .await?;
        tracing::info!("recorded reference baseline: {p50:.1} ns");
        log_event(db, None, "reference_baseline_set", serde_json::json!({ "p50_ns": p50 })).await?;
        return Ok(());
    };

    let deviation = (p50 - base).abs() / base;
    if deviation > SPOT_CHECK_TOLERANCE {
        tracing::error!(
            "reference spot check deviated {:.1}% from baseline ({p50:.1} vs {base:.1} ns); \
             marking this node unhealthy",
            deviation * 100.0
        );
        sqlx::query("UPDATE workers SET healthy = false WHERE id = $1")
            .bind(&cfg.worker_id)
            .execute(db)
            .await?;
        // `held` marks this as a deliberate verdict, so the heartbeat cannot
        // quietly undo it 15 seconds later.
        sqlx::query("UPDATE workers SET detail = jsonb_build_object('held', true) WHERE id = $1")
            .bind(&cfg.worker_id)
            .execute(db)
            .await?;
        log_event(
            db,
            None,
            "reference_spot_check_alert",
            serde_json::json!({ "p50_ns": p50, "baseline_ns": base, "deviation": deviation }),
        )
        .await?;
    } else {
        // Back within tolerance. The check that condemned the node is the one
        // entitled to absolve it — otherwise a single transient spike parks
        // every submission in pending_benchmark for the rest of the event.
        let restored: Option<(String,)> = sqlx::query_as(
            "UPDATE workers SET healthy = true, detail = NULL \
             WHERE id = $1 AND healthy = false RETURNING id",
        )
        .bind(&cfg.worker_id)
        .fetch_optional(db)
        .await?;
        if restored.is_some() {
            tracing::warn!("reference spot check recovered ({p50:.1} ns); node healthy again");
            log_event(
                db,
                None,
                "reference_spot_check_recovered",
                serde_json::json!({ "p50_ns": p50, "baseline_ns": base }),
            )
            .await?;
        }
        log_event(
            db,
            None,
            "reference_spot_check",
            serde_json::json!({ "p50_ns": p50, "baseline_ns": base, "deviation": deviation }),
        )
        .await?;
    }
    Ok(())
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
    // the pool and the bench node are different instance families, and a binary
    // verified on one must be the binary measured on the other.
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
    // They are a few kilobytes, it removes a whole class of mount-path
    // mistakes, and the compile is hermetic: -Iinclude resolves inside the box
    // no matter where the platform is installed on the node.
    stage_headers(cfg, b, kind).await?;

    let mut opts = RunOpts::for_compile();
    // isolate starts the box with a minimal environment and no PATH, so a bare
    // "g++" cannot resolve. cfg.cxx is an absolute path for the same reason the
    // spec pins the compiler: which g++ ran must not depend on the node's PATH.
    opts.env.push(("PATH".into(), "/usr/bin:/bin".into()));
    sandbox.run(b, &opts, &refs).await
}

/// A generated stream, held open outside the box. Only the descriptor crosses
/// in; the file itself is never readable by the box UID, and the seed never
/// appears in argv.
pub struct HiddenStream {
    file: Option<std::fs::File>,
    pub path: std::path::PathBuf,
}

impl HiddenStream {
    /// Hand the descriptor to a sandboxed run. The file stays owned here, so
    /// the temp file is still removed when this drops.
    pub fn dup_fd(&self) -> std::io::Result<std::fs::File> {
        self.file.as_ref().expect("stream file present").try_clone()
    }
}

impl Drop for HiddenStream {
    fn drop(&mut self) {
        let _ = std::fs::remove_file(&self.path);
    }
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

/// Copy the frozen headers into the box, plus the visible tests for a Run job.
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

/// Generate a stream outside the sandbox. The seed never enters the box.
async fn generate_stream(
    cfg: &Config,
    seed: u64,
    profile: &str,
    events: u64,
) -> Result<HiddenStream> {
    let path = std::env::temp_dir().join(format!("stream-{}-{}.bin", cfg.worker_id, seed));
    let out = tokio::process::Command::new(&cfg.gen_bin)
        .args([
            "--seed",
            &seed.to_string(),
            "--profile",
            profile,
            "--events",
            &events.to_string(),
            "-o",
            path.to_str().unwrap(),
        ])
        .output()
        .await
        .context("running gen")?;
    if !out.status.success() {
        anyhow::bail!("gen failed: {}", String::from_utf8_lossy(&out.stderr));
    }

    // Readable only by the worker. The box runs as a different unprivileged
    // UID, so even the path leaking would not be enough.
    use std::os::unix::fs::PermissionsExt;
    std::fs::set_permissions(&path, std::fs::Permissions::from_mode(0o600))?;
    let file = std::fs::File::open(&path)?;
    Ok(HiddenStream { file: Some(file), path })
}

/// Computed OUTSIDE the box, by the reference, over the same stream.
async fn oracle_digest(cfg: &Config, path: &std::path::Path) -> Result<String> {
    let out = tokio::process::Command::new(&cfg.harness_bin)
        .args(["digest", "--stream", path.to_str().unwrap(), "--engine", "builtin"])
        .output()
        .await?;
    Ok(String::from_utf8_lossy(&out.stdout).trim().to_string())
}

async fn set_state(db: &PgPool, id: i64, state: SubState) -> Result<()> {
    sqlx::query("UPDATE submissions SET state = $2, updated_at = now() WHERE id = $1")
        .bind(id)
        .bind(state)
        .execute(db)
        .await?;
    Ok(())
}

async fn clear_pending(db: &PgPool, participant_id: i32) -> Result<()> {
    sqlx::query("UPDATE bench_slots SET pending_sub = NULL WHERE participant_id = $1")
        .bind(participant_id)
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

/// A fresh seed per submission, so the hidden stream cannot be
/// reverse-engineered across attempts.
///
/// Drawn from the OS entropy pool, not from the clock. The seed is the one
/// thing standing between the contest and the exploit the plan names: read the
/// stream, hardcode the outputs. A submission never SEES the seed — the stream
/// arrives on an inherited descriptor — but a clock-derived seed is guessable
/// by anyone who knows the algorithm and roughly when their job was claimed,
/// and "the repository is private" is obscurity rather than a defence. It also
/// removes the collision between pool boxes claiming in the same nanosecond.
fn rand_seed() -> i64 {
    use std::io::Read;
    let mut bytes = [0u8; 8];
    if let Ok(mut f) = std::fs::File::open("/dev/urandom") {
        if f.read_exact(&mut bytes).is_ok() {
            return (u64::from_le_bytes(bytes) & 0x7FFF_FFFF_FFFF_FFFF) as i64;
        }
    }
    // Only reachable if /dev/urandom is unavailable, which on these nodes means
    // something is badly wrong — but a worker that cannot seed is worse than a
    // worker with a weak seed, so fall back rather than refuse to run.
    tracing::error!("/dev/urandom unavailable; falling back to a clock-derived seed");
    use std::time::{SystemTime, UNIX_EPOCH};
    let d = SystemTime::now().duration_since(UNIX_EPOCH).unwrap();
    ((d.as_secs().wrapping_mul(1_000_000_007).wrapping_add(d.subsec_nanos() as u64))
        & 0x7FFF_FFFF_FFFF_FFFF) as i64
}
