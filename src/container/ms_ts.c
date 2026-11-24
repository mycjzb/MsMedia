/*
 * MPEG-TS：同步搜索、188/192 包长、PAT/PMT、可选 PES 组装。
 * by: Long <mycjzb@gmail.com>
 */

#include "ms_ts.h"
#include "ms_ts_scan.h"
#include "ms_crc32.h"

static void psi_reset(ms_psi_acc *s)
{
    s->len = 0;
    s->need = 0;
    s->active = 0;
}

static void pes_acc_free(ms_ts_parser *p, ms_pes_acc *a)
{
    if (a->buf != NULL) {
        ms_mem_free(p->alloc, a->buf);
    }
    memset(a, 0, sizeof(*a));
}

static void pes_done_free(ms_ts_parser *p, ms_pes_done *d)
{
    if (d->payload != NULL) {
        ms_mem_free(p->alloc, d->payload);
    }
    memset(d, 0, sizeof(*d));
}


static void pending_free(ms_ts_parser *p, ms_es_pending *pend)
{
    if (pend->data != NULL) {
        ms_mem_free(p->alloc, pend->data);
    }
    memset(pend, 0, sizeof(*pend));
}

/* AVS：丢弃未完成 AU（不释放缓冲）。 */
static void avs_reset_au(ms_ts_track *t)
{
    t->avs_len = 0;
    t->avs_has_pic = 0;
    t->avs_key = 0;
    t->avs_has_seq = 0;
    t->avs_has_pts = 0;
    t->avs_has_dts = 0;
    t->avs_pts = 0;
    t->avs_dts = 0;
}

static void track_release(ms_ts_parser *p, ms_ts_track *t)
{
    if (t->adts != NULL) {
        ms_adts_destroy(t->adts);
        ms_mem_free(p->alloc, t->adts);
        t->adts = NULL;
    }
    t->adts_inited = 0;
    if (t->h264 != NULL) {
        ms_h264_destroy(t->h264);
        ms_mem_free(p->alloc, t->h264);
        t->h264 = NULL;
    }
    t->h264_inited = 0;
    pending_free(p, &t->pending);
    if (t->avs_au != NULL) {
        ms_mem_free(p->alloc, t->avs_au);
    }
    t->avs_au = NULL;
    t->avs_cap = 0;
    avs_reset_au(t);
    t->avs_cfg_sig = 0;
}

static void fill_track_info(const ms_ts_track *t, ms_track_info *info)
{
    memset(info, 0, sizeof(*info));
    info->id = t->pid;
    info->kind = t->kind;
    info->codec = t->codec;
    info->level = t->level;
    info->stream_type = t->stream_type;
    info->language = (t->language[0] != 0) ? t->language : NULL;
    info->name = NULL;
    info->is_default = t->selected;
    info->is_forced = 0;
    info->selected = t->selected;
}

static ms_status adts_on_config(void *ctx, const ms_adts_frame *fr)
{
    ms_ts_track *t = (ms_ts_track *)ctx;
    ms_ts_parser *p = (ms_ts_parser *)t->parser;
    ms_track_info info;
    ms_status st;

    if (p == NULL || fr == NULL) {
        return MS_ERR_STATE;
    }
    if (!t->level_upgraded) {
        t->level = MS_RECOG_AU;
        if (p->cb.on_track != NULL) {
            fill_track_info(t, &info);
            st = p->cb.on_track(p->cb.ctx, &info, 1);
            if (st != MS_OK) {
                t->level = MS_RECOG_CONTAINER_ID;
                return st;
            }
        }
        t->level_upgraded = 1;
    }
    if (p->cb.on_codec_config == NULL) {
        return MS_OK;
    }
    fill_track_info(t, &info);
    info.level = MS_RECOG_AU;
    return p->cb.on_codec_config(p->cb.ctx, &info, fr->asc, fr->asc_size);
}

static ms_status adts_on_au(void *ctx, const ms_adts_frame *fr)
{
    ms_ts_track *t = (ms_ts_track *)ctx;
    ms_ts_parser *p = (ms_ts_parser *)t->parser;
    ms_track_info info;
    ms_au_frame au;

    if (p == NULL || fr == NULL) {
        return MS_ERR_STATE;
    }
    if (p->cb.on_au == NULL) {
        return MS_OK;
    }
    fill_track_info(t, &info);
    info.level = MS_RECOG_AU;
    memset(&au, 0, sizeof(au));
    au.data = fr->data;
    au.size = fr->size;
    au.has_pts = fr->has_pts;
    au.has_dts = fr->has_dts;
    au.pts = fr->pts;
    au.dts = fr->dts;
    au.format_changed = fr->format_changed;
    au.is_keyframe = 1;
    au.owned_base = fr->owned_base;
    au.owned_cap = fr->owned_cap;
    au.owned_taken = fr->owned_taken;
    au.discontinuity = t->pending_disc ? 1 : 0;
    t->pending_disc = 0;
    t->disc_event_open = 0;
    return p->cb.on_au(p->cb.ctx, &info, &au);
}

static ms_status h264_on_config(void *ctx, const uint8_t *data, size_t size)
{
    ms_ts_track *t = (ms_ts_track *)ctx;
    ms_ts_parser *p = (ms_ts_parser *)t->parser;
    ms_track_info info;
    ms_status st;

    if (p == NULL) {
        return MS_ERR_STATE;
    }
    if (!t->level_upgraded) {
        t->level = MS_RECOG_AU;
        if (p->cb.on_track != NULL) {
            fill_track_info(t, &info);
            st = p->cb.on_track(p->cb.ctx, &info, 1);
            if (st != MS_OK) {
                t->level = MS_RECOG_CONTAINER_ID;
                return st;
            }
        }
        t->level_upgraded = 1;
    }
    if (p->cb.on_codec_config == NULL) {
        return MS_OK;
    }
    fill_track_info(t, &info);
    info.level = MS_RECOG_AU;
    return p->cb.on_codec_config(p->cb.ctx, &info, data, size);
}

static ms_status h264_on_au(void *ctx, const ms_au_frame *fr)
{
    ms_ts_track *t = (ms_ts_track *)ctx;
    ms_ts_parser *p = (ms_ts_parser *)t->parser;
    ms_track_info info;

    if (p == NULL || fr == NULL) {
        return MS_ERR_STATE;
    }
    if (p->cb.on_au == NULL) {
        return MS_OK;
    }
    {
        ms_au_frame au = *fr;
        au.discontinuity = t->pending_disc ? 1 : 0;
        t->pending_disc = 0;
        t->disc_event_open = 0;
        fill_track_info(t, &info);
        info.level = MS_RECOG_AU;
        return p->cb.on_au(p->cb.ctx, &info, &au);
    }
}

static ms_status track_init_adts(ms_ts_parser *p, ms_ts_track *t)
{
    ms_adts_callbacks cb;
    ms_adts *obj;

    if (t->codec != MS_CODEC_AAC) {
        return MS_OK;
    }
    if (t->adts_inited && t->adts != NULL) {
        return MS_OK;
    }
    obj = (ms_adts *)ms_mem_alloc(p->alloc, sizeof(*obj));
    if (obj == NULL) {
        return ms_acct_is_budget_limit(p->alloc) ? MS_LIMIT : MS_ERR_NOMEM;
    }
    memset(&cb, 0, sizeof(cb));
    cb.ctx = t;
    cb.on_config = adts_on_config;
    cb.on_au = adts_on_au;
    t->parser = p;
    ms_adts_init(obj, p->alloc, p->max_pes_bytes, &cb);
    ms_adts_set_meter(obj, p->meter);
    t->adts = obj;
    t->adts_inited = 1;
    return MS_OK;
}

static ms_status track_init_h264(ms_ts_parser *p, ms_ts_track *t)
{
    ms_h264_callbacks cb;
    ms_h264 *obj;

    if (t->codec != MS_CODEC_H264) {
        return MS_OK;
    }
    if (t->h264_inited && t->h264 != NULL) {
        return MS_OK;
    }
    obj = (ms_h264 *)ms_mem_alloc(p->alloc, sizeof(*obj));
    if (obj == NULL) {
        return ms_acct_is_budget_limit(p->alloc) ? MS_LIMIT : MS_ERR_NOMEM;
    }
    memset(&cb, 0, sizeof(cb));
    cb.ctx = t;
    cb.on_config = h264_on_config;
    cb.on_au = h264_on_au;
    t->parser = p;
    ms_h264_init(obj, p->alloc, p->max_pes_bytes, &cb);
    ms_h264_set_meter(obj, p->meter);
    t->h264 = obj;
    t->h264_inited = 1;
    return MS_OK;
}

static ms_status pending_store(ms_ts_parser *p, ms_es_pending *pend,
                               const uint8_t *data, size_t size,
                               int begun, int has_pts, uint64_t pts,
                               int has_dts, uint64_t dts)
{
    uint8_t *nbuf;
    size_t cap = 0;

    if (size == 0) {
        pending_free(p, pend);
        return MS_OK;
    }
    if (size > p->max_pes_bytes) {
        return MS_ERR_MALFORMED;
    }
    /* 先分配再释放旧块，失败时旧 pending 仍可用。 */
    nbuf = (uint8_t *)ms_mem_alloc(p->alloc, size);
    if (nbuf == NULL) {
        if (ms_acct_is_budget_limit(p->alloc)) {
            return MS_LIMIT;
        }
        return MS_ERR_NOMEM;
    }
    memcpy(nbuf, data, size);
    ms_copy_meter_add(p->meter, MS_COPY_ASM, size);
    pending_free(p, pend);
    (void)cap;
    pend->data = nbuf;
    pend->len = size;
    pend->off = 0;
    pend->begun = begun;
    pend->has_pts = has_pts;
    pend->pts = pts;
    pend->has_dts = has_dts;
    pend->dts = dts;
    return MS_OK;
}

/* 容错：编码组帧报坏数据时恢复组帧器，丢弃该段，下一 AU 标不连续。 */
static ms_status tol_codec(ms_ts_parser *p, ms_ts_track *t, ms_status st)
{
    if (!p->tolerant || (st != MS_ERR_MALFORMED && st != MS_ERR_UNSUPPORTED && st != MS_ERR_OVERFLOW)) {
        return st;
    }
    if (t->h264 != NULL && t->h264->failed) {
        ms_h264_recover(t->h264);
    } else if (t->adts != NULL && t->adts->failed) {
        ms_adts_recover(t->adts);
    } else {
        return st;
    }
    pending_free(p, &t->pending);
    t->pending_disc = 1;
    p->tolerated++;
    return MS_OK;
}

