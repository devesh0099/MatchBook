//! Participant routes. All JSON. Identity comes from the session cookie via
//! the `Auth` extractor (auth.rs) — the client never says who it is, the
//! server already knows. Operator routes live in `admin.rs` on a separate
//! listener bound to loopback.

use axum::{
    extract::{Path, State},
    http::StatusCode,
    routing::{get, post, put},
    Json, Router,
};
use serde::{Deserialize, Serialize};
use serde_json::json;
use sha2::{Digest, Sha256};

use common::SubState;
use crate::auth::{self, Auth};
use crate::state::{AppState, Storage};

const MAX_SOURCE_BYTES: usize = 256 * 1024;

pub fn router(state: AppState) -> Router {
    Router::new()
        .route("/login", post(auth::login))
        .route("/logout", post(auth::logout))
        .route("/session", get(auth::session))
        .route("/draft", put(put_draft).get(get_draft))
        .route("/run", post(run))
        .route("/runs/:id", get(run_result))
        .route("/submit", post(submit))
        .route("/submissions/:id", get(submission))
        .route("/me", get(me))
        .route("/leaderboard", get(leaderboard))
        .route("/final", get(final_standings))
        .route("/provisional", get(provisional_leaderboard))
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

// ---------------------------------------------------------------- drafts

#[derive(Deserialize)]
pub struct DraftBody {
    pub source: String,
}

/// The server is the source of truth for the editor buffer, so a browser crash
/// loses nothing. Debounced ~3s client-side.
async fn put_draft(
    State(st): State<AppState>,
    auth: Auth,
    Json(body): Json<DraftBody>,
) -> ApiResult<serde_json::Value> {
    if body.source.len() > MAX_SOURCE_BYTES {
        return Err(err(StatusCode::PAYLOAD_TOO_LARGE, "source exceeds 256 KB"));
    }
    sqlx::query(
        "INSERT INTO drafts (participant_id, source, updated_at) VALUES ($1, $2, now()) \
         ON CONFLICT (participant_id) DO UPDATE SET source = $2, updated_at = now()",
    )
    .bind(auth.participant_id)
    .bind(&body.source)
    .execute(&st.db)
    .await
    .map_err(internal)?;
    Ok(Json(json!({ "saved": true })))
}

async fn get_draft(State(st): State<AppState>, auth: Auth) -> ApiResult<serde_json::Value> {
    let row: Option<(String,)> =
        sqlx::query_as("SELECT source FROM drafts WHERE participant_id = $1")
            .bind(auth.participant_id)
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

/// "Run": Phase 0's visible tests + Phase I, feedback only — the exact
/// instrument that will judge them, on every iteration. Unlimited, creates no
/// submission, and the agent claims run_jobs ahead of submissions.
///
/// Rejected while a Submit is evaluating: the box is theirs, but the
/// measurement core is single-file (PLAN-measurement-redesign §3 mechanics).
async fn run(
    State(st): State<AppState>,
    auth: Auth,
    Json(body): Json<DraftBody>,
) -> ApiResult<serde_json::Value> {
    if body.source.len() > MAX_SOURCE_BYTES {
        return Err(err(StatusCode::PAYLOAD_TOO_LARGE, "source exceeds 256 KB"));
    }
    if let Some(active) = active_submission(&st, auth.participant_id).await? {
        return Err(err(
            StatusCode::CONFLICT,
            format!("submission #{active} is being evaluated — Run is available again the moment it finishes"),
        ));
    }
    let hash = store_source(&st, &body.source).await.map_err(|e| internal(e))?;

    let (id,): (i64,) = sqlx::query_as(
        "INSERT INTO run_jobs (participant_id, source_hash) VALUES ($1, $2) RETURNING id",
    )
    .bind(auth.participant_id)
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
/// never become submissions. Owner-only, and a foreign id reads as NOT FOUND
/// rather than FORBIDDEN — existence is not leaked either.
async fn run_result(
    State(st): State<AppState>,
    auth: Auth,
    Path(id): Path<i64>,
) -> ApiResult<serde_json::Value> {
    let row: Option<RunJobRow> = sqlx::query_as(
        "SELECT id, state, result FROM run_jobs WHERE id = $1 AND participant_id = $2",
    )
    .bind(id)
    .bind(auth.participant_id)
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
}

/// "Submit": snapshot the draft as a submission and run the whole gauntlet —
/// Phase 0 (tests + hidden verify), Phase I, Phase II ladder — on the
/// participant's box. Unlimited, EXCEPT one evaluation at a time: a Submit
/// while any evaluation of theirs is running is rejected with a clear
/// message, not queued and not cancel-and-replace (recorded decision,
/// PLAN-measurement-redesign §3). One-at-a-time is the only throttle.
async fn submit(
    State(st): State<AppState>,
    auth: Auth,
    Json(body): Json<DraftBody>,
) -> ApiResult<SubmitResponse> {
    if body.source.trim().is_empty() {
        return Err(err(StatusCode::BAD_REQUEST, "source is empty"));
    }
    if body.source.len() > MAX_SOURCE_BYTES {
        return Err(err(StatusCode::PAYLOAD_TOO_LARGE, "source exceeds 256 KB"));
    }
    // The cutoff (PLAN §3 mechanics): after /admin/close, nothing new is
    // stored — the last submission stored BEFORE this moment is the rejudge
    // target, so refusing here is what makes "stored by the cutoff" exact.
    let closed: Option<(serde_json::Value,)> =
        sqlx::query_as("SELECT value FROM settings WHERE key = 'submissions_closed'")
            .fetch_optional(&st.db)
            .await
            .map_err(internal)?;
    if closed.map(|c| c.0 == serde_json::Value::Bool(true)).unwrap_or(false) {
        return Err(err(
            StatusCode::FORBIDDEN,
            "submissions are closed — the contest has ended; your last submission is what the rejudge measures",
        ));
    }
    if let Some(active) = active_submission(&st, auth.participant_id).await? {
        return Err(err(
            StatusCode::CONFLICT,
            format!(
                "submission #{active} is still being evaluated — one evaluation at a time; \
                 submit again the moment it finishes"
            ),
        ));
    }
    let hash = store_source(&st, &body.source).await.map_err(|e| internal(e))?;

    let mut tx = st.db.begin().await.map_err(internal)?;

    // Snapshot the draft into a submission, so "what code produced this
    // result" is always answerable. This S3-stored snapshot is also what the
    // rejudge judges: latest stored via Submit, regardless of how it fares.
    let (id,): (i64,) = sqlx::query_as(
        "INSERT INTO submissions (participant_id, source_hash, state) \
         VALUES ($1, $2, 'received') RETURNING id",
    )
    .bind(auth.participant_id)
    .bind(&hash)
    .fetch_one(&mut *tx)
    .await
    .map_err(internal)?;

    sqlx::query(
        "INSERT INTO drafts (participant_id, source, updated_at) VALUES ($1, $2, now()) \
         ON CONFLICT (participant_id) DO UPDATE SET source = $2, updated_at = now()",
    )
    .bind(auth.participant_id)
    .bind(&body.source)
    .execute(&mut *tx)
    .await
    .map_err(internal)?;

    tx.commit().await.map_err(internal)?;

    Ok(Json(SubmitResponse { submission_id: id, source_hash: hash }))
}

/// The participant's in-flight submission, if any. The one-at-a-time rule in
/// one place, used by submit, run, and the status endpoint, so the editor's
/// message and the server's behaviour cannot drift apart.
async fn active_submission(
    st: &AppState,
    participant_id: i32,
) -> Result<Option<i64>, (StatusCode, Json<serde_json::Value>)> {
    let row: Option<(i64,)> = sqlx::query_as(
        "SELECT id FROM submissions WHERE participant_id = $1 \
         AND state IN ('received','compiling','testing','verifying','phase1','phase2') \
         ORDER BY id DESC LIMIT 1",
    )
    .bind(participant_id)
    .fetch_optional(&st.db)
    .await
    .map_err(internal)?;
    Ok(row.map(|r| r.0))
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
    /// One stable blob per phase — the dashboard contract.
    pub phase0: Option<serde_json::Value>,
    pub phase1: Option<serde_json::Value>,
    pub phase2: Option<serde_json::Value>,
    pub max_level: Option<i32>,
    pub top_p50_ns: Option<f64>,
    pub top_p95_ns: Option<f64>,
    pub top_p99_ns: Option<f64>,
}

pub(crate) const SUBMISSION_COLUMNS: &str = "id, participant_id, source_hash, state, created_at, updated_at, \
     phase0, phase1, phase2, max_level, top_p50_ns, top_p95_ns, top_p99_ns";

/// Owner-only; a foreign id reads as NOT FOUND, leaking neither content nor
/// existence. The leaderboard is the public surface.
async fn submission(
    State(st): State<AppState>,
    auth: Auth,
    Path(id): Path<i64>,
) -> ApiResult<serde_json::Value> {
    let row: Option<SubmissionRow> = sqlx::query_as(&format!(
        "SELECT {SUBMISSION_COLUMNS} FROM submissions WHERE id = $1 AND participant_id = $2"
    ))
    .bind(id)
    .bind(auth.participant_id)
    .fetch_optional(&st.db)
    .await
    .map_err(internal)?;

    let Some(row) = row else {
        return Err(err(StatusCode::NOT_FOUND, "no such submission"));
    };
    let terminal = row.state.is_terminal();
    Ok(Json(json!({ "submission": row, "terminal": terminal })))
}

async fn me(State(st): State<AppState>, auth: Auth) -> ApiResult<Vec<SubmissionRow>> {
    let rows: Vec<SubmissionRow> = sqlx::query_as(&format!(
        "SELECT {SUBMISSION_COLUMNS} FROM submissions WHERE participant_id = $1 \
         ORDER BY created_at DESC LIMIT 100"
    ))
    .bind(auth.participant_id)
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

    let entries = crate::state::progress_board_from_db(&st.db).await.map_err(internal)?;
    Ok(Json(json!({ "frozen": is_frozen, "entries": entries })))
}

// ---------------------------------------------------------------- final

#[derive(Serialize, sqlx::FromRow)]
pub struct FinalRow {
    pub handle: String,
    pub submission_id: i64,
    pub finished: Option<bool>,
    pub p95_ns: Option<f64>,
    pub p50_ns: Option<f64>,
    pub p99_ns: Option<f64>,
    pub events_processed: Option<i64>,
}

/// The final standings — golden-box numbers, sealed seed, the recorded chain
/// (PLAN §3): finishers above non-finishers; finishers on p95, p50, p99;
/// non-finishers on p99 over the events they processed, then progress; ties
/// fall to the earlier submission. Hidden until the operator publishes.
async fn final_standings(State(st): State<AppState>) -> ApiResult<serde_json::Value> {
    let published: Option<(serde_json::Value,)> =
        sqlx::query_as("SELECT value FROM settings WHERE key = 'final_published'")
            .fetch_optional(&st.db)
            .await
            .map_err(internal)?;
    if !published.map(|p| p.0 == serde_json::Value::Bool(true)).unwrap_or(false) {
        return Ok(Json(json!({ "published": false, "entries": [] })));
    }

    let rows: Vec<FinalRow> = sqlx::query_as(
        "SELECT p.handle, r.submission_id, r.finished, r.p95_ns, r.p50_ns, r.p99_ns, \
                r.events_processed \
         FROM rejudge_results r \
         JOIN participants p ON p.id = r.participant_id \
         JOIN submissions s ON s.id = r.submission_id \
         ORDER BY r.finished DESC NULLS LAST, \
                  CASE WHEN r.finished THEN r.p95_ns END ASC NULLS LAST, \
                  CASE WHEN r.finished THEN r.p50_ns END ASC NULLS LAST, \
                  CASE WHEN r.finished THEN r.p99_ns END ASC NULLS LAST, \
                  CASE WHEN NOT r.finished THEN NULLIF(r.p99_ns, 0) END ASC NULLS LAST, \
                  r.events_processed DESC NULLS LAST, \
                  s.created_at ASC",
    )
    .fetch_all(&st.db)
    .await
    .map_err(internal)?;
    Ok(Json(json!({ "published": true, "entries": rows })))
}

/// The provisional (live) standings as they stood the moment Phase III began
/// — snapshotted at rejudge time. Lets the public board offer a "how it looked
/// before the rejudge" view alongside the final standings.
async fn provisional_leaderboard(State(st): State<AppState>) -> ApiResult<serde_json::Value> {
    let row: Option<(serde_json::Value,)> =
        sqlx::query_as("SELECT value FROM settings WHERE key = 'pre_phase3_leaderboard'")
            .fetch_optional(&st.db)
            .await
            .map_err(internal)?;
    Ok(Json(json!({ "entries": row.map(|r| r.0).unwrap_or(json!([])) })))
}

// ---------------------------------------------------------------- status

/// Nobody queues behind anybody anymore — the box is theirs. What remains to
/// report is the one-at-a-time rule: whether THEIR evaluation is in flight,
/// by exactly the rule submit() enforces.
async fn queue(State(st): State<AppState>, auth: Auth) -> ApiResult<serde_json::Value> {
    let active = active_submission(&st, auth.participant_id).await?;
    Ok(Json(json!({
        "evaluating": active.is_some(),
        "submission_id": active,
        "reason": match active {
            Some(id) => format!("submission #{id} is being evaluated — one evaluation at a time"),
            None => "eligible".to_string(),
        },
    })))
}
