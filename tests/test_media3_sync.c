/*
 * Media3 音画同步移植的宿主对照（可注入时钟，不依赖设备）。
 * 核对阈值、getFrameReleaseAction 分支、VSYNC 滞回、maybeDropBuffersToKeyframe
 * 「0 跳过不 flush」、DefaultAudioSink 未初始化返回 TIME_UNSET。
 * by: Long <mycjzb@gmail.com>
 */
#include "ms_media3.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
static int64_t g_now_ns;

static int64_t test_nano(void) { return g_now_ns; }
static int64_t test_elapsed_ms(void) { return g_now_ns / 1000000ll; }

static void set_now_us(int64_t us) { g_now_ns = us * 1000ll; }

static void ck(const char *name, int ok)
{
    if (ok) {
        printf("  PASS %s\n", name);
    } else {
        printf("  FAIL %s\n", name);
        fails++;
    }
}

/* ---- 假 AudioTrack ---- */
typedef struct fake_track {
    int play_state;
    uint32_t head;
    int rate;
    int ts_ok;
    int64_t ts_frames;
    int64_t ts_nano;
    int latency_ms;
} fake_track;

static int ft_play(void *c) { return ((fake_track *)c)->play_state; }
static uint32_t ft_head(void *c) { return ((fake_track *)c)->head; }
static int ft_rate(void *c) { return ((fake_track *)c)->rate; }
static int ft_ts(void *c, int64_t *f, int64_t *n)
{
    fake_track *t = (fake_track *)c;
    if (!t->ts_ok) {
        return 0;
    }
    *f = t->ts_frames;
    *n = t->ts_nano;
    return 1;
}
static int ft_lat(void *c, int *ms)
{
    *ms = ((fake_track *)c)->latency_ms;
    return 0;
}
static AudioTrackOps ft_ops(fake_track *t)
{
    AudioTrackOps o;
    memset(&o, 0, sizeof(o));
    o.ctx = t;
    o.getPlayState = ft_play;
    o.getPlaybackHeadPosition = ft_head;
    o.getSampleRate = ft_rate;
    o.getTimestamp = ft_ts;
    o.getLatencyMs = ft_lat;
    return o;
}

static int g_skip_count;
static int maybe_drop_from_count(void *ctx, int64_t pos, int treat)
{
    (void)ctx;
    (void)pos;
    (void)treat;
    return g_skip_count > 0;
}

static void test_audio_sink(void)
{
    fake_track ft;
    DefaultAudioSink sink;
    int64_t pos;
    memset(&ft, 0, sizeof(ft));
    ft.play_state = PLAYSTATE_PLAYING;
    ft.rate = 48000;
    ft.latency_ms = 90;
    DefaultAudioSink_init(&sink);
    DefaultAudioSink_configure(&sink, ft_ops(&ft), 48000, 1, 4, 48000 * 4 / 10);
    pos = DefaultAudioSink_getCurrentPositionUs(&sink, 0);
    ck("DefaultAudioSink getCurrentPositionUs TIME_UNSET before first buffer",
       pos == MS_C_TIME_UNSET);
    DefaultAudioSink_handleBufferTimestamps(&sink, 0);
    DefaultAudioSink_addWrittenFrames(&sink, 48000); /* 1s */
    ft.head = 24000; /* 0.5s */
    pos = DefaultAudioSink_getCurrentPositionUs(&sink, 0);
    ck("DefaultAudioSink position after first buffer is set (not UNSET)", pos != MS_C_TIME_UNSET);
    ck("DefaultAudioSink position does not exceed written", pos <= 1000000);
}

static void test_standalone_clock(void)
{
    StandaloneMediaClock c;
    StandaloneMediaClock_init(&c);
    set_now_us(1 * 1000000ll);
    StandaloneMediaClock_start(&c);
    set_now_us(3 * 1000000ll);
    ck("StandaloneMediaClock advances 2s", StandaloneMediaClock_getPositionUs(&c) == 2000000ll);
    StandaloneMediaClock_setPlaybackParameters(&c, 2.0f);
    set_now_us(4 * 1000000ll);
    ck("StandaloneMediaClock speed 2x adds 2s in 1s wall",
       StandaloneMediaClock_getPositionUs(&c) == 4000000ll);
}

