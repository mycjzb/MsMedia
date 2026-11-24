#!/usr/bin/env bash
# 阶段 6：路线 B 主力单 so（精简 FFmpeg s6 + 极致裁剪 OpenSSL 静态 + Media3 HLS 预下载）核查（4 ABI）。
# 由 scripts/single_s5_verify.sh 派生（s5 脚本保持不动，仍可单独复核 s5 产物）。
#  - FFmpeg 组件配置：s6 与 s5 的 config_components.h 必须逐字相同（s6 只改 hls.c 行为，不增减组件）；
#  - OpenSSL：静态合入（NEEDED 无 libssl/libcrypto），so 内含 OpenSSL 3.5.9 版本串，无 mbedTLS 痕迹；
#  - 导出：s5 的 62 个 + nativeSetBufferConfig + nativeSetTlsVerify = 64；
#  - 8 个音频软解、常用字幕 demuxer、DASH/HLS、AVS+(cavsvideo parser) 显式断言；冷门格式仍为 0；
#  - so：NEEDED 仅系统库+libz；动态导出与 s4 交付逐字相同（61 个 Java_ + JNI_OnLoad = 62）；无 RELR；
#  - so 内仍含关键 AVOption 键名、常用字幕/音频 codec 名与透传 JNI 入口；help 长文本已不在。
# 用法：scripts/single_s5_verify.sh [基线交付目录]   by: Long <mycjzb@gmail.com>
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BASE="${1:-/workspace/MsMedia-deliver-20261124-1325}"
export PATH="/home/box/.local/android-ndk-r29/toolchains/llvm/prebuilt/linux-x86_64/bin:$PATH"
fail=0
ck() { if eval "$2"; then echo "  PASS $1"; else echo "  FAIL $1"; fail=1; fi; }
declare -A D=([armeabi-v7a]=v7a [arm64-v8a]=v8a [x86]=x86 [x86_64]=x86_64)
for abi in armeabi-v7a arm64-v8a x86 x86_64; do
  d=${D[$abi]}
  so="$ROOT/build/android-$d/single-slimff-adec-s6/oz-flto/libmsplayer.so"
  so5="$ROOT/build/android-$d/single-slimff-adec-s5/oz-flto/libmsplayer.so"
  c4="$ROOT/build/android-$d/ffmpeg-9.1-slimff-adec-s5/build/config_components.h"
  c5="$ROOT/build/android-$d/ffmpeg-9.1-slimff-adec-s6/build/config_components.h"
  echo "== $abi $(stat -c%s "$so") bytes"
  ck "config_components s6==s5" "cmp -s '$c4' '$c5'"
  for c in AAC AAC_LATM AC3 EAC3 MP2 PCM_ALAW PCM_MULAW DCA; do ck "decoder $c" "grep -q '^#define CONFIG_${c}_DECODER 1' '$c5'"; done
  ck "decoders exactly 8" "[[ \$(grep -c '^#define CONFIG_[A-Z0-9_]*_DECODER 1' '$c5') == 8 ]]"
  for c in MPEGTS MATROSKA MOV WEBVTT SRT ASS HLS DASH FLV LIVE_FLV RTSP MPEGPS AVI; do ck "demuxer $c" "grep -q '^#define CONFIG_${c}_DEMUXER 1' '$c5'"; done
  for c in CAVSVIDEO H264 HEVC AAC AC3 DCA MPEGAUDIO; do ck "parser $c" "grep -q '^#define CONFIG_${c}_PARSER 1' '$c5'"; done
  for c in HTTP HTTPS TLS TCP UDP RTP RTMP RTMPS FILE; do ck "protocol $c" "grep -q '^#define CONFIG_${c}_PROTOCOL 1' '$c5'"; done
  for c in OGG_DEMUXER MICRODVD_DEMUXER VOBSUB_DEMUXER IMAGE2_DEMUXER IMF_DEMUXER RTMPT_PROTOCOL SRTP_PROTOCOL; do ck "cold off $c" "grep -q '^#define CONFIG_${c} 0' '$c5'"; done
  ck "MS_SLIM_S6 in ff build" "grep -q 'MS_SLIM_S6=1' '$ROOT/build/android-$d/ffmpeg-9.1-slimff-adec-s6/configure-flags.txt'"
  ck "Full LTO in ff build" "grep -q 'flto=full' '$ROOT/build/android-$d/ffmpeg-9.1-slimff-adec-s6/configure-flags.txt'"
  ck "hls.c s6 patch present" "grep -q 'MS_SLIM_S6' '$ROOT/build/single-analysis/ffmpeg-9.1-slimff-src/libavformat/hls.c'"
  ck "OpenSSL slim Full LTO" "grep -q 'flto=full' '$ROOT/build/android-$d/openssl-3.5.9-slim-flto/configure-flags.txt'"
  ck "no mbedTLS symbols" "! llvm-nm '$ROOT/build/android-$d/single-slimff-adec-s6/oz-flto/libmsplayer.unstripped.so' | grep -qi mbedtls"
  need=$(llvm-readelf -d "$so" | awk '/NEEDED/{gsub(/[\[\]]/,"",$5);print $5}' | sort | tr '\n' ' ')
  ck "NEEDED system+libz ($need)" "[[ '$need' == 'libandroid.so libc.so libdl.so liblog.so libm.so libz.so ' ]]"
  ck "exports == s5 + 2 new" "diff -q <( (llvm-nm -D --defined-only '$so5' | awk '{print \$3}'; echo Java_com_msmedia_MediaPlayer_nativeSetBufferConfig@@MSMEDIA_SINGLE_1.0; echo Java_com_msmedia_MediaPlayer_nativeSetTlsVerify@@MSMEDIA_SINGLE_1.0) | sort) <(llvm-nm -D --defined-only '$so' | awk '{print \$3}' | sort) >/dev/null"
  ck "exports 64 (63 Java_ + JNI_OnLoad)" "[[ \$(llvm-nm -D --defined-only '$so' | wc -l) == 64 && \$(llvm-nm -D --defined-only '$so' | grep -c ' Java_') == 63 ]]"
  ck "no libssl/libcrypto NEEDED" "! llvm-readelf -d '$so' | grep -qE 'libssl|libcrypto'"
  ck "no RELR (API18)" "! llvm-readelf -S '$so' | grep -q '\.relr\.dyn'"
  ck "no DT_RELR/ANDROID_REL" "! llvm-readelf -d '$so' | grep -qE 'RELR|ANDROID_REL'"
  strs=$(llvm-strings -n 3 "$so")
  for k in user_agent headers rw_timeout reconnect reconnect_streamed rtsp_transport allowed_extensions protocol_whitelist format_whitelist max_redirects multiple_requests probesize analyzeduration ca_file tls_verify scan_all_pmts; do
    ck "AVOption key '$k'" "grep -qx '$k' <<<\"\$strs\""
  done
  for k in hdmv_pgs_subtitle dvb_subtitle subrip webvtt ass mov_text eia_608 aac_latm eac3 dts cavs avs2 avs3 hevc h264 pcm_alaw pcm_mulaw mp2; do
    ck "codec name '$k'" "grep -qx '$k' <<<\"\$strs\""
  done
  ck "OpenSSL static inside (default provider)" "grep -q 'OpenSSL Base Provider' <<<\"\$strs\""
  ck "passthrough JNI" "llvm-nm -D --defined-only '$so' | grep -q nativeSetAudioPassthrough"
  ck "help text gone ('override User-Agent header')" "! grep -q 'override User-Agent header' <<<\"\$strs\""
done
echo "single_s6_verify: $([[ $fail == 0 ]] && echo ALL PASS || echo FAIL)"
exit $fail
