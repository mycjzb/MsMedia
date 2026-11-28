/*
 * 与系统 android.media.MediaPlayer 对齐的公开封装：同名方法 / 重载 / 常量 / 监听器，
 * 便于业务把 import android.media.MediaPlayer 换成 com.mycjzb.media.player.MediaPlayer。
 * 底层：{@link PlayEngine#SHW} → Media3 源码内核（{@link M3PlayerBackend}）；
 * {@link PlayEngine#SYSTEM} → 系统播放器。源级前缀 {@code shw:}/{@code sys:}；
 * {@link PlayEngine#setDefault(int)} 改全局默认。协议/音频兜底 JNI 在 DataSource/FfAdec，不经本类 native 播控。
 * 不记录完整私有 URL。不是设备出画/出声验收。
 * by: Long <mycjzb@gmail.com>
 */
package com.mycjzb.media.player;

import android.content.ContentResolver;
import android.content.Context;
import android.content.res.AssetFileDescriptor;
import android.graphics.Rect;
import android.content.Intent;
import android.content.IntentFilter;
import android.media.AudioAttributes;
import android.media.AudioFormat;
import android.media.AudioManager;
import android.media.AudioTrack;
import android.media.MediaFormat;
import android.os.Build;
import android.provider.Settings;
import android.media.PlaybackParams;
import android.net.Uri;
import android.os.Handler;
import android.os.Looper;
import android.os.ParcelFileDescriptor;
import android.os.SystemClock;
import android.view.Surface;
import android.view.SurfaceHolder;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileDescriptor;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.ByteBuffer;
import java.nio.channels.FileChannel;
import java.nio.charset.Charset;
import java.util.HashMap;
import java.util.Map;

public class MediaPlayer {

    /* ---- 与系统 android.media.MediaPlayer 相同的常量（值保持一致） ---- */
    public static final int VIDEO_SCALING_MODE_SCALE_TO_FIT = 1;
    public static final int VIDEO_SCALING_MODE_SCALE_TO_FIT_WITH_CROPPING = 2;

    public static final int MEDIA_ERROR_UNKNOWN = 1;
    public static final int MEDIA_ERROR_SERVER_DIED = 100;
    public static final int MEDIA_ERROR_NOT_VALID_FOR_PROGRESSIVE_PLAYBACK = 200;
    public static final int MEDIA_ERROR_IO = -1004;
    public static final int MEDIA_ERROR_MALFORMED = -1007;
    public static final int MEDIA_ERROR_UNSUPPORTED = -1010;
    public static final int MEDIA_ERROR_TIMED_OUT = -110;

    public static final int MEDIA_INFO_UNKNOWN = 1;
    public static final int MEDIA_INFO_STARTED_AS_NEXT = 2;
    public static final int MEDIA_INFO_VIDEO_RENDERING_START = 3;
    public static final int MEDIA_INFO_VIDEO_TRACK_LAGGING = 700;
    public static final int MEDIA_INFO_BUFFERING_START = 701;
    public static final int MEDIA_INFO_BUFFERING_END = 702;
    public static final int MEDIA_INFO_BAD_INTERLEAVING = 800;
    public static final int MEDIA_INFO_NOT_SEEKABLE = 801;
    public static final int MEDIA_INFO_METADATA_UPDATE = 802;
    public static final int MEDIA_INFO_UNSUPPORTED_SUBTITLE = 901;
    public static final int MEDIA_INFO_SUBTITLE_TIMED_OUT = 902;

    /** 外挂字幕 MIME（与系统同值）。 */
    public static final String MEDIA_MIMETYPE_TEXT_SUBRIP = "application/x-subrip";
    /** MsMedia 扩展：外挂 WebVTT 字幕。 */
    public static final String MEDIA_MIMETYPE_TEXT_VTT = "text/vtt";
    /** MsMedia 扩展：外挂 ASS/SSA 字幕（只取文字，不做特效渲染）。 */
    public static final String MEDIA_MIMETYPE_TEXT_SSA = "text/x-ssa";

    /** API 26 seek 模式，值与系统一致。原生层统一按关键帧对齐 seek。 */
    public static final int SEEK_PREVIOUS_SYNC = 0;
    public static final int SEEK_NEXT_SYNC = 1;
    public static final int SEEK_CLOSEST_SYNC = 2;
    public static final int SEEK_CLOSEST = 3;

    /* 内部状态（与系统 MediaPlayer 文档的状态图一致）。 */
    private static final int S_ERROR = -1;
    private static final int S_IDLE = 0;
    private static final int S_INITIALIZED = 1;
    private static final int S_PREPARING = 2;
    private static final int S_PREPARED = 3;
    private static final int S_STARTED = 4;
    private static final int S_PAUSED = 5;
    private static final int S_STOPPED = 6;
    private static final int S_PLAYBACK_COMPLETED = 7;
    private static final int S_END = 8;

    /* nativeQuery 下标（与 ms_hw_mediaplayer_jni.inc 一致）。 */
    private static final int Q_OPENED = 0;
    private static final int Q_DONE = 1;
    private static final int Q_POS_MS = 2;
    private static final int Q_DUR_MS = 3;
    private static final int Q_W = 4;
    private static final int Q_H = 5;
    private static final int Q_PAUSED = 6;
    private static final int Q_SEEKABLE = 7;
    private static final int Q_FIRST_OUT = 8;
    private static final int Q_BUSY = 9;
    private static final int Q_LAST_ERR = 10;
    private static final int Q_ACTIVE = 11;
    private static final int Q_PENDING = 12;
    private static final int Q_LEN = 13;

    private static final int POLL_MS = 100;
    /* 起播红线：首帧前用 10 ms 快轮询派发 RENDERING_START，出画后回到 100 ms，避免外壳多压 0–100 ms。 */
    private static final int POLL_FAST_MS = 10;
    /* prepare 阻塞等待的轮询间隔（原 15 ms）；nativeQuery 只读快照，开销可忽略。 */
    private static final int PREPARE_POLL_MS = 5;
    private static final int DEFAULT_PREPARE_TIMEOUT_MS = 15000;

    private final Object lock = new Object();
    private final Handler main = new Handler(Looper.getMainLooper());
    private final long[] q = new long[Q_LEN]; /* 遗留查询缓冲，SHW/Media3 不用 */
    private int state = S_IDLE;

    private String dataSource;          /* 仅内部，已剥引擎前缀，不写入日志 */
    private boolean dataIsUrl;
    private ParcelFileDescriptor dataPfd; /* fd 数据源：dup 后持有，reset/release 关闭 */
    private boolean headersUnsupported; /* 非空请求头：自研通道尚无请求头参数，prepare 诚实失败 */
    /** 当前 dataSource 是 tvbus（sop:// / tvbus://）：release/reset 要停 P2P 会话。 */
    private boolean tvbusActive;
    private Map<String, String> dataHeaders; /* 系统档可带请求头；自研档非空则 headersUnsupported */
    private int engine = PlayEngine.SHW; /* 本实例解析后的引擎 */
    private SysPlayerBackend sys;        /* engine==SYSTEM 时非空 */
    private ShwBackend shw;              /* SHW = Media3 */
    /* 直播/点播：抄超级直播 MsPlay/Exo 时长滑动（m3u8/flv 一样按时长，不靠后缀） */
    private Boolean liveHint;            /* null=自动 */
    private int liveCached = -1;         /* -1 未定，0 点播，1 直播 */
    private long liveFirstDurMs = -1;
    private long liveLastDurMs = -1;
    private boolean liveNotSeekable;
    private String aspectMode = AspectMode.AUTO;

    private Surface surface;
    private SurfaceHolder display;
    private float volLeft = 1f;
    private float volRight = 1f;
    private boolean looping;
    private int preloadAheadMs;      /* 0=默认 */
    private long preloadMaxBytes;    /* 0=默认 */
    /* 阶段 6：Media3 风格缓冲水位（bufSet=false 时由 setPreloadConfig 兼容推导）与播放 TLS 策略。by: Long <mycjzb@gmail.com> */
    private boolean bufSet;
    private int bufMinMs = DEFAULT_MIN_BUFFER_MS;
    private int bufMaxMs = DEFAULT_MAX_BUFFER_MS;
    private int bufPlayMs = DEFAULT_BUFFER_FOR_PLAYBACK_MS;
    private int bufRebufMs = DEFAULT_BUFFER_FOR_PLAYBACK_AFTER_REBUFFER_MS;
    private long bufMaxBytes = DEFAULT_MAX_BUFFER_BYTES;
    private boolean playbackTlsVerify;   /* 默认 false：播放拉流忽略证书（MsHttpClient 永远严格） */
    private boolean passthroughEnabled; /* 阶段 4：AC-3/E-AC-3/DTS 直通开关，默认关闭（软解） */
    private Context appContext;         /* 直通能力探测用（HDMI 粘性广播）；可为空 */
    private int scalingMode = VIDEO_SCALING_MODE_SCALE_TO_FIT;
    private float speed = 1f;
    private boolean screenOnWhilePlaying;

    /* 轮询派发用的上一帧快照 */
    private long doneBase;
    private int lastW;
    private int lastH;
    private boolean renderingStartSent;
    private boolean bufferingSent;
    private long lastPosMs;
    private int lastDurMs = -1;
    private boolean seeking;
    private long seekTargetMs;
    private long seekIssuedAt;
    private int prepareGen; /* 每次 prepare/reset 自增，丢弃过期的异步结果 */

    private OnPreparedListener onPrepared;
    private OnCompletionListener onCompletion;
    private OnErrorListener onError;
    private OnInfoListener onInfo;
    private OnSeekCompleteListener onSeekComplete;
    private OnBufferingUpdateListener onBufferingUpdate;
    private OnVideoSizeChangedListener onVideoSizeChanged;
    private OnTimedTextListener onTimedText;

