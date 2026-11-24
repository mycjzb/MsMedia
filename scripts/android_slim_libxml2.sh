#!/usr/bin/env bash
# 路线 B 单 so 专用精简 libxml2（只为 FFmpeg dashdec 解析 MPD）：
#   --with-minimum 起步，只回开 tree（xmlCopyNode/xmlNodeSetContent 等）与 threads（FFmpeg 多线程安全）。
#   去掉 HTML/XPath/Schema/Reader/Writer/Valid/Catalog/HTTP/zlib/iconv 等 dashdec 不用的模块；
#   -fno-unwind-tables 去掉 .eh_frame。
# 输出新目录 build/android-{v7a,v8a}/libxml2-2.13.8-slim/prefix，不改多 so 官方构建使用的 libxml2-2.13.8。
# by: Long <mycjzb@gmail.com>
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
NDK="${ANDROID_NDK_HOME:-${ANDROID_NDK_ROOT:-/home/box/.local/android-ndk-r29}}"
API="${ANDROID_API:-21}"
PREBUILT="$NDK/toolchains/llvm/prebuilt/linux-x86_64"
export PATH="/usr/bin:/bin:$PREBUILT/bin:${PATH}"
SYSROOT="$PREBUILT/sysroot"
XML_VER=2.13.8
XML_TAR="$ROOT/third_party/cache/libxml2-${XML_VER}.tar.xz"
XML_SHA="277294cb33119ab71b2bc81f2f445e9bc9435b893ad15bb2cd2b0e859a0ee84a"
ABIS="${ABIS:-armeabi-v7a arm64-v8a}"

got=$(sha256sum "$XML_TAR" | awk '{print $1}')
[[ "$got" == "$XML_SHA" ]] || { echo "error: libxml2 tarball sha256 mismatch: $got" >&2; exit 2; }

for abi in $ABIS; do
  case "$abi" in
    armeabi-v7a) tag=android-v7a; host=armv7a-linux-androideabi
      cc="$PREBUILT/bin/armv7a-linux-androideabi${API}-clang"
      arch="-march=armv7-a -mfloat-abi=softfp -mfpu=vfpv3-d16 -mthumb" ;;
    arm64-v8a) tag=android-v8a; host=aarch64-linux-android
      cc="$PREBUILT/bin/aarch64-linux-android${API}-clang"
      arch="-march=armv8-a" ;;
    x86) tag=android-x86; host=i686-linux-android
      cc="$PREBUILT/bin/i686-linux-android${API}-clang"
      arch="" ;;
    x86_64) tag=android-x86_64; host=x86_64-linux-android
      cc="$PREBUILT/bin/x86_64-linux-android${API}-clang"
      arch="" ;;
    *) echo "error: unsupported ABI $abi" >&2; exit 2 ;;
  esac
  root="$ROOT/build/$tag/libxml2-${XML_VER}-slim"
  rm -rf "$root"; mkdir -p "$root/build"
  tar -xJf "$XML_TAR" -C "$root"
  mv "$root/libxml2-${XML_VER}" "$root/src"
  (
    cd "$root/build"
    env -u CPATH -u C_INCLUDE_PATH -u LIBRARY_PATH \
      CC="$cc" AR="$PREBUILT/bin/llvm-ar" RANLIB="$PREBUILT/bin/llvm-ranlib" \
      CFLAGS="-fPIC -Os -ffunction-sections -fdata-sections -fno-unwind-tables -fno-asynchronous-unwind-tables $arch -D__ANDROID_API__=${API}" \
      "$root/src/configure" --host="$host" --prefix="$root/prefix" --with-sysroot="$SYSROOT" \
        --enable-static --disable-shared --disable-dependency-tracking \
        --with-minimum --with-tree --with-threads \
        --without-python --without-zlib --without-lzma --without-iconv --without-icu \
        > "$root/configure.log" 2>&1
    make -j"$(nproc)" > "$root/make.log" 2>&1
    make install >> "$root/make.log" 2>&1
  )
  # xmlversion.h 里每个特性是 "#if 1/#if 0" + "#define"，只取真正打开的。
  grep -B1 "#define LIBXML_[A-Z0-9_]*_ENABLED" "$root/prefix/include/libxml2/libxml/xmlversion.h" \
    | grep -A1 "^#if 1" | grep define > "$root/enabled-features.txt" || true
  echo "slim-libxml2[$abi]: $(wc -c < "$root/prefix/lib/libxml2.a") bytes libxml2.a; features: $(awk '{print $2}' "$root/enabled-features.txt" | tr '\n' ' ')"
done
