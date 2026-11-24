/*
 * 通用视频配置 / 完整 AU / 关键帧判断（MPEG-2 Video、MPEG-4 Part 2、H.263、AV1；
 * H.264/HEVC 仍用既有解析，避免平行复制门控）。
 * 不是软解，不栅格化图像。by: Long <mycjzb@gmail.com>
 */
#include "msmedia/msmedia.h"

#include <string.h>

void ms_video_cfg_default(ms_video_cfg *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->struct_size = sizeof(*out);
}

int ms_video_hw_path_ready(ms_codec codec)
{
    return codec == MS_CODEC_H264 || codec == MS_CODEC_HEVC ||
           codec == MS_CODEC_MPEG2_VIDEO || codec == MS_CODEC_MPEG4_PART2 ||
           codec == MS_CODEC_H263 || codec == MS_CODEC_AV1 ||
           codec == MS_CODEC_AVS_PLUS;
}

int ms_video_cfg_equal(const ms_video_cfg *a, const ms_video_cfg *b)
{
    if (a == NULL || b == NULL) {
        return 0;
    }
    if (a->codec != b->codec || a->complete != b->complete) {
        return 0;
    }
    if (a->width != b->width || a->height != b->height) {
        return 0;
    }
    if (a->payload_sig != b->payload_sig || a->csd_size != b->csd_size) {
        return 0;
    }
    if (a->csd_size > sizeof(a->csd) || b->csd_size > sizeof(b->csd)) {
        return 0;
    }
    if (a->csd_size == 0) {
        return 1;
    }
    return memcmp(a->csd, b->csd, a->csd_size) == 0;
}

static uint32_t payload_sig(const uint8_t *p, size_t n)
{
    uint32_t h = 2166136261u;
    size_t i;
    if (p == NULL) {
        return 0;
    }
    for (i = 0; i < n; i++) {
        h ^= (uint32_t)p[i];
        h *= 16777619u;
    }
    return h == 0 ? 1u : h;
}

static int append_sc(uint8_t *dst, size_t cap, size_t *used,
                     const uint8_t *payload, size_t psz, unsigned sc)
{
    size_t need;
    if (dst == NULL || used == NULL || payload == NULL) {
        return -1;
    }
    need = 4u + psz;
    if (*used > cap || need > cap - *used) {
        return -1;
    }
    dst[*used] = 0;
    dst[*used + 1] = 0;
    dst[*used + 2] = 1;
    dst[*used + 3] = (uint8_t)sc;
    memcpy(dst + *used + 4, payload, psz);
    *used += need;
    return 0;
}

/* 在 [off, size) 找起始码 00 00 01 xx，兼容 00 00 00 01。返回载荷起点；*sc 为 xx。 */
static int find_start(const uint8_t *data, size_t size, size_t off,
                      unsigned *sc, size_t *payload, size_t *next)
{
    size_t i;
    if (data == NULL || sc == NULL || payload == NULL || next == NULL) {
        return 0;
    }
    if (off >= size || size - off < 4) {
        return 0;
    }
    for (i = off; i + 3 < size; i++) {
        if (data[i] != 0 || data[i + 1] != 0) {
            continue;
        }
        if (data[i + 2] == 1) {
            *sc = data[i + 3];
            *payload = i + 4;
        } else if (data[i + 2] == 0 && i + 4 < size && data[i + 3] == 1) {
            *sc = data[i + 4];
            *payload = i + 5;
        } else {
            continue;
        }
        *next = *payload;
        /* 只以 00 00 01 为下一起始码。前一单元若以 0 收尾，后接 00 00 01
         * 会形成 00 00 00 01，不得把末尾 0 从载荷里剥走。 */
        while (*next + 2 < size) {
            if (data[*next] == 0 && data[*next + 1] == 0 && data[*next + 2] == 1) {
                break;
            }
            (*next)++;
        }
        if (*next + 2 >= size) {
            *next = size;
        }
        return 1;
    }
    return 0;
}

typedef struct {
    const uint8_t *p;
    size_t n;
    size_t bit;
} bitr;

static int br_need(const bitr *b, unsigned bits)
{
    if (b == NULL || bits > 64) {
        return 0;
    }
    if (b->bit > b->n * 8u) {
        return 0;
    }
    return (b->n * 8u - b->bit) >= bits;
}

static unsigned br_get(bitr *b, unsigned bits)
{
    unsigned v = 0;
    unsigned i;
    if (b == NULL || bits == 0 || bits > 32 || !br_need(b, bits)) {
        return 0;
    }
    for (i = 0; i < bits; i++) {
        size_t byte = b->bit / 8u;
        unsigned sh = 7u - (unsigned)(b->bit % 8u);
        v = (v << 1) | (unsigned)((b->p[byte] >> sh) & 1u);
        b->bit++;
    }
    return v;
}

