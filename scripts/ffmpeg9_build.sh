#!/usr/bin/env bash
# 构建官方 FFmpeg 9.1 裁剪静态库（LGPL，无 GPL/nonfree，无软编解码器）。
# 由仓库根目录 ./build.sh ffmpeg 调用。下载按 pin 校验，不自动升级。
# 宿主自测，不能代替 el7/四平台正式验收。
# by: Long <mycjzb@gmail.com>
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PIN="$ROOT/third_party/ffmpeg-9.1.pin"
CACHE="$ROOT/third_party/cache"
FFMPEG_ROOT="${FFMPEG_BUILD_ROOT:-$ROOT/build/ffmpeg-9.1}"
PREFIX="${FFMPEG_PREFIX:-$FFMPEG_ROOT/prefix}"
BUILDDIR="${FFMPEG_BUILDDIR:-$FFMPEG_ROOT/build}"
SRC="$CACHE/ffmpeg-9.1"
FLAGS_LOG="$FFMPEG_ROOT/configure-flags.txt"

# shellcheck disable=SC1090
source "$PIN"

need() { command -v "$1" >/dev/null || { echo "error: missing $1" >&2; exit 2; }; }
need curl; need gpg; need sha256sum; need tar; need make; need pkg-config

OPENSSL_PREFIX="${OPENSSL_PREFIX:-$ROOT/build/openssl-3.5.9/prefix}"
if [[ ! -f "$OPENSSL_PREFIX/lib/libssl.a" || ! -f "$OPENSSL_PREFIX/lib/libcrypto.a" ]]; then
  if ! env -u CC -u CXX -u LD PATH="/usr/bin:/bin:$PATH" bash "$ROOT/scripts/openssl_build.sh"; then
    echo "error: pinned OpenSSL 3.5.9 prefix build failed" >&2
    exit 1
  fi
fi
if [[ ! -f "$OPENSSL_PREFIX/lib/libssl.a" || ! -d "$OPENSSL_PREFIX/include/openssl" ]]; then
  echo "error: OpenSSL prefix missing at $OPENSSL_PREFIX" >&2
  exit 1
fi
OPENSSL_STAMP="$OPENSSL_PREFIX/.msmedia-openssl-pin"
FF_SSL_STAMP="$PREFIX/.msmedia-ffmpeg-openssl"

mkdir -p "$CACHE" "$BUILDDIR" "$PREFIX"

if [[ ! -f "$CACHE/ffmpeg-devel.asc" ]]; then
  curl -fsSL --retry 3 -o "$CACHE/ffmpeg-devel.asc" "$pgp_key_url"
fi
if [[ ! -f "$CACHE/ffmpeg-9.1.tar.xz.asc" ]]; then
  curl -fsSL --retry 3 -o "$CACHE/ffmpeg-9.1.tar.xz.asc" "$signature_url"
fi
if [[ ! -f "$CACHE/ffmpeg-9.1.tar.xz" ]]; then
  curl -fL --retry 3 --retry-delay 2 -o "$CACHE/ffmpeg-9.1.tar.xz" "$tarball_url"
fi

got="$(sha256sum "$CACHE/ffmpeg-9.1.tar.xz" | awk '{print $1}')"
if [[ "$got" != "$tarball_sha256" ]]; then
  echo "error: SHA-256 mismatch for ffmpeg-9.1.tar.xz" >&2
  echo "  expected $tarball_sha256" >&2
  echo "  got      $got" >&2
  echo "  refusing to auto-upgrade; check $PIN" >&2
  exit 1
fi

gpg --import "$CACHE/ffmpeg-devel.asc" >/dev/null 2>&1 || true
# LANG=C：避免中文「完好的签名」导致英文匹配误杀。
if ! LANG=C gpg --verify "$CACHE/ffmpeg-9.1.tar.xz.asc" "$CACHE/ffmpeg-9.1.tar.xz" 2>"$BUILDDIR/gpg-verify.log"; then
  if ! grep -qE 'Good signature|完好的签名' "$BUILDDIR/gpg-verify.log"; then
    echo "error: gpg --verify failed; see $BUILDDIR/gpg-verify.log" >&2
    exit 1
  fi
