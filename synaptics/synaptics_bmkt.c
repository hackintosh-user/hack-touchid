/*
 * synaptics_bmkt.c
 *
 * Transport and BMKT framing for the Synaptics Prometheus 06cb:00f0
 * (match-in-sensor, no TLS, no pairing, no calibration). Wire format from
 * PROTOCOL-06cb-00f0.md (labrouss); reference is libfprint's synaptics
 * driver (LGPL-2.1).
 *
 * Standalone smoke test (open, GET_VERSION, FPS_INIT, power down):
 *   build with -DSYN_STANDALONE_TEST, see scripts/build_synaptics_test.sh
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "synaptics_bmkt.h"
#include "synaptics_proto.h"

#define SYN_XFER_TIMEOUT_MS 2000
#define SYN_RX_CAP          512

static void hexdump(const char *tag, const uint8_t *b, size_t n) {
    printf("  %s (%zu):", tag, n);
    for (size_t i = 0; i < n; i++) printf(" %02x", b[i]);
    printf("\n");
}

static int bulk_out(syn_dev_t *d, const uint8_t *buf, size_t n) {
    int got = 0;
    if (d->verbose) hexdump("OUT", buf, n);
    int r = libusb_bulk_transfer(d->h, SYN_EP_OUT, (unsigned char *)buf, (int)n,
                                 &got, SYN_XFER_TIMEOUT_MS);
    if (r != 0 || got != (int)n) {
        fprintf(stderr, "bulk OUT failed: %s (sent %d of %zu)\n",
                libusb_error_name(r), got, n);
        return -1;
    }
    return 0;
}

static int bulk_in(syn_dev_t *d, uint8_t *buf, size_t cap, int timeout_ms) {
    int got = 0;
    int r = libusb_bulk_transfer(d->h, SYN_EP_IN, buf, (int)cap, &got, timeout_ms);
    if (r != 0) {
        if (r != LIBUSB_ERROR_TIMEOUT)
            fprintf(stderr, "bulk IN failed: %s\n", libusb_error_name(r));
        return -1;
    }
    if (d->verbose) hexdump("IN ", buf, (size_t)got);
    return got;
}

/* Polls the interrupt endpoint. Returns 1 if an "async message pending"
 * report arrived, 0 on timeout, -1 on error. */
static int int_wait(syn_dev_t *d, int timeout_ms) {
    uint8_t rep[16];
    int got = 0;
    int r = libusb_interrupt_transfer(d->h, SYN_EP_INT, rep, sizeof(rep), &got, timeout_ms);
    if (r == LIBUSB_ERROR_TIMEOUT) return 0;
    if (r != 0) {
        fprintf(stderr, "interrupt IN failed: %s\n", libusb_error_name(r));
        return -1;
    }
    if (d->verbose) hexdump("INT", rep, (size_t)got);
    return (got >= 1 && (rep[0] & SYN_INT_ASYNC_PENDING)) ? 1 : 0;
}

/* Parses "status(2) + BMKT message" or a bare BMKT message at buf. */
static int parse_msg(const uint8_t *buf, int n, syn_msg_t *m) {
    int off = -1;
    if (n >= 6 && buf[2] == SYN_BMKT_MAGIC) off = 2;      /* status + message */
    else if (n >= 4 && buf[0] == SYN_BMKT_MAGIC) off = 0; /* bare message */
    if (off < 0) return -1;
    m->seq    = buf[off + 1];
    m->msg_id = buf[off + 2];
    m->len    = buf[off + 3];
    if (m->len > SYN_MAX_PAYLOAD || off + 4 + m->len > n) return -1;
    memcpy(m->payload, buf + off + 4, m->len);
    return 0;
}

