package com.mycjzb.media.player;

/**
 * URL 形态提示（弱提示，不参与选源/isLive）。
 * 选源看字节头（msm3 {@code ByteHeadSource}）；直播/点播真源是 {@link MediaPlayer#isLive()}。
 */
public final class LiveDetect {
    private LiveDetect() {}

    /**
     * 仅 URL 提示；不等于已确认 HLS/DASH 内容，也不参与选源。
     * 无扩展名的地址不要靠本方法硬判直播。
     */
    public static boolean isHlsOrDashUrl(String url) {
        if (url == null || url.length() == 0) return false;
        String u = url.toLowerCase();
        return u.contains(".m3u8") || u.contains("/hls/") || u.contains("format=m3u8")
                || u.contains(".mpd") || u.contains("/dash/") || u.contains("format=mpd");
    }

    /** @deprecated 用 {@link #isHlsOrDashUrl}；语义仅为 URL 提示。 */
    @Deprecated
    public static boolean isHlsOrDash(String url) {
        return isHlsOrDashUrl(url);
    }

    /** 无点播总时长的传输协议（可选提示）。 */
    public static boolean isLiveProto(String url) {
        if (url == null || url.length() == 0) return false;
        String u = url.trim().toLowerCase();
        return u.startsWith("udp:") || u.startsWith("rtp:")
                || u.startsWith("rtsp:") || u.startsWith("rtmp:") || u.startsWith("rtmps:")
                || u.startsWith("dvb:");
    }
}
