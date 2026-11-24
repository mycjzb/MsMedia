#!/usr/bin/env python3
# 独立解析渐进 MP4/MOV 的 sample 表与 fMP4 moof/trun，对照 ms_ts_inspect 的轨道/配置/AU。
# 不入库、不链产品库。时间戳用媒体解码/合成时间，不把 elst 映射进 PTS。
# by: Long <mycjzb@gmail.com>
"""Progressive MP4/MOV and fMP4 AU crosscheck. Not a product demux."""

from __future__ import annotations

import hashlib
import os
import re
import struct
import subprocess
import sys
from pathlib import Path


def be16(b, o=0):
    return struct.unpack(">H", b[o : o + 2])[0]


def be32(b, o=0):
    return struct.unpack(">I", b[o : o + 4])[0]


def be64(b, o=0):
    return struct.unpack(">Q", b[o : o + 8])[0]


def parse_hdr(buf, off):
    if off + 8 > len(buf):
        return None
    sz32, typ = struct.unpack(">I4s", buf[off : off + 8])
    hs = 8
    if sz32 == 1:
        if off + 16 > len(buf):
            return None
        tot = be64(buf, off + 8)
        hs = 16
    elif sz32 == 0:
        tot = len(buf) - off
    else:
        tot = sz32
    if tot < hs or off + tot > len(buf):
        if sz32 == 0:
            tot = len(buf) - off
        else:
            return None
    return typ, tot, hs


CONTAINERS = {
    b"moov", b"trak", b"edts", b"mdia", b"minf", b"dinf", b"stbl",
    b"mvex", b"moof", b"traf",
}

TFHD_BASE_DATA_OFFSET = 0x000001
TFHD_SAMPLE_DESC = 0x000002
TFHD_DEFAULT_DURATION = 0x000008
TFHD_DEFAULT_SIZE = 0x000010
TFHD_DEFAULT_FLAGS = 0x000020
TFHD_DURATION_IS_EMPTY = 0x000100
TFHD_DEFAULT_BASE_IS_MOOF = 0x020000
TRUN_DATA_OFFSET = 0x000001
TRUN_FIRST_SAMPLE_FLAGS = 0x000004
TRUN_SAMPLE_DURATION = 0x000100
TRUN_SAMPLE_SIZE = 0x000200
TRUN_SAMPLE_FLAGS = 0x000400
TRUN_SAMPLE_CTO = 0x000800
SAMPLE_NON_SYNC = 0x00010000


def u32_to_i32(raw):
    return raw - 0x100000000 if raw >= 0x80000000 else raw


def parse_trex_map(data, start, end):
    out = {}
    for _off, _tot, _hs, body in find_boxes(data, start, end, b"trex"):
        if len(body) < 24:
            continue
        tid = be32(body, 4)
        out[tid] = dict(
            desc=be32(body, 8),
            dur=be32(body, 12),
            size=be32(body, 16),
            flags=be32(body, 20),
        )
    return out


