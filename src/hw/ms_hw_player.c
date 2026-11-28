#define _DEFAULT_SOURCE
/*
 * 压缩 AU 到硬解 Surface 的编排：CSD、时间戳、关键帧门闩、
 * 反压、取消、EOS、flush、切换释放、分项拷贝与分层计时。
 * 不把 OUTPUT_RENDERED / releaseOutputBuffer 当成实际显示。
 * by: Long <mycjzb@gmail.com>
 */
#include "msmedia/ms_hw.h"
#include "ms_hw_ass_bind.h"
#include "ms_hw_ai_bind.h"

#include "ms_hw_internal.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <limits.h>

#ifndef MS_HW_LANG_MAX
#define MS_HW_LANG_MAX 16
#endif
#ifndef MS_HW_NAME_MAX
#define MS_HW_NAME_MAX 48
#endif

typedef struct ms_hw_trk {
    int in_use;
    uint32_t id;
    ms_track_kind kind;
    ms_codec codec;
    ms_recog_level level;
    char language[MS_HW_LANG_MAX];
    char name[MS_HW_NAME_MAX];
    int is_default;
    int is_forced;
    int selected;
} ms_hw_trk;

struct ms_hw_player {
    unsigned pt_caps; /* 阶段 4：音频直通能力位（内部；0 = 关闭直通） */
    const ms_allocator *alloc;
    ms_allocator owned_alloc;
    ms_hw_player_opts opts;
    ms_hw_decoder *dec;
    void *surface;
    int have_csd;
    uint8_t *csd;
    size_t csd_size;
    /* flush 后需向仍存活的解码器再送一遍 CSD（API 18 OMX.google）。 */
    int csd_resubmit;
    ms_codec codec;
    ms_sample_format sample_fmt;
    int width;
    int height;
    int video_seen;
    int waiting_key;
    char force_decoder_name[128];
    int configured;
    int started;
    int eos_in;
    int eos_out;
    int canceled;
    int flushed;
    unsigned epoch;
    int64_t t_start_us;
    int64_t t_first_packet_us;
    int64_t t_first_decodable_au_us;
    int64_t t_first_submit_us;
    int64_t t_first_output_us;
    int64_t t_first_display_us;
    int64_t t_first_codec_rendered_us;
    int have_first_packet;
    int have_first_decodable_au;
    int have_first_submit;
    int have_first_output;
    int have_first_display;
    int have_first_codec_rendered;
    int64_t t_request_us;
    int have_request;
    int64_t t_req_first_packet_us;
    int64_t t_req_first_decodable_au_us;
    int64_t t_req_first_submit_us;
    int64_t t_req_first_output_us;
    int64_t t_req_first_display_us;
    int64_t t_req_first_codec_rendered_us;
    int have_req_first_packet;
    int have_req_first_decodable_au;
    int have_req_first_submit;
    int have_req_first_output;
    int have_req_first_display;
    int have_req_first_codec_rendered;
    int64_t t_worker_start_us;
    int have_worker_start;
    int64_t t_req_worker_start_us;
    int64_t t_req_open_input_done_us;
    int64_t t_req_find_stream_info_done_us;
    int64_t dur_open_input_us;
    int64_t dur_find_stream_info_us;
    int have_open_phases;
    uint64_t cfg_accepted;
    uint64_t video_au_in;
    uint64_t video_au_dropped_nokey;
    uint64_t video_au_dropped_preroll;
    uint64_t video_au_submitted;
    uint64_t video_au_backpressure;
    uint64_t staging_copy_ops;
    uint64_t staging_copy_bytes;
    ms_status last_error;
    ms_hw_log_fn log_fn;
    void *log_ctx;
    int enable_audio;
    ms_hw_audio *audio;
    int have_audio_track;
    int audio_unsupported;
    int audio_ready;
    ms_status audio_status;
    char audio_detail_buf[192];
    uint8_t audio_asc[MS_AUDIO_CSD_MAX];
    size_t audio_asc_size;
    ms_sample_format audio_fmt;
    ms_codec audio_codec;
    int audio_rate;
    int audio_channels;
    uint32_t audio_spf;
    uint32_t audio_pre_skip;
    uint32_t audio_seek_preroll;
    uint64_t audio_au_dropped_unsupported;
    int first_video_presented;
    int64_t last_video_output_ns; /* 最近一次交 Surface；0=尚未 */
    int sync_fallback_no_audio;
    struct {
        int64_t pts_us;
        int has_pts;
        int occupied;
    } vpend[8];
    unsigned vpend_n;
    uint64_t video_au_late;
    uint64_t video_au_held;
    uint64_t video_au_dropped_late;
    ms_h264_ps_set applied_ps;
    int have_applied_ps;
    ms_h264_ps_set pending_ps;
    unsigned pending_ps_wait;
    unsigned avcc_nal_len;
    ms_hevc_ps_set applied_hevc;
    int have_applied_hevc;
    ms_hevc_ps_set pending_hevc;
    unsigned hvcc_nal_len;
    ms_video_cfg applied_vcfg;
    int have_applied_vcfg;
    ms_hdr_info hdr;
    int have_hdr;
    int logged_hdr;
    int logged_dv;
    uint64_t video_csd_inband;
    uint64_t video_csd_inband_same;
    uint64_t video_csd_pending_abandoned;
    /* P4 轨道目录与字幕（懒分配） */
    ms_hw_trk *tracks;
    int ntracks;
    int cap_tracks;
    int user_audio_sel;
    int user_sub_sel;
    int user_ai_sel;
    int user_video_sel;
    unsigned selection_epoch;
    uint64_t audio_switch_count;
    uint64_t audio_au_dropped_unselected;
    ms_sub_doc *sub_ext;
    ms_sub_doc *sub_inband;
    ms_hw_ass_bind *ass;
    ms_hw_ai_bind *ai;
    uint64_t last_media_90k;
    int have_last_media;
    int64_t last_video_pts_us;
    int have_last_video_pts;
    int paused; /* 有效暂停：user_paused 或 focus_paused */
    int user_paused;
    int focus_paused;
    int audio_focus;
    int rate_num;
    int rate_den;
    int seekable; /* -1 未知 */
    int64_t duration_90k;
    int seek_kind;
    int64_t win_start_90k;
    int64_t win_end_90k;
    int64_t live_edge_90k;
    int64_t safety_90k;
    int64_t last_seek_request_90k;
    int64_t last_seek_land_90k;
    int have_seek_land;
    unsigned timeline_epoch;
    int need_surface_resync;
    int surface_resync_taken;
    int64_t surface_resync_90k;
    int have_pending_trim;
    uint32_t pending_skip_start;
    uint32_t pending_skip_end;
    /* Media3 对齐的出帧决策（Android Surface 路径，见 sync_release_decide）。 */
    int sync_in_decoder;          /* 1=解码器出帧前回调本播放器决定时机 */
    int vsync_acquired;
    int sync_holding;             /* 解码器正握着一个“太早”的输出缓冲 */
    int sync_drop_to_key;         /* 严重落后：drive 返回后 flush 并等关键帧 */
    int64_t sync_last_release_ns; /* 最近一次交 Surface 的单调时钟（强制释放判定用） */
    int64_t sync_last_held_pts;
    int64_t sync_last_early_us;
    int64_t vfr_last_adj_ns;      /* VideoFrameReleaseHelper 帧节奏：上一帧调整后的释放时刻 */
    int64_t vfr_last_pts_us;
    int vfr_have;
    uint64_t video_release_scheduled;
    uint64_t video_drop_to_keyframe;
    int64_t sync_log_ns;
};

/*
 * 阈值全部取自 Media3（androidx/media main@101e97c，2026-11-24 核对）：
 *   MediaCodecVideoRenderer.MIN_EARLY_US_LATE_THRESHOLD        = -30_000  → 迟到丢帧
 *   MediaCodecVideoRenderer.MIN_EARLY_US_VERY_LATE_THRESHOLD   = -500_000 → 丢到关键帧
 *   MediaCodecVideoRenderer.DEFAULT_EARLY_SCHEDULING_THRESHOLD_US = 50_000 → 超前多于此先不交
 *   shouldForceRenderOutputBuffer: 迟到且距上次释放 > 100_000us 则强制上屏
 *   VideoFrameReleaseHelper.MAX_ALLOWED_ADJUSTMENT_NS = 20_000_000, VSYNC_OFFSET_PERCENTAGE = 80
 * API < 21 无 releaseOutputBuffer(index, ns)：照 ExoPlayer 2.x（release-v2）renderOutputBuffer
 * 旧路径“earlyUs > 11000 先等到约提前 10ms 再 render”，这里不在持锁线程里 sleep，改为 LATER 重询。
 * mock/VAAPI 等不走回调的路径（宿主测试）沿用 drain_vpend 的同一组阈值。
 * by: Long <mycjzb@gmail.com>
 */
#define MS_HW_SYNC_LATE_US 30000          /* -MIN_EARLY_US_LATE_THRESHOLD */
#define MS_HW_SYNC_VERY_LATE_US 500000    /* -MIN_EARLY_US_VERY_LATE_THRESHOLD */
#define MS_HW_SYNC_EARLY_US 50000         /* DEFAULT_EARLY_SCHEDULING_THRESHOLD_US */
#define MS_HW_SYNC_FORCE_GAP_US 100000    /* shouldForceRenderOutputBuffer 的 100ms */
#define MS_HW_SYNC_LEGACY_EARLY_US 11000  /* ExoPlayer 2.x API<21：>11ms 先等 */
#define MS_HW_VFR_MAX_ADJ_NS 20000000ll   /* MAX_ALLOWED_ADJUSTMENT_NS */
#define MS_HW_VSYNC_OFFSET_PCT 80         /* VSYNC_OFFSET_PERCENTAGE */
#define MS_HW_CSD_PENDING_AU_MAX 16u

typedef struct copy_rel {
    const ms_allocator *alloc;
    uint8_t *data;
} copy_rel;

static void *std_alloc(void *ctx, size_t n)
{
    (void)ctx;
    return malloc(n ? n : 1);
}

static void std_free(void *ctx, void *p)
{
    (void)ctx;
    free(p);
}

static void *palloc(ms_hw_player *p, size_t n)
{
    return p->alloc->alloc_fn(p->alloc->ctx, n);
}

static void pfree(ms_hw_player *p, void *m)
{
    if (m != NULL) {
        p->alloc->free_fn(p->alloc->ctx, m);
    }
}

static int64_t mono_us(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (int64_t)ts.tv_sec * 1000000LL + (int64_t)(ts.tv_nsec / 1000L);
}

static void mark_one(int64_t now, int64_t origin, int64_t *slot, int *have)
{
    if (*have) {
        return;
    }
    *slot = now - origin;
    if (*slot < 0) {
        *slot = 0;
    }
    *have = 1;
}

static void mark_stage_and_req(ms_hw_player *p, int64_t *local_slot, int *local_have,
                               int64_t *req_slot, int *req_have)
{
    int64_t now = mono_us();
    mark_one(now, p->t_start_us, local_slot, local_have);
    if (p->have_request) {
        mark_one(now, p->t_request_us, req_slot, req_have);
    }
}

static void clear_request_times(ms_hw_player *p)
{
    p->t_request_us = -1;
    p->have_request = 0;
    p->t_req_first_packet_us = -1;
    p->t_req_first_decodable_au_us = -1;
    p->t_req_first_submit_us = -1;
    p->t_req_first_output_us = -1;
    p->t_req_first_display_us = -1;
    p->t_req_first_codec_rendered_us = -1;
    p->have_req_first_packet = 0;
    p->have_req_first_decodable_au = 0;
    p->have_req_first_submit = 0;
    p->have_req_first_output = 0;
    p->have_req_first_display = 0;
    p->have_req_first_codec_rendered = 0;
    p->t_worker_start_us = -1;
    p->have_worker_start = 0;
    p->t_req_worker_start_us = -1;
    p->t_req_open_input_done_us = -1;
    p->t_req_find_stream_info_done_us = -1;
    p->dur_open_input_us = -1;
    p->dur_find_stream_info_us = -1;
    p->have_open_phases = 0;
}

static void copy_rel_fn(void *ctx, uint8_t *data, size_t size)
{
    copy_rel *c = (copy_rel *)ctx;
    (void)data;
    (void)size;
    if (c == NULL) {
        return;
    }
    if (c->alloc != NULL && c->data != NULL) {
        c->alloc->free_fn(c->alloc->ctx, c->data);
    }
    if (c->alloc != NULL) {
        c->alloc->free_fn(c->alloc->ctx, c);
    }
}

void ms_hw_player_opts_default(ms_hw_player_opts *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->struct_size = sizeof(*out);
    out->decoder.struct_size = sizeof(out->decoder);
    out->limits.struct_size = sizeof(out->limits);
    out->wait_keyframe = 1;
    out->enable_audio = 1;
    out->allow_system_aac = 0;
    out->audio_mock_mode = -1;
    out->force_decoder_name = NULL;
}


static void hw_ass_lazy(ms_hw_player *p)
{
    if (p == NULL) {
        return;
    }
    (void)ms_hw_ass_bind_ensure(p->alloc, &p->ass);
}

static void hw_ai_lazy(ms_hw_player *p)
{
    if (p == NULL) {
        return;
    }
    (void)ms_hw_ai_bind_ensure(p->alloc, &p->ai);
}

static void hw_ai_sync_audio(ms_hw_player *p)
{
    if (p == NULL) {
        return;
    }
    if (p->audio != NULL) {
        ms_hw_ai_bind_attach_audio(p->ai, p->audio);
    }
}

static int hw_alloc_tracks(ms_hw_player *p);
static int hw_find_track(const ms_hw_player *p, uint32_t id);
static void hw_copy_z(char *dst, size_t cap, const char *s);

static ms_status hw_ai_ensure_track(ms_hw_player *p)
{
    int i;
    if (hw_alloc_tracks(p) != 0) {
        return MS_ERR_NOMEM;
    }
    i = hw_find_track(p, MS_TRACK_ID_AI_SUB);
    if (i < 0) {
        if (p->ntracks >= p->cap_tracks) {
            return MS_ERR_OVERFLOW;
        }
        i = p->ntracks++;
        memset(&p->tracks[i], 0, sizeof(p->tracks[i]));
    }
    p->tracks[i].in_use = 1;
    p->tracks[i].id = MS_TRACK_ID_AI_SUB;
    p->tracks[i].kind = MS_TRACK_SUBTITLE;
    p->tracks[i].level = MS_RECOG_AU;
    hw_copy_z(p->tracks[i].name, sizeof(p->tracks[i].name),
              "AI simulated (not real ASR)");
    if (!p->user_ai_sel) {
        p->tracks[i].selected = 0;
    }
    return MS_OK;
}

static void hw_ai_drop_track(ms_hw_player *p)
{
    int i;
    if (p == NULL) {
        return;
    }
    i = hw_find_track(p, MS_TRACK_ID_AI_SUB);
    if (i >= 0) {
        memset(&p->tracks[i], 0, sizeof(p->tracks[i]));
    }
    p->user_ai_sel = 0;
}

