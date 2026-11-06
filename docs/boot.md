# Boot

Linux boots as an application of the [app-loader], a second-stage bootloader
with a boot menu that launches ELF images from the SD card, from flash, or over
USB. Linux itself runs entirely from SDRAM and never writes the flash.

## Before you start

- **Install the app-loader once.** It comes from [deluge-sdk] and installs
  through the Deluge's normal firmware-update path: copy `app-loader.bin` from
  a [deluge-sdk release] to the top of the SD card, hold **SHIFT** while
  powering on, then power-cycle. It replaces the stock firmware as the unit's
  default firmware. The first-stage bootloader is untouched, so you can always
  re-flash the official or Community firmware the same way. See the
  [app-loader manual][app-loader] for the details, including flash-protected
  builds.
- **Linux can't brick the unit.** Images run from SDRAM, so a power cycle always
  returns to the app-loader's boot menu. Hold **LOAD** while powering on to
  force the menu if auto-boot is set to `INSTANT`.
- **USB uploads need DEV MODE.** In the boot menu, open `SETTINGS`, select
  `DEV MODE` to turn it on, then `BACK` to save. It persists across power
  cycles and keeps the menu waiting for an upload; turn it off again to stop
  the unit accepting firmware over USB. Booting from the SD card's `/APPS/`
  doesn't need it.
- **The CV outputs and the audio codec drive real hardware.** A driver or app
  bug can put up to about 10 V on the CV jacks, or full-scale audio into
  whatever is plugged in. Disconnect anything you care about (modular gear,
  headphones, monitors) while testing new drivers or apps.

```
Deluge bootloader (first stage)
        │
        ▼
Deluge app-loader (second stage, boot menu)
        │  loads LINUX.ELF from the SD card's /APPS/ (or over USB in DEV MODE),
        │  copies its four segments into SDRAM, jumps to U-Boot
        ▼
   U-Boot  @ 0x0C000000
        │  bootz 0x0e000000 0x0e800000 0x0df00000   (kernel, initrd, DTB)
        ▼
   Linux   @ 0x0E000000
        │  unpacks the rootfs initrd from 0x0E800000
        ▼
   /init → runsvdir → the app
```

Versions: U-Boot v2026.07, Linux 7.1 with PREEMPT_RT, built by Buildroot
pinned at commit `afc0ca1723a7` (`BUILDROOT_VERSION` in the `Makefile`).

## The app-loader ELF

The app-loader's boot menu can launch an arbitrary ELF from `/APPS/` on the SD
card, so `LINUX.ELF` packs the whole system into one ELF: U-Boot, the device
tree, the kernel, and the root filesystem, as four `PT_LOAD` segments at fixed
addresses. The firmware copies the segments into SDRAM and jumps to the entry
point, which is U-Boot. No relocation, no symbol resolution; the ELF is a
container, not a program.

`tools/mk-app-elf.py` builds it (`make appelf` runs it on `out/`).
`tools/check-app-elf.py` validates the result against the app-loader's
acceptance checks and the memory map: segment addresses and sizes, overlap,
the rootfs's uImage header, and the entry point.

## Memory map

SDRAM is 64 MB at `0x0C000000`–`0x0FFFFFFF`. The app-loader keeps the top of
it, `0x0FD20000` upward, as its own staging window, so every segment has to end
below `0x0FD20000` (`SDRAM_END` in `tools/memmap.py`).

| segment | load address | max size | what |
| --- | --- | --- | --- |
| `u-boot.bin` | `0x0C000000` | | also the ELF entry point |
| `r7s72102-deluge.dtb` | `0x0DF00000` | | device tree |
| `zImage` | `0x0E000000` | 8 MB (up to the rootfs) | the kernel |
| `rootfs.uimg` | `0x0E800000` | 16 MB (`0x1000000`) | the rootfs cpio in a U-Boot legacy header |

The map is data, owned by the board:
[`board/synthstrom/deluge/memory-map.json`](../board/synthstrom/deluge/memory-map.json).
The packer and the checker read it; U-Boot's compiled-in `bootcmd`
(`board/synthstrom/deluge/uboot/configs/deluge_defconfig`) repeats the three
addresses it boots from, and is the one place they are duplicated. If you move
a segment, change both.

The root filesystem is not embedded in the kernel. Buildroot produces an
uncompressed `rootfs.cpio`, and the packer wraps it in a U-Boot legacy
(uImage) ramdisk header at pack time; that is the map's one `generated`
segment. U-Boot reads the rootfs length from that header and hands it to the
kernel as an initrd (`CONFIG_BLK_DEV_INITRD`; `CONFIG_INITRAMFS_SOURCE` is
empty). Because the kernel and the rootfs are separate segments, one prebuilt
`zImage` serves every app image. There is no rootfs partition; the SD card is
mounted afterwards at `/sd`.

## U-Boot

The board support lives in `board/synthstrom/deluge/uboot/` and is stamped into
a pristine upstream tree at build time.

Its job is narrow: take the machine from the state the Deluge firmware left it
in and `bootz` the kernel, initrd and device tree already sitting in RAM. It
does not read the SD card, has no persistent environment, and has no
networking. The kernel command line is compiled into the kernel
(`CONFIG_CMDLINE_FORCE`), so U-Boot's `bootargs` play no part.

The console is SEGGER RTT (`serial_rtt.c`), which needs a J-Link attached. See
[debugging.md](debugging.md).

## Getting an image onto the board

Over USB, which is fastest. Put the Deluge on its boot menu with DEV MODE on
(see [Before you start](#before-you-start)), then:

```sh
python3 tools/devupload.py out/LINUX.ELF
```

In DEV MODE the Deluge enumerates as a USB serial port, `16d0:0eda` ("Deluge
Dev Upload"). `devupload.py` (which needs `pyserial`) sends the image as a
`DLUP` frame with a CRC32, and the app-loader boots it immediately. No SD card,
nothing written to flash.

From the SD card: copy `out/LINUX.ELF` to `/APPS/` and pick it from the boot
menu.

## Boot output

`/init` mounts devtmpfs first, then routes everything to `/dev/kmsg`. It has to
mount devtmpfs before it can redirect its own stderr, so the rootfs ships four
device nodes that exist beforehand: `/dev/console`, `/dev/null`, `/dev/kmsg`,
`/dev/ttyprintk`. They come from
`board/synthstrom/deluge/rootfs/device_table.txt`. Without `/dev/null`, PID 1
dies on its first redirect with no console to say so.

In both profiles the kernel console goes to RTT (`hvc0`) and to USB CDC-ACM
(`ttyGS0`), so kernel messages are visible over the same USB cable you
uploaded with. Only `platform` adds a login on that port. See
[debugging.md](debugging.md).

[app-loader]: https://github.com/FirestormAudio/deluge-sdk/blob/main/docs/app-loader.md
[deluge-sdk]: https://github.com/FirestormAudio/deluge-sdk
[deluge-sdk release]: https://github.com/FirestormAudio/deluge-sdk/releases
