#!/usr/bin/env bash
# MsMedia 出包入口（ABI 参数对齐超级直播 bs-mycjzb/scripts/build.sh）。
#
#   ./scripts/build.sh                 # slimff 生产 so，双 ABI，默认 UPX
#   ./scripts/build.sh -va v7a         # 仅 armeabi-v7a
#   ./scripts/build.sh -va v8a         # 仅 arm64-v8a
#   ./scripts/build.sh -va both        # 双 ABI（默认；可用 MS_ABI=）
#   ./scripts/build.sh -va all         # 依次打 v7a、v8a、both
#   ./scripts/build.sh pure -va v7a    # 纯自研 MODE=pure
#   ./scripts/build.sh slimff --pack 1 # 默认；OPT=os + LINK_BFD=1 + pack_apk_so（抄超级直播）
#   ./scripts/build.sh slimff --pack 0 # 不压 so（仍可用默认 oz-flto 体积档）
#   ./scripts/build.sh --publish       # 编完同步到 MsPlayer（Java + v7a/v8a so）
#   ./scripts/build.sh --deps          # 先编 OpenSSL/FFmpeg/msxml 前缀（首次或换 TRIM 时）
#   ./scripts/build.sh apk             # 只打 MsSpeed 演示 APK（用已有 so）
#   ./scripts/build.sh --apk           # 先编 so 再打演示 APK
#
# 等价：--abi / 环境 MS_ABI=v7a|v8a|both|all；MS_SO_PACK / --pack 0|1
# by: Long <mycjzb@gmail.com>
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
# shellcheck source=android_env.sh
source "$ROOT/scripts/android_env.sh"

MODE="slimff"
APK_ONLY=0
if [[ $# -gt 0 && "$1" != -* ]]; then
  case "$(printf '%s' "$1" | tr '[:upper:]' '[:lower:]')" in
    pure|slimff|release)
      [[ "$1" == release ]] && MODE=slimff || MODE="$(printf '%s' "$1" | tr '[:upper:]' '[:lower:]')"
      shift
      ;;
    apk|demo)
      APK_ONLY=1
      shift
      ;;
  esac
fi

SAVED_ARGS=("$@")
ABI_SEL="${MS_ABI:-}"
PACK="${MS_SO_PACK:-${PACK_SO:-1}}"
PUBLISH=0
DO_DEPS=0
DO_APK=0
OPT="${OPT:-}"
ADEC="${ADEC:-1}"
ADEC_TAG="${ADEC_TAG:-s6}"
XML_FLAVOR="${XML_FLAVOR:-ms}"
WITH_XML="${WITH_XML:-0}"
TLS="${TLS:-openssl}"
HLS_PF="${HLS_PF:-0}"
SDK_DIR="${SDK_DIR:-/home/long/code/MsPlayer}"

while [[ $# -gt 0 ]]; do
  case "$1" in
    -va|--abi)     ABI_SEL="$2"; shift 2 ;;
    --pack|--PACK) PACK="$2"; shift 2 ;;
    --no-pack)     PACK=0; shift ;;
    --publish)     PUBLISH=1; shift ;;
    --deps)        DO_DEPS=1; shift ;;
    --apk|--demo)  DO_APK=1; shift ;;
    --opt)         OPT="$2"; shift 2 ;;
    --tls)         TLS="$2"; shift 2 ;;
    --sdk-dir)     SDK_DIR="$2"; shift 2 ;;
    -h|--help)
      awk 'NR==2,/^# by:/{sub(/^# ?/,""); print} /^set -euo/{exit}' "$0"
      exit 0
      ;;
    *)
      echo "error: unknown option '$1' (want -va v7a|v8a|both|all | apk | --apk)" >&2
      exit 1
      ;;
  esac
done

