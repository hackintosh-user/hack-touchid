#!/bin/bash
# Build script for the PAM module (v2.1.0 fingerprint for sudo and su).
# Lives in scripts/, writes shared/pam/build/pam_hacktouchid.so, works from any directory.
#
#   ./scripts/build_pam.sh            build for this Mac's architecture
#   ./scripts/build_pam.sh --universal  build x86_64 + arm64
#   ./scripts/build_pam.sh --test     build and run the end-to-end self-test (needs sudo)
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"   # scripts/
cd "$SCRIPT_DIR/.."                           # repo root
. "$SCRIPT_DIR/ht_env.sh"

OUT_DIR="shared/pam/build"
mkdir -p "$OUT_DIR"

ARCH_FLAGS=""
RUN_TEST=0
for a in "$@"; do
    case "$a" in
        --universal) ARCH_FLAGS="-arch x86_64 -arch arm64" ;;
        --test) RUN_TEST=1 ;;
        *) echo "usage: $0 [--universal] [--test]" >&2; exit 1 ;;
    esac
done

# A PAM module is loaded with dlopen() by sudo/su, so it is a bundle.
clang -bundle -Wall -Wextra $ARCH_FLAGS \
    shared/pam/pam_hacktouchid.c \
    -Ishared/pam -lpam \
    -o "$OUT_DIR/pam_hacktouchid.so"
codesign -s - -f "$OUT_DIR/pam_hacktouchid.so" 2>/dev/null || true
echo "Build complete: $OUT_DIR/pam_hacktouchid.so"

if [ "$RUN_TEST" = "1" ]; then
    clang -Wall -Wextra shared/pam/pam_selftest.c -Ishared/pam -o "$OUT_DIR/pam_selftest" -lpthread
    if [ "$(id -u)" != "0" ]; then
        echo "The self-test talks to a root-owned socket, re-running it with sudo..."
        sudo "$OUT_DIR/pam_selftest"
    else
        "$OUT_DIR/pam_selftest"
    fi
fi
