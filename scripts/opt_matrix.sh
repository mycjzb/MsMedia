#!/usr/bin/env bash
# 独立优化/sanitizer 矩阵。产物只写 build/opt-matrix/<cfg>，
# 不删除、不覆盖 build/debug 与 build/release。
# 用法（仓库根）：
#   scripts/opt_matrix.sh            # 默认配置集，跑独立 miss 复现 + 可选全组
#   scripts/opt_matrix.sh --full     # 每配置加跑 test_core
#   scripts/opt_matrix.sh o2 o2-novrp
# by: Long <mycjzb@gmail.com>
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
# 避免继承缺失的 conda gcc
export PATH="/usr/bin:/bin:${PATH:-}"
unset CC CXX LD 2>/dev/null || true

CC="${MATRIX_CC:-/usr/bin/gcc}"
AR="${AR:-/usr/bin/ar}"
RANLIB="${RANLIB:-/usr/bin/ranlib}"
BASE_OUT="$ROOT/build/opt-matrix"
FULL=0
CFGS=()

while [[ $# -gt 0 ]]; do
  case "$1" in
    --full) FULL=1; shift ;;
    --cc) CC="${2:-}"; shift 2 ;;
    -h|--help)
      echo "usage: scripts/opt_matrix.sh [--full] [--cc /usr/bin/gcc] [cfg...]"
      echo "cfgs: o0 o1 o2 o2-novrp o2-ndebug-off o2-asan o2-ubsan o2-asan-ubsan o2-nosa o2-fwrapv"
      exit 0
      ;;
    *) CFGS+=("$1"); shift ;;
  esac
done

if [[ ${#CFGS[@]} -eq 0 ]]; then
  CFGS=(o0 o1 o2 o2-novrp o2-ndebug-off o2-ubsan o2-asan-ubsan o2-nosa o2-fwrapv)
fi

COMMON=(-std=c11 -Wall -Wextra -Werror -Wshadow -Wstrict-prototypes
        -Wmissing-prototypes -fno-common -g -fno-omit-frame-pointer
        -I"$ROOT/include" -I"$ROOT/src/core" -I"$ROOT/src/codec"
        -I"$ROOT/src/container")

SOURCES=(
  src/core/ms_crc32.c
  src/core/ms_util.c
  src/core/ms_registry.c
  src/core/ms_session.c
  src/codec/ms_adts.c
  src/codec/ms_h264.c
  src/codec/ms_hevc.c
  src/codec/ms_hdr.c
  src/codec/ms_vbit.c
  src/codec/ms_abit.c
  src/codec/ms_avcc.c
  src/container/ms_ts.c
  src/container/ms_bmff.c
  src/container/ms_bmff_sample.c
  src/container/ms_flv.c
  src/source/ms_source.c
  src/hls/ms_hls.c
)

flags_for() {
  local cfg="$1"
  EXTRA=()
  SAN=()
  case "$cfg" in
    o0) EXTRA=(-O0) ;;
    o1) EXTRA=(-O1 -DNDEBUG) ;;
    o2) EXTRA=(-O2 -DNDEBUG) ;;
    o2-novrp) EXTRA=(-O2 -DNDEBUG -fno-tree-vrp) ;;
    o2-ndebug-off) EXTRA=(-O2) ;;  # 无 NDEBUG，对照 assert/宏
    o2-ubsan) EXTRA=(-O2 -DNDEBUG); SAN=(-fsanitize=undefined) ;;
    o2-asan) EXTRA=(-O2 -DNDEBUG); SAN=(-fsanitize=address) ;;
    o2-asan-ubsan) EXTRA=(-O2 -DNDEBUG); SAN=(-fsanitize=address,undefined) ;;
    o2-nosa) EXTRA=(-O2 -DNDEBUG -fno-strict-aliasing) ;;
    o2-fwrapv) EXTRA=(-O2 -DNDEBUG -fwrapv) ;;
    o0-ndebug) EXTRA=(-O0 -DNDEBUG) ;;
    *) echo "unknown cfg: $cfg" >&2; return 2 ;;
  esac
}

