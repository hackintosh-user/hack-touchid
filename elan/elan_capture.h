/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef HT_ELAN_CAPTURE_H
#define HT_ELAN_CAPTURE_H
#include <stddef.h>
#include <stdint.h>
#define ELAN_VID 0x04f3
#define ELAN_PID 0x0c6e
#define ELAN_WIDTH 150
#define ELAN_HEIGHT 52
#define ELAN_PIXELS (ELAN_WIDTH * ELAN_HEIGHT)
typedef struct elan_capture_device elan_capture_device;
/* Returns a libusb error code. Does not detach drivers or change configuration. */
int elan_capture_open(elan_capture_device **out);
/* Row-major little-endian sensor samples decoded to host uint16_t values.
 * On error, no frame is returned. timeout_ms bounds the entire read sequence. */
int elan_capture_frame(elan_capture_device *dev, uint16_t *out,
                       size_t pixels, unsigned timeout_ms);
void elan_capture_close(elan_capture_device *dev);
#endif
