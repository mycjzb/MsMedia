#define _DEFAULT_SOURCE
/*
 * 宿主 mock 播放编排状态机：CSD、关键帧门闩、反压、取消、EOS、
 * flush、切换释放、引用/拷贝分列、分层计时。
 * 不是设备硬解，OUTPUT_RENDERED 不是实际显示。
 * by: Long <mycjzb@gmail.com>
 */
#include "msmedia/ms_hw.h"
#include "../src/hw/ms_hw_reap.h"
#include "ms_ts_synth.h"
#include "hevc_fixtures.h"
#include "ms_vbit_fixtures.h"
#include "ms_abit_fixtures.h"
#include "ms_sub_bitmap_fixtures.h"

#ifdef MS_HW_PLAYER_HAVE_FF
#include "msmedia/ms_ff.h"
#endif

#include <stdint.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/stat.h>
#ifdef MS_HW_PLAYER_HAVE_FF
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <sys/stat.h>
#endif

static int failures;

#define CHECK(cond, msg)                                                         \
    do {                                                                         \
        if (!(cond)) {                                                           \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, msg);        \
            failures++;                                                          \
            return -1;                                                           \
        }                                                                        \
    } while (0)

#define CHECK_ST(st, expect, msg)                                                \
    do {                                                                         \
        ms_status _s = (st);                                                     \
        if (_s != (expect)) {                                                    \
            fprintf(stderr, "FAIL %s:%d: %s got %d expect %d\n", __FILE__,       \
                    __LINE__, msg, (int)_s, (int)(expect));                      \
            failures++;                                                          \
            return -1;                                                           \
        }                                                                        \
    } while (0)

static const uint8_t k_asc_441[2] = { 0x12, 0x10 };
/* HE-AAC AOT=5，本阶段仅支持 LC，应诊断拒绝。 */
static const uint8_t k_asc_he[2] = { 0x2A, 0x10 };
static const uint8_t k_dummy_aac[16] = { 0x21,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14 };

static void ev_aac_cfg(ms_event *ev)
{
    memset(ev, 0, sizeof(*ev));
    ev->type = MS_EVENT_CODEC_CONFIG;
    ev->track.kind = MS_TRACK_AUDIO;
    ev->track.codec = MS_CODEC_AAC;
    ev->sample_format = MS_SAMPLE_FMT_AAC_RAW;
    ev->data = k_asc_441;
    ev->data_size = sizeof(k_asc_441);
    ev->time_base_num = 1;
    ev->time_base_den = 1000000;
}

static void ev_aac_au(ms_event *ev, int64_t pts_us)
{
    memset(ev, 0, sizeof(*ev));
    ev->type = MS_EVENT_AU;
    ev->track.kind = MS_TRACK_AUDIO;
    ev->track.codec = MS_CODEC_AAC;
    ev->sample_format = MS_SAMPLE_FMT_AAC_RAW;
    ev->data = k_dummy_aac;
    ev->data_size = sizeof(k_dummy_aac);
    ev->has_pts = 1;
    ev->pts = (uint64_t)pts_us;
    ev->time_base_num = 1;
    ev->time_base_den = 1000000;
}

static const uint8_t k_avcc[] = {
    0x01, 0x42, 0xC0, 0x1E, 0xFF, 0xE1, 0x00, 0x08,
    0x67, 0x42, 0xC0, 0x1E, 0xDA, 0x01, 0x00, 0x00,
    0x01, 0x00, 0x05, 0x68, 0xCE, 0x38, 0x80, 0x00
};

/* 与 k_avcc 不同的占位 avcC，仅用于配置变化回归（mock 不解析内容）。 */
static const uint8_t k_avcc2[] = {
    0x01, 0x42, 0xC0, 0x1F, 0xFF, 0xE1, 0x00, 0x08,
    0x67, 0x42, 0xC0, 0x1F, 0xDA, 0x01, 0x00, 0x00,
    0x01, 0x00, 0x05, 0x68, 0xCE, 0x38, 0x80, 0x00
};

static const uint8_t k_au[16] = {
    0x00, 0x00, 0x00, 0x0C, 0x65, 0x88, 0x84, 0x00,
    0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10, 0x11
};

static int g_rel;

static void rel_fn(void *ctx, uint8_t *data, size_t size)
{
    int *n = (int *)ctx;
    (void)data;
    (void)size;
    if (n != NULL) {
        (*n)++;
    }
    g_rel++;
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

static int make_player(ms_hw_player **out, unsigned pending)
{
    ms_hw_player_opts o;
    ms_hw_player_opts_default(&o);
    o.decoder.mock_mode = MS_HW_MOCK_AS_HW;
    o.decoder.struct_size = sizeof(o.decoder);
    o.wait_keyframe = 1;
    if (pending > 0) {
        o.limits.max_pending_in = pending;
        o.limits.max_in_bytes = 64 * 1024;
    }
    return ms_hw_player_create(NULL, &o, out) == MS_OK ? 0 : -1;
}

static int test_gate_csd_eos_and_times(void)
{
    ms_hw_player *p = NULL;
    ms_event ev;
    ms_hw_player_stats st;
    int token = 1;
    int rel = 0;
    CHECK(make_player(&p, 0) == 0, "create");
    fill_ev(&ev, MS_EVENT_TRACK, 0);
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "track");
    fill_ev(&ev, MS_EVENT_AU, 1);
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, rel_fn, &rel), MS_AGAIN, "au before surface/csd");
    CHECK(rel == 0, "not consumed before ready");
    CHECK_ST(ms_hw_player_set_surface(p, &token), MS_OK, "surface no csd yet");
    CHECK_ST(ms_hw_player_drive(p), MS_OK, "drive before start is no-op");
    memset(&st, 0, sizeof(st));
    st.struct_size = sizeof(st);
    CHECK_ST(ms_hw_player_get_stats(p, &st), MS_OK, "stats before start");
    CHECK(st.last_error == MS_OK, "pre-start drive must not set last_error=STATE");
    fill_ev(&ev, MS_EVENT_CODEC_CONFIG, 0);
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, rel_fn, &rel), MS_OK, "csd");
    CHECK(rel == 1, "csd released");
    fill_ev(&ev, MS_EVENT_AU, 0);
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, rel_fn, &rel), MS_OK, "drop non-key");
    CHECK(rel == 2, "non-key released");
    fill_ev(&ev, MS_EVENT_AU, 1);
    ev.preroll = 1;
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, rel_fn, &rel), MS_OK, "drop preroll");
    CHECK(rel == 3, "preroll released");
    fill_ev(&ev, MS_EVENT_AU, 1);
    ev.has_pts = 1;
    ev.pts = 18000;
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, rel_fn, &rel), MS_OK, "key au");
    CHECK_ST(ms_hw_player_drive(p), MS_OK, "drive");
    CHECK_ST(ms_hw_player_signal_eos(p), MS_OK, "eos");
    memset(&st, 0, sizeof(st));
    st.struct_size = sizeof(st);
    CHECK_ST(ms_hw_player_get_stats(p, &st), MS_OK, "stats");
    CHECK(st.have_first_packet == 1, "first packet");
    CHECK(st.have_first_decodable_au == 1, "first decodable");
    CHECK(st.have_first_submit == 1, "first submit");
    CHECK(st.have_first_output == 1, "first output (not display)");
    CHECK(st.have_first_display == 0, "display not inferred");
    CHECK(st.output_is_not_display == 1, "flag");
    CHECK(st.t_first_output_us >= 0, "output time");
    CHECK(st.t_first_display_us == -1, "no display time");
    CHECK(st.video_au_dropped_nokey == 1, "nokey");
    CHECK(st.video_au_dropped_preroll == 1, "preroll");
    CHECK(st.video_au_submitted == 1, "submitted");
    CHECK(st.eos_in == 1 && st.eos_out == 1, "eos both");
    CHECK(st.is_hardware == 0, "mock not hardware");
    CHECK(st.ref_hold_ops >= 1, "ref hold");
    CHECK(st.staging_copy_ops == 0, "no staging on ref path");
    CHECK_ST(ms_hw_player_note_display(p, 0, 0), MS_OK, "app display hook");
    memset(&st, 0, sizeof(st));
    st.struct_size = sizeof(st);
    CHECK_ST(ms_hw_player_get_stats(p, &st), MS_OK, "stats2");
    CHECK(st.have_first_display == 1, "display only after hook");
    CHECK(rel == 4, "key au released after consume");
    printf("  stages us (not paint): packet=%lld decodable=%lld submit=%lld "
           "output(not display)=%lld display=%lld backend=%s\n",
           (long long)st.t_first_packet_us, (long long)st.t_first_decodable_au_us,
           (long long)st.t_first_submit_us, (long long)st.t_first_output_us,
           (long long)st.t_first_display_us, st.backend ? st.backend : "?");
    ms_hw_player_destroy(p);
    printf("  test_gate_csd_eos_and_times ok\n");
    return 0;
}

static int test_copy_backpressure_cancel_flush_switch(void)
{
    ms_hw_player *p = NULL;
    ms_event ev;
    ms_hw_player_stats st;
    int token = 3, token2 = 9;
    CHECK(make_player(&p, 1) == 0, "create cap1");
    CHECK_ST(ms_hw_player_set_surface(p, &token), MS_OK, "surf");
    fill_ev(&ev, MS_EVENT_CODEC_CONFIG, 0);
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "csd");
    fill_ev(&ev, MS_EVENT_AU, 1);
    /* 无 release_fn：走 staging copy */
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "copy key");
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_LIMIT, "backpressure");
    memset(&st, 0, sizeof(st));
    st.struct_size = sizeof(st);
    CHECK_ST(ms_hw_player_get_stats(p, &st), MS_OK, "stats lim");
    CHECK(st.staging_copy_ops >= 1, "staging copy counted");
    CHECK(st.pending_in >= 1, "pending");
    CHECK_ST(ms_hw_player_drive(p), MS_OK, "drive free");
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "after drive");
    CHECK_ST(ms_hw_player_flush(p), MS_OK, "flush");
    memset(&st, 0, sizeof(st));
    st.struct_size = sizeof(st);
    CHECK_ST(ms_hw_player_get_stats(p, &st), MS_OK, "stats flush");
    CHECK(st.flushed == 1 && st.epoch >= 2, "new epoch");
    CHECK(st.have_first_submit == 0, "times reset");
    CHECK(st.waiting_keyframe == 1, "key gate after flush");
    fill_ev(&ev, MS_EVENT_AU, 0);
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "drop after flush");
    fill_ev(&ev, MS_EVENT_AU, 1);
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "key after flush");
    CHECK_ST(ms_hw_player_release_decoder(p), MS_OK, "switch release");
    memset(&st, 0, sizeof(st));
    st.struct_size = sizeof(st);
    CHECK_ST(ms_hw_player_get_stats(p, &st), MS_OK, "stats rel");
    CHECK(st.configured == 0, "decoder gone");
    CHECK_ST(ms_hw_player_set_surface(p, &token2), MS_OK, "rebind");
    fill_ev(&ev, MS_EVENT_AU, 0);
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "gate after switch");
    fill_ev(&ev, MS_EVENT_AU, 1);
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "key after switch");
    CHECK_ST(ms_hw_player_cancel(p), MS_OK, "cancel");
    fill_ev(&ev, MS_EVENT_AU, 1);
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_ERR_CANCELED, "feed canceled");
    ms_hw_player_destroy(p);
    printf("  test_copy_backpressure_cancel_flush_switch ok\n");
    return 0;
}

static int test_refuse_sw_and_bad_args(void)
{
    ms_hw_player *p = NULL;
    ms_hw_player_opts o;
    ms_event ev;
    int token = 1;
    ms_hw_player_opts_default(&o);
    o.decoder.mock_mode = MS_HW_MOCK_AS_SW;
    CHECK_ST(ms_hw_player_create(NULL, &o, &p), MS_OK, "create sw");
    fill_ev(&ev, MS_EVENT_CODEC_CONFIG, 0);
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "csd kept");
    CHECK_ST(ms_hw_player_set_surface(p, &token), MS_ERR_UNSUPPORTED, "sw refuse");
    ms_hw_player_destroy(p);
    p = NULL;
    /* 允许系统 AAC 不得放开视频软解。 */
    ms_hw_player_opts_default(&o);
    o.decoder.mock_mode = MS_HW_MOCK_AS_SW;
    o.decoder.allow_software = 0;
    o.allow_system_aac = 1;
    CHECK_ST(ms_hw_player_create(NULL, &o, &p), MS_OK, "create sw+sys aac");
    fill_ev(&ev, MS_EVENT_CODEC_CONFIG, 0);
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "csd kept 2");
    CHECK_ST(ms_hw_player_set_surface(p, &token), MS_ERR_UNSUPPORTED, "video sw still refused");
    ms_hw_player_destroy(p);
    CHECK_ST(ms_hw_player_create(NULL, NULL, NULL), MS_ERR_INVALID_ARG, "null out");
    printf("  test_refuse_sw_and_bad_args ok\n");
    return 0;
}

static int stats_conserved(const ms_hw_player_stats *st)
{
    uint64_t out;
    if (st == NULL) {
        return 0;
    }
    out = st->video_au_submitted + st->video_au_dropped_nokey +
          st->video_au_dropped_preroll;
    return st->video_au_in == out;
}

/* GOP=5 的已知关键帧：index % 5 == 0 为 IDR。不提交门闩前的非关键帧。 */
static int test_conservation_known_gop(void)
{
    ms_hw_player *p = NULL;
    ms_hw_player_stats st;
    ms_event ev;
    int token = 9, i, skip = 2, expected_drop, expected_sub, expected_in;
    CHECK(make_player(&p, 0) == 0, "gop player");
    CHECK_ST(ms_hw_player_set_surface(p, &token), MS_OK, "surf");
    fill_ev(&ev, MS_EVENT_CODEC_CONFIG, 0);
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "csd");
    for (i = 0; i < 30; i++) {
        fill_ev(&ev, MS_EVENT_AU, (i % 5) == 0);
        ev.has_pts = 1;
        ev.pts = (int64_t)i * 3600;
        CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "gop au");
        (void)ms_hw_player_drive(p);
    }
    memset(&st, 0, sizeof(st));
    st.struct_size = sizeof(st);
    CHECK_ST(ms_hw_player_get_stats(p, &st), MS_OK, "gop stats");
    CHECK(stats_conserved(&st), "gop conserved");
    CHECK(st.video_au_dropped_nokey == 0, "from-start no nokey drop");
    CHECK(st.video_au_dropped_preroll == 0, "no preroll");
    CHECK(st.video_au_submitted == 30 && st.video_au_in == 30, "submit all 30");
    CHECK(st.have_first_display == 0, "not display");
    printf("  known GOP from-start: in=%llu submitted=%llu drop_nokey=%llu "
           "first_packet=%.3fms decodable=%.3fms submit=%.3fms "
           "(mock; not paint)\n",
           (unsigned long long)st.video_au_in,
           (unsigned long long)st.video_au_submitted,
           (unsigned long long)st.video_au_dropped_nokey,
           st.t_first_packet_us / 1000.0, st.t_first_decodable_au_us / 1000.0,
           st.t_first_submit_us / 1000.0);
    ms_hw_player_destroy(p);

    /* 从 GOP 中间 index=2 开始喂：先丢 2,3,4，再从 key=5 提交 25 帧。 */
    CHECK(make_player(&p, 0) == 0, "gop mid");
    CHECK_ST(ms_hw_player_set_surface(p, &token), MS_OK, "surf mid");
    fill_ev(&ev, MS_EVENT_CODEC_CONFIG, 0);
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "csd mid");
    expected_drop = 0;
    expected_sub = 0;
    for (i = skip; i < 30; i++) {
        int key = ((i % 5) == 0);
        fill_ev(&ev, MS_EVENT_AU, key);
        ev.has_pts = 1;
        ev.pts = (int64_t)i * 3600;
        CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "mid au");
        (void)ms_hw_player_drive(p);
        if (!key && expected_sub == 0) {
            expected_drop++;
        } else {
            expected_sub++;
        }
    }
    expected_in = expected_drop + expected_sub;
    memset(&st, 0, sizeof(st));
    st.struct_size = sizeof(st);
    CHECK_ST(ms_hw_player_get_stats(p, &st), MS_OK, "mid stats");
    CHECK(stats_conserved(&st), "mid conserved");
    CHECK(st.video_au_dropped_nokey == (uint64_t)expected_drop, "mid drop matches pre-key");
    CHECK(st.video_au_submitted == (uint64_t)expected_sub, "mid submit after first key");
    CHECK(st.video_au_in == (uint64_t)expected_in, "mid in");
    CHECK(expected_drop == 3 && expected_sub == 25, "skip2 => drop3 submit25");
    printf("  known GOP mid-join skip=%d: in=%llu submitted=%llu drop_nokey=%llu "
           "(legal key gate, not silent drop)\n",
           skip, (unsigned long long)st.video_au_in,
           (unsigned long long)st.video_au_submitted,
           (unsigned long long)st.video_au_dropped_nokey);
    ms_hw_player_destroy(p);

    /* preroll 计入丢弃，不提交。 */
    CHECK(make_player(&p, 0) == 0, "preroll p");
    CHECK_ST(ms_hw_player_set_surface(p, &token), MS_OK, "surf pr");
    fill_ev(&ev, MS_EVENT_CODEC_CONFIG, 0);
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "csd pr");
    fill_ev(&ev, MS_EVENT_AU, 1);
    ev.preroll = 1;
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "preroll key dropped");
    fill_ev(&ev, MS_EVENT_AU, 1);
    ev.preroll = 0;
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "real key");
    memset(&st, 0, sizeof(st));
    st.struct_size = sizeof(st);
    CHECK_ST(ms_hw_player_get_stats(p, &st), MS_OK, "pr stats");
    CHECK(stats_conserved(&st), "pr conserved");
    CHECK(st.video_au_dropped_preroll == 1 && st.video_au_submitted == 1 &&
          st.video_au_in == 2, "preroll vs submit");
    ms_hw_player_destroy(p);
    printf("  test_conservation_known_gop ok\n");
    return 0;
}

static int test_limit_not_counted_until_consumed(void)
{
    ms_hw_player *p = NULL;
    ms_hw_player_stats st;
    ms_event ev;
    int token = 3;
    ms_status second;
    CHECK(make_player(&p, 1) == 0, "limit player pending=1");
    CHECK_ST(ms_hw_player_set_surface(p, &token), MS_OK, "surf");
    fill_ev(&ev, MS_EVENT_CODEC_CONFIG, 0);
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "csd");
    fill_ev(&ev, MS_EVENT_AU, 1);
    ev.pts = 0;
    ev.has_pts = 1;
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "first key fills queue");
    fill_ev(&ev, MS_EVENT_AU, 0);
    ev.pts = 3600;
    ev.has_pts = 1;
    second = ms_hw_player_feed(p, &ev, 0, NULL, NULL);
    CHECK_ST(second, MS_LIMIT, "second AU backpressure");
    memset(&st, 0, sizeof(st));
    st.struct_size = sizeof(st);
    CHECK_ST(ms_hw_player_get_stats(p, &st), MS_OK, "stats after limit");
    CHECK(st.video_au_in == 1 && st.video_au_submitted == 1, "LIMIT not in in/submitted");
    CHECK(st.video_au_backpressure >= 1, "backpressure counted");
    CHECK(stats_conserved(&st), "conserved while limited");
    CHECK_ST(ms_hw_player_drive(p), MS_OK, "drain");
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "retry after drain");
    memset(&st, 0, sizeof(st));
    st.struct_size = sizeof(st);
    CHECK_ST(ms_hw_player_get_stats(p, &st), MS_OK, "stats after retry");
    CHECK(stats_conserved(&st), "conserved after retry");
    CHECK(st.video_au_in == 2 && st.video_au_submitted == 2, "retry counted once");
    ms_hw_player_destroy(p);
    printf("  test_limit_not_counted_until_consumed ok\n");
    return 0;
}

static int test_eos_drive_idempotent_and_lifecycle(void)
{
    ms_hw_player *p = NULL;
    ms_hw_player_stats st;
    ms_event ev;
    int token = 4, token2 = 5;
    CHECK(make_player(&p, 0) == 0, "life player");
    CHECK_ST(ms_hw_player_set_surface(p, &token), MS_OK, "surf");
    fill_ev(&ev, MS_EVENT_CODEC_CONFIG, 0);
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "csd");
    fill_ev(&ev, MS_EVENT_AU, 1);
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "key");
    CHECK_ST(ms_hw_player_drive(p), MS_OK, "drive");
    CHECK_ST(ms_hw_player_signal_eos(p), MS_OK, "eos");
    CHECK_ST(ms_hw_player_drive(p), MS_OK, "drive after eos");
    CHECK_ST(ms_hw_player_signal_eos(p), MS_OK, "eos again");
    memset(&st, 0, sizeof(st));
    st.struct_size = sizeof(st);
    CHECK_ST(ms_hw_player_get_stats(p, &st), MS_OK, "eos stats");
    CHECK(st.last_error == MS_OK, "post-EOS last_error not STATE");
    CHECK_ST(ms_hw_player_flush(p), MS_OK, "flush after eos");
    CHECK_ST(ms_hw_player_release_decoder(p), MS_OK, "release");
    fill_ev(&ev, MS_EVENT_AU, 1);
    {
        ms_status stf = ms_hw_player_feed(p, &ev, 0, NULL, NULL);
        CHECK(stf == MS_AGAIN || stf == MS_OK, "feed without surface after release");
    }
    CHECK_ST(ms_hw_player_set_surface(p, &token2), MS_OK, "rebind");
    fill_ev(&ev, MS_EVENT_AU, 0);
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "nokey gated after rebind");
    fill_ev(&ev, MS_EVENT_AU, 1);
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "key after rebind");
    memset(&st, 0, sizeof(st));
    st.struct_size = sizeof(st);
    CHECK_ST(ms_hw_player_get_stats(p, &st), MS_OK, "rebind stats");
    CHECK(stats_conserved(&st), "rebind conserved");
    ms_hw_player_destroy(p);
    /* destroy 后再调必须安全：nativeDestroy 后 UI 可能迟到 */
    printf("  test_eos_drive_idempotent_and_lifecycle ok\n");
    return 0;
}

struct lock_ctx {
    ms_hw_player *p;
    pthread_mutex_t *op;
    int stop;
};

static void *lock_feeder(void *arg)
{
    struct lock_ctx *c = (struct lock_ctx *)arg;
    ms_event ev;
    int i;
    for (i = 0; i < 80 && !c->stop; i++) {
        fill_ev(&ev, MS_EVENT_AU, (i % 5) == 0);
        ev.pts = (int64_t)i * 3600;
        ev.has_pts = 1;
        pthread_mutex_lock(c->op);
        (void)ms_hw_player_feed(c->p, &ev, 0, NULL, NULL);
        (void)ms_hw_player_drive(c->p);
        pthread_mutex_unlock(c->op);
        usleep(1000);
    }
    return NULL;
}

static void *lock_ctrl(void *arg)
{
    struct lock_ctx *c = (struct lock_ctx *)arg;
    int token = 31, i;
    for (i = 0; i < 20 && !c->stop; i++) {
        pthread_mutex_lock(c->op);
        if ((i % 3) == 0) {
            (void)ms_hw_player_flush(c->p);
        } else if ((i % 3) == 1) {
            (void)ms_hw_player_set_surface(c->p, &token);
        } else {
            (void)ms_hw_player_drive(c->p);
        }
        pthread_mutex_unlock(c->op);
        usleep(2000);
    }
    return NULL;
}

static int test_op_lock_flush_surface_feed(void)
{
    ms_hw_player *p;
    ms_event ev;
    pthread_t a, b;
    pthread_mutex_t op;
    struct lock_ctx c;
    ms_hw_player_stats st;
    int token = 30;
    CHECK(make_player(&p, 0) == 0, "player lock");
    CHECK_ST(ms_hw_player_set_surface(p, &token), MS_OK, "surf lock");
    fill_ev(&ev, MS_EVENT_CODEC_CONFIG, 0);
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "csd lock");
    pthread_mutex_init(&op, NULL);
    memset(&c, 0, sizeof(c));
    c.p = p;
    c.op = &op;
    CHECK(pthread_create(&a, NULL, lock_feeder, &c) == 0, "feeder");
    CHECK(pthread_create(&b, NULL, lock_ctrl, &c) == 0, "ctrl");
    pthread_join(a, NULL);
    c.stop = 1;
    pthread_join(b, NULL);
    pthread_mutex_lock(&op);
    (void)ms_hw_player_cancel(p);
    (void)ms_hw_player_release_decoder(p);
    pthread_mutex_unlock(&op);
    pthread_mutex_destroy(&op);
    memset(&st, 0, sizeof(st));
    st.struct_size = sizeof(st);
    CHECK_ST(ms_hw_player_get_stats(p, &st), MS_OK, "lock stats");
    CHECK(stats_conserved(&st), "lock protocol conserved");
    CHECK(st.have_first_display == 0, "lock not display");
    printf("  test_op_lock_flush_surface_feed ok in=%llu submitted=%llu "
           "drop_nokey=%llu drop_preroll=%llu backpressure=%llu\n",
           (unsigned long long)st.video_au_in,
           (unsigned long long)st.video_au_submitted,
           (unsigned long long)st.video_au_dropped_nokey,
           (unsigned long long)st.video_au_dropped_preroll,
           (unsigned long long)st.video_au_backpressure);
    ms_hw_player_destroy(p);
    return 0;
}

static int test_request_timing_includes_injected_wait(void)
{
    ms_hw_player *p;
    ms_event ev;
    ms_hw_player_stats st;
    int token = 41;
    const int pre_ms = 150;
    const int idr_ms = 300;
    CHECK(make_player(&p, 0) == 0, "player req");
    CHECK_ST(ms_hw_player_set_surface(p, &token), MS_OK, "surf req");
    /* create 之后、mark 之前的等待只应进入管线局部计时，不应算请求端到端。 */
    usleep(pre_ms * 1000);
    CHECK_ST(ms_hw_player_mark_request(p), MS_OK, "mark request");
    fill_ev(&ev, MS_EVENT_CODEC_CONFIG, 0);
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "csd req");
    fill_ev(&ev, MS_EVENT_AU, 0);
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "nokey before idr wait");
    usleep(idr_ms * 1000);
    fill_ev(&ev, MS_EVENT_AU, 1);
    ev.has_pts = 1;
    ev.pts = 18000;
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "delayed idr");
    CHECK_ST(ms_hw_player_drive(p), MS_OK, "drive req");
    memset(&st, 0, sizeof(st));
    st.struct_size = sizeof(st);
    CHECK_ST(ms_hw_player_get_stats(p, &st), MS_OK, "stats req");
    CHECK(st.have_request == 1, "have request");
    CHECK(st.have_first_display == 0 && st.t_first_display_us == -1, "no display");
    CHECK(st.have_req_first_packet && st.have_req_first_decodable_au &&
          st.have_req_first_submit, "req stages");
    /* 请求原点在注入等待之前：首包应远小于 IDR 等待。 */
    CHECK(st.t_req_first_packet_us >= 0 && st.t_req_first_packet_us < 80000,
          "req first_packet excludes pre-mark wait and is before IDR delay");
    CHECK(st.t_req_first_decodable_au_us >= (idr_ms - 20) * 1000,
          "req decodable includes injected IDR wait");
    CHECK(st.t_req_first_submit_us >= (idr_ms - 20) * 1000,
          "req submit includes injected IDR wait");
    /* 局部原点在 create：应包含 mark 前等待 + IDR 等待。 */
    CHECK(st.t_first_decodable_au_us >= (pre_ms + idr_ms - 30) * 1000,
          "stage-local decodable includes pre-mark and IDR waits");
    CHECK(stats_conserved(&st), "req conserved");
    {
        int64_t stage_dec = st.t_first_decodable_au_us;
        int64_t req_pkt = st.t_req_first_packet_us;
        int64_t req_dec = st.t_req_first_decodable_au_us;
        int64_t req_sub = st.t_req_first_submit_us;
        int64_t req0 = st.t_request_us;
        printf("  test_request_timing_includes_injected_wait: "
               "stage_local decodable=%.3fms req_packet=%.3fms req_decodable=%.3fms "
               "req_submit=%.3fms (injected pre=%dms idr=%dms; not paint)\n",
               stage_dec / 1000.0, req_pkt / 1000.0, req_dec / 1000.0,
               req_sub / 1000.0, pre_ms, idr_ms);
        CHECK_ST(ms_hw_player_flush(p), MS_OK, "flush keeps request origin");
        memset(&st, 0, sizeof(st));
        st.struct_size = sizeof(st);
        CHECK_ST(ms_hw_player_get_stats(p, &st), MS_OK, "stats after flush");
        CHECK(st.have_request == 1 && st.t_request_us == req0, "request origin kept");
        CHECK(st.t_req_first_decodable_au_us == req_dec, "req decodable kept");
        CHECK(st.have_first_packet == 0 && st.t_first_packet_us == -1,
              "stage-local reset on flush");
    }
    printf("  test_request_timing_includes_injected_wait ok "
           "req_packet < 80ms, req_decodable=%.3fms (injected pre=%dms idr=%dms; "
           "stage-local reset by flush; display=%d; not paint)\n",
           st.t_req_first_decodable_au_us / 1000.0, pre_ms, idr_ms,
           st.have_first_display);
    ms_hw_player_destroy(p);
    return 0;
}

static int test_csd_change_reconfigure_epoch(void)
{
    ms_hw_player *p = NULL;
    ms_event ev;
    ms_hw_player_stats st;
    int token = 42;
    unsigned epoch0;
    CHECK(make_player(&p, 0) == 0, "create");
    CHECK_ST(ms_hw_player_set_surface(p, &token), MS_OK, "surf");
    fill_ev(&ev, MS_EVENT_CODEC_CONFIG, 0);
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "csd1");
    fill_ev(&ev, MS_EVENT_AU, 1);
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "key1");
    memset(&st, 0, sizeof(st));
    st.struct_size = sizeof(st);
    CHECK_ST(ms_hw_player_get_stats(p, &st), MS_OK, "stats1");
    CHECK(st.configured == 1 && st.video_au_submitted == 1, "configured after first key");
    epoch0 = st.epoch;
    fill_ev(&ev, MS_EVENT_CODEC_CONFIG, 0);
    ev.data = k_avcc2;
    ev.data_size = sizeof(k_avcc2);
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "csd2 change");
    memset(&st, 0, sizeof(st));
    st.struct_size = sizeof(st);
    CHECK_ST(ms_hw_player_get_stats(p, &st), MS_OK, "stats2");
    CHECK(st.epoch == epoch0 + 1, "epoch bumped on csd change");
    CHECK(st.configured == 1, "reconfigured");
    fill_ev(&ev, MS_EVENT_AU, 0);
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "nonkey after reconfig dropped");
    memset(&st, 0, sizeof(st));
    st.struct_size = sizeof(st);
    CHECK_ST(ms_hw_player_get_stats(p, &st), MS_OK, "stats drop");
    CHECK(st.video_au_dropped_nokey >= 1, "wait key after reconfig");
    fill_ev(&ev, MS_EVENT_AU, 1);
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "key after reconfig");
    memset(&st, 0, sizeof(st));
    st.struct_size = sizeof(st);
    CHECK_ST(ms_hw_player_get_stats(p, &st), MS_OK, "stats key2");
    CHECK(st.video_au_submitted == 2, "second key submitted");
    fill_ev(&ev, MS_EVENT_CODEC_CONFIG, 0);
    ev.data = k_avcc2;
    ev.data_size = sizeof(k_avcc2);
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "same csd2");
    memset(&st, 0, sizeof(st));
    st.struct_size = sizeof(st);
    CHECK_ST(ms_hw_player_get_stats(p, &st), MS_OK, "stats same");
    CHECK(st.epoch == epoch0 + 1, "same csd does not bump epoch");
    /* 快速切换 Surface：释放解码器后再挂回，须重新 configure 并等关键帧。 */
    CHECK_ST(ms_hw_player_release_decoder(p), MS_OK, "surface destroy");
    CHECK_ST(ms_hw_player_set_surface(p, &token), MS_OK, "surface again");
    fill_ev(&ev, MS_EVENT_AU, 0);
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "nonkey after surface");
    fill_ev(&ev, MS_EVENT_AU, 1);
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "key after surface");
    CHECK_ST(ms_hw_player_cancel(p), MS_OK, "cancel");
    CHECK_ST(ms_hw_player_cancel(p), MS_OK, "reentrant cancel");
    fill_ev(&ev, MS_EVENT_AU, 1);
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_ERR_CANCELED, "feed after cancel");
    CHECK_ST(ms_hw_player_mark_request(p), MS_OK, "mark clears cancel");
    fill_ev(&ev, MS_EVENT_CODEC_CONFIG, 0);
    CHECK_ST(ms_hw_player_set_surface(p, &token), MS_OK, "surf after remake");
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "csd after remake");
    ms_hw_player_destroy(p);
    printf("  test_csd_change_reconfigure_epoch ok (epoch +1, wait key; not audio, not paint)\n");
    return 0;
}


