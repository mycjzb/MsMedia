/*
 * single_passthrough_test.c — 阶段 4 音频直通纯 C 部分宿主测试：
 * 路由/回退决策、(E-)AC-3/DTS 帧采样数、IEC 61937 突发封装、坏帧容错。
 * 宿主无 HDMI，不证明功放出声；真实接收端需设备验收。
 * by: Long <mycjzb@gmail.com>
 */
#include "ms_hw_passthrough.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint8_t *load(const char *p, size_t *n)
{
    FILE *f = fopen(p, "rb");
    uint8_t *b;
    long sz;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    b = (uint8_t *)malloc((size_t)sz);
    if (b && fread(b, 1, (size_t)sz, f) != (size_t)sz) { free(b); b = NULL; }
    fclose(f);
    *n = (size_t)sz;
    return b;
}

/* 以同步字切帧：AC-3/E-AC-3 用帧长字段，DTS 用 FSIZE。 */
static size_t frame_len(ms_codec c, const uint8_t *p, size_t n)
{
    if (c == MS_CODEC_DTS) {
        if (n < 8 || p[0] != 0x7F || p[1] != 0xFE) return 0;
        return (size_t)((((p[5] & 3u) << 12) | ((unsigned)p[6] << 4) | (p[7] >> 4)) + 1u);
    }
    if (n < 6 || p[0] != 0x0B || p[1] != 0x77) return 0;
    {
        /* E-AC-3 用 frmsiz；AC-3 取下一个同步字前、且整帧可被解析的最短前缀。 */
        if ((p[5] >> 3) > 10) return ((((size_t)(p[2] & 7)) << 8 | p[3]) + 1) * 2;
        for (size_t k = 128; k <= 3840 && k <= n; k += 2) {
            if (k == n || (p[k] == 0x0B && p[k + 1] == 0x77)) {
                if (ms_pt_frame_samples(c, p, k) == 1536) return k;
            }
        }
        return 0;
    }
}

