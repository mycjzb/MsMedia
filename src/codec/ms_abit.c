/*
 * 通用音频配置 / 完整帧检查 / 签名（AAC 转发既有 ASC 解析；AC-3、MP2、MP3、Opus）。
 * 不是软解，不输出 PCM。不得为非 AAC 编码捏造 AAC ASC。
 * by: Long <mycjzb@gmail.com>
 */
#include "msmedia/msmedia.h"

#include <string.h>

void ms_audio_cfg_default(ms_audio_cfg *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->struct_size = sizeof(*out);
}

int ms_audio_hw_path_ready(ms_codec codec)
{
    return codec == MS_CODEC_AAC || codec == MS_CODEC_AC3 ||
           codec == MS_CODEC_MP3 || codec == MS_CODEC_MP2 ||
           codec == MS_CODEC_OPUS || codec == MS_CODEC_FLAC ||
#ifdef MS_SINGLE_SO
           codec == MS_CODEC_EAC3 || /* 单 so：系统 audio/eac3 或路线 B FFmpeg 软解 */
           codec == MS_CODEC_G711_ALAW || codec == MS_CODEC_G711_ULAW ||
           codec == MS_CODEC_DTS || codec == MS_CODEC_AAC_LATM ||
#endif
           codec == MS_CODEC_PCM;
}

static uint32_t abit_sig(const uint8_t *p, size_t n)
{
    uint32_t h = 2166136261u;
    size_t i;
    if (p == NULL) {
        return 0;
    }
    for (i = 0; i < n; i++) {
        h ^= p[i];
        h *= 16777619u;
    }
    return h == 0 ? 1u : h;
}

int ms_audio_cfg_equal(const ms_audio_cfg *a, const ms_audio_cfg *b)
{
    if (a == NULL || b == NULL) {
        return 0;
    }
    if (a->codec != b->codec || a->complete != b->complete) {
        return 0;
    }
    if (a->sample_rate != b->sample_rate || a->channels != b->channels) {
        return 0;
    }
    if (a->payload_sig != b->payload_sig || a->csd_size != b->csd_size) {
        return 0;
    }
    if (a->csd_size > sizeof(a->csd) || b->csd_size > sizeof(b->csd)) {
        return 0;
    }
    if (a->csd_size > 0 && memcmp(a->csd, b->csd, a->csd_size) != 0) {
        return 0;
    }
    return 1;
}

/* ATSC A/52 表：以 16-bit word 计的帧长；字节数 = words * 2。 */
static const uint16_t k_ac3_words[3][38] = {
    { 64, 64, 80, 80, 96, 96, 112, 112, 128, 128, 160, 160, 192, 192, 224, 224,
      256, 256, 320, 320, 384, 384, 448, 448, 512, 512, 640, 640, 768, 768, 896,
      896, 1024, 1024, 1152, 1152, 1280, 1280 },
    { 69, 70, 87, 88, 104, 105, 121, 122, 139, 140, 174, 175, 208, 209, 243, 244,
      278, 279, 348, 349, 417, 418, 487, 488, 557, 558, 696, 697, 835, 836, 975,
      976, 1114, 1115, 1253, 1254, 1393, 1394 },
    { 96, 96, 120, 120, 144, 144, 168, 168, 192, 192, 240, 240, 288, 288, 336,
      336, 384, 384, 480, 480, 576, 576, 672, 672, 768, 768, 960, 960, 1152,
      1152, 1344, 1344, 1536, 1536, 1728, 1728, 1920, 1920 }
};

static const int k_ac3_acmod_ch[8] = { 2, 1, 2, 3, 3, 4, 4, 5 };
static const int k_ac3_rate[3] = { 48000, 44100, 32000 };

