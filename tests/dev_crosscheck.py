#!/usr/bin/env python3
# 对照 ms_ts_inspect 与 ffprobe 公开输出。开发用，不进产品库。
# by: Long <mycjzb@gmail.com>
"""Compare inspect output to ffprobe packets/frames and ADTS payloads."""

from __future__ import annotations

import hashlib
import json
import os
import subprocess
import sys
from pathlib import Path

PTS_MASK = (1 << 33) - 1
OUTDIR = Path(os.environ["OUTDIR"])
FFPROBE = os.environ["FFPROBE"]
FFMPEG = os.environ["FFMPEG"]
INSPECT = os.environ["INSPECT"]
REPORT = Path(os.environ["REPORT"])


def log(msg: str) -> None:
    print(msg)
    with REPORT.open("a", encoding="utf-8") as f:
        f.write(msg + "\n")


def run(cmd, **kw):
    return subprocess.run(cmd, check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, **kw)


def parse_inspect(text: str) -> dict:
    out = {
        "tracks": [],
        "configs": [],
        "au": [],
        "disc": 0,
        "status": "?",
        "summary": {},
    }
    for line in text.splitlines():
        if line.startswith("track "):
            rec = {}
            for part in line.split()[1:]:
                if "=" in part:
                    k, v = part.split("=", 1)
                    rec[k] = v
            out["tracks"].append(rec)
        elif line.startswith("config "):
            rec = {}
            for part in line.split()[1:]:
                if "=" in part:
                    k, v = part.split("=", 1)
                    rec[k] = v
            out["configs"].append(rec)
        elif line.startswith("au "):
            rec = {}
            for part in line.split()[1:]:
                if "=" in part:
                    k, v = part.split("=", 1)
                    rec[k] = v
            out["au"].append(rec)
        elif line.startswith("discontinuity"):
            out["disc"] += 1
        elif line.startswith("summary "):
            rec = {}
            for part in line.split()[1:]:
                if "=" in part:
                    k, v = part.split("=", 1)
                    rec[k] = v
            out["summary"] = rec
            out["status"] = rec.get("status", "?")
    return out


def pts_u(s: str | None) -> int | None:
    if s is None or s.startswith("na:"):
        return None
    return int(s) & PTS_MASK


def ffprobe_json(path: Path) -> dict:
    cmd = [
        FFPROBE, "-hide_banner", "-loglevel", "error",
        "-show_packets", "-show_frames", "-show_streams",
        "-of", "json", str(path),
    ]
    p = run(cmd)
    return json.loads(p.stdout.decode())


def extract_adts(path: Path, dest: Path) -> None:
    run([
        FFMPEG, "-y", "-hide_banner", "-loglevel", "error",
        "-i", str(path), "-map", "0:a:0", "-c", "copy", "-f", "adts", str(dest),
    ])


def split_adts_payloads(data: bytes) -> list[bytes]:
    frames = []
    i = 0
    n = len(data)
    while i + 7 <= n:
        if data[i] != 0xFF or (data[i + 1] & 0xF0) != 0xF0:
            i += 1
            continue
        hdr = 7 if (data[i + 1] & 0x01) else 9
        flen = ((data[i + 3] & 0x03) << 11) | (data[i + 4] << 3) | ((data[i + 5] >> 5) & 0x07)
        if flen < hdr or i + flen > n:
            break
        frames.append(data[i + hdr : i + flen])
        i += flen
    return frames


def sha256_hex(b: bytes) -> str:
    return hashlib.sha256(b).hexdigest()


def stream_ids(info: dict) -> tuple[set[int], set[int]]:
    v, a = set(), set()
    for s in info.get("streams") or []:
        idx = int(s.get("index", -1))
        if s.get("codec_type") == "video":
            v.add(idx)
        elif s.get("codec_type") == "audio":
            a.add(idx)
    return v, a


def packet_pts(pkt: dict) -> int | None:
    if "pts" in pkt and pkt["pts"] not in (None, "N/A"):
        return int(pkt["pts"]) & PTS_MASK
    if "pts_time" in pkt and pkt["pts_time"] not in (None, "N/A"):
        # seconds * 90000, only as fallback
        return int(round(float(pkt["pts_time"]) * 90000)) & PTS_MASK
    return None


def packet_hash(pkt: dict) -> str | None:
    h = pkt.get("data_hash") or ""
    if not h:
        return None
    if ":" in h:
        h = h.split(":", 1)[1]
    return h.lower()



def iter_ts_packets(data: bytes, size: int = 188):
    i = 0
    n = len(data)
    # 跳过可选 192 字节前缀，本脚本只处理 188。
    while i + size <= n:
        if data[i] != 0x47:
            i += 1
            continue
        yield data[i : i + 188]
        i += 188


