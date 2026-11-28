#!/usr/bin/env bash
# 阶段 5：路线 B 单 so 零功能损失核查（4 ABI）。
#  - FFmpeg 组件配置：s5 与 s4 的 config_components.h 必须逐字相同（协议/封装/解码器/parser/bsf 一个不少）；
#  - 8 个音频软解、常用字幕 demuxer、DASH/HLS、AVS+(cavsvideo parser) 显式断言；冷门格式仍为 0；
#  - so：NEEDED 仅系统库+libz；动态导出与 s4 交付逐字相同（61 个 Java_ + JNI_OnLoad = 62）；无 RELR；
#  - so 内仍含关键 AVOption 键名、常用字幕/音频 codec 名与透传 JNI 入口；help 长文本已不在。
# 用法：scripts/single_s5_verify.sh [基线交付目录]   by: Long <mycjzb@gmail.com>
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
source "$ROOT/scripts/android_env.sh"
BASE="${1:-/workspace/MsMedia-deliver-20261124-1325}"
export PATH="$MS_NDK/toolchains/llvm/prebuilt/linux-x86_64/bin:$PATH"
fail=0
ck() { if eval "$2"; then echo "  PASS $1"; else echo "  FAIL $1"; fail=1; fi; }
declare -A D=([armeabi-v7a]=v7a [arm64-v8a]=v8a [x86]=x86 [x86_64]=x86_64)
for abi in armeabi-v7a arm64-v8a x86 x86_64; do
  d=${D[$abi]}
  so="$ROOT/build/android-$d/single-slimff-adec-s5/oz-flto/libmsplayer.so"
  c4="$ROOT/build/android-$d/ffmpeg-9.1-slimff-adec-s4/build/config_components.h"
  c5="$ROOT/build/android-$d/ffmpeg-9.1-slimff-adec-s5/build/config_components.h"
  echo "== $abi $(stat -c%s "$so") bytes"
  ck "config_components s5==s4" "cmp -s '$c4' '$c5'"
  for c in AAC AAC_LATM AC3 EAC3 MP2 PCM_ALAW PCM_MULAW DCA; do ck "decoder $c" "grep -q '^#define CONFIG_${c}_DECODER 1' '$c5'"; done
  ck "decoders exactly 8" "[[ \$(grep -c '^#define CONFIG_[A-Z0-9_]*_DECODER 1' '$c5') == 8 ]]"
  for c in MPEGTS MATROSKA MOV WEBVTT SRT ASS HLS DASH FLV LIVE_FLV RTSP MPEGPS AVI; do ck "demuxer $c" "grep -q '^#define CONFIG_${c}_DEMUXER 1' '$c5'"; done
  for c in CAVSVIDEO H264 HEVC AAC AC3 DCA MPEGAUDIO; do ck "parser $c" "grep -q '^#define CONFIG_${c}_PARSER 1' '$c5'"; done
  for c in HTTP HTTPS TLS TCP UDP RTP RTMP RTMPS FILE; do ck "protocol $c" "grep -q '^#define CONFIG_${c}_PROTOCOL 1' '$c5'"; done
  for c in OGG_DEMUXER MICRODVD_DEMUXER VOBSUB_DEMUXER IMAGE2_DEMUXER IMF_DEMUXER RTMPT_PROTOCOL SRTP_PROTOCOL; do ck "cold off $c" "grep -q '^#define CONFIG_${c} 0' '$c5'"; done
  ck "MS_SLIM_S5 in ff build" "grep -q 'MS_SLIM_S5=1' '$ROOT/build/android-$d/ffmpeg-9.1-slimff-adec-s5/configure-flags.txt'"
  ck "Full LTO in ff build" "grep -q 'flto=full' '$ROOT/build/android-$d/ffmpeg-9.1-slimff-adec-s5/configure-flags.txt'"
  need=$(llvm-readelf -d "$so" | awk '/NEEDED/{gsub(/[\[\]]/,"",$5);print $5}' | sort | tr '\n' ' ')
  ck "NEEDED system+libz ($need)" "[[ '$need' == 'libandroid.so libc.so libdl.so liblog.so libm.so libz.so ' ]]"
  ck "exports == s4 delivery" "diff -q <(llvm-nm -D --defined-only '$BASE/slimff/lib/$abi/libmsplayer.so' | awk '{print \$3}') <(llvm-nm -D --defined-only '$so' | awk '{print \$3}') >/dev/null"
  ck "exports 62 (61 Java_ + JNI_OnLoad)" "[[ \$(llvm-nm -D --defined-only '$so' | wc -l) == 62 && \$(llvm-nm -D --defined-only '$so' | grep -c ' Java_') == 61 ]]"
  ck "no RELR (API18)" "! llvm-readelf -S '$so' | grep -q '\.relr\.dyn'"
  ck "no DT_RELR/ANDROID_REL" "! llvm-readelf -d '$so' | grep -qE 'RELR|ANDROID_REL'"
  strs=$(llvm-strings -n 3 "$so")
  for k in user_agent headers rw_timeout reconnect reconnect_streamed rtsp_transport allowed_extensions protocol_whitelist format_whitelist max_redirects multiple_requests probesize analyzeduration ca_file tls_verify scan_all_pmts; do
    ck "AVOption key '$k'" "grep -qx '$k' <<<\"\$strs\""
  done
  for k in hdmv_pgs_subtitle dvb_subtitle subrip webvtt ass mov_text eia_608 aac_latm eac3 dts cavs avs2 avs3 hevc h264 pcm_alaw pcm_mulaw mp2; do
    ck "codec name '$k'" "grep -qx '$k' <<<\"\$strs\""
  done
  ck "passthrough JNI" "llvm-nm -D --defined-only '$so' | grep -q nativeSetAudioPassthrough"
  ck "help text gone ('override User-Agent header')" "! grep -q 'override User-Agent header' <<<\"\$strs\""
done
echo "single_s5_verify: $([[ $fail == 0 ]] && echo ALL PASS || echo FAIL)"
exit $fail
