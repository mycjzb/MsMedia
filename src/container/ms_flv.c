/*
 * 经典 FLV：有界 tag 解析，复用 avcC/ASC 校验与会话事件队列。
 * by: Long <mycjzb@gmail.com>
 */
#include "ms_flv.h"
#include "ms_avcc.h"

#include <string.h>

#define FLV_ST_HEADER 0
#define FLV_ST_HPAD   1
#define FLV_ST_PREV   2
#define FLV_ST_TAGHDR 3
#define FLV_ST_BODY   4
#define FLV_ST_SKIP   5
#define FLV_ST_EMIT   6

#define FLV_TAG_AUDIO  8
#define FLV_TAG_VIDEO  9
#define FLV_TAG_SCRIPT 18
#define FLV_TID_VIDEO  1u
#define FLV_TID_AUDIO  2u
#define FLV_MAX_HDR_PAD 4096u

enum {
    FLV_PEND_NONE = 0,
    FLV_PEND_FORMAT,
    FLV_PEND_TRACK,
    FLV_PEND_CONFIG,
    FLV_PEND_AU
};

struct flv_trk {
    int announced;
    int have_cfg;
    int cfg_changed;
    int saw_au_wo_cfg;
    uint8_t *cfg;
    size_t cfg_n;
    uint8_t nal_len_size;
    int hevc; /* 视频：0=H.264，1=HEVC（国内扩展 CodecID 12 或 Enhanced RTMP 'hvc1'） */
};

struct ms_flv_parser {
    const ms_allocator *alloc;
    ms_flv_callbacks cb;
    size_t max_assembly;
    int failed;
    ms_status fail_st;
    int state;
    int format_emitted;
    int unsup;
    int eos;
    uint8_t scratch[11];
    size_t scratch_n;
    uint32_t data_offset;
    uint32_t pad_left;
    uint32_t prev_expect;
    uint8_t tag_type;
    uint32_t data_size;
    uint32_t dts;
    uint32_t stream_id;
    uint8_t *body;
    size_t body_n;
    size_t body_cap;
    uint32_t skip_left;
    struct flv_trk video;
    struct flv_trk audio;
    int pend;
    int pend_video;
    int tag_out_done; /* 本 tag 的事件已全部入队，反压重试时不得再组一次 */
    ms_au_frame pend_fr;
    ms_track_info pend_ti;
    int body_taken;
    ms_copy_meter *meter;
};

size_t ms_flv_parser_size(void)
{
    return sizeof(struct ms_flv_parser);
}

static uint32_t rd_be24(const uint8_t *p)
{
    return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | (uint32_t)p[2];
}

static uint32_t rd_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static int32_t rd_si24(const uint8_t *p)
{
    uint32_t u = rd_be24(p);
    if (u & 0x800000u) {
        return (int32_t)(u | 0xFF000000u);
    }
    return (int32_t)u;
}

static void fail_set(ms_flv_parser *p, ms_status st)
{
    if (!p->failed) {
        p->failed = 1;
        p->fail_st = st;
    }
}

static void body_free(ms_flv_parser *p)
{
    if (p->body != NULL) {
        ms_mem_free(p->alloc, p->body);
        p->body = NULL;
    }
    p->body_n = 0;
    p->body_cap = 0;
}

static void trk_reset(const ms_allocator *a, struct flv_trk *t)
{
    if (t->cfg != NULL) {
        ms_mem_free(a, t->cfg);
    }
    memset(t, 0, sizeof(*t));
}

void ms_flv_parser_reset(ms_flv_parser *p)
{
    const ms_allocator *a;
    ms_flv_callbacks cb;
    size_t maxa;
    if (p == NULL) {
        return;
    }
    a = p->alloc;
    cb = p->cb;
    maxa = p->max_assembly;
    {
        ms_copy_meter *meter = p->meter;
        body_free(p);
        trk_reset(a, &p->video);
        trk_reset(a, &p->audio);
        memset(p, 0, sizeof(*p));
        p->alloc = a;
        p->cb = cb;
        p->max_assembly = maxa;
        p->meter = meter;
        p->state = FLV_ST_HEADER;
    }
}