static ms_status feed_aac_track(ms_ts_parser *p, ms_ts_track *t,
                                const uint8_t *data, size_t size,
                                int has_group, int has_pts, uint64_t pts,
                                int has_dts, uint64_t dts)
{
    ms_status st;
    size_t used = 0;

    if (!t->adts_inited) {
        ms_status ist = track_init_adts(p, t);
        if (ist != MS_OK) {
            return ist;
        }
    }
    if (!t->adts_inited || t->adts == NULL) {
        return MS_OK;
    }

    /* 先排空上次因反压留下的载荷。 */
    if (t->pending.data != NULL && t->pending.off < t->pending.len) {
        if (!t->pending.begun) {
            ms_adts_begin_pes(t->adts, t->pending.has_pts, t->pending.pts,
                              t->pending.has_dts, t->pending.dts);
            t->pending.begun = 1;
        }
        used = 0;
        st = ms_adts_push(t->adts, t->pending.data + t->pending.off,
                          t->pending.len - t->pending.off, &used);
        t->pending.off += used;
        if (t->pending.off >= t->pending.len) {
            pending_free(p, &t->pending);
        }
        if (st != MS_OK) {
            st = tol_codec(p, t, st);
        }
        if (st != MS_OK) {
            return st;
        }
    } else if (t->pending.data != NULL) {
        pending_free(p, &t->pending);
    }

    if (has_group) {
        ms_adts_begin_pes(t->adts, has_pts, pts, has_dts, dts);
    }
    if (size == 0) {
        return ms_adts_drain(t->adts);
    }
    used = 0;
    st = ms_adts_push(t->adts, data, size, &used);
    if (st != MS_OK && st != MS_LIMIT && tol_codec(p, t, st) == MS_OK) {
        return MS_OK; /* 容错：本段丢弃 */
    }
    if (used < size) {
        ms_status pst = pending_store(p, &t->pending, data + used, size - used,
                                      1, 0, 0, 0, 0);
        if (pst != MS_OK) {
            return pst;
        }
    }
    return st;
}

static ms_status feed_h264_track(ms_ts_parser *p, ms_ts_track *t,
                                 const uint8_t *data, size_t size,
                                 int has_group, int has_pts, uint64_t pts,
                                 int has_dts, uint64_t dts)
{
    ms_status st;
    size_t used = 0;

    if (!t->h264_inited) {
        ms_status ist = track_init_h264(p, t);
        if (ist != MS_OK) {
            return ist;
        }
    }
    if (!t->h264_inited || t->h264 == NULL) {
        return MS_OK;
    }

    if (t->pending.data != NULL && t->pending.off < t->pending.len) {
        if (!t->pending.begun) {
            ms_h264_begin_pes(t->h264, t->pending.has_pts, t->pending.pts,
                              t->pending.has_dts, t->pending.dts);
            t->pending.begun = 1;
        }
        used = 0;
        st = ms_h264_push(t->h264, t->pending.data + t->pending.off,
                          t->pending.len - t->pending.off, &used);
        t->pending.off += used;
        if (t->pending.off >= t->pending.len) {
            pending_free(p, &t->pending);
        }
        if (st != MS_OK) {
            st = tol_codec(p, t, st);
        }
        if (st != MS_OK) {
            return st;
        }
    } else if (t->pending.data != NULL) {
        pending_free(p, &t->pending);
    }

    if (has_group) {
        ms_h264_begin_pes(t->h264, has_pts, pts, has_dts, dts);
    }
    if (size == 0) {
        return ms_h264_drain(t->h264);
    }
    used = 0;
    st = ms_h264_push(t->h264, data, size, &used);
    if (st != MS_OK && st != MS_LIMIT && tol_codec(p, t, st) == MS_OK) {
        return MS_OK; /* 容错：本段丢弃 */
    }
    if (used < size) {
        ms_status pst = pending_store(p, &t->pending, data + used, size - used,
                                      1, 0, 0, 0, 0);
        if (pst != MS_OK) {
            return pst;
        }
    }
    return st;
}

static ms_status drain_all_adts(ms_ts_parser *p)
{
    int i;
    ms_status st;
    for (i = 0; i < MS_TS_MAX_TRACKS; i++) {
        if (!p->tracks[i].in_use || (!p->tracks[i].adts_inited || p->tracks[i].adts == NULL)) {
            continue;
        }
        st = feed_aac_track(p, &p->tracks[i], NULL, 0, 0, 0, 0, 0, 0);
        if (st != MS_OK) {
            return st;
        }
    }
    for (i = 0; i < MS_TS_MAX_TRACKS; i++) {
        if (!p->tracks[i].in_use || (!p->tracks[i].h264_inited || p->tracks[i].h264 == NULL)) {
            continue;
        }
        st = feed_h264_track(p, &p->tracks[i], NULL, 0, 0, 0, 0, 0, 0);
        if (st != MS_OK) {
            return st;
        }
    }
    return MS_OK;
}

static ms_status close_open_pes(ms_ts_parser *p);

static ms_status end_all_adts(ms_ts_parser *p)
{
    int i;
    ms_status st;
    st = close_open_pes(p);
    if (st != MS_OK) {
        return st;
    }
    st = drain_all_adts(p);
    if (st != MS_OK) {
        return st;
    }
    for (i = 0; i < MS_TS_MAX_TRACKS; i++) {
        if (!p->tracks[i].in_use || (!p->tracks[i].adts_inited || p->tracks[i].adts == NULL)) {
            continue;
        }
        st = ms_adts_end(p->tracks[i].adts);
        if (st != MS_OK) {
            return st;
        }
        pending_free(p, &p->tracks[i].pending);
    }
    for (i = 0; i < MS_TS_MAX_TRACKS; i++) {
        if (!p->tracks[i].in_use || (!p->tracks[i].h264_inited || p->tracks[i].h264 == NULL)) {
            continue;
        }
        st = ms_h264_end(p->tracks[i].h264);
        if (st != MS_OK) {
            return st;
        }
        pending_free(p, &p->tracks[i].pending);
    }
    return MS_OK;
}


static void pre_pmt_free(ms_ts_parser *p)
{
    if (p == NULL) {
        return;
    }
    ms_mem_free(p->alloc, p->pre_pmt);
    p->pre_pmt = NULL;
    p->pre_pmt_cap = 0;
    p->pre_pmt_count = 0;
    p->pre_pmt_sent = 0;
}

static void clear_tracks_and_pes(ms_ts_parser *p)
{
    int i;
    for (i = 0; i < MS_TS_MAX_TRACKS; i++) {
        pes_acc_free(p, &p->pes[i]);
        track_release(p, &p->tracks[i]);
        memset(&p->tracks[i], 0, sizeof(p->tracks[i]));
    }
    for (i = 0; i < MS_PES_DONE_MAX; i++) {
        pes_done_free(p, &p->done[i]);
    }
    pre_pmt_free(p);
    p->ntracks = 0;
    p->selected_video = 0;
    p->selected_audio = 0;
}

void ms_ts_parser_init(ms_ts_parser *p,
                       const ms_allocator *alloc,
                       size_t probe_bytes,
                       size_t max_resync_bytes,
                       size_t max_pes_bytes,
                       const ms_ts_callbacks *cb)
{
    memset(p, 0, sizeof(*p));
    p->alloc = alloc;
    p->probe_bytes = probe_bytes;
    p->max_resync_bytes = max_resync_bytes;
    p->max_pes_bytes = (max_pes_bytes == 0) ? MS_DEFAULT_AU_QUEUE_BYTES : max_pes_bytes;
    if (cb != NULL) {
        p->cb = *cb;
    }
}

void ms_ts_parser_set_meter(ms_ts_parser *p, ms_copy_meter *m)
{
    if (p != NULL) {
        p->meter = m;
    }
}

void ms_ts_parser_destroy(ms_ts_parser *p)
{
    if (p == NULL) {
        return;
    }
    clear_tracks_and_pes(p);
    memset(p, 0, sizeof(*p));
}

void ms_ts_parser_reset(ms_ts_parser *p)
{
    const ms_allocator *alloc;
    size_t probe, resync, pes;
    ms_ts_callbacks cb;

    if (p == NULL) {
        return;
    }
    alloc = p->alloc;
    probe = p->probe_bytes;
    resync = p->max_resync_bytes;
    pes = p->max_pes_bytes;
    cb = p->cb;
    {
        ms_copy_meter *meter = p->meter;
        clear_tracks_and_pes(p);
        memset(p, 0, sizeof(*p));
        p->alloc = alloc;
        p->probe_bytes = probe;
        p->max_resync_bytes = resync;
        p->max_pes_bytes = pes;
        p->cb = cb;
        p->meter = meter;
    }
}

void ms_ts_parser_flush_partial(ms_ts_parser *p)
{
    int i;

    if (p == NULL) {
        return;
    }
    psi_reset(&p->pat);
    psi_reset(&p->pmt);
    for (i = 0; i < MS_TS_MAX_TRACKS; i++) {
        if (p->pes[i].buf != NULL || p->pes[i].in_use) {
            pes_acc_free(p, &p->pes[i]);
        }
    }
    for (i = 0; i < MS_PES_DONE_MAX; i++) {
        pes_done_free(p, &p->done[i]);
    }
    for (i = 0; i < MS_TS_MAX_TRACKS; i++) {
        if (p->tracks[i].adts_inited && p->tracks[i].adts != NULL) {
            ms_adts_flush_partial(p->tracks[i].adts);
        }
        if (p->tracks[i].h264_inited && p->tracks[i].h264 != NULL) {
            ms_h264_flush_partial(p->tracks[i].h264);
        }
        pending_free(p, &p->tracks[i].pending);
        /* AVS：flush/seek 后丢半帧并清序列头签名，下一序列头重新上送 CODEC_CONFIG。 */
        avs_reset_au(&p->tracks[i]);
        p->tracks[i].avs_cfg_sig = 0;
    }
    p->resync_spent = 0;
    if (!p->pre_pmt_replayed) {
        pre_pmt_free(p);
    }
}

