package com.tvbus.engine;

import android.content.Context;

/**
 * {@code libtvcore.so}（TVBus / SOPCast 引擎）的 Java 绑定。
 *
 * <p>本类**不自己 load so**：由 {@link com.mycjzb.media.player.MsTvbus} 在
 * {@code libmsplayer.so} 内 dlopen + hook 签名校验 + {@code RegisterNatives}。
 * 因此类名/包名、方法签名、静态/实例修饰符都必须与那边注册的逐字一致。
 *
 * <p>生命周期由 {@link com.mycjzb.media.player.tvbus.TvbusSession} 管理，
 * 业务侧不直接使用本类。
 */
public final class TVCore {

    /** 引擎句柄；{@code -1} 表示未初始化。 */
    public static volatile long nativeHandle = -1L;

    private TVCore() {
    }

    /**
     * 取引擎实例。前提：{@code MsTvbus.load(...)} 已成功。
     *
     * @return null 表示引擎未就绪或 initialise 失败
     */
    public static TVCore create() {
        if (!com.mycjzb.media.player.MsTvbus.isReady()) {
            return null;
        }
        TVCore c = new TVCore();
        long h;
        try {
            h = c.initialise();
        } catch (Throwable t) {
            return null;
        }
        if (h == -1L) {
            return null;
        }
        nativeHandle = h;
        return c;
    }

    public boolean isReady() {
        return nativeHandle != -1L;
    }

    // ---- 配置（init/run 之前调用）----

    public native long initialise();

    public native void setListener(long h, TVListener l);

    public native void setPlayPort(long h, int port);

    public native void setServPort(long h, int port);

    public native void setInboundConns(long h, int n);

    public native void setTotalConns(long h, int n);

    public native void setRunningMode(long h, int mode);

    public native void setDomainSuffix(long h, String suffix);

    public native void setOption(long h, String key, String val);

    public native void setAuthUrl(long h, String url);

    public native void setUsername(long h, String name);

    public native void setPassword(long h, String pass);

    public native void setMKBroker(long h, String broker);

    // ---- 生命周期 ----

    public native int init(long h, Context ctx);

    /** 阻塞：引擎主循环。必须跑在独立线程。 */
    public native int run(long h);

    public native void start(long h, String url);

    public native String description(long h);

    public native void diagnose(long h);

    public native String getVersion(long h);

    public native String err2String(long h, int code);

    public native int getPlayPort(long h);

    public native String getSockPath(long h);

    public static native void stop(long h);

    public static native void stop2(long h, int code);

    public static native void quit(long h);

    // ---- 带空值保护的便利方法 ----

    public void play(String uri) {
        if (nativeHandle != -1L && uri != null && !uri.isEmpty()) {
            try {
                start(nativeHandle, uri);
            } catch (Throwable ignored) {
            }
        }
    }

    public void stopPlay() {
        if (nativeHandle != -1L) {
            try {
                stop(nativeHandle);
            } catch (Throwable ignored) {
            }
        }
    }
}
