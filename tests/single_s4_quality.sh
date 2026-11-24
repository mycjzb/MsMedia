#!/usr/bin/env bash
# 阶段 4 质量红线（宿主，ASan+UBSan+LSan）：自研 xml / probe 伪装剥离 / HLS 预加载线程 / netsrc+proto+mkvps+tsguard / passthrough。
# 含高频 open/close、预加载中途取消/切台、多线程并发退出、畸形输入（截断/0 字节/超大 box/XML/RTMP/RTSP/TS、坏 PNG/JPG 头、中途断连）。
# 只监听 127.0.0.1。输出 build/single-analysis/s4-host/quality.txt。
# by: Long <mycjzb@gmail.com>
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd); cd "$ROOT"
O=build/single-analysis/s4-host; N=$O/net; M=$O/malformed; S=build/single-analysis/samples
OUT=$O/quality.txt; : > $OUT
SAN="-g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer"
export ASAN_OPTIONS=detect_leaks=1:halt_on_error=1:abort_on_error=0 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1
fails=0
note() { echo "$*" | tee -a $OUT; }
check() { # name rc logfile
  local name=$1 rc=$2 log=$3 san=0
  grep -qE "ERROR: (Address|Leak)Sanitizer|runtime error|SUMMARY: (Address|Undefined|Leak)" "$log" && san=1
  if [ "$rc" -eq 0 ] && [ $san -eq 0 ]; then note "OK   $name :: $(grep -E '^(STRESS|PASSTHROUGH|PROBE_TEST|XML_TEST|NETSRC)' "$log" | tail -1)"
  else note "BAD  $name rc=$rc san=$san"; tail -40 "$log" >> $OUT; fails=$((fails+1)); fi
}
# 1) 单元：passthrough / probe / xml
gcc -std=c11 $SAN -Wall -Wextra -Iinclude -Isrc/hw tests/single_passthrough_test.c src/hw/ms_hw_passthrough.c -o $O/pt/pt_test
$O/pt/pt_test $O/pt > $O/q_pt.log 2>&1; check passthrough $? $O/q_pt.log
gcc -std=c99 $SAN -DMS_SINGLE_SO=1 -Iinclude -Isrc -Isrc/core -Isrc/codec -Isrc/container tests/single_probe_test.c src/core/ms_probe.c \
  $(cat build/single-analysis/s2-host/core.list) -lm -o $O/probe_test_q
$O/probe_test_q $O/probe $S > $O/q_probe.log 2>&1; check probe_disguise $? $O/q_probe.log
bash tests/single_xml_test.sh > $O/q_xml.log 2>&1; check xml_shim $? $O/q_xml.log
# 2) 服务
T=$O/single_s4_stress
gcc -std=c99 $SAN -DMS_SINGLE_SO=1 -Iinclude -Isrc -Isrc/core -Isrc/codec -Isrc/container -Isrc/hw -Isrc/hls -Isrc/netcore -Itests \
  tests/single_s4_stress.c src/netsrc/ms_netsrc.c src/netsrc/ms_flvw.c src/netsrc/ms_proto_udp.c src/netsrc/ms_proto_rtmp.c \
  src/netsrc/ms_proto_rtsp.c src/netsrc/ms_mkvps.c src/netsrc/ms_tsguard.c src/netcore/ms_netcore.c src/core/ms_probe.c \
  $(cat build/single-analysis/s2-host/core.list) -lpthread -lm -o $T || exit 1
pids=()
python3 tests/single_netsrc_servers.py http $N 18180 </dev/null >/dev/null 2>&1 & pids+=($!)
python3 tests/single_netsrc_servers.py rtmp $S/h264_aac.flv 11945 </dev/null >/dev/null 2>&1 & pids+=($!)
python3 tests/single_netsrc_servers.py rtsp $S/h264_aac.flv $S/h264_aac.ts 18564 </dev/null >/dev/null 2>&1 & pids+=($!)
python3 tests/single_netsrc_servers.py junk 18600 </dev/null >/dev/null 2>&1 & pids+=($!)
trap 'kill ${pids[@]} 2>/dev/null' EXIT
sleep 1
H=http://127.0.0.1:18180; F=file://$ROOT/$N
URLS=("$H/hls_ts/index.m3u8" "$H/hls_fmp4/index.m3u8" "$H/hls_png/index.m3u8" "$H/master.m3u8" "$H/hls_fmp4_jpg/index.m3u8"
      "$H/plain.mkv" "$H/gb_aac.ps" "$H/slow" "$F/plain.mkv" "$F/corrupt.ts" "rtmp://127.0.0.1:11945/app/live"
      "rtsp://127.0.0.1:18564/ts" "rtsp://127.0.0.1:18564/es")
# 高频 open/close + 随机时刻取消（含预加载中途）+ URL 轮换（切台）
$T openclose ${OPENCLOSE_ITERS:-300} "${URLS[@]}" > $O/q_openclose.log 2>&1; check openclose_switch $? $O/q_openclose.log
# 多线程并发生命周期（并发线程退出）
$T concurrent ${CONC_ITERS:-40} ${CONC_THREADS:-6} "${URLS[@]}" > $O/q_concurrent.log 2>&1; check concurrent_threads $? $O/q_concurrent.log
# 畸形本地文件
MF=(); for f in $M/*; do MF+=("file://$ROOT/$f"); done
$T malformed 3000 "${MF[@]}" > $O/q_malformed_file.log 2>&1; check malformed_files $? $O/q_malformed_file.log
# 畸形网络：中途断连 / 超大长度 / 畸形清单 / 畸形 RTMP / RTSP
NU=("$H/hugelen" "$H/cut/plain.mkv" "$H/cut/gb_aac.ps" "$H/cut/prog_png_ts.bin" "$H/cut/prog_png_mp4.bin" "$H/cut/corrupt.ts"
    "$H/bad/loop.m3u8" "$H/bad/huge.m3u8" "$H/bad/badmap.m3u8" "$H/bad/bin.m3u8" "$H/bad/deep.mpd" "$H/bad/huge.mpd" "$H/bad/trunc.mpd"
    "$H/cut/hls_ts/index.m3u8")
for i in 1 2 3 4 5 6 7 8; do NU+=("rtmp://127.0.0.1:18600/app/x$i" "rtsp://127.0.0.1:18600/x$i"); done
$T malformed 4000 "${NU[@]}" > $O/q_malformed_net.log 2>&1; check malformed_net $? $O/q_malformed_net.log
# 3) 既有 netsrc 全量回归
bash tests/single_netsrc_test.sh > $O/q_netsrc.log 2>&1; check netsrc_regression $? $O/q_netsrc.log
note "QUALITY_$([ $fails -eq 0 ] && echo PASS || echo FAIL) fails=$fails date_cst=$(date '+%Y-%m-%d %H:%M:%S') Asia/Shanghai"
[ $fails -eq 0 ]
