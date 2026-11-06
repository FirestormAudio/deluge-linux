# Debugging

There are two consoles. Use the USB one unless you are debugging U-Boot or a
kernel that dies before USB comes up; in that window JTAG is the only way in.

## The USB console

On the `platform` profile, USB CDC-ACM carries the kernel console and an
interactive login, with no J-Link in the path.

```sh
make platform && make appelf
python3 tools/devupload.py out/LINUX.ELF      # boot menu, DEV MODE on
```

The board comes back as `1d6b:0104` with a `/dev/ttyACM*` port:

```sh
picocom -b 115200 /dev/ttyACM0     # or screen, minicom, whatever
```

Log in as `root` / `deluge`. `dmesg` works and the whole boot log is there.
That password is the same on every `platform` image; see
[SECURITY.md](../SECURITY.md).

The `base` profile is an appliance: no getty, no shell, no network. Its kernel
console is still on USB CDC-ACM (`ttyGS0`) as well as RTT, so the same
`/dev/ttyACM*` port shows kernel messages, but there is nothing to log in to.

## USB networking (`platform`)

The `platform` profile also adds a CDC-NCM network function. The board
configures `usb0` as `192.168.7.2/24` and runs an SSH server (dropbear) on
port 22. The host side needs `192.168.7.1/24` on the matching interface;
`tools/net-connect.sh` sets that up:

```sh
tools/net-connect.sh          # finds the interface, adds 192.168.7.1/24, pings the board
ssh root@192.168.7.2          # password: deluge
tools/net-connect.sh down     # removes the host address again
```

The host interface's name depends on the USB port, so the script finds it by
the gadget's fixed host-side MAC address, `02:22:82:ff:ff:22`. It re-runs
itself under `sudo`, and tells NetworkManager (if present) to leave the
interface alone so it doesn't replace the static address with DHCP.
`DELUGE_HOST_MAC`, `DELUGE_HOST_IP` and `DELUGE_DEV_IP` override the defaults.

To do it by hand instead, assign `192.168.7.1/24` to the new `usb*` or
`enx*` interface and bring it up.

The SSH host key is generated on first boot and kept on the SD card in
`/sd/dropbear/` (in `/run`, so regenerated every boot, if there is no card).

## The RTT console, for U-Boot and the early kernel

U-Boot's console is SEGGER RTT, a ring buffer in on-chip SRAM at `0x20080000`
that J-Link reads out of memory in the background. The kernel keeps RTT as its
earlycon and as `hvc0`.

You need:

- A SEGGER J-Link probe connected to the board over JTAG.
- SEGGER's J-Link Software, for `JLinkGDBServerCLExe`, `JLinkRTTClient` and
  `JLinkExe`.
- `arm-none-eabi-gdb`.

```sh
debug/jlink/harness.sh server              # J-Link GDB server (run in background)
debug/jlink/harness.sh run debug/jlink/boot-rtt.gdb
JLinkRTTClient                             # the interactive console
```

`harness.sh` exists so this can be driven without a human at the keyboard.
`server` starts the GDB server; `run <gdbfile>` waits for the target to attach
before running `arm-none-eabi-gdb -batch`, since attaching mid-connect races
and wedges the JTAG link; `stop` kills it.

Both entry points use the J-Link device name `R7S721020` (the RZ/A1L part
number).

The target must be warm, meaning booted to the app-loader menu at least once, so
that SDRAM and the clocks are initialised. A cold target has no working SDRAM to
load into.

Each gdb script performs the same handoff: `monitor reset`, then set `SCTLR =
0x00C50078`, `VBAR = 0x0C000000`, `cpsr = 0x1D3`, load the images, point J-Link
at the RTT buffer (`monitor exec SetRTTAddr 0x20080000`), and `continue`.

Kept scripts:

| script | what it does |
| --- | --- |
| `boot-rtt.gdb` | load U-Boot, run, console over RTT |
| `boot-linux.gdb` | load U-Boot, the DTB and the kernel (not the rootfs) and run U-Boot; from its RTT prompt, `bootz 0x0e000000 - 0x0df00000` boots the kernel without an initrd |
| `catch-oops.gdb` | break on the kernel's oops path |
| `sdram-test.gdb` | prove SDRAM is alive before blaming anything else |

Run them from the repo root: they load `out/u-boot`, `out/zImage` and
`out/r7s72102-deluge.dtb` (from `make base` or `make platform`), and
`catch-oops.gdb` reads symbols from the pinned kernel build under `.buildroot/`.
Its breakpoint addresses belong to one particular build; refresh them from that
build's `System.map` after rebuilding.

`make load` is the shortcut for pushing `u-boot.bin` into SDRAM over JTAG.

## When it won't boot at all

1. Is the target warm? Power it to the boot menu once.
2. Run `sdram-test.gdb`. If SDRAM is dead, nothing else matters.
3. Did the image actually get there? `devupload.py` prints the CRC32 it sent.
4. Use `catch-oops.gdb` for a kernel that starts and then dies.
