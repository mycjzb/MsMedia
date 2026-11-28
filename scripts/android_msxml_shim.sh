#!/usr/bin/env bash
# 自研 XML 垫片（src/xml/ms_xml.c）按 libxml2 前缀布局打包，供精简 FFmpeg 的 dashdec 链接，彻底替换 libxml2。
# 输出：build/android-<abi>/msxml-shim/prefix/{include/libxml2/libxml/*.h, lib/libxml2.a, lib/pkgconfig/libxml-2.0.pc}
# 只写新目录，不碰冻结的 libxml2-2.13.8 前缀。
# by: Long <mycjzb@gmail.com>
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
source "$ROOT/scripts/android_env.sh"
NDK="$MS_NDK"
API="${ANDROID_API:-21}"
PB="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin"
ABIS="${ABIS:-armeabi-v7a arm64-v8a x86_64 x86}"
for abi in $ABIS; do
  case "$abi" in
    armeabi-v7a) cc="$PB/armv7a-linux-androideabi${API}-clang"; arch="-march=armv7-a -mfloat-abi=softfp -mfpu=vfpv3-d16 -mthumb"; tag=android-v7a ;;
    arm64-v8a)   cc="$PB/aarch64-linux-android${API}-clang"; arch="-march=armv8-a"; tag=android-v8a ;;
    x86_64)      cc="$PB/x86_64-linux-android${API}-clang"; arch=""; tag=android-x86_64 ;;
    x86)         cc="$PB/i686-linux-android${API}-clang"; arch=""; tag=android-x86 ;;
    *) echo "error: abi $abi" >&2; exit 2 ;;
  esac
  p="$ROOT/build/$tag/msxml-shim/prefix"
  rm -rf "$p"; mkdir -p "$p/lib/pkgconfig" "$p/include"
  cp -r "$ROOT/src/xml/include/libxml2" "$p/include/"
  "$cc" $arch -fPIC ${XML_OPT:--Os} -ffunction-sections -fdata-sections -fvisibility=hidden -Wall -Wextra \
    -I"$ROOT/src/xml/include/libxml2" -c "$ROOT/src/xml/ms_xml.c" -o "$p/lib/ms_xml.o"
  "$PB/llvm-ar" rcs "$p/lib/libxml2.a" "$p/lib/ms_xml.o"
  rm -f "$p/lib/ms_xml.o"
  cat > "$p/lib/pkgconfig/libxml-2.0.pc" <<PC
prefix=$p
libdir=\${prefix}/lib
includedir=\${prefix}/include
Name: libXML
Version: 2.13.8
Description: MsMedia msxml read-only shim (not libxml2)
Libs: -L\${libdir} -lxml2
Cflags: -I\${includedir}/libxml2
PC
  echo "msxml[$abi]: $(stat -c %s "$p/lib/libxml2.a") bytes archive -> $p"
done
