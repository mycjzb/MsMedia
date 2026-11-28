#define _GNU_SOURCE
/*
 * 播放编排 JNI：Surface + 可选 ms_ff 网络/文件 URL 或本地 ms_session / 演示 AU。
 * 不把 releaseOutputBuffer / OUTPUT_RENDERED / OnFrameRendered
 * / 单次 Choreographer 写成实际显示。不记录私密 URL。
 * by: Long <mycjzb@gmail.com>
 */
#ifdef __ANDROID__

#include "msmedia/ms_hw.h"
#include "ms_hw_reap.h"
#include "msmedia/msmedia.h"
#ifdef MS_HW_HAVE_FF
#include "msmedia/ms_ff.h"
#endif
#if defined(MS_SINGLE_SO) && !defined(MS_SINGLE_FFADEC)
#include "../netsrc/ms_netsrc.h"
#include "../core/ms_probe.h"
#endif

#include <android/log.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <jni.h>
#include "ms_hw_api.h"
#include "ms_hw_jni.h"
#include <pthread.h>
#include "ms_hw_bionic18.h"
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <limits.h>

#if defined(MS_SINGLE_SO) && defined(MS_HW_HAVE_FF)
/* 阶段 5 瘦身：路线 B 单 so 不再编入 ms_session.c，这里给出与其等价的本地 trim 默认值初始化
 * （清零 + struct_size），避免仅为这一个小函数拉入整套自研解封装。by: Long <mycjzb@gmail.com> */
static void jni_event_trim_default(ms_event_trim *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->struct_size = sizeof(*out);
}
#define ms_event_trim_default jni_event_trim_default
#endif

typedef struct player_jni {
    ms_hw_player *p;
    ANativeWindow *win;
#ifdef MS_HW_HAVE_FF
    ms_ff *ff;
    char last_ff[192];
#endif
    pthread_t th;
    int th_alive;
    atomic_int stop;
    int exit_req;
    pthread_mutex_t mu;
    pthread_mutex_t op; /* 串行化 player 调用：UI 与工作线程 */
    pthread_cond_t cv;
    char last[768];
    char *pending_loc; /* URL 或本地路径；不写入 last/log */
    char pending_ca[512];
    int pending_timeout;
    int pending_kind; /* 1=url 2=path */
    int has_pending;
    int th_finished;
    int abandoned;
    int has_pending_seek;
    int pending_seek_live;
    int64_t pending_seek_90k;
#define PJ_PENDING_SEL_CAP 8
    struct {
        uint32_t id;
        int on;
    } pending_sel[PJ_PENDING_SEL_CAP];
    int pending_sel_n;
    uint32_t applied_audio_id;
    int applied_audio_valid;
    ANativeWindow *pending_win;
    jobject jsurf;          /* Java Surface 全局引用，JNI MediaCodec.configure 用 */
    jobject pending_jsurf;  /* 与 pending_win 一起入队 */
    int has_pending_surface;
    /* 加法（单 so MediaPlayer 用；多 so 构建不读）：会话进度与循环。 */
#if defined(MS_SINGLE_SO) && !defined(MS_SINGLE_FFADEC)
    ms_session *sess;       /* 本地路径会话（仅 worker 线程使用） */
#endif
    atomic_int mp_opened;   /* 1=已打开并识别（prepare 完成条件） */
    atomic_int mp_done;     /* 每结束一个会话 +1 */
    atomic_int mp_active;   /* 1=worker 正在跑 kind 1/2 会话（stop 后 Java 等它归零再 prepare） */
    atomic_int mp_looping;
#ifdef MS_SINGLE_SO
#if !defined(MS_SINGLE_FFADEC)
    /* 加法：纯自研网络/容错输入（路线 A URL、本地 MKV/PS/伪装头文件）。由 mu 保护指针，取消线程安全。 */
    ms_netsrc *ns;
#endif
    int ns_ahead_ms;        /* 0=默认 10000；FFADEC 仅作缓冲水位兼容字段 */
    long ns_max_bytes;      /* 0=默认 16 MiB */
    /* 阶段 6（路线 B）：Media3 风格缓冲水位（buf_set=0 时由 setPreloadConfig 推导或用默认）与播放 TLS 策略。 */
    int buf_set;
    int buf_min_ms, buf_max_ms, buf_play_ms, buf_rebuf_ms;
    long long buf_max_bytes;
    int tls_verify;         /* 0=播放拉流忽略证书（默认），1=严格 */
#endif
} player_jni;

static const uint8_t k_avcc[] = {
    0x01, 0x42, 0xC0, 0x1E, 0xFF, 0xE1, 0x00, 0x08,
    0x67, 0x42, 0xC0, 0x1E, 0xDA, 0x01, 0x00, 0x00,
    0x01, 0x00, 0x05, 0x68, 0xCE, 0x38, 0x80, 0x00
};
static const uint8_t k_au[16] = {
    0x00, 0x00, 0x00, 0x0C, 0x65, 0x88, 0x84, 0x00,
    0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10, 0x11
};

static player_jni *pj_from(jlong h)
{
    return (player_jni *)(intptr_t)h;
}

static void replace_jsurf(JNIEnv *env, jobject *slot, jobject surf)
{
    if (env == NULL || slot == NULL) {
        return;
    }
    if (*slot != NULL && (surf == NULL || !(*env)->IsSameObject(env, *slot, surf))) {
        (*env)->DeleteGlobalRef(env, *slot);
        *slot = NULL;
    }
    if (surf != NULL && *slot == NULL) {
        *slot = (*env)->NewGlobalRef(env, surf);
    }
}

/* UI 不得在 worker 持有 op（MediaCodec configure 可长达数十秒）时阻塞，
 * 否则会 ANR，随后被系统 freezer 冻结，命令广播再也无法送达。 */
static void maybe_queue_surface_resync(player_jni *pj);

static int lock_op_ms(player_jni *pj, int timeout_ms)
{
    struct timespec ts;
    if (pj == NULL) {
        return 0;
    }
    if (timeout_ms <= 0) {
        return pthread_mutex_trylock(&pj->op) == 0;
    }
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
        return pthread_mutex_trylock(&pj->op) == 0;
    }
    ts.tv_nsec += (long)timeout_ms * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec += ts.tv_nsec / 1000000000L;
        ts.tv_nsec %= 1000000000L;
    }
    /* pthread_mutex_timedlock 是 API 21；弱引用为空时轮询 trylock，避免 API 18 加载失败。 */
    if (pthread_mutex_timedlock != NULL) {
        return pthread_mutex_timedlock(&pj->op, &ts) == 0;
    }
    for (;;) {
        struct timespec now;
        struct timespec slp;
        if (pthread_mutex_trylock(&pj->op) == 0) {
            return 1;
        }
        if (clock_gettime(CLOCK_REALTIME, &now) != 0) {
            return 0;
        }
        if (now.tv_sec > ts.tv_sec ||
            (now.tv_sec == ts.tv_sec && now.tv_nsec >= ts.tv_nsec)) {
            return 0;
        }
        slp.tv_sec = 0;
        slp.tv_nsec = 1000000L;
        nanosleep(&slp, NULL);
    }
}

static int has_pending_surface(player_jni *pj)
{
    int n;
    pthread_mutex_lock(&pj->mu);
    n = pj->has_pending_surface;
    pthread_mutex_unlock(&pj->mu);
    return n;
}

static void apply_pending_surface(player_jni *pj)
{
    ANativeWindow *nw = NULL;
    int has = 0;
    if (pj == NULL || pj->p == NULL) {
        return;
    }
    jobject js = NULL;
    JNIEnv *env = NULL;
    pthread_mutex_lock(&pj->mu);
    if (pj->has_pending_surface) {
        nw = pj->pending_win;
        js = pj->pending_jsurf;
        pj->pending_win = NULL;
        pj->pending_jsurf = NULL;
        pj->has_pending_surface = 0;
        has = 1;
    }
    pthread_mutex_unlock(&pj->mu);
    if (!has) {
        return;
    }
    (void)ms_hw_jni_env(&env);
    pthread_mutex_lock(&pj->op);
    if (pj->win != NULL && pj->win != nw) {
        ANativeWindow_release(pj->win);
        pj->win = NULL;
    }
    pj->win = nw;
    if (env != NULL && pj->jsurf != NULL && pj->jsurf != js) {
        (*env)->DeleteGlobalRef(env, pj->jsurf);
        pj->jsurf = NULL;
    }
    pj->jsurf = js; /* 转移 pending 全局引用 */
    /* JNI MediaCodec.configure 需要 jobject Surface，不能把 ANativeWindow* 当 JNI 引用。
     * API 21+ NDK 路径仍传 ANativeWindow*。 */
    if (ms_hw_use_ndk_mediacodec()) {
        (void)ms_hw_player_set_surface(pj->p, pj->win);
    } else {
        (void)ms_hw_player_set_surface(pj->p, pj->jsurf);
    }
    if (pj->win != NULL || pj->jsurf != NULL) {
        maybe_queue_surface_resync(pj);
    }
    pthread_mutex_unlock(&pj->op);
}

static void set_last(player_jni *pj, const char *s)
{
    if (pj == NULL) {
        return;
    }
    pthread_mutex_lock(&pj->mu);
    snprintf(pj->last, sizeof(pj->last), "%s", s ? s : "");
    pthread_mutex_unlock(&pj->mu);
}

static void fill_ev(ms_event *ev, ms_event_type typ, int key)
{
    memset(ev, 0, sizeof(*ev));
    ev->type = typ;
    ev->track.kind = MS_TRACK_VIDEO;
    ev->track.codec = MS_CODEC_H264;
    ev->sample_format = MS_SAMPLE_FMT_AVCC;
    ev->time_base_num = 1;
    ev->time_base_den = 90000;
    ev->is_keyframe = key;
    if (typ == MS_EVENT_CODEC_CONFIG) {
        ev->data = k_avcc;
        ev->data_size = sizeof(k_avcc);
    } else if (typ == MS_EVENT_AU) {
        ev->data = k_au;
        ev->data_size = sizeof(k_au);
        ev->has_pts = 1;
        ev->pts = 9000;
    }
}

static void format_stats(char *dst, size_t cap, const ms_hw_player *p, const char *note)
{
    ms_hw_player_stats st;
    memset(&st, 0, sizeof(st));
    st.struct_size = sizeof(st);
    if (p == NULL || ms_hw_player_get_stats(p, &st) != MS_OK) {
        snprintf(dst, cap, "no stats");
        return;
    }
    snprintf(dst, cap,
             "stage_local(ms, from create/flush, NOT e2e): first_packet=%.3f "
             "first_decodable_AU=%.3f first_submit=%.3f first_output_to_surface=%.3f "
             "first_codec_rendered=%.3f first_display=%.3f "
             "request_e2e(ms, from enqueue mark_request): have_request=%d "
             "worker_start=%.3fms open_input_done=%.3fms find_info_done=%.3fms "
             "dur_open_input=%.3fms dur_find_info=%.3fms "
             "packet=%.3fms decodable=%.3fms submit=%.3fms output_not_display=%.3fms "
             "codec_rendered=%.3fms display=%.3f "
             "output_is_NOT_display=%d codec_rendered_is_NOT_display=%d "
             "have_display=%d display_evidence=%s "
             "cfg=%llu au_in=%llu submitted=%llu drop_nokey=%llu "
             "drop_preroll=%llu backpressure=%llu "
             "ref_hold=%llu plat_copy=%llu staging_copy=%llu "
             "configured=%d hw=%d backend=%s err=%s epoch=%u\n"
             "NOTE: first_output is releaseOutputBuffer/Surface handoff. "
             "first_codec_rendered is MediaCodec OnFrameRendered (API33+; "
             "informational, may be delayed/batched). Neither is on-screen paint. "
             "Choreographer is not hooked as display. %s",
             st.t_first_packet_us / 1000.0, st.t_first_decodable_au_us / 1000.0,
             st.t_first_submit_us / 1000.0, st.t_first_output_us / 1000.0,
             st.t_first_codec_rendered_us / 1000.0, st.t_first_display_us / 1000.0,
             st.have_request,
             st.t_req_worker_start_us / 1000.0, st.t_req_open_input_done_us / 1000.0,
             st.t_req_find_stream_info_done_us / 1000.0,
             st.dur_open_input_us / 1000.0, st.dur_find_stream_info_us / 1000.0,
             st.t_req_first_packet_us / 1000.0, st.t_req_first_decodable_au_us / 1000.0,
             st.t_req_first_submit_us / 1000.0, st.t_req_first_output_us / 1000.0,
             st.t_req_first_codec_rendered_us / 1000.0, st.t_req_first_display_us / 1000.0,
             st.output_is_not_display, st.codec_rendered_is_not_display,
             st.have_first_display,
             st.display_evidence ? st.display_evidence : "none",
             (unsigned long long)st.cfg_accepted,
             (unsigned long long)st.video_au_in,
             (unsigned long long)st.video_au_submitted,
             (unsigned long long)st.video_au_dropped_nokey,
             (unsigned long long)st.video_au_dropped_preroll,
             (unsigned long long)st.video_au_backpressure,
             (unsigned long long)st.ref_hold_ops,
             (unsigned long long)st.platform_copy_ops,
             (unsigned long long)st.staging_copy_ops, st.configured,
             st.is_hardware, st.backend ? st.backend : "?",
             ms_status_name(st.last_error), st.epoch,
             note ? note : "");
    {
        size_t n = strlen(dst);
        const char *ad = st.audio_detail ? st.audio_detail : "";
        const char *label = "";
        if (st.audio_configured && st.audio_system_decode_is_not_hardware) {
            label = "系统 AAC，非硬件";
        } else if (st.audio_unsupported) {
            label = "AAC unsupported (not hardware)";
        }
        if (n + 8 < cap) {
            snprintf(dst + n, cap - n,
                     "\naudio: status=%s unsupported=%d have=%d configured=%d "
                     "is_hardware=%d system_decode_is_not_hardware=%d kind=%s "
                     "policy=%s detail=%s (%s). "
                     "This is not a claim of device speaker output.",
                     ms_status_name(st.audio_status), st.audio_unsupported, st.have_audio,
                     st.audio_configured, st.audio_is_hardware,
                     st.audio_system_decode_is_not_hardware,
                     ms_hw_kind_name(st.audio_kind),
                     label[0] ? label : (st.audio_is_hardware ? "hardware AAC" : "none"),
                     ad, label[0] ? label : "no system-AAC banner");
        }
        n = strlen(dst);
        if (n + 8 < cap) {
            snprintf(dst + n, cap - n,
                     "\nrate=%d/%d paused=%d pos_90k=%lld dur_90k=%lld seekable=%d "
                     "timeline_epoch=%llu (host/API rate caps apply; mock is not device A/V sync).",
                     st.rate_num, st.rate_den, st.paused,
                     (long long)st.position_90k, (long long)st.duration_90k,
                     st.seekable, (unsigned long long)st.timeline_epoch);
        }
        n = strlen(dst);
        if (n + 8 < cap) {
            const char *ai = ms_hw_player_ai_status(p);
            snprintf(dst + n, cap - n,
                     "\nai=%s (not a real-ASR claim; default disabled).",
                     ai ? ai : "disabled");
        }
        n = strlen(dst);
        if (n + 8 < cap) {
            snprintf(dst + n, cap - n,
                     "\naudio_track: write_frames=%llu write_bytes=%llu head_frames=%lld "
                     "clock_us=%lld (AudioTrack.write / playbackHeadPosition; "
                     "not speaker listening; AVD -no-audio is not device audio).",
                     (unsigned long long)st.audio_track_write_frames,
                     (unsigned long long)st.audio_track_write_bytes,
                     (long long)st.audio_track_head_frames,
                     (long long)st.audio_clock_us);
        }
        n = strlen(dst);
        if (n + 8 < cap) {
            snprintf(dst + n, cap - n,
                     "\nav_sync: have=%d video_pts_us=%lld audio_clock_us=%lld delta_us=%lld "
                     "media3=%d clock_src=%d latency_us=%lld vsync_ns=%lld held=%llu "
                     "dropped_late=%llu forced_late=%llu drop_to_key=%llu scheduled=%llu "
                     "(not device lip-sync).",
                     st.have_av_sync, (long long)st.video_last_pts_us,
                     (long long)st.audio_clock_us, (long long)st.av_sync_delta_us,
                     st.sync_in_decoder, st.audio_clock_source, (long long)st.audio_latency_us,
                     (long long)st.vsync_period_ns, (unsigned long long)st.video_au_held,
                     (unsigned long long)st.video_au_dropped_late,
                     (unsigned long long)st.video_au_late,
                     (unsigned long long)st.video_drop_to_keyframe,
                     (unsigned long long)st.video_release_scheduled);
        }
        n = strlen(dst);
        if (n + 8 < cap) {
            snprintf(dst + n, cap - n,
                     "\ntracks: audio_id=%u sub_id=%u audio_switch=%llu sel_epoch=%u "
                     "user_paused=%d focus=%d surface_bound=%d",
                     (unsigned)st.audio_track_id, (unsigned)st.sub_track_id,
                     (unsigned long long)st.audio_switch_count,
                     (unsigned)st.selection_epoch, st.user_paused, st.audio_focus,
                     st.surface_bound);
        }
        n = strlen(dst);
        if (n + 8 < cap) {
            const ms_hdr_info *h = &st.hdr;
            snprintf(dst + n, cap - n,
                     "\nhdr=%s prim=%d trc=%d matrix=%d range=%d bit=%d mastering=%d cll=%d "
                     "src=vui:%d,sei:%d,container:%d hdr10plus=%d(detected-not-passed-unverified) "
                     "dv=%d%s (identify/passthrough, not a display-effect claim)",
                     ms_hdr_kind_name(h->kind), h->color_primaries, h->color_transfer,
                     h->color_matrix, h->color_range, h->bit_depth, h->have_mastering, h->have_cll,
                     h->from_vui, h->from_sei, h->from_container, h->have_hdr10_plus, h->dolby_vision,
                     h->dolby_vision ? " (unsupported, base-layer fallback if present)" : "");
        }
    }
}