static void run_stream(const char *path, ms_codec c, int expect_frames, int spf, int burst_frames,
                       int rate_mul)
{
    size_t n = 0, off = 0;
    uint8_t *b = load(path, &n);
    int frames = 0, bursts = 0, total = 0, burst_total = 0;
    ms_pt_iec w;
    FILE *dump = NULL;
    char dp[600];
    CHECK(b != NULL, "load %s", path);
    snprintf(dp, sizeof(dp), "%s.iec", path);
    dump = fopen(dp, "wb");
    if (!b) return;
    CHECK(ms_pt_iec_init(&w, c) == 0, "iec init");
    CHECK(ms_pt_iec_rate_mul(c) == rate_mul, "rate mul");
    while (off < n) {
        size_t fl = frame_len(c, b + off, n - off);
        const uint8_t *o;
        size_t on;
        int fr, r, s;
        if (fl == 0 || fl > n - off) break;
        s = ms_pt_frame_samples(c, b + off, fl);
        CHECK(s == spf, "%s frame %d samples=%d", path, frames, s);
        total += s;
        r = ms_pt_iec_feed(&w, b + off, fl, &o, &on, &fr);
        CHECK(r >= 0, "%s feed %d r=%d", path, frames, r);
        if (r == 1) {
            bursts++;
            burst_total += fr;
            if (dump) fwrite(o, 1, on, dump);
            CHECK(on == (size_t)burst_frames * 4u, "burst bytes %zu", on);
            CHECK(o[0] == 0x72 && o[1] == 0xF8 && o[2] == 0x1F && o[3] == 0x4E, "Pa/Pb");
            if (c == MS_CODEC_AC3) {
                CHECK(o[4] == 1 && ((o[6] | (o[7] << 8)) == (int)(fl * 8)), "AC3 Pc/Pd");
                /* 字节对交换：载荷首字 0x0B77 -> 77 0B */
                CHECK(o[8] == 0x77 && o[9] == 0x0B, "swap");
            } else if (c == MS_CODEC_EAC3) {
                CHECK(o[4] == 21, "EAC3 Pc");
            } else {
                CHECK(o[4] == 11, "DTS type I");
                CHECK(o[8] == 0xFE && o[9] == 0x7F, "dts swap");
            }
        }
        frames++;
        off += fl;
    }
    if (dump) fclose(dump);
    CHECK(frames == expect_frames, "%s frames=%d expect %d", path, frames, expect_frames);
    CHECK(off == n, "%s consumed %zu/%zu", path, off, n);
    printf("STREAM %s frames=%d samples=%d bursts=%d burst_frames=%d\n", path, frames, total,
           bursts, burst_total);
    /* 时钟一致性：突发帧数 / (流采样率*倍数) == 已封装音频时长 */
    if (c == MS_CODEC_EAC3) {
        CHECK(burst_total == bursts * 6144 && bursts == total / 1536, "eac3 4x clock");
    } else {
        CHECK(burst_total == total, "burst frames == samples");
    }
    /* 坏帧：截断/错同步返回 <0，不崩溃 */
    {
        const uint8_t *o;
        size_t on;
        int fr;
        uint8_t junk[64];
        memset(junk, 0x55, sizeof(junk));
        CHECK(ms_pt_iec_feed(&w, junk, sizeof(junk), &o, &on, &fr) < 0, "junk rejected");
        CHECK(ms_pt_iec_feed(&w, b, 3, &o, &on, &fr) < 0, "short rejected");
        CHECK(ms_pt_frame_samples(c, junk, sizeof(junk)) == 0, "junk samples");
    }
    /* 多帧 AU（MKV/TS 里常见两帧一包）：采样数累加 */
    if (c != MS_CODEC_DTS) {
        size_t f1 = frame_len(c, b, n);
        size_t f2 = frame_len(c, b + f1, n - f1);
        CHECK(ms_pt_frame_samples(c, b, f1 + f2) == 2 * spf, "multi-frame AU");
    }
    ms_pt_iec_free(&w);
    free(b);
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : ".";
    char p[512];
    int enc;
    /* 路由：开关关闭/未知编码/API<21 一律软解 */
    CHECK(ms_pt_pick_route(MS_CODEC_AC3, MS_PT_CAP_AC3, 34, &enc) == MS_PT_ROUTE_SOFT, "no enable bit");
    CHECK(ms_pt_pick_route(MS_CODEC_AC3, MS_PT_CAP_ENABLED | MS_PT_CAP_AC3, 20, &enc) == MS_PT_ROUTE_SOFT, "api20");
    CHECK(ms_pt_pick_route(MS_CODEC_AAC, MS_PT_CAP_ENABLED | 0xF, 34, &enc) == MS_PT_ROUTE_SOFT, "aac never");
    CHECK(ms_pt_pick_route(MS_CODEC_AC3, MS_PT_CAP_ENABLED | MS_PT_CAP_AC3, 21, &enc) == MS_PT_ROUTE_RAW && enc == 5, "ac3 raw");
    CHECK(ms_pt_pick_route(MS_CODEC_EAC3, MS_PT_CAP_ENABLED | MS_PT_CAP_E_AC3, 29, &enc) == MS_PT_ROUTE_RAW && enc == 6, "eac3 raw");
    CHECK(ms_pt_pick_route(MS_CODEC_DTS, MS_PT_CAP_ENABLED | MS_PT_CAP_DTS, 29, &enc) == MS_PT_ROUTE_RAW && enc == 7, "dts raw");
    CHECK(ms_pt_pick_route(MS_CODEC_DTS, MS_PT_CAP_ENABLED | MS_PT_CAP_IEC61937, 24, &enc) == MS_PT_ROUTE_IEC && enc == 13, "dts iec");
    CHECK(ms_pt_pick_route(MS_CODEC_DTS, MS_PT_CAP_ENABLED | MS_PT_CAP_IEC61937, 23, &enc) == MS_PT_ROUTE_SOFT, "iec api23");
    CHECK(ms_pt_pick_route(MS_CODEC_EAC3, MS_PT_CAP_ENABLED | MS_PT_CAP_AC3, 34, &enc) == MS_PT_ROUTE_SOFT, "eac3 w/o cap -> soft");
    CHECK(ms_pt_iec_init(NULL, MS_CODEC_AC3) < 0, "null init");
    {
        ms_pt_iec w;
        CHECK(ms_pt_iec_init(&w, MS_CODEC_MP2) < 0, "mp2 not passthrough");
    }
    snprintf(p, sizeof(p), "%s/s.ac3", dir);
    run_stream(p, MS_CODEC_AC3, 63, 1536, 1536, 1);
    snprintf(p, sizeof(p), "%s/s.eac3", dir);
    run_stream(p, MS_CODEC_EAC3, 63, 1536, 6144, 4);
    snprintf(p, sizeof(p), "%s/s.dts", dir);
    run_stream(p, MS_CODEC_DTS, 188, 512, 512, 1);
    printf("PASSTHROUGH_%s fails=%d\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
