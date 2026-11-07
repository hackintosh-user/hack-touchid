/*
 * vfs7552_probe.c
 *
 * See vfs7552_probe.h. Read-only probe for the Synaptics VFS7552
 * (138a:0092). Only needs libusb.
 *
 * What it checks, in order:
 *   1. the sensor enumerates (VID:PID 138a:0092) and what its device and
 *      interface/endpoint descriptors look like;
 *   2. interface 0 can be claimed;
 *   3. RomInfo (0x01), 0x19 and the fwext info query (0x43 0x02) get
 *      answered on bulk endpoints 0x01 (OUT) / 0x81 (IN), the same
 *      endpoints python-validity's usb.py uses for every Prometheus-family
 *      sensor.
 * A tester's log of this is enough to decide whether 0092 really speaks
 * the same protocol as the Metallica MIS / Prometheus backend.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>

#include <libusb.h>

#include "vfs7552_probe.h"

#define PROBE_EP_OUT_DEFAULT 0x01
#define PROBE_EP_IN_DEFAULT  0x81
#define PROBE_IN_BUF_SIZE    (64 * 1024)
#define PROBE_TIMEOUT_MS     3000

static int g_dbg = 0;

static void dump_hex(const char *label, const unsigned char *buf, int len) {
    int limit = len;
    if (g_dbg < 2 && limit > 64) limit = 64;
    printf("%s (%d bytes):", label, len);
    for (int i = 0; i < limit; i++) {
        if (i % 16 == 0) printf("\n    ");
        printf("%02x ", buf[i]);
    }
    if (limit < len) printf("\n    ... (%d more bytes, use --debug-full to see all)", len - limit);
    printf("\n");
}

static const char *xfer_name(uint8_t attr) {
    switch (attr & LIBUSB_TRANSFER_TYPE_MASK) {
        case LIBUSB_TRANSFER_TYPE_CONTROL:     return "control";
        case LIBUSB_TRANSFER_TYPE_ISOCHRONOUS: return "isochronous";
        case LIBUSB_TRANSFER_TYPE_BULK:        return "bulk";
        case LIBUSB_TRANSFER_TYPE_INTERRUPT:   return "interrupt";
        default:                               return "?";
    }
}

/* Prints every interface/endpoint and picks the first bulk OUT and bulk IN
 * endpoint of interface 0. Leaves the defaults in place if nothing is found. */
static void print_config(libusb_device *dev, uint8_t *ep_out, uint8_t *ep_in) {
    struct libusb_config_descriptor *cfg = NULL;
    int r = libusb_get_active_config_descriptor(dev, &cfg);
    if (r != 0) r = libusb_get_config_descriptor(dev, 0, &cfg);
    if (r != 0 || !cfg) {
        printf("config descriptor: unavailable (%s)\n", libusb_error_name(r));
        return;
    }
    printf("config: value=%d interfaces=%d maxPower=%d mA\n",
           cfg->bConfigurationValue, cfg->bNumInterfaces, cfg->MaxPower * 2);
    int found_out = 0, found_in = 0;
    for (int i = 0; i < cfg->bNumInterfaces; i++) {
        const struct libusb_interface *ifc = &cfg->interface[i];
        for (int a = 0; a < ifc->num_altsetting; a++) {
            const struct libusb_interface_descriptor *alt = &ifc->altsetting[a];
            printf("  interface %d alt %d: class=0x%02x subclass=0x%02x protocol=0x%02x endpoints=%d\n",
                   alt->bInterfaceNumber, alt->bAlternateSetting, alt->bInterfaceClass,
                   alt->bInterfaceSubClass, alt->bInterfaceProtocol, alt->bNumEndpoints);
            for (int e = 0; e < alt->bNumEndpoints; e++) {
                const struct libusb_endpoint_descriptor *ep = &alt->endpoint[e];
                printf("    endpoint 0x%02x %s %s maxPacket=%d interval=%d\n",
                       ep->bEndpointAddress,
                       (ep->bEndpointAddress & 0x80) ? "IN " : "OUT",
                       xfer_name(ep->bmAttributes), ep->wMaxPacketSize, ep->bInterval);
                if (alt->bInterfaceNumber == 0 && a == 0 &&
                    (ep->bmAttributes & LIBUSB_TRANSFER_TYPE_MASK) == LIBUSB_TRANSFER_TYPE_BULK) {
                    if (!(ep->bEndpointAddress & 0x80) && !found_out) { *ep_out = ep->bEndpointAddress; found_out = 1; }
                    if ((ep->bEndpointAddress & 0x80) && !found_in)   { *ep_in = ep->bEndpointAddress; found_in = 1; }
                }
            }
        }
    }
    libusb_free_config_descriptor(cfg);
    if (!found_out || !found_in)
        printf("note: no bulk IN/OUT pair found on interface 0, falling back to 0x%02x / 0x%02x\n",
               *ep_out, *ep_in);
    else if (*ep_out != PROBE_EP_OUT_DEFAULT || *ep_in != PROBE_EP_IN_DEFAULT)
        printf("note: endpoints differ from python-validity's 0x%02x / 0x%02x\n",
               PROBE_EP_OUT_DEFAULT, PROBE_EP_IN_DEFAULT);
}