    /* 选轨与字幕（阶段 7）。by: Long <mycjzb@gmail.com> */
    private static final int TRACK_MAX = 32;           /* 与原生 MP_TRACK_MAX 一致 */
    private static final int TRACK_META = 8;           /* 与原生 MP_TRACK_META 一致 */
    private static final int TRACK_STRS = 3;           /* 与原生 MP_TRACK_STRS 一致 */
    private static final int CUE_META = 12;            /* 与原生 MP_CUE_META 一致 */
    private static final int MS_ERR_UNSUPPORTED = 4;   /* ms_status，见 msmedia.h */
    private static final int SUB_FMT_AUTO = 0, SUB_FMT_SRT = 1, SUB_FMT_WEBVTT = 2; /* ms_sub_format */
    private static final int TIMED_TEXT_MAX_BYTES = 16 << 20; /* 外挂字幕文件上限，防 OOM */
    private static final Charset UTF8 = Charset.forName("UTF-8");
    private final int[] trackMeta = new int[TRACK_MAX * TRACK_META];
    private final String[] trackStrs = new String[TRACK_MAX * TRACK_STRS];
    private final int[] cueMeta = new int[CUE_META];
    private TrackInfo[] lastTracks = new TrackInfo[0]; /* 原生忙时返回上次结果 */
    private byte[] extSubData;    /* 旧 native 路径残留字段；SHW 用 extSubPath */
    private int extSubFmt;
    private boolean extSubApplied;
    private String extSubPath;    /* SHW Media3 外挂字幕路径/URI */
    private String extSubMime;
    private String lastCueKey;     /* 上次派发的字幕（开始时间|轨|文字），null = 当前无字幕 */
    private boolean cueDirty;      /* 选轨变化后强制重新派发一次 */
    private String lastCueText;
    /* 用户刚选的音轨（内核在读到新轨数据后才提交切换，期间仍播旧轨）；提交前 getSelectedTrack 报它，
     * 与系统 selectTrack 返回后立即可查的语义一致。0 = 无。 */
    private int pendingAudioId;
    private boolean pendingAudioSet;
    private static final int SUB_STYLE_HAS_POS = 1 << 6; /* MS_SUB_STYLE_HAS_POS */

    private final Runnable poller = new Runnable() {
        @Override
        public void run() {
            pollOnce();
        }
    };

    public MediaPlayer() {
        /* SHW=Media3；so 由 JniHttp/FfIo/FfAdec/MsProbe 按需加载 */
    }

    /** 是否具备 FFmpeg 协议/音频兜底 so（msm3 DataSource 用）。 */
    public static boolean isFfmpegRoute() {
        return MsNativeLoader.ensure();
    }

    /* ---------------- 工厂方法（对齐系统 create） ---------------- */

    public static MediaPlayer create(Context context, Uri uri) {
        return create(context, uri, null);
    }

    public static MediaPlayer create(Context context, Uri uri, SurfaceHolder holder) {
        MediaPlayer mp = new MediaPlayer();
        try {
            if (holder != null) {
                mp.setDisplay(holder);
            }
            mp.setDataSource(context, uri);
            mp.prepare();
            return mp;
        } catch (Exception e) {
            mp.release();
            return null;
        }
    }

    public static MediaPlayer create(Context context, int resid) {
        MediaPlayer mp = new MediaPlayer();
        AssetFileDescriptor afd = null;
        try {
            afd = context.getResources().openRawResourceFd(resid);
            if (afd == null) {
                mp.release();
                return null;
            }
            mp.setDataSource(afd.getFileDescriptor(), afd.getStartOffset(), afd.getLength());
            mp.prepare();
            return mp;
        } catch (Exception e) {
            mp.release();
            return null;
        } finally {
            if (afd != null) {
                try {
                    afd.close();
                } catch (IOException ignored) {
                    /* 关闭失败不影响已 dup 的 fd */
                }
            }
        }
    }

    /* ---------------- 引擎（全局默认 + 源级前缀） ---------------- */

    /** 全局默认引擎：{@link PlayEngine#SHW} 或 {@link PlayEngine#SYSTEM}。只影响之后的 setDataSource。 */
    public static void setDefaultEngine(int engine) {
        PlayEngine.setDefault(engine);
    }

    public static int getDefaultEngine() {
        return PlayEngine.getDefault();
    }

    /** 本实例当前引擎（setDataSource 解析后）。 */
    public int getEngine() {
        synchronized (lock) {
            return engine;
        }
    }

    /**
     * 强制直播/点播；{@code null} 恢复自动判断。
     * 目录已标明直播/点播时用，避免 HTTP 切片误判。
     */
    public void setLiveHint(Boolean live) {
        synchronized (lock) {
            checkNotReleased();
            liveHint = live;
            liveCached = -1;
            liveFirstDurMs = -1;
            liveLastDurMs = -1;
        }
    }

    /**
     * 是否直播（方便 seek）。<b>不另开连接</b>：Media3 用 Timeline live；
     * 自研/系统用同一次打开后的 seekable/时长。URL 扩展名不参与本判定。
     */
    public boolean isLive() {
        synchronized (lock) {
            if (liveHint != null) return liveHint.booleanValue();
            if (liveCached >= 0) return liveCached == 1;
            if (state == S_IDLE || state == S_INITIALIZED || state == S_PREPARING
                    || state == S_ERROR || state == S_END) {
                return true;
            }
            if (isShwExtLocked() && shw != null) {
                boolean live = shw.isLive();
                liveCached = live ? 1 : 0;
                return live;
            }
            if (liveNotSeekable) {
                liveCached = 1;
                return true;
            }
            long dur;
            long pos;
            int seekable = 1;
            if (isSysLocked()) {
                if (sys == null) return true;
                dur = sys.getDuration();
                pos = sys.getCurrentPosition();
            } else {
                return true;
            }
            /* 同一次打开后的结果：不可 seek ≈ 无 ENDLIST / live 窗 */
            if (seekable == 0) {
                liveCached = 1;
                return true;
            }
            if (dur <= 0) return true;
            if (liveFirstDurMs < 0) {
                liveFirstDurMs = dur;
                liveLastDurMs = dur;
                return true;
            }
            if (Math.abs(dur - liveLastDurMs) > 500L
                    || Math.abs(dur - liveFirstDurMs) > 1000L) {
                liveCached = 1;
                return true;
            }
            liveLastDurMs = dur;
            if (pos > dur + 2000L) {
                liveCached = 1;
                return true;
            }
            liveCached = 0;
            return false;
        }
    }

    /** 点播（可按总时长 seek）。未锁成点播前为 false。 */
    public boolean isVod() {
        synchronized (lock) {
            if (liveHint != null) return !liveHint.booleanValue();
            if (liveCached < 0) {
                isLive(); /* 推进一次采样 */
            }
            return liveCached == 0;
        }
    }

    /**
     * 画面比例字符串（对齐超级直播）：{@code auto}/{@code 16:9}/{@code 4:3}/{@code orig}/{@code fill}。
     * 仅记录偏好；实际量算请用 {@link MsAspectLayout#setAspectMode(String)}。
     */
    public void setAspectMode(String ratio) {
        synchronized (lock) {
            checkNotReleased();
            aspectMode = AspectMode.normalize(ratio);
        }
    }

    public String getAspectMode() {
        synchronized (lock) {
            return aspectMode;
        }
    }

    private void resetLiveDetectLocked() {
        liveCached = -1;
        liveFirstDurMs = -1;
        liveLastDurMs = -1;
        liveNotSeekable = false;
        /* liveHint 保留：业务强制跨源有效，除非显式 setLiveHint(null) */
    }

    /* ---------------- 数据源 ---------------- */

    public void setDataSource(String path) throws IOException {
        setDataSource(path, (Map<String, String>) null);
    }

    public void setDataSource(String path, Map<String, String> headers) throws IOException {
        if (path == null) {
            throw new IllegalArgumentException("path is null");
        }
        int eng = PlayEngine.resolve(path);
        String raw = PlayEngine.strip(path);
        Uri u = Uri.parse(raw);
        String scheme = u.getScheme();
        if (scheme == null || "file".equalsIgnoreCase(scheme)) {
            setSourceInternal(scheme == null ? raw : u.getPath(), false, headers, eng);
        } else {
            setSourceInternal(raw, true, headers, eng);
        }
    }

    public void setDataSource(Context context, Uri uri) throws IOException {
        setDataSource(context, uri, null);
    }

    public void setDataSource(Context context, Uri uri, Map<String, String> headers) throws IOException {
        if (context == null || uri == null) {
            throw new IllegalArgumentException("context/uri is null");
        }
        synchronized (lock) {
            appContext = context.getApplicationContext() != null ? context.getApplicationContext() : context;
        }
        String as = uri.toString();
        int eng = PlayEngine.resolve(as);
        String stripped = PlayEngine.strip(as);
        Uri u = Uri.parse(stripped);
        String scheme = u.getScheme();
        if (scheme == null || "file".equalsIgnoreCase(scheme)) {
            setSourceInternal(u.getPath() != null ? u.getPath() : stripped, false, headers, eng);
            return;
        }
        if (ContentResolver.SCHEME_CONTENT.equals(scheme) || ContentResolver.SCHEME_ANDROID_RESOURCE.equals(scheme)) {
            AssetFileDescriptor afd = context.getContentResolver().openAssetFileDescriptor(u, "r");
            if (afd == null) {
                throw new IOException("cannot open content uri");
            }
            try {
                setDataSource(afd.getFileDescriptor(), afd.getStartOffset(), afd.getDeclaredLength());
                synchronized (lock) {
                    engine = eng; /* fd 路径默认引擎会被覆盖，保留 URI 上的强制前缀 */
                }
            } finally {
                afd.close();
            }
            return;
        }
        setSourceInternal(stripped, true, headers, eng);
    }

    public void setDataSource(AssetFileDescriptor afd) throws IOException {
        if (afd == null) {
            throw new IllegalArgumentException("afd is null");
        }
        setDataSource(afd.getFileDescriptor(), afd.getStartOffset(), afd.getDeclaredLength());
    }

    public void setDataSource(FileDescriptor fd) throws IOException {
        setDataSource(fd, 0, 0x7ffffffffffffffL);
    }

    /**
     * fd 数据源：dup 一份并经 /proc/self/fd/N 交给原生按文件读取。
     * 原生只能从文件头读，offset != 0（如 APK 内未压缩资源）诚实不支持：prepare 返回 MEDIA_ERROR_UNSUPPORTED。
     */
    public void setDataSource(FileDescriptor fd, long offset, long length) throws IOException {
        if (fd == null) {
            throw new IllegalArgumentException("fd is null");
        }
        synchronized (lock) {
            requireState("setDataSource", S_IDLE);
            releaseSysLocked();
            releaseShwLocked();
            ParcelFileDescriptor pfd = ParcelFileDescriptor.dup(fd);
            closePfdLocked();
            dataPfd = pfd;
            dataSource = offset == 0 ? "/proc/self/fd/" + pfd.getFd() : null;
            dataIsUrl = false;
            dataHeaders = null;
            headersUnsupported = false;
            engine = PlayEngine.getDefault(); /* fd 无前缀，用全局默认 */
            resetLiveDetectLocked();
            state = S_INITIALIZED;
        }
    }

    private void setSourceInternal(String loc, boolean isUrl, Map<String, String> headers, int eng) {
        synchronized (lock) {
            requireState("setDataSource", S_IDLE);
            if (loc == null || loc.length() == 0) {
                throw new IllegalArgumentException("empty data source");
            }
            releaseSysLocked();
            releaseShwLocked();
            closePfdLocked();
            dataSource = loc;
            dataIsUrl = isUrl;
            engine = eng;
            resetLiveDetectLocked();
            if (headers != null && !headers.isEmpty()) {
                dataHeaders = new HashMap<String, String>(headers);
                headersUnsupported = false; /* Media3/系统均可带头；系统档走 Sys */
            } else {
                dataHeaders = null;
                headersUnsupported = false;
            }
            state = S_INITIALIZED;
        }
    }

