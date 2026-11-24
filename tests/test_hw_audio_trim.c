/*
 * PCM trim carry 反例与回归：1 AU->多块输出、多 AU->一块输出、
 * 空/延迟/EOS 输出，以及经 mock 的部分写。校验写出样点总数与边界内容
 * （递增样点）。宿主模型不是真机通过。
 * by: Long <mycjzb@gmail.com>
 */
#include "ms_hw_audio_trim.h"
#include "msmedia/ms_hw.h"
#include "ms_abit_fixtures.h"

#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

int test_hw_audio_trim_unit(void);
int test_hw_audio_trim_via_mock(void);

static int g_fail;

static void fail_msg(const char *msg)
{
    fprintf(stderr, "test_hw_audio_trim FAIL: %s\n", msg);
    g_fail = 1;
}

static void gen_inc(int16_t *pcm, uint32_t nsamp, int ch, uint32_t start)
{
    uint32_t i;
    int c;
    for (i = 0; i < nsamp; i++) {
        int16_t v = (int16_t)((start + i) & 0xFFFF);
        for (c = 0; c < ch; c++) {
            pcm[(size_t)i * (size_t)ch + (size_t)c] = v;
        }
    }
}

static int check_inc(const int16_t *pcm, uint32_t nsamp, int ch,
                     uint32_t first, uint32_t last, const char *tag)
{
    uint32_t i;
    int c;
    if (nsamp == 0) {
        if (g_fail == 0 && first != 0) {
            /* 允许 0 样点 */
        }
        return 0;
    }
    if ((uint32_t)(int16_t)(first & 0xFFFF) != (uint32_t)(uint16_t)pcm[0] ||
        (uint32_t)(int16_t)(last & 0xFFFF) != (uint32_t)(uint16_t)pcm[(nsamp - 1) * (unsigned)ch]) {
        fprintf(stderr, "  %s bounds got first=%d last=%d want %u..%u n=%u\n",
                tag, (int)pcm[0], (int)pcm[(nsamp - 1) * (unsigned)ch],
                first, last, nsamp);
        fail_msg(tag);
        return -1;
    }
    for (i = 0; i < nsamp; i++) {
        int16_t want = (int16_t)((first + i) & 0xFFFF);
        for (c = 0; c < ch; c++) {
            if (pcm[(size_t)i * (size_t)ch + (size_t)c] != want) {
                fprintf(stderr, "  %s sample %u ch %d got %d want %d\n",
                        tag, i, c, (int)pcm[(size_t)i * (size_t)ch + (size_t)c],
                        (int)want);
                fail_msg(tag);
                return -1;
            }
        }
    }
    (void)last;
    return 0;
}

static ms_hw_pcm_trim_meta meta_s(uint32_t start, uint32_t end, uint32_t exp,
                                 int has_pts, int64_t pts)
{
    ms_hw_pcm_trim_meta m;
    memset(&m, 0, sizeof(m));
    m.struct_size = sizeof(m);
    m.skip_start = start;
    m.skip_end = end;
    m.expected_samples = exp;
    m.has_pts = has_pts;
    m.pts_us = pts;
    return m;
}

/* 若按 1:1 对每块输出 pop 一条 meta，本用例会把 648 end 扣在第一块 400 上。 */
static int test_split_one_au(void)
{
    ms_hw_pcm_trim t;
    int16_t in[960];
    int16_t out[960];
    uint8_t *dst = (uint8_t *)out;
    size_t n1 = 0, n2 = 0;
    uint32_t s1, s2;
    ms_hw_pcm_trim_meta m = meta_s(312, 0, 960, 0, 0);
    ms_hw_pcm_trim_init(&t);
    if (ms_hw_pcm_trim_set_bpf(&t, 2) != MS_OK) {
        fail_msg("split bpf");
        return -1;
    }
    if (ms_hw_pcm_trim_push(&t, &m) != MS_OK) {
        fail_msg("split push");
        return -1;
    }
    gen_inc(in, 960, 1, 0);
    if (ms_hw_pcm_trim_feed(&t, (uint8_t *)in, 400, 0, 0, dst, sizeof(out), &n1) != MS_OK) {
        fail_msg("split feed1");
        return -1;
    }
    s1 = (uint32_t)(n1 / 2);
    if (ms_hw_pcm_trim_feed(&t, (uint8_t *)(in + 400), 560, 0, 0,
                            dst + n1, sizeof(out) - n1, &n2) != MS_OK) {
        fail_msg("split feed2");
        return -1;
    }
    s2 = (uint32_t)(n2 / 2);
    if (s1 + s2 != 648) {
        fprintf(stderr, "  split got %u+%u want 648\n", s1, s2);
        fail_msg("split total");
        return -1;
    }
    /* 第一块 0..399 去掉 312，可听 312..399（88）；第二块 400..959 */
    if (s1 != 88 || check_inc(out, s1, 1, 312, 399, "split part1") != 0) {
        return -1;
    }
    if (s2 != 560 || check_inc(out + s1, s2, 1, 400, 959, "split part2") != 0) {
        return -1;
    }
    if (ms_hw_pcm_trim_pending_meta(&t) != 0 || ms_hw_pcm_trim_hold_samples(&t) != 0) {
        fail_msg("split leftover");
        return -1;
    }
    return 0;
}

/* 两包合成一块：若 pop 一次，第二包 648 end 会丢失或错扣到整块。 */
static int test_merge_two_au(void)
{
    ms_hw_pcm_trim t;
    int16_t in[1920];
    int16_t out[1920];
    size_t n = 0;
    uint32_t s;
    ms_hw_pcm_trim_meta a = meta_s(312, 0, 960, 1, 0);
    ms_hw_pcm_trim_meta b = meta_s(0, 648, 960, 1, 20000);
    ms_hw_pcm_trim_init(&t);
    (void)ms_hw_pcm_trim_set_bpf(&t, 2);
    if (ms_hw_pcm_trim_push(&t, &a) != MS_OK ||
        ms_hw_pcm_trim_push(&t, &b) != MS_OK) {
        fail_msg("merge push");
        return -1;
    }
    gen_inc(in, 1920, 1, 0);
    if (ms_hw_pcm_trim_feed(&t, (uint8_t *)in, 1920, 1, 0,
                            (uint8_t *)out, sizeof(out), &n) != MS_OK) {
        fail_msg("merge feed");
        return -1;
    }
    s = (uint32_t)(n / 2);
    /* AU1: 312..959 (648)；AU2: 960..1271 (312)；合计 960 */
    if (s != 960) {
        fprintf(stderr, "  merge got %u want 960\n", s);
        fail_msg("merge total");
        return -1;
    }
    if (check_inc(out, 648, 1, 312, 959, "merge au1") != 0) {
        return -1;
    }
    if (check_inc(out + 648, 312, 1, 960, 1271, "merge au2") != 0) {
        return -1;
    }
    return 0;
}

