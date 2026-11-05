/* Checks the type 0xdb capture command builder against golden vectors generated from
 * python-validity master (sensor.py build_cmd_02 + average).
 * Build: gcc -I.. mmis_type2_test.c ../mmis_timeslot.c -o /tmp/mmis_type2_test && /tmp/mmis_type2_test */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mmis_timeslot.h"
#include "metallica_type00db_tables.h"
#include "mmis_type2_golden.h"

static void unhex(const char *h, uint8_t *out, size_t *len) {
    size_t n = strlen(h) / 2;
    for (size_t i = 0; i < n; i++) { unsigned v; sscanf(h + 2 * i, "%2x", &v); out[i] = (uint8_t)v; }
    *len = n;
}

static int check(const char *name, mmis_capture_mode_t mode, const char *golden,
                 const uint8_t *prog, size_t plen, size_t lpf,
                 const uint8_t *fcv, const uint8_t *fcd) {
    static uint8_t out[8192], scratch[65536], want[8192];
    size_t wlen;
    unhex(golden, want, &wlen);
    size_t n = mmis_build_cmd_02_type2(mode, prog, plen, METALLICA_TYPE00DB_BYTES_PER_LINE,
                                       METALLICA_TYPE00DB_CALIBRATION_FRAMES, lpf,
                                       METALLICA_TYPE00DB_REPEAT_MULTIPLIER,
                                       METALLICA_TYPE00DB_KEY_CALIBRATION_LINE,
                                       fcv, 144, fcd, 144,
                                       METALLICA_TYPE00DB_CALIB_BLOB, sizeof(METALLICA_TYPE00DB_CALIB_BLOB),
                                       out, sizeof(out), scratch, sizeof(scratch));
    if (n != wlen || memcmp(out, want, wlen) != 0) {
        size_t i = 0;
        while (i < n && i < wlen && out[i] == want[i]) i++;
        printf("FAIL %s: got %zu bytes, want %zu, first diff at %zu\n", name, n, wlen, i);
        return 1;
    }
    printf("ok   %s (%zu bytes)\n", name, n);
    return 0;
}

int main(void) {
    uint8_t prog[METALLICA_TYPE00DB_PROG_MAX_LEN];
    size_t plen, lpf;
    metallica_type00db_build_prog(prog, &plen);
    if (!mmis_get_lines_per_frame(prog, plen, METALLICA_TYPE00DB_REPEAT_MULTIPLIER, &lpf) || lpf != 144) {
        printf("FAIL lines_per_frame\n");
        return 1;
    }
    uint8_t fcv[144], fcd[144];
    for (int i = 0; i < 144; i++) { fcv[i] = (uint8_t)(i * 5 + 3); fcd[i] = (uint8_t)(i * 11 + 1); }

    int bad = 0;
    bad += check("CALIBRATE", MMIS_CAPTURE_CALIBRATE, GOLDEN_CALIBRATE, prog, plen, lpf, fcv, fcd);
    bad += check("IDENTIFY", MMIS_CAPTURE_IDENTIFY, GOLDEN_IDENTIFY, prog, plen, lpf, fcv, fcd);
    bad += check("ENROLL", MMIS_CAPTURE_ENROLL, GOLDEN_ENROLL, prog, plen, lpf, fcv, fcd);

    size_t raw_len = (size_t)6 * 144 * 0x98 + 0x98;
    uint8_t *raw = malloc(raw_len);
    for (size_t i = 0; i < raw_len; i++) raw[i] = (uint8_t)((i * 7) + (i / 251));
    static uint8_t avg[32768];
    size_t alen = 0;
    if (!mmis_average_flat(raw, raw_len, 144, 0x98, 6, avg, sizeof(avg), &alen) || alen != GOLDEN_AVG_LEN) {
        printf("FAIL average_flat length\n");
        bad++;
    } else {
        uint32_t h = 0x811c9dc5u;
        for (size_t i = 0; i < alen; i++) { h ^= avg[i]; h *= 0x01000193u; }
        if (h != GOLDEN_AVG_FNV) { printf("FAIL average_flat hash %08x\n", h); bad++; }
        else printf("ok   average_flat (%zu bytes)\n", alen);
    }
    free(raw);
    return bad ? 1 : 0;
}
