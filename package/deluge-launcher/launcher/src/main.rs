//! Reference Linux appliance: the app launcher.
//!
//! Lists `/sd/LINUX/APPS/` on the OLED, launches the selected app with the select
//! encoder, and always returns here on SHIFT+TRIPLETS+LEARN held ~1s. While an
//! app runs the launcher holds no device: it watches the buttons straight from
//! evdev, stops the app with SIGTERM then SIGKILL, and resets the panel after.
//! SHIFT+SELECT opens the settings screen, which offers an SD-safe shutdown.

mod applog;
mod apps;
mod buttons;
mod killwatch;
mod model;
mod panel;
mod supervisor;
mod ui;

use deluge_linux_ui::{controls, OledTarget};
use killwatch::KillWatch;
use libdeluge::{Deluge, Event, EventKind};
use model::{Model, Screen};
use std::path::Path;
use std::sync::mpsc::{self, RecvTimeoutError};
use std::time::{Duration, Instant};
use ui::View;

const FRAME: Duration = Duration::from_millis(33);
const HOLD_MS: u64 = 1000;

/// SIGTERM → SIGKILL grace when the escape chord stops an app.
const STOP_GRACE_MS: u64 = 2000;

enum Msg {
    /// A libdeluge input event (only while the list is showing).
    Input(Event),
    /// A raw button event from the evdev watcher (only while an app runs).
    Button(u8, bool),
    ChildExited(supervisor::Exit),
}

/// Open the instrument and start forwarding its input onto the channel.
fn open_device(tx: &mpsc::Sender<Msg>) -> Result<Deluge, libdeluge::Error> {
    let mut dlg = Deluge::open()?;
    let input_tx = tx.clone();
    dlg.input_start(move |ev| {
        let _ = input_tx.send(Msg::Input(ev));
    })?;
    Ok(dlg)
}

/// Reopen after an app exits. The app's descriptors close as it dies, but
/// give the kernel a moment; if it never succeeds, exit and let runit
/// restart the launcher.
fn reopen_device(tx: &mpsc::Sender<Msg>) -> Deluge {
    for _ in 0..5 {
        match open_device(tx) {
            Ok(d) => return d,
            Err(_) => std::thread::sleep(Duration::from_millis(100)),
        }
    }
    eprintln!("launcher: could not reopen the device after the app exited");
    std::process::exit(1);
}

/// While an app runs: its process group, how it is being stopped, and the
/// button watcher that sees the escape chord.
struct Running {
    pid: i32,
    killed_by_user: bool,
    stopper: supervisor::Stopper,
    watcher: Option<buttons::Watcher>,
}

