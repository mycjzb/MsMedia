#!/usr/bin/env bash
# 非正式 Android 硬解交叉编译：NDK r29 API 21 armeabi-v7a + arm64-v8a libmshw + 可运行播放编排 APK（含探测 Activity；非正式实机硬解验收）。
# 不是四平台/旧 Android 正式验收。NDK r29 最低目标 API 21（r26 起），不能以 18 为 compile target；
# 交付清单 minSdk=18：运行时 API 18–20 走 JNI MediaCodec，21+ 走 dlopen 的 libmediandk（不 NEEDED）。
# 不启动会破坏已有 AVD 的操作；不声明设备硬解已跑通。
# by: Long <mycjzb@gmail.com>
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
NDK="${ANDROID_NDK_HOME:-${ANDROID_NDK_ROOT:-/home/box/.local/android-ndk-r29}}"
SDK="${ANDROID_SDK_ROOT:-${ANDROID_HOME:-/home/box/.local/android-sdk}}"
JDK="${JAVA_HOME:-/home/box/.local/jdk}"
API="${ANDROID_API:-21}"
PREBUILT="$NDK/toolchains/llvm/prebuilt/linux-x86_64"
export PATH="/usr/bin:/bin:$PREBUILT/bin:$JDK/bin:${SDK}/build-tools/34.0.0:${PATH}"
export ANDROID_NDK_ROOT="$NDK"
export ANDROID_NDK_HOME="$NDK"

CC_V7A="$PREBUILT/bin/armv7a-linux-androideabi${API}-clang"
CC_V8A="$PREBUILT/bin/aarch64-linux-android${API}-clang"
CC="$CC_V7A"
AR="$PREBUILT/bin/llvm-ar"
STRIP="$PREBUILT/bin/llvm-strip"
SYSROOT="$PREBUILT/sysroot"
SYSROOT_LIB_V7A="$SYSROOT/usr/lib/arm-linux-androideabi/${API}"
SYSROOT_LIB_V8A="$SYSROOT/usr/lib/aarch64-linux-android/${API}"
OUT="$ROOT/build/android-hw"
OBJ="$OUT/obj"
INC="-I$ROOT/include -I$ROOT/src/hw -I$ROOT/src/core -I$ROOT/src/codec -I$ROOT/src/container"
# 与宿主 release-size 同类的精简，但不改写 build/release 或历史 build/android-v7a/size。
CFLAGS_COMMON="-fPIC -Os -ffunction-sections -fdata-sections -fvisibility=hidden -DMSMEDIA_BUILD_SHARED -D__ANDROID__ -DMS_HW_HAVE_FF -Wall -Wextra -Wno-unused-parameter -std=c11 $INC"
CFLAGS_V7A="$CFLAGS_COMMON -march=armv7-a -mfloat-abi=softfp -mfpu=vfpv3-d16"
CFLAGS_V8A="$CFLAGS_COMMON -march=armv8-a"
CFLAGS="$CFLAGS_V7A"
MAP="$ROOT/src/hw/libmshw.map"

# ms_hw_android.c 编译两次并改名，主 so 里再按 SDK 分发。编译目标仍是 API 21。
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
SRCS=(
  "$ROOT/src/hw/ms_hw_name.c"
  "$ROOT/src/hw/ms_hw_csd.c"
  "$ROOT/src/hw/ms_hw.c"
  "$ROOT/src/hw/ms_hw_vaapi.c"
  "$ROOT/src/hw/ms_hw_player.c" \
  "$ROOT/src/hw/ms_media3_audio_clock.c" \
  "$ROOT/src/hw/ms_media3_video_release.c"
  "$ROOT/src/hw/ms_hw_ass_bind.c"
  "$ROOT/src/hw/ms_hw_ai_bind.c"
  "$ROOT/src/hw/ms_hw_reap.c"
  "$ROOT/src/hw/ms_hw_audio.c"
  "$ROOT/src/hw/ms_hw_audio_trim.c"
  "$ROOT/src/hw/ms_hw_jni.c"
  "$ROOT/src/hw/ms_hw_api.c"
  "$ROOT/src/hw/ms_hw_mediandk.c"
  "$ROOT/src/hw/ms_hw_android_dispatch.c"
  "$ROOT/src/hw/ms_hw_player_jni.c"
  "$ROOT/src/hw/ms_hw_probe.c"
)

if [[ ! -x "$CC" ]]; then
  echo "error: missing $CC" >&2
  exit 2
fi

