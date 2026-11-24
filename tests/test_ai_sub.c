#define _DEFAULT_SOURCE
/*
 * P5 可选 AI 字幕：经已解码 PCM 修剪后 tap 的受控闭环测试。
 * 使用明确标示的本地假供应商（模拟数据），不是真实识别验收。
 * 禁止把手工直接塞 cue 当成闭环。
 * by: Long <mycjzb@gmail.com>
 */
#include "msmedia/ms_ai_sub.h"
#include "msmedia/ms_hw.h"
#include "ms_hw_audio_trim.h"
#include "ms_abit_fixtures.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int g_fail;

static void fail_msg(const char *msg)
{
    fprintf(stderr, "test_ai_sub FAIL: %s\n", msg);
    g_fail = 1;
}

static void rel_nop(void *ctx, uint8_t *data, size_t n)
{
    int *r = (int *)ctx;
    (void)data;
    (void)n;
    if (r != NULL) {
        *r = 1;
    }
}

static int wait_merged(ms_ai_sub *s, uint64_t want, int ms)
{
    int i;
    for (i = 0; i < ms; i++) {
        ms_ai_sub_stats st;
        memset(&st, 0, sizeof(st));
        st.struct_size = sizeof(st);
        (void)ms_ai_sub_drive(s);
        if (ms_ai_sub_get_stats(s, &st) == MS_OK && st.results_merged >= want) {
            return 0;
        }
        usleep(1000);
    }
    return -1;
}

static ms_status make_opus_hw(ms_hw_audio **out)
{
    ms_hw_create_opts o;
    ms_hw_audio_config cfg;
    ms_status st;
    memset(&o, 0, sizeof(o));
    o.struct_size = sizeof(o);
    o.mock_mode = MS_HW_MOCK_AS_HW;
    st = ms_hw_audio_create(NULL, NULL, &o, out);
    if (st != MS_OK) {
        return st;
    }
    memset(&cfg, 0, sizeof(cfg));
    cfg.struct_size = sizeof(cfg);
    cfg.codec = MS_CODEC_OPUS;
    cfg.sample_fmt = MS_SAMPLE_FMT_OPUS;
    cfg.sample_rate = 48000;
    cfg.channels = 2;
    cfg.asc = k_opus_head;
    cfg.asc_size = sizeof(k_opus_head);
    st = ms_hw_audio_configure(*out, &cfg);
    if (st != MS_OK) {
        ms_hw_audio_destroy(*out);
        *out = NULL;
        return st;
    }
    return ms_hw_audio_start(*out);
}

static ms_status sub_toc(ms_hw_audio *a, int64_t pts, int *rel)
{
    ms_hw_buf buf;
    memset(&buf, 0, sizeof(buf));
    buf.data = k_opus_toc20;
    buf.size = sizeof(k_opus_toc20);
    buf.release_fn = rel_nop;
    buf.release_ctx = rel;
    if (rel != NULL) {
        *rel = 0;
    }
    return ms_hw_audio_submit(a, &buf, pts, 0);
}

static void tap_to_ai(void *ctx, const uint8_t *pcm, size_t bytes, int rate, int ch,
                      int bits, int has_pts, int64_t pts_us)
{
    (void)ms_ai_sub_tap_pcm((ms_ai_sub *)ctx, pcm, bytes, rate, ch, bits, has_pts, pts_us);
}

static int test_pcm_helpers(void)
{
    int16_t stereo[8] = { 10, 10, 20, 20, 30, 30, 40, 40 };
    int16_t mono[4];
    int16_t rs[16];
    size_t n = 0, m = 0;
    if (ms_ai_pcm_s16_downmix_mono(stereo, 2, 4, mono, &n) != MS_OK || n != 4 ||
        mono[0] != 10 || mono[3] != 40) {
        fail_msg("downmix");
        return -1;
    }
    /* 48000 -> 16000：4 点线性重采样应得 2 点（覆盖首末）。 */
    if (ms_ai_pcm_s16_resample_linear(mono, 4, 48000, 16000, rs, 16, &m) != MS_OK ||
        m != 2 || rs[0] != 10) {
        fprintf(stderr, "  resample n=%zu rs0=%d\n", m, (int)rs[0]);
        fail_msg("resample");
        return -1;
    }
    if (ms_ai_pcm_s16_resample_linear(mono, 4, 16000, 16000, rs, 16, &m) != MS_OK ||
        m != 4 || rs[2] != 30) {
        fail_msg("resample same rate");
        return -1;
    }
    return 0;
}

