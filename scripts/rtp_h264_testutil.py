#!/usr/bin/env python3
"""受控 H.264 RTP 打包 / UDP 发送 / 最小 loopback RTSP 服务。

仅供 test_ff_backend 使用，不是产品后端。产品库不得 exec 本脚本。
实现 RFC 6184 非交错模式（single NAL / STAP-A / FU-A + marker）。
by: Long <mycjzb@gmail.com>
"""
from __future__ import annotations

import argparse
import hashlib
import os
import signal
import socket
import struct
import sys
import time
from typing import Iterable, List, Optional, Sequence, Tuple

PT = 96
SSRC = 0x534DFF01  # 'SM' test ssrc
CLOCK = 90000


def start_codes(buf: bytes) -> List[Tuple[int, int, int]]:
    """Return list of (sc_pos, sc_len, nal_start) for each NAL."""
    out = []
    i = 0
    n = len(buf)
    while i + 3 <= n:
        if i + 4 <= n and buf[i : i + 4] == b"\x00\x00\x00\x01":
            out.append((i, 4, i + 4))
            i += 4
            continue
        if buf[i : i + 3] == b"\x00\x00\x01":
            out.append((i, 3, i + 3))
            i += 3
            continue
        i += 1
    return out


def split_nals(buf: bytes) -> List[bytes]:
    marks = start_codes(buf)
    nals: List[bytes] = []
    for idx, (_p, _l, ns) in enumerate(marks):
        ne = marks[idx + 1][0] if idx + 1 < len(marks) else len(buf)
        nal = buf[ns:ne].rstrip(b"\x00")
        if nal:
            nals.append(nal)
    return nals


def _unescape_rbsp(nal: bytes) -> bytes:
    # skip nal header; unescape 00 00 03
    body = nal[1:]
    return body.replace(b"\x00\x00\x03", b"\x00\x00")


