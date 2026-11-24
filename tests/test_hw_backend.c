/*
 * 宿主 mock 硬解状态机测试。不是设备硬解，也不跑像素。
 * by: Long <mycjzb@gmail.com>
 */
#include "msmedia/ms_hw.h"
#include "hevc_fixtures.h"
#include "ms_vbit_fixtures.h"
#include "ms_abit_fixtures.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

/* 最小 avcC：SPS/PPS 各 1，长度够解析，内容不必可被真实解码器接受。 */
static const uint8_t k_avcc[] = {
    0x01, 0x42, 0xC0, 0x1E, 0xFF, 0xE1, 0x00, 0x08,
    0x67, 0x42, 0xC0, 0x1E, 0xDA, 0x01, 0x00, 0x00,
    0x01, 0x00, 0x05, 0x68, 0xCE, 0x38, 0x80, 0x00
};

static const uint8_t k_au[16] = {
    0x00, 0x00, 0x00, 0x0C, 0x65, 0x88, 0x84, 0x00,
    0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10, 0x11
};

static int g_rel;
static size_t g_rel_bytes;

static void rel_fn(void *ctx, uint8_t *data, size_t size)
{
    int *n = (int *)ctx;
    (void)data;
    if (n != NULL) {
        (*n)++;
    }
    g_rel++;
    g_rel_bytes += size;
}

static int test_classify(void)
{
    CHECK(ms_hw_classify_codec_name("OMX.google.h264.decoder", 21, -1, -1) ==
              MS_HW_KIND_SOFTWARE,
          "google software");
    CHECK(ms_hw_classify_codec_name("c2.android.avc.decoder", 21, -1, -1) ==
              MS_HW_KIND_SOFTWARE,
          "c2.android software");
    CHECK(ms_hw_classify_codec_name("OMX.google.aac.decoder", 21, -1, -1) ==
              MS_HW_KIND_SOFTWARE,
          "google aac software");
    CHECK(ms_hw_classify_codec_name("c2.android.aac.decoder", 29, -1, -1) ==
              MS_HW_KIND_SOFTWARE,
          "c2.android aac software");
    CHECK(ms_hw_classify_codec_name("c2.google.avc.decoder", 21, -1, -1) ==
              MS_HW_KIND_SOFTWARE,
          "c2.google software");
    CHECK(ms_hw_classify_codec_name("OMX.qcom.video.decoder.avc", 21, -1, -1) ==
              MS_HW_KIND_HARDWARE,
          "qcom name heuristic hw");
    CHECK(ms_hw_classify_codec_name("c2.qti.avc.decoder", 21, -1, -1) ==
              MS_HW_KIND_HARDWARE,
          "c2.qti name heuristic hw");
    CHECK(ms_hw_classify_codec_name("OMX.SEC.avc.sw.dec", 21, -1, -1) ==
              MS_HW_KIND_SOFTWARE,
          "sec sw");
    CHECK(ms_hw_classify_codec_name("weird.decoder", 21, -1, -1) ==
              MS_HW_KIND_SOFTWARE,
          "unknown namespace is software");
    CHECK(ms_hw_classify_codec_name("c2.qti.avc.decoder", 29, 0, 1) ==
              MS_HW_KIND_SOFTWARE,
          "API29 sw flag wins over name");
    CHECK(ms_hw_classify_codec_name("OMX.google.h264.decoder", 29, 1, 0) ==
              MS_HW_KIND_HARDWARE,
          "API29 hw flag wins over name");
    CHECK(ms_hw_classify_codec_name("", 21, -1, -1) == MS_HW_KIND_NONE, "empty");
    CHECK(ms_hw_is_system_software_codec_name("OMX.google.aac.decoder") == 1, "sys google aac");
    CHECK(ms_hw_is_system_software_codec_name("c2.android.aac.decoder") == 1, "sys c2 android");
    CHECK(ms_hw_is_system_software_codec_name("c2.google.aac.decoder") == 1, "sys c2 google");
    CHECK(ms_hw_is_system_software_codec_name("omx.ffmpeg.aac.decoder") == 0, "ffmpeg not system");
    CHECK(ms_hw_is_system_software_codec_name("OMX.qcom.audio.decoder.aac") == 0, "qcom not system sw");
    printf("  test_classify ok\n");
    return 0;
}

static int test_query_default(void)
{
    ms_hw_caps caps;
    memset(&caps, 0, sizeof(caps));
    caps.struct_size = sizeof(caps);
    CHECK_ST(ms_hw_decoder_query(MS_CODEC_H264, &caps), MS_OK, "query");
    CHECK(caps.is_hardware == 0, "query not hardware");
    CHECK(caps.verified == 1, "query verified as not device hw");
    CHECK(caps.kind == MS_HW_KIND_NONE, "kind none");
    CHECK(caps.backend != NULL && strcmp(caps.backend, "vaapi") == 0, "backend vaapi probe");
    CHECK(caps.detail != NULL && strstr(caps.detail, "not implemented") != NULL,
          "query notes decode not implemented");
    printf("  test_query_default ok\n");
    return 0;
}

static ms_status make_cfg(ms_hw_config *cfg, void *surf)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->struct_size = sizeof(*cfg);
    cfg->codec = MS_CODEC_H264;
    cfg->sample_fmt = MS_SAMPLE_FMT_AVCC;
    cfg->csd = k_avcc;
    cfg->csd_size = sizeof(k_avcc);
    cfg->width = 16;
    cfg->height = 16;
    cfg->surface = surf;
    return MS_OK;
}

static int test_refuse_default_and_sw(void)
{
    ms_hw_decoder *d = NULL;
    ms_hw_create_opts o;
    ms_hw_config cfg;
    int token = 1;
    memset(&o, 0, sizeof(o));
    o.struct_size = sizeof(o);
    o.mock_mode = MS_HW_MOCK_DEFAULT;
    CHECK_ST(ms_hw_decoder_create(NULL, NULL, &o, &d), MS_OK, "create");
    make_cfg(&cfg, &token);
    CHECK_ST(ms_hw_decoder_configure(d, &cfg), MS_ERR_UNSUPPORTED, "default refuse");
    ms_hw_decoder_destroy(d);

    d = NULL;
    o.mock_mode = MS_HW_MOCK_AS_SW;
    CHECK_ST(ms_hw_decoder_create(NULL, NULL, &o, &d), MS_OK, "create sw");
    CHECK_ST(ms_hw_decoder_configure(d, &cfg), MS_ERR_UNSUPPORTED, "sw refuse");
    {
        ms_hw_caps caps;
        memset(&caps, 0, sizeof(caps));
        caps.struct_size = sizeof(caps);
        CHECK_ST(ms_hw_decoder_get_caps(d, &caps), MS_OK, "caps");
        CHECK(caps.kind == MS_HW_KIND_SOFTWARE, "kind sw");
        CHECK(caps.is_hardware == 0, "not hw");
        CHECK(caps.verified == 1, "verified sw");
    }
    ms_hw_decoder_destroy(d);
    printf("  test_refuse_default_and_sw ok\n");
    return 0;
}

static int test_no_surface_and_bad_codec(void)
{
    ms_hw_decoder *d = NULL;
    ms_hw_create_opts o;
    ms_hw_config cfg;
    memset(&o, 0, sizeof(o));
    o.struct_size = sizeof(o);
    o.mock_mode = MS_HW_MOCK_AS_HW;
    CHECK_ST(ms_hw_decoder_create(NULL, NULL, &o, &d), MS_OK, "create");
    make_cfg(&cfg, NULL);
    CHECK_ST(ms_hw_decoder_configure(d, &cfg), MS_ERR_INVALID_ARG, "no surface");
    make_cfg(&cfg, &o);
    cfg.codec = MS_CODEC_AVS_PLUS;
    CHECK_ST(ms_hw_decoder_configure(d, &cfg), MS_ERR_INVALID_ARG, "avs+ rejects non-CAVS fmt");
    cfg.codec = MS_CODEC_HEVC;
    cfg.sample_fmt = MS_SAMPLE_FMT_AVCC;
    CHECK_ST(ms_hw_decoder_configure(d, &cfg), MS_ERR_INVALID_ARG, "hevc rejects avcc fmt");
    ms_hw_decoder_destroy(d);
    printf("  test_no_surface_and_bad_codec ok\n");
    return 0;
}

static int drain_format(ms_hw_decoder *d)
{
    ms_hw_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.struct_size = sizeof(ev);
    CHECK_ST(ms_hw_decoder_pull_event(d, &ev), MS_OK, "format ev");
    CHECK(ev.type == MS_HW_EVENT_FORMAT_CHANGED, "format changed");
    return 0;
}

