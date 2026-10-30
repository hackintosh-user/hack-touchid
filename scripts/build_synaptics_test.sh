#!/bin/bash
# Builds ./synaptics-test, the read-only open test for the Synaptics
# Prometheus 06cb:00f0 (GET_VERSION, FPS_INIT, power down). It never enrolls
# and never deletes anything. Run it with: sudo ./synaptics-test

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"   # scripts/
cd "$SCRIPT_DIR/.."                           # repo root
. "$SCRIPT_DIR/ht_env.sh"

if ! ht_dep_installed libusb; then
    echo "error: libusb headers not found under $HT_LIBUSB_INC" >&2
    echo "       Homebrew: brew install libusb" >&2
    echo "       Intel Mac without Homebrew: build libusb from source into /usr/local" >&2
    exit 1
fi

clang -DSYN_STANDALONE_TEST -Isynaptics -I"$HT_LIBUSB_INC" \
    synaptics/synaptics_bmkt.c \
    -o synaptics-test \
    -L"$HT_LIBUSB_LIB" -lusb-1.0 -framework CoreFoundation -framework IOKit -lpthread

echo "Build complete: ./synaptics-test"
echo "Run: sudo ./synaptics-test"
