package com.mycjzb.media.player;

import android.net.Uri;

import androidx.media3.common.C;
import androidx.media3.datasource.DataSource;
import androidx.media3.datasource.DataSpec;
import androidx.media3.datasource.TransferListener;

import java.io.IOException;
import java.util.List;
import java.util.Map;

/**
 * 打开后先读一段前缀，经 {@link MsProbe} 剥图片伪装头，再把真正媒体交给下游读。
 */
final class PrefixStripDataSource implements DataSource {
    private static final int PEEK_CAP = 256 * 1024;

    static final class Factory implements DataSource.Factory {
        private final DataSource.Factory upstream;

        Factory(DataSource.Factory upstream) {
            this.upstream = upstream;
        }

        @Override
        public DataSource createDataSource() {
            return new PrefixStripDataSource(upstream.createDataSource());
        }
    }

    private final DataSource upstream;
    private byte[] hold;
    private int holdOff;
    private int holdLen;
    private boolean opened;

    PrefixStripDataSource(DataSource upstream) {
        this.upstream = upstream;
    }

    @Override
    public void addTransferListener(TransferListener transferListener) {
        upstream.addTransferListener(transferListener);
    }

    @Override
    public long open(DataSpec dataSpec) throws IOException {
        close();
        long len = upstream.open(dataSpec);
        /* JS 入口不应进本类；若仍进到，整文件透传，禁止剥头 */
        if (uriLooksJs(dataSpec != null ? dataSpec.uri : null)) {
            return openPassthrough(len);
        }
        byte[] peek = new byte[PEEK_CAP];
        int n = 0;
        int off = 0;
        while (n < PEEK_CAP) {
            int r = upstream.read(peek, n, PEEK_CAP - n);
            if (r == C.RESULT_END_OF_INPUT || r <= 0) {
                break;
            }
            n += r;
            /* MSJS/gzip/明文 main：整段透传（含伪装成 .m3u8 的加密 JS） */
            if (ContentSniff.isJsPayload(peek, n)) {
                return drainHold(peek, n, len);
            }
            int o = MsProbe.mediaOffset(peek, n, false);
            if (o == -1) {
                continue; /* 还要更多字节 */
            }
            off = Math.max(0, o);
            if (off > n) {
                off = 0;
            }
            return finishHold(peek, n, off, len);
        }
        if (ContentSniff.isJsPayload(peek, n)) {
            return finishHold(peek, n, 0, len);
        }
        off = MsProbe.mediaOffset(peek, n, true);
        if (off < 0) {
            off = 0;
        }
        if (off > n) {
            off = 0;
        }
        return finishHold(peek, n, off, len);
    }

    private long openPassthrough(long len) throws IOException {
        byte[] peek = new byte[PEEK_CAP];
        int n = 0;
        while (n < PEEK_CAP) {
            int r = upstream.read(peek, n, PEEK_CAP - n);
            if (r == C.RESULT_END_OF_INPUT || r <= 0) {
                break;
            }
            n += r;
        }
        return finishHold(peek, n, 0, len);
    }

    /** 已确认 JS 容器：尽量多读进 hold，剩余仍走 upstream */
    private long drainHold(byte[] peek, int n, long len) throws IOException {
        while (n < PEEK_CAP) {
            int r = upstream.read(peek, n, PEEK_CAP - n);
            if (r == C.RESULT_END_OF_INPUT || r <= 0) {
                break;
            }
            n += r;
        }
        return finishHold(peek, n, 0, len);
    }

    private long finishHold(byte[] peek, int n, int off, long len) {
        hold = peek;
        holdOff = off;
        holdLen = n - off;
        opened = true;
        return adjustLen(len, off);
    }

    private static boolean uriLooksJs(Uri uri) {
        if (uri == null) {
            return false;
        }
        String p = uri.getPath();
        if (p == null) {
            return false;
        }
        String low = p.toLowerCase(java.util.Locale.US);
        return low.endsWith(".js") || low.endsWith(".msjs");
    }

    private static long adjustLen(long len, int off) {
        if (len == C.LENGTH_UNSET) {
            return C.LENGTH_UNSET;
        }
        return Math.max(0L, len - off);
    }

    @Override
    public int read(byte[] buffer, int offset, int length) throws IOException {
        if (!opened) {
            throw new IOException("not open");
        }
        if (hold != null && holdLen > 0) {
            int n = Math.min(length, holdLen);
            System.arraycopy(hold, holdOff, buffer, offset, n);
            holdOff += n;
            holdLen -= n;
            if (holdLen == 0) {
                hold = null;
            }
            return n;
        }
        return upstream.read(buffer, offset, length);
    }

    @Override
    public Uri getUri() {
        return upstream.getUri();
    }

    @Override
    public Map<String, List<String>> getResponseHeaders() {
        return upstream.getResponseHeaders();
    }

    @Override
    public void close() throws IOException {
        hold = null;
        holdOff = holdLen = 0;
        opened = false;
        upstream.close();
    }
}
