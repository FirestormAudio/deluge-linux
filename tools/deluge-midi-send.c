/* deluge-midi-send — send MIDI to the Deluge USB gadget from a host by writing
 * UMP packets straight to its MIDI-2.0 OUT bulk endpoint via libusb.
 *
 * Why: on some hosts snd-usb-audio's UMP *transmit* path doesn't send to the
 * f_midi2 gadget (its receive path works, so device->host MIDI is fine via
 * ALSA, but host->device isn't). This bypasses snd-usb-audio entirely and talks
 * to the raw endpoint, so a host can drive the Deluge's MIDI regardless.
 *
 * Each argument is one byte (hex) of a MIDI 1.0 channel-voice message; the bytes
 * are wrapped in a single UMP "MIDI 1.0 Channel Voice" (MT2) packet on group 0
 * and sent little-endian (the USB MIDI 2.0 wire order). With no args it sends a
 * note-on/off test on note 0x40.
 *
 *   build:  cc deluge-midi-send.c $(pkg-config --cflags --libs libusb-1.0) -O2 -o deluge-midi-send
 *   run:    sudo ./deluge-midi-send            # test note
 *           sudo ./deluge-midi-send 90 40 7f   # note-on ch0 note64 vel127
 *           sudo ./deluge-midi-send b0 07 64    # CC7 (volume) = 100
 */
#include <libusb-1.0/libusb.h>
#include <stdio.h>
#include <stdlib.h>

#define VID 0x1d6b
#define PID 0x0104
#define MIDI_IF 6
#define ALT 1

/* Wrap a 1-3 byte MIDI 1.0 message in a UMP MT2 word and send it little-endian. */
static int send_msg(libusb_device_handle *h, unsigned char ep,
                    unsigned char s, unsigned char d1, unsigned char d2) {
    unsigned char be[4] = { 0x20, s, d1, d2 };   /* MT2, group 0, status, data */
    unsigned char le[4] = { be[3], be[2], be[1], be[0] };
    int sent;
    return libusb_bulk_transfer(h, ep, le, 4, &sent, 500);
}

int main(int argc, char **argv) {
    libusb_context *ctx = NULL;
    if (libusb_init(&ctx)) { fprintf(stderr, "libusb_init failed\n"); return 1; }
    libusb_device_handle *h = libusb_open_device_with_vid_pid(ctx, VID, PID);
    if (!h) { fprintf(stderr, "Deluge %04x:%04x not found (run as root?)\n", VID, PID); return 1; }

    libusb_set_auto_detach_kernel_driver(h, 1);
    int r = libusb_claim_interface(h, MIDI_IF);
    if (r) { fprintf(stderr, "claim iface %d: %s\n", MIDI_IF, libusb_error_name(r)); return 1; }
    libusb_set_interface_alt_setting(h, MIDI_IF, ALT);

    /* find the OUT bulk endpoint of the MIDI interface's operational alt */
    struct libusb_config_descriptor *cfg;
    unsigned char ep_out = 0;
    if (!libusb_get_active_config_descriptor(libusb_get_device(h), &cfg)) {
        for (int i = 0; i < cfg->bNumInterfaces; i++)
            for (int a = 0; a < cfg->interface[i].num_altsetting; a++) {
                const struct libusb_interface_descriptor *id = &cfg->interface[i].altsetting[a];
                if (id->bInterfaceNumber != MIDI_IF || id->bAlternateSetting != ALT) continue;
                for (int e = 0; e < id->bNumEndpoints; e++)
                    if (!(id->endpoint[e].bEndpointAddress & 0x80) &&
                        (id->endpoint[e].bmAttributes & 3) == LIBUSB_TRANSFER_TYPE_BULK)
                        ep_out = id->endpoint[e].bEndpointAddress;
            }
        libusb_free_config_descriptor(cfg);
    }
    if (!ep_out) { fprintf(stderr, "no OUT bulk endpoint on iface %d alt %d\n", MIDI_IF, ALT); return 1; }

    if (argc > 1) {
        unsigned char b[3] = {0, 0, 0};
        int n = argc - 1 > 3 ? 3 : argc - 1;
        for (int i = 0; i < n; i++) b[i] = (unsigned char)strtol(argv[i + 1], 0, 16);
        r = send_msg(h, ep_out, b[0], b[1], b[2]);
        if (r) fprintf(stderr, "bulk out: %s\n", libusb_error_name(r));
        else printf("sent %02x %02x %02x -> ep 0x%02x\n", b[0], b[1], b[2], ep_out);
    } else {
        for (int i = 0; i < 4; i++) {
            send_msg(h, ep_out, 0x90, 0x40, 0x7f);
            send_msg(h, ep_out, 0x80, 0x40, 0x00);
        }
        printf("sent 4 note-on/off pairs (note 0x40) -> ep 0x%02x\n", ep_out);
    }

    libusb_release_interface(h, MIDI_IF);
    libusb_close(h);
    libusb_exit(ctx);
    return 0;
}
