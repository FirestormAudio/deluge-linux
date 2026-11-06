//! Per-app log files: each run's stdout/stderr go to `LINUX/log/<app>.log` on
//! the card, beside `LINUX/APPS/`, truncated at the start of the run.

use std::path::{Path, PathBuf};
use std::process::Stdio;

/// `<apps_dir>/../log/<app file name>.log`.
pub fn log_path(apps_dir: &Path, app: &Path) -> PathBuf {
    let base = apps_dir.parent().unwrap_or(apps_dir);
    let name = app.file_name().map(|n| n.to_string_lossy()).unwrap_or_default();
    base.join("log").join(format!("{name}.log"))
}

/// Open (create and truncate) the log for one run. Any failure, such as no
/// card or a read-only card, gives `/dev/null` so the app still launches.
pub fn open_log(path: &Path) -> Stdio {
    let open = || -> std::io::Result<std::fs::File> {
        if let Some(dir) = path.parent() {
            std::fs::create_dir_all(dir)?;
        }
        std::fs::File::create(path)
    };
    match open() {
        Ok(f) => Stdio::from(f),
        Err(_) => Stdio::null(),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::os::unix::fs::PermissionsExt;

    fn tmpdir(tag: &str) -> PathBuf {
        let d = std::env::temp_dir().join(format!(
            "launcher-applog-{tag}-{}-{}",
            std::process::id(),
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .unwrap()
                .as_nanos()
        ));
        std::fs::create_dir_all(&d).unwrap();
        d
    }

    fn run_echo(log: Stdio) -> std::process::ExitStatus {
        std::process::Command::new("/bin/sh")
            .args(["-c", "echo new"])
            .stdout(log)
            .status()
            .unwrap()
    }

    #[test]
    fn log_path_is_sibling_log_dir() {
        assert_eq!(
            log_path(Path::new("/sd/LINUX/APPS"), Path::new("/sd/LINUX/APPS/spark")),
            PathBuf::from("/sd/LINUX/log/spark.log")
        );
    }

    #[test]
    fn open_log_creates_dir_and_truncates() {
        let d = tmpdir("trunc");
        let path = d.join("log/spark.log");
        std::fs::create_dir_all(path.parent().unwrap()).unwrap();
        std::fs::write(&path, "old old old\n").unwrap();
        assert!(run_echo(open_log(&path)).success());
        assert_eq!(std::fs::read_to_string(&path).unwrap(), "new\n");

        let fresh = d.join("newdir/app.log");
        assert!(run_echo(open_log(&fresh)).success());
        assert_eq!(std::fs::read_to_string(&fresh).unwrap(), "new\n");
    }

    #[test]
    fn log_dir_failure_falls_back_to_null() {
        let d = tmpdir("ro");
        std::fs::set_permissions(&d, std::fs::Permissions::from_mode(0o555)).unwrap();
        let path = d.join("log/app.log");
        // Root ignores directory permissions; only assert the fallback when the
        // directory really is unwritable.
        let writable = std::fs::create_dir(d.join("probe")).is_ok();
        assert!(run_echo(open_log(&path)).success());
        if !writable {
            assert!(!path.exists());
        }
        std::fs::set_permissions(&d, std::fs::Permissions::from_mode(0o755)).unwrap();
    }
}
