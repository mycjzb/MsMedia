#!/usr/bin/env bash
# 单 so 合并构建（阶段 1：路线 A 纯自研；阶段 2：路线 A 加网络层 + 路线 B 精简 FFmpeg 合并）。
# 把自研核心（libmsmedia 源码）+ 硬件播放/JNI 层（libmshw 源码）合并编译为统一交付名 libmsplayer.so。
# 不链 libmsff / libmsass / libmsai / OpenSSL；libmsass、libmsai 仍可在运行时 dlopen（缺失诚实降级，不 NEEDED）。
# 现有多 so APK / MsHwPlayerActivity（loadLibrary mshw/msmedia/msff）不改；本产物供单 so 包用 loadLibrary("msplayer")。
# 用 --gc-sections / ICF / hidden + 仅导出 JNI 的 version script 清掉未引用算法与模块（见 MEASURE 中 gc_dropped）。
# 不改 scripts/android_hw_build.sh、不写 build/android-hw、不碰冻结目录；不用 UPX。
# 阶段 2：
#   - 两条路线都并入同一份自研网络层 src/netcore（TCP/HTTP/DNS 钩子/SOCKS5 + 安卓系统 TLS JNI 泵），
#     供 com.mycjzb.media.player.MsHttpClient 与播放器共用；0 字节 OpenSSL。
#   - MODE=pure   -> build/android-*/single-native-s2/（阶段 1 的 single-native/ 冻结，不覆盖）
#   - MODE=slimff -> build/android-*/single-slimff/：再并入 ms_ff 适配器 + 精简 FFmpeg 9.1 静态库
#                    （--disable-openssl --enable-androidtls；ff_tcp_protocol/ff_tls_protocol 由
#                    src/ff/tcp_ms.c、src/ff/tls_android.c 覆盖为 netcore 实现）+ libxml2（DASH）。
#     ADEC=1（默认）用 ffmpeg-9.1-slimff-adec 前缀（内置 ac3,eac3,mp2 音频软解，仅本路线）；
#     ADEC=0 用无音频软解前缀，输出 single-slimff-noadec/，只用于量出软解字节增量。
#     WITH_XML=0 不链 libxml2（DASH 诚实不支持），仅在体积超限时作为第二刀。
# 用法：
#   scripts/android_single_build.sh            # 默认 MODE=pure OPT=os，两 ABI
#   OPT=oz-lto scripts/android_single_build.sh # -Oz + ThinLTO 对照（默认可作为主交付）
#   ABIS="armeabi-v7a" scripts/android_single_build.sh
# 输出：
#   build/android-v7a/<outsub>/<opt>/libmsplayer.so
#   build/android-v8a/<outsub>/<opt>/libmsplayer.so
#   若 OPT 为默认交付档，另复制到 build/android-*/<outsub>/libmsplayer.so
# by: Long <mycjzb@gmail.com>
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
NDK="${ANDROID_NDK_HOME:-${ANDROID_NDK_ROOT:-/home/box/.local/android-ndk-r29}}"
API="${ANDROID_API:-21}"
PREBUILT="$NDK/toolchains/llvm/prebuilt/linux-x86_64"
export PATH="/usr/bin:/bin:$PREBUILT/bin:${PATH}"
STRIP="$PREBUILT/bin/llvm-strip"
READELF="$PREBUILT/bin/llvm-readelf"
SYSROOT="$PREBUILT/sysroot"
MODE="${MODE:-pure}"
OPT="${OPT:-os}"
ABIS="${ABIS:-armeabi-v7a arm64-v8a}"
LIMIT=2097152
MAP="$ROOT/src/single/libmssingle.map"