def parse_moof_samples(data, moof_off, moof_tot, moof_hs, meta, trex, next_dts):
    recs = []
    first_traf = True
    traf_data_end = moof_off
    end = moof_off + moof_tot
    for typ, toff, ttot, ths, _body in walk_children(data, moof_off + moof_hs, end):
        if typ != b"traf":
            continue
        tb = toff + ths
        te = toff + ttot
        kinds = {t for t, *_ in walk_children(data, tb, te)}
        if kinds & {b"senc", b"saiz", b"saio"}:
            first_traf = False
            continue
        tfhds = find_boxes(data, tb, te, b"tfhd")
        if not tfhds:
            first_traf = False
            continue
        body = tfhds[0][3]
        if len(body) < 8:
            first_traf = False
            continue
        flags = (body[1] << 16) | (body[2] << 8) | body[3]
        tid = be32(body, 4)
        trk = meta.get(tid)
        tx = trex.get(tid, dict(desc=1, dur=0, size=0, flags=0))
        def_dur = tx["dur"]
        def_size = tx["size"]
        def_flags = tx["flags"]
        desc = tx["desc"] or 1
        o = 8
        has_base = False
        base = 0
        if flags & TFHD_BASE_DATA_OFFSET:
            if o + 8 > len(body):
                first_traf = False
                continue
            base = be64(body, o)
            has_base = True
            o += 8
        if flags & TFHD_SAMPLE_DESC:
            if o + 4 > len(body):
                first_traf = False
                continue
            desc = be32(body, o)
            o += 4
        if flags & TFHD_DEFAULT_DURATION:
            if o + 4 > len(body):
                first_traf = False
                continue
            def_dur = be32(body, o)
            o += 4
        if flags & TFHD_DEFAULT_SIZE:
            if o + 4 > len(body):
                first_traf = False
                continue
            def_size = be32(body, o)
            o += 4
        if flags & TFHD_DEFAULT_FLAGS:
            if o + 4 > len(body):
                first_traf = False
                continue
            def_flags = be32(body, o)
            o += 4
        if trk is None or (flags & TFHD_DURATION_IS_EMPTY):
            first_traf = False
            continue
        if not has_base:
            if flags & TFHD_DEFAULT_BASE_IS_MOOF or first_traf:
                base = moof_off
            else:
                base = traf_data_end
        next_off = base
        tfdts = find_boxes(data, tb, te, b"tfdt")
        has_tfdt = False
        traf_dts = next_dts.get(tid, 0)
        if tfdts:
            fb = tfdts[0][3]
            if fb:
                ver = fb[0]
                if ver == 1 and len(fb) >= 12:
                    traf_dts = be64(fb, 4)
                    has_tfdt = True
                elif ver == 0 and len(fb) >= 8:
                    traf_dts = be32(fb, 4)
                    has_tfdt = True
        if has_tfdt:
            next_dts[tid] = traf_dts
        for _u, _v, _w, trun in find_boxes(data, tb, te, b"trun"):
            if len(trun) < 8:
                continue
            ver = trun[0]
            tflags = (trun[1] << 16) | (trun[2] << 8) | trun[3]
            count = be32(trun, 4)
            p = 8
            run_off = next_off
            if tflags & TRUN_DATA_OFFSET:
                if p + 4 > len(trun):
                    break
                run_off = (base + u32_to_i32(be32(trun, p))) & 0xFFFFFFFFFFFFFFFF
                p += 4
            first_flags = def_flags
            has_first = False
            if tflags & TRUN_FIRST_SAMPLE_FLAGS:
                if p + 4 > len(trun):
                    break
                first_flags = be32(trun, p)
                has_first = True
                p += 4
            dts = next_dts.get(tid, traf_dts)
            for i in range(count):
                dur = def_dur
                sz = def_size
                smp_flags = first_flags if (has_first and i == 0) else def_flags
                cto = 0
                if tflags & TRUN_SAMPLE_DURATION:
                    dur = be32(trun, p)
                    p += 4
                if tflags & TRUN_SAMPLE_SIZE:
                    sz = be32(trun, p)
                    p += 4
                if tflags & TRUN_SAMPLE_FLAGS:
                    smp_flags = be32(trun, p)
                    p += 4
                if tflags & TRUN_SAMPLE_CTO:
                    raw = be32(trun, p)
                    p += 4
                    cto = u32_to_i32(raw) if ver == 1 else raw
                if sz == 0:
                    break
                pts = dts + cto
                key = 1 if (trk["codec"] == "aac" or not (smp_flags & SAMPLE_NON_SYNC)) else 0
                recs.append(
                    dict(
                        i=len(recs),
                        tid=tid,
                        codec=trk["codec"],
                        sfmt=trk["sfmt"],
                        off=run_off,
                        size=sz,
                        dts=dts,
                        pts=pts,
                        key=key,
                        tb=trk["tb"],
                        cfg=trk["cfg"],
                    )
                )
                run_off += sz
                dts += dur
            next_off = run_off
            if run_off > traf_data_end:
                traf_data_end = run_off
            next_dts[tid] = dts
        first_traf = False
    recs.sort(key=lambda r: (r["off"], r["tid"]))
    return recs


