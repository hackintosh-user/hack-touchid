/*
 * egis_probe.c
 *
 * See egis_probe.h. Read-only probe for the EGIS ETU905A80-E (1c7a:05a1).
 * Only needs libusb.
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
 * Where the protocol comes from: libfprint's egismoc driver (Joshua
 * Grisham's EGIS match-on-chip work, LGPL 2.1 or later) and the 0586/0587
 * proof-of-concept helper scripts shipped in PikaOS' libfprint package,
 * which document the "EGIS"/"SIGE" packet framing, the check bytes and the
 * start-up sequence from Windows driver traces. This file is a fresh
 * implementation of that documented framing, nothing is copied from them.
 * Those scripts target 0586/0587, not 05a1, so every answer this probe gets
 * is a finding, not a guarantee.
 *
 * What it checks, in order:
 *   1. the sensor enumerates (VID:PID 1c7a:05a1) and what its device and
 *      interface/endpoint descriptors look like;
 *   2. interface 0 can be claimed;
 *   3. the IN-direction control reads the Windows driver sends first
 *      (GET_STATUS and two vendor requests), each one allowed to fail;
 *   4. four fixed query commands get answered on the bulk endpoints in the
 *      EGIS framing. The list of enrolled prints is only counted, the print
 *      identifiers themselves are never shown.
 * A tester's log of this is enough to decide whether 05a1 speaks the same
 * protocol as the sensors the PoCs were written for, and whether it needs
 * an SDCP step first.
 *
 * The command table below is the ONLY thing this file ever sends over bulk.
 * There is deliberately no code path that builds an enroll, verify or wipe
 * command.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>

#include <libusb.h>

#include "egis_probe.h"

/* Defaults taken from the 0586/0587 PoCs; the real endpoints are read from
 * the descriptors and these are only used when that fails. */
#define PROBE_EP_OUT_DEFAULT   0x02
#define PROBE_EP_IN_DEFAULT    0x81
#define PROBE_EP_INT_DEFAULT   0x83
#define PROBE_IN_BUF_SIZE      (64 * 1024)
#define PROBE_TIMEOUT_MS       3000
#define PROBE_CTRL_TIMEOUT_MS  1000

#define EGIS_PREFIX_LEN   8   /* "EGIS" or "SIGE" plus 00 00 00 01 */
#define EGIS_HDR_LEN      13  /* prefix, 2 check bytes, 00 00 00 */

static const unsigned char EGIS_WRITE_PREFIX[EGIS_PREFIX_LEN] = { 'E', 'G', 'I', 'S', 0x00, 0x00, 0x00, 0x01 };
static const unsigned char EGIS_READ_PREFIX[EGIS_PREFIX_LEN]  = { 'S', 'I', 'G', 'E', 0x00, 0x00, 0x00, 0x01 };

/* ---- framing ---------------------------------------------------------- */

/* Sum of all big-endian 16-bit words of buf (an odd last byte counts as the
 * high byte of a final word). A valid packet, check bytes included, sums to
 * a multiple of 0xFFFF; this holds for the packets sent to the sensor and
 * for the replies seen in the PoC traces. */
static uint32_t egis_word_sum(const unsigned char *buf, int len) {
    uint32_t sum = 0;
    for (int i = 0; i < len; i += 2) {
        unsigned hi = buf[i];
        unsigned lo = (i + 1 < len) ? buf[i + 1] : 0;
        sum += (hi << 8) | lo;
    }
    return sum;
}

/* Builds one command packet: EGIS 00 00 00 01, 2 check bytes, 00 00 00, then
 * the payload. payload[0] is the length of the rest of the payload, exactly
 * as in the PoC command strings. Returns the packet length. out must hold
 * EGIS_HDR_LEN + plen bytes. */
static int egis_build_packet(unsigned char *out, const unsigned char *payload, int plen) {
    memcpy(out, EGIS_WRITE_PREFIX, EGIS_PREFIX_LEN);
    out[8] = 0; out[9] = 0;
    out[10] = 0; out[11] = 0; out[12] = 0;
    memcpy(out + EGIS_HDR_LEN, payload, (size_t)plen);
    int total = EGIS_HDR_LEN + plen;
    uint32_t chk = 0xFFFFu - (egis_word_sum(out, total) % 0xFFFFu);
    out[8] = (unsigned char)(chk >> 8);
    out[9] = (unsigned char)(chk & 0xff);
    return total;
}

