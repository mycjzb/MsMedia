#!/usr/bin/env bash
# MsMedia 唯一标准编译入口。
# by: Long <mycjzb@gmail.com>
#
# 用法：
#   ./build.sh              # 默认：带调试信息，不启用 sanitizer
#   ./build.sh debug        # 调试；编译器支持时加 ASan/UBSan
#   ./build.sh release      # -O2 -DNDEBUG，并 strip 库与测试程序
#   ./build.sh release upx  # 先做 release，再用 pack 压缩可执行文件（绝不压缩库）
#   ./build.sh release-upx  # 同上别名
#   ./build.sh clean
#   ./build.sh test         # 可选：按 debug 编译并运行 tests/test_core
#
# 产物（相对本脚本所在目录）：
#   build/<default|debug|release>/libmsmedia.a
#   build/<mode>/test_core
#   build/<mode>/ms_ts_inspect   # 开发用 TS 检查器，不链 FFmpeg；非正式验收入口
#
# 依赖：C11 编译器（CC，默认 cc）、ar、ranlib；debug sanitizer 需编译器支持。
# release upx 只调用 /usr/local/bin/pack，其次 /home/long/bin/pack；没有 pack 则失败。
# 不对 .a / .so 做 UPX。
#
# 正式 C 发版应在 CentOS 7 隔离容器中执行（例如 ms-build-c7:latest）：
#   docker run --rm -v "$PWD:/src" -w /src ms-build-c7:latest ./build.sh release
# 本脚本在宿主机上的编译只供开发自检，不能代替四平台或 el7 正式验收。
# 核心链接行不加入网络库、TLS 库或解码库。

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT"

usage() {
  cat <<'USAGE'
用法: ./build.sh [default|debug|release|clean|test] [upx]
  ./build.sh
  ./build.sh debug
  ./build.sh release
  ./build.sh release upx
  ./build.sh clean
USAGE
}

MODE="${1:-default}"
EXTRA="${2:-}"

if [[ "$MODE" == "release-upx" ]]; then
  MODE="release"
  EXTRA="upx"
fi

case "$MODE" in
  default|debug|release|clean|test) ;;
  -h|--help|help)
    usage
    exit 0
    ;;
  *)
    echo "error: unknown target: $MODE" >&2
    usage >&2
    exit 1
    ;;
esac

if [[ "$MODE" == "clean" ]]; then
  rm -rf "$ROOT/build"
  echo "cleaned $ROOT/build"
  exit 0
fi

DO_UPX=0
if [[ -n "$EXTRA" ]]; then
  if [[ "$MODE" != "release" || "$EXTRA" != "upx" ]]; then
    echo "error: only './build.sh release upx' accepts a second argument" >&2
    exit 1
  fi
  DO_UPX=1
fi

if [[ "$MODE" == "test" ]]; then
  MODE="debug"
  RUN_TEST=1
else
  RUN_TEST=0
fi

CC="${CC:-cc}"
AR="${AR:-ar}"
RANLIB="${RANLIB:-ranlib}"
STRIP="${STRIP:-strip}"

OUT="$ROOT/build/$MODE"
mkdir -p "$OUT/obj" "$OUT/obj/core" "$OUT/obj/container" "$OUT/obj/tests"

cflags_common=(
  -std=c11
  -Wall
  -Wextra
  -Werror
  -Wshadow
  -Wstrict-prototypes
  -Wmissing-prototypes
  -fno-common
  -I"$ROOT/include"
  -I"$ROOT/src/core"
  -I"$ROOT/src/codec"
  -I"$ROOT/src/container"
)

SAN_CFLAGS=()
SAN_LDFLAGS=()

