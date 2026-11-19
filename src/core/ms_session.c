/*
 * 会话：上限、增量 push/pull、所有权、flush/reset/cancel、epoch。
 * by: Long <mycjzb@gmail.com>
 */

#include "ms_internal.h"
#include "ms_ts.h"

typedef struct ms_event_node {
    int used;
    int in_slot; /* 已被 pull 占用 */
    uint32_t gen;
    ms_event ev;
    char language[4];
    uint8_t *data;
    size_t data_size;
} ms_event_node;

typedef struct ms_slot {
    int used;
    uint32_t gen;
    int ev_index;
} ms_slot;

struct ms_session {
    ms_allocator user_alloc; /* 调用方或默认 malloc，不走动态预算 */
    ms_allocator alloc;      /* 动态缓冲的记账包装 */
    ms_mem_acct acct;
    int using_default_alloc;
    ms_session_limits limits;
    uint64_t session_epoch;
    uint64_t timeline_epoch;
    uint64_t selection_epoch;
    int canceled;
    int failed;
    ms_status fail_status;
    int eos_in;
    int eos_queued;
    int rate_num;
    int rate_den;

    uint8_t *in_buf;
    size_t in_len;
    size_t in_cap;

    ms_ts_parser ts;

    ms_event_node evq[MS_PENDING_EVENT_MAX];
    uint32_t ev_gen;

    ms_slot *slots;
    size_t nslots;

    uint8_t *debug_pes;
    size_t debug_pes_len;
    ms_pes_view debug_view;
    int debug_pes_live;
};

static void fail_session(ms_session *s, ms_status st)
{
    s->failed = 1;
    s->fail_status = st;
}

static size_t in_limit(const ms_session *s)
{
    if (s->ts.confirmed) {
        size_t lim = s->limits.max_resync_bytes;
        if (lim < 192u * 4u) {
            lim = 192u * 4u;
        }
        return lim;
    }
    return s->limits.probe_bytes;
}

static int evq_count(const ms_session *s)
{
    int n = 0;
    int i;
    for (i = 0; i < MS_PENDING_EVENT_MAX; i++) {
        if (s->evq[i].used) {
            n++;
        }
    }
    return n;
}

/* 入队世代比较：考虑 uint32 回绕。队列深度远小于 2^31，有符号差可靠。 */
static int evq_gen_before(uint32_t a, uint32_t b)
{
    return (int32_t)(a - b) < 0;
}

static void ev_clear_node(ms_session *s, ms_event_node *n)
{
    if (n->data != NULL) {
        ms_mem_free(&s->alloc, n->data);
    }
    memset(n, 0, sizeof(*n));
}

static void evq_clear_all(ms_session *s)
{
    int i;
    for (i = 0; i < MS_PENDING_EVENT_MAX; i++) {
        ev_clear_node(s, &s->evq[i]);
    }
    if (s->slots != NULL) {
        memset(s->slots, 0, sizeof(ms_slot) * s->nslots);
    }
}

static size_t evq_media_bytes(const ms_session *s)
{
    size_t n = 0;
    int i;
    for (i = 0; i < MS_PENDING_EVENT_MAX; i++) {
        if (s->evq[i].used &&
            (s->evq[i].ev.type == MS_EVENT_AU ||
             s->evq[i].ev.type == MS_EVENT_CODEC_CONFIG)) {
            n += s->evq[i].data_size;
        }
    }
    return n;
}

