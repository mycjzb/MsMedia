#!/usr/bin/env bash
# MsMedia 唯一标准编译入口。
# by: Long <mycjzb@gmail.com>
#
# 用法：
#   ./build.sh              # 默认：带调试信息，不启用 sanitizer
#   ./build.sh debug        # 调试；编译器支持时加 ASan/UBSan
#   ./build.sh release      # -O2 -DNDEBUG，并 strip 共享库与测试程序（基线，勿被 size 覆盖）
#   ./build.sh release-size # -Os --enable-small / section-gc / strip；独立 prefix 与 build/release-size
#   ./build.sh test-size    # 按 release-size 编译并跑 test_core / test_ts_scan / test_net / test_ff_backend / test_hw_backend
#   ./build.sh release upx  # 先做 release，再用 pack 压缩可执行文件（绝不压缩库）
#   ./build.sh release-upx  # 同上别名
#   ./build.sh clean
#   ./build.sh test         # 可选：按 debug 编译并运行 tests/test_core（及 test_net / test_ff_backend）
#   ./build.sh openssl      # 构建钉选的官方 OpenSSL 3.5.9 LTS 静态裁剪前缀（不自动升级）
#   ./build.sh ffmpeg       # 构建钉选 OpenSSL + 官方 FFmpeg 9.1 裁剪前缀，并编可选 libmsff + test_ff_backend
#   ./build.sh android-hw   # 非正式 NDK r29 API21+ v7a+v8a 交叉编译 libmshw 与验证 App（非实机硬解/出声验收）
#   ./build.sh android-hw-x86_64  # 非正式测试用 x86_64 APK（build/android-hw-x86_64；不覆盖默认双 ABI APK）
#   ./build.sh release --out-dir=build/release-audit-YYYYMMDD [--test]
#   ./build.sh release --opt=O3 --out-dir=build/release-audit-YYYYMMDD-o3 [--test]
#   ./build.sh release-size --out-dir=build/release-size-audit-YYYYMMDD [--test]
#   MS_BUILD_OUT=build/dir ./build.sh release   # 与 --out-dir 相同，--out-dir 优先
#   --out-dir / MS_BUILD_OUT 必须落在本树 build/ 子目录；默认 ./build.sh release 仍写 build/release。
#   --opt=O3 不得写进默认 build/release（保持历史 -O2 基线）。默认 release 仍是 -O2。
#   --test 可附加在 debug/release/release-size 上跑同一套官方测试（含 1838 旧头）。
#
# 产物（相对本脚本所在目录）：
#   build/<default|debug|release>/libmsmedia.a
#   build/<mode>/libmsmedia.so          # 真实共享库（符号链接 -> .so.1 -> .so.1.0.0）
#   build/<mode>/libmsmedia.so.1
#   build/<mode>/libmsmedia.so.1.0.0
#   build/<mode>/test_core              # 仍链静态库，不因 .so 关闭测试
#   build/<mode>/ms_ts_inspect   # 开发用 TS 检查器，不链 FFmpeg；非正式验收入口
#   build/<mode>/ms_bmff_inspect # 开发用 BMFF 箱体检查器，不链 FFmpeg；非正式验收
#   build/<mode>/ms_so_abi_smoke # 链接 .so 的公共 ABI 冒烟；即便 debug 也不加 sanitizer
#   build/<mode>/test_abi_old_header # 1838 旧头独立客户端动态链当前 .so；debug 加 ASan/UBSan + 金丝雀
#   build/<mode>/libmsnet.a / libmsnet.so*  # 可选 HTTP/HTTPS 网络库（探测到 OpenSSL 且未设 ENABLE_NET=0）
#   build/<mode>/test_net                   # 受控本机 HTTP/HTTPS + HLS 取流自测
#   build/<mode>/net-ca/                    # 测试临时 CA，不入库
#   build/openssl-3.5.9/prefix              # 官方 3.5.9 LTS 静态裁剪（不入库；见 third_party/openssl-3.5.9.pin）
#   build/ffmpeg-9.1/prefix                 # 官方 9.1 裁剪静态库（不入库；见 third_party/ffmpeg-9.1.pin）
#   build/<mode>/libmsff.a / libmsff.so*    # 可选 FFmpeg 后端（探测到钉选前缀且未设 ENABLE_FFMPEG=0）
#   build/<mode>/test_ff_backend            # 本地 TS/MP4/FLV + 受控 HTTP/HLS/HTTPS 自测
#   build/<mode>/libmshw.a / libmshw.so*    # 可选硬解会话（宿主为 mock；Android 见 ./build.sh android-hw）
#   build/<mode>/test_hw_backend            # 宿主状态机/反压/flush/引用释放；不是设备硬解
#   build/<mode>/test_hw_vaapi              # Linux VAAPI 码流 profile 解析 + DRM 探测；不是出画
#   build/<mode>/libmsass.a / libmsass.so*  # 可选完整 ASS 栅格（隔离 libass 前缀，不链入 libmsmedia/libmsff）
#   build/<mode>/test_ass_render            # 宿主多时间点像素证据；不是设备视觉验收
#   build/<mode>/libmsai.a / libmsai.so*    # 可选 AI 字幕插件（无模型，默认禁用）
#   build/<mode>/test_ai_sub                # 受控假供应商闭环；不是真实识别验收
#   build/ass-deps/prefix                   # 钉选 FreeType/HarfBuzz/FriBidi/libass 静态前缀（不入库）
# 共享库用独立 -fPIC 目标文件链接，不是把 .a 改后缀。.so 只导出公共 ABI。
# 宿主 Linux ELF 不能冒充 Android NDK ABI。release 对 .so 做 strip --strip-unneeded。
#
# 依赖：C11 编译器（CC，默认 cc）、ar、ranlib；debug sanitizer 需编译器支持。
# 可选网络库另需系统 OpenSSL 开发头与 libssl/libcrypto（不链入 libmsmedia）。
# 关闭开关：ENABLE_NET=0 ./build.sh … 即使本机有 OpenSSL 也不构建/链接 libmsnet。
# release upx 只调用 /usr/local/bin/pack，其次 /home/long/bin/pack；没有 pack 则失败。
# 不对 .a / .so 做 UPX。
#
# 正式 C 发版应在 CentOS 7 隔离容器中执行（例如 ms-build-c7:latest）：
#   docker run --rm -v "$PWD:/src" -w /src ms-build-c7:latest ./build.sh release
# 本脚本在宿主机上的编译只供开发自检，不能代替四平台或 el7 正式验收。
# 核心 libmsmedia 链接行仍不加入网络库、TLS 库或 FFmpeg。
# 可选 libmsff 静态链入本树钉选的 FFmpeg 9.1 裁剪库与钉选 OpenSSL 3.5.9（PIC .a），
# 并动态链系统 libxml2 与 zlib。最终 libmsff.so 不得 NEEDED 系统 libssl/libcrypto。
# 核心 libmsmedia 仍不带 SSL。旧 libmsnet 若构建，仍动态链系统 OpenSSL，仅作对照。
# 关闭开关：ENABLE_FFMPEG=0 ./build.sh … 不构建 libmsff。不得链系统预装 libav。
# 可选 libmsass 静态链入本树钉选的 libass + FreeType(FTL) + HarfBuzz + FriBidi（隔离前缀），
# 不改变顶层许可证，不链入 libmsmedia / libmsff。关闭：ENABLE_ASS_RENDER=0。
# 若前缀不存在且未显式 ENABLE_ASS_RENDER=1，则跳过（不假装完整渲染已内置）。

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT"

