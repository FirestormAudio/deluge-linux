import pytest

from deluge_mkimage import appelf

from conftest import EM_ARM, EM_X86_64, make_elf


def write(tmp_path, blob, name="app"):
    p = tmp_path / name
    p.write_bytes(blob)
    return p


def test_accepts_a_static_hard_float_arm_binary(tmp_path):
    appelf.validate(write(tmp_path, make_elf()))


def test_rejects_a_non_elf(tmp_path):
    with pytest.raises(appelf.InvalidApp, match="not an ELF"):
        appelf.validate(write(tmp_path, b"#!/bin/sh\necho hi\n"))


def test_rejects_a_host_x86_binary(tmp_path):
    with pytest.raises(appelf.InvalidApp, match="not ARM"):
        appelf.validate(write(tmp_path, make_elf(machine=EM_X86_64)))


def test_rejects_a_soft_float_binary(tmp_path):
    with pytest.raises(appelf.InvalidApp, match="hard-float|EABIhf"):
        appelf.validate(write(tmp_path, make_elf(hard_float=False)))


def test_rejects_a_dynamically_linked_binary(tmp_path):
    # PT_INTERP is the definitive marker: it names the dynamic loader. A binary
    # with one is bound to whatever rootfs it lands in — exactly what static
    # linking exists to prevent.
    with pytest.raises(appelf.InvalidApp, match="dynamically linked"):
        appelf.validate(write(tmp_path, make_elf(interp=True)))


def test_the_dynamic_error_explains_how_to_fix_it(tmp_path):
    with pytest.raises(appelf.InvalidApp, match="-static|crt-static"):
        appelf.validate(write(tmp_path, make_elf(interp=True)))


def test_rejects_a_truncated_header_instead_of_crashing(tmp_path):
    # Valid magic, but chopped well short of the 52-byte ELF32 header. This
    # must not surface as a raw IndexError/struct.error: on a console-less
    # device that traceback is the only diagnostic anyone gets.
    with pytest.raises(appelf.InvalidApp):
        appelf.validate(write(tmp_path, make_elf()[:16]))


def test_rejects_a_program_header_table_past_eof_instead_of_crashing(tmp_path):
    # A well-formed ELF header claiming more program headers than the file
    # actually holds. struct.unpack_from would raise struct.error mid-loop;
    # that must be converted to InvalidApp, not leak out raw.
    blob = bytearray(make_elf())
    # e_phnum lives at offset 44 (after e_phentsize at 42), as a <H.
    blob[44:46] = (100).to_bytes(2, "little")
    with pytest.raises(appelf.InvalidApp):
        appelf.validate(write(tmp_path, bytes(blob)))


def test_dynamic_binary_still_rejected_by_default(tmp_path):
    from test_dynelf import build_dynamic_elf
    p = tmp_path / "app"
    p.write_bytes(build_dynamic_elf(needed=["libc.so"]))
    with pytest.raises(appelf.InvalidApp, match="must be static"):
        appelf.validate(p)


def test_dynamic_binary_accepted_when_allowed(tmp_path):
    from test_dynelf import build_dynamic_elf
    p = tmp_path / "app"
    p.write_bytes(build_dynamic_elf(needed=["libc.so"]))
    appelf.validate(p, allow_dynamic=True)      # must not raise


def test_static_binary_accepted_either_way(tmp_path):
    from conftest import make_elf
    p = tmp_path / "app"
    p.write_bytes(make_elf())
    appelf.validate(p)
    appelf.validate(p, allow_dynamic=True)
