/*
 * elanmoc2_probe.c
 *
 * See elanmoc2_probe.h. Read-only probe for the ELAN WBF / ELAN:ARM-M4
 * match-on-chip sensor (04f3:0c4c). Only needs libusb.
 *
 * Copyright (C) 2026 Mohammad
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * Where the protocol comes from: the elanmoc2 driver for libfprint by Davide
 * Depau (LGPL 2.1 or later), which he reverse engineered from captures of the
 * official Windows driver, with the Python proof of concept at
 * github.com/depau/Elan-Fingerprint-0c4c-PoC. It documents the command
 * framing (0x40, then the command bytes, zero padded), the bulk endpoints and
 * the reply rule (a reply starts with 0x40). This file is a fresh
 * implementation of that documented framing, nothing is copied from it.
 * That driver is known to work on Linux, so every answer this probe gets on
 * macOS is a finding about macOS access, not about the protocol itself.
 *
 * What it checks, in order:
 *   1. the sensor enumerates (VID:PID 04f3:0c4c) and what its device and
 *      interface/endpoint descriptors look like;
 *   2. interface 0 can be claimed;
 *   3. two fixed query commands get answered on the bulk endpoints:
 *      the firmware version and the number of enrolled fingers. Only the
 *      count is read, the finger identifiers are never requested.
 * A tester's log of this is enough to decide whether the sensor can be
 * driven from macOS the same way, and what the firmware answers.
 *
 * The command table below is the ONLY thing this file ever sends over bulk.
 * There is deliberately no code path that builds an identify, enroll, commit,
 * delete, abort or wipe command, and no USB reset is sent (the Linux driver
 * resets the device when it opens it, this probe does not).
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>

#include <libusb.h>

#include "elanmoc2_probe.h"

/* Endpoints used by the Linux driver for the quick commands. The real ones
 * are checked against the descriptors and a note is printed if they differ. */
#define PROBE_EP_OUT           0x01
#define PROBE_EP_IN            0x83
#define PROBE_EP_MOC_IN        0x84   /* only listed, never read */
#define PROBE_IN_BUF_SIZE      512
#define PROBE_TIMEOUT_MS       3000
#define PROBE_MAX_ATTEMPTS     3
#define PROBE_OUT_MAX          8

#define ELANMOC2_CMD_BYTE      0x40   /* first byte of every command and reply */

/* The only commands this probe ever sends. A command packet is 0x40, the
 * command bytes, then zero padding up to out_len. */
typedef struct {
    const char *name;
    unsigned char cmd[2];
    int cmd_len;
    int out_len;
    int is_count;
} probe_cmd_t;

static const probe_cmd_t PROBE_CMDS[] = {
    { "firmware version query (cmd 0x19)",          { 0x19, 0x00 }, 1, 2, 0 },
    { "enrolled finger count query (cmd 0xff 0x04)", { 0xff, 0x04 }, 2, 3, 1 },
};
#define PROBE_CMD_COUNT ((int)(sizeof(PROBE_CMDS) / sizeof(PROBE_CMDS[0])))

/* ---- framing ---------------------------------------------------------- */

/* Builds one command packet into out (PROBE_OUT_MAX bytes). Returns the
 * packet length, or 0 if the command does not fit. */
static int elanmoc2_build_packet(unsigned char *out, const probe_cmd_t *c) {
    if (c->out_len < 1 + c->cmd_len || c->out_len > PROBE_OUT_MAX) return 0;
    memset(out, 0, PROBE_OUT_MAX);
    out[0] = ELANMOC2_CMD_BYTE;
    memcpy(out + 1, c->cmd, (size_t)c->cmd_len);
    return c->out_len;
}

/* 1 if the reply starts with 0x40, the only framing rule the driver applies. */
static int elanmoc2_reply_ok(const unsigned char *rsp, int len) {
    return len >= 1 && rsp[0] == ELANMOC2_CMD_BYTE;
}

#ifndef ELANMOC2_PROBE_SELFTEST

static int g_dbg = 0;

/* ---- printing --------------------------------------------------------- */

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

/* Prints every interface/endpoint of the device and checks that the bulk
 * endpoints the Linux driver uses (0x01 OUT, 0x83 IN) exist on interface 0. */