static int mpeg2_parse_seq(const uint8_t *payload, size_t psz,
                           int *w, int *h, int *ok_basic)
{
    bitr b;
    unsigned hor, ver;
    if (payload == NULL || psz < 8 || w == NULL || h == NULL || ok_basic == NULL) {
        return 0;
    }
    b.p = payload;
    b.n = psz;
    b.bit = 0;
    if (!br_need(&b, 64)) {
        return 0;
    }
    hor = br_get(&b, 12);
    ver = br_get(&b, 12);
    (void)br_get(&b, 4);  /* aspect */
    (void)br_get(&b, 4);  /* frame_rate */
    (void)br_get(&b, 18); /* bit_rate */
    if (br_get(&b, 1) != 1) {
        return 0; /* marker */
    }
    (void)br_get(&b, 10); /* vbv */
    (void)br_get(&b, 1);  /* constrained */
    if (br_get(&b, 1)) {  /* load intra quant */
        if (!br_need(&b, 64 * 8)) {
            return 0;
        }
        b.bit += 64u * 8u;
    }
    if (br_get(&b, 1)) {  /* load non-intra */
        if (!br_need(&b, 64 * 8)) {
            return 0;
        }
        b.bit += 64u * 8u;
    }
    if (hor == 0 || ver == 0 || hor > 16384 || ver > 16384) {
        return 0;
    }
    *w = (int)hor;
    *h = (int)ver;
    *ok_basic = 1;
    return 1;
}

static int mpeg2_parse_seq_ext(const uint8_t *payload, size_t psz,
                               int *w, int *h)
{
    bitr b;
    unsigned id, hx, vx;
    if (payload == NULL || psz < 2 || w == NULL || h == NULL) {
        return 0;
    }
    b.p = payload;
    b.n = psz;
    b.bit = 0;
    if (!br_need(&b, 4 + 8 + 1 + 2 + 2 + 2)) {
        return 0;
    }
    id = br_get(&b, 4);
    if (id != 1) { /* sequence_extension */
        return 0;
    }
    (void)br_get(&b, 8); /* profile/level */
    (void)br_get(&b, 1); /* progressive */
    (void)br_get(&b, 2); /* chroma */
    hx = br_get(&b, 2);
    vx = br_get(&b, 2);
    *w = (*w & 0x0FFF) | (int)(hx << 12);
    *h = (*h & 0x0FFF) | (int)(vx << 12);
    return 1;
}

static int mpeg2_pic_type(const uint8_t *payload, size_t psz, ms_vpic_type *t)
{
    bitr b;
    unsigned typ;
    if (payload == NULL || t == NULL || psz < 2) {
        return 0;
    }
    b.p = payload;
    b.n = psz;
    b.bit = 0;
    if (!br_need(&b, 13)) {
        return 0;
    }
    (void)br_get(&b, 10);
    typ = br_get(&b, 3);
    if (typ == 1) {
        *t = MS_VPIC_I;
    } else if (typ == 2) {
        *t = MS_VPIC_P;
    } else if (typ == 3) {
        *t = MS_VPIC_B;
    } else {
        *t = MS_VPIC_UNKNOWN;
        return 0;
    }
    return 1;
}

static int mpeg4_vol_wh(const uint8_t *payload, size_t psz, int *w, int *h)
{
    bitr b;
    unsigned typ, ident, verid, ar, ctrl, vbv, shape, inc;
    unsigned bits;
    if (payload == NULL || w == NULL || h == NULL || psz < 5) {
        return 0;
    }
    b.p = payload;
    b.n = psz;
    b.bit = 0;
    if (!br_need(&b, 1 + 8 + 1)) {
        return 0;
    }
    (void)br_get(&b, 1); /* random_accessible_vol */
    typ = br_get(&b, 8);
    if (typ == 0x12) {
        return 0; /* FGS 不解析 */
    }
    ident = br_get(&b, 1);
    verid = 1;
    if (ident) {
        if (!br_need(&b, 7)) {
            return 0;
        }
        verid = br_get(&b, 4);
        (void)br_get(&b, 3);
    }
    if (!br_need(&b, 4)) {
        return 0;
    }
    ar = br_get(&b, 4);
    if (ar == 15) {
        if (!br_need(&b, 16)) {
            return 0;
        }
        (void)br_get(&b, 8);
        (void)br_get(&b, 8);
    }
    if (!br_need(&b, 1)) {
        return 0;
    }
    ctrl = br_get(&b, 1);
    if (ctrl) {
        if (!br_need(&b, 2 + 1 + 1)) {
            return 0;
        }
        (void)br_get(&b, 2);
        (void)br_get(&b, 1);
        vbv = br_get(&b, 1);
        if (vbv) {
            /* first_half 15 + marker + latter 15 + marker + first_half vbv 3 +
             * latter 11 + marker  — ISO 14496-2 */
            if (!br_need(&b, 15 + 1 + 15 + 1 + 3 + 11 + 1)) {
                return 0;
            }
            (void)br_get(&b, 15);
            if (br_get(&b, 1) != 1) {
                return 0;
            }
            (void)br_get(&b, 15);
            if (br_get(&b, 1) != 1) {
                return 0;
            }
            (void)br_get(&b, 3);
            (void)br_get(&b, 11);
            if (br_get(&b, 1) != 1) {
                return 0;
            }
        }
    }
    if (!br_need(&b, 2)) {
        return 0;
    }
    shape = br_get(&b, 2);
    if (shape != 0) { /* 仅矩形 */
        return 0;
    }
    if (verid != 1 && typ != 1 && typ != 2) {
        /* 可跳过；矩形 shape 仍继续 */
    }
    if (!br_need(&b, 1 + 16 + 1 + 1)) {
        return 0;
    }
    if (br_get(&b, 1) != 1) { /* marker */
        return 0;
    }
    inc = br_get(&b, 16);
    if (br_get(&b, 1) != 1) {
        return 0;
    }
    if (br_get(&b, 1)) { /* fixed_vop_rate */
        bits = 1;
        if (inc > 1) {
            unsigned t = inc - 1;
            bits = 0;
            while (t > 0) {
                bits++;
                t >>= 1;
            }
        }
        if (bits == 0) {
            bits = 1;
        }
        if (!br_need(&b, bits)) {
            return 0;
        }
        (void)br_get(&b, bits);
    }
    if (!br_need(&b, 1 + 13 + 1 + 13 + 1)) {
        return 0;
    }
    if (br_get(&b, 1) != 1) {
        return 0;
    }
    *w = (int)br_get(&b, 13);
    if (br_get(&b, 1) != 1) {
        return 0;
    }
    *h = (int)br_get(&b, 13);
    if (br_get(&b, 1) != 1) {
        return 0;
    }
    if (*w <= 0 || *h <= 0 || *w > 16384 || *h > 16384) {
        return 0;
    }
    return 1;
}

