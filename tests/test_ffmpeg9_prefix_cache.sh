#!/usr/bin/env bash
# 隔离验证 ffmpeg9_build.sh 前缀缓存决策：不得凭组件清单签发 stamp。
# 不修改真实补丁、不触碰冻结制品/历史归档、不编译 FFmpeg。
# 由仓库根目录 ./build.sh ffmpeg-cache-selftest 调用。
# by: Long <mycjzb@gmail.com>
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SCRIPT="$ROOT/scripts/ffmpeg9_build.sh"
SSL_PREFIX="$ROOT/build/openssl-3.5.9/prefix"
SSL_STAMP="$SSL_PREFIX/.msmedia-openssl-pin"
WORKDIR="$ROOT/build/ffmpeg-cache-selftest"
RUN="$WORKDIR/run-$$"

fail() { echo "FAIL: $*" >&2; exit 1; }
pass() { echo "PASS: $*"; }

if [[ ! -f "$SCRIPT" ]]; then
  fail "missing $SCRIPT"
fi
if [[ ! -f "$SSL_STAMP" || ! -f "$SSL_PREFIX/lib/libssl.a" ]]; then
  fail "pinned OpenSSL prefix/stamp missing at $SSL_PREFIX"
fi

rm -rf "$WORKDIR"
mkdir -p "$RUN"

# 当前脚本启用清单（与 scripts/ffmpeg9_build.sh CFG_ARGS 保持一致，仅用于假 config 头）。
DEMUXERS=(mpegts mpegtsraw flv live_flv mov hls dash rtsp rtp sdp aac h264 ogg matroska webvtt)
PARSERS=(h264 hevc mpegvideo mpeg4video h263 av1 aac aac_latm mpegaudio opus cavsvideo)
BSFS=(extract_extradata h264_mp4toannexb hevc_mp4toannexb aac_adtstoasc)

macro_of() {
  echo "$1" | tr 'a-z' 'A-Z' | tr '-' '_'
}

write_complete_components() {
  local dest="$1"
  local small="${2:-0}"
  local f
  mkdir -p "$(dirname "$dest/config_components.h")"
  {
    echo "/* isolated cache-selftest fixture; not a real FFmpeg build */"
    for item in "${DEMUXERS[@]}"; do
      echo "#define CONFIG_$(macro_of "$item")_DEMUXER 1"
    done
    for item in "${PARSERS[@]}"; do
      echo "#define CONFIG_$(macro_of "$item")_PARSER 1"
    done
    for item in "${BSFS[@]}"; do
      echo "#define CONFIG_$(macro_of "$item")_BSF 1"
    done
  } > "$dest/config_components.h"
  {
    echo "#define CONFIG_DECODERS 0"
    echo "#define CONFIG_SMALL $small"
    echo "#define CONFIG_GPL 0"
    echo "#define CONFIG_NONFREE 0"
  } > "$dest/config.h"
}

plant_prefix() {
  local root="$1"
  local small="${2:-0}"
  mkdir -p "$root/prefix/lib" "$root/build"
  printf 'old-archive-fixture\n' > "$root/prefix/lib/libavformat.a"
  # 模拟无 stamp 旧档案（早于当前配置请求）。
  touch -t 202611211205.00 "$root/prefix/lib/libavformat.a"
  write_complete_components "$root" "$small"
  cp "$SSL_STAMP" "$root/prefix/.msmedia-ffmpeg-openssl"
  printf 'UNIQUE-BUILT-FLAGS-DO-NOT-REWRITE\n' > "$root/configure-flags.txt"
  touch -t 202611211205.01 "$root/configure-flags.txt"
}

record_state() {
  local root="$1"
  local tag="$2"
  stat -c '%y %s %n' \
    "$root/prefix/lib/libavformat.a" \
    "$root/configure-flags.txt" \
    > "$tag.files"
  if [[ -f "$root/configure.stamp" ]]; then
    sha256sum "$root/configure.stamp" > "$tag.stamp"
    stat -c '%y %s' "$root/configure.stamp" >> "$tag.stamp"
    cp "$root/configure.stamp" "$tag.stamp.copy"
  else
    echo 'NO_STAMP' > "$tag.stamp"
  fi
  sha256sum "$root/configure-flags.txt" "$root/prefix/lib/libavformat.a" > "$tag.hashes"
}

assert_unchanged() {
  local root="$1"
  local tag="$2"
  record_state "$root" "$tag.after"
  cmp -s "$tag.files" "$tag.after.files" || fail "mtime/size changed vs $tag: $(diff -u "$tag.files" "$tag.after.files" || true)"
  cmp -s "$tag.hashes" "$tag.after.hashes" || fail "archive/flags hash changed vs $tag"
  cmp -s "$tag.stamp" "$tag.after.stamp" || fail "stamp changed vs $tag"
}