fn main() {
    let (tx, rx) = mpsc::channel::<Msg>();

    // The instrument is held only while the list is showing (`Some`); it is
    // released while an app runs so the app can open everything, including
    // the exclusive DIN MIDI and audio devices.
    let mut dlg = Some(match open_device(&tx) {
        Ok(d) => d,
        Err(e) => {
            eprintln!("launcher: deluge_open failed: {e}");
            std::process::exit(1);
        }
    });

    let mut target = OledTarget::new();
    let mut view = View::new();
    let mut model = Model::new(apps::scan(&apps::apps_dir()));
    let mut kw = KillWatch::new(HOLD_MS);
    let mut running: Option<Running> = None;

    // SHIFT is a modifier for chords (SHIFT+SELECT opens SETTINGS).
    let mut shift_held = false;

    let start = Instant::now();
    let now_ms = || start.elapsed().as_millis() as u64;

    if let Some(d) = dlg.as_mut() {
        panel::reset(d);
        view.render(&model, &mut target);
        let _ = target.flush(d);
    }

    let mut last_tick = Instant::now();

    loop {
        match rx.recv_timeout(FRAME) {
            Ok(Msg::Input(ev)) => {
                if running.is_some() {
                    continue; // stale event from before the launch
                }
                let t = now_ms();
                if ev.kind == EventKind::Button {
                    let pressed = ev.value == 1;
                    if ev.id == controls::button::SHIFT {
                        shift_held = pressed;
                    }
                    if model.is_browsing() && pressed {
                        if ev.id == controls::encoder_button::SELECT {
                            match model.screen() {
                                Screen::Apps => {
                                    if shift_held {
                                        model.open_settings();
                                    } else if let Some(r) =
                                        launch_selected(&mut model, &tx, &mut dlg, t)
                                    {
                                        running = Some(r);
                                        shift_held = false;
                                    }
                                }
                                Screen::Settings => model.activate_settings_item(),
                                Screen::ConfirmShutdown => model.confirm_shutdown(),
                            }
                        } else if ev.id == controls::button::BACK {
                            model.back();
                        }
                    }
                } else if ev.kind == EventKind::Encoder
                    && ev.id == controls::encoder::SELECT
                    && model.is_browsing()
                {
                    match model.screen() {
                        Screen::Apps => model.move_selection(ev.value as i32),
                        Screen::Settings => model.move_settings_selection(ev.value as i32),
                        Screen::ConfirmShutdown => {}
                    }
                }
            }
            Ok(Msg::Button(id, pressed)) => {
                if kw.on_button(id, pressed, now_ms()) {
                    stop_app(&mut running, now_ms());
                }
            }
            Ok(Msg::ChildExited(exit)) => {
                let r = running.take();
                let killed_by_user = r.as_ref().is_some_and(|r| r.killed_by_user);
                if let Some(w) = r.and_then(|r| r.watcher) {
                    w.stop();
                }
                kw.reset();
                let mut d = reopen_device(&tx);
                panel::reset(&mut d);
                model.on_child_exit(exit, killed_by_user, now_ms());
                model.set_entries(apps::scan(&apps::apps_dir())); // rescan on return
                view.render(&model, &mut target);
                let _ = target.flush(&mut d);
                dlg = Some(d);
            }
            Err(RecvTimeoutError::Timeout) => {}
            Err(RecvTimeoutError::Disconnected) => break,
        }

        // Frame cadence: the chord timer and the SIGKILL deadline while an app
        // runs; toasts, animation and repaint while browsing.
        let t = now_ms();
        if kw.poll(t) {
            stop_app(&mut running, t);
        }
        if let Some(r) = running.as_mut() {
            if let Some(sig) = r.stopper.poll(t) {
                supervisor::signal_group(r.pid, sig);
            }
        }
        model.tick(t);

        let Some(d) = dlg.as_mut() else {
            last_tick = Instant::now();
            continue;
        };

        // A confirmed shutdown. This board can't cut its own power, so a clean
        // shutdown means: make the SD safe, blank the panel, then park the CPU
        // with a "SAFE TO POWER OFF" message the OLED holds on its own.
        //
        //   1. "SHUTTING DOWN" while `prepare` syncs + unmounts /sd.
        //   2. only once the card is safe: blank the LEDs/pads and draw the
        //      final message (so we never claim "safe" before it is).
        //   3. `stop` halts; on hardware it never returns. If it does (an
        //      off-device stub, or a prepare failure), fall back to the list.
        if model.take_shutdown_request() {
            view.render_shutting_down(&mut target);
            let _ = target.flush(d);

            if run_shutdown("prepare")
                .map(|s| s.success())
                .unwrap_or(false)
            {
                panel::reset(d);
                view.render_safe_to_power_off(&mut target);
                let _ = target.flush(d);
                // Both the OLED and pad framebuffers are deferred-io: their
                // blits land ~1 frame (33ms) later on a workqueue, and that
                // work stops the instant `stop` disables interrupts. Give the
                // blits time to reach the panels before we halt.
                std::thread::sleep(Duration::from_millis(200));
                let _ = run_shutdown("stop");
            }
            // prepare failed, or stop returned without halting (off-device).
            model.return_to_apps();
            model.show_toast("SHUTDOWN FAILED".into(), now_ms());
        }

        if model.is_browsing() {
            let delta = last_tick.elapsed().as_millis() as u32;
            last_tick = Instant::now();
            view.tick(&model, delta);
            view.render(&model, &mut target);
            let _ = target.flush(d);
        } else {
            last_tick = Instant::now();
        }
    }
}

