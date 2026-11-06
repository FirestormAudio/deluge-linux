# The `deluge-*` drivers and their userspace ABI

libdeluge and the apps built on it are written against the interfaces on this
page, not against the hardware. Each driver presents a standard Linux interface
(ALSA, evdev, fbdev, IIO, GPIO, sysfs LEDs), so nothing downstream needs to
know it is talking to an RZ/A1L.

The drivers live in `board/synthstrom/deluge/kernel/drivers/` and are stamped
into the kernel tree at build time. Device tree:
`board/synthstrom/deluge/kernel/dts/r7s72102-deluge.dts`.

## Finding devices by name

Device numbers (`fbN`, `iio:deviceN`, `eventN`, ALSA card numbers, and so
`pcmC0D0p`, `midiC0D0`, `umpC2D0`) depend on probe and bind order. The numbers
on this page are what a current image produces, not a contract. Resolve every
device by its name:

| what | stable name | where to look |
| --- | --- | --- |
| OLED framebuffer | `deluge-oled` | `/sys/class/graphics/fb*/name` |
| Pad framebuffer | `deluge-pads` | `/sys/class/graphics/fb*/name` |
| Input devices | see each section | `/sys/class/input/event*/device/name`, or `EVIOCGNAME` |
| CV DAC | `deluge-cv` | `/sys/bus/iio/devices/iio:device*/name` |
| Gate outputs | `deluge-gate0`…`deluge-gate3` | GPIO line names, via `GPIO_V2_GET_LINEINFO_IOCTL` on `/dev/gpiochip*` |
| Audio + DIN MIDI | card `Deluge` | ALSA `hw:CARD=Deluge` |
| USB MIDI 2.0 | card `MIDI 2.0 Gadget` | `/proc/asound/cards` |
| LEDs | `deluge:*` | `/sys/class/leds/` |

For example, the OLED:

```sh
for fb in /sys/class/graphics/fb*; do
    [ "$(cat $fb/name)" = "deluge-oled" ] && echo "/dev/${fb##*/}"
done
```

## Audio: `deluge-ssi` + `deluge-audio`

Built on ASoC. `deluge-ssi` is the CPU-side DAI; `deluge-audio` is the machine
driver that binds it to the codec and registers the card.

- Card: `Deluge` (currently card 0, so `pcmC0D0p` / `pcmC0D0c`)
- Playback and capture (line in): stereo, `S24_LE` (24-bit data in a 32-bit
  slot)
- Sample rate: 44.1 kHz. The codec master clock is the 22.5792 MHz `AUDIO_X1`
  crystal (512 × 44.1 kHz), and `hw_params` rejects a rate the SSI cannot
  divide down from it, which includes 48 kHz. Playback and capture run at the
  same rate.