static ms_status parse_ac3(const uint8_t *data, size_t size, int is_csd,
                           ms_audio_cfg *out, ms_audio_au_info *au)
{
    unsigned fscod, frmsizecod, bsid, acmod, lfe;
    size_t need, bit, off;
    int rate, ch;
    uint32_t words;

    (void)is_csd;
    if (data == NULL || size < 7) {
        return MS_ERR_MALFORMED;
    }
    if (data[0] != 0x0B || data[1] != 0x77) {
        return MS_ERR_MALFORMED;
    }
    fscod = (unsigned)(data[4] >> 6) & 3u;
    frmsizecod = (unsigned)data[4] & 0x3Fu;
    bsid = (unsigned)(data[5] >> 3) & 0x1Fu;
    if (fscod > 2u || frmsizecod > 37u || bsid > 8u) {
        /* bsid>8 多为 E-AC-3，本切片不声称已覆盖 */
        return MS_ERR_MALFORMED;
    }
    acmod = (unsigned)(data[6] >> 5) & 7u;
    off = 8 + 16 + 2 + 6 + 5 + 3 + 3; /* 已消费到 acmod 末（从帧首 bit0） */
    /* data[6] 高 3 位是 acmod，余下位从 bit 3 起 */
    bit = 3;
    if ((acmod & 1u) != 0 && acmod != 1u) {
        bit += 2; /* cmixlev */
    }
    if ((acmod & 4u) != 0) {
        bit += 2; /* surmixlev */
    }
    if (acmod == 2u) {
        bit += 2; /* dsurmod */
    }
    if (bit >= 8u) {
        /* 头跨越 data[7]，7 字节下限可能不够 */
        if (size < 8) {
            return MS_ERR_MALFORMED;
        }
        lfe = (unsigned)((data[7] >> (15u - bit)) & 1u);
    } else {
        lfe = (unsigned)((data[6] >> (7u - bit)) & 1u);
    }
    (void)off;
    rate = k_ac3_rate[fscod];
    ch = k_ac3_acmod_ch[acmod] + (int)lfe;
    words = k_ac3_words[fscod][frmsizecod];
    need = (size_t)words * 2u;
    if (need < 7 || need > (1u << 16)) {
        return MS_ERR_OVERFLOW;
    }
    if (out != NULL) {
        out->codec = MS_CODEC_AC3;
        out->sample_fmt = MS_SAMPLE_FMT_AC3;
        out->sample_rate = rate;
        out->channels = ch;
        out->samples_per_frame = 1536;
        out->pre_skip = 0;
        out->codec_delay = 0;
        out->seek_preroll = 0;
        out->csd_size = 0; /* 合法无 CSD，不捏造 ASC */
        out->complete = 1;
        {
            uint8_t sigb[8];
            sigb[0] = (uint8_t)(rate & 0xFF);
            sigb[1] = (uint8_t)((rate >> 8) & 0xFF);
            sigb[2] = (uint8_t)((rate >> 16) & 0xFF);
            sigb[3] = (uint8_t)((rate >> 24) & 0xFF);
            sigb[4] = (uint8_t)ch;
            sigb[5] = (uint8_t)fscod;
            sigb[6] = (uint8_t)frmsizecod;
            sigb[7] = (uint8_t)acmod;
            out->payload_sig = abit_sig(sigb, sizeof(sigb));
        }
    }
    if (au != NULL) {
        au->has_cfg = 1;
        au->frame_bytes = need;
        au->samples = 1536;
        au->sample_rate = rate;
        au->channels = ch;
        if (size < need) {
            au->has_cfg = 1;
            return MS_ERR_MALFORMED; /* 截断帧：头可解析，但不是完整帧 */
        }
    }
    return MS_OK;
}

static const int k_mp_br_v1[3][16] = {
    /* layer I / II / III */
    { 0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448, 0 },
    { 0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384, 0 },
    { 0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0 }
};
static const int k_mp_br_v2[3][16] = {
    { 0, 32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256, 0 },
    { 0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0 },
    { 0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0 }
};
static const int k_mp_sr_v1[4] = { 44100, 48000, 32000, 0 };
static const int k_mp_sr_v2[4] = { 22050, 24000, 16000, 0 };
static const int k_mp_ch[4] = { 2, 2, 2, 1 }; /* stereo / joint / dual / mono */

