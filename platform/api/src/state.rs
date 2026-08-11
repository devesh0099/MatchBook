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

    pub fn source_key(hash: &str) -> String {
        format!("source/{hash}.cpp")
    }
}

#[derive(Debug, Serialize, sqlx::FromRow)]
pub struct LeaderboardRow {
    pub handle: String,
    pub submission_id: i64,
    pub p50_ns: Option<f64>,
    pub p99_ns: Option<f64>,
    pub probe_cost_ns: Option<f64>,
    pub run_p50s_ns: Option<Vec<f64>>,
}

#[derive(Debug, Serialize)]
pub struct LeaderboardEntry {
    pub handle: String,
    pub p50_ns: f64,
    pub p99_ns: Option<f64>,
    pub probe_cost_ns: Option<f64>,
    /// Bootstrap CI across the per-run medians. Published so a close pair can be
    /// read honestly — it does NOT affect rank. Ranking is a strict total order
    /// on p50, ties broken by the earlier submission.
    pub ci_low_ns: f64,
    pub ci_high_ns: f64,
    pub submission_id: i64,
}

/// Postgres is the source of truth; the view is one query over 18 rows, so
/// rebuilding the Redis serving copy from it is always cheap enough to be the
/// answer to any suspicion of drift.
pub async fn leaderboard_from_db(db: &PgPool) -> Result<Vec<LeaderboardEntry>> {
    // The order this returns IS the published ranking: p50 ascending, and where
    // two scores are equal the earlier submission is above. Equal p50s are not a
    // curiosity — if the bench node's TSC is quantised, a ranked p50 is a count
    // of quanta and exact ties are expected. `created_at` is ordered on but not
    // selected; the client cannot reproduce this order and must not re-sort.
    let rows: Vec<LeaderboardRow> = sqlx::query_as(
        "SELECT handle, submission_id, p50_ns, p99_ns, probe_cost_ns, run_p50s_ns \
         FROM leaderboard ORDER BY p50_ns ASC, created_at ASC",
    )
    .fetch_all(db)
    .await?;

    Ok(rows
        .into_iter()
        .filter_map(|r| {
            let p50 = r.p50_ns?;
            let runs = r.run_p50s_ns.clone().unwrap_or_default();
            let (lo, hi) = bootstrap_ci(&runs, p50);
            Some(LeaderboardEntry {
                handle: r.handle,
                p50_ns: p50,
                p99_ns: r.p99_ns,
                probe_cost_ns: r.probe_cost_ns,
                ci_low_ns: lo,
                ci_high_ns: hi,
                submission_id: r.submission_id,
            })
        })
        .collect())
}

/// Percentile bootstrap over the per-run medians.
///
/// This quantifies sampling noise across runs. It says nothing about
/// systematic bias: a node that is 8% slow yields a tight interval around a
/// wrong number. That is what the end-of-event rejudge block and the machine
/// health checks are for.
fn bootstrap_ci(runs: &[f64], fallback: f64) -> (f64, f64) {
    if runs.len() < 2 {
        return (fallback, fallback);
    }
    const RESAMPLES: usize = 2000;
    // Deterministic: the same runs always produce the same published interval.
    let mut rng: u64 = 0x5EED_5EED;
    let mut next = || {
        rng ^= rng << 13;
        rng ^= rng >> 7;
        rng ^= rng << 17;
        rng
    };

    let mut medians = Vec::with_capacity(RESAMPLES);
    let mut sample = vec![0.0f64; runs.len()];
    for _ in 0..RESAMPLES {
        for slot in sample.iter_mut() {
            *slot = runs[(next() as usize) % runs.len()];
        }
        sample.sort_by(|a, b| a.partial_cmp(b).unwrap());
        medians.push(sample[sample.len() / 2]);
    }
    medians.sort_by(|a, b| a.partial_cmp(b).unwrap());
    (
        medians[(RESAMPLES as f64 * 0.025) as usize],
        medians[(RESAMPLES as f64 * 0.975) as usize],
    )
}