static ms_status evq_enqueue(ms_session *s, const ms_event *src,
                             const char *language,
                             const uint8_t *data, size_t data_size)
{
    int i;
    ms_event_node *n = NULL;

    /* 空槽仅作存储；交付顺序由 ev_gen 决定，与下标无关。 */
    for (i = 0; i < MS_PENDING_EVENT_MAX; i++) {
        if (!s->evq[i].used) {
            n = &s->evq[i];
            break;
        }
    }
    if (n == NULL) {
        return MS_LIMIT;
    }
    if ((src->type == MS_EVENT_AU || src->type == MS_EVENT_CODEC_CONFIG) &&
        data_size > 0) {
        size_t used = evq_media_bytes(s);
        size_t total;
        if (ms_size_add(used, data_size, &total) != 0 ||
            total > s->limits.au_queue_bytes) {
            return MS_LIMIT;
        }
    }
    memset(n, 0, sizeof(*n));
    n->ev = *src;
    n->ev.session_epoch = s->session_epoch;
    n->ev.timeline_epoch = s->timeline_epoch;
    n->ev.selection_epoch = s->selection_epoch;
    if (language != NULL && language[0] != 0) {
        n->language[0] = language[0];
        n->language[1] = language[1];
        n->language[2] = language[2];
        n->language[3] = 0;
        n->ev.track.language = n->language;
    } else {
        n->ev.track.language = NULL;
    }
    if (data != NULL && data_size > 0) {
        n->data = (uint8_t *)ms_mem_alloc(&s->alloc, data_size);
        if (n->data == NULL) {
            memset(n, 0, sizeof(*n));
            if (ms_acct_is_budget_limit(&s->alloc)) {
                return MS_LIMIT;
            }
            return MS_ERR_NOMEM;
        }
        memcpy(n->data, data, data_size);
        n->data_size = data_size;
        n->ev.data = n->data;
        n->ev.data_size = data_size;
    }
    s->ev_gen++;
    if (s->ev_gen == 0) {
        s->ev_gen = 1;
    }
    n->gen = s->ev_gen;
    n->used = 1;
    n->in_slot = 0;
    return MS_OK;
}

static ms_status cb_format(void *ctx, uint16_t packet_size)
{
    ms_session *s = (ms_session *)ctx;
    ms_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = MS_EVENT_FORMAT;
    ev.format = MS_FORMAT_MPEG_TS;
    ev.ts_packet_size = packet_size;
    return evq_enqueue(s, &ev, NULL, NULL, 0);
}

static ms_status cb_track(void *ctx, const ms_track_info *track, int update)
{
    ms_session *s = (ms_session *)ctx;
    ms_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = update ? MS_EVENT_TRACK_UPDATE : MS_EVENT_TRACK;
    ev.format = MS_FORMAT_MPEG_TS;
    ev.ts_packet_size = s->ts.packet_size;
    ev.track = *track;
    ev.track.language = NULL;
    return evq_enqueue(s, &ev, track->language, NULL, 0);
}

static ms_status cb_disc(void *ctx, const ms_track_info *track)
{
    ms_session *s = (ms_session *)ctx;
    ms_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = MS_EVENT_DISCONTINUITY;
    ev.format = s->ts.confirmed ? MS_FORMAT_MPEG_TS : MS_FORMAT_UNKNOWN;
    ev.ts_packet_size = s->ts.packet_size;
    ev.discontinuity = 1;
    if (track != NULL) {
        ev.track = *track;
        ev.track.language = NULL;
    }
    return evq_enqueue(s, &ev, track != NULL ? track->language : NULL, NULL, 0);
}

static void fill_event_track(ms_event *ev, const ms_track_info *track)
{
    ev->track = *track;
    ev->track.language = NULL;
}

static ms_status cb_config(void *ctx, const ms_track_info *track,
                           const uint8_t *data, size_t size)
{
    ms_session *s = (ms_session *)ctx;
    ms_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = MS_EVENT_CODEC_CONFIG;
    ev.format = MS_FORMAT_MPEG_TS;
    ev.ts_packet_size = s->ts.packet_size;
    fill_event_track(&ev, track);
    ev.time_base_num = 1;
    ev.time_base_den = 90000;
    return evq_enqueue(s, &ev, track->language, data, size);
}

static ms_status cb_au(void *ctx, const ms_track_info *track, const ms_au_frame *fr)
{
    ms_session *s = (ms_session *)ctx;
    ms_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = MS_EVENT_AU;
    ev.format = MS_FORMAT_MPEG_TS;
    ev.ts_packet_size = s->ts.packet_size;
    fill_event_track(&ev, track);
    ev.has_pts = fr->has_pts;
    ev.has_dts = fr->has_dts;
    ev.pts = fr->pts;
    ev.dts = fr->dts;
    ev.time_base_num = 1;
    ev.time_base_den = 90000;
    ev.is_keyframe = fr->is_keyframe;
    ev.format_changed = fr->format_changed;
    ev.discontinuity = fr->discontinuity;
    ev.preroll = 0;
    return evq_enqueue(s, &ev, track->language, fr->data, fr->size);
}

