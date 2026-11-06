import os
from pathlib import Path

import pytest

from deluge_mkimage import cpio, depcheck
from test_dynelf import build_dynamic_elf

# Defaults to the newest versioned bundle built in this checkout.
_REPO = Path(__file__).resolve().parents[2]
_BUNDLES = sorted(p for p in (_REPO / "out").glob("deluge-base-[0-9]*") if p.is_dir())
REAL_BASE_CPIO = Path(os.environ.get(
    "DELUGE_BASE", _BUNDLES[-1] if _BUNDLES else _REPO / "out/deluge-base")
) / "rootfs" / "base.cpio"


def lib(soname, needed=()):
    return build_dynamic_elf(soname=soname, needed=list(needed), interp=None)


def rootfs(**libs):
    """A cpio holding each named library at usr/lib/<soname>."""
    # Include the default interpreter so apps can find it
    entries = {f"usr/lib/{n}": (data, 0o755) for n, data in libs.items()}
    entries["lib/ld-musl-armhf.so.1"] = (lib("ld-musl-armhf.so.1"), 0o755)
    return cpio.build(entries)


def test_satisfied_closure_reports_no_problems():
    archive = rootfs(**{"libc.so": lib("libc.so")})
    app = build_dynamic_elf(needed=["libc.so"])
    assert depcheck.check(app, depcheck.index_rootfs(archive), "app") == []


def test_missing_library_is_reported():
    archive = rootfs(**{"libc.so": lib("libc.so")})
    app = build_dynamic_elf(needed=["libc.so", "libsndfile.so.1"])
    problems = depcheck.check(app, depcheck.index_rootfs(archive), "app")
    assert len(problems) == 1
    assert "libsndfile.so.1" in problems[0]


def test_transitively_missing_library_is_reported_with_its_chain():
    archive = rootfs(**{
        "libc.so": lib("libc.so"),
        "libstdc++.so.6": lib("libstdc++.so.6", needed=["libgcc_s.so.1"]),
    })
    app = build_dynamic_elf(needed=["libstdc++.so.6"])
    problems = depcheck.check(app, depcheck.index_rootfs(archive), "app")
    assert len(problems) == 1
    assert "libgcc_s.so.1" in problems[0]
    assert "libstdc++.so.6" in problems[0]   # the chain that required it


def test_symlinked_provider_in_the_rootfs_resolves():
    archive = cpio.overlay(
        cpio.build({"lib/libc.so": (lib("libc.so"), 0o755)}),
        cpio.build({"lib/ld-musl-armhf.so.1": cpio.symlink("libc.so")}),
    )
    providers = depcheck.index_rootfs(archive)
    assert "ld-musl-armhf.so.1" in providers
    assert providers["ld-musl-armhf.so.1"].data == providers["libc.so"].data


def test_symlink_in_the_private_prefix_is_rejected(tmp_path):
    (tmp_path / "libreal.so.1.0").write_bytes(lib("libreal.so.1"))
    (tmp_path / "libreal.so.1").symlink_to("libreal.so.1.0")
    with pytest.raises(depcheck.UnresolvedDependency, match="symlink"):
        depcheck.index_prefix(tmp_path)


def test_prefix_library_satisfies_a_dependency(tmp_path):
    (tmp_path / "libplugin.so.1").write_bytes(lib("libplugin.so.1"))
    providers = depcheck.index_rootfs(rootfs(**{"libc.so": lib("libc.so")}))
    providers.update(depcheck.index_prefix(tmp_path))
    app = build_dynamic_elf(needed=["libc.so", "libplugin.so.1"],
                            runpath="$ORIGIN/../lib/app")
    assert depcheck.check(app, providers, "app", prefix_name="app") == []


def test_prefix_dependency_without_origin_runpath_is_reported(tmp_path):
    (tmp_path / "libplugin.so.1").write_bytes(lib("libplugin.so.1"))
    providers = depcheck.index_rootfs(rootfs(**{"libc.so": lib("libc.so")}))
    providers.update(depcheck.index_prefix(tmp_path))
    app = build_dynamic_elf(needed=["libc.so", "libplugin.so.1"], runpath=None)
    problems = depcheck.check(app, providers, "app", prefix_name="app")
    assert len(problems) == 1
    assert "$ORIGIN" in problems[0]