/* 仅在线程已 join，或被 abandon 的工作线程即将退出时调用。 */
static void pj_cleanup_owned(player_jni *pj)
{
    if (pj == NULL) {
        return;
    }
#ifdef MS_HW_HAVE_FF
    if (pj->ff) {
        ms_ff_destroy(pj->ff);
        pj->ff = NULL;
    }
#endif
    if (pj->p) {
        ms_hw_player_destroy(pj->p);
        pj->p = NULL;
    }
    if (pj->win) {
        ANativeWindow_release(pj->win);
        pj->win = NULL;
    }
    {
        JNIEnv *env = NULL;
        if (ms_hw_jni_env(&env) && env != NULL) {
            if (pj->jsurf != NULL) {
                (*env)->DeleteGlobalRef(env, pj->jsurf);
                pj->jsurf = NULL;
            }
            if (pj->pending_jsurf != NULL) {
                (*env)->DeleteGlobalRef(env, pj->pending_jsurf);
                pj->pending_jsurf = NULL;
            }
        }
    }
    pthread_mutex_lock(&pj->mu);
    if (pj->pending_win != NULL) {
        ANativeWindow_release(pj->pending_win);
        pj->pending_win = NULL;
    }
    free(pj->pending_loc);
    pj->pending_loc = NULL;
    pj->has_pending = 0;
    pthread_mutex_unlock(&pj->mu);
    pthread_cond_destroy(&pj->cv);
    pthread_mutex_destroy(&pj->op);
    pthread_mutex_destroy(&pj->mu);
    free(pj);
}

static void request_stop_io(player_jni *pj)
{
    if (pj == NULL) {
        return;
    }
    atomic_store(&pj->stop, 1);
#ifdef MS_HW_HAVE_FF
    if (pj->ff) {
        (void)ms_ff_cancel(pj->ff);
    }
#endif
#if defined(MS_SINGLE_SO) && !defined(MS_HW_HAVE_FF) && !defined(MS_SINGLE_FFADEC)
    pthread_mutex_lock(&pj->mu);
    if (pj->ns) {
        ms_netsrc_cancel(pj->ns); /* 线程安全；阻塞中的网络读写 ≤100ms 返回 */
    }
    pthread_mutex_unlock(&pj->mu);
#endif
}

static int apply_pending_seek(player_jni *pj);
#ifdef MS_HW_HAVE_FF
static int apply_pending_select(player_jni *pj);
#endif
static void maybe_queue_surface_resync(player_jni *pj);
static void apply_pending_surface(player_jni *pj);

static int has_pending_cmds(player_jni *pj)
{
    int n;
    if (pj == NULL) {
        return 0;
    }
    pthread_mutex_lock(&pj->mu);
    n = pj->has_pending_seek || pj->pending_sel_n > 0 || pj->has_pending_surface;
    pthread_mutex_unlock(&pj->mu);
    return n;
}

static ms_status feed_retry_jni(player_jni *pj, const ms_event *ev, int64_t origin)
{
    ms_status st = MS_AGAIN;
    int spins = 0;
    /* LIMIT/AGAIN 时 drive+重试；起播前限制自旋，避免音频/坏 CSD 堵死 demux（H264 直播无首帧）。
     * 背压等待期间必须让出待处理的 seek/选轨。 */
    while (!atomic_load(&pj->stop)) {
        pthread_mutex_lock(&pj->op);
        st = ms_hw_player_feed(pj->p, ev, origin, NULL, NULL);
        if (st == MS_LIMIT || st == MS_AGAIN) {
            (void)ms_hw_player_drive(pj->p);
        }
        pthread_mutex_unlock(&pj->op);
        if (has_pending_surface(pj)) {
            apply_pending_surface(pj);
        }
        if (st != MS_LIMIT && st != MS_AGAIN) {
            return st;
        }
        if (has_pending_cmds(pj)) {
            return MS_AGAIN;
        }
        /* ~40ms：仍 LIMIT/AGAIN 则丢本包继续拉，避免起播被少数 AU 吃光预算。 */
        if (++spins >= 5) {
            return MS_OK;
        }
        usleep(8000);
    }
    return st;
}

/* demux 结束或提前退出后继续 drive：解码器可能仍有在途帧。
 * UI 的 stats 轮询不能作为唯一抽帧路径（ANR 会卡住 Handler）。 */
static void drain_after_session(player_jni *pj)
{
    int i;
    if (pj == NULL || pj->p == NULL) {
        return;
    }
    pthread_mutex_lock(&pj->op);
    (void)ms_hw_player_signal_eos(pj->p);
    pthread_mutex_unlock(&pj->op);
    for (i = 0; i < 800 && !atomic_load(&pj->stop); i++) {
        apply_pending_surface(pj);
        (void)apply_pending_seek(pj);
#ifdef MS_HW_HAVE_FF
        (void)apply_pending_select(pj);
#endif
        pthread_mutex_lock(&pj->op);
        (void)ms_hw_player_drive(pj->p);
        pthread_mutex_unlock(&pj->op);
        usleep(10000);
    }
}

static void begin_session(player_jni *pj)
{
    pthread_mutex_lock(&pj->op);
#ifdef MS_HW_HAVE_FF
    if (pj->ff) {
        ms_ff_destroy(pj->ff);
        pj->ff = NULL;
    }
#endif
    (void)ms_hw_player_flush(pj->p);
    (void)ms_hw_player_mark_worker_start(pj->p);
    pthread_mutex_unlock(&pj->op);
    pthread_mutex_lock(&pj->mu);
    pj->has_pending_seek = 0;
    pj->pending_seek_live = 0;
    pj->pending_sel_n = 0;
    pj->applied_audio_valid = 0;
    pj->applied_audio_id = 0;
    pthread_mutex_unlock(&pj->mu);
}


#ifdef MS_HW_HAVE_FF
static void note_ff_seek_range(player_jni *pj)
{
    ms_ff_seek_range fr;
    ms_hw_seek_range hr;
    if (pj == NULL || pj->ff == NULL || pj->p == NULL) {
        return;
    }
    memset(&fr, 0, sizeof(fr));
    fr.struct_size = sizeof(fr);
    if (ms_ff_get_seek_range(pj->ff, &fr) != MS_OK) {
        (void)ms_hw_player_note_timeline(pj->p, ms_ff_is_seekable(pj->ff),
                                         ms_ff_duration_90k(pj->ff));
        return;
    }
    memset(&hr, 0, sizeof(hr));
    hr.struct_size = sizeof(hr);
    hr.seekable = fr.seekable;
    hr.kind = fr.kind;
    hr.start_90k = fr.start_90k;
    hr.end_90k = fr.end_90k;
    hr.live_edge_90k = fr.live_edge_90k;
    hr.safety_90k = fr.safety_90k;
    hr.duration_90k = fr.duration_90k;
    (void)ms_hw_player_note_seek_range(pj->p, &hr);
}
#endif

static int apply_pending_seek(player_jni *pj)
{
    int64_t t;
    int live;
    ms_status st;
    char buf[192];
    pthread_mutex_lock(&pj->mu);
    if (!pj->has_pending_seek) {
        pthread_mutex_unlock(&pj->mu);
        return 0;
    }
    t = pj->pending_seek_90k;
    live = pj->pending_seek_live;
    pj->has_pending_seek = 0;
    pj->pending_seek_live = 0;
    pthread_mutex_unlock(&pj->mu);
    pthread_mutex_lock(&pj->op);
#ifdef MS_HW_HAVE_FF
    if (pj->ff != NULL) {
        note_ff_seek_range(pj);
    }
#endif
    if (live) {
#ifdef MS_HW_HAVE_FF
        if (pj->ff != NULL) {
            st = ms_ff_seek_live_edge(pj->ff);
            if (st != MS_OK) {
                pthread_mutex_unlock(&pj->op);
                snprintf(buf, sizeof(buf), "ff seek live edge: %s", ms_status_name(st));
                set_last(pj, buf);
                return -1;
            }
            note_ff_seek_range(pj);
        }
#endif
        st = ms_hw_player_seek_live_edge(pj->p);
    } else {
#ifdef MS_HW_HAVE_FF
        if (pj->ff != NULL) {
            st = ms_ff_seek(pj->ff, t);
            if (st != MS_OK) {
                pthread_mutex_unlock(&pj->op);
                snprintf(buf, sizeof(buf), "ff seek: %s", ms_status_name(st));
                set_last(pj, buf);
                return -1;
            }
            note_ff_seek_range(pj);
        }
#endif
#if defined(MS_SINGLE_SO) && !defined(MS_HW_HAVE_FF) && !defined(MS_SINGLE_FFADEC)
        if (pj->sess != NULL) {
            st = ms_session_seek(pj->sess, t);
            if (st != MS_OK) {
                pthread_mutex_unlock(&pj->op);
                snprintf(buf, sizeof(buf), "session seek: %s", ms_status_name(st));
                set_last(pj, buf);
                return -1;
            }
        }
#endif
        st = ms_hw_player_seek(pj->p, t);
    }
    pthread_mutex_unlock(&pj->op);
    if (st != MS_OK) {
        snprintf(buf, sizeof(buf), "player seek: %s", ms_status_name(st));
        set_last(pj, buf);
        return -1;
    }
    set_last(pj, "seek applied (waiting keyframe/CSD; not device paint)");
    return 1;
}

#ifdef MS_HW_HAVE_FF
static void sync_ff_audio_to_player(player_jni *pj)
{
    ms_track_info ti[32];
    size_t n = 0, i;
    uint32_t want_a = 0, want_v = 0;
    int have_a = 0, have_v = 0;
    ms_status st;
    if (pj == NULL || pj->ff == NULL || pj->p == NULL) {
        return;
    }
    if (ms_ff_list_tracks(pj->ff, ti, 32, &n) != MS_OK) {
        return;
    }
    for (i = 0; i < n; i++) {
        if (ti[i].kind == MS_TRACK_AUDIO && ti[i].selected) {
            want_a = ti[i].id;
            have_a = 1;
        }
        if (ti[i].kind == MS_TRACK_VIDEO && ti[i].selected) {
            want_v = ti[i].id;
            have_v = 1;
        }
    }
    if (!have_a) {
        if (pj->applied_audio_valid && pj->applied_audio_id != 0) {
            st = ms_hw_player_select_track(pj->p, pj->applied_audio_id, 0);
            if (st == MS_OK) {
                pj->applied_audio_id = 0;
                pj->applied_audio_valid = 1;
            }
        }
    } else if (!(pj->applied_audio_valid && pj->applied_audio_id == want_a)) {
        st = ms_hw_player_select_track(pj->p, want_a, 1);
        if (st == MS_OK) {
            pj->applied_audio_id = want_a;
            pj->applied_audio_valid = 1;
        }
    }
    if (have_v) {
        /* 视频在 ff 提交（新轨关键帧）后才 selected；此处入队到播放器并等关键帧。 */
        (void)ms_hw_player_select_track(pj->p, want_v, 1);
    }
}

