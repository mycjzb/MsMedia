#!/usr/bin/env python3
# lld -Map 体积归属：把 strip 后仍存在的分配段字节按输入归到 .a 库 / 适配器 .o / 链接器内部合成段。
# 用法：single_map_anatomy.py <map.txt> <stripped.so>
# by: Long <mycjzb@gmail.com>
import re, subprocess, sys, collections

map_path, so_path = sys.argv[1], sys.argv[2]
secs = subprocess.run(["llvm-readelf", "-S", "-W", so_path], capture_output=True, text=True).stdout
kept = {}
for line in secs.splitlines():
    m = re.match(r"\s*\[\s*\d+\]\s+(\S+)\s+\S+\s+[0-9a-f]+\s+[0-9a-f]+\s+([0-9a-f]+)\s+\S+\s+(\S*)", line)
    if m and m.group(1) != "NULL":
        kept[m.group(1)] = int(m.group(2), 16)
file_size = sum(1 for _ in [0]) and None
import os
file_size = os.path.getsize(so_path)

def lib_of(src):
    m = re.match(r"(.*/)?([^/]+\.a)\(([^)]+)\)", src)
    if m:
        return m.group(2), m.group(2) + ":" + m.group(3)
    if src.startswith("<internal>"):
        return "<linker-synth>", src
    m = re.match(r"(.*/)?([^/]+\.o)$", src)
    if m:
        base = m.group(2)
        if "crt" in base:
            return "crt(ndk)", base
        return "adapter(.o 自研/适配器)", base
    return "other", src

by_lib = collections.Counter()
by_obj = collections.Counter()
by_sec_lib = collections.defaultdict(collections.Counter)
internal = collections.Counter()
cur = None
for line in open(map_path, errors="replace"):
    parts = line.rstrip("\n")
    m = re.match(r"\s*([0-9a-f]+)\s+([0-9a-f]+)\s+([0-9a-f]+)\s+(\d+)\s(.*)$", parts)
    if not m:
        continue
    size = int(m.group(3), 16)
    rest = m.group(5)
    ind = len(rest) - len(rest.lstrip(" "))
    rest = rest.strip()
    if ind == 0:
        cur = rest
        continue
    if ind > 8 or cur not in kept:
        continue  # 符号行或未保留段
    if cur.startswith(".bss") or cur in (".tbss",):
        continue  # NOBITS 不占文件字节
    mm = re.match(r"(.*):\((.*)\)$", rest)
    src = mm.group(1) if mm else rest
    lib, obj = lib_of(src)
    by_lib[lib] += size
    by_obj[obj] += size
    by_sec_lib[cur][lib] += size
    if lib == "<linker-synth>":
        internal[(cur, mm.group(2) if mm else rest)] += size

total_attr = sum(by_lib.values())
print(f"stripped_file_bytes={file_size}")
print(f"attributed_alloc_bytes={total_attr}  (余下为 ELF 头/程序头/段对齐填充/.shstrtab: {file_size-total_attr})")
print("\n## 按库归属（字节，占 strip 后文件比例）")
for lib, v in by_lib.most_common():
    print(f"{lib:32s} {v:>10d}  {100.0*v/file_size:5.1f}%")
print("\n## 链接器合成段明细（.dynsym/.dynstr/重定位/合并字符串/.eh_frame 等）")
for (sec, name), v in internal.most_common(20):
    print(f"{sec:20s} {name:32s} {v:>9d}")
print("\n## 前 40 个最大输入目标文件")
for obj, v in by_obj.most_common(40):
    print(f"{v:>9d}  {obj}")
print("\n## 每个输出段内按库")
for sec in sorted(by_sec_lib, key=lambda s: -sum(by_sec_lib[s].values())):
    t = sum(by_sec_lib[sec].values())
    print(f"{sec} total={t}: " + ", ".join(f"{l}={v}" for l, v in by_sec_lib[sec].most_common(6)))

