#!/usr/bin/env bash
# 一键：编 so + UPX → 拷 jniLibs → 打 :msplayer AAR + msm3 APK（可选 adb）
#
#   bash scripts/build_msm3.sh -va both
#   bash scripts/build_msm3.sh -va v7a -adb 192.168.0.166
#   bash scripts/build_msm3.sh -va v7a -skip-so   # 已有 so 只打 Java/AAR
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
M3="$ROOT/android/msm3"
ACTIVITY=com.mycjzb.media.msm3/.PlayActivity

KIND=release
ABI_SEL=v7a
ADB_TARGET=
SKIP_SO=0

usage() {
  cat <<'EOF' >&2
usage: bash scripts/build_msm3.sh [release|debug] [-va v7a|v8a|both] [-adb host[:port]] [-skip-so]
  默认先 ./scripts/build.sh slimff（含 UPX），再打 SDK AAR + demo APK
  -skip-so  跳过编 so，只用已有产物
EOF
  exit 2
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    release|debug) KIND="$1"; shift ;;
    -va|--abi) [[ $# -ge 2 ]] || usage; ABI_SEL="$2"; shift 2 ;;
    -adb|--adb) [[ $# -ge 2 ]] || usage; ADB_TARGET="$2"; shift 2 ;;
    -skip-so|--skip-so) SKIP_SO=1; shift ;;
    -h|--help) usage ;;
    *) echo "error: unknown arg '$1'" >&2; usage ;;
  esac
done

case "$ABI_SEL" in
  v7a|armeabi-v7a) ABIS=(armeabi-v7a); GRADLE_ABIS=armeabi-v7a ;;
  v8a|arm64-v8a)   ABIS=(arm64-v8a);   GRADLE_ABIS=arm64-v8a ;;
  both|all)        ABIS=(armeabi-v7a arm64-v8a); GRADLE_ABIS=armeabi-v7a,arm64-v8a ;;
  *) echo "error: -va must be v7a|v8a|both (got '$ABI_SEL')" >&2; exit 2 ;;
esac

if [[ "$SKIP_SO" -eq 0 ]]; then
  echo "==> build.sh slimff -va $ABI_SEL (so + UPX)"
  "$ROOT/scripts/build.sh" slimff -va "$ABI_SEL"
fi

if [[ -z "${JAVA_HOME:-}" ]]; then
  for j in /home/long/Android/jdk-17.0.19+10 /home/long/.jdks/ms-17.0.19; do
    [[ -x "$j/bin/java" ]] && export JAVA_HOME="$j" && break
  done
fi
[[ -n "${JAVA_HOME:-}" ]] && export PATH="$JAVA_HOME/bin:$PATH"

find_so() {
  local abi="$1" d
  case "$abi" in
    armeabi-v7a) d=android-v7a ;;
    arm64-v8a) d=android-v8a ;;
    *) return 1 ;;
  esac
  local cands=(
    "$ROOT/build/$d/single-slimff-adec-s6-noxml/os/libmsplayer.so"
    "$ROOT/build/$d/single-slimff-adec-s6/os/libmsplayer.so"
    "$ROOT/build/$d/single-slimff-adec-s6-noxml/libmsplayer.so"
    "$ROOT/build/$d/single-slimff-adec-s6/libmsplayer.so"
    "$ROOT/build/$d/single-slimff-adec-s6-noxml/oz-flto/libmsplayer.so"
    "$ROOT/build/$d/single-slimff-adec-s6/oz-flto/libmsplayer.so"
    "$ROOT/build/$d/single-slimff-adec-s6-noxml/os/libmsplayer.so.unpacked"
    "$ROOT/build/$d/single-slimff-adec-s6-noxml/oz-flto/libmsplayer.so.unpacked"
  )
  local p
  for p in "${cands[@]}"; do
    [[ -f "$p" ]] && { printf '%s\n' "$p"; return 0; }
  done
  return 1
}

JNI="$M3/app/src/main/jniLibs"
rm -rf "$JNI/armeabi-v7a" "$JNI/arm64-v8a"
for abi in "${ABIS[@]}"; do
  so="$(find_so "$abi" || true)"
  [[ -n "$so" ]] || { echo "error: missing so for $abi" >&2; exit 2; }
  mkdir -p "$JNI/$abi"
  cp -f "$so" "$JNI/$abi/libmsplayer.so"
  echo "jniLibs $abi <- $so ($(du -h "$so" | awk '{print $1}'))"
done

cd "$M3"
if [[ "$KIND" == release ]]; then
  ./gradlew :msplayer:assembleRelease :app:assembleRelease --no-daemon -Pmsm3Abis="$GRADLE_ABIS"
  APK="$M3/app/build/outputs/apk/release/app-release.apk"
else
  ./gradlew :msplayer:assembleDebug :app:assembleDebug --no-daemon -Pmsm3Abis="$GRADLE_ABIS"
  APK="$M3/app/build/outputs/apk/debug/app-debug.apk"
fi
AAR_DIR="$M3/../msplayer/build/outputs/aar"

echo "---- size ----"
ls -lh "$JNI"/*/*.so 2>/dev/null || true
ls -lh "$APK" 2>/dev/null || true
ls -lh "$AAR_DIR"/*.aar 2>/dev/null || true

if [[ -n "$ADB_TARGET" ]]; then
  command -v adb >/dev/null || { echo "error: adb not in PATH" >&2; exit 2; }
  serial="$ADB_TARGET"
  [[ "$serial" == *:* ]] || serial="${serial}:5555"
  echo "==> adb connect $serial"
  adb connect "$serial" >/dev/null || true
  adb -s "$serial" wait-for-device
  echo "==> install $APK"
  adb -s "$serial" install -r -t "$APK"
  echo "==> start $ACTIVITY"
  adb -s "$serial" shell am start -n "$ACTIVITY" || true
fi