run_script() {
  # 参数经环境传入；退出码不因 set -e 中断调用方。
  env -u CC -u CXX -u LD -u CPATH -u C_INCLUDE_PATH -u LIBRARY_PATH \
    -u FFMPEG_PREFIX -u FFMPEG_BUILDDIR -u FFMPEG_REBUILD \
    -u OPENSSL_CONF -u OPENSSL_MODULES \
    PATH="/usr/bin:/bin:$PATH" \
    OPENSSL_PREFIX="$SSL_PREFIX" \
    bash "$SCRIPT"
  return $?
}

# stamp 含 --prefix，必须按该前缀路径单独生成期望指纹。
emit_expected_for() {
  local root="$1"
  local dest="$2"
  local size="${3:-0}"
  local rc=0
  local log="$dest.log"
  MS_FF_CACHE_CHECK_ONLY=1 \
    MS_FF_EMIT_EXPECTED="$dest" \
    FFMPEG_BUILD_ROOT="$root" \
    MS_SIZE_OPT="$size" \
    run_script >"$log" 2>&1 || rc=$?
  [[ "$rc" -eq 3 ]] || fail "emit_expected_for expected REBUILD/exit 3, got $rc; $(cat "$log")"
  [[ -f "$dest" ]] || fail "emit_expected_for did not write $dest"
  [[ ! -f "$root/configure.stamp" ]] || fail "emit_expected_for wrote configure.stamp"
}

case_no_stamp_complete_old_archive() {
  local root="$RUN/no-stamp-complete"
  plant_prefix "$root" 0
  record_state "$root" "$RUN/no-stamp-before"
  local rc=0
  local log="$RUN/no-stamp.log"
  MS_FF_CACHE_CHECK_ONLY=1 \
    MS_FF_EMIT_EXPECTED="$RUN/expected.stamp" \
    FFMPEG_BUILD_ROOT="$root" \
    MS_SIZE_OPT=0 \
    run_script >"$log" 2>&1 || rc=$?
  [[ "$rc" -eq 3 ]] || fail "no-stamp expected exit 3, got $rc; log: $(cat "$log")"
  grep -q "cache-decision: REBUILD" "$log" || fail "no-stamp missing REBUILD decision"
  grep -q "not issuing a stamp from components alone" "$log" || fail "no-stamp did not refuse component-only issuance"
  [[ -f "$root/configure.stamp" ]] && fail "no-stamp+complete components wrote configure.stamp"
  [[ -f "$RUN/expected.stamp" ]] || fail "expected stamp was not emitted for later cases"
  grep -q "UNIQUE-BUILT-FLAGS-DO-NOT-REWRITE" "$root/configure-flags.txt" || fail "flags log rewritten on no-stamp"
  assert_unchanged "$root" "$RUN/no-stamp-before"
  pass "no stamp + complete components + older archive → REBUILD, no stamp issued, flags/archive untouched"
}

case_complete_match() {
  local root="$RUN/complete-match"
  plant_prefix "$root" 0
  emit_expected_for "$root" "$RUN/match.expected" 0
  cp "$RUN/match.expected" "$root/configure.stamp"
  touch -t 202611221000.00 "$root/configure.stamp"
  record_state "$root" "$RUN/match-before"
  local rc=0
  local log="$RUN/match.log"
  MS_FF_CACHE_CHECK_ONLY=1 \
    FFMPEG_BUILD_ROOT="$root" \
    MS_SIZE_OPT=0 \
    run_script >"$log" 2>&1 || rc=$?
  [[ "$rc" -eq 0 ]] || fail "complete match expected exit 0, got $rc; log: $(cat "$log")"
  grep -q "cache-decision: SKIP" "$log" || fail "complete match missing SKIP"
  grep -q "configure stamp matches" "$log" || fail "complete match missing stamp-match message"
  assert_unchanged "$root" "$RUN/match-before"
  pass "complete stamp match → SKIP, no rebuild, flags/stamp/archive not rewritten"
}

case_request_flag_change() {
  local root="$RUN/flag-change"
  plant_prefix "$root" 0
  emit_expected_for "$root" "$RUN/flag.expected" 0
  cp "$RUN/flag.expected" "$root/configure.stamp"
  record_state "$root" "$RUN/flag-before"
  local rc=0
  local log="$RUN/flag-change.log"
  # 同一前缀改请求编译参数（size / -Os / --enable-small），不改真实补丁。
  MS_FF_CACHE_CHECK_ONLY=1 \
    FFMPEG_BUILD_ROOT="$root" \
    MS_SIZE_OPT=1 \
    run_script >"$log" 2>&1 || rc=$?
  [[ "$rc" -eq 3 ]] || fail "flag change expected exit 3, got $rc; log: $(cat "$log")"
  grep -q "cache-decision: REBUILD" "$log" || fail "flag change missing REBUILD"
  grep -q "configure.stamp differs" "$log" || fail "flag change did not detect stamp mismatch"
  assert_unchanged "$root" "$RUN/flag-before"
  pass "request flag change → REBUILD, existing stamp/flags not overwritten"
}

