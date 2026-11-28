package com.mycjzb.media.player;

import com.mycjzb.media.player.MsNativeLoader;

/**
 * 剥 PNG/JPEG/GIF/BMP/WebP 伪装头，定位真正媒体起点（设计「其他」）。
 */
final class MsProbe {
    private static final boolean OK;

    static {
        boolean ok = false;
        try {
            ok = MsNativeLoader.ensure();
        } catch (Throwable ignored) {
        }
        OK = ok;
    }

    private MsProbe() {}

    /** ≥0 起点偏移；-1 还要更多字节；不可用时返回 0（原样）。 */
    static int mediaOffset(byte[] buf, int len, boolean eof) {
        if (!OK || buf == null || len <= 0) {
            return 0;
        }
        try {
            return nativeMediaOffset(buf, len, eof);
        } catch (Throwable t) {
            return 0;
        }
    }

    private static native int nativeMediaOffset(byte[] buf, int len, boolean eof);
}
