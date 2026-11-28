#!/usr/bin/env bash
# 非正式 x86（i686）体积/模拟器验收前缀：只在 build/android-x86 下新建 OpenSSL/FFmpeg/libxml2 size-opt 前缀。
# 供 API 18/19 x86 AVD 测试 APK 使用；不是默认交付。
# 不覆盖 build/android-v7a、build/android-v8a、build/release 及任何冻结产物。
# 默认不链接历史 size 对照 so（本 ABI 无冻结制品）；由 android_hw_x86_build.sh 重链 libmsff。
# by: Long <mycjzb@gmail.com>
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
source "$ROOT/scripts/android_env.sh"
NDK="$MS_NDK"
API="${ANDROID_API:-21}"
PREBUILT="$NDK/toolchains/llvm/prebuilt/linux-x86_64"
export ANDROID_NDK_ROOT="$NDK"
export ANDROID_NDK_HOME="$NDK"
export ANDROID_API="$API"
export PATH="$PREBUILT/bin:/usr/bin:/bin"
export OPENSSL_TARGET=android-x86

CC="$PREBUILT/bin/i686-linux-android${API}-clang"
CXX="$PREBUILT/bin/i686-linux-android${API}-clang++"
AR="$PREBUILT/bin/llvm-ar"
NM="$PREBUILT/bin/llvm-nm"
RANLIB="$PREBUILT/bin/llvm-ranlib"
STRIP="$PREBUILT/bin/llvm-strip"
SYSROOT="$PREBUILT/sysroot"

if [[ ! -x "$CC" || ! -d "$SYSROOT" ]]; then
  echo "error: NDK r29 i686 API $API clang/sysroot missing under $NDK" >&2
  exit 2
fi

CACHE="$ROOT/third_party/cache"
XML_VER=2.13.8
XML_TAR="$CACHE/libxml2-${XML_VER}.tar.xz"
XML_SHA="277294cb33119ab71b2bc81f2f445e9bc9435b893ad15bb2cd2b0e859a0ee84a"
XML_ROOT="$ROOT/build/android-x86/libxml2-${XML_VER}"
XML_PREFIX="$XML_ROOT/prefix"
OUT_ROOT="$ROOT/build/android-x86"
mkdir -p "$OUT_ROOT"

need() { command -v "$1" >/dev/null || { echo "error: missing $1" >&2; exit 2; }; }
need make; need tar; need sha256sum

if [[ ! -f "$XML_TAR" ]]; then
  echo "error: missing $XML_TAR (informal libxml2 for x86 (i686))" >&2
  exit 1
fi
got="$(sha256sum "$XML_TAR" | awk '{print $1}')"
if [[ "$got" != "$XML_SHA" ]]; then
  echo "error: libxml2 tarball sha256 mismatch: $got" >&2
  exit 1
fi

if [[ ! -f "$XML_PREFIX/lib/libxml2.a" || "${XML_REBUILD:-0}" == 1 ]]; then
  rm -rf "$XML_ROOT/src" "$XML_ROOT/build"
  mkdir -p "$XML_ROOT/build"
  tar -xJf "$XML_TAR" -C "$XML_ROOT"
  mv "$XML_ROOT/libxml2-${XML_VER}" "$XML_ROOT/src"
  cd "$XML_ROOT/build"
  env -u CPATH -u C_INCLUDE_PATH -u LIBRARY_PATH \
    CC="$CC" AR="$AR" RANLIB="$RANLIB" \
    CFLAGS="-fPIC -Os -ffunction-sections -fdata-sections -D__ANDROID_API__=${API}" \
    "$XML_ROOT/src/configure" \
      --host=i686-linux-android \
      --prefix="$XML_PREFIX" \
      --with-sysroot="$SYSROOT" \
      --enable-static --disable-shared \
      --without-python --without-lzma --without-iconv \
      --without-http --without-ftp --without-modules --without-legacy \
      --with-zlib="$SYSROOT/usr" \
      --disable-dependency-tracking
  make -j"$(nproc)"
  make install
fi
if [[ ! -f "$XML_PREFIX/lib/libxml2.a" ]]; then
  echo "error: libxml2.a not installed" >&2
  exit 1
fi

ANDROID_CFLAGS_COMMON="-fPIC -fno-lto"
ANDROID_CFLAGS_SIZE="-Os -ffunction-sections -fdata-sections ${ANDROID_CFLAGS_COMMON}"

