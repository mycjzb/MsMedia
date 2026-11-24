/*
 * 路线 B 单 so 专用：系统普遍缺失的音频（AC-3 / E-AC-3 / MP2）用 FFmpeg 内置轻量软解出 PCM，
 * 统一送 AudioTrack。只在系统 MediaCodec 没有对应解码器时启用；视频永不软解。
 * 输出 s16 交织，最多 2 声道（AC-3/E-AC-3 请求解码器内下混到立体声）。
 * 仅在 -DMS_SINGLE_FFADEC 的 single-slimff 构建中编译；冻结产物与官方多 so 不含本文件。
 * by: Long <mycjzb@gmail.com>
 */
#include "ms_hw_ffadec.h"

#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <stdlib.h>
#include <string.h>

struct ms_ffadec {
    AVCodecContext *ctx;
    AVPacket *pkt;
    AVFrame *frm;
    uint8_t *pcm;
    size_t pcm_cap;
    int out_rate;
    int out_ch;
};

int ms_ffadec_supported(ms_codec codec)
{
    return codec == MS_CODEC_AC3 || codec == MS_CODEC_EAC3 || codec == MS_CODEC_MP2 ||
           codec == MS_CODEC_G711_ALAW || codec == MS_CODEC_G711_ULAW ||
           codec == MS_CODEC_DTS || codec == MS_CODEC_AAC_LATM;
}

static enum AVCodecID to_id(ms_codec c)
{
    switch (c) {
    case MS_CODEC_AC3: return AV_CODEC_ID_AC3;
    case MS_CODEC_EAC3: return AV_CODEC_ID_EAC3;
    case MS_CODEC_MP2: return AV_CODEC_ID_MP2;
    case MS_CODEC_G711_ALAW: return AV_CODEC_ID_PCM_ALAW;
    case MS_CODEC_G711_ULAW: return AV_CODEC_ID_PCM_MULAW;
    case MS_CODEC_DTS: return AV_CODEC_ID_DTS;
    case MS_CODEC_AAC_LATM: return AV_CODEC_ID_AAC_LATM;
    default: return AV_CODEC_ID_NONE;
    }
}

int ms_ffadec_open(ms_codec codec, int rate, int ch, ms_ffadec **out)
{
    const AVCodec *dec;
    ms_ffadec *d;
    AVDictionary *opt = NULL;
    if (out == NULL) return -1;
    *out = NULL;
    dec = avcodec_find_decoder(to_id(codec));
    if (dec == NULL) return -1;
    d = (ms_ffadec *)calloc(1, sizeof(*d));
    if (d == NULL) return -1;
    d->ctx = avcodec_alloc_context3(dec);
    d->pkt = av_packet_alloc();
    d->frm = av_frame_alloc();
    if (d->ctx == NULL || d->pkt == NULL || d->frm == NULL) {
        ms_ffadec_close(d);
        return -1;
    }
    if (rate > 0) d->ctx->sample_rate = rate;
    if (ch > 0) av_channel_layout_default(&d->ctx->ch_layout, ch);
    if (codec == MS_CODEC_AC3 || codec == MS_CODEC_EAC3 || codec == MS_CODEC_DTS) {
        av_dict_set(&opt, "downmix", "stereo", 0); /* 多声道在解码器内下混到立体声 */
    }
    if ((codec == MS_CODEC_G711_ALAW || codec == MS_CODEC_G711_ULAW) && (rate <= 0 || ch <= 0)) {
        av_dict_free(&opt);
        ms_ffadec_close(d);
        return -1; /* G.711 无带内参数，缺采样率/声道不能猜 */
    }
    if (avcodec_open2(d->ctx, dec, &opt) < 0) {
        av_dict_free(&opt);
        ms_ffadec_close(d);
        return -1;
    }
    av_dict_free(&opt);
    d->out_rate = rate > 0 ? rate : 48000;
    d->out_ch = (ch > 0 && ch < 2) ? 1 : 2;
    *out = d;
    return 0;
}

int ms_ffadec_out_rate(const ms_ffadec *d) { return d ? d->out_rate : 0; }
int ms_ffadec_out_channels(const ms_ffadec *d) { return d ? d->out_ch : 0; }