/*
 * 码率/采样率查表：先核行、列上界再索引。layer 保留值不得落到「else=Layer III」
 * 这种隐式映射上。不用 signed (layer_n-1) 直接当下标。
 */
static int mpega_bitrate_kbps(int ver_id, unsigned row, unsigned bridx)
{
    const int (*tab)[16];
    if ((ver_id != 1 && ver_id != 2) || row > 2u || bridx >= 16u) {
        return 0;
    }
    tab = (ver_id == 1) ? k_mp_br_v1 : k_mp_br_v2;
    return tab[row][bridx];
}

static int mpega_sample_rate(int ver_id, unsigned sridx)
{
    if ((ver_id != 1 && ver_id != 2) || sridx >= 4u) {
        return 0;
    }
    return (ver_id == 1) ? k_mp_sr_v1[sridx] : k_mp_sr_v2[sridx];
}

/* 把头字段收到 0..3 / 0..15 的有界值；不足 4 字节即失败。 */
static int mpega_read_header(const uint8_t *data, size_t size, unsigned *ver,
                             unsigned *layer, unsigned *bridx, unsigned *sridx,
                             unsigned *pad, unsigned *mode)
{
    unsigned hdr;
    if (data == NULL || size < 4) {
        return -1;
    }
    if (data[0] != 0xFF || (data[1] & 0xE0) != 0xE0) {
        return -1;
    }
    hdr = ((unsigned)data[0] << 24) | ((unsigned)data[1] << 16) |
          ((unsigned)data[2] << 8) | (unsigned)data[3];
    *ver = (hdr >> 19) & 3u;
    *layer = (hdr >> 17) & 3u;
    *bridx = (hdr >> 12) & 0xFu;
    *sridx = (hdr >> 10) & 3u;
    *pad = (hdr >> 9) & 1u;
    *mode = (hdr >> 6) & 3u;
    return 0;
}

static ms_status parse_mpega(ms_codec want, const uint8_t *data, size_t size,
                             int is_csd, ms_audio_cfg *out, ms_audio_au_info *au)
{
    unsigned ver, layer, bridx, sridx, pad, mode, row;
    int br, sr, ch, layer_n, ver_id, spf;
    size_t need;

    (void)is_csd;
    if (mpega_read_header(data, size, &ver, &layer, &bridx, &sridx, &pad, &mode) != 0) {
        return MS_ERR_MALFORMED;
    }
    /* ver=1 保留；ver=0 为 MPEG-2.5，本切片不声称。layer=0 保留。
     * bridx 0=自由格式、15=坏值；sridx 3=保留。 */
    if (ver == 1u || ver == 0u || layer == 0u || bridx == 0u || bridx == 15u ||
        sridx == 3u) {
        return MS_ERR_MALFORMED;
    }
    /* 显式映射 ISO layer 字段到 1 基 Layer I/II/III 与 0 基表行，拒绝其它值。 */
    if (layer == 3u) {
        layer_n = 1;
        row = 0;
    } else if (layer == 2u) {
        layer_n = 2;
        row = 1;
    } else if (layer == 1u) {
        layer_n = 3;
        row = 2;
    } else {
        return MS_ERR_MALFORMED;
    }
    if (want == MS_CODEC_MP3 && layer_n != 3) {
        return MS_ERR_MALFORMED;
    }
    if (want == MS_CODEC_MP2 && layer_n != 2) {
        return MS_ERR_MALFORMED;
    }
    ver_id = (ver == 3u) ? 1 : 2; /* 此处 ver 只能是 2 或 3 */
    br = mpega_bitrate_kbps(ver_id, row, bridx) * 1000;
    sr = mpega_sample_rate(ver_id, sridx);
    if (br <= 0 || sr <= 0) {
        return MS_ERR_MALFORMED;
    }
    ch = k_mp_ch[mode];
    if (layer_n == 1) {
        need = (size_t)((12 * br) / sr + (int)pad) * 4u;
        spf = 384;
    } else if (layer_n == 2) {
        need = (size_t)((144 * br) / sr + (int)pad);
        spf = 1152;
    } else {
        if (ver_id == 1) {
            need = (size_t)((144 * br) / sr + (int)pad);
            spf = 1152;
        } else {
            need = (size_t)((72 * br) / sr + (int)pad);
            spf = 576;
        }
    }
    if (need < 4 || need > (1u << 16)) {
        return MS_ERR_OVERFLOW;
    }
    if (out != NULL) {
        out->codec = (layer_n == 3) ? MS_CODEC_MP3 : MS_CODEC_MP2;
        out->sample_fmt = MS_SAMPLE_FMT_MPEG_AUDIO;
        out->sample_rate = sr;
        out->channels = ch;
        out->samples_per_frame = (uint32_t)spf;
        out->pre_skip = 0;
        out->codec_delay = 0;
        out->seek_preroll = 0;
        out->csd_size = 0;
        out->complete = 1;
        out->payload_sig = abit_sig(data, 4);
    }
    if (au != NULL) {
        au->has_cfg = 1;
        au->frame_bytes = need;
        au->samples = (uint32_t)spf;
        au->sample_rate = sr;
        au->channels = ch;
        if (size < need) {
            return MS_ERR_MALFORMED;
        }
    }
    return MS_OK;
}