fi
if ! grep -qE 'Good signature from "FFmpeg release signing key|完好的签名' "$BUILDDIR/gpg-verify.log"; then
  echo "error: PGP signature not good; see $BUILDDIR/gpg-verify.log" >&2
  exit 1
fi
if ! grep -q "$pgp_key_fingerprint" "$BUILDDIR/gpg-verify.log" && \
   ! grep -q "FCF9 86EA 15E6 E293 A564  4F10 B432 2F04 D676 58D8" "$BUILDDIR/gpg-verify.log"; then
  echo "error: unexpected PGP key; see $BUILDDIR/gpg-verify.log" >&2
  exit 1
fi

if [[ ! -x "$SRC/configure" ]]; then
  tar -xJf "$CACHE/ffmpeg-9.1.tar.xz" -C "$CACHE"
fi
if [[ ! -f "$SRC/RELEASE" ]] || [[ "$(tr -d '[:space:]' < "$SRC/RELEASE")" != "9.1" ]]; then
  echo "error: extracted RELEASE is not 9.1" >&2
  exit 1
fi

# 最小本地补丁：播放中启用字幕 rendition 时不要按 cur_timestamp 丢掉本段已开始的 cue。
HLS_SUB_PATCH="$ROOT/patches/ffmpeg-9.1-hls-sub-rendition-switch.patch"
if [[ -f "$HLS_SUB_PATCH" ]]; then
  if ! grep -q "MsMedia: do not catch-up-drop subtitle cues" "$SRC/libavformat/hls.c"; then
    patch -p1 -d "$SRC" < "$HLS_SUB_PATCH"
  fi
fi

# 最小本地补丁 2：字幕 seek 覆盖已开始未结束 cue、直播窗口重放、WebVTT X-TIMESTAMP-MAP。
HLS_SUB_TL_PATCH="$ROOT/patches/ffmpeg-9.1-hls-sub-timeline.patch"
if [[ -f "$HLS_SUB_TL_PATCH" ]]; then
  if ! grep -q "MsMedia: keep overlapping subtitle cues on seek" "$SRC/libavformat/hls.c" || \
     ! grep -q "MsMedia: apply X-TIMESTAMP-MAP" "$SRC/libavformat/webvttdec.c"; then
    patch -p1 -d "$SRC" < "$HLS_SUB_TL_PATCH"
  fi
fi

# 最小本地补丁 3：公布 HLS EVENT/滑动窗口 start/end 元数据，不改 VOD duration，不关字幕 ctx。
HLS_LIVE_WIN_PATCH="$ROOT/patches/ffmpeg-9.1-hls-live-window.patch"
if [[ -f "$HLS_LIVE_WIN_PATCH" ]]; then
  if ! grep -q "MsMedia: publish HLS live window in metadata" "$SRC/libavformat/hls.c"; then
    patch -p1 -d "$SRC" < "$HLS_LIVE_WIN_PATCH"
  fi
fi

# 最小本地补丁 4：短 TS 探针分数须高于 mpegps，避免误认成 MPEG-PS。
MPEGTS_SHORT_PROBE_PATCH="$ROOT/patches/ffmpeg-9.1-mpegts-short-probe.patch"
if [[ -f "$MPEGTS_SHORT_PROBE_PATCH" ]]; then
  if ! grep -q "MsMedia: short TS (fewer than 10 FEC-sized slots)" "$SRC/libavformat/mpegts.c"; then
    patch -p1 -d "$SRC" < "$MPEGTS_SHORT_PROBE_PATCH"
  fi
fi

# 默认 -O2。MS_SIZE_OPT=1 时改 -Os 并 --enable-small，避免 extra-cflags 里的 -O2 盖掉 enable-small。
if [[ "${MS_SIZE_OPT:-0}" == "1" ]]; then
  FF_EXTRA_CFLAGS="-fPIC -Os -ffunction-sections -fdata-sections -fno-lto -I${OPENSSL_PREFIX}/include"
  FF_EXTRA_LDFLAGS="-Wl,--gc-sections -L${OPENSSL_PREFIX}/lib"
