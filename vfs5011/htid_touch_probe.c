/*
 * htid_touch_probe.c -- what does the VFS5011 send for a finger that is NOT moving?
 *
 * Experiment for a possible "touch to unlock" mode (active-development only).
 * It arms the sensor exactly like the daemon does, then records every raw line
 * the sensor sends while you follow the prompts: nothing on the sensor, a finger
 * resting still, nothing again, then (optionally) a second finger. Nothing is
 * matched or enrolled and nothing is written to the sensor's flash.
 *
 * Build (run from the repo root, same flags as the daemon):
 *   clang vfs5011/htid_touch_probe.c -o htid_touch_probe -Ivfs5011 \
 *       -I/usr/local/include/libusb-1.0 -L/usr/local/lib -lusb-1.0
 *
 * Usage (stop the daemon first so the sensor is free):
 *   ./htid_touch_probe [--seconds N] [--second-finger] [--out DIR]
 *
 * Output goes to DIR (default /tmp/htid-touch-probe): report.txt, lines.csv,
 * raw.bin and one .pgm picture per phase. Send report.txt, lines.csv and the .pgm files.
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

typedef struct { const char *name; const char *prompt; int seconds; } phase_t;

typedef struct {
    int lines, present, new_rows, first_present_idx;
    long long first_present_ms, first_line_ms, last_line_ms;
    double dev_sum, diff_sum; int diff_n;
    int dev_min, dev_max;
    int run_present, longest_present_run;
} stats_t;

static void stats_init(stats_t *s) { memset(s, 0, sizeof *s); s->dev_min = 1 << 30; s->first_present_idx = -1; }

int main(int argc, char **argv) {
    int seconds = 5, second_finger = 0;
    const char *outdir = "/tmp/htid-touch-probe";
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--second-finger")) second_finger = 1;
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) outdir = argv[++i];
        else { fprintf(stderr, "usage: %s [--seconds N] [--second-finger] [--out DIR]\n", argv[0]); return 2; }
    }
    if (seconds < 2) seconds = 2;
    if (seconds > 30) seconds = 30;
    mkdir(outdir, 0755);

    phase_t phases[MAX_PHASES]; int nph = 0;
    phases[nph++] = (phase_t){ "A-no-finger", "Do NOT touch the sensor.", 3 };
    phases[nph++] = (phase_t){ "B-rest-finger", "Put your finger flat on the sensor and keep it STILL (do not swipe).", seconds };
    phases[nph++] = (phase_t){ "C-lifted", "Lift your finger off completely.", 3 };
    if (second_finger) {
        phases[nph++] = (phase_t){ "D-second-finger", "Rest a DIFFERENT finger (or someone else's) on the sensor, keep it STILL.", seconds };
        phases[nph++] = (phase_t){ "E-lifted", "Lift it off completely.", 3 };
    }

    libusb_context *ctx = NULL;
    if (libusb_init(&ctx) < 0) { fprintf(stderr, "libusb_init failed\n"); return 1; }
    libusb_set_option(ctx, LIBUSB_OPTION_LOG_LEVEL, LIBUSB_LOG_LEVEL_NONE);
    libusb_device_handle *h = NULL;
    for (int i = 0; i < 5 && !h; i++) { h = libusb_open_device_with_vid_pid(ctx, VID, PID); if (!h) usleep(300000); }
    if (!h) { fprintf(stderr, "VFS5011 (138a:0018) not found. Is the daemon stopped?\n"); libusb_exit(ctx); return 1; }
    libusb_set_auto_detach_kernel_driver(h, 1);
    if (libusb_claim_interface(h, 0) != 0) { fprintf(stderr, "Could not claim interface 0. Stop the daemon and try again.\n"); libusb_close(h); libusb_exit(ctx); return 1; }

    printf("Arming the sensor...\n");
    if (run_sequence(h, vfs5011_initialization, sizeof(vfs5011_initialization) / sizeof(vfs5011_initialization[0])) != 0 ||
        run_sequence(h, vfs5011_initiate_capture, sizeof(vfs5011_initiate_capture) / sizeof(vfs5011_initiate_capture[0])) != 0) {
        fprintf(stderr, "Sensor init failed. Unplug/replug is not possible on a laptop: stop the daemon, wait 5 s, retry.\n");
        libusb_release_interface(h, 0); libusb_close(h); libusb_exit(ctx); return 1;
    }

    char path[1024];
    snprintf(path, sizeof path, "%s/lines.csv", outdir);
    FILE *csv = fopen(path, "w");
    snprintf(path, sizeof path, "%s/raw.bin", outdir);
    FILE *raw = fopen(path, "wb");
    snprintf(path, sizeof path, "%s/report.txt", outdir);
    FILE *rep = fopen(path, "w");
    if (!csv || !raw || !rep) { fprintf(stderr, "Cannot write to %s\n", outdir); return 1; }
    fprintf(csv, "phase,line_index,t_ms,deviation,diff_prev,new_row\n");

    unsigned char *chunk = malloc((size_t)CAPTURE_LINES * VFS5011_LINE_SIZE);
    unsigned char *acc = malloc((size_t)2 * CAPTURE_LINES * VFS5011_LINE_SIZE);
    int acc_len = 0;
    unsigned char prev[VFS5011_LINE_SIZE]; int have_prev = 0;
    unsigned char lastrow[VFS5011_LINE_SIZE]; int have_row = 0;
    long long t0 = now_ms();
    int global_idx = 0, read_errors = 0, timeouts = 0, aborted = 0;
    long long bytes_total = 0;
    stats_t all[MAX_PHASES];

    fprintf(rep, "VFS5011 touch probe\nstart: %lld ms epoch, phases: %d, rest time per finger phase: %d s\n\n", t0, nph, seconds);

    for (int p = 0; p < nph && !aborted; p++) {
        stats_t *s = &all[p]; stats_init(s);
        printf("\n[%d/%d] %s  (%d s)\n", p + 1, nph, phases[p].prompt, phases[p].seconds);
        printf("      Starting in 3 s...\n"); fflush(stdout);
        /* keep draining the sensor during the countdown so nothing queues up */
        long long cd_end = now_ms() + 3000;
        while (now_ms() < cd_end) {
            int n = 0;
            int r = libusb_bulk_transfer(h, VFS5011_IN_ENDPOINT_DATA, chunk, CAPTURE_LINES * VFS5011_LINE_SIZE, &n, 100);
            if (r != 0 && r != LIBUSB_ERROR_TIMEOUT) { read_errors++; break; }
        }
        acc_len = 0; have_prev = 0;
        printf("      GO\n"); fflush(stdout);

        /* pixel rows of this phase, for the picture */
        int cap_rows = 20000;
        unsigned char *pic = malloc((size_t)cap_rows * VFS5011_LINE_SIZE);
        int pic_rows = 0;

        long long ph_start = now_ms(), ph_end = ph_start + (long long)phases[p].seconds * 1000;
        while (now_ms() < ph_end) {
            int n = 0;
            int r = libusb_bulk_transfer(h, VFS5011_IN_ENDPOINT_DATA, chunk, CAPTURE_LINES * VFS5011_LINE_SIZE, &n, 100);
            if (r == LIBUSB_ERROR_TIMEOUT) { timeouts++; }
            else if (r != 0) { fprintf(stderr, "read failed: %s\n", libusb_error_name(r)); read_errors++; aborted = 1; break; }
            if (n <= 0) continue;
            bytes_total += n;
            if (acc_len + n > 2 * CAPTURE_LINES * VFS5011_LINE_SIZE) n = 2 * CAPTURE_LINES * VFS5011_LINE_SIZE - acc_len;
            memcpy(acc + acc_len, chunk, (size_t)n);
            acc_len += n;
            int nl = acc_len / VFS5011_LINE_SIZE;
            long long t = now_ms() - t0;
            for (int i = 0; i < nl; i++) {
                unsigned char *line = acc + (size_t)i * VFS5011_LINE_SIZE;
                int dev = deviation(line + 8, VFS5011_IMAGE_WIDTH);
                int dp = have_prev ? diff_norm(prev + 8, line + 8, VFS5011_IMAGE_WIDTH) : -1;
                int present = dev >= DEVIATION_THRESHOLD;
                int newrow = present && (!have_row || diff_norm(lastrow + 8, line + 8, VFS5011_IMAGE_WIDTH) >= DIFFERENCE_THRESHOLD);
                if (newrow) { memcpy(lastrow, line, VFS5011_LINE_SIZE); have_row = 1; }
                fprintf(csv, "%s,%d,%lld,%d,%d,%d\n", phases[p].name, global_idx, t, dev, dp, newrow);
                fwrite(line, 1, VFS5011_LINE_SIZE, raw);
                if (pic_rows < cap_rows) { memcpy(pic + (size_t)pic_rows * VFS5011_LINE_SIZE, line, VFS5011_LINE_SIZE); pic_rows++; }
                if (s->lines == 0) s->first_line_ms = t;
                s->last_line_ms = t;
                s->lines++;
                s->dev_sum += dev; if (dev < s->dev_min) s->dev_min = dev; if (dev > s->dev_max) s->dev_max = dev;
                if (dp >= 0) { s->diff_sum += dp; s->diff_n++; }
                if (present) {
                    s->present++; s->run_present++;
                    if (s->run_present > s->longest_present_run) s->longest_present_run = s->run_present;
                    if (s->first_present_idx < 0) { s->first_present_idx = s->lines - 1; s->first_present_ms = t - (ph_start - t0); }
                } else s->run_present = 0;
                if (newrow) s->new_rows++;
                memcpy(prev, line, VFS5011_LINE_SIZE); have_prev = 1;
                global_idx++;
            }
            int used = nl * VFS5011_LINE_SIZE;
            memmove(acc, acc + used, (size_t)(acc_len - used));
            acc_len -= used;
        }

        /* picture of this phase: the 160 pixel columns of every line */
        if (pic_rows > 0) {
            snprintf(path, sizeof path, "%s/%s.pgm", outdir, phases[p].name);
            FILE *pg = fopen(path, "wb");
            if (pg) {
                fprintf(pg, "P5\n%d %d\n255\n", VFS5011_IMAGE_WIDTH, pic_rows);
                for (int y = 0; y < pic_rows; y++) fwrite(pic + (size_t)y * VFS5011_LINE_SIZE + 8, 1, VFS5011_IMAGE_WIDTH, pg);
                fclose(pg);
            }
        }
        free(pic);

        double secs = phases[p].seconds;
        fprintf(rep, "== Phase %s (%d s): %s\n", phases[p].name, phases[p].seconds, phases[p].prompt);
        fprintf(rep, "   lines received            : %d (%.0f lines/s)\n", s->lines, s->lines / secs);
        fprintf(rep, "   lines above finger test   : %d (%.1f%%)\n", s->present, s->lines ? 100.0 * s->present / s->lines : 0.0);
        fprintf(rep, "   longest run above test    : %d lines\n", s->longest_present_run);
        fprintf(rep, "   first line above test     : %s", s->first_present_idx < 0 ? "never\n" : "");
        if (s->first_present_idx >= 0) fprintf(rep, "line %d, %lld ms after GO\n", s->first_present_idx, s->first_present_ms);
        fprintf(rep, "   deviation min/avg/max     : %d / %.0f / %d (finger test threshold %d)\n",
                s->lines ? s->dev_min : 0, s->lines ? s->dev_sum / s->lines : 0.0, s->dev_max, DEVIATION_THRESHOLD);
        fprintf(rep, "   avg diff between lines    : %.1f (daemon treats >= %d as a new row)\n", s->diff_n ? s->diff_sum / s->diff_n : 0.0, DIFFERENCE_THRESHOLD);
        fprintf(rep, "   rows the daemon would keep: %d\n\n", s->new_rows);
        printf("      %d lines, %d above finger test, %d would-be rows\n", s->lines, s->present, s->new_rows);
    }

    fprintf(rep, "== Totals\n   bytes read %lld, read timeouts %d, read errors %d%s\n", bytes_total, timeouts, read_errors,
            aborted ? " (stopped early by a read error)" : "");
    fprintf(rep, "   line stride %d bytes, picture uses bytes 8..%d of each line\n", VFS5011_LINE_SIZE, 8 + VFS5011_IMAGE_WIDTH - 1);
    fprintf(rep, "   Read: if B shows lots of lines above the test but only 1-3 would-be rows, the still finger\n"
                 "   gives a repeating strip (not an image). Compare A and C with B for the noise floor.\n");

    fclose(csv); fclose(raw); fclose(rep);
    free(chunk); free(acc);
    libusb_clear_halt(h, VFS5011_IN_ENDPOINT_CTRL);
    libusb_clear_halt(h, VFS5011_IN_ENDPOINT_DATA);
    libusb_clear_halt(h, VFS5011_OUT_ENDPOINT);
    libusb_release_interface(h, 0);
    libusb_close(h);
    libusb_exit(ctx);
    printf("\nDone. Send me %s/report.txt, lines.csv and the .pgm files.\n", outdir);
    return aborted ? 1 : 0;
}
