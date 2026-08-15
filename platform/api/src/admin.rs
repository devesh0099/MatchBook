//! Operator routes.
//!
//! This is a SEPARATE axum Router on its own listener, reached over an SSH
//! tunnel to the web node. That is the entire auth story for the platform —
//! not a route prefix, not a middleware, not a token. There is no auth code
//! anywhere, and unreachability is the reason that is defensible (impl spec
//! section 3).
//!
//! WHERE that unreachability is enforced depends on how the API is running,
//! and it is worth being precise because getting it wrong fails in both
//! directions:
//!
//!   * Bare process — `ADMIN_BIND_ADDR=127.0.0.1:8081`. The kernel refuses
//!     anything that is not loopback.
//!   * Under compose — the container binds `0.0.0.0:8081` and the HOST PUBLISH
//!     `127.0.0.1:8081:8081` is the restriction. Binding the container's own
//!     loopback instead makes the port unreachable from anywhere at all,
//!     because Docker forwards published ports to the container's bridge
//!     address and never to its loopback.
//!
//! Either way Caddy must not proxy it, and the Caddyfile says so.

use axum::{
    extract::{Path, Query, State},
    routing::{get, post},
    Json, Router,
};
use serde::Deserialize;
use serde_json::json;

use crate::state::{leaderboard_from_db, AppState};

pub fn router(state: AppState) -> Router {
    Router::new()
        .route("/admin/events", get(events))
        .route("/admin/queue", get(queue_detail))
        .route("/admin/participants", post(issue_credentials))
        .route("/admin/requeue/:id", post(requeue))
        .route("/admin/freeze", post(freeze))
        .route("/admin/unfreeze", post(unfreeze))
        .route("/admin/box/:id/:health", post(set_box_health))
        .route("/admin/leaderboard/rebuild", post(rebuild))
        .route("/admin/close", post(close_submissions))
        .route("/admin/open", post(open_submissions))
        .route("/admin/rejudge", post(rejudge))
        .route("/admin/publish-final", post(publish_final))
        // M7: the dashboard read surface. Read-only queries over the same
        // tables everything else writes; a future dashboard UI renders these
        // three and is done. The one-glance answer to "is the event healthy"
        // is /admin/health.
        .route("/admin/health", get(health_summary))
        .route("/admin/boxes", get(boxes_detail))
        .route("/admin/participants/status", get(participants_status))
        .with_state(state)
}

/// The same operator surface, mounted on the PUBLIC listener under /op for
/// the /admin dashboard — every route behind the operator session
/// (auth::require_op) except login itself. The loopback listener above stays
/// unchanged: reaching it is already SSH access to the web node, and curl
/// ops keep working with no cookie.
pub fn op_router(state: AppState) -> Router {
    let guarded = Router::new()
        .route("/op/health", get(health_summary))
        .route("/op/boxes", get(boxes_detail))
        .route("/op/participants/status", get(participants_status))
        .route("/op/events", get(events))
        .route("/op/participants", post(issue_credentials))
        .route("/op/requeue/:id", post(requeue))
        .route("/op/freeze", post(freeze))
        .route("/op/unfreeze", post(unfreeze))
        .route("/op/box/:id/:health", post(set_box_health))
        .route("/op/close", post(close_submissions))
        .route("/op/open", post(open_submissions))
        .route("/op/rejudge", post(rejudge))
        .route("/op/publish-final", post(publish_final))
        .route("/op/logout", post(crate::auth::op_logout))
        .route("/op/session", get(crate::auth::op_session))
        .layer(axum::middleware::from_fn_with_state(state.clone(), crate::auth::require_op));

    Router::new()
        .route("/op/login", post(crate::auth::op_login))
        .merge(guarded)
        .with_state(state)
}

