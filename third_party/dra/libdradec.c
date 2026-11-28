/*
 * DRA (Digital Rise Audio / GB/T 22726) decoder wrapper for FFmpeg n6.0.
 * Core is dradec.c from msServer FFmpeg-AVSPlus (maliwen2015/ffmpeg_cavs_dra, DRA only).
 */
#include <string.h>

#include "libavutil/channel_layout.h"

#include "avcodec.h"
#include "codec_internal.h"
#include "decode.h"
#include "dradec.h"

typedef struct DRAContext {
    void *dec;
} DRAContext;

static av_cold int dra_decode_init(AVCodecContext *avctx)
{
    DRAContext *s = avctx->priv_data;

    avctx->sample_fmt = AV_SAMPLE_FMT_S16;
    avctx->frame_size = 1024;
    s->dec = DRADecCreate();
    if (!s->dec)
        return AVERROR(ENOMEM);
    return 0;
}

static av_cold int dra_decode_close(AVCodecContext *avctx)
{
    DRAContext *s = avctx->priv_data;

    if (s->dec) {
        DRADecDestroy(s->dec);
        s->dec = NULL;
    }
    return 0;
}

static int dra_output_frame(AVCodecContext *avctx, AVFrame *frame,
                            uint8_t *pcm, int pcm_len)
{
    DRAFrameInfo info = { 0 };
    DRAContext *s = avctx->priv_data;
    int channels, ret;

    DRADecGetFrameInfo(s->dec, &info);
    channels = info.nChannels;
    if (channels <= 0 || info.nSampleRate <= 0 || pcm_len < channels * 2)
        return AVERROR_INVALIDDATA;

    avctx->sample_rate = info.nSampleRate;
    av_channel_layout_uninit(&avctx->ch_layout);
    av_channel_layout_default(&avctx->ch_layout, channels);

    frame->nb_samples = pcm_len / (channels * 2);
    frame->sample_rate = avctx->sample_rate;
    if ((ret = av_channel_layout_copy(&frame->ch_layout, &avctx->ch_layout)) < 0)
        return ret;
    if ((ret = ff_get_buffer(avctx, frame, 0)) < 0)
        return ret;
    memcpy(frame->data[0], pcm, (size_t)pcm_len);
    return 0;
}

static int dra_decode_frame(AVCodecContext *avctx, AVFrame *frame,
                            int *got_frame_ptr, AVPacket *avpkt)
{
    DRAContext *s = avctx->priv_data;
    unsigned char *pcm = NULL;
    int pcm_len = 0;
    int ret;

    *got_frame_ptr = 0;

    if (avpkt->size > 0) {
        if (DRADecSendData(s->dec, avpkt->data, avpkt->size) != 0)
            return AVERROR_INVALIDDATA;
    }

    ret = DRADecRecvFrame(s->dec, &pcm, &pcm_len);
    if (ret != 0 || !pcm || pcm_len <= 0)
        return avpkt->size > 0 ? avpkt->size : 0;

    ret = dra_output_frame(avctx, frame, pcm, pcm_len);
    if (ret < 0)
        return ret;
    *got_frame_ptr = 1;
    return avpkt->size;
}

const FFCodec ff_dra_decoder = {
    .p.name         = "dra",
    CODEC_LONG_NAME("DRA (Digital Rise Audio)"),
    .p.type         = AVMEDIA_TYPE_AUDIO,
    .p.id           = AV_CODEC_ID_DRA,
    .priv_data_size = sizeof(DRAContext),
    .init           = dra_decode_init,
    .close          = dra_decode_close,
    FF_CODEC_DECODE_CB(dra_decode_frame),
    .p.capabilities = AV_CODEC_CAP_CHANNEL_CONF |
                      AV_CODEC_CAP_DR1 |
                      AV_CODEC_CAP_DELAY,
    .caps_internal  = FF_CODEC_CAP_INIT_CLEANUP,
    CODEC_SAMPLEFMTS(AV_SAMPLE_FMT_S16),
};
