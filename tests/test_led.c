/*
 * test_led.c -- unit tests for vfs5011/vfs5011_led.h.
 * No USB, no sensor. The golden vector is the 117-byte LED command that the
 * project already replays from the captured Windows traffic
 * (vfs5011_init_17 / vfs5011_prepare_03).
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

/* vfs5011_proto.h uses libusb endpoint macros; give it plain values. */
#define LIBUSB_ENDPOINT_OUT 0x00
#define LIBUSB_ENDPOINT_IN  0x80
#include "vfs5011_proto.h"
#include "vfs5011_led.h"

static int g_fail = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("  FAIL: %s\n", msg); g_fail++; } else { printf("  ok:   %s\n", msg); } } while (0)

int main(void) {
    uint8_t pkt[VFS5011_LED_PKT_LEN];

    printf("golden vector\n");
    CHECK(sizeof(vfs5011_init_17) == VFS5011_LED_PKT_LEN, "init_17 is 117 bytes");
    CHECK(vfs5011_led_is_packet(vfs5011_init_17, sizeof(vfs5011_init_17)), "init_17 recognised as LED packet");
    CHECK(vfs5011_led_is_packet(vfs5011_prepare_03, sizeof(vfs5011_prepare_03)), "prepare_03 recognised as LED packet");
    CHECK(!vfs5011_led_is_packet(vfs5011_init_18, sizeof(vfs5011_init_18)), "init_18 not an LED packet");
    CHECK(vfs5011_led_build(pkt, 0, 100, 0, 1000, 50, 180) == 0, "build ready pattern");
    CHECK(memcmp(pkt, vfs5011_init_17, VFS5011_LED_PKT_LEN) == 0, "ready pattern == captured init_17 byte for byte");
    CHECK(memcmp(pkt, vfs5011_prepare_03, VFS5011_LED_PKT_LEN) == 0, "ready pattern == captured prepare_03 byte for byte");

    printf("off packet\n");
    vfs5011_led_build_off(pkt);
    CHECK(pkt[0] == 0x2a && pkt[113] == 1 && pkt[114] == 0 && pkt[115] == 0 && pkt[116] == 0, "opcode and duration 1");
    int zero = 1; for (int i = 1; i < 113; i++) if (pkt[i]) zero = 0;
    CHECK(zero, "all entries zero");
    CHECK(vfs5011_led_build(pkt, 1, 100, 0, 1000, 50, 0) == 0 && pkt[113] == 1 && pkt[8] == 0, "duration 0 gives all-off");
    CHECK(vfs5011_led_build(pkt, 1, 100, 0, 1000, 0, 5) == 0 && pkt[113] == 1, "duty 0 gives all-off");

    printf("red (attention) port\n");
    CHECK(vfs5011_led_build(pkt, 2, 100, 0, 400, 100, 2) == 0, "build steady red 2 s");
    uint8_t *on = pkt + 1 + 7 * (2 * 4 + 1), *off = pkt + 1 + 7 * (2 * 4 + 3);
    CHECK(on[0] == 1 && on[1] == 0xff && on[4] == 0xff && (on[5] | on[6] << 8) == 500, "port 2 on slot");
    CHECK(off[0] == 1 && off[1] == 0 && (off[5] | off[6] << 8) == 0, "port 2 off slot (duty 100 -> 0 ms off)");
    CHECK(pkt[1 + 7 * 1] == 0, "port 0 untouched");
    CHECK(pkt[113] == (2000 & 0xff) && pkt[114] == (2000 >> 8), "duration 2000 ms");

    printf("several ports\n");
    CHECK(vfs5011_led_build_mask(pkt, 0x03, 100, 0, 1000, 50, 10) == 0, "build ports 0 and 1");
    CHECK(pkt[1 + 7 * 1] == 1 && pkt[1 + 7 * 5] == 1 && pkt[1 + 7 * 9] == 0, "ports 0 and 1 set, port 2 clear");
    uint8_t one[VFS5011_LED_PKT_LEN];
    vfs5011_led_build(one, 0, 100, 0, 1000, 50, 10);
    CHECK(memcmp(pkt + 1, one + 1, 28) == 0, "port 0 entries same as single-port build");
    CHECK(vfs5011_led_build_mask(pkt, 0, 100, 0, 1000, 50, 10) == -1, "empty mask rejected");

    printf("brightness\n");
    memcpy(pkt, vfs5011_init_17, VFS5011_LED_PKT_LEN);
    vfs5011_led_apply_brightness(pkt, 50);
    CHECK(pkt[9] == 127 && pkt[12] == 127, "50% -> 127 on both level bytes");
    CHECK(pkt[16] == 0 && pkt[19] == 0, "off slot stays 0");
    CHECK(memcmp(pkt + 113, vfs5011_init_17 + 113, 4) == 0, "duration unchanged");
    memcpy(pkt, vfs5011_init_17, VFS5011_LED_PKT_LEN);
    vfs5011_led_apply_brightness(pkt, 1);
    CHECK(pkt[9] == 2, "1% -> 2 (255*1/100)");
    memcpy(pkt, vfs5011_init_17, VFS5011_LED_PKT_LEN);
    vfs5011_led_apply_brightness(pkt, 100);
    CHECK(memcmp(pkt, vfs5011_init_17, VFS5011_LED_PKT_LEN) == 0, "100% leaves packet untouched");
    memcpy(pkt, vfs5011_init_17, VFS5011_LED_PKT_LEN);
    vfs5011_led_apply_brightness(pkt, 0);
    CHECK(pkt[113] == 1 && pkt[8] == 0, "0% -> all-off");

    printf("argument checks\n");
    CHECK(vfs5011_led_build(pkt, 4, 100, 0, 1000, 50, 1) == -1, "port 4 rejected");
    CHECK(vfs5011_led_build(pkt, 0, 101, 0, 1000, 50, 1) == -1, "brightness 101 rejected");
    CHECK(vfs5011_led_build(pkt, 0, 100, 0, 10, 50, 1) == -1, "blink under 32 ms rejected");
    CHECK(vfs5011_led_build(pkt, 0, 100, 0, 1000, 50, 256) == -1, "duration 256 rejected");

    printf("red over the ready blink\n");
    uint8_t base[VFS5011_LED_PKT_LEN];
    memcpy(base, vfs5011_prepare_03, VFS5011_LED_PKT_LEN);
    memcpy(pkt, base, VFS5011_LED_PKT_LEN);
    CHECK(vfs5011_led_overlay_pulse(pkt, 2, 100, 1600, 60000) == 0, "overlay on port 2");
    CHECK(memcmp(pkt, base, 1 + 7 * 8) == 0, "ports 0 and 1 unchanged");
    CHECK(memcmp(pkt + 1 + 7 * 12, base + 1 + 7 * 12, 113 - 1 - 7 * 12) == 0, "port 3 and duration unchanged");
    {
        const uint8_t *on = pkt + 1 + 7 * 9, *off = pkt + 1 + 7 * 11;
        CHECK(on[0] == 1 && on[1] == 0xff && on[4] == 0xff && (on[5] | on[6] << 8) == 1600, "port 2 on slot: full level, 1600 ms");
        CHECK(off[0] == 1 && off[1] == 0 && (off[5] | off[6] << 8) == 60000, "port 2 off slot: level 0, 60000 ms");
    }
    CHECK(vfs5011_led_overlay_pulse(pkt, 4, 100, 1600, 60000) == -1, "bad port rejected");
    CHECK(vfs5011_led_overlay_pulse(pkt, 2, 0, 1600, 60000) == -1, "level 0 rejected");
    CHECK(vfs5011_led_overlay_pulse(pkt, 2, 100, 1600, 70000) == -1, "off time over 65535 rejected");

    printf("steady idle light\n");
    {
        uint8_t sc[VFS5011_LED_PKT_LEN], expect[VFS5011_LED_PKT_LEN];
        const unsigned char *r = vfs5011_led_prepare_send_ex(vfs5011_prepare_03, sizeof(vfs5011_prepare_03), sc, 100, 0);
        CHECK(r == sc && memcmp(sc, vfs5011_prepare_03, VFS5011_LED_PKT_LEN) == 0, "blink mode at 100% is the driver packet");
        r = vfs5011_led_prepare_send_ex(vfs5011_prepare_03, sizeof(vfs5011_prepare_03), sc, 100, 1);
        vfs5011_led_build(expect, 0, 100, 0, 500, 100, 180);
        CHECK(r == sc && memcmp(sc, expect, VFS5011_LED_PKT_LEN) == 0, "steady mode: port 0 steady, 180 s");
        CHECK(sc[1 + 7 * 1 + 5] == 0xf4 && sc[1 + 7 * 1 + 6] == 0x01 && sc[1 + 7 * 3 + 5] == 0 && sc[1 + 7 * 3 + 6] == 0, "steady mode: 500 ms on, 0 ms off");
        vfs5011_led_prepare_send_ex(vfs5011_init_17, sizeof(vfs5011_init_17), sc, 40, 1);
        CHECK(sc[1 + 7 * 1 + 1] == (uint8_t)(40 * 255 / 100), "steady mode scales to the brightness setting");
        vfs5011_led_prepare_send_ex(vfs5011_init_17, sizeof(vfs5011_init_17), sc, 0, 1);
        vfs5011_led_build_off(expect);
        CHECK(memcmp(sc, expect, VFS5011_LED_PKT_LEN) == 0, "steady mode at 0% is all off");
        r = vfs5011_led_prepare_send_ex(vfs5011_init_18, sizeof(vfs5011_init_18), sc, 100, 1);
        CHECK(r == vfs5011_init_18, "non LED packets pass through untouched");
    }

    printf("\n%s (%d failure%s)\n", g_fail ? "LED TESTS FAILED" : "LED TESTS PASSED", g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
