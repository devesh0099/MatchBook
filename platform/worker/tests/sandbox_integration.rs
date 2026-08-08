//! Integration tests against a REAL isolate.
//!
//! These need isolate installed (see ops/install-isolate.sh) and a free box id,
//! so they are ignored by default:
//!
//!     cargo test -p worker -- --ignored --test-threads=1
//!
//! Every case here exists because the behaviour it pins was got wrong once
//! while writing the worker against isolate's documentation alone.

use std::io::Write;
use std::process::Command;

/// Box id kept away from the low ids a running pool would use.
const BOX: u32 = 31;

fn isolate_available() -> bool {
    Command::new("isolate")
        .arg("--version")
        .output()
        .map(|o| o.status.success())
        .unwrap_or(false)
}

/// Holds the box open for the duration of a test; cleanup runs on drop so a
/// failing assertion cannot wedge the box id.
struct Box_;

fn init() -> Box_ {
    let _ = Command::new("isolate")
        .args(["--cg", "--box-id", &BOX.to_string(), "--cleanup"])
        .output();
    let out = Command::new("isolate")
        .args(["--cg", "--box-id", &BOX.to_string(), "--init"])
        .output()
        .expect("isolate --init");
    assert!(out.status.success(), "init: {}", String::from_utf8_lossy(&out.stderr));
    Box_
}

impl Drop for Box_ {
    fn drop(&mut self) {
        let _ = Command::new("isolate")
            .args(["--cg", "--box-id", &BOX.to_string(), "--cleanup"])
            .output();
    }
}

fn meta_path() -> std::path::PathBuf {
    std::env::temp_dir().join(format!("isolate-test-meta-{BOX}"))
}

fn read_meta() -> std::collections::HashMap<String, String> {
    let mut m = std::collections::HashMap::new();
    if let Ok(s) = std::fs::read_to_string(meta_path()) {
        for line in s.lines() {
            if let Some((k, v)) = line.split_once(':') {
                m.insert(k.to_string(), v.to_string());
            }
        }
    }
    m
}

fn run(args: &[&str], processes: u32) -> (std::process::Output, std::collections::HashMap<String, String>) {
    let _ = std::fs::remove_file(meta_path());
    let mut cmd = Command::new("isolate");
    cmd.arg("--cg")
        .args(["--box-id", &BOX.to_string()])
        .args(["--meta", meta_path().to_str().unwrap()])
        .args(["--wall-time", "20"])
        .args(["--time", "20"])
        .args(["--cg-mem", "500000"])
        // The attached form. See the test below for why.
        .arg(format!("--processes={processes}"))
        .arg("--run")
        .arg("--");
    for a in args {
        cmd.arg(a);
    }
    let out = cmd.output().expect("isolate --run");
    (out, read_meta())
}

/// --processes takes an OPTIONAL argument, so the separated form leaves the
/// number unconsumed; getopt then stops at the first non-option and isolate
/// tries to exec it. This produced `execve("1"): No such file or directory`.
#[test]
#[ignore]
fn processes_flag_must_use_the_attached_form() {
    if !isolate_available() {
        eprintln!("isolate not installed, skipping");
        return;
    }
    let _b = init();

    let separated = Command::new("isolate")
        .args(["--cg", "--box-id", &BOX.to_string()])
        .args(["--processes", "1"])
        .args(["--run", "--", "/bin/echo", "hello"])
        .output()
        .expect("run");
    let sep_err = String::from_utf8_lossy(&separated.stderr).to_string();
    assert!(
        !separated.status.success() || sep_err.contains("execve"),
        "the separated form is expected to misparse; if isolate ever fixes this, \
         relax the worker's requirement rather than keeping a workaround"
    );

    let (out, _) = run(&["/bin/echo", "hello"], 1);
    assert!(out.status.success(), "attached form must work: {}", String::from_utf8_lossy(&out.stderr));
    assert_eq!(String::from_utf8_lossy(&out.stdout).trim(), "hello");
}

