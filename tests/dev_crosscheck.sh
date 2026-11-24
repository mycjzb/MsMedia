#!/usr/bin/env bash
# 开发交叉验证：用已安装的 ffmpeg/ffprobe 生成自有 testsrc/正弦 TS，
# 再经公共 ABI（ms_ts_inspect）对照公开探测输出。
# 不链入产品库，不作为 ./build.sh 正式入口，不把软件编码当产品软解。
# by: Long <mycjzb@gmail.com>
#
# 用法（在仓库根，先 ./build.sh debug 或 release）：
#   tests/dev_crosscheck.sh
#   tests/dev_crosscheck.sh --inspect build/release/ms_ts_inspect
#
# 许可与来源：样本由 lavfi 的 testsrc / sine 在本机即时编码封装，
# 不是第三方成片，不入库。FFmpeg 为系统包，仅作可选开发工具。

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

FFMPEG="${FFMPEG:-/usr/bin/ffmpeg}"
FFPROBE="${FFPROBE:-/usr/bin/ffprobe}"
INSPECT=""
OUTDIR="$ROOT/build/dev-samples"
CHUNK_LIST=(1 17 188 512 0)

usage() {
  cat <<'U'
用法: tests/dev_crosscheck.sh [--inspect PATH] [--outdir DIR]
  需要已用 ./build.sh 编出 ms_ts_inspect，以及系统 ffmpeg/ffprobe。
U
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --inspect) INSPECT="${2:-}"; shift 2 ;;
    --outdir) OUTDIR="${2:-}"; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) echo "unknown arg: $1" >&2; usage >&2; exit 2 ;;
  esac
done

if [[ -z "$INSPECT" ]]; then
  for c in "$ROOT/build/debug/ms_ts_inspect" \
           "$ROOT/build/release/ms_ts_inspect" \
           "$ROOT/build/default/ms_ts_inspect"; do
    if [[ -x "$c" ]]; then
      INSPECT="$c"
      break
    fi
  done
fi
if [[ -z "$INSPECT" || ! -x "$INSPECT" ]]; then
  echo "error: ms_ts_inspect not found. Run ./build.sh debug first." >&2
  exit 2
fi
if [[ ! -x "$FFMPEG" || ! -x "$FFPROBE" ]]; then
  echo "error: need $FFMPEG and $FFPROBE (optional dev tools; not used in product)." >&2
  exit 2
fi

mkdir -p "$OUTDIR"
REPORT="$OUTDIR/crosscheck-report.txt"
: > "$REPORT"
log() { printf '%s\n' "$*" | tee -a "$REPORT"; }

log "inspect=$INSPECT"
log "ffmpeg=$FFMPEG ($("$FFMPEG" -version | head -n 1))"
log "ffprobe=$FFPROBE ($("$FFPROBE" -version | head -n 1))"
log "outdir=$OUTDIR"
log "samples: lavfi testsrc+sine, encoded on this host, not redistributed media"
log ""

enc_ts() {
  # args: outfile profile aud slices rate extra_x264 a_rate bf duration offset
  local out="$1" profile="$2" aud="$3" slices="$4" fps="$5"
  local extra="$6" ar="$7" bf="$8" dur="$9" off="${10:-0}"
  local params="aud=${aud}:sliced-threads=0:slices=${slices}:keyint=15:min-keyint=15:b-pyramid=none"
  if [[ -n "$extra" ]]; then
    params="${params}:${extra}"
  fi
  local off_args=()
  if [[ "$off" != "0" ]]; then
    off_args=(-output_ts_offset "$off")
  fi
  "$FFMPEG" -y -hide_banner -loglevel error \
    -f lavfi -i "testsrc=size=160x120:rate=${fps}:duration=${dur}" \
    -f lavfi -i "sine=frequency=440:sample_rate=${ar}:duration=${dur}" \
    "${off_args[@]}" \
    -c:v libx264 -pix_fmt yuv420p -profile:v "$profile" -preset veryfast \
    -x264-params "$params" -bf "$bf" -g 15 -b:v 80k \
    -c:a aac -ar "$ar" -ac 1 -b:a 32k -profile:a aac_low \
    -f mpegts -mpegts_flags +resend_headers \
    "$out"
}

# 小型矩阵：常见逐行 profile、AUD 有/无、多 slice、44.1/48k、可选 B 帧、PTS 临近回绕。
enc_ts "$OUTDIR/bl_aud1_s1_48k.tsx"  baseline 1 1 15 ""     48000 0 1 0
# 上面误用 tsx 扩展，下面用 .ts
rm -f "$OUTDIR/bl_aud1_s1_48k.tsx"
enc_ts "$OUTDIR/bl_aud1_s1_48k.ts"   baseline 1 1 15 ""     48000 0 1 0
enc_ts "$OUTDIR/bl_aud0_s1_48k.ts"   baseline 0 1 15 ""     48000 0 1 0
enc_ts "$OUTDIR/bl_aud0_s2_48k.ts"   baseline 0 2 15 ""     48000 0 1 0
enc_ts "$OUTDIR/main_aud1_s1_441.ts" main     1 1 15 ""     44100 2 1 0
enc_ts "$OUTDIR/high_aud0_s2_48k.ts" high     0 2 15 ""     48000 2 1 0
# 2^33/90000 = 95443.377777...；从 95443.0 起 2 秒会跨过回绕点
# mux 默认约 +1.4s。选偏移使第一帧在回绕前、2 秒内越过 2^33/90000。
enc_ts "$OUTDIR/bl_pts_wrap.ts"      baseline 1 1 15 ""     48000 0 2 95441.2

# 断流参考：两段短 TS 直接拼接（第二段 CC/PCR 不连续）
enc_ts "$OUTDIR/part_a.ts" baseline 1 1 15 "" 48000 0 0.6 0
enc_ts "$OUTDIR/part_b.ts" baseline 1 1 15 "" 48000 0 0.6 0
cat "$OUTDIR/part_a.ts" "$OUTDIR/part_b.ts" > "$OUTDIR/concat_disc.ts"

export OUTDIR FFMPEG FFPROBE INSPECT REPORT
python3 "$ROOT/tests/dev_crosscheck.py"

# P2a：同一套宿主机工具下的 MP4/fMP4/MOV 箱体对照（不链产品库）
log ""
log "== BMFF box crosscheck"
BMFF_INSPECT_BIN=""
for c in "$ROOT/build/debug/ms_bmff_inspect" "$ROOT/build/release/ms_bmff_inspect" "$ROOT/build/default/ms_bmff_inspect"; do
  if [[ -x "$c" ]]; then BMFF_INSPECT_BIN="$c"; break; fi
done
bash "$ROOT/tests/dev_bmff_crosscheck.sh" --outdir "$OUTDIR/bmff-crosscheck" --inspect "$BMFF_INSPECT_BIN"

# P2b/P2c/P2d：渐进 MP4/MOV 与 fMP4 H264/AAC AU 对照（独立 sample/moof 表 + ms_ts_inspect；不链产品库）
log ""
log "== BMFF progressive AU crosscheck"
bash "$ROOT/tests/dev_bmff_au_crosscheck.sh" --outdir "$OUTDIR/bmff-au-crosscheck" --inspect "$INSPECT"

# P2e：经典 FLV H264/AAC AU 对照（独立 tag 表 + ms_ts_inspect + ffprobe；不链产品库）
log ""
log "== FLV AU crosscheck"
bash "$ROOT/tests/dev_flv_au_crosscheck.sh" --outdir "$OUTDIR/flv-au-crosscheck" --inspect "$INSPECT"