static int test_lifecycle_submit_eos_flush(void)
{
    ms_hw_decoder *d = NULL;
    ms_hw_create_opts o;
    ms_hw_config cfg;
    ms_hw_buf buf;
    ms_hw_event ev;
    ms_hw_stats st;
    ms_hw_caps caps;
    int token = 7;
    int rel = 0;
    unsigned i;
    memset(&o, 0, sizeof(o));
    o.struct_size = sizeof(o);
    o.mock_mode = MS_HW_MOCK_AS_HW;
    CHECK_ST(ms_hw_decoder_create(NULL, NULL, &o, &d), MS_OK, "create");
    make_cfg(&cfg, &token);
    CHECK_ST(ms_hw_decoder_configure(d, &cfg), MS_OK, "cfg");
    memset(&caps, 0, sizeof(caps));
    caps.struct_size = sizeof(caps);
    CHECK_ST(ms_hw_decoder_get_caps(d, &caps), MS_OK, "caps");
    CHECK(caps.is_hardware == 1 && caps.verified == 1, "sim hw caps");
    CHECK(caps.detail != NULL && strstr(caps.detail, "not a device") != NULL,
          "detail says not a device");
    CHECK_ST(ms_hw_decoder_start(d), MS_OK, "start");
    CHECK(drain_format(d) == 0, "fmt");

    g_rel = 0;
    g_rel_bytes = 0;
    memset(&buf, 0, sizeof(buf));
    buf.data = k_au;
    buf.size = sizeof(k_au);
    buf.release_fn = rel_fn;
    buf.release_ctx = &rel;
    CHECK_ST(ms_hw_decoder_submit(d, &buf, 3000, MS_HW_FLAG_KEY), MS_OK, "submit");
    CHECK(rel == 0, "hold ref until drive");
    CHECK_ST(ms_hw_decoder_drive(d), MS_OK, "drive");
    CHECK(rel == 1, "released after render");
    memset(&ev, 0, sizeof(ev));
    ev.struct_size = sizeof(ev);
    CHECK_ST(ms_hw_decoder_pull_event(d, &ev), MS_OK, "rendered");
    CHECK(ev.type == MS_HW_EVENT_OUTPUT_RENDERED, "rendered type");
    CHECK(ev.has_pts == 1 && ev.pts == 3000, "pts passthrough");

    /* 反压 */
    {
        ms_hw_limits lim;
        ms_hw_decoder *d2 = NULL;
        int r2 = 0;
        ms_hw_limits_default(&lim);
        lim.max_pending_in = 2;
        lim.max_in_bytes = 64;
        CHECK_ST(ms_hw_decoder_create(NULL, &lim, &o, &d2), MS_OK, "d2");
        CHECK_ST(ms_hw_decoder_configure(d2, &cfg), MS_OK, "d2cfg");
        CHECK_ST(ms_hw_decoder_start(d2), MS_OK, "d2start");
        (void)drain_format(d2);
        buf.release_ctx = &r2;
        CHECK_ST(ms_hw_decoder_submit(d2, &buf, 0, MS_HW_FLAG_KEY), MS_OK, "s1");
        CHECK_ST(ms_hw_decoder_submit(d2, &buf, 1, 0), MS_OK, "s2");
        CHECK_ST(ms_hw_decoder_submit(d2, &buf, 2, 0), MS_LIMIT, "backpressure");
        CHECK(r2 == 0, "still held");
        CHECK_ST(ms_hw_decoder_flush(d2), MS_OK, "flush releases");
        CHECK(r2 == 2, "flush released both");
        CHECK_ST(ms_hw_decoder_submit(d2, &buf, 3, MS_HW_FLAG_KEY), MS_OK, "after flush");
        ms_hw_decoder_destroy(d2);
        CHECK(r2 == 3, "destroy releases leftover");
        buf.release_ctx = &rel;
    }

    /* EOS：drive 只在 STARTED/DRAINING；进入 EOS 后应停止 drive。 */
    CHECK_ST(ms_hw_decoder_signal_eos(d), MS_OK, "eos");
    {
        int got_eos = 0;
        for (i = 0; i < 8u && !got_eos; i++) {
            ms_status s = ms_hw_decoder_drive(d);
            if (s != MS_OK && s != MS_AGAIN) {
                fprintf(stderr, "drive eos st=%d\n", (int)s);
                failures++;
                ms_hw_decoder_destroy(d);
                return -1;
            }
            memset(&ev, 0, sizeof(ev));
            ev.struct_size = sizeof(ev);
            while (ms_hw_decoder_pull_event(d, &ev) == MS_OK) {
                if (ev.type == MS_HW_EVENT_EOS) {
                    got_eos = 1;
                    break;
                }
                ev.struct_size = sizeof(ev);
            }
        }
        CHECK(got_eos, "eos event");
    }

    memset(&st, 0, sizeof(st));
    st.struct_size = sizeof(st);
    CHECK_ST(ms_hw_decoder_get_stats(d, &st), MS_OK, "stats");
    CHECK(st.staging_copy_ops == 0 && st.staging_copy_bytes == 0, "no staging");
    CHECK(st.platform_copy_ops == 0, "mock has no platform copy");
    CHECK(st.ref_hold_ops >= 1, "ref hold");
    CHECK(st.pin_peak_bytes >= sizeof(k_au), "pin peak");
    CHECK(st.rendered_frames >= 1, "rendered");

    /* 已 EOS 后再 submit 应 STATE；flush 后可再送关键帧 */
    CHECK_ST(ms_hw_decoder_submit(d, &buf, 9, MS_HW_FLAG_KEY), MS_ERR_STATE,
             "submit after eos");
    CHECK_ST(ms_hw_decoder_flush(d), MS_OK, "flush after eos");
    CHECK_ST(ms_hw_decoder_submit(d, &buf, 100, MS_HW_FLAG_KEY), MS_OK, "seek key");
    ms_hw_decoder_destroy(d);
    printf("  test_lifecycle_submit_eos_flush ok\n");
    return 0;
}

static int test_bad_csd_and_ops(void)
{
    ms_hw_decoder *d = NULL;
    ms_hw_create_opts o;
    ms_hw_config cfg;
    uint8_t bad[4] = {0, 0, 0, 1};
    int token = 1;
    memset(&o, 0, sizeof(o));
    o.struct_size = sizeof(o);
    o.mock_mode = MS_HW_MOCK_AS_HW;
    CHECK_ST(ms_hw_decoder_create(NULL, NULL, &o, &d), MS_OK, "create");
    make_cfg(&cfg, &token);
    cfg.csd = bad;
    cfg.csd_size = sizeof(bad);
    CHECK_ST(ms_hw_decoder_configure(d, &cfg), MS_ERR_MALFORMED, "bad avcC");
    CHECK_ST(ms_hw_decoder_start(d), MS_ERR_STATE, "start before cfg");
    CHECK_ST(ms_hw_decoder_create(NULL, NULL, NULL, NULL), MS_ERR_INVALID_ARG,
             "null out");
    ms_hw_decoder_destroy(d);
    ms_hw_decoder_destroy(NULL);
    printf("  test_bad_csd_and_ops ok\n");
    return 0;
}


/* LC-AAC ASC: AOT=2, 44.1 kHz, stereo */
static const uint8_t k_asc_441_st[2] = { 0x12, 0x10 };
/* 48 kHz stereo */
static const uint8_t k_asc_48_st[2] = { 0x11, 0x90 };
static const uint8_t k_aac_au[32] = {
    0x21, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06,
    0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E,
    0x0F, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16,
    0x17, 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E
};

static int test_aac_asc_parse(void)
{
    ms_aac_asc_info info;
    memset(&info, 0, sizeof(info));
    info.struct_size = sizeof(info);
    CHECK_ST(ms_aac_asc_parse(k_asc_441_st, sizeof(k_asc_441_st), &info), MS_OK, "asc 44k");
    CHECK(info.audio_object_type == 2 && info.sample_rate == 44100 && info.channels == 2,
          "asc fields 44k stereo LC");
    CHECK_ST(ms_aac_asc_parse(k_asc_48_st, sizeof(k_asc_48_st), &info), MS_OK, "asc 48k");
    CHECK(info.sample_rate == 48000, "48k");
    CHECK_ST(ms_aac_asc_parse((const uint8_t *)"\xff\xff", 2, &info), MS_ERR_UNSUPPORTED,
             "bad asc");
    printf("  test_aac_asc_parse ok\n");
    return 0;
}

