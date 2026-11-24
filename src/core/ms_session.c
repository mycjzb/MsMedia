/*
 * 会话：上限、增量 push/pull、所有权、flush/reset/cancel、epoch。
 * by: Long <mycjzb@gmail.com>
 */

#include "ms_internal.h"
#include "ms_ts.h"
#include "ms_bmff.h"
#include "ms_flv.h"

#ifndef MS_SESS_TRACK_MAX
#define MS_SESS_TRACK_MAX 32
#endif
#ifndef MS_SESS_CFG_MAX
#define MS_SESS_CFG_MAX 1024
#endif
#ifndef MS_SESS_NAME_MAX
#define MS_SESS_NAME_MAX 48
#endif
#ifndef MS_SESS_LANG_MAX
#define MS_SESS_LANG_MAX 16
#endif

typedef struct ms_sess_trk {
    int in_use;
    uint32_t id;
    ms_track_kind kind;
    ms_codec codec;
    ms_recog_level level;
    uint16_t stream_type;
    char language[MS_SESS_LANG_MAX];
    char name[MS_SESS_NAME_MAX];
    int is_default;
    int is_forced;
    int selected;
    uint8_t cfg[MS_SESS_CFG_MAX];
    size_t cfg_size;
    ms_sample_format cfg_fmt;
    int has_cfg;
    int tb_num;
    int tb_den;
} ms_sess_trk;


typedef struct ms_event_node {
    int used;
    int in_slot; /* 已被 pull 占用 */
    uint32_t gen;
    ms_event ev;
    char language[4];
    uint8_t *data;
    size_t data_size;
    uint32_t skip_samples_start;
    uint32_t skip_samples_end;
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

    int demux; /* 0 未定，1 TS，2 BMFF，3 FLV */
    ms_ts_parser ts;
    int ts_tolerant; /* 单 so 网络输入容错（内部开关，默认 0） */
    ms_bmff_parser *bmff; /* 确认 BMFF 后懒分配，计入动态预算 */
    int bmff_unsup_queued;
    ms_flv_parser *flv; /* 确认 FLV 后懒分配，计入动态预算 */
    int flv_unsup_queued;

    ms_copy_meter meter;
    int force_copy;
    uint64_t bytes_in;
    int has_format;
    int has_track;
    int has_config;
    int has_key_au;
    int has_video_key_au;
    uint64_t bytes_at_format;
    uint64_t bytes_at_track;
    uint64_t bytes_at_config;
    uint64_t bytes_at_key_au;
    uint64_t bytes_at_video_key_au;

    ms_event_node evq[MS_PENDING_EVENT_MAX];
    uint32_t ev_gen;

    ms_slot *slots;
    size_t nslots;

    uint8_t *debug_pes;
    size_t debug_pes_len;
    ms_pes_view debug_view;
    int debug_pes_live;

    int has_src;
    ms_source src;
    int in_source_cb;
    uint64_t src_pos;
    int src_eos;
    uint8_t *src_buf;
    size_t src_buf_cap;
    int has_seek_land;
    int64_t seek_request_90k;
    int64_t seek_land_90k;

    /* P4 轨道目录：懒分配，不挤占固定会话对象。 */
    struct ms_sess_trk *tracks;
    int ntracks;
    int cap_tracks;
    int user_audio_sel; /* 0=跟随默认首条，1=用户已选/关闭 */
    int user_sub_sel;
    ms_sub_doc *sub_ext;
    ms_sub_doc *sub_inband;
    int ext_sub_on; /* 目录中有外挂轨 */
};

static void fail_session(ms_session *s, ms_status st)
{
    s->failed = 1;
    s->fail_status = st;
}

static size_t in_limit(const ms_session *s)
{
    if (s->demux == 2 || s->demux == 3) {
        /* BMFF/FLV 确认后大载荷直接交给解析器；输入窗只容纳短头。 */
        return 256;
    }
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
             s->evq[i].ev.type == MS_EVENT_CODEC_CONFIG ||
             s->evq[i].ev.type == MS_EVENT_SUBTITLE)) {
            n += s->evq[i].data_size;
        }
    }
    return n;
}


static ms_format session_format(const ms_session *s)
{
    if (s->demux == 2 && s->bmff != NULL) {
        ms_format f = ms_bmff_parser_format(s->bmff);
        return (f != MS_FORMAT_UNKNOWN) ? f : MS_FORMAT_UNKNOWN;
    }
    if (s->demux == 3) {
        return MS_FORMAT_FLV;
    }
    if (s->ts.confirmed) {
        return MS_FORMAT_MPEG_TS;
    }
    return MS_FORMAT_UNKNOWN;
}

static ms_sample_format default_sample_fmt(ms_codec codec, ms_format fmt)
{
    if (fmt == MS_FORMAT_MP4 || fmt == MS_FORMAT_MOV || fmt == MS_FORMAT_FMP4 ||
        fmt == MS_FORMAT_FLV) {
        if (codec == MS_CODEC_H264) {
            return MS_SAMPLE_FMT_AVCC;
        }
        if (codec == MS_CODEC_AAC) {
            return MS_SAMPLE_FMT_AAC_RAW;
        }
        if (codec == MS_CODEC_HEVC) {
            return MS_SAMPLE_FMT_HVCC; /* MP4 hvc1/hev1、FLV HEVC：长度前缀 NAL */
        }
        return MS_SAMPLE_FMT_UNKNOWN;
    }
    if (codec == MS_CODEC_H264) {
        return MS_SAMPLE_FMT_ANNEXB;
    }
    if (codec == MS_CODEC_AAC) {
        return MS_SAMPLE_FMT_ADTS;
    }
    if (codec == MS_CODEC_AVS_PLUS || codec == MS_CODEC_AVS_FAMILY) {
        return MS_SAMPLE_FMT_CAVS; /* TS 0x42：起始码 ES */
    }
    return MS_SAMPLE_FMT_UNKNOWN;
}

static void note_startup(ms_session *s, const ms_event *ev);

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
    if ((src->type == MS_EVENT_AU || src->type == MS_EVENT_CODEC_CONFIG ||
         src->type == MS_EVENT_SUBTITLE) &&
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
        ms_copy_meter_add(&s->meter, MS_COPY_OUT, data_size);
    }
    s->ev_gen++;
    if (s->ev_gen == 0) {
        s->ev_gen = 1;
    }
    n->gen = s->ev_gen;
    n->used = 1;
    n->in_slot = 0;
    note_startup(s, &n->ev);
    return MS_OK;
}

static void note_startup(ms_session *s, const ms_event *ev)
{
    if (s == NULL || ev == NULL) {
        return;
    }
    if (ev->type == MS_EVENT_FORMAT && !s->has_format) {
        s->has_format = 1;
        s->bytes_at_format = s->bytes_in;
    } else if (ev->type == MS_EVENT_TRACK && !s->has_track) {
        s->has_track = 1;
        s->bytes_at_track = s->bytes_in;
    } else if (ev->type == MS_EVENT_CODEC_CONFIG && !s->has_config) {
        s->has_config = 1;
        s->bytes_at_config = s->bytes_in;
    } else if (ev->type == MS_EVENT_AU && ev->is_keyframe) {
        if (!s->has_key_au) {
            s->has_key_au = 1;
            s->bytes_at_key_au = s->bytes_in;
        }
        if (!s->has_video_key_au && ev->track.kind == MS_TRACK_VIDEO) {
            s->has_video_key_au = 1;
            s->bytes_at_video_key_au = s->bytes_in;
        }
    }
}

static void stats_clear(ms_session *s)
{
    memset(&s->meter, 0, sizeof(s->meter));
    s->bytes_in = 0;
    s->has_format = 0;
    s->has_track = 0;
    s->has_config = 0;
    s->has_key_au = 0;
    s->has_video_key_au = 0;
    s->bytes_at_format = 0;
    s->bytes_at_track = 0;
    s->bytes_at_config = 0;
    s->bytes_at_key_au = 0;
    s->bytes_at_video_key_au = 0;
}

/*
 * 可选所有权移交。成功且采取移交时 *owned_taken=1，n->data 为可释放块首，
 * ev.data 可为块内视图。失败不置 taken，调用方继续持有。
 */