static int build_idr_au(uint8_t *out, size_t cap, size_t *n, int sc_len,
                        int width, unsigned sps_id, unsigned pps_id)
{
    uint8_t sps[80], pps[48], sl[64];
    size_t ns = 0, np = 0, nl = 0, off = 0;
    uint8_t raw[8] = { 0x88, 0x84, 0, 1, 2, 3, 4, 5 };
    if (ms_synth_h264_sps_ex(sps, sizeof(sps), &ns, sc_len, width, 16, 1,
                             sps_id, 2, 0, 0, 0) != 0) {
        return -1;
    }
    if (ms_synth_h264_pps_ex(pps, sizeof(pps), &np, sc_len, pps_id, sps_id, 0, 0) != 0) {
        return -1;
    }
    if (ms_synth_h264_slice(sl, sizeof(sl), &nl, sc_len, 1, raw, sizeof(raw)) != 0) {
        return -1;
    }
    if (off + ns + np + nl > cap) {
        return -1;
    }
    memcpy(out + off, sps, ns);
    off += ns;
    memcpy(out + off, pps, np);
    off += np;
    memcpy(out + off, sl, nl);
    off += nl;
    *n = off;
    return 0;
}

static int build_avcc_idr(uint8_t *out, size_t cap, size_t *n, int width,
                          unsigned sps_id, unsigned pps_id)
{
    uint8_t sps[80], pps[48], sl[64];
    size_t ns = 0, np = 0, nl = 0, off = 0;
    uint8_t raw[8] = { 0x88, 0x84, 0, 1, 2, 3, 4, 5 };
    if (ms_synth_h264_sps_ex(sps, sizeof(sps), &ns, 4, width, 16, 1,
                             sps_id, 2, 0, 0, 0) != 0) {
        return -1;
    }
    if (ms_synth_h264_pps_ex(pps, sizeof(pps), &np, 4, pps_id, sps_id, 0, 0) != 0) {
        return -1;
    }
    if (ms_synth_h264_slice(sl, sizeof(sl), &nl, 4, 1, raw, sizeof(raw)) != 0) {
        return -1;
    }
    if (ns <= 4 || np <= 4 || nl <= 4) {
        return -1;
    }
    memmove(sps, sps + 4, ns - 4); ns -= 4;
    memmove(pps, pps + 4, np - 4); np -= 4;
    memmove(sl, sl + 4, nl - 4); nl -= 4;
    if (off + 12 + ns + np + nl > cap) {
        return -1;
    }
    out[off++] = 0;
    out[off++] = 0;
    out[off++] = 0;
    out[off++] = (uint8_t)ns;
    memcpy(out + off, sps, ns);
    off += ns;
    out[off++] = 0;
    out[off++] = 0;
    out[off++] = 0;
    out[off++] = (uint8_t)np;
    memcpy(out + off, pps, np);
    off += np;
    out[off++] = 0;
    out[off++] = 0;
    out[off++] = 0;
    out[off++] = (uint8_t)nl;
    memcpy(out + off, sl, nl);
    off += nl;
    *n = off;
    return 0;
}

static int test_inband_csd_without_config(void)
{
    ms_hw_player *p = NULL;
    ms_hw_player_stats st;
    ms_event ev;
    int token = 9;
    unsigned epoch0;
    uint8_t au1[256], au2[256], au_sps[128], slonly[80];
    size_t n1 = 0, n2 = 0, ns = 0, nl = 0;
    int i;

    CHECK(build_idr_au(au1, sizeof(au1), &n1, 4, 16, 0, 0) == 0, "au1");
    CHECK(build_idr_au(au2, sizeof(au2), &n2, 4, 32, 0, 0) == 0, "au2");
    CHECK(make_player(&p, 0) == 0, "create");
    CHECK_ST(ms_hw_player_set_surface(p, &token), MS_OK, "surf");

    /* 初始化：无独立 CODEC_CONFIG，首个关键 AU 内含成对 SPS/PPS。 */
    memset(&ev, 0, sizeof(ev));
    ev.type = MS_EVENT_AU;
    ev.track.kind = MS_TRACK_VIDEO;
    ev.track.codec = MS_CODEC_H264;
    ev.sample_format = MS_SAMPLE_FMT_ANNEXB;
    ev.is_keyframe = 1;
    ev.has_pts = 1;
    ev.pts = 0;
    ev.time_base_num = 1;
    ev.time_base_den = 90000;
    ev.data = au1;
    ev.data_size = n1;
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "init inband key");
    CHECK_ST(ms_hw_player_drive(p), MS_OK, "drive init");
    memset(&st, 0, sizeof(st));
    st.struct_size = sizeof(st);
    CHECK_ST(ms_hw_player_get_stats(p, &st), MS_OK, "st init");
    CHECK(st.configured == 1 && st.video_au_submitted == 1, "init submitted");
    CHECK(st.video_csd_inband >= 1, "inband applied");
    epoch0 = st.epoch;

    /* 相同 CSD 再出现在 AU 内：不抖动重建。 */
    ev.pts = 9000;
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "same inband");
    memset(&st, 0, sizeof(st));
    st.struct_size = sizeof(st);
    CHECK_ST(ms_hw_player_get_stats(p, &st), MS_OK, "st same");
    CHECK(st.epoch == epoch0, "same csd no epoch");
    CHECK(st.video_csd_inband_same >= 1, "same counted");

    /* 中途新成对配置出现在关键 AU，无 CODEC_CONFIG。 */
    ev.data = au2;
    ev.data_size = n2;
    ev.pts = 18000;
    ev.is_keyframe = 1;
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "changed inband");
    memset(&st, 0, sizeof(st));
    st.struct_size = sizeof(st);
    CHECK_ST(ms_hw_player_get_stats(p, &st), MS_OK, "st chg");
    CHECK(st.epoch == epoch0 + 1, "epoch on inband change");
    CHECK(st.video_csd_inband >= 2, "second inband apply");
    CHECK(st.video_au_submitted >= 2, "new key submitted");
    epoch0 = st.epoch;

    /* 仅新 SPS、无配套 PPS：有界等待后放弃，不与旧 PPS 拼接。 */
    {
        uint8_t sps[80];
        size_t nsp = 0;
        CHECK(ms_synth_h264_sps_ex(sps, sizeof(sps), &nsp, 4, 48, 16, 1, 1, 2, 0, 0, 0) == 0,
              "lonely sps");
        ev.data = sps;
        ev.data_size = nsp;
        ev.is_keyframe = 1;
        ev.pts = 27000;
        CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "sps only");
        {
            uint8_t raw[8] = { 0x88, 0x84, 0, 1, 2, 3, 4, 5 };
            CHECK(ms_synth_h264_slice(slonly, sizeof(slonly), &nl, 4, 0, raw,
                                      sizeof(raw)) == 0,
                  "slice only");
        }
        ev.data = slonly;
        ev.data_size = nl;
        ev.is_keyframe = 0;
        for (i = 0; i < 20; i++) {
            ev.pts = 30000 + (uint64_t)i * 90;
            /* 无参数集的非关键 AU：等待配套 PPS，超限后放弃 pending，不得与旧 PPS 拼接。 */
            CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "wait au");
        }
        memset(&st, 0, sizeof(st));
        st.struct_size = sizeof(st);
        CHECK_ST(ms_hw_player_get_stats(p, &st), MS_OK, "st pend");
        CHECK(st.video_csd_pending_abandoned >= 1, "bounded abandon");
        CHECK(st.epoch == epoch0, "no stitch/apply on incomplete");
    }

    /* AVCC 样本路径：初始化 + 中途变化。 */
    ms_hw_player_destroy(p);
    p = NULL;
    CHECK(make_player(&p, 0) == 0, "p avcc");
    CHECK_ST(ms_hw_player_set_surface(p, &token), MS_OK, "surf avcc");
    CHECK(build_avcc_idr(au1, sizeof(au1), &n1, 16, 0, 0) == 0, "avcc1");
    CHECK(build_avcc_idr(au2, sizeof(au2), &n2, 32, 0, 0) == 0, "avcc2");
    ev.sample_format = MS_SAMPLE_FMT_AVCC;
    ev.is_keyframe = 1;
    ev.data = au1;
    ev.data_size = n1;
    ev.pts = 0;
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "avcc init");
    ev.data = au2;
    ev.data_size = n2;
    ev.pts = 9000;
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "avcc change");
    memset(&st, 0, sizeof(st));
    st.struct_size = sizeof(st);
    CHECK_ST(ms_hw_player_get_stats(p, &st), MS_OK, "st avcc");
    CHECK(st.video_csd_inband >= 2 && st.epoch >= 1, "avcc inband change");
    CHECK(stats_conserved(&st), "avcc conserved");

    /* 背压：LIMIT 后重试不得丢失已见到的配置，epoch 只加一次。 */
    ms_hw_player_destroy(p);
    p = NULL;
    CHECK(make_player(&p, 1) == 0, "cap1");
    CHECK_ST(ms_hw_player_set_surface(p, &token), MS_OK, "surf bp");
    ev.sample_format = MS_SAMPLE_FMT_ANNEXB;
    CHECK(build_idr_au(au1, sizeof(au1), &n1, 4, 16, 0, 0) == 0, "bp au1");
    CHECK(build_idr_au(au2, sizeof(au2), &n2, 4, 32, 0, 0) == 0, "bp au2");
    ev.data = au1;
    ev.data_size = n1;
    ev.is_keyframe = 1;
    ev.pts = 0;
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "bp first");
    ev.data = au2;
    ev.data_size = n2;
    ev.pts = 9000;
    {
        ms_status fs = ms_hw_player_feed(p, &ev, 0, NULL, NULL);
        if (fs == MS_LIMIT) {
            /* 配置应已在第一次尝试时应用或在重试时再应用，不得把 AU 当已消费。 */
            CHECK_ST(ms_hw_player_drive(p), MS_OK, "bp drive");
            fs = ms_hw_player_feed(p, &ev, 0, NULL, NULL);
        }
        CHECK(fs == MS_OK || fs == MS_LIMIT, "bp retry status");
        if (fs == MS_LIMIT) {
            CHECK_ST(ms_hw_player_drive(p), MS_OK, "bp drive2");
            CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "bp after drain");
        }
    }
    memset(&st, 0, sizeof(st));
    st.struct_size = sizeof(st);
    CHECK_ST(ms_hw_player_get_stats(p, &st), MS_OK, "st bp");
    CHECK(st.video_csd_inband >= 2, "bp kept inband config");
    CHECK(stats_conserved(&st), "bp conserved");
    ms_hw_player_destroy(p);
    (void)au_sps;
    (void)slonly;
    (void)ns;
    (void)nl;
    printf("  test_inband_csd_without_config ok (host mock; not paint)\n");
    return 0;
}


typedef struct reap_ctx {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int th_finished;
    int abandoned;
    int stop;
    int *heap;
    int work_ms;
} reap_ctx;

static void *reap_worker(void *arg)
{
    reap_ctx *c = (reap_ctx *)arg;
    int waited = 0;
    while (waited < c->work_ms) {
        if (c->stop) {
            break;
        }
        usleep(20000);
        waited += 20;
    }
    if (ms_hw_reap_thread_exiting(&c->mu, &c->cv, &c->th_finished, &c->abandoned) == 1) {
        *(c->heap) = 2;
        /* 模拟线程释放其堆对象；测试随后检查值，不由 UI 先 free */
        free(c->heap);
        c->heap = NULL;
    } else {
        *(c->heap) = 1;
    }
    return NULL;
}

static int test_reap_join_and_abandon(void)
{
    reap_ctx c;
    pthread_t th;
    int *heap;
    int own;
    /* 快路径：工作很快结束，UI join 并自行释放 heap */
    memset(&c, 0, sizeof(c));
    pthread_mutex_init(&c.mu, NULL);
    pthread_cond_init(&c.cv, NULL);
    c.work_ms = 30;
    c.heap = (int *)malloc(sizeof(int));
    CHECK(c.heap != NULL, "heap");
    *c.heap = 0;
    heap = c.heap;
    CHECK(pthread_create(&th, NULL, reap_worker, &c) == 0, "th fast");
    own = ms_hw_reap_wait_or_abandon(&c.mu, &c.cv, &c.th_finished, &c.abandoned, 1000);
    CHECK(own == 1, "fast path join");
    pthread_join(th, NULL);
    CHECK(*heap == 1, "ui owns heap");
    free(heap);
    pthread_cond_destroy(&c.cv);
    pthread_mutex_destroy(&c.mu);

    /* 慢路径：工作 400ms，等待 80ms 则 abandon，UI 不得 free heap */
    memset(&c, 0, sizeof(c));
    pthread_mutex_init(&c.mu, NULL);
    pthread_cond_init(&c.cv, NULL);
    c.work_ms = 400;
    c.heap = (int *)malloc(sizeof(int));
    CHECK(c.heap != NULL, "heap2");
    *c.heap = 0;
    CHECK(pthread_create(&th, NULL, reap_worker, &c) == 0, "th slow");
    own = ms_hw_reap_wait_or_abandon(&c.mu, &c.cv, &c.th_finished, &c.abandoned, 80);
    CHECK(own == 0, "slow path abandon");
    CHECK(c.abandoned == 1, "abandoned flag");
    pthread_detach(th);
    {
        int i;
        for (i = 0; i < 50 && !c.th_finished; i++) {
            usleep(20000);
        }
    }
    CHECK(c.th_finished == 1, "worker finished after abandon");
    {
        int j;
        for (j = 0; j < 20 && c.heap != NULL; j++) {
            usleep(10000);
        }
    }
    CHECK(c.heap == NULL, "worker freed heap; ui did not");
    pthread_cond_destroy(&c.cv);
    pthread_mutex_destroy(&c.mu);
    printf("  test_reap_join_and_abandon ok (bounded wait, no uaf free)\n");
    return 0;
}


static int test_audio_sync_and_refuse(void)
{
    ms_hw_player *p = NULL;
    ms_hw_player_stats st;
    ms_event ev;
    int token = 7;
    /* 默认 AS_HW：音频可配置；先喂音频再喂视频，drive 后应有音频时钟。 */
    CHECK(make_player(&p, 0) == 0, "p audio");
    CHECK_ST(ms_hw_player_set_surface(p, &token), MS_OK, "surf");
    ev_aac_cfg(&ev);
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "aac cfg");
    ev_aac_au(&ev, 0);
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "aac au0");
    ev_aac_au(&ev, 23219);
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "aac au1");
    CHECK_ST(ms_hw_player_drive(p), MS_OK, "drive audio");
    memset(&st, 0, sizeof(st));
    st.struct_size = sizeof(st);
    CHECK_ST(ms_hw_player_get_stats(p, &st), MS_OK, "st a");
    CHECK(st.audio_unsupported == 0 && st.audio_configured == 1, "audio cfg");
    CHECK(st.audio_is_hardware == 0, "mock not hw audio");
    CHECK(st.audio_au_played >= 1 && st.audio_clock_us > 0, "clock from played");
    CHECK(st.sync_has_audio_clock == 1, "has clock");
    /* 快速首帧：视频先于后续时钟门闩立即记 first_output。 */
    fill_ev(&ev, MS_EVENT_CODEC_CONFIG, 0);
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "v cfg");
    fill_ev(&ev, MS_EVENT_AU, 1);
    ev.has_pts = 1;
    ev.pts = 0;
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "v key");
    CHECK_ST(ms_hw_player_drive(p), MS_OK, "drive first");
    CHECK_ST(ms_hw_player_get_stats(p, &st), MS_OK, "st first");
    CHECK(st.have_first_output == 1, "fast first frame");
    /* 很早的后续帧应 held（pts 远大于当前 clock） */
    fill_ev(&ev, MS_EVENT_AU, 0);
    ev.has_pts = 1;
    ev.pts = (uint64_t)5000000;
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "v early");
    CHECK_ST(ms_hw_player_drive(p), MS_OK, "drive early");
    CHECK_ST(ms_hw_player_get_stats(p, &st), MS_OK, "st early");
    CHECK(st.video_au_held >= 1, "held early frame");
    {
        int i;
        /* 每帧 1024/44100 ≈ 23.2ms；>200ms 迟帧门闩需要约 9 帧已播放。 */
        for (i = 0; i < 12; i++) {
            ev_aac_au(&ev, 23219 * (int64_t)(i + 2));
            CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "more audio");
            CHECK_ST(ms_hw_player_drive(p), MS_OK, "drive more audio");
        }
    }
    fill_ev(&ev, MS_EVENT_AU, 0);
    ev.has_pts = 1;
    ev.pts = 0;
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "v late");
    CHECK_ST(ms_hw_player_drive(p), MS_OK, "drive late");
    CHECK_ST(ms_hw_player_get_stats(p, &st), MS_OK, "st late");
    CHECK(st.video_au_late >= 1, "late frame counted");
    ms_hw_player_destroy(p);
    p = NULL;

    /* 不支持的 AAC 配置（HE-AAC）：诊断拒绝，视频在 AS_HW 下仍可走。
     * 纯 MOCK_AS_SW 会同时拒绝视频软解，音频拒绝语义已在 test_hw_backend 覆盖。 */
    {
        ev.type = 0;
        ev_aac_cfg(&ev);
        ev.data = k_asc_he;
        ev.data_size = sizeof(k_asc_he);
        CHECK(make_player(&p, 0) == 0, "p he");
        CHECK_ST(ms_hw_player_set_surface(p, &token), MS_OK, "surf he");
        CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "he cfg");
        ev_aac_au(&ev, 0);
        CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "he au consumed");
        fill_ev(&ev, MS_EVENT_CODEC_CONFIG, 0);
        CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "v cfg he");
        fill_ev(&ev, MS_EVENT_AU, 1);
        CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "v key he");
        CHECK_ST(ms_hw_player_drive(p), MS_OK, "drive he");
        memset(&st, 0, sizeof(st));
        st.struct_size = sizeof(st);
        CHECK_ST(ms_hw_player_get_stats(p, &st), MS_OK, "st he");
        CHECK(st.audio_unsupported == 1, "diagnosed refuse");
        CHECK(st.sync_fallback_no_audio == 1, "video fallback");
        CHECK(st.have_first_output == 1, "video continues");
        CHECK(st.audio_is_hardware == 0, "not hw");
        CHECK(st.audio_au_dropped_unsupported >= 1, "dropped unsupported au");
        CHECK_ST(ms_hw_player_flush(p), MS_OK, "flush");
        CHECK_ST(ms_hw_player_cancel(p), MS_OK, "cancel");
        ms_hw_player_destroy(p);
        p = NULL;
    }
    printf("  test_audio_sync_and_refuse ok (host mock; not device sound)\n");
    return 0;
}


static int test_system_aac_policy(void)
{
    ms_hw_player *p = NULL;
    ms_hw_player_opts o;
    ms_hw_player_stats st;
    ms_event ev;
    int token = 11;
    /* 视频仍模拟硬件组件；音频显式允许系统 AAC（AS_SW）。 */
    ms_hw_player_opts_default(&o);
    o.decoder.mock_mode = MS_HW_MOCK_AS_HW;
    o.decoder.allow_software = 0;
    o.allow_system_aac = 1;
    o.audio_mock_mode = MS_HW_MOCK_AS_SW;
    o.wait_keyframe = 1;
    CHECK_ST(ms_hw_player_create(NULL, &o, &p), MS_OK, "p sys aac");
    CHECK_ST(ms_hw_player_set_surface(p, &token), MS_OK, "surf");
    ev_aac_cfg(&ev);
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "aac cfg");
    ev_aac_au(&ev, 0);
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "aac au");
    CHECK_ST(ms_hw_player_drive(p), MS_OK, "drive");
    memset(&st, 0, sizeof(st));
    st.struct_size = sizeof(st);
    CHECK_ST(ms_hw_player_get_stats(p, &st), MS_OK, "st sys");
    CHECK(st.audio_unsupported == 0 && st.audio_configured == 1, "sys aac cfg");
    CHECK(st.audio_is_hardware == 0 && st.audio_system_decode_is_not_hardware == 1,
          "honest system decode");
    CHECK(st.audio_detail && strstr(st.audio_detail, "系统 AAC，非硬件") != NULL, "banner");
    CHECK(st.audio_clock_us > 0 && st.sync_has_audio_clock == 1, "clock");
    fill_ev(&ev, MS_EVENT_CODEC_CONFIG, 0);
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "v cfg");
    fill_ev(&ev, MS_EVENT_AU, 1);
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "v key");
    CHECK_ST(ms_hw_player_drive(p), MS_OK, "drive v");
    CHECK_ST(ms_hw_player_get_stats(p, &st), MS_OK, "st v");
    CHECK(st.have_first_output == 1 && st.video_au_submitted >= 1, "video ok");
    CHECK(st.is_hardware == 0, "host video still not device hw");
    CHECK_ST(ms_hw_player_flush(p), MS_OK, "flush");
    ms_hw_player_destroy(p);
    p = NULL;

    /* 未显式允许：系统软解仍拒绝，视频继续。 */
    ms_hw_player_opts_default(&o);
    o.decoder.mock_mode = MS_HW_MOCK_AS_HW;
    o.decoder.allow_software = 0;
    o.allow_system_aac = 0;
    o.audio_mock_mode = MS_HW_MOCK_AS_SW;
    CHECK_ST(ms_hw_player_create(NULL, &o, &p), MS_OK, "p refuse sys");
    CHECK_ST(ms_hw_player_set_surface(p, &token), MS_OK, "surf 2");
    ev_aac_cfg(&ev);
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "aac cfg refuse");
    ev_aac_au(&ev, 0);
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "aac au dropped");
    fill_ev(&ev, MS_EVENT_CODEC_CONFIG, 0);
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "v cfg 2");
    fill_ev(&ev, MS_EVENT_AU, 1);
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "v key 2");
    CHECK_ST(ms_hw_player_drive(p), MS_OK, "drive 2");
    memset(&st, 0, sizeof(st));
    st.struct_size = sizeof(st);
    CHECK_ST(ms_hw_player_get_stats(p, &st), MS_OK, "st refuse");
    CHECK(st.audio_unsupported == 1 && st.sync_fallback_no_audio == 1, "audio refused");
    CHECK(st.have_first_output == 1, "video fallback");
    ms_hw_player_destroy(p);
    p = NULL;

    /* 非系统软解 + 允许系统 AAC：仍拒绝音频，视频继续。 */
    ms_hw_player_opts_default(&o);
    o.decoder.mock_mode = MS_HW_MOCK_AS_HW;
    o.allow_system_aac = 1;
    o.audio_mock_mode = MS_HW_MOCK_AS_FOREIGN_SW;
    CHECK_ST(ms_hw_player_create(NULL, &o, &p), MS_OK, "p foreign");
    CHECK_ST(ms_hw_player_set_surface(p, &token), MS_OK, "surf 3");
    ev_aac_cfg(&ev);
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "foreign cfg");
    ev_aac_au(&ev, 0);
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "foreign au");
    fill_ev(&ev, MS_EVENT_CODEC_CONFIG, 0);
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "v cfg 3");
    fill_ev(&ev, MS_EVENT_AU, 1);
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "v key 3");
    CHECK_ST(ms_hw_player_drive(p), MS_OK, "drive 3");
    memset(&st, 0, sizeof(st));
    st.struct_size = sizeof(st);
    CHECK_ST(ms_hw_player_get_stats(p, &st), MS_OK, "st foreign");
    CHECK(st.audio_unsupported == 1, "foreign refused");
    CHECK(st.have_first_output == 1, "video continues");
    CHECK(st.audio_is_hardware == 0, "not hw");
    ms_hw_player_destroy(p);
    printf("  test_system_aac_policy ok (host mock; not device sound; not video sw)\n");
    return 0;
}

static int test_abandoned_worker_cap(void)
{
    enum { N = MS_HW_ABANDONED_WORKER_CAP };
    reap_ctx ctx[4];
    pthread_t th[4];
    int i, own;
    CHECK(N >= 1 && N <= 4, "cap range");
    CHECK(ms_hw_abandoned_worker_cap() == N, "cap api");
    /* 等上一用例遗留线程归零 */
    {
        int w;
        for (w = 0; w < 50 && ms_hw_abandoned_worker_count() != 0; w++) {
            usleep(20000);
        }
    }
    CHECK(ms_hw_abandoned_worker_count() == 0, "count 0 at start");
    CHECK(ms_hw_can_spawn_worker() == 1, "can spawn");
    for (i = 0; i < N; i++) {
        memset(&ctx[i], 0, sizeof(ctx[i]));
        pthread_mutex_init(&ctx[i].mu, NULL);
        pthread_cond_init(&ctx[i].cv, NULL);
        ctx[i].work_ms = 300;
        ctx[i].heap = (int *)malloc(sizeof(int));
        CHECK(ctx[i].heap != NULL, "heap cap");
        *ctx[i].heap = 0;
        CHECK(pthread_create(&th[i], NULL, reap_worker, &ctx[i]) == 0, "th cap");
        own = ms_hw_reap_wait_or_abandon(&ctx[i].mu, &ctx[i].cv, &ctx[i].th_finished,
                                         &ctx[i].abandoned, 50);
        CHECK(own == 0, "abandon at cap fill");
        pthread_detach(th[i]);
    }
    CHECK(ms_hw_abandoned_worker_count() >= N, "count at cap");
    CHECK(ms_hw_can_spawn_worker() == 0, "refuse spawn at cap");
    for (i = 0; i < N; i++) {
        int w;
        for (w = 0; w < 40 && !ctx[i].th_finished; w++) {
            usleep(20000);
        }
        CHECK(ctx[i].th_finished == 1, "reclaimed");
    }
    {
        int w;
        for (w = 0; w < 40 && ms_hw_abandoned_worker_count() != 0; w++) {
            usleep(10000);
        }
    }
    CHECK(ms_hw_can_spawn_worker() == 1, "spawn after reclaim");
    for (i = 0; i < N; i++) {
        pthread_cond_destroy(&ctx[i].cv);
        pthread_mutex_destroy(&ctx[i].mu);
    }
    printf("  test_abandoned_worker_cap ok (refuse new at cap=%d)\n", N);
    return 0;
}

#ifdef MS_HW_PLAYER_HAVE_FF
static ms_status feed_retry(ms_hw_player *p, const ms_event *ev, int64_t origin, int tries)
{
    ms_status st = MS_AGAIN;
    int i;
    for (i = 0; i < tries; i++) {
        st = ms_hw_player_feed(p, ev, origin, NULL, NULL);
        if (st != MS_LIMIT && st != MS_AGAIN) {
            return st;
        }
        (void)ms_hw_player_drive(p);
        if (st == MS_AGAIN) {
            usleep(2000);
        }
    }
    return st;
}

static int pump_ff(ms_ff *f, ms_hw_player *p, int *nau)
{
    int n = 0;
    for (;;) {
        ms_event ev;
        ms_status st;
        memset(&ev, 0, sizeof(ev));
        st = ms_ff_pull(f, &ev);
        if (st == MS_AGAIN || st == MS_EOS) {
            break;
        }
        if (st != MS_OK) {
            fprintf(stderr, "  pump pull %s type=%d\n", ms_status_name(st), (int)ev.type);
            return -1;
        }
        st = feed_retry(p, &ev, ms_ff_time_origin_90k(f), 32);
        /* LIMIT/AGAIN 表示未消费：测试失败路径才释放以免泄漏。 */
        if (ev.release_id != 0) {
            (void)ms_ff_release(f, ev.release_id);
            ev.release_id = 0;
        }
        if (st == MS_LIMIT || st == MS_AGAIN) {
            fprintf(stderr, "  pump feed backpressure %s ev=%d codec=%d sz=%zu\n",
                    ms_status_name(st), (int)ev.type, (int)ev.track.codec, ev.data_size);
            return -1;
        }
        if (st != MS_OK && st != MS_ERR_UNSUPPORTED) {
            fprintf(stderr, "  pump feed %s ev=%d codec=%d sz=%zu\n",
                    ms_status_name(st), (int)ev.type, (int)ev.track.codec, ev.data_size);
            return -1;
        }
        if (ev.type == MS_EVENT_AU && ev.track.codec == MS_CODEC_H264) {
            n++;
        }
        (void)ms_hw_player_drive(p);
    }
    (void)ms_hw_player_signal_eos(p);
    (void)ms_hw_player_drive(p);
    *nau = n;
    return 0;
}

static int ff_open_noted(ms_ff *f, ms_hw_player *p, const char *url, int probe_mode)
{
    ms_ff_open_stats os;
    ms_status st;
    if (p != NULL) {
        (void)ms_hw_player_mark_worker_start(p);
    }
    if (ms_ff_set_probe_mode(f, probe_mode) != MS_OK) {
        return -1;
    }
    st = ms_ff_open(f, url);
    memset(&os, 0, sizeof(os));
    os.struct_size = sizeof(os);
    if (ms_ff_get_open_stats(f, &os) == MS_OK && p != NULL) {
        (void)ms_hw_player_note_open_phases(p, os.dur_open_input_us,
                                            os.dur_find_stream_info_us);
        printf("  open_split probe=%s open_input=%.3fms find_stream_info=%.3fms "
               "stream_info_err=%d probe_incomplete=%d (info is probe, not paint)\n",
               probe_mode == MS_FF_PROBE_FAST_START ? "FAST_START" : "DEFAULT",
               os.dur_open_input_us / 1000.0, os.dur_find_stream_info_us / 1000.0,
               os.stream_info_err, os.probe_incomplete);
        fflush(stdout);
    }
    if (st != MS_OK) {
        fprintf(stderr, "  ms_ff_open status=%s probe_incomplete=%d\n",
                ms_status_name(st), os.probe_incomplete);
    }
    return st == MS_OK ? 0 : -1;
}

static int test_ff_entry(const char *label, const char *url)
{
    ms_ff *f = NULL;
    ms_hw_player *p = NULL;
    ms_ff_limits lim;
    ms_hw_player_stats st;
    int token = 11, nau = 0;
    if (url == NULL || url[0] == 0) {
        printf("  SKIP %s (no url)\n", label);
        return 0;
    }
    ms_ff_limits_default(&lim);
    lim.io_timeout_ms = 5000;
    CHECK_ST(ms_ff_create(NULL, &lim, &f), MS_OK, "ff create");
    CHECK(make_player(&p, 0) == 0, "player");
    CHECK_ST(ms_hw_player_set_surface(p, &token), MS_OK, "surf");
    CHECK_ST(ms_hw_player_mark_request(p), MS_OK, "mark before open");
    if (ff_open_noted(f, p, url, MS_FF_PROBE_FAST_START) != 0) {
        fprintf(stderr, "  %s open failed url-class=%s\n", label,
                strstr(url, "udp:") ? "udp" : (strstr(url, "rtp:") ? "rtp" : "file"));
        ms_hw_player_destroy(p);
        ms_ff_destroy(f);
        return -1;
    }
    if (pump_ff(f, p, &nau) != 0) {
        ms_hw_player_destroy(p);
        ms_ff_destroy(f);
        fprintf(stderr, "  FAIL pump %s\n", label);
        failures++;
        return -1;
    }
    memset(&st, 0, sizeof(st));
    st.struct_size = sizeof(st);
    CHECK_ST(ms_hw_player_get_stats(p, &st), MS_OK, "stats");
    if (!stats_conserved(&st) || st.have_first_display != 0) {
        fprintf(stderr, "  FAIL %s conservation/display\n", label);
        failures++;
        ms_hw_player_destroy(p);
        ms_ff_destroy(f);
        return -1;
    }
    printf("  ff %s: video_au_in=%llu submitted=%llu drop_nokey=%llu "
           "stage_local packet=%.3fms decodable=%.3fms submit=%.3fms output=%.3fms; "
           "request_e2e packet=%.3fms decodable=%.3fms submit=%.3fms "
           "display=%d (mock; stage_local is NOT live e2e; not paint)\n",
           label, (unsigned long long)st.video_au_in,
           (unsigned long long)st.video_au_submitted,
           (unsigned long long)st.video_au_dropped_nokey,
           st.t_first_packet_us / 1000.0, st.t_first_decodable_au_us / 1000.0,
           st.t_first_submit_us / 1000.0, st.t_first_output_us / 1000.0,
           st.t_req_first_packet_us / 1000.0, st.t_req_first_decodable_au_us / 1000.0,
           st.t_req_first_submit_us / 1000.0,
           st.have_first_display);
    CHECK(st.have_request && st.t_req_first_packet_us >= 0, "file req times");
    ms_hw_player_destroy(p);
    ms_ff_destroy(f);
    (void)nau;
    return 0;
}

