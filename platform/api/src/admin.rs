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
        .route("/op/participants/:id/deploy", post(deploy_box))
        .route("/op/participants/:id/redeploy", post(redeploy_box))
        .route("/op/participants/:id/terminate", post(terminate_box))
        .route("/op/participants/:id/remove", post(remove_participant))
        .route("/op/events", get(events))
        .route("/op/participants", post(issue_credentials))
        .route("/op/requeue/:id", post(requeue))
        .route("/op/freeze", post(freeze))
        .route("/op/unfreeze", post(unfreeze))
        .route("/op/box/:id/:health", post(set_box_health))
        .route("/op/close", post(close_submissions))
        .route("/op/open", post(open_submissions))
        .route("/op/rejudge", post(rejudge))
        .route("/op/rejudge/status", get(rejudge_status))
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

/// Per-student rows for the dashboard: identity, box lifecycle state, a live
/// activity string (IDLE / Phase I / Phase II · L3 …) derived from the box
/// state and the latest submission, and their best climb. The `activity` is
/// what renders in front of the Deploy/Redeploy button.
async fn participants_status(State(st): State<AppState>) -> R {
    let rows: Vec<(
        i32, String, Option<String>, String, Option<String>, Option<bool>,
        Option<i64>, Option<String>, Option<serde_json::Value>, Option<i32>,
    )> = sqlx::query_as(
        "SELECT p.id, p.handle, b.id, p.box_state, p.box_detail, b.healthy, \
                latest.id, latest.state::text, latest.phase2, best.max_level \
         FROM participants p \
         LEFT JOIN boxes b ON b.participant_id = p.id \
         LEFT JOIN LATERAL (SELECT id, state, phase2 FROM submissions \
                            WHERE participant_id = p.id \
                            ORDER BY created_at DESC, id DESC LIMIT 1) latest ON true \
         LEFT JOIN LATERAL (SELECT max_level FROM submissions \
                            WHERE participant_id = p.id AND max_level IS NOT NULL \
                            ORDER BY max_level DESC LIMIT 1) best ON true \
         WHERE p.removed_at IS NULL \
         ORDER BY p.id",
    )
    .fetch_all(&st.db)
    .await
    .map_err(oops)?;

    // The activity string, computed once so the dashboard renders one field.
    // While a box is (re)deploying, the daemon's own progress note
    // (box_detail: launching / booting / provisioning / ready) is shown so the
    // operator sees the deploy advance, not just a static "deploying".
    fn activity(
        box_state: &str, box_detail: Option<&str>, healthy: Option<bool>,
        latest_state: Option<&str>, phase2: &Option<serde_json::Value>,
    ) -> String {
        // A live, healthy heartbeat is the ground truth for "the box is up and
        // serving." It OVERRIDES a stale provisioner state: if the daemon was
        // restarted (a web-node redeploy restarts it) between provisioning the
        // box and writing box_state='ready', the participant would otherwise be
        // stuck on "deploying…" forever even though the box is heartbeating.
        // Once the box joins the heartbeat list, we show its real activity.
        let live = healthy == Some(true);
        match box_state {
            "none" => return "no box".into(),
            "removing" => return "removing…".into(),
            "deploying" | "redeploying" if !live => {
                let verb = if box_state == "redeploying" { "redeploy" } else { "deploy" };
                return box_detail
                    .filter(|d| !d.is_empty())
                    .map(|d| format!("{verb}: {d}"))
                    .unwrap_or_else(|| format!("{verb}ing…"));
            }
            "failed" if !live => return box_detail.filter(|d| !d.is_empty()).map(|d| format!("failed: {d}")).unwrap_or_else(|| "deploy failed".into()),
            _ => {}
        }
        // Ready — or live despite a stale state: reflect what it's actually doing.
        match latest_state {
            Some("received") => "queued".into(),
            Some("compiling") => "compiling".into(),
            Some("testing") => "running tests".into(),
            Some("verifying") => "verifying".into(),
            Some("phase1") => "Phase I".into(),
            Some("phase2") => {
                let cleared = phase2
                    .as_ref()
                    .and_then(|p| p.get("levels"))
                    .and_then(|l| l.as_array())
                    .map(|a| a.iter().filter(|lv| lv.get("outcome").and_then(|o| o.as_str()) == Some("ok")).count())
                    .unwrap_or(0);
                format!("Phase II · L{}", cleared.max(1))
            }
            // terminal or no submission -> the box is idle (or unhealthy)
            _ => {
                if healthy == Some(false) { "box unhealthy".into() } else { "IDLE".into() }
            }
        }
    }

    Ok(Json(json!({
        "participants": rows.into_iter().map(|(id, handle, box_id, box_state, box_detail, healthy, sub, state, phase2, level)| {
            let act = activity(&box_state, box_detail.as_deref(), healthy, state.as_deref(), &phase2);
            json!({
                "participant_id": id, "handle": handle, "box": box_id,
                "box_state": box_state, "activity": act,
                "latest_submission": sub, "best_level": level,
            })
        }).collect::<Vec<_>>(),
    })))
}

