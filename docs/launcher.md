# The launcher

The `platform` image (`LINUX-platform.ELF` in a release, or `make platform
appelf`) boots into an app launcher: boot Linux once, then switch between apps
on the SD card from the Deluge's own OLED and buttons, without rebooting.

`base` has no launcher. It is the profile for single-app images, where
`deluge-mkimage` bakes one app into the slot the launcher occupies in
`platform` (`/usr/bin/deluge-app`). Baking an app into a `platform` image
replaces the launcher in the same way.

## Putting apps on the card

Copy app binaries into the card's `/LINUX/APPS/`. Build them with libdeluge's
`deluge_add_app()` (the `card/LINUX/APPS/<name>` product), or with
`cargo deluge linux --bare` from deluge-sdk. Copying the whole `card/LINUX/`
folder also brings any private libraries an app needs (`/LINUX/lib/<name>/`).

The list shows every ARM executable in `/LINUX/APPS/`, by filename,
upper-cased and sorted. Anything else in the folder (READMEs, scripts, the
`._*` files macOS leaves on FAT cards) is ignored. The list is rescanned each
time it is shown, so apps copied over SSH while the launcher is running appear
when you return to it.

## Controls

| action | control |
| --- | --- |
| Move through the list | turn SELECT |
| Launch the highlighted app | press SELECT |
| Return to the list from any app | hold **SHIFT + TRIPLETS + LEARN** for about a second |
| Settings (SD-safe shutdown) | SHIFT + SELECT |
| Back out of settings | BACK |

The escape chord works even if the app has hung. The launcher asks the app to
quit (SIGTERM to its whole process group), and forces it (SIGKILL) two seconds
later if it is still running. The app sees the three button presses too.

## Between apps

While an app runs it owns the whole instrument: audio, DIN and USB MIDI, pads,
OLED, LEDs, CV and gates. The launcher holds nothing except a read-only watch
on the buttons for the escape chord.

When the app exits, crashes or is stopped, the launcher turns every LED off,
blanks the pads and OLED, sets both CV outputs to 0 V and all gates off, then
shows the list again. If the app exited with an error or crashed, a short
message says so (`SPARK EXITED (2)`, `SPARK CRASHED (11)`); `CAN'T RUN SPARK`
means it could not be started at all.

## Logs

Each app's standard output and error go to `/LINUX/log/<app>.log` on the card,
replaced on every run. If the card can't be written, output is discarded and
the app still runs. The launcher's own messages go to the kernel log
(`dmesg`).

## Shutting down

The Deluge can't switch its own power off. SETTINGS → SHUTDOWN syncs and
unmounts the SD card, darkens the panel and shows `SAFE TO POWER OFF`; then
use the power switch.

## Running apps from a shell

`platform` also has a root shell (USB console and SSH; see
[debugging.md](debugging.md)). While the list is showing, the launcher holds
the instrument, so stop it before running an app by hand, and start it again
afterwards:

```sh
sv down deluge-app
/sd/LINUX/APPS/myapp
sv up deluge-app
```

## The watchdog

The launcher never opens `/dev/watchdog`. An app that arms it accepts that if
it hangs, or is stopped with the escape chord, the board resets back to the
app-loader: the RZ/A1's watchdog times out in about 0.2 s, too soon for the
launcher to take over.

## Source

`package/deluge-launcher/launcher/` is a Rust crate built with Buildroot's
`cargo-package`, against deluge-sdk's `libdeluge` bindings and UI toolkit
(pinned by git revision in its `Cargo.toml`). It is GPL-3.0-or-later, because
that toolkit and its fonts are.

Buildroot doesn't vendor crates for in-tree packages, so building `platform`
fetches those crates from GitHub at build time: it needs network access, and
`make source` doesn't prefetch them. The deluge-sdk sources it links are
public at the pinned revision. Run its tests on the host as described in
[CONTRIBUTING.md](../CONTRIBUTING.md#running-the-tests).