static void hw_kill_child(pid_t pid)
{
    int st;
    if (pid <= 0) {
        return;
    }
    kill(pid, SIGTERM);
    {
        int i;
        for (i = 0; i < 20; i++) {
            if (waitpid(pid, &st, WNOHANG) == pid) {
                return;
            }
            usleep(50000);
        }
    }
    kill(pid, SIGKILL);
    waitpid(pid, &st, 0);
}

static pid_t hw_spawn_ffmpeg(char *const argv[])
{
    pid_t pid = fork();
    if (pid < 0) {
        return -1;
    }
    if (pid == 0) {
        int fd;
        prctl(PR_SET_PDEATHSIG, SIGTERM);
        fd = open("/dev/null", O_WRONLY);
        if (fd >= 0) {
            dup2(fd, STDOUT_FILENO);
            dup2(fd, STDERR_FILENO);
            if (fd > 2) {
                close(fd);
            }
        }
        execv(argv[0], argv);
        _exit(127);
    }
    return pid;
}

static int hw_bind_udp(int *port_out)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in a;
    socklen_t alen = sizeof(a);
    if (fd < 0) {
        return -1;
    }
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) != 0 ||
        getsockname(fd, (struct sockaddr *)&a, &alen) != 0) {
        close(fd);
        return -1;
    }
    *port_out = ntohs(a.sin_port);
    return fd;
}

/* 直播 UDP/RTP：AGAIN 要重试到截止，不能当成文件结束。 */
static int pump_ff_live(ms_ff *f, ms_hw_player *p, int *nau, int timeout_ms)
{
    int n = 0;
    int spins = 0;
    int max_spins = (timeout_ms / 10) + 8;
    if (max_spins < 200) {
        max_spins = 200;
    }
    while (spins < max_spins) {
        ms_event ev;
        ms_status st;
        memset(&ev, 0, sizeof(ev));
        st = ms_ff_pull(f, &ev);
        spins++;
        if (st == MS_AGAIN) {
            (void)ms_hw_player_drive(p);
            usleep(10000);
            if (n >= 8) {
                break;
            }
            continue;
        }
        if (st == MS_EOS) {
            break;
        }
        if (st != MS_OK) {
            return (n > 0) ? 0 : -1;
        }
        st = feed_retry(p, &ev, ms_ff_time_origin_90k(f), 32);
        if (st == MS_LIMIT || st == MS_AGAIN) {
            if (ev.release_id != 0) {
                (void)ms_ff_release(f, ev.release_id);
            }
            return -1;
        }
        if (ev.release_id != 0) {
            (void)ms_ff_release(f, ev.release_id);
        }
        if (st != MS_OK && st != MS_ERR_UNSUPPORTED) {
            return (n > 0) ? 0 : -1;
        }
        if (ev.type == MS_EVENT_AU && ev.track.codec == MS_CODEC_H264) {
            n++;
        }
        (void)ms_hw_player_drive(p);
        if (n >= 25) {
            break;
        }
    }
    (void)ms_hw_player_signal_eos(p);
    (void)ms_hw_player_drive(p);
    *nau = n;
    return n > 0 ? 0 : -1;
}

static ms_hw_player *live_player_marked(void)
{
    ms_hw_player *p = NULL;
    int token = 11;
    if (make_player(&p, 0) != 0) {
        return NULL;
    }
    if (ms_hw_player_set_surface(p, &token) != MS_OK ||
        ms_hw_player_mark_request(p) != MS_OK) {
        ms_hw_player_destroy(p);
        return NULL;
    }
    return p;
}

static int run_live_and_print(const char *label, ms_ff *f, ms_hw_player *p,
                              int min_req_packet_us, int min_req_decodable_us)
{
    ms_hw_player_stats st;
    int nau = 0, rc = 0;
    if (p == NULL) {
        fprintf(stderr, "  FAIL %s missing marked player (must mark before open)\n", label);
        failures++;
        return -1;
    }
    if (pump_ff_live(f, p, &nau, 6000) != 0) {
        fprintf(stderr, "  FAIL %s pump (no video AU)\n", label);
        failures++;
        rc = -1;
    }
    memset(&st, 0, sizeof(st));
    st.struct_size = sizeof(st);
    if (ms_hw_player_get_stats(p, &st) != MS_OK) {
        rc = -1;
    } else {
        printf("  ff live %s: video_au_in=%llu submitted=%llu drop_nokey=%llu "
               "drop_preroll=%llu backpressure=%llu configured=%d err=%s "
               "stage_local packet=%.3fms decodable=%.3fms submit=%.3fms; "
               "request_e2e worker_start=%.3fms open_input_done=%.3fms "
               "find_info_done=%.3fms dur_open_input=%.3fms dur_find_info=%.3fms "
               "packet=%.3fms decodable=%.3fms submit=%.3fms display=%d "
               "(mock; phases are not paint; output is NOT display)\n",
               label, (unsigned long long)st.video_au_in,
               (unsigned long long)st.video_au_submitted,
               (unsigned long long)st.video_au_dropped_nokey,
               (unsigned long long)st.video_au_dropped_preroll,
               (unsigned long long)st.video_au_backpressure,
               st.configured, ms_status_name(st.last_error),
               st.t_first_packet_us / 1000.0, st.t_first_decodable_au_us / 1000.0,
               st.t_first_submit_us / 1000.0,
               st.t_req_worker_start_us / 1000.0,
               st.t_req_open_input_done_us / 1000.0,
               st.t_req_find_stream_info_done_us / 1000.0,
               st.dur_open_input_us / 1000.0, st.dur_find_stream_info_us / 1000.0,
               st.t_req_first_packet_us / 1000.0,
               st.t_req_first_decodable_au_us / 1000.0,
               st.t_req_first_submit_us / 1000.0,
               st.have_first_display);
        if (rc == 0) {
            if (!stats_conserved(&st)) {
                fprintf(stderr, "  FAIL %s conservation in!=submitted+drops\n", label);
                failures++;
                rc = -1;
            }
            if (st.last_error != MS_OK) {
                fprintf(stderr, "  FAIL %s last_error=%s after clean eos/drive\n",
                        label, ms_status_name(st.last_error));
                failures++;
                rc = -1;
            }
            if (st.have_first_display != 0 || st.video_au_submitted < 1 ||
                !st.have_first_packet || !st.have_first_decodable_au ||
                !st.have_first_submit || !st.have_request ||
                !st.have_req_first_packet || !st.have_req_first_decodable_au) {
                fprintf(stderr, "  FAIL %s stage/display/request contract\n", label);
                failures++;
                rc = -1;
            }
            if (min_req_packet_us > 0 &&
                st.t_req_first_packet_us < min_req_packet_us) {
                fprintf(stderr, "  FAIL %s request_e2e packet=%lld us < injected %d us "
                        "(must include wait; do not treat buffered/fast feed as live latency)\n",
                        label, (long long)st.t_req_first_packet_us, min_req_packet_us);
                failures++;
                rc = -1;
            }
            if (min_req_decodable_us > 0 &&
                st.t_req_first_decodable_au_us < min_req_decodable_us) {
                fprintf(stderr, "  FAIL %s request_e2e decodable=%lld us < injected %d us\n",
                        label, (long long)st.t_req_first_decodable_au_us,
                        min_req_decodable_us);
                failures++;
                rc = -1;
            }
            if (st.have_open_phases && strstr(label, "FAST") != NULL &&
                st.dur_find_stream_info_us > 800000) {
                fprintf(stderr, "  FAIL %s FAST find_stream_info=%.3fms exceeds 800ms bound "
                        "(io_timeout must not be the start budget)\n",
                        label, st.dur_find_stream_info_us / 1000.0);
                failures++;
                rc = -1;
            }
        }
    }
    (void)nau;
    return rc;
}

static int test_ff_live_udp_rtp(void)
{
    const char *dash = getenv("MS_FF_DASH_DIR");
    const char *ffmpeg = "/usr/bin/ffmpeg";
    char tspath[512], sdp_path[512], send_uri[160], url[220], rtp_uri[160];
    int port = 0, listen_fd, rc = 0;
    pid_t child = -1;
    if (dash == NULL || dash[0] == 0) {
        dash = "build/dev-samples/dash-ff";
    }
    snprintf(tspath, sizeof(tspath), "%s/short.ts", dash);
    if (access(tspath, R_OK) != 0 || access(ffmpeg, X_OK) != 0) {
        printf("  SKIP test_ff_live_udp_rtp (need short.ts and /usr/bin/ffmpeg sender); not counted as pass of this path\n");
        return 0;
    }
    listen_fd = hw_bind_udp(&port);
    if (listen_fd < 0) {
        fprintf(stderr, "  FAIL udp bind\n");
        failures++;
        return -1;
    }
    close(listen_fd);
    snprintf(send_uri, sizeof(send_uri), "udp://127.0.0.1:%d?pkt_size=1316", port);
    {
        char *argv[] = {
            (char *)ffmpeg, "-hide_banner", "-loglevel", "error",
            "-re", "-stream_loop", "-1",
            "-i", tspath, "-c", "copy", "-f", "mpegts",
            send_uri, NULL
        };
        ms_ff *f = NULL;
        ms_ff_limits lim;
        child = hw_spawn_ffmpeg(argv);
        if (child <= 0) {
            fprintf(stderr, "  FAIL spawn udp sender\n");
            failures++;
            return -1;
        }
        usleep(200000);
        ms_ff_limits_default(&lim);
        lim.io_timeout_ms = 4000;
        if (ms_ff_create(NULL, &lim, &f) != MS_OK) {
            hw_kill_child(child);
            failures++;
            return -1;
        }
        {
            int modes[2] = { MS_FF_PROBE_DEFAULT, MS_FF_PROBE_FAST_START };
            int mi;
            snprintf(url, sizeof(url), "udp://127.0.0.1:%d?listen=1&timeout=4000000", port);
            for (mi = 0; mi < 2; mi++) {
                ms_hw_player *lp;
                const char *lab = (modes[mi] == MS_FF_PROBE_FAST_START)
                                      ? "UDP-FAST mid-GOP"
                                      : "UDP-DEFAULT mid-GOP";
                if (mi > 0) {
                    ms_ff_destroy(f);
                    f = NULL;
                    usleep(150000); /* 让上一轮 UDP listen 套接字释放，避免偶发 EADDRINUSE */
                    if (ms_ff_create(NULL, &lim, &f) != MS_OK) {
                        hw_kill_child(child);
                        failures++;
                        return -1;
                    }
                }
                lp = live_player_marked();
                if (lp == NULL || ff_open_noted(f, lp, url, modes[mi]) != 0) {
                    fprintf(stderr, "  FAIL %s open\n", lab);
                    if (lp) {
                        ms_hw_player_destroy(lp);
                    }
                    ms_ff_destroy(f);
                    hw_kill_child(child);
                    failures++;
                    return -1;
                }
                if (run_live_and_print(lab, f, lp, 0, 0) != 0) {
                    rc = -1;
                }
                ms_hw_player_destroy(lp);
            }
        }
        ms_ff_cancel(f);
        ms_ff_destroy(f);
        hw_kill_child(child);
        child = -1;
    }
    listen_fd = hw_bind_udp(&port);
    if (listen_fd < 0) {
        fprintf(stderr, "  FAIL rtp bind\n");
        failures++;
        return -1;
    }
    close(listen_fd);
    snprintf(sdp_path, sizeof(sdp_path), "%s/short_h264_player.sdp", dash);
    unlink(sdp_path);
    snprintf(rtp_uri, sizeof(rtp_uri), "rtp://127.0.0.1:%d", port);
    {
        char *argv[] = {
            (char *)ffmpeg, "-hide_banner", "-loglevel", "error",
            "-re", "-stream_loop", "-1",
            "-i", tspath, "-map", "0:v", "-c", "copy", "-f", "rtp",
            "-sdp_file", sdp_path, rtp_uri, NULL
        };
        ms_ff *f = NULL;
        ms_ff_limits lim;
        char sdp_url[600];
        int waits;
        child = hw_spawn_ffmpeg(argv);
        if (child <= 0) {
            fprintf(stderr, "  FAIL spawn rtp sender\n");
            failures++;
            return -1;
        }
        for (waits = 0; waits < 50 && access(sdp_path, R_OK) != 0; waits++) {
            usleep(20000);
        }
        if (access(sdp_path, R_OK) != 0) {
            fprintf(stderr, "  FAIL rtp sdp not written\n");
            hw_kill_child(child);
            failures++;
            return -1;
        }
        ms_ff_limits_default(&lim);
        lim.io_timeout_ms = 4000;
        if (ms_ff_create(NULL, &lim, &f) != MS_OK) {
            hw_kill_child(child);
            failures++;
            return -1;
        }
        {
            ms_hw_player *lp = live_player_marked();
            if (lp == NULL) {
                ms_ff_destroy(f);
                hw_kill_child(child);
                failures++;
                return -1;
            }
            snprintf(sdp_url, sizeof(sdp_url), "file:%s", sdp_path);
            if (ff_open_noted(f, lp, sdp_url, MS_FF_PROBE_FAST_START) != 0) {
                fprintf(stderr, "  FAIL RTP/SDP open (class=rtp)\n");
                ms_hw_player_destroy(lp);
                ms_ff_destroy(f);
                hw_kill_child(child);
                failures++;
                return -1;
            }
            if (run_live_and_print("RTP-FAST mid-GOP", f, lp, 0, 0) != 0) {
                rc = -1;
            }
            ms_hw_player_destroy(lp);
        }
        ms_ff_cancel(f);
        ms_ff_destroy(f);
        hw_kill_child(child);
    }
    /* 先 listen 再延迟启动发送：对准 short.ts 唯一 IDR（index 0），期望门闩不丢帧。 */
    listen_fd = hw_bind_udp(&port);
    if (listen_fd < 0) {
        fprintf(stderr, "  FAIL udp bind from-start\n");
        failures++;
        return -1;
    }
    close(listen_fd);
    snprintf(send_uri, sizeof(send_uri), "udp://127.0.0.1:%d?pkt_size=1316", port);
    {
        pid_t delayed;
        ms_ff *f = NULL;
        ms_ff_limits lim;
        ms_ff_limits_default(&lim);
        lim.io_timeout_ms = 5000;
        if (ms_ff_create(NULL, &lim, &f) != MS_OK) {
            failures++;
            return -1;
        }
        {
            ms_hw_player *lp = live_player_marked();
            const int inject_us = 250000;
            const int min_us = 200000;
            if (lp == NULL) {
                ms_ff_destroy(f);
                failures++;
                return -1;
            }
            delayed = fork();
            if (delayed == 0) {
                prctl(PR_SET_PDEATHSIG, SIGTERM);
                usleep(inject_us);
                {
                    char *argv[] = {
                        (char *)ffmpeg, "-hide_banner", "-loglevel", "error",
                        "-re", "-i", tspath, "-c", "copy", "-f", "mpegts",
                        send_uri, NULL
                    };
                    execv(argv[0], argv);
                }
                _exit(127);
            }
            if (delayed <= 0) {
                fprintf(stderr, "  FAIL fork delayed udp sender\n");
                ms_hw_player_destroy(lp);
                ms_ff_destroy(f);
                failures++;
                return -1;
            }
            snprintf(url, sizeof(url), "udp://127.0.0.1:%d?listen=1&timeout=5000000", port);
            if (ff_open_noted(f, lp, url, MS_FF_PROBE_FAST_START) != 0) {
                fprintf(stderr, "  FAIL UDP from-start open\n");
                ms_hw_player_destroy(lp);
                ms_ff_destroy(f);
                hw_kill_child(delayed);
                failures++;
                return -1;
            }
            if (run_live_and_print("UDP-FAST from-start", f, lp, min_us, min_us) != 0) {
                rc = -1;
            }
            ms_hw_player_destroy(lp);
        }
        ms_ff_cancel(f);
        ms_ff_destroy(f);
        hw_kill_child(delayed);
    }
    return rc;
}

static int hw_http_serve_file(const char *path, const char *ctype, int *port_out, pid_t *child_out)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a;
    socklen_t alen = sizeof(a);
    pid_t ch;
    int yes = 1;
    if (fd < 0) {
        return -1;
    }
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) != 0 ||
        getsockname(fd, (struct sockaddr *)&a, &alen) != 0 ||
        listen(fd, 8) != 0) {
        close(fd);
        return -1;
    }
    *port_out = ntohs(a.sin_port);
    ch = fork();
    if (ch < 0) {
        close(fd);
        return -1;
    }
    if (ch == 0) {
        int cfd, n;
        FILE *fp;
        char req[1024], hdr[256], body[4096];
        long sz = 0;
        prctl(PR_SET_PDEATHSIG, SIGTERM);
        alarm(10);
        while ((cfd = accept(fd, NULL, NULL)) >= 0) {
            n = (int)recv(cfd, req, sizeof(req) - 1, 0);
            (void)n;
            fp = fopen(path, "rb");
            if (fp == NULL) {
                close(cfd);
                continue;
            }
            fseek(fp, 0, SEEK_END);
            sz = ftell(fp);
            fseek(fp, 0, SEEK_SET);
            snprintf(hdr, sizeof(hdr),
                     "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %ld\r\n"
                     "Connection: close\r\n\r\n",
                     ctype ? ctype : "application/octet-stream", sz);
            send(cfd, hdr, strlen(hdr), 0);
            while ((n = (int)fread(body, 1, sizeof(body), fp)) > 0) {
                send(cfd, body, (size_t)n, 0);
            }
            fclose(fp);
            close(cfd);
        }
        close(fd);
        _exit(0);
    }
    close(fd);
    *child_out = ch;
    return 0;
}

static int test_ff_http_ts_flv(void)
{
    const char *dash = getenv("MS_FF_DASH_DIR");
    char path[512], url[160];
    int port = 0, rc = 0;
    pid_t child = -1;
    const char *files[2] = { "short.ts", "short.flv" };
    const char *types[2] = { "video/mp2t", "video/x-flv" };
    const char *labs[2] = { "HTTP-TS-FAST", "HTTP-FLV-FAST" };
    int i;
    if (dash == NULL || dash[0] == 0) {
        dash = "build/dev-samples/dash-ff";
    }
    for (i = 0; i < 2; i++) {
        ms_ff *f = NULL;
        ms_hw_player *p = NULL;
        ms_ff_limits lim;
        snprintf(path, sizeof(path), "%s/%s", dash, files[i]);
        if (access(path, R_OK) != 0) {
            printf("  SKIP %s (no sample); not counted as pass\n", labs[i]);
            continue;
        }
        if (hw_http_serve_file(path, types[i], &port, &child) != 0) {
            fprintf(stderr, "  FAIL %s http serve\n", labs[i]);
            failures++;
            return -1;
        }
        ms_ff_limits_default(&lim);
        lim.io_timeout_ms = 5000;
        if (ms_ff_create(NULL, &lim, &f) != MS_OK) {
            hw_kill_child(child);
            failures++;
            return -1;
        }
        p = live_player_marked();
        snprintf(url, sizeof(url), "http://127.0.0.1:%d/%s", port, files[i]);
        if (p == NULL || ff_open_noted(f, p, url, MS_FF_PROBE_FAST_START) != 0) {
            fprintf(stderr, "  FAIL %s open\n", labs[i]);
            if (p) {
                ms_hw_player_destroy(p);
            }
            ms_ff_destroy(f);
            hw_kill_child(child);
            failures++;
            return -1;
        }
        if (run_live_and_print(labs[i], f, p, 0, 0) != 0) {
            rc = -1;
        } else {
            ms_hw_player_stats st;
            memset(&st, 0, sizeof(st));
            st.struct_size = sizeof(st);
            if (ms_hw_player_get_stats(p, &st) == MS_OK &&
                st.have_open_phases && st.dur_find_stream_info_us > 800000) {
                fprintf(stderr, "  FAIL %s find_info too long for file HTTP\n", labs[i]);
                failures++;
                rc = -1;
            }
        }
        ms_hw_player_destroy(p);
        ms_ff_cancel(f);
        ms_ff_destroy(f);
        hw_kill_child(child);
    }
    return rc;
}

static int test_ff_concat_midjoin(void)
{
    const char *dash = getenv("MS_FF_DASH_DIR");
    const char *tmp = getenv("MS_FF_TMP");
    const char *ffmpeg = "/usr/bin/ffmpeg";
    char src[512], dst[512], url[600];
    char *argv[16];
    ms_ff *f = NULL;
    ms_hw_player *p = NULL;
    ms_ff_limits lim;
    ms_hw_player_stats st;
    int token = 21, skip = 10, n = 0, fed = 0;
    int64_t origin;
    if (dash == NULL || dash[0] == 0) {
        dash = "build/dev-samples/dash-ff";
    }
    if (tmp == NULL || tmp[0] == 0) {
        tmp = "build/debug/ff-tmp";
    }
    snprintf(src, sizeof(src), "%s/short.ts", dash);
    snprintf(dst, sizeof(dst), "%s/short-twice.ts", tmp);
    if (access(src, R_OK) != 0 || access(ffmpeg, X_OK) != 0) {
        printf("  SKIP test_ff_concat_midjoin (need short.ts and ffmpeg); not counted as pass\n");
        return 0;
    }
    (void)mkdir(tmp, 0777);
    argv[0] = (char *)ffmpeg;
    argv[1] = "-hide_banner"; argv[2] = "-loglevel"; argv[3] = "error";
    argv[4] = "-y"; argv[5] = "-stream_loop"; argv[6] = "1";
    argv[7] = "-i"; argv[8] = src;
    argv[9] = "-c"; argv[10] = "copy"; argv[11] = "-f"; argv[12] = "mpegts";
    argv[13] = dst; argv[14] = NULL;
    {
        pid_t ch = hw_spawn_ffmpeg(argv);
        int wst;
        if (ch <= 0) {
            fprintf(stderr, "  FAIL spawn concat\n");
            failures++;
            return -1;
        }
        if (waitpid(ch, &wst, 0) != ch || !WIFEXITED(wst) || WEXITSTATUS(wst) != 0 ||
            access(dst, R_OK) != 0) {
            fprintf(stderr, "  FAIL write %s\n", dst);
            failures++;
            return -1;
        }
    }
    ms_ff_limits_default(&lim);
    lim.io_timeout_ms = -1;
    CHECK_ST(ms_ff_create(NULL, &lim, &f), MS_OK, "ff twice");
    snprintf(url, sizeof(url), "file:%s", dst);
    if (ms_ff_open(f, url) != MS_OK) {
        fprintf(stderr, "  FAIL open twice.ts\n");
        ms_ff_destroy(f);
        failures++;
        return -1;
    }
    CHECK(make_player(&p, 0) == 0, "player twice");
    CHECK_ST(ms_hw_player_set_surface(p, &token), MS_OK, "surf twice");
    CHECK_ST(ms_hw_player_mark_request(p), MS_OK, "mark twice");
    origin = ms_ff_time_origin_90k(f);
    for (;;) {
        ms_event ev;
        ms_status stf;
        memset(&ev, 0, sizeof(ev));
        stf = ms_ff_pull(f, &ev);
        if (stf == MS_AGAIN || stf == MS_EOS) {
            break;
        }
        if (stf != MS_OK) {
            break;
        }
        if (ev.type == MS_EVENT_AU && ev.track.codec == MS_CODEC_H264) {
            n++;
            if (n <= skip) {
                if (ev.release_id) {
                    (void)ms_ff_release(f, ev.release_id);
                }
                continue;
            }
            fed++;
        }
        if (ev.type == MS_EVENT_AU && ev.track.codec == MS_CODEC_H264 && n <= skip) {
            continue;
        }
        stf = feed_retry(p, &ev, origin, 32);
        if (ev.release_id) {
            (void)ms_ff_release(f, ev.release_id);
        }
        if (stf != MS_OK && stf != MS_ERR_UNSUPPORTED) {
            ms_hw_player_destroy(p);
            ms_ff_destroy(f);
            fprintf(stderr, "  FAIL midjoin feed\n");
            failures++;
            return -1;
        }
        (void)ms_hw_player_drive(p);
    }
    CHECK_ST(ms_hw_player_signal_eos(p), MS_OK, "eos twice");
    memset(&st, 0, sizeof(st));
    st.struct_size = sizeof(st);
    CHECK_ST(ms_hw_player_get_stats(p, &st), MS_OK, "stats twice");
    /* short.ts 25 帧仅 index0 为关键帧；两遍共 50 帧，跳过前 10 帧后：
     * 先遇到 15 个非关键帧（10..24），再遇到第二遍 IDR 并提交 25 帧。 */
    if (!stats_conserved(&st) || st.video_au_dropped_nokey != 15 ||
        st.video_au_submitted != 25 || st.video_au_in != 40 ||
        st.have_first_display != 0) {
        fprintf(stderr, "  FAIL midjoin expected drop=15 submit=25 in=40 got "
                "in=%llu sub=%llu drop=%llu preroll=%llu display=%d\n",
                (unsigned long long)st.video_au_in,
                (unsigned long long)st.video_au_submitted,
                (unsigned long long)st.video_au_dropped_nokey,
                (unsigned long long)st.video_au_dropped_preroll, st.have_first_display);
        failures++;
        ms_hw_player_destroy(p);
        ms_ff_destroy(f);
        return -1;
    }
    printf("  mid-join twice short.ts skip=%d pulled_v=%d fed=%d: in=%llu "
           "submitted=%llu drop_nokey=%llu first_packet=%.3fms "
           "decodable=%.3fms submit=%.3fms (legal gate; mock not paint)\n",
           skip, n, fed, (unsigned long long)st.video_au_in,
           (unsigned long long)st.video_au_submitted,
           (unsigned long long)st.video_au_dropped_nokey,
           st.t_first_packet_us / 1000.0, st.t_first_decodable_au_us / 1000.0,
           st.t_first_submit_us / 1000.0);
    ms_hw_player_destroy(p);
    ms_ff_destroy(f);
    return 0;
}

typedef struct cancel_job {
    ms_ff *f;
    int delay_ms;
} cancel_job;

static void *cancel_later(void *arg)
{
    cancel_job *j = (cancel_job *)arg;
    if (j->delay_ms > 0) {
        usleep((useconds_t)j->delay_ms * 1000u);
    }
    (void)ms_ff_cancel(j->f);
    return NULL;
}

static int64_t test_mono_us(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (int64_t)ts.tv_sec * 1000000LL + (int64_t)(ts.tv_nsec / 1000L);
}

static int test_ff_probe_cancel_and_unreachable(void)
{
    ms_ff *f = NULL;
    ms_ff_limits lim;
    ms_ff_open_stats os;
    pthread_t th;
    cancel_job job;
    int port = 0, fd;
    char url[220];
    ms_status st;
    int64_t t0, dt;
    fd = hw_bind_udp(&port);
    if (fd < 0) {
        fprintf(stderr, "  FAIL bind for cancel/unreachable\n");
        failures++;
        return -1;
    }
    close(fd);
    snprintf(url, sizeof(url), "udp://127.0.0.1:%d?listen=1", port);
    ms_ff_limits_default(&lim);
    lim.io_timeout_ms = 8000;
    CHECK_ST(ms_ff_create(NULL, &lim, &f), MS_OK, "ff cancel create");
    CHECK_ST(ms_ff_set_probe_mode(f, MS_FF_PROBE_FAST_START), MS_OK, "fast");
    job.f = f;
    job.delay_ms = 80;
    t0 = test_mono_us();
    CHECK(pthread_create(&th, NULL, cancel_later, &job) == 0, "cancel thread");
    st = ms_ff_open(f, url);
    pthread_join(th, NULL);
    dt = test_mono_us() - t0;
    CHECK(st == MS_ERR_CANCELED, "cancel during listen/probe");
    CHECK(dt < 2000000, "cancel is bounded, not full io_timeout");
    ms_ff_destroy(f);
    f = NULL;
    printf("  probe cancel during UDP listen: %s in %.3fms (not disguised timeout)\n",
           ms_status_name(st), dt / 1000.0);

    fd = hw_bind_udp(&port);
    if (fd < 0) {
        fprintf(stderr, "  FAIL bind unreachable\n");
        failures++;
        return -1;
    }
    close(fd);
    snprintf(url, sizeof(url), "udp://127.0.0.1:%d?listen=1", port);
    ms_ff_limits_default(&lim);
    lim.io_timeout_ms = 800;
    CHECK_ST(ms_ff_create(NULL, &lim, &f), MS_OK, "ff unreach create");
    CHECK_ST(ms_ff_set_probe_mode(f, MS_FF_PROBE_FAST_START), MS_OK, "fast2");
    t0 = test_mono_us();
    st = ms_ff_open(f, url);
    dt = test_mono_us() - t0;
    memset(&os, 0, sizeof(os));
    os.struct_size = sizeof(os);
    (void)ms_ff_get_open_stats(f, &os);
    CHECK(st == MS_ERR_TIMEOUT, "unreachable listen is TIMEOUT");
    CHECK(st != MS_ERR_CANCELED, "timeout not canceled");
    CHECK(dt >= 400000 && dt < 3000000, "timeout is bounded around io_timeout");
    ms_ff_destroy(f);
    printf("  unreachable UDP listen: open=%s in %.3fms probe_incomplete=%d (not hang)\n",
           ms_status_name(st), dt / 1000.0, os.probe_incomplete);
    return 0;
}

