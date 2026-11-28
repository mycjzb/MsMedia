package com.mycjzb.media.player;

import android.net.Uri;

import androidx.media3.common.C;
import androidx.media3.common.MediaItem;
import androidx.media3.common.MimeTypes;
import androidx.media3.common.Timeline;
import androidx.media3.datasource.ByteArrayDataSource;
import androidx.media3.datasource.DataSource;
import androidx.media3.datasource.DataSpec;
import androidx.media3.datasource.FileDataSource;
import androidx.media3.datasource.TransferListener;
import androidx.media3.exoplayer.dash.DashMediaSource;
import androidx.media3.exoplayer.hls.HlsMediaSource;
import androidx.media3.exoplayer.source.CompositeMediaSource;
import androidx.media3.exoplayer.source.MediaPeriod;
import androidx.media3.exoplayer.source.MediaSource;
import androidx.media3.exoplayer.source.ProgressiveMediaSource;
import androidx.media3.exoplayer.upstream.Allocator;
import androidx.media3.extractor.ExtractorsFactory;

import com.mycjzb.media.player.n.a;
import com.mycjzb.media.player.n.b;

import java.io.IOException;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;

/**
 * prepare 时同连接读字节头 → 选 HLS/DASH/JS/Progressive；
 * JS：eval main → 再套一层本类探测播放。剥图伪装仍走 {@link PrefixStripDataSource}。
 */
final class ByteHeadSource extends CompositeMediaSource<Void> {
    private static final int HEAD_CAP = 64 * 1024;
    private static final int SCRIPT_CAP = 512 * 1024; /* JS 明文/密文均 ≤512KiB */
    private static final int MAX_JS_DEPTH = 2;

    private final Uri uri;
    private final MediaItem placeholder;
    private final DataSource.Factory base;
    private final ExtractorsFactory extractors;
    private final int jsDepth;

    private Head hold;
    private MediaSource child;
    private IOException prepareError;

    ByteHeadSource(Uri uri, DataSource.Factory base, ExtractorsFactory extractors) {
        this(uri, base, extractors, 0);
    }

    ByteHeadSource(Uri uri, DataSource.Factory base, ExtractorsFactory extractors, int jsDepth) {
        this.uri = uri;
        this.placeholder = MediaItem.fromUri(uri);
        this.base = base;
        this.extractors = extractors;
        this.jsDepth = jsDepth;
    }

    @Override
    public MediaItem getMediaItem() {
        return child != null ? child.getMediaItem() : placeholder;
    }

    @Override
    protected void prepareSourceInternal(TransferListener mediaTransferListener) {
        super.prepareSourceInternal(mediaTransferListener);
        prepareError = null;
        try {
            hold = Head.open(base, uri);
            child = buildChild(hold, base, extractors, jsDepth);
            prepareChildSource(/* id= */ null, child);
        } catch (IOException e) {
            prepareError = e;
            if (hold != null) {
                hold.abandon();
                hold = null;
            }
            child = null;
        }
    }

    @Override
    public void maybeThrowSourceInfoRefreshError() throws IOException {
        if (prepareError != null) {
            throw prepareError;
        }
        super.maybeThrowSourceInfoRefreshError();
    }

    @Override
    protected void onChildSourceInfoRefreshed(
            Void id, MediaSource mediaSource, Timeline newTimeline) {
        refreshSourceInfo(newTimeline);
    }

    @Override
    public MediaPeriod createPeriod(MediaPeriodId id, Allocator allocator, long startPositionUs) {
        if (child == null) {
            throw new IllegalStateException("ByteHeadSource not prepared");
        }
        return child.createPeriod(id, allocator, startPositionUs);
    }

    @Override
    public void releasePeriod(MediaPeriod mediaPeriod) {
        if (child != null) {
            child.releasePeriod(mediaPeriod);
        }
    }

    @Override
    protected void releaseSourceInternal() {
        super.releaseSourceInternal();
        if (hold != null) {
            hold.abandon();
            hold = null;
        }
        child = null;
        prepareError = null;
    }