static int test_empty_then_data(void)
{
    ms_hw_pcm_trim t;
    int16_t in[960];
    int16_t out[960];
    size_t n0 = 99, n = 0;
    ms_hw_pcm_trim_meta m = meta_s(312, 648, 960, 0, 0);
    ms_hw_pcm_trim_init(&t);
    (void)ms_hw_pcm_trim_set_bpf(&t, 2);
    (void)ms_hw_pcm_trim_push(&t, &m);
    if (ms_hw_pcm_trim_feed(&t, (uint8_t *)in, 0, 0, 0,
                            (uint8_t *)out, sizeof(out), &n0) != MS_OK || n0 != 0) {
        fail_msg("empty output popped or wrote");
        return -1;
    }
    if (ms_hw_pcm_trim_pending_meta(&t) != 1) {
        fail_msg("empty must not pop meta");
        return -1;
    }
    gen_inc(in, 960, 1, 0);
    if (ms_hw_pcm_trim_feed(&t, (uint8_t *)in, 960, 0, 0,
                            (uint8_t *)out, sizeof(out), &n) != MS_OK) {
        fail_msg("empty then data");
        return -1;
    }
    /* 960-312-648=0 */
    if (n != 0) {
        fprintf(stderr, "  empty-then 312+648 got %zu bytes\n", n);
        fail_msg("all trimmed");
        return -1;
    }
    return 0;
}

static int test_delay_and_eos_unknown(void)
{
    ms_hw_pcm_trim t;
    int16_t in[960];
    int16_t out[960];
    size_t n = 0, ne = 0;
    uint32_t s, se;
    /* 未知 expected：先 hold 尾 648，EOS 再丢尾。 */
    ms_hw_pcm_trim_meta m = meta_s(0, 648, 0, 0, 0);
    ms_hw_pcm_trim_init(&t);
    (void)ms_hw_pcm_trim_set_bpf(&t, 2);
    (void)ms_hw_pcm_trim_push(&t, &m);
    /* 延迟：push 后暂不 feed */
    if (ms_hw_pcm_trim_pending_meta(&t) != 1 ||
        ms_hw_pcm_trim_hold_samples(&t) != 0) {
        fail_msg("delay pending");
        return -1;
    }
    gen_inc(in, 960, 1, 0);
    if (ms_hw_pcm_trim_feed(&t, (uint8_t *)in, 960, 0, 0,
                            (uint8_t *)out, sizeof(out), &n) != MS_OK) {
        fail_msg("delay feed");
        return -1;
    }
    s = (uint32_t)(n / 2);
    /* 滚动 hold：可先放出 960-648=312，尾 648 仍 hold */
    if (s != 312 || ms_hw_pcm_trim_hold_samples(&t) != 648) {
        fprintf(stderr, "  unknown hold got emit=%u hold=%u\n",
                s, ms_hw_pcm_trim_hold_samples(&t));
        fail_msg("unknown rolling hold");
        return -1;
    }
    if (check_inc(out, 312, 1, 0, 311, "delay body") != 0) {
        return -1;
    }
    if (ms_hw_pcm_trim_eos(&t, (uint8_t *)out + n, sizeof(out) - n, &ne) != MS_OK) {
        fail_msg("eos");
        return -1;
    }
    se = (uint32_t)(ne / 2);
    if (se != 0 || ms_hw_pcm_trim_hold_samples(&t) != 0) {
        fprintf(stderr, "  eos must drop tail, got %u\n", se);
        fail_msg("eos drops padding");
        return -1;
    }
    return 0;
}

static int test_prefix_warmup_3840(void)
{
    ms_hw_pcm_trim t;
    int16_t in[960];
    int16_t out[960];
    size_t n;
    uint32_t total = 0;
    int i;
    ms_hw_pcm_trim_init(&t);
    (void)ms_hw_pcm_trim_set_bpf(&t, 2);
    if (ms_hw_pcm_trim_add_prefix(&t, 3840) != MS_OK) {
        fail_msg("prefix");
        return -1;
    }
    for (i = 0; i < 5; i++) {
        ms_hw_pcm_trim_meta m = meta_s(0, 0, 960, 0, 0);
        (void)ms_hw_pcm_trim_push(&t, &m);
        gen_inc(in, 960, 1, (uint32_t)i * 960u);
        n = 0;
        if (ms_hw_pcm_trim_feed(&t, (uint8_t *)in, 960, 0, 0,
                                (uint8_t *)out, sizeof(out), &n) != MS_OK) {
            fail_msg("warmup feed");
            return -1;
        }
        total += (uint32_t)(n / 2);
    }
    /* 前 4 包被 3840 吞掉，第 5 包整包可听 */
    if (total != 960) {
        fprintf(stderr, "  warmup total %u want 960\n", total);
        fail_msg("3840 warmup");
        return -1;
    }
    if (check_inc(out, 960, 1, 3840, 4799, "warmup last") != 0) {
        return -1;
    }
    return 0;
}

static int test_multipacket_start(void)
{
    ms_hw_pcm_trim t;
    int16_t in[960];
    int16_t out[960];
    size_t n1 = 0, n2 = 0;
    uint32_t s1, s2;
    ms_hw_pcm_trim_meta a = meta_s(1200, 0, 960, 0, 0);
    ms_hw_pcm_trim_meta b = meta_s(0, 0, 960, 0, 0);
    ms_hw_pcm_trim_init(&t);
    (void)ms_hw_pcm_trim_set_bpf(&t, 2);
    (void)ms_hw_pcm_trim_push(&t, &a);
    (void)ms_hw_pcm_trim_push(&t, &b);
    gen_inc(in, 960, 1, 0);
    if (ms_hw_pcm_trim_feed(&t, (uint8_t *)in, 960, 0, 0,
                            (uint8_t *)out, sizeof(out), &n1) != MS_OK) {
        fail_msg("multi start 1");
        return -1;
    }
    gen_inc(in, 960, 1, 960);
    if (ms_hw_pcm_trim_feed(&t, (uint8_t *)in, 960, 0, 0,
                            (uint8_t *)out, sizeof(out), &n2) != MS_OK) {
        fail_msg("multi start 2");
        return -1;
    }
    s1 = (uint32_t)(n1 / 2);
    s2 = (uint32_t)(n2 / 2);
    /* 第一包全部扣 start，剩余 240 转到第二包前缀 → 第二包可听 720，从 1200 起 */
    if (s1 != 0 || s2 != 720) {
        fprintf(stderr, "  multipacket start got %u+%u want 0+720\n", s1, s2);
        fail_msg("multipacket start");
        return -1;
    }
    if (check_inc(out, 720, 1, 1200, 1919, "multi start body") != 0) {
        return -1;
    }
    return 0;
}

