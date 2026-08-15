//! ioi/isolate integration.
//!
//! The threat model is not LeetCode's: 18 identifiable participants in a
//! supervised room, on nodes torn down at 6pm, holding nothing valuable.
//! Malicious attack is unlikely. ACCIDENTS ARE NEAR-CERTAIN — infinite loops,
//! runaway allocation, stack-blowing recursion, at least one segfault leaving a
//! zombie on the pinned core. Every one of those stalls the queue for everyone,
//! and a dead event at hour three is the actual risk being managed here.
//!
//! Flag names shifted between isolate's cgroup v1 and v2 support, so anything
//! surprising here should be checked against `man isolate` on the target node
//! rather than assumed.

use anyhow::{bail, Context, Result};
use std::os::unix::io::AsRawFd;
use std::os::unix::process::CommandExt;
use std::path::PathBuf;
use std::process::Stdio;
use tokio::process::Command;

/// The descriptor number the hidden stream arrives on inside the box. The
/// harness is told `--stream-fd 9`; the submission never learns a path.
pub const STREAM_FD: i32 = 9;

pub struct Box_ {
    pub id: u32,
    pub root: PathBuf,
}

#[derive(Debug, Default)]
pub struct RunOutcome {
    pub exit_code: i32,
    pub stdout: String,
    pub stderr: String,
    /// isolate's own verdict: why the process died, if it did.
    pub status: Option<String>,
    pub time_s: Option<f64>,
    pub wall_time_s: Option<f64>,
    pub max_rss_kb: Option<u64>,
}

impl RunOutcome {
    pub fn timed_out(&self) -> bool {
        matches!(self.status.as_deref(), Some("TO"))
    }
    /// Died on a signal — segfault, OOM kill, abort.
    ///
    /// Only SG counts. isolate reports RE for ANY non-zero exit status, and a
    /// failed verification exits 1 by design, so treating RE as a crash would
    /// tell every participant with a wrong answer that their engine had
    /// segfaulted.
    pub fn crashed(&self) -> bool {
        matches!(self.status.as_deref(), Some("SG"))
    }

    /// isolate's RE covers ordinary non-zero exits too, so a crash is only
    /// implied when the code is not one the harness defines.
    pub fn unexpected_exit(&self, known: &[i32]) -> bool {
        matches!(self.status.as_deref(), Some("RE")) && !known.contains(&self.exit_code)
    }
}

#[derive(Clone)]
pub struct Sandbox {
    pub binary: String,
}

impl Sandbox {
    pub fn new() -> Self {
        Self {
            binary: std::env::var("ISOLATE_BIN").unwrap_or_else(|_| "isolate".into()),
        }
    }

    /// Each --box-id runs as a different unprivileged UID, so boxes cannot
    /// touch each other's files even if a mount is misconfigured.
    pub async fn init(&self, id: u32) -> Result<Box_> {
        let out = Command::new(&self.binary)
            .args(["--cg", "--box-id", &id.to_string(), "--init"])
            .output()
            .await
            .context("running isolate --init")?;
        if !out.status.success() {
            bail!(
                "isolate --init failed for box {id}: {}",
                String::from_utf8_lossy(&out.stderr)
            );
        }
        let root = PathBuf::from(String::from_utf8_lossy(&out.stdout).trim()).join("box");
        Ok(Box_ { id, root })
    }

    /// Synchronous on purpose: this runs from Drop, and a leaked box wedges
    /// that box id for the rest of the event. It takes milliseconds.
    pub fn cleanup_blocking(&self, id: u32) {
        let _ = std::process::Command::new(&self.binary)
            .args(["--cg", "--box-id", &id.to_string(), "--cleanup"])
            .stdout(Stdio::null())
            .stderr(Stdio::null())
            .status();
    }

    pub async fn run(&self, b: &Box_, opts: &RunOpts, argv: &[&str]) -> Result<RunOutcome> {
        self.run_with_stream(b, opts, argv, None).await
    }

