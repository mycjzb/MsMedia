#!/usr/bin/env bash
# 把生产 SDK（Java + v7a/v8a libmsplayer.so）同步到 MsPlayer 仓库目录。
# 默认读 slimff s6 oz-flto 交付 so（已 UPX）；可用环境变量覆盖。
#
#   SDK_DIR=/home/long/code/MsPlayer scripts/publish_msplayer_sdk.sh
#   SUB=single-slimff-adec-s6/oz-flto PACK_SO=1 …
#
# by: Long <mycjzb@gmail.com>
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SDK_DIR="${SDK_DIR:-/home/long/code/MsPlayer}"
SUB="${SINGLE_SUB:-single-slimff-adec-s6/oz-flto}"
V7A="$ROOT/build/android-v7a/$SUB/libmsplayer.so"
V8A="$ROOT/build/android-v8a/$SUB/libmsplayer.so"
# 兼容 deliver_copy 落在 outsub 根下
[[ -f "$V7A" ]] || V7A="$ROOT/build/android-v7a/${SUB%%/*}/libmsplayer.so"
[[ -f "$V8A" ]] || V8A="$ROOT/build/android-v8a/${SUB%%/*}/libmsplayer.so"

need() { [[ -f "$1" ]] || { echo "error: missing $1" >&2; exit 2; }; }
need "$V7A"; need "$V8A"
need "$ROOT/android/hw/java/com/mycjzb/media/player/MediaPlayer.java"

mkdir -p "$SDK_DIR/java/com/mycjzb/media/player" \
  "$SDK_DIR/jniLibs/armeabi-v7a" \
  "$SDK_DIR/jniLibs/arm64-v8a"

for f in MediaPlayer.java TimedText.java MsNativeLoader.java MsHttpClient.java; do
  cp -f "$ROOT/android/hw/java/com/mycjzb/media/player/$f" "$SDK_DIR/java/com/mycjzb/media/player/"
done
cp -f "$ROOT/android/hw/consumer-rules.pro" "$SDK_DIR/consumer-rules.pro"
cp -f "$ROOT/LICENSE" "$SDK_DIR/LICENSE"

cp -f "$V7A" "$SDK_DIR/jniLibs/armeabi-v7a/libmsplayer.so"
cp -f "$V8A" "$SDK_DIR/jniLibs/arm64-v8a/libmsplayer.so"

{
  echo "date_cst=$(date '+%Y-%m-%d %H:%M:%S') Asia/Shanghai"
  echo "msmedia_head=$(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || echo unknown)"
  echo "sub=$SUB"
  echo "armeabi-v7a $(wc -c < "$SDK_DIR/jniLibs/armeabi-v7a/libmsplayer.so") $(sha256sum "$SDK_DIR/jniLibs/armeabi-v7a/libmsplayer.so" | awk '{print $1}')"
  echo "arm64-v8a $(wc -c < "$SDK_DIR/jniLibs/arm64-v8a/libmsplayer.so") $(sha256sum "$SDK_DIR/jniLibs/arm64-v8a/libmsplayer.so" | awk '{print $1}')"
} | tee "$SDK_DIR/VERSION.txt"

echo "published -> $SDK_DIR"