build_cfg() {
  local cfg="$1"
  local out="$BASE_OUT/$cfg"
  mkdir -p "$out/obj" "$out/obj/tests"
  flags_for "$cfg"
  local cflags=("${COMMON[@]}" "${EXTRA[@]}" "${SAN[@]+"${SAN[@]}"}")
  local objs=()
  local src base obj
  for src in "${SOURCES[@]}"; do
    base="${src#src/}"
    obj="$out/obj/${base%.c}.o"
    mkdir -p "$(dirname "$obj")"
    "$CC" "${cflags[@]}" -c "$ROOT/$src" -o "$obj"
    objs+=("$obj")
  done
  "$AR" rcs "$out/libmsmedia.a" "${objs[@]}"
  "$RANLIB" "$out/libmsmedia.a"
  "$CC" "${cflags[@]}" -I"$ROOT/tests" -c "$ROOT/tests/ms_ts_synth.c" -o "$out/obj/tests/ms_ts_synth.o"
  "$CC" "${cflags[@]}" -I"$ROOT/tests" -c "$ROOT/tests/repro_hls_miss.c" -o "$out/obj/tests/repro_hls_miss.o"
  "$CC" "${cflags[@]}" -o "$out/repro_hls_miss" \
    "$out/obj/tests/repro_hls_miss.o" "$out/obj/tests/ms_ts_synth.o" "$out/libmsmedia.a"
  if [[ "$FULL" == "1" ]]; then
    "$CC" "${cflags[@]}" -I"$ROOT/tests" -c "$ROOT/tests/test_core.c" -o "$out/obj/tests/test_core.o"
    "$CC" "${cflags[@]}" -o "$out/test_core" \
      "$out/obj/tests/test_core.o" "$out/obj/tests/ms_ts_synth.o" "$out/libmsmedia.a"
  fi
  {
    echo "cfg=$cfg"
    echo "cc=$("$CC" --version | head -1)"
    echo "cc_path=$CC"
    echo -n "cflags="; printf '%s ' "${cflags[@]}"; echo
    date '+time=%Y-%m-%d %H:%M:%S %Z'
  } > "$out/flags.txt"
}

run_cfg() {
  local cfg="$1"
  local out="$BASE_OUT/$cfg"
  local st
  set +e
  "$out/repro_hls_miss" >"$out/repro.out" 2>"$out/repro.err"
  st=$?
  set -e
  echo "repro_exit=$st" >> "$out/flags.txt"
  if [[ $st -eq 0 ]] && grep -q "repro_hls_miss ok" "$out/repro.out"; then
    echo "REPRO $cfg PASS"
  else
    echo "REPRO $cfg FAIL exit=$st"
    tail -5 "$out/repro.out" || true
    cat "$out/repro.err" || true
  fi
  if [[ "$FULL" == "1" && -x "$out/test_core" ]]; then
    set +e
    "$out/test_core" >"$out/test_core.out" 2>"$out/test_core.err"
    st=$?
    set -e
    echo "test_core_exit=$st" >> "$out/flags.txt"
    if [[ $st -eq 0 ]] && grep -q "all tests passed" "$out/test_core.out"; then
      echo "CORE  $cfg PASS"
    else
      echo "CORE  $cfg FAIL exit=$st"
      rg -n "FAIL |all tests|SUMMARY|ERROR" "$out/test_core.out" "$out/test_core.err" | head -8 || true
    fi
  fi
}

echo "matrix CC=$CC root=$ROOT out=$BASE_OUT full=$FULL"
echo "cfgs: ${CFGS[*]}"
"$CC" --version | head -1
mkdir -p "$BASE_OUT"
for cfg in "${CFGS[@]}"; do
  echo "== build $cfg"
  build_cfg "$cfg"
  run_cfg "$cfg"
done
echo "== matrix done"
