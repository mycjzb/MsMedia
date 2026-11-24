#!/usr/bin/env python3
# 独立解析经典 FLV tag（不链产品库），对照 ms_ts_inspect 的配置/AU，并与 ffprobe 的
# PTS/DTS/关键帧/包数交叉核对。样本不入库。不做 AMF、不做 Enhanced FLV。
# by: Long <mycjzb@gmail.com>
"""Classic FLV AU crosscheck. Independent tag walk; not a product demux."""

from __future__ import annotations

import hashlib
import json
import os
import re
import struct
import subprocess
import sys
from pathlib import Path


def be24(b, o=0):
    return (b[o] << 16) | (b[o + 1] << 8) | b[o + 2]


def be32(b, o=0):
    return struct.unpack(">I", b[o : o + 4])[0]


def si24(b, o=0):
    v = be24(b, o)
    if v & 0x800000:
        v -= 0x1000000
    return v


def extract_flv(path: Path):
    data = path.read_bytes()
    if len(data) < 9 or data[0:3] != b"FLV":
        raise ValueError(f"{path}: not FLV")
    if data[3] != 1:
        raise ValueError(f"{path}: version {data[3]} not classic 1")
    if data[4] & ~0x05:
        raise ValueError(f"{path}: reserved header flags 0x{data[4]:02x}")
    off = be32(data, 5)
    if off < 9 or off > len(data):
        raise ValueError(f"{path}: bad DataOffset {off}")
    pos = off
    if pos + 4 > len(data):
        raise ValueError(f"{path}: truncated prevTagSize0")
    if be32(data, pos) != 0:
        raise ValueError(f"{path}: prevTagSize0 != 0")
    pos += 4

    configs = []
    aus = []
    have_v = have_a = False
    cfg_v = cfg_a = None
    prev_expect = 0
    while pos + 11 <= len(data):
        typ = data[pos]
        size = be24(data, pos + 1)
        ts = be24(data, pos + 4) | (data[pos + 7] << 24)
        sid = be24(data, pos + 8)
        if sid != 0:
            raise ValueError(f"{path}: StreamID {sid} != 0")
        body_at = pos + 11
        body_end = body_at + size
        if body_end + 4 > len(data):
            # 残片：独立表不收不完整 tag
            break
        prev = be32(data, body_end)
        if prev != 11 + size:
            raise ValueError(f"{path}: prevTagSize {prev} != {11 + size}")
        body = data[body_at:body_end]
        if typ == 18 or typ not in (8, 9, 18):
            pos = body_end + 4
            prev_expect = 11 + size
            continue
        if typ == 9:
            if not body:
                pos = body_end + 4
                continue
            if body[0] & 0x80:
                raise ValueError(f"{path}: Enhanced FLV IsExHeader")
            frame = body[0] >> 4
            codec = body[0] & 0x0F
            if codec != 7:
                raise ValueError(f"{path}: video codec {codec} not AVC")
            if len(body) < 5:
                raise ValueError(f"{path}: short AVC tag")
            ptype = body[1]
            cto = si24(body, 2)
            pay = body[5:]
            if ptype == 0:
                cfg_v = pay
                have_v = True
                configs.append(
                    dict(
                        tid=1,
                        size=len(pay),
                        sha=hashlib.sha256(pay).hexdigest(),
                        sfmt="avcc",
                        tb=1000,
                    )
                )
            elif ptype == 1 and pay and have_v:
                pts = ts + cto
                if pts < 0:
                    raise ValueError(f"{path}: negative pts {pts}")
                aus.append(
                    dict(
                        tid=1,
                        codec="h264",
                        pts=pts,
                        dts=ts,
                        key=1 if frame == 1 else 0,
                        size=len(pay),
                        sha=hashlib.sha256(pay).hexdigest(),
                        sfmt="avcc",
                        tb=1000,
                    )
                )
            elif ptype == 2:
                pass
            elif ptype not in (0, 1, 2):
                raise ValueError(f"{path}: unknown AVC packet type {ptype}")
        elif typ == 8:
            if not body:
                pos = body_end + 4
                continue
            fmt = body[0] >> 4
            if fmt != 10:
                raise ValueError(f"{path}: audio format {fmt} not AAC")
            if len(body) < 2:
                raise ValueError(f"{path}: short AAC tag")
            ptype = body[1]
            pay = body[2:]
            if ptype == 0:
                cfg_a = pay
                have_a = True
                configs.append(
                    dict(
                        tid=2,
                        size=len(pay),
                        sha=hashlib.sha256(pay).hexdigest(),
                        sfmt="aac_raw",
                        tb=1000,
                    )
                )
            elif ptype == 1 and pay and have_a:
                aus.append(
                    dict(
                        tid=2,
                        codec="aac",
                        pts=ts,
                        dts=ts,
                        key=1,
                        size=len(pay),
                        sha=hashlib.sha256(pay).hexdigest(),
                        sfmt="aac_raw",
                        tb=1000,
                    )
                )
            elif ptype not in (0, 1):
                raise ValueError(f"{path}: unknown AAC packet type {ptype}")
        pos = body_end + 4
        prev_expect = 11 + size
        (prev_expect)  # keep last prev for readability
    return dict(format="flv", configs=configs, aus=aus, cfg_v=cfg_v, cfg_a=cfg_a)