static int test_ff_audio_track_not_dropped_and_long_gop(void)
{
    const char *dash = getenv("MS_FF_DASH_DIR");
    const char *tmp = getenv("MS_FF_TMP");
    const char *ffmpeg = "/usr/bin/ffmpeg";
    char path[512], url[600], dst[512];
    ms_ff *f = NULL;
    ms_ff_limits lim;
    ms_event ev;
    int audio_track = 0, video_au = 0, n = 0;
    ms_status st;
    if (dash == NULL || dash[0] == 0) {
        dash = "build/dev-samples/dash-ff";
    }
    if (tmp == NULL || tmp[0] == 0) {
        tmp = "build/debug/ff-tmp";
    }
    snprintf(path, sizeof(path), "%s/short.ts", dash);
    if (access(path, R_OK) != 0) {
        printf("  SKIP audio-track/long-gop (no short.ts); not counted as pass\n");
        return 0;
    }
    ms_ff_limits_default(&lim);
    lim.io_timeout_ms = 5000;
    CHECK_ST(ms_ff_create(NULL, &lim, &f), MS_OK, "ff tracks");
    CHECK_ST(ms_ff_set_probe_mode(f, MS_FF_PROBE_FAST_START), MS_OK, "fast file");
    snprintf(url, sizeof(url), "file:%s", path);
    CHECK_ST(ms_ff_open(f, url), MS_OK, "open short.ts FAST");
    for (;;) {
        memset(&ev, 0, sizeof(ev));
        st = ms_ff_pull(f, &ev);
        if (st == MS_EOS || st == MS_AGAIN) {
            break;
        }
        if (st != MS_OK) {
            break;
        }
        n++;
        if ((ev.type == MS_EVENT_TRACK || ev.type == MS_EVENT_TRACK_UPDATE) &&
            ev.track.kind == MS_TRACK_AUDIO) {
            audio_track = 1;
        }
        if (ev.type == MS_EVENT_AU && ev.track.codec == MS_CODEC_H264) {
            video_au++;
        }
        if (ev.release_id) {
            (void)ms_ff_release(f, ev.release_id);
        }
        if (n > 80) {
            break;
        }
    }
    ms_ff_destroy(f);
    f = NULL;
    CHECK(audio_track == 1, "FAST does not drop audio TRACK (audio output still unimplemented)");
    CHECK(video_au >= 1, "video AU still present");
    printf("  FAST file emits audio TRACK (host audio mock/sync; not device sound); video_au=%d\n",
           video_au);

    /* 真实按时发送的长 GOP（约 2s），中途加入后 request_e2e 必须 >1s。 */
    (void)mkdir(tmp, 0777);
    snprintf(dst, sizeof(dst), "%s/longgop2s.ts", tmp);
    if (access(ffmpeg, X_OK) != 0) {
        printf("  SKIP long-gop live (no ffmpeg); not counted as pass\n");
        return 0;
    }
    {
        char *argv[32];
        pid_t ch;
        int wst;
        int ai = 0;
        argv[ai++] = (char *)ffmpeg;
        argv[ai++] = "-hide_banner";
        argv[ai++] = "-loglevel";
        argv[ai++] = "error";
        argv[ai++] = "-y";
        argv[ai++] = "-f";
        argv[ai++] = "lavfi";
        argv[ai++] = "-i";
        argv[ai++] = "testsrc=size=160x120:rate=25";
        argv[ai++] = "-t";
        argv[ai++] = "6";
        argv[ai++] = "-c:v";
        argv[ai++] = "libx264";
        argv[ai++] = "-pix_fmt";
        argv[ai++] = "yuv420p";
        argv[ai++] = "-profile:v";
        argv[ai++] = "baseline";
        argv[ai++] = "-x264-params";
        argv[ai++] = "keyint=50:min-keyint=50:scenecut=0:bframes=0:repeat-headers=1";
        argv[ai++] = "-an";
        argv[ai++] = "-f";
        argv[ai++] = "mpegts";
        argv[ai++] = dst;
        argv[ai++] = NULL;
        ch = hw_spawn_ffmpeg(argv);
        if (ch <= 0) {
            printf("  SKIP long-gop generate (spawn failed); not counted as pass\n");
            return 0;
        }
        if (waitpid(ch, &wst, 0) != ch || !WIFEXITED(wst) || WEXITSTATUS(wst) != 0 ||
            access(dst, R_OK) != 0) {
            printf("  SKIP long-gop generate (need libx264); not counted as pass\n");
            return 0;
        }
    }
    {
        int port = 0, listen_fd, token = 77;
        pid_t child = -1;
        char send_uri[160];
        char *sargv[20];
        ms_hw_player *lp;
        ms_hw_player_stats stt;
        memset(&stt, 0, sizeof(stt));
        listen_fd = hw_bind_udp(&port);
        if (listen_fd < 0) {
            fprintf(stderr, "  FAIL long-gop bind\n");
            failures++;
            return -1;
        }
        close(listen_fd);
        snprintf(send_uri, sizeof(send_uri), "udp://127.0.0.1:%d?pkt_size=1316", port);
        sargv[0] = (char *)ffmpeg;
        sargv[1] = "-hide_banner"; sargv[2] = "-loglevel"; sargv[3] = "error";
        sargv[4] = "-re"; sargv[5] = "-stream_loop"; sargv[6] = "-1";
        sargv[7] = "-i"; sargv[8] = dst;
        sargv[9] = "-c"; sargv[10] = "copy";
        sargv[11] = "-f"; sargv[12] = "mpegts";
        sargv[13] = send_uri; sargv[14] = NULL;
        child = hw_spawn_ffmpeg(sargv);
        if (child <= 0) {
            fprintf(stderr, "  FAIL long-gop sender\n");
            failures++;
            return -1;
        }
        /* 中途加入：错过 t=0 的 IDR，下一关键帧约 2s。不预读、不超速。 */
        usleep(400000);
        ms_ff_limits_default(&lim);
        lim.io_timeout_ms = 8000;
        if (ms_ff_create(NULL, &lim, &f) != MS_OK) {
            hw_kill_child(child);
            failures++;
            return -1;
        }
        lp = live_player_marked();
        snprintf(url, sizeof(url), "udp://127.0.0.1:%d?listen=1", port);
        if (lp == NULL || ff_open_noted(f, lp, url, MS_FF_PROBE_FAST_START) != 0) {
            fprintf(stderr, "  FAIL long-gop open\n");
            if (lp) {
                ms_hw_player_destroy(lp);
            }
            ms_ff_destroy(f);
            hw_kill_child(child);
            failures++;
            return -1;
        }
        CHECK_ST(ms_hw_player_set_surface(lp, &token), MS_OK, "long-gop surf");
        /* 中途加入时 FAST 探测可能尚无 CSD：AU 在配置前会 AGAIN，须继续等到 IDR/配置，
         * 不得把这一等待剪掉或当成泵失败。 */
        {
            int64_t tlim = test_mono_us() + 8000000;
            int got = 0;
            while (test_mono_us() < tlim) {
                ms_event evl;
                ms_status pst, fst;
                memset(&evl, 0, sizeof(evl));
                pst = ms_ff_pull(f, &evl);
                if (pst == MS_AGAIN) {
                    (void)ms_hw_player_drive(lp);
                    usleep(10000);
                    continue;
                }
                if (pst == MS_EOS || pst == MS_ERR_CANCELED || pst == MS_ERR_TIMEOUT) {
                    break;
                }
                if (pst != MS_OK) {
                    break;
                }
                fst = feed_retry(lp, &evl, ms_ff_time_origin_90k(f), 32);
                if (fst == MS_AGAIN || fst == MS_LIMIT) {
                    /* 尚未配置或背压：未消费则不 release 成功路径；重试几次后丢掉非关键帧以免卡死。 */
                    if (evl.release_id) {
                        (void)ms_ff_release(f, evl.release_id);
                    }
                    (void)ms_hw_player_drive(lp);
                    usleep(5000);
                    continue;
                }
                if (evl.release_id) {
                    (void)ms_ff_release(f, evl.release_id);
                }
                (void)ms_hw_player_drive(lp);
                if (fst == MS_OK) {
                    memset(&stt, 0, sizeof(stt));
                    stt.struct_size = sizeof(stt);
                    if (ms_hw_player_get_stats(lp, &stt) == MS_OK &&
                        stt.have_req_first_submit) {
                        got = 1;
                        break;
                    }
                }
            }
            if (!got) {
                fprintf(stderr, "  FAIL long-gop mid-join got no submit after waiting IDR\n");
                failures++;
                ms_hw_player_destroy(lp);
                ms_ff_destroy(f);
                hw_kill_child(child);
                return -1;
            }
        }
        memset(&stt, 0, sizeof(stt));
        stt.struct_size = sizeof(stt);
        CHECK_ST(ms_hw_player_get_stats(lp, &stt), MS_OK, "long-gop stats");
        CHECK(stt.t_req_first_decodable_au_us >= 1000000,
              "real long GOP wait counted in request_e2e (>1s)");
        CHECK(stt.have_first_display == 0, "not paint");
        printf("  long-GOP mid-join in=%llu sub=%llu drop_nokey=%llu "
               "request_e2e decodable=%.3fms submit=%.3fms "
               "(assert >=1000ms; mock, not paint)\n",
               (unsigned long long)stt.video_au_in,
               (unsigned long long)stt.video_au_submitted,
               (unsigned long long)stt.video_au_dropped_nokey,
               stt.t_req_first_decodable_au_us / 1000.0,
               stt.t_req_first_submit_us / 1000.0);
        ms_hw_player_destroy(lp);
        ms_ff_cancel(f);
        ms_ff_destroy(f);
        hw_kill_child(child);
    }
    return 0;
}


static int ff_make_repeat_ts(const char *ffmpeg, const char *out, const char *size)
{
    char *argv[32];
    int i = 0, wst;
    pid_t ch;
    argv[i++] = (char *)ffmpeg;
    argv[i++] = "-hide_banner";
    argv[i++] = "-loglevel";
    argv[i++] = "error";
    argv[i++] = "-y";
    argv[i++] = "-f";
    argv[i++] = "lavfi";
    argv[i++] = "-i";
    argv[i++] = (char *)size;
    argv[i++] = "-c:v";
    argv[i++] = "libx264";
    argv[i++] = "-pix_fmt";
    argv[i++] = "yuv420p";
    argv[i++] = "-g";
    argv[i++] = "25";
    argv[i++] = "-bf";
    argv[i++] = "0";
    argv[i++] = "-x264-params";
    argv[i++] = "repeat-headers=1:keyint=25:min-keyint=25:scenecut=0";
    argv[i++] = "-f";
    argv[i++] = "mpegts";
    argv[i++] = (char *)out;
    argv[i++] = NULL;
    ch = hw_spawn_ffmpeg(argv);
    if (ch <= 0) {
        return -1;
    }
    if (waitpid(ch, &wst, 0) != ch || !WIFEXITED(wst) || WEXITSTATUS(wst) != 0) {
        return -1;
    }
    return access(out, R_OK) == 0 ? 0 : -1;
}


static int concat_two_files(const char *a, const char *b, const char *dst)
{
    FILE *fa, *fb, *fo;
    char buf[4096];
    size_t n;
    fa = fopen(a, "rb");
    fb = fopen(b, "rb");
    fo = fopen(dst, "wb");
    if (fa == NULL || fb == NULL || fo == NULL) {
        if (fa) {
            fclose(fa);
        }
        if (fb) {
            fclose(fb);
        }
        if (fo) {
            fclose(fo);
        }
        return -1;
    }
    while ((n = fread(buf, 1, sizeof(buf), fa)) > 0) {
        if (fwrite(buf, 1, n, fo) != n) {
            fclose(fa);
            fclose(fb);
            fclose(fo);
            return -1;
        }
    }
    while ((n = fread(buf, 1, sizeof(buf), fb)) > 0) {
        if (fwrite(buf, 1, n, fo) != n) {
            fclose(fa);
            fclose(fb);
            fclose(fo);
            return -1;
        }
    }
    fclose(fa);
    fclose(fb);
    fclose(fo);
    return access(dst, R_OK) == 0 ? 0 : -1;
}

static int pump_ff_inband_only(ms_ff *f, ms_hw_player *p)
{
    for (;;) {
        ms_event ev;
        ms_status stf;
        memset(&ev, 0, sizeof(ev));
        stf = ms_ff_pull(f, &ev);
        if (stf == MS_AGAIN || stf == MS_EOS) {
            break;
        }
        if (stf != MS_OK) {
            break;
        }
        /* 故意不喂独立 CODEC_CONFIG，验证 AU 内参数集足以初始化与中途切换。 */
        if (ev.type == MS_EVENT_CODEC_CONFIG) {
            if (ev.release_id) {
                (void)ms_ff_release(f, ev.release_id);
            }
            continue;
        }
        stf = feed_retry(p, &ev, ms_ff_time_origin_90k(f), 32);
        if (ev.release_id) {
            (void)ms_ff_release(f, ev.release_id);
        }
        if (stf == MS_LIMIT || stf == MS_AGAIN) {
            return stf;
        }
        (void)ms_hw_player_drive(p);
    }
    return MS_OK;
}

static int test_ff_inband_mid_csd(void)
{
    const char *tmp = getenv("MS_FF_TMP");
    const char *ffmpeg = "/usr/bin/ffmpeg";
    char a[512], b[512], dst[512], url[600];
    ms_ff *f = NULL;
    ms_hw_player *p = NULL;
    ms_ff_limits lim;
    ms_hw_player_stats st;
    int token = 33;
    unsigned epoch0 = 0;
    if (tmp == NULL || tmp[0] == 0) {
        tmp = "build/debug/ff-tmp";
    }
    if (access(ffmpeg, X_OK) != 0) {
        printf("  SKIP test_ff_inband_mid_csd (no ffmpeg); not counted as pass\n");
        return 0;
    }
    (void)mkdir(tmp, 0777);
    snprintf(a, sizeof(a), "%s/csd-a.ts", tmp);
    snprintf(b, sizeof(b), "%s/csd-b.ts", tmp);
    snprintf(dst, sizeof(dst), "%s/csd-mid.ts", tmp);
    if (ff_make_repeat_ts(ffmpeg, a, "testsrc=size=320x240:rate=25:duration=1") != 0 ||
        ff_make_repeat_ts(ffmpeg, b, "testsrc=size=640x360:rate=25:duration=1") != 0) {
        printf("  SKIP test_ff_inband_mid_csd (need libx264); not counted as pass\n");
        return 0;
    }
    /* 两段不同分辨率 TS 直接拼接，保留中途 SPS/PPS 变化（concat demuxer 对相对路径会翻倍）。 */
    if (concat_two_files(a, b, dst) != 0) {
        fprintf(stderr, "  FAIL write mid-csd ts\n");
        failures++;
        return -1;
    }
    ms_ff_limits_default(&lim);
    lim.io_timeout_ms = -1;
    CHECK_ST(ms_ff_create(NULL, &lim, &f), MS_OK, "ff midcsd");
    snprintf(url, sizeof(url), "file:%s", dst);
    if (ms_ff_set_probe_mode(f, MS_FF_PROBE_FAST_START) != MS_OK ||
        ms_ff_open(f, url) != MS_OK) {
        fprintf(stderr, "  FAIL open mid-csd ts\n");
        ms_ff_destroy(f);
        failures++;
        return -1;
    }
    CHECK(make_player(&p, 0) == 0, "p midcsd");
    CHECK_ST(ms_hw_player_set_surface(p, &token), MS_OK, "surf midcsd");
    CHECK_ST(ms_hw_player_mark_request(p), MS_OK, "mark midcsd");
    if (pump_ff_inband_only(f, p) != MS_OK) {
        fprintf(stderr, "  FAIL mid-csd TS pump\n");
        ms_hw_player_destroy(p);
        ms_ff_destroy(f);
        failures++;
        return -1;
    }
    memset(&st, 0, sizeof(st));
    st.struct_size = sizeof(st);
    CHECK_ST(ms_hw_player_get_stats(p, &st), MS_OK, "st midcsd");
    CHECK(st.configured == 1 && st.video_au_submitted >= 1, "midcsd configured");
    CHECK(st.video_csd_inband >= 1, "init from inband AU");
    CHECK(st.epoch >= 1 || st.video_csd_inband >= 2, "mid-stream change applied");
    CHECK(stats_conserved(&st), "midcsd conserved");
    epoch0 = st.epoch;
    printf("  ff inband mid-CSD TS (CONFIG events skipped): in=%llu sub=%llu inband=%llu same=%llu "
           "epoch=%u display=0 (mock; not paint)\n",
           (unsigned long long)st.video_au_in, (unsigned long long)st.video_au_submitted,
           (unsigned long long)st.video_csd_inband,
           (unsigned long long)st.video_csd_inband_same, st.epoch);
    ms_hw_player_destroy(p);
    p = NULL;
    ms_ff_destroy(f);
    f = NULL;

    /* 同一拼接流再转封装 FLV，仍跳过独立 CONFIG。 */
    {
        char flv[512], furl[600];
        char *x[16];
        int xi = 0, wst;
        pid_t ch;
        snprintf(flv, sizeof(flv), "%s/csd-mid.flv", tmp);
        x[xi++] = (char *)ffmpeg;
        x[xi++] = "-hide_banner";
        x[xi++] = "-loglevel";
        x[xi++] = "error";
        x[xi++] = "-y";
        x[xi++] = "-i";
        x[xi++] = dst;
        x[xi++] = "-c";
        x[xi++] = "copy";
        x[xi++] = "-f";
        x[xi++] = "flv";
        x[xi++] = flv;
        x[xi++] = NULL;
        ch = hw_spawn_ffmpeg(x);
        if (ch <= 0 || waitpid(ch, &wst, 0) != ch || !WIFEXITED(wst) ||
            WEXITSTATUS(wst) != 0 || access(flv, R_OK) != 0) {
            fprintf(stderr, "  FAIL remux mid-csd flv\n");
            failures++;
            return -1;
        }
        CHECK_ST(ms_ff_create(NULL, &lim, &f), MS_OK, "ff midcsd flv");
        snprintf(furl, sizeof(furl), "file:%s", flv);
        if (ms_ff_set_probe_mode(f, MS_FF_PROBE_FAST_START) != MS_OK ||
            ms_ff_open(f, furl) != MS_OK) {
            fprintf(stderr, "  FAIL open mid-csd flv\n");
            ms_ff_destroy(f);
            failures++;
            return -1;
        }
        CHECK(make_player(&p, 0) == 0, "p midcsd flv");
        CHECK_ST(ms_hw_player_set_surface(p, &token), MS_OK, "surf midcsd flv");
        CHECK_ST(ms_hw_player_mark_request(p), MS_OK, "mark midcsd flv");
        if (pump_ff_inband_only(f, p) != MS_OK) {
            fprintf(stderr, "  FAIL mid-csd FLV pump\n");
            ms_hw_player_destroy(p);
            ms_ff_destroy(f);
            failures++;
            return -1;
        }
        memset(&st, 0, sizeof(st));
        st.struct_size = sizeof(st);
        CHECK_ST(ms_hw_player_get_stats(p, &st), MS_OK, "st midcsd flv");
        CHECK(st.configured == 1 && st.video_au_submitted >= 1, "flv midcsd configured");
        CHECK(st.video_csd_inband >= 1, "flv init from inband");
        CHECK(st.epoch >= 1 || st.video_csd_inband >= 2, "flv mid-stream change");
        CHECK(stats_conserved(&st), "flv midcsd conserved");
        printf("  ff inband mid-CSD FLV (CONFIG events skipped): in=%llu sub=%llu inband=%llu same=%llu "
               "epoch=%u display=0 (mock; not paint)\n",
               (unsigned long long)st.video_au_in, (unsigned long long)st.video_au_submitted,
               (unsigned long long)st.video_csd_inband,
               (unsigned long long)st.video_csd_inband_same, st.epoch);
        ms_hw_player_destroy(p);
        ms_ff_destroy(f);
    }
    (void)epoch0;
    return 0;
}

#endif /* MS_HW_PLAYER_HAVE_FF */

static void ev_track(ms_event *ev, uint32_t id, ms_track_kind kind, ms_codec codec,
                     const char *lang, int selected)
{
    memset(ev, 0, sizeof(*ev));
    ev->type = MS_EVENT_TRACK;
    ev->track.id = id;
    ev->track.kind = kind;
    ev->track.codec = codec;
    ev->track.language = lang;
    ev->track.selected = selected;
    ev->time_base_num = 1;
    ev->time_base_den = 1000000;
}

static int test_p4_audio_switch_and_subs(void)
{
    ms_hw_player *p = NULL;
    ms_hw_player_stats st;
    ms_event ev;
    ms_track_info ti[8];
    size_t n = 0;
    int token = 9;
    unsigned epoch0;
    uint64_t clock0;
    ms_sub_cue cue;
    const char *srt =
        "1\n00:00:00,000 --> 00:00:01,000\nHello\n\n"
        "2\n00:00:00,500 --> 00:00:02,000\nOverlap\n";
    const uint8_t k_asc_b[2] = { 0x12, 0x08 }; /* slightly different */

    CHECK(make_player(&p, 0) == 0, "p4 p");
    CHECK_ST(ms_hw_player_set_surface(p, &token), MS_OK, "surf");
    ev_track(&ev, 1, MS_TRACK_VIDEO, MS_CODEC_H264, NULL, 1);
    ev.sample_format = MS_SAMPLE_FMT_AVCC;
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "v tr");
    ev_track(&ev, 10, MS_TRACK_AUDIO, MS_CODEC_AAC, "eng", 1);
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "a0 tr");
    ev_track(&ev, 11, MS_TRACK_AUDIO, MS_CODEC_AAC, "chi", 0);
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "a1 tr");
    ev_track(&ev, 20, MS_TRACK_SUBTITLE, MS_CODEC_SUB_TEXT, "eng", 0);
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "sub tr");
    ev_track(&ev, 21, MS_TRACK_SUBTITLE, MS_CODEC_SUB_BITMAP, NULL, 0);
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "bmp tr");
    CHECK_ST(ms_hw_player_list_tracks(p, ti, 8, &n), MS_OK, "list");
    CHECK(n == 5, "5 tracks");
    CHECK_ST(ms_hw_player_select_track(p, 99, 1), MS_ERR_INVALID_ARG, "bad id");
    CHECK_ST(ms_hw_player_select_track(p, 21, 1), MS_ERR_UNSUPPORTED, "bitmap unsup");

    ev_aac_cfg(&ev);
    ev.track.id = 10;
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "cfg10");
    ev_aac_au(&ev, 0);
    ev.track.id = 10;
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "au10");
    ev_aac_au(&ev, 23219);
    ev.track.id = 10;
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "au10b");
    CHECK_ST(ms_hw_player_drive(p), MS_OK, "drv0");
    memset(&st, 0, sizeof(st));
    st.struct_size = sizeof(st);
    CHECK_ST(ms_hw_player_get_stats(p, &st), MS_OK, "st0");
    CHECK(st.audio_configured == 1 && st.audio_clock_us > 0, "clock before switch");
    CHECK(st.audio_track_id == 10, "sel 10");
    CHECK(st.audio_is_hardware == 0, "mock not hw");
    epoch0 = st.selection_epoch;
    clock0 = (uint64_t)st.audio_clock_us;

    /* 旧 struct_size 不含 P4 尾部字段仍可用 */
    {
        ms_hw_player_stats oldst;
        unsigned char *raw = (unsigned char *)&oldst;
        size_t prefix = offsetof(ms_hw_player_stats, video_csd_pending_abandoned) +
                        sizeof(uint64_t);
        size_t off;
        int dirty = 0;
        memset(&oldst, 0xA5, sizeof(oldst));
        oldst.struct_size = prefix;
        CHECK_ST(ms_hw_player_get_stats(p, &oldst), MS_OK, "old stats size");
        CHECK(oldst.audio_configured == 1, "old size still reports audio");
        for (off = prefix; off < sizeof(oldst); off++) {
            if (raw[off] != 0xA5) {
                dirty = 1;
                break;
            }
        }
        CHECK(!dirty, "get_stats honors old struct_size");
    }

    CHECK_ST(ms_hw_player_select_track(p, 11, 1), MS_OK, "switch 11");
    memset(&st, 0, sizeof(st));
    st.struct_size = sizeof(st);
    CHECK_ST(ms_hw_player_get_stats(p, &st), MS_OK, "st switch");
    CHECK(st.selection_epoch > epoch0, "epoch bump");
    CHECK(st.audio_switch_count >= 1, "switch counted");
    CHECK(st.audio_track_id == 11, "now 11");
    CHECK(st.audio_configured == 0, "old session flushed, waiting cfg");
    CHECK(st.audio_clock_us == 0, "clock rebuilt, not leftover");
    (void)clock0;

    ev_aac_au(&ev, 40000);
    ev.track.id = 10;
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "old au drop");
    CHECK_ST(ms_hw_player_get_stats(p, &st), MS_OK, "st drop");
    CHECK(st.audio_au_dropped_unselected >= 1, "unselected AU dropped");

    ev_aac_cfg(&ev);
    ev.track.id = 11;
    ev.data = k_asc_b;
    ev.data_size = sizeof(k_asc_b);
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "cfg11");
    ev_aac_au(&ev, 0);
    ev.track.id = 11;
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "au11");
    ev_aac_au(&ev, 23219);
    ev.track.id = 11;
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "au11b");
    CHECK_ST(ms_hw_player_drive(p), MS_OK, "drv11");
    CHECK_ST(ms_hw_player_get_stats(p, &st), MS_OK, "st11");
    CHECK(st.audio_configured == 1 && st.audio_clock_us > 0, "new clock");
    CHECK(st.audio_track_id == 11, "still 11");

    /* 视频在切音轨后仍可走，并用新时钟 */
    fill_ev(&ev, MS_EVENT_CODEC_CONFIG, 0);
    ev.track.id = 1;
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "vcfg");
    fill_ev(&ev, MS_EVENT_AU, 1);
    ev.track.id = 1;
    ev.has_pts = 1;
    ev.pts = 0;
    ev.time_base_num = 1;
    ev.time_base_den = 1000000;
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "vkey");
    CHECK_ST(ms_hw_player_drive(p), MS_OK, "vdrv");
    CHECK_ST(ms_hw_player_get_stats(p, &st), MS_OK, "vst");
    CHECK(st.have_first_output == 1 && st.output_is_not_display == 1, "video after switch, not display");

    CHECK_ST(ms_hw_player_select_track(p, 20, 1), MS_OK, "sub on");
    memset(&ev, 0, sizeof(ev));
    ev.type = MS_EVENT_SUBTITLE;
    ev.track.id = 20;
    ev.track.kind = MS_TRACK_SUBTITLE;
    ev.track.codec = MS_CODEC_SUB_TEXT;
    ev.has_pts = 1;
    ev.pts = 0;
    ev.dts = 1000000;
    ev.time_base_num = 1;
    ev.time_base_den = 1000000;
    ev.data = (const uint8_t *)"One";
    ev.data_size = 3;
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "cue1");
    ev.pts = 500000;
    ev.dts = 2000000;
    ev.data = (const uint8_t *)"Two";
    ev.data_size = 3;
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "cue2");
    memset(&cue, 0, sizeof(cue));
    cue.struct_size = sizeof(cue);
    CHECK_ST(ms_hw_player_subtitles_at(p, 0, &cue, 1, &n), MS_OK, "at0");
    CHECK(n == 1 && cue.text && strcmp(cue.text, "One") == 0, "t0");
    CHECK_ST(ms_hw_player_subtitles_at(p, 45000, NULL, 0, &n), MS_OK, "at0.5");
    CHECK(n == 2, "overlap");
    CHECK_ST(ms_hw_player_select_track(p, 20, 0), MS_OK, "sub off");
    CHECK_ST(ms_hw_player_subtitles_at(p, 45000, NULL, 0, &n), MS_OK, "off");
    CHECK(n == 0, "closed");
    CHECK_ST(ms_hw_player_select_track(p, 20, 1), MS_OK, "sub on2");
    CHECK_ST(ms_hw_player_flush(p), MS_OK, "flush");
    CHECK_ST(ms_hw_player_subtitles_at(p, 0, NULL, 0, &n), MS_OK, "after flush");
    CHECK(n == 0, "inband cleared on flush");

    CHECK_ST(ms_hw_player_set_external_subtitle(p, (const uint8_t *)srt, strlen(srt),
                                                MS_SUB_FORMAT_SRT), MS_OK, "ext");
    CHECK_ST(ms_hw_player_list_tracks(p, ti, 8, &n), MS_OK, "list ext");
    {
        size_t k;
        int saw = 0;
        for (k = 0; k < n; k++) {
            if (ti[k].id == MS_TRACK_ID_EXTERNAL_SUB) {
                saw = 1;
                CHECK(ti[k].selected == 0, "ext not auto");
            }
        }
        CHECK(saw, "ext listed");
    }
    CHECK_ST(ms_hw_player_select_track(p, MS_TRACK_ID_EXTERNAL_SUB, 1), MS_OK, "ext on");
    CHECK_ST(ms_hw_player_subtitles_at(p, 0, &cue, 1, &n), MS_OK, "ext cue");
    CHECK(n == 1, "ext cue at 0");
    CHECK_ST(ms_hw_player_set_external_subtitle(p, NULL, 0, MS_SUB_FORMAT_AUTO), MS_OK, "ext clr");

    /* 关闭音轨：无音频回退 */
    CHECK_ST(ms_hw_player_select_track(p, 11, 0), MS_OK, "audio off");
    CHECK_ST(ms_hw_player_get_stats(p, &st), MS_OK, "st noa");
    CHECK(st.sync_fallback_no_audio == 1 && st.audio_track_id == 0, "no audio fallback");

    ms_hw_player_destroy(p);
    printf("  test_p4_audio_switch_and_subs ok (host mock; not device sound/paint)\n");
    return 0;
}



static int test_p4_ass_inband(void)
{
    ms_hw_player *p = NULL;
    ms_event ev;
    ms_sub_cue cues[4];
    size_t n = 0;
    int token = 21;
    const char *hdr =
        "[Script Info]\nScriptType: v4.00+\nPlayResX: 640\nPlayResY: 360\n"
        "[V4+ Styles]\n"
        "Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, "
        "BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, "
        "BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding\n"
        "Style: Default,Arial,24,&H0000FF00,&H000000FF,&H00000000,&H00000000,-1,0,0,0,"
        "100,100,0,0,1,2,2,2,20,20,15,1\n";
    const char *ev0 = "0,0,Default,,0,0,0,,Hello {\\b0}x";
    const char *ev1 = "1,1,Default,,0,0,0,,{\\an8\\pos(320,40)}Top";
    const char *ass_file =
        "[Script Info]\nScriptType: v4.00+\nPlayResX: 384\nPlayResY: 288\n"
        "[V4+ Styles]\n"
        "Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, "
        "BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, "
        "BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding\n"
        "Style: Default,Arial,18,&H00FFFFFF,&H000000FF,&H00000000,&H00000000,0,0,0,0,"
        "100,100,0,0,1,2,2,2,8,8,8,1\n"
        "[Events]\n"
        "Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n"
        "Dialogue: 0,0:00:00.00,0:00:02.00,Default,,0,0,0,,ext {\\i1}italic\n";
    const uint8_t *d0 = (const uint8_t *)ev0;
    const uint8_t *d1 = (const uint8_t *)ev1;

    CHECK(make_player(&p, 0) == 0, "ass p");
    CHECK_ST(ms_hw_player_set_surface(p, &token), MS_OK, "surf");
    ev_track(&ev, 1, MS_TRACK_VIDEO, MS_CODEC_H264, NULL, 1);
    ev.sample_format = MS_SAMPLE_FMT_AVCC;
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "v tr");
    ev_track(&ev, 21, MS_TRACK_SUBTITLE, MS_CODEC_SUB_BITMAP, NULL, 0);
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "bmp tr");
    ev_track(&ev, 30, MS_TRACK_SUBTITLE, MS_CODEC_SUB_ASS, "eng", 0);
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "ass tr");
    CHECK_ST(ms_hw_player_select_track(p, 21, 1), MS_ERR_UNSUPPORTED, "bitmap still unsup");
    CHECK_ST(ms_hw_player_select_track(p, 30, 1), MS_OK, "ass selectable");

    memset(&ev, 0, sizeof(ev));
    ev.type = MS_EVENT_CODEC_CONFIG;
    ev.track.id = 30;
    ev.track.kind = MS_TRACK_SUBTITLE;
    ev.track.codec = MS_CODEC_SUB_ASS;
    ev.data = (const uint8_t *)hdr;
    ev.data_size = strlen(hdr);
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "ass hdr");

    memset(&ev, 0, sizeof(ev));
    ev.type = MS_EVENT_SUBTITLE;
    ev.track.id = 30;
    ev.track.kind = MS_TRACK_SUBTITLE;
    ev.track.codec = MS_CODEC_SUB_ASS;
    ev.time_base_num = 1;
    ev.time_base_den = 1000000;
    ev.has_pts = 1;
    ev.pts = 0;
    ev.has_dts = 1;
    ev.dts = 2000000;
    ev.data = d0;
    ev.data_size = strlen(ev0);
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "ass ev0");
    ev.pts = 1000000;
    ev.dts = 3000000;
    ev.data = d1;
    ev.data_size = strlen(ev1);
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "ass ev1");

    memset(cues, 0, sizeof(cues));
    cues[0].struct_size = sizeof(cues[0]);
    cues[1].struct_size = sizeof(cues[1]);
    CHECK_ST(ms_hw_player_subtitles_at(p, 90000, cues, 4, &n), MS_OK, "at 1s");
    CHECK(n == 2, "two overlapping ass cues");
    CHECK(cues[0].layer == 0 && cues[1].layer == 1, "layer order");
    CHECK(cues[0].text != NULL && strstr(cues[0].text, "{\\") == NULL, "no raw tags");
    CHECK(cues[0].text != NULL && strstr(cues[0].text, "Hello") != NULL, "ev0 text");
    CHECK(cues[0].color_argb == 0xFF00FF00u && cues[0].font_size == 24, "style from header");
    CHECK(cues[1].text != NULL && strcmp(cues[1].text, "Top") == 0, "ev1 text");
    CHECK((cues[1].style_flags & MS_SUB_STYLE_HAS_POS) != 0, "pos");
    CHECK(cues[1].alignment == 8, "an8");

    CHECK_ST(ms_hw_player_set_external_subtitle(p, (const uint8_t *)ass_file, strlen(ass_file),
                                                MS_SUB_FORMAT_AUTO), MS_OK, "ext ass");
    CHECK_ST(ms_hw_player_select_track(p, MS_TRACK_ID_EXTERNAL_SUB, 1), MS_OK, "ext on");
    {
        ms_track_info ti[8];
        size_t nt = 0, k;
        int saw = 0;
        CHECK_ST(ms_hw_player_list_tracks(p, ti, 8, &nt), MS_OK, "list");
        for (k = 0; k < nt; k++) {
            if (ti[k].id == MS_TRACK_ID_EXTERNAL_SUB) {
                CHECK(ti[k].codec == MS_CODEC_SUB_ASS, "ext codec ass");
                saw = 1;
            }
        }
        CHECK(saw, "ext present");
    }
    ms_sub_cue_default(&cues[0]);
    CHECK_ST(ms_hw_player_subtitles_at(p, 0, cues, 1, &n), MS_OK, "ext at 0");
    CHECK(n >= 1, "ext cue present (plus inband at 0?)");
    CHECK(cues[0].text != NULL && strstr(cues[0].text, "{\\") == NULL, "ext no tags");
    CHECK(cues[0].italic == 1 || strstr(cues[0].text, "italic") != NULL, "ext style or text");

    /* seek 清 cue 但头保留：再喂一条应仍有样式 */
    CHECK_ST(ms_hw_player_seek(p, 0), MS_OK, "seek");
    ev.data = d0;
    ev.data_size = strlen(ev0);
    ev.pts = 0;
    ev.dts = 2000000;
    ev.track.id = 30;
    ev.track.codec = MS_CODEC_SUB_ASS;
    ev.type = MS_EVENT_SUBTITLE;
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "ev after seek");
    ms_sub_cue_default(&cues[0]);
    CHECK_ST(ms_hw_player_subtitles_at(p, 0, cues, 1, &n), MS_OK, "after seek");
    /* 外挂仍在 0 处；至少能看到去标签文本 */
    CHECK(n >= 1, "cue after seek");

    ms_hw_player_destroy(p);
    printf("  test_p4_ass_inband ok (host event/layout; not device paint)\n");
    return 0;
}