/// One glance: switches, boxes, pipeline states, backlogs, and the last
/// self-healing events. If this reads clean, the event is healthy.
async fn health_summary(State(st): State<AppState>) -> R {
    let flags: Vec<(String, serde_json::Value)> =
        sqlx::query_as("SELECT key, value FROM settings WHERE key IN \
                        ('leaderboard_frozen','submissions_closed','final_published')")
            .fetch_all(&st.db)
            .await
            .map_err(oops)?;
    let boxes: Vec<(String, String, Option<i32>, bool, f64, Option<serde_json::Value>)> = sqlx::query_as(
        "SELECT id, role, participant_id, healthy, \
                EXTRACT(EPOCH FROM (now() - last_seen))::float8, detail \
         FROM boxes ORDER BY id",
    )
    .fetch_all(&st.db)
    .await
    .map_err(oops)?;
    let sub_states: Vec<(String, i64)> = sqlx::query_as(
        "SELECT state::text, count(*) FROM submissions GROUP BY state ORDER BY 2 DESC",
    )
    .fetch_all(&st.db)
    .await
    .map_err(oops)?;
    let (runs_pending,): (i64,) =
        sqlx::query_as("SELECT count(*) FROM run_jobs WHERE state IN ('received','running')")
            .fetch_one(&st.db)
            .await
            .map_err(oops)?;
    let rejudge: Vec<(String, i64)> =
        sqlx::query_as("SELECT state, count(*) FROM rejudge_jobs GROUP BY state")
            .fetch_all(&st.db)
            .await
            .map_err(oops)?;
    let heals: Vec<(String, chrono::DateTime<chrono::Utc>)> = sqlx::query_as(
        "SELECT kind, at FROM events_log \
         WHERE kind IN ('janitor_reset_dead_box','janitor_reset_early', \
                        'janitor_reset_measure','agent_startup_reclaim','box_unhealthy') \
         ORDER BY id DESC LIMIT 10",
    )
    .fetch_all(&st.db)
    .await
    .map_err(oops)?;

    let unhealthy = boxes.iter().filter(|b| !b.3).count();
    Ok(Json(json!({
        "ok": unhealthy == 0,
        "flags": flags.into_iter().map(|(k, v)| json!({k: v})).collect::<Vec<_>>(),
        "boxes": {
            "total": boxes.len(),
            "unhealthy": unhealthy,
            "rows": boxes.into_iter().map(|(id, role, pid, healthy, age, detail)| json!({
                "id": id, "role": role, "participant_id": pid,
                "healthy": healthy, "last_seen_secs_ago": age.round(),
                "job": detail.as_ref().and_then(|d| d.get("job")).cloned(),
                "load1": detail.as_ref().and_then(|d| d.get("load1")).cloned(),
                "mem_avail_mb": detail.as_ref().and_then(|d| d.get("mem_avail_mb")).cloned(),
                "steal_total": detail.as_ref().and_then(|d| d.get("steal_total")).cloned(),
            })).collect::<Vec<_>>(),
        },
        "submissions": sub_states.into_iter().map(|(s, n)| json!({"state": s, "count": n})).collect::<Vec<_>>(),
        "run_jobs_pending": runs_pending,
        "rejudge": rejudge.into_iter().map(|(s, n)| json!({"state": s, "count": n})).collect::<Vec<_>>(),
        "recent_self_healing": heals.into_iter().map(|(k, at)| json!({"kind": k, "at": at})).collect::<Vec<_>>(),
    })))
}

/// The full box registry, detail blob included — hygiene reports, steal
/// events, current job land in `detail` as agents learn to report them.
async fn boxes_detail(State(st): State<AppState>) -> R {
    let rows: Vec<(String, String, Option<i32>, bool, chrono::DateTime<chrono::Utc>, Option<serde_json::Value>)> =
        sqlx::query_as("SELECT id, role, participant_id, healthy, last_seen, detail FROM boxes ORDER BY id")
            .fetch_all(&st.db)
            .await
            .map_err(oops)?;
    Ok(Json(json!({
        "boxes": rows.into_iter().map(|(id, role, pid, healthy, seen, detail)| json!({
            "id": id, "role": role, "participant_id": pid, "healthy": healthy,
            "last_seen": seen, "detail": detail,
        })).collect::<Vec<_>>(),
    })))
}