def _ue(data: bytes, bitpos: int) -> Tuple[int, int]:
    zeros = 0
    nbits = len(data) * 8
    while bitpos + zeros < nbits:
        byte = data[(bitpos + zeros) // 8]
        bit = 7 - ((bitpos + zeros) % 8)
        if (byte >> bit) & 1:
            break
        zeros += 1
    else:
        return 0, bitpos
    bitpos = bitpos + zeros
    val = 1
    for _ in range(zeros):
        bitpos += 1
        if bitpos > nbits:
            break
        byte = data[(bitpos - 1) // 8]
        bit = 7 - ((bitpos - 1) % 8)
        val = (val << 1) | ((byte >> bit) & 1)
    return val - 1, bitpos


def first_mb_in_slice(nal: bytes) -> Optional[int]:
    typ = nal[0] & 0x1F
    if typ < 1 or typ > 5:
        return None
    body = _unescape_rbsp(nal)
    if not body:
        return None
    v, _ = _ue(body, 0)
    return v


def split_aus(nals: Sequence[bytes]) -> List[List[bytes]]:
    aus: List[List[bytes]] = []
    cur: List[bytes] = []

    def has_vcl(group: Sequence[bytes]) -> bool:
        return any(1 <= (n[0] & 0x1F) <= 5 for n in group if n)

    for nal in nals:
        typ = nal[0] & 0x1F
        new_au = False
        if typ == 9 and cur:
            new_au = True
        elif typ in (6, 7, 8) and has_vcl(cur):
            new_au = True
        elif typ in (1, 2, 3, 4, 5) and has_vcl(cur):
            fmb = first_mb_in_slice(nal)
            if fmb == 0:
                new_au = True
        if new_au:
            aus.append(cur)
            cur = []
        cur.append(nal)
    if cur:
        aus.append(cur)
    return aus


def au_annexb(nals: Sequence[bytes]) -> bytes:
    return b"".join(b"\x00\x00\x00\x01" + n for n in nals)


def au_types(nals: Sequence[bytes]) -> str:
    return ",".join(str(n[0] & 0x1F) for n in nals)


def au_is_key(nals: Sequence[bytes]) -> int:
    return 1 if any((n[0] & 0x1F) == 5 for n in nals) else 0


def rtp_header(seq: int, ts: int, marker: int, ssrc: int = SSRC, pt: int = PT) -> bytes:
    b0 = 0x80
    b1 = (0x80 if marker else 0) | (pt & 0x7F)
    return struct.pack("!BBHII", b0, b1, seq & 0xFFFF, ts & 0xFFFFFFFF, ssrc & 0xFFFFFFFF)


def packetize_nals(
    nals: Sequence[bytes],
    mtu: int,
    seq: int,
    ts: int,
) -> Tuple[List[bytes], int]:
    """Packetize one AU. mtu is max RTP payload size (not including 12-byte header)."""
    packets: List[bytes] = []
    if not nals:
        return packets, seq
    # Prefer STAP-A when 2+ NALs fit together ( typ(1) + sum(2+len) )
    stap_len = 1 + sum(2 + len(n) for n in nals)
    if len(nals) >= 2 and stap_len <= mtu:
        payload = bytes([24])  # STAP-A
        for n in nals:
            payload += struct.pack("!H", len(n)) + n
        packets.append(payload)
    else:
        for n in nals:
            if len(n) <= mtu:
                packets.append(n)
            else:
                # FU-A: fu_indicator + fu_header + fragment
                nal_hdr = n[0]
                nal_type = nal_hdr & 0x1F
                nri = nal_hdr & 0x60
                fu_ind = nri | 28
                body = n[1:]
                max_frag = mtu - 2
                if max_frag < 1:
                    max_frag = 1
                offset = 0
                first = True
                while offset < len(body):
                    chunk = body[offset : offset + max_frag]
                    offset += len(chunk)
                    start = 0x80 if first else 0
                    end = 0x40 if offset >= len(body) else 0
                    fu_hdr = start | end | nal_type
                    packets.append(bytes([fu_ind, fu_hdr]) + chunk)
                    first = False
    # wrap with RTP headers; marker on last
    out = []
    for i, pl in enumerate(packets):
        marker = 1 if i == len(packets) - 1 else 0
        out.append(rtp_header(seq, ts, marker) + pl)
        seq = (seq + 1) & 0xFFFF
    return out, seq


def pack_stream(nals_by_au: Sequence[Sequence[bytes]], mtu: int, fps: int = 25):
    seq = 0
    ts = 0
    inc = CLOCK // fps
    all_pkts: List[bytes] = []
    meta = []
    for i, nals in enumerate(nals_by_au):
        start_seq = seq
        pkts, seq = packetize_nals(nals, mtu, seq, ts)
        raw = au_annexb(nals)
        kinds = []
        for p in pkts:
            pl = p[12] if len(p) > 12 else 0
            typ = pl & 0x1F
            if typ == 28 and len(p) > 13:
                kinds.append("FU-A")
            elif typ == 24:
                kinds.append("STAP-A")
            else:
                kinds.append("NAL")
        meta.append(
            {
                "index": i,
                "pts_90k": ts,
                "key": au_is_key(nals),
                "size": len(raw),
                "sha256": hashlib.sha256(raw).hexdigest(),
                "nal_types": au_types(nals),
                "rtp_seq0": start_seq,
                "rtp_count": len(pkts),
                "pkt_kinds": ",".join(kinds),
            }
        )
        all_pkts.extend(pkts)
        ts += inc
    return all_pkts, meta


def write_ref(path: str, meta, mtu: int, src: str) -> None:
    with open(path, "w", encoding="utf-8") as f:
        f.write(f"# rtp h264 au reference mtu={mtu} src={src}\n")
        f.write("# index pts_90k key size sha256 nal_types rtp_seq0 rtp_count pkt_kinds\n")
        for m in meta:
            f.write(
                f"{m['index']}\t{m['pts_90k']}\t{m['key']}\t{m['size']}\t{m['sha256']}\t"
                f"{m['nal_types']}\t{m['rtp_seq0']}\t{m['rtp_count']}\t{m['pkt_kinds']}\n"
            )


def sprop_from_nals(nals_by_au: Sequence[Sequence[bytes]]) -> str:
    import base64

    sps = pps = None
    for nals in nals_by_au:
        for n in nals:
            typ = n[0] & 0x1F
            if typ == 7 and sps is None:
                sps = n
            elif typ == 8 and pps is None:
                pps = n
    if not sps or not pps:
        return ""
    b64 = lambda x: base64.b64encode(x).decode("ascii")
    return f"sprop-parameter-sets={b64(sps)},{b64(pps)}"


def write_sdp(path: str, port: int, sprop: str) -> None:
    fmtp = "packetization-mode=1"
    if sprop:
        fmtp += ";" + sprop
    text = (
        "v=0\r\n"
        "o=- 0 0 IN IP4 127.0.0.1\r\n"
        "s=MsMedia RTP test\r\n"
        "c=IN IP4 127.0.0.1\r\n"
        "t=0 0\r\n"
        f"m=video {port} RTP/AVP 96\r\n"
        "a=rtpmap:96 H264/90000\r\n"
        f"a=fmtp:96 {fmtp}\r\n"
    )
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)


def load_annexb(path: str) -> List[List[bytes]]:
    data = Path_read(path)
    nals = split_nals(data)
    if not nals:
        raise SystemExit(f"no NAL units in {path}")
    return split_aus(nals)


def Path_read(path: str) -> bytes:
    with open(path, "rb") as f:
        return f.read()


def apply_faults(pkts: List[bytes], drop: Sequence[int], reorder: bool) -> List[bytes]:
    out = [p for i, p in enumerate(pkts) if i not in set(drop)]
    if reorder and len(out) >= 3:
        # swap two non-adjacent payload packets in the first AU if possible
        # specifically swap index 1 and 2 to test jitter buffer (keep first)
        out[1], out[2] = out[2], out[1]
    return out


def cmd_pack(args: argparse.Namespace) -> int:
    aus = load_annexb(args.annexb)
    os.makedirs(args.out_dir, exist_ok=True)
    pkts, meta = pack_stream(aus, args.mtu, args.fps)
    blob = bytearray()
    lens = []
    for p in pkts:
        lens.append(len(p))
        blob += p
    with open(os.path.join(args.out_dir, "rtp.bin"), "wb") as f:
        f.write(blob)
    with open(os.path.join(args.out_dir, "rtp_sizes.txt"), "w", encoding="utf-8") as f:
        f.write("\n".join(str(x) for x in lens) + "\n")
    write_ref(os.path.join(args.out_dir, "ref_aus.txt"), meta, args.mtu, args.annexb)
    for i, nals in enumerate(aus):
        with open(os.path.join(args.out_dir, f"au_{i:03d}.bin"), "wb") as f:
            f.write(au_annexb(nals))
    sprop = sprop_from_nals(aus)
    write_sdp(os.path.join(args.out_dir, "template.sdp"), 0, sprop)
    kinds = set()
    for m in meta:
        kinds.update(m["pkt_kinds"].split(","))
    print(
        f"packed aus={len(aus)} pkts={len(pkts)} kinds={','.join(sorted(kinds))} "
        f"mtu={args.mtu} out={args.out_dir}",
        flush=True,
    )
    return 0


def read_pkts(out_dir: str) -> List[bytes]:
    sizes = [
        int(x)
        for x in open(os.path.join(out_dir, "rtp_sizes.txt"), encoding="utf-8")
        .read()
        .split()
        if x
    ]
    data = Path_read(os.path.join(out_dir, "rtp.bin"))
    pkts = []
    off = 0
    for n in sizes:
        pkts.append(data[off : off + n])
        off += n
    return pkts


def cmd_send(args: argparse.Namespace) -> int:
    pkts = read_pkts(args.out_dir)
    drop = [int(x) for x in (args.drop or "").split(",") if x.strip() != ""]
    pkts = apply_faults(pkts, drop, args.reorder)
    if args.delay_ms > 0:
        time.sleep(args.delay_ms / 1000.0)
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        sock.bind(("127.0.0.1", 0))
        dest = ("127.0.0.1", args.port)
        for i, p in enumerate(pkts):
            sock.sendto(p, dest)
            if args.pace_us > 0:
                time.sleep(args.pace_us / 1e6)
        print(f"sent {len(pkts)} rtp packets to 127.0.0.1:{args.port}", flush=True)
    finally:
        sock.close()
    return 0


def _read_rtsp_req(conn: socket.socket, timeout: float = 5.0) -> Optional[Tuple[str, dict, bytes]]:
    conn.settimeout(timeout)
    buf = b""
    try:
        while b"\r\n\r\n" not in buf and len(buf) < 65536:
            chunk = conn.recv(4096)
            if not chunk:
                return None
            buf += chunk
    except socket.timeout:
        return None
    head, _, rest = buf.partition(b"\r\n\r\n")
    lines = head.decode("ascii", "replace").split("\r\n")
    if not lines or not lines[0]:
        return None
    headers = {}
    for ln in lines[1:]:
        if ":" in ln:
            k, v = ln.split(":", 1)
            headers[k.strip().lower()] = v.strip()
    clen = int(headers.get("content-length", "0") or "0")
    body = rest
    while len(body) < clen:
        body += conn.recv(clen - len(body))
    return lines[0], headers, body[:clen]


def _rtsp_ok(conn: socket.socket, cseq: str, extra: str = "", body: bytes = b"", ctype: str = "") -> None:
    hdr = f"RTSP/1.0 200 OK\r\nCSeq: {cseq}\r\n"
    if extra:
        hdr += extra if extra.endswith("\r\n") else extra + "\r\n"
    if body:
        hdr += f"Content-Type: {ctype}\r\nContent-Length: {len(body)}\r\n"
    hdr += "\r\n"
    conn.sendall(hdr.encode("ascii") + body)


def cmd_rtsp(args: argparse.Namespace) -> int:
    aus = load_annexb(args.annexb)
    pkts, _meta = pack_stream(aus, args.mtu, args.fps)
    drop = [int(x) for x in (args.drop or "").split(",") if x.strip() != ""]
    send_pkts = apply_faults(pkts, drop, args.reorder)
    sprop = sprop_from_nals(aus)

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", args.port))
    port = srv.getsockname()[1]
    srv.listen(1)
    srv.settimeout(args.listen_timeout)
    print(f"READY port={port} pkts={len(send_pkts)} aus={len(aus)}", flush=True)

    stop = {"v": False}

    def _stop(signum, _frm):
        stop["v"] = True

    signal.signal(signal.SIGTERM, _stop)
    signal.signal(signal.SIGINT, _stop)

    conn = None
    try:
        try:
            conn, _addr = srv.accept()
        except socket.timeout:
            print("LISTEN_TIMEOUT", flush=True)
            return 2
        conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        if getattr(args, "accept_only", False):
            # 接受后不回复，供客户端 timeout 对照（不是 cancel）
            time.sleep(max(args.listen_timeout, 1.0))
            print("ACCEPT_ONLY done", flush=True)
            return 0
        session = "1"
        interleaved = (0, 1)
        client_rtp = None
        udp_sock = None
        control = "track1"
        played = False
        use_interleaved = False
        while not stop["v"]:
            req = _read_rtsp_req(conn, timeout=args.listen_timeout)
            if req is None:
                break
            start, headers, _body = req
            parts = start.split()
            method = parts[0] if parts else ""
            cseq = headers.get("cseq", "0")
            if method == "OPTIONS":
                _rtsp_ok(
                    conn,
                    cseq,
                    "Public: OPTIONS, DESCRIBE, SETUP, TEARDOWN, PLAY, GET_PARAMETER\r\n",
                )
            elif method == "DESCRIBE":
                fmtp = "packetization-mode=1"
                if sprop:
                    fmtp += ";" + sprop
                sdp = (
                    "v=0\r\n"
                    "o=- 0 0 IN IP4 127.0.0.1\r\n"
                    "s=MsMedia RTSP test\r\n"
                    "c=IN IP4 127.0.0.1\r\n"
                    "t=0 0\r\n"
                    "m=video 0 RTP/AVP 96\r\n"
                    "a=rtpmap:96 H264/90000\r\n"
                    f"a=fmtp:96 {fmtp}\r\n"
                    f"a=control:{control}\r\n"
                ).encode("ascii")
                base = f"Content-Base: rtsp://127.0.0.1:{port}/live/\r\n"
                _rtsp_ok(conn, cseq, base, sdp, "application/sdp")
            elif method == "SETUP":
                tr = headers.get("transport", "")
                extra = f"Session: {session}\r\n"
                use_interleaved = False
                if "RTP/AVP/TCP" in tr or "interleaved=" in tr:
                    use_interleaved = True
                    # TCP interleaved
                    if "interleaved=" in tr:
                        try:
                            rng = tr.split("interleaved=")[1].split(";")[0]
                            a, b = rng.split("-")
                            interleaved = (int(a), int(b))
                        except (IndexError, ValueError):
                            interleaved = (0, 1)
                    extra += (
                        f"Transport: RTP/AVP/TCP;unicast;interleaved="
                        f"{interleaved[0]}-{interleaved[1]}\r\n"
                    )
                    args.transport = "tcp"
                else:
                    # UDP
                    cports = None
                    if "client_port=" in tr:
                        try:
                            rng = tr.split("client_port=")[1].split(";")[0]
                            a, b = rng.split("-")
                            cports = (int(a), int(b))
                            client_rtp = cports[0]
                        except (IndexError, ValueError):
                            cports = None
                    udp_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
                    udp_sock.bind(("127.0.0.1", 0))
                    sport = udp_sock.getsockname()[1]
                    if cports is None:
                        cports = (sport + 2, sport + 3)  # unused fallback
                        client_rtp = cports[0]
                    extra += (
                        f"Transport: RTP/AVP;unicast;client_port={cports[0]}-{cports[1]};"
                        f"server_port={sport}-{sport + 1}\r\n"
                    )
                    args.transport = "udp"
                _rtsp_ok(conn, cseq, extra)
            elif method == "PLAY":
                extra = (
                    f"Session: {session}\r\n"
                    f"RTP-Info: url=rtsp://127.0.0.1:{port}/live/{control};seq=0;rtptime=0\r\n"
                )
                _rtsp_ok(conn, cseq, extra)
                played = True
                # send after a short delay so the client can finish PLAY setup
                time.sleep(0.05)
                if use_interleaved:
                    ch = interleaved[0]
                    for p in send_pkts:
                        frame = b"$" + bytes([ch]) + struct.pack("!H", len(p)) + p
                        conn.sendall(frame)
                        if args.pace_us > 0:
                            time.sleep(args.pace_us / 1e6)
                else:
                    if udp_sock is None or client_rtp is None:
                        print("no udp dest", flush=True)
                        return 3
                    # peer address is the TCP peer (loopback) with client_rtp port
                    dest = (conn.getpeername()[0], client_rtp)
                    for p in send_pkts:
                        udp_sock.sendto(p, dest)
                        if args.pace_us > 0:
                            time.sleep(args.pace_us / 1e6)
                print(f"PLAYED transport={args.transport} pkts={len(send_pkts)}", flush=True)
            elif method in ("TEARDOWN",):
                _rtsp_ok(conn, cseq, f"Session: {session}\r\n")
                break
            elif method in ("GET_PARAMETER", "SET_PARAMETER"):
                _rtsp_ok(conn, cseq, f"Session: {session}\r\n")
            else:
                msg = (
                    f"RTSP/1.0 501 Not Implemented\r\nCSeq: {cseq}\r\n\r\n"
                ).encode("ascii")
                conn.sendall(msg)
        if not played:
            print("NO_PLAY", flush=True)
    finally:
        if conn is not None:
            try:
                conn.close()
            except OSError:
                pass
        if "udp_sock" in dir() and udp_sock is not None:
            try:
                udp_sock.close()
            except OSError:
                pass
        srv.close()
    return 0


def main() -> int:
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("pack")
    p.add_argument("--annexb", required=True)
    p.add_argument("--out-dir", required=True)
    p.add_argument("--mtu", type=int, default=200)
    p.add_argument("--fps", type=int, default=25)

    p = sub.add_parser("send")
    p.add_argument("--out-dir", required=True)
    p.add_argument("--port", type=int, required=True)
    p.add_argument("--delay-ms", type=int, default=200)
    p.add_argument("--pace-us", type=int, default=0)
    p.add_argument("--drop", default="")
    p.add_argument("--reorder", action="store_true")

    p = sub.add_parser("rtsp")
    p.add_argument("--annexb", required=True)
    p.add_argument("--port", type=int, default=0)
    p.add_argument("--mtu", type=int, default=200)
    p.add_argument("--fps", type=int, default=25)
    p.add_argument("--drop", default="")
    p.add_argument("--reorder", action="store_true")
    p.add_argument("--pace-us", type=int, default=0)
    p.add_argument("--listen-timeout", type=float, default=15.0)
    p.add_argument("--transport", default="auto", help="hint; actual chosen by SETUP")
    p.add_argument("--accept-only", action="store_true")

    args = ap.parse_args()
    if args.cmd == "pack":
        return cmd_pack(args)
    if args.cmd == "send":
        return cmd_send(args)
    if args.cmd == "rtsp":
        return cmd_rtsp(args)
    return 2


if __name__ == "__main__":
    sys.exit(main())