run_apk() {
  local sub
  if [[ "$MODE" == pure ]]; then
    sub="single-native-s2/${OPT:-os}"
  else
    sub="single-slimff-adec-${ADEC_TAG}"
    [[ "${WITH_XML:-0}" == 1 ]] || sub="${sub}-noxml"
    sub="${sub}/${OPT:-oz-flto}"
  fi
  echo "==> demo APK VARIANT=$MODE SUB=$sub"
  VARIANT="$MODE" SINGLE_SUB="$sub" APK_OUT="$ROOT/build/single-apk" \
    scripts/android_single_apk.sh
  echo "apk: $ROOT/build/single-apk/$MODE/MsSpeed-$MODE-debug.apk"
  echo "apk: $ROOT/build/single-apk/$MODE/MsSpeed-$MODE-release-arm.apk"
}

if [[ "$APK_ONLY" -eq 1 ]]; then
  [[ -z "$OPT" ]] && { [[ "$MODE" == pure ]] && OPT=os || OPT=oz-flto; }
  run_apk
  exit 0
fi

ABI_SEL_LC="$(printf '%s' "$ABI_SEL" | tr '[:upper:]' '[:lower:]')"

# -va all：去掉本层 -va 后依次打 v7a、v8a、both（与超级直播相同）
if [[ "$ABI_SEL_LC" == "all" ]]; then
  base_args=()
  skip_next=0
  for ((i = 0; i < ${#SAVED_ARGS[@]}; i++)); do
    if [[ "$skip_next" -eq 1 ]]; then skip_next=0; continue; fi
    a="${SAVED_ARGS[$i]}"
    case "$a" in
      -va|--abi) skip_next=1; continue ;;
      *) base_args+=("$a") ;;
    esac
  done
  rc=0
  for va in v7a v8a both; do
    echo "==> -va all: building $va …"
    cmd=(bash "$ROOT/scripts/build.sh" "$MODE")
    cmd+=("${base_args[@]}")
    cmd+=(-va "$va")
    if ! "${cmd[@]}"; then rc=$?; fi
  done
  exit "$rc"
fi

case "$ABI_SEL_LC" in
  ''|both|dual)
    ABIS="armeabi-v7a arm64-v8a"
    ABI_TAG=""
    ;;
  v7a|armeabi-v7a|arm32)
    ABIS="armeabi-v7a"
    ABI_TAG="-v7a"
    ;;
  v8a|arm64-v8a|arm64)
    ABIS="arm64-v8a"
    ABI_TAG="-v8a"
    ;;
  *)
    echo "error: -va / --abi must be v7a, v8a, both, or all (got '$ABI_SEL')" >&2
    exit 1
    ;;
esac

# 压壳对齐超级直播：bfd+Hardening → 2×LOAD → pack_apk_so。不能用 oz-flto/lld。
LINK_BFD=0
FF_DEP_OPT=oz-flto
OSSL_LTO=1
if [[ "$PACK" == 1 ]]; then
  if [[ -n "$OPT" && "$OPT" != os && "$OPT" != oz ]]; then
    echo "error: --pack 1 只能 OPT=os|oz（当前 OPT=$OPT）；LTO+lld 无法出 2×LOAD" >&2
    exit 1
  fi
  [[ -z "$OPT" ]] && OPT=os
  LINK_BFD=1
  FF_DEP_OPT=os
  OSSL_LTO=0
elif [[ -z "$OPT" ]]; then
  if [[ "$MODE" == pure ]]; then OPT=os; else OPT=oz-flto; fi
fi

export ABIS PACK_SO="$PACK" LINK_BFD
export MODE OPT ADEC ADEC_TAG XML_FLAVOR WITH_XML TLS HLS_PF

echo "==> NDK=$MS_NDK MODE=$MODE OPT=$OPT ABIS=$ABIS PACK_SO=$PACK LINK_BFD=$LINK_BFD${ABI_TAG:+ tag=$ABI_TAG}"