void ms_flv_parser_set_meter(ms_flv_parser *p, ms_copy_meter *m)
{
    if (p != NULL) {
        p->meter = m;
    }
}

void ms_flv_parser_init(ms_flv_parser *p, const ms_allocator *alloc,
                        const ms_flv_callbacks *cb)
{
    memset(p, 0, sizeof(*p));
    p->alloc = alloc;
    if (cb != NULL) {
        p->cb = *cb;
    }
    p->max_assembly = 2u * 1024u * 1024u;
    p->state = FLV_ST_HEADER;
}

void ms_flv_parser_set_limits(ms_flv_parser *p, size_t max_assembly_bytes)
{
    if (p != NULL && max_assembly_bytes > 0) {
        p->max_assembly = max_assembly_bytes;
    }
}

void ms_flv_parser_destroy(ms_flv_parser *p)
{
    if (p == NULL) {
        return;
    }
    body_free(p);
    trk_reset(p->alloc, &p->video);
    trk_reset(p->alloc, &p->audio);
    memset(p, 0, sizeof(*p));
}

void ms_flv_flush_partial(ms_flv_parser *p)
{
    if (p == NULL) {
        return;
    }
    body_free(p);
    p->scratch_n = 0;
    p->skip_left = 0;
    p->pad_left = 0;
    p->pend = FLV_PEND_NONE;
    if (p->state != FLV_ST_HEADER && p->state != FLV_ST_HPAD) {
        p->state = FLV_ST_PREV;
    }
}

int ms_flv_parser_need_unsup(const ms_flv_parser *p)
{
    return (p != NULL) ? p->unsup : 0;
}

int ms_flv_probe(const uint8_t *data, size_t size)
{
    if (data == NULL || size == 0) {
        return -1;
    }
    if (data[0] != (uint8_t)'F') {
        return 0;
    }
    if (size < 2) {
        return -1;
    }
    if (data[1] != (uint8_t)'L') {
        return 0;
    }
    if (size < 3) {
        return -1;
    }
    if (data[2] != (uint8_t)'V') {
        return 0;
    }
    return 1;
}

static void fill_track_c(ms_track_info *ti, int video, int hevc)
{
    memset(ti, 0, sizeof(*ti));
    ti->id = video ? FLV_TID_VIDEO : FLV_TID_AUDIO;
    ti->kind = video ? MS_TRACK_VIDEO : MS_TRACK_AUDIO;
    ti->codec = video ? (hevc ? MS_CODEC_HEVC : MS_CODEC_H264) : MS_CODEC_AAC;
    ti->level = MS_RECOG_AU;
    ti->selected = 1;
}

static ms_status emit_pending(ms_flv_parser *p)
{
    ms_status st = MS_OK;
    struct flv_trk *t;
    ms_sample_format sf;
    if (p->pend == FLV_PEND_NONE) {
        return MS_OK;
    }
    if (p->pend == FLV_PEND_FORMAT) {
        if (p->cb.on_format != NULL) {
            st = p->cb.on_format(p->cb.ctx, MS_FORMAT_FLV);
        }
        if (st == MS_OK) {
            p->format_emitted = 1;
            p->pend = FLV_PEND_NONE;
        }
        return st;
    }
    if (p->pend == FLV_PEND_TRACK) {
        if (p->cb.on_track != NULL) {
            st = p->cb.on_track(p->cb.ctx, &p->pend_ti, 0);
        }
        if (st == MS_OK) {
            if (p->pend_video) {
                p->video.announced = 1;
            } else {
                p->audio.announced = 1;
            }
            p->pend = FLV_PEND_NONE;
        }
        return st;
    }
    if (p->pend == FLV_PEND_CONFIG) {
        t = p->pend_video ? &p->video : &p->audio;
        sf = p->pend_video ? (p->video.hevc ? MS_SAMPLE_FMT_HVCC : MS_SAMPLE_FMT_AVCC) : MS_SAMPLE_FMT_AAC_RAW;
        if (p->cb.on_codec_config != NULL) {
            st = p->cb.on_codec_config(p->cb.ctx, &p->pend_ti, t->cfg, t->cfg_n,
                                       sf, 1, 1000);
        }
        if (st == MS_OK) {
            p->pend = FLV_PEND_NONE;
            p->tag_out_done = 1;
        }
        return st;
    }
    if (p->pend == FLV_PEND_AU) {
        if (p->cb.on_au != NULL) {
            st = p->cb.on_au(p->cb.ctx, &p->pend_ti, &p->pend_fr);
        }
        if (st == MS_OK) {
            p->pend = FLV_PEND_NONE;
            p->tag_out_done = 1;
            if (p->body_taken) {
                p->body = NULL;
                p->body_n = 0;
                p->body_cap = 0;
                p->body_taken = 0;
            }
        }
        return st;
    }
    return MS_OK;
}

