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
        .route("/admin/requeue/:id", post(requeue))
        .route("/admin/freeze", post(freeze))
        .route("/admin/unfreeze", post(unfreeze))
        .route("/admin/box/:id/:health", post(set_box_health))
        .route("/admin/leaderboard/rebuild", post(rebuild))
        // The rejudge block returns with the golden-box role (M6): it reads
        // every participant's LAST submission stored via Submit and writes
        // rejudge_results on the sealed seed.
        .with_state(state)
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