/// isolate reports status RE for ANY non-zero exit, and a failed verification
/// exits 1 by design. Treating RE as a crash told every participant with a
/// wrong answer that their engine had segfaulted.
#[test]
#[ignore]
fn ordinary_nonzero_exit_is_not_a_crash() {
    if !isolate_available() {
        return;
    }
    let _b = init();

    let (_out, meta) = run(&["/bin/sh", "-c", "exit 1"], 1);
    assert_eq!(meta.get("status").map(String::as_str), Some("RE"));
    assert_eq!(meta.get("exitcode").map(String::as_str), Some("1"));

    // A real crash is SG, and only SG.
    let (_out, meta) = run(&["/bin/sh", "-c", "kill -SEGV $$"], 2);
    assert_eq!(meta.get("status").map(String::as_str), Some("SG"));
}

/// The hidden stream must reach the sandboxed harness on an inherited
/// descriptor, never as a file inside the box — the box directory is the
/// submission's own.
#[test]
#[ignore]
fn stream_arrives_on_an_inherited_fd_and_not_as_a_path() {
    if !isolate_available() {
        return;
    }
    let _b = init();

    let secret = std::env::temp_dir().join(format!("hidden-{BOX}.bin"));
    {
        let mut f = std::fs::File::create(&secret).unwrap();
        f.write_all(b"MEBENCH1-SECRET-PAYLOAD").unwrap();
    }
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        std::fs::set_permissions(&secret, std::fs::Permissions::from_mode(0o600)).unwrap();
    }

    // The path must not resolve inside the box at all.
    let (out, _) = run(&["/bin/cat", secret.to_str().unwrap()], 2);
    assert!(!out.status.success(), "the box must not be able to open the stream by path");
    assert!(!String::from_utf8_lossy(&out.stdout).contains("SECRET"));

    // But an inherited descriptor gets through.
    let script = "head -c 23 <&9";
    let file = std::fs::File::open(&secret).unwrap();
    let out = {
        use std::os::unix::io::AsRawFd;
        use std::os::unix::process::CommandExt;
        let raw = file.as_raw_fd();
        let mut cmd = Command::new("isolate");
        cmd.arg("--cg")
            .args(["--box-id", &BOX.to_string()])
            .args(["--wall-time", "20"])
            .arg("--processes=4")
            .arg("--inherit-fds")
            .args(["--run", "--", "/bin/sh", "-c", script]);
        unsafe {
            cmd.pre_exec(move || {
                extern "C" {
                    fn dup2(oldfd: i32, newfd: i32) -> i32;
                }
                if dup2(raw, 9) < 0 {
                    return Err(std::io::Error::last_os_error());
                }
                Ok(())
            });
        }
        cmd.output().expect("run with inherited fd")
    };
    assert_eq!(
        String::from_utf8_lossy(&out.stdout).trim(),
        "MEBENCH1-SECRET-PAYLOAD",
        "the harness must receive the stream on fd 9"
    );

    let _ = std::fs::remove_file(&secret);
}

/// Accidents are near-certain, and every one of them stalls the queue for
/// everyone if it is not contained.
#[test]
#[ignore]
fn runaway_submissions_fail_cleanly() {
    if !isolate_available() {
        return;
    }
    let _b = init();

    // Infinite loop -> wall-clock timeout, reported as TO.
    let _ = std::fs::remove_file(meta_path());
    let out = Command::new("isolate")
        .args(["--cg", "--box-id", &BOX.to_string()])
        .args(["--meta", meta_path().to_str().unwrap()])
        .args(["--wall-time", "2", "--time", "2"])
        .arg("--processes=1")
        .args(["--run", "--", "/bin/sh", "-c", "while :; do :; done"])
        .output()
        .expect("run");
    let _ = out;
    assert_eq!(read_meta().get("status").map(String::as_str), Some("TO"));

    // Fork bomb -> contained by the process cap, and nothing survives it.
    let (_out, _meta) = run(&["/bin/sh", "-c", "while :; do /bin/true & done"], 1);
    let survivors = Command::new("pgrep")
        .args(["-c", "-u", "165536"])
        .output()
        .map(|o| String::from_utf8_lossy(&o.stdout).trim().parse::<i32>().unwrap_or(0))
        .unwrap_or(0);
    assert_eq!(survivors, 0, "no process may outlive the box");
}
