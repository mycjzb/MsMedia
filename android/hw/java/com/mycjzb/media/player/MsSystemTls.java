/*
 * 安卓系统 TLS 引擎：SSLEngine + 系统 TrustManager，主机名必须校验。
 * 无关闭校验的开关。供 libmsplayer.so 的 FFmpeg tls 后端通过 JNI 调用。
 * by: Long <mycjzb@gmail.com>
 */
package com.mycjzb.media.player;

import java.nio.ByteBuffer;
import java.security.cert.Certificate;

import javax.net.ssl.HostnameVerifier;
import javax.net.ssl.HttpsURLConnection;
import javax.net.ssl.SSLContext;
import javax.net.ssl.SSLEngine;
import javax.net.ssl.SSLEngineResult;
import javax.net.ssl.SSLException;
import javax.net.ssl.SSLParameters;
import javax.net.ssl.SSLSession;

public final class MsSystemTls {
    private final String host;
    private final SSLEngine engine;
    private final ByteBuffer appIn;
    private final ByteBuffer appOut;
    private final ByteBuffer netIn;
    private final ByteBuffer netOut;
    private boolean handshakeDone;
    private boolean hostnameChecked;
    private int need; /* 0 无，1 读 TCP，2 写 TCP */

    public MsSystemTls(String hostname) throws Exception {
        if (hostname == null || hostname.length() == 0) {
            throw new IllegalArgumentException("hostname required for TLS");
        }
        this.host = hostname;
        /* null TrustManager = 系统默认 CA，不使用信任全部的自定义 Manager。 */
        SSLContext ctx = SSLContext.getInstance("TLS");
        ctx.init(null, null, null);
        engine = ctx.createSSLEngine(hostname, 443);
        engine.setUseClientMode(true);
        SSLParameters p = engine.getSSLParameters();
        try {
            p.setEndpointIdentificationAlgorithm("HTTPS");
        } catch (Throwable ignored) {
            /* API 18–23 无此方法；握手完成后用 HostnameVerifier 补上。 */
        }
        engine.setSSLParameters(p);
        int app = Math.max(engine.getSession().getApplicationBufferSize(), 16384);
        int net = Math.max(engine.getSession().getPacketBufferSize(), 16384);
        appIn = ByteBuffer.allocate(app);
        appOut = ByteBuffer.allocate(app);
        netIn = ByteBuffer.allocate(net);
        netOut = ByteBuffer.allocate(net);
        engine.beginHandshake();
    }

    private static String NULL_TO_EMPTY(String s) {
        return s == null ? "" : s;
    }

    /** 0=完成，1=需读 TCP，2=需把 takeNet 的字节写出，负值失败。 */
    public synchronized int handshake() {
        try {
            return handshakeInner();
        } catch (Exception e) {
            need = 0;
            return -1;
        }
    }

    private int handshakeInner() throws SSLException {
        if (handshakeDone) {
            return 0;
        }
        for (;;) {
            SSLEngineResult.HandshakeStatus hs = engine.getHandshakeStatus();
            if (hs == SSLEngineResult.HandshakeStatus.FINISHED
                    || hs == SSLEngineResult.HandshakeStatus.NOT_HANDSHAKING) {
                verifyHostname();
                handshakeDone = true;
                need = 0;
                return 0;
            }
            if (hs == SSLEngineResult.HandshakeStatus.NEED_UNWRAP) {
                if (netIn.position() == 0) {
                    need = 1;
                    return 1;
                }
                netIn.flip();
                SSLEngineResult r = engine.unwrap(netIn, appIn);
                netIn.compact();
                if (r.getStatus() == SSLEngineResult.Status.BUFFER_UNDERFLOW) {
                    need = 1;
                    return 1;
                }
                if (r.getStatus() == SSLEngineResult.Status.CLOSED) {
                    return -1;
                }
                continue;
            }
            if (hs == SSLEngineResult.HandshakeStatus.NEED_WRAP) {
                netOut.clear();
                SSLEngineResult r = engine.wrap(appOut, netOut);
                if (r.getStatus() != SSLEngineResult.Status.OK
                        && r.getStatus() != SSLEngineResult.Status.BUFFER_UNDERFLOW) {
                    if (r.getStatus() == SSLEngineResult.Status.CLOSED) {
                        return -1;
                    }
                }
                netOut.flip();
                if (netOut.remaining() > 0) {
                    need = 2;
                    return 2;
                }
                continue;
            }
            if (hs == SSLEngineResult.HandshakeStatus.NEED_TASK) {
                Runnable task;
                while ((task = engine.getDelegatedTask()) != null) {
                    task.run();
                }
                continue;
            }
            return -1;
        }
    }

