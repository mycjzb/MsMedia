#!/usr/bin/env bash
# 非正式测试用 x86（i686）APK：仅供 API 18/19 x86 模拟器验收，去掉 ARM 翻译开销。
# 输出到 build/android-hw-x86，不覆盖 build/android-hw 默认 v7a+v8a 交付 APK，
# 也不改写 build/android-v7a、build/android-v8a 前缀或任何冻结产物。
# 不是实机硬解/出声验收；模拟器解码是 goldfish/c2.android 软件路径。
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

CC="$PREBUILT/bin/i686-linux-android${API}-clang"
CXX="${CC}++"
AR="$PREBUILT/bin/llvm-ar"
STRIP="$PREBUILT/bin/llvm-strip"
SYSROOT="$PREBUILT/sysroot"
SYSROOT_LIB="$SYSROOT/usr/lib/i686-linux-android/${API}"
OUT="$ROOT/build/android-hw-x86"
OBJ="$OUT/obj"
INC="-I$ROOT/include -I$ROOT/src/hw -I$ROOT/src/core -I$ROOT/src/codec -I$ROOT/src/container"
CFLAGS="-fPIC -Os -ffunction-sections -fdata-sections -fvisibility=hidden -DMSMEDIA_BUILD_SHARED -D__ANDROID__ -DMS_HW_HAVE_FF -Wall -Wextra -Wno-unused-parameter -std=c11 $INC"
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
  "$ROOT/src/hw/ms_hw_player.c"
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

if [[ ! -x "$CC" ]]; then
  echo "error: missing $CC" >&2
  exit 2
fi

# 如缺 x86 size-opt 前缀则在独立目录新建；绝不重编 v7a/v8a/x86_64 前缀或冻结产物。
SSL_P="$ROOT/build/android-x86/openssl-3.5.9-size/prefix"
XML_P="$ROOT/build/android-x86/libxml2-2.13.8/prefix"
if [[ "${ANDROID_SIZE_MOREFMT:-0}" == 1 ]]; then
  # 测试包链 size-morefmt；绝不重编/覆盖冻结 ffmpeg-9.1-size。
  FF_P="$ROOT/build/android-x86/ffmpeg-9.1-size-morefmt/prefix"
else
  FF_P="$ROOT/build/android-x86/ffmpeg-9.1-size/prefix"
fi
if [[ ! -f "$FF_P/lib/libavformat.a" || ! -f "$SSL_P/lib/libssl.a" || ! -f "$XML_P/lib/libxml2.a" ]]; then
  if [[ "${ANDROID_SIZE_MOREFMT:-0}" == 1 ]]; then
    echo "error: missing x86 size-morefmt prefixes (run ANDROID_SIZE_MOREFMT=1 scripts/android_x86_size.sh)" >&2
    exit 2
  fi
  echo "android-hw-x86: building new size-opt prefixes under build/android-x86"
  bash "$ROOT/scripts/android_x86_size.sh"
fi
if [[ ! -f "$FF_P/lib/libavformat.a" || ! -f "$SSL_P/lib/libssl.a" || ! -f "$XML_P/lib/libxml2.a" ]]; then
  echo "error: missing size-opt x86 prefixes under build/android-x86" >&2
  exit 2
fi

