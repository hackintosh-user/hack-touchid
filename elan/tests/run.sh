#!/bin/bash
# SPDX-License-Identifier: LGPL-2.1-or-later
set -euo pipefail
cd "$(dirname "$0")/../.."
TEST_DIR="$(mktemp -d "${TMPDIR:-/tmp}/htid-elan-tests.XXXXXX")"
clang -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -Ielan/tests/include -Ielan elan/elan_capture.c elan/tests/test_capture.c \
    -o "$TEST_DIR/test_capture"
"$TEST_DIR/test_capture"
printf 'Test executable: %s\n' "$TEST_DIR/test_capture"

clang -std=c11 -D_POSIX_C_SOURCE=200809L -DELAN_FAKE_USB_ONLY -Wall -Wextra -Werror \
    -Ielan/tests/include -Ielan elan/elan_capture.c elan/elan_capture_test.c \
    elan/tests/test_capture.c -o "$TEST_DIR/capture_cli"
python3 - "$TEST_DIR" <<'PYTEST'
import pathlib, subprocess, sys, stat
folder=pathlib.Path(sys.argv[1]); exe=folder/'capture_cli'
def run(*args):
    return subprocess.run([str(exe), *args],capture_output=True)
for args in [('--frames','0'),('--frames','-1'),('--frames','2x'),('--probe','--frames','1'),('--probe','--output','x')]:
    assert run(*args).returncode==2
assert run('--probe').returncode==0
out=folder/'frame.pgm'
assert run('--frames','1','--output',str(out)).returncode==0
content=out.read_bytes();header=b'P5\n150 52\n65535\n'
assert content.startswith(header) and len(content)==len(header)+150*52*2
assert content[len(header)+2:len(header)+4]==bytes([0,52])
assert stat.S_IMODE(out.stat().st_mode)==0o600
assert run('--frames','1','--output',str(out)).returncode==1
assert out.read_bytes()==content
link=folder/'link.pgm';link.symlink_to(out)
assert run('--frames','1','--output',str(link)).returncode==1
assert out.read_bytes()==content
print('PASS: CLI validation, private PGM format, permissions and no-overwrite/symlink guards')
PYTEST
