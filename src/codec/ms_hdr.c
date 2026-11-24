#define _DEFAULT_SOURCE
/*
 * HDR10 / HLG 识别：HEVC VUI 与 mastering/CLL SEI，以及 Android
 * hdr-static-info 25 字节打包。不是软解，也不是显示器 HDR 效果。
 * by: Long <mycjzb@gmail.com>
 */
#include "msmedia/msmedia.h"

#include <string.h>

#define HEVC_NAL_SPS 33u
#define HEVC_NAL_PREFIX_SEI 39u
#define HEVC_NAL_SUFFIX_SEI 40u
#define HEVC_NAL_UNSPEC62 62u
#define HEVC_SEI_MASTERING 137u
#define HEVC_SEI_CLL 144u
#define HEVC_SEI_USER_T35 4u

#define MS_HDR_ST_RPS_MAX 64u
#define MS_HDR_RBSP_MAX MS_HEVC_PS_NAL_MAX

typedef struct {
    const uint8_t *p;
    size_t n;
    size_t i;
    unsigned acc;
    int left;
} br;

static size_t nal_to_rbsp(const uint8_t *nal, size_t n, uint8_t *out, size_t cap)
{
    size_t i, o, z;
    if (nal == NULL || n < 3 || out == NULL) {
        return 0;
    }
    i = 2; /* 跳过 2 字节 HEVC NAL 头 */
    o = 0;
    z = 0;
    while (i < n) {
        if (z >= 2 && nal[i] == 0x03 && i + 1 < n && nal[i + 1] <= 3) {
            i++;
            z = 0;
            continue;
        }
        if (o >= cap) {
            return (size_t)-1;
        }
        if (nal[i] == 0) {
            z++;
        } else {
            z = 0;
        }
        out[o++] = nal[i++];
    }
    return o;
}

static void br_init(br *b, const uint8_t *p, size_t n)
{
    memset(b, 0, sizeof(*b));
    b->p = p;
    b->n = n;
}

static int br_u1(br *b, unsigned *out)
{
    if (b->left == 0) {
        if (b->i >= b->n) {
            return -1;
        }
        b->acc = b->p[b->i++];
        b->left = 8;
    }
    b->left--;
    *out = (b->acc >> b->left) & 1u;
    return 0;
}

static int br_u(br *b, int bits, unsigned *out)
{
    unsigned v = 0;
    int i;
    if (bits <= 0 || bits > 32) {
        return -1;
    }
    for (i = 0; i < bits; i++) {
        unsigned bit = 0;
        if (br_u1(b, &bit) != 0) {
            return -1;
        }
        v = (v << 1) | bit;
    }
    *out = v;
    return 0;
}

static int br_ue(br *b, unsigned *out)
{
    int z = 0;
    unsigned bit = 0, suf = 0;
    while (z < 31) {
        if (br_u(b, 1, &bit) != 0) {
            return -1;
        }
        if (bit == 1) {
            break;
        }
        z++;
    }
    if (bit != 1) {
        return -1;
    }
    if (z == 0) {
        *out = 0;
        return 0;
    }
    if (br_u(b, z, &suf) != 0) {
        return -1;
    }
    *out = ((1u << z) - 1u) + suf;
    return 0;
}

static int br_se(br *b, int *out)
{
    unsigned k = 0;
    if (br_ue(b, &k) != 0) {
        return -1;
    }
    if (k == 0) {
        *out = 0;
        return 0;
    }
    if ((k & 1u) != 0) {
        *out = (int)((k + 1u) / 2u);
    } else {
        *out = -(int)(k / 2u);
    }
    return 0;
}