/* Sends 0xA8 and reads the pending BMKT message. */
static int read_async(syn_dev_t *d, syn_msg_t *m) {
    uint8_t cmd = SYN_CMD_ASYNCMSG_READ;
    uint8_t rx[SYN_RX_CAP];
    if (bulk_out(d, &cmd, 1) != 0) return -1;
    int n = bulk_in(d, rx, sizeof(rx), SYN_XFER_TIMEOUT_MS);
    if (n < 2) return -1;
    if (rx[0] != 0 || rx[1] != 0) {
        fprintf(stderr, "ASYNCMSG_READ status %02x %02x\n", rx[0], rx[1]);
        return -1;
    }
    return parse_msg(rx, n, m);
}

int syn_open(syn_dev_t *d, int verbose) {
    memset(d, 0, sizeof(*d));
    d->verbose = verbose;
    d->iface = -1;

    int r = libusb_init(&d->ctx);
    if (r != 0) { fprintf(stderr, "libusb_init: %s\n", libusb_error_name(r)); return -1; }

    d->h = libusb_open_device_with_vid_pid(d->ctx, SYN_VID, SYN_PID);
    if (!d->h) {
        fprintf(stderr, "06cb:00f0 not found (or no permission, try sudo)\n");
        libusb_exit(d->ctx);
        d->ctx = NULL;
        return -1;
    }

    libusb_device *dev = libusb_get_device(d->h);
    struct libusb_config_descriptor *cfg = NULL;
    if (libusb_get_active_config_descriptor(dev, &cfg) == 0) {
        for (int i = 0; i < cfg->bNumInterfaces && d->iface < 0; i++) {
            const struct libusb_interface_descriptor *a = &cfg->interface[i].altsetting[0];
            if (a->bInterfaceClass == 0xFF) d->iface = a->bInterfaceNumber;
        }
        libusb_free_config_descriptor(cfg);
    }
    if (d->iface < 0) d->iface = 0;

    /* macOS has no kernel driver to detach for this device (it enumerates as
     * a vendor-specific device); on other hosts auto-detach is harmless. */
    libusb_set_auto_detach_kernel_driver(d->h, 1);

    r = libusb_claim_interface(d->h, d->iface);
    if (r != 0) {
        fprintf(stderr, "claim interface %d failed: %s\n", d->iface, libusb_error_name(r));
        libusb_close(d->h);
        libusb_exit(d->ctx);
        d->h = NULL;
        d->ctx = NULL;
        return -1;
    }
    printf("Opened 06cb:00f0, vendor interface %d claimed\n", d->iface);
    return 0;
}

void syn_close(syn_dev_t *d) {
    if (d->h) {
        libusb_release_interface(d->h, d->iface);
        libusb_close(d->h);
        d->h = NULL;
    }
    if (d->ctx) {
        libusb_exit(d->ctx);
        d->ctx = NULL;
    }
}

int syn_get_version(syn_dev_t *d, uint8_t *out, size_t cap) {
    uint8_t cmd = SYN_CMD_GET_VERSION;
    uint8_t rx[SYN_RX_CAP];
    if (bulk_out(d, &cmd, 1) != 0) return -1;
    int n = bulk_in(d, rx, sizeof(rx), SYN_XFER_TIMEOUT_MS);
    if (n < 0) return -1;
    if ((size_t)n > cap) n = (int)cap;
    memcpy(out, rx, (size_t)n);
    return n;
}

