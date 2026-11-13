/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define _POSIX_C_SOURCE 200809L
#include "elan_capture.h"
#include <libusb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
static volatile sig_atomic_t stopped;
static void stop(int sig) { (void)sig; stopped = 1; }
static void usage(const char *name) {
    printf("Usage: %s [--probe | --frames N] [--output frame.pgm]\n"
           "Default: 120 frames (~10 seconds), statistics only.\n"
           "--probe queries dimensions without capturing images.\n"
           "--output saves the raw frame with the largest max-min spread as a private 16-bit PGM.\n"
           "No enrollment, matching, password access, or installation.\n", name);
}
static int write_all(int fd, const unsigned char *data, size_t len) {
    while (len) {
        ssize_t n = write(fd, data, len);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        data += n; len -= (size_t)n;
    }
    return 0;
}
static int save_pgm(const char *path, const uint16_t *frame) {
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) return -1;
    const char header[] = "P5\n150 52\n65535\n";
    unsigned char bytes[ELAN_PIXELS * 2];
    for (size_t i = 0; i < ELAN_PIXELS; i++) {
        bytes[2*i] = (unsigned char)(frame[i] >> 8);
        bytes[2*i+1] = (unsigned char)frame[i];
    }
    int rc = write_all(fd, (const unsigned char *)header, sizeof(header)-1);
    if (!rc) rc = write_all(fd, bytes, sizeof(bytes));
    if (close(fd) < 0) rc = -1;
    if (rc) unlink(path); /* Only the new file created by this invocation. */
    return rc;
}
int main(int argc, char **argv) {
    unsigned frames = 120;
    int probe = 0, frames_set = 0;
    const char *output = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--help")) { usage(argv[0]); return 0; }
        if (!strcmp(argv[i], "--probe")) { probe = 1; continue; }
        if (!strcmp(argv[i], "--frames") && i+1 < argc) {
            const char *value = argv[++i]; char *end = NULL; errno = 0;
            unsigned long n = strtoul(value, &end, 10);
            if (errno || !value[0] || *end || n < 1 || n > 10000) {
                fprintf(stderr, "--frames must be between 1 and 10000\n"); return 2;
            }
            frames = (unsigned)n; frames_set = 1; continue;
        }
        if (!strcmp(argv[i], "--output") && i+1 < argc) { output = argv[++i]; continue; }
        usage(argv[0]); return 2;
    }
    if (probe && (output || frames_set)) { usage(argv[0]); return 2; }
    signal(SIGINT, stop); signal(SIGTERM, stop);
    elan_capture_device *d = NULL;
    int rc = elan_capture_open(&d);
    if (rc) {
        fprintf(stderr, "ELAN open: %s\n", libusb_error_name(rc));
        fprintf(stderr, "Stop other fingerprint readers. This test does not detach kernel drivers.\n");
        return 1;
    }
    printf("ELAN 04f3:0c6e, %d x %d; capture-test-only\n", ELAN_WIDTH, ELAN_HEIGHT);
    if (probe) { elan_capture_close(d); return 0; }
    puts("Keep the pad clear initially, then touch and release it. Do not press the power button.");
    if (output) puts("The frame with the largest max-min spread will be saved. It may contain biometric data; do not attach it to a public issue.");
    uint16_t frame[ELAN_PIXELS] = {0};
    uint16_t best_frame[ELAN_PIXELS] = {0};
    unsigned completed = 0;
    unsigned best_frame_number = 0;
    unsigned best_spread = 0;
    struct timespec pause = {0, 60000000};
    while (completed < frames && !stopped) {
        rc = elan_capture_frame(d, frame, ELAN_PIXELS, 2000);
        if (rc) { fprintf(stderr, "Frame: %s\n", libusb_error_name(rc)); break; }
        uint64_t total = 0; uint16_t min = UINT16_MAX, max = 0;
        for (size_t j = 0; j < ELAN_PIXELS; j++) {
            total += frame[j]; if (frame[j] < min) min = frame[j]; if (frame[j] > max) max = frame[j];
        }
        completed++;
        unsigned spread = (unsigned)max - (unsigned)min;
        if (output && (best_frame_number == 0 || spread > best_spread)) {
            memcpy(best_frame, frame, sizeof(best_frame));
            best_frame_number = completed;
            best_spread = spread;
        }
        if (completed == 1 || completed % 10 == 0 || completed == frames) {
            printf("frame=%u mean=%.1f min=%u max=%u\n", completed,
                   (double)total / ELAN_PIXELS, (unsigned)min, (unsigned)max);
            fflush(stdout);
        }
        nanosleep(&pause, NULL);
    }
    elan_capture_close(d);
    if (!rc && !stopped && completed == frames && output) {
        if (save_pgm(output, best_frame) != 0) {
            perror("Could not create output (existing files are not overwritten)");
            rc = -1;
        } else {
            printf("Saved frame=%u spread=%u.\n", best_frame_number, best_spread);
        }
    }
    printf("Captured %u/%u frames%s.\n", completed, frames, stopped ? " (cancelled)" : "");
    memset(frame, 0, sizeof(frame));
    memset(best_frame, 0, sizeof(best_frame));
    return stopped ? 130 : (rc ? 1 : 0);
}
