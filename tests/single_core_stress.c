/*
 * 纯自研核心（路线 A 单 so 同一份源码）宿主压力自测：
 * 多格式循环 create → push(随机分块)/drive(Source 短读/AGAIN 注入) → pull/release
 * → 渐进 MP4/MOV 随机 seek → 随机 cancel → destroy，外加字节变异（防越界/闪退）
 * 与外挂字幕解析。配合 -fsanitize=address,undefined（含 LSan）运行。
 * 另用计数分配器核对每轮 destroy 后核心内存归零（不只依赖 LSan）。
 * 用法：single_core_stress <iters> <file>...
 * 不打印样本完整路径以外的私有信息；只读本地样本，不联网。
 * by: Long <mycjzb@gmail.com>
 */
#define _DEFAULT_SOURCE
#include "msmedia/msmedia.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 计数分配器：记录核心经 ms_allocator 申请的未释放字节与块数。 */
typedef struct cnt_alloc {
    long long live_bytes;
    long long live_blocks;
    long long peak_bytes;
} cnt_alloc;

static void *cnt_alloc_fn(void *ctx, size_t size)
{
    cnt_alloc *c = (cnt_alloc *)ctx;
    size_t *p = (size_t *)malloc(size + sizeof(size_t) * 2);
    if (p == NULL) {
        return NULL;
    }
    p[0] = size;
    c->live_bytes += (long long)size;
    c->live_blocks++;
    if (c->live_bytes > c->peak_bytes) {
        c->peak_bytes = c->live_bytes;
    }
    return p + 2;
}

static void cnt_free_fn(void *ctx, void *ptr)
{
    cnt_alloc *c = (cnt_alloc *)ctx;
    size_t *p;
    if (ptr == NULL) {
        return;
    }
    p = (size_t *)ptr - 2;
    c->live_bytes -= (long long)p[0];
    c->live_blocks--;
    free(p);
}

/* 确定性伪随机（xorshift），便于复现。 */
static uint32_t g_rng = 0x12345678u;
static uint32_t rnd(void)
{
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return g_rng;
}

typedef struct run_stat {
    long au;
    long cfg;
    long sub;
    long eos;
    long seek_ok;
    long seek_other;
    long errors;
    long cancels;
} run_stat;

/* 公共：取空当前可取事件；按概率故意不 release 一部分，交给 destroy 回收。 */
static ms_status drain(ms_session *s, run_stat *rs, int keep_some)
{
    for (;;) {
        ms_event ev;
        ms_status st;
        memset(&ev, 0, sizeof(ev));
        st = ms_session_pull(s, &ev);
        if (st != MS_OK) {
            return st;
        }
        switch (ev.type) {
        case MS_EVENT_AU: rs->au++; break;
        case MS_EVENT_CODEC_CONFIG: rs->cfg++; break;
        case MS_EVENT_SUBTITLE: rs->sub++; break;
        case MS_EVENT_EOS: rs->eos++; break;
        default: break;
        }
        /* 触碰载荷，让 ASan 检查视图有效性。 */
        if (ev.data != NULL && ev.data_size > 0) {
            volatile uint8_t x = ev.data[0] ^ ev.data[ev.data_size - 1];
            (void)x;
        }
        if (ev.release_id != 0 && !(keep_some && (rnd() % 64) == 0)) {
            (void)ms_session_release(s, ev.release_id);
        }
        if (ev.type == MS_EVENT_EOS) {
            return MS_EOS;
        }
    }
}

/* 模式 1：push 随机分块。 */
static void run_push(const uint8_t *d, size_t n, run_stat *rs, cnt_alloc *ca)
{
    ms_allocator al = { cnt_alloc_fn, cnt_free_fn, ca };
    ms_session *s = NULL;
    size_t off = 0;
    int guard = 0;
    if (ms_session_create(&al, NULL, &s) != MS_OK) {
        rs->errors++;
        return;
    }
    while (off < n && guard++ < 2000000) {
        size_t want = 1 + (rnd() % 65536u);
        size_t used = 0;
        ms_status st;
        if (want > n - off) {
            want = n - off;
        }
        st = ms_session_push(s, d + off, want, &used);
        off += used;
        if (st == MS_LIMIT || st == MS_AGAIN || st == MS_OK) {
            ms_status ds = drain(s, rs, 1);
            if (st == MS_LIMIT && used == 0 && ds != MS_OK && ds != MS_AGAIN) {
                break;
            }
            continue;
        }
        rs->errors++; /* 畸形/不支持：诚实失败，不崩 */
        break;
    }
    (void)ms_session_end_of_input(s);
    (void)drain(s, rs, 0);
    if ((rnd() % 4) == 0) {
        (void)ms_session_cancel(s);
        rs->cancels++;
    }
    ms_session_destroy(s);
}

/* 模式 2：Source/drive，注入短读与 AGAIN；渐进 MP4/MOV 随机 seek。 */
static void run_source(const uint8_t *d, size_t n, run_stat *rs, cnt_alloc *ca)
{
    ms_allocator al = { cnt_alloc_fn, cnt_free_fn, ca };
    ms_session *s = NULL;
    ms_mem_source ms;
    ms_source src;
    int steps = 0;
    int seeks = 0;
    if (ms_session_create(&al, NULL, &s) != MS_OK) {
        rs->errors++;
        return;
    }
    ms_mem_source_init(&ms, d, n);
    ms.max_read = 1 + (rnd() % 32768u);
    ms.again_left = (int)(rnd() % 3);
    ms_mem_source_bind(&ms, &src);
    if (ms_session_set_source(s, &src) != MS_OK) {
        rs->errors++;
        ms_session_destroy(s);
        return;
    }
    while (steps++ < 400000) {
        ms_status st = ms_session_drive(s);
        ms_status ds = drain(s, rs, 1);
        if (rs->au > 0 && seeks < 3 && (rnd() % 97) == 0) {
            ms_status ss = ms_session_seek(s, (int64_t)(rnd() % (5u * 90000u)));
            if (ss == MS_OK) {
                rs->seek_ok++;
            } else {
                rs->seek_other++;
            }
            seeks++;
        }
        if ((rnd() % 5000) == 0) {
            (void)ms_session_cancel(s);
            rs->cancels++;
            break;
        }
        if (st == MS_EOS || ds == MS_EOS) {
            break;
        }
        if (st != MS_OK && st != MS_AGAIN && st != MS_LIMIT) {
            rs->errors++;
            break;
        }
    }
    ms_session_destroy(s);
}