    private static MediaSource buildChild(
            Head hold, DataSource.Factory base, ExtractorsFactory extractors, int depth)
            throws IOException {
        if (hold.kind == ContentSniff.JS) {
            return buildFromJs(hold, base, extractors, depth);
        }
        /* 拉到 HTML/JSON 却走 Progressive → CHIP 上报 None of the extractors */
        if (looksNonMediaText(hold.buf, hold.len)) {
            hold.abandon();
            throw new IOException("not media (html/json), check js url/headers");
        }
        DataSource.Factory play = hold.asFactory(base);
        Uri uri = hold.uri;
        int tea = hold.tea;
        int kind = hold.kind;
        /* URL 标明 m3u8 但头被网关改写时，仍优先 HLS；茶模式清单可能嗅探不像 m3u8 */
        if ((kind == ContentSniff.PROG && looksM3u8Url(uri))
                || ((tea == a.m1 || tea == a.m2) && looksM3u8Url(uri))) {
            kind = ContentSniff.HLS;
        }
        if (kind == ContentSniff.HLS) {
            if (tea == a.m1 || tea == a.m2) {
                return new HlsMediaSource.Factory(new YspHlsDataSourceFactory(play, tea))
                        .createMediaSource(new MediaItem.Builder().setUri(uri)
                                .setMimeType(MimeTypes.APPLICATION_M3U8).build());
            }
            return new HlsMediaSource.Factory(play)
                    .createMediaSource(new MediaItem.Builder().setUri(uri)
                            .setMimeType(MimeTypes.APPLICATION_M3U8).build());
        }
        if (kind == ContentSniff.DASH) {
            return new DashMediaSource.Factory(play)
                    .createMediaSource(new MediaItem.Builder().setUri(uri)
                            .setMimeType(MimeTypes.APPLICATION_MPD).build());
        }
        DataSource.Factory prog = play;
        if (tea == a.m1 || tea == a.m2) {
            prog = new YspTeaDataSource.Factory(play, tea);
        }
        return new ProgressiveMediaSource.Factory(prog, extractors)
                .createMediaSource(new MediaItem.Builder().setUri(uri).build());
    }

    private static boolean looksJsUrl(Uri uri) {
        if (uri == null) {
            return false;
        }
        String sch = uri.getScheme();
        if (sch != null && sch.equalsIgnoreCase("asset")) {
            return true;
        }
        String p = uri.getPath();
        if (p == null) {
            return false;
        }
        String low = p.toLowerCase(Locale.US);
        return low.endsWith(".js") || low.endsWith(".msjs");
    }

    private static boolean looksM3u8Url(Uri uri) {
        if (uri == null) {
            return false;
        }
        String s = uri.toString().toLowerCase(Locale.US);
        /* 勿用 /live/：凤凰 FLV 等也会命中 */
        return s.contains(".m3u8") || s.contains("mpegurl") || s.contains("m3u8?");
    }

    private static boolean looksNonMediaText(byte[] b, int n) {
        if (b == null || n < 8) {
            return false;
        }
        int i = 0;
        while (i < n && (b[i] == ' ' || b[i] == '\t' || b[i] == '\r' || b[i] == '\n')) {
            i++;
        }
        if (i >= n) {
            return false;
        }
        if (b[i] == '{') {
            return true; /* JSON 错误页 */
        }
        if (b[i] != '<') {
            return false;
        }
        /* <MPD 已在 sniff 成 DASH；此处拦 html */
        return indexOfAscii(b, i, n, "<MPD") < 0 && indexOfAscii(b, i, n, "<mpd") < 0;
    }

    private static int indexOfAscii(byte[] b, int off, int n, String s) {
        int len = s.length();
        outer:
        for (int i = off; i + len <= n; i++) {
            for (int k = 0; k < len; k++) {
                if (b[i + k] != (byte) s.charAt(k)) {
                    continue outer;
                }
            }
            return i;
        }
        return -1;
    }

