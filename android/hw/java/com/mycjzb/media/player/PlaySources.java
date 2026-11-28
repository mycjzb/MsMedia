package com.mycjzb.media.player;

import android.content.Context;
import android.net.Uri;
import android.text.TextUtils;

import androidx.media3.common.C;
import androidx.media3.common.MediaItem;
import androidx.media3.common.MimeTypes;
import androidx.media3.common.util.TimestampAdjuster;
import androidx.media3.datasource.AssetDataSource;
import androidx.media3.datasource.DataSource;
import androidx.media3.datasource.FileDataSource;
import androidx.media3.exoplayer.source.MediaSource;
import androidx.media3.exoplayer.source.MergingMediaSource;
import androidx.media3.exoplayer.source.SingleSampleMediaSource;
import androidx.media3.extractor.Extractor;
import androidx.media3.extractor.ExtractorsFactory;
import androidx.media3.extractor.avi.AviExtractor;
import androidx.media3.extractor.flac.FlacExtractor;
import androidx.media3.extractor.flv.FlvExtractor;
import androidx.media3.extractor.mkv.MatroskaExtractor;
import androidx.media3.extractor.mp3.Mp3Extractor;
import androidx.media3.extractor.mp4.FragmentedMp4Extractor;
import androidx.media3.extractor.mp4.Mp4Extractor;
import androidx.media3.extractor.text.DefaultSubtitleParserFactory;
import androidx.media3.extractor.text.SubtitleParser;
import androidx.media3.extractor.ts.Ac3Extractor;
import androidx.media3.extractor.ts.AdtsExtractor;
import androidx.media3.extractor.ts.DefaultTsPayloadReaderFactory;
import androidx.media3.extractor.ts.PsExtractor;
import androidx.media3.extractor.ts.TsExtractor;
import androidx.media3.extractor.wav.WavExtractor;

import java.io.File;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.Locale;
import java.util.Map;

/**
 * 取流/解封装；字幕：内嵌 + 外挂合并。
 * 选 HLS/DASH/Progressive：{@link ByteHeadSource} prepare 时同连接看字节头（不信扩名/Content-Type）。
 * 直播/点播以打开后 Timeline/时长为准。
 */
public final class PlaySources {
    private PlaySources() {}

    private static final SubtitleParser.Factory SUB_PARSERS = new DefaultSubtitleParserFactory();

    /**
     * 解封装期解析字幕 → {@code application/x-media3-cues}（Media3 默认关 legacy）。
     * TS 必须用带 {@link SubtitleParser.Factory} 且 flags=0 的构造；
     * {@code new TsExtractor(MODE_MULTI_PMT)} 会误走弃用重载并 {@code FLAG_EMIT_RAW_SUBTITLE_DATA}，
     * DVB 会炸：Legacy decoding disabled / application/dvbsubs。
     */
    private static final ExtractorsFactory EXTRACTORS = () -> new Extractor[] {
            new FlvExtractor(),
            new FlacExtractor(),
            new WavExtractor(),
            new FragmentedMp4Extractor(SUB_PARSERS),
            new Mp4Extractor(SUB_PARSERS),
            new PsExtractor(),
            new TsExtractor(
                    TsExtractor.MODE_MULTI_PMT,
                    /* extractorFlags= */ 0,
                    SUB_PARSERS,
                    new TimestampAdjuster(0),
                    new DefaultTsPayloadReaderFactory(/* flags= */ 0),
                    TsExtractor.DEFAULT_TIMESTAMP_SEARCH_BYTES),
            new MatroskaExtractor(SUB_PARSERS),
            new AviExtractor(/* flags= */ 0, SUB_PARSERS),
            new AdtsExtractor(),
            new Ac3Extractor(),
            new Mp3Extractor(),
    };

    public static MediaSource create(String url) {
        return create(null, url, Collections.<MediaItem.SubtitleConfiguration>emptyList(),
                null, true);
    }

    public static MediaSource create(Context context, String url) {
        return create(context, url, Collections.<MediaItem.SubtitleConfiguration>emptyList(),
                null, true);
    }

    public static MediaSource create(String url, List<MediaItem.SubtitleConfiguration> extraSubs) {
        return create(null, url, extraSubs, null, true);
    }

    public static MediaSource create(Context context, String url,
                                    List<MediaItem.SubtitleConfiguration> extraSubs) {
        return create(context, url, extraSubs, null, true);
    }

