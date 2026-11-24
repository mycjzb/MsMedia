#!/usr/bin/env bash
# libmsff.so 体积解剖：在新目录按 android_hw_build.sh 同一命令重链一份（只加 -Wl,-Map），
# 不改冻结前缀（只读 build/android-*/ffmpeg-9.1-size*、openssl-3.5.9-size、libxml2），
# 不写 build/android-hw。输出 build/single-analysis/msff-map/<abi>-<variant>/。
# 用法：VARIANT=morefmt|size scripts/single_msff_anatomy.sh
# by: Long <mycjzb@gmail.com>
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
NDK="${ANDROID_NDK_HOME:-/home/box/.local/android-ndk-r29}"
API=21
PREBUILT="$NDK/toolchains/llvm/prebuilt/linux-x86_64"
export PATH="/usr/bin:/bin:$PREBUILT/bin"
VARIANT="${VARIANT:-morefmt}"

# 公共：一个 ABI 的重链（与 relink_v7a_msff/relink_v8a_msff 同参数，仅多 -Map）。
relink_one() {
  local abi="$1" tag cc arch libdir ffdir out
  case "$abi" in
    armeabi-v7a) tag=android-v7a; cc="$PREBUILT/bin/armv7a-linux-androideabi${API}-clang"
      arch="-march=armv7-a -mfloat-abi=softfp -mfpu=vfpv3-d16"; libdir="$PREBUILT/sysroot/usr/lib/arm-linux-androideabi/$API" ;;
    arm64-v8a) tag=android-v8a; cc="$PREBUILT/bin/aarch64-linux-android${API}-clang"
      arch="-march=armv8-a"; libdir="$PREBUILT/sysroot/usr/lib/aarch64-linux-android/$API" ;;
  esac
  if [[ "$VARIANT" == morefmt ]]; then ffdir=ffmpeg-9.1-size-morefmt; else ffdir=ffmpeg-9.1-size; fi
  local ff_p="$ROOT/build/$tag/$ffdir/prefix" ssl_p="$ROOT/build/$tag/openssl-3.5.9-size/prefix" xml_p="$ROOT/build/$tag/libxml2-2.13.8/prefix"
  out="$ROOT/build/single-analysis/msff-map/$abi-$VARIANT"
  rm -rf "$out"; mkdir -p "$out/obj"
  local cflags="-std=c11 -Os -DNDEBUG -ffunction-sections -fdata-sections -fPIC -fvisibility=hidden -DMSMEDIA_BUILD_SHARED $arch -fno-lto -pthread"
  "$cc" $cflags -I"$ROOT/include" -I"$ROOT/src/ff" -I"$ROOT/src/container" -I"$ROOT/src/core" \
    -I"$ff_p/include" -I"$ssl_p/include" -c "$ROOT/src/ff/ms_ff.c" -o "$out/obj/ms_ff.o"
  "$cc" $cflags -I"$ROOT/include" -I"$ROOT/src/core" -c "$ROOT/src/container/ms_ts_scan.c" -o "$out/obj/ms_ts_scan.o"
  "$cc" $cflags -I"$ROOT/include" -I"$ROOT/src/core" -c "$ROOT/src/ff/ms_ff_scan_support.c" -o "$out/obj/ms_ff_scan_support.o"
  local s
  for s in sub/ms_sub codec/ms_hevc codec/ms_hdr codec/ms_vbit codec/ms_abit codec/ms_avcc; do
    "$cc" $cflags -I"$ROOT/include" -c "$ROOT/src/$s.c" -o "$out/obj/$(basename $s).o"
  done
  "$cc" $cflags -c "$ROOT/src/compat/ms_api18_libc.c" -o "$out/obj/ms_api18_libc.o"
  "$cc" $cflags -shared -Wl,-soname,libmsff.so -Wl,--version-script="$ROOT/src/ff/libmsff.map" \
    -Wl,--gc-sections -Wl,--exclude-libs,ALL -Wl,-z,defs -Wl,-Map="$out/libmsff.map.txt" \
    -o "$out/libmsff.unstripped.so" \
    "$out"/obj/ms_ff.o "$out"/obj/ms_ts_scan.o "$out"/obj/ms_ff_scan_support.o "$out"/obj/ms_sub.o \
    "$out"/obj/ms_hevc.o "$out"/obj/ms_hdr.o "$out"/obj/ms_vbit.o "$out"/obj/ms_abit.o "$out"/obj/ms_avcc.o \
    "$out"/obj/ms_api18_libc.o \
    "$ff_p/lib/libavformat.a" "$ff_p/lib/libavcodec.a" "$ff_p/lib/libavutil.a" \
    "$ssl_p/lib/libssl.a" "$ssl_p/lib/libcrypto.a" "$xml_p/lib/libxml2.a" \
    -L"$libdir" -lz -lm -ldl -lc
  cp -f "$out/libmsff.unstripped.so" "$out/libmsff.so"
  llvm-strip --strip-unneeded "$out/libmsff.so"
  echo "$abi $VARIANT stripped=$(wc -c < "$out/libmsff.so") sha256=$(sha256sum "$out/libmsff.so" | cut -c1-64)"
  python3 "$ROOT/scripts/single_map_anatomy.py" "$out/libmsff.map.txt" "$out/libmsff.so" > "$out/ANATOMY.txt"
}
for abi in ${ABIS:-armeabi-v7a arm64-v8a}; do relink_one "$abi"; done