static int mpeg4_vop_type(const uint8_t *payload, size_t psz, ms_vpic_type *t)
{
    unsigned typ;
    if (payload == NULL || t == NULL || psz < 1) {
        return 0;
    }
    typ = (unsigned)(payload[0] >> 6);
    if (typ == 0) {
        *t = MS_VPIC_I;
    } else if (typ == 1) {
        *t = MS_VPIC_P;
    } else if (typ == 2) {
        *t = MS_VPIC_B;
    } else {
        *t = MS_VPIC_S;
    }
    return 1;
}

static int mpeg2_keep_sc(unsigned sc, const uint8_t *payload, size_t psz)
{
    unsigned id;
    if (sc == 0xB3) {
        return 1;
    }
    if (sc != 0xB5 || payload == NULL || psz < 1) {
        return 0;
    }
    id = payload[0] >> 4;
    /* 序列级扩展：sequence / display / quant / scalable */
    return id == 1 || id == 2 || id == 3 || id == 5;
}

static int mpeg4_keep_sc(unsigned sc)
{
    if (sc == 0xB0 || sc == 0xB2 || sc == 0xB5) {
        return 1;
    }
    if (sc <= 0x1F) { /* visual object */
        return 1;
    }
    if (sc >= 0x20 && sc <= 0x2F) {
        return 1;
    }
    return 0;
}

static ms_status parse_mpeg2(int is_csd, const uint8_t *data, size_t size,
                             ms_video_cfg *out, ms_video_au_info *au)
{
    size_t off = 0, pay = 0, nxt = 0, used = 0;
    unsigned sc = 0;
    int have_seq = 0, have_ext = 0, ok_basic = 0;
    int w = 0, h = 0, overflow = 0, saw_any = 0;
    ms_vpic_type pic = MS_VPIC_UNKNOWN;
    int saw_pic = 0;
    uint8_t csd[MS_VIDEO_CSD_MAX];

    memset(csd, 0, sizeof(csd));
    while (find_start(data, size, off, &sc, &pay, &nxt)) {
        size_t psz;
        saw_any = 1;
        if (pay > nxt) {
            break;
        }
        psz = nxt - pay;
        if (sc == 0xB3) {
            if (mpeg2_parse_seq(data + pay, psz, &w, &h, &ok_basic)) {
                have_seq = 1;
            }
            if (append_sc(csd, sizeof(csd), &used, data + pay, psz, sc) != 0) {
                overflow = 1;
            }
        } else if (sc == 0xB5 && mpeg2_keep_sc(sc, data + pay, psz)) {
            if ((data[pay] >> 4) == 1) {
                if (mpeg2_parse_seq_ext(data + pay, psz, &w, &h)) {
                    have_ext = 1;
                }
            }
            if (have_seq) {
                if (append_sc(csd, sizeof(csd), &used, data + pay, psz, sc) != 0) {
                    overflow = 1;
                }
            }
        } else if (sc == 0x00) {
            if (mpeg2_pic_type(data + pay, psz, &pic)) {
                saw_pic = 1;
            }
            if (is_csd) {
                break;
            }
        } else if (is_csd && (sc == 0xB8 || (sc >= 0x01 && sc <= 0xAF))) {
            break;
        }
        off = (nxt > off) ? nxt : (off + 1);
    }
    if (out != NULL) {
        out->codec = MS_CODEC_MPEG2_VIDEO;
        out->sample_fmt = MS_SAMPLE_FMT_MPV;
        out->width = w;
        out->height = h;
        if (overflow) {
            out->complete = 0;
            out->csd_size = 0;
            out->payload_sig = 0;
            return MS_ERR_OVERFLOW;
        }
        if (have_seq && used > 0 && used <= sizeof(out->csd)) {
            memcpy(out->csd, csd, used);
            out->csd_size = used;
            out->payload_sig = payload_sig(out->csd, out->csd_size);
            /* 序列头即可配置多数硬解；有 sequence_extension 更完整。 */
            out->complete = 1;
            (void)have_ext;
            (void)ok_basic;
        }
    }
    if (au != NULL) {
        au->has_cfg = have_seq;
        au->pic_type = pic;
        au->is_key = (pic == MS_VPIC_I);
    }
    if (!saw_any && size > 0 && !is_csd) {
        return MS_ERR_MALFORMED;
    }
    if (is_csd && out != NULL && !out->complete) {
        return (size > 0 && !have_seq) ? MS_ERR_MALFORMED : MS_ERR_MALFORMED;
    }
    (void)saw_pic;
    return MS_OK;
}