static int test_audio_refuse_and_lifecycle(void)
{
    ms_hw_audio *a = NULL;
    ms_hw_audio_caps cap;
    ms_hw_audio_stats st;
    ms_hw_audio_config cfg;
    ms_hw_create_opts o;
    ms_hw_buf buf;
    int64_t clock = 0;
    int rel = 0;

    memset(&cap, 0, sizeof(cap));
    cap.struct_size = sizeof(cap);
    CHECK_ST(ms_hw_audio_query(MS_CODEC_AAC, &cap), MS_OK, "query aac");
    CHECK(cap.is_hardware == 0 && cap.verified == 1, "query not device hw");
    CHECK(cap.backend && strcmp(cap.backend, "mock") == 0, "backend mock");

    memset(&o, 0, sizeof(o));
    o.struct_size = sizeof(o);
    o.mock_mode = MS_HW_MOCK_AS_SW;
    o.allow_software = 0;
    CHECK_ST(ms_hw_audio_create(NULL, NULL, &o, &a), MS_OK, "create sw");
    memset(&cfg, 0, sizeof(cfg));
    cfg.struct_size = sizeof(cfg);
    cfg.codec = MS_CODEC_AAC;
    cfg.sample_fmt = MS_SAMPLE_FMT_AAC_RAW;
    cfg.asc = k_asc_441_st;
    cfg.asc_size = sizeof(k_asc_441_st);
    CHECK_ST(ms_hw_audio_configure(a, &cfg), MS_ERR_UNSUPPORTED, "refuse software aac");
    memset(&cap, 0, sizeof(cap));
    cap.struct_size = sizeof(cap);
    CHECK_ST(ms_hw_audio_get_caps(a, &cap), MS_OK, "caps sw");
    CHECK(cap.is_hardware == 0 && cap.system_decode_is_not_hardware == 1, "not pretend hw");
    CHECK(cap.kind == MS_HW_KIND_SOFTWARE, "kind software");
    ms_hw_audio_destroy(a);
    a = NULL;

    o.mock_mode = MS_HW_MOCK_DEFAULT;
    CHECK_ST(ms_hw_audio_create(NULL, NULL, &o, &a), MS_OK, "create default");
    CHECK_ST(ms_hw_audio_configure(a, &cfg), MS_ERR_UNSUPPORTED, "default not pretend output");
    CHECK_ST(ms_hw_audio_set_aac_policy(a, MS_HW_AAC_POLICY_ALLOW_SYSTEM), MS_OK, "policy on default");
    CHECK_ST(ms_hw_audio_configure(a, &cfg), MS_ERR_UNSUPPORTED, "default still not pretend");
    ms_hw_audio_destroy(a);
    a = NULL;

    /* 显式允许系统 AAC：AS_SW 可配置，统计诚实。 */
    o.mock_mode = MS_HW_MOCK_AS_SW;
    o.allow_software = 0;
    CHECK_ST(ms_hw_audio_create(NULL, NULL, &o, &a), MS_OK, "create sys aac");
    CHECK_ST(ms_hw_audio_set_aac_policy(a, MS_HW_AAC_POLICY_ALLOW_SYSTEM), MS_OK, "allow system");
    CHECK(ms_hw_audio_get_aac_policy(a) == MS_HW_AAC_POLICY_ALLOW_SYSTEM, "policy get");
    CHECK_ST(ms_hw_audio_configure(a, &cfg), MS_OK, "cfg system aac");
    CHECK_ST(ms_hw_audio_start(a), MS_OK, "start system aac");
    memset(&cap, 0, sizeof(cap));
    cap.struct_size = sizeof(cap);
    CHECK_ST(ms_hw_audio_get_caps(a, &cap), MS_OK, "caps system");
    CHECK(cap.is_hardware == 0 && cap.system_decode_is_not_hardware == 1, "honest not hw");
    CHECK(cap.detail && strstr(cap.detail, "系统 AAC，非硬件") != NULL, "banner");
    rel = 0;
    memset(&buf, 0, sizeof(buf));
    buf.data = k_aac_au;
    buf.size = sizeof(k_aac_au);
    buf.release_fn = rel_fn;
    buf.release_ctx = &rel;
    CHECK_ST(ms_hw_audio_submit(a, &buf, 0, 0), MS_OK, "sys sub");
    CHECK_ST(ms_hw_audio_drive(a), MS_OK, "sys drive");
    CHECK_ST(ms_hw_audio_get_clock_us(a, &clock), MS_OK, "sys clock");
    CHECK(clock == (int64_t)((1024ull * 1000000ull) / 44100ull), "sys clock from played");
    CHECK_ST(ms_hw_audio_flush(a), MS_OK, "sys flush");
    memset(&st, 0, sizeof(st));
    st.struct_size = sizeof(st);
    CHECK_ST(ms_hw_audio_get_stats(a, &st), MS_OK, "sys flush st");
    CHECK(st.clock_us == 0 && st.flushed == 1 && st.is_hardware == 0, "flush zero, still not hw");
    CHECK_ST(ms_hw_audio_set_aac_policy(a, MS_HW_AAC_POLICY_HARDWARE_ONLY), MS_ERR_STATE,
             "policy locked after configure");
    ms_hw_audio_destroy(a);
    a = NULL;

    /* 非系统软解即使用户允许系统 AAC 也拒绝。 */
    o.mock_mode = MS_HW_MOCK_AS_FOREIGN_SW;
    o.allow_software = 0;
    CHECK_ST(ms_hw_audio_create(NULL, NULL, &o, &a), MS_OK, "create foreign");
    CHECK_ST(ms_hw_audio_set_aac_policy(a, MS_HW_AAC_POLICY_ALLOW_SYSTEM), MS_OK, "policy foreign");
    CHECK_ST(ms_hw_audio_configure(a, &cfg), MS_ERR_UNSUPPORTED, "refuse foreign sw");
    memset(&cap, 0, sizeof(cap));
    cap.struct_size = sizeof(cap);
    CHECK_ST(ms_hw_audio_get_caps(a, &cap), MS_OK, "caps foreign");
    CHECK(cap.is_hardware == 0 && cap.system_decode_is_not_hardware == 1, "foreign not hw");
    ms_hw_audio_destroy(a);
    a = NULL;

    o.mock_mode = MS_HW_MOCK_AS_HW;
    o.allow_software = 0;
    rel = 0;
    CHECK_ST(ms_hw_audio_create(NULL, NULL, &o, &a), MS_OK, "create as-hw");
    CHECK_ST(ms_hw_audio_configure(a, &cfg), MS_OK, "cfg 44k");
    CHECK_ST(ms_hw_audio_start(a), MS_OK, "start");
    memset(&cap, 0, sizeof(cap));
    cap.struct_size = sizeof(cap);
    CHECK_ST(ms_hw_audio_get_caps(a, &cap), MS_OK, "caps as-hw");
    CHECK(cap.is_hardware == 0, "mock as-hw still is_hardware=0");
    memset(&buf, 0, sizeof(buf));
    buf.data = k_aac_au;
    buf.size = sizeof(k_aac_au);
    buf.release_fn = rel_fn;
    buf.release_ctx = &rel;
    CHECK_ST(ms_hw_audio_submit(a, &buf, 0, 0), MS_OK, "sub0");
    /* 未 drive 前：有排队字节，时钟仍为 0（不得用排队字节当时钟） */
    memset(&st, 0, sizeof(st));
    st.struct_size = sizeof(st);
    CHECK_ST(ms_hw_audio_get_stats(a, &st), MS_OK, "st queued");
    CHECK(st.bytes_queued > 0 && st.clock_us == 0 && st.au_played == 0, "queued != clock");
    CHECK_ST(ms_hw_audio_drive(a), MS_OK, "drive");
    CHECK_ST(ms_hw_audio_get_clock_us(a, &clock), MS_OK, "clock");
    CHECK(clock == (int64_t)((1024ull * 1000000ull) / 44100ull), "clock from played frame");
    CHECK(rel == 1, "released after play");
    CHECK_ST(ms_hw_audio_get_stats(a, &st), MS_OK, "st played");
    CHECK(st.au_played == 1 && st.samples_played == 1024, "played 1 frame");
    CHECK(st.is_hardware == 0, "stats not hw");

    /* PCM 格式变化 */
    cfg.asc = k_asc_48_st;
    cfg.asc_size = sizeof(k_asc_48_st);
    CHECK_ST(ms_hw_audio_configure(a, &cfg), MS_OK, "recfg 48k");
    CHECK_ST(ms_hw_audio_get_stats(a, &st), MS_OK, "st fmt");
    CHECK(st.sample_rate == 48000 && st.pcm_format_changes >= 1, "pcm change");

    /* 部分 write：额度小于一帧 PCM（1024*2*2=4096） */
    CHECK_ST(ms_hw_audio_add_write_quota(a, 100), MS_OK, "quota 100");
    rel = 0;
    buf.release_ctx = &rel;
    CHECK_ST(ms_hw_audio_submit(a, &buf, 21000, 0), MS_OK, "sub partial");
    CHECK_ST(ms_hw_audio_drive(a), MS_OK, "drive partial");
    CHECK_ST(ms_hw_audio_get_stats(a, &st), MS_OK, "st partial");
    CHECK(st.partial_writes >= 1 && st.pending == 1 && rel == 0, "partial not complete");
    CHECK_ST(ms_hw_audio_add_write_quota(a, 10000), MS_OK, "quota rest");
    CHECK_ST(ms_hw_audio_drive(a), MS_OK, "drive rest");
    CHECK_ST(ms_hw_audio_get_stats(a, &st), MS_OK, "st complete");
    CHECK(st.pending == 0 && rel == 1, "completed after quota");

    /* 有界背压 */
    {
        ms_hw_limits lim;
        ms_hw_audio *b = NULL;
        memset(&lim, 0, sizeof(lim));
        lim.struct_size = sizeof(lim);
        lim.max_pending_in = 1;
        CHECK_ST(ms_hw_audio_create(NULL, &lim, &o, &b), MS_OK, "cap1");
        CHECK_ST(ms_hw_audio_configure(b, &cfg), MS_OK, "cfg b");
        CHECK_ST(ms_hw_audio_start(b), MS_OK, "start b");
        buf.release_fn = NULL;
        CHECK_ST(ms_hw_audio_submit(b, &buf, 0, 0), MS_OK, "b sub0");
        CHECK_ST(ms_hw_audio_submit(b, &buf, 0, 0), MS_LIMIT, "b limit");
        CHECK_ST(ms_hw_audio_get_stats(b, &st), MS_OK, "st bp");
        CHECK(st.backpressure >= 1, "backpressure counted");
        CHECK_ST(ms_hw_audio_flush(b), MS_OK, "flush");
        CHECK_ST(ms_hw_audio_get_stats(b, &st), MS_OK, "st flush");
        CHECK(st.pending == 0 && st.clock_us == 0 && st.flushed == 1, "flush clears clock/queue");
        CHECK_ST(ms_hw_audio_submit(b, &buf, 0, 0), MS_OK, "after flush");
        CHECK_ST(ms_hw_audio_signal_eos(b), MS_OK, "eos");
        CHECK_ST(ms_hw_audio_drive(b), MS_OK, "drive eos");
        CHECK_ST(ms_hw_audio_get_stats(b, &st), MS_OK, "st eos");
        CHECK(st.eos_in == 1 && st.eos_out == 1, "eos out after play");
        CHECK_ST(ms_hw_audio_cancel(b), MS_OK, "cancel");
        CHECK_ST(ms_hw_audio_submit(b, &buf, 0, 0), MS_ERR_CANCELED, "submit after cancel");
        ms_hw_audio_destroy(b);
    }

    /* 切换释放 / 重复 destroy 安全 */
    ms_hw_audio_destroy(a);
    ms_hw_audio_destroy(NULL);
    printf("  test_audio_refuse_and_lifecycle ok (host mock; not device audio)\n");
    return 0;
}