static void debug_pes_clear(ms_session *s)
{
    if (s->debug_pes != NULL) {
        ms_mem_free(&s->alloc, s->debug_pes);
    }
    s->debug_pes = NULL;
    s->debug_pes_len = 0;
    s->debug_pes_live = 0;
    memset(&s->debug_view, 0, sizeof(s->debug_view));
}

static void in_buf_clear(ms_session *s)
{
    s->in_len = 0;
}

static void in_buf_release(ms_session *s)
{
    ms_mem_free(&s->alloc, s->in_buf);
    s->in_buf = NULL;
    s->in_len = 0;
    s->in_cap = 0;
}

static void ts_bind(ms_session *s)
{
    ms_ts_callbacks cb;
    memset(&cb, 0, sizeof(cb));
    cb.ctx = s;
    cb.on_format = cb_format;
    cb.on_track = cb_track;
    cb.on_discontinuity = cb_disc;
    cb.on_codec_config = cb_config;
    cb.on_au = cb_au;
    /*
     * 组装上限使用独立的 max_assembly_bytes，不跟随缩小后的
     * au_queue_bytes，避免一 PES 多帧时反压变成 MALFORMED。
     */
    ms_ts_parser_init(&s->ts, &s->alloc,
                      s->limits.probe_bytes,
                      s->limits.max_resync_bytes,
                      s->limits.max_assembly_bytes,
                      &cb);
}

static int limits_has_field(size_t sz, size_t off, size_t field_sz)
{
    return sz >= off + field_sz;
}

static ms_status apply_one_limit(size_t req, size_t defv, size_t abs, size_t *out)
{
    if (req == 0) {
        *out = defv;
    } else if (req > abs || req > defv) {
        return MS_ERR_INVALID_ARG;
    } else {
        *out = req;
    }
    return MS_OK;
}

static ms_status apply_limits(const ms_session_limits *req, ms_session_limits *out)
{
    ms_session_limits d;
    ms_session_limits abs;
    size_t sz;

    ms_session_limits_default(&d);
    ms_session_limits_absolute(&abs);
    if (req == NULL) {
        *out = d;
        return MS_OK;
    }

    /* struct_size==0：旧五字段布局，不读扩展字段。 */
    sz = req->struct_size;
    if (sz == 0) {
        sz = offsetof(ms_session_limits, struct_size);
    }
    if (sz > sizeof(*req)) {
        sz = sizeof(*req);
    }
    if (sz < offsetof(ms_session_limits, struct_size)) {
        return MS_ERR_INVALID_ARG;
    }

    *out = d;
    if (apply_one_limit(req->probe_bytes, d.probe_bytes, abs.probe_bytes,
                        &out->probe_bytes) != MS_OK ||
        apply_one_limit(req->au_queue_bytes, d.au_queue_bytes, abs.au_queue_bytes,
                        &out->au_queue_bytes) != MS_OK ||
        apply_one_limit(req->out_slots, d.out_slots, abs.out_slots,
                        &out->out_slots) != MS_OK ||
        apply_one_limit(req->max_push_bytes, d.max_push_bytes, abs.max_push_bytes,
                        &out->max_push_bytes) != MS_OK ||
        apply_one_limit(req->max_resync_bytes, d.max_resync_bytes,
                        abs.max_resync_bytes, &out->max_resync_bytes) != MS_OK) {
        return MS_ERR_INVALID_ARG;
    }
    if (limits_has_field(sz, offsetof(ms_session_limits, max_assembly_bytes),
                         sizeof(req->max_assembly_bytes))) {
        if (apply_one_limit(req->max_assembly_bytes, d.max_assembly_bytes,
                            abs.max_assembly_bytes,
                            &out->max_assembly_bytes) != MS_OK) {
            return MS_ERR_INVALID_ARG;
        }
    }
    if (limits_has_field(sz, offsetof(ms_session_limits, session_budget_bytes),
                         sizeof(req->session_budget_bytes))) {
        if (apply_one_limit(req->session_budget_bytes, d.session_budget_bytes,
                            abs.session_budget_bytes,
                            &out->session_budget_bytes) != MS_OK) {
            return MS_ERR_INVALID_ARG;
        }
    }
    out->struct_size = sizeof(*out);
    if (out->out_slots < 1) {
        return MS_ERR_INVALID_ARG;
    }
    return MS_OK;
}