static ms_status parse_mpeg4(int is_csd, const uint8_t *data, size_t size,
                             ms_video_cfg *out, ms_video_au_info *au)
{
    size_t off = 0, pay = 0, nxt = 0, used = 0;
    unsigned sc = 0;
    int have_vol = 0, overflow = 0, saw_any = 0, w = 0, h = 0;
    ms_vpic_type pic = MS_VPIC_UNKNOWN;
    uint8_t csd[MS_VIDEO_CSD_MAX];

    memset(csd, 0, sizeof(csd));
    while (find_start(data, size, off, &sc, &pay, &nxt)) {
        size_t psz;
        saw_any = 1;
        if (pay > nxt) {
            break;
        }
        psz = nxt - pay;
        if (sc == 0xB6) {
            (void)mpeg4_vop_type(data + pay, psz, &pic);
            break;
        }
        if (sc == 0xB3 || sc == 0xB1) { /* GOV / VOS end：配置到此结束 */
            if (is_csd) {
                break;
            }
            off = (nxt > off) ? nxt : (off + 1);
            continue;
        }
        if (mpeg4_keep_sc(sc)) {
            if (sc >= 0x20 && sc <= 0x2F) {
                if (mpeg4_vol_wh(data + pay, psz, &w, &h)) {
                    have_vol = 1;
                } else {
                    /* VOL 起始码在但宽高解析失败：仍保留为配置，宽高为 0 */
                    have_vol = 1;
                }
            }
            if (append_sc(csd, sizeof(csd), &used, data + pay, psz, sc) != 0) {
                overflow = 1;
            }
        }
        off = (nxt > off) ? nxt : (off + 1);
    }
    if (out != NULL) {
        out->codec = MS_CODEC_MPEG4_PART2;
        out->sample_fmt = MS_SAMPLE_FMT_MP4V;
        out->width = w;
        out->height = h;
        if (overflow) {
            out->complete = 0;
            out->csd_size = 0;
            out->payload_sig = 0;
            return MS_ERR_OVERFLOW;
        }
        if (have_vol && used > 0 && used <= sizeof(out->csd)) {
            memcpy(out->csd, csd, used);
            out->csd_size = used;
            out->payload_sig = payload_sig(out->csd, out->csd_size);
            out->complete = 1;
        }
    }
    if (au != NULL) {
        au->has_cfg = have_vol;
        au->pic_type = pic;
        au->is_key = (pic == MS_VPIC_I);
    }
    if (!saw_any && size > 0) {
        return MS_ERR_MALFORMED;
    }
    if (is_csd && out != NULL && !out->complete) {
        return (size > 0) ? MS_ERR_MALFORMED : MS_ERR_MALFORMED;
    }
    return MS_OK;
}


/* ITU-T H.263 短视频头：PSC 22 bit = 0x20，配置在图象头，不是独立 CSD。
 * MediaCodec video/3gpp 通常不需要 csd-0，不得把图象头捏造成 CSD。 */
static int h263_size_from_fmt(unsigned fmt, int *w, int *h)
{
    switch (fmt) {
    case 1:
        *w = 128;
        *h = 96;
        return 1; /* SQCIF */
    case 2:
        *w = 176;
        *h = 144;
        return 1; /* QCIF */
    case 3:
        *w = 352;
        *h = 288;
        return 1; /* CIF */
    case 4:
        *w = 704;
        *h = 576;
        return 1; /* 4CIF */
    case 5:
        *w = 1408;
        *h = 1152;
        return 1; /* 16CIF */
    default:
        return 0; /* 111=PLUSPTYPE 等本切片不声称已覆盖 */
    }
}

static int find_h263_psc(const uint8_t *data, size_t size, size_t *off)
{
    size_t i;
    if (data == NULL || off == NULL || size < 6) {
        return 0;
    }
    for (i = 0; i + 5 < size; i++) {
        if (data[i] == 0 && data[i + 1] == 0 && (data[i + 2] & 0xFCu) == 0x80u) {
            *off = i;
            return 1;
        }
    }
    return 0;
}

static uint32_t dim_sig(int w, int h)
{
    uint8_t b[8];
    b[0] = (uint8_t)(w & 0xFF);
    b[1] = (uint8_t)((w >> 8) & 0xFF);
    b[2] = (uint8_t)((w >> 16) & 0xFF);
    b[3] = (uint8_t)((w >> 24) & 0xFF);
    b[4] = (uint8_t)(h & 0xFF);
    b[5] = (uint8_t)((h >> 8) & 0xFF);
    b[6] = (uint8_t)((h >> 16) & 0xFF);
    b[7] = (uint8_t)((h >> 24) & 0xFF);
    return payload_sig(b, sizeof(b));
}

