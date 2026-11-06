# The hardware

**SoC:** Renesas RZ/A1L (`r7s72102`), single-core ARM Cortex-A9 @ ~400 MHz, with
an MMU, a PL310 L2 cache controller (enabled in the kernel,
`CONFIG_CACHE_L2X0`), and 3 MB of on-chip SRAM. No GPU, but enough on-chip RAM
that the audio path can avoid SDRAM entirely, which is unusual for a chip in
this class. (The RZ/A1H is the 10 MB part; this is the L.)

**RAM:** 64 MB SDRAM at `0x0C000000`. Plus 3 MB on-chip SRAM, of which 1 MB is
handed to userspace (see [drivers.md](drivers.md#fast-sram-deluge-sram)).

Most of the RZ/A1's controllers have mainline Linux drivers (SCIF, RSPI, SDHI,
USBHS, RIIC, MTU2, GPIO, DMAC, CPG clocks). Where a peripheral is standard, the
work is a device-tree node and pinmux, not new code. The custom drivers exist only
where the Deluge does something the SoC doesn't model.

## Peripheral map

Renesas names its peripheral blocks by function rather than by a familiar
acronym: SCIF is the serial UART block, SSI/SSIF is the I²S-style audio
interface, RSPI is the SPI controller. The table maps each block to what it
drives on this board.

| what | block | driver | notes |
| --- | --- | --- | --- |
| Audio codec | SSI0 / SSIF0 | `deluge-ssi` + `deluge-audio` (ASoC) | direct I²S; DMA in on-chip SRAM |
| DIN MIDI | SCIF0 | `deluge-midi` | 31250 baud serial MIDI |
| Front panel | SCIF1 → **PIC32** | `deluge-pic` (transport) | pads, buttons, LEDs, 7-seg all live behind the PIC |
| Pad grid (18×8) | via PIC | `deluge-pad` | evdev multi-touch + fbdev, one driver both ways |
| Buttons | via PIC | `deluge-pic` | evdev |
| Indicator LEDs | via PIC | `leds-deluge` | sysfs LEDs |
| Encoders (6×) | GPIO | `deluge-encoders` | evdev |
| Clock in | P1.14 → IRQC IRQ6 | `deluge-clkin` | falling edge |
| OLED (128×43) | SSD1309 on RSPI0 | `deluge-oled` | CS/DC driven through the PIC |
| CV / gate | MAX5136 on RSPI0 + GPIO | `deluge-cv` | IIO DAC + GPIO gates |
| SD card | SDHI1 | mainline | mounted at `/sd` |
| USB | USBHS channel 0 (`usbhs0`, `renesas_usbhs`) | mainline + patches | single jack, gadget + host; channel 1 unused |
| Fast SRAM | on-chip | `deluge-sram` | `/dev/deluge-sram`, mmap |
| QSPI NOR | SPIBSC | — | not wired up |

## The PIC32 is the front panel

The pads, buttons, LEDs, and 7-segment display are not on the SoC's pins.
They hang off a PIC32 microcontroller, which the RZ/A1L talks to over SCIF1 with
DMA.

That makes `deluge-pic` the link rather than the button driver. It owns the
serial protocol and dispatches to the drivers that model the devices:
`deluge-pad`, `leds-deluge`, and its own evdev node for buttons. Adding
something that lives behind the PIC means extending the transport, not writing a
new serial driver.

## Things that bite

**Audio must not touch SDRAM.** SDRAM latency under load causes underruns at small
periods. The SSI driver puts both its DMA descriptors and its buffers in on-chip
SRAM. With only 3 MB on this part that budget is tight, which is why
`deluge-sram` hands userspace 1 MB and no more.

**USB has two ISO pipes, not three.** The `renesas_usbhs` pipe table can't
satisfy mainline's UAC2 layout (playback, capture, and a dedicated feedback
endpoint), so UAC2 runs in implicit feedback mode, where capture doubles as the
sync source. Without that patch the composite won't bind alongside MIDI and CDC.

**USB is one jack.** Host and peripheral share it, and VBUS sense is strapped
high, which defeats autonomous role detection. The role is switched explicitly
via sysfs; see [drivers.md](drivers.md#usb-gadget).

**PREEMPT_RT is not optional.** At 400 MHz with this much I/O behind a serial
link, audio latency is meaningless without it.

The four out-of-tree patches these constraints require are in
`board/synthstrom/deluge/kernel/patches/`, each with a comment explaining the
hardware reason it exists.