static int opus_toc_samples(uint8_t toc, uint32_t *samples, unsigned *nframes)
{
    unsigned cfg, c, i;
    static const uint16_t k_cfg_sp[32] = {
        480, 960, 1920, 2880, 480, 960, 1920, 2880, 480, 960, 1920, 2880,
        480, 960, 1920, 2880, 480, 960, 1920, 2880, 480, 960, 1920, 2880,
        120, 240, 480, 960, 120, 240, 480, 960
    };
    cfg = (unsigned)(toc >> 3) & 31u;
    c = (unsigned)toc & 3u;
    if (c == 0) {
        *nframes = 1;
    } else if (c == 1 || c == 2) {
        *nframes = 2;
    } else {
        return 0; /* code 3 需第二字节，本切片对完整 packet 在调用方再读 */
    }
    i = (unsigned)k_cfg_sp[cfg] * (*nframes);
    if (i == 0 || i > 5760) { /* 120 ms @ 48 kHz */
        return 0;
    }
    *samples = i;
    return 1;
}

static int parse_opus_head_bytes(const uint8_t *p, size_t n, int *ch,
                                 uint32_t *pre_skip, uint32_t *input_rate)
{
    if (p == NULL || n < 19) {
        return 0;
    }
    if (memcmp(p, "OpusHead", 8) != 0) {
        return 0;
    }
    if (p[8] != 1) {
        return 0;
    }
    *ch = (int)p[9];
    *pre_skip = (uint32_t)p[10] | ((uint32_t)p[11] << 8);
    *input_rate = (uint32_t)p[12] | ((uint32_t)p[13] << 8) |
                  ((uint32_t)p[14] << 16) | ((uint32_t)p[15] << 24);
    if (*ch <= 0 || *ch > 8) {
        return 0;
    }
    return 1;
}

static int parse_dops(const uint8_t *p, size_t n, int *ch, uint32_t *pre_skip,
                      uint32_t *input_rate)
{
    if (p == NULL || n < 11) {
        return 0;
    }
    if (p[0] != 0) { /* Version */
        return 0;
    }
    *ch = (int)p[1];
    *pre_skip = (uint32_t)p[2] | ((uint32_t)p[3] << 8);
    *input_rate = (uint32_t)p[4] | ((uint32_t)p[5] << 8) |
                  ((uint32_t)p[6] << 16) | ((uint32_t)p[7] << 24);
    if (*ch <= 0 || *ch > 8) {
        return 0;
    }
    return 1;
}

