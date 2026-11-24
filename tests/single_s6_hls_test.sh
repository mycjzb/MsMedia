#!/usr/bin/env bash
# 阶段 6 宿主专项回归（ASan + UBSan + LSan）：Media3 风格 HLS 预下载（水位/滞回/首片边下边播/重缓冲/
# 弱网断点续传重试/4xx 不重试/取消时延/切台零泄漏/Keep-Alive）、AES-128（显式 IV 与序号 IV）、
# EXT-X-MAP fMP4、EXT-X-BYTERANGE、伪装 .png/.jpg 切片与伪装 MKV、直播刷新、
# 自签 HTTPS：播放器（默认忽略证书）放行 vs MsHttpClient 同款 netcore 严格校验拦截；播放器严格开关也拦截。
# 依赖：scripts/host_slimff_ffmpeg.sh（宿主精简 FFmpeg s6 前缀）、ABIS=host scripts/openssl_slim_build.sh。
# by: Long <mycjzb@gmail.com>
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd); cd "$ROOT"
O=build/single-analysis/s6-host; N=$O/net; T=$O/single_s6_hls_test; OUT=$O/s6-host.txt
FFP=build/host/ffmpeg-9.1-slimff-s6; SSL=build/host/openssl-3.5.9-slim/prefix
SRC=build/single-analysis/ffmpeg-9.1-slimff-src
HP=${HP:-18290}; SP=${SP:-18291}
mkdir -p $O/obj
[[ -d $N/aes ]] || python3 tests/single_s6_hls_server.py gen build/single-analysis/s4-host/net $N || exit 1
SAN="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined"
DEFS="-DMS_SINGLE_SO=1 -DMSMEDIA_NO_OPENSSL -DMS_NC_TLS_OPENSSL -DMS_HLS_PREFETCH -D_GNU_SOURCE"
INC="-Iinclude -Isrc -Isrc/core -Isrc/codec -Isrc/container -Isrc/netcore -Isrc/ff -I$FFP/prefix/include -I$SSL/include"
INTERNAL="-I$FFP/build -I$SRC -I$SRC/libavformat"
CF="-std=gnu11 -g -O1 $SAN $DEFS"
objs=()
build() { # src [internal]
  local s=$1 o=$O/obj/$(echo "$1" | tr '/' '_' | sed 's/\.c$/.o/')
  gcc $CF $INC ${2:+$INTERNAL} -c "$s" -o "$o" || exit 1
  objs+=("$o")
}
if [[ "${SKIP_BUILD:-0}" != 1 ]]; then
  for s in tests/single_s6_hls_test.c src/ff/ms_ff.c src/netcore/ms_netcore.c src/netcore/ms_nc_tls_ossl.c src/core/ms_probe.c \
           $(cat build/single-analysis/s2-host/core.list); do build "$s"; done
  for s in src/ff/ms_hls_pf.c src/ff/tcp_ms.c src/ff/tls_android.c; do build "$s" internal; done
  gcc $SAN -o $T "${objs[@]}" $FFP/prefix/lib/libavformat.a $FFP/prefix/lib/libavcodec.a $FFP/prefix/lib/libavutil.a \
    $SSL/lib/libssl.a $SSL/lib/libcrypto.a -lz -lm -lpthread -ldl || exit 1
