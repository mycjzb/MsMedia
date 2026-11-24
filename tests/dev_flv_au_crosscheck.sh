#!/usr/bin/env bash
# 用宿主机 ffmpeg 生成经典 FLV（单视频/单音频/双轨/B 帧），独立 tag 表对照
# ms_ts_inspect 与 ffprobe。不链入产品库，样本不入库。非正式验收。
# by: Long <mycjzb@gmail.com>
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
FFMPEG="${FFMPEG:-/usr/bin/ffmpeg}"
FFPROBE="${FFPROBE:-/usr/bin/ffprobe}"
INSPECT="${INSPECT:-}"
OUTDIR="${OUTDIR:-$ROOT/build/dev-samples/flv-au}"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --inspect) INSPECT="${2:-}"; shift 2 ;;
    --outdir) OUTDIR="${2:-}"; shift 2 ;;
    *) echo "unknown arg: $1" >&2; exit 2 ;;
  esac
done

if [[ -z "$INSPECT" ]]; then
  for c in "$ROOT/build/debug/ms_ts_inspect" \
           "$ROOT/build/release/ms_ts_inspect" \
           "$ROOT/build/default/ms_ts_inspect"; do
    if [[ -x "$c" ]]; then INSPECT="$c"; break; fi
  done
fi
if [[ -z "$INSPECT" || ! -x "$INSPECT" ]]; then
  echo "error: ms_ts_inspect not found" >&2
  exit 2
fi
if [[ ! -x "$FFMPEG" || ! -x "$FFPROBE" ]]; then
  echo "error: need ffmpeg/ffprobe as optional dev tools" >&2
  exit 2
fi

mkdir -p "$OUTDIR"
enc() {
  local out="$1"; shift
  "$FFMPEG" -y -hide_banner -loglevel error "$@" "$out"
}

# 单视频、无 B 帧
enc "$OUTDIR/flv_video_only.flv" \
  -f lavfi -i testsrc=size=160x120:rate=25:duration=1 \
  -an -c:v libx264 -pix_fmt yuv420p -profile:v baseline -bf 0 -g 25 \
  -f flv

# 单音频 AAC
enc "$OUTDIR/flv_audio_only.flv" \
  -f lavfi -i sine=frequency=1000:sample_rate=48000:duration=1 \
  -vn -c:a aac -ar 48000 -ac 1 -b:a 32k -profile:a aac_low \
  -f flv

# 双轨、无 B 帧
enc "$OUTDIR/flv_baseline.flv" \
  -f lavfi -i testsrc=size=160x120:rate=25:duration=1 \
  -f lavfi -i sine=frequency=1000:sample_rate=48000:duration=1 \
  -c:v libx264 -pix_fmt yuv420p -profile:v baseline -bf 0 -g 25 \
  -c:a aac -ar 48000 -ac 1 -b:a 32k -profile:a aac_low \
  -f flv

# 双轨 + B 帧（有符号合成时间）
enc "$OUTDIR/flv_bframes.flv" \
  -f lavfi -i testsrc=size=160x120:rate=25:duration=1 \
  -f lavfi -i sine=frequency=1000:sample_rate=48000:duration=1 \
  -c:v libx264 -pix_fmt yuv420p -profile:v high -bf 3 -g 25 \
  -c:a aac -ar 48000 -ac 1 -b:a 32k -profile:a aac_low \
  -f flv

export INSPECT FFPROBE OUTDIR
python3 "$ROOT/tests/dev_flv_au_crosscheck.py" \
  "$OUTDIR/flv_video_only.flv" \
  "$OUTDIR/flv_audio_only.flv" \
  "$OUTDIR/flv_baseline.flv" \
  "$OUTDIR/flv_bframes.flv"