ms_status ms_hw_player_create(const ms_allocator *alloc,
                              const ms_hw_player_opts *opts,
                              ms_hw_player **out)
{
    ms_hw_player *p;
    ms_hw_player_opts o;
    if (out == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    *out = NULL;
    ms_hw_player_opts_default(&o);
    if (opts != NULL) {
        size_t n = opts->struct_size;
        if (n < sizeof(opts->struct_size)) {
            return MS_ERR_INVALID_ARG;
        }
        if (n > sizeof(o)) {
            n = sizeof(o);
        }
        memcpy(&o, opts, n);
        o.struct_size = sizeof(o);
    }
    if (o.wait_keyframe < 0) {
        o.wait_keyframe = 1;
    }
    if (o.decoder.struct_size == 0) {
        o.decoder.struct_size = sizeof(o.decoder);
    }
    if (o.limits.struct_size == 0) {
        o.limits.struct_size = sizeof(o.limits);
    }
    if (opts == NULL || opts->struct_size < offsetof(ms_hw_player_opts, enable_audio) + sizeof(int)) {
        o.enable_audio = 1;
    }
    if (opts == NULL ||
        opts->struct_size < offsetof(ms_hw_player_opts, allow_system_aac) + sizeof(int)) {
        o.allow_system_aac = 0;
    }
    if (opts == NULL ||
        opts->struct_size < offsetof(ms_hw_player_opts, audio_mock_mode) + sizeof(int)) {
        o.audio_mock_mode = -1;
    }
    p = (ms_hw_player *)(alloc && alloc->alloc_fn ? alloc->alloc_fn(alloc->ctx, sizeof(*p))
                                                  : malloc(sizeof(*p)));
    if (p == NULL) {
        return MS_ERR_NOMEM;
    }
    memset(p, 0, sizeof(*p));
    if (alloc != NULL && alloc->alloc_fn != NULL && alloc->free_fn != NULL) {
        p->alloc = alloc;
    } else {
        p->owned_alloc.alloc_fn = std_alloc;
        p->owned_alloc.free_fn = std_free;
        p->alloc = &p->owned_alloc;
    }
    p->opts = o;
    p->force_decoder_name[0] = 0;
    if (opts != NULL &&
        opts->struct_size >= offsetof(ms_hw_player_opts, force_decoder_name) +
                                 sizeof(const char *) &&
        o.force_decoder_name != NULL && o.force_decoder_name[0]) {
        size_t n = strlen(o.force_decoder_name);
        if (n >= sizeof(p->force_decoder_name)) {
            n = sizeof(p->force_decoder_name) - 1u;
        }
        memcpy(p->force_decoder_name, o.force_decoder_name, n);
        p->force_decoder_name[n] = 0;
        p->opts.force_decoder_name = p->force_decoder_name;
    } else {
        p->opts.force_decoder_name = NULL;
    }
    p->enable_audio = (o.enable_audio != 0);
    p->codec = MS_CODEC_H264;
    p->sample_fmt = MS_SAMPLE_FMT_UNKNOWN;
    p->waiting_key = (o.wait_keyframe != 0);
    p->t_start_us = mono_us();
    ms_hdr_info_default(&p->hdr);
    p->t_first_packet_us = -1;
    p->t_first_decodable_au_us = -1;
    p->t_first_submit_us = -1;
    p->t_first_output_us = -1;
    p->t_first_display_us = -1;
    p->t_first_codec_rendered_us = -1;
    p->have_first_codec_rendered = 0;
    clear_request_times(p);
    p->epoch = 1;
    p->selection_epoch = 1;
    p->rate_num = 1;
    p->rate_den = 1;
    p->paused = 0;
    p->user_paused = 0;
    p->focus_paused = 0;
    p->audio_focus = MS_HW_AUDIO_FOCUS_NONE;
    p->seekable = -1;
    p->duration_90k = -1;
    p->seek_kind = MS_HW_SEEK_KIND_UNKNOWN;
    p->win_start_90k = 0;
    p->win_end_90k = -1;
    p->live_edge_90k = -1;
    p->safety_90k = 0;
    p->timeline_epoch = 1;
    *out = p;
    return MS_OK;
}

static void close_audio(ms_hw_player *p)
{
    if (p->audio != NULL) {
        ms_hw_audio_destroy(p->audio);
        p->audio = NULL;
    p->audio_ready = 0;
    p->have_applied_ps = 0;
    p->pending_ps_wait = 0;
    p->avcc_nal_len = 4;
    memset(&p->applied_ps, 0, sizeof(p->applied_ps));
    memset(&p->pending_ps, 0, sizeof(p->pending_ps));
    p->applied_ps.struct_size = sizeof(p->applied_ps);
    p->pending_ps.struct_size = sizeof(p->pending_ps);
    }
}

static void clear_vpend(ms_hw_player *p)
{
    memset(p->vpend, 0, sizeof(p->vpend));
    p->vpend_n = 0;
}


static void close_decoder(ms_hw_player *p);
#ifdef __ANDROID__
#include "ms_hw_api.h" /* ms_hw_device_sdk_int */
#include <android/log.h>
static ms_hw_release_action sync_release_decide(void *ctx, int64_t pts_us, int is_eos,
                                                int can_schedule, int64_t *release_ns);
#endif
static void reset_stage_times(ms_hw_player *p);
static int hw_find_track(const ms_hw_player *p, uint32_t id);
static uint32_t hw_selected_id(const ms_hw_player *p, ms_track_kind kind);
static int hw_emit_ok(const ms_hw_player *p, uint32_t id, int default_if_unknown);
static int catalog_upsert_ev(ms_hw_player *p, const ms_event *ev);
static void hw_note_media_time(ms_hw_player *p, const ms_event *ev);
static ms_status feed_subtitle_ev(ms_hw_player *p, const ms_event *ev,
                                  void (*release_fn)(void *, uint8_t *, size_t),
                                  void *release_ctx);
static void reset_audio_session(ms_hw_player *p);

static void clear_ps_pending(ms_hw_player *p)
{
    memset(&p->pending_ps, 0, sizeof(p->pending_ps));
    p->pending_ps.struct_size = sizeof(p->pending_ps);
    memset(&p->pending_hevc, 0, sizeof(p->pending_hevc));
    p->pending_hevc.struct_size = sizeof(p->pending_hevc);
    p->pending_ps_wait = 0;
}

static ms_status store_player_csd(ms_hw_player *p, const uint8_t *data, size_t n)
{
    uint8_t *nb;
    if (data == NULL || n == 0) {
        return MS_ERR_INVALID_ARG;
    }
    nb = (uint8_t *)palloc(p, n);
    if (nb == NULL) {
        return MS_ERR_NOMEM;
    }
    memcpy(nb, data, n);
    pfree(p, p->csd);
    p->csd = nb;
    p->csd_size = n;
    p->have_csd = 1;
    p->cfg_accepted++;
    return MS_OK;
}

static ms_status apply_ps_set(ms_hw_player *p, const ms_h264_ps_set *ps, int from_inband)
{
    uint8_t packed[8 + 2 * MS_H264_PS_NAL_MAX];
    size_t pn = 0;
    ms_sample_format fmt = p->sample_fmt;
    ms_status st;
    if (ps == NULL || !ps->pair_complete) {
        return MS_ERR_INVALID_ARG;
    }
    if (p->have_applied_ps && ms_h264_ps_equal(&p->applied_ps, ps)) {
        if (from_inband) {
            p->video_csd_inband_same++;
        }
        clear_ps_pending(p);
        return MS_OK;
    }
    if (fmt != MS_SAMPLE_FMT_AVCC) {
        fmt = MS_SAMPLE_FMT_ANNEXB;
    }
    st = ms_h264_ps_pack(fmt, p->avcc_nal_len, ps, packed, sizeof(packed), &pn);
    if (st != MS_OK) {
        return st;
    }
    /* Media3：已 configure 后不因 SPS/PPS 字节抖拆硬解（直播 TS 常见）。 */
    if (p->configured) {
        (void)store_player_csd(p, packed, pn);
        p->applied_ps = *ps;
        p->applied_ps.struct_size = sizeof(p->applied_ps);
        p->have_applied_ps = 1;
        clear_ps_pending(p);
        if (from_inband) {
            p->video_csd_inband_same++;
        }
        return MS_OK;
    }
    st = store_player_csd(p, packed, pn);
    if (st != MS_OK) {
        p->last_error = st;
        return st;
    }
    {
        int w = 0, h = 0;
        if (ms_h264_csd_video_size(fmt, 1, packed, pn, p->avcc_nal_len, &w, &h) == MS_OK) {
            if (w > 0) {
                p->width = w;
            }
            if (h > 0) {
                p->height = h;
            }
        }
    }
    p->applied_ps = *ps;
    p->applied_ps.struct_size = sizeof(p->applied_ps);
    p->have_applied_ps = 1;
    clear_ps_pending(p);
    if (from_inband) {
        p->video_csd_inband++;
    }
    return MS_OK;
}



static void player_log(ms_hw_player *p, const char *msg)
{
    if (p != NULL && p->log_fn != NULL && msg != NULL) {
        p->log_fn(p->log_ctx, 1, msg);
    }
}

/* 从 HEVC CSD/AU 合并 VUI/SEI。已配置后仍更新查询结果，但不重配 MediaCodec。 */
static void merge_hdr_from_buf(ms_hw_player *p, ms_sample_format fmt, int is_csd,
                               const uint8_t *data, size_t size)
{
    ms_hdr_info hi;
    char buf[256];
    if (p == NULL || data == NULL || size == 0) {
        return;
    }
    if (p->codec != MS_CODEC_HEVC && p->codec != MS_CODEC_UNKNOWN) {
        /* 仅解析 HEVC 码流；AV1/VP9 的 color 走容器 ms_ff_get_hdr_info。 */
        if (p->codec != MS_CODEC_H264) {
            return;
        }
    }
    if (ms_hdr_parse_hevc(fmt, is_csd, data, size, &hi) != MS_OK) {
        return;
    }
    if (!hi.have_color && !hi.have_mastering && !hi.have_cll &&
        !hi.have_hdr10_plus && !hi.dolby_vision && hi.profile_idc == 0) {
        return;
    }
    ms_hdr_merge(&p->hdr, &hi);
    p->have_hdr = 1;
    if (p->hdr.dolby_vision && !p->logged_dv) {
        p->logged_dv = 1;
        player_log(p, "HDR: Dolby Vision identified; not supported, falling back to base layer if present");
    }
    if (!p->logged_hdr && p->hdr.kind != MS_HDR_KIND_UNKNOWN) {
        p->logged_hdr = 1;
        snprintf(buf, sizeof(buf),
                 "HDR: kind=%s prim=%d trc=%d matrix=%d range=%d mastering=%d cll=%d "
                 "hdr10plus=%d(detected, not passed, unverified) dv=%d (identify/passthrough, not display effect)",
                 ms_hdr_kind_name(p->hdr.kind), p->hdr.color_primaries, p->hdr.color_transfer,
                 p->hdr.color_matrix, p->hdr.color_range, p->hdr.have_mastering, p->hdr.have_cll,
                 p->hdr.have_hdr10_plus, p->hdr.dolby_vision);
        player_log(p, buf);
    }
}

static ms_status apply_hevc_ps_set(ms_hw_player *p, const ms_hevc_ps_set *ps, int from_inband)
{
    uint8_t packed[16 + 3 * MS_HEVC_PS_NAL_MAX];
    size_t pn = 0;
    ms_sample_format fmt = p->sample_fmt;
    ms_status st;
    if (ps == NULL || !ps->triple_complete) {
        return MS_ERR_INVALID_ARG;
    }
    if (p->have_applied_hevc && ms_hevc_ps_equal(&p->applied_hevc, ps)) {
        if (from_inband) {
            p->video_csd_inband_same++;
        }
        clear_ps_pending(p);
        return MS_OK;
    }
    if (fmt != MS_SAMPLE_FMT_HVCC) {
        fmt = MS_SAMPLE_FMT_ANNEXB;
    }
    st = ms_hevc_ps_pack(fmt, p->hvcc_nal_len ? p->hvcc_nal_len : 4u, ps, packed,
                         sizeof(packed), &pn);
    if (st != MS_OK) {
        return st;
    }
    /* Media3：已 configure 后不因带内/后续 VPS/SPS/PPS 字节抖拆硬解。
     * 直播 TS 常抖 PPS（ffprobe 亦见 PPS id 异常）；连环 recreate 永不首帧。 */
    if (p->configured) {
        (void)store_player_csd(p, packed, pn);
        merge_hdr_from_buf(p, fmt, 1, packed, pn);
        p->applied_hevc = *ps;
        p->applied_hevc.struct_size = sizeof(p->applied_hevc);
        p->have_applied_hevc = 1;
        clear_ps_pending(p);
        if (from_inband) {
            p->video_csd_inband_same++;
        }
        return MS_OK;
    }
    st = store_player_csd(p, packed, pn);
    if (st != MS_OK) {
        p->last_error = st;
        return st;
    }
    merge_hdr_from_buf(p, fmt, 1, packed, pn);
    p->applied_hevc = *ps;
    p->applied_hevc.struct_size = sizeof(p->applied_hevc);
    p->have_applied_hevc = 1;
    p->codec = MS_CODEC_HEVC;
    clear_ps_pending(p);
    if (from_inband) {
        p->video_csd_inband++;
    }
    return MS_OK;
}

static ms_status handle_inband_hevc(ms_hw_player *p, const ms_event *ev, int *drop_au)
{
    ms_hevc_ps_set cur;
    ms_status st;
    unsigned ls;
    *drop_au = 0;
    memset(&cur, 0, sizeof(cur));
    cur.struct_size = sizeof(cur);
    if (ev->data == NULL || ev->data_size == 0) {
        return MS_OK;
    }
    merge_hdr_from_buf(p,
                       ev->sample_format != MS_SAMPLE_FMT_UNKNOWN ? ev->sample_format
                                                                  : p->sample_fmt,
                       0, ev->data, ev->data_size);
    ls = p->hvcc_nal_len ? p->hvcc_nal_len : 4u;
    if (ev->sample_format == MS_SAMPLE_FMT_HVCC && p->sample_fmt != MS_SAMPLE_FMT_ANNEXB) {
        p->sample_fmt = MS_SAMPLE_FMT_HVCC;
    } else if (ev->sample_format == MS_SAMPLE_FMT_ANNEXB) {
        p->sample_fmt = MS_SAMPLE_FMT_ANNEXB;
    }
    st = ms_hevc_ps_parse(ev->sample_format != MS_SAMPLE_FMT_UNKNOWN ? ev->sample_format
                                                                    : p->sample_fmt,
                          0, ev->data, ev->data_size, ls, &cur);
    if (st == MS_ERR_OVERFLOW) {
        return st;
    }
    if (st != MS_OK) {
        return MS_OK;
    }
    if (!cur.have_vps && !cur.have_sps && !cur.have_pps) {
        if (p->pending_hevc.have_vps || p->pending_hevc.have_sps || p->pending_hevc.have_pps) {
            p->pending_ps_wait++;
            if (p->pending_ps_wait > MS_HW_CSD_PENDING_AU_MAX) {
                clear_ps_pending(p);
                p->video_csd_pending_abandoned++;
                return MS_OK;
            }
            *drop_au = 1;
        }
        return MS_OK;
    }
    if (cur.have_vps) {
        int same = (p->have_applied_hevc && p->applied_hevc.have_vps &&
                    p->applied_hevc.vps_size == cur.vps_size &&
                    memcmp(p->applied_hevc.vps, cur.vps, cur.vps_size) == 0);
        if (!same) {
            p->pending_hevc.have_vps = 1;
            p->pending_hevc.vps_size = cur.vps_size;
            memcpy(p->pending_hevc.vps, cur.vps, cur.vps_size);
            p->pending_hevc.vps_id = cur.vps_id;
            if (p->pending_hevc.have_sps && p->pending_hevc.sps_vps_id != cur.vps_id) {
                p->pending_hevc.have_sps = 0;
                p->pending_hevc.sps_size = 0;
                p->pending_hevc.have_pps = 0;
                p->pending_hevc.pps_size = 0;
            }
        } else if (!p->pending_hevc.have_vps) {
            p->pending_hevc.have_vps = 1;
            p->pending_hevc.vps_size = cur.vps_size;
            memcpy(p->pending_hevc.vps, cur.vps, cur.vps_size);
            p->pending_hevc.vps_id = cur.vps_id;
        }
    }
    if (cur.have_sps) {
        int same = (p->have_applied_hevc && p->applied_hevc.have_sps &&
                    p->applied_hevc.sps_size == cur.sps_size &&
                    memcmp(p->applied_hevc.sps, cur.sps, cur.sps_size) == 0);
        if (!same) {
            p->pending_hevc.have_sps = 1;
            p->pending_hevc.sps_size = cur.sps_size;
            memcpy(p->pending_hevc.sps, cur.sps, cur.sps_size);
            p->pending_hevc.sps_id = cur.sps_id;
            p->pending_hevc.sps_vps_id = cur.sps_vps_id;
            if (p->pending_hevc.have_pps && p->pending_hevc.pps_sps_id != cur.sps_id) {
                p->pending_hevc.have_pps = 0;
                p->pending_hevc.pps_size = 0;
            }
            /* 新 SPS 不得配不匹配的旧 VPS */
            if (p->pending_hevc.have_vps && p->pending_hevc.vps_id != cur.sps_vps_id) {
                p->pending_hevc.have_vps = 0;
                p->pending_hevc.vps_size = 0;
            }
        } else if (!p->pending_hevc.have_sps) {
            p->pending_hevc.have_sps = 1;
            p->pending_hevc.sps_size = cur.sps_size;
            memcpy(p->pending_hevc.sps, cur.sps, cur.sps_size);
            p->pending_hevc.sps_id = cur.sps_id;
            p->pending_hevc.sps_vps_id = cur.sps_vps_id;
        }
        if (!p->pending_hevc.have_vps && p->have_applied_hevc &&
            p->applied_hevc.have_vps && p->applied_hevc.vps_id == cur.sps_vps_id) {
            p->pending_hevc.have_vps = 1;
            p->pending_hevc.vps_size = p->applied_hevc.vps_size;
            memcpy(p->pending_hevc.vps, p->applied_hevc.vps, p->applied_hevc.vps_size);
            p->pending_hevc.vps_id = p->applied_hevc.vps_id;
        }
    }
    if (cur.have_pps) {
        p->pending_hevc.have_pps = 1;
        p->pending_hevc.pps_size = cur.pps_size;
        memcpy(p->pending_hevc.pps, cur.pps, cur.pps_size);
        p->pending_hevc.pps_id = cur.pps_id;
        p->pending_hevc.pps_sps_id = cur.pps_sps_id;
        if (!p->pending_hevc.have_sps && p->have_applied_hevc &&
            p->applied_hevc.have_sps && p->applied_hevc.sps_id == cur.pps_sps_id) {
            p->pending_hevc.have_sps = 1;
            p->pending_hevc.sps_size = p->applied_hevc.sps_size;
            memcpy(p->pending_hevc.sps, p->applied_hevc.sps, p->applied_hevc.sps_size);
            p->pending_hevc.sps_id = p->applied_hevc.sps_id;
            p->pending_hevc.sps_vps_id = p->applied_hevc.sps_vps_id;
        }
        if (!p->pending_hevc.have_vps && p->pending_hevc.have_sps &&
            p->have_applied_hevc && p->applied_hevc.have_vps &&
            p->applied_hevc.vps_id == p->pending_hevc.sps_vps_id) {
            p->pending_hevc.have_vps = 1;
            p->pending_hevc.vps_size = p->applied_hevc.vps_size;
            memcpy(p->pending_hevc.vps, p->applied_hevc.vps, p->applied_hevc.vps_size);
            p->pending_hevc.vps_id = p->applied_hevc.vps_id;
        }
    }
    p->pending_hevc.struct_size = sizeof(p->pending_hevc);
    p->pending_hevc.triple_complete =
        (p->pending_hevc.have_vps && p->pending_hevc.have_sps && p->pending_hevc.have_pps &&
         p->pending_hevc.sps_vps_id == p->pending_hevc.vps_id &&
         p->pending_hevc.pps_sps_id == p->pending_hevc.sps_id);
    if (!p->pending_hevc.triple_complete) {
        p->pending_ps_wait++;
        if (p->pending_ps_wait > MS_HW_CSD_PENDING_AU_MAX) {
            clear_ps_pending(p);
            p->video_csd_pending_abandoned++;
            return MS_OK;
        }
        *drop_au = 1;
        return MS_OK;
    }
    st = apply_hevc_ps_set(p, &p->pending_hevc, 1);
    if (st != MS_OK) {
        return st;
    }
    return MS_OK;
}


static ms_status apply_video_cfg(ms_hw_player *p, const ms_video_cfg *cfg, int from_inband)
{
    ms_status st;
    if (p == NULL || cfg == NULL || !cfg->complete) {
        return MS_ERR_INVALID_ARG;
    }
    if (cfg->csd_size == 0) {
        if ((cfg->codec != MS_CODEC_H263 && cfg->codec != MS_CODEC_AV1) ||
            cfg->width <= 0 || cfg->height <= 0) {
            return MS_ERR_INVALID_ARG;
        }
    } else if (cfg->csd_size > sizeof(cfg->csd)) {
        return MS_ERR_OVERFLOW;
    }
    if (p->have_applied_vcfg && ms_video_cfg_equal(&p->applied_vcfg, cfg)) {
        if (from_inband) {
            p->video_csd_inband_same++;
        }
        return MS_OK;
    }
    if (p->configured) {
        close_decoder(p);
        p->waiting_key = (p->opts.wait_keyframe != 0);
        p->eos_in = 0;
        p->eos_out = 0;
        p->epoch++;
        reset_stage_times(p);
    }
    if (cfg->csd_size == 0) {
        pfree(p, p->csd);
        p->csd = NULL;
        p->csd_size = 0;
        p->have_csd = 1; /* 配置已就绪（合法无 CSD），不是捏造的参数集 */
        p->cfg_accepted++;
        st = MS_OK;
    } else {
        st = store_player_csd(p, cfg->csd, cfg->csd_size);
        if (st != MS_OK) {
            p->last_error = st;
            return st;
        }
    }
    p->applied_vcfg = *cfg;
    p->applied_vcfg.struct_size = sizeof(p->applied_vcfg);
    p->have_applied_vcfg = 1;
    p->codec = cfg->codec;
    if (cfg->sample_fmt != MS_SAMPLE_FMT_UNKNOWN) {
        p->sample_fmt = cfg->sample_fmt;
    }
    if (cfg->width > 0) {
        p->width = cfg->width;
    }
    if (cfg->height > 0) {
        p->height = cfg->height;
    }
    if (from_inband) {
        p->video_csd_inband++;
    }
    return MS_OK;
}

static ms_status handle_inband_vbit(ms_hw_player *p, const ms_event *ev, int *drop_au)
{
    ms_video_cfg cfg;
    ms_status st;
    *drop_au = 0;
    if (ev == NULL || ev->data == NULL || ev->data_size == 0) {
        return MS_OK;
    }
    ms_video_cfg_default(&cfg);
    st = ms_video_cfg_parse(ev->track.codec, ev->sample_format, 0, ev->data,
                            ev->data_size, &cfg);
    if (st == MS_ERR_OVERFLOW) {
        return st;
    }
    if (st != MS_OK || !cfg.complete) {
        return MS_OK;
    }
    return apply_video_cfg(p, &cfg, 1);
}

/* 返回 OK 且 *drop_au=1：本 AU 含未成对的新参数集，已消费/丢弃，勿提交解码器。 */
static ms_status handle_inband_ps(ms_hw_player *p, const ms_event *ev, int *drop_au)
{
    ms_h264_ps_set cur;
    ms_status st;
    unsigned ls;
    *drop_au = 0;
    memset(&cur, 0, sizeof(cur));
    cur.struct_size = sizeof(cur);
    if (ev->data == NULL || ev->data_size == 0) {
        return MS_OK;
    }
    ls = p->avcc_nal_len ? p->avcc_nal_len : 4u;
    if (ev->sample_format == MS_SAMPLE_FMT_AVCC && p->sample_fmt != MS_SAMPLE_FMT_ANNEXB) {
        p->sample_fmt = MS_SAMPLE_FMT_AVCC;
    } else if (ev->sample_format == MS_SAMPLE_FMT_ANNEXB) {
        p->sample_fmt = MS_SAMPLE_FMT_ANNEXB;
    }
    st = ms_h264_ps_parse(ev->sample_format != MS_SAMPLE_FMT_UNKNOWN ? ev->sample_format
                                                                    : p->sample_fmt,
                          0, ev->data, ev->data_size, ls, &cur);
    if (st == MS_ERR_OVERFLOW) {
        return st;
    }
    if (st != MS_OK) {
        /* 畸形长度：不应用配置，已配置则仍可尝试提交本 AU。 */
        return MS_OK;
    }
    if (!cur.have_sps && !cur.have_pps) {
        if (p->pending_ps.have_sps || p->pending_ps.have_pps) {
            p->pending_ps_wait++;
            if (p->pending_ps_wait > MS_HW_CSD_PENDING_AU_MAX) {
                clear_ps_pending(p);
                p->video_csd_pending_abandoned++;
                return MS_OK;
            }
            /* 等待配套参数集期间不向可能已过期的解码器送帧。 */
            *drop_au = 1;
        }
        return MS_OK;
    }
    /* 增量合并：新 SPS 丢弃不匹配的旧 pending PPS（禁止新 SPS+旧 PPS）。 */
    if (cur.have_sps) {
        int same_applied_sps = (p->have_applied_ps && p->applied_ps.have_sps &&
                                p->applied_ps.sps_size == cur.sps_size &&
                                memcmp(p->applied_ps.sps, cur.sps, cur.sps_size) == 0);
        if (!same_applied_sps) {
            p->pending_ps.have_sps = 1;
            p->pending_ps.sps_size = cur.sps_size;
            memcpy(p->pending_ps.sps, cur.sps, cur.sps_size);
            p->pending_ps.sps_id = cur.sps_id;
            if (p->pending_ps.have_pps && p->pending_ps.pps_sps_id != cur.sps_id) {
                p->pending_ps.have_pps = 0;
                p->pending_ps.pps_size = 0;
            }
        } else if (!p->pending_ps.have_sps) {
            /* 与已应用 SPS 相同，供仅 PPS 更新配对。 */
            p->pending_ps.have_sps = 1;
            p->pending_ps.sps_size = cur.sps_size;
            memcpy(p->pending_ps.sps, cur.sps, cur.sps_size);
            p->pending_ps.sps_id = cur.sps_id;
        }
    }
    if (cur.have_pps) {
        p->pending_ps.have_pps = 1;
        p->pending_ps.pps_size = cur.pps_size;
        memcpy(p->pending_ps.pps, cur.pps, cur.pps_size);
        p->pending_ps.pps_id = cur.pps_id;
        p->pending_ps.pps_sps_id = cur.pps_sps_id;
        /* 仅 PPS：允许与已应用且 id 匹配的 SPS 配对（PPS 更新，不是胡乱拼接新 SPS）。 */
        if (!p->pending_ps.have_sps && p->have_applied_ps &&
            p->applied_ps.have_sps && p->applied_ps.sps_id == cur.pps_sps_id) {
            p->pending_ps.have_sps = 1;
            p->pending_ps.sps_size = p->applied_ps.sps_size;
            memcpy(p->pending_ps.sps, p->applied_ps.sps, p->applied_ps.sps_size);
            p->pending_ps.sps_id = p->applied_ps.sps_id;
        }
    }
    p->pending_ps.struct_size = sizeof(p->pending_ps);
    p->pending_ps.pair_complete = (p->pending_ps.have_sps && p->pending_ps.have_pps &&
                                   p->pending_ps.pps_sps_id == p->pending_ps.sps_id);
    if (!p->pending_ps.pair_complete) {
        p->pending_ps_wait++;
        if (p->pending_ps_wait > MS_HW_CSD_PENDING_AU_MAX) {
            clear_ps_pending(p);
            p->video_csd_pending_abandoned++;
            return MS_OK;
        }
        *drop_au = 1;
        return MS_OK;
    }
    st = apply_ps_set(p, &p->pending_ps, 1);
    if (st != MS_OK) {
        return st;
    }
    return MS_OK;
}

static void sync_reset(ms_hw_player *p)
{
    p->sync_holding = 0;
    p->sync_drop_to_key = 0;
    p->sync_last_held_pts = INT64_MIN;
    p->vfr_have = 0;
}

static void close_decoder(ms_hw_player *p)
{
    if (p->dec != NULL) {
        ms_hw_decoder_destroy(p->dec);
        p->dec = NULL;
    }
    p->sync_in_decoder = 0;
    sync_reset(p);
#ifdef __ANDROID__
    if (p->vsync_acquired) {
        ms_hw_vsync_release();
        p->vsync_acquired = 0;
    }
#endif
    p->configured = 0;
    p->started = 0;
    p->eos_out = 0;
    p->csd_resubmit = 0;
    clear_vpend(p);
}

/* flush 之后把已缓存的 SPS/PPS 再作为 CODEC_CONFIG 送入仍配置着的解码器。
 * NDK / API 21+ 常能在 flush 后保留 CSD；API 16–20 JNI 路径上 OMX.google
 * 往往需要再排队一次，否则 seek 后关键帧解不出来。所有权仍在 p->csd。 */
static ms_status resubmit_cached_video_csd(ms_hw_player *p)
{
    ms_hw_buf buf;
    ms_status st;
    if (p == NULL || p->dec == NULL || !p->configured ||
        !p->have_csd || p->csd == NULL || p->csd_size == 0) {
        if (p != NULL) {
            p->csd_resubmit = 0;
        }
        return MS_OK;
    }
    memset(&buf, 0, sizeof(buf));
    buf.data = p->csd;
    buf.size = p->csd_size;
    st = ms_hw_decoder_submit(p->dec, &buf, 0, MS_HW_FLAG_CODEC_CONFIG);
    if (st == MS_LIMIT || st == MS_AGAIN) {
        return st;
    }
    if (st != MS_OK) {
        /* 非背压错误不让 pump 因 OVERFLOW 退出；保留标志后续再试。 */
        return MS_OK;
    }
    p->csd_resubmit = 0;
    /* CONFIG 占一个 input slot。立刻 drive，避免 cap=1 时挡住紧随其后的关键帧。
     * mock 需 drive 才释放；JNI/NDK 也在 drive 路径回收入队缓冲。 */
    (void)ms_hw_decoder_drive(p->dec);
    return MS_OK;
}

void ms_hw_player_destroy(ms_hw_player *p)
{
    const ms_allocator *a;
    if (p == NULL) {
        return;
    }
    close_decoder(p);
    close_audio(p);
    pfree(p, p->csd);
    p->csd = NULL;
    if (p->sub_ext != NULL) {
        ms_sub_doc_destroy(p->sub_ext);
        p->sub_ext = NULL;
    }
    if (p->sub_inband != NULL) {
        ms_sub_doc_destroy(p->sub_inband);
        p->sub_inband = NULL;
    }
    if (p->ass != NULL) {
        ms_hw_ass_bind_destroy(p->ass);
        p->ass = NULL;
    }
    if (p->audio != NULL) {
        ms_hw_audio_set_pcm_tap(p->audio, NULL, NULL);
    }
    if (p->ai != NULL) {
        (void)ms_hw_ai_bind_set_enabled(p->ai, 0);
        ms_hw_ai_bind_destroy(p->ai);
        p->ai = NULL;
    }
    pfree(p, p->tracks);
    p->tracks = NULL;
    a = p->alloc;
    if (a == &p->owned_alloc) {
        free(p);
    } else if (a != NULL && a->free_fn != NULL) {
        a->free_fn(a->ctx, p);
    } else {
        free(p);
    }
}

void ms_hw_player_set_log(ms_hw_player *p, ms_hw_log_fn fn, void *ctx)
{
    if (p == NULL) {
        return;
    }
    p->log_fn = fn;
    p->log_ctx = ctx;
    if (p->dec != NULL) {
        ms_hw_decoder_set_log(p->dec, fn, ctx);
    }
}

static void reset_stage_times(ms_hw_player *p)
{
    p->t_first_packet_us = -1;
    p->t_first_decodable_au_us = -1;
    p->t_first_submit_us = -1;
    p->t_first_output_us = -1;
    p->t_first_display_us = -1;
    p->t_first_codec_rendered_us = -1;
    p->have_first_packet = 0;
    p->have_first_decodable_au = 0;
    p->have_first_submit = 0;
    p->have_first_output = 0;
    p->have_first_display = 0;
    p->have_first_codec_rendered = 0;
    p->t_start_us = mono_us();
}

ms_status ms_hw_player_mark_request(ms_hw_player *p)
{
    if (p == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    p->canceled = 0;
    clear_request_times(p);
    p->t_request_us = mono_us();
    p->have_request = 1;
    return MS_OK;
}

ms_status ms_hw_player_mark_worker_start(ms_hw_player *p)
{
    int64_t now;
    if (p == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    now = mono_us();
    p->t_worker_start_us = now;
    p->have_worker_start = 1;
    if (p->have_request) {
        mark_one(now, p->t_request_us, &p->t_req_worker_start_us, &p->have_worker_start);
        /* have_worker_start already 1; mark_one is fine */
        p->t_req_worker_start_us = now - p->t_request_us;
        if (p->t_req_worker_start_us < 0) {
            p->t_req_worker_start_us = 0;
        }
    }
    return MS_OK;
}

ms_status ms_hw_player_note_open_phases(ms_hw_player *p, int64_t dur_open_input_us,
                                        int64_t dur_find_stream_info_us)
{
    int64_t base;
    if (p == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    p->dur_open_input_us = dur_open_input_us;
    p->dur_find_stream_info_us = dur_find_stream_info_us;
    p->have_open_phases = 1;
    if (!p->have_request) {
        return MS_OK;
    }
    base = p->have_worker_start ? p->t_req_worker_start_us : 0;
    if (dur_open_input_us >= 0) {
        p->t_req_open_input_done_us = base + dur_open_input_us;
    }
    if (dur_open_input_us >= 0 && dur_find_stream_info_us >= 0) {
        p->t_req_find_stream_info_done_us = base + dur_open_input_us + dur_find_stream_info_us;
    }
    return MS_OK;
}

static ms_status ensure_decoder(ms_hw_player *p)
{
    ms_status st;
    if (p->canceled) {
        return MS_ERR_CANCELED;
    }
    if (p->dec == NULL) {
        st = ms_hw_decoder_create(p->alloc, &p->opts.limits, &p->opts.decoder, &p->dec);
        if (st != MS_OK) {
            p->last_error = st;
            return st;
        }
        ms_hw_decoder_set_log(p->dec, p->log_fn, p->log_ctx);
        if (p->force_decoder_name[0]) {
            (void)ms_hw_decoder_set_preferred_name(p->dec, p->force_decoder_name);
        }
    }
    if (p->configured) {
        return MS_OK;
    }
    if (p->surface == NULL || !p->have_csd) {
        return MS_AGAIN;
    }
    {
        ms_hw_config cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.struct_size = sizeof(cfg);
        cfg.codec = p->codec;
        cfg.sample_fmt = p->sample_fmt;
        if (cfg.sample_fmt == MS_SAMPLE_FMT_UNKNOWN) {
            if (p->codec == MS_CODEC_HEVC) {
                cfg.sample_fmt = MS_SAMPLE_FMT_ANNEXB;
            } else if (p->codec == MS_CODEC_MPEG2_VIDEO) {
                cfg.sample_fmt = MS_SAMPLE_FMT_MPV;
            } else if (p->codec == MS_CODEC_MPEG4_PART2) {
                cfg.sample_fmt = MS_SAMPLE_FMT_MP4V;
            } else if (p->codec == MS_CODEC_H263) {
                cfg.sample_fmt = MS_SAMPLE_FMT_H263;
            } else if (p->codec == MS_CODEC_AV1) {
                cfg.sample_fmt = MS_SAMPLE_FMT_AV1;
            } else if (p->codec == MS_CODEC_AVS_PLUS) {
                cfg.sample_fmt = MS_SAMPLE_FMT_CAVS;
            } else {
                cfg.sample_fmt = MS_SAMPLE_FMT_AVCC;
            }
        }
        cfg.csd = p->csd;
        cfg.csd_size = p->csd_size;
        if ((p->width <= 0 || p->height <= 0) && p->csd != NULL && p->csd_size > 0) {
            /* 部分 Android 解码器（含模拟器 c2.goldfish）要求 MediaFormat 宽高。
             * H.264 事件本身不带尺寸，从 CSD/SPS 补齐；其它编码走 ms_video_cfg_parse。 */
            if (p->codec == MS_CODEC_H264) {
                int w = 0, h = 0;
                if (ms_h264_csd_video_size(p->sample_fmt, 1, p->csd, p->csd_size,
                                           0, &w, &h) == MS_OK && w > 0 && h > 0) {
                    if (p->width <= 0) {
                        p->width = w;
                    }
                    if (p->height <= 0) {
                        p->height = h;
                    }
                }
            } else {
                ms_video_cfg vcfg;
                ms_video_cfg_default(&vcfg);
                if (ms_video_cfg_parse(p->codec, p->sample_fmt, 1, p->csd,
                                       p->csd_size, &vcfg) == MS_OK &&
                    vcfg.width > 0 && vcfg.height > 0) {
                    if (p->width <= 0) {
                        p->width = vcfg.width;
                    }
                    if (p->height <= 0) {
                        p->height = vcfg.height;
                    }
                }
            }
        }
        if (p->width <= 0 && p->hdr.width > 0) {
            p->width = p->hdr.width;
        }
        if (p->height <= 0 && p->hdr.height > 0) {
            p->height = p->hdr.height;
        }
        /* Media3：Format 须带齐 width/height 才 maybeInitCodec；缺尺寸则 AGAIN 等下一组 SPS。 */
        if (p->width <= 0 || p->height <= 0) {
            return MS_AGAIN;
        }
        cfg.width = p->width;
        cfg.height = p->height;
        cfg.time_base_num = 1;
        cfg.time_base_den = 1000000;
        cfg.surface = p->surface;
        if (p->have_hdr || p->hdr.struct_size > 0) {
            cfg.hdr = p->hdr;
            cfg.hdr.struct_size = sizeof(cfg.hdr);
            ms_hdr_classify(&cfg.hdr);
        }
        st = ms_hw_decoder_configure(p->dec, &cfg);
        if (st != MS_OK) {
            p->last_error = st;
            return st;
        }
        st = ms_hw_decoder_start(p->dec);
        if (st != MS_OK) {
            p->last_error = st;
            return st;
        }
        p->configured = 1;
        p->started = 1;
        /* 直播 TS：lavf 常不置 KEY，IDR 扫描偶发漏；CSD 已齐则交给硬解自行丢非参考帧。
         * 若仍等 KEY，易 15s 无 submit（深圳 H264 实测）。 */
        p->waiting_key = 0;
#ifdef __ANDROID__
        /* 只有 Surface 输出才按 Media3 决定出帧时机；无 Surface（测试回读）保持旧行为。 */
        if (p->surface != NULL && p->dec != NULL) {
            p->dec->release_fn = sync_release_decide;
            p->dec->release_fn_ctx = p;
            p->sync_in_decoder = 1;
            sync_reset(p);
            if (!p->vsync_acquired && ms_hw_device_sdk_int() >= 21) {
                ms_hw_vsync_acquire();
                p->vsync_acquired = 1;
            }
        }
#endif
    }
    return MS_OK;
}

static void detach_video_for_surface(ms_hw_player *p)
{
    /* 只拆视频解码器与待呈现队列，保留 CSD、音频会话与请求计时。
     * 旧 Surface 上的 note_display 证据不再适用于新表面。
     * 记下当前位置，重绑后由调用方向最近关键帧内部 seek（可 seek）或等 IDR。 */
    p->surface_resync_90k = ms_hw_player_position_90k(p);
    p->need_surface_resync = 1;
    p->surface_resync_taken = 0;
    close_decoder(p);
    p->first_video_presented = 0;
    p->waiting_key = (p->opts.wait_keyframe != 0);
    p->have_first_display = 0;
    p->t_first_display_us = -1;
    p->have_req_first_display = 0;
    p->t_req_first_display_us = -1;
}

ms_status ms_hw_player_set_surface(ms_hw_player *p, void *surface)
{
    if (p == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    if (p->canceled) {
        return MS_ERR_CANCELED;
    }
    if (p->surface != surface) {
        if (p->dec != NULL || p->configured) {
            detach_video_for_surface(p);
        }
        p->surface = surface;
    }
    if (p->surface == NULL) {
        return MS_OK;
    }
    {
        ms_status st = ensure_decoder(p);
        if (st == MS_AGAIN) {
            return MS_OK;
        }
        return st;
    }
}

ms_status ms_hw_player_set_force_decoder_name(ms_hw_player *p, const char *name)
{
    if (p == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    if (p->canceled) {
        return MS_ERR_CANCELED;
    }
    if (p->configured) {
        return MS_ERR_STATE;
    }
    p->force_decoder_name[0] = 0;
    p->opts.force_decoder_name = NULL;
    if (name != NULL && name[0]) {
        size_t n = strlen(name);
        if (n >= sizeof(p->force_decoder_name)) {
            n = sizeof(p->force_decoder_name) - 1u;
        }
        memcpy(p->force_decoder_name, name, n);
        p->force_decoder_name[n] = 0;
        p->opts.force_decoder_name = p->force_decoder_name;
    }
    if (p->dec != NULL) {
        return ms_hw_decoder_set_preferred_name(p->dec,
                                                p->force_decoder_name[0]
                                                    ? p->force_decoder_name
                                                    : NULL);
    }
    return MS_OK;
}

static int ev_is_video_h264(const ms_event *ev)
{
    if (ev == NULL) {
        return 0;
    }
    if (ev->track.codec == MS_CODEC_H264 || ev->track.codec == MS_CODEC_HEVC ||
        ev->track.codec == MS_CODEC_MPEG2_VIDEO ||
        ev->track.codec == MS_CODEC_MPEG4_PART2 ||
        ev->track.codec == MS_CODEC_H263 ||
        ev->track.codec == MS_CODEC_AV1 ||
        ev->track.codec == MS_CODEC_AVS_PLUS) {
        return 1;
    }
    if (ev->track.kind == MS_TRACK_VIDEO && ev->track.codec == MS_CODEC_UNKNOWN) {
        return 0;
    }
    return ev->track.codec == MS_CODEC_H264 || ev->track.codec == MS_CODEC_HEVC ||
           ev->track.codec == MS_CODEC_MPEG2_VIDEO ||
           ev->track.codec == MS_CODEC_MPEG4_PART2 ||
           ev->track.codec == MS_CODEC_H263 ||
           ev->track.codec == MS_CODEC_AV1 ||
           ev->track.codec == MS_CODEC_AVS_PLUS;
}

static int ev_is_startcode_vbit(const ms_event *ev)
{
    if (ev == NULL) {
        return 0;
    }
    return ev->track.codec == MS_CODEC_MPEG2_VIDEO ||
           ev->track.codec == MS_CODEC_MPEG4_PART2 ||
           ev->track.codec == MS_CODEC_H263 ||
           ev->track.codec == MS_CODEC_AV1 ||
           ev->track.codec == MS_CODEC_AVS_PLUS;
}

static int audio_legal_no_csd_player(ms_codec codec)
{
    return codec == MS_CODEC_AC3 || codec == MS_CODEC_MP3 || codec == MS_CODEC_MP2 ||
#ifdef MS_SINGLE_SO
           codec == MS_CODEC_EAC3 || codec == MS_CODEC_G711_ALAW || codec == MS_CODEC_G711_ULAW ||
           codec == MS_CODEC_DTS || codec == MS_CODEC_AAC_LATM ||
#endif
           codec == MS_CODEC_PCM || codec == MS_CODEC_FLAC;
}

static int ev_is_hw_audio(const ms_event *ev)
{
    return ev != NULL && ms_audio_hw_path_ready(ev->track.codec);
}

static ms_status au_to_us(const ms_event *ev, int64_t origin_90k, int64_t *pts_us, int *has)
{
    int64_t pts;
    int num, den;
    *has = 0;
    *pts_us = 0;
#ifndef __ANDROID__
    {
        int hp = 0, hd = 0;
        int64_t dts = 0;
        ms_status st = ms_event_signed_ts(ev, origin_90k, &hp, &pts, &hd, &dts);
        if (st != MS_OK) {
            return st;
        }
        if (!hp) {
            return MS_OK;
        }
        num = ev->time_base_num > 0 ? ev->time_base_num : 1;
        den = ev->time_base_den > 0 ? ev->time_base_den : 90000;
        st = ms_rescale_q(pts, num, den, 1, 1000000, pts_us);
        if (st != MS_OK) {
            return st;
        }
        *has = 1;
        return MS_OK;
    }
#else
    (void)origin_90k;
    if (!ev->has_pts) {
        return MS_OK;
    }
    /* Android 此 .so 不链 libmsmedia：用 64 位中间运算把事件时基转到微秒。 */
    pts = (int64_t)ev->pts;
    num = ev->time_base_num > 0 ? ev->time_base_num : 1;
    den = ev->time_base_den > 0 ? ev->time_base_den : 90000;
    if (den <= 0 || num <= 0) {
        return MS_ERR_INVALID_ARG;
    }
    {
        long double v = ((long double)pts * (long double)num * 1000000.0L) /
                        (long double)den;
        if (v > (long double)9223372036854775807.0 ||
            v < (long double)(-9223372036854775807.0 - 1.0)) {
            return MS_ERR_OVERFLOW;
        }
        *pts_us = (int64_t)v;
        *has = 1;
    }
    return MS_OK;
#endif
}

static void set_audio_detail(ms_hw_player *p, const char *s)
{
    size_t n;
    if (s == NULL) {
        p->audio_detail_buf[0] = 0;
        return;
    }
    n = strlen(s);
    if (n >= sizeof(p->audio_detail_buf)) {
        n = sizeof(p->audio_detail_buf) - 1u;
    }
    memcpy(p->audio_detail_buf, s, n);
    p->audio_detail_buf[n] = 0;
}

static ms_status ensure_audio(ms_hw_player *p)
{
    ms_status st;
    ms_hw_audio_config cfg;
    if (!p->enable_audio) {
        p->sync_fallback_no_audio = 1;
        return MS_ERR_UNSUPPORTED;
    }
    if (p->audio_unsupported) {
        return p->audio_status != 0 ? p->audio_status : MS_ERR_UNSUPPORTED;
    }
    if (p->audio == NULL) {
        ms_hw_create_opts ao;
        memset(&ao, 0, sizeof(ao));
        ao = p->opts.decoder;
        ao.struct_size = sizeof(ao);
        /* 不把 allow_system_aac 写成 decoder.allow_software；视频软解仍拒绝。 */
        if (p->opts.audio_mock_mode >= 0) {
            ao.mock_mode = p->opts.audio_mock_mode;
        }
        st = ms_hw_audio_create(p->alloc, &p->opts.limits, &ao, &p->audio);
        if (st != MS_OK) {
            p->audio_status = st;
            return st;
        }
        hw_ai_sync_audio(p);
        ms_hw_audio_set_passthrough_caps(p->audio, p->pt_caps);
        if (p->opts.allow_system_aac) {
            st = ms_hw_audio_set_aac_policy(p->audio, MS_HW_AAC_POLICY_ALLOW_SYSTEM);
            if (st != MS_OK) {
                p->audio_status = st;
                return st;
            }
        }
    }
    if (p->audio_asc_size == 0) {
        /* AC-3 / MP2 / MP3 / PCM / FLAC：有采样率与声道即可配置（FLAC 有 STREAMINFO 则一并下发）。AAC/Opus 仍需真实 CSD。 */
        if (!(audio_legal_no_csd_player(p->audio_codec) &&
              p->audio_rate > 0 && p->audio_channels > 0)) {
            return MS_AGAIN;
        }
    }
    memset(&cfg, 0, sizeof(cfg));
    cfg.struct_size = sizeof(cfg);
    cfg.codec = (p->audio_codec != MS_CODEC_UNKNOWN) ? p->audio_codec : MS_CODEC_AAC;
    cfg.sample_fmt = p->audio_fmt;
    cfg.asc = (p->audio_asc_size > 0) ? p->audio_asc : NULL;
    cfg.asc_size = p->audio_asc_size;
    cfg.sample_rate = p->audio_rate;
    cfg.channels = p->audio_channels;
    cfg.samples_per_frame = p->audio_spf;
    cfg.pre_skip = p->audio_pre_skip;
    cfg.codec_delay = p->audio_pre_skip;
    cfg.seek_preroll = p->audio_seek_preroll;
    st = ms_hw_audio_configure(p->audio, &cfg);
    if (st != MS_OK) {
        ms_hw_audio_caps cap;
        memset(&cap, 0, sizeof(cap));
        cap.struct_size = sizeof(cap);
        (void)ms_hw_audio_get_caps(p->audio, &cap);
        set_audio_detail(p, cap.detail ? cap.detail : "audio output unsupported");
        p->audio_unsupported = 1;
        p->audio_status = st;
        close_audio(p);
        p->sync_fallback_no_audio = 1;
        return st;
    }
    st = ms_hw_audio_start(p->audio);
    if (st != MS_OK) {
        p->audio_status = st;
        p->audio_unsupported = 1;
        close_audio(p);
        p->sync_fallback_no_audio = 1;
        return st;
    }
    p->have_audio_track = 1;
    p->audio_ready = 1;
    p->audio_status = MS_OK;
    p->sync_fallback_no_audio = 0;
    if (p->rate_num > 0 && p->rate_den > 0 &&
        (p->rate_num != 1 || p->rate_den != 1)) {
        (void)ms_hw_audio_set_rate(p->audio, p->rate_num, p->rate_den);
    }
    if (p->paused) {
        (void)ms_hw_audio_pause(p->audio);
    }
    return MS_OK;
}

static ms_status feed_aac_config(ms_hw_player *p, const ms_event *ev,
                                 void (*release_fn)(void *, uint8_t *, size_t), void *release_ctx)
{
    ms_status st;
    if (!p->enable_audio) {
        p->sync_fallback_no_audio = 1;
        if (release_fn) {
            release_fn(release_ctx, (uint8_t *)(void *)(uintptr_t)ev->data, ev->data_size);
        }
        return MS_OK;
    }
    p->have_audio_track = 1;
    if (ev->track.codec != MS_CODEC_UNKNOWN) {
        p->audio_codec = ev->track.codec;
    }
#ifdef MS_SINGLE_SO
    /* 单 so 额外音频（E-AC-3/G.711/DTS/AAC-LATM）：ms_ff 用 'MSPM' 块带容器采样率/声道，
     * 只取参数，不当 csd 交给系统解码器。 */
    if ((p->audio_codec == MS_CODEC_EAC3 || p->audio_codec == MS_CODEC_G711_ALAW ||
         p->audio_codec == MS_CODEC_G711_ULAW || p->audio_codec == MS_CODEC_DTS ||
         p->audio_codec == MS_CODEC_AAC_LATM) &&
        ev->data != NULL && ev->data_size >= 12 && memcmp(ev->data, "MSPM", 4) == 0) {
        ms_audio_cfg pc;
        ms_audio_cfg_default(&pc);
        if (ms_audio_cfg_parse(MS_CODEC_PCM, MS_SAMPLE_FMT_PCM, 1, ev->data, ev->data_size, &pc) == MS_OK) {
            if (pc.sample_rate > 0) {
                p->audio_rate = pc.sample_rate;
            }
            if (pc.channels > 0) {
                p->audio_channels = pc.channels;
            }
        }
        p->audio_asc_size = 0;
        if (release_fn) {
            release_fn(release_ctx, (uint8_t *)(void *)(uintptr_t)ev->data, ev->data_size);
        }
        st = ensure_audio(p);
        if (st == MS_ERR_UNSUPPORTED) {
            return MS_OK;
        }
        return (st == MS_AGAIN) ? MS_OK : st;
    }
#endif
    if (ev->data != NULL && ev->data_size > 0) {
        ms_audio_cfg parsed;
        if (ev->data_size > sizeof(p->audio_asc)) {
            return MS_ERR_OVERFLOW;
        }
        memcpy(p->audio_asc, ev->data, ev->data_size);
        p->audio_asc_size = ev->data_size;
        ms_audio_cfg_default(&parsed);
        if (ms_audio_cfg_parse(p->audio_codec != MS_CODEC_UNKNOWN ? p->audio_codec
                                                                  : ev->track.codec,
                               ev->sample_format, 1, ev->data, ev->data_size,
                               &parsed) == MS_OK && parsed.complete) {
            if (parsed.csd_size > 0 && parsed.csd_size <= sizeof(p->audio_asc)) {
                memcpy(p->audio_asc, parsed.csd, parsed.csd_size);
                p->audio_asc_size = parsed.csd_size;
            }
            if (parsed.sample_rate > 0) {
                p->audio_rate = parsed.sample_rate;
            }
            if (parsed.channels > 0) {
                p->audio_channels = parsed.channels;
            }
            if (parsed.samples_per_frame > 0) {
                p->audio_spf = parsed.samples_per_frame;
            }
            if (parsed.pre_skip > 0) {
                p->audio_pre_skip = parsed.pre_skip;
            }
            if (parsed.seek_preroll > 0) {
                p->audio_seek_preroll = parsed.seek_preroll;
            }
        }
    }
    if (ev->sample_format != MS_SAMPLE_FMT_UNKNOWN) {
        p->audio_fmt = ev->sample_format;
    }
    if (release_fn) {
        release_fn(release_ctx, (uint8_t *)(void *)(uintptr_t)ev->data, ev->data_size);
    }
    st = ensure_audio(p);
    if (st == MS_ERR_UNSUPPORTED) {
        return MS_OK;
    }
    return (st == MS_AGAIN) ? MS_OK : st;
}

static void clear_pending_trim(ms_hw_player *p)
{
    p->have_pending_trim = 0;
    p->pending_skip_start = 0;
    p->pending_skip_end = 0;
}

static ms_status feed_aac_au(ms_hw_player *p, const ms_event *ev, int64_t origin_90k,
                             void (*release_fn)(void *, uint8_t *, size_t), void *release_ctx)
{
    ms_status st;
    int64_t pts_us = 0;
    int has_pts = 0;
    ms_hw_buf buf;
    if (!p->enable_audio || p->audio_unsupported) {
        p->audio_au_dropped_unsupported++;
        clear_pending_trim(p);
        if (release_fn) {
            release_fn(release_ctx, (uint8_t *)(void *)(uintptr_t)ev->data, ev->data_size);
        }
        return MS_OK;
    }
    if (ev->track.codec != MS_CODEC_UNKNOWN) {
        p->audio_codec = ev->track.codec;
    }
    if (ev->sample_format != MS_SAMPLE_FMT_UNKNOWN) {
        p->audio_fmt = ev->sample_format;
    }
    if (ev->data != NULL && ev->data_size > 0) {
        ms_audio_au_info info;
        ms_audio_cfg parsed;
        memset(&info, 0, sizeof(info));
        info.struct_size = sizeof(info);
        if (ms_audio_au_inspect(p->audio_codec, ev->sample_format, ev->data,
                                ev->data_size, &info) == MS_OK) {
            if (info.sample_rate > 0) {
                p->audio_rate = info.sample_rate;
            }
            if (info.channels > 0) {
                p->audio_channels = info.channels;
            }
            if (info.samples > 0) {
                p->audio_spf = info.samples;
            }
        }
        ms_audio_cfg_default(&parsed);
        if (ms_audio_cfg_parse(p->audio_codec, ev->sample_format, 0, ev->data,
                               ev->data_size, &parsed) == MS_OK && parsed.complete) {
            if (parsed.csd_size > 0 && parsed.csd_size <= sizeof(p->audio_asc) &&
                p->audio_asc_size == 0) {
                memcpy(p->audio_asc, parsed.csd, parsed.csd_size);
                p->audio_asc_size = parsed.csd_size;
            }
            if (parsed.pre_skip > 0) {
                p->audio_pre_skip = parsed.pre_skip;
            }
            if (parsed.seek_preroll > 0) {
                p->audio_seek_preroll = parsed.seek_preroll;
            }
        }
    }
    if (!p->audio_ready) {
        ms_status es = ensure_audio(p);
        if (es == MS_AGAIN) {
            /* 尚无必要 CSD（AAC/Opus）或无帧头参数：消费并丢弃，避免卡住。 */
            p->audio_au_dropped_unsupported++;
            clear_pending_trim(p);
            if (release_fn) {
                release_fn(release_ctx, (uint8_t *)(void *)(uintptr_t)ev->data, ev->data_size);
            }
            return MS_OK;
        }
        if (es == MS_ERR_UNSUPPORTED || !p->audio_ready) {
            p->audio_au_dropped_unsupported++;
            clear_pending_trim(p);
            if (release_fn) {
                release_fn(release_ctx, (uint8_t *)(void *)(uintptr_t)ev->data, ev->data_size);
            }
            return MS_OK;
        }
        if (es != MS_OK) {
            return es;
        }
    }
    st = au_to_us(ev, origin_90k, &pts_us, &has_pts);
    if (st != MS_OK) {
        return st;
    }
    memset(&buf, 0, sizeof(buf));
    buf.data = ev->data;
    buf.size = ev->data_size;
    buf.release_fn = release_fn;
    buf.release_ctx = release_ctx;
    {
        ms_hw_audio_au au;
        memset(&au, 0, sizeof(au));
        au.struct_size = sizeof(au);
        au.buf = &buf;
        au.has_pts = has_pts;
        au.pts_us = has_pts ? pts_us : 0;
        au.flags = 0;
        if (p->have_pending_trim) {
            au.skip_samples_start = p->pending_skip_start;
            au.skip_samples_end = p->pending_skip_end;
        }
        st = ms_hw_audio_submit_au(p->audio, &au);
    }
    /* 直播/起播：音频背压不得堵 demux（否则 H264+MP2 等不到 SPS/IDR）。
     * Media3 音视频分轨消费；此处丢本包继续拉视频。 */
    if (st == MS_LIMIT || st == MS_AGAIN) {
        p->audio_au_dropped_unsupported++;
        clear_pending_trim(p);
        if (release_fn) {
            release_fn(release_ctx, (uint8_t *)(void *)(uintptr_t)ev->data, ev->data_size);
        }
        return MS_OK;
    }
    clear_pending_trim(p);
    if (st != MS_OK) {
        p->audio_status = st;
        return st;
    }
    return MS_OK;
}

static void accept_csd(ms_hw_player *p, const ms_event *ev)
{
    if (ev->data == NULL || ev->data_size == 0) {
        return;
    }
    pfree(p, p->csd);
    p->csd = (uint8_t *)palloc(p, ev->data_size);
    if (p->csd == NULL) {
        p->have_csd = 0;
        p->csd_size = 0;
        p->last_error = MS_ERR_NOMEM;
        return;
    }
    memcpy(p->csd, ev->data, ev->data_size);
    p->csd_size = ev->data_size;
    p->have_csd = 1;
    p->cfg_accepted++;
    if (ev->sample_format != MS_SAMPLE_FMT_UNKNOWN) {
        p->sample_fmt = ev->sample_format;
    }
    if (ev->track.codec != MS_CODEC_UNKNOWN) {
        p->codec = ev->track.codec;
    }
}

ms_status ms_hw_player_note_event_trim(ms_hw_player *p,
                                       uint32_t skip_samples_start,
                                       uint32_t skip_samples_end)
{
    if (p == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    if (p->canceled) {
        return MS_ERR_CANCELED;
    }
    p->pending_skip_start = skip_samples_start;
    p->pending_skip_end = skip_samples_end;
    p->have_pending_trim = 1;
    return MS_OK;
}

ms_status ms_hw_player_feed(ms_hw_player *p, const ms_event *ev, int64_t origin_90k,
                            void (*release_fn)(void *ctx, uint8_t *data, size_t size),
                            void *release_ctx)
{
    ms_status st;
    unsigned flags;
    int64_t pts_us = 0;
    int has_pts = 0;
    ms_hw_buf buf;
    uint8_t *copy = NULL;
    copy_rel *cr = NULL;
    if (p == NULL || ev == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    if (p->canceled) {
        return MS_ERR_CANCELED;
    }
    if (ev->type == MS_EVENT_FORMAT || ev->type == MS_EVENT_TRACK ||
        ev->type == MS_EVENT_TRACK_UPDATE || ev->type == MS_EVENT_CODEC_CONFIG ||
        ev->type == MS_EVENT_AU || ev->type == MS_EVENT_SUBTITLE ||
        ev->type == MS_EVENT_DISCONTINUITY || ev->type == MS_EVENT_EOS) {
        mark_stage_and_req(p, &p->t_first_packet_us, &p->have_first_packet,
                           &p->t_req_first_packet_us, &p->have_req_first_packet);
    }
    if (ev->type == MS_EVENT_TRACK || ev->type == MS_EVENT_TRACK_UPDATE) {
        (void)catalog_upsert_ev(p, ev);
        if (ev->track.codec == MS_CODEC_H264 || ev->track.kind == MS_TRACK_VIDEO) {
            p->video_seen = 1;
            if (ev->sample_format != MS_SAMPLE_FMT_UNKNOWN) {
                p->sample_fmt = ev->sample_format;
            }
        }
        if (ev_is_hw_audio(ev) || ev->track.kind == MS_TRACK_AUDIO) {
            p->have_audio_track = 1;
            if (ev->track.codec != MS_CODEC_UNKNOWN) {
                p->audio_codec = ev->track.codec;
            }
            if (ev->sample_format != MS_SAMPLE_FMT_UNKNOWN) {
                p->audio_fmt = ev->sample_format;
            }
        }
        return MS_OK;
    }
    if (ev->type == MS_EVENT_CODEC_CONFIG) {
        if (ev->track.codec == MS_CODEC_SUB_DVB ||
            ev->track.codec == MS_CODEC_SUB_PGS ||
            ev->track.codec == MS_CODEC_SUB_VOBSUB) {
            ms_status hst = MS_OK;
            ms_sub_bitmap_kind kind = MS_SUB_BITMAP_DVB;
            if (ev->track.codec == MS_CODEC_SUB_PGS) {
                kind = MS_SUB_BITMAP_PGS;
            } else if (ev->track.codec == MS_CODEC_SUB_VOBSUB) {
                kind = MS_SUB_BITMAP_VOBSUB;
            }
            if (p->sub_inband == NULL) {
                hst = ms_sub_doc_create(p->alloc, &p->sub_inband);
            }
            if (hst == MS_OK && p->sub_inband != NULL && ev->data != NULL && ev->data_size > 0) {
                hst = ms_sub_doc_set_bitmap_header(p->sub_inband, kind, ev->data, ev->data_size);
            }
            if (release_fn) {
                release_fn(release_ctx, (uint8_t *)(void *)(uintptr_t)ev->data, ev->data_size);
            }
            return (hst == MS_ERR_OVERFLOW || hst == MS_ERR_NOMEM ||
                    hst == MS_ERR_INVALID_ARG) ? hst : MS_OK;
        }
        if (ev->track.codec == MS_CODEC_SUB_ASS) {
            ms_status hst = MS_OK;
            if (p->sub_inband == NULL) {
                hst = ms_sub_doc_create(p->alloc, &p->sub_inband);
            }
            if (hst == MS_OK && p->sub_inband != NULL && ev->data != NULL && ev->data_size > 0) {
                hst = ms_sub_doc_set_ass_header(p->sub_inband, ev->data, ev->data_size);
                hw_ass_lazy(p);
    hw_ai_lazy(p);
                if (p->ass != NULL) {
                    (void)ms_hw_ass_bind_set_header(p->ass, ev->data, ev->data_size);
                }
            }
            if (release_fn) {
                release_fn(release_ctx, (uint8_t *)(void *)(uintptr_t)ev->data, ev->data_size);
            }
            return (hst == MS_ERR_OVERFLOW || hst == MS_ERR_NOMEM ||
                    hst == MS_ERR_INVALID_ARG) ? hst : MS_OK;
        }
        if (ev_is_hw_audio(ev)) {
            if (!hw_emit_ok(p, ev->track.id, 1)) {
                if (release_fn) {
                    release_fn(release_ctx, (uint8_t *)(void *)(uintptr_t)ev->data,
                               ev->data_size);
                }
                return MS_OK;
            }
            return feed_aac_config(p, ev, release_fn, release_ctx);
        }
        if (ev_is_startcode_vbit(ev)) {
            ms_video_cfg vcfg;
            if (!hw_emit_ok(p, ev->track.id, 1)) {
                if (release_fn) {
                    release_fn(release_ctx, (uint8_t *)(void *)(uintptr_t)ev->data,
                               ev->data_size);
                }
                return MS_OK;
            }
            ms_video_cfg_default(&vcfg);
            if (ev->data != NULL && ev->data_size > 0 &&
                ms_video_cfg_parse(ev->track.codec, ev->sample_format, 1,
                                   ev->data, ev->data_size, &vcfg) == MS_OK &&
                vcfg.complete) {
                st = apply_video_cfg(p, &vcfg, 0);
                if (st != MS_OK) {
                    if (release_fn) {
                        release_fn(release_ctx, (uint8_t *)(void *)(uintptr_t)ev->data,
                                   ev->data_size);
                    }
                    return st;
                }
            } else if (ev->data != NULL && ev->data_size > 0) {
                accept_csd(p, ev);
                p->codec = ev->track.codec;
                if (ev->sample_format != MS_SAMPLE_FMT_UNKNOWN) {
                    p->sample_fmt = ev->sample_format;
                }
            }
            if (release_fn != NULL) {
                release_fn(release_ctx, (uint8_t *)(void *)(uintptr_t)ev->data,
                           ev->data_size);
            }
            st = ensure_decoder(p);
            if (st == MS_AGAIN || st == MS_OK) {
                return MS_OK;
            }
            return st;
        }
        if (!ev_is_video_h264(ev) && ev->track.codec != MS_CODEC_H264) {
            /* 其它非已接线视频/AAC 配置忽略，不静默丢视频。 */
            return MS_OK;
        }
        if (ev->track.codec == MS_CODEC_H264 || ev_is_video_h264(ev)) {
            int changed = 0;
            if (!hw_emit_ok(p, ev->track.id, 1)) {
                if (release_fn) {
                    release_fn(release_ctx, (uint8_t *)(void *)(uintptr_t)ev->data,
                               ev->data_size);
                }
                return MS_OK;
            }
            if (p->have_csd && ev->data != NULL && ev->data_size > 0 &&
                (p->csd_size != ev->data_size || p->csd == NULL ||
                 memcmp(p->csd, ev->data, ev->data_size) != 0)) {
                changed = 1;
            }
            /* Media3：已 configure 则忽略 CSD 字节抖，不拆硬解；仅刷新缓存。 */
            if (changed && p->configured) {
                changed = 0;
            }
            accept_csd(p, ev);
            if (!p->have_csd) {
                return p->last_error != 0 ? p->last_error : MS_ERR_NOMEM;
            }
            if (ev->sample_format == MS_SAMPLE_FMT_AVCC) {
                p->sample_fmt = MS_SAMPLE_FMT_AVCC;
                if (ev->data != NULL && ev->data_size >= 7 && ev->data[0] == 1) {
                    unsigned ls = (unsigned)((ev->data[4] & 3u) + 1u);
                    if (ls >= 1 && ls <= 4) {
                        p->avcc_nal_len = ls;
                    }
                }
            } else if (ev->sample_format == MS_SAMPLE_FMT_HVCC) {
                p->sample_fmt = MS_SAMPLE_FMT_HVCC;
                if (ev->data != NULL && ev->data_size >= 23 && ev->data[0] == 1) {
                    unsigned ls = (unsigned)((ev->data[21] & 3u) + 1u);
                    if (ls >= 1 && ls <= 4) {
                        p->hvcc_nal_len = ls;
                    }
                }
            } else if (ev->sample_format == MS_SAMPLE_FMT_ANNEXB) {
                p->sample_fmt = MS_SAMPLE_FMT_ANNEXB;
            }
            if (p->codec == MS_CODEC_HEVC || ev->track.codec == MS_CODEC_HEVC) {
                ms_hevc_ps_set cfg;
                memset(&cfg, 0, sizeof(cfg));
                cfg.struct_size = sizeof(cfg);
                if (ms_hevc_ps_parse(p->sample_fmt, 1, ev->data, ev->data_size,
                                     p->hvcc_nal_len, &cfg) == MS_OK &&
                    cfg.triple_complete) {
                    p->applied_hevc = cfg;
                    p->have_applied_hevc = 1;
                    p->codec = MS_CODEC_HEVC;
                    clear_ps_pending(p);
                }
            } else {
                ms_h264_ps_set cfg;
                memset(&cfg, 0, sizeof(cfg));
                cfg.struct_size = sizeof(cfg);
                if (ms_h264_ps_parse(p->sample_fmt, 1, ev->data, ev->data_size,
                                     p->avcc_nal_len, &cfg) == MS_OK &&
                    cfg.pair_complete) {
                    p->applied_ps = cfg;
                    p->have_applied_ps = 1;
                    clear_ps_pending(p);
                }
            }
            if (release_fn != NULL) {
                release_fn(release_ctx, (uint8_t *)(void *)(uintptr_t)ev->data,
                           ev->data_size);
            }
            if (p->csd_resubmit && p->configured) {
                st = resubmit_cached_video_csd(p);
                if (st != MS_OK && st != MS_LIMIT && st != MS_AGAIN) {
                    return st;
                }
            }
            st = ensure_decoder(p);
            if (st == MS_AGAIN || st == MS_OK) {
                return MS_OK;
            }
            return st;
        }
        return MS_OK;
    }
    if (ev->type == MS_EVENT_SUBTITLE) {
        return feed_subtitle_ev(p, ev, release_fn, release_ctx);
    }
    if (ev->type == MS_EVENT_DISCONTINUITY) {
        p->waiting_key = (p->opts.wait_keyframe != 0);
        return MS_OK;
    }
    if (ev->type == MS_EVENT_EOS) {
        return ms_hw_player_signal_eos(p);
    }
    if (ev->type != MS_EVENT_AU) {
        return MS_OK;
    }
    if (ev_is_hw_audio(ev)) {
        if (!hw_emit_ok(p, ev->track.id, 1)) {
            p->audio_au_dropped_unselected++;
            clear_pending_trim(p);
            if (release_fn) {
                release_fn(release_ctx, (uint8_t *)(void *)(uintptr_t)ev->data, ev->data_size);
            }
            return MS_OK;
        }
        hw_note_media_time(p, ev);
        return feed_aac_au(p, ev, origin_90k, release_fn, release_ctx);
    }
    if (!ev_is_video_h264(ev)) {
        return MS_OK;
    }
    if (!hw_emit_ok(p, ev->track.id, 1)) {
        if (release_fn) {
            release_fn(release_ctx, (uint8_t *)(void *)(uintptr_t)ev->data, ev->data_size);
        }
        return MS_OK;
    }
    /* 轨 codec 以事件为准（Media3 Format.sampleMimeType），避免仍按默认 H264 配解码器。 */
    if (ev->track.codec != MS_CODEC_UNKNOWN) {
        p->codec = ev->track.codec;
    }
    {
        int drop_inband = 0;
        int video_hevc = (ev->track.codec == MS_CODEC_HEVC || p->codec == MS_CODEC_HEVC);
        int video_vbit = ev_is_startcode_vbit(ev) ||
                         p->codec == MS_CODEC_MPEG2_VIDEO ||
                         p->codec == MS_CODEC_MPEG4_PART2 ||
                         p->codec == MS_CODEC_H263 ||
                         p->codec == MS_CODEC_AV1 ||
                         p->codec == MS_CODEC_AVS_PLUS;
        if (video_vbit) {
            st = handle_inband_vbit(p, ev, &drop_inband);
        } else if (video_hevc) {
            st = handle_inband_hevc(p, ev, &drop_inband);
        } else {
            st = handle_inband_ps(p, ev, &drop_inband);
        }
        if (st != MS_OK) {
            return st;
        }
        if (drop_inband) {
            p->video_au_in++;
            p->video_au_dropped_nokey++;
            if (release_fn != NULL) {
                release_fn(release_ctx, (uint8_t *)(void *)(uintptr_t)ev->data,
                           ev->data_size);
            }
            return MS_OK;
        }
    }
    /* 仅在消费成功（提交或门闩/preroll 丢弃）时增加 video_au_in。
     * LIMIT/AGAIN/错误所有权仍在调用方，不得重复计数。 */
    if (ev->preroll) {
        p->video_au_in++;
        p->video_au_dropped_preroll++;
        if (release_fn != NULL) {
            release_fn(release_ctx, (uint8_t *)(void *)(uintptr_t)ev->data, ev->data_size);
        }
        return MS_OK;
    }
    st = ensure_decoder(p);
    if (st == MS_AGAIN) {
        /* Media3：Format 未就绪（无 CSD / 无宽高）则丢样继续拉流，禁止 AGAIN 堵泵。 */
        if (!p->have_csd || p->width <= 0 || p->height <= 0) {
            p->video_au_in++;
            p->video_au_dropped_nokey++;
            if (release_fn != NULL) {
                release_fn(release_ctx, (uint8_t *)(void *)(uintptr_t)ev->data,
                           ev->data_size);
            }
            return MS_OK;
        }
        /* 有 Format 无 Surface：非关键帧丢掉免堵泵；关键帧保留等 Surface。 */
        if (p->surface == NULL && p->waiting_key && !ev->is_keyframe) {
            p->video_au_in++;
            p->video_au_dropped_nokey++;
            if (release_fn != NULL) {
                release_fn(release_ctx, (uint8_t *)(void *)(uintptr_t)ev->data,
                           ev->data_size);
            }
            return MS_OK;
        }
        return MS_AGAIN;
    }
    if (st != MS_OK) {
        /* Media3：codec init 失败不终结 demux——清掉坏 Format，等下一组参数集。 */
        if (!p->configured &&
            (st == MS_ERR_UNKNOWN || st == MS_ERR_UNSUPPORTED)) {
            clear_ps_pending(p);
            pfree(p, p->csd);
            p->csd = NULL;
            p->csd_size = 0;
            p->have_csd = 0;
            p->have_applied_ps = 0;
            p->have_applied_hevc = 0;
            p->width = 0;
            p->height = 0;
            p->video_au_in++;
            p->video_au_dropped_nokey++;
            if (release_fn != NULL) {
                release_fn(release_ctx, (uint8_t *)(void *)(uintptr_t)ev->data,
                           ev->data_size);
            }
            return MS_OK;
        }
        return st;
    }
    {
        int is_key = ev->is_keyframe;
        if (!is_key && ev_is_startcode_vbit(ev) && ev->data != NULL && ev->data_size > 0) {
            ms_video_au_info info;
            memset(&info, 0, sizeof(info));
            info.struct_size = sizeof(info);
            if (ms_video_au_inspect(ev->track.codec, ev->sample_format, ev->data,
                                    ev->data_size, &info) == MS_OK &&
                info.is_key) {
                is_key = 1;
            }
        }
        /* FAST_START 后 lavf 常不置 KEY；按 NAL 扫 IDR，与 HEVC IRAP 对称。 */
        if (!is_key && (ev->track.codec == MS_CODEC_H264 || p->codec == MS_CODEC_H264) &&
            ev->data != NULL && ev->data_size > 4) {
            const uint8_t *d = ev->data;
            size_t n = ev->data_size, i = 0;
            ms_sample_format sf = ev->sample_format != MS_SAMPLE_FMT_UNKNOWN
                                     ? ev->sample_format
                                     : p->sample_fmt;
            if (sf == MS_SAMPLE_FMT_AVCC) {
                unsigned ls = p->avcc_nal_len ? p->avcc_nal_len : 4u;
                while (i + ls < n) {
                    unsigned sz = 0, j;
                    for (j = 0; j < ls; j++) {
                        sz = (sz << 8) | d[i + j];
                    }
                    i += ls;
                    if (sz == 0 || i + sz > n) {
                        break;
                    }
                    if ((d[i] & 0x1f) == 5) {
                        is_key = 1;
                        break;
                    }
                    i += sz;
                }
            } else {
                while (i + 4 <= n) {
                    size_t sc, nal;
                    if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1) {
                        sc = 3;
                    } else if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 0 &&
                               d[i + 3] == 1) {
                        sc = 4;
                    } else {
                        i++;
                        continue;
                    }
                    nal = i + sc;
                    if (nal < n && (d[nal] & 0x1f) == 5) {
                        is_key = 1;
                        break;
                    }
                    i = nal + 1;
                }
            }
        }
        if (!is_key && (ev->track.codec == MS_CODEC_HEVC || p->codec == MS_CODEC_HEVC) &&
            ev->data != NULL && ev->data_size > 0) {
            ms_hevc_au_info info;
            memset(&info, 0, sizeof(info));
            info.struct_size = sizeof(info);
            if (ms_hevc_au_inspect(ev->sample_format != MS_SAMPLE_FMT_UNKNOWN
                                       ? ev->sample_format
                                       : p->sample_fmt,
                                   ev->data, ev->data_size,
                                   p->hvcc_nal_len ? p->hvcc_nal_len : 4u,
                                   &info) == MS_OK &&
                info.is_irap) {
                is_key = 1;
            }
        }
        if (p->waiting_key && !is_key) {
            p->video_au_in++;
            p->video_au_dropped_nokey++;
            if (release_fn != NULL) {
                release_fn(release_ctx, (uint8_t *)(void *)(uintptr_t)ev->data, ev->data_size);
            }
            return MS_OK;
        }
        flags = is_key ? MS_HW_FLAG_KEY : 0;
    }
    /* 位置只跟将提交的视频 AU，不跟等待关键帧期间被丢掉的包。 */
    hw_note_media_time(p, ev);
    if (p->csd_resubmit) {
        st = resubmit_cached_video_csd(p);
        if (st != MS_OK) {
            return st;
        }
    }
    mark_stage_and_req(p, &p->t_first_decodable_au_us, &p->have_first_decodable_au,
                       &p->t_req_first_decodable_au_us, &p->have_req_first_decodable_au);
    st = au_to_us(ev, origin_90k, &pts_us, &has_pts);
    if (st != MS_OK) {
        return st;
    }
    memset(&buf, 0, sizeof(buf));
    if (release_fn != NULL) {
        buf.data = ev->data;
        buf.size = ev->data_size;
        buf.release_fn = release_fn;
        buf.release_ctx = release_ctx;
    } else if (ev->data != NULL && ev->data_size > 0) {
        copy = (uint8_t *)palloc(p, ev->data_size);
        cr = (copy_rel *)palloc(p, sizeof(*cr));
        if (copy == NULL || cr == NULL) {
            pfree(p, copy);
            pfree(p, cr);
            return MS_ERR_NOMEM;
        }
        memcpy(copy, ev->data, ev->data_size);
        cr->alloc = p->alloc;
        cr->data = copy;
        buf.data = copy;
        buf.size = ev->data_size;
        buf.release_fn = copy_rel_fn;
        buf.release_ctx = cr;
        p->staging_copy_ops++;
        p->staging_copy_bytes += ev->data_size;
    }
    st = ms_hw_decoder_submit(p->dec, &buf, has_pts ? pts_us : 0, flags);
    if (st != MS_OK) {
        /* 未入队：收回本次拷贝，引用路径所有权仍在调用方。 */
        if (cr != NULL) {
            copy_rel_fn(cr, copy, ev->data_size);
        }
        if (st == MS_LIMIT) {
            p->video_au_backpressure++;
            return MS_LIMIT;
        }
        p->last_error = st;
        return st;
    }
    p->video_au_in++;
    p->video_au_submitted++;
    if (flags & MS_HW_FLAG_KEY) {
        p->waiting_key = 0;
    }
    mark_stage_and_req(p, &p->t_first_submit_us, &p->have_first_submit,
                       &p->t_req_first_submit_us, &p->have_req_first_submit);
    return MS_OK;
}

static int audio_clock_now(ms_hw_player *p, int64_t *clock_us)
{
    if (p->audio == NULL || p->audio_unsupported || !p->enable_audio) {
        return 0;
    }
    if (ms_hw_audio_get_clock_us(p->audio, clock_us) != MS_OK) {
        return 0;
    }
    /* 尚无已播放帧时不当作有效音频时钟。 */
    return *clock_us > 0;
}

#ifdef __ANDROID__
static int64_t sync_mono_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (int64_t)ts.tv_sec * 1000000000ll + (int64_t)ts.tv_nsec;
}

/* VideoFrameReleaseHelper.adjustReleaseTime 精简：
 * 1) 帧节奏平滑：按上一帧调整后的释放时刻 + PTS 差推算，偏差 ≤20ms 才采用（无帧率估计器，用 PTS 差）；
 * 2) VSYNC 吸附：取最近的 vsync，再提前 80% 个周期交出去（落在目标 vsync 之前、上一 vsync 之后）。
 *    Media3 的 60fps@90Hz 等滞回逻辑未移植。 */
static int64_t sync_adjust_release(ms_hw_player *p, int64_t release_ns, int64_t pts_us,
                                   double speed)
{
    int64_t adj = release_ns;
    int64_t vt = 0, per = 0;
    if (p->vfr_have && pts_us > p->vfr_last_pts_us) {
        int64_t cand = p->vfr_last_adj_ns +
                       (int64_t)((double)(pts_us - p->vfr_last_pts_us) * 1000.0 / speed);
        int64_t diff = release_ns - cand;
        if (diff < 0) {
            diff = -diff;
        }
        if (diff <= MS_HW_VFR_MAX_ADJ_NS) {
            adj = cand;
        } else {
            p->vfr_have = 0; /* resetAdjustment */
        }
    }
    if (ms_hw_vsync_get(&vt, &per) && per > 0) {
        int64_t cnt = (adj - vt) / per;
        int64_t snapped = vt + per * cnt;
        int64_t before, after;
        if (adj <= snapped) {
            before = snapped - per;
            after = snapped;
        } else {
            before = snapped;
            after = snapped + per;
        }
        snapped = (after - adj < adj - before) ? after : before;
        return snapped - (per * MS_HW_VSYNC_OFFSET_PCT) / 100;
    }
    return adj;
}

/*
 * 出帧决策：对齐 Media3 VideoFrameReleaseControl.getFrameReleaseActionInternal +
 * MediaCodecVideoRenderer 的 FrameTimingEvaluator。顺序与 Media3 一致：
 *   强制释放（首帧 / 迟到且 >100ms 没出过帧）→ 计算 releaseTimeNs 与 VSYNC 吸附 →
 *   严重落后丢到关键帧 → 迟到丢帧 → 超前 >50ms 稍后 → 定时释放。
 * 保留既有行为：首帧不等音频；暂停时只在需要出“当前帧”（首帧/Surface 重绑）时出一帧，否则握住；
 * 无音频钟（无音轨/音频不支持/关声/尚未出声）时直接出帧。
 */
static ms_hw_release_action sync_release_decide(void *ctx, int64_t pts_us, int is_eos,
                                                int can_schedule, int64_t *release_ns)
{
    ms_hw_player *p = (ms_hw_player *)ctx;
    int64_t clock = 0;
    int64_t now_ns;
    int64_t early_us;
    double speed = 1.0;
    if (p == NULL) {
        return MS_HW_REL_NOW;
    }
    now_ns = sync_mono_ns();
    if (!p->first_video_presented) {
        /* Media3 FIRST_FRAME_NOT_RENDERED：强制立即释放；同轮内防重复首帧。 */
        p->first_video_presented = 1;
        p->sync_holding = 0;
        p->sync_last_release_ns = now_ns;
        p->vfr_have = 0;
        return MS_HW_REL_NOW;
    }
    if (p->paused) {
        p->sync_holding = 1; /* 暂停：已有画面，不再推进 */
        return MS_HW_REL_LATER;
    }
    if (!audio_clock_now(p, &clock)) {
        p->sync_holding = 0;
        p->sync_last_release_ns = now_ns;
        return MS_HW_REL_NOW;
    }
    if (p->rate_num > 0 && p->rate_den > 0) {
        speed = (double)p->rate_num / (double)p->rate_den;
    }
    /* calculateEarlyTimeUs：(帧时间 - 播放位置) / 速度；clock 在本函数内刚取，无需扣循环耗时。 */
    early_us = (int64_t)((double)(pts_us - clock) / speed);
    p->sync_last_early_us = early_us;
    /* shouldForceRelease（FIRST_FRAME_RENDERED 分支）→ shouldForceRenderOutputBuffer */
    if (early_us < -MS_HW_SYNC_LATE_US &&
        now_ns - p->sync_last_release_ns > (int64_t)MS_HW_SYNC_FORCE_GAP_US * 1000ll) {
        p->video_au_late++;
        p->sync_holding = 0;
        p->sync_last_release_ns = now_ns;
        p->vfr_have = 0;
        return MS_HW_REL_NOW;
    }
    if (can_schedule) {
        int64_t rel = sync_adjust_release(p, now_ns + early_us * 1000ll, pts_us, speed);
        early_us = (rel - now_ns) / 1000ll;
        *release_ns = rel;
    }
    if (!is_eos && early_us < -MS_HW_SYNC_VERY_LATE_US) {
        /* shouldDropBuffersToKeyframe：Media3 跳过源到下一关键帧并 flush 解码器。
         * 解码器回调里不能 flush 自己，置位后由 ms_hw_player_drive 处理。 */
        p->sync_drop_to_key = 1;
        p->video_au_dropped_late++;
        p->sync_holding = 0;
        return MS_HW_REL_DROP;
    }
    if (!is_eos && early_us < -MS_HW_SYNC_LATE_US) {
        p->video_au_dropped_late++; /* shouldDropOutputBuffer：不渲染直接还给解码器 */
        p->sync_holding = 0;
        return MS_HW_REL_DROP;
    }
    if (early_us > (can_schedule ? MS_HW_SYNC_EARLY_US : MS_HW_SYNC_LEGACY_EARLY_US)) {
        if (pts_us != p->sync_last_held_pts) {
            p->video_au_held++; /* 每个缓冲只计一次，不按轮询次数计 */
            p->sync_last_held_pts = pts_us;
        }
        p->sync_holding = 1;
        return MS_HW_REL_LATER;
    }
    p->sync_holding = 0;
    p->sync_last_release_ns = now_ns;
    if (can_schedule) {
        p->vfr_last_adj_ns = *release_ns;
        p->vfr_last_pts_us = pts_us;
        p->vfr_have = 1;
        p->video_release_scheduled++;
        return MS_HW_REL_AT;
    }
    return MS_HW_REL_NOW;
}
#endif

static void present_video(ms_hw_player *p, int late)
{
    mark_stage_and_req(p, &p->t_first_output_us, &p->have_first_output,
                       &p->t_req_first_output_us, &p->have_req_first_output);
    p->first_video_presented = 1;
    {
        struct timespec ts;
        if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
            p->last_video_output_ns = (int64_t)ts.tv_sec * 1000000000ll + (int64_t)ts.tv_nsec;
        }
    }
    p->need_surface_resync = 0;
    if (late) {
        p->video_au_late++;
    }
}

static void present_video_pts(ms_hw_player *p, int late, int64_t pts_us, int has_pts)
{
    if (has_pts) {
        p->last_video_pts_us = pts_us;
        p->have_last_video_pts = 1;
    }
    present_video(p, late);
}

static int enqueue_vpend(ms_hw_player *p, int64_t pts_us, int has_pts)
{
    unsigned i;
    if (p->vpend_n >= 8) {
        return 0;
    }
    for (i = 0; i < 8; i++) {
        if (!p->vpend[i].occupied) {
            p->vpend[i].pts_us = pts_us;
            p->vpend[i].has_pts = has_pts;
            p->vpend[i].occupied = 1;
            p->vpend_n++;
            return 1;
        }
    }
    return 0;
}

static void drain_vpend(ms_hw_player *p)
{
    int64_t clock = 0;
    int have_clock = audio_clock_now(p, &clock);
    unsigned i;
    p->sync_fallback_no_audio = (!have_clock && (p->audio == NULL || p->audio_unsupported || !p->enable_audio));
    if (p->sync_in_decoder) {
        /* 时机已由 sync_release_decide 在 releaseOutputBuffer 前决定，这里只记账。 */
        for (i = 0; i < 8; i++) {
            if (p->vpend[i].occupied) {
                present_video_pts(p, 0, p->vpend[i].has_pts ? p->vpend[i].pts_us : 0,
                                  p->vpend[i].has_pts);
                p->vpend[i].occupied = 0;
                p->vpend_n--;
            }
        }
        return;
    }
    for (i = 0; i < 8; i++) {
        int late = 0;
        int64_t pts;
        if (!p->vpend[i].occupied) {
            continue;
        }
        pts = p->vpend[i].has_pts ? p->vpend[i].pts_us : 0;
        if (!p->first_video_presented || p->paused) {
            /* 快速首帧：不等待音频预滚。暂停态重绑也立即出当前帧，不按音频钟等待。
             * 已有画面时暂停不再把队列里后续帧一次倒完。 */
            if (!p->first_video_presented) {
                present_video_pts(p, 0, pts, p->vpend[i].has_pts);
                p->vpend[i].occupied = 0;
                p->vpend_n--;
            }
            continue;
        }
        if (!have_clock) {
            present_video_pts(p, 0, pts, p->vpend[i].has_pts);
            p->vpend[i].occupied = 0;
            p->vpend_n--;
            continue;
        }
        if (p->vpend[i].has_pts && pts + MS_HW_SYNC_LATE_US < clock) {
            late = 1;
            present_video_pts(p, 1, pts, p->vpend[i].has_pts);
            p->vpend[i].occupied = 0;
            p->vpend_n--;
            continue;
        }
        if (p->vpend[i].has_pts && pts > clock + MS_HW_SYNC_EARLY_US) {
            p->video_au_held++;
            continue;
        }
        present_video_pts(p, late, pts, p->vpend[i].has_pts);
        p->vpend[i].occupied = 0;
        p->vpend_n--;
    }
}

ms_status ms_hw_player_drive(ms_hw_player *p)
{
    ms_status st = MS_OK;
    if (p == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    if (p->canceled) {
        return MS_ERR_CANCELED;
    }
    if (p->paused) {
        if (p->ai != NULL) {
            (void)ms_hw_ai_bind_drive(p->ai);
        }
        /* 暂停不推进音频，但仍 drive 视频解码器并尽快出一帧：
         * Surface 重绑 / seek 后不能因 paused 早退导致黑屏。 */
    } else if (p->audio != NULL && !p->audio_unsupported) {
        st = ms_hw_audio_drive(p->audio);
        if (st != MS_OK && st != MS_AGAIN) {
            p->audio_status = st;
            /* 音频失败不把视频路径一并打成 last_error，除非是取消。 */
            if (st == MS_ERR_CANCELED) {
                p->last_error = st;
                return st;
            }
        }
    }
    if (p->ai != NULL && !p->paused) {
        (void)ms_hw_ai_bind_drive(p->ai);
    }
    if (p->dec != NULL) {
        st = ms_hw_decoder_drive(p->dec);
        if (st != MS_OK && st != MS_AGAIN) {
            p->last_error = st;
            return st;
        }
        for (;;) {
            ms_hw_event ev;
            ms_status pst;
            memset(&ev, 0, sizeof(ev));
            ev.struct_size = sizeof(ev);
            pst = ms_hw_decoder_pull_event(p->dec, &ev);
            if (pst == MS_AGAIN) {
                break;
            }
            if (pst != MS_OK) {
                return pst;
            }
            if (ev.type == MS_HW_EVENT_OUTPUT_RENDERED) {
                if (!enqueue_vpend(p, ev.pts, ev.has_pts)) {
                    /* 待呈现队列满：丢掉最迟策略——立即呈现并记 dropped_late。 */
                    p->video_au_dropped_late++;
                    present_video_pts(p, 1, ev.pts, ev.has_pts);
                }
            } else if (ev.type == MS_HW_EVENT_EOS) {
                p->eos_out = 1;
            } else if (ev.type == MS_HW_EVENT_ERROR) {
                p->last_error = ev.error != 0 ? ev.error : MS_ERR_MALFORMED;
                return p->last_error;
            }
        }
    }
    drain_vpend(p);
    if (p->sync_drop_to_key && p->dec != NULL) {
        /* Media3 maybeDropBuffersToKeyframe：严重落后（<-500ms）时 flush 解码器并等下一个关键帧。 */
        p->sync_drop_to_key = 0;
        (void)ms_hw_decoder_flush(p->dec);
        clear_vpend(p);
        sync_reset(p);
        p->waiting_key = 1;
        p->video_drop_to_keyframe++;
    }
#ifdef __ANDROID__
    /* 每 5s 一行同步统计到 logcat（tag MsSync），供模拟器/真机核对；不打印 URL。 */
    if (p->sync_in_decoder && !p->paused) {
        int64_t now = sync_mono_ns();
        if (p->sync_log_ns == 0) {
            p->sync_log_ns = now;
        } else if (now - p->sync_log_ns >= 5000000000ll) {
            int64_t clk = 0, lat = 0, vt = 0, per = 0;
            int src = 0;
            int hc = audio_clock_now(p, &clk);
            if (p->audio != NULL) {
                ms_hw_audio_sync_info(p->audio, &src, &lat);
            }
            if (p->vsync_acquired) {
                (void)ms_hw_vsync_get(&vt, &per);
            }
            p->sync_log_ns = now;
            __android_log_print(ANDROID_LOG_INFO, "MsSync",
                                "av_sync_delta_us=%lld early_us=%lld clock_src=%d latency_us=%lld "
                                "vsync_ns=%lld held=%llu dropped_late=%llu forced_late=%llu "
                                "drop_to_key=%llu scheduled=%llu",
                                (long long)((hc && p->have_last_video_pts) ? p->last_video_pts_us - clk : 0),
                                (long long)p->sync_last_early_us, src, (long long)lat, (long long)per,
                                (unsigned long long)p->video_au_held,
                                (unsigned long long)p->video_au_dropped_late,
                                (unsigned long long)p->video_au_late,
                                (unsigned long long)p->video_drop_to_keyframe,
                                (unsigned long long)p->video_release_scheduled);
        }
    }
#endif
    /* 首帧之后若解码器输入已满且 2s 无新输出，冲洗等待关键帧，避免 goldfish
     * 卡死 8 个 pending 后整条推送停住。不是把软件解报成真机硬解。
     * 解码器正按音频钟握着“太早”的帧时不算卡死（视频超前音频 >2s 也不能误冲）。 */
    if (!p->paused && p->first_video_presented && p->dec != NULL &&
        p->last_video_output_ns > 0 && !p->sync_holding) {
        struct timespec ts;
        if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
            int64_t now = (int64_t)ts.tv_sec * 1000000000ll + (int64_t)ts.tv_nsec;
            unsigned pend = 0;
            ms_hw_stats ds;
            memset(&ds, 0, sizeof(ds));
            ds.struct_size = sizeof(ds);
            if (ms_hw_decoder_get_stats(p->dec, &ds) == MS_OK) {
                pend = (unsigned)ds.pending_in;
            }
            if (pend >= 8u && now - p->last_video_output_ns >= 2000000000ll) {
                (void)ms_hw_decoder_flush(p->dec);
                clear_vpend(p);
                p->waiting_key = 1;
                p->last_video_output_ns = now;
            }
        }
    }
    /* 空转 AGAIN 对调用方视为本轮已处理完毕；真错误已提前返回。 */
    if (st == MS_AGAIN) {
        st = MS_OK;
    }
    return st;
}

ms_status ms_hw_player_pull_event(ms_hw_player *p, ms_hw_event *out)
{
    if (p == NULL || out == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    if (p->dec == NULL) {
        return MS_AGAIN;
    }
    return ms_hw_decoder_pull_event(p->dec, out);
}

ms_status ms_hw_player_signal_eos(ms_hw_player *p)
{
    ms_status st;
    if (p == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    if (p->canceled) {
        return MS_ERR_CANCELED;
    }
    p->eos_in = 1;
    if (p->audio != NULL && !p->audio_unsupported) {
        (void)ms_hw_audio_signal_eos(p->audio);
    }
    if (p->dec == NULL || !p->configured || !p->started) {
        /* 仅音频或尚未完成视频 configure 时，EOS 仍应收束，不把未启动解码器报成 STATE。 */
        p->eos_out = 1;
        drain_vpend(p);
        return MS_OK;
    }
    st = ms_hw_decoder_signal_eos(p->dec);
    if (st != MS_OK) {
        p->last_error = st;
        return st;
    }
    return ms_hw_player_drive(p);
}

ms_status ms_hw_player_flush(ms_hw_player *p)
{
    ms_status st = MS_OK;
    if (p == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    if (p->canceled) {
        return MS_ERR_CANCELED;
    }
    if (p->dec != NULL) {
        st = ms_hw_decoder_flush(p->dec);
        /* 仅 set_surface、尚未 configure 时 flush 可能 STATE；对播放器视为空转成功。 */
        if (st == MS_ERR_STATE && !p->configured) {
            st = MS_OK;
        }
    }
    if (p->audio != NULL && !p->audio_unsupported) {
        ms_status ast = ms_hw_audio_flush(p->audio);
        if (st == MS_OK) {
            st = ast;
        }
    }
    clear_vpend(p);
    sync_reset(p); /* flush 后解码器握着的 index 已失效 */
    p->have_last_video_pts = 0;
    p->last_video_pts_us = -1;
    clear_ps_pending(p);
    clear_pending_trim(p);
    if (p->sub_inband != NULL) {
        ms_sub_doc_clear_cues(p->sub_inband);
        if (p->ass != NULL) {
            (void)ms_hw_ass_bind_clear(p->ass);
        }
    }
    if (p->ai != NULL) {
        ms_hw_ai_bind_bump_timeline(p->ai);
        ms_hw_ai_bind_flush(p->ai);
    }
    p->first_video_presented = 0;
    p->waiting_key = (p->opts.wait_keyframe != 0);
    p->eos_in = 0;
    p->eos_out = 0;
    p->flushed = 1;
    /* 解码器仍配置着：seek/flush 后先重送 CSD，再交关键帧。 */
    if (p->configured && p->have_csd && p->csd != NULL && p->csd_size > 0) {
        p->csd_resubmit = 1;
    }
    p->epoch++;
    reset_stage_times(p);
    if (p->paused && p->audio != NULL && !p->audio_unsupported) {
        (void)ms_hw_audio_pause(p->audio);
    }
    return st;
}

ms_status ms_hw_player_cancel(ms_hw_player *p)
{
    if (p == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    p->canceled = 1;
    close_decoder(p);
    if (p->audio != NULL) {
        (void)ms_hw_audio_cancel(p->audio);
    }
    close_audio(p);
    return MS_OK;
}

ms_status ms_hw_player_release_decoder(ms_hw_player *p)
{
    if (p == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    close_decoder(p);
    close_audio(p);
    clear_vpend(p);
    if (p->sub_inband != NULL) {
        ms_sub_doc_clear_cues(p->sub_inband);
        if (p->ass != NULL) {
            (void)ms_hw_ass_bind_clear(p->ass);
        }
    }
    p->first_video_presented = 0;
    p->waiting_key = (p->opts.wait_keyframe != 0);
    p->eos_in = 0;
    p->eos_out = 0;
    p->epoch++;
    p->surface = NULL;
    return MS_OK;
}

ms_status ms_hw_player_get_stats(const ms_hw_player *p, ms_hw_player_stats *out)
{
    ms_hw_stats st;
    ms_hw_caps caps;
    size_t in_sz;
    ms_hw_player_stats tmp;
    static const size_t k_v1_min =
        offsetof(ms_hw_player_stats, last_error) + sizeof(((ms_hw_player_stats *)0)->last_error);
    if (p == NULL || out == NULL || out->struct_size < k_v1_min) {
        return MS_ERR_INVALID_ARG;
    }
    in_sz = out->struct_size;
    if (in_sz > sizeof(tmp)) {
        in_sz = sizeof(tmp);
    }
    memset(&tmp, 0, sizeof(tmp));
    tmp.struct_size = in_sz;
    tmp.t_start_us = p->t_start_us;
    tmp.t_first_packet_us = p->t_first_packet_us;
    tmp.t_first_decodable_au_us = p->t_first_decodable_au_us;
    tmp.t_first_submit_us = p->t_first_submit_us;
    tmp.t_first_output_us = p->t_first_output_us;
    tmp.t_first_display_us = p->t_first_display_us;
    tmp.t_first_codec_rendered_us = p->t_first_codec_rendered_us;
    tmp.have_first_packet = p->have_first_packet;
    tmp.have_first_decodable_au = p->have_first_decodable_au;
    tmp.have_first_submit = p->have_first_submit;
    tmp.have_first_output = p->have_first_output;
    tmp.have_first_display = p->have_first_display;
    tmp.have_first_codec_rendered = p->have_first_codec_rendered;
    tmp.codec_rendered_is_not_display = 1;
    tmp.display_evidence = p->have_first_display ? "app_note_display" : "none";
    tmp.cfg_accepted = p->cfg_accepted;
    tmp.video_au_in = p->video_au_in;
    tmp.video_au_dropped_nokey = p->video_au_dropped_nokey;
    tmp.video_au_dropped_preroll = p->video_au_dropped_preroll;
    tmp.video_au_submitted = p->video_au_submitted;
    tmp.video_au_backpressure = p->video_au_backpressure;
    tmp.staging_copy_ops = p->staging_copy_ops;
    tmp.staging_copy_bytes = p->staging_copy_bytes;
    tmp.waiting_keyframe = p->waiting_key;
    tmp.configured = p->configured;
    tmp.started = p->started;
    tmp.eos_in = p->eos_in;
    tmp.eos_out = p->eos_out;
    tmp.canceled = p->canceled;
    tmp.flushed = p->flushed;
    tmp.epoch = p->epoch;
    tmp.output_is_not_display = 1;
    tmp.last_error = p->last_error;
    tmp.backend = "none";
    tmp.kind = MS_HW_KIND_NONE;
    if (p->dec != NULL) {
        memset(&st, 0, sizeof(st));
        st.struct_size = sizeof(st);
        if (ms_hw_decoder_get_stats(p->dec, &st) == MS_OK) {
            if (st.have_first_codec_rendered && !p->have_first_codec_rendered &&
                st.t_first_codec_rendered_us >= 0) {
                /* decoder 存绝对 CLOCK_MONOTONIC µs；分别换算到 t_start 与 t_request */
                {
                    int64_t abs_us = st.t_first_codec_rendered_us;
                    mark_one(abs_us, p->t_start_us, &((ms_hw_player *)p)->t_first_codec_rendered_us,
                             &((ms_hw_player *)p)->have_first_codec_rendered);
                    if (p->have_request) {
                        mark_one(abs_us, p->t_request_us,
                                 &((ms_hw_player *)p)->t_req_first_codec_rendered_us,
                                 &((ms_hw_player *)p)->have_req_first_codec_rendered);
                    }
                }
                tmp.t_first_codec_rendered_us = p->t_first_codec_rendered_us;
                tmp.have_first_codec_rendered = 1;
                tmp.t_req_first_codec_rendered_us = p->t_req_first_codec_rendered_us;
                tmp.have_req_first_codec_rendered = p->have_req_first_codec_rendered;
            }
            tmp.ref_hold_ops = st.ref_hold_ops;
            tmp.platform_copy_ops = st.platform_copy_ops;
            tmp.platform_copy_bytes = st.platform_copy_bytes;
            tmp.staging_copy_ops += st.staging_copy_ops;
            tmp.staging_copy_bytes += st.staging_copy_bytes;
            tmp.pending_in = st.pending_in;
        }
        memset(&caps, 0, sizeof(caps));
        caps.struct_size = sizeof(caps);
        if (ms_hw_decoder_get_caps(p->dec, &caps) == MS_OK) {
            tmp.kind = caps.kind;
            /* mock 即使模拟“选到硬件组件”也不得报设备硬解。 */
            if (caps.backend != NULL && strcmp(caps.backend, "mock") == 0) {
                tmp.is_hardware = 0;
            } else {
                tmp.is_hardware = caps.is_hardware;
            }
            tmp.backend = caps.backend;
        }
    }
    tmp.t_request_us = p->t_request_us;
    tmp.have_request = p->have_request;
    tmp.t_req_first_packet_us = p->t_req_first_packet_us;
    tmp.t_req_first_decodable_au_us = p->t_req_first_decodable_au_us;
    tmp.t_req_first_submit_us = p->t_req_first_submit_us;
    tmp.t_req_first_output_us = p->t_req_first_output_us;
    tmp.t_req_first_display_us = p->t_req_first_display_us;
    tmp.t_req_first_codec_rendered_us = p->t_req_first_codec_rendered_us;
    tmp.have_req_first_packet = p->have_req_first_packet;
    tmp.have_req_first_decodable_au = p->have_req_first_decodable_au;
    tmp.have_req_first_submit = p->have_req_first_submit;
    tmp.have_req_first_output = p->have_req_first_output;
    tmp.have_req_first_display = p->have_req_first_display;
    tmp.have_req_first_codec_rendered = p->have_req_first_codec_rendered;
    tmp.t_worker_start_us = p->t_worker_start_us;
    tmp.have_worker_start = p->have_worker_start;
    tmp.t_req_worker_start_us = p->t_req_worker_start_us;
    tmp.t_req_open_input_done_us = p->t_req_open_input_done_us;
    tmp.t_req_find_stream_info_done_us = p->t_req_find_stream_info_done_us;
    tmp.dur_open_input_us = p->dur_open_input_us;
    tmp.dur_find_stream_info_us = p->dur_find_stream_info_us;
    tmp.have_open_phases = p->have_open_phases;
    tmp.audio_status = p->audio_status;
    tmp.audio_unsupported = p->audio_unsupported;
    tmp.have_audio = p->have_audio_track;
    tmp.audio_detail = p->audio_detail_buf[0] ? p->audio_detail_buf : NULL;
    tmp.audio_au_dropped_unsupported = p->audio_au_dropped_unsupported;
    tmp.video_au_late = p->video_au_late;
    tmp.video_au_held = p->video_au_held;
    tmp.video_au_dropped_late = p->video_au_dropped_late;
    tmp.sync_fallback_no_audio = p->sync_fallback_no_audio;
    tmp.video_csd_inband = p->video_csd_inband;
    tmp.video_csd_inband_same = p->video_csd_inband_same;
    tmp.video_csd_pending_abandoned = p->video_csd_pending_abandoned;
    tmp.selection_epoch = p->selection_epoch;
    tmp.audio_track_id = hw_selected_id(p, MS_TRACK_AUDIO);
    tmp.sub_track_id = hw_selected_id(p, MS_TRACK_SUBTITLE);
    tmp.audio_switch_count = p->audio_switch_count;
    tmp.audio_au_dropped_unselected = p->audio_au_dropped_unselected;
    tmp.have_external_sub = (hw_find_track(p, MS_TRACK_ID_EXTERNAL_SUB) >= 0);
    tmp.paused = p->paused;
    tmp.user_paused = p->user_paused;
    tmp.audio_focus = p->audio_focus;
    tmp.surface_bound = (p->surface != NULL);
    tmp.rate_num = p->rate_num > 0 ? p->rate_num : 1;
    tmp.rate_den = p->rate_den > 0 ? p->rate_den : 1;
    tmp.seekable = p->seekable;
    tmp.duration_90k = p->duration_90k;
    tmp.position_90k = 0; /* 下面在 last_media 赋值后再填，占位 */
    tmp.timeline_epoch = p->timeline_epoch;
    tmp.last_seek_request_90k = p->last_seek_request_90k;
    tmp.last_seek_land_90k = p->last_seek_land_90k;
    tmp.have_seek_land = p->have_seek_land;
    tmp.audio_backend = "none";
    tmp.audio_kind = MS_HW_KIND_NONE;
    tmp.audio_is_hardware = 0;
    if (p->audio != NULL) {
        ms_hw_audio_stats as;
        ms_hw_audio_caps ac;
        memset(&as, 0, sizeof(as));
        as.struct_size = sizeof(as);
        if (ms_hw_audio_get_stats(p->audio, &as) == MS_OK) {
            tmp.audio_configured = as.configured;
            tmp.audio_au_in = as.au_in;
            tmp.audio_au_submitted = as.au_submitted;
            tmp.audio_au_played = as.au_played;
            tmp.audio_backpressure = as.backpressure;
            tmp.audio_partial_writes = as.partial_writes;
            tmp.audio_samples_played = as.samples_played;
            tmp.audio_clock_us = as.clock_us;
            tmp.audio_sample_rate = as.sample_rate;
            tmp.audio_channels = as.channels;
            tmp.audio_pcm_format_changes = as.pcm_format_changes;
            tmp.audio_is_hardware = as.is_hardware;
            tmp.audio_system_decode_is_not_hardware = as.system_decode_is_not_hardware;
            tmp.audio_kind = as.kind;
            tmp.audio_backend = as.backend;
            tmp.sync_has_audio_clock = (as.clock_us > 0);
            tmp.audio_track_write_bytes = as.track_write_bytes;
            tmp.audio_track_write_frames = as.track_write_frames;
            tmp.audio_track_head_frames = as.track_head_frames;
            if (as.detail && p->audio_detail_buf[0] == 0) {
                set_audio_detail((ms_hw_player *)p, as.detail);
                tmp.audio_detail = p->audio_detail_buf;
            }
        }
        memset(&ac, 0, sizeof(ac));
        ac.struct_size = sizeof(ac);
        if (ms_hw_audio_get_caps(p->audio, &ac) == MS_OK) {
            if (ac.backend != NULL && strcmp(ac.backend, "mock") == 0) {
                tmp.audio_is_hardware = 0;
            }
        }
    }
    if (p->have_last_media) {
        tmp.position_90k = (int64_t)p->last_media_90k;
    } else if (tmp.sync_has_audio_clock && tmp.audio_clock_us > 0) {
        tmp.position_90k = (tmp.audio_clock_us * 90000ll) / 1000000ll;
    } else {
        tmp.position_90k = 0;
    }
    tmp.video_last_pts_us = p->have_last_video_pts ? p->last_video_pts_us : -1;
    tmp.have_video_last_pts = p->have_last_video_pts;
    tmp.have_av_sync = 0;
    tmp.av_sync_delta_us = 0;
    if (p->have_last_video_pts && tmp.sync_has_audio_clock) {
        tmp.av_sync_delta_us = p->last_video_pts_us - tmp.audio_clock_us;
        tmp.have_av_sync = 1;
    }
    tmp.need_keyframe_resync = p->need_surface_resync ? 1 : 0;
    tmp.hdr = p->hdr;
    tmp.hdr.struct_size = sizeof(tmp.hdr);
    ms_hdr_classify(&tmp.hdr);
    /* 阶段 8：Media3 对齐的同步统计（加法字段）。 */
    tmp.sync_in_decoder = p->sync_in_decoder;
    tmp.video_release_scheduled = p->video_release_scheduled;
    tmp.video_drop_to_keyframe = p->video_drop_to_keyframe;
    tmp.video_last_early_us = p->sync_last_early_us;
    tmp.audio_clock_source = 0;
    tmp.audio_latency_us = 0;
    tmp.vsync_period_ns = 0;
    if (p->audio != NULL) {
        (void)ms_hw_audio_sync_info(p->audio, &tmp.audio_clock_source, &tmp.audio_latency_us);
    }
#ifdef __ANDROID__
    {
        int64_t vt = 0;
        if (p->vsync_acquired) {
            (void)ms_hw_vsync_get(&vt, &tmp.vsync_period_ns);
        }
    }
#endif
    memcpy(out, &tmp, in_sz);
    out->struct_size = in_sz;
    return MS_OK;
}

ms_status ms_hw_player_get_caps(const ms_hw_player *p, ms_hw_caps *out)
{
    if (p == NULL || out == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    if (out->struct_size < sizeof(*out)) {
        return MS_ERR_INVALID_ARG;
    }
    if (p->dec == NULL) {
        size_t sz = out->struct_size;
        memset(out, 0, sizeof(*out));
        out->struct_size = sz;
        out->kind = MS_HW_KIND_NONE;
        out->backend = "none";
        out->verified = 1;
        out->is_hardware = 0;
        return MS_OK;
    }
    return ms_hw_decoder_get_caps(p->dec, out);
}

ms_status ms_hw_player_note_display(ms_hw_player *p, int64_t pts, int has_pts)
{
    (void)pts;
    (void)has_pts;
    if (p == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    if (p->canceled) {
        return MS_ERR_CANCELED;
    }
    /* 不从 output 事件推断；只信应用回调。 */
    mark_stage_and_req(p, &p->t_first_display_us, &p->have_first_display,
                       &p->t_req_first_display_us, &p->have_req_first_display);
    return MS_OK;
}

/* ===== P4 helpers and APIs appended by this slice ===== */

#ifndef MS_HW_TRACK_MAX
#define MS_HW_TRACK_MAX 32
#endif

static int sub_codec_playable(ms_codec c)
{
    return c == MS_CODEC_SUB_TEXT || c == MS_CODEC_SUB_SRT ||
           c == MS_CODEC_SUB_WEBVTT || c == MS_CODEC_SUB_TX3G ||
           c == MS_CODEC_SUB_ASS || c == MS_CODEC_SUB_DVB ||
           c == MS_CODEC_SUB_PGS || c == MS_CODEC_SUB_VOBSUB;
}

static void hw_copy_z(char *dst, size_t cap, const char *src)
{
    size_t n;
    if (dst == NULL || cap == 0) {
        return;
    }
    dst[0] = 0;
    if (src == NULL) {
        return;
    }
    n = strlen(src);
    if (n >= cap) {
        n = cap - 1u;
    }
    memcpy(dst, src, n);
    dst[n] = 0;
}

static int hw_find_track(const ms_hw_player *p, uint32_t id)
{
    int i;
    if (p == NULL || p->tracks == NULL) {
        return -1;
    }
    for (i = 0; i < p->ntracks; i++) {
        if (p->tracks[i].in_use && p->tracks[i].id == id) {
            return i;
        }
    }
    return -1;
}

static uint32_t hw_selected_id(const ms_hw_player *p, ms_track_kind kind)
{
    int i;
    if (p == NULL || p->tracks == NULL) {
        return 0;
    }
    for (i = 0; i < p->ntracks; i++) {
        if (p->tracks[i].in_use && p->tracks[i].kind == kind && p->tracks[i].selected) {
            return p->tracks[i].id;
        }
    }
    return 0;
}

static int hw_emit_ok(const ms_hw_player *p, uint32_t id, int default_if_unknown)
{
    int i;
    if (p == NULL || p->tracks == NULL || p->ntracks == 0 || id == 0) {
        return default_if_unknown;
    }
    i = hw_find_track(p, id);
    if (i < 0) {
        return default_if_unknown;
    }
    return p->tracks[i].selected;
}

static void reset_audio_session(ms_hw_player *p)
{
    if (p->audio != NULL) {
        (void)ms_hw_audio_flush(p->audio);
        ms_hw_audio_destroy(p->audio);
        p->audio = NULL;
    }
    p->audio_ready = 0;
    p->audio_asc_size = 0;
    memset(p->audio_asc, 0, sizeof(p->audio_asc));
    p->audio_codec = MS_CODEC_UNKNOWN;
    p->audio_rate = 0;
    p->audio_channels = 0;
    p->audio_spf = 0;
    p->audio_pre_skip = 0;
    p->audio_seek_preroll = 0;
    p->audio_fmt = MS_SAMPLE_FMT_UNKNOWN;
    /* 切轨后重新诊断，不把旧轨的 UNSUPPORTED 带到新轨。 */
    p->audio_unsupported = 0;
    p->audio_status = 0;
    p->sync_fallback_no_audio = 0;
    p->audio_detail_buf[0] = 0;
    if (p->ai != NULL) {
        ms_hw_ai_bind_bump_selection(p->ai);
    }
}

static uint64_t ev_ts_90k(const ms_event *ev, uint64_t t)
{
    int num;
    int den;
    if (ev == NULL) {
        return 0;
    }
    num = ev->time_base_num > 0 ? ev->time_base_num : 1;
    den = ev->time_base_den > 0 ? ev->time_base_den : 90000;
    if (num == 1 && den == 90000) {
        return t;
    }
#if defined(__SIZEOF_INT128__)
    return (uint64_t)((__int128)t * (__int128)90000 * (__int128)num / (__int128)den);
#else
    return (uint64_t)(((double)t * 90000.0 * (double)num) / (double)den);
#endif
}

static void bump_selection_epoch(ms_hw_player *p)
{
    p->selection_epoch++;
    if (p->selection_epoch == 0) {
        p->selection_epoch = 1;
    }
}

static int hw_alloc_tracks(ms_hw_player *p)
{
    if (p->tracks != NULL) {
        return 0;
    }
    p->tracks = (ms_hw_trk *)palloc(p, sizeof(ms_hw_trk) * MS_HW_TRACK_MAX);
    if (p->tracks == NULL) {
        return -1;
    }
    memset(p->tracks, 0, sizeof(ms_hw_trk) * MS_HW_TRACK_MAX);
    p->ntracks = 0;
    p->cap_tracks = MS_HW_TRACK_MAX;
    return 0;
}

static int hw_any_selected(const ms_hw_player *p, ms_track_kind kind)
{
    return hw_selected_id(p, kind) != 0;
}

static int catalog_upsert_ev(ms_hw_player *p, const ms_event *ev)
{
    int i;
    int is_new = 0;
    ms_hw_trk *tr;
    uint32_t prev_audio;
    uint32_t now_audio;
    if (p == NULL || ev == NULL || ev->track.id == 0) {
        return 0;
    }
    prev_audio = hw_selected_id(p, MS_TRACK_AUDIO);
    if (hw_alloc_tracks(p) != 0) {
        return -1;
    }
    i = hw_find_track(p, ev->track.id);
    if (i < 0) {
        if (p->ntracks >= p->cap_tracks) {
            return -1;
        }
        i = p->ntracks++;
        memset(&p->tracks[i], 0, sizeof(p->tracks[i]));
        is_new = 1;
    }
    tr = &p->tracks[i];
    tr->in_use = 1;
    tr->id = ev->track.id;
    tr->kind = ev->track.kind;
    tr->codec = ev->track.codec;
    tr->level = ev->track.level;
    tr->is_default = ev->track.is_default;
    tr->is_forced = ev->track.is_forced;
    hw_copy_z(tr->language, sizeof(tr->language), ev->track.language);
    hw_copy_z(tr->name, sizeof(tr->name), ev->track.name);
    if (is_new) {
        if (ev->track.selected) {
            tr->selected = 1;
        } else if (tr->kind == MS_TRACK_VIDEO && !hw_any_selected(p, MS_TRACK_VIDEO)) {
            tr->selected = 1;
        } else if (tr->kind == MS_TRACK_AUDIO && !p->user_audio_sel &&
                   !hw_any_selected(p, MS_TRACK_AUDIO)) {
            tr->selected = 1;
        } else {
            tr->selected = 0;
        }
    } else if (!p->user_audio_sel && tr->kind == MS_TRACK_AUDIO) {
        tr->selected = ev->track.selected;
    } else if (!p->user_sub_sel && tr->kind == MS_TRACK_SUBTITLE &&
               tr->id != MS_TRACK_ID_AI_SUB) {
        tr->selected = ev->track.selected;
    } else if (!p->user_video_sel && tr->kind == MS_TRACK_VIDEO) {
        tr->selected = ev->track.selected;
    }
    /* 同一 kind 互斥：新选中时关掉其它。 */
    if (tr->selected) {
        int j;
        for (j = 0; j < p->ntracks; j++) {
            if (j == i || !p->tracks[j].in_use || p->tracks[j].kind != tr->kind) {
                continue;
            }
            if (p->tracks[j].id == MS_TRACK_ID_AI_SUB) {
                continue;
            }
            if (tr->kind == MS_TRACK_VIDEO || tr->kind == MS_TRACK_AUDIO ||
                tr->kind == MS_TRACK_SUBTITLE) {
                p->tracks[j].selected = 0;
            }
        }
    }
    now_audio = hw_selected_id(p, MS_TRACK_AUDIO);
    if (prev_audio != 0 && now_audio != prev_audio) {
        reset_audio_session(p);
        bump_selection_epoch(p);
        p->audio_switch_count++;
        if (now_audio == 0) {
            p->sync_fallback_no_audio = 1;
        }
    }
    return 0;
}

static void hw_note_media_time(ms_hw_player *p, const ms_event *ev)
{
    if (ev != NULL && ev->has_pts) {
        p->last_media_90k = ev_ts_90k(ev, ev->pts);
        p->have_last_media = 1;
    }
}

static ms_status feed_subtitle_ev(ms_hw_player *p, const ms_event *ev,
                                  void (*release_fn)(void *, uint8_t *, size_t),
                                  void *release_ctx)
{
    ms_status st = MS_OK;
    ms_sub_cue cue;
    uint64_t t1;
    /* 未选中的字幕也入缓存；显示由 subtitles_at 按当前选中轨过滤。
     * 稀疏文字轨往往只有开场一包，若这里丢掉，切轨后画面会一直空。 */
    if (ev->data == NULL || ev->data_size == 0) {
        if (release_fn) {
            release_fn(release_ctx, (uint8_t *)(void *)(uintptr_t)ev->data, ev->data_size);
        }
        return MS_OK;
    }
    if (p->sub_inband == NULL) {
        st = ms_sub_doc_create(p->alloc, &p->sub_inband);
        if (st != MS_OK) {
            if (release_fn) {
                release_fn(release_ctx, (uint8_t *)(void *)(uintptr_t)ev->data, ev->data_size);
            }
            return st;
        }
    }
    memset(&cue, 0, sizeof(cue));
    cue.struct_size = sizeof(cue);
    cue.track_id = ev->track.id != 0 ? ev->track.id : 1u;
    cue.has_start = ev->has_pts;
    cue.start_90k = ev_ts_90k(ev, ev->has_pts ? ev->pts : 0);
    t1 = ev->dts;
    if (t1 <= (ev->has_pts ? ev->pts : 0ull)) {
        int den = ev->time_base_den > 0 ? ev->time_base_den : 90000;
        int num = ev->time_base_num > 0 ? ev->time_base_num : 1;
        t1 = (ev->has_pts ? ev->pts : 0ull) + (uint64_t)den * 2ull / (uint64_t)num;
    }
    cue.has_end = 1;
    cue.end_90k = ev_ts_90k(ev, t1);
    if (ev->track.codec == MS_CODEC_SUB_DVB ||
        ev->track.codec == MS_CODEC_SUB_PGS ||
        ev->track.codec == MS_CODEC_SUB_VOBSUB) {
        ms_sub_bitmap_kind kind = MS_SUB_BITMAP_DVB;
        if (ev->track.codec == MS_CODEC_SUB_PGS) {
            kind = MS_SUB_BITMAP_PGS;
        } else if (ev->track.codec == MS_CODEC_SUB_VOBSUB) {
            kind = MS_SUB_BITMAP_VOBSUB;
        }
        st = ms_sub_doc_feed_bitmap(p->sub_inband, kind, cue.track_id,
                                    ev->data, ev->data_size,
                                    cue.has_start, cue.start_90k,
                                    cue.has_end, cue.end_90k);
        if (release_fn) {
            release_fn(release_ctx, (uint8_t *)(void *)(uintptr_t)ev->data, ev->data_size);
        }
        if (st == MS_ERR_OVERFLOW || st == MS_ERR_MALFORMED || st == MS_ERR_UNSUPPORTED) {
            return MS_OK;
        }
        return st;
    }
    if (ev->track.codec == MS_CODEC_SUB_ASS) {
        char parsed[MS_SUB_CUE_TEXT_MAX + 1];
        ms_status pst;
        hw_ass_lazy(p);
        if (p->ass != NULL && ev->data != NULL && ev->data_size > 0 &&
            hw_emit_ok(p, ev->track.id, 1)) {
            (void)ms_hw_ass_bind_add_event(p->ass, ev->data, ev->data_size,
                                           cue.has_start || cue.has_end,
                                           (int64_t)cue.start_90k, (int64_t)cue.end_90k);
        }
        pst = ms_sub_ass_parse_event(p->sub_inband, ev->data, ev->data_size,
                                     cue.has_start, cue.start_90k, 1, cue.end_90k,
                                     &cue, parsed, sizeof(parsed));
        if (pst != MS_OK || cue.text_size == 0) {
            if (release_fn) {
                release_fn(release_ctx, (uint8_t *)(void *)(uintptr_t)ev->data, ev->data_size);
            }
            return MS_OK;
        }
        /* add_cue 会复制 text；parsed 在本函数结束前仍有效。 */
        st = ms_sub_doc_add_cue(p->sub_inband, &cue);
    } else {
        cue.text = (const char *)ev->data;
        cue.text_size = ev->data_size;
        st = ms_sub_doc_add_cue(p->sub_inband, &cue);
    }
    if (release_fn) {
        release_fn(release_ctx, (uint8_t *)(void *)(uintptr_t)ev->data, ev->data_size);
    }
    if (st == MS_ERR_OVERFLOW) {
        /* 已有条目保留；本条丢弃，不让字幕把播放打死。 */
        return MS_OK;
    }
    return st;
}

static void hw_fill_info(const ms_hw_player *p, int i, ms_track_info *out)
{
    memset(out, 0, sizeof(*out));
    if (p == NULL || p->tracks == NULL || i < 0 || i >= p->ntracks) {
        return;
    }
    out->id = p->tracks[i].id;
    out->kind = p->tracks[i].kind;
    out->codec = p->tracks[i].codec;
    out->level = p->tracks[i].level;
    out->language = p->tracks[i].language[0] ? p->tracks[i].language : NULL;
    out->name = p->tracks[i].name[0] ? p->tracks[i].name : NULL;
    out->is_default = p->tracks[i].is_default;
    out->is_forced = p->tracks[i].is_forced;
    out->selected = p->tracks[i].selected;
}

ms_status ms_hw_player_list_tracks(const ms_hw_player *p, ms_track_info *out,
                                   size_t cap, size_t *count)
{
    size_t n = 0;
    int i;
    if (p == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    if (p->tracks != NULL) {
        for (i = 0; i < p->ntracks; i++) {
            if (!p->tracks[i].in_use) {
                continue;
            }
            if (out != NULL && n < cap) {
                hw_fill_info(p, i, &out[n]);
            }
            n++;
        }
    }
    if (count != NULL) {
        *count = n;
    }
    return MS_OK;
}

ms_status ms_hw_player_select_track(ms_hw_player *p, uint32_t track_id, int on)
{
    int tgt;
    int j;
    ms_track_kind kind;
    uint32_t prev_audio;
    if (p == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    if (p->canceled) {
        return MS_ERR_CANCELED;
    }
    tgt = hw_find_track(p, track_id);
    if (tgt < 0) {
        return MS_ERR_INVALID_ARG;
    }
    kind = p->tracks[tgt].kind;
    if (on && kind == MS_TRACK_SUBTITLE && !sub_codec_playable(p->tracks[tgt].codec) &&
        track_id != MS_TRACK_ID_EXTERNAL_SUB && track_id != MS_TRACK_ID_AI_SUB) {
        return MS_ERR_UNSUPPORTED;
    }
    if (track_id == MS_TRACK_ID_AI_SUB) {
        if ((on && p->tracks[tgt].selected) || (!on && !p->tracks[tgt].selected)) {
            return MS_OK;
        }
        if (on && (p->ai == NULL || !ms_hw_ai_bind_enabled(p->ai))) {
            return MS_ERR_STATE;
        }
        p->user_ai_sel = on ? 1 : 0;
        p->tracks[tgt].selected = on ? 1 : 0;
        return MS_OK;
    }
    if ((on && p->tracks[tgt].selected) || (!on && !p->tracks[tgt].selected)) {
        return MS_OK;
    }
    prev_audio = hw_selected_id(p, MS_TRACK_AUDIO);
    if (kind == MS_TRACK_AUDIO) {
        p->user_audio_sel = 1;
    }
    if (kind == MS_TRACK_SUBTITLE) {
        p->user_sub_sel = 1;
    }
    if (kind == MS_TRACK_VIDEO) {
        p->user_video_sel = 1;
    }
    bump_selection_epoch(p);
    for (j = 0; j < p->ntracks; j++) {
        int want;
        if (!p->tracks[j].in_use || p->tracks[j].kind != kind) {
            continue;
        }
        if (p->tracks[j].id == MS_TRACK_ID_AI_SUB) {
            continue;
        }
        want = on && p->tracks[j].id == track_id;
        p->tracks[j].selected = want;
    }
    if (kind == MS_TRACK_AUDIO) {
        reset_audio_session(p);
        p->audio_switch_count++;
        if (!on || hw_selected_id(p, MS_TRACK_AUDIO) == 0) {
            p->sync_fallback_no_audio = 1;
        }
        (void)prev_audio;
    }
    if (kind == MS_TRACK_VIDEO && on) {
        close_decoder(p);
        p->waiting_key = (p->opts.wait_keyframe != 0);
        p->eos_in = 0;
        p->eos_out = 0;
        p->epoch++;
    }
    if (kind == MS_TRACK_SUBTITLE) {
        /* 改选不清 inband 缓存：稀疏文字/ASS 包可能已经读过。
         * 显示按选中轨过滤。ASS 栅格绑定仍清掉，避免旧轨画面叠在新轨上。 */
        if (p->ass != NULL) {
            (void)ms_hw_ass_bind_clear(p->ass);
        }
    }
    return MS_OK;
}

ms_status ms_hw_player_set_external_subtitle(ms_hw_player *p, const uint8_t *data,
                                             size_t size, ms_sub_format fmt)
{
    ms_status st;
    int i;
    if (p == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    if (p->canceled) {
        return MS_ERR_CANCELED;
    }
    if (data == NULL || size == 0) {
        i = hw_find_track(p, MS_TRACK_ID_EXTERNAL_SUB);
        if (i >= 0) {
            if (p->tracks[i].selected) {
                bump_selection_epoch(p);
            }
            memset(&p->tracks[i], 0, sizeof(p->tracks[i]));
        }
        if (p->sub_ext != NULL) {
            ms_sub_doc_destroy(p->sub_ext);
            p->sub_ext = NULL;
        }
        return MS_OK;
    }
    if (p->sub_ext == NULL) {
        st = ms_sub_doc_create(p->alloc, &p->sub_ext);
        if (st != MS_OK) {
            return st;
        }
    }
    st = ms_sub_doc_parse(p->sub_ext, data, size, fmt, MS_TRACK_ID_EXTERNAL_SUB);
    if (st != MS_OK) {
        return st;
    }
    {
        ms_sub_format got = (fmt == MS_SUB_FORMAT_AUTO) ? ms_sub_doc_format(p->sub_ext) : fmt;
        if (got == MS_SUB_FORMAT_ASS || got == MS_SUB_FORMAT_SSA) {
            hw_ass_lazy(p);
            if (p->ass != NULL) {
                (void)ms_hw_ass_bind_set_script(p->ass, data, size);
            }
        }
    }
    if (hw_alloc_tracks(p) != 0) {
        return MS_ERR_NOMEM;
    }
    i = hw_find_track(p, MS_TRACK_ID_EXTERNAL_SUB);
    if (i < 0) {
        if (p->ntracks >= p->cap_tracks) {
            return MS_ERR_OVERFLOW;
        }
        i = p->ntracks++;
        memset(&p->tracks[i], 0, sizeof(p->tracks[i]));
    }
    p->tracks[i].in_use = 1;
    p->tracks[i].id = MS_TRACK_ID_EXTERNAL_SUB;
    p->tracks[i].kind = MS_TRACK_SUBTITLE;
    p->tracks[i].codec = ms_sub_codec_for_format(
        fmt == MS_SUB_FORMAT_AUTO ? ms_sub_doc_format(p->sub_ext) : fmt);
    p->tracks[i].level = MS_RECOG_AU;
    hw_copy_z(p->tracks[i].name, sizeof(p->tracks[i].name), "external");
    /* 默认不自动选中；若已选中则保留。 */
    return MS_OK;
}

ms_status ms_hw_player_subtitles_at(const ms_hw_player *p, uint64_t time_90k,
                                    ms_sub_cue *out, size_t cap, size_t *count)
{
    size_t n = 0, add = 0;
    int i;
    int sel_ext = 0;
    int sel_in = 0;
    ms_status st;
    if (p == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    i = hw_find_track(p, MS_TRACK_ID_EXTERNAL_SUB);
    sel_ext = (i >= 0 && p->tracks[i].selected);
    if (sel_ext && p->sub_ext != NULL) {
        st = ms_sub_doc_cues_at(p->sub_ext, time_90k, out, cap, &add);
        if (st != MS_OK) {
            return st;
        }
        n += add;
    }
    if (p->tracks != NULL) {
        for (i = 0; i < p->ntracks; i++) {
            if (p->tracks[i].in_use && p->tracks[i].kind == MS_TRACK_SUBTITLE &&
                p->tracks[i].selected && p->tracks[i].id != MS_TRACK_ID_EXTERNAL_SUB) {
                sel_in = 1;
                break;
            }
        }
    }
    if (sel_in && p->sub_inband != NULL) {
        ms_sub_cue *dst = out;
        size_t left = cap;
        if (out != NULL && n < cap) {
            dst = out + n;
            left = cap - n;
        } else {
            dst = NULL;
            left = 0;
        }
        {
            ms_sub_cue tmp[32];
            uint32_t sid = hw_selected_id(p, MS_TRACK_SUBTITLE);
            size_t r, w, got = 0;
            for (r = 0; r < 32; r++) {
                ms_sub_cue_default(&tmp[r]);
            }
            st = ms_sub_doc_cues_at(p->sub_inband, time_90k, tmp, 32, &got);
            if (st != MS_OK) {
                return st;
            }
            if (got > 32) {
                got = 32;
            }
            w = 0;
            for (r = 0; r < got; r++) {
                if (tmp[r].track_id != sid) {
                    continue;
                }
                if (dst != NULL && w < left) {
                    dst[w] = tmp[r];
                }
                w++;
            }
            add = w;
        }
        n += add;
    }
    {
        int sel_ai = 0;
        ms_sub_doc *adoc;
        i = hw_find_track(p, MS_TRACK_ID_AI_SUB);
        sel_ai = (i >= 0 && p->tracks[i].selected);
        adoc = ms_hw_ai_bind_doc(p->ai);
        if (sel_ai && adoc != NULL) {
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
            st = ms_sub_doc_cues_at(adoc, time_90k, dst, left, &add);
            if (st != MS_OK) {
                return st;
            }
            n += add;
        }
    }
    if (count != NULL) {
        *count = n;
    }
    (void)sel_ext;
    return MS_OK;
}

uint64_t ms_hw_player_last_media_90k(const ms_hw_player *p)
{
    if (p == NULL || !p->have_last_media) {
        return 0;
    }
    return p->last_media_90k;
}


static int player_rate_cmp(int n1, int d1, int n2, int d2)
{
#if defined(__SIZEOF_INT128__)
    __int128 a = (__int128)n1 * d2;
    __int128 b = (__int128)n2 * d1;
    return (a < b) ? -1 : (a > b) ? 1 : 0;
#else
    double a = (double)n1 / (double)d1;
    double b = (double)n2 / (double)d2;
    if (a < b) {
        return -1;
    }
    if (a > b) {
        return 1;
    }
    return 0;
#endif
}

static int player_is_host_mock(const ms_hw_player *p)
{
#ifdef __ANDROID__
    (void)p;
    return 0;
#else
    (void)p;
    return 1;
#endif
}

ms_status ms_hw_player_get_rate_caps(const ms_hw_player *p, ms_hw_rate_caps *out)
{
    size_t sz;
    ms_hw_rate_caps tmp;
    if (p == NULL || out == NULL || out->struct_size < sizeof(size_t)) {
        return MS_ERR_INVALID_ARG;
    }
    if (p->audio != NULL && !p->audio_unsupported) {
        ms_status st = ms_hw_audio_get_rate_caps(p->audio, out);
        return st;
    }
    memset(&tmp, 0, sizeof(tmp));
    tmp.struct_size = sizeof(tmp);
    tmp.can_apply = 1;
    if (player_is_host_mock(p)) {
        tmp.can_sync_av = 0;
        tmp.can_keep_pitch = 0;
        tmp.min_num = 1;
        tmp.min_den = 8;
        tmp.max_num = 8;
        tmp.max_den = 1;
        tmp.api_level = 0;
        tmp.mock_is_not_device = 1;
        tmp.backend = "mock";
        tmp.detail = "host mock rate bookkeeping only; not device A/V-synced speed";
    } else {
        /* 尚无音频会话：不得把只改视频时钟报成音画同步倍速。 */
        int api = p->opts.decoder.api_level;
        tmp.can_sync_av = 0;
        tmp.can_keep_pitch = 0;
        tmp.min_num = 1;
        tmp.min_den = 1;
        tmp.max_num = 1;
        tmp.max_den = 1;
        tmp.api_level = api;
        tmp.mock_is_not_device = 0;
        tmp.backend = "none";
        tmp.detail = "no audio session; rate other than 1x would be video-clock-only and is UNSUPPORTED";
    }
    sz = out->struct_size;
    if (sz > sizeof(tmp)) {
        sz = sizeof(tmp);
    }
    memcpy(out, &tmp, sz);
    out->struct_size = sz;
    return MS_OK;
}

ms_status ms_hw_player_set_rate(ms_hw_player *p, int num, int den)
{
    ms_hw_rate_caps caps;
    ms_status st;
    if (p == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    if (num <= 0 || den <= 0) {
        return MS_ERR_INVALID_ARG;
    }
    memset(&caps, 0, sizeof(caps));
    caps.struct_size = sizeof(caps);
    st = ms_hw_player_get_rate_caps(p, &caps);
    if (st != MS_OK) {
        return st;
    }
    if (player_rate_cmp(num, den, caps.min_num, caps.min_den) < 0 ||
        player_rate_cmp(num, den, caps.max_num, caps.max_den) > 0) {
        return MS_ERR_UNSUPPORTED;
    }
    if ((num != caps.min_num || den != caps.min_den) &&
        player_rate_cmp(num, den, 1, 1) != 0 &&
        !caps.can_sync_av && !caps.mock_is_not_device) {
        return MS_ERR_UNSUPPORTED;
    }
    if (p->audio != NULL && !p->audio_unsupported) {
        st = ms_hw_audio_set_rate(p->audio, num, den);
        if (st != MS_OK) {
            return st;
        }
    }
    p->rate_num = num;
    p->rate_den = den;
    return MS_OK;
}

ms_status ms_hw_player_get_rate(const ms_hw_player *p, int *num, int *den)
{
    if (p == NULL || num == NULL || den == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    *num = p->rate_num > 0 ? p->rate_num : 1;
    *den = p->rate_den > 0 ? p->rate_den : 1;
    return MS_OK;
}

static void apply_pause_state(ms_hw_player *p)
{
    int now = (p->user_paused || p->focus_paused) ? 1 : 0;
    int was = p->paused;
    p->paused = now;
    if (p->audio == NULL || p->audio_unsupported) {
        return;
    }
    if (now && !was) {
        (void)ms_hw_audio_pause(p->audio);
    } else if (!now && was) {
        (void)ms_hw_audio_resume(p->audio);
    }
}

ms_status ms_hw_player_pause(ms_hw_player *p)
{
    if (p == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    p->user_paused = 1;
    apply_pause_state(p);
    return MS_OK;
}

ms_status ms_hw_player_resume(ms_hw_player *p)
{
    if (p == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    p->user_paused = 0;
    apply_pause_state(p);
    return MS_OK;
}

int ms_hw_player_is_paused(const ms_hw_player *p)
{
    return p != NULL && p->paused;
}

int ms_hw_player_is_user_paused(const ms_hw_player *p)
{
    return p != NULL && p->user_paused;
}

ms_status ms_hw_player_notify_audio_focus(ms_hw_player *p, int focus_event)
{
    if (p == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    switch (focus_event) {
    case MS_HW_AUDIO_FOCUS_GAIN:
        p->audio_focus = focus_event;
        p->focus_paused = 0;
        apply_pause_state(p);
        return MS_OK;
    case MS_HW_AUDIO_FOCUS_LOSS:
    case MS_HW_AUDIO_FOCUS_LOSS_TRANSIENT:
    case MS_HW_AUDIO_FOCUS_LOSS_TRANSIENT_CAN_DUCK:
        /* CAN_DUCK 暂按短暂丢失暂停，不做音量闪避。 */
        p->audio_focus = focus_event;
        p->focus_paused = 1;
        apply_pause_state(p);
        return MS_OK;
    default:
        return MS_ERR_INVALID_ARG;
    }
}

ms_status ms_hw_player_note_timeline(ms_hw_player *p, int seekable, int64_t duration_90k)
{
    if (p == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    if (seekable < -1 || seekable > 1) {
        return MS_ERR_INVALID_ARG;
    }
    p->seekable = seekable;
    p->duration_90k = duration_90k < 0 ? -1 : duration_90k;
    /* 旧入口：有正时长视为 VOD；seekable 且无时长不把 0 冒充窗口。 */
    if (seekable == 0) {
        p->seek_kind = MS_HW_SEEK_KIND_UNSEEKABLE;
        p->win_start_90k = 0;
        p->win_end_90k = -1;
        p->live_edge_90k = -1;
        p->safety_90k = 0;
    } else if (p->duration_90k >= 0) {
        p->seek_kind = MS_HW_SEEK_KIND_VOD;
        p->win_start_90k = 0;
        p->win_end_90k = p->duration_90k;
        p->live_edge_90k = p->duration_90k;
        p->safety_90k = 0;
    }
    return MS_OK;
}

static int64_t clamp_player_target(const ms_hw_player *p, int64_t time_90k)
{
    if (p->win_start_90k > 0 && time_90k < p->win_start_90k) {
        time_90k = p->win_start_90k;
    }
    if (p->live_edge_90k >= 0 && time_90k > p->live_edge_90k) {
        time_90k = p->live_edge_90k;
    } else if (p->seek_kind == MS_HW_SEEK_KIND_VOD &&
               p->duration_90k >= 0 && time_90k > p->duration_90k) {
        time_90k = p->duration_90k;
    }
    return time_90k;
}

ms_status ms_hw_player_note_seek_range(ms_hw_player *p, const ms_hw_seek_range *r)
{
    if (p == NULL || r == NULL || r->struct_size < sizeof(size_t)) {
        return MS_ERR_INVALID_ARG;
    }
    p->seekable = r->seekable ? 1 : 0;
    p->seek_kind = r->kind;
    p->win_start_90k = r->start_90k > 0 ? r->start_90k : 0;
    p->win_end_90k = r->end_90k;
    p->live_edge_90k = r->live_edge_90k;
    p->safety_90k = r->safety_90k > 0 ? r->safety_90k : 0;
    if (r->kind == MS_HW_SEEK_KIND_VOD && r->duration_90k >= 0) {
        p->duration_90k = r->duration_90k;
    } else {
        p->duration_90k = -1; /* 直播不得用窗口长度冒充 VOD 时长 */
    }
    if (!p->seekable) {
        p->seek_kind = MS_HW_SEEK_KIND_UNSEEKABLE;
    }
    return MS_OK;
}

ms_status ms_hw_player_get_seek_range(const ms_hw_player *p, ms_hw_seek_range *out)
{
    ms_hw_seek_range tmp;
    size_t n;
    if (p == NULL || out == NULL || out->struct_size < sizeof(size_t)) {
        return MS_ERR_INVALID_ARG;
    }
    memset(&tmp, 0, sizeof(tmp));
    tmp.struct_size = out->struct_size;
    tmp.seekable = p->seekable > 0 ? 1 : 0;
    tmp.kind = p->seekable == 0 ? MS_HW_SEEK_KIND_UNSEEKABLE : p->seek_kind;
    tmp.start_90k = p->win_start_90k;
    tmp.end_90k = p->win_end_90k;
    tmp.live_edge_90k = p->live_edge_90k;
    tmp.safety_90k = p->safety_90k;
    tmp.duration_90k = p->duration_90k;
    n = out->struct_size;
    if (n > sizeof(tmp)) {
        n = sizeof(tmp);
    }
    memcpy(out, &tmp, n);
    return MS_OK;
}

ms_status ms_hw_player_seek_live_edge(ms_hw_player *p)
{
    int64_t tgt;
    if (p == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    if (p->seekable == 0) {
        return MS_ERR_NOT_SEEKABLE;
    }
    if (p->live_edge_90k >= 0) {
        tgt = p->live_edge_90k;
    } else if (p->win_end_90k >= 0) {
        tgt = p->win_end_90k;
    } else if (p->duration_90k >= 0) {
        tgt = p->duration_90k;
    } else {
        return MS_ERR_NOT_SEEKABLE;
    }
    return ms_hw_player_seek(p, tgt);
}

int64_t ms_hw_player_position_90k(const ms_hw_player *p)
{
    if (p == NULL) {
        return 0;
    }
    if (p->have_last_media) {
        return (int64_t)p->last_media_90k;
    }
    if (p->audio != NULL && !p->audio_unsupported) {
        int64_t clock = 0;
        if (ms_hw_audio_get_clock_us(p->audio, &clock) == MS_OK && clock > 0) {
            return (clock * 90000ll) / 1000000ll;
        }
    }
    return 0;
}

int64_t ms_hw_player_duration_90k(const ms_hw_player *p)
{
    return p != NULL ? p->duration_90k : -1;
}

int ms_hw_player_is_seekable(const ms_hw_player *p)
{
    return p != NULL ? p->seekable : 0;
}

ms_status ms_hw_player_seek(ms_hw_player *p, int64_t time_90k)
{
    ms_status st;
    if (p == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    if (time_90k < 0) {
        return MS_ERR_INVALID_ARG;
    }
    if (p->seekable == 0) {
        return MS_ERR_NOT_SEEKABLE;
    }
    time_90k = clamp_player_target(p, time_90k);
    p->last_seek_request_90k = time_90k;
    st = ms_hw_player_flush(p);
    if (st != MS_OK) {
        return st;
    }
    if (p->audio != NULL && !p->audio_unsupported) {
        int64_t media_us = 0;
        /* 90 kHz -> us；0 表示呈现起点，只武装 pre-skip fallback，不武装 80ms 预热 */
        (void)ms_rescale_q(time_90k, 1, 90000, 1, 1000000, &media_us);
        (void)ms_hw_audio_note_seek(p->audio, media_us);
    }
    p->last_media_90k = (uint64_t)time_90k;
    p->have_last_media = 1;
    p->last_seek_land_90k = time_90k;
    p->have_seek_land = 1;
    p->timeline_epoch++;
    if (p->timeline_epoch == 0) {
        p->timeline_epoch = 1;
    }
    if (p->ai != NULL) {
        ms_hw_ai_bind_set_epochs(p->ai, 0, p->timeline_epoch, p->selection_epoch);
    }
    p->waiting_key = (p->opts.wait_keyframe != 0);
    /* 速率与暂停保持；CSD 保留。 */
    return MS_OK;
}

ms_status ms_hw_player_seek_relative(ms_hw_player *p, int64_t delta_90k)
{
    int64_t pos;
    int64_t tgt;
    if (p == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    if (p->seekable == 0) {
        return MS_ERR_NOT_SEEKABLE;
    }
    pos = ms_hw_player_position_90k(p);
    if (delta_90k > 0) {
        if (pos > INT64_MAX - delta_90k) {
            tgt = INT64_MAX;
        } else {
            tgt = pos + delta_90k;
        }
    } else if (delta_90k < 0) {
        if (pos < INT64_MIN - delta_90k) {
            tgt = INT64_MIN;
        } else {
            tgt = pos + delta_90k;
        }
    } else {
        tgt = pos;
    }
    if (tgt < 0) {
        tgt = 0;
    }
    tgt = clamp_player_target(p, tgt);
    return ms_hw_player_seek(p, tgt);
}

int ms_hw_player_ass_render_loaded(const ms_hw_player *p)
{
    if (p != NULL && p->ass != NULL) {
        return ms_hw_ass_bind_loaded(p->ass);
    }
    return ms_hw_ass_bind_probe();
}

int ms_hw_player_ass_has_script(const ms_hw_player *p)
{
    return p != NULL && ms_hw_ass_bind_has_script(p->ass);
}

ms_status ms_hw_player_ass_set_canvas(ms_hw_player *p, int canvas_w, int canvas_h)
{
    if (p == NULL || canvas_w <= 0 || canvas_h <= 0) {
        return MS_ERR_INVALID_ARG;
    }
    hw_ass_lazy(p);
    if (p->ass == NULL) {
        return MS_ERR_UNSUPPORTED;
    }
    return ms_hw_ass_bind_set_canvas(p->ass, canvas_w, canvas_h);
}

ms_status ms_hw_player_ass_frame_at(ms_hw_player *p, uint64_t time_90k, ms_hw_ass_frame *out)
{
    if (p == NULL || out == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    return ms_hw_ass_bind_frame_at(p->ass, time_90k, out);
}

int ms_hw_player_ai_module_loaded(const ms_hw_player *p)
{
    if (p == NULL) {
        return ms_hw_ai_bind_probe();
    }
    return ms_hw_ai_bind_loaded(p->ai);
}

const char *ms_hw_player_ai_status(const ms_hw_player *p)
{
    if (p == NULL || p->ai == NULL) {
        return "disabled";
    }
    return ms_hw_ai_bind_status(p->ai);
}

ms_status ms_hw_player_ai_configure(ms_hw_player *p, int vendor,
                                    const char *source_lang, const char *target_lang)
{
    ms_ai_sub_opts opts;
    ms_status st;
    if (p == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    hw_ai_lazy(p);
    if (p->ai == NULL) {
        return MS_ERR_UNSUPPORTED;
    }
    memset(&opts, 0, sizeof(opts));
    opts.struct_size = sizeof(opts);
    opts.want_partial = 1;
    opts.target_rate = 16000;
    opts.target_channels = 1;
    if (source_lang != NULL) {
        snprintf(opts.source_lang, sizeof(opts.source_lang), "%s", source_lang);
    }
    if (target_lang != NULL) {
        snprintf(opts.target_lang, sizeof(opts.target_lang), "%s", target_lang);
        opts.want_translation = target_lang[0] != 0;
    }
    if (vendor == MS_AI_VENDOR_NONE) {
        (void)ms_hw_ai_bind_set_enabled(p->ai, 0);
        hw_ai_sync_audio(p);
        hw_ai_drop_track(p);
        return MS_OK;
    }
    if (vendor != MS_AI_VENDOR_FAKE_LOCAL) {
        return MS_ERR_UNSUPPORTED;
    }
    opts.vendor = MS_AI_VENDOR_FAKE_LOCAL;
    st = ms_hw_ai_bind_configure(p->ai, &opts);
    if (st != MS_OK) {
        return st;
    }
    return ms_hw_ai_bind_use_fake_local(p->ai);
}

ms_status ms_hw_player_ai_set_enabled(ms_hw_player *p, int on)
{
    ms_status st;
    if (p == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    hw_ai_lazy(p);
    if (p->ai == NULL) {
        return on ? MS_ERR_UNSUPPORTED : MS_OK;
    }
    if (on) {
        st = ms_hw_ai_bind_set_enabled(p->ai, 1);
        if (st != MS_OK) {
            return st;
        }
        ms_hw_ai_bind_set_epochs(p->ai, 0, p->timeline_epoch, p->selection_epoch);
        st = hw_ai_ensure_track(p);
        if (st != MS_OK) {
            (void)ms_hw_ai_bind_set_enabled(p->ai, 0);
            return st;
        }
        hw_ai_sync_audio(p);
        return MS_OK;
    }
    if (p->audio != NULL) {
        ms_hw_audio_set_pcm_tap(p->audio, NULL, NULL);
    }
    (void)ms_hw_ai_bind_set_enabled(p->ai, 0);
    hw_ai_drop_track(p);
    return MS_OK;
}

int ms_hw_player_ai_enabled(const ms_hw_player *p)
{
    return p != NULL && ms_hw_ai_bind_enabled(p->ai);
}

int ms_hw_player_has_presented_video(const ms_hw_player *p)
{
    return p != NULL && p->first_video_presented;
}

int ms_hw_player_surface_bound(const ms_hw_player *p)
{
    return p != NULL && p->surface != NULL;
}

int ms_hw_player_take_surface_resync(ms_hw_player *p, int64_t *resync_90k)
{
    if (p == NULL) {
        return 0;
    }
    if (!p->need_surface_resync || p->surface_resync_taken) {
        return 0;
    }
    p->surface_resync_taken = 1;
    if (resync_90k != NULL) {
        *resync_90k = p->surface_resync_90k;
    }
    /* 直播不可 seek：调用方只等 IDR，不要内部 seek。 */
    if (p->seekable == 0) {
        return 0;
    }
    return 1;
}

ms_status ms_hw_player_clear_subtitles(ms_hw_player *p)
{
    int j;
    int changed = 0;
    if (p == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    if (p->canceled) {
        return MS_ERR_CANCELED;
    }
    for (j = 0; j < p->ntracks; j++) {
        if (!p->tracks[j].in_use || p->tracks[j].kind != MS_TRACK_SUBTITLE) {
            continue;
        }
        if (p->tracks[j].id == MS_TRACK_ID_AI_SUB) {
            continue;
        }
        if (p->tracks[j].selected) {
            p->tracks[j].selected = 0;
            changed = 1;
        }
    }
    p->user_sub_sel = 1;
    /* 只取消选中；保留 inband 缓存，打开同一轨时当前 cue 立即恢复。 */
    if (p->ass != NULL) {
        (void)ms_hw_ass_bind_clear(p->ass);
    }
    if (changed) {
        bump_selection_epoch(p);
    }
    return MS_OK;
}


ms_status ms_hw_player_debug_audio_add_write_quota(ms_hw_player *p, size_t pcm_bytes)
{
    if (p == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    if (p->audio == NULL || p->audio_unsupported) {
        return MS_ERR_STATE;
    }
    return ms_hw_audio_add_write_quota(p->audio, pcm_bytes);
}

ms_status ms_hw_player_set_hdr_info(ms_hw_player *p, const ms_hdr_info *info)
{
    if (p == NULL || info == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    if (info->struct_size < offsetof(ms_hdr_info, kind) + sizeof(ms_hdr_kind)) {
        return MS_ERR_INVALID_ARG;
    }
    ms_hdr_merge(&p->hdr, info);
    p->have_hdr = 1;
    if (p->hdr.dolby_vision && !p->logged_dv) {
        p->logged_dv = 1;
        player_log(p, "HDR: Dolby Vision identified; not supported, falling back to base layer if present");
    }
    return MS_OK;
}

ms_status ms_hw_player_get_hdr_info(const ms_hw_player *p, ms_hdr_info *out)
{
    size_t n;
    if (p == NULL || out == NULL || out->struct_size < offsetof(ms_hdr_info, kind) + sizeof(ms_hdr_kind)) {
        return MS_ERR_INVALID_ARG;
    }
    n = out->struct_size;
    if (n > sizeof(*out)) {
        n = sizeof(*out);
    }
    if (n > sizeof(p->hdr)) {
        n = sizeof(p->hdr);
    }
    memcpy(out, &p->hdr, n);
    out->struct_size = n;
    return MS_OK;
}

ms_status ms_hw_player_set_volume(ms_hw_player *p, float left, float right)
{
    if (p == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    if (p->audio == NULL) {
        return MS_OK; /* 尚未建音频；开始出声后再调一次 */
    }
    return ms_hw_audio_set_volume(p->audio, left, right);
}

/* 加法：当前视频宽高（来自 CSD/SPS 或容器），0=未知。供 MediaPlayer.getVideoWidth/Height。 */
ms_status ms_hw_player_video_size(const ms_hw_player *p, int *w, int *h)
{
    if (p == NULL || w == NULL || h == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    *w = p->width > 0 ? p->width : 0;
    *h = p->height > 0 ? p->height : 0;
    return MS_OK;
}

/* 阶段 4（内部）：音频直通开关与设备能力位，须在音频首次配置前设置。by: Long <mycjzb@gmail.com> */
void ms_hw_player_set_passthrough_caps(ms_hw_player *p, unsigned caps)
{
    if (p == NULL) {
        return;
    }
    p->pt_caps = caps;
    if (p->audio != NULL) {
        ms_hw_audio_set_passthrough_caps(p->audio, caps);
    }
}

int ms_hw_player_passthrough_route(ms_hw_player *p, unsigned *fallbacks)
{
    if (p == NULL || p->audio == NULL) {
        if (fallbacks != NULL) {
            *fallbacks = 0;
        }
        return 0;
    }
    return ms_hw_audio_passthrough_route(p->audio, fallbacks);
}
