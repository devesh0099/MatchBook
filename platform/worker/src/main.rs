//! flashmatch agent (PLAN-measurement-redesign §7).
//!
//!   worker --role agent    the per-box agent: Run and Submit pipelines,
//!                          phases 0 -> I -> II, one job at a time
//!
//! (`--role pool` is accepted as an alias while the compose stack stands in
//! for the fleet; the golden-box role arrives with the rejudge in M6.)
//!
//! The agent polls Postgres directly — no broker. On the real fleet each
//! agent is bound to its participant's box; on the compose stack one agent
//! serves everyone, which exercises the identical code path.

mod roles;
mod sandbox;

use anyhow::{Context, Result};
use sqlx::postgres::PgPoolOptions;
use std::sync::Arc;
use std::time::Duration;

pub struct Storage {
    client: aws_sdk_s3::Client,
    bucket: String,
}

impl Storage {
    async fn get(&self, key: &str) -> Result<Vec<u8>> {
        let obj = self.client.get_object().bucket(&self.bucket).key(key).send().await?;
        Ok(obj.body.collect().await?.into_bytes().to_vec())
    }
    async fn put(&self, key: &str, body: Vec<u8>) -> Result<()> {
        self.client
            .put_object()
            .bucket(&self.bucket)
            .key(key)
            .body(body.into())
            .send()
            .await?;
        Ok(())
    }
    pub async fn get_source(&self, hash: &str) -> Result<Vec<u8>> {
        self.get(&format!("source/{hash}.cpp")).await
    }
    pub async fn get_binary(&self, hash: &str) -> Result<Vec<u8>> {
        self.get(&format!("binary/{hash}.so")).await
    }
    pub async fn put_binary(&self, hash: &str, body: Vec<u8>) -> Result<()> {
        self.put(&format!("binary/{hash}.so"), body).await
    }
}

#[derive(Clone)]
pub struct Config {
    pub worker_id: String,
    pub role: String,
    pub box_id: u32,
    pub cxx: String,
    pub march: String,
    pub include_dir: String,
    pub tests_dir: String,
    pub harness_bin: String,
    pub gen_bin: String,
    pub reference_so: String,
    /// The fixed measurement seeds (PLAN-measurement-redesign §4). SEED1
    /// drives Phase I and the hidden verify's 50k prefix; SEED2 drives every
    /// ladder level. SEED3 is the sealed rejudge seed and never reaches an
    /// agent box — it arrives with the golden role in M6. Fixed seeds are
    /// what let streams and solutions be baked once and judged by lookup.
    pub seed1: u64,
    pub seed2: u64,
    /// The sealed rejudge seed. Set ONLY on the golden box, ONLY when the
    /// rejudge begins — before that it exists nowhere executable.
    pub seed3: Option<u64>,
    /// Where baked streams and solutions live. On the real fleet this is AMI
    /// content; in dev the agent bakes lazily on first use.
    pub bake_dir: String,
    /// The participant -> box binding (PLAN-measurement-redesign §6): when
    /// set, this agent claims ONLY that participant's jobs, which is the
    /// entire routing story on the fleet. Unset (the compose stack), it
    /// serves everyone through the identical code path.
    pub participant_id: Option<i32>,
    /// CPU steering on an isolated-core box (§7): measured runs pin to the
    /// isolated core, compiles to the spare cores, via taskset around the
    /// sandbox invocation. Both unset in dev.
    pub bench_cpus: Option<String>,
    pub compile_cpus: Option<String>,
    /// What this agent is doing right now — written by the claim loops, read
    /// by the heartbeat, rendered on the dashboard's box row.
    pub current_job: Arc<std::sync::Mutex<String>>,
    pub storage: Arc<Storage>,
}

fn env_or(key: &str, default: &str) -> String {
    std::env::var(key).unwrap_or_else(|_| default.into())
}