# --- F1: the $ORIGIN RUNPATH entry must point at the SAME name the packer
# bakes to, not merely contain the literal substring "$ORIGIN" somewhere.


def test_runpath_pointing_at_a_different_prefix_name_is_rejected(tmp_path):
    """A build staging to e.g. 'stage/' while the binary's RPATH encodes a
    different name (say, $ORIGIN/../lib/myapp) must be rejected here — the
    packer will bake to usr/lib/<prefix_name>/, and a RUNPATH that merely
    contains SOME $ORIGIN entry pointing elsewhere passes a weaker gate and
    then dies with a black screen on the device (the C1 shape, for the
    RUNPATH instead of the image bake)."""
    (tmp_path / "libplugin.so.1").write_bytes(lib("libplugin.so.1"))
    providers = depcheck.index_rootfs(rootfs(**{"libc.so": lib("libc.so")}))
    providers.update(depcheck.index_prefix(tmp_path))
    app = build_dynamic_elf(needed=["libc.so", "libplugin.so.1"],
                            runpath="$ORIGIN/../lib/wrongname")
    problems = depcheck.check(app, providers, "app", prefix_name="stage")
    assert len(problems) == 1
    assert "$ORIGIN/../lib/stage" in problems[0]


def test_runpath_pointing_at_the_matching_prefix_name_is_accepted(tmp_path):
    (tmp_path / "libplugin.so.1").write_bytes(lib("libplugin.so.1"))
    providers = depcheck.index_rootfs(rootfs(**{"libc.so": lib("libc.so")}))
    providers.update(depcheck.index_prefix(tmp_path))
    app = build_dynamic_elf(needed=["libc.so", "libplugin.so.1"],
                            runpath="$ORIGIN/../lib/stage")
    assert depcheck.check(app, providers, "app", prefix_name="stage") == []


def test_missing_interpreter_is_reported():
    archive = rootfs(**{"libc.so": lib("libc.so")})
    app = build_dynamic_elf(needed=["libc.so"], interp="/lib/ld-nonexistent.so")
    problems = depcheck.check(app, depcheck.index_rootfs(archive), "app")
    assert any("ld-nonexistent.so" in p for p in problems)


def test_interpreter_satisfied_only_by_a_prefix_provider_is_rejected(tmp_path):
    """PT_INTERP is resolved by the kernel as an absolute rootfs path at
    exec() time, before the dynamic loader (and $ORIGIN) exist. A file of the
    right basename sitting in the app's private prefix can never satisfy it on
    real hardware, even though it satisfies plain dict membership."""
    archive = cpio.build({"usr/lib/libc.so": (lib("libc.so"), 0o755)})
    (tmp_path / "ld-musl-armhf.so.1").write_bytes(lib("ld-musl-armhf.so.1"))

    providers = depcheck.index_rootfs(archive)
    providers.update(depcheck.index_prefix(tmp_path))

    app = build_dynamic_elf(needed=["libc.so"])  # default interp is ld-musl-armhf.so.1
    problems = depcheck.check(app, providers, "app", prefix_name="app")
    assert any("ld-musl-armhf.so.1" in p for p in problems)


def test_cross_directory_symlink_target_resolves():
    """A symlink's target is joined with `.parent / target` and `.as_posix()`,
    which does not collapse `..` segments. A target that crosses from usr/lib
    up to the top-level lib/ must still resolve against the flat archive
    keys."""
    archive = cpio.overlay(
        cpio.build({"lib/foo_real.so.1": (lib("foo_real.so.1"), 0o755)}),
        cpio.build({"usr/lib/foo.so": cpio.symlink("../../lib/foo_real.so.1")}),
    )
    providers = depcheck.index_rootfs(archive)
    assert "foo.so" in providers
    assert providers["foo.so"].data == providers["foo_real.so.1"].data


def test_absolute_symlink_target_resolves_from_archive_root():
    archive = cpio.overlay(
        cpio.build({"lib/foo_real.so.1": (lib("foo_real.so.1"), 0o755)}),
        cpio.build({"usr/lib/foo.so": cpio.symlink("/lib/foo_real.so.1")}),
    )
    providers = depcheck.index_rootfs(archive)
    assert "foo.so" in providers
    assert providers["foo.so"].data == providers["foo_real.so.1"].data


