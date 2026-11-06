# deluge-linux

[![build](https://github.com/FirestormAudio/deluge-linux/actions/workflows/build.yml/badge.svg)](https://github.com/FirestormAudio/deluge-linux/actions/workflows/build.yml)

Linux on the [Synthstrom Deluge](https://synthstrom.com/product/deluge/), a
Renesas RZ/A1L (single-core Cortex-A9 @ 400 MHz, 64 MB SDRAM). Mainline kernel
with PREEMPT_RT, plus drivers for the instrument's own hardware: the pad grid,
OLED, encoders, audio codec, MIDI, and CV/gate.

This repo is the BSP and the image factory, everything needed to go from nothing
to a bootable Linux image. It's a [Buildroot](https://buildroot.org)
`BR2_EXTERNAL` tree: board files, kernel drivers, and patches that Buildroot
assembles into U-Boot, the kernel, and the root filesystem. The repo root is the
external tree, so there's no separate Buildroot checkout to manage.

Applications for the Deluge are built against
[`libdeluge`](https://github.com/FirestormAudio/libdeluge) (C/C++) or
[`deluge-sdk`](https://github.com/FirestormAudio/deluge-sdk) (Rust), which
wrap the kernel ABI this repo exposes (documented in
[docs/drivers.md](docs/drivers.md)) and consumes the release bundle this repo
publishes (documented in [docs/release.md](docs/release.md)).

## Status

Boots from the
[deluge-sdk app-loader](https://github.com/FirestormAudio/deluge-sdk/blob/main/docs/app-loader.md)
off the SD card or over USB, and runs from SDRAM without writing the flash.
The app-loader itself is installed once, like a firmware update; read
[Before you start](docs/boot.md#before-you-start) first. Hardware-verified:

| subsystem | driver | userspace interface |
| --- | --- | --- |
| Audio out + in | `deluge-ssi`, `deluge-audio` | ALSA card `Deluge` (`pcmC0D0p` / `pcmC0D0c`) |
| DIN MIDI | `deluge-midi` | ALSA rawmidi (`midiC0D0`) |
| USB MIDI 2.0 | `f_midi2` gadget | ALSA UMP (`umpC2D0`) |
| OLED display | `deluge-oled` | fbdev `fb1` (128×43) |
| Pad grid | `deluge-pad` | fbdev `fb0` (18×8) + evdev multi-touch |
| Buttons | `deluge-pic` | evdev |
| Encoders (6×) | `deluge-encoders` | evdev |
| LEDs | `leds-deluge` | sysfs LEDs (36 indicators + 2 gold columns) |
| CV / gate | `deluge-cv` | IIO DAC (`iio:device0`) + GPIO lines |
| Clock in | `deluge-clkin` | evdev |
| Fast SRAM | `deluge-sram` | `/dev/deluge-sram` (1 MB, on-chip) |
| USB gadget | composite | CDC-ACM + UAC2 + MIDI 2.0, plus NCM on `platform` only (`1d6b:0104`) |

Device numbers in the table (`fb0`, `iio:device0`, `pcmC0D0p`, …) are what a
current image produces. They depend on probe order, so apps should resolve
devices by name; [docs/drivers.md](docs/drivers.md#finding-devices-by-name)
lists the names.

Every tagged release ships the `deluge-base` bundle, which is what
libdeluge and deluge-sdk consume to build app images without running Buildroot. See
[docs/release.md](docs/release.md).

## Quickstart

**Just want to try it?** Each [release](https://github.com/FirestormAudio/deluge-linux/releases)
has a prebuilt `LINUX.ELF`: the `platform` image, which boots into the
[app launcher](docs/launcher.md). Read [Before you start](docs/boot.md#before-you-start),
copy it to the SD card's `/APPS/`, and pick it from the app-loader's boot menu.

To build it yourself, you need a host compiler, `make`, `git`, and Buildroot's host prerequisites. The
cross-toolchain is a prebuilt tarball Buildroot downloads, so you don't need one
installed.

1. **Install the prerequisites** (Debian/Ubuntu package names; see
   [CONTRIBUTING.md](CONTRIBUTING.md) for what each is for):

   ```sh
   sudo apt install build-essential bc cpio file git rsync unzip wget python3 \
                    libncurses-dev device-tree-compiler python3-serial
   ```

2. **Clone and build** the appliance profile, the same image the Deluge's own
   app-loader boots (no shell, no login, just the instrument):

   ```sh
   git clone https://github.com/FirestormAudio/deluge-linux.git
   cd deluge-linux
   make base       # ~15 min cold on a multi-core desktop, mostly the kernel
   make appelf     # packs the result into out/LINUX.ELF
   ```

3. **Get the image onto the board.** Easiest is over USB, with no SD card: put
   the Deluge on its boot menu with DEV MODE on (see
   [Before you start](docs/boot.md#before-you-start)), then:

   ```sh
   python3 tools/devupload.py out/LINUX.ELF
   ```

   Copying `out/LINUX.ELF` to the SD card's `/APPS/` and picking it from the
   boot menu works too. See [docs/boot.md](docs/boot.md).

4. **Verify it booted.** The `base` profile has no login by design (its USB
   serial port shows kernel messages only), so what you'll see is the instrument's own UI coming up on the pad grid and OLED. For
   a shell and confirmation that the drivers loaded, build `make platform`
   instead; it adds a USB console and SSH, and boots into the
   [app launcher](docs/launcher.md) for the apps in the card's `/LINUX/APPS/`.
   See [docs/debugging.md](docs/debugging.md).

   `platform` logs in as `root` with the password `deluge`, on the USB console
   and over SSH. Change it (`BR2_TARGET_GENERIC_ROOT_PASSWD` in
   `configs/deluge_defconfig`) before building, or don't use `platform` on a
   host or network you don't trust. See [SECURITY.md](SECURITY.md).

Run `make` with no arguments for the full target list.

## Layout

```
board/synthstrom/deluge/   the board: kernel drivers + DTS, U-Boot support, rootfs overlays
configs/                   Buildroot defconfig + the two profile fragments
package/                   Buildroot packages
scripts/                   installs the board's files into the upstream trees;
                           mk-bundle.sh assembles the release bundle
tools/                     host tools:
  mk-app-elf.py              pack out/ into the app-loader ELF
  check-app-elf.py           validate an app-loader ELF against the memory map
  deluge-mkimage             pack an app into an image from the release bundle
  devupload.py               boot an ELF over USB in DEV MODE
  net-connect.sh             host side of the platform profile's USB network
  deluge-midi-send.c         send MIDI 2.0 to the board's USB gadget via libusb
  gen-splash-fonts.py        regenerate the boot splash's font headers
tests/                     pytest suite for the Python tools
debug/                     J-Link / RTT harness
.buildroot/                pinned Buildroot, fetched by make (gitignored, throwaway)
```

`board/synthstrom/deluge/` is the source of truth. Buildroot fetches pristine
upstream sources and the `scripts/install-*.sh` hooks stamp the board's files
into them at build time; nothing under `.buildroot/` is ever edited by hand.

## Docs

- [CONTRIBUTING.md](CONTRIBUTING.md): the development workflow. Build prereqs,
  running the tests, adding a driver or package, patching upstream.
- [docs/hardware.md](docs/hardware.md): the board itself. The SoC, the
  peripherals, and how each one is wired. Start here.
- [docs/launcher.md](docs/launcher.md): the `platform` image's app launcher.
  Putting apps on the card, the escape chord, logs and shutdown.
- [docs/boot.md](docs/boot.md): safety notes, app-loader to U-Boot to kernel,
  the memory map, and how to get an image onto the board.
- [docs/drivers.md](docs/drivers.md): the `deluge-*` drivers and the userspace
  ABI they expose, which is what libdeluge is written against.
- [docs/debugging.md](docs/debugging.md): the USB console (CDC-ACM) and
  network link, JTAG and the RTT console, and what to check when it won't
  boot.
- [docs/release.md](docs/release.md): the release bundle, CI, and how app
  builds consume what this repo produces.

## License

GPL-2.0. The kernel drivers are derivative works of Linux; the U-Boot board
support is GPL-2.0-or-later. The exception is the launcher in
`package/deluge-launcher/`, which is GPL-3.0-or-later (see its `LICENSE`)
because the deluge-sdk UI toolkit and fonts it builds on are.
