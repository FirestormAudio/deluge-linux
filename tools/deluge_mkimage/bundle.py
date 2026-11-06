"""Resolve the base bundle: the prebuilt platform (boot artifacts, rootfs cpios,
toolchain) that deluge-linux publishes once per release.

DELUGE_BASE points at an unpacked bundle and skips the network entirely — that
is how BSP work iterates without cutting a release.
"""
import hashlib
import json
import os
import tarfile
import urllib.request
from dataclasses import dataclass
from pathlib import Path

DEFAULT_CACHE = Path.home() / ".cache" / "deluge" / "base"


class IntegrityError(Exception):
    pass


@dataclass(frozen=True)
class Bundle:
    root: Path

    @property
    def manifest(self) -> Path:
        return self.root / "manifest.json"

    def boot(self, profile: str) -> Path:
        p = self.root / "boot" / profile
        if not p.is_dir():
            available = sorted(
                q.name for q in (self.root / "boot").iterdir() if q.is_dir()
            ) if (self.root / "boot").is_dir() else []
            raise FileNotFoundError(
                f"no '{profile}' boot artifacts in this bundle; "
                f"available: {', '.join(available) or '(none)'}"
            )
        return p

    @property
    def toolchain(self) -> Path:
        return self.root / "toolchain"

    def rootfs(self, profile: str) -> Path:
        p = self.root / "rootfs" / f"{profile}.cpio"
        if not p.exists():
            available = sorted(
                q.stem for q in (self.root / "rootfs").glob("*.cpio")
            )
            raise FileNotFoundError(
                f"no '{profile}' profile in this bundle; "
                f"available: {', '.join(available) or '(none)'}"
            )
        return p


def verify(tarball: Path, expected_sha256: str) -> None:
    actual = hashlib.sha256(tarball.read_bytes()).hexdigest()
    if actual != expected_sha256:
        raise IntegrityError(
            f"sha256 mismatch for {tarball.name}\n"
            f"  expected {expected_sha256}\n"
            f"  got      {actual}"
        )


def resolve(lock_path, cache_dir=None) -> Bundle:
    override = os.environ.get("DELUGE_BASE")
    if override:
        root = Path(override)
        if not (root / "manifest.json").exists():
            raise FileNotFoundError(
                f"DELUGE_BASE={root} has no manifest.json — "
                "is it an unpacked bundle?"
            )
        return Bundle(root)

    lock = json.loads(Path(lock_path).read_text())
    cache = Path(cache_dir) if cache_dir else DEFAULT_CACHE
    root = cache / lock["version"]
    if (root / "manifest.json").exists():
        return Bundle(root)

    cache.mkdir(parents=True, exist_ok=True)
    tarball = cache / f"deluge-base-{lock['version']}.tar.gz"
    print(f"fetching {lock['url']}")
    urllib.request.urlretrieve(lock["url"], tarball)
    verify(tarball, lock["sha256"])

    root.mkdir(parents=True, exist_ok=True)
    with tarfile.open(tarball) as tf:
        # The tarball's top-level dir is deluge-base-<version>/; strip it.
        for member in tf.getmembers():
            parts = Path(member.name).parts
            if len(parts) <= 1:
                continue
            member.name = str(Path(*parts[1:]))
            tf.extract(member, root, filter="data")
    tarball.unlink()
    return Bundle(root)
