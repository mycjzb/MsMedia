#!/usr/bin/env python3
"""压 Android .so：直接调用超级直播 bs-player/scripts/pack_apk_so.py 的 pack_blob。

与 bs-mycjzb/scripts/build.sh --pack 1 一致：
  MS_SO_LZMA=0（走 UPX，不是自写壳）
  v7a → upx396 --android-shlib --best --no-filter（NRV）
  v8a → 同脚本 PACKER_PATHS_64 + 默认 --lzma
  压完 strip_upx_marks；arm64 做 16KB / RELRO 收口

    python3 scripts/pack_android_so.py path/to/libmsplayer.so [--abi armeabi-v7a]
环境：
  MS_BS_PLAYER_SCRIPTS  默认 /home/long/code/bs-p2p/bs-player/scripts
  MS_SO_KEEP_UPX_SIG=1  保留 UPX 特征（交给 pack_apk_so）
  MS_SO_UPX_LZMA=0/1    交给 pack_apk_so（勿在本仓另发明默认）
"""
from __future__ import print_function

import argparse
import os
import shutil
import struct
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
BS = os.environ.get(
    "MS_BS_PLAYER_SCRIPTS", "/home/long/code/bs-p2p/bs-player/scripts"
)
if not os.path.isdir(BS):
    sys.stderr.write("error: 超级直播 scripts 不在 %s\n" % BS)
    sys.exit(2)
if BS not in sys.path:
    sys.path.insert(0, BS)

# 与 mycjzb build.sh 一样：UPX 路径，禁止自写 LZMA 壳
os.environ["MS_SO_LZMA"] = "0"

import pack_apk_so as pack  # noqa: E402  真源：超级直播


def elf_machine(data):
    if len(data) < 20 or data[:4] != b"\x7fELF":
        raise ValueError("not ELF")
    endian = "<" if data[5] == 1 else ">"
    (machine,) = struct.unpack_from(endian + "H", data, 18)
    return machine


def guess_abi(path, data):
    m = elf_machine(data)
    if m == 40:
        return "armeabi-v7a"
    if m == 183:
        return "arm64-v8a"
    if m == 3:
        return "x86"
    if m == 62:
        return "x86_64"
    raise ValueError("%s: unknown ELF machine %d" % (path, m))


def count_pt_load(data):
    return sum(1 for p in pack.elf_phdrs(data) if p[0] == pack.PT_LOAD)


def pack_one(path, abi=None, keep_orig=True, strict=False):
    with open(path, "rb") as f:
        blob = f.read()
    if b"UPX!" in blob[: min(len(blob), 4096)] or b"UPX!" in blob[-64:]:
        print("skip already packed: %s" % path)
        return 0
    # 超级直播 libaliiptv：压前必须 2×LOAD（bfd+Hardening）。lld 4×LOAD 上 UPX
    # --android-shlib 会出「能过校验但海思 dlopen 闪退」的残片，禁止硬压。
    nload = count_pt_load(blob)
    if nload != 2:
        msg = (
            "%s: PT_LOAD=%d（要 2，对齐超级直播）。先非 LTO 依赖 + bfd Hardening 再压"
            % (path, nload)
        )
        if strict:
            print("error: %s" % msg, file=sys.stderr)
            return 1
        print("warning: %s；保持原样" % msg, file=sys.stderr)
        return 0
    abi = abi or guess_abi(path, blob)
    page = pack.PAGE_64 if abi in pack.ABI_64 else pack.PAGE_32
    packer = pack.find_packer(
        pack.PACKER_PATHS_64 if abi in pack.ABI_64 else pack.PACKER_PATHS_32
    )
    if not packer:
        msg = "找不到包器（abi=%s）" % abi
        if strict:
            print("error: %s" % msg, file=sys.stderr)
            return 1
        print("warning: %s；保持原样" % msg, file=sys.stderr)
        return 0
    work = tempfile.mkdtemp(prefix="ms_pack_so_")
    try:
        out, reason = pack.pack_blob(packer, blob, work, page)
    finally:
        shutil.rmtree(work, ignore_errors=True)
    if out is None:
        if strict:
            print("error: %s pack fail: %s" % (path, reason), file=sys.stderr)
            return 1
        print("warning: %s 不压：%s" % (path, reason), file=sys.stderr)
        return 0
    if keep_orig:
        shutil.copy2(path, path + ".unpacked")
    with open(path, "wb") as f:
        f.write(out)
    print(
        "so pack: %s abi=%s packer=%s %d -> %d (%.0f%%) [pack_apk_so]"
        % (path, abi, packer, len(blob), len(out), 100.0 * len(out) / len(blob))
    )
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("paths", nargs="*")
    ap.add_argument("--dir", help="扫描目录下 *.so")
    ap.add_argument("--abi")
    ap.add_argument("--strict", action="store_true")
    ap.add_argument("--no-keep-orig", action="store_true")
    args = ap.parse_args()
    paths = list(args.paths)
    if args.dir:
        for root, _dirs, files in os.walk(args.dir):
            for name in files:
                if name.endswith(".so") and not name.endswith(".unpacked.so"):
                    paths.append(os.path.join(root, name))
    if not paths:
        ap.error("need path(s) or --dir")
    rc = 0
    for p in paths:
        rc |= pack_one(
            p, abi=args.abi, keep_orig=not args.no_keep_orig, strict=args.strict
        )
    return rc


if __name__ == "__main__":
    sys.exit(main())
