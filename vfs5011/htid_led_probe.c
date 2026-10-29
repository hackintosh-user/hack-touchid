/*
 * htid_led_probe.c -- try the VFS5011 LED by hand, without the daemon.
 *
 * Build (same flags as the daemon, run from the repo root):
 *   clang vfs5011/htid_led_probe.c -o htid_led_probe -Ivfs5011 \
 *       -I/usr/local/include/libusb-1.0 -L/usr/local/lib -lusb-1.0
 *
 * Usage:
 *   ./htid_led_probe off
 *   ./htid_led_probe ready                 idle blink on port 0 (what the sensor shows while waiting)
 *   ./htid_led_probe red [port] [pct]      steady pulse, 3 s, default port 2, default 100 %
 *   ./htid_led_probe ports [pct]           walks ports 0..3, 3 s each, so you can see which one is red
 *   ./htid_led_probe idle MASK [duty] [blink_ms] [pct]
 *        runs a pattern for 10 s on several ports at once. MASK is a number: 1 = port 0,
 *        2 = port 1, 4 = port 2, 8 = port 3, add them (3 = ports 0 and 1). duty 100 = steady on,
 *        50 = half the time. Defaults: duty 50, blink 1000 ms, 100 %. Try to find a brighter
 *        waiting light, e.g. "idle 1 100" (steady port 0) or "idle 3 50" (ports 0 and 1).
 *   add --init before the command to replay the first steps of the daemon's init
 *   (version and info reads) first, in case the sensor needs to be awake.
 *
 * The sensor must not be in use: leave the lock screen idle and do not
 * swipe while this runs. It prints the 2-byte status of every command;
 * 00 00 means the sensor accepted it.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <libusb.h>

#include "vfs5011_proto.h"
#include "vfs5011_led.h"

#define VID 0x138a
#define PID 0x0018

static int xfer_out(libusb_device_handle *h, const unsigned char *d, int len) {
    int n = 0;
    int r = libusb_bulk_transfer(h, VFS5011_OUT_ENDPOINT, (unsigned char *)d, len, &n, 2000);
    if (r == LIBUSB_ERROR_PIPE) { libusb_clear_halt(h, VFS5011_OUT_ENDPOINT); r = libusb_bulk_transfer(h, VFS5011_OUT_ENDPOINT, (unsigned char *)d, len, &n, 2000); }
    if (r != 0 || n != len) { fprintf(stderr, "  send failed: %s (%d of %d)\n", libusb_error_name(r), n, len); return -1; }
    return 0;
}

static int xfer_in(libusb_device_handle *h, unsigned char *buf, int len, int *got) {
    int r = libusb_bulk_transfer(h, VFS5011_IN_ENDPOINT_CTRL, buf, len, got, 2000);
    if (r == LIBUSB_ERROR_PIPE) { libusb_clear_halt(h, VFS5011_IN_ENDPOINT_CTRL); r = libusb_bulk_transfer(h, VFS5011_IN_ENDPOINT_CTRL, buf, len, got, 2000); }
    if (r != 0) { fprintf(stderr, "  read failed: %s\n", libusb_error_name(r)); return -1; }
    return 0;
}

static int send_led(libusb_device_handle *h, const uint8_t *pkt, const char *what) {
    unsigned char rsp[64];
    int got = 0;
    printf("-> %s\n", what);
    if (xfer_out(h, pkt, VFS5011_LED_PKT_LEN) != 0) return -1;
    if (xfer_in(h, rsp, sizeof(rsp), &got) != 0) return -1;
    printf("   status:");
    for (int i = 0; i < got && i < 8; i++) printf(" %02x", rsp[i]);
    printf("%s\n", (got >= 2 && rsp[0] == 0 && rsp[1] == 0) ? "   (accepted)" : "   (NOT the normal 00 00 reply)");
    return 0;
}

static void replay_init(libusb_device_handle *h) {
    unsigned char rsp[256];
    int got = 0;
    printf("-> init: cmd 01\n");
    if (xfer_out(h, vfs5011_cmd_01, sizeof(vfs5011_cmd_01)) == 0 && xfer_in(h, rsp, 64, &got) == 0) {
        printf("   %d bytes:", got);
        for (int i = 0; i < got && i < 32; i++) printf(" %02x", rsp[i]);
        printf("\n");
    }
    printf("-> init: cmd 19\n");
    if (xfer_out(h, vfs5011_cmd_19, sizeof(vfs5011_cmd_19)) == 0) {
        if (xfer_in(h, rsp, 64, &got) == 0) printf("   reply 1: %d bytes\n", got);
        if (xfer_in(h, rsp, 64, &got) == 0) printf("   reply 2: %d bytes\n", got);
    }
}

int main(int argc, char **argv) {
    int ai = 1, do_init = 0;
    if (ai < argc && strcmp(argv[ai], "--init") == 0) { do_init = 1; ai++; }
    if (ai >= argc) { fprintf(stderr, "usage: %s [--init] off | ready | red [port] [pct] | ports [pct] | idle MASK [duty] [blink_ms] [pct]\n", argv[0]); return 2; }
    const char *cmd = argv[ai++];

    libusb_context *ctx = NULL;
    if (libusb_init(&ctx) < 0) { fprintf(stderr, "libusb_init failed\n"); return 1; }
    libusb_device_handle *h = libusb_open_device_with_vid_pid(ctx, VID, PID);
    if (!h) { fprintf(stderr, "VFS5011 (138a:0018) not found, or something else holds it. Is the daemon scanning?\n"); libusb_exit(ctx); return 1; }
    libusb_set_auto_detach_kernel_driver(h, 1);
    if (libusb_claim_interface(h, 0) != 0) { fprintf(stderr, "could not claim the interface\n"); libusb_close(h); libusb_exit(ctx); return 1; }

    int rc = 0;
    uint8_t pkt[VFS5011_LED_PKT_LEN];
    if (do_init) replay_init(h);

    if (strcmp(cmd, "off") == 0) {
        vfs5011_led_build_off(pkt);
        rc = send_led(h, pkt, "all off");
    } else if (strcmp(cmd, "ready") == 0) {
        memcpy(pkt, vfs5011_init_17, VFS5011_LED_PKT_LEN);
        rc = send_led(h, pkt, "ready blink (captured driver packet)");
    } else if (strcmp(cmd, "red") == 0) {
        int port = ai < argc ? atoi(argv[ai++]) : VFS5011_LED_PORT_ATTENTION;
        int pct = ai < argc ? atoi(argv[ai++]) : 100;
        if (vfs5011_led_build(pkt, port, pct, 0, 500, 100, 3) != 0) { fprintf(stderr, "bad port or percent\n"); rc = 1; }
        else { char what[64]; snprintf(what, sizeof(what), "steady pulse, port %d, %d%%, 3 s", port, pct); rc = send_led(h, pkt, what); sleep(4); }
    } else if (strcmp(cmd, "idle") == 0) {
        int mask = ai < argc ? atoi(argv[ai++]) : 1;
        int duty = ai < argc ? atoi(argv[ai++]) : 50;
        int blink = ai < argc ? atoi(argv[ai++]) : 1000;
        int pct = ai < argc ? atoi(argv[ai++]) : 100;
        if (vfs5011_led_build_mask(pkt, mask, pct, 0, blink, duty, 10) != 0) { fprintf(stderr, "bad mask (1..15), duty, blink or percent\n"); rc = 1; }
        else { char what[96]; snprintf(what, sizeof(what), "mask %d, duty %d%%, blink %d ms, %d%%, 10 s", mask, duty, blink, pct); rc = send_led(h, pkt, what); sleep(11); }
    } else if (strcmp(cmd, "ports") == 0) {
        int pct = ai < argc ? atoi(argv[ai++]) : 100;
        for (int port = 0; port < 4 && rc == 0; port++) {
            if (vfs5011_led_build(pkt, port, pct, 0, 500, 100, 3) != 0) { rc = 1; break; }
            char what[64]; snprintf(what, sizeof(what), "port %d, %d%%, 3 s -- note the color", port, pct);
            rc = send_led(h, pkt, what);
            sleep(4);
        }
    } else {
        fprintf(stderr, "unknown command: %s\n", cmd);
        rc = 2;
    }

    libusb_clear_halt(h, VFS5011_IN_ENDPOINT_CTRL);
    libusb_clear_halt(h, VFS5011_OUT_ENDPOINT);
    libusb_release_interface(h, 0);
    libusb_close(h);
    libusb_exit(ctx);
    return rc ? 1 : 0;
}
