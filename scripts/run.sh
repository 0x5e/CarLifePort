#!/usr/bin/env bash
#
# 一键启动 CarLifePort 全部进程，Ctrl-C 停止全部。
#
#   ./scripts/run.sh
#
# 启动顺序：carlife-usb-bridge → carlife-host → carlifed
# carlife-host 会等待桥接层的 7200 端口就绪后才建立通道，因此无需手工等待。
# 任一进程退出时，其余进程一并停止。
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

BRIDGE="tools/usb-bridge/carlife-usb-bridge"
HOST="host/carlife-host"
SERVER="server/carlifed"
WEB="web/dist"

# ---------------------------------------------------------------- 前置检查
for f in "$BRIDGE" "$HOST" "$SERVER"; do
    [ -x "$f" ] || { echo "缺少 $f，请先运行 ./scripts/build.sh" >&2; exit 1; }
done
[ -f "$WEB/index.html" ] || { echo "缺少 $WEB，请先运行 ./scripts/build.sh" >&2; exit 1; }

if pgrep -x WeChat >/dev/null 2>&1; then
    cat >&2 <<'EOF'
检测到桌面版微信正在运行。

微信会自动对 Android 手机执行 AOA 握手并独占整个 USB 设备，此时 libusb
无法 claim 设备（LIBUSB_ERROR_ACCESS），adb kill-server 也无效。

请先退出微信，再重新运行本脚本。
EOF
    exit 1
fi

# ------------------------------------------------------------ USB 模式复位
# accessory 模式跨插拔粘住，只能用 adb 清除；所以先清模式，再停 adb server。
if command -v adb >/dev/null 2>&1; then
    echo "== 复位手机 USB 模式"
    if adb shell svc usb setFunctions >/dev/null 2>&1; then
        echo "   已清除 accessory 模式"
    else
        echo "   手机未连接或未授权，跳过（插上手机后重跑本脚本）"
    fi
    adb kill-server >/dev/null 2>&1 || true
else
    echo "== 未找到 adb，跳过 USB 模式复位" >&2
fi

# -------------------------------------------------------------------- 启动
PIDS=()
WATCH_PID=""

stop_all() {
    trap - INT TERM EXIT
    echo
    echo "== 停止全部进程"
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
    printf '== 启动 %-7s pid %s\n' "$name" "$pid"
}

echo
start bridge "$BRIDGE"
start host   "$HOST"
start server "$SERVER"

# 任一进程退出即整体停止，避免 bridge 起不来时脚本静默卡住。
watch() {
    while :; do
        sleep 1
        for pid in "${PIDS[@]}"; do
            if ! kill -0 "$pid" 2>/dev/null; then
                echo "== 检测到进程 $pid 已退出，停止全部"
                kill -TERM $$ 2>/dev/null || true
                return
            fi
        done
    done
}
watch &
WATCH_PID=$!

cat <<'EOF'

== 浏览器打开 http://127.0.0.1:8080
== 手机端打开 CarLife App 并确认连接
== Ctrl-C 停止全部

EOF

wait
