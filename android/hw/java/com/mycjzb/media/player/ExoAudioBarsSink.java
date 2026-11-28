package com.mycjzb.media.player;

import android.annotation.TargetApi;

import androidx.media3.common.C;
import androidx.media3.exoplayer.audio.TeeAudioProcessor;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;

/**
 * 抄超级直播 {@code ExoAudioBarsSink}：Tee PCM → 5 段能量 0–255。
 * 透传无 PCM 时 bars 恒 0，属正常。
 */
@TargetApi(19)
final class ExoAudioBarsSink implements TeeAudioProcessor.AudioBufferSink {
    private final int[] bands = new int[5];
    private int channels = 2;
    private int encoding = C.ENCODING_PCM_16BIT;

    @Override
    public void flush(int sampleRateHz, int channelCount, @C.PcmEncoding int encoding) {
        this.channels = channelCount > 0 ? channelCount : 2;
        this.encoding = encoding;
    }

    @Override
    public void handleBuffer(ByteBuffer buffer) {
        if (buffer == null || !buffer.hasRemaining()) {
            return;
        }
        ByteBuffer b = buffer.order(ByteOrder.nativeOrder());
        if (encoding != C.ENCODING_PCM_16BIT && encoding != C.ENCODING_PCM_FLOAT) {
            return;
        }
        int ch = channels > 0 ? channels : 2;
        int frameBytes = encoding == C.ENCODING_PCM_FLOAT ? 4 * ch : 2 * ch;
        if (frameBytes <= 0) {
            return;
        }
        int frames = b.remaining() / frameBytes;
        if (frames <= 0) {
            return;
        }
        int[] sum = new int[5];
        int[] cnt = new int[5];
        int pos = b.position();
        for (int f = 0; f < frames; f++) {
            int band = (f * 5) / frames;
            if (band > 4) {
                band = 4;
            }
            int peak = 0;
            for (int c = 0; c < ch; c++) {
                int abs;
                if (encoding == C.ENCODING_PCM_FLOAT) {
                    float v = b.getFloat(pos);
                    pos += 4;
                    abs = (int) (Math.min(1f, Math.abs(v)) * 32767f);
                } else {
                    short s = b.getShort(pos);
                    pos += 2;
                    abs = s >= 0 ? s : -s;
                }
                if (abs > peak) {
                    peak = abs;
                }
            }
            sum[band] += peak;
            cnt[band]++;
        }
        synchronized (bands) {
            for (int i = 0; i < 5; i++) {
                int avg = cnt[i] > 0 ? sum[i] / cnt[i] : 0;
                int v = avg >> 7;
                if (v > 255) {
                    v = 255;
                }
                bands[i] = v;
            }
        }
    }

    /** @return true 若至少一段非 0 */
    boolean read(int[] out) {
        if (out == null || out.length == 0) {
            return false;
        }
        boolean any = false;
        synchronized (bands) {
            int n = out.length < 5 ? out.length : 5;
            for (int i = 0; i < n; i++) {
                out[i] = bands[i];
                if (bands[i] != 0) {
                    any = true;
                }
            }
            for (int i = n; i < out.length; i++) {
                out[i] = 0;
            }
        }
        return any;
    }
}