static ms_status pack_opus_head_from_dops(const uint8_t *dops, size_t n,
                                          uint8_t *dst, size_t cap, size_t *out_n)
{
    size_t need;
    if (dops == NULL || dst == NULL || out_n == NULL || n < 11) {
        return MS_ERR_MALFORMED;
    }
    need = 8 + n;
    if (need > cap || need > MS_AUDIO_CSD_MAX) {
        return MS_ERR_OVERFLOW;
    }
    memcpy(dst, "OpusHead", 8);
    memcpy(dst + 8, dops, n);
    /* dOps Version 写入 OpusHead version：规范 dOps 0 对应 OpusHead 1 */
    dst[8] = 1;
    *out_n = need;
    return MS_OK;
}

static ms_status parse_opus(const uint8_t *data, size_t size, int is_csd,
                            ms_audio_cfg *out, ms_audio_au_info *au)
{
    int ch = 0, have = 0;
    uint32_t pre = 0, irate = 0, samples = 0;
    unsigned nframes = 0;
    int from_head = 0, from_dops = 0;

    if (data == NULL || size == 0) {
        return MS_ERR_MALFORMED;
    }
    if (parse_opus_head_bytes(data, size, &ch, &pre, &irate)) {
        have = 1;
        from_head = 1;
    } else if (is_csd && parse_dops(data, size, &ch, &pre, &irate)) {
        have = 1;
        from_dops = 1;
    }
    if (out != NULL) {
        out->codec = MS_CODEC_OPUS;
        out->sample_fmt = MS_SAMPLE_FMT_OPUS;
        out->sample_rate = 48000; /* 解码时钟固定 48 kHz */
        out->channels = ch;
        out->samples_per_frame = 0;
        out->pre_skip = pre;
        out->codec_delay = pre;
        out->seek_preroll = 0;
        out->csd_size = 0;
        out->complete = 0;
        if (have) {
            out->complete = 1;
            out->seek_preroll = 3840; /* RFC 7845 / FFmpeg：80 ms @ 48 kHz，与 pre-skip 不是同一件事 */
            if (from_head) {
                if (size > MS_AUDIO_CSD_MAX) {
                    out->complete = 0;
                    return MS_ERR_OVERFLOW;
                }
                memcpy(out->csd, data, size);
                out->csd_size = size;
            } else if (from_dops) {
                ms_status st = pack_opus_head_from_dops(data, size, out->csd,
                                                         sizeof(out->csd),
                                                         &out->csd_size);
                if (st != MS_OK) {
                    out->complete = 0;
                    return st;
                }
            }
            out->payload_sig = abit_sig(out->csd, out->csd_size);
        }
    }
    if (au != NULL) {
        au->has_cfg = have;
        au->sample_rate = 48000;
        au->channels = ch;
        au->frame_bytes = size;
        au->samples = 0;
        if (!from_head && !from_dops) {
            uint8_t toc = data[0];
            unsigned c = (unsigned)toc & 3u;
            if (c == 3) {
                if (size < 2) {
                    return MS_ERR_MALFORMED;
                }
                nframes = (unsigned)data[1] & 0x3Fu;
                samples = (uint32_t)((toc >> 3) & 31u);
                /* 用表再乘帧数 */
                {
                    uint32_t one = 0;
                    unsigned dummy = 1;
                    if (!opus_toc_samples((uint8_t)(toc & ~3u), &one, &dummy)) {
                        return MS_ERR_MALFORMED;
                    }
                    if (nframes == 0 || nframes > 48) {
                        return MS_ERR_MALFORMED;
                    }
                    samples = one * nframes;
                    if (samples > 5760) {
                        return MS_ERR_OVERFLOW;
                    }
                }
            } else if (!opus_toc_samples(toc, &samples, &nframes)) {
                return MS_ERR_MALFORMED;
            }
            au->samples = samples;
            if (out != NULL && !out->complete) {
                /* 合法无 OpusHead：只能报帧长，不能捏造 Head，complete 仍 0 */
                out->samples_per_frame = samples;
            } else if (out != NULL) {
                out->samples_per_frame = samples;
            }
        }
    }
    if (is_csd) {
        return have ? MS_OK : MS_ERR_MALFORMED;
    }
    if (au != NULL && au->samples == 0 && !have) {
        return MS_ERR_MALFORMED;
    }
    if (au != NULL) {
        return MS_OK;
    }
    return have ? MS_OK : MS_ERR_MALFORMED;
}

