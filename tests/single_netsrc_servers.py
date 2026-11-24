#!/usr/bin/env python3
# 宿主 netsrc 测试本地服务（只监听 127.0.0.1）：
#   http <dir> <port>          ：HTTP/1.1 Keep-Alive 静态文件 + /slow（取消测试）+ /stats（连接计数）
#   rtmp <flv> <port>          ：最小 RTMP 服务（握手/connect/createStream/play，FLV 标签按块发送，含聚合消息）
#   rtsp <flv> <ts> <port>     ：最小 RTSP 服务（/ts=MP2T over RTP，/es=H264 FU-A/STAP-A + AAC RFC3640，/auth=Digest）
#   junk <port>                ：畸形 RTMP/RTSP 应答（超大长度/超长头/乱块/坏 SDP/半截断开）
#   http 额外：/cut/<file> 中途断连，/hugelen 伪造超大 Content-Length
#   udp-send <file> <port> raw|rtp ：按 TS 包 7 个一组发 UDP（可选 RTP 头，含一次乱序/重复）
# by: Long <mycjzb@gmail.com>
import os, sys, socket, socketserver, struct, threading, time, hashlib, base64
from http.server import BaseHTTPRequestHandler, HTTPServer

# ---------------- HTTP ----------------
CONNS = {"n": 0}
class H(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    root = "."
    def log_message(self, *a): pass
    def setup(self):
        CONNS["n"] += 1
        super().setup()
    def do_GET(self):
        p = self.path.split("?")[0]
        if p == "/stats":
            b = str(CONNS["n"]).encode()
            self.send_response(200); self.send_header("Content-Length", str(len(b))); self.end_headers(); self.wfile.write(b); return
        if p == "/slow":
            self.send_response(200); self.send_header("Content-Length", "100000000"); self.end_headers()
            try:
                for _ in range(600):
                    self.wfile.write(b"\x47" + b"\x00" * 187); self.wfile.flush(); time.sleep(0.05)
            except Exception: pass
            return
        cut = p.startswith("/cut/")
        if cut:
            p = p[4:]
        if p == "/hugelen":
            # 伪造超大 Content-Length，再给少量垃圾后断开（中途断连 + 超大长度）
            self.send_response(200); self.send_header("Content-Length", "9223372036854775000"); self.end_headers()
            try: self.wfile.write(os.urandom(4096)); self.wfile.flush()
            except Exception: pass
            self.close_connection = True; return
        fp = os.path.normpath(os.path.join(self.root, p.lstrip("/")))
        if not fp.startswith(os.path.abspath(self.root)) or not os.path.isfile(fp):
            self.send_response(404); self.send_header("Content-Length", "0"); self.end_headers(); return
        b = open(fp, "rb").read()
        rng = self.headers.get("Range")
        if rng and rng.startswith("bytes="):
            a, _, z = rng[6:].partition("-")
            a = int(a); z = int(z) if z else len(b) - 1
            part = b[a:z + 1]
            self.send_response(206); self.send_header("Content-Range", "bytes %d-%d/%d" % (a, a + len(part) - 1, len(b)))
            self.send_header("Content-Length", str(len(part))); self.end_headers(); self.wfile.write(part); return
        if cut:
            # 网络中途断连：声明完整长度，只发一半就关连接
            self.send_response(200); self.send_header("Content-Length", str(len(b))); self.end_headers()
            try: self.wfile.write(b[:len(b) // 2]); self.wfile.flush()
            except Exception: pass
            self.close_connection = True
            try: self.connection.shutdown(socket.SHUT_RDWR)
            except Exception: pass
            return
        self.send_response(200); self.send_header("Content-Length", str(len(b))); self.end_headers(); self.wfile.write(b)

class TS(socketserver.ThreadingMixIn, HTTPServer):
    daemon_threads = True
    allow_reuse_address = True

# ---------------- FLV 读取 ----------------
def flv_tags(path):
    d = open(path, "rb").read()
    off = struct.unpack(">I", d[5:9])[0] + 4
    out = []
    while off + 11 <= len(d):
        t = d[off]; sz = int.from_bytes(d[off + 1:off + 4], "big")
        ts = int.from_bytes(d[off + 4:off + 7], "big") | (d[off + 7] << 24)
        body = d[off + 11:off + 11 + sz]
        if len(body) < sz: break
        out.append((t, ts, body))
        off += 11 + sz + 4
    return out

# ---------------- RTMP ----------------
def amf_str(s): b = s.encode(); return b"\x02" + struct.pack(">H", len(b)) + b
def amf_num(v): return b"\x00" + struct.pack(">d", v)
def amf_obj(kv):
    o = b"\x03"
    for k, v in kv:
        kb = k.encode(); o += struct.pack(">H", len(kb)) + kb + (amf_str(v) if isinstance(v, str) else amf_num(v))
    return o + b"\x00\x00\x09"

class RtmpConn:
    def __init__(self, s, flv):
        self.s = s; self.flv = flv; self.in_chunk = 128; self.out_chunk = 128; self.cs = {}
    def rd(self, n):
        b = b""
        while len(b) < n:
            k = self.s.recv(n - len(b))
            if not k: raise EOFError
            b += k
        return b
    def send(self, csid, typ, sid, ts, payload):
        out = bytes([csid & 0x3F]) + min(ts, 0xFFFFFF).to_bytes(3, "big") + len(payload).to_bytes(3, "big") + bytes([typ]) + struct.pack("<I", sid)
        if ts >= 0xFFFFFF: out += struct.pack(">I", ts)
        off = 0; first = True
        while first or off < len(payload):
            if not first:
                out += bytes([0xC0 | csid])
                if ts >= 0xFFFFFF: out += struct.pack(">I", ts)
            out += payload[off:off + self.out_chunk]; off += self.out_chunk; first = False
        self.s.sendall(out)
    def read_msg(self):
        while True:
            b0 = self.rd(1)[0]; fmt = b0 >> 6; csid = b0 & 0x3F
            if csid == 0: csid = 64 + self.rd(1)[0]
            elif csid == 1: x = self.rd(2); csid = 64 + x[0] + 256 * x[1]
            c = self.cs.setdefault(csid, {"len": 0, "type": 0, "buf": b""})
            if fmt <= 2:
                h = self.rd([11, 7, 3][fmt])
                if fmt <= 1: c["len"] = int.from_bytes(h[3:6], "big"); c["type"] = h[6]
                if int.from_bytes(h[0:3], "big") == 0xFFFFFF: self.rd(4)
            n = min(self.in_chunk, c["len"] - len(c["buf"]))
            c["buf"] += self.rd(n)
            if len(c["buf"]) >= c["len"]:
                m = (c["type"], c["buf"]); c["buf"] = b""
                if m[0] == 1: self.in_chunk = struct.unpack(">I", m[1][:4])[0]; continue
                return m
    def run(self):
        c0c1 = self.rd(1537)
        self.s.sendall(b"\x03" + b"\x00" * 8 + os.urandom(1528) + c0c1[1:])
        self.rd(1536)
        stream = None
        while True:
            typ, d = self.read_msg()
            if typ != 20: continue
            name_len = struct.unpack(">H", d[1:3])[0]; name = d[3:3 + name_len].decode()
            txn = struct.unpack(">d", d[4 + name_len:12 + name_len])[0]
            if name == "connect":
                self.send(2, 5, 0, 0, struct.pack(">I", 2500000))
                self.send(2, 6, 0, 0, struct.pack(">IB", 2500000, 2))
                self.send(2, 1, 0, 0, struct.pack(">I", 4096)); self.out_chunk = 4096
                self.send(3, 20, 0, 0, amf_str("_result") + amf_num(txn) + amf_obj([("fmsVer", "FMS/3,0,1,123")]) + amf_obj([("code", "NetConnection.Connect.Success")]))
            elif name == "createStream":
                self.send(3, 20, 0, 0, amf_str("_result") + amf_num(txn) + b"\x05" + amf_num(1))
            elif name == "play":
                off = 12 + name_len; off += 1  # null
                sl = struct.unpack(">H", d[off + 1:off + 3])[0]; stream = d[off + 3:off + 3 + sl].decode()
                break
        if stream != "live":
            self.send(5, 20, 1, 0, amf_str("onStatus") + amf_num(0) + b"\x05" + amf_obj([("level", "error"), ("code", "NetStream.Play.StreamNotFound")]))
            time.sleep(0.3); return
        self.send(2, 4, 0, 0, struct.pack(">HI", 0, 1))  # StreamBegin
        self.send(2, 4, 0, 0, struct.pack(">HI", 6, 1234))  # PingRequest
        self.send(5, 20, 1, 0, amf_str("onStatus") + amf_num(0) + b"\x05" + amf_obj([("level", "status"), ("code", "NetStream.Play.Start")]))
        tags = flv_tags(self.flv)
        i = 0
        while i < len(tags):
            t, ts, body = tags[i]
            if i % 10 == 5 and i + 3 < len(tags):  # 每 10 个里聚合 3 个
                agg = b""; base = ts
                for t2, ts2, b2 in tags[i:i + 3]:
                    agg += bytes([t2]) + len(b2).to_bytes(3, "big") + (ts2 & 0xFFFFFF).to_bytes(3, "big") + bytes([ts2 >> 24]) + b"\x00\x00\x00" + b2 + struct.pack(">I", 11 + len(b2))
                self.send(6, 22, 1, base, agg); i += 3; continue
            if t == 18: self.send(5, 18, 1, ts, amf_str("@setDataFrame") + body)
            else: self.send(4 if t == 8 else 6, t, 1, ts, body)
            i += 1
        self.send(5, 20, 1, 0, amf_str("onStatus") + amf_num(0) + b"\x05" + amf_obj([("level", "status"), ("code", "NetStream.Play.Stop")]))
        time.sleep(0.5)

class RtmpH(socketserver.BaseRequestHandler):
    flv = None
    def handle(self):
        try: RtmpConn(self.request, self.flv).run()
        except Exception: pass

class TCPS(socketserver.ThreadingMixIn, socketserver.TCPServer):
    daemon_threads = True; allow_reuse_address = True

# ---------------- RTSP ----------------
def rtp(pt, seq, ts, marker, payload, ssrc=0x1234):
    return struct.pack(">BBHII", 0x80, (0x80 if marker else 0) | pt, seq & 0xFFFF, ts & 0xFFFFFFFF, ssrc) + payload

class RtspH(socketserver.BaseRequestHandler):
    flv = None; tsf = None
    def handle(self):
        s = self.request; buf = b""; sess = "ABCD1234"; nonce = "n0nce42"; authed = False
        def resp(code, cseq, extra="", body=b""):
            h = "RTSP/1.0 %d %s\r\nCSeq: %s\r\n%s" % (code, "OK" if code == 200 else "ERR", cseq, extra)
            if body: h += "Content-Length: %d\r\n" % len(body)
            s.sendall(h.encode() + b"\r\n" + body)
        path = None
        try:
            while True:
                while b"\r\n\r\n" not in buf:
                    k = s.recv(4096)
                    if not k: return
                    buf += k
                head, _, buf = buf.partition(b"\r\n\r\n")
                lines = head.decode().split("\r\n"); method, uri, _ = lines[0].split(" ")
                hd = {l.split(":", 1)[0].lower(): l.split(":", 1)[1].strip() for l in lines[1:] if ":" in l}
                cseq = hd.get("cseq", "0")
                path = "/" + uri.split("/", 3)[3] if uri.count("/") >= 3 else "/"
                base = path.split("/")[1]
                if base == "auth" and not authed and method != "OPTIONS":
                    a = hd.get("authorization", "")
                    if a.startswith("Digest"):
                        f = dict(x.strip().split("=", 1) for x in a[7:].split(","))
                        f = {k: v.strip('"') for k, v in f.items()}
                        h1 = hashlib.md5(b"user:testrealm:pass").hexdigest()
                        h2 = hashlib.md5(("%s:%s" % (method, f["uri"])).encode()).hexdigest()
                        if f.get("response") == hashlib.md5(("%s:%s:%s" % (h1, nonce, h2)).encode()).hexdigest(): authed = True
                    if not authed:
                        resp(401, cseq, 'WWW-Authenticate: Digest realm="testrealm", nonce="%s"\r\n' % nonce); continue
                if method == "OPTIONS":
                    resp(200, cseq, "Public: OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN, GET_PARAMETER\r\n")
                elif method == "DESCRIBE":
                    if base == "ts":
                        sdp = "v=0\r\no=- 0 0 IN IP4 127.0.0.1\r\ns=t\r\nt=0 0\r\nm=video 0 RTP/AVP 33\r\na=control:track1\r\n"
                    else:
                        tags = flv_tags(self.flv)
                        avcc = next(b for t, ts, b in tags if t == 9 and b[1] == 0)[5:]
                        sps = avcc[8:8 + struct.unpack(">H", avcc[6:8])[0]]; o = 8 + len(sps)
                        pps = avcc[o + 3:o + 3 + struct.unpack(">H", avcc[o + 1:o + 3])[0]]
                        asc = next(b for t, ts, b in tags if t == 8 and b[1] == 0)[2:]
                        sdp = ("v=0\r\no=- 0 0 IN IP4 127.0.0.1\r\ns=t\r\nt=0 0\r\n"
                               "m=video 0 RTP/AVP 96\r\na=rtpmap:96 H264/90000\r\na=fmtp:96 packetization-mode=1;sprop-parameter-sets=%s,%s\r\na=control:trackID=0\r\n"
                               "m=audio 0 RTP/AVP 97\r\na=rtpmap:97 MPEG4-GENERIC/44100/2\r\na=fmtp:97 streamtype=5;profile-level-id=1;mode=AAC-hbr;sizelength=13;indexlength=3;indexdeltalength=3;config=%s\r\na=control:trackID=1\r\n"
                               % (base64.b64encode(sps).decode(), base64.b64encode(pps).decode(), asc.hex()))
                    resp(200, cseq, "Content-Type: application/sdp\r\nContent-Base: rtsp://127.0.0.1/%s/\r\n" % base, sdp.encode())
                elif method == "SETUP":
                    resp(200, cseq, "Session: %s;timeout=60\r\nTransport: %s\r\n" % (sess, hd.get("transport", "")))
                elif method == "PLAY":
                    resp(200, cseq, "Session: %s\r\n" % sess)
                    if base == "ts": self.play_ts(s)
                    else: self.play_es(s)
                    time.sleep(0.3); return
                else:
                    resp(200, cseq, "Session: %s\r\n" % sess)
        except Exception:
            return
    def il(self, s, ch, pkt): s.sendall(b"$" + bytes([ch]) + struct.pack(">H", len(pkt)) + pkt)
    def play_ts(self, s):
        d = open(self.tsf, "rb").read(); seq = 0
        for off in range(0, len(d) - 188 * 7 + 1, 188 * 7):
            self.il(s, 0, rtp(33, seq, seq * 90, 0, d[off:off + 188 * 7])); seq += 1
    def play_es(self, s):
        tags = flv_tags(self.flv); vseq = 0; aseq = 0
        for t, ts, b in tags:
            if t == 9 and b[1] == 1:
                cts = int.from_bytes(b[2:5], "big"); rts = (ts + cts) * 90; d = b[5:]; nals = []
                while len(d) >= 4:
                    l = struct.unpack(">I", d[:4])[0]; nals.append(d[4:4 + l]); d = d[4 + l:]
                if b[0] >> 4 == 1 and len(nals) > 0:
                    pass
                for i, n in enumerate(nals):
                    last = i == len(nals) - 1
                    if len(n) <= 1400:
                        self.il(s, 0, rtp(96, vseq, rts, last, n)); vseq += 1
                    else:
                        hdr = n[0]; p = n[1:]; first = True
                        while p:
                            ch, p = p[:1400], p[1400:]
                            fu = bytes([(hdr & 0xE0) | 28, (0x80 if first else 0) | (0x40 if not p else 0) | (hdr & 0x1F)])
                            self.il(s, 0, rtp(96, vseq, rts, last and not p, fu + ch)); vseq += 1; first = False
            elif t == 8 and b[1] == 1:
                raw = b[2:]; hdr = struct.pack(">HH", 16, (len(raw) << 3) & 0xFFFF)
                self.il(s, 2, rtp(97, aseq, ts * 44100 // 1000, 1, hdr + raw)); aseq += 1

# ---------------- 畸形 RTMP/RTSP（宿主健壮性测试） ----------------
JUNK = {"n": 0}
class JunkH(socketserver.BaseRequestHandler):
    def handle(self):
        s = self.request; s.settimeout(3)
        JUNK["n"] += 1; k = JUNK["n"] % 4
        try:
            first = s.recv(1)
            if first == b"\x03":  # RTMP C0
                s.recv(1536)
                s.sendall(b"\x03" + b"\x00" * 1536 + b"\x00" * 1536)
                if k == 0:   # 超大消息长度 + 垃圾
                    s.sendall(bytes([0x04]) + b"\x00\x00\x00" + b"\xff\xff\xff" + b"\x09" + b"\x01\x00\x00\x00" + os.urandom(20000))
                elif k == 1: # SetChunkSize 0x7fffffff 后乱块
                    s.sendall(bytes([0x02]) + b"\x00\x00\x00" + b"\x00\x00\x04" + b"\x01" + b"\x00\x00\x00\x00" + b"\x7f\xff\xff\xff" + os.urandom(30000))
                elif k == 2: # 扩展时间戳 + fmt3 风暴
                    s.sendall(bytes([0x05]) + b"\xff\xff\xff" + b"\x00\x10\x00" + b"\x08" + b"\x01\x00\x00\x00" + b"\xff\xff\xff\xff" + bytes([0xC5]) * 5000)
                else:        # 半截断开
                    s.sendall(os.urandom(37))
            else:
                buf = first + s.recv(4096)
                if k == 0:   # 超大 Content-Length + 断开
                    s.sendall(b"RTSP/1.0 200 OK\r\nCSeq: 1\r\nContent-Length: 999999999\r\n\r\n" + os.urandom(5000))
                elif k == 1: # 无 CRLF 的超长头
                    s.sendall(b"RTSP/1.0 200 OK\r\nX: " + b"A" * 70000)
                elif k == 2: # 伪造 interleaved 帧（随机长度）
                    s.sendall(b"RTSP/1.0 200 OK\r\nCSeq: 1\r\nPublic: DESCRIBE\r\n\r\n")
                    for _ in range(200):
                        n = struct.unpack(">H", os.urandom(2))[0]
                        s.sendall(b"$" + os.urandom(1) + struct.pack(">H", n) + os.urandom(min(n, 512)))
                else:        # 错误 SDP
                    sdp = b"v=0\r\nm=video 0 RTP/AVP 96\r\na=rtpmap:96 H264/90000\r\na=fmtp:96 sprop-parameter-sets=!!!!,@@@@;packetization-mode=1\r\na=control:" + b"x" * 3000 + b"\r\n"
                    s.sendall(b"RTSP/1.0 200 OK\r\nCSeq: 1\r\nPublic: OPTIONS, DESCRIBE, SETUP, PLAY\r\n\r\n")
                    s.recv(4096)
                    s.sendall(b"RTSP/1.0 200 OK\r\nCSeq: 2\r\nContent-Type: application/sdp\r\nContent-Length: %d\r\n\r\n" % len(sdp) + sdp)
                    s.recv(4096)
            time.sleep(0.2)
        except Exception:
            pass

def main():
    mode = sys.argv[1]
    if mode == "http":
        H.root = os.path.abspath(sys.argv[2]); TS(("127.0.0.1", int(sys.argv[3])), H).serve_forever()
    elif mode == "rtmp":
        RtmpH.flv = sys.argv[2]; TCPS(("127.0.0.1", int(sys.argv[3])), RtmpH).serve_forever()
    elif mode == "rtsp":
        RtspH.flv = sys.argv[2]; RtspH.tsf = sys.argv[3]; TCPS(("127.0.0.1", int(sys.argv[4])), RtspH).serve_forever()
    elif mode == "junk":
        TCPS(("127.0.0.1", int(sys.argv[2])), JunkH).serve_forever()
    elif mode == "udp-send":
        d = open(sys.argv[2], "rb").read(); port = int(sys.argv[3]); use_rtp = sys.argv[4] == "rtp"
        u = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); time.sleep(0.5); seq = 100; pk = []
        for off in range(0, len(d) - 188 * 7 + 1, 188 * 7):
            p = d[off:off + 188 * 7]
            pk.append(rtp(33, seq, seq * 90, 0, p) if use_rtp else p); seq += 1
        for i, p in enumerate(pk):
            u.sendto(p, ("127.0.0.1", port))
            if use_rtp and i == 50: u.sendto(pk[49], ("127.0.0.1", port))  # 重复包
            if i % 20 == 0: time.sleep(0.002)
main()
