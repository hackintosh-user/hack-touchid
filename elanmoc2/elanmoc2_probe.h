#ifndef __ELANMOC2_PROBE_H
#define __ELANMOC2_PROBE_H

/*
 * elanmoc2_probe.h
 *
 * First base of the ELAN match-on-chip backend for the "elanmoc2" family
 * (ELAN WBF / ELAN:ARM-M4, 04f3:0c4c, GitHub issue #16). This is a READ-ONLY
 * probe: it finds the sensor, prints its USB descriptors, claims interface 0
 * and sends exactly two short query commands (firmware version and the number
 * of enrolled fingers) in the framing that the Linux elanmoc2 driver uses.
 * It never enrolls, never verifies, never deletes, never wipes, never resets
 * the device and never reads the identifiers of enrolled fingers. Its job is
 * to tell us whether this sensor, on macOS, answers the elanmoc2 protocol the
 * way it does on Linux, before any real backend is written.
 *
 * Run through the client:  sudo hack-touchid --probe-0c4c --debug
 *
 * Licensed under the GNU LGPL 2.1 or later, see elanmoc2_probe.c.
 */

#define ELANMOC2_VID       0x04f3
#define ELANMOC2_PID_0C4C  0x0c4c

/* Returns 0 if the sensor answered at least one query in the expected
 * framing, 1 if it was found but did not answer (or could not be opened or
 * claimed), 2 if it was not found at all, 3 on a libusb initialisation
 * failure. `debug` follows --debug / --debug-full: 0 = normal, 1 = libusb
 * transfer log on stderr, 2 = never truncate dumps. */
int elanmoc2_probe_run(int debug);

#endif /* __ELANMOC2_PROBE_H */