def run_inspect(inspect, path, chunk):
    cmd = [str(inspect), "--chunk", str(chunk), str(path)]
    p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    return p.returncode, p.stdout, p.stderr


def parse_inspect(text):
    fmt = None
    configs = []
    aus = []
    unsup = 0
    for line in text.splitlines():
        if line.startswith("format="):
            fmt = line.split("=", 1)[1].strip()
        elif line.startswith("config "):
            m = re.search(
                r"track=(\d+) size=(\d+) sha256=([0-9a-f]+) sample_fmt=(\S+) tb=(\d+)/(\d+)",
                line,
            )
            if m:
                configs.append(
                    dict(
                        tid=int(m.group(1)),
                        size=int(m.group(2)),
                        sha=m.group(3),
                        sfmt=m.group(4),
                        tb=int(m.group(6)),
                    )
                )
        elif line.startswith("au "):
            m = re.search(
                r"track=(\d+) kind=\S+ codec=(\S+) pts=(\S+) dts=(\S+) key=(\d+) .* size=(\d+) sha256=([0-9a-f]+).* sample_fmt=(\S+) tb=(\d+)/(\d+)",
                line,
            )
            if m:
                pts, dts = m.group(3), m.group(4)
                pts = None if pts.startswith("na:") else int(pts)
                dts = None if dts.startswith("na:") else int(dts)
                aus.append(
                    dict(
                        tid=int(m.group(1)),
                        codec=m.group(2),
                        pts=pts,
                        dts=dts,
                        key=int(m.group(5)),
                        size=int(m.group(6)),
                        sha=m.group(7),
                        sfmt=m.group(8),
                        tb=int(m.group(10)),
                    )
                )
        elif line.startswith("unsupported"):
            unsup += 1
    return dict(fmt=fmt, configs=configs, aus=aus, unsup=unsup)


def compare(ref, got):
    errors = []
    if got["fmt"] != ref["format"]:
        errors.append(f"format {got['fmt']} != {ref['format']}")
    if got["unsup"]:
        errors.append(f"unexpected unsupported x{got['unsup']}")
    if len(got["configs"]) != len(ref["configs"]):
        errors.append(f"config count {len(got['configs'])} != {len(ref['configs'])}")
    else:
        for a, b in zip(ref["configs"], got["configs"]):
            if a["sha"] != b["sha"] or a["sfmt"] != b["sfmt"] or a["tid"] != b["tid"]:
                errors.append(
                    f"config mismatch tid={a['tid']} {a['sfmt']}/{a['sha'][:8]} vs {b}"
                )
            if a["tb"] != b["tb"]:
                errors.append(f"config tb {a['tb']} != {b['tb']}")
    if len(got["aus"]) != len(ref["aus"]):
        errors.append(f"au count {len(got['aus'])} != {len(ref['aus'])}")
        return errors
    for i, (a, b) in enumerate(zip(ref["aus"], got["aus"])):
        if a["tid"] != b["tid"] or a["codec"] != b["codec"]:
            errors.append(
                f"au[{i}] track/codec {a['tid']}/{a['codec']} vs {b['tid']}/{b['codec']}"
            )
        if a["sha"] != b["sha"] or a["size"] != b["size"]:
            errors.append(
                f"au[{i}] payload {a['size']}/{a['sha'][:8]} vs {b['size']}/{b['sha'][:8]}"
            )
        if a["pts"] != b["pts"] or a["dts"] != b["dts"] or a["key"] != b["key"]:
            errors.append(
                f"au[{i}] ts/key pts={a['pts']}/{b['pts']} dts={a['dts']}/{b['dts']} key={a['key']}/{b['key']}"
            )
        if a["sfmt"] != b["sfmt"] or a["tb"] != b["tb"]:
            errors.append(f"au[{i}] fmt/tb {a['sfmt']}/{a['tb']} vs {b['sfmt']}/{b['tb']}")
    return errors