    private void verifyHostname() throws SSLException {
        if (hostnameChecked) {
            return;
        }
        SSLSession session = engine.getSession();
        Certificate[] chain = session.getPeerCertificates();
        if (chain == null || chain.length == 0) {
            throw new SSLException("empty peer certificate chain");
        }
        HostnameVerifier hv = HttpsURLConnection.getDefaultHostnameVerifier();
        if (!hv.verify(host, session)) {
            throw new SSLException("hostname verification failed");
        }
        hostnameChecked = true;
    }

    public synchronized int feedNet(byte[] src) {
        if (src == null || src.length == 0) {
            return -1;
        }
        if (netIn.remaining() < src.length) {
            return -1;
        }
        netIn.put(src);
        return src.length;
    }

    private static byte[] NULL_TO_EMPTY_ARR(byte[] a) {
        return a == null ? new byte[0] : a;
    }

    public synchronized int takeNet(byte[] dst) {
        if (dst == null || dst.length == 0) {
            return 0;
        }
        int n = Math.min(dst.length, netOut.remaining());
        if (n > 0) {
            netOut.get(dst, 0, n);
        }
        if (!netOut.hasRemaining()) {
            netOut.clear();
            need = 0;
        }
        return n;
    }

    public synchronized int needNet() {
        return need;
    }

    public synchronized int readApp(byte[] dst) {
        try {
            if (!handshakeDone) {
                int hs = handshakeInner();
                if (hs != 0) {
                    return 0;
                }
            }
            if (appIn.position() > 0) {
                appIn.flip();
                int n = Math.min(dst.length, appIn.remaining());
                appIn.get(dst, 0, n);
                appIn.compact();
                if (n > 0) {
                    return n;
                }
            }
            if (netIn.position() == 0) {
                need = 1;
                return 0;
            }
            netIn.flip();
            SSLEngineResult r = engine.unwrap(netIn, appIn);
            netIn.compact();
            if (r.getStatus() == SSLEngineResult.Status.BUFFER_UNDERFLOW) {
                need = 1;
                return 0;
            }
            if (r.getStatus() == SSLEngineResult.Status.CLOSED) {
                return -1;
            }
            if (r.getHandshakeStatus() == SSLEngineResult.HandshakeStatus.NEED_WRAP) {
                netOut.clear();
                engine.wrap(appOut, netOut);
                netOut.flip();
                need = 2;
                return 0;
            }
            if (appIn.position() > 0) {
                appIn.flip();
                int n = Math.min(dst.length, appIn.remaining());
                appIn.get(dst, 0, n);
                appIn.compact();
                return n;
            }
            need = 1;
            return 0;
        } catch (Exception e) {
            return -1;
        }
    }

    public synchronized int writeApp(byte[] src) {
        try {
            if (!handshakeDone) {
                int hs = handshakeInner();
                if (hs != 0) {
                    return 0;
                }
            }
            appOut.clear();
            int n = Math.min(src.length, appOut.remaining());
            appOut.put(src, 0, n);
            appOut.flip();
            netOut.clear();
            SSLEngineResult r = engine.wrap(appOut, netOut);
            netOut.flip();
            if (r.getStatus() == SSLEngineResult.Status.CLOSED) {
                return -1;
            }
            int used = n - appOut.remaining();
            need = netOut.hasRemaining() ? 2 : 0;
            return used;
        } catch (Exception e) {
            return -1;
        }
    }

    public synchronized void close() {
        try {
            engine.closeOutbound();
        } catch (Exception ignored) {
        }
    }
}
