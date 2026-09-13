#!/bin/sh
#
# install.sh — pfSense-pkg-zapret2 one-shot installer
#
# Run as root on a pfSense box:
#   sh install.sh
#
# What it does:
#   1. Downloads zapret2 backend (dvtws2 + Lua files) from GitHub releases
#   2. Installs plugin files (GUI, rc.d, helpers)
#   3. Registers the Services → Zapret2 menu in pfSense config
#   4. Creates log file
#
# Requirements: pfSense 2.6+, FreeBSD amd64, curl, php
#

set -e

# Flags
FORCE=0
LOCAL_TARBALL=""
while [ $# -gt 0 ]; do
    case "$1" in
        --force|-f)   FORCE=1 ;;
        --tarball)    shift; LOCAL_TARBALL="$1" ;;
        --tarball=*)  LOCAL_TARBALL="${1#--tarball=}" ;;
    esac
    shift
done

ZAPRET2_VERSION="v0.9.5.2"
ZAPRET2_URL="https://github.com/bol-van/zapret2/releases/download/${ZAPRET2_VERSION}/zapret2-${ZAPRET2_VERSION}.tar.gz"

PLUGIN_DIR="$(cd "$(dirname "$0")"; pwd)"

info()    { printf '\033[0;32m[INFO]\033[0m  %s\n' "$*"; }
warn()    { printf '\033[1;33m[WARN]\033[0m  %s\n' "$*"; }
error()   { printf '\033[0;31m[ERROR]\033[0m %s\n' "$*" >&2; }
die()     { error "$*"; exit 1; }
section() { printf '\n=== %s ===\n' "$*"; }

# ---- Sanity checks ----
section "Checking environment"

[ "$(id -u)" = "0" ] || die "Must be run as root"

[ -f /etc/inc/config.inc ] || die "This does not look like a pfSense system (no /etc/inc/config.inc)"

ARCH=$(uname -m)
[ "$ARCH" = "amd64" ] || die "Only amd64 is supported (detected: $ARCH)"

which curl >/dev/null 2>&1 || die "curl not found"
which php  >/dev/null 2>&1 || die "php not found"

PFSENSE_VER=$(cat /etc/version 2>/dev/null || echo "unknown")
FREEBSD_VER=$(uname -r)
info "pfSense: $PFSENSE_VER | FreeBSD: $FREEBSD_VER | arch: $ARCH"

# ---- Download zapret2 backend ----
section "Installing zapret2 backend (${ZAPRET2_VERSION})"

TMPDIR=$(mktemp -d /tmp/zapret2-install.XXXXXX)
trap 'rm -rf "$TMPDIR"' EXIT

if [ -x /usr/local/bin/dvtws2 ]; then
    EXISTING_VER=$(/usr/local/bin/dvtws2 --version 2>/dev/null | head -1 || echo "unknown")
    info "dvtws2 already installed: $EXISTING_VER"
    if [ "$FORCE" -eq 0 ]; then
        info "Skipping backend download (use --force to reinstall)"
        SKIP_BACKEND=1
    else
        info "Reinstalling backend (--force)"
    fi
fi

if [ -z "$SKIP_BACKEND" ]; then
    if [ -n "$LOCAL_TARBALL" ] && [ -f "$LOCAL_TARBALL" ]; then
        info "Using local tarball: $LOCAL_TARBALL"
        cp "$LOCAL_TARBALL" "$TMPDIR/zapret2.tar.gz"
    else
        info "Downloading ${ZAPRET2_URL} ..."
        curl -fsSL "$ZAPRET2_URL" -o "$TMPDIR/zapret2.tar.gz" || die "Download failed. Check internet connectivity on this machine."
    fi

    info "Extracting..."
    tar -xzf "$TMPDIR/zapret2.tar.gz" -C "$TMPDIR"

    # Find the extracted root directory (handles any version prefix)
    SRCDIR=$(find "$TMPDIR" -maxdepth 1 -mindepth 1 -type d | head -1)
    [ -d "$SRCDIR" ] || die "Could not find extracted directory in $TMPDIR"
    info "Extracted to: $SRCDIR"

    # Find dvtws2 binary anywhere under binaries/freebsd*/
    DVTWS2_BIN=$(find "$SRCDIR/binaries" -name "dvtws2" -type f 2>/dev/null | head -1)
    if [ -z "$DVTWS2_BIN" ]; then
        warn "Contents of binaries/:"
        ls "$SRCDIR/binaries/" 2>/dev/null || true
        die "dvtws2 binary not found in release tarball"
    fi
    info "Found dvtws2 at: $DVTWS2_BIN"
    install -m 0555 "$DVTWS2_BIN" /usr/local/bin/dvtws2
    info "dvtws2 installed: /usr/local/bin/dvtws2"

    # Install Lua files
    LUA_LIB=$(find "$SRCDIR" -name "zapret-lib.lua" -type f 2>/dev/null | head -1)
    LUA_ANTIDPI=$(find "$SRCDIR" -name "zapret-antidpi.lua" -type f 2>/dev/null | head -1)
    [ -f "$LUA_LIB" ]    || die "zapret-lib.lua not found in tarball"
    [ -f "$LUA_ANTIDPI" ] || die "zapret-antidpi.lua not found in tarball"
    mkdir -p /usr/local/etc/zapret2
    install -m 0644 "$LUA_LIB"    /usr/local/etc/zapret2/zapret-lib.lua
    install -m 0644 "$LUA_ANTIDPI" /usr/local/etc/zapret2/zapret-antidpi.lua
    info "Lua files installed: /usr/local/etc/zapret2/"
