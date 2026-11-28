package com.mycjzb.media.player;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.Rect;
import android.net.Uri;
import android.os.Handler;
import android.os.Looper;
import android.view.Surface;
import android.view.SurfaceHolder;

import androidx.media3.common.C;
import androidx.media3.common.Format;
import androidx.media3.common.MediaItem;
import androidx.media3.common.MimeTypes;
import androidx.media3.common.PlaybackException;
import androidx.media3.common.Player;
import androidx.media3.common.TrackGroup;
import androidx.media3.common.TrackSelectionOverride;
import androidx.media3.common.Tracks;
import androidx.media3.common.VideoSize;
import androidx.media3.common.audio.AudioProcessor;
import androidx.media3.common.text.Cue;
import androidx.media3.common.text.CueGroup;
import androidx.media3.exoplayer.DefaultLoadControl;
import androidx.media3.exoplayer.DefaultRenderersFactory;
import androidx.media3.exoplayer.ExoPlayer;
import androidx.media3.exoplayer.audio.AudioCapabilities;
import androidx.media3.exoplayer.audio.AudioSink;
import androidx.media3.exoplayer.audio.DefaultAudioSink;
import androidx.media3.exoplayer.audio.TeeAudioProcessor;
import androidx.media3.exoplayer.trackselection.DefaultTrackSelector;

import java.io.IOException;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.Map;

/**
 * Media3（ExoPlayer）作 {@code shw:} 内核；选源走 {@link PlaySources}/{@link ByteHeadSource}（同连接字节头）。
 */
public final class M3PlayerBackend implements ShwBackend {
    private final Context app;
    private final Handler main = new Handler(Looper.getMainLooper());
    private Host host;
    private ExoPlayer player;
    private DefaultTrackSelector trackSelector;
    private String pendingUrl;
    private Map<String, String> pendingHeaders;
    private Surface surface;
    private SurfaceHolder holder;
    private float volL = 1f, volR = 1f;
    private boolean looping;
    private boolean prepared;
    private boolean released;
    /** 掉出直播窗口后追窗次数；成功 READY 清零。 */
    private int liveBehindTries;
    private static final int LIVE_BEHIND_MAX = 3;
    private final ExoAudioBarsSink barsSink = new ExoAudioBarsSink();
    private final ArrayList<Flat> flat = new ArrayList<Flat>();
    private String extSubPath;
    private String extSubMime;
    private int bufMinMs = MediaPlayer.DEFAULT_MIN_BUFFER_MS;
    private int bufMaxMs = MediaPlayer.DEFAULT_MAX_BUFFER_MS;
    private int bufPlayMs = MediaPlayer.DEFAULT_BUFFER_FOR_PLAYBACK_MS;
    private int bufRebufMs = MediaPlayer.DEFAULT_BUFFER_FOR_PLAYBACK_AFTER_REBUFFER_MS;
    private int bufMaxBytes = (int) MediaPlayer.DEFAULT_MAX_BUFFER_BYTES;
    private boolean tlsVerifyStrict;
    private boolean passthroughEnabled;
    private static final int SUB_CANVAS_W = 1280;
    private static final int SUB_CANVAS_H = 720;

    private static final class Flat {
        final TrackGroup group;
        final int inGroup;
        final int type;

        Flat(TrackGroup group, int inGroup, int type) {
            this.group = group;
            this.inGroup = inGroup;
            this.type = type;
        }
    }

    public M3PlayerBackend(Context context) {
        this.app = context.getApplicationContext() != null ? context.getApplicationContext() : context;
    }

    @Override
    public void setHost(Host host) {
        this.host = host;
    }

    @Override
    public void setDataSource(String path) throws IOException {
        if (path == null || path.length() == 0) {
            throw new IOException("empty path");
        }
        pendingUrl = path;
        pendingHeaders = null;
        prepared = false;
        liveBehindTries = 0;
    }

    @Override
    public void setDataSource(Context ctx, Uri uri, Map<String, String> headers) throws IOException {
        if (uri == null) {
            throw new IOException("null uri");
        }
        pendingUrl = uri.toString();
        pendingHeaders = headers;
        prepared = false;
        liveBehindTries = 0;
    }

