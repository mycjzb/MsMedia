#!/usr/bin/env bash
# 阶段 3：单 libmsplayer.so 调试 APK（不改 android_hw_build.sh 与冻结产物）。
# VARIANT=pure   → build/android-*/single-native-s2/libmsplayer.so（路线 A，纯自研）
# VARIANT=slimff → build/android-*/single-slimff/libmsplayer.so（路线 B，精简 FFmpeg）
# 每个路线出两个包到 build/single-apk-s3/<variant>/：
#   MsSpeed-<variant>-debug.apk   ：armeabi-v7a + arm64-v8a + x86_64 (+x86 若有)，模拟器/真机通用
#   MsSpeed-<variant>-release-arm.apk：仅 v7a + v8a（交付用 ABI），同样是 debug 签名
# 另把 v7a / v8a 单 so 复制到同目录 lib/<abi>/ 便于交付。
# 包名：com.mycjzb.media.single.pure / com.mycjzb.media.single.slimff（可同时安装对比）。
# 自动化示例：
#   adb shell am start -n com.mycjzb.media.single.slimff/com.mycjzb.media.player.demo.MsSpeedDemoActivity \
#     --es auto bench --es players sys,ms --ei repeat 3 --es urls 'h264_aac.ts|h264_aac.mp4'
#   adb shell am start -n ... --es http 'http://10.0.2.2:8080/x|https://example.com/'
# by: Long <mycjzb@gmail.com>
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
source "$ROOT/scripts/android_env.sh"
SDK="$MS_SDK"
JDK="$MS_JDK"
# d8/apksigner 脚本用 PATH 里的 java，必须带上 JDK。
export JAVA_HOME="$JDK"
export PATH="$JDK/bin:/usr/bin:/bin:$PATH"
VARIANT="${VARIANT:-slimff}"
case "$VARIANT" in
  pure)   SUB="${SINGLE_SUB:-single-native-s2/os}"; PKG=com.mycjzb.media.single.pure;   LABEL="MsSpeed-A纯自研" ;;
  slimff) SUB="${SINGLE_SUB:-single-slimff-adec-s6/oz-flto}"; PKG=com.mycjzb.media.single.slimff; LABEL="MsSpeed-B精简FF" ;;
  *) echo "error: VARIANT=pure|slimff" >&2; exit 2 ;;
esac
# APK_OUT：阶段 4 起输出到新目录（如 build/single-apk-s4），不覆盖历史 single-apk-s3。
OUT="${APK_OUT:-$ROOT/build/single-apk}/$VARIANT"
ANDROID_JAR="$SDK/platforms/android-34/android.jar"
BT="$SDK/build-tools/34.0.0"
for t in "$BT/aapt2" "$BT/d8" "$BT/zipalign" "$BT/apksigner" "$JDK/bin/javac" "$JDK/bin/java"; do
  [[ -x "$t" ]] || { echo "error: missing $t (JDK=$JDK SDK=$SDK)" >&2; exit 2; }
done
declare -A ABIDIR=([armeabi-v7a]=android-v7a [arm64-v8a]=android-v8a [x86_64]=android-x86_64 [x86]=android-x86)
rm -rf "$OUT"; mkdir -p "$OUT/work/classes" "$OUT/work/gen" "$OUT/lib"
DEBUG_SOS=(); ARM_SOS=()
find_so() {
  local abi="$1" d="${ABIDIR[$abi]}"
  # 演示 APK 优先未压 so：部分机顶盒（如 Hi3798）UPX 运行时解压异常会秒失败 -1010。
  local cands=(
    "$ROOT/build/$d/$SUB/libmsplayer.so.unpacked"
    "$ROOT/build/$d/$SUB/libmsplayer.so"
    "$ROOT/build/$d/${SUB%%/*}/libmsplayer.so.unpacked"
    "$ROOT/build/$d/${SUB%%/*}/libmsplayer.so"
  )
  if [[ "$VARIANT" == slimff && "$abi" == x86 ]]; then
    cands+=("$ROOT/build/$d/single-slimff-noxml/libmsplayer.so")
  fi
  local p
  for p in "${cands[@]}"; do
    [[ -f "$p" ]] && { printf '%s\n' "$p"; return 0; }
  done
  return 1
}
for abi in armeabi-v7a arm64-v8a x86_64 x86; do
  so="$(find_so "$abi" || true)"
  if [[ -z "$so" ]]; then
    [[ "$abi" == armeabi-v7a || "$abi" == arm64-v8a ]] && {
      echo "error: missing libmsplayer.so for $abi (sub=$SUB); 先 ./scripts/build.sh -va both" >&2
      exit 2
    }
    echo "single-apk: skip $abi (no so)"; continue
  fi
  mkdir -p "$OUT/lib/$abi"; cp -f "$so" "$OUT/lib/$abi/libmsplayer.so"
  DEBUG_SOS+=("$OUT/lib/$abi/libmsplayer.so")
  [[ "$abi" == armeabi-v7a || "$abi" == arm64-v8a ]] && ARM_SOS+=("$OUT/lib/$abi/libmsplayer.so")