    /** 脚本不当媒体播：关探测连接 → QuickJS → A 再 ByteHead / B 内存正文。 */
    private static MediaSource buildFromJs(
            Head hold, DataSource.Factory base, ExtractorsFactory extractors, int depth)
            throws IOException {
        if (depth >= MAX_JS_DEPTH) {
            hold.abandon();
            throw new IOException("js resolve depth exceeded");
        }
        if (!b.r()) {
            hold.abandon();
            throw new IOException("js engine unavailable");
        }
        byte[] script = hold.scriptBytes();
        hold.abandon();
        Object[] pack = b.e(script, hold.uri.toString());
        if (pack == null || pack.length < 5 || !(pack[0] instanceof Integer)) {
            throw new IOException("js main() failed");
        }
        int kind = ((Integer) pack[0]).intValue();
        String playUrl = pack[1] instanceof String ? (String) pack[1] : "";
        String hdrBlock = pack[2] instanceof String ? (String) pack[2] : "";
        byte[] body = pack[3] instanceof byte[] ? (byte[]) pack[3] : null;
        String contentType = pack[4] instanceof String ? (String) pack[4] : "";
        boolean stream = pack.length > 5 && pack[5] instanceof Integer
                && ((Integer) pack[5]).intValue() != 0;
        String streamArgs = pack.length > 6 && pack[6] instanceof String ? (String) pack[6] : "";
        Map<String, String> hdrs = parseHdrBlock(hdrBlock);

        if (kind == b.KIND_BODY) {
            if (body == null || body.length == 0) {
                throw new IOException("js empty body");
            }
            return buildFromJsBody(body, contentType, playUrl, hdrs, extractors, script, stream,
                    streamArgs);
        }

        if (playUrl.length() == 0) {
            throw new IOException("js empty play url");
        }
        String raw = stripJsPlayUrl(playUrl);
        if (raw.length() == 0) {
            throw new IOException("js empty play url after strip");
        }
        Uri playUri = Uri.parse(raw);
        DataSource.Factory next = factoryForPlayUrl(raw, hdrs);
        if (stream) {
            next = new JsStreamHookFactory(script, streamArgs, next);
        }
        /*
         * YSP 等：playurl 带 ysp-tea 时清单体是密文，ByteHead 嗅探不到 #EXTM3U 会误走
         * Progressive → CHIP 报 3003 PARSING_CONTAINER_UNSUPPORTED。已知 HLS 则直建。
         */
        int tea = a.q(playUri);
        if (tea == a.m1 || tea == a.m2 || looksM3u8Url(playUri)) {
            MediaItem.Builder mb = new MediaItem.Builder().setUri(playUri)
                    .setMimeType(MimeTypes.APPLICATION_M3U8);
            if (tea == a.m1 || tea == a.m2) {
                return new HlsMediaSource.Factory(new YspHlsDataSourceFactory(next, tea))
                        .createMediaSource(mb.build());
            }
            return new HlsMediaSource.Factory(next).createMediaSource(mb.build());
        }
        return new ByteHeadSource(playUri, next, extractors, depth + 1);
    }

    /** B：JS 已下好的清单/文件 → 内存起播；分片可走 net 或 stream_call。 */
    private static MediaSource buildFromJsBody(
            byte[] body, String contentType, String baseUrl, Map<String, String> hdrs,
            ExtractorsFactory extractors, byte[] script, boolean stream, String streamArgs) {
        int sniff = sniffBodyKind(body, contentType);
        Uri inline = Uri.parse("msjs://inline/0");
        DataSource.Factory net = factoryForPlayUrl(
                baseUrl != null && baseUrl.length() > 0 ? stripJsPlayUrl(baseUrl) : "https://msjs.local/",
                hdrs);
        if (stream) {
            net = new JsStreamHookFactory(script, streamArgs, net);
        }
        if (sniff == ContentSniff.HLS) {
            DataSource.Factory f = new InlineThenNetFactory(body, inline, net);
            return new HlsMediaSource.Factory(f)
                    .createMediaSource(new MediaItem.Builder().setUri(inline)
                            .setMimeType(MimeTypes.APPLICATION_M3U8).build());
        }
        if (sniff == ContentSniff.DASH) {
            DataSource.Factory f = new InlineThenNetFactory(body, inline, net);
            return new DashMediaSource.Factory(f)
                    .createMediaSource(new MediaItem.Builder().setUri(inline)
                            .setMimeType(MimeTypes.APPLICATION_MPD).build());
        }
        DataSource.Factory only = () -> new ByteArrayDataSource(body);
        return new ProgressiveMediaSource.Factory(only, extractors)
                .createMediaSource(new MediaItem.Builder().setUri(inline).build());
    }

