#ifndef __EGIS_PROBE_H
#define __EGIS_PROBE_H

/*
 * egis_probe.h
 *
 * First base of the EGIS match-on-chip backend (ETU905A80-E, 1c7a:05a1,
 * GitHub issue #15). This is a READ-ONLY probe: it finds the sensor, prints
 * its USB descriptors, claims interface 0, sends the same IN-direction
 * start-up reads the Windows driver sends, and then sends only the four
 * query commands that the open EGIS MoC PoC scripts (0586/0587) use to read
 * the sensor's state. It never enrolls, never verifies, never wipes and
 * never writes anything to the sensor. Its job is to tell us whether this
 * sensor answers the EGIS "EGIS"/"SIGE" framing the way libfprint's egismoc
 * driver says it does, or whether it wants something else first (for
 * example an SDCP handshake on newer firmware).
 *
 * Run through the client:  sudo hack-touchid --probe-egis --debug
 *
 * Licensed under the GNU LGPL 2.1 or later, see egis_probe.c.
 */

#define EGIS_VID       0x1c7a
#define EGIS_PID_05A1  0x05a1

/* Returns 0 if the sensor answered a query in the expected framing, 1 if it
 * was found but did not answer (or could not be opened or claimed), 2 if it
 * was not found at all, 3 on a libusb initialisation failure. `debug` follows
 * --debug / --debug-full: 0 = normal, 1 = libusb transfer log on stderr,
 * 2 = never truncate dumps. */
int egis_probe_run(int debug);

#endif /* __EGIS_PROBE_H */
