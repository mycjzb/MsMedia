#!/usr/bin/env python3
# 独立 ISO BMFF 箱体行走，对照 ms_bmff_inspect。不入库、不链产品库。
# by: Long <mycjzb@gmail.com>
"""Independent box walk for dev crosscheck. Not a product demux."""

from __future__ import annotations

import os
import re
import struct
import subprocess
import sys
from pathlib import Path

CONTAINERS = {
    b"moov", b"trak", b"edts", b"mdia", b"minf", b"dinf", b"stbl",
    b"mvex", b"moof", b"traf", b"mfra", b"udta",
}
TOPLEVEL_HINT = {
    b"ftyp", b"styp", b"moov", b"mdat", b"free", b"skip", b"wide",
    b"pnot", b"PICT", b"moof", b"sidx", b"ssix", b"prft", b"mfra",
    b"meta", b"meco", b"uuid", b"pdin", b"junk", b"idat", b"udta",
}

FRAG = {b"moof", b"styp", b"mvex"}
STILL = {b"heic", b"heix", b"avif", b"avis", b"mif1", b"msf1", b"miaf"}


def typ_ok(t: bytes) -> bool:
    return len(t) == 4


def typ_str(t: bytes) -> str:
    return "".join(chr(c) if 32 <= c <= 126 else "." for c in t)


def parse_header(buf: bytes, off: int):
    if off + 8 > len(buf):
        return None, "truncated_header"
    sz32, typ = struct.unpack(">I4s", buf[off : off + 8])
    hs = 8
    if not typ_ok(typ):
        return None, "bad_type"
    if sz32 == 1:
        if off + 16 > len(buf):
            return None, "truncated_largesize"
        tot = struct.unpack(">Q", buf[off + 8 : off + 16])[0]
        hs = 16
        if tot < 16:
            return None, "tiny_largesize"
    elif sz32 == 0:
        tot = 0
    elif sz32 < 8:
        return None, "tiny_size"
    else:
        tot = sz32
    if typ == b"uuid":
        if off + hs + 16 > len(buf):
            return None, "truncated_uuid"
        if tot != 0 and tot < hs + 16:
            return None, "uuid_size"
        hs += 16
    if tot != 0 and tot < hs:
        return None, "size_lt_header"
    return (typ, tot, hs), None


def walk(buf: bytes):
    boxes = []
    errors = []

    def rec(start, end, parent, depth):
        off = start
        while off < end:
            hdr, err = parse_header(buf, off)
            if hdr is None:
                errors.append((off, err))
                return
            typ, tot, hs = hdr
            if tot == 0:
                box_end = end
                if parent is not None:
                    errors.append((off, "size0_not_top"))
                    return
            else:
                box_end = off + tot
                if box_end > end:
                    errors.append((off, "child_overflow"))
                    return
            boxes.append(
                {
                    "off": off,
                    "size": tot,
                    "typ": typ,
                    "parent": parent,
                    "depth": depth,
                    "container": typ in CONTAINERS,
                    "hdr": hs,
                }
            )
            body = off + hs
            if typ in CONTAINERS:
                rec(body, box_end, typ, depth + 1)
            off = box_end if tot != 0 else end

    rec(0, len(buf), None, 0)
    return boxes, errors