static int test_hevc_mock_configure_submit(void)
{
    ms_hw_decoder *d = NULL;
    ms_hw_create_opts o;
    ms_hw_config cfg;
    ms_hw_caps caps;
    ms_hw_buf buf;
    uint8_t csd[256], au[256];
    size_t cn = 0, an = 0;
    int token = 3;

    CHECK(hevc_annexb_csd(csd, sizeof(csd), &cn, k_hevc_vps_16, sizeof(k_hevc_vps_16),
                          k_hevc_sps_16, sizeof(k_hevc_sps_16),
                          k_hevc_pps_16, sizeof(k_hevc_pps_16)) == 0, "csd");
    CHECK(hevc_annexb_append(au, sizeof(au), &an, k_hevc_irap_16, sizeof(k_hevc_irap_16)) == 0,
          "irap");
    memset(&o, 0, sizeof(o));
    o.struct_size = sizeof(o);
    o.mock_mode = MS_HW_MOCK_AS_HW;
    CHECK_ST(ms_hw_decoder_create(NULL, NULL, &o, &d), MS_OK, "create");
    memset(&cfg, 0, sizeof(cfg));
    cfg.struct_size = sizeof(cfg);
    cfg.codec = MS_CODEC_HEVC;
    cfg.sample_fmt = MS_SAMPLE_FMT_ANNEXB;
    cfg.csd = csd;
    cfg.csd_size = cn;
    cfg.width = 16;
    cfg.height = 16;
    cfg.surface = &token;
    CHECK_ST(ms_hw_decoder_configure(d, &cfg), MS_OK, "hevc cfg");
    memset(&caps, 0, sizeof(caps));
    caps.struct_size = sizeof(caps);
    CHECK_ST(ms_hw_decoder_get_caps(d, &caps), MS_OK, "caps");
    CHECK(caps.codec == MS_CODEC_HEVC, "codec hevc");
    CHECK(caps.is_hardware == 1, "mock_as_hw flag");
    CHECK(caps.kind == MS_HW_KIND_HARDWARE, "kind");
    CHECK(caps.codec_name != NULL && strcmp(caps.codec_name, "mock.hevc.hw") == 0, "name");
    CHECK(caps.detail != NULL && strstr(caps.detail, "not a device") != NULL, "not device");
    CHECK_ST(ms_hw_decoder_start(d), MS_OK, "start");
    memset(&buf, 0, sizeof(buf));
    buf.data = au;
    buf.size = an;
    CHECK_ST(ms_hw_decoder_submit(d, &buf, 0, MS_HW_FLAG_KEY), MS_OK, "submit");
    ms_hw_decoder_destroy(d);

    d = NULL;
    CHECK_ST(ms_hw_decoder_create(NULL, NULL, &o, &d), MS_OK, "create2");
    {
        uint8_t bad[8] = { 0, 0, 0, 1, 0x40, 0x01, 0, 0 };
        cfg.csd = bad;
        cfg.csd_size = sizeof(bad);
        CHECK_ST(ms_hw_decoder_configure(d, &cfg), MS_ERR_MALFORMED, "incomplete csd");
    }
    memset(&caps, 0, sizeof(caps));
    caps.struct_size = sizeof(caps);
    CHECK_ST(ms_hw_decoder_query(MS_CODEC_HEVC, &caps), MS_OK, "query");
    CHECK(caps.is_hardware == 0 && caps.verified == 1, "host query not hardware");
    ms_hw_decoder_destroy(d);
    printf("  test_hevc_mock_configure_submit ok (host mock; not device decode)\n");
    return 0;
}



