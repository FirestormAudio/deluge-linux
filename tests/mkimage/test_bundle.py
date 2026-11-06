import json

import pytest

from deluge_mkimage import bundle


def write_lock(tmp_path, url, sha256, version="0.1.0"):
    p = tmp_path / "bundle.lock"
    p.write_text(json.dumps({"version": version, "url": url, "sha256": sha256}))
    return p


def test_deluge_base_env_var_wins(fake_bundle, tmp_path, monkeypatch):
    monkeypatch.setenv("DELUGE_BASE", str(fake_bundle))
    lock = write_lock(tmp_path, "http://never.invalid/x.tar.gz", "0" * 64)

    b = bundle.resolve(lock)

    assert b.root == fake_bundle
    assert b.manifest.exists()
    assert b.rootfs("base").exists()


def test_rootfs_names_the_available_profiles_when_one_is_missing(
    fake_bundle, monkeypatch
):
    monkeypatch.setenv("DELUGE_BASE", str(fake_bundle))
    b = bundle.Bundle(fake_bundle)

    with pytest.raises(FileNotFoundError, match="base"):
        b.rootfs("nonesuch")


def test_boot_resolves_the_named_profiles_own_directory(fake_bundle, monkeypatch):
    monkeypatch.setenv("DELUGE_BASE", str(fake_bundle))
    b = bundle.Bundle(fake_bundle)

    platform_boot = b.boot("platform")

    assert platform_boot == fake_bundle / "boot" / "platform"
    assert (platform_boot / "u-boot.bin").exists()
    assert (platform_boot / "r7s72102-deluge.dtb").exists()
    assert (platform_boot / "zImage").exists()


def test_boot_names_the_available_profiles_when_one_is_missing(
    fake_bundle, monkeypatch
):
    monkeypatch.setenv("DELUGE_BASE", str(fake_bundle))
    b = bundle.Bundle(fake_bundle)

    with pytest.raises(FileNotFoundError, match="base"):
        b.boot("nonexistent")


def test_fetches_extracts_and_exposes_the_pinned_bundle(
    bundle_tarball, tmp_path, monkeypatch
):
    monkeypatch.delenv("DELUGE_BASE", raising=False)
    tarball, digest = bundle_tarball
    lock = write_lock(tmp_path, tarball.as_uri(), digest)
    cache = tmp_path / "cache"

    b = bundle.resolve(lock, cache_dir=cache)

    assert b.manifest.exists()
    assert b.rootfs("base").exists()
    assert (b.toolchain / "bin" / "arm-linux-gcc").exists()


def test_a_sha256_mismatch_is_a_hard_failure(bundle_tarball, tmp_path, monkeypatch):
    monkeypatch.delenv("DELUGE_BASE", raising=False)
    tarball, _ = bundle_tarball
    lock = write_lock(tmp_path, tarball.as_uri(), "b" * 64)

    with pytest.raises(bundle.IntegrityError, match="sha256"):
        bundle.resolve(lock, cache_dir=tmp_path / "cache")


def test_a_cached_bundle_is_not_refetched(bundle_tarball, tmp_path, monkeypatch):
    monkeypatch.delenv("DELUGE_BASE", raising=False)
    tarball, digest = bundle_tarball
    lock = write_lock(tmp_path, tarball.as_uri(), digest)
    cache = tmp_path / "cache"

    bundle.resolve(lock, cache_dir=cache)
    tarball.unlink()  # the source is gone; the cache must still serve it

    b = bundle.resolve(lock, cache_dir=cache)
    assert b.manifest.exists()
