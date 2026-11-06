/*
 * htid_touch_probe2.c -- does a resting VFS5011 finger identify the owner?
 *
 * Follow-up to htid_touch_probe.c (experiment for a possible "touch to unlock"
 * mode, active-development only). The first probe showed that a finger resting
 * still gives a very stable single strip. This one asks the question that decides
 * whether the idea is usable: how much does that strip change when you LIFT the
 * finger and put it down again, compared with a different finger?
 *
 * It arms the sensor like the daemon does, then walks you through repeated
 * place / lift cycles, alternating finger A (your enrolled finger) and finger B
 * (a different finger of yours), optionally finger C (someone else's). From the
 * recorded strips it prints how well same-finger placements match each other
 * versus different fingers, using three feature variants, plus how fast a touch
 * settles and how fast a lift is visible. Nothing is enrolled and nothing is
 * written to the sensor's flash.
 *
 * Build (run from the repo root, same flags as the daemon):
 *   clang vfs5011/htid_touch_probe2.c -o htid_touch_probe2 -Ivfs5011 \
 *       -I/usr/local/include/libusb-1.0 -L/usr/local/lib -lusb-1.0
 *
 * Usage (stop the daemon first so the sensor is free):
 *   sudo ./htid_touch_probe2 [--placements N] [--hold S] [--third] [--out DIR]
 *   --placements N  place/lift cycles per finger (default 6, 3..10)
 *   --hold S        seconds to hold each placement (default 3.5)
 *   --third         add finger C (someone else) as a third class
 *   --synthetic     self-test of the analysis with made-up data, no sensor needed
 *
 * Output goes to DIR (default /tmp/htid-touch-probe2): report.txt and strips.csv.
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
#include <stdarg.h>

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


/* ------------------------------------------------------------------ */

#define PX VFS5011_IMAGE_WIDTH
#define MAXP 36        /* placements: up to 10 per finger x 3 fingers, plus headroom */
#define NSAMP 20       /* single lines sampled per placement for the single-line test */
#define NVAR 3
static const char *VAR_NAME[NVAR] = { "raw strip (no blank removal)", "blank removed", "blank removed + ridge band only" };
static const char *FINGER_NAME[3] = { "A (your enrolled finger)", "B (a different finger of yours)", "C (someone else's finger)" };

typedef struct { unsigned char *px; long long *t; int n, cap; } rec_t;
static void rec_init(rec_t *r, int cap) {
    r->px = malloc((size_t)cap * PX); r->t = malloc(sizeof(long long) * (size_t)cap); r->n = 0; r->cap = cap;
    if (!r->px || !r->t) { fprintf(stderr, "out of memory\n"); exit(1); }
}