static int test_disabled_idle(void)
{
    ms_ai_sub *s = NULL;
    ms_ai_sub_opts o;
    ms_ai_sub_stats st;
    uint8_t pcm[32];
    memset(pcm, 1, sizeof(pcm));
    ms_ai_sub_opts_default(&o);
    if (ms_ai_sub_create(NULL, &o, &s) != MS_OK || s == NULL) {
        fail_msg("create idle");
        return -1;
    }
    if (ms_ai_sub_set_enabled(s, 1) == MS_OK) {
        fail_msg("enable without vendor");
        ms_ai_sub_destroy(s);
        return -1;
    }
    memset(&st, 0, sizeof(st));
    st.struct_size = sizeof(st);
    if (ms_ai_sub_get_stats(s, &st) != MS_OK || st.enabled || st.worker_running ||
        st.pcm_taps != 0) {
        fail_msg("idle stats");
        ms_ai_sub_destroy(s);
        return -1;
    }
    if (ms_ai_sub_tap_pcm(s, pcm, sizeof(pcm), 16000, 1, 16, 1, 0) != MS_OK) {
        fail_msg("tap disabled");
        ms_ai_sub_destroy(s);
        return -1;
    }
    if (ms_ai_sub_get_stats(s, &st) != MS_OK || st.pcm_taps != 0 || st.worker_running) {
        fail_msg("disabled still tapped");
        ms_ai_sub_destroy(s);
        return -1;
    }
    ms_ai_sub_destroy(s);
    return 0;
}

