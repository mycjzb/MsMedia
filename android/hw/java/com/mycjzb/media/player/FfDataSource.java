package com.mycjzb.media.player;

import android.net.Uri;

import androidx.media3.common.C;
import androidx.media3.common.PlaybackException;
import androidx.media3.datasource.BaseDataSource;
import androidx.media3.datasource.DataSource;
import androidx.media3.datasource.DataSourceException;
import androidx.media3.datasource.DataSpec;

import java.io.IOException;
import java.util.LinkedHashMap;
import java.util.Map;

/**
 * Media3 DataSource：字节来自 FFmpeg AVIO（仅 rtsp/rtmp/udp/rtp 等非 HTTP 协议）。
 * HTTP(S)/HLS/DASH 分片走 {@link JniHttpDataSource}。
 */
final class FfDataSource extends BaseDataSource {
    static final class Factory implements DataSource.Factory {
        private int timeoutMs = 15000;
        private Map<String, String> defaultHeaders;

        Factory setTimeoutMs(int ms) {
            timeoutMs = ms > 0 ? ms : 15000;
            return this;
        }

        /** rtmp 映射到 Connect AMF，rtsp 取 user_agent；udp/rtp 忽略。 */
        Factory setDefaultRequestProperties(Map<String, String> headers) {
            defaultHeaders = headers != null && !headers.isEmpty()
                    ? new LinkedHashMap<String, String>(headers) : null;
            return this;
        }

        @Override
        public DataSource createDataSource() {
            return new FfDataSource(timeoutMs, defaultHeaders);
        }
    }

    private final int timeoutMs;
    private final Map<String, String> defaultHeaders;
    private Uri uri;
    private long handle;
    private long bytesRead;
    private long bytesToRead = C.LENGTH_UNSET;
    private boolean opened;

    FfDataSource(int timeoutMs, Map<String, String> defaultHeaders) {
        super(/* isNetwork= */ true);
        this.timeoutMs = timeoutMs;
        this.defaultHeaders = defaultHeaders;
    }

    /* DataSpec 头优先，其余用 Factory 的默认头。FFmpeg 认 "Name: value\r\n"。 */
    private String requestHeaders(DataSpec dataSpec) {
        if (defaultHeaders == null && dataSpec.httpRequestHeaders.isEmpty()) {
            return null;
        }
        StringBuilder sb = new StringBuilder();
        if (defaultHeaders != null) {
            appendHeaders(sb, defaultHeaders);
        }
        appendHeaders(sb, dataSpec.httpRequestHeaders);
        return sb.length() > 0 ? sb.toString() : null;
    }

    private static void appendHeaders(StringBuilder sb, Map<String, String> headers) {
        for (Map.Entry<String, String> e : headers.entrySet()) {
            String name = e.getKey();
            String value = e.getValue();
            if (name != null && value != null && value.trim().length() > 0) {
                sb.append(name).append(": ").append(value.trim()).append("\r\n");
            }
        }
    }

    @Override
    public long open(DataSpec dataSpec) throws IOException {
        close();
        this.uri = dataSpec.uri;
        bytesRead = 0;
        bytesToRead = C.LENGTH_UNSET;
        transferInitializing(dataSpec);
        String url = dataSpec.uri != null ? dataSpec.uri.toString() : "";
        if (url.length() == 0) {
            throw new DataSourceException(new IOException("empty url"),
                    PlaybackException.ERROR_CODE_FAILED_RUNTIME_CHECK);
        }
        /* Range：有 position 时先开再 seek（HTTP AVIO 可处理）。 */
        handle = FfIo.nativeOpenEx(url, timeoutMs, requestHeaders(dataSpec));
        if (handle == 0) {
            throw new DataSourceException(new IOException("ff open fail"),
                    PlaybackException.ERROR_CODE_IO_NETWORK_CONNECTION_FAILED);
        }
        if (dataSpec.position > 0) {
            long pos = FfIo.nativeSeek(handle, dataSpec.position, 0);
            if (pos < 0) {
                String err = FfIo.nativeLastErr(handle);
                FfIo.nativeClose(handle);
                handle = 0;
                throw new DataSourceException(new IOException("ff seek " + err),
                        PlaybackException.ERROR_CODE_IO_READ_POSITION_OUT_OF_RANGE);
            }
        }
        if (dataSpec.length != C.LENGTH_UNSET) {
            bytesToRead = dataSpec.length;
        } else {
            long sz = FfIo.nativeSize(handle);
            if (sz >= 0 && dataSpec.position >= 0 && sz >= dataSpec.position) {
                bytesToRead = sz - dataSpec.position;
            }
        }
        opened = true;
        transferStarted(dataSpec);
        return bytesToRead;
    }

    @Override
    public int read(byte[] buffer, int offset, int length) throws IOException {
        if (length == 0) {
            return 0;
        }
        if (bytesToRead != C.LENGTH_UNSET) {
            long left = bytesToRead - bytesRead;
            if (left == 0) {
                return C.RESULT_END_OF_INPUT;
            }
            length = (int) Math.min(length, left);
        }
        int n = FfIo.nativeRead(handle, buffer, offset, length);
        if (n == 0) {
            return C.RESULT_END_OF_INPUT;
        }
        if (n < 0) {
            throw new DataSourceException(
                    new IOException("ff read " + FfIo.nativeLastErr(handle)),
                    PlaybackException.ERROR_CODE_IO_UNSPECIFIED);
        }
        bytesRead += n;
        bytesTransferred(n);
        return n;
    }

    @Override
    public Uri getUri() {
        return uri;
    }

    @Override
    public void close() {
        if (handle != 0) {
            FfIo.nativeClose(handle);
            handle = 0;
        }
        if (opened) {
            transferEnded();
            opened = false;
        }
        uri = null;
    }
}
