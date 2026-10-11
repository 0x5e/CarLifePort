#!/usr/bin/env bash
#
# Start every CarLifePort process; Ctrl-C stops them all.
#
#   ./scripts/run.sh
#
# The phone may be plugged in before or after this script starts: usb-bridge waits for a
# device instead of giving up, and carlife-host waits for the bridge without a deadline.
#
# bridge and host are restarted together whenever either exits, which is what happens when
# the phone is unplugged. The link cannot be resumed in place: CarLifeVehicleLib only
# establishes its six channels inside connectionSetup(), and the phone side starts a fresh
# session on every accessory attach. carlifed stays up across a rebuild, so the browser
# keeps its WebSocket connection and reconnects by itself.
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

BRIDGE="tools/usb-bridge/carlife-usb-bridge"
HOST="host/carlife-host"
SERVER="server/carlifed"
WEB="vendor/playport/web/dist"
# The listen address, the requested frame rate and the access token are not passed to
# carlifed here: it reads ADDR, FPS and TOKEN from the environment and carries its own
# defaults, so this script does not have to restate them. See server/main.go.
# Head unit identity reporting (statistic info / hu info) is on, and the vendor channel
# number it carries is a flag on carlife-host that already has a default (20000000). Add
# --channel here to try another one. See README ("Head unit identity").

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
# Only needed before the first start: accessory mode sticks across reconnects, so clearing
# it here makes the bridge run a full 51/52/53 handshake instead of attaching to the
# accessory session left over from a previous run.
if command -v adb >/dev/null 2>&1; then
    echo "== resetting phone USB mode"
    if adb shell svc usb setFunctions >/dev/null 2>&1; then
        echo "   cleared accessory mode"
    else
        echo "   no phone connected or not authorized, skipping (plug it in any time)"
    fi
    adb kill-server >/dev/null 2>&1 || true
else
    echo "== adb not found, skipping USB mode reset" >&2
fi

# ------------------------------------------------------------------ start
PIDS=()
LAST_PID=0

stop_all() {
    trap - INT TERM EXIT
    echo
    echo "== stopping all processes"
    [ "${#PIDS[@]}" -gt 0 ] && kill "${PIDS[@]}" 2>/dev/null || true
    wait 2>/dev/null || true
}
trap stop_all INT TERM EXIT

start() {
    local name="$1"; shift
    "$@" > >(while IFS= read -r line; do printf '[%s] %s\n' "$name" "$line"; done) 2>&1 &
    LAST_PID=$!
    printf '== started %-7s pid %s\n' "$name" "$LAST_PID"
}

cat <<'EOF'

== open one of the viewer URLs printed by carlifed below; it carries the access token
== the certificate is self-signed: accept the browser warning once
== carlifed reads ADDR, FPS and TOKEN from the environment, for example
==   ADDR=127.0.0.1:8080 FPS=30 ./scripts/run.sh
== open the CarLife app on the phone and confirm the connection
== the phone may be plugged in before or after this script starts
== unplugging and replugging the phone rebuilds the link automatically
== Ctrl-C stops everything

EOF

start server "$SERVER"
server_pid=$LAST_PID
PIDS=("$server_pid")

while :; do
    start bridge "$BRIDGE"
    bridge_pid=$LAST_PID
    # The channel number is left to carlife-host's own default; see above.
    start host "$HOST"
    host_pid=$LAST_PID
    PIDS=("$server_pid" "$bridge_pid" "$host_pid")

    # The bridge exits when the USB link drops (phone unplugged); the host exits when the
    # bridge it depends on is gone. carlifed exiting is a real failure, so stop everything.
    while :; do
        sleep 1
        if ! kill -0 "$server_pid" 2>/dev/null; then
            echo "== carlifed exited, stopping" >&2
            exit 1
        fi
        if ! kill -0 "$bridge_pid" 2>/dev/null || ! kill -0 "$host_pid" 2>/dev/null; then
            break
        fi
    done

    echo "== bridge or host exited, rebuilding the link in 2 s (Ctrl-C to stop)"
    kill "$bridge_pid" "$host_pid" 2>/dev/null || true
    wait "$bridge_pid" 2>/dev/null || true
    wait "$host_pid" 2>/dev/null || true
    PIDS=("$server_pid")
    sleep 2
done