else
  FF_EXTRA_CFLAGS="-fPIC -O2 -fno-lto -I${OPENSSL_PREFIX}/include"
  FF_EXTRA_LDFLAGS="-L${OPENSSL_PREFIX}/lib"
fi

# 明确不用系统 libav 的搜索路径；只链本 prefix 与 OpenSSL/libxml2。
CFG_ARGS=(
  --prefix="$PREFIX"
  --enable-pic
  --enable-static
  --disable-shared
  --disable-doc
  --disable-htmlpages
  --disable-manpages
  --disable-podpages
  --disable-txtpages
  --disable-programs
  --disable-ffmpeg
  --disable-ffplay
  --disable-ffprobe
  --disable-avdevice
  --disable-avfilter
  --disable-swscale
  --disable-swresample
  --disable-everything
  --enable-avutil
  --enable-avcodec
  --enable-avformat
  --enable-network
  --enable-openssl
  --enable-libxml2
  --disable-gpl
  --disable-version3
  --disable-nonfree
  --enable-protocol=file,http,https,tls,tcp,udp,rtp,rtmp,rtmps,rtmpt,crypto,pipe,fd,srtp
  --enable-demuxer=mpegts,mpegtsraw,flv,live_flv,mov,hls,dash,rtsp,rtp,sdp,aac,h264,hevc,ogg,matroska,webvtt,avi,mpegps,mp3,flac,wav,asf,rm,ac3,eac3,srt,ass,microdvd,vobsub
  # 不启用 mpeg4video/h263/av1/cavsvideo：mpeg4video_parser 会 select mpegvideodec，
  # 把 mpeg4videodec/ituh263dec 等解码器实现编进 libavcodec.a（CONFIG_DECODERS 仍为 0）；
  # av1_parser 会 select cbs_av1。容器 extradata 与产品 in-band CSD 已覆盖这些编码。
  # flac/ac3/mpegaudio 为对应 demuxer 所需；mpegvideo parser 体积小，供 MPEG-PS。
  --enable-parser=h264,hevc,mpegvideo,aac,aac_latm,mpegaudio,flac,opus,ac3
  --enable-bsf=extract_extradata,h264_mp4toannexb,hevc_mp4toannexb,aac_adtstoasc
  --disable-decoders
  --disable-encoders
  --disable-muxers
  --disable-filters
  --disable-hwaccels
  --disable-indevs
  --disable-outdevs
  --extra-cflags="${FF_EXTRA_CFLAGS}"
  --extra-ldflags="${FF_EXTRA_LDFLAGS}"
)
if [[ "${MS_SIZE_OPT:-0}" == "1" ]]; then
  CFG_ARGS+=(--enable-small)
fi
# OpenSSL 用钉选静态 prefix（PKG_CONFIG_PATH 仅把该 prefix 放在最前）。
# libxml2 仍为系统动态库，不用 pkg-config --static，避免绑死 liblzma。
# 最终 libmsff.so 显式链 libssl.a/libcrypto.a，不得再 NEEDED 系统 libssl。


STAMP="$FFMPEG_ROOT/configure.stamp"
REQUESTED_FLAGS="$FFMPEG_ROOT/configure-requested.txt"

# 当前请求的配置单独落盘，不得在跳过重建时改写「实际建成」的 configure-flags.txt。
{
  echo "# requested $(date -Iseconds)"
  echo "source=$SRC"
  echo "prefix=$PREFIX"
  printf 'configure'
  for a in "${CFG_ARGS[@]}"; do printf ' %q' "$a"; done
  echo
} > "$REQUESTED_FLAGS"

ms_ff_write_stamp() {
  local dest="$1"
  {
    echo "tarball_sha256=$tarball_sha256"
    echo "ms_size_opt=${MS_SIZE_OPT:-0}"
    echo "openssl_prefix=$OPENSSL_PREFIX"
    if [[ -f "$OPENSSL_STAMP" ]]; then
      echo "openssl_pin=$(tr -d '\n' < "$OPENSSL_STAMP")"
    else
      echo "openssl_pin="
    fi
    local p
    for p in "$HLS_SUB_PATCH" "$HLS_SUB_TL_PATCH" "$HLS_LIVE_WIN_PATCH"; do
      if [[ -f "$p" ]]; then
        sha256sum "$p"
      fi
    done
    echo "args:"
    printf '%s\n' "${CFG_ARGS[@]}"
  } > "$dest"
}

