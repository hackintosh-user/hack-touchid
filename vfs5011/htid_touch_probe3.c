/*
 * htid_touch_probe3.c -- can the finger LANDING on the VFS5011 be matched like a very short swipe?
 *
 * Experiment for "touch to unlock" (active-development only). The sensor reads one thin row at a
 * time, so a finger resting still cannot be matched (probe 2). But placing a finger is not perfectly
 * still: the landing moves it a little across the sensor, and the daemon's own pipeline can turn
 * that movement into a tiny image. This probe runs the daemon's real capture-row selection, line
 * assembly, minutiae extraction and matcher on touch-down captures, with no swipe, and reports
 * how often the right finger passes and the wrong finger does not.
 *
 * Each trial arms the sensor fresh (like the daemon does for every capture), waits for a touch,
 * records for --touch-ms after the first line, then closes the sensor. Nothing is written to disk
 * except the report, a CSV and one picture per trial in the output folder. It does not touch the
 * real enrolled templates.
 *
 * Build (from the repo root; the same files the matcher tests use plus libusb):
 *   clang vfs5011/htid_touch_probe3.c shared/sen/hack-touchid-matcher.c \
 *       NBIS/mindtct/ALL.c NBIS/bozorth3/ALL.c -o htid_touch_probe3 \
 *       -Ivfs5011 -Ishared/sen -INBIS/include -I/usr/local/include/libusb-1.0 \
 *       -L/usr/local/lib -lusb-1.0 -lm -Wno-implicit-function-declaration
 *   (ALL.c stands for every .c file in that folder: the same list as scripts/build_daemon.sh)
 *
 * Usage (stop the daemon first so the sensor is free):
 *   sudo ./htid_touch_probe3 [--enroll N] [--verify N] [--touch-ms MS] [--out DIR]
 *   --enroll N     touches of finger A used as the enrollment (default 4)
 *   --verify N     touches of finger A and of finger B to test, alternating (default 6 each)
 *   --touch-ms MS  how long to record after the first line (default 1200)
 *   --synthetic    self-test of the code path with made-up data, no sensor needed
 *
 * Press the finger down the way you would to unlock, do not swipe, and keep it on until it says LIFT.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <libusb.h>
#include <math.h>
#include "hack-touchid-matcher.h"

#include "vfs5011_proto.h"

#define VID 0x138a
#define PID 0x0018
#define CAPTURE_LINES 256
#define DEVIATION_THRESHOLD (15*15)   /* same "finger present" test as the daemon */
#define DIFFERENCE_THRESHOLD 600      /* same "new row" test as the daemon */
#define MAX_PHASES 6

enum action_type { ACTION_SEND, ACTION_RECEIVE };
struct usb_action {
    enum action_type type;
    const char *name;
    int endpoint;
    int size;
    unsigned char *data;
    int correct_reply_size;
};

