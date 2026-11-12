#!/bin/bash
# ht_env.sh
#
# Shared helpers for the build scripts. Source it, do not run it:
#
#   . "$(dirname "$0")/ht_env.sh"
#
# Everything sensor-specific comes from shared/sen/supported_sensors.h through
# ht_sensor_tool (see shared/sen/ht_sensor_tool.c), so there is one place to edit
# when a sensor or its dependencies change.

# This file lives in scripts/. HT_ROOT is the repo root: every build script
# cd's there, so all the paths below are relative to it.
HT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
HT_ROOT="$(cd "$HT_DIR/.." && pwd)"
HT_TOOL="$HT_ROOT/.ht_sensor_tool"

# Where the headers live after the reorganization (see README, "Repository
# layout"). Every compile uses these instead of one flat folder.
# shellcheck disable=SC2034 # used by the scripts that source this file
HT_INC="-I. -Ishared/sen -Imetallica -Iupek -Ivfs5011 -Ivfs7552 -Iegis -INBIS/include"

# Package manager: Homebrew when brew is on PATH, otherwise MacPorts when its
# port tool is found, otherwise none (the libraries may still be installed
# some other way). Intel Homebrew lives under /usr/local, MacPorts under
# /opt/local.
HT_PORT_BIN=""
if command -v port >/dev/null 2>&1; then
    HT_PORT_BIN="$(command -v port)"
elif [ -x /opt/local/bin/port ]; then
    HT_PORT_BIN="/opt/local/bin/port"
fi

if command -v brew >/dev/null 2>&1; then
    HT_PKG="brew"
    HT_BREW_PREFIX="$(brew --prefix 2>/dev/null || echo /usr/local)"
elif [ -n "$HT_PORT_BIN" ]; then
    HT_PKG="macports"
    HT_BREW_PREFIX="/usr/local"
else
    HT_PKG="none"
    HT_BREW_PREFIX="/usr/local"
fi

# HT_PREFIX is the prefix the libraries are actually taken from: the first of
# Homebrew's prefix, /usr/local and /opt/local (MacPorts) that has libusb.
# When none has it yet, it is the prefix of the package manager that was found,
# so the "install with" hint and the missing-file message point at the right place.
HT_PREFIX="$HT_BREW_PREFIX"
[ "$HT_PKG" != "macports" ] || HT_PREFIX="/opt/local"
for HT_P in "$HT_BREW_PREFIX" /usr/local /opt/local; do
    if [ -f "$HT_P/include/libusb-1.0/libusb.h" ]; then
        HT_PREFIX="$HT_P"
        break
    fi
done
unset HT_P
HT_LIBUSB_INC="$HT_PREFIX/include/libusb-1.0"
# shellcheck disable=SC2034 # used by the scripts that source this file
HT_LIBUSB_LIB="$HT_PREFIX/lib"

# OpenSSL 3 is keg-only in Homebrew (opt/openssl@3) and lives in
# /opt/local/libexec/openssl3 in MacPorts. Only the Metallica sensor needs it.
HT_OPENSSL_PREFIX="$HT_BREW_PREFIX/opt/openssl@3"
if [ ! -d "$HT_OPENSSL_PREFIX" ]; then
    if [ -d /opt/local/libexec/openssl3 ] || [ "$HT_PKG" = "macports" ]; then
        HT_OPENSSL_PREFIX="/opt/local/libexec/openssl3"
    fi
fi

# Package name of a Homebrew formula in MacPorts (the formula names are the
# ones supported_sensors.h uses).
ht_port_name() {
    case "$1" in
        openssl@3) echo "openssl3" ;;
        sevenzip)  echo "7zip" ;;
        *)         echo "$1" ;;
    esac
}

# ht_install_hint <formula>... prints the install command for the package
# manager that was found (Homebrew when neither was found).
ht_install_hint() {
    local names="" f
    if [ "$HT_PKG" = "macports" ]; then
        for f in "$@"; do names="$names $(ht_port_name "$f")"; done
        echo "sudo port install$names"
    else
        echo "brew install $*"
    fi
}