    /**
     * @param requestHeaders 可空；挂到 HTTP DataSource
     * @param tlsInsecure true=播放忽略证书（默认）；false=严格
     */
    public static MediaSource create(Context context, String url,
                                    List<MediaItem.SubtitleConfiguration> extraSubs,
                                    Map<String, String> requestHeaders,
                                    boolean tlsInsecure) {
        if (TextUtils.isEmpty(url)) {
            throw new IllegalArgumentException("empty url");
        }
        url = expandDemoJsUrl(url);
        String low = url.toLowerCase(Locale.US);
        DataSource.Factory ds = dataFactory(context, low, requestHeaders, tlsInsecure);
        List<MediaItem.SubtitleConfiguration> subs = new ArrayList<MediaItem.SubtitleConfiguration>();
        if (extraSubs != null) {
            subs.addAll(extraSubs);
        }
        for (MediaItem.SubtitleConfiguration c : discoverSidecarSubs(url, low)) {
            subs.add(c);
        }

        MediaSource main = new ByteHeadSource(normalizeUri(url, low), ds, EXTRACTORS);
        if (subs.isEmpty()) {
            return main;
        }
        SingleSampleMediaSource.Factory subFactory = new SingleSampleMediaSource.Factory(ds);
        MediaSource[] all = new MediaSource[1 + subs.size()];
        all[0] = main;
        for (int i = 0; i < subs.size(); i++) {
            all[i + 1] = subFactory.createMediaSource(subs.get(i), C.TIME_UNSET);
        }
        return new MergingMediaSource(all);
    }

    /**
     * Demo 短址展开（包内 assets/js）：
     *   {@code js?id=cctv1} → {@code asset:///js/yspweb.js?id=cctv1}
     *   {@code js://hebtv?id=hbws} → {@code asset:///js/hebtv_live.js?id=hbws}
     *   {@code js/body_demo.js?page=} → {@code asset:///js/body_demo.js?page=}
     */
    static String expandDemoJsUrl(String url) {
        String u = url.trim();
        String low = u.toLowerCase(Locale.US);
        if (low.startsWith("asset:")) {
            return u;
        }
        if (low.startsWith("js?") || low.startsWith("js/?") || low.equals("js")) {
            int q = u.indexOf('?');
            return "asset:///js/yspweb.js" + (q >= 0 ? u.substring(q) : "");
        }
        if (low.startsWith("js://")) {
            Uri p = Uri.parse(u);
            String host = p.getHost();
            String file = demoJsFile(host);
            String q = p.getEncodedQuery();
            return "asset:///js/" + file + (q != null && q.length() > 0 ? "?" + q : "");
        }
        if (low.startsWith("js/") && low.contains(".js")) {
            return "asset:///" + (u.charAt(0) == '/' ? u.substring(1) : u);
        }
        return u;
    }

    private static String demoJsFile(String host) {
        if (host == null || host.length() == 0) {
            return "yspweb.js";
        }
        String h = host.toLowerCase(Locale.US);
        if ("ysp".equals(h) || "yspweb".equals(h)) {
            return "yspweb.js";
        }
        if ("hebtv".equals(h)) {
            return "hebtv_live.js";
        }
        if ("mgtv".equals(h)) {
            return "mgtv_live.js";
        }
        if ("huya".equals(h)) {
            return "huya_live.js";
        }
        if ("ahtv".equals(h)) {
            return "ahtv_live.js";
        }
        if ("cctv".equals(h) || "cctvnews".equals(h)) {
            return "cctv_live.js";
        }
        if ("body".equals(h)) {
            return "body_demo.js";
        }
        if ("stream".equals(h)) {
            return "stream_hook_demo.js";
        }
        if ("web".equals(h) || "webpage".equals(h)) {
            return "web_page.js";
        }
        if ("yy".equals(h)) {
            return "yy_live.js";
        }
        if ("gdy".equals(h)) {
            return "gdy_tv.js";
        }
        return h.endsWith(".js") ? h : h + ".js";
    }