#[tokio::main]
async fn main() -> Result<()> {
    tracing_subscriber::fmt()
        .with_env_filter(
            tracing_subscriber::EnvFilter::try_from_default_env()
                .unwrap_or_else(|_| "info,sqlx=warn".into()),
        )
        .init();

    let mut role = String::new();
    let args: Vec<String> = std::env::args().collect();
    for i in 1..args.len() {
        if args[i] == "--role" && i + 1 < args.len() {
            role = args[i + 1].clone();
        }
    }
    if role == "pool" {
        role = "agent".into(); // compose-era alias
    }
    if role != "agent" && role != "golden" {
        anyhow::bail!("usage: worker --role agent|golden");
    }

    let database_url = std::env::var("DATABASE_URL").context("DATABASE_URL must be set")?;
    let db = PgPoolOptions::new().max_connections(8).connect(&database_url).await?;

    let s3_client = common::s3_client().await;
    let storage = Arc::new(Storage {
        client: s3_client,
        bucket: env_or("S3_BUCKET", "flashmatch-artifacts"),
    });

    // HOSTNAME is a shell convention, not part of a systemd unit's
    // environment — and two boxes falling back to the same name upsert one
    // registry row. /etc/hostname is the authority when the variable is gone.
    let hostname = std::env::var("HOSTNAME").ok().filter(|h| !h.trim().is_empty()).unwrap_or_else(|| {
        std::fs::read_to_string("/etc/hostname")
            .map(|h| h.trim().to_string())
            .ok()
            .filter(|h| !h.is_empty())
            .unwrap_or_else(|| "worker".into())
    });
    let box_id: u32 = env_or("BOX_ID", "0").parse().unwrap_or(0);
    let cfg = Config {
        worker_id: format!("{role}-{hostname}-{box_id}"),
        role: role.clone(),
        box_id,
        cxx: env_or("MEBENCH_CXX", "/usr/bin/g++"),
        march: env_or("MEBENCH_MARCH", "x86-64-v3"),
        include_dir: env_or("MEBENCH_INCLUDE", "/opt/mebench/include"),
        tests_dir: env_or("MEBENCH_TESTS", "/opt/mebench/tests"),
        harness_bin: env_or("MEBENCH_HARNESS", "/opt/mebench/bin/harness"),
        gen_bin: env_or("MEBENCH_GEN", "/opt/mebench/bin/gen"),
        reference_so: env_or("MEBENCH_REFERENCE_SO", "/opt/mebench/lib/libreference_engine.so"),
        // Dev defaults; the event's seeds are chosen before kickoff and set in
        // the environment. Never published.
        seed1: env_or("MEBENCH_SEED1", "101").trim().parse().unwrap_or(101),
        seed2: env_or("MEBENCH_SEED2", "202").trim().parse().unwrap_or(202),
        seed3: std::env::var("MEBENCH_SEED3").ok().and_then(|v| v.trim().parse().ok()),
        bake_dir: env_or(
            "MEBENCH_BAKE",
            &std::env::temp_dir().join("mebench-bake").to_string_lossy(),
        ),
        participant_id: std::env::var("MEBENCH_PARTICIPANT_ID").ok().and_then(|v| v.trim().parse().ok()),
        bench_cpus: std::env::var("MEBENCH_BENCH_CPUS").ok().filter(|v| !v.trim().is_empty()),
        compile_cpus: std::env::var("MEBENCH_COMPILE_CPUS").ok().filter(|v| !v.trim().is_empty()),
        current_job: Arc::new(std::sync::Mutex::new("idle".into())),
        storage,
    };

    register(&db, &cfg).await?;
    reclaim_own_orphans(&db, &cfg).await?;
    spawn_heartbeat(db.clone(), cfg.worker_id.clone(), cfg.current_job.clone());

    tracing::info!("worker {} starting as {role}", cfg.worker_id);
    match role.as_str() {
        "golden" => roles::run_golden(db, cfg).await,
        _ => roles::run_agent(db, cfg).await,
    }
}