/* 1 if the reply starts with SIGE 00 00 00 01. */
static int egis_reply_has_prefix(const unsigned char *rsp, int len) {
    return len >= EGIS_HDR_LEN + 1 && memcmp(rsp, EGIS_READ_PREFIX, EGIS_PREFIX_LEN) == 0;
}

/* 1 if the reply's check bytes make the word sum a multiple of 0xFFFF. */
static int egis_reply_check_ok(const unsigned char *rsp, int len) {
    return (egis_word_sum(rsp, len) % 0xFFFFu) == 0;
}

/* The only commands this probe ever sends. Each payload is the PoC command
 * as it appears in the 0586/0587 start-up sequence, payload[0] being the
 * length of the rest. */
typedef struct {
    const char *name;
    unsigned char payload[8];
    int len;
    int quiet;
} probe_cmd_t;

static const probe_cmd_t PROBE_CMDS[] = {
    { "query 0x7f (start-up query, 12 byte answer expected)", { 0x07, 0x50, 0x7f, 0x00, 0x00, 0x00, 0x00, 0x0c }, 8, 0 },
    { "query 0x43 (start-up query, 4 byte answer expected)",  { 0x07, 0x50, 0x43, 0x00, 0x00, 0x00, 0x00, 0x04 }, 8, 0 },
    { "query 0x07 (start-up query, 0x1d byte answer expected)", { 0x07, 0x50, 0x07, 0x00, 0x02, 0x00, 0x00, 0x1d }, 8, 0 },
    { "query 0x19 (enrolled print list)",                      { 0x07, 0x50, 0x19, 0x04, 0x00, 0x00, 0x01, 0x40 }, 8, 1 },
};
#define PROBE_CMD_COUNT ((int)(sizeof(PROBE_CMDS) / sizeof(PROBE_CMDS[0])))

#ifndef EGIS_PROBE_SELFTEST

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

/* Prints every interface/endpoint and picks the first bulk OUT, bulk IN and
 * interrupt IN endpoint of interface 0. Leaves the defaults in place for
 * whatever is not found. */
static void print_config(libusb_device *dev, uint8_t *ep_out, uint8_t *ep_in, uint8_t *ep_int) {
    struct libusb_config_descriptor *cfg = NULL;
    int r = libusb_get_active_config_descriptor(dev, &cfg);
    if (r != 0) r = libusb_get_config_descriptor(dev, 0, &cfg);
    if (r != 0 || !cfg) {
        printf("config descriptor: unavailable (%s)\n", libusb_error_name(r));
        return;
    }
    printf("config: value=%d interfaces=%d maxPower=%d mA\n",
           cfg->bConfigurationValue, cfg->bNumInterfaces, cfg->MaxPower * 2);
    int found_out = 0, found_in = 0, found_int = 0;
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
                int type = ep->bmAttributes & LIBUSB_TRANSFER_TYPE_MASK;
                int is_in = (ep->bEndpointAddress & 0x80) != 0;
                if (type == LIBUSB_TRANSFER_TYPE_BULK && !is_in && !found_out) { *ep_out = ep->bEndpointAddress; found_out = 1; }
                if (type == LIBUSB_TRANSFER_TYPE_BULK && is_in && !found_in)   { *ep_in = ep->bEndpointAddress; found_in = 1; }
                if (type == LIBUSB_TRANSFER_TYPE_INTERRUPT && is_in && !found_int) { *ep_int = ep->bEndpointAddress; found_int = 1; }
            }
        }
    }
    libusb_free_config_descriptor(cfg);
    if (!found_out || !found_in)
        printf("note: no bulk IN/OUT pair found on interface 0, falling back to 0x%02x / 0x%02x\n",
               *ep_out, *ep_in);
    else if (*ep_out != PROBE_EP_OUT_DEFAULT || *ep_in != PROBE_EP_IN_DEFAULT)
        printf("note: bulk endpoints differ from the 0586/0587 PoCs' 0x%02x / 0x%02x\n",
               PROBE_EP_OUT_DEFAULT, PROBE_EP_IN_DEFAULT);
    if (!found_int)
        printf("note: no interrupt IN endpoint found, the PoCs use 0x%02x for the finger-detect event\n", *ep_int);
    else if (*ep_int != PROBE_EP_INT_DEFAULT)
        printf("note: interrupt endpoint differs from the PoCs' 0x%02x\n", PROBE_EP_INT_DEFAULT);
}

/* ---- start-up control reads ------------------------------------------- */

/* One IN-direction control read. Prints the result; a failure is only
 * reported, it never stops the probe. */
