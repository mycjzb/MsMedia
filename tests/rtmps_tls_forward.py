#!/usr/bin/env python3
# 测试用：把本机明文 RTMP listen 套上 TLS，供受控 RTMPS 客户端校验。
# 不是产品服务器，不关对端校验策略（服务端只出示证书）。
# by: Long <mycjzb@gmail.com>
from __future__ import print_function

import argparse
import socket
import ssl
import sys
import threading


def _pipe(src, dst):
    try:
        while True:
            data = src.recv(16384)
            if not data:
                break
            dst.sendall(data)
    except Exception:
        pass
    for s, how in ((src, socket.SHUT_RD), (dst, socket.SHUT_WR)):
        try:
            s.shutdown(how)
        except Exception:
            pass


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--listen", type=int, required=True)
    ap.add_argument("--backend-port", type=int, required=True)
    ap.add_argument("--backend-host", default="127.0.0.1")
    ap.add_argument("--cert", required=True)
    ap.add_argument("--key", required=True)
    args = ap.parse_args()
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(args.cert, args.key)
    ls = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    ls.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    ls.bind(("127.0.0.1", args.listen))
    ls.listen(8)
    print("READY %d" % args.listen, flush=True)
    while True:
        conn, _addr = ls.accept()
        try:
            tls = ctx.wrap_socket(conn, server_side=True)
        except Exception:
            try:
                conn.close()
            except Exception:
                pass
            continue
        try:
            back = socket.create_connection((args.backend_host, args.backend_port), 5)
        except Exception:
            try:
                tls.close()
            except Exception:
                pass
            continue
        threading.Thread(target=_pipe, args=(tls, back), daemon=True).start()
        threading.Thread(target=_pipe, args=(back, tls), daemon=True).start()


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        sys.exit(0)