static ms_status parse_h263(int is_csd, const uint8_t *data, size_t size,
                            ms_video_cfg *out, ms_video_au_info *au)
{
    bitr b;
    size_t off = 0;
    unsigned psc, tr, marker, ign, fmt, ptype;
    int w = 0, h = 0, have = 0;
    ms_vpic_type pic = MS_VPIC_UNKNOWN;

    (void)is_csd;
    if (data == NULL || size == 0) {
        return MS_ERR_MALFORMED;
    }
    if (!find_h263_psc(data, size, &off)) {
        return MS_ERR_MALFORMED;
    }
    b.p = data + off;
    b.n = size - off;
    b.bit = 0;
    if (!br_need(&b, 22 + 8 + 2 + 3 + 3 + 1)) {
        return MS_ERR_MALFORMED;
    }
    psc = br_get(&b, 22);
    if (psc != 0x20u) {
        return MS_ERR_MALFORMED;
    }
    tr = br_get(&b, 8);
    (void)tr;
    marker = br_get(&b, 2);
    if (marker != 2u) { /* PTYPE bits 1-2 固定为 10 */
        return MS_ERR_MALFORMED;
    }
    ign = br_get(&b, 3);
    (void)ign;
    fmt = br_get(&b, 3);
    ptype = br_get(&b, 1);
    if (h263_size_from_fmt(fmt, &w, &h)) {
        have = 1;
        pic = (ptype == 0) ? MS_VPIC_I : MS_VPIC_P;
    } else {
        /* PLUSPTYPE/自定义尺寸：不捏造宽高或 CSD，complete=0。 */
        pic = MS_VPIC_UNKNOWN;
    }
    if (out != NULL) {
        out->codec = MS_CODEC_H263;
        out->sample_fmt = MS_SAMPLE_FMT_H263;
        out->width = w;
        out->height = h;
        out->csd_size = 0; /* 合法无 CSD，不把 PSC 当 MediaCodec csd-0 */
        out->complete = have;
        out->payload_sig = have ? dim_sig(w, h) : 0;
    }
    if (au != NULL) {
        au->has_cfg = have;
        au->pic_type = pic;
        au->is_key = (pic == MS_VPIC_I);
    }
    return have ? MS_OK : MS_ERR_MALFORMED;
}

#define AV1_OBU_SEQ 1
#define AV1_OBU_TD 2
#define AV1_OBU_FRAME_HDR 3
#define AV1_OBU_FRAME 6
#define AV1_OBU_MAX 128

static int av1_leb128(const uint8_t *p, size_t n, size_t *used, uint32_t *out)
{
    size_t i;
    uint32_t v = 0;
    if (p == NULL || used == NULL || out == NULL) {
        return 0;
    }
    *used = 0;
    *out = 0;
    for (i = 0; i < 8; i++) {
        uint8_t b;
        if (i >= n) {
            return 0;
        }
        b = p[i];
        if (i == 4 && (b & 0x7Fu) > 0x0Fu && (v >> 28) != 0) {
            /* 超过 32-bit 载荷长度：当作溢出 */
            return -1;
        }
        v |= (uint32_t)(b & 0x7Fu) << (i * 7);
        if ((b & 0x80u) == 0) {
            *used = i + 1;
            *out = v;
            return 1;
        }
    }
    return -1;
}

static int av1_uvlc(bitr *b, uint32_t *out)
{
    unsigned z = 0;
    uint32_t extra;
    if (b == NULL || out == NULL) {
        return 0;
    }
    while (z < 32u) {
        if (!br_need(b, 1)) {
            return 0;
        }
        if (br_get(b, 1) == 1u) {
            break;
        }
        z++;
    }
    if (z >= 32u) {
        return 0;
    }
    if (z == 0) {
        *out = 0;
        return 1;
    }
    if (!br_need(b, z)) {
        return 0;
    }
    extra = br_get(b, z);
    *out = ((uint32_t)1u << z) - 1u + extra;
    return 1;
}