/* general 96 bit 抽出 profile/tier/level；子层按 88+8 跳过。与 ms_hevc.c 一致。 */
static int parse_ptl_simple(br *b, unsigned max_sub_layers_minus1,
                            unsigned *profile_idc, unsigned *tier, unsigned *level_idc)
{
    unsigned space = 0, t = 0, pidc = 0, compat = 0, c0 = 0, c1 = 0, lvl = 0;
    unsigned i, tmp = 0, sp[8], sl[8];
    if (br_u(b, 2, &space) != 0 || br_u(b, 1, &t) != 0 || br_u(b, 5, &pidc) != 0) {
        return -1;
    }
    if (br_u(b, 32, &compat) != 0) {
        return -1;
    }
    if (br_u(b, 32, &c0) != 0 || br_u(b, 16, &c1) != 0) {
        return -1;
    }
    if (br_u(b, 8, &lvl) != 0) {
        return -1;
    }
    if (profile_idc) {
        *profile_idc = pidc;
    }
    if (tier) {
        *tier = t;
    }
    if (level_idc) {
        *level_idc = lvl;
    }
    memset(sp, 0, sizeof(sp));
    memset(sl, 0, sizeof(sl));
    if (max_sub_layers_minus1 > 0) {
        for (i = 0; i < max_sub_layers_minus1 && i < 8; i++) {
            if (br_u(b, 1, &sp[i]) != 0 || br_u(b, 1, &sl[i]) != 0) {
                return -1;
            }
        }
        if (max_sub_layers_minus1 < 8) {
            for (i = max_sub_layers_minus1; i < 8; i++) {
                if (br_u(b, 2, &tmp) != 0) {
                    return -1;
                }
            }
        }
        for (i = 0; i < max_sub_layers_minus1 && i < 8; i++) {
            int k;
            if (sp[i]) {
                for (k = 0; k < 88; k++) {
                    if (br_u(b, 1, &tmp) != 0) {
                        return -1;
                    }
                }
            }
            if (sl[i] && br_u(b, 8, &tmp) != 0) {
                return -1;
            }
        }
    }
    (void)space;
    (void)compat;
    (void)c0;
    (void)c1;
    return 0;
}

static int skip_scaling_list(br *b)
{
    unsigned size_id, matrix_id, pred = 0, delta = 0;
    int se, i, coef_num;
    for (size_id = 0; size_id < 4; size_id++) {
        for (matrix_id = 0; matrix_id < 6; matrix_id += (size_id == 3) ? 3u : 1u) {
            if (br_u(b, 1, &pred) != 0) {
                return -1;
            }
            if (!pred) {
                if (br_ue(b, &delta) != 0) {
                    return -1;
                }
            } else {
                if (size_id > 1 && br_se(b, &se) != 0) {
                    return -1;
                }
                coef_num = 1 << (4 + (int)size_id * 2);
                if (coef_num > 64) {
                    coef_num = 64;
                }
                for (i = 0; i < coef_num; i++) {
                    if (br_se(b, &se) != 0) {
                        return -1;
                    }
                }
            }
        }
    }
    return 0;
}

static int parse_st_rps(br *b, unsigned idx, unsigned num_sets, unsigned *num_delta)
{
    unsigned inter = 0, nneg = 0, npos = 0, i, used = 0, tmp = 0;
    unsigned ref_num = 0;
    if (idx >= MS_HDR_ST_RPS_MAX) {
        return -1;
    }
    if (idx != 0) {
        if (br_u(b, 1, &inter) != 0) {
            return -1;
        }
    }
    if (inter) {
        unsigned delta_idx = 0, sign = 0, absd = 0, j, cnt = 0, ref;
        if (idx == num_sets) {
            if (br_ue(b, &delta_idx) != 0) {
                return -1;
            }
        }
        if (br_u(b, 1, &sign) != 0 || br_ue(b, &absd) != 0) {
            return -1;
        }
        if (delta_idx + 1u > idx) {
            return -1;
        }
        ref = idx - (delta_idx + 1u);
        ref_num = num_delta[ref];
        if (ref_num > 32) {
            return -1;
        }
        for (j = 0; j <= ref_num; j++) {
            if (br_u(b, 1, &used) != 0) {
                return -1;
            }
            if (!used) {
                if (br_u(b, 1, &tmp) != 0) {
                    return -1;
                }
                if (tmp) {
                    cnt++;
                }
            } else {
                cnt++;
            }
        }
        num_delta[idx] = cnt;
        (void)sign;
        (void)absd;
        return 0;
    }
    if (br_ue(b, &nneg) != 0 || br_ue(b, &npos) != 0) {
        return -1;
    }
    if (nneg > 16 || npos > 16) {
        return -1;
    }
    for (i = 0; i < nneg; i++) {
        if (br_ue(b, &tmp) != 0 || br_u(b, 1, &used) != 0) {
            return -1;
        }
    }
    for (i = 0; i < npos; i++) {
        if (br_ue(b, &tmp) != 0 || br_u(b, 1, &used) != 0) {
            return -1;
        }
    }
    num_delta[idx] = nneg + npos;
    return 0;
}

