#!/usr/bin/env bash
# 宿主编跑 QuickJS eval 自检（不编 APK）。
#   bash tests/qjs_eval_host.sh
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
[[ -f "$ROOT/third_party/quickjs/quickjs.c" ]] || bash "$ROOT/scripts/fetch_quickjs.sh"
QJS="$ROOT/third_party/quickjs"
VER=$(tr -d '\r\n' <"$QJS/VERSION")
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
cc -O1 -D_GNU_SOURCE -DMS_QJS_HOST_STUB -DCONFIG_VERSION="\"$VER\"" -fwrapv \
  -I"$QJS" -I"$ROOT/src/qjs" -I"$ROOT/src" -I"$ROOT/src/netcore" \
  -o "$OUT/t" \
  "$ROOT/tests/qjs_eval_host.c" \
  "$ROOT/tests/qjs_net_stub.c" \
  "$ROOT/src/qjs/ms_qjs.c" \
  "$ROOT/src/qjs/ms_qjs_bind.c" \
  "$ROOT/src/qjs/ms_qjs_crypto.c" \
  "$ROOT/src/qjs/ms_qjs_wrap.c" \
  "$QJS/quickjs.c" "$QJS/dtoa.c" "$QJS/libregexp.c" "$QJS/libunicode.c" "$QJS/cutils.c" \
  -lpthread -lm \
  -Wno-unused-parameter -Wno-sign-compare
"$OUT/t"
echo "qjs_eval_host: PASS"
