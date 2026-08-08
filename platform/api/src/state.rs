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

/// Fixed band thresholds, calibrated against the reference implementation.
///
/// Positions are not published: the correctness gate compresses the surviving
/// cohort — everyone left wrote a working order book — so spreads are often
/// under 2x and defending rank 7 against rank 9 is not honestly possible
/// (plan section 8).
pub const BANDS: &[(&str, f64)] = &[
    ("gold", 60.0),
    ("silver", 90.0),
    ("bronze", 140.0),
    ("finisher", f64::INFINITY),
];

pub fn band_for(p50_ns: f64) -> &'static str {
    for (name, ceiling) in BANDS {
        if p50_ns <= *ceiling {
            return name;
        }
    }
    "finisher"
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
    pub band: &'static str,
    pub p50_ns: f64,
    pub p99_ns: Option<f64>,
    pub probe_cost_ns: Option<f64>,
    /// Bootstrap CI across the per-run medians. Overlapping intervals mean
    /// tied rank.
    pub ci_low_ns: f64,
    pub ci_high_ns: f64,
    pub submission_id: i64,
}

/// Postgres is the source of truth; the view is one query over 18 rows, so
/// rebuilding the Redis serving copy from it is always cheap enough to be the
/// answer to any suspicion of drift.
pub async fn leaderboard_from_db(db: &PgPool) -> Result<Vec<LeaderboardEntry>> {
    let rows: Vec<LeaderboardRow> = sqlx::query_as(
        "SELECT handle, submission_id, p50_ns, p99_ns, probe_cost_ns, run_p50s_ns \
         FROM leaderboard ORDER BY p50_ns ASC",
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
                band: band_for(p50),
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
