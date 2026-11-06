"""deluge-mkimage — an app plus a prebuilt base, in seconds, with no Buildroot.

Two products from one compiled app:

  image  (default)  the app baked into the base rootfs and packed into a
                    self-contained ELF for the card's /APPS/
  --bare            the validated binary alone, for the card's /LINUX/APPS/,
                    exec'd at run time by the launcher

Both validate the app ELF. That check is the entire reason bare executables go
through this tool rather than being copied by hand.
"""
import argparse
import shutil
import sys
from pathlib import Path

from . import appelf, bundle, cpio, depcheck, dynelf, elf
import memmap

APP_PATH = "usr/bin/deluge-app"
LOCK = Path(__file__).resolve().parents[2] / "bundle.lock"


def _build_parser():
    p = argparse.ArgumentParser(prog="deluge-mkimage")
    p.add_argument("app", help="the compiled app (armv7, EABIhf, static)")
    p.add_argument("-o", "--output", help="output path")
    p.add_argument("--profile", default="base",
                   help="rootfs profile from the bundle (default: base)")
    p.add_argument("--overlay", help="a directory of extra files to bake in")
    p.add_argument("--bare", action="store_true",
                   help="emit the validated binary for /LINUX/APPS/, not an image")
    p.add_argument("--dynamic", action="store_true",
                   help="permit a dynamically linked app, verifying its library "
                        "closure against the rootfs it will run on")
    p.add_argument("--prefix",
                   help="a directory of libraries shipped in the app's private "
                        "$ORIGIN prefix (implies --dynamic)")
    return p


def main(argv=None) -> int:
    args = _build_parser().parse_args(argv)
    app = Path(args.app)

    if not app.exists():
        print(f"error: {app}: no such file", file=sys.stderr)
        return 1

    allow_dynamic = args.dynamic or bool(args.prefix)

    # prefix_name is the private prefix directory's own basename — the value
    # both the depcheck gate (below) and the image bake (further down) must
    # agree on. Validated once, up front: cpio.build() writes dict keys into
    # archive record names verbatim, so a degenerate basename ('', '.', '..')
    # would silently mis-place the bake while every check upstream reports
    # success.
    prefix_name = None
    if args.prefix:
        prefix_name = Path(args.prefix).name
        try:
            depcheck.validate_prefix_name(prefix_name)
        except depcheck.UnresolvedDependency as e:
            print(f"error: {e}", file=sys.stderr)
            return 1

    try:
        appelf.validate(app, allow_dynamic=allow_dynamic)
    except (appelf.InvalidApp, FileNotFoundError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 1

    app_data = app.read_bytes()

    # Only a dynamic app needs the bundle at this point. A static app — bare or
    # image — must keep working with no bundle resolved, which is what makes
    # deluge-mkimage optional in DelugeApp.cmake.
    #
    # Gate on the same property appelf.validate() used to tolerate this binary
    # in the first place: a PT_INTERP segment. dynelf.is_dynamic() instead
    # keys on PT_DYNAMIC, which a PT_INTERP-only binary lacks — that mismatch
    # would let such a binary slip past depcheck entirely and get packed
    # unverified, interpreter included.
    #
    # is_dynamic_app and prefix_providers are also the gate for the image
    # bake further down: a static app given --prefix must not get unverified
    # library bytes baked into its image just because --prefix was passed.
    is_dynamic_app = False
    prefix_providers = {}
    if allow_dynamic:
        try:
            is_dynamic_app = dynelf.parse(app_data).interp is not None
        except dynelf.MalformedELF as e:
            print(f"error: {e}", file=sys.stderr)
            return 1

        if is_dynamic_app:
            try:
                base = bundle.resolve(LOCK)
                providers = depcheck.index_rootfs(base.rootfs(args.profile).read_bytes())
                # Indexed once here and threaded through to the image bake
                # below — re-reading every staged library a second time is
                # wasteful (a SuperCollider-sized bundle stages ~100 of them).
                prefix_providers = depcheck.index_prefix(args.prefix) if args.prefix else {}
                for dup in depcheck.duplicates(providers, prefix_providers):
                    print(f"warning: {dup} is shipped in the prefix but the rootfs "
                          f"already provides it", file=sys.stderr)
                providers.update(prefix_providers)
                problems = depcheck.check(app_data, providers, app.name, prefix_name)
            except (bundle.IntegrityError, depcheck.UnresolvedDependency,
                    dynelf.MalformedELF, FileNotFoundError, ValueError) as e:
                print(f"error: {e}", file=sys.stderr)
                return 1

            if problems:
                for p in problems:
                    print(f"error: {p}", file=sys.stderr)
                return 1

    if args.bare:
        out = Path(args.output) if args.output else Path(app.name)
        shutil.copy2(app, out)
        out.chmod(0o755)
        print(f"wrote {out} (bare; copy to the card's /LINUX/APPS/)")
        return 0

    out = Path(args.output) if args.output else Path(f"{app.name.upper()}.ELF")

    try:
        base = bundle.resolve(LOCK)
        rootfs_base = base.rootfs(args.profile)
    except (bundle.IntegrityError, FileNotFoundError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 1

    entries = {APP_PATH: (app.read_bytes(), 0o755)}
    if args.prefix and is_dynamic_app:
        # Bake the private prefix into the image at the same place its RPATH
        # ($ORIGIN/../lib/<name>) expects to find it: the app lives at
        # /usr/bin/deluge-app, so the prefix lives at /usr/lib/<name>/.
        # <name> is the prefix directory's own basename — CMake stages
        # BUNDLE_LIBS to card/LINUX/lib/<name>, so its basename IS <name> —
        # not the app's filename, since the image renames the app to
        # deluge-app. Without this, depcheck verifies a closure against the
        # staging directory that the packed artifact never actually ships.
        #
        # prefix_providers was already indexed above (and verified by
        # depcheck.check(), since is_dynamic_app is true here) — reused, not
        # re-read. Guarded on is_dynamic_app too: a static app given
        # --prefix skips depcheck entirely (there is nothing dynamic to
        # verify a closure for), so it must not get unverified library bytes
        # baked into its image just because --prefix was passed.
        for lib_name, provider in prefix_providers.items():
            entries[f"usr/lib/{prefix_name}/{lib_name}"] = (provider.data, 0o755)
    if args.overlay:
        entries.update(cpio.from_dir(args.overlay))

    combined = cpio.overlay(rootfs_base.read_bytes(), cpio.build(entries))

    rootfs = out.parent / f".{out.name}.rootfs.cpio"
    rootfs.write_bytes(combined)
    try:
        segments = memmap.load(base.manifest)
        elf.pack(out, segments, base.boot(args.profile), rootfs)
        if not elf.check(out, segments):
            print("error: the packed image failed validation", file=sys.stderr)
            return 1
    except (ValueError, FileNotFoundError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    finally:
        rootfs.unlink(missing_ok=True)

    print(f"wrote {out} (copy to the card's /APPS/)")
    return 0
