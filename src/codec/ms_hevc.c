/*
 * HEVC 参数集与访问单元检查：Annex B / hvcC，有界、不成对不拼接。
 * 不是软解，不解释图像载荷。
 * by: Long <mycjzb@gmail.com>
 */
#include "msmedia/msmedia.h"

#include <string.h>

#define HEVC_NAL_VPS 32u
#define HEVC_NAL_SPS 33u
#define HEVC_NAL_PPS 34u
#define HEVC_NAL_AUD 35u

typedef struct {
    uint8_t nal[MS_HEVC_PS_NAL_MAX];
    size_t n;
    unsigned id;
    unsigned ref_id;
} ps_nal;

typedef struct {
    const uint8_t *p;
    size_t n;
    size_t i;
    unsigned acc;
    int left;
} br;

static unsigned hevc_nal_type(const uint8_t *nal, size_t n)
{
    if (nal == NULL || n < 2) {
        return 0xFFu;
    }
    return (unsigned)((nal[0] >> 1) & 0x3Fu);
}

int ms_hevc_nal_is_irap(unsigned nal_type)
{
    return nal_type >= 16u && nal_type <= 23u;
}

static const uint8_t *ps_start(const uint8_t *p, const uint8_t *end, size_t *sc)
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

static size_t nal_to_rbsp(const uint8_t *nal, size_t n, uint8_t *out, size_t cap)
{
    size_t i, o, z;
    if (n < 3) {
        return 0;
    }
    i = 2; /* 跳过 2 字节 NAL 头 */
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
    b->left = 0;
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

static int br_skip(br *b, int bits)
{
    int i;
    unsigned dummy = 0;
    if (bits < 0) {
        return -1;
    }
    for (i = 0; i < bits; i++) {
        if (br_u1(b, &dummy) != 0) {
            return -1;
        }
    }
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

static int skip_profile_tier_level(br *b, unsigned max_sub_layers_minus1)
{
    unsigned tmp = 0, i, sp[8], sl[8];
    /* general: 2+1+5+32+48+8 = 96 bits */
    if (br_skip(b, 96) != 0) {
        return -1;
    }
    memset(sp, 0, sizeof(sp));
    memset(sl, 0, sizeof(sl));
    if (max_sub_layers_minus1 > 0) {
        for (i = 0; i < max_sub_layers_minus1 && i < 8; i++) {
            if (br_u(b, 1, &sp[i]) != 0 || br_u(b, 1, &sl[i]) != 0) {
                return -1;
            }
        }
        if (max_sub_layers_minus1 > 0 && max_sub_layers_minus1 < 8) {
            /* reserved_zero_2bits for i = max .. 7 */
            for (i = max_sub_layers_minus1; i < 8; i++) {
                if (br_u(b, 2, &tmp) != 0) {
                    return -1;
                }
            }
        }
        for (i = 0; i < max_sub_layers_minus1 && i < 8; i++) {
            if (sp[i]) {
                if (br_skip(b, 88) != 0) {
                    return -1;
                }
            }
            if (sl[i]) {
                if (br_u(b, 8, &tmp) != 0) {
                    return -1;
                }
            }
        }
    }
    return 0;
}

static int parse_vps_id(const uint8_t *nal, size_t n, unsigned *id)
{
    uint8_t rb[MS_HEVC_PS_NAL_MAX];
    size_t rn;
    br b;
    unsigned v = 0;
    rn = nal_to_rbsp(nal, n, rb, sizeof(rb));
    if (rn == 0 || rn == (size_t)-1) {
        return -1;
    }
    br_init(&b, rb, rn);
    if (br_u(&b, 4, &v) != 0) {
        return -1;
    }
    *id = v;
    return 0;
}

static int parse_sps_ids(const uint8_t *nal, size_t n, unsigned *sps_id, unsigned *vps_id)
{
    uint8_t rb[MS_HEVC_PS_NAL_MAX];
    size_t rn;
    br b;
    unsigned vps = 0, maxsl = 0, nest = 0, sid = 0;
    rn = nal_to_rbsp(nal, n, rb, sizeof(rb));
    if (rn == 0 || rn == (size_t)-1) {
        return -1;
    }
    br_init(&b, rb, rn);
    if (br_u(&b, 4, &vps) != 0 || br_u(&b, 3, &maxsl) != 0 || br_u(&b, 1, &nest) != 0) {
        return -1;
    }
    if (skip_profile_tier_level(&b, maxsl) != 0) {
        return -1;
    }
    if (br_ue(&b, &sid) != 0) {
        return -1;
    }
    *vps_id = vps;
    *sps_id = sid;
    return 0;
}

static int parse_pps_ids(const uint8_t *nal, size_t n, unsigned *pps_id, unsigned *sps_id)
{
    uint8_t rb[MS_HEVC_PS_NAL_MAX];
    size_t rn;
    br b;
    unsigned pid = 0, sid = 0;
    rn = nal_to_rbsp(nal, n, rb, sizeof(rb));
    if (rn == 0 || rn == (size_t)-1) {
        return -1;
    }
    br_init(&b, rb, rn);
    if (br_ue(&b, &pid) != 0 || br_ue(&b, &sid) != 0) {
        return -1;
    }
    *pps_id = pid;
    *sps_id = sid;
    return 0;
}

static int first_slice_flag(const uint8_t *nal, size_t n)
{
    unsigned typ = hevc_nal_type(nal, n);
    uint8_t rb[8];
    size_t rn;
    br b;
    unsigned v = 0;
    if (typ > 31u) {
        return 0;
    }
    rn = nal_to_rbsp(nal, n, rb, sizeof(rb));
    if (rn == 0 || rn == (size_t)-1) {
        return 0;
    }
    br_init(&b, rb, rn);
    if (br_u(&b, 1, &v) != 0) {
        return 0;
    }
    return v ? 1 : 0;
}

static ms_status ps_store(ps_nal *slots, int *n, const uint8_t *nal, size_t nn,
                          unsigned id, unsigned ref)
{
    if (nn == 0 || nn > MS_HEVC_PS_NAL_MAX) {
        return MS_ERR_OVERFLOW;
    }
    if (*n >= MS_HEVC_PS_SLOT_MAX) {
        /* 保留最近的，丢更早的，不报溢出，与 H.264 提取策略一致：有界。 */
        if (MS_HEVC_PS_SLOT_MAX > 1) {
            memmove(&slots[0], &slots[1], sizeof(slots[0]) * (MS_HEVC_PS_SLOT_MAX - 1));
            *n = MS_HEVC_PS_SLOT_MAX - 1;
        } else {
            *n = 0;
        }
    }
    memcpy(slots[*n].nal, nal, nn);
    slots[*n].n = nn;
    slots[*n].id = id;
    slots[*n].ref_id = ref;
    (*n)++;
    return MS_OK;
}

static ms_status ps_take_nal(const uint8_t *nal, size_t nn, ps_nal *vps, int *nv,
                             ps_nal *sps, int *ns, ps_nal *pps, int *np)
{
    unsigned typ, id = 0, ref = 0;
    typ = hevc_nal_type(nal, nn);
    if (typ != HEVC_NAL_VPS && typ != HEVC_NAL_SPS && typ != HEVC_NAL_PPS) {
        return MS_OK;
    }
    if (nn > MS_HEVC_PS_NAL_MAX) {
        return MS_ERR_OVERFLOW;
    }
    if (typ == HEVC_NAL_VPS) {
        if (parse_vps_id(nal, nn, &id) != 0) {
            return MS_ERR_MALFORMED;
        }
        return ps_store(vps, nv, nal, nn, id, 0);
    }
    if (typ == HEVC_NAL_SPS) {
        if (parse_sps_ids(nal, nn, &id, &ref) != 0) {
            return MS_ERR_MALFORMED;
        }
        return ps_store(sps, ns, nal, nn, id, ref);
    }
    if (typ == HEVC_NAL_PPS) {
        if (parse_pps_ids(nal, nn, &id, &ref) != 0) {
            return MS_ERR_MALFORMED;
        }
        return ps_store(pps, np, nal, nn, id, ref);
    }
    return MS_OK;
}

static void ps_pick_triple(const ps_nal *vps, int nv, const ps_nal *sps, int ns,
                           const ps_nal *pps, int np, ms_hevc_ps_set *out)
{
    int i, j, k;
    memset(out, 0, sizeof(*out));
    out->struct_size = sizeof(*out);
    if (nv > 0) {
        out->have_vps = 1;
        out->vps_size = vps[nv - 1].n;
        memcpy(out->vps, vps[nv - 1].nal, vps[nv - 1].n);
        out->vps_id = vps[nv - 1].id;
    }
    if (ns > 0) {
        out->have_sps = 1;
        out->sps_size = sps[ns - 1].n;
        memcpy(out->sps, sps[ns - 1].nal, sps[ns - 1].n);
        out->sps_id = sps[ns - 1].id;
        out->sps_vps_id = sps[ns - 1].ref_id;
    }
    if (np > 0) {
        out->have_pps = 1;
        out->pps_size = pps[np - 1].n;
        memcpy(out->pps, pps[np - 1].nal, pps[np - 1].n);
        out->pps_id = pps[np - 1].id;
        out->pps_sps_id = pps[np - 1].ref_id;
    }
    /* 只在本缓冲内配对，从后往前，不拼接旧集。 */
    for (i = np - 1; i >= 0; i--) {
        for (j = ns - 1; j >= 0; j--) {
            if (pps[i].ref_id != sps[j].id) {
                continue;
            }
            for (k = nv - 1; k >= 0; k--) {
                if (sps[j].ref_id != vps[k].id) {
                    continue;
                }
                out->have_vps = 1;
                out->have_sps = 1;
                out->have_pps = 1;
                out->vps_size = vps[k].n;
                memcpy(out->vps, vps[k].nal, vps[k].n);
                out->vps_id = vps[k].id;
                out->sps_size = sps[j].n;
                memcpy(out->sps, sps[j].nal, sps[j].n);
                out->sps_id = sps[j].id;
                out->sps_vps_id = sps[j].ref_id;
                out->pps_size = pps[i].n;
                memcpy(out->pps, pps[i].nal, pps[i].n);
                out->pps_id = pps[i].id;
                out->pps_sps_id = pps[i].ref_id;
                out->triple_complete = 1;
                return;
            }
        }
    }
}

static ms_status walk_annexb(const uint8_t *d, size_t n, ps_nal *vps, int *nv,
                             ps_nal *sps, int *ns, ps_nal *pps, int *np)
{
    const uint8_t *end = d + n;
    const uint8_t *p = d;
    while (p < end) {
        size_t sc = 0;
        const uint8_t *nal, *next;
        size_t nn;
        ms_status st;
        nal = ps_start(p, end, &sc);
        if (nal == NULL) {
            break;
        }
        nal += sc;
        next = ps_start(nal, end, &sc);
        nn = (size_t)((next ? next : end) - nal);
        st = ps_take_nal(nal, nn, vps, nv, sps, ns, pps, np);
        if (st != MS_OK) {
            return st;
        }
        p = next ? next : end;
    }
    return MS_OK;
}

static ms_status walk_len_sample(const uint8_t *d, size_t n, unsigned ls,
                                 ps_nal *vps, int *nv, ps_nal *sps, int *ns,
                                 ps_nal *pps, int *np)
{
    size_t off = 0;
    if (ls == 0) {
        ls = 4;
    }
    if (ls > 4) {
        return MS_ERR_MALFORMED;
    }
    while (off < n) {
        uint64_t nalsz = 0;
        unsigned k;
        ms_status st;
        if (off + ls > n) {
            return MS_ERR_MALFORMED;
        }
        for (k = 0; k < ls; k++) {
            nalsz = (nalsz << 8) | (uint64_t)d[off + k];
        }
        off += ls;
        if (nalsz == 0 || off + nalsz > n) {
            return MS_ERR_MALFORMED;
        }
        st = ps_take_nal(d + off, (size_t)nalsz, vps, nv, sps, ns, pps, np);
        if (st != MS_OK) {
            return st;
        }
        off += (size_t)nalsz;
    }
    return MS_OK;
}

static int looks_hvcc(const uint8_t *d, size_t n)
{
    return n >= 23 && d[0] == 1;
}

static ms_status walk_hvcc_cfg(const uint8_t *d, size_t n, ps_nal *vps, int *nv,
                               ps_nal *sps, int *ns, ps_nal *pps, int *np,
                               unsigned *out_ls)
{
    size_t off;
    unsigned arrays, a;
    if (!looks_hvcc(d, n)) {
        return MS_ERR_MALFORMED;
    }
    if (out_ls) {
        *out_ls = (unsigned)((d[21] & 3u) + 1u);
    }
    arrays = d[22];
    off = 23;
    for (a = 0; a < arrays; a++) {
        unsigned num, i;
        if (off + 3 > n) {
            return MS_ERR_MALFORMED;
        }
        /* array completeness + type in d[off], then numNalus */
        num = ((unsigned)d[off + 1] << 8) | (unsigned)d[off + 2];
        off += 3;
        for (i = 0; i < num; i++) {
            unsigned ln;
            ms_status st;
            if (off + 2 > n) {
                return MS_ERR_MALFORMED;
            }
            ln = ((unsigned)d[off] << 8) | (unsigned)d[off + 1];
            off += 2;
            if (ln == 0 || off + ln > n) {
                return MS_ERR_MALFORMED;
            }
            st = ps_take_nal(d + off, ln, vps, nv, sps, ns, pps, np);
            if (st != MS_OK) {
                return st;
            }
            off += ln;
        }
    }
    return MS_OK;
}

ms_status ms_hevc_ps_parse(ms_sample_format fmt, int is_csd, const uint8_t *data,
                           size_t size, unsigned nal_len_size, ms_hevc_ps_set *out)
{
    ps_nal vps[MS_HEVC_PS_SLOT_MAX], sps[MS_HEVC_PS_SLOT_MAX], pps[MS_HEVC_PS_SLOT_MAX];
    int nv = 0, ns = 0, np = 0;
    ms_status st;
    if (out == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    out->struct_size = sizeof(*out);
    if (data == NULL || size == 0) {
        return MS_OK;
    }
    if (fmt == MS_SAMPLE_FMT_ANNEXB ||
        (fmt == MS_SAMPLE_FMT_UNKNOWN && !is_csd && !looks_hvcc(data, size))) {
        st = walk_annexb(data, size, vps, &nv, sps, &ns, pps, &np);
    } else if (fmt == MS_SAMPLE_FMT_HVCC && is_csd) {
        st = walk_hvcc_cfg(data, size, vps, &nv, sps, &ns, pps, &np, NULL);
    } else if (fmt == MS_SAMPLE_FMT_HVCC) {
        st = walk_len_sample(data, size, nal_len_size, vps, &nv, sps, &ns, pps, &np);
    } else if (fmt == MS_SAMPLE_FMT_UNKNOWN && is_csd && looks_hvcc(data, size)) {
        st = walk_hvcc_cfg(data, size, vps, &nv, sps, &ns, pps, &np, NULL);
    } else if (fmt == MS_SAMPLE_FMT_UNKNOWN && is_csd) {
        st = walk_annexb(data, size, vps, &nv, sps, &ns, pps, &np);
    } else if (fmt == MS_SAMPLE_FMT_AVCC) {
        /* 不得把 avcC 误当 hvcC。 */
        return MS_ERR_UNSUPPORTED;
    } else {
        return MS_ERR_UNSUPPORTED;
    }
    if (st != MS_OK) {
        memset(out, 0, sizeof(*out));
        out->struct_size = sizeof(*out);
        return st;
    }
    ps_pick_triple(vps, nv, sps, ns, pps, np, out);
    return MS_OK;
}

int ms_hevc_ps_equal(const ms_hevc_ps_set *a, const ms_hevc_ps_set *b)
{
    if (a == NULL || b == NULL || !a->triple_complete || !b->triple_complete) {
        return 0;
    }
    if (a->vps_size != b->vps_size || a->sps_size != b->sps_size ||
        a->pps_size != b->pps_size) {
        return 0;
    }
    if (a->vps_size > 0 && memcmp(a->vps, b->vps, a->vps_size) != 0) {
        return 0;
    }
    if (a->sps_size > 0 && memcmp(a->sps, b->sps, a->sps_size) != 0) {
        return 0;
    }
    if (a->pps_size > 0 && memcmp(a->pps, b->pps, a->pps_size) != 0) {
        return 0;
    }
    return 1;
}

static ms_status append_annexb(uint8_t *out, size_t cap, size_t *n, const uint8_t *nal,
                               size_t nn)
{
    if (*n + 4 + nn > cap) {
        return MS_ERR_OVERFLOW;
    }
    out[(*n)++] = 0;
    out[(*n)++] = 0;
    out[(*n)++] = 0;
    out[(*n)++] = 1;
    memcpy(out + *n, nal, nn);
    *n += nn;
    return MS_OK;
}

ms_status ms_hevc_ps_pack(ms_sample_format fmt, unsigned nal_len_size,
                          const ms_hevc_ps_set *in, uint8_t *out, size_t cap,
                          size_t *out_size)
{
    size_t n = 0;
    if (in == NULL || !in->triple_complete || out == NULL || out_size == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    *out_size = 0;
    if (fmt == MS_SAMPLE_FMT_HVCC) {
        unsigned ls = nal_len_size ? nal_len_size : 4u;
        size_t need;
        if (ls < 1 || ls > 4) {
            return MS_ERR_INVALID_ARG;
        }
        need = 23 + 3 * 3 + 2 * 3 + in->vps_size + in->sps_size + in->pps_size;
        if (need > cap) {
            return MS_ERR_OVERFLOW;
        }
        memset(out, 0, 23);
        out[0] = 1;
        /* reserved bits in hvcC 须为 1；profile 字段可填 0。 */
        out[6] = 0xF0;
        out[7] = 0x00;
        out[8] = 0xFC;
        out[9] = 0xFC;
        out[10] = 0xF8;
        out[11] = 0xF8;
        out[21] = (uint8_t)(0xF8u | ((ls - 1u) & 3u));
        out[22] = 3; /* VPS, SPS, PPS 各一阵列 */
        n = 23;
        {
            unsigned types[3] = { HEVC_NAL_VPS, HEVC_NAL_SPS, HEVC_NAL_PPS };
            const uint8_t *nals[3] = { in->vps, in->sps, in->pps };
            size_t lens[3] = { in->vps_size, in->sps_size, in->pps_size };
            int i;
            for (i = 0; i < 3; i++) {
                out[n++] = (uint8_t)(0x80u | (types[i] & 0x3Fu));
                out[n++] = 0;
                out[n++] = 1;
                out[n++] = (uint8_t)((lens[i] >> 8) & 0xFFu);
                out[n++] = (uint8_t)(lens[i] & 0xFFu);
                memcpy(out + n, nals[i], lens[i]);
                n += lens[i];
            }
        }
        *out_size = n;
        return MS_OK;
    }
    /* 默认 Annex B */
    if (append_annexb(out, cap, &n, in->vps, in->vps_size) != MS_OK ||
        append_annexb(out, cap, &n, in->sps, in->sps_size) != MS_OK ||
        append_annexb(out, cap, &n, in->pps, in->pps_size) != MS_OK) {
        return MS_ERR_OVERFLOW;
    }
    *out_size = n;
    return MS_OK;
}

static void inspect_nal(const uint8_t *nal, size_t nn, ms_hevc_au_info *out)
{
    unsigned typ;
    if (nn == 0) {
        return;
    }
    typ = hevc_nal_type(nal, nn);
    out->nal_count++;
    if (typ <= 31u) {
        out->has_vcl = 1;
        if (ms_hevc_nal_is_irap(typ)) {
            out->is_irap = 1;
        }
        (void)first_slice_flag(nal, nn);
    } else if (typ == HEVC_NAL_VPS) {
        out->has_vps = 1;
    } else if (typ == HEVC_NAL_SPS) {
        out->has_sps = 1;
    } else if (typ == HEVC_NAL_PPS) {
        out->has_pps = 1;
    }
}

ms_status ms_hevc_au_inspect(ms_sample_format fmt, const uint8_t *data, size_t size,
                             unsigned nal_len_size, ms_hevc_au_info *out)
{
    if (out == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    out->struct_size = sizeof(*out);
    if (data == NULL || size == 0) {
        return MS_OK;
    }
    if (fmt == MS_SAMPLE_FMT_HVCC) {
        size_t off = 0;
        unsigned ls = nal_len_size ? nal_len_size : 4u;
        if (ls < 1 || ls > 4) {
            return MS_ERR_MALFORMED;
        }
        while (off < size) {
            uint64_t nalsz = 0;
            unsigned k;
            if (off + ls > size) {
                return MS_ERR_MALFORMED;
            }
            for (k = 0; k < ls; k++) {
                nalsz = (nalsz << 8) | (uint64_t)data[off + k];
            }
            off += ls;
            if (nalsz == 0 || off + nalsz > size) {
                return MS_ERR_MALFORMED;
            }
            inspect_nal(data + off, (size_t)nalsz, out);
            off += (size_t)nalsz;
        }
        return MS_OK;
    }
    {
        const uint8_t *end = data + size;
        const uint8_t *p = data;
        while (p < end) {
            size_t sc = 0;
            const uint8_t *nal = ps_start(p, end, &sc);
            const uint8_t *next;
            size_t nn;
            if (nal == NULL) {
                break;
            }
            nal += sc;
            next = ps_start(nal, end, &sc);
            nn = (size_t)((next ? next : end) - nal);
            inspect_nal(nal, nn, out);
            p = next ? next : end;
        }
    }
    return MS_OK;
}