# ---------- 二次归属：把链接器合成段按来源摊回各库 ----------
# 1) 动态重定位：按 r_offset 落在哪个输入段归库（REL 8 字节 / RELA 24 字节）。
# 2) 合并字符串 .rodata.str*：按存活目标文件合并前字符串段大小比例摊分（估算，标注）。
# 3) .eh_frame_hdr：按各库 .eh_frame 比例摊分。
import bisect
unstripped = so_path.replace("libmsff.so", "libmsff.unstripped.so")
if os.path.exists(unstripped):
    ranges = []  # (start, end, lib, objkey)
    live_objs = collections.defaultdict(set)
    cur = None
    for line in open(map_path, errors="replace"):
        m = re.match(r"\s*([0-9a-f]+)\s+([0-9a-f]+)\s+([0-9a-f]+)\s+(\d+)\s(.*)$", line.rstrip("\n"))
        if not m:
            continue
        rest = m.group(5); ind = len(rest) - len(rest.lstrip(" ")); rest = rest.strip()
        if ind == 0:
            cur = rest; continue
        if ind > 8:
            continue
        mm = re.match(r"(.*):\((.*)\)$", rest)
        src = mm.group(1) if mm else rest
        lib, obj = lib_of(src)
        va, sz = int(m.group(1), 16), int(m.group(3), 16)
        if sz:
            ranges.append((va, va + sz, lib))
        am = re.match(r"(.*\.a)\(([^)]+)\)", src)
        if am:
            live_objs[am.group(1)].add(am.group(2))
    ranges.sort()
    starts = [r[0] for r in ranges]
    hdr = subprocess.run(["llvm-readelf", "-h", unstripped], capture_output=True, text=True).stdout
    is64 = "ELF64" in hdr
    ent = 24 if is64 else 8
    rel = subprocess.run(["llvm-readelf", "-r", "-W", unstripped], capture_output=True, text=True).stdout
    reloc_by_lib = collections.Counter()
    for line in rel.splitlines():
        m = re.match(r"\s*([0-9a-f]{8,16})\s+[0-9a-f]+\s+(R_\S+)", line)
        if not m or "JUMP_SLOT" in m.group(2):
            continue
        off = int(m.group(1), 16)
        i = bisect.bisect_right(starts, off) - 1
        lib = ranges[i][2] if i >= 0 and off < ranges[i][1] else "<linker-synth>"
        if lib == "<linker-synth>":
            lib = "<got/其他>"
        reloc_by_lib[lib] += ent
    # 合并前字符串
    str_by_lib = collections.Counter()
    for apath, members in live_objs.items():
        out = subprocess.run(["llvm-readelf", "-S", "-W", apath], capture_output=True, text=True).stdout
        member = None
        for line in out.splitlines():
            fm = re.match(r"File: .*\(([^)]+)\)", line)
            if fm:
                member = fm.group(1); continue
            if member in members:
                sm = re.match(r"\s*\[\s*\d+\]\s+(\.rodata\.str\S*)\s+\S+\s+[0-9a-f]+\s+[0-9a-f]+\s+([0-9a-f]+)", line)
                if sm:
                    str_by_lib[os.path.basename(apath)] += int(sm.group(2), 16)
    for o in os.listdir(os.path.join(os.path.dirname(map_path), "obj")):
        out = subprocess.run(["llvm-readelf", "-S", "-W", os.path.join(os.path.dirname(map_path), "obj", o)], capture_output=True, text=True).stdout
        for line in out.splitlines():
            sm = re.match(r"\s*\[\s*\d+\]\s+(\.rodata\.str\S*)\s+\S+\s+[0-9a-f]+\s+[0-9a-f]+\s+([0-9a-f]+)", line)
            if sm:
                str_by_lib["adapter(.o 自研/适配器)"] += int(sm.group(2), 16)
    merged = sum(v for (s, n), v in internal.items() if n.startswith(".rodata.str"))
    pre = sum(str_by_lib.values()) or 1
    adj = collections.Counter({k: v for k, v in by_lib.items() if k != "<linker-synth>"})
    synth_left = by_lib["<linker-synth>"]
    for k, v in str_by_lib.items():
        adj[k] += merged * v // pre
    synth_left -= merged
    relsec = sum(v for (s, n), v in internal.items() if s in (".rel.dyn", ".rela.dyn"))
    for k, v in reloc_by_lib.items():
        adj[k] += v
    synth_left -= relsec
    ehh = sum(v for (s, n), v in internal.items() if s == ".eh_frame_hdr")
    eh = by_sec_lib.get(".eh_frame", collections.Counter())
    eht = sum(eh.values()) or 1
    if ehh:
        for k, v in eh.items():
            adj[k] += ehh * v // eht
        synth_left -= ehh
    adj["<其余链接器段/头/对齐>"] = synth_left + (file_size - total_attr)
    print("\n## 二次归属后（合并字符串按合并前比例估算；重定位按 r_offset 精确；eh_frame_hdr 按比例）")
    print(f"merged_strings={merged} pre_merge_strings={pre} dyn_reloc_bytes={relsec} eh_frame_hdr={ehh} eh_frame={sum(eh.values())}")
    tot = 0
    for k, v in adj.most_common():
        tot += v
        print(f"{k:32s} {v:>10d}  {100.0*v/file_size:5.1f}%")
    print(f"{'合计':32s} {tot:>10d}  (文件 {file_size})")
    print("\n## 重定位按库（字节）: " + ", ".join(f"{k}={v}" for k, v in reloc_by_lib.most_common()))
    print("## 合并前字符串按库（字节）: " + ", ".join(f"{k}={v}" for k, v in str_by_lib.most_common()))
