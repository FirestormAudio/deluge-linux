import subprocess

import pytest

from deluge_mkimage import cpio


def extract(archive: bytes, dest):
    """Extract with the system cpio — proving we emit a real newc archive and
    not merely something our own reader agrees with."""
    dest.mkdir(parents=True, exist_ok=True)
    subprocess.run(
        ["cpio", "-idmu", "--quiet"],
        input=archive, cwd=dest, check=True,
    )
    return dest


def test_system_cpio_can_extract_what_we_write(tmp_path):
    archive = cpio.build({"usr/bin/app": (b"hello", 0o755)})
    d = extract(archive, tmp_path / "out")
    assert (d / "usr/bin/app").read_bytes() == b"hello"


def test_parent_directories_are_created(tmp_path):
    archive = cpio.build({"a/b/c/file": (b"x", 0o644)})
    d = extract(archive, tmp_path / "out")
    assert (d / "a/b/c").is_dir()


def test_files_are_mode_0755(tmp_path):
    archive = cpio.build({"usr/bin/app": (b"x", 0o755)})
    d = extract(archive, tmp_path / "out")
    assert (d / "usr/bin/app").stat().st_mode & 0o777 == 0o755


def test_uid_and_gid_fields_are_zero():
    archive = cpio.build({"usr/bin/app": (b"x", 0o755)})
    # newc: magic(6) ino(8) mode(8) uid(8) gid(8) ... all ASCII hex.
    uid = archive[22:30]
    gid = archive[30:38]
    assert uid == b"00000000"
    assert gid == b"00000000"


def test_the_app_layer_shadows_the_base_stub(tmp_path):
    base = cpio.build({"usr/bin/deluge-app": (b"STUB", 0o755)})
    app = cpio.build({"usr/bin/deluge-app": (b"REAL", 0o755)})

    combined = cpio.overlay(base, app)

    d = extract(combined, tmp_path / "out")
    assert (d / "usr/bin/deluge-app").read_bytes() == b"REAL"


def test_overlay_keeps_base_files_the_layer_does_not_touch(tmp_path):
    base = cpio.build({
        "usr/bin/deluge-app": (b"STUB", 0o755),
        "etc/hostname": (b"deluge\n", 0o644),
    })
    app = cpio.build({"usr/bin/deluge-app": (b"REAL", 0o755)})

    d = extract(cpio.overlay(base, app), tmp_path / "out")

    assert (d / "usr/bin/deluge-app").read_bytes() == b"REAL"
    assert (d / "etc/hostname").read_bytes() == b"deluge\n"


def test_overlay_strips_the_intermediate_trailer():
    base = cpio.build({"a": (b"1", 0o644)})
    app = cpio.build({"b": (b"2", 0o644)})

    combined = cpio.overlay(base, app)

    assert combined.count(b"TRAILER!!!") == 1


def test_from_dir_preserves_the_executable_bit(tmp_path):
    src = tmp_path / "overlay"
    (src / "etc").mkdir(parents=True)
    (src / "etc" / "conf").write_text("k=v\n")
    script = src / "etc" / "hook"
    script.write_text("#!/bin/sh\n")
    script.chmod(0o755)

    entries = cpio.from_dir(src)

    assert entries["etc/conf"][1] == 0o644
    assert entries["etc/hook"][1] == 0o755


def test_read_round_trips_what_build_writes():
    archive = cpio.build({"usr/bin/app": (b"hello", 0o755)})
    entries = cpio.read(archive)
    mode, data = entries["usr/bin/app"]
    assert data == b"hello"
    assert mode & cpio.S_IFMT == cpio.S_IFREG
    assert mode & 0o777 == 0o755


def test_read_stops_at_the_trailer():
    archive = cpio.build({"a": (b"x", 0o644)})
    assert "TRAILER!!!" not in cpio.read(archive)


def test_symlinks_round_trip():
    archive = cpio.build({"lib/ld-musl-armhf.so.1": cpio.symlink("libc.so")})
    mode, data = cpio.read(archive)["lib/ld-musl-armhf.so.1"]
    assert mode & cpio.S_IFMT == cpio.S_IFLNK
    assert data == b"libc.so"


def test_system_cpio_extracts_our_symlinks(tmp_path):
    archive = cpio.build({
        "libc.so": (b"real", 0o755),
        "ld-musl-armhf.so.1": cpio.symlink("libc.so"),
    })
    d = extract(archive, tmp_path / "out")
    assert (d / "ld-musl-armhf.so.1").is_symlink()
    assert (d / "ld-musl-armhf.so.1").read_bytes() == b"real"


def test_read_parses_a_multi_entry_archive():
    archive = cpio.build({
        "a": (b"1", 0o644),
        "b/c": (b"22", 0o755),
        "b/d": (b"333", 0o644),
    })
    entries = cpio.read(archive)
    assert entries["a"][1] == b"1"
    assert entries["b/c"][1] == b"22"
    assert entries["b/d"][1] == b"333"
