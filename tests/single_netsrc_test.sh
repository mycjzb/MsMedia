#!/usr/bin/env bash
# 宿主 netsrc 全量回归（ASan/UBSan/LSan）：HTTP 渐进伪装、HLS TS/伪装 .png/fMP4 EXT-X-MAP+.m4s/伪装 .jpg/主清单、
# 内存上限、取消时延、错误路径、RTMP、RTSP(TS/ES/Digest)、UDP/RTP。只监听 127.0.0.1。
# by: Long <mycjzb@gmail.com>
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd); cd "$ROOT"
O=build/single-analysis/s4-host; N=$O/net; S=build/single-analysis/samples; T=$O/single_netsrc_test
OUT=$O/netsrc.txt
gcc -std=c99 -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer -DMS_SINGLE_SO=1 -Iinclude -Isrc -Isrc/core -Isrc/codec \
  -Isrc/container -Isrc/hw -Isrc/hls -Isrc/netcore -Itests tests/single_netsrc_test.c src/netsrc/ms_netsrc.c src/netsrc/ms_flvw.c \
  src/netsrc/ms_proto_udp.c src/netsrc/ms_proto_rtmp.c src/netsrc/ms_proto_rtsp.c src/netsrc/ms_mkvps.c src/netsrc/ms_tsguard.c src/netcore/ms_netcore.c src/core/ms_probe.c \
  $(cat build/single-analysis/s2-host/core.list) -lpthread -lm -o $T || exit 1
export ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1
pids=()
python3 tests/single_netsrc_servers.py http $N 18080 </dev/null >/dev/null 2>&1 & pids+=($!)
python3 tests/single_netsrc_servers.py rtmp $S/h264_aac.flv 11935 </dev/null >/dev/null 2>&1 & pids+=($!)
python3 tests/single_netsrc_servers.py rtsp $S/h264_aac.flv $S/h264_aac.ts 18554 </dev/null >/dev/null 2>&1 & pids+=($!)
trap 'kill ${pids[@]} 2>/dev/null' EXIT
sleep 1
: > $OUT; fails=0
run() { # name expect(PASS|FAIL) args...
  local name=$1 exp=$2; shift 2
  local r; r=$(timeout 90 $T "$@" 2>&1); local rc=$?
  local line; line=$(echo "$r" | grep '^RESULT' | tail -1)
  local san=0; echo "$r" | grep -qE "ERROR: (Address|Leak)Sanitizer|runtime error" && san=1
  local got=FAIL; [ $rc -eq 0 ] && got=PASS
  if [ "$got" = "$exp" ] && [ $san -eq 0 ] && [ -n "$line" ]; then echo "OK   $name :: $line" >> $OUT; else echo "BAD  $name rc=$rc san=$san :: $line" >> $OUT; echo "$r" | tail -30 >> $OUT; fails=$((fails+1)); fi
}
H=http://127.0.0.1:18080
run prog_png_ts     PASS $H/prog_png_ts.bin 200 400
run prog_png_mp4    PASS $H/prog_png_mp4.bin 200 400
run prog_jpeg_flv   PASS $H/prog_jpeg_flv.bin 200 400
run prog_mkv        PASS $H/plain.mkv 200 400
run prog_png_mkv_g711 PASS $H/png_mkv.bin 200 0
run prog_mkv_hevc   PASS $H/hevc.mkv 200 400
run prog_mkv_ac3    PASS $H/ac3.mkv 200 0
run prog_ps_gb_aac  PASS $H/gb_aac.ps 200 400
run prog_ps_mpeg1   PASS $H/plain.mpg 200 0
run prog_vob_ac3    PASS $H/ac3.vob 200 0
run ts_pts_wrap     PASS $H/wrap.ts 200 400
run ts_corrupt      PASS $H/corrupt.ts 150 300
F=file://$ROOT/$N
run file_mkv        PASS $F/plain.mkv 200 400
run file_png_ts     PASS $F/prog_png_ts.bin 200 400
run file_ps         PASS $F/gb_aac.ps 200 400
run file_corrupt    PASS $F/corrupt.ts 150 300
run file_m3u8_unsup FAIL $F/hls_ts/index.m3u8 0 0
run file_missing    FAIL $F/nope.bin 0 0
run hls_ts          PASS $H/hls_ts/index.m3u8 200 400
run hls_png_seg     PASS $H/hls_png/index.m3u8 200 400
run hls_fmp4_m4s    PASS $H/hls_fmp4/index.m3u8 200 400
run hls_fmp4_jpg    PASS $H/hls_fmp4_jpg/index.m3u8 200 400
run hls_master      PASS $H/master.m3u8 200 400
run hls_cap400k     PASS $H/hls_png/index.m3u8 200 400 4000 400000
run cancel_slow     PASS $H/slow 0 0 0 0 300
run cancel_hls      PASS $H/hls_fmp4/index.m3u8 0 0 0 0 20
run http_404        FAIL $H/nope.m3u8 0 0
run rtmp_live       PASS rtmp://127.0.0.1:11935/app/live 200 400
run rtmp_notfound   FAIL rtmp://127.0.0.1:11935/app/missing 0 0
run rtsp_mp2t       PASS rtsp://127.0.0.1:18554/ts 200 400
run rtsp_h264_aac   PASS rtsp://127.0.0.1:18554/es 200 400
run rtsp_digest     PASS rtsp://user:pass@127.0.0.1:18554/auth 200 400
run rtsp_badauth    FAIL rtsp://user:bad@127.0.0.1:18554/auth 0 0
(python3 tests/single_netsrc_servers.py udp-send $S/h264_aac.ts 15000 raw &)
run udp_ts          PASS udp://@127.0.0.1:15000 200 400 0 0 -1 4000
(python3 tests/single_netsrc_servers.py udp-send $S/h264_aac.ts 15002 rtp &)
run rtp_ts          PASS rtp://@127.0.0.1:15002 200 400 0 0 -1 4000
(python3 tests/single_netsrc_servers.py udp-send $S/h264_aac.ts 15004 rtp &)
run udp_auto_rtp    PASS udp://@127.0.0.1:15004 200 400 0 0 -1 4000
run udp_cancel      PASS udp://@127.0.0.1:15006 0 0 0 0 300
echo "NETSRC_$([ $fails -eq 0 ] && echo PASS || echo FAIL) fails=$fails" >> $OUT
cat $OUT
[ $fails -eq 0 ]
