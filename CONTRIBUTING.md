# Contributing

## The one rule

**`board/synthstrom/deluge/` is the source of truth. `.buildroot/` is throwaway.**

Buildroot fetches pristine upstream U-Boot and Linux, and the hooks in
`external.mk` run `scripts/install-*.sh` to stamp this repo's board files into
them at build time. Anything you edit inside `.buildroot/output/build/` is
silently destroyed on the next clean build. Edit the board, not the tree.

## Build

The cross-toolchain is a prebuilt Bootlin tarball that Buildroot downloads, so
you don't need one installed. You do need the usual Buildroot host
prerequisites. On Debian or Ubuntu:

```sh
sudo apt install build-essential bc cpio file git rsync unzip wget python3 \
                 libncurses-dev device-tree-compiler python3-serial
```

These are Debian package names; on other distributions install the
equivalents. Most are Buildroot's own requirements; see the "System
requirements" section of the Buildroot manual for the full list. The rest:

- `device-tree-compiler` (`dtc`) for the device tree.
- `cpio` for the pytest suite, which checks the packer's archives against the
  system `cpio`.
- `python3-serial` (`pyserial`) for `tools/devupload.py`.
- `zstd` if you run `make bundle`, which compresses the bundle with it.
- For the JTAG flow only: SEGGER's J-Link Software and `arm-none-eabi-gdb`. See
  [docs/debugging.md](docs/debugging.md).

Then:

```sh
make base           # appliance profile
make platform       # hackable profile: getty, ssh, usb networking
make appelf         # -> out/LINUX.ELF
make                # list every target
```

A cold build takes about 15 minutes on a multi-core desktop, most of it the
kernel; after that it's incremental. CI's 2-core hosted runners take hours,
which is why the workflows allow 180 minutes per profile and 240 for the
bundle.

`make sdk` builds the relocatable cross-toolchain on its own. `make bundle`
assembles the full release artifact, both profiles plus the toolchain, that
libdeluge and deluge-sdk consume. See [docs/release.md](docs/release.md) if you're
touching either.

## Running the tests

`tests/` is a pytest suite for the Python packing tools. It doesn't touch
Buildroot or hardware, so it runs in seconds. It needs Python 3.10 or newer
and the system `cpio`:

```sh
pip install pytest
python3 -m pytest tests/ -v
```

The launcher (`package/deluge-launcher/launcher/`) has its own Rust unit tests.
They run on the host against a host build of libdeluge, the same tag
`package/libdeluge` pins:

```sh
git clone --depth 1 -b v0.1.0 https://github.com/FirestormAudio/libdeluge /tmp/libdeluge
cmake -S /tmp/libdeluge -B /tmp/libdeluge/build -DLIBDELUGE_EXAMPLES=OFF -DLIBDELUGE_TESTS=OFF
cmake --build /tmp/libdeluge/build
cmake --install /tmp/libdeluge/build --prefix /tmp/libdeluge-host
cd package/deluge-launcher/launcher
DELUGE_SDK_ROOT=/tmp/libdeluge-host cargo test
```

What it covers:

- `test_memmap.py` covers `tools/memmap.py`'s loader: parsing
  `memory-map.json`, and rejecting a segment placed outside SDRAM, segments
  that overlap, or more than one segment marked `generated`.
- `test_uimage.py` covers `tools/uimage.py`'s U-Boot legacy image header: the
  64-byte header shape, big-endian fields, the ramdisk type/arch/compression
  constants, and both CRCs.
- `test_mk_app_elf.py` covers `tools/mk-app-elf.py` end to end. It packs a fake
  U-Boot/kernel/rootfs into an ELF, checks the `PT_LOAD` segments land at the
  addresses `memory-map.json` specifies, checks the rootfs is wrapped in a
  uImage header, and checks `tools/check-app-elf.py` accepts what was just
  packed and rejects a corrupted one.
- `test_profiles.py` pins the profile layout: the placeholder app slot is in
  `base`'s overlay only, and the launcher package is enabled in `platform`
  only.
- `tests/mkimage/` covers `deluge-mkimage` (`tools/deluge_mkimage/`): the
  newc cpio writer and reader (cross-checked by extracting with the system
  `cpio`), ELF packing and checking, app validation (`appelf`), the dynamic
  section parser (`dynelf`) and `DT_NEEDED` closure check (`depcheck`), bundle
  resolution and checksum verification, and the CLI end to end.
  `test_qemu.py` runs a dynamically linked app under `qemu-arm` against a real
  bundle's rootfs; it skips unless `qemu-arm`, a built bundle (`DELUGE_BASE`),
  and a libdeluge build with its `dlopen_demo` example (`LIBDELUGE_BUILD`) are
  all available.

CI runs this suite as its own job in `build.yml`, alongside the per-profile
Buildroot builds. It fails in seconds if the packing tools are broken, without
blocking or cancelling the `build` job.

## Test it on hardware