static int apply_pending_select(player_jni *pj)
{
    uint32_t ids[PJ_PENDING_SEL_CAP];
    int ons[PJ_PENDING_SEL_CAP];
    int n = 0, i, applied = 0;
    ms_status st;
    char buf[192];
    pthread_mutex_lock(&pj->mu);
    n = pj->pending_sel_n;
    if (n > PJ_PENDING_SEL_CAP) {
        n = PJ_PENDING_SEL_CAP;
    }
    for (i = 0; i < n; i++) {
        ids[i] = pj->pending_sel[i].id;
        ons[i] = pj->pending_sel[i].on;
    }
    pj->pending_sel_n = 0;
    pthread_mutex_unlock(&pj->mu);
    if (n <= 0 || pj->ff == NULL) {
        return 0;
    }
    /* 一次排空队列。特殊 id（外挂字幕哨兵）不得交给 ff。
     * 播放器侧在此补做 select，避免 UI 因 busy 只入队后 stats 永远不变。 */
    pthread_mutex_lock(&pj->op);
    for (i = 0; i < n; i++) {
        if (ids[i] == MS_TRACK_ID_EXTERNAL_SUB || ids[i] == MS_TRACK_ID_AI_SUB) {
            if (!ons[i] && ids[i] == MS_TRACK_ID_EXTERNAL_SUB) {
                (void)ms_hw_player_clear_subtitles(pj->p);
            }
            applied = 1;
            continue;
        }
        st = ms_ff_select_track(pj->ff, ids[i], ons[i]);
        if (st != MS_OK) {
            pthread_mutex_unlock(&pj->op);
            snprintf(buf, sizeof(buf), "ff select id=%u on=%d: %s (previous session kept)",
                     (unsigned)ids[i], ons[i], ms_status_name(st));
            set_last(pj, buf);
            return -1;
        }
        (void)ms_hw_player_select_track(pj->p, ids[i], ons[i]);
        applied = 1;
    }
    sync_ff_audio_to_player(pj);
    pthread_mutex_unlock(&pj->op);
    set_last(pj, "track select applied (worker demux+player; not device A/V)");
    return applied ? 1 : 0;
}

static void enqueue_select(player_jni *pj, uint32_t id, int on)
{
    /* 关字幕哨兵只走 clear_subtitles，不要进 ff 队列。 */
    if (id == MS_TRACK_ID_EXTERNAL_SUB || id == MS_TRACK_ID_AI_SUB) {
        return;
    }
    pthread_mutex_lock(&pj->mu);
    if (pj->pending_sel_n >= PJ_PENDING_SEL_CAP) {
        memmove(&pj->pending_sel[0], &pj->pending_sel[1],
                sizeof(pj->pending_sel[0]) * (PJ_PENDING_SEL_CAP - 1));
        pj->pending_sel_n = PJ_PENDING_SEL_CAP - 1;
    }
    pj->pending_sel[pj->pending_sel_n].id = id;
    pj->pending_sel[pj->pending_sel_n].on = on;
    pj->pending_sel_n++;
    pthread_mutex_unlock(&pj->mu);
}

#endif

static void maybe_queue_surface_resync(player_jni *pj)
{
    int64_t t = 0;
    if (pj == NULL || pj->p == NULL) {
        return;
    }
    if (!ms_hw_player_take_surface_resync(pj->p, &t)) {
        return;
    }
    pthread_mutex_lock(&pj->mu);
    if (!pj->has_pending_seek) {
        pj->pending_seek_90k = t;
        pj->pending_seek_live = 0;
        pj->has_pending_seek = 1;
    }
    pthread_mutex_unlock(&pj->mu);
    set_last(pj, "surface rebind: queued internal keyframe resync seek (seekable; not live IDR wait)");
}

#ifdef MS_HW_HAVE_FF
#ifdef MS_HW_HAVE_FF
static void pj_ff_log(void *ctx, int level, const char *msg)
{
    player_jni *pj = (player_jni *)ctx;
    (void)level;
    if (msg == NULL || msg[0] == 0) {
        return;
    }
    __android_log_print(ANDROID_LOG_INFO, "MsFf", "%s", msg);
    if (pj != NULL) {
        snprintf(pj->last_ff, sizeof(pj->last_ff), "%s", msg);
    }
}
#endif

#ifdef MS_SINGLE_SO
/* 阶段 6：把 Java 层缓冲水位 / 播放 TLS 策略下发给 ms_ff（下一次 prepare 生效）。
 * 未调用 setBufferConfig 时：setPreloadConfig(aheadMs, maxBytes) 兼容映射为 max=aheadMs、min=min(15000, aheadMs/2)。
 * by: Long <mycjzb@gmail.com> */
static void apply_ff_buffer_tls(player_jni *pj)
{
    ms_ff_buffer_config bc;
    int set, amin, amax, aplay, arebuf, ahead, verify;
    long long abytes;
    long nsbytes;
    ms_ff_buffer_config_default(&bc);
    pthread_mutex_lock(&pj->mu);
    set = pj->buf_set;
    amin = pj->buf_min_ms;
    amax = pj->buf_max_ms;
    aplay = pj->buf_play_ms;
    arebuf = pj->buf_rebuf_ms;
    abytes = pj->buf_max_bytes;
    ahead = pj->ns_ahead_ms;
    nsbytes = pj->ns_max_bytes;
    verify = pj->tls_verify;
    pthread_mutex_unlock(&pj->mu);
    if (set) {
        bc.min_buffer_ms = amin;
        bc.max_buffer_ms = amax;
        bc.buffer_for_playback_ms = aplay;
        bc.buffer_for_playback_after_rebuffer_ms = arebuf;
        bc.max_buffer_bytes = abytes;
    } else {
        if (ahead > 0) {
            bc.max_buffer_ms = ahead;
            bc.min_buffer_ms = ahead / 2 < 15000 ? ahead / 2 : 15000;
        }
        if (nsbytes > 0) {
            bc.max_buffer_bytes = nsbytes;
        }
    }
    if (ms_ff_set_buffer_config(pj->ff, &bc) != MS_OK) {
        ms_ff_buffer_config_default(&bc);
        (void)ms_ff_set_buffer_config(pj->ff, &bc);
    }
    (void)ms_ff_set_tls_verify(pj->ff, verify);
}
#endif

static void run_one_url(player_jni *pj, char *url, const char *ca, int timeout_ms)
{
    ms_ff_limits lim;
    ms_status st;
    char buf[768];
    int au = 0;
    begin_session(pj);
    ms_ff_limits_default(&lim);
    if (timeout_ms > 0) {
        lim.io_timeout_ms = timeout_ms;
    }
    st = ms_ff_create(NULL, &lim, &pj->ff);
    if (st != MS_OK) {
        set_last(pj, "ms_ff_create failed");
        free(url);
        return;
    }
    pj->last_ff[0] = 0;
    ms_ff_set_log(pj->ff, pj_ff_log, pj);
    if (ca != NULL && ca[0] != 0) {
        st = ms_ff_set_ca_file(pj->ff, ca);
        if (st != MS_OK) {
            set_last(pj, "set_ca_file failed");
            free(url);
            return;
        }
    }
    if (url == NULL || url[0] == 0) {
        set_last(pj, "empty url");
        free(url);
        return;
    }
    if (ms_ff_url_needs_tls_trust(url) && !ms_ff_trust_ready(pj->ff)) {
        set_last(pj, "UNSUPPORTED: TLS trust anchors missing; verification stays required");
        free(url);
        return;
    }
    if (atomic_load(&pj->stop)) {
        free(url);
        set_last(pj, "canceled before open");
        return;
    }
    (void)ms_ff_set_probe_mode(pj->ff, MS_FF_PROBE_FAST_START);
#ifdef MS_SINGLE_SO
    apply_ff_buffer_tls(pj);
#endif
    st = ms_ff_open(pj->ff, url);
    free(url);
    url = NULL;
    {
        ms_ff_open_stats os;
        memset(&os, 0, sizeof(os));
        os.struct_size = sizeof(os);
        if (ms_ff_get_open_stats(pj->ff, &os) == MS_OK) {
            pthread_mutex_lock(&pj->op);
            (void)ms_hw_player_note_open_phases(pj->p, os.dur_open_input_us,
                                                os.dur_find_stream_info_us);
            pthread_mutex_unlock(&pj->op);
        }
    }
    if (st != MS_OK) {
        snprintf(buf, sizeof(buf), "ms_ff_open failed: %s (%s) (url hidden)",
                 ms_status_name(st), pj->last_ff[0] ? pj->last_ff : "no-ff-log");
        set_last(pj, buf);
        return;
    }
    pthread_mutex_lock(&pj->op);
    note_ff_seek_range(pj);
    pthread_mutex_unlock(&pj->op);
    atomic_store(&pj->mp_opened, 1);
    {
        ms_hdr_info hi;
        ms_hdr_info_default(&hi);
        if (ms_ff_get_hdr_info(pj->ff, &hi) == MS_OK) {
            pthread_mutex_lock(&pj->op);
            (void)ms_hw_player_set_hdr_info(pj->p, &hi);
            pthread_mutex_unlock(&pj->op);
            snprintf(buf, sizeof(buf),
                     "opened; hdr=%s dv=%d (passthrough/identify, not display effect); pumping events",
                     ms_hdr_kind_name(hi.kind), hi.dolby_vision);
            set_last(pj, buf);
        } else {
            set_last(pj, "opened; pumping events");
        }
    }
    pthread_mutex_lock(&pj->op);
    note_ff_seek_range(pj);
    pthread_mutex_unlock(&pj->op);
    while (!atomic_load(&pj->stop)) {
        ms_event ev;
        int paused = 0;
        int need_still = 0;
        apply_pending_surface(pj);
        if (apply_pending_seek(pj) != 0) {
            continue;
        }
        if (apply_pending_select(pj) != 0) {
            continue;
        }
        pthread_mutex_lock(&pj->op);
        paused = ms_hw_player_is_paused(pj->p);
        if (paused) {
            /* 暂停也 drive 视频，尽快出当前帧；不在这里走音频。 */
            (void)ms_hw_player_drive(pj->p);
            need_still = ms_hw_player_surface_bound(pj->p) &&
                         !ms_hw_player_has_presented_video(pj->p);
        }
        pthread_mutex_unlock(&pj->op);
        if (paused && !need_still) {
            usleep(20000);
            continue;
        }
        memset(&ev, 0, sizeof(ev));
        st = ms_ff_pull(pj->ff, &ev);
        if (st == MS_AGAIN) {
            pthread_mutex_lock(&pj->op);
            (void)ms_hw_player_drive(pj->p);
            pthread_mutex_unlock(&pj->op);
            if (paused) {
                usleep(20000);
            }
            continue;
        }
        if (st == MS_EOS && atomic_load(&pj->mp_looping) && ms_ff_is_seekable(pj->ff)) {
            /* setLooping(true)：同一连接内回到 0，不重新建连。 */
            pthread_mutex_lock(&pj->op);
            if (ms_ff_seek(pj->ff, 0) == MS_OK) {
                (void)ms_hw_player_seek(pj->p, 0);
                pthread_mutex_unlock(&pj->op);
                continue;
            }
            pthread_mutex_unlock(&pj->op);
        }
        if (st == MS_EOS || st == MS_ERR_CANCELED) {
            break;
        }
        if (st == MS_ERR_TIMEOUT) {
            set_last(pj, "timeout while pulling");
            break;
        }
        if (st != MS_OK) {
            snprintf(buf, sizeof(buf), "ms_ff_pull failed: %s", ms_status_name(st));
            set_last(pj, buf);
            break;
        }
        pthread_mutex_lock(&pj->op);
        sync_ff_audio_to_player(pj);
        {
            ms_event_trim tr;
            ms_event_trim_default(&tr);
            if (ev.release_id) {
                (void)ms_ff_event_trim(pj->ff, ev.release_id, &tr);
            }
            (void)ms_hw_player_note_event_trim(pj->p, tr.skip_samples_start,
                                               tr.skip_samples_end);
        }
        pthread_mutex_unlock(&pj->op);
        st = feed_retry_jni(pj, &ev, ms_ff_time_origin_90k(pj->ff));
        if (st == MS_LIMIT || st == MS_AGAIN) {
            if (ev.release_id) {
                (void)ms_ff_release(pj->ff, ev.release_id);
            }
            if (atomic_load(&pj->stop)) {
                set_last(pj, "backpressure/surface wait unresolved; AU not silently submitted");
                break;
            }
            /* 待处理 seek/选轨或短暂背压：释放当前 AU，回到循环顶部消费命令，不结束会话。 */
            continue;
        }
        if (ev.release_id) {
            (void)ms_ff_release(pj->ff, ev.release_id);
        }
        if (ev.type == MS_EVENT_AU && ev.track.codec == MS_CODEC_H264) {
            au++;
        }
        pthread_mutex_lock(&pj->op);
        (void)ms_hw_player_drive(pj->p);
        pthread_mutex_unlock(&pj->op);
        if (st != MS_OK && st != MS_ERR_UNSUPPORTED) {
            snprintf(buf, sizeof(buf), "feed failed: %s", ms_status_name(st));
            set_last(pj, buf);
            break;
        }
    }
    drain_after_session(pj);
    pthread_mutex_lock(&pj->op);
    format_stats(buf, sizeof(buf), pj->p,
                 "ms_ff demux into player; request_e2e includes open/probe; not device paint.");
    pthread_mutex_unlock(&pj->op);
    {
        char all[900];
        snprintf(all, sizeof(all), "video_au=%d stop=%d. %s", au,
                 atomic_load(&pj->stop), buf);
        set_last(pj, all);
    }
}
#endif /* MS_HW_HAVE_FF */

#if defined(MS_SINGLE_SO) && !defined(MS_HW_HAVE_FF) && !defined(MS_SINGLE_FFADEC)
/* 纯自研网络/容错输入：URL（http/https/HLS/DASH/udp/rtp/rtmp/rtsp）或 file://（本地 MKV/PS/伪装头）。
 * ms_netsrc 独立线程下载/预加载，本线程把字节推进 session 并喂播放器。不记录 URL。 */
