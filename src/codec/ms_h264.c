/*
 * H.264 Annex B 组帧：AUD 边界，或无 AUD 时按 7.4.1.2.4 比较主图像字段。
 * by: Long <mycjzb@gmail.com>
 */

#include "ms_h264.h"

#define MS_H264_RBSP_MAX 2048

static void free_param_sets(ms_h264 *h)
{
    unsigned i;

    if (h == NULL) {
        return;
    }
    for (i = 0; i <= MS_H264_SPS_ID_MAX; i++) {
        ms_mem_free(h->alloc, h->sps[i].nal);
        h->sps[i].nal = NULL;
        h->sps[i].nal_len = 0;
        h->sps[i].nal_cap = 0;
        h->sps[i].present = 0;
    }
    for (i = 0; i < MS_H264_PPS_SLOTS; i++) {
        ms_mem_free(h->alloc, h->pps[i].nal);
        h->pps[i].nal = NULL;
        h->pps[i].nal_len = 0;
        h->pps[i].nal_cap = 0;
        h->pps[i].present = 0;
    }
}

void ms_h264_init(ms_h264 *h,
                  const ms_allocator *alloc,
                  size_t max_au,
                  const ms_h264_callbacks *cb)
{
    memset(h, 0, sizeof(*h));
    h->alloc = alloc;
    h->max_au = (max_au == 0) ? MS_DEFAULT_AU_QUEUE_BYTES : max_au;
    if (cb != NULL) {
        h->cb = *cb;
    }
}

void ms_h264_destroy(ms_h264 *h)
{
    if (h == NULL) {
        return;
    }
    ms_mem_free(h->alloc, h->buf);
    free_param_sets(h);
    memset(h, 0, sizeof(*h));
}

static void clear_au_state(ms_h264 *h)
{
    h->have_aud = 0;
    h->have_vcl = 0;
    h->have_idr = 0;
    h->au_open = 0;
    h->have_start_ts = 0;
    h->start_has_pts = 0;
    h->start_has_dts = 0;
    h->start_pts = 0;
    h->start_dts = 0;
    h->have_prev_slice = 0;
    memset(&h->prev_slice, 0, sizeof(h->prev_slice));
    h->have_prefix = 0;
    h->prefix_off = 0;
    h->au_have_ids = 0;
    h->au_sps_id = 0;
    h->au_pps_id = 0;
}

void ms_h264_reset(ms_h264 *h)
{
    const ms_allocator *alloc;
    size_t max_au;
    ms_h264_callbacks cb;

    if (h == NULL) {
        return;
    }
    alloc = h->alloc;
    max_au = h->max_au;
    cb = h->cb;
    ms_h264_destroy(h);
    ms_h264_init(h, alloc, max_au, &cb);
}

void ms_h264_flush_partial(ms_h264 *h)
{
    if (h == NULL) {
        return;
    }
    ms_mem_free(h->alloc, h->buf);
    h->buf = NULL;
    h->len = 0;
    h->cap = 0;
    h->parsed = 0;
    h->pes_ts_available = 0;
    clear_au_state(h);
    /* SPS/PPS 与 config_dirty 保留 */
}

void ms_h264_begin_pes(ms_h264 *h, int has_pts, uint64_t pts, int has_dts, uint64_t dts)
{
    if (h == NULL) {
        return;
    }
    h->pes_ts_available = 1;
    h->pes_has_pts = has_pts;
    h->pes_pts = pts;
    h->pes_has_dts = has_dts;
    h->pes_dts = dts;
}

static void attach_start_ts(ms_h264 *h)
{
    if (h->have_start_ts) {
        return;
    }
    if (h->pes_ts_available) {
        h->have_start_ts = 1;
        h->start_has_pts = h->pes_has_pts;
        h->start_pts = h->pes_pts;
        h->start_has_dts = h->pes_has_dts;
        h->start_dts = h->pes_dts;
        h->pes_ts_available = 0;
        return;
    }
    h->have_start_ts = 1;
    h->start_has_pts = 0;
    h->start_has_dts = 0;
    h->start_pts = 0;
    h->start_dts = 0;
}

static int nal_type_of(uint8_t hdr)
{
    return (int)(hdr & 0x1Fu);
}

static int is_vcl_type(int t)
{
    return t == 1 || t == 5;
}

static int is_unsup_type(int t)
{
    /* 数据分割、扩展/MVC/SVC，以及未在本子集声明的类型。 */
    if (t == 2 || t == 3 || t == 4) {
        return 1;
    }
    if (t >= 14) {
        return 1;
    }
    return 0;
}

static int is_next_au_prefix_type(int t)
{
    /* 主图像之后的 SPS/PPS/SEI/AUD 属于下一访问单元的前缀。 */
    return t == 6 || t == 7 || t == 8 || t == 9;
}

typedef struct {
    const uint8_t *p;
    size_t n;
    size_t bit;
} ms_br;

static int br_u(ms_br *b, unsigned bits, unsigned *out)
{
    unsigned v = 0;
    unsigned i;
    if (b == NULL || out == NULL || bits == 0 || bits > 32) {
        return -1;
    }
    if (b->bit > (size_t)-1 - bits) {
        return -1;
    }
    if (b->bit + bits > b->n * 8u) {
        return -1;
    }
    for (i = 0; i < bits; i++) {
        size_t pos = b->bit + i;
        unsigned by = b->p[pos / 8u];
        unsigned bit = (unsigned)(by >> (7u - (pos % 8u))) & 1u;
        v = (v << 1) | bit;
    }
    b->bit += bits;
    *out = v;
    return 0;
}