    private static int sniffBodyKind(byte[] body, String contentType) {
        String ct = contentType != null ? contentType.toLowerCase(Locale.US) : "";
        if (ct.indexOf("mpegurl") >= 0 || ct.indexOf("m3u8") >= 0) {
            return ContentSniff.HLS;
        }
        if (ct.indexOf("mpd") >= 0 || ct.indexOf("dash") >= 0) {
            return ContentSniff.DASH;
        }
        return ContentSniff.sniff(body, body.length, true);
    }

    /**
     * main() 带 stream 时：非 inline 的 open 把原始 URL 丢给 {@link b#s}（stream_call）取字节；
     * stream_call 失败则回落下游 net（可纯透传脚本）。
     */
    private static final class JsStreamHookFactory implements DataSource.Factory {
        private final byte[] script;
        private final String argsJson;
        private final DataSource.Factory fallback;

        JsStreamHookFactory(byte[] script, String argsJson, DataSource.Factory fallback) {
            this.script = script;
            this.argsJson = argsJson != null ? argsJson : "";
            this.fallback = fallback;
        }

        @Override
        public DataSource createDataSource() {
            return new DataSource() {
                private DataSource cur;
                private Uri opened;

                @Override
                public void addTransferListener(TransferListener transferListener) {}

                @Override
                public long open(DataSpec dataSpec) throws IOException {
                    close();
                    opened = dataSpec.uri;
                    String u = dataSpec.uri != null ? dataSpec.uri.toString() : "";
                    /* 清单本身不解；只对分片走 stream_call */
                    String low = u.toLowerCase(Locale.US);
                    boolean playlist = low.contains(".m3u8") || low.contains(".mpd");
                    byte[] got = null;
                    if (!playlist) {
                        try {
                            got = b.s(script, u, argsJson);
                        } catch (Throwable ignored) {
                        }
                    }
                    if (got != null && got.length > 0) {
                        cur = new ByteArrayDataSource(got);
                        return cur.open(dataSpec);
                    }
                    cur = fallback.createDataSource();
                    return cur.open(dataSpec);
                }

                @Override
                public int read(byte[] buffer, int offset, int length) throws IOException {
                    return cur != null ? cur.read(buffer, offset, length) : C.RESULT_END_OF_INPUT;
                }

                @Override
                public Uri getUri() {
                    return cur != null ? cur.getUri() : opened;
                }

                @Override
                public Map<String, List<String>> getResponseHeaders() {
                    return cur != null ? cur.getResponseHeaders() : java.util.Collections.emptyMap();
                }

                @Override
                public void close() throws IOException {
                    if (cur != null) {
                        cur.close();
                        cur = null;
                    }
                }
            };
        }
    }

    /** 清单 URI 读内存；其它 URI 走下游（分片）。 */
    private static final class InlineThenNetFactory implements DataSource.Factory {
        private final byte[] body;
        private final Uri inline;
        private final DataSource.Factory net;

        InlineThenNetFactory(byte[] body, Uri inline, DataSource.Factory net) {
            this.body = body;
            this.inline = inline;
            this.net = net;
        }

        @Override
        public DataSource createDataSource() {
            return new DataSource() {
                private DataSource cur;

                @Override
                public void addTransferListener(TransferListener transferListener) {
                    /* 子源各自监听即可 */
                }

                @Override
                public long open(DataSpec dataSpec) throws IOException {
                    close();
                    if (inline.equals(dataSpec.uri)
                            || "msjs".equalsIgnoreCase(dataSpec.uri.getScheme())) {
                        cur = new ByteArrayDataSource(body);
                    } else {
                        cur = net.createDataSource();
                    }
                    return cur.open(dataSpec);
                }

                @Override
                public int read(byte[] buffer, int offset, int length) throws IOException {
                    return cur != null ? cur.read(buffer, offset, length) : C.RESULT_END_OF_INPUT;
                }

                @Override
                public Uri getUri() {
                    return cur != null ? cur.getUri() : null;
                }

                @Override
                public Map<String, List<String>> getResponseHeaders() {
                    return cur != null ? cur.getResponseHeaders() : java.util.Collections.emptyMap();
                }

                @Override
                public void close() throws IOException {
                    if (cur != null) {
                        cur.close();
                        cur = null;
                    }
                }
            };
        }
    }