    /// Run a command inside the box, optionally handing it the hidden stream on
    /// an inherited descriptor.
    ///
    /// This is the whole point of plan section 10's fd design: the stream file
    /// lives OUTSIDE the box, owned by the worker, and only its descriptor
    /// crosses in. Writing the stream into the box directory instead would hand
    /// it straight to the submission — that directory is the submission's own,
    /// and a participant who reads the stream and hardcodes outputs invalidates
    /// the leaderboard.
    pub async fn run_with_stream(
        &self,
        b: &Box_,
        opts: &RunOpts,
        argv: &[&str],
        stream: Option<std::fs::File>,
    ) -> Result<RunOutcome> {
        let meta_path = std::env::temp_dir().join(format!("isolate-meta-{}", b.id));
        // taskset wraps isolate rather than living inside the box: affinity is
        // inherited across exec, and the box has no taskset binary of its own.
        let mut cmd = match &opts.cpus {
            Some(cpus) => {
                let mut c = std::process::Command::new("taskset");
                c.arg("-c").arg(cpus).arg(&self.binary);
                c
            }
            None => std::process::Command::new(&self.binary),
        };
        cmd.arg("--cg")
            .args(["--box-id", &b.id.to_string()])
            .args(["--meta", meta_path.to_str().unwrap()])
            .args(["--wall-time", &opts.wall_time_s.to_string()])
            .args(["--time", &opts.cpu_time_s.to_string()])
            .args(["--cg-mem", &opts.memory_kb.to_string()])
            // MUST use the attached form. --processes takes an OPTIONAL
            // argument, so `--processes 1` leaves "1" unconsumed, getopt stops
            // at the first non-option, and isolate tries to exec "1".
            // Verified against isolate 2.6's option table: it is the only
            // option declared with an optional argument.
            .arg(format!("--processes={}", opts.max_processes));

        if !opts.network {
            // isolate gives no network by default; stated explicitly because
            // the compile step is the one that would otherwise be tempted.
        }
        for (k, v) in &opts.env {
            cmd.arg(format!("--env={k}={v}"));
        }
        for dir in &opts.binds {
            cmd.arg(format!("--dir={dir}"));
        }
        if opts.inherit_fds {
            cmd.arg("--inherit-fds");
        }

        cmd.arg("--run").arg("--");
        for a in argv {
            cmd.arg(a);
        }
        cmd.stdout(Stdio::piped()).stderr(Stdio::piped());

        if let Some(file) = &stream {
            // dup2 onto a fixed descriptor and clear FD_CLOEXEC, so it survives
            // exec into isolate and then into the harness.
            let raw = file.as_raw_fd();
            unsafe {
                cmd.pre_exec(move || {
                    if libc_dup2(raw, STREAM_FD) < 0 {
                        return Err(std::io::Error::last_os_error());
                    }
                    Ok(())
                });
            }
        }

        // std::process::Command is used rather than tokio's so pre_exec is
        // available; the call is short and runs on a blocking thread.
        let out = tokio::task::spawn_blocking(move || cmd.output())
            .await
            .context("joining isolate task")?
            .context("running isolate --run")?;
        drop(stream);
        let mut outcome = RunOutcome {
            exit_code: out.status.code().unwrap_or(-1),
            stdout: String::from_utf8_lossy(&out.stdout).into_owned(),
            stderr: String::from_utf8_lossy(&out.stderr).into_owned(),
            ..Default::default()
        };

        // isolate hands back resource accounting directly, including WHY the
        // process died — no scraping cgroup files.
        if let Ok(meta) = tokio::fs::read_to_string(&meta_path).await {
            for line in meta.lines() {
                let Some((k, v)) = line.split_once(':') else { continue };
                match k {
                    "status" => outcome.status = Some(v.to_string()),
                    "time" => outcome.time_s = v.parse().ok(),
                    "time-wall" => outcome.wall_time_s = v.parse().ok(),
                    "max-rss" => outcome.max_rss_kb = v.parse().ok(),
                    "exitcode" => {
                        if let Ok(c) = v.parse() {
                            outcome.exit_code = c;
                        }
                    }
                    _ => {}
                }
            }
            let _ = tokio::fs::remove_file(&meta_path).await;
        }
        Ok(outcome)
    }
}

/// dup2 without pulling in the libc crate for one call.
fn libc_dup2(old: i32, new: i32) -> i32 {
    extern "C" {
        fn dup2(oldfd: i32, newfd: i32) -> i32;
    }
    unsafe { dup2(old, new) }
}

pub struct RunOpts {
    pub wall_time_s: f64,
    pub cpu_time_s: f64,
    pub memory_kb: u64,
    pub max_processes: u32,
    pub network: bool,
    pub inherit_fds: bool,
    pub env: Vec<(String, String)>,
    pub binds: Vec<String>,
    /// CPU list handed to `taskset -c` AROUND the isolate invocation, so the
    /// affinity is inherited through isolate into the harness and the
    /// submission (the same mechanism the old bench node used and
    /// bench-hygiene.sh asserts). On an isolated-core box, measured runs pin
    /// to the isolated core and compiles to the spare cores; None (dev) runs
    /// unpinned.
    pub cpus: Option<String>,
}

impl Default for RunOpts {
    fn default() -> Self {
        Self {
            wall_time_s: 60.0,
            cpu_time_s: 50.0,
            memory_kb: 2_000_000,
            // A fork bomb is one of the near-certain accidents, not a
            // hypothetical.
            max_processes: 1,
            network: false,
            inherit_fds: false,
            env: Vec::new(),
            binds: Vec::new(),
            cpus: None,
        }
    }
}

impl RunOpts {
    /// The compile step is the underrated one. The preprocessor is a capable
    /// interpreter: #include "/proc/self/environ", #embed on the stream file,
    /// preprocessor bombs. So compiles get no network and a read-only mount of
    /// just what they need.
    pub fn for_compile() -> Self {
        Self {
            wall_time_s: 60.0,
            cpu_time_s: 50.0,
            memory_kb: 4_000_000,
            max_processes: 16, // g++ forks cc1plus and as
            ..Default::default()
        }
    }

    pub fn for_verify(wall_time_s: f64) -> Self {
        Self {
            wall_time_s,
            cpu_time_s: wall_time_s,
            memory_kb: 4_000_000,
            // Threads are blocked: thread-based gaming of the wall clock is not
            // a strategy here.
            max_processes: 1,
            inherit_fds: true,
            ..Default::default()
        }
    }

    pub fn for_bench(wall_time_s: f64) -> Self {
        Self {
            wall_time_s,
            cpu_time_s: wall_time_s,
            memory_kb: 8_000_000,
            max_processes: 1,
            inherit_fds: true,
            ..Default::default()
        }
    }
}

/// Copy a file into the box, owned by the box UID.
pub async fn place(b: &Box_, name: &str, bytes: &[u8]) -> Result<PathBuf> {
    let dest = b.root.join(name);
    tokio::fs::write(&dest, bytes)
        .await
        .with_context(|| format!("writing {} into box {}", name, b.id))?;
    Ok(dest)
}

pub async fn read_out(b: &Box_, name: &str) -> Result<Vec<u8>> {
    Ok(tokio::fs::read(b.root.join(name)).await?)
}