def test_multihop_symlink_chain_resolves_to_the_final_regular_file():
    """libz.so -> libz.so.1 -> libz.so.1.2.11 (a regular file). Only one hop
    is currently followed, so libz.so.1 resolves but libz.so does not."""
    archive = cpio.overlay(
        cpio.build({"lib/libz.so.1.2.11": (lib("libz.so.1.2.11"), 0o755)}),
        cpio.build({
            "lib/libz.so.1": cpio.symlink("libz.so.1.2.11"),
            "lib/libz.so": cpio.symlink("libz.so.1"),
        }),
    )
    providers = depcheck.index_rootfs(archive)
    assert "libz.so.1" in providers
    assert "libz.so" in providers
    assert providers["libz.so"].data == providers["libz.so.1.2.11"].data


def test_symlink_chain_cycle_does_not_resolve_or_hang():
    """A -> B -> A with neither a regular file: must fail to resolve, not
    hang. (Guards the chain-following fix from Finding 3 against looping.)"""
    archive = cpio.build({
        "lib/liba.so": cpio.symlink("libb.so"),
        "lib/libb.so": cpio.symlink("liba.so"),
    })
    providers = depcheck.index_rootfs(archive)
    assert "liba.so" not in providers
    assert "libb.so" not in providers


def test_a_cycle_terminates():
    archive = rootfs(**{
        "liba.so": lib("liba.so", needed=["libb.so"]),
        "libb.so": lib("libb.so", needed=["liba.so"]),
    })
    app = build_dynamic_elf(needed=["liba.so"])
    assert depcheck.check(app, depcheck.index_rootfs(archive), "app") == []


def test_duplicate_of_a_rootfs_library_warns(tmp_path):
    (tmp_path / "libc.so").write_bytes(lib("libc.so"))
    providers = depcheck.index_rootfs(rootfs(**{"libc.so": lib("libc.so")}))
    warnings = depcheck.duplicates(providers, depcheck.index_prefix(tmp_path))
    assert warnings == ["libc.so"]


# --- C2: a dlopen()ed prefix library's own DT_NEEDED must be walked too ---


def test_prefix_librarys_own_missing_dependency_is_reported(tmp_path):
    """libdemoplugin.so.1 is never in the app's DT_NEEDED — it's dlopen()ed —
    so the only way its own dependencies get verified is by seeding the walk
    with every prefix provider as an additional root."""
    (tmp_path / "libplugin.so.1").write_bytes(
        lib("libplugin.so.1", needed=["libmissing.so.1"]))
    providers = depcheck.index_rootfs(rootfs(**{"libc.so": lib("libc.so")}))
    providers.update(depcheck.index_prefix(tmp_path))
    # The app itself only needs libc.so — libplugin.so.1 is purely
    # dlopen()ed at runtime and never appears in DT_NEEDED.
    app = build_dynamic_elf(needed=["libc.so"], runpath="$ORIGIN/../lib/app")
    problems = depcheck.check(app, providers, "app", prefix_name="app")
    assert any("libmissing.so.1" in p for p in problems)


def test_uses_prefix_is_true_whenever_a_prefix_is_supplied(tmp_path):
    """uses_prefix must come from the prefix being non-empty, not from
    traversal happening to reach a prefix provider — otherwise the $ORIGIN
    check stays dead code for the dlopen case C2 fixes."""
    (tmp_path / "libplugin.so.1").write_bytes(lib("libplugin.so.1"))
    providers = depcheck.index_rootfs(rootfs(**{"libc.so": lib("libc.so")}))
    providers.update(depcheck.index_prefix(tmp_path))
    # Needs nothing from the prefix directly in DT_NEEDED (dlopen-only use),
    # and has no $ORIGIN runpath entry — this must still be flagged.
    app = build_dynamic_elf(needed=["libc.so"], runpath=None)
    problems = depcheck.check(app, providers, "app", prefix_name="app")
    assert any("$ORIGIN" in p for p in problems)


# --- I1: a staged file whose DT_SONAME does not match its filename ---


def test_prefix_file_with_mismatched_soname_is_rejected(tmp_path):
    """DelugeApp.cmake and building-an-app.md both warn that passing
    $<TARGET_FILE:tgt> instead of $<TARGET_SONAME_FILE:tgt> silently stages a
    file dlopen/DT_NEEDED will never find. That mistake is mechanically
    detectable: dynelf.parse() already returns DT_SONAME."""
    (tmp_path / "libplugin.so.1.2.3").write_bytes(lib("libplugin.so.1"))
    with pytest.raises(depcheck.UnresolvedDependency, match="SONAME"):
        depcheck.index_prefix(tmp_path)