static ms_status evq_enqueue_owned(ms_session *s, const ms_event *src,
                                   const char *language,
                                   const uint8_t *data, size_t data_size,
                                   uint8_t *owned_base, size_t owned_cap,
                                   int *owned_taken)
{
    int i;
    ms_event_node *n = NULL;
    int can_move = 0;

    for (i = 0; i < MS_PENDING_EVENT_MAX; i++) {
        if (!s->evq[i].used) {
            n = &s->evq[i];
            break;
        }
    }
    if (n == NULL) {
        return MS_LIMIT;
    }
    if ((src->type == MS_EVENT_AU || src->type == MS_EVENT_CODEC_CONFIG ||
         src->type == MS_EVENT_SUBTITLE) &&
        data_size > 0) {
        size_t used = evq_media_bytes(s);
        size_t total;
        if (ms_size_add(used, data_size, &total) != 0 ||
            total > s->limits.au_queue_bytes) {
            return MS_LIMIT;
        }
    }
    if (data != NULL && data_size > 0 && owned_base != NULL && !s->force_copy &&
        ms_owned_contains(owned_base, owned_cap, data, data_size)) {
        can_move = 1;
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
    if (data != NULL && data_size > 0 && can_move) {
        n->data = owned_base;
        n->data_size = data_size;
        n->ev.data = data;
        n->ev.data_size = data_size;
        if (owned_taken != NULL) {
            *owned_taken = 1;
        }
        ms_copy_meter_add(&s->meter, MS_COPY_MOVE, data_size);
    } else if (data != NULL && data_size > 0) {
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
        ms_copy_meter_add(&s->meter, MS_COPY_OUT, data_size);
    }
    s->ev_gen++;
    if (s->ev_gen == 0) {
        s->ev_gen = 1;
    }
    n->gen = s->ev_gen;
    n->used = 1;
    n->in_slot = 0;
    note_startup(s, &n->ev);
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


static int sub_playable(ms_codec c)
{
    return c == MS_CODEC_SUB_TEXT || c == MS_CODEC_SUB_SRT ||
           c == MS_CODEC_SUB_WEBVTT || c == MS_CODEC_SUB_TX3G ||
           c == MS_CODEC_SUB_ASS || c == MS_CODEC_SUB_DVB ||
           c == MS_CODEC_SUB_PGS || c == MS_CODEC_SUB_VOBSUB;
}

static void catalog_clear(ms_session *s)
{
    if (s->tracks != NULL) {
        ms_mem_free(&s->user_alloc, s->tracks);
        s->tracks = NULL;
    }
    s->ntracks = 0;
    s->cap_tracks = 0;
    s->user_audio_sel = 0;
    s->user_sub_sel = 0;
    if (s->sub_ext != NULL) {
        ms_sub_doc_destroy(s->sub_ext);
        s->sub_ext = NULL;
    }
    if (s->sub_inband != NULL) {
        ms_sub_doc_destroy(s->sub_inband);
        s->sub_inband = NULL;
    }
    s->ext_sub_on = 0;
}

static void catalog_clear_inband(ms_session *s)
{
    if (s->sub_inband != NULL) {
        ms_sub_doc_clear_cues(s->sub_inband);
    }
}

static int catalog_find(const ms_session *s, uint32_t id)
{
    int i;
    if (s == NULL || s->tracks == NULL) {
        return -1;
    }
    for (i = 0; i < s->ntracks; i++) {
        if (s->tracks[i].in_use && s->tracks[i].id == id) {
            return i;
        }
    }
    return -1;
}

static void copy_lang(char *dst, size_t cap, const char *src)
{
    size_t n = 0;
    if (cap == 0) {
        return;
    }
    if (src == NULL) {
        dst[0] = 0;
        return;
    }
    while (n + 1 < cap && src[n] != 0) {
        dst[n] = src[n];
        n++;
    }
    dst[n] = 0;
}

static int catalog_upsert(ms_session *s, const ms_track_info *tr)
{
    int i;
    if (s == NULL || tr == NULL) {
        return -1;
    }
    i = catalog_find(s, tr->id);
    if (i < 0) {
        if (s->tracks == NULL) {
            s->tracks = (ms_sess_trk *)ms_mem_alloc(&s->user_alloc,
                                                    sizeof(ms_sess_trk) * MS_SESS_TRACK_MAX);
            if (s->tracks == NULL) {
                return -1;
            }
            memset(s->tracks, 0, sizeof(ms_sess_trk) * MS_SESS_TRACK_MAX);
            s->cap_tracks = MS_SESS_TRACK_MAX;
        }
        if (s->ntracks >= s->cap_tracks) {
            return -1;
        }
        i = s->ntracks++;
        memset(&s->tracks[i], 0, sizeof(s->tracks[i]));
        s->tracks[i].in_use = 1;
        s->tracks[i].id = tr->id;
        s->tracks[i].is_default = tr->is_default;
        /* 新音频：用户未指定时跟随 demux 默认（通常是第一条）。字幕默认不选中。 */
        if (tr->kind == MS_TRACK_AUDIO && !s->user_audio_sel) {
            s->tracks[i].selected = tr->selected || tr->is_default;
        } else if (tr->kind == MS_TRACK_VIDEO) {
            s->tracks[i].selected = tr->selected || tr->is_default;
        } else if (tr->kind == MS_TRACK_SUBTITLE) {
            s->tracks[i].selected = s->user_sub_sel ? 0 : 0;
        } else {
            s->tracks[i].selected = 0;
        }
    }
    s->tracks[i].kind = tr->kind;
    s->tracks[i].codec = tr->codec;
    s->tracks[i].level = tr->level;
    s->tracks[i].stream_type = tr->stream_type;
    s->tracks[i].is_default = tr->is_default;
    s->tracks[i].is_forced = tr->is_forced;
    copy_lang(s->tracks[i].language, sizeof(s->tracks[i].language), tr->language);
    copy_lang(s->tracks[i].name, sizeof(s->tracks[i].name), tr->name);
    return i;
}

static void catalog_fill_info(const ms_session *s, int i, ms_track_info *out)
{
    memset(out, 0, sizeof(*out));
    if (s == NULL || s->tracks == NULL || i < 0 || i >= s->ntracks || !s->tracks[i].in_use) {
        return;
    }
    out->id = s->tracks[i].id;
    out->kind = s->tracks[i].kind;
    out->codec = s->tracks[i].codec;
    out->level = s->tracks[i].level;
    out->stream_type = s->tracks[i].stream_type;
    out->language = s->tracks[i].language[0] ? s->tracks[i].language : NULL;
    out->name = s->tracks[i].name[0] ? s->tracks[i].name : NULL;
    out->is_default = s->tracks[i].is_default;
    out->is_forced = s->tracks[i].is_forced;
    out->selected = s->tracks[i].selected;
}

static int catalog_selected(const ms_session *s, uint32_t id, ms_track_kind kind)
{
    int i = catalog_find(s, id);
    if (i >= 0) {
        return s->tracks[i].selected;
    }
    if (kind == MS_TRACK_VIDEO) {
        return 1;
    }
    if (kind == MS_TRACK_AUDIO) {
        return !s->user_audio_sel; /* 未知 id：未选过则允许 demux 默认路径 */
    }
    return 0;
}

static int should_emit_media(const ms_session *s, const ms_track_info *tr)
{
    if (tr == NULL) {
        return 0;
    }
    if (tr->kind == MS_TRACK_VIDEO) {
        return catalog_selected(s, tr->id, tr->kind);
    }
    if (tr->kind == MS_TRACK_AUDIO || tr->kind == MS_TRACK_SUBTITLE) {
        return catalog_selected(s, tr->id, tr->kind);
    }
    return 0;
}

static void evq_drop_track_media(ms_session *s, uint32_t id, int any_sub)
{
    int i;
    for (i = 0; i < MS_PENDING_EVENT_MAX; i++) {
        ms_event_type ty;
        if (!s->evq[i].used || s->evq[i].in_slot) {
            continue;
        }
        ty = s->evq[i].ev.type;
        if (ty != MS_EVENT_AU && ty != MS_EVENT_CODEC_CONFIG && ty != MS_EVENT_SUBTITLE) {
            continue;
        }
        if (any_sub && ty == MS_EVENT_SUBTITLE) {
            ev_clear_node(s, &s->evq[i]);
            continue;
        }
        if (s->evq[i].ev.track.id == id) {
            ev_clear_node(s, &s->evq[i]);
        }
    }
}

static void cache_cfg(ms_session *s, const ms_track_info *tr, const uint8_t *data, size_t size,
                      ms_sample_format fmt, int tb_num, int tb_den)
{
    int i;
    if (tr == NULL || data == NULL || size == 0) {
        return;
    }
    i = catalog_upsert(s, tr);
    if (i < 0) {
        return;
    }
    if (size > MS_SESS_CFG_MAX) {
        return;
    }
    memcpy(s->tracks[i].cfg, data, size);
    s->tracks[i].cfg_size = size;
    s->tracks[i].cfg_fmt = fmt;
    s->tracks[i].has_cfg = 1;
    s->tracks[i].tb_num = tb_num > 0 ? tb_num : 1;
    s->tracks[i].tb_den = tb_den > 0 ? tb_den : 90000;
}

static ms_status enqueue_cached_cfg(ms_session *s, int i)
{
    ms_event ev;
    ms_track_info info;
    if (i < 0 || !s->tracks[i].has_cfg) {
        return MS_OK;
    }
    catalog_fill_info(s, i, &info);
    memset(&ev, 0, sizeof(ev));
    ev.type = MS_EVENT_CODEC_CONFIG;
    ev.format = session_format(s);
    ev.ts_packet_size = (s->demux == 1) ? s->ts.packet_size : 0;
    ev.track = info;
    ev.track.language = NULL;
    ev.time_base_num = s->tracks[i].tb_num;
    ev.time_base_den = s->tracks[i].tb_den;
    ev.sample_format = s->tracks[i].cfg_fmt;
    ev.is_keyframe = 1;
    return evq_enqueue(s, &ev, info.language, s->tracks[i].cfg, s->tracks[i].cfg_size);
}

static ms_status enqueue_track_update(ms_session *s, int i)
{
    ms_event ev;
    ms_track_info info;
    catalog_fill_info(s, i, &info);
    memset(&ev, 0, sizeof(ev));
    ev.type = MS_EVENT_TRACK_UPDATE;
    ev.format = session_format(s);
    ev.ts_packet_size = (s->demux == 1) ? s->ts.packet_size : 0;
    ev.track = info;
    ev.track.language = NULL;
    ev.sample_format = default_sample_fmt(info.codec, ev.format);
    return evq_enqueue(s, &ev, info.language, NULL, 0);
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
    ev.sample_format = default_sample_fmt(track->codec, ev.format);
    {
        int ix = catalog_upsert(s, track);
        if (ix >= 0) {
            ev.track.selected = s->tracks[ix].selected;
            ev.track.is_default = s->tracks[ix].is_default;
        }
    }
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
    ev.sample_format = default_sample_fmt(track->codec, ev.format);
    cache_cfg(s, track, data, size, ev.sample_format, 1, 90000);
    if (!should_emit_media(s, track)) {
        return MS_OK;
    }
    return evq_enqueue(s, &ev, track->language, data, size);
}

static ms_status cb_au(void *ctx, const ms_track_info *track, const ms_au_frame *fr)
{
    ms_session *s = (ms_session *)ctx;
    ms_event ev;
    memset(&ev, 0, sizeof(ev));
    /* 字幕轨（目前 TS 只上送 DVB 位图）走 SUBTITLE 事件，播放器按字幕缓存处理。 */
    ev.type = (track->kind == MS_TRACK_SUBTITLE) ? MS_EVENT_SUBTITLE : MS_EVENT_AU;
    ev.format = MS_FORMAT_MPEG_TS;
    ev.ts_packet_size = s->ts.packet_size;
    fill_event_track(&ev, track);
    ev.has_pts = fr->has_pts;
    ev.has_dts = fr->has_dts;
    ev.pts = fr->pts;
    ev.dts = fr->dts;
    ev.time_base_num = (fr->time_base_den > 0) ? fr->time_base_num : 1;
    ev.time_base_den = (fr->time_base_den > 0) ? fr->time_base_den : 90000;
    ev.is_keyframe = fr->is_keyframe;
    ev.format_changed = fr->format_changed;
    ev.discontinuity = fr->discontinuity;
    ev.preroll = 0;
    ev.sample_format = (fr->sample_format != MS_SAMPLE_FMT_UNKNOWN)
                           ? fr->sample_format
                           : default_sample_fmt(track->codec, ev.format);
    if (!should_emit_media(s, track)) {
        return MS_OK;
    }
    return evq_enqueue_owned(s, &ev, track->language, fr->data, fr->size,
                             fr->owned_base, fr->owned_cap, fr->owned_taken);
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

static ms_status cb_bmff_format(void *ctx, ms_format format,
                                uint32_t major_brand, uint32_t minor_version)
{
    ms_session *s = (ms_session *)ctx;
    ms_event ev;
    (void)major_brand;
    (void)minor_version;
    memset(&ev, 0, sizeof(ev));
    ev.type = MS_EVENT_FORMAT;
    ev.format = format;
    ev.ts_packet_size = 0;
    return evq_enqueue(s, &ev, NULL, NULL, 0);
}

static ms_status cb_bmff_track(void *ctx, const ms_track_info *track, int update)
{
    ms_session *s = (ms_session *)ctx;
    ms_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = update ? MS_EVENT_TRACK_UPDATE : MS_EVENT_TRACK;
    ev.format = session_format(s);
    ev.ts_packet_size = 0;
    ev.track = *track;
    ev.track.language = NULL;
    ev.sample_format = default_sample_fmt(track->codec, ev.format);
    {
        int ix = catalog_upsert(s, track);
        if (ix >= 0) {
            ev.track.selected = s->tracks[ix].selected;
            ev.track.is_default = s->tracks[ix].is_default;
        }
    }
    return evq_enqueue(s, &ev, track->language, NULL, 0);
}

static ms_status cb_bmff_config(void *ctx, const ms_track_info *track,
                                const uint8_t *data, size_t size,
                                ms_sample_format sample_fmt,
                                int time_base_num, int time_base_den)
{
    ms_session *s = (ms_session *)ctx;
    ms_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = MS_EVENT_CODEC_CONFIG;
    ev.format = session_format(s);
    ev.ts_packet_size = 0;
    ev.track = *track;
    ev.track.language = NULL;
    ev.time_base_num = time_base_num;
    ev.time_base_den = time_base_den;
    ev.sample_format = sample_fmt;
    cache_cfg(s, track, data, size, sample_fmt, time_base_num, time_base_den);
    if (!should_emit_media(s, track)) {
        return MS_OK;
    }
    return evq_enqueue(s, &ev, track->language, data, size);
}

static ms_status cb_bmff_au(void *ctx, const ms_track_info *track,
                            const ms_au_frame *fr)
{
    ms_session *s = (ms_session *)ctx;
    /* 复用 cb_au，但先把 format 从 BMFF 填入：临时改 demux 已是 2。 */
    ms_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = MS_EVENT_AU;
    ev.format = session_format(s);
    ev.ts_packet_size = 0;
    ev.track = *track;
    ev.track.language = NULL;
    ev.has_pts = fr->has_pts;
    ev.has_dts = fr->has_dts;
    ev.pts = fr->pts;
    ev.dts = fr->dts;
    ev.time_base_num = (fr->time_base_den > 0) ? fr->time_base_num : 1;
    ev.time_base_den = (fr->time_base_den > 0) ? fr->time_base_den : 1;
    ev.is_keyframe = fr->is_keyframe;
    ev.format_changed = fr->format_changed;
    ev.discontinuity = fr->discontinuity;
    ev.preroll = fr->preroll;
    ev.sample_format = fr->sample_format;
    if (!should_emit_media(s, track)) {
        return MS_OK;
    }
    return evq_enqueue_owned(s, &ev, track->language, fr->data, fr->size,
                             fr->owned_base, fr->owned_cap, fr->owned_taken);
}

static ms_status maybe_bmff_unsup(ms_session *s)
{
    ms_event ev;
    ms_status st;
    if (s->demux != 2 || s->bmff == NULL || s->bmff_unsup_queued) {
        return MS_OK;
    }
    if (!ms_bmff_parser_need_unsup(s->bmff)) {
        return MS_OK;
    }
    memset(&ev, 0, sizeof(ev));
    ev.type = MS_EVENT_UNSUPPORTED;
    ev.format = session_format(s);
    st = evq_enqueue(s, &ev, NULL, NULL, 0);
    if (st == MS_OK) {
        s->bmff_unsup_queued = 1;
    }
    return st;
}

static void bmff_free(ms_session *s)
{
    if (s->bmff != NULL) {
        ms_bmff_parser_destroy(s->bmff);
        ms_mem_free(&s->alloc, s->bmff);
        s->bmff = NULL;
    }
}

static void flv_free(ms_session *s)
{
    if (s->flv != NULL) {
        ms_flv_parser_destroy(s->flv);
        ms_mem_free(&s->alloc, s->flv);
        s->flv = NULL;
    }
}


static ms_status bmff_ensure(ms_session *s)
{
    ms_bmff_callbacks cb;
    if (s->bmff != NULL) {
        return MS_OK;
    }
    s->bmff = (ms_bmff_parser *)ms_mem_alloc(&s->alloc, sizeof(*s->bmff));
    if (s->bmff == NULL) {
        return ms_acct_is_budget_limit(&s->alloc) ? MS_LIMIT : MS_ERR_NOMEM;
    }
    memset(&cb, 0, sizeof(cb));
    cb.ctx = s;
    cb.on_format = cb_bmff_format;
    cb.on_box = NULL;
    cb.on_track = cb_bmff_track;
    cb.on_codec_config = cb_bmff_config;
    cb.on_au = cb_bmff_au;
    ms_bmff_parser_init(s->bmff, &s->alloc, &cb);
    ms_bmff_parser_set_meter(s->bmff, &s->meter);
    ms_bmff_parser_set_limits(s->bmff, s->limits.max_assembly_bytes,
                               s->limits.max_lookback_bytes);
    if (s->has_src) {
        ms_bmff_parser_set_random(s->bmff, 1);
    }
    return MS_OK;
}

static ms_status cb_flv_format(void *ctx, ms_format format)
{
    ms_session *s = (ms_session *)ctx;
    ms_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = MS_EVENT_FORMAT;
    ev.format = format;
    ev.ts_packet_size = 0;
    return evq_enqueue(s, &ev, NULL, NULL, 0);
}

static ms_status maybe_flv_unsup(ms_session *s)
{
    ms_event ev;
    ms_status st;
    if (s->demux != 3 || s->flv == NULL || s->flv_unsup_queued) {
        return MS_OK;
    }
    if (!ms_flv_parser_need_unsup(s->flv)) {
        return MS_OK;
    }
    memset(&ev, 0, sizeof(ev));
    ev.type = MS_EVENT_UNSUPPORTED;
    ev.format = MS_FORMAT_FLV;
    st = evq_enqueue(s, &ev, NULL, NULL, 0);
    if (st == MS_OK) {
        s->flv_unsup_queued = 1;
    }
    return st;
}

static ms_status flv_ensure(ms_session *s)
{
    ms_flv_callbacks cb;
    if (s->flv != NULL) {
        return MS_OK;
    }
    s->flv = (ms_flv_parser *)ms_mem_alloc(&s->alloc, ms_flv_parser_size());
    if (s->flv == NULL) {
        return ms_acct_is_budget_limit(&s->alloc) ? MS_LIMIT : MS_ERR_NOMEM;
    }
    memset(&cb, 0, sizeof(cb));
    cb.ctx = s;
    cb.on_format = cb_flv_format;
    cb.on_track = cb_bmff_track;
    cb.on_codec_config = cb_bmff_config;
    cb.on_au = cb_bmff_au;
    ms_flv_parser_init(s->flv, &s->alloc, &cb);
    ms_flv_parser_set_meter(s->flv, &s->meter);
    ms_flv_parser_set_limits(s->flv, s->limits.max_assembly_bytes);
    return MS_OK;
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
    ms_ts_parser_set_meter(&s->ts, &s->meter);
    s->ts.tolerant = s->ts_tolerant;
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
    if (limits_has_field(sz, offsetof(ms_session_limits, max_lookback_bytes),
                         sizeof(req->max_lookback_bytes))) {
        if (apply_one_limit(req->max_lookback_bytes, d.max_lookback_bytes,
                            abs.max_lookback_bytes,
                            &out->max_lookback_bytes) != MS_OK) {
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
    catalog_clear(session);
    ms_ts_parser_destroy(&session->ts);
    bmff_free(session);
    flv_free(session);
    in_buf_release(session);
    if (session->src_buf != NULL) {
        ms_mem_free(&session->alloc, session->src_buf);
        session->src_buf = NULL;
        session->src_buf_cap = 0;
    }
    ms_mem_free(&a, session->slots);
    ms_mem_free(&a, session);
}

static void inbuf_consume(ms_session *s, size_t used)
{
    if (used > s->in_len) {
        used = s->in_len;
    }
    if (used == 0) {
        return;
    }
    if (used < s->in_len) {
        memmove(s->in_buf, s->in_buf + used, s->in_len - used);
        s->in_len -= used;
    } else {
        s->in_len = 0;
    }
}

static ms_status finish_parse_status(ms_session *s, ms_status st)
{
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

static ms_status process_inbuf(ms_session *s, int eos)
{
    size_t used = 0;
    ms_status st;

    if (s->in_len == 0 && !eos) {
        return MS_OK;
    }

    if (s->demux == 0 && s->in_len > 0) {
        int flv = ms_flv_probe(s->in_buf, s->in_len);
        int pr;
        if (flv == 1) {
            st = flv_ensure(s);
            if (st != MS_OK) {
                return st;
            }
            s->demux = 3;
        } else {
            pr = ms_bmff_probe(s->in_buf, s->in_len);
            if ((pr < 0 || flv < 0) && !eos) {
                /* 可能是未凑齐的 FLV/BMFF 头，等更多字节。 */
                return MS_OK;
            }
            if (pr == 1) {
                st = bmff_ensure(s);
                if (st != MS_OK) {
                    return st;
                }
                s->demux = 2;
            }
        }
    }

    if (s->demux == 3) {
        used = 0;
        if (s->flv == NULL) {
            return MS_ERR_STATE;
        }
        st = ms_flv_parser_push(s->flv, s->in_buf, s->in_len, &used, eos);
        inbuf_consume(s, used);
        if (st == MS_OK || st == MS_AGAIN) {
            ms_status u = maybe_flv_unsup(s);
            if (u != MS_OK) {
                return finish_parse_status(s, u);
            }
        }
        return finish_parse_status(s, st);
    }

    if (s->demux == 2) {
        if (s->bmff == NULL) {
            return MS_ERR_STATE;
        }
        if (s->in_len == 0 && !eos) {
            return MS_OK;
        }
        st = ms_bmff_parser_push(s->bmff, s->in_buf, s->in_len, &used, eos);
        inbuf_consume(s, used);
        if (st == MS_OK || st == MS_AGAIN || st == MS_ERR_NOT_SEEKABLE) {
            ms_status u = maybe_bmff_unsup(s);
            if (u != MS_OK) {
                return finish_parse_status(s, u);
            }
        }
        return finish_parse_status(s, st);
    }

    st = ms_ts_parser_push(&s->ts, s->in_buf, s->in_len, &used, eos);
    inbuf_consume(s, used);
    if (s->ts.confirmed) {
        s->demux = 1;
    }
    return finish_parse_status(s, st);
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
    if (session->in_source_cb) {
        return MS_ERR_STATE;
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

    /* FLV：当前 tag 由解析器持有，后续字节直接喂给解析器。 */
    if (session->demux == 3 && session->flv != NULL && size > 0) {
        size_t used = 0;
        session->bytes_in += size;
        st = ms_flv_parser_push(session->flv, data, size, &used, 0);
        if (used < size) {
            session->bytes_in -= (size - used);
        }
        *consumed = used;
        if (st != MS_OK && st != MS_AGAIN && st != MS_LIMIT) {
            return finish_parse_status(session, st);
        }
        if (st == MS_OK || st == MS_AGAIN) {
            ms_status u = maybe_flv_unsup(session);
            if (u != MS_OK && u != MS_LIMIT) {
                return finish_parse_status(session, u);
            }
            if (u == MS_LIMIT) {
                st = MS_LIMIT;
            }
        }
        if (used >= size) {
            return (st == MS_AGAIN) ? MS_OK : st;
        }
        data += used;
        size -= used;
    }

    /* BMFF：mdat 等大载荷直接交给解析器跳过，不进入 in_buf。 */
    if (session->demux == 2 && session->bmff != NULL && size > 0) {
        size_t used = 0;
        session->bytes_in += size;
        st = ms_bmff_parser_push(session->bmff, data, size, &used, 0);
        if (used < size) {
            session->bytes_in -= (size - used);
        }
        *consumed = used;
        if (st != MS_OK && st != MS_AGAIN && st != MS_LIMIT &&
            st != MS_ERR_NOT_SEEKABLE) {
            return finish_parse_status(session, st);
        }
        if (st == MS_OK || st == MS_AGAIN || st == MS_ERR_NOT_SEEKABLE) {
            ms_status u = maybe_bmff_unsup(session);
            if (u != MS_OK && u != MS_LIMIT) {
                return finish_parse_status(session, u);
            }
            if (u == MS_LIMIT) {
                st = MS_LIMIT;
            }
        }
        if (st == MS_ERR_NOT_SEEKABLE) {
            return finish_parse_status(session, st);
        }
        if (used >= size) {
            return (st == MS_AGAIN) ? MS_OK : st;
        }
        data += used;
        size -= used;
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
            ms_copy_meter_add(&session->meter, MS_COPY_IN, copied);
            session->in_len = need;
            session->bytes_in += copied;
        }
    }

    st = process_inbuf(session, 0);
    *consumed += copied;
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
    if (session->in_source_cb) {
        return MS_ERR_STATE;
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
        /* 与 FLV/BMFF 相同：队列腾出后继续消化 in_buf，避免 EOS 后完整 AU 卡住。 */
        if (session->demux == 0 || session->demux == 1) {
            drain_st = process_inbuf(session, session->eos_in);
            if (drain_st == MS_ERR_UNKNOWN || drain_st == MS_ERR_MALFORMED ||
                drain_st == MS_ERR_OVERFLOW || drain_st == MS_ERR_NOMEM ||
                drain_st == MS_ERR_UNSUPPORTED) {
                fail_session(session, drain_st);
                return drain_st;
            }
        }
    }
    if (session->demux == 2 && session->bmff != NULL) {
        ms_status drain_st = ms_bmff_parser_drain(session->bmff);
        if (drain_st == MS_ERR_UNKNOWN || drain_st == MS_ERR_MALFORMED ||
            drain_st == MS_ERR_OVERFLOW || drain_st == MS_ERR_NOMEM ||
            drain_st == MS_ERR_UNSUPPORTED) {
            fail_session(session, drain_st);
            return drain_st;
        }
        if (drain_st == MS_ERR_NOT_SEEKABLE) {
            /* 回看失败：事件仍可取出，不把会话标失败。 */
            drain_st = maybe_bmff_unsup(session);
            if (drain_st == MS_ERR_UNKNOWN || drain_st == MS_ERR_MALFORMED ||
                drain_st == MS_ERR_OVERFLOW || drain_st == MS_ERR_NOMEM ||
                drain_st == MS_ERR_UNSUPPORTED) {
                fail_session(session, drain_st);
                return drain_st;
            }
            drain_st = MS_OK;
        }
        if (drain_st == MS_OK) {
            drain_st = maybe_bmff_unsup(session);
            if (drain_st == MS_ERR_UNKNOWN || drain_st == MS_ERR_MALFORMED ||
                drain_st == MS_ERR_OVERFLOW || drain_st == MS_ERR_NOMEM ||
                drain_st == MS_ERR_UNSUPPORTED) {
                fail_session(session, drain_st);
                return drain_st;
            }
        }
        /* 同 FLV：腾出队列后再消化 pending / in_buf，避免 EOS 后完整 AU 卡住。 */
        drain_st = process_inbuf(session, session->eos_in);
        if (drain_st == MS_ERR_UNKNOWN || drain_st == MS_ERR_MALFORMED ||
            drain_st == MS_ERR_OVERFLOW || drain_st == MS_ERR_NOMEM ||
            drain_st == MS_ERR_UNSUPPORTED) {
            fail_session(session, drain_st);
            return drain_st;
        }
    }
    if (session->demux == 3 && session->flv != NULL) {
        ms_status drain_st = ms_flv_parser_drain(session->flv);
        if (drain_st == MS_ERR_UNKNOWN || drain_st == MS_ERR_MALFORMED ||
            drain_st == MS_ERR_OVERFLOW || drain_st == MS_ERR_NOMEM ||
            drain_st == MS_ERR_UNSUPPORTED) {
            fail_session(session, drain_st);
            return drain_st;
        }
        /* 队列腾出空间后继续消化 in_buf 残留，避免 EOS 后待入队 AU 永远卡住。 */
        drain_st = process_inbuf(session, session->eos_in);
        if (drain_st == MS_ERR_UNKNOWN || drain_st == MS_ERR_MALFORMED ||
            drain_st == MS_ERR_OVERFLOW || drain_st == MS_ERR_NOMEM ||
            drain_st == MS_ERR_UNSUPPORTED) {
            fail_session(session, drain_st);
            return drain_st;
        }
        if (drain_st == MS_OK || drain_st == MS_AGAIN) {
            drain_st = maybe_flv_unsup(session);
            if (drain_st == MS_ERR_UNKNOWN || drain_st == MS_ERR_MALFORMED ||
                drain_st == MS_ERR_OVERFLOW || drain_st == MS_ERR_NOMEM ||
                drain_st == MS_ERR_UNSUPPORTED) {
                fail_session(session, drain_st);
                return drain_st;
            }
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
            ev.format = session_format(session);
            ev.ts_packet_size = (session->demux == 1) ? session->ts.packet_size : 0;
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
    if (session->in_source_cb) {
        return MS_ERR_STATE;
    }
    if (session->canceled) {
        return MS_ERR_CANCELED;
    }
    evq_clear_all(session);
    debug_pes_clear(session);
    catalog_clear_inband(session);
    in_buf_release(session);
    ms_ts_parser_flush_partial(&session->ts);
    if (session->bmff != NULL) {
        ms_bmff_flush_partial(session->bmff);
    }
    if (session->flv != NULL) {
        ms_flv_flush_partial(session->flv);
    }
    session->eos_queued = 0;
    session->bmff_unsup_queued = 0;
    session->flv_unsup_queued = 0;
    return MS_OK;
}

ms_status ms_session_reset(ms_session *session)
{
    if (session == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    if (session->in_source_cb) {
        return MS_ERR_STATE;
    }
    evq_clear_all(session);
    debug_pes_clear(session);
    catalog_clear(session);
    session->selection_epoch = 1;
    in_buf_release(session);
    ms_ts_parser_reset(&session->ts);
    ts_bind(session);
    bmff_free(session);
    flv_free(session);
    session->demux = 0;
    session->bmff_unsup_queued = 0;
    session->flv_unsup_queued = 0;
    session->canceled = 0;
    session->failed = 0;
    session->fail_status = MS_OK;
    session->eos_in = 0;
    session->eos_queued = 0;
    session->force_copy = 0;
    session->src_pos = 0;
    session->src_eos = 0;
    session->has_seek_land = 0;
    session->seek_request_90k = 0;
    session->seek_land_90k = 0;
    if (session->src_buf != NULL) {
        ms_mem_free(&session->alloc, session->src_buf);
        session->src_buf = NULL;
        session->src_buf_cap = 0;
    }
    stats_clear(session);
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
    if (session->in_source_cb) {
        return MS_ERR_STATE;
    }
    session->canceled = 1;
    if (session->has_src && session->src.cancel_fn != NULL) {
        session->in_source_cb = 1;
        session->src.cancel_fn(session->src.ctx);
        session->in_source_cb = 0;
    }
    return MS_OK;
}

ms_status ms_session_signal_discontinuity(ms_session *session)
{
    ms_event ev;
    ms_status st;
    uint64_t epoch;

    if (session == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    if (session->in_source_cb) {
        return MS_ERR_STATE;
    }
    if (session->canceled) {
        return MS_ERR_CANCELED;
    }
    if (session->failed) {
        return session->fail_status;
    }
    debug_pes_clear(session);
    in_buf_release(session);
    ms_ts_parser_flush_partial(&session->ts);
    if (session->bmff != NULL) {
        ms_bmff_flush_partial(session->bmff);
    }
    if (session->flv != NULL) {
        ms_flv_flush_partial(session->flv);
    }
    session->eos_in = 0;
    session->eos_queued = 0;
    session->src_eos = 0;
    epoch = session->timeline_epoch + 1;
    if (epoch == 0) {
        epoch = 1;
    }
    session->timeline_epoch = epoch;
    memset(&ev, 0, sizeof(ev));
    ev.type = MS_EVENT_DISCONTINUITY;
    ev.format = session_format(session);
    ev.discontinuity = 1;
    st = evq_enqueue(session, &ev, NULL, NULL, 0);
    /* evq 对无载荷 DISCONTINUITY 只返回 OK/LIMIT。AGAIN 视为已接受。
     * 不用 {OK,LIMIT,AGAIN} 成员测试再三元映射：gcc 14.2 -O2 会把该形态
     * 降成 bt+cmovae，在 st==OK 时误返回 MS_ERR_NOMEM（掩码 0x9 与
     * NOMEM 同值且条件被颠倒）。见 tests/repro_hls_miss.c 与
     * tests/repro_gcc_vrp_status.c。 */
    if (st == MS_AGAIN) {
        return MS_OK;
    }
    return st;
}

ms_status ms_session_end_of_input(ms_session *session)
{
    ms_status st;
    if (session == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    if (session->in_source_cb) {
        return MS_ERR_STATE;
    }
    if (session->canceled) {
        return MS_ERR_CANCELED;
    }
    if (session->failed) {
        return session->fail_status;
    }
    session->eos_in = 1;
    st = MS_OK;
    /* fMP4 样本常停在解析器 pending 里而非 in_buf；只冲 in_buf 会把
     * 已完整 AU 留在 demux，却按成功结束输入并随后发出 EOS。 */
    if (session->demux == 2 && session->bmff != NULL) {
        st = ms_bmff_parser_drain(session->bmff);
        if (st != MS_OK && st != MS_LIMIT && st != MS_AGAIN &&
            st != MS_ERR_NOT_SEEKABLE) {
            return finish_parse_status(session, st);
        }
    }
    {
        ms_status pst = process_inbuf(session, 1);
        if (pst != MS_OK && pst != MS_LIMIT && pst != MS_AGAIN &&
            pst != MS_ERR_NOT_SEEKABLE) {
            return pst;
        }
        if (st != MS_LIMIT && st != MS_ERR_NOT_SEEKABLE) {
            st = pst;
        } else if (pst == MS_LIMIT) {
            st = MS_LIMIT;
        }
    }
    if (session->demux == 3 && session->flv != NULL) {
        ms_status u = maybe_flv_unsup(session);
        if (u != MS_OK && u != MS_LIMIT && u != MS_AGAIN) {
            return finish_parse_status(session, u);
        }
        if (u == MS_LIMIT && st == MS_OK) {
            st = MS_LIMIT;
        }
    }
    /* LIMIT 时 in_buf 里还有未解析字节，清掉会丢后续 tag/AU。 */
    if (st != MS_LIMIT) {
        in_buf_clear(session);
    }
    if (st == MS_ERR_NOT_SEEKABLE ||
        (session->demux == 2 && session->bmff != NULL &&
         ms_bmff_parser_need_seek(session->bmff))) {
        st = maybe_bmff_unsup(session);
        if (st != MS_OK && st != MS_LIMIT && st != MS_AGAIN) {
            return finish_parse_status(session, st);
        }
        return MS_ERR_NOT_SEEKABLE;
    }
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
    ms_status st;
    int64_t land = 0;
    uint64_t epoch;

    if (session == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    if (session->in_source_cb) {
        return MS_ERR_STATE;
    }
    if (session->canceled) {
        return MS_ERR_CANCELED;
    }
    if (session->failed) {
        return session->fail_status;
    }
    if (time_90k < 0) {
        return MS_ERR_INVALID_ARG;
    }
    if (!session->has_src) {
        return MS_ERR_NOT_SEEKABLE;
    }
    if (session->demux == 0) {
        return MS_AGAIN;
    }
    if (session->demux != 2 || session->bmff == NULL) {
        return MS_ERR_NOT_SEEKABLE;
    }
    st = ms_bmff_parser_seek_time(session->bmff, time_90k, &land);
    if (st != MS_OK) {
        return st;
    }
    evq_clear_all(session);
    debug_pes_clear(session);
    catalog_clear_inband(session);
    in_buf_release(session);
    session->eos_in = 0;
    session->eos_queued = 0;
    session->src_eos = 0;
    session->bmff_unsup_queued = 0;
    epoch = session->timeline_epoch + 1;
    if (epoch == 0) {
        epoch = 1;
    }
    session->timeline_epoch = epoch;
    session->has_seek_land = 1;
    session->seek_request_90k = time_90k;
    session->seek_land_90k = land;
    return MS_OK;
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
    int i, j, changed = 0;
    int tgt;
    ms_track_kind kind;
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
    tgt = catalog_find(session, track_id);
    if (tgt < 0) {
        return MS_ERR_INVALID_ARG;
    }
    kind = session->tracks[tgt].kind;
    if (on && kind == MS_TRACK_SUBTITLE && !sub_playable(session->tracks[tgt].codec) &&
        track_id != MS_TRACK_ID_EXTERNAL_SUB) {
        return MS_ERR_UNSUPPORTED;
    }
    if ((on && session->tracks[tgt].selected) || (!on && !session->tracks[tgt].selected)) {
        return MS_OK;
    }
    session->selection_epoch++;
    if (session->selection_epoch == 0) {
        session->selection_epoch = 1;
    }
    if (kind == MS_TRACK_AUDIO) {
        session->user_audio_sel = 1;
    }
    if (kind == MS_TRACK_SUBTITLE) {
        session->user_sub_sel = 1;
    }
    for (j = 0; j < session->ntracks; j++) {
        int want = 0;
        if (!session->tracks[j].in_use || session->tracks[j].kind != kind) {
            continue;
        }
        want = on && session->tracks[j].id == track_id;
        if (session->tracks[j].selected != want) {
            evq_drop_track_media(session, session->tracks[j].id,
                                 kind == MS_TRACK_SUBTITLE);
            session->tracks[j].selected = want;
            changed = 1;
            st = enqueue_track_update(session, j);
            if (st != MS_OK) {
                return st;
            }
        }
    }
    if (on && (kind == MS_TRACK_AUDIO || kind == MS_TRACK_VIDEO)) {
        st = enqueue_cached_cfg(session, tgt);
        if (st != MS_OK) {
            return st;
        }
    }
    if (kind == MS_TRACK_SUBTITLE && !on) {
        catalog_clear_inband(session);
    }
    (void)i;
    (void)changed;
    return MS_OK;
}

ms_status ms_session_list_tracks(const ms_session *session,
                                 ms_track_info *out, size_t cap, size_t *count)
{
    size_t n = 0;
    int i;
    if (session == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    if (session->tracks != NULL) {
    for (i = 0; i < session->ntracks; i++) {
        if (!session->tracks[i].in_use) {
            continue;
        }
        if (out != NULL && n < cap) {
            catalog_fill_info(session, i, &out[n]);
        }
        n++;
    }
    }
    if (count != NULL) {
        *count = n;
    }
    (void)cap;
    return MS_OK;
}

ms_status ms_session_set_external_subtitle(ms_session *session,
                                           const uint8_t *data, size_t size,
                                           ms_sub_format fmt)
{
    ms_status st;
    int i;
    ms_track_info ti;
    if (session == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    if (session->canceled) {
        return MS_ERR_CANCELED;
    }
    if (data == NULL || size == 0) {
        i = catalog_find(session, MS_TRACK_ID_EXTERNAL_SUB);
        if (i >= 0) {
            evq_drop_track_media(session, MS_TRACK_ID_EXTERNAL_SUB, 1);
            if (session->tracks[i].selected) {
                session->selection_epoch++;
                if (session->selection_epoch == 0) {
                    session->selection_epoch = 1;
                }
            }
            memset(&session->tracks[i], 0, sizeof(session->tracks[i]));
        }
        if (session->sub_ext != NULL) {
            ms_sub_doc_destroy(session->sub_ext);
            session->sub_ext = NULL;
        }
        session->ext_sub_on = 0;
        return MS_OK;
    }
    if (session->sub_ext == NULL) {
        st = ms_sub_doc_create(&session->alloc, &session->sub_ext);
        if (st != MS_OK) {
            return st;
        }
    }
    st = ms_sub_doc_parse(session->sub_ext, data, size, fmt, MS_TRACK_ID_EXTERNAL_SUB);
    if (st != MS_OK) {
        return st;
    }
    memset(&ti, 0, sizeof(ti));
    ti.id = MS_TRACK_ID_EXTERNAL_SUB;
    ti.kind = MS_TRACK_SUBTITLE;
    ti.codec = ms_sub_codec_for_format(fmt == MS_SUB_FORMAT_AUTO
                                           ? ms_sub_doc_format(session->sub_ext)
                                           : fmt);
    ti.level = MS_RECOG_AU;
    ti.name = "external";
    ti.selected = 0;
    i = catalog_upsert(session, &ti);
    if (i < 0) {
        return MS_ERR_OVERFLOW;
    }
    session->tracks[i].codec = ti.codec;
    session->tracks[i].level = MS_RECOG_AU;
    copy_lang(session->tracks[i].name, sizeof(session->tracks[i].name), "external");
    session->ext_sub_on = 1;
    return enqueue_track_update(session, i);
}

ms_status ms_session_subtitles_at(const ms_session *session, uint64_t time_90k,
                                  ms_sub_cue *out, size_t cap, size_t *count)
{
    size_t n = 0, add = 0;
    int i, sel_ext = 0;
    ms_status st;
    if (session == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    i = catalog_find(session, MS_TRACK_ID_EXTERNAL_SUB);
    sel_ext = (i >= 0 && session->tracks[i].selected);
    if (sel_ext && session->sub_ext != NULL) {
        st = ms_sub_doc_cues_at(session->sub_ext, time_90k, out, cap, &add);
        if (st != MS_OK) {
            return st;
        }
        n += add;
    }
    i = -1;
    for (i = 0; i < session->ntracks; i++) {
        if (session->tracks[i].in_use && session->tracks[i].kind == MS_TRACK_SUBTITLE &&
            session->tracks[i].selected && session->tracks[i].id != MS_TRACK_ID_EXTERNAL_SUB) {
            break;
        }
    }
    if (i >= 0 && i < session->ntracks && session->sub_inband != NULL) {
        ms_sub_cue *dst = out;
        size_t left = cap;
        if (out != NULL && n < cap) {
            dst = out + n;
            left = cap - n;
        } else {
            dst = NULL;
            left = 0;
        }
        add = 0;
        st = ms_sub_doc_cues_at(session->sub_inband, time_90k, dst, left, &add);
        if (st != MS_OK) {
            return st;
        }
        n += add;
    }
    if (count != NULL) {
        *count = n;
    }
    return MS_OK;
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

ms_status ms_session_debug_bmff(const ms_session *session, ms_bmff_debug_info *out)
{
    if (session == NULL || out == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    if (session->demux != 2 || session->bmff == NULL) {
        return MS_AGAIN;
    }
    out->format = ms_bmff_parser_format(session->bmff);
    out->major_brand = ms_bmff_parser_major_brand(session->bmff);
    out->nboxes = ms_bmff_parser_box_count(session->bmff);
    out->max_depth = ms_bmff_parser_max_depth(session->bmff);
    out->saw_moov = ms_bmff_parser_saw_moov(session->bmff);
    out->saw_mdat = ms_bmff_parser_saw_mdat(session->bmff);
    out->saw_moof = ms_bmff_parser_saw_moof(session->bmff);
    out->saw_mvex = ms_bmff_parser_saw_mvex(session->bmff);
    out->truncated_mdat = ms_bmff_parser_truncated_mdat(session->bmff);
    out->bytes_skipped = ms_bmff_parser_bytes_skipped(session->bmff);
    return MS_OK;
}

void ms_session_debug_set_force_copy(ms_session *session, int on)
{
    if (session != NULL) {
        session->force_copy = on ? 1 : 0;
    }
}

ms_status ms_session_get_stats(const ms_session *session, ms_session_stats *out)
{
    size_t n;
    ms_session_stats tmp;

    if (session == NULL || out == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    if (out->struct_size < sizeof(out->struct_size)) {
        return MS_ERR_INVALID_ARG;
    }
    memset(&tmp, 0, sizeof(tmp));
    tmp.struct_size = out->struct_size;
    tmp.bytes_in = session->bytes_in;
    tmp.copy_in_ops = session->meter.copy_in_ops;
    tmp.copy_in_bytes = session->meter.copy_in_bytes;
    tmp.copy_asm_ops = session->meter.copy_asm_ops;
    tmp.copy_asm_bytes = session->meter.copy_asm_bytes;
    tmp.copy_out_ops = session->meter.copy_out_ops;
    tmp.copy_out_bytes = session->meter.copy_out_bytes;
    tmp.move_out_ops = session->meter.move_out_ops;
    tmp.move_out_bytes = session->meter.move_out_bytes;
    tmp.baseline_out_bytes = session->meter.copy_out_bytes +
                             session->meter.move_out_bytes;
    tmp.has_format = session->has_format;
    tmp.has_track = session->has_track;
    tmp.has_config = session->has_config;
    tmp.has_key_au = session->has_key_au;
    tmp.has_video_key_au = session->has_video_key_au;
    tmp.bytes_at_format = session->bytes_at_format;
    tmp.bytes_at_track = session->bytes_at_track;
    tmp.bytes_at_config = session->bytes_at_config;
    tmp.bytes_at_key_au = session->bytes_at_key_au;
    tmp.bytes_at_video_key_au = session->bytes_at_video_key_au;
    tmp.has_seek_land = session->has_seek_land;
    tmp.seek_request_90k = session->seek_request_90k;
    tmp.seek_land_90k = session->seek_land_90k;
    tmp.ts_prefix_bytes = session->ts.prefix_bytes;
    tmp.ts_prefix_kind = (int)session->ts.prefix_kind;
    n = out->struct_size;
    if (n > sizeof(tmp)) {
        n = sizeof(tmp);
    }
    memcpy(out, &tmp, n);
    out->struct_size = tmp.struct_size;
    return MS_OK;
}

static ms_status src_buf_ensure(ms_session *s, size_t need)
{
    uint8_t *q;
    if (need == 0) {
        return MS_OK;
    }
    if (s->src_buf != NULL && s->src_buf_cap >= need) {
        return MS_OK;
    }
    q = (uint8_t *)ms_mem_alloc(&s->alloc, need);
    if (q == NULL) {
        return ms_acct_is_budget_limit(&s->alloc) ? MS_LIMIT : MS_ERR_NOMEM;
    }
    ms_mem_free(&s->alloc, s->src_buf);
    s->src_buf = q;
    s->src_buf_cap = need;
    return MS_OK;
}

ms_status ms_session_set_source(ms_session *session, const ms_source *src)
{
    if (session == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    if (session->in_source_cb) {
        return MS_ERR_STATE;
    }
    if (session->canceled) {
        return MS_ERR_CANCELED;
    }
    if (src == NULL) {
        memset(&session->src, 0, sizeof(session->src));
        session->has_src = 0;
        if (session->bmff != NULL) {
            ms_bmff_parser_set_random(session->bmff, 0);
        }
        return MS_OK;
    }
    if (src->struct_size < sizeof(size_t) || src->read_at_fn == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    memset(&session->src, 0, sizeof(session->src));
    session->src.struct_size = sizeof(session->src);
    session->src.ctx = src->ctx;
    session->src.size_fn = src->size_fn;
    session->src.read_at_fn = src->read_at_fn;
    if (src->struct_size >= sizeof(*src)) {
        session->src.cancel_fn = src->cancel_fn;
    }
    session->has_src = 1;
    if (session->bmff != NULL) {
        ms_bmff_parser_set_random(session->bmff, 1);
        session->src_pos = ms_bmff_parser_file_off(session->bmff);
    }
    return MS_OK;
}

static ms_status drive_query_size(ms_session *s, int *have, uint64_t *sz)
{
    ms_status st;
    *have = 0;
    *sz = 0;
    if (!s->has_src || s->src.size_fn == NULL) {
        return MS_ERR_NOT_SEEKABLE;
    }
    s->in_source_cb = 1;
    st = s->src.size_fn(s->src.ctx, sz);
    s->in_source_cb = 0;
    if (st == MS_OK) {
        *have = 1;
    }
    return st;
}

static ms_status drive_read(ms_session *s, uint64_t off, size_t len, size_t *out_n)
{
    ms_status st;
    *out_n = 0;
    if (len == 0) {
        return MS_OK;
    }
    st = src_buf_ensure(s, len);
    if (st != MS_OK) {
        return st;
    }
    s->in_source_cb = 1;
    st = s->src.read_at_fn(s->src.ctx, off, s->src_buf, len, out_n);
    s->in_source_cb = 0;
    if (st != MS_OK) {
        *out_n = 0;
    }
    return st;
}

ms_status ms_session_drive(ms_session *session)
{
    ms_status st;
    ms_status zst;
    int have_size = 0;
    uint64_t src_size = 0;
    ms_bmff_need need;
    size_t hint;
    size_t n = 0;
    size_t used = 0;

    if (session == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    if (session->in_source_cb) {
        return MS_ERR_STATE;
    }
    if (session->canceled) {
        return MS_ERR_CANCELED;
    }
    if (session->failed) {
        return session->fail_status;
    }
    if (!session->has_src) {
        return MS_ERR_STATE;
    }

    if (session->demux == 2 && session->bmff != NULL) {
        st = ms_bmff_parser_drain(session->bmff);
        if (st != MS_OK && st != MS_AGAIN && st != MS_LIMIT &&
            st != MS_ERR_NOT_SEEKABLE) {
            return finish_parse_status(session, st);
        }
        if (st == MS_LIMIT) {
            return MS_LIMIT;
        }
        if (st == MS_ERR_NOT_SEEKABLE) {
            st = maybe_bmff_unsup(session);
            if (st != MS_OK && st != MS_LIMIT && st != MS_AGAIN) {
                return finish_parse_status(session, st);
            }
        }
    }

    zst = drive_query_size(session, &have_size, &src_size);
    if (zst != MS_OK && zst != MS_AGAIN && zst != MS_ERR_NOT_SEEKABLE) {
        return zst;
    }
    have_size = (zst == MS_OK);
    if (!have_size) {
        src_size = 0;
    }

    hint = session->limits.max_push_bytes;
    if (hint == 0 || hint > 16384) {
        hint = 16384;
    }
    memset(&need, 0, sizeof(need));
    if (session->demux == 2 && session->bmff != NULL) {
        st = ms_bmff_parser_need(session->bmff, have_size, src_size, hint, &need);
        if (st != MS_OK) {
            return finish_parse_status(session, st);
        }
    } else {
        need.kind = MS_BMFF_NEED_SEQ;
        need.offset = session->src_pos;
        need.length = hint;
        if (have_size) {
            if (need.offset >= src_size) {
                need.kind = MS_BMFF_NEED_NONE;
                need.length = 0;
            } else if (src_size - need.offset < (uint64_t)need.length) {
                need.length = (size_t)(src_size - need.offset);
            }
        }
    }

    if (need.kind == MS_BMFF_NEED_NONE) {
        if (!session->eos_in) {
            st = ms_session_end_of_input(session);
            session->src_eos = 1;
            if (st != MS_OK && st != MS_AGAIN && st != MS_LIMIT &&
                st != MS_ERR_NOT_SEEKABLE) {
                return st;
            }
            if (st == MS_LIMIT) {
                return MS_LIMIT;
            }
        }
        if (evq_count(session) > 0) {
            return MS_OK;
        }
        return session->eos_in ? MS_EOS : MS_AGAIN;
    }

    if (need.kind == MS_BMFF_NEED_JUMP) {
        st = ms_bmff_parser_skip_to(session->bmff, need.offset);
        if (st != MS_OK) {
            return finish_parse_status(session, st);
        }
        session->src_pos = need.offset;
        return MS_OK;
    }

    if (need.length > session->limits.max_push_bytes &&
        session->limits.max_push_bytes > 0) {
        need.length = session->limits.max_push_bytes;
    }
    if (need.kind == MS_BMFF_NEED_SAMPLE && session->bmff != NULL) {
        size_t cap = session->limits.max_assembly_bytes;
        if (cap > 0 && need.length > cap) {
            /* 样本本身过大由 supply 报畸形；这里只限制单次读取。 */
            need.length = (cap < session->limits.max_push_bytes ||
                           session->limits.max_push_bytes == 0)
                              ? cap
                              : session->limits.max_push_bytes;
        }
    }

    st = drive_read(session, need.offset, need.length, &n);
    if (st == MS_AGAIN) {
        return MS_AGAIN;
    }
    if (st == MS_ERR_CANCELED) {
        session->canceled = 1;
        return MS_ERR_CANCELED;
    }
    if (st != MS_OK) {
        return st;
    }
    if (n == 0) {
        session->src_eos = 1;
        if (need.kind == MS_BMFF_NEED_SAMPLE) {
            /* 样本截断：交给 supply 以 0 字节收尾，不伪造完整 AU。 */
            st = ms_bmff_supply_sample(session->bmff, need.offset, session->src_buf,
                                       0, &used);
            if (st != MS_OK && st != MS_AGAIN && st != MS_LIMIT) {
                return finish_parse_status(session, st);
            }
        }
        if (!session->eos_in) {
            st = ms_session_end_of_input(session);
            if (st != MS_OK && st != MS_AGAIN && st != MS_LIMIT &&
                st != MS_ERR_NOT_SEEKABLE) {
                return st;
            }
        }
        if (evq_count(session) > 0) {
            return MS_OK;
        }
        return session->eos_in ? MS_EOS : MS_AGAIN;
    }

    session->src_pos = need.offset + (uint64_t)n;
    if (need.kind == MS_BMFF_NEED_SAMPLE) {
        st = ms_bmff_supply_sample(session->bmff, need.offset, session->src_buf, n,
                                   &used);
        if (used < n && session->src_pos >= (uint64_t)(n - used)) {
            session->src_pos -= (uint64_t)(n - used);
        }
        if (st != MS_OK && st != MS_AGAIN && st != MS_LIMIT) {
            return finish_parse_status(session, st);
        }
        return (st == MS_AGAIN) ? MS_OK : st;
    }

    /* 顺序块：复用 push。 */
    used = 0;
    st = ms_session_push(session, session->src_buf, n, &used);
    if (used < n && session->src_pos >= (uint64_t)(n - used)) {
        session->src_pos -= (uint64_t)(n - used);
    }
    if (st == MS_AGAIN) {
        return MS_OK;
    }
    return st;
}

void ms_event_trim_default(ms_event_trim *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->struct_size = sizeof(*out);
}

static ms_status copy_event_trim(size_t want, uint32_t skip_s, uint32_t skip_e,
                                 ms_event_trim *out)
{
    ms_event_trim tmp;
    size_t n;
    if (out == NULL || want < sizeof(out->struct_size)) {
        return MS_ERR_INVALID_ARG;
    }
    memset(&tmp, 0, sizeof(tmp));
    tmp.struct_size = want;
    tmp.skip_samples_start = skip_s;
    tmp.skip_samples_end = skip_e;
    n = want;
    if (n > sizeof(tmp)) {
        n = sizeof(tmp);
    }
    memcpy(out, &tmp, n);
    out->struct_size = want;
    return MS_OK;
}

ms_status ms_session_event_trim(const ms_session *session, uint32_t release_id,
                                ms_event_trim *out)
{
    uint32_t slot;
    uint32_t gen;
    int ev_i;
    size_t want;

    if (session == NULL || out == NULL || release_id == 0) {
        return MS_ERR_INVALID_ARG;
    }
    want = out->struct_size;
    if (want < sizeof(out->struct_size)) {
        return MS_ERR_INVALID_ARG;
    }
    slot = release_id & 0xFFFFu;
    gen = release_id >> 16;
    if (slot >= session->nslots) {
        return MS_ERR_INVALID_ARG;
    }
    if (!session->slots[slot].used || session->slots[slot].gen != gen) {
        return MS_ERR_STATE;
    }
    ev_i = session->slots[slot].ev_index;
    if (ev_i < 0 || ev_i >= MS_PENDING_EVENT_MAX || !session->evq[ev_i].used) {
        return MS_ERR_STATE;
    }
    return copy_event_trim(want, session->evq[ev_i].skip_samples_start,
                           session->evq[ev_i].skip_samples_end, out);
}

/* 内部：TS 容错开关（不导出，不进 C ABI v1）。坏包/坏帧丢弃并标不连续，不整路失败。 */
void ms_session_set_ts_tolerant(ms_session *session, int on)
{
    if (session == NULL) {
        return;
    }
    session->ts_tolerant = on ? 1 : 0;
    session->ts.tolerant = session->ts_tolerant;
}

uint64_t ms_session_ts_tolerated(const ms_session *session)
{
    return session != NULL ? session->ts.tolerated : 0;
}