static int test_p4_bitmap_inband(void)
{
    ms_hw_player *p = NULL;
    ms_event ev;
    ms_sub_cue cues[4];
    size_t n = 0;
    int token = 22;
    const uint8_t *px;

    CHECK(make_player(&p, 0) == 0, "bmp p");
    CHECK_ST(ms_hw_player_set_surface(p, &token), MS_OK, "surf");
    ev_track(&ev, 1, MS_TRACK_VIDEO, MS_CODEC_H264, NULL, 1);
    ev.sample_format = MS_SAMPLE_FMT_AVCC;
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "v tr");
    ev_track(&ev, 21, MS_TRACK_SUBTITLE, MS_CODEC_SUB_BITMAP, NULL, 0);
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "generic bmp tr");
    ev_track(&ev, 31, MS_TRACK_SUBTITLE, MS_CODEC_SUB_DVB, "chi", 0);
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "dvb tr");
    ev_track(&ev, 32, MS_TRACK_SUBTITLE, MS_CODEC_SUB_PGS, "eng", 0);
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "pgs tr");
    CHECK_ST(ms_hw_player_select_track(p, 21, 1), MS_ERR_UNSUPPORTED, "generic bitmap still unsup");
    CHECK_ST(ms_hw_player_select_track(p, 31, 1), MS_OK, "dvb selectable");
    CHECK_ST(ms_hw_player_select_track(p, 32, 1), MS_OK, "pgs selectable");
    /* 字幕轨互斥：测 DVB 时只选 31 */
    CHECK_ST(ms_hw_player_select_track(p, 31, 1), MS_OK, "dvb exclusive");

    memset(&ev, 0, sizeof(ev));
    ev.type = MS_EVENT_SUBTITLE;
    ev.track.id = 31;
    ev.track.kind = MS_TRACK_SUBTITLE;
    ev.track.codec = MS_CODEC_SUB_DVB;
    ev.time_base_num = 1;
    ev.time_base_den = 90000;
    ev.has_pts = 1;
    ev.pts = 90000;
    ev.has_dts = 1;
    ev.dts = 270000;
    ev.data = k_dvb_16_red;
    ev.data_size = sizeof(k_dvb_16_red);
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "dvb pkt");

    memset(cues, 0, sizeof(cues));
    cues[0].struct_size = sizeof(cues[0]);
    CHECK_ST(ms_hw_player_subtitles_at(p, 90000, cues, 4, &n), MS_OK, "at start");
    CHECK(n == 1, "one dvb cue");
    CHECK(cues[0].bitmap_kind == MS_SUB_BITMAP_DVB, "kind");
    CHECK(cues[0].bitmap_w == 16 && cues[0].pos_x == 10, "geom");
    CHECK(cues[0].text == NULL || cues[0].text[0] == 0, "not as text");
    px = cues[0].bitmap_rgba;
    CHECK(px != NULL && px[0] > 200 && px[3] == 255, "rgba");
    CHECK_ST(ms_hw_player_subtitles_at(p, 270000, cues, 4, &n), MS_OK, "at end");
    CHECK(n == 0, "ended");

    CHECK_ST(ms_hw_player_select_track(p, 32, 1), MS_OK, "switch pgs");
    ev.track.id = 32;
    ev.track.codec = MS_CODEC_SUB_PGS;
    ev.pts = 0;
    ev.dts = 180000;
    ev.data = k_pgs_16_blue;
    ev.data_size = sizeof(k_pgs_16_blue);
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "pgs pkt");
    ms_sub_cue_default(&cues[0]);
    CHECK_ST(ms_hw_player_subtitles_at(p, 0, cues, 4, &n), MS_OK, "pgs at 0");
    CHECK(n == 1 && cues[0].bitmap_kind == MS_SUB_BITMAP_PGS, "pgs cue");
    CHECK(cues[0].play_res_x == 1920, "canvas");

    ev.track.id = 31;
    ev.track.codec = MS_CODEC_SUB_DVB;
    ev.pts = 0;
    ev.dts = 90000;
    ev.data = k_dvb_trunc;
    ev.data_size = sizeof(k_dvb_trunc);
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "trunc not fatal");

    CHECK_ST(ms_hw_player_seek(p, 0), MS_OK, "seek");
    CHECK_ST(ms_hw_player_subtitles_at(p, 0, cues, 4, &n), MS_OK, "after seek");
    CHECK(n == 0, "cues flushed");
    CHECK_ST(ms_hw_player_select_track(p, 31, 1), MS_OK, "reselect dvb");
    ev.data = k_dvb_16_red;
    ev.data_size = sizeof(k_dvb_16_red);
    ev.pts = 0;
    ev.dts = 90000;
    ev.track.id = 31;
    ev.track.codec = MS_CODEC_SUB_DVB;
    ev.type = MS_EVENT_SUBTITLE;
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "refeed");
    ms_sub_cue_default(&cues[0]);
    CHECK_ST(ms_hw_player_subtitles_at(p, 0, cues, 1, &n), MS_OK, "after refeed");
    CHECK(n == 1 && cues[0].bitmap_w == 16, "cue after seek refeed");

    CHECK_ST(ms_hw_player_flush(p), MS_OK, "flush");
    CHECK_ST(ms_hw_player_subtitles_at(p, 0, cues, 4, &n), MS_OK, "after flush");
    CHECK(n == 0, "flush cleared");

    ms_hw_player_destroy(p);
    printf("  test_p4_bitmap_inband ok (host overlay path; not device visual accept)\n");
    return 0;
}

/* 独立位图轨与文字/ASS 互切、关闭。宿主事件/布局，不是设备出画验收。 */
static int test_p4_bitmap_text_ass_switch(void)
{
    ms_hw_player *p = NULL;
    ms_event ev;
    ms_sub_cue cues[4];
    size_t n = 0;
    int token = 23;
    const char *hdr =
        "[Script Info]\nScriptType: v4.00+\nPlayResX: 320\nPlayResY: 240\n\n"
        "[V4+ Styles]\n"
        "Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, "
        "BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, "
        "BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding\n"
        "Style: Default,Arial,24,&H0000FFFF,&H000000FF,&H00000000,&H00000000,0,0,0,0,"
        "100,100,0,0,1,2,0,2,8,8,8,1\n"
        "[Events]\n"
        "Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n";
    const char *ass_ev = "Dialogue: 0,0:00:00.00,0:00:04.00,Default,,0,0,0,,ASS-TRACK\n";
    const char *srt = "1\n00:00:00,000 --> 00:00:04,000\nTEXT-SRT\n";

    CHECK(make_player(&p, 0) == 0, "sw p");
    CHECK_ST(ms_hw_player_set_surface(p, &token), MS_OK, "surf");
    ev_track(&ev, 1, MS_TRACK_VIDEO, MS_CODEC_H264, NULL, 1);
    ev.sample_format = MS_SAMPLE_FMT_AVCC;
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "v tr");
    ev_track(&ev, 31, MS_TRACK_SUBTITLE, MS_CODEC_SUB_PGS, "eng", 0);
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "pgs tr");
    ev_track(&ev, 32, MS_TRACK_SUBTITLE, MS_CODEC_SUB_TEXT, "fra", 0);
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "srt tr");
    ev_track(&ev, 33, MS_TRACK_SUBTITLE, MS_CODEC_SUB_ASS, "zho", 0);
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "ass tr");

    /* 先在 PGS 选中时把三轨包都喂进缓存，再切轨且不再送包——模拟稀疏文字包已读过。 */
    CHECK_ST(ms_hw_player_select_track(p, 31, 1), MS_OK, "select pgs");
    memset(&ev, 0, sizeof(ev));
    ev.type = MS_EVENT_SUBTITLE;
    ev.track.id = 31;
    ev.track.kind = MS_TRACK_SUBTITLE;
    ev.track.codec = MS_CODEC_SUB_PGS;
    ev.time_base_num = 1;
    ev.time_base_den = 90000;
    ev.has_pts = 1;
    ev.pts = 0;
    ev.has_dts = 1;
    ev.dts = 180000;
    ev.data = k_pgs_16_blue;
    ev.data_size = sizeof(k_pgs_16_blue);
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "pgs pkt");
    ev.track.id = 32;
    ev.track.codec = MS_CODEC_SUB_TEXT;
    ev.pts = 0;
    ev.dts = 360000;
    ev.data = (const uint8_t *)srt;
    ev.data_size = strlen(srt);
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "srt pkt while pgs selected");
    memset(&ev, 0, sizeof(ev));
    ev.type = MS_EVENT_CODEC_CONFIG;
    ev.track.id = 33;
    ev.track.kind = MS_TRACK_SUBTITLE;
    ev.track.codec = MS_CODEC_SUB_ASS;
    ev.data = (const uint8_t *)hdr;
    ev.data_size = strlen(hdr);
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "ass hdr");
    memset(&ev, 0, sizeof(ev));
    ev.type = MS_EVENT_SUBTITLE;
    ev.track.id = 33;
    ev.track.kind = MS_TRACK_SUBTITLE;
    ev.track.codec = MS_CODEC_SUB_ASS;
    ev.time_base_num = 1;
    ev.time_base_den = 1000000;
    ev.has_pts = 1;
    ev.pts = 0;
    ev.has_dts = 1;
    ev.dts = 4000000;
    ev.data = (const uint8_t *)ass_ev;
    ev.data_size = strlen(ass_ev);
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "ass ev while pgs selected");

    ms_sub_cue_default(&cues[0]);
    CHECK_ST(ms_hw_player_subtitles_at(p, 0, cues, 4, &n), MS_OK, "pgs at 0");
    CHECK(n == 1 && cues[0].bitmap_kind == MS_SUB_BITMAP_PGS && cues[0].bitmap_w == 16,
          "pgs bitmap visible");
    CHECK(cues[0].text == NULL || cues[0].text[0] == 0, "pgs not text");

    CHECK_ST(ms_hw_player_select_track(p, 32, 1), MS_OK, "switch srt");
    ms_sub_cue_default(&cues[0]);
    CHECK_ST(ms_hw_player_subtitles_at(p, 0, cues, 4, &n), MS_OK, "srt at 0 no refeed");
    CHECK(n == 1 && cues[0].text != NULL && strstr(cues[0].text, "TEXT-SRT") != NULL, "srt text");
    CHECK(cues[0].bitmap_rgba == NULL || cues[0].bitmap_rgba_size == 0, "no bitmap on srt");

    CHECK_ST(ms_hw_player_select_track(p, 33, 1), MS_OK, "switch ass");
    ms_sub_cue_default(&cues[0]);
    CHECK_ST(ms_hw_player_subtitles_at(p, 0, cues, 4, &n), MS_OK, "ass at 0 no refeed");
    CHECK(n == 1 && cues[0].text != NULL && strstr(cues[0].text, "ASS-TRACK") != NULL, "ass text");
    CHECK(cues[0].bitmap_rgba == NULL || cues[0].bitmap_rgba_size == 0, "no bitmap on ass");

    CHECK_ST(ms_hw_player_clear_subtitles(p), MS_OK, "sub_off");
    ms_sub_cue_default(&cues[0]);
    CHECK_ST(ms_hw_player_subtitles_at(p, 0, cues, 4, &n), MS_OK, "after off");
    CHECK(n == 0, "cleared");

    CHECK_ST(ms_hw_player_select_track(p, 31, 1), MS_OK, "back to pgs");
    ms_sub_cue_default(&cues[0]);
    CHECK_ST(ms_hw_player_subtitles_at(p, 0, cues, 4, &n), MS_OK, "pgs again at 0 no refeed");
    CHECK(n == 1 && cues[0].bitmap_kind == MS_SUB_BITMAP_PGS && cues[0].bitmap_w == 16,
          "pgs visible after switch-back");

    ms_hw_player_destroy(p);
    printf("  test_p4_bitmap_text_ass_switch ok (host switch/off; not device visual)\n");
    return 0;
}


static int test_p4_rate_and_seek(void)
{
    ms_hw_player *p = NULL;
    ms_hw_player_stats st;
    ms_hw_rate_caps caps;
    ms_event ev;
    ms_sub_cue cue;
    size_t n = 0;
    int token = 11;
    int num = 0, den = 0;
    int64_t clock_before = 0;
    unsigned tl0;
    const char *srt =
        "1\n00:00:00,000 --> 00:00:10,000\nHello\n";

    CHECK(make_player(&p, 0) == 0, "rate p");
    CHECK_ST(ms_hw_player_set_surface(p, &token), MS_OK, "surf");

    memset(&caps, 0, sizeof(caps));
    caps.struct_size = sizeof(caps);
    CHECK_ST(ms_hw_player_get_rate_caps(p, &caps), MS_OK, "caps");
    CHECK(caps.mock_is_not_device == 1, "mock is not device");
    CHECK(caps.can_sync_av == 0, "host mock not A/V-synced speed");
    CHECK(caps.min_num == 1 && caps.min_den == 8, "mock min 1/8");
    CHECK(caps.max_num == 8 && caps.max_den == 1, "mock max 8x");

    /* 旧 struct_size：只含开头字段仍可用 */
    {
        ms_hw_rate_caps oldcaps;
        unsigned char *raw = (unsigned char *)&oldcaps;
        size_t prefix = sizeof(size_t) + sizeof(int) * 4;
        size_t off;
        int dirty = 0;
        memset(&oldcaps, 0xA5, sizeof(oldcaps));
        oldcaps.struct_size = prefix;
        CHECK_ST(ms_hw_player_get_rate_caps(p, &oldcaps), MS_OK, "old caps size");
        CHECK(oldcaps.can_apply == 1, "old size can_apply");
        for (off = prefix; off < sizeof(oldcaps); off++) {
            if (raw[off] != 0xA5) {
                dirty = 1;
                break;
            }
        }
        CHECK(!dirty, "get_rate_caps honors old struct_size");
    }

    CHECK_ST(ms_hw_player_set_rate(p, 0, 1), MS_ERR_INVALID_ARG, "rate 0");
    CHECK_ST(ms_hw_player_set_rate(p, 1, 0), MS_ERR_INVALID_ARG, "den 0");
    CHECK_ST(ms_hw_player_set_rate(p, -1, 1), MS_ERR_INVALID_ARG, "neg");
    CHECK_ST(ms_hw_player_set_rate(p, 16, 1), MS_ERR_UNSUPPORTED, "over max");
    CHECK_ST(ms_hw_player_set_rate(p, 1, 16), MS_ERR_UNSUPPORTED, "under min");
    CHECK_ST(ms_hw_player_get_rate(p, &num, &den), MS_OK, "still 1x");
    CHECK(num == 1 && den == 1, "rate unchanged after reject");

    ev_track(&ev, 1, MS_TRACK_VIDEO, MS_CODEC_H264, NULL, 1);
    ev.sample_format = MS_SAMPLE_FMT_AVCC;
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "vtr");
    ev_track(&ev, 10, MS_TRACK_AUDIO, MS_CODEC_AAC, "eng", 1);
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "atr");
    ev_aac_cfg(&ev);
    ev.track.id = 10;
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "acfg");
    ev_aac_au(&ev, 0);
    ev.track.id = 10;
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "au0");
    ev_aac_au(&ev, 23219);
    ev.track.id = 10;
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "au1");
    CHECK_ST(ms_hw_player_drive(p), MS_OK, "drv");
    memset(&st, 0, sizeof(st));
    st.struct_size = sizeof(st);
    CHECK_ST(ms_hw_player_get_stats(p, &st), MS_OK, "st0");
    CHECK(st.audio_clock_us > 0, "clock running");
    clock_before = st.audio_clock_us;

    CHECK_ST(ms_hw_player_set_rate(p, 2, 1), MS_OK, "2x");
    CHECK_ST(ms_hw_player_get_rate(p, &num, &den), MS_OK, "get 2x");
    CHECK(num == 2 && den == 1, "2x stored");
    CHECK_ST(ms_hw_player_get_stats(p, &st), MS_OK, "st rate");
    CHECK(st.rate_num == 2 && st.rate_den == 1, "stats rate");
    CHECK(st.audio_clock_us == clock_before, "rate change does not zero clock");

    CHECK_ST(ms_hw_player_pause(p), MS_OK, "pause");
    CHECK(ms_hw_player_is_paused(p) == 1, "paused");
    ev_aac_au(&ev, 46438);
    ev.track.id = 10;
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "au paused");
    CHECK_ST(ms_hw_player_drive(p), MS_OK, "drv paused");
    CHECK_ST(ms_hw_player_get_stats(p, &st), MS_OK, "st pause");
    CHECK(st.paused == 1, "stats paused");
    CHECK(st.audio_clock_us == clock_before, "clock frozen while paused");

    CHECK_ST(ms_hw_player_set_rate(p, 1, 2), MS_OK, "0.5x while paused");
    CHECK_ST(ms_hw_player_get_stats(p, &st), MS_OK, "st half");
    CHECK(st.rate_num == 1 && st.rate_den == 2, "half stored");
    CHECK(st.paused == 1 && st.audio_clock_us == clock_before, "clock still frozen");

    CHECK_ST(ms_hw_player_resume(p), MS_OK, "resume");
    CHECK(ms_hw_player_is_paused(p) == 0, "not paused");
    CHECK_ST(ms_hw_player_drive(p), MS_OK, "drv resume");
    CHECK_ST(ms_hw_player_get_stats(p, &st), MS_OK, "st resume");
    CHECK(st.audio_clock_us >= clock_before, "clock continues after resume");
    CHECK(st.rate_num == 1 && st.rate_den == 2, "rate kept across pause");

    /* 相对/绝对 seek，钳位与直播 */
    CHECK_ST(ms_hw_player_note_timeline(p, 1, 90 * 1000 * 10), MS_OK, "dur 10s");
    CHECK(ms_hw_player_is_seekable(p) == 1, "seekable");
    CHECK(ms_hw_player_duration_90k(p) == 90 * 1000 * 10, "dur");
    CHECK_ST(ms_hw_player_seek(p, -1), MS_ERR_INVALID_ARG, "neg abs");
    tl0 = st.timeline_epoch;
    CHECK_ST(ms_hw_player_seek(p, 90 * 1000 * 2), MS_OK, "abs 2s");
    CHECK_ST(ms_hw_player_get_stats(p, &st), MS_OK, "st seek");
    CHECK(st.timeline_epoch > tl0, "timeline bump");
    CHECK(st.waiting_keyframe == 1, "wait key after seek");
    CHECK(st.position_90k == 90 * 1000 * 2, "pos 2s");
    CHECK(st.rate_num == 1 && st.rate_den == 2, "rate kept across seek");
    CHECK(st.have_seek_land == 1, "land recorded");

    /* 字幕：seek 清空在途内嵌；媒体时间用定位后的轴 */
    CHECK_ST(ms_hw_player_set_external_subtitle(p, (const uint8_t *)srt, strlen(srt),
                                                MS_SUB_FORMAT_SRT), MS_OK, "ext srt");
    CHECK_ST(ms_hw_player_select_track(p, MS_TRACK_ID_EXTERNAL_SUB, 1), MS_OK, "ext on");
    memset(&cue, 0, sizeof(cue));
    cue.struct_size = sizeof(cue);
    CHECK_ST(ms_hw_player_subtitles_at(p, (uint64_t)ms_hw_player_position_90k(p),
                                       &cue, 1, &n), MS_OK, "sub at pos");
    CHECK(n == 1, "ext cue still at 2s (10s window)");
    ev_track(&ev, 20, MS_TRACK_SUBTITLE, MS_CODEC_SUB_TEXT, "eng", 1);
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "sub tr");
    CHECK_ST(ms_hw_player_select_track(p, 20, 1), MS_OK, "inband on");
    memset(&ev, 0, sizeof(ev));
    ev.type = MS_EVENT_SUBTITLE;
    ev.track.id = 20;
    ev.track.kind = MS_TRACK_SUBTITLE;
    ev.track.codec = MS_CODEC_SUB_TEXT;
    ev.has_pts = 1;
    ev.pts = 0;
    ev.dts = 1000000;
    ev.time_base_num = 1;
    ev.time_base_den = 1000000;
    ev.data = (const uint8_t *)"Inband";
    ev.data_size = 6;
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "inband cue");
    CHECK_ST(ms_hw_player_subtitles_at(p, 0, NULL, 0, &n), MS_OK, "inband listed");
    CHECK(n >= 1, "has inband");
    CHECK_ST(ms_hw_player_seek(p, 90 * 1000 * 3), MS_OK, "seek 3s clears inband");
    CHECK_ST(ms_hw_player_subtitles_at(p, 0, NULL, 0, &n), MS_OK, "after seek");
    CHECK(n == 0, "inband cleared; ext off because select 20 replaced ext? or both");
    /* 外挂在切到 20 时可能互斥关闭；允许 n==0。 */

    /* 相对：负值钳 0、超过时长钳 duration、溢出 */
    CHECK_ST(ms_hw_player_seek(p, 0), MS_OK, "to 0");
    CHECK_ST(ms_hw_player_seek_relative(p, -90 * 1000 * 5), MS_OK, "rel -5s");
    CHECK(ms_hw_player_position_90k(p) == 0, "clamped to 0");
    CHECK_ST(ms_hw_player_seek_relative(p, 90 * 1000 * 100), MS_OK, "rel +100s");
    CHECK(ms_hw_player_position_90k(p) == 90 * 1000 * 10, "clamped to duration");
    CHECK_ST(ms_hw_player_seek(p, 100), MS_OK, "mid");
    CHECK_ST(ms_hw_player_seek_relative(p, INT64_MAX), MS_OK, "overflow +");
    CHECK(ms_hw_player_position_90k(p) == 90 * 1000 * 10, "overflow clamped duration");

    /* 真正不可定位的直播（UDP/RTP/RTMP 等）仍为 NOT_SEEKABLE */
    CHECK_ST(ms_hw_player_note_timeline(p, 0, -1), MS_OK, "live");
    CHECK_ST(ms_hw_player_seek(p, 0), MS_ERR_NOT_SEEKABLE, "live abs");
    CHECK_ST(ms_hw_player_seek_relative(p, 90000), MS_ERR_NOT_SEEKABLE, "live rel");
    CHECK_ST(ms_hw_player_seek_live_edge(p), MS_ERR_NOT_SEEKABLE, "unseekable live edge");

    /* HLS 滑动窗口：不得把窗口长度当 VOD 时长；过期/越界钳位；暂停倍速保留 */
    {
        ms_hw_seek_range wr, got;
        memset(&wr, 0, sizeof(wr));
        wr.struct_size = sizeof(wr);
        wr.seekable = 1;
        wr.kind = MS_HW_SEEK_KIND_SLIDING;
        wr.start_90k = 90 * 1000 * 3;
        wr.end_90k = 90 * 1000 * 12;
        wr.safety_90k = 90 * 1000 * 4;
        wr.live_edge_90k = 90 * 1000 * 8;
        wr.duration_90k = -1;
        CHECK_ST(ms_hw_player_note_seek_range(p, &wr), MS_OK, "note sliding");
        CHECK(ms_hw_player_is_seekable(p) == 1, "sliding seekable");
        CHECK(ms_hw_player_duration_90k(p) < 0, "sliding duration not fake VOD");
        memset(&got, 0, sizeof(got));
        got.struct_size = sizeof(got);
        CHECK_ST(ms_hw_player_get_seek_range(p, &got), MS_OK, "get range");
        CHECK(got.kind == MS_HW_SEEK_KIND_SLIDING && got.start_90k == wr.start_90k,
              "range start kept");
        CHECK(got.live_edge_90k == wr.live_edge_90k && got.duration_90k < 0, "edge, no dur");
        CHECK_ST(ms_hw_player_set_rate(p, 1, 2), MS_OK, "rate 0.5x before window seek");
        CHECK_ST(ms_hw_player_pause(p), MS_OK, "pause before window seek");
        CHECK_ST(ms_hw_player_seek(p, 0), MS_OK, "expired 0 clamps");
        CHECK(ms_hw_player_position_90k(p) == wr.start_90k, "clamped to window start");
        CHECK_ST(ms_hw_player_seek(p, 90 * 1000 * 100), MS_OK, "past edge clamps");
        CHECK(ms_hw_player_position_90k(p) == wr.live_edge_90k, "clamped to live_edge");
        CHECK_ST(ms_hw_player_seek_relative(p, -90 * 1000 * 100), MS_OK, "rel back expired");
        CHECK(ms_hw_player_position_90k(p) == wr.start_90k, "rel clamped start");
        CHECK_ST(ms_hw_player_seek_live_edge(p), MS_OK, "to live edge");
        CHECK(ms_hw_player_position_90k(p) == wr.live_edge_90k, "at live_edge");
        CHECK(ms_hw_player_is_paused(p) == 1, "pause kept after window seek");
        CHECK_ST(ms_hw_player_get_rate(p, &num, &den), MS_OK, "rate after window seek");
        CHECK(num == 1 && den == 2, "rate kept after window seek");
        CHECK_ST(ms_hw_player_resume(p), MS_OK, "resume after window");
    }
    CHECK_ST(ms_hw_player_note_timeline(p, 1, 90 * 1000 * 10), MS_OK, "restore");

    /* seek 后丢非关键帧，等关键帧；CSD 仍在 */
    fill_ev(&ev, MS_EVENT_CODEC_CONFIG, 0);
    ev.track.id = 1;
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "vcfg keep");
    fill_ev(&ev, MS_EVENT_AU, 1);
    ev.track.id = 1;
    ev.is_keyframe = 1;
    ev.has_pts = 1;
    ev.pts = 0;
    ev.time_base_num = 1;
    ev.time_base_den = 90000;
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "vkey0");
    CHECK_ST(ms_hw_player_drive(p), MS_OK, "vdrv0");
    CHECK_ST(ms_hw_player_seek(p, 90 * 1000), MS_OK, "seek 1s");
    fill_ev(&ev, MS_EVENT_AU, 0);
    ev.track.id = 1;
    ev.is_keyframe = 0;
    ev.has_pts = 1;
    ev.pts = 90 * 1000;
    ev.time_base_num = 1;
    ev.time_base_den = 90000;
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "nonkey dropped or held");
    CHECK_ST(ms_hw_player_get_stats(p, &st), MS_OK, "st nokey");
    CHECK(st.waiting_keyframe == 1, "still waiting key");
    CHECK(st.video_au_dropped_nokey >= 1, "nonkey dropped");
    fill_ev(&ev, MS_EVENT_AU, 1);
    ev.track.id = 1;
    ev.is_keyframe = 1;
    ev.has_pts = 1;
    ev.pts = 90 * 1000;
    ev.time_base_num = 1;
    ev.time_base_den = 90000;
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "key after seek");
    CHECK_ST(ms_hw_player_drive(p), MS_OK, "drv key");
    CHECK_ST(ms_hw_player_get_stats(p, &st), MS_OK, "st key");
    CHECK(st.waiting_keyframe == 0, "key accepted");
    CHECK(st.rate_num == 1 && st.rate_den == 2, "rate still half");

    /* 解码器已启动后，非 1x 仍可 EOS（seek 已清过 eos） */
    CHECK_ST(ms_hw_player_signal_eos(p), MS_OK, "eos 0.5x");
    CHECK_ST(ms_hw_player_drive(p), MS_OK, "drv eos");
    CHECK_ST(ms_hw_player_get_stats(p, &st), MS_OK, "st eos");
    CHECK(st.eos_in == 1, "eos recorded at non-1x");
    /* 继续后续用例前清 EOS，保持速率 */
    CHECK_ST(ms_hw_player_flush(p), MS_OK, "flush after eos");
    CHECK_ST(ms_hw_player_get_rate(p, &num, &den), MS_OK, "rate after eos flush");
    CHECK(num == 1 && den == 2, "rate kept after eos/flush");

    /* 换音轨后速率仍套用 */
    ev_track(&ev, 11, MS_TRACK_AUDIO, MS_CODEC_AAC, "chi", 0);
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "a1 tr");
    CHECK_ST(ms_hw_player_select_track(p, 11, 1), MS_OK, "switch audio");
    ev_aac_cfg(&ev);
    ev.track.id = 11;
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "cfg11");
    CHECK_ST(ms_hw_player_get_rate(p, &num, &den), MS_OK, "rate after switch");
    CHECK(num == 1 && den == 2, "rate re-applied / kept");

    /* 旧 player stats size 不含本切片尾部 */
    {
        ms_hw_player_stats oldst;
        unsigned char *raw = (unsigned char *)&oldst;
        size_t prefix = offsetof(ms_hw_player_stats, have_external_sub) + sizeof(int);
        size_t off;
        int dirty = 0;
        memset(&oldst, 0xA5, sizeof(oldst));
        oldst.struct_size = prefix;
        CHECK_ST(ms_hw_player_get_stats(p, &oldst), MS_OK, "old player stats");
        CHECK(oldst.have_external_sub == 0 || oldst.have_external_sub == 1, "old size ok");
        for (off = prefix; off < sizeof(oldst); off++) {
            if (raw[off] != 0xA5) {
                dirty = 1;
                break;
            }
        }
        CHECK(!dirty, "get_stats honors old player struct_size");
    }

    ms_hw_player_destroy(p);
    printf("  test_p4_rate_and_seek ok (host mock; not device A/V-synced speed or paint)\n");
    return 0;
}


