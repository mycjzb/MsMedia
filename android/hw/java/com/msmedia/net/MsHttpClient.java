/*
 * 标准网络请求封装：走 libmsplayer.so 里的公共可控网络层（自研 TCP/HTTP，自定义 DNS / SOCKS5，
 * HTTPS 叠加安卓系统 TLS 且保留完整证书链与主机名校验）。与播放器取流共用同一份原生实现。
 * 支持 GET/POST/PUT/DELETE/HEAD/PATCH、自定义请求头、连接/读超时、重定向（禁止 https->http 降级）、
 * 流式读取或一次性读取、任意线程取消。
 * DNS / SOCKS5 是进程级配置（播放器与本类同时生效），用 {@link #configure(NetConfig)} 设置。
 * 不记录 URL、请求头和请求体。
 * by: Long <mycjzb@gmail.com>
 */
package com.msmedia.net;

import com.msmedia.MsNativeLoader;

import java.io.ByteArrayOutputStream;
import java.io.Closeable;
import java.io.IOException;
import java.io.InputStream;
import java.net.UnknownHostException;
import java.nio.charset.Charset;
import java.util.ArrayList;
import java.util.Collections;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.TreeMap;

public final class MsHttpClient {

    /** 自定义 DNS：返回 IP 字面量列表；返回 null/空表示交给系统解析。 */
    public interface Dns {
        List<String> lookup(String host) throws UnknownHostException;
    }

    /** 进程级网络配置（播放器与 MsHttpClient 共用）。 */
    public static final class NetConfig {
        final Map<String, List<String>> hosts = new LinkedHashMap<String, List<String>>();
        Dns dns;
        String socksHost;
        int socksPort;
        String socksUser;
        String socksPass;
        boolean socksRemoteDns = true;

        /** 静态 hosts 覆盖（优先于 Dns 回调与系统解析）。 */
        public NetConfig hostOverride(String host, String... ips) {
            if (host == null || ips == null || ips.length == 0) {
                throw new IllegalArgumentException("host/ips");
            }
            List<String> l = new ArrayList<String>();
            for (String ip : ips) {
                if (ip == null || ip.indexOf(',') >= 0 || ip.indexOf(';') >= 0 || ip.indexOf('=') >= 0) {
                    throw new IllegalArgumentException("bad ip literal");
                }
                l.add(ip.trim());
            }
            if (host.indexOf(',') >= 0 || host.indexOf(';') >= 0 || host.indexOf('=') >= 0) {
                throw new IllegalArgumentException("bad host");
            }
            hosts.put(host.toLowerCase(Locale.ROOT), l);
            return this;
        }

        public NetConfig dns(Dns d) {
            dns = d;
            return this;
        }

        /** SOCKS5 代理；host 为 null 关闭代理。 */
        public NetConfig socks5(String host, int port) {
            if (host != null && (port <= 0 || port > 65535)) {
                throw new IllegalArgumentException("port");
            }
            socksHost = host;
            socksPort = host == null ? 0 : port;
            return this;
        }

        public NetConfig socks5Auth(String user, String pass) {
            socksUser = user;
            socksPass = pass;
            return this;
        }

        /** true（默认）=域名交给代理解析（socks5h）；false=本地解析后发 IP。 */
        public NetConfig socks5RemoteDns(boolean remote) {
            socksRemoteDns = remote;
            return this;
        }
    }

    private static volatile Dns sDns;
    private static boolean sInit;

    static {
        MsNativeLoader.ensure();
    }

    /** 由 MsNativeLoader 在库加载后调用（Java 线程），缓存原生线程要用的类。 */
    public static synchronized void initFromLoader() {
        if (!sInit) {
            sInit = true;
            nativeInit();
        }
    }

    private static void requireLib() throws IOException {
        if (!MsNativeLoader.ensure()) {
            throw new IOException("libmsplayer.so not loaded");
        }
        initFromLoader();
    }

    /** 设置进程级 DNS / SOCKS5（同时影响 com.msmedia.MediaPlayer 的网络取流）。 */
    public static void configure(NetConfig c) throws IOException {
        requireLib();
        if (c == null) {
            c = new NetConfig();
        }
        StringBuilder map = new StringBuilder();
        for (Map.Entry<String, List<String>> e : c.hosts.entrySet()) {
            if (map.length() > 0) {
                map.append(';');
            }
            map.append(e.getKey()).append('=');
            for (int i = 0; i < e.getValue().size(); i++) {
                if (i > 0) {
                    map.append(',');
                }
                map.append(e.getValue().get(i));
            }
        }
        if (map.length() >= 1024) {
            throw new IllegalArgumentException("host overrides too long");
        }
        sDns = c.dns;
        int r = nativeConfigure(map.toString(), c.dns != null, c.socksHost, c.socksPort,
                c.socksUser, c.socksPass, c.socksRemoteDns);
        if (r != 0) {
            throw new IOException("configure failed: " + r);
        }
    }

