/*
 * ms_hw_passthrough.c — 音频直通的纯 C 部分：路由、帧采样数、IEC 61937 突发封装。
 * 参考 IEC 61937-3（AC-3）、-5（DTS）以及 E-AC-3 的 4 倍突发周期约定。
 * by: Long <mycjzb@gmail.com>
 */
#include "ms_hw_passthrough.h"

#include <stdlib.h>
#include <string.h>

#define IEC_PA 0xF872u
#define IEC_PB 0x4E1Fu
#define IEC_TYPE_AC3 1u
#define IEC_TYPE_DTS1 11u
#define IEC_TYPE_DTS2 12u
#define IEC_TYPE_DTS3 13u
#define IEC_TYPE_EAC3 21u
#define IEC_HDR 8u
#define EAC3_AGG_SAMPLES 1536 /* 聚合 6 个音频块再出一个突发 */
#define EAC3_PERIOD_FRAMES 6144 /* 4x 采样率下的突发周期 */

static unsigned codec_cap(ms_codec c)
{
    switch (c) {
    case MS_CODEC_AC3: return MS_PT_CAP_AC3;
    case MS_CODEC_EAC3: return MS_PT_CAP_E_AC3;
    case MS_CODEC_DTS: return MS_PT_CAP_DTS;
    default: return 0;
    }
}

static int codec_enc(ms_codec c)
{
    switch (c) {
    case MS_CODEC_AC3: return MS_PT_ENC_AC3;
    case MS_CODEC_EAC3: return MS_PT_ENC_E_AC3;
    case MS_CODEC_DTS: return MS_PT_ENC_DTS;
    default: return 0;
    }
}

ms_pt_route ms_pt_pick_route(ms_codec codec, unsigned caps, int api_level, int *out_encoding)
{
    unsigned need = codec_cap(codec);
    if (out_encoding != NULL) {
        *out_encoding = 0;
    }
    if (!(caps & MS_PT_CAP_ENABLED) || need == 0 || api_level < 21) {
        return MS_PT_ROUTE_SOFT;
    }
    if (caps & need) {
        if (out_encoding != NULL) {
            *out_encoding = codec_enc(codec);
        }
        return MS_PT_ROUTE_RAW;
    }
    if ((caps & MS_PT_CAP_IEC61937) && api_level >= 24) {
        if (out_encoding != NULL) {
            *out_encoding = MS_PT_ENC_IEC61937;
        }
        return MS_PT_ROUTE_IEC;
    }
    return MS_PT_ROUTE_SOFT;
}

/* AC-3 帧长（字），按 frmsizecod/2 与 fscod 查表（ATSC A/52 表 5.18）。 */
static const uint16_t ac3_words[38][3] = {
    {64, 69, 96}, {64, 70, 96}, {80, 87, 120}, {80, 88, 120}, {96, 104, 144}, {96, 105, 144},
    {112, 121, 168}, {112, 122, 168}, {128, 139, 192}, {128, 140, 192}, {160, 174, 240},
    {160, 175, 240}, {192, 208, 288}, {192, 209, 288}, {224, 243, 336}, {224, 244, 336},
    {256, 278, 384}, {256, 279, 384}, {320, 348, 480}, {320, 349, 480}, {384, 417, 576},
    {384, 418, 576}, {448, 487, 672}, {448, 488, 672}, {512, 557, 768}, {512, 558, 768},
    {640, 696, 960}, {640, 697, 960}, {768, 835, 1152}, {768, 836, 1152}, {896, 975, 1344},
    {896, 976, 1344}, {1024, 1114, 1536}, {1024, 1115, 1536}, {1152, 1253, 1728},
    {1152, 1254, 1728}, {1280, 1393, 1920}, {1280, 1394, 1920}};

/* 解析一个 (E-)AC-3 同步帧：返回帧字节数，*samples 为该帧 PCM 采样数，*indep 是否独立子流。 */
static size_t ac3_sync_frame(const uint8_t *p, size_t n, int *samples, int *indep)
{
    unsigned bsid;
    if (n < 6 || p[0] != 0x0B || p[1] != 0x77) {
        return 0;
    }
    bsid = (unsigned)(p[5] >> 3);
    if (bsid <= 10) {
        unsigned fscod = (unsigned)(p[4] >> 6), code = (unsigned)(p[4] & 0x3F);
        if (fscod > 2 || code >= 38) {
            return 0;
        }
        *samples = 1536;
        *indep = 1;
        return (size_t)ac3_words[code][fscod] * 2u;
    }
    if (bsid >= 11 && bsid <= 16) {
        unsigned strmtyp = (unsigned)(p[2] >> 6);
        size_t fsz = ((((size_t)(p[2] & 0x07)) << 8) | p[3]) + 1u;
        unsigned fscod = (unsigned)(p[4] >> 6);
        unsigned blk = (fscod == 3) ? 3u : (unsigned)((p[4] >> 4) & 3);
        static const int nblk[4] = {1, 2, 3, 6};
        *samples = nblk[blk] * 256;
        *indep = (strmtyp != 1); /* 1 = 依赖子流，不增加时长 */
        return fsz * 2u;
    }
    return 0;
}

static int dts_samples(const uint8_t *p, size_t n)
{
    unsigned nblks;
    /* 只认 16 位大端核心同步字（TS/MKV/MP4 中最常见）。 */
    if (n < 10 || p[0] != 0x7F || p[1] != 0xFE || p[2] != 0x80 || p[3] != 0x01) {
        return 0;
    }
    nblks = ((unsigned)(p[4] & 0x01) << 6) | (unsigned)(p[5] >> 2);
    return (int)(nblks + 1u) * 32;
}