def pes_payloads_by_pid(data: bytes) -> dict[int, list[tuple[int | None, bytes]]]:
    """独立的 TS PES 重组（不调用 MsMedia），作为载荷对照。"""
    acc: dict[int, bytearray] = {}
    out: dict[int, list[tuple[int | None, bytes]]] = {}

    def flush(pid: int):
        buf = acc.get(pid)
        if not buf or len(buf) < 6:
            acc[pid] = bytearray()
            return
        if buf[0] != 0 or buf[1] != 0 or buf[2] != 1:
            acc[pid] = bytearray()
            return
        pts = None
        if len(buf) >= 9:
            flags = buf[7]
            hlen = buf[8]
            if 9 + hlen <= len(buf):
                payload = bytes(buf[9 + hlen :])
                if (flags & 0x80) and hlen >= 5 and len(buf) >= 14:
                    b = buf[9:14]
                    pts = (((b[0] >> 1) & 7) << 30) | (b[1] << 22) | (((b[2] >> 1) & 0x7F) << 15) | (b[3] << 7) | (b[4] >> 1)
                    pts &= PTS_MASK
            else:
                payload = b""
        else:
            payload = b""
        if payload:
            out.setdefault(pid, []).append((pts, payload))
        acc[pid] = bytearray()

    for pkt in iter_ts_packets(data):
        pid = ((pkt[1] & 0x1F) << 8) | pkt[2]
        if pid == 0x1FFF:
            continue
        pusi = (pkt[1] >> 6) & 1
        adapt = (pkt[3] >> 4) & 0x3
        off = 4
        if adapt in (2, 3):
            alen = pkt[4]
            off = 5 + alen
        if off >= 188:
            continue
        payload = pkt[off:]
        if pusi:
            if pid in acc and acc[pid]:
                flush(pid)
            acc[pid] = bytearray(payload)
        else:
            if pid in acc and acc[pid]:
                acc[pid].extend(payload)
    for pid in list(acc.keys()):
        flush(pid)
    return out


def compare_one(ts: Path, chunk: int, expect_ok: bool = True) -> list[str]:
    issues: list[str] = []
    notes: list[str] = []
    cmd = [INSPECT, "--chunk", str(chunk), str(ts)]
    p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    text = p.stdout.decode()
    if p.returncode != 0 and expect_ok:
        issues.append(f"inspect exit {p.returncode}: {p.stderr.decode().strip() or text.strip()[-200:]}")
    insp = parse_inspect(text)
    if expect_ok and insp["status"] != "ok":
        issues.append(f"inspect status={insp['status']}")

    info = ffprobe_json(ts)
    v_idx, a_idx = stream_ids(info)
    pkts = list(info.get("packets") or [])
    frames = list(info.get("frames") or [])
    for item in info.get("packets_and_frames") or []:
        typ = item.get("type")
        if typ == "packet":
            pkts.append(item)
        elif typ == "frame":
            frames.append(item)
    vpk = [x for x in pkts if int(x.get("stream_index", -1)) in v_idx]
    apk = [x for x in pkts if int(x.get("stream_index", -1)) in a_idx]
    vfr = [x for x in frames if int(x.get("stream_index", -1)) in v_idx]
    afr = [x for x in frames if int(x.get("stream_index", -1)) in a_idx]

    v_au = [x for x in insp["au"] if x.get("codec") == "h264"]
    a_au = [x for x in insp["au"] if x.get("codec") == "aac"]

    if len(v_au) != len(vfr):
        issues.append(f"video AU {len(v_au)} != ffprobe frames {len(vfr)} (packets {len(vpk)})")
    if len(a_au) != len(afr) and len(a_au) != len(apk):
        # 一 PES 多 AAC 时 packet 数可少于 frame 数；优先对 frame
        issues.append(f"audio AU {len(a_au)} != ffprobe frames {len(afr)} (packets {len(apk)})")

    # PTS/关键帧对照必须用 packet（解码/码流顺序）。show_frames 常是显示顺序，
    # 有 B 帧时与 AU 发射顺序不同，不能按索引对拍。
    ncmp = min(len(v_au), len(vpk))
    pts_mismatch = 0
    key_mismatch = 0
    for i in range(ncmp):
        ip = pts_u(v_au[i].get("pts"))
        fp = packet_pts(vpk[i])
        if ip is not None and fp is not None and ip != fp:
            pts_mismatch += 1
            if pts_mismatch <= 3:
                issues.append(f"video[{i}] pts inspect={ip} packet={fp}")
        ik = int(v_au[i].get("key", "0"))
        flags = vpk[i].get("flags") or ""
        fk = 1 if str(flags).startswith("K") else 0
        if ik != fk:
            key_mismatch += 1
            if key_mismatch <= 3:
                issues.append(f"video[{i}] key inspect={ik} packet_flags={flags}")
    if pts_mismatch:
        issues.append(f"video pts mismatches total {pts_mismatch}")
    if key_mismatch:
        issues.append(f"video key mismatches total {key_mismatch}")

    # 视频载荷：用独立 TS PES 重组对照（不经过 MsMedia）。1 PES=1 AU 时应对上。
    v_pids = set()
    for au in v_au:
        try:
            v_pids.add(int(au.get("track", "0")))
        except ValueError:
            pass
    pes_map = pes_payloads_by_pid(ts.read_bytes())
    vpes = []
    for pid, items in pes_map.items():
        if pid in v_pids or (not v_pids and len(items) == len(v_au)):
            vpes.extend(items)
    # 若按 PID 精确过滤
    if v_pids:
        vpes = []
        for pid in v_pids:
            vpes.extend(pes_map.get(pid, []))
    hash_mismatch = 0
    if vpes and v_au:
        if len(vpes) != len(v_au):
            issues.append(f"video PES {len(vpes)} != inspect AU {len(v_au)}")
        n = min(len(vpes), len(v_au))
        for i in range(n):
            ph = sha256_hex(vpes[i][1])
            ih = (v_au[i].get("sha256") or "").lower()
            if ph != ih:
                hash_mismatch += 1
                if hash_mismatch <= 3:
                    issues.append(f"video[{i}] sha256 inspect={ih} pes={ph}")
        if hash_mismatch:
            issues.append(f"video payload hash mismatches {hash_mismatch}/{n}")

    # AAC：用 ffmpeg -c copy ADTS 去头后对照 payload hash；PTS 对 frame
    adts_path = ts.with_suffix(ts.suffix + f".chunk{chunk}.adts")
    try:
        extract_adts(ts, adts_path)
        payloads = split_adts_payloads(adts_path.read_bytes())
        if len(payloads) != len(a_au):
            issues.append(f"aac payload frames {len(payloads)} != inspect AU {len(a_au)}")
        nh = min(len(payloads), len(a_au))
        pay_mis = 0
        for i in range(nh):
            want = sha256_hex(payloads[i])
            got = (a_au[i].get("payload_sha256") or "").lower()
            if got != want:
                pay_mis += 1
                if pay_mis <= 3:
                    issues.append(f"aac[{i}] payload_sha256 inspect={got} ref={want}")
        if pay_mis:
            issues.append(f"aac payload hash mismatches {pay_mis}/{nh}")
    except subprocess.CalledProcessError as e:
        issues.append(f"ffmpeg extract adts failed: {e.stderr.decode()[-200:]}")

    ncmpa = min(len(a_au), len(afr))
    a_pts_mis = 0
    a_round = 0
    max_drift = 0
    for i in range(ncmpa):
        ip = pts_u(a_au[i].get("pts"))
        fp = packet_pts(afr[i])
        if ip is not None and fp is not None and ip != fp:
            drift = abs(int(ip) - int(fp))
            if drift > max_drift:
                max_drift = drift
            # 余数累积与 floor(k*90000*1024/rate) 等价；与 ffprobe 最多允许 1 tick。
            if drift <= 1:
                a_round += 1
            else:
                a_pts_mis += 1
                if a_pts_mis <= 3:
                    issues.append(f"audio[{i}] pts inspect={ip} ffprobe={fp}")
    if a_pts_mis:
        issues.append(f"audio pts mismatches total {a_pts_mis} (max drift {max_drift})")
    elif a_round:
        notes.append(f"aac intra-PES PTS remainder vs ffprobe: {a_round} frames differ, max {max_drift} ticks")

    return issues, notes