def classify(boxes):
    types = [b["typ"] for b in boxes]
    brands = []
    major = b"    "
    still = False
    for b in boxes:
        if b["typ"] in (b"ftyp", b"styp") and b["size"] != 0:
            body = data_slice(b)
            if body is None or len(body) < 8:
                continue
            major = body[0:4]
            brands.append(major)
            for i in range(8, len(body) // 4 * 4, 4):
                brands.append(body[i : i + 4])
        # body capture needs file bytes; fill later
    return types, major, brands


# filled after walk by attaching file
FILE = b""


def data_slice(box):
    if box["size"] == 0:
        return FILE[box["off"] + box["hdr"] :]
    return FILE[box["off"] + box["hdr"] : box["off"] + box["size"]]


def classify_format(boxes):
    saw = {b["typ"] for b in boxes}
    major = b""
    brands = []
    still = False
    for b in boxes:
        if b["typ"] in (b"ftyp", b"styp"):
            body = data_slice(b)
            if len(body) >= 8:
                if not major:
                    major = body[0:4]
                brands.append(body[0:4])
                for i in range(8, len(body) // 4 * 4, 4):
                    brands.append(body[i : i + 4])
    for br in [major] + brands:
        if br in STILL:
            still = True
    if still and b"moov" not in saw and b"moof" not in saw:
        return "unsupported", major
    if saw & FRAG:
        return "fmp4", major
    if major == b"qt  " or b"qt  " in brands:
        return "mov", major
    if b"ftyp" in saw:
        return "mp4", major
    if b"moov" in saw or b"mdat" in saw:
        return "mov", major
    return "unknown", major


def run_inspect(inspect: str, path: Path, chunk: int) -> str:
    r = subprocess.run(
        [inspect, "--chunk", str(chunk), str(path)],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        check=False,
    )
    return r.stdout + r.stderr, r.returncode


def parse_inspect(text: str):
    boxes = []
    fmt = None
    summary = {}
    for line in text.splitlines():
        if line.startswith("box "):
            d = {}
            for part in line.split():
                if "=" in part:
                    k, v = part.split("=", 1)
                    d[k] = v
            boxes.append(d)
        elif line.startswith("format="):
            fmt = line.split()[0].split("=", 1)[1]
        elif line.startswith("summary "):
            for part in line.split():
                if "=" in part:
                    k, v = part.split("=", 1)
                    summary[k] = v
    return boxes, fmt, summary


def compare(path: Path, inspect: str, chunks):
    global FILE
    FILE = path.read_bytes()
    boxes, errors = walk(FILE)
    fmt, major = classify_format(boxes)
    notes = []
    ok = True
    if errors:
        notes.append(f"independent walk errors: {errors[:4]}")
        ok = False
    for chunk in chunks:
        text, rc = run_inspect(inspect, path, chunk)
        iboxes, ifmt, summary = parse_inspect(text)
        status = summary.get("status", "")
        want_rc = 0 if fmt != "unsupported" else 1
        if fmt == "unsupported" and status != "UNSUPPORTED":
            notes.append(f"chunk={chunk} expected UNSUPPORTED got {status}")
            ok = False
        elif fmt != "unsupported" and (rc != 0 or status.upper() != "OK"):
            notes.append(f"chunk={chunk} inspect rc={rc} status={status}")
            ok = False
        if ifmt != fmt and not (fmt == "unsupported"):
            notes.append(f"chunk={chunk} format inspect={ifmt} ref={fmt}")
            ok = False
        if len(iboxes) != len(boxes):
            notes.append(f"chunk={chunk} box count inspect={len(iboxes)} ref={len(boxes)}")
            ok = False
        else:
            for i, (a, b) in enumerate(zip(iboxes, boxes)):
                if a.get("typ") != typ_str(b["typ"]) or int(a.get("depth", "-1")) != b["depth"]:
                    notes.append(
                        f"chunk={chunk} box[{i}] inspect typ/depth={a.get('typ')}/{a.get('depth')} "
                        f"ref={typ_str(b['typ'])}/{b['depth']}"
                    )
                    ok = False
                    break
                if a.get("size") != str(b["size"]) or a.get("off") != str(b["off"]):
                    notes.append(
                        f"chunk={chunk} box[{i}] inspect off/size={a.get('off')}/{a.get('size')} "
                        f"ref={b['off']}/{b['size']}"
                    )
                    ok = False
                    break
    return ok, fmt, typ_str(major) if major else "----", len(boxes), notes


def main():
    outdir = Path(os.environ["OUTDIR"])
    inspect = os.environ["BMFF_INSPECT"]
    report = Path(os.environ["REPORT"])
    samples = list(Path(os.environ.get("SAMPLE_LIST", outdir / "bmff-samples.list")).read_text().splitlines())
    chunks = [1, 17, 4096, 0]
    # 0 means default 4096 in inspect if we pass 4096 already; use 4096 and 1,17,256
    chunks = [1, 17, 256, 4096]
    all_ok = True
    lines = []
    for rel in samples:
        if not rel.strip():
            continue
        path = Path(rel)
        if not path.is_file():
            path = outdir / rel
        ok, fmt, major, n, notes = compare(path, inspect, chunks)
        mark = "OK" if ok else "FAIL"
        msg = f"== {path.name} ({path.stat().st_size} bytes) format={fmt} major={major} boxes={n} {mark}"
        if notes:
            msg += "\n  " + "\n  ".join(notes)
        lines.append(msg)
        if not ok:
            all_ok = False
    text = "\n".join(lines) + "\n"
    report.parent.mkdir(parents=True, exist_ok=True)
    with report.open("a") as f:
        f.write(text)
    sys.stdout.write(text)
    if not all_ok:
        sys.exit(1)
    print("RESULT: all expected BMFF samples matched the independent box walk")


if __name__ == "__main__":
    main()