static int parse_vui_color(br *b, ms_hdr_info *out)
{
    unsigned f = 0, tmp = 0, prim = 0, trc = 0, mat = 0, range = 0;
    if (br_u(b, 1, &f) != 0) {
        return -1;
    }
    if (f) {
        if (br_u(b, 8, &tmp) != 0) {
            return -1;
        }
        if (tmp == 255u) {
            if (br_u(b, 16, &tmp) != 0 || br_u(b, 16, &tmp) != 0) {
                return -1;
            }
        }
    }
    if (br_u(b, 1, &f) != 0) {
        return -1;
    }
    if (f && br_u(b, 1, &tmp) != 0) {
        return -1;
    }
    if (br_u(b, 1, &f) != 0) {
        return -1;
    }
    if (f) {
        if (br_u(b, 3, &tmp) != 0 || br_u(b, 1, &range) != 0 || br_u(b, 1, &f) != 0) {
            return -1;
        }
        out->color_range = range ? 2 : 1;
        if (f) {
            if (br_u(b, 8, &prim) != 0 || br_u(b, 8, &trc) != 0 || br_u(b, 8, &mat) != 0) {
                return -1;
            }
            out->color_primaries = (int)prim;
            out->color_transfer = (int)trc;
            out->color_matrix = (int)mat;
            out->have_color = 1;
            out->from_vui = 1;
        }
    }
    /* 颜色字段已读完，不必再解析 HRD。 */
    return 0;
}