static void run_one_net(player_jni *pj, char *url)
{
    ms_session *s = NULL;
    ms_netsrc *ns = NULL;
    ms_netsrc_opts o;
    char out[768];
    int au = 0, eos = 0;
    ms_status st;
    begin_session(pj);
    if (url == NULL || url[0] == 0) {
        set_last(pj, "empty url");
        free(url);
        return;
    }
    ms_netsrc_opts_default(&o);
    if (pj->ns_ahead_ms > 0) o.preload_ahead_ms = pj->ns_ahead_ms;
    if (pj->ns_max_bytes > 0) o.preload_max_bytes = (size_t)pj->ns_max_bytes;
reopen:
    if (ms_session_create(NULL, NULL, &s) != MS_OK) {
        set_last(pj, "session create failed");
        free(url);
        return;
    }
    if (ms_netsrc_open(url, &o, s, &ns) != MS_OK) {
        ms_session_destroy(s);
        set_last(pj, "UNSUPPORTED: netsrc open failed (url hidden)");
        free(url);
        return;
    }
    pthread_mutex_lock(&pj->mu);
    pj->ns = ns;
    pthread_mutex_unlock(&pj->mu);
    if (atomic_load(&pj->stop)) ms_netsrc_cancel(ns); /* stop 早于登记时补一次取消 */
    pthread_mutex_lock(&pj->op);
    pj->sess = s;
    pthread_mutex_unlock(&pj->op);
    while (!atomic_load(&pj->stop)) {
        ms_event ev;
        int paused = 0, need_still = 0;
        apply_pending_surface(pj);
        pthread_mutex_lock(&pj->mu);
        pj->has_pending_seek = 0; /* 网络流（纯自研）不支持 seek：丢弃请求，不假装成功 */
        pthread_mutex_unlock(&pj->mu);
        pthread_mutex_lock(&pj->op);
        paused = ms_hw_player_is_paused(pj->p);
        if (paused) {
            (void)ms_hw_player_drive(pj->p);
            need_still = ms_hw_player_surface_bound(pj->p) &&
                         !ms_hw_player_has_presented_video(pj->p);
        }
        pthread_mutex_unlock(&pj->op);
        if (paused && !need_still && atomic_load(&pj->mp_opened)) {
            usleep(20000);
            continue;
        }
        memset(&ev, 0, sizeof(ev));
        st = ms_session_pull(s, &ev);
        if (st == MS_OK) {
            ms_event_trim tr;
            ms_event_trim_default(&tr);
            if (ev.release_id) {
                (void)ms_session_event_trim(s, ev.release_id, &tr);
            }
            pthread_mutex_lock(&pj->op);
            (void)ms_hw_player_note_event_trim(pj->p, tr.skip_samples_start, tr.skip_samples_end);
            pthread_mutex_unlock(&pj->op);
            if (ev.type == MS_EVENT_CODEC_CONFIG || ev.type == MS_EVENT_AU) {
                atomic_store(&pj->mp_opened, 1);
            }
            st = feed_retry_jni(pj, &ev, 0);
            if (ev.release_id) {
                (void)ms_session_release(s, ev.release_id);
            }
            if (st == MS_LIMIT || st == MS_AGAIN) {
                continue;
            }
            if (ev.type == MS_EVENT_AU) {
                au++;
            }
            pthread_mutex_lock(&pj->op);
            (void)ms_hw_player_drive(pj->p);
            pthread_mutex_unlock(&pj->op);
            if (st != MS_OK && st != MS_ERR_UNSUPPORTED) {
                snprintf(out, sizeof(out), "feed failed: %s", ms_status_name(st));
                set_last(pj, out);
                break;
            }
            continue;
        }
        if (eos) {
            break;
        }
        st = ms_netsrc_drive(ns);
        if (st == MS_EOS) {
            eos = 1;
            continue;
        }
        if (st == MS_OK || st == MS_LIMIT) {
            if (st == MS_LIMIT) {
                pthread_mutex_lock(&pj->op);
                (void)ms_hw_player_drive(pj->p);
                pthread_mutex_unlock(&pj->op);
            }
            continue;
        }
        if (st == MS_AGAIN) {
            pthread_mutex_lock(&pj->op);
            (void)ms_hw_player_drive(pj->p);
            pthread_mutex_unlock(&pj->op);
            usleep(2000);
            continue;
        }
        snprintf(out, sizeof(out), "netsrc: %s (%s)", ms_status_name(st), ms_netsrc_last_error(ns));
        set_last(pj, out);
        break;
    }
    drain_after_session(pj);
    {
        ms_netsrc_stats nst;
        char rest[512];
        ms_netsrc_get_stats(ns, &nst);
        pthread_mutex_lock(&pj->op);
        pj->sess = NULL;
        format_stats(rest, sizeof(rest), pj->p, "ms_netsrc -> ms_session demux; not device paint.");
        pthread_mutex_unlock(&pj->op);
        snprintf(out, sizeof(out),
                 "net mode=%s probe=%d disguise=%d stripped=%llu seg=%d/%d/%d cache_peak=%zu ka=%d/%d bad=%llu session_au=%d. %s",
                 ms_netsrc_mode_name(nst.mode), nst.probe_fmt, nst.probe_disguise,
                 (unsigned long long)nst.stripped_bytes, nst.seg_demand, nst.seg_prefetched, nst.seg_hit,
                 nst.cache_peak, nst.http_opened, nst.http_reused, (unsigned long long)nst.bad_packets, au, rest);
    }
    if (eos == 0 || atomic_load(&pj->stop)) {
        set_last(pj, out);
    }
    pthread_mutex_lock(&pj->mu);
    pj->ns = NULL;
    pthread_mutex_unlock(&pj->mu);
    ms_netsrc_close(ns); /* 取消并回收线程与全部缓存 */
    ns = NULL;
    ms_session_destroy(s);
    s = NULL;
    if (eos && !atomic_load(&pj->stop) && atomic_load(&pj->mp_looping) && atomic_load(&pj->mp_opened)) {
        /* 循环：点播结束后重新打开（网络重连；直播流不会到 EOS） */
        pthread_mutex_lock(&pj->op);
        (void)ms_hw_player_flush(pj->p);
        pthread_mutex_unlock(&pj->op);
        eos = 0;
        goto reopen;
    }
    set_last(pj, out);
    free(url);
}

/* 本地文件首段字节探测：MKV/PS、图片伪装头或前导脏字节 → 走 netsrc(file://) 容错/转封装路径；
 * 其它（MP4/TS/FLV 干净文件）保持 ms_file_source（可 seek）。 */
static int path_needs_netsrc(const char *path)
{
    FILE *fp = fopen(path, "rb");
    uint8_t *b;
    size_t n;
    ms_probe_result pr;
    int r, need = 0;
    if (fp == NULL) {
        return 0;
    }
    b = (uint8_t *)malloc(256u * 1024u);
    if (b == NULL) {
        fclose(fp);
        return 0;
    }
    n = fread(b, 1, 256u * 1024u, fp);
    fclose(fp);
    memset(&pr, 0, sizeof(pr));
    r = ms_probe_payload(b, n, 1, &pr);
    free(b);
    if (r == 1 && (pr.offset > 0 || pr.fmt == MS_PF_MKV || pr.fmt == MS_PF_PS)) {
        need = 1;
    }
    return need;
}
#endif

#if defined(MS_SINGLE_SO) && defined(MS_HW_HAVE_FF)
/* 阶段 5 瘦身（路线 B 单 so）：本地文件与 URL 一样交给 FFmpeg（file: 协议），FFmpeg 解封装能力是自研
 * ms_session 的超集（另含 MKV/PS/AVI 等），因此不再编入自研 TS/MP4/FLV/HLS/DASH 解封装，功能不减。
 * by: Long <mycjzb@gmail.com> */
static void run_one_path(player_jni *pj, char *path)
{
    size_t l;
    char *u;
    if (path == NULL || path[0] == 0) {
        begin_session(pj);
        set_last(pj, "empty path");
        free(path);
        return;
    }
    l = strlen(path);
    u = (char *)malloc(l + 6);
    if (u == NULL) {
        begin_session(pj);
        set_last(pj, "nomem");
        free(path);
        return;
    }
    memcpy(u, "file:", 5);
    memcpy(u + 5, path, l + 1);
    free(path);
    run_one_url(pj, u, "", 15000); /* run_one_url 负责释放 u，不记录路径 */
}
#elif defined(MS_SINGLE_SO) && defined(MS_SINGLE_FFADEC)
/* msm3：解封装归 Media3；本 so 只提供 AVIO/ADEC，JNI 播控 demux 路径关闭。 */
static void run_one_path(player_jni *pj, char *path)
{
    begin_session(pj);
    set_last(pj, "UNSUPPORTED: msm3 uses Media3; native demux removed");
    free(path);
}
#elif defined(MS_SINGLE_SO)
/* 单 so：本地文件走 ms_file_source + ms_session_drive（有界读、不整文件进内存），
 * 渐进 MP4/MOV 可 seek；遵守暂停；循环时优先 session seek，不支持则本地重开（不是网络重连）。 */
static int path_open_session(const char *path, ms_file_source *fs, ms_session **out)
{
    ms_source src;
    memset(fs, 0, sizeof(*fs));
    fs->struct_size = sizeof(*fs);
    fs->fd = -1;
    *out = NULL;
    if (ms_file_source_open(fs, path) != MS_OK) {
        return -1;
    }
    if (ms_session_create(NULL, NULL, out) != MS_OK) {
        ms_file_source_close(fs);
        return -1;
    }
    ms_file_source_bind(fs, &src);
    if (ms_session_set_source(*out, &src) != MS_OK) {
        ms_session_destroy(*out);
        *out = NULL;
        ms_file_source_close(fs);
        return -1;
    }
    return 0;
}

static void run_one_path(player_jni *pj, char *path)
{
    ms_file_source fs;
    ms_session *s = NULL;
    char out[768];
    int au = 0, eos = 0;
    ms_status st;
    begin_session(pj);
    if (path == NULL || path[0] == 0) {
        set_last(pj, "empty path");
        free(path);
        return;
    }
#ifndef MS_HW_HAVE_FF
    if (path_needs_netsrc(path)) {
        size_t l = strlen(path);
        char *u = (char *)malloc(l + 8);
        if (u != NULL) {
            memcpy(u, "file://", 7);
            memcpy(u + 7, path, l + 1);
            free(path);
            run_one_net(pj, u);
            return;
        }
    }
#endif
    if (path_open_session(path, &fs, &s) != 0) {
        free(path);
        set_last(pj, "open failed (path hidden)");
        return;
    }
    pthread_mutex_lock(&pj->op);
    pj->sess = s;
    pthread_mutex_unlock(&pj->op);
    while (!atomic_load(&pj->stop)) {
        ms_event ev;
        int paused = 0, need_still = 0;
        apply_pending_surface(pj);
        if (apply_pending_seek(pj) != 0) {
            eos = 0;
            continue;
        }
        pthread_mutex_lock(&pj->op);
        paused = ms_hw_player_is_paused(pj->p);
        if (paused) {
            (void)ms_hw_player_drive(pj->p);
            need_still = ms_hw_player_surface_bound(pj->p) &&
                         !ms_hw_player_has_presented_video(pj->p);
        }
        pthread_mutex_unlock(&pj->op);
        /* 未识别前即使暂停也要继续解复用，否则 prepare 永远等不到 CODEC_CONFIG。 */
        if (paused && !need_still && atomic_load(&pj->mp_opened)) {
            usleep(20000);
            continue;
        }
        memset(&ev, 0, sizeof(ev));
        st = ms_session_pull(s, &ev);
        if (st == MS_OK) {
            ms_event_trim tr;
            ms_event_trim_default(&tr);
            if (ev.release_id) {
                (void)ms_session_event_trim(s, ev.release_id, &tr);
            }
            pthread_mutex_lock(&pj->op);
            (void)ms_hw_player_note_event_trim(pj->p, tr.skip_samples_start, tr.skip_samples_end);
            pthread_mutex_unlock(&pj->op);
            if (ev.type == MS_EVENT_CODEC_CONFIG || ev.type == MS_EVENT_AU) {
                atomic_store(&pj->mp_opened, 1);
            }
            st = feed_retry_jni(pj, &ev, 0);
            if (ev.release_id) {
                (void)ms_session_release(s, ev.release_id);
            }
            if (st == MS_LIMIT || st == MS_AGAIN) {
                continue;
            }
            if (ev.type == MS_EVENT_AU) {
                au++;
            }
            pthread_mutex_lock(&pj->op);
            (void)ms_hw_player_drive(pj->p);
            pthread_mutex_unlock(&pj->op);
            if (st != MS_OK && st != MS_ERR_UNSUPPORTED) {
                snprintf(out, sizeof(out), "feed failed: %s", ms_status_name(st));
                set_last(pj, out);
                break;
            }
            continue;
        }
        if (eos) {
            if (atomic_load(&pj->mp_looping) && atomic_load(&pj->mp_opened)) {
                pthread_mutex_lock(&pj->op);
                st = ms_session_seek(s, 0);
                if (st == MS_OK) {
                    (void)ms_hw_player_seek(pj->p, 0);
                    pthread_mutex_unlock(&pj->op);
                    eos = 0;
                    continue;
                }
                /* 不可 seek 的本地格式（TS/FLV/fMP4）：本地重开文件，非网络重连。 */
                pj->sess = NULL;
                ms_session_destroy(s);
                s = NULL;
                ms_file_source_close(&fs);
                (void)ms_hw_player_flush(pj->p);
                pthread_mutex_unlock(&pj->op);
                if (path_open_session(path, &fs, &s) != 0) {
                    set_last(pj, "loop reopen failed (path hidden)");
                    break;
                }
                pthread_mutex_lock(&pj->op);
                pj->sess = s;
                pthread_mutex_unlock(&pj->op);
                eos = 0;
                continue;
            }
            break;
        }
        st = ms_session_drive(s);
        if (st == MS_EOS) {
            eos = 1;
            continue;
        }
        if (st == MS_OK || st == MS_LIMIT) {
            continue;
        }
        if (st == MS_AGAIN) {
            usleep(2000);
            continue;
        }
        snprintf(out, sizeof(out), "session drive: %s", ms_status_name(st));
        set_last(pj, out);
        break;
    }
    drain_after_session(pj);
    pthread_mutex_lock(&pj->op);
    pj->sess = NULL;
    {
        char rest[512];
        format_stats(rest, sizeof(rest), pj->p,
                     "local ms_session(file source) demux; not device paint.");
        snprintf(out, sizeof(out), "session_au=%d. %s", au, rest);
    }
    pthread_mutex_unlock(&pj->op);
    set_last(pj, out);
    if (s != NULL) {
        ms_session_destroy(s);
        ms_file_source_close(&fs);
    }
    free(path);
}
#else
static void run_one_path(player_jni *pj, char *path)
{
    FILE *fp = NULL;
    uint8_t *buf = NULL;
    size_t cap = 0;
    ms_session *s = NULL;
    char out[768];
    int au = 0;
    begin_session(pj);
    if (path == NULL || path[0] == 0) {
        set_last(pj, "empty path");
        free(path);
        return;
    }
    fp = fopen(path, "rb");
    free(path);
    path = NULL;
    if (fp == NULL) {
        set_last(pj, "open failed (path hidden)");
        return;
    }
    for (;;) {
        uint8_t tmp[4096];
        size_t r;
        uint8_t *nb;
        if (atomic_load(&pj->stop)) {
            fclose(fp);
            free(buf);
            set_last(pj, "canceled during file read");
            return;
        }
        r = fread(tmp, 1, sizeof(tmp), fp);
        if (r == 0) {
            break;
        }
        nb = (uint8_t *)realloc(buf, cap + r);
        if (nb == NULL) {
            fclose(fp);
            free(buf);
            set_last(pj, "nomem");
            return;
        }
        buf = nb;
        memcpy(buf + cap, tmp, r);
        cap += r;
    }
    fclose(fp);
    if (ms_session_create(NULL, NULL, &s) != MS_OK) {
        free(buf);
        set_last(pj, "session create failed");
        return;
    }
    {
        size_t off = 0;
        while (off < cap && !atomic_load(&pj->stop)) {
            size_t used = 0;
            ms_status st = ms_session_push(s, buf + off, cap - off, &used);
            if (used == 0) {
                break;
            }
            off += used;
            if (st != MS_OK && st != MS_AGAIN && st != MS_LIMIT) {
                break;
            }
        }
        (void)ms_session_end_of_input(s);
    }
    for (;;) {
        ms_event ev;
        ms_status st;
        if (atomic_load(&pj->stop)) {
            break;
        }
        memset(&ev, 0, sizeof(ev));
        st = ms_session_pull(s, &ev);
        if (st != MS_OK) {
            break;
        }
        {
            ms_event_trim tr;
            ms_event_trim_default(&tr);
            if (ev.release_id) {
                (void)ms_session_event_trim(s, ev.release_id, &tr);
            }
            pthread_mutex_lock(&pj->op);
            (void)ms_hw_player_note_event_trim(pj->p, tr.skip_samples_start,
                                               tr.skip_samples_end);
            pthread_mutex_unlock(&pj->op);
        }
        st = feed_retry_jni(pj, &ev, 0);
        if (st == MS_LIMIT || st == MS_AGAIN) {
            if (ev.release_id) {
                (void)ms_session_release(s, ev.release_id);
            }
            if (atomic_load(&pj->stop)) {
                set_last(pj, "path backpressure unresolved; AU not silently dropped");
                drain_after_session(pj);
                ms_session_destroy(s);
                free(buf);
                return;
            }
            continue;
        }
        if (ev.release_id) {
            (void)ms_session_release(s, ev.release_id);
        }
        if (ev.type == MS_EVENT_AU) {
            au++;
        }
        pthread_mutex_lock(&pj->op);
        (void)ms_hw_player_drive(pj->p);
        pthread_mutex_unlock(&pj->op);
        if (st != MS_OK && st != MS_ERR_UNSUPPORTED) {
            break;
        }
    }
    drain_after_session(pj);
    pthread_mutex_lock(&pj->op);
    {
        char rest[512];
        format_stats(rest, sizeof(rest), pj->p,
                     "local ms_session demux; request_e2e includes file read; not device paint.");
        snprintf(out, sizeof(out), "path bytes=%zu session_au=%d. %s", cap, au, rest);
        set_last(pj, out);
    }
    pthread_mutex_unlock(&pj->op);
    ms_session_destroy(s);
    free(buf);
}

