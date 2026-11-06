#ifndef MMIS_SENSOR_PROFILE_H
#define MMIS_SENSOR_PROFILE_H

/*
 * mmis_sensor_profile.h
 *
 * Per sensor type constants for the Metallica MIS / Prometheus family, so the
 * calibrate and capture code stops hardcoding the type 0x199 values.
 *   0x199: 06cb:009a (X1C6), line_update_type_1, 3 calibration iterations
 *   0xdb : 138a:0097 (X1C5), line_update_type_2, no calibration loop
 * The type comes from factory bits subtag 8 (metallica_mis_get_sensor_type()).
 * Header-only on purpose: no new .c file for the build scripts and CI lists.
 */

#include <stddef.h>
#include <stdint.h>
#include "metallica_type0199_tables.h"
#include "metallica_type00db_tables.h"
#include "mmis_timeslot.h"

typedef struct {
    uint32_t sensor_type;
    const char *name;
    uint16_t bytes_per_line;
    uint8_t repeat_multiplier;
    size_t lines_per_calibration_data;
    size_t line_width;
    uint8_t key_calibration_line;
    uint8_t calibration_frames;
    int calibration_iterations;
    const uint8_t *calib_blob;
    size_t calib_blob_len;
    void (*build_prog)(uint8_t *out, size_t *out_len);
    size_t prog_max_len;
    int line_update_type;   /* 1 or 2 */
} mmis_sensor_profile_t;

/* Largest capture program of any profile, for stack buffers. */
#define MMIS_PROG_MAX_LEN \
    (METALLICA_TYPE00DB_PROG_MAX_LEN > METALLICA_TYPE0199_PROG_MAX_LEN \
         ? METALLICA_TYPE00DB_PROG_MAX_LEN : METALLICA_TYPE0199_PROG_MAX_LEN)

static inline const mmis_sensor_profile_t *mmis_sensor_profile_get(uint32_t sensor_type) {
    static const mmis_sensor_profile_t p0199 = {
        0x199, "type 0x199 (06cb:009a)",
        METALLICA_TYPE0199_BYTES_PER_LINE, METALLICA_TYPE0199_REPEAT_MULTIPLIER,
        METALLICA_TYPE0199_LINES_PER_CALIBRATION_DATA, METALLICA_TYPE0199_LINE_WIDTH,
        METALLICA_TYPE0199_KEY_CALIBRATION_LINE, METALLICA_TYPE0199_CALIBRATION_FRAMES,
        METALLICA_TYPE0199_CALIBRATION_ITERATIONS,
        METALLICA_TYPE0199_CALIB_BLOB, sizeof(METALLICA_TYPE0199_CALIB_BLOB),
        metallica_type0199_build_prog, METALLICA_TYPE0199_PROG_MAX_LEN, 1,
    };
    static const mmis_sensor_profile_t p00db = {
        0xdb, "type 0xdb (138a:0097)",
        METALLICA_TYPE00DB_BYTES_PER_LINE, METALLICA_TYPE00DB_REPEAT_MULTIPLIER,
        METALLICA_TYPE00DB_LINES_PER_CALIBRATION_DATA, METALLICA_TYPE00DB_LINE_WIDTH,
        METALLICA_TYPE00DB_KEY_CALIBRATION_LINE, METALLICA_TYPE00DB_CALIBRATION_FRAMES,
        METALLICA_TYPE00DB_CALIBRATION_ITERATIONS,
        METALLICA_TYPE00DB_CALIB_BLOB, sizeof(METALLICA_TYPE00DB_CALIB_BLOB),
        metallica_type00db_build_prog, METALLICA_TYPE00DB_PROG_MAX_LEN, 2,
    };
    if (sensor_type == 0x199) return &p0199;
    if (sensor_type == 0xdb) return &p00db;
    return NULL;
}

/* build_cmd_02() for either profile. fcd/fcd_len (factory bits subtag 7) is
 * only used by line_update_type_2 and may be NULL/0 for type 1. */
static inline size_t mmis_profile_build_cmd_02(const mmis_sensor_profile_t *p, mmis_capture_mode_t mode,
                                               const uint8_t *prog, size_t prog_len, size_t lines_per_frame,
                                               const uint8_t *fcv, size_t fcv_len,
                                               const uint8_t *fcd, size_t fcd_len,
                                               const uint8_t *calib, size_t calib_len,
                                               uint8_t *out, size_t out_max,
                                               uint8_t *scratch, size_t scratch_len) {
    if (p->line_update_type == 2) {
        return mmis_build_cmd_02_type2(mode, prog, prog_len,
                                       p->bytes_per_line, p->calibration_frames, lines_per_frame,
                                       p->repeat_multiplier, p->key_calibration_line,
                                       fcv, fcv_len, fcd, fcd_len,
                                       p->calib_blob, p->calib_blob_len,
                                       out, out_max, scratch, scratch_len);
    }
    return mmis_build_cmd_02(mode, prog, prog_len,
                             p->bytes_per_line, p->calibration_frames, lines_per_frame,
                             p->repeat_multiplier, p->key_calibration_line,
                             fcv, fcv_len, calib, calib_len,
                             p->lines_per_calibration_data, p->line_width,
                             p->calib_blob, p->calib_blob_len,
                             out, out_max, scratch, scratch_len);
}

/* sensor.py average() for either profile (interleave > 1 or == 1). */
static inline bool mmis_profile_average(const mmis_sensor_profile_t *p,
                                        const uint8_t *raw, size_t raw_len, size_t lines_per_frame,
                                        uint8_t *out, size_t out_max, size_t *out_len) {
    if (lines_per_frame / p->lines_per_calibration_data > 1) {
        return mmis_average(raw, raw_len, lines_per_frame, p->bytes_per_line,
                            p->lines_per_calibration_data, out, out_max, out_len);
    }
    return mmis_average_flat(raw, raw_len, lines_per_frame, p->bytes_per_line,
                             p->calibration_frames, out, out_max, out_len);
}

#endif /* MMIS_SENSOR_PROFILE_H */
