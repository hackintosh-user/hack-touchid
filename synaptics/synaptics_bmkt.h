/*
 * synaptics_bmkt.h
 *
 * Transport and BMKT framing for the Synaptics Prometheus 06cb:00f0.
 * First milestone: open the device, GET_VERSION, FPS_INIT, close.
 * See PROTOCOL-06cb-00f0.md for the observed wire format.
 */

#ifndef __SYNAPTICS_BMKT_H
#define __SYNAPTICS_BMKT_H

#include <libusb.h>
#include <stddef.h>
#include <stdint.h>

#define SYN_MAX_PAYLOAD 240

typedef struct {
    uint8_t seq;
    uint8_t msg_id;
    uint8_t len;
    uint8_t payload[SYN_MAX_PAYLOAD];
} syn_msg_t;

typedef struct {
    libusb_context       *ctx;
    libusb_device_handle *h;
    int                   iface;
    uint8_t               seq;     /* last sequence number used, 1..255 */
    int                   verbose; /* hex-dump every transfer */
} syn_dev_t;

/* Opens 06cb:00f0, finds the vendor-class interface, claims it.
 * Returns 0 on success, -1 on failure (reason printed to stderr). */
int  syn_open(syn_dev_t *d, int verbose);
void syn_close(syn_dev_t *d);

/* 0x01 GET_VERSION. Copies the raw reply (status bytes included) into out.
 * Returns bytes read, or -1. */
int  syn_get_version(syn_dev_t *d, uint8_t *out, size_t cap);

/* Sends one BMKT message wrapped in 0xA7 and waits for the matching reply
 * (synchronous, or announced on the interrupt endpoint and fetched with
 * 0xA8). Returns 0 and fills *reply, or -1 on timeout or transport error.
 * Refuses to send SYN_MSG_DEL_FULL_DB: it also erases Windows Hello and
 * BIOS prints, so no HTID code path may ever send it. */
int  syn_send_bmkt(syn_dev_t *d, uint8_t msg_id, const uint8_t *payload,
                   uint8_t payload_len, syn_msg_t *reply, int timeout_ms);

/* Waits for the next unsolicited BMKT event (seq 0). 0 ok, -1 timeout. */
int  syn_wait_event(syn_dev_t *d, syn_msg_t *ev, int timeout_ms);

/* Convenience: 0x11 FPS_INIT. Returns 0 if OK or already initialised. */
int  syn_fps_init(syn_dev_t *d);

/* Convenience: 0xA1 POWER_DOWN_NOTIFY, best effort. */
void syn_power_down(syn_dev_t *d);

#endif /* __SYNAPTICS_BMKT_H */