    @Override
    public void setSurface(Surface s) {
        surface = s;
        holder = null;
        if (player != null) {
            player.setVideoSurface(s);
        }
    }

    @Override
    public void setDisplay(SurfaceHolder h) {
        holder = h;
        surface = null;
        if (player != null) {
            if (h != null) {
                player.setVideoSurfaceHolder(h);
            } else {
                player.setVideoSurface(null);
            }
        }
    }

    @Override
    public void prepareAsync() {
        if (released) {
            return;
        }
        main.post(new Runnable() {
            @Override
            public void run() {
                try {
                    bindAndPrepare();
                } catch (Throwable t) {
                    prepared = false;
                    fireError(1, -1004);
                }
            }
        });
    }

    private void bindAndPrepare() throws IOException {
        releasePlayerOnly();
        if (pendingUrl == null || pendingUrl.length() == 0) {
            throw new IOException("empty data source");
        }
        liveBehindTries = 0;
        DefaultLoadControl load = new DefaultLoadControl.Builder()
                .setBufferDurationsMs(bufMinMs, bufMaxMs, bufPlayMs, bufRebufMs)
                .setTargetBufferBytes(bufMaxBytes > 0 ? bufMaxBytes : (int) MediaPlayer.DEFAULT_MAX_BUFFER_BYTES)
                .build();
        final boolean passthrough = passthroughEnabled;
        /* Tee 旁路 PCM → 频谱；关直通时强制 DEFAULT 能力（PCM），开则跟 Context 探测 HDMI */
        DefaultRenderersFactory rf = new DefaultRenderersFactory(app) {
            @Override
            protected AudioSink buildAudioSink(Context context, boolean enableFloatOutput,
                                               boolean enableAudioTrackPlaybackParams) {
                DefaultAudioSink.Builder b = new DefaultAudioSink.Builder(context)
                        .setEnableFloatOutput(enableFloatOutput)
                        .setEnableAudioTrackPlaybackParams(enableAudioTrackPlaybackParams)
                        .setAudioProcessors(new AudioProcessor[] { new TeeAudioProcessor(barsSink) });
                if (!passthrough) {
                    b.setAudioCapabilities(AudioCapabilities.DEFAULT_AUDIO_CAPABILITIES);
                }
                return b.build();
            }
        };
        rf.setExtensionRendererMode(DefaultRenderersFactory.EXTENSION_RENDERER_MODE_ON)
                .setEnableDecoderFallback(true);
        trackSelector = new DefaultTrackSelector(app);
        /* 字幕默认关（与系统外挂语义一致）；用户 selectTrack 再开。防 DVB 自动选中拖垮弱盒 */
        trackSelector.setParameters(trackSelector.buildUponParameters()
                .setPreferredAudioLanguage("zh")
                .setPreferredTextLanguage("zh")
                .setSelectUndeterminedTextLanguage(false)
                .setTrackTypeDisabled(C.TRACK_TYPE_TEXT, true)
                .setForceHighestSupportedBitrate(true)
                .build());
        player = new ExoPlayer.Builder(app)
                .setRenderersFactory(rf)
                .setTrackSelector(trackSelector)
                .setLoadControl(load)
                .build();
        if (holder != null) {
            player.setVideoSurfaceHolder(holder);
        } else if (surface != null) {
            player.setVideoSurface(surface);
        }
        player.setVolume((volL + volR) * 0.5f);
        player.setRepeatMode(looping ? Player.REPEAT_MODE_ONE : Player.REPEAT_MODE_OFF);
        player.setMediaSource(PlaySources.create(app, pendingUrl, externalSubs(),
                pendingHeaders, !tlsVerifyStrict));
        player.addListener(new Player.Listener() {
            @Override
            public void onPlaybackStateChanged(int state) {
                if (state == Player.STATE_READY) {
                    liveBehindTries = 0;
                    if (!prepared) {
                        prepared = true;
                        firePrepared();
                    }
                }
                if (state == Player.STATE_ENDED) {
                    fireCompletion();
                }
            }

            @Override
            public void onPlayerError(PlaybackException error) {
                int code = error != null ? error.errorCode : -1;
                if (code == PlaybackException.ERROR_CODE_BEHIND_LIVE_WINDOW
                        && recoverBehindLiveWindow()) {
                    return;
                }
                prepared = false;
                fireError(1, code);
            }

            @Override
            public void onVideoSizeChanged(VideoSize videoSize) {
                if (videoSize != null) {
                    fireVideoSize(videoSize.width, videoSize.height);
                }
            }

            @Override
            public void onRenderedFirstFrame() {
                fireInfo(3, 0); /* MEDIA_INFO_VIDEO_RENDERING_START */
            }

            @Override
            public void onIsLoadingChanged(boolean isLoading) {
                if (isLoading) {
                    fireInfo(701, 0); /* BUFFERING_START */
                } else if (prepared) {
                    fireInfo(702, 0); /* BUFFERING_END */
                }
            }

            @Override
            public void onTracksChanged(Tracks tracks) {
                rebuildFlat(tracks);
            }

            @Override
            public void onCues(CueGroup cueGroup) {
                fireTimedText(cueToTimed(cueGroup));
            }
        });
        player.prepare();
    }