static ms_status parse_aac(const uint8_t *data, size_t size, int is_csd,
                           ms_audio_cfg *out, ms_audio_au_info *au)
{
    ms_aac_asc_info info;
    if (!is_csd) {
        if (au != NULL) {
            au->has_cfg = 0;
            au->frame_bytes = size;
            au->samples = 1024;
            au->sample_rate = 0;
            au->channels = 0;
        }
        return (data != NULL && size > 0) ? MS_OK : MS_ERR_MALFORMED;
    }
    memset(&info, 0, sizeof(info));
    info.struct_size = sizeof(info);
    if (ms_aac_asc_parse(data, size, &info) != MS_OK) {
        return MS_ERR_MALFORMED;
    }
    if (out != NULL) {
        if (size > MS_AUDIO_CSD_MAX) {
            return MS_ERR_OVERFLOW;
        }
        out->codec = MS_CODEC_AAC;
        out->sample_fmt = MS_SAMPLE_FMT_AAC_RAW;
        out->sample_rate = info.sample_rate;
        out->channels = info.channels;
        out->samples_per_frame = 1024;
        out->pre_skip = 0;
        out->codec_delay = 0;
        out->seek_preroll = 0;
        memcpy(out->csd, data, size);
        out->csd_size = size;
        out->complete = 1;
        out->payload_sig = abit_sig(data, size);
    }
    if (au != NULL) {
        au->has_cfg = 1;
        au->frame_bytes = size;
        au->samples = 1024;
        au->sample_rate = info.sample_rate;
        au->channels = info.channels;
    }
    return MS_OK;
}