/// The escape chord fired: SIGTERM the running app's group now; the frame
/// loop sends SIGKILL once the grace period runs out.
fn stop_app(running: &mut Option<Running>, now_ms: u64) {
    if let Some(r) = running.as_mut() {
        r.killed_by_user = true;
        if let Some(sig) = r.stopper.request(now_ms) {
            supervisor::signal_group(r.pid, sig);
        }
    }
}

/// Launch the highlighted app. Releases the instrument first, starts the
/// button watcher, spawns the app with its log, and starts a reaper thread
/// that reports the exit status. On failure the instrument is reopened and a
/// toast shown.
fn launch_selected(
    model: &mut Model,
    tx: &mpsc::Sender<Msg>,
    dlg: &mut Option<Deluge>,
    now_ms: u64,
) -> Option<Running> {
    let path = model.selected_path()?;
    let name = model
        .entries
        .get(model.selected)
        .map(|e| e.display_name.clone())
        .unwrap_or_default();

    // Release everything (joins the input thread, closes MIDI/display/etc.).
    if let Some(mut d) = dlg.take() {
        d.input_stop();
        drop(d);
    }

    let watcher = match buttons::find_buttons(Path::new("/sys/class/input")) {
        Some(dev) => {
            let btx = tx.clone();
            let (wtx, wrx) = mpsc::channel::<(u8, bool)>();
            std::thread::spawn(move || {
                for (id, pressed) in wrx {
                    if btx.send(Msg::Button(id, pressed)).is_err() {
                        break;
                    }
                }
            });
            match buttons::Watcher::spawn(&dev, wtx) {
                Ok(w) => Some(w),
                Err(e) => {
                    eprintln!("launcher: {}: {e}; chord unavailable", dev.display());
                    None
                }
            }
        }
        None => {
            eprintln!("launcher: no button device, chord unavailable");
            None
        }
    };

    let log = applog::open_log(&applog::log_path(&apps::apps_dir(), &path));
    match supervisor::launch(&path, log) {
        Ok(supervisor::Launched { pid, mut child }) => {
            model.begin_running(name);
            let reap_tx = tx.clone();
            std::thread::spawn(move || {
                let exit = match child.wait() {
                    Ok(s) => supervisor::classify(s),
                    Err(_) => supervisor::Exit::Nonzero(-1),
                };
                let _ = reap_tx.send(Msg::ChildExited(exit));
            });
            Some(Running {
                pid,
                killed_by_user: false,
                stopper: supervisor::Stopper::new(STOP_GRACE_MS),
                watcher,
            })
        }
        Err(_) => {
            if let Some(w) = watcher {
                w.stop();
            }
            let mut d = reopen_device(tx);
            panel::reset(&mut d);
            *dlg = Some(d);
            // exec failed — the app never started; show a toast and stay in the list.
            model.show_toast(format!("CAN'T RUN {name}"), now_ms);
            None
        }
    }
}

/// Run a step of the SD-safe shutdown sequence. `verb` is `prepare` (sync +
/// unmount /sd, then return) or `stop` (halt the CPU). Defaults to
/// `/sbin/deluge-shutdown`; overridable via `LAUNCHER_SHUTDOWN_CMD` so running
/// the launcher off-device (on a workstation) cannot accidentally halt it.
fn run_shutdown(verb: &str) -> std::io::Result<std::process::ExitStatus> {
    let cmd = std::env::var("LAUNCHER_SHUTDOWN_CMD")
        .unwrap_or_else(|_| "/sbin/deluge-shutdown".to_string());
    std::process::Command::new(cmd).arg(verb).status()
}
