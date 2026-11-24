#!/usr/bin/env python3
# 宿主 netcore 测试用本地服务：HTTP（普通/chunked/重定向/POST 回显/慢速/计连接数）+ 最小 SOCKS5（含用户名密码）。
# 只监听 127.0.0.1。by: Long <mycjzb@gmail.com>
import socket, socketserver, threading, sys, time, struct
from http.server import BaseHTTPRequestHandler, HTTPServer

CONNS = {"n": 0}

class H(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    def log_message(self, *a):
        pass
    def setup(self):
        CONNS["n"] += 1
        super().setup()
    def do_GET(self):
        p = self.path
        if p == "/plain":
            b = b"hello-netcore" * 100
            self.send_response(200); self.send_header("Content-Length", str(len(b))); self.send_header("X-Test", "1"); self.end_headers(); self.wfile.write(b)
        elif p == "/chunked":
            self.send_response(200); self.send_header("Transfer-Encoding", "chunked"); self.end_headers()
            for part in [b"abc", b"defgh", b"x" * 5000]:
                self.wfile.write(b"%x\r\n" % len(part) + part + b"\r\n")
            self.wfile.write(b"0\r\n\r\n")
        elif p == "/redir":
            self.send_response(302); self.send_header("Location", "/plain"); self.send_header("Content-Length", "0"); self.end_headers()
        elif p == "/loop":
            self.send_response(302); self.send_header("Location", "/loop"); self.send_header("Content-Length", "0"); self.end_headers()
        elif p == "/to-http-from-https":
            self.send_response(302); self.send_header("Location", "http://127.0.0.1/"); self.send_header("Content-Length", "0"); self.end_headers()
        elif p == "/slow":
            self.send_response(200); self.send_header("Content-Length", "1000000"); self.end_headers()
            try:
                for _ in range(100):
                    self.wfile.write(b"z" * 10); self.wfile.flush(); time.sleep(0.2)
            except Exception:
                pass
        elif p == "/hdr":
            v = self.headers.get("X-Custom", "")
            b = ("custom=" + v).encode()
            self.send_response(200); self.send_header("Content-Length", str(len(b))); self.end_headers(); self.wfile.write(b)
        elif p == "/conns":
            b = str(CONNS["n"]).encode()
            self.send_response(200); self.send_header("Content-Length", str(len(b))); self.end_headers(); self.wfile.write(b)
        elif p == "/close":
            self.send_response(200); self.send_header("Connection", "close"); self.end_headers(); self.wfile.write(b"until-close"); self.close_connection = True
        else:
            self.send_response(404); self.send_header("Content-Length", "0"); self.end_headers()
    def do_POST(self):
        n = int(self.headers.get("Content-Length", "0"))
        b = self.rfile.read(n)
        self.send_response(200); self.send_header("Content-Length", str(len(b) + 5)); self.end_headers(); self.wfile.write(b"echo:" + b)

class TS(socketserver.ThreadingMixIn, HTTPServer):
    daemon_threads = True
    allow_reuse_address = True

SOCKS = {"n": 0, "last_atyp": 0}

def pipe(a, b):
    try:
        while True:
            d = a.recv(65536)
            if not d:
                break
            b.sendall(d)
    except Exception:
        pass
    finally:
        try: b.shutdown(socket.SHUT_WR)
        except Exception: pass

class S5(socketserver.BaseRequestHandler):
    def handle(self):
        c = self.request
        SOCKS["n"] += 1
        ver, nm = c.recv(2)
        methods = c.recv(nm)
        need_auth = self.server.auth is not None
        if need_auth:
            if 2 not in methods:
                c.sendall(b"\x05\xff"); return
            c.sendall(b"\x05\x02")
            v = c.recv(1); ul = c.recv(1)[0]; u = c.recv(ul); pl = c.recv(1)[0]; pw = c.recv(pl)
            ok = (u.decode(), pw.decode()) == self.server.auth
            c.sendall(b"\x01\x00" if ok else b"\x01\x01")
            if not ok: return
        else:
            c.sendall(b"\x05\x00")
        hdr = c.recv(4)
        atyp = hdr[3]; SOCKS["last_atyp"] = atyp
        if atyp == 1:
            host = socket.inet_ntoa(c.recv(4))
        elif atyp == 3:
            l = c.recv(1)[0]; host = c.recv(l).decode()
            if host == "mapped.test": host = "127.0.0.1"
        else:
            c.sendall(b"\x05\x08\x00\x01" + b"\0" * 6); return
        port = struct.unpack(">H", c.recv(2))[0]
        try:
            u = socket.create_connection((host, port), 3)
        except Exception:
            c.sendall(b"\x05\x05\x00\x01" + b"\0" * 6); return
        c.sendall(b"\x05\x00\x00\x01" + b"\0" * 6)
        t = threading.Thread(target=pipe, args=(u, c), daemon=True); t.start()
        pipe(c, u); t.join(5)

class S5S(socketserver.ThreadingMixIn, socketserver.TCPServer):
    daemon_threads = True
    allow_reuse_address = True

class Stat(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    def log_message(self, *a): pass
    def do_GET(self):
        b = ("%d %d" % (SOCKS["n"], SOCKS["last_atyp"])).encode()
        self.send_response(200); self.send_header("Content-Length", str(len(b))); self.end_headers(); self.wfile.write(b)

if __name__ == "__main__":
    http = TS(("127.0.0.1", 0), H)
    s5 = S5S(("127.0.0.1", 0), S5); s5.auth = None
    s5a = S5S(("127.0.0.1", 0), S5); s5a.auth = ("user", "pass")
    st = TS(("127.0.0.1", 0), Stat)
    for srv in (http, s5, s5a, st):
        threading.Thread(target=srv.serve_forever, daemon=True).start()
    print(http.server_address[1], s5.server_address[1], s5a.server_address[1], st.server_address[1], flush=True)
    sys.stdin.read()