ms_ff_comp_macro() {
  echo "$1" | tr 'a-z' 'A-Z' | tr '-' '_'
}

# 用已安装/构建树的 config_components.h 核对本趟 --enable-demuxer/parser/bsf 与 SMALL/DECODERS。
ms_ff_components_match() {
  local comp="" conf="" item macro
  for comp in "$FFMPEG_ROOT/config_components.h" "$BUILDDIR/config_components.h"; do
    if [[ -f "$comp" ]]; then
      break
    fi
    comp=""
  done
  for conf in "$FFMPEG_ROOT/config.h" "$BUILDDIR/config.h"; do
    if [[ -f "$conf" ]]; then
      break
    fi
    conf=""
  done
  if [[ -z "$comp" ]]; then
    echo "no config_components.h beside $FFMPEG_ROOT or $BUILDDIR" >&2
    return 1
  fi
  for a in "${CFG_ARGS[@]}"; do
    case "$a" in
      --enable-demuxer=*)
        IFS=',' read -r -a _items <<< "${a#--enable-demuxer=}"
        for item in "${_items[@]}"; do
          macro="CONFIG_$(ms_ff_comp_macro "$item")_DEMUXER"
          if ! grep -qE "^#define ${macro} 1[[:space:]]*$" "$comp"; then
            echo "stale/missing $macro=1 (wanted demuxer $item) in $comp" >&2
            return 1
          fi
        done
        ;;
      --enable-parser=*)
        IFS=',' read -r -a _items <<< "${a#--enable-parser=}"
        for item in "${_items[@]}"; do
          macro="CONFIG_$(ms_ff_comp_macro "$item")_PARSER"
          if ! grep -qE "^#define ${macro} 1[[:space:]]*$" "$comp"; then
            echo "stale/missing $macro=1 (wanted parser $item) in $comp" >&2
            return 1
          fi
        done
        ;;
      --enable-bsf=*)
        IFS=',' read -r -a _items <<< "${a#--enable-bsf=}"
        for item in "${_items[@]}"; do
          macro="CONFIG_$(ms_ff_comp_macro "$item")_BSF"
          if ! grep -qE "^#define ${macro} 1[[:space:]]*$" "$comp"; then
            echo "stale/missing $macro=1 (wanted bsf $item) in $comp" >&2
            return 1
          fi
        done
        ;;
    esac
  done
  if [[ -n "$conf" ]]; then
    if grep -qE '^#define CONFIG_DECODERS 1' "$conf"; then
      echo "prefix has CONFIG_DECODERS=1; refusing" >&2
      return 1
    fi
    if grep -qE '^#define CONFIG_MPEG4VIDEO_PARSER 1' "$comp"; then
      echo "prefix has CONFIG_MPEG4VIDEO_PARSER=1 (pulls mpegvideodec); refusing" >&2
      return 1
    fi
    if grep -qE '^#define CONFIG_MPEGVIDEODEC 1' "$conf"; then
      echo "prefix has CONFIG_MPEGVIDEODEC=1; refusing" >&2
      return 1
    fi
    if [[ "${MS_SIZE_OPT:-0}" == "1" ]]; then
      if ! grep -qE '^#define CONFIG_SMALL 1' "$conf"; then
        echo "MS_SIZE_OPT=1 but CONFIG_SMALL is not 1 in $conf" >&2
        return 1
      fi
    fi
  fi
  return 0
}

ms_ff_write_stamp "$STAMP.expected"

