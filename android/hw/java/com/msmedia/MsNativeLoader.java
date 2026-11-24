/*
 * 统一加载合并后的 libmsplayer.so（路线 A 纯自研 / 路线 B 精简 FFmpeg 同名，按 APK 打包的变体区分），
 * 并在 Java 线程上缓存原生工作线程需要的应用类（系统 TLS / DNS 回调）。只加载一次。
 * by: Long <mycjzb@gmail.com>
 */
package com.msmedia;

public final class MsNativeLoader {
    private static boolean tried;
    private static boolean loaded;

    private MsNativeLoader() {
    }

    public static synchronized boolean ensure() {
        if (tried) {
            return loaded;
        }
        tried = true;
        try {
            System.loadLibrary("msplayer");
            loaded = true;
        } catch (UnsatisfiedLinkError e) {
            loaded = false;
            return false;
        }
        try {
            // 触发 MsHttpClient 类初始化并缓存 MsSystemTls / DNS 回调类；失败不影响本地播放。
            com.msmedia.net.MsHttpClient.initFromLoader();
        } catch (Throwable ignored) {
        }
        return loaded;
    }
}