case "$MODE" in
  default)
    cflags_common+=(-O0 -g)
    ;;
  debug)
    cflags_common+=(-O0 -g -fno-omit-frame-pointer)
    # 探测 sanitizer；不支持则仍完成 debug 构建并注明。
    if echo 'int main(void){return 0;}' | "$CC" -fsanitize=address,undefined -x c - -o "$OUT/.san_probe" >/dev/null 2>&1; then
      SAN_CFLAGS=(-fsanitize=address,undefined)
      SAN_LDFLAGS=(-fsanitize=address,undefined)
      echo "debug: ASan/UBSan enabled ($CC)"
    else
      echo "debug: ASan/UBSan not available with $CC; building without sanitizers"
    fi
    rm -f "$OUT/.san_probe"
    ;;
  release)
    cflags_common+=(-O2 -DNDEBUG)
    ;;
esac

SOURCES=(
  src/core/ms_crc32.c
  src/core/ms_util.c
  src/core/ms_registry.c
  src/core/ms_session.c
  src/codec/ms_adts.c
  src/codec/ms_h264.c
  src/container/ms_ts.c
)

objs=()
for src in "${SOURCES[@]}"; do
  base="${src#src/}"
  obj="$OUT/obj/${base%.c}.o"
  mkdir -p "$(dirname "$obj")"
  "$CC" "${cflags_common[@]}" "${SAN_CFLAGS[@]+"${SAN_CFLAGS[@]}"}" -c "$ROOT/$src" -o "$obj"
  objs+=("$obj")
done

LIB="$OUT/libmsmedia.a"
"$AR" rcs "$LIB" "${objs[@]}"
"$RANLIB" "$LIB"

TEST_SRC=(
  tests/ms_ts_synth.c
  tests/test_core.c
)
test_objs=()
for src in "${TEST_SRC[@]}"; do
  obj="$OUT/obj/${src%.c}.o"
  mkdir -p "$(dirname "$obj")"
  "$CC" "${cflags_common[@]}" "${SAN_CFLAGS[@]+"${SAN_CFLAGS[@]}"}" -c "$ROOT/$src" -o "$obj"
  test_objs+=("$obj")
done

TEST_BIN="$OUT/test_core"
"$CC" "${cflags_common[@]}" "${SAN_LDFLAGS[@]+"${SAN_LDFLAGS[@]}"}" -o "$TEST_BIN" "${test_objs[@]}" "$LIB"

# 开发用检查器：只链产品静态库与公共头，不链 FFmpeg / 解码库。
INSPECT_SRC="$ROOT/tests/ms_ts_inspect.c"
INSPECT_BIN="$OUT/ms_ts_inspect"
"$CC" "${cflags_common[@]}" "${SAN_CFLAGS[@]+"${SAN_CFLAGS[@]}"}"   -I"$ROOT/tests" -c "$INSPECT_SRC" -o "$OUT/obj/tests/ms_ts_inspect.o"
"$CC" "${cflags_common[@]}" "${SAN_LDFLAGS[@]+"${SAN_LDFLAGS[@]}"}"   -o "$INSPECT_BIN" "$OUT/obj/tests/ms_ts_inspect.o" "$LIB"

if [[ "$MODE" == "release" ]]; then
  "$STRIP" "$LIB" "$TEST_BIN" "$INSPECT_BIN"
fi

if [[ "$DO_UPX" -eq 1 ]]; then
  PACK=""
  if [[ -x /usr/local/bin/pack ]]; then
    PACK=/usr/local/bin/pack
  elif [[ -x /home/long/bin/pack ]]; then
    PACK=/home/long/bin/pack
  else
    echo "error: './build.sh release upx' requires /usr/local/bin/pack (fallback: /home/long/bin/pack)" >&2
    echo "error: libraries are never packed; pack was not found, so executables were not compressed" >&2
    exit 1
  fi
  echo "packing executable with $PACK: $TEST_BIN"
  "$PACK" "$TEST_BIN"
fi

echo "built $LIB"
echo "built $TEST_BIN"
echo "built $INSPECT_BIN"
if command -v "$CC" >/dev/null 2>&1; then
  echo "compiler: $CC ($("$CC" --version 2>/dev/null | head -n 1 || echo unknown))"
fi
ls -l "$LIB" "$TEST_BIN" "$INSPECT_BIN"

if [[ "$RUN_TEST" -eq 1 ]]; then
  echo "running $TEST_BIN"
  "$TEST_BIN"
fi
