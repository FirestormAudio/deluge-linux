"""`make buildroot` must work when CI has already restored .buildroot/dl."""
import shutil
import subprocess
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[1]


def _git(cwd, *args):
    return subprocess.run(["git", "-C", str(cwd), *args], check=True,
                          capture_output=True, text=True).stdout.strip()


@pytest.mark.skipif(shutil.which("git") is None or shutil.which("make") is None,
                    reason="needs git and make")
def test_fetch_into_dir_with_restored_dl_cache(tmp_path):
    # A stand-in Buildroot: one commit with a Makefile.
    upstream = tmp_path / "upstream"
    upstream.mkdir()
    _git(upstream, "init", "-q")
    (upstream / "Makefile").write_text("all:\n")
    _git(upstream, "add", "Makefile")
    _git(upstream, "-c", "user.name=t", "-c", "user.email=t@t", "commit", "-qm", "br")
    sha = _git(upstream, "rev-parse", "HEAD")

    # The checkout, as CI leaves it after restoring the download cache.
    top = tmp_path / "top"
    (top / ".buildroot" / "dl").mkdir(parents=True)
    (top / ".buildroot" / "dl" / "cached.tar").write_text("x")

    r = subprocess.run(
        ["make", "-f", str(REPO / "Makefile"), "-C", str(top), "buildroot",
         f"BUILDROOT_URL=file://{upstream}", f"BUILDROOT_VERSION={sha}"],
        capture_output=True, text=True)
    assert r.returncode == 0, r.stdout + r.stderr
    assert (top / ".buildroot" / "Makefile").is_file()
    assert _git(top / ".buildroot", "rev-parse", "HEAD") == sha
    assert (top / ".buildroot" / "dl" / "cached.tar").read_text() == "x"
