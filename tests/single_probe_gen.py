#!/usr/bin/env python3
# 生成「图片伪装头 + 脏字节 + 真实媒体」样本与期望偏移清单，供 single_probe_test.c 校验。
# by: Long <mycjzb@gmail.com>
import os, struct, sys, zlib

src, out = sys.argv[1], sys.argv[2]
os.makedirs(out, exist_ok=True)

def chunk(t, d):
    return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)

def png():
    ihdr = struct.pack(">IIBBBBB", 2, 2, 8, 2, 0, 0, 0)
    raw = b"".join(b"\x00" + b"\x10\x20\x47" * 2 for _ in range(2))
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) + chunk(b"tEXt", b"k\x00\x47\x47\x47") + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b"")

def jpeg():
    seg = lambda m, d: b"\xff" + bytes([m]) + struct.pack(">H", len(d) + 2) + d
    ent = b"\x12\xff\x00\x47\xff\xd0\x47\x33\xff\x00"  # 填充 FF00、RST、伪 0x47
    return b"\xff\xd8" + seg(0xE0, b"JFIF\x00\x01\x01\x00\x00\x01\x00\x01\x00\x00") + seg(0xDB, bytes(65)) + seg(0xDA, bytes(10)) + ent + b"\xff\xd9"

def gif():
    g = b"GIF89a" + struct.pack("<HH", 1, 1) + b"\x80\x00\x00" + b"\x00\x00\x00\xff\xff\xff"
    g += b"\x21\xf9\x04\x00\x00\x00\x00\x00"
    g += b"\x2c" + struct.pack("<HHHH", 0, 0, 1, 1) + b"\x00" + b"\x02\x02\x44\x01\x00"
    return g + b"\x3b"

def bmp():
    pix = b"\x00\x00\xff\x00"
    hdr = b"BM" + struct.pack("<IHHI", 14 + 40 + len(pix), 0, 0, 54)
    return hdr + struct.pack("<IiiHHIIiiII", 40, 1, 1, 1, 32, 0, len(pix), 0, 0, 0, 0) + pix

def webp():
    body = b"WEBP" + b"VP8L" + struct.pack("<I", 5) + b"\x2f\x00\x00\x00\x00" + b"\x00"
    return b"RIFF" + struct.pack("<I", len(body)) + body

junk = b"\x00\x13garbage\x47\x47 not ts \x00\x00\x01"
media = {
    "ts": ("h264_aac.ts", "ts"),
    "mp4": ("h264_aac.mp4", "bmff"),
    "flv": ("h264_aac.flv", "flv"),
    "mkv": ("h264_alaw.mkv", "mkv"),
}
lines = []
imgs = {"png": png(), "jpeg": jpeg(), "gif": gif(), "bmp": bmp(), "webp": webp(), "none": b""}
for mk, (fn, fmt) in media.items():
    data = open(os.path.join(src, fn), "rb").read()
    for ik, img in imgs.items():
        for j in (0, 1):
            pre = img + (junk if j else b"")
            if not pre:
                continue
            name = f"{ik}{'_junk' if j else ''}_{mk}.bin"
            open(os.path.join(out, name), "wb").write(pre + data)
            lines.append(f"{name} {len(pre)} {fmt} {ik} {fn}")
# m4s：取 mp4 里第一个 moof 起的片段（若有）否则跳过
mp4 = open(os.path.join(src, "h264_aac.mp4"), "rb").read()
# 双图叠加：PNG+JPEG 再接 TS
ts = open(os.path.join(src, "h264_aac.ts"), "rb").read()
open(os.path.join(out, "png_jpeg_ts.bin"), "wb").write(png() + jpeg() + ts)
lines.append(f"png_jpeg_ts.bin {len(png()) + len(jpeg())} ts png h264_aac.ts")
# 192 字节 M2TS
m2ts = b"".join(b"\x00\x00\x00\x00" + ts[i:i + 188] for i in range(0, 188 * 200, 188))
open(os.path.join(out, "png_m2ts.bin"), "wb").write(png() + m2ts)
lines.append(f"png_m2ts.bin {len(png())} ts192 png -")
# HLS / DASH 文本（带 BOM）
open(os.path.join(out, "bom_hls.bin"), "wb").write(b"\xef\xbb\xbf#EXTM3U\n#EXT-X-VERSION:3\n")
lines.append("bom_hls.bin 0 hls none -")
open(os.path.join(out, "xml_dash.bin"), "wb").write(b'<?xml version="1.0"?>\n<MPD xmlns="urn:mpeg:dash:schema:mpd:2011"></MPD>')
lines.append("xml_dash.bin 0 dash none -")
open(os.path.join(out, "MANIFEST"), "w").write("\n".join(lines) + "\n")
print(f"generated {len(lines)} cases")