static void print_config(libusb_device *dev) {
    struct libusb_config_descriptor *cfg = NULL;
    int r = libusb_get_active_config_descriptor(dev, &cfg);
    if (r != 0) r = libusb_get_config_descriptor(dev, 0, &cfg);
    if (r != 0 || !cfg) {
        printf("config descriptor: unavailable (%s)\n", libusb_error_name(r));
        return;
    }
    printf("config: value=%d interfaces=%d maxPower=%d mA\n",
           cfg->bConfigurationValue, cfg->bNumInterfaces, cfg->MaxPower * 2);
    int has_out = 0, has_in = 0, has_moc = 0;
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
                if (alt->bInterfaceNumber != 0 || a != 0) continue;
                if ((ep->bmAttributes & LIBUSB_TRANSFER_TYPE_MASK) != LIBUSB_TRANSFER_TYPE_BULK) continue;
                if (ep->bEndpointAddress == PROBE_EP_OUT)    has_out = 1;
                if (ep->bEndpointAddress == PROBE_EP_IN)     has_in = 1;
                if (ep->bEndpointAddress == PROBE_EP_MOC_IN) has_moc = 1;
            }
        }
    }
    libusb_free_config_descriptor(cfg);
    if (!has_out || !has_in)
        printf("note: bulk endpoint 0x%02x and/or 0x%02x not found on interface 0, "
               "the Linux driver uses these two for its quick commands\n",
               PROBE_EP_OUT, PROBE_EP_IN);
    if (!has_moc)
        printf("note: bulk IN 0x%02x not found, the Linux driver uses it for enroll and identify "
               "replies (this probe never reads it)\n", PROBE_EP_MOC_IN);
}

static void print_strings(libusb_device_handle *h, const struct libusb_device_descriptor *d) {
    unsigned char s[128];
    if (d->iManufacturer && libusb_get_string_descriptor_ascii(h, d->iManufacturer, s, sizeof(s)) > 0)
        printf("manufacturer string: %s\n", (char *)s);
    if (d->iProduct && libusb_get_string_descriptor_ascii(h, d->iProduct, s, sizeof(s)) > 0)
        printf("product string: %s\n", (char *)s);
    /* The serial number string is left out on purpose (this sensor reports none). */
}

/* ---- bulk exchange ---------------------------------------------------- */

/* One command round trip with up to PROBE_MAX_ATTEMPTS tries: the Linux
 * driver retries the count query because some units occasionally answer with
 * an empty payload. Returns the reply length (>= 0) of the last attempt, or a
 * negative libusb error code if the write or read itself failed. */
static int probe_exchange(libusb_device_handle *h, const probe_cmd_t *c,
                          unsigned char *rsp, int rsp_max) {
    unsigned char pkt[PROBE_OUT_MAX];
    int pkt_len = elanmoc2_build_packet(pkt, c);
    int got = 0, r = 0;

    printf("\n> %s\n", c->name);
    if (pkt_len == 0) {
        printf("  internal error: command does not fit the packet buffer\n");
        return LIBUSB_ERROR_OTHER;
    }
    for (int attempt = 1; attempt <= PROBE_MAX_ATTEMPTS; attempt++) {
        int sent = 0;
        got = 0;
        if (attempt > 1) {
            printf("  empty or missing reply, trying again (attempt %d of %d)\n", attempt, PROBE_MAX_ATTEMPTS);
            usleep(100000);
        }
        dump_hex("  sent", pkt, pkt_len);
        r = libusb_bulk_transfer(h, PROBE_EP_OUT, pkt, pkt_len, &sent, PROBE_TIMEOUT_MS);
        if (r != 0) {
            printf("  write failed: %s\n", libusb_error_name(r));
            return r;
        }
        if (sent != pkt_len)
            printf("  note: only %d of %d bytes were sent\n", sent, pkt_len);
        r = libusb_bulk_transfer(h, PROBE_EP_IN, rsp, rsp_max, &got, PROBE_TIMEOUT_MS);
        if (r == 0 && got > 0) break;
        if (r != 0 && r != LIBUSB_ERROR_TIMEOUT) {
            printf("  read failed: %s\n", libusb_error_name(r));
            return r;
        }
    }
    if (r != 0) {
        printf("  read failed: %s\n", libusb_error_name(r));
        return r;
    }
    dump_hex("  reply", rsp, got);
    if (got == 0)
        printf("  reply: empty after %d attempts\n", PROBE_MAX_ATTEMPTS);
    else if (elanmoc2_reply_ok(rsp, got))
        printf("  framing: reply starts with 0x40 as expected\n");
    else
        printf("  framing: reply does NOT start with 0x40 (first byte 0x%02x)\n", rsp[0]);
    return got;
}