static int closed_loop_and_merge(void)
{
    ms_hw_audio *a = NULL;
    ms_ai_sub *s = NULL;
    ms_ai_sub_opts o;
    ms_ai_sub_stats st;
    int16_t sink[2048];
    size_t n = 0;
    int rel = 0, rel2 = 0;
    size_t count = 0;
    ms_sub_cue cues[8];
    int i, got_p = 0, got_f = 0, got_tr = 0;
    const char *txt;

    ms_ai_sub_opts_default(&o);
    o.want_translation = 1;
    snprintf(o.target_lang, sizeof(o.target_lang), "en");
    if (ms_ai_sub_create(NULL, &o, &s) != MS_OK ||
        ms_ai_sub_use_fake_local(s) != MS_OK ||
        ms_ai_sub_set_enabled(s, 1) != MS_OK) {
        fail_msg("enable fake");
        ms_ai_sub_destroy(s);
        return -1;
    }
    if (make_opus_hw(&a) != MS_OK) {
        fail_msg("audio create");
        ms_ai_sub_destroy(s);
        return -1;
    }
    ms_hw_audio_debug_set_pcm_sink(a, sink, 2048, &n);
    ms_hw_audio_set_pcm_tap(a, tap_to_ai, s);
    if (sub_toc(a, 0, &rel) != MS_OK || ms_hw_audio_drive(a) != MS_OK) {
        fail_msg("drive audio");
        ms_hw_audio_destroy(a);
        ms_ai_sub_destroy(s);
        return -1;
    }
    if (n != 648) {
        fprintf(stderr, "  audible n=%zu want 648\n", n);
        fail_msg("audible count");
        ms_hw_audio_destroy(a);
        ms_ai_sub_destroy(s);
        return -1;
    }
    if (sink[0] != 312) {
        fprintf(stderr, "  first sample %d want 312\n", (int)sink[0]);
        fail_msg("first sample");
        ms_hw_audio_destroy(a);
        ms_ai_sub_destroy(s);
        return -1;
    }
    memset(&st, 0, sizeof(st));
    st.struct_size = sizeof(st);
    if (ms_ai_sub_get_stats(s, &st) != MS_OK || st.pcm_taps == 0 ||
        st.last_tap_first != 312 || !st.last_tap_has_pts ||
        st.last_tap_pts_us != 6500 || st.last_tap_rate != 48000 ||
        st.last_tap_ch != 2) {
        fprintf(stderr, "  tap first=%d pts=%lld rate=%d ch=%d taps=%llu\n",
                (int)st.last_tap_first, (long long)st.last_tap_pts_us,
                st.last_tap_rate, st.last_tap_ch,
                (unsigned long long)st.pcm_taps);
        fail_msg("tap content/pts");
        ms_hw_audio_destroy(a);
        ms_ai_sub_destroy(s);
        return -1;
    }
    if (wait_merged(s, 2, 500) != 0) {
        fail_msg("wait merge");
        ms_hw_audio_destroy(a);
        ms_ai_sub_destroy(s);
        return -1;
    }
    memset(cues, 0, sizeof(cues));
    for (i = 0; i < 8; i++) {
        cues[i].struct_size = sizeof(cues[i]);
    }
    /* 可听起点 6500us = 585/90k，持续约 13.5ms。 */
    if (ms_sub_doc_cues_at(ms_ai_sub_doc(s), 900, cues, 8, &count) != MS_OK ||
        count == 0) {
        fail_msg("cues_at");
        ms_hw_audio_destroy(a);
        ms_ai_sub_destroy(s);
        return -1;
    }
    for (i = 0; i < (int)count; i++) {
        txt = cues[i].text ? cues[i].text : "";
        if (strstr(txt, "pcm0=312") == NULL || strstr(txt, "pts_us=6500") == NULL) {
            fprintf(stderr, "  cue %d text=%s\n", i, txt);
            fail_msg("cue missing pcm/pts (must come from audio path)");
            ms_hw_audio_destroy(a);
            ms_ai_sub_destroy(s);
            return -1;
        }
        if ((cues[i].cap_flags & MS_SUB_CAP_AI_SIMULATED) == 0 ||
            cues[i].track_id != MS_TRACK_ID_AI_SUB) {
            fail_msg("cue caps");
            ms_hw_audio_destroy(a);
            ms_ai_sub_destroy(s);
            return -1;
        }
        if (cues[i].cap_flags & MS_SUB_CAP_AI_PARTIAL) {
            got_p = 1;
        }
        if (cues[i].cap_flags & MS_SUB_CAP_AI_FINAL) {
            got_f = 1;
        }
        if (cues[i].cap_flags & MS_SUB_CAP_AI_TRANSLATION) {
            got_tr = 1;
        }
        if (strstr(txt, "SIM-F") != NULL) {
            got_f = 1;
        }
        if (strstr(txt, "SIM-P") != NULL) {
            got_p = 1;
        }
        if (strstr(txt, "SIM-TR") != NULL) {
            got_tr = 1;
        }
    }
    /* 同 utterance 最终应保留 final（partial 被修订），翻译为并列 cue。 */
    if (!got_f || !got_tr) {
        fprintf(stderr, "  flags p=%d f=%d tr=%d count=%zu\n", got_p, got_f, got_tr, count);
        fail_msg("final/translation merge");
        ms_hw_audio_destroy(a);
        ms_ai_sub_destroy(s);
        return -1;
    }
    (void)got_p;
    /* pause：drive 立即返回，不得再解码/修剪/tap。 */
    if (ms_hw_audio_pause(a) != MS_OK) {
        fail_msg("pause");
        ms_hw_audio_destroy(a);
        ms_ai_sub_destroy(s);
        return -1;
    }
    {
        uint64_t taps = st.pcm_taps;
        if (ms_hw_audio_drive(a) != MS_OK) {
            fail_msg("drive paused");
            ms_hw_audio_destroy(a);
            ms_ai_sub_destroy(s);
            return -1;
        }
        memset(&st, 0, sizeof(st));
        st.struct_size = sizeof(st);
        (void)ms_ai_sub_get_stats(s, &st);
        if (st.pcm_taps != taps) {
            fail_msg("pause still tapped");
            ms_hw_audio_destroy(a);
            ms_ai_sub_destroy(s);
            return -1;
        }
    }
    if (ms_hw_audio_resume(a) != MS_OK ||
        ms_hw_audio_set_rate(a, 2, 1) != MS_OK) {
        fail_msg("resume/rate");
        ms_hw_audio_destroy(a);
        ms_ai_sub_destroy(s);
        return -1;
    }
    {
        uint64_t taps = st.pcm_taps;
        rel2 = 0;
        if (sub_toc(a, 20000, &rel2) != MS_OK || ms_hw_audio_drive(a) != MS_OK) {
            fail_msg("drive rate");
            ms_hw_audio_destroy(a);
            ms_ai_sub_destroy(s);
            return -1;
        }
        memset(&st, 0, sizeof(st));
        st.struct_size = sizeof(st);
        (void)ms_ai_sub_get_stats(s, &st);
        if (st.pcm_taps <= taps) {
            fail_msg("rate produced no tap");
            ms_hw_audio_destroy(a);
            ms_ai_sub_destroy(s);
            return -1;
        }
        /* 媒体 PTS 不因倍速改写 */
        if (!st.last_tap_has_pts || st.last_tap_pts_us != 20000) {
            fprintf(stderr, "  rate pts=%lld\n", (long long)st.last_tap_pts_us);
            fail_msg("rate pts");
            ms_hw_audio_destroy(a);
            ms_ai_sub_destroy(s);
            return -1;
        }
    }
    /* disable：不再采集，工作线程退出 */
    if (ms_ai_sub_set_enabled(s, 0) != MS_OK) {
        fail_msg("disable");
        ms_hw_audio_destroy(a);
        ms_ai_sub_destroy(s);
        return -1;
    }
    {
        uint64_t taps;
        rel2 = 0;
        memset(&st, 0, sizeof(st));
        st.struct_size = sizeof(st);
        (void)ms_ai_sub_get_stats(s, &st);
        if (st.enabled || st.worker_running) {
            fail_msg("worker still up");
            ms_hw_audio_destroy(a);
            ms_ai_sub_destroy(s);
            return -1;
        }
        taps = st.pcm_taps;
        if (sub_toc(a, 40000, &rel2) != MS_OK || ms_hw_audio_drive(a) != MS_OK) {
            fail_msg("drive disabled");
            ms_hw_audio_destroy(a);
            ms_ai_sub_destroy(s);
            return -1;
        }
        (void)ms_ai_sub_get_stats(s, &st);
        if (st.pcm_taps != taps) {
            fail_msg("disable still tapped");
            ms_hw_audio_destroy(a);
            ms_ai_sub_destroy(s);
            return -1;
        }
    }
    ms_hw_audio_destroy(a);
    ms_ai_sub_destroy(s);
    return 0;
}

