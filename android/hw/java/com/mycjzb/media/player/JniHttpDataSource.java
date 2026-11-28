package com.mycjzb.media.player;

import android.net.Uri;
import android.text.TextUtils;

import androidx.media3.common.C;
import androidx.media3.common.PlaybackException;
import androidx.media3.datasource.BaseDataSource;
import androidx.media3.datasource.DataSource;
import androidx.media3.datasource.DataSourceException;
import androidx.media3.datasource.DataSpec;
import androidx.media3.datasource.HttpDataSource;

import com.mycjzb.media.player.MsHttpClient;

import java.io.IOException;
import java.io.InputStream;
import java.util.Collections;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

/**
 * Media3 HTTP(S) DataSource：请求形态跟 DefaultHttpDataSource（含 Range），
 * 底层走 {@link MsHttpClient} JNI（与 slimff 播放器同一套网）。
 */
final class JniHttpDataSource extends BaseDataSource {

    static final class Factory implements DataSource.Factory {
        private Map<String, String> defaultHeaders = Collections.emptyMap();
        private boolean tlsInsecure = true; /* 默认播放忽略证书 */

        Factory setDefaultRequestProperties(Map<String, String> headers) {
            defaultHeaders = headers == null || headers.isEmpty()
                    ? Collections.<String, String>emptyMap()
                    : new HashMap<String, String>(headers);
            return this;
        }

        /** false=严格 TLS（对应 MediaPlayer.setPlaybackTlsVerify(true)）。 */
        Factory setTlsInsecure(boolean insecure) {
            tlsInsecure = insecure;
            return this;
        }

        @Override
        public DataSource createDataSource() {
            MsHttpClient client = new MsHttpClient.Builder()
                    .connectTimeout(8000).readTimeout(15000).tlsInsecure(tlsInsecure).build();
            return new JniHttpDataSource(client, defaultHeaders);
        }
    }

    private final MsHttpClient client;
    private final Map<String, String> defaultHeaders;
    private Uri uri;
    private DataSpec dataSpec;
    private MsHttpClient.Response response;
    private InputStream stream;
    private long bytesToRead = C.LENGTH_UNSET;
    private long bytesRead;
    private boolean opened;

    JniHttpDataSource(MsHttpClient client, Map<String, String> defaultHeaders) {
        super(/* isNetwork= */ true);
        this.client = client;
        this.defaultHeaders = defaultHeaders;
    }

    @Override
    public long open(DataSpec dataSpec) throws IOException {
        close();
        this.dataSpec = dataSpec;
        this.uri = dataSpec.uri;
        bytesRead = 0;
        bytesToRead = C.LENGTH_UNSET;
        transferInitializing(dataSpec);

        String url = dataSpec.uri != null ? dataSpec.uri.toString() : "";
        if (TextUtils.isEmpty(url)) {
            throw new DataSourceException(new IOException("empty url"),
                    PlaybackException.ERROR_CODE_FAILED_RUNTIME_CHECK);
        }

        MsHttpClient.RequestBuilder rb = new MsHttpClient.RequestBuilder().url(url).get();
        for (Map.Entry<String, String> e : defaultHeaders.entrySet()) {
            rb.header(e.getKey(), e.getValue());
        }
        if (dataSpec.httpRequestHeaders != null) {
            for (Map.Entry<String, String> e : dataSpec.httpRequestHeaders.entrySet()) {
                rb.header(e.getKey(), e.getValue());
            }
        }
        /* 对齐 Media3 DefaultHttpDataSource：按 DataSpec 发 Range */
        if (dataSpec.position != 0 || dataSpec.length != C.LENGTH_UNSET) {
            StringBuilder range = new StringBuilder("bytes=");
            range.append(dataSpec.position).append('-');
            if (dataSpec.length != C.LENGTH_UNSET) {
                range.append(dataSpec.position + dataSpec.length - 1);
            }
            rb.header("Range", range.toString());
        }

        try {
            response = client.newCall(rb.build()).execute();
        } catch (IOException e) {
            throw new HttpDataSource.HttpDataSourceException(e, dataSpec,
                    PlaybackException.ERROR_CODE_IO_NETWORK_CONNECTION_FAILED,
                    HttpDataSource.HttpDataSourceException.TYPE_OPEN);
        }

        int code = response.code();
        if (code < 200 || code > 299) {
            Map<String, List<String>> hdrs = response.headers();
            byte[] errBody = new byte[0];
            try {
                errBody = response.bytes();
            } catch (Throwable ignored) {}
            closeQuietly();
            throw new HttpDataSource.InvalidResponseCodeException(
                    code, null, null, hdrs, dataSpec, errBody);
        }

        long contentLength = response.contentLength();
        long skip = (code == 200 && dataSpec.position != 0) ? dataSpec.position : 0;
        if (dataSpec.length != C.LENGTH_UNSET) {
            bytesToRead = dataSpec.length;
        } else if (contentLength >= 0) {
            bytesToRead = contentLength - skip;
            if (bytesToRead < 0) {
                bytesToRead = C.LENGTH_UNSET;
            }
        }
        stream = response.body();
        opened = true;
        transferStarted(dataSpec);
        if (skip > 0) {
            skipFully(skip);
        }
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
        if (stream == null) {
            return C.RESULT_END_OF_INPUT;
        }
        int n = stream.read(buffer, offset, length);
        if (n < 0) {
            return C.RESULT_END_OF_INPUT;
        }
        if (n == 0) {
            return 0;
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
        closeQuietly();
        if (opened) {
            opened = false;
            transferEnded();
        }
        uri = null;
        dataSpec = null;
    }

    private void skipFully(long n) throws IOException {
        byte[] buf = new byte[4096];
        while (n > 0) {
            int want = (int) Math.min(n, buf.length);
            int r = stream.read(buf, 0, want);
            if (r < 0) {
                throw new HttpDataSource.HttpDataSourceException(
                        "EOS while skipping", dataSpec,
                        PlaybackException.ERROR_CODE_IO_READ_POSITION_OUT_OF_RANGE,
                        HttpDataSource.HttpDataSourceException.TYPE_OPEN);
            }
            n -= r;
            bytesTransferred(r);
        }
    }

    private void closeQuietly() {
        if (stream != null) {
            try { stream.close(); } catch (Throwable ignored) {}
            stream = null;
        }
        if (response != null) {
            try { response.close(); } catch (Throwable ignored) {}
            response = null;
        }
    }
}