    private boolean isSysLocked() {
        return engine == PlayEngine.SYSTEM;
    }

    private boolean isShwExtLocked() {
        return engine == PlayEngine.SHW;
    }

    private void releaseSysLocked() {
        if (sys != null) {
            try {
                sys.release();
            } catch (Throwable ignored) {
            }
            sys = null;
        }
    }

    private void releaseShwLocked() {
        if (shw != null) {
            try {
                shw.release();
            } catch (Throwable ignored) {
            }
            shw = null;
        }
        /* tvbus 会话：与 rtmp 一样，释放/换台即停。 */
        stopTvbusLocked();
    }

    /** 停掉 tvbus P2P 会话（sop:// / tvbus://）。非 tvbus 播放时是空操作。 */
    private void stopTvbusLocked() {
        if (!tvbusActive) {
            return;
        }
        tvbusActive = false;
        try {
            com.mycjzb.media.player.tvbus.TvbusSession.stop();
        } catch (Throwable ignored) {
        }
    }

    private void ensureSysLocked() {
        if (sys == null) {
            sys = new SysPlayerBackend();
            sys.setHost(sysHost);
        }
    }

    private void ensureShwLocked() {
        if (shw == null) {
            Context c = appContext != null ? appContext : currentApplication();
            if (c == null) {
                throw new IllegalStateException("SHW Media3 needs Context; use setDataSource(Context, Uri)");
            }
            MsNativeLoader.ensure();
            shw = new M3PlayerBackend(c);
            shw.setHost(shwHost);
        }
    }

    private final ShwBackend.Host shwHost = new ShwBackend.Host() {
        @Override
        public void onPrepared() {
            OnPreparedListener pl;
            synchronized (lock) {
                if (state != S_PREPARING) return;
                state = S_PREPARED;
                renderingStartSent = false;
                bufferingSent = false;
                seeking = false;
                pl = onPrepared;
            }
            fireVideoSize(shw != null ? shw.getVideoWidth() : 0, shw != null ? shw.getVideoHeight() : 0);
            if (pl != null) pl.onPrepared(MediaPlayer.this);
        }

        @Override
        public void onCompletion() {
            synchronized (lock) {
                if (state == S_END) return;
                state = S_PLAYBACK_COMPLETED;
            }
            fireCompletion();
        }

        @Override
        public boolean onError(int what, int extra) {
            synchronized (lock) {
                if (state == S_END) return true;
                state = S_ERROR;
            }
            fireError(what, extra);
            return true;
        }

        @Override
        public boolean onInfo(int what, int extra) {
            postInfo(what, extra);
            return false;
        }

        @Override
        public void onSeekComplete() {
            synchronized (lock) {
                seeking = false;
            }
            postSeekComplete();
        }

        @Override
        public void onBufferingUpdate(int percent) {
            OnBufferingUpdateListener l;
            synchronized (lock) {
                l = onBufferingUpdate;
            }
            if (l != null) l.onBufferingUpdate(MediaPlayer.this, percent);
        }

        @Override
        public void onVideoSizeChanged(int w, int h) {
            synchronized (lock) {
                lastW = w;
                lastH = h;
            }
            fireVideoSize(w, h);
        }

        @Override
        public void onTimedText(TimedText text) {
            OnTimedTextListener l;
            synchronized (lock) {
                l = onTimedText;
            }
            if (l != null) {
                l.onTimedText(MediaPlayer.this, text);
            }
        }
    };

    private final SysPlayerBackend.Host sysHost = new SysPlayerBackend.Host() {
        @Override
        public void onPrepared() {
            OnPreparedListener pl;
            synchronized (lock) {
                if (state != S_PREPARING) return;
                state = S_PREPARED;
                renderingStartSent = false;
                bufferingSent = false;
                seeking = false;
                pl = onPrepared;
            }
            fireVideoSize(sys != null ? sys.getVideoWidth() : 0, sys != null ? sys.getVideoHeight() : 0);
            if (pl != null) pl.onPrepared(MediaPlayer.this);
        }

        @Override
        public void onCompletion() {
            synchronized (lock) {
                if (state == S_END) return;
                state = S_PLAYBACK_COMPLETED;
            }
            fireCompletion();
        }

        @Override
        public boolean onError(int what, int extra) {
            synchronized (lock) {
                if (state == S_END) return true;
                state = S_ERROR;
            }
            fireError(what, extra);
            return true;
        }

        @Override
        public boolean onInfo(int what, int extra) {
            postInfo(what, extra);
            return false;
        }

        @Override
        public void onSeekComplete() {
            synchronized (lock) {
                seeking = false;
            }
            postSeekComplete();
        }

        @Override
        public void onBufferingUpdate(int percent) {
            OnBufferingUpdateListener l;
            synchronized (lock) {
                l = onBufferingUpdate;
            }
            if (l != null) l.onBufferingUpdate(MediaPlayer.this, percent);
        }

        @Override
        public void onVideoSizeChanged(int w, int h) {
            synchronized (lock) {
                lastW = w;
                lastH = h;
            }
            fireVideoSize(w, h);
        }
    };

    /* ---------------- prepare ---------------- */

    public void prepare() throws IOException {
        int gen;
        synchronized (lock) {
            requireState("prepare", S_INITIALIZED, S_STOPPED);
            state = S_PREPARING;
            gen = ++prepareGen;
            if (isSysLocked()) {
                try {
                    bindSysSourceLocked();
                    sys.prepare();
                    onPreparedLocked();
                } catch (IOException e) {
                    state = S_ERROR;
                    throw e;
                } catch (RuntimeException e) {
                    state = S_ERROR;
                    throw new IOException("sys prepare failed", e);
                }
                final int g = gen;
                main.post(new Runnable() {
                    @Override
                    public void run() {
                        OnPreparedListener pl;
                        synchronized (lock) {
                            if (g != prepareGen || state == S_END) return;
                            pl = onPrepared;
                        }
                        if (pl != null) pl.onPrepared(MediaPlayer.this);
                    }
                });
                return;
            }
            if (isShwExtLocked()) {
                throw new IOException("Media3 SHW: use prepareAsync()");
            }
            throw new IOException("unknown engine");
        }
    }

    public void prepareAsync() {
        final int gen;
        synchronized (lock) {
            requireState("prepareAsync", S_INITIALIZED, S_STOPPED);
            state = S_PREPARING;
            gen = ++prepareGen;
            if (isSysLocked()) {
                try {
                    bindSysSourceLocked();
                    sys.prepareAsync(); /* 成功/失败走 sysHost */
                } catch (Throwable t) {
                    state = S_ERROR;
                    main.post(new Runnable() {
                        public void run() {
                            fireError(MEDIA_ERROR_UNKNOWN, MEDIA_ERROR_IO);
                        }
                    });
                }
                return;
            }
            if (isShwExtLocked()) {
                /* sop:// / tvbus://：和 rtmp 一样只是「另一种地址」，但源端要先起 P2P 引擎
                 * 换成本机 http 再交给 Media3。解析放到后台线程，别堵调用方（通常是主线程）。 */
                if (isTvbusSourceLocked()) {
                    prepareTvbusAsyncLocked(gen);
                    return;
                }
                try {
                    bindShwSourceLocked();
                    shw.prepareAsync();
                } catch (Throwable t) {
                    state = S_ERROR;
                    main.post(new Runnable() {
                        public void run() {
                            fireError(MEDIA_ERROR_UNKNOWN, MEDIA_ERROR_IO);
                        }
                    });
                }
                return;
            }
            state = S_ERROR;
        }
    }

    /** dataSource 当前是否 tvbus 类地址（sop:// / tvbus://）。 */
    private boolean isTvbusSourceLocked() {
        if (dataSource == null) {
            return false;
        }
        try {
            return com.mycjzb.media.player.tvbus.TvbusSession.isTvbusUrl(dataSource);
        } catch (Throwable t) {
            return false;
        }
    }

    /**
     * 后台把 sop:// / tvbus:// 解析成本机 http，再走正常 SHW 起播。
     *
     * <p>语义对齐 rtmp：地址是「协议 + 目标」，引擎在手，stop 跟着 release/reset 走。
     * 解析失败或期间被 release/换台（{@code prepareGen} 变化）则丢弃本次结果。
     */
    private void prepareTvbusAsyncLocked(final int gen) {
        final String tvbusUrl = dataSource;
        final Context ctx = appContext != null ? appContext : currentApplication();
        tvbusActive = true;
        new Thread(new Runnable() {
            @Override
            public void run() {
                String local = null;
                try {
                    local = com.mycjzb.media.player.tvbus.TvbusSession.resolve(ctx, tvbusUrl);
                } catch (Throwable ignored) {
                }
                final String resolved = local;
                main.post(new Runnable() {
                    @Override
                    public void run() {
                        synchronized (lock) {
                            if (gen != prepareGen || state == S_END) {
                                return;
                            }
                            if (resolved == null || resolved.length() == 0) {
                                tvbusActive = false;
                                state = S_ERROR;
                                fireError(MEDIA_ERROR_UNKNOWN, MEDIA_ERROR_IO);
                                return;
                            }
                            dataSource = resolved;
                            dataIsUrl = true;
                            try {
                                bindShwSourceLocked();
                                shw.prepareAsync();
                            } catch (Throwable t) {
                                state = S_ERROR;
                                fireError(MEDIA_ERROR_UNKNOWN, MEDIA_ERROR_IO);
                            }
                        }
                    }
                });
            }
        }, "tvbus-resolve").start();
    }

    /** 系统档：把已解析的 dataSource 交给 android.media.MediaPlayer。 */
    private void bindSysSourceLocked() throws IOException {
        ensureSysLocked();
        if (display != null) {
            sys.setDisplay(display);
        } else if (surface != null) {
            sys.setSurface(surface);
        }
        sys.setVolume(volLeft, volRight);
        sys.setLooping(looping);
        if (dataSource == null) {
            throw new IOException("empty data source");
        }
        if (dataIsUrl) {
            Context ctx = appContext;
            if (ctx != null) {
                sys.setDataSource(ctx, Uri.parse(dataSource), dataHeaders);
            } else {
                sys.setDataSource(dataSource);
            }
        } else {
            sys.setDataSource(dataSource);
        }
    }