#endif /* MS_SINGLE_SO run_one_path */

static int ensure_worker(player_jni *pj);

static void *session_worker(void *arg)
{
    player_jni *pj = (player_jni *)arg;
    for (;;) {
        char *loc = NULL;
        char ca[512];
        int timeout, kind;
        pthread_mutex_lock(&pj->mu);
        while (!pj->has_pending && !pj->exit_req) {
            pthread_cond_wait(&pj->cv, &pj->mu);
        }
        if (pj->exit_req) {
            free(pj->pending_loc);
            pj->pending_loc = NULL;
            pj->has_pending = 0;
            pj->pending_kind = 0;
            pthread_mutex_unlock(&pj->mu);
            break;
        }
        loc = pj->pending_loc;
        pj->pending_loc = NULL;
        memcpy(ca, pj->pending_ca, sizeof(ca));
        timeout = pj->pending_timeout;
        kind = pj->pending_kind;
        pj->has_pending = 0;
        pj->pending_kind = 0;
        pthread_mutex_unlock(&pj->mu);
        atomic_store(&pj->stop, 0);
        if (kind == 1 || kind == 2) {
            atomic_store(&pj->mp_active, 1);
        }
        if (kind == 2) {
            run_one_path(pj, loc);
        } else if (kind == 1) {
#ifdef MS_HW_HAVE_FF
            run_one_url(pj, loc, ca, timeout);
#elif defined(MS_SINGLE_FFADEC)
            (void)ca;
            (void)timeout;
            begin_session(pj);
            set_last(pj, "UNSUPPORTED: msm3 uses Media3; native demux removed");
            free(loc);
#elif defined(MS_SINGLE_SO)
            (void)ca;
            (void)timeout;
            run_one_net(pj, loc); /* 路线 A：纯自研网络栈 */
#else
            (void)ca;
            (void)timeout;
            free(loc);
            set_last(pj, "UNSUPPORTED: libmsff not linked in this build");
#endif
        } else {
            free(loc);
        }
#ifdef MS_HW_HAVE_FF
        pthread_mutex_lock(&pj->op);
        if (pj->ff) {
            ms_ff_destroy(pj->ff);
            pj->ff = NULL;
        }
        pthread_mutex_unlock(&pj->op);
#endif
        if (kind == 1 || kind == 2) {
            atomic_fetch_add(&pj->mp_done, 1);
            atomic_store(&pj->mp_active, 0);
        }
    }
    if (ms_hw_reap_thread_exiting(&pj->mu, &pj->cv, &pj->th_finished,
                                 &pj->abandoned) == 1) {
        pj_cleanup_owned(pj);
    }
    return NULL;
}

static int ensure_worker(player_jni *pj)
{
    if (pj->th_alive) {
        return 0;
    }
    if (!ms_hw_can_spawn_worker()) {
        set_last(pj, "UNSUPPORTED: abandoned worker cap reached; refuse new worker");
        return -1;
    }
    if (pthread_create(&pj->th, NULL, session_worker, pj) != 0) {
        return -1;
    }
    pj->th_alive = 1;
    return 0;
}

static int queue_session(player_jni *pj, int kind, const char *loc,
                         const char *ca, int timeout_ms)
{
    int queued;
    request_stop_io(pj);
    if (pj->p != NULL) {
        pthread_mutex_lock(&pj->op);
        (void)ms_hw_player_mark_request(pj->p);
        pthread_mutex_unlock(&pj->op);
    }
    pthread_mutex_lock(&pj->mu);
    free(pj->pending_loc);
    pj->pending_loc = NULL;
    if (loc != NULL) {
        pj->pending_loc = (char *)malloc(strlen(loc) + 1);
        if (pj->pending_loc) {
            memcpy(pj->pending_loc, loc, strlen(loc) + 1);
        }
    }
    pj->pending_ca[0] = 0;
    if (ca != NULL && ca[0] != 0) {
        snprintf(pj->pending_ca, sizeof(pj->pending_ca), "%s", ca);
    }
    pj->pending_timeout = timeout_ms > 0 ? timeout_ms : 15000;
    queued = pj->pending_loc != NULL;
    pj->pending_kind = queued ? kind : 0;
    pj->has_pending = queued;
    pthread_cond_signal(&pj->cv);
    pthread_mutex_unlock(&pj->mu);
    if (!queued) {
        return -1;
    }
    if (ensure_worker(pj) != 0) {
        return -2;
    }
    return 0;
}

JNIEXPORT jlong JNICALL
Java_com_mycjzb_media_player_MsHwPlayerActivity_nativeCreate(JNIEnv *env, jobject thiz)
{
    player_jni *pj;
    ms_hw_player_opts o;
    (void)env;
    (void)thiz;
    if (!ms_hw_can_spawn_worker()) {
        return 0;
    }
    pj = (player_jni *)calloc(1, sizeof(*pj));
    if (pj == NULL) {
        return 0;
    }
    pthread_mutex_init(&pj->mu, NULL);
    pthread_mutex_init(&pj->op, NULL);
    pthread_cond_init(&pj->cv, NULL);
    atomic_init(&pj->stop, 0);
    pj->exit_req = 0;
    ms_hw_player_opts_default(&o);
    o.decoder.mock_mode = 0;
    o.decoder.allow_software = 0; /* 视频硬解 only，不因此放开软解 */
    o.allow_system_aac = 1;       /* 测试 APK：允许系统 AAC，典型无硬件 AAC 设备可出声 */
    o.audio_mock_mode = -1;
    o.wait_keyframe = 1;
    if (ms_hw_player_create(NULL, &o, &pj->p) != MS_OK) {
        pthread_cond_destroy(&pj->cv);
        pthread_mutex_destroy(&pj->op);
        pthread_mutex_destroy(&pj->mu);
        free(pj);
        return 0;
    }
    set_last(pj, "created");
    return (jlong)(intptr_t)pj;
}

JNIEXPORT jstring JNICALL
Java_com_mycjzb_media_player_MsHwPlayerActivity_nativeSetForceDecoder(JNIEnv *env, jobject thiz,
                                                          jlong h, jstring jname)
{
    player_jni *pj = pj_from(h);
    const char *name = NULL;
    char buf[256];
    ms_status st;
    (void)thiz;
    if (pj == NULL || pj->p == NULL) {
        return (*env)->NewStringUTF(env, "no player");
    }
    if (jname) {
        name = (*env)->GetStringUTFChars(env, jname, NULL);
    }
    pthread_mutex_lock(&pj->op);
    st = ms_hw_player_set_force_decoder_name(pj->p, name);
    pthread_mutex_unlock(&pj->op);
    if (name) {
        (*env)->ReleaseStringUTFChars(env, jname, name);
    }
    snprintf(buf, sizeof(buf),
             "force_decoder st=%s (test only; software is not hardware)",
             ms_status_name(st));
    return (*env)->NewStringUTF(env, buf);
}

JNIEXPORT jstring JNICALL
Java_com_mycjzb_media_player_MsHwPlayerActivity_nativeSetSurface(JNIEnv *env, jobject thiz,
                                                     jlong h, jobject surface)
{
    player_jni *pj = pj_from(h);
    char buf[256];
    (void)thiz;
    if (pj == NULL || pj->p == NULL) {
        return (*env)->NewStringUTF(env, "no player");
    }
    /* 在 UI 线程取出 ANativeWindow，入队后立刻返回；worker 持锁时再绑。
     * 避免 MediaCodec configure 占用 op 时把 UI 卡住导致 ANR / freezer。 */
    {
        ANativeWindow *nw = NULL;
        if (surface) {
            nw = ANativeWindow_fromSurface(env, surface);
        }
        pthread_mutex_lock(&pj->mu);
        if (pj->pending_win != NULL && pj->pending_win != nw) {
            ANativeWindow_release(pj->pending_win);
        }
        pj->pending_win = nw;
        replace_jsurf(env, &pj->pending_jsurf, surface);
        pj->has_pending_surface = 1;
        pthread_mutex_unlock(&pj->mu);
    }
    if (lock_op_ms(pj, 80)) {
        pthread_mutex_unlock(&pj->op);
        apply_pending_surface(pj);
        snprintf(buf, sizeof(buf), "set_surface st=OK win=%s (applied)",
                 pj->win ? "yes" : "no");
        return (*env)->NewStringUTF(env, buf);
    }
    snprintf(buf, sizeof(buf), "set_surface queued win=%s (worker will bind; UI not blocked)",
             surface ? "yes" : "no");
    return (*env)->NewStringUTF(env, buf);
}

JNIEXPORT jstring JNICALL
Java_com_mycjzb_media_player_MsHwPlayerActivity_nativeInjectDemo(JNIEnv *env, jobject thiz,
                                                     jlong h)
{
    player_jni *pj = pj_from(h);
    ms_event ev;
    char buf[768];
    (void)thiz;
    if (pj == NULL || pj->p == NULL) {
        return (*env)->NewStringUTF(env, "no player");
    }
    pthread_mutex_lock(&pj->op);
    fill_ev(&ev, MS_EVENT_CODEC_CONFIG, 0);
    (void)ms_hw_player_feed(pj->p, &ev, 0, NULL, NULL);
    fill_ev(&ev, MS_EVENT_AU, 0);
    (void)ms_hw_player_feed(pj->p, &ev, 0, NULL, NULL);
    fill_ev(&ev, MS_EVENT_AU, 1);
    (void)ms_hw_player_feed(pj->p, &ev, 0, NULL, NULL);
    (void)ms_hw_player_drive(pj->p);
    format_stats(buf, sizeof(buf), pj->p,
                 "demo AU is not a real bitstream; device MediaCodec may error. "
                 "Not a hardware-decode pass.");
    pthread_mutex_unlock(&pj->op);
    return (*env)->NewStringUTF(env, buf);
}

JNIEXPORT jstring JNICALL
Java_com_mycjzb_media_player_MsHwPlayerActivity_nativePlayPath(JNIEnv *env, jobject thiz,
                                                   jlong h, jstring jpath)
{
    player_jni *pj = pj_from(h);
    const char *path = NULL;
    int q;
    (void)thiz;
    if (pj == NULL || pj->p == NULL) {
        return (*env)->NewStringUTF(env, "no player");
    }
    if (jpath) {
        path = (*env)->GetStringUTFChars(env, jpath, NULL);
    }
    if (path == NULL || path[0] == 0) {
        if (path) {
            (*env)->ReleaseStringUTFChars(env, jpath, path);
        }
        return (*env)->NewStringUTF(env, "empty path");
    }
#ifdef MS_HW_HAVE_FF
    {
        char fileurl[768];
        if (strncmp(path, "file:", 5) == 0 || strncmp(path, "FILE:", 5) == 0) {
            snprintf(fileurl, sizeof(fileurl), "%s", path);
        } else {
            snprintf(fileurl, sizeof(fileurl), "file:%s", path);
        }
        q = queue_session(pj, 1, fileurl, NULL, 15000);
    }
#else
    q = queue_session(pj, 2, path, NULL, 15000);
#endif
    (*env)->ReleaseStringUTFChars(env, jpath, path);
    if (q == -1) {
        return (*env)->NewStringUTF(env, "nomem");
    }
    if (q != 0) {
        return (*env)->NewStringUTF(env, "thread failed");
    }
    return (*env)->NewStringUTF(env, "path session queued (async; path not logged; libmsff file: used when linked)");
}

JNIEXPORT jstring JNICALL
Java_com_mycjzb_media_player_MsHwPlayerActivity_nativeOpenUrl(JNIEnv *env, jobject thiz,
                                                  jlong h, jstring jurl,
                                                  jstring jca, jint timeout_ms)
{
    player_jni *pj = pj_from(h);
    (void)thiz;
    if (pj == NULL || pj->p == NULL) {
        return (*env)->NewStringUTF(env, "no player");
    }
#ifndef MS_HW_HAVE_FF
    (void)jurl;
    (void)jca;
    (void)timeout_ms;
    return (*env)->NewStringUTF(env, "UNSUPPORTED: libmsff not linked in this build");
#else
    {
        int q;
        const char *url = NULL;
        const char *ca = NULL;
        char ca_buf[512];
        ca_buf[0] = 0;
        if (jurl) {
            url = (*env)->GetStringUTFChars(env, jurl, NULL);
        }
        if (url == NULL || url[0] == 0) {
            if (url) {
                (*env)->ReleaseStringUTFChars(env, jurl, url);
            }
            return (*env)->NewStringUTF(env, "empty url");
        }
        if (jca) {
            ca = (*env)->GetStringUTFChars(env, jca, NULL);
            if (ca && ca[0]) {
                snprintf(ca_buf, sizeof(ca_buf), "%s", ca);
            }
            if (ca) {
                (*env)->ReleaseStringUTFChars(env, jca, ca);
            }
        }
        q = queue_session(pj, 1, url, ca_buf, timeout_ms > 0 ? (int)timeout_ms : 15000);
        (*env)->ReleaseStringUTFChars(env, jurl, url);
        if (q == -1) {
            return (*env)->NewStringUTF(env, "nomem");
        }
        if (q != 0) {
            return (*env)->NewStringUTF(env, "thread failed");
        }
    }
    return (*env)->NewStringUTF(env, "url session queued (async; url not logged)");
#endif
}

