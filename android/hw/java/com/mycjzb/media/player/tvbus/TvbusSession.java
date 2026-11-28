package com.mycjzb.media.player.tvbus;

import android.content.Context;
import android.text.TextUtils;

import com.mycjzb.media.player.MsTvbus;
import com.tvbus.engine.TVCore;
import com.tvbus.engine.TVListener;

import org.json.JSONObject;

import java.util.Locale;

/**
 * TVBus / SOPCast 会话：把 {@code sop://} / {@code tvbus://} 变成
 * 本机 {@code http://127.0.0.1:<port>/...}，再交给正常播放链路。
 *
 * <p>生命周期由 {@code MediaPlayer} 托管（与 rtmp 等协议一致）：
 * prepare 前 {@link #resolve}，release / reset / 换台时 {@link #stop}。
 * 业务侧不需要感知本类。
 *
 * <p>引擎本体（{@code libtvcore.so}）由 {@link MsTvbus} 在 {@code libmsplayer.so}
 * 内 dlopen + 绕过签名校验 + {@code RegisterNatives}。
 */
public final class TvbusSession implements TVListener {

    private static final int WAIT_INIT_MS = 20_000;
    private static final int WAIT_PREP_MS = 20_000;
    private static final int BASE_PLAY_PORT = 7892;
    private static final String TAG = "MsTvbus";

    private static volatile TvbusSession sInst;
    private static volatile boolean sLoadTried;
    private static volatile boolean sSupported;

    private final TVCore core;
    private final Object lock = new Object();
    private volatile boolean inited;
    private volatile boolean prepared;
    private volatile String preparedHls;
    private volatile String preparedHttp;

    private TvbusSession(TVCore core) {
        this.core = core;
    }

    /** 当前 so 是否带 tvbus 加载器（旧 AAR 没有）。 */
    public static boolean isAvailable() {
        return MsTvbus.isAvailable();
    }

    /** 是否为本模块负责的地址。 */
    public static boolean isTvbusUrl(String url) {
        if (TextUtils.isEmpty(url)) {
            return false;
        }
        String lower = url.trim().toLowerCase(Locale.US);
        return lower.startsWith("sop://") || lower.startsWith("tvbus://");
    }

    /**
     * 把 {@code sop://} / {@code tvbus://} 解析成本机可播地址。
     *
     * @param ctx 任一带 nativeLibraryDir 的 Context（取 App 的即可）
     * @return 本地 http 地址；不是 tvbus 地址或失败返回 null
     */
    public static String resolve(Context ctx, String url) {
        if (!isTvbusUrl(url)) {
            return null;
        }
        if (ctx == null) {
            return null;
        }
        String sop = url.trim();
        if (sop.toLowerCase(Locale.US).startsWith("tvbus://")) {
            String decoded = TvbUrl.decode(sop);
            if (TextUtils.isEmpty(decoded)) {
                return null;
            }
            sop = decoded;
        }
        if (!sop.toLowerCase(Locale.US).startsWith("sop://")) {
            return null;
        }
        TvbusSession s = get(ctx);
        if (s == null) {
            return null;
        }
        return s.playUrl(sop);
    }

    /** 换台 / 停止：收掉当前 P2P 会话（对齐 rtmp 的 stop 语义）。 */
    public static void stop() {
        TvbusSession s = sInst;
        if (s != null) {
            s.stopPlay();
        }
    }

    private static TvbusSession get(Context ctx) {
        if (sInst != null) {
            return sInst;
        }
        synchronized (TvbusSession.class) {
            if (sInst != null) {
                return sInst;
            }
            if (sLoadTried && !sSupported) {
                return null;
            }
            Context app = ctx.getApplicationContext() != null
                    ? ctx.getApplicationContext() : ctx;
            if (!sLoadTried) {
                sLoadTried = true;
                sSupported = loadEngine(app);
            }
            if (!sSupported) {
                return null;
            }
            TvbusSession created = createSession(app);
            sInst = created;
            return created;
        }
    }

    /**
     * 加载 libtvcore.so 并完成 JNI 绑定。
     *
     * <p>必须在 {@code nativeLibraryDir} 下存在**实体文件**：{@code ms_tvb.c} 需要
     * 文件路径 {@code dlopen} 才能在 {@code JNI_OnLoad} 之前 patch 掉签名校验的
     * {@code GOT[memcmp]}。所以宿主 App 必须 {@code extractNativeLibs=true}
     * （AGP: {@code packagingOptions { jniLibs { useLegacyPackaging true } }}）。
     * 若 so 只以未解压形式躺在 APK 里，这里会明确报出来，而不是静默不可用。
     */
    private static boolean loadEngine(Context app) {
        String path = null;
        try {
            path = app.getApplicationInfo().nativeLibraryDir + "/libtvcore.so";
        } catch (Throwable t) {
            android.util.Log.w(TAG, "cannot resolve nativeLibraryDir", t);
            return false;
        }
        if (!MsTvbus.isAvailable()) {
            /* 老版本 libmsplayer.so 没有 MsTvbus JNI：SDK 未更新，不是配置问题。 */
            android.util.Log.i(TAG, "libmsplayer.so has no MsTvbus JNI; sop:// disabled");
            return false;
        }
        if (!new java.io.File(path).isFile()) {
            android.util.Log.w(TAG, "libtvcore.so missing at " + path
                    + " — host must set extractNativeLibs/useLegacyPackaging=true");
            return false;
        }
        boolean ok;
        try {
            ok = MsTvbus.load(path);
        } catch (Throwable t) {
            android.util.Log.w(TAG, "MsTvbus.load failed: " + path, t);
            ok = false;
        }
        if (ok) {
            android.util.Log.i(TAG, "tvbus engine ready: " + path);
        } else {
            android.util.Log.w(TAG, "tvbus engine not ready (hook/RegisterNatives failed): " + path);
        }
        return ok;
    }

