#ifndef __VFS7552_PROBE_H
#define __VFS7552_PROBE_H

/*
 * vfs7552_probe.h
 *
 * First base of the Synaptics VFS7552 (138a:0092) backend. This is a
 * READ-ONLY probe: it finds the sensor, prints its USB descriptors,
 * claims interface 0 and sends only harmless read-only query commands
 * python-validity's Usb.send_init() starts with (RomInfo 0x01, 0x19 and
 * the fwext info query 0x43 0x02). It never sends the init blob, never
 * pairs, never touches the flash. Its job is to tell us whether this
 * sensor answers the Prometheus-family command set the way
 * python-validity's device/0092 branch says it does.
 *
 * Run through the client:  sudo hack-touchid --probe-7552 --debug
 */

#define VFS7552_VID 0x138a
#define VFS7552_PID 0x0092

/* Returns 0 if the sensor answered RomInfo, 1 if it was found but did not
 * answer (or could not be claimed), 2 if it was not found at all, 3 on a
 * libusb initialisation failure. `debug` follows --debug / --debug-full:
 * 0 = normal, 1 = libusb transfer log on stderr, 2 = never truncate dumps. */
int vfs7552_probe_run(int debug);

#endif /* __VFS7552_PROBE_H */