# 为本机 test_net / test_ff 生成 1 天临时 CA（只写当前 OUT，不入库）。
# 显式 basicConstraints=CA:TRUE，不依赖 openssl.cnf 默认扩展；调用系统
# /usr/bin/openssl 时清掉钉选前缀的 OPENSSL_CONF。不改变产品 TLS 校验。
ms_gen_test_ca() {
  local dir="$1"
  local ossl=(env -u OPENSSL_CONF -u OPENSSL_MODULES /usr/bin/openssl)
  mkdir -p "$dir"
  if [[ -f "$dir/ca.crt" && -f "$dir/server.crt" && -f "$dir/wrong.crt" && -f "$dir/server-ecdsa.crt" ]]; then
    if "${ossl[@]}" verify -CAfile "$dir/ca.crt" "$dir/server.crt" >/dev/null 2>&1 &&
       "${ossl[@]}" verify -CAfile "$dir/ca.crt" "$dir/wrong.crt" >/dev/null 2>&1 &&
       "${ossl[@]}" verify -CAfile "$dir/ca.crt" "$dir/server-ecdsa.crt" >/dev/null 2>&1; then
      return 0
    fi
    echo "warning: existing test CA at $dir failed openssl verify; regenerating" >&2
    rm -f "$dir/ca.crt" "$dir/ca.key" "$dir/ca.srl" "$dir/ca.ext" \
      "$dir/server.crt" "$dir/server.key" "$dir/server.csr" "$dir/server.ext" \
      "$dir/wrong.crt" "$dir/wrong.key" "$dir/wrong.csr" "$dir/wrong.ext" \
      "$dir/server-ecdsa.crt" "$dir/server-ecdsa.key" "$dir/server-ecdsa.csr" \
      "$dir/server-ecdsa.ext"
  fi
  # req 没有 -extfile（那是 x509 子命令的选项）；用 -addext 显式写出 CA 约束。
  "${ossl[@]}" req -x509 -newkey rsa:2048 -sha256 -days 1 -nodes \
    -keyout "$dir/ca.key" -out "$dir/ca.crt" \
    -subj "/CN=MsMedia Test CA" \
    -addext "basicConstraints=critical,CA:TRUE" \
    -addext "keyUsage=critical,keyCertSign,cRLSign" \
    -addext "subjectKeyIdentifier=hash" >/dev/null
  "${ossl[@]}" req -newkey rsa:2048 -nodes -sha256 \
    -keyout "$dir/server.key" -out "$dir/server.csr" \
    -subj "/CN=localhost" >/dev/null
  printf '%s\n' "subjectAltName=DNS:localhost,IP:127.0.0.1" > "$dir/server.ext"
  "${ossl[@]}" x509 -req -in "$dir/server.csr" -CA "$dir/ca.crt" \
    -CAkey "$dir/ca.key" -CAcreateserial -out "$dir/server.crt" \
    -days 1 -sha256 -extfile "$dir/server.ext" >/dev/null
  "${ossl[@]}" req -newkey rsa:2048 -nodes -sha256 \
    -keyout "$dir/wrong.key" -out "$dir/wrong.csr" \
    -subj "/CN=wrong.example" >/dev/null
  printf '%s\n' "subjectAltName=DNS:wrong.example" > "$dir/wrong.ext"
  "${ossl[@]}" x509 -req -in "$dir/wrong.csr" -CA "$dir/ca.crt" \
    -CAkey "$dir/ca.key" -CAcreateserial -out "$dir/wrong.crt" \
    -days 1 -sha256 -extfile "$dir/wrong.ext" >/dev/null
  "${ossl[@]}" req -newkey ec -pkeyopt ec_paramgen_curve:P-256 -nodes -sha256 \
    -keyout "$dir/server-ecdsa.key" -out "$dir/server-ecdsa.csr" \
    -subj "/CN=localhost" >/dev/null
  printf '%s\n' "subjectAltName=DNS:localhost,IP:127.0.0.1" > "$dir/server-ecdsa.ext"
  "${ossl[@]}" x509 -req -in "$dir/server-ecdsa.csr" -CA "$dir/ca.crt" \
    -CAkey "$dir/ca.key" -CAcreateserial -out "$dir/server-ecdsa.crt" \
    -days 1 -sha256 -extfile "$dir/server-ecdsa.ext" >/dev/null
  if ! "${ossl[@]}" verify -CAfile "$dir/ca.crt" "$dir/server.crt" >/dev/null ||
     ! "${ossl[@]}" verify -CAfile "$dir/ca.crt" "$dir/wrong.crt" >/dev/null ||
     ! "${ossl[@]}" verify -CAfile "$dir/ca.crt" "$dir/server-ecdsa.crt" >/dev/null; then
    echo "error: test CA at $dir failed openssl verify (leaf vs CA)" >&2
    exit 1
  fi
}

usage() {
  cat <<'USAGE'
用法: ./build.sh [default|debug|release|release-size|test-size|clean|test|openssl|ffmpeg|ffmpeg-cache-selftest|android-hw|android-hw-x86_64|android-hw-x86] [upx]
         [--out-dir DIR | --out-dir=DIR] [--test] [--opt=O3]
  ./build.sh
  ./build.sh debug
  ./build.sh release
  ./build.sh release-size
  ./build.sh test-size
  ./build.sh release upx
  ./build.sh release --out-dir=build/release-audit-YYYYMMDD --test
  ./build.sh release --opt=O3 --out-dir=build/release-audit-YYYYMMDD-o3 --test
  ./build.sh release-size --out-dir=build/release-size-audit-YYYYMMDD --test
  ./build.sh clean
  ./build.sh openssl
  ./build.sh ffmpeg
  ./build.sh ffmpeg-cache-selftest
  ./build.sh android-hw
  ./build.sh android-hw-x86_64
  ./build.sh android-hw-x86
  --out-dir 也可写成环境变量 MS_BUILD_OUT（--out-dir 优先）；必须是本树 build/ 下的子目录。
  --opt=O3 只覆盖本趟优化级别，不改变默认 release -O2，且禁止写入 build/release。
  探测到系统 OpenSSL 且未设 ENABLE_NET=0 时额外构建可选 libmsnet 与 test_net（对照路径）。
  存在钉选 FFmpeg 9.1 与 OpenSSL 3.5.9 前缀且未设 ENABLE_FFMPEG=0 时额外构建可选 libmsff 与 test_ff_backend。
  存在钉选 ass-deps 前缀且未设 ENABLE_ASS_RENDER=0 时额外构建可选 libmsass 与 test_ass_render。
  未设 ENABLE_AI_SUB=0 时额外构建可选 libmsai 与 test_ai_sub（默认禁用，仅假供应商）。
  ./build.sh android-hw-x86_64 只产出测试用 x86_64 APK 到 build/android-hw-x86_64，不覆盖默认 v7a+v8a APK，也不动已有前缀/冻结产物。
  ./build.sh android-hw-x86 只产出测试用 x86（i686）APK 到 build/android-hw-x86，供 API 18/19 x86 AVD，不覆盖默认交付包，也不动已有前缀/冻结产物。
USAGE
}

MODE="${1:-default}"
if [[ $# -gt 0 ]]; then
  shift
fi

if [[ "$MODE" == "release-upx" ]]; then
  MODE="release"
  set -- upx "$@"
fi

OUT_DIR_ARG="${MS_BUILD_OUT:-}"
OPT_OVERRIDE=""
EXPLICIT_TEST=0
DO_UPX=0
while [[ $# -gt 0 ]]; do
  case "$1" in
    upx)
      DO_UPX=1
      ;;
    --test)
      EXPLICIT_TEST=1
      ;;
    --out-dir)
      if [[ $# -lt 2 ]]; then
        echo "error: --out-dir needs a directory" >&2
        exit 1
      fi
      shift
      OUT_DIR_ARG="$1"
      ;;
    --out-dir=*)
      OUT_DIR_ARG="${1#--out-dir=}"
      ;;
    --opt=O3|--opt=o3|--opt=3)
      OPT_OVERRIDE="O3"
      ;;
    --opt=O2|--opt=o2|--opt=2)
      OPT_OVERRIDE="O2"
      ;;
    *)
      echo "error: unknown argument: $1" >&2
      usage >&2
      exit 1
      ;;
  esac
  shift
done

# FFmpeg 前缀缓存决策的隔离自测（不改真实补丁/冻结制品，不编译 FFmpeg）。
if [[ "$MODE" == "ffmpeg-cache-selftest" ]]; then
  env -u CC -u CXX -u LD -u FFMPEG_BUILD_ROOT -u FFMPEG_PREFIX -u FFMPEG_BUILDDIR \
    -u MS_SIZE_OPT -u FFMPEG_REBUILD -u MS_FF_CACHE_CHECK_ONLY -u MS_FF_FAKE_INSTALL \
    -u MS_FF_INJECT_BUILD_FAILURE -u MS_FF_EMIT_EXPECTED \
    PATH="/usr/bin:/bin:$PATH" bash "$ROOT/tests/test_ffmpeg9_prefix_cache.sh"
  exit $?
fi

# openssl：只构建钉选 3.5.9 静态前缀（不自动升级；非正式验收）。
if [[ "$MODE" == "openssl" ]]; then
  env -u CC -u CXX -u LD PATH="/usr/bin:/bin:$PATH" bash "$ROOT/scripts/openssl_build.sh"
  exit $?
fi

# ffmpeg：先确保钉选 OpenSSL + 9.1 前缀，再按 default 编译入口编可选后端（非正式验收）。
if [[ "$MODE" == "ffmpeg" ]]; then
  MODE="default"
  WANT_FFMPEG_EXPLICIT=1
fi

if [[ "$MODE" == "android-hw" ]]; then
  exec env -u CC -u CXX -u LD PATH="/usr/bin:/bin:$PATH" bash "$ROOT/scripts/android_hw_build.sh"
fi
# 模拟器验收用的非正式 x86_64 APK（独立输出目录）。默认交付仍只用 ./build.sh android-hw 的 v7a+v8a。
if [[ "$MODE" == "android-hw-x86_64" ]]; then
  exec env -u CC -u CXX -u LD PATH="/usr/bin:/bin:$PATH" bash "$ROOT/scripts/android_hw_x86_64_build.sh"
fi
# 模拟器验收用的非正式 x86（i686）APK（独立输出目录）。默认交付仍只用 ./build.sh android-hw 的 v7a+v8a。
if [[ "$MODE" == "android-hw-x86" ]]; then
  exec env -u CC -u CXX -u LD PATH="/usr/bin:/bin:$PATH" bash "$ROOT/scripts/android_hw_x86_build.sh"
fi

case "$MODE" in
  default|debug|release|release-size|test-size|clean|test) ;;
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

