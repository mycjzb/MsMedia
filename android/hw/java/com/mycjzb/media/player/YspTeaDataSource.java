package com.mycjzb.media.player;

import android.net.Uri;

import androidx.media3.common.C;
import androidx.media3.datasource.DataSource;
import androidx.media3.datasource.DataSpec;
import androidx.media3.datasource.TransferListener;

import com.mycjzb.media.player.n.a;

import java.io.IOException;
import java.util.List;
import java.util.Map;

/**
 * 上游 HTTP 读密文 TS → n.a feed/flush → 明文。
 * {@code mode==0} 时只剥 ysp-tea query，不做解密（清单/密钥用）。
 */
final class YspTeaDataSource implements DataSource {
    private final DataSource upstream;
    private final int mode;
    private long handle;
    private byte[] pending;
    private int pendingOff;
    private boolean opened;

    YspTeaDataSource(DataSource upstream, int mode) {
        this.upstream = upstream;
        this.mode = mode;
    }

    static final class Factory implements DataSource.Factory {
        private final DataSource.Factory upstream;
        private final int mode;

        Factory(DataSource.Factory upstream, int mode) {
            this.upstream = upstream;
            this.mode = mode;
        }

        @Override
        public DataSource createDataSource() {
            return new YspTeaDataSource(upstream.createDataSource(), mode);
        }
    }

    @Override
    public void addTransferListener(TransferListener transferListener) {
        upstream.addTransferListener(transferListener);
    }

    @Override
    public long open(DataSpec dataSpec) throws IOException {
        close();
        Uri u = dataSpec.uri;
        Uri stripped = a.s(u);
        DataSpec openSpec = dataSpec;
        if (stripped != null && !stripped.equals(u)) {
            openSpec = dataSpec.buildUpon().setUri(stripped).build();
        }
        long len = upstream.open(openSpec);
        /* mode 由 Factory 固定；0=只剥 query（清单），勿再从 URI 推断以免清单误进 TEA */
        if (mode == a.m1 || mode == a.m2) {
            if (!a.r()) {
                upstream.close();
                throw new IOException("n.a unavailable");
            }
            handle = a.o(mode);
            if (handle == 0) {
                upstream.close();
                throw new IOException("n.a o failed mode=" + mode);
            }
        }
        opened = true;
        return len;
    }

    @Override
    public int read(byte[] buffer, int offset, int length) throws IOException {
        if (!opened) {
            throw new IOException("not open");
        }
        if (handle == 0) {
            return upstream.read(buffer, offset, length);
        }
        int copied = drainPending(buffer, offset, length);
        if (copied > 0) {
            return copied;
        }
        byte[] in = new byte[Math.min(Math.max(length, 4096), 64 * 1024)];
        int n = upstream.read(in, 0, in.length);
        if (n == C.RESULT_END_OF_INPUT) {
            byte[] tail = a.u(handle);
            if (tail == null) {
                throw new IOException("n.a u failed");
            }
            if (tail.length == 0) {
                return C.RESULT_END_OF_INPUT;
            }
            pending = tail;
            pendingOff = 0;
            return drainPending(buffer, offset, length);
        }
        if (n < 0) {
            return n;
        }
        byte[] out = a.f(handle, in, 0, n);
        if (out == null) {
            throw new IOException("n.a f failed");
        }
        if (out.length == 0) {
            return read(buffer, offset, length);
        }
        pending = out;
        pendingOff = 0;
        return drainPending(buffer, offset, length);
    }

    private int drainPending(byte[] buffer, int offset, int length) {
        if (pending == null || pendingOff >= pending.length) {
            pending = null;
            pendingOff = 0;
            return 0;
        }
        int n = Math.min(length, pending.length - pendingOff);
        System.arraycopy(pending, pendingOff, buffer, offset, n);
        pendingOff += n;
        if (pendingOff >= pending.length) {
            pending = null;
            pendingOff = 0;
        }
        return n;
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
        if (!opened && handle == 0) {
            return;
        }
        opened = false;
        pending = null;
        pendingOff = 0;
        if (handle != 0) {
            try {
                a.c(handle);
            } catch (Throwable ignored) {
            }
            handle = 0;
        }
        upstream.close();
    }
}
