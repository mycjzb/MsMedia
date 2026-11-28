package com.mycjzb.media.player;

import android.content.Context;
import android.net.Uri;
import android.view.Surface;
import android.view.SurfaceHolder;

import java.io.IOException;
import java.util.Map;

/**
 * {@link PlayEngine#SHW} 可替换后端（Media3 内核）。无 Factory 时 {@link MediaPlayer} 仍走 libmsplayer 全路径，
 * 保证无 Media3 的 single APK 能编过。
 */
public interface ShwBackend {
    interface Host {
        void onPrepared();
        void onCompletion();
        boolean onError(int what, int extra);
        boolean onInfo(int what, int extra);
        void onSeekComplete();
        void onBufferingUpdate(int percent);
        void onVideoSizeChanged(int w, int h);
        /** Media3 onCues；null=清屏。 */
        void onTimedText(TimedText text);
    }

    interface Factory {
        ShwBackend create(Context context);
    }

    void setHost(Host host);

    void setDataSource(String path) throws IOException;

    void setDataSource(Context ctx, Uri uri, Map<String, String> headers) throws IOException;

    void setSurface(Surface s);

    void setDisplay(SurfaceHolder h);

    void prepareAsync();

    void start();

    void pause();

    void stop();

    void reset();

    void release();

    void seekTo(int msec);

    int getCurrentPosition();

    int getDuration();

    boolean isPlaying();

    int getVideoWidth();

    int getVideoHeight();

    /** 演示 OSD：容器/解码 MIME，无则 null。 */
    String videoMime();

    String audioMime();

    /** 演示 OSD：帧率，未知 0。 */
    float videoFrameRate();

    /** 演示 OSD：码率 bps，未知 -1。 */
    int videoBitrate();

    int audioBitrate();

    void setVolume(float l, float r);

    void setLooping(boolean loop);

    boolean isLooping();

    /** Timeline/窗 live；未知时 true（偏保守，避免误出 seek 条）。 */
    boolean isLive();

    /** 扁平 AUDIO/TIMEDTEXT（对齐超级直播 flat）；未就绪返回空数组。 */
    MediaPlayer.TrackInfo[] getTrackInfo();

    void selectTrack(int flatIndex);

    void deselectTrack(int flatIndex);

    int getSelectedTrack(int trackType);

    /**
     * 外挂字幕路径/URI；null 清除。prepare 前设置，已起播则重建 MediaSource。
     * mime 用 {@link MediaPlayer#MEDIA_MIMETYPE_TEXT_SUBRIP} 等。
     */
    void setExternalTimedText(String pathOrUri, String mime);

    /** 5 段 PCM 能量；透传无 PCM 时恒 false。 */
    boolean readAudioBars(int[] out);

    /** 对齐 {@link MediaPlayer#setBufferConfig}；下次 prepare 生效。 */
    void setBufferConfig(int minMs, int maxMs, int playMs, int rebufMs, int maxBytes);

    /** true=严格校验证书；默认 false（播放忽略）。下次 prepare 生效。 */
    void setPlaybackTlsVerify(boolean strict);

    /** AC-3/E-AC-3/DTS HDMI 直通；false 强制 PCM。下次 prepare 生效。 */
    void setAudioPassthroughEnabled(boolean enabled);
}