def walk_children(buf, start, end):
    off = start
    while off + 8 <= end:
        h = parse_hdr(buf, off)
        if h is None:
            break
        typ, tot, hs = h
        yield typ, off, tot, hs, buf[off + hs : off + tot]
        off += tot


def find_boxes(buf, start, end, want):
    out = []
    for typ, off, tot, hs, body in walk_children(buf, start, end):
        if typ == want:
            out.append((off, tot, hs, body))
        if typ in CONTAINERS:
            out.extend(find_boxes(buf, off + hs, off + tot, want))
    return out


def parse_expandable(d):
    i = 0
    v = 0
    while True:
        if i >= len(d) or i >= 4:
            raise ValueError("exp")
        v = (v << 7) | (d[i] & 0x7F)
        i += 1
        if d[i - 1] & 0x80 == 0:
            return v, i


def walk_esds(d, acc):
    off = 0
    while off < len(d):
        tag = d[off]
        ln, hdr = parse_expandable(d[off + 1 :])
        body = d[off + 1 + hdr : off + 1 + hdr + ln]
        if tag == 0x03:
            flags = body[2]
            b = 3
            if flags & 0x80:
                b += 2
            if flags & 0x40:
                b += 1 + body[b]
            if flags & 0x20:
                b += 2
            walk_esds(body[b:], acc)
        elif tag == 0x04:
            acc["oti"] = body[0]
            walk_esds(body[13:], acc)
        elif tag == 0x05:
            acc["asc"] = body
        off += 1 + hdr + ln


def parse_stsd(body):
    if len(body) < 8:
        return None
    count = be32(body, 4)
    off = 8
    for _ in range(count):
        if off + 8 > len(body):
            return None
        sz = be32(body, off)
        typ = body[off + 4 : off + 8]
        ent = body[off + 8 : off + sz]
        off += sz
        if typ == b"avc1" and len(ent) >= 78:
            inner = ent[78:]
            o = 0
            while o + 8 <= len(inner):
                s = be32(inner, o)
                t = inner[o + 4 : o + 8]
                if t == b"avcC":
                    avcc = inner[o + 8 : o + s]
                    ls = (avcc[4] & 3) + 1 if len(avcc) >= 5 else 4
                    return ("h264", "avcc", avcc, ls)
                o += s if s >= 8 else 8
        if typ == b"mp4a" and len(ent) >= 28:
            ver = be16(ent, 8)
            pre = 28 if ver == 0 else 44 if ver == 1 else 64 if ver == 2 else 28
            inner = ent[pre:]
            def look_esds(buf):
                o = 0
                while o + 8 <= len(buf):
                    s = be32(buf, o)
                    t = buf[o + 4 : o + 8]
                    if s < 8 or o + s > len(buf):
                        break
                    if t == b"esds":
                        acc = {}
                        walk_esds(buf[o + 8 + 4 : o + s], acc)
                        return ("aac", "aac_raw", acc.get("asc", b""), 0)
                    if t == b"wave":
                        got = look_esds(buf[o + 8 : o + s])
                        if got:
                            return got
                    o += s
                return None
            got = look_esds(inner)
            if got:
                return got
    return None