    /** shw:/sys: + js-rules 的 soft:/soft_hw:（MsMedia 均走自研探测，soft≈shw）。 */
    private static String stripJsPlayUrl(String playUrl) {
        String s = PlayEngine.strip(playUrl);
        if (s == null) {
            s = "";
        }
        String low = s.toLowerCase(Locale.US);
        if (low.startsWith("soft_hw:")) {
            s = s.substring(8);
        } else if (low.startsWith("soft:")) {
            s = s.substring(5);
        }
        if (s.startsWith("//")) {
            s = s.substring(2);
        }
        return s.trim();
    }

    private static Map<String, String> parseHdrBlock(String block) {
        Map<String, String> m = new HashMap<String, String>();
        if (block == null || block.length() == 0) {
            return m;
        }
        String[] lines = block.split("\n");
        for (int i = 0; i < lines.length; i++) {
            String line = lines[i].trim();
            if (line.length() == 0) {
                continue;
            }
            int c = line.indexOf(':');
            if (c <= 0) {
                continue;
            }
            String k = line.substring(0, c).trim();
            String v = line.substring(c + 1).trim();
            if (k.length() > 0 && v.length() > 0) {
                m.put(k, v);
            }
        }
        return m;
    }

    private static DataSource.Factory factoryForPlayUrl(String url, Map<String, String> hdrs) {
        String low = url.toLowerCase(Locale.US);
        if (low.startsWith("rtsp:") || low.startsWith("rtmp:") || low.startsWith("rtmps:")
                || low.startsWith("udp:") || low.startsWith("rtp:")) {
            return new PrefixStripDataSource.Factory(new FfDataSource.Factory().setTimeoutMs(15000));
        }
        if (low.startsWith("file:") || (low.startsWith("/") && !low.contains("://"))) {
            return new PrefixStripDataSource.Factory(new FileDataSource.Factory());
        }
        /* 嵌套再探一般是 http(s)；asset 仅用于入口脚本，不再次走 asset */
        JniHttpDataSource.Factory http = new JniHttpDataSource.Factory().setTlsInsecure(true);
        if (hdrs != null && !hdrs.isEmpty()) {
            http.setDefaultRequestProperties(hdrs);
        }
        return new PrefixStripDataSource.Factory(http);
    }

    /** 一次 open + 读头；首次同 URI 打开续传，其它请求走 base 新连接。 */
    private static final class Head {
        final Uri uri;
        final DataSource live;
        final byte[] buf;
        final int len;
        final long openLen;
        final int kind;
        /** 来自原 URL 的茶模式；请求 URI 已剥 query，不能再 a.q(uri) */
        final int tea;
        boolean adopted;

        private Head(Uri uri, DataSource live, byte[] buf, int len, long openLen, int kind,
                int tea) {
            this.uri = uri;
            this.live = live;
            this.buf = buf;
            this.len = len;
            this.openLen = openLen;
            this.kind = kind;
            this.tea = tea;
        }

        static Head open(DataSource.Factory f, Uri uri) throws IOException {
            int tea = a.q(uri);
            Uri req = a.s(uri);
            if (req == null) {
                req = uri;
            }
            DataSource ds = f.createDataSource();
            long openLen = ds.open(new DataSpec(req));
            boolean jsUrl = looksJsUrl(uri);
            byte[] buf = new byte[jsUrl ? SCRIPT_CAP : HEAD_CAP];
            int n = 0;
            int kind = ContentSniff.MORE;
            while (n < buf.length) {
                int r = ds.read(buf, n, buf.length - n);
                if (r == C.RESULT_END_OF_INPUT || r <= 0) {
                    kind = ContentSniff.sniff(buf, n, true);
                    break;
                }
                n += r;
                kind = ContentSniff.sniff(buf, n, false);
                if (kind == ContentSniff.JS || (jsUrl && kind == ContentSniff.MORE)) {
                    /* 脚本：读完再定；asset/.js 即使暂未扫到 main 也继续读 */
                    if (buf.length < SCRIPT_CAP) {
                        byte[] nb = new byte[SCRIPT_CAP];
                        System.arraycopy(buf, 0, nb, 0, n);
                        buf = nb;
                    }
                    while (n < SCRIPT_CAP) {
                        int r2 = ds.read(buf, n, SCRIPT_CAP - n);
                        if (r2 == C.RESULT_END_OF_INPUT || r2 <= 0) {
                            break;
                        }
                        n += r2;
                    }
                    kind = ContentSniff.sniff(buf, n, true);
                    if (kind != ContentSniff.JS && jsUrl) {
                        /* 路径是脚本但正文无 main：仍当 JS 交给 unwrap/eval 报错，勿当媒体 */
                        kind = ContentSniff.JS;
                    }
                    break;
                }
                if (kind != ContentSniff.MORE) {
                    break;
                }
                if (n >= HEAD_CAP && kind == ContentSniff.MORE) {
                    kind = ContentSniff.PROG;
                    break;
                }
            }
            if (kind == ContentSniff.MORE) {
                kind = jsUrl ? ContentSniff.JS : ContentSniff.PROG;
            }
            /* JS 须保留原始 URI 的 query（id/page），resolved 常被 Asset/重定向剥掉 */
            Uri keep = kind == ContentSniff.JS ? uri
                    : (ds.getUri() != null ? ds.getUri() : req);
            return new Head(keep, ds, buf, n, openLen, kind, tea);
        }

