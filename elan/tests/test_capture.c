/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "elan_capture.h"
#include <libusb.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#ifdef ELAN_FAKE_USB_ONLY
#include <stdlib.h>
#endif
struct libusb_context { int unused; };
struct libusb_device { int unused; };
struct libusb_device_handle { int unused; };
static libusb_context ctx;
static libusb_device dev;
static libusb_device_handle handle;
static libusb_device *devices[] = {&dev, NULL};
static int phase, writes, releases, closes, exits, short_at, fail_at;
static int wrong_dimensions, zero_based, absent, denied, busy;
#ifndef ELAN_FAKE_USB_ONLY
static void reset(void) {
    phase = writes = releases = closes = exits = short_at = fail_at = 0;
    wrong_dimensions = zero_based = absent = denied = busy = 0;
}
#endif
int libusb_init(libusb_context **out) { *out = &ctx; return 0; }
void libusb_exit(libusb_context *c) { assert(c == &ctx); exits++; }
ssize_t libusb_get_device_list(libusb_context *c, libusb_device ***out) {
    assert(c == &ctx); *out = devices; return absent ? 0 : 1;
}
void libusb_free_device_list(libusb_device **list, int unref) { assert(list == devices && unref == 1); }
int libusb_get_device_descriptor(libusb_device *d, struct libusb_device_descriptor *out) {
    assert(d == &dev); out->idVendor = ELAN_VID; out->idProduct = ELAN_PID; return 0;
}
int libusb_open(libusb_device *d, libusb_device_handle **out) {
    assert(d == &dev); if (denied) return LIBUSB_ERROR_ACCESS; *out = &handle; return 0;
}
void libusb_close(libusb_device_handle *h) { assert(h == &handle); closes++; }
int libusb_claim_interface(libusb_device_handle *h, int i) {
    assert(h == &handle && i == 0); return busy ? LIBUSB_ERROR_BUSY : 0;
}
int libusb_release_interface(libusb_device_handle *h, int i) {
    assert(h == &handle && i == 0); releases++; return 0;
}
int libusb_bulk_transfer(libusb_device_handle *h, unsigned char ep,
                        unsigned char *b, int n, int *actual, unsigned timeout) {
    assert(h == &handle && timeout > 0 && timeout <= 5000);
    writes++;
    if (writes == fail_at) return LIBUSB_ERROR_NO_DEVICE;
    if (writes == short_at) { *actual = n-1; return 0; }
    *actual = n;
    if (ep == 1 && n == 2 && b[0] == 0 && b[1] == 11) return 0;
    switch (phase++) {
    case 0: assert(ep == 1 && n == 2 && b[0] == 0 && b[1] == 12); break;
    case 1:
        assert(ep == 0x83 && n == 4);
        b[0] = wrong_dimensions ? 40 : (zero_based ? 51 : 52);
        b[1] = b[3] = 0; b[2] = zero_based ? 149 : 150;
        break;
    case 2: assert(ep == 1 && n == 2 && b[0] == 0x40 && b[1] == 0x13); break;
    case 3: assert(ep == 0x83 && n == 1); b[0] = 0x55; break;
    case 4: assert(ep == 1 && n == 2 && b[0] == 0 && b[1] == 9); break;
    case 5:
        assert(ep == 0x82 && n == ELAN_PIXELS * 2);
        for (int i = 0; i < ELAN_PIXELS; i++) { b[2*i] = i & 255; b[2*i+1] = i >> 8; }
#ifdef ELAN_FAKE_USB_ONLY
        static unsigned frame_number;
        frame_number++;
        const char *pattern = getenv("ELAN_TEST_FRAME_PATTERN");
        if (pattern && strcmp(pattern, "spread") == 0) {
            for (int i = 0; i < ELAN_PIXELS; i++) {
                unsigned sample;
                switch (frame_number) {
                case 1: sample = 60000; break; /* High mean, zero spread. */
                case 2: sample = i == 0 ? 50000 : 1000; break;
                case 3: sample = i == 0 ? 30000 : 1000; break;
                case 4: sample = 100; break; /* Empty-looking final frame. */
                default: sample = i == 0 ? 50010 : 1010; break; /* Equal best spread. */
                }
                b[2*i] = (unsigned char)sample;
                b[2*i+1] = (unsigned char)(sample >> 8);
            }
        }
#endif
        phase = 2; break;
    default: assert(!"Unexpected USB sequence");
    }
    return 0;
}
const char *libusb_error_name(int e) { (void)e; return "mock error"; }
#ifndef ELAN_FAKE_USB_ONLY
static void expect_closed(int claimed) {
    assert(releases == claimed && closes == 1 && exits == 1);
}
int main(void) {
    elan_capture_device *d = NULL;
    uint16_t frame[ELAN_PIXELS];
    reset(); assert(elan_capture_open(&d) == 0 && d);
    assert(elan_capture_frame(d, frame, ELAN_PIXELS, 1000) == 0);
    for (int y = 0; y < ELAN_HEIGHT; y++)
        for (int x = 0; x < ELAN_WIDTH; x++) assert(frame[y * ELAN_WIDTH + x] == x * ELAN_HEIGHT + y);
    assert(elan_capture_frame(d, frame, ELAN_PIXELS, 1000) == 0);
    elan_capture_close(d); expect_closed(1);
    reset(); zero_based = 1; assert(elan_capture_open(&d) == 0); elan_capture_close(d); expect_closed(1);
    reset(); wrong_dimensions = 1; assert(elan_capture_open(&d) == LIBUSB_ERROR_NOT_SUPPORTED && !d); expect_closed(1);
    reset(); absent = 1; assert(elan_capture_open(&d) == LIBUSB_ERROR_NO_DEVICE && !d); assert(exits == 1 && closes == 0);
    reset(); denied = 1; assert(elan_capture_open(&d) == LIBUSB_ERROR_ACCESS && !d); assert(exits == 1 && closes == 0);
    reset(); busy = 1; assert(elan_capture_open(&d) == LIBUSB_ERROR_BUSY && !d); expect_closed(0);
    for (int i = 1; i <= 7; i++) {
        reset(); short_at = i; int rc = elan_capture_open(&d);
        memset(frame, 0x5a, sizeof(frame));
        if (!rc) { rc = elan_capture_frame(d, frame, ELAN_PIXELS, 1000); elan_capture_close(d); }
        assert(rc == LIBUSB_ERROR_IO);
        for (int j = 0; j < ELAN_PIXELS; j++) assert(frame[j] == 0x5a5a);
        expect_closed(1);
    }
    for (int i = 1; i <= 7; i++) {
        reset(); fail_at = i; int rc = elan_capture_open(&d);
        if (!rc) { rc = elan_capture_frame(d, frame, ELAN_PIXELS, 1000); elan_capture_close(d); }
        assert(rc == LIBUSB_ERROR_NO_DEVICE); expect_closed(1);
    }
    reset(); assert(elan_capture_open(&d) == 0);
    assert(elan_capture_frame(d, frame, ELAN_PIXELS-1, 1000) == LIBUSB_ERROR_INVALID_PARAM);
    assert(elan_capture_frame(d, frame, ELAN_PIXELS, 0) == LIBUSB_ERROR_INVALID_PARAM);
    elan_capture_close(d); expect_closed(1);
    assert(elan_capture_open(NULL) == LIBUSB_ERROR_INVALID_PARAM);
    elan_capture_close(NULL);
    puts("PASS: protocol ordering, endian/rotation, dimensions, permissions, busy, short transfers, disconnects and cleanup");
    return 0;
}

#endif