static int av1_parse_seq(const uint8_t *p, size_t n, int *w, int *h)
{
    bitr b;
    unsigned profile, still, reduced, timing, delay, opcnt, i;
    unsigned wb, hb, mw, mh;
    uint32_t dummy = 0;

    if (p == NULL || n == 0 || w == NULL || h == NULL) {
        return 0;
    }
    *w = 0;
    *h = 0;
    b.p = p;
    b.n = n;
    b.bit = 0;
    if (!br_need(&b, 3 + 1 + 1)) {
        return 0;
    }
    profile = br_get(&b, 3);
    still = br_get(&b, 1);
    reduced = br_get(&b, 1);
    (void)still;
    (void)profile;
    if (reduced) {
        if (!br_need(&b, 5 + 4 + 4)) {
            return 0;
        }
        (void)br_get(&b, 5); /* seq_level_idx[0] */
    } else {
        if (!br_need(&b, 1 + 1 + 5)) {
            return 0;
        }
        unsigned dec_model = 0;
        timing = br_get(&b, 1);
        if (timing) {
            unsigned equal;
            if (!br_need(&b, 32 + 32 + 1)) {
                return 0;
            }
            (void)br_get(&b, 32);
            (void)br_get(&b, 32);
            equal = br_get(&b, 1);
            if (equal && !av1_uvlc(&b, &dummy)) {
                return 0;
            }
            if (!br_need(&b, 1)) {
                return 0;
            }
            dec_model = br_get(&b, 1);
            if (dec_model) {
                if (!br_need(&b, 5 + 32 + 5 + 5)) {
                    return 0;
                }
                (void)br_get(&b, 5);
                (void)br_get(&b, 32);
                (void)br_get(&b, 5);
                (void)br_get(&b, 5);
            }
        }
        if (!br_need(&b, 1 + 5)) {
            return 0;
        }
        delay = br_get(&b, 1);
        opcnt = br_get(&b, 5);
        for (i = 0; i <= opcnt; i++) {
            unsigned level, tier_need;
            if (!br_need(&b, 12 + 5)) {
                return 0;
            }
            (void)br_get(&b, 12);
            level = br_get(&b, 5);
            tier_need = (level > 7u);
            if (tier_need) {
                if (!br_need(&b, 1)) {
                    return 0;
                }
                (void)br_get(&b, 1);
            }
            /* decoder_model / initial_display_delay 本切片不完整跳过字段，保守失败以免错读宽高。 */
            (void)timing;
            if (dec_model || delay) {
                return 0;
            }
        }
    }
    if (!br_need(&b, 4 + 4)) {
        return 0;
    }
    wb = br_get(&b, 4) + 1u;
    hb = br_get(&b, 4) + 1u;
    if (wb > 16u || hb > 16u || !br_need(&b, wb + hb)) {
        return 0;
    }
    mw = br_get(&b, wb);
    mh = br_get(&b, hb);
    if (mw > 65534u || mh > 65534u) {
        return 0;
    }
    *w = (int)mw + 1;
    *h = (int)mh + 1;
    return (*w > 0 && *h > 0);
}

static int av1_frame_is_key(const uint8_t *p, size_t n, int reduced)
{
    bitr b;
    unsigned show_existing, ftype;
    if (p == NULL || n == 0) {
        return 0;
    }
    if (reduced) {
        return 1;
    }
    b.p = p;
    b.n = n;
    b.bit = 0;
    if (!br_need(&b, 1 + 2)) {
        return 0;
    }
    show_existing = br_get(&b, 1);
    if (show_existing) {
        return 0; /* 不把 show_existing 当新的随机接入关键帧 */
    }
    ftype = br_get(&b, 2);
    return ftype == 0u || ftype == 3u; /* KEY or SWITCH */
}

typedef struct {
    int have_sh;
    int w;
    int h;
    int is_key;
    int have_frame;
    int tu;
    int fu;
    int overflow;
    const uint8_t *sh;
    size_t sh_n;
} av1_walk;

static int av1_walk_obus(const uint8_t *data, size_t size, av1_walk *st)
{
    size_t off = 0;
    unsigned n_obu = 0;
    int seen_fh = 0;
    if (data == NULL || st == NULL) {
        return 0;
    }
    memset(st, 0, sizeof(*st));
    while (off < size) {
        unsigned hdr, type, ext, has_size;
        size_t used = 0, payload_off, payload_n;
        uint32_t leb = 0;
        int leb_st;
        if (n_obu >= AV1_OBU_MAX) {
            st->overflow = 1;
            return 0;
        }
        hdr = data[off];
        off++;
        if ((hdr & 0x80u) != 0) { /* forbidden */
            return 0;
        }
        type = (hdr >> 3) & 0x0Fu;
        ext = (hdr >> 2) & 1u;
        has_size = (hdr >> 1) & 1u;
        if (ext) {
            if (off >= size) {
                return 0;
            }
            off++; /* temporal/spatial id */
        }
        if (has_size) {
            leb_st = av1_leb128(data + off, size - off, &used, &leb);
            if (leb_st == -1) {
                st->overflow = 1;
                return 0;
            }
            if (leb_st != 1) {
                return 0;
            }
            off += used;
            if ((size_t)leb > size - off) {
                return 0;
            }
            payload_off = off;
            payload_n = (size_t)leb;
            off += payload_n;
        } else {
            payload_off = off;
            payload_n = size - off;
            off = size;
        }
        n_obu++;
        if (type == AV1_OBU_TD) {
            st->tu++;
            seen_fh = 0;
        } else if (type == AV1_OBU_SEQ) {
            st->have_sh = 1;
            st->sh = data + payload_off;
            st->sh_n = payload_n;
            if (!av1_parse_seq(st->sh, st->sh_n, &st->w, &st->h)) {
                st->have_sh = 0;
                st->w = 0;
                st->h = 0;
            }
        } else if (type == AV1_OBU_FRAME_HDR || type == AV1_OBU_FRAME) {
            if (av1_frame_is_key(data + payload_off, payload_n, 0)) {
                st->is_key = 1;
            }
            st->have_frame = 1;
            if (!seen_fh) {
                st->fu++;
                seen_fh = 1;
            }
            if (type == AV1_OBU_FRAME) {
                seen_fh = 0;
            }
        }
    }
    if (st->tu == 0 && (st->have_frame || st->have_sh)) {
        st->tu = 1;
    }
    if (st->fu == 0 && st->have_frame) {
        st->fu = 1;
    }
    return 1;
}