done
sed -e "s/@PKG@/$PKG/" -e "s/@LABEL@/$LABEL/" "$ROOT/android/single/AndroidManifest.xml" > "$OUT/work/AndroidManifest.xml"
"$BT/aapt2" compile --dir "$ROOT/android/single/res" -o "$OUT/work/res.zip"
AAPT_ASSETS=()
[[ -d "$ROOT/android/single/assets" ]] && AAPT_ASSETS=(-A "$ROOT/android/single/assets")
"$BT/aapt2" link -o "$OUT/work/base.apk" --manifest "$OUT/work/AndroidManifest.xml" -I "$ANDROID_JAR" \
  --java "$OUT/work/gen" "${AAPT_ASSETS[@]}" "$OUT/work/res.zip"
J="$ROOT/android/hw/java/com/mycjzb/media/player"
"$JDK/bin/javac" --release 11 -nowarn -encoding UTF-8 -cp "$ANDROID_JAR" -d "$OUT/work/classes" \
  "$J/MediaPlayer.java" "$J/TimedText.java" "$J/MsNativeLoader.java" "$J/MsHttpClient.java" \
  "$J/media3/VSyncSampler.java" "$J/media3/VSyncSamplerV33.java" \
  "$ROOT/android/single/java/com/mycjzb/media/player/demo/MsSpeedDemoActivity.java" "$ROOT/android/single/java/com/mycjzb/media/player/demo/MsTrackSelfTest.java" 2>&1 | grep -v "bootstrap classpath\|^1 warning\|^warning: \[options\]" || true
python3 "$ROOT/scripts/strip_method_params.py" "$OUT/work/classes"
# 以 jar 喂 d8（逐个 .class 传入时 d8 8.2 对 MediaPlayer$1 报内部 NPE）。
(cd "$OUT/work/classes" && "$JDK/bin/jar" cf "$OUT/work/classes.jar" .)
"$BT/d8" --lib "$ANDROID_JAR" --min-api 18 --output "$OUT/work" "$OUT/work/classes.jar"
mk_apk() {
  local name="$1"; shift
  cp -f "$OUT/work/base.apk" "$OUT/work/$name.tmp.apk"
  python3 "$ROOT/scripts/apk_add_native.py" "$OUT/work/$name.tmp.apk" "$OUT/work/classes.dex" "$@"
  "$BT/zipalign" -f -p 4 "$OUT/work/$name.tmp.apk" "$OUT/work/$name.aligned.apk"
  KS="$ROOT/build/single-apk/debug.keystore"
  mkdir -p "$(dirname "$KS")"
  if [[ ! -f "$KS" ]]; then
    "$JDK/bin/keytool" -genkeypair -keystore "$KS" -storepass android -keypass android \
      -alias androiddebugkey -keyalg RSA -keysize 2048 -validity 10000 -dname "CN=MsSpeed,O=MsMedia,C=CN" >/dev/null 2>&1
  fi
  "$BT/apksigner" sign --ks "$KS" --ks-pass pass:android --key-pass pass:android --out "$OUT/$name.apk" "$OUT/work/$name.aligned.apk"
  "$BT/apksigner" verify --min-sdk-version 18 "$OUT/$name.apk"
}
mk_apk "MsSpeed-$VARIANT-debug" "${DEBUG_SOS[@]}"
mk_apk "MsSpeed-$VARIANT-release-arm" "${ARM_SOS[@]}"
{
  echo "variant=$VARIANT pkg=$PKG sub=$SUB date_cst=$(date '+%Y-%m-%d %H:%M:%S') Asia/Shanghai"
  for f in "$OUT"/*.apk "${DEBUG_SOS[@]}"; do echo "$(wc -c < "$f") $(sha256sum "$f" | awk '{print $1}') ${f#$ROOT/}"; done
  echo "NOTE: APK 打包不等于真机硬解出画；debug 签名，仅调试。"
} | tee "$OUT/MEASURE.txt"