def parse_full_table(body, rec_size, signed_last=False, ver_signed=False):
    if len(body) < 8:
        return []
    ver = body[0]
    count = be32(body, 4)
    out = []
    o = 8
    for _ in range(count):
        if rec_size == 8 and ver_signed:
            c = be32(body, o)
            raw = be32(body, o + 4)
            if ver == 1:
                off = raw - 0x100000000 if raw >= 0x80000000 else raw
            else:
                off = raw
            out.append((c, off))
            o += 8
        elif rec_size == 8:
            out.append((be32(body, o), be32(body, o + 4)))
            o += 8
        elif rec_size == 12:
            out.append((be32(body, o), be32(body, o + 4), be32(body, o + 8)))
            o += 12
        elif rec_size == 4:
            out.append(be32(body, o))
            o += 4
        else:
            break
    return out


def extract(path: Path):
    data = path.read_bytes()
    moov = None
    mdats = []
    saw_ftyp = False
    saw_moof = saw_mvex = saw_styp = False
    major = b""
    off = 0
    first_mdat_payload = None
    moov_end = None
    while off + 8 <= len(data):
        h = parse_hdr(data, off)
        if h is None:
            break
        typ, tot, hs = h
        if typ == b"ftyp":
            saw_ftyp = True
            if tot >= hs + 4:
                major = data[off + hs : off + hs + 4]
        if typ == b"styp":
            saw_styp = True
        if typ == b"moof":
            saw_moof = True
        if typ == b"moov":
            moov = (off, tot, hs)
            moov_end = off + tot
            # mvex inside?
            for t, *_ in walk_children(data, off + hs, off + tot):
                if t == b"mvex":
                    saw_mvex = True
        if typ == b"mdat":
            mdats.append((off + hs, off + tot))
            if first_mdat_payload is None:
                first_mdat_payload = off + hs
        off += tot if tot else len(data) - off

    frag = saw_moof or saw_mvex or saw_styp
    if major == b"qt  ":
        fmt = "mov"
    elif saw_ftyp:
        fmt = "fmp4" if frag else "mp4"
    else:
        fmt = "mov"
    if moov is None:
        return {
            "format": fmt,
            "frag": frag,
            "need_seek": False,
            "tracks": [],
            "aus": [],
            "configs": [],
        }

    tracks = []
    meta = {}
    moov_off, moov_tot, moov_hs = moov
    trex = parse_trex_map(data, moov_off + moov_hs, moov_off + moov_tot)
    for typ, toff, ttot, ths, _ in walk_children(data, moov_off + moov_hs, moov_off + moov_tot):
        if typ != b"trak":
            continue
        def one(want):
            found = find_boxes(data, toff + ths, toff + ttot, want)
            return found[0][3] if found else None

        tkhd = one(b"tkhd")
        mdhd = one(b"mdhd")
        hdlr = one(b"hdlr")
        stsd = one(b"stsd")
        stts = one(b"stts")
        stsc = one(b"stsc")
        stsz = one(b"stsz")
        stco = one(b"stco")
        co64 = one(b"co64")
        stss = one(b"stss")
        ctts = one(b"ctts")
        if not (tkhd and mdhd and hdlr and stsd):
            continue
        tid0 = be32(tkhd, 20 if tkhd[0] == 1 else 12)
        tscale0 = be32(mdhd, 20 if mdhd[0] == 1 else 12)
        desc0 = parse_stsd(stsd)
        if desc0 is None:
            continue
        codec0, sfmt0, cfg0, _nalsz0 = desc0
        meta[tid0] = dict(tid=tid0, tb=tscale0, codec=codec0, sfmt=sfmt0, cfg=cfg0)
        if frag:
            continue
        if not (stts and stsc and stsz and (stco or co64)):
            continue
        tid = be32(tkhd, 20 if tkhd[0] == 1 else 12)
        tscale = be32(mdhd, 20 if mdhd[0] == 1 else 12)
        desc = parse_stsd(stsd)
        if desc is None:
            continue
        codec, sfmt, cfg, nalsz = desc
        if stts[0] != 0 or stsc[0] != 0 or stsz[0] != 0:
            continue
        stts_e = parse_full_table(stts, 8)
        stsc_e = parse_full_table(stsc, 12)
        default = be32(stsz, 4)
        scount = be32(stsz, 8)
        sizes = [default] * scount if default else [be32(stsz, 12 + 4 * i) for i in range(scount)]
        if co64:
            cn = be32(co64, 4)
            coff = [be64(co64, 8 + 8 * i) for i in range(cn)]
        else:
            cn = be32(stco, 4)
            coff = [be32(stco, 8 + 4 * i) for i in range(cn)]
        sync = None
        if stss is not None:
            nss = be32(stss, 4)
            sync = set(be32(stss, 8 + 4 * i) for i in range(nss))
        ctts_e = parse_full_table(ctts, 8, ver_signed=True) if ctts is not None else []
        # expand deltas / ctts
        deltas = []
        for c, dlt in stts_e:
            deltas.extend([dlt] * c)
        ctoffs = []
        for c, offv in ctts_e:
            ctoffs.extend([offv] * c)
        if len(deltas) != scount or sizes and len(sizes) != scount:
            continue
        if ctoffs and len(ctoffs) != scount:
            continue
        # build sample offsets
        # stsc: (first_chunk, spc, desc)
        offs = []
        si = 0
        for ci, ch_off in enumerate(coff):
            spc = 0
            for fc, sp, _dc in stsc_e:
                if ci + 1 >= fc:
                    spc = sp
                else:
                    break
            cur = ch_off
            for _k in range(spc):
                if si >= scount:
                    break
                offs.append(cur)
                cur += sizes[si]
                si += 1
        dts = 0
        recs = []
        for i in range(scount):
            pts = dts + (ctoffs[i] if ctoffs else 0)
            key = 1 if (codec == "aac" or sync is None or (i + 1) in sync) else 0
            recs.append(
                dict(
                    i=i,
                    tid=tid,
                    codec=codec,
                    sfmt=sfmt,
                    off=offs[i],
                    size=sizes[i],
                    dts=dts,
                    pts=pts,
                    key=key,
                    tb=tscale,
                    cfg=cfg,
                )
            )
            dts += deltas[i]
        tracks.append(recs)

    need_seek = False
    if first_mdat_payload is not None and moov_end is not None and moov_off is not None:
        # moov starts after mdat payload began
        if moov_off > first_mdat_payload:
            need_seek = True

    aus = []
    configs = []
    seen_tid = set()
    flat = []
    if frag:
        next_dts = {tid: 0 for tid in meta}
        off = 0
        while off + 8 <= len(data):
            h = parse_hdr(data, off)
            if h is None:
                break
            typ, tot, hs = h
            if typ == b"moof":
                flat.extend(
                    parse_moof_samples(data, off, tot, hs, meta, trex, next_dts)
                )
            off += tot if tot else len(data) - off
        for tid, trk in meta.items():
            configs.append(
                dict(
                    tid=trk["tid"],
                    codec=trk["codec"],
                    sfmt=trk["sfmt"],
                    cfg=trk["cfg"],
                    sha=hashlib.sha256(trk["cfg"]).hexdigest(),
                    tb=trk["tb"],
                )
            )
    else:
        for recs in tracks:
            if recs:
                flat.extend(recs)
        flat.sort(key=lambda r: (r["off"], r["tid"]))
        # config first per track in announce order: track id appearance in moov (already)
        for recs in tracks:
            if not recs:
                continue
            r0 = recs[0]
            if r0["tid"] not in seen_tid:
                seen_tid.add(r0["tid"])
                configs.append(
                    dict(
                        tid=r0["tid"],
                        codec=r0["codec"],
                        sfmt=r0["sfmt"],
                        cfg=r0["cfg"],
                        sha=hashlib.sha256(r0["cfg"]).hexdigest(),
                        tb=r0["tb"],
                    )
                )
    # 独立表始终按文件内偏移抽出 AU，供与会话对照。
    # moov 在 mdat 之后只是布局标记，不再让对照表空置。
    for r in flat:
        payload = data[r["off"] : r["off"] + r["size"]]
        if len(payload) != r["size"]:
            continue  # truncated
        aus.append(
            dict(
                tid=r["tid"],
                codec=r["codec"],
                sfmt=r["sfmt"],
                pts=r["pts"],
                dts=r["dts"],
                key=r["key"],
                size=r["size"],
                sha=hashlib.sha256(payload).hexdigest(),
                tb=r["tb"],
            )
        )
    return {
        "format": fmt,
        "frag": frag,
        "need_seek": need_seek,
        "tracks": configs,
        "aus": aus,
        "configs": configs,
    }