static void probe_ctrl_in(libusb_device_handle *h, uint8_t bm, uint8_t req, uint16_t value,
                          uint16_t index, uint16_t len, const char *name) {
    unsigned char buf[64];
    if (len > sizeof(buf)) len = sizeof(buf);
    printf("\n> control read: %s\n", name);
    int r = libusb_control_transfer(h, bm, req, value, index, buf, len, PROBE_CTRL_TIMEOUT_MS);
    if (r < 0) {
        printf("  failed: %s (this one is allowed to fail)\n", libusb_error_name(r));
        return;
    }
    dump_hex("  reply", buf, r);
}

/* ---- bulk exchange ---------------------------------------------------- */

/* One command round trip. Returns the reply length (>= 0), or a negative
 * libusb error code. quiet = 1 prints the reply length and framing result
 * only, never the reply bytes (used for the enrolled print list, whose
 * 32 byte blocks identify enrollments and do not belong in a tester's log). */
static int probe_exchange(libusb_device_handle *h, uint8_t ep_out, uint8_t ep_in,
                          const unsigned char *payload, int plen,
                          unsigned char *rsp, int rsp_max, const char *name, int quiet) {
    unsigned char pkt[64];
    int sent = 0, got = 0;
    int pkt_len = egis_build_packet(pkt, payload, plen);

    printf("\n> %s\n", name);
    dump_hex("  sent", pkt, pkt_len);
    int r = libusb_bulk_transfer(h, ep_out, pkt, pkt_len, &sent, PROBE_TIMEOUT_MS);
    if (r != 0) {
        printf("  write failed: %s\n", libusb_error_name(r));
        return r;
    }
    r = libusb_bulk_transfer(h, ep_in, rsp, rsp_max, &got, PROBE_TIMEOUT_MS);
    if (r != 0) {
        printf("  read failed: %s\n", libusb_error_name(r));
        return r;
    }
    if (quiet) printf("  reply: %d bytes (contents not shown)\n", got);
    else dump_hex("  reply", rsp, got);

    if (egis_reply_has_prefix(rsp, got)) {
        printf("  framing: SIGE prefix ok, check bytes %s, length byte=0x%02x\n",
               egis_reply_check_ok(rsp, got) ? "ok" : "do NOT add up",
               rsp[EGIS_HDR_LEN]);
        if (got >= EGIS_HDR_LEN + 3)
            printf("  trailer: 0x%02x%02x\n", rsp[got - 2], rsp[got - 1]);
    } else {
        printf("  framing: reply does NOT start with the SIGE prefix\n");
    }
    return got;
}


static void print_strings(libusb_device_handle *h, const struct libusb_device_descriptor *d) {
    unsigned char s[128];
    if (d->iManufacturer && libusb_get_string_descriptor_ascii(h, d->iManufacturer, s, sizeof(s)) > 0)
        printf("manufacturer string: %s\n", (char *)s);
    if (d->iProduct && libusb_get_string_descriptor_ascii(h, d->iProduct, s, sizeof(s)) > 0)
        printf("product string: %s\n", (char *)s);
    /* The serial number string is left out on purpose. */
}