static int test_pts_mismatch_and_switch(void)
{
    ms_hw_pcm_trim t;
    int16_t in[960];
    int16_t out[1920];
    size_t n = 0;
    ms_hw_pcm_trim_meta a = meta_s(0, 0, 960, 1, 10000);
    ms_hw_pcm_trim_meta b = meta_s(0, 0, 960, 1, 20000);
    ms_hw_pcm_trim_init(&t);
    (void)ms_hw_pcm_trim_set_bpf(&t, 2);
    (void)ms_hw_pcm_trim_push(&t, &a);
    (void)ms_hw_pcm_trim_push(&t, &b);
    gen_inc(in, 400, 1, 0);
    if (ms_hw_pcm_trim_feed(&t, (uint8_t *)in, 400, 1, 10000,
                            (uint8_t *)out, sizeof(out), &n) != MS_OK) {
        fail_msg("pts first");
        return -1;
    }
    /* 下一块 PTS 跳到下一 AU：提前结束当前（不足 expected），再吃下一包。 */
    gen_inc(in, 960, 1, 1000);
    n = 0;
    if (ms_hw_pcm_trim_feed(&t, (uint8_t *)in, 960, 1, 20000,
                            (uint8_t *)out, sizeof(out), &n) != MS_OK) {
        fail_msg("pts switch");
        return -1;
    }
    if ((uint32_t)(n / 2) != 960) {
        fprintf(stderr, "  pts switch wrote %zu samples\n", n / 2);
        fail_msg("pts switch count");
        return -1;
    }
    /* 反向 PTS */
    ms_hw_pcm_trim_reset(&t);
    (void)ms_hw_pcm_trim_set_bpf(&t, 2);
    (void)ms_hw_pcm_trim_push(&t, &a);
    gen_inc(in, 200, 1, 0);
    if (ms_hw_pcm_trim_feed(&t, (uint8_t *)in, 200, 1, 10000,
                            (uint8_t *)out, sizeof(out), &n) != MS_OK) {
        fail_msg("pts mid");
        return -1;
    }
    if (ms_hw_pcm_trim_feed(&t, (uint8_t *)in, 200, 1, 0,
                            (uint8_t *)out, sizeof(out), &n) != MS_ERR_STATE) {
        fail_msg("reorder pts must be STATE");
        return -1;
    }
    return 0;
}

static int test_overflow_and_flush(void)
{
    ms_hw_pcm_trim t;
    unsigned i;
    int16_t in[8];
    int16_t out[8];
    size_t n = 0;
    ms_hw_pcm_trim_init(&t);
    (void)ms_hw_pcm_trim_set_bpf(&t, 2);
    for (i = 0; i < MS_HW_PCM_TRIM_META_MAX; i++) {
        ms_hw_pcm_trim_meta m = meta_s(0, 0, 8, 0, 0);
        if (ms_hw_pcm_trim_push(&t, &m) != MS_OK) {
            fail_msg("meta cap fill");
            return -1;
        }
    }
    {
        ms_hw_pcm_trim_meta m = meta_s(0, 0, 8, 0, 0);
        if (ms_hw_pcm_trim_push(&t, &m) != MS_LIMIT) {
            fail_msg("meta overflow LIMIT");
            return -1;
        }
    }
    ms_hw_pcm_trim_reset(&t);
    (void)ms_hw_pcm_trim_set_bpf(&t, 2);
    {
        /* 未知 expected 且 skip_end 超过 hold 上限：OVERFLOW，不得把应丢尾部先写出。 */
        ms_hw_pcm_trim_meta m = meta_s(0, MS_HW_PCM_TRIM_HOLD_MAX + 8, 0, 0, 0);
        uint8_t *big;
        uint32_t ns = MS_HW_PCM_TRIM_HOLD_MAX + 8;
        (void)ms_hw_pcm_trim_push(&t, &m);
        big = (uint8_t *)malloc((size_t)ns * 2u);
        if (big == NULL) {
            fail_msg("oom");
            return -1;
        }
        memset(big, 0, (size_t)ns * 2u);
        if (ms_hw_pcm_trim_feed(&t, big, ns, 0, 0, (uint8_t *)out, sizeof(out), &n) !=
            MS_ERR_OVERFLOW) {
            free(big);
            fail_msg("oversize hold OVERFLOW");
            return -1;
        }
        free(big);
    }
    ms_hw_pcm_trim_reset(&t);
    (void)ms_hw_pcm_trim_set_bpf(&t, 2);
    {
        ms_hw_pcm_trim_meta m = meta_s(0, 4, 0, 0, 0);
        (void)ms_hw_pcm_trim_push(&t, &m);
        gen_inc(in, 8, 1, 0);
        (void)ms_hw_pcm_trim_feed(&t, (uint8_t *)in, 8, 0, 0,
                                  (uint8_t *)out, sizeof(out), &n);
        if (ms_hw_pcm_trim_hold_samples(&t) != 4) {
            fail_msg("flush setup hold");
            return -1;
        }
        ms_hw_pcm_trim_reset(&t);
        if (ms_hw_pcm_trim_hold_samples(&t) != 0 ||
            ms_hw_pcm_trim_pending_meta(&t) != 0) {
            fail_msg("flush must drop hold not write");
            return -1;
        }
    }
    (void)out;
    return 0;
}

static int test_stereo_bpf_and_no_meta(void)
{
    ms_hw_pcm_trim t;
    int16_t in[16];
    int16_t out[16];
    size_t n = 0;
    ms_hw_pcm_trim_init(&t);
    if (ms_hw_pcm_trim_set_bpf(&t, 4) != MS_OK) {
        fail_msg("bpf 4");
        return -1;
    }
    gen_inc(in, 8, 2, 0);
    if (ms_hw_pcm_trim_feed(&t, (uint8_t *)in, 8, 0, 0,
                            (uint8_t *)out, sizeof(out), &n) != MS_ERR_STATE) {
        fail_msg("pcm without meta is STATE");
        return -1;
    }
    return 0;
}

/* expected=0（平台已修剪 delay）时靠 PTS 切 AU，才能在第二包上扣 skip_end。 */
static int test_unknown_expected_pts_switch_end(void)
{
    ms_hw_pcm_trim t;
    int16_t in[1920];
    int16_t out[1920];
    size_t n1 = 0, n2 = 0;
    uint32_t s;
    ms_hw_pcm_trim_meta a = meta_s(0, 0, 0, 1, 0);
    ms_hw_pcm_trim_meta b = meta_s(0, 648, 0, 1, 20000);
    ms_hw_pcm_trim_init(&t);
    (void)ms_hw_pcm_trim_set_bpf(&t, 2);
    if (ms_hw_pcm_trim_push(&t, &a) != MS_OK ||
        ms_hw_pcm_trim_push(&t, &b) != MS_OK) {
        fail_msg("unk exp push");
        return -1;
    }
    gen_inc(in, 1920, 1, 312);
    if (ms_hw_pcm_trim_feed(&t, (uint8_t *)in, 648, 1, 0,
                            (uint8_t *)out, sizeof(out), &n1) != MS_OK) {
        fail_msg("unk exp feed1");
        return -1;
    }
    if (ms_hw_pcm_trim_feed(&t, (uint8_t *)(in + 648), 960, 1, 20000,
                            (uint8_t *)out + n1, sizeof(out) - n1, &n2) != MS_OK) {
        fail_msg("unk exp feed2");
        return -1;
    }
    s = (uint32_t)((n1 + n2) / 2);
    if (s != 960) {
        fprintf(stderr, "  unk exp got %u want 960 (n1=%zu n2=%zu)\n",
                s, n1, n2);
        fail_msg("unk exp total");
        return -1;
    }
    if (check_inc(out, 960, 1, 312, 1271, "unk exp pts switch") != 0) {
        return -1;
    }
    return 0;
}

