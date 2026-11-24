#!/usr/bin/env bash
# 自研 XML 垫片验收：真 libxml2 vs ms_xml 差分（逐字节一致）+ ASan/UBSan/LSan 变异模糊。
# by: Long <mycjzb@gmail.com>
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/build/single-analysis/s4-host/xml"
mkdir -p "$OUT"
SAN="-fsanitize=address,undefined -fno-sanitize-recover=all -g -O1"
gcc $SAN -I/usr/include/libxml2 "$ROOT/tests/single_xml_dump.c" -lxml2 -o "$OUT/dump_libxml2"
gcc $SAN -Wall -Wextra -I"$ROOT/src/xml/include/libxml2" "$ROOT/tests/single_xml_dump.c" "$ROOT/src/xml/ms_xml.c" -o "$OUT/dump_msxml"
gcc $SAN -Wall -Wextra -I"$ROOT/src/xml/include/libxml2" "$ROOT/tests/single_xml_fuzz.c" "$ROOT/src/xml/ms_xml.c" -o "$OUT/fuzz_msxml"
fail=0
for f in "$ROOT"/tests/fixtures/mpd/*.mpd; do
  b=$(basename "$f")
  "$OUT/dump_libxml2" "$f" > "$OUT/$b.libxml2.txt" 2>/dev/null
  "$OUT/dump_msxml" "$f" > "$OUT/$b.msxml.txt"
  if cmp -s "$OUT/$b.libxml2.txt" "$OUT/$b.msxml.txt"; then echo "DIFF_OK $b ($(wc -l < "$OUT/$b.msxml.txt") lines)"; else echo "DIFF_FAIL $b"; diff "$OUT/$b.libxml2.txt" "$OUT/$b.msxml.txt" | head -20 || true; fail=1; fi
done
"$OUT/fuzz_msxml" "$ROOT"/tests/fixtures/mpd/*.mpd
[[ $fail == 0 ]] && echo XML_TEST_PASS || { echo XML_TEST_FAIL; exit 1; }