build_ffmpeg_android_size() {
  local ssl_root ff_root extra_c extra_ld
  ssl_root="$OUT_ROOT/openssl-3.5.9-size"
  if [[ "${ANDROID_SIZE_MOREFMT:-0}" == 1 ]]; then
    ff_root="$OUT_ROOT/ffmpeg-9.1-size-morefmt"
  else
    ff_root="$OUT_ROOT/ffmpeg-9.1-size"
  fi
  extra_c="${ANDROID_CFLAGS_SIZE} -I${ssl_root}/prefix/include -I${XML_PREFIX}/include -I${XML_PREFIX}/include/libxml2"
  extra_ld="-Wl,--gc-sections -L${ssl_root}/prefix/lib -L${XML_PREFIX}/lib -L${SYSROOT}/usr/lib/i686-linux-android/${API}"

  if ! env -u CC -u CXX -u LD PATH="$PREBUILT/bin:/usr/bin:/bin" \
      ANDROID_NDK_ROOT="$NDK" ANDROID_API="$API" \
      OPENSSL_TARGET=android-x86 \
      OPENSSL_BUILD_ROOT="$ssl_root" OPENSSL_PREFIX="$ssl_root/prefix" \
      MS_SIZE_OPT=1 \
      bash "$ROOT/scripts/openssl_build.sh"; then
    echo "error: android-x86 OpenSSL size build failed" >&2
    return 1
  fi

  local prefix="$ff_root/prefix"
  local builddir="$ff_root/build"
  mkdir -p "$ff_root"
  if [[ -f "$prefix/lib/libavformat.a" && "${FFMPEG_REBUILD:-0}" != 1 ]]; then
    echo "ffmpeg android x86 (i686) size already at $prefix"
    return 0
  fi
  rm -rf "$builddir"
  mkdir -p "$builddir" "$prefix"
  cd "$builddir"
  local src="$ROOT/third_party/cache/ffmpeg-9.1"
  if [[ ! -d "$src" ]]; then
    # 与 v7a/v8a 脚本一致：解压钉选源码到 cache（若已由既有流程展开则直接用）
    src="$CACHE/ffmpeg-9.1"
  fi
  if [[ ! -d "$src" ]]; then
    echo "error: missing FFmpeg 9.1 sources (expected $src)" >&2
    exit 1
  fi

  HLS_SUB_PATCH="$ROOT/patches/ffmpeg-9.1-hls-sub-rendition-switch.patch"
  if [[ -f "$HLS_SUB_PATCH" ]]; then
    if ! grep -q "MsMedia: do not catch-up-drop subtitle cues" "$src/libavformat/hls.c"; then
      patch -p1 -d "$src" < "$HLS_SUB_PATCH"
    fi
  fi
  HLS_SUB_TL_PATCH="$ROOT/patches/ffmpeg-9.1-hls-sub-timeline.patch"
  if [[ -f "$HLS_SUB_TL_PATCH" ]]; then
    if ! grep -q "MsMedia: keep overlapping subtitle cues on seek" "$src/libavformat/hls.c" || \
       ! grep -q "MsMedia: apply X-TIMESTAMP-MAP" "$src/libavformat/webvttdec.c"; then
      patch -p1 -d "$src" < "$HLS_SUB_TL_PATCH"
    fi
  fi
  HLS_LIVE_WIN_PATCH="$ROOT/patches/ffmpeg-9.1-hls-live-window.patch"
  if [[ -f "$HLS_LIVE_WIN_PATCH" ]]; then
    if ! grep -q "MsMedia: publish HLS live window in metadata" "$src/libavformat/hls.c"; then
      patch -p1 -d "$src" < "$HLS_LIVE_WIN_PATCH"
    fi
  fi

  MPEGTS_SHORT_PROBE_PATCH="$ROOT/patches/ffmpeg-9.1-mpegts-short-probe.patch"
  if [[ -f "$MPEGTS_SHORT_PROBE_PATCH" ]]; then
    if ! grep -q "MsMedia: short TS (fewer than 10 FEC-sized slots)" "$src/libavformat/mpegts.c"; then
      patch -p1 -d "$src" < "$MPEGTS_SHORT_PROBE_PATCH"
    fi
  fi
  local cfg=(
    --prefix="$prefix"
    --enable-cross-compile
    --arch=x86
    --cpu=i686
    --target-os=android
    --cc="$CC"
    --cxx="$CXX"
    --ar="$AR"
    --nm="$NM"
    --ranlib="$RANLIB"
    --strip="$STRIP"
    --sysroot="$SYSROOT"
    --enable-pic
    --enable-static
    --disable-shared
    --disable-doc --disable-htmlpages --disable-manpages --disable-podpages --disable-txtpages
    --disable-programs --disable-ffmpeg --disable-ffplay --disable-ffprobe
    --disable-avdevice --disable-avfilter --disable-swscale --disable-swresample
    --disable-everything
    --enable-avutil --enable-avcodec --enable-avformat --enable-network
    --enable-openssl --enable-libxml2
    --disable-gpl --disable-version3 --disable-nonfree
    --enable-protocol=file,http,https,tls,tcp,udp,rtp,rtmp,rtmps,rtmpt,crypto,pipe,fd,srtp
    --enable-demuxer=mpegts,mpegtsraw,flv,live_flv,mov,hls,dash,rtsp,rtp,sdp,aac,h264,hevc,ogg,matroska,webvtt,avi,mpegps,mp3,flac,wav,asf,rm,ac3,eac3,srt,ass,microdvd,vobsub
    --enable-parser=h264,hevc,mpegvideo,aac,aac_latm,mpegaudio,flac,opus,ac3
    --enable-bsf=extract_extradata,h264_mp4toannexb,hevc_mp4toannexb,aac_adtstoasc
    --disable-decoders --disable-encoders --disable-muxers --disable-filters
    --disable-hwaccels --disable-indevs --disable-outdevs
    --disable-linux-perf --disable-x86asm
    --enable-small
    --extra-cflags="$extra_c"
    --extra-ldflags="$extra_ld"
  )
  {
    echo "# informal android x86 (i686) FFmpeg size $(date -Iseconds)"
    printf 'configure'
    local a
    for a in "${cfg[@]}"; do printf ' %q' "$a"; done
    echo
  } | tee "$ff_root/configure-flags.txt"
  env -u CPATH -u C_INCLUDE_PATH -u LIBRARY_PATH -u OPENSSL_CONF -u OPENSSL_MODULES \
    PKG_CONFIG_PATH="$ssl_root/prefix/lib/pkgconfig:$XML_PREFIX/lib/pkgconfig" \
    PKG_CONFIG_LIBDIR="$ssl_root/prefix/lib/pkgconfig:$XML_PREFIX/lib/pkgconfig" \
    "$src/configure" "${cfg[@]}"
  local ev last want
  ev="$(awk -F= '/^CFLAGS=/{print; exit}' ffbuild/config.mak || true)"
  echo "effective $ev" | tee -a "$ff_root/configure-flags.txt"
  want="-Os"
  if ! grep -E '^#define CONFIG_SMALL 1' config.h >/dev/null; then
    echo "error: android ffmpeg --enable-small missing" >&2
    exit 1
  fi
  last=$(echo "$ev" | grep -oE -- '-O([0-9sz]|fast|g)' | tail -1 || true)
  if [[ "$last" != "$want" ]]; then
    echo "warning: android x86 ffmpeg size last -O is ${last:-none}, appending $want" | tee -a "$ff_root/configure-flags.txt"
    awk -v w="$want" '{if($0 ~ /^CFLAGS=/ || $0 ~ /^CXXFLAGS=/){print $0 " " w} else print}' ffbuild/config.mak > ffbuild/config.mak.adj && mv ffbuild/config.mak.adj ffbuild/config.mak
    ev="$(awk -F= '/^CFLAGS=/{print; exit}' ffbuild/config.mak || true)"
    echo "adjusted $ev" | tee -a "$ff_root/configure-flags.txt"
    last=$(echo "$ev" | grep -oE -- '-O([0-9sz]|fast|g)' | tail -1 || true)
    if [[ "$last" != "$want" ]]; then
      echo "error: failed to make $want the last -O ($ev)" >&2
      exit 1
    fi
  fi
  if grep -E '^#define CONFIG_GPL 1' config.h >/dev/null || \
     grep -E '^#define CONFIG_NONFREE 1' config.h >/dev/null; then
    echo "error: android ffmpeg enabled GPL/nonfree" >&2
    exit 1
  fi
  if grep -E '^#define CONFIG_DECODERS 1' config.h >/dev/null; then
    echo "error: android ffmpeg enabled decoders" >&2
    exit 1
  fi
  make -j"$(nproc)"
  make install
  rg "FFMPEG_LICENSE|CONFIG_GPL |CONFIG_NONFREE |CONFIG_DECODERS |CONFIG_SMALL " config.h \
    | tee "$ff_root/license-config.txt" || true
}