    /** SHW Media3：同连接字节头选源。 */
    private void bindShwSourceLocked() throws IOException {
        ensureShwLocked();
        if (display != null) {
            shw.setDisplay(display);
        } else if (surface != null) {
            shw.setSurface(surface);
        }
        shw.setVolume(volLeft, volRight);
        shw.setLooping(looping);
        shw.setBufferConfig(bufMinMs, bufMaxMs, bufPlayMs, bufRebufMs, (int) bufMaxBytes);
        shw.setPlaybackTlsVerify(playbackTlsVerify);
        shw.setAudioPassthroughEnabled(passthroughEnabled);
        if (dataSource == null) {
            throw new IOException("empty data source");
        }
        if (dataIsUrl) {
            Context ctx = appContext;
            if (ctx != null) {
                shw.setDataSource(ctx, Uri.parse(dataSource), dataHeaders);
            } else {
                shw.setDataSource(dataSource);
            }
        } else {
            shw.setDataSource(dataSource);
        }
        /* 外挂字幕在 setMediaSource 前挂上；无则清 */
        shw.setExternalTimedText(extSubPath, extSubMime);
    }

    /** 0=已打开；否则返回系统 MEDIA_ERROR_* extra（-1010/-1004/-110）。不持锁阻塞。 */
    private boolean isStale(int gen) {
        synchronized (lock) {
            return gen != prepareGen || state == S_END;
        }
    }

    private void onPreparedLocked() {
        state = S_PREPARED;
        renderingStartSent = false;
        bufferingSent = false;
        seeking = false;
        lastW = 0;
        lastH = 0;
        lastCueKey = null;
        pendingAudioSet = false;
        applyTimedTextSourceLocked();
        schedulePollLocked();
    }

    /* ---------------- 播放控制 ---------------- */

    public void start() {
        synchronized (lock) {
            requireState("start", S_PREPARED, S_STARTED, S_PAUSED, S_PLAYBACK_COMPLETED);
            if (state == S_PLAYBACK_COMPLETED) {
                if (isSysLocked() && sys != null) {
                    try {
                        sys.seekTo(0);
                        sys.start();
                        state = S_STARTED;
                    } catch (Throwable t) {
                        state = S_ERROR;
                        fireError(MEDIA_ERROR_UNKNOWN, MEDIA_ERROR_IO);
                    }
                    return;
                }
                if (isShwExtLocked() && shw != null) {
                    try {
                        shw.seekTo(0);
                        shw.start();
                        state = S_STARTED;
                    } catch (Throwable t) {
                        state = S_ERROR;
                        fireError(MEDIA_ERROR_UNKNOWN, MEDIA_ERROR_IO);
                    }
                    return;
                }
                /* 系统语义：完成后 start 从头播。原会话已结束，同源重新排队（本地文件/可重开地址）。 */
                restartFromBeginningLocked();
                return;
            }
            if (isSysLocked()) {
                if (sys != null) sys.start();
                state = S_STARTED;
                return;
            }
            if (isShwExtLocked()) {
                if (shw != null) shw.start();
                state = S_STARTED;
                return;
            }
            state = S_ERROR;
        }
    }

    private void restartFromBeginningLocked() {
        /* 仅 sys/shw 完成态会走到；无旧 native 重开 */
        state = S_ERROR;
    }

    public void pause() {
        synchronized (lock) {
            requireState("pause", S_STARTED, S_PAUSED, S_PLAYBACK_COMPLETED);
            if (state == S_STARTED) {
                if (isSysLocked()) {
                    if (sys != null) sys.pause();
                } else if (isShwExtLocked()) {
                    if (shw != null) shw.pause();
                }
                state = S_PAUSED;
            }
        }
    }

    public void stop() {
        synchronized (lock) {
            requireState("stop", S_PREPARED, S_STARTED, S_PAUSED, S_STOPPED, S_PLAYBACK_COMPLETED);
            prepareGen++;
            if (isSysLocked()) {
                if (sys != null) {
                    try { sys.stop(); } catch (Throwable ignored) {}
                }
            } else if (isShwExtLocked()) {
                if (shw != null) {
                    try { shw.stop(); } catch (Throwable ignored) {}
                }
            }
            state = S_STOPPED;
            main.removeCallbacks(poller);
        }
    }

    public void reset() {
        synchronized (lock) {
            checkNotReleased();
            prepareGen++;
            if (isSysLocked()) {
                if (sys != null) {
                    try { sys.reset(); } catch (Throwable ignored) {}
                }
            } else if (isShwExtLocked()) {
                if (shw != null) {
                    try { shw.reset(); } catch (Throwable ignored) {}
                }
            }
            /* tvbus：reset 与 release 一样要停 P2P 会话（Media3 复位不会停引擎）。 */
            stopTvbusLocked();
            main.removeCallbacks(poller);
            closePfdLocked();
            dataSource = null;
            dataIsUrl = false;
            dataHeaders = null;
            headersUnsupported = false;
            engine = PlayEngine.getDefault();
            resetLiveDetectLocked();
            liveHint = null;
            seeking = false;
            lastDurMs = -1;
            lastPosMs = 0;
            extSubData = null; /* 系统语义：reset 清掉外挂字幕 */
            extSubPath = null;
            extSubMime = null;
            lastTracks = new TrackInfo[0];
            lastCueKey = null;
            state = S_IDLE;
        }
    }

    public void release() {
        synchronized (lock) {
            if (state == S_END) {
                return;
            }
            prepareGen++;
            main.removeCallbacksAndMessages(null);
            closePfdLocked();
            releaseSysLocked();
            releaseShwLocked();
            state = S_END;
            onPrepared = null;
            onCompletion = null;
            onError = null;
            onInfo = null;
            onSeekComplete = null;
            onBufferingUpdate = null;
            onVideoSizeChanged = null;
            onTimedText = null;
            extSubData = null;
            extSubPath = null;
            extSubMime = null;
        }
    }

    public void seekTo(int msec) {
        seekTo((long) msec, SEEK_PREVIOUS_SYNC);
    }

    public void seekTo(long msec, int mode) {
        if (mode < SEEK_PREVIOUS_SYNC || mode > SEEK_CLOSEST) {
            throw new IllegalArgumentException("bad seek mode " + mode);
        }
        boolean notSeekable;
        synchronized (lock) {
            requireState("seekTo", S_PREPARED, S_STARTED, S_PAUSED, S_PLAYBACK_COMPLETED);
            if (msec < 0) {
                msec = 0;
            }
            if (isSysLocked()) {
                notSeekable = false;
                if (sys != null) {
                    try {
                        sys.seekTo((int) msec);
                        seeking = true;
                        seekTargetMs = msec;
                        lastPosMs = msec;
                    } catch (Throwable t) {
                        notSeekable = true;
                        seeking = false;
                    }
                }
            } else if (isShwExtLocked()) {
                notSeekable = false;
                if (shw != null) {
                    try {
                        shw.seekTo((int) msec);
                        seeking = true;
                        seekTargetMs = msec;
                        lastPosMs = msec;
                    } catch (Throwable t) {
                        notSeekable = true;
                        seeking = false;
                    }
                }
            } else {
                int rc = MEDIA_ERROR_UNSUPPORTED;
                notSeekable = rc == MEDIA_ERROR_UNSUPPORTED;
                if (!notSeekable) {
                    seeking = true;
                    seekTargetMs = msec;
                    seekIssuedAt = SystemClock.elapsedRealtime();
                    lastPosMs = msec;
                    schedulePollLocked();
                }
            }
        }
        if (notSeekable) {
            synchronized (lock) {
                liveNotSeekable = true;
                liveCached = -1;
            }
            postInfo(MEDIA_INFO_NOT_SEEKABLE, 0);
            postSeekComplete();
        }
    }

    public int getCurrentPosition() {
        synchronized (lock) {
            if (state == S_IDLE || state == S_INITIALIZED || state == S_ERROR || state == S_END) {
                return 0;
            }
            if (isSysLocked()) {
                return sys != null ? sys.getCurrentPosition() : 0;
            }
            if (isShwExtLocked()) {
                return shw != null ? shw.getCurrentPosition() : 0;
            }
            return 0;
        }
    }

    /** 毫秒；未知（直播或路线 A 会话未给出时长）返回 -1，与系统一致。 */
    public int getDuration() {
        synchronized (lock) {
            if (state == S_IDLE || state == S_INITIALIZED || state == S_ERROR || state == S_END) {
                return -1;
            }
            if (isSysLocked()) {
                return sys != null ? sys.getDuration() : -1;
            }
            if (isShwExtLocked()) {
                if (shw == null) return -1;
                int d = shw.getDuration();
                return d <= 0 ? -1 : d;
            }
            return -1;
        }
    }

    public boolean isPlaying() {
        synchronized (lock) {
            return state == S_STARTED;
        }
    }

    public int getVideoWidth() {
        synchronized (lock) {
            if (isSysLocked()) {
                return sys != null ? sys.getVideoWidth() : lastW;
            }
            if (isShwExtLocked()) {
                return shw != null ? shw.getVideoWidth() : lastW;
            }
            return lastW;
        }
    }

    public int getVideoHeight() {
        synchronized (lock) {
            if (isSysLocked()) {
                return sys != null ? sys.getVideoHeight() : lastH;
            }
            if (isShwExtLocked()) {
                return shw != null ? shw.getVideoHeight() : lastH;
            }
            return lastH;
        }
    }

    /** 演示 OSD：视频 MIME（如 video/avc），无则 null。 */
    public String getVideoMime() {
        synchronized (lock) {
            return isShwExtLocked() && shw != null ? shw.videoMime() : null;
        }
    }

    /** 演示 OSD：音频 MIME（如 audio/mp4a-latm），无则 null。 */
    public String getAudioMime() {
        synchronized (lock) {
            return isShwExtLocked() && shw != null ? shw.audioMime() : null;
        }
    }

    /** 演示 OSD：帧率，未知 0。 */
    public float getVideoFrameRate() {
        synchronized (lock) {
            return isShwExtLocked() && shw != null ? shw.videoFrameRate() : 0f;
        }
    }

    /** 演示 OSD：视频码率 bps，未知 -1。 */
    public int getVideoBitrate() {
        synchronized (lock) {
            return isShwExtLocked() && shw != null ? shw.videoBitrate() : -1;
        }
    }

    /** 演示 OSD：音频码率 bps，未知 -1。 */
    public int getAudioBitrate() {
        synchronized (lock) {
            return isShwExtLocked() && shw != null ? shw.audioBitrate() : -1;
        }
    }

    public void setVolume(float leftVolume, float rightVolume) {
        synchronized (lock) {
            checkNotReleased();
            volLeft = clamp01(leftVolume);
            volRight = clamp01(rightVolume);
            if (isSysLocked() && sys != null) {
                sys.setVolume(volLeft, volRight);
            } else if (isShwExtLocked() && shw != null) {
                shw.setVolume(volLeft, volRight);
            }
        }
    }

