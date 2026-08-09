//! Operator routes.
//!
//! This is a SEPARATE axum Router served on its own listener bound to
//! 127.0.0.1, reached over an SSH tunnel to the web node. That is the entire
//! auth story for the platform — not a route prefix, not a middleware, not a
//! token. There is no auth code anywhere, and this listener is the reason that
//! is defensible (impl spec section 3).

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
        .route("/admin/discards", get(discards))
        .route("/admin/requeue/:id", post(requeue))
        .route("/admin/freeze", post(freeze))
        .route("/admin/unfreeze", post(unfreeze))
        .route("/admin/bench/:state", post(set_bench_health))
        .route("/admin/leaderboard/rebuild", post(rebuild))
        .route("/admin/rejudge", post(rejudge))
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

async fn queue_detail(State(st): State<AppState>) -> R {
    let rows: Vec<(String, i64)> = sqlx::query_as(
        "SELECT state::text, count(*) FROM submissions GROUP BY state ORDER BY 2 DESC",
    )
    .fetch_all(&st.db)
    .await
    .map_err(oops)?;
    let workers: Vec<(String, String, bool, chrono::DateTime<chrono::Utc>)> =
        sqlx::query_as("SELECT id, role, healthy, last_seen FROM workers ORDER BY id")
            .fetch_all(&st.db)
            .await
            .map_err(oops)?;
    Ok(Json(json!({
        "states": rows.into_iter().map(|(s, n)| json!({"state": s, "count": n})).collect::<Vec<_>>(),
        "workers": workers.into_iter()
            .map(|(id, role, healthy, seen)| json!({"id": id, "role": role, "healthy": healthy, "last_seen": seen}))
            .collect::<Vec<_>>(),
    })))
}

/// A rising machine-signal discard rate is the earliest warning that the bench
/// node has become unstable (plan section 8). Watch this, not the leaderboard.
async fn discards(State(st): State<AppState>) -> R {
    let (total, discarded): (i64, Option<i64>) = sqlx::query_as(
        "SELECT count(*), sum(discard_count) FROM submissions WHERE discard_count IS NOT NULL",
    )
    .fetch_one(&st.db)
    .await
    .map_err(oops)?;
    let repeat: Vec<(i64, i32)> = sqlx::query_as(
        "SELECT id, discard_count FROM submissions WHERE discard_count >= 3 ORDER BY id DESC LIMIT 20",
    )
    .fetch_all(&st.db)
    .await
    .map_err(oops)?;
    Ok(Json(json!({
        "submissions": total,
        "total_discards": discarded.unwrap_or(0),
        "repeat_offenders": repeat.into_iter().map(|(id, n)| json!({"id": id, "discards": n})).collect::<Vec<_>>(),
    })))
}

/// Manual requeue, kept available throughout the event. Goes to the front, like
/// every other requeue that was not the participant's fault.
async fn requeue(State(st): State<AppState>, Path(id): Path<i64>) -> R {
    sqlx::query(
        "UPDATE submissions SET state = 'bench_queued', claimed_by = NULL, requeue_priority = 1 \
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

/// Mark the bench node healthy or unhealthy by hand. Unhealthy parks queued
/// jobs as pending_benchmark; it never errors them.
async fn set_bench_health(State(st): State<AppState>, Path(state): Path<String>) -> R {
    let healthy = match state.as_str() {
        "healthy" => true,
        "unhealthy" => false,
        _ => return Err((axum::http::StatusCode::BAD_REQUEST, "use healthy|unhealthy".into())),
    };
    // Marking unhealthy by hand is a deliberate verdict: flag it `held` so the
    // worker's own heartbeat does not undo it on the next tick.
    sqlx::query(
        "UPDATE workers SET healthy = $1, \
         detail = CASE WHEN $1 THEN NULL ELSE jsonb_build_object('held', true) END \
         WHERE role = 'bench'",
    )
    .bind(healthy)
    .execute(&st.db)
    .await
    .map_err(oops)?;
    log(&st, None, "admin_bench_health", json!({ "healthy": healthy }))
        .await
        .map_err(oops)?;
    Ok(Json(json!({ "bench_healthy": healthy })))
}

/// Redis is disposable. On restart, or on any suspicion of drift, rebuild the
/// serving copy from Postgres — it is 18 rows.
async fn rebuild(State(st): State<AppState>) -> R {
    let entries = leaderboard_from_db(&st.db).await.map_err(oops)?;
    if let Some(client) = &st.redis {
        let mut conn = client.get_multiplexed_async_connection().await.map_err(oops)?;
        let _: () = redis::cmd("DEL").arg("leaderboard").query_async(&mut conn).await.map_err(oops)?;
        for e in &entries {
            let _: () = redis::cmd("ZADD")
                .arg("leaderboard")
                .arg(e.p50_ns)
                .arg(&e.handle)
                .query_async(&mut conn)
                .await
                .map_err(oops)?;
        }
    }
    Ok(Json(json!({ "rebuilt": entries.len() })))
}

#[derive(Deserialize)]
pub struct RejudgeQuery {
    /// One seed for the whole block. Omit and one is chosen for you.
    pub seed: Option<i64>,
}

/// Queue every participant's final `done` submission for re-measurement on ONE
/// shared seed.
///
/// This is what makes absolute scoring defensible without drift correction: all
/// final numbers then come from the same short window on the same machine, so
/// cross-time comparability stops being required. The live leaderboard during
/// the event is indicative; this is the authoritative measurement.
///
/// Bracket it with a reference run on either side. If the two disagree, rerun
/// the block before announcing.
async fn rejudge(State(st): State<AppState>, Query(q): Query<RejudgeQuery>) -> R {
    let seed = q.seed.unwrap_or_else(|| {
        use std::time::{SystemTime, UNIX_EPOCH};
        (SystemTime::now().duration_since(UNIX_EPOCH).unwrap().as_secs() & 0x7FFF_FFFF) as i64
    });

    let rows: Vec<(i64,)> = sqlx::query_as(
        "WITH final AS (            SELECT DISTINCT ON (participant_id) id FROM submissions            WHERE state = 'done' ORDER BY participant_id, created_at DESC)          UPDATE submissions s SET state = 'bench_queued', claimed_by = NULL,            requeue_priority = 1, bench_seed_set = ARRAY[$1::bigint]          FROM final WHERE s.id = final.id RETURNING s.id",
    )
    .bind(seed)
    .fetch_all(&st.db)
    .await
    .map_err(oops)?;

    log(&st, None, "rejudge_started", json!({ "seed": seed, "submissions": rows.len() }))
        .await
        .map_err(oops)?;

    Ok(Json(json!({
        "seed": seed,
        "queued": rows.len(),
        "submissions": rows.into_iter().map(|r| r.0).collect::<Vec<_>>(),
    })))
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
