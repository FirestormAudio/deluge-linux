//! Launch and supervise a child application.
//!
//! The child is put in its own session/process group (`setsid`) so the kill
//! chord can tear down the whole group: SIGTERM first, SIGKILL after a grace
//! period (see [`Stopper`]).

use std::io;
use std::os::unix::process::{CommandExt, ExitStatusExt};
use std::path::Path;
use std::process::{Child, Command, ExitStatus, Stdio};

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Exit {
    Clean,
    Nonzero(i32),
    Signalled(i32),
}

pub fn classify(status: ExitStatus) -> Exit {
    if let Some(code) = status.code() {
        if code == 0 {
            Exit::Clean
        } else {
            Exit::Nonzero(code)
        }
    } else if let Some(sig) = status.signal() {
        Exit::Signalled(sig)
    } else {
        Exit::Nonzero(-1)
    }
}

pub struct Launched {
    pub pid: i32,
    pub child: Child,
}

/// Start `path` in its own session (so the whole process group can be
/// signalled) with stdout and stderr going to `log` and stdin from /dev/null.
pub fn launch(path: &Path, log: Stdio) -> io::Result<Launched> {
    let mut cmd = Command::new(path);
    cmd.stdin(Stdio::null());
    // stdout goes to the log; pre_exec dups it onto stderr so both share it.
    cmd.stdout(log);
    // SAFETY: setsid and dup2 are async-signal-safe and touch no Rust heap state.
    unsafe {
        cmd.pre_exec(|| {
            if libc::setsid() < 0 {
                return Err(io::Error::last_os_error());
            }
            if libc::dup2(1, 2) < 0 {
                return Err(io::Error::last_os_error());
            }
            Ok(())
        });
    }
    let child = cmd.spawn()?;
    let pid = child.id() as i32; // setsid makes pgid == pid
    Ok(Launched { pid, child })
}

/// Send `sig` to the whole process group led by `pid`. ESRCH (already gone)
/// is ignored.
pub fn signal_group(pid: i32, sig: i32) {
    unsafe {
        libc::kill(-pid, sig);
    }
}

/// Graceful stop: SIGTERM on request, SIGKILL once `grace_ms` has passed.
/// Pure state driven by an external monotonic clock.
pub struct Stopper {
    grace_ms: u64,
    requested_at: Option<u64>,
    killed: bool,
}

impl Stopper {
    pub fn new(grace_ms: u64) -> Self {
        Self { grace_ms, requested_at: None, killed: false }
    }

    /// First call returns SIGTERM; later calls (until `reset`) return None.
    pub fn request(&mut self, now_ms: u64) -> Option<i32> {
        if self.requested_at.is_some() {
            return None;
        }
        self.requested_at = Some(now_ms);
        Some(libc::SIGTERM)
    }

    /// SIGKILL once, when the grace period after a request has run out.
    pub fn poll(&mut self, now_ms: u64) -> Option<i32> {
        match self.requested_at {
            Some(t) if !self.killed && now_ms >= t + self.grace_ms => {
                self.killed = true;
                Some(libc::SIGKILL)
            }
            _ => None,
        }
    }

