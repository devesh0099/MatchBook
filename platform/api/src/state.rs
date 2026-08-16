//! Shared application state and the leaderboard read model.

use anyhow::Result;
use serde::Serialize;
use sqlx::PgPool;
use std::sync::Arc;

#[derive(Clone)]
pub struct AppState {
    pub db: PgPool,
    pub redis: Option<redis::Client>,
    pub s3: Arc<Storage>,
    /// (email, argon2 hash) for the /admin dashboard's operator login, from
    /// ADMIN_EMAIL / ADMIN_PASSWORD env — deployment config, never the repo.
    /// None disables operator login entirely.
    pub op_credential: Option<Arc<(String, String)>>,
}

/// Blob storage: source, binaries, histograms, flamegraphs, keyed on source
/// hash so it doubles as the compile cache.
pub struct Storage {
    pub client: aws_sdk_s3::Client,
    pub bucket: String,
}

impl Storage {
    pub async fn put(&self, key: &str, body: Vec<u8>) -> Result<()> {
        self.client
            .put_object()
            .bucket(&self.bucket)
            .key(key)
            .body(body.into())
            .send()
            .await?;
        Ok(())
    }

    /// Fetch a blob back — the operator's submission view reads the source the
    /// worker only ever saw as a hash.
    pub async fn get(&self, key: &str) -> Result<Vec<u8>> {
        let out = self
            .client
            .get_object()
            .bucket(&self.bucket)
            .key(key)
            .send()
            .await?;
        let data = out.body.collect().await?.into_bytes().to_vec();
        Ok(data)
    }

    pub fn source_key(hash: &str) -> String {
        format!("source/{hash}.cpp")
    }
}

#[derive(Debug, Serialize, sqlx::FromRow)]
pub struct LeaderboardEntry {
    pub handle: String,
    pub submission_id: i64,
    /// Highest ladder level cleared. 0 means the climb ended at rung 1 — the
    /// row still ranks, on its Phase I chain.
    pub max_level: i32,
    /// The scoring chain at the highest cleared level (or Phase I's when
    /// max_level is 0): p95, then p50, then p99. Measured on the student's
    /// own box, so live gaps are provisional; the golden box settles it.
    pub chain_p95_ns: Option<f64>,
    pub chain_p50_ns: Option<f64>,
    pub chain_p99_ns: Option<f64>,
}

/// Postgres is the source of truth; the view is one query over 18 rows, so
/// rebuilding the Redis serving copy from it is always cheap enough to be the
/// answer to any suspicion of drift.
pub async fn leaderboard_from_db(db: &PgPool) -> Result<Vec<LeaderboardEntry>> {
    // The order this returns IS the published ranking: level, then the chain,
    // then the earlier submission. Exact chain ties are expected — rdtscp
    // steps in quanta — and they fall to created_at, which is ordered on but
    // not selected; the client cannot reproduce this order and must not
    // re-sort.
    let rows: Vec<LeaderboardEntry> = sqlx::query_as(
        "SELECT handle, submission_id, max_level, chain_p95_ns, chain_p50_ns, chain_p99_ns \
         FROM leaderboard \
         ORDER BY max_level DESC, chain_p95_ns ASC NULLS LAST, chain_p50_ns ASC NULLS LAST, \
                  chain_p99_ns ASC NULLS LAST, created_at ASC",
    )
    .fetch_all(db)
    .await?;
    Ok(rows)
}
