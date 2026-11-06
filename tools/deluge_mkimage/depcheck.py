"""Resolve an app's library closure against the rootfs it is being packed into.

A dynamic app that is missing a library does not fail at pack time by default —
it fails when someone boots a card on a device with no console. This module
moves that failure onto the developer's machine.

What it proves: every library in the DT_NEEDED graph, and the interpreter, is
present. What it does not prove: that every symbol resolves. Symbol-level
checking across versioned .so files is a separate, larger problem.
"""
import posixpath
from dataclasses import dataclass
from pathlib import Path

from . import cpio, dynelf

DEFAULT_PATHS = ("lib", "usr/lib")

# A generous bound on symlink chain length. Real chains are 2-3 hops
# (foo.so -> foo.so.1 -> foo.so.1.2.3); this only exists so a symlink cycle
# fails to resolve instead of looping forever.
MAX_SYMLINK_HOPS = 40


class UnresolvedDependency(Exception):
    pass


@dataclass
class Provider:
    name: str
    data: bytes
    origin: str          # "rootfs" | "prefix"


def _join_symlink_target(link_path: str, target: str) -> str:
    """Resolve a symlink's target against the path of the link itself.

    An absolute target (starting with "/") is rooted at the archive root
    rather than joined to the link's directory. A relative target is joined
    and then normalized with posixpath.normpath so that ".." segments are
    collapsed — otherwise a cross-directory target (e.g. usr/lib/foo.so ->
    ../../lib/foo.so.1) never matches the archive's flat, basename-free keys.
    """
    if target.startswith("/"):
        joined = target
    else:
        joined = posixpath.join(posixpath.dirname(link_path), target)
    return posixpath.normpath(joined).lstrip("/")


def _resolve_symlink(name: str, regular: dict, symlinks: dict):
    """Follow a symlink — including a chain of symlinks — to its final
    regular-file target, or return None if it does not resolve.

    Bounded by MAX_SYMLINK_HOPS and a visited-set so a symlink cycle fails to
    resolve instead of hanging.
    """
    current = name
    visited = set()
    for _ in range(MAX_SYMLINK_HOPS):
        if current in visited:
            return None
        visited.add(current)
        target = symlinks.get(current)
        if target is None:
            return regular.get(current)
        current = _join_symlink_target(current, target)
    return None


def index_rootfs(archive: bytes) -> dict:
    """Index a rootfs cpio by soname, following symlinks (including chains).

    Keyed by basename rather than full path: the loader searches by soname, and
    the rootfs puts libraries in both /lib and /usr/lib.
    """
    entries = cpio.read(archive)
    regular = {
        name: data for name, (mode, data) in entries.items()
        if mode & cpio.S_IFMT != cpio.S_IFLNK
    }
    symlinks = {
        name: data.decode() for name, (mode, data) in entries.items()
        if mode & cpio.S_IFMT == cpio.S_IFLNK
    }

    providers = {}
    for name, data in regular.items():
        if Path(name).parent.as_posix() in DEFAULT_PATHS:
            providers[Path(name).name] = Provider(Path(name).name, data, "rootfs")

    for name in symlinks:
        if Path(name).parent.as_posix() not in DEFAULT_PATHS:
            continue
        resolved = _resolve_symlink(name, regular, symlinks)
        if resolved is not None:
            base = Path(name).name
            providers[base] = Provider(base, resolved, "rootfs")
    return providers


def index_prefix(directory) -> dict:
    """Index an app's private $ORIGIN prefix.

    A symlink here is a hard error: the prefix is copied to a vfat SD card,
    which cannot represent one, so it would silently not arrive.

    Every other entry must be a parseable ELF shared library. Two shapes are
    valid:
      - DT_SONAME present and equal to the file's own name — a SHARED library
        built with VERSION/SOVERSION, staged via $<TARGET_SONAME_FILE:tgt>.
      - DT_SONAME absent entirely — a CMake MODULE library (e.g. a dlopen()ed
        plugin). MODULE targets get no SONAME at all on Linux, and
        $<TARGET_SONAME_FILE:tgt> is a hard configure error against one, so
        this is the normal, valid shape for a plugin, not a defect: the
        loader matches a no-SONAME .so by filename, which is exactly how it
        got staged here.
    A DT_SONAME that IS present but disagrees with the filename is the one
    real mistake (see DelugeApp.cmake and building-an-app.md's
    $<TARGET_SONAME_FILE:tgt> warning: staging under $<TARGET_FILE:tgt>
    instead silently produces a prefix that dlopen/DT_NEEDED will never find
    anything in) and is a hard error. A file that isn't ELF at all, or is ELF
    but not a shared library at all (no PT_DYNAMIC — e.g. a stray static
    object or build-system stamp file), is also a hard error rather than
    being silently skipped, so the mistake surfaces at pack time instead of
    on the device.
    """
    providers = {}
    for path in sorted(Path(directory).iterdir()):
        if path.is_symlink():
            raise UnresolvedDependency(
                f"{path}: symlink in the private prefix. The SD card is vfat and "
                "cannot store symlinks — ship a real file named for its DT_SONAME."
            )
        if not path.is_file():
            continue

        data = path.read_bytes()
        try:
            info = dynelf.parse(data)
        except dynelf.MalformedELF as e:
            raise UnresolvedDependency(
                f"{path}: not a valid ELF library ({e}) — the private prefix "
                "may only contain libraries."
            )
        if not dynelf.is_dynamic(data):
            raise UnresolvedDependency(
                f"{path}: not a shared library (no PT_DYNAMIC segment) — the "
                "private prefix may only contain libraries."
            )
        if info.soname is not None and info.soname != path.name:
            raise UnresolvedDependency(
                f"{path}: DT_SONAME is '{info.soname}', not '{path.name}'. "
                "Stage this library with $<TARGET_SONAME_FILE:tgt>, not "
                "$<TARGET_FILE:tgt> — the latter is the versioned real file, "
                "and staging it under its own name silently produces a prefix "
                "that dlopen/DT_NEEDED will not find anything in at runtime."
            )
        providers[path.name] = Provider(path.name, data, "prefix")
    return providers


