//! Participant routes. All JSON, no auth: 18 known people in a supervised
//! room pick a handle from a preloaded roster, and the frontend remembers the
//! pick in localStorage. Operator routes live in `admin.rs` on a separate
//! listener bound to loopback.

use axum::{
    extract::{Path, Query, State},
    http::StatusCode,
    routing::{get, post, put},
    Json, Router,
};
use serde::{Deserialize, Serialize};
use serde_json::json;
use sha2::{Digest, Sha256};

use common::SubState;
use crate::state::{leaderboard_from_db, AppState, Storage};

/// One benchmark run per participant per 15 minutes, checked at enqueue.
const BENCH_RATE_LIMIT_SECS: i64 = 15 * 60;

/// Rough seconds per benchmark job, used only for the queue ETA. Measured
/// against the reference; the bench worker writes back real per-run wall times,
/// so this is a starting estimate and not a claim.
const BENCH_JOB_SECS: i64 = 45;

const MAX_SOURCE_BYTES: usize = 256 * 1024;

pub fn router(state: AppState) -> Router {
    Router::new()
        .route("/participants", get(participants))
        .route("/draft", put(put_draft).get(get_draft))
        .route("/run", post(run))
        .route("/runs/:id", get(run_result))
        .route("/submit", post(submit))
        .route("/submissions/:id", get(submission))
        .route("/me", get(me))
        .route("/leaderboard", get(leaderboard))
        .route("/queue", get(queue))
        .route("/health", get(|| async { "ok" }))
        .with_state(state)
}

type ApiResult<T> = Result<Json<T>, (StatusCode, Json<serde_json::Value>)>;

fn err(code: StatusCode, msg: impl Into<String>) -> (StatusCode, Json<serde_json::Value>) {
    (code, Json(json!({ "error": msg.into() })))
}

fn internal(e: impl std::fmt::Display) -> (StatusCode, Json<serde_json::Value>) {
    tracing::error!("internal error: {e}");
    err(StatusCode::INTERNAL_SERVER_ERROR, "internal error")
}

// ---------------------------------------------------------------- roster

#[derive(Serialize, sqlx::FromRow)]
pub struct Participant {
    pub id: i32,
    pub handle: String,
}

async fn participants(State(st): State<AppState>) -> ApiResult<Vec<Participant>> {
    let rows: Vec<Participant> =
        sqlx::query_as("SELECT id, handle FROM participants ORDER BY handle")
            .fetch_all(&st.db)
            .await
            .map_err(internal)?;
    Ok(Json(rows))
}

// ---------------------------------------------------------------- drafts

#[derive(Deserialize)]
pub struct DraftBody {
    pub participant_id: i32,
    pub source: String,
}

#[derive(Deserialize)]
pub struct WhoQuery {
    pub participant_id: i32,
}

/// The server is the source of truth for the editor buffer, so a browser crash
/// loses nothing. Debounced ~3s client-side.
async fn put_draft(
    State(st): State<AppState>,
    Json(body): Json<DraftBody>,
) -> ApiResult<serde_json::Value> {
    if body.source.len() > MAX_SOURCE_BYTES {
        return Err(err(StatusCode::PAYLOAD_TOO_LARGE, "source exceeds 256 KB"));
    }
    sqlx::query(
        "INSERT INTO drafts (participant_id, source, updated_at) VALUES ($1, $2, now()) \
         ON CONFLICT (participant_id) DO UPDATE SET source = $2, updated_at = now()",
    )
    .bind(body.participant_id)
    .bind(&body.source)
    .execute(&st.db)
    .await
    .map_err(internal)?;
    Ok(Json(json!({ "saved": true })))
}

async fn get_draft(
    State(st): State<AppState>,
    Query(q): Query<WhoQuery>,
) -> ApiResult<serde_json::Value> {
    let row: Option<(String,)> =
        sqlx::query_as("SELECT source FROM drafts WHERE participant_id = $1")
            .bind(q.participant_id)
            .fetch_optional(&st.db)
            .await
            .map_err(internal)?;
    Ok(Json(json!({ "source": row.map(|r| r.0) })))
}

fn hash_source(source: &str) -> String {
    let mut h = Sha256::new();
    h.update(source.as_bytes());
    format!("{:x}", h.finalize())
}

/// Source goes to S3 keyed on its hash; the worker only ever sees hashes. That
/// key doubles as the compile cache, which matters because participants
/// resubmit unchanged code more often than expected — especially when chasing a
/// fresh benchmark slot.
async fn store_source(st: &AppState, source: &str) -> Result<String, String> {
    let hash = hash_source(source);
    st.s3
        .put(&Storage::source_key(&hash), source.as_bytes().to_vec())
        .await
        .map_err(|e| e.to_string())?;
    Ok(hash)
}

// ---------------------------------------------------------------- run