static int parse_sps_vui(const uint8_t *nal, size_t n, ms_hdr_info *out)
{
    uint8_t rb[MS_HDR_RBSP_MAX];
    size_t rn;
    br b;
    unsigned vps = 0, maxsl = 0, nest = 0, sid = 0, chroma = 0, flag = 0, tmp = 0;
    unsigned width = 0, height = 0, bd_luma = 0, bd_chroma = 0, poc = 0;
    unsigned i, num_st = 0, num_delta[MS_HDR_ST_RPS_MAX];
    unsigned profile = 0, tier = 0, level = 0;
    rn = nal_to_rbsp(nal, n, rb, sizeof(rb));
    if (rn == 0 || rn == (size_t)-1) {
        return -1;
    }
    br_init(&b, rb, rn);
    if (br_u(&b, 4, &vps) != 0 || br_u(&b, 3, &maxsl) != 0 || br_u(&b, 1, &nest) != 0) {
        return -1;
    }
    if (parse_ptl_simple(&b, maxsl, &profile, &tier, &level) != 0) {
        return -1;
    }
    out->profile_idc = profile;
    out->tier_flag = tier;
    out->level_idc = level;
    if (br_ue(&b, &sid) != 0 || br_ue(&b, &chroma) != 0) {
        return -1;
    }
    out->chroma_format_idc = (int)chroma;
    if (chroma == 3u && br_u(&b, 1, &tmp) != 0) {
        return -1;
    }
    if (br_ue(&b, &width) != 0 || br_ue(&b, &height) != 0 || br_u(&b, 1, &flag) != 0) {
        return -1;
    }
    out->width = (int)width;
    out->height = (int)height;
    if (flag) {
        for (i = 0; i < 4; i++) {
            if (br_ue(&b, &tmp) != 0) {
                return -1;
            }
        }
    }
    if (br_ue(&b, &bd_luma) != 0 || br_ue(&b, &bd_chroma) != 0) {
        return -1;
    }
    out->bit_depth = (int)bd_luma + 8;
    if (br_ue(&b, &poc) != 0 || br_u(&b, 1, &flag) != 0) {
        return -1;
    }
    {
        unsigned start = flag ? 0u : maxsl;
        for (i = start; i <= maxsl && i < 8; i++) {
            if (br_ue(&b, &tmp) != 0 || br_ue(&b, &tmp) != 0 || br_ue(&b, &tmp) != 0) {
                return -1;
            }
        }
    }
    if (br_ue(&b, &tmp) != 0 || br_ue(&b, &tmp) != 0 ||
        br_ue(&b, &tmp) != 0 || br_ue(&b, &tmp) != 0 ||
        br_ue(&b, &tmp) != 0 || br_ue(&b, &tmp) != 0) {
        return -1;
    }
    if (br_u(&b, 1, &flag) != 0) {
        return -1;
    }
    if (flag) {
        if (br_u(&b, 1, &flag) != 0) {
            return -1;
        }
        if (flag && skip_scaling_list(&b) != 0) {
            return -1;
        }
    }
    if (br_u(&b, 1, &tmp) != 0 || br_u(&b, 1, &tmp) != 0 || br_u(&b, 1, &flag) != 0) {
        return -1;
    }
    if (flag) {
        if (br_u(&b, 4, &tmp) != 0 || br_u(&b, 4, &tmp) != 0 ||
            br_ue(&b, &tmp) != 0 || br_ue(&b, &tmp) != 0 || br_u(&b, 1, &tmp) != 0) {
            return -1;
        }
    }
    if (br_ue(&b, &num_st) != 0) {
        return -1;
    }
    if (num_st > MS_HDR_ST_RPS_MAX) {
        return -1;
    }
    memset(num_delta, 0, sizeof(num_delta));
    for (i = 0; i < num_st; i++) {
        if (parse_st_rps(&b, i, num_st, num_delta) != 0) {
            return -1;
        }
    }
    if (br_u(&b, 1, &flag) != 0) {
        return -1;
    }
    if (flag) {
        unsigned nlt = 0, bits = poc + 4u;
        if (br_ue(&b, &nlt) != 0) {
            return -1;
        }
        if (nlt > 32 || bits > 16) {
            return -1;
        }
        for (i = 0; i < nlt; i++) {
            if (br_u(&b, (int)bits, &tmp) != 0 || br_u(&b, 1, &tmp) != 0) {
                return -1;
            }
        }
    }
    if (br_u(&b, 1, &tmp) != 0 || br_u(&b, 1, &tmp) != 0 || br_u(&b, 1, &flag) != 0) {
        return -1;
    }
    if (flag) {
        return parse_vui_color(&b, out);
    }
    (void)vps;
    (void)nest;
    (void)sid;
    (void)width;
    (void)height;
    (void)bd_chroma;
    return 0;
}

