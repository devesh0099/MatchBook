//! me-platform API.
//!
//! JSON only — Next.js owns every pixel, this owns state and the queue.
//!
//! Two listeners: the public one, and an operator router bound to 127.0.0.1
//! reached over an SSH tunnel. There is no authentication code in this binary,
//! deliberately: 18 known participants in a supervised room pick a handle from
//! a preloaded roster, and the operator surface is simply not reachable from
//! the network.
//!
//! A note on sqlx: queries here are runtime-checked (`query_as`) rather than
//! compile-time-checked (`query_as!`). The macros need either a live database
//! or a checked-in offline cache AT BUILD TIME, and the API image is built on
//! the web node from a clean checkout. Trading the compile-time check for a
//! build that cannot break on a database that is not up yet is the right side
//! of that bargain for a schema this small; row decoding is still typed through
//! FromRow.

mod admin;
mod janitor;
mod routes;
mod state;

use anyhow::{Context, Result};
use sqlx::postgres::PgPoolOptions;
use std::sync::Arc;
use tower_http::trace::TraceLayer;

use state::{AppState, Storage};

#[tokio::main]
async fn main() -> Result<()> {
    tracing_subscriber::fmt()
        .with_env_filter(
            tracing_subscriber::EnvFilter::try_from_default_env()
                .unwrap_or_else(|_| "info,sqlx=warn".into()),
        )
        .init();

    let database_url = std::env::var("DATABASE_URL")
        .context("DATABASE_URL must be set (postgres://user:pass@host/db)")?;
    let bind = std::env::var("BIND_ADDR").unwrap_or_else(|_| "0.0.0.0:8080".into());
    let admin_bind = std::env::var("ADMIN_BIND_ADDR").unwrap_or_else(|_| "127.0.0.1:8081".into());

    let db = PgPoolOptions::new()
        .max_connections(16)
        .connect(&database_url)
        .await
        .context("connecting to Postgres")?;

    // Redis is a disposable read model for the leaderboard, never a queue and
    // never a source of truth. If it is down the API still serves, straight
    // from the Postgres view.
    let redis = match std::env::var("REDIS_URL") {
        Ok(url) => match redis::Client::open(url) {
            Ok(c) => Some(c),
            Err(e) => {
                tracing::warn!("redis unavailable, serving leaderboard from Postgres: {e}");
                None
            }
        },
        Err(_) => None,
    };

    let aws = aws_config::load_from_env().await;
    let s3 = Arc::new(Storage {
        client: aws_sdk_s3::Client::new(&aws),
        bucket: std::env::var("S3_BUCKET").unwrap_or_else(|_| "me-platform-artifacts".into()),
    });

    let app_state = AppState { db: db.clone(), redis, s3 };

    janitor::spawn(db.clone());

    let public = routes::router(app_state.clone()).layer(TraceLayer::new_for_http());
    let operator = admin::router(app_state).layer(TraceLayer::new_for_http());

    let public_listener = tokio::net::TcpListener::bind(&bind).await.context("binding public port")?;
    let admin_listener = tokio::net::TcpListener::bind(&admin_bind)
        .await
        .context("binding admin port")?;

    tracing::info!("api listening on {bind}, operator router on {admin_bind} (loopback only)");

    // Either listener going down takes the process with it: a half-serving API
    // is harder to notice than a dead one, and systemd will restart it.
    let public_srv = tokio::spawn(async move { axum::serve(public_listener, public).await });
    let admin_srv = tokio::spawn(async move { axum::serve(admin_listener, operator).await });

    tokio::select! {
        r = public_srv => r.context("public server task")?.context("public server")?,
        r = admin_srv => r.context("admin server task")?.context("admin server")?,
    }
    Ok(())
}
