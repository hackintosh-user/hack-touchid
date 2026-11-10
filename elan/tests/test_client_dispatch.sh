#!/bin/bash
# SPDX-License-Identifier: LGPL-2.1-or-later
# Run after the normal client build. No real sensor is used.
set -euo pipefail
cd "$(dirname "$0")/../.."
TEST_DIR="$(mktemp -d "${TMPDIR:-/tmp}/htid-elan-dispatch.XXXXXX")"
cp ./hack-touchid "$TEST_DIR/hack-touchid"
cat > "$TEST_DIR/elan_capture_test" <<'STUB'
#!/bin/sh
[ "$#" = 2 ] && [ "$1" = --frames ] && [ "$2" = 400 ] || exit 99
printf 'ELAN_DISPATCH_OK\n'
exit 17
STUB
chmod +x "$TEST_DIR/elan_capture_test"
if output="$("$TEST_DIR/hack-touchid" --capture-elan 2>&1)"; then
    echo 'Expected the child exit status to propagate.' >&2
    exit 1
else
    test_exit="$?"
fi
if [ "$test_exit" != 17 ] || [ "$output" != ELAN_DISPATCH_OK ]; then
    printf 'Unexpected dispatcher result: exit=%s output=%s\n' "$test_exit" "$output" >&2
    exit 1
fi
if "$TEST_DIR/hack-touchid" --capture-elan --cli > "$TEST_DIR/invalid.txt" 2>&1; then
    exit 1
else
    [ "$?" = 2 ]
fi
mv "$TEST_DIR/elan_capture_test" "$TEST_DIR/stub-disabled"
if "$TEST_DIR/hack-touchid" --capture-elan > "$TEST_DIR/missing.txt" 2>&1; then
    exit 1
else
    [ "$?" = 1 ]
fi
grep -q 'not built' "$TEST_DIR/missing.txt"
echo 'PASS: client dispatch argv, exit status, invalid flags and missing diagnostic'