static int test_vbit_mock_configure_submit(void)
{
    ms_hw_decoder *d = NULL;
    ms_hw_create_opts o;
    ms_hw_config cfg;
    ms_hw_caps caps;
    ms_hw_buf buf;
    int token = 5;
    uint8_t au[64];
    size_t an;

    memset(&o, 0, sizeof(o));
    o.struct_size = sizeof(o);
    o.mock_mode = MS_HW_MOCK_AS_SW;
    CHECK_ST(ms_hw_decoder_create(NULL, NULL, &o, &d), MS_OK, "create sw");
    memset(&cfg, 0, sizeof(cfg));
    cfg.struct_size = sizeof(cfg);
    cfg.codec = MS_CODEC_MPEG2_VIDEO;
    cfg.sample_fmt = MS_SAMPLE_FMT_MPV;
    cfg.csd = k_mpv_csd_16;
    cfg.csd_size = sizeof(k_mpv_csd_16);
    cfg.width = 16;
    cfg.height = 16;
    cfg.surface = &token;
    CHECK_ST(ms_hw_decoder_configure(d, &cfg), MS_ERR_UNSUPPORTED, "mpeg2 sw refuse");
    memset(&caps, 0, sizeof(caps));
    caps.struct_size = sizeof(caps);
    CHECK_ST(ms_hw_decoder_get_caps(d, &caps), MS_OK, "caps sw");
    CHECK(caps.kind == MS_HW_KIND_SOFTWARE && caps.is_hardware == 0, "sw kind");
    CHECK(caps.codec_name != NULL && strcmp(caps.codec_name, "mock.mpeg2.sw") == 0, "sw name");
    ms_hw_decoder_destroy(d);

    d = NULL;
    o.mock_mode = MS_HW_MOCK_AS_HW;
    CHECK_ST(ms_hw_decoder_create(NULL, NULL, &o, &d), MS_OK, "create hw");
    CHECK_ST(ms_hw_decoder_configure(d, &cfg), MS_OK, "mpeg2 cfg");
    memset(&caps, 0, sizeof(caps));
    caps.struct_size = sizeof(caps);
    CHECK_ST(ms_hw_decoder_get_caps(d, &caps), MS_OK, "caps");
    CHECK(caps.codec == MS_CODEC_MPEG2_VIDEO, "codec");
    CHECK(caps.is_hardware == 1 && caps.kind == MS_HW_KIND_HARDWARE, "mock as hw");
    CHECK(caps.codec_name != NULL && strcmp(caps.codec_name, "mock.mpeg2.hw") == 0, "name");
    CHECK(caps.detail != NULL && strstr(caps.detail, "not a device") != NULL, "not device");
    CHECK_ST(ms_hw_decoder_start(d), MS_OK, "start");
    an = 0;
    memcpy(au, k_mpv_csd_16, sizeof(k_mpv_csd_16));
    an = sizeof(k_mpv_csd_16);
    memcpy(au + an, k_mpv_pic_i, sizeof(k_mpv_pic_i));
    an += sizeof(k_mpv_pic_i);
    memset(&buf, 0, sizeof(buf));
    buf.data = au;
    buf.size = an;
    CHECK_ST(ms_hw_decoder_submit(d, &buf, 0, MS_HW_FLAG_KEY), MS_OK, "submit mpeg2");
    ms_hw_decoder_destroy(d);

    d = NULL;
    CHECK_ST(ms_hw_decoder_create(NULL, NULL, &o, &d), MS_OK, "create mp4v");
    cfg.codec = MS_CODEC_MPEG4_PART2;
    cfg.sample_fmt = MS_SAMPLE_FMT_MP4V;
    cfg.csd = k_mp4v_csd_16;
    cfg.csd_size = sizeof(k_mp4v_csd_16);
    CHECK_ST(ms_hw_decoder_configure(d, &cfg), MS_OK, "mpeg4 cfg");
    memset(&caps, 0, sizeof(caps));
    caps.struct_size = sizeof(caps);
    CHECK_ST(ms_hw_decoder_get_caps(d, &caps), MS_OK, "caps4");
    CHECK(caps.codec_name != NULL && strcmp(caps.codec_name, "mock.mpeg4.hw") == 0, "name4");
    CHECK_ST(ms_hw_decoder_start(d), MS_OK, "start4");
    memset(&buf, 0, sizeof(buf));
    buf.data = k_mp4v_vop_i;
    buf.size = sizeof(k_mp4v_vop_i);
    CHECK_ST(ms_hw_decoder_submit(d, &buf, 0, MS_HW_FLAG_KEY), MS_OK, "submit mpeg4");
    /* 错误采样格式拒绝 */
    {
        ms_hw_decoder *d2 = NULL;
        CHECK_ST(ms_hw_decoder_create(NULL, NULL, &o, &d2), MS_OK, "d2");
        cfg.sample_fmt = MS_SAMPLE_FMT_AVCC;
        CHECK_ST(ms_hw_decoder_configure(d2, &cfg), MS_ERR_INVALID_ARG, "mpeg4 rejects avcc");
        ms_hw_decoder_destroy(d2);
    }
    ms_hw_decoder_destroy(d);

    /* H.263：合法无 CSD，按宽高配置；拒绝捏造 csd-0 与软件 codec */
    d = NULL;
    o.mock_mode = MS_HW_MOCK_AS_SW;
    CHECK_ST(ms_hw_decoder_create(NULL, NULL, &o, &d), MS_OK, "h263 sw");
    memset(&cfg, 0, sizeof(cfg));
    cfg.struct_size = sizeof(cfg);
    cfg.codec = MS_CODEC_H263;
    cfg.sample_fmt = MS_SAMPLE_FMT_H263;
    cfg.csd = NULL;
    cfg.csd_size = 0;
    cfg.width = 176;
    cfg.height = 144;
    cfg.surface = &token;
    CHECK_ST(ms_hw_decoder_configure(d, &cfg), MS_ERR_UNSUPPORTED, "h263 sw refuse");
    memset(&caps, 0, sizeof(caps));
    caps.struct_size = sizeof(caps);
    CHECK_ST(ms_hw_decoder_get_caps(d, &caps), MS_OK, "h263 sw caps");
    CHECK(caps.kind == MS_HW_KIND_SOFTWARE && caps.is_hardware == 0, "h263 sw kind");
    CHECK(caps.codec_name != NULL && strcmp(caps.codec_name, "mock.h263.sw") == 0, "h263 sw name");
    ms_hw_decoder_destroy(d);

    d = NULL;
    o.mock_mode = MS_HW_MOCK_AS_HW;
    CHECK_ST(ms_hw_decoder_create(NULL, NULL, &o, &d), MS_OK, "h263 hw");
    CHECK_ST(ms_hw_decoder_configure(d, &cfg), MS_OK, "h263 empty csd cfg");
    memset(&caps, 0, sizeof(caps));
    caps.struct_size = sizeof(caps);
    CHECK_ST(ms_hw_decoder_get_caps(d, &caps), MS_OK, "h263 caps");
    CHECK(caps.codec == MS_CODEC_H263 && caps.is_hardware == 1, "h263 mock hw");
    CHECK(caps.codec_name != NULL && strcmp(caps.codec_name, "mock.h263.hw") == 0, "h263 name");
    CHECK(caps.detail != NULL && strstr(caps.detail, "not a device") != NULL, "not device");
    CHECK_ST(ms_hw_decoder_start(d), MS_OK, "h263 start");
    memset(&buf, 0, sizeof(buf));
    buf.data = k_h263_qcif_i;
    buf.size = sizeof(k_h263_qcif_i);
    CHECK_ST(ms_hw_decoder_submit(d, &buf, 0, MS_HW_FLAG_KEY), MS_OK, "submit I");
    buf.data = k_h263_qcif_p;
    buf.size = sizeof(k_h263_qcif_p);
    CHECK_ST(ms_hw_decoder_submit(d, &buf, 9000, 0), MS_OK, "submit P");
    {
        ms_hw_decoder *d2 = NULL;
        CHECK_ST(ms_hw_decoder_create(NULL, NULL, &o, &d2), MS_OK, "h263 d2");
        cfg.csd = k_h263_qcif_i;
        cfg.csd_size = sizeof(k_h263_qcif_i);
        CHECK_ST(ms_hw_decoder_configure(d2, &cfg), MS_ERR_INVALID_ARG, "reject invented csd");
        cfg.csd = NULL;
        cfg.csd_size = 0;
        cfg.width = 0;
        cfg.height = 0;
        CHECK_ST(ms_hw_decoder_configure(d2, &cfg), MS_ERR_INVALID_ARG, "need size");
        cfg.width = 176;
        cfg.height = 144;
        cfg.sample_fmt = MS_SAMPLE_FMT_AVCC;
        CHECK_ST(ms_hw_decoder_configure(d2, &cfg), MS_ERR_INVALID_ARG, "reject avcc");
        ms_hw_decoder_destroy(d2);
    }
    ms_hw_decoder_destroy(d);

    /* AV1：合法 av1C 与合法无 CSD；拒绝软件 codec */
    d = NULL;
    o.mock_mode = MS_HW_MOCK_AS_SW;
    CHECK_ST(ms_hw_decoder_create(NULL, NULL, &o, &d), MS_OK, "av1 sw");
    memset(&cfg, 0, sizeof(cfg));
    cfg.struct_size = sizeof(cfg);
    cfg.codec = MS_CODEC_AV1;
    cfg.sample_fmt = MS_SAMPLE_FMT_AV1;
    cfg.csd = k_av1_av1c_64;
    cfg.csd_size = sizeof(k_av1_av1c_64);
    cfg.width = 64;
    cfg.height = 64;
    cfg.surface = &token;
    CHECK_ST(ms_hw_decoder_configure(d, &cfg), MS_ERR_UNSUPPORTED, "av1 sw refuse");
    memset(&caps, 0, sizeof(caps));
    caps.struct_size = sizeof(caps);
    CHECK_ST(ms_hw_decoder_get_caps(d, &caps), MS_OK, "av1 sw caps");
    CHECK(caps.kind == MS_HW_KIND_SOFTWARE && caps.is_hardware == 0, "av1 sw kind");
    CHECK(caps.codec_name != NULL && strcmp(caps.codec_name, "mock.av1.sw") == 0, "av1 sw name");
    ms_hw_decoder_destroy(d);

    d = NULL;
    o.mock_mode = MS_HW_MOCK_AS_HW;
    CHECK_ST(ms_hw_decoder_create(NULL, NULL, &o, &d), MS_OK, "av1 hw");
    CHECK_ST(ms_hw_decoder_configure(d, &cfg), MS_OK, "av1 av1c cfg");
    memset(&caps, 0, sizeof(caps));
    caps.struct_size = sizeof(caps);
    CHECK_ST(ms_hw_decoder_get_caps(d, &caps), MS_OK, "av1 caps");
    CHECK(caps.codec == MS_CODEC_AV1 && caps.is_hardware == 1, "av1 mock hw");
    CHECK(caps.codec_name != NULL && strcmp(caps.codec_name, "mock.av1.hw") == 0, "av1 name");
    CHECK_ST(ms_hw_decoder_start(d), MS_OK, "av1 start");
    memset(&buf, 0, sizeof(buf));
    buf.data = k_av1_tu_key_64;
    buf.size = sizeof(k_av1_tu_key_64);
    CHECK_ST(ms_hw_decoder_submit(d, &buf, 0, MS_HW_FLAG_KEY), MS_OK, "submit key");
    ms_hw_decoder_destroy(d);

    d = NULL;
    CHECK_ST(ms_hw_decoder_create(NULL, NULL, &o, &d), MS_OK, "av1 empty");
    cfg.csd = NULL;
    cfg.csd_size = 0;
    CHECK_ST(ms_hw_decoder_configure(d, &cfg), MS_OK, "av1 legal no csd");
    CHECK_ST(ms_hw_decoder_start(d), MS_OK, "start empty");
    memset(&buf, 0, sizeof(buf));
    buf.data = k_av1_tu_key_64;
    buf.size = sizeof(k_av1_tu_key_64);
    CHECK_ST(ms_hw_decoder_submit(d, &buf, 0, MS_HW_FLAG_KEY), MS_OK, "submit no-csd");
    {
        ms_hw_decoder *d2 = NULL;
        CHECK_ST(ms_hw_decoder_create(NULL, NULL, &o, &d2), MS_OK, "av1 d2");
        cfg.width = 0;
        cfg.height = 0;
        CHECK_ST(ms_hw_decoder_configure(d2, &cfg), MS_ERR_INVALID_ARG, "need size");
        cfg.width = 64;
        cfg.height = 64;
        cfg.sample_fmt = MS_SAMPLE_FMT_AVCC;
        CHECK_ST(ms_hw_decoder_configure(d2, &cfg), MS_ERR_INVALID_ARG, "reject avcc");
        ms_hw_decoder_destroy(d2);
    }
    ms_hw_decoder_destroy(d);

    /* AVS+：真实广播类序列头作 CSD；拒绝软件 codec、基准类头、错误 fmt。不是设备硬解。 */
    d = NULL;
    o.mock_mode = MS_HW_MOCK_AS_SW;
    CHECK_ST(ms_hw_decoder_create(NULL, NULL, &o, &d), MS_OK, "avs+ sw");
    memset(&cfg, 0, sizeof(cfg));
    cfg.struct_size = sizeof(cfg);
    cfg.codec = MS_CODEC_AVS_PLUS;
    cfg.sample_fmt = MS_SAMPLE_FMT_CAVS;
    cfg.csd = k_avsplus_seq_16;
    cfg.csd_size = sizeof(k_avsplus_seq_16);
    cfg.width = 16;
    cfg.height = 16;
    cfg.surface = &token;
    CHECK_ST(ms_hw_decoder_configure(d, &cfg), MS_ERR_UNSUPPORTED, "avs+ sw refuse");
    memset(&caps, 0, sizeof(caps));
    caps.struct_size = sizeof(caps);
    CHECK_ST(ms_hw_decoder_get_caps(d, &caps), MS_OK, "avs+ sw caps");
    CHECK(caps.kind == MS_HW_KIND_SOFTWARE && caps.is_hardware == 0, "avs+ sw kind");
    CHECK(caps.codec_name != NULL && strcmp(caps.codec_name, "mock.avsplus.sw") == 0,
          "avs+ sw name");
    ms_hw_decoder_destroy(d);

    d = NULL;
    o.mock_mode = MS_HW_MOCK_AS_HW;
    CHECK_ST(ms_hw_decoder_create(NULL, NULL, &o, &d), MS_OK, "avs+ hw");
    CHECK_ST(ms_hw_decoder_configure(d, &cfg), MS_OK, "avs+ cfg");
    memset(&caps, 0, sizeof(caps));
    caps.struct_size = sizeof(caps);
    CHECK_ST(ms_hw_decoder_get_caps(d, &caps), MS_OK, "avs+ caps");
    CHECK(caps.codec == MS_CODEC_AVS_PLUS && caps.is_hardware == 1, "avs+ mock hw");
    CHECK(caps.codec_name != NULL && strcmp(caps.codec_name, "mock.avsplus.hw") == 0,
          "avs+ name");
    CHECK(caps.detail != NULL && strstr(caps.detail, "not a device") != NULL, "not device");
    CHECK_ST(ms_hw_decoder_start(d), MS_OK, "avs+ start");
    an = 0;
    memcpy(au, k_avsplus_seq_16, sizeof(k_avsplus_seq_16));
    an = sizeof(k_avsplus_seq_16);
    memcpy(au + an, k_avs_pic_i, sizeof(k_avs_pic_i));
    an += sizeof(k_avs_pic_i);
    memset(&buf, 0, sizeof(buf));
    buf.data = au;
    buf.size = an;
    CHECK_ST(ms_hw_decoder_submit(d, &buf, 0, MS_HW_FLAG_KEY), MS_OK, "submit I");
    memset(&buf, 0, sizeof(buf));
    buf.data = k_avs_pic_p;
    buf.size = sizeof(k_avs_pic_p);
    CHECK_ST(ms_hw_decoder_submit(d, &buf, 9000, 0), MS_OK, "submit P");
    {
        ms_hw_decoder *d2 = NULL;
        unsigned char raw[128];
        ms_hw_config *old;
        CHECK_ST(ms_hw_decoder_create(NULL, NULL, &o, &d2), MS_OK, "avs+ d2");
        cfg.csd = k_avs_jizhun_seq_16;
        cfg.csd_size = sizeof(k_avs_jizhun_seq_16);
        /* CSD 拆分只拷贝缓冲，不在此校验档次；错误 fmt 仍拒绝 */
        cfg.sample_fmt = MS_SAMPLE_FMT_AVCC;
        CHECK_ST(ms_hw_decoder_configure(d2, &cfg), MS_ERR_INVALID_ARG, "avs+ reject avcc");
        cfg.sample_fmt = MS_SAMPLE_FMT_CAVS;
        cfg.csd = NULL;
        cfg.csd_size = 0;
        CHECK_ST(ms_hw_decoder_configure(d2, &cfg), MS_ERR_INVALID_ARG, "avs+ need csd");
        /* 旧 ABI：struct_size 截到 surface 之前仍可配置（加法字段） */
        old = (ms_hw_config *)(void *)raw;
        memset(raw, 0xA5, sizeof(raw));
        memset(old, 0, offsetof(ms_hw_config, surface) + sizeof(void *));
        old->struct_size = offsetof(ms_hw_config, surface) + sizeof(void *);
        old->codec = MS_CODEC_AVS_PLUS;
        old->sample_fmt = MS_SAMPLE_FMT_CAVS;
        old->csd = k_avsplus_seq_16;
        old->csd_size = sizeof(k_avsplus_seq_16);
        old->width = 16;
        old->height = 16;
        old->surface = &token;
        CHECK_ST(ms_hw_decoder_configure(d2, old), MS_OK, "avs+ old abi");
        ms_hw_decoder_destroy(d2);
    }
    ms_hw_decoder_destroy(d);
    printf("  test_vbit_mock_configure_submit ok (host mock; not device decode)\n");
    return 0;
}