/// LeetCode "Run": compile the draft and execute the visible tests. Unlimited,
/// returns in seconds, creates no submission. This is the iteration loop, so
/// the pool claims run_jobs ahead of submissions.
async fn run(
    State(st): State<AppState>,
    Json(body): Json<DraftBody>,
) -> ApiResult<serde_json::Value> {
    if body.source.len() > MAX_SOURCE_BYTES {
        return Err(err(StatusCode::PAYLOAD_TOO_LARGE, "source exceeds 256 KB"));
    }
    let hash = store_source(&st, &body.source).await.map_err(|e| internal(e))?;

    let (id,): (i64,) = sqlx::query_as(
        "INSERT INTO run_jobs (participant_id, source_hash) VALUES ($1, $2) RETURNING id",
    )
    .bind(body.participant_id)
    .bind(&hash)
    .fetch_one(&st.db)
    .await
    .map_err(internal)?;

    Ok(Json(json!({ "run_id": id, "source_hash": hash })))
}

#[derive(Serialize, sqlx::FromRow)]
pub struct RunJobRow {
    pub id: i64,
    pub state: String,
    pub result: Option<serde_json::Value>,
}

/// Polled by the editor until `state` is terminal. Run jobs are ephemeral and
/// never become submissions.
async fn run_result(State(st): State<AppState>, Path(id): Path<i64>) -> ApiResult<serde_json::Value> {
    let row: Option<RunJobRow> = sqlx::query_as("SELECT id, state, result FROM run_jobs WHERE id = $1")
        .bind(id)
        .fetch_optional(&st.db)
        .await
        .map_err(internal)?;
    let Some(row) = row else {
        return Err(err(StatusCode::NOT_FOUND, "no such run"));
    };
    let terminal = row.state == "done" || row.state == "failed";
    Ok(Json(json!({ "run": row, "terminal": terminal })))
}

// ---------------------------------------------------------------- submit

#[derive(Serialize)]
pub struct SubmitResponse {
    pub submission_id: i64,
    pub source_hash: String,
    /// Correctness is always accepted. This says what will happen on the bench
    /// side, at enqueue time — never silently dropped later.
    pub bench_will_queue: bool,
    pub bench_reason: String,
    pub bench_wait_secs: i64,
}

/// LeetCode "Submit": snapshot the draft as a submission, run the full
/// correctness lane on a fresh seed, then enqueue for benchmark per the rate
/// rules.
///
/// The rate limit is evaluated HERE, at enqueue, and the answer is returned in
/// this response. Queueing a job and silently discarding it later generates
/// angry questions at hour four (plan section 2).
async fn submit(
    State(st): State<AppState>,
    Json(body): Json<DraftBody>,
) -> ApiResult<SubmitResponse> {
    if body.source.trim().is_empty() {
        return Err(err(StatusCode::BAD_REQUEST, "source is empty"));
    }
    if body.source.len() > MAX_SOURCE_BYTES {
        return Err(err(StatusCode::PAYLOAD_TOO_LARGE, "source exceeds 256 KB"));
    }
    let hash = store_source(&st, &body.source).await.map_err(|e| internal(e))?;

    let mut tx = st.db.begin().await.map_err(internal)?;

    // Snapshot the draft into a submission, so "what code produced this
    // result" is always answerable.
    let (id,): (i64,) = sqlx::query_as(
        "INSERT INTO submissions (participant_id, source_hash, state) \
         VALUES ($1, $2, 'received') RETURNING id",
    )
    .bind(body.participant_id)
    .bind(&hash)
    .fetch_one(&mut *tx)
    .await
    .map_err(internal)?;

    sqlx::query(
        "INSERT INTO drafts (participant_id, source, updated_at) VALUES ($1, $2, now()) \
         ON CONFLICT (participant_id) DO UPDATE SET source = $2, updated_at = now()",
    )
    .bind(body.participant_id)
    .bind(&body.source)
    .execute(&mut *tx)
    .await
    .map_err(internal)?;

    // Both rate rules in one transaction: one bench run per 15 minutes, and at
    // most one pending bench job per participant. The second rule is what makes
    // the queue self-throttling — otherwise five spammed submissions occupy
    // twelve minutes of a serialized resource.
    let slot: Option<(Option<chrono::DateTime<chrono::Utc>>, Option<i64>)> = sqlx::query_as(
        "SELECT last_bench_at, pending_sub FROM bench_slots WHERE participant_id = $1 FOR UPDATE",
    )
    .bind(body.participant_id)
    .fetch_optional(&mut *tx)
    .await
    .map_err(internal)?;

    let now = chrono::Utc::now();
    let (mut will_queue, mut reason, mut wait) = (true, "eligible".to_string(), 0i64);

    if let Some((last_at, pending)) = slot {
        if pending.is_some() {
            will_queue = false;
            reason = "you already have a benchmark job pending".into();
        } else if let Some(last) = last_at {
            let elapsed = (now - last).num_seconds();
            if elapsed < BENCH_RATE_LIMIT_SECS {
                will_queue = false;
                wait = BENCH_RATE_LIMIT_SECS - elapsed;
                reason = format!("rate limited, {wait}s remaining");
            }
        }
    }

    tx.commit().await.map_err(internal)?;

    Ok(Json(SubmitResponse {
        submission_id: id,
        source_hash: hash,
        bench_will_queue: will_queue,
        bench_reason: reason,
        bench_wait_secs: wait,
    }))
}