DMA descriptors and buffers go in on-chip SRAM rather than SDRAM; see
[hardware.md](hardware.md#things-that-bite) for why. The driver logs the
addresses it picked at probe, for example:

```
deluge-ssi e820b000.ssi: tx desc @ 0x20100100, rx desc @ 0x20100500 (on-chip SRAM)
deluge-ssi e820b000.ssi: tx buf @ 0x20100900, rx buf @ 0x20108900 (on-chip SRAM)
```

If the SRAM pool is missing, the first line ends in `(SDRAM)` and the second
line is not printed.

Open it as a normal ALSA device. The kernel is PREEMPT_RT, so use a real-time
thread and small periods.

### Speaker and jack detect

The card carries these controls (`amixer -c Deluge controls`):

| control | access | meaning |
| --- | --- | --- |
| `Speaker Playback Switch` | read/write | user enable for the on-board speaker amplifier (default on) |
| `Headphone Jack` | read-only | 1 = headphone plugged in |
| `Line Out L Jack` | read-only | 1 = left line out plugged in |
| `Line Out R Jack` | read-only | 1 = right line out plugged in |
| `Line In Jack` | read-only | 1 = line in plugged in |
| `Mic Jack` | read-only | 1 = mic plugged in |

The amplifier is on only when `Speaker Playback Switch` is on and no headphone
or line-out jack is plugged in. The input jacks don't affect the speaker. The
jack-detect pins have no interrupt, so the driver polls them every 500 ms for
the speaker policy; the jack controls read the pin when you read them.

## MIDI: `deluge-midi` (DIN) and `f_midi2` (USB)

DIN MIDI is `deluge-midi`, a serial MIDI driver on SCIF0 at 31250 baud. It
registers on the `Deluge` card as ALSA rawmidi:

- `midiC0D0` (with `Deluge` as card 0), the 5-pin DIN in/out

USB MIDI 2.0 comes from the mainline `f_midi2` gadget function, which creates
its own ALSA card, `MIDI 2.0 Gadget`, exposed as ALSA UMP:

- `umpC2D0` on a current image, MIDI 2.0 with MIDI 1.0 fallback, one
  bidirectional group

## Display: `deluge-oled`

The 128×43 OLED (SSD1309 on RSPI0; chip-select and data/command are driven
through the PIC).

- fbdev, `name` = `deluge-oled` (currently `fb1`)
- 128×43, 1 bit per pixel, 16 bytes per line, row-major, most significant bit
  is the leftmost pixel, 1 = lit (`FB_VISUAL_MONO10`)
- Deferred I/O: writes reach the panel at up to about 30 fps

## Pads: `deluge-pad`

The 18×8 RGB pad grid is both an input device and a display. One driver owns
both directions and the coordinate mapping between them. Column 0, row 0 is the
top-left pad in both.

- Output (the LEDs under the pads): fbdev, `name` = `deluge-pads` (currently
  `fb0`). 18×8, 24 bits per pixel, 54 bytes per line, bytes in R, G, B order.
  Deferred I/O at up to about 30 fps.
- Input (pad presses): evdev `Deluge Pads`, multi-touch type B. One slot per
  pad; `ABS_MT_POSITION_X` = column × 256 + 128 (range 0–4607),
  `ABS_MT_POSITION_Y` = row × 256 + 128 (range 0–2047), `ABS_MT_PRESSURE` =
  4095 on press. The pads are not velocity-sensitive.

## Buttons, encoders, clock in: `deluge-pic`, `deluge-encoders`, `deluge-clkin`

The front panel is behind a PIC32, reached over SCIF1 with DMA. `deluge-pic` is
transport only: it owns the link and hands events to the drivers that model
them.

- `deluge-pic`: the 36 buttons, evdev `Deluge Buttons`. Button *n* (0–35)
  reports `EV_KEY` code `BTN_TRIGGER_HAPPY1 + n`.
- `deluge-encoders`: the 6 rotary encoders, evdev `Synthstrom Deluge Encoders`,
  as `EV_REL` steps. Detented encoders report one step per detent; the two gold
  knobs are detentless and report one step per edge.

  | encoder | code |
  | --- | --- |
  | scroll Y | `REL_WHEEL` |
  | scroll X | `REL_HWHEEL` |
  | tempo | `REL_MISC` |
  | select | `REL_DIAL` |
  | gold knob, top | `REL_RY` |
  | gold knob, bottom | `REL_RX` |

- `deluge-clkin`: the analog clock input, evdev `Deluge Clock In`. One
  `EV_MSC`/`MSC_RAW` event per pulse, whose value is a running pulse count.
  Falling edge on RZ/A1 IRQC line IRQ6 (pin P1.14); the Linux virq is assigned
  at probe.

### PIC link attributes

`deluge-pic` has two sysfs attributes on its platform device. The device is
named after SCIF1's address (`e8007800.serial`), which is a device-tree detail,
so find it through the driver directory rather than hard-coding it:
`/sys/bus/platform/drivers/deluge-pic/*/`.

| attribute | access | meaning |
| --- | --- | --- |
| `refresh_time` | read/write | The PIC's display refresh interval (PIC command 19). Write an integer 0–25; lower is brighter. A value above 25 returns `EINVAL`. Reads back the last value written, or `unset` if nothing has been written since boot. The value is a raw PIC setting with no SI unit. |
| `rx_overruns` | read-only | Number of SCIF1 receive-FIFO overruns seen since boot. Each one means at least one byte from the PIC was dropped, so a nonzero count means the link has lost pad or button data. |

## LEDs: `leds-deluge`, plus the sync LED

The indicator LEDs, as LED-class devices under `/sys/class/leds/`. The pad grid
is the `deluge-pads` framebuffer, not here.

| name | count | `max_brightness` | what |
| --- | --- | --- | --- |
| `deluge:indicator:0` … `deluge:indicator:35` | 36 | 1 | one per button, on/off |
| `deluge:gold0:0` … `deluge:gold0:3` | 4 | 255 | gold knob column 0 |
| `deluge:gold1:0` … `deluge:gold1:3` | 4 | 255 | gold knob column 1 |
| `deluge:sync` | 1 | 1 | the SYNC LED, a plain GPIO LED (`gpio-leds`, P6.7) |

The indicators and gold columns are driven through the PIC; the sync LED is not.

## CV / gate: `deluge-cv`

The MAX5136 DAC on RSPI0 drives the two CV outputs; four GPIOs drive the
gates.

CV is an IIO DAC named `deluge-cv` (currently `iio:device0`):

- `out_voltage0_raw`, `out_voltage1_raw`: read/write, 0–65535. Reads return
  the last value written.
- `out_voltage_scale`: millivolts per count, 1000/6552 (about 0.1526). 6552
  counts is 1 V, so 65520 is 10 V; 0 is 0 V. The outputs are unipolar.

Gates are GPIO lines named `deluge-gate0` to `deluge-gate3` (P2.7, P2.8, P2.9,
P4.0). They are V-trig outputs and electrically active-low: driving the pin low
turns the gate on. The device tree doesn't mark them active-low, so request
them with `GPIO_V2_LINE_FLAG_OUTPUT | GPIO_V2_LINE_FLAG_ACTIVE_LOW` through the
GPIO character device (`/dev/gpiochip*`, `<linux/gpio.h>` v2 uAPI); a logical 1
is then gate on. The lines are on different GPIO chips, so look each one up by
name.

## Fast SRAM: `deluge-sram`

Exposes on-chip SRAM to userspace so an audio engine can keep hot data off
SDRAM.

- `/dev/deluge-sram` (a misc device)
- 1 MB at `0x20200000`, cached, CPU-only. Don't use it as a DMA target.
- `mmap()` it, the whole region from offset 0
- `DELUGE_SRAM_GET_INFO` reports the region's physical base and size, so you
  don't have to hard-code them. The header is
  `board/synthstrom/deluge/kernel/include/uapi/deluge-sram.h`:

  ```c
  struct deluge_sram_info {
      __u32 phys;   /* physical base address of the region */
      __u32 size;   /* region size in bytes */
  };
  #define DELUGE_SRAM_IOC_MAGIC 'D'
  #define DELUGE_SRAM_GET_INFO  _IOR(DELUGE_SRAM_IOC_MAGIC, 1, struct deluge_sram_info)
  ```

1 MB rather than the chip's full 3 MB. [hardware.md](hardware.md#things-that-bite)
covers what the rest is doing.

## USB gadget

Configured at boot by `/usr/sbin/deluge-gadget` through configfs. The composite
enumerates as `1d6b:0104`:

| function | what it is | profile |
| --- | --- | --- |
| CDC-ACM | `ttyGS0`: kernel console in both profiles, plus a login on `platform` | both |
| UAC2 | USB audio, stereo playback + capture, 44.1 kHz only, 16-bit | both |
| `f_midi2` | USB MIDI 2.0 / 1.0 | both |
| NCM | USB ethernet (`usb0`, 192.168.7.2/24) | `platform` only |

The `base` kernel is built without networking (`CONFIG_NET` off), so NCM can't
exist there.

The RZ/A1L's USB controller has only two ISO-capable pipes, which is why UAC2
runs in implicit-feedback mode. See the patches in
`board/synthstrom/deluge/kernel/patches/`.

USB is a single jack shared with host mode. VBUS sensing can't detect the
role, so it is switched explicitly through the `role` attribute that
`renesas-usbhs-role-switch.patch` adds to the controller (USBHS channel 0,
`e8010000.usb`):

```sh
cat /sys/bus/platform/devices/e8010000.usb/role          # host | peripheral
echo host > /sys/bus/platform/devices/e8010000.usb/role
```

The default is `peripheral`. `/sys/bus/platform/devices/e8010000.usb` is a
symlink to the device's place under `/sys/devices/platform/`.