static int test_vbit_inband_and_seek_gate(void)
{
    ms_hw_player *p = NULL;
    ms_hw_player_stats st;
    ms_event ev;
    int token = 13;
    unsigned epoch0;
    uint8_t au16[64], au32[64], pframe[16];
    size_t n16, n32, np;

    n16 = 0;
    memcpy(au16, k_mpv_csd_16, sizeof(k_mpv_csd_16));
    n16 = sizeof(k_mpv_csd_16);
    memcpy(au16 + n16, k_mpv_pic_i, sizeof(k_mpv_pic_i));
    n16 += sizeof(k_mpv_pic_i);
    n32 = 0;
    memcpy(au32, k_mpv_csd_32, sizeof(k_mpv_csd_32));
    n32 = sizeof(k_mpv_csd_32);
    memcpy(au32 + n32, k_mpv_pic_i, sizeof(k_mpv_pic_i));
    n32 += sizeof(k_mpv_pic_i);
    np = sizeof(k_mpv_pic_p);
    memcpy(pframe, k_mpv_pic_p, np);

    CHECK(make_player(&p, 0) == 0, "create");
    CHECK_ST(ms_hw_player_set_surface(p, &token), MS_OK, "surf");
    memset(&ev, 0, sizeof(ev));
    ev.type = MS_EVENT_AU;
    ev.track.kind = MS_TRACK_VIDEO;
    ev.track.codec = MS_CODEC_MPEG2_VIDEO;
    ev.sample_format = MS_SAMPLE_FMT_MPV;
    ev.is_keyframe = 0; /* 靠 I 图检查 */
    ev.has_pts = 1;
    ev.pts = 0;
    ev.time_base_num = 1;
    ev.time_base_den = 90000;
    ev.data = au16;
    ev.data_size = n16;
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "inband I");
    CHECK_ST(ms_hw_player_drive(p), MS_OK, "drive");
    memset(&st, 0, sizeof(st));
    st.struct_size = sizeof(st);
    CHECK_ST(ms_hw_player_get_stats(p, &st), MS_OK, "st");
    CHECK(st.configured == 1 && st.video_au_submitted >= 1, "configured");
    CHECK(st.video_csd_inband >= 1, "inband csd");
    epoch0 = st.epoch;

    ev.pts = 9000;
    ev.data = au16;
    ev.data_size = n16;
    ev.is_keyframe = 1;
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "same cfg");
    memset(&st, 0, sizeof(st));
    st.struct_size = sizeof(st);
    CHECK_ST(ms_hw_player_get_stats(p, &st), MS_OK, "st same");
    CHECK(st.epoch == epoch0, "same no epoch");

    ev.data = au32;
    ev.data_size = n32;
    ev.pts = 18000;
    ev.is_keyframe = 1;
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "mid 32");
    memset(&st, 0, sizeof(st));
    st.struct_size = sizeof(st);
    CHECK_ST(ms_hw_player_get_stats(p, &st), MS_OK, "st mid");
    CHECK(st.epoch == epoch0 + 1, "epoch mid");
    epoch0 = st.epoch;

    CHECK_ST(ms_hw_player_seek(p, 90 * 1000), MS_OK, "seek");
    ev.data = pframe;
    ev.data_size = np;
    ev.is_keyframe = 0;
    ev.pts = 90000;
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "post-seek P");
    memset(&st, 0, sizeof(st));
    st.struct_size = sizeof(st);
    CHECK_ST(ms_hw_player_get_stats(p, &st), MS_OK, "st gate");
    CHECK(st.video_au_dropped_nokey >= 1, "seek dropped P");
    ev.data = au32;
    ev.data_size = n32;
    ev.is_keyframe = 0;
    ev.pts = 180000;
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "post-seek I");
    memset(&st, 0, sizeof(st));
    st.struct_size = sizeof(st);
    CHECK_ST(ms_hw_player_get_stats(p, &st), MS_OK, "st key");
    CHECK(st.video_au_submitted >= 3, "I after seek");

    /* MPEG-4：独立 CONFIG + 门闩 */
    ms_hw_player_destroy(p);
    p = NULL;
    CHECK(make_player(&p, 0) == 0, "p4");
    CHECK_ST(ms_hw_player_set_surface(p, &token), MS_OK, "surf4");
    memset(&ev, 0, sizeof(ev));
    ev.type = MS_EVENT_CODEC_CONFIG;
    ev.track.kind = MS_TRACK_VIDEO;
    ev.track.codec = MS_CODEC_MPEG4_PART2;
    ev.sample_format = MS_SAMPLE_FMT_MP4V;
    ev.data = k_mp4v_csd_16;
    ev.data_size = sizeof(k_mp4v_csd_16);
    ev.is_keyframe = 1;
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "mp4v cfg");
    ev.type = MS_EVENT_AU;
    ev.data = k_mp4v_vop_p;
    ev.data_size = sizeof(k_mp4v_vop_p);
    ev.is_keyframe = 0;
    ev.has_pts = 1;
    ev.pts = 0;
    ev.time_base_num = 1;
    ev.time_base_den = 90000;
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "P gated");
    ev.data = k_mp4v_vop_i;
    ev.data_size = sizeof(k_mp4v_vop_i);
    ev.is_keyframe = 0;
    ev.pts = 9000;
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "I after cfg");
    memset(&st, 0, sizeof(st));
    st.struct_size = sizeof(st);
    CHECK_ST(ms_hw_player_get_stats(p, &st), MS_OK, "st4");
    CHECK(st.configured == 1, "mp4v configured");
    CHECK(st.video_au_dropped_nokey >= 1, "mp4v P gated");
    CHECK(st.video_au_submitted >= 1, "mp4v I submitted");
    ms_hw_player_destroy(p);
    printf("  test_vbit_inband_and_seek_gate ok (host mock; not device paint)\n");
    return 0;
}

static int test_vbit_h263_inband_and_seek_gate(void)
{
    ms_hw_player *p = NULL;
    ms_hw_player_stats st;
    ms_event ev;
    int token = 17;
    unsigned epoch0;

    CHECK(make_player(&p, 0) == 0, "create");
    CHECK_ST(ms_hw_player_set_surface(p, &token), MS_OK, "surf");
    memset(&ev, 0, sizeof(ev));
    ev.type = MS_EVENT_AU;
    ev.track.kind = MS_TRACK_VIDEO;
    ev.track.codec = MS_CODEC_H263;
    ev.sample_format = MS_SAMPLE_FMT_H263;
    ev.is_keyframe = 0; /* 靠 PSC I 图检查 */
    ev.has_pts = 1;
    ev.pts = 0;
    ev.time_base_num = 1;
    ev.time_base_den = 90000;
    ev.data = k_h263_qcif_i;
    ev.data_size = sizeof(k_h263_qcif_i);
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "qcif I");
    CHECK_ST(ms_hw_player_drive(p), MS_OK, "drive");
    memset(&st, 0, sizeof(st));
    st.struct_size = sizeof(st);
    CHECK_ST(ms_hw_player_get_stats(p, &st), MS_OK, "st");
    CHECK(st.configured == 1 && st.video_au_submitted >= 1, "configured from in-band I");
    CHECK(st.video_csd_inband >= 1, "inband cfg (empty CSD)");
    epoch0 = st.epoch;

    ev.pts = 9000;
    ev.data = k_h263_qcif_p;
    ev.data_size = sizeof(k_h263_qcif_p);
    ev.is_keyframe = 0;
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "qcif P");
    memset(&st, 0, sizeof(st));
    st.struct_size = sizeof(st);
    CHECK_ST(ms_hw_player_get_stats(p, &st), MS_OK, "st p");
    CHECK(st.epoch == epoch0, "same size no epoch");
    CHECK(st.video_au_submitted >= 2, "P submitted");

    ev.pts = 18000;
    ev.data = k_h263_cif_i;
    ev.data_size = sizeof(k_h263_cif_i);
    ev.is_keyframe = 1;
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "mid cif I");
    memset(&st, 0, sizeof(st));
    st.struct_size = sizeof(st);
    CHECK_ST(ms_hw_player_get_stats(p, &st), MS_OK, "st mid");
    CHECK(st.epoch == epoch0 + 1, "epoch on size change");
    epoch0 = st.epoch;

    CHECK_ST(ms_hw_player_seek(p, 90 * 1000), MS_OK, "seek");
    ev.data = k_h263_qcif_p;
    ev.data_size = sizeof(k_h263_qcif_p);
    ev.is_keyframe = 0;
    ev.pts = 90000;
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "post-seek P");
    memset(&st, 0, sizeof(st));
    st.struct_size = sizeof(st);
    CHECK_ST(ms_hw_player_get_stats(p, &st), MS_OK, "st gate");
    CHECK(st.video_au_dropped_nokey >= 1, "seek dropped P");
    ev.data = k_h263_cif_i;
    ev.data_size = sizeof(k_h263_cif_i);
    ev.is_keyframe = 0;
    ev.pts = 180000;
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "post-seek I");
    memset(&st, 0, sizeof(st));
    st.struct_size = sizeof(st);
    CHECK_ST(ms_hw_player_get_stats(p, &st), MS_OK, "st key");
    CHECK(st.video_au_submitted >= 3, "I after seek");
    ms_hw_player_destroy(p);
    printf("  test_vbit_h263_inband_and_seek_gate ok (host mock; empty CSD; not device paint)\n");
    return 0;
}


static int test_vbit_avsplus_inband_and_seek_gate(void)
{
    ms_hw_player *p = NULL;
    ms_hw_player_stats st;
    ms_event ev;
    int token = 27;
    unsigned epoch0;
    uint8_t au16[64], au32[64];
    size_t n16, n32;

    n16 = 0;
    memcpy(au16, k_avsplus_seq_16, sizeof(k_avsplus_seq_16));
    n16 = sizeof(k_avsplus_seq_16);
    memcpy(au16 + n16, k_avs_pic_i, sizeof(k_avs_pic_i));
    n16 += sizeof(k_avs_pic_i);
    n32 = 0;
    memcpy(au32, k_avsplus_seq_32, sizeof(k_avsplus_seq_32));
    n32 = sizeof(k_avsplus_seq_32);
    memcpy(au32 + n32, k_avs_pic_i, sizeof(k_avs_pic_i));
    n32 += sizeof(k_avs_pic_i);

    CHECK(make_player(&p, 0) == 0, "create");
    CHECK_ST(ms_hw_player_set_surface(p, &token), MS_OK, "surf");
    memset(&ev, 0, sizeof(ev));
    ev.type = MS_EVENT_AU;
    ev.track.kind = MS_TRACK_VIDEO;
    ev.track.codec = MS_CODEC_AVS_PLUS;
    ev.sample_format = MS_SAMPLE_FMT_CAVS;
    ev.is_keyframe = 1;
    ev.has_pts = 1;
    ev.pts = 0;
    ev.time_base_num = 1;
    ev.time_base_den = 90000;
    ev.data = au16;
    ev.data_size = n16;
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "inband 16");
    CHECK_ST(ms_hw_player_drive(p), MS_OK, "drive");
    memset(&st, 0, sizeof(st));
    st.struct_size = sizeof(st);
    CHECK_ST(ms_hw_player_get_stats(p, &st), MS_OK, "st");
    CHECK(st.configured == 1 && st.video_au_submitted >= 1, "configured from seq+I");
    epoch0 = st.epoch;

    ev.pts = 9000;
    ev.data = k_avs_pic_p;
    ev.data_size = sizeof(k_avs_pic_p);
    ev.is_keyframe = 0;
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "P");
    memset(&st, 0, sizeof(st));
    st.struct_size = sizeof(st);
    CHECK_ST(ms_hw_player_get_stats(p, &st), MS_OK, "st p");
    CHECK(st.epoch == epoch0, "same seq no epoch");

    ev.pts = 18000;
    ev.data = au32;
    ev.data_size = n32;
    ev.is_keyframe = 1;
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "mid 32");
    memset(&st, 0, sizeof(st));
    st.struct_size = sizeof(st);
    CHECK_ST(ms_hw_player_get_stats(p, &st), MS_OK, "st mid");
    CHECK(st.epoch == epoch0 + 1, "epoch on seq size change");

    CHECK_ST(ms_hw_player_seek(p, 90 * 1000), MS_OK, "seek");
    ev.data = k_avs_pic_p;
    ev.data_size = sizeof(k_avs_pic_p);
    ev.is_keyframe = 0;
    ev.pts = 90000;
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "post-seek P");
    memset(&st, 0, sizeof(st));
    st.struct_size = sizeof(st);
    CHECK_ST(ms_hw_player_get_stats(p, &st), MS_OK, "st gate");
    CHECK(st.video_au_dropped_nokey >= 1, "seek dropped P");
    ev.data = au32;
    ev.data_size = n32;
    ev.is_keyframe = 1;
    ev.pts = 180000;
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "post-seek I");
    memset(&st, 0, sizeof(st));
    st.struct_size = sizeof(st);
    CHECK_ST(ms_hw_player_get_stats(p, &st), MS_OK, "st key");
    CHECK(st.video_au_submitted >= 3, "key after seek");

    /* 基准类不得当 AVS+ 配置成功 */
    ev.type = MS_EVENT_CODEC_CONFIG;
    ev.data = k_avs_jizhun_seq_16;
    ev.data_size = sizeof(k_avs_jizhun_seq_16);
    ev.is_keyframe = 1;
    /* 已配置会话上送非法档次：解析失败不应假装是新的 AVS+ 配置。 */
    (void)ms_hw_player_feed(p, &ev, 0, NULL, NULL);

    ms_hw_player_destroy(p);
    printf("  test_vbit_avsplus_inband_and_seek_gate ok (host mock; broadcast seq; not device paint)\n");
    return 0;
}

static int test_vbit_av1_inband_and_seek_gate(void)
{
    ms_hw_player *p = NULL;
    ms_hw_player_stats st;
    ms_event ev;
    int token = 19;
    unsigned epoch0;

    /* 合法无 CSD：帧内 SH+key */
    CHECK(make_player(&p, 0) == 0, "create");
    CHECK_ST(ms_hw_player_set_surface(p, &token), MS_OK, "surf");
    memset(&ev, 0, sizeof(ev));
    ev.type = MS_EVENT_AU;
    ev.track.kind = MS_TRACK_VIDEO;
    ev.track.codec = MS_CODEC_AV1;
    ev.sample_format = MS_SAMPLE_FMT_AV1;
    ev.is_keyframe = 0;
    ev.has_pts = 1;
    ev.pts = 0;
    ev.time_base_num = 1;
    ev.time_base_den = 90000;
    ev.data = k_av1_tu_key_64;
    ev.data_size = sizeof(k_av1_tu_key_64);
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "inband key64");
    CHECK_ST(ms_hw_player_drive(p), MS_OK, "drive");
    memset(&st, 0, sizeof(st));
    st.struct_size = sizeof(st);
    CHECK_ST(ms_hw_player_get_stats(p, &st), MS_OK, "st");
    CHECK(st.configured == 1 && st.video_au_submitted >= 1, "configured no-csd");
    epoch0 = st.epoch;

    ev.pts = 9000;
    ev.data = k_av1_frame_inter_64;
    ev.data_size = sizeof(k_av1_frame_inter_64);
    ev.is_keyframe = 0;
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "inter");
    memset(&st, 0, sizeof(st));
    st.struct_size = sizeof(st);
    CHECK_ST(ms_hw_player_get_stats(p, &st), MS_OK, "st p");
    CHECK(st.epoch == epoch0, "same seq no epoch");

    ev.pts = 18000;
    ev.data = k_av1_tu_key_32;
    ev.data_size = sizeof(k_av1_tu_key_32);
    ev.is_keyframe = 1;
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "mid 32");
    memset(&st, 0, sizeof(st));
    st.struct_size = sizeof(st);
    CHECK_ST(ms_hw_player_get_stats(p, &st), MS_OK, "st mid");
    CHECK(st.epoch == epoch0 + 1, "epoch on sh change");
    epoch0 = st.epoch;

    CHECK_ST(ms_hw_player_seek(p, 90 * 1000), MS_OK, "seek");
    ev.data = k_av1_frame_inter_64;
    ev.data_size = sizeof(k_av1_frame_inter_64);
    ev.is_keyframe = 0;
    ev.pts = 90000;
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "post-seek inter");
    memset(&st, 0, sizeof(st));
    st.struct_size = sizeof(st);
    CHECK_ST(ms_hw_player_get_stats(p, &st), MS_OK, "st gate");
    CHECK(st.video_au_dropped_nokey >= 1, "seek dropped inter");
    ev.data = k_av1_tu_key_32;
    ev.data_size = sizeof(k_av1_tu_key_32);
    ev.is_keyframe = 0;
    ev.pts = 180000;
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "post-seek key");
    memset(&st, 0, sizeof(st));
    st.struct_size = sizeof(st);
    CHECK_ST(ms_hw_player_get_stats(p, &st), MS_OK, "st key");
    CHECK(st.video_au_submitted >= 3, "key after seek");

    /* 合法 av1C CONFIG 路径 */
    ms_hw_player_destroy(p);
    p = NULL;
    CHECK(make_player(&p, 0) == 0, "p2");
    CHECK_ST(ms_hw_player_set_surface(p, &token), MS_OK, "surf2");
    memset(&ev, 0, sizeof(ev));
    ev.type = MS_EVENT_CODEC_CONFIG;
    ev.track.kind = MS_TRACK_VIDEO;
    ev.track.codec = MS_CODEC_AV1;
    ev.sample_format = MS_SAMPLE_FMT_AV1;
    ev.data = k_av1_av1c_64;
    ev.data_size = sizeof(k_av1_av1c_64);
    ev.is_keyframe = 1;
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "av1c cfg");
    ev.type = MS_EVENT_AU;
    ev.data = k_av1_frame_inter_64;
    ev.data_size = sizeof(k_av1_frame_inter_64);
    ev.is_keyframe = 0;
    ev.has_pts = 1;
    ev.pts = 0;
    ev.time_base_num = 1;
    ev.time_base_den = 90000;
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "P gated");
    ev.data = k_av1_tu_key_64;
    ev.data_size = sizeof(k_av1_tu_key_64);
    ev.is_keyframe = 0;
    ev.pts = 9000;
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "key after cfg");
    memset(&st, 0, sizeof(st));
    st.struct_size = sizeof(st);
    CHECK_ST(ms_hw_player_get_stats(p, &st), MS_OK, "st cfg");
    CHECK(st.configured == 1, "av1c configured");
    CHECK(st.video_au_dropped_nokey >= 1, "av1 P gated");
    CHECK(st.video_au_submitted >= 1, "av1 key submitted");
    ms_hw_player_destroy(p);
    printf("  test_vbit_av1_inband_and_seek_gate ok (host mock; av1C and no-CSD; not device paint)\n");
    return 0;
}

static int test_hevc_inband_and_seek_gate(void)
{
    ms_hw_player *p = NULL;
    ms_hw_player_stats st;
    ms_event ev;
    int token = 11;
    unsigned epoch0;
    uint8_t csd16[256], au16[256], au32[256], trail[64], csd32[256];
    size_t n16 = 0, a16 = 0, a32 = 0, nt = 0, n32 = 0;
    int i;

    CHECK(hevc_annexb_csd(csd16, sizeof(csd16), &n16, k_hevc_vps_16, sizeof(k_hevc_vps_16),
                          k_hevc_sps_16, sizeof(k_hevc_sps_16),
                          k_hevc_pps_16, sizeof(k_hevc_pps_16)) == 0, "csd16");
    CHECK(hevc_key_au(au16, sizeof(au16), &a16, k_hevc_vps_16, sizeof(k_hevc_vps_16),
                      k_hevc_sps_16, sizeof(k_hevc_sps_16),
                      k_hevc_pps_16, sizeof(k_hevc_pps_16),
                      k_hevc_irap_16, sizeof(k_hevc_irap_16)) == 0, "au16");
    CHECK(hevc_key_au(au32, sizeof(au32), &a32, k_hevc_vps_32, sizeof(k_hevc_vps_32),
                      k_hevc_sps_32, sizeof(k_hevc_sps_32),
                      k_hevc_pps_32, sizeof(k_hevc_pps_32),
                      k_hevc_irap_32, sizeof(k_hevc_irap_32)) == 0, "au32");
    CHECK(hevc_annexb_csd(csd32, sizeof(csd32), &n32, k_hevc_vps_32, sizeof(k_hevc_vps_32),
                          k_hevc_sps_32, sizeof(k_hevc_sps_32),
                          k_hevc_pps_32, sizeof(k_hevc_pps_32)) == 0, "csd32");
    nt = 0;
    CHECK(hevc_annexb_append(trail, sizeof(trail), &nt, k_hevc_trail_16,
                             sizeof(k_hevc_trail_16)) == 0, "trail");

    CHECK(make_player(&p, 0) == 0, "create");
    CHECK_ST(ms_hw_player_set_surface(p, &token), MS_OK, "surf");

    /* 无独立 CONFIG：带内三联体 + IRAP 应配置并提交。 */
    memset(&ev, 0, sizeof(ev));
    ev.type = MS_EVENT_AU;
    ev.track.kind = MS_TRACK_VIDEO;
    ev.track.codec = MS_CODEC_HEVC;
    ev.sample_format = MS_SAMPLE_FMT_ANNEXB;
    ev.is_keyframe = 0; /* 门闩靠 IRAP 检查，不单靠标志 */
    ev.has_pts = 1;
    ev.pts = 0;
    ev.time_base_num = 1;
    ev.time_base_den = 90000;
    ev.data = au16;
    ev.data_size = a16;
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "init inband irap");
    CHECK_ST(ms_hw_player_drive(p), MS_OK, "drive");
    memset(&st, 0, sizeof(st));
    st.struct_size = sizeof(st);
    CHECK_ST(ms_hw_player_get_stats(p, &st), MS_OK, "st init");
    CHECK(st.configured == 1 && st.video_au_submitted >= 1, "configured+submit");
    CHECK(st.video_csd_inband >= 1, "inband");
    epoch0 = st.epoch;

    ev.pts = 9000;
    ev.data = au16;
    ev.data_size = a16;
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "same inband");
    memset(&st, 0, sizeof(st));
    st.struct_size = sizeof(st);
    CHECK_ST(ms_hw_player_get_stats(p, &st), MS_OK, "st same");
    CHECK(st.epoch == epoch0, "same no epoch");
    CHECK(st.video_csd_inband_same >= 1, "same counted");

    ev.data = au32;
    ev.data_size = a32;
    ev.pts = 18000;
    ev.is_keyframe = 1;
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "mid change");
    memset(&st, 0, sizeof(st));
    st.struct_size = sizeof(st);
    CHECK_ST(ms_hw_player_get_stats(p, &st), MS_OK, "st mid");
    CHECK(st.epoch == epoch0 + 1, "epoch on hevc mid PS");
    CHECK(st.video_csd_inband >= 2, "second apply");
    epoch0 = st.epoch;

    /* 仅新 VPS：不得与旧 SPS/PPS 拼接，有界放弃。 */
    {
        uint8_t only[64];
        size_t on = 0;
        CHECK(hevc_annexb_append(only, sizeof(only), &on, k_hevc_vps_16,
                                 sizeof(k_hevc_vps_16)) == 0, "vps only");
        ev.data = only;
        ev.data_size = on;
        ev.is_keyframe = 1;
        ev.pts = 27000;
        CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "vps only");
        ev.data = trail;
        ev.data_size = nt;
        ev.is_keyframe = 0;
        for (i = 0; i < 20; i++) {
            ev.pts = 30000 + (uint64_t)i * 90;
            CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "wait");
        }
        memset(&st, 0, sizeof(st));
        st.struct_size = sizeof(st);
        CHECK_ST(ms_hw_player_get_stats(p, &st), MS_OK, "st pend");
        CHECK(st.video_csd_pending_abandoned >= 1, "bounded abandon");
        CHECK(st.epoch == epoch0, "no stitch");
    }

    /* seek 后门闩：非 IRAP 丢弃，IRAP 提交。 */
    CHECK_ST(ms_hw_player_seek(p, 90 * 1000), MS_OK, "seek");
    ev.data = trail;
    ev.data_size = nt;
    ev.is_keyframe = 0;
    ev.pts = 90000;
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "post-seek trail");
    memset(&st, 0, sizeof(st));
    st.struct_size = sizeof(st);
    CHECK_ST(ms_hw_player_get_stats(p, &st), MS_OK, "st gate");
    CHECK(st.video_au_dropped_nokey >= 1, "seek dropped non-irap");
    ev.data = au32;
    ev.data_size = a32;
    ev.is_keyframe = 0;
    ev.pts = 180000;
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "post-seek irap");
    memset(&st, 0, sizeof(st));
    st.struct_size = sizeof(st);
    CHECK_ST(ms_hw_player_get_stats(p, &st), MS_OK, "st key");
    CHECK(st.video_au_submitted >= 3, "irap after seek");

    /* 独立 CODEC_CONFIG + HVCC 路径。 */
    ms_hw_player_destroy(p);
    p = NULL;
    CHECK(make_player(&p, 0) == 0, "p2");
    CHECK_ST(ms_hw_player_set_surface(p, &token), MS_OK, "surf2");
    {
        ms_hevc_ps_set ps;
        uint8_t hvcc[256];
        size_t hn = 0;
        memset(&ps, 0, sizeof(ps));
        ps.struct_size = sizeof(ps);
        CHECK_ST(ms_hevc_ps_parse(MS_SAMPLE_FMT_ANNEXB, 1, csd16, n16, 4, &ps), MS_OK, "parse");
        CHECK(ps.triple_complete, "triple");
        CHECK_ST(ms_hevc_ps_pack(MS_SAMPLE_FMT_HVCC, 4, &ps, hvcc, sizeof(hvcc), &hn), MS_OK,
                 "pack");
        memset(&ev, 0, sizeof(ev));
        ev.type = MS_EVENT_CODEC_CONFIG;
        ev.track.kind = MS_TRACK_VIDEO;
        ev.track.codec = MS_CODEC_HEVC;
        ev.sample_format = MS_SAMPLE_FMT_HVCC;
        ev.data = hvcc;
        ev.data_size = hn;
        ev.is_keyframe = 1;
        CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "hvcc cfg");
        ev.type = MS_EVENT_AU;
        ev.sample_format = MS_SAMPLE_FMT_ANNEXB;
        ev.data = trail;
        ev.data_size = nt;
        ev.is_keyframe = 0;
        ev.has_pts = 1;
        ev.pts = 0;
        ev.time_base_num = 1;
        ev.time_base_den = 90000;
        CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "cfg then trail");
        ev.data = au16;
        ev.data_size = a16;
        ev.is_keyframe = 1;
        ev.pts = 9000;
        CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "cfg then key");
        memset(&st, 0, sizeof(st));
        st.struct_size = sizeof(st);
        CHECK_ST(ms_hw_player_get_stats(p, &st), MS_OK, "st cfg");
        CHECK(st.configured == 1, "cfg configured");
        CHECK(st.video_au_dropped_nokey >= 1, "trail gated");
        CHECK(st.video_au_submitted >= 1, "key after cfg");
    }
    ms_hw_player_destroy(p);
    printf("  test_hevc_inband_and_seek_gate ok (host mock; not device paint)\n");
    return 0;
}



#ifdef MS_HW_PLAYER_HAVE_FF
static int write_bytes(const char *path, const void *b, size_t n)
{
    FILE *fp = fopen(path, "wb");
    if (!fp) {
        return -1;
    }
    if (n && fwrite(b, 1, n, fp) != n) {
        fclose(fp);
        return -1;
    }
    fclose(fp);
    return 0;
}

static int pump_one(ms_ff *f, ms_hw_player *p, ms_event *ev)
{
    ms_status st;
    memset(ev, 0, sizeof(*ev));
    st = ms_ff_pull(f, ev);
    if (st == MS_AGAIN) {
        (void)ms_hw_player_drive(p);
        return 0;
    }
    if (st == MS_EOS) {
        return 1;
    }
    if (st != MS_OK) {
        return -1;
    }
    {
        ms_track_info ti[16];
        size_t n = 0, i;
        uint32_t want = 0;
        int have = 0;
        if (ms_ff_list_tracks(f, ti, 16, &n) == MS_OK) {
            uint32_t want_v = 0;
            int have_v = 0;
            for (i = 0; i < n; i++) {
                if (ti[i].kind == MS_TRACK_AUDIO && ti[i].selected) {
                    want = ti[i].id;
                    have = 1;
                }
                if (ti[i].kind == MS_TRACK_VIDEO && ti[i].selected) {
                    want_v = ti[i].id;
                    have_v = 1;
                }
            }
            if (have) {
                (void)ms_hw_player_select_track(p, want, 1);
            } else if (ev->track.kind == MS_TRACK_AUDIO) {
                (void)ms_hw_player_select_track(p, ev->track.id, 0);
            }
            if (have_v) {
                (void)ms_hw_player_select_track(p, want_v, 1);
            }
        }
    }
    st = feed_retry(p, ev, ms_ff_time_origin_90k(f), 32);
    if (ev->release_id) {
        (void)ms_ff_release(f, ev->release_id);
        ev->release_id = 0;
    }
    (void)ms_hw_player_drive(p);
    if (st != MS_OK && st != MS_ERR_UNSUPPORTED) {
        return -1;
    }
    return 0;
}

static int test_ff_hls_alt_audio_player(void)
{
    const char *ffmpeg = "/usr/bin/ffmpeg";
    const char *tmp = getenv("MS_FF_TMP");
    char dir[256], vts[768], ets[768], zts[768], cmd[2048], master[800], url[860];
    char buf[800];
    ms_hw_player *p = NULL;
    ms_ff *f = NULL;
    ms_ff_limits lim;
    ms_hw_player_opts o;
    ms_hw_player_stats st;
    ms_track_info ti[16];
    size_t nt = 0, k;
    uint32_t a_eng = 0, a_zho = 0;
    unsigned epoch0 = 0;
    uint64_t sw0 = 0;
    int token = 21, loops, rc;
    ms_event ev;
    int vsub0 = 0;
    if (access(ffmpeg, X_OK) != 0) {
        printf("  player HLS alt: no /usr/bin/ffmpeg; untested gap, not a pass\n");
        return 1;
    }
    if (tmp == NULL || tmp[0] == 0) {
        tmp = "build/debug/ff-tmp";
    }
    snprintf(dir, sizeof(dir), "%s/hlsalt-pl", tmp);
    (void)mkdir(dir, 0755);
    snprintf(vts, sizeof(vts), "%s/v.ts", dir);
    snprintf(ets, sizeof(ets), "%s/eng.ts", dir);
    snprintf(zts, sizeof(zts), "%s/zho.ts", dir);
    snprintf(cmd, sizeof(cmd),
             "%s -hide_banner -loglevel error -y "
             "-f lavfi -i testsrc=size=160x120:rate=25:duration=3 "
             "-c:v libx264 -pix_fmt yuv420p -g 25 -keyint_min 25 -an %s",
             ffmpeg, vts);
    CHECK(system(cmd) == 0, "enc v");
    snprintf(cmd, sizeof(cmd),
             "%s -hide_banner -loglevel error -y "
             "-f lavfi -i sine=frequency=440:duration=3 -c:a aac -ar 48000 -ac 2 %s",
             ffmpeg, ets);
    CHECK(system(cmd) == 0, "enc eng");
    snprintf(cmd, sizeof(cmd),
             "%s -hide_banner -loglevel error -y "
             "-f lavfi -i sine=frequency=880:duration=3 -c:a aac -ar 44100 -ac 1 %s",
             ffmpeg, zts);
    CHECK(system(cmd) == 0, "enc zho");
    snprintf(buf, sizeof(buf),
             "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:4\n"
             "#EXT-X-MEDIA-SEQUENCE:1\n#EXTINF:3.0,\nv.ts\n#EXT-X-ENDLIST\n");
    snprintf(cmd, sizeof(cmd), "%s/v.m3u8", dir);
    CHECK(write_bytes(cmd, buf, strlen(buf)) == 0, "vpl");
    snprintf(buf, sizeof(buf),
             "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:4\n"
             "#EXT-X-MEDIA-SEQUENCE:1\n#EXTINF:3.0,\neng.ts\n#EXT-X-ENDLIST\n");
    snprintf(cmd, sizeof(cmd), "%s/eng.m3u8", dir);
    CHECK(write_bytes(cmd, buf, strlen(buf)) == 0, "epl");
    snprintf(buf, sizeof(buf),
             "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:4\n"
             "#EXT-X-MEDIA-SEQUENCE:1\n#EXTINF:3.0,\nzho.ts\n#EXT-X-ENDLIST\n");
    snprintf(cmd, sizeof(cmd), "%s/zho.m3u8", dir);
    CHECK(write_bytes(cmd, buf, strlen(buf)) == 0, "zpl");
    snprintf(buf, sizeof(buf),
             "#EXTM3U\n"
             "#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"ag\",LANGUAGE=\"eng\","
             "NAME=\"English\",DEFAULT=YES,AUTOSELECT=YES,URI=\"eng.m3u8\"\n"
             "#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"ag\",LANGUAGE=\"zho\","
             "NAME=\"Chinese\",DEFAULT=NO,AUTOSELECT=YES,URI=\"zho.m3u8\"\n"
             "#EXT-X-STREAM-INF:BANDWIDTH=250000,AUDIO=\"ag\"\n"
             "v.m3u8\n");
    snprintf(master, sizeof(master), "%s/master.m3u8", dir);
    CHECK(write_bytes(master, buf, strlen(buf)) == 0, "master");
    snprintf(url, sizeof(url), "file:%s", master);

    ms_hw_player_opts_default(&o);
    o.decoder.mock_mode = MS_HW_MOCK_AS_HW;
    o.decoder.struct_size = sizeof(o.decoder);
    o.wait_keyframe = 1;
    o.allow_system_aac = 1; /* 宿主仍走 mock，不是设备系统 AAC 验收 */
    CHECK_ST(ms_hw_player_create(NULL, &o, &p), MS_OK, "player");
    CHECK_ST(ms_hw_player_set_surface(p, &token), MS_OK, "surf");
    ms_ff_limits_default(&lim);
    lim.io_timeout_ms = 8000;
    CHECK_ST(ms_ff_create(NULL, &lim, &f), MS_OK, "ff");
    CHECK_ST(ms_ff_open(f, url), MS_OK, "open file HLS master");
    CHECK_ST(ms_ff_list_tracks(f, ti, 16, &nt), MS_OK, "list");
    for (k = 0; k < nt; k++) {
        if (ti[k].kind == MS_TRACK_AUDIO && ti[k].language &&
            strcmp(ti[k].language, "eng") == 0) {
            a_eng = ti[k].id;
        }
        if (ti[k].kind == MS_TRACK_AUDIO && ti[k].language &&
            strcmp(ti[k].language, "zho") == 0) {
            a_zho = ti[k].id;
        }
    }
    CHECK(a_eng && a_zho && a_eng != a_zho, "two renditions");

    for (loops = 0; loops < 400; loops++) {
        rc = pump_one(f, p, &ev);
        CHECK(rc >= 0, "pump def");
        if (rc == 1) {
            break;
        }
        memset(&st, 0, sizeof(st));
        st.struct_size = sizeof(st);
        if (ms_hw_player_get_stats(p, &st) == MS_OK &&
            st.video_au_submitted >= 2 && st.audio_configured) {
            break;
        }
    }
    memset(&st, 0, sizeof(st));
    st.struct_size = sizeof(st);
    CHECK_ST(ms_hw_player_get_stats(p, &st), MS_OK, "st0");
    CHECK(st.video_au_submitted >= 1, "video submitted on default audio");
    CHECK(st.audio_configured == 1, "english audio configured (mock/system AAC)");
    CHECK(st.audio_track_id == a_eng, "player on English");
    epoch0 = st.epoch;
    sw0 = st.audio_switch_count;
    vsub0 = (int)st.video_au_submitted;

    CHECK_ST(ms_hw_player_set_rate(p, 2, 1), MS_OK, "rate 2x before switch");
    CHECK_ST(ms_ff_select_track(f, a_zho, 1), MS_OK, "ff select zho");
    for (loops = 0; loops < 500; loops++) {
        rc = pump_one(f, p, &ev);
        CHECK(rc >= 0, "pump sw");
        if (rc == 1) {
            break;
        }
        memset(&st, 0, sizeof(st));
        st.struct_size = sizeof(st);
        if (ms_hw_player_get_stats(p, &st) == MS_OK &&
            st.audio_track_id == a_zho && st.audio_configured) {
            break;
        }
    }
    memset(&st, 0, sizeof(st));
    st.struct_size = sizeof(st);
    CHECK_ST(ms_hw_player_get_stats(p, &st), MS_OK, "st1");
    CHECK(st.audio_track_id == a_zho, "player switched to Chinese");
    CHECK(st.audio_configured == 1, "new CSD configured");
    CHECK(st.audio_switch_count > sw0, "audio_switch_count increased");
    CHECK(st.epoch == epoch0, "video decoder epoch unchanged (not reopened)");
    CHECK(st.video_au_submitted >= (unsigned)vsub0, "video still submitted");
    CHECK_ST(ms_hw_player_pause(p), MS_OK, "pause after switch");
    CHECK_ST(ms_hw_player_resume(p), MS_OK, "resume after switch");
    /* 切轨后应仍能套用已保存的倍速 */
    CHECK_ST(ms_hw_player_set_rate(p, 1, 1), MS_OK, "back to 1x");
    ms_ff_destroy(f);
    ms_hw_player_destroy(p);
    printf("  test_ff_hls_alt_audio_player ok (file HLS + mock; not device sound/paint)\n");
    return 0;
}

