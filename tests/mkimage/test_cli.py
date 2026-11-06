import subprocess

import pytest

from deluge_mkimage import cli

from conftest import make_elf


def test_image_mode_produces_a_bootable_looking_elf(fake_bundle, tmp_path, monkeypatch):
    monkeypatch.setenv("DELUGE_BASE", str(fake_bundle))
    app = tmp_path / "spark"
    app.write_bytes(make_elf())
    out = tmp_path / "SPARK.ELF"

    rc = cli.main([str(app), "-o", str(out)])

    assert rc == 0
    assert out.read_bytes()[:4] == b"\x7fELF"


def test_the_app_shadows_the_base_stub_in_the_packed_rootfs(
    fake_bundle, tmp_path, monkeypatch
):
    monkeypatch.setenv("DELUGE_BASE", str(fake_bundle))
    app = tmp_path / "spark"
    app.write_bytes(make_elf())
    out = tmp_path / "SPARK.ELF"

    cli.main([str(app), "-o", str(out)])

    # The packed rootfs must contain the app's bytes, and the stub must no longer
    # be the last word on /usr/bin/deluge-app.
    blob = out.read_bytes()
    assert make_elf() in blob
    assert blob.rindex(b"usr/bin/deluge-app") > blob.index(b"echo stub")


def test_bare_mode_emits_the_binary_and_does_not_pack(fake_bundle, tmp_path, monkeypatch):
    monkeypatch.setenv("DELUGE_BASE", str(fake_bundle))
    app = tmp_path / "spark"
    app.write_bytes(make_elf())
    out = tmp_path / "out" / "spark"
    out.parent.mkdir()

    rc = cli.main([str(app), "--bare", "-o", str(out)])

    assert rc == 0
    assert out.read_bytes() == make_elf()


def test_a_dynamic_app_is_rejected_in_both_modes(fake_bundle, tmp_path, monkeypatch):
    monkeypatch.setenv("DELUGE_BASE", str(fake_bundle))
    app = tmp_path / "spark"
    app.write_bytes(make_elf(interp=True))

    assert cli.main([str(app), "-o", str(tmp_path / "x.ELF")]) != 0
    assert cli.main([str(app), "--bare", "-o", str(tmp_path / "x")]) != 0


def test_a_nonexistent_app_path_is_a_clean_error(fake_bundle, tmp_path, monkeypatch, capsys):
    monkeypatch.setenv("DELUGE_BASE", str(fake_bundle))
    app = tmp_path / "does-not-exist"

    rc = cli.main([str(app), "-o", str(tmp_path / "X.ELF")])

    assert rc != 0
    assert "error" in capsys.readouterr().err


def test_an_unknown_profile_names_the_ones_that_exist(
    fake_bundle, tmp_path, monkeypatch, capsys
):
    monkeypatch.setenv("DELUGE_BASE", str(fake_bundle))
    app = tmp_path / "spark"
    app.write_bytes(make_elf())

    rc = cli.main([str(app), "--profile", "nonesuch", "-o", str(tmp_path / "x.ELF")])

    assert rc != 0
    assert "base" in capsys.readouterr().err


def test_an_overlay_dir_lands_in_the_image(fake_bundle, tmp_path, monkeypatch):
    monkeypatch.setenv("DELUGE_BASE", str(fake_bundle))
    app = tmp_path / "spark"
    app.write_bytes(make_elf())
    ov = tmp_path / "assets"
    ov.mkdir()
    (ov / "banner.txt").write_bytes(b"SPARK-ASSET")
    out = tmp_path / "SPARK.ELF"

    cli.main([str(app), "-o", str(out), "--overlay", str(ov)])

    assert b"SPARK-ASSET" in out.read_bytes()