if [[ "$DO_UPX" -eq 1 && "$MODE" != "release" ]]; then
  echo "error: upx is only accepted with './build.sh release upx'" >&2
  exit 1
fi

if [[ "$MODE" == "test" ]]; then
  MODE="debug"
  RUN_TEST=1
elif [[ "$MODE" == "test-size" ]]; then
  MODE="release-size"
  RUN_TEST=1
else
  RUN_TEST=0
fi
if [[ "$EXPLICIT_TEST" -eq 1 ]]; then
  RUN_TEST=1
fi
if [[ "$OPT_OVERRIDE" == "O3" && "$MODE" == "release-size" ]]; then
  echo "error: --opt=O3 conflicts with release-size / -Os" >&2
  exit 1
fi
if [[ "$OPT_OVERRIDE" == "O3" && "$MODE" != "release" ]]; then
  echo "error: --opt=O3 is only accepted with release" >&2
  exit 1
fi

# 体积优化走独立 prefix 与产物目录，不覆盖已有 build/release 基线。
if [[ "$MODE" == "release-size" ]]; then
  export MS_SIZE_OPT=1
  export OPENSSL_BUILD_ROOT="${OPENSSL_BUILD_ROOT:-$ROOT/build/openssl-3.5.9-size}"
  export OPENSSL_PREFIX="${OPENSSL_PREFIX:-$OPENSSL_BUILD_ROOT/prefix}"
  export FFMPEG_BUILD_ROOT="${FFMPEG_BUILD_ROOT:-$ROOT/build/ffmpeg-9.1-size}"
  export FFMPEG_PREFIX="${FFMPEG_PREFIX:-$FFMPEG_BUILD_ROOT/prefix}"
  echo "release-size: MS_SIZE_OPT=1 openssl=$OPENSSL_PREFIX ffmpeg=$FFMPEG_PREFIX"
fi

CC="${CC:-cc}"
AR="${AR:-ar}"
RANLIB="${RANLIB:-ranlib}"
STRIP="${STRIP:-strip}"

