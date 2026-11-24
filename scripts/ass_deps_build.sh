#!/usr/bin/env bash
# Build pinned FreeType (FTL) + HarfBuzz (Old MIT) + FriBidi (LGPL-2.1+) + libass (ISC)
# into an isolated prefix for the optional libmsass renderer.
# Not linked into libmsmedia or libmsff. Not a product FFmpeg build.
# Official compiler is /usr/bin/gcc; cmake/ninja/meson may come from $HOME/.local
# only for these dep builds.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CACHE="$ROOT/third_party/cache"
PREFIX="${ASS_DEPS_PREFIX:-$ROOT/build/ass-deps/prefix}"
SRCROOT="$ROOT/build/ass-deps/src"
JOBS="${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)}"
TOOL_PATH="${HOME}/.local/bin:${HOME}/.local/meson-venv/bin:/usr/bin:/bin"
export PATH="/usr/bin:/bin"
# cmake/ninja/meson only as named tools for deps
CMAKE="${CMAKE:-$HOME/.local/bin/cmake}"
NINJA="${NINJA:-$HOME/.local/bin/ninja}"
export CC="${CC:-/usr/bin/gcc}"
# 本箱 /usr/bin 无 g++ 与 C++ 头；HarfBuzz 是 C++。仅此依赖构建使用用户本机已有的
# conda-forge g++，并把 libstdc++ 静态链进可选 libmsass（见 build.sh）。
# 忽略环境里残留的非绝对路径 CXX（例如仅有工具名），以免 CMake 找不到。
if [[ -z "${CXX:-}" || "${CXX}" != /* ]]; then
  if [[ -x /usr/bin/g++ ]]; then
    CXX=/usr/bin/g++
  elif [[ -x /home/box/.local/mamba-build/bin/x86_64-conda-linux-gnu-g++ ]]; then
    CXX=/home/box/.local/mamba-build/bin/x86_64-conda-linux-gnu-g++
  elif [[ -x "$HOME/.local/bin/g++" ]]; then
    CXX="$(readlink -f "$HOME/.local/bin/g++")"
  else
    echo "error: no C++ compiler (needed only to build isolated HarfBuzz)" >&2
    exit 1
  fi
fi
export CXX
if [[ "${CXX:-}" == *mamba-build* ]]; then
  TOOL_PATH="$(dirname "$CXX"):$TOOL_PATH"
fi
echo "ass-deps CXX=$CXX"
export AR="${AR:-/usr/bin/ar}"
export RANLIB="${RANLIB:-/usr/bin/ranlib}"
export CFLAGS="${CFLAGS:--fPIC -O2 -fno-lto}"
export CXXFLAGS="${CXXFLAGS:--fPIC -O2 -fno-lto}"

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

if [[ -f "$PREFIX/lib/libass.a" && "${ASS_DEPS_REBUILD:-0}" != 1 ]]; then
  echo "ass-deps prefix already at $PREFIX (set ASS_DEPS_REBUILD=1 to rebuild)"
  exit 0
fi

mkdir -p "$CACHE" "$SRCROOT" "$PREFIX"
verify_pin libass "$CACHE/libass-0.17.3.tar.xz" "$ROOT/third_party/libass-0.17.3.pin"
verify_pin freetype "$CACHE/freetype-2.13.3.tar.xz" "$ROOT/third_party/freetype-2.13.3.pin"
verify_pin harfbuzz "$CACHE/harfbuzz-10.2.0.tar.xz" "$ROOT/third_party/harfbuzz-10.2.0.pin"
verify_pin fribidi "$CACHE/fribidi-1.0.16.tar.xz" "$ROOT/third_party/fribidi-1.0.16.pin"

rm -rf "$SRCROOT"
mkdir -p "$SRCROOT"
tar -C "$SRCROOT" -xf "$CACHE/freetype-2.13.3.tar.xz"
tar -C "$SRCROOT" -xf "$CACHE/harfbuzz-10.2.0.tar.xz"
tar -C "$SRCROOT" -xf "$CACHE/fribidi-1.0.16.tar.xz"
tar -C "$SRCROOT" -xf "$CACHE/libass-0.17.3.tar.xz"

# 1) FreeType under FTL (not GPLv2). No brotli/bzip2/png/harfbuzz in this first pass.
(
  cd "$SRCROOT/freetype-2.13.3"
  ./configure --prefix="$PREFIX" --disable-shared --enable-static \
    --with-zlib --without-bzip2 --without-png --without-harfbuzz --without-brotli \
    --with-pic
  make -j"$JOBS"
  make install
)

# 2) HarfBuzz: FreeType only, no glib/icu/cairo/graphite/subset.
mkdir -p "$SRCROOT/harfbuzz-build"
PATH="$TOOL_PATH" "$CMAKE" -S "$SRCROOT/harfbuzz-10.2.0" -B "$SRCROOT/harfbuzz-build" \
  -G Ninja \
  -DCMAKE_C_COMPILER="$CC" \
  -DCMAKE_CXX_COMPILER="$CXX" \
  -DCMAKE_INSTALL_PREFIX="$PREFIX" \
  -DCMAKE_PREFIX_PATH="$PREFIX" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
  -DBUILD_SHARED_LIBS=OFF \
  -DHB_HAVE_FREETYPE=ON \
  -DHB_HAVE_GLIB=OFF \
  -DHB_HAVE_ICU=OFF \
  -DHB_HAVE_CAIRO=OFF \
  -DHB_HAVE_GRAPHITE2=OFF \
  -DHB_HAVE_GOBJECT=OFF \
  -DHB_BUILD_UTILS=OFF \
  -DHB_BUILD_SUBSET=OFF \
  -DHB_HAVE_INTROSPECTION=OFF
PATH="$TOOL_PATH" "$CMAKE" --build "$SRCROOT/harfbuzz-build" -j"$JOBS"
PATH="$TOOL_PATH" "$CMAKE" --install "$SRCROOT/harfbuzz-build"

# 3) FriBidi (LGPL-2.1+): static PIC only, into this isolated prefix.
(
  cd "$SRCROOT/fribidi-1.0.16"
  ./configure --prefix="$PREFIX" --disable-shared --enable-static --disable-docs --with-pic
  make -j"$JOBS"
  make install
)

# 4) libass: no fontconfig (system fonts via fonts_dir / default file).
# No libunibreak. Skip fontconfig so we do not add another runtime.
export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig:${PKG_CONFIG_PATH:-}"
(
  cd "$SRCROOT/libass-0.17.3"
  ./configure --prefix="$PREFIX" --disable-shared --enable-static \
    --disable-fontconfig --disable-libunibreak \
    --disable-require-system-font-provider \
    --disable-test --disable-compare --disable-profile \
    --with-pic
  make -j"$JOBS"
  make install
)

echo "ass-deps installed $PREFIX"
ls -l "$PREFIX/lib/libass.a" "$PREFIX/lib/libfreetype.a" "$PREFIX/lib/libharfbuzz.a" "$PREFIX/lib/libfribidi.a"