static ms_status store_cfg(ms_flv_parser *p, struct flv_trk *t,
                           const uint8_t *d, size_t n)
{
    uint8_t *q;
    if (t->cfg != NULL && t->cfg_n == n && memcmp(t->cfg, d, n) == 0) {
        return MS_OK;
    }
    q = (uint8_t *)ms_mem_alloc(p->alloc, n);
    if (q == NULL) {
        return ms_acct_is_budget_limit(p->alloc) ? MS_LIMIT : MS_ERR_NOMEM;
    }
    memcpy(q, d, n);
    ms_copy_meter_add(p->meter, MS_COPY_ASM, n);
    if (t->cfg != NULL) {
        ms_mem_free(p->alloc, t->cfg);
    }
    t->cfg = q;
    t->cfg_n = n;
    t->have_cfg = 1;
    t->cfg_changed = 1;
    return MS_OK;
}

static ms_status queue_track_and_cfg(ms_flv_parser *p, int video)
{
    struct flv_trk *t = video ? &p->video : &p->audio;
    ms_status st;
    fill_track_c(&p->pend_ti, video, video ? p->video.hevc : 0);
    p->pend_video = video;
    if (!t->announced) {
        p->pend = FLV_PEND_TRACK;
        st = emit_pending(p);
        if (st != MS_OK) {
            return st;
        }
    }
    p->pend = FLV_PEND_CONFIG;
    return emit_pending(p);
}

