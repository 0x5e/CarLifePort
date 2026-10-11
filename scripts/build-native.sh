#!/usr/bin/env bash
#
# 构建 macOS(aarch64) 原生依赖：
#   1. protobuf 2.5.0 静态库（CarLifeVehicleLib 的序列化依赖）
#   2. libcarlifevehicle.dylib（CarLife 车机侧协议栈）
#
# 产物落在 .build/ 下，不进入版本库。
#
set -euo pipefail

FORCE=0
for arg in "$@"; do
    case "$arg" in
        --force) FORCE=1 ;;
        *) echo "未知参数：$arg" >&2; exit 2 ;;
    esac
done

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="$ROOT/.build"
PB_SRC="$BUILD/protobuf-2.5.0"
PB_PREFIX="$BUILD/pb"
LIB_SRC="$ROOT/vendor/carlife/CarLife-Vehicle-Lib/LibSource"
LIB_OUT="$BUILD/lib"
JOBS="$(sysctl -n hw.ncpu 2>/dev/null || echo 4)"

mkdir -p "$BUILD" "$PB_PREFIX" "$LIB_OUT"

# ---------------------------------------------------------------- protobuf
if [ ! -d "$PB_SRC" ]; then
    echo "== 解包 protobuf 2.5.0"
    tar xzf "$ROOT/vendor/carlife/CarLife-Vehicle-Lib/protobuf-2.5.0.tar.gz" -C "$BUILD"
fi

# protobuf 2.5.0 发布时还没有 Apple Silicon，平台宏识别不出 arm64，
# 补齐 aarch64 分支后即可原样编译。
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
    print("== 已为 protobuf 补上 arm64 平台宏")
PY

if [ ! -f "$PB_PREFIX/lib/libprotobuf.a" ]; then
    echo "== 构建 protobuf（日志：.build/protobuf-*.log）"
    (
        cd "$PB_SRC"
        # configure 用 conftest 探测运行时能力；上一次中断留下的同名文件或目录会让
        # 链接器无法产出测试程序，导致 pthread 之类的探测全部误判为「否」。
        rm -rf conftest conftest.dir conftest.c conftest.cpp conftest.o conftest.err
        # 本机编译，不能传 --host：autoconf 会把 host 与 build 不等判成交叉编译，
        # 进而跳过 pthread 等运行时能力探测，导致 common.cc 直接 #error。
        ./configure --disable-shared --enable-static \
            CC=clang CXX=clang++ \
            CFLAGS="-O2 -w" CXXFLAGS="-std=gnu++98 -O2 -w" \
            --prefix="$PB_PREFIX" > "$BUILD/protobuf-configure.log" 2>&1
        grep -q '^#define HAVE_PTHREAD 1' config.h || {
            echo "protobuf 的 pthread 探测失败，见 .build/protobuf-configure.log" >&2
            exit 1
        }
        make -j"$JOBS" > "$BUILD/protobuf-make.log" 2>&1
        make install > "$BUILD/protobuf-install.log" 2>&1
    )
fi
echo "== protobuf 就绪：$PB_PREFIX/lib/libprotobuf.a"

# ------------------------------------------------------- libcarlifevehicle
if [ -f "$LIB_OUT/libcarlifevehicle.dylib" ] && [ "$FORCE" != 1 ]; then
    echo "== libcarlifevehicle 已存在，跳过编译（--force 可强制重建）"
    ls -lh "$LIB_OUT/libcarlifevehicle.dylib"
    exit 0
fi

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
echo "== 编译完成，共 ${#OBJECTS[@]} 个源文件"

clang++ -dynamiclib -o "$LIB_OUT/libcarlifevehicle.dylib" "${OBJECTS[@]}" \
    -L"$PB_PREFIX/lib" -lprotobuf -lpthread
echo "== 链接完成：$LIB_OUT/libcarlifevehicle.dylib"
ls -lh "$LIB_OUT/libcarlifevehicle.dylib"