    @Override
    public void start() {
        if (player != null) {
            player.play();
        }
    }

    @Override
    public void pause() {
        if (player != null) {
            player.pause();
        }
    }

    @Override
    public void stop() {
        prepared = false;
        if (player != null) {
            try {
                player.stop();
            } catch (Throwable ignored) {
            }
        }
    }

    @Override
    public void reset() {
        prepared = false;
        pendingUrl = null;
        pendingHeaders = null;
        extSubPath = null;
        extSubMime = null;
        releasePlayerOnly();
    }

    @Override
    public void release() {
        released = true;
        prepared = false;
        host = null;
        releasePlayerOnly();
    }

    private void releasePlayerOnly() {
        if (player != null) {
            try {
                player.release();
            } catch (Throwable ignored) {
            }
            player = null;
        }
        trackSelector = null;
        flat.clear();
    }

    /** 掉出 HLS/DASH 直播窗：seek 到默认 live 边再 prepare；最多 LIVE_BEHIND_MAX 次。 */
    private boolean recoverBehindLiveWindow() {
        ExoPlayer p = player;
        if (p == null || released || liveBehindTries >= LIVE_BEHIND_MAX) {
            return false;
        }
        liveBehindTries++;
        try {
            p.seekToDefaultPosition();
            p.prepare();
            p.play();
            return true;
        } catch (Throwable t) {
            return false;
        }
    }

    @Override
    public void seekTo(int msec) {
        if (player != null) {
            player.seekTo(msec);
            fireSeekComplete();
        }
    }

    @Override
    public int getCurrentPosition() {
        if (player == null || !prepared) {
            return 0;
        }
        try {
            return (int) player.getCurrentPosition();
        } catch (Throwable t) {
            return 0;
        }
    }

    @Override
    public int getDuration() {
        if (player == null || !prepared) {
            return 0;
        }
        try {
            long d = player.getDuration();
            return d < 0 || d == androidx.media3.common.C.TIME_UNSET ? 0 : (int) d;
        } catch (Throwable t) {
            return 0;
        }
    }

    @Override
    public boolean isPlaying() {
        try {
            return player != null && player.isPlaying();
        } catch (Throwable t) {
            return false;
        }
    }

    @Override
    public int getVideoWidth() {
        if (player == null) {
            return 0;
        }
        try {
            VideoSize v = player.getVideoSize();
            return v != null ? v.width : 0;
        } catch (Throwable t) {
            return 0;
        }
    }

    @Override
    public int getVideoHeight() {
        if (player == null) {
            return 0;
        }
        try {
            VideoSize v = player.getVideoSize();
            return v != null ? v.height : 0;
        } catch (Throwable t) {
            return 0;
        }
    }

    @Override
    public String videoMime() {
        Format f = videoFormat();
        return f != null ? f.sampleMimeType : null;
    }

    @Override
    public String audioMime() {
        Format f = audioFormat();
        return f != null ? f.sampleMimeType : null;
    }

