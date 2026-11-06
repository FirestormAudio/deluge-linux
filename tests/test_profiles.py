"""Profile layout: base keeps the stub app slot; platform ships the launcher."""
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
OVERLAY = REPO / "board/synthstrom/deluge/rootfs/overlay"
CONFIGS = REPO / "configs"


def _fragment(name):
    return (CONFIGS / name).read_text()


def test_stub_only_in_base_overlay():
    assert not (OVERLAY / "common/usr/bin/deluge-app").exists()
    assert (OVERLAY / "base/usr/bin/deluge-app").is_file()
    assert "rootfs/overlay/base" in _fragment("base.fragment")
    assert "rootfs/overlay/base" not in _fragment("platform.fragment")


def test_launcher_only_in_platform():
    assert "BR2_PACKAGE_DELUGE_LAUNCHER=y" in _fragment("platform.fragment")
    assert "BR2_PACKAGE_DELUGE_LAUNCHER" not in _fragment("base.fragment")
    assert "BR2_PACKAGE_DELUGE_LAUNCHER" not in _fragment("deluge_defconfig")


def test_launcher_package_is_registered():
    assert "package/deluge-launcher/Config.in" in (REPO / "Config.in").read_text()


def test_ccache_in_repo_and_cached_in_ci():
    defconfig = _fragment("deluge_defconfig")
    assert "BR2_CCACHE=y" in defconfig
    assert 'BR2_CCACHE_DIR="$(BR2_EXTERNAL_DELUGE_PATH)/.ccache"' in defconfig
    assert "/.ccache/" in (REPO / ".gitignore").read_text()
    for wf in ("build.yml", "release.yml"):
        text = (REPO / ".github/workflows" / wf).read_text()
        assert "path: .ccache" in text, wf
