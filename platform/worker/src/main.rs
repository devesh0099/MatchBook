//! me-platform worker.
//!
//!   worker --role pool     compile + verify, 4-8 boxes in parallel
//!   worker --role bench    ranked runs, strictly one at a time
//!
//! Both roles poll Postgres directly. Nothing containerized ever runs on the
//! benchmark node: these are plain systemd units.
//!
//! The bench role is never scaled. Two submissions on different physical cores
//! still share L3, memory bandwidth and the memory controller, and an order
//! book is memory-latency-bound — a neighbour thrashing L3 reintroduces exactly
//! the noisy-neighbour problem the dedicated node was bought to avoid.

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
    if role != "pool" && role != "bench" {
        anyhow::bail!("usage: worker --role pool|bench");
    }

    let database_url = std::env::var("DATABASE_URL").context("DATABASE_URL must be set")?;
    let db = PgPoolOptions::new().max_connections(8).connect(&database_url).await?;

    let s3_client = common::s3_client().await;
    let storage = Arc::new(Storage {
        client: s3_client,
        bucket: env_or("S3_BUCKET", "me-platform-artifacts"),
    });

    let hostname = env_or("HOSTNAME", "worker");
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
        storage,
    };

    register(&db, &cfg).await?;
    spawn_heartbeat(db.clone(), cfg.worker_id.clone());

    tracing::info!("worker {} starting as {role}", cfg.worker_id);
    match role.as_str() {
        "pool" => roles::run_pool(db, cfg).await,
        _ => roles::run_bench(db, cfg).await,
    }
}

async fn register(db: &sqlx::PgPool, cfg: &Config) -> Result<()> {
    sqlx::query(
        "INSERT INTO workers (id, role, last_seen, healthy) VALUES ($1, $2, now(), true) \
         ON CONFLICT (id) DO UPDATE SET role = $2, last_seen = now(), healthy = true",
    )
    .bind(&cfg.worker_id)
    .bind(&cfg.role)
    .execute(db)
    .await?;
    Ok(())
}

/// The API marks a worker unhealthy after 90s of silence, and bench jobs then
/// park as pending_benchmark rather than erroring — losing the bench node must
/// not kill the event.
fn spawn_heartbeat(db: sqlx::PgPool, worker_id: String) {
    tokio::spawn(async move {
        let mut ticker = tokio::time::interval(Duration::from_secs(15));
        loop {
            ticker.tick().await;
            if let Err(e) = sqlx::query("UPDATE workers SET last_seen = now() WHERE id = $1")
                .bind(&worker_id)
                .execute(&db)
                .await
            {
                tracing::warn!("heartbeat failed: {e}");
            }
        }
    });
}
