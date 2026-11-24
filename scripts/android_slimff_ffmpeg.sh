#!/usr/bin/env bash
# 路线 B：在新目录交叉编译无静态 OpenSSL 的 FFmpeg 9.1 前缀（供 single-slimff 单 so）。
# 启用自研 androidtls 后端以满足 tls/https/rtmps 依赖，不链 libssl/libcrypto。
# 不改 third_party/cache、不改冻结的 ffmpeg-9.1-size / android-*/size。
# 用法：ABIS="armeabi-v7a arm64-v8a" scripts/android_slimff_ffmpeg.sh
# 输出：build/android-v7a/ffmpeg-9.1-slimff/prefix、build/android-v8a/ffmpeg-9.1-slimff/prefix
# by: Long <mycjzb@gmail.com>
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
NDK="${ANDROID_NDK_HOME:-${ANDROID_NDK_ROOT:-/home/box/.local/android-ndk-r29}}"
API="${ANDROID_API:-21}"
PREBUILT="$NDK/toolchains/llvm/prebuilt/linux-x86_64"
export PATH="/usr/bin:/bin:$PREBUILT/bin:${PATH}"
CACHE="$ROOT/third_party/cache"
SRC_BASE="$CACHE/ffmpeg-9.1"
SLIM_SRC="$ROOT/build/single-analysis/ffmpeg-9.1-slimff-src"
ABIS="${ABIS:-armeabi-v7a arm64-v8a}"
WITH_XML="${WITH_XML:-1}"
# ADEC=1：仅为系统普遍缺失的音频开 FFmpeg 内置软解（ac3/eac3/mp2/pcm_alaw/pcm_mulaw/dca/aac_latm；aac_latm 依赖 aac 解码器；eac3 依赖 ac3 浮点解码器，故不另开 ac3_fixed），输出到 -adec 新前缀；
# 视频软解一律不开。默认 0 与原 slimff 前缀一致（CONFIG_DECODERS=0）。
ADEC="${ADEC:-0}"
ADEC_LIST="${ADEC_LIST:-ac3,eac3,mp2,pcm_alaw,pcm_mulaw,dca,aac_latm}"
# ADEC_TAG：可选输出后缀（逐个解码器量体积增量时用），默认无后缀即交付前缀 ffmpeg-9.1-slimff-adec。
ADEC_TAG="${ADEC_TAG:-}"
JOBS="${JOBS:-8}"
# 阶段 4 深度裁剪：TRIM=s4 去冷门格式（ogg/microdvd/vobsub demuxer、rtmpt/srtp 协议；image2/imf 本就未开，另加断言），
# 常用字幕所需 demuxer（mpegts/matroska/mov/webvtt/srt/ass）全部保留；音频软解列表不变。
# XML_FLAVOR=ms：dashdec 链自研 XML 垫片（scripts/android_msxml_shim.sh 产物），不再链 libxml2。
# FF_OPT=oz-lto：-Oz + ThinLTO（静态库为位码，单 so 最终链接时统一优化）。默认 os 与原前缀一致。
# 阶段 5：TRIM=s5 = s4 配置 + slimff_patch_s5.py 的 MS_SLIM_S5 守卫补丁（h264dsp/exif 摘除、pixdesc/codec_desc
# 精简、AVOption 帮助文本置空），零功能损失。FF_OPT=oz-flto：-Oz + Full LTO（不启用 relr，保持 API 18）。
# 阶段 6：TRIM=s6 = s5 + MS_SLIM_S6（见 scripts/slimff_patch_s6_hls.py），输出前缀用 ADEC_TAG=s6。
# by: Long <mycjzb@gmail.com>
TRIM="${TRIM:-}"
XML_FLAVOR="${XML_FLAVOR:-libxml2}"
FF_OPT="${FF_OPT:-os}"
# CAVS_PARSER=1（默认）：开 cavsvideo parser 给 AVS/AVS+ 切帧并从序列头取宽高（只含 parser，不含 cavs 视频解码器）。
CAVS_PARSER="${CAVS_PARSER:-1}"