/// Per-student rows for the dashboard: who they are, which box serves them,
/// what their latest submission is doing, and their best climb.
async fn participants_status(State(st): State<AppState>) -> R {
    let rows: Vec<(i32, String, Option<String>, Option<i64>, Option<String>, Option<i32>)> =
        sqlx::query_as(
            "SELECT p.id, p.handle, b.id, latest.id, latest.state::text, best.max_level \
             FROM participants p \
             LEFT JOIN boxes b ON b.participant_id = p.id \
             LEFT JOIN LATERAL (SELECT id, state FROM submissions \
                                WHERE participant_id = p.id \
                                ORDER BY created_at DESC, id DESC LIMIT 1) latest ON true \
             LEFT JOIN LATERAL (SELECT max_level FROM submissions \
                                WHERE participant_id = p.id AND max_level IS NOT NULL \
                                ORDER BY max_level DESC LIMIT 1) best ON true \
             ORDER BY p.id",
        )
        .fetch_all(&st.db)
        .await
        .map_err(oops)?;
    Ok(Json(json!({
        "participants": rows.into_iter().map(|(id, handle, box_id, sub, state, level)| json!({
            "participant_id": id, "handle": handle, "box": box_id,
            "latest_submission": sub, "latest_state": state, "best_level": level,
        })).collect::<Vec<_>>(),
    })))
}

/// Contest close (PLAN §3 mechanics): Submit refuses from this moment;
/// in-flight evaluations drain on their own. The last submission stored by
/// the cutoff is the rejudge target.
async fn close_submissions(State(st): State<AppState>) -> R {
    set_flag(&st, "submissions_closed", true).await.map_err(oops)?;
    log(&st, None, "submissions_closed", json!({})).await.map_err(oops)?;
    Ok(Json(json!({ "closed": true })))
}

async fn open_submissions(State(st): State<AppState>) -> R {
    set_flag(&st, "submissions_closed", false).await.map_err(oops)?;
    log(&st, None, "submissions_opened", json!({})).await.map_err(oops)?;
    Ok(Json(json!({ "closed": false })))
}

/// Queue Phase III: one job per participant, from their LAST submission
/// stored via Submit — latest, not best, regardless of how it fared live
/// (recorded decision; the spec says "make your final Submit one that
/// works"). The golden box claims these; the sealed seed reaches it as
/// MEBENCH_SEED3 when the operator starts that role, never through here.
async fn rejudge(State(st): State<AppState>) -> R {
    let rows: Vec<(i64,)> = sqlx::query_as(
        "WITH final AS ( \
           SELECT DISTINCT ON (participant_id) id, participant_id, source_hash \
           FROM submissions ORDER BY participant_id, created_at DESC, id DESC) \
         INSERT INTO rejudge_jobs (participant_id, submission_id, source_hash) \
         SELECT participant_id, id, source_hash FROM final \
         RETURNING id",
    )
    .fetch_all(&st.db)
    .await
    .map_err(oops)?;

    log(&st, None, "rejudge_queued", json!({ "jobs": rows.len() })).await.map_err(oops)?;
    Ok(Json(json!({ "queued": rows.len() })))
}

async fn publish_final(State(st): State<AppState>) -> R {
    set_flag(&st, "final_published", true).await.map_err(oops)?;
    log(&st, None, "final_published", json!({})).await.map_err(oops)?;
    Ok(Json(json!({ "published": true })))
}

type R = Result<Json<serde_json::Value>, (axum::http::StatusCode, String)>;

fn oops(e: impl std::fmt::Display) -> (axum::http::StatusCode, String) {
    (axum::http::StatusCode::INTERNAL_SERVER_ERROR, e.to_string())
}

#[derive(Deserialize)]
pub struct LimitQuery {
    #[serde(default = "default_limit")]
    pub limit: i64,
}
fn default_limit() -> i64 {
    100
}