JNIEXPORT jboolean JNICALL
Java_com_mycjzb_media_player_MsHwPlayerActivity_nativeHasFf(JNIEnv *env, jobject thiz)
{
    (void)env;
    (void)thiz;
#ifdef MS_HW_HAVE_FF
    return JNI_TRUE;
#else
    return JNI_FALSE;
#endif
}

JNIEXPORT jstring JNICALL
Java_com_mycjzb_media_player_MsHwPlayerActivity_nativeDrive(JNIEnv *env, jobject thiz, jlong h)
{
    player_jni *pj = pj_from(h);
    char buf[3072];
    (void)thiz;
    if (pj == NULL || pj->p == NULL) {
        return (*env)->NewStringUTF(env, "no player");
    }
    if (!lock_op_ms(pj, 50)) {
        return (*env)->NewStringUTF(env, "busy (worker in decode/configure; UI not blocked)");
    }
    (void)ms_hw_player_drive(pj->p);
    format_stats(buf, sizeof(buf), pj->p, "drive only");
    pthread_mutex_unlock(&pj->op);
    return (*env)->NewStringUTF(env, buf);
}

JNIEXPORT jstring JNICALL
Java_com_mycjzb_media_player_MsHwPlayerActivity_nativeStats(JNIEnv *env, jobject thiz, jlong h)
{
    player_jni *pj = pj_from(h);
    char buf[4096];
    char stats[3072];
    char last[1024];
    (void)thiz;
    if (pj == NULL || pj->p == NULL) {
        return (*env)->NewStringUTF(env, "no player");
    }
    if (!lock_op_ms(pj, 50)) {
        return (*env)->NewStringUTF(env, "busy (worker in decode/configure; UI not blocked)");
    }
    format_stats(stats, sizeof(stats), pj->p, "");
    pthread_mutex_unlock(&pj->op);
    pthread_mutex_lock(&pj->mu);
    snprintf(last, sizeof(last), "%s", pj->last);
    pthread_mutex_unlock(&pj->mu);
    snprintf(buf, sizeof(buf), "%s\n%s", last, stats);
    return (*env)->NewStringUTF(env, buf);
}

JNIEXPORT void JNICALL
Java_com_mycjzb_media_player_MsHwPlayerActivity_nativeFlush(JNIEnv *env, jobject thiz, jlong h)
{
    player_jni *pj = pj_from(h);
    (void)env;
    (void)thiz;
    if (pj && pj->p) {
        pthread_mutex_lock(&pj->op);
        (void)ms_hw_player_flush(pj->p);
        pthread_mutex_unlock(&pj->op);
    }
}

JNIEXPORT void JNICALL
Java_com_mycjzb_media_player_MsHwPlayerActivity_nativeCancel(JNIEnv *env, jobject thiz, jlong h)
{
    player_jni *pj = pj_from(h);
    (void)env;
    (void)thiz;
    if (pj == NULL) {
        return;
    }
    request_stop_io(pj);
    if (pj->p && pthread_mutex_trylock(&pj->op) == 0) {
        (void)ms_hw_player_cancel(pj->p);
        pthread_mutex_unlock(&pj->op);
    }
}

JNIEXPORT void JNICALL
Java_com_mycjzb_media_player_MsHwPlayerActivity_nativeDestroy(JNIEnv *env, jobject thiz, jlong h)
{
    player_jni *pj = pj_from(h);
    (void)env;
    (void)thiz;
    if (pj == NULL) {
        return;
    }
    pthread_mutex_lock(&pj->mu);
    pj->exit_req = 1;
    pthread_cond_signal(&pj->cv);
    pthread_mutex_unlock(&pj->mu);
    request_stop_io(pj);
    /* 先取消 I/O。有界等待；到期不 join、不 free，避免 UI 无限阻塞与 UAF。
     * 残留线程在退出时自行释放（后台回收）。若 I/O 无法被打断，线程仍可能
     * 滞留到进程退出，但不会在超时后增长出无主对象被 UI 释放。 */
    if (pj->th_alive) {
        int own = ms_hw_reap_wait_or_abandon(&pj->mu, &pj->cv, &pj->th_finished,
                                             &pj->abandoned, 2000);
        if (own == 1) {
            pthread_join(pj->th, NULL);
            pj->th_alive = 0;
            pj_cleanup_owned(pj);
        } else if (own == 0) {
            pthread_detach(pj->th);
            pj->th_alive = 0;
            /* pj 所有权已交给工作线程，此处不得再访问。 */
        } else {
            /* 参数失败时仍尝试取消后 join，避免直接 free。 */
            pthread_join(pj->th, NULL);
            pj->th_alive = 0;
            pj_cleanup_owned(pj);
        }
    } else {
        pj_cleanup_owned(pj);
    }
}


static void format_tracks(char *dst, size_t cap, const ms_hw_player *p)
{
    ms_track_info ti[32];
    size_t n = 0, i;
    size_t off = 0;
    memset(ti, 0, sizeof(ti));
    if (p == NULL || dst == NULL || cap == 0) {
        if (dst && cap) dst[0] = 0;
        return;
    }
    if (ms_hw_player_list_tracks(p, ti, 32, &n) != MS_OK) {
        snprintf(dst, cap, "list_tracks failed");
        return;
    }
    off += (size_t)snprintf(dst + off, cap - off, "tracks=%u", (unsigned)n);
    for (i = 0; i < n && i < 32 && off + 8 < cap; i++) {
        const char *kn = (ti[i].kind == MS_TRACK_VIDEO) ? "video" :
                         (ti[i].kind == MS_TRACK_AUDIO) ? "audio" :
                         (ti[i].kind == MS_TRACK_SUBTITLE) ? "sub" : "other";
        off += (size_t)snprintf(dst + off, cap - off,
                                " | id=%u kind=%s codec=%s lang=%s name=%s def=%d forced=%d sel=%d",
                                (unsigned)ti[i].id, kn, ms_codec_name(ti[i].codec),
                                ti[i].language ? ti[i].language : "-",
                                ti[i].name ? ti[i].name : "-",
                                ti[i].is_default, ti[i].is_forced, ti[i].selected);
    }
}

JNIEXPORT jstring JNICALL
Java_com_mycjzb_media_player_MsHwPlayerActivity_nativeListTracks(JNIEnv *env, jobject thiz, jlong h)
{
    player_jni *pj = pj_from(h);
    char buf[1536];
    (void)thiz;
    if (pj == NULL || pj->p == NULL) {
        return (*env)->NewStringUTF(env, "no player");
    }
    pthread_mutex_lock(&pj->op);
    format_tracks(buf, sizeof(buf), pj->p);
    pthread_mutex_unlock(&pj->op);
    return (*env)->NewStringUTF(env, buf);
}

/* 选轨公共实现（演示 Activity 与 com.mycjzb.media.player.MediaPlayer 共用，by: Long <mycjzb@gmail.com>）。
 * 播放器侧立即生效（stats / 清 ASS），demux 选择仍可异步入队。
 * 不得只入队等到 feed 成功才提交，否则背压下命令永不完成。
 * 调用线程最多等 wait_ms 拿锁，避免 configure 期间 ANR；拿不到锁时整条命令交给 worker。
 * 返回播放器侧状态；*queued=1 表示已交给 worker 异步执行。msg 可为 NULL。 */
static ms_status pj_select_track(player_jni *pj, uint32_t id, int on, int wait_ms,
                                 int *queued, char *msg, size_t cap)
{
    ms_status st;
    char dummy[8];
    if (msg == NULL || cap == 0) {
        msg = dummy;
        cap = sizeof(dummy);
    }
    if (queued != NULL) {
        *queued = 0;
    }
    if (pj == NULL || pj->p == NULL) {
        snprintf(msg, cap, "no player");
        return MS_ERR_STATE;
    }
    if ((id == MS_TRACK_ID_EXTERNAL_SUB || id == MS_TRACK_ID_AI_SUB) && !lock_op_ms(pj, wait_ms)) {
        /* 外挂/AI 字幕哨兵只改播放器侧开关，worker 队列不接（enqueue_select 会丢弃），
         * 拿不到锁时阻塞等锁，否则命令会静默丢失。 */
        pthread_mutex_lock(&pj->op);
    } else if (id != MS_TRACK_ID_EXTERNAL_SUB && id != MS_TRACK_ID_AI_SUB && !lock_op_ms(pj, wait_ms)) {
#ifdef MS_HW_HAVE_FF
        if (pj->ff != NULL && pj->th_alive) {
            enqueue_select(pj, id, on != 0);
            if (queued != NULL) {
                *queued = 1;
            }
        }
#endif
        snprintf(msg, cap,
                 "select id=%u on=%d queued (player busy; worker will apply; UI not blocked)",
                 (unsigned)id, on != 0);
        return MS_OK;
    }
    st = ms_hw_player_select_track(pj->p, id, on != 0);
    if (st == MS_OK) {
        /* 同步 applied_audio_id，避免 worker 稍后 sync 再切一次。 */
        ms_hw_player_stats stt;
        memset(&stt, 0, sizeof(stt));
        stt.struct_size = sizeof(stt);
        if (ms_hw_player_get_stats(pj->p, &stt) == MS_OK) {
            pj->applied_audio_id = stt.audio_track_id;
            pj->applied_audio_valid = 1;
        }
    }
    pthread_mutex_unlock(&pj->op);
#ifdef MS_HW_HAVE_FF
    if (pj->ff != NULL && pj->th_alive) {
        enqueue_select(pj, id, on != 0);
        if (queued != NULL) {
            *queued = 1;
        }
        snprintf(msg, cap, "select id=%u on=%d player=%s queued-demux (async; not device sound)",
                 (unsigned)id, on != 0, ms_status_name(st));
        return st;
    }
    if (pj->ff != NULL) {
        ms_status fst;
        pthread_mutex_lock(&pj->op);
        fst = ms_ff_select_track(pj->ff, id, on != 0);
        if (fst == MS_OK) {
            sync_ff_audio_to_player(pj);
        }
        pthread_mutex_unlock(&pj->op);
        snprintf(msg, cap, "select id=%u on=%d source=%s (no worker; not device sound)",
                 (unsigned)id, on != 0, ms_status_name(fst));
        return fst;
    }
#endif
    snprintf(msg, cap, "select id=%u on=%d -> %s (not device A/V acceptance)",
             (unsigned)id, on != 0, ms_status_name(st));
    return st;
}

JNIEXPORT jstring JNICALL
Java_com_mycjzb_media_player_MsHwPlayerActivity_nativeSelectTrack(JNIEnv *env, jobject thiz, jlong h,
                                                      jint id, jint on)
{
    player_jni *pj = pj_from(h);
    char buf[256];
    (void)thiz;
    if (pj == NULL || pj->p == NULL) {
        return (*env)->NewStringUTF(env, "no player");
    }
    /* UI 最多等 80ms。 */
    (void)pj_select_track(pj, (uint32_t)id, on != 0, 80, NULL, buf, sizeof(buf));
    return (*env)->NewStringUTF(env, buf);
}

JNIEXPORT jstring JNICALL
Java_com_mycjzb_media_player_MsHwPlayerActivity_nativeClearSubtitles(JNIEnv *env, jobject thiz, jlong h)
{
    player_jni *pj = pj_from(h);
    ms_status st;
    char buf[192];
    (void)thiz;
    if (pj == NULL || pj->p == NULL) {
        return (*env)->NewStringUTF(env, "no player");
    }
    if (!lock_op_ms(pj, 80)) {
        snprintf(buf, sizeof(buf), "clear_subtitles queued-busy (UI hidden already; worker will clear)");
        return (*env)->NewStringUTF(env, buf);
    }
    st = ms_hw_player_clear_subtitles(pj->p);
    pthread_mutex_unlock(&pj->op);
#ifdef MS_HW_HAVE_FF
    if (pj->ff != NULL && pj->th_alive) {
        /* 再入队关掉带内字幕：遍历由 Java disableSelectedSubs 负责；这里保证 ASS 已清。 */
        (void)st;
    }
#endif
    snprintf(buf, sizeof(buf), "clear_subtitles -> %s (overlay cleared immediately; not device paint)",
             ms_status_name(st));
    return (*env)->NewStringUTF(env, buf);
}

/* 当前播放时刻的字幕 cue（调用方已持 pj->op）。演示 Activity 与 MediaPlayer 共用，
 * 避免每个 JNI 入口各写一遍 struct_size 初始化 + subtitles_at。返回条数（<=cap）。
 * by: Long <mycjzb@gmail.com> */
static size_t pj_cues_now_locked(player_jni *pj, ms_sub_cue *cues, size_t cap)
{
    size_t n = 0, i;
    uint64_t t;
    if (pj == NULL || pj->p == NULL || cues == NULL || cap == 0) {
        return 0;
    }
    /* 原 nativeCurrentCue 里对 ms_ff_subtitles_at(cap=0) 的调用是只读查询、结果未用，已去掉。 */
    t = ms_hw_player_last_media_90k(pj->p);
    memset(cues, 0, cap * sizeof(cues[0]));
    for (i = 0; i < cap; i++) {
        cues[i].struct_size = sizeof(cues[i]);
    }
    if (ms_hw_player_subtitles_at(pj->p, t, cues, cap, &n) != MS_OK) {
        n = 0;
    }
    return n > cap ? cap : n;
}

/* 文字 cue 判定：位图不得当文字拷贝。 */
static int pj_cue_is_text(const ms_sub_cue *c)
{
    return c != NULL && (c->style_flags & MS_SUB_STYLE_HAS_BITMAP) == 0 &&
           c->bitmap_rgba_size == 0 && c->text != NULL && c->text_size > 0;
}

JNIEXPORT jstring JNICALL
Java_com_mycjzb_media_player_MsHwPlayerActivity_nativeCurrentCue(JNIEnv *env, jobject thiz, jlong h)
{
    player_jni *pj = pj_from(h);
    ms_sub_cue cues[8];
    size_t n;
    char buf[MS_SUB_CUE_TEXT_MAX + 8];
    (void)thiz;
    buf[0] = 0;
    if (pj == NULL || pj->p == NULL) {
        return (*env)->NewStringUTF(env, "");
    }
    pthread_mutex_lock(&pj->op);
    n = pj_cues_now_locked(pj, cues, 8);
    if (n > 0 && pj_cue_is_text(&cues[n - 1])) {
        const ms_sub_cue *top = &cues[n - 1];
        size_t cpy = top->text_size;
        if (cpy >= sizeof(buf)) {
            cpy = sizeof(buf) - 1u;
        }
        memcpy(buf, top->text, cpy);
        buf[cpy] = 0;
    }
    pthread_mutex_unlock(&pj->op);
    return (*env)->NewStringUTF(env, buf);
}

