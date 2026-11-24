#!/usr/bin/env python3
# 去掉 .class 里方法的 MethodParameters 属性（新 JDK 给匿名内部类构造器写入无名参数，
# build-tools 34 的 d8 8.2 遇到会内部 NPE）。只删该调试属性，不改字节码。
# 用法：strip_method_params.py <classes_dir>
# by: Long <mycjzb@gmail.com>
import os
import struct
import sys


def strip(data):
    off = 10
    count = struct.unpack_from(">H", data, 8)[0]
    utf8 = {}
    i = 1
    while i < count:
        tag = data[off]
        if tag == 1:
            ln = struct.unpack_from(">H", data, off + 1)[0]
            utf8[i] = data[off + 3:off + 3 + ln].decode("utf-8", "replace")
            off += 3 + ln
        elif tag in (3, 4, 9, 10, 11, 12, 17, 18):
            off += 5
        elif tag in (5, 6):
            off += 9
            i += 1
        elif tag in (7, 8, 16, 19, 20):
            off += 3
        elif tag == 15:
            off += 4
        else:
            raise ValueError("bad cp tag %d" % tag)
        i += 1
    out = bytearray(data[:off])
    p = off + 6
    n_if = struct.unpack_from(">H", data, p)[0]
    p += 2 + 2 * n_if
    out += data[off:p]

    def members(p, drop):
        n = struct.unpack_from(">H", data, p)[0]
        res = bytearray(data[p:p + 2])
        p += 2
        removed = 0
        for _ in range(n):
            res += data[p:p + 6]
            na = struct.unpack_from(">H", data, p + 6)[0]
            p += 8
            keep = []
            for _ in range(na):
                ni, ln = struct.unpack_from(">HI", data, p)
                blob = data[p:p + 6 + ln]
                p += 6 + ln
                if drop and utf8.get(ni) == "MethodParameters":
                    removed += 1
                    continue
                keep.append(blob)
            res += struct.pack(">H", len(keep))
            for b in keep:
                res += b
        return p, res, removed

    p, f, _ = members(p, False)
    out += f
    p, m, removed = members(p, True)
    out += m
    out += data[p:]
    return bytes(out), removed


def main():
    total = 0
    for root, _, files in os.walk(sys.argv[1]):
        for fn in files:
            if fn.endswith(".class"):
                path = os.path.join(root, fn)
                with open(path, "rb") as fp:
                    d = fp.read()
                nd, r = strip(d)
                if r:
                    with open(path, "wb") as fp:
                        fp.write(nd)
                    total += r
    print("strip_method_params: removed %d attribute(s)" % total)


if __name__ == "__main__":
    main()