static void test_release_control(void)
{
    VideoFrameReleaseControl c;
    FrameReleaseInfo info;
    int act;
    VideoFrameReleaseControl_init(&c);
    VideoFrameReleaseControl_allowReleaseFirstFrameBeforeStarted(&c);
    VideoFrameReleaseControl_setOutputSurface(&c, 1);
    VideoFrameReleaseControl_onStarted(&c);
    set_now_us(0);

    g_skip_count = 0;
    act = VideoFrameReleaseControl_getFrameReleaseAction(
        &c, /*pts*/ 0, /*pos*/ 0, /*elapsed*/ 0, /*streamStart*/ 0, 0, 0, MS_C_TIME_UNSET,
        1, &info, maybe_drop_from_count, NULL);
    ck("first frame (NOT_RENDERED) -> IMMEDIATELY", act == FRAME_RELEASE_IMMEDIATELY);
    VideoFrameReleaseControl_onFrameReleasedIsFirstFrame(&c);

    /* 时间戳尚未前进：positionUs == initialPositionUs → TRY_AGAIN_LATER */
    act = VideoFrameReleaseControl_getFrameReleaseAction(
        &c, 40000, 0, 0, 0, 0, 0, MS_C_TIME_UNSET, 2, &info, maybe_drop_from_count, NULL);
    ck("position not yet advancing -> TRY_AGAIN_LATER", act == FRAME_RELEASE_TRY_AGAIN_LATER);

    /* 推进 position 与墙钟，视频超前 80ms → TRY_AGAIN_LATER（阈值 50ms） */
    set_now_us(10000);
    act = VideoFrameReleaseControl_getFrameReleaseAction(
        &c, /*pts*/ 90000, /*pos*/ 10000, /*elapsed*/ 10000, 0, 0, 0, MS_C_TIME_UNSET, 3, &info,
        maybe_drop_from_count, NULL);
    ck("earlyUs ~80ms > 50ms -> TRY_AGAIN_LATER", act == FRAME_RELEASE_TRY_AGAIN_LATER);

    /* 超前 20ms → SCHEDULED */
    set_now_us(20000);
    act = VideoFrameReleaseControl_getFrameReleaseAction(
        &c, /*pts*/ 40000, /*pos*/ 20000, /*elapsed*/ 20000, 0, 0, 0, 40000000ll, 4, &info,
        maybe_drop_from_count, NULL);
    ck("earlyUs ~20ms -> SCHEDULED", act == FRAME_RELEASE_SCHEDULED);
    ck("SCHEDULED releaseTimeNs is in the future",
       info.releaseTimeNs != MS_C_TIME_UNSET && info.releaseTimeNs > ms_media3_nanoTime());

    /* 迟到 40ms、非末帧、无可跳源 → DROP，不得 IGNORE/flush */
    set_now_us(100000);
    g_skip_count = 0;
    act = VideoFrameReleaseControl_getFrameReleaseAction(
        &c, /*pts*/ 60000, /*pos*/ 100000, /*elapsed*/ 100000, 0, 0, 0, MS_C_TIME_UNSET, 5, &info,
        maybe_drop_from_count, NULL);
    ck("late 40ms without skipSource -> DROP (not IGNORE)", act == FRAME_RELEASE_DROP);

    /* 迟到 600ms、有可跳源 → IGNORE（maybeDrop 成功） */
    g_skip_count = 3;
    act = VideoFrameReleaseControl_getFrameReleaseAction(
        &c, /*pts*/ 0, /*pos*/ 700000, /*elapsed*/ 100000, 0, 0, 0, MS_C_TIME_UNSET, 6, &info,
        maybe_drop_from_count, NULL);
    ck("very late with skipSource > 0 -> IGNORE", act == FRAME_RELEASE_IGNORE);

    /* 迟到 600ms、无可跳源 → 不得 IGNORE，应 DROP */
    g_skip_count = 0;
    act = VideoFrameReleaseControl_getFrameReleaseAction(
        &c, /*pts*/ 0, /*pos*/ 700000, /*elapsed*/ 100000, 0, 0, 0, MS_C_TIME_UNSET, 7, &info,
        maybe_drop_from_count, NULL);
    ck("very late with skipSource == 0 -> DROP (do not flush)", act == FRAME_RELEASE_DROP);
}

static void test_vsync_hysteresis(void)
{
    VideoFrameReleaseHelper h;
    int64_t vsync0 = 1000000000ll;
    int64_t period = 11111111ll; /* 90Hz */
    int64_t mid, a, b, release;
    VideoFrameReleaseHelper_init(&h);
    VideoFrameReleaseHelper_setVsyncData(&h, vsync0, period);
    /* 目标正好在某个 vsync 上：吸附到该 vsync 再提前 80% */
    release = VideoFrameReleaseHelper_adjustReleaseTime(&h, vsync0 + period * 4, 40000, period, 10);
    ck("VSYNC snap applies 80% offset",
       release == (vsync0 + period * 4) - (period * 80 / 100));

    /* 滞回：两次释放落在两档 vsync 正中间附近，第二次应粘住第一次的选择 */
    mid = vsync0 + period * 10 + period / 2;
    VideoFrameReleaseHelper_init(&h);
    VideoFrameReleaseHelper_setVsyncData(&h, vsync0, period);
    /* 用 INDEX_UNSET 关闭帧节奏推算，只测 VSYNC 滞回。 */
    a = VideoFrameReleaseHelper_adjustReleaseTime(&h, mid, 100000, MS_C_TIME_UNSET, MS_C_INDEX_UNSET);
    b = VideoFrameReleaseHelper_adjustReleaseTime(&h, mid + 200, 100040, MS_C_TIME_UNSET, MS_C_INDEX_UNSET);
    ck("hysteresis keeps the same neighboring vsync on tiny jitter", a == b);
}

static void test_force_render_threshold(void)
{
    ck("shouldDropOutputBuffer -30ms",
       shouldDropOutputBuffer(-30001, 0, 0) && !shouldDropOutputBuffer(-29999, 0, 0) &&
           !shouldDropOutputBuffer(-40000, 0, 1));
    ck("shouldDropBuffersToKeyframe -500ms",
       shouldDropBuffersToKeyframe(-500001, 0, 0) &&
           !shouldDropBuffersToKeyframe(-499999, 0, 0));
    ck("shouldForceRenderOutputBuffer late and >100ms since last",
       shouldForceRenderOutputBuffer(-30001, 100001) &&
           !shouldForceRenderOutputBuffer(-30001, 99999));
}

int main(void)
{
    ms_media3_clock_fns fns;
    fns.nanoTime = test_nano;
    fns.elapsedRealtimeMs = test_elapsed_ms;
    ms_media3_set_test_clock(&fns);
    set_now_us(0);
    printf("test_media3_sync (host, androidx/media main@101e97c)\n");
    test_force_render_threshold();
    test_audio_sink();
    test_standalone_clock();
    test_release_control();
    test_vsync_hysteresis();
    ms_media3_set_test_clock(NULL);
    printf("test_media3_sync: %s (%d fail)\n", fails ? "FAIL" : "ok", fails);
    return fails ? 1 : 0;
}
