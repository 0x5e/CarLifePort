#!/usr/bin/env bash
#
# 一键构建 CarLifePort 全部组件。
#
#   ./scripts/build.sh            增量构建
#   ./scripts/build.sh --force    强制重建原生库
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

step() { printf '\n\033[1;36m==> %s\033[0m\n' "$1"; }

step "1/5 原生库（protobuf 2.5.0 + libcarlifevehicle.dylib）"
./scripts/build-native.sh "$@"

step "2/5 USB 桥接层"
make -C tools/usb-bridge

step "3/5 车机协议栈宿主"
make -C host

step "4/5 Go 后端"
(cd server && go build -o carlifed .)

step "5/5 前端"
if [ ! -d web/node_modules ]; then
    echo "首次构建，安装前端依赖…"
    (cd web && npm install)
fi
(cd web && npm run build)

printf '\n\033[1;32m构建完成\033[0m\n'
printf '  桥接层  %s\n' "tools/usb-bridge/carlife-usb-bridge"
printf '  宿主    %s\n' "host/carlife-host"
printf '  后端    %s\n' "server/carlifed"
printf '  前端    %s\n' "web/dist"
printf '\n用 ./scripts/run.sh 启动全部进程\n'