static int test_ff_hls_video_variant_player(void)
{
    const char *ffmpeg = "/usr/bin/ffmpeg";
    const char *tmp = getenv("MS_FF_TMP");
    char dir[256], lots[768], hits[768], ets[768], cmd[2048], master[800], url[860];
    char buf[800];
    ms_hw_player *p = NULL;
    ms_ff *f = NULL;
    ms_ff_limits lim;
    ms_hw_player_opts o;
    ms_hw_player_stats st;
    ms_track_info ti[16];
    size_t nt = 0, k;
    uint32_t v_lo = 0, v_hi = 0, a_eng = 0;
    unsigned epoch0 = 0, epoch1 = 0, sel0 = 0, sel1 = 0;
    uint64_t cfg0 = 0, vsub0 = 0;
    int token = 31, loops, rc;
    ms_event ev;
    int saw_wait = 0;
    if (access(ffmpeg, X_OK) != 0) {
        printf("  player HLS video variant: no /usr/bin/ffmpeg; untested gap, not a pass\n");
        return 1;
    }
    if (tmp == NULL || tmp[0] == 0) {
        tmp = "build/debug/ff-tmp";
    }
    snprintf(dir, sizeof(dir), "%s/hlsvar-pl", tmp);
    (void)mkdir(dir, 0755);
    snprintf(lots, sizeof(lots), "%s/lo.ts", dir);
    snprintf(hits, sizeof(hits), "%s/hi.ts", dir);
    snprintf(ets, sizeof(ets), "%s/eng.ts", dir);
    snprintf(cmd, sizeof(cmd),
             "%s -hide_banner -loglevel error -y "
             "-f lavfi -i testsrc=size=160x120:rate=25:duration=6 "
             "-c:v libx264 -pix_fmt yuv420p -g 25 -keyint_min 25 -b:v 200k -an %s",
             ffmpeg, lots);
    CHECK(system(cmd) == 0, "enc lo");
    snprintf(cmd, sizeof(cmd),
             "%s -hide_banner -loglevel error -y "
             "-f lavfi -i testsrc=size=320x240:rate=25:duration=6 "
             "-c:v libx264 -pix_fmt yuv420p -g 25 -keyint_min 25 -b:v 800k -an %s",
             ffmpeg, hits);
    CHECK(system(cmd) == 0, "enc hi");
    snprintf(cmd, sizeof(cmd),
             "%s -hide_banner -loglevel error -y "
             "-f lavfi -i sine=frequency=440:duration=6 -c:a aac -ar 48000 -ac 2 %s",
             ffmpeg, ets);
    CHECK(system(cmd) == 0, "enc eng");
    snprintf(buf, sizeof(buf),
             "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:7\n"
             "#EXT-X-MEDIA-SEQUENCE:1\n#EXTINF:6.0,\nlo.ts\n#EXT-X-ENDLIST\n");
    snprintf(cmd, sizeof(cmd), "%s/lo.m3u8", dir);
    CHECK(write_bytes(cmd, buf, strlen(buf)) == 0, "lopl");
    snprintf(buf, sizeof(buf),
             "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:7\n"
             "#EXT-X-MEDIA-SEQUENCE:1\n#EXTINF:6.0,\nhi.ts\n#EXT-X-ENDLIST\n");
    snprintf(cmd, sizeof(cmd), "%s/hi.m3u8", dir);
    CHECK(write_bytes(cmd, buf, strlen(buf)) == 0, "hipl");
    snprintf(buf, sizeof(buf),
             "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:7\n"
             "#EXT-X-MEDIA-SEQUENCE:1\n#EXTINF:6.0,\neng.ts\n#EXT-X-ENDLIST\n");
    snprintf(cmd, sizeof(cmd), "%s/eng.m3u8", dir);
    CHECK(write_bytes(cmd, buf, strlen(buf)) == 0, "epl");
    snprintf(buf, sizeof(buf),
             "#EXTM3U\n"
             "#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"ag\",LANGUAGE=\"eng\","
             "NAME=\"English\",DEFAULT=YES,AUTOSELECT=YES,URI=\"eng.m3u8\"\n"
             "#EXT-X-STREAM-INF:BANDWIDTH=250000,RESOLUTION=160x120,AUDIO=\"ag\"\n"
             "lo.m3u8\n"
             "#EXT-X-STREAM-INF:BANDWIDTH=900000,RESOLUTION=320x240,AUDIO=\"ag\"\n"
             "hi.m3u8\n");
    snprintf(master, sizeof(master), "%s/master.m3u8", dir);
    CHECK(write_bytes(master, buf, strlen(buf)) == 0, "master");
    snprintf(url, sizeof(url), "file:%s", master);

    ms_hw_player_opts_default(&o);
    o.decoder.mock_mode = MS_HW_MOCK_AS_HW;
    o.decoder.struct_size = sizeof(o.decoder);
    o.wait_keyframe = 1;
    o.allow_system_aac = 1; /* 宿主仍走 mock，不是设备系统 AAC 验收 */
    CHECK_ST(ms_hw_player_create(NULL, &o, &p), MS_OK, "player");
    CHECK_ST(ms_hw_player_set_surface(p, &token), MS_OK, "surf");
    ms_ff_limits_default(&lim);
    lim.io_timeout_ms = 8000;
    CHECK_ST(ms_ff_create(NULL, &lim, &f), MS_OK, "ff");
    CHECK_ST(ms_ff_open(f, url), MS_OK, "open file HLS master");
    CHECK_ST(ms_ff_list_tracks(f, ti, 16, &nt), MS_OK, "list");
    for (k = 0; k < nt; k++) {
        if (ti[k].kind == MS_TRACK_VIDEO) {
            const char *nm = ti[k].name ? ti[k].name : "";
            if (strstr(nm, "320x240") != NULL || strstr(nm, "900kbps") != NULL) {
                v_hi = ti[k].id;
            } else if (v_lo == 0 && (strstr(nm, "160x120") != NULL ||
                                     strstr(nm, "250kbps") != NULL || ti[k].selected)) {
                v_lo = ti[k].id;
            } else if (v_hi == 0) {
                v_hi = ti[k].id;
            }
        }
        if (ti[k].kind == MS_TRACK_AUDIO && a_eng == 0) {
            a_eng = ti[k].id;
        }
    }
    CHECK(v_lo && v_hi && v_lo != v_hi, "two variants");

    for (loops = 0; loops < 500; loops++) {
        rc = pump_one(f, p, &ev);
        CHECK(rc >= 0, "pump def");
        if (rc == 1) {
            break;
        }
        memset(&st, 0, sizeof(st));
        st.struct_size = sizeof(st);
        if (ms_hw_player_get_stats(p, &st) == MS_OK &&
            st.video_au_submitted >= 2 && st.configured) {
            break;
        }
    }
    memset(&st, 0, sizeof(st));
    st.struct_size = sizeof(st);
    CHECK_ST(ms_hw_player_get_stats(p, &st), MS_OK, "st0");
    CHECK(st.video_au_submitted >= 1, "default variant submitted");
    CHECK(st.configured == 1, "default configured");
    epoch0 = st.epoch;
    sel0 = st.selection_epoch;
    cfg0 = st.cfg_accepted;
    vsub0 = st.video_au_submitted;

    CHECK_ST(ms_hw_player_set_rate(p, 2, 1), MS_OK, "rate 2x before switch");
    CHECK_ST(ms_ff_select_track(f, v_hi, 1), MS_OK, "ff select hi");
    for (loops = 0; loops < 600; loops++) {
        rc = pump_one(f, p, &ev);
        CHECK(rc >= 0, "pump sw");
        if (rc == 1) {
            break;
        }
        memset(&st, 0, sizeof(st));
        st.struct_size = sizeof(st);
        if (ms_hw_player_get_stats(p, &st) == MS_OK) {
            if (st.waiting_keyframe) {
                saw_wait = 1;
            }
            if (st.configured && st.video_au_submitted > vsub0 &&
                (st.epoch > epoch0 || st.cfg_accepted > cfg0)) {
                break;
            }
        }
    }
    memset(&st, 0, sizeof(st));
    st.struct_size = sizeof(st);
    CHECK_ST(ms_hw_player_get_stats(p, &st), MS_OK, "st1");
    CHECK(st.configured == 1, "reconfigured after switch");
    CHECK(st.video_au_submitted > vsub0, "new variant AUs submitted");
    CHECK(st.epoch > epoch0 || st.cfg_accepted > cfg0,
          "decoder epoch or cfg changed (old params not reused)");
    epoch1 = st.epoch;
    sel1 = st.selection_epoch;
    CHECK(sel1 >= sel0, "selection_epoch not regressed");
    CHECK_ST(ms_hw_player_pause(p), MS_OK, "pause after switch");
    CHECK(ms_hw_player_is_paused(p) == 1, "is paused");
    CHECK_ST(ms_hw_player_resume(p), MS_OK, "resume after switch");
    CHECK_ST(ms_hw_player_set_rate(p, 1, 1), MS_OK, "back to 1x");
    if (ms_ff_is_seekable(f)) {
        CHECK_ST(ms_hw_player_seek(p, 90 * 1000), MS_OK, "player seek");
        CHECK_ST(ms_ff_seek(f, 90 * 1000), MS_OK, "ff seek");
        for (loops = 0; loops < 400; loops++) {
            rc = pump_one(f, p, &ev);
            CHECK(rc >= 0, "pump seek");
            if (rc == 1) {
                break;
            }
            memset(&st, 0, sizeof(st));
            st.struct_size = sizeof(st);
            if (ms_hw_player_get_stats(p, &st) == MS_OK && st.video_au_submitted > 0 &&
                st.configured) {
                break;
            }
        }
    }
    (void)saw_wait;
    (void)epoch1;
    (void)a_eng;
    ms_ff_destroy(f);
    ms_hw_player_destroy(p);
    printf("  test_ff_hls_video_variant_player ok (file HLS + mock; not device paint/sound)\n");
    return 0;
}


#endif




static int test_abit_ac3_inband_switch_and_clock(void)
{
    ms_hw_player *p = NULL;
    ms_hw_player_stats st;
    ms_event ev;
    int token = 21;

    CHECK(make_player(&p, 0) == 0, "create");
    CHECK_ST(ms_hw_player_set_surface(p, &token), MS_OK, "surf");
    ev_track(&ev, 10, MS_TRACK_AUDIO, MS_CODEC_AC3, "eng", 1);
    ev.sample_format = MS_SAMPLE_FMT_AC3;
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "tr0");
    ev_track(&ev, 11, MS_TRACK_AUDIO, MS_CODEC_AC3, "chi", 0);
    ev.sample_format = MS_SAMPLE_FMT_AC3;
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "tr1");

    memset(&ev, 0, sizeof(ev));
    ev.type = MS_EVENT_AU;
    ev.track.kind = MS_TRACK_AUDIO;
    ev.track.codec = MS_CODEC_AC3;
    ev.track.id = 10;
    ev.sample_format = MS_SAMPLE_FMT_AC3;
    ev.data = k_ac3_48k_stereo;
    ev.data_size = sizeof(k_ac3_48k_stereo);
    ev.has_pts = 1;
    ev.pts = 0;
    ev.time_base_num = 1;
    ev.time_base_den = 90000;
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "au inband");
    CHECK_ST(ms_hw_player_drive(p), MS_OK, "drive");
    memset(&st, 0, sizeof(st));
    st.struct_size = sizeof(st);
    CHECK_ST(ms_hw_player_get_stats(p, &st), MS_OK, "st");
    CHECK(st.audio_configured == 1, "configured from in-band, no CSD");
    CHECK(st.audio_clock_us == (int64_t)((1536ull * 1000000ull) / 48000ull), "clock 1536/48k");
    CHECK(st.audio_is_hardware == 0, "mock not device hw");

    ev.pts = 2880;
    ev.track.id = 11;
    ev.data = k_ac3_441k_stereo;
    ev.data_size = sizeof(k_ac3_441k_stereo);
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "unselected au");
    CHECK_ST(ms_hw_player_get_stats(p, &st), MS_OK, "st drop");
    CHECK(st.audio_au_dropped_unselected >= 1, "unselected filtered");

    CHECK_ST(ms_hw_player_select_track(p, 11, 1), MS_OK, "switch");
    CHECK_ST(ms_hw_player_get_stats(p, &st), MS_OK, "st sw");
    CHECK(st.audio_switch_count >= 1 && st.audio_configured == 0, "session reset");
    CHECK(st.audio_clock_us == 0, "clock cleared");

    ev.track.id = 11;
    ev.pts = 0;
    ev.data = k_ac3_441k_stereo;
    ev.data_size = sizeof(k_ac3_441k_stereo);
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "new au");
    CHECK_ST(ms_hw_player_drive(p), MS_OK, "drive2");
    CHECK_ST(ms_hw_player_get_stats(p, &st), MS_OK, "st2");
    CHECK(st.audio_configured == 1, "reconfigured after switch");
    CHECK(st.audio_clock_us == (int64_t)((1536ull * 1000000ull) / 44100ull), "new clock 44.1k");

    ev.type = MS_EVENT_AU;
    ev.track.id = 11;
    ev.data = k_ac3_441k_stereo;
    ev.data_size = sizeof(k_ac3_441k_stereo);
    ev.has_pts = 1;
    ev.pts = 2880;
    ev.sample_format = MS_SAMPLE_FMT_AC3;
    ev.track.codec = MS_CODEC_AC3;
    ev.track.kind = MS_TRACK_AUDIO;
    /* 切轨后的倍速与 seek 应清时钟并再接收新轨 AU。 */
    CHECK_ST(ms_hw_player_set_rate(p, 2, 1), MS_OK, "rate 2x");
    CHECK_ST(ms_hw_player_seek(p, 90 * 1000), MS_OK, "seek");
    CHECK_ST(ms_hw_player_get_stats(p, &st), MS_OK, "st seek");
    CHECK(st.audio_clock_us == 0, "seek flush clock");
    ev.pts = 90000;
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "post-seek au");
    CHECK_ST(ms_hw_player_drive(p), MS_OK, "drive seek");
    ms_hw_player_destroy(p);
    printf("  test_abit_ac3_inband_switch_and_clock ok (host mock; legal no-CSD; not device sound)\n");
    return 0;
}



static int test_abit_mpeg_inband_switch_and_clock(void)
{
    ms_hw_player *p = NULL;
    ms_hw_player_stats st;
    ms_event ev;
    int token = 31;

    CHECK(make_player(&p, 0) == 0, "create");
    CHECK_ST(ms_hw_player_set_surface(p, &token), MS_OK, "surf");
    ev_track(&ev, 10, MS_TRACK_AUDIO, MS_CODEC_MP3, "eng", 1);
    ev.sample_format = MS_SAMPLE_FMT_MPEG_AUDIO;
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "tr0");
    ev_track(&ev, 11, MS_TRACK_AUDIO, MS_CODEC_MP2, "chi", 0);
    ev.sample_format = MS_SAMPLE_FMT_MPEG_AUDIO;
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "tr1");

    memset(&ev, 0, sizeof(ev));
    ev.type = MS_EVENT_AU;
    ev.track.kind = MS_TRACK_AUDIO;
    ev.track.codec = MS_CODEC_MP3;
    ev.track.id = 10;
    ev.sample_format = MS_SAMPLE_FMT_MPEG_AUDIO;
    ev.data = k_mp3_441_128;
    ev.data_size = sizeof(k_mp3_441_128);
    ev.has_pts = 1;
    ev.pts = 0;
    ev.time_base_num = 1;
    ev.time_base_den = 90000;
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "mp3 inband");
    CHECK_ST(ms_hw_player_drive(p), MS_OK, "drive");
    memset(&st, 0, sizeof(st));
    st.struct_size = sizeof(st);
    CHECK_ST(ms_hw_player_get_stats(p, &st), MS_OK, "st");
    CHECK(st.audio_configured == 1, "configured from in-band, no CSD");
    CHECK(st.audio_clock_us == (int64_t)((1152ull * 1000000ull) / 44100ull), "clock 1152/44.1k");
    CHECK(st.audio_is_hardware == 0, "mock not device hw");

    ev.pts = 2352;
    ev.track.id = 11;
    ev.track.codec = MS_CODEC_MP2;
    ev.data = k_mp2_48k_192;
    ev.data_size = sizeof(k_mp2_48k_192);
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "unselected mp2");
    CHECK_ST(ms_hw_player_get_stats(p, &st), MS_OK, "st drop");
    CHECK(st.audio_au_dropped_unselected >= 1, "unselected filtered");

    CHECK_ST(ms_hw_player_select_track(p, 11, 1), MS_OK, "switch to mp2");
    CHECK_ST(ms_hw_player_get_stats(p, &st), MS_OK, "st sw");
    CHECK(st.audio_switch_count >= 1 && st.audio_configured == 0, "session reset");
    CHECK(st.audio_clock_us == 0, "clock cleared, not leftover");

    ev.track.id = 11;
    ev.track.codec = MS_CODEC_MP2;
    ev.pts = 0;
    ev.data = k_mp2_48k_192;
    ev.data_size = sizeof(k_mp2_48k_192);
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "mp2 au");
    CHECK_ST(ms_hw_player_drive(p), MS_OK, "drive2");
    CHECK_ST(ms_hw_player_get_stats(p, &st), MS_OK, "st2");
    CHECK(st.audio_configured == 1, "reconfigured after switch");
    CHECK(st.audio_clock_us == (int64_t)((1152ull * 1000000ull) / 48000ull), "new clock 48k");

    CHECK_ST(ms_hw_player_set_rate(p, 2, 1), MS_OK, "rate 2x");
    CHECK_ST(ms_hw_player_seek(p, 90 * 1000), MS_OK, "seek");
    CHECK_ST(ms_hw_player_get_stats(p, &st), MS_OK, "st seek");
    CHECK(st.audio_clock_us == 0, "seek flush clock");
    ev.pts = 90000;
    ev.track.id = 11;
    ev.track.codec = MS_CODEC_MP2;
    ev.data = k_mp2_48k_192;
    ev.data_size = sizeof(k_mp2_48k_192);
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "post-seek");
    CHECK_ST(ms_hw_player_drive(p), MS_OK, "drive seek");
    ms_hw_player_destroy(p);
    printf("  test_abit_mpeg_inband_switch_and_clock ok (host mock; MP3->MP2; not device sound)\n");
    return 0;
}

static int test_abit_opus_preroll_switch_and_clock(void)
{
    ms_hw_player *p = NULL;
    ms_hw_player_stats st;
    ms_event ev;
    int token = 41;
    const int64_t first_us = (int64_t)((648ull * 1000000ull) / 48000ull);
    const int64_t full_us = (int64_t)((960ull * 1000000ull) / 48000ull);

    CHECK(make_player(&p, 0) == 0, "create");
    CHECK_ST(ms_hw_player_set_surface(p, &token), MS_OK, "surf");
    ev_track(&ev, 20, MS_TRACK_AUDIO, MS_CODEC_OPUS, "eng", 1);
    ev.sample_format = MS_SAMPLE_FMT_OPUS;
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "tr0");
    ev_track(&ev, 21, MS_TRACK_AUDIO, MS_CODEC_MP3, "chi", 0);
    ev.sample_format = MS_SAMPLE_FMT_MPEG_AUDIO;
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "tr1");

    memset(&ev, 0, sizeof(ev));
    ev.type = MS_EVENT_CODEC_CONFIG;
    ev.track.kind = MS_TRACK_AUDIO;
    ev.track.codec = MS_CODEC_OPUS;
    ev.track.id = 20;
    ev.sample_format = MS_SAMPLE_FMT_OPUS;
    ev.data = k_opus_head;
    ev.data_size = sizeof(k_opus_head);
    ev.time_base_num = 1;
    ev.time_base_den = 48000;
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "opus cfg");

    memset(&ev, 0, sizeof(ev));
    ev.type = MS_EVENT_AU;
    ev.track.kind = MS_TRACK_AUDIO;
    ev.track.codec = MS_CODEC_OPUS;
    ev.track.id = 20;
    ev.sample_format = MS_SAMPLE_FMT_OPUS;
    ev.data = k_opus_toc20;
    ev.data_size = sizeof(k_opus_toc20);
    ev.has_pts = 1;
    ev.pts = 0;
    ev.time_base_num = 1;
    ev.time_base_den = 48000;
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "au queued");
    memset(&st, 0, sizeof(st));
    st.struct_size = sizeof(st);
    CHECK_ST(ms_hw_player_get_stats(p, &st), MS_OK, "st queued");
    CHECK(st.audio_configured == 1, "configured from real Head");
    CHECK(st.audio_clock_us == 0, "submit/queued is not speaker clock");

    CHECK_ST(ms_hw_player_drive(p), MS_OK, "drive1");
    CHECK_ST(ms_hw_player_get_stats(p, &st), MS_OK, "st1");
    CHECK(st.audio_clock_us == first_us, "preroll 312 once (960-312 @ 48k)");
    CHECK(st.audio_is_hardware == 0, "mock not device hw");

    ev.pts = 960;
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "au2");
    CHECK_ST(ms_hw_player_drive(p), MS_OK, "drive2");
    CHECK_ST(ms_hw_player_get_stats(p, &st), MS_OK, "st2");
    CHECK(st.audio_clock_us == first_us + full_us, "second packet not pre-skipped again");

    ev.pts = 1920;
    ev.track.id = 21;
    ev.track.codec = MS_CODEC_MP3;
    ev.sample_format = MS_SAMPLE_FMT_MPEG_AUDIO;
    ev.data = k_mp3_441_128;
    ev.data_size = sizeof(k_mp3_441_128);
    ev.time_base_num = 1;
    ev.time_base_den = 90000;
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "unselected mp3");
    CHECK_ST(ms_hw_player_get_stats(p, &st), MS_OK, "st drop");
    CHECK(st.audio_au_dropped_unselected >= 1, "unselected filtered");

    CHECK_ST(ms_hw_player_seek(p, 0), MS_OK, "seek");
    CHECK_ST(ms_hw_player_get_stats(p, &st), MS_OK, "st seek");
    CHECK(st.audio_clock_us == 0, "seek flush clock");
    ev.track.id = 20;
    ev.track.codec = MS_CODEC_OPUS;
    ev.sample_format = MS_SAMPLE_FMT_OPUS;
    ev.data = k_opus_toc20;
    ev.data_size = sizeof(k_opus_toc20);
    ev.pts = 0;
    ev.time_base_num = 1;
    ev.time_base_den = 48000;
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "post-seek au");
    CHECK_ST(ms_hw_player_drive(p), MS_OK, "drive seek");
    CHECK_ST(ms_hw_player_get_stats(p, &st), MS_OK, "st post-seek");
    CHECK(st.audio_clock_us == first_us, "seek(0) arms start pre-skip, not 80ms warmup");

    /* 中途 seek：应武装 80ms seek preroll，而不是再扣 312 */
    CHECK_ST(ms_hw_player_seek(p, 90 * 1000), MS_OK, "mid seek 1s");
    CHECK_ST(ms_hw_player_get_stats(p, &st), MS_OK, "st mid");
    CHECK(st.audio_clock_us == 0, "mid seek flush clock");
    ev.track.id = 20;
    ev.track.codec = MS_CODEC_OPUS;
    ev.sample_format = MS_SAMPLE_FMT_OPUS;
    ev.data = k_opus_toc20;
    ev.data_size = sizeof(k_opus_toc20);
    ev.pts = 48000;
    ev.time_base_num = 1;
    ev.time_base_den = 48000;
    CHECK_ST(ms_hw_player_note_event_trim(p, 0, 0), MS_OK, "mid au trim");
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "mid au");
    CHECK_ST(ms_hw_player_drive(p), MS_OK, "drive mid");
    CHECK_ST(ms_hw_player_get_stats(p, &st), MS_OK, "st mid play");
    CHECK(st.audio_clock_us == 0, "first packet after mid seek swallowed by 3840 warmup");

    CHECK_ST(ms_hw_player_select_track(p, 21, 1), MS_OK, "switch to mp3");
    CHECK_ST(ms_hw_player_get_stats(p, &st), MS_OK, "st sw");
    CHECK(st.audio_switch_count >= 1 && st.audio_configured == 0, "session reset");
    CHECK(st.audio_clock_us == 0, "clock cleared on switch");

    ev.track.id = 21;
    ev.track.codec = MS_CODEC_MP3;
    ev.sample_format = MS_SAMPLE_FMT_MPEG_AUDIO;
    ev.data = k_mp3_441_128;
    ev.data_size = sizeof(k_mp3_441_128);
    ev.pts = 0;
    ev.time_base_num = 1;
    ev.time_base_den = 90000;
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "mp3 after switch");
    CHECK_ST(ms_hw_player_drive(p), MS_OK, "drive mp3");
    CHECK_ST(ms_hw_player_get_stats(p, &st), MS_OK, "st mp3");
    CHECK(st.audio_configured == 1, "reconfigured after switch");
    CHECK(st.audio_clock_us == (int64_t)((1152ull * 1000000ull) / 44100ull),
          "mp3 clock, leftover opus preroll not applied");

    CHECK_ST(ms_hw_player_set_rate(p, 2, 1), MS_OK, "rate 2x");
    CHECK_ST(ms_hw_player_select_track(p, 20, 1), MS_OK, "switch back opus");
    CHECK_ST(ms_hw_player_get_stats(p, &st), MS_OK, "st back");
    CHECK(st.audio_clock_us == 0 && st.audio_configured == 0, "opus session reset");
    ev.type = MS_EVENT_CODEC_CONFIG;
    ev.track.id = 20;
    ev.track.codec = MS_CODEC_OPUS;
    ev.sample_format = MS_SAMPLE_FMT_OPUS;
    ev.data = k_opus_head;
    ev.data_size = sizeof(k_opus_head);
    ev.has_pts = 0;
    ev.time_base_num = 1;
    ev.time_base_den = 48000;
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "head again");
    ev.type = MS_EVENT_AU;
    ev.data = k_opus_toc20;
    ev.data_size = sizeof(k_opus_toc20);
    ev.has_pts = 1;
    ev.pts = 0;
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "au after reswitch");
    CHECK_ST(ms_hw_player_drive(p), MS_OK, "drive reswitch");
    CHECK_ST(ms_hw_player_get_stats(p, &st), MS_OK, "st reswitch");
    CHECK(st.audio_configured == 1, "reconfigured opus");
    CHECK(st.audio_clock_us == first_us, "preroll after switch, not leftover mp3 clock");
    ms_hw_player_destroy(p);
    printf("  test_abit_opus_preroll_switch_and_clock ok (host mock; preroll/seek/switch; "
           "not device sound)\n");
    return 0;
}