int test_hw_audio_trim_unit(void)
{
    g_fail = 0;
    if (test_split_one_au() != 0 || test_merge_two_au() != 0 ||
        test_empty_then_data() != 0 || test_delay_and_eos_unknown() != 0 ||
        test_prefix_warmup_3840() != 0 || test_multipacket_start() != 0 ||
        test_pts_mismatch_and_switch() != 0 ||
        test_unknown_expected_pts_switch_end() != 0 ||
        test_overflow_and_flush() != 0 ||
        test_stereo_bpf_and_no_meta() != 0) {
        fprintf(stderr, "test_hw_audio_trim_unit failed\n");
        return -1;
    }
    if (g_fail) {
        return -1;
    }
    printf("  test_hw_audio_trim_unit ok (split/merge/empty/delay/eos/3840/312 carry; not device)\n");
    return 0;
}

static void rel_trim(void *ctx, uint8_t *data, size_t n)
{
    (void)data;
    (void)n;
    if (ctx != NULL) {
        *((int *)ctx) += 1;
    }
}

static int clk_us(uint32_t samples)
{
    return (int)((samples * 1000000ull) / 48000ull);
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
    st = ms_hw_audio_start(*out);
    if (st != MS_OK) {
        ms_hw_audio_destroy(*out);
        *out = NULL;
    }
    return st;
}

static ms_status sub_toc_ex(ms_hw_audio *a, int64_t pts, int has_pts,
                            uint32_t skip_s, uint32_t skip_e, int *rel)
{
    ms_hw_buf buf;
    ms_hw_audio_au au;
    memset(&buf, 0, sizeof(buf));
    buf.data = k_opus_toc20;
    buf.size = sizeof(k_opus_toc20);
    buf.release_fn = rel_trim;
    buf.release_ctx = rel;
    if (rel != NULL) {
        *rel = 0;
    }
    memset(&au, 0, sizeof(au));
    au.struct_size = sizeof(au);
    au.buf = &buf;
    au.has_pts = has_pts ? 1 : 0;
    au.pts_us = has_pts ? pts : 0;
    au.skip_samples_start = skip_s;
    au.skip_samples_end = skip_e;
    return ms_hw_audio_submit_au(a, &au);
}

static ms_status sub_toc(ms_hw_audio *a, int64_t pts, uint32_t skip_s,
                         uint32_t skip_e, int *rel)
{
    return sub_toc_ex(a, pts, 1, skip_s, skip_e, rel);
}

static int via_split_312(void)
{
    ms_hw_audio *a = NULL;
    ms_hw_audio_dec_pat pat;
    int16_t sink[2048];
    size_t n = 0;
    int rel = 0;
    int64_t clock = 0;
    if (make_opus_hw(&a) != MS_OK) {
        fail_msg("create split");
        return -1;
    }
    memset(&pat, 0, sizeof(pat));
    pat.struct_size = sizeof(pat);
    pat.mode = MS_HW_AUDIO_DEC_SPLIT;
    pat.split[0] = 400;
    pat.split[1] = 560;
    pat.split_n = 2;
    if (ms_hw_audio_debug_set_dec_pat(a, &pat) != MS_OK) {
        fail_msg("pat split");
        ms_hw_audio_destroy(a);
        return -1;
    }
    ms_hw_audio_debug_set_pcm_sink(a, sink, 2048, &n);
    if (sub_toc(a, 0, 312, 0, &rel) != MS_OK || ms_hw_audio_drive(a) != MS_OK) {
        fail_msg("drive split");
        ms_hw_audio_destroy(a);
        return -1;
    }
    if (n != 648) {
        fprintf(stderr, "  split n=%zu want 648 (1AU->2 PCM; 1:1 would mis-trim)\n", n);
        fail_msg("split count");
        ms_hw_audio_destroy(a);
        return -1;
    }
    if (check_inc(sink, 648, 1, 312, 959, "via split") != 0) {
        ms_hw_audio_destroy(a);
        return -1;
    }
    if (ms_hw_audio_get_clock_us(a, &clock) != MS_OK || clock != clk_us(648) ||
        rel != 1) {
        fail_msg("split clock/rel");
        ms_hw_audio_destroy(a);
        return -1;
    }
    ms_hw_audio_destroy(a);
    return 0;
}

static int via_merge_end(void)
{
    /* 两包合成一块输出：第一包 start 312，第二包 end 648。
     * 1:1 会把 end 误用到整块 1920，或丢掉第二包 end。 */
    ms_hw_audio *a = NULL;
    ms_hw_audio_dec_pat pat;
    int16_t sink[4096];
    size_t n = 0;
    int rel1 = 0, rel2 = 0;
    int64_t clock = 0;
    if (make_opus_hw(&a) != MS_OK) {
        fail_msg("create merge");
        return -1;
    }
    memset(&pat, 0, sizeof(pat));
    pat.struct_size = sizeof(pat);
    pat.mode = MS_HW_AUDIO_DEC_MERGE;
    pat.merge_n = 2;
    if (ms_hw_audio_debug_set_dec_pat(a, &pat) != MS_OK) {
        fail_msg("pat merge");
        ms_hw_audio_destroy(a);
        return -1;
    }
    ms_hw_audio_debug_set_pcm_sink(a, sink, 4096, &n);
    if (sub_toc(a, 0, 312, 0, &rel1) != MS_OK) {
        fail_msg("sub merge 1");
        ms_hw_audio_destroy(a);
        return -1;
    }
    if (sub_toc(a, 20000, 0, 648, &rel2) != MS_OK) {
        fail_msg("sub merge 2");
        ms_hw_audio_destroy(a);
        return -1;
    }
    if (ms_hw_audio_drive(a) != MS_OK) {
        fail_msg("drive merge");
        ms_hw_audio_destroy(a);
        return -1;
    }
    if (n != 960) {
        fprintf(stderr, "  merge n=%zu want 960 (648+312)\n", n);
        fail_msg("merge count");
        ms_hw_audio_destroy(a);
        return -1;
    }
    if (check_inc(sink, 960, 1, 312, 1271, "via merge") != 0) {
        ms_hw_audio_destroy(a);
        return -1;
    }
    if (ms_hw_audio_get_clock_us(a, &clock) != MS_OK || clock != clk_us(960) ||
        rel1 != 1 || rel2 != 1) {
        fail_msg("merge clock/rel");
        ms_hw_audio_destroy(a);
        return -1;
    }
    ms_hw_audio_destroy(a);
    return 0;
}

