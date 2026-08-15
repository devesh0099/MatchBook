//! Types shared by the API and the worker.
//!
//! The submission state machine and the harness exit-code contract both cross
//! a process boundary, so they are defined once here rather than transcribed
//! into two crates that can drift apart.

use serde::{Deserialize, Serialize};

/// Mirrors the `sub_state` enum in the schema exactly. Adding a state here
/// without adding it there (or vice versa) is a decode error at runtime, which
/// is the point of using a real enum type rather than text.
#[derive(Debug, Clone, Copy, PartialEq, Eq, sqlx::Type, Serialize, Deserialize)]
#[sqlx(type_name = "sub_state", rename_all = "snake_case")]
#[serde(rename_all = "snake_case")]
pub enum SubState {
    Received,
    Compiling,
    CompileFailed,
    /// Phase 0: the visible spec tests. All must pass.
    Testing,
    TestsFailed,
    /// Phase 0: the hidden differential verify, on a fixed 50k prefix of the
    /// Phase I stream (PLAN-measurement-redesign §4).
    Verifying,
    VerifyFailed,
    /// Distinct from VerifyFailed on purpose: a timeout and a wrong answer are
    /// different bugs, and conflating them under one red X wastes participant
    /// time.
    VerifyTimeout,
    /// Phase I: the bulk run — 3 gated runs, median percentile chain.
    Phase1,
    /// Missed the Phase I deadline or diverged on its stream.
    Phase1Failed,
    /// Phase II: the ladder. A climb that stops at rung k is still `done` —
    /// the level reached IS the result, so phase2 has no failure state of its
    /// own; only a worker error lands in `error`.
    Phase2,
    Done,
    Error,
}

impl SubState {
    /// Terminal states stop the frontend's 2s poll.
    pub fn is_terminal(self) -> bool {
        matches!(
            self,
            SubState::CompileFailed
                | SubState::TestsFailed
                | SubState::VerifyFailed
                | SubState::VerifyTimeout
                | SubState::Phase1Failed
                | SubState::Done
                | SubState::Error
        )
    }
}

/// The harness's exit-code contract. This is an interface between two
/// languages, so it is written down once and referenced, never re-derived.
///
/// Kept in sync with `engine/harness/harness_main.cpp`.
pub mod harness_exit {
    pub const PASSED: i32 = 0;
    pub const FAILED: i32 = 1;
    pub const USAGE: i32 = 2;
    /// Liveness, not correctness.
    pub const TIMEOUT: i32 = 3;
    /// The node, not the submission.
    pub const UNHEALTHY: i32 = 4;
    /// The gate: correct as far as it got, too slow to finish in the budget.
    /// A deadline is never a score (PLAN-measurement-redesign §3).
    pub const DEADLINE: i32 = 5;
}

/// Build the S3 client.
///
/// Credentials, region and endpoint all come from the environment, so the same
/// binary talks to real S3 in the event and to any S3-compatible endpoint (a
/// local MinIO, say) when the platform is being exercised end to end.
///
/// S3_ENDPOINT forces path-style addressing. Virtual-host style needs
/// bucket.host DNS, which does not exist for a container on localhost, and the
/// failure it produces is a confusing DNS error rather than an obvious
/// misconfiguration.
pub async fn s3_client() -> aws_sdk_s3::Client {
    let base = aws_config::load_from_env().await;
    let mut builder = aws_sdk_s3::config::Builder::from(&base);

    if let Ok(endpoint) = std::env::var("S3_ENDPOINT") {
        builder = builder.endpoint_url(endpoint).force_path_style(true);
    }
    aws_sdk_s3::Client::from_conf(builder.build())
}
