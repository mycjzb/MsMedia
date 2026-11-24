#!/usr/bin/env bash
# 开发交叉验证：用系统 ffmpeg 生成自有 lavfi MP4/fMP4/MOV，
# 再经 ms_bmff_inspect 对照独立箱体行走。不链入产品库，非正式验收。
# 样本不入库。本步只验证箱体结构，不组帧、不可播放。
# by: Long <mycjzb@gmail.com>
#
# 用法（仓库根，先 ./build.sh debug 或 release）：
#   tests/dev_bmff_crosscheck.sh
#   tests/dev_bmff_crosscheck.sh --inspect build/release/ms_bmff_inspect

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

FFMPEG="${FFMPEG:-/usr/bin/ffmpeg}"
FFPROBE="${FFPROBE:-/usr/bin/ffprobe}"
INSPECT=""
OUTDIR="$ROOT/build/dev-samples/bmff-crosscheck"

usage() {
  cat <<'U'
用法: tests/dev_bmff_crosscheck.sh [--inspect PATH] [--outdir DIR]
  需要已用 ./build.sh 编出 ms_bmff_inspect，以及系统 ffmpeg（可选开发工具）。
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
  for c in "$ROOT/build/debug/ms_bmff_inspect" \
           "$ROOT/build/release/ms_bmff_inspect" \
           "$ROOT/build/default/ms_bmff_inspect"; do
    if [[ -x "$c" ]]; then
      INSPECT="$c"
      break
    fi
  done
fi
if [[ -z "$INSPECT" || ! -x "$INSPECT" ]]; then
  echo "error: ms_bmff_inspect not found. Run ./build.sh debug first." >&2
  exit 2
fi
if [[ ! -x "$FFMPEG" ]]; then
  echo "error: need $FFMPEG (optional dev tool; not used in product)." >&2
  exit 2
fi

mkdir -p "$OUTDIR"
REPORT="$OUTDIR/crosscheck-report.txt"
: > "$REPORT"
log() { printf '%s\n' "$*" | tee -a "$REPORT"; }

log "inspect=$INSPECT"
log "ffmpeg=$FFMPEG ($("$FFMPEG" -version | head -n 1))"
if [[ -x "$FFPROBE" ]]; then
  log "ffprobe=$FFPROBE ($("$FFPROBE" -version | head -n 1))"
fi
log "outdir=$OUTDIR"
log "samples: lavfi testsrc+sine, encoded on this host, not redistributed media"
log "scope: box structure only; no sample table, no AU, not playable"
log ""

enc() {
  # args: outfile extra ffmpeg args...
  local out="$1"
  shift
  "$FFMPEG" -y -hide_banner -loglevel error \
    -f lavfi -i "testsrc=size=160x120:rate=15:duration=1" \
    -f lavfi -i "sine=frequency=440:sample_rate=48000:duration=1" \
    -c:v libx264 -pix_fmt yuv420p -c:a aac -ac 2 \
    "$@" "$out"
}

enc "$OUTDIR/prog_faststart.mp4" -movflags +faststart
enc "$OUTDIR/prog_moov_at_end.mp4"
enc "$OUTDIR/frag_empty_moov.mp4" -movflags frag_keyframe+empty_moov+default_base_moof
enc "$OUTDIR/classic.mov" -f mov

LIST="$OUTDIR/bmff-samples.list"
{
  echo "$OUTDIR/prog_faststart.mp4"
  echo "$OUTDIR/prog_moov_at_end.mp4"
  echo "$OUTDIR/frag_empty_moov.mp4"
  echo "$OUTDIR/classic.mov"
} > "$LIST"

export OUTDIR FFMPEG FFPROBE BMFF_INSPECT="$INSPECT" REPORT SAMPLE_LIST="$LIST"
python3 "$ROOT/tests/dev_bmff_crosscheck.py"
