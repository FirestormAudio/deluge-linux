"""Validate a consuming app's ELF before it is put on a device that has no
console to explain itself with.

Every failure here is one that would otherwise present as a board that boots to
a black screen.
"""
import struct
from pathlib import Path

EM_ARM = 40
PT_INTERP = 3
EF_ARM_ABI_FLOAT_HARD = 0x00000400


class InvalidApp(Exception):
    pass


def validate(path, allow_dynamic: bool = False) -> None:
    data = Path(path).read_bytes()

    if data[:4] != b"\x7fELF":
        raise InvalidApp(f"{path}: not an ELF file")
    if len(data) < 52:
        raise InvalidApp(f"{path}: truncated ELF header")
    if data[4] != 1 or data[5] != 1:
        raise InvalidApp(f"{path}: not a little-endian 32-bit ELF (ELFCLASS32/LSB)")

    e_machine, = struct.unpack_from("<H", data, 18)
    if e_machine != EM_ARM:
        raise InvalidApp(
            f"{path}: not ARM (e_machine={e_machine}). "
            "Built for the host? Use the bundle's toolchain."
        )

    e_flags, = struct.unpack_from("<I", data, 36)
    if not e_flags & EF_ARM_ABI_FLOAT_HARD:
        raise InvalidApp(
            f"{path}: not hard-float (EABIhf). The Deluge's rootfs and kernel are "
            "EABIhf/NEON; a soft-float binary cannot call into them."
        )

    e_phoff, = struct.unpack_from("<I", data, 28)
    e_phentsize, e_phnum = struct.unpack_from("<HH", data, 42)
    try:
        for i in range(e_phnum):
            p_type, = struct.unpack_from("<I", data, e_phoff + i * e_phentsize)
            if p_type == PT_INTERP:
                if allow_dynamic:
                    continue
                raise InvalidApp(
                    f"{path}: dynamically linked (has a PT_INTERP segment).\n"
                    "Deluge apps must be static unless the app opts in: a dynamic "
                    "binary is bound to the rootfs it was built against, and dies "
                    "with no console when they drift.\n"
                    "  C:    add -static, or deluge_add_app(LINKAGE DYNAMIC)\n"
                    "  Rust: RUSTFLAGS='-C target-feature=+crt-static'"
                )
    except struct.error:
        raise InvalidApp(f"{path}: malformed program header table")