    @Override
    public float videoFrameRate() {
        Format f = videoFormat();
        if (f == null || f.frameRate <= 0f) {
            return 0f;
        }
        return f.frameRate;
    }

    @Override
    public int videoBitrate() {
        Format f = videoFormat();
        return f != null && f.bitrate > 0 ? f.bitrate : -1;
    }

    @Override
    public int audioBitrate() {
        Format f = audioFormat();
        return f != null && f.bitrate > 0 ? f.bitrate : -1;
    }

    private Format videoFormat() {
        if (player == null) {
            return null;
        }
        try {
            Format f = player.getVideoFormat();
            return f != null ? f : selectedFormat(C.TRACK_TYPE_VIDEO);
        } catch (Throwable t) {
            return null;
        }
    }

    private Format audioFormat() {
        if (player == null) {
            return null;
        }
        try {
            /* FFmpeg 扩展开音轨时 getAudioFormat 常仍为空；回落已选 Tracks。 */
            Format f = player.getAudioFormat();
            return f != null ? f : selectedFormat(C.TRACK_TYPE_AUDIO);
        } catch (Throwable t) {
            return null;
        }
    }

    private Format selectedFormat(int trackType) {
        try {
            Tracks tracks = player.getCurrentTracks();
            for (int i = 0; i < tracks.getGroups().size(); i++) {
                Tracks.Group g = tracks.getGroups().get(i);
                if (g.getType() != trackType) {
                    continue;
                }
                for (int j = 0; j < g.length; j++) {
                    if (g.isTrackSelected(j)) {
                        return g.getTrackFormat(j);
                    }
                }
                if (g.length > 0) {
                    return g.getTrackFormat(0);
                }
            }
        } catch (Throwable ignored) {
        }
        return null;
    }

    @Override
    public void setVolume(float l, float r) {
        volL = l;
        volR = r;
        if (player != null) {
            try {
                player.setVolume((l + r) * 0.5f);
            } catch (Throwable ignored) {
            }
        }
    }

    @Override
    public void setLooping(boolean loop) {
        looping = loop;
        if (player != null) {
            player.setRepeatMode(loop ? Player.REPEAT_MODE_ONE : Player.REPEAT_MODE_OFF);
        }
    }

    @Override
    public boolean isLooping() {
        return looping;
    }

    @Override
    public boolean isLive() {
        if (player == null || !prepared) {
            return true;
        }
        try {
            return player.isCurrentMediaItemLive();
        } catch (Throwable t) {
            return true;
        }
    }

    @Override
    public MediaPlayer.TrackInfo[] getTrackInfo() {
        if (flat.isEmpty() && player != null && !released) {
            try {
                rebuildFlat(player.getCurrentTracks());
            } catch (Throwable ignored) {
            }
        }
        MediaPlayer.TrackInfo[] out = new MediaPlayer.TrackInfo[flat.size()];
        for (int i = 0; i < flat.size(); i++) {
            out[i] = toTrackInfo(i, flat.get(i), isFlatSelected(i));
        }
        return out;
    }

    @Override
    public void selectTrack(int flatIndex) {
        if (player == null || released || trackSelector == null
                || flatIndex < 0 || flatIndex >= flat.size()) {
            return;
        }
        Flat f = flat.get(flatIndex);
        try {
            DefaultTrackSelector.Parameters.Builder b = trackSelector.getParameters().buildUpon();
            b.clearOverridesOfType(f.group.type);
            b.setTrackTypeDisabled(f.group.type, false);
            b.setOverrideForType(new TrackSelectionOverride(f.group, f.inGroup));
            trackSelector.setParameters(b.build());
        } catch (Throwable ignored) {
        }
    }