static FILE *g_rep = NULL;
static void out(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void out(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
    if (g_rep) { va_start(ap, fmt); vfprintf(g_rep, fmt, ap); va_end(ap); }
}

/* ---- stream reader: lines are parsed across transfers so a partial read never shifts line boundaries ---- */
static unsigned char g_chunk[CAPTURE_LINES * VFS5011_LINE_SIZE];
static unsigned char g_acc[2 * CAPTURE_LINES * VFS5011_LINE_SIZE];
static int g_acc_len = 0, g_timeouts = 0;
static long long g_last_t = 0;

static int pump(libusb_device_handle *h, unsigned int to_ms, rec_t *sink, long long t0) {
    int n = 0;
    int r = libusb_bulk_transfer(h, VFS5011_IN_ENDPOINT_DATA, g_chunk, sizeof g_chunk, &n, to_ms);
    long long tnow = now_ms() - t0;
    if (r == LIBUSB_ERROR_TIMEOUT) g_timeouts++;
    else if (r != 0) { fprintf(stderr, "read failed: %s\n", libusb_error_name(r)); return -1; }
    if (n <= 0) { g_last_t = tnow; return 0; }
    if (g_acc_len + n > (int)sizeof g_acc) n = (int)sizeof g_acc - g_acc_len;
    memcpy(g_acc + g_acc_len, g_chunk, (size_t)n);
    g_acc_len += n;
    int nl = g_acc_len / VFS5011_LINE_SIZE;
    for (int i = 0; i < nl; i++) {
        if (sink && sink->n < sink->cap) {
            memcpy(sink->px + (size_t)sink->n * PX, g_acc + (size_t)i * VFS5011_LINE_SIZE + 8, PX);
            /* spread the lines of one transfer evenly over the time since the last one */
            sink->t[sink->n] = g_last_t + (tnow - g_last_t) * (i + 1) / nl;
            sink->n++;
        }
    }
    g_last_t = tnow;
    int used = nl * VFS5011_LINE_SIZE;
    memmove(g_acc, g_acc + used, (size_t)(g_acc_len - used));
    g_acc_len -= used;
    return 0;
}
static int run_phase(libusb_device_handle *h, int ms, rec_t *sink, long long t0) {
    long long end = now_ms() + ms;
    while (now_ms() < end) if (pump(h, 100, sink, t0) != 0) return -1;
    return 0;
}

/* ---- maths ---- */
static double pearson(const double *a, const double *b, int n) {
    double ma = 0, mb = 0, sab = 0, saa = 0, sbb = 0;
    for (int i = 0; i < n; i++) { ma += a[i]; mb += b[i]; }
    ma /= n; mb /= n;
    for (int i = 0; i < n; i++) { double x = a[i] - ma, y = b[i] - mb; sab += x * y; saa += x * x; sbb += y * y; }
    return (saa > 1e-9 && sbb > 1e-9) ? sab / sqrt(saa * sbb) : 0.0;
}
static double rms_diff(const double *a, const double *b, int n) {
    double s = 0; for (int i = 0; i < n; i++) { double d = a[i] - b[i]; s += d * d; } return sqrt(s / n);
}
static void highpass(const double *in, double *o, int n) {
    for (int i = 0; i < n; i++) {
        int lo = i - 7 < 0 ? 0 : i - 7, hi = i + 7 >= n ? n - 1 : i + 7;
        double s = 0; for (int j = lo; j <= hi; j++) s += in[j];
        o[i] = in[i] - s / (hi - lo + 1);
    }
}
/* the three feature variants of a strip, given the blank strip that belongs to it */
static void features(const double *raw, const double *blank, double f[NVAR][PX]) {
    double sub[PX];
    for (int i = 0; i < PX; i++) { f[0][i] = raw[i]; sub[i] = raw[i] - blank[i]; f[1][i] = sub[i]; }
    highpass(sub, f[2], PX);
}
static void line_as_double(const unsigned char *px, double *o) { for (int i = 0; i < PX; i++) o[i] = px[i]; }
static void window_mean(const rec_t *r, int ms, double *o, int *count) {
    memset(o, 0, sizeof(double) * PX);
    *count = 0;
    if (r->n == 0) return;
    long long tend = r->t[r->n - 1];
    for (int i = 0; i < r->n; i++) if (r->t[i] >= tend - ms) { for (int k = 0; k < PX; k++) o[k] += r->px[(size_t)i * PX + k]; (*count)++; }
    if (*count) for (int k = 0; k < PX; k++) o[k] /= *count;
}
static int cmp_dbl(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return (x > y) - (x < y); }
static double median(double *v, int n) { if (n == 0) return -1; qsort(v, (size_t)n, sizeof(double), cmp_dbl); return v[n / 2]; }

/* equal error rate over genuine/impostor score lists (higher score = better match) */
static void error_rates(const double *g, int ng, const double *im, int ni, double *eer, double *far_at_frr0, double *frr_at_far0) {
    double best = 2.0; *eer = 1.0;
    for (int k = 0; k < ng + ni; k++) {
        double th = k < ng ? g[k] : im[k - ng];
        int fr = 0, fa = 0;
        for (int i = 0; i < ng; i++) if (g[i] < th) fr++;
        for (int i = 0; i < ni; i++) if (im[i] >= th) fa++;
        double FRR = (double)fr / ng, FAR = (double)fa / ni;
        double d = fabs(FRR - FAR);
        if (d < best) { best = d; *eer = (FRR + FAR) / 2; }
    }
    double gmin = 2, imax = -2;
    for (int i = 0; i < ng; i++) if (g[i] < gmin) gmin = g[i];
    for (int i = 0; i < ni; i++) if (im[i] > imax) imax = im[i];
    int fa = 0; for (int i = 0; i < ni; i++) if (im[i] >= gmin) fa++;
    int fr = 0; for (int i = 0; i < ng; i++) if (g[i] <= imax) fr++;
    *far_at_frr0 = (double)fa / ni; *frr_at_far0 = (double)fr / ng;
}

/* ---- the analysis, shared by the real run and --synthetic ---- */
static void analyze(rec_t *fin, rec_t *blk, const int *finger, int np, const char *outdir) {
    static double M[MAXP][PX], B[MAXP][PX];
    static double F[NVAR][MAXP][PX];
    static double S[NVAR][MAXP][NSAMP][PX];
    int ns[MAXP], valid[MAXP], nwin[MAXP], nblk[MAXP];
    double settle_ms[MAXP], lift_ms[MAXP]; int nsettle = 0, nlift = 0;
    double contact[MAXP], resid[MAXP];
    char path[1024];

    out("\n=== Placements ===\n");
    for (int k = 0; k < np; k++) {
        window_mean(&fin[k], 1200, M[k], &nwin[k]);
        window_mean(&blk[k], 1000, B[k], &nblk[k]);
        valid[k] = nwin[k] >= 100 && nblk[k] >= 100;
    }
    for (int k = 0; k < np; k++) {
        double ft[NVAR][PX];
        features(M[k], B[k], ft);
        for (int v = 0; v < NVAR; v++) memcpy(F[v][k], ft[v], sizeof ft[v]);
        /* contact quality: strength of the blank-removed strip, and how much it still moves inside the window */
        double sm = 0, sq = 0; for (int i = 0; i < PX; i++) sm += ft[1][i];
        sm /= PX; for (int i = 0; i < PX; i++) sq += (ft[1][i] - sm) * (ft[1][i] - sm);
        contact[k] = sqrt(sq / PX);
        double rs = 0; int rc = 0; long long tend = fin[k].n ? fin[k].t[fin[k].n - 1] : 0;
        for (int i = 0; i < fin[k].n; i++) if (fin[k].t[i] >= tend - 1200) {
            double ld[PX]; line_as_double(fin[k].px + (size_t)i * PX, ld);
            rs += rms_diff(ld, M[k], PX); rc++;
        }
        resid[k] = rc ? rs / rc : 0;
        /* sample lines from the window for the single-line test */
        ns[k] = 0;
        if (valid[k]) {
            int first = -1, cnt = 0;
            for (int i = 0; i < fin[k].n; i++) if (fin[k].t[i] >= tend - 1200) { if (first < 0) first = i; cnt++; }
            for (int s = 0; s < NSAMP; s++) {
                int i = first + (int)((long long)cnt * s / NSAMP);
                double ld[PX], fv[NVAR][PX];
                line_as_double(fin[k].px + (size_t)i * PX, ld);
                features(ld, B[k], fv);
                for (int v = 0; v < NVAR; v++) memcpy(S[v][k][s], fv[v], sizeof fv[v]);
                ns[k]++;
            }
        }
        /* settle time of the touch and visibility of the lift, relative to the settled strip */
        double R = rms_diff(M[k], B[k], PX);
        settle_ms[k] = -1; lift_ms[k] = -1;
        if (valid[k] && R > 5) {
            int n = fin[k].n, on = -1;
            double *d = malloc(sizeof(double) * (size_t)n), *c = malloc(sizeof(double) * (size_t)n);
            double fsub[PX];
            for (int i = 0; i < PX; i++) fsub[i] = M[k][i] - B[k][i];
            for (int i = 0; i < n; i++) {
                double ld[PX], sub[PX]; line_as_double(fin[k].px + (size_t)i * PX, ld);
                for (int j = 0; j < PX; j++) sub[j] = ld[j] - B[k][j];
                d[i] = rms_diff(ld, B[k], PX); c[i] = pearson(sub, fsub, PX);
            }
            for (int i = 0; i + 15 < n && on < 0; i++) { double a = 0; for (int j = 0; j < 15; j++) a += d[i + j]; if (a / 15 > 0.3 * R) on = i; }
            if (on >= 0) for (int i = on; i + 30 < n; i++) {
                double a = 0; for (int j = 0; j < 30; j++) a += c[i + j];
                if (a / 30 >= 0.98) { settle_ms[k] = (double)(fin[k].t[i] - fin[k].t[on]); break; }
            }
            free(d); free(c);
            int nb = blk[k].n; double *e = malloc(sizeof(double) * (size_t)nb);
            for (int i = 0; i < nb; i++) { double ld[PX]; line_as_double(blk[k].px + (size_t)i * PX, ld); e[i] = rms_diff(ld, B[k], PX); }
            for (int i = 0; i + 30 < nb; i++) {
                double a = 0; for (int j = 0; j < 30; j++) a += e[i + j];
                if (a / 30 < 0.2 * R) { lift_ms[k] = (double)(blk[k].t[i] - blk[k].t[0]); break; }
            }
            free(e);
        }
        if (settle_ms[k] >= 0) nsettle++;
        if (lift_ms[k] >= 0) nlift++;
        char settle_txt[32];
        if (settle_ms[k] >= 0) snprintf(settle_txt, sizeof settle_txt, "%.0f ms", settle_ms[k]); else snprintf(settle_txt, sizeof settle_txt, "n/a");
        out("  #%02d finger %-34s lines %5d, contact strength %5.1f, movement in window %4.1f, settle %s, lift seen %s%s\n",
            k + 1, FINGER_NAME[finger[k]], nwin[k], contact[k], resid[k],
            settle_txt,
            lift_ms[k] >= 0 ? "yes" : "no",
            !valid[k] ? "   <-- NO DATA, redo" : (contact[k] < 25 ? "   <-- WEAK contact" : (resid[k] > 12 ? "   <-- finger moved" : "")));
    }
    {
        double st[MAXP], lf[MAXP]; int a = 0, b = 0;
        for (int k = 0; k < np; k++) { if (settle_ms[k] >= 0) st[a++] = settle_ms[k]; if (lift_ms[k] >= 0) lf[b++] = lift_ms[k]; }
        out("  settle time after the touch begins (strip >= 0.98 of final): median %.0f ms over %d placements\n", median(st, a), nsettle);
        out("  lift becomes visible (strip back near blank): median %.0f ms after LIFT over %d placements\n", median(lf, b), nlift);
    }

    for (int v = 0; v < NVAR; v++) {
        out("\n=== Feature variant %d: %s ===\n", v + 1, VAR_NAME[v]);
        out("Placement-to-placement correlation (rows and columns in order #1..#%d):\n      ", np);
        for (int j = 0; j < np; j++) out(" %c%02d ", 'A' + finger[j], j + 1);
        out("\n");
        double same[MAXP * MAXP], diff[MAXP * MAXP]; int nsame = 0, ndiff = 0;
        for (int i = 0; i < np; i++) {
            out("  %c%02d ", 'A' + finger[i], i + 1);
            for (int j = 0; j < np; j++) {
                double c = valid[i] && valid[j] ? pearson(F[v][i], F[v][j], PX) : 0;
                if (i == j) out("   -  "); else out("%5.2f ", c);
                if (j > i && valid[i] && valid[j]) { if (finger[i] == finger[j]) same[nsame++] = c; else diff[ndiff++] = c; }
            }
            out("\n");
        }
        if (nsame && ndiff) {
            double smin = 2, ssum = 0, dmax = -2, dsum = 0;
            for (int i = 0; i < nsame; i++) { if (same[i] < smin) smin = same[i]; ssum += same[i]; }
            for (int i = 0; i < ndiff; i++) { if (diff[i] > dmax) dmax = diff[i]; dsum += diff[i]; }
            out("Same finger, different placements : min %.2f, mean %.2f (%d pairs)\n", smin, ssum / nsame, nsame);
            out("Different fingers                 : max %.2f, mean %.2f (%d pairs)\n", dmax, dsum / ndiff, ndiff);
            out("Gap (lowest same - highest different): %+.2f   %s\n", smin - dmax, smin > dmax ? "(fully separated)" : "(OVERLAP: some different-finger pairs score higher than some same-finger pairs)");
        }

        /* single-line test: one line from a placement against the template of finger A built from the OTHER A placements */
        double g[MAXP * NSAMP], im[MAXP * NSAMP * 2]; int ng = 0, ni = 0;
        for (int i = 0; i < np; i++) {
            if (!valid[i] || ns[i] == 0) continue;
            double tpl[PX]; memset(tpl, 0, sizeof tpl); int nt = 0;
            for (int j = 0; j < np; j++) {
                if (!valid[j] || finger[j] != 0 || j == i) continue;
                double z[PX], m = 0; for (int q = 0; q < PX; q++) m += F[v][j][q]; m /= PX;
                double nrm = 0; for (int q = 0; q < PX; q++) { z[q] = F[v][j][q] - m; nrm += z[q] * z[q]; }
                nrm = sqrt(nrm); if (nrm < 1e-9) continue;
                for (int q = 0; q < PX; q++) tpl[q] += z[q] / nrm;
                nt++;
            }
            if (nt == 0) continue;
            for (int s = 0; s < ns[i]; s++) {
                double sc = pearson(S[v][i][s], tpl, PX);
                if (finger[i] == 0) g[ng++] = sc; else im[ni++] = sc;
            }
        }
        if (ng && ni) {
            double eer, fa0, fr0, gsum = 0, gmin = 2, isum = 0, imax = -2;
            for (int i = 0; i < ng; i++) { gsum += g[i]; if (g[i] < gmin) gmin = g[i]; }
            for (int i = 0; i < ni; i++) { isum += im[i]; if (im[i] > imax) imax = im[i]; }
            error_rates(g, ng, im, ni, &eer, &fa0, &fr0);
            out("Single line vs finger-A template (leave-one-placement-out): genuine min %.2f mean %.2f (%d lines), impostor max %.2f mean %.2f (%d lines)\n",
                gmin, gsum / ng, ng, imax, isum / ni, ni);
            out("  equal error rate about %.1f%%, impostors accepted when no genuine line is rejected %.1f%%, genuine rejected when no impostor is accepted %.1f%%\n",
                eer * 100, fa0 * 100, fr0 * 100);
        }
    }

    out("\nHow to read this: variant 3 (ridge band only) is the one that says whether real fingerprint detail is present;\n"
        "variants 1 and 2 may be matching the overall contact shape (finger size, pressure, position), which a different\n"
        "finger of the same person, or a re-placed finger, can change or imitate. The error rates come from only a few\n"
        "placements, so they say if the idea is hopeless or plausible, not what the real false accept rate would be.\n"
        "Each placement uses the blank frame recorded right after its own lift; a real unlock would have to get its blank\n"
        "from before the touch, which this does not test.\n");

    snprintf(path, sizeof path, "%s/strips.csv", outdir);
    FILE *cs = fopen(path, "w");
    if (cs) {
        fprintf(cs, "placement,finger,kind");
        for (int i = 0; i < PX; i++) fprintf(cs, ",p%d", i);
        fprintf(cs, "\n");
        for (int k = 0; k < np; k++) {
            fprintf(cs, "%d,%c,finger", k + 1, 'A' + finger[k]); for (int i = 0; i < PX; i++) fprintf(cs, ",%.2f", M[k][i]); fprintf(cs, "\n");
            fprintf(cs, "%d,%c,blank", k + 1, 'A' + finger[k]); for (int i = 0; i < PX; i++) fprintf(cs, ",%.2f", B[k][i]); fprintf(cs, "\n");
        }
        fclose(cs);
    }
}

/* ---- made-up data for --synthetic: checks the analysis and the output format, says nothing about the sensor ---- */
static unsigned lcg_state = 12345;
static double frand(void) { lcg_state = lcg_state * 1664525u + 1013904223u; return (lcg_state >> 8) / 16777216.0; }
static double gauss(void) { double s = 0; for (int i = 0; i < 12; i++) s += frand(); return s - 6.0; }
static void synth(rec_t *fin, rec_t *blk, int *finger, int np, int nf) {
    double base[PX], prof[3][PX];
    for (int i = 0; i < PX; i++) base[i] = 118 + 37 * sin(i * 0.11) + 8 * gauss();
    for (int f = 0; f < nf; f++) {
        double sm = 0;
        for (int i = 0; i < PX; i++) { sm = 0.9 * sm + 0.1 * gauss() * 18; prof[f][i] = sm * 3 + 6 * sin(i * (0.7 + 0.1 * f) + f); }
        double pm = 0, pv = 0;
        for (int i = 0; i < PX; i++) pm += prof[f][i];
        pm /= PX;
        for (int i = 0; i < PX; i++) { prof[f][i] -= pm; pv += prof[f][i] * prof[f][i]; }
        pv = sqrt(pv / PX);
        for (int i = 0; i < PX; i++) prof[f][i] *= 45.0 / pv;
    }
    for (int k = 0; k < np; k++) {
        finger[k] = k % nf;
        double shift = gauss() * 1.0, scale = 1 + gauss() * 0.04, drift = gauss() * 3;
        rec_init(&fin[k], 16000); rec_init(&blk[k], 16000);
        for (int i = 0; i < 10500; i++) {
            double ramp = i < 1500 ? 0.0 : (i < 3000 ? (i - 1500) / 1500.0 : 1.0);
            for (int q = 0; q < PX; q++) {
                int qs = q + (int)floor(shift); if (qs < 0) qs = 0; if (qs >= PX) qs = PX - 1;
                double v = base[q] + drift + ramp * scale * prof[finger[k]][qs] + gauss() * 4.0;
                fin[k].px[(size_t)i * PX + q] = (unsigned char)(v < 0 ? 0 : v > 255 ? 255 : v);
            }
            fin[k].t[i] = i / 3; fin[k].n++;
        }
        for (int i = 0; i < 7500; i++) {
            double ramp = i < 600 ? 1.0 - i / 600.0 : 0.0;
            for (int q = 0; q < PX; q++) {
                double v = base[q] + drift + ramp * prof[finger[k]][q] + gauss() * 4.0;
                blk[k].px[(size_t)i * PX + q] = (unsigned char)(v < 0 ? 0 : v > 255 ? 255 : v);
            }
            blk[k].t[i] = i / 3; blk[k].n++;
        }
    }
}

int main(int argc, char **argv) {
    int placements = 6, third = 0, synthetic = 0;
    double hold = 3.5;
    const char *outdir = "/tmp/htid-touch-probe2";
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--placements") && i + 1 < argc) placements = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--hold") && i + 1 < argc) hold = atof(argv[++i]);
        else if (!strcmp(argv[i], "--third")) third = 1;
        else if (!strcmp(argv[i], "--synthetic")) synthetic = 1;
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) outdir = argv[++i];
        else { fprintf(stderr, "usage: %s [--placements N] [--hold S] [--third] [--out DIR] [--synthetic]\n", argv[0]); return 2; }
    }
    if (placements < 3) placements = 3;
    if (placements > 10) placements = 10;
    if (hold < 2.5) hold = 2.5;
    if (hold > 8) hold = 8;
    int nf = third ? 3 : 2, np = placements * nf;
    mkdir(outdir, 0755);
    char path[1024];
    snprintf(path, sizeof path, "%s/report.txt", outdir);
    g_rep = fopen(path, "w");
    if (!g_rep) { fprintf(stderr, "Cannot write to %s\n", outdir); return 1; }

    static rec_t fin[MAXP], blk[MAXP];
    int finger[MAXP];
    libusb_context *ctx = NULL;
    libusb_device_handle *h = NULL;

    if (synthetic) {
        out("VFS5011 touch probe 2 (SYNTHETIC SELF-TEST: made-up data, not the sensor)\n");
        synth(fin, blk, finger, np, nf);
    } else {
        out("VFS5011 touch probe 2: %d placements per finger, %d fingers, hold %.1f s\n", placements, nf, hold);
        if (libusb_init(&ctx) < 0) { fprintf(stderr, "libusb_init failed\n"); return 1; }
        libusb_set_option(ctx, LIBUSB_OPTION_LOG_LEVEL, LIBUSB_LOG_LEVEL_NONE);
        for (int i = 0; i < 5 && !h; i++) { h = libusb_open_device_with_vid_pid(ctx, VID, PID); if (!h) usleep(300000); }
        if (!h) { fprintf(stderr, "VFS5011 (138a:0018) not found. Is the daemon stopped?\n"); libusb_exit(ctx); return 1; }
        libusb_set_auto_detach_kernel_driver(h, 1);
        if (libusb_claim_interface(h, 0) != 0) { fprintf(stderr, "Could not claim interface 0. Stop the daemon and try again.\n"); libusb_close(h); libusb_exit(ctx); return 1; }
        printf("Arming the sensor...\n");
        if (run_sequence(h, vfs5011_initialization, sizeof(vfs5011_initialization) / sizeof(vfs5011_initialization[0])) != 0 ||
            run_sequence(h, vfs5011_initiate_capture, sizeof(vfs5011_initiate_capture) / sizeof(vfs5011_initiate_capture[0])) != 0) {
            fprintf(stderr, "Sensor init failed. Stop the daemon, wait 10 s (or power-cycle) and retry.\n");
            libusb_release_interface(h, 0); libusb_close(h); libusb_exit(ctx); return 1;
        }
        printf("\nPlace each finger flat, the way you would to unlock, and keep it still until it says LIFT.\n"
               "Lift it completely and keep it off until the next prompt. A beep marks GO and LIFT.\n");
        long long t0 = now_ms();
        int aborted = 0;
        for (int k = 0; k < np && !aborted; k++) {
            finger[k] = k % nf;
            rec_init(&fin[k], 40000); rec_init(&blk[k], 40000);
            printf("\n[%d/%d] NEXT: finger %s\n      get ready", k + 1, np, FINGER_NAME[finger[k]]); fflush(stdout);
            if (run_phase(h, 2500, NULL, t0) != 0) { aborted = 1; break; }
            printf(" ... PLACE IT NOW\a\n"); fflush(stdout);
            if (run_phase(h, (int)(hold * 1000), &fin[k], t0) != 0) { aborted = 1; break; }
            printf("      LIFT\a\n"); fflush(stdout);
            if (run_phase(h, 2500, &blk[k], t0) != 0) { aborted = 1; break; }
            printf("      %d + %d lines\n", fin[k].n, blk[k].n);
        }
        libusb_clear_halt(h, VFS5011_IN_ENDPOINT_CTRL);
        libusb_clear_halt(h, VFS5011_IN_ENDPOINT_DATA);
        libusb_clear_halt(h, VFS5011_OUT_ENDPOINT);
        libusb_release_interface(h, 0);
        libusb_close(h);
        libusb_exit(ctx);
        if (aborted) { fprintf(stderr, "\nStopped early by a read error.\n"); fclose(g_rep); return 1; }
        out("read timeouts: %d\n", g_timeouts);
    }
    analyze(fin, blk, finger, np, outdir);
    fclose(g_rep);
    printf("\nDone. Send me %s/report.txt and %s/strips.csv\n", outdir, outdir);
    return 0;
}