static int test_abit_mock_configure_submit(void)
{
    ms_hw_audio *a = NULL;
    ms_hw_audio_caps cap;
    ms_hw_audio_stats st;
    ms_hw_audio_config cfg;
    ms_hw_create_opts o;
    ms_hw_buf buf;
    int64_t clock = 0;
    int rel = 0;

    memset(&cap, 0, sizeof(cap));
    cap.struct_size = sizeof(cap);
    CHECK_ST(ms_hw_audio_query(MS_CODEC_AC3, &cap), MS_OK, "query ac3");
    CHECK(cap.is_hardware == 0 && cap.verified == 1, "query not device hw");
    CHECK(cap.codec == MS_CODEC_AC3, "query codec");

    memset(&o, 0, sizeof(o));
    o.struct_size = sizeof(o);
    o.mock_mode = MS_HW_MOCK_AS_SW;
    o.allow_software = 0;
    CHECK_ST(ms_hw_audio_create(NULL, NULL, &o, &a), MS_OK, "create sw");
    memset(&cfg, 0, sizeof(cfg));
    cfg.struct_size = sizeof(cfg);
    cfg.codec = MS_CODEC_AC3;
    cfg.sample_fmt = MS_SAMPLE_FMT_AC3;
    cfg.sample_rate = 48000;
    cfg.channels = 2;
    cfg.samples_per_frame = 1536;
    /* 即使打开系统 AAC 例外，也不得把软件 AC-3 当硬解。 */
    CHECK_ST(ms_hw_audio_set_aac_policy(a, MS_HW_AAC_POLICY_ALLOW_SYSTEM), MS_OK, "policy");
    CHECK_ST(ms_hw_audio_configure(a, &cfg), MS_ERR_UNSUPPORTED, "refuse sw ac3");
    memset(&cap, 0, sizeof(cap));
    cap.struct_size = sizeof(cap);
    CHECK_ST(ms_hw_audio_get_caps(a, &cap), MS_OK, "caps sw");
    CHECK(cap.is_hardware == 0 && cap.system_decode_is_not_hardware == 1, "not pretend hw");
    ms_hw_audio_destroy(a);
    a = NULL;

    o.mock_mode = MS_HW_MOCK_AS_HW;
    CHECK_ST(ms_hw_audio_create(NULL, NULL, &o, &a), MS_OK, "create as-hw");
    CHECK_ST(ms_hw_audio_configure(a, &cfg), MS_OK, "cfg empty csd");
    CHECK_ST(ms_hw_audio_start(a), MS_OK, "start");
    memset(&cap, 0, sizeof(cap));
    cap.struct_size = sizeof(cap);
    CHECK_ST(ms_hw_audio_get_caps(a, &cap), MS_OK, "caps");
    CHECK(cap.is_hardware == 0, "mock not device hw");
    memset(&buf, 0, sizeof(buf));
    buf.data = k_ac3_48k_stereo;
    buf.size = sizeof(k_ac3_48k_stereo);
    buf.release_fn = rel_fn;
    buf.release_ctx = &rel;
    CHECK_ST(ms_hw_audio_submit(a, &buf, 0, 0), MS_OK, "sub");
    CHECK_ST(ms_hw_audio_drive(a), MS_OK, "drive");
    CHECK_ST(ms_hw_audio_get_clock_us(a, &clock), MS_OK, "clock");
    CHECK(clock == (int64_t)((1536ull * 1000000ull) / 48000ull), "clock 1536 @ 48k");
    memset(&st, 0, sizeof(st));
    st.struct_size = sizeof(st);
    CHECK_ST(ms_hw_audio_get_stats(a, &st), MS_OK, "st");
    CHECK(st.samples_played == 1536 && st.au_played == 1, "played ac3 frame");
    /* 把 AAC ASC 塞进 AC-3 CSD 必须拒绝，不得当配置成功。 */
    cfg.asc = k_asc_441_st;
    cfg.asc_size = sizeof(k_asc_441_st);
    CHECK_ST(ms_hw_audio_configure(a, &cfg), MS_ERR_MALFORMED, "refuse invented asc");
    cfg.asc = NULL;
    cfg.asc_size = 0;
    /* 旧 ABI：struct_size 不含加法字段，仍可按采样率/声道配置。 */
    {
        unsigned char raw[64];
        ms_hw_audio_config *old = (ms_hw_audio_config *)(void *)raw;
        memset(raw, 0xA5, sizeof(raw));
        memset(old, 0, offsetof(ms_hw_audio_config, samples_per_frame));
        old->struct_size = offsetof(ms_hw_audio_config, samples_per_frame);
        old->codec = MS_CODEC_AC3;
        old->sample_fmt = MS_SAMPLE_FMT_AC3;
        old->sample_rate = 44100;
        old->channels = 2;
        CHECK_ST(ms_hw_audio_configure(a, old), MS_OK, "old abi cfg");
        CHECK_ST(ms_hw_audio_get_stats(a, &st), MS_OK, "st old");
        CHECK(st.sample_rate == 44100, "old abi rate");
    }
    ms_hw_audio_destroy(a);
    printf("  test_abit_mock_configure_submit ok (host mock; empty CSD; not device sound)\n");
    return 0;
}