def run_inspect(inspect, path, chunk, extra=None):
    cmd = [str(inspect), "--chunk", str(chunk)]
    if extra:
        cmd.extend(extra)
    cmd.append(str(path))
    p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    return p.returncode, p.stdout, p.stderr


def parse_inspect(text):
    fmt = None
    tracks = []
    configs = []
    aus = []
    unsup = 0
    for line in text.splitlines():
        if line.startswith("format="):
            if line.startswith("format=mpeg_ts"):
                fmt = "mpeg_ts"
            else:
                fmt = line.split("=", 1)[1].strip()
        elif line.startswith("track "):
            m = re.search(r"id=(\d+).*codec=(\S+).*sample_fmt=(\S+)", line)
            if m:
                tracks.append((int(m.group(1)), m.group(2), m.group(3)))
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
                pts = m.group(3)
                dts = m.group(4)
                if pts.startswith("na:"):
                    pts = None
                else:
                    pts = int(pts)
                if dts.startswith("na:"):
                    dts = None
                else:
                    dts = int(dts)
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
    return dict(fmt=fmt, tracks=tracks, configs=configs, aus=aus, unsup=unsup)


def compare(name, ref, got, expect_seek=False):
    errors = []
    if got["fmt"] != ref["format"]:
        errors.append(f"format {got['fmt']} != {ref['format']}")
    if expect_seek:
        if got["unsup"] < 1:
            errors.append("expected unsupported for lookback overflow / seek")
        if got["aus"]:
            errors.append(f"unexpected {len(got['aus'])} au on seek-needed file")
        # configs may still appear
        if len(got["configs"]) != len(ref["configs"]):
            errors.append(f"config count {len(got['configs'])} != {len(ref['configs'])}")
        return errors
    if len(got["configs"]) != len(ref["configs"]):
        errors.append(f"config count {len(got['configs'])} != {len(ref['configs'])}")
    else:
        for a, b in zip(ref["configs"], got["configs"]):
            if a["sha"] != b["sha"] or a["sfmt"] != b["sfmt"] or a["tid"] != b["tid"]:
                errors.append(f"config mismatch tid={a['tid']} {a['sfmt']}/{a['sha'][:8]} vs {b}")
            if a["tb"] != b["tb"]:
                errors.append(f"config tb {a['tb']} != {b['tb']}")
    if len(got["aus"]) != len(ref["aus"]):
        errors.append(f"au count {len(got['aus'])} != {len(ref['aus'])}")
        return errors
    for i, (a, b) in enumerate(zip(ref["aus"], got["aus"])):
        cmap = {"h264": "h264", "aac": "aac"}
        if a["tid"] != b["tid"] or cmap.get(a["codec"], a["codec"]) != b["codec"]:
            errors.append(f"au[{i}] track/codec {a['tid']}/{a['codec']} vs {b['tid']}/{b['codec']}")
        if a["sha"] != b["sha"] or a["size"] != b["size"]:
            errors.append(f"au[{i}] payload {a['size']}/{a['sha'][:8]} vs {b['size']}/{b['sha'][:8]}")
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
        "-of",
        "json",
        str(path),
    ]
    p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    if p.returncode != 0:
        return None
    import json

    try:
        data = json.loads(p.stdout)
    except json.JSONDecodeError:
        return None
    return data.get("packets") or []


