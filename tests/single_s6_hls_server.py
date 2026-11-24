#!/usr/bin/env python3
# 阶段 6 宿主/模拟器专项测试服务：Media3 风格 HLS 预下载 + AES-128 + 自签 HTTPS 隔离。
# 只监听 127.0.0.1（模拟器经 10.0.2.2 访问时用 --bind 0.0.0.0）。
#  gen <src_net_dir> <out_dir>     由 s4 夹具生成：aes(显式 IV)/aes_seq(隐式 IV=序号)/br(EXT-X-BYTERANGE 单文件)/
#                                  fmp4(EXT-X-MAP)/png(伪装 .png 切片)/png_mkv.bin 等
#  serve <dir> <http_port> <https_port> [--bind ADDR]
#      /live/index.m3u8   —— 滑动窗口直播（每 2s 前进一片，循环用 hls_ts 的切片）
#      /flaky/<path>      —— 每个路径第 1 次请求只发一半就断开（模拟弱网），之后正常
#      /fail404/<path>    —— 永远 404（验证 4xx 不重试、错误上报）
#      /slow/<path>       —— 每 16KB 停 50ms（验证边下边播/取消时延）
#      /stats             —— 返回 JSON：TCP 连接数 / 请求数（验证 Keep-Alive 复用）
#  HTTPS 端口用启动时现生成的自签证书（SAN 只有 IP:127.0.0.1，不在任何信任库里；用 localhost 访问可验证主机名校验）。
# by: Long <mycjzb@gmail.com>
import os, sys, ssl, json, time, shutil, threading, subprocess, http.server, socketserver

def gen(src, out):
    from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
    from cryptography.hazmat.primitives import padding
    os.makedirs(out, exist_ok=True)
    for d in ("hls_ts", "hls_fmp4", "hls_png", "hls_fmp4_jpg"):
        if os.path.isdir(os.path.join(src, d)):
            shutil.copytree(os.path.join(src, d), os.path.join(out, d), dirs_exist_ok=True)
    for f in ("png_mkv.bin", "prog_png_ts.bin", "plain.mkv"):
        if os.path.exists(os.path.join(src, f)):
            shutil.copy(os.path.join(src, f), out)
    segs = sorted(x for x in os.listdir(os.path.join(src, "hls_ts")) if x.endswith(".ts"))
    key = bytes(range(16))
    def enc(data, iv):
        p = padding.PKCS7(128).padder(); d = p.update(data) + p.finalize()
        e = Cipher(algorithms.AES(key), modes.CBC(iv)).encryptor(); return e.update(d) + e.finalize()
    for name, explicit in (("aes", True), ("aes_seq", False)):
        od = os.path.join(out, name); os.makedirs(od, exist_ok=True)
        open(os.path.join(od, "key.bin"), "wb").write(key)
        pl = ["#EXTM3U", "#EXT-X-VERSION:3", "#EXT-X-TARGETDURATION:2", "#EXT-X-MEDIA-SEQUENCE:0"]
        for i, s in enumerate(segs):
            iv = bytes([0xA5] * 8) + i.to_bytes(8, "big") if explicit else i.to_bytes(16, "big")
            line = '#EXT-X-KEY:METHOD=AES-128,URI="key.bin"'
            if explicit: line += ",IV=0x" + iv.hex()
            pl += [line, "#EXTINF:2.000000,", "e%03d.dat" % i]
            open(os.path.join(od, "e%03d.dat" % i), "wb").write(enc(open(os.path.join(src, "hls_ts", s), "rb").read(), iv))
        pl.append("#EXT-X-ENDLIST")
        open(os.path.join(od, "index.m3u8"), "w").write("\n".join(pl) + "\n")
    # EXT-X-BYTERANGE：所有切片拼成一个文件，偏移连续
    od = os.path.join(out, "br"); os.makedirs(od, exist_ok=True)
    blob = b""; pl = ["#EXTM3U", "#EXT-X-VERSION:4", "#EXT-X-TARGETDURATION:2", "#EXT-X-MEDIA-SEQUENCE:0"]
    for s in segs:
        d = open(os.path.join(src, "hls_ts", s), "rb").read()
        pl += ["#EXTINF:2.000000,", "#EXT-X-BYTERANGE:%d@%d" % (len(d), len(blob)), "all.bin"]
        blob += d
    pl.append("#EXT-X-ENDLIST")
    open(os.path.join(od, "all.bin"), "wb").write(blob)
    open(os.path.join(od, "index.m3u8"), "w").write("\n".join(pl) + "\n")
    print("gen ok", out)