static ms_status handle_video(ms_flv_parser *p, const uint8_t *d, size_t n)
{
    unsigned frame, codec;
    if (p->tag_out_done) {
        return MS_OK;
    }
    uint8_t ptype;
    int32_t cto;
    int64_t pts;
    ms_status st;
    const uint8_t *pay;
    size_t pn;
    int hevc;
    if (n < 1) {
        return MS_OK;
    }
    if (d[0] & 0x80u) {
#ifndef MS_SINGLE_SO
        /* 官方多 so 契约：Enhanced FLV 诚实报不支持（tests/test_core.c "enh rejected"）。 */
        p->unsup = 1;
        return MS_OK;
#else
        /* 单 so：Enhanced RTMP/FLV，低 4 位为 PacketType，后接 FourCC。by: Long <mycjzb@gmail.com> */
        unsigned pt = (unsigned)(d[0] & 0x0Fu);
        frame = (unsigned)((d[0] >> 4) & 0x07u);
        if (n < 5) {
            return MS_ERR_MALFORMED;
        }
        if (memcmp(d + 1, "hvc1", 4) == 0) {
            hevc = 1;
        } else if (memcmp(d + 1, "avc1", 4) == 0) {
            hevc = 0;
        } else {
            p->unsup = 1;
            return MS_OK;
        }
        if (pt == 0) {
            ptype = 0; cto = 0; pay = d + 5; pn = n - 5;
        } else if (pt == 1) {
            if (n < 8) {
                return MS_ERR_MALFORMED;
            }
            ptype = 1; cto = rd_si24(d + 5); pay = d + 8; pn = n - 8;
        } else if (pt == 3) {
            ptype = 1; cto = 0; pay = d + 5; pn = n - 5;
        } else if (pt == 2) {
            return MS_OK; /* SequenceEnd */
        } else {
            return MS_OK; /* Metadata / MPEG2TSSequenceStart 等：忽略 */
        }
#endif
    } else {
        frame = (unsigned)(d[0] >> 4);
        codec = (unsigned)(d[0] & 0x0Fu);
#ifdef MS_SINGLE_SO
        if (codec != 7u && codec != 12u) { /* 单 so：国内扩展 CodecID 12 = HEVC */
#else
        if (codec != 7u) {
#endif
            p->unsup = 1;
            return MS_OK;
        }
        hevc = (codec == 12u);
        if (n < 5) {
            return MS_ERR_MALFORMED;
        }
        ptype = d[1];
        cto = rd_si24(d + 2);
        pay = d + 5;
        pn = n - 5;
    }
    if (ptype == 0) {
        uint8_t ls = 0;
        if (pn == 0) {
            return MS_ERR_MALFORMED;
        }
        if (hevc) {
            if (pn < 23 || pay[0] != 1 || (pay[21] & 3u) == 2u) {
                return MS_ERR_MALFORMED;
            }
            ls = (uint8_t)((pay[21] & 3u) + 1u);
        } else {
            st = ms_avcc_parse(pay, pn, &ls);
            if (st != MS_OK) {
                return st;
            }
        }
        if (p->video.have_cfg && p->video.hevc != hevc) {
            p->video.cfg_changed = 1;
        }
        p->video.hevc = hevc;
        st = store_cfg(p, &p->video, pay, pn);
        if (st != MS_OK) {
            return st;
        }
        p->video.nal_len_size = ls;
        return queue_track_and_cfg(p, 1);
    }
    if (ptype == 2) {
        return MS_OK;
    }
    if (ptype != 1) {
        p->unsup = 1;
        return MS_OK;
    }
    if (pn == 0) {
        return MS_OK;
    }
    if (p->video.have_cfg && p->video.hevc != hevc) {
        p->unsup = 1; /* 帧与当前配置的编码不一致：不送错码流 */
        return MS_OK;
    }
    if (!p->video.have_cfg) {
        p->video.saw_au_wo_cfg = 1;
        p->unsup = 1;
        return MS_OK;
    }
    st = ms_avcc_validate_sample(pay, pn, p->video.nal_len_size);
    if (st != MS_OK) {
        return st;
    }
    pts = (int64_t)p->dts + (int64_t)cto;
    if (pts < 0) {
        return MS_ERR_UNSUPPORTED;
    }
    fill_track_c(&p->pend_ti, 1, p->video.hevc);
    memset(&p->pend_fr, 0, sizeof(p->pend_fr));
    p->pend_fr.data = pay;
    p->pend_fr.size = pn;
    p->pend_fr.has_pts = 1;
    p->pend_fr.has_dts = 1;
    p->pend_fr.pts = (uint64_t)pts;
    p->pend_fr.dts = (uint64_t)p->dts;
    p->pend_fr.is_keyframe = (frame == 1u);
    p->pend_fr.format_changed = p->video.cfg_changed;
    p->pend_fr.sample_format = p->video.hevc ? MS_SAMPLE_FMT_HVCC : MS_SAMPLE_FMT_AVCC;
    p->pend_fr.time_base_num = 1;
    p->pend_fr.time_base_den = 1000;
    p->body_taken = 0;
    p->pend_fr.owned_base = p->body;
    p->pend_fr.owned_cap = p->body_cap;
    p->pend_fr.owned_taken = &p->body_taken;
    p->video.cfg_changed = 0;
    p->pend_video = 1;
    p->pend = FLV_PEND_AU;
    return emit_pending(p);
}

static ms_status handle_audio(ms_flv_parser *p, const uint8_t *d, size_t n)
{
    unsigned fmt;
    if (p->tag_out_done) {
        return MS_OK;
    }
    uint8_t ptype;
    const uint8_t *pay;
    size_t pn;
    ms_status st;
    if (n < 1) {
        return MS_OK;
    }
    fmt = (unsigned)(d[0] >> 4);
    if (fmt != 10u) {
        p->unsup = 1;
        return MS_OK;
    }
    if (n < 2) {
        return MS_ERR_MALFORMED;
    }
    ptype = d[1];
    pay = d + 2;
    pn = n - 2;
    if (ptype == 0) {
        if (pn == 0 || !ms_asc_ok(pay, pn)) {
            return MS_ERR_UNSUPPORTED;
        }
        st = store_cfg(p, &p->audio, pay, pn);
        if (st != MS_OK) {
            return st;
        }
        return queue_track_and_cfg(p, 0);
    }
    if (ptype != 1) {
        p->unsup = 1;
        return MS_OK;
    }
    if (pn == 0) {
        return MS_OK;
    }
    if (!p->audio.have_cfg) {
        p->audio.saw_au_wo_cfg = 1;
        p->unsup = 1;
        return MS_OK;
    }
    fill_track_c(&p->pend_ti, 0, 0);
    memset(&p->pend_fr, 0, sizeof(p->pend_fr));
    p->pend_fr.data = pay;
    p->pend_fr.size = pn;
    p->pend_fr.has_pts = 1;
    p->pend_fr.has_dts = 1;
    p->pend_fr.pts = (uint64_t)p->dts;
    p->pend_fr.dts = (uint64_t)p->dts;
    p->pend_fr.is_keyframe = 1;
    p->pend_fr.format_changed = p->audio.cfg_changed;
    p->pend_fr.sample_format = MS_SAMPLE_FMT_AAC_RAW;
    p->pend_fr.time_base_num = 1;
    p->pend_fr.time_base_den = 1000;
    p->body_taken = 0;
    p->pend_fr.owned_base = p->body;
    p->pend_fr.owned_cap = p->body_cap;
    p->pend_fr.owned_taken = &p->body_taken;
    p->audio.cfg_changed = 0;
    p->pend_video = 0;
    p->pend = FLV_PEND_AU;
    return emit_pending(p);
}

static ms_status process_body(ms_flv_parser *p)
{
    if (p->tag_type == FLV_TAG_VIDEO) {
        return handle_video(p, p->body, p->body_n);
    }
    if (p->tag_type == FLV_TAG_AUDIO) {
        return handle_audio(p, p->body, p->body_n);
    }
    return MS_OK;
}

static int want_media_body(const ms_flv_parser *p)
{
    return p->tag_type == FLV_TAG_AUDIO || p->tag_type == FLV_TAG_VIDEO;
}

static ms_status ensure_body_buf(ms_flv_parser *p)
{
    if (p->body != NULL && p->body_cap >= p->data_size) {
        return MS_OK;
    }
    body_free(p);
    p->body = (uint8_t *)ms_mem_alloc(p->alloc, p->data_size);
    if (p->body == NULL) {
        return ms_acct_is_budget_limit(p->alloc) ? MS_LIMIT : MS_ERR_NOMEM;
    }
    p->body_cap = p->data_size;
    p->body_n = 0;
    return MS_OK;
}

static ms_status begin_tag_payload(ms_flv_parser *p)
{
    p->prev_expect = 11u + p->data_size;
    if (p->data_size == 0) {
        p->state = FLV_ST_PREV;
        return MS_OK;
    }
    if (!want_media_body(p)) {
        p->skip_left = p->data_size;
        p->state = FLV_ST_SKIP;
        return MS_OK;
    }
    if (p->data_size > p->max_assembly) {
        return MS_ERR_MALFORMED;
    }
    p->state = FLV_ST_BODY;
    return ensure_body_buf(p);
}

static size_t take_scratch(ms_flv_parser *p, const uint8_t *data, size_t size,
                           size_t need)
{
    size_t room = need - p->scratch_n;
    size_t n = (size < room) ? size : room;
    if (n > 0) {
        memcpy(p->scratch + p->scratch_n, data, n);
        p->scratch_n += n;
    }
    return n;
}

static ms_status retry_emit(ms_flv_parser *p)
{
    ms_status st = emit_pending(p);
    if (st != MS_OK) {
        return st;
    }
    /* TRACK 成功后还可能要发 CONFIG；AU/CONFIG 成功则 tag_out_done，不再 process_body。 */
    if (p->state == FLV_ST_EMIT) {
        if (!p->tag_out_done && p->body != NULL &&
            p->body_n == p->data_size && want_media_body(p)) {
            st = process_body(p);
            if (st != MS_OK) {
                return st;
            }
            p->tag_out_done = 1;
        }
        body_free(p);
        p->state = FLV_ST_PREV;
    }
    return MS_OK;
}

ms_status ms_flv_parser_push(ms_flv_parser *p, const uint8_t *data,
                             size_t size, size_t *consumed, int eos)
{
    size_t used = 0;
    ms_status st;
    if (consumed != NULL) {
        *consumed = 0;
    }
    if (p == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    if (p->failed) {
        return p->fail_st;
    }
    if (eos) {
        p->eos = 1;
    }
    if (data == NULL && size > 0) {
        return MS_ERR_INVALID_ARG;
    }

    st = retry_emit(p);
    if (st != MS_OK) {
        if (st != MS_LIMIT) {
            fail_set(p, st);
        }
        return st;
    }

    while (used < size) {
        size_t left = size - used;
        if (p->state == FLV_ST_HEADER) {
            size_t n = take_scratch(p, data + used, left, 9);
            used += n;
            if (p->scratch_n < 9) {
                break;
            }
            if (p->scratch[0] != (uint8_t)'F' ||
                p->scratch[1] != (uint8_t)'L' ||
                p->scratch[2] != (uint8_t)'V') {
                fail_set(p, MS_ERR_MALFORMED);
                return MS_ERR_MALFORMED;
            }
            if (p->scratch[3] != 1 ||
                (p->scratch[4] & (uint8_t)~0x05u) != 0) {
                p->pend = FLV_PEND_FORMAT;
                st = emit_pending(p);
                if (st == MS_LIMIT) {
                    if (consumed != NULL) {
                        *consumed = used;
                    }
                    fail_set(p, MS_ERR_UNSUPPORTED);
                    return (st == MS_LIMIT) ? MS_LIMIT : MS_ERR_UNSUPPORTED;
                }
                fail_set(p, MS_ERR_UNSUPPORTED);
                return MS_ERR_UNSUPPORTED;
            }
            p->data_offset = rd_be32(p->scratch + 5);
            if (p->data_offset < 9 || p->data_offset > FLV_MAX_HDR_PAD) {
                fail_set(p, MS_ERR_MALFORMED);
                return MS_ERR_MALFORMED;
            }
            if (!p->format_emitted) {
                p->pend = FLV_PEND_FORMAT;
                st = emit_pending(p);
                if (st != MS_OK) {
                    if (st != MS_LIMIT) {
                        fail_set(p, st);
                    }
                    if (consumed != NULL) {
                        *consumed = used;
                    }
                    return st;
                }
            }
            p->scratch_n = 0;
            p->pad_left = p->data_offset - 9u;
            p->prev_expect = 0;
            p->state = (p->pad_left > 0) ? FLV_ST_HPAD : FLV_ST_PREV;
            continue;
        }
        if (p->state == FLV_ST_HPAD) {
            size_t n = (left < (size_t)p->pad_left) ? left : (size_t)p->pad_left;
            used += n;
            p->pad_left -= (uint32_t)n;
            if (p->pad_left > 0) {
                break;
            }
            p->state = FLV_ST_PREV;
            continue;
        }
        if (p->state == FLV_ST_PREV) {
            size_t n = take_scratch(p, data + used, left, 4);
            used += n;
            if (p->scratch_n < 4) {
                break;
            }
            if (rd_be32(p->scratch) != p->prev_expect) {
                fail_set(p, MS_ERR_MALFORMED);
                return MS_ERR_MALFORMED;
            }
            p->scratch_n = 0;
            p->state = FLV_ST_TAGHDR;
            continue;
        }
        if (p->state == FLV_ST_TAGHDR) {
            size_t n = take_scratch(p, data + used, left, 11);
            used += n;
            if (p->scratch_n < 11) {
                break;
            }
            p->tag_type = p->scratch[0];
            p->data_size = rd_be24(p->scratch + 1);
            p->dts = ((uint32_t)p->scratch[7] << 24) | rd_be24(p->scratch + 4);
            p->stream_id = rd_be24(p->scratch + 8);
            p->scratch_n = 0;
            p->tag_out_done = 0;
            if (p->stream_id != 0) {
                fail_set(p, MS_ERR_MALFORMED);
                return MS_ERR_MALFORMED;
            }
            if (p->tag_type != FLV_TAG_AUDIO && p->tag_type != FLV_TAG_VIDEO &&
                p->tag_type != FLV_TAG_SCRIPT) {
                p->unsup = 1;
            }
            st = begin_tag_payload(p);
            if (st == MS_LIMIT) {
                /* 状态已是 BODY，下次先重试分配。 */
                if (consumed != NULL) {
                    *consumed = used;
                }
                return MS_LIMIT;
            }
            if (st != MS_OK) {
                fail_set(p, st);
                return st;
            }
            continue;
        }
        if (p->state == FLV_ST_SKIP) {
            size_t n = (left < (size_t)p->skip_left) ? left : (size_t)p->skip_left;
            used += n;
            p->skip_left -= (uint32_t)n;
            if (p->skip_left > 0) {
                break;
            }
            p->state = FLV_ST_PREV;
            continue;
        }
        if (p->state == FLV_ST_BODY) {
            if (p->body == NULL) {
                st = ensure_body_buf(p);
                if (st != MS_OK) {
                    if (st != MS_LIMIT) {
                        fail_set(p, st);
                    }
                    if (consumed != NULL) {
                        *consumed = used;
                    }
                    return st;
                }
            }
            {
                size_t need = p->data_size - p->body_n;
                size_t n = (left < need) ? left : need;
                if (n > 0) {
                    memcpy(p->body + p->body_n, data + used, n);
                    ms_copy_meter_add(p->meter, MS_COPY_ASM, n);
                    p->body_n += n;
                    used += n;
                }
            }
            if (p->body_n < p->data_size) {
                break;
            }
            st = process_body(p);
            if (st == MS_LIMIT) {
                p->state = FLV_ST_EMIT;
                if (consumed != NULL) {
                    *consumed = used;
                }
                return MS_LIMIT;
            }
            if (st != MS_OK) {
                fail_set(p, st);
                return st;
            }
            body_free(p);
            p->state = FLV_ST_PREV;
            continue;
        }
        break;
    }

    if (consumed != NULL) {
        *consumed = used;
    }
    if (p->eos) {
        /* 半截 tag / prevTagSize：不把残片当 AU，也不因此判畸形。 */
        if ((p->video.saw_au_wo_cfg && !p->video.have_cfg) ||
            (p->audio.saw_au_wo_cfg && !p->audio.have_cfg)) {
            p->unsup = 1;
        }
    }
    return MS_OK;
}

ms_status ms_flv_parser_drain(ms_flv_parser *p)
{
    ms_status st;
    if (p == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    if (p->failed) {
        return p->fail_st;
    }
    /* 只重试已完整 tag 的待入队事件，不置 EOS，以免中途反压被当成结束。 */
    st = retry_emit(p);
    if (st != MS_OK && st != MS_LIMIT && st != MS_AGAIN) {
        fail_set(p, st);
        return st;
    }
    if (p->eos) {
        if ((p->video.saw_au_wo_cfg && !p->video.have_cfg) ||
            (p->audio.saw_au_wo_cfg && !p->audio.have_cfg)) {
            p->unsup = 1;
        }
    }
    return st;
}