    @Override
    public void deselectTrack(int flatIndex) {
        if (player == null || released || trackSelector == null) {
            return;
        }
        try {
            int type = C.TRACK_TYPE_TEXT;
            if (flatIndex >= 0 && flatIndex < flat.size()) {
                type = flat.get(flatIndex).group.type;
            }
            if (type != C.TRACK_TYPE_TEXT) {
                return; /* 音轨不可 deselect，与系统一致 */
            }
            DefaultTrackSelector.Parameters.Builder b = trackSelector.getParameters().buildUpon();
            b.clearOverridesOfType(type);
            b.setTrackTypeDisabled(C.TRACK_TYPE_TEXT, true);
            trackSelector.setParameters(b.build());
        } catch (Throwable ignored) {
        }
    }

    @Override
    public int getSelectedTrack(int trackType) {
        if (player == null || released) {
            return -1;
        }
        for (int i = 0; i < flat.size(); i++) {
            if (flat.get(i).type == trackType && isFlatSelected(i)) {
                return i;
            }
        }
        return -1;
    }

    @Override
    public void setExternalTimedText(String pathOrUri, String mime) {
        extSubPath = pathOrUri;
        extSubMime = mime;
        /* 仅已 READY 后中途加挂才重建；prepare 前只记字段，由 bindAndPrepare 吃进 create */
        if (!prepared || player == null || released || pendingUrl == null) {
            return;
        }
        try {
            long pos = player.getCurrentPosition();
            boolean play = player.isPlaying();
            player.setMediaSource(PlaySources.create(app, pendingUrl, externalSubs(),
                    pendingHeaders, !tlsVerifyStrict));
            player.prepare();
            if (pos > 0) {
                player.seekTo(pos);
            }
            if (play) {
                player.play();
            }
        } catch (Throwable ignored) {
        }
    }

    @Override
    public boolean readAudioBars(int[] out) {
        return barsSink.read(out);
    }

    @Override
    public void setBufferConfig(int minMs, int maxMs, int playMs, int rebufMs, int maxBytes) {
        bufMinMs = minMs;
        bufMaxMs = maxMs;
        bufPlayMs = playMs;
        bufRebufMs = rebufMs;
        bufMaxBytes = maxBytes > 0 ? maxBytes : (int) MediaPlayer.DEFAULT_MAX_BUFFER_BYTES;
    }

    @Override
    public void setPlaybackTlsVerify(boolean strict) {
        tlsVerifyStrict = strict;
    }

    @Override
    public void setAudioPassthroughEnabled(boolean enabled) {
        passthroughEnabled = enabled;
    }

    private List<MediaItem.SubtitleConfiguration> externalSubs() {
        if (extSubPath == null || extSubPath.length() == 0) {
            return Collections.emptyList();
        }
        String mime = extSubMime != null ? extSubMime : MediaPlayer.MEDIA_MIMETYPE_TEXT_SUBRIP;
        try {
            return Collections.singletonList(PlaySources.externalSub(extSubPath, mime));
        } catch (Throwable t) {
            return Collections.emptyList();
        }
    }

    private void rebuildFlat(Tracks tracks) {
        flat.clear();
        if (tracks == null) {
            return;
        }
        for (int g = 0; g < tracks.getGroups().size(); g++) {
            Tracks.Group tg = tracks.getGroups().get(g);
            if (tg.getType() != C.TRACK_TYPE_AUDIO && tg.getType() != C.TRACK_TYPE_TEXT) {
                continue;
            }
            TrackGroup mg = tg.getMediaTrackGroup();
            for (int i = 0; i < tg.length; i++) {
                if (!tg.isTrackSupported(i)) {
                    continue;
                }
                Format fmt = tg.getTrackFormat(i);
                int type = tg.getType() == C.TRACK_TYPE_AUDIO
                        ? MediaPlayer.TrackInfo.MEDIA_TRACK_TYPE_AUDIO
                        : textTrackType(fmt);
                flat.add(new Flat(mg, i, type));
            }
        }
    }

    /** 位图（DVB/PGS…）→ SUBTITLE；文字 → TIMEDTEXT。转码后原 mime 在 codecs。 */
    private static int textTrackType(Format fmt) {
        return isBitmapSub(fmt)
                ? MediaPlayer.TrackInfo.MEDIA_TRACK_TYPE_SUBTITLE
                : MediaPlayer.TrackInfo.MEDIA_TRACK_TYPE_TIMEDTEXT;
    }

