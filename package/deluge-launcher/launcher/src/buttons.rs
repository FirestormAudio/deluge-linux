//! Watch the front-panel buttons straight from evdev while an app runs.
//!
//! While an app owns the instrument the launcher holds no libdeluge handle, so
//! it reads the "Deluge Buttons" input device itself, without grabbing it (the
//! app sees every press too). Only the escape chord is acted on.

use std::os::fd::{AsRawFd, OwnedFd};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::mpsc::Sender;
use std::sync::Arc;
use std::thread::JoinHandle;

/// The input device the `deluge-pic` driver registers for the buttons.
const BUTTONS_NAME: &str = "Deluge Buttons";
const EV_KEY: u16 = 1;
/// `BTN_TRIGGER_HAPPY1`: button id 0.
const BTN_BASE: u16 = 0x2c0;
const N_BUTTONS: u16 = 36;

/// Find the buttons' `/dev/input/eventN` by scanning
/// `<sys_class_input>/event*/device/name` for exactly "Deluge Buttons".
pub fn find_buttons(sys_class_input: &Path) -> Option<PathBuf> {
    let mut found: Vec<String> = std::fs::read_dir(sys_class_input)
        .ok()?
        .flatten()
        .filter_map(|e| {
            let ev = e.file_name().to_str()?.to_string();
            if !ev.starts_with("event") {
                return None;
            }
            let name = std::fs::read_to_string(e.path().join("device/name")).ok()?;
            (name.trim_end() == BUTTONS_NAME).then_some(ev)
        })
        .collect();
    found.sort();
    found.first().map(|ev| Path::new("/dev/input").join(ev))
}

/// Decode one evdev record into `(button id, pressed)`. Only `EV_KEY` presses
/// and releases of the 36 front-panel buttons; auto-repeat (value 2) is dropped.
pub fn decode(ev_type: u16, code: u16, value: i32) -> Option<(u8, bool)> {
    if ev_type != EV_KEY || !(BTN_BASE..BTN_BASE + N_BUTTONS).contains(&code) {
        return None;
    }
    match value {
        0 | 1 => Some(((code - BTN_BASE) as u8, value == 1)),
        _ => None,
    }
}

/// A thread reading the buttons device and forwarding decoded events.
pub struct Watcher {
    stop: Arc<AtomicBool>,
    thread: JoinHandle<()>,
}

impl Watcher {
    /// Open `dev` (no grab) and forward every decoded button event to `tx`.
    pub fn spawn(dev: &Path, tx: Sender<(u8, bool)>) -> std::io::Result<Watcher> {
        use std::os::unix::fs::OpenOptionsExt;
        let file = std::fs::OpenOptions::new()
            .read(true)
            .custom_flags(libc::O_NONBLOCK | libc::O_CLOEXEC)
            .open(dev)?;
        let fd: OwnedFd = file.into();
        let stop = Arc::new(AtomicBool::new(false));
        let flag = stop.clone();
        let thread = std::thread::spawn(move || {
            let size = std::mem::size_of::<libc::input_event>();
            let mut ev: libc::input_event = unsafe { std::mem::zeroed() };
            while !flag.load(Ordering::Relaxed) {
                let mut pfd = libc::pollfd { fd: fd.as_raw_fd(), events: libc::POLLIN, revents: 0 };
                // 50 ms timeout so stop() is noticed promptly.
                if unsafe { libc::poll(&mut pfd, 1, 50) } <= 0 {
                    continue;
                }
                loop {
                    let n = unsafe {
                        libc::read(fd.as_raw_fd(), &mut ev as *mut _ as *mut libc::c_void, size)
                    };
                    if n as usize != size {
                        if n == 0 {
                            // EOF (device gone): stop reading, keep the app running.
                            return;
                        }
                        break;
                    }
                    if let Some(b) = decode(ev.type_, ev.code, ev.value) {
                        if tx.send(b).is_err() {
                            return;
                        }
                    }
                }
            }
        });
        Ok(Watcher { stop, thread })
    }

    /// Stop the thread and wait for it (within about 50 ms).
    pub fn stop(self) {
        self.stop.store(true, Ordering::Relaxed);
        let _ = self.thread.join();
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn tree(entries: &[(&str, &str)]) -> PathBuf {
        let d = std::env::temp_dir().join(format!(
            "launcher-input-{}-{}",
            std::process::id(),
            std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).unwrap().as_nanos()
        ));
        for (ev, name) in entries {
            let dev = d.join(ev).join("device");
            std::fs::create_dir_all(&dev).unwrap();
            std::fs::write(dev.join("name"), format!("{name}\n")).unwrap();
        }
        std::fs::create_dir_all(&d).unwrap();
        d
    }

    #[test]
    fn decode_maps_trigger_happy() {
        assert_eq!(decode(1, 0x2c0 + 8, 1), Some((8, true)));
        assert_eq!(decode(1, 0x2c0, 0), Some((0, false)));
        assert_eq!(decode(1, 0x2c0 + 35, 1), Some((35, true)));
    }

    #[test]
    fn decode_drops_autorepeat_and_out_of_range() {
        assert_eq!(decode(1, 0x2c0 + 8, 2), None);
        assert_eq!(decode(1, 0x2c0 + 36, 1), None);
        assert_eq!(decode(1, 0x2bf, 1), None);
        assert_eq!(decode(2, 0x2c0, 1), None);
    }

    #[test]
    fn find_buttons_matches_name() {
        let d = tree(&[("event1", "Deluge Pads"), ("event3", "Deluge Buttons"), ("event4", "Deluge Buttons2")]);
        assert_eq!(find_buttons(&d), Some(PathBuf::from("/dev/input/event3")));
    }

    #[test]
    fn find_buttons_none_when_absent() {
        let d = tree(&[("event1", "Deluge Pads")]);
        assert_eq!(find_buttons(&d), None);
        assert_eq!(find_buttons(Path::new("/no/such/dir")), None);
    }

    #[test]
    fn watcher_forwards_events_and_stops() {
        // A pipe stands in for the evdev node: write raw input_event records.
        let mut fds = [0; 2];
        assert_eq!(unsafe { libc::pipe(fds.as_mut_ptr()) }, 0);
        let path = PathBuf::from(format!("/proc/self/fd/{}", fds[0]));
        let (tx, rx) = std::sync::mpsc::channel();
        let w = Watcher::spawn(&path, tx).unwrap();
        let mut ev: libc::input_event = unsafe { std::mem::zeroed() };
        ev.type_ = 1;
        ev.code = 0x2c0 + 26;
        ev.value = 1;
        let n = unsafe {
            libc::write(fds[1], &ev as *const _ as *const libc::c_void, std::mem::size_of_val(&ev))
        };
        assert_eq!(n as usize, std::mem::size_of_val(&ev));
        assert_eq!(rx.recv_timeout(std::time::Duration::from_secs(1)).unwrap(), (26, true));
        let t = std::time::Instant::now();
        w.stop();
        assert!(t.elapsed() < std::time::Duration::from_millis(200));
        unsafe {
            libc::close(fds[1]);
        }
    }
}