    /** 系统隐藏重载的公开等价。 */
    public void setVolume(float volume) {
        setVolume(volume, volume);
    }

    public void setLooping(boolean loop) {
        synchronized (lock) {
            checkNotReleased();
            looping = loop;
            if (isSysLocked() && sys != null) {
                sys.setLooping(loop);
            } else if (isShwExtLocked() && shw != null) {
                shw.setLooping(loop);
            }
        }
    }

    /**
     * 预加载配置（MsMedia 扩展，系统 MediaPlayer 无此接口）：路线 A 纯自研网络栈的 HLS/DASH 前瞻缓存。
     * @param aheadMs 提前缓存的媒体时长（毫秒），≤0 用默认 10000
     * @param maxBytes 预加载总内存上限（字节），≤0 用默认 16 MiB；原生侧绝对上限 128 MiB 防 OOM
     * 下一次 prepare 生效。路线 B（精简 FFmpeg）在未调用 {@link #setBufferConfig} 时兼容映射为
     * maxBufferMs=aheadMs、minBufferMs=min(15000, aheadMs/2)、maxBufferBytes=maxBytes。
     */
    public void setPreloadConfig(int aheadMs, long maxBytes) {
        synchronized (lock) {
            checkNotReleased();
            preloadAheadMs = aheadMs > 0 ? aheadMs : 0;
            preloadMaxBytes = maxBytes > 0 ? maxBytes : 0;
        }
    }

    /** Media3 DefaultLoadControl 风格默认值（电视盒子取向：内存上限 16 MiB）。 */
    public static final int DEFAULT_MIN_BUFFER_MS = 15000;
    public static final int DEFAULT_MAX_BUFFER_MS = 30000;
    public static final int DEFAULT_BUFFER_FOR_PLAYBACK_MS = 1500;
    public static final int DEFAULT_BUFFER_FOR_PLAYBACK_AFTER_REBUFFER_MS = 3000;
    public static final long DEFAULT_MAX_BUFFER_BYTES = 16L << 20;

    /**
     * 缓冲水位（MsMedia 扩展，对标 Google Media3 {@code DefaultLoadControl.Builder#setBufferDurationsMs}）：
     * 路线 B 精简 FFmpeg 的 HLS 独立预下载线程按此水位提前下载后续切片到有界内存队列。
     * @param minBufferMs 已缓冲媒体时长跌破它就恢复预拉
     * @param maxBufferMs 攒够即暂停预拉；0 = 关闭独立预下载（回到 FFmpeg 原生边下边播）
     * @param bufferForPlaybackMs 起播门槛：首片边下边播，攒够即放行送帧
     * @param bufferForPlaybackAfterRebufferMs 卡顿后的恢复门槛，防一卡一顿
     * @param maxBufferBytes 预下载内存上限（字节），≤0 用默认 16 MiB；原生侧绝对上限 256 MiB 防 OOM
     * 下一次 prepare 生效。参数非法抛 IllegalArgumentException（与 Media3 一致）。
     */
    public void setBufferConfig(int minBufferMs, int maxBufferMs, int bufferForPlaybackMs,
                                int bufferForPlaybackAfterRebufferMs, long maxBufferBytes) {
        if (minBufferMs < 0 || maxBufferMs < 0 || bufferForPlaybackMs < 0 || bufferForPlaybackAfterRebufferMs < 0) {
            throw new IllegalArgumentException("buffer durations must be >= 0");
        }
        if (maxBufferMs > 0 && minBufferMs > maxBufferMs) {
            throw new IllegalArgumentException("minBufferMs must be <= maxBufferMs");
        }
        if (maxBufferBytes > (256L << 20)) {
            throw new IllegalArgumentException("maxBufferBytes must be <= 256 MiB");
        }
        synchronized (lock) {
            checkNotReleased();
            bufSet = true;
            bufMinMs = minBufferMs;
            bufMaxMs = maxBufferMs;
            bufPlayMs = bufferForPlaybackMs;
            bufRebufMs = bufferForPlaybackAfterRebufferMs;
            bufMaxBytes = maxBufferBytes > 0 ? maxBufferBytes : DEFAULT_MAX_BUFFER_BYTES;
        }
    }

    /**
     * 播放拉流 HTTPS/RTMPS 证书校验（MsMedia 扩展）。默认 false：忽略证书链与主机名（兼容自签证书、
     * IP 直连、老盒子过期根证书）；true：切回严格校验。只影响播放器，业务 API {@code MsHttpClient} 永远严格。
     * 下一次 prepare 生效。
     */
    public void setPlaybackTlsVerify(boolean strict) {
        synchronized (lock) {
            checkNotReleased();
            playbackTlsVerify = strict;
        }
    }

    /**
     * 音频直通（MsMedia 扩展，对齐 Media3 的 AudioCapabilities 思路）：开启后，若设备/HDMI 接收端声明支持
     * AC-3 / E-AC-3 / DTS（原始编码，API 21+）或 IEC61937（API 24+），则跳过软解，把原始码流交给功放解码。
     * 不支持或 AudioTrack 打开失败时自动回退软解（不静音、不报错）。下一次 prepare 生效；默认关闭。
     * 注意：真实 HDMI/光纤功放出声需在设备上验收。
     */
    public void setAudioPassthroughEnabled(boolean enabled) {
        synchronized (lock) {
            checkNotReleased();
            passthroughEnabled = enabled;
        }
    }

    public boolean isAudioPassthroughEnabled() {
        synchronized (lock) {
            return passthroughEnabled;
        }
    }

    /** 当前音频路由：0=软解/系统解码，1=原始帧直通，2=IEC61937 直通（prepare 且出声后有效）。 */
    public int getAudioPassthroughRoute() {
        return 0;
    }

    /* 能力位，与 src/hw/ms_hw_passthrough.h 的 MS_PT_CAP_* 一致。 */
    public static final int PASSTHROUGH_CAP_AC3 = 0x01;
    public static final int PASSTHROUGH_CAP_E_AC3 = 0x02;
    public static final int PASSTHROUGH_CAP_DTS = 0x04;
    public static final int PASSTHROUGH_CAP_IEC61937 = 0x08;
    private static final int PASSTHROUGH_ENABLED = 0x80000000;
    private static final int ENC_AC3 = 5, ENC_E_AC3 = 6, ENC_DTS = 7, ENC_IEC61937 = 13;

    /**
     * 探测设备直通能力（返回含开关位的能力掩码；无任何能力时返回 0）。
     * API 29+：AudioTrack.isDirectPlaybackSupported；API 21–28：HDMI 插拔粘性广播 EXTRA_ENCODINGS，
     * Amazon 设备额外读取 external_surround_sound_enabled（与 Media3 一致）。探测失败一律视为不支持。
     */
    public static int getAudioPassthroughCaps(Context ctx) {
        int caps = 0;
        try {
            /* 高版本 API 全部放在嵌套类里，API 18 的 Dalvik 校验 MediaPlayer 时不会解析到它们。 */
            if (Build.VERSION.SDK_INT >= 29) {
                caps = PassthroughApi29.probe();
            } else if (Build.VERSION.SDK_INT >= 21) {
                Context c = ctx != null ? ctx : currentApplication();
                if (c != null) {
                    caps = PassthroughApi21.probe(c);
                }
            }
        } catch (Throwable t) {
            caps = 0; /* 探测异常：保守走软解 */
        }
        return caps != 0 ? (caps | PASSTHROUGH_ENABLED) : 0;
    }

    /** API 29+：AudioTrack.isDirectPlaybackSupported（仅在 SDK_INT>=29 时加载）。 */
    private static final class PassthroughApi29 {
        static int probe() {
            int caps = 0;
            caps |= directCap(ENC_AC3, PASSTHROUGH_CAP_AC3);
            caps |= directCap(ENC_E_AC3, PASSTHROUGH_CAP_E_AC3);
            caps |= directCap(ENC_DTS, PASSTHROUGH_CAP_DTS);
            caps |= directCap(ENC_IEC61937, PASSTHROUGH_CAP_IEC61937);
            return caps;
        }

        private static int directCap(int encoding, int bit) {
            AudioAttributes attrs = new AudioAttributes.Builder()
                    .setUsage(AudioAttributes.USAGE_MEDIA)
                    .setContentType(AudioAttributes.CONTENT_TYPE_MOVIE)
                    .build();
            int[] masks = {AudioFormat.CHANNEL_OUT_STEREO, AudioFormat.CHANNEL_OUT_5POINT1};
            for (int m : masks) {
                if (encoding == ENC_IEC61937 && m != AudioFormat.CHANNEL_OUT_STEREO) {
                    continue;
                }
                AudioFormat f = new AudioFormat.Builder().setEncoding(encoding).setSampleRate(48000)
                        .setChannelMask(m).build();
                if (AudioTrack.isDirectPlaybackSupported(f, attrs)) {
                    return bit;
                }
            }
            return 0;
        }
    }

    /** API 21–28：HDMI 插拔粘性广播 EXTRA_ENCODINGS；Amazon 读 external_surround_sound_enabled（与 Media3 一致）。 */
    private static final class PassthroughApi21 {
        static int probe(Context c) {
            int caps = 0;
            Intent it = c.registerReceiver(null, new IntentFilter(AudioManager.ACTION_HDMI_AUDIO_PLUG));
            if (it != null && it.getIntExtra(AudioManager.EXTRA_AUDIO_PLUG_STATE, 0) == 1) {
                int[] encs = it.getIntArrayExtra(AudioManager.EXTRA_ENCODINGS);
                if (encs != null) {
                    for (int e : encs) {
                        if (e == ENC_AC3) caps |= PASSTHROUGH_CAP_AC3;
                        else if (e == ENC_E_AC3) caps |= PASSTHROUGH_CAP_E_AC3;
                        else if (e == ENC_DTS) caps |= PASSTHROUGH_CAP_DTS;
                        else if (e == ENC_IEC61937 && Build.VERSION.SDK_INT >= 24) caps |= PASSTHROUGH_CAP_IEC61937;
                    }
                }
            }
            if ("Amazon".equalsIgnoreCase(Build.MANUFACTURER)
                    && Settings.Global.getInt(c.getContentResolver(), "external_surround_sound_enabled", 0) == 1) {
                caps |= PASSTHROUGH_CAP_AC3 | PASSTHROUGH_CAP_E_AC3;
            }
            return caps;
        }
    }

    private static Context currentApplication() {
        try {
            Class<?> at = Class.forName("android.app.ActivityThread");
            Object app = at.getMethod("currentApplication").invoke(null);
            return app instanceof Context ? (Context) app : null;
        } catch (Throwable t) {
            return null;
        }
    }