case_patch_content_change() {
  local root="$RUN/patch-change"
  plant_prefix "$root" 0
  emit_expected_for "$root" "$RUN/patch.expected" 0
  # 不改真实补丁：只把已种入 stamp 中的补丁哈希换成另一值，模拟补丁内容变化。
  python3 - "$RUN/patch.expected" "$root/configure.stamp" <<'PY'
import pathlib, re, sys
src, dest = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
text = src.read_text()
new, n = re.subn(
    r'(?m)^[0-9a-f]{64}  .+/patches/.+$',
    lambda m: ("ab" * 32) + m.group(0)[64:],
    text,
    count=1,
)
if n != 1:
    raise SystemExit(f"could not mutate one patch hash (n={n})")
dest.write_text(new)
PY
  record_state "$root" "$RUN/patch-before"
  local rc=0
  local log="$RUN/patch-change.log"
  MS_FF_CACHE_CHECK_ONLY=1 \
    FFMPEG_BUILD_ROOT="$root" \
    MS_SIZE_OPT=0 \
    run_script >"$log" 2>&1 || rc=$?
  [[ "$rc" -eq 3 ]] || fail "patch change expected exit 3, got $rc; log: $(cat "$log")"
  grep -q "cache-decision: REBUILD" "$log" || fail "patch change missing REBUILD"
  grep -q "configure.stamp differs" "$log" || fail "patch change did not detect stamp mismatch"
  assert_unchanged "$root" "$RUN/patch-before"
  pass "patch-content change (isolated stamp hash) → REBUILD, stamp/flags not overwritten"
}

case_failed_build_no_stamp() {
  local root="$RUN/fail-no-stamp"
  plant_prefix "$root" 0
  record_state "$root" "$RUN/fail-before"
  local rc=0
  local log="$RUN/fail-inject.log"
  MS_FF_FAKE_INSTALL=1 \
    MS_FF_INJECT_BUILD_FAILURE=1 \
    FFMPEG_BUILD_ROOT="$root" \
    MS_SIZE_OPT=0 \
    run_script >"$log" 2>&1 || rc=$?
  [[ "$rc" -eq 1 ]] || fail "injected failure expected exit 1, got $rc; log: $(cat "$log")"
  grep -q "injected build failure before writing configure.stamp" "$log" || fail "missing inject message"
  [[ -f "$root/configure.stamp" ]] && fail "failed build wrote configure.stamp"
  grep -q "UNIQUE-BUILT-FLAGS-DO-NOT-REWRITE" "$root/configure-flags.txt" || fail "failed build rewrote flags log"
  assert_unchanged "$root" "$RUN/fail-before"
  pass "build failure does not write success stamp or rewrite flags"

  # 负例：假安装不得在自测根目录之外执行。
  local rc2=0
  local outside="$ROOT/build/ffmpeg-9.1-cache-refuse-outside"
  mkdir -p "$outside/prefix/lib" "$outside/build"
  printf 'x\n' > "$outside/prefix/lib/libavformat.a"
  cp "$SSL_STAMP" "$outside/prefix/.msmedia-ffmpeg-openssl"
  MS_FF_FAKE_INSTALL=1 \
    MS_FF_INJECT_BUILD_FAILURE=0 \
    MS_FF_CACHE_CHECK_ONLY=0 \
    FFMPEG_BUILD_ROOT="$outside" \
    MS_SIZE_OPT=0 \
    run_script >"$RUN/fake-refuse.log" 2>&1 || rc2=$?
  [[ "$rc2" -eq 2 ]] || fail "FAKE_INSTALL outside selftest expected exit 2, got $rc2; $(cat "$RUN/fake-refuse.log")"
  grep -q "refused outside ffmpeg-cache-selftest" "$RUN/fake-refuse.log" || fail "missing refuse message"
  [[ -f "$outside/configure.stamp" ]] && fail "refused FAKE_INSTALL wrote a stamp outside selftest"
  rm -rf "$outside"
  pass "FAKE_INSTALL refused outside ffmpeg-cache-selftest root"
}

case_no_stamp_complete_old_archive
case_complete_match
case_request_flag_change
case_patch_content_change
case_failed_build_no_stamp

echo "all ffmpeg prefix cache selftests passed under $RUN"
