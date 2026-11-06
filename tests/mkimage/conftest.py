import hashlib
import json
import struct
import sys
import tarfile
from pathlib import Path

import pytest

# tools/ holds the deluge_mkimage package.
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))

EM_ARM = 40
EM_X86_64 = 62
ET_EXEC = 2
PT_LOAD = 1
PT_INTERP = 3
EF_ARM_ABI_FLOAT_HARD = 0x00000400
EF_ARM_EABI_VER5 = 0x05000000


def make_elf(machine=EM_ARM, hard_float=True, interp=False) -> bytes:
    """A minimal ELF32 with just enough header for the validator to read.

    The validator only inspects e_machine, e_flags and the program headers, so a
    synthetic header is a faithful stand-in — and it means the tests need no ARM
    cross-compiler to run.
    """
    flags = EF_ARM_EABI_VER5 | (EF_ARM_ABI_FLOAT_HARD if hard_float else 0)
    phdrs = [(PT_LOAD, 0x8000)]
    if interp:
        phdrs.append((PT_INTERP, 0))

    ehsize, phentsize = 52, 32
    e_ident = b"\x7fELF" + bytes([1, 1, 1, 0]) + b"\x00" * 8
    ehdr = e_ident + struct.pack(
        "<HHIIIIIHHHHHH",
        ET_EXEC, machine, 1, 0x8000, ehsize, 0, flags,
        ehsize, phentsize, len(phdrs), 0, 0, 0,
    )
    body = b""
    for p_type, vaddr in phdrs:
        body += struct.pack("<8I", p_type, 0, vaddr, vaddr, 0, 0, 5, 4)
    return ehdr + body


@pytest.fixture
def fake_bundle(tmp_path) -> Path:
    """A bundle with the same shape as a real one, small enough to be readable."""
    root = tmp_path / "bundle"
    (root / "rootfs").mkdir(parents=True)
    (root / "toolchain" / "bin").mkdir(parents=True)

    # Each boot/<profile>/ is self-contained, mirroring scripts/mk-bundle.sh:
    # U-Boot + DTB are config-independent but staged per profile anyway so the
    # packer resolves every segment file from one directory.
    for profile in ("base", "platform"):
        boot = root / "boot" / profile
        boot.mkdir(parents=True)
        (boot / "u-boot.bin").write_bytes(b"UBOOT" * 8)
        (boot / "r7s72102-deluge.dtb").write_bytes(b"DTB" * 8)
        (boot / "zImage").write_bytes(b"ZIMAGE" * 8)

    (root / "toolchain" / "bin" / "arm-linux-gcc").write_text("#!/bin/sh\n")

    # A base rootfs holding the stub the app must shadow.
    from deluge_mkimage import cpio
    (root / "rootfs" / "base.cpio").write_bytes(
        cpio.build({"usr/bin/deluge-app": (b"#!/bin/sh\necho stub\n", 0o755)})
    )

    (root / "manifest.json").write_text(json.dumps({
        "version": "0.1.0",
        "segments": [
            {"file": "u-boot.bin", "load": "0x0C000000"},
            {"file": "r7s72102-deluge.dtb", "load": "0x0DF00000"},
            {"file": "zImage", "load": "0x0E000000"},
            {"file": "rootfs.uimg", "load": "0x0E800000",
             "max_size": "0x1000000", "generated": True},
        ],
    }))
    return root


@pytest.fixture
def bundle_tarball(fake_bundle, tmp_path):
    """The fake bundle, packaged the way a release is: tarball + sha256."""
    tarball = tmp_path / "deluge-base-0.1.0.tar.gz"
    with tarfile.open(tarball, "w:gz") as tf:
        tf.add(fake_bundle, arcname="deluge-base-0.1.0")
    digest = hashlib.sha256(tarball.read_bytes()).hexdigest()
    return tarball, digest