// ---------------------------------------------------------------- results

#[derive(Serialize, sqlx::FromRow)]
pub struct SubmissionRow {
    pub id: i64,
    pub participant_id: i32,
    pub source_hash: String,
    pub state: SubState,
    pub created_at: Option<chrono::DateTime<chrono::Utc>>,
    pub updated_at: Option<chrono::DateTime<chrono::Utc>>,
    pub verify_detail: Option<serde_json::Value>,
    pub p50_ns: Option<f64>,
    pub p95_ns: Option<f64>,
    pub p99_ns: Option<f64>,
    pub percentiles: Option<serde_json::Value>,
    pub runs: Option<serde_json::Value>,
    pub probe_cost_ns: Option<f64>,
    pub run_p50s_ns: Option<Vec<f64>>,
    pub discard_count: Option<i32>,
    pub histogram_s3: Option<String>,
    pub flamegraph_s3: Option<String>,
}

const SUBMISSION_COLUMNS: &str = "id, participant_id, source_hash, state, created_at, updated_at, \
     verify_detail, p50_ns, p95_ns, p99_ns, percentiles, runs, probe_cost_ns, \
     run_p50s_ns, discard_count, histogram_s3, flamegraph_s3";

async fn submission(
    State(st): State<AppState>,
    Path(id): Path<i64>,
) -> ApiResult<serde_json::Value> {
    let row: Option<SubmissionRow> =
        sqlx::query_as(&format!("SELECT {SUBMISSION_COLUMNS} FROM submissions WHERE id = $1"))
            .bind(id)
            .fetch_optional(&st.db)
            .await
            .map_err(internal)?;

    let Some(row) = row else {
        return Err(err(StatusCode::NOT_FOUND, "no such submission"));
    };
    let terminal = row.state.is_terminal();
    Ok(Json(json!({ "submission": row, "terminal": terminal })))
}

async fn me(State(st): State<AppState>, Query(q): Query<WhoQuery>) -> ApiResult<Vec<SubmissionRow>> {
    let rows: Vec<SubmissionRow> = sqlx::query_as(&format!(
        "SELECT {SUBMISSION_COLUMNS} FROM submissions WHERE participant_id = $1 \
         ORDER BY created_at DESC LIMIT 100"
    ))
    .bind(q.participant_id)
    .fetch_all(&st.db)
    .await
    .map_err(internal)?;
    Ok(Json(rows))
}

// ---------------------------------------------------------------- leaderboard

/// Served from the Redis sorted set when it is warm, and rebuilt from Postgres
/// otherwise. After the freeze the public view serves the frozen snapshot while
/// `done` transitions keep updating the live one underneath; the reveal is a
/// key swap.
async fn leaderboard(State(st): State<AppState>) -> ApiResult<serde_json::Value> {
    let frozen: Option<(serde_json::Value,)> =
        sqlx::query_as("SELECT value FROM settings WHERE key = 'leaderboard_frozen'")
            .fetch_optional(&st.db)
            .await
            .map_err(internal)?;
    let is_frozen = frozen.map(|f| f.0 == serde_json::Value::Bool(true)).unwrap_or(false);

    if is_frozen {
        if let Some(client) = &st.redis {
            if let Ok(mut conn) = client.get_multiplexed_async_connection().await {
                let snap: redis::RedisResult<Option<String>> =
                    redis::cmd("GET").arg("leaderboard:frozen").query_async(&mut conn).await;
                if let Ok(Some(json)) = snap {
                    if let Ok(v) = serde_json::from_str::<serde_json::Value>(&json) {
                        return Ok(Json(json!({ "frozen": true, "entries": v })));
                    }
                }
            }
        }
    }

    let entries = leaderboard_from_db(&st.db).await.map_err(internal)?;
    Ok(Json(json!({ "frozen": is_frozen, "entries": entries })))
}

// ---------------------------------------------------------------- queue

/// "3 ahead, ~4 min" is the difference between patience and repeated
/// resubmission (plan section 2).
async fn queue(State(st): State<AppState>, Query(q): Query<WhoQuery>) -> ApiResult<serde_json::Value> {
    let (depth,): (i64,) =
        sqlx::query_as("SELECT count(*) FROM submissions WHERE state IN ('bench_queued','benchmarking')")
            .fetch_one(&st.db)
            .await
            .map_err(internal)?;

    let mine: Option<(i64,)> = sqlx::query_as(
        "SELECT count(*) FROM submissions s WHERE s.state = 'bench_queued' \
         AND s.id < COALESCE((SELECT pending_sub FROM bench_slots WHERE participant_id = $1), \
                             9223372036854775807)",
    )
    .bind(q.participant_id)
    .fetch_optional(&st.db)
    .await
    .map_err(internal)?;

    let ahead = mine.map(|m| m.0).unwrap_or(depth);
    Ok(Json(json!({
        "depth": depth,
        "ahead": ahead,
        "eta_secs": ahead * BENCH_JOB_SECS,
    })))
}
