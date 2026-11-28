package androidx.media3.decoder.ffmpeg;

import android.os.Handler;

import androidx.media3.common.C;
import androidx.media3.common.Format;
import androidx.media3.common.MimeTypes;
import androidx.media3.common.util.UnstableApi;
import androidx.media3.common.util.Util;
import androidx.media3.decoder.CryptoConfig;
import androidx.media3.exoplayer.audio.AudioRendererEventListener;
import androidx.media3.exoplayer.audio.AudioSink;
import androidx.media3.exoplayer.audio.DecoderAudioRenderer;
import androidx.media3.exoplayer.audio.DefaultAudioSink;

import com.mycjzb.media.player.FfAdec;

/**
 * Media3 扩展音频渲染器（{@link DefaultRenderersFactory} 反射加载本类）。
 * MediaCodec 找不到再回落；常用音频软解：aac/mp2/mp3/ac3/eac3/dts/dra/g711 + opus/flac/pcm/vorbis/alac。
 */
@UnstableApi
public final class FfmpegAudioRenderer extends DecoderAudioRenderer<FfmpegAudioDecoder> {

    public FfmpegAudioRenderer() {
        this(/* eventHandler= */ null, /* eventListener= */ null);
    }

    public FfmpegAudioRenderer(Handler eventHandler, AudioRendererEventListener eventListener) {
        this(eventHandler, eventListener, new DefaultAudioSink.Builder().build());
    }

    public FfmpegAudioRenderer(
            Handler eventHandler,
            AudioRendererEventListener eventListener,
            AudioSink audioSink) {
        super(eventHandler, eventListener, audioSink);
    }

    @Override
    public String getName() {
        return "FfmpegAudioRenderer";
    }

    @Override
    protected @C.FormatSupport int supportsFormatInternal(Format format) {
        if (!FfAdec.isAvailable()) {
            return C.FORMAT_UNSUPPORTED_TYPE;
        }
        String mime = format.sampleMimeType;
        if (mime == null) {
            return C.FORMAT_UNSUPPORTED_TYPE;
        }
        int codec = mimeToCodec(mime);
        if (codec < 0 || !FfAdec.supports(codec)) {
            return C.FORMAT_UNSUPPORTED_SUBTYPE;
        }
        if (format.cryptoType != C.CRYPTO_TYPE_NONE) {
            return C.FORMAT_UNSUPPORTED_DRM;
        }
        int ch = format.channelCount != Format.NO_VALUE ? format.channelCount : 2;
        int rate = format.sampleRate != Format.NO_VALUE ? format.sampleRate : 48000;
        return sinkSupportsFormat(Util.getPcmFormat(C.ENCODING_PCM_16BIT, ch, rate))
                ? C.FORMAT_HANDLED
                : C.FORMAT_UNSUPPORTED_SUBTYPE;
    }

    @Override
    protected FfmpegAudioDecoder createDecoder(Format format, CryptoConfig cryptoConfig)
            throws FfmpegDecoderException {
        int codec = mimeToCodec(format.sampleMimeType);
        if (codec < 0) {
            throw new FfmpegDecoderException("unsupported " + format.sampleMimeType);
        }
        return new FfmpegAudioDecoder(format, codec);
    }

    @Override
    protected Format getOutputFormat(FfmpegAudioDecoder decoder) {
        return Util.getPcmFormat(
                C.ENCODING_PCM_16BIT, decoder.getChannelCount(), decoder.getSampleRate());
    }

    /** 国标 DRA；Extractor 可能标此 mime（对齐 bs-mycjzb-media3）。 */
    public static final String AUDIO_DRA = "audio/dra";

    static int mimeToCodec(String mime) {
        if (mime == null) {
            return -1;
        }
        switch (mime) {
            case MimeTypes.AUDIO_AAC: /* audio/mp4a-latm */
                return FfAdec.CODEC_AAC;
            case MimeTypes.AUDIO_MPEG_L2:
                return FfAdec.CODEC_MP2;
            case MimeTypes.AUDIO_MPEG:
                return FfAdec.CODEC_MP3;
            case MimeTypes.AUDIO_AC3:
                return FfAdec.CODEC_AC3;
            case MimeTypes.AUDIO_E_AC3:
            case MimeTypes.AUDIO_E_AC3_JOC:
                return FfAdec.CODEC_EAC3;
            case MimeTypes.AUDIO_DTS:
            case MimeTypes.AUDIO_DTS_HD:
            case MimeTypes.AUDIO_DTS_EXPRESS:
                return FfAdec.CODEC_DTS;
            case MimeTypes.AUDIO_ALAW:
                return FfAdec.CODEC_G711_ALAW;
            case MimeTypes.AUDIO_MLAW:
                return FfAdec.CODEC_G711_ULAW;
            case MimeTypes.AUDIO_OPUS:
                return FfAdec.CODEC_OPUS;
            case MimeTypes.AUDIO_FLAC:
                return FfAdec.CODEC_FLAC;
            case MimeTypes.AUDIO_RAW:
            case MimeTypes.AUDIO_WAV:
                return FfAdec.CODEC_PCM;
            case MimeTypes.AUDIO_VORBIS:
                return FfAdec.CODEC_VORBIS;
            case MimeTypes.AUDIO_ALAC:
                return FfAdec.CODEC_ALAC;
            case AUDIO_DRA:
            case "audio/x-dra":
            case "audio/vnd.dra":
                return FfAdec.CODEC_DRA;
            default:
                if ("dra".equalsIgnoreCase(mime)) {
                    return FfAdec.CODEC_DRA;
                }
                if ("audio/mpeg-L3".equalsIgnoreCase(mime)) {
                    return FfAdec.CODEC_MP3;
                }
                return -1;
        }
    }
}
