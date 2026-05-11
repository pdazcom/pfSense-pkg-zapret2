#!/bin/sh
#
# configtest.sh — Zapret2 configuration test / dry-run
#
# Checks:
#   1. dvtws2 binary present and executable
#   2. zapret-lib.lua present in Lua directory
#   3. zapret-antidpi.lua present in Lua directory
#   4. profiles.conf readable
#   5. PHP CLI available
#   6. zapret2.inc loadable (syntax)
#   7. Prints the command that would be executed (dry-run)
#
# Exit codes:
#   0 — all required checks passed
#   1 — one or more required checks failed
#
# Usage:
#   sh configtest.sh [--dry-run]
#

DRY_RUN=0
for arg in "$@"; do
    case "$arg" in
        --dry-run) DRY_RUN=1 ;;
    esac
done

DVTWS2_BIN="/usr/local/bin/dvtws2"
LUA_DIR="/usr/local/etc/zapret2"
PROFILES_CONF="/usr/local/etc/zapret2/profiles.conf"
ZAPRET2_INC="/usr/local/pkg/zapret2/includes/zapret2.inc"
PHP_BIN="/usr/local/bin/php"

ERRORS=0

err() {
    echo "  [ERROR] $*" >&2
    ERRORS=$((ERRORS + 1))
}

ok() {
    echo "  [OK]    $*"
}

warn() {
    echo "  [WARN]  $*"
}

echo "=== Zapret2 Configuration Test @ $(date) ==="
echo ""

# ---- 1. dvtws2 binary ----
echo "-- Binaries --"
if [ -f "$DVTWS2_BIN" ]; then
    if [ -x "$DVTWS2_BIN" ]; then
        ok "dvtws2 found and executable: ${DVTWS2_BIN}"
    else
        err "dvtws2 found but NOT executable: ${DVTWS2_BIN}"
        err "Fix: chmod +x ${DVTWS2_BIN}"
    fi
else
    err "dvtws2 not found at ${DVTWS2_BIN}"
    err "Install zapret2 backend first — see INSTALL.md"
fi

# ---- 2. Lua files ----
echo ""
echo "-- Lua Files --"
if [ -f "${LUA_DIR}/zapret-lib.lua" ]; then
    ok "zapret-lib.lua found: ${LUA_DIR}/zapret-lib.lua"
else
    err "zapret-lib.lua not found at ${LUA_DIR}/zapret-lib.lua"
    err "Copy Lua files from zapret2 source to ${LUA_DIR}/ — see INSTALL.md"
fi

if [ -f "${LUA_DIR}/zapret-antidpi.lua" ]; then
    ok "zapret-antidpi.lua found: ${LUA_DIR}/zapret-antidpi.lua"
else
    err "zapret-antidpi.lua not found at ${LUA_DIR}/zapret-antidpi.lua"
    err "Copy Lua files from zapret2 source to ${LUA_DIR}/ — see INSTALL.md"
fi

# ---- 3. Config files ----
echo ""
echo "-- Configuration Files --"
if [ -f "$PROFILES_CONF" ]; then
    ok "profiles.conf found: ${PROFILES_CONF}"
    # Check that at least SAFE= and DEFAULT= entries exist
    if grep -q "^DEFAULT=" "$PROFILES_CONF" && grep -q "^SAFE=" "$PROFILES_CONF"; then
        ok "profiles.conf contains DEFAULT and SAFE entries"
    else
        warn "profiles.conf is missing expected DEFAULT or SAFE entries"
    fi
else
    warn "profiles.conf not found at ${PROFILES_CONF} (using built-in defaults)"
fi

# ---- 4. PHP CLI ----
echo ""
echo "-- PHP Environment --"
if [ -x "$PHP_BIN" ]; then
    PHP_VER=$("$PHP_BIN" -r 'echo PHP_MAJOR_VERSION . "." . PHP_MINOR_VERSION;' 2>/dev/null)
    ok "PHP CLI found: ${PHP_BIN} (${PHP_VER})"
else
    err "PHP CLI not found at ${PHP_BIN}"
fi

if [ -f "$ZAPRET2_INC" ]; then
    ok "zapret2.inc found: ${ZAPRET2_INC}"
    if [ -x "$PHP_BIN" ]; then
        SYNTAX=$("$PHP_BIN" -l "$ZAPRET2_INC" 2>&1)
        SYNTAX_RET=$?
        if [ $SYNTAX_RET -eq 0 ]; then
            ok "zapret2.inc syntax OK"
        else
            err "zapret2.inc has PHP syntax errors:"
            echo "$SYNTAX" | sed 's/^/    /'
        fi
    fi
else
    err "zapret2.inc not found at ${ZAPRET2_INC}"
fi

# ---- 5. Dry-run command ----
echo ""
echo "-- Dry Run --"
if [ "$ERRORS" -eq 0 ] || [ "$DRY_RUN" -eq 1 ]; then
    CMD=$("$PHP_BIN" "$ZAPRET2_INC" configtest 2>/dev/null)
    if [ $? -eq 0 ]; then
        echo "  Command that would be executed:"
        echo "    ${CMD}"
    else
        warn "Could not generate command (zapret2.inc configtest failed)"
        warn "Check that pfSense config.xml is accessible"
    fi
else
    warn "Skipping dry-run due to errors above"
fi

# ---- Summary ----
echo ""
if [ "$ERRORS" -gt 0 ]; then
    echo "=== FAILED: ${ERRORS} error(s) found. Fix before starting service. ==="
    exit 1
fi

echo "=== OK: Configuration is valid ==="
exit 0
