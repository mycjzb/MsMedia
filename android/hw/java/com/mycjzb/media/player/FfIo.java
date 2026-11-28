package com.mycjzb.media.player;

import com.mycjzb.media.player.MsNativeLoader;

/** FFmpeg AVIO JNI（libmsplayer.so）。非 HTTP 协议 DataSource 经这里读字节。 */
final class FfIo {
    static {
        if (!MsNativeLoader.ensure()) {
            throw new UnsatisfiedLinkError("libmsplayer.so");
        }
    }

    static native long nativeOpen(String url, int timeoutMs);
    static native long nativeOpenEx(String url, int timeoutMs, String headers);
    static native void nativeClose(long h);
    static native int nativeRead(long h, byte[] buf, int off, int len);
    static native long nativeSeek(long h, long off, int whence);
    static native long nativeSize(long h);
    static native String nativeLastErr(long h);

    private FfIo() {}
}