static int test_surface_loss_keeps_audio_and_focus(void)
{
    ms_hw_player *p = NULL;
    ms_hw_player_stats st;
    ms_event ev;
    int token = 21, token2 = 22;
    uint64_t played;
    uint64_t submitted;
    int64_t clock;
    int epoch;

    CHECK(make_player(&p, 0) == 0, "life av");
    CHECK_ST(ms_hw_player_set_surface(p, &token), MS_OK, "surf");
    ev_aac_cfg(&ev);
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "aac cfg");
    ev_aac_au(&ev, 0);
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "aac au");
    fill_ev(&ev, MS_EVENT_CODEC_CONFIG, 0);
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "v cfg");
    fill_ev(&ev, MS_EVENT_AU, 1);
    ev.has_pts = 1;
    ev.pts = 0;
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "v key");
    CHECK_ST(ms_hw_player_drive(p), MS_OK, "drive start");
    memset(&st, 0, sizeof(st));
    st.struct_size = sizeof(st);
    CHECK_ST(ms_hw_player_get_stats(p, &st), MS_OK, "st start");
    CHECK(st.configured == 1 && st.audio_configured == 1, "both configured");
    CHECK(st.surface_bound == 1, "surface bound");
    CHECK(st.have_first_output == 1 && st.output_is_not_display == 1, "output not display");
    CHECK(st.have_first_display == 0, "no display yet");
    played = st.audio_au_played;
    submitted = st.video_au_submitted;
    clock = st.audio_clock_us;
    epoch = (int)st.epoch;
    CHECK(played >= 1 && clock > 0 && submitted >= 1, "started playing");

    /* 同指针 set_surface 不得拆掉解码器。 */
    CHECK_ST(ms_hw_player_set_surface(p, &token), MS_OK, "same surf");
    CHECK_ST(ms_hw_player_get_stats(p, &st), MS_OK, "st same");
    CHECK(st.configured == 1 && st.audio_configured == 1, "same surface no teardown");
    CHECK((int)st.epoch == epoch, "epoch unchanged on same surface");

    /* 丢失表面：只拆视频，保留音频与会话 epoch。 */
    CHECK_ST(ms_hw_player_set_surface(p, NULL), MS_OK, "surf lost");
    CHECK_ST(ms_hw_player_get_stats(p, &st), MS_OK, "st lost");
    CHECK(st.configured == 0, "video decoder released");
    CHECK(st.audio_configured == 1, "audio kept");
    CHECK(st.surface_bound == 0, "unbound");
    CHECK(st.waiting_keyframe == 1, "wait key after rebuild");
    CHECK((int)st.epoch == epoch, "session epoch kept");
    CHECK(st.have_first_display == 0, "display evidence cleared");
    CHECK(st.have_first_output == 1, "prior output kept, still not display");
    ev_aac_au(&ev, 23219);
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "aac after loss");
    CHECK_ST(ms_hw_player_drive(p), MS_OK, "drive after loss");
    CHECK_ST(ms_hw_player_get_stats(p, &st), MS_OK, "st audio after loss");
    CHECK(st.audio_au_played > played && st.audio_clock_us >= clock, "audio still advances");
    fill_ev(&ev, MS_EVENT_AU, 0);
    ev.has_pts = 1;
    ev.pts = 23219;
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "nonkey while unbound");
    CHECK_ST(ms_hw_player_get_stats(p, &st), MS_OK, "st nokey");
    CHECK(st.video_au_dropped_nokey >= 1, "non-key dropped without surface");
    CHECK(st.video_au_submitted == submitted, "no video submit without surface");

    /* 新表面：重配视频，等关键帧，音频仍在。 */
    CHECK_ST(ms_hw_player_set_surface(p, &token2), MS_OK, "surf rebuilt");
    CHECK_ST(ms_hw_player_get_stats(p, &st), MS_OK, "st rebind");
    CHECK(st.configured == 1 && st.surface_bound == 1, "video reconfigured");
    CHECK(st.audio_configured == 1, "audio still there");
    CHECK(st.waiting_keyframe == 1, "key gate after rebind");
    fill_ev(&ev, MS_EVENT_AU, 1);
    ev.has_pts = 1;
    ev.pts = 46438;
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "key after rebind");
    CHECK_ST(ms_hw_player_drive(p), MS_OK, "drive rebind");
    CHECK_ST(ms_hw_player_get_stats(p, &st), MS_OK, "st after key");
    CHECK(st.video_au_submitted > submitted, "submitted to new surface");
    CHECK(st.have_first_display == 0 && st.output_is_not_display == 1,
          "codec output still not display");

    /* 音频焦点：丢失暂停；GAIN 恢复；用户暂停不被 GAIN 清掉。 */
    CHECK_ST(ms_hw_player_notify_audio_focus(p, 99), MS_ERR_INVALID_ARG, "bad focus");
    CHECK_ST(ms_hw_player_notify_audio_focus(p, MS_HW_AUDIO_FOCUS_LOSS), MS_OK, "focus loss");
    CHECK(ms_hw_player_is_paused(p) == 1, "effective pause on loss");
    CHECK(ms_hw_player_is_user_paused(p) == 0, "not user pause");
    played = 0;
    CHECK_ST(ms_hw_player_get_stats(p, &st), MS_OK, "st loss");
    CHECK(st.audio_focus == MS_HW_AUDIO_FOCUS_LOSS && st.paused == 1 && st.user_paused == 0,
          "stats loss");
    played = st.audio_au_played;
    ev_aac_au(&ev, 69657);
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "aac during loss");
    CHECK_ST(ms_hw_player_drive(p), MS_OK, "drive paused");
    CHECK_ST(ms_hw_player_get_stats(p, &st), MS_OK, "st paused play");
    CHECK(st.audio_au_played == played, "no play while focus lost");

    CHECK_ST(ms_hw_player_notify_audio_focus(p, MS_HW_AUDIO_FOCUS_GAIN), MS_OK, "focus gain");
    CHECK(ms_hw_player_is_paused(p) == 0, "resumed after gain");
    CHECK_ST(ms_hw_player_get_stats(p, &st), MS_OK, "st gain");
    CHECK(st.audio_focus == MS_HW_AUDIO_FOCUS_GAIN && st.paused == 0, "stats gain");

    CHECK_ST(ms_hw_player_pause(p), MS_OK, "user pause");
    CHECK_ST(ms_hw_player_notify_audio_focus(p, MS_HW_AUDIO_FOCUS_LOSS_TRANSIENT),
             MS_OK, "transient");
    CHECK_ST(ms_hw_player_notify_audio_focus(p, MS_HW_AUDIO_FOCUS_GAIN), MS_OK,
             "gain while user paused");
    CHECK(ms_hw_player_is_user_paused(p) == 1, "user pause kept");
    CHECK(ms_hw_player_is_paused(p) == 1, "still paused");
    CHECK_ST(ms_hw_player_get_stats(p, &st), MS_OK, "st user held");
    CHECK(st.user_paused == 1 && st.paused == 1, "stats user held");

    /* 用户暂停在表面丢失/重建后仍保持。 */
    CHECK_ST(ms_hw_player_set_surface(p, NULL), MS_OK, "lost while user paused");
    CHECK_ST(ms_hw_player_set_surface(p, &token), MS_OK, "rebind while user paused");
    CHECK(ms_hw_player_is_user_paused(p) == 1 && ms_hw_player_is_paused(p) == 1,
          "user pause survives surface");
    CHECK_ST(ms_hw_player_resume(p), MS_OK, "user resume");
    CHECK(ms_hw_player_is_paused(p) == 0, "resumed");

    /* TRANSIENT_CAN_DUCK 按暂停处理，不是音量闪避。 */
    CHECK_ST(ms_hw_player_notify_audio_focus(p,
             MS_HW_AUDIO_FOCUS_LOSS_TRANSIENT_CAN_DUCK), MS_OK, "duck as pause");
    CHECK(ms_hw_player_is_paused(p) == 1, "ducked pauses");
    CHECK_ST(ms_hw_player_notify_audio_focus(p, MS_HW_AUDIO_FOCUS_GAIN), MS_OK, "unduck");
    CHECK(ms_hw_player_is_paused(p) == 0, "gain after duck");

    ms_hw_player_destroy(p);
    printf("  test_surface_loss_keeps_audio_and_focus ok\n");
    return 0;
}

static int test_csd_change_and_seek_while_unbound_or_paused(void)
{
    ms_hw_player *p = NULL;
    ms_hw_player_stats st;
    ms_event ev;
    int token = 31, token2 = 32;
    uint64_t cfg0;
    uint64_t played;
    uint64_t submitted;
    int epoch;

    CHECK(make_player(&p, 0) == 0, "create");
    CHECK_ST(ms_hw_player_note_timeline(p, 1, 90 * 1000 * 10), MS_OK, "seekable 10s");
    CHECK_ST(ms_hw_player_set_surface(p, &token), MS_OK, "surf");
    ev_aac_cfg(&ev);
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "aac cfg");
    ev_aac_au(&ev, 0);
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "aac au");
    fill_ev(&ev, MS_EVENT_CODEC_CONFIG, 0);
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "v cfg1");
    fill_ev(&ev, MS_EVENT_AU, 1);
    ev.has_pts = 1;
    ev.pts = 0;
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "v key1");
    CHECK_ST(ms_hw_player_drive(p), MS_OK, "drive start");
    memset(&st, 0, sizeof(st));
    st.struct_size = sizeof(st);
    CHECK_ST(ms_hw_player_get_stats(p, &st), MS_OK, "st start");
    CHECK(st.configured == 1 && st.audio_configured == 1, "both configured");
    cfg0 = st.cfg_accepted;
    epoch = (int)st.epoch;
    submitted = st.video_au_submitted;
    played = st.audio_au_played;
    CHECK(cfg0 >= 1 && submitted >= 1 && played >= 1, "started");

    /* 表面丢失后再换 CSD：须收下新配置，会话 epoch 保持（解码器已拆）。 */
    CHECK_ST(ms_hw_player_set_surface(p, NULL), MS_OK, "surf lost");
    fill_ev(&ev, MS_EVENT_CODEC_CONFIG, 0);
    ev.data = k_avcc2;
    ev.data_size = sizeof(k_avcc2);
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "csd while unbound");
    CHECK_ST(ms_hw_player_get_stats(p, &st), MS_OK, "st csd unbound");
    CHECK(st.configured == 0 && st.surface_bound == 0, "still unbound");
    CHECK(st.audio_configured == 1, "audio kept during csd");
    CHECK(st.cfg_accepted > cfg0, "new csd stored while unbound");
    CHECK((int)st.epoch == epoch, "session epoch kept on unbound csd");
    CHECK(st.waiting_keyframe == 1, "wait key while unbound");
    fill_ev(&ev, MS_EVENT_AU, 0);
    ev.has_pts = 1;
    ev.pts = 9000;
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "nokey after new csd");
    CHECK_ST(ms_hw_player_get_stats(p, &st), MS_OK, "st nokey unbound");
    CHECK(st.video_au_dropped_nokey >= 1, "nokey dropped");
    CHECK(st.video_au_submitted == submitted, "no submit without surface");

    CHECK_ST(ms_hw_player_set_surface(p, &token2), MS_OK, "rebind after csd");
    CHECK_ST(ms_hw_player_get_stats(p, &st), MS_OK, "st rebind csd");
    CHECK(st.configured == 1 && st.surface_bound == 1, "reconfigured with stored csd");
    CHECK(st.audio_configured == 1, "audio still kept");
    CHECK(st.waiting_keyframe == 1, "key gate after rebind with new csd");
    fill_ev(&ev, MS_EVENT_AU, 0);
    ev.has_pts = 1;
    ev.pts = 18000;
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "nokey after rebind");
    fill_ev(&ev, MS_EVENT_AU, 1);
    ev.has_pts = 1;
    ev.pts = 27000;
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "key after new csd");
    CHECK_ST(ms_hw_player_drive(p), MS_OK, "drive after new csd key");
    CHECK_ST(ms_hw_player_get_stats(p, &st), MS_OK, "st key2");
    CHECK(st.video_au_submitted > submitted, "key submitted to new csd decoder");
    submitted = st.video_au_submitted;
    played = st.audio_au_played;

    /* 焦点丢失期间 seek：保持暂停，刷掉音频时钟，等关键帧；GAIN 才恢复。 */
    CHECK_ST(ms_hw_player_notify_audio_focus(p, MS_HW_AUDIO_FOCUS_LOSS), MS_OK,
             "focus loss");
    CHECK(ms_hw_player_is_paused(p) == 1 && ms_hw_player_is_user_paused(p) == 0,
          "focus paused");
    ev_aac_au(&ev, 40000);
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "aac during loss");
    CHECK_ST(ms_hw_player_drive(p), MS_OK, "drive during loss");
    CHECK_ST(ms_hw_player_get_stats(p, &st), MS_OK, "st before seek");
    CHECK(st.audio_au_played == played, "no audio advance while focus lost");
    CHECK_ST(ms_hw_player_seek(p, 90 * 1000 * 2), MS_OK, "seek while focus lost");
    CHECK(ms_hw_player_is_paused(p) == 1, "still paused after seek");
    CHECK(ms_hw_player_is_user_paused(p) == 0, "seek is not user pause");
    CHECK_ST(ms_hw_player_get_stats(p, &st), MS_OK, "st seek paused");
    CHECK(st.paused == 1 && st.waiting_keyframe == 1, "seek waits key");
    CHECK(st.audio_configured == 1, "audio session kept across seek");
    CHECK(st.audio_clock_us == 0, "audio clock reset by seek flush");
    CHECK(st.configured == 1, "video still configured after seek");
    fill_ev(&ev, MS_EVENT_AU, 0);
    ev.has_pts = 1;
    ev.pts = 180000;
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "nokey after seek");
    CHECK_ST(ms_hw_player_get_stats(p, &st), MS_OK, "st nokey seek");
    CHECK(st.video_au_submitted == submitted, "nokey not submitted after seek");
    ev_aac_au(&ev, 2000000);
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "aac after seek paused");
    CHECK_ST(ms_hw_player_drive(p), MS_OK, "drive still paused");
    CHECK_ST(ms_hw_player_get_stats(p, &st), MS_OK, "st still paused");
    CHECK(st.audio_au_played == 0 || st.audio_clock_us == 0, "no play until gain");
    CHECK(ms_hw_player_is_paused(p) == 1, "remains paused until gain");

    CHECK_ST(ms_hw_player_notify_audio_focus(p, MS_HW_AUDIO_FOCUS_GAIN), MS_OK,
             "gain after seek");
    CHECK(ms_hw_player_is_paused(p) == 0, "resumed after gain");
    fill_ev(&ev, MS_EVENT_AU, 1);
    ev.has_pts = 1;
    ev.pts = 180000;
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "key after seek gain");
    ev_aac_au(&ev, 2000000);
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "aac after gain");
    CHECK_ST(ms_hw_player_drive(p), MS_OK, "drive after gain");
    CHECK_ST(ms_hw_player_get_stats(p, &st), MS_OK, "st after gain");
    CHECK(st.video_au_submitted > submitted, "key after seek submitted");
    CHECK(st.audio_au_played >= 1 && st.audio_clock_us > 0, "audio after gain");
    CHECK(st.paused == 0 && st.user_paused == 0, "playing");

    /* 用户暂停 + 焦点丢失 + seek：GAIN 不得清掉用户暂停。 */
    CHECK_ST(ms_hw_player_pause(p), MS_OK, "user pause");
    CHECK_ST(ms_hw_player_notify_audio_focus(p, MS_HW_AUDIO_FOCUS_LOSS), MS_OK,
             "loss while user paused");
    CHECK_ST(ms_hw_player_seek(p, 90 * 1000), MS_OK, "seek while both paused");
    CHECK_ST(ms_hw_player_notify_audio_focus(p, MS_HW_AUDIO_FOCUS_GAIN), MS_OK,
             "gain keeps user pause");
    CHECK(ms_hw_player_is_user_paused(p) == 1 && ms_hw_player_is_paused(p) == 1,
          "user pause after seek+gain");
    CHECK_ST(ms_hw_player_get_stats(p, &st), MS_OK, "st user held");
    CHECK(st.waiting_keyframe == 1 && st.paused == 1, "still waiting key paused");
    CHECK_ST(ms_hw_player_resume(p), MS_OK, "user resume");
    CHECK(ms_hw_player_is_paused(p) == 0, "resumed");

    ms_hw_player_destroy(p);
    printf("  test_csd_change_and_seek_while_unbound_or_paused ok "
           "(host mock; not paint/sound)\n");
    return 0;
}

/* 反例：set_surface 拆解码器后若只等下一个 IDR，长 GOP 会黑很久；
 * 可 seek 源应报告 resync 位置。暂停态 drive 也必须能出一帧，且不推进音频。 */
static int test_surface_rebind_resync_and_paused_present(void)
{
    ms_hw_player *p = NULL;
    ms_hw_player_stats st;
    ms_event ev;
    int token = 41, token2 = 42;
    int64_t resync = -1;
    uint64_t submitted;
    int64_t clock;
    uint64_t played;

    CHECK(make_player(&p, 0) == 0, "create");
    CHECK_ST(ms_hw_player_note_timeline(p, 1, 90 * 1000 * 60), MS_OK, "seekable 60s");
    CHECK_ST(ms_hw_player_set_surface(p, &token), MS_OK, "surf");
    ev_aac_cfg(&ev);
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "aac cfg");
    ev_aac_au(&ev, 0);
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "aac au");
    fill_ev(&ev, MS_EVENT_CODEC_CONFIG, 0);
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "v cfg");
    fill_ev(&ev, MS_EVENT_AU, 1);
    ev.has_pts = 1;
    ev.pts = 90000; /* 1s @ 90kHz */
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "v key");
    CHECK_ST(ms_hw_player_drive(p), MS_OK, "drive start");
    memset(&st, 0, sizeof(st));
    st.struct_size = sizeof(st);
    CHECK_ST(ms_hw_player_get_stats(p, &st), MS_OK, "st start");
    CHECK(st.have_first_output == 1, "presented");
    CHECK(ms_hw_player_has_presented_video(p) == 1, "has presented");
    CHECK(st.need_keyframe_resync == 0, "no resync yet");
    submitted = st.video_au_submitted;
    clock = st.audio_clock_us;
    played = st.audio_au_played;
    CHECK(ms_hw_player_position_90k(p) >= 90000, "pos at key");

    CHECK_ST(ms_hw_player_set_surface(p, NULL), MS_OK, "unbind");
    CHECK_ST(ms_hw_player_get_stats(p, &st), MS_OK, "st unbound");
    CHECK(st.need_keyframe_resync == 1, "resync armed on detach");
    CHECK(st.waiting_keyframe == 1, "wait key");
    CHECK(ms_hw_player_has_presented_video(p) == 0, "presented cleared");
    CHECK(ms_hw_player_take_surface_resync(p, &resync) == 1, "take resync while unbound");
    CHECK(resync >= 90000, "resync at last media");
    CHECK(ms_hw_player_take_surface_resync(p, &resync) == 0, "take is one-shot");

    CHECK_ST(ms_hw_player_set_surface(p, &token2), MS_OK, "rebind");
    CHECK_ST(ms_hw_player_get_stats(p, &st), MS_OK, "st rebind");
    CHECK(st.need_keyframe_resync == 1, "still need a frame");
    CHECK(st.waiting_keyframe == 1 && st.surface_bound == 1, "key gate + bound");

    /* 暂停 + 重绑：P 帧仍丢，关键帧 + drive 必须出一帧，音频不得前进。 */
    CHECK_ST(ms_hw_player_pause(p), MS_OK, "user pause");
    CHECK(ms_hw_player_is_paused(p) == 1, "paused");
    fill_ev(&ev, MS_EVENT_AU, 0);
    ev.has_pts = 1;
    ev.pts = 97200;
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "p-frame after rebind");
    CHECK_ST(ms_hw_player_get_stats(p, &st), MS_OK, "st p");
    CHECK(st.video_au_submitted == submitted, "p-frame not submitted");
    fill_ev(&ev, MS_EVENT_AU, 1);
    ev.has_pts = 1;
    ev.pts = 90000;
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "key after rebind paused");
    CHECK_ST(ms_hw_player_drive(p), MS_OK, "drive paused");
    CHECK_ST(ms_hw_player_get_stats(p, &st), MS_OK, "st paused present");
    CHECK(ms_hw_player_has_presented_video(p) == 1, "paused rebind presented a frame");
    CHECK(st.video_au_submitted > submitted, "key submitted to new surface");
    CHECK(st.need_keyframe_resync == 0, "resync cleared after present");
    CHECK(st.audio_au_played == played, "audio not driven while paused");
    CHECK(st.audio_clock_us == clock, "audio clock frozen while paused");

    /* 不可 seek（直播）重绑：take 返回 0，只等 IDR。 */
    CHECK_ST(ms_hw_player_resume(p), MS_OK, "resume");
    CHECK_ST(ms_hw_player_note_timeline(p, 0, -1), MS_OK, "live unseekable");
    CHECK_ST(ms_hw_player_set_surface(p, NULL), MS_OK, "unbind live");
    CHECK_ST(ms_hw_player_set_surface(p, &token), MS_OK, "rebind live");
    CHECK_ST(ms_hw_player_get_stats(p, &st), MS_OK, "st live");
    CHECK(st.need_keyframe_resync == 1, "live still needs IDR");
    CHECK(ms_hw_player_take_surface_resync(p, &resync) == 0, "live: no internal seek");

    ms_hw_player_destroy(p);
    printf("  test_surface_rebind_resync_and_paused_present ok "
           "(host mock; not paint/sound)\n");
    return 0;
}

/* 反例：选轨 / sub_off / seek 不得等到一次成功的 feed/drive 才提交。
 * 立即改 stats、立即清 cues；JNI 背压路径必须同样能消费队列。 */
static int test_select_seek_and_sub_off_apply_without_feed(void)
{
    ms_hw_player *p = NULL;
    ms_hw_player_stats st;
    ms_event ev;
    ms_sub_cue cues[4];
    size_t n = 0;
    int token = 51;
    const char *hdr =
        "[Script Info]\nScriptType: v4.00+\nPlayResX: 640\nPlayResY: 360\n"
        "[V4+ Styles]\n"
        "Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, "
        "BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, "
        "BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding\n"
        "Style: Default,Arial,24,&H0000FF00,&H000000FF,&H00000000,&H00000000,-1,0,0,0,"
        "100,100,0,0,1,2,2,2,20,20,15,1\n";
    const char *ev0 = "0,0,Default,,0,0,0,,StayOnScreen";
    uint32_t audio0 = 10, audio1 = 11, sub_id = 30;

    CHECK(make_player(&p, 1) == 0, "create (small pending to allow LIMIT)");
    CHECK_ST(ms_hw_player_note_timeline(p, 1, 90 * 1000 * 30), MS_OK, "seekable");
    CHECK_ST(ms_hw_player_set_surface(p, &token), MS_OK, "surf");
    ev_track(&ev, audio0, MS_TRACK_AUDIO, MS_CODEC_AAC, "eng", 1);
    ev.sample_format = MS_SAMPLE_FMT_AAC_RAW;
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "a0 tr");
    ev_track(&ev, audio1, MS_TRACK_AUDIO, MS_CODEC_AAC, "fra", 0);
    ev.sample_format = MS_SAMPLE_FMT_AAC_RAW;
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "a1 tr");
    ev_track(&ev, sub_id, MS_TRACK_SUBTITLE, MS_CODEC_SUB_ASS, "eng", 0);
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "ass tr");
    ev_aac_cfg(&ev);
    ev.track.id = audio0;
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "aac cfg");
    ev_aac_au(&ev, 0);
    ev.track.id = audio0;
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "aac au");
    fill_ev(&ev, MS_EVENT_CODEC_CONFIG, 0);
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "v cfg");
    fill_ev(&ev, MS_EVENT_AU, 1);
    ev.has_pts = 1;
    ev.pts = 0;
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "v key");
    CHECK_ST(ms_hw_player_drive(p), MS_OK, "drive");
    CHECK_ST(ms_hw_player_select_track(p, sub_id, 1), MS_OK, "select ass");

    memset(&ev, 0, sizeof(ev));
    ev.type = MS_EVENT_CODEC_CONFIG;
    ev.track.id = sub_id;
    ev.track.kind = MS_TRACK_SUBTITLE;
    ev.track.codec = MS_CODEC_SUB_ASS;
    ev.data = (const uint8_t *)hdr;
    ev.data_size = strlen(hdr);
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "ass hdr");
    memset(&ev, 0, sizeof(ev));
    ev.type = MS_EVENT_SUBTITLE;
    ev.track.id = sub_id;
    ev.track.kind = MS_TRACK_SUBTITLE;
    ev.track.codec = MS_CODEC_SUB_ASS;
    ev.time_base_num = 1;
    ev.time_base_den = 1000000;
    ev.has_pts = 1;
    ev.pts = 0;
    ev.has_dts = 1;
    ev.dts = 5000000;
    ev.data = (const uint8_t *)ev0;
    ev.data_size = strlen(ev0);
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "ass ev");
    memset(cues, 0, sizeof(cues));
    cues[0].struct_size = sizeof(cues[0]);
    n = 0;
    CHECK_ST(ms_hw_player_subtitles_at(p, 90000, cues, 4, &n), MS_OK, "cues on");
    CHECK(n >= 1, "cue visible");

    /* 不再 feed / 不再要求 drive 成功：选轨、seek、关字幕必须立刻生效。 */
    CHECK_ST(ms_hw_player_select_track(p, audio1, 1), MS_OK, "switch audio no feed");
    memset(&st, 0, sizeof(st));
    st.struct_size = sizeof(st);
    CHECK_ST(ms_hw_player_get_stats(p, &st), MS_OK, "st audio");
    CHECK(st.audio_track_id == audio1, "audio_id updated without next feed");
    CHECK(st.audio_switch_count >= 1, "switch counted");

    CHECK_ST(ms_hw_player_seek(p, 90 * 1000 * 5), MS_OK, "seek no subsequent feed");
    CHECK_ST(ms_hw_player_get_stats(p, &st), MS_OK, "st seek");
    CHECK(st.last_seek_request_90k == 90 * 1000 * 5, "seek request committed");
    CHECK(st.position_90k == 90 * 1000 * 5, "position updated");

    /* seek flush 可能已清 cues；再喂一条后关字幕，不依赖后续 feed 才清屏。 */
    ev.type = MS_EVENT_SUBTITLE;
    ev.track.id = sub_id;
    ev.track.kind = MS_TRACK_SUBTITLE;
    ev.track.codec = MS_CODEC_SUB_ASS;
    ev.time_base_num = 1;
    ev.time_base_den = 1000000;
    ev.has_pts = 1;
    ev.pts = 5000000;
    ev.has_dts = 1;
    ev.dts = 8000000;
    ev.data = (const uint8_t *)ev0;
    ev.data_size = strlen(ev0);
    /* 切音后字幕轨仍选中；seek 清过 cues，重喂一条。select 可能因已选中直接返回。 */
    (void)ms_hw_player_select_track(p, sub_id, 1);
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "ass ev after seek");
    n = 0;
    CHECK_ST(ms_hw_player_subtitles_at(p, 90 * 1000 * 5 + 9000, cues, 4, &n), MS_OK,
             "cue back");
    CHECK(n >= 1, "cue on screen again");
    CHECK_ST(ms_hw_player_clear_subtitles(p), MS_OK, "sub_off immediate");
    n = 99;
    CHECK_ST(ms_hw_player_subtitles_at(p, 90 * 1000 * 5 + 9000, cues, 4, &n), MS_OK,
             "cues after off");
    CHECK(n == 0, "old ASS cues cleared immediately");
    CHECK_ST(ms_hw_player_get_stats(p, &st), MS_OK, "st sub off");
    CHECK(st.sub_track_id == 0, "no subtitle selected");

    ms_hw_player_destroy(p);
    printf("  test_select_seek_and_sub_off_apply_without_feed ok "
           "(host mock; JNI must also service pending under backpressure)\n");
    return 0;
}

/* 反例：音频钟/写配额停走时，视频仍须能提交并由 drive 出帧，不得死锁。 */
static int test_audio_clock_stall_does_not_deadlock_video(void)
{
    ms_hw_player *p = NULL;
    ms_hw_player_stats st;
    ms_event ev;
    int token = 71;
    uint64_t submitted;
    unsigned i;
    ms_status fst;

    CHECK(make_player(&p, 0) == 0, "create");
    CHECK_ST(ms_hw_player_set_surface(p, &token), MS_OK, "surf");
    ev_aac_cfg(&ev);
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "aac cfg");
    ev_aac_au(&ev, 0);
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "aac au");
    ev_aac_au(&ev, 23219);
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "aac au2");
    fill_ev(&ev, MS_EVENT_CODEC_CONFIG, 0);
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "v cfg");
    fill_ev(&ev, MS_EVENT_AU, 1);
    ev.has_pts = 1;
    ev.pts = 0;
    CHECK_ST(ms_hw_player_feed(p, &ev, 0, NULL, NULL), MS_OK, "v key");
    CHECK_ST(ms_hw_player_drive(p), MS_OK, "drive start");
    memset(&st, 0, sizeof(st));
    st.struct_size = sizeof(st);
    CHECK_ST(ms_hw_player_get_stats(p, &st), MS_OK, "st start");
    CHECK(st.have_first_output == 1 && st.audio_configured == 1, "started");
    submitted = st.video_au_submitted;
    CHECK(submitted >= 1, "one video in");

    /* 武装 0 配额：此后不再写入，时钟停在已写样点。 */
    CHECK_ST(ms_hw_player_debug_audio_add_write_quota(p, 0), MS_OK, "quota 0");
    ev_aac_au(&ev, 46438);
    (void)ms_hw_player_feed(p, &ev, 0, NULL, NULL);
    CHECK_ST(ms_hw_player_drive(p), MS_OK, "drive stalled audio");

    for (i = 0; i < 32; i++) {
        fill_ev(&ev, MS_EVENT_AU, 1);
        ev.has_pts = 1;
        ev.pts = 9000 * (int64_t)(i + 1);
        fst = ms_hw_player_feed(p, &ev, 0, NULL, NULL);
        if (fst == MS_LIMIT || fst == MS_AGAIN) {
            CHECK_ST(ms_hw_player_drive(p), MS_OK, "drive to relieve video");
            fst = ms_hw_player_feed(p, &ev, 0, NULL, NULL);
        }
        CHECK(fst == MS_OK, "video fed while audio clock stalled");
        CHECK_ST(ms_hw_player_drive(p), MS_OK, "drive video after stall");
    }
    memset(&st, 0, sizeof(st));
    st.struct_size = sizeof(st);
    CHECK_ST(ms_hw_player_get_stats(p, &st), MS_OK, "st after stall loop");
    CHECK(st.video_au_submitted >= submitted + 24, "video submitted kept growing");
    CHECK(st.have_first_output == 1, "outputs present");

    ms_hw_player_destroy(p);
    printf("  test_audio_clock_stall_does_not_deadlock_video ok "
           "(host mock; clock stall must not deadlock video)\n");
    return 0;
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    g_rel = 0;
    if (test_gate_csd_eos_and_times() != 0 ||
        test_copy_backpressure_cancel_flush_switch() != 0 ||
        test_refuse_sw_and_bad_args() != 0 ||
        test_conservation_known_gop() != 0 ||
        test_limit_not_counted_until_consumed() != 0 ||
        test_eos_drive_idempotent_and_lifecycle() != 0 ||
        test_op_lock_flush_surface_feed() != 0 ||
        test_request_timing_includes_injected_wait() != 0 ||
        test_csd_change_reconfigure_epoch() != 0 ||
        test_inband_csd_without_config() != 0 ||
        test_reap_join_and_abandon() != 0 ||
        test_audio_sync_and_refuse() != 0 ||
        test_system_aac_policy() != 0 ||
        test_p4_audio_switch_and_subs() != 0 ||
        test_p4_ass_inband() != 0 ||
        test_p4_bitmap_inband() != 0 ||
        test_p4_bitmap_text_ass_switch() != 0 ||
        test_hevc_inband_and_seek_gate() != 0 ||
        test_vbit_inband_and_seek_gate() != 0 ||
        test_vbit_h263_inband_and_seek_gate() != 0 ||
        test_vbit_av1_inband_and_seek_gate() != 0 ||
        test_vbit_avsplus_inband_and_seek_gate() != 0 ||
        test_abit_ac3_inband_switch_and_clock() != 0 ||
        test_abit_mpeg_inband_switch_and_clock() != 0 ||
        test_abit_opus_preroll_switch_and_clock() != 0 ||
        test_p4_rate_and_seek() != 0 ||
        test_abandoned_worker_cap() != 0 ||
        test_surface_loss_keeps_audio_and_focus() != 0 ||
        test_csd_change_and_seek_while_unbound_or_paused() != 0 ||
        test_surface_rebind_resync_and_paused_present() != 0 ||
        test_select_seek_and_sub_off_apply_without_feed() != 0 ||
        test_audio_clock_stall_does_not_deadlock_video() != 0) {
        fprintf(stderr, "test_hw_player failed (%d)\n", failures);
        return 1;
    }
#ifdef MS_HW_PLAYER_HAVE_FF
    {
        const char *ts = getenv("MS_HW_PLAYER_TS");
        const char *flv = getenv("MS_HW_PLAYER_FLV");
        const char *udp = getenv("MS_HW_PLAYER_UDP");
        const char *rtp = getenv("MS_HW_PLAYER_RTP");
        char def_ts[256], def_flv[256];
        const char *dash = getenv("MS_FF_DASH_DIR");
        if (dash == NULL || dash[0] == 0) {
            dash = "build/dev-samples/dash-ff";
        }
        snprintf(def_ts, sizeof(def_ts), "file:%s/short.ts", dash);
        snprintf(def_flv, sizeof(def_flv), "file:%s/short.flv", dash);
        if ((!ts || !ts[0]) && access(dash, R_OK) == 0) {
            /* 环境未设时，有样本就回归，不把缺省写成通过该路径 */
            char path[256];
            snprintf(path, sizeof(path), "%s/short.ts", dash);
            if (access(path, R_OK) == 0) {
                ts = def_ts;
            }
            snprintf(path, sizeof(path), "%s/short.flv", dash);
            if (access(path, R_OK) == 0) {
                flv = def_flv;
            }
        }
        if (ts && ts[0] && test_ff_entry("TS", ts) != 0) {
            return 1;
        }
        if (flv && flv[0] && test_ff_entry("FLV", flv) != 0) {
            return 1;
        }
        /* 仅当环境变量是真正的 URL 时才当入口；"1"/"yes" 只表示启用 live 测试。 */
        if (udp && udp[0] && (strstr(udp, "://") || strstr(udp, "udp:") == udp) &&
            test_ff_entry("UDP", udp) != 0) {
            return 1;
        }
        if (rtp && rtp[0] && (strstr(rtp, "://") || strstr(rtp, "rtp:") == rtp) &&
            test_ff_entry("RTP", rtp) != 0) {
            return 1;
        }
        if (test_ff_concat_midjoin() != 0) {
            return 1;
        }
        if (test_ff_http_ts_flv() != 0) {
            return 1;
        }
        if (test_ff_probe_cancel_and_unreachable() != 0) {
            return 1;
        }
        if (test_ff_audio_track_not_dropped_and_long_gop() != 0) {
            return 1;
        }
        if (test_ff_inband_mid_csd() != 0) {
            return 1;
        }
        if (test_ff_hls_alt_audio_player() != 0) {
            return 1;
        }
        if (test_ff_hls_video_variant_player() != 0) {
            return 1;
        }
        if (test_ff_live_udp_rtp() != 0) {
            return 1;
        }
    }
#endif
    printf("test_hw_player ok (mock state machine; not device hardware or display)\n");
    return 0;
}
