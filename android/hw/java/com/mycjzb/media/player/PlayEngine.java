package com.mycjzb.media.player;

/**
 * 播放引擎选择（对齐超级直播目录前缀习惯）。
 * <ul>
 *   <li>{@link #SYSTEM} / 前缀 {@code sys:} → 系统 {@code android.media.MediaPlayer}</li>
 *   <li>{@link #SHW} / 前缀 {@code shw:} → Media3 源码内核（{@code M3PlayerBackend}）</li>
 * </ul>
 * 源级前缀只影响这一条地址；{@link #setDefault(int)} 改无前缀时的默认引擎。
 * 容忍 {@code sys://http://…}：剥前缀后再剥紧随的 {@code //}。
 */
public final class PlayEngine {
    /** 系统播放器。 */
    public static final int SYSTEM = 0;
    /** 自研引擎（SDK 默认）。 */
    public static final int SHW = 1;

    private static final String P_SYS = "sys:";
    private static final String P_SHW = "shw:";

    private static volatile int sDefault = SHW;

    private PlayEngine() {}

    /** 全局默认引擎；非法值忽略。只影响之后的 {@code setDataSource}，不回溯已打开实例。 */
    public static void setDefault(int engine) {
        if (engine == SYSTEM || engine == SHW) {
            sDefault = engine;
        }
    }

    public static int getDefault() {
        return sDefault;
    }

    /** 源上强制引擎；无前缀返回 -1。 */
    public static int forced(String url) {
        int n = prefixLen(url);
        if (n < 0) return -1;
        if (url.regionMatches(true, 0, P_SHW, 0, 4)) return SHW;
        return SYSTEM;
    }

    /** 最终引擎：有前缀用前缀，否则用默认。 */
    public static int resolve(String url) {
        int f = forced(url);
        return f >= 0 ? f : sDefault;
    }

    /** 剥解码前缀（及紧随 {@code //}）；无前缀原样返回。 */
    public static String strip(String url) {
        if (url == null) return "";
        int n = prefixLen(url);
        if (n < 0) return url;
        String rest = url.substring(n);
        if (rest.startsWith("//")) rest = rest.substring(2);
        return rest;
    }

    /** 无前缀时贴上；已有前缀或空串原样。 */
    public static String with(int engine, String url) {
        if (url == null || url.length() == 0) return url;
        if (forced(url) >= 0) return url;
        if (engine == SYSTEM) return P_SYS + url;
        if (engine == SHW) return P_SHW + url;
        return url;
    }

    public static String name(int engine) {
        if (engine == SYSTEM) return "sys";
        if (engine == SHW) return "shw";
        return "unknown";
    }

    /** {@code sys:} / {@code shw:} 均为 4；无则 -1。 */
    static int prefixLen(String url) {
        if (url == null || url.length() < 4) return -1;
        if (url.regionMatches(true, 0, P_SHW, 0, 4)) return 4;
        if (url.regionMatches(true, 0, P_SYS, 0, 4)) return 4;
        return -1;
    }
}