#define SEND(ENDPOINT, COMMAND) \
    { ACTION_SEND, #COMMAND, ENDPOINT, sizeof(COMMAND), COMMAND, 0 },
#define RECV(ENDPOINT, SIZE) \
    { ACTION_RECEIVE, "recv", ENDPOINT, SIZE, NULL, 0 },
#define RECV_CHECK(ENDPOINT, SIZE, EXPECTED) \
    { ACTION_RECEIVE, "recv_check", ENDPOINT, SIZE, EXPECTED, sizeof(EXPECTED) },

static struct usb_action vfs5011_initialization[] = {
    SEND(VFS5011_OUT_ENDPOINT, vfs5011_cmd_01)
    RECV(VFS5011_IN_ENDPOINT_CTRL, 64)
    SEND(VFS5011_OUT_ENDPOINT, vfs5011_cmd_19)
    RECV(VFS5011_IN_ENDPOINT_CTRL, 64)
    RECV(VFS5011_IN_ENDPOINT_CTRL, 64)
    SEND(VFS5011_OUT_ENDPOINT, vfs5011_init_00)
    RECV(VFS5011_IN_ENDPOINT_CTRL, 64)
    SEND(VFS5011_OUT_ENDPOINT, vfs5011_init_01)
    RECV(VFS5011_IN_ENDPOINT_CTRL, 64)
    SEND(VFS5011_OUT_ENDPOINT, vfs5011_init_02)
    RECV_CHECK(VFS5011_IN_ENDPOINT_CTRL, 64, VFS5011_NORMAL_CONTROL_REPLY)
    SEND(VFS5011_OUT_ENDPOINT, vfs5011_cmd_01)
    RECV(VFS5011_IN_ENDPOINT_CTRL, 64)
    SEND(VFS5011_OUT_ENDPOINT, vfs5011_cmd_1A)
    RECV_CHECK(VFS5011_IN_ENDPOINT_CTRL, 64, VFS5011_NORMAL_CONTROL_REPLY)
    SEND(VFS5011_OUT_ENDPOINT, vfs5011_init_03)
    RECV_CHECK(VFS5011_IN_ENDPOINT_CTRL, 64, VFS5011_NORMAL_CONTROL_REPLY)
    SEND(VFS5011_OUT_ENDPOINT, vfs5011_init_04)
    RECV_CHECK(VFS5011_IN_ENDPOINT_CTRL, 64, VFS5011_NORMAL_CONTROL_REPLY)
    RECV(VFS5011_IN_ENDPOINT_DATA, 256)
    RECV(VFS5011_IN_ENDPOINT_DATA, 64)
    SEND(VFS5011_OUT_ENDPOINT, vfs5011_cmd_1A)
    RECV_CHECK(VFS5011_IN_ENDPOINT_CTRL, 64, VFS5011_NORMAL_CONTROL_REPLY)
    SEND(VFS5011_OUT_ENDPOINT, vfs5011_init_05)
    RECV_CHECK(VFS5011_IN_ENDPOINT_CTRL, 64, VFS5011_NORMAL_CONTROL_REPLY)
    SEND(VFS5011_OUT_ENDPOINT, vfs5011_cmd_01)
    RECV(VFS5011_IN_ENDPOINT_CTRL, 64)
    SEND(VFS5011_OUT_ENDPOINT, vfs5011_init_06)
    RECV_CHECK(VFS5011_IN_ENDPOINT_CTRL, 64, VFS5011_NORMAL_CONTROL_REPLY)
    RECV(VFS5011_IN_ENDPOINT_DATA, 17216)
    RECV(VFS5011_IN_ENDPOINT_DATA, 32)
    SEND(VFS5011_OUT_ENDPOINT, vfs5011_init_07)
    RECV_CHECK(VFS5011_IN_ENDPOINT_CTRL, 64, VFS5011_NORMAL_CONTROL_REPLY)
    RECV(VFS5011_IN_ENDPOINT_DATA, 45056)
    SEND(VFS5011_OUT_ENDPOINT, vfs5011_init_08)
    RECV_CHECK(VFS5011_IN_ENDPOINT_CTRL, 64, VFS5011_NORMAL_CONTROL_REPLY)
    RECV(VFS5011_IN_ENDPOINT_DATA, 16896)
    SEND(VFS5011_OUT_ENDPOINT, vfs5011_init_09)
    RECV_CHECK(VFS5011_IN_ENDPOINT_CTRL, 64, VFS5011_NORMAL_CONTROL_REPLY)
    RECV(VFS5011_IN_ENDPOINT_DATA, 4928)
    SEND(VFS5011_OUT_ENDPOINT, vfs5011_init_10)
    RECV_CHECK(VFS5011_IN_ENDPOINT_CTRL, 64, VFS5011_NORMAL_CONTROL_REPLY)
    RECV(VFS5011_IN_ENDPOINT_DATA, 5632)
    SEND(VFS5011_OUT_ENDPOINT, vfs5011_init_11)
    RECV_CHECK(VFS5011_IN_ENDPOINT_CTRL, 64, VFS5011_NORMAL_CONTROL_REPLY)
    RECV(VFS5011_IN_ENDPOINT_DATA, 5632)
    SEND(VFS5011_OUT_ENDPOINT, vfs5011_init_12)
    RECV_CHECK(VFS5011_IN_ENDPOINT_CTRL, 64, VFS5011_NORMAL_CONTROL_REPLY)
    RECV(VFS5011_IN_ENDPOINT_DATA, 3328)
    RECV(VFS5011_IN_ENDPOINT_DATA, 64)
    SEND(VFS5011_OUT_ENDPOINT, vfs5011_init_13)
    RECV_CHECK(VFS5011_IN_ENDPOINT_CTRL, 64, VFS5011_NORMAL_CONTROL_REPLY)
    SEND(VFS5011_OUT_ENDPOINT, vfs5011_cmd_1A)
    RECV_CHECK(VFS5011_IN_ENDPOINT_CTRL, 64, VFS5011_NORMAL_CONTROL_REPLY)
    SEND(VFS5011_OUT_ENDPOINT, vfs5011_init_03)
    RECV_CHECK(VFS5011_IN_ENDPOINT_CTRL, 64, VFS5011_NORMAL_CONTROL_REPLY)
    SEND(VFS5011_OUT_ENDPOINT, vfs5011_init_14)
    RECV_CHECK(VFS5011_IN_ENDPOINT_CTRL, 64, VFS5011_NORMAL_CONTROL_REPLY)
    RECV(VFS5011_IN_ENDPOINT_DATA, 4800)
    SEND(VFS5011_OUT_ENDPOINT, vfs5011_cmd_1A)
    RECV_CHECK(VFS5011_IN_ENDPOINT_CTRL, 64, VFS5011_NORMAL_CONTROL_REPLY)
    SEND(VFS5011_OUT_ENDPOINT, vfs5011_init_02)
    RECV_CHECK(VFS5011_IN_ENDPOINT_CTRL, 64, VFS5011_NORMAL_CONTROL_REPLY)
    SEND(VFS5011_OUT_ENDPOINT, vfs5011_cmd_27)
    RECV(VFS5011_IN_ENDPOINT_CTRL, 64)
    SEND(VFS5011_OUT_ENDPOINT, vfs5011_cmd_1A)
    RECV_CHECK(VFS5011_IN_ENDPOINT_CTRL, 64, VFS5011_NORMAL_CONTROL_REPLY)
    SEND(VFS5011_OUT_ENDPOINT, vfs5011_init_15)
    RECV_CHECK(VFS5011_IN_ENDPOINT_CTRL, 64, VFS5011_NORMAL_CONTROL_REPLY)
    SEND(VFS5011_OUT_ENDPOINT, vfs5011_init_16)
    RECV(VFS5011_IN_ENDPOINT_CTRL, 2368)
    RECV(VFS5011_IN_ENDPOINT_CTRL, 64)
    RECV(VFS5011_IN_ENDPOINT_DATA, 4800)
    SEND(VFS5011_OUT_ENDPOINT, vfs5011_init_17)
    RECV_CHECK(VFS5011_IN_ENDPOINT_CTRL, 64, VFS5011_NORMAL_CONTROL_REPLY)
    SEND(VFS5011_OUT_ENDPOINT, vfs5011_init_18)
    RECV_CHECK(VFS5011_IN_ENDPOINT_CTRL, 64, VFS5011_NORMAL_CONTROL_REPLY)
};

static struct usb_action vfs5011_initiate_capture[] = {
    SEND(VFS5011_OUT_ENDPOINT, vfs5011_cmd_04)
    RECV(VFS5011_IN_ENDPOINT_DATA, 64)
    RECV(VFS5011_IN_ENDPOINT_DATA, 84032)
    RECV_CHECK(VFS5011_IN_ENDPOINT_CTRL, 64, VFS5011_NORMAL_CONTROL_REPLY)
    SEND(VFS5011_OUT_ENDPOINT, vfs5011_cmd_1A)
    RECV_CHECK(VFS5011_IN_ENDPOINT_CTRL, 64, VFS5011_NORMAL_CONTROL_REPLY)
    SEND(VFS5011_OUT_ENDPOINT, vfs5011_prepare_00)
    RECV_CHECK(VFS5011_IN_ENDPOINT_CTRL, 64, VFS5011_NORMAL_CONTROL_REPLY)
    SEND(VFS5011_OUT_ENDPOINT, vfs5011_cmd_1A)
    RECV_CHECK(VFS5011_IN_ENDPOINT_CTRL, 64, VFS5011_NORMAL_CONTROL_REPLY)
    SEND(VFS5011_OUT_ENDPOINT, vfs5011_prepare_01)
    RECV_CHECK(VFS5011_IN_ENDPOINT_CTRL, 64, VFS5011_NORMAL_CONTROL_REPLY)
    SEND(VFS5011_OUT_ENDPOINT, vfs5011_prepare_02)
    RECV(VFS5011_IN_ENDPOINT_CTRL, 2368)
    RECV(VFS5011_IN_ENDPOINT_CTRL, 64)
    RECV(VFS5011_IN_ENDPOINT_DATA, 4800)
    SEND(VFS5011_OUT_ENDPOINT, vfs5011_prepare_03)
    RECV_CHECK(VFS5011_IN_ENDPOINT_CTRL, 64, VFS5011_NORMAL_CONTROL_REPLY)
    SEND(VFS5011_OUT_ENDPOINT, vfs5011_prepare_04)
    RECV_CHECK(VFS5011_IN_ENDPOINT_CTRL, 2368, VFS5011_NORMAL_CONTROL_REPLY)
};

/* ------------------------------------------------------------------ */

static long long now_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

static int xfer(libusb_device_handle *h, int ep, unsigned char *buf, int len, int *got, unsigned int to) {
    int r = libusb_bulk_transfer(h, ep, buf, len, got, to);
    if (r == LIBUSB_ERROR_PIPE) { libusb_clear_halt(h, ep); r = libusb_bulk_transfer(h, ep, buf, len, got, to); }
    return r;
}

static int run_sequence(libusb_device_handle *h, struct usb_action *seq, int count) {
    static unsigned char rb[VFS5011_RECEIVE_BUF_SIZE];
    for (int i = 0; i < count; i++) {
        struct usb_action *a = &seq[i];
        int n = 0, r;
        if (a->type == ACTION_SEND) {
            r = xfer(h, a->endpoint, (unsigned char *)a->data, a->size, &n, VFS5011_DEFAULT_WAIT_TIMEOUT);
            if (r != 0 || n != a->size) { fprintf(stderr, "SEND failed at step %d (%s): %s\n", i + 1, a->name, libusb_error_name(r)); return -1; }
        } else {
            r = xfer(h, a->endpoint, rb, a->size, &n, VFS5011_DEFAULT_WAIT_TIMEOUT);
            if (r != 0) { fprintf(stderr, "RECV failed at step %d: %s\n", i + 1, libusb_error_name(r)); return -1; }
            if (a->data && (n != a->correct_reply_size || memcmp(rb, a->data, (size_t)a->correct_reply_size) != 0)) {
                fprintf(stderr, "RECV_CHECK mismatch at step %d\n", i + 1); return -1;
            }
        }
    }
    return 0;
}

static int deviation(const unsigned char *buf, int size) {
    int mean = 0, res = 0;
    for (int i = 0; i < size; i++) mean += buf[i];
    mean /= size;
    for (int i = 0; i < size; i++) { int d = (int)buf[i] - mean; res += d * d; }
    return res / size;
}
static int diff_norm(const unsigned char *a, const unsigned char *b, int size) {
    int res = 0;
    for (int i = 0; i < size; i++) { int d = (int)a[i] - (int)b[i]; res += d * d; }
    return res / size;
}


#define MAX_LINES_TOTAL 2000
#define ASM_RESOLUTION 10
#define ASM_MEDIAN_FILTER_SIZE 25
#define ASM_MAX_SEARCH_OFFSET 30

static int get_deviation(unsigned char *buf, int size) {
    int mean = 0, res = 0, i;
    for (i = 0; i < size; i++) mean += buf[i];
    mean /= size;
    for (i = 0; i < size; i++) { int d = (int)buf[i] - mean; res += d * d; }
    return res / size;
}
static int get_diff_norm(unsigned char *a, unsigned char *b, int size) {
    int res = 0, i;
    for (i = 0; i < size; i++) { int d = (int)a[i] - (int)b[i]; res += d * d; }
    return res / size;
}
static unsigned char get_pixel(unsigned char *line, int x) { return line[8 + x]; }
static int get_deviation2(unsigned char *row1, unsigned char *row2) {
    unsigned char *buf1 = row1 + 56;
    unsigned char *buf2 = row2 + 168;
    const int size = 64;
    int mean = 0, res = 0, i;
    for (i = 0; i < size; i++) mean += (int)buf1[i] + (int)buf2[i];
    mean /= size;
    for (i = 0; i < size; i++) { int d = (int)buf1[i] + (int)buf2[i] - mean; res += d * d; }
    return res / size;
}
static int cmpint(const void *a, const void *b) { return (*(const int *)a) - (*(const int *)b); }
static void median_filter(int *data, int size, int filtersize) {
    int *result = calloc(size, sizeof(int));
    int *sortbuf = calloc(filtersize, sizeof(int));
    if (!result || !sortbuf) { free(result); free(sortbuf); return; }
    for (int i = 0; i < size; i++) {
        int i1 = i - (filtersize - 1) / 2, i2 = i + (filtersize - 1) / 2;
        if (i1 < 0) i1 = 0;
        if (i2 >= size) i2 = size - 1;
        memmove(sortbuf, data + i1, (size_t)(i2 - i1 + 1) * sizeof(int));
        qsort(sortbuf, i2 - i1 + 1, sizeof(int), cmpint);
        result[i] = sortbuf[(i2 - i1 + 1) / 2];
    }
    memmove(data, result, (size_t)size * sizeof(int));
    free(result); free(sortbuf);
}
static void interpolate_lines(unsigned char *line1, float y1, unsigned char *line2,
                               float y2, unsigned char *output, float yi, int size) {
    if (!line1 || !line2) return;
    for (int i = 0; i < size; i++) {
        unsigned char p1 = get_pixel(line1, i), p2 = get_pixel(line2, i);
        output[i] = (unsigned char)((float)p1 + (yi - y1) / (y2 - y1) * ((float)p2 - (float)p1));
    }
}
static unsigned char *assemble_lines(unsigned char *lines, int lines_len, int max_height, int *out_height) {
    int line_stride = VFS5011_LINE_SIZE, width = VFS5011_IMAGE_WIDTH;
    int *offsets = calloc((size_t)(lines_len / 2), sizeof(int));
    unsigned char *output = calloc((size_t)width * max_height, 1);
    if (!offsets || !output) { free(offsets); free(output); *out_height = 0; return NULL; }
    float y = 0.0f; int line_ind = 0;
    for (int i = 0; i < lines_len - 1; i += 2) {
        int bestmatch = i, bestdiff = 0;
        int firstrow = i + 1;
        int lastrow = (i + ASM_MAX_SEARCH_OFFSET < lines_len - 1) ? i + ASM_MAX_SEARCH_OFFSET : lines_len - 1;
        for (int j = firstrow; j <= lastrow; j++) {
            int diff = get_deviation2(lines + (size_t)i * line_stride, lines + (size_t)j * line_stride);
            if (j == firstrow || diff < bestdiff) { bestdiff = diff; bestmatch = j; }
        }
        offsets[i / 2] = bestmatch - i;
    }
    int off_count = (lines_len / 2) - 1;
    if (off_count > 0) median_filter(offsets, off_count, ASM_MEDIAN_FILTER_SIZE);
    for (int i = 0; i < lines_len - 1; i++) {
        int offset = offsets[i / 2];
        unsigned char *row1 = lines + (size_t)i * line_stride;
        unsigned char *row2 = lines + (size_t)(i + 1) * line_stride;
        if (offset > 0) {
            float ynext = y + (float)ASM_RESOLUTION / (float)offset;
            while ((float)line_ind < ynext) {
                if (line_ind > max_height - 1) goto out;
                interpolate_lines(row1, y, row2, ynext, output + (size_t)line_ind * width, (float)line_ind, width);
                line_ind++;
            }
            y = ynext;
        }
    }
out:
    free(offsets);
    *out_height = line_ind;
    return output;
}

/* ------------------------------------------------------------------ */

#define MAXT 40
static const int THRESH[4] = { 20, 25, 30, 40 };

static libusb_context *g_ctx = NULL;
static libusb_device_handle *g_h = NULL;

static int open_dev(void) {
    if (libusb_init(&g_ctx) < 0) return -1;
    libusb_set_option(g_ctx, LIBUSB_OPTION_LOG_LEVEL, LIBUSB_LOG_LEVEL_NONE);
    for (int i = 0; i < 5 && !g_h; i++) { g_h = libusb_open_device_with_vid_pid(g_ctx, VID, PID); if (!g_h) usleep(300000); }
    if (!g_h) return -1;
    libusb_set_auto_detach_kernel_driver(g_h, 1);
    if (libusb_claim_interface(g_h, 0) == 0) return 0;
    for (int i = 0; i < 3; i++) { usleep(150000); if (libusb_claim_interface(g_h, 0) == 0) return 0; }
    return -1;
}
static void close_dev(void) {
    if (g_h) {
        libusb_clear_halt(g_h, VFS5011_IN_ENDPOINT_CTRL);
        libusb_clear_halt(g_h, VFS5011_IN_ENDPOINT_DATA);
        libusb_clear_halt(g_h, VFS5011_OUT_ENDPOINT);
        libusb_release_interface(g_h, 0);
        libusb_close(g_h);
        g_h = NULL;
    }
    if (g_ctx) { libusb_exit(g_ctx); g_ctx = NULL; }
}

/* the daemon's row selection (capture_fingerprint_image), one line at a time */
typedef struct { unsigned char *recorded; int rec, counted; unsigned char *last; long long t_first; } sel_t;
static int feed_line(sel_t *s, unsigned char *line) {
    if (get_deviation(line + 8, VFS5011_IMAGE_WIDTH) < DEVIATION_THRESHOLD && s->counted == 0) return 0;
    s->counted++;
    if (s->counted == 1) s->t_first = now_ms();
    if (s->last == NULL || get_diff_norm(s->last + 8, line + 8, VFS5011_IMAGE_WIDTH) >= DIFFERENCE_THRESHOLD) {
        if (s->rec >= MAX_LINES_TOTAL) return 1;
        s->last = s->recorded + (size_t)s->rec * VFS5011_LINE_SIZE;
        memcpy(s->last, line, VFS5011_LINE_SIZE);
        s->rec++;
    }
    return 0;
}

typedef struct { unsigned char *img; int height, recorded, counted; double wait_ms; } cap_t;

/* returns 0 ok, -2 init failed, -3 arm failed, -4 no touch in time, -5 read error */
static int capture_touch(int touch_ms, int wait_ms, cap_t *o) {
    memset(o, 0, sizeof *o);
    if (run_sequence(g_h, vfs5011_initialization, sizeof(vfs5011_initialization) / sizeof(vfs5011_initialization[0])) != 0) return -2;
    if (run_sequence(g_h, vfs5011_initiate_capture, sizeof(vfs5011_initiate_capture) / sizeof(vfs5011_initiate_capture[0])) != 0) return -3;
    printf("      ARMED: put your finger down now (do not swipe)\a\n"); fflush(stdout);
    sel_t s; memset(&s, 0, sizeof s);
    s.recorded = malloc((size_t)MAX_LINES_TOTAL * VFS5011_LINE_SIZE);
    unsigned char *chunk = malloc((size_t)CAPTURE_LINES * VFS5011_LINE_SIZE);
    unsigned char *acc = malloc((size_t)2 * CAPTURE_LINES * VFS5011_LINE_SIZE);
    int acc_len = 0, rc = 0, full = 0;
    long long armed = now_ms();
    while (!full) {
        long long now = now_ms();
        if (s.counted == 0 && now - armed > wait_ms) { rc = -4; break; }
        if (s.counted > 0 && now - s.t_first >= touch_ms) break;
        int n = 0;
        int r = libusb_bulk_transfer(g_h, VFS5011_IN_ENDPOINT_DATA, chunk, CAPTURE_LINES * VFS5011_LINE_SIZE, &n, 100);
        if (r != 0 && r != LIBUSB_ERROR_TIMEOUT) { rc = -5; break; }
        if (n <= 0) continue;
        if (acc_len + n > 2 * CAPTURE_LINES * VFS5011_LINE_SIZE) n = 2 * CAPTURE_LINES * VFS5011_LINE_SIZE - acc_len;
        memcpy(acc + acc_len, chunk, (size_t)n); acc_len += n;
        int nl = acc_len / VFS5011_LINE_SIZE;
        for (int i = 0; i < nl && !full; i++) full = feed_line(&s, acc + (size_t)i * VFS5011_LINE_SIZE);
        int used = nl * VFS5011_LINE_SIZE;
        memmove(acc, acc + used, (size_t)(acc_len - used)); acc_len -= used;
    }
    o->wait_ms = (double)((s.counted ? s.t_first : now_ms()) - armed);
    o->recorded = s.rec; o->counted = s.counted;
    if (rc == 0 && s.rec >= 2) o->img = assemble_lines(s.recorded, s.rec, MAX_LINES_TOTAL, &o->height);
    free(s.recorded); free(chunk); free(acc);
    return rc;
}

/* ---- made-up touch-downs for --synthetic: exercises the code path only, says nothing about the sensor ---- */
static unsigned lcg = 777;
static double frand(void) { lcg = lcg * 1664525u + 1013904223u; return (lcg >> 8) / 16777216.0; }
static double ridge(double x, double y, int finger) {
    double th = 0.5 + 0.02 * x * (finger ? 1.0 : -1.0) + 0.03 * y + 0.7 * finger;
    double ph = (x * cos(th) + y * sin(th)) * 2 * M_PI / 9.0 + 3.0 * sin(0.05 * x + 0.04 * y + finger * 2.0);
    return 128 + 90 * sin(ph);
}
static int synth_capture(int finger, int touch_ms, cap_t *o) {
    (void)touch_ms;
    memset(o, 0, sizeof *o);
    sel_t s; memset(&s, 0, sizeof s);
    s.recorded = malloc((size_t)MAX_LINES_TOTAL * VFS5011_LINE_SIZE);
    double y0 = frand() * 20.0, x0 = frand() * 6.0 - 3.0;
    unsigned char line[VFS5011_LINE_SIZE];
    for (int k = 0; k < 3600; k++) {
        double y = y0 + 0.012 * k;           /* the finger creeps about 40 rows during the touch */
        memset(line, 0, sizeof line);
        line[0] = 1; line[1] = 0xfe;
        for (int x = 0; x < VFS5011_IMAGE_WIDTH; x++) line[8 + x] = (unsigned char)(ridge(x + x0, y, finger) + frand() * 6);
        for (int x = 0; x < 64; x++) line[168 + x] = (unsigned char)(ridge(56 + x + x0, y + 3.0, finger) + frand() * 6);
        if (feed_line(&s, line)) break;
    }
    o->recorded = s.rec; o->counted = s.counted;
    if (s.rec >= 2) o->img = assemble_lines(s.recorded, s.rec, MAX_LINES_TOTAL, &o->height);
    free(s.recorded);
    return 0;
}

typedef struct { int finger, is_enroll, height, recorded, minutiae, score, ok; } trial_t;

int main(int argc, char **argv) {
    int n_enroll = 4, n_verify = 6, touch_ms = 1200, synthetic = 0;
    const char *outdir = "/tmp/htid-touch-probe3";
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--enroll") && i + 1 < argc) n_enroll = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--verify") && i + 1 < argc) n_verify = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--touch-ms") && i + 1 < argc) touch_ms = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) outdir = argv[++i];
        else if (!strcmp(argv[i], "--synthetic")) synthetic = 1;
        else { fprintf(stderr, "usage: %s [--enroll N] [--verify N] [--touch-ms MS] [--out DIR] [--synthetic]\n", argv[0]); return 2; }
    }
    if (n_enroll < 2) n_enroll = 2;
    if (n_enroll > 10) n_enroll = 10;
    if (n_verify < 2) n_verify = 2;
    if (n_verify > 10) n_verify = 10;
    if (touch_ms < 400) touch_ms = 400;
    if (touch_ms > 5000) touch_ms = 5000;
    mkdir(outdir, 0755);
    char path[1024];
    snprintf(path, sizeof path, "%s/report.txt", outdir);
    FILE *rep = fopen(path, "w");
    snprintf(path, sizeof path, "%s/trials.csv", outdir);
    FILE *csv = fopen(path, "w");
    if (!rep || !csv) { fprintf(stderr, "Cannot write to %s\n", outdir); return 1; }
    fprintf(csv, "trial,role,finger,rows_recorded,image_height,minutiae,best_score\n");

