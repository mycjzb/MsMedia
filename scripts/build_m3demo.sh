#!/usr/bin/env bash
# 编 Media3 源码演示 APK（共享 bs-p2p/third_party/media3-src 直接参与编译，不用 media3 AAR）。
# 源码位置：默认同级仓库 ../bs-p2p；MS_MEDIA3_SRC 可覆盖。
# 用法：
#   cd /home/long/code/MsMedia && bash scripts/build_m3demo.sh
# 产物：android/m3demo/app/build/outputs/apk/debug/app-debug.apk
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
M3="$ROOT/android/m3demo"
SRC="${MS_MEDIA3_SRC:-$ROOT/../bs-p2p/third_party/media3-src}"
[[ -f "$SRC/libraries/exoplayer/src/main/java/androidx/media3/exoplayer/ExoPlayer.java" ]] || {
  echo "error: missing Media3 SOURCE at $SRC（把 bs-p2p 拉到 MsMedia 同级，或设 MS_MEDIA3_SRC）" >&2
  exit 2
}
[[ -x "$M3/gradlew" ]] || { echo "error: missing $M3/gradlew" >&2; exit 2; }
cd "$M3"
./gradlew :app:assembleDebug --no-daemon
APK="$M3/app/build/outputs/apk/debug/app-debug.apk"
echo "apk: $APK"
ls -la "$APK"
