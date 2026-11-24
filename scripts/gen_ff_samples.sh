#!/usr/bin/env bash
# 用系统 ffmpeg/ffprobe 生成受控测试样本到构建目录（不入库）。
# 只供 test_ff_* 使用，不是产品后端，产品库不 exec 本脚本或 ffmpeg。
# by: Long <mycjzb@gmail.com>
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${1:-$ROOT/build/dev-samples/dash-ff}"
FFMPEG="${FFMPEG:-/usr/bin/ffmpeg}"
FFPROBE="${FFPROBE:-/usr/bin/ffprobe}"
mkdir -p "$OUT"
if [[ ! -x "$FFMPEG" ]]; then
  echo "gen_ff_samples: no $FFMPEG, skip" >&2
  exit 0
fi
# 短 VOD DASH：分离 A/V AdaptationSet，相对 SegmentTemplate，约 3 秒，无 B 帧。
"$FFMPEG" -y -hide_banner -loglevel error \
  -f lavfi -i "testsrc=size=160x120:rate=25:duration=3" \
  -f lavfi -i "sine=frequency=1000:sample_rate=48000:duration=3" \
  -c:v libx264 -pix_fmt yuv420p -profile:v baseline -preset veryfast \
  -x264-params "keyint=25:min-keyint=25:scenecut=0:bframes=0" \
  -c:a aac -b:a 64k -ac 2 -ar 48000 \
  -f dash -seg_duration 1 -use_template 1 -use_timeline 1 \
  -adaptation_sets "id=0,streams=v id=1,streams=a" \
  -init_seg_name 'init-$RepresentationID$.m4s' \
  -media_seg_name 'chunk-$RepresentationID$-$Number%05d$.m4s' \
  "$OUT/manifest.mpd"
cp "$OUT/manifest.mpd" "$OUT/manifest_noext"
# 独立对照：系统 ffprobe 的 packet 表（不是产品软解，也不入库码流以外的隐私）
if [[ -x "$FFPROBE" ]]; then
  "$FFPROBE" -hide_banner -v error -show_packets -of json "$OUT/manifest.mpd" > "$OUT/ffprobe_packets.json"
  python3 - "$OUT" << 'PY'
import json, sys, os
out = sys.argv[1]
data = json.load(open(os.path.join(out, "ffprobe_packets.json")))
pkts = data.get("packets") or data.get("packet") or []
# ffprobe json may be {packets:[...]}
if isinstance(data, dict) and "packets" not in data:
    # sometimes a list at top via alternate
    pass
rows_v, rows_a = [], []
for p in data.get("packets", []):
    codec = p.get("codec_type") or ""
    pts = p.get("pts", "N/A")
    dts = p.get("dts", "N/A")
    size = p.get("size") or 0
    flags = p.get("flags") or ""
    key = 1 if "K" in flags else 0
    line = f"{codec}\t{pts}\t{dts}\t{size}\t{key}\n"
    if codec == "video":
        rows_v.append(line)
    elif codec == "audio":
        rows_a.append(line)
open(os.path.join(out, "ref_video.txt"), "w").writelines(rows_v)
open(os.path.join(out, "ref_audio.txt"), "w").writelines(rows_a)
open(os.path.join(out, "ref_meta.txt"), "w").write(
    f"video_packets={len(rows_v)}\naudio_packets={len(rows_a)}\n"
)
print(f"ref: video={len(rows_v)} audio={len(rows_a)}")
PY
fi
# 独立 init+trun 表（含 elst），对照解封装尾包，不依赖某一版 ffprobe 的 packet 数
python3 "$ROOT/scripts/dash_trun_ref.py" "$OUT"

# B 帧短 MP4，供 DTS/PTS 重排与有符号时间对照（不入库）
"$FFMPEG" -y -hide_banner -loglevel error \
  -f lavfi -i "testsrc=size=160x120:rate=25:duration=1" \
  -f lavfi -i "sine=frequency=1000:sample_rate=48000:duration=1" \
  -c:v libx264 -pix_fmt yuv420p -preset veryfast \
  -x264-params "bframes=3:b-adapt=0:keyint=12:min-keyint=12" \
  -c:a aac -b:a 64k -ac 2 -ar 48000 \
  -movflags +faststart "$OUT/bframes.mp4"

# 短 MPEG-TS（约 1 秒），供可选 UDP 发送端，不入库到源码树
"$FFMPEG" -y -hide_banner -loglevel error \
  -f lavfi -i "testsrc=size=160x120:rate=25:duration=1" \
  -f lavfi -i "sine=frequency=1000:sample_rate=48000:duration=1" \
  -c:v libx264 -pix_fmt yuv420p -profile:v baseline -preset veryfast \
  -x264-params "keyint=25:min-keyint=25:scenecut=0:bframes=0" \
  -c:a aac -b:a 64k -ac 2 -ar 48000 \
  -f mpegts "$OUT/short.ts"
ls -l "$OUT"
echo "samples ready in $OUT"

# RTP/RTSP 帧合同用短 AnnexB（多 slice + 可 FU-A 分片），及 RTMP 用短 FLV。不入库。
"$FFMPEG" -y -hide_banner -loglevel error \
  -f lavfi -i "testsrc=size=320x240:rate=25:duration=0.64" \
  -c:v libx264 -pix_fmt yuv420p -profile:v baseline -preset veryfast \
  -x264-params "keyint=5:min-keyint=5:scenecut=0:bframes=0:sliced-threads=0" \
  -slices 2 -an -bsf:v h264_mp4toannexb -f h264 "$OUT/slices.264"
python3 "$ROOT/scripts/rtp_h264_testutil.py" pack --annexb "$OUT/slices.264" --out-dir "$OUT/rtp-h264" --mtu 200

"$FFMPEG" -y -hide_banner -loglevel error \
  -f lavfi -i "testsrc=size=160x120:rate=25:duration=1" \
  -f lavfi -i "sine=frequency=1000:sample_rate=48000:duration=1" \
  -c:v libx264 -pix_fmt yuv420p -profile:v baseline -preset veryfast \
  -x264-params "keyint=25:min-keyint=25:scenecut=0:bframes=0" \
  -c:a aac -b:a 64k -ac 2 -ar 48000 \
  -f flv "$OUT/short.flv"