def ffprobe_packets(ffprobe, path):
    cmd = [
        str(ffprobe),
        "-hide_banner",
        "-show_packets",
        "-show_entries",
        "packet=codec_type,pts,dts,flags,size,duration,stream_index",
        "-of",
        "json",
        str(path),
    ]
    p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    if p.returncode != 0:
        return None
    try:
        data = json.loads(p.stdout)
    except json.JSONDecodeError:
        return None
    return data.get("packets") or []


def note_ffprobe(ref, packets):
    # 只对照媒体包（不含 avcC/ASC 配置 tag），按解码/存储顺序。
    media = [p for p in packets if p.get("codec_type") in ("video", "audio")]
    if len(media) != len(ref["aus"]):
        return f"ffprobe media packets {len(media)} != tag AU {len(ref['aus'])}"
    dpts, ddts, dkey = [], [], 0
    for pk, au in zip(media, ref["aus"]):
        kind = "h264" if pk.get("codec_type") == "video" else "aac"
        if kind != au["codec"]:
            return f"ffprobe order/codec {kind} vs {au['codec']}"
        try:
            ppts = int(pk["pts"]) if "pts" in pk and pk["pts"] is not None else None
            pdts = int(pk["dts"]) if "dts" in pk and pk["dts"] is not None else None
        except (TypeError, ValueError):
            return "ffprobe pts/dts not integers"
        if ppts is not None:
            dpts.append(ppts - au["pts"])
        if pdts is not None:
            ddts.append(pdts - au["dts"])
        flags = str(pk.get("flags") or "")
        fkey = 1 if "K" in flags else 0
        if au["codec"] == "h264" and fkey != au["key"]:
            dkey += 1
    msg = []
    if dpts and all(d == dpts[0] for d in dpts):
        msg.append(
            "pts match media timeline"
            if dpts[0] == 0
            else f"pts constant offset {dpts[0]}"
        )
    elif dpts:
        msg.append(f"pts offsets {min(dpts)}..{max(dpts)}")
    if ddts and all(d == ddts[0] for d in ddts):
        msg.append(
            "dts match media timeline"
            if ddts[0] == 0
            else f"dts constant offset {ddts[0]}"
        )
    elif ddts:
        msg.append(f"dts offsets {min(ddts)}..{max(ddts)}")
    if dkey:
        msg.append(f"keyframe mismatches {dkey}")
    else:
        msg.append("keyframes match")
    return "ffprobe: " + "; ".join(msg) + f" packets={len(media)}"


def main():
    inspect = Path(os.environ.get("INSPECT", "build/debug/ms_ts_inspect"))
    files = sys.argv[1:]
    if not files:
        print("usage: dev_flv_au_crosscheck.py file.flv ...", file=sys.stderr)
        return 2
    chunks = [int(x) for x in os.environ.get("CHUNKS", "1,17,512,0").split(",") if x]
    failed = 0
    ffprobe = os.environ.get("FFPROBE", "/usr/bin/ffprobe")
    for f in files:
        path = Path(f)
        ref = extract_flv(path)
        print(
            f"== {path.name} format={ref['format']} aus={len(ref['aus'])} "
            f"cfg={len(ref['configs'])}"
        )
        for ch in chunks:
            rc, out, err = run_inspect(inspect, path, ch)
            got = parse_inspect(out)
            errors = compare(ref, got)
            if rc != 0:
                errors.append(f"inspect exit {rc}: {err.strip()[:200]}")
            status = "OK" if not errors else "FAIL"
            print(
                f"  chunk={ch} {status} inspect_fmt={got['fmt']} "
                f"au={len(got['aus'])} unsup={got['unsup']}"
            )
            for e in errors:
                print(f"    {e}")
                failed += 1
        if ref["aus"] and Path(ffprobe).exists():
            pk = ffprobe_packets(ffprobe, path)
            if pk is not None:
                note = note_ffprobe(ref, pk)
                print(f"  {note}")
                if note.startswith("ffprobe media packets") or "mismatches" in note or "order/codec" in note:
                    # 包数/顺序/关键帧应对上；常量偏移只报告不算失败
                    if "packets" in note and " != " in note:
                        failed += 1
                    if "keyframe mismatches" in note or "order/codec" in note:
                        failed += 1
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