    /** 原生拨号线程回调（已 attach）。异常即解析失败。 */
    @SuppressWarnings("unused")
    static String[] dnsResolveFromNative(String host) throws UnknownHostException {
        Dns d = sDns;
        if (d == null) {
            return null;
        }
        List<String> l = d.lookup(host);
        if (l == null || l.isEmpty()) {
            return null;
        }
        return l.toArray(new String[0]);
    }

    /* ------------------------- 客户端 ------------------------- */
    final int connectTimeoutMs;
    final int readTimeoutMs;
    final int maxRedirects;

    private MsHttpClient(Builder b) {
        connectTimeoutMs = b.connectTimeoutMs;
        readTimeoutMs = b.readTimeoutMs;
        maxRedirects = b.followRedirects ? b.maxRedirects : 0;
    }

    public static final class Builder {
        int connectTimeoutMs = 15000;
        int readTimeoutMs = 15000;
        boolean followRedirects = true;
        int maxRedirects = 5;

        public Builder connectTimeout(int ms) {
            if (ms <= 0) {
                throw new IllegalArgumentException("timeout");
            }
            connectTimeoutMs = ms;
            return this;
        }

        public Builder readTimeout(int ms) {
            if (ms <= 0) {
                throw new IllegalArgumentException("timeout");
            }
            readTimeoutMs = ms;
            return this;
        }

        public Builder followRedirects(boolean f) {
            followRedirects = f;
            return this;
        }

        public Builder maxRedirects(int n) {
            if (n < 0 || n > 20) {
                throw new IllegalArgumentException("redirects");
            }
            maxRedirects = n;
            return this;
        }

        public MsHttpClient build() {
            return new MsHttpClient(this);
        }
    }

    public static final class Request {
        final String url;
        final String method;
        final String headers;
        final byte[] body;

        private Request(RequestBuilder b) {
            url = b.url;
            method = b.method;
            body = b.body;
            StringBuilder sb = new StringBuilder();
            for (String[] h : b.headers) {
                sb.append(h[0]).append(": ").append(h[1]).append('\n');
            }
            headers = sb.toString();
        }
    }

    public static final class RequestBuilder {
        String url;
        String method = "GET";
        byte[] body;
        final List<String[]> headers = new ArrayList<String[]>();

        public RequestBuilder url(String u) {
            if (u == null || !(u.startsWith("http://") || u.startsWith("https://"))) {
                throw new IllegalArgumentException("only http/https");
            }
            url = u;
            return this;
        }

        public RequestBuilder header(String name, String value) {
            if (name == null || value == null || name.isEmpty()
                    || name.indexOf(':') >= 0 || hasCtl(name) || hasCtl(value)) {
                throw new IllegalArgumentException("bad header");
            }
            headers.add(new String[] {name, value});
            return this;
        }

        public RequestBuilder get() {
            method = "GET";
            body = null;
            return this;
        }

        public RequestBuilder head() {
            method = "HEAD";
            body = null;
            return this;
        }

        public RequestBuilder post(byte[] b, String contentType) {
            return method("POST", b, contentType);
        }

        public RequestBuilder method(String m, byte[] b, String contentType) {
            if (m == null || !m.matches("[A-Z]{1,16}")) {
                throw new IllegalArgumentException("method");
            }
            method = m;
            body = b;
            if (contentType != null) {
                header("Content-Type", contentType);
            }
            return this;
        }

        public Request build() {
            if (url == null) {
                throw new IllegalStateException("url");
            }
            return new Request(this);
        }

        private static boolean hasCtl(String s) {
            for (int i = 0; i < s.length(); i++) {
                char ch = s.charAt(i);
                if ((ch < 0x20 && ch != '\t') || ch == 0x7f) {
                    return true;
                }
            }
            return false;
        }
    }

    public Call newCall(Request r) {
        if (r == null) {
            throw new NullPointerException("request");
        }
        return new Call(this, r);
    }

    /** 一次请求；execute 只能调用一次。cancel 可在任意线程、任意时刻调用。 */
    public static final class Call {
        private final MsHttpClient client;
        private final Request req;
        private final Object lock = new Object();
        private long handle;
        private boolean executed;
        private volatile boolean canceled;

        Call(MsHttpClient c, Request r) {
            client = c;
            req = r;
        }