int egis_probe_run(int debug) {
    g_dbg = debug;
    printf("EGIS probe (1c7a:05a1), read-only, nothing is written to the sensor's storage\n");

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
        if (d.idVendor == EGIS_VID && d.idProduct == EGIS_PID_05A1) { found = list[i]; break; }
    }
    if (!found) {
        printf("sensor 1c7a:05a1 not found on the USB bus.\n"
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

    uint8_t ep_out = PROBE_EP_OUT_DEFAULT, ep_in = PROBE_EP_IN_DEFAULT, ep_int = PROBE_EP_INT_DEFAULT;
    print_config(found, &ep_out, &ep_in, &ep_int);

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

    /* Same IN-direction reads the Windows driver sends before its first bulk
     * command on 0586/0587. All of them may fail on 05a1. */
    probe_ctrl_in(h, LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE,
                  32, 0x0000, 4, 16, "vendor request 32, index 4, 16 bytes");
    probe_ctrl_in(h, LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE,
                  32, 0x0000, 4, 40, "vendor request 32, index 4, 40 bytes");
    probe_ctrl_in(h, LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_STANDARD | LIBUSB_RECIPIENT_DEVICE,
                  LIBUSB_REQUEST_GET_STATUS, 0x0000, 0, 2, "GET_STATUS");
    probe_ctrl_in(h, LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE,
                  82, 0x0000, 0, 8, "vendor request 82, 8 bytes");

    int rc = 1;
    int answered = 0;
    for (int i = 0; i < PROBE_CMD_COUNT; i++) {
        int got = probe_exchange(h, ep_out, ep_in, PROBE_CMDS[i].payload, PROBE_CMDS[i].len,
                                 rsp, PROBE_IN_BUF_SIZE, PROBE_CMDS[i].name, PROBE_CMDS[i].quiet);
        if (got >= 0 && egis_reply_has_prefix(rsp, got)) {
            answered++;
            rc = 0;
            /* The print list: only count the 32 byte blocks (header 14 bytes,
             * 2 byte trailer), never show them. Count only when the status
             * word is 0x9000, otherwise show the status word instead. */
            if (i == PROBE_CMD_COUNT - 1) {
                unsigned status = (got >= 2) ? (unsigned)((rsp[got - 2] << 8) | rsp[got - 1]) : 0;
                if (got < 16)
                    printf("  enrolled print count: reply too short (%d bytes), see the length above\n", got);
                else if (status != 0x9000)
                    printf("  enrolled print count: not counted, status word is 0x%04x instead of 0x9000 (%d bytes)\n", status, got);
                else if ((got - 16) % 32 == 0)
                    printf("  enrolled prints on the sensor: %d\n", (got - 16) / 32);
                else
                    printf("  enrolled print count: reply layout differs from the PoCs (%d bytes), see the length above\n", got);
            }
        }
        /* A timeout or a bad reply on one query does not stop the others, but
         * if nothing answered after the first two there is no point going on. */
        if (i == 1 && answered == 0) {
            printf("\nno answer to the first two queries, stopping here.\n");
            break;
        }
    }

    printf("\nprobe finished, result: %s (%d of %d queries answered in the EGIS framing)\n",
           rc == 0 ? "sensor answered" : "sensor did NOT answer as expected", answered, PROBE_CMD_COUNT);
    if (rc != 0)
        printf("This does not mean the sensor is unsupported. It may need a different start-up step first\n"
               "(for example SDCP on newer firmware). Please post this whole output in the GitHub issue.\n");
    free(rsp);
    libusb_release_interface(h, 0);
    libusb_close(h);
    libusb_exit(ctx);
    return rc;
}

#else /* EGIS_PROBE_SELFTEST */

/* Framing self-test, no USB needed:
 *   cc -DEGIS_PROBE_SELFTEST -I. -I<libusb include dir> egis/egis_probe.c -o egis_selftest */
int main(void) {
    int bad = 0;

    /* Every built packet must sum to a multiple of 0xFFFF, for any payload. */
    for (int c = 0; c < PROBE_CMD_COUNT; c++) {
        unsigned char pkt[64];
        int n = egis_build_packet(pkt, PROBE_CMDS[c].payload, PROBE_CMDS[c].len);
        int ok = n == EGIS_HDR_LEN + PROBE_CMDS[c].len &&
                 memcmp(pkt, EGIS_WRITE_PREFIX, EGIS_PREFIX_LEN) == 0 &&
                 (egis_word_sum(pkt, n) % 0xFFFFu) == 0;
        printf("%-60s %s\n", PROBE_CMDS[c].name, ok ? "ok" : "FAIL");
        if (!ok) bad++;
    }
    /* Odd-length payload (last byte padded as the high byte of a word). */
    {
        unsigned char pkt[64];
        static const unsigned char odd[] = { 0x04, 0x50, 0x1a, 0x00, 0x00 };
        int n = egis_build_packet(pkt, odd, sizeof(odd));
        int ok = (egis_word_sum(pkt, n) % 0xFFFFu) == 0;
        printf("%-60s %s\n", "odd length payload", ok ? "ok" : "FAIL");
        if (!ok) bad++;
    }
    /* A reply seen in the PoC traces: SIGE 00 00 00 01 d5 6d 00 00 00 02 90 00 */
    {
        static const unsigned char rep[] = { 'S', 'I', 'G', 'E', 0, 0, 0, 1, 0xd5, 0x6d, 0, 0, 0, 2, 0x90, 0x00 };
        int ok = egis_reply_has_prefix(rep, sizeof(rep)) && egis_reply_check_ok(rep, sizeof(rep));
        printf("%-60s %s\n", "known reply prefix and check bytes", ok ? "ok" : "FAIL");
        if (!ok) bad++;
    }
    printf(bad ? "\n%d check(s) FAILED\n" : "\nall checks passed\n", bad);
    return bad ? 1 : 0;
}

#endif
