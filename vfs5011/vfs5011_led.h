/*
 * vfs5011_led.h
 *
 * LED control for the Validity VFS5011 (138a:0018) swipe sensor.
 *
 * Header-only on purpose (static inline, no libusb, no globals): it is
 * included by the daemon, the client and the unit test, so there is no new
 * .c file to add to any build line.
 *
 * Where this comes from: the Windows driver (HP softpaq sp58900, usdk
 * 4.4.234, scsFwV4.c / scsLED.c) builds the "LED_EX" command, firmware
 * opcode 0x2a, 117 bytes, which this sensor family accepts as a plain
 * bulk-OUT command on endpoint 0x01 with a 2-byte 00 00 status reply on the
 * control IN endpoint. The two 117-byte blobs already replayed by the capture
 * sequences (vfs5011_init_17 and vfs5011_prepare_03) are exactly this command:
 * the idle "ready for swipe" blink on LED port 0. The builder below was
 * checked against that blob byte for byte (tests/test_led.c).
 *
 * Packet layout (117 bytes):
 *   [0]        0x2a opcode
 *   [1..112]   16 entries x 7 bytes = 4 LED ports x 4 slots
 *              entry = { flag, level, 0, 0, level, time_lo, time_hi }
 *              Only slot 1 (on) and slot 3 (off) of the chosen port are used:
 *              slot 1 = level + time in ms the LED stays on,
 *              slot 3 = level + time in ms the LED stays off.
 *   [113..116] pattern duration in ms, little endian (the whole program
 *              repeats until this runs out; 1 is used for "all off")
 *
 * Ports: the driver defaults to port 0 for normal states (ready, accept) and
 * port 2 for the attention state. Tested on the HP DV6-7070ex (138a:0018):
 * all four ports accept the command, and port 2 is the red LED (port 3 shows white).
 * So 2 is the default here; led_red_port.conf overrides it for a laptop wired differently.
 */
#ifndef VFS5011_LED_H
#define VFS5011_LED_H

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define VFS5011_LED_OPCODE        0x2a
#define VFS5011_LED_PKT_LEN       117
#define VFS5011_LED_PORT_NORMAL   0
#define VFS5011_LED_PORT_ATTENTION 2 /* red on the HP DV6 (tested, port walk); matches the driver default */

#define VFS5011_LED_CONF_DIR        "/usr/local/libexec/hack-touchid/"
#define VFS5011_LED_BRIGHTNESS_CONF VFS5011_LED_CONF_DIR "led_brightness.conf" /* 0..100, default 100 */
#define VFS5011_LED_RED_CONF        VFS5011_LED_CONF_DIR "led_red.conf"        /* 0/1, default 1 */
#define VFS5011_LED_RED_PORT_CONF   VFS5011_LED_CONF_DIR "led_red_port.conf"   /* 0..3, default 2 */
#define VFS5011_LED_IDLE_STEADY_CONF VFS5011_LED_CONF_DIR "led_idle_steady.conf" /* 0/1, default 0 (driver blink) */

/* Reads one integer from a conf file. Returns def when the file is missing,
 * unreadable or out of [lo, hi]. */
static inline int vfs5011_led_read_conf(const char *path, int def, int lo, int hi) {
    FILE *f = fopen(path, "r");
    if (!f) return def;
    int v = def;
    int got = fscanf(f, "%d", &v);
    fclose(f);
    if (got != 1 || v < lo || v > hi) return def;
    return v;
}

static inline int vfs5011_led_load_brightness(void) {
    return vfs5011_led_read_conf(VFS5011_LED_BRIGHTNESS_CONF, 100, 0, 100);
}
static inline int vfs5011_led_load_red_enabled(void) {
    return vfs5011_led_read_conf(VFS5011_LED_RED_CONF, 1, 0, 1);
}
static inline int vfs5011_led_load_idle_steady(void) {
    return vfs5011_led_read_conf(VFS5011_LED_IDLE_STEADY_CONF, 0, 0, 1);
}
static inline int vfs5011_led_load_red_port(void) {
    return vfs5011_led_read_conf(VFS5011_LED_RED_PORT_CONF, VFS5011_LED_PORT_ATTENTION, 0, 3);
}

