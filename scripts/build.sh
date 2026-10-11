#!/usr/bin/env bash
#
# Build every CarLifePort component.
#
#   ./scripts/build.sh            incremental build
#   ./scripts/build.sh --force    rebuild the native library as well
#   ./scripts/build.sh --native   build only the native library (host/Makefile uses this)
#
set -euo pipefail

FORCE=0
NATIVE_ONLY=0
for arg in "$@"; do
    case "$arg" in
        --force) FORCE=1 ;;
        --native) NATIVE_ONLY=1 ;;
        *) echo "unknown argument: $arg" >&2; exit 2 ;;
    esac
done

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

step() { printf '\n\033[1;36m==> %s\033[0m\n' "$1"; }

# ------------------------------------------------------------ native library
# macOS (aarch64) native dependencies:
#   1. protobuf 2.5.0 static library (serialization dependency of CarLifeVehicleLib)
#   2. libcarlifevehicle.dylib (CarLife head-unit protocol stack)
# Sources and artifacts land in .build/ and are not tracked by git. Building the
# library is idempotent: an existing dylib is kept unless --force is given.
step "1/5 native library (protobuf 2.5.0 + libcarlifevehicle.dylib)"

BUILD="$ROOT/.build"
PB_SRC="$BUILD/protobuf-2.5.0"
PB_PREFIX="$BUILD/pb"
LIB_SRC="$ROOT/vendor/carlife/CarLife-Vehicle-Lib/LibSource"
LIB_OUT="$BUILD/lib"
JOBS="$(sysctl -n hw.ncpu 2>/dev/null || echo 4)"

mkdir -p "$BUILD" "$PB_PREFIX" "$LIB_OUT"

# ---------------------------------------------------------------- protobuf
if [ ! -d "$PB_SRC" ]; then
    echo "== extracting protobuf 2.5.0"
    tar xzf "$ROOT/vendor/carlife/CarLife-Vehicle-Lib/protobuf-2.5.0.tar.gz" -C "$BUILD"
fi

# protobuf 2.5.0 predates Apple Silicon, so its platform macros cannot identify
# arm64. Adding the aarch64 branch lets every other source compile unchanged.
python3 - "$PB_SRC/src/google/protobuf/stubs/platform_macros.h" <<'PY'
import pathlib, sys
path = pathlib.Path(sys.argv[1])
source = path.read_text()
if '__aarch64__' not in source:
    source = source.replace(
        '#elif defined(__MIPSEL__)',
        '#elif defined(__aarch64__) || defined(__arm64__)\n'
        '#define GOOGLE_PROTOBUF_ARCH_ARM 1\n'
        '#define GOOGLE_PROTOBUF_ARCH_64_BIT 1\n'
        '#elif defined(__MIPSEL__)',
        1,
    )
    path.write_text(source)
    print("== patched protobuf platform macros for arm64")
PY

if [ ! -f "$PB_PREFIX/lib/libprotobuf.a" ]; then
    echo "== building protobuf (logs: .build/protobuf-*.log)"
    (
        cd "$PB_SRC"
        # configure probes runtime capabilities with conftest. A leftover file or
        # directory of that name from an interrupted run makes the linker fail to
        # produce the test binaries, so every probe (pthread included) reports no.
        rm -rf conftest conftest.dir conftest.c conftest.cpp conftest.o conftest.err
        # This is a native build, so do not pass --host. autoconf would treat
        # host != build as a cross build and skip runtime capability probes such
        # as pthread, which makes common.cc fail with #error.
        ./configure --disable-shared --enable-static \
            CC=clang CXX=clang++ \
            CFLAGS="-O2 -w" CXXFLAGS="-std=gnu++98 -O2 -w" \
            --prefix="$PB_PREFIX" > "$BUILD/protobuf-configure.log" 2>&1
        grep -q '^#define HAVE_PTHREAD 1' config.h || {
            echo "protobuf pthread probe failed, see .build/protobuf-configure.log" >&2
            exit 1
        }
        make -j"$JOBS" > "$BUILD/protobuf-make.log" 2>&1
        make install > "$BUILD/protobuf-install.log" 2>&1
    )
fi
echo "== protobuf ready: $PB_PREFIX/lib/libprotobuf.a"

# ------------------------------------------------------- libcarlifevehicle
if [ -f "$LIB_OUT/libcarlifevehicle.dylib" ] && [ "$FORCE" != 1 ]; then
    echo "== libcarlifevehicle already built, skipping (--force rebuilds it)"
    ls -lh "$LIB_OUT/libcarlifevehicle.dylib"
else
    INCLUDES=(
        -I"$LIB_SRC/include"
        -I"$LIB_SRC/protobuf/cc/include"
        -I"$PB_PREFIX/include"
        -I"$PB_PREFIX/include/google/protobuf"
        -I"$PB_PREFIX/include/google/protobuf/io"
    )

    OBJ_DIR="$BUILD/obj"
    rm -rf "$OBJ_DIR" && mkdir -p "$OBJ_DIR"
    OBJECTS=()
    while IFS= read -r src; do
        [ -n "$src" ] || continue
        base="${src#"$LIB_SRC"/}"
        name="${base//\//_}"
        name="${name%.cpp}"
        name="${name%.cc}"
        obj="$OBJ_DIR/$name.o"
        clang++ -std=gnu++98 -O2 -w -fPIC -c "${INCLUDES[@]}" "$src" -o "$obj"
        OBJECTS+=("$obj")
    done < <(
        find "$LIB_SRC/core" "$LIB_SRC/libwrapper" "$LIB_SRC/modules" \
             "$LIB_SRC/utility" "$LIB_SRC/protobuf/cc/src" \
             \( -name '*.cpp' -o -name '*.cc' \) | sort
    )
    echo "== compiled ${#OBJECTS[@]} source files"

    clang++ -dynamiclib -o "$LIB_OUT/libcarlifevehicle.dylib" "${OBJECTS[@]}" \
        -L"$PB_PREFIX/lib" -lprotobuf -lpthread
    echo "== linked: $LIB_OUT/libcarlifevehicle.dylib"
    ls -lh "$LIB_OUT/libcarlifevehicle.dylib"
fi

if [ "$NATIVE_ONLY" = 1 ]; then
    exit 0
fi

# ------------------------------------------------------------------- others
step "2/5 USB bridge"
make -C tools/usb-bridge

step "3/5 protocol stack host"
make -C host

step "4/5 Go backend"
(cd server && go build -o carlifed .)

step "5/5 web frontend (vendor/playport, built unmodified)"
CLIENT="vendor/playport/web"
if [ ! -d "$CLIENT/node_modules" ]; then
    echo "first build, installing web dependencies..."
    # --no-package-lock keeps the submodule pristine: npm would otherwise rewrite
    # vendor/playport/web/package-lock.json.
    (cd "$CLIENT" && npm install --no-package-lock)
fi
(cd "$CLIENT" && npm run build)

printf '\n\033[1;32mbuild complete\033[0m\n'
printf '  bridge   %s\n' "tools/usb-bridge/carlife-usb-bridge"
printf '  host     %s\n' "host/carlife-host"
printf '  backend  %s\n' "server/carlifed"
printf '  web      %s\n' "$CLIENT/dist"
printf '\nrun ./scripts/run.sh to start everything\n'
