/* SPDX-License-Identifier: LGPL-2.1-or-later
 * ELAN press sensor protocol adapted from filip-rs/libfprint elanpress,
 * Copyright (C) 2026 Filip Spanne, and casimir-engineering/elan-fp-fix.
 * See README.md for source revisions and protocol constraints.
 */
#include "elan_capture.h"
#include <libusb.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
struct elan_capture_device {
    libusb_context *ctx;
    libusb_device_handle *handle;
    int claimed;
    unsigned char raw[ELAN_PIXELS * 2];
};
static double now_ms(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t) != 0) return -1;
    return t.tv_sec * 1000.0 + t.tv_nsec / 1000000.0;
}
static int transfer(elan_capture_device *d, unsigned char ep,
                    unsigned char *data, int length, double deadline) {
    double now = now_ms();
    if (now < 0) return LIBUSB_ERROR_OTHER;
    double remain = deadline - now;
    if (remain <= 0) return LIBUSB_ERROR_TIMEOUT;
    unsigned timeout = remain > 5000 ? 5000 : (unsigned)remain;
    if (!timeout) timeout = 1; /* libusb's zero timeout would wait forever. */
    int actual = 0;
    int rc = libusb_bulk_transfer(d->handle, ep, data, length, &actual, timeout);
    if (rc != 0) return rc;
    return actual == length ? 0 : LIBUSB_ERROR_IO;
}
static int command(elan_capture_device *d, unsigned char a, unsigned char b,
                   double deadline) {
    unsigned char cmd[2] = {a, b};
    return transfer(d, 0x01, cmd, sizeof(cmd), deadline);
}
int elan_capture_open(elan_capture_device **out) {
    if (!out) return LIBUSB_ERROR_INVALID_PARAM;
    *out = NULL;
    elan_capture_device *d = calloc(1, sizeof(*d));
    if (!d) return LIBUSB_ERROR_NO_MEM;
    int rc = libusb_init(&d->ctx);
    if (rc != 0) goto fail;
    libusb_device **list = NULL;
    ssize_t count = libusb_get_device_list(d->ctx, &list);
    if (count < 0) { rc = (int)count; goto fail; }
    rc = LIBUSB_ERROR_NO_DEVICE;
    for (ssize_t i = 0; i < count; i++) {
        struct libusb_device_descriptor desc;
        if (libusb_get_device_descriptor(list[i], &desc) == 0 &&
            desc.idVendor == ELAN_VID && desc.idProduct == ELAN_PID) {
            rc = libusb_open(list[i], &d->handle);
            break;
        }
    }
    libusb_free_device_list(list, 1);
    if (rc != 0) goto fail;
    rc = libusb_claim_interface(d->handle, 0);
    if (rc != 0) goto fail;
    d->claimed = 1;
    double start = now_ms();
    if (start < 0) { rc = LIBUSB_ERROR_OTHER; goto fail; }
    double deadline = start + 5000;
    unsigned char dim[4];
    rc = command(d, 0x00, 0x0c, deadline);
    if (!rc) rc = transfer(d, 0x83, dim, sizeof(dim), deadline);
    if (rc) goto fail;
    unsigned w = dim[2], h = dim[0];
    if ((w & 1) && (h & 1)) { w++; h++; }
    if (w != ELAN_WIDTH || h != ELAN_HEIGHT) {
        rc = LIBUSB_ERROR_NOT_SUPPORTED;
        goto fail;
    }
    rc = command(d, 0x00, 0x0b, deadline);
    if (rc) goto fail;
    *out = d;
    return 0;
fail:
    elan_capture_close(d);
    return rc;
}
int elan_capture_frame(elan_capture_device *d, uint16_t *out,
                       size_t pixels, unsigned timeout_ms) {
    if (!d || !out || pixels != ELAN_PIXELS || !timeout_ms)
        return LIBUSB_ERROR_INVALID_PARAM;
    double start = now_ms();
    if (start < 0) return LIBUSB_ERROR_OTHER;
    double deadline = start + timeout_ms;
    unsigned char status;
    /* Status read arms a fresh exposure; avoid the legacy LED/pre_scan
     * commands that can freeze imaging on other 0c6e firmware. */
    int rc = command(d, 0x40, 0x13, deadline);
    if (!rc) rc = transfer(d, 0x83, &status, 1, deadline);
    if (!rc) rc = command(d, 0x00, 0x09, deadline);
    if (!rc) rc = transfer(d, 0x82, d->raw, sizeof(d->raw), deadline);
    if (rc) return rc;
    for (size_t y = 0; y < ELAN_HEIGHT; y++)
        for (size_t x = 0; x < ELAN_WIDTH; x++) {
            size_t j = 2 * (y + x * ELAN_HEIGHT);
            out[y * ELAN_WIDTH + x] =
                (uint16_t)d->raw[j] | (uint16_t)((uint16_t)d->raw[j + 1] << 8);
        }
    return 0;
}
void elan_capture_close(elan_capture_device *d) {
    if (!d) return;
    if (d->handle && d->claimed) {
        unsigned char stop[2] = {0, 0x0b};
        int actual = 0;
        /* Best effort, including disconnect/error paths. */
        (void)libusb_bulk_transfer(d->handle, 0x01, stop, 2, &actual, 250);
        libusb_release_interface(d->handle, 0);
    }
    if (d->handle) libusb_close(d->handle);
    if (d->ctx) libusb_exit(d->ctx);
    memset(d, 0, sizeof(*d));
    free(d);
}
