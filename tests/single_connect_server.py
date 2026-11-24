#!/usr/bin/env python3
# 单次建连核验：静态文件 HTTP 服务（支持 Range），统计 TCP 连接数与 GET 次数；/__stats 返回计数（不计入）。
# 只监听 127.0.0.1。by: Long <mycjzb@gmail.com>
import os, sys, threading, socketserver
from http.server import SimpleHTTPRequestHandler, HTTPServer
ST = {"conns": 0, "gets": 0, "ranges": []}
class H(SimpleHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    def log_message(self, *a): pass
    def setup(self):
        super().setup()
        self._counted = False
    def do_GET(self):
        if self.path == "/__stats":
            b = ("%d %d %s" % (ST["conns"], ST["gets"], ",".join(ST["ranges"]) or "-")).encode()
            self.send_response(200); self.send_header("Content-Length", str(len(b))); self.end_headers(); self.wfile.write(b); return
        if self.path == "/__reset":
            ST["conns"] = 0; ST["gets"] = 0; ST["ranges"] = []
            self.send_response(200); self.send_header("Content-Length", "0"); self.end_headers(); return
        if not self._counted:
            ST["conns"] += 1; self._counted = True
        ST["gets"] += 1
        ST["ranges"].append(self.headers.get("Range", "none").replace(",", ";"))
        path = self.translate_path(self.path)
        size = os.path.getsize(path)
        rng = self.headers.get("Range")
        start, end = 0, size - 1
        if rng and rng.startswith("bytes="):
            a, _, b = rng[6:].partition("-")
            start = int(a or 0); end = int(b) if b else size - 1
            self.send_response(206); self.send_header("Content-Range", "bytes %d-%d/%d" % (start, end, size))
        else:
            self.send_response(200)
        self.send_header("Accept-Ranges", "bytes")
        self.send_header("Content-Length", str(end - start + 1)); self.end_headers()
        with open(path, "rb") as f:
            f.seek(start); left = end - start + 1
            try:
                while left > 0:
                    d = f.read(min(65536, left))
                    if not d: break
                    self.wfile.write(d); left -= len(d)
            except Exception:
                pass
class TS(socketserver.ThreadingMixIn, HTTPServer):
    daemon_threads = True
    allow_reuse_address = True
os.chdir(sys.argv[1])
s = TS(("127.0.0.1", int(sys.argv[2]) if len(sys.argv) > 2 else 0), H)
threading.Thread(target=s.serve_forever, daemon=True).start()
print(s.server_address[1], flush=True)
sys.stdin.read()