/* All LEDs off: every entry flag 0, duration field 1 (what the driver sends). */
static inline void vfs5011_led_build_off(uint8_t out[VFS5011_LED_PKT_LEN]) {
    memset(out, 0, VFS5011_LED_PKT_LEN);
    out[0] = VFS5011_LED_OPCODE;
    out[113] = 1;
}

/* Builds a blink program for several LED ports at once (port_mask bit n = port n,
 * 1..15), mirroring the driver (non-ramp path), so two LEDs can share one pattern.
 *   hi_pct      brightness while on, 0..100 (the driver treats 0 as full)
 *   lo_pct      brightness while off, 0..100
 *   blink_ms    full on+off period in ms (driver minimum is 32)
 *   duty_pct    share of the period the LED is on, 0..100 (100 = steady on)
 *   duration_s  how long the program runs, 0..255 seconds
 * Returns 0 on success, -1 on bad arguments. duration 0 or duty 0 gives the
 * all-off packet, as in the driver. */
static inline int vfs5011_led_build_mask(uint8_t out[VFS5011_LED_PKT_LEN], int port_mask, int hi_pct,
                                         int lo_pct, int blink_ms, int duty_pct, int duration_s) {
    if (port_mask < 1 || port_mask > 15 || hi_pct < 0 || hi_pct > 100 || lo_pct < 0 || lo_pct > 100 ||
        blink_ms < 32 || duty_pct < 0 || duty_pct > 100 || duration_s < 0 || duration_s > 255)
        return -1;
    if (duration_s == 0 || duty_pct == 0) { vfs5011_led_build_off(out); return 0; }

    unsigned on_ms, off_ms;
    if (duty_pct == 100) { on_ms = 500; off_ms = 0; }
    else { on_ms = (unsigned)blink_ms * (unsigned)duty_pct / 100u; off_ms = (unsigned)blink_ms - on_ms; }

    unsigned hi = (unsigned)hi_pct * 255u / 100u;
    unsigned lo = (unsigned)lo_pct * 255u / 100u;
    if (hi == 0) { hi = 0xff; lo = 0; } /* driver quirk: zero high level means full, low forced to 0 */

    memset(out, 0, VFS5011_LED_PKT_LEN);
    out[0] = VFS5011_LED_OPCODE;
    for (int port = 0; port < 4; port++) {
        if (!(port_mask & (1 << port))) continue;
        uint8_t *on  = out + 1 + 7 * (port * 4 + 1);
        uint8_t *off = out + 1 + 7 * (port * 4 + 3);
        on[0] = 1;  on[1] = (uint8_t)hi;  on[4] = (uint8_t)hi;
        on[5] = (uint8_t)(on_ms & 0xff);  on[6] = (uint8_t)(on_ms >> 8);
        off[0] = 1; off[1] = (uint8_t)lo; off[4] = (uint8_t)lo;
        off[5] = (uint8_t)(off_ms & 0xff); off[6] = (uint8_t)(off_ms >> 8);
    }
    unsigned dur_ms = (unsigned)duration_s * 1000u;
    out[113] = (uint8_t)(dur_ms & 0xff);
    out[114] = (uint8_t)((dur_ms >> 8) & 0xff);
    out[115] = (uint8_t)((dur_ms >> 16) & 0xff);
    out[116] = (uint8_t)((dur_ms >> 24) & 0xff);
    return 0;
}

/* Single-port form of the above. port 0..3. */
static inline int vfs5011_led_build(uint8_t out[VFS5011_LED_PKT_LEN], int port, int hi_pct,
                                    int lo_pct, int blink_ms, int duty_pct, int duration_s) {
    if (port < 0 || port > 3) return -1;
    return vfs5011_led_build_mask(out, 1 << port, hi_pct, lo_pct, blink_ms, duty_pct, duration_s);
}