def test_the_entry_script_runs(fake_bundle, tmp_path, monkeypatch):
    app = tmp_path / "spark"
    app.write_bytes(make_elf())
    env = {**dict(__import__("os").environ), "DELUGE_BASE": str(fake_bundle)}
    from pathlib import Path
    script = Path(__file__).resolve().parents[2] / "tools" / "deluge-mkimage"

    r = subprocess.run(
        [str(script), str(app), "-o", str(tmp_path / "SPARK.ELF")],
        env=env, capture_output=True, text=True,
    )

    assert r.returncode == 0, r.stderr


def test_dynamic_app_rejected_without_the_flag(tmp_path, fake_bundle, monkeypatch):
    from test_dynelf import build_dynamic_elf
    app = tmp_path / "app"
    app.write_bytes(build_dynamic_elf(needed=["libc.so"]))
    monkeypatch.setenv("DELUGE_BASE", str(fake_bundle))
    rc = cli.main([str(app), "--bare", "-o", str(tmp_path / "out")])
    assert rc == 1


def test_dynamic_app_with_missing_library_fails(tmp_path, fake_bundle, monkeypatch,
                                                capsys):
    from test_dynelf import build_dynamic_elf
    app = tmp_path / "app"
    app.write_bytes(build_dynamic_elf(needed=["libnowhere.so.9"]))
    monkeypatch.setenv("DELUGE_BASE", str(fake_bundle))
    rc = cli.main([str(app), "--bare", "--dynamic", "-o", str(tmp_path / "out")])
    assert rc == 1
    assert "libnowhere.so.9" in capsys.readouterr().err


def test_interp_without_dynamic_segment_still_goes_through_depcheck(
    tmp_path, fake_bundle, monkeypatch
):
    """appelf.validate() tolerates a binary via --dynamic solely because it
    carries a PT_INTERP segment. If the depcheck gate instead keys on
    dynelf.is_dynamic() (PT_DYNAMIC-keyed), the two checks disagree for a
    PT_INTERP-without-PT_DYNAMIC binary: the gate never runs, and the binary
    — including its unverified interpreter — is packed straight through."""
    from test_dynelf import build_dynamic_elf
    app = tmp_path / "app"
    app.write_bytes(build_dynamic_elf(dynamic=False))
    monkeypatch.setenv("DELUGE_BASE", str(fake_bundle))

    rc = cli.main([str(app), "--bare", "--dynamic", "-o", str(tmp_path / "out")])

    # fake_bundle's rootfs never provides /lib/ld-musl-armhf.so.1, so a real
    # depcheck pass must reject this as an unverified interpreter.
    assert rc != 0


def test_corrupt_rootfs_cpio_is_a_clean_error_not_a_traceback(
    tmp_path, fake_bundle, monkeypatch, capsys
):
    from test_dynelf import build_dynamic_elf
    # cpio.read() needs >= 110 bytes to even inspect the magic; shorter than
    # that it just returns no entries instead of raising.
    (fake_bundle / "rootfs" / "base.cpio").write_bytes(b"not a cpio archive, deliberately corrupt" * 4)
    app = tmp_path / "app"
    app.write_bytes(build_dynamic_elf(needed=["libc.so"]))
    monkeypatch.setenv("DELUGE_BASE", str(fake_bundle))

    rc = cli.main([str(app), "--bare", "--dynamic", "-o", str(tmp_path / "out")])

    assert rc != 0
    assert "error" in capsys.readouterr().err


def test_static_bare_still_works_without_a_bundle(tmp_path, monkeypatch):
    """The packer is optional in DelugeApp.cmake; a static bare build must not
    start requiring a bundle just because dynamic support landed."""
    from conftest import make_elf
    monkeypatch.delenv("DELUGE_BASE", raising=False)
    app = tmp_path / "app"
    app.write_bytes(make_elf())
    assert cli.main([str(app), "--bare", "-o", str(tmp_path / "out")]) == 0