INVALID_PREFIX_NAMES = ("", ".", "..")


def validate_prefix_name(prefix_name: str) -> None:
    """Reject a --prefix whose basename can't name a real bake location.

    The packer bakes the prefix to usr/lib/<prefix_name>/ and cpio.build()
    writes dict keys into archive record names verbatim: a basename of ''
    folds to 'usr/lib//<file>', and '..' folds to 'usr/lib/../<file>' ==
    'usr/<file>' — outside the private prefix entirely, and outside the
    directory the app's own RUNPATH expects. Both would report success while
    shipping a broken image.
    """
    if prefix_name in INVALID_PREFIX_NAMES:
        raise UnresolvedDependency(
            f"--prefix has basename '{prefix_name}', which cannot name a real "
            "usr/lib/<name>/ bake location. Pass a directory with a real "
            "name, e.g. --prefix build/myapp-prefix — not '.', '..', or a "
            "path with a trailing slash that resolves to one of those."
        )


def check(app_data: bytes, providers: dict, app_name: str,
          prefix_name: str | None = None) -> list:
    """Walk the DT_NEEDED closure. Returns a list of problems; empty means OK.

    prefix_name is the private prefix directory's own basename — the same
    value the packer bakes to (usr/lib/<prefix_name>/ for the image product).
    When any provider is prefix-origin, the app's DT_RUNPATH must contain
    exactly '$ORIGIN/../lib/<prefix_name>', not merely some $ORIGIN entry:
    otherwise the closure this function verifies and the location the packer
    actually bakes to can silently disagree — the same shape as the C1 bug
    this branch already fixed for the image product, but for the RPATH
    itself. A build staging to one directory while the binary's RPATH
    encodes a different name would pass a weaker "any $ORIGIN" gate and then
    die with a black screen on the device.
    """
    problems = []
    info = dynelf.parse(app_data)

    if info.interp:
        # PT_INTERP is resolved by the kernel as an absolute rootfs path at
        # exec() time, before the dynamic loader — and therefore $ORIGIN and
        # private-prefix semantics — exist at all. Only a "rootfs"-origin
        # provider can satisfy it; a same-named file sitting in the app's
        # private prefix is invisible to the kernel at this point.
        interp_provider = providers.get(Path(info.interp).name)
        if interp_provider is None or interp_provider.origin != "rootfs":
            problems.append(
                f"interpreter {info.interp} is not in the rootfs — "
                "the app cannot start at all"
            )

    # A prefix library is by definition dlopen()ed — that's the whole reason
    # it's not in the app's own DT_NEEDED — so whether the app *uses* it can't
    # be observed by traversal reaching it. Whether the prefix was supplied at
    # all is the only signal available; a non-empty prefix is what obligates
    # the $ORIGIN RUNPATH entry checked below.
    uses_prefix = any(p.origin == "prefix" for p in providers.values())

    seen = set()
    queue = [(n, [app_name]) for n in info.needed]
    # Seed the walk with every prefix provider as an additional root, so each
    # staged library's own DT_NEEDED closure gets resolved too. Without this,
    # a dlopen()ed plugin's dependencies go completely unverified: it never
    # appears in the app's DT_NEEDED, so the app-seeded walk alone never
    # reaches it.
    queue += [(name, [app_name]) for name, p in providers.items()
              if p.origin == "prefix"]

    while queue:
        soname, chain = queue.pop(0)
        if soname in seen:
            continue
        seen.add(soname)

        provider = providers.get(soname)
        if provider is None:
            problems.append(
                f"{soname} not found (required by {' -> '.join(chain)}). "
                "Add it to the rootfs, or ship it via BUNDLE_LIBS."
            )
            continue

        try:
            queue.extend(
                (n, chain + [soname]) for n in dynelf.parse(provider.data).needed
            )
        except dynelf.MalformedELF as e:
            problems.append(f"{soname}: {e}")

    if uses_prefix:
        expected = f"$ORIGIN/../lib/{prefix_name}"
        if expected not in info.runpath:
            problems.append(
                f"the app depends on a private-prefix library but its "
                f"DT_RUNPATH is {info.runpath!r}, which does not contain "
                f"'{expected}' — the packer bakes the prefix to "
                f"usr/lib/{prefix_name}/, and the loader will not find it "
                "there on the device"
            )

    return problems


def duplicates(rootfs_providers: dict, prefix_providers: dict) -> list:
    """Prefix libraries that the rootfs already supplies. Space is scarce."""
    return sorted(set(rootfs_providers) & set(prefix_providers))