/* One command round trip. Returns the reply length (>= 0), or a negative
 * libusb error code. */
static int probe_cmd(libusb_device_handle *h, uint8_t ep_out, uint8_t ep_in,
                     const unsigned char *cmd, int cmd_len,
                     unsigned char *rsp, int rsp_max, const char *name) {
    int sent = 0, got = 0;
    printf("\n> %s\n", name);
    dump_hex("  sent", cmd, cmd_len);
    int r = libusb_bulk_transfer(h, ep_out, (unsigned char *)cmd, cmd_len, &sent, PROBE_TIMEOUT_MS);
    if (r != 0) {
        printf("  write failed: %s\n", libusb_error_name(r));
        return r;
    }
    r = libusb_bulk_transfer(h, ep_in, rsp, rsp_max, &got, PROBE_TIMEOUT_MS);
    if (r != 0) {
        printf("  read failed: %s\n", libusb_error_name(r));
        return r;
    }
    dump_hex("  reply", rsp, got);
    if (got >= 3 && rsp[0] == 0x15 && rsp[1] == 0x03 && rsp[2] == 0x03) {
        printf("  note: this looks like a TLS alert, the sensor still holds an old session.\n"
               "        Reboot or replug the sensor (shut down fully, not just restart) and run again.\n");
    } else if (got >= 2) {
        unsigned status = (unsigned)rsp[0] | ((unsigned)rsp[1] << 8);
        printf("  status word: 0x%04x%s\n", status, status == 0 ? " (OK)" : "");
    }
    return got;
}