def test_dynamic_app_image_bakes_the_prefix_into_the_cpio(
    fake_bundle, tmp_path, monkeypatch, capsys
):
    """C1: depcheck resolves the closure against the staged --prefix dir, but
    the image product used to never write that prefix into the cpio it packs
    — a gate that passes for an artifact whose closure provably fails at
    runtime. The prefix must land at usr/lib/<name>/, matching the RPATH
    ($ORIGIN/../lib/<name>) DelugeApp.cmake sets, where <name> is the prefix
    directory's own basename (not the app's filename — the image renames the
    app to deluge-app)."""
    from deluge_mkimage import cpio
    from test_dynelf import build_dynamic_elf

    libc = build_dynamic_elf(soname="libc.so", interp=None)
    ld = build_dynamic_elf(soname="ld-musl-armhf.so.1", interp=None)
    (fake_bundle / "rootfs" / "base.cpio").write_bytes(cpio.build({
        "usr/bin/deluge-app": (b"#!/bin/sh\necho stub\n", 0o755),
        "lib/libc.so": (libc, 0o755),
        "lib/ld-musl-armhf.so.1": (ld, 0o755),
    }))
    monkeypatch.setenv("DELUGE_BASE", str(fake_bundle))

    # CMake stages BUNDLE_LIBS to card/LINUX/lib/<name>/ — the prefix
    # directory's basename IS <name>.
    prefix = tmp_path / "myapp"
    prefix.mkdir()
    plugin = build_dynamic_elf(soname="libplugin.so.1", interp=None)
    (prefix / "libplugin.so.1").write_bytes(plugin)

    app = tmp_path / "app"
    app.write_bytes(build_dynamic_elf(
        needed=["libc.so", "libplugin.so.1"], runpath="$ORIGIN/../lib/myapp"))
    out = tmp_path / "APP.ELF"

    rc = cli.main([str(app), "--prefix", str(prefix), "-o", str(out)])

    assert rc == 0, capsys.readouterr().err
    blob = out.read_bytes()
    assert b"usr/lib/myapp/libplugin.so.1" in blob
    assert plugin in blob


# --- F1: the $ORIGIN RUNPATH entry must point at the SAME name the packer
# bakes to, end to end through the CLI.


def test_staging_dir_and_binary_runpath_disagreeing_is_rejected(
    fake_bundle, tmp_path, monkeypatch, capsys
):
    """A build staging to 'stage/' while the binary's RPATH encodes a
    DIFFERENT name ($ORIGIN/../lib/myapp) must be rejected at pack time, not
    baked into an image that dies with a black screen on the device — the
    same shape as C1 (image bake vs. closure verification disagreeing), but
    for the RUNPATH itself."""
    from deluge_mkimage import cpio
    from test_dynelf import build_dynamic_elf

    libc = build_dynamic_elf(soname="libc.so", interp=None)
    (fake_bundle / "rootfs" / "base.cpio").write_bytes(cpio.build({
        "usr/bin/deluge-app": (b"#!/bin/sh\necho stub\n", 0o755),
        "lib/libc.so": (libc, 0o755),
    }))
    monkeypatch.setenv("DELUGE_BASE", str(fake_bundle))

    # Staged to "stage", but the binary's RPATH says "myapp".
    prefix = tmp_path / "stage"
    prefix.mkdir()
    plugin = build_dynamic_elf(soname="libplugin.so.1", interp=None)
    (prefix / "libplugin.so.1").write_bytes(plugin)

    app = tmp_path / "app"
    app.write_bytes(build_dynamic_elf(
        needed=["libc.so", "libplugin.so.1"], runpath="$ORIGIN/../lib/myapp"))
    out = tmp_path / "APP.ELF"

    rc = cli.main([str(app), "--prefix", str(prefix), "-o", str(out)])

    assert rc != 0
    err = capsys.readouterr().err
    assert "stage" in err
    assert not out.exists()


# --- F3: a degenerate --prefix basename must be a clean, explicit error