if [[ -n "$OUT_DIR_ARG" ]]; then
  if [[ "$OUT_DIR_ARG" != /* ]]; then
    OUT="$ROOT/${OUT_DIR_ARG#./}"
  else
    OUT="$OUT_DIR_ARG"
  fi
else
  OUT="$ROOT/build/$MODE"
fi
OUT="$(realpath -m "$OUT")"
ROOT_BUILD="$(realpath -m "$ROOT/build")"
if [[ "$OUT" != "$ROOT_BUILD"/* ]]; then
  echo "error: out-dir must be a subdirectory of $ROOT/build (got $OUT)" >&2
  exit 1
fi
RELEASE_DEFAULT="$(realpath -m "$ROOT/build/release")"
if [[ "$OPT_OVERRIDE" == "O3" && "$OUT" == "$RELEASE_DEFAULT" ]]; then
  echo "error: --opt=O3 must use --out-dir or MS_BUILD_OUT; refusing to overwrite build/release" >&2
  exit 1
fi
echo "OUT=$OUT"
mkdir -p "$OUT/obj" "$OUT/obj/core" "$OUT/obj/container" "$OUT/obj/tests" "$OUT/obj/net"

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
    # 默认保持 -O2；HLS 缺失分片 + signal_discontinuity 的 gcc 14.2
    # bt+cmovae 误编译已在源码侧规避（不再全局 -fno-tree-vrp）。
    # --opt=O3 仅本趟覆盖，不得改默认、不得写入 build/release。
    if [[ "$OPT_OVERRIDE" == "O3" ]]; then
      cflags_common+=(-O3 -DNDEBUG)
      echo "release: using -O3 (audit only; default release remains -O2)"
    else
      cflags_common+=(-O2 -DNDEBUG)
    fi
    ;;
  release-size)
    # -Os 必须是本数组里最后一次 -O，避免被前面的 -O2 覆盖。
    cflags_common+=(-Os -DNDEBUG -ffunction-sections -fdata-sections)
    ;;
esac

SOURCES=(
  src/core/ms_crc32.c
  src/core/ms_util.c
  src/core/ms_registry.c
  src/core/ms_session.c
  src/codec/ms_adts.c
  src/codec/ms_h264.c
  src/codec/ms_hevc.c
  src/codec/ms_hdr.c
  src/codec/ms_vbit.c
  src/codec/ms_abit.c
  src/codec/ms_avcc.c
  src/sub/ms_sub.c
  src/container/ms_ts.c
  src/container/ms_ts_scan.c
  src/container/ms_bmff.c
  src/container/ms_bmff_sample.c
  src/container/ms_flv.c
  src/source/ms_source.c
  src/hls/ms_hls.c
  src/dash/ms_dash.c
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

TEST_TS_SCAN="$OUT/test_ts_scan"
"$CC" "${cflags_common[@]}" "${SAN_CFLAGS[@]+"${SAN_CFLAGS[@]}"}" \
  -I"$ROOT/tests" -c "$ROOT/tests/test_ts_scan.c" -o "$OUT/obj/tests/test_ts_scan.o"
"$CC" "${cflags_common[@]}" "${SAN_LDFLAGS[@]+"${SAN_LDFLAGS[@]}"}" \
  -o "$TEST_TS_SCAN" "$OUT/obj/tests/test_ts_scan.o" "$OUT/obj/tests/ms_ts_synth.o" "$LIB"

TEST_HDR="$OUT/test_hdr"
"$CC" "${cflags_common[@]}" "${SAN_CFLAGS[@]+"${SAN_CFLAGS[@]}"}" \
  -I"$ROOT/tests" -c "$ROOT/tests/test_hdr.c" -o "$OUT/obj/tests/test_hdr.o"
"$CC" "${cflags_common[@]}" "${SAN_LDFLAGS[@]+"${SAN_LDFLAGS[@]}"}" \
  -o "$TEST_HDR" "$OUT/obj/tests/test_hdr.o" "$LIB"

# 开发用检查器：只链产品静态库与公共头，不链 FFmpeg / 解码库。
INSPECT_SRC="$ROOT/tests/ms_ts_inspect.c"
INSPECT_BIN="$OUT/ms_ts_inspect"
"$CC" "${cflags_common[@]}" "${SAN_CFLAGS[@]+"${SAN_CFLAGS[@]}"}"   -I"$ROOT/tests" -c "$INSPECT_SRC" -o "$OUT/obj/tests/ms_ts_inspect.o"
"$CC" "${cflags_common[@]}" "${SAN_LDFLAGS[@]+"${SAN_LDFLAGS[@]}"}"   -o "$INSPECT_BIN" "$OUT/obj/tests/ms_ts_inspect.o" "$LIB"

# 开发用 BMFF 箱体检查器：只链产品库，不链 FFmpeg。非正式验收。
BMFF_INSPECT_SRC="$ROOT/tests/ms_bmff_inspect.c"
BMFF_INSPECT_BIN="$OUT/ms_bmff_inspect"
"$CC" "${cflags_common[@]}" "${SAN_CFLAGS[@]+"${SAN_CFLAGS[@]}"}"   -I"$ROOT/tests" -c "$BMFF_INSPECT_SRC" -o "$OUT/obj/tests/ms_bmff_inspect.o"
"$CC" "${cflags_common[@]}" "${SAN_LDFLAGS[@]+"${SAN_LDFLAGS[@]}"}"   -o "$BMFF_INSPECT_BIN" "$OUT/obj/tests/ms_bmff_inspect.o" "$LIB"

# 真实共享库：独立 PIC 编译 + -shared，不是把 .a 改后缀。
# 默认/debug 保留调试符号；release 再 strip --strip-unneeded。
# 链接行不加 FFmpeg、OpenSSL、libpthread 或其它网络/解码库。
SO_REAL="$OUT/libmsmedia.so.1.0.0"
SO_SONAME="$OUT/libmsmedia.so.1"
SO_LINK="$OUT/libmsmedia.so"
MAP="$ROOT/src/core/libmsmedia.map"
pic_objs=()
for src in "${SOURCES[@]}"; do
  base="${src#src/}"
  obj="$OUT/obj-pic/${base%.c}.o"
  mkdir -p "$(dirname "$obj")"
  "$CC" "${cflags_common[@]}" \
    -fPIC -fvisibility=hidden -DMSMEDIA_BUILD_SHARED \
    -c "$ROOT/$src" -o "$obj"
  pic_objs+=("$obj")
done
SO_LDFLAGS=(-shared -Wl,-soname,libmsmedia.so.1 -Wl,--version-script="$MAP" -Wl,-z,defs)
if [[ "$MODE" == "release-size" ]]; then
  SO_LDFLAGS+=(-Wl,--gc-sections)
fi
"$CC" "${cflags_common[@]}" -fPIC -fvisibility=hidden \
  "${SO_LDFLAGS[@]}" \
  -o "$SO_REAL" "${pic_objs[@]}" -lc
ln -sfn "$(basename "$SO_REAL")" "$SO_SONAME"
ln -sfn "$(basename "$SO_SONAME")" "$SO_LINK"
SO_UNSTRIPPED_BYTES=$(wc -c < "$SO_REAL" | tr -d " ")

# 共享库 ABI 冒烟：始终不加 sanitizer，用 rpath 链本目录 .so，测试程序仍链 .a。
SMOKE_SRC="$ROOT/tests/ms_so_abi_smoke.c"
SMOKE_BIN="$OUT/ms_so_abi_smoke"
"$CC" "${cflags_common[@]}" -o "$SMOKE_BIN" "$SMOKE_SRC" \
  -L"$OUT" -lmsmedia -Wl,-rpath,'$ORIGIN' -Wl,-z,defs -lc

# ---------- 可选网络库（不链入 libmsmedia）----------
NET_BUILT=0
NET_A="$OUT/libmsnet.a"
NET_SO="$OUT/libmsnet.so.1.0.0"
TEST_NET=""
OPENSSL_PROBE="$OUT/.ssl_probe"
WANT_NET=1
if [[ "${ENABLE_NET:-auto}" == "0" || "${ENABLE_NET:-}" == "no" ]]; then
  WANT_NET=0
fi
if [[ "$WANT_NET" -eq 1 ]] && echo '#include <openssl/ssl.h>
int main(void){SSL_CTX *c=SSL_CTX_new(TLS_client_method()); if(c) SSL_CTX_free(c); return 0;}' \
  | "$CC" -x c - -o "$OPENSSL_PROBE" -lssl -lcrypto >/dev/null 2>&1; then
  NET_CFLAGS=("${cflags_common[@]}" -I"$ROOT/src/net")
  NET_SRCS=(src/net/ms_tls.c src/net/ms_net.c)
  net_objs=()
  for src in "${NET_SRCS[@]}"; do
    obj="$OUT/obj/${src#src/}"
    obj="${obj%.c}.o"
    mkdir -p "$(dirname "$obj")"
    "$CC" "${NET_CFLAGS[@]}" "${SAN_CFLAGS[@]+"${SAN_CFLAGS[@]}"}" -pthread \
      -c "$ROOT/$src" -o "$obj"
    net_objs+=("$obj")
  done
  "$AR" rcs "$NET_A" "${net_objs[@]}"
  "$RANLIB" "$NET_A"
  net_pic=()
  for src in "${NET_SRCS[@]}"; do
    obj="$OUT/obj-pic/${src#src/}"
    obj="${obj%.c}.o"
    mkdir -p "$(dirname "$obj")"
    "$CC" "${NET_CFLAGS[@]}" -fPIC -fvisibility=hidden -DMSMEDIA_BUILD_SHARED \
      -pthread -c "$ROOT/$src" -o "$obj"
    net_pic+=("$obj")
  done
  "$CC" "${NET_CFLAGS[@]}" -fPIC -fvisibility=hidden -shared -pthread \
    -Wl,-soname,libmsnet.so.1 -Wl,--version-script="$ROOT/src/net/libmsnet.map" \
    -Wl,-rpath,'$ORIGIN' -Wl,-z,defs -o "$NET_SO" "${net_pic[@]}" \
    -L"$OUT" -lmsmedia -lssl -lcrypto -lpthread -lc
  ln -sfn "$(basename "$NET_SO")" "$OUT/libmsnet.so.1"
  ln -sfn "libmsnet.so.1" "$OUT/libmsnet.so"
  NET_SO_UNSTRIPPED=$(wc -c < "$NET_SO" | tr -d " ")
  # 测试临时 CA（只写 build 目录）
  CA_DIR="$OUT/net-ca"
  ms_gen_test_ca "$CA_DIR"
  "$CC" "${cflags_common[@]}" "${SAN_CFLAGS[@]+"${SAN_CFLAGS[@]}"}" -pthread \
    -I"$ROOT/tests" -c "$ROOT/tests/test_net.c" -o "$OUT/obj/tests/test_net.o"
  if [[ ! -f "$OUT/obj/tests/ms_ts_synth.o" ]]; then
    "$CC" "${cflags_common[@]}" "${SAN_CFLAGS[@]+"${SAN_CFLAGS[@]}"}" \
      -I"$ROOT/tests" -c "$ROOT/tests/ms_ts_synth.c" -o "$OUT/obj/tests/ms_ts_synth.o"
  fi
  TEST_NET="$OUT/test_net"
  "$CC" "${cflags_common[@]}" "${SAN_LDFLAGS[@]+"${SAN_LDFLAGS[@]}"}" -pthread \
    -o "$TEST_NET" "$OUT/obj/tests/test_net.o" "$OUT/obj/tests/ms_ts_synth.o" \
    "$NET_A" "$LIB" -lssl -lcrypto -lpthread
  NET_BUILT=1
  echo "optional net: OpenSSL enabled, built $NET_A and $NET_SO (unstripped ${NET_SO_UNSTRIPPED} bytes)"
else
  if [[ "$WANT_NET" -eq 0 ]]; then
    echo "optional net: skipped (ENABLE_NET=0); core remains without OpenSSL"
  else
    echo "optional net: OpenSSL dev not available; skipped libmsnet"
  fi
fi
rm -f "$OPENSSL_PROBE"

# ---------- 可选硬解（不链入 libmsmedia；宿主为 mock）----------
HW_BUILT=0
HW_A="$OUT/libmshw.a"
HW_SO="$OUT/libmshw.so.1.0.0"
TEST_HW=""
WANT_HW=1
if [[ "${ENABLE_HW:-auto}" == "0" || "${ENABLE_HW:-}" == "no" ]]; then
  WANT_HW=0
fi
if [[ "$WANT_HW" -eq 1 ]]; then
  mkdir -p "$OUT/obj/hw" "$OUT/obj-pic/hw"
  HW_CFLAGS=("${cflags_common[@]}" -I"$ROOT/src/hw")
  HW_SRCS=(src/hw/ms_hw_name.c src/hw/ms_hw_csd.c src/hw/ms_hw.c src/hw/ms_hw_mock.c src/hw/ms_hw_vaapi.c src/hw/ms_hw_player.c src/hw/ms_hw_ass_bind.c src/hw/ms_hw_ai_bind.c src/hw/ms_hw_reap.c src/hw/ms_hw_audio.c src/hw/ms_hw_audio_trim.c src/hw/ms_media3_audio_clock.c src/hw/ms_media3_video_release.c)
  hw_objs=()
  for src in "${HW_SRCS[@]}"; do
    obj="$OUT/obj/${src#src/}"
    obj="${obj%.c}.o"
    mkdir -p "$(dirname "$obj")"
    "$CC" "${HW_CFLAGS[@]}" "${SAN_CFLAGS[@]+"${SAN_CFLAGS[@]}"}" -c "$ROOT/$src" -o "$obj"
    hw_objs+=("$obj")
  done
  "$AR" rcs "$HW_A" "${hw_objs[@]}"
  "$RANLIB" "$HW_A"
  hw_pic=()
  for src in "${HW_SRCS[@]}"; do
    obj="$OUT/obj-pic/${src#src/}"
    obj="${obj%.c}.o"
    mkdir -p "$(dirname "$obj")"
    "$CC" "${HW_CFLAGS[@]}" -fPIC -fvisibility=hidden -DMSMEDIA_BUILD_SHARED \
      -c "$ROOT/$src" -o "$obj"
    hw_pic+=("$obj")
  done
  HW_LDFLAGS=(-shared -Wl,-soname,libmshw.so.1 -Wl,--version-script="$ROOT/src/hw/libmshw.map" -Wl,-rpath,'$ORIGIN' -Wl,-z,defs)
  if [[ "$MODE" == "release-size" ]]; then
    HW_LDFLAGS+=(-Wl,--gc-sections)
  fi
  "$CC" "${HW_CFLAGS[@]}" -fPIC -fvisibility=hidden "${HW_LDFLAGS[@]}" \
    -o "$HW_SO" "${hw_pic[@]}" -L"$OUT" -lmsmedia -ldl -lc
  ln -sfn "$(basename "$HW_SO")" "$OUT/libmshw.so.1"
  ln -sfn "libmshw.so.1" "$OUT/libmshw.so"
  HW_SO_UNSTRIPPED=$(wc -c < "$HW_SO" | tr -d " ")
  TEST_HW="$OUT/test_hw_backend"
  "$CC" "${cflags_common[@]}" "${SAN_CFLAGS[@]+"${SAN_CFLAGS[@]}"}" \
    -c "$ROOT/tests/test_hw_backend.c" -o "$OUT/obj/tests/test_hw_backend.o"
  "$CC" "${cflags_common[@]}" "${SAN_CFLAGS[@]+"${SAN_CFLAGS[@]}"}" \
    -I"$ROOT/src/hw" -I"$ROOT/tests" \
    -c "$ROOT/tests/test_hw_audio_trim.c" -o "$OUT/obj/tests/test_hw_audio_trim.o"
  "$CC" "${cflags_common[@]}" "${SAN_LDFLAGS[@]+"${SAN_LDFLAGS[@]}"}" \
    -o "$TEST_HW" "$OUT/obj/tests/test_hw_backend.o" \
    "$OUT/obj/tests/test_hw_audio_trim.o" "$HW_A" "$LIB" -ldl
  
  TEST_MEDIA3_SYNC="$OUT/test_media3_sync"
  "$CC" "${cflags_common[@]}" "${SAN_CFLAGS[@]+"${SAN_CFLAGS[@]}"}" \
    -I"$ROOT/include" -I"$ROOT/src/hw" \
    -c "$ROOT/tests/test_media3_sync.c" -o "$OUT/obj/tests/test_media3_sync.o"
  "$CC" "${cflags_common[@]}" "${SAN_LDFLAGS[@]+"${SAN_LDFLAGS[@]}"}" \
    -o "$TEST_MEDIA3_SYNC" "$OUT/obj/tests/test_media3_sync.o" \
    "$OUT/obj/hw/ms_media3_audio_clock.o" "$OUT/obj/hw/ms_media3_video_release.o"

  TEST_HW_PLAYER="$OUT/test_hw_player"
  if [[ ! -f "$OUT/obj/tests/ms_ts_synth.o" ]]; then
    "$CC" "${cflags_common[@]}" "${SAN_CFLAGS[@]+"${SAN_CFLAGS[@]}"}" \
      -I"$ROOT/tests" -c "$ROOT/tests/ms_ts_synth.c" -o "$OUT/obj/tests/ms_ts_synth.o"
  fi
  "$CC" "${cflags_common[@]}" "${SAN_CFLAGS[@]+"${SAN_CFLAGS[@]}"}" \
    -I"$ROOT/tests" -c "$ROOT/tests/test_hw_player.c" -o "$OUT/obj/tests/test_hw_player.o"
  "$CC" "${cflags_common[@]}" "${SAN_LDFLAGS[@]+"${SAN_LDFLAGS[@]}"}" \
    -o "$TEST_HW_PLAYER" "$OUT/obj/tests/test_hw_player.o" "$OUT/obj/tests/ms_ts_synth.o" "$HW_A" "$LIB" -ldl
  TEST_HW_VAAPI="$OUT/test_hw_vaapi"
  "$CC" "${cflags_common[@]}" "${SAN_CFLAGS[@]+"${SAN_CFLAGS[@]}"}" \
    -I"$ROOT/tests" -c "$ROOT/tests/test_hw_vaapi.c" -o "$OUT/obj/tests/test_hw_vaapi.o"
  "$CC" "${cflags_common[@]}" "${SAN_LDFLAGS[@]+"${SAN_LDFLAGS[@]}"}" \
    -o "$TEST_HW_VAAPI" "$OUT/obj/tests/test_hw_vaapi.o" "$HW_A" "$LIB" -ldl
  HW_BUILT=1
  echo "optional hw: libmshw built $HW_A and $HW_SO (unstripped ${HW_SO_UNSTRIPPED} bytes); host mock + Linux VAAPI parse/probe, not device decode"
else
  echo "optional hw: skipped (ENABLE_HW=0)"
fi

# ---------- 可选 FFmpeg 9.1 后端（不链入 libmsmedia）----------
FF_BUILT=0
FF_A="$OUT/libmsff.a"
FF_SO="$OUT/libmsff.so.1.0.0"
TEST_FF=""
FFMPEG_PREFIX="${FFMPEG_PREFIX:-$ROOT/build/ffmpeg-9.1/prefix}"
OPENSSL_PREFIX="${OPENSSL_PREFIX:-$ROOT/build/openssl-3.5.9/prefix}"
WANT_FF=1
if [[ "${ENABLE_FFMPEG:-auto}" == "0" || "${ENABLE_FFMPEG:-}" == "no" ]]; then
  WANT_FF=0
fi
if [[ "$WANT_FF" -eq 1 ]]; then
  # 始终走 ffmpeg9_build.sh：前缀已存在且 OpenSSL 钉选未变则快速跳过。
  if ! env -u CC -u CXX -u LD PATH="/usr/bin:/bin:$PATH" \
      OPENSSL_PREFIX="$OPENSSL_PREFIX" bash "$ROOT/scripts/ffmpeg9_build.sh"; then
    echo "error: trimmed FFmpeg 9.1 prefix build failed" >&2
    if [[ "${WANT_FFMPEG_EXPLICIT:-0}" == "1" ]]; then
      exit 1
    fi
    WANT_FF=0
  fi
fi
if [[ "$WANT_FF" -eq 1 && -f "$FFMPEG_PREFIX/lib/libavformat.a" ]]; then
  # 只用绝对路径下的钉选静态库，避免链到系统 libavformat.so.61 或系统 libssl。
  if [[ ! -f "$OPENSSL_PREFIX/lib/libssl.a" || ! -f "$OPENSSL_PREFIX/lib/libcrypto.a" ]]; then
    echo "error: pinned OpenSSL static libs missing at $OPENSSL_PREFIX" >&2
    exit 1
  fi
  # 测试 CA：即使 ENABLE_NET=0 也要给 test_ff_backend 用（仍不入库）。
  CA_DIR="${CA_DIR:-$OUT/net-ca}"
  ms_gen_test_ca "$CA_DIR"
  FF_CFLAGS=("${cflags_common[@]}" -I"$ROOT/src/ff" -I"$FFMPEG_PREFIX/include" -I"$OPENSSL_PREFIX/include")
  mkdir -p "$OUT/obj/ff" "$OUT/obj-pic/ff" "$OUT/obj/tests"
  "$CC" "${FF_CFLAGS[@]}" "${SAN_CFLAGS[@]+"${SAN_CFLAGS[@]}"}" -pthread \
    -c "$ROOT/src/ff/ms_ff.c" -o "$OUT/obj/ff/ms_ff.o"
  "$AR" rcs "$FF_A" "$OUT/obj/ff/ms_ff.o"
  "$RANLIB" "$FF_A"
  "$CC" "${FF_CFLAGS[@]}" -fPIC -fvisibility=hidden -DMSMEDIA_BUILD_SHARED \
    -pthread -c "$ROOT/src/ff/ms_ff.c" -o "$OUT/obj-pic/ff/ms_ff.o"
  # 共享库不 NEEDED libmsmedia：隐藏编入扫描器与溢出加法。
  "$CC" "${cflags_common[@]}" -fPIC -fvisibility=hidden -DMSMEDIA_BUILD_SHARED \
    -c "$ROOT/src/ff/ms_ff_scan_support.c" -o "$OUT/obj-pic/ff/ms_ff_scan_support.o"
  # 同一份字幕解析、HEVC 参数集与通用视频配置检查静态编入 libmsff（隐藏符号），避免 NEEDED libmsmedia。
  "$CC" "${cflags_common[@]}" -fPIC -fvisibility=hidden \
    -c "$ROOT/src/sub/ms_sub.c" -o "$OUT/obj-pic/ff/ms_sub.o"
  "$CC" "${cflags_common[@]}" -fPIC -fvisibility=hidden \
    -c "$ROOT/src/codec/ms_hevc.c" -o "$OUT/obj-pic/ff/ms_hevc.o"
  "$CC" "${cflags_common[@]}" -fPIC -fvisibility=hidden \
    -c "$ROOT/src/codec/ms_hdr.c" -o "$OUT/obj-pic/ff/ms_hdr.o"
  "$CC" "${cflags_common[@]}" -fPIC -fvisibility=hidden \
    -c "$ROOT/src/codec/ms_vbit.c" -o "$OUT/obj-pic/ff/ms_vbit.o"
  "$CC" "${cflags_common[@]}" -fPIC -fvisibility=hidden \
    -c "$ROOT/src/codec/ms_abit.c" -o "$OUT/obj-pic/ff/ms_abit.o"
  "$CC" "${cflags_common[@]}" -fPIC -fvisibility=hidden \
    -c "$ROOT/src/codec/ms_avcc.c" -o "$OUT/obj-pic/ff/ms_avcc.o"
  "$CC" "${FF_CFLAGS[@]}" -fPIC -fvisibility=hidden -shared -pthread \
    -Wl,-soname,libmsff.so.1 -Wl,--version-script="$ROOT/src/ff/libmsff.map" \
    -Wl,--gc-sections -Wl,--exclude-libs,ALL -Wl,-z,defs -o "$FF_SO" "$OUT/obj-pic/ff/ms_ff.o" \
    "$OUT/obj-pic/container/ms_ts_scan.o" \
    "$OUT/obj-pic/ff/ms_ff_scan_support.o" \
    "$OUT/obj-pic/ff/ms_sub.o" \
    "$OUT/obj-pic/ff/ms_hevc.o" "$OUT/obj-pic/ff/ms_hdr.o" \
    "$OUT/obj-pic/ff/ms_vbit.o" \
    "$OUT/obj-pic/ff/ms_abit.o" \
    "$OUT/obj-pic/ff/ms_avcc.o" \
    "$FFMPEG_PREFIX/lib/libavformat.a" \
    "$FFMPEG_PREFIX/lib/libavcodec.a" "$FFMPEG_PREFIX/lib/libavutil.a" \
    "$OPENSSL_PREFIX/lib/libssl.a" "$OPENSSL_PREFIX/lib/libcrypto.a" \
    -lxml2 -lz -lm -ldl -lpthread -lc
  ln -sfn "$(basename "$FF_SO")" "$OUT/libmsff.so.1"
  ln -sfn "libmsff.so.1" "$OUT/libmsff.so"
  FF_SO_UNSTRIPPED=$(wc -c < "$FF_SO" | tr -d " ")
  if [[ ! -f "$OUT/obj/tests/ms_ts_synth.o" ]]; then
    "$CC" "${cflags_common[@]}" "${SAN_CFLAGS[@]+"${SAN_CFLAGS[@]}"}" \
      -I"$ROOT/tests" -c "$ROOT/tests/ms_ts_synth.c" -o "$OUT/obj/tests/ms_ts_synth.o"
  fi
  "$CC" "${cflags_common[@]}" "${SAN_CFLAGS[@]+"${SAN_CFLAGS[@]}"}" -pthread \
    -I"$ROOT/tests" -I"$FFMPEG_PREFIX/include" -I"$OPENSSL_PREFIX/include" \
    -c "$ROOT/tests/test_ff_backend.c" -o "$OUT/obj/tests/test_ff_backend.o"
  TEST_FF="$OUT/test_ff_backend"
  "$CC" "${cflags_common[@]}" "${SAN_LDFLAGS[@]+"${SAN_LDFLAGS[@]}"}" -pthread \
    -o "$TEST_FF" "$OUT/obj/tests/test_ff_backend.o" "$OUT/obj/tests/ms_ts_synth.o" \
    "$FF_A" \
    "$FFMPEG_PREFIX/lib/libavformat.a" \
    "$FFMPEG_PREFIX/lib/libavcodec.a" \
    "$FFMPEG_PREFIX/lib/libavutil.a" \
    "$OPENSSL_PREFIX/lib/libssl.a" "$OPENSSL_PREFIX/lib/libcrypto.a" \
    "$LIB" -lxml2 -lz -lm -ldl -lpthread
  # 防误链系统 libav
  if command -v ldd >/dev/null && ldd "$TEST_FF" | grep -E 'libav(format|codec|util)\.so' >/dev/null; then
    echo "error: test_ff_backend unexpectedly links system libav" >&2
    ldd "$TEST_FF" >&2
    exit 1
  fi
  if command -v ldd >/dev/null && ldd "$FF_SO" | grep -E 'libav(format|codec|util)\.so' >/dev/null; then
    echo "error: libmsff.so unexpectedly links system libav" >&2
    ldd "$FF_SO" >&2
    exit 1
  fi
  if command -v ldd >/dev/null && ldd "$FF_SO" | grep -E 'lib(ssl|crypto)\.so' >/dev/null; then
    echo "error: libmsff.so unexpectedly NEEDED system libssl/libcrypto" >&2
    ldd "$FF_SO" >&2
    exit 1
  fi
  if command -v ldd >/dev/null && ldd "$TEST_FF" | grep -E 'lib(ssl|crypto)\.so' >/dev/null; then
    echo "error: test_ff_backend unexpectedly NEEDED system libssl/libcrypto" >&2
    ldd "$TEST_FF" >&2
    exit 1
  fi
  if command -v nm >/dev/null; then
    bad_syms="$(nm -D --defined-only "$FF_SO" 2>/dev/null | awk '$2 ~ /^[TDBW]$/ {print $3}' | grep -vE '^(_init|_fini|_end|_edata|__bss_start|ms_ff_)' || true)"
    if [[ -n "$bad_syms" ]]; then
      echo "error: libmsff.so dynamically exports non-public symbols:" >&2
      echo "$bad_syms" >&2
      exit 1
    fi
  fi
  FF_BUILT=1
  echo "optional ffmpeg backend: official 9.1 + static OpenSSL 3.5.9, built $FF_A and $FF_SO (unstripped ${FF_SO_UNSTRIPPED} bytes)"
  # 有 FFmpeg 时重链 test_hw_player，便于受控 TS/FLV/UDP/RTP 入口（仍是 mock 硬解）。
  if [[ "$HW_BUILT" -eq 1 ]]; then
    if [[ ! -f "$OUT/obj/tests/ms_ts_synth.o" ]]; then
      "$CC" "${cflags_common[@]}" "${SAN_CFLAGS[@]+"${SAN_CFLAGS[@]}"}" \
        -I"$ROOT/tests" -c "$ROOT/tests/ms_ts_synth.c" -o "$OUT/obj/tests/ms_ts_synth.o"
    fi
    "$CC" "${cflags_common[@]}" "${SAN_CFLAGS[@]+"${SAN_CFLAGS[@]}"}" -pthread \
      -DMS_HW_PLAYER_HAVE_FF -I"$FFMPEG_PREFIX/include" -I"$ROOT/tests" \
      -c "$ROOT/tests/test_hw_player.c" -o "$OUT/obj/tests/test_hw_player.o"
    "$CC" "${cflags_common[@]}" "${SAN_LDFLAGS[@]+"${SAN_LDFLAGS[@]}"}" -pthread \
      -o "$OUT/test_hw_player" "$OUT/obj/tests/test_hw_player.o" "$OUT/obj/tests/ms_ts_synth.o" "$HW_A" "$FF_A" \
      "$FFMPEG_PREFIX/lib/libavformat.a" "$FFMPEG_PREFIX/lib/libavcodec.a" \
      "$FFMPEG_PREFIX/lib/libavutil.a" \
      "$OPENSSL_PREFIX/lib/libssl.a" "$OPENSSL_PREFIX/lib/libcrypto.a" \
      "$LIB" -lxml2 -lz -lm -ldl -lpthread
  fi
else
  if [[ "${ENABLE_FFMPEG:-auto}" == "0" || "${ENABLE_FFMPEG:-}" == "no" ]]; then
    echo "optional ffmpeg backend: skipped (ENABLE_FFMPEG=0); core remains without FFmpeg"
  else
    echo "optional ffmpeg backend: trimmed 9.1 prefix not available; skipped libmsff"
  fi
fi

# 可选完整 ASS 栅格：隔离前缀，不链入 libmsmedia / libmsff。
ASS_PREFIX="${ASS_DEPS_PREFIX:-$ROOT/build/ass-deps/prefix}"
ASS_A="$OUT/libmsass.a"
ASS_SO="$OUT/libmsass.so.1.0.0"
ASS_BUILT=0
WANT_ASS=1
if [[ "${ENABLE_ASS_RENDER:-auto}" == "0" || "${ENABLE_ASS_RENDER:-}" == "no" ]]; then
  WANT_ASS=0
fi
if [[ "$WANT_ASS" -eq 1 && -f "$ASS_PREFIX/lib/libass.a" && -f "$ASS_PREFIX/lib/libfreetype.a" && -f "$ASS_PREFIX/lib/libharfbuzz.a" && -f "$ASS_PREFIX/lib/libfribidi.a" ]]; then
  mkdir -p "$OUT/obj/ass" "$OUT/obj-pic/ass" "$OUT/obj/tests"
  ASS_CFLAGS=("${cflags_common[@]}" -I"$ASS_PREFIX/include" -I"$ASS_PREFIX/include/freetype2")
  "$CC" "${ASS_CFLAGS[@]}" "${SAN_CFLAGS[@]+"${SAN_CFLAGS[@]}"}" \
    -c "$ROOT/src/ass/ms_ass_render.c" -o "$OUT/obj/ass/ms_ass_render.o"
  "$AR" rcs "$ASS_A" "$OUT/obj/ass/ms_ass_render.o"
  "$RANLIB" "$ASS_A"
  "$CC" "${ASS_CFLAGS[@]}" -fPIC -fvisibility=hidden -DMSMEDIA_BUILD_SHARED \
    -c "$ROOT/src/ass/ms_ass_render.c" -o "$OUT/obj-pic/ass/ms_ass_render.o"
  ASS_CXX=""
  if [[ -x /usr/bin/g++ ]]; then
    ASS_CXX=/usr/bin/g++
  elif [[ -x /home/box/.local/mamba-build/bin/x86_64-conda-linux-gnu-g++ ]]; then
    ASS_CXX=/home/box/.local/mamba-build/bin/x86_64-conda-linux-gnu-g++
  elif [[ -x "$HOME/.local/bin/g++" ]]; then
    ASS_CXX="$(readlink -f "$HOME/.local/bin/g++" 2>/dev/null || true)"
  fi
  if [[ -z "$ASS_CXX" ]]; then
    echo "error: libmsass needs a C++ linker only to statically include HarfBuzz/libstdc++; no g++ found" >&2
    exit 1
  fi
  ASS_CXXFLAGS=(-fPIC -O2 -fno-lto -static-libstdc++ -static-libgcc)
  "$ASS_CXX" "${ASS_CXXFLAGS[@]}" -shared \
    -Wl,-soname,libmsass.so.1 -Wl,--version-script="$ROOT/src/ass/libmsass.map" \
    -Wl,--gc-sections -Wl,--exclude-libs,ALL -Wl,-z,defs \
    -o "$ASS_SO" "$OUT/obj-pic/ass/ms_ass_render.o" \
    "$ASS_PREFIX/lib/libass.a" \
    "$ASS_PREFIX/lib/libharfbuzz.a" \
    "$ASS_PREFIX/lib/libfribidi.a" \
    "$ASS_PREFIX/lib/libfreetype.a" \
    -lz -lm -lpthread -ldl -lc
  ln -sfn "$(basename "$ASS_SO")" "$OUT/libmsass.so.1"
  ln -sfn "libmsass.so.1" "$OUT/libmsass.so"
  ASS_SO_UNSTRIPPED=$(wc -c < "$ASS_SO" | tr -d " ")
  if command -v ldd >/dev/null; then
    if ldd "$ASS_SO" | grep -E 'lib(ass|harfbuzz|fribidi|freetype|stdc\+\+)\.so' >/dev/null; then
      echo "error: libmsass.so unexpectedly NEEDED a shared dep that should be static:" >&2
      ldd "$ASS_SO" >&2
      exit 1
    fi
    if ldd "$SO_REAL" | grep -E 'libmsass\.so' >/dev/null; then
      echo "error: libmsmedia.so unexpectedly NEEDED libmsass" >&2
      exit 1
    fi
    if [[ "${FF_BUILT:-0}" -eq 1 && -e "$FF_SO" ]] && ldd "$FF_SO" | grep -E 'libmsass\.so|libass\.so' >/dev/null; then
      echo "error: libmsff.so unexpectedly NEEDED libmsass/libass" >&2
      exit 1
    fi
  fi
  "$CC" "${cflags_common[@]}" "${SAN_CFLAGS[@]+"${SAN_CFLAGS[@]}"}" \
    -c "$ROOT/tests/test_ass_render.c" -o "$OUT/obj/tests/test_ass_render.o"
  TEST_ASS="$OUT/test_ass_render"
  "$CC" "${cflags_common[@]}" "${SAN_LDFLAGS[@]+"${SAN_LDFLAGS[@]}"}" \
    -o "$TEST_ASS" "$OUT/obj/tests/test_ass_render.o" \
    -L"$OUT" -lmsass -Wl,-rpath,'$ORIGIN' -lz -lm -lpthread -ldl
  ASS_BUILT=1
  echo "optional ass render: libmsass built $ASS_A and $ASS_SO (unstripped ${ASS_SO_UNSTRIPPED} bytes); not in core"
elif [[ "${ENABLE_ASS_RENDER:-auto}" == "1" || "${ENABLE_ASS_RENDER:-}" == "yes" ]]; then
  echo "error: ENABLE_ASS_RENDER=1 but ass-deps prefix missing at $ASS_PREFIX (run scripts/ass_deps_build.sh)" >&2
  exit 1
else
  echo "optional ass render: skipped (no prefix or ENABLE_ASS_RENDER=0); light text path remains"
fi

# 可选 AI 字幕插件：无第三方模型/SDK，不链入 libmsmedia / libmsff / 默认快启动路径。
AI_A="$OUT/libmsai.a"
AI_SO="$OUT/libmsai.so.1.0.0"
AI_BUILT=0
WANT_AI=1
if [[ "${ENABLE_AI_SUB:-auto}" == "0" || "${ENABLE_AI_SUB:-}" == "no" ]]; then
  WANT_AI=0
fi
if [[ "$WANT_AI" -eq 1 ]]; then
  mkdir -p "$OUT/obj/ai" "$OUT/obj-pic/ai" "$OUT/obj/tests"
  AI_SRCS=("$ROOT/src/ai/ms_ai_pcm.c" "$ROOT/src/ai/ms_ai_fake.c" "$ROOT/src/ai/ms_ai_sub.c")
  AI_OBJS=()
  AI_PIC_OBJS=()
  for src in "${AI_SRCS[@]}"; do
    base=$(basename "$src" .c)
    "$CC" "${cflags_common[@]}" "${SAN_CFLAGS[@]+"${SAN_CFLAGS[@]}"}"       -c "$src" -o "$OUT/obj/ai/${base}.o"
    AI_OBJS+=("$OUT/obj/ai/${base}.o")
    "$CC" "${cflags_common[@]}" -fPIC -fvisibility=hidden -DMSMEDIA_BUILD_SHARED       "${SAN_CFLAGS[@]+"${SAN_CFLAGS[@]}"}"       -c "$src" -o "$OUT/obj-pic/ai/${base}.o"
    AI_PIC_OBJS+=("$OUT/obj-pic/ai/${base}.o")
  done
  "$AR" rcs "$AI_A" "${AI_OBJS[@]}"
  "$RANLIB" "$AI_A"
  "$CC" "${cflags_common[@]}" "${SAN_LDFLAGS[@]+"${SAN_LDFLAGS[@]}"}" -shared     -Wl,-soname,libmsai.so.1 -Wl,--version-script="$ROOT/src/ai/libmsai.map"     -Wl,--gc-sections -Wl,-z,defs     -o "$AI_SO" "${AI_PIC_OBJS[@]}" -L"$OUT" -lmsmedia -lpthread -lc -Wl,-rpath,'$ORIGIN'
  ln -sfn "$(basename "$AI_SO")" "$OUT/libmsai.so.1"
  ln -sfn "libmsai.so.1" "$OUT/libmsai.so"
  AI_SO_UNSTRIPPED=$(wc -c < "$AI_SO" | tr -d " ")
  if command -v ldd >/dev/null; then
    if ldd "$SO_REAL" | grep -E 'libmsai\.so' >/dev/null; then
      echo "error: libmsmedia.so unexpectedly NEEDED libmsai" >&2
      exit 1
    fi
    if [[ "${FF_BUILT:-0}" -eq 1 && -e "$FF_SO" ]] && ldd "$FF_SO" | grep -E 'libmsai\.so' >/dev/null; then
      echo "error: libmsff.so unexpectedly NEEDED libmsai" >&2
      exit 1
    fi
    if [[ "${HW_BUILT:-0}" -eq 1 && -e "$HW_SO" ]] && ldd "$HW_SO" | grep -E 'libmsai\.so' >/dev/null; then
      echo "error: libmshw.so unexpectedly NEEDED libmsai" >&2
      exit 1
    fi
  fi
  "$CC" "${cflags_common[@]}" "${SAN_CFLAGS[@]+"${SAN_CFLAGS[@]}"}"     -I"$ROOT/src/hw" -I"$ROOT/tests" -c "$ROOT/tests/test_ai_sub.c" -o "$OUT/obj/tests/test_ai_sub.o"
  TEST_AI="$OUT/test_ai_sub"
  "$CC" "${cflags_common[@]}" "${SAN_LDFLAGS[@]+"${SAN_LDFLAGS[@]}"}"     -o "$TEST_AI" "$OUT/obj/tests/test_ai_sub.o" "$AI_A" "$HW_A" "$LIB" -lpthread -ldl
  AI_BUILT=1
  echo "optional ai subtitle: libmsai built $AI_A and $AI_SO (unstripped ${AI_SO_UNSTRIPPED} bytes); not in core; fake vendor only"
else
  echo "optional ai subtitle: skipped (ENABLE_AI_SUB=0)"
fi

if [[ "$MODE" == "release" || "$MODE" == "release-size" ]]; then
  # 静态库不 strip，保留链接符号；只 strip 可执行文件与共享库。
  "$STRIP" "$TEST_BIN" "$INSPECT_BIN" "$BMFF_INSPECT_BIN" "$SMOKE_BIN"
  "$STRIP" --strip-unneeded "$SO_REAL"
  if [[ "$NET_BUILT" -eq 1 ]]; then
    "$STRIP" "$TEST_NET"
    "$STRIP" --strip-unneeded "$NET_SO"
  fi
  if [[ "$FF_BUILT" -eq 1 ]]; then
    "$STRIP" "$TEST_FF"
    "$STRIP" --strip-unneeded "$FF_SO"
  fi
  if [[ "$HW_BUILT" -eq 1 ]]; then
    "$STRIP" "$TEST_HW"
    "$STRIP" --strip-unneeded "$HW_SO"
  fi
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
echo "built $SO_REAL (soname libmsmedia.so.1; unstripped ${SO_UNSTRIPPED_BYTES} bytes)"
echo "built $TEST_BIN"
echo "built $TEST_TS_SCAN"
echo "built $INSPECT_BIN"
echo "built $BMFF_INSPECT_BIN"
echo "built $SMOKE_BIN (links libmsmedia.so; no sanitizer)"
if [[ "$NET_BUILT" -eq 1 ]]; then
  echo "built $NET_A"
  echo "built $NET_SO (soname libmsnet.so.1; unstripped ${NET_SO_UNSTRIPPED} bytes)"
  echo "built $TEST_NET"
  ls -l "$NET_A" "$NET_SO" "$OUT/libmsnet.so" "$TEST_NET"
fi
if [[ "$FF_BUILT" -eq 1 ]]; then
  echo "built $FF_A"
  echo "built $FF_SO (soname libmsff.so.1; unstripped ${FF_SO_UNSTRIPPED} bytes)"
  echo "built $TEST_FF"
  ls -l "$FF_A" "$FF_SO" "$OUT/libmsff.so" "$TEST_FF"
fi
if [[ "$HW_BUILT" -eq 1 ]]; then
  echo "built $HW_A"
  echo "built $HW_SO (soname libmshw.so.1; unstripped ${HW_SO_UNSTRIPPED} bytes)"
  echo "built $TEST_HW"
  echo "built $OUT/test_hw_player"
  ls -l "$HW_A" "$HW_SO" "$OUT/libmshw.so" "$TEST_HW" "$OUT/test_hw_player"
fi
if [[ "${ASS_BUILT:-0}" -eq 1 ]]; then
  echo "built $ASS_A"
  echo "built $ASS_SO (soname libmsass.so.1; unstripped ${ASS_SO_UNSTRIPPED} bytes)"
  echo "built $TEST_ASS"
  ls -l "$ASS_A" "$ASS_SO" "$OUT/libmsass.so" "$TEST_ASS"
  echo "ass-deps static: $(wc -c < "$ASS_PREFIX/lib/libass.a") libass.a  $(wc -c < "$ASS_PREFIX/lib/libfreetype.a") libfreetype.a  $(wc -c < "$ASS_PREFIX/lib/libharfbuzz.a") libharfbuzz.a  $(wc -c < "$ASS_PREFIX/lib/libfribidi.a") libfribidi.a"
fi


# 新旧头动态兼容专项：用已交付 1838 公共头编译独立客户端，动态链当前
# libmsmedia.so / libmsff.so（若已构建）。debug 模式带 ASan/UBSan，并用栈金丝雀
# 检测 ms_event 按值 pull 是否越过旧布局。不得用当前 include/ 编译旧客户端 TU。
ABI1838="$ROOT/tests/abi_headers/delivered-20261121-1838/include"
ABI_OLD_SRC="$ROOT/tests/test_abi_old_header.c"
ABI_STRIDE_SRC="$ROOT/tests/test_abi_stride_current.c"
ABI_BIN="$OUT/test_abi_old_header"
ABI_BUILT=0
if [[ -f "$ABI_OLD_SRC" && -f "$ABI_STRIDE_SRC" && -f "$ABI1838/msmedia/msmedia.h" && -e "$SO_LINK" ]]; then
  abi_cflags=()
  for flag in "${cflags_common[@]}"; do
    if [[ "$flag" == "-I$ROOT/include" ]]; then
      continue
    fi
    abi_cflags+=("$flag")
  done
  abi_defs=()
  abi_libs=(-L"$OUT" -lmsmedia)
  if [[ "${FF_BUILT:-0}" -eq 1 ]]; then
    abi_defs+=(-DMS_ABI_HAVE_FF)
    abi_libs=(-L"$OUT" -lmsff -lmsmedia)
  fi
  "$CC" "${abi_cflags[@]}" "${SAN_CFLAGS[@]+"${SAN_CFLAGS[@]}"}" "${abi_defs[@]}" \
    -I"$ABI1838" -c "$ABI_OLD_SRC" -o "$OUT/obj/tests/test_abi_old_header.o"
  "$CC" "${cflags_common[@]}" "${SAN_CFLAGS[@]+"${SAN_CFLAGS[@]}"}" \
    -c "$ABI_STRIDE_SRC" -o "$OUT/obj/tests/test_abi_stride_current.o"
  "$CC" "${cflags_common[@]}" "${SAN_LDFLAGS[@]+"${SAN_LDFLAGS[@]}"}" \
    -o "$ABI_BIN" "$OUT/obj/tests/test_abi_old_header.o" "$OUT/obj/tests/test_abi_stride_current.o" \
    "${abi_libs[@]}" -Wl,-rpath,'$ORIGIN' -Wl,-z,defs -lc
  ABI_BUILT=1
  echo "built $ABI_BIN (1838 headers, dynamic current .so${FF_BUILT:+ + libmsff})"
fi

if command -v "$CC" >/dev/null 2>&1; then
  echo "compiler: $CC ($("$CC" --version 2>/dev/null | head -n 1 || echo unknown))"
fi
ls -l "$LIB" "$SO_REAL" "$SO_SONAME" "$SO_LINK" "$TEST_BIN" "$TEST_TS_SCAN" "$TEST_HDR" "$INSPECT_BIN" "$BMFF_INSPECT_BIN" "$SMOKE_BIN"

if [[ "$RUN_TEST" -eq 1 ]]; then
  echo "running $TEST_BIN"
  "$TEST_BIN"
  echo "running $TEST_TS_SCAN"
  "$TEST_TS_SCAN"
  echo "running $TEST_HDR (host VUI/SEI + hdr-static-info layout; not display)"
  "$TEST_HDR"
  if [[ "$NET_BUILT" -eq 1 ]]; then
    echo "running $TEST_NET"
    "$TEST_NET" "$OUT/net-ca"
  fi
  if [[ "$HW_BUILT" -eq 1 ]]; then
    echo "running $TEST_HW"
    "$TEST_HW"
    echo "running $OUT/test_media3_sync (Media3 A/V sync port, injected clock)"
    "$OUT/test_media3_sync"
    echo "running $OUT/test_hw_player (mock state machine first)"
    "$OUT/test_hw_player"
    echo "running $OUT/test_hw_vaapi (parse+probe; not device decode)"
    "$OUT/test_hw_vaapi"
  fi
  if [[ "$FF_BUILT" -eq 1 ]]; then
    echo "running $TEST_FF"
    mkdir -p "$OUT/ff-tmp"
    DASH_DIR="$ROOT/build/dev-samples/dash-ff"
    if [[ -x /usr/bin/ffmpeg ]]; then
      env -u CC -u CXX -u LD PATH="/usr/bin:/bin:$PATH"         bash "$ROOT/scripts/gen_ff_samples.sh" "$DASH_DIR" || true
    fi
    env -u OPENSSL_MODULES OPENSSL_CONF="$OPENSSL_PREFIX/ssl/openssl.cnf" MS_FF_ROOT="$ROOT" MS_FF_CA="$OUT/net-ca" MS_FF_TMP="$OUT/ff-tmp" MS_FF_DASH_DIR="$DASH_DIR" "$TEST_FF"
    if [[ "$HW_BUILT" -eq 1 ]]; then
      TS_IN=""
      FLV_IN=""
      if [[ -f "$OUT/ff-tmp/a.ts" ]]; then TS_IN="file:$OUT/ff-tmp/a.ts"; fi
      if [[ -z "$TS_IN" && -f "$DASH_DIR/short.ts" ]]; then TS_IN="file:$DASH_DIR/short.ts"; fi
      if [[ -f "$OUT/ff-tmp/a.flv" ]]; then FLV_IN="file:$OUT/ff-tmp/a.flv"; fi
      echo "running $OUT/test_hw_player with controlled TS/FLV entries (mock hw, not display)"
      env -u OPENSSL_MODULES OPENSSL_CONF="$OPENSSL_PREFIX/ssl/openssl.cnf" \
        MS_HW_PLAYER_TS="$TS_IN" MS_HW_PLAYER_FLV="$FLV_IN" \
        "$OUT/test_hw_player"
    fi
  fi

  if [[ "${ASS_BUILT:-0}" -eq 1 ]]; then
    echo "running $TEST_ASS (host ASS pixel evidence; not device visual)"
    "$TEST_ASS"
  fi

  if [[ "${AI_BUILT:-0}" -eq 1 ]]; then
    echo "running $TEST_AI (simulated fake vendor loop; not real ASR)"
    env LD_LIBRARY_PATH="$OUT${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" "$TEST_AI"
  fi

  if [[ "${ABI_BUILT:-0}" -eq 1 ]]; then
    echo "running $ABI_BIN (1838 headers + current .so, ASan canary)"
    ABI_URL=""
    if [[ -f "$OUT/ff-tmp/a.ts" ]]; then
      ABI_URL="file:$OUT/ff-tmp/a.ts"
    elif [[ -f "$ROOT/build/dev-samples/dash-ff/short.ts" ]]; then
      ABI_URL="file:$ROOT/build/dev-samples/dash-ff/short.ts"
    fi
    if [[ -n "$ABI_URL" ]]; then
      "$ABI_BIN" "$ABI_URL"
    else
      "$ABI_BIN"
    fi
  fi
fi
if [[ "$RUN_TEST" -ne 1 && "${WANT_FFMPEG_EXPLICIT:-0}" == "1" && "$FF_BUILT" -eq 1 ]]; then
  echo "running $TEST_FF"
  mkdir -p "$OUT/ff-tmp"
  DASH_DIR="$ROOT/build/dev-samples/dash-ff"
  if [[ -x /usr/bin/ffmpeg ]]; then
    env -u CC -u CXX -u LD PATH="/usr/bin:/bin:$PATH"       bash "$ROOT/scripts/gen_ff_samples.sh" "$DASH_DIR" || true
  fi
  env -u OPENSSL_MODULES OPENSSL_CONF="$OPENSSL_PREFIX/ssl/openssl.cnf" MS_FF_ROOT="$ROOT" MS_FF_CA="$OUT/net-ca" MS_FF_TMP="$OUT/ff-tmp" MS_FF_DASH_DIR="$DASH_DIR" "$TEST_FF"
fi
