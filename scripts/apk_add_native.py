#!/usr/bin/env python3
# 向未签名 APK 添加 classes.dex 与 native .so。
# 若 so 的父目录是已知 ABI（armeabi-v7a / arm64-v8a 等），按该 ABI 放入 lib/<abi>/；
# 否则回退到 lib/armeabi-v7a/（兼容旧调用）。
# by: Long <mycjzb@gmail.com>
import sys
import os
import zipfile

KNOWN_ABI = ("armeabi-v7a", "arm64-v8a", "x86", "x86_64")

def abi_arc(so):
    parent = os.path.basename(os.path.dirname(os.path.abspath(so)))
    base = os.path.basename(so)
    if parent in KNOWN_ABI:
        return "lib/%s/%s" % (parent, base)
    return "lib/armeabi-v7a/" + base

def main():
    if len(sys.argv) < 4:
        print("usage: apk_add_native.py apk classes.dex so [so...]",
              file=sys.stderr)
        return 2
    apk, dex = sys.argv[1], sys.argv[2]
    sos = sys.argv[3:]
    with zipfile.ZipFile(apk, "a") as z:
        names = set(z.namelist())
        if os.path.isfile(dex) and "classes.dex" not in names:
            z.write(dex, "classes.dex", compress_type=zipfile.ZIP_DEFLATED)
        for so in sos:
            if not os.path.isfile(so):
                print("error: missing " + so, file=sys.stderr)
                return 2
            arc = abi_arc(so)
            # 本脚本在空 apk 上追加；若已存在则跳过以免重复条目。
            if arc not in names:
                z.write(so, arc, compress_type=zipfile.ZIP_STORED)
                names.add(arc)
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