    private static boolean isBitmapSub(Format fmt) {
        if (fmt == null) {
            return false;
        }
        String s = fmt.codecs != null ? fmt.codecs : fmt.sampleMimeType;
        if (s == null) {
            return false;
        }
        String m = s.toLowerCase();
        return m.contains("dvb") || m.contains("pgs") || m.contains("vobsub")
                || m.startsWith("image/") || MimeTypes.APPLICATION_DVBSUBS.equals(s)
                || MimeTypes.APPLICATION_PGS.equals(s);
    }

    private boolean isFlatSelected(int flatIndex) {
        if (player == null || flatIndex < 0 || flatIndex >= flat.size()) {
            return false;
        }
        Flat f = flat.get(flatIndex);
        try {
            Tracks tracks = player.getCurrentTracks();
            for (int g = 0; g < tracks.getGroups().size(); g++) {
                Tracks.Group tg = tracks.getGroups().get(g);
                if (!tg.getMediaTrackGroup().equals(f.group)) {
                    continue;
                }
                return f.inGroup < tg.length && tg.isTrackSelected(f.inGroup);
            }
        } catch (Throwable ignored) {
        }
        return false;
    }

    private MediaPlayer.TrackInfo toTrackInfo(int index, Flat f, boolean selected) {
        Format fmt = null;
        try {
            if (player != null) {
                Tracks tracks = player.getCurrentTracks();
                for (int g = 0; g < tracks.getGroups().size(); g++) {
                    Tracks.Group tg = tracks.getGroups().get(g);
                    if (tg.getMediaTrackGroup().equals(f.group) && f.inGroup < tg.length) {
                        fmt = tg.getTrackFormat(f.inGroup);
                        break;
                    }
                }
            }
        } catch (Throwable ignored) {
        }
        String lang = fmt != null && fmt.language != null && fmt.language.length() > 0
                ? fmt.language : "und";
        String mime = fmt != null ? fmt.sampleMimeType : null;
        String name = fmt != null ? fmt.label : null;
        boolean isDefault = fmt != null
                && (fmt.selectionFlags & C.SELECTION_FLAG_DEFAULT) != 0;
        boolean isForced = fmt != null
                && (fmt.selectionFlags & C.SELECTION_FLAG_FORCED) != 0;
        return new MediaPlayer.TrackInfo(index, f.type, selected, isDefault, isForced, lang, mime, name);
    }

    private static TimedText cueToTimed(CueGroup cueGroup) {
        if (cueGroup == null || cueGroup.cues == null || cueGroup.cues.isEmpty()) {
            return null;
        }
        Cue c = cueGroup.cues.get(0);
        if (c == null) {
            return null;
        }
        try {
            if (c.bitmap != null && !c.bitmap.isRecycled()) {
                Bitmap src = c.bitmap;
                Bitmap copy = src.getConfig() != null ? src.copy(src.getConfig(), false) : null;
                if (copy == null) {
                    copy = src;
                }
                Rect bounds = bitmapBounds(c);
                if (bounds == null) {
                    return null;
                }
                return TimedText.fromBitmap(copy, bounds, SUB_CANVAS_W, SUB_CANVAS_H);
            }
        } catch (Throwable ignored) {
            /* 弱盒位图失败不拖垮播控 */
            return null;
        }
        if (c.text != null && c.text.length() > 0) {
            return new TimedText(c.text.toString(), null, -1, -1, 0);
        }
        return null;
    }

