#!/bin/sh
#
# healthcheck.sh — Zapret2 health check
#
# Checks:
#   1. dvtws2 process running
#   2. PID file matches running process
#   3. Kernel modules loaded (ipfw, ipdivert)
#   4. IPFW rules active and consistent
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

ipfw_has_rule() {
    ipfw list 2>/dev/null | grep -qE "^0*${1}[[:space:]]"
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
kldstat -n ipfw     >/dev/null 2>&1; check "ipfw module loaded"    $?
kldstat -n ipdivert >/dev/null 2>&1; check "ipdivert module loaded" $?

# ---- 4. IPFW rules ----
log ""
log "-- IPFW rules --"

# Rule 100 is the first and only required rule

# Rule 100 — main TCP divert (always required when service is running)
if [ -n "$dvtws2_pids" ] && ipfw_has_rule 100; then
    check "IPFW rule 100 (TCP 80/443 divert) active" 0
elif [ -z "$dvtws2_pids" ] && ipfw_has_rule 100; then
    check "IPFW rule 100 present but dvtws2 is NOT running — traffic is blackholed!" 1
    log "  Fix: service zapret2 restart  OR  ipfw delete 100"
elif [ -n "$dvtws2_pids" ] && ! ipfw_has_rule 100; then
    check "IPFW rule 100 (TCP 80/443 divert) active" 1
    log "  Hint: run 'ipfw list' to see current rules"
else
    log "  [INFO] IPFW rule 100 not active (service stopped)"
fi

# Rule 101 — UDP 443 divert (QUIC — YouTube HTTP/3 or Discord)
if ipfw_has_rule 101; then
    rule101_detail=$(ipfw list 2>/dev/null | grep -E "^0*101[[:space:]]")
    log "  [INFO] IPFW rule 101 (UDP 443 QUIC) active: ${rule101_detail}"
else
    log "  [INFO] IPFW rule 101 (UDP 443 QUIC) not active"
fi

# Rule 102 — UDP 50000-65535 divert (Discord voice/video)
if ipfw_has_rule 102; then
    log "  [INFO] IPFW rule 102 (UDP 50000-65535 Discord voice/video) active"
else
    log "  [INFO] IPFW rule 102 (UDP 50000-65535 Discord voice/video) not active"
fi

# IPFW table 1 — alias include mode
TABLE1=$(ipfw table 1 list 2>/dev/null | wc -l | tr -d ' ')
if [ "${TABLE1}" -gt 0 ]; then
    log "  [INFO] IPFW table 1: ${TABLE1} entries (alias include mode active)"
fi

# ---- 5. Hostlists ----
log ""
log "-- Hostlists --"

DISCORD_LIST="/usr/local/etc/zapret2/discord-hostlist.txt"
YOUTUBE_LIST="/usr/local/etc/zapret2/youtube-hostlist.txt"

if [ -f "$DISCORD_LIST" ]; then
    dc=$(wc -l < "$DISCORD_LIST" | tr -d ' ')
    log "  [INFO] Discord hostlist: ${dc} entries"
else
    log "  [INFO] Discord hostlist: not present (Discord bypass disabled)"
fi

if [ -f "$YOUTUBE_LIST" ]; then
    yt=$(wc -l < "$YOUTUBE_LIST" | tr -d ' ')
    log "  [INFO] YouTube hostlist: ${yt} entries"
else
    log "  [INFO] YouTube hostlist: not present (YouTube bypass disabled)"
fi

# ---- 6. dvtws2 command line (active args) ----
log ""
log "-- Active dvtws2 arguments --"
if [ -n "$dvtws2_pids" ]; then
    # ps on FreeBSD: -o args= prints full command line
    ps_args=$(ps -p "$dvtws2_pids" -o args= 2>/dev/null | head -1)
    if [ -n "$ps_args" ]; then
        log "  ${ps_args}"
    else
        log "  (could not read process args)"
    fi
fi

# ---- Summary ----
log ""
log "=== Summary: ${PASS} passed, ${FAIL} failed ==="

if [ "$FAIL" -gt 0 ]; then
    exit 1
fi
exit 0