static int via_empty_delay_eos(void)
{
    ms_hw_audio *a = NULL;
    ms_hw_audio_dec_pat pat;
    int16_t sink[2048];
    size_t n = 0;
    int rel = 0;
    int64_t clock = 0;
    ms_hw_audio_stats st;
    if (make_opus_hw(&a) != MS_OK) {
        fail_msg("create empty");
        return -1;
    }
    memset(&pat, 0, sizeof(pat));
    pat.struct_size = sizeof(pat);
    pat.mode = MS_HW_AUDIO_DEC_EMPTY_FIRST;
    pat.empty_n = 1;
    ms_hw_audio_debug_set_dec_pat(a, &pat);
    ms_hw_audio_debug_set_pcm_sink(a, sink, 2048, &n);
    if (sub_toc(a, 0, 312, 0, &rel) != MS_OK || ms_hw_audio_drive(a) != MS_OK) {
        fail_msg("drive empty_first");
        ms_hw_audio_destroy(a);
        return -1;
    }
    if (n != 648 || check_inc(sink, 648, 1, 312, 959, "via empty") != 0) {
        fail_msg("empty_first count/bounds");
        ms_hw_audio_destroy(a);
        return -1;
    }
    ms_hw_audio_destroy(a);

    n = 0;
    if (make_opus_hw(&a) != MS_OK) {
        fail_msg("create delay");
        return -1;
    }
    memset(&pat, 0, sizeof(pat));
    pat.struct_size = sizeof(pat);
    pat.mode = MS_HW_AUDIO_DEC_DELAY;
    pat.delay_drives = 1;
    ms_hw_audio_debug_set_dec_pat(a, &pat);
    ms_hw_audio_debug_set_pcm_sink(a, sink, 2048, &n);
    if (sub_toc(a, 0, 312, 0, &rel) != MS_OK) {
        fail_msg("sub delay");
        ms_hw_audio_destroy(a);
        return -1;
    }
    if (ms_hw_audio_drive(a) != MS_OK) {
        fail_msg("drive delay 1");
        ms_hw_audio_destroy(a);
        return -1;
    }
    if (n != 0 || ms_hw_audio_get_clock_us(a, &clock) != MS_OK || clock != 0) {
        fail_msg("delay first drive must emit nothing");
        ms_hw_audio_destroy(a);
        return -1;
    }
    if (ms_hw_audio_drive(a) != MS_OK) {
        fail_msg("drive delay 2");
        ms_hw_audio_destroy(a);
        return -1;
    }
    if (n != 648 || check_inc(sink, 648, 1, 312, 959, "via delay") != 0) {
        fail_msg("delay second drive");
        ms_hw_audio_destroy(a);
        return -1;
    }
    memset(&st, 0, sizeof(st));
    st.struct_size = sizeof(st);
    if (ms_hw_audio_signal_eos(a) != MS_OK ||
        ms_hw_audio_get_stats(a, &st) != MS_OK || st.eos_out == 0) {
        fail_msg("eos_out after drain");
        ms_hw_audio_destroy(a);
        return -1;
    }
    ms_hw_audio_destroy(a);
    return 0;
}

static int via_partial_and_flush(void)
{
    ms_hw_audio *a = NULL;
    ms_hw_audio_dec_pat pat;
    int16_t sink[2048];
    size_t n = 0, n_before;
    int rel = 0;
    int64_t clock = 0;
    size_t bpf = 4; /* s16 stereo */
    ms_hw_audio_stats st;
    if (make_opus_hw(&a) != MS_OK) {
        fail_msg("create partial");
        return -1;
    }
    memset(&pat, 0, sizeof(pat));
    pat.struct_size = sizeof(pat);
    pat.mode = MS_HW_AUDIO_DEC_SPLIT;
    pat.split[0] = 400;
    pat.split[1] = 560;
    pat.split_n = 2;
    ms_hw_audio_debug_set_dec_pat(a, &pat);
    ms_hw_audio_debug_set_pcm_sink(a, sink, 2048, &n);
    if (ms_hw_audio_add_write_quota(a, 100 * bpf) != MS_OK) {
        fail_msg("quota 100");
        ms_hw_audio_destroy(a);
        return -1;
    }
    if (sub_toc(a, 0, 312, 0, &rel) != MS_OK || ms_hw_audio_drive(a) != MS_OK) {
        fail_msg("drive quota");
        ms_hw_audio_destroy(a);
        return -1;
    }
    memset(&st, 0, sizeof(st));
    st.struct_size = sizeof(st);
    ms_hw_audio_get_stats(a, &st);
    if (n != 100 || rel != 0 || st.pending != 1) {
        fprintf(stderr, "  partial n=%zu rel=%d pending=%u\n", n, rel, st.pending);
        fail_msg("partial mid");
        ms_hw_audio_destroy(a);
        return -1;
    }
    if (check_inc(sink, 100, 1, 312, 411, "via partial mid") != 0) {
        ms_hw_audio_destroy(a);
        return -1;
    }
    n_before = n;
    if (ms_hw_audio_add_write_quota(a, 648 * bpf) != MS_OK ||
        ms_hw_audio_drive(a) != MS_OK) {
        fail_msg("quota rest");
        ms_hw_audio_destroy(a);
        return -1;
    }
    if (n != 648 || rel != 1 ||
        check_inc(sink, 648, 1, 312, 959, "via partial full") != 0) {
        fail_msg("partial complete");
        ms_hw_audio_destroy(a);
        return -1;
    }
    (void)n_before;
    /* flush 丢弃未写完的可听 leftover，不把 hold/padding 写出 */
    if (ms_hw_audio_flush(a) != MS_OK) {
        fail_msg("flush");
        ms_hw_audio_destroy(a);
        return -1;
    }
    n = 0;
    ms_hw_audio_debug_set_pcm_sink(a, sink, 2048, &n);
    memset(&pat, 0, sizeof(pat));
    pat.struct_size = sizeof(pat);
    pat.mode = MS_HW_AUDIO_DEC_NATURAL;
    ms_hw_audio_debug_set_dec_pat(a, &pat);
    if (ms_hw_audio_note_seek(a, 1000000) != MS_OK) {
        fail_msg("seek warmup");
        ms_hw_audio_destroy(a);
        return -1;
    }
    {
        unsigned i;
        for (i = 0; i < 5; i++) {
            if (sub_toc(a, (int64_t)i * 20000, 0, 0, &rel) != MS_OK) {
                fail_msg("sub warmup");
                ms_hw_audio_destroy(a);
                return -1;
            }
        }
    }
    if (ms_hw_audio_drive(a) != MS_OK) {
        fail_msg("drive warmup");
        ms_hw_audio_destroy(a);
        return -1;
    }
    if (n != 960 || check_inc(sink, 960, 1, 3840, 4799, "via 3840") != 0) {
        fprintf(stderr, "  warmup n=%zu\n", n);
        fail_msg("3840 warmup");
        ms_hw_audio_destroy(a);
        return -1;
    }
    if (ms_hw_audio_get_clock_us(a, &clock) != MS_OK || clock != clk_us(960)) {
        fail_msg("warmup clock");
        ms_hw_audio_destroy(a);
        return -1;
    }
    /* seek flush 取消：提交后未 drive 完就 flush，sink 不再增加 */
    n = 0;
    if (ms_hw_audio_flush(a) != MS_OK ||
        sub_toc(a, 0, 0, 648, &rel) != MS_OK) {
        fail_msg("flush cancel setup");
        ms_hw_audio_destroy(a);
        return -1;
    }
    if (ms_hw_audio_flush(a) != MS_OK) {
        fail_msg("flush cancel");
        ms_hw_audio_destroy(a);
        return -1;
    }
    if (n != 0) {
        fail_msg("flush must not emit discarded PCM");
        ms_hw_audio_destroy(a);
        return -1;
    }
    ms_hw_audio_destroy(a);
    return 0;
}

