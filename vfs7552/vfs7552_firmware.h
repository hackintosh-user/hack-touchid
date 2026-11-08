#ifndef __VFS7552_FIRMWARE_H
#define __VFS7552_FIRMWARE_H

/*
 * vfs7552_firmware.h
 *
 * On-demand firmware fetch for the Synaptics VFS7552 (138a:0092). Same
 * idea as metallica_mis_firmware.h, with one real difference: HP ships
 * this sensor's firmware inside a SoftPaq that is a 7-Zip
 * self-extracting archive, not an Inno Setup installer, so it is
 * unpacked with 7-Zip (brew install sevenzip) and NOT with innoextract.
 * python-validity's own 0092 table carries a comment saying innoextract
 * does not like these archives.
 *
 * Source: HP SoftPaq SP135737, "Synaptics VFS7552 WBF Touch Fingerprint
 * Driver" 5.2.5030.26, which lists USB\VID_138A&PID_0092 in its INF.
 *
 * LICENSING NOTE (same rule as metallica_mis_firmware.h): the firmware is
 * never bundled or hosted by this project. It is downloaded from HP's own
 * ftp.hp.com, checked against pinned SHA-256 hashes, and extracted on the
 * user's own machine. Do not add any URL here that isn't HP's.
 */

#include <stdbool.h>

/* Exact firmware filename this sensor needs (python-validity's
 * firmware_tables.py FIRMWARE_NAMES entry for DEV_92). */
#define VFS7552_FW_FILENAME "6_07f_hp_8x8pb.xpfwext"

/* Fills out_path with ~/.hack-touchid/firmware/<name>. Returns 0 on
 * success, -1 if $HOME could not be resolved. */
int vfs7552_firmware_path(char *out_path, int out_path_size);

/* True when the file exists at that path with the expected size. */
bool vfs7552_firmware_is_present(void);

/* Downloads SP135737, verifies it, extracts the firmware with 7-Zip and
 * installs it. Prints its own progress and errors. Returns true on success. */
bool vfs7552_firmware_fetch(void);

#endif /* __VFS7552_FIRMWARE_H */
