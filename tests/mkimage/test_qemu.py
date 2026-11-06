"""Execute a dynamically linked app under qemu-arm against the real rootfs.

The unit tests prove depcheck's logic against synthetic ELFs. This proves the
loader path itself: real toolchain output, real musl, real dlopen, no hardware.

Running the app requires more than `qemu-arm -L <sysroot>`: `-L` only sets the
prefix qemu uses to locate the ELF *interpreter* (ld-musl-armhf.so.1). Once
that interpreter is running under emulation, *it* resolves DT_NEEDED entries
against the ordinary library search path — which, left alone, is the host's
real /lib and /usr/lib, full of x86-64 objects. That produces a confusing
"Exec format error" on perfectly valid ARM libraries. The fix is to also set
QEMU_LD_PREFIX (so qemu's own view of "the root" is the sysroot) and
LD_LIBRARY_PATH (so the musl loader inside the emulation searches the
sysroot's lib directories instead of the host's).
"""
import os
import shutil
import subprocess
from pathlib import Path

import pytest

from deluge_mkimage import cpio

# Defaults assume the newest versioned bundle built in this checkout and a libdeluge checkout
# beside it; override either with DELUGE_BASE / LIBDELUGE_BUILD.
REPO = Path(__file__).resolve().parents[2]
_BUNDLES = sorted(p for p in (REPO / "out").glob("deluge-base-[0-9]*") if p.is_dir())
BUNDLE = Path(os.environ.get(
    "DELUGE_BASE", _BUNDLES[-1] if _BUNDLES else REPO / "out/deluge-base"))
DEMO = Path(os.environ.get(
    "LIBDELUGE_BUILD", REPO.parent / "libdeluge/build")) \
    / "examples/dlopen_demo/card/LINUX"

APP = DEMO / "APPS/dlopen_demo"
PLUGIN = DEMO / "lib/dlopen_demo/libdemoplugin.so.1"

pytestmark = [
    pytest.mark.skipif(shutil.which("qemu-arm") is None,
                        reason="qemu-arm not installed"),
    pytest.mark.skipif(not (BUNDLE / "rootfs/base.cpio").exists(),
                        reason=f"no base bundle at {BUNDLE}/rootfs/base.cpio"),
    pytest.mark.skipif(not APP.exists(),
                        reason=f"dlopen_demo not built — build libdeluge "
                               f"first (set LIBDELUGE_BUILD); expected {APP}"),
]


@pytest.fixture
def sysroot(tmp_path):
    """Unpack base.cpio to a directory qemu-arm can use as its sysroot.

    The card's own on-disk layout (APPS/ + lib/<name>/, joined by an
    $ORIGIN-relative RPATH baked into the binary) already matches the build
    tree's layout, so the app is run straight out of DEMO — nothing needs to
    be copied into a fake card tree. Only the *rootfs* — libc, the musl
    loader, libasound and friends — needs unpacking, since that's what
    stands in for the real SD card / eMMC root the Deluge would boot from.
    """
    root = tmp_path / "sysroot"
    entries = cpio.read((BUNDLE / "rootfs/base.cpio").read_bytes())

    for name, (mode, data) in entries.items():
        path = root / name
        if mode & cpio.S_IFMT == cpio.S_IFDIR:
            path.mkdir(parents=True, exist_ok=True)
    for name, (mode, data) in entries.items():
        path = root / name
        kind = mode & cpio.S_IFMT
        path.parent.mkdir(parents=True, exist_ok=True)
        if kind == cpio.S_IFLNK:
            if not path.exists():
                path.symlink_to(data.decode())
        elif kind == cpio.S_IFREG:
            path.write_bytes(data)
            path.chmod(mode & 0o777)
    return root


def run_under_qemu(sysroot, binary, **kwargs):
    """Run `binary` under qemu-arm against `sysroot`.

    QEMU_LD_PREFIX and LD_LIBRARY_PATH are both required — see the module
    docstring. `-L` alone is not enough.
    """
    env = dict(os.environ)
    env["QEMU_LD_PREFIX"] = str(sysroot)
    env["LD_LIBRARY_PATH"] = f"{sysroot / 'lib'}:{sysroot / 'usr/lib'}"
    return subprocess.run(
        ["qemu-arm", "-L", str(sysroot), str(binary)],
        capture_output=True, text=True, timeout=60, env=env, **kwargs,
    )


def test_dynamic_app_runs_and_dlopens_its_plugin(sysroot):
    # Run straight out of the build tree: RPATH=$ORIGIN/../lib/dlopen_demo
    # already resolves against APPS/dlopen_demo's own location.
    result = run_under_qemu(sysroot, APP)

    # The plugin calling back into the host binary (demo_host_version, defined
    # in main.c and reached only via a symbol the host exports) is what proves
    # --export-dynamic is actually wired up end to end.
    assert "host reports version 21" in result.stdout, result.stderr
    assert "plugin returned 42" in result.stdout, result.stderr
    assert result.returncode == 0


def test_the_loader_is_present_in_the_rootfs(sysroot):
    loader = sysroot / "lib/ld-musl-armhf.so.1"
    assert loader.is_symlink()
    assert (sysroot / "lib/libc.so").exists()
