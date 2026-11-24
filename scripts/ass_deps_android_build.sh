#!/usr/bin/env bash
# 交叉构建钉选 FreeType(FTL)/HarfBuzz/FriBidi/libass 到 Android v7a/v8a 隔离前缀。
# 仅供可选 libmsass；不链入 libmsmedia/libmsff。非正式，不是四平台验收。
# by: Long <mycjzb@gmail.com>
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CACHE="$ROOT/third_party/cache"
NDK="${ANDROID_NDK_HOME:-${ANDROID_NDK_ROOT:-/home/box/.local/android-ndk-r29}}"
API="${ANDROID_API:-21}"
PREBUILT="$NDK/toolchains/llvm/prebuilt/linux-x86_64"
SYSROOT="$PREBUILT/sysroot"
CMAKE="${CMAKE:-$HOME/.local/bin/cmake}"
NINJA="${NINJA:-$HOME/.local/bin/ninja}"
JOBS="${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)}"
OUT_ROOT="${ASS_ANDROID_ROOT:-$ROOT/build/android-hw/ass-deps}"
ABIS="${ASS_ANDROID_ABIS:-armeabi-v7a arm64-v8a}"

if [[ ! -x "$PREBUILT/bin/armv7a-linux-androideabi${API}-clang" || ! -x "$PREBUILT/bin/aarch64-linux-android${API}-clang++" ]]; then
  echo "error: NDK r29 clang for API $API missing under $NDK" >&2
  exit 2
fi
if [[ ! -x "$CMAKE" || ! -x "$NINJA" ]]; then
  echo "error: cmake/ninja required for Android HarfBuzz (looked at $CMAKE $NINJA)" >&2
  exit 2
fi

verify_pin() {
  local name="$1" tar="$2" pin="$3"
  local want got
  want="$(awk -F= '/^tarball_sha256=/{print $2}' "$pin")"
  got="$(sha256sum "$tar" | awk '{print $1}')"
  if [[ "$want" != "$got" ]]; then
    echo "error: $name sha256 mismatch want=$want got=$got" >&2
    exit 1
  fi
}

verify_pin libass "$CACHE/libass-0.17.3.tar.xz" "$ROOT/third_party/libass-0.17.3.pin"
verify_pin freetype "$CACHE/freetype-2.13.3.tar.xz" "$ROOT/third_party/freetype-2.13.3.pin"
verify_pin harfbuzz "$CACHE/harfbuzz-10.2.0.tar.xz" "$ROOT/third_party/harfbuzz-10.2.0.pin"
verify_pin fribidi "$CACHE/fribidi-1.0.16.tar.xz" "$ROOT/third_party/fribidi-1.0.16.pin"