static unsigned be16(const uint8_t *p)
{
    return ((unsigned)p[0] << 8) | (unsigned)p[1];
}

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void parse_sei_payloads(const uint8_t *rb, size_t n, ms_hdr_info *out)
{
    size_t i = 0;
    while (i < n) {
        unsigned typ = 0, sz = 0, k;
        if (rb[i] == 0x80 && i + 1 == n) {
            break; /* rbsp trailing */
        }
        typ = 0;
        while (i < n && rb[i] == 0xFF) {
            typ += 255;
            i++;
        }
        if (i >= n) {
            break;
        }
        typ += rb[i++];
        sz = 0;
        while (i < n && rb[i] == 0xFF) {
            sz += 255;
            i++;
        }
        if (i >= n) {
            break;
        }
        sz += rb[i++];
        if (i + sz > n) {
            break;
        }
        if (typ == HEVC_SEI_MASTERING && sz >= 24) {
            const uint8_t *p = rb + i;
            for (k = 0; k < 3; k++) {
                out->display_primaries_x[k] = (uint16_t)be16(p + k * 4);
                out->display_primaries_y[k] = (uint16_t)be16(p + k * 4 + 2);
            }
            out->white_point_x = (uint16_t)be16(p + 12);
            out->white_point_y = (uint16_t)be16(p + 14);
            out->max_display_mastering_luminance = be32(p + 16);
            out->min_display_mastering_luminance = be32(p + 20);
            out->have_mastering = 1;
            out->from_sei = 1;
        } else if (typ == HEVC_SEI_CLL && sz >= 4) {
            out->max_content_light_level = (uint16_t)be16(rb + i);
            out->max_frame_average_light_level = (uint16_t)be16(rb + i + 2);
            out->have_cll = 1;
            out->from_sei = 1;
        } else if (typ == HEVC_SEI_USER_T35 && sz >= 6) {
            /* 0xB5 = ITU-T T.35 US；ST 2094-40 / Dolby 常见走这条。 */
            unsigned country = rb[i];
            unsigned pcode = ((unsigned)rb[i + 1] << 8) | (unsigned)rb[i + 2];
            if (country == 0xB5 && pcode == 0x003C) {
                /* SMPTE ST 2094-40 常见 provider_code 0x003C（60）。未验证透传。 */
                out->have_hdr10_plus = 1;
            } else if (country == 0xB5 && (pcode == 0x0000 || pcode == 0x0001)) {
                /* Dolby 常见 T.35 provider，仅作识别，不配置 DV。 */
                out->dolby_vision = 1;
            }
        }
        i += sz;
    }
}

static int parse_sei_nal(const uint8_t *nal, size_t n, ms_hdr_info *out)
{
    uint8_t rb[MS_HDR_RBSP_MAX];
    size_t rn;
    rn = nal_to_rbsp(nal, n, rb, sizeof(rb));
    if (rn == 0 || rn == (size_t)-1) {
        return -1;
    }
    parse_sei_payloads(rb, rn, out);
    return 0;
}

static unsigned hevc_nal_type(const uint8_t *nal, size_t n)
{
    if (nal == NULL || n < 2) {
        return 0xFFu;
    }
    return (unsigned)((nal[0] >> 1) & 0x3Fu);
}

static const uint8_t *find_start(const uint8_t *p, const uint8_t *end, size_t *sc)
{
    const uint8_t *q;
    for (q = p; q + 3 <= end; q++) {
        if (q[0] == 0 && q[1] == 0 && q[2] == 1) {
            *sc = 3;
            return q;
        }
        if (q + 4 <= end && q[0] == 0 && q[1] == 0 && q[2] == 0 && q[3] == 1) {
            *sc = 4;
            return q;
        }
    }
    return NULL;
}

static void handle_nal(const uint8_t *nal, size_t n, ms_hdr_info *out)
{
    unsigned typ;
    typ = hevc_nal_type(nal, n);
    if (typ == HEVC_NAL_SPS) {
        (void)parse_sps_vui(nal, n, out);
    } else if (typ == HEVC_NAL_PREFIX_SEI || typ == HEVC_NAL_SUFFIX_SEI) {
        (void)parse_sei_nal(nal, n, out);
    }
    (void)HEVC_NAL_UNSPEC62;
}

static void walk_annexb(const uint8_t *data, size_t size, ms_hdr_info *out)
{
    const uint8_t *end = data + size;
    const uint8_t *s, *n;
    size_t sc = 0, sc2 = 0;
    s = find_start(data, end, &sc);
    while (s != NULL) {
        n = find_start(s + sc, end, &sc2);
        handle_nal(s + sc, (size_t)((n ? n : end) - (s + sc)), out);
        s = n;
        sc = sc2;
    }
}

static void walk_len(const uint8_t *data, size_t size, unsigned ls, ms_hdr_info *out)
{
    size_t off = 0;
    if (ls < 1 || ls > 4) {
        return;
    }
    while (off + ls <= size) {
        unsigned i;
        size_t ln = 0;
        for (i = 0; i < ls; i++) {
            ln = (ln << 8) | data[off + i];
        }
        off += ls;
        if (ln == 0 || off + ln > size) {
            break;
        }
        handle_nal(data + off, ln, out);
        off += ln;
    }
}