        byte[] scriptBytes() {
            byte[] out = new byte[len];
            System.arraycopy(buf, 0, out, 0, len);
            return out;
        }

        void abandon() {
            if (!adopted) {
                try {
                    live.close();
                } catch (IOException ignored) {
                }
                adopted = true; /* 防止 Continue 误用已关连接 */
            }
        }

        DataSource.Factory asFactory(DataSource.Factory base) {
            return () -> new ContinueDataSource(this, base);
        }
    }

    private static final class ContinueDataSource implements DataSource {
        private final Head head;
        private final DataSource.Factory base;
        private final List<TransferListener> listeners = new ArrayList<TransferListener>(1);
        private DataSource nested;
        private boolean usingHead;
        private int headPos;
        private boolean opened;

        ContinueDataSource(Head head, DataSource.Factory base) {
            this.head = head;
            this.base = base;
        }

        @Override
        public void addTransferListener(TransferListener transferListener) {
            listeners.add(transferListener);
            if (nested != null) {
                nested.addTransferListener(transferListener);
            }
        }

        @Override
        public long open(DataSpec dataSpec) throws IOException {
            close();
            synchronized (head) {
                if (!head.adopted && sameUri(head.uri, dataSpec.uri) && dataSpec.position == 0) {
                    head.adopted = true;
                    usingHead = true;
                    headPos = 0;
                    for (int i = 0; i < listeners.size(); i++) {
                        head.live.addTransferListener(listeners.get(i));
                    }
                    opened = true;
                    return head.openLen;
                }
            }
            nested = base.createDataSource();
            for (int i = 0; i < listeners.size(); i++) {
                nested.addTransferListener(listeners.get(i));
            }
            long len = nested.open(dataSpec);
            opened = true;
            return len;
        }

        @Override
        public int read(byte[] buffer, int offset, int length) throws IOException {
            if (!opened) {
                throw new IOException("not open");
            }
            if (usingHead) {
                if (headPos < head.len) {
                    int n = Math.min(length, head.len - headPos);
                    System.arraycopy(head.buf, headPos, buffer, offset, n);
                    headPos += n;
                    return n;
                }
                return head.live.read(buffer, offset, length);
            }
            return nested.read(buffer, offset, length);
        }

        @Override
        public Uri getUri() {
            if (usingHead) {
                return head.live.getUri();
            }
            return nested != null ? nested.getUri() : null;
        }

        @Override
        public Map<String, List<String>> getResponseHeaders() {
            if (usingHead) {
                return head.live.getResponseHeaders();
            }
            return nested != null
                    ? nested.getResponseHeaders()
                    : java.util.Collections.<String, List<String>>emptyMap();
        }

        @Override
        public void close() throws IOException {
            if (!opened) {
                return;
            }
            opened = false;
            if (usingHead) {
                usingHead = false;
                head.live.close();
            } else if (nested != null) {
                nested.close();
                nested = null;
            }
        }

        private static boolean sameUri(Uri ua, Uri ub) {
            if (ua == null || ub == null) {
                return false;
            }
            if (ua.equals(ub) || ua.toString().equals(ub.toString())) {
                return true;
            }
            /* ysp-tea 剥参后仍应对上探测连接 */
            Uri sa = a.s(ua);
            Uri sb = a.s(ub);
            return sa != null && sb != null
                    && (sa.equals(sb) || sa.toString().equals(sb.toString()));
        }
    }
}