if [[ ! -x "$SRC_BASE/configure" ]]; then
  echo "error: missing $SRC_BASE/configure" >&2
  exit 2
fi

# 公共：准备一份仅用于 slimff 的源码副本并打上 androidtls 依赖补丁。
prepare_src() {
  mkdir -p "$(dirname "$SLIM_SRC")"
  if [[ ! -f "$SLIM_SRC/.msmedia-androidtls-patched" ]]; then
    echo "slimff-ffmpeg: copying source to $SLIM_SRC"
    rm -rf "$SLIM_SRC"
    cp -a "$SRC_BASE" "$SLIM_SRC"
    # 加入假外部库 androidtls：只为满足 tls_protocol_deps_any，不拉 OpenSSL。
    if ! grep -q "^    androidtls$" "$SLIM_SRC/configure"; then
      sed -i '/^    openssl$/a\    androidtls' "$SLIM_SRC/configure"
    fi
    sed -i 's/tls_protocol_deps_any="gnutls openssl schannel securetransport libtls mbedtls"/tls_protocol_deps_any="androidtls gnutls openssl schannel securetransport libtls mbedtls"/' \
      "$SLIM_SRC/configure"
    touch "$SLIM_SRC/.msmedia-androidtls-patched"
  fi
  # ff_tls_protocol 只保留一份实现：src/ff/tls_android.c 在 libmsplayer.so 最终链接时编译，
  # FFmpeg 构建系统不再编译它（避免两份 TLS 代码）。
  sed -i '/tls_android.o/d' "$SLIM_SRC/libavformat/Makefile"
  rm -f "$SLIM_SRC/libavformat/tls_android.c"
  # --disable-neon 时 tx.c 仍无条件引用 aarch64 NEON codelet 表（其 .S 未编译），
  # 最终链接会缺 ff_tx_*_neon。仅在 slim 源码副本里加 HAVE_NEON 守卫，走 C 版 FFT/MDCT。
  sed -i 's/^#if ARCH_AARCH64$/#if ARCH_AARCH64 \&\& HAVE_NEON/' "$SLIM_SRC/libavutil/tx.c"
  # 本路线只开 ac3/eac3(float MDCT)/mp2(自带 DCT32)，double/int32 变换码表无人使用（约 80KB）。
  # 只从 slim 副本的 codelet 表里摘掉；若有人请求 double/int32 变换，av_tx_init 诚实返回 ENOSYS。
  # 用户确认 ASF/WMV、RM/RMVB 不再需要：去掉 asf/rm demuxer，RTSP/RTP 里依赖它们的负载改为诚实失败桩。
  python3 "$ROOT/scripts/slimff_patch_noasfrm.py" "$SLIM_SRC"
  # 阶段 4：HLS 不看后缀 + 切片起点图片伪装头/脏字节剥离（调用单 so 内公共 ms_probe）。
  python3 "$ROOT/scripts/slimff_patch_hls_probe.py" "$SLIM_SRC"
  # 阶段 5：补丁全部在 MS_SLIM_S5 守卫内，未定义该宏时与 s4 源码等价（幂等）。
  python3 "$ROOT/scripts/slimff_patch_s5.py" "$SLIM_SRC"
  # 阶段 6：Media3 风格 HLS 补丁，全部在 MS_SLIM_S6 守卫内（未定义时与 s5 源码等价，幂等）。
  python3 "$ROOT/scripts/slimff_patch_s6_hls.py" "$SLIM_SRC"
  sed -i -e 's/^    ff_tx_codelet_list_double_c,$/    \/* msmedia slim: double 变换不编入 *\//' \
         -e 's/^    ff_tx_codelet_list_int32_c,$/    \/* msmedia slim: int32 变换不编入 *\//' "$SLIM_SRC/libavutil/tx.c"
}