static int via_natural_end_and_clock(void)
{
    /* NATURAL 保持 312/648 与「提交时钟不是扬声器时钟」 */
    ms_hw_audio *a = NULL;
    int16_t sink[2048];
    size_t n = 0;
    int rel = 0;
    int64_t clock = 0;
    if (make_opus_hw(&a) != MS_OK) {
        fail_msg("create natural");
        return -1;
    }
    ms_hw_audio_debug_set_pcm_sink(a, sink, 2048, &n);
    if (sub_toc(a, 0, 0, 0, &rel) != MS_OK) {
        fail_msg("sub natural");
        ms_hw_audio_destroy(a);
        return -1;
    }
    if (ms_hw_audio_get_clock_us(a, &clock) != MS_OK || clock != 0) {
        fail_msg("submit is not speaker clock");
        ms_hw_audio_destroy(a);
        return -1;
    }
    if (ms_hw_audio_drive(a) != MS_OK || n != 648 ||
        check_inc(sink, 648, 1, 312, 959, "via natural 312") != 0) {
        fail_msg("natural 312");
        ms_hw_audio_destroy(a);
        return -1;
    }
    n = 0;
    if (sub_toc(a, 20000, 0, 648, &rel) != MS_OK || ms_hw_audio_drive(a) != MS_OK ||
        n != 312 || check_inc(sink, 312, 1, 960, 1271, "via natural end") != 0) {
        fail_msg("natural end 648");
        ms_hw_audio_destroy(a);
        return -1;
    }
    ms_hw_audio_destroy(a);
    return 0;
}

/* 部分写中途 flush：必须丢掉 leftover，不得在随后 drive 中写出旧 PCM。 */
static int via_partial_flush_mid_leftover(void)
{
    ms_hw_audio *a = NULL;
    int16_t sink[2048];
    size_t n = 0;
    int rel = 0;
    int64_t clock = 0;
    size_t bpf = 4;
    ms_hw_audio_stats st;
    if (make_opus_hw(&a) != MS_OK) {
        fail_msg("create mid leftover");
        return -1;
    }
    ms_hw_audio_debug_set_pcm_sink(a, sink, 2048, &n);
    if (ms_hw_audio_add_write_quota(a, 100 * bpf) != MS_OK) {
        fail_msg("quota mid leftover");
        ms_hw_audio_destroy(a);
        return -1;
    }
    if (sub_toc(a, 0, 312, 0, &rel) != MS_OK || ms_hw_audio_drive(a) != MS_OK) {
        fail_msg("drive mid leftover");
        ms_hw_audio_destroy(a);
        return -1;
    }
    memset(&st, 0, sizeof(st));
    st.struct_size = sizeof(st);
    if (ms_hw_audio_get_stats(a, &st) != MS_OK || n != 100 || rel != 0 ||
        st.pending != 1 ||
        check_inc(sink, 100, 1, 312, 411, "mid leftover written") != 0) {
        fail_msg("mid leftover partial");
        ms_hw_audio_destroy(a);
        return -1;
    }
    if (ms_hw_audio_pause(a) != MS_OK) {
        fail_msg("pause leftover");
        ms_hw_audio_destroy(a);
        return -1;
    }
    if (ms_hw_audio_add_write_quota(a, 2000 * bpf) != MS_OK ||
        ms_hw_audio_drive(a) != MS_OK) {
        fail_msg("drive while paused");
        ms_hw_audio_destroy(a);
        return -1;
    }
    if (n != 100) {
        fail_msg("pause must not drain leftover");
        ms_hw_audio_destroy(a);
        return -1;
    }
    /* flush 在暂停中也必须丢掉 leftover 并释放 AU。 */
    if (ms_hw_audio_flush(a) != MS_OK) {
        fail_msg("flush mid leftover");
        ms_hw_audio_destroy(a);
        return -1;
    }
    if (rel != 1) {
        fail_msg("flush must release leftover AU");
        ms_hw_audio_destroy(a);
        return -1;
    }
    n = 0;
    ms_hw_audio_debug_set_pcm_sink(a, sink, 2048, &n);
    if (ms_hw_audio_resume(a) != MS_OK || ms_hw_audio_drive(a) != MS_OK) {
        fail_msg("resume after leftover flush");
        ms_hw_audio_destroy(a);
        return -1;
    }
    if (n != 0 || ms_hw_audio_get_clock_us(a, &clock) != MS_OK || clock != 0) {
        fprintf(stderr, "  leftover after flush n=%zu clock=%lld\n",
                n, (long long)clock);
        fail_msg("flush must discard leftover");
        ms_hw_audio_destroy(a);
        return -1;
    }
    ms_hw_audio_destroy(a);
    return 0;
}

/* 暂停中保留 leftover；恢复后从断点续写，不得重复或跳过。 */
static int via_pause_leftover_resume(void)
{
    ms_hw_audio *a = NULL;
    int16_t sink[2048];
    size_t n = 0;
    int rel = 0;
    size_t bpf = 4;
    if (make_opus_hw(&a) != MS_OK) {
        fail_msg("create pause leftover");
        return -1;
    }
    ms_hw_audio_debug_set_pcm_sink(a, sink, 2048, &n);
    if (ms_hw_audio_add_write_quota(a, 100 * bpf) != MS_OK ||
        sub_toc(a, 0, 312, 0, &rel) != MS_OK ||
        ms_hw_audio_drive(a) != MS_OK) {
        fail_msg("partial before pause");
        ms_hw_audio_destroy(a);
        return -1;
    }
    if (n != 100 || rel != 0) {
        fail_msg("partial before pause count");
        ms_hw_audio_destroy(a);
        return -1;
    }
    if (ms_hw_audio_pause(a) != MS_OK ||
        ms_hw_audio_add_write_quota(a, 2000 * bpf) != MS_OK ||
        ms_hw_audio_drive(a) != MS_OK || n != 100) {
        fail_msg("paused leftover held");
        ms_hw_audio_destroy(a);
        return -1;
    }
    if (ms_hw_audio_resume(a) != MS_OK || ms_hw_audio_drive(a) != MS_OK) {
        fail_msg("resume leftover");
        ms_hw_audio_destroy(a);
        return -1;
    }
    if (n != 648 || rel != 1 ||
        check_inc(sink, 648, 1, 312, 959, "resume leftover") != 0) {
        fail_msg("resume leftover bounds");
        ms_hw_audio_destroy(a);
        return -1;
    }
    ms_hw_audio_destroy(a);
    return 0;
}

/* Android 路径：平台已扣 pre-skip / seek preroll，本层不得再扣 start，仍扣 skip_end。
 * 宿主 mock 在旗标打开后从输出丢掉 leading，再走同一修剪机。不是听感通过。 */