static int br_ue(ms_br *b, unsigned *out)
{
    unsigned lz = 0;
    unsigned info = 0;
    unsigned bit;
    unsigned max_lz = 31;

    if (b == NULL || out == NULL) {
        return -1;
    }
    for (;;) {
        if (br_u(b, 1, &bit) != 0) {
            return -1;
        }
        if (bit == 1u) {
            break;
        }
        lz++;
        if (lz > max_lz) {
            return -1;
        }
    }
    if (lz == 0) {
        *out = 0;
        return 0;
    }
    if (br_u(b, lz, &info) != 0) {
        return -1;
    }
    if (info > (0xFFFFFFFFu - 1u) - ((1u << lz) - 1u)) {
        return -1;
    }
    *out = ((1u << lz) - 1u) + info;
    return 0;
}

static int br_se(ms_br *b, int *out)
{
    unsigned k;
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

static int skip_scaling_list(ms_br *b, int size)
{
    int last = 8;
    int next = 8;
    int i;
    for (i = 0; i < size; i++) {
        if (next != 0) {
            int delta;
            int tmp;
            if (br_se(b, &delta) != 0) {
                return -1;
            }
            tmp = last + delta;
            /* (x + 256) % 256，避免实现定义的负值取模。 */
            tmp %= 256;
            if (tmp < 0) {
                tmp += 256;
            }
            next = tmp;
        }
        if (next != 0) {
            last = next;
        }
    }
    return 0;
}

static int rbsp_unescape(const uint8_t *nal, size_t nal_len,
                         uint8_t *out, size_t cap, size_t *out_len)
{
    size_t i = 0;
    size_t o = 0;
    if (nal == NULL || out == NULL || out_len == NULL) {
        return -1;
    }
    *out_len = 0;
    /* 跳过 NAL 头一字节，只对 RBSP 去 emulation prevention。 */
    if (nal_len < 1) {
        return -1;
    }
    while (i < nal_len) {
        if (o >= cap) {
            return -1;
        }
        if (i + 2 < nal_len && nal[i] == 0 && nal[i + 1] == 0 && nal[i + 2] == 3) {
            if (i + 3 >= nal_len) {
                return -1;
            }
            /* 00 00 03 xx → 00 00 xx，且 xx 必须是 0..3。 */
            if (nal[i + 3] > 3u) {
                return -1;
            }
            out[o++] = 0;
            out[o++] = 0;
            i += 3;
            continue;
        }
        out[o++] = nal[i++];
    }
    *out_len = o;
    return 0;
}

static int high_profile(unsigned profile)
{
    return profile == 100u || profile == 110u || profile == 122u ||
           profile == 244u || profile == 44u || profile == 83u ||
           profile == 86u || profile == 118u || profile == 128u ||
           profile == 138u || profile == 139u || profile == 134u ||
           profile == 135u;
}

/*
 * 解析 SPS。返回 0 成功，1 为明确不支持的子集，-1 畸形。
 */
static int parse_sps(const uint8_t *nal, size_t nal_len, ms_h264_sps *out)
{
    uint8_t rbsp[MS_H264_RBSP_MAX];
    size_t rbsp_len = 0;
    ms_br br;
    unsigned profile = 0, flags = 0, level = 0, sps_id = 0;
    unsigned tmp = 0, poc_type = 0, width = 0, height = 0, fmo = 0;
    unsigned log2_fn = 0, log2_poc = 0;
    int delta_always = 0;
    const uint8_t *body;
    size_t body_len;

    if (out == NULL) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    if (nal_len < 2) {
        return -1;
    }
    body = nal + 1;
    body_len = nal_len - 1;
    if (rbsp_unescape(body, body_len, rbsp, sizeof(rbsp), &rbsp_len) != 0) {
        return -1;
    }
    memset(&br, 0, sizeof(br));
    br.p = rbsp;
    br.n = rbsp_len;
    if (br_u(&br, 8, &profile) != 0 || br_u(&br, 8, &flags) != 0 ||
        br_u(&br, 8, &level) != 0 || br_ue(&br, &sps_id) != 0) {
        return -1;
    }
    (void)flags;
    (void)level;
    if (sps_id > MS_H264_SPS_ID_MAX) {
        return -1;
    }
    if (high_profile(profile)) {
        unsigned chroma = 0, present = 0, nlist, i;
        if (br_ue(&br, &chroma) != 0) {
            return -1;
        }
        if (chroma == 3u) {
            unsigned sep = 0;
            if (br_u(&br, 1, &sep) != 0) {
                return -1;
            }
            if (sep != 0) {
                /* 4:4:4 分平面本子集不支持。 */
                return 1;
            }
        }
        if (br_ue(&br, &tmp) != 0 || br_ue(&br, &tmp) != 0 ||
            br_u(&br, 1, &tmp) != 0 || br_u(&br, 1, &present) != 0) {
            return -1;
        }
        if (present) {
            nlist = (chroma == 3u) ? 12u : 8u;
            for (i = 0; i < nlist; i++) {
                unsigned flag = 0;
                if (br_u(&br, 1, &flag) != 0) {
                    return -1;
                }
                if (flag) {
                    if (skip_scaling_list(&br, (i < 6u) ? 16 : 64) != 0) {
                        return -1;
                    }
                }
            }
        }
    }
    if (br_ue(&br, &log2_fn) != 0 || br_ue(&br, &poc_type) != 0) {
        return -1;
    }
    if (log2_fn > 12u) {
        return -1;
    }
    if (poc_type == 0) {
        if (br_ue(&br, &log2_poc) != 0) {
            return -1;
        }
        if (log2_poc > 12u) {
            return -1;
        }
    } else if (poc_type == 1) {
        unsigned always = 0, nref = 0, i;
        int se;
        if (br_u(&br, 1, &always) != 0 || br_se(&br, &se) != 0 ||
            br_se(&br, &se) != 0 || br_ue(&br, &nref) != 0) {
            return -1;
        }
        if (nref > 255u) {
            return -1;
        }
        for (i = 0; i < nref; i++) {
            if (br_se(&br, &se) != 0) {
                return -1;
            }
        }
        delta_always = (always != 0);
    } else if (poc_type != 2) {
        return -1;
    }
    if (br_ue(&br, &tmp) != 0 || br_u(&br, 1, &tmp) != 0 ||
        br_ue(&br, &width) != 0 || br_ue(&br, &height) != 0 ||
        br_u(&br, 1, &fmo) != 0) {
        return -1;
    }
    (void)width;
    (void)height;
    if (fmo == 0) {
        /* 隔行或 PAFF；若随后 mb_adaptive_frame_field_flag=1 则还是 MBAFF。一律拒绝。 */
        return 1;
    }
    out->present = 1;
    out->id = sps_id;
    out->log2_max_frame_num_minus4 = log2_fn;
    out->poc_type = poc_type;
    out->log2_max_poc_lsb_minus4 = log2_poc;
    out->delta_poc_always_zero = delta_always;
    out->frame_mbs_only = 1;
    return 0;
}

static int parse_pps(const uint8_t *nal, size_t nal_len, ms_h264_pps *out)
{
    uint8_t rbsp[MS_H264_RBSP_MAX];
    size_t rbsp_len = 0;
    ms_br br;
    unsigned pps_id = 0, sps_id = 0, entropy = 0, bottom = 0, groups = 0;

    if (out == NULL) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    if (nal_len < 2) {
        return -1;
    }
    if (rbsp_unescape(nal + 1, nal_len - 1, rbsp, sizeof(rbsp), &rbsp_len) != 0) {
        return -1;
    }
    memset(&br, 0, sizeof(br));
    br.p = rbsp;
    br.n = rbsp_len;
    if (br_ue(&br, &pps_id) != 0 || br_ue(&br, &sps_id) != 0 ||
        br_u(&br, 1, &entropy) != 0 || br_u(&br, 1, &bottom) != 0 ||
        br_ue(&br, &groups) != 0) {
        return -1;
    }
    (void)entropy;
    if (pps_id > MS_H264_PPS_ID_MAX || sps_id > MS_H264_SPS_ID_MAX) {
        return -1;
    }
    out->present = 1;
    out->id = pps_id;
    out->sps_id = sps_id;
    out->bottom_field_pic_order_in_frame = (bottom != 0);
    out->num_slice_groups_minus1 = groups;
    out->fmo = (groups != 0);
    return 0;
}

static ms_status store_nal_copy(ms_h264 *h, uint8_t **dst, size_t *len, size_t *cap,
                                const uint8_t *nal, size_t nal_len)
{
    ms_status st;
    if (nal_len == 0 || nal_len > MS_H264_RBSP_MAX + 1u) {
        return MS_ERR_MALFORMED;
    }
    st = ms_buf_reserve(h->alloc, dst, cap, nal_len, MS_H264_RBSP_MAX + 1u);
    if (st != MS_OK) {
        if (st == MS_LIMIT && ms_acct_is_budget_limit(h->alloc)) {
            return MS_LIMIT;
        }
        return (st == MS_LIMIT) ? MS_ERR_MALFORMED : st;
    }
    memcpy(*dst, nal, nal_len);
    *len = nal_len;
    return MS_OK;
}

static ms_h264_pps *find_pps(ms_h264 *h, unsigned id)
{
    unsigned i;
    for (i = 0; i < MS_H264_PPS_SLOTS; i++) {
        if (h->pps[i].present && h->pps[i].id == id) {
            return &h->pps[i];
        }
    }
    return NULL;
}

static const ms_h264_pps *find_pps_c(const ms_h264 *h, unsigned id)
{
    unsigned i;
    for (i = 0; i < MS_H264_PPS_SLOTS; i++) {
        if (h->pps[i].present && h->pps[i].id == id) {
            return &h->pps[i];
        }
    }
    return NULL;
}

static ms_h264_pps *alloc_pps_slot(ms_h264 *h, unsigned id)
{
    unsigned i;
    ms_h264_pps *found = find_pps(h, id);
    if (found != NULL) {
        return found;
    }
    for (i = 0; i < MS_H264_PPS_SLOTS; i++) {
        if (!h->pps[i].present) {
            return &h->pps[i];
        }
    }
    return NULL;
}

/*
 * 解析条带头直至 7.4.1.2.4 所需字段即停止。
 * 返回 0 成功，1 不支持，-1 畸形（含未知参数集）。
 */
static int parse_slice_header(ms_h264 *h, const uint8_t *nal, size_t nal_len,
                              int typ, unsigned nal_ref_idc, ms_h264_slice *out)
{
    uint8_t rbsp[MS_H264_RBSP_MAX];
    size_t rbsp_len = 0;
    ms_br br;
    unsigned first_mb = 0, slice_type = 0, pps_id = 0, frame_num = 0;
    unsigned fn_bits, poc_bits, poc_lsb = 0, idr_pic_id = 0;
    const ms_h264_pps *pps;
    const ms_h264_sps *sps;
    int idr = (typ == 5);

    if (out == NULL) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    if (nal_len < 2) {
        return -1;
    }
    if (rbsp_unescape(nal + 1, nal_len - 1, rbsp, sizeof(rbsp), &rbsp_len) != 0) {
        return -1;
    }
    memset(&br, 0, sizeof(br));
    br.p = rbsp;
    br.n = rbsp_len;
    if (br_ue(&br, &first_mb) != 0 || br_ue(&br, &slice_type) != 0 ||
        br_ue(&br, &pps_id) != 0) {
        return -1;
    }
    if (pps_id > MS_H264_PPS_ID_MAX) {
        return -1;
    }
    pps = find_pps_c(h, pps_id);
    if (pps == NULL || !pps->present) {
        return 2; /* 参数集尚未到达：等待，不是畸形 */
    }
    if (pps->fmo) {
        return 1;
    }
    if (pps->sps_id > MS_H264_SPS_ID_MAX || !h->sps[pps->sps_id].present) {
        return 2;
    }
    sps = &h->sps[pps->sps_id];
    if (!sps->frame_mbs_only) {
        return 1;
    }
    fn_bits = sps->log2_max_frame_num_minus4 + 4u;
    if (fn_bits < 4u || fn_bits > 16u) {
        return -1;
    }
    if (br_u(&br, fn_bits, &frame_num) != 0) {
        return -1;
    }
    /* 本子集要求 frame_mbs_only=1，码流中不出现 field_pic_flag。 */
    if (idr) {
        if (br_ue(&br, &idr_pic_id) != 0) {
            return -1;
        }
    }
    out->first_mb = first_mb;
    out->slice_type = slice_type;
    out->pps_id = pps_id;
    out->sps_id = sps->id;
    out->frame_num = frame_num;
    out->nal_ref_idc = nal_ref_idc;
    out->idr = idr;
    out->idr_pic_id = idr_pic_id;
    out->poc_type = sps->poc_type;
    if (sps->poc_type == 0) {
        poc_bits = sps->log2_max_poc_lsb_minus4 + 4u;
        if (poc_bits < 4u || poc_bits > 16u) {
            return -1;
        }
        if (br_u(&br, poc_bits, &poc_lsb) != 0) {
            return -1;
        }
        out->poc_lsb = poc_lsb;
        if (pps->bottom_field_pic_order_in_frame) {
            int db = 0;
            if (br_se(&br, &db) != 0) {
                return -1;
            }
            out->has_delta_bottom = 1;
            out->delta_bottom = db;
        }
    } else if (sps->poc_type == 1 && !sps->delta_poc_always_zero) {
        int d0 = 0, d1 = 0;
        if (br_se(&br, &d0) != 0) {
            return -1;
        }
        out->has_delta0 = 1;
        out->delta0 = d0;
        if (pps->bottom_field_pic_order_in_frame) {
            if (br_se(&br, &d1) != 0) {
                return -1;
            }
            out->has_delta1 = 1;
            out->delta1 = d1;
        }
    }
    return 0;
}

/* ITU-T H.264 7.4.1.2.4：主编码图像的首个 VCL。不用 first_mb_in_slice。 */
static int is_new_primary_picture(const ms_h264_slice *prev, const ms_h264_slice *cur)
{
    if (prev == NULL || cur == NULL) {
        return 0;
    }
    if (cur->frame_num != prev->frame_num) {
        return 1;
    }
    if (cur->pps_id != prev->pps_id) {
        return 1;
    }
    if ((cur->nal_ref_idc == 0) != (prev->nal_ref_idc == 0)) {
        return 1;
    }
    if (cur->idr != prev->idr) {
        return 1;
    }
    if (cur->idr && prev->idr && cur->idr_pic_id != prev->idr_pic_id) {
        return 1;
    }
    if (cur->poc_type == 0 && prev->poc_type == 0) {
        if (cur->poc_lsb != prev->poc_lsb) {
            return 1;
        }
        if (cur->has_delta_bottom != prev->has_delta_bottom ||
            (cur->has_delta_bottom && cur->delta_bottom != prev->delta_bottom)) {
            return 1;
        }
    }
    if (cur->poc_type == 1 && prev->poc_type == 1) {
        if (cur->has_delta0 != prev->has_delta0 ||
            (cur->has_delta0 && cur->delta0 != prev->delta0)) {
            return 1;
        }
        if (cur->has_delta1 != prev->has_delta1 ||
            (cur->has_delta1 && cur->delta1 != prev->delta1)) {
            return 1;
        }
    }
    return 0;
}

static int find_start_code(const uint8_t *b, size_t len, size_t from,
                           size_t *sc_off, int *sc_len)
{
    size_t i;
    if (b == NULL || sc_off == NULL || sc_len == NULL) {
        return 0;
    }
    if (len < 3 || from >= len) {
        return 0;
    }
    for (i = from; i + 2 < len; i++) {
        if (b[i] == 0 && b[i + 1] == 0 && b[i + 2] == 1) {
            if (i > 0 && b[i - 1] == 0 && i >= from) {
                /* 4 字节起始码；from 已落在该 00 上时不要再往前借一字节。 */
                if (i > from) {
                    *sc_off = i - 1;
                    *sc_len = 4;
                    return 1;
                }
            }
            *sc_off = i;
            *sc_len = 3;
            return 1;
        }
    }
    return 0;
}

static int maybe_start_prefix(const uint8_t *b, size_t len)
{
    if (len == 0) {
        return 1;
    }
    if (b[len - 1] == 0) {
        return 1;
    }
    return 0;
}

static ms_status fail_h(ms_h264 *h, ms_status st)
{
    h->failed = 1;
    h->fail_status = st;
    return st;
}

static const ms_h264_sps *cfg_sps(const ms_h264 *h)
{
    unsigned id;
    if (h->au_have_ids) {
        id = h->au_sps_id;
    } else {
        id = h->last_sps_id;
    }
    if (id > MS_H264_SPS_ID_MAX || !h->sps[id].present) {
        return NULL;
    }
    return &h->sps[id];
}

static const ms_h264_pps *cfg_pps(const ms_h264 *h)
{
    unsigned id;
    if (h->au_have_ids) {
        id = h->au_pps_id;
    } else {
        id = h->last_pps_id;
    }
    return find_pps_c(h, id);
}

static ms_status emit_config_if_needed(ms_h264 *h)
{
    uint8_t cfg[2 * (MS_H264_RBSP_MAX + 8)];
    size_t n = 0;
    ms_status st;
    const ms_h264_sps *sps;
    const ms_h264_pps *pps;

    if (!h->have_sps || !h->have_pps) {
        return MS_OK;
    }
    sps = cfg_sps(h);
    pps = cfg_pps(h);
    if (sps == NULL || pps == NULL || sps->nal == NULL || pps->nal == NULL) {
        return MS_OK;
    }
    if (!h->config_dirty && h->have_last_cfg &&
        h->last_cfg_sps_id == sps->id && h->last_cfg_pps_id == pps->id) {
        return MS_OK;
    }
    if (h->cb.on_config == NULL) {
        h->config_dirty = 0;
        h->have_last_cfg = 1;
        h->last_cfg_sps_id = sps->id;
        h->last_cfg_pps_id = pps->id;
        return MS_OK;
    }
    /* 4 字节起始码 + SPS + 4 字节起始码 + PPS */
    if (sps->nal_len + pps->nal_len + 8u > sizeof(cfg)) {
        return fail_h(h, MS_ERR_OVERFLOW);
    }
    cfg[n++] = 0;
    cfg[n++] = 0;
    cfg[n++] = 0;
    cfg[n++] = 1;
    memcpy(cfg + n, sps->nal, sps->nal_len);
    n += sps->nal_len;
    cfg[n++] = 0;
    cfg[n++] = 0;
    cfg[n++] = 0;
    cfg[n++] = 1;
    memcpy(cfg + n, pps->nal, pps->nal_len);
    n += pps->nal_len;
    st = h->cb.on_config(h->cb.ctx, cfg, n);
    if (st != MS_OK) {
        return st;
    }
    h->config_dirty = 0;
    h->have_last_cfg = 1;
    h->last_cfg_sps_id = sps->id;
    h->last_cfg_pps_id = pps->id;
    return MS_OK;
}

static ms_status emit_au(ms_h264 *h, size_t end)
{
    ms_au_frame fr;
    ms_status st;
    int changed;
    const ms_h264_sps *sps;
    const ms_h264_pps *pps;

    if (end > h->len || end == 0) {
        return fail_h(h, MS_ERR_STATE);
    }
    if (!h->have_sps || !h->have_pps || (!h->have_idr && !h->started)) {
        /* 中途接入：还没有参数集，或尚未发出过 IDR。丢弃本单元，
         * 不把残片交给解码，也不把会话标为失败。 */
        if (end < h->len) {
            memmove(h->buf, h->buf + end, h->len - end);
            h->len -= end;
        } else {
            h->len = 0;
        }
        if (h->parsed >= end) {
            h->parsed -= end;
        } else {
            h->parsed = 0;
        }
        clear_au_state(h);
        return MS_OK;
    }
    sps = cfg_sps(h);
    pps = cfg_pps(h);
    if (sps == NULL || pps == NULL) {
        return fail_h(h, MS_ERR_UNSUPPORTED);
    }
    changed = h->config_dirty ||
              !h->have_last_cfg ||
              h->last_cfg_sps_id != sps->id ||
              h->last_cfg_pps_id != pps->id;
    st = emit_config_if_needed(h);
    if (st != MS_OK) {
        return st;
    }
    attach_start_ts(h);
    memset(&fr, 0, sizeof(fr));
    fr.data = h->buf;
    fr.size = end;
    fr.has_pts = h->start_has_pts;
    fr.has_dts = h->start_has_dts;
    fr.pts = h->start_pts;
    fr.dts = h->start_dts;
    fr.format_changed = changed;
    fr.is_keyframe = h->have_idr ? 1 : 0;
    if (h->cb.on_au != NULL) {
        st = h->cb.on_au(h->cb.ctx, &fr);
        if (st != MS_OK) {
            /* config 已发出则 dirty 已清；AU 仍留在缓冲，重试时不再重复 config。 */
            return st;
        }
    }
    if (h->have_idr) {
        h->started = 1;
    }
    if (end < h->len) {
        memmove(h->buf, h->buf + end, h->len - end);
        h->len -= end;
    } else {
        h->len = 0;
    }
    if (h->parsed >= end) {
        h->parsed -= end;
    } else {
        h->parsed = 0;
    }
    /* 下一访问单元的时间戳不得沿用，也不按帧率推算。 */
    clear_au_state(h);
    return MS_OK;
}

static ms_status store_sps(ms_h264 *h, const uint8_t *nal, size_t nal_len,
                           const ms_h264_sps *parsed)
{
    ms_h264_sps *slot;
    ms_status st;
    int same;

    if (parsed->id > MS_H264_SPS_ID_MAX) {
        return MS_ERR_MALFORMED;
    }
    slot = &h->sps[parsed->id];
    same = (slot->present && slot->nal != NULL && slot->nal_len == nal_len &&
            memcmp(slot->nal, nal, nal_len) == 0);
    st = store_nal_copy(h, &slot->nal, &slot->nal_len, &slot->nal_cap, nal, nal_len);
    if (st != MS_OK) {
        return st;
    }
    slot->present = 1;
    slot->id = parsed->id;
    slot->log2_max_frame_num_minus4 = parsed->log2_max_frame_num_minus4;
    slot->poc_type = parsed->poc_type;
    slot->log2_max_poc_lsb_minus4 = parsed->log2_max_poc_lsb_minus4;
    slot->delta_poc_always_zero = parsed->delta_poc_always_zero;
    slot->frame_mbs_only = parsed->frame_mbs_only;
    h->have_sps = 1;
    h->last_sps_id = parsed->id;
    if (!same) {
        h->config_dirty = 1;
    }
    return MS_OK;
}

static ms_status store_pps(ms_h264 *h, const uint8_t *nal, size_t nal_len,
                           const ms_h264_pps *parsed)
{
    ms_h264_pps *slot;
    ms_status st;
    int same;

    slot = alloc_pps_slot(h, parsed->id);
    if (slot == NULL) {
        return MS_ERR_UNSUPPORTED;
    }
    same = (slot->present && slot->nal != NULL && slot->nal_len == nal_len &&
            memcmp(slot->nal, nal, nal_len) == 0);
    st = store_nal_copy(h, &slot->nal, &slot->nal_len, &slot->nal_cap, nal, nal_len);
    if (st != MS_OK) {
        return st;
    }
    slot->present = 1;
    slot->id = parsed->id;
    slot->sps_id = parsed->sps_id;
    slot->bottom_field_pic_order_in_frame = parsed->bottom_field_pic_order_in_frame;
    slot->num_slice_groups_minus1 = parsed->num_slice_groups_minus1;
    slot->fmo = parsed->fmo;
    h->have_pps = 1;
    h->last_pps_id = parsed->id;
    if (!same) {
        h->config_dirty = 1;
    }
    return MS_OK;
}

static void mark_prefix(ms_h264 *h, size_t sc_off)
{
    if (h->have_vcl && !h->have_prefix) {
        h->have_prefix = 1;
        h->prefix_off = sc_off;
    }
}

static void accept_vcl(ms_h264 *h, int typ, const ms_h264_slice *sl, int have_hdr)
{
    h->have_vcl = 1;
    if (typ == 5) {
        h->have_idr = 1;
    }
    h->au_open = 1;
    h->have_prefix = 0;
    h->prefix_off = 0;
    if (have_hdr && sl != NULL) {
        h->prev_slice = *sl;
        h->have_prev_slice = 1;
        if (!h->au_have_ids) {
            h->au_have_ids = 1;
            h->au_sps_id = sl->sps_id;
            h->au_pps_id = sl->pps_id;
        }
    }
    attach_start_ts(h);
}

static ms_status handle_nal(ms_h264 *h, size_t sc_off, int sc_len, size_t nal_end,
                            int *emit_before, size_t *emit_end)
{
    const uint8_t *nal;
    size_t nal_len;
    uint8_t hdr;
    int typ;
    int pr;
    unsigned nal_ref;
    ms_status st;
    ms_h264_sps sps_tmp;
    ms_h264_pps pps_tmp;
    ms_h264_slice sl;
    int hdr_ok;

    *emit_before = 0;
    *emit_end = 0;
    if (nal_end <= sc_off + (size_t)sc_len) {
        return fail_h(h, MS_ERR_MALFORMED);
    }
    nal = h->buf + sc_off + (size_t)sc_len;
    nal_len = nal_end - (sc_off + (size_t)sc_len);
    if (nal_len < 1) {
        return fail_h(h, MS_ERR_MALFORMED);
    }
    hdr = nal[0];
    if ((hdr & 0x80u) != 0) {
        return fail_h(h, MS_ERR_MALFORMED);
    }
    typ = nal_type_of(hdr);
    nal_ref = (unsigned)((hdr >> 5) & 0x03u);
    if (is_unsup_type(typ)) {
        return fail_h(h, MS_ERR_UNSUPPORTED);
    }
    if (is_next_au_prefix_type(typ) && h->have_vcl) {
        /* 主图像之后的 AUD/SPS/PPS/SEI 属于下一 AU 前缀：先结束上一单元。 */
        mark_prefix(h, sc_off);
        *emit_before = 1;
        *emit_end = h->have_prefix ? h->prefix_off : sc_off;
        return MS_OK;
    }
    if (is_next_au_prefix_type(typ)) {
        mark_prefix(h, sc_off);
    }
    if (typ == 9) {
        if (h->have_vcl) {
            *emit_before = 1;
            *emit_end = sc_off;
        }
        h->au_open = 1;
        h->have_aud = 1;
        /* have_vcl/idr 在 emit 后由 clear 清掉；若这里是空 AU 上的新 AUD，重置图像标志。 */
        if (!(*emit_before)) {
            h->have_vcl = 0;
            h->have_idr = 0;
            h->have_prev_slice = 0;
        }
        attach_start_ts(h);
        return MS_OK;
    }
    if (typ == 7) {
        memset(&sps_tmp, 0, sizeof(sps_tmp));
        pr = parse_sps(nal, nal_len, &sps_tmp);
        if (pr < 0) {
            return fail_h(h, MS_ERR_MALFORMED);
        }
        if (pr > 0) {
            return fail_h(h, MS_ERR_UNSUPPORTED);
        }
        st = store_sps(h, nal, nal_len, &sps_tmp);
        if (st != MS_OK) {
            if (st == MS_LIMIT || st == MS_ERR_NOMEM) {
                return st;
            }
            return fail_h(h, st);
        }
        h->au_open = 1;
        attach_start_ts(h);
        return MS_OK;
    }
    if (typ == 8) {
        memset(&pps_tmp, 0, sizeof(pps_tmp));
        if (parse_pps(nal, nal_len, &pps_tmp) != 0) {
            return fail_h(h, MS_ERR_MALFORMED);
        }
        if (pps_tmp.fmo) {
            /* 先记下该 ID，条带引用时再报 UNSUPPORTED，避免被当成「未知 PPS」。 */
            st = store_pps(h, nal, nal_len, &pps_tmp);
            if (st != MS_OK) {
                if (st == MS_LIMIT || st == MS_ERR_NOMEM) {
                    return st;
                }
                return fail_h(h, st);
            }
            return fail_h(h, MS_ERR_UNSUPPORTED);
        }
        st = store_pps(h, nal, nal_len, &pps_tmp);
        if (st != MS_OK) {
            if (st == MS_LIMIT || st == MS_ERR_NOMEM) {
                return st;
            }
            return fail_h(h, st);
        }
        h->au_open = 1;
        attach_start_ts(h);
        return MS_OK;
    }
    if (is_vcl_type(typ)) {
        memset(&sl, 0, sizeof(sl));
        pr = parse_slice_header(h, nal, nal_len, typ, nal_ref, &sl);
        hdr_ok = (pr == 0);
        if (h->have_aud) {
            /*
             * 保留 AUD 路径：AUD 已给出 AU 边界，条带头即使不完整
             * （历史合成样本的占位载荷）也不拆帧、不失败。
             */
            if (pr == 1) {
                return fail_h(h, MS_ERR_UNSUPPORTED);
            }
            accept_vcl(h, typ, &sl, hdr_ok);
            return MS_OK;
        }
        if (pr == 2) {
            /* 尚无参数集：等待，记下已有 VCL 以便后续前缀结束并丢弃残片。 */
            h->au_open = 1;
            h->have_vcl = 1;
            attach_start_ts(h);
            return MS_OK;
        }
        if (pr < 0) {
            return fail_h(h, MS_ERR_MALFORMED);
        }
        if (pr > 0) {
            return fail_h(h, MS_ERR_UNSUPPORTED);
        }
        if (h->have_vcl && h->have_prev_slice &&
            is_new_primary_picture(&h->prev_slice, &sl)) {
            *emit_before = 1;
            *emit_end = h->have_prefix ? h->prefix_off : sc_off;
            return MS_OK;
        }
        accept_vcl(h, typ, &sl, 1);
        return MS_OK;
    }
    /* SEI / 序列结束 / 流结束 / 填充：若已在 AU 中则收下，否则作为前缀。 */
    h->au_open = 1;
    attach_start_ts(h);
    return MS_OK;
}

static ms_status drain_buf(ms_h264 *h, int eos)
{
    if (h->failed) {
        return h->fail_status;
    }
    for (;;) {
        size_t sc1 = 0, sc2 = 0;
        int len1 = 0, len2 = 0;
        int got1, got2;
        int emit_before = 0;
        size_t emit_end = 0;
        size_t nal_end;
        ms_status st;
        size_t i;

        if (h->parsed > h->len) {
            h->parsed = 0;
        }
        /* 尚未找到第一个起始码：前导 0 可等待，非 0 且无法再构成起始码则畸形。 */
        got1 = find_start_code(h->buf, h->len, h->parsed, &sc1, &len1);
        if (!got1) {
            if (eos) {
                h->len = 0;
                h->parsed = 0;
                h->pes_ts_available = 0;
                clear_au_state(h);
                return MS_OK;
            }
            if (h->len >= 3 && !maybe_start_prefix(h->buf, h->len)) {
                for (i = 0; i < h->len; i++) {
                    if (h->buf[i] != 0) {
                        return fail_h(h, MS_ERR_MALFORMED);
                    }
                }
            }
            return MS_OK;
        }
        for (i = h->parsed; i < sc1; i++) {
            if (h->buf[i] != 0) {
                return fail_h(h, MS_ERR_MALFORMED);
            }
        }
        got2 = find_start_code(h->buf, h->len, sc1 + (size_t)len1, &sc2, &len2);
        if (!got2) {
            if (!eos) {
                attach_start_ts(h);
                if (h->len > h->max_au) {
                    return fail_h(h, MS_ERR_MALFORMED);
                }
                return MS_OK;
            }
            if (h->len < sc1 + (size_t)len1 + 1u) {
                /* 只有起始码没有 NAL 头：按截断丢弃，不越界。 */
                if (h->have_vcl) {
                    size_t end = h->have_prefix ? h->prefix_off : sc1;
                    if (end > 0) {
                        st = emit_au(h, end);
                        if (st != MS_OK) {
                            return st;
                        }
                    }
                }
                h->len = 0;
                h->parsed = 0;
                h->pes_ts_available = 0;
                clear_au_state(h);
                return MS_OK;
            }
            nal_end = h->len;
        } else {
            nal_end = sc2;
        }
        st = handle_nal(h, sc1, len1, nal_end, &emit_before, &emit_end);
        if (st != MS_OK) {
            return st;
        }
        if (emit_before) {
            if (emit_end == 0 || emit_end > h->len) {
                emit_end = sc1;
            }
            st = emit_au(h, emit_end);
            if (st != MS_OK) {
                return st;
            }
            /* 下一 AU 的前缀或新 VCL 已移到缓冲头部，下一轮再处理。 */
            continue;
        }
        if (got2) {
            h->parsed = sc2;
        } else {
            /* EOS 上最后一枚 NAL 已处理。若已有完整 AU，下面发出。 */
            h->parsed = h->len;
            break;
        }
    }
    if (eos && h->have_vcl) {
        size_t end = h->have_prefix ? h->prefix_off : h->len;
        ms_status st;
        if (end == 0) {
            h->len = 0;
            h->parsed = 0;
            h->pes_ts_available = 0;
            clear_au_state(h);
            return MS_OK;
        }
        st = emit_au(h, end);
        if (st != MS_OK) {
            return st;
        }
        h->len = 0;
        h->parsed = 0;
        h->pes_ts_available = 0;
        clear_au_state(h);
    } else if (eos) {
        h->len = 0;
        h->parsed = 0;
        h->pes_ts_available = 0;
        clear_au_state(h);
    }
    return MS_OK;
}

ms_status ms_h264_drain(ms_h264 *h)
{
    if (h == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    return drain_buf(h, 0);
}

ms_status ms_h264_push(ms_h264 *h, const uint8_t *data, size_t size, size_t *consumed)
{
    ms_status st;
    size_t need;
    size_t accepted;

    if (consumed != NULL) {
        *consumed = 0;
    }
    if (h == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    if (h->failed) {
        return h->fail_status;
    }
    if (size > 0 && data == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    st = drain_buf(h, 0);
    if (st != MS_OK) {
        return st;
    }
    accepted = 0;
    while (size > 0) {
        size_t room;
        size_t chunk;

        st = drain_buf(h, 0);
        if (st != MS_OK) {
            if (consumed != NULL) {
                *consumed = accepted;
            }
            return st;
        }
        if (h->len > h->max_au) {
            return fail_h(h, MS_ERR_MALFORMED);
        }
        room = h->max_au - h->len;
        if (room == 0) {
            return fail_h(h, MS_ERR_MALFORMED);
        }
        chunk = (size < room) ? size : room;
        if (ms_size_add(h->len, chunk, &need) != 0) {
            return fail_h(h, MS_ERR_OVERFLOW);
        }
        st = ms_buf_reserve(h->alloc, &h->buf, &h->cap, need, h->max_au);
        if (st != MS_OK) {
            if (st == MS_LIMIT && ms_acct_is_budget_limit(h->alloc)) {
                return MS_LIMIT;
            }
            if (st == MS_LIMIT) {
                return fail_h(h, MS_ERR_MALFORMED);
            }
            /* NOMEM：不把会话打成失败，旧缓冲仍可用。 */
            return st;
        }
        memcpy(h->buf + h->len, data, chunk);
        h->len = need;
        attach_start_ts(h);
        data += chunk;
        size -= chunk;
        accepted += chunk;
        if (consumed != NULL) {
            *consumed = accepted;
        }
    }
    st = drain_buf(h, 0);
    if (consumed != NULL) {
        *consumed = accepted;
    }
    return st;
}

ms_status ms_h264_end(ms_h264 *h)
{
    if (h == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    if (h->failed) {
        return h->fail_status;
    }
    return drain_buf(h, 1);
}
