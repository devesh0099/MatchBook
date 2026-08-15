//! Crash recovery — one statement per stage.
//!
//! The claim commits immediately (no transaction held across the job), the
//! agent does the work, then commits the terminal state. If the agent dies in
//! between, the row sits in a working state forever unless something resets
//! it.
//!
//! Every timeout here sits WELL ABOVE the corresponding stage's own limits,
//! so the janitor can only ever catch a dead agent, never a live one. The
//! agent touches `updated_at` after every phase and every ladder level as a
//! heartbeat.

use sqlx::PgPool;
use std::time::Duration;

/// Compile, tests, and verify are seconds of work under sub-minute isolate
/// wall-times.
const STUCK_EARLY: &str = "3 minutes";
/// Phase I is three short gated runs; Phase II is a whole climb — minutes at
/// the top rungs, with a heartbeat per level. Thirty minutes only catches an
/// agent that died mid-level.
const STUCK_MEASURE: &str = "30 minutes";
/// A box is marked unhealthy after this much silence.
const BOX_SILENCE_SECS: i64 = 90;

pub fn spawn(db: PgPool) {
    tokio::spawn(async move {
        let mut ticker = tokio::time::interval(Duration::from_secs(60));
        loop {
            ticker.tick().await;
            if let Err(e) = sweep(&db).await {
                tracing::error!("janitor sweep failed: {e}");
            }
        }
    });
}

async fn sweep(db: &PgPool) -> anyhow::Result<()> {
    // Early stages go back to the front of the lane.
    let reset_early = sqlx::query(&format!(
        "UPDATE submissions SET state = 'received', claimed_by = NULL \
         WHERE state IN ('compiling','testing','verifying') \
         AND updated_at < now() - interval '{STUCK_EARLY}' RETURNING id"
    ))
    .fetch_all(db)
    .await?;

    // A measurement phase that died restarts the whole evaluation: phases are
    // one claimed job, and a half-measured climb is not a result. The phase
    // blobs are cleared so stale progress cannot masquerade as fresh.
    let reset_measure = sqlx::query(&format!(
        "UPDATE submissions SET state = 'received', claimed_by = NULL, \
         phase1 = NULL, phase2 = NULL \
         WHERE state IN ('phase1','phase2') \
         AND updated_at < now() - interval '{STUCK_MEASURE}' RETURNING id"
    ))
    .fetch_all(db)
    .await?;

    for (rows, kind) in [
        (&reset_early, "janitor_reset_early"),
        (&reset_measure, "janitor_reset_measure"),
    ] {
        for row in rows.iter() {
            use sqlx::Row;
            let id: i64 = row.get("id");
            tracing::warn!("janitor reset submission {id} ({kind})");
            // Every janitor action is recorded: if the pipeline behaves oddly
            // at hour four, this is the only trustworthy account.
            sqlx::query("INSERT INTO events_log (submission_id, kind, detail) VALUES ($1, $2, $3)")
                .bind(id)
                .bind(kind)
                .bind(serde_json::json!({ "reason": "agent heartbeat expired" }))
                .execute(db)
                .await?;
        }
    }

    // Run jobs: an agent dying mid-Run leaves the row in 'running' forever,
    // and the participant's Run button simply never returns.
    let reset_runs = sqlx::query(&format!(
        "UPDATE run_jobs SET state = 'received', claimed_by = NULL \
         WHERE state = 'running' AND updated_at < now() - interval '{STUCK_EARLY}' \
         RETURNING id"
    ))
    .fetch_all(db)
    .await?;
    for row in reset_runs.iter() {
        use sqlx::Row;
        let id: i64 = row.get("id");
        tracing::warn!("janitor reset run job {id}");
        sqlx::query("INSERT INTO events_log (kind, detail) VALUES ($1, $2)")
            .bind("janitor_reset_run_job")
            .bind(serde_json::json!({ "run_job": id }))
            .execute(db)
            .await?;
    }

    // Box liveness, into the registry the dashboard reads.
    let newly_unhealthy = sqlx::query(&format!(
        "UPDATE boxes SET healthy = false \
         WHERE healthy = true AND last_seen < now() - interval '{BOX_SILENCE_SECS} seconds' \
         RETURNING id"
    ))
    .fetch_all(db)
    .await?;
    for row in newly_unhealthy.iter() {
        use sqlx::Row;
        let id: String = row.get("id");
        tracing::error!("box {id} marked unhealthy after {BOX_SILENCE_SECS}s of silence");
        sqlx::query("INSERT INTO events_log (kind, detail) VALUES ($1, $2)")
            .bind("box_unhealthy")
            .bind(serde_json::json!({ "box": id }))
            .execute(db)
            .await?;
    }

    Ok(())
}