static int test_abit_mpeg_mock_configure_submit(void)
{
    ms_hw_audio *a = NULL;
    ms_hw_audio_caps cap;
    ms_hw_audio_stats st;
    ms_hw_audio_config cfg;
    ms_hw_create_opts o;
    ms_hw_buf buf;
    int64_t clock = 0;
    int rel = 0;

    memset(&cap, 0, sizeof(cap));
    cap.struct_size = sizeof(cap);
    CHECK_ST(ms_hw_audio_query(MS_CODEC_MP3, &cap), MS_OK, "query mp3");
    CHECK(cap.is_hardware == 0 && cap.verified == 1, "query not device hw");
    CHECK_ST(ms_hw_audio_query(MS_CODEC_MP2, &cap), MS_OK, "query mp2");
    CHECK(cap.is_hardware == 0, "mp2 query not hw");

    memset(&o, 0, sizeof(o));
    o.struct_size = sizeof(o);
    o.mock_mode = MS_HW_MOCK_AS_SW;
    o.allow_software = 0;
    CHECK_ST(ms_hw_audio_create(NULL, NULL, &o, &a), MS_OK, "create sw");
    memset(&cfg, 0, sizeof(cfg));
    cfg.struct_size = sizeof(cfg);
    cfg.codec = MS_CODEC_MP3;
    cfg.sample_fmt = MS_SAMPLE_FMT_MPEG_AUDIO;
    cfg.sample_rate = 44100;
    cfg.channels = 2;
    cfg.samples_per_frame = 1152;
    CHECK_ST(ms_hw_audio_set_aac_policy(a, MS_HW_AAC_POLICY_ALLOW_SYSTEM), MS_OK, "policy");
    CHECK_ST(ms_hw_audio_configure(a, &cfg), MS_ERR_UNSUPPORTED, "refuse sw mp3");
    memset(&cap, 0, sizeof(cap));
    cap.struct_size = sizeof(cap);
    CHECK_ST(ms_hw_audio_get_caps(a, &cap), MS_OK, "caps sw");
    CHECK(cap.is_hardware == 0 && cap.system_decode_is_not_hardware == 1, "not pretend hw");
    ms_hw_audio_destroy(a);
    a = NULL;

    o.mock_mode = MS_HW_MOCK_AS_HW;
    CHECK_ST(ms_hw_audio_create(NULL, NULL, &o, &a), MS_OK, "create as-hw");
    CHECK_ST(ms_hw_audio_configure(a, &cfg), MS_OK, "cfg empty csd mp3");
    CHECK_ST(ms_hw_audio_start(a), MS_OK, "start");
    memset(&buf, 0, sizeof(buf));
    buf.data = k_mp3_441_128;
    buf.size = sizeof(k_mp3_441_128);
    buf.release_fn = rel_fn;
    buf.release_ctx = &rel;
    CHECK_ST(ms_hw_audio_submit(a, &buf, 0, 0), MS_OK, "sub");
    CHECK_ST(ms_hw_audio_drive(a), MS_OK, "drive");
    CHECK_ST(ms_hw_audio_get_clock_us(a, &clock), MS_OK, "clock");
    CHECK(clock == (int64_t)((1152ull * 1000000ull) / 44100ull), "clock 1152 @ 44.1k");
    memset(&st, 0, sizeof(st));
    st.struct_size = sizeof(st);
    CHECK_ST(ms_hw_audio_get_stats(a, &st), MS_OK, "st");
    CHECK(st.samples_played == 1152 && st.au_played == 1, "played mp3 frame");
    CHECK(st.is_hardware == 0, "mock not device hw");

    CHECK_ST(ms_hw_audio_flush(a), MS_OK, "flush after mp3");
    cfg.codec = MS_CODEC_MP2;
    cfg.sample_rate = 48000;
    cfg.samples_per_frame = 1152;
    cfg.asc = k_asc_441_st;
    cfg.asc_size = sizeof(k_asc_441_st);
    CHECK_ST(ms_hw_audio_configure(a, &cfg), MS_ERR_MALFORMED, "refuse invented asc on mp2");
    cfg.asc = NULL;
    cfg.asc_size = 0;
    CHECK_ST(ms_hw_audio_configure(a, &cfg), MS_OK, "cfg mp2 empty csd");
    rel = 0;
    buf.data = k_mp2_48k_192;
    buf.size = sizeof(k_mp2_48k_192);
    buf.release_ctx = &rel;
    CHECK_ST(ms_hw_audio_start(a), MS_OK, "start mp2");
    CHECK_ST(ms_hw_audio_submit(a, &buf, 0, 0), MS_OK, "sub mp2");
    CHECK_ST(ms_hw_audio_drive(a), MS_OK, "drive mp2");
    CHECK_ST(ms_hw_audio_get_clock_us(a, &clock), MS_OK, "clock mp2");
    CHECK(clock == (int64_t)((1152ull * 1000000ull) / 48000ull), "clock 1152 @ 48k");
    {
        unsigned char raw[64];
        ms_hw_audio_config *old = (ms_hw_audio_config *)(void *)raw;
        memset(raw, 0xA5, sizeof(raw));
        memset(old, 0, offsetof(ms_hw_audio_config, samples_per_frame));
        old->struct_size = offsetof(ms_hw_audio_config, samples_per_frame);
        old->codec = MS_CODEC_MP3;
        old->sample_fmt = MS_SAMPLE_FMT_MPEG_AUDIO;
        old->sample_rate = 44100;
        old->channels = 2;
        CHECK_ST(ms_hw_audio_configure(a, old), MS_OK, "old abi cfg");
        CHECK_ST(ms_hw_audio_get_stats(a, &st), MS_OK, "st old");
        CHECK(st.sample_rate == 44100, "old abi rate");
    }
    ms_hw_audio_destroy(a);
    printf("  test_abit_mpeg_mock_configure_submit ok (host mock; empty CSD; not device sound)\n");
    return 0;
}