#ifndef MS_JNI_CUE_MAX
#define MS_JNI_CUE_MAX 8
#endif
#ifndef MS_JNI_CUE_META
#define MS_JNI_CUE_META 16
#endif
#ifndef MS_JNI_SPAN_META
#define MS_JNI_SPAN_META 8
#endif

/* 逻辑接入：把已去标签的 cue 样式填给 Java TextView。不是设备出画验收。 */
JNIEXPORT jint JNICALL
Java_com_mycjzb_media_player_MsHwPlayerActivity_nativeFillCues(JNIEnv *env, jobject thiz, jlong h,
                                                   jintArray jmeta, jobjectArray jtexts,
                                                   jintArray jspans)
{
    player_jni *pj = pj_from(h);
    ms_sub_cue cues[MS_JNI_CUE_MAX];
    size_t n = 0, i, span_w = 0;
    jint *meta = NULL;
    jint *spans = NULL;
    jsize meta_n, span_n, text_n;
    (void)thiz;
    if (pj == NULL || pj->p == NULL || jmeta == NULL || jtexts == NULL) {
        return 0;
    }
    meta_n = (*env)->GetArrayLength(env, jmeta);
    text_n = (*env)->GetArrayLength(env, jtexts);
    span_n = (jspans != NULL) ? (*env)->GetArrayLength(env, jspans) : 0;
    if (meta_n < MS_JNI_CUE_META || text_n <= 0) {
        return 0;
    }
    pthread_mutex_lock(&pj->op);
    n = pj_cues_now_locked(pj, cues, MS_JNI_CUE_MAX);
    if ((size_t)text_n < n) {
        n = (size_t)text_n;
    }
    if ((size_t)meta_n < n * MS_JNI_CUE_META) {
        n = (size_t)meta_n / MS_JNI_CUE_META;
    }
    meta = (*env)->GetIntArrayElements(env, jmeta, NULL);
    if (jspans != NULL && span_n > 0) {
        spans = (*env)->GetIntArrayElements(env, jspans, NULL);
    }
    if (meta == NULL) {
        pthread_mutex_unlock(&pj->op);
        return 0;
    }
    memset(meta, 0, (size_t)meta_n * sizeof(jint));
    for (i = 0; i < n; i++) {
        jint *m = meta + i * MS_JNI_CUE_META;
        char tmp[MS_SUB_CUE_TEXT_MAX + 1];
        size_t cpy = 0;
        jstring js;
        m[0] = cues[i].layer;
        m[1] = cues[i].alignment;
        m[2] = cues[i].font_size;
        m[3] = (jint)cues[i].color_argb;
        m[4] = cues[i].bold;
        m[5] = cues[i].italic;
        m[6] = cues[i].margin_l;
        m[7] = cues[i].margin_r;
        m[8] = cues[i].margin_v;
        m[9] = (jint)cues[i].style_flags;
        m[10] = cues[i].pos_x;
        m[11] = cues[i].pos_y;
        m[12] = cues[i].play_res_x;
        m[13] = cues[i].play_res_y;
        m[14] = (jint)cues[i].cap_flags;
        m[15] = (jint)cues[i].span_count;
        /* 位图不得当文字拷贝；HAS_BITMAP 由 style_flags 告知 Java。 */
        if (pj_cue_is_text(&cues[i])) {
            cpy = cues[i].text_size;
            if (cpy >= sizeof(tmp)) {
                cpy = sizeof(tmp) - 1u;
            }
            memcpy(tmp, cues[i].text, cpy);
        }
        tmp[cpy] = 0;
        js = (*env)->NewStringUTF(env, tmp);
        (*env)->SetObjectArrayElement(env, jtexts, (jsize)i, js);
        if (js != NULL) {
            (*env)->DeleteLocalRef(env, js);
        }
        if (spans != NULL && cues[i].span_count > 0) {
            uint32_t s;
            for (s = 0; s < cues[i].span_count; s++) {
                if (span_w + MS_JNI_SPAN_META > (size_t)span_n) {
                    break;
                }
                spans[span_w + 0] = (jint)cues[i].spans[s].start;
                spans[span_w + 1] = (jint)cues[i].spans[s].end;
                spans[span_w + 2] = (jint)cues[i].spans[s].style_flags;
                spans[span_w + 3] = (jint)cues[i].spans[s].color_argb;
                spans[span_w + 4] = cues[i].spans[s].font_size;
                spans[span_w + 5] = cues[i].spans[s].bold;
                spans[span_w + 6] = cues[i].spans[s].italic;
                spans[span_w + 7] = (jint)i;
                span_w += MS_JNI_SPAN_META;
            }
        }
    }
    (*env)->ReleaseIntArrayElements(env, jmeta, meta, 0);
    if (spans != NULL) {
        (*env)->ReleaseIntArrayElements(env, jspans, spans, 0);
    }
    pthread_mutex_unlock(&pj->op);
    return (jint)n;
}


JNIEXPORT jint JNICALL
Java_com_mycjzb_media_player_MsHwPlayerActivity_nativeFillBitmap(JNIEnv *env, jobject thiz, jlong h,
                                                     jint index, jintArray jgeom, jbyteArray jrgba)
{
    player_jni *pj = pj_from(h);
    ms_sub_cue cues[MS_JNI_CUE_MAX];
    size_t n = 0;
    jint *geom = NULL;
    jsize geom_n, rgba_n = 0;
    const ms_sub_cue *c;
    (void)thiz;
    if (pj == NULL || pj->p == NULL || jgeom == NULL || index < 0) {
        return 0;
    }
    geom_n = (*env)->GetArrayLength(env, jgeom);
    if (geom_n < 12) {
        return 0;
    }
    pthread_mutex_lock(&pj->op);
    n = pj_cues_now_locked(pj, cues, MS_JNI_CUE_MAX);
    if ((size_t)index >= n) {
        pthread_mutex_unlock(&pj->op);
        return 0;
    }
    c = &cues[index];
    if (c->bitmap_rgba == NULL || c->bitmap_rgba_size == 0 ||
        c->bitmap_w <= 0 || c->bitmap_h <= 0) {
        pthread_mutex_unlock(&pj->op);
        return 0;
    }
    geom = (*env)->GetIntArrayElements(env, jgeom, NULL);
    if (geom != NULL) {
        geom[0] = c->pos_x;
        geom[1] = c->pos_y;
        geom[2] = c->bitmap_w;
        geom[3] = c->bitmap_h;
        geom[4] = c->crop_x;
        geom[5] = c->crop_y;
        geom[6] = c->crop_w;
        geom[7] = c->crop_h;
        geom[8] = c->play_res_x;
        geom[9] = c->play_res_y;
        geom[10] = c->bitmap_kind;
        geom[11] = (jint)c->bitmap_rgba_size;
        (*env)->ReleaseIntArrayElements(env, jgeom, geom, 0);
    }
    if (jrgba != NULL) {
        rgba_n = (*env)->GetArrayLength(env, jrgba);
        if (rgba_n >= (jsize)c->bitmap_rgba_size) {
            (*env)->SetByteArrayRegion(env, jrgba, 0, (jsize)c->bitmap_rgba_size,
                                       (const jbyte *)c->bitmap_rgba);
        }
    }
    pthread_mutex_unlock(&pj->op);
    return (jint)c->bitmap_rgba_size;
}

/* 外挂字幕装载公共实现（演示 Activity 与 MediaPlayer.addTimedTextSource 共用）。
 * jdata 为空或长度 0 = 清除外挂轨。源（ms_ff）侧失败时不再改播放器侧，*src_fail=1。
 * 返回最终状态；*nbytes 回填字节数。by: Long <mycjzb@gmail.com> */
static ms_status pj_load_ext_sub(JNIEnv *env, player_jni *pj, jbyteArray jdata, int fmt,
                                 int *nbytes, int *src_fail)
{
    jbyte *bytes = NULL;
    jsize n = 0;
    ms_status st;
    if (nbytes != NULL) {
        *nbytes = 0;
    }
    if (src_fail != NULL) {
        *src_fail = 0;
    }
    if (pj == NULL || pj->p == NULL) {
        return MS_ERR_STATE;
    }
    if (jdata != NULL) {
        n = (*env)->GetArrayLength(env, jdata);
        if (n > 0) {
            bytes = (*env)->GetByteArrayElements(env, jdata, NULL);
            if (bytes == NULL) {
                return MS_ERR_NOMEM;
            }
        }
    }
    if (nbytes != NULL) {
        *nbytes = (int)n;
    }
    pthread_mutex_lock(&pj->op);
#ifdef MS_HW_HAVE_FF
    if (pj->ff != NULL) {
        st = ms_ff_set_external_subtitle(pj->ff,
                                         bytes ? (const uint8_t *)bytes : NULL,
                                         (size_t)(bytes ? n : 0),
                                         (ms_sub_format)fmt);
        if (st != MS_OK && bytes != NULL) {
            pthread_mutex_unlock(&pj->op);
            (*env)->ReleaseByteArrayElements(env, jdata, bytes, JNI_ABORT);
            if (src_fail != NULL) {
                *src_fail = 1;
            }
            return st;
        }
    }
#endif
    st = ms_hw_player_set_external_subtitle(pj->p,
                                            bytes ? (const uint8_t *)bytes : NULL,
                                            (size_t)(bytes ? n : 0),
                                            (ms_sub_format)fmt);
    pthread_mutex_unlock(&pj->op);
    if (bytes) {
        (*env)->ReleaseByteArrayElements(env, jdata, bytes, JNI_ABORT);
    }
    return st;
}

JNIEXPORT jstring JNICALL
Java_com_mycjzb_media_player_MsHwPlayerActivity_nativeLoadSubtitle(JNIEnv *env, jobject thiz, jlong h,
                                                       jbyteArray jdata, jint fmt)
{
    player_jni *pj = pj_from(h);
    int n = 0, src_fail = 0;
    ms_status st;
    char buf[192];
    (void)thiz;
    if (pj == NULL || pj->p == NULL) {
        return (*env)->NewStringUTF(env, "no player");
    }
    st = pj_load_ext_sub(env, pj, jdata, (int)fmt, &n, &src_fail);
    if (src_fail) {
        snprintf(buf, sizeof(buf), "source ext sub: %s", ms_status_name(st));
    } else {
        snprintf(buf, sizeof(buf), "ext sub bytes=%d fmt=%d -> %s (overlay only; not device paint acceptance)",
                 n, (int)fmt, ms_status_name(st));
    }
    return (*env)->NewStringUTF(env, buf);
}


JNIEXPORT jstring JNICALL
Java_com_mycjzb_media_player_MsHwPlayerActivity_nativePause(JNIEnv *env, jobject thiz, jlong h)
{
    player_jni *pj = pj_from(h);
    ms_status st;
    (void)thiz;
    if (pj == NULL || pj->p == NULL) {
        return (*env)->NewStringUTF(env, "no player");
    }
    pthread_mutex_lock(&pj->op);
    st = ms_hw_player_pause(pj->p);
    pthread_mutex_unlock(&pj->op);
    return (*env)->NewStringUTF(env, st == MS_OK ? "paused" : ms_status_name(st));
}

JNIEXPORT jstring JNICALL
Java_com_mycjzb_media_player_MsHwPlayerActivity_nativeResume(JNIEnv *env, jobject thiz, jlong h)
{
    player_jni *pj = pj_from(h);
    ms_status st;
    (void)thiz;
    if (pj == NULL || pj->p == NULL) {
        return (*env)->NewStringUTF(env, "no player");
    }
    pthread_mutex_lock(&pj->op);
    st = ms_hw_player_resume(pj->p);
    pthread_mutex_unlock(&pj->op);
    return (*env)->NewStringUTF(env, st == MS_OK ? "resumed" : ms_status_name(st));
}

JNIEXPORT jstring JNICALL
Java_com_mycjzb_media_player_MsHwPlayerActivity_nativeNotifyAudioFocus(JNIEnv *env, jobject thiz,
                                                          jlong h, jint ev)
{
    player_jni *pj = pj_from(h);
    ms_status st;
    char buf[96];
    (void)thiz;
    if (pj == NULL || pj->p == NULL) {
        return (*env)->NewStringUTF(env, "no player");
    }
    pthread_mutex_lock(&pj->op);
    st = ms_hw_player_notify_audio_focus(pj->p, (int)ev);
    pthread_mutex_unlock(&pj->op);
    if (st != MS_OK) {
        return (*env)->NewStringUTF(env, ms_status_name(st));
    }
    snprintf(buf, sizeof(buf), "audio_focus ev=%d paused=%d user_paused=%d",
             (int)ev,
             ms_hw_player_is_paused(pj->p),
             ms_hw_player_is_user_paused(pj->p));
    return (*env)->NewStringUTF(env, buf);
}

JNIEXPORT jstring JNICALL
Java_com_mycjzb_media_player_MsHwPlayerActivity_nativeSetRate(JNIEnv *env, jobject thiz, jlong h,
                                                  jint num, jint den)
{
    player_jni *pj = pj_from(h);
    ms_status st;
    char buf[256];
    ms_hw_rate_caps caps;
    (void)thiz;
    if (pj == NULL || pj->p == NULL) {
        return (*env)->NewStringUTF(env, "no player");
    }
    pthread_mutex_lock(&pj->op);
    memset(&caps, 0, sizeof(caps));
    caps.struct_size = sizeof(caps);
    (void)ms_hw_player_get_rate_caps(pj->p, &caps);
    st = ms_hw_player_set_rate(pj->p, (int)num, (int)den);
    pthread_mutex_unlock(&pj->op);
    snprintf(buf, sizeof(buf),
             "set_rate %d/%d -> %s; caps can_sync_av=%d can_keep_pitch=%d "
             "range=%d/%d..%d/%d api=%d mock_is_not_device=%d backend=%s (%s). "
             "Unsupported rates are errors; video-clock-only is not claimed as A/V sync.",
             (int)num, (int)den, ms_status_name(st),
             caps.can_sync_av, caps.can_keep_pitch,
             caps.min_num, caps.min_den, caps.max_num, caps.max_den,
             caps.api_level, caps.mock_is_not_device,
             caps.backend ? caps.backend : "?",
             caps.detail ? caps.detail : "");
    return (*env)->NewStringUTF(env, buf);
}

JNIEXPORT jstring JNICALL
Java_com_mycjzb_media_player_MsHwPlayerActivity_nativeRateCaps(JNIEnv *env, jobject thiz, jlong h)
{
    player_jni *pj = pj_from(h);
    ms_hw_rate_caps caps;
    char buf[256];
    (void)thiz;
    if (pj == NULL || pj->p == NULL) {
        return (*env)->NewStringUTF(env, "no player");
    }
    memset(&caps, 0, sizeof(caps));
    caps.struct_size = sizeof(caps);
    pthread_mutex_lock(&pj->op);
    (void)ms_hw_player_get_rate_caps(pj->p, &caps);
    pthread_mutex_unlock(&pj->op);
    snprintf(buf, sizeof(buf),
             "rate caps: can_sync_av=%d keep_pitch=%d %d/%d..%d/%d api=%d "
             "mock_is_not_device=%d backend=%s (%s)",
             caps.can_sync_av, caps.can_keep_pitch,
             caps.min_num, caps.min_den, caps.max_num, caps.max_den,
             caps.api_level, caps.mock_is_not_device,
             caps.backend ? caps.backend : "?",
             caps.detail ? caps.detail : "");
    return (*env)->NewStringUTF(env, buf);
}