    /** 抄超级直播 ExoUdpxyPlayer.bitmapBounds：1280×720 画布。 */
    private static Rect bitmapBounds(Cue c) {
        Bitmap bmp = c.bitmap;
        if (bmp == null) {
            return null;
        }
        int bw = bmp.getWidth();
        int bh = bmp.getHeight();
        if (bw <= 0 || bh <= 0) {
            return null;
        }
        float unset = Cue.DIMEN_UNSET;
        int boxW;
        int boxH;
        if (c.bitmapHeight != unset && c.bitmapHeight > 0f) {
            boxH = Math.round(SUB_CANVAS_H * c.bitmapHeight);
            boxW = Math.max(1, Math.round(boxH * (bw / (float) bh)));
        } else if (c.size != unset && c.size > 0f) {
            boxW = Math.round(SUB_CANVAS_W * c.size);
            boxH = Math.max(1, Math.round(boxW * (bh / (float) bw)));
        } else {
            boxW = Math.min(SUB_CANVAS_W, Math.round(SUB_CANVAS_W * 0.9f));
            boxH = Math.max(1, Math.round(boxW * (bh / (float) bw)));
            if (boxH > SUB_CANVAS_H / 3) {
                boxH = SUB_CANVAS_H / 3;
                boxW = Math.max(1, Math.round(boxH * (bw / (float) bh)));
            }
        }
        float line = c.line;
        float pos = c.position;
        int lineAnchor = c.lineAnchor;
        int posAnchor = c.positionAnchor;
        if (line == unset) {
            line = 1f;
            if (lineAnchor == Cue.TYPE_UNSET) {
                lineAnchor = Cue.ANCHOR_TYPE_END;
            }
        }
        if (pos == unset) {
            pos = 0.5f;
            if (posAnchor == Cue.TYPE_UNSET) {
                posAnchor = Cue.ANCHOR_TYPE_MIDDLE;
            }
        }
        int top;
        if (c.lineType == Cue.LINE_TYPE_NUMBER) {
            top = Math.round(line * boxH);
        } else {
            float y = line * SUB_CANVAS_H;
            if (lineAnchor == Cue.ANCHOR_TYPE_START) {
                top = Math.round(y);
            } else if (lineAnchor == Cue.ANCHOR_TYPE_MIDDLE) {
                top = Math.round(y - boxH / 2f);
            } else {
                top = Math.round(y - boxH);
            }
        }
        float x = pos * SUB_CANVAS_W;
        int left;
        if (posAnchor == Cue.ANCHOR_TYPE_START) {
            left = Math.round(x);
        } else if (posAnchor == Cue.ANCHOR_TYPE_END) {
            left = Math.round(x - boxW);
        } else {
            left = Math.round(x - boxW / 2f);
        }
        if (left < 0) {
            left = 0;
        }
        if (top < 0) {
            top = 0;
        }
        if (left + boxW > SUB_CANVAS_W) {
            left = Math.max(0, SUB_CANVAS_W - boxW);
        }
        if (top + boxH > SUB_CANVAS_H) {
            top = Math.max(0, SUB_CANVAS_H - boxH);
        }
        return new Rect(left, top, left + boxW, top + boxH);
    }

    private void fireTimedText(final TimedText text) {
        final Host h = host;
        if (h == null) {
            return;
        }
        post(new Runnable() {
            public void run() {
                h.onTimedText(text);
            }
        });
    }

    private void firePrepared() {
        final Host h = host;
        if (h == null) {
            return;
        }
        post(new Runnable() {
            public void run() {
                h.onPrepared();
            }
        });
    }

    private void fireCompletion() {
        final Host h = host;
        if (h == null) {
            return;
        }
        post(new Runnable() {
            public void run() {
                h.onCompletion();
            }
        });
    }

    private void fireError(final int what, final int extra) {
        final Host h = host;
        if (h == null) {
            return;
        }
        post(new Runnable() {
            public void run() {
                h.onError(what, extra);
            }
        });
    }

    private void fireInfo(final int what, final int extra) {
        final Host h = host;
        if (h == null) {
            return;
        }
        post(new Runnable() {
            public void run() {
                h.onInfo(what, extra);
            }
        });
    }

    private void fireSeekComplete() {
        final Host h = host;
        if (h == null) {
            return;
        }
        post(new Runnable() {
            public void run() {
                h.onSeekComplete();
            }
        });
    }

    private void fireVideoSize(final int w, final int hgt) {
        final Host h = host;
        if (h == null) {
            return;
        }
        post(new Runnable() {
            public void run() {
                h.onVideoSizeChanged(w, hgt);
            }
        });
    }

    private void post(Runnable r) {
        if (Looper.myLooper() == main.getLooper()) {
            r.run();
        } else {
            main.post(r);
        }
    }
}