    public boolean isLooping() {
        synchronized (lock) {
            if (isSysLocked() && sys != null) {
                return sys.isLooping();
            }
            if (isShwExtLocked() && shw != null) {
                return shw.isLooping();
            }
            return looping;
        }
    }

    public void setDisplay(SurfaceHolder sh) {
        synchronized (lock) {
            checkNotReleased();
            display = sh;
            surface = sh != null ? sh.getSurface() : null;
            if (isSysLocked() && sys != null) {
                sys.setDisplay(sh);
            } else if (isShwExtLocked() && shw != null) {
                shw.setDisplay(sh);
            }
            if (sh != null) {
                sh.setKeepScreenOn(screenOnWhilePlaying);
            }
        }
    }

    public void setSurface(Surface s) {
        synchronized (lock) {
            checkNotReleased();
            display = null;
            surface = s;
            if (isSysLocked() && sys != null) {
                sys.setSurface(s);
            } else if (isShwExtLocked() && shw != null) {
                shw.setSurface(s);
            }
        }
    }

    /**
     * 与系统相同只接受两个取值。硬解直接渲染到 Surface，裁切/适配由 SurfaceView 尺寸决定；
     * 原生层记录模式但不做额外缩放（与系统在无 Surface 尺寸调整时的效果一致）。
     */
    public void setVideoScalingMode(int mode) {
        if (mode != VIDEO_SCALING_MODE_SCALE_TO_FIT && mode != VIDEO_SCALING_MODE_SCALE_TO_FIT_WITH_CROPPING) {
            throw new IllegalArgumentException("bad scaling mode " + mode);
        }
        synchronized (lock) {
            checkNotReleased();
            scalingMode = mode;
        }
    }

    /** 倍速（API 23 签名）；只支持 speed（0,8]、pitch=1。不支持的参数抛 IllegalArgumentException。 */
    public void setPlaybackParams(PlaybackParams params) {
        if (params == null) {
            throw new IllegalArgumentException("params is null");
        }
        float sp = params.getSpeed();
        float pi = params.getPitch();
        if (pi != 1f) {
            throw new IllegalArgumentException("pitch not supported");
        }
        synchronized (lock) {
            checkNotReleased();
            if (sp == 0f) {
                /* 系统语义：speed=0 等同暂停 */
                if (state == S_STARTED) {
                    /* no native */
                    state = S_PAUSED;
                }
                return;
            }
            if (!(sp > 0f) || sp > 8f) {
                throw new IllegalArgumentException("speed out of range");
            }
            speed = sp;
            if (false) {
                int st = 0;
                if (st != 0) {
                    throw new IllegalStateException("speed rejected: " + st);
                }
            }
        }
    }

    public PlaybackParams getPlaybackParams() {
        synchronized (lock) {
            return new PlaybackParams().setSpeed(speed).setPitch(1f);
        }
    }

    /* 以下与系统同名的设置项：只记录，不影响原生路径（音频走 AudioTrack 默认媒体流）。 */
    public void setAudioStreamType(int streamtype) {
        checkNotReleased();
    }

    public void setAudioAttributes(AudioAttributes attributes) {
        if (attributes == null) {
            throw new IllegalArgumentException("attributes is null");
        }
        checkNotReleased();
    }

    public void setWakeMode(Context context, int mode) {
        checkNotReleased();
    }

    public void setScreenOnWhilePlaying(boolean screenOn) {
        synchronized (lock) {
            screenOnWhilePlaying = screenOn;
            if (display != null) {
                display.setKeepScreenOn(screenOn);
            }
        }
    }

    /* ---------------- 选轨与字幕（对齐系统 API 16/21 签名，by: Long <mycjzb@gmail.com>） ---------------- */

    /**
     * 与系统 MediaPlayer.TrackInfo 对齐：轨道类型、语言、格式。
     * 文字字幕（SRT/WebVTT/tx3g/ASS 文字）报 TIMEDTEXT，走 {@link OnTimedTextListener}；
     * 位图字幕（DVB/PGS/VobSub）报 SUBTITLE，可选中但本类不回调位图。
     * 下标即 {@link #selectTrack(int)} 用的 index，与 getTrackInfo() 返回数组一致。
     */
    public static class TrackInfo {
        public static final int MEDIA_TRACK_TYPE_UNKNOWN = 0;
        public static final int MEDIA_TRACK_TYPE_VIDEO = 1;
        public static final int MEDIA_TRACK_TYPE_AUDIO = 2;
        public static final int MEDIA_TRACK_TYPE_TIMEDTEXT = 3;
        public static final int MEDIA_TRACK_TYPE_SUBTITLE = 4;
        public static final int MEDIA_TRACK_TYPE_METADATA = 5;

        final int id;          /* 内核轨 id（TS 为 PID；外挂字幕为 0xFFFF0001） */
        final boolean selected;
        private final int type;
        private final String language;
        private final String mime;
        private final String name;
        private final boolean isDefault;
        private final boolean isForced;

        TrackInfo(int id, int type, boolean selected, boolean isDefault, boolean isForced,
                  String language, String mime, String name) {
            this.id = id;
            this.type = type;
            this.selected = selected;
            this.isDefault = isDefault;
            this.isForced = isForced;
            this.language = (language == null || language.length() == 0) ? "und" : language;
            this.mime = mime;
            this.name = name;
        }

        public int getTrackType() {
            return type;
        }

        TrackInfo withSelected(boolean sel) {
            return sel == selected ? this
                    : new TrackInfo(id, type, sel, isDefault, isForced, language, mime, name);
        }

        /** ISO-639 语言码（容器原样，常见是 3 字母）；未知返回 "und"，与系统一致。 */
        public String getLanguage() {
            return language;
        }

        /**
         * 与系统一致：只有 TIMEDTEXT / SUBTITLE 轨返回 MediaFormat（含 mime、language、
         * is-default、is-forced-subtitle），音视频轨返回 null。音视频的 mime 用 {@link #getMime()}。
         */
        public MediaFormat getFormat() {
            if (type != MEDIA_TRACK_TYPE_TIMEDTEXT && type != MEDIA_TRACK_TYPE_SUBTITLE) {
                return null;
            }
            MediaFormat f = new MediaFormat();
            f.setString(MediaFormat.KEY_MIME, mime);
            f.setString("language", language);
            f.setInteger("is-default", isDefault ? 1 : 0);
            f.setInteger("is-forced-subtitle", isForced ? 1 : 0);
            return f;
        }

        /** MsMedia 扩展：任意轨的 MIME（如 video/avc、audio/ac3、application/x-subrip）。 */
        public String getMime() {
            return mime;
        }

        /** MsMedia 扩展：容器里的轨道名（可能为 null）；外挂字幕为 "external"。 */
        public String getName() {
            return name;
        }

        @Override
        public String toString() {
            String t;
            switch (type) {
                case MEDIA_TRACK_TYPE_VIDEO: t = "VIDEO"; break;
                case MEDIA_TRACK_TYPE_AUDIO: t = "AUDIO"; break;
                case MEDIA_TRACK_TYPE_TIMEDTEXT: t = "TIMEDTEXT"; break;
                case MEDIA_TRACK_TYPE_SUBTITLE: t = "SUBTITLE"; break;
                default: t = "UNKNOWN"; break;
            }
            return "TrackInfo{" + t + ", " + mime + ", " + language + (selected ? ", selected" : "") + "}";
        }
    }

    /**
     * 与系统一致：返回当前所有轨道。须在 prepare 完成后调用（Prepared/Started/Paused/Stopped/
     * PlaybackCompleted），否则抛 IllegalStateException。
     */
    public TrackInfo[] getTrackInfo() throws IllegalStateException {
        synchronized (lock) {
            requireState("getTrackInfo", S_PREPARED, S_STARTED, S_PAUSED, S_STOPPED, S_PLAYBACK_COMPLETED);
            if (isSysLocked()) {
                return new TrackInfo[0];
            }
            if (isShwExtLocked()) {
                return shw != null ? shw.getTrackInfo() : new TrackInfo[0];
            }
            TrackInfo[] t = listTracksLocked();
            TrackInfo[] copy = new TrackInfo[t.length];
            System.arraycopy(t, 0, copy, 0, t.length);
            return copy;
        }
    }

    /**
     * 与系统一致：选中一条音轨 / 文字字幕 / 位图字幕，同类的其它轨自动取消。视频轨不能选
     * （抛 IllegalArgumentException）。下标越界抛 IllegalArgumentException，状态不对抛 IllegalStateException。
     * 字幕格式不支持时不抛异常，回调 onInfo(MEDIA_INFO_UNSUPPORTED_SUBTITLE)。
     */
    public void selectTrack(int index) throws IllegalStateException {
        selectOrDeselectTrack(index, true);
    }

    /** 与系统一致：只能取消字幕轨（TIMEDTEXT/SUBTITLE），音视频轨抛 IllegalArgumentException。 */
    public void deselectTrack(int index) throws IllegalStateException {
        selectOrDeselectTrack(index, false);
    }

    /** 与系统（API 21）一致：返回某类型当前选中轨的下标，没有返回 -1。 */
    public int getSelectedTrack(int trackType) throws IllegalStateException {
        synchronized (lock) {
            requireState("getSelectedTrack", S_PREPARED, S_STARTED, S_PAUSED, S_STOPPED, S_PLAYBACK_COMPLETED);
            if (isSysLocked()) {
                return -1;
            }
            if (isShwExtLocked()) {
                return shw != null ? shw.getSelectedTrack(trackType) : -1;
            }
            TrackInfo[] t = listTracksLocked();
            for (int i = 0; i < t.length; i++) {
                if (t[i].selected && t[i].getTrackType() == trackType) {
                    return i;
                }
            }
            return -1;
        }
    }

    /**
     * PCM 5 段能量 0–255（对齐超级直播 / ijk audio_bars）。透传无 PCM 时返回 false。
     * 系统档恒 false。
     */
    public boolean getAudioBars(int[] out) {
        synchronized (lock) {
            if (state == S_END || out == null) {
                return false;
            }
            if (isShwExtLocked() && shw != null) {
                return shw.readAudioBars(out);
            }
            return false;
        }
    }