mkdir -p "$OBJ" "$OUT/obj/v8a" "$OUT/lib/armeabi-v7a" "$OUT/lib/arm64-v8a" "$OUT/legacy"
rm -f "$OBJ"/*.o "$OUT"/libmshw.so "$OUT"/libmshw-legacy.a

echo "android-hw: NDK=$NDK API=$API CC=$CC (informal; not four-platform acceptance)"

compile_one() {
  local src="$1" out="$2"
  shift 2
  echo "  cc $(basename "$src") -> $(basename "$out")"
  "$CC" $CFLAGS "$@" -c "$src" -o "$out"
}

CORE_MAP="$ROOT/src/core/libmsmedia.map"
CORE_SRCS=(
  "$ROOT/src/core/ms_crc32.c"
  "$ROOT/src/core/ms_util.c"
  "$ROOT/src/core/ms_registry.c"
  "$ROOT/src/core/ms_session.c"
  "$ROOT/src/codec/ms_adts.c"
  "$ROOT/src/codec/ms_h264.c"
  "$ROOT/src/codec/ms_hevc.c"
  "$ROOT/src/codec/ms_hdr.c"
  "$ROOT/src/codec/ms_vbit.c"
  "$ROOT/src/codec/ms_abit.c"
  "$ROOT/src/codec/ms_avcc.c"
  "$ROOT/src/sub/ms_sub.c"
  "$ROOT/src/container/ms_ts.c"
  "$ROOT/src/container/ms_ts_scan.c"
  "$ROOT/src/container/ms_bmff.c"
  "$ROOT/src/container/ms_bmff_sample.c"
  "$ROOT/src/container/ms_flv.c"
  "$ROOT/src/source/ms_source.c"
  "$ROOT/src/hls/ms_hls.c"
  "$ROOT/src/dash/ms_dash.c"
)
# 重链已有优化 v7a FFmpeg/OpenSSL/libxml2 前缀 + 当前 ms_ff 适配器（含前缀扫描）。
# 不重编 FFmpeg/OpenSSL，不覆盖 build/android-v7a/size 的历史体积对照产物。
relink_v7a_msff() {
  local ssl_p="$ROOT/build/android-v7a/openssl-3.5.9-size/prefix"
  local ff_p
  if [[ "${ANDROID_SIZE_MOREFMT:-0}" == 1 ]]; then
    ff_p="$ROOT/build/android-v7a/ffmpeg-9.1-size-morefmt/prefix"
  else
    ff_p="$ROOT/build/android-v7a/ffmpeg-9.1-size/prefix"
  fi
  local xml_p="$ROOT/build/android-v7a/libxml2-2.13.8/prefix"
  local cflags="-std=c11 -Os -DNDEBUG -ffunction-sections -fdata-sections -fPIC -fvisibility=hidden -DMSMEDIA_BUILD_SHARED -march=armv7-a -mfloat-abi=softfp -mfpu=vfpv3-d16 -fno-lto"
  if [[ ! -f "$ff_p/lib/libavformat.a" || ! -f "$ssl_p/lib/libssl.a" || ! -f "$xml_p/lib/libxml2.a" ]]; then
    echo "error: missing size-opt v7a prefixes under build/android-v7a (run scripts/android_v7a_size.sh once)" >&2
    exit 2
  fi
  mkdir -p "$OUT/obj/msff"
  echo "android-hw: relinking libmsff.so from size-opt v7a prefixes + current adapter"
  "$CC" $cflags -pthread -I"$ROOT/include" -I"$ROOT/src/ff" -I"$ROOT/src/container" -I"$ROOT/src/core" \
    -I"$ff_p/include" -I"$ssl_p/include" \
    -c "$ROOT/src/ff/ms_ff.c" -o "$OUT/obj/msff/ms_ff.o"
  "$CC" $cflags -pthread -I"$ROOT/include" -I"$ROOT/src/core" \
    -c "$ROOT/src/container/ms_ts_scan.c" -o "$OUT/obj/msff/ms_ts_scan.o"
  "$CC" $cflags -pthread -I"$ROOT/include" -I"$ROOT/src/core" \
    -c "$ROOT/src/ff/ms_ff_scan_support.c" -o "$OUT/obj/msff/ms_ff_scan_support.o"
  "$CC" $cflags -pthread -I"$ROOT/include" \
    -c "$ROOT/src/sub/ms_sub.c" -o "$OUT/obj/msff/ms_sub.o"
  "$CC" $cflags -pthread -I"$ROOT/include" \
    -c "$ROOT/src/codec/ms_hevc.c" -o "$OUT/obj/msff/ms_hevc.o"
  "$CC" $cflags -pthread -I"$ROOT/include" \
    -c "$ROOT/src/codec/ms_hdr.c" -o "$OUT/obj/msff/ms_hdr.o"
  "$CC" $cflags -pthread -I"$ROOT/include" \
    -c "$ROOT/src/codec/ms_vbit.c" -o "$OUT/obj/msff/ms_vbit.o"
  "$CC" $cflags -pthread -I"$ROOT/include" \
    -c "$ROOT/src/codec/ms_abit.c" -o "$OUT/obj/msff/ms_abit.o"
  "$CC" $cflags -pthread -I"$ROOT/include" \
    -c "$ROOT/src/codec/ms_avcc.c" -o "$OUT/obj/msff/ms_avcc.o"
  # API 18 加载：补 atof/signal（API 21 libc 符号），不改冻结前缀。
  "$CC" $cflags -pthread -c "$ROOT/src/compat/ms_api18_libc.c" -o "$OUT/obj/msff/ms_api18_libc.o"
  "$CC" $cflags -fPIC -fvisibility=hidden -shared -pthread \
    -Wl,-soname,libmsff.so -Wl,--version-script="$ROOT/src/ff/libmsff.map" \
    -Wl,--gc-sections -Wl,--exclude-libs,ALL -Wl,-z,defs \
    -o "$OUT/libmsff.unstripped.so" \
    "$OUT/obj/msff/ms_ff.o" "$OUT/obj/msff/ms_ts_scan.o" "$OUT/obj/msff/ms_ff_scan_support.o" \
    "$OUT/obj/msff/ms_sub.o" \
    "$OUT/obj/msff/ms_hevc.o" "$OUT/obj/msff/ms_hdr.o" \
    "$OUT/obj/msff/ms_vbit.o" \
    "$OUT/obj/msff/ms_abit.o" \
    "$OUT/obj/msff/ms_avcc.o" \
    "$OUT/obj/msff/ms_api18_libc.o" \
    "$ff_p/lib/libavformat.a" "$ff_p/lib/libavcodec.a" "$ff_p/lib/libavutil.a" \
    "$ssl_p/lib/libssl.a" "$ssl_p/lib/libcrypto.a" \
    "$xml_p/lib/libxml2.a" \
    -L"$SYSROOT_LIB_V7A" \
    -lz -lm -ldl -lc
  cp -f "$OUT/libmsff.unstripped.so" "$OUT/lib/armeabi-v7a/libmsff.so"
  cp -f "$OUT/libmsff.unstripped.so" "$OUT/libmsff.so"
  "$STRIP" --strip-unneeded "$OUT/lib/armeabi-v7a/libmsff.so"
  "$STRIP" --strip-unneeded "$OUT/libmsff.so"
  echo "android-hw: libmsff.so $(wc -c < "$OUT/lib/armeabi-v7a/libmsff.so") bytes stripped (armeabi-v7a)"
}
# 重链已有优化 v8a FFmpeg/OpenSSL/libxml2 前缀 + 当前 ms_ff 适配器。
# 不重编 FFmpeg/OpenSSL，不覆盖 build/android-v7a/size 或 build/android-v8a/size 的历史体积对照产物。
relink_v8a_msff() {
  local ssl_p="$ROOT/build/android-v8a/openssl-3.5.9-size/prefix"
  local ff_p
  if [[ "${ANDROID_SIZE_MOREFMT:-0}" == 1 ]]; then
    ff_p="$ROOT/build/android-v8a/ffmpeg-9.1-size-morefmt/prefix"
  else
    ff_p="$ROOT/build/android-v8a/ffmpeg-9.1-size/prefix"
  fi
  local xml_p="$ROOT/build/android-v8a/libxml2-2.13.8/prefix"
  local cflags="-std=c11 -Os -DNDEBUG -ffunction-sections -fdata-sections -fPIC -fvisibility=hidden -DMSMEDIA_BUILD_SHARED -march=armv8-a -fno-lto"
  if [[ ! -f "$ff_p/lib/libavformat.a" || ! -f "$ssl_p/lib/libssl.a" || ! -f "$xml_p/lib/libxml2.a" ]]; then
    echo "error: missing size-opt v8a prefixes under build/android-v8a (run scripts/android_v8a_size.sh once)" >&2
    exit 2
  fi
  if [[ ! -x "$CC_V8A" ]]; then
    echo "error: missing $CC_V8A" >&2
    exit 2
  fi
  mkdir -p "$OUT/obj/msff-v8a" "$OUT/lib/arm64-v8a"
  echo "android-hw: relinking libmsff.so from size-opt v8a prefixes + current adapter"
  "$CC_V8A" $cflags -pthread -I"$ROOT/include" -I"$ROOT/src/ff" -I"$ROOT/src/container" -I"$ROOT/src/core" \
    -I"$ff_p/include" -I"$ssl_p/include" \
    -c "$ROOT/src/ff/ms_ff.c" -o "$OUT/obj/msff-v8a/ms_ff.o"
  "$CC_V8A" $cflags -pthread -I"$ROOT/include" -I"$ROOT/src/core" \
    -c "$ROOT/src/container/ms_ts_scan.c" -o "$OUT/obj/msff-v8a/ms_ts_scan.o"
  "$CC_V8A" $cflags -pthread -I"$ROOT/include" -I"$ROOT/src/core" \
    -c "$ROOT/src/ff/ms_ff_scan_support.c" -o "$OUT/obj/msff-v8a/ms_ff_scan_support.o"
  "$CC_V8A" $cflags -pthread -I"$ROOT/include" \
    -c "$ROOT/src/sub/ms_sub.c" -o "$OUT/obj/msff-v8a/ms_sub.o"
  "$CC_V8A" $cflags -pthread -I"$ROOT/include" \
    -c "$ROOT/src/codec/ms_hevc.c" -o "$OUT/obj/msff-v8a/ms_hevc.o"
  "$CC_V8A" $cflags -pthread -I"$ROOT/include" \
    -c "$ROOT/src/codec/ms_hdr.c" -o "$OUT/obj/msff-v8a/ms_hdr.o"
  "$CC_V8A" $cflags -pthread -I"$ROOT/include" \
    -c "$ROOT/src/codec/ms_vbit.c" -o "$OUT/obj/msff-v8a/ms_vbit.o"
  "$CC_V8A" $cflags -pthread -I"$ROOT/include" \
    -c "$ROOT/src/codec/ms_abit.c" -o "$OUT/obj/msff-v8a/ms_abit.o"
  "$CC_V8A" $cflags -pthread -I"$ROOT/include" \
    -c "$ROOT/src/codec/ms_avcc.c" -o "$OUT/obj/msff-v8a/ms_avcc.o"
  "$CC_V8A" $cflags -pthread -c "$ROOT/src/compat/ms_api18_libc.c" -o "$OUT/obj/msff-v8a/ms_api18_libc.o"
  "$CC_V8A" $cflags -fPIC -fvisibility=hidden -shared -pthread \
    -Wl,-soname,libmsff.so -Wl,--version-script="$ROOT/src/ff/libmsff.map" \
    -Wl,--gc-sections -Wl,--exclude-libs,ALL -Wl,-z,defs \
    -o "$OUT/libmsff.v8a.unstripped.so" \
    "$OUT/obj/msff-v8a/ms_ff.o" "$OUT/obj/msff-v8a/ms_ts_scan.o" "$OUT/obj/msff-v8a/ms_ff_scan_support.o" \
    "$OUT/obj/msff-v8a/ms_sub.o" \
    "$OUT/obj/msff-v8a/ms_hevc.o" "$OUT/obj/msff-v8a/ms_hdr.o" \
    "$OUT/obj/msff-v8a/ms_vbit.o" \
    "$OUT/obj/msff-v8a/ms_abit.o" \
    "$OUT/obj/msff-v8a/ms_avcc.o" \
    "$OUT/obj/msff-v8a/ms_api18_libc.o" \
    "$ff_p/lib/libavformat.a" "$ff_p/lib/libavcodec.a" "$ff_p/lib/libavutil.a" \
    "$ssl_p/lib/libssl.a" "$ssl_p/lib/libcrypto.a" \
    "$xml_p/lib/libxml2.a" \
    -L"$SYSROOT_LIB_V8A" \
    -lz -lm -ldl -lc
  cp -f "$OUT/libmsff.v8a.unstripped.so" "$OUT/lib/arm64-v8a/libmsff.so"
  "$STRIP" --strip-unneeded "$OUT/lib/arm64-v8a/libmsff.so"
  echo "android-hw: libmsff.so $(wc -c < "$OUT/lib/arm64-v8a/libmsff.so") bytes stripped (arm64-v8a)"
}
relink_v7a_msff
relink_v8a_msff

echo "android-hw: building libmsmedia.so (session demux for local file play)"
CORE_OBJS=()
for s in "${CORE_SRCS[@]}"; do
  o="$OBJ/media_$(basename "${s%.c}").o"
  compile_one "$s" "$o"
  CORE_OBJS+=("$o")
done
"$CC" -shared -Wl,--gc-sections -Wl,--version-script="$CORE_MAP" -Wl,-soname,libmsmedia.so \
  -o "$OUT/libmsmedia.so" "${CORE_OBJS[@]}" -lc -lm
mkdir -p "$OUT/lib/armeabi-v7a"
cp -f "$OUT/libmsmedia.so" "$OUT/lib/armeabi-v7a/libmsmedia.so"
cp -f "$OUT/libmsmedia.so" "$OUT/libmsmedia.unstripped.so"
"$STRIP" --strip-unneeded "$OUT/lib/armeabi-v7a/libmsmedia.so"
"$STRIP" --strip-unneeded "$OUT/libmsmedia.so"

OBJS=()
for s in "${SRCS[@]}"; do
  o="$OBJ/$(basename "${s%.c}").o"
  compile_one "$s" "$o"
  OBJS+=("$o")
done
compile_one "$ROOT/src/hw/ms_hw_android.c" "$OBJ/ms_hw_android_ndk.o" "${ANDROID_NDK_DEFS[@]}"
compile_one "$ROOT/src/hw/ms_hw_android.c" "$OBJ/ms_hw_android_jni.o" "${ANDROID_JNI_DEFS[@]}"
OBJS+=("$OBJ/ms_hw_android_ndk.o" "$OBJ/ms_hw_android_jni.o")

SONAME=libmshw.so
"$CC" -shared -Wl,--gc-sections -Wl,--version-script="$MAP" -Wl,-soname,"$SONAME" \
  -Wl,-rpath,\$ORIGIN -L"$OUT" \
  -o "$OUT/$SONAME" "${OBJS[@]}" \
  -lmsff -lmsmedia -landroid -llog -ldl -lc
echo "android-hw: linked $OUT/$SONAME"
cp -f "$OUT/$SONAME" "$OUT/lib/armeabi-v7a/$SONAME"
# 保留一份未 strip 对照
cp -f "$OUT/$SONAME" "$OUT/libmshw.unstripped.so"
"$STRIP" --strip-unneeded "$OUT/lib/armeabi-v7a/$SONAME"
"$STRIP" --strip-unneeded "$OUT/$SONAME"

# API 16–20 JNI I/O：NDK r29 无 API16 工具链，仅在 API21 工具链下编译校验。
echo "android-hw: compiling JNI legacy I/O with API $API toolchain (-DMS_HW_LEGACY_JNI)"
LEG_OBJS=()
for s in "$ROOT/src/hw/ms_hw_name.c" "$ROOT/src/hw/ms_hw_csd.c" \
         "$ROOT/src/hw/ms_hw.c" "$ROOT/src/hw/ms_hw_player.c" "$ROOT/src/hw/ms_media3_audio_clock.c" "$ROOT/src/hw/ms_media3_video_release.c" "$ROOT/src/hw/ms_hw_ass_bind.c" \
         "$ROOT/src/hw/ms_hw_audio.c" "$ROOT/src/hw/ms_hw_audio_trim.c" "$ROOT/src/hw/ms_hw_reap.c" \
         "$ROOT/src/hw/ms_hw_jni.c" "$ROOT/src/hw/ms_hw_android.c"; do
  o="$OUT/legacy/$(basename "${s%.c}").o"
  compile_one "$s" "$o" -DMS_HW_LEGACY_JNI
  LEG_OBJS+=("$o")
done
"$AR" rcs "$OUT/libmshw-legacy.a" "${LEG_OBJS[@]}"
# 不把 JNI_OnLoad 与主 so 重复进 APK；legacy 只作编译产物。

# 可选独立探测可执行文件（无 JVM 时诚实报告非硬解）
echo "android-hw: linking mshw_probe (no JVM; will not claim hardware)"
"$CC" $CFLAGS -DMS_HW_PROBE_MAIN -c "$ROOT/src/hw/ms_hw_probe.c" -o "$OBJ/ms_hw_probe_main.o"
# probe_main.o 与 so 内 probe 符号冲突，只重链一份
PROBE_OBJS=()
for s in ms_hw_name.c ms_hw_csd.c ms_hw.c ms_hw_jni.c ms_hw_api.c ms_hw_mediandk.c          ms_hw_android_dispatch.c ms_hw_android_ndk.c ms_hw_android_jni.c; do
  # 双编译产物文件名与源名不同
  case "$s" in
    ms_hw_android_ndk.c) PROBE_OBJS+=("$OBJ/ms_hw_android_ndk.o") ;;
    ms_hw_android_jni.c) PROBE_OBJS+=("$OBJ/ms_hw_android_jni.o") ;;
    *) PROBE_OBJS+=("$OBJ/${s%.c}.o") ;;
  esac
done
"$CC" -pie -Wl,--gc-sections -o "$OUT/mshw_probe" \
  "$OBJ/ms_hw_probe_main.o" "${PROBE_OBJS[@]}" "$OBJ/media_ms_vbit.o" \
  -landroid -llog -lc
"$STRIP" --strip-unneeded "$OUT/mshw_probe" || true

# arm64-v8a：同一套源码与当前适配器，使用已有 size-opt 前缀。不覆盖 v7a 产物。
echo "android-hw: building arm64-v8a libmsmedia.so / libmshw.so"
CC="$CC_V8A"
CFLAGS="$CFLAGS_V8A"
OBJ_V8A="$OUT/obj/v8a"
mkdir -p "$OBJ_V8A" "$OUT/lib/arm64-v8a"
CORE_OBJS_V8A=()
for s in "${CORE_SRCS[@]}"; do
  o="$OBJ_V8A/media_$(basename "${s%.c}").o"
  compile_one "$s" "$o"
  CORE_OBJS_V8A+=("$o")
done
"$CC" -shared -Wl,--gc-sections -Wl,--version-script="$CORE_MAP" -Wl,-soname,libmsmedia.so \
  -o "$OUT/libmsmedia.v8a.unstripped.so" "${CORE_OBJS_V8A[@]}" -lc -lm
cp -f "$OUT/libmsmedia.v8a.unstripped.so" "$OUT/lib/arm64-v8a/libmsmedia.so"
"$STRIP" --strip-unneeded "$OUT/lib/arm64-v8a/libmsmedia.so"

OBJS_V8A=()
for s in "${SRCS[@]}"; do
  o="$OBJ_V8A/$(basename "${s%.c}").o"
  compile_one "$s" "$o"
  OBJS_V8A+=("$o")
done
compile_one "$ROOT/src/hw/ms_hw_android.c" "$OBJ_V8A/ms_hw_android_ndk.o" "${ANDROID_NDK_DEFS[@]}"
compile_one "$ROOT/src/hw/ms_hw_android.c" "$OBJ_V8A/ms_hw_android_jni.o" "${ANDROID_JNI_DEFS[@]}"
OBJS_V8A+=("$OBJ_V8A/ms_hw_android_ndk.o" "$OBJ_V8A/ms_hw_android_jni.o")
"$CC" -shared -Wl,--gc-sections -Wl,--version-script="$MAP" -Wl,-soname,libmshw.so \
  -Wl,-rpath,\$ORIGIN -L"$OUT/lib/arm64-v8a" \
  -o "$OUT/libmshw.v8a.unstripped.so" "${OBJS_V8A[@]}" \
  -lmsff -lmsmedia -landroid -llog -ldl -lc
cp -f "$OUT/libmshw.v8a.unstripped.so" "$OUT/lib/arm64-v8a/libmshw.so"
"$STRIP" --strip-unneeded "$OUT/lib/arm64-v8a/libmshw.so"
echo "android-hw: native ok  v8a $(wc -c < "$OUT/lib/arm64-v8a/libmshw.so") bytes stripped so"
# 恢复 v7a 工具链，避免后续误用
CC="$CC_V7A"
CFLAGS="$CFLAGS_V7A"

# 记录体积与 NEEDED
{
  echo "informal NDK r29 API ${API} armeabi-v7a + arm64-v8a libmshw (not four-platform acceptance)"
  echo "date_cst=$(date '+%Y-%m-%d %H:%M:%S') Asia/Shanghai"
  echo
  for f in \
      "$OUT/libmsff.unstripped.so" "$OUT/lib/armeabi-v7a/libmsff.so" \
      "$OUT/libmsff.v8a.unstripped.so" "$OUT/lib/arm64-v8a/libmsff.so" \
      "$OUT/libmsmedia.unstripped.so" "$OUT/lib/armeabi-v7a/libmsmedia.so" \
      "$OUT/libmsmedia.v8a.unstripped.so" "$OUT/lib/arm64-v8a/libmsmedia.so" \
      "$OUT/libmshw.unstripped.so" "$OUT/lib/armeabi-v7a/libmshw.so" \
      "$OUT/libmshw.v8a.unstripped.so" "$OUT/lib/arm64-v8a/libmshw.so" \
      "$OUT/libmshw-legacy.a" "$OUT/mshw_probe"; do
    if [[ -f "$f" ]]; then
      echo "== $(basename "$(dirname "$f")")/$(basename "$f")  bytes=$(wc -c < "$f")"
      llvm-readelf -d "$f" 2>/dev/null | awk '/NEEDED|SONAME/ {print}' || true
      llvm-strip --strip-unneeded -o /dev/null "$f" 2>/dev/null || true
    fi
  done
} | tee "$OUT/MEASURE.txt"

echo "android-hw: native ok  v7a $(wc -c < "$OUT/lib/armeabi-v7a/libmshw.so") bytes stripped so"
echo "android-hw: compare in-APK libmsff.so  v7a=$(wc -c < "$OUT/lib/armeabi-v7a/libmsff.so")  v8a=$(wc -c < "$OUT/lib/arm64-v8a/libmsff.so")  (historical v7a in-APK baseline 4244104; do not overwrite build/android-v7a/size)"

# 可选完整 ASS：交叉构建隔离前缀并打入 libmsass.so。失败不拖垮默认 APK，但必须写明未打包。
ASS_PACKED=0
ASS_EXTRA_APK=()
if [[ "${ENABLE_ASS_RENDER:-auto}" == "0" || "${ENABLE_ASS_RENDER:-}" == "no" ]]; then
  echo "android-hw: optional libmsass skipped (ENABLE_ASS_RENDER=0); light text path remains"
elif [[ ! -x "$ROOT/scripts/ass_deps_android_build.sh" ]]; then
  echo "android-hw: optional libmsass skipped (missing scripts/ass_deps_android_build.sh)"
else
  if ASS_ANDROID_ROOT="$OUT/ass-deps" "$ROOT/scripts/ass_deps_android_build.sh"; then
    link_android_msass() {
      local abi="$1"
      local cc="$2"
      local cxx="$3"
      local prefix="$OUT/ass-deps/$abi/prefix"
      local objdir="$OUT/obj-ass/$abi"
      local inc="-I$ROOT/include -I$prefix/include -I$prefix/include/freetype2"
      local cflags="-fPIC -Os -ffunction-sections -fdata-sections -fvisibility=hidden -DMSMEDIA_BUILD_SHARED -D__ANDROID__ -Wall -Wextra -Wno-unused-parameter -std=c11 $inc"
      if [[ ! -f "$prefix/lib/libass.a" ]]; then
        return 1
      fi
      mkdir -p "$objdir" "$OUT/lib/$abi"
      echo "android-hw: linking optional libmsass.so ($abi)"
      "$cc" $cflags -c "$ROOT/src/ass/ms_ass_render.c" -o "$objdir/ms_ass_render.o"
      "$cxx" -shared -static-libstdc++ -fPIC -Wl,--gc-sections         -Wl,-soname,libmsass.so -Wl,--version-script="$ROOT/src/ass/libmsass.map"         -Wl,--exclude-libs,ALL -Wl,-z,defs         -o "$OUT/lib/$abi/libmsass.so" "$objdir/ms_ass_render.o"         "$prefix/lib/libass.a" "$prefix/lib/libharfbuzz.a"         "$prefix/lib/libfribidi.a" "$prefix/lib/libfreetype.a"         -lz -lm -ldl -lc
      "$STRIP" --strip-unneeded "$OUT/lib/$abi/libmsass.so"
      echo "android-hw: libmsass.so $abi $(wc -c < "$OUT/lib/$abi/libmsass.so") bytes stripped"
    }
    if link_android_msass armeabi-v7a "$CC_V7A" "${CC_V7A}++" &&        link_android_msass arm64-v8a "$CC_V8A" "${CC_V8A}++"; then
      ASS_PACKED=1
    else
      echo "android-hw: optional libmsass link failed; APK will not contain the complete-ASS module"
      ASS_PACKED=0
    fi
  else
    echo "android-hw: optional libmsass deps cross-build failed; APK will not contain the complete-ASS module"
    ASS_PACKED=0
  fi
fi

# 可选 AI 字幕插件（无模型/SDK）。失败不拖垮默认 APK，必须写明是否打包。
AI_PACKED=0
if [[ "${ENABLE_AI_SUB:-auto}" == "0" || "${ENABLE_AI_SUB:-}" == "no" ]]; then
  echo "android-hw: optional libmsai skipped (ENABLE_AI_SUB=0)"
else
  link_android_msai() {
    local abi
    local cc
    local objdir
    local inc
    local cflags
    local obj
    local src
    local base
    abi="$1"
    cc="$2"
    objdir="$OUT/obj-ai/$abi"
    inc="-I$ROOT/include"
    cflags="-fPIC -Os -ffunction-sections -fdata-sections -fvisibility=hidden -DMSMEDIA_BUILD_SHARED -D__ANDROID__ -Wall -Wextra -Wno-unused-parameter -std=c11 $inc"
    mkdir -p "$objdir" "$OUT/lib/$abi"
    echo "android-hw: linking optional libmsai.so ($abi)"
    obj=()
    for src in "$ROOT/src/ai/ms_ai_pcm.c" "$ROOT/src/ai/ms_ai_fake.c" "$ROOT/src/ai/ms_ai_sub.c"; do
      base=$(basename "$src" .c)
      "$cc" $cflags -c "$src" -o "$objdir/${base}.o"
      obj+=("$objdir/${base}.o")
    done
    "$cc" -shared -fPIC -Wl,--gc-sections       -Wl,-soname,libmsai.so -Wl,--version-script="$ROOT/src/ai/libmsai.map"       -Wl,-z,defs       -o "$OUT/lib/$abi/libmsai.so" "${obj[@]}" -L"$OUT/lib/$abi" -lmsmedia -lc
    "$STRIP" --strip-unneeded "$OUT/lib/$abi/libmsai.so"
    echo "android-hw: libmsai.so $abi $(wc -c < "$OUT/lib/$abi/libmsai.so") bytes stripped"
  }
  if link_android_msai armeabi-v7a "$CC_V7A" && link_android_msai arm64-v8a "$CC_V8A"; then
    AI_PACKED=1
    {
      echo
      echo "optional libmsai (not default core; simulated fake vendor only; not real ASR)"
      for f in "$OUT/lib/armeabi-v7a/libmsai.so" "$OUT/lib/arm64-v8a/libmsai.so"                "$OUT/lib/armeabi-v7a/libmshw.so" "$OUT/lib/arm64-v8a/libmshw.so"; do
        if [[ -f "$f" ]]; then
          echo "== $f  bytes=$(wc -c < "$f")"
          llvm-readelf -d "$f" 2>/dev/null | awk '/NEEDED|SONAME/ {print}' || true
        fi
      done
    } | tee -a "$OUT/MEASURE.txt"
    if llvm-readelf -d "$OUT/lib/armeabi-v7a/libmshw.so" 2>/dev/null | grep -q 'libmsai\.so'; then
      echo "error: libmshw.so v7a NEEDED libmsai" >&2
      exit 1
    fi
    if llvm-readelf -d "$OUT/lib/arm64-v8a/libmshw.so" 2>/dev/null | grep -q 'libmsai\.so'; then
      echo "error: libmshw.so v8a NEEDED libmsai" >&2
      exit 1
    fi
  else
    echo "android-hw: optional libmsai link failed; APK will not contain the AI-subtitle module"
    AI_PACKED=0
  fi
fi

# APK（若 SDK/JDK 齐全）
ANDROID_JAR="$SDK/platforms/android-34/android.jar"
AAPT2="${SDK}/build-tools/34.0.0/aapt2"
D8="${SDK}/build-tools/34.0.0/d8"
ZIPALIGN="${SDK}/build-tools/34.0.0/zipalign"
APKSIGNER="${SDK}/build-tools/34.0.0/apksigner"
if [[ ! -f "$ANDROID_JAR" || ! -x "$AAPT2" || ! -x "$JDK/bin/javac" ]]; then
  echo "android-hw: SDK/JDK incomplete; skipping APK (native already built)"
  echo "SKIP_APK=1" >> "$OUT/MEASURE.txt"
  exit 0
fi

APK_DIR="$OUT/apk"
rm -rf "$APK_DIR"
mkdir -p "$APK_DIR/res" "$APK_DIR/gen" "$APK_DIR/classes" "$APK_DIR/lib/armeabi-v7a" "$APK_DIR/lib/arm64-v8a"
cp -f "$OUT/lib/armeabi-v7a/libmshw.so" "$APK_DIR/lib/armeabi-v7a/"
cp -f "$OUT/lib/armeabi-v7a/libmsmedia.so" "$APK_DIR/lib/armeabi-v7a/"
cp -f "$OUT/lib/armeabi-v7a/libmsff.so" "$APK_DIR/lib/armeabi-v7a/"
cp -f "$OUT/lib/arm64-v8a/libmshw.so" "$APK_DIR/lib/arm64-v8a/"
cp -f "$OUT/lib/arm64-v8a/libmsmedia.so" "$APK_DIR/lib/arm64-v8a/"
cp -f "$OUT/lib/arm64-v8a/libmsff.so" "$APK_DIR/lib/arm64-v8a/"
if [[ "${ASS_PACKED:-0}" -eq 1 ]]; then
  cp -f "$OUT/lib/armeabi-v7a/libmsass.so" "$APK_DIR/lib/armeabi-v7a/"
  cp -f "$OUT/lib/arm64-v8a/libmsass.so" "$APK_DIR/lib/arm64-v8a/"
  ASS_EXTRA_APK=(
    "$APK_DIR/lib/armeabi-v7a/libmsass.so"
    "$APK_DIR/lib/arm64-v8a/libmsass.so"
  )
  echo "android-hw: packaging optional libmsass.so (complete ASS renderer; not default core)"
else
  ASS_EXTRA_APK=()
  echo "android-hw: APK does not include optional libmsass.so (light subtitle path only)"
fi
if [[ "${AI_PACKED:-0}" -eq 1 ]]; then
  cp -f "$OUT/lib/armeabi-v7a/libmsai.so" "$APK_DIR/lib/armeabi-v7a/"
  cp -f "$OUT/lib/arm64-v8a/libmsai.so" "$APK_DIR/lib/arm64-v8a/"
  AI_EXTRA_APK=(
    "$APK_DIR/lib/armeabi-v7a/libmsai.so"
    "$APK_DIR/lib/arm64-v8a/libmsai.so"
  )
  echo "android-hw: packaging optional libmsai.so (AI subtitle plugin; default off; fake vendor only; not real ASR)"
else
  AI_EXTRA_APK=()
  echo "android-hw: APK does not include optional libmsai.so"
fi
"$AAPT2" compile --dir "$ROOT/android/hw/res" -o "$APK_DIR/res.zip"
"$AAPT2" link -o "$APK_DIR/unaligned-unsigned.apk" \
  --manifest "$ROOT/android/hw/AndroidManifest.xml" \
  -I "$ANDROID_JAR" \
  --java "$APK_DIR/gen" \
  "$APK_DIR/res.zip"
"$JDK/bin/javac" --release 11 -cp "$ANDROID_JAR" \
  -d "$APK_DIR/classes" \
  "$ROOT/android/hw/java/com/mycjzb/media/player/MsHwSmokeActivity.java" \
  "$ROOT/android/hw/java/com/mycjzb/media/player/MsHwPlayerActivity.java"
# 含匿名/内部类（如 Player$1），不能只喂两个顶层 .class
mapfile -t DEX_CLASSES < <(find "$APK_DIR/classes" -name '*.class' | sort)
"$D8" --lib "$ANDROID_JAR" --min-api 18 \
  --output "$APK_DIR" \
  "${DEX_CLASSES[@]}"
# 把 dex 与 native so 打进 apk（Bot 可能无 zip(1)）
cp -f "$APK_DIR/unaligned-unsigned.apk" "$APK_DIR/tmp.apk"
python3 "$ROOT/scripts/apk_add_native.py" "$APK_DIR/tmp.apk" "$APK_DIR/classes.dex" \
  "$APK_DIR/lib/armeabi-v7a/libmshw.so" "$APK_DIR/lib/armeabi-v7a/libmsmedia.so" \
  "$APK_DIR/lib/armeabi-v7a/libmsff.so" \
  "$APK_DIR/lib/arm64-v8a/libmshw.so" "$APK_DIR/lib/arm64-v8a/libmsmedia.so" \
  "$APK_DIR/lib/arm64-v8a/libmsff.so" \
  "${ASS_EXTRA_APK[@]+"${ASS_EXTRA_APK[@]}"}"   "${AI_EXTRA_APK[@]+"${AI_EXTRA_APK[@]}"}"
"$ZIPALIGN" -f -p 4 "$APK_DIR/tmp.apk" "$OUT/MsHwPlayer-unsigned.apk"

KS="$OUT/debug.keystore"
if [[ ! -f "$KS" ]]; then
  "$JDK/bin/keytool" -genkeypair -v -keystore "$KS" -storepass android \
    -keypass android -alias androiddebugkey -keyalg RSA -keysize 2048 \
    -validity 10000 -dname "CN=MsHwPlayer,O=MsMedia,C=CN" >/tmp/ms-hw-keytool.log
fi
"$APKSIGNER" sign --ks "$KS" --ks-pass pass:android --key-pass pass:android \
  --out "$OUT/MsHwPlayer.apk" "$OUT/MsHwPlayer-unsigned.apk"
"$APKSIGNER" verify --min-sdk-version 18 "$OUT/MsHwPlayer.apk"
echo "android-hw: APK $OUT/MsHwPlayer.apk  bytes=$(wc -c < "$OUT/MsHwPlayer.apk")"
echo "APK=$OUT/MsHwPlayer.apk  bytes=$(wc -c < "$OUT/MsHwPlayer.apk")" >> "$OUT/MEASURE.txt"
echo "NOTE: packaging an APK is not a device hardware-decode run, on-screen first frame, or speaker output" >> "$OUT/MEASURE.txt"
echo "NOTE: test APK sets allow_system_aac=1 (系统 AAC，非硬件); library default remains hardware-only; this is not video software decode" >> "$OUT/MEASURE.txt"