static void fail_parser(ms_ts_parser *p, ms_status st)
{
    p->failed = 1;
    p->fail_status = st;
}

static const uint8_t *packet_ts(const uint8_t *pkt, uint16_t psz)
{
    (void)psz;
    /* 调用方保证 pkt 指向 0x47，188 字节负载从这里开始。 */
    return pkt;
}

static int payload_of(const uint8_t *ts, const uint8_t **payload, size_t *plen)
{
    unsigned adapt = (ts[3] >> 4) & 0x3u;
    size_t off = 4;

    if (adapt == 0) {
        return -1; /* 保留值，畸形 */
    }
    if (adapt == 2 || adapt == 3) {
        unsigned alen;
        if (off >= 188) {
            return -1;
        }
        alen = ts[off];
        off += 1;
        if (off + alen > 188) {
            return -1;
        }
        off += alen;
    }
    if (adapt == 2) {
        *payload = ts + off;
        *plen = 0;
        return 0;
    }
    *payload = ts + off;
    *plen = 188 - off;
    return 0;
}

static ms_status emit_format(ms_ts_parser *p)
{
    ms_status st;
    if (p->format_emitted) {
        return MS_OK;
    }
    if (p->cb.on_format == NULL) {
        p->format_emitted = 1;
        return MS_OK;
    }
    st = p->cb.on_format(p->cb.ctx, p->packet_size);
    if (st == MS_OK) {
        p->format_emitted = 1;
    }
    return st;
}

static ms_ts_track *track_by_pid(ms_ts_parser *p, uint16_t pid);