static int test_policies(void)
{
    ms_ai_sub *s = NULL;
    ms_ai_sub_opts o;
    ms_ai_plugin_vt vt;
    ms_ai_fake_opts fo;
    ms_ai_sub_stats st;
    int16_t pcm[160];
    unsigned i;

    for (i = 0; i < 160; i++) {
        pcm[i] = (int16_t)(100 + i);
    }

    /* 乱序：先 final 后 partial，不得降级 */
    ms_ai_sub_opts_default(&o);
    if (ms_ai_sub_create(NULL, &o, &s) != MS_OK) {
        fail_msg("create reorder");
        return -1;
    }
    ms_ai_fake_opts_default(&fo);
    fo.reorder = 1;
    fo.emit_translation = 0;
    if (ms_ai_fake_vendor_init(NULL, &fo, &vt) != MS_OK ||
        ms_ai_sub_set_plugin(s, &vt) != MS_OK ||
        ms_ai_sub_set_enabled(s, 1) != MS_OK) {
        fail_msg("plugin reorder");
        ms_ai_sub_destroy(s);
        ms_ai_fake_vendor_free(&vt);
        return -1;
    }
    if (ms_ai_sub_tap_pcm(s, (const uint8_t *)pcm, sizeof(pcm), 16000, 1, 16, 1,
                          1000) != MS_OK ||
        wait_merged(s, 1, 500) != 0) {
        fail_msg("reorder merge");
        ms_ai_sub_set_enabled(s, 0);
        ms_ai_sub_destroy(s);
        ms_ai_fake_vendor_free(&vt);
        return -1;
    }
    {
        ms_sub_cue cues[4];
        size_t count = 0;
        int k, saw_f = 0, saw_only_p = 0;
        memset(cues, 0, sizeof(cues));
        for (k = 0; k < 4; k++) {
            cues[k].struct_size = sizeof(cues[k]);
        }
        (void)ms_sub_doc_cues_at(ms_ai_sub_doc(s), 90, cues, 4, &count);
        for (k = 0; k < (int)count; k++) {
            const char *tx = cues[k].text ? cues[k].text : "";
            if (strstr(tx, "SIM-F") != NULL) {
                saw_f = 1;
            }
            if (strstr(tx, "SIM-P") != NULL && strstr(tx, "SIM-F") == NULL) {
                saw_only_p = 1;
            }
        }
        if (!saw_f || saw_only_p) {
            fail_msg("reorder demoted final");
            ms_ai_sub_set_enabled(s, 0);
            ms_ai_sub_destroy(s);
            ms_ai_fake_vendor_free(&vt);
            return -1;
        }
    }
    /* 迟到：滞留结果在 bump timeline 后必须丢弃 */
    ms_ai_sub_set_enabled(s, 0);
    ms_ai_sub_destroy(s);
    ms_ai_fake_vendor_free(&vt);

    ms_ai_sub_opts_default(&o);
    if (ms_ai_sub_create(NULL, &o, &s) != MS_OK) {
        fail_msg("create late");
        return -1;
    }
    ms_ai_fake_opts_default(&fo);
    fo.hold_polls = 8;
    fo.ignore_cancel = 1;
    if (ms_ai_fake_vendor_init(NULL, &fo, &vt) != MS_OK ||
        ms_ai_sub_set_plugin(s, &vt) != MS_OK ||
        ms_ai_sub_set_enabled(s, 1) != MS_OK) {
        fail_msg("plugin late");
        ms_ai_sub_destroy(s);
        ms_ai_fake_vendor_free(&vt);
        return -1;
    }
    if (ms_ai_sub_tap_pcm(s, (const uint8_t *)pcm, sizeof(pcm), 16000, 1, 16, 1,
                          0) != MS_OK) {
        fail_msg("tap late");
        ms_ai_sub_set_enabled(s, 0);
        ms_ai_sub_destroy(s);
        ms_ai_fake_vendor_free(&vt);
        return -1;
    }
    usleep(20000);
    (void)ms_ai_sub_drive(s);
    ms_ai_sub_bump_timeline(s);
    {
        int k;
        for (k = 0; k < 16; k++) {
            (void)ms_ai_sub_drive(s);
        }
    }
    memset(&st, 0, sizeof(st));
    st.struct_size = sizeof(st);
    (void)ms_ai_sub_get_stats(s, &st);
    if (st.results_dropped_epoch == 0) {
        fail_msg("late epoch not dropped");
        ms_ai_sub_set_enabled(s, 0);
        ms_ai_sub_destroy(s);
        ms_ai_fake_vendor_free(&vt);
        return -1;
    }
    if (ms_sub_doc_count(ms_ai_sub_doc(s)) != 0) {
        fail_msg("late polluted doc");
        ms_ai_sub_set_enabled(s, 0);
        ms_ai_sub_destroy(s);
        ms_ai_fake_vendor_free(&vt);
        return -1;
    }
    ms_ai_sub_set_enabled(s, 0);
    ms_ai_sub_destroy(s);
    ms_ai_fake_vendor_free(&vt);

    /* 取消 / flush */
    ms_ai_sub_opts_default(&o);
    if (ms_ai_sub_create(NULL, &o, &s) != MS_OK ||
        ms_ai_sub_use_fake_local(s) != MS_OK ||
        ms_ai_sub_set_enabled(s, 1) != MS_OK) {
        fail_msg("create flush");
        ms_ai_sub_destroy(s);
        return -1;
    }
    (void)ms_ai_sub_tap_pcm(s, (const uint8_t *)pcm, sizeof(pcm), 16000, 1, 16, 1, 0);
    usleep(5000);
    ms_ai_sub_flush(s);
    (void)ms_ai_sub_drive(s);
    if (ms_sub_doc_count(ms_ai_sub_doc(s)) != 0) {
        fail_msg("flush left cues");
        ms_ai_sub_destroy(s);
        return -1;
    }
    ms_ai_sub_destroy(s);

    /* 错误 */
    ms_ai_sub_opts_default(&o);
    if (ms_ai_sub_create(NULL, &o, &s) != MS_OK) {
        fail_msg("create err");
        return -1;
    }
    ms_ai_fake_opts_default(&fo);
    fo.force_error = 1;
    if (ms_ai_fake_vendor_init(NULL, &fo, &vt) != MS_OK ||
        ms_ai_sub_set_plugin(s, &vt) != MS_OK ||
        ms_ai_sub_set_enabled(s, 1) != MS_OK) {
        fail_msg("plugin err");
        ms_ai_sub_destroy(s);
        ms_ai_fake_vendor_free(&vt);
        return -1;
    }
    (void)ms_ai_sub_tap_pcm(s, (const uint8_t *)pcm, sizeof(pcm), 16000, 1, 16, 1, 0);
    if (wait_merged(s, 1, 80) == 0) {
        fail_msg("error produced merge");
        ms_ai_sub_set_enabled(s, 0);
        ms_ai_sub_destroy(s);
        ms_ai_fake_vendor_free(&vt);
        return -1;
    }
    memset(&st, 0, sizeof(st));
    st.struct_size = sizeof(st);
    (void)ms_ai_sub_get_stats(s, &st);
    if (st.plugin_errors == 0) {
        fail_msg("plugin error not counted");
        ms_ai_sub_set_enabled(s, 0);
        ms_ai_sub_destroy(s);
        ms_ai_fake_vendor_free(&vt);
        return -1;
    }
    ms_ai_sub_set_enabled(s, 0);
    ms_ai_sub_destroy(s);
    ms_ai_fake_vendor_free(&vt);

    /* 超长 */
    ms_ai_sub_opts_default(&o);
    if (ms_ai_sub_create(NULL, &o, &s) != MS_OK) {
        fail_msg("create oversize");
        return -1;
    }
    ms_ai_fake_opts_default(&fo);
    fo.force_oversize = 1;
    if (ms_ai_fake_vendor_init(NULL, &fo, &vt) != MS_OK ||
        ms_ai_sub_set_plugin(s, &vt) != MS_OK ||
        ms_ai_sub_set_enabled(s, 1) != MS_OK) {
        fail_msg("plugin oversize");
        ms_ai_sub_destroy(s);
        ms_ai_fake_vendor_free(&vt);
        return -1;
    }
    (void)ms_ai_sub_tap_pcm(s, (const uint8_t *)pcm, sizeof(pcm), 16000, 1, 16, 1, 0);
    usleep(20000);
    (void)ms_ai_sub_drive(s);
    memset(&st, 0, sizeof(st));
    st.struct_size = sizeof(st);
    (void)ms_ai_sub_get_stats(s, &st);
    if (st.results_dropped_oversize == 0) {
        fail_msg("oversize not dropped");
        ms_ai_sub_set_enabled(s, 0);
        ms_ai_sub_destroy(s);
        ms_ai_fake_vendor_free(&vt);
        return -1;
    }
    ms_ai_sub_set_enabled(s, 0);
    ms_ai_sub_destroy(s);
    ms_ai_fake_vendor_free(&vt);

    /* 队列上限 */
    ms_ai_sub_opts_default(&o);
    o.queue_chunks_max = 2;
    o.queue_bytes_max = 1024;
    if (ms_ai_sub_create(NULL, &o, &s) != MS_OK) {
        fail_msg("create q");
        return -1;
    }
    ms_ai_fake_opts_default(&fo);
    fo.hold_polls = 0;
    fo.emit_partial = 0;
    fo.emit_final = 0;
    if (ms_ai_fake_vendor_init(NULL, &fo, &vt) != MS_OK ||
        ms_ai_sub_set_plugin(s, &vt) != MS_OK ||
        ms_ai_sub_set_enabled(s, 1) != MS_OK) {
        fail_msg("plugin q");
        ms_ai_sub_destroy(s);
        ms_ai_fake_vendor_free(&vt);
        return -1;
    }
    for (i = 0; i < 40; i++) {
        (void)ms_ai_sub_tap_pcm(s, (const uint8_t *)pcm, sizeof(pcm), 16000, 1, 16, 1, 0);
    }
    memset(&st, 0, sizeof(st));
    st.struct_size = sizeof(st);
    (void)ms_ai_sub_get_stats(s, &st);
    if (st.queue_drops == 0) {
        fail_msg("queue cap no drop");
        ms_ai_sub_set_enabled(s, 0);
        ms_ai_sub_destroy(s);
        ms_ai_fake_vendor_free(&vt);
        return -1;
    }
    ms_ai_sub_set_enabled(s, 0);
    ms_ai_sub_destroy(s);
    ms_ai_fake_vendor_free(&vt);

    /* 切音轨 epoch */
    ms_ai_sub_opts_default(&o);
    if (ms_ai_sub_create(NULL, &o, &s) != MS_OK ||
        ms_ai_sub_use_fake_local(s) != MS_OK ||
        ms_ai_sub_set_enabled(s, 1) != MS_OK) {
        fail_msg("create sel");
        ms_ai_sub_destroy(s);
        return -1;
    }
    ms_ai_sub_bump_selection(s);
    (void)ms_ai_sub_tap_pcm(s, (const uint8_t *)pcm, sizeof(pcm), 16000, 1, 16, 1, 5000);
    if (wait_merged(s, 1, 500) != 0) {
        fail_msg("sel merge");
        ms_ai_sub_destroy(s);
        return -1;
    }
    ms_ai_sub_destroy(s);
    return 0;
}

