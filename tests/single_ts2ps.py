#!/usr/bin/env python3
# 测试样本生成：MPEG-TS → MPEG-PS（GB28181 风格：pack 头 + PSM(0x1B/0x0F) + 视频/音频 PES，大 PES 切分），
# 用于验证纯自研 PS→TS 转封装（含 AAC/ADTS，FFmpeg mpeg 复用器不支持 AAC）。by: Long <mycjzb@gmail.com>
import sys, struct
d = open(sys.argv[1], "rb").read(); out = open(sys.argv[2], "wb")
pmt_pid = None; es = {}; pes = {}
def scr_pack(scr):
    b = bytearray(14); b[0:4] = b"\x00\x00\x01\xba"
    b[4] = 0x44 | ((scr >> 27) & 0x38) | ((scr >> 28) & 3); b[5] = (scr >> 20) & 0xFF
    b[6] = ((scr >> 12) & 0xF8) | 4 | ((scr >> 13) & 3); b[7] = (scr >> 5) & 0xFF; b[8] = ((scr << 3) & 0xF8) | 4; b[9] = 1
    b[10:13] = b"\x01\x89\xc3"; b[13] = 0xF8; return bytes(b)
def psm():
    m = bytes([0x1B, 0xE0, 0, 0, 0x0F, 0xC0, 0, 0])
    body = b"\x80\x01" + b"\x00\x00" + struct.pack(">H", len(m)) + m + b"\x00\x00\x00\x00"
    return b"\x00\x00\x01\xbc" + struct.pack(">H", len(body)) + body
first = True; npk = 0
def flush(pid):
    global first, npk
    p = pes.get(pid)
    if not p or len(p) < 9: return
    sid = 0xE0 if es[pid] in (0x1B, 0x24) else 0xC0
    hl = 9 + p[8]; hdr = bytearray(p[:hl]); body = p[hl:]
    pts = ((p[9] & 0x0E) << 29) | (p[10] << 22) | ((p[11] & 0xFE) << 14) | (p[12] << 7) | (p[13] >> 1) if p[7] & 0x80 else 0
    out.write(scr_pack(max(pts - 9000, 0)))
    if first or npk % 50 == 0: out.write(psm())
    first = False; npk += 1
    hdr[3] = sid; off = 0; firstc = True
    while off < len(body) or firstc:
        if firstc: h = bytes(hdr); room = 65535 - (len(h) - 6)
        else: h = b"\x00\x00\x01" + bytes([sid]) + b"\x00\x00\x80\x00\x00"; room = 65535 - 3
        chunk = body[off:off + room]; off += len(chunk)
        pkt = bytearray(h + chunk); pkt[4:6] = struct.pack(">H", len(pkt) - 6); out.write(pkt); firstc = False
for i in range(0, len(d) - 187, 188):
    p = d[i:i + 188]
    if p[0] != 0x47: continue
    pid = ((p[1] & 0x1F) << 8) | p[2]; pusi = p[1] & 0x40; afc = (p[3] >> 4) & 3
    o = 4 + (1 + p[4] if afc & 2 else 0)
    if not afc & 1 or o >= 188: continue
    pl = p[o:]
    if pid == 0 and pusi:
        pmt_pid = ((pl[1 + 10] & 0x1F) << 8) | pl[1 + 11]
    elif pid == pmt_pid and pusi and not es:
        s = pl[1:]; sl = ((s[1] & 0xF) << 8) | s[2]; pil = ((s[10] & 0xF) << 8) | s[11]; k = 12 + pil
        while k < 3 + sl - 4:
            st = s[k]; epid = ((s[k + 1] & 0x1F) << 8) | s[k + 2]; il = ((s[k + 3] & 0xF) << 8) | s[k + 4]
            if st in (0x1B, 0x0F, 0x24): es[epid] = st
            k += 5 + il
    elif pid in es:
        if pusi: flush(pid); pes[pid] = bytearray(pl)
        elif pid in pes: pes[pid] += pl
for pid in es: flush(pid)
out.write(b"\x00\x00\x01\xb9")
