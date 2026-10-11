#!/usr/bin/env bash
#
# Start every CarLifePort process; Ctrl-C stops them all.
#
#   ./scripts/run.sh
#
# Startup order: carlife-usb-bridge -> carlife-host -> carlifed.
# carlife-host waits for the bridge port 7200 before setting up channels, so no
# manual delay is needed. When any process exits, the rest are stopped.
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

BRIDGE="tools/usb-bridge/carlife-usb-bridge"
HOST="host/carlife-host"
SERVER="server/carlifed"
WEB="web/dist"

# ---------------------------------------------------------------- prechecks
for f in "$BRIDGE" "$HOST" "$SERVER"; do
    [ -x "$f" ] || { echo "missing $f, run ./scripts/build.sh first" >&2; exit 1; }
done
[ -f "$WEB/index.html" ] || { echo "missing $WEB, run ./scripts/build.sh first" >&2; exit 1; }

if pgrep -x WeChat >/dev/null 2>&1; then
    cat >&2 <<'EOF'
WeChat is running.

WeChat performs an AOA handshake with Android phones on its own and takes
exclusive ownership of the whole USB device. libusb then cannot claim it
(LIBUSB_ERROR_ACCESS), and adb kill-server does not help either.

Quit WeChat, then run this script again.
EOF
    exit 1
fi

# -------------------------------------------------------------- USB reset
# accessory mode sticks across reconnects and can only be cleared through adb,
# so clear the mode first and stop the adb server afterwards.
if command -v adb >/dev/null 2>&1; then
    echo "== resetting phone USB mode"
    if adb shell svc usb setFunctions >/dev/null 2>&1; then
        echo "   cleared accessory mode"
    else
        echo "   no phone connected or not authorized, skipping (plug it in and rerun)"
    fi
    adb kill-server >/dev/null 2>&1 || true
else
    echo "== adb not found, skipping USB mode reset" >&2
fi

# ------------------------------------------------------------------ start
PIDS=()
WATCH_PID=""

stop_all() {
    trap - INT TERM EXIT
    echo
    echo "== stopping all processes"
    [ -n "$WATCH_PID" ] && kill "$WATCH_PID" 2>/dev/null || true
    [ "${#PIDS[@]}" -gt 0 ] && kill "${PIDS[@]}" 2>/dev/null || true
    wait 2>/dev/null || true
}
trap stop_all INT TERM EXIT

start() {
    local name="$1"; shift
    "$@" > >(while IFS= read -r line; do printf '[%s] %s\n' "$name" "$line"; done) 2>&1 &
    local pid=$!
    PIDS+=("$pid")
    printf '== started %-7s pid %s\n' "$name" "$pid"
}

echo
start bridge "$BRIDGE"
start host   "$HOST"
start server "$SERVER"

# Stop everything as soon as one process exits, so a bridge that fails to start
# does not leave the script hanging silently.
watch() {
    while :; do
        sleep 1
        for pid in "${PIDS[@]}"; do
            if ! kill -0 "$pid" 2>/dev/null; then
                echo "== process $pid exited, stopping all"
                kill -TERM $$ 2>/dev/null || true
                return
            fi
        done
    done
}
watch &
WATCH_PID=$!

cat <<'EOF'

== open http://127.0.0.1:8080 in a browser
== open the CarLife app on the phone and confirm the connection
== Ctrl-C stops everything

EOF

wait