static int test_player_api(void)
{
    ms_hw_player *p = NULL;
    ms_hw_player_opts o;
    ms_status st;
    const char *s;
    memset(&o, 0, sizeof(o));
    ms_hw_player_opts_default(&o);
    o.audio_mock_mode = MS_HW_MOCK_AS_HW;
    if (ms_hw_player_create(NULL, &o, &p) != MS_OK || p == NULL) {
        fail_msg("player create");
        return -1;
    }
    s = ms_hw_player_ai_status(p);
    if (s == NULL) {
        fail_msg("status null");
        ms_hw_player_destroy(p);
        return -1;
    }
    if (ms_hw_player_ai_enabled(p)) {
        fail_msg("default enabled");
        ms_hw_player_destroy(p);
        return -1;
    }
    st = ms_hw_player_ai_configure(p, 99, NULL, NULL);
    if (st != MS_ERR_UNSUPPORTED) {
        fail_msg("unknown vendor accepted");
        ms_hw_player_destroy(p);
        return -1;
    }
    /* 模块若已构建，可配置假供应商；未打包则 UNSUPPORTED，不算失败。 */
    st = ms_hw_player_ai_configure(p, MS_AI_VENDOR_FAKE_LOCAL, "", "en");
    if (st == MS_OK) {
        if (ms_hw_player_ai_set_enabled(p, 1) != MS_OK) {
            fail_msg("player enable fake");
            ms_hw_player_destroy(p);
            return -1;
        }
        s = ms_hw_player_ai_status(p);
        if (s == NULL || strstr(s, "simulated") == NULL || strstr(s, "not real") == NULL) {
            fprintf(stderr, "  status=%s\n", s ? s : "");
            fail_msg("status must say simulated");
            ms_hw_player_destroy(p);
            return -1;
        }
        if (strstr(s, "whisper") != NULL || strstr(s, "OpenAI") != NULL ||
            strstr(s, "Google") != NULL) {
            fail_msg("status names a real vendor");
            ms_hw_player_destroy(p);
            return -1;
        }
        if (ms_hw_player_select_track(p, MS_TRACK_ID_AI_SUB, 1) != MS_OK) {
            fail_msg("select ai track");
            ms_hw_player_destroy(p);
            return -1;
        }
        (void)ms_hw_player_ai_set_enabled(p, 0);
    }
    ms_hw_player_destroy(p);
    return 0;
}

int main(void)
{
    if (test_pcm_helpers() != 0) {
        return 1;
    }
    if (test_disabled_idle() != 0) {
        return 1;
    }
    if (closed_loop_and_merge() != 0) {
        return 1;
    }
    if (test_policies() != 0) {
        return 1;
    }
    if (test_player_api() != 0) {
        return 1;
    }
    if (g_fail) {
        return 1;
    }
    fprintf(stderr, "test_ai_sub: PASS (simulated fake vendor only; not real ASR)\n");
    return 0;
}