static uint64_t payload_tag(const uint8_t *p, size_t n)
{
    uint64_t h = 14695981039346656037ULL;
    size_t i;
    h ^= (uint64_t)n;
    h *= 1099511628211ULL;
    for (i = 0; i < n; i++) {
        h ^= p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

static int adapt_disc_indicator(const uint8_t *ts)
{
    unsigned adapt = (ts[3] >> 4) & 0x3u;
    unsigned alen;
    if (adapt != 2 && adapt != 3) {
        return 0;
    }
    alen = ts[4];
    if (alen == 0) {
        return 0;
    }
    return (ts[5] & 0x80) != 0;
}

static void remember_payload(ms_pes_acc *acc, const uint8_t *payload, size_t plen)
{
    acc->last_plen = (uint8_t)plen;
    acc->last_tag = payload_tag(payload, plen);
}

static void remember_psi_payload(ms_psi_acc *acc, const uint8_t *payload, size_t plen)
{
    acc->last_plen = (uint8_t)plen;
    acc->last_tag = payload_tag(payload, plen);
}

static ms_status emit_disc(ms_ts_parser *p, const ms_ts_track *t)
{
    ms_track_info info;
    const ms_track_info *pi = NULL;
    if (p->cb.on_discontinuity == NULL) {
        return MS_OK;
    }
    if (t != NULL) {
        fill_track_info(t, &info);
        pi = &info;
    }
    return p->cb.on_discontinuity(p->cb.ctx, pi);
}

static void drop_incomplete_pes(ms_pes_acc *acc)
{
    if (acc == NULL) {
        return;
    }
    acc->len = 0;
    acc->need = 0;
}

static void drop_incomplete_es(ms_ts_parser *p, ms_ts_track *t)
{
    if (t == NULL) {
        return;
    }
    pending_free(p, &t->pending);
    if (t->adts_inited && t->adts != NULL) {
        ms_adts_flush_partial(t->adts);
    }
    if (t->h264_inited && t->h264 != NULL) {
        ms_h264_flush_partial(t->h264);
    }
    avs_reset_au(t);
    t->avs_cfg_sig = 0;
    t->pending_disc = 1;
}

static ms_pes_acc *pes_find(ms_ts_parser *p, uint16_t pid)
{
    int i;
    for (i = 0; i < MS_TS_MAX_TRACKS; i++) {
        if (p->pes[i].in_use && p->pes[i].pid == pid) {
            return &p->pes[i];
        }
    }
    return NULL;
}

/* 丢弃该 PID 未完成 PES/ES，发出一次不连续事件。其它轨不动。 */
static ms_status notify_pid_gap(ms_ts_parser *p, uint16_t pid)
{
    ms_pes_acc *acc = pes_find(p, pid);
    ms_ts_track *t = track_by_pid(p, pid);
    if (acc != NULL) {
        drop_incomplete_pes(acc);
    }
    drop_incomplete_es(p, t);
    if (t != NULL) {
        if (t->disc_event_open) {
            return MS_OK;
        }
        t->disc_event_open = 1;
        return emit_disc(p, t);
    }
    return emit_disc(p, NULL);
}

/* 字节层失同步：所有未完成 PES/ES 都必须丢掉，配置保留。 */
static ms_status notify_stream_resync(ms_ts_parser *p)
{
    int i;
    for (i = 0; i < MS_TS_MAX_TRACKS; i++) {
        if (p->pes[i].in_use) {
            drop_incomplete_pes(&p->pes[i]);
        }
        if (p->tracks[i].in_use) {
            drop_incomplete_es(p, &p->tracks[i]);
            p->tracks[i].disc_event_open = 1;
        }
    }
    return emit_disc(p, NULL);
}

/*
 * CC 判定（ITU-T H.222.0 / ISO/IEC 13818-1 2.4.3.3–2.4.3.4）：
 *  - 有载荷（AFC 01/11）时 CC 应相对上一有载荷包 +1（含 15→0 回绕）
 *  - 纯适配（AFC 10）不递增，CC 与上一有载荷包相同
 *  - 合法重复：连续第二个同 CC 且载荷相同的有载荷包，丢弃且不视为间隙
 *  - 第三次同 CC 或同 CC 载荷不同：异常
 *  - discontinuity_indicator 允许 CC 跳跃，仍须丢弃未完成 PES/ES
 * skip=1 表示本包载荷不得追加；gap=1 表示已通知间隙且未完成单元已丢弃。
 */
static ms_status evaluate_media_cc(ms_ts_parser *p, ms_pes_acc *acc, uint16_t pid,
                                  uint8_t cc, int has_payload, int disc_ind,
                                  int pusi, const uint8_t *payload, size_t plen,
                                  int *skip, int *gap)
{
    uint8_t expected;
    *skip = 0;
    *gap = 0;
    if (!acc->have_cc) {
        acc->have_cc = 1;
        acc->last_cc = cc;
        acc->dup_seen = 0;
        if (has_payload) {
            remember_payload(acc, payload, plen);
        }
        if (disc_ind) {
            *gap = 1;
            return notify_pid_gap(p, pid);
        }
        return MS_OK;
    }
    expected = (uint8_t)((acc->last_cc + 1) & 0x0F);
    if (has_payload && cc == acc->last_cc) {
        if (!acc->dup_seen && (size_t)acc->last_plen == plen &&
            acc->last_tag == payload_tag(payload, plen)) {
            acc->dup_seen = 1;
            *skip = 1;
            return MS_OK;
        }
        /* 同 CC 第三包，或同 CC 但载荷不同。 */
        acc->dup_seen = 0;
        acc->last_cc = cc;
        remember_payload(acc, payload, plen);
        *gap = 1;
        *skip = pusi ? 0 : 1;
        return notify_pid_gap(p, pid);
    }
    if (!has_payload && cc == acc->last_cc) {
        *skip = 1;
        if (disc_ind) {
            *gap = 1;
            return notify_pid_gap(p, pid);
        }
        return MS_OK;
    }
    if (has_payload && cc == expected) {
        acc->last_cc = cc;
        acc->dup_seen = 0;
        remember_payload(acc, payload, plen);
        if (disc_ind) {
            *gap = 1;
            return notify_pid_gap(p, pid);
        }
        return MS_OK;
    }
    if (!has_payload) {
        acc->last_cc = cc;
        acc->dup_seen = 0;
        *skip = 1;
        if (disc_ind) {
            *gap = 1;
            return notify_pid_gap(p, pid);
        }
        return MS_OK;
    }
    /* CC 跳跃。无 disc_ind 且无 PUSI：本包是缺片后的续传，丢弃。 */
    acc->last_cc = cc;
    acc->dup_seen = 0;
    remember_payload(acc, payload, plen);
    *gap = 1;
    *skip = (!disc_ind && !pusi) ? 1 : 0;
    return notify_pid_gap(p, pid);
}


static ms_ts_track *track_by_pid(ms_ts_parser *p, uint16_t pid)
{
    int i;
    for (i = 0; i < MS_TS_MAX_TRACKS; i++) {
        if (p->tracks[i].in_use && p->tracks[i].pid == pid) {
            return &p->tracks[i];
        }
    }
    return NULL;
}

static ms_pes_acc *pes_by_pid(ms_ts_parser *p, uint16_t pid)
{
    int i;
    for (i = 0; i < MS_TS_MAX_TRACKS; i++) {
        if (p->pes[i].in_use && p->pes[i].pid == pid) {
            return &p->pes[i];
        }
        if (!p->pes[i].in_use && p->pes[i].buf == NULL && p->pes[i].pid == 0) {
            /* 空槽，下面分配时再用 */
        }
    }
    for (i = 0; i < MS_TS_MAX_TRACKS; i++) {
        if (!p->pes[i].in_use) {
            p->pes[i].in_use = 1;
            p->pes[i].pid = pid;
            return &p->pes[i];
        }
    }
    return NULL;
}


static int desc_is_bitmap_sub(const uint8_t *d, size_t dlen)
{
    size_t i = 0;
    while (i + 2 <= dlen) {
        unsigned tag = d[i];
        unsigned ln = d[i + 1];
        i += 2;
        if (i + ln > dlen) {
            break;
        }
        /* 0x59 subtitling_descriptor；0x56 teletext_descriptor：位图/图文，不解析 */
        if (tag == 0x59 || tag == 0x56) {
            return 1;
        }
        i += ln;
    }
    return 0;
}

static int desc_has_tag(const uint8_t *d, size_t dlen, unsigned want)
{
    size_t i = 0;
    while (i + 2 <= dlen) {
        unsigned tag = d[i];
        unsigned ln = d[i + 1];
        i += 2;
        if (i + ln > dlen) {
            break;
        }
        if (tag == want) {
            return 1;
        }
        i += ln;
    }
    return 0;
}

static void lang_from_descriptors(const uint8_t *d, size_t dlen, char lang[4])
{
    size_t i = 0;
    lang[0] = 0;
    while (i + 2 <= dlen) {
        unsigned tag = d[i];
        unsigned ln = d[i + 1];
        i += 2;
        if (i + ln > dlen) {
            break;
        }
        /* ISO_639_language_descriptor */
        if (tag == 0x0A && ln >= 3) {
            lang[0] = (char)d[i];
            lang[1] = (char)d[i + 1];
            lang[2] = (char)d[i + 2];
            lang[3] = 0;
            return;
        }
        i += ln;
    }
}

static ms_status finish_pat(ms_ts_parser *p, const uint8_t *sec, size_t slen)
{
    uint16_t section_length;
    uint8_t version;
    uint8_t current_next;
    uint8_t section_number;
    uint8_t last_section;
    size_t off;
    size_t end;
    uint32_t crc;
    uint32_t got;
    uint16_t chosen_prog = 0;
    uint16_t chosen_pid = 0;
    int found = 0;

    if (slen < 8) {
        return MS_ERR_MALFORMED;
    }
    if (sec[0] != 0x00) {
        return MS_OK; /* 不是 PAT，忽略 */
    }
    if ((sec[1] & 0x80) == 0) {
        return MS_ERR_MALFORMED;
    }
    section_length = (uint16_t)(((sec[1] & 0x0F) << 8) | sec[2]);
    if ((size_t)3 + section_length != slen) {
        return MS_ERR_MALFORMED;
    }
    if (section_length < 9 || section_length > 1021) {
        return MS_ERR_MALFORMED;
    }
    crc = ms_crc32_mpeg2(sec, slen - 4);
    got = ((uint32_t)sec[slen - 4] << 24) | ((uint32_t)sec[slen - 3] << 16) |
          ((uint32_t)sec[slen - 2] << 8) | (uint32_t)sec[slen - 1];
    if (crc != got) {
        return MS_ERR_MALFORMED;
    }

    version = (uint8_t)((sec[5] >> 1) & 0x1F);
    current_next = (uint8_t)(sec[5] & 0x01);
    section_number = sec[6];
    last_section = sec[7];
    if (current_next == 0) {
        return MS_OK;
    }
    /* 本阶段只接受单 section PAT，避免把不完整节目表当成完成。 */
    if (section_number != 0 || last_section != 0) {
        return MS_OK;
    }

    off = 8;
    end = slen - 4;
    while (off + 4 <= end) {
        uint16_t prog = (uint16_t)((sec[off] << 8) | sec[off + 1]);
        uint16_t pid = (uint16_t)(((sec[off + 2] & 0x1F) << 8) | sec[off + 3]);
        off += 4;
        if (prog == 0) {
            continue; /* NIT */
        }
        if (!found) {
            chosen_prog = prog;
            chosen_pid = pid;
            found = 1;
        }
    }
    if (!found || chosen_pid == 0 || chosen_pid == 0x1FFF) {
        return MS_OK;
    }

    if (p->have_pat && p->pat_version == version &&
        p->program_number == chosen_prog && p->pmt_pid == chosen_pid) {
        return MS_OK;
    }

    /* PAT 版本变化时清空旧 PMT/轨道，等待新的 PMT。 */
    if (p->have_pmt) {
        int i;
        for (i = 0; i < MS_TS_MAX_TRACKS; i++) {
            pes_acc_free(p, &p->pes[i]);
            track_release(p, &p->tracks[i]);
            memset(&p->tracks[i], 0, sizeof(p->tracks[i]));
        }
        p->ntracks = 0;
        p->selected_video = 0;
        p->selected_audio = 0;
        p->have_pmt = 0;
        psi_reset(&p->pmt);
        p->pre_pmt_replayed = 0;
        pre_pmt_free(p);
    }

    p->have_pat = 1;
    p->pat_version = version;
    p->program_number = chosen_prog;
    p->pmt_pid = chosen_pid;
    return MS_OK;
}

static ms_status finish_pmt(ms_ts_parser *p, const uint8_t *sec, size_t slen)
{
    uint16_t section_length;
    uint16_t prog;
    uint8_t version;
    uint8_t current_next;
    uint8_t section_number;
    uint8_t last_section;
    uint16_t prog_info_len;
    size_t off;
    size_t end;
    uint32_t crc;
    uint32_t got;
    int update;
    int first_video = 1;
    int first_audio = 1;
    int new_count = 0;
    ms_ts_track fresh[MS_TS_MAX_TRACKS];

    if (slen < 12) {
        return MS_ERR_MALFORMED;
    }
    if (sec[0] != 0x02) {
        return MS_OK;
    }
    if ((sec[1] & 0x80) == 0) {
        return MS_ERR_MALFORMED;
    }
    section_length = (uint16_t)(((sec[1] & 0x0F) << 8) | sec[2]);
    if ((size_t)3 + section_length != slen) {
        return MS_ERR_MALFORMED;
    }
    if (section_length < 13 || section_length > 1021) {
        return MS_ERR_MALFORMED;
    }
    crc = ms_crc32_mpeg2(sec, slen - 4);
    got = ((uint32_t)sec[slen - 4] << 24) | ((uint32_t)sec[slen - 3] << 16) |
          ((uint32_t)sec[slen - 2] << 8) | (uint32_t)sec[slen - 1];
    if (crc != got) {
        return MS_ERR_MALFORMED;
    }

    prog = (uint16_t)((sec[3] << 8) | sec[4]);
    if (p->have_pat && prog != p->program_number) {
        return MS_OK;
    }
    version = (uint8_t)((sec[5] >> 1) & 0x1F);
    current_next = (uint8_t)(sec[5] & 0x01);
    section_number = sec[6];
    last_section = sec[7];
    if (current_next == 0) {
        return MS_OK;
    }
    if (section_number != 0 || last_section != 0) {
        return MS_OK;
    }

    prog_info_len = (uint16_t)(((sec[10] & 0x0F) << 8) | sec[11]);
    off = 12;
    if (off + prog_info_len + 4 > slen) {
        return MS_ERR_MALFORMED;
    }
    off += prog_info_len;
    end = slen - 4;
    memset(fresh, 0, sizeof(fresh));

    while (off + 5 <= end) {
        uint8_t stype = sec[off];
        uint16_t epid = (uint16_t)(((sec[off + 1] & 0x1F) << 8) | sec[off + 2]);
        uint16_t es_len = (uint16_t)(((sec[off + 3] & 0x0F) << 8) | sec[off + 4]);
        const uint8_t *desc = sec + off + 5;
        ms_ts_track *t;
        off += 5;
        if (off + es_len > end) {
            return MS_ERR_MALFORMED;
        }
        if (new_count >= MS_TS_MAX_TRACKS) {
            return MS_ERR_OVERFLOW;
        }
        t = &fresh[new_count];
        t->in_use = 1;
        t->pid = epid;
        t->stream_type = stype;
        ms_ts_map_stream_type(stype, &t->codec, &t->kind, &t->level);
        lang_from_descriptors(desc, es_len, t->language);
        if (stype == 0x06 && desc_is_bitmap_sub(desc, es_len)) {
            t->kind = MS_TRACK_SUBTITLE;
            /* 0x59 subtitling_descriptor = DVB 位图字幕（EN 300 743），可解码到 RGBA cue；
             * 0x56 teletext 仍按未解析位图类登记（不播放）。 */
            t->codec = desc_has_tag(desc, es_len, 0x59) ? MS_CODEC_SUB_DVB : MS_CODEC_SUB_BITMAP;
            t->level = MS_RECOG_CONTAINER_ID;
        }
        if (t->kind == MS_TRACK_VIDEO && first_video) {
            t->selected = 1;
            first_video = 0;
        } else if (t->kind == MS_TRACK_AUDIO && first_audio) {
            t->selected = 1;
            first_audio = 0;
        }
        new_count++;
        off += es_len;
    }

    update = (p->have_pmt && p->pmt_version != version);
    if (p->have_pmt && p->pmt_version == version && p->ntracks == new_count) {
        int same = 1;
        int i;
        for (i = 0; i < new_count; i++) {
            if (p->tracks[i].pid != fresh[i].pid ||
                p->tracks[i].stream_type != fresh[i].stream_type) {
                same = 0;
                break;
            }
        }
        if (same) {
            return MS_OK;
        }
        update = 1;
    }

    {
        int i;
        for (i = 0; i < MS_TS_MAX_TRACKS; i++) {
            pes_acc_free(p, &p->pes[i]);
            track_release(p, &p->tracks[i]);
        }
    }
    memcpy(p->tracks, fresh, sizeof(fresh));
    p->ntracks = new_count;
    /* 解析状态按首次 PES 惰性分配，PMT 只登记轨道。 */
    p->have_pmt = 1;
    p->pmt_version = version;
    p->selected_video = !first_video;
    p->selected_audio = !first_audio;

    if (p->cb.on_track != NULL) {
        int i;
        for (i = 0; i < p->ntracks; i++) {
            ms_track_info info;
            ms_status st;
            memset(&info, 0, sizeof(info));
            info.id = p->tracks[i].pid;
            info.kind = p->tracks[i].kind;
            info.codec = p->tracks[i].codec;
            info.level = p->tracks[i].level;
            info.stream_type = p->tracks[i].stream_type;
            info.language = (p->tracks[i].language[0] != 0) ? p->tracks[i].language : NULL;
            info.name = NULL;
            info.is_default = p->tracks[i].selected;
            info.is_forced = 0;
            info.selected = p->tracks[i].selected;
            st = p->cb.on_track(p->cb.ctx, &info, update);
            if (st != MS_OK) {
                /* 回调失败时轨道表已写入；调用方应以 LIMIT 重试事件投递。
                 * 为避免重复投递，已成功的标 announced，失败处停止。 */
                int j;
                for (j = 0; j < i; j++) {
                    p->tracks[j].announced = 1;
                }
                return st;
            }
            p->tracks[i].announced = 1;
        }
    }
    return MS_OK;
}

static ms_status announce_pending_tracks(ms_ts_parser *p)
{
    int i;
    if (p->cb.on_track == NULL) {
        return MS_OK;
    }
    for (i = 0; i < p->ntracks; i++) {
        if (!p->tracks[i].announced) {
            ms_track_info info;
            ms_status st;
            memset(&info, 0, sizeof(info));
            info.id = p->tracks[i].pid;
            info.kind = p->tracks[i].kind;
            info.codec = p->tracks[i].codec;
            info.level = p->tracks[i].level;
            info.stream_type = p->tracks[i].stream_type;
            info.language = (p->tracks[i].language[0] != 0) ? p->tracks[i].language : NULL;
            info.is_default = p->tracks[i].selected;
            info.selected = p->tracks[i].selected;
            st = p->cb.on_track(p->cb.ctx, &info, 0);
            if (st != MS_OK) {
                return st;
            }
            p->tracks[i].announced = 1;
        }
    }
    return MS_OK;
}

/* 同一 TS 包里 section 后面的 0xFF 是填充，不能当成下一张表。 */
static void psi_skip_stuffing(ms_psi_acc *acc)
{
    size_t i = 0;
    while (i < acc->len && acc->buf[i] == 0xFF) {
        i++;
    }
    if (i == 0) {
        return;
    }
    if (i < acc->len) {
        memmove(acc->buf, acc->buf + i, acc->len - i);
        acc->len -= i;
    } else {
        acc->len = 0;
    }
    if (acc->len == 0) {
        psi_reset(acc);
    }
}

static ms_status psi_append(ms_psi_acc *acc, const uint8_t *data, size_t len)
{
    if (!acc->active) {
        return MS_OK;
    }
    if (len == 0) {
        return MS_OK;
    }
    if (acc->len > MS_PSI_MAX_SECTION || len > MS_PSI_MAX_SECTION - acc->len) {
        psi_reset(acc);
        return MS_ERR_OVERFLOW;
    }
    memcpy(acc->buf + acc->len, data, len);
    acc->len += len;
    if (acc->need == 0) {
        psi_skip_stuffing(acc);
        if (!acc->active) {
            return MS_OK;
        }
    }
    if (acc->need == 0 && acc->len >= 3) {
        unsigned slen = ((acc->buf[1] & 0x0F) << 8) | acc->buf[2];
        if (slen > MS_PSI_MAX_SECTION - 3) {
            psi_reset(acc);
            return MS_ERR_MALFORMED;
        }
        acc->need = 3 + slen;
    }
    return MS_OK;
}

static ms_status psi_take_complete(ms_ts_parser *p, ms_psi_acc *acc, int is_pat)
{
    ms_status st = MS_OK;
    while (acc->active && acc->need != 0 && acc->len >= acc->need) {
        size_t need = acc->need;
        if (is_pat) {
            st = finish_pat(p, acc->buf, need);
        } else {
            st = finish_pmt(p, acc->buf, need);
        }
        if (acc->len > need) {
            memmove(acc->buf, acc->buf + need, acc->len - need);
            acc->len -= need;
            acc->need = 0;
            psi_skip_stuffing(acc);
        } else {
            psi_reset(acc);
        }
        if (st != MS_OK) {
            return st;
        }
        if (!acc->active) {
            break;
        }
        if (acc->need == 0 && acc->len >= 3) {
            unsigned slen = ((acc->buf[1] & 0x0F) << 8) | acc->buf[2];
            if (slen > MS_PSI_MAX_SECTION - 3) {
                psi_reset(acc);
                return MS_ERR_MALFORMED;
            }
            acc->need = 3 + slen;
        }
    }
    return MS_OK;
}

static ms_status feed_psi(ms_ts_parser *p, ms_psi_acc *acc, int is_pat,
                          int pusi, int cc, int disc_ind,
                          const uint8_t *payload, size_t plen)
{
    ms_status st;
    int has_payload = (plen > 0);

    if (acc->have_cc) {
        uint8_t ccv = (uint8_t)(cc & 0x0F);
        uint8_t expected = (uint8_t)((acc->last_cc + 1) & 0x0F);
        if (has_payload && ccv == acc->last_cc) {
            if (!acc->dup_seen && (size_t)acc->last_plen == plen &&
                acc->last_tag == payload_tag(payload, plen)) {
                acc->dup_seen = 1;
                return MS_OK;
            }
            psi_reset(acc);
            acc->have_cc = 1;
            acc->last_cc = ccv;
            acc->dup_seen = 0;
            remember_psi_payload(acc, payload, plen);
            if (!pusi) {
                return MS_OK;
            }
        } else if (!has_payload && ccv == acc->last_cc) {
            if (disc_ind) {
                psi_reset(acc);
            }
            return MS_OK;
        } else if (!(has_payload && ccv == expected)) {
            psi_reset(acc);
            acc->have_cc = 1;
            acc->last_cc = ccv;
            acc->dup_seen = 0;
            if (has_payload) {
                remember_psi_payload(acc, payload, plen);
            }
            if (!pusi || !has_payload) {
                return MS_OK;
            }
        } else {
            acc->last_cc = ccv;
            acc->dup_seen = 0;
            remember_psi_payload(acc, payload, plen);
            if (disc_ind) {
                psi_reset(acc);
                if (!pusi) {
                    return MS_OK;
                }
            }
        }
    } else if (!has_payload) {
        acc->have_cc = 1;
        acc->last_cc = (uint8_t)(cc & 0x0F);
        acc->dup_seen = 0;
        return MS_OK;
    }

    if (!has_payload) {
        return MS_OK;
    }

    if (pusi) {
        unsigned pointer;
        if (plen < 1) {
            return MS_ERR_MALFORMED;
        }
        pointer = payload[0];
        if ((size_t)1 + pointer > plen) {
            return MS_ERR_MALFORMED;
        }
        if (acc->active && pointer > 0) {
            st = psi_append(acc, payload + 1, pointer);
            if (st != MS_OK) {
                return st;
            }
            st = psi_take_complete(p, acc, is_pat);
            if (st != MS_OK && st != MS_AGAIN) {
                return st;
            }
        }
        psi_reset(acc);
        acc->active = 1;
        acc->have_cc = 1;
        acc->last_cc = (uint8_t)cc;
        acc->dup_seen = 0;
        remember_psi_payload(acc, payload, plen);
        st = psi_append(acc, payload + 1 + pointer, plen - 1 - pointer);
        if (st != MS_OK) {
            return st;
        }
        return psi_take_complete(p, acc, is_pat);
    }

    if (!acc->active) {
        return MS_OK;
    }
    acc->last_cc = (uint8_t)cc;
    acc->have_cc = 1;
    acc->dup_seen = 0;
    remember_psi_payload(acc, payload, plen);
    st = psi_append(acc, payload, plen);
    if (st != MS_OK) {
        return st;
    }
    return psi_take_complete(p, acc, is_pat);
}

static int parse_pts(const uint8_t *h, unsigned expected_prefix, uint64_t *out)
{
    unsigned prefix;
    uint64_t pts;

    if (((h[0] & 0x01) == 0) || ((h[2] & 0x01) == 0) || ((h[4] & 0x01) == 0)) {
        return -1;
    }
    prefix = (h[0] >> 4) & 0x0F;
    if (prefix != expected_prefix) {
        return -1;
    }
    pts = ((uint64_t)(h[0] >> 1) & 0x07) << 30;
    pts |= ((uint64_t)h[1]) << 22;
    pts |= ((uint64_t)(h[2] >> 1)) << 15;
    pts |= ((uint64_t)h[3]) << 7;
    pts |= ((uint64_t)(h[4] >> 1));
    *out = pts;
    return 0;
}

/* ---- AVS / AVS+ 起始码切帧（GB/T 20090.2 / GY/T 257.1） ----
 * 0x42 只表示 AVS 家族；仅序列头 profile_id=0x48 才升格 AVS+ 并上送 CODEC_CONFIG（CSD=序列头）。
 * 切分规则：已含图像的 AU 遇到 B0（序列头）/B3（I）/B6（PB）/B7（编辑码）即结束；
 * PES 带 data_alignment 且已含图像时在 PES 末尾立即出帧，不等下一 PES（起播少等一帧）。 */
#define MS_TS_AVS_SC_SEQ 0xB0u
#define MS_TS_AVS_SC_END 0xB1u
#define MS_TS_AVS_SC_I   0xB3u
#define MS_TS_AVS_SC_PB  0xB6u
#define MS_TS_AVS_SC_EDIT 0xB7u

static ms_status avs_append(ms_ts_parser *p, ms_ts_track *t, const uint8_t *d, size_t n)
{
    size_t need;
    if (n == 0) {
        return MS_OK;
    }
    if (ms_size_add(t->avs_len, n, &need) != 0 || need > p->max_pes_bytes) {
        /* 超过单帧上限：丢掉这一帧，防止异常流无界增长。 */
        avs_reset_au(t);
        return MS_LIMIT;
    }
    if (need > t->avs_cap) {
        size_t cap = t->avs_cap ? t->avs_cap : 4096u;
        uint8_t *nb;
        while (cap < need) {
            cap = (cap > p->max_pes_bytes / 2u) ? p->max_pes_bytes : cap * 2u;
        }
        nb = (uint8_t *)ms_mem_alloc(p->alloc, cap);
        if (nb == NULL) {
            avs_reset_au(t);
            return ms_acct_is_budget_limit(p->alloc) ? MS_LIMIT : MS_ERR_NOMEM;
        }
        if (t->avs_len > 0) {
            memcpy(nb, t->avs_au, t->avs_len);
        }
        if (t->avs_au != NULL) {
            ms_mem_free(p->alloc, t->avs_au);
        }
        t->avs_au = nb;
        t->avs_cap = cap;
    }
    memcpy(t->avs_au + t->avs_len, d, n);
    t->avs_len += n;
    return MS_OK;
}

static ms_status avs_emit(ms_ts_parser *p, ms_ts_track *t)
{
    ms_track_info info;
    ms_au_frame au;
    ms_status st = MS_OK;

    if (t->avs_len == 0 || !t->avs_has_pic) {
        avs_reset_au(t);
        return MS_OK;
    }
    if (t->avs_has_seq) {
        ms_video_cfg vc;
        ms_video_cfg_default(&vc);
        if (ms_video_cfg_parse(MS_CODEC_AVS_PLUS, MS_SAMPLE_FMT_CAVS, 1,
                               t->avs_au, t->avs_len, &vc) == MS_OK &&
            vc.complete && vc.csd_size > 0 && vc.payload_sig != t->avs_cfg_sig) {
            if (t->codec != MS_CODEC_AVS_PLUS || !t->level_upgraded) {
                t->codec = MS_CODEC_AVS_PLUS;
                t->level = MS_RECOG_AU;
                t->level_upgraded = 1;
                if (p->cb.on_track != NULL) {
                    fill_track_info(t, &info);
                    st = p->cb.on_track(p->cb.ctx, &info, 1);
                    if (st != MS_OK && st != MS_LIMIT) {
                        avs_reset_au(t);
                        return st;
                    }
                }
            }
            if (p->cb.on_codec_config != NULL) {
                fill_track_info(t, &info);
                info.level = MS_RECOG_AU;
                st = p->cb.on_codec_config(p->cb.ctx, &info, vc.csd, vc.csd_size);
                if (st != MS_OK && st != MS_LIMIT) {
                    avs_reset_au(t);
                    return st;
                }
            }
            t->avs_cfg_sig = vc.payload_sig;
        }
    }
    if (p->cb.on_au != NULL) {
        fill_track_info(t, &info);
        info.level = (t->codec == MS_CODEC_AVS_PLUS) ? MS_RECOG_AU : t->level;
        memset(&au, 0, sizeof(au));
        au.data = t->avs_au;
        au.size = t->avs_len;
        au.has_pts = t->avs_has_pts;
        au.pts = t->avs_pts;
        au.has_dts = t->avs_has_dts ? 1 : t->avs_has_pts;
        au.dts = t->avs_has_dts ? t->avs_dts : t->avs_pts;
        au.is_keyframe = t->avs_key;
        au.sample_format = MS_SAMPLE_FMT_CAVS;
        au.discontinuity = t->pending_disc ? 1 : 0;
        t->pending_disc = 0;
        t->disc_event_open = 0;
        st = p->cb.on_au(p->cb.ctx, &info, &au);
    }
    avs_reset_au(t);
    return st;
}

/* 一个完整 PES 载荷进切帧器。aligned=PES 头 data_alignment_indicator。 */
static ms_status feed_avs_pes(ms_ts_parser *p, ms_ts_track *t,
                              const uint8_t *d, size_t n, int aligned,
                              int has_pts, uint64_t pts, int has_dts, uint64_t dts)
{
    size_t i = 0, seg = 0;
    int pts_used = 0;
    ms_status st, ret = MS_OK;

    while (n >= 4 && i + 3 < n) {
        unsigned sc;
        if (d[i] != 0 || d[i + 1] != 0 || d[i + 2] != 1) {
            i++;
            continue;
        }
        sc = d[i + 3];
        if (t->avs_has_pic &&
            (sc == MS_TS_AVS_SC_SEQ || sc == MS_TS_AVS_SC_I ||
             sc == MS_TS_AVS_SC_PB || sc == MS_TS_AVS_SC_EDIT)) {
            st = avs_append(p, t, d + seg, i - seg);
            if (st == MS_OK) {
                st = avs_emit(p, t);
            }
            if (st == MS_LIMIT) {
                ret = MS_LIMIT;
            } else if (st != MS_OK) {
                return st;
            }
            seg = i;
        }
        if (sc == MS_TS_AVS_SC_SEQ) {
            t->avs_has_seq = 1;
        } else if ((sc == MS_TS_AVS_SC_I || sc == MS_TS_AVS_SC_PB) && !t->avs_has_pic) {
            t->avs_has_pic = 1;
            t->avs_key = (sc == MS_TS_AVS_SC_I);
            if (has_pts && !pts_used) {
                t->avs_has_pts = 1;
                t->avs_pts = pts;
                t->avs_has_dts = has_dts;
                t->avs_dts = dts;
                pts_used = 1;
            }
        }
        i += 4;
    }
    if (seg < n) {
        st = avs_append(p, t, d + seg, n - seg);
        if (st == MS_LIMIT) {
            ret = MS_LIMIT;
        } else if (st != MS_OK) {
            return st;
        }
    }
    if (aligned && t->avs_has_pic) {
        st = avs_emit(p, t);
        if (st == MS_LIMIT) {
            ret = MS_LIMIT;
        } else if (st != MS_OK) {
            return st;
        }
    }
    (void)MS_TS_AVS_SC_END;
    return ret;
}

static ms_status store_done_pes(ms_ts_parser *p, ms_pes_acc *acc, ms_codec codec)
{
    const uint8_t *buf = acc->buf;
    size_t len = acc->len;
    unsigned stream_id;
    unsigned plen;
    unsigned hlen;
    unsigned flags;
    int has_pts = 0;
    int has_dts = 0;
    int aligned = 0;
    uint64_t pts = 0;
    uint64_t dts = 0;
    const uint8_t *payload;
    size_t payload_size;
    int slot = -1;
    int i;
    ms_pes_done *d;

    if (len < 6 || buf[0] != 0x00 || buf[1] != 0x00 || buf[2] != 0x01) {
        acc->len = 0;
        acc->need = 0;
        acc->in_use = 1;
        return MS_OK; /* 不是合法 PES，丢弃这一单元而不让会话失败 */
    }
    stream_id = buf[3];
    plen = ((unsigned)buf[4] << 8) | buf[5];
    (void)plen;

    /* padding / 特殊流不解析 PTS */
    if (stream_id != 0xBE && stream_id != 0xBF && stream_id != 0xB0 &&
        stream_id != 0xB1 && stream_id != 0xF0 && stream_id != 0xF1 &&
        stream_id != 0xF2 && stream_id != 0xF8 && stream_id != 0xFF &&
        len >= 9) {
        flags = buf[7];
        hlen = buf[8];
        aligned = (buf[6] & 0x04) ? 1 : 0;
        if (9u + hlen > len) {
            acc->len = 0;
            acc->need = 0;
            return MS_OK;
        }
        if ((flags & 0x80) != 0 && hlen >= 5) {
            if (parse_pts(buf + 9, ((flags & 0x40) != 0) ? 0x3 : 0x2, &pts) == 0) {
                has_pts = 1;
            }
        }
        if ((flags & 0x40) != 0 && hlen >= 10) {
            if (parse_pts(buf + 14, 0x1, &dts) == 0) {
                has_dts = 1;
            }
        }
        payload = buf + 9 + hlen;
        payload_size = len - (9 + hlen);
    } else {
        payload = buf + 6;
        payload_size = len - 6;
    }

    {
        ms_status aac_st = MS_OK;
        if (codec == MS_CODEC_AAC) {
            ms_ts_track *tr = track_by_pid(p, acc->pid);
            if (tr != NULL) {
                aac_st = feed_aac_track(p, tr, payload, payload_size, 1,
                                        has_pts, pts, has_dts, dts);
                if (aac_st != MS_OK && aac_st != MS_AGAIN && aac_st != MS_LIMIT) {
                    acc->len = 0;
                    acc->need = 0;
                    return aac_st;
                }
            }
        } else if (codec == MS_CODEC_SUB_DVB && payload_size > 0 && p->cb.on_au != NULL) {
            /* DVB 位图字幕：一 PES 即一个显示集，原样（含 0x20 0x00 前缀）作为字幕 AU 上送，
             * 由播放器 ms_sub_bitmap 解析为 RGBA cue。非持有缓冲，会话入队时复制。 */
            ms_ts_track *tr = track_by_pid(p, acc->pid);
            if (tr != NULL) {
                ms_track_info info;
                ms_au_frame au;
                fill_track_info(tr, &info);
                info.level = MS_RECOG_AU;
                memset(&au, 0, sizeof(au));
                au.data = payload;
                au.size = payload_size;
                au.has_pts = has_pts;
                au.pts = pts;
                au.has_dts = has_pts;
                au.dts = pts;
                au.is_keyframe = 1;
                au.discontinuity = tr->pending_disc ? 1 : 0;
                tr->pending_disc = 0;
                aac_st = p->cb.on_au(p->cb.ctx, &info, &au);
                if (aac_st != MS_OK && aac_st != MS_AGAIN && aac_st != MS_LIMIT) {
                    acc->len = 0;
                    acc->need = 0;
                    return aac_st;
                }
            }
        } else if ((codec == MS_CODEC_AVS_FAMILY || codec == MS_CODEC_AVS_PLUS) &&
                   payload_size > 0) {
            ms_ts_track *tr = track_by_pid(p, acc->pid);
            if (tr != NULL) {
                aac_st = feed_avs_pes(p, tr, payload, payload_size, aligned,
                                      has_pts, pts, has_dts, dts);
                if (aac_st != MS_OK && aac_st != MS_AGAIN && aac_st != MS_LIMIT) {
                    acc->len = 0;
                    acc->need = 0;
                    return aac_st;
                }
            }
        } else if (codec == MS_CODEC_H264) {
            ms_ts_track *tr = track_by_pid(p, acc->pid);
            if (tr != NULL) {
                aac_st = feed_h264_track(p, tr, payload, payload_size, 1,
                                         has_pts, pts, has_dts, dts);
                if (aac_st != MS_OK && aac_st != MS_AGAIN && aac_st != MS_LIMIT) {
                    acc->len = 0;
                    acc->need = 0;
                    return aac_st;
                }
            }
        }

    for (i = 0; i < MS_PES_DONE_MAX; i++) {
        if (!p->done[i].in_use) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        /* 调试队列满则丢掉最旧的一条，避免阻塞公共 push。 */
        pes_done_free(p, &p->done[0]);
        memmove(&p->done[0], &p->done[1], sizeof(p->done[0]) * (MS_PES_DONE_MAX - 1));
        memset(&p->done[MS_PES_DONE_MAX - 1], 0, sizeof(p->done[0]));
        slot = MS_PES_DONE_MAX - 1;
    }

    d = &p->done[slot];
    memset(d, 0, sizeof(*d));
    if (payload_size > 0) {
        d->payload = (uint8_t *)ms_mem_alloc(p->alloc, payload_size);
        if (d->payload == NULL) {
            /* 调试副本在预算不足或分配失败时放弃，不失败公共路径。 */
            d->payload_size = 0;
        } else {
            memcpy(d->payload, payload, payload_size);
            d->payload_size = payload_size;
        }
    }
    d->pid = acc->pid;
    d->codec = codec;
    d->has_pts = has_pts;
    d->has_dts = has_dts;
    d->pts = pts;
    d->dts = dts;
    d->in_use = 1;

    acc->len = 0;
    acc->need = 0;
        if (aac_st == MS_LIMIT) {
            return MS_LIMIT;
        }
        return MS_OK;
    }
}

static ms_status close_open_pes(ms_ts_parser *p)
{
    int i;
    for (i = 0; i < MS_TS_MAX_TRACKS; i++) {
        ms_pes_acc *acc = &p->pes[i];
        ms_ts_track *t;
        ms_codec codec;
        ms_status st;
        if (!acc->in_use || acc->buf == NULL || acc->len == 0) {
            continue;
        }
        t = track_by_pid(p, acc->pid);
        codec = (t != NULL) ? t->codec : MS_CODEC_UNKNOWN;
        if (acc->need != 0 && acc->len > acc->need) {
            acc->len = acc->need;
        }
        /* PES_packet_length=0 的视频 PES 只在下一 PUSI 或 EOS 结束。 */
        st = store_done_pes(p, acc, codec);
        if (st != MS_OK) {
            return st;
        }
    }
    /* EOS：未带 data_alignment 而暂存的 AVS 末帧也要送出。 */
    for (i = 0; i < MS_TS_MAX_TRACKS; i++) {
        ms_ts_track *t = &p->tracks[i];
        if (t->in_use && t->avs_has_pic && t->avs_len > 0) {
            ms_status st = avs_emit(p, t);
            if (st != MS_OK && st != MS_LIMIT) {
                return st;
            }
        }
    }
    return MS_OK;
}


static ms_status pes_close_if_complete(ms_ts_parser *p, ms_pes_acc *acc, ms_codec codec)
{
    if (acc->need != 0 && acc->len >= acc->need) {
        if (acc->len > acc->need) {
            acc->len = acc->need;
        }
        return store_done_pes(p, acc, codec);
    }
    return MS_OK;
}

static ms_status feed_pes(ms_ts_parser *p, uint16_t pid, ms_codec codec,
                          int pusi, int cc, int disc_ind,
                          const uint8_t *payload, size_t plen)
{
    ms_pes_acc *acc;
    ms_status st;
    int skip = 0;
    int gap = 0;
    int has_payload = (plen > 0);

    acc = pes_by_pid(p, pid);
    if (acc == NULL) {
        return MS_ERR_OVERFLOW;
    }

    st = evaluate_media_cc(p, acc, pid, (uint8_t)(cc & 0x0F), has_payload,
                           disc_ind, pusi, payload, plen, &skip, &gap);
    if (st != MS_OK) {
        return st;
    }
    if (skip || !has_payload) {
        return MS_OK;
    }

    if (pusi) {
        if (!gap && acc->len > 0) {
            /* 下一个 PES 开始，结束上一个（含 length=0 的视频 PES）。 */
            st = store_done_pes(p, acc, codec);
            if (st != MS_OK) {
                return st;
            }
        }
        acc->in_use = 1;
        acc->pid = pid;
        acc->len = 0;
        acc->need = 0;
        st = ms_buf_reserve(p->alloc, &acc->buf, &acc->cap, plen, p->max_pes_bytes);
        if (st != MS_OK) {
            if (st == MS_LIMIT && !ms_acct_is_budget_limit(p->alloc)) {
                return MS_ERR_MALFORMED;
            }
            return st;
        }
        memcpy(acc->buf, payload, plen);
        ms_copy_meter_add(p->meter, MS_COPY_ASM, plen);
        acc->len = plen;
        if (acc->len >= 6) {
            unsigned pkt_len = ((unsigned)acc->buf[4] << 8) | acc->buf[5];
            if (pkt_len != 0) {
                acc->need = 6 + pkt_len;
                if (acc->need > p->max_pes_bytes) {
                    acc->len = 0;
                    acc->need = 0;
                    return MS_ERR_MALFORMED;
                }
            }
        }
        return pes_close_if_complete(p, acc, codec);
    }

    if (acc->len == 0 && acc->need == 0) {
        return MS_OK; /* 尚未见到 PUSI，或缺片后等待新起点 */
    }
    {
        size_t total;
        if (ms_size_add(acc->len, plen, &total) != 0) {
            acc->len = 0;
            acc->need = 0;
            return MS_ERR_OVERFLOW;
        }
        if (total > p->max_pes_bytes) {
            acc->len = 0;
            acc->need = 0;
            return MS_ERR_MALFORMED;
        }
        st = ms_buf_reserve(p->alloc, &acc->buf, &acc->cap, total, p->max_pes_bytes);
        if (st != MS_OK) {
            if (st == MS_LIMIT && !ms_acct_is_budget_limit(p->alloc)) {
                return MS_ERR_MALFORMED;
            }
            return st;
        }
        memcpy(acc->buf + acc->len, payload, plen);
        ms_copy_meter_add(p->meter, MS_COPY_ASM, plen);
        acc->len = total;
    }
    if (acc->need == 0 && acc->len >= 6) {
        unsigned pkt_len = ((unsigned)acc->buf[4] << 8) | acc->buf[5];
        if (pkt_len != 0) {
            acc->need = 6 + pkt_len;
            if (acc->need > p->max_pes_bytes) {
                acc->len = 0;
                acc->need = 0;
                return MS_ERR_MALFORMED;
            }
        }
    }
    return pes_close_if_complete(p, acc, codec);
}

static ms_status parse_one_packet(ms_ts_parser *p, const uint8_t *ts, int *disc);

static ms_status hold_pre_pmt_packet(ms_ts_parser *p, const uint8_t *ts)
{
    size_t need;

    if (p->pre_pmt_replayed || p->pre_pmt_replaying) {
        return MS_OK;
    }
    if (p->pre_pmt == NULL) {
        need = (size_t)MS_TS_PRE_PMT_PACKETS * (size_t)MS_TS_PRE_PMT_PKT;
        p->pre_pmt = (uint8_t *)ms_mem_alloc(p->alloc, need);
        if (p->pre_pmt == NULL) {
            /* 预算不足则放弃暂存，行为与原先「PMT 前丢媒体」相同。 */
            return MS_OK;
        }
        p->pre_pmt_cap = (size_t)MS_TS_PRE_PMT_PACKETS;
        p->pre_pmt_count = 0;
        p->pre_pmt_sent = 0;
    }
    if (p->pre_pmt_count >= p->pre_pmt_cap) {
        memmove(p->pre_pmt, p->pre_pmt + MS_TS_PRE_PMT_PKT,
                (p->pre_pmt_count - 1) * (size_t)MS_TS_PRE_PMT_PKT);
        p->pre_pmt_count--;
        if (p->pre_pmt_sent > 0) {
            p->pre_pmt_sent--;
        }
    }
    memcpy(p->pre_pmt + p->pre_pmt_count * (size_t)MS_TS_PRE_PMT_PKT,
           ts, (size_t)MS_TS_PRE_PMT_PKT);
    ms_copy_meter_add(p->meter, MS_COPY_ASM, (size_t)MS_TS_PRE_PMT_PKT);
    p->pre_pmt_count++;
    return MS_OK;
}

static ms_status replay_pre_pmt(ms_ts_parser *p)
{
    ms_status st;

    if (p->pre_pmt_replaying || p->pre_pmt == NULL || p->pre_pmt_count == 0 ||
        !p->have_pmt) {
        if (p->have_pmt && (p->pre_pmt == NULL || p->pre_pmt_count == 0)) {
            p->pre_pmt_replayed = 1;
        }
        return MS_OK;
    }
    p->pre_pmt_replaying = 1;
    st = announce_pending_tracks(p);
    if (st != MS_OK) {
        p->pre_pmt_replaying = 0;
        return st;
    }
    while (p->pre_pmt_sent < p->pre_pmt_count) {
        int disc = 0;
        const uint8_t *pkt = p->pre_pmt + p->pre_pmt_sent * (size_t)MS_TS_PRE_PMT_PKT;
        st = parse_one_packet(p, pkt, &disc);
        if (st != MS_OK) {
            p->pre_pmt_replaying = 0;
            return st;
        }
        if (disc) {
            st = emit_disc(p, NULL);
            if (st != MS_OK) {
                p->pre_pmt_replaying = 0;
                return st;
            }
        }
        p->pre_pmt_sent++;
        st = announce_pending_tracks(p);
        if (st != MS_OK) {
            p->pre_pmt_replaying = 0;
            return st;
        }
    }
    pre_pmt_free(p);
    p->pre_pmt_replayed = 1;
    p->pre_pmt_replaying = 0;
    return MS_OK;
}

static ms_status parse_one_packet(ms_ts_parser *p, const uint8_t *ts, int *disc)
{
    unsigned tei = (ts[1] >> 7) & 0x1u;
    unsigned pusi = (ts[1] >> 6) & 0x1u;
    uint16_t pid = (uint16_t)(((ts[1] & 0x1F) << 8) | ts[2]);
    unsigned scram = (ts[3] >> 6) & 0x3u;
    unsigned adapt = (ts[3] >> 4) & 0x3u;
    unsigned cc = ts[3] & 0x0Fu;
    const uint8_t *payload = NULL;
    size_t plen = 0;
    int has_payload;

    *disc = 0;
    if (ts[0] != 0x47) {
        return MS_ERR_MALFORMED;
    }
    if (adapt == 0) {
        return MS_ERR_MALFORMED;
    }
    if (payload_of(ts, &payload, &plen) != 0) {
        return MS_ERR_MALFORMED;
    }
    has_payload = (plen > 0);

    if (pid == 0x1FFF) {
        return MS_OK;
    }
    if (tei) {
        /* TEI：载荷不可用，不更新 CC，丢弃该 PID 未完成 PES/ES 并发事件。 */
        return notify_pid_gap(p, pid);
    }
    if (scram != 0) {
        /* 加扰包不当成明文解析。仅当该 PID 已是媒体轨或已有未完成 PES 时
         * 丢弃不完整单元并发 DISCONTINUITY；PAT/PMT/未知 PID 只跳过明文解析。 */
        {
            ms_ts_track *tr = track_by_pid(p, pid);
            ms_pes_acc *acc = pes_find(p, pid);
            if (tr != NULL || (acc != NULL && acc->len > 0)) {
                ms_status gst = notify_pid_gap(p, pid);
                acc = pes_find(p, pid);
                if (acc != NULL) {
                    acc->have_cc = 1;
                    acc->last_cc = (uint8_t)cc;
                    acc->dup_seen = 0;
                }
                return gst;
            }
            return MS_OK;
        }
    }

    {
        int disc_ind = adapt_disc_indicator(ts);
        if (pid == 0) {
            return feed_psi(p, &p->pat, 1, (int)pusi, (int)cc, disc_ind, payload, plen);
        }
        if (p->have_pat && pid == p->pmt_pid) {
            return feed_psi(p, &p->pmt, 0, (int)pusi, (int)cc, disc_ind, payload, plen);
        }
        {
            ms_ts_track *tr = track_by_pid(p, pid);
            if (tr != NULL) {
                return feed_pes(p, pid, tr->codec, (int)pusi, (int)cc, disc_ind,
                                payload, plen);
            }
        }
        /* 尚无 PMT，或 PID 尚未登记：有界暂存，待首次/新 PMT 后回放。 */
        if (!p->have_pmt && !p->pre_pmt_replaying) {
            (void)hold_pre_pmt_packet(p, ts);
        }
    }
    (void)has_payload;
    return MS_OK;
}

ms_status ms_ts_parser_push(ms_ts_parser *p,
                            const uint8_t *data,
                            size_t size,
                            size_t *consumed,
                            int eos)
{
    size_t pos = 0;
    ms_status st;

    if (consumed == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    *consumed = 0;
    if (p == NULL || (data == NULL && size != 0)) {
        return MS_ERR_INVALID_ARG;
    }
    if (p->failed) {
        return p->fail_status;
    }

    st = announce_pending_tracks(p);
    if (st != MS_OK) {
        return st;
    }
    st = drain_all_adts(p);
    if (st != MS_OK) {
        return st;
    }
    if (p->have_pmt && !p->pre_pmt_replayed) {
        st = replay_pre_pmt(p);
        if (st != MS_OK) {
            return st;
        }
    }
    if (p->confirmed && !p->format_emitted) {
        st = emit_format(p);
        if (st != MS_OK) {
            return st;
        }
    }

    if (!p->confirmed) {
        ms_ts_scan_opt opt;
        ms_ts_scan_result r;
        size_t remain_cap;
        memset(&opt, 0, sizeof(opt));
        opt.struct_size = sizeof(opt);
        remain_cap = (p->probe_bytes > p->prefix_bytes)
                         ? (p->probe_bytes - p->prefix_bytes)
                         : 0;
        opt.max_scan = remain_cap;
        memset(&r, 0, sizeof(r));
        r.struct_size = sizeof(r);
        if (remain_cap < (size_t)MS_TS_SYNC_NEEDED * 188u) {
            fail_parser(p, MS_ERR_UNKNOWN);
            *consumed = 0;
            return MS_ERR_UNKNOWN;
        }
        st = ms_ts_scan(data, size, eos, &opt, &r);
        /* 仅在确认 PNG/JPEG 魔术或最终确认偏移后落种类；
         * 192 包头的前 4 字节在凑齐同步前不要标成 OTHER。 */
        if (r.kind == MS_TS_PREFIX_PNG || r.kind == MS_TS_PREFIX_JPEG) {
            p->prefix_kind = r.kind;
        }
        if (st == MS_OK) {
            p->confirmed = 1;
            p->packet_size = r.packet_size;
            p->resync_spent = 0;
            if (!(r.packet_size == 192 && r.ts_offset < 192 &&
                  r.kind != MS_TS_PREFIX_PNG && r.kind != MS_TS_PREFIX_JPEG)) {
                p->prefix_bytes += r.ts_offset;
            }
            if (p->prefix_kind == MS_TS_PREFIX_NONE) {
                p->prefix_kind = r.kind;
            }
            if (p->prefix_bytes == 0) {
                p->prefix_kind = MS_TS_PREFIX_NONE;
            }
            pos = r.ts_offset;
            st = emit_format(p);
            if (st != MS_OK) {
                /* 格式事件未入队时不要把同步前缀丢掉以外的数据标成已消费。 */
                *consumed = r.ts_offset;
                return st;
            }
            *consumed = r.ts_offset;
        } else if (st == MS_AGAIN) {
            size_t drop = r.keep_from;
            if (drop > size) {
                drop = size;
            }
            p->prefix_bytes += drop;
            *consumed = drop;
            return MS_AGAIN;
        } else {
            fail_parser(p, st);
            *consumed = 0;
            return st;
        }
    }

    while (pos < size) {
        const uint8_t *pkt;
        int disc = 0;
        size_t remain = size - pos;
        uint16_t psz = p->packet_size;
        size_t need = 188;

        if (remain < need) {
            break;
        }
        pkt = packet_ts(data + pos, psz);
        if (pkt[0] != 0x47) {
            size_t off = 0;
            uint16_t newpsz = 0;
            size_t window;

            window = remain;
            if (window > p->max_resync_bytes - p->resync_spent &&
                p->max_resync_bytes >= p->resync_spent) {
                window = p->max_resync_bytes - p->resync_spent;
            }
            if (p->resync_spent >= p->max_resync_bytes ||
                window < (size_t)MS_TS_SYNC_NEEDED * 188u ||
                !ms_ts_find_sync(data + pos, window, 0, &off, &newpsz)) {
                size_t spend = window;
                if (p->tolerant && !eos) {
                    /* 容错：长段垃圾不判死，丢掉已扫过的字节继续找同步 */
                    size_t keep = (size_t)MS_TS_SYNC_NEEDED * 204u;
                    p->resync_spent = 0;
                    p->tolerated++;
                    *consumed = pos + (window > keep ? window - keep : 0);
                    return MS_AGAIN;
                }
                if (ms_size_add(p->resync_spent, spend, &p->resync_spent) != 0 ||
                    p->resync_spent >= p->max_resync_bytes) {
                    fail_parser(p, MS_ERR_MALFORMED);
                    *consumed = pos;
                    return MS_ERR_MALFORMED;
                }
                if (eos) {
                    fail_parser(p, MS_ERR_MALFORMED);
                    *consumed = pos;
                    return MS_ERR_MALFORMED;
                }
                *consumed = pos;
                return MS_AGAIN;
            }
            p->packet_size = newpsz;
            psz = newpsz;
            pos += off;
            p->resync_spent = 0;
            st = notify_stream_resync(p);
            if (st != MS_OK) {
                *consumed = pos;
                return st;
            }
            continue;
        }

        if (psz == 192 && remain < 192 && !eos) {
            break; /* 等前缀也到齐，避免把下一包前缀拆开 */
        }
        if (psz == 192 && remain < 192 && eos && remain >= 188) {
            /* 允许末尾只有 188 字节 TS。 */
        } else if (psz == 192 && remain < 192) {
            break;
        }

        st = parse_one_packet(p, pkt, &disc);
        if (st == MS_LIMIT) {
            *consumed = pos;
            return MS_LIMIT;
        }
        if (st != MS_OK && p->tolerant &&
            (st == MS_ERR_MALFORMED || st == MS_ERR_OVERFLOW)) {
            /* 容错：坏包丢弃，PSI 累积复位，媒体 PID 按缺口处理（丢未完成单元并发不连续） */
            uint16_t bpid = (uint16_t)(((pkt[1] & 0x1F) << 8) | pkt[2]);
            p->tolerated++;
            disc = 0;
            if (bpid == 0) {
                psi_reset(&p->pat);
                st = MS_OK;
            } else if (p->have_pat && bpid == p->pmt_pid) {
                psi_reset(&p->pmt);
                st = MS_OK;
            } else {
                st = notify_pid_gap(p, bpid);
                if (st != MS_OK && st != MS_LIMIT) {
                    st = MS_OK;
                }
            }
        }
        if (st != MS_OK) {
            if (st == MS_ERR_MALFORMED || st == MS_ERR_OVERFLOW ||
                st == MS_ERR_UNKNOWN || st == MS_ERR_UNSUPPORTED) {
                fail_parser(p, st);
            }
            *consumed = pos;
            return st;
        }
        if (p->have_pmt && !p->pre_pmt_replayed) {
            st = replay_pre_pmt(p);
            if (st != MS_OK) {
                *consumed = pos;
                return st;
            }
        }
        if (disc) {
            st = emit_disc(p, NULL);
            if (st != MS_OK) {
                *consumed = pos;
                return st;
            }
        }
        if (psz == 192 && remain >= 192) {
            pos += 192;
        } else {
            pos += 188;
        }
        p->resync_spent = 0;
        *consumed = pos;

        st = announce_pending_tracks(p);
        if (st != MS_OK) {
            return st;
        }
    }

    *consumed = pos;
    if (eos && pos < size && p->confirmed) {
        /* 尾部不足一包：丢弃不完整包，不把它当成数据继续攒。 */
        *consumed = size;
    }
    if (eos) {
        st = end_all_adts(p);
        if (st != MS_OK) {
            return st;
        }
    }
    return MS_OK;
}

ms_status ms_ts_parser_pull_pes(ms_ts_parser *p, ms_pes_view *out)
{
    int i;
    if (p == NULL || out == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    if (p->failed) {
        return p->fail_status;
    }
    for (i = 0; i < MS_PES_DONE_MAX; i++) {
        if (p->done[i].in_use) {
            out->pid = p->done[i].pid;
            out->codec = p->done[i].codec;
            out->has_pts = p->done[i].has_pts;
            out->has_dts = p->done[i].has_dts;
            out->pts = p->done[i].pts;
            out->dts = p->done[i].dts;
            out->payload = p->done[i].payload;
            out->payload_size = p->done[i].payload_size;
            /* 所有权转给调用方：移出队列，由 session 再拷到自己的视图或立即释放。 */
            p->done[i].in_use = 0;
            p->done[i].payload = NULL;
            p->done[i].payload_size = 0;
            return MS_OK;
        }
    }
    return MS_AGAIN;
}

ms_status ms_ts_parser_drain(ms_ts_parser *p)
{
    if (p == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    if (p->failed) {
        return p->fail_status;
    }
    return drain_all_adts(p);
}