# Builds the sensor tool (needs clang only). Always rebuilt, it compiles
# in well under a second. A timestamp check ("rebuild only if the source
# is newer") is not safe here: files that arrive through a GitHub zip,
# a web upload or the in-app updater can carry timestamps OLDER than a
# binary built from a previous version, which left a stale tool running
# and made sensor auto-detect fall back to asking.
ht_ensure_tool() {
    if ! command -v clang >/dev/null 2>&1; then
        echo "error: clang not found. Install the Xcode Command Line Tools: xcode-select --install" >&2
        return 1
    fi
    rm -f "$HT_TOOL"
    clang -O1 -I"$HT_ROOT/shared/sen" "$HT_ROOT/shared/sen/ht_sensor_tool.c" -o "$HT_TOOL" || return 1
}

# Is this Homebrew formula's content present? Checked by looking for the
# files the build actually uses, not by asking brew, so it also works
# when brew itself is slow or broken.
ht_dep_installed() {
    case "$1" in
        libusb)      [ -f "$HT_LIBUSB_INC/libusb.h" ] ;;
        openssl@3)   [ -f "$HT_OPENSSL_PREFIX/include/openssl/evp.h" ] ;;
        innoextract) command -v innoextract >/dev/null 2>&1 || [ -x "$HT_PREFIX/bin/innoextract" ] \
                         || [ -x "$HT_BREW_PREFIX/bin/innoextract" ] || [ -x /opt/local/bin/innoextract ] ;;
        sevenzip)    command -v 7zz >/dev/null 2>&1 || command -v 7z >/dev/null 2>&1 \
                         || [ -x "$HT_PREFIX/bin/7zz" ] || [ -x "$HT_PREFIX/bin/7z" ] \
                         || [ -x "$HT_BREW_PREFIX/bin/7zz" ] || [ -x "$HT_BREW_PREFIX/bin/7z" ] \
                         || [ -x /opt/local/bin/7zz ] || [ -x /opt/local/bin/7z ] ;;
        *)           return 1 ;;
    esac
}

# Prints the formulas from stdin that are NOT installed, one per line.
ht_filter_missing() {
    local f
    while read -r f; do
        [ -n "$f" ] || continue
        ht_dep_installed "$f" || echo "$f"
    done
}

# brew refuses to run as root. When a script is started with sudo, drop
# back to the invoking user for the brew call.
ht_brew() {
    if [ "$(id -u)" = "0" ] && [ -n "$SUDO_USER" ] && [ "$SUDO_USER" != "root" ]; then
        sudo -u "$SUDO_USER" -H brew "$@"
    else
        brew "$@"
    fi
}

# ht_install_missing <assume_yes 0|1> <formula>...
# Installs only the formulas passed in. Asks first unless assume_yes.
ht_install_missing() {
    local assume_yes="$1"; shift
    [ "$#" -gt 0 ] || return 0

    if [ "$HT_PKG" = "none" ]; then
        echo "Neither Homebrew nor MacPorts was found. Install these yourself, then run this again:" >&2
        printf '  %s\n' "$@" >&2
        return 1
    fi

    local manager="Homebrew" hint
    [ "$HT_PKG" != "macports" ] || manager="MacPorts"
    hint="$(ht_install_hint "$@")"

    echo "Missing for your sensor: $*"
    if [ "$assume_yes" != "1" ]; then
        if [ ! -t 0 ]; then
            echo "Not an interactive terminal. Run: $hint" >&2
            return 1
        fi
        printf 'Install them now with %s? [y/N] ' "$manager"
        read -r ans
        case "$ans" in y|Y|yes|YES) ;; *) echo "Skipped. Install later with: $hint"; return 1 ;; esac
    fi

    if [ "$HT_PKG" = "macports" ]; then
        # MacPorts needs root. Start with sudo unless this already runs as root.
        local ports="" f
        for f in "$@"; do ports="$ports $(ht_port_name "$f")"; done
        # shellcheck disable=SC2086
        if [ "$(id -u)" = "0" ]; then
            "$HT_PORT_BIN" install $ports
        else
            sudo "$HT_PORT_BIN" install $ports
        fi
        return
    fi
    ht_brew install "$@"
}