# 复用已有前缀的唯一充分证明：OpenSSL pin 一致，且 configure.stamp 与本趟期望逐字节相同。
# 组件清单只能作附加核对，不能在无 stamp 或 stamp 不一致时签发新指纹
# （补丁、extra-cflags、源码版本、OpenSSL pin 都不会被组件宏证明）。
ms_ff_prefix_reusable() {
  if [[ ! -f "$PREFIX/lib/libavformat.a" || "${FFMPEG_REBUILD:-0}" == 1 ]]; then
    return 1
  fi
  if [[ ! -f "$OPENSSL_STAMP" || ! -f "$FF_SSL_STAMP" ]] || ! cmp -s "$OPENSSL_STAMP" "$FF_SSL_STAMP"; then
    echo "ffmpeg prefix OpenSSL pin missing or changed vs $OPENSSL_PREFIX" >&2
    return 1
  fi
  if [[ ! -f "$STAMP" ]]; then
    echo "ffmpeg prefix has no configure.stamp; not issuing a stamp from components alone" >&2
    return 1
  fi
  if ! cmp -s "$STAMP" "$STAMP.expected"; then
    echo "ffmpeg prefix configure.stamp differs from current pin/patches/configure args" >&2
    return 1
  fi
  if ! ms_ff_components_match; then
    echo "ffmpeg prefix stamp matched but config_components do not; treating as stale" >&2
    return 1
  fi
  return 0
}

ms_ff_cache_check_only() {
  [[ "${MS_FF_CACHE_CHECK_ONLY:-0}" == "1" ]]
}

ms_ff_emit_or_cleanup_expected() {
  if [[ -n "${MS_FF_EMIT_EXPECTED:-}" ]]; then
    cp "$STAMP.expected" "$MS_FF_EMIT_EXPECTED"
  fi
  rm -f "$STAMP.expected"
}

if ms_ff_prefix_reusable; then
  echo "ffmpeg 9.1 prefix already present at $PREFIX (configure stamp matches; set FFMPEG_REBUILD=1 to rebuild)"
  ms_ff_emit_or_cleanup_expected
  if ms_ff_cache_check_only; then
    echo "cache-decision: SKIP"
  fi
  exit 0
fi

if [[ -f "$PREFIX/lib/libavformat.a" && "${FFMPEG_REBUILD:-0}" != 1 ]]; then
  echo "ffmpeg prefix at $PREFIX is not reusable as the current configuration (missing/mismatched stamp, OpenSSL pin, or components)."
  echo "Rebuilding this FFmpeg prefix only. Frozen Android size artifacts and historical build/release are not this tree."
  echo "To keep an old prefix, point FFMPEG_BUILD_ROOT at a new directory instead of overwriting it."
fi

if ms_ff_cache_check_only; then
  echo "cache-decision: REBUILD"
  ms_ff_emit_or_cleanup_expected
  exit 3
fi