static jstring queue_seek_abs(JNIEnv *env, player_jni *pj, int64_t time_90k)
{
    char buf[192];
    if (pj == NULL || pj->p == NULL) {
        return (*env)->NewStringUTF(env, "no player");
    }
    if (time_90k < 0) {
        return (*env)->NewStringUTF(env, "INVALID_ARG");
    }
    pthread_mutex_lock(&pj->op);
    if (ms_hw_player_is_seekable(pj->p) == 0) {
        pthread_mutex_unlock(&pj->op);
        return (*env)->NewStringUTF(env, "NOT_SEEKABLE");
    }
#ifdef MS_HW_HAVE_FF
    if (pj->ff != NULL && !ms_ff_is_seekable(pj->ff)) {
        pthread_mutex_unlock(&pj->op);
        return (*env)->NewStringUTF(env, "NOT_SEEKABLE");
    }
#endif
    pthread_mutex_unlock(&pj->op);
    pthread_mutex_lock(&pj->mu);
    pj->pending_seek_90k = time_90k;
    pj->has_pending_seek = 1;
    pthread_mutex_unlock(&pj->mu);
    snprintf(buf, sizeof(buf), "seek %lld queued (async; UI not blocked)",
             (long long)time_90k);
    return (*env)->NewStringUTF(env, buf);
}

JNIEXPORT jstring JNICALL
Java_com_mycjzb_media_player_MsHwPlayerActivity_nativeSeek(JNIEnv *env, jobject thiz, jlong h,
                                               jlong time90k)
{
    (void)thiz;
    return queue_seek_abs(env, pj_from(h), (int64_t)time90k);
}

JNIEXPORT jstring JNICALL
Java_com_mycjzb_media_player_MsHwPlayerActivity_nativeSeekRelative(JNIEnv *env, jobject thiz, jlong h,
                                                       jlong delta90k)
{
    player_jni *pj = pj_from(h);
    int64_t pos, tgt, delta;
    ms_hw_seek_range wr;
    (void)thiz;
    if (pj == NULL || pj->p == NULL) {
        return (*env)->NewStringUTF(env, "no player");
    }
    delta = (int64_t)delta90k;
    pthread_mutex_lock(&pj->op);
    if (ms_hw_player_is_seekable(pj->p) == 0) {
        pthread_mutex_unlock(&pj->op);
        return (*env)->NewStringUTF(env, "NOT_SEEKABLE");
    }
    pos = ms_hw_player_position_90k(pj->p);
#ifdef MS_HW_HAVE_FF
    if (pj->ff != NULL) {
        note_ff_seek_range(pj);
    }
#endif
    wr.struct_size = sizeof(wr);
    if (ms_hw_player_get_seek_range(pj->p, &wr) != MS_OK) {
        wr.struct_size = sizeof(wr);
        wr.start_90k = 0;
        wr.live_edge_90k = ms_hw_player_duration_90k(pj->p);
        wr.duration_90k = wr.live_edge_90k;
        wr.kind = MS_HW_SEEK_KIND_VOD;
    }
    pthread_mutex_unlock(&pj->op);
    if (delta > 0) {
        tgt = (pos > INT64_MAX - delta) ? INT64_MAX : pos + delta;
    } else if (delta < 0) {
        tgt = (pos < INT64_MIN - delta) ? INT64_MIN : pos + delta;
    } else {
        tgt = pos;
    }
    if (tgt < wr.start_90k) {
        tgt = wr.start_90k;
    }
    if (tgt < 0) {
        tgt = 0;
    }
    if (wr.live_edge_90k >= 0 && tgt > wr.live_edge_90k) {
        tgt = wr.live_edge_90k;
    } else if (wr.kind == MS_HW_SEEK_KIND_VOD && wr.duration_90k >= 0 &&
               tgt > wr.duration_90k) {
        tgt = wr.duration_90k;
    }
    return queue_seek_abs(env, pj, tgt);
}

JNIEXPORT jstring JNICALL
Java_com_mycjzb_media_player_MsHwPlayerActivity_nativePosition(JNIEnv *env, jobject thiz, jlong h)
{
    player_jni *pj = pj_from(h);
    char buf[256];
    int64_t pos, dur;
    int seekable, paused, num, den;
    ms_hw_seek_range wr;
    (void)thiz;
    if (pj == NULL || pj->p == NULL) {
        return (*env)->NewStringUTF(env, "no player");
    }
    pthread_mutex_lock(&pj->op);
#ifdef MS_HW_HAVE_FF
    if (pj->ff != NULL) {
        note_ff_seek_range(pj);
    }
#endif
    pos = ms_hw_player_position_90k(pj->p);
    dur = ms_hw_player_duration_90k(pj->p);
    seekable = ms_hw_player_is_seekable(pj->p);
    paused = ms_hw_player_is_paused(pj->p);
    (void)ms_hw_player_get_rate(pj->p, &num, &den);
    wr.struct_size = sizeof(wr);
    if (ms_hw_player_get_seek_range(pj->p, &wr) != MS_OK) {
        wr.kind = 0;
        wr.start_90k = 0;
        wr.end_90k = -1;
        wr.live_edge_90k = -1;
        wr.safety_90k = 0;
    }
    pthread_mutex_unlock(&pj->op);
    snprintf(buf, sizeof(buf),
             "pos=%.3fs dur=%s seekable=%d kind=%d win=%.3fs..%.3fs edge=%.3fs "
             "safety=%.3fs paused=%d rate=%d/%d (not device paint/sound)",
             pos / 90000.0,
             dur < 0 ? "unknown" : "v",
             seekable, wr.kind,
             wr.start_90k / 90000.0,
             wr.end_90k / 90000.0,
             wr.live_edge_90k / 90000.0,
             wr.safety_90k / 90000.0,
             paused, num, den);
    if (dur >= 0) {
        snprintf(buf, sizeof(buf),
                 "pos=%.3fs dur=%.3fs seekable=%d kind=%d win=%.3fs..%.3fs edge=%.3fs "
                 "safety=%.3fs paused=%d rate=%d/%d (not device paint/sound)",
                 pos / 90000.0, dur / 90000.0, seekable, wr.kind,
                 wr.start_90k / 90000.0, wr.end_90k / 90000.0,
                 wr.live_edge_90k / 90000.0, wr.safety_90k / 90000.0,
                 paused, num, den);
    }
    return (*env)->NewStringUTF(env, buf);
}

JNIEXPORT jstring JNICALL
Java_com_mycjzb_media_player_MsHwPlayerActivity_nativeSeekLiveEdge(JNIEnv *env, jobject thiz, jlong h)
{
    char buf[192];
    player_jni *pj = pj_from(h);
    (void)thiz;
    if (pj == NULL || pj->p == NULL) {
        return (*env)->NewStringUTF(env, "no player");
    }
    pthread_mutex_lock(&pj->op);
    if (ms_hw_player_is_seekable(pj->p) == 0) {
        pthread_mutex_unlock(&pj->op);
        return (*env)->NewStringUTF(env, "NOT_SEEKABLE");
    }
#ifdef MS_HW_HAVE_FF
    if (pj->ff != NULL && !ms_ff_is_seekable(pj->ff)) {
        pthread_mutex_unlock(&pj->op);
        return (*env)->NewStringUTF(env, "NOT_SEEKABLE");
    }
#endif
    pthread_mutex_unlock(&pj->op);
    pthread_mutex_lock(&pj->mu);
    pj->pending_seek_live = 1;
    pj->has_pending_seek = 1;
    pthread_mutex_unlock(&pj->mu);
    snprintf(buf, sizeof(buf), "seek live-edge queued (async; UI not blocked)");
    return (*env)->NewStringUTF(env, buf);
}



JNIEXPORT jint JNICALL
Java_com_mycjzb_media_player_MsHwPlayerActivity_nativeAssAvailable(JNIEnv *env, jobject thiz, jlong h)
{
    player_jni *pj = pj_from(h);
    int loaded = 0, has = 0;
    (void)env;
    (void)thiz;
    if (pj == NULL || pj->p == NULL) {
        return 0;
    }
    if (!lock_op_ms(pj, 50)) {
        return 0;
    }
    loaded = ms_hw_player_ass_render_loaded(pj->p);
    has = ms_hw_player_ass_has_script(pj->p);
    pthread_mutex_unlock(&pj->op);
    if (!loaded) {
        return 0;
    }
    return has ? 2 : 1;
}

JNIEXPORT jstring JNICALL
Java_com_mycjzb_media_player_MsHwPlayerActivity_nativeAssSetCanvas(JNIEnv *env, jobject thiz, jlong h,
                                                       jint canvas_w, jint canvas_h)
{
    player_jni *pj = pj_from(h);
    ms_status st;
    char buf[64];
    (void)thiz;
    if (pj == NULL || pj->p == NULL) {
        return (*env)->NewStringUTF(env, "no player");
    }
    if (!lock_op_ms(pj, 50)) {
        return (*env)->NewStringUTF(env, "busy");
    }
    st = ms_hw_player_ass_set_canvas(pj->p, (int)canvas_w, (int)canvas_h);
    pthread_mutex_unlock(&pj->op);
    snprintf(buf, sizeof(buf), "%s", st == MS_OK ? "ok" : ms_status_name(st));
    return (*env)->NewStringUTF(env, buf);
}

JNIEXPORT jint JNICALL
Java_com_mycjzb_media_player_MsHwPlayerActivity_nativeFillAssFrame(JNIEnv *env, jobject thiz, jlong h,
                                                       jintArray jgeom, jbyteArray jrgba)
{
    player_jni *pj = pj_from(h);
    ms_hw_ass_frame fr;
    uint64_t t;
    jint *geom = NULL;
    jsize geom_n;
    size_t need = 0;
    (void)thiz;
    if (pj == NULL || pj->p == NULL || jgeom == NULL) {
        return 0;
    }
    geom_n = (*env)->GetArrayLength(env, jgeom);
    if (geom_n < 12) {
        return 0;
    }
    if (!lock_op_ms(pj, 50)) {
        return 0;
    }
    t = ms_hw_player_last_media_90k(pj->p);
    memset(&fr, 0, sizeof(fr));
    fr.struct_size = sizeof(fr);
    if (ms_hw_player_ass_frame_at(pj->p, t, &fr) != MS_OK) {
        pthread_mutex_unlock(&pj->op);
        return 0;
    }
    need = fr.rgba_size;
    geom = (*env)->GetIntArrayElements(env, jgeom, NULL);
    if (geom != NULL) {
        geom[0] = fr.x;
        geom[1] = fr.y;
        geom[2] = fr.canvas_w;
        geom[3] = fr.canvas_h;
        geom[4] = fr.w;
        geom[5] = fr.h;
        geom[6] = fr.changed;
        geom[7] = fr.available;
        geom[8] = fr.has_script;
        geom[9] = fr.canvas_w;
        geom[10] = fr.canvas_h;
        geom[11] = (jint)need;
        (*env)->ReleaseIntArrayElements(env, jgeom, geom, 0);
    }
    if (jrgba != NULL && fr.rgba != NULL && need > 0) {
        jsize rgba_n = (*env)->GetArrayLength(env, jrgba);
        if (rgba_n >= (jsize)need) {
            (*env)->SetByteArrayRegion(env, jrgba, 0, (jsize)need, (const jbyte *)fr.rgba);
        }
    }
    pthread_mutex_unlock(&pj->op);
    return (jint)need;
}

JNIEXPORT jstring JNICALL
Java_com_mycjzb_media_player_MsHwPlayerActivity_nativeAiStatus(JNIEnv *env, jobject thiz, jlong h)
{
    player_jni *pj = pj_from(h);
    const char *s;
    (void)thiz;
    if (pj == NULL || pj->p == NULL) {
        return (*env)->NewStringUTF(env, "no player");
    }
    pthread_mutex_lock(&pj->op);
    s = ms_hw_player_ai_status(pj->p);
    pthread_mutex_unlock(&pj->op);
    return (*env)->NewStringUTF(env, s ? s : "disabled");
}

JNIEXPORT jstring JNICALL
Java_com_mycjzb_media_player_MsHwPlayerActivity_nativeAiConfigure(JNIEnv *env, jobject thiz, jlong h,
                                                      jint vendor, jstring jsrc, jstring jtgt)
{
    player_jni *pj = pj_from(h);
    const char *src = NULL, *tgt = NULL;
    ms_status st;
    char buf[160];
    (void)thiz;
    if (pj == NULL || pj->p == NULL) {
        return (*env)->NewStringUTF(env, "no player");
    }
    if (jsrc != NULL) {
        src = (*env)->GetStringUTFChars(env, jsrc, NULL);
    }
    if (jtgt != NULL) {
        tgt = (*env)->GetStringUTFChars(env, jtgt, NULL);
    }
    pthread_mutex_lock(&pj->op);
    st = ms_hw_player_ai_configure(pj->p, (int)vendor, src, tgt);
    pthread_mutex_unlock(&pj->op);
    if (jsrc != NULL && src != NULL) {
        (*env)->ReleaseStringUTFChars(env, jsrc, src);
    }
    if (jtgt != NULL && tgt != NULL) {
        (*env)->ReleaseStringUTFChars(env, jtgt, tgt);
    }
    if (st == MS_OK) {
        snprintf(buf, sizeof(buf), "ok %s", ms_hw_player_ai_status(pj->p));
    } else {
        snprintf(buf, sizeof(buf), "%s", ms_status_name(st));
    }
    return (*env)->NewStringUTF(env, buf);
}

JNIEXPORT jstring JNICALL
Java_com_mycjzb_media_player_MsHwPlayerActivity_nativeAiSetEnabled(JNIEnv *env, jobject thiz, jlong h,
                                                       jint on)
{
    player_jni *pj = pj_from(h);
    ms_status st;
    char buf[192];
    const char *s;
    (void)thiz;
    if (pj == NULL || pj->p == NULL) {
        return (*env)->NewStringUTF(env, "no player");
    }
    pthread_mutex_lock(&pj->op);
    st = ms_hw_player_ai_set_enabled(pj->p, on ? 1 : 0);
    s = ms_hw_player_ai_status(pj->p);
    pthread_mutex_unlock(&pj->op);
    snprintf(buf, sizeof(buf), "%s enabled=%d status=%s",
             st == MS_OK ? "ok" : ms_status_name(st), on ? 1 : 0,
             s ? s : "disabled");
    return (*env)->NewStringUTF(env, buf);
}

#ifdef MS_SINGLE_SO
#include "ms_hw_mediaplayer_jni.inc"
#endif

#endif /* __ANDROID__ */