def test_non_elf_file_in_the_prefix_is_rejected(tmp_path):
    """Nothing but libraries belongs in the private prefix (see I4: a stray
    .stamp file must not be indexed as a provider)."""
    (tmp_path / ".stamp").write_bytes(b"")
    with pytest.raises(depcheck.UnresolvedDependency):
        depcheck.index_prefix(tmp_path)


# --- F2: a library with no DT_SONAME at all is the normal MODULE/plugin
# shape (SuperCollider's ~100 dlopen()ed UGens are CMake MODULE targets,
# which get no SONAME on Linux at all -- and $<TARGET_SONAME_FILE:tgt> is a
# hard configure error against a MODULE target, so the old message's advice
# could not even be followed). It must be accepted, not rejected.
# A genuine soname/filename mismatch must still be rejected. And a valid ELF
# that isn't a shared library at all (no PT_DYNAMIC) must get its own
# message rather than falling into the mismatch branch and reporting
# "DT_SONAME is 'None'".


def test_no_soname_library_in_prefix_is_accepted(tmp_path):
    """The SuperCollider case: a CMake MODULE .so carries no DT_SONAME. The
    loader matches it on filename in an $ORIGIN prefix, so this must be
    accepted, not rejected."""
    (tmp_path / "libugen.so").write_bytes(build_dynamic_elf(interp=None))
    providers = depcheck.index_prefix(tmp_path)
    assert "libugen.so" in providers
    assert providers["libugen.so"].origin == "prefix"


def test_non_library_elf_in_prefix_gets_its_own_message(tmp_path):
    """A valid ELF with no PT_DYNAMIC segment at all is not a shared library
    — it must not be conflated with the DT_SONAME-mismatch message, which
    would misleadingly report "DT_SONAME is 'None'" for something that was
    never a library in the first place."""
    from conftest import make_elf
    (tmp_path / "not-a-lib.so").write_bytes(make_elf())
    with pytest.raises(depcheck.UnresolvedDependency, match="not a shared library"):
        depcheck.index_prefix(tmp_path)


# --- I2: the symlink loop must respect DEFAULT_PATHS too ---


def test_symlink_outside_default_paths_is_not_indexed():
    archive = cpio.build({
        "usr/share/doc/libfoo.so": cpio.symlink("../../../lib/libfoo_real.so.1"),
        "lib/libfoo_real.so.1": (lib("libfoo_real.so.1"), 0o755),
    })
    providers = depcheck.index_rootfs(archive)
    assert "libfoo.so" not in providers


@pytest.mark.skipif(not REAL_BASE_CPIO.exists(),
                     reason=f"no base bundle at {REAL_BASE_CPIO}")
def test_real_rootfs_index_excludes_busybox_applet_symlinks():
    """Before the DEFAULT_PATHS filter applied to symlinks too, indexing the
    real base.cpio produced 309 providers, 298 of them busybox applet
    symlinks (cat, chmod, dd, ...) living in bin/, sbin/, usr/bin/ rather than
    lib/ or usr/lib/. Assert the real index is now dramatically smaller and
    contains none of them."""
    providers = depcheck.index_rootfs(REAL_BASE_CPIO.read_bytes())
    assert len(providers) < 50
    assert "cat" not in providers
    assert "chmod" not in providers
    assert "dd" not in providers


# --- F3: a degenerate --prefix basename must not silently mis-place the bake


@pytest.mark.parametrize("prefix_name", ["", ".", ".."])
def test_degenerate_prefix_name_is_rejected(prefix_name):
    """cpio.build() writes dict keys verbatim into the archive record name:
    '' folds the bake to usr/lib//<file>, and '..' folds it to
    usr/lib/../<file> == usr/<file> — outside the private prefix the RPATH
    expects entirely. Both must be a hard, explicit error."""
    with pytest.raises(depcheck.UnresolvedDependency):
        depcheck.validate_prefix_name(prefix_name)


@pytest.mark.parametrize("prefix_name", ["myapp", "stage", "libugen"])
def test_ordinary_prefix_name_is_accepted(prefix_name):
    depcheck.validate_prefix_name(prefix_name)  # must not raise
