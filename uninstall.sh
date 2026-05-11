#!/bin/sh
#
# uninstall.sh — pfSense-pkg-zapret2 uninstaller
#
# Run as root on a pfSense box:
#   sh uninstall.sh
#
# What it does:
#   1. Stops the zapret2 service
#   2. Removes plugin files (GUI, rc.d, helpers)
#   3. Unregisters the Services → Zapret2 menu from pfSense config
#   4. Optionally removes the zapret2 backend (dvtws2 + Lua files)
#
# The zapret2 backend is NOT removed by default.
# Use --remove-backend to also remove dvtws2 and Lua files.
#

set -e

REMOVE_BACKEND=0
for arg in "$@"; do
    case "$arg" in
        --remove-backend) REMOVE_BACKEND=1 ;;
    esac
done

info()    { printf '\033[0;32m[INFO]\033[0m  %s\n' "$*"; }
warn()    { printf '\033[1;33m[WARN]\033[0m  %s\n' "$*"; }
section() { printf '\n=== %s ===\n' "$*"; }

[ "$(id -u)" = "0" ] || { echo "Must be run as root"; exit 1; }

# ---- Stop service ----
section "Stopping zapret2 service"

if [ -f /var/run/zapret2.pid ]; then
    PID=$(cat /var/run/zapret2.pid)
    if kill -0 "$PID" 2>/dev/null; then
        kill "$PID" 2>/dev/null && info "Stopped dvtws2 (pid $PID)" || warn "Could not stop pid $PID"
    fi
    rm -f /var/run/zapret2.pid
else
    info "Service not running"
fi

# Remove IPFW divert rule if present
if ipfw list 2>/dev/null | grep -q "divert"; then
    ipfw delete 100 2>/dev/null && info "IPFW divert rule removed" || warn "Could not remove IPFW rule 100"
fi

# Disable on boot
RC_CONF=/etc/rc.conf.local
if [ -f "$RC_CONF" ] && grep -q "zapret2_enable" "$RC_CONF"; then
    sed -i '' '/zapret2_enable/d' "$RC_CONF"
    info "Removed zapret2_enable from $RC_CONF"
fi

# ---- Remove plugin files ----
section "Removing plugin files"

rm -f /usr/local/www/zapret2/zapret2.php
rmdir /usr/local/www/zapret2 2>/dev/null && true

rm -f /usr/local/pkg/zapret2/includes/zapret2.inc
rmdir /usr/local/pkg/zapret2/includes 2>/dev/null && true
rmdir /usr/local/pkg/zapret2 2>/dev/null && true

rm -f /usr/local/etc/rc.d/zapret2

rm -f /usr/local/share/zapret2/healthcheck.sh
rm -f /usr/local/share/zapret2/configtest.sh
rmdir /usr/local/share/zapret2 2>/dev/null && true

rm -f /var/log/zapret2.log

info "Plugin files removed"

# ---- Unregister from pfSense config ----
section "Unregistering from pfSense"

php -r "
require_once('/etc/inc/globals.inc');
require_once('/etc/inc/config.inc');
require_once('/etc/inc/functions.inc');
global \$config;

// Remove menu entry
if (is_array(\$config['installedpackages']['menu'] ?? null)) {
    \$before = count(\$config['installedpackages']['menu']);
    \$config['installedpackages']['menu'] = array_values(array_filter(
        \$config['installedpackages']['menu'],
        function(\$m) { return (\$m['name'] ?? '') !== 'Zapret2'; }
    ));
    if (count(\$config['installedpackages']['menu']) < \$before) {
        echo 'Menu entry removed' . PHP_EOL;
    }
}

// Remove package config
if (isset(\$config['installedpackages']['zapret2'])) {
    unset(\$config['installedpackages']['zapret2']);
    echo 'Package config removed' . PHP_EOL;
}

write_config('Zapret2: uninstall');
echo 'pfSense config updated' . PHP_EOL;
" 2>/dev/null || warn "pfSense config update failed (may be OK outside pfSense)"

# ---- Optionally remove backend ----
if [ "$REMOVE_BACKEND" -eq 1 ]; then
    section "Removing zapret2 backend"
    rm -f /usr/local/bin/dvtws2
    rm -f /usr/local/etc/zapret2/zapret-lib.lua
    rm -f /usr/local/etc/zapret2/zapret-antidpi.lua
    rm -f /usr/local/etc/zapret2/profiles.conf
    rmdir /usr/local/etc/zapret2 2>/dev/null && true
    info "Backend (dvtws2 + Lua files) removed"
else
    info "Backend (dvtws2 + Lua files) kept — use --remove-backend to also remove them"
fi

echo ""
info "Uninstall complete."