build_one() {
  local abi="$1" host cc cxx prefix src
  if [[ "$abi" == "armeabi-v7a" ]]; then
    host=armv7a-linux-androideabi
    cc="$PREBUILT/bin/armv7a-linux-androideabi${API}-clang"
    cxx="$PREBUILT/bin/armv7a-linux-androideabi${API}-clang++"
  elif [[ "$abi" == "x86_64" ]]; then
    host=x86_64-linux-android
    cc="$PREBUILT/bin/x86_64-linux-android${API}-clang"
    cxx="$PREBUILT/bin/x86_64-linux-android${API}-clang++"
  elif [[ "$abi" == "x86" ]]; then
    host=i686-linux-android
    cc="$PREBUILT/bin/i686-linux-android${API}-clang"
    cxx="$PREBUILT/bin/i686-linux-android${API}-clang++"
  else
    host=aarch64-linux-android
    cc="$PREBUILT/bin/aarch64-linux-android${API}-clang"
    cxx="$PREBUILT/bin/aarch64-linux-android${API}-clang++"
  fi
  prefix="$OUT_ROOT/$abi/prefix"
  src="$OUT_ROOT/$abi/src"
  if [[ -f "$prefix/lib/libass.a" && "${ASS_DEPS_REBUILD:-0}" != 1 ]]; then
    echo "android ass-deps $abi already at $prefix"
    return 0
  fi
  echo "android ass-deps: building $abi (informal; not four-platform)"
  rm -rf "$src"
  mkdir -p "$src" "$prefix"
  tar -C "$src" -xf "$CACHE/freetype-2.13.3.tar.xz"
  tar -C "$src" -xf "$CACHE/harfbuzz-10.2.0.tar.xz"
  tar -C "$src" -xf "$CACHE/fribidi-1.0.16.tar.xz"
  tar -C "$src" -xf "$CACHE/libass-0.17.3.tar.xz"

  export CC="$cc" CXX="$cxx"
  export AR="$PREBUILT/bin/llvm-ar" RANLIB="$PREBUILT/bin/llvm-ranlib"
  export CFLAGS="-fPIC -O2 -fno-lto -D__ANDROID_API__=${API}"
  export CXXFLAGS="-fPIC -O2 -fno-lto -D__ANDROID_API__=${API}"
  export PKG_CONFIG_PATH="$prefix/lib/pkgconfig"
  export PKG_CONFIG_LIBDIR="$prefix/lib/pkgconfig"
  export PKG_CONFIG_SYSROOT_DIR=""

  (
    cd "$src/freetype-2.13.3"
    ./configure --host="$host" --prefix="$prefix" --with-sysroot="$SYSROOT" \
      --disable-shared --enable-static --with-pic \
      --with-zlib --without-bzip2 --without-png --without-harfbuzz --without-brotli \
      --disable-dependency-tracking
    make -j"$JOBS"
    make install
  )

  mkdir -p "$src/harfbuzz-build"
  PATH="$(dirname "$NINJA"):$(dirname "$CMAKE"):$PATH" \
  "$CMAKE" -S "$src/harfbuzz-10.2.0" -B "$src/harfbuzz-build" \
    -G Ninja \
    -DCMAKE_MAKE_PROGRAM="$NINJA" \
    -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI="$abi" \
    -DANDROID_PLATFORM="android-${API}" \
    -DCMAKE_INSTALL_PREFIX="$prefix" \
    -DCMAKE_PREFIX_PATH="$prefix" \
    -DCMAKE_FIND_ROOT_PATH="$prefix;$SYSROOT" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_FLAGS_RELEASE="-O2 -DNDEBUG -fPIC -fno-lto" \
    -DCMAKE_CXX_FLAGS_RELEASE="-O2 -DNDEBUG -fPIC -fno-lto" \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DBUILD_SHARED_LIBS=OFF \
    -DHB_HAVE_FREETYPE=ON \
    -DHB_HAVE_GLIB=OFF \
    -DHB_HAVE_ICU=OFF \
    -DHB_HAVE_CAIRO=OFF \
    -DHB_HAVE_GRAPHITE2=OFF \
    -DHB_HAVE_GOBJECT=OFF \
    -DHB_HAVE_INTROSPECTION=OFF \
    -DHB_BUILD_UTILS=OFF \
    -DHB_BUILD_SUBSET=OFF \
    -DHB_HAVE_CORETEXT=OFF
  "$CMAKE" --build "$src/harfbuzz-build" --parallel "$JOBS"
  "$CMAKE" --install "$src/harfbuzz-build"

  (
    cd "$src/fribidi-1.0.16"
    ./configure --host="$host" --prefix="$prefix" --with-sysroot="$SYSROOT" \
      --disable-shared --enable-static --with-pic --disable-docs \
      --disable-dependency-tracking
    make -j"$JOBS"
    make install
  )

  (
    cd "$src/libass-0.17.3"
    ./configure --host="$host" --prefix="$prefix" --with-sysroot="$SYSROOT" \
      --disable-shared --enable-static --with-pic \
      --disable-fontconfig --disable-libunibreak \
      --disable-require-system-font-provider \
      --disable-test --disable-compare --disable-profile \
      --disable-dependency-tracking
    make -j"$JOBS"
    make install
  )
  # NDK CMake Release 可能不带 -O 且含调试符号；去掉 .a 内 debug，避免把增量藏在未 strip 的 HarfBuzz 里。
  STRIP_BIN="$PREBUILT/bin/llvm-strip"
  if [[ -x "$STRIP_BIN" ]]; then
    for a in "$prefix/lib/"lib{ass,freetype,harfbuzz,fribidi}.a; do
      [[ -f "$a" ]] && "$STRIP_BIN" --strip-debug "$a" || true
    done
  fi
  echo "android ass-deps installed $prefix"
  ls -l "$prefix/lib/"lib{ass,freetype,harfbuzz,fribidi}.a
}

for abi in $ABIS; do
  build_one "$abi"
done