mkdir -p "$OBJ" "$OUT/lib/x86"
rm -f "$OBJ"/*.o

echo "android-hw-x86: NDK=$NDK API=$API CC=$CC (test APK only; not default delivery)"

compile_one() {
  local src="$1" out="$2"
  shift 2
  echo "  cc $(basename "$src") -> $(basename "$out")"
  "$CC" $CFLAGS "$@" -c "$src" -o "$out"
}

echo "android-hw-x86: relinking libmsff.so from size-opt x86 prefixes + current adapter"
# API 18 bionic 无法 mmap p_filesz=0 的 LOAD（lld RELRO padding 把 .bss 单独成段时会 EINVAL）。
X86_NORELRO="-Wl,-z,norelro"
FFCFLAGS="-std=c11 -Os -DNDEBUG -ffunction-sections -fdata-sections -fPIC -fvisibility=hidden -DMSMEDIA_BUILD_SHARED -fno-lto"
mkdir -p "$OUT/obj/msff" "$OUT/lib/x86"
"$CC" $FFCFLAGS -pthread -I"$ROOT/include" -I"$ROOT/src/ff" -I"$ROOT/src/container" -I"$ROOT/src/core" \
  -I"$FF_P/include" -I"$SSL_P/include" \
  -c "$ROOT/src/ff/ms_ff.c" -o "$OUT/obj/msff/ms_ff.o"
"$CC" $FFCFLAGS -pthread -I"$ROOT/include" -I"$ROOT/src/core" \
  -c "$ROOT/src/container/ms_ts_scan.c" -o "$OUT/obj/msff/ms_ts_scan.o"
"$CC" $FFCFLAGS -pthread -I"$ROOT/include" -I"$ROOT/src/core" \
  -c "$ROOT/src/ff/ms_ff_scan_support.c" -o "$OUT/obj/msff/ms_ff_scan_support.o"
"$CC" $FFCFLAGS -pthread -I"$ROOT/include" \
  -c "$ROOT/src/sub/ms_sub.c" -o "$OUT/obj/msff/ms_sub.o"
"$CC" $FFCFLAGS -pthread -I"$ROOT/include" \
  -c "$ROOT/src/codec/ms_hevc.c" -o "$OUT/obj/msff/ms_hevc.o"
"$CC" $FFCFLAGS -pthread -I"$ROOT/include" \
  -c "$ROOT/src/codec/ms_hdr.c" -o "$OUT/obj/msff/ms_hdr.o"
"$CC" $FFCFLAGS -pthread -I"$ROOT/include" \
  -c "$ROOT/src/codec/ms_vbit.c" -o "$OUT/obj/msff/ms_vbit.o"
"$CC" $FFCFLAGS -pthread -I"$ROOT/include" \
  -c "$ROOT/src/codec/ms_abit.c" -o "$OUT/obj/msff/ms_abit.o"
"$CC" $FFCFLAGS -pthread -I"$ROOT/include" \
  -c "$ROOT/src/codec/ms_avcc.c" -o "$OUT/obj/msff/ms_avcc.o"
"$CC" $FFCFLAGS -pthread -c "$ROOT/src/compat/ms_api18_libc.c" -o "$OUT/obj/msff/ms_api18_libc.o"
"$CC" $FFCFLAGS -fPIC -fvisibility=hidden -shared $X86_NORELRO -pthread \
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
  "$FF_P/lib/libavformat.a" "$FF_P/lib/libavcodec.a" "$FF_P/lib/libavutil.a" \
  "$SSL_P/lib/libssl.a" "$SSL_P/lib/libcrypto.a" \
  "$XML_P/lib/libxml2.a" \
  -L"$SYSROOT_LIB" \
  -lz -lm -ldl -lc
cp -f "$OUT/libmsff.unstripped.so" "$OUT/lib/x86/libmsff.so"
"$STRIP" --strip-unneeded "$OUT/lib/x86/libmsff.so"
echo "android-hw-x86: libmsff.so $(wc -c < "$OUT/lib/x86/libmsff.so") bytes stripped (x86)"

echo "android-hw-x86: building libmsmedia.so / libmshw.so"
CORE_OBJS=()
for s in "${CORE_SRCS[@]}"; do
  o="$OBJ/media_$(basename "${s%.c}").o"
  compile_one "$s" "$o"
  CORE_OBJS+=("$o")
done
"$CC" -shared $X86_NORELRO -Wl,--gc-sections -Wl,--version-script="$CORE_MAP" -Wl,-soname,libmsmedia.so \
  -o "$OUT/libmsmedia.unstripped.so" "${CORE_OBJS[@]}" -lc -lm
cp -f "$OUT/libmsmedia.unstripped.so" "$OUT/lib/x86/libmsmedia.so"
"$STRIP" --strip-unneeded "$OUT/lib/x86/libmsmedia.so"

OBJS=()
for s in "${SRCS[@]}"; do
  o="$OBJ/$(basename "${s%.c}").o"
  compile_one "$s" "$o"
  OBJS+=("$o")
done
compile_one "$ROOT/src/hw/ms_hw_android.c" "$OBJ/ms_hw_android_ndk.o" "${ANDROID_NDK_DEFS[@]}"
compile_one "$ROOT/src/hw/ms_hw_android.c" "$OBJ/ms_hw_android_jni.o" "${ANDROID_JNI_DEFS[@]}"
OBJS+=("$OBJ/ms_hw_android_ndk.o" "$OBJ/ms_hw_android_jni.o")
"$CC" -shared $X86_NORELRO -Wl,--gc-sections -Wl,--version-script="$MAP" -Wl,-soname,libmshw.so \
  -Wl,-rpath,\$ORIGIN -L"$OUT/lib/x86" \
  -o "$OUT/libmshw.unstripped.so" "${OBJS[@]}" \
  -lmsff -lmsmedia -landroid -llog -ldl -lc
cp -f "$OUT/libmshw.unstripped.so" "$OUT/lib/x86/libmshw.so"
"$STRIP" --strip-unneeded "$OUT/lib/x86/libmshw.so"
echo "android-hw-x86: libmshw.so $(wc -c < "$OUT/lib/x86/libmshw.so") bytes stripped"

# 测试用无 UI 取帧工具（不打进默认交付 APK）。
echo "android-hw-x86: linking mshw_frame_dump (test tool; not device-HW acceptance)"
"$CC" $CFLAGS -DMS_HW_FRAME_DUMP_MAIN -c "$ROOT/src/hw/ms_hw_frame_dump.c" -o "$OBJ/ms_hw_frame_dump.o"
"$CC" -pie -Wl,--gc-sections -Wl,-rpath,\$ORIGIN -L"$OUT/lib/x86" \
  -o "$OUT/mshw_frame_dump" "$OBJ/ms_hw_frame_dump.o" \
  -lmshw -lmsff -lmsmedia -landroid -llog -ldl -lc -lm
"$STRIP" --strip-unneeded "$OUT/mshw_frame_dump" || true
echo "android-hw-x86: mshw_frame_dump $(wc -c < "$OUT/mshw_frame_dump") bytes"

{
  echo "informal NDK r29 API ${API} x86 (i686) test APK (not default delivery; not four-platform / device-HW acceptance)"
  echo "date_cst=$(date '+%Y-%m-%d %H:%M:%S') Asia/Shanghai"
  echo
  for f in "$OUT/libmsff.unstripped.so" "$OUT/lib/x86/libmsff.so" \
           "$OUT/libmsmedia.unstripped.so" "$OUT/lib/x86/libmsmedia.so" \
           "$OUT/libmshw.unstripped.so" "$OUT/lib/x86/libmshw.so"; do
    if [[ -f "$f" ]]; then
      echo "== $f  bytes=$(wc -c < "$f")"
      llvm-readelf -d "$f" 2>/dev/null | awk '/NEEDED|SONAME/ {print}' || true
    fi
  done
} | tee "$OUT/MEASURE.txt"

# 可选完整 ASS：x86 依赖前缀可能尚未建立，失败不拖垮测试 APK。
ASS_PACKED=0
ASS_EXTRA_APK=()
if [[ "${ENABLE_ASS_RENDER:-auto}" == "0" || "${ENABLE_ASS_RENDER:-}" == "no" ]]; then
  echo "android-hw-x86: optional libmsass skipped (ENABLE_ASS_RENDER=0); light text path remains"
elif [[ ! -x "$ROOT/scripts/ass_deps_android_build.sh" ]]; then
  echo "android-hw-x86: optional libmsass skipped (missing scripts/ass_deps_android_build.sh)"
else
  if ASS_ANDROID_ROOT="$OUT/ass-deps" ASS_ANDROID_ABIS="x86" \
      "$ROOT/scripts/ass_deps_android_build.sh"; then
    prefix="$OUT/ass-deps/x86/prefix"
    if [[ -f "$prefix/lib/libass.a" ]]; then
      objdir="$OUT/obj-ass/x86"
      inc="-I$ROOT/include -I$prefix/include -I$prefix/include/freetype2"
      cflags="-fPIC -Os -ffunction-sections -fdata-sections -fvisibility=hidden -DMSMEDIA_BUILD_SHARED -D__ANDROID__ -Wall -Wextra -Wno-unused-parameter -std=c11 $inc"
      mkdir -p "$objdir" "$OUT/lib/x86"
      echo "android-hw-x86: linking optional libmsass.so (x86)"
      "$CC" $cflags -c "$ROOT/src/ass/ms_ass_render.c" -o "$objdir/ms_ass_render.o"
      "$CXX" -shared $X86_NORELRO -static-libstdc++ -fPIC -Wl,--gc-sections \
        -Wl,-soname,libmsass.so -Wl,--version-script="$ROOT/src/ass/libmsass.map" \
        -Wl,--exclude-libs,ALL -Wl,-z,defs \
        -o "$OUT/lib/x86/libmsass.so" "$objdir/ms_ass_render.o" \
        "$prefix/lib/libass.a" "$prefix/lib/libharfbuzz.a" \
        "$prefix/lib/libfribidi.a" "$prefix/lib/libfreetype.a" \
        -lz -lm -ldl -lc
      "$STRIP" --strip-unneeded "$OUT/lib/x86/libmsass.so"
      ASS_PACKED=1
      echo "android-hw-x86: libmsass.so $(wc -c < "$OUT/lib/x86/libmsass.so") bytes stripped"
    fi
  fi
  if [[ "$ASS_PACKED" -ne 1 ]]; then
    echo "android-hw-x86: optional libmsass not packed; light subtitle path remains"
  fi
fi

AI_PACKED=0
AI_EXTRA_APK=()
if [[ "${ENABLE_AI_SUB:-auto}" == "0" || "${ENABLE_AI_SUB:-}" == "no" ]]; then
  echo "android-hw-x86: optional libmsai skipped (ENABLE_AI_SUB=0)"
else
  objdir="$OUT/obj-ai/x86"
  inc="-I$ROOT/include"
  cflags="-fPIC -Os -ffunction-sections -fdata-sections -fvisibility=hidden -DMSMEDIA_BUILD_SHARED -D__ANDROID__ -Wall -Wextra -Wno-unused-parameter -std=c11 $inc"
  mkdir -p "$objdir" "$OUT/lib/x86"
  echo "android-hw-x86: linking optional libmsai.so (x86)"
  obj=()
  for src in "$ROOT/src/ai/ms_ai_pcm.c" "$ROOT/src/ai/ms_ai_fake.c" "$ROOT/src/ai/ms_ai_sub.c"; do
    base=$(basename "$src" .c)
    "$CC" $cflags -c "$src" -o "$objdir/${base}.o"
    obj+=("$objdir/${base}.o")
  done
  if "$CC" -shared $X86_NORELRO -fPIC -Wl,--gc-sections \
      -Wl,-soname,libmsai.so -Wl,--version-script="$ROOT/src/ai/libmsai.map" \
      -Wl,-z,defs \
      -o "$OUT/lib/x86/libmsai.so" "${obj[@]}" -L"$OUT/lib/x86" -lmsmedia -lc; then
    "$STRIP" --strip-unneeded "$OUT/lib/x86/libmsai.so"
    AI_PACKED=1
    echo "android-hw-x86: libmsai.so $(wc -c < "$OUT/lib/x86/libmsai.so") bytes stripped"
    if llvm-readelf -d "$OUT/lib/x86/libmshw.so" 2>/dev/null | grep -q 'libmsai\.so'; then
      echo "error: libmshw.so x86 NEEDED libmsai" >&2
      exit 1
    fi
  else
    echo "android-hw-x86: optional libmsai link failed; not packed"
  fi
fi

ANDROID_JAR="$SDK/platforms/android-34/android.jar"
AAPT2="${SDK}/build-tools/34.0.0/aapt2"
D8="${SDK}/build-tools/34.0.0/d8"
ZIPALIGN="${SDK}/build-tools/34.0.0/zipalign"
APKSIGNER="${SDK}/build-tools/34.0.0/apksigner"
if [[ ! -f "$ANDROID_JAR" || ! -x "$AAPT2" || ! -x "$JDK/bin/javac" ]]; then
  echo "android-hw-x86: SDK/JDK incomplete; skipping APK (native already built)"
  echo "SKIP_APK=1" >> "$OUT/MEASURE.txt"
  exit 0
fi

APK_DIR="$OUT/apk"
rm -rf "$APK_DIR"
mkdir -p "$APK_DIR/res" "$APK_DIR/gen" "$APK_DIR/classes" "$APK_DIR/lib/x86"
cp -f "$OUT/lib/x86/libmshw.so" "$APK_DIR/lib/x86/"
cp -f "$OUT/lib/x86/libmsmedia.so" "$APK_DIR/lib/x86/"
cp -f "$OUT/lib/x86/libmsff.so" "$APK_DIR/lib/x86/"
if [[ "${ASS_PACKED:-0}" -eq 1 ]]; then
  cp -f "$OUT/lib/x86/libmsass.so" "$APK_DIR/lib/x86/"
  ASS_EXTRA_APK=("$APK_DIR/lib/x86/libmsass.so")
  echo "android-hw-x86: packaging optional libmsass.so"
else
  ASS_EXTRA_APK=()
  echo "android-hw-x86: APK does not include optional libmsass.so (light subtitle path only)"
fi
if [[ "${AI_PACKED:-0}" -eq 1 ]]; then
  cp -f "$OUT/lib/x86/libmsai.so" "$APK_DIR/lib/x86/"
  AI_EXTRA_APK=("$APK_DIR/lib/x86/libmsai.so")
  echo "android-hw-x86: packaging optional libmsai.so (fake vendor only; not real ASR)"
else
  AI_EXTRA_APK=()
  echo "android-hw-x86: APK does not include optional libmsai.so"
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
mapfile -t DEX_CLASSES < <(find "$APK_DIR/classes" -name '*.class' | sort)
"$D8" --lib "$ANDROID_JAR" --min-api 18 \
  --output "$APK_DIR" \
  "${DEX_CLASSES[@]}"
cp -f "$APK_DIR/unaligned-unsigned.apk" "$APK_DIR/tmp.apk"
python3 "$ROOT/scripts/apk_add_native.py" "$APK_DIR/tmp.apk" "$APK_DIR/classes.dex" \
  "$APK_DIR/lib/x86/libmshw.so" "$APK_DIR/lib/x86/libmsmedia.so" \
  "$APK_DIR/lib/x86/libmsff.so" \
  "${ASS_EXTRA_APK[@]+"${ASS_EXTRA_APK[@]}"}"   "${AI_EXTRA_APK[@]+"${AI_EXTRA_APK[@]}"}"
"$ZIPALIGN" -f -p 4 "$APK_DIR/tmp.apk" "$OUT/MsHwPlayer-unsigned.apk"

# 复用默认非正式 APK 的 debug 钥匙（只读复制）；没有则在本输出目录新建，不写回 build/android-hw。
KS="$OUT/debug.keystore"
if [[ ! -f "$KS" && -f "$ROOT/build/android-hw/debug.keystore" ]]; then
  cp -f "$ROOT/build/android-hw/debug.keystore" "$KS"
fi
if [[ ! -f "$KS" ]]; then
  "$JDK/bin/keytool" -genkeypair -v -keystore "$KS" -storepass android \
    -keypass android -alias androiddebugkey -keyalg RSA -keysize 2048 \
    -validity 10000 -dname "CN=MsHwPlayer,O=MsMedia,C=CN" >/tmp/ms-hw-x86-keytool.log
fi
"$APKSIGNER" sign --ks "$KS" --ks-pass pass:android --key-pass pass:android \
  --out "$OUT/MsHwPlayer.apk" "$OUT/MsHwPlayer-unsigned.apk"
"$APKSIGNER" verify --min-sdk-version 18 "$OUT/MsHwPlayer.apk"
echo "android-hw-x86: APK $OUT/MsHwPlayer.apk  bytes=$(wc -c < "$OUT/MsHwPlayer.apk")"
echo "APK=$OUT/MsHwPlayer.apk  bytes=$(wc -c < "$OUT/MsHwPlayer.apk")" >> "$OUT/MEASURE.txt"
echo "NOTE: x86 (i686) test APK is not the default v7a+v8a delivery and not a real-device hardware-decode run" >> "$OUT/MEASURE.txt"
echo "NOTE: test APK sets allow_system_aac=1 (系统 AAC，非硬件); library default remains hardware-only; this is not video software decode" >> "$OUT/MEASURE.txt"