# 公共：为一个 ABI 配置并安装到新前缀。
build_abi() {
  local abi="$1"
  local tag cc cxx arch cpu xml_p out extra_c extra_ld
  case "$abi" in
    armeabi-v7a)
      tag=android-v7a
      cc="$PREBUILT/bin/armv7a-linux-androideabi${API}-clang"
      cxx="$PREBUILT/bin/armv7a-linux-androideabi${API}-clang++"
      arch=arm
      cpu=armv7-a
      extra_c="-fPIC -Os -ffunction-sections -fdata-sections -fno-lto -fno-unwind-tables -fno-asynchronous-unwind-tables -march=armv7-a -mfloat-abi=softfp -mfpu=vfpv3-d16"
      extra_ld="-Wl,--gc-sections"
      xml_p="$ROOT/build/android-v7a/libxml2-2.13.8/prefix"
      ;;
    arm64-v8a)
      tag=android-v8a
      cc="$PREBUILT/bin/aarch64-linux-android${API}-clang"
      cxx="$PREBUILT/bin/aarch64-linux-android${API}-clang++"
      arch=aarch64
      cpu=armv8-a
      extra_c="-fPIC -Os -ffunction-sections -fdata-sections -fno-lto -fno-unwind-tables -fno-asynchronous-unwind-tables -march=armv8-a"
      extra_ld="-Wl,--gc-sections"
      xml_p="$ROOT/build/android-v8a/libxml2-2.13.8/prefix"
      ;;
    x86)
      # 仅供 API 18 模拟器（i686）调试；不是交付 ABI。
      tag=android-x86
      cc="$PREBUILT/bin/i686-linux-android${API}-clang"
      cxx="$PREBUILT/bin/i686-linux-android${API}-clang++"
      arch=x86
      cpu=i686
      extra_c="-fPIC -Os -ffunction-sections -fdata-sections -fno-lto -fno-unwind-tables -fno-asynchronous-unwind-tables"
      extra_ld="-Wl,--gc-sections"
      xml_p="$ROOT/build/android-x86/libxml2-2.13.8/prefix"
      ;;
    x86_64)
      # 仅供模拟器调试 APK；不是交付 ABI。
      tag=android-x86_64
      cc="$PREBUILT/bin/x86_64-linux-android${API}-clang"
      cxx="$PREBUILT/bin/x86_64-linux-android${API}-clang++"
      arch=x86_64
      cpu=x86-64
      extra_c="-fPIC -Os -ffunction-sections -fdata-sections -fno-lto -fno-unwind-tables -fno-asynchronous-unwind-tables"
      extra_ld="-Wl,--gc-sections"
      xml_p="$ROOT/build/android-x86_64/libxml2-2.13.8/prefix"
      ;;
    *) echo "error: unsupported ABI $abi" >&2; exit 2 ;;
  esac
  if [[ "$XML_FLAVOR" == ms ]]; then
    xml_p="$ROOT/build/$tag/msxml-shim/prefix"
  fi
  if [[ "$FF_OPT" == oz-lto ]]; then
    extra_c="${extra_c/-Os/-Oz}"
    extra_c="${extra_c/-fno-lto/-flto=thin}"
    extra_ld="$extra_ld -flto=thin"
  elif [[ "$FF_OPT" == oz-flto ]]; then
    extra_c="${extra_c/-Os/-Oz}"
    extra_c="${extra_c/-fno-lto/-flto=full}"
    extra_ld="$extra_ld -flto=full"
  fi
  [[ "$TRIM" == s5 ]] && extra_c="$extra_c -DMS_SLIM_S5=1"
  # 阶段 6：s5 全部瘦身 + MS_SLIM_S6（hls.c 对标 Media3：切片计划下发给单 so 内预下载器、m3u8 刷新/切片打开退避重试）。
  [[ "$TRIM" == s6 ]] && extra_c="$extra_c -DMS_SLIM_S5=1 -DMS_SLIM_S6=1"
  # FF_EXTRA_CFLAGS：仅用于逐项量化（如 -DMS_S5_SKIP_PIXDESC 留一法），交付构建不设。
  [[ -n "${FF_EXTRA_CFLAGS:-}" ]] && extra_c="$extra_c $FF_EXTRA_CFLAGS"
  out="$ROOT/build/$tag/ffmpeg-9.1-slimff"
  [[ "$ADEC" == 1 ]] && out="$ROOT/build/$tag/ffmpeg-9.1-slimff-adec${ADEC_TAG:+-$ADEC_TAG}"
  local prefix="$out/prefix" builddir="$out/build"
  if [[ -f "$prefix/lib/libavformat.a" && -f "$prefix/.msmedia-slimff-ok" && "${FFMPEG_SLIM_REBUILD:-0}" != 1 ]]; then
    echo "slimff-ffmpeg: $abi prefix already present, skip (set FFMPEG_SLIM_REBUILD=1 to rebuild)"
    return 0
  fi
  rm -rf "$builddir" "$prefix"
  mkdir -p "$builddir" "$prefix"
  local cfg=(
    --prefix="$prefix"
    --enable-cross-compile
    --arch="$arch"
    --cpu="$cpu"
    --target-os=android
    --cc="$cc"
    --cxx="$cxx"
    --ar="$PREBUILT/bin/llvm-ar"
    --nm="$PREBUILT/bin/llvm-nm"
    --ranlib="$PREBUILT/bin/llvm-ranlib"
    --strip="$PREBUILT/bin/llvm-strip"
    --sysroot="$PREBUILT/sysroot"
    --enable-pic --enable-static --disable-shared
    --disable-doc --disable-htmlpages --disable-manpages --disable-podpages --disable-txtpages
    --disable-programs --disable-ffmpeg --disable-ffplay --disable-ffprobe
    --disable-avdevice --disable-avfilter --disable-swscale --disable-swresample
    --disable-everything
    --enable-avutil --enable-avcodec --enable-avformat --enable-network
    --disable-openssl --enable-androidtls
    --disable-gpl --disable-version3 --disable-nonfree
    --enable-protocol=file,http,https,tls,tcp,udp,rtp,rtmp,rtmps,rtmpt,crypto,pipe,fd,srtp
    --enable-demuxer=mpegts,mpegtsraw,flv,live_flv,mov,hls,dash,rtsp,rtp,sdp,aac,h264,hevc,ogg,matroska,webvtt,avi,mpegps,mp3,flac,wav,ac3,eac3,srt,ass,microdvd,vobsub
    --enable-parser=h264,hevc,mpegvideo,aac,aac_latm,mpegaudio,flac,opus,ac3
    --enable-bsf=extract_extradata,h264_mp4toannexb,hevc_mp4toannexb,aac_adtstoasc
    --disable-decoders --disable-encoders --disable-muxers --disable-filters
    --disable-hwaccels --disable-indevs --disable-outdevs
    --disable-linux-perf --disable-x86asm --disable-neon
    --enable-small
    --extra-ldflags="$extra_ld"
  )
  if [[ "$TRIM" == s4 || "$TRIM" == s5 || "$TRIM" == s6 ]]; then
    local newcfg=() a
    for a in "${cfg[@]}"; do
      case "$a" in
        --enable-protocol=*) a="--enable-protocol=file,http,https,tls,tcp,udp,rtp,rtmp,rtmps,crypto,pipe,fd" ;;
        --enable-demuxer=*) a="--enable-demuxer=mpegts,mpegtsraw,flv,live_flv,mov,hls,dash,rtsp,rtp,sdp,aac,h264,hevc,matroska,webvtt,avi,mpegps,mp3,flac,wav,ac3,eac3,srt,ass" ;;
      esac
      newcfg+=("$a")
    done
    cfg=("${newcfg[@]}")
  fi
  local pcpath=""
  if [[ "$WITH_XML" == 1 ]]; then
    if [[ ! -f "$xml_p/lib/libxml2.a" ]]; then
      echo "error: missing $xml_p/lib/libxml2.a (reuse existing prefix, do not rebuild into frozen trees)" >&2
      exit 2
    fi
    cfg+=(--enable-libxml2)
    extra_c="$extra_c -I$xml_p/include -I$xml_p/include/libxml2"
    pcpath="$xml_p/lib/pkgconfig"
  else
    cfg+=(--disable-libxml2)
    # 无 libxml2 时 dash demuxer 无法启用（deps=libxml2），诚实去掉。
    local newcfg=() a
    for a in "${cfg[@]}"; do
      case "$a" in --enable-demuxer=*) continue ;; esac
      newcfg+=("$a")
    done
    if [[ "$TRIM" == s4 || "$TRIM" == s5 || "$TRIM" == s6 ]]; then
      cfg=("${newcfg[@]}" --enable-demuxer=mpegts,mpegtsraw,flv,live_flv,mov,hls,rtsp,rtp,sdp,aac,h264,hevc,matroska,webvtt,avi,mpegps,mp3,flac,wav,ac3,eac3,srt,ass)
    else
      cfg=("${newcfg[@]}" --enable-demuxer=mpegts,mpegtsraw,flv,live_flv,mov,hls,rtsp,rtp,sdp,aac,h264,hevc,ogg,matroska,webvtt,avi,mpegps,mp3,flac,wav,ac3,eac3,srt,ass,microdvd,vobsub)
    fi
  fi
  if [[ "$ADEC" == 1 ]]; then
    cfg+=(--enable-decoder="$ADEC_LIST")
    # DTS：裸 .dts 需要 dts demuxer，TS/MKV 里的 DTS 需要 dca parser 组帧（都很小，不拉其它模块）。
    if [[ ",$ADEC_LIST," == *",dca,"* ]]; then
      cfg+=(--enable-parser=dca --enable-demuxer=dts)
    fi
  fi
  if [[ "$CAVS_PARSER" == 1 ]]; then
    cfg+=(--enable-parser=cavsvideo)
  fi
  cfg+=(--extra-cflags="$extra_c")
  {
    echo "# slimff android $abi $(date '+%Y-%m-%d %H:%M:%S') Asia/Shanghai"
    echo "with_xml=$WITH_XML xml_flavor=$XML_FLAVOR trim=${TRIM:-none} ff_opt=$FF_OPT"
    printf 'configure'
    local a
    for a in "${cfg[@]}"; do printf ' %q' "$a"; done
    echo
  } | tee "$out/configure-flags.txt"
  cd "$builddir"
  if [[ -n "$pcpath" ]]; then
    env -u CPATH -u C_INCLUDE_PATH -u LIBRARY_PATH -u OPENSSL_CONF -u OPENSSL_MODULES \
      PKG_CONFIG_PATH="$pcpath" PKG_CONFIG_LIBDIR="$pcpath" \
      "$SLIM_SRC/configure" "${cfg[@]}"
  else
    env -u CPATH -u C_INCLUDE_PATH -u LIBRARY_PATH -u OPENSSL_CONF -u OPENSSL_MODULES \
      "$SLIM_SRC/configure" "${cfg[@]}"
  fi
  # 强制 -Os 在 CFLAGS 最后；禁止 OpenSSL。
  if grep -E '^#define CONFIG_OPENSSL 1' config.h >/dev/null; then
    echo "error: slimff ffmpeg unexpectedly has CONFIG_OPENSSL" >&2
    exit 1
  fi
  if ! grep -E '^#define CONFIG_TLS_PROTOCOL 1' config.h config_components.h >/dev/null 2>&1; then
    echo "error: slimff ffmpeg missing CONFIG_TLS_PROTOCOL" >&2
    exit 1
  fi
  if [[ "$ADEC" != 1 ]] && grep -E '^#define CONFIG_DECODERS 1' config.h config_components.h >/dev/null 2>&1; then
    echo "error: slimff ffmpeg CONFIG_DECODERS=1" >&2
    exit 1
  fi
  if [[ "$ADEC" == 1 ]]; then
    # 只允许白名单音频解码器；任何视频/其它解码器出现即失败。
    local bad
    bad=$(grep -E '^#define CONFIG_[A-Z0-9_]+_DECODER 1' config_components.h | sed -E 's/#define CONFIG_([A-Z0-9_]+)_DECODER 1/\1/' | tr 'A-Z' 'a-z' | grep -vxE "$(echo "$ADEC_LIST" | tr ',' '|')$( [[ ",$ADEC_LIST," == *",aac_latm,"* ]] && echo '|aac')" || true)
    if [[ -n "$bad" ]]; then
      echo "error: unexpected decoders enabled: $bad" >&2
      exit 1
    fi
    grep -E '^#define CONFIG_[A-Z0-9_]+_DECODER 1' config_components.h | tee "$out/enabled-decoders.txt"
  fi
  if grep -E '^#define CONFIG_(ASF|ASF_O|RM)_DEMUXER 1' config_components.h >/dev/null 2>&1; then
    echo "error: slimff ffmpeg still has asf/rm demuxer" >&2
    exit 1
  fi
  if grep -E '^#define CONFIG_GPL 1' config.h config_components.h >/dev/null 2>&1; then
    echo "error: slimff ffmpeg CONFIG_GPL=1" >&2
    exit 1
  fi
  if [[ "$TRIM" == s4 || "$TRIM" == s5 || "$TRIM" == s6 ]]; then
    # 裁剪断言：冷门格式/协议必须为 0，常用字幕 demuxer 必须为 1。
    local c
    for c in OGG_DEMUXER MICRODVD_DEMUXER VOBSUB_DEMUXER IMAGE2_DEMUXER IMF_DEMUXER RTMPT_PROTOCOL SRTP_PROTOCOL \
             JACOSUB_DEMUXER MPL2_DEMUXER SAMI_DEMUXER REALTEXT_DEMUXER; do
      if grep -E "^#define CONFIG_${c} 1" config_components.h >/dev/null 2>&1; then
        echo "error: TRIM=s4 still has CONFIG_${c}" >&2
        exit 1
      fi
    done
    for c in MPEGTS_DEMUXER MATROSKA_DEMUXER MOV_DEMUXER WEBVTT_DEMUXER SRT_DEMUXER ASS_DEMUXER HLS_DEMUXER; do
      if ! grep -E "^#define CONFIG_${c} 1" config_components.h >/dev/null 2>&1; then
        echo "error: TRIM=s4 lost CONFIG_${c} (subtitle/container keep list)" >&2
        exit 1
      fi
    done
  fi
  if [[ "$XML_FLAVOR" == ms && "$WITH_XML" == 1 ]] && ! grep -q "msxml read-only shim" "$xml_p/lib/pkgconfig/libxml-2.0.pc"; then
    echo "error: XML_FLAVOR=ms but $xml_p is not the msxml shim" >&2
    exit 1
  fi
  local optflag="-Os"
  [[ "$FF_OPT" == oz-lto ]] && optflag="-Oz -flto=thin"
  [[ "$FF_OPT" == oz-flto ]] && optflag="-Oz -flto=full"
  [[ "$TRIM" == s5 ]] && optflag="$optflag -DMS_SLIM_S5=1"
  [[ "$TRIM" == s6 ]] && optflag="$optflag -DMS_SLIM_S5=1 -DMS_SLIM_S6=1"
  [[ -n "${FF_EXTRA_CFLAGS:-}" ]] && optflag="$optflag $FF_EXTRA_CFLAGS"
  awk -v o="$optflag" '{if($0 ~ /^CFLAGS=/ || $0 ~ /^CXXFLAGS=/){print $0 " " o} else print}' ffbuild/config.mak > ffbuild/config.mak.adj
  mv ffbuild/config.mak.adj ffbuild/config.mak
  make -j"$JOBS"
  make install
  {
    echo "abi=$abi date_cst=$(date '+%Y-%m-%d %H:%M:%S') Asia/Shanghai"
    echo "CONFIG_OPENSSL=$(grep CONFIG_OPENSSL "$prefix/include/libavutil/ffversion.h" 2>/dev/null || grep CONFIG_OPENSSL $builddir/config.h | head -1)"
    grep -E 'CONFIG_(OPENSSL|TLS_PROTOCOL|HTTPS_PROTOCOL|RTMPS_PROTOCOL|LIBXML2|DECODERS|GPL) ' "$builddir/config.h" || true
    ls -l "$prefix/lib/"*.a
  } | tee "$prefix/.msmedia-slimff-ok"
  echo "slimff-ffmpeg: installed $prefix"
}

prepare_src
for abi in $ABIS; do
  build_abi "$abi"
done
echo "slimff-ffmpeg: done"