int elanmoc2_probe_run(int debug) {
    g_dbg = debug;
    printf("ELAN WBF probe (04f3:0c4c), read-only, nothing is written to the sensor's storage\n");

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
        if (d.idVendor == ELANMOC2_VID && d.idProduct == ELANMOC2_PID_0C4C) { found = list[i]; break; }
    }
    if (!found) {
        printf("sensor 04f3:0c4c not found on the USB bus.\n"
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

    print_config(found);

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
    print_strings(h, &d);

    int cfg_value = 0;
    if (libusb_get_configuration(h, &cfg_value) == 0 && cfg_value == 0) {
        printf("\nno active configuration, selecting configuration 1: %s\n",
               libusb_error_name(libusb_set_configuration(h, 1)));
    }

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
    int answered = 0;
    for (int i = 0; i < PROBE_CMD_COUNT; i++) {
        int got = probe_exchange(h, &PROBE_CMDS[i], rsp, PROBE_IN_BUF_SIZE);
        if (got > 0 && elanmoc2_reply_ok(rsp, got)) {
            answered++;
            rc = 0;
            /* The count query: byte 1 of the reply is the number of enrolled
             * fingers (only the number, never who they are). */
            if (PROBE_CMDS[i].is_count) {
                if (got >= 2)
                    printf("  enrolled fingers on the sensor: %d\n", rsp[1]);
                else
                    printf("  enrolled finger count: reply too short (%d bytes), see the length above\n", got);
            }
        }
        /* A timeout or a bad reply on one query does not stop the other, but
         * if nothing answered after the first one there is no point going on
         * with a second command on a silent sensor. */
        if (i == 0 && answered == 0) {
            printf("\nno answer to the first query, stopping here.\n");
            break;
        }
    }

    printf("\nprobe finished, result: %s (%d of %d queries answered with a 0x40 reply)\n",
           rc == 0 ? "sensor answered" : "sensor did NOT answer as expected", answered, PROBE_CMD_COUNT);
    if (rc != 0)
        printf("This does not mean the sensor is unsupported. It may need a different start-up step first,\n"
               "or something on the system may be holding the device. Please post this whole output in the GitHub issue.\n");
    free(rsp);
    libusb_release_interface(h, 0);
    libusb_close(h);
    libusb_exit(ctx);
    return rc;
}

#else /* ELANMOC2_PROBE_SELFTEST */

/* Framing self-test, no USB needed:
 *   cc -DELANMOC2_PROBE_SELFTEST -I. -I<libusb include dir> elanmoc2/elanmoc2_probe.c -o elanmoc2_selftest */
int main(void) {
    int bad = 0;
    static const unsigned char want_fw[]    = { 0x40, 0x19 };
    static const unsigned char want_count[] = { 0x40, 0xff, 0x04 };
    const unsigned char *want[] = { want_fw, want_count };
    const int want_len[] = { (int)sizeof(want_fw), (int)sizeof(want_count) };

    /* Every command must build to exactly the bytes the Linux driver sends,
     * and nothing beyond out_len may be touched. */
    for (int c = 0; c < PROBE_CMD_COUNT; c++) {
        unsigned char pkt[PROBE_OUT_MAX];
        int n = elanmoc2_build_packet(pkt, &PROBE_CMDS[c]);
        int ok = n == want_len[c] && memcmp(pkt, want[c], (size_t)n) == 0;
        printf("%-50s %s\n", PROBE_CMDS[c].name, ok ? "ok" : "FAIL");
        if (!ok) bad++;
    }
    /* A command longer than the buffer must be refused. */
    {
        probe_cmd_t big = { "oversized", { 0x01, 0x02 }, 2, PROBE_OUT_MAX + 1, 0 };
        unsigned char pkt[PROBE_OUT_MAX];
        int ok = elanmoc2_build_packet(pkt, &big) == 0;
        printf("%-50s %s\n", "oversized command refused", ok ? "ok" : "FAIL");
        if (!ok) bad++;
    }
    /* Reply rule: starts with 0x40. */
    {
        static const unsigned char good[]  = { 0x40, 0x00 };
        static const unsigned char other[] = { 0x00, 0x40 };
        int ok = elanmoc2_reply_ok(good, sizeof(good)) && !elanmoc2_reply_ok(other, sizeof(other)) &&
                 !elanmoc2_reply_ok(good, 0);
        printf("%-50s %s\n", "reply starts with 0x40", ok ? "ok" : "FAIL");
        if (!ok) bad++;
    }
    printf(bad ? "\n%d check(s) FAILED\n" : "\nall checks passed\n", bad);
    return bad ? 1 : 0;
}

#endif