int syn_send_bmkt(syn_dev_t *d, uint8_t msg_id, const uint8_t *payload,
                  uint8_t payload_len, syn_msg_t *reply, int timeout_ms) {
    if (msg_id == SYN_MSG_DEL_FULL_DB) {
        fprintf(stderr, "refused: DEL_FULL_DB would erase Windows Hello and BIOS prints\n");
        return -1;
    }
    if (payload_len > SYN_MAX_PAYLOAD) return -1;

    d->seq = (uint8_t)(d->seq >= 255 ? 1 : d->seq + 1);   /* 0 is reserved for events */

    uint8_t tx[5 + SYN_MAX_PAYLOAD];
    tx[0] = SYN_CMD_ACE_COMMAND;
    tx[1] = SYN_BMKT_MAGIC;
    tx[2] = d->seq;
    tx[3] = msg_id;
    tx[4] = payload_len;
    if (payload_len) memcpy(tx + 5, payload, payload_len);
    if (bulk_out(d, tx, (size_t)5 + payload_len) != 0) return -1;

    /* The ACE reply is a status word, optionally with the BMKT reply inline.
     * Otherwise the reply is announced on the interrupt endpoint. */
    uint8_t rx[SYN_RX_CAP];
    int n = bulk_in(d, rx, sizeof(rx), SYN_XFER_TIMEOUT_MS);
    if (n < 2) return -1;
    if (rx[0] != 0 || rx[1] != 0) {
        fprintf(stderr, "ACE_COMMAND status %02x %02x\n", rx[0], rx[1]);
        return -1;
    }
    if (n >= 6 && parse_msg(rx, n, reply) == 0 && reply->seq == d->seq) return 0;

    time_t start = time(NULL);
    while ((time(NULL) - start) * 1000 < timeout_ms) {
        int w = int_wait(d, 500);
        if (w < 0) return -1;
        if (w == 0) continue;
        syn_msg_t m;
        if (read_async(d, &m) != 0) return -1;
        if (m.seq == d->seq) { *reply = m; return 0; }
        if (d->verbose)
            printf("  (ignored async msg 0x%02x seq %u while waiting for seq %u)\n",
                   m.msg_id, m.seq, d->seq);
    }
    fprintf(stderr, "timeout waiting for reply to msg 0x%02x\n", msg_id);
    return -1;
}

int syn_wait_event(syn_dev_t *d, syn_msg_t *ev, int timeout_ms) {
    time_t start = time(NULL);
    while ((time(NULL) - start) * 1000 < timeout_ms) {
        int w = int_wait(d, 500);
        if (w < 0) return -1;
        if (w == 0) continue;
        if (read_async(d, ev) == 0) return 0;
        return -1;
    }
    return -1;
}

int syn_fps_init(syn_dev_t *d) {
    syn_msg_t r;
    if (syn_send_bmkt(d, SYN_MSG_FPS_INIT, NULL, 0, &r, 3000) != 0) return -1;
    if (r.msg_id == SYN_MSG_FPS_INIT_OK) return 0;
    if (r.msg_id == SYN_MSG_FPS_INIT_FAIL && r.len >= 2 &&
        r.payload[0] == 0x00 && r.payload[1] == 0x67) return 0;   /* already initialised */
    fprintf(stderr, "FPS_INIT unexpected reply 0x%02x\n", r.msg_id);
    return -1;
}

void syn_power_down(syn_dev_t *d) {
    syn_msg_t r;
    syn_send_bmkt(d, SYN_MSG_POWER_DOWN_NOTIFY, NULL, 0, &r, 1000);
}

#ifdef SYN_STANDALONE_TEST
int main(int argc, char **argv) {
    int verbose = 1;
    for (int i = 1; i < argc; i++)
        if (strcmp(argv[i], "--quiet") == 0) verbose = 0;

    printf("Synaptics Prometheus 06cb:00f0 open test (read-only: no enroll, no delete)\n");
    syn_dev_t d;
    if (syn_open(&d, verbose) != 0) return 1;

    uint8_t ver[64];
    int n = syn_get_version(&d, ver, sizeof(ver));
    if (n < 0) { printf("RESULT: GET_VERSION failed\n"); syn_close(&d); return 2; }
    printf("GET_VERSION: %d bytes\n", n);
    if (!verbose) hexdump("reply", ver, (size_t)n);

    if (syn_fps_init(&d) != 0) { printf("RESULT: FPS_INIT failed\n"); syn_close(&d); return 3; }
    printf("FPS_INIT: OK\n");

    syn_power_down(&d);
    syn_close(&d);
    printf("RESULT: OK, sensor answered GET_VERSION and FPS_INIT\n");
    return 0;
}
#endif
