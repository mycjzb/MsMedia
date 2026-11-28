package com.mycjzb.media.player.n;

import com.mycjzb.media.player.MsNativeLoader;

/**
 * 短名：JS 规则脚本（QuickJS）。
 * e = main → Object[]{ kind, url, headers, body, contentType, stream, streamArgs }
 * s = stream_call(url, argsJson) → 字节（分片二次处理）。
 */
public final class b {
    public static final int KIND_URL = 0;
    public static final int KIND_BODY = 1;

    private static final boolean ok;

    static {
        boolean v = false;
        try {
            v = MsNativeLoader.ensure();
        } catch (Throwable ignored) {
        }
        ok = v;
    }

    private b() {}

    public static boolean r() {
        return ok;
    }

    /**
     * @return Object[]{ Integer kind, String url, String headers, byte[] body, String contentType,
     *     Integer stream, String streamArgs }；失败 null
     */
    public static native Object[] e(byte[] script, String itemUrl);

    /** 调脚本 {@code stream_call(url, args)}；失败 null */
    public static native byte[] s(byte[] script, String url, String argsJson);
}