fi

# ---- Install plugin files ----
section "Installing pfSense-pkg-zapret2 plugin"

install -m 0644 "$PLUGIN_DIR/pkg/zapret2.xml" /usr/local/pkg/zapret2.xml

install -d /usr/local/www/zapret2
install -d /usr/local/pkg/zapret2/includes
install -d /usr/local/etc/zapret2
install -d /usr/local/share/zapret2

install -m 0644 "$PLUGIN_DIR/files/usr/local/www/zapret2/zapret2.php" \
    /usr/local/www/zapret2/zapret2.php

install -m 0644 "$PLUGIN_DIR/files/usr/local/www/zapret2/zapret2_test_runner.php" \
    /usr/local/www/zapret2/zapret2_test_runner.php

install -m 0644 "$PLUGIN_DIR/files/usr/local/pkg/zapret2/includes/zapret2.inc" \
    /usr/local/pkg/zapret2/includes/zapret2.inc

install -m 0555 "$PLUGIN_DIR/files/usr/local/etc/rc.d/zapret2" \
    /usr/local/etc/rc.d/zapret2

install -m 0644 "$PLUGIN_DIR/files/usr/local/etc/zapret2/profiles.conf" \
    /usr/local/etc/zapret2/profiles.conf

install -m 0555 "$PLUGIN_DIR/scripts/healthcheck.sh" \
    /usr/local/share/zapret2/healthcheck.sh

install -m 0555 "$PLUGIN_DIR/scripts/configtest.sh" \
    /usr/local/share/zapret2/configtest.sh

# Create log file
touch /var/log/zapret2.log
chmod 640 /var/log/zapret2.log

info "Plugin files installed"

# ---- Register in pfSense config ----
section "Registering with pfSense"

php -r "
require_once('/etc/inc/globals.inc');
require_once('/etc/inc/config.inc');
require_once('/etc/inc/functions.inc');
require_once('/usr/local/pkg/zapret2/includes/zapret2.inc');
global \$config;

if (!is_array(\$config['installedpackages'])) {
    \$config['installedpackages'] = [];
}
if (!is_array(\$config['installedpackages']['menu'])) {
    \$config['installedpackages']['menu'] = [];
}

\$found = false;
foreach (\$config['installedpackages']['menu'] as \$m) {
    if ((\$m['name'] ?? '') === 'Zapret2') { \$found = true; break; }
}
if (!\$found) {
    \$config['installedpackages']['menu'][] = [
        'name'        => 'Zapret2',
        'tooltiptext' => 'Configure Zapret2 DPI bypass',
        'section'     => 'Services',
        'url'         => '/zapret2/zapret2.php',
    ];
    write_config('Zapret2: register menu');
    echo 'Menu registered' . PHP_EOL;
} else {
    echo 'Menu already registered' . PHP_EOL;
}
zapret2_install();
echo 'Install hook done' . PHP_EOL;
" || die "pfSense registration failed; the package filter hook is required"

# ---- Validation ----
section "Validating installation"

sh /usr/local/share/zapret2/configtest.sh

echo ""
info "Installation complete!"
info "Open your browser: Services → Zapret2"
info "Or directly: https://<pfsense-ip>/zapret2/zapret2.php"