int vfs7552_probe_run(int debug) {
    g_dbg = debug;
    printf("VFS7552 probe (138a:0092), read-only, nothing is written to the sensor's flash\n");

    libusb_context *ctx = NULL;
    if (libusb_init(&ctx) < 0) {
        printf("libusb_init failed\n");
        return 3;
    }
    libusb_set_option(ctx, LIBUSB_OPTION_LOG_LEVEL,
                      debug ? LIBUSB_LOG_LEVEL_DEBUG : LIBUSB_LOG_LEVEL_NONE);

    libusb_device **list = NULL;
    ssize_t n = libusb_get_device_list(ctx, &list);
    libusb_device *found = NULL;
    for (ssize_t i = 0; i < n; i++) {
        struct libusb_device_descriptor d;
        if (libusb_get_device_descriptor(list[i], &d) != 0) continue;
        if (d.idVendor == VFS7552_VID && d.idProduct == VFS7552_PID) { found = list[i]; break; }
    }
    if (!found) {
        printf("sensor 138a:0092 not found on the USB bus.\n"
               "Check that it shows up in System Information > USB and is powered in your USBMap.\n");
        libusb_free_device_list(list, 1);
        libusb_exit(ctx);
        return 2;
    }

    struct libusb_device_descriptor d;
    libusb_get_device_descriptor(found, &d);
    printf("device: %04x:%04x bcdUSB=%x.%02x bcdDevice=0x%04x class=0x%02x subclass=0x%02x protocol=0x%02x "
           "maxPacket0=%d configs=%d\n",
           d.idVendor, d.idProduct, d.bcdUSB >> 8, d.bcdUSB & 0xff, d.bcdDevice,
           d.bDeviceClass, d.bDeviceSubClass, d.bDeviceProtocol, d.bMaxPacketSize0, d.bNumConfigurations);

    uint8_t ep_out = PROBE_EP_OUT_DEFAULT, ep_in = PROBE_EP_IN_DEFAULT;
    print_config(found, &ep_out, &ep_in);

    libusb_device_handle *h = NULL;
    int r = libusb_open(found, &h);
    libusb_free_device_list(list, 1);
    if (r != 0 || !h) {
        printf("\nopen failed: %s (run with sudo, and close anything else using the sensor)\n",
               libusb_error_name(r));
        libusb_exit(ctx);
        return 1;
    }
    libusb_set_auto_detach_kernel_driver(h, 1);

    r = libusb_claim_interface(h, 0);
    for (int i = 0; r != 0 && i < 3; i++) {
        usleep(150000);
        r = libusb_claim_interface(h, 0);
    }
    printf("\nclaim interface 0: %s\n", libusb_error_name(r));
    if (r != 0) {
        libusb_close(h);
        libusb_exit(ctx);
        return 1;
    }

    unsigned char *rsp = (unsigned char *)malloc(PROBE_IN_BUF_SIZE);
    if (!rsp) {
        libusb_release_interface(h, 0);
        libusb_close(h);
        libusb_exit(ctx);
        return 1;
    }

    int rc = 1;
    static const unsigned char cmd_rom[]  = { 0x01 };
    static const unsigned char cmd_19[]   = { 0x19 };
    static const unsigned char cmd_fw[]   = { 0x43, 0x02 };

    int got = probe_cmd(h, ep_out, ep_in, cmd_rom, sizeof(cmd_rom), rsp, PROBE_IN_BUF_SIZE,
                        "RomInfo (0x01)");
    if (got >= 2 + 0x10 && rsp[0] == 0 && rsp[1] == 0) {
        const unsigned char *p = rsp + 2;
        uint32_t ts    = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
        uint32_t build = (uint32_t)p[4] | ((uint32_t)p[5] << 8) | ((uint32_t)p[6] << 16) | ((uint32_t)p[7] << 24);
        printf("  rom_info: timestamp=%u build=%u major=%u minor=%u product=0x%02x u1=%u\n",
               ts, build, p[8], p[9], p[11], p[15]);
        rc = 0;
    } else if (got >= 0) {
        rc = 1;
    }

    /* The other two queries only make sense once RomInfo got a real answer,
     * but they are harmless either way, so a failure here never changes rc. */
    probe_cmd(h, ep_out, ep_in, cmd_19, sizeof(cmd_19), rsp, PROBE_IN_BUF_SIZE, "query (0x19)");
    probe_cmd(h, ep_out, ep_in, cmd_fw, sizeof(cmd_fw), rsp, PROBE_IN_BUF_SIZE,
              "fwext info (0x43 0x02)");

    printf("\nprobe finished, result: %s\n",
           rc == 0 ? "sensor answered RomInfo" : "sensor did NOT answer RomInfo as expected");
    free(rsp);
    libusb_release_interface(h, 0);
    libusb_close(h);
    libusb_exit(ctx);
    return rc;
}