static void walk_hvcc(const uint8_t *data, size_t size, ms_hdr_info *out)
{
    size_t off;
    unsigned arrays, a, ls;
    if (data == NULL || size < 23 || data[0] != 1) {
        return;
    }
    ls = (unsigned)((data[21] & 3u) + 1u);
    arrays = data[22];
    off = 23;
    for (a = 0; a < arrays && off + 3 <= size; a++) {
        unsigned nnal, i;
        nnal = ((unsigned)data[off + 1] << 8) | (unsigned)data[off + 2];
        off += 3;
        for (i = 0; i < nnal && off + 2 <= size; i++) {
            unsigned ln = ((unsigned)data[off] << 8) | (unsigned)data[off + 1];
            off += 2;
            if (off + ln > size) {
                return;
            }
            handle_nal(data + off, ln, out);
            off += ln;
        }
    }
    (void)ls;
}

void ms_hdr_info_default(ms_hdr_info *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->struct_size = sizeof(*out);
}

const char *ms_hdr_kind_name(ms_hdr_kind kind)
{
    switch (kind) {
    case MS_HDR_KIND_SDR:
        return "sdr";
    case MS_HDR_KIND_HDR10:
        return "hdr10";
    case MS_HDR_KIND_HLG:
        return "hlg";
    default:
        return "unknown";
    }
}

void ms_hdr_classify(ms_hdr_info *info)
{
    if (info == NULL) {
        return;
    }
    if (info->color_transfer == 16) {
        info->kind = MS_HDR_KIND_HDR10;
    } else if (info->color_transfer == 18) {
        info->kind = MS_HDR_KIND_HLG;
    } else if (info->have_color || info->color_transfer > 0 ||
               info->color_primaries > 0) {
        info->kind = MS_HDR_KIND_SDR;
    } else if (info->have_mastering || info->have_cll) {
        /* 有静态元数据但 transfer 未写时，按 HDR10 静态信息对待。 */
        info->kind = MS_HDR_KIND_HDR10;
    } else {
        info->kind = MS_HDR_KIND_UNKNOWN;
    }
}

void ms_hdr_merge(ms_hdr_info *dst, const ms_hdr_info *src)
{
    unsigned i;
    if (dst == NULL || src == NULL) {
        return;
    }
    if (!dst->have_color && src->have_color) {
        dst->have_color = 1;
        dst->color_primaries = src->color_primaries;
        dst->color_transfer = src->color_transfer;
        dst->color_matrix = src->color_matrix;
        dst->color_range = src->color_range;
    } else {
        if (dst->color_primaries == 0) {
            dst->color_primaries = src->color_primaries;
        }
        if (dst->color_transfer == 0) {
            dst->color_transfer = src->color_transfer;
        }
        if (dst->color_matrix == 0) {
            dst->color_matrix = src->color_matrix;
        }
        if (dst->color_range == 0) {
            dst->color_range = src->color_range;
        }
        if (src->have_color) {
            dst->have_color = 1;
        }
    }
    if (dst->bit_depth == 0) {
        dst->bit_depth = src->bit_depth;
    }
    if (dst->width <= 0 && src->width > 0) {
        dst->width = src->width;
    }
    if (dst->height <= 0 && src->height > 0) {
        dst->height = src->height;
    }
    if (dst->chroma_format_idc == 0) {
        dst->chroma_format_idc = src->chroma_format_idc;
    }
    if (dst->profile_idc == 0) {
        dst->profile_idc = src->profile_idc;
    }
    if (dst->level_idc == 0) {
        dst->level_idc = src->level_idc;
    }
    if (dst->tier_flag == 0) {
        dst->tier_flag = src->tier_flag;
    }
    if (!dst->have_mastering && src->have_mastering) {
        dst->have_mastering = 1;
        for (i = 0; i < 3; i++) {
            dst->display_primaries_x[i] = src->display_primaries_x[i];
            dst->display_primaries_y[i] = src->display_primaries_y[i];
        }
        dst->white_point_x = src->white_point_x;
        dst->white_point_y = src->white_point_y;
        dst->max_display_mastering_luminance = src->max_display_mastering_luminance;
        dst->min_display_mastering_luminance = src->min_display_mastering_luminance;
    }
    if (!dst->have_cll && src->have_cll) {
        dst->have_cll = 1;
        dst->max_content_light_level = src->max_content_light_level;
        dst->max_frame_average_light_level = src->max_frame_average_light_level;
    }
    if (src->have_hdr10_plus) {
        dst->have_hdr10_plus = 1;
    }
    if (src->dolby_vision) {
        dst->dolby_vision = 1;
    }
    dst->from_vui |= src->from_vui;
    dst->from_sei |= src->from_sei;
    dst->from_container |= src->from_container;
    ms_hdr_classify(dst);
}