    pub fn reset(&mut self) {
        self.requested_at = None;
        self.killed = false;
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::process::Stdio;
    use std::time::Duration;

    fn sh(script: &str) -> Launched {
        sh_with(script, None)
    }

    /// Write `script` to a fresh file and launch it, logging to `log` if given.
    /// Retries on ETXTBSY: another test thread may fork while the file is
    /// still open for writing.
    fn sh_with(script: &str, log: Option<&Path>) -> Launched {
        use std::os::unix::fs::PermissionsExt;
        use std::sync::atomic::{AtomicU64, Ordering};
        static N: AtomicU64 = AtomicU64::new(0);
        let path = std::env::temp_dir().join(format!(
            "launcher-sv-{}-{}.sh",
            std::process::id(),
            N.fetch_add(1, Ordering::Relaxed)
        ));
        std::fs::write(&path, format!("#!/bin/sh\n{script}\n")).unwrap();
        std::fs::set_permissions(&path, std::fs::Permissions::from_mode(0o755)).unwrap();
        for _ in 0..100 {
            let out = match log {
                Some(p) => Stdio::from(std::fs::File::create(p).unwrap()),
                None => Stdio::null(),
            };
            match launch(&path, out) {
                Ok(launched) => return launched,
                Err(e) if e.raw_os_error() == Some(libc::ETXTBSY) => {
                    std::thread::sleep(Duration::from_millis(10));
                }
                Err(e) => panic!("launch failed: {e}"),
            }
        }
        panic!("launch kept failing with ETXTBSY");
    }

    fn alive(pid: i32) -> bool {
        unsafe { libc::kill(pid, 0) == 0 }
    }

    #[test]
    fn clean_exit_classifies_clean() {
        let mut l = launch(Path::new("/bin/true"), Stdio::null()).unwrap();
        assert_eq!(classify(l.child.wait().unwrap()), Exit::Clean);
    }

    #[test]
    fn nonzero_exit_classifies_nonzero() {
        let mut l = launch(Path::new("/bin/false"), Stdio::null()).unwrap();
        assert_eq!(classify(l.child.wait().unwrap()), Exit::Nonzero(1));
    }

    #[test]
    fn launching_missing_binary_errors() {
        assert!(launch(Path::new("/no/such/binary/zzz"), Stdio::null()).is_err());
    }

    #[test]
    fn stopper_terms_then_kills() {
        let mut s = Stopper::new(2000);
        assert_eq!(s.poll(0), None);
        assert_eq!(s.request(0), Some(libc::SIGTERM));
        assert_eq!(s.request(10), None);
        assert_eq!(s.poll(1999), None);
        assert_eq!(s.poll(2000), Some(libc::SIGKILL));
        assert_eq!(s.poll(3000), None);
    }

    #[test]
    fn stopper_reset_clears() {
        let mut s = Stopper::new(2000);
        s.request(0);
        s.reset();
        assert_eq!(s.poll(5000), None);
        assert_eq!(s.request(6000), Some(libc::SIGTERM));
    }

    #[test]
    fn stubborn_child_is_killed_after_grace() {
        let mut l = sh("trap '' TERM\nwhile :; do sleep 1; done");
        std::thread::sleep(Duration::from_millis(100));
        signal_group(l.pid, libc::SIGTERM);
        std::thread::sleep(Duration::from_millis(200));
        assert!(l.child.try_wait().unwrap().is_none(), "ignored SIGTERM, still running");
        signal_group(l.pid, libc::SIGKILL);
        assert_eq!(classify(l.child.wait().unwrap()), Exit::Signalled(libc::SIGKILL));
    }

    #[test]
    fn group_kill_reaps_grandchildren() {
        let pidfile = std::env::temp_dir().join(format!("launcher-gc-{}", std::process::id()));
        let mut l = sh(&format!("sleep 60 &\necho $! > {}\nwait", pidfile.display()));
        let mut gc = 0;
        for _ in 0..50 {
            if let Some(p) = std::fs::read_to_string(&pidfile).ok().and_then(|s| s.trim().parse().ok()) {
                gc = p;
                break;
            }
            std::thread::sleep(Duration::from_millis(10));
        }
        assert!(gc > 0 && alive(gc));
        signal_group(l.pid, libc::SIGKILL);
        l.child.wait().unwrap();
        let mut gone = false;
        for _ in 0..50 {
            // The grandchild is reparented; once killed it may linger as a
            // zombie of init, so check its state rather than kill(0).
            let stat = std::fs::read_to_string(format!("/proc/{gc}/stat")).unwrap_or_default();
            if stat.is_empty() || stat.contains(") Z") {
                gone = true;
                break;
            }
            std::thread::sleep(Duration::from_millis(10));
        }
        assert!(gone, "grandchild {gc} survived the group kill");
    }

    #[test]
    fn launch_writes_output_to_log() {
        let log = std::env::temp_dir().join(format!("launcher-out-{}.log", std::process::id()));
        let mut l = sh_with("echo out; echo err >&2", Some(&log));
        l.child.wait().unwrap();
        let text = std::fs::read_to_string(&log).unwrap();
        assert!(text.contains("out") && text.contains("err"), "{text:?}");
    }
}
