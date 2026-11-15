#!/bin/bash
# hack-touchid-pam-install.sh (v2.1.0)
#
# Turns the fingerprint on or off for sudo (and optionally su and other PAM
# services). Run with sudo from the repo or from /usr/local/libexec/hack-touchid:
#
#   sudo ./hack-touchid-pam-install.sh install [--service NAME ...]
#   sudo ./hack-touchid-pam-install.sh uninstall
#   ./hack-touchid-pam-install.sh status
#
# install:   builds pam_hacktouchid.so if needed, puts it in /usr/local/lib/pam,
#            enables the invoking user ($SUDO_USER) in pam_users.conf and adds one
#            line to /etc/pam.d/sudo_local (macOS 14+, survives updates) and to
#            /etc/pam.d/NAME for every --service.
# uninstall: removes our lines, the module and the users file entries. The password
#            keeps working at every step, the module only ever adds a fingerprint
#            attempt in front of it ("auth sufficient").
#
# The daemon must be a v2.1.0 build (it owns the sensor and the socket). If it is
# not running, sudo simply asks for the password as before.

set -e

MARK="# hack-touchid"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

# Paths, overridable only for the sandbox test (HTID_PAM_TEST=1).
PAM_D="${HTID_PAM_D:-/etc/pam.d}"
LIB_DIR="${HTID_PAM_LIB_DIR:-/usr/local/lib/pam}"
CONF_DIR="${HTID_PAM_CONF_DIR:-/usr/local/libexec/hack-touchid}"
SOCKET="${HTID_PAM_SOCKET:-/var/run/hack-touchid-pam.sock}"
DISABLED_FLAG="${HTID_PAM_DISABLED_FLAG:-/Library/Application Support/VFS5011/pam_disabled}"
MODULE_NAME="pam_hacktouchid.so"
MODULE="$LIB_DIR/$MODULE_NAME"
USERS_CONF="$CONF_DIR/pam_users.conf"
LINE="auth       sufficient     $MODULE $MARK"

own_root() {
    if [ "$HTID_PAM_TEST" = "1" ]; then return 0; fi
    chown root:wheel "$@"
}

need_root() {
    [ "$HTID_PAM_TEST" = "1" ] && return 0
    if [ "$(id -u)" != "0" ]; then
        echo "error: run this with sudo." >&2
        exit 1
    fi
}

check_macos() {
    [ "$HTID_PAM_TEST" = "1" ] && return 0
    if [ "$(uname -s)" != "Darwin" ]; then
        echo "error: macOS only." >&2
        exit 1
    fi
    if [ "$(uname -r | cut -d. -f1)" -lt 23 ]; then
        echo "error: needs macOS 14 Sonoma or later (/etc/pam.d/sudo_local)." >&2
        exit 1
    fi
}

valid_name() {
    case "$1" in
        ""|-*|*[!A-Za-z0-9._-]*) return 1 ;;
    esac
    [ "${#1}" -le 64 ]
}

# Rewrites a pam.d file with our line removed, keeping its mode and owner.
strip_line() {
    local f="$1"
    [ -f "$f" ] || return 0
    grep -q "$MARK" "$f" || return 0
    local tmp; tmp="$(mktemp)"
    grep -v "$MARK" "$f" > "$tmp" || true
    cat "$tmp" > "$f"
    rm -f "$tmp"
}

# Puts our line at the top of a pam.d file (before the password modules),
# unless it is already there. Creates the file for sudo_local.
add_line() {
    local f="$1"
    if [ -f "$f" ] && grep -q "$MARK" "$f"; then return 0; fi
    local tmp; tmp="$(mktemp)"
    printf '%s\n' "$LINE" > "$tmp"
    [ -f "$f" ] && cat "$f" >> "$tmp"
    if [ -f "$f" ]; then
        [ -f "$f.hack-touchid.bak" ] || cp -p "$f" "$f.hack-touchid.bak"
        cat "$tmp" > "$f"
    else
        cat "$tmp" > "$f"
        chmod 444 "$f"
        own_root "$f"
    fi
    rm -f "$tmp"
}