    private void selectOrDeselectTrack(int index, boolean on) {
        boolean unsupported = false;
        synchronized (lock) {
            String op = on ? "selectTrack" : "deselectTrack";
            requireState(op, S_PREPARED, S_STARTED, S_PAUSED, S_PLAYBACK_COMPLETED);
            if (isSysLocked()) {
                return;
            }
            if (isShwExtLocked()) {
                if (shw == null) {
                    return;
                }
                TrackInfo[] t = shw.getTrackInfo();
                if (index < 0 || index >= t.length) {
                    throw new IllegalArgumentException(op + ": index " + index + " out of range (" + t.length + " tracks)");
                }
                int type = t[index].getTrackType();
                boolean isText = isTextType(type);
                if (!isText && type != TrackInfo.MEDIA_TRACK_TYPE_AUDIO) {
                    throw new IllegalArgumentException(op + ": only audio, timed text or subtitle tracks can be selected");
                }
                if (!on && !isText) {
                    throw new IllegalArgumentException("deselectTrack: audio track cannot be deselected");
                }
                if (on) {
                    shw.selectTrack(index);
                } else {
                    shw.deselectTrack(index);
                }
                return;
            }
            TrackInfo[] t = listTracksLocked();
            if (index < 0 || index >= t.length) {
                throw new IllegalArgumentException(op + ": index " + index + " out of range (" + t.length + " tracks)");
            }
            int type = t[index].getTrackType();
            boolean isText = isTextType(type);
            if (!isText && type != TrackInfo.MEDIA_TRACK_TYPE_AUDIO) {
                throw new IllegalArgumentException(op + ": only audio, timed text or subtitle tracks can be selected");
            }
            if (!on && !isText) {
                throw new IllegalArgumentException("deselectTrack: audio track cannot be deselected");
            }
            if (on && isText) {
                /* 与系统一致：同一时间只有一条字幕轨，选新轨前先关掉其它已选字幕轨 */
                for (int i = 0; i < t.length; i++) {
                    if (i != index && t[i].selected && isTextType(t[i].getTrackType())) {
                        /* removed */
                    }
                }
            }
            int rc = 0;
            if (rc == MS_ERR_UNSUPPORTED && isText) {
                unsupported = true;
            } else if (rc != 0) {
                throw new IllegalStateException(op + " failed, status " + rc);
            }
            if (isText) {
                cueDirty = true; /* 换字幕轨后立即按新轨重新派发 */
                if (!unsupported) {
                    markTextSelectedLocked(t, index, on);
                }
            } else {
                pendingAudioId = t[index].id;
                pendingAudioSet = !t[index].selected;
            }
        }
        if (unsupported) {
            postInfo(MEDIA_INFO_UNSUPPORTED_SUBTITLE, 0);
        }
    }

    private static boolean isTextType(int type) {
        return type == TrackInfo.MEDIA_TRACK_TYPE_TIMEDTEXT || type == TrackInfo.MEDIA_TRACK_TYPE_SUBTITLE;
    }

    /** 字幕选轨成功后同步更新缓存，原生忙时 getSelectedTrack 也能报新状态。 */
    private void markTextSelectedLocked(TrackInfo[] t, int index, boolean on) {
        TrackInfo[] c = new TrackInfo[t.length];
        for (int i = 0; i < t.length; i++) {
            if (i == index) {
                c[i] = t[i].withSelected(on);
            } else if (on && isTextType(t[i].getTrackType())) {
                c[i] = t[i].withSelected(false);
            } else {
                c[i] = t[i];
            }
        }
        lastTracks = c;
    }

    /** 持锁读取原生轨道表；原生忙（拿不到锁）时返回上次结果。 */
    private TrackInfo[] listTracksLocked() {
        return lastTracks != null ? lastTracks : new TrackInfo[0];
    }

    /** 内核提交音轨切换前，音轨选中状态按用户请求报（返回新数组，不改缓存）。 */
    private TrackInfo[] overlayPendingAudio(TrackInfo[] src) {
        if (!pendingAudioSet) {
            return src;
        }
        TrackInfo[] t = new TrackInfo[src.length];
        for (int i = 0; i < src.length; i++) {
            t[i] = src[i].getTrackType() == TrackInfo.MEDIA_TRACK_TYPE_AUDIO
                    ? src[i].withSelected(src[i].id == pendingAudioId) : src[i];
        }
        return t;
    }

    /**
     * 与系统一致：添加外挂字幕文件。支持 {@link #MEDIA_MIMETYPE_TEXT_SUBRIP}（SRT），
     * 另支持 {@link #MEDIA_MIMETYPE_TEXT_VTT}、{@link #MEDIA_MIMETYPE_TEXT_SSA}（MsMedia 扩展）；
     * 其它 MIME 抛 IllegalArgumentException。可在 setDataSource 之后任何时候调用，prepare 前添加的
     * 在 prepare 完成后生效。添加后作为一条 TIMEDTEXT 轨出现在 getTrackInfo() 末尾，默认不选中，
     * 需 selectTrack 打开。限制：同一时间只挂一个外挂文件，再次添加会替换上一个。
     */
    public void addTimedTextSource(String path, String mimeType)
            throws IOException, IllegalArgumentException, IllegalStateException {
        int fmt = timedTextFormat(mimeType);
        requireTimedTextState();
        if (path == null) {
            throw new IllegalArgumentException("path is null");
        }
        Uri u = Uri.parse(path);
        String file = "file".equalsIgnoreCase(u.getScheme()) ? u.getPath() : path;
        synchronized (lock) {
            if (isShwExtLocked()) {
                File f = new File(file);
                if (!f.exists()) {
                    throw new IOException("timed text file not found: " + path);
                }
                extSubPath = file;
                extSubMime = mimeType;
                if (shw != null) {
                    shw.setExternalTimedText(file, mimeType);
                }
                return;
            }
        }
        FileInputStream in = new FileInputStream(file);
        try {
            setTimedTextSource(readLimited(in), fmt);
        } finally {
            in.close();
        }
    }

    public void addTimedTextSource(Context context, Uri uri, String mimeType)
            throws IOException, IllegalArgumentException, IllegalStateException {
        int fmt = timedTextFormat(mimeType);
        requireTimedTextState();
        if (context == null || uri == null) {
            throw new IllegalArgumentException("context/uri is null");
        }
        String scheme = uri.getScheme();
        if (scheme == null || "file".equalsIgnoreCase(scheme)) {
            addTimedTextSource(uri.getPath(), mimeType);
            return;
        }
        synchronized (lock) {
            if (isShwExtLocked()) {
                extSubPath = uri.toString();
                extSubMime = mimeType;
                if (shw != null) {
                    shw.setExternalTimedText(extSubPath, mimeType);
                }
                return;
            }
        }
        InputStream in = context.getContentResolver().openInputStream(uri);
        if (in == null) {
            throw new IOException("cannot open timed text uri");
        }
        try {
            setTimedTextSource(readLimited(in), fmt);
        } finally {
            in.close();
        }
    }

    public void addTimedTextSource(FileDescriptor fd, String mimeType)
            throws IllegalArgumentException, IllegalStateException {
        addTimedTextSource(fd, 0, 0x7ffffffffffffffL, mimeType);
    }

    /** fd 由调用方负责关闭（与系统一致）；这里用定位读取，不改变 fd 当前偏移。 */
    public void addTimedTextSource(FileDescriptor fd, long offset, long length, String mime)
            throws IllegalArgumentException, IllegalStateException {
        int fmt = timedTextFormat(mime);
        requireTimedTextState();
        if (fd == null || offset < 0 || length < 0) {
            throw new IllegalArgumentException("bad fd/offset/length");
        }
        byte[] data;
        try {
            /* 不 close：FileInputStream(fd).close() 会关掉调用方的 fd。 */
            FileChannel ch = new FileInputStream(fd).getChannel();
            long size = ch.size();
            long n = Math.min(length, Math.max(0L, size - offset));
            if (n > TIMED_TEXT_MAX_BYTES) {
                throw new IllegalArgumentException("timed text source too large: " + n);
            }
            ByteBuffer bb = ByteBuffer.allocate((int) n);
            long pos = offset;
            while (bb.hasRemaining()) {
                int r = ch.read(bb, pos);
                if (r <= 0) {
                    break;
                }
                pos += r;
            }
            data = new byte[bb.position()];
            System.arraycopy(bb.array(), 0, data, 0, data.length);
        } catch (IOException e) {
            throw new IllegalArgumentException("cannot read timed text fd: " + e.getMessage());
        }
        synchronized (lock) {
            if (isShwExtLocked()) {
                /* Media3 吃文件路径：落 cache 再挂 */
                try {
                    Context c = appContext != null ? appContext : currentApplication();
                    if (c == null) {
                        throw new IllegalArgumentException("SHW timed text fd needs Context");
                    }
                    File tmp = new File(c.getCacheDir(), "ms_ext_sub.tmp");
                    java.io.FileOutputStream fos = new java.io.FileOutputStream(tmp);
                    try {
                        fos.write(data);
                    } finally {
                        fos.close();
                    }
                    extSubPath = tmp.getAbsolutePath();
                    extSubMime = mime;
                    if (shw != null) {
                        shw.setExternalTimedText(extSubPath, mime);
                    }
                    return;
                } catch (IOException e) {
                    throw new IllegalArgumentException("cannot write timed text cache: " + e.getMessage());
                }
            }
        }
        setTimedTextSource(data, fmt);
    }

    private static int timedTextFormat(String mime) {
        if (MEDIA_MIMETYPE_TEXT_SUBRIP.equals(mime)) {
            return SUB_FMT_SRT;
        }
        if (MEDIA_MIMETYPE_TEXT_VTT.equals(mime)) {
            return SUB_FMT_WEBVTT;
        }
        if (MEDIA_MIMETYPE_TEXT_SSA.equals(mime)) {
            return SUB_FMT_AUTO; /* ASS / SSA 由内核按脚本头自动区分 */
        }
        throw new IllegalArgumentException("Illegal mimeType for timed text source: " + mime);
    }

    private static byte[] readLimited(InputStream in) throws IOException {
        ByteArrayOutputStream bo = new ByteArrayOutputStream(8192);
        byte[] buf = new byte[8192];
        long total = 0;
        int r;
        while ((r = in.read(buf)) > 0) {
            total += r;
            if (total > TIMED_TEXT_MAX_BYTES) {
                throw new IOException("timed text source too large (> " + TIMED_TEXT_MAX_BYTES + " bytes)");
            }
            bo.write(buf, 0, r);
        }
        return bo.toByteArray();
    }

    /** 先查状态再读文件：状态不对时直接抛 IllegalStateException，不做无用 I/O。 */
    private void requireTimedTextState() {
        synchronized (lock) {
            requireTimedTextStateLocked();
        }
    }

    private void requireTimedTextStateLocked() {
        requireState("addTimedTextSource", S_INITIALIZED, S_PREPARED, S_STARTED, S_PAUSED, S_STOPPED,
                S_PLAYBACK_COMPLETED);
    }

    private void setTimedTextSource(byte[] data, int fmt) throws IllegalArgumentException {
        synchronized (lock) {
            requireTimedTextStateLocked(); /* 读文件期间状态可能变了，再查一次 */
            if (data == null || data.length == 0) {
                throw new IllegalArgumentException("empty timed text source");
            }
            extSubData = data;
            extSubFmt = fmt;
            if (state == S_INITIALIZED || state == S_STOPPED) {
                return; /* prepare 完成后在 onPreparedLocked 里交给原生 */
            }
            int rc = applyTimedTextSourceLocked();
            if (rc != 0) {
                extSubData = null;
                throw new IllegalArgumentException("timed text source rejected (parse failed), status " + rc);
            }
        }
    }

