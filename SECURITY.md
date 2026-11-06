# Security

## What this is

A BSP that runs Linux on a musical instrument. It is not a hardened platform,
and several things that would be findings elsewhere are deliberate here:

- Both profiles set the root password to `deluge`
  (`BR2_TARGET_GENERIC_ROOT_PASSWD` in `configs/deluge_defconfig`).
- The `platform` profile uses it: a root login on the USB serial console
  (`ttyGS0`), and a dropbear SSH server that accepts password logins on port 22
  over the USB network link (`usb0`, 192.168.7.2). That profile exists to be
  poked at. Anyone who can reach the cable or the link can log in as root,
  and that includes the prebuilt `LINUX.ELF` attached to releases.
  Change the password before you build if that matters to you, and don't use
  `platform` on a host or network you don't trust.
- The `base` profile has no login surface: no getty, no SSH, and a kernel
  built without networking. Its USB serial port carries kernel messages only.
- Everything runs as root out of an initramfs. There is no privilege boundary
  inside the image, in either profile.
- Linux runs as an app from the deluge-sdk app-loader, entirely from SDRAM; it
  never writes the flash, and a power cycle returns to the boot menu. The
  app-loader replaces the stock firmware when installed and is a separate
  project with its own security policy. Its DEV MODE accepts and runs any
  image sent over USB, so turn it off when you aren't developing. See
  [docs/boot.md](docs/boot.md#before-you-start).

## Worth reporting

- A published bundle or `LINUX.ELF` that doesn't match what this repo's source
  builds, or a checksum that doesn't verify.
- A build step that fetches and runs something unpinned.
- Anything in `base` that opens a network or USB service not listed in
  [docs/drivers.md](docs/drivers.md).
- A driver bug that can damage hardware. The CV outputs and the codec are the
  ones with real-world consequences.

## Reporting

[Private advisory][advisory]. Please keep the
things above off the public tracker until they're sorted.

Everything else — build failures, driver bugs, boot problems — belongs in a
normal issue, and will get looked at sooner there.

## Supported versions

The latest tag. Nothing is backported.

[advisory]: https://github.com/FirestormAudio/deluge-linux/security/advisories/new