static int16_t clip16(float v)
{
    int x = (int)(v * 32767.0f);
    if (x > 32767) x = 32767;
    if (x < -32768) x = -32768;
    return (int16_t)x;
}

static float sample_at(const AVFrame *f, int c, int i)
{
    int nch = f->ch_layout.nb_channels;
    switch (f->format) {
    case AV_SAMPLE_FMT_FLTP: return ((const float *)f->extended_data[c])[i];
    case AV_SAMPLE_FMT_FLT:  return ((const float *)f->extended_data[0])[i * nch + c];
    case AV_SAMPLE_FMT_S16P: return ((const int16_t *)f->extended_data[c])[i] / 32768.0f;
    case AV_SAMPLE_FMT_S16:  return ((const int16_t *)f->extended_data[0])[i * nch + c] / 32768.0f;
    case AV_SAMPLE_FMT_S32P: return ((const int32_t *)f->extended_data[c])[i] / 2147483648.0f;
    case AV_SAMPLE_FMT_S32:  return ((const int32_t *)f->extended_data[0])[i * nch + c] / 2147483648.0f;
    default: return 0.f;
    }
}

/* 解一个 AU，输出指针归 d 所有，下一次调用前有效。返回 0 成功（*pcm_n 可能为 0），负值失败。 */
int ms_ffadec_decode(ms_ffadec *d, const uint8_t *au, size_t n, const uint8_t **pcm, size_t *pcm_n)
{
    size_t used = 0;
    int r;
    if (d == NULL || pcm == NULL || pcm_n == NULL) return -1;
    *pcm = NULL;
    *pcm_n = 0;
    if (au == NULL || n == 0 || n > (1u << 20)) return -1;
    av_packet_unref(d->pkt);
    if (av_new_packet(d->pkt, (int)n) < 0) return -1;
    memcpy(d->pkt->data, au, n);
    r = avcodec_send_packet(d->ctx, d->pkt);
    av_packet_unref(d->pkt);
    if (r < 0 && r != AVERROR(EAGAIN)) return 0; /* 坏帧：丢弃，不中断播放 */
    for (;;) {
        int nch, outc, i, c, ns;
        size_t need;
        int16_t *dst;
        r = avcodec_receive_frame(d->ctx, d->frm);
        if (r < 0) break;
        nch = d->frm->ch_layout.nb_channels;
        ns = d->frm->nb_samples;
        if (nch <= 0 || ns <= 0 || ns > 65536) {
            av_frame_unref(d->frm);
            continue;
        }
        outc = d->out_ch;
        need = used + (size_t)ns * (size_t)outc * 2u;
        if (need > d->pcm_cap) {
            size_t nc = need * 2;
            uint8_t *nb = (uint8_t *)realloc(d->pcm, nc);
            if (nb == NULL) {
                av_frame_unref(d->frm);
                return -1;
            }
            d->pcm = nb;
            d->pcm_cap = nc;
        }
        dst = (int16_t *)(void *)(d->pcm + used);
        for (i = 0; i < ns; i++) {
            if (outc == 1) {
                dst[i] = clip16(sample_at(d->frm, 0, i));
            } else if (nch == 1) {
                int16_t s = clip16(sample_at(d->frm, 0, i));
                dst[2 * i] = s;
                dst[2 * i + 1] = s;
            } else {
                /* 解码器已下混时 nch==2；否则取前两声道（不做环绕混音，诚实标注） */
                for (c = 0; c < 2; c++) dst[2 * i + c] = clip16(sample_at(d->frm, c, i));
            }
        }
        used = need;
        if (d->frm->sample_rate > 0) d->out_rate = d->frm->sample_rate;
        av_frame_unref(d->frm);
    }
    *pcm = d->pcm;
    *pcm_n = used;
    return 0;
}

void ms_ffadec_flush(ms_ffadec *d)
{
    if (d && d->ctx) avcodec_flush_buffers(d->ctx);
}

void ms_ffadec_close(ms_ffadec *d)
{
    if (d == NULL) return;
    avcodec_free_context(&d->ctx);
    av_packet_free(&d->pkt);
    av_frame_free(&d->frm);
    free(d->pcm);
    free(d);
}