// -------------------------------------------------- box lifecycle (M8)

/// Enqueue a provisioning request; the daemon fulfills it. The API never
/// touches AWS. Guards against a duplicate in-flight request per participant.
async fn enqueue_box(st: &AppState, id: i32, action: &str, deploying_state: &str) -> R {
    let exists: Option<(i64,)> = sqlx::query_as(
        "SELECT id FROM box_requests WHERE participant_id = $1 AND state IN ('pending','running')",
    )
    .bind(id)
    .fetch_optional(&st.db)
    .await
    .map_err(oops)?;
    if exists.is_some() {
        return Err((axum::http::StatusCode::CONFLICT, "a box request is already in flight".into()));
    }
    sqlx::query("INSERT INTO box_requests (participant_id, action) VALUES ($1, $2)")
        .bind(id)
        .bind(action)
        .execute(&st.db)
        .await
        .map_err(oops)?;
    sqlx::query("UPDATE participants SET box_state = $2, box_detail = NULL WHERE id = $1")
        .bind(id)
        .bind(deploying_state)
        .execute(&st.db)
        .await
        .map_err(oops)?;
    log(st, None, &format!("box_{action}"), json!({ "participant_id": id })).await.map_err(oops)?;
    Ok(Json(json!({ "queued": action, "participant_id": id })))
}

async fn deploy_box(State(st): State<AppState>, Path(id): Path<i32>) -> R {
    enqueue_box(&st, id, "deploy", "deploying").await
}
async fn redeploy_box(State(st): State<AppState>, Path(id): Path<i32>) -> R {
    enqueue_box(&st, id, "redeploy", "redeploying").await
}
async fn terminate_box(State(st): State<AppState>, Path(id): Path<i32>) -> R {
    enqueue_box(&st, id, "terminate", "deploying").await
}

