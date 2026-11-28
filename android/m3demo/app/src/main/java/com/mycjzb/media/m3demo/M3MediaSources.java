package com.mycjzb.media.m3demo;

import android.net.Uri;
import android.text.TextUtils;

import androidx.media3.common.MediaItem;
import androidx.media3.common.MimeTypes;
import androidx.media3.datasource.DefaultHttpDataSource;
import androidx.media3.exoplayer.hls.HlsMediaSource;
import androidx.media3.exoplayer.source.MediaSource;
import androidx.media3.exoplayer.source.ProgressiveMediaSource;
import androidx.media3.extractor.DefaultExtractorsFactory;
import androidx.media3.extractor.ts.TsExtractor;

import java.util.Locale;

/** 按 URL 选 HLS / Progressive（TS/FLV/MP4）；与超级直播 ClearPlayMediaSource 同思路，无业务解密。 */
final class M3MediaSources {
    private M3MediaSources() {}

    static MediaSource create(String url) {
        if (TextUtils.isEmpty(url)) {
            throw new IllegalArgumentException("empty url");
        }
        DefaultHttpDataSource.Factory http = new DefaultHttpDataSource.Factory()
                .setUserAgent("MsMedia-M3Demo/1.0")
                .setConnectTimeoutMs(8000)
                .setReadTimeoutMs(8000)
                .setAllowCrossProtocolRedirects(true);
        String low = url.toLowerCase(Locale.US);
        if (low.contains(".m3u8") || low.contains("/hls/") || low.contains("format=m3u8")) {
            return new HlsMediaSource.Factory(http)
                    .createMediaSource(new MediaItem.Builder().setUri(Uri.parse(url))
                            .setMimeType(MimeTypes.APPLICATION_M3U8).build());
        }
        DefaultExtractorsFactory extractors = new DefaultExtractorsFactory()
                .setTsExtractorMode(TsExtractor.MODE_MULTI_PMT);
        String mime = null;
        if (low.contains(".flv")) {
            mime = MimeTypes.VIDEO_FLV;
        } else if (low.contains(".mp4") || low.contains(".m4v")) {
            mime = MimeTypes.VIDEO_MP4;
        } else if (low.contains(".ts") || low.contains("/rtp/") || low.contains("/udp/")) {
            mime = MimeTypes.VIDEO_MP2T;
        }
        MediaItem.Builder b = new MediaItem.Builder().setUri(Uri.parse(url));
        if (mime != null) {
            b.setMimeType(mime);
        }
        return new ProgressiveMediaSource.Factory(http, extractors).createMediaSource(b.build());
    }
}