@pytest.mark.parametrize("degenerate_prefix", [".", ".."])
def test_degenerate_prefix_basename_is_a_clean_error(
    fake_bundle, tmp_path, monkeypatch, capsys, degenerate_prefix
):
    """--prefix . -> Path.name == '' ; --prefix .. -> Path.name == '..'. Both
    would fold to a bake key outside the intended usr/lib/<name>/ location
    (see depcheck.validate_prefix_name) — must be a clean, explicit error,
    not a silently mis-placed bake."""
    from test_dynelf import build_dynamic_elf
    monkeypatch.setenv("DELUGE_BASE", str(fake_bundle))

    app = tmp_path / "app"
    app.write_bytes(build_dynamic_elf(needed=["libc.so"]))
    out = tmp_path / "APP.ELF"

    rc = cli.main([str(app), "--prefix", degenerate_prefix, "-o", str(out)])

    assert rc != 0
    assert "error" in capsys.readouterr().err
    assert not out.exists()


# --- Cheap correctness minors ---


def test_static_app_with_prefix_does_not_bake_unverified_libraries(
    fake_bundle, tmp_path, monkeypatch, capsys
):
    """The bake used to be guarded on `args.prefix` alone, not on the app
    actually being dynamic. A static app (no PT_INTERP) given --prefix skips
    depcheck entirely (nothing dynamic to verify a closure for) — it must
    not then get unverified library bytes baked into its image just because
    --prefix happened to be passed."""
    from conftest import make_elf

    monkeypatch.setenv("DELUGE_BASE", str(fake_bundle))

    prefix = tmp_path / "myapp"
    prefix.mkdir()
    (prefix / "not-really-a-lib.so").write_bytes(b"not even ELF")

    app = tmp_path / "app"
    app.write_bytes(make_elf())  # no PT_INTERP: a genuinely static binary
    out = tmp_path / "APP.ELF"

    rc = cli.main([str(app), "--prefix", str(prefix), "-o", str(out)])

    assert rc == 0, capsys.readouterr().err
    blob = out.read_bytes()
    assert b"usr/lib/myapp" not in blob
    assert b"not even ELF" not in blob


def test_index_prefix_is_called_only_once_per_run(
    fake_bundle, tmp_path, monkeypatch, capsys
):
    """index_prefix() used to be called once for depcheck and again to bake
    the image — re-reading and re-parsing every staged library a second
    time. Wasteful for a ~100-plugin SuperCollider-sized bundle. Thread the
    first result through instead."""
    from deluge_mkimage import cpio, depcheck as depcheck_mod
    from test_dynelf import build_dynamic_elf

    libc = build_dynamic_elf(soname="libc.so", interp=None)
    ld = build_dynamic_elf(soname="ld-musl-armhf.so.1", interp=None)
    (fake_bundle / "rootfs" / "base.cpio").write_bytes(cpio.build({
        "usr/bin/deluge-app": (b"#!/bin/sh\necho stub\n", 0o755),
        "lib/libc.so": (libc, 0o755),
        "lib/ld-musl-armhf.so.1": (ld, 0o755),
    }))
    monkeypatch.setenv("DELUGE_BASE", str(fake_bundle))

    prefix = tmp_path / "myapp"
    prefix.mkdir()
    plugin = build_dynamic_elf(soname="libplugin.so.1", interp=None)
    (prefix / "libplugin.so.1").write_bytes(plugin)

    app = tmp_path / "app"
    app.write_bytes(build_dynamic_elf(
        needed=["libc.so", "libplugin.so.1"], runpath="$ORIGIN/../lib/myapp"))
    out = tmp_path / "APP.ELF"

    calls = []
    real_index_prefix = depcheck_mod.index_prefix

    def counting_index_prefix(directory):
        calls.append(directory)
        return real_index_prefix(directory)

    monkeypatch.setattr(depcheck_mod, "index_prefix", counting_index_prefix)

    rc = cli.main([str(app), "--prefix", str(prefix), "-o", str(out)])

    assert rc == 0, capsys.readouterr().err
    assert len(calls) == 1