static int via_platform_trims_delay_and_end(void)
{
    ms_hw_audio *a = NULL;
    int16_t sink[4096];
    size_t n = 0;
    int rel1 = 0, rel2 = 0;
    int64_t clock = 0;
    size_t bpf = 4;
    int rel = 0;
    if (make_opus_hw(&a) != MS_OK) {
        fail_msg("create plat trim");
        return -1;
    }
    if (ms_hw_audio_debug_set_platform_trims_delay(a, 1) != MS_OK) {
        fail_msg("enable plat trim");
        ms_hw_audio_destroy(a);
        return -1;
    }
    ms_hw_audio_debug_set_pcm_sink(a, sink, 4096, &n);
    if (sub_toc(a, 0, 312, 0, &rel1) != MS_OK ||
        sub_toc(a, 20000, 0, 648, &rel2) != MS_OK ||
        ms_hw_audio_drive(a) != MS_OK) {
        fail_msg("drive plat trim");
        ms_hw_audio_destroy(a);
        return -1;
    }
    /* 与 platform_trims=0 相同可听：648+312=960，样点 312..1271。
     * 若再扣 start，第一包只剩 336 或与 skip_end 错位。 */
    if (n != 960 || rel1 != 1 || rel2 != 1 ||
        check_inc(sink, 960, 1, 312, 1271, "plat no double start") != 0) {
        fprintf(stderr, "  plat trim n=%zu want 960\n", n);
        fail_msg("plat trim start/end");
        ms_hw_audio_destroy(a);
        return -1;
    }
    if (ms_hw_audio_get_clock_us(a, &clock) != MS_OK || clock != clk_us(960)) {
        fail_msg("plat trim clock");
        ms_hw_audio_destroy(a);
        return -1;
    }
    ms_hw_audio_destroy(a);

    /* 中途 seek：平台扣 3840 warmup，本层不得再扣；再加 leftover flush。 */
    n = 0;
    if (make_opus_hw(&a) != MS_OK) {
        fail_msg("create plat seek");
        return -1;
    }
    if (ms_hw_audio_debug_set_platform_trims_delay(a, 1) != MS_OK) {
        fail_msg("enable plat seek");
        ms_hw_audio_destroy(a);
        return -1;
    }
    ms_hw_audio_debug_set_pcm_sink(a, sink, 4096, &n);
    if (sub_toc(a, 0, 312, 0, &rel) != MS_OK || ms_hw_audio_drive(a) != MS_OK ||
        n != 648) {
        fail_msg("plat seek priming");
        ms_hw_audio_destroy(a);
        return -1;
    }
    if (ms_hw_audio_add_write_quota(a, 80 * bpf) != MS_OK ||
        sub_toc(a, 20000, 0, 0, &rel) != MS_OK ||
        ms_hw_audio_drive(a) != MS_OK) {
        fail_msg("plat leftover setup");
        ms_hw_audio_destroy(a);
        return -1;
    }
    if (n != 648 + 80 || rel != 0) {
        fprintf(stderr, "  plat leftover n=%zu rel=%d\n", n, rel);
        fail_msg("plat leftover mid");
        ms_hw_audio_destroy(a);
        return -1;
    }
    if (ms_hw_audio_flush(a) != MS_OK || rel != 1) {
        fail_msg("plat flush leftover");
        ms_hw_audio_destroy(a);
        return -1;
    }
    n = 0;
    ms_hw_audio_debug_set_pcm_sink(a, sink, 4096, &n);
    if (ms_hw_audio_note_seek(a, 1000000) != MS_OK) {
        fail_msg("plat note_seek");
        ms_hw_audio_destroy(a);
        return -1;
    }
    {
        unsigned i;
        for (i = 0; i < 5; i++) {
            if (sub_toc(a, (int64_t)i * 20000, 0, 0, &rel) != MS_OK) {
                fail_msg("plat seek sub");
                ms_hw_audio_destroy(a);
                return -1;
            }
        }
    }
    if (ms_hw_audio_drive(a) != MS_OK) {
        fail_msg("plat seek drive");
        ms_hw_audio_destroy(a);
        return -1;
    }
    /* 5*960-3840=960。若再扣 warmup 则为 0 或错位。 */
    if (n != 960 ||
        check_inc(sink, 960, 1, 3840, 4799, "plat seek warmup") != 0) {
        fprintf(stderr, "  plat seek n=%zu want 960\n", n);
        fail_msg("plat seek warmup double");
        ms_hw_audio_destroy(a);
        return -1;
    }
    if (ms_hw_audio_get_clock_us(a, &clock) != MS_OK || clock != clk_us(960)) {
        fail_msg("plat seek clock");
        ms_hw_audio_destroy(a);
        return -1;
    }
    ms_hw_audio_destroy(a);
    return 0;
}

/* 产品合同：platform_trims 的 AU 边界按「输出 expected」计数，不得依赖互异 PTS。
 * 缺 PTS 或两包时间戳相同，第二包 skip_end 仍须生效（旧 expected=0 会得到 1608）。 */
static int via_platform_trims_no_or_same_pts(void)
{
    ms_hw_audio *a = NULL;
    int16_t sink[4096];
    size_t n = 0;
    int rel1 = 0, rel2 = 0, rel = 0;
    int64_t clock = 0;
    unsigned i;

    if (make_opus_hw(&a) != MS_OK ||
        ms_hw_audio_debug_set_platform_trims_delay(a, 1) != MS_OK) {
        fail_msg("create plat no pts");
        if (a) {
            ms_hw_audio_destroy(a);
        }
        return -1;
    }
    ms_hw_audio_debug_set_pcm_sink(a, sink, 4096, &n);
    if (sub_toc_ex(a, 0, 0, 312, 0, &rel1) != MS_OK ||
        sub_toc_ex(a, 0, 0, 0, 648, &rel2) != MS_OK ||
        ms_hw_audio_drive(a) != MS_OK) {
        fail_msg("drive plat no pts");
        ms_hw_audio_destroy(a);
        return -1;
    }
    if (n != 960 || rel1 != 1 || rel2 != 1 ||
        check_inc(sink, 960, 1, 312, 1271, "plat no pts") != 0) {
        fprintf(stderr, "  plat no pts n=%zu want 960 (not 1608)\n", n);
        fail_msg("plat no pts skip_end");
        ms_hw_audio_destroy(a);
        return -1;
    }
    ms_hw_audio_destroy(a);

    n = 0;
    if (make_opus_hw(&a) != MS_OK ||
        ms_hw_audio_debug_set_platform_trims_delay(a, 1) != MS_OK) {
        fail_msg("create plat same pts");
        if (a) {
            ms_hw_audio_destroy(a);
        }
        return -1;
    }
    ms_hw_audio_debug_set_pcm_sink(a, sink, 4096, &n);
    if (sub_toc_ex(a, 0, 1, 312, 0, &rel1) != MS_OK ||
        sub_toc_ex(a, 0, 1, 0, 648, &rel2) != MS_OK ||
        ms_hw_audio_drive(a) != MS_OK) {
        fail_msg("drive plat same pts");
        ms_hw_audio_destroy(a);
        return -1;
    }
    if (n != 960 ||
        check_inc(sink, 960, 1, 312, 1271, "plat same pts") != 0) {
        fprintf(stderr, "  plat same pts n=%zu want 960\n", n);
        fail_msg("plat same pts skip_end");
        ms_hw_audio_destroy(a);
        return -1;
    }
    ms_hw_audio_destroy(a);

    /* seek 后 3840 lead 整包吞掉，第五包带 skip_end、无 PTS。 */
    n = 0;
    if (make_opus_hw(&a) != MS_OK ||
        ms_hw_audio_debug_set_platform_trims_delay(a, 1) != MS_OK) {
        fail_msg("create plat seek end");
        if (a) {
            ms_hw_audio_destroy(a);
        }
        return -1;
    }
    ms_hw_audio_debug_set_pcm_sink(a, sink, 4096, &n);
    if (sub_toc_ex(a, 0, 0, 312, 0, &rel) != MS_OK ||
        ms_hw_audio_drive(a) != MS_OK || n != 648) {
        fail_msg("plat seek-end priming");
        ms_hw_audio_destroy(a);
        return -1;
    }
    if (ms_hw_audio_flush(a) != MS_OK ||
        ms_hw_audio_note_seek(a, 1000000) != MS_OK) {
        fail_msg("plat seek-end seek");
        ms_hw_audio_destroy(a);
        return -1;
    }
    n = 0;
    ms_hw_audio_debug_set_pcm_sink(a, sink, 4096, &n);
    for (i = 0; i < 4; i++) {
        if (sub_toc_ex(a, 0, 0, 0, 0, &rel) != MS_OK) {
            fail_msg("plat seek-end lead au");
            ms_hw_audio_destroy(a);
            return -1;
        }
    }
    if (sub_toc_ex(a, 0, 0, 0, 648, &rel) != MS_OK ||
        ms_hw_audio_drive(a) != MS_OK) {
        fail_msg("plat seek-end last");
        ms_hw_audio_destroy(a);
        return -1;
    }
    if (n != 312 ||
        check_inc(sink, 312, 1, 3840, 4151, "plat seek-end skip") != 0) {
        fprintf(stderr, "  plat seek-end n=%zu want 312\n", n);
        fail_msg("plat seek-end skip_end");
        ms_hw_audio_destroy(a);
        return -1;
    }
    if (ms_hw_audio_get_clock_us(a, &clock) != MS_OK || clock != clk_us(312)) {
        fail_msg("plat seek-end clock");
        ms_hw_audio_destroy(a);
        return -1;
    }
    ms_hw_audio_destroy(a);
    return 0;
}