build_ffmpeg_android_size
if [[ "${ANDROID_SIZE_MOREFMT:-0}" == 1 && "${ANDROID_SIZE_FFMPEG_ONLY:-0}" != 1 ]]; then
  out="$OUT_ROOT/size-morefmt"
  ssl_p="$OUT_ROOT/openssl-3.5.9-size/prefix"
  ff_p="$OUT_ROOT/ffmpeg-9.1-size-morefmt/prefix"
  cflags="-std=c11 -Os -DNDEBUG -ffunction-sections -fdata-sections ${ANDROID_CFLAGS_COMMON}"
  mkdir -p "$out/obj"
  "$CC" $cflags -fPIC -fvisibility=hidden -DMSMEDIA_BUILD_SHARED -pthread \
    -I"$ROOT/include" -I"$ROOT/src/ff" -I"$ROOT/src/container" -I"$ROOT/src/core" \
    -I"$ff_p/include" -I"$ssl_p/include" \
    -c "$ROOT/src/ff/ms_ff.c" -o "$out/obj/ms_ff.o"
  "$CC" $cflags -fPIC -fvisibility=hidden -pthread -I"$ROOT/include" -I"$ROOT/src/core" \
    -c "$ROOT/src/container/ms_ts_scan.c" -o "$out/obj/ms_ts_scan.o"
  "$CC" $cflags -fPIC -fvisibility=hidden -pthread -I"$ROOT/include" -I"$ROOT/src/core" \
    -c "$ROOT/src/ff/ms_ff_scan_support.c" -o "$out/obj/ms_ff_scan_support.o"
  "$CC" $cflags -fPIC -fvisibility=hidden -pthread -I"$ROOT/include" \
    -c "$ROOT/src/codec/ms_hevc.c" -o "$out/obj/ms_hevc.o"
  "$CC" $cflags -fPIC -fvisibility=hidden -pthread -I"$ROOT/include" \
    -c "$ROOT/src/codec/ms_hdr.c" -o "$out/obj/ms_hdr.o"
  "$CC" $cflags -fPIC -fvisibility=hidden -pthread -I"$ROOT/include" \
    -c "$ROOT/src/codec/ms_vbit.c" -o "$out/obj/ms_vbit.o"
  "$CC" $cflags -fPIC -fvisibility=hidden -pthread -I"$ROOT/include" \
    -c "$ROOT/src/codec/ms_abit.c" -o "$out/obj/ms_abit.o"
  "$CC" $cflags -fPIC -fvisibility=hidden -pthread -I"$ROOT/include" \
    -c "$ROOT/src/codec/ms_avcc.c" -o "$out/obj/ms_avcc.o"
  # 与宿主 libmsff 相同：隐藏编入同一份 ms_sub，满足外挂/内嵌字幕符号，避免 NEEDED libmsmedia。
  "$CC" $cflags -fPIC -fvisibility=hidden -pthread -I"$ROOT/include" \
    -c "$ROOT/src/sub/ms_sub.c" -o "$out/obj/ms_sub.o"
  "$CC" $cflags -fPIC -fvisibility=hidden -shared -pthread \
    -Wl,-soname,libmsff.so.1 -Wl,--version-script="$ROOT/src/ff/libmsff.map" \
    -Wl,--gc-sections -Wl,--exclude-libs,ALL -Wl,-z,defs -Wl,-z,norelro \
    -o "$out/libmsff.so.1.0.0.unstripped" "$out/obj/ms_ff.o" \
    "$out/obj/ms_ts_scan.o" "$out/obj/ms_ff_scan_support.o" "$out/obj/ms_hevc.o" \
    "$out/obj/ms_hdr.o" "$out/obj/ms_vbit.o" "$out/obj/ms_abit.o" "$out/obj/ms_avcc.o" "$out/obj/ms_sub.o" \
    "$ff_p/lib/libavformat.a" "$ff_p/lib/libavcodec.a" "$ff_p/lib/libavutil.a" \
    "$ssl_p/lib/libssl.a" "$ssl_p/lib/libcrypto.a" \
    "$XML_PREFIX/lib/libxml2.a" \
    -L"$SYSROOT/usr/lib/i686-linux-android/${API}" \
    -lz -lm -ldl -lc
  cp -f "$out/libmsff.so.1.0.0.unstripped" "$out/libmsff.so.1.0.0"
  "$STRIP" --strip-unneeded "$out/libmsff.so.1.0.0"
  echo "android size-morefmt libmsff $(wc -c < "$out/libmsff.so.1.0.0") bytes stripped under $out"
fi
echo "android x86 (i686) informal size-opt prefixes ready under $OUT_ROOT (no frozen artifacts touched)"
