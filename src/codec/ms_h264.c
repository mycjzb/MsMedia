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

void ms_h264_set_meter(ms_h264 *h, ms_copy_meter *m)
{
    if (h != NULL) {
        h->meter = m;
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
    {
        ms_copy_meter *meter = h->meter;
        ms_h264_destroy(h);
        ms_h264_init(h, alloc, max_au, &cb);
        h->meter = meter;
    }
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

void ms_h264_recover(ms_h264 *h)
{
    if (h == NULL) {
        return;
    }
    ms_h264_flush_partial(h);
    h->failed = 0;
    h->fail_status = MS_OK;
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
    unsigned chroma = 1; /* 非 high profile 时规范默认 4:2:0 */
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
        unsigned present = 0, nlist, i;
        chroma = 0;
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
    /* pic_width/height_in_mbs 是 minus1。frame_mbs_only=1 时编码高即 map_units*16。 */
    out->width = (int)((width + 1u) * 16u);
    out->height = (int)((height + 1u) * 16u);
    {
        unsigned d8 = 0, crop_flag = 0;
        /* 裁剪读失败不推翻已接受的 SPS，保留宏块对齐尺寸。 */
        if (br_u(&br, 1, &d8) == 0 && br_u(&br, 1, &crop_flag) == 0 && crop_flag) {
            unsigned cl = 0, cr = 0, ct = 0, cb = 0;
            if (br_ue(&br, &cl) == 0 && br_ue(&br, &cr) == 0 &&
                br_ue(&br, &ct) == 0 && br_ue(&br, &cb) == 0) {
                unsigned sub_w = 2, sub_h = 2;
                int w2, h2;
                if (chroma == 0u || chroma == 3u) {
                    sub_w = 1;
                    sub_h = 1;
                } else if (chroma == 2u) {
                    sub_w = 2;
                    sub_h = 1;
                }
                w2 = out->width - (int)((cl + cr) * sub_w);
                h2 = out->height - (int)((ct + cb) * sub_h);
                if (w2 > 0 && h2 > 0) {
                    out->width = w2;
                    out->height = h2;
                }
            }
        }
        (void)d8;
    }
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
    {
        int taken = 0;
        uint8_t *rem_buf = NULL;
        size_t rem_n = (end < h->len) ? (h->len - end) : 0;
        size_t rem_cap = 0;
        if (h->buf != NULL && end > 0) {
            if (rem_n > 0) {
                rem_buf = (uint8_t *)ms_mem_alloc(h->alloc, rem_n);
                if (rem_buf != NULL) {
                    memcpy(rem_buf, h->buf + end, rem_n);
                    ms_copy_meter_add(h->meter, MS_COPY_ASM, rem_n);
                    rem_cap = rem_n;
                    fr.owned_base = h->buf;
                    fr.owned_cap = h->cap;
                    fr.owned_taken = &taken;
                }
            } else {
                fr.owned_base = h->buf;
                fr.owned_cap = h->cap;
                fr.owned_taken = &taken;
            }
        }
        if (h->cb.on_au != NULL) {
            st = h->cb.on_au(h->cb.ctx, &fr);
            if (st != MS_OK) {
                if (rem_buf != NULL) {
                    ms_mem_free(h->alloc, rem_buf);
                }
                /* config 已发出则 dirty 已清；AU 仍留在缓冲，重试时不再重复 config。 */
                return st;
            }
        }
        if (h->have_idr) {
            h->started = 1;
        }
        if (taken) {
            h->buf = rem_buf;
            h->cap = rem_cap;
            h->len = rem_n;
        } else {
            if (rem_buf != NULL) {
                ms_mem_free(h->alloc, rem_buf);
            }
            if (end < h->len) {
                memmove(h->buf, h->buf + end, h->len - end);
                h->len -= end;
            } else {
                h->len = 0;
            }
        }
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
        ms_copy_meter_add(h->meter, MS_COPY_ASM, chunk);
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

/* ---------- 公共：从 AU/CSD 提取本缓冲内成对 SPS/PPS ---------- */

typedef struct {
    uint8_t nal[MS_H264_PS_NAL_MAX];
    size_t n;
    unsigned id;
    unsigned sps_id; /* only PPS */
} ps_nal;

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

static ms_status ps_store(ps_nal *slots, int *n, const uint8_t *nal, size_t nn,
                          unsigned id, unsigned sps_id)
{
    if (nn == 0) {
        return MS_OK;
    }
    if (nn > MS_H264_PS_NAL_MAX) {
        return MS_ERR_OVERFLOW;
    }
    if (*n < MS_H264_PS_SLOT_MAX) {
        memcpy(slots[*n].nal, nal, nn);
        slots[*n].n = nn;
        slots[*n].id = id;
        slots[*n].sps_id = sps_id;
        (*n)++;
    } else {
        /* 有界：只保留最近 SLOT_MAX 个，挤掉最旧的。 */
        memmove(&slots[0], &slots[1], sizeof(slots[0]) * (MS_H264_PS_SLOT_MAX - 1));
        memcpy(slots[MS_H264_PS_SLOT_MAX - 1].nal, nal, nn);
        slots[MS_H264_PS_SLOT_MAX - 1].n = nn;
        slots[MS_H264_PS_SLOT_MAX - 1].id = id;
        slots[MS_H264_PS_SLOT_MAX - 1].sps_id = sps_id;
    }
    return MS_OK;
}

static ms_status ps_take_nal(const uint8_t *nal, size_t nn, ps_nal *sps, int *ns,
                             ps_nal *pps, int *np)
{
    unsigned typ;
    if (nal == NULL || nn == 0) {
        return MS_OK;
    }
    typ = (unsigned)(nal[0] & 0x1fu);
    if (typ == 7u) {
        ms_h264_sps tmp;
        if (parse_sps(nal, nn, &tmp) != 0) {
            return MS_OK; /* 无法解析则忽略，不当成可应用配置 */
        }
        return ps_store(sps, ns, nal, nn, tmp.id, tmp.id);
    }
    if (typ == 8u) {
        ms_h264_pps tmp;
        if (parse_pps(nal, nn, &tmp) != 0) {
            return MS_OK;
        }
        return ps_store(pps, np, nal, nn, tmp.id, tmp.sps_id);
    }
    return MS_OK;
}

static void ps_pick_pair(const ps_nal *sps, int ns, const ps_nal *pps, int np,
                         ms_h264_ps_set *out)
{
    int i, j;
    memset(out, 0, sizeof(*out));
    out->struct_size = sizeof(*out);
    if (ns > 0) {
        out->have_sps = 1;
        out->sps_size = sps[ns - 1].n;
        memcpy(out->sps, sps[ns - 1].nal, sps[ns - 1].n);
        out->sps_id = sps[ns - 1].id;
    }
    if (np > 0) {
        out->have_pps = 1;
        out->pps_size = pps[np - 1].n;
        memcpy(out->pps, pps[np - 1].nal, pps[np - 1].n);
        out->pps_id = pps[np - 1].id;
        out->pps_sps_id = pps[np - 1].sps_id;
    }
    /* 只在本缓冲内配对：从后往前找 PPS 与同缓冲 SPS.id 匹配，不拼接旧集。 */
    for (i = np - 1; i >= 0; i--) {
        for (j = ns - 1; j >= 0; j--) {
            if (pps[i].sps_id == sps[j].id) {
                out->have_sps = 1;
                out->have_pps = 1;
                out->sps_size = sps[j].n;
                memcpy(out->sps, sps[j].nal, sps[j].n);
                out->sps_id = sps[j].id;
                out->pps_size = pps[i].n;
                memcpy(out->pps, pps[i].nal, pps[i].n);
                out->pps_id = pps[i].id;
                out->pps_sps_id = pps[i].sps_id;
                out->pair_complete = 1;
                return;
            }
        }
    }
}

static ms_status ps_walk_annexb(const uint8_t *d, size_t n, ps_nal *sps, int *ns,
                                ps_nal *pps, int *np)
{
    const uint8_t *end = d + n;
    const uint8_t *p = d;
    while (p < end) {
        size_t sc = 0;
        const uint8_t *nal;
        const uint8_t *next;
        size_t nn;
        ms_status st;
        nal = ps_start(p, end, &sc);
        if (nal == NULL) {
            break;
        }
        nal += sc;
        next = ps_start(nal, end, &sc);
        nn = (size_t)((next ? next : end) - nal);
        st = ps_take_nal(nal, nn, sps, ns, pps, np);
        if (st != MS_OK) {
            return st;
        }
        p = next ? next : end;
    }
    return MS_OK;
}

static ms_status ps_walk_avcc_sample(const uint8_t *d, size_t n, unsigned ls,
                                     ps_nal *sps, int *ns, ps_nal *pps, int *np)
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
        st = ps_take_nal(d + off, (size_t)nalsz, sps, ns, pps, np);
        if (st != MS_OK) {
            return st;
        }
        off += (size_t)nalsz;
    }
    return MS_OK;
}

static ms_status ps_walk_avcc_cfg(const uint8_t *d, size_t n, ps_nal *sps, int *ns,
                                  ps_nal *pps, int *np, unsigned *out_ls)
{
    size_t off;
    unsigned i, nSPS, nPPS, ls;
    if (n < 7 || d[0] != 1) {
        return MS_ERR_MALFORMED;
    }
    ls = (unsigned)((d[4] & 3u) + 1u);
    if (out_ls) {
        *out_ls = ls;
    }
    off = 5;
    nSPS = (unsigned)(d[off++] & 0x1fu);
    for (i = 0; i < nSPS; i++) {
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
        st = ps_take_nal(d + off, ln, sps, ns, pps, np);
        if (st != MS_OK) {
            return st;
        }
        off += ln;
    }
    if (off >= n) {
        return MS_OK;
    }
    nPPS = (unsigned)d[off++];
    for (i = 0; i < nPPS; i++) {
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
        st = ps_take_nal(d + off, ln, sps, ns, pps, np);
        if (st != MS_OK) {
            return st;
        }
        off += ln;
    }
    return MS_OK;
}

ms_status ms_h264_ps_parse(ms_sample_format fmt, int is_csd, const uint8_t *data,
                           size_t size, unsigned nal_len_size, ms_h264_ps_set *out)
{
    ps_nal sps[MS_H264_PS_SLOT_MAX];
    ps_nal pps[MS_H264_PS_SLOT_MAX];
    int ns = 0, np = 0;
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
        (fmt == MS_SAMPLE_FMT_UNKNOWN && !is_csd)) {
        st = ps_walk_annexb(data, size, sps, &ns, pps, &np);
    } else if (fmt == MS_SAMPLE_FMT_AVCC && is_csd) {
        st = ps_walk_avcc_cfg(data, size, sps, &ns, pps, &np, NULL);
    } else if (fmt == MS_SAMPLE_FMT_AVCC) {
        st = ps_walk_avcc_sample(data, size, nal_len_size, sps, &ns, pps, &np);
    } else if (fmt == MS_SAMPLE_FMT_UNKNOWN && is_csd && size >= 7 && data[0] == 1) {
        st = ps_walk_avcc_cfg(data, size, sps, &ns, pps, &np, NULL);
    } else if (fmt == MS_SAMPLE_FMT_UNKNOWN && is_csd) {
        st = ps_walk_annexb(data, size, sps, &ns, pps, &np);
    } else {
        return MS_ERR_UNSUPPORTED;
    }
    if (st != MS_OK) {
        memset(out, 0, sizeof(*out));
        out->struct_size = sizeof(*out);
        return st;
    }
    ps_pick_pair(sps, ns, pps, np, out);
    return MS_OK;
}

ms_status ms_h264_csd_video_size(ms_sample_format fmt, int is_csd,
                                 const uint8_t *data, size_t size,
                                 unsigned nal_len_size, int *width, int *height)
{
    ms_h264_ps_set ps;
    ms_h264_sps tmp;
    ms_status st;
    if (width == NULL || height == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    *width = 0;
    *height = 0;
    memset(&ps, 0, sizeof(ps));
    ps.struct_size = sizeof(ps);
    st = ms_h264_ps_parse(fmt, is_csd, data, size, nal_len_size, &ps);
    if (st != MS_OK) {
        return st;
    }
    if (!ps.have_sps || ps.sps_size == 0) {
        return MS_OK;
    }
    if (parse_sps(ps.sps, ps.sps_size, &tmp) != 0 || tmp.width <= 0 || tmp.height <= 0) {
        return MS_OK;
    }
    *width = tmp.width;
    *height = tmp.height;
    return MS_OK;
}

int ms_h264_ps_equal(const ms_h264_ps_set *a, const ms_h264_ps_set *b)
{
    if (a == NULL || b == NULL) {
        return 0;
    }
    if (!a->pair_complete || !b->pair_complete) {
        return 0;
    }
    if (a->sps_size != b->sps_size || a->pps_size != b->pps_size) {
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

ms_status ms_h264_ps_pack(ms_sample_format fmt, unsigned nal_len_size,
                          const ms_h264_ps_set *in, uint8_t *out, size_t cap,
                          size_t *out_size)
{
    size_t n = 0;
    if (in == NULL || !in->pair_complete || out == NULL || out_size == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    *out_size = 0;
    if (fmt == MS_SAMPLE_FMT_AVCC) {
        unsigned ls = nal_len_size ? nal_len_size : 4u;
        if (ls < 1 || ls > 4) {
            return MS_ERR_INVALID_ARG;
        }
        if (6 + 2 + in->sps_size + 1 + 2 + in->pps_size > cap) {
            return MS_ERR_OVERFLOW;
        }
        out[n++] = 1;
        out[n++] = (in->sps_size >= 2) ? in->sps[1] : 0;
        out[n++] = (in->sps_size >= 3) ? in->sps[2] : 0;
        out[n++] = (in->sps_size >= 4) ? in->sps[3] : 0;
        out[n++] = (uint8_t)(0xFCu | ((ls - 1u) & 3u));
        out[n++] = (uint8_t)(0xE0u | 1u);
        out[n++] = (uint8_t)((in->sps_size >> 8) & 0xFFu);
        out[n++] = (uint8_t)(in->sps_size & 0xFFu);
        memcpy(out + n, in->sps, in->sps_size);
        n += in->sps_size;
        out[n++] = 1;
        out[n++] = (uint8_t)((in->pps_size >> 8) & 0xFFu);
        out[n++] = (uint8_t)(in->pps_size & 0xFFu);
        memcpy(out + n, in->pps, in->pps_size);
        n += in->pps_size;
        *out_size = n;
        return MS_OK;
    }
    if (fmt != MS_SAMPLE_FMT_ANNEXB && fmt != MS_SAMPLE_FMT_UNKNOWN) {
        return MS_ERR_UNSUPPORTED;
    }
    if (8 + in->sps_size + in->pps_size > cap) {
        return MS_ERR_OVERFLOW;
    }
    out[n++] = 0;
    out[n++] = 0;
    out[n++] = 0;
    out[n++] = 1;
    memcpy(out + n, in->sps, in->sps_size);
    n += in->sps_size;
    out[n++] = 0;
    out[n++] = 0;
    out[n++] = 0;
    out[n++] = 1;
    memcpy(out + n, in->pps, in->pps_size);
    n += in->pps_size;
    *out_size = n;
    return MS_OK;
}
