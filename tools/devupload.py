#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Dev-mode USB upload for the Deluge app-loader (DLUP frame; see
DelugeFirmware scripts/tasks/task-run.py). Usage: devupload.py [elf]
Deluge must be on the boot menu with DEV MODE: ON."""
import binascii, sys, time, serial, serial.tools.list_ports as lp
elf=sys.argv[1] if len(sys.argv)>1 else "out/LINUX.ELF"
data=open(elf,"rb").read()
port=next((p.device for p in lp.comports() if p.vid==0x16d0 and p.pid==0x0eda), None)
if not port: sys.exit("No 'Deluge Dev Upload' port (VID 16d0/PID 0eda). Boot menu + DEV MODE: ON?")
fr=bytearray(b"DLUP"); fr+=bytes([1,0]); fr+=len(data).to_bytes(4,"little")
fr+=(binascii.crc32(data)&0xFFFFFFFF).to_bytes(4,"little"); fr+=data
print(f"{elf}: {len(data)} B crc {binascii.crc32(data)&0xFFFFFFFF:#010x} -> {port}")
t=time.time()
with serial.Serial(port,115200,timeout=5) as s:
    for i in range(0,len(fr),4096): s.write(fr[i:i+4096])
    s.flush()
print(f"uploaded in {time.time()-t:.1f}s")
