#!/usr/bin/env bash
#
# Build every CarLifePort component.
#
#   ./scripts/build.sh            incremental build
#   ./scripts/build.sh --force    rebuild the native library as well
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

step() { printf '\n\033[1;36m==> %s\033[0m\n' "$1"; }

step "1/5 native library (protobuf 2.5.0 + libcarlifevehicle.dylib)"
./scripts/build-native.sh "$@"

step "2/5 USB bridge"
make -C tools/usb-bridge

step "3/5 protocol stack host"
make -C host

step "4/5 Go backend"
(cd server && go build -o carlifed .)

step "5/5 web frontend"
if [ ! -d web/node_modules ]; then
    echo "first build, installing web dependencies..."
    (cd web && npm install)
fi
(cd web && npm run build)

printf '\n\033[1;32mbuild complete\033[0m\n'
printf '  bridge   %s\n' "tools/usb-bridge/carlife-usb-bridge"
printf '  host     %s\n' "host/carlife-host"
printf '  backend  %s\n' "server/carlifed"
printf '  web      %s\n' "web/dist"
printf '\nrun ./scripts/run.sh to start everything\n'
