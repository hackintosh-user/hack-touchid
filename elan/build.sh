#!/bin/bash
# SPDX-License-Identifier: LGPL-2.1-or-later
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR/.."
# shellcheck source=scripts/ht_env.sh
. "$SCRIPT_DIR/../scripts/ht_env.sh"
if [ "$(uname -s)" != Darwin ]; then
    echo "ELAN capture test targets macOS; protocol unit tests can run separately." >&2
    exit 1
fi
# Optional local libusb prefix for developers without Homebrew/MacPorts.
if [ -n "${ELAN_LIBUSB_PREFIX:-}" ]; then
    HT_LIBUSB_INC="$ELAN_LIBUSB_PREFIX/include/libusb-1.0"
    HT_LIBUSB_LIB="$ELAN_LIBUSB_PREFIX/lib"
fi
if [ ! -f "$HT_LIBUSB_INC/libusb.h" ]; then
    echo "Missing libusb headers; install libusb or set ELAN_LIBUSB_PREFIX." >&2
    exit 1
fi
clang -std=c11 -O2 -Wall -Wextra -Werror -Ielan -I"$HT_LIBUSB_INC" \
    elan/elan_capture.c elan/elan_capture_test.c -L"$HT_LIBUSB_LIB" -lusb-1.0 \
    -Wl,-rpath,"$HT_LIBUSB_LIB" -o elan_capture_test
echo "Built ./elan_capture_test (capture only; nothing installed)."