    private static TvbusSession createSession(Context app) {
        TVCore core = TVCore.create();
        if (core == null) {
            return null;
        }
        final long h = TVCore.nativeHandle;
        final TvbusSession session = new TvbusSession(core);
        try {
            core.setListener(h, session);
        } catch (Throwable t) {
            return null;
        }
        final int port = pickPort(BASE_PLAY_PORT);
        Thread t = new Thread(() -> {
            try {
                core.setPlayPort(h, port);
                core.setServPort(h, 0);
                core.setRunningMode(h, 1);
                int r = core.init(h, app);
                if (r == 0) {
                    core.run(h); /* 阻塞 */
                }
            } catch (Throwable ignored) {
            }
        }, "tvbus");
        t.setDaemon(true);
        t.start();
        return session;
    }

    private static int pickPort(int base) {
        for (int p = base; p < 65000; p++) {
            java.net.ServerSocket ss = null;
            try {
                ss = new java.net.ServerSocket(p);
                return p;
            } catch (Throwable ignored) {
                /* 被占，下一个 */
            } finally {
                if (ss != null) {
                    try {
                        ss.close();
                    } catch (Throwable ignored) {
                    }
                }
            }
        }
        return base;
    }

    /** 起播并等本地地址，优先 hls，超时退 mpegts（对齐 HDLibrary / 超级直播）。 */
    String playUrl(String sop) {
        if (core == null || TextUtils.isEmpty(sop)) {
            return null;
        }
        if (!waitInited()) {
            return null;
        }
        final long h = TVCore.nativeHandle;
        synchronized (lock) {
            prepared = false;
            preparedHls = null;
            preparedHttp = null;
        }
        /* 先停再起：清掉上一路 peer / 缓冲，防并发叠会话。 */
        try {
            TVCore.stop(h);
        } catch (Throwable ignored) {
        }
        try {
            core.start(h, sop);
        } catch (Throwable t) {
            return null;
        }
        return waitPrepared();
    }

    void stopPlay() {
        if (core == null) {
            return;
        }
        synchronized (lock) {
            prepared = false;
            preparedHls = null;
            preparedHttp = null;
        }
        core.stopPlay();
    }

    private boolean waitInited() {
        long end = System.currentTimeMillis() + WAIT_INIT_MS;
        synchronized (lock) {
            while (!inited && System.currentTimeMillis() < end) {
                try {
                    lock.wait(200);
                } catch (InterruptedException e) {
                    Thread.currentThread().interrupt();
                    return false;
                }
            }
            return inited;
        }
    }

    private String waitPrepared() {
        long end = System.currentTimeMillis() + WAIT_PREP_MS;
        synchronized (lock) {
            while (!prepared && System.currentTimeMillis() < end) {
                try {
                    lock.wait(200);
                } catch (InterruptedException e) {
                    Thread.currentThread().interrupt();
                    return null;
                }
            }
            if (!TextUtils.isEmpty(preparedHls)) {
                return preparedHls;
            }
            return TextUtils.isEmpty(preparedHttp) ? null : preparedHttp;
        }
    }

    // ---- TVListener（libtvcore 回调）----

    @Override
    public void onInited(String result) {
        if (TextUtils.isEmpty(result)) {
            return;
        }
        try {
            JSONObject o = new JSONObject(result.trim());
            if ("0".equals(o.optString("tvcore"))) {
                synchronized (lock) {
                    inited = true;
                    lock.notifyAll();
                }
            }
        } catch (Throwable ignored) {
        }
    }

    @Override
    public void onStart(String result) {
    }

    @Override
    public void onPrepared(String result) {
        if (TextUtils.isEmpty(result)) {
            return;
        }
        try {
            JSONObject o = new JSONObject(result.trim());
            String hls = o.optString("hls");
            String http = o.optString("http");
            synchronized (lock) {
                if (!TextUtils.isEmpty(http)) {
                    preparedHttp = http;
                }
                if (!TextUtils.isEmpty(hls)) {
                    preparedHls = hls;
                    prepared = true;
                    lock.notifyAll();
                }
            }
        } catch (Throwable ignored) {
        }
    }

    @Override
    public void onInfo(String result) {
    }

    @Override
    public void onStop(String result) {
    }

    @Override
    public void onQuit(String result) {
    }
}
