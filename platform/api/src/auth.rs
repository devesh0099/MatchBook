//! Authentication (PLAN-measurement-redesign §6, M3).
//!
//! The browser proves identity ONCE — POST /login with the printed
//! username + password — and everything after that is server-side: the
//! session cookie resolves to a participant on every request, and no client
//! ever says who it is again. The `participant_id` that requests used to
//! carry is gone from the client API entirely; the `Auth` extractor is the
//! only source of identity.
//!
//! Mechanics, sized for under 20 users on a one-day event:
//!   - passwords are Argon2id hashes in `participants.credential_hash`,
//!     issued by the operator (ops/issue-credentials.sh) — no signup, no
//!     email, no reset UX;
//!   - the cookie holds a 32-byte random token; the DB stores its sha256, so
//!     a leaked table replays nothing;
//!   - sessions expire a fixed 24h after login, enforced at lookup;
//!   - HttpOnly + SameSite=Lax: page JavaScript cannot read the token, and
//!     cross-site POSTs do not carry it. No CSRF token on top — every
//!     mutating route is same-origin JSON behind Lax, which is proportionate
//!     here.

use argon2::password_hash::{PasswordHash, PasswordHasher, PasswordVerifier, SaltString};
use argon2::Argon2;
use axum::{
    extract::{FromRequestParts, State},
    http::{header, request::Parts, HeaderMap, HeaderValue, StatusCode},
    Json,
};
use serde::Deserialize;
use serde_json::json;
use sha2::{Digest, Sha256};

use crate::state::AppState;

const COOKIE: &str = "mb_session";
const SESSION_HOURS: i32 = 24;

type ApiError = (StatusCode, Json<serde_json::Value>);

fn unauthorized(msg: &str) -> ApiError {
    (StatusCode::UNAUTHORIZED, Json(json!({ "error": msg })))
}

fn internal(e: impl std::fmt::Display) -> ApiError {
    tracing::error!("auth internal error: {e}");
    (StatusCode::INTERNAL_SERVER_ERROR, Json(json!({ "error": "internal error" })))
}

fn token_hash(token: &str) -> String {
    format!("{:x}", Sha256::digest(token.as_bytes()))
}

fn cookie_token(headers: &axum::http::HeaderMap) -> Option<String> {
    let raw = headers.get(header::COOKIE)?.to_str().ok()?;
    raw.split(';').find_map(|part| {
        let (k, v) = part.trim().split_once('=')?;
        (k == COOKIE).then(|| v.to_string())
    })
}

/// The authenticated participant. Add this as a handler argument and the
/// request either carries a live session or is rejected 401 before the
/// handler runs.
pub struct Auth {
    pub participant_id: i32,
    pub handle: String,
    /// Kept so logout can delete exactly this session.
    pub token_hash: String,
}

#[axum::async_trait]
impl FromRequestParts<AppState> for Auth {
    type Rejection = ApiError;

    async fn from_request_parts(parts: &mut Parts, st: &AppState) -> Result<Self, Self::Rejection> {
        let Some(token) = cookie_token(&parts.headers) else {
            return Err(unauthorized("not signed in"));
        };
        let th = token_hash(&token);
        let row: Option<(i32, String)> = sqlx::query_as(
            "SELECT p.id, p.handle FROM sessions s \
             JOIN participants p ON p.id = s.participant_id \
             WHERE s.token_hash = $1 \
             AND s.created_at > now() - make_interval(hours => $2)",
        )
        .bind(&th)
        .bind(SESSION_HOURS)
        .fetch_optional(&st.db)
        .await
        .map_err(internal)?;

        let Some((participant_id, handle)) = row else {
            return Err(unauthorized("session expired — sign in again"));
        };
        // Fire-and-forget liveness stamp; a failed touch must not fail the
        // request it rides on.
        let db = st.db.clone();
        let th2 = th.clone();
        tokio::spawn(async move {
            let _ = sqlx::query("UPDATE sessions SET last_seen = now() WHERE token_hash = $1")
                .bind(th2)
                .execute(&db)
                .await;
        });
        Ok(Auth { participant_id, handle, token_hash: th })
    }
}

