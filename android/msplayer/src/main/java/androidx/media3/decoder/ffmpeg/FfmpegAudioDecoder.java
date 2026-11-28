package androidx.media3.decoder.ffmpeg;

import androidx.media3.common.Format;
import androidx.media3.common.util.UnstableApi;
import androidx.media3.decoder.DecoderInputBuffer;
import androidx.media3.decoder.SimpleDecoder;
import androidx.media3.decoder.SimpleDecoderOutputBuffer;

import com.mycjzb.media.player.FfAdec;

import java.nio.ByteBuffer;
import java.util.List;

/** 薄封装：解码路径走 {@link FfAdec} / ms_ffadec。 */
@UnstableApi
final class FfmpegAudioDecoder
        extends SimpleDecoder<DecoderInputBuffer, SimpleDecoderOutputBuffer, FfmpegDecoderException> {

    private final long handle;
    private final int outputRate;
    private final int outputChannels;
    private volatile boolean released;

    FfmpegAudioDecoder(Format format, int codec) throws FfmpegDecoderException {
        super(new DecoderInputBuffer[4], new SimpleDecoderOutputBuffer[4]);
        int rate = format.sampleRate > 0 ? format.sampleRate : 48000;
        int ch = format.channelCount > 0 ? format.channelCount : 2;
        byte[] extra = packExtra(format.initializationData);
        handle = FfAdec.nativeOpenEx(codec, rate, ch, extra);
        if (handle == 0) {
            throw new FfmpegDecoderException("ffadec open failed codec=" + codec);
        }
        outputRate = Math.max(1, FfAdec.nativeOutRate(handle));
        outputChannels = Math.max(1, FfAdec.nativeOutChannels(handle));
        setInitialInputBufferSize(Math.max(4096, format.maxInputSize > 0 ? format.maxInputSize : 4096));
    }

    /** 多段 csd 拼成一块给 FFmpeg extradata（单段直接用）。 */
    private static byte[] packExtra(List<byte[]> init) {
        if (init == null || init.isEmpty()) {
            return null;
        }
        if (init.size() == 1) {
            byte[] one = init.get(0);
            return one == null || one.length == 0 ? null : one;
        }
        int n = 0;
        for (byte[] b : init) {
            if (b != null) {
                n += b.length;
            }
        }
        if (n <= 0) {
            return null;
        }
        byte[] out = new byte[n];
        int o = 0;
        for (byte[] b : init) {
            if (b == null || b.length == 0) {
                continue;
            }
            System.arraycopy(b, 0, out, o, b.length);
            o += b.length;
        }
        return out;
    }

    int getSampleRate() {
        return outputRate;
    }

    int getChannelCount() {
        return outputChannels;
    }

    @Override
    public String getName() {
        return "ffmpeg-ms";
    }

    @Override
    protected DecoderInputBuffer createInputBuffer() {
        return new DecoderInputBuffer(DecoderInputBuffer.BUFFER_REPLACEMENT_MODE_DIRECT);
    }

    @Override
    protected SimpleDecoderOutputBuffer createOutputBuffer() {
        return new SimpleDecoderOutputBuffer(this::releaseOutputBuffer);
    }

    @Override
    protected FfmpegDecoderException createUnexpectedDecodeException(Throwable error) {
        return new FfmpegDecoderException("decode", error);
    }

    @Override
    protected FfmpegDecoderException decode(
            DecoderInputBuffer inputBuffer, SimpleDecoderOutputBuffer outputBuffer, boolean reset) {
        if (released) {
            return new FfmpegDecoderException("released");
        }
        if (reset) {
            FfAdec.nativeFlush(handle);
        }
        if (inputBuffer.isEndOfStream()) {
            outputBuffer.setFlags(androidx.media3.common.C.BUFFER_FLAG_END_OF_STREAM);
            return null;
        }
        ByteBuffer in = inputBuffer.data;
        if (in == null || !in.hasRemaining()) {
            outputBuffer.timeUs = inputBuffer.timeUs;
            return null;
        }
        int inOff = in.position();
        int inLen = in.remaining();
        int cap = Math.max(8192, inLen * 16);
        ByteBuffer out = outputBuffer.init(inputBuffer.timeUs, cap);
        int n = FfAdec.nativeDecode(handle, in, inOff, inLen, out, cap);
        if (n == -2) {
            cap = Math.max(cap * 2, 65536);
            out = outputBuffer.init(inputBuffer.timeUs, cap);
            n = FfAdec.nativeDecode(handle, in, inOff, inLen, out, cap);
        }
        if (n < 0) {
            return new FfmpegDecoderException("ffadec decode " + n);
        }
        in.position(in.limit());
        out.position(0);
        out.limit(n);
        outputBuffer.timeUs = inputBuffer.timeUs;
        return null;
    }

    @Override
    public void release() {
        super.release();
        if (!released) {
            released = true;
            FfAdec.nativeClose(handle);
        }
    }
}
