#!/usr/bin/env python3
# 对照编译出的 .o 与链接 map，列出被 --gc-sections 整份丢掉的目标文件。
# by: Long <mycjzb@gmail.com>
import os, re, sys
map_path, objdir = sys.argv[1], sys.argv[2]
text = open(map_path, errors="replace").read()
live = set()
for m in re.finditer(r"/([^/\s]+\.o)\s*:", text):
    live.add(m.group(1))
for m in re.finditer(r"lto\.([^/\s]+\.o)", text):
    live.add(m.group(1))
drop = []
for o in sorted(os.listdir(objdir)):
    if not o.endswith(".o"):
        continue
    if o in live or any(o.endswith(x) or x.endswith(o) for x in live):
        continue
    drop.append(o)
print(",".join(drop) if drop else "(none)")