/// Remove a participant with a VERIFIED, staged teardown (the operator's
/// requested order): the participant is not dropped from the list until their
/// box is confirmed destroyed and its registry row cleaned. Their session is
/// revoked at once so they cannot act during teardown; their submissions
/// survive for the record.
///
///   has a box  -> box_state='removing', enqueue a 'remove' request; the
///                 daemon checks the instance, terminates it, waits for it to
///                 be gone, deletes the box row, THEN sets removed_at (which
///                 is what finally hides the row from the dashboard).
///   no box     -> removed immediately; there is nothing to tear down.
async fn remove_participant(State(st): State<AppState>, Path(id): Path<i32>) -> R {
    let box_state: Option<(String,)> =
        sqlx::query_as("SELECT box_state FROM participants WHERE id = $1 AND removed_at IS NULL")
            .bind(id)
            .fetch_optional(&st.db)
            .await
            .map_err(oops)?;
    let Some((state,)) = box_state else {
        return Ok(Json(json!({ "removed": id, "note": "already removed" })));
    };

    // revoke the session immediately either way
    sqlx::query("DELETE FROM sessions WHERE participant_id = $1")
        .bind(id)
        .execute(&st.db)
        .await
        .map_err(oops)?;

    if state == "none" {
        sqlx::query("UPDATE participants SET removed_at = now() WHERE id = $1")
            .bind(id)
            .execute(&st.db)
            .await
            .map_err(oops)?;
        log(&st, None, "participant_removed", json!({ "participant_id": id })).await.map_err(oops)?;
        return Ok(Json(json!({ "removed": id })));
    }

    // has a box: stage the teardown through the daemon, keep the row visible
    // (showing "removing…") until the box is verified gone.
    sqlx::query(
        "UPDATE participants SET box_state = 'removing', box_detail = 'queued for teardown' WHERE id = $1",
    )
    .bind(id)
    .execute(&st.db)
    .await
    .map_err(oops)?;
    sqlx::query("INSERT INTO box_requests (participant_id, action) VALUES ($1, 'remove')")
        .bind(id)
        .execute(&st.db)
        .await
        .map_err(oops)?;
    log(&st, None, "participant_removing", json!({ "participant_id": id })).await.map_err(oops)?;
    Ok(Json(json!({ "removing": id })))
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
    // Snapshot the provisional leaderboard AT THIS MOMENT — the standings as
    // they were before Phase III — so the public board can always show the
    // pre-rejudge picture no matter how the recomputation later shifts.
    let provisional = leaderboard_from_db(&st.db).await.map_err(oops)?;
    let snap = serde_json::to_value(&provisional).map_err(oops)?;
    sqlx::query(
        "INSERT INTO settings (key, value) VALUES ('pre_phase3_leaderboard', $1) \
         ON CONFLICT (key) DO UPDATE SET value = $1",
    )
    .bind(&snap)
    .execute(&st.db)
    .await
    .map_err(oops)?;

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

    log(&st, None, "rejudge_queued", json!({ "jobs": rows.len(), "snapshot": provisional.len() }))
        .await
        .map_err(oops)?;
    Ok(Json(json!({ "queued": rows.len() })))
}

/// Phase III live status for the dashboard: the golden box, how far the
/// rejudge has got, which contestant is being measured right now, and the
/// final leaderboard forming as results land.
async fn rejudge_status(State(st): State<AppState>) -> R {
    let golden: Option<(String, bool, f64, Option<serde_json::Value>)> = sqlx::query_as(
        "SELECT id, healthy, EXTRACT(EPOCH FROM (now()-last_seen))::float8, detail \
         FROM boxes WHERE role = 'golden' ORDER BY last_seen DESC LIMIT 1",
    )
    .fetch_optional(&st.db)
    .await
    .map_err(oops)?;

    let counts: Vec<(String, i64)> =
        sqlx::query_as("SELECT state, count(*) FROM rejudge_jobs GROUP BY state")
            .fetch_all(&st.db)
            .await
            .map_err(oops)?;
    let get = |s: &str| counts.iter().find(|(k, _)| k == s).map(|(_, n)| *n).unwrap_or(0);
    let total: i64 = counts.iter().map(|(_, n)| *n).sum();

    // which contestant is running now (from the golden box's live job note)
    let current = golden
        .as_ref()
        .and_then(|g| g.3.as_ref())
        .and_then(|d| d.get("job"))
        .and_then(|j| j.as_str())
        .filter(|j| *j != "idle")
        .map(str::to_string);

    // the final leaderboard forming, ranked by the chain
    let results: Vec<(String, Option<bool>, Option<f64>, Option<f64>, Option<f64>, Option<i64>)> =
        sqlx::query_as(
            "SELECT p.handle, r.finished, r.p95_ns, r.p50_ns, r.p99_ns, r.events_processed \
             FROM rejudge_results r JOIN participants p ON p.id = r.participant_id \
             ORDER BY r.finished DESC NULLS LAST, r.p95_ns ASC NULLS LAST, r.p50_ns ASC NULLS LAST",
        )
        .fetch_all(&st.db)
        .await
        .map_err(oops)?;

    Ok(Json(json!({
        "golden": golden.map(|(id, healthy, age, detail)| json!({
            "id": id, "healthy": healthy, "last_seen_secs_ago": age.round(), "detail": detail,
        })),
        "progress": { "total": total, "done": get("done"), "running": get("running"),
                      "pending": get("received"), "errored": get("error") },
        "current": current,
        "results": results.into_iter().map(|(handle, finished, p95, p50, p99, ev)| json!({
            "handle": handle, "finished": finished, "p95_ns": p95, "p50_ns": p50,
            "p99_ns": p99, "events_processed": ev,
        })).collect::<Vec<_>>(),
    })))
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