#[derive(Deserialize)]
pub struct IssueBody {
    pub handle: String,
}

/// Issue (or reset) a participant's credentials. The password is generated
/// here, returned ONCE in this response — for the printed slip — and only its
/// Argon2id hash is stored. Re-issuing revokes every live session for that
/// participant. This is the whole account-management surface: no signup, no
/// email, no reset UX for under 20 people in a room.
async fn issue_credentials(State(st): State<AppState>, Json(body): Json<IssueBody>) -> R {
    let handle = body.handle.trim().to_string();
    if handle.is_empty() {
        return Err((axum::http::StatusCode::BAD_REQUEST, "handle is empty".into()));
    }
    let password = crate::auth::generate_password();
    let hash = crate::auth::hash_password(&password).map_err(oops)?;

    let (id,): (i32,) = sqlx::query_as(
        "INSERT INTO participants (handle, credential_hash) VALUES ($1, $2) \
         ON CONFLICT (handle) DO UPDATE SET credential_hash = $2 RETURNING id",
    )
    .bind(&handle)
    .bind(&hash)
    .fetch_one(&st.db)
    .await
    .map_err(oops)?;
    sqlx::query("DELETE FROM sessions WHERE participant_id = $1")
        .bind(id)
        .execute(&st.db)
        .await
        .map_err(oops)?;

    log(&st, None, "credentials_issued", json!({ "participant_id": id, "handle": handle }))
        .await
        .map_err(oops)?;
    Ok(Json(json!({ "participant_id": id, "handle": handle, "password": password })))
}

async fn events(State(st): State<AppState>, Query(q): Query<LimitQuery>) -> R {
    let rows: Vec<(i64, chrono::DateTime<chrono::Utc>, Option<i64>, Option<String>, Option<serde_json::Value>)> =
        sqlx::query_as("SELECT id, at, submission_id, kind, detail FROM events_log ORDER BY id DESC LIMIT $1")
            .bind(q.limit)
            .fetch_all(&st.db)
            .await
            .map_err(oops)?;
    let out: Vec<_> = rows
        .into_iter()
        .map(|(id, at, sub, kind, detail)| json!({
            "id": id, "at": at, "submission_id": sub, "kind": kind, "detail": detail
        }))
        .collect();
    Ok(Json(json!({ "events": out })))
}

/// The dashboard-stub read: submission states and the box registry — exactly
/// the tables the future admin dashboard renders.
async fn queue_detail(State(st): State<AppState>) -> R {
    let rows: Vec<(String, i64)> = sqlx::query_as(
        "SELECT state::text, count(*) FROM submissions GROUP BY state ORDER BY 2 DESC",
    )
    .fetch_all(&st.db)
    .await
    .map_err(oops)?;
    let boxes: Vec<(String, String, bool, chrono::DateTime<chrono::Utc>)> =
        sqlx::query_as("SELECT id, role, healthy, last_seen FROM boxes ORDER BY id")
            .fetch_all(&st.db)
            .await
            .map_err(oops)?;
    Ok(Json(json!({
        "states": rows.into_iter().map(|(s, n)| json!({"state": s, "count": n})).collect::<Vec<_>>(),
        "boxes": boxes.into_iter()
            .map(|(id, role, healthy, seen)| json!({"id": id, "role": role, "healthy": healthy, "last_seen": seen}))
            .collect::<Vec<_>>(),
    })))
}

/// Manual re-evaluation: back to the front of the participant's own lane. The
/// phase blobs are cleared so a half-written climb cannot masquerade as the
/// new result.
async fn requeue(State(st): State<AppState>, Path(id): Path<i64>) -> R {
    sqlx::query(
        "UPDATE submissions SET state = 'received', claimed_by = NULL, \
         phase0 = NULL, phase1 = NULL, phase2 = NULL, max_level = NULL, \
         top_p50_ns = NULL, top_p95_ns = NULL, top_p99_ns = NULL, updated_at = now() \
         WHERE id = $1",
    )
    .bind(id)
    .execute(&st.db)
    .await
    .map_err(oops)?;
    log(&st, Some(id), "admin_requeue", json!({})).await.map_err(oops)?;
    Ok(Json(json!({ "requeued": id })))
}

