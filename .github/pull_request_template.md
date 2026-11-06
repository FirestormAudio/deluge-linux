<!-- Edit board/synthstrom/deluge/, not .buildroot/ — see CONTRIBUTING.md. -->

## What and why

## Verification

- [ ] `python3 -m pytest tests/ -v`
- [ ] `make base`
- [ ] `make platform`
- [ ] Booted on hardware; `dmesg | grep -i deluge` shows every driver probing

Hardware tested on:

<!--
CI builds both profiles and runs the tests, but can't boot anything. If you
couldn't test on hardware, say so — someone else will before this merges.

If this changes anything in docs/drivers.md (a device node, an ALSA/evdev/IIO
interface, a sysfs path), note it: libdeluge is written against that contract.
-->
