# ELAN 04f3:0c6e capture test

This first integration reads frames only. It does not enroll or match fingers,
read a password, inject keys, modify Windows data, or install an auth daemon.
The sensor registry keeps `backend_available = 0`, so HTID must continue to
refuse Enroll, Verify and Deploy for this identity.

## Hardware and protocol

Tested in the standalone implementation on an ASUS Vivobook Pro 14X OLED
M7400QC, Ryzen 7 5800H, macOS Sequoia 15.8.1 x86_64. ELAN USB VID:PID is
04f3:0c6e; raw images are 150 by 52 samples. This port rejects other dimensions.
The USB device revision is not assumed to identify the firmware version.

- Bulk OUT 0x01: commands; bulk IN 0x83: replies; bulk IN 0x82: image data.
- `00 0c` queries dimensions (including the odd/odd zero-based size quirk).
- `00 0b` stops capture before reading and during best-effort cleanup.
- Before each `00 09` image request, `40 13` plus a one-byte reply arms a fresh
  exposure. The status byte is not treated as a reliable live finger signal.
- Raw little-endian column-major samples are decoded into row-major uint16_t.
- No LED/pre_scan handshake, firmware upload, pairing, storage wipe, kernel
  driver detach or configuration reset is used.
- Failed and short transfers fail the frame. No partial/stale frame is returned.

Protocol references:

- [filip-rs/libfprint elanpress](https://github.com/filip-rs/libfprint/tree/fa2f98fb8cb56e72d9e316cd86a987456b137893/libfprint/drivers),
  copyright 2026 Filip Spanne, LGPL-2.1-or-later.
- [casimir-engineering/elan-fp-fix](https://github.com/casimir-engineering/elan-fp-fix/tree/a3d6e99/driver),
  including its status-before-image ordering and warning about frozen imaging.
- Working standalone port: [elan-fingerprint-macos](https://github.com/s17765152890-gif/elan-fingerprint-macos).

All source in this directory is LGPL-2.1-or-later; see LICENSE. Shared
registry and build entry points retain the host project's BSD license. This
capture executable does not link a new LGPL matcher into the BSD client.

## Build

With libusb provided by Homebrew or MacPorts:

```sh
./prep_and_build.sh --sensor elan --no-pane
```

With an existing custom libusb prefix containing include/libusb-1.0/libusb.h
and lib/libusb-1.0.dylib (or a static library):

```sh
ELAN_LIBUSB_PREFIX=/path/to/prefix ./elan/build.sh
```

The sensor-driven build adds ./elan_capture_test alongside the existing client.
The direct capture-only build entry point is ./elan/build.sh; it does not build
or install any client, preference pane, matching engine or authentication agent.
The client dispatches [E] to the standalone test for this identity. Alternatively,
./hack-touchid --capture-elan starts the test before any sudo, pane or keychain
setup. Generic VFS5011 capture/reset paths are refused for ELAN.

## Device test

Stop other software using the same sensor before testing. Use your normal
account first; a libusb access error must be investigated rather than bypassed
by detaching the device's kernel driver.

```sh
./elan_capture_test --probe
./elan_capture_test --frames 400
```

First leave the pad clear, then lightly touch and release it. Do not press the
power button. Watch the frame means change; this proves sensor response, not
fingerprint matching or an authentication accuracy claim. Ctrl+C cancels with
bounded USB waits, followed by cleanup. No biometric image is saved by default.

For a local visual inspection only, --output frame.pgm writes the final frame
as a 16-bit big-endian PGM with mode 0600. The file is created exclusively, so
existing files and symlinks are never overwritten. It can contain biometric
data. Do not attach it to a public issue or commit it. The repository ignores
PGM files as an additional guard.

## Protocol regression tests

```sh
./elan/tests/run.sh
# After building the client:
./elan/tests/test_client_dispatch.sh
```

These use a fake USB endpoint; no real sensor or credentials are accessed.
Real-hardware capture still needs a separate run on the sensor. Enrollment,
verification and integration with HTID's unlock flow are later work. This
phase deliberately does not import NBIS or the standalone SIFT matcher.
