#!/bin/sh
#
# healthcheck.sh — Zapret2 health check
#
# Checks:
#   1. dvtws2 process running
#   2. PID file matches running process
#   3. IPFW divert rule is active
#   4. Kernel modules loaded (ipfw, ipdivert)
#
# Exit codes:
#   0 — all checks passed
#   1 — one or more checks failed
#
# Usage:
#   sh healthcheck.sh [--quiet]
#
# NOTE: Connectivity checks (curl to YouTube etc.) are intentionally omitted.
# Traffic bypass is for LAN clients, not the router itself. Test from a client.
#

QUIET=0
for arg in "$@"; do
    case "$arg" in
        --quiet) QUIET=1 ;;
    esac
done

PASS=0
FAIL=0

log() {
    if [ "$QUIET" -eq 0 ]; then
        echo "$@"
    fi
}

check() {
    local label="$1"
    local result="$2"
    if [ "$result" -eq 0 ]; then
        log "  [PASS] ${label}"
        PASS=$((PASS + 1))
    else
        log "  [FAIL] ${label}"
        FAIL=$((FAIL + 1))
    fi
}

log "=== Zapret2 Health Check @ $(date) ==="

# ---- 1. Process check ----
log ""
log "-- Process --"

dvtws2_pids=$(pgrep -x dvtws2 2>/dev/null)
if [ -n "$dvtws2_pids" ]; then
    check "dvtws2 running (pid: $dvtws2_pids)" 0
else
    check "dvtws2 running" 1
fi

# ---- 2. PID file ----
log ""
log "-- PID file --"
if [ -f /var/run/zapret2.pid ]; then
    pid_file=$(cat /var/run/zapret2.pid)
    if kill -0 "$pid_file" 2>/dev/null; then
        check "PID file valid (pid: ${pid_file})" 0
    else
        check "PID file exists but process ${pid_file} is dead" 1
    fi
else
    check "PID file /var/run/zapret2.pid exists" 1
fi

# ---- 3. Kernel modules ----
log ""
log "-- Kernel modules --"
kldstat -n ipfw   >/dev/null 2>&1; check "ipfw module loaded"    $?
kldstat -n ipdivert >/dev/null 2>&1; check "ipdivert module loaded" $?

# ---- 4. IPFW divert rule ----
log ""
log "-- IPFW rule --"
if ipfw list 2>/dev/null | grep -q "divert"; then
    check "IPFW divert rule active" 0
else
    check "IPFW divert rule active" 1
    log "  Hint: run 'ipfw add 100 divert 990 tcp from any to any 80,443 out not diverted not sockarg'"
fi

# ---- Summary ----
log ""
log "=== Summary: ${PASS} passed, ${FAIL} failed ==="

if [ "$FAIL" -gt 0 ]; then
    exit 1
fi
exit 0
