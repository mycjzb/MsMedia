#!/usr/bin/env python3
# 从 DASH init+m4s 独立生成 trun/elst 对照表。仅开发样本，不入库、不是产品解析器。
# by: Long <mycjzb@gmail.com>
import struct
import sys
from pathlib import Path


def be32(b, o=0):
    return struct.unpack_from(">I", b, o)[0]


def be64(b, o=0):
    return struct.unpack_from(">Q", b, o)[0]


def boxes(data, off=0, end=None):
    if end is None:
        end = len(data)
    while off + 8 <= end:
        size = be32(data, off)
        typ = data[off + 4 : off + 8]
        if size == 1:
            if off + 16 > end:
                break
            size = be64(data, off + 8)
            hdr = 16
        elif size == 0:
            size = end - off
            hdr = 8
        else:
            hdr = 8
        if size < hdr or off + size > end:
            break
        yield typ, off, hdr, size
        off += size


CONTAINER = {
    b"moov",
    b"trak",
    b"mdia",
    b"minf",
    b"stbl",
    b"moof",
    b"traf",
    b"mvex",
    b"edts",
    b"dinf",
}


def walk(data, off=0, end=None):
    for typ, o, hdr, size in boxes(data, off, end):
        yield typ, o, hdr, size, data[o + hdr : o + size]
        if typ in CONTAINER:
            yield from walk(data, o + hdr, o + size)


def parse_full(pl):
    return pl[0], int.from_bytes(pl[1:4], "big"), pl[4:]


def parse_mdhd(pl):
    ver, _flags, rest = parse_full(pl)
    return be32(rest, 16) if ver == 1 else be32(rest, 8)


def parse_elst(pl):
    ver, _flags, rest = parse_full(pl)
    n = be32(rest, 0)
    p = 4
    ents = []
    for _ in range(n):
        if ver == 1:
            seg = be64(rest, p)
            media = struct.unpack_from(">q", rest, p + 8)[0]
            p += 20
        else:
            seg = be32(rest, p)
            media = struct.unpack_from(">i", rest, p + 4)[0]
            p += 12
        ents.append((seg, media))
    return ents


def parse_trex(pl):
    _ver, _flags, rest = parse_full(pl)
    return dict(
        tid=be32(rest, 0),
        desc=be32(rest, 4),
        dur=be32(rest, 8),
        size=be32(rest, 12),
        flags=be32(rest, 16),
    )


def parse_tfhd(pl):
    _ver, flags, rest = parse_full(pl)
    p = 4
    out = dict(tid=be32(rest, 0), flags=flags, base=None, desc=None, dur=None, size=None, sflags=None)
    if flags & 0x000001:
        out["base"] = struct.unpack_from(">Q", rest, p)[0]
        p += 8
    if flags & 0x000002:
        out["desc"] = be32(rest, p)
        p += 4
    if flags & 0x000008:
        out["dur"] = be32(rest, p)
        p += 4
    if flags & 0x000010:
        out["size"] = be32(rest, p)
        p += 4
    if flags & 0x000020:
        out["sflags"] = be32(rest, p)
        p += 4
    return out


def parse_tfdt(pl):
    ver, _flags, rest = parse_full(pl)
    return be64(rest, 0) if ver == 1 else be32(rest, 0)


def parse_trun(pl):
    ver, flags, rest = parse_full(pl)
    n = be32(rest, 0)
    p = 4
    data_offset = None
    first_flags = None
    if flags & 0x1:
        data_offset = struct.unpack_from(">i", rest, p)[0]
        p += 4
    if flags & 0x4:
        first_flags = be32(rest, p)
        p += 4
    samples = []
    for _i in range(n):
        dur = size = cto = sf = None
        if flags & 0x100:
            dur = be32(rest, p)
            p += 4
        if flags & 0x200:
            size = be32(rest, p)
            p += 4
        if flags & 0x400:
            sf = be32(rest, p)
            p += 4
        if flags & 0x800:
            fmt = ">i" if ver == 1 else ">I"
            cto = struct.unpack_from(fmt, rest, p)[0]
            p += 4
        samples.append((dur, size, sf, cto))
    return dict(n=n, flags=flags, ver=ver, data_offset=data_offset, first_flags=first_flags, samples=samples)