STATS = {"conns": 0, "reqs": 0}
FLAKY_SEEN = set()
LOCK = threading.Lock()
T0 = time.time()

class H(http.server.SimpleHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    def log_message(self, *a): pass
    def setup(self):
        super().setup()
        with LOCK: STATS["conns"] += 1
    def send_bytes(self, data, ctype="application/octet-stream", slow=False, cut=False):
        rng = self.headers.get("Range"); code = 200; start, end = 0, len(data) - 1
        if rng and rng.startswith("bytes="):
            a, b = rng[6:].split("-", 1)
            start = int(a) if a else 0; end = int(b) if b else len(data) - 1
            end = min(end, len(data) - 1); code = 206
        body = data[start:end + 1]
        self.send_response(code); self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        if code == 206: self.send_header("Content-Range", "bytes %d-%d/%d" % (start, end, len(data)))
        self.send_header("Accept-Ranges", "bytes"); self.end_headers()
        if self.command == "HEAD": return
        if cut:
            self.wfile.write(body[:len(body) // 2]); self.wfile.flush(); self.close_connection = True
            try: self.connection.shutdown(2)
            except OSError: pass
            return
        if slow:
            for i in range(0, len(body), 16384):
                self.wfile.write(body[i:i + 16384]); self.wfile.flush(); time.sleep(0.05)
        else:
            self.wfile.write(body)
    def do_HEAD(self): self.do_GET()
    def do_GET(self):
        with LOCK: STATS["reqs"] += 1
        p = self.path.split("?", 1)[0]
        if p == "/stats":
            return self.send_bytes(json.dumps(STATS).encode(), "application/json")
        if p == "/live/index.m3u8":
            n = int((time.time() - T0) / 2)
            pl = ["#EXTM3U", "#EXT-X-VERSION:3", "#EXT-X-TARGETDURATION:2", "#EXT-X-MEDIA-SEQUENCE:%d" % n]
            for i in range(n, n + 3): pl += ["#EXTINF:2.000000,", "/hls_ts/s%03d.ts" % (i % 5)]
            return self.send_bytes(("\n".join(pl) + "\n").encode(), "application/vnd.apple.mpegurl")
        slow = cut = False
        if p.startswith("/fail404/"):
            self.send_response(404); self.send_header("Content-Length", "0"); self.end_headers(); return
        if p.startswith("/slow/"): slow = True; p = p[5:]
        if p.startswith("/flaky/"):
            p = p[6:]
            with LOCK:
                if p not in FLAKY_SEEN and not p.endswith(".m3u8"): FLAKY_SEEN.add(p); cut = True
        fp = os.path.join(ROOT, p.lstrip("/"))
        if not os.path.isfile(fp):
            self.send_response(404); self.send_header("Content-Length", "0"); self.end_headers(); return
        ctype = "application/vnd.apple.mpegurl" if fp.endswith(".m3u8") else "application/octet-stream"
        self.send_bytes(open(fp, "rb").read(), ctype, slow, cut)

class TS(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True; allow_reuse_address = True; request_queue_size = 64

def selfsigned(d):
    crt, key = os.path.join(d, "selfsigned.crt"), os.path.join(d, "selfsigned.key")
    if not os.path.exists(crt):
        subprocess.check_call(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "30",
                               "-subj", "/CN=127.0.0.1", "-addext", "subjectAltName=IP:127.0.0.1",
                               "-addext", "basicConstraints=critical,CA:TRUE", "-keyout", key, "-out", crt],
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return crt, key

if __name__ == "__main__":
    if sys.argv[1] == "gen":
        gen(sys.argv[2], sys.argv[3]); sys.exit(0)
    ROOT = sys.argv[2]; hp, sp = int(sys.argv[3]), int(sys.argv[4])
    bind = sys.argv[sys.argv.index("--bind") + 1] if "--bind" in sys.argv else "127.0.0.1"
    h = TS((bind, hp), H); s = TS((bind, sp), H)
    crt, key = selfsigned(ROOT)
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER); ctx.load_cert_chain(crt, key)
    s.socket = ctx.wrap_socket(s.socket, server_side=True)
    threading.Thread(target=s.serve_forever, daemon=True).start()
    print("serving", ROOT, hp, sp, flush=True)
    h.serve_forever()