def note_elst(ref, packets):
    if not packets or not ref["aus"]:
        return "no ffprobe packets"
    # map first video/audio
    # ffprobe decode order should match file order for these muxes
    n = min(len(packets), len(ref["aus"]))
    deltas = []
    for i in range(n):
        pk = packets[i]
        au = ref["aus"][i]
        try:
            ppts = int(pk.get("pts", pk.get("pts_time", 0)))
        except (TypeError, ValueError):
            continue
        # pts may be in packet time_base; ffprobe json pts is already in stream tb usually
        if "pts" in pk:
            deltas.append(int(pk["pts"]) - au["pts"])
    if not deltas:
        return "ffprobe pts not comparable"
    if all(d == deltas[0] for d in deltas):
        if deltas[0] == 0:
            return "ffprobe pts/dts match media timeline (no elst shift)"
        return f"ffprobe pts constant offset {deltas[0]} vs media timeline (likely elst; not applied)"
    # maybe dts-only compare
    return f"ffprobe pts offsets not constant ({min(deltas)}..{max(deltas)}); compare uses independent tables"


def main():
    inspect = Path(os.environ.get("INSPECT", "build/debug/ms_ts_inspect"))
    outdir = Path(os.environ.get("OUTDIR", "build/dev-samples/bmff-au"))
    files = sys.argv[1:]
    if not files:
        print("usage: dev_bmff_au_crosscheck.py file.mp4 ...", file=sys.stderr)
        return 2
    chunks = [int(x) for x in os.environ.get("CHUNKS", "1,17,512,0").split(",") if x]
    failed = 0
    for f in files:
        path = Path(f)
        ref = extract(path)
        # 默认小文件 moov-at-end 走有界回看，应对照 AU。
        # 仅当文件名标明回看溢出或显式 EXPECT_SEEK=1 时才期望无 AU。
        expect_seek = (
            "lookback_overflow" in path.name
            or os.environ.get("EXPECT_SEEK") == "1"
        )
        print(f"== {path.name} format={ref['format']} aus={len(ref['aus'])} "
              f"cfg={len(ref['configs'])} layout_moov_after={int(ref['need_seek'])}")
        for ch in chunks:
            rc, out, err = run_inspect(inspect, path, ch)
            got = parse_inspect(out)
            errors = compare(path.name, ref, got, expect_seek=expect_seek)
            if rc != 0 and not (expect_seek and got["unsup"]):
                # inspect may still exit 0 on unsupported event
                if rc != 0:
                    errors.append(f"inspect exit {rc}: {err.strip()[:200]}")
            status = "OK" if not errors else "FAIL"
            print(f"  chunk={ch} {status} inspect_fmt={got['fmt']} au={len(got['aus'])} unsup={got['unsup']}")
            for e in errors:
                print(f"    {e}")
                failed += 1
        if "moov_at_end" in path.name and not expect_seek:
            rc, out, err = run_inspect(inspect, path, 0,
                                       extra=["--source", "--max-lookback", "16"])
            got = parse_inspect(out)
            errors = compare(path.name + " source+lookback16", ref, got,
                             expect_seek=False)
            if rc != 0:
                errors.append(f"source inspect exit {rc}: {err.strip()[:200]}")
            status = "OK" if not errors else "FAIL"
            print(f"  source+max_lookback=16 {status} inspect_fmt={got['fmt']} "
                  f"au={len(got['aus'])} unsup={got['unsup']}")
            for e in errors:
                print(f"    {e}")
                failed += 1
        ffprobe = os.environ.get("FFPROBE", "/usr/bin/ffprobe")
        if not expect_seek and ref["aus"] and Path(ffprobe).exists():
            pk = ffprobe_packets(ffprobe, path)
            if pk is not None:
                print(f"  ffprobe: {note_elst(ref, pk)} packets={len(pk)}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
