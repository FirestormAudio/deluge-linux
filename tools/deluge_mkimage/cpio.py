"""Write newc cpio archives, and overlay them.

The kernel unpacks *concatenated* cpio archives in order, with later entries
overwriting earlier ones. That is the whole overlay mechanism: base + app, as
bytes. The base is never unpacked, so there is no repack, no root, no fakeroot.

NOTE — the tuple order is INVERTED between writing and reading, deliberately
left that way (too many callers to change now): build()/from_dir()/symlink()
take/produce (data, mode), but read() returns (mode, data). Every caller has
to know which side of that line it is on; see each function's docstring.
"""
import os
from pathlib import Path

MAGIC = b"070701"
TRAILER = "TRAILER!!!"

S_IFREG = 0o100000
S_IFDIR = 0o040000
S_IFLNK = 0o120000
S_IFMT = 0o170000


def symlink(target: str):
    """An entry value for build(): a symlink whose data is its target.

    newc stores a symlink's target as the record's data, with S_IFLNK in the
    mode. The rootfs reaches libc.so only through such a link, so depcheck has
    to be able to both write and follow them.
    """
    return (target.encode(), S_IFLNK | 0o777)


def _pad4(n: int) -> bytes:
    return b"\x00" * ((-n) & 3)


def _record(name: str, mode: int, data: bytes, ino: int) -> bytes:
    """One newc record: a 110-byte ASCII-hex header, the NUL-terminated name and
    the data, each padded to a 4-byte boundary."""
    name_bytes = name.encode() + b"\x00"
    fields = [
        ino, mode, 0, 0,        # ino, mode, uid, gid — root-owned, always
        1, 0, len(data),        # nlink, mtime, filesize
        0, 0, 0, 0,             # devmajor, devminor, rdevmajor, rdevminor
        len(name_bytes), 0,     # namesize, check
    ]
    header = MAGIC + b"".join(b"%08X" % f for f in fields)
    out = header + name_bytes
    out += _pad4(len(out))
    out += data
    out += _pad4(len(data))
    return out


def build(entries: dict) -> bytes:
    """Build an archive. `entries` maps 'usr/bin/app' -> (data, mode) — data
    FIRST, mode second. read() returns the opposite order, (mode, data); see
    the module docstring."""
    out = bytearray()
    ino = 1
    seen_dirs = set()

    for path in sorted(entries):
        parts = Path(path).parts
        for i in range(1, len(parts)):
            d = "/".join(parts[:i])
            if d not in seen_dirs:
                seen_dirs.add(d)
                out += _record(d, S_IFDIR | 0o755, b"", ino)
                ino += 1

        data, mode = entries[path]
        if not mode & S_IFMT:      # a bare permission mode means a regular file
            mode |= S_IFREG
        out += _record(path, mode, data, ino)
        ino += 1

    out += _record(TRAILER, 0, b"", 0)
    return bytes(out)


def _strip_trailer(archive: bytes) -> bytes:
    """Remove a trailing TRAILER!!! record so the next archive is not ignored.

    The kernel stops unpacking an archive at its trailer, so a concatenation
    whose first half still has one would silently drop everything after it —
    the app would vanish and the stub would run in its place. The header is
    exactly 110 bytes, so the record starts 110 bytes before the name.
    """
    idx = archive.rfind(b"TRAILER!!!\x00")
    if idx < 110:
        return archive
    return archive[: idx - 110]


def overlay(*archives: bytes) -> bytes:
    """Concatenate archives, later entries winning. Only the last keeps its
    trailer."""
    if not archives:
        raise ValueError("overlay needs at least one archive")
    head = b"".join(_strip_trailer(a) for a in archives[:-1])
    return head + archives[-1]


def from_dir(root) -> dict:
    """Walk a --overlay directory into build()'s entry format."""
    root = Path(root)
    entries = {}
    for path in sorted(root.rglob("*")):
        if not path.is_file():
            continue
        rel = path.relative_to(root).as_posix()
        mode = 0o755 if os.access(path, os.X_OK) else 0o644
        entries[rel] = (path.read_bytes(), mode)
    return entries


def read(archive: bytes) -> dict:
    """Parse a newc archive into {name: (mode, data)} — mode FIRST, data
    second. This is the OPPOSITE order from what build()/from_dir()/symlink()
    take/produce, (data, mode); see the module docstring. Nothing is
    currently wrong with this asymmetry, but a caller moving a tuple between
    the two APIs without swapping it will get silently wrong results (a mode
    int where data bytes are expected, or vice versa), so double-check which
    side of the line you're on.

    Every record is a multiple of 4 bytes long (both the header+name and the
    data are individually padded), so records are always 4-aligned and the
    padding can be computed from the running offset.
    """
    entries = {}
    off = 0
    while off + 110 <= len(archive):
        if archive[off:off + 6] != MAGIC:
            raise ValueError(f"bad cpio magic at offset {off}")
        try:
            fields = [
                int(archive[off + 6 + i * 8: off + 14 + i * 8], 16)
                for i in range(13)
            ]
        except ValueError:
            raise ValueError(f"malformed cpio header at offset {off}")

        mode, filesize, namesize = fields[1], fields[6], fields[11]

        name_start = off + 110
        name = archive[name_start: name_start + namesize - 1].decode()
        if name == TRAILER:
            break

        data_start = name_start + namesize
        data_start += (-data_start) & 3
        entries[name] = (mode, archive[data_start: data_start + filesize])

        off = data_start + filesize
        off += (-off) & 3
    return entries