        public Response execute() throws IOException {
            requireLib();
            long h;
            synchronized (lock) {
                if (executed) {
                    throw new IllegalStateException("already executed");
                }
                executed = true;
                handle = nativeCreate();
                if (handle == 0) {
                    throw new IOException("nomem");
                }
                h = handle;
                if (canceled) {
                    nativeCancel(h);
                }
            }
            try {
                int code = nativeExecute(h, req.method, req.url, req.headers, req.body,
                        client.connectTimeoutMs, client.readTimeoutMs, client.maxRedirects);
                return new Response(this, code, nativeHeaders(h), nativeContentLength(h));
            } catch (IOException e) {
                release();
                throw e;
            }
        }

        public void cancel() {
            canceled = true;
            synchronized (lock) {
                if (handle != 0) {
                    nativeCancel(handle);
                }
            }
        }

        public boolean isCanceled() {
            return canceled;
        }

        long handle() {
            synchronized (lock) {
                return handle;
            }
        }

        void release() {
            long h;
            synchronized (lock) {
                h = handle;
                handle = 0;
            }
            if (h != 0) {
                nativeRelease(h);
            }
        }
    }

    public static final class Response implements Closeable {
        private final Call call;
        private final int code;
        private final Map<String, List<String>> headers;
        private final long contentLength;
        private final Object readLock = new Object();
        private boolean closed;
        private InputStream stream;

        Response(Call c, int code, String rawHeaders, long len) {
            call = c;
            this.code = code;
            contentLength = len;
            Map<String, List<String>> m = new TreeMap<String, List<String>>(String.CASE_INSENSITIVE_ORDER);
            for (String line : rawHeaders.split("\r\n")) {
                int i = line.indexOf(':');
                if (i <= 0) {
                    continue;
                }
                String k = line.substring(0, i).trim();
                String v = line.substring(i + 1).trim();
                List<String> l = m.get(k);
                if (l == null) {
                    l = new ArrayList<String>();
                    m.put(k, l);
                }
                l.add(v);
            }
            headers = Collections.unmodifiableMap(m);
        }

        public int code() {
            return code;
        }

        public boolean isSuccessful() {
            return code >= 200 && code < 300;
        }

        public String header(String name) {
            List<String> l = headers.get(name);
            return (l == null || l.isEmpty()) ? null : l.get(0);
        }

        public Map<String, List<String>> headers() {
            return headers;
        }

        /** -1 表示未知（chunked 或读到关闭）。 */
        public long contentLength() {
            return contentLength;
        }

        /** 流式读取响应体；关闭流即关闭响应。 */
        public synchronized InputStream body() {
            if (stream == null) {
                stream = new InputStream() {
                    private final byte[] one = new byte[1];

                    @Override
                    public int read() throws IOException {
                        int n = read(one, 0, 1);
                        return n <= 0 ? -1 : (one[0] & 0xff);
                    }

                    @Override
                    public int read(byte[] b, int off, int len) throws IOException {
                        synchronized (readLock) {
                            if (closed) {
                                throw new IOException("closed");
                            }
                            long h = call.handle();
                            if (h == 0) {
                                throw new IOException("released");
                            }
                            return nativeRead(h, b, off, len);
                        }
                    }

                    @Override
                    public void close() {
                        Response.this.close();
                    }
                };
            }
            return stream;
        }

        /** 一次性读取（上限 64 MiB，防止误用撑爆内存）。 */
        public byte[] bytes() throws IOException {
            ByteArrayOutputStream out = new ByteArrayOutputStream();
            byte[] buf = new byte[16384];
            InputStream in = body();
            int n;
            try {
                while ((n = in.read(buf, 0, buf.length)) > 0) {
                    out.write(buf, 0, n);
                    if (out.size() > 64 * 1024 * 1024) {
                        throw new IOException("body too large for bytes(); use body()");
                    }
                }
            } finally {
                close();
            }
            return out.toByteArray();
        }

        public String string() throws IOException {
            return new String(bytes(), Charset.forName("UTF-8"));
        }

        @Override
        public void close() {
            call.cancel(); // 先解除可能阻塞的读，再在读锁外释放
            synchronized (readLock) {
                if (closed) {
                    return;
                }
                closed = true;
            }
            call.release();
        }
    }

    /* ------------------------- native ------------------------- */
    static native int nativeInit();

    static native int nativeConfigure(String dnsMap, boolean javaDns, String socksHost, int socksPort,
                                      String user, String pass, boolean remoteDns);

    static native long nativeCreate();

    static native int nativeExecute(long h, String method, String url, String headers, byte[] body,
                                    int connectMs, int readMs, int maxRedirects) throws IOException;

    static native String nativeHeaders(long h);

    static native long nativeContentLength(long h);

    static native int nativeRead(long h, byte[] buf, int off, int len) throws IOException;

    static native void nativeCancel(long h);

    static native void nativeRelease(long h);
}