ms_status ms_session_create(const ms_allocator *alloc,
                            const ms_session_limits *limits,
                            ms_session **out)
{
    ms_session *s;
    ms_allocator a;
    ms_session_limits lim;
    ms_status st;

    if (out == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    *out = NULL;
    if (alloc != NULL && (alloc->alloc_fn == NULL || alloc->free_fn == NULL)) {
        return MS_ERR_INVALID_ARG;
    }
    st = apply_limits(limits, &lim);
    if (st != MS_OK) {
        return st;
    }
    if (alloc != NULL) {
        a = *alloc;
    } else {
        ms_default_allocator(&a);
    }
    /* 会话本体与输出槽不计入动态预算。 */
    s = (ms_session *)ms_mem_alloc(&a, sizeof(*s));
    if (s == NULL) {
        return MS_ERR_NOMEM;
    }
    memset(s, 0, sizeof(*s));
    s->user_alloc = a;
    s->using_default_alloc = (alloc == NULL);
    s->limits = lim;
    ms_acct_init(&s->acct, &s->user_alloc, lim.session_budget_bytes);
    ms_acct_bind(&s->acct, &s->alloc);
    s->session_epoch = 1;
    s->timeline_epoch = 1;
    s->selection_epoch = 1;
    s->rate_num = 1;
    s->rate_den = 1;
    s->nslots = lim.out_slots;
    s->slots = (ms_slot *)ms_mem_alloc(&s->user_alloc, sizeof(ms_slot) * s->nslots);
    if (s->slots == NULL) {
        ms_mem_free(&s->user_alloc, s);
        return MS_ERR_NOMEM;
    }
    memset(s->slots, 0, sizeof(ms_slot) * s->nslots);
    ts_bind(s);
    *out = s;
    return MS_OK;
}

void ms_session_destroy(ms_session *session)
{
    ms_allocator a;
    if (session == NULL) {
        return;
    }
    a = session->user_alloc;
    evq_clear_all(session);
    debug_pes_clear(session);
    ms_ts_parser_destroy(&session->ts);
    in_buf_release(session);
    ms_mem_free(&a, session->slots);
    ms_mem_free(&a, session);
}

static ms_status process_inbuf(ms_session *s, int eos)
{
    size_t used = 0;
    ms_status st;

    if (s->in_len == 0 && !eos) {
        return MS_OK;
    }
    st = ms_ts_parser_push(&s->ts, s->in_buf, s->in_len, &used, eos);
    if (used > s->in_len) {
        used = s->in_len;
    }
    if (used > 0) {
        if (used < s->in_len) {
            memmove(s->in_buf, s->in_buf + used, s->in_len - used);
            s->in_len -= used;
        } else {
            s->in_len = 0;
        }
    }
    if (st == MS_AGAIN) {
        return MS_OK;
    }
    if (st == MS_ERR_UNKNOWN || st == MS_ERR_MALFORMED ||
        st == MS_ERR_OVERFLOW || st == MS_ERR_NOMEM ||
        st == MS_ERR_UNSUPPORTED) {
        fail_session(s, st);
    }
    return st;
}

ms_status ms_session_push(ms_session *session,
                          const uint8_t *data,
                          size_t size,
                          size_t *consumed)
{
    ms_status st;
    size_t copied = 0;
    size_t lim;
    size_t space;

    if (consumed == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    *consumed = 0;
    if (session == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    if (size > 0 && data == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    if (session->canceled) {
        return MS_ERR_CANCELED;
    }
    if (session->failed) {
        return session->fail_status;
    }
    if (session->eos_in && size > 0) {
        return MS_ERR_STATE;
    }
    if (size > session->limits.max_push_bytes) {
        return MS_ERR_INVALID_ARG;
    }

    st = process_inbuf(session, 0);
    if (st != MS_OK && st != MS_LIMIT) {
        return st;
    }

    lim = in_limit(session);
    if (session->in_len > lim) {
        /* 不应发生；防御。 */
        session->in_len = lim;
    }
    space = lim - session->in_len;
    if (size > 0) {
        copied = (size < space) ? size : space;
        if (copied > 0) {
            size_t need;
            if (ms_size_add(session->in_len, copied, &need) != 0) {
                fail_session(session, MS_ERR_OVERFLOW);
                return MS_ERR_OVERFLOW;
            }
            st = ms_buf_reserve(&session->alloc, &session->in_buf,
                                &session->in_cap, need, lim);
            if (st != MS_OK) {
                /* 预算/硬上限 LIMIT 与 NOMEM 均回滚（未改 in_len），旧状态可用。 */
                return st;
            }
            memcpy(session->in_buf + session->in_len, data, copied);
            session->in_len = need;
        }
    }

    st = process_inbuf(session, 0);
    *consumed = copied;
    /* 处理过程中的预算 LIMIT/错误必须回传，不能因本次输入已拷入 in_buf 就改报 OK。 */
    if (st != MS_OK) {
        return st;
    }
    if (copied < size) {
        return MS_LIMIT;
    }
    return MS_OK;
}

ms_status ms_session_pull(ms_session *session, ms_event *out)
{
    int i;
    int ev_i = -1;
    int slot_i = -1;
    ms_event_node *n;
    uint32_t id;

    if (session == NULL || out == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    if (session->canceled) {
        return MS_ERR_CANCELED;
    }
    if (session->failed) {
        return session->fail_status;
    }

    {
        ms_status drain_st = ms_ts_parser_drain(&session->ts);
        if (drain_st == MS_ERR_UNKNOWN || drain_st == MS_ERR_MALFORMED ||
            drain_st == MS_ERR_OVERFLOW || drain_st == MS_ERR_NOMEM ||
            drain_st == MS_ERR_UNSUPPORTED) {
            fail_session(session, drain_st);
            return drain_st;
        }
    }

    for (i = 0; i < (int)session->nslots; i++) {
        if (!session->slots[i].used) {
            slot_i = i;
            break;
        }
    }
    if (slot_i < 0) {
        return MS_LIMIT;
    }

    /* 严格 FIFO：按入队世代取最旧的待交付事件，不按下标复用顺序。 */
    for (i = 0; i < MS_PENDING_EVENT_MAX; i++) {
        if (session->evq[i].used && !session->evq[i].in_slot) {
            if (ev_i < 0 || evq_gen_before(session->evq[i].gen, session->evq[ev_i].gen)) {
                ev_i = i;
            }
        }
    }
    if (ev_i < 0) {
        if (session->eos_in && !session->eos_queued && evq_count(session) == 0) {
            ms_event ev;
            ms_status st;
            memset(&ev, 0, sizeof(ev));
            ev.type = MS_EVENT_EOS;
            ev.format = session->ts.confirmed ? MS_FORMAT_MPEG_TS : MS_FORMAT_UNKNOWN;
            ev.ts_packet_size = session->ts.packet_size;
            st = evq_enqueue(session, &ev, NULL, NULL, 0);
            if (st != MS_OK) {
                return st;
            }
            session->eos_queued = 1;
            return ms_session_pull(session, out);
        }
        if (session->eos_queued) {
            return MS_EOS;
        }
        return MS_AGAIN;
    }

    n = &session->evq[ev_i];
    session->slots[slot_i].used = 1;
    session->slots[slot_i].ev_index = ev_i;
    session->slots[slot_i].gen++;
    if (session->slots[slot_i].gen == 0) {
        session->slots[slot_i].gen = 1;
    }
    n->in_slot = 1;
    /* release_id：高 16 位 generation，低 16 位槽位。过期 id 不会误释放。 */
    id = (session->slots[slot_i].gen << 16) | (uint32_t)slot_i;
    n->ev.release_id = id;
    *out = n->ev;
    return MS_OK;
}

ms_status ms_session_release(ms_session *session, uint32_t release_id)
{
    uint32_t slot = release_id & 0xFFFFu;
    uint32_t gen = release_id >> 16;
    int ev_i;

    if (session == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    if (slot >= session->nslots) {
        return MS_ERR_INVALID_ARG;
    }
    if (!session->slots[slot].used || session->slots[slot].gen != gen) {
        return MS_ERR_STATE;
    }
    ev_i = session->slots[slot].ev_index;
    session->slots[slot].used = 0;
    session->slots[slot].ev_index = 0;
    if (ev_i >= 0 && ev_i < MS_PENDING_EVENT_MAX && session->evq[ev_i].used) {
        ev_clear_node(session, &session->evq[ev_i]);
    }
    return MS_OK;
}

ms_status ms_session_flush(ms_session *session)
{
    if (session == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    if (session->canceled) {
        return MS_ERR_CANCELED;
    }
    evq_clear_all(session);
    debug_pes_clear(session);
    in_buf_release(session);
    ms_ts_parser_flush_partial(&session->ts);
    session->eos_queued = 0;
    return MS_OK;
}

ms_status ms_session_reset(ms_session *session)
{
    if (session == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    evq_clear_all(session);
    debug_pes_clear(session);
    in_buf_release(session);
    ms_ts_parser_reset(&session->ts);
    ts_bind(session);
    session->canceled = 0;
    session->failed = 0;
    session->fail_status = MS_OK;
    session->eos_in = 0;
    session->eos_queued = 0;
    session->session_epoch++;
    if (session->session_epoch == 0) {
        session->session_epoch = 1;
    }
    return MS_OK;
}

ms_status ms_session_cancel(ms_session *session)
{
    if (session == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    session->canceled = 1;
    return MS_OK;
}

ms_status ms_session_end_of_input(ms_session *session)
{
    ms_status st;
    if (session == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    if (session->canceled) {
        return MS_ERR_CANCELED;
    }
    if (session->failed) {
        return session->fail_status;
    }
    session->eos_in = 1;
    st = process_inbuf(session, 1);
    if (st != MS_OK && st != MS_LIMIT && st != MS_AGAIN) {
        return st;
    }
    in_buf_clear(session);
    return MS_OK;
}

ms_status ms_session_epochs(const ms_session *session, ms_epochs *out)
{
    if (session == NULL || out == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    out->session_epoch = session->session_epoch;
    out->timeline_epoch = session->timeline_epoch;
    out->selection_epoch = session->selection_epoch;
    return MS_OK;
}

ms_status ms_session_seek(ms_session *session, int64_t time_90k)
{
    (void)time_90k;
    if (session == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    if (session->canceled) {
        return MS_ERR_CANCELED;
    }
    return MS_ERR_NOT_SEEKABLE;
}

ms_status ms_session_set_rate(ms_session *session, int num, int den)
{
    if (session == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    if (session->canceled) {
        return MS_ERR_CANCELED;
    }
    if (den == 0) {
        return MS_ERR_INVALID_ARG;
    }
    session->rate_num = num;
    session->rate_den = den;
    return MS_OK;
}

ms_status ms_session_select_track(ms_session *session, uint32_t track_id, int on)
{
    (void)track_id;
    (void)on;
    if (session == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    if (session->canceled) {
        return MS_ERR_CANCELED;
    }
    return MS_ERR_UNSUPPORTED;
}

ms_status ms_session_debug_pull_pes(ms_session *session, ms_pes_view *out)
{
    ms_status st;
    ms_pes_view tmp;

    if (session == NULL || out == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    if (session->canceled) {
        return MS_ERR_CANCELED;
    }
    if (session->failed) {
        return session->fail_status;
    }
    memset(&tmp, 0, sizeof(tmp));
    st = ms_ts_parser_pull_pes(&session->ts, &tmp);
    if (st != MS_OK) {
        return st;
    }
    debug_pes_clear(session);
    session->debug_pes = (uint8_t *)(void *)tmp.payload;
    session->debug_pes_len = tmp.payload_size;
    session->debug_view = tmp;
    session->debug_pes_live = 1;
    *out = tmp;
    /* 指针由会话持有，直到下一次 debug_pull / flush / reset / destroy。不是 AU。 */
    return MS_OK;
}

void ms_session_debug_budget(const ms_session *session,
                             size_t *used, size_t *peak, size_t *budget)
{
    if (used != NULL) {
        *used = (session != NULL) ? session->acct.used : 0;
    }
    if (peak != NULL) {
        *peak = (session != NULL) ? session->acct.peak : 0;
    }
    if (budget != NULL) {
        *budget = (session != NULL) ? session->acct.budget : 0;
    }
}

int ms_session_debug_limit_relievable(const ms_session *session)
{
    int i;
    if (session == NULL) {
        return 0;
    }
    for (i = 0; i < MS_PENDING_EVENT_MAX; i++) {
        if (session->evq[i].used) {
            return 1;
        }
    }
    return 0;
}
