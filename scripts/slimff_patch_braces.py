#!/usr/bin/env python3
# clang -Wmissing-braces：嵌套聚合 {0} → {{0}}。只改 slimff 源码副本，幂等。
# by: Long <mycjzb@gmail.com>
import pathlib
import sys

MARK = "/* msmedia-braces */"
FIXES = [
    ("libavformat/ip.c",
     "struct sockaddr_storage source_addr = {0};",
     "struct sockaddr_storage source_addr = {{0}}; " + MARK),
    ("libavcodec/aac/aacdec_usac_mps212.c",
     "int16_t *p0_data[2][2] = { 0 };",
     "int16_t *p0_data[2][2] = { { 0 } }; " + MARK),
    ("libavcodec/aac/aacdec_usac_mps212.c",
     "int16_t data_pair[2][28] = { 0 };",
     "int16_t data_pair[2][28] = { { 0 } }; " + MARK),
    ("libavcodec/aac/aacdec_usac_mps212.c",
     "int16_t data_diff[2][28] = { 0 };",
     "int16_t data_diff[2][28] = { { 0 } }; " + MARK),
    ("libavcodec/aac/aacdec_usac_mps212.c",
     "int16_t tmp_idx_data[MPS_MAX_PARAM_SETS][MPS_MAX_PARAM_BANDS] = { 0 };",
     "int16_t tmp_idx_data[MPS_MAX_PARAM_SETS][MPS_MAX_PARAM_BANDS] = { { 0 } }; " + MARK),
    ("libavcodec/aac/aacdec_usac.c",
     "uint8_t layout_map[MAX_ELEM_ID*4][3] = { 0 };",
     "uint8_t layout_map[MAX_ELEM_ID*4][3] = { { 0 } }; " + MARK),
    ("libavcodec/vlc.c",
     "VLC_MULTI_ELEM info = { 0 };",
     "VLC_MULTI_ELEM info = { { 0 } }; " + MARK),
]

src = pathlib.Path(sys.argv[1])
n = 0
for rel, old, new in FIXES:
    p = src / rel
    if not p.is_file():
        continue
    t = p.read_text()
    if MARK in t and old not in t:
        continue
    if old not in t:
        if new.split(MARK)[0].rstrip() in t or MARK in t:
            continue
        sys.exit("slimff_patch_braces: missing: %s :: %s" % (rel, old))
    p.write_text(t.replace(old, new, 1))
    n += 1
print("slimff_patch_braces: patched %d site(s) in %s" % (n, src))