/// Any row still claimed by THIS worker id at startup is orphaned by
/// definition — a just-started process cannot have work in flight. Reclaiming
/// it here means the common crash case (agent dies, systemd restarts it in
/// 5s) recovers in SECONDS; the janitor's dead-box and staleness sweeps only
/// cover the box that never comes back and the agent that wedges while
/// alive. Partial measurement blobs are cleared so a half-climb cannot
/// masquerade as the re-evaluation's result.
async fn reclaim_own_orphans(db: &sqlx::PgPool, cfg: &Config) -> Result<()> {
    let subs = sqlx::query(
        "UPDATE submissions SET state = 'received', claimed_by = NULL, \
         phase1 = NULL, phase2 = NULL, updated_at = now() \
         WHERE claimed_by = $1 \
         AND state IN ('compiling','testing','verifying','phase1','phase2') RETURNING id",
    )
    .bind(&cfg.worker_id)
    .fetch_all(db)
    .await?;
    let runs = sqlx::query(
        "UPDATE run_jobs SET state = 'received', claimed_by = NULL, updated_at = now() \
         WHERE claimed_by = $1 AND state = 'running' RETURNING id",
    )
    .bind(&cfg.worker_id)
    .fetch_all(db)
    .await?;
    if !subs.is_empty() || !runs.is_empty() {
        use sqlx::Row;
        for row in &subs {
            let id: i64 = row.get("id");
            tracing::warn!("reclaimed my orphaned submission {id} at startup");
            sqlx::query("INSERT INTO events_log (submission_id, kind, detail) VALUES ($1, $2, $3)")
                .bind(id)
                .bind("agent_startup_reclaim")
                .bind(serde_json::json!({ "worker": cfg.worker_id }))
                .execute(db)
                .await?;
        }
        tracing::warn!(
            "startup reclaim: {} submission(s), {} run job(s)",
            subs.len(),
            runs.len()
        );
    }
    Ok(())
}

/// Registration goes to the BOX REGISTRY — the table the future admin
/// dashboard watches — carrying the participant binding when this agent has
/// one.
async fn register(db: &sqlx::PgPool, cfg: &Config) -> Result<()> {
    sqlx::query(
        "INSERT INTO boxes (id, role, participant_id, last_seen, healthy) \
         VALUES ($1, $2, $3, now(), true) \
         ON CONFLICT (id) DO UPDATE SET role = $2, participant_id = $3, \
         last_seen = now(), healthy = true",
    )
    .bind(&cfg.worker_id)
    .bind(&cfg.role)
    .bind(cfg.participant_id)
    .execute(db)
    .await?;
    Ok(())
}

/// The janitor marks a box unhealthy after 90s of silence. A box condemned
/// for silence is healthy again the moment it speaks; a verdict recorded as
/// `held` (the operator's) is NOT undone here.
///
/// Each beat also carries the box's live telemetry — current job, load,
/// available memory, cumulative steal — into `detail`, which is what the
/// dashboard's box rows render. Cheap: three /proc reads every 15s.
fn spawn_heartbeat(db: sqlx::PgPool, worker_id: String, current_job: Arc<std::sync::Mutex<String>>) {
    fn proc_metric(path: &str, field: usize) -> Option<f64> {
        std::fs::read_to_string(path)
            .ok()?
            .split_whitespace()
            .nth(field)?
            .parse()
            .ok()
    }
    tokio::spawn(async move {
        let mut ticker = tokio::time::interval(Duration::from_secs(15));
        loop {
            ticker.tick().await;
            let job = current_job.lock().map(|j| j.clone()).unwrap_or_default();
            let load1 = proc_metric("/proc/loadavg", 0).unwrap_or(0.0);
            let mem_avail_mb = std::fs::read_to_string("/proc/meminfo")
                .ok()
                .and_then(|m| {
                    m.lines()
                        .find(|l| l.starts_with("MemAvailable:"))
                        .and_then(|l| l.split_whitespace().nth(1))
                        .and_then(|v| v.parse::<u64>().ok())
                })
                .map(|kb| kb / 1024)
                .unwrap_or(0);
            // Field 9 of the aggregate cpu line is cumulative steal ticks —
            // the dashboard watches the DELTA between beats for co-tenant
            // interference.
            let steal = proc_metric("/proc/stat", 9).unwrap_or(0.0);
            let telemetry = serde_json::json!({
                "job": job,
                "load1": load1,
                "mem_avail_mb": mem_avail_mb,
                "steal_total": steal,
            });
            if let Err(e) = sqlx::query(
                "UPDATE boxes SET last_seen = now(), \
                 detail = COALESCE(detail, '{}'::jsonb) || $2, \
                 healthy = CASE WHEN COALESCE(detail->>'held','false') = 'true' \
                                THEN healthy ELSE true END \
                 WHERE id = $1",
            )
                .bind(&worker_id)
                .bind(&telemetry)
                .execute(&db)
                .await
            {
                tracing::warn!("heartbeat failed: {e}");
            }
        }
    });
}