static int test_abit_opus_mock_configure_submit(void)
{
    ms_hw_audio *a = NULL;
    ms_hw_audio_caps cap;
    ms_hw_audio_stats st;
    ms_hw_audio_config cfg;
    ms_hw_create_opts o;
    ms_hw_buf buf;
    int64_t clock = 0;
    int rel = 0;
    const int64_t first_us = (int64_t)((648ull * 1000000ull) / 48000ull); /* 960-312 */
    const int64_t full_us = (int64_t)((960ull * 1000000ull) / 48000ull);

    memset(&cap, 0, sizeof(cap));
    cap.struct_size = sizeof(cap);
    CHECK_ST(ms_hw_audio_query(MS_CODEC_OPUS, &cap), MS_OK, "query opus");
    CHECK(cap.is_hardware == 0 && cap.verified == 1, "query not device hw");

    memset(&o, 0, sizeof(o));
    o.struct_size = sizeof(o);
    o.mock_mode = MS_HW_MOCK_AS_SW;
    o.allow_software = 0;
    CHECK_ST(ms_hw_audio_create(NULL, NULL, &o, &a), MS_OK, "create sw");
    memset(&cfg, 0, sizeof(cfg));
    cfg.struct_size = sizeof(cfg);
    cfg.codec = MS_CODEC_OPUS;
    cfg.sample_fmt = MS_SAMPLE_FMT_OPUS;
    cfg.sample_rate = 48000;
    cfg.channels = 2;
    cfg.asc = k_opus_head;
    cfg.asc_size = sizeof(k_opus_head);
    CHECK_ST(ms_hw_audio_set_aac_policy(a, MS_HW_AAC_POLICY_ALLOW_SYSTEM), MS_OK, "policy");
    CHECK_ST(ms_hw_audio_configure(a, &cfg), MS_ERR_UNSUPPORTED, "refuse sw opus");
    memset(&cap, 0, sizeof(cap));
    cap.struct_size = sizeof(cap);
    CHECK_ST(ms_hw_audio_get_caps(a, &cap), MS_OK, "caps sw");
    CHECK(cap.is_hardware == 0 && cap.system_decode_is_not_hardware == 1, "not pretend hw");
    ms_hw_audio_destroy(a);
    a = NULL;

    o.mock_mode = MS_HW_MOCK_AS_HW;
    CHECK_ST(ms_hw_audio_create(NULL, NULL, &o, &a), MS_OK, "create as-hw");
    cfg.asc = NULL;
    cfg.asc_size = 0;
    CHECK_ST(ms_hw_audio_configure(a, &cfg), MS_ERR_INVALID_ARG, "refuse no CSD");
    cfg.asc = k_asc_441_st;
    cfg.asc_size = sizeof(k_asc_441_st);
    CHECK_ST(ms_hw_audio_configure(a, &cfg), MS_ERR_MALFORMED, "refuse invented asc");
    cfg.asc = k_opus_dops;
    cfg.asc_size = sizeof(k_opus_dops);
    CHECK_ST(ms_hw_audio_configure(a, &cfg), MS_OK, "cfg from real dOps");
    CHECK_ST(ms_hw_audio_start(a), MS_OK, "start");
    memset(&buf, 0, sizeof(buf));
    buf.data = k_opus_toc20;
    buf.size = sizeof(k_opus_toc20);
    buf.release_fn = rel_fn;
    buf.release_ctx = &rel;
    CHECK_ST(ms_hw_audio_submit(a, &buf, 0, 0), MS_OK, "sub before drive");
    CHECK_ST(ms_hw_audio_get_clock_us(a, &clock), MS_OK, "clock queued");
    CHECK(clock == 0, "submit clock is not speaker clock");
    memset(&st, 0, sizeof(st));
    st.struct_size = sizeof(st);
    CHECK_ST(ms_hw_audio_get_stats(a, &st), MS_OK, "st queued");
    CHECK(st.au_submitted >= 1 && st.au_played == 0 && st.clock_us == 0,
          "queued != played");
    CHECK_ST(ms_hw_audio_drive(a), MS_OK, "drive1");
    CHECK_ST(ms_hw_audio_get_clock_us(a, &clock), MS_OK, "clock1");
    CHECK(clock == first_us, "pre-skip consumed once (960-312)");
    CHECK_ST(ms_hw_audio_get_stats(a, &st), MS_OK, "st1");
    CHECK(st.samples_played == 648 && st.au_played == 1, "played after preroll");

    rel = 0;
    buf.release_ctx = &rel;
    CHECK_ST(ms_hw_audio_submit(a, &buf, 20000, 0), MS_OK, "sub2");
    CHECK_ST(ms_hw_audio_drive(a), MS_OK, "drive2");
    CHECK_ST(ms_hw_audio_get_clock_us(a, &clock), MS_OK, "clock2");
    CHECK(clock == first_us + full_us, "second packet not pre-skipped again");

    CHECK_ST(ms_hw_audio_flush(a), MS_OK, "flush zeros clock, not pre-skip");
    CHECK_ST(ms_hw_audio_get_clock_us(a, &clock), MS_OK, "clock flush");
    CHECK(clock == 0, "flush zeros clock");
    rel = 0;
    buf.release_ctx = &rel;
    CHECK_ST(ms_hw_audio_submit(a, &buf, 0, 0), MS_OK, "sub after flush only");
    CHECK_ST(ms_hw_audio_drive(a), MS_OK, "drive after flush only");
    CHECK_ST(ms_hw_audio_get_clock_us(a, &clock), MS_OK, "clock after flush only");
    CHECK(clock == full_us, "flush alone does not re-apply pre-skip");

    CHECK_ST(ms_hw_audio_flush(a), MS_OK, "flush before seek 0");
    CHECK_ST(ms_hw_audio_note_seek(a, 0), MS_OK, "seek to start");
    rel = 0;
    buf.release_ctx = &rel;
    CHECK_ST(ms_hw_audio_submit(a, &buf, 0, 0), MS_OK, "sub seek0");
    CHECK_ST(ms_hw_audio_drive(a), MS_OK, "drive seek0");
    CHECK_ST(ms_hw_audio_get_clock_us(a, &clock), MS_OK, "clock seek0");
    CHECK(clock == first_us, "seek(0) arms pre-skip fallback once");

    /* 包上的 skip_start 与 fallback 不双扣 */
    CHECK_ST(ms_hw_audio_flush(a), MS_OK, "flush skip meta");
    CHECK_ST(ms_hw_audio_note_seek(a, 0), MS_OK, "seek0 again");
    {
        ms_hw_audio_au au;
        memset(&au, 0, sizeof(au));
        au.struct_size = sizeof(au);
        au.buf = &buf;
        au.skip_samples_start = 312;
        rel = 0;
        buf.release_ctx = &rel;
        CHECK_ST(ms_hw_audio_submit_au(a, &au), MS_OK, "sub with skip 312");
        CHECK_ST(ms_hw_audio_drive(a), MS_OK, "drive skip meta");
        CHECK_ST(ms_hw_audio_get_clock_us(a, &clock), MS_OK, "clock skip meta");
        CHECK(clock == first_us, "packet skip and fallback not doubled");
    }

    /* 多包 start skip 1200：960+240 */
    CHECK_ST(ms_hw_audio_flush(a), MS_OK, "flush multipacket");
    {
        ms_hw_audio_au au;
        memset(&au, 0, sizeof(au));
        au.struct_size = sizeof(au);
        au.buf = &buf;
        au.skip_samples_start = 1200;
        rel = 0;
        buf.release_ctx = &rel;
        CHECK_ST(ms_hw_audio_submit_au(a, &au), MS_OK, "sub 1200");
        CHECK_ST(ms_hw_audio_drive(a), MS_OK, "drive 1200");
        CHECK_ST(ms_hw_audio_get_clock_us(a, &clock), MS_OK, "clock 1200 p1");
        CHECK(clock == 0, "first packet fully consumed by 1200 skip");
        au.skip_samples_start = 0;
        rel = 0;
        CHECK_ST(ms_hw_audio_submit_au(a, &au), MS_OK, "sub 1200 p2");
        CHECK_ST(ms_hw_audio_drive(a), MS_OK, "drive 1200 p2");
        CHECK_ST(ms_hw_audio_get_clock_us(a, &clock), MS_OK, "clock 1200 p2");
        CHECK(clock == (int64_t)((720ull * 1000000ull) / 48000ull), "remainder 240 of 1200");
    }

    /* 尾部 padding 648 */
    CHECK_ST(ms_hw_audio_flush(a), MS_OK, "flush end");
    {
        ms_hw_audio_au au;
        memset(&au, 0, sizeof(au));
        au.struct_size = sizeof(au);
        au.buf = &buf;
        au.skip_samples_end = 648;
        rel = 0;
        buf.release_ctx = &rel;
        CHECK_ST(ms_hw_audio_submit_au(a, &au), MS_OK, "sub end");
        CHECK_ST(ms_hw_audio_drive(a), MS_OK, "drive end");
        CHECK_ST(ms_hw_audio_get_clock_us(a, &clock), MS_OK, "clock end");
        CHECK(clock == (int64_t)((312ull * 1000000ull) / 48000ull), "end padding 648");
    }

    /* 中途 seek：80ms=3840 预热，不是再扣 312 */
    CHECK_ST(ms_hw_audio_flush(a), MS_OK, "flush mid");
    CHECK_ST(ms_hw_audio_note_seek(a, 500000), MS_OK, "mid seek");
    {
        int i;
        for (i = 0; i < 4; i++) {
            rel = 0;
            buf.release_ctx = &rel;
            CHECK_ST(ms_hw_audio_submit(a, &buf, 500000 + i * 20000, 0), MS_OK, "warmup sub");
            CHECK_ST(ms_hw_audio_drive(a), MS_OK, "warmup drive");
        }
        CHECK_ST(ms_hw_audio_get_clock_us(a, &clock), MS_OK, "clock warmup 4");
        CHECK(clock == 0, "3840 warmup swallows 4*960");
        rel = 0;
        buf.release_ctx = &rel;
        CHECK_ST(ms_hw_audio_submit(a, &buf, 580000, 0), MS_OK, "after warmup");
        CHECK_ST(ms_hw_audio_drive(a), MS_OK, "drive after warmup");
        CHECK_ST(ms_hw_audio_get_clock_us(a, &clock), MS_OK, "clock after warmup");
        CHECK(clock == full_us, "after 80ms preroll, full packet plays");
    }

    /* 溢出 skip 不绕回 */
    CHECK_ST(ms_hw_audio_flush(a), MS_OK, "flush ovf");
    {
        ms_hw_audio_au au;
        memset(&au, 0, sizeof(au));
        au.struct_size = sizeof(au);
        au.buf = &buf;
        au.skip_samples_start = 0xFFFFFFFFu;
        au.skip_samples_end = 0xFFFFFFFFu;
        rel = 0;
        buf.release_ctx = &rel;
        CHECK_ST(ms_hw_audio_submit_au(a, &au), MS_OK, "sub ovf");
        CHECK_ST(ms_hw_audio_drive(a), MS_OK, "drive ovf");
        CHECK_ST(ms_hw_audio_get_clock_us(a, &clock), MS_OK, "clock ovf");
        CHECK(clock == 0, "overflow skip clamped, no wrap");
    }

    /* pre-skip=0 的 Head：开始不扣样 */
    {
        uint8_t head0[19];
        memcpy(head0, k_opus_head, 19);
        head0[10] = 0;
        head0[11] = 0;
        cfg.asc = head0;
        cfg.asc_size = 19;
        cfg.pre_skip = 0;
        CHECK_ST(ms_hw_audio_configure(a, &cfg), MS_OK, "cfg pre0");
        CHECK_ST(ms_hw_audio_start(a), MS_OK, "start pre0");
        rel = 0;
        buf.release_ctx = &rel;
        CHECK_ST(ms_hw_audio_submit(a, &buf, 0, 0), MS_OK, "sub pre0");
        CHECK_ST(ms_hw_audio_drive(a), MS_OK, "drive pre0");
        CHECK_ST(ms_hw_audio_get_clock_us(a, &clock), MS_OK, "clock pre0");
        CHECK(clock == full_us, "pre_skip 0 plays full packet");
        cfg.asc = k_opus_head;
        cfg.asc_size = sizeof(k_opus_head);
        cfg.pre_skip = 0; /* 重新解析 Head 的 312 */
        CHECK_ST(ms_hw_audio_configure(a, &cfg), MS_OK, "cfg restore 312");
        CHECK_ST(ms_hw_audio_start(a), MS_OK, "start restore");
    }

    /* 部分写：修剪后配额小于一帧 */
    CHECK_ST(ms_hw_audio_flush(a), MS_OK, "flush quota");
    CHECK_ST(ms_hw_audio_note_seek(a, 0), MS_OK, "seek0 quota");
    {
        size_t bpf = 2 * 2; /* 16-bit stereo */
        CHECK_ST(ms_hw_audio_add_write_quota(a, 100 * bpf), MS_OK, "quota 100");
        rel = 0;
        buf.release_ctx = &rel;
        CHECK_ST(ms_hw_audio_submit(a, &buf, 0, 0), MS_OK, "sub quota");
        CHECK_ST(ms_hw_audio_drive(a), MS_OK, "drive quota");
        CHECK_ST(ms_hw_audio_get_stats(a, &st), MS_OK, "st quota");
        CHECK(st.partial_writes >= 1 || st.clock_us == 0, "partial or still pending");
        /* 补足配额，完成剩余 audible 648（若已部分推进则时钟单调不倒退） */
        CHECK_ST(ms_hw_audio_add_write_quota(a, 648 * bpf), MS_OK, "quota rest");
        CHECK_ST(ms_hw_audio_drive(a), MS_OK, "drive rest");
        CHECK_ST(ms_hw_audio_get_clock_us(a, &clock), MS_OK, "clock rest");
        CHECK(clock == first_us, "completed after partial write");
    }

    {
        unsigned char raw[128];
        ms_hw_audio_config *old = (ms_hw_audio_config *)(void *)raw;
        memset(raw, 0xA5, sizeof(raw));
        memset(old, 0, offsetof(ms_hw_audio_config, pre_skip));
        old->struct_size = offsetof(ms_hw_audio_config, pre_skip);
        old->codec = MS_CODEC_OPUS;
        old->sample_fmt = MS_SAMPLE_FMT_OPUS;
        old->asc = k_opus_head;
        old->asc_size = sizeof(k_opus_head);
        CHECK_ST(ms_hw_audio_configure(a, old), MS_OK, "old abi; pre_skip from Head");
        CHECK_ST(ms_hw_audio_flush(a), MS_OK, "flush old");
        rel = 0;
        buf.release_ctx = &rel;
        CHECK_ST(ms_hw_audio_start(a), MS_OK, "start old");
        CHECK_ST(ms_hw_audio_submit(a, &buf, 0, 0), MS_OK, "sub old");
        CHECK_ST(ms_hw_audio_drive(a), MS_OK, "drive old");
        CHECK_ST(ms_hw_audio_get_clock_us(a, &clock), MS_OK, "clock old");
        CHECK(clock == first_us, "old abi still prerolls from parsed Head");
    }
    ms_hw_audio_destroy(a);
    printf("  test_abit_opus_mock_configure_submit ok (host mock; Head/dOps; "
           "preroll; not device sound)\n");
    return 0;
}


int test_hw_audio_trim_unit(void);
int test_hw_audio_trim_via_mock(void);

int main(void)
{
    if (test_classify() != 0 || test_query_default() != 0 ||
        test_refuse_default_and_sw() != 0 || test_no_surface_and_bad_codec() != 0 ||
        test_hevc_mock_configure_submit() != 0 ||
        test_vbit_mock_configure_submit() != 0 ||
        test_abit_mock_configure_submit() != 0 ||
        test_abit_mpeg_mock_configure_submit() != 0 ||
        test_abit_opus_mock_configure_submit() != 0 ||
        test_hw_audio_trim_unit() != 0 ||
        test_hw_audio_trim_via_mock() != 0 ||
        test_lifecycle_submit_eos_flush() != 0 || test_bad_csd_and_ops() != 0 ||
        test_aac_asc_parse() != 0 || test_audio_refuse_and_lifecycle() != 0) {
        fprintf(stderr, "test_hw_backend failed (%d)\n", failures);
        return 1;
    }
    printf("test_hw_backend ok (host mock only; not device hardware decode)\n");
    return 0;
}