// ---------------------------------------------------------------- handlers

#[derive(Deserialize)]
pub struct LoginBody {
    pub handle: String,
    pub password: String,
}

fn session_cookie(value: &str, max_age: i64) -> HeaderMap {
    let mut headers = HeaderMap::new();
    let cookie =
        format!("{COOKIE}={value}; HttpOnly; SameSite=Lax; Path=/; Max-Age={max_age}");
    headers.insert(header::SET_COOKIE, HeaderValue::from_str(&cookie).unwrap());
    headers
}

pub async fn login(
    State(st): State<AppState>,
    Json(body): Json<LoginBody>,
) -> Result<(HeaderMap, Json<serde_json::Value>), ApiError> {
    let row: Option<(i32, Option<String>)> =
        sqlx::query_as("SELECT id, credential_hash FROM participants WHERE handle = $1")
            .bind(body.handle.trim())
            .fetch_optional(&st.db)
            .await
            .map_err(internal)?;

    // One rejection message for every failure mode: an attacker probing the
    // roster learns nothing from the response shape.
    let reject = || unauthorized("wrong handle or password");
    let Some((id, Some(hash))) = row else { return Err(reject()) };
    let parsed = PasswordHash::new(&hash).map_err(internal)?;
    if Argon2::default().verify_password(body.password.as_bytes(), &parsed).is_err() {
        return Err(reject());
    }

    let token = {
        use rand::RngCore;
        let mut bytes = [0u8; 32];
        rand::rngs::OsRng.fill_bytes(&mut bytes);
        bytes.iter().map(|b| format!("{b:02x}")).collect::<String>()
    };
    sqlx::query("INSERT INTO sessions (token_hash, participant_id) VALUES ($1, $2)")
        .bind(token_hash(&token))
        .bind(id)
        .execute(&st.db)
        .await
        .map_err(internal)?;

    Ok((
        session_cookie(&token, (SESSION_HOURS as i64) * 3600),
        Json(json!({ "participant_id": id, "handle": body.handle.trim() })),
    ))
}

pub async fn logout(
    State(st): State<AppState>,
    auth: Auth,
) -> Result<(HeaderMap, Json<serde_json::Value>), ApiError> {
    sqlx::query("DELETE FROM sessions WHERE token_hash = $1")
        .bind(&auth.token_hash)
        .execute(&st.db)
        .await
        .map_err(internal)?;
    Ok((session_cookie("", 0), Json(json!({ "signed_out": true }))))
}

/// "Who am I" — what the frontend renders instead of a localStorage pick.
pub async fn session(auth: Auth) -> Json<serde_json::Value> {
    Json(json!({ "participant_id": auth.participant_id, "handle": auth.handle }))
}

// ---------------------------------------------------------------- issuing

/// Hash a password for storage. Used by the operator route that issues
/// credentials; Argon2id with the crate's defaults.
pub fn hash_password(password: &str) -> anyhow::Result<String> {
    let salt = SaltString::generate(&mut rand::rngs::OsRng);
    Ok(Argon2::default()
        .hash_password(password.as_bytes(), &salt)
        .map_err(|e| anyhow::anyhow!("hashing: {e}"))?
        .to_string())
}

/// A password a human can type from a printed slip: lowercase groups drawn
/// from an alphabet with no confusable glyphs (no 0/o, 1/l/i).
pub fn generate_password() -> String {
    use rand::Rng;
    const ALPHABET: &[u8] = b"abcdefghjkmnpqrstuvwxyz23456789";
    let mut rng = rand::rngs::OsRng;
    (0..3)
        .map(|_| {
            (0..4)
                .map(|_| ALPHABET[rng.gen_range(0..ALPHABET.len())] as char)
                .collect::<String>()
        })
        .collect::<Vec<_>>()
        .join("-")
}