/* 反例：停转兜底把对外钟抬高后，head 再以更小值恢复时不得回退。
 * 用调试钩子拨 last_change / 强制 emitted，避免依赖宿主 mock 墙钟是否走到兜底。 */
static int via_clock_fallback_is_monotonic(void)
{
    ms_hw_audio *a = NULL;
    ms_hw_audio_dec_pat pat;
    int16_t sink[2048];
    size_t n = 0;
    int rel = 0;
    int64_t c0 = 0, c1 = 0, c2 = 0, c3 = 0;
    if (make_opus_hw(&a) != MS_OK) {
        fail_msg("create clock mono");
        return -1;
    }
    memset(&pat, 0, sizeof(pat));
    pat.struct_size = sizeof(pat);
    pat.mode = MS_HW_AUDIO_DEC_SPLIT;
    pat.split[0] = 400;
    pat.split[1] = 560;
    pat.split_n = 2;
    if (ms_hw_audio_debug_set_dec_pat(a, &pat) != MS_OK) {
        fail_msg("pat clock mono");
        ms_hw_audio_destroy(a);
        return -1;
    }
    ms_hw_audio_debug_set_pcm_sink(a, sink, 2048, &n);
    if (sub_toc(a, 0, 312, 0, &rel) != MS_OK || ms_hw_audio_drive(a) != MS_OK) {
        fail_msg("drive clock mono");
        ms_hw_audio_destroy(a);
        return -1;
    }
    if (ms_hw_audio_get_clock_us(a, &c0) != MS_OK || c0 <= 0) {
        fail_msg("c0 raw clock");
        ms_hw_audio_destroy(a);
        return -1;
    }
    {
        int paused = 0, quota = 0;
        int64_t mono = 0, last_raw = 0, last_chg = 0, emitted = 0, origin = 0;
        ms_hw_audio_debug_clock_info(a, &paused, &quota, &mono, &last_raw,
                                     &last_chg, &emitted, &origin);
        /* 先测 hold：不依赖停转兜底是否走进墙钟。 */
        ms_hw_audio_debug_force_emitted_clock(a, 3000000);
        if (ms_hw_audio_get_clock_us(a, &c2) != MS_OK || c2 < 3000000) {
            fprintf(stderr, "  forced emitted not held c2=%lld\n", (long long)c2);
            fail_msg("forced emitted must hold");
            ms_hw_audio_destroy(a);
            return -1;
        }
        if (mono > 0 && quota == 0 && paused == 0) {
            ms_hw_audio_debug_force_emitted_clock(a, c0);
            ms_hw_audio_debug_backdate_clock_change(a, 500000000ll);
            if (ms_hw_audio_get_clock_us(a, &c1) != MS_OK) {
                fail_msg("c1 fallback");
                ms_hw_audio_destroy(a);
                return -1;
            }
            if (c1 < c0 + 400000) {
                ms_hw_audio_debug_clock_info(a, &paused, &quota, &mono, &last_raw,
                                             &last_chg, &emitted, &origin);
                fprintf(stderr,
                        "  fallback did not advance c0=%lld c1=%lld paused=%d quota=%d "
                        "mono=%lld last_chg=%lld origin=%lld\n",
                        (long long)c0, (long long)c1, paused, quota,
                        (long long)mono, (long long)last_chg, (long long)origin);
                fail_msg("fallback should advance");
                ms_hw_audio_destroy(a);
                return -1;
            }
        } else {
            fprintf(stderr, "  skip stall-advance (mono/quota/paused); hold still tested\n");
            c1 = c2;
        }
    }
    /* 停转断言后重新抬到 3s，再测 head 恢复不得回退。 */
    ms_hw_audio_debug_force_emitted_clock(a, 3000000);
    if (ms_hw_audio_get_clock_us(a, &c2) != MS_OK || c2 < 3000000) {
        fprintf(stderr, "  re-hold failed c2=%lld\n", (long long)c2);
        fail_msg("forced emitted must hold");
        ms_hw_audio_destroy(a);
        return -1;
    }
    /* head 再前进一小段（远小于已发出钟），旧实现会跳回 raw。 */
    n = 0;
    if (sub_toc(a, 20000, 0, 0, &rel) != MS_OK || ms_hw_audio_drive(a) != MS_OK) {
        fail_msg("drive resume head");
        ms_hw_audio_destroy(a);
        return -1;
    }
    if (ms_hw_audio_get_clock_us(a, &c3) != MS_OK || c3 < c2) {
        fprintf(stderr, "  clock jumped back after head resume c2=%lld c3=%lld\n",
                (long long)c2, (long long)c3);
        fail_msg("head resume must not rewind clock");
        ms_hw_audio_destroy(a);
        return -1;
    }
    ms_hw_audio_destroy(a);
    printf("  via_clock_fallback_is_monotonic ok c0=%lld c1=%lld c2=%lld c3=%lld\n",
           (long long)c0, (long long)c1, (long long)c2, (long long)c3);
    return 0;
}

int test_hw_audio_trim_via_mock(void)
{
    g_fail = 0;
    if (via_natural_end_and_clock() != 0 || via_split_312() != 0 ||
        via_merge_end() != 0 || via_empty_delay_eos() != 0 ||
        via_partial_and_flush() != 0 ||
        via_partial_flush_mid_leftover() != 0 ||
        via_pause_leftover_resume() != 0 ||
        via_platform_trims_delay_and_end() != 0 ||
        via_platform_trims_no_or_same_pts() != 0 ||
        via_clock_fallback_is_monotonic() != 0) {
        fprintf(stderr, "test_hw_audio_trim_via_mock failed\n");
        return -1;
    }
    if (g_fail) {
        return -1;
    }
    printf("  test_hw_audio_trim_via_mock ok (product trim via mock; "
           "split/merge/empty/delay/partial/3840/"
           "leftover-flush/pause/platform_trims; not device)\n");
    return 0;
}