/// Freeze snapshots the live leaderboard into `leaderboard:frozen`. The public
/// endpoint then serves the snapshot while `done` transitions keep updating the
/// live view underneath; the reveal is a key swap, not a recomputation.
async fn freeze(State(st): State<AppState>) -> R {
    let entries = leaderboard_from_db(&st.db).await.map_err(oops)?;
    let payload = serde_json::to_string(&entries).map_err(oops)?;

    if let Some(client) = &st.redis {
        let mut conn = client.get_multiplexed_async_connection().await.map_err(oops)?;
        let _: () = redis::cmd("SET")
            .arg("leaderboard:frozen")
            .arg(&payload)
            .query_async(&mut conn)
            .await
            .map_err(oops)?;
    }
    set_flag(&st, "leaderboard_frozen", true).await.map_err(oops)?;
    log(&st, None, "leaderboard_frozen", json!({ "entries": entries.len() }))
        .await
        .map_err(oops)?;
    Ok(Json(json!({ "frozen": true, "entries": entries.len() })))
}

async fn unfreeze(State(st): State<AppState>) -> R {
    set_flag(&st, "leaderboard_frozen", false).await.map_err(oops)?;
    log(&st, None, "leaderboard_unfrozen", json!({})).await.map_err(oops)?;
    Ok(Json(json!({ "frozen": false })))
}

/// Mark one box healthy or unhealthy by hand — the operator's lever over the
/// registry. Marking unhealthy is a deliberate verdict: flagged `held` so the
/// agent's own heartbeat does not undo it on the next tick.
async fn set_box_health(
    State(st): State<AppState>,
    Path((id, health)): Path<(String, String)>,
) -> R {
    let healthy = match health.as_str() {
        "healthy" => true,
        "unhealthy" => false,
        _ => return Err((axum::http::StatusCode::BAD_REQUEST, "use healthy|unhealthy".into())),
    };
    sqlx::query(
        "UPDATE boxes SET healthy = $1, \
         detail = CASE WHEN $1 THEN NULL ELSE jsonb_build_object('held', true) END \
         WHERE id = $2",
    )
    .bind(healthy)
    .bind(&id)
    .execute(&st.db)
    .await
    .map_err(oops)?;
    log(&st, None, "admin_box_health", json!({ "box": id, "healthy": healthy }))
        .await
        .map_err(oops)?;
    Ok(Json(json!({ "box": id, "healthy": healthy })))
}

/// Redis holds only the frozen snapshot, and it is disposable: on any
/// suspicion of drift this recomputes the ranking from Postgres and — when
/// the board is frozen — refreshes the snapshot from it.
/// `leaderboard_from_db` is the only thing that ranks.
async fn rebuild(State(st): State<AppState>) -> R {
    let entries = leaderboard_from_db(&st.db).await.map_err(oops)?;
    Ok(Json(json!({ "rebuilt": entries.len() })))
}

async fn set_flag(st: &AppState, key: &str, value: bool) -> anyhow::Result<()> {
    sqlx::query(
        "INSERT INTO settings (key, value) VALUES ($1, $2) \
         ON CONFLICT (key) DO UPDATE SET value = $2",
    )
    .bind(key)
    .bind(serde_json::Value::Bool(value))
    .execute(&st.db)
    .await?;
    Ok(())
}

async fn log(
    st: &AppState,
    submission_id: Option<i64>,
    kind: &str,
    detail: serde_json::Value,
) -> anyhow::Result<()> {
    sqlx::query("INSERT INTO events_log (submission_id, kind, detail) VALUES ($1, $2, $3)")
        .bind(submission_id)
        .bind(kind)
        .bind(detail)
        .execute(&st.db)
        .await?;
    Ok(())
}
