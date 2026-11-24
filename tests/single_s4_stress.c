/*
 * single_s4_stress.c — 阶段 4 自研网络/转封装/容错模块的 ASan/UBSan/LSan 压力测试：
 *   openclose：高频 open/drive/cancel/close，按 URL 列表轮换（模拟切台），取消时刻随机（含预加载进行中）；
 *   concurrent：多线程同时 openclose（并发线程退出）；
 *   malformed：逐个 URL 打开并驱动到终态（截断/0 字节/伪造 box/坏头/断连/畸形 RTMP/RTSP），只要求不崩溃、不死锁。
 * 输出 STRESS 行；任一 Sanitizer 报错即进程失败。
 * by: Long <mycjzb@gmail.com>
 */
#define _GNU_SOURCE
#include "msmedia/msmedia.h"
#include "../src/netsrc/ms_netsrc.h"
#include "../src/netcore/ms_netcore.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

typedef struct cancel_arg {
    ms_netsrc *n;
    int delay_ms;
} cancel_arg;

static void *cancel_thread(void *p)
{
    cancel_arg *c = (cancel_arg *)p;
    usleep((useconds_t)c->delay_ms * 1000);
    ms_netsrc_cancel(c->n); /* 另一线程取消，模拟 reset/release/切台 */
    return NULL;
}

typedef struct run_res {
    int opened, eos, canceled, errors, aus;
    int64_t max_close_ms, max_run_ms;
} run_res;

/* 一次完整生命周期；cancel_ms<0 不取消，max_ms 为驱动上限。 */
static void one(const char *url, int cancel_ms, int max_ms, unsigned *seed, run_res *rr)
{
    ms_session *s = NULL;
    ms_netsrc *n = NULL;
    ms_netsrc_opts o;
    pthread_t th;
    cancel_arg ca;
    int have_th = 0, done = 0;
    int64_t t0 = now_ms(), c0;
    ms_netsrc_opts_default(&o);
    o.read_timeout_ms = 2000;
    o.connect_timeout_ms = 2000;
    if (rand_r(seed) % 2) {
        o.preload_ahead_ms = 1000 + (int)(rand_r(seed) % 9000);
        o.preload_max_bytes = 200000 + (size_t)(rand_r(seed) % 4000000);
    }
    if (ms_session_create(NULL, NULL, &s) != MS_OK) {
        rr->errors++;
        return;
    }
    if (ms_netsrc_open(url, &o, s, &n) != MS_OK) {
        rr->errors++;
        ms_session_destroy(s);
        return;
    }
    rr->opened++;
    if (cancel_ms >= 0) {
        ca.n = n;
        ca.delay_ms = cancel_ms;
        have_th = pthread_create(&th, NULL, cancel_thread, &ca) == 0;
    }
    while (!done) {
        ms_event ev;
        int prog = 0;
        ms_status r = ms_netsrc_drive(n);
        if (r == MS_OK) {
            prog = 1;
        } else if (r == MS_ERR_CANCELED) {
            rr->canceled++;
            break;
        } else if (r != MS_AGAIN && r != MS_LIMIT && r != MS_EOS) {
            rr->errors++;
            break;
        }
        while (ms_session_pull(s, &ev) == MS_OK) {
            prog = 1;
            if (ev.type == MS_EVENT_AU) rr->aus++;
            if (ev.release_id) ms_session_release(s, ev.release_id);
            if (ev.type == MS_EVENT_EOS) {
                rr->eos++;
                done = 1;
            }
        }
        if (now_ms() - t0 > max_ms) break;
        if (!prog) usleep(1000);
    }
    /* 与 JNI 相同的契约：取消方先于 close 退出（JNI 用 pj->mu 置空指针后再 close）。
     * 取消线程与 drive/预加载线程之间仍是真实并发。 */
    if (have_th) pthread_join(th, NULL);
    c0 = now_ms();
    ms_netsrc_close(n);
    if (now_ms() - c0 > rr->max_close_ms) rr->max_close_ms = now_ms() - c0;
    if (now_ms() - t0 > rr->max_run_ms) rr->max_run_ms = now_ms() - t0;
    ms_session_destroy(s);
}

typedef struct worker {
    char **urls;
    int nurls, iters, id;
    run_res rr;
} worker;

