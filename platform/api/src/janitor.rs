//! Crash recovery — the moral equivalent of Redis Streams' XAUTOCLAIM, in one
//! statement per stage.
//!
//! The claim commits immediately (no transaction held across the job), the
//! worker does the work, then commits the terminal state. If the worker dies in
//! between, the row sits in a working state forever unless something resets it.
//!
//! Every timeout here sits WELL ABOVE the corresponding isolate wall-time
//! limit, so the janitor can only ever catch a dead worker, never a live one.
//! Long-running workers touch `updated_at` as a heartbeat between bench runs.

use sqlx::PgPool;
use std::time::Duration;

/// Compile and verify are seconds of work under a 45s isolate wall-time.
const STUCK_POOL: &str = "3 minutes";
/// A bench job is 7-10 runs; 15 minutes is far past any healthy one.
const STUCK_BENCH: &str = "15 minutes";
/// The bench node is marked unhealthy after this much silence, and jobs then
/// park in pending_benchmark rather than erroring.
const WORKER_SILENCE_SECS: i64 = 90;

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
    // Pool-side stages go back to the front of the correctness lane.
    let reset_pool = sqlx::query(&format!(
        "UPDATE submissions SET state = 'received', claimed_by = NULL \
         WHERE state IN ('compiling','verifying') AND updated_at < now() - interval '{STUCK_POOL}' \
         RETURNING id"
    ))
    .fetch_all(db)
    .await?;

    // A bench job that died gets requeued at the FRONT: the participant already
    // waited once and it was not their fault.
    let reset_bench = sqlx::query(&format!(
        "UPDATE submissions SET state = 'bench_queued', claimed_by = NULL, requeue_priority = 1 \
         WHERE state = 'benchmarking' AND updated_at < now() - interval '{STUCK_BENCH}' \
         RETURNING id"
    ))
    .fetch_all(db)
    .await?;

    for (rows, kind) in [
        (&reset_pool, "janitor_reset_pool"),
        (&reset_bench, "janitor_reset_bench"),
    ] {
        for row in rows.iter() {
            use sqlx::Row;
            let id: i64 = row.get("id");
            tracing::warn!("janitor reset submission {id} ({kind})");
            // Every janitor action is recorded: if the queue behaves oddly at
            // hour four, this is the only trustworthy account of what happened.
            sqlx::query("INSERT INTO events_log (submission_id, kind, detail) VALUES ($1, $2, $3)")
                .bind(id)
                .bind(kind)
                .bind(serde_json::json!({ "reason": "worker heartbeat expired" }))
                .execute(db)
                .await?;
        }
    }

    // Run jobs need the same treatment. A worker dying mid-Run leaves the row
    // in 'running' forever, and the participant's Run button simply never
    // returns — with no error to explain it.
    let reset_runs = sqlx::query(&format!(
        "UPDATE run_jobs SET state = 'received', claimed_by = NULL \
         WHERE state = 'running' AND updated_at < now() - interval '{STUCK_POOL}' \
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

    // Worker liveness.
    let newly_unhealthy = sqlx::query(&format!(
        "UPDATE workers SET healthy = false \
         WHERE healthy = true AND last_seen < now() - interval '{WORKER_SILENCE_SECS} seconds' \
         RETURNING id"
    ))
    .fetch_all(db)
    .await?;
    for row in newly_unhealthy.iter() {
        use sqlx::Row;
        let id: String = row.get("id");
        tracing::error!("worker {id} marked unhealthy after {WORKER_SILENCE_SECS}s of silence");
        sqlx::query("INSERT INTO events_log (kind, detail) VALUES ($1, $2)")
            .bind("worker_unhealthy")
            .bind(serde_json::json!({ "worker": id }))
            .execute(db)
            .await?;
    }

    // If no bench worker is healthy, park queued jobs instead of erroring them.
    // Correctness keeps working and participants keep iterating; the queue is
    // rejudged when the node returns.
    let (healthy_bench,): (i64,) =
        sqlx::query_as("SELECT count(*) FROM workers WHERE role = 'bench' AND healthy = true")
            .fetch_one(db)
            .await?;
    if healthy_bench == 0 {
        let parked = sqlx::query(
            "UPDATE submissions SET state = 'pending_benchmark' WHERE state = 'bench_queued' \
             RETURNING id",
        )
        .fetch_all(db)
        .await?;
        if !parked.is_empty() {
            tracing::warn!("parked {} bench jobs: no healthy bench worker", parked.len());
        }
    } else {
        // Node is back — unpark. Position is whatever it was when it first
        // entered the queue; parking is not a demotion.
        sqlx::query("UPDATE submissions SET state = 'bench_queued' WHERE state = 'pending_benchmark'")
            .execute(db)
            .await?;
        promote_held(db).await?;
    }

    Ok(())
}

/// Seat the newest held submission of every participant whose bench slot is
/// free.
///
/// Without this, `verify_passed` is a dead end. `try_enqueue_bench` is the only
/// thing that ever seats a bench job and it runs exactly once, when
/// verification passes; a submission held behind the participant's own running
/// job is never reconsidered, and `clear_pending` frees the slot without
/// looking for a successor. The participant is left reading "held: #N is being
/// benchmarked" about a job that finished minutes ago.
///
/// This lives in the janitor rather than in the worker's `clear_pending` on
/// purpose: a worker that dies between finishing a job and promoting the next
/// one would lose the promotion permanently, whereas a sweep re-derives the
/// right answer from the table on every tick. It costs up to 60s of latency
/// against a bench job that takes about that long anyway.
async fn promote_held(db: &PgPool) -> anyhow::Result<()> {
    // Never seat fresh work during a rejudge block. Rejudge queues every
    // finalist on one pinned seed and never sets `pending_sub`, so every slot
    // reads free; promotions would sort behind the block (priority 1) but run
    // immediately after it on unpinned seeds, inside the window whose whole
    // point is that it is settled.
    let (rejudging,): (i64,) = sqlx::query_as(
        "SELECT count(*) FROM submissions \
         WHERE bench_seed_set IS NOT NULL AND state IN ('bench_queued','benchmarking')",
    )
    .fetch_one(db)
    .await?;
    if rejudging > 0 {
        return Ok(());
    }

    let mut tx = db.begin().await?;

    // Lock the free slots first, in a stable order. The worker takes the same
    // lock in try_enqueue_bench, so a verify passing right now either waits for
    // this sweep or is seen by it — never both seating a job.
    let free: Vec<(i32,)> = sqlx::query_as(
        "SELECT participant_id FROM bench_slots WHERE pending_sub IS NULL \
         ORDER BY participant_id FOR UPDATE",
    )
    .fetch_all(&mut *tx)
    .await?;

    let mut promoted = Vec::new();
    for (pid,) in free {
        // Newest held submission wins, and the rest are superseded rather than
        // left to accumulate. That cap is what keeps the queue fair: a
        // participant who submits ten times while blocked contributes ONE job
        // when their slot frees, not ten, so nobody can build a backlog that
        // starves everyone else.
        let held: Vec<(i64,)> = sqlx::query_as(
            "SELECT id FROM submissions WHERE participant_id = $1 AND state = 'verify_passed' \
             ORDER BY created_at DESC, id DESC FOR UPDATE",
        )
        .bind(pid)
        .fetch_all(&mut *tx)
        .await?;

        let Some((newest,)) = held.first().copied() else { continue };

        sqlx::query(
            "UPDATE submissions SET state = 'bench_queued', bench_queued_at = now(), \
             updated_at = now(), verify_detail = COALESCE(verify_detail, '{}'::jsonb) - 'bench_held' \
             WHERE id = $1",
        )
        .bind(newest)
        .execute(&mut *tx)
        .await?;

        sqlx::query(
            "INSERT INTO bench_slots (participant_id, last_bench_at, pending_sub) \
             VALUES ($1, now(), $2) ON CONFLICT (participant_id) \
             DO UPDATE SET last_bench_at = now(), pending_sub = $2",
        )
        .bind(pid)
        .bind(newest)
        .execute(&mut *tx)
        .await?;

        for (older,) in held.iter().skip(1) {
            sqlx::query(
                "UPDATE submissions SET state = 'superseded', updated_at = now(), \
                 verify_detail = COALESCE(verify_detail, '{}'::jsonb) \
                   || jsonb_build_object('superseded_by', $2::bigint) \
                 WHERE id = $1",
            )
            .bind(older)
            .bind(newest)
            .execute(&mut *tx)
            .await?;
        }

        promoted.push((newest, held.len() - 1));
    }

    tx.commit().await?;

    for (id, superseded) in promoted {
        tracing::info!("promoted held submission {id} to the bench queue ({superseded} superseded)");
        sqlx::query("INSERT INTO events_log (submission_id, kind, detail) VALUES ($1,$2,$3)")
            .bind(id)
            .bind("bench_promoted")
            .bind(serde_json::json!({ "superseded": superseded }))
            .execute(db)
            .await?;
    }

    Ok(())
}