def main() -> int:
    samples = [
        ("bl_aud1_s1_48k.ts", True, "baseline +AUD 1-slice 48k no-B"),
        ("bl_aud0_s1_48k.ts", True, "baseline no-AUD 1-slice 48k no-B"),
        ("bl_aud0_s2_48k.ts", True, "baseline no-AUD 2-slice 48k no-B"),
        ("main_aud1_s1_441.ts", True, "main +AUD 44.1k with B-frames"),
        ("high_aud0_s2_48k.ts", True, "high no-AUD 2-slice 48k with B-frames"),
        ("bl_pts_wrap.ts", True, "baseline +AUD 48k PTS near 33-bit wrap"),
        ("concat_disc.ts", False, "concat two TS (CC/PCR break; inspect may error or emit disc)"),
    ]
    failed = 0
    chunks = [188, 17, 1]
    for name, expect_ok, desc in samples:
        ts = OUTDIR / name
        if not ts.is_file():
            log(f"MISSING {name}")
            failed += 1
            continue
        size = ts.stat().st_size
        log(f"== {name} ({size} bytes) {desc}")
        # 用 188 做主对照；其它 chunk 只核对计数与 status
        issues, notes = compare_one(ts, 188, expect_ok=expect_ok)
        if expect_ok:
            for ch in (17, 1):
                extra, extra_notes = compare_one(ts, ch, expect_ok=True)
                if extra:
                    issues.append(f"chunk={ch}: " + "; ".join(extra[:8]))
                notes.extend(extra_notes)
        if issues:
            if expect_ok:
                log("  FAIL: " + " | ".join(issues))
                failed += 1
            else:
                log("  NOTE (not a product pass): " + " | ".join(issues[:8]))
        else:
            msg = "  OK vs ffprobe/ffmpeg extract"
            if notes:
                # 去重
                uniq = []
                for n in notes:
                    if n not in uniq:
                        uniq.append(n)
                msg += " | " + " | ".join(uniq)
            log(msg)
    log("")
    if failed:
        log(f"RESULT: {failed} sample(s) differed or failed")
        return 1
    log("RESULT: all expected samples matched the independent reference")
    return 0


if __name__ == "__main__":
    sys.exit(main())