static void *worker_main(void *p)
{
    worker *w = (worker *)p;
    unsigned seed = 1234u + (unsigned)w->id * 7919u;
    int i;
    for (i = 0; i < w->iters; i++) {
        const char *u = w->urls[(i + w->id) % w->nurls];
        int c = (int)(rand_r(&seed) % 60); /* 0–59 ms 后取消：覆盖连接中/预加载中/播放中 */
        one(u, (i % 7 == 6) ? -1 : c, (i % 7 == 6) ? 3000 : 2000, &seed, &w->rr);
    }
    return NULL;
}

int main(int argc, char **argv)
{
    int64_t t0 = now_ms();
    if (argc < 4) {
        fprintf(stderr, "usage: %s openclose|concurrent <iters> [threads] url... | malformed <max_ms> url...\n", argv[0]);
        return 2;
    }
    if (strcmp(argv[1], "malformed") == 0) {
        run_res rr;
        unsigned seed = 7;
        int i, max_ms = atoi(argv[2]);
        memset(&rr, 0, sizeof(rr));
        for (i = 3; i < argc; i++) {
            run_res one_rr;
            memset(&one_rr, 0, sizeof(one_rr));
            one(argv[i], -1, max_ms, &seed, &one_rr);
            rr.opened += one_rr.opened; rr.eos += one_rr.eos; rr.errors += one_rr.errors; rr.aus += one_rr.aus;
            if (one_rr.max_close_ms > rr.max_close_ms) rr.max_close_ms = one_rr.max_close_ms;
            if (one_rr.max_run_ms > rr.max_run_ms) rr.max_run_ms = one_rr.max_run_ms;
        }
        ms_nc_pool_clear();
        printf("STRESS malformed inputs=%d opened=%d eos=%d errors=%d aus=%d max_run_ms=%lld max_close_ms=%lld wall_ms=%lld\n",
               argc - 3, rr.opened, rr.eos, rr.errors, rr.aus, (long long)rr.max_run_ms,
               (long long)rr.max_close_ms, (long long)(now_ms() - t0));
        /* 死锁判据：单个输入的生命周期不超过上限 + 3 s，close 不超过 1 s */
        return (rr.max_run_ms <= max_ms + 3000 && rr.max_close_ms <= 1000) ? 0 : 1;
    }
    {
        int conc = strcmp(argv[1], "concurrent") == 0;
        int iters = atoi(argv[2]);
        int nth = conc ? atoi(argv[3]) : 1;
        int first = conc ? 4 : 3, i;
        worker *ws = (worker *)calloc((size_t)nth, sizeof(worker));
        pthread_t *ths = (pthread_t *)calloc((size_t)nth, sizeof(pthread_t));
        run_res tot;
        memset(&tot, 0, sizeof(tot));
        for (i = 0; i < nth; i++) {
            ws[i].urls = argv + first;
            ws[i].nurls = argc - first;
            ws[i].iters = iters;
            ws[i].id = i;
            pthread_create(&ths[i], NULL, worker_main, &ws[i]);
        }
        for (i = 0; i < nth; i++) {
            pthread_join(ths[i], NULL);
            tot.opened += ws[i].rr.opened; tot.eos += ws[i].rr.eos; tot.canceled += ws[i].rr.canceled;
            tot.errors += ws[i].rr.errors; tot.aus += ws[i].rr.aus;
            if (ws[i].rr.max_close_ms > tot.max_close_ms) tot.max_close_ms = ws[i].rr.max_close_ms;
            if (ws[i].rr.max_run_ms > tot.max_run_ms) tot.max_run_ms = ws[i].rr.max_run_ms;
        }
        free(ws);
        free(ths);
        ms_nc_pool_clear();
        printf("STRESS %s threads=%d iters=%d lifecycles=%d opened=%d eos=%d canceled=%d errors=%d aus=%d max_run_ms=%lld max_close_ms=%lld wall_ms=%lld\n",
               argv[1], nth, iters, nth * iters, tot.opened, tot.eos, tot.canceled, tot.errors, tot.aus,
               (long long)tot.max_run_ms, (long long)tot.max_close_ms, (long long)(now_ms() - t0));
        return (tot.opened == nth * iters && tot.max_close_ms <= 1000) ? 0 : 1;
    }
}