# 隔离自测才允许假安装：只写入 stamp/flags 或在写入前注入失败，不跑 configure/make。
ms_ff_selftest_root_ok() {
  case "$FFMPEG_ROOT" in
    */ffmpeg-cache-selftest|*/ffmpeg-cache-selftest/*)
      return 0
      ;;
  esac
  return 1
}

if [[ "${MS_FF_FAKE_INSTALL:-0}" == "1" ]]; then
  if ! ms_ff_selftest_root_ok; then
    echo "error: MS_FF_FAKE_INSTALL refused outside ffmpeg-cache-selftest root ($FFMPEG_ROOT)" >&2
    rm -f "$STAMP.expected"
    exit 2
  fi
  if [[ "${MS_FF_INJECT_BUILD_FAILURE:-0}" == "1" ]]; then
    echo "error: injected build failure before writing configure.stamp" >&2
    rm -f "$STAMP.expected"
    exit 1
  fi
  {
    echo "# FFmpeg 9.1 trimmed configure (built $(date -Iseconds))"
    echo "source=$SRC"
    echo "prefix=$PREFIX"
    printf 'configure'
    for a in "${CFG_ARGS[@]}"; do printf ' %q' "$a"; done
    echo
  } | tee "$FLAGS_LOG" >/dev/null
  ms_ff_write_stamp "$STAMP"
  if [[ -f "$OPENSSL_STAMP" ]]; then
    cp "$OPENSSL_STAMP" "$FF_SSL_STAMP"
  fi
  rm -f "$STAMP.expected"
  echo "fake-install recorded stamp at $STAMP (selftest only)"
  exit 0
fi
rm -f "$STAMP.expected"

mkdir -p "$BUILDDIR"

# 若先前失败留下半成品，允许重新 configure
rm -rf "$BUILDDIR"
mkdir -p "$BUILDDIR"
# 上面 FLAGS_LOG 写在 BUILDDIR 的父目录，安全
cd "$BUILDDIR"
# 钉选 OpenSSL 的 pkg-config 优先；其后才是系统路径（供 libxml2）。不用系统 libav 的 -L。
# 不用 pkg-config --static（会把 libxml2 的 lzma 等静态依赖绑进来）。
SYS_PC="/usr/lib/x86_64-linux-gnu/pkgconfig:/usr/lib/pkgconfig"
env -u CPATH -u C_INCLUDE_PATH -u LIBRARY_PATH -u OPENSSL_CONF -u OPENSSL_MODULES \
  PKG_CONFIG_PATH="$OPENSSL_PREFIX/lib/pkgconfig:$SYS_PC" \
  "$SRC/configure" "${CFG_ARGS[@]}"
if [[ "${MS_SIZE_OPT:-0}" == "1" ]]; then
  ev=""
  for _mak in ffbuild/config.mak config.mak; do
    if [[ -f "$_mak" ]]; then
      ev="$(awk -F= '/^CFLAGS=/{print; exit}' "$_mak" || true)"
      echo "flags-file=$_mak" | tee -a "$FLAGS_LOG"
      break
    fi
  done
  echo "effective $ev" | tee -a "$FLAGS_LOG"
  last=$(echo "$ev" | grep -oE -- '-O([0-9sz]|fast|g)' | tail -1 || true)
  if [[ "$last" != "-Os" ]]; then
    echo "error: FFmpeg config.mak last -O is ${last:-none}, expected -Os (would be overwritten)" >&2
    echo "$ev" >&2
    exit 1
  fi
  if ! grep -E '^#define CONFIG_SMALL 1' config.h >/dev/null; then
    echo "error: --enable-small did not set CONFIG_SMALL" >&2
    exit 1
  fi
fi
# 许可与解码器自检：禁止 GPL/nonfree，禁止误开 decoder
if grep -E '^#define CONFIG_GPL 1' config.h >/dev/null || \
   grep -E '^#define CONFIG_NONFREE 1' config.h >/dev/null; then
  echo "error: configure enabled GPL or nonfree" >&2
  exit 1
fi
if grep -E '^#define CONFIG_DECODERS 1' config.h >/dev/null; then
  echo "error: configure enabled decoders; refusing silent soft-decode" >&2
  exit 1
fi
make -j"$(nproc)"
make install
# 注入失败只用于 cache 自测，且必须在成功写入 stamp 之前退出。
if [[ "${MS_FF_INJECT_BUILD_FAILURE:-0}" == "1" ]]; then
  echo "error: injected build failure before writing configure.stamp" >&2
  exit 1
fi
# 记录 license 字符串与实际建成配置（仅在真正 configure/install 后写入）
{
  echo "# FFmpeg 9.1 trimmed configure (built $(date -Iseconds))"
  echo "source=$SRC"
  echo "prefix=$PREFIX"
  printf 'configure'
  for a in "${CFG_ARGS[@]}"; do printf ' %q' "$a"; done
  echo
} | tee "$FLAGS_LOG"
if [[ -f "$BUILDDIR/config.h" ]]; then
  rg "FFMPEG_LICENSE|CONFIG_GPL |CONFIG_NONFREE |CONFIG_DECODERS |CONFIG_SMALL " "$BUILDDIR/config.h" \
    | tee "$FFMPEG_ROOT/license-config.txt" || true
  cp "$BUILDDIR/config.h" "$FFMPEG_ROOT/config.h"
fi
if [[ -f "$BUILDDIR/config_components.h" ]]; then
  cp "$BUILDDIR/config_components.h" "$FFMPEG_ROOT/config_components.h"
fi
ms_ff_write_stamp "$STAMP"
if [[ -f "$OPENSSL_STAMP" ]]; then
  cp "$OPENSSL_STAMP" "$FF_SSL_STAMP"
fi
echo "installed $PREFIX (OpenSSL prefix $OPENSSL_PREFIX)"