    /** 同目录旁路：video.mkv → video.srt / .vtt / .ass / .ssa（文件名配对，不是流类型判断） */
    static List<MediaItem.SubtitleConfiguration> discoverSidecarSubs(String url, String low) {
        List<MediaItem.SubtitleConfiguration> out = new ArrayList<MediaItem.SubtitleConfiguration>();
        if (!isLocalFile(low)) {
            return out;
        }
        String path = low.startsWith("file:") ? Uri.parse(url).getPath() : url;
        if (path == null) {
            return out;
        }
        int dot = path.lastIndexOf('.');
        if (dot <= 0) {
            return out;
        }
        String base = path.substring(0, dot);
        addIfExists(out, base + ".srt", MimeTypes.APPLICATION_SUBRIP);
        addIfExists(out, base + ".vtt", MimeTypes.TEXT_VTT);
        addIfExists(out, base + ".webvtt", MimeTypes.TEXT_VTT);
        addIfExists(out, base + ".ass", MimeTypes.TEXT_SSA);
        addIfExists(out, base + ".ssa", MimeTypes.TEXT_SSA);
        return out;
    }

    public static MediaItem.SubtitleConfiguration externalSub(String pathOrUri, String mime) {
        Uri u;
        String low = pathOrUri.toLowerCase(Locale.US);
        if (low.startsWith("/") && !low.contains("://")) {
            u = Uri.fromFile(new File(pathOrUri));
        } else {
            u = Uri.parse(pathOrUri);
        }
        /* 不加 DEFAULT：与系统一致，外挂需 selectTrack 才开 */
        return new MediaItem.SubtitleConfiguration.Builder(u)
                .setMimeType(mime)
                .setLanguage("zh")
                .build();
    }

    public static String guessSubMime(String path) {
        String low = path.toLowerCase(Locale.US);
        if (low.endsWith(".srt")) {
            return MimeTypes.APPLICATION_SUBRIP;
        }
        if (low.endsWith(".vtt") || low.endsWith(".webvtt")) {
            return MimeTypes.TEXT_VTT;
        }
        if (low.endsWith(".ass") || low.endsWith(".ssa")) {
            return MimeTypes.TEXT_SSA;
        }
        return MimeTypes.APPLICATION_SUBRIP;
    }

    private static void addIfExists(List<MediaItem.SubtitleConfiguration> out, String path,
                                   String mime) {
        if (new File(path).isFile()) {
            out.add(externalSub(path, mime));
        }
    }

    private static DataSource.Factory dataFactory(Context context, String low,
                                                  Map<String, String> requestHeaders,
                                                  boolean tlsInsecure) {
        DataSource.Factory raw;
        if (needsFfProtocol(low)) {
            FfDataSource.Factory ff = new FfDataSource.Factory().setTimeoutMs(15000);
            ff.setDefaultRequestProperties(requestHeaders);
            raw = ff;
        } else if (isAsset(low)) {
            if (context == null) {
                throw new IllegalArgumentException("asset:// needs Context");
            }
            final Context app = context.getApplicationContext() != null
                    ? context.getApplicationContext() : context;
            raw = () -> new AssetDataSource(app);
        } else if (isLocalFile(low)) {
            raw = new FileDataSource.Factory();
        } else {
            JniHttpDataSource.Factory http = new JniHttpDataSource.Factory()
                    .setTlsInsecure(tlsInsecure);
            if (requestHeaders != null && !requestHeaders.isEmpty()) {
                http.setDefaultRequestProperties(requestHeaders);
            }
            raw = http;
        }
        /* JS 规则脚本禁止走图片/媒体剥头：正文含 ORIGIN 等会误判 TS192，截成半截 → SyntaxError */
        if (isJsScript(low)) {
            return raw;
        }
        return new PrefixStripDataSource.Factory(raw);
    }

    private static boolean needsFfProtocol(String low) {
        return low.startsWith("rtsp:") || low.startsWith("rtmp:") || low.startsWith("rtmps:")
                || low.startsWith("udp:") || low.startsWith("rtp:");
    }

    /** asset/http 的 .js/.msjs（可带 query） */
    private static boolean isJsScript(String low) {
        if (low == null || low.length() == 0) {
            return false;
        }
        int q = low.indexOf('?');
        String p = q >= 0 ? low.substring(0, q) : low;
        return p.endsWith(".js") || p.endsWith(".msjs");
    }

    private static boolean isAsset(String low) {
        return low.startsWith("asset:");
    }

    private static boolean isLocalFile(String low) {
        return low.startsWith("file:") || (low.startsWith("/") && !low.contains("://"));
    }

    private static Uri normalizeUri(String url, String low) {
        if (low.startsWith("/") && !low.contains("://")) {
            return Uri.fromFile(new File(url));
        }
        return Uri.parse(url);
    }
}