if [[ "$DO_DEPS" -eq 1 || "$MODE" == slimff ]]; then
  need_ff=0
  for abi in $ABIS; do
    case "$abi" in
      armeabi-v7a) d=android-v7a ;;
      arm64-v8a)   d=android-v8a ;;
      *) continue ;;
    esac
    if [[ "$MODE" == slimff ]]; then
      # 同目录可被 os / oz-flto 轮流占用；缺文件或 stamp 与本次 FF_DEP_OPT 不一致都要重编（bfd 不能链位码）
      ff_a="$ROOT/build/$d/ffmpeg-9.1-slimff-adec-${ADEC_TAG}/prefix/lib/libavformat.a"
      ff_ok="$ROOT/build/$d/ffmpeg-9.1-slimff-adec-${ADEC_TAG}/prefix/.msmedia-slimff-ok"
      if [[ ! -f "$ff_a" ]] || ! grep -q "^ff_opt=$FF_DEP_OPT\$" "$ff_ok" 2>/dev/null; then
        need_ff=1
      fi
      if [[ "$OSSL_LTO" == 0 ]]; then
        [[ -f "$ROOT/build/$d/openssl-3.5.9-slim/prefix/lib/libssl.a" ]] || need_ff=1
      else
        [[ -f "$ROOT/build/$d/openssl-3.5.9-slim-flto/prefix/lib/libssl.a" ]] || need_ff=1
      fi
      if [[ "$XML_FLAVOR" == ms ]]; then
        [[ -f "$ROOT/build/$d/msxml-shim/prefix/lib/libxml2.a" ]] || need_ff=1
      fi
    fi
  done
  if [[ "$DO_DEPS" -eq 1 || "$need_ff" -eq 1 ]]; then
    echo "==> deps (openssl slim LTO=$OSSL_LTO / msxml / slimff ffmpeg FF_OPT=$FF_DEP_OPT)"
    OPENSSL_SLIM_LTO="$OSSL_LTO" ABIS="$ABIS" scripts/openssl_slim_build.sh
    if [[ "$XML_FLAVOR" == ms ]]; then
      ABIS="$ABIS" scripts/android_msxml_shim.sh
    fi
    if [[ "$MODE" == slimff ]]; then
      TRIM=s6 ADEC="$ADEC" ADEC_TAG="$ADEC_TAG" FF_OPT="$FF_DEP_OPT" \
        XML_FLAVOR="$XML_FLAVOR" WITH_XML="$WITH_XML" ABIS="$ABIS" \
        scripts/android_slimff_ffmpeg.sh
    fi
  fi
fi

MODE="$MODE" OPT="$OPT" ADEC="$ADEC" ADEC_TAG="$ADEC_TAG" \
  XML_FLAVOR="$XML_FLAVOR" WITH_XML="$WITH_XML" TLS="$TLS" HLS_PF="$HLS_PF" \
  PACK_SO="$PACK" LINK_BFD="$LINK_BFD" ABIS="$ABIS" \
  scripts/android_single_build.sh

if [[ "$PUBLISH" -eq 1 ]]; then
  if [[ "$ABIS" != *"armeabi-v7a"* || "$ABIS" != *"arm64-v8a"* ]]; then
    echo "error: --publish 需要双 ABI（-va both / 默认）；当前 ABIS=$ABIS" >&2
    exit 1
  fi
  SUB="single-native-s2/$OPT"
  if [[ "$MODE" == slimff ]]; then
    SUB="single-slimff-adec-${ADEC_TAG}"
    [[ "${WITH_XML:-0}" == 1 ]] || SUB="${SUB}-noxml"
    SUB="${SUB}/$OPT"
  fi
  echo "==> publish -> $SDK_DIR (sub=$SUB)"
  SDK_DIR="$SDK_DIR" SINGLE_SUB="$SUB" scripts/publish_msplayer_sdk.sh
fi

if [[ "$DO_APK" -eq 1 ]]; then
  run_apk
fi

echo "build: done MODE=$MODE ABIS=$ABIS PACK_SO=$PACK"