int ms_pt_frame_samples(ms_codec codec, const uint8_t *data, size_t n)
{
    if (data == NULL) {
        return 0;
    }
    if (codec == MS_CODEC_AC3 || codec == MS_CODEC_EAC3) {
        size_t off = 0;
        int total = 0;
        while (off < n) {
            int s = 0, ind = 0;
            size_t fl = ac3_sync_frame(data + off, n - off, &s, &ind);
            if (fl == 0 || fl > n - off) {
                break;
            }
            if (ind) {
                total += s;
            }
            off += fl;
        }
        return total;
    }
    if (codec == MS_CODEC_DTS) {
        return dts_samples(data, n);
    }
    return 0;
}

int ms_pt_iec_rate_mul(ms_codec codec)
{
    return codec == MS_CODEC_EAC3 ? 4 : 1;
}

int ms_pt_iec_init(ms_pt_iec *w, ms_codec codec)
{
    if (w == NULL || codec_cap(codec) == 0) {
        return -1;
    }
    memset(w, 0, sizeof(*w));
    w->codec = codec;
    return 0;
}

void ms_pt_iec_free(ms_pt_iec *w)
{
    if (w == NULL) {
        return;
    }
    free(w->acc);
    free(w->out);
    memset(w, 0, sizeof(*w));
}

static int ensure_cap(uint8_t **b, size_t *cap, size_t need)
{
    uint8_t *nb;
    if (*cap >= need) {
        return 0;
    }
    nb = (uint8_t *)realloc(*b, need);
    if (nb == NULL) {
        return -1;
    }
    *b = nb;
    *cap = need;
    return 0;
}

static void put16le(uint8_t *p, unsigned v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)(v >> 8);
}

/* 写一个突发：头 Pa/Pb/Pc/Pd + 字节对交换后的载荷 + 补零到 period_frames*4 字节。 */
static int iec_burst(ms_pt_iec *w, unsigned type, unsigned pd, const uint8_t *pl, size_t n,
                     int period_frames, const uint8_t **out, size_t *out_n)
{
    size_t total = (size_t)period_frames * 4u, i;
    if (IEC_HDR + n + 1 > total) {
        return -1;
    }
    if (ensure_cap(&w->out, &w->out_cap, total) != 0) {
        return -1;
    }
    memset(w->out, 0, total);
    put16le(w->out + 0, IEC_PA);
    put16le(w->out + 2, IEC_PB);
    put16le(w->out + 4, type);
    put16le(w->out + 6, pd);
    /* 码流为大端字节序；IEC 61937 以 16 位小端样本承载，故成对交换。 */
    for (i = 0; i + 1 < n; i += 2) {
        w->out[IEC_HDR + i] = pl[i + 1];
        w->out[IEC_HDR + i + 1] = pl[i];
    }
    if (n & 1u) {
        w->out[IEC_HDR + n] = pl[n - 1];
    }
    *out = w->out;
    *out_n = total;
    return 1;
}

int ms_pt_iec_feed(ms_pt_iec *w, const uint8_t *au, size_t n,
                   const uint8_t **out, size_t *out_n, int *frames)
{
    int s;
    if (w == NULL || au == NULL || n == 0 || out == NULL || out_n == NULL || frames == NULL) {
        return -1;
    }
    *out = NULL;
    *out_n = 0;
    *frames = 0;
    s = ms_pt_frame_samples(w->codec, au, n);
    if (s <= 0) {
        return -1;
    }
    if (w->codec == MS_CODEC_AC3) {
        *frames = 1536;
        return iec_burst(w, IEC_TYPE_AC3, (unsigned)(n * 8u), au, n, 1536, out, out_n);
    }
    if (w->codec == MS_CODEC_DTS) {
        unsigned type;
        if (s == 512) {
            type = IEC_TYPE_DTS1;
        } else if (s == 1024) {
            type = IEC_TYPE_DTS2;
        } else if (s == 2048) {
            type = IEC_TYPE_DTS3;
        } else {
            return -1;
        }
        *frames = s;
        return iec_burst(w, type, (unsigned)(n * 8u), au, n, s, out, out_n);
    }
    /* E-AC-3：聚合到 1536 音频采样，按 4 倍采样率封装为 6144 帧突发（24576 字节，Pd 以字节计）。
     * 调用方须以 4×流采样率打开 IEC61937 AudioTrack。 */
    if (w->acc_n + n > 24576u - IEC_HDR) {
        w->acc_n = 0;
        w->acc_samples = 0;
        return -1;
    }
    if (ensure_cap(&w->acc, &w->acc_cap, w->acc_n + n) != 0) {
        return -1;
    }
    memcpy(w->acc + w->acc_n, au, n);
    w->acc_n += n;
    w->acc_samples += s;
    if (w->acc_samples < EAC3_AGG_SAMPLES) {
        return 0;
    }
    {
        int r = iec_burst(w, IEC_TYPE_EAC3, (unsigned)w->acc_n, w->acc, w->acc_n,
                          EAC3_PERIOD_FRAMES, out, out_n);
        *frames = (r == 1) ? EAC3_PERIOD_FRAMES : 0;
        w->acc_n = 0;
        w->acc_samples = 0;
        return r;
    }
}