static int av1_looks_like_av1c(const uint8_t *data, size_t size)
{
    if (data == NULL || size < 4) {
        return 0;
    }
    /* marker=1, version=1 的常见 av1C；拒绝把普通 SH OBU（常以 0x0a 起）当成 av1C。 */
    return data[0] == 0x81u;
}

static ms_status parse_av1(int is_csd, const uint8_t *data, size_t size,
                           ms_video_cfg *out, ms_video_au_info *au)
{
    av1_walk w;
    const uint8_t *obus = data;
    size_t obu_n = size;
    int from_av1c = 0;
    int ok_walk;
    int complete;

    if (data == NULL || size == 0) {
        return MS_ERR_MALFORMED;
    }
    if (is_csd || av1_looks_like_av1c(data, size)) {
        if (!av1_looks_like_av1c(data, size)) {
            return MS_ERR_MALFORMED;
        }
        if (size > MS_VIDEO_CSD_MAX) {
            if (out != NULL) {
                out->complete = 0;
            }
            return MS_ERR_OVERFLOW;
        }
        from_av1c = 1;
        obus = data + 4;
        obu_n = size - 4;
    }
    ok_walk = av1_walk_obus(obus, obu_n, &w);
    if (w.overflow) {
        if (out != NULL) {
            out->complete = 0;
        }
        return MS_ERR_OVERFLOW;
    }
    if (!ok_walk) {
        return MS_ERR_MALFORMED;
    }
    complete = (w.have_sh && w.w > 0 && w.h > 0);
    if (out != NULL) {
        out->codec = MS_CODEC_AV1;
        out->sample_fmt = MS_SAMPLE_FMT_AV1;
        out->width = w.w;
        out->height = w.h;
        out->csd_size = 0;
        out->complete = complete;
        out->payload_sig = 0;
        if (complete && from_av1c) {
            memcpy(out->csd, data, size);
            out->csd_size = size;
            out->payload_sig = payload_sig(out->csd, out->csd_size);
        } else if (complete && w.sh != NULL && w.sh_n > 0) {
            /* 合法无 CSD：签名取 sequence header，不把 SH 包装成假 av1C。 */
            out->payload_sig = payload_sig(w.sh, w.sh_n);
        }
    }
    if (au != NULL) {
        au->has_cfg = complete;
        au->is_key = w.is_key != 0;
        au->pic_type = w.is_key ? MS_VPIC_I : (w.have_frame ? MS_VPIC_P : MS_VPIC_UNKNOWN);
    }
    if (is_csd && from_av1c) {
        return complete ? MS_OK : MS_ERR_MALFORMED;
    }
    /* 检查 AU：有帧或有 SH 即认为可检查；配置未齐仍可返回关键帧判断。 */
    if (au != NULL) {
        return (w.have_frame || w.have_sh) ? MS_OK : MS_ERR_MALFORMED;
    }
    return complete ? MS_OK : MS_ERR_MALFORMED;
}



/* GY/T 257.1-2012 / GY/T 257.2-2014：序列头 0x000001B0。
 * 仅 profile_id=0x48（广播类）确认为 AVS+。0x20 基准类停留在家族标识，不得升格。
 * 0x42 流类型或「AVS」四字符不能单独当成 AVS+。AVS2/AVS3 禁止映射为本编码。 */
#define MS_AVS_SC_SEQ   0xB0u
#define MS_AVS_SC_I     0xB3u
#define MS_AVS_SC_PB    0xB6u
#define MS_AVS_PROFILE_JIZHUN    0x20u
#define MS_AVS_PROFILE_BROADCAST 0x48u

static int avs_get_bits(const uint8_t *p, size_t n, size_t *bitpos, unsigned nbits,
                        unsigned *out)
{
    unsigned v = 0;
    size_t i;
    if (p == NULL || bitpos == NULL || out == NULL) {
        return 0;
    }
    for (i = 0; i < nbits; i++) {
        size_t b = *bitpos;
        if ((b / 8) >= n) {
            return 0;
        }
        v = (v << 1) | (unsigned)((p[b / 8] >> (7u - (unsigned)(b % 8))) & 1u);
        (*bitpos)++;
    }
    *out = v;
    return 1;
}

static int avs_parse_seq(const uint8_t *p, size_t n, unsigned *profile,
                         int *w, int *h)
{
    size_t bitpos = 0;
    unsigned pr = 0, level = 0, prog = 0, width = 0, height = 0;
    if (!avs_get_bits(p, n, &bitpos, 8, &pr) ||
        !avs_get_bits(p, n, &bitpos, 8, &level) ||
        !avs_get_bits(p, n, &bitpos, 1, &prog) ||
        !avs_get_bits(p, n, &bitpos, 14, &width) ||
        !avs_get_bits(p, n, &bitpos, 14, &height)) {
        return 0;
    }
    if (width == 0 || height == 0) {
        return 0;
    }
    *profile = pr;
    *w = (int)width;
    *h = (int)height;
    (void)level;
    (void)prog;
    return 1;
}

