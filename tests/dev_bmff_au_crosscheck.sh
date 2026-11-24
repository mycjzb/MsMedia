#!/usr/bin/env bash
# 用宿主机 ffmpeg 生成渐进 H264/AAC MP4/MOV 与多片/双轨/B 帧 fMP4，对照独立表与 ms_ts_inspect。
# 不链入产品库，样本不入库。非正式验收。
# by: Long <mycjzb@gmail.com>
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
FFMPEG="${FFMPEG:-/usr/bin/ffmpeg}"
FFPROBE="${FFPROBE:-/usr/bin/ffprobe}"
INSPECT="${INSPECT:-}"
OUTDIR="${OUTDIR:-$ROOT/build/dev-samples/bmff-au}"

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

# 渐进 moov 在前，无 B 帧
enc "$OUTDIR/prog_baseline.mp4" \
  -f lavfi -i testsrc=size=160x120:rate=25:duration=1 \
  -f lavfi -i sine=frequency=1000:sample_rate=48000:duration=1 \
  -c:v libx264 -pix_fmt yuv420p -profile:v baseline -bf 0 -g 25 \
  -c:a aac -ar 48000 -ac 2 -b:a 64k \
  -movflags +faststart -f mp4

# 有 B 帧（ctts）
enc "$OUTDIR/prog_bframes.mp4" \
  -f lavfi -i testsrc=size=160x120:rate=25:duration=1 \
  -f lavfi -i sine=frequency=1000:sample_rate=48000:duration=1 \
  -c:v libx264 -pix_fmt yuv420p -profile:v high -bf 3 -g 25 \
  -c:a aac -ar 48000 -ac 2 -b:a 64k \
  -movflags +faststart -f mp4

# QuickTime / MOV
enc "$OUTDIR/prog_baseline.mov" \
  -f lavfi -i testsrc=size=160x120:rate=25:duration=1 \
  -f lavfi -i sine=frequency=1000:sample_rate=44100:duration=1 \
  -c:v libx264 -pix_fmt yuv420p -profile:v baseline -bf 0 -g 25 \
  -c:a aac -ar 44100 -ac 2 -b:a 64k \
  -movflags +faststart -f mov

# moov 在尾：默认有界回看内应输出与 moov 在前相同合同的 AU
enc "$OUTDIR/prog_moov_at_end.mp4" \
  -f lavfi -i testsrc=size=160x120:rate=25:duration=1 \
  -f lavfi -i sine=frequency=1000:sample_rate=48000:duration=1 \
  -c:v libx264 -pix_fmt yuv420p -profile:v baseline -bf 0 \
  -c:a aac -ar 48000 -ac 2 -b:a 64k \
  -f mp4

enc "$OUTDIR/prog_moov_at_end.mov" \
  -f lavfi -i testsrc=size=160x120:rate=25:duration=1 \
  -f lavfi -i sine=frequency=1000:sample_rate=44100:duration=1 \
  -c:v libx264 -pix_fmt yuv420p -profile:v baseline -bf 0 \
  -c:a aac -ar 44100 -ac 2 -b:a 64k \
  -f mov

# fMP4：init（empty_moov）+ default_base_is_moof，无 B 帧，按关键帧分片
enc "$OUTDIR/fmp4_baseline.mp4" \
  -f lavfi -i testsrc=size=160x120:rate=25:duration=1 \
  -f lavfi -i sine=frequency=1000:sample_rate=48000:duration=1 \
  -c:v libx264 -pix_fmt yuv420p -profile:v baseline -bf 0 -g 12 \
  -c:a aac -ar 48000 -ac 2 -b:a 64k \
  -movflags +empty_moov+default_base_moof+frag_keyframe -f mp4

# 多片：较短 frag_duration
enc "$OUTDIR/fmp4_multifrag.mp4" \
  -f lavfi -i testsrc=size=160x120:rate=25:duration=1 \
  -f lavfi -i sine=frequency=1000:sample_rate=48000:duration=1 \
  -c:v libx264 -pix_fmt yuv420p -profile:v baseline -bf 0 -g 25 \
  -c:a aac -ar 48000 -ac 2 -b:a 64k \
  -movflags +empty_moov+default_base_moof -frag_duration 200000 -f mp4

# 双轨 + B 帧（composition offset）+ 多片
enc "$OUTDIR/fmp4_bframes.mp4" \
  -f lavfi -i testsrc=size=160x120:rate=25:duration=1 \
  -f lavfi -i sine=frequency=1000:sample_rate=48000:duration=1 \
  -c:v libx264 -pix_fmt yuv420p -profile:v high -bf 3 -g 25 \
  -c:a aac -ar 48000 -ac 2 -b:a 64k \
  -movflags +empty_moov+default_base_moof+dash -frag_duration 400000 -f mp4

export INSPECT FFPROBE OUTDIR
python3 "$ROOT/tests/dev_bmff_au_crosscheck.py" \
  "$OUTDIR/prog_baseline.mp4" \
  "$OUTDIR/prog_bframes.mp4" \
  "$OUTDIR/prog_baseline.mov" \
  "$OUTDIR/prog_moov_at_end.mp4" \
  "$OUTDIR/prog_moov_at_end.mov" \
  "$OUTDIR/fmp4_baseline.mp4" \
  "$OUTDIR/fmp4_multifrag.mp4" \
  "$OUTDIR/fmp4_bframes.mp4"
