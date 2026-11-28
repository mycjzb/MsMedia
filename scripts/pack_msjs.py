#!/usr/bin/env python3
"""
打包 JS 规则脚本为 MSJS（AES-256-GCM ± gzip），对齐 so 内 ms_qjs_unwrap。
也可只 gzip：python3 scripts/pack_msjs.py --gzip-only -i a.js -o a.js.gz

固定文件头（加密包必以此开头，便于嗅探）：
  0..3  'M''S''J''S'
  4..5  ver=1 BE
  6..7  flags BE (bit0=GCM, bit2=gzip)
  8..19 iv
  20..23 clen BE
  24..   cipher + tag[16]

脚本上限 512KiB（明文与输出文件均校验）。

  python3 scripts/pack_msjs.py -i samples/js/yspweb.js -o build/yspweb.msjs
  python3 scripts/pack_msjs.py -i samples/js/yspweb.js -o build/yspweb.msjs --no-gzip
"""
from __future__ import annotations

import argparse
import gzip
import os
import struct
import sys

SCRIPT_CAP = 512 * 1024

# 与 src/qjs/ms_qjs_wrap.c 一致
KA = bytes(
    [
        0x4A, 0x91, 0x2E, 0x7C, 0xB3, 0x18, 0xD5, 0x60, 0x0F, 0xA4, 0x57, 0xE2, 0x39, 0x8B, 0xC6, 0x1D,
        0x72, 0xFE, 0x45, 0xA0, 0x2B, 0x96, 0xDB, 0x3C, 0x81, 0xF4, 0x59, 0x0E, 0xC3, 0x68, 0xAD, 0x14,
    ]
)
KB = bytes(
    [
        0xE7, 0x3A, 0x9D, 0x50, 0x0B, 0xC6, 0x71, 0x24, 0xD8, 0x4F, 0xA2, 0x15, 0xB9, 0x6C, 0x01, 0xF3,
        0x5E, 0xA9, 0x34, 0xCF, 0x7A, 0x15, 0xB0, 0x4D, 0xE8, 0x63, 0x2E, 0x91, 0xD4, 0x47, 0x0A, 0xBD,
    ]
)
SALT = bytes([0x5A, 0xC3, 0x17, 0x8E, 0x42, 0xF1, 0x6B, 0x29])


def assemble_key() -> bytes:
    return bytes(KA[i] ^ KB[i] ^ SALT[i % 8] for i in range(32))


def pack_msjs(plain: bytes, use_gzip: bool) -> bytes:
    try:
        from cryptography.hazmat.primitives.ciphers.aead import AESGCM
    except ImportError as e:
        raise SystemExit("需要 cryptography：pip install cryptography") from e
    body = gzip.compress(plain, compresslevel=9) if use_gzip else plain
    flags = 0x1 | (0x4 if use_gzip else 0)  # GCM 必置；可选 gzip
    iv = os.urandom(12)
    ct = AESGCM(assemble_key()).encrypt(iv, body, None)
    # cryptography 把 tag 附在末尾 16 字节
    if len(ct) < 16:
        raise RuntimeError("gcm short")
    cipher, tag = ct[:-16], ct[-16:]
    # 固定头：MSJS + ver=1 + flags + iv + clen
    return b"MSJS" + struct.pack(">HH", 1, flags) + iv + struct.pack(">I", len(cipher)) + cipher + tag


def main() -> None:
    ap = argparse.ArgumentParser(description="pack JS → MSJS / gzip")
    ap.add_argument("-i", "--in", required=True, dest="inp")
    ap.add_argument("-o", "--out", required=True)
    ap.add_argument("--no-gzip", action="store_true", help="MSJS 内不 gzip")
    ap.add_argument("--gzip-only", action="store_true", help="只输出裸 gzip，不加密")
    args = ap.parse_args()
    plain = open(args.inp, "rb").read()
    if len(plain) > SCRIPT_CAP:
        print(f"error: 明文超过 {SCRIPT_CAP} 字节", file=sys.stderr)
        sys.exit(2)
    if b"function main" not in plain and b"async function main" not in plain:
        print("error: 正文须含 function main", file=sys.stderr)
        sys.exit(2)
    if args.gzip_only:
        data = gzip.compress(plain, compresslevel=9)
    else:
        data = pack_msjs(plain, use_gzip=not args.no_gzip)
        if data[:4] != b"MSJS" or data[4:6] != b"\x00\x01":
            print("error: 固定头异常", file=sys.stderr)
            sys.exit(3)
    if len(data) > SCRIPT_CAP:
        print(f"error: 输出超过 {SCRIPT_CAP} 字节", file=sys.stderr)
        sys.exit(2)
    os.makedirs(os.path.dirname(os.path.abspath(args.out)) or ".", exist_ok=True)
    open(args.out, "wb").write(data)
    head = data[:4]
    print(f"ok {args.out} ({len(data)} bytes) head={head!r} from {args.inp} ({len(plain)} plain)")


if __name__ == "__main__":
    main()