ADEC="${ADEC:-1}"
WITH_XML="${WITH_XML:-1}"
# 阶段 6（路线 B 主力）：
#   TLS=openssl —— 静态合入 scripts/openssl_slim_build.sh 的极致裁剪 OpenSSL（openssl-3.5.9-slim-flto），
#                  netcore TLS 泵改用 src/netcore/ms_nc_tls_ossl.c（播放拉流默认忽略证书 / MsHttpClient 严格），
#                  不再编入 JNI SSLEngine 泵 ms_android_tls.c；TLS=system（默认）保持原系统 TLS。
#   HLS_PF=1    —— 编入 Media3 风格 HLS 独立预下载 src/ff/ms_hls_pf.c（需 TRIM=s6 的 FFmpeg 前缀，ADEC_TAG=s6）。
# by: Long <mycjzb@gmail.com>
TLS="${TLS:-system}"
HLS_PF="${HLS_PF:-0}"
case "$MODE" in
  pure)   OUTSUB="${SINGLE_OUTSUB:-single-native-s2}" ;;
  slimff)
    if [[ "$ADEC" == 1 ]]; then OUTSUB="${SINGLE_OUTSUB:-single-slimff${ADEC_TAG:+-adec-$ADEC_TAG}}"; else OUTSUB="${SINGLE_OUTSUB:-single-slimff-noadec}"; fi
    [[ "$WITH_XML" == 1 ]] || OUTSUB="${OUTSUB}-noxml" ;;
  *) echo "error: unknown MODE=$MODE (pure|slimff)" >&2; exit 2 ;;
esac
# 阶段 1 交付目录冻结：不允许任何模式覆盖。
if [[ "$OUTSUB" == "single-native" ]]; then
  echo "error: build/android-*/single-native is the frozen stage-1 output; pick another SINGLE_OUTSUB" >&2
  exit 2
fi
SLIM_SRC="$ROOT/build/single-analysis/ffmpeg-9.1-slimff-src"

# 自研核心：与 android_hw_build.sh 的 CORE_SRCS 同一清单（不另起平行实现）。
CORE_SRCS=(
  src/core/ms_crc32.c src/core/ms_util.c src/core/ms_registry.c src/core/ms_session.c
  src/core/ms_probe.c
  src/codec/ms_adts.c src/codec/ms_h264.c src/codec/ms_hevc.c src/codec/ms_hdr.c
  src/codec/ms_vbit.c src/codec/ms_abit.c src/codec/ms_avcc.c
  src/sub/ms_sub.c
  src/container/ms_ts.c src/container/ms_ts_scan.c src/container/ms_bmff.c
  src/container/ms_bmff_sample.c src/container/ms_flv.c
  src/source/ms_source.c src/hls/ms_hls.c src/dash/ms_dash.c
)
# 阶段 5（零功能损失瘦身第 1 项）：路线 B 的解封装全部走 FFmpeg（本地文件亦经 file: 交给 ms_ff），
# 以下自研解封装/会话源文件只服务路线 A，slimff 不再编入。SLIM_CORE_SPLIT=0 可恢复全量编入作对照。
# 链接带 -z defs：若仍有引用会直接链接失败，不会静默缺功能。by: Long <mycjzb@gmail.com>
SLIMFF_SKIP_CORE=(
  src/core/ms_session.c src/core/ms_registry.c
  src/container/ms_ts.c src/container/ms_bmff.c src/container/ms_bmff_sample.c src/container/ms_flv.c
  src/source/ms_source.c src/hls/ms_hls.c src/dash/ms_dash.c
)
SLIM_CORE_SPLIT="${SLIM_CORE_SPLIT:-1}"
# 硬件播放 + JNI：与 android_hw_build.sh 的 SRCS 同一清单（ms_hw_android.c 另按 NDK/JNI 双编译）。
HW_SRCS=(
  src/hw/ms_hw_name.c src/hw/ms_hw_csd.c src/hw/ms_hw.c src/hw/ms_hw_vaapi.c
  src/hw/ms_hw_player.c src/hw/ms_media3_audio_clock.c src/hw/ms_media3_video_release.c src/hw/ms_hw_ass_bind.c src/hw/ms_hw_ai_bind.c src/hw/ms_hw_reap.c
  src/hw/ms_hw_audio.c src/hw/ms_hw_audio_trim.c src/hw/ms_hw_jni.c src/hw/ms_hw_api.c
  src/hw/ms_hw_mediandk.c src/hw/ms_hw_android_dispatch.c src/hw/ms_hw_player_jni.c
  src/hw/ms_hw_probe.c src/hw/ms_hw_passthrough.c
)
# 阶段 2 公共网络层（两路线共用一份）：netcore + MsHttpClient JNI + 系统 TLS JNI 泵。
NET_SRCS=(
  src/netcore/ms_netcore.c src/netcore/ms_netcore_jni.c src/ff/ms_android_tls.c
)
# 阶段 4 路线 A 额外：纯自研网络输入（HTTP 渐进/HLS 预加载/DASH/UDP/RTP/RTMP/RTSP）、
# 本地 MKV/PS 转封装与 TS 坏包过滤。路线 B 由 FFmpeg 承担，不编入以免重复。
PURE_NET_SRCS=(
  src/netsrc/ms_netsrc.c src/netsrc/ms_flvw.c src/netsrc/ms_mkvps.c src/netsrc/ms_tsguard.c
  src/netsrc/ms_proto_udp.c src/netsrc/ms_proto_rtmp.c src/netsrc/ms_proto_rtsp.c
)
# 路线 B 额外：ms_ff 适配器 + FFmpeg tcp/tls 协议覆盖（走 netcore）。
FF_SRCS=(
  # ms_ff_scan_support.c 只给 libmsff 补 ms_size_add；单 so 里核心 ms_util.c 已提供，不重复编入。
  src/ff/ms_ff.c src/ff/tcp_ms.c src/ff/tls_android.c
  src/compat/ms_api18_libc.c
)
ANDROID_NDK_DEFS=(
  -Dms_hw_android_query=ms_hw_android_ndk_query
  -Dms_hw_android_configure=ms_hw_android_ndk_configure
  -Dms_hw_android_start=ms_hw_android_ndk_start
  -Dms_hw_android_submit=ms_hw_android_ndk_submit
  -Dms_hw_android_drive=ms_hw_android_ndk_drive
  -Dms_hw_android_flush=ms_hw_android_ndk_flush
  -Dms_hw_android_close=ms_hw_android_ndk_close
  -Dms_hw_android_copy_last_output=ms_hw_android_ndk_copy_last_output
)
ANDROID_JNI_DEFS=(
  -DMS_HW_LEGACY_JNI
  -Dms_hw_android_query=ms_hw_android_jni_query
  -Dms_hw_android_configure=ms_hw_android_jni_configure
  -Dms_hw_android_start=ms_hw_android_jni_start
  -Dms_hw_android_submit=ms_hw_android_jni_submit
  -Dms_hw_android_drive=ms_hw_android_jni_drive
  -Dms_hw_android_flush=ms_hw_android_jni_flush
  -Dms_hw_android_close=ms_hw_android_jni_close
)