ms_status ms_hdr_parse_hevc(ms_sample_format fmt, int is_csd,
                            const uint8_t *data, size_t size, ms_hdr_info *out)
{
    if (out == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    ms_hdr_info_default(out);
    if (data == NULL || size == 0) {
        return MS_ERR_INVALID_ARG;
    }
    if (is_csd && fmt == MS_SAMPLE_FMT_HVCC && size >= 23 && data[0] == 1) {
        walk_hvcc(data, size, out);
    } else if (fmt == MS_SAMPLE_FMT_HVCC || fmt == MS_SAMPLE_FMT_AVCC) {
        unsigned ls = 4;
        if (is_csd && size >= 23 && data[0] == 1) {
            walk_hvcc(data, size, out);
        } else {
            walk_len(data, size, ls, out);
        }
    } else {
        /* Annex B 或未知：先按起始码，再尝试 4 字节长度前缀。 */
        walk_annexb(data, size, out);
        if (!out->have_color && !out->have_mastering && !out->have_cll &&
            !out->from_vui) {
            walk_len(data, size, 4, out);
        }
    }
    ms_hdr_classify(out);
    return MS_OK;
}

static void wr_le16(uint8_t *p, unsigned v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

ms_status ms_hdr_pack_static_info(const ms_hdr_info *info,
                                  uint8_t *out, size_t cap, size_t *out_size)
{
    unsigned max_nits, min_units;
    if (out_size) {
        *out_size = 0;
    }
    if (info == NULL || out == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    if (cap < MS_HDR_STATIC_INFO_SIZE) {
        return MS_ERR_OVERFLOW;
    }
    if (!info->have_mastering && !info->have_cll) {
        return MS_AGAIN;
    }
    memset(out, 0, MS_HDR_STATIC_INFO_SIZE);
    out[0] = 1; /* Static Metadata Type 1 */
    /* HEVC SEI 为 G、B、R；Android / CTA-861.3 为 R、G、B。 */
    wr_le16(out + 1, info->display_primaries_x[2]);
    wr_le16(out + 3, info->display_primaries_y[2]);
    wr_le16(out + 5, info->display_primaries_x[0]);
    wr_le16(out + 7, info->display_primaries_y[0]);
    wr_le16(out + 9, info->display_primaries_x[1]);
    wr_le16(out + 11, info->display_primaries_y[1]);
    wr_le16(out + 13, info->white_point_x);
    wr_le16(out + 15, info->white_point_y);
    /* SEI 最大亮度单位 0.0001 cd/m^2 → HDMI uint16 的 1 cd/m^2。 */
    max_nits = info->max_display_mastering_luminance / 10000u;
    if (max_nits > 65535u) {
        max_nits = 65535u;
    }
    wr_le16(out + 17, max_nits);
    /* 最小亮度：SEI 已是 0.0001 cd/m^2，与 HDMI uint16 相同，截断到 16 位。 */
    min_units = info->min_display_mastering_luminance;
    if (min_units > 65535u) {
        min_units = 65535u;
    }
    wr_le16(out + 19, min_units);
    wr_le16(out + 21, info->max_content_light_level);
    wr_le16(out + 23, info->max_frame_average_light_level);
    if (out_size) {
        *out_size = MS_HDR_STATIC_INFO_SIZE;
    }
    return MS_OK;
}

int ms_hdr_android_color_standard(const ms_hdr_info *info)
{
    int p;
    if (info == NULL) {
        return 0;
    }
    p = info->color_primaries;
    if (p == 9) {
        return 6; /* COLOR_STANDARD_BT2020 */
    }
    if (p == 1) {
        return 1; /* COLOR_STANDARD_BT709 */
    }
    if (p == 5) {
        return 2; /* COLOR_STANDARD_BT601_PAL */
    }
    if (p == 6) {
        return 4; /* COLOR_STANDARD_BT601_NTSC */
    }
    return 0;
}

int ms_hdr_android_color_transfer(const ms_hdr_info *info)
{
    int t;
    if (info == NULL) {
        return 0;
    }
    t = info->color_transfer;
    if (t == 16) {
        return 6; /* COLOR_TRANSFER_ST2084 */
    }
    if (t == 18) {
        return 7; /* COLOR_TRANSFER_HLG */
    }
    if (t == 8) {
        return 1; /* COLOR_TRANSFER_LINEAR */
    }
    if (t == 1 || t == 6 || t == 14 || t == 15) {
        return 3; /* COLOR_TRANSFER_SDR_VIDEO */
    }
    return 0;
}

int ms_hdr_android_color_range(const ms_hdr_info *info)
{
    if (info == NULL || info->color_range == 0) {
        return 0;
    }
    if (info->color_range == 2) {
        return 1; /* COLOR_RANGE_FULL */
    }
    return 2; /* COLOR_RANGE_LIMITED */
}

int ms_hdr_android_hevc_profile(const ms_hdr_info *info)
{
    if (info == NULL) {
        return 0;
    }
    if (info->kind == MS_HDR_KIND_HDR10) {
        /* 用 Main10。Main10HDR10(4096) 在部分组件（含模拟器 goldfish）上会导致
         * configure EINVAL；PQ 已由 color-transfer=ST2084 表达。 */
        return 2;
    }
    if (info->kind == MS_HDR_KIND_HLG || info->bit_depth >= 10 ||
        info->profile_idc == 2) {
        return 2; /* HEVCProfileMain10 */
    }
    if (info->profile_idc == 1 || info->profile_idc == 0) {
        return info->profile_idc == 1 ? 1 : 0;
    }
    return 0;
}

int ms_hdr_android_hevc_level(const ms_hdr_info *info)
{
    unsigned lidc;
    int high;
    if (info == NULL || info->level_idc == 0) {
        return 0;
    }
    lidc = info->level_idc;
    high = info->tier_flag != 0 && lidc >= 120; /* High tier 自 4.0 */
    /* CodecProfileLevel：Main/High 的取值见 Android 文档。High=对应 Main 的 2 倍（同档）。 */
    switch (lidc) {
    case 30:
        return 1; /* Level1 */
    case 60:
        return high ? 8 : 4; /* Level2 */
    case 63:
        return high ? 32 : 16; /* Level2.1 */
    case 90:
        return high ? 128 : 64; /* Level3 */
    case 93:
        return high ? 512 : 256; /* Level3.1 */
    case 120:
        return high ? 2048 : 1024; /* Level4 */
    case 123:
        return high ? 8192 : 4096; /* Level4.1 */
    case 150:
        return high ? 32768 : 16384; /* Level5 */
    case 153:
        return high ? 131072 : 65536; /* Level5.1 */
    case 156:
        return high ? 524288 : 262144; /* Level5.2 */
    case 180:
        return high ? 2097152 : 1048576; /* Level6 */
    case 183:
        return high ? 8388608 : 4194304; /* Level6.1 */
    case 186:
        return high ? 33554432 : 16777216; /* Level6.2 */
    default:
        return 0;
    }
}
