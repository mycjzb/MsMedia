/*
 * 与系统 android.media.MediaPlayer 对齐的公开封装：同名方法 / 重载 / 常量 / 监听器，
 * 便于业务把 import android.media.MediaPlayer 换成 com.msmedia.MediaPlayer。
 * 底层只加载合并后的单个 libmsplayer.so（路线 A 纯自研 / 路线 B 精简 FFmpeg 产物同名，本类共用）。
 * 阶段 2：已接原生通道（prepare/start/pause/stop/reset/release/seek/位置/时长/音量/循环/
 * Surface/倍速/七个监听器）。对 Java 一律毫秒，90 kHz 只在原生层换算。
 * 不能播放的输入（例如路线 A 的网络地址、带偏移的 fd、需要自定义请求头的地址）prepare 诚实失败。
 * 不记录完整私有 URL。不是设备出画/出声验收。
 * by: Long <mycjzb@gmail.com>
 */
package com.msmedia;

import android.content.ContentResolver;
import android.content.Context;
import android.content.res.AssetFileDescriptor;
import android.content.Intent;
import android.content.IntentFilter;
import android.media.AudioAttributes;
import android.media.AudioFormat;
import android.media.AudioManager;
import android.media.AudioTrack;
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

import java.io.FileDescriptor;
import java.io.IOException;
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
    private final long[] q = new long[Q_LEN];
    private long nh; /* 原生句柄；0=未建/已释放 */
    private int state = S_IDLE;

    private String dataSource;          /* 仅内部，不写入日志 */
    private boolean dataIsUrl;
    private ParcelFileDescriptor dataPfd; /* fd 数据源：dup 后持有，reset/release 关闭 */
    private boolean headersUnsupported; /* 非空请求头：原生取流通道尚无请求头参数，prepare 诚实失败 */

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

    private final Runnable poller = new Runnable() {
        @Override
        public void run() {
            pollOnce();
        }
    };

    public MediaPlayer() {
        if (MsNativeLoader.ensure()) {
            try {
                nh = nativeSetup();
            } catch (UnsatisfiedLinkError e) {
                nh = 0;
            }
        }
    }

    /** 本 so 是否链入了精简 FFmpeg（路线 B）；路线 A 返回 false（网络地址 prepare 会诚实失败）。 */
    public static boolean isFfmpegRoute() {
        if (!MsNativeLoader.ensure()) {
            return false;
        }
        try {
            return nativeHasFf();
        } catch (UnsatisfiedLinkError e) {
            return false;
        }
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

    /* ---------------- 数据源 ---------------- */

    public void setDataSource(String path) throws IOException {
        setDataSource(path, (Map<String, String>) null);
    }

    public void setDataSource(String path, Map<String, String> headers) throws IOException {
        if (path == null) {
            throw new IllegalArgumentException("path is null");
        }
        Uri u = Uri.parse(path);
        String scheme = u.getScheme();
        if (scheme == null || "file".equalsIgnoreCase(scheme)) {
            setSourceInternal(scheme == null ? path : u.getPath(), false, headers);
        } else {
            setSourceInternal(path, true, headers);
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
        String scheme = uri.getScheme();
        if (scheme == null || "file".equalsIgnoreCase(scheme)) {
            setSourceInternal(uri.getPath(), false, headers);
            return;
        }
        if (ContentResolver.SCHEME_CONTENT.equals(scheme) || ContentResolver.SCHEME_ANDROID_RESOURCE.equals(scheme)) {
            AssetFileDescriptor afd = context.getContentResolver().openAssetFileDescriptor(uri, "r");
            if (afd == null) {
                throw new IOException("cannot open content uri");
            }
            try {
                setDataSource(afd.getFileDescriptor(), afd.getStartOffset(), afd.getDeclaredLength());
            } finally {
                afd.close();
            }
            return;
        }
        setSourceInternal(uri.toString(), true, headers);
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
            ParcelFileDescriptor pfd = ParcelFileDescriptor.dup(fd);
            closePfdLocked();
            dataPfd = pfd;
            dataSource = offset == 0 ? "/proc/self/fd/" + pfd.getFd() : null;
            dataIsUrl = false;
            headersUnsupported = false;
            state = S_INITIALIZED;
        }
    }

    private void setSourceInternal(String loc, boolean isUrl, Map<String, String> headers) {
        synchronized (lock) {
            requireState("setDataSource", S_IDLE);
            if (loc == null || loc.length() == 0) {
                throw new IllegalArgumentException("empty data source");
            }
            closePfdLocked();
            dataSource = loc;
            dataIsUrl = isUrl;
            headersUnsupported = headers != null && !headers.isEmpty();
            state = S_INITIALIZED;
        }
    }

    /* ---------------- prepare ---------------- */

    public void prepare() throws IOException {
        int gen;
        synchronized (lock) {
            requireState("prepare", S_INITIALIZED, S_STOPPED);
            state = S_PREPARING;
            gen = ++prepareGen;
        }
        int err = openAndWait(gen, DEFAULT_PREPARE_TIMEOUT_MS);
        synchronized (lock) {
            if (gen != prepareGen || state == S_END) {
                throw new IOException("prepare interrupted by reset/release");
            }
            if (err != 0) {
                state = S_ERROR;
                throw new IOException("prepare failed: " + errName(err));
            }
            onPreparedLocked();
        }
        dispatchVideoSize();
        /* 与系统一致：同步 prepare 成功后同样在主线程回调 onPrepared。 */
        final int g = gen;
        main.post(new Runnable() {
            @Override
            public void run() {
                OnPreparedListener pl;
                synchronized (lock) {
                    if (g != prepareGen || state == S_END) {
                        return;
                    }
                    pl = onPrepared;
                }
                if (pl != null) {
                    pl.onPrepared(MediaPlayer.this);
                }
            }
        });
    }

    public void prepareAsync() {
        final int gen;
        synchronized (lock) {
            requireState("prepareAsync", S_INITIALIZED, S_STOPPED);
            state = S_PREPARING;
            gen = ++prepareGen;
        }
        Thread t = new Thread(new Runnable() {
            @Override
            public void run() {
                final int err = openAndWait(gen, DEFAULT_PREPARE_TIMEOUT_MS);
                main.post(new Runnable() {
                    @Override
                    public void run() {
                        OnPreparedListener pl = null;
                        synchronized (lock) {
                            if (gen != prepareGen || state != S_PREPARING) {
                                return; /* 已被 reset/release，丢弃 */
                            }
                            if (err != 0) {
                                state = S_ERROR;
                            } else {
                                onPreparedLocked();
                                pl = onPrepared;
                            }
                        }
                        if (err != 0) {
                            fireError(MEDIA_ERROR_UNKNOWN, err);
                            return;
                        }
                        dispatchVideoSize();
                        if (pl != null) {
                            pl.onPrepared(MediaPlayer.this);
                        }
                    }
                });
            }
        }, "msplayer-prepare");
        t.start();
    }

    /** 0=已打开；否则返回系统 MEDIA_ERROR_* extra（-1010/-1004/-110）。不持锁阻塞。 */
    private int openAndWait(int gen, int timeoutMs) {
        String loc;
        boolean isUrl;
        long h;
        Surface s;
        synchronized (lock) {
            loc = dataSource;
            isUrl = dataIsUrl;
            h = nh;
            s = surface;
            if (h == 0) {
                return MEDIA_ERROR_UNSUPPORTED; /* libmsplayer.so 未加载 */
            }
            if (loc == null || headersUnsupported) {
                return MEDIA_ERROR_UNSUPPORTED;
            }
        }
        long deadline = SystemClock.elapsedRealtime() + timeoutMs;
        long[] v = new long[Q_LEN];
        /* 上一会话（stop/reset 后）还在收尾时先等它结束，避免把旧会话的结束误当成本次失败。 */
        while (true) {
            nativeQuery(h, v);
            if (v[Q_ACTIVE] == 0 && v[Q_PENDING] == 0) {
                break;
            }
            if (SystemClock.elapsedRealtime() > deadline || isStale(gen)) {
                return MEDIA_ERROR_TIMED_OUT;
            }
            nativeStop(h);
            SystemClock.sleep(10);
        }
        if (s != null) {
            nativeSetSurface(h, s);
        }
        long base = v[Q_DONE];
        int pa;
        long pm;
        synchronized (lock) {
            pa = preloadAheadMs;
            pm = preloadMaxBytes;
        }
        nativeSetPreload(h, pa, pm);
        boolean bs;
        int b0, b1, b2, b3;
        long b4;
        boolean tv;
        synchronized (lock) {
            bs = bufSet;
            b0 = bufMinMs;
            b1 = bufMaxMs;
            b2 = bufPlayMs;
            b3 = bufRebufMs;
            b4 = bufMaxBytes;
            tv = playbackTlsVerify;
        }
        if (bs) {
            nativeSetBufferConfig(h, b0, b1, b2, b3, b4);
        }
        nativeSetTlsVerify(h, tv);
        boolean pt;
        Context ctx;
        synchronized (lock) {
            pt = passthroughEnabled;
            ctx = appContext;
        }
        /* 每次 prepare 重新探测：HDMI 功放可能热插拔。不支持则下发 0，C 层走软解。 */
        nativeSetAudioPassthrough(h, pt ? getAudioPassthroughCaps(ctx) : 0);
        int rc = nativePrepare(h, loc, isUrl, timeoutMs);
        if (rc != 0) {
            return rc;
        }
        while (true) {
            if (isStale(gen)) {
                return MEDIA_ERROR_IO;
            }
            nativeQuery(h, v);
            if (v[Q_OPENED] != 0) {
                synchronized (lock) {
                    doneBase = base;
                }
                nativeSetVolume(h, volLeft, volRight);
                nativeSetLooping(h, looping);
                if (speed != 1f) {
                    nativeSetSpeed(h, speed);
                }
                return 0;
            }
            if (v[Q_DONE] != base) {
                /* 会话结束但从未打开：本变体打不开/不支持该输入 */
                if (isUrl && !nativeHasFf()) {
                    return MEDIA_ERROR_UNSUPPORTED;
                }
                return v[Q_LAST_ERR] != 0 ? MEDIA_ERROR_UNSUPPORTED : MEDIA_ERROR_IO;
            }
            if (SystemClock.elapsedRealtime() > deadline) {
                nativeStop(h);
                return MEDIA_ERROR_TIMED_OUT;
            }
            SystemClock.sleep(PREPARE_POLL_MS);
        }
    }

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
        schedulePollLocked();
    }

    /* ---------------- 播放控制 ---------------- */

    public void start() {
        synchronized (lock) {
            requireState("start", S_PREPARED, S_STARTED, S_PAUSED, S_PLAYBACK_COMPLETED);
            if (state == S_PLAYBACK_COMPLETED) {
                /* 系统语义：完成后 start 从头播。原会话已结束，同源重新排队（本地文件/可重开地址）。 */
                restartFromBeginningLocked();
                return;
            }
            nativeStart(nh);
            state = S_STARTED;
            schedulePollLocked();
        }
    }

    private void restartFromBeginningLocked() {
        final int gen = ++prepareGen;
        state = S_STARTED;
        renderingStartSent = false;
        final long h = nh;
        Thread t = new Thread(new Runnable() {
            @Override
            public void run() {
                final int err = openAndWait(gen, DEFAULT_PREPARE_TIMEOUT_MS);
                main.post(new Runnable() {
                    @Override
                    public void run() {
                        synchronized (lock) {
                            if (gen != prepareGen || state != S_STARTED) {
                                return;
                            }
                            if (err != 0) {
                                state = S_ERROR;
                            } else {
                                nativeStart(h);
                                schedulePollLocked();
                            }
                        }
                        if (err != 0) {
                            fireError(MEDIA_ERROR_UNKNOWN, err);
                        }
                    }
                });
            }
        }, "msplayer-restart");
        t.start();
    }

    public void pause() {
        synchronized (lock) {
            requireState("pause", S_STARTED, S_PAUSED, S_PLAYBACK_COMPLETED);
            if (state == S_STARTED) {
                nativePause(nh);
                state = S_PAUSED;
            }
        }
    }

    public void stop() {
        synchronized (lock) {
            requireState("stop", S_PREPARED, S_STARTED, S_PAUSED, S_STOPPED, S_PLAYBACK_COMPLETED);
            prepareGen++;
            nativeStop(nh);
            state = S_STOPPED;
            main.removeCallbacks(poller);
        }
    }

    public void reset() {
        synchronized (lock) {
            checkNotReleased();
            prepareGen++;
            if (nh != 0) {
                nativeStop(nh);
            }
            main.removeCallbacks(poller);
            closePfdLocked();
            dataSource = null;
            dataIsUrl = false;
            headersUnsupported = false;
            seeking = false;
            lastDurMs = -1;
            lastPosMs = 0;
            state = S_IDLE;
        }
    }

    public void release() {
        long h;
        synchronized (lock) {
            if (state == S_END) {
                return;
            }
            prepareGen++;
            main.removeCallbacksAndMessages(null);
            closePfdLocked();
            h = nh;
            nh = 0;
            state = S_END;
            onPrepared = null;
            onCompletion = null;
            onError = null;
            onInfo = null;
            onSeekComplete = null;
            onBufferingUpdate = null;
            onVideoSizeChanged = null;
        }
        if (h != 0) {
            nativeStop(h);
            nativeRelease(h); /* 原生侧 join/回收 worker，不泄漏 */
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
            int rc = nativeSeekTo(nh, msec);
            notSeekable = rc == MEDIA_ERROR_UNSUPPORTED;
            if (!notSeekable) {
                seeking = true;
                seekTargetMs = msec;
                seekIssuedAt = SystemClock.elapsedRealtime();
                lastPosMs = msec;
                schedulePollLocked();
            }
        }
        if (notSeekable) {
            postInfo(MEDIA_INFO_NOT_SEEKABLE, 0);
            postSeekComplete();
        }
    }

    public int getCurrentPosition() {
        synchronized (lock) {
            if (nh == 0 || state == S_IDLE || state == S_INITIALIZED || state == S_ERROR || state == S_END) {
                return 0;
            }
            if (seeking) {
                return (int) seekTargetMs;
            }
            refreshLocked();
            return (int) lastPosMs;
        }
    }

    /** 毫秒；未知（直播或路线 A 会话未给出时长）返回 -1，与系统一致。 */
    public int getDuration() {
        synchronized (lock) {
            if (nh == 0 || state == S_IDLE || state == S_INITIALIZED || state == S_ERROR || state == S_END) {
                return -1;
            }
            refreshLocked();
            return lastDurMs;
        }
    }

    public boolean isPlaying() {
        synchronized (lock) {
            return state == S_STARTED;
        }
    }

    public int getVideoWidth() {
        synchronized (lock) {
            if (nh != 0 && state != S_END) {
                refreshLocked();
            }
            return lastW;
        }
    }

    public int getVideoHeight() {
        synchronized (lock) {
            if (nh != 0 && state != S_END) {
                refreshLocked();
            }
            return lastH;
        }
    }

    public void setVolume(float leftVolume, float rightVolume) {
        synchronized (lock) {
            checkNotReleased();
            volLeft = clamp01(leftVolume);
            volRight = clamp01(rightVolume);
            if (nh != 0) {
                nativeSetVolume(nh, volLeft, volRight);
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
            if (nh != 0) {
                nativeSetLooping(nh, loop);
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
        long h;
        synchronized (lock) {
            h = nh;
        }
        return h != 0 ? nativeGetAudioPassthroughRoute(h) : 0;
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
            return looping;
        }
    }

    public void setDisplay(SurfaceHolder sh) {
        synchronized (lock) {
            checkNotReleased();
            display = sh;
            surface = sh != null ? sh.getSurface() : null;
            if (nh != 0) {
                nativeSetSurface(nh, surface);
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
            if (nh != 0) {
                nativeSetSurface(nh, s);
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
                    nativePause(nh);
                    state = S_PAUSED;
                }
                return;
            }
            if (!(sp > 0f) || sp > 8f) {
                throw new IllegalArgumentException("speed out of range");
            }
            speed = sp;
            if (nh != 0 && (state == S_PREPARED || state == S_STARTED || state == S_PAUSED)) {
                int st = nativeSetSpeed(nh, sp);
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
        main.removeCallbacks(poller);
        main.postDelayed(poller, (!renderingStartSent && state == S_STARTED) ? POLL_FAST_MS : POLL_MS);
    }

    /** 持锁读取原生快照（busy 时保留旧值）。 */
    private void refreshLocked() {
        if (nh == 0) {
            return;
        }
        nativeQuery(nh, q);
        if (q[Q_BUSY] == 0) {
            lastPosMs = q[Q_POS_MS];
            lastDurMs = q[Q_DUR_MS] > Integer.MAX_VALUE ? Integer.MAX_VALUE : (int) q[Q_DUR_MS];
            if (q[Q_W] > 0 && q[Q_H] > 0) {
                lastW = (int) q[Q_W];
                lastH = (int) q[Q_H];
            }
        }
    }

    private void pollOnce() {
        boolean sizeChanged = false;
        boolean renderStart = false;
        boolean seekDone = false;
        boolean completed = false;
        boolean buffering = false;
        int errExtra = 0;
        int w, h;
        synchronized (lock) {
            if (nh == 0 || !(state == S_PREPARED || state == S_STARTED || state == S_PAUSED)) {
                return;
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
            if (nh != 0) {
                nativeStartupStats(nh, v);
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
    private static native long nativeSetup();
    private static native void nativeRelease(long h);
    private static native void nativeSetSurface(long h, Surface s);
    private static native boolean nativeHasFf();
    private static native int nativePrepare(long h, String loc, boolean isUrl, int timeoutMs);
    private static native void nativeQuery(long h, long[] out);
    private static native void nativeStart(long h);
    private static native void nativePause(long h);
    private static native void nativeStop(long h);
    private static native int nativeSeekTo(long h, long msec);
    private static native void nativeSetVolume(long h, float l, float r);
    private static native void nativeSetLooping(long h, boolean on);
    private static native int nativeSetSpeed(long h, float speed);
    private static native void nativeStartupStats(long h, long[] out);
    private static native void nativeSetPreload(long h, int aheadMs, long maxBytes);
    private static native void nativeSetBufferConfig(long h, int minMs, int maxMs, int playMs, int rebufMs, long maxBytes);
    private static native void nativeSetTlsVerify(long h, boolean strict);
    private static native void nativeSetAudioPassthrough(long h, int caps);
    private static native int nativeGetAudioPassthroughRoute(long h);
}
