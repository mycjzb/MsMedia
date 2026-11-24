#!/usr/bin/env bash
# 阶段 6 宿主专项测试用：用与安卓 s6 相同的精简 FFmpeg 源码副本（含 MS_SLIM_S5/S6 补丁）编一份 Linux x86_64 静态前缀，
# 供 tests/single_s6_hls_test.c 在宿主跑 ASan/UBSan/LSan（Media3 HLS 预下载 / AES-128 / 自签 HTTPS 隔离）。
# 与安卓差异：无 libxml2（DASH 不在本专项范围）、-O1 -g 便于栈回溯；协议/解封装/补丁与交付一致。
# 输出：build/host/ffmpeg-9.1-slimff-s6/prefix 。不是交付物，不改任何冻结目录。by: Long <mycjzb@gmail.com>
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SLIM_SRC="$ROOT/build/single-analysis/ffmpeg-9.1-slimff-src"
OUT="$ROOT/build/host/ffmpeg-9.1-slimff-s6"
[[ -f "$SLIM_SRC/.msmedia-androidtls-patched" ]] || { echo "error: 先运行 scripts/android_slimff_ffmpeg.sh 准备精简源码副本" >&2; exit 2; }
python3 "$ROOT/scripts/slimff_patch_s6_hls.py" "$SLIM_SRC"
if [[ -f "$OUT/prefix/lib/libavformat.a" && "${HOST_FF_REBUILD:-0}" != 1 ]]; then echo "host slimff: 已存在 $OUT"; exit 0; fi
rm -rf "$OUT"; mkdir -p "$OUT/build" "$OUT/prefix"
cd "$OUT/build"
env -u CPATH -u C_INCLUDE_PATH "$SLIM_SRC/configure" --prefix="$OUT/prefix" --cc=gcc \
  --enable-pic --enable-static --disable-shared --disable-doc --disable-programs \
  --disable-avdevice --disable-avfilter --disable-swscale --disable-swresample --disable-everything \
  --enable-avutil --enable-avcodec --enable-avformat --enable-network \
  --disable-openssl --enable-androidtls --disable-gpl --disable-version3 --disable-nonfree \
  --enable-protocol=file,http,https,tls,tcp,udp,rtp,rtmp,rtmps,crypto,pipe,fd \
  --enable-demuxer=mpegts,mpegtsraw,flv,live_flv,mov,hls,rtsp,rtp,sdp,aac,h264,hevc,matroska,webvtt,avi,mpegps,mp3,flac,wav,ac3,eac3,srt,ass \
  --enable-parser=h264,hevc,mpegvideo,aac,aac_latm,mpegaudio,flac,opus,ac3 \
  --enable-bsf=extract_extradata,h264_mp4toannexb,hevc_mp4toannexb,aac_adtstoasc \
  --disable-decoders --disable-encoders --disable-muxers --disable-filters --disable-hwaccels \
  --disable-indevs --disable-outdevs --disable-x86asm --enable-small --disable-libxml2 \
  --enable-decoder=ac3,eac3,mp2,pcm_alaw,pcm_mulaw,dca,aac_latm --enable-parser=dca,cavsvideo --enable-demuxer=dts \
  --extra-cflags="-O1 -g -fno-omit-frame-pointer -ffunction-sections -fdata-sections -DMS_SLIM_S5=1 -DMS_SLIM_S6=1" >"$OUT/configure.log" 2>&1
sed -i '/tls_android.o/d' "$SLIM_SRC/libavformat/Makefile"
make -j"$(nproc)" >"$OUT/make.log" 2>&1
make install >>"$OUT/make.log" 2>&1
echo "host slimff: installed $OUT/prefix"