static ms_status parse_avsplus(int is_csd, const uint8_t *data, size_t size,
                               ms_video_cfg *out, ms_video_au_info *au)
{
    size_t off = 0, pay = 0, nxt = 0, used = 0;
    unsigned sc = 0, profile = 0;
    int have_seq = 0, overflow = 0, saw_any = 0, w = 0, h = 0;
    int is_key = 0, saw_pic = 0, plus = 0;
    uint8_t csd[MS_VIDEO_CSD_MAX];

    memset(csd, 0, sizeof(csd));
    while (find_start(data, size, off, &sc, &pay, &nxt)) {
        size_t psz;
        saw_any = 1;
        if (pay > nxt) {
            break;
        }
        psz = nxt - pay;
        if (sc == MS_AVS_SC_SEQ) {
            unsigned pr = 0;
            int sw = 0, sh = 0;
            if (avs_parse_seq(data + pay, psz, &pr, &sw, &sh)) {
                have_seq = 1;
                profile = pr;
                w = sw;
                h = sh;
                plus = (pr == MS_AVS_PROFILE_BROADCAST);
            }
            if (append_sc(csd, sizeof(csd), &used, data + pay, psz, sc) != 0) {
                overflow = 1;
            }
        } else if (sc == MS_AVS_SC_I) {
            saw_pic = 1;
            is_key = 1;
            if (is_csd) {
                break;
            }
        } else if (sc == MS_AVS_SC_PB) {
            saw_pic = 1;
            if (is_csd) {
                break;
            }
        } else if (is_csd && sc <= 0xAF) {
            break;
        }
        off = (nxt > off) ? nxt : (off + 1);
    }
    if (out != NULL) {
        out->codec = plus ? MS_CODEC_AVS_PLUS : MS_CODEC_AVS_FAMILY;
        out->sample_fmt = MS_SAMPLE_FMT_CAVS;
        out->width = w;
        out->height = h;
        if (overflow) {
            out->complete = 0;
            out->csd_size = 0;
            out->payload_sig = 0;
            return MS_ERR_OVERFLOW;
        }
        if (plus && used > 0 && used <= sizeof(out->csd)) {
            memcpy(out->csd, csd, used);
            out->csd_size = used;
            out->payload_sig = payload_sig(out->csd, out->csd_size);
            out->complete = 1;
        } else {
            out->complete = 0;
            out->csd_size = 0;
            out->payload_sig = 0;
        }
    }
    if (au != NULL) {
        au->has_cfg = have_seq;
        au->pic_type = is_key ? MS_VPIC_I : (saw_pic ? MS_VPIC_P : MS_VPIC_UNKNOWN);
        au->is_key = is_key;
    }
    if (!saw_any && size > 0) {
        return MS_ERR_MALFORMED;
    }
    if (out != NULL && !out->complete) {
        if (have_seq && profile == MS_AVS_PROFILE_JIZHUN) {
            return MS_ERR_UNSUPPORTED;
        }
        return MS_ERR_MALFORMED;
    }
    return MS_OK;
}

ms_status ms_video_cfg_parse(ms_codec codec, ms_sample_format fmt, int is_csd,
                             const uint8_t *data, size_t size, ms_video_cfg *out)
{
    if (out == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    if (out->struct_size < sizeof(*out)) {
        /* 允许稍大的未来结构；过小拒绝，以免旧栈缓冲被写穿 */
        if (out->struct_size != 0 && out->struct_size < sizeof(*out)) {
            return MS_ERR_INVALID_ARG;
        }
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
    if (codec == MS_CODEC_MPEG2_VIDEO) {
        return parse_mpeg2(is_csd, data, size, out, NULL);
    }
    if (codec == MS_CODEC_MPEG4_PART2) {
        return parse_mpeg4(is_csd, data, size, out, NULL);
    }
    if (codec == MS_CODEC_H263) {
        return parse_h263(is_csd, data, size, out, NULL);
    }
    if (codec == MS_CODEC_AV1) {
        return parse_av1(is_csd, data, size, out, NULL);
    }
    if (codec == MS_CODEC_AVS_PLUS || codec == MS_CODEC_AVS_FAMILY) {
        return parse_avsplus(is_csd, data, size, out, NULL);
    }
    /* H.264/HEVC 仍用既有 ms_h264_* / ms_hevc_*，避免把组帧对象再链进 libmsff。 */
    return MS_ERR_UNSUPPORTED;
}

ms_status ms_video_au_inspect(ms_codec codec, ms_sample_format fmt,
                              const uint8_t *data, size_t size,
                              ms_video_au_info *out)
{
    ms_status st;
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
    if (codec == MS_CODEC_MPEG2_VIDEO) {
        return parse_mpeg2(0, data, size, NULL, out);
    }
    if (codec == MS_CODEC_MPEG4_PART2) {
        return parse_mpeg4(0, data, size, NULL, out);
    }
    if (codec == MS_CODEC_H263) {
        return parse_h263(0, data, size, NULL, out);
    }
    if (codec == MS_CODEC_AV1) {
        return parse_av1(0, data, size, NULL, out);
    }
    if (codec == MS_CODEC_AVS_PLUS || codec == MS_CODEC_AVS_FAMILY) {
        return parse_avsplus(0, data, size, NULL, out);
    }
    (void)st;
    (void)fmt;
    return MS_ERR_UNSUPPORTED;
}