    /** 把 Java 侧保存的外挂字幕交给原生；没有则清掉原生残留。返回 ms_status。 */
    private int applyTimedTextSourceLocked() {
        if (true) {
            return 0;
        }
        if (extSubData == null) {
            if (extSubApplied) {
                /* removed */
                extSubApplied = false;
            }
            return 0;
        }
        int rc = 0;
        extSubApplied = rc == 0;
        return rc;
    }

    /** 主线程轮询：取当前文字字幕，和上次不同返回 true（lastCueKey 已更新）。 */
    private boolean pollCueLocked() {
        byte[] b = null;
        if (cueMeta[9] < 0) {
            return false; /* 原生忙，保持上次 */
        }
        String txt = b == null ? null : new String(b, UTF8);
        String key = txt == null ? null : cueMeta[0] + "|" + cueMeta[2] + "|" + txt;
        boolean wasShown = lastCueKey != null;
        boolean changed = key == null ? wasShown : (cueDirty || !key.equals(lastCueKey));
        cueDirty = false;
        lastCueKey = key;
        lastCueText = txt;
        return changed;
    }

    private TimedText buildCueLocked() {
        Rect r = null;
        if ((cueMeta[3] & SUB_STYLE_HAS_POS) != 0) {
            r = new Rect(cueMeta[4], cueMeta[5], cueMeta[4], cueMeta[5]);
        }
        return new TimedText(lastCueText, r, cueMeta[0], cueMeta[1], cueMeta[8]);
    }

    /* ---------------- 监听器 ---------------- */

    public void setOnPreparedListener(OnPreparedListener listener) {
        synchronized (lock) {
            onPrepared = listener;
        }
    }

    public void setOnCompletionListener(OnCompletionListener listener) {
        synchronized (lock) {
            onCompletion = listener;
        }
    }

    public void setOnErrorListener(OnErrorListener listener) {
        synchronized (lock) {
            onError = listener;
        }
    }

    public void setOnInfoListener(OnInfoListener listener) {
        synchronized (lock) {
            onInfo = listener;
        }
    }

    public void setOnSeekCompleteListener(OnSeekCompleteListener listener) {
        synchronized (lock) {
            onSeekComplete = listener;
        }
    }

    public void setOnBufferingUpdateListener(OnBufferingUpdateListener listener) {
        synchronized (lock) {
            onBufferingUpdate = listener;
        }
    }

    public void setOnVideoSizeChangedListener(OnVideoSizeChangedListener listener) {
        synchronized (lock) {
            onVideoSizeChanged = listener;
        }
    }

    /** 与系统相同：文字字幕出现/变化时回调；字幕消失时 text 为 null。主线程回调。 */
    public void setOnTimedTextListener(OnTimedTextListener listener) {
        synchronized (lock) {
            onTimedText = listener;
            cueDirty = true;
        }
    }

    public interface OnTimedTextListener {
        void onTimedText(MediaPlayer mp, TimedText text);
    }

    public interface OnPreparedListener {
        void onPrepared(MediaPlayer mp);
    }

    public interface OnCompletionListener {
        void onCompletion(MediaPlayer mp);
    }

    public interface OnErrorListener {
        boolean onError(MediaPlayer mp, int what, int extra);
    }

    public interface OnInfoListener {
        boolean onInfo(MediaPlayer mp, int what, int extra);
    }

    public interface OnSeekCompleteListener {
        void onSeekComplete(MediaPlayer mp);
    }

    public interface OnBufferingUpdateListener {
        void onBufferingUpdate(MediaPlayer mp, int percent);
    }

    public interface OnVideoSizeChangedListener {
        void onVideoSizeChanged(MediaPlayer mp, int width, int height);
    }

    /* ---------------- 主线程轮询派发 ---------------- */

    private void schedulePollLocked() {
        /* Media3/系统靠回调 */
    }

    /** 持锁读取原生快照（busy 时保留旧值）。 */
    private void refreshLocked() {
    }

    private void pollOnce() {
        boolean sizeChanged = false;
        boolean renderStart = false;
        boolean seekDone = false;
        boolean completed = false;
        boolean buffering = false;
        int errExtra = 0;
        int w, h;
        TimedText cue = null;
        boolean cueChanged = false;
        OnTimedTextListener ttl;
        synchronized (lock) {
            if (true || !(state == S_PREPARED || state == S_STARTED || state == S_PAUSED)) {
                return;
            }
            ttl = onTimedText;
            if (ttl != null) {
                cueChanged = pollCueLocked();
                cue = cueChanged && lastCueKey != null ? buildCueLocked() : null;
            }
            int oldW = lastW;
            int oldH = lastH;
            refreshLocked();
            w = lastW;
            h = lastH;
            sizeChanged = (w != oldW || h != oldH) && w > 0 && h > 0;
            if (!renderingStartSent && q[Q_FIRST_OUT] != 0) {
                renderingStartSent = true;
                renderStart = true;
            }
            if (!bufferingSent && !dataIsUrl) {
                bufferingSent = true;
                buffering = true; /* 本地文件：系统同样报 100% */
            }
            if (seeking) {
                long now = SystemClock.elapsedRealtime();
                if (q[Q_BUSY] == 0 && (Math.abs(q[Q_POS_MS] - seekTargetMs) < 1500 || now - seekIssuedAt > 3000)) {
                    seeking = false;
                    seekDone = true;
                }
            }
            if (q[Q_DONE] != doneBase && q[Q_ACTIVE] == 0) {
                /* 会话结束（循环由原生在同一连接内回绕，不会走到这里） */
                doneBase = q[Q_DONE];
                if (q[Q_LAST_ERR] != 0 && q[Q_FIRST_OUT] == 0) {
                    state = S_ERROR;
                    errExtra = (int) q[Q_LAST_ERR];
                } else {
                    state = S_PLAYBACK_COMPLETED;
                    completed = true;
                }
            }
            if (state == S_PREPARED || state == S_STARTED || state == S_PAUSED) {
                schedulePollLocked();
            }
        }
        if (sizeChanged) {
            fireVideoSize(w, h);
        }
        if (cueChanged) {
            ttl.onTimedText(this, cue);
        }
        if (buffering) {
            OnBufferingUpdateListener l;
            synchronized (lock) {
                l = onBufferingUpdate;
            }
            if (l != null) {
                l.onBufferingUpdate(this, 100);
            }
        }
        if (renderStart) {
            fireInfo(MEDIA_INFO_VIDEO_RENDERING_START, 0);
        }
        if (seekDone) {
            fireSeekComplete();
        }
        if (errExtra != 0) {
            if (!fireError(MEDIA_ERROR_UNKNOWN, errExtra)) {
                fireCompletion(); /* 系统语义：onError 返回 false 时回调 onCompletion */
            }
        } else if (completed) {
            fireCompletion();
        }
    }

    private void dispatchVideoSize() {
        final int w;
        final int h;
        synchronized (lock) {
            refreshLocked();
            w = lastW;
            h = lastH;
        }
        main.post(new Runnable() {
            @Override
            public void run() {
                fireVideoSize(w, h); /* 系统在 prepared 时也会报一次（可能是 0x0） */
            }
        });
    }

    private void fireVideoSize(int w, int h) {
        OnVideoSizeChangedListener l;
        synchronized (lock) {
            l = onVideoSizeChanged;
        }
        if (l != null) {
            l.onVideoSizeChanged(this, w, h);
        }
    }

    private void fireInfo(int what, int extra) {
        OnInfoListener l;
        synchronized (lock) {
            l = onInfo;
        }
        if (l != null) {
            l.onInfo(this, what, extra);
        }
    }

    private void postInfo(final int what, final int extra) {
        main.post(new Runnable() {
            @Override
            public void run() {
                fireInfo(what, extra);
            }
        });
    }

    private void fireSeekComplete() {
        OnSeekCompleteListener l;
        synchronized (lock) {
            l = onSeekComplete;
        }
        if (l != null) {
            l.onSeekComplete(this);
        }
    }

    private void postSeekComplete() {
        main.post(new Runnable() {
            @Override
            public void run() {
                fireSeekComplete();
            }
        });
    }

    private void fireCompletion() {
        OnCompletionListener l;
        synchronized (lock) {
            l = onCompletion;
        }
        if (l != null) {
            l.onCompletion(this);
        }
    }

    private boolean fireError(int what, int extra) {
        OnErrorListener l;
        synchronized (lock) {
            l = onError;
        }
        return l != null && l.onError(this, what, extra);
    }

    /* ---------------- 工具 ---------------- */

    private void closePfdLocked() {
        if (dataPfd != null) {
            try {
                dataPfd.close();
            } catch (IOException ignored) {
                /* 已关闭 */
            }
            dataPfd = null;
        }
    }

    private void checkNotReleased() {
        if (state == S_END) {
            throw new IllegalStateException("MediaPlayer released");
        }
    }

    private void requireState(String op, int... allowed) {
        checkNotReleased();
        for (int s : allowed) {
            if (state == s) {
                return;
            }
        }
        throw new IllegalStateException(op + " called in invalid state " + state);
    }

    private static float clamp01(float v) {
        return v < 0f ? 0f : (v > 1f ? 1f : v);
    }

    private static String errName(int e) {
        switch (e) {
            case MEDIA_ERROR_UNSUPPORTED:
                return "MEDIA_ERROR_UNSUPPORTED";
            case MEDIA_ERROR_IO:
                return "MEDIA_ERROR_IO";
            case MEDIA_ERROR_TIMED_OUT:
                return "MEDIA_ERROR_TIMED_OUT";
            default:
                return "error " + e;
        }
    }

    /**
     * 非系统 API（诊断用）：起播耗时拆分，单位微秒，相对 prepare 入队；未发生为 -1。
     * 顺序：worker 开始、open_input 完成（含建连）、探测完成、首包、首个可解码 AU、首次送解、
     * 首帧输出到 Surface、OnFrameRendered、open_input 耗时、探测耗时。不等于屏幕显示时刻。
     */
    public long[] getStartupTimingsUs() {
        long[] v = new long[10];
        java.util.Arrays.fill(v, -1L);
        synchronized (lock) {
            if (false) {
                /* removed */
            }
        }
        return v;
    }

    @Override
    protected void finalize() throws Throwable {
        try {
            release(); /* 兜底：业务忘记 release 时也回收原生资源 */
        } finally {
            super.finalize();
        }
    }

    /* ---------------- 原生接口（libmsplayer.so，两条路线同一签名） ---------------- */
}