# 公共：按优化档位给出编译/链接选项（两 ABI 共用，避免重复）。
opt_flags() {
  case "$OPT" in
    os)     OPT_CFLAGS="-Os";           OPT_LDFLAGS="-Wl,--icf=all" ;;
    oz)     OPT_CFLAGS="-Oz";           OPT_LDFLAGS="-Wl,--icf=all" ;;
    oz-lto) OPT_CFLAGS="-Oz -flto=thin"; OPT_LDFLAGS="-flto=thin -Wl,--icf=all -Wl,--lto-O2" ;;
    # 阶段 5：Full LTO（整程序单模块优化）。不加 --pack-dyn-relocs=android+relr（API 23+/28+），保持 API 18 兼容。
    oz-flto) OPT_CFLAGS="-Oz -flto=full"; OPT_LDFLAGS="-flto=full -Wl,--icf=all -Wl,--lto-O2" ;;
    *) echo "error: unknown OPT=$OPT" >&2; exit 2 ;;
  esac
}

# 公共：构建单个 ABI 的单 so，并做 NEEDED/UND/体积检查。
build_abi() {
  local abi="$1" cc arch outdir objdir libdir abidir
  case "$abi" in
    armeabi-v7a)
      cc="$PREBUILT/bin/armv7a-linux-androideabi${API}-clang"
      arch="-march=armv7-a -mfloat-abi=softfp -mfpu=vfpv3-d16 -mthumb"
      libdir="$SYSROOT/usr/lib/arm-linux-androideabi/${API}"
      outdir="$ROOT/build/android-v7a/$OUTSUB/$OPT"; abidir="$ROOT/build/android-v7a" ;;
    arm64-v8a)
      cc="$PREBUILT/bin/aarch64-linux-android${API}-clang"
      arch="-march=armv8-a"
      libdir="$SYSROOT/usr/lib/aarch64-linux-android/${API}"
      outdir="$ROOT/build/android-v8a/$OUTSUB/$OPT"; abidir="$ROOT/build/android-v8a" ;;
    x86)
      # 仅供 API 18 模拟器（i686）调试，不是交付 ABI。
      cc="$PREBUILT/bin/i686-linux-android${API}-clang"
      arch=""
      libdir="$SYSROOT/usr/lib/i686-linux-android/${API}"
      outdir="$ROOT/build/android-x86/$OUTSUB/$OPT"; abidir="$ROOT/build/android-x86" ;;
    x86_64)
      # 仅供模拟器调试 APK，不是交付 ABI。
      cc="$PREBUILT/bin/x86_64-linux-android${API}-clang"
      arch=""
      libdir="$SYSROOT/usr/lib/x86_64-linux-android/${API}"
      outdir="$ROOT/build/android-x86_64/$OUTSUB/$OPT"; abidir="$ROOT/build/android-x86_64" ;;
    *) echo "error: unsupported ABI $abi" >&2; exit 2 ;;
  esac
  [[ -x "$cc" ]] || { echo "error: missing $cc" >&2; exit 2; }
  objdir="$outdir/obj"
  rm -rf "$objdir"; mkdir -p "$objdir"
  local cflags="-fPIC $OPT_CFLAGS -DNDEBUG -ffunction-sections -fdata-sections -fvisibility=hidden \
-DMSMEDIA_BUILD_SHARED -D__ANDROID__ -DMS_SINGLE_SO=1 -Wall -Wextra -Wno-unused-parameter -std=c11 \
-fno-unwind-tables -fno-asynchronous-unwind-tables -fno-stack-protector-strong \
-I$ROOT/include -I$ROOT/src/hw -I$ROOT/src/core -I$ROOT/src/codec -I$ROOT/src/container $arch"
  # -fno-stack-protector-strong 不是有效选项时回退为保留栈保护（安全优先，不主动关）
  cflags="${cflags/-fno-stack-protector-strong /}"
  local objs=() s o extra_defs="" ff_p="" ff_b="" xml_p="" ff_libs=() ff_inc="" ssl_p="" ssl_inc="" net_srcs=("${NET_SRCS[@]}")
  if [[ "$TLS" == openssl ]]; then
    ssl_p="$abidir/openssl-3.5.9-slim-flto/prefix"
    for s in "$ssl_p/lib/libssl.a" "$ssl_p/lib/libcrypto.a"; do
      [[ -f "$s" ]] || { echo "error: missing $s (run scripts/openssl_slim_build.sh)" >&2; exit 2; }
    done
    ssl_inc="-DMS_NC_TLS_OPENSSL -I$ssl_p/include"
    net_srcs=(src/netcore/ms_netcore.c src/netcore/ms_netcore_jni.c src/netcore/ms_nc_tls_ossl.c)
  fi
  if [[ "$MODE" == slimff ]]; then
    if [[ "$ADEC" == 1 ]]; then
      ff_p="$abidir/ffmpeg-9.1-slimff-adec${ADEC_TAG:+-$ADEC_TAG}/prefix"; ff_b="$abidir/ffmpeg-9.1-slimff-adec${ADEC_TAG:+-$ADEC_TAG}/build"
      extra_defs="-DMS_HW_HAVE_FF -DMSMEDIA_NO_OPENSSL -DMS_SINGLE_FFADEC"
    else
      ff_p="$abidir/ffmpeg-9.1-slimff/prefix"; ff_b="$abidir/ffmpeg-9.1-slimff/build"
      extra_defs="-DMS_HW_HAVE_FF -DMSMEDIA_NO_OPENSSL"
    fi
    # 默认用精简 libxml2（scripts/android_slim_libxml2.sh：minimum+tree+threads，无 .eh_frame）；
    # XML_FLAVOR=full 回到多 so 同款 libxml2，仅用于对照体积。
    # 阶段 4：XML_FLAVOR=ms 链自研 ms_xml 垫片（scripts/android_msxml_shim.sh），彻底去掉 libxml2。
    if [[ "${XML_FLAVOR:-slim}" == ms ]]; then xml_p="$abidir/msxml-shim/prefix";
    elif [[ "${XML_FLAVOR:-slim}" == slim ]]; then xml_p="$abidir/libxml2-2.13.8-slim/prefix"; else xml_p="$abidir/libxml2-2.13.8/prefix"; fi
    for s in "$ff_p/lib/libavformat.a" "$ff_p/lib/libavcodec.a" "$ff_p/lib/libavutil.a" "$ff_b/config.h"; do
      [[ -f "$s" ]] || { echo "error: missing $s (run scripts/android_slimff_ffmpeg.sh, ADEC=$ADEC)" >&2; exit 2; }
    done
    ff_libs=("$ff_p/lib/libavformat.a" "$ff_p/lib/libavcodec.a" "$ff_p/lib/libavutil.a")
    if [[ "$WITH_XML" == 1 ]]; then
      [[ -f "$xml_p/lib/libxml2.a" ]] || { echo "error: missing $xml_p/lib/libxml2.a" >&2; exit 2; }
      ff_libs+=("$xml_p/lib/libxml2.a")
    fi
    ff_inc="-I$ROOT/src/ff -I$ff_p/include"
  fi
  echo "single[$abi/$OPT/$MODE]: compile core+hw+net${ff_p:+ +ff($ff_p)}"
  [[ "$HLS_PF" == 1 ]] && extra_defs="$extra_defs -DMS_HLS_PREFETCH"
  for s in "${CORE_SRCS[@]}" "${HW_SRCS[@]}" "${net_srcs[@]}"; do
    if [[ "$MODE" == slimff && "$SLIM_CORE_SPLIT" == 1 && " ${SLIMFF_SKIP_CORE[*]} " == *" $s "* ]]; then
      continue
    fi
    o="$objdir/$(echo "${s#src/}" | tr '/' '_' | sed 's/\.c$/.o/')"
    "$cc" $cflags $extra_defs $ssl_inc $ff_inc -c "$ROOT/$s" -o "$o"
    objs+=("$o")
  done
  if [[ "$MODE" != slimff ]]; then
    for s in "${PURE_NET_SRCS[@]}"; do
      o="$objdir/$(echo "${s#src/}" | tr '/' '_' | sed 's/\.c$/.o/')"
      "$cc" $cflags -I"$ROOT/src" -c "$ROOT/$s" -o "$o"
      objs+=("$o")
    done
  fi
  if [[ "$MODE" == slimff ]]; then
    local ffsrcs=("${FF_SRCS[@]}")
    [[ "$ADEC" == 1 ]] && ffsrcs+=(src/hw/ms_hw_ffadec.c)
    [[ "$HLS_PF" == 1 ]] && ffsrcs+=(src/ff/ms_hls_pf.c)
    for s in "${ffsrcs[@]}"; do
      o="$objdir/$(echo "${s#src/}" | tr '/' '_' | sed 's/\.c$/.o/')"
      # tcp_ms.c / tls_android.c 用 FFmpeg 内部头（url.h/network.h/tls.h + 构建目录 config.h）。
      "$cc" $cflags $extra_defs $ssl_inc $ff_inc -I"$ff_b" -I"$SLIM_SRC" -I"$SLIM_SRC/libavformat" \
        -Wno-sign-compare -Wno-missing-field-initializers -c "$ROOT/$s" -o "$o"
      objs+=("$o")
    done
  fi
  "$cc" $cflags $extra_defs "${ANDROID_NDK_DEFS[@]}" -c "$ROOT/src/hw/ms_hw_android.c" -o "$objdir/hw_ms_hw_android_ndk.o"
  "$cc" $cflags $extra_defs "${ANDROID_JNI_DEFS[@]}" -c "$ROOT/src/hw/ms_hw_android.c" -o "$objdir/hw_ms_hw_android_jni.o"
  objs+=("$objdir/hw_ms_hw_android_ndk.o" "$objdir/hw_ms_hw_android_jni.o")

  # 自研对象在前，确保 ff_tcp_protocol/ff_tls_protocol 由 netcore 版本解析（不拉 FFmpeg 的 tcp.o）。
  local zlib=""
  [[ "$MODE" == slimff ]] && zlib="-lz"
  # 极致裁剪 OpenSSL 静态库（LTO 位码）放在 FFmpeg 之后，libssl 先于 libcrypto。
  [[ -n "$ssl_p" ]] && ff_libs+=("$ssl_p/lib/libssl.a" "$ssl_p/lib/libcrypto.a")
  "$cc" -shared -fPIC $OPT_CFLAGS $OPT_LDFLAGS -Wl,-soname,libmsplayer.so \
    -Wl,--version-script="$MAP" -Wl,--gc-sections -Wl,--exclude-libs,ALL -Wl,-z,defs \
    -Wl,--build-id=none -Wl,-Map="$outdir/libmsplayer.map.txt" ${STRIP_EH:+-Wl,--no-eh-frame-hdr} \
    -o "$outdir/libmsplayer.unstripped.so" "${objs[@]}" "${ff_libs[@]}" \
    -L"$libdir" -landroid -llog $zlib -lm -ldl -lc
  cp -f "$outdir/libmsplayer.unstripped.so" "$outdir/libmsplayer.so"
  "$STRIP" --strip-all "$outdir/libmsplayer.so"

  # 检查：NEEDED 只允许安卓系统库；不得出现 libmsff/libmsass/libmsmedia。
  local needed bytes sha
  needed=$("$READELF" -d "$outdir/libmsplayer.so" | awk '/NEEDED/ {gsub(/[\[\]]/,"",$5); print $5}' | tr '\n' ' ')
  for n in $needed; do
    case "$n" in
      libandroid.so|liblog.so|libm.so|libdl.so|libc.so) ;;
      libz.so) [[ "$MODE" == slimff ]] || { echo "error: libz NEEDED in pure mode" >&2; exit 1; } ;;
      *) echo "error: unexpected NEEDED $n in $abi single so" >&2; exit 1 ;;
    esac
  done
  bytes=$(wc -c < "$outdir/libmsplayer.so")
  sha=$(sha256sum "$outdir/libmsplayer.so" | awk '{print $1}')
  # 用户已放宽：略超 2MB（几百 KB）可接受。默认只告警并记入 MEASURE；STRICT_LIMIT=1 时超限失败。
  local over=0
  if (( bytes > LIMIT )); then
    over=$((bytes - LIMIT))
    echo "warning: $abi single so $bytes > $LIMIT (+$over)" >&2
    [[ "${STRICT_LIMIT:-0}" == 1 ]] && { echo "error: STRICT_LIMIT" >&2; exit 1; }
  fi
  {
    echo "tls=$TLS hls_pf=$HLS_PF"
    echo "abi=$abi mode=$MODE adec=$ADEC with_xml=$WITH_XML xml_flavor=${XML_FLAVOR:-slim} outsub=$OUTSUB opt=$OPT api=$API date_cst=$(date '+%Y-%m-%d %H:%M:%S') Asia/Shanghai"
    echo "file=$outdir/libmsplayer.so bytes=$bytes sha256=$sha limit=$LIMIT margin=$((LIMIT - bytes))"
    echo "unstripped_bytes=$(wc -c < "$outdir/libmsplayer.unstripped.so")"
    echo "NEEDED: $needed"
    echo "exported_dynsym=$("$READELF" --dyn-syms -W "$outdir/libmsplayer.so" | awk '$5=="GLOBAL" && $7!="UND"' | wc -l)"
    echo "jni_exports=$("$READELF" --dyn-syms -W "$outdir/libmsplayer.so" | awk '$5=="GLOBAL" && $7!="UND" && $8 ~ /^(Java_|JNI_OnLoad)/' | wc -l)"
    echo "undefined_imports=$("$READELF" --dyn-syms -W "$outdir/libmsplayer.so" | awk '$7=="UND" && $8!=""' | wc -l)"
    "$READELF" -S -W "$outdir/libmsplayer.so" | awk '$2 ~ /^\.(text|rodata|data|data\.rel\.ro|bss|dynsym|dynstr|rel\.dyn|rela\.dyn|ARM\.exidx|eh_frame)$/ {print "section " $2 " size=0x" $6}'
  } | tee "$outdir/MEASURE.txt"
  echo "gc_dropped_objs=$(python3 "$ROOT/scripts/single_gc_report.py" "$outdir/libmsplayer.map.txt" "$objdir")" | tee -a "$outdir/MEASURE.txt"
  if [[ "$OPT" == "${DELIVER_OPT:-os}" ]]; then
    cp -f "$outdir/libmsplayer.so" "$outdir/../libmsplayer.so"
    cp -f "$outdir/MEASURE.txt" "$outdir/../MEASURE.txt"
    echo "deliver_copy=$outdir/../libmsplayer.so" | tee -a "$outdir/MEASURE.txt" | tee -a "$outdir/../MEASURE.txt" >/dev/null
    echo "deliver_copy=$outdir/../libmsplayer.so"
  fi
}


opt_flags
for abi in $ABIS; do
  build_abi "$abi"
done
echo "single: done (MODE=$MODE OPT=$OPT); not device acceptance"
