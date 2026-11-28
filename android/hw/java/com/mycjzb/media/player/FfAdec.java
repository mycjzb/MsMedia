package com.mycjzb.media.player;

import com.mycjzb.media.player.MsNativeLoader;

import java.nio.ByteBuffer;

/**
 * Media3 音频软解 JNI：复用 so 内 {@code ms_ffadec}。
 * codec 与 {@code ms_codec} 枚举数值一致。
 */
public final class FfAdec {
    public static final int CODEC_AAC = 2;
    public static final int CODEC_AC3 = 10;
    public static final int CODEC_EAC3 = 11;
    public static final int CODEC_MP3 = 13;
    public static final int CODEC_MP2 = 14;
    public static final int CODEC_OPUS = 15;
    public static final int CODEC_G711_ALAW = 16;
    public static final int CODEC_G711_ULAW = 17;
    public static final int CODEC_FLAC = 27;
    public static final int CODEC_PCM = 28;
    public static final int CODEC_DTS = 29;
    public static final int CODEC_AAC_LATM = 30;
    public static final int CODEC_DRA = 31;
    public static final int CODEC_VORBIS = 32;
    public static final int CODEC_ALAC = 33;

    private static final boolean LOADED;

    static {
        boolean ok = false;
        try {
            ok = MsNativeLoader.ensure() && nativeAvailable();
        } catch (Throwable ignored) {
        }
        LOADED = ok;
    }

    private FfAdec() {}

    public static boolean isAvailable() {
        return LOADED;
    }

    public static boolean supports(int codec) {
        return LOADED && nativeSupports(codec);
    }

    public static native boolean nativeAvailable();
    public static native boolean nativeSupports(int codec);
    public static native long nativeOpen(int codec, int rate, int ch);
    /** Opus/FLAC/Vorbis/ALAC 等需要 csd；extra 可 null。 */
    public static native long nativeOpenEx(int codec, int rate, int ch, byte[] extra);
    public static native void nativeClose(long h);
    public static native void nativeFlush(long h);
    public static native int nativeOutRate(long h);
    public static native int nativeOutChannels(long h);
    public static native int nativeDecode(long h, ByteBuffer in, int inOff, int inLen,
                                          ByteBuffer out, int outCap);
}