do_install() {
    need_root
    check_macos
    shift
    local services=()
    while [ $# -gt 0 ]; do
        case "$1" in
            --service) shift; valid_name "$1" || { echo "error: bad service name." >&2; exit 1; }; services+=("$1") ;;
            *) echo "usage: $0 install [--service NAME ...]" >&2; exit 1 ;;
        esac
        shift
    done

    local user="${SUDO_USER:-}"
    if [ -z "$user" ] || [ "$user" = "root" ] || ! valid_name "$user"; then
        echo "error: run it as 'sudo $0 install' from your own account (SUDO_USER is '$user')." >&2
        exit 1
    fi

    local built="$REPO_ROOT/shared/pam/build/$MODULE_NAME"
    if [ ! -f "$built" ]; then
        echo "Building $MODULE_NAME..."
        if [ -n "$SUDO_USER" ] && [ "$HTID_PAM_TEST" != "1" ]; then
            sudo -u "$SUDO_USER" bash "$REPO_ROOT/scripts/build_pam.sh"
        else
            bash "$REPO_ROOT/scripts/build_pam.sh"
        fi
    fi
    [ -f "$built" ] || { echo "error: $built was not built." >&2; exit 1; }

    local fresh=0
    [ -f "$MODULE" ] || fresh=1
    mkdir -p "$LIB_DIR" "$CONF_DIR"
    chmod 755 "$LIB_DIR" "$CONF_DIR"
    own_root "$LIB_DIR" "$CONF_DIR"
    cp "$built" "$MODULE.new"
    chmod 444 "$MODULE.new"
    own_root "$MODULE.new"
    codesign -s - -f "$MODULE.new" 2>/dev/null || true
    mv -f "$MODULE.new" "$MODULE"
    echo "Module installed: $MODULE"

    # Users file: root-owned, not writable by anyone else, or the daemon ignores it.
    touch "$USERS_CONF"
    grep -qx "$user" "$USERS_CONF" || printf '%s\n' "$user" >> "$USERS_CONF"
    chmod 644 "$USERS_CONF"
    own_root "$USERS_CONF"
    echo "Enabled for user: $user"
    # A first install starts ON. A reinstall keeps whatever the person chose.
    [ "$fresh" = "1" ] && rm -f "$DISABLED_FLAG"

    add_line "$PAM_D/sudo_local"
    echo "sudo: line added to $PAM_D/sudo_local"
    if [ -f "$PAM_D/sudo" ] && ! grep -q "sudo_local" "$PAM_D/sudo"; then
        echo "warning: $PAM_D/sudo does not include sudo_local, so sudo will ignore it." >&2
    fi
    local s
    for s in "${services[@]}"; do
        if [ ! -f "$PAM_D/$s" ]; then echo "warning: $PAM_D/$s does not exist, skipped." >&2; continue; fi
        add_line "$PAM_D/$s"
        echo "$s: line added to $PAM_D/$s (a macOS update may reset this file, re-run install)"
    done
    echo
    echo "Done. Open a new terminal and run: sudo -k && sudo true"
    echo "The daemon (v2.1.0 or newer) must be running; if it is not, sudo just asks for the password."
}

do_uninstall() {
    need_root
    strip_line "$PAM_D/sudo_local"
    # Drop an empty sudo_local that we created.
    if [ -f "$PAM_D/sudo_local" ] && [ ! -s "$PAM_D/sudo_local" ]; then rm -f "$PAM_D/sudo_local"; fi
    local f
    for f in "$PAM_D"/*; do
        [ -f "$f" ] && grep -q "$MARK" "$f" && strip_line "$f" && echo "line removed from $f"
    done
    rm -f "$MODULE"
    rm -f "$USERS_CONF"
    rm -f "$DISABLED_FLAG"
    echo "Fingerprint for sudo/su removed. The password is unchanged."
}

do_status() {
    echo "module:      $([ -f "$MODULE" ] && echo "installed ($MODULE)" || echo "not installed")"
    echo "switch:      $([ -f "$DISABLED_FLAG" ] && echo "OFF" || echo "ON")"
    echo "users file:  $([ -f "$USERS_CONF" ] && tr '\n' ' ' < "$USERS_CONF" || echo "none")"
    local f any=0
    for f in "$PAM_D"/*; do
        if [ -f "$f" ] && grep -q "$MARK" "$f"; then echo "pam.d line:  $f"; any=1; fi
    done
    [ "$any" = "1" ] || echo "pam.d line:  none"
    echo "daemon socket: $([ -S "$SOCKET" ] && echo "present" || echo "missing (daemon not running, or older than v2.1.0)")"
}

case "$1" in
    install)   do_install "$@" ;;
    uninstall) do_uninstall ;;
    status)    do_status ;;
    *) echo "usage: $0 install [--service NAME ...] | uninstall | status" >&2; exit 1 ;;
esac
