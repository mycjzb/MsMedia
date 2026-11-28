#!/usr/bin/env python3
"""对照 ContentSniff 规则的轻量自检（不跑 JVM）。"""

def sniff(data: bytes, eof: bool) -> str:
    b = data
    n = len(b)
    if n <= 0:
        return "PROG" if eof else "MORE"
    i = 0
    if n >= 3 and b[0:3] == b"\xef\xbb\xbf":
        i = 3
    while i < n and b[i] in b" \t\r\n":
        i += 1
    if i >= n:
        return "PROG" if eof else "MORE"
    if n - i >= 188 and b[i] == 0x47:
        return "PROG"
    if n - i >= 8 and b[i + 4 : i + 8] == b"ftyp":
        return "PROG"
    if n - i >= 2 and b[i] == 0x1F and b[i + 1] == 0x8B:
        return "JS"
    if n - i >= 4 and b[i : i + 4] == b"MSJS":
        return "JS"
    if b[i:].startswith(b"#EXTM3U"):
        return "HLS"
    if b[i] == ord("<") and (b"<MPD" in b[i:] or b"<mpd" in b[i:]):
        return "DASH"
    if b"function main" in b[i:] or b"async function main" in b[i:]:
        return "JS"
    if not eof and n < 8192 and b"function" in b[i:]:
        return "MORE"
    return "PROG"


def main() -> None:
    cases = [
        (b"#EXTM3U\n#EXTINF", True, "HLS"),
        (b'{"a":1}', True, "PROG"),
        (b"function main(item){return 'http://x';}", True, "JS"),
        (b"// hi\nfunction main(item){}", True, "JS"),
        (b"MSJS" + b"\x00" * 8, True, "JS"),
        (b"\x1f\x8b\x08" + b"\x00" * 8, True, "JS"),
        (b"\x47" + b"\x00" * 187, True, "PROG"),
        (b"function ", False, "MORE"),
    ]
    for data, eof, exp in cases:
        got = sniff(data, eof)
        assert got == exp, (data[:40], eof, got, exp)
    print("content_sniff_js_check: PASS")


if __name__ == "__main__":
    main()