#define OUT(...) do { printf(__VA_ARGS__); fprintf(rep, __VA_ARGS__); } while (0)
    OUT("VFS5011 touch probe 3%s: enroll %d x finger A, verify %d x A and %d x B, record %d ms after the first line\n",
        synthetic ? " (SYNTHETIC SELF-TEST)" : "", n_enroll, n_verify, n_verify, touch_ms);

    static trial_t T[MAXT]; static struct xyt_struct tmpl[MAXT];
    static int has_tmpl[MAXT];
    int nt = 0;
    int plan_finger[MAXT], plan_enroll[MAXT];
    for (int i = 0; i < n_enroll; i++) { plan_finger[nt] = 0; plan_enroll[nt++] = 1; }
    for (int i = 0; i < n_verify; i++) { plan_finger[nt] = 0; plan_enroll[nt++] = 0; plan_finger[nt] = 1; plan_enroll[nt++] = 0; }

    if (!synthetic) printf("\nStop the daemon first. Each trial arms the sensor, then waits for a touch.\n");
    for (int k = 0; k < nt; k++) {
        T[k].finger = plan_finger[k]; T[k].is_enroll = plan_enroll[k];
        printf("\n[%d/%d] %s: finger %c\n", k + 1, nt, T[k].is_enroll ? "ENROLL" : "VERIFY", 'A' + T[k].finger);
        cap_t cap; int rc = -2;
        if (synthetic) rc = synth_capture(T[k].finger, touch_ms, &cap);
        else {
            for (int attempt = 0; attempt < 3 && rc == -2; attempt++) {
                if (open_dev() != 0) { close_dev(); usleep(600000); continue; }
                rc = capture_touch(touch_ms, 20000, &cap);
                close_dev();
                if (rc == -2 || rc == -3) { usleep(800000); rc = -2; }
            }
        }
        T[k].recorded = cap.recorded; T[k].height = cap.height; T[k].minutiae = -1; T[k].score = -1;
        if (rc != 0 || !cap.img || cap.height < 16) {
            printf("      no usable image (rc=%d, rows recorded %d, image height %d)\n", rc, cap.recorded, cap.height);
        } else {
            memset(&tmpl[k], 0, sizeof tmpl[k]);
            if (vfs5011_extract_template(cap.img, VFS5011_IMAGE_WIDTH, cap.height, &tmpl[k]) == 0) {
                T[k].minutiae = tmpl[k].nrows; has_tmpl[k] = 1; T[k].ok = 1;
            }
            printf("      rows recorded %d, image height %d, minutiae %d\n", cap.recorded, cap.height, T[k].minutiae);
            snprintf(path, sizeof path, "%s/trial%02d_%c_%s.pgm", outdir, k + 1, 'A' + T[k].finger, T[k].is_enroll ? "enroll" : "verify");
            FILE *pg = fopen(path, "wb");
            if (pg) { fprintf(pg, "P5\n%d %d\n255\n", VFS5011_IMAGE_WIDTH, cap.height); fwrite(cap.img, 1, (size_t)VFS5011_IMAGE_WIDTH * cap.height, pg); fclose(pg); }
        }
        free(cap.img);
        if (!synthetic && k + 1 < nt) { printf("      LIFT your finger off completely...\a\n"); fflush(stdout); sleep(3); }
    }

    int ne = 0; for (int k = 0; k < nt; k++) if (T[k].is_enroll && has_tmpl[k]) ne++;
    OUT("\n=== Results ===\nEnrollment templates built from touch-downs: %d of %d\n", ne, n_enroll);
    for (int k = 0; k < nt; k++) {
        if (T[k].is_enroll) {
            OUT("  #%02d enroll A: image height %3d, minutiae %3d%s\n", k + 1, T[k].height, T[k].minutiae, T[k].minutiae >= 20 ? "" : "   (below the daemon's 20-minutiae minimum)");
            fprintf(csv, "%d,enroll,A,%d,%d,%d,\n", k + 1, T[k].recorded, T[k].height, T[k].minutiae);
            continue;
        }
        int best = -1;
        if (has_tmpl[k]) for (int j = 0; j < nt; j++) if (T[j].is_enroll && has_tmpl[j]) {
            int sc = vfs5011_match_score(&tmpl[k], &tmpl[j]);
            if (sc > best) best = sc;
        }
        T[k].score = best;
        OUT("  #%02d verify %c: image height %3d, minutiae %3d, best score %3d\n", k + 1, 'A' + T[k].finger, T[k].height, T[k].minutiae, best);
        fprintf(csv, "%d,verify,%c,%d,%d,%d,%d\n", k + 1, 'A' + T[k].finger, T[k].recorded, T[k].height, T[k].minutiae, best);
    }
    OUT("\nPass rate by match threshold (the daemon's default is 20), counting a failed capture as a reject:\n");
    OUT("  threshold   finger A accepted   finger B accepted\n");
    for (int t = 0; t < 4; t++) {
        int ga = 0, gt = 0, ia = 0, it = 0;
        for (int k = 0; k < nt; k++) {
            if (T[k].is_enroll) continue;
            int pass = T[k].score >= THRESH[t];
            if (T[k].finger == 0) { gt++; ga += pass; } else { it++; ia += pass; }
        }
        OUT("  %9d   %2d / %2d             %2d / %2d\n", THRESH[t], ga, gt, ia, it);
    }
    int gmin = 9999, gmax = -1, imax = -1, gs = 0, gn = 0;
    for (int k = 0; k < nt; k++) {
        if (T[k].is_enroll) continue;
        if (T[k].finger == 0) { if (T[k].score < gmin) gmin = T[k].score; if (T[k].score > gmax) gmax = T[k].score; gs += T[k].score; gn++; }
        else if (T[k].score > imax) imax = T[k].score;
    }
    OUT("\nFinger A scores: min %d, mean %.1f, max %d. Finger B highest score: %d.\n", gn ? gmin : 0, gn ? (double)gs / gn : 0.0, gmax, imax);
    OUT("Read: this needs finger A to pass clearly and finger B to stay well below, on few trials. With so few trials a\n"
        "clean result is only a hint, and any overlap means the landing alone is not enough.\n");
    fclose(rep); fclose(csv);
    printf("\nDone. Send me %s/report.txt, trials.csv and the .pgm pictures.\n", outdir);
    return 0;
}