Boot menu, DEV MODE on (see
[Before you start](docs/boot.md#before-you-start)), then:

```sh
python3 tools/devupload.py out/LINUX.ELF
```

See [docs/boot.md](docs/boot.md) for what this does and the SD-card
alternative, and [docs/debugging.md](docs/debugging.md) for getting a console.
Use `make platform` while developing — you get a console and a shell over the
same USB cable.

## Changing the kernel config

Never edit the tree's `.config` by hand — it gets overwritten.

```sh
make linux-menuconfig          # configure
make linux-update-defconfig    # snapshot back into board/.../kernel/deluge.config
```

`deluge.config` is the authoritative config and is applied on every build.

Careful: for anything Buildroot models itself, its `BR2_*` symbol overrides
`deluge.config`. Kernel compression is the one that bites. Set
`BR2_LINUX_KERNEL_LZ4` in `configs/deluge_defconfig`; putting
`CONFIG_KERNEL_LZ4=y` in `deluge.config` will be silently reverted. The same
would apply to `CONFIG_INITRAMFS_SOURCE` if `BR2_TARGET_ROOTFS_INITRAMFS` were
set; it is deliberately unset, because the rootfs ships as a separate initrd
(see [docs/boot.md](docs/boot.md#memory-map)).

## Adding a driver

1. Drop the `.c` in `board/synthstrom/deluge/kernel/drivers/`.
2. Add a Kconfig stanza in `board/synthstrom/deluge/kernel/kconfig/`.
3. Register both in `scripts/install-deluge-drivers.sh`: the `DST` map (which
   kernel subdirectory it belongs in), an `addmk` line (the Makefile `obj-`
   entry), and an `addkc` line (where the Kconfig fragment gets spliced in).
4. Add the DT node to `board/synthstrom/deluge/kernel/dts/r7s72102-deluge.dts`.
5. Enable it in `deluge.config`.
6. Document its userspace ABI in [docs/drivers.md](docs/drivers.md). libdeluge is
   written against that page, so a driver missing from it is not usable from an
   app.

## Adding a Buildroot package

Most of what apps need is already in Buildroot. You only need a new package for
something Buildroot doesn't build, or a variant of something it does.
`package/deluge-alsa-static/` is the worked example: a static-only second build
of alsa-lib, because Buildroot's alsa-lib produces a shared or a static library
but not both, and statically linked apps need `libasound.a` while every rootfs
still needs `libasound.so`.

1. Create `package/<name>/Config.in`: a Buildroot `bool` option, `depends on`
   anything it needs, and a `help` block explaining why it exists rather than
   what it is.
2. Create `package/<name>/<name>.mk`, the package recipe. For an autotools
   package that means `<NAME>_VERSION`, `_SITE`, `_SOURCE`, `_LICENSE`,
   `_DEPENDENCIES`, `_CONF_OPTS`, and a call to `$(eval $(autotools-package))`.
   `deluge-alsa-static.mk` installs to the staging sysroot only
   (`_INSTALL_STAGING = YES`, `_INSTALL_TARGET = NO`) and never ships in the
   target rootfs. Copy that shape if yours is toolchain-only too.
3. Register it in the repo's top-level `Config.in`. Buildroot only sees packages
   reachable from there.
4. Enable it in `configs/deluge_defconfig`, or in a profile fragment under
   `configs/` if it's profile-specific.

`package/libdeluge/` fetches [libdeluge](https://github.com/FirestormAudio/libdeluge)
by tag. To build against a local checkout while changing both repos, use
Buildroot's source override:

```sh
make base LIBDELUGE_OVERRIDE_SRCDIR=/path/to/libdeluge
```

To make an override stick, copy `local.mk.example` to `local.mk` (gitignored)
and set it there; `configs/deluge_defconfig` points Buildroot's
`BR2_PACKAGE_OVERRIDE_FILE` at that file.

## Patching upstream

Out-of-tree patches to mainline live in `board/synthstrom/deluge/kernel/patches/`
and `.../uboot/patches/`, and are applied by name in the install scripts. Each
one carries a comment explaining the hardware reason it exists. Keep that up.

Patches are applied with `patch(1)`, deliberately not `git apply`. Buildroot
builds inside this repo's git work tree, so `git apply` resolves patch paths
against the repository root, silently skips every hunk, and still exits 0,
reporting success on a file it never touched.

## Bumping a pin

- Buildroot: `BUILDROOT_VERSION` in the `Makefile`.
- U-Boot: `BR2_TARGET_UBOOT_CUSTOM_REPO_VERSION` in `configs/deluge_defconfig`.
- Linux: `BR2_LINUX_KERNEL_CUSTOM_VERSION_VALUE` in `configs/deluge_defconfig`.

After a kernel bump, check that the four patches still apply; they fail loudly
if not. After a U-Boot bump, check that the board still links
`board/renesas/common` (`nm out/u-boot | grep reset_cpu`).

## The boot splash

`deluge-splash` builds from in-tree source under
`package/deluge-splash/splash/` (`SITE_METHOD = local`), so there's no fetch and
nothing to pin. Edit that directory and the next build picks it up. The wordmark
is a config option rather than a source edit:
`BR2_PACKAGE_DELUGE_SPLASH_NAME`, default `deluge/linux`.

It's on by default. Turning it off still boots, since `/init` guards on
`[ -x /usr/bin/splash ]`.

## Before you send a patch

Run the pytest suite (`python3 -m pytest tests/ -v`). Build both profiles, and
boot at least one of them on real hardware. `dmesg | grep -i deluge` should
show every driver probing. CI builds both profiles and runs the tests, but it
cannot boot anything.
