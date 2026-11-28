package com.mycjzb.media.player;

/**
 * TVBus / SOPCast 引擎（{@code libtvcore.so}）的加载入口。
 *
 * <p>不能直接 {@code System.loadLibrary("tvcore")}：
 * <ul>
 *   <li>tvcore 会校验宿主 App 签名，必须在它的 {@code JNI_OnLoad} 之前把
 *       {@code GOT[memcmp]} 换掉；</li>
 *   <li>{@code dlopen} 不会自动调 {@code JNI_OnLoad}，需要手动调用并
 *       {@code RegisterNatives} 到 {@code com.tvbus.engine.TVCore}；</li>
 *   <li>部分设备 / native bridge 上 {@code dlopen} 会 SIGBUS/SIGABRT，需软捕获。</li>
 * </ul>
 * 上述均在 {@code libmsplayer.so} 内完成，本类只转发路径。
 *
 * <p>用法：
 * <pre>{@code
 * String dir = context.getApplicationInfo().nativeLibraryDir;
 * boolean ok = MsTvbus.load(dir + "/libtvcore.so");
 * if (ok) { TVCore core = TVCore.create(); ... }
 * }</pre>
 */
public final class MsTvbus {

    private static volatile boolean sTried;
    private static volatile boolean sReady;

    private MsTvbus() {
    }

    static {
        MsNativeLoader.ensure();
    }

    /**
     * 加载并绑定 tvcore。幂等；重复调用返回首次结果。
     *
     * @param soPath {@code libtvcore.so} 的绝对路径
     * @return true = 引擎就绪，可以 {@code TVCore.create()}
     */
    public static boolean load(String soPath) {
        if (sReady) {
            return true;
        }
        synchronized (MsTvbus.class) {
            if (sReady) {
                return true;
            }
            if (soPath == null || soPath.trim().isEmpty()) {
                return false;
            }
            if (sTried && !sReady) {
                /* 失败不反复 dlopen：个别机上重复尝试会 SIGBUS 反复刷。 */
                return false;
            }
            sTried = true;
            try {
                sReady = nativeLoad(soPath.trim()) == 1;
            } catch (Throwable t) {
                sReady = false;
            }
            return sReady;
        }
    }

    /** 引擎是否已就绪（不触发加载）。 */
    public static boolean isReady() {
        if (sReady) {
            return true;
        }
        try {
            return nativeReady() == 1;
        } catch (Throwable t) {
            return false;
        }
    }

    /**
     * 本次构建的 so 是否带 tvbus 加载器（{@code Java_com_mycjzb_media_player_MsTvbus_*}）。
     *
     * <p>老版本 so 没有这两个符号，调 {@code nativeReady()} 会
     * {@code UnsatisfiedLinkError}，据此判定。
     */
    public static boolean isAvailable() {
        try {
            nativeReady();
            return true;
        } catch (Throwable t) {
            return false;
        }
    }

    private static native int nativeLoad(String soPath);

    private static native int nativeReady();
}