/* 模式 3：随机字节变异 + 截断，再走 push。 */
static void run_mutate(const uint8_t *d, size_t n, run_stat *rs, cnt_alloc *ca)
{
    uint8_t *m;
    size_t i, flips, len;
    if (n == 0) {
        return;
    }
    len = 1 + (rnd() % n);
    m = (uint8_t *)malloc(len);
    if (m == NULL) {
        return;
    }
    memcpy(m, d, len);
    flips = 1 + (rnd() % 64u);
    for (i = 0; i < flips; i++) {
        m[rnd() % len] ^= (uint8_t)(1u << (rnd() % 8));
    }
    run_push(m, len, rs, ca);
    free(m);
}

/* 外挂字幕（SRT/VTT/ASS/SSA/PGS 等）解析 + 变异。 */
static void run_sub(const uint8_t *d, size_t n, ms_sub_format fmt, run_stat *rs, cnt_alloc *ca)
{
    ms_allocator al = { cnt_alloc_fn, cnt_free_fn, ca };
    ms_sub_doc *doc = NULL;
    uint8_t *m;
    size_t i;
    if (ms_sub_doc_create(&al, &doc) != MS_OK) {
        rs->errors++;
        return;
    }
    if (ms_sub_doc_parse(doc, d, n, fmt, 1) != MS_OK) {
        rs->errors++;
    }
    rs->sub += (long)ms_sub_doc_count(doc);
    m = (uint8_t *)malloc(n ? n : 1);
    if (m != NULL && n > 0) {
        memcpy(m, d, n);
        for (i = 0; i < 16; i++) {
            m[rnd() % n] = (uint8_t)rnd();
        }
        ms_sub_doc_clear(doc);
        (void)ms_sub_doc_parse(doc, m, 1 + (rnd() % n), fmt, 1);
    }
    free(m);
    ms_sub_doc_destroy(doc);
}

static uint8_t *load(const char *path, size_t *out_n)
{
    FILE *f = fopen(path, "rb");
    uint8_t *b = NULL;
    long sz;
    if (f == NULL) {
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0 || (sz = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    b = (uint8_t *)malloc((size_t)sz + 1);
    if (b != NULL && fread(b, 1, (size_t)sz, f) != (size_t)sz) {
        free(b);
        b = NULL;
    }
    fclose(f);
    *out_n = (size_t)sz;
    return b;
}

static int is_sub(const char *p)
{
    size_t l = strlen(p);
    return (l > 4 && (strcmp(p + l - 4, ".srt") == 0 || strcmp(p + l - 4, ".vtt") == 0 ||
                      strcmp(p + l - 4, ".ass") == 0 || strcmp(p + l - 4, ".ssa") == 0 ||
                      strcmp(p + l - 4, ".sup") == 0));
}

int main(int argc, char **argv)
{
    int iters, a, i, bad = 0;
    if (argc < 3) {
        fprintf(stderr, "usage: %s <iters> <file>...\n", argv[0]);
        return 2;
    }
    iters = atoi(argv[1]);
    for (a = 2; a < argc; a++) {
        size_t n = 0;
        uint8_t *d = load(argv[a], &n);
        run_stat rs;
        cnt_alloc ca;
        const char *base = strrchr(argv[a], '/');
        base = base ? base + 1 : argv[a];
        memset(&rs, 0, sizeof(rs));
        memset(&ca, 0, sizeof(ca));
        if (d == NULL) {
            fprintf(stderr, "skip %s (read failed)\n", base);
            continue;
        }
        for (i = 0; i < iters; i++) {
            if (is_sub(argv[a])) {
                size_t l = strlen(argv[a]);
                run_sub(d, n, (l > 4 && strcmp(argv[a] + l - 4, ".sup") == 0) ? MS_SUB_FORMAT_PGS
                                                                             : MS_SUB_FORMAT_AUTO,
                        &rs, &ca);
            } else {
                run_push(d, n, &rs, &ca);
                run_source(d, n, &rs, &ca);
                run_mutate(d, n, &rs, &ca);
            }
            if (ca.live_bytes != 0 || ca.live_blocks != 0) {
                fprintf(stderr, "LEAK %s iter=%d live_bytes=%lld blocks=%lld\n", base, i,
                        ca.live_bytes, ca.live_blocks);
                bad = 1;
                break;
            }
        }
        printf("%-34s bytes=%-9zu iters=%d au=%ld cfg=%ld sub=%ld eos=%ld seek_ok=%ld seek_rej=%ld "
               "err=%ld cancel=%ld peak_core=%lld live_after=%lld\n",
               base, n, iters, rs.au, rs.cfg, rs.sub, rs.eos, rs.seek_ok, rs.seek_other,
               rs.errors, rs.cancels, ca.peak_bytes, ca.live_bytes);
        free(d);
    }
    printf(bad ? "RESULT: FAIL\n" : "RESULT: PASS\n");
    return bad;
}