fi
export ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1
python3 tests/single_s6_hls_server.py serve $N $HP $SP </dev/null >$O/server.log 2>&1 & SPID=$!
trap 'kill $SPID 2>/dev/null' EXIT
sleep 1.5
: > $OUT; fails=0
run() { # name expect(PASS|FAIL) grep_regex_or_- args...
  local name=$1 exp=$2 want=$3; shift 3
  if [[ -n "${ONLY:-}" && ! "$name" =~ $ONLY ]]; then return; fi
  local r; r=$(timeout 120 $T "$@" 2>&1); local rc=$?
  local line; line=$(echo "$r" | grep -E '^(RESULT|STATS)' | tr '\n' ' ')
  local san=0; echo "$r" | grep -qE "ERROR: (Address|Leak)Sanitizer|runtime error" && san=1
  local got=FAIL; [ $rc -eq 0 ] && got=PASS
  local wok=1; [ "$want" != - ] && ! echo "$r" | grep -qE "$want" && wok=0
  if [ "$got" = "$exp" ] && [ $san -eq 0 ] && [ $wok -eq 1 ] && echo "$r" | grep -q '^RESULT'; then
    echo "OK   $name :: $line" >> $OUT
  else
    echo "BAD  $name rc=$rc san=$san want=$wok :: $line" >> $OUT; echo "$r" | tail -40 >> $OUT; fails=$((fails+1))
  fi
}
H=http://127.0.0.1:$HP; S=https://127.0.0.1:$SP
# 预下载 + 各类 HLS
run hls_ts          PASS 'planned=[1-9]'           play $H/hls_ts/index.m3u8 200
run hls_png_seg     PASS 'hit=[1-9]'               play $H/hls_png/index.m3u8 200
run hls_fmp4_map    PASS 'hit=[1-9]'               play $H/hls_fmp4/index.m3u8 200
run hls_fmp4_jpg    PASS 'au=[0-9]'                play $H/hls_fmp4_jpg/index.m3u8 200
run hls_byterange   PASS 'planned=1 done=1 hit=1'       play $H/br/index.m3u8 200
run hls_aes_iv      PASS 'hit=[1-9]'               play $H/aes/index.m3u8 200
run hls_aes_seq     PASS 'hit=[1-9]'               play $H/aes_seq/index.m3u8 200
run hls_keepalive   PASS 'conn_reuse=[1-9]'        play $H/hls_ts/index.m3u8 250
run hls_live        PASS 'au=[0-9]'                play $H/live/index.m3u8 150
# 水位/滞回/字节上限
run hls_watermark   PASS 'planned=[1-9]'           play $H/hls_ts/index.m3u8 200 -1 2000,4000,500,1000,300000
run hls_pf_off      PASS 'au=[0-9]'                play $H/hls_ts/index.m3u8 200 -1 0,0,0,0,0
# 弱网：每个切片第 1 次请求断在一半 -> 断点续传重试；4xx -> 失败不重试
run hls_flaky       PASS 'retry=[1-9]'             play $H/flaky/hls_ts/index.m3u8 200
run hls_flaky_aes   PASS 'retry=[1-9]'             play $H/flaky/aes/index.m3u8 200
run hls_slow_first  PASS 'au=[0-9]'                play $H/slow/hls_ts/index.m3u8 100
run http_404        FAIL -                         play $H/fail404/hls_ts/index.m3u8 1
# 渐进式伪装
run png_mkv         PASS 'au=[0-9]'                play $H/png_mkv.bin 50
run png_ts          PASS 'au=[0-9]'                play $H/prog_png_ts.bin 100
# 取消 / seek / 切台压力
run cancel_open     PASS -                         cancel $H/slow/hls_ts/index.m3u8 30
run cancel_play     PASS -                         cancel $H/slow/hls_ts/index.m3u8 600
run seek_hls        PASS -                         seek $H/hls_ts/index.m3u8
run seek_aes        PASS -                         seek $H/aes/index.m3u8
run stress_switch   PASS 'ok=30'                   stress $H/hls_ts/index.m3u8 30
run stress_aes      PASS 'ok=15'                   stress $H/flaky/aes_seq/index.m3u8 15
# TLS 隔离：自签 HTTPS
run tls_player_default_pass PASS 'au=[0-9]'        play $S/hls_ts/index.m3u8 150
run tls_player_aes_pass     PASS 'hit=[1-9]'       play $S/aes/index.m3u8 150
run tls_player_strict_block FAIL -                 play $S/hls_ts/index.m3u8 1 1
run tls_nc_strict_block     FAIL 'blocked'         nc $S/hls_ts/index.m3u8
run tls_nc_with_ca_pass     PASS 'status=200'      nc $S/hls_ts/index.m3u8 $N/selfsigned.crt
run tls_nc_real_hostcheck   FAIL 'blocked'         nc https://localhost:$SP/hls_ts/index.m3u8 $N/selfsigned.crt
echo "S6_HOST_$([ $fails -eq 0 ] && echo PASS || echo FAIL) fails=$fails" >> $OUT
cat $OUT
[ $fails -eq 0 ]