/* 容器带来的 PCM 参数块（'MSPM' + le32 rate + le16 ch + le16 bits），不是压缩软解。 */
static ms_status parse_pcm_csd(const uint8_t *data, size_t size, ms_audio_cfg *out)
{
    uint32_t magic, rate;
    uint16_t ch, bits;
    if (data == NULL || size < 12u) {
        return MS_ERR_MALFORMED;
    }
    magic = (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
            ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
    if (magic != 0x4d50534du) { /* 'MSPM' little-endian */
        return MS_ERR_MALFORMED;
    }
    rate = (uint32_t)data[4] | ((uint32_t)data[5] << 8) |
           ((uint32_t)data[6] << 16) | ((uint32_t)data[7] << 24);
    ch = (uint16_t)data[8] | (uint16_t)((uint16_t)data[9] << 8);
    bits = (uint16_t)data[10] | (uint16_t)((uint16_t)data[11] << 8);
    if (rate == 0 || ch == 0 || (bits != 8 && bits != 16 && bits != 24 && bits != 32)) {
        return MS_ERR_MALFORMED;
    }
    if (out != NULL) {
        out->complete = 1;
        out->sample_rate = (int)rate;
        out->channels = (int)ch;
        out->csd_size = 0;
        out->payload_sig = abit_sig(data, size);
    }
    (void)bits;
    return MS_OK;
}

/* 最小 STREAMINFO：可带 fLaC 头与 metadata block 头。只取采样率/声道，不是软解。 */
static ms_status parse_flac_csd(const uint8_t *data, size_t size, ms_audio_cfg *out)
{
    const uint8_t *p = data;
    size_t n = size;
    unsigned rate, ch;
    if (p == NULL || n < 34u) {
        return MS_ERR_MALFORMED;
    }
    if (n >= 42u && p[0] == 'f' && p[1] == 'L' && p[2] == 'a' && p[3] == 'C') {
        p += 4;
        n -= 4;
    }
    if (n >= 38u && (p[0] & 0x7fu) == 0) {
        p += 4;
        n -= 4;
    }
    if (n < 34u) {
        return MS_ERR_MALFORMED;
    }
    rate = ((unsigned)p[10] << 12) | ((unsigned)p[11] << 4) | ((unsigned)p[12] >> 4);
    ch = ((unsigned)(p[12] >> 1) & 7u) + 1u;
    if (rate == 0 || ch == 0) {
        return MS_ERR_MALFORMED;
    }
    if (out != NULL) {
        size_t csz = size > sizeof(out->csd) ? sizeof(out->csd) : size;
        out->complete = 1;
        out->sample_rate = (int)rate;
        out->channels = (int)ch;
        memcpy(out->csd, data, csz);
        out->csd_size = csz;
        out->payload_sig = abit_sig(data, size);
    }
    return MS_OK;
}

ms_status ms_audio_cfg_parse(ms_codec codec, ms_sample_format fmt, int is_csd,
                             const uint8_t *data, size_t size, ms_audio_cfg *out)
{
    if (out == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    if (out->struct_size != 0 && out->struct_size < sizeof(*out)) {
        return MS_ERR_INVALID_ARG;
    }
    {
        size_t caller_sz = out->struct_size != 0 ? out->struct_size : sizeof(*out);
        memset(out, 0, sizeof(*out));
        out->struct_size = caller_sz < sizeof(*out) ? sizeof(*out) : caller_sz;
        out->codec = codec;
        out->sample_fmt = fmt;
    }
    if (data == NULL && size > 0) {
        return MS_ERR_INVALID_ARG;
    }
    if (size == 0) {
        return MS_ERR_MALFORMED;
    }
    if (codec == MS_CODEC_AAC) {
        return parse_aac(data, size, is_csd, out, NULL);
    }
    if (codec == MS_CODEC_AC3) {
        return parse_ac3(data, size, is_csd, out, NULL);
    }
    if (codec == MS_CODEC_MP3 || codec == MS_CODEC_MP2) {
        return parse_mpega(codec, data, size, is_csd, out, NULL);
    }
    if (codec == MS_CODEC_OPUS) {
        return parse_opus(data, size, is_csd, out, NULL);
    }
    if (codec == MS_CODEC_PCM) {
        return parse_pcm_csd(data, size, out);
    }
    if (codec == MS_CODEC_FLAC) {
        return parse_flac_csd(data, size, out);
    }
    return MS_ERR_UNSUPPORTED;
}

ms_status ms_audio_au_inspect(ms_codec codec, ms_sample_format fmt,
                              const uint8_t *data, size_t size,
                              ms_audio_au_info *out)
{
    ms_status st;
    (void)fmt;
    if (out == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    out->struct_size = sizeof(*out);
    if (data == NULL && size > 0) {
        return MS_ERR_INVALID_ARG;
    }
    if (size == 0) {
        return MS_ERR_MALFORMED;
    }
    if (codec == MS_CODEC_AAC) {
        return parse_aac(data, size, 0, NULL, out);
    }
    if (codec == MS_CODEC_AC3) {
        st = parse_ac3(data, size, 0, NULL, out);
        return st;
    }
    if (codec == MS_CODEC_MP3 || codec == MS_CODEC_MP2) {
        return parse_mpega(codec, data, size, 0, NULL, out);
    }
    if (codec == MS_CODEC_OPUS) {
        return parse_opus(data, size, 0, NULL, out);
    }
    return MS_ERR_UNSUPPORTED;
}
