#!/usr/bin/env python3
# 阶段 5：AVOption 生效性探针服务器（宿主 HTTP，记录每个请求的方法/路径/关键头，不记录完整私有 URL）。
#  /cut/<file>   发送 64KB 后断开 → 验证 reconnect/reconnect_streamed 生效（应出现带 Range 的续传请求）
#  /stall/<x>    接受连接后永不应答 → 验证 rw_timeout/timeout 生效（客户端应在超时后放弃，不会无限挂起）
#  /<file>       正常 200/206，记录 User-Agent / Connection / Range（multiple_requests 体现为 keep-alive）
# 用法：single_s5_optprobe_server.py <样本目录> <端口> <日志文件>   by: Long <mycjzb@gmail.com>
import http.server, socketserver, sys, os, time, threading
root, port, logf = sys.argv[1], int(sys.argv[2]), sys.argv[3]
lock = threading.Lock()
def log(s):
    with lock, open(logf, "a") as f:
        f.write(time.strftime("%H:%M:%S ") + s + "\n")
class H(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    def log_message(self, *a): pass
    def do_GET(self):
        p = self.path.split("?")[0]
        log("GET %s ua=%r conn=%r range=%r" % (p, self.headers.get("User-Agent"), self.headers.get("Connection"), self.headers.get("Range")))
        if p.startswith("/stall/"):
            time.sleep(120); return
        cut = p.startswith("/cut/")
        name = os.path.basename(p)
        fp = os.path.join(root, name)
        if not os.path.isfile(fp):
            self.send_response(404); self.send_header("Content-Length", "0"); self.end_headers(); return
        b = open(fp, "rb").read()
        start = 0
        rng = self.headers.get("Range")
        if rng and rng.startswith("bytes="):
            start = int(rng[6:].split("-")[0] or 0)
        part = b[start:]
        if start:
            self.send_response(206); self.send_header("Content-Range", "bytes %d-%d/%d" % (start, len(b) - 1, len(b)))
        else:
            self.send_response(200)
        self.send_header("Accept-Ranges", "bytes"); self.send_header("Content-Length", str(len(part))); self.end_headers()
        if cut and start == 0:
            n = min(65536, len(b) // 2)  # 尽早断开，让探测阶段就触发续传
            self.wfile.write(part[:n]); self.wfile.flush(); log("CUT after %d bytes" % n)
            self.close_connection = True
            try: self.connection.shutdown(2)
            except Exception: pass
            return
        self.wfile.write(part)
class S(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True; allow_reuse_address = True
S(("127.0.0.1", port), H).serve_forever()