def elst_shift(ents):
    if not ents:
        return 0
    for _seg, media in ents:
        if media >= 0:
            return media
    return 0


def is_key(flags):
    if flags is None:
        return 1
    return 0 if (flags & 0x00010000) else 1


def analyze(init_path, chunks):
    init = Path(init_path).read_bytes()
    timescale = 1
    ents = []
    trex = dict(dur=0, size=0, flags=0)
    for typ, _o, _h, _s, pl in walk(init):
        if typ == b"mdhd":
            timescale = parse_mdhd(pl)
        elif typ == b"elst":
            ents = parse_elst(pl)
        elif typ == b"trex":
            trex = parse_trex(pl)
    shift = elst_shift(ents)
    rows = []
    for ch in chunks:
        data = Path(ch).read_bytes()
        tfhd = None
        tfdt = 0
        trun = None
        for typ, _o, _h, _s, pl in walk(data):
            if typ == b"tfhd":
                tfhd = parse_tfhd(pl)
            elif typ == b"tfdt":
                tfdt = parse_tfdt(pl)
            elif typ == b"trun":
                trun = parse_trun(pl)
        if trun is None:
            continue
        def_dur = (tfhd or {}).get("dur")
        if def_dur is None:
            def_dur = trex.get("dur") or 0
        def_size = (tfhd or {}).get("size")
        if def_size is None:
            def_size = trex.get("size") or 0
        def_flags = (tfhd or {}).get("sflags")
        if def_flags is None:
            def_flags = trex.get("flags") or 0
        dts = tfdt
        for i, (dur, size, sf, cto) in enumerate(trun["samples"]):
            if dur is None:
                dur = def_dur
            if size is None:
                size = def_size
            if i == 0 and trun["first_flags"] is not None:
                fl = trun["first_flags"]
            elif sf is not None:
                fl = sf
            else:
                fl = def_flags
            cto = 0 if cto is None else cto
            pts = dts + cto - shift
            dts_out = dts - shift
            rows.append((pts, dts_out, size, is_key(fl), dur, Path(ch).name))
            dts += dur
    return rows, timescale, shift, ents


def write_rows(path, rows, kind):
    with open(path, "w") as f:
        for pts, dts, size, key, dur, name in rows:
            f.write(f"{kind}\t{pts}\t{dts}\t{size}\t{key}\t{dur}\t{name}\n")


def main():
    out = Path(sys.argv[1] if len(sys.argv) > 1 else ".")
    vrows, vtb, vshift, velst = analyze(out / "init-0.m4s", sorted(out.glob("chunk-0-*.m4s")))
    arows, atb, ashift, aelst = analyze(out / "init-1.m4s", sorted(out.glob("chunk-1-*.m4s")))
    write_rows(out / "ref_video_trun.txt", vrows, "video")
    write_rows(out / "ref_audio_trun.txt", arows, "audio")
    (out / "ref_trun_meta.txt").write_text(
        f"video_trun={len(vrows)} timescale={vtb} elst_shift={vshift} elst={velst}\n"
        f"audio_trun={len(arows)} timescale={atb} elst_shift={ashift} elst={aelst}\n"
        "audio last sample is a short-duration remainder to reach MPD 3.0s; "
        "system ffprobe 7.1.5 may omit it. Compare demux against this table, "
        "not packet counts across versions.\n"
    )
    print(
        f"trun: video={len(vrows)} audio={len(arows)} audio_elst_shift={ashift} "
        f"last_audio={arows[-1] if arows else None}"
    )


if __name__ == "__main__":
    main()
