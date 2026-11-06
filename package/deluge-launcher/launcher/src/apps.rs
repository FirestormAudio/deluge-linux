//! Enumerate runnable apps in the SD card's app folder. The card's
//! `LINUX/APPS/` folder is reached at `/sd/LINUX/APPS` at runtime, since the
//! appliance init mounts the SD at `/sd`.

use std::path::{Path, PathBuf};

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct AppEntry {
    pub path: PathBuf,
    pub display_name: String,
}

/// The app directory: `$LAUNCHER_APPS_DIR` if set, else `/sd/LINUX/APPS`
/// (the card's `LINUX/APPS/` folder under the `/sd` SD mountpoint).
pub fn apps_dir() -> PathBuf {
    std::env::var_os("LAUNCHER_APPS_DIR")
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from("/sd/LINUX/APPS"))
}

/// True if `path` starts with an ELF32 little-endian header for an ARM
/// (`EM_ARM`) executable or PIE (`ET_EXEC` / `ET_DYN`). Any read error is false.
pub fn is_arm_elf(path: &Path) -> bool {
    use std::io::Read;
    let mut h = [0u8; 20];
    let ok = std::fs::File::open(path).and_then(|mut f| f.read_exact(&mut h)).is_ok();
    if !ok || h[..4] != [0x7f, b'E', b'L', b'F'] || h[4] != 1 || h[5] != 1 {
        return false;
    }
    let e_type = u16::from_le_bytes([h[16], h[17]]);
    let e_machine = u16::from_le_bytes([h[18], h[19]]);
    matches!(e_type, 2 | 3) && e_machine == 40
}

/// List the ARM executables in `dir` (see [`is_arm_elf`]), skipping
/// dotfiles, sorted by display name.
/// A missing or unreadable directory yields an empty list.
pub fn scan(dir: &Path) -> Vec<AppEntry> {
    let mut out = Vec::new();
    let Ok(rd) = std::fs::read_dir(dir) else {
        return out;
    };
    for entry in rd.flatten() {
        let path = entry.path();
        let Ok(meta) = entry.metadata() else { continue };
        if !meta.is_file() {
            continue;
        }
        let Some(stem) = path.file_name().and_then(|s| s.to_str()) else {
            continue;
        };
        // The card is vfat: every file shows as 0755, so the exec bit says
        // nothing. List only ARM executables, and skip dotfiles (macOS `._*`,
        // `.DS_Store`).
        if stem.starts_with('.') || !is_arm_elf(&path) {
            continue;
        }
        out.push(AppEntry {
            path: path.clone(),
            display_name: stem.to_uppercase(),
        });
    }
    out.sort_by(|a, b| a.display_name.cmp(&b.display_name));
    out
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::os::unix::fs::PermissionsExt;

    /// A minimal ELF32 little-endian header (enough for `is_arm_elf`).
    fn elf_header(e_type: u16, e_machine: u16) -> Vec<u8> {
        let mut h = vec![0x7f, b'E', b'L', b'F', 1, 1, 1, 0];
        h.resize(16, 0);
        h.extend_from_slice(&e_type.to_le_bytes());
        h.extend_from_slice(&e_machine.to_le_bytes());
        h.resize(52, 0);
        h
    }

    fn write_file(dir: &Path, name: &str, bytes: &[u8]) {
        let p = dir.join(name);
        std::fs::write(&p, bytes).unwrap();
        std::fs::set_permissions(&p, std::fs::Permissions::from_mode(0o755)).unwrap();
    }

    /// An ARM executable, as the SD card shows it (vfat: every file is 0755).
    fn write_exec(dir: &Path, name: &str) {
        write_file(dir, name, &elf_header(2, 40));
    }

    fn tmpdir() -> PathBuf {
        use std::sync::atomic::{AtomicU64, Ordering};
        static N: AtomicU64 = AtomicU64::new(0);
        let uniq = format!(
            "{}-{}-{}",
            std::process::id(),
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .unwrap()
                .as_nanos(),
            N.fetch_add(1, Ordering::Relaxed),
        );
        let base = std::env::temp_dir().join(format!("launcher-apps-{uniq}"));
        let _ = std::fs::remove_dir_all(&base);
        std::fs::create_dir_all(&base).unwrap();
        base
    }

    #[test]
    fn lists_executables_uppercased_and_sorted() {
        let d = tmpdir();
        write_exec(&d, "spark");
        write_exec(&d, "wren");
        let apps = scan(&d);
        assert_eq!(apps.len(), 2);
        assert_eq!(apps[0].display_name, "SPARK");
        assert_eq!(apps[1].display_name, "WREN");
        assert_eq!(apps[0].path, d.join("spark"));
    }

    #[test]
    fn skips_non_executable_and_non_files() {
        let d = tmpdir();
        write_exec(&d, "runnable");
        std::fs::write(d.join("readme.txt"), b"hi").unwrap(); // not executable
        std::fs::create_dir(d.join("subdir")).unwrap(); // not a file
        let apps = scan(&d);
        assert_eq!(apps.len(), 1);
        assert_eq!(apps[0].display_name, "RUNNABLE");
    }

    #[test]
    fn lists_only_arm_elf() {
        let d = tmpdir();
        write_exec(&d, "arm");
        write_file(&d, "x86", &elf_header(2, 62));
        write_file(&d, "script", b"#!/bin/sh\necho hi\n");
        write_file(&d, "readme.txt", b"hello");
        let names: Vec<_> = scan(&d).into_iter().map(|e| e.display_name).collect();
        assert_eq!(names, vec!["ARM"]);
    }

    #[test]
    fn pie_is_listed() {
        let d = tmpdir();
        write_file(&d, "pie", &elf_header(3, 40));
        assert_eq!(scan(&d).len(), 1);
    }

    #[test]
    fn skips_dotfiles() {
        let d = tmpdir();
        write_exec(&d, "._spark");
        write_exec(&d, ".DS_Store");
        write_exec(&d, "spark");
        let names: Vec<_> = scan(&d).into_iter().map(|e| e.display_name).collect();
        assert_eq!(names, vec!["SPARK"]);
    }

    #[test]
    fn is_arm_elf_rejects_short_and_missing_files() {
        let d = tmpdir();
        write_file(&d, "short", &[0x7f, b'E', b'L', b'F']);
        assert!(!is_arm_elf(&d.join("short")));
        assert!(!is_arm_elf(&d.join("absent")));
    }

    #[test]
    fn missing_dir_yields_empty() {
        assert!(scan(Path::new("/no/such/dir/nowhere")).is_empty());
    }

    #[test]
    fn default_apps_dir_is_under_sd_mount() {
        // The SD is mounted at /sd by the appliance init, so the card's
        // LINUX/APPS/ folder lives at /sd/LINUX/APPS at runtime. Only assert
        // the shipped default when no override is set.
        if std::env::var_os("LAUNCHER_APPS_DIR").is_none() {
            assert_eq!(apps_dir(), PathBuf::from("/sd/LINUX/APPS"));
        }
    }
}