/* Adds a one-shot pulse on one port of an existing program, leaving the other
 * ports as they are: the port is on at hi_pct (1..100) for on_ms, then off for
 * off_ms (both 32..65535). Used to flash red over the waiting blink. Returns 0
 * on success, -1 on bad arguments. */
static inline int vfs5011_led_overlay_pulse(uint8_t pkt[VFS5011_LED_PKT_LEN], int port, int hi_pct,
                                            unsigned on_ms, unsigned off_ms) {
    if (port < 0 || port > 3 || hi_pct < 1 || hi_pct > 100 ||
        on_ms < 32 || on_ms > 0xffff || off_ms < 32 || off_ms > 0xffff)
        return -1;
    unsigned hi = (unsigned)hi_pct * 255u / 100u;
    if (hi == 0) hi = 1;
    uint8_t *on  = pkt + 1 + 7 * (port * 4 + 1);
    uint8_t *off = pkt + 1 + 7 * (port * 4 + 3);
    on[0] = 1;  on[1] = (uint8_t)hi;  on[4] = (uint8_t)hi;
    on[5] = (uint8_t)(on_ms & 0xff);  on[6] = (uint8_t)(on_ms >> 8);
    off[0] = 1; off[1] = 0; off[4] = 0;
    off[5] = (uint8_t)(off_ms & 0xff); off[6] = (uint8_t)(off_ms >> 8);
    return 0;
}

/* True when data is an LED command of the right size. */
static inline int vfs5011_led_is_packet(const unsigned char *data, int size) {
    return data != NULL && size == VFS5011_LED_PKT_LEN && data[0] == VFS5011_LED_OPCODE;
}

/* Scales the "on" level of an existing program to pct (0..100). 100 leaves it
 * untouched, 0 turns the LED off. Entries whose level is already 0 (the off
 * slots) stay 0, and any lit level stays at least 1 so a low setting does not
 * silently become "off". */
static inline void vfs5011_led_apply_brightness(uint8_t pkt[VFS5011_LED_PKT_LEN], int pct) {
    if (pct >= 100) return;
    if (pct <= 0) { vfs5011_led_build_off(pkt); return; }
    for (int i = 0; i < 16; i++) {
        uint8_t *e = pkt + 1 + 7 * i;
        if (e[0] == 1 && e[1] != 0) {
            unsigned v = (unsigned)e[1] * (unsigned)pct / 100u;
            if (v == 0) v = 1;
            e[1] = e[4] = (uint8_t)v;
        }
    }
}

/* Core of vfs5011_led_prepare_send with the settings passed in (testable without
 * conf files). When data is an LED command, fills scratch and returns it: with
 * idle_steady the waiting light becomes a steady light on port 0 at brightness_pct
 * (the driver's own blink is lit only half the time and looks dim), otherwise the
 * driver packet with its level scaled to brightness_pct. Anything else is
 * returned untouched. */
static inline const unsigned char *vfs5011_led_prepare_send_ex(const unsigned char *data, int size,
                                                               uint8_t scratch[VFS5011_LED_PKT_LEN],
                                                               int brightness_pct, int idle_steady) {
    if (!vfs5011_led_is_packet(data, size)) return data;
    if (idle_steady) {
        if (brightness_pct <= 0) vfs5011_led_build_off(scratch);
        else vfs5011_led_build(scratch, VFS5011_LED_PORT_NORMAL, brightness_pct, 0, 500, 100, 180);
        return scratch;
    }
    memcpy(scratch, data, VFS5011_LED_PKT_LEN);
    vfs5011_led_apply_brightness(scratch, brightness_pct);
    return scratch;
}

/* For the sequence runners: same as the _ex form, with the saved settings. */
static inline const unsigned char *vfs5011_led_prepare_send(const unsigned char *data, int size,
                                                            uint8_t scratch[VFS5011_LED_PKT_LEN]) {
    if (!vfs5011_led_is_packet(data, size)) return data;
    return vfs5011_led_prepare_send_ex(data, size, scratch, vfs5011_led_load_brightness(),
                                       vfs5011_led_load_idle_steady());
}

#endif /* VFS5011_LED_H */
