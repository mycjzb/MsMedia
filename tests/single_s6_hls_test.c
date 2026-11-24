/*
 * 阶段 6 宿主专项测试（ASan/UBSan/LSan）：路线 B 精简 FFmpeg + Media3 风格 HLS 预下载 + 裁剪 OpenSSL。
 * 子命令（服务端见 tests/single_s6_hls_server.py，驱动脚本 tests/single_s6_hls_test.sh）：
 *   play   <url> <min_au> [verify] [min,max,play,rebuf,bytes]  打开并拉帧，打印预下载统计
 *   cancel <url> <after_ms>                                     拉帧线程运行中跨线程 ms_ff_cancel，时延须 <500ms
 *   seek   <url>                                                拉帧→seek 6s→继续拉帧（预下载计划替换）
 *   stress <url> <n>                                            n 轮 打开/拉帧/中途销毁（切台）零泄漏
 *   nc     <url> [ca_file]                                      MsHttpClient 同款 netcore 严格 HTTPS（必须拦截自签）
 * 每个子命令最后一行打印 RESULT ...，返回 0=通过。by: Long <mycjzb@gmail.com>
 */
#include "msmedia/ms_ff.h"
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

/* 创建并按参数配置 ms_ff（TLS 策略 + 缓冲水位）；公用于各子命令。 */
static ms_ff *make_ff(int verify, const char *bufspec)
{
    ms_ff *f = NULL;
    ms_ff_limits lim;
    ms_ff_limits_default(&lim);
    lim.io_timeout_ms = 5000;
    if (ms_ff_create(NULL, &lim, &f) != MS_OK) {
        return NULL;
    }
    if (verify >= 0) {
        ms_ff_set_tls_verify(f, verify);
    }
    if (bufspec != NULL && bufspec[0]) {
        ms_ff_buffer_config c;
        long long b = 0;
        ms_ff_buffer_config_default(&c);
        if (sscanf(bufspec, "%d,%d,%d,%d,%lld", &c.min_buffer_ms, &c.max_buffer_ms, &c.buffer_for_playback_ms,
                   &c.buffer_for_playback_after_rebuffer_ms, &b) == 5) {
            c.max_buffer_bytes = b;
        }
        if (ms_ff_set_buffer_config(f, &c) != MS_OK) {
            printf("set_buffer_config rejected\n");
        }
    }
    return f;
}

/* 拉帧直到 want 个 AU / EOS / 错误；返回 AU 数，*last_st 带回最后状态。 */
static int pull_au(ms_ff *f, int want, ms_status *last_st)
{
    int au = 0, loops = 0;
    ms_status st = MS_OK;
    while (au < want && loops < 2000000) {
        ms_event ev;
        memset(&ev, 0, sizeof(ev));
        st = ms_ff_pull(f, &ev);
        loops++;
        if (st == MS_AGAIN) {
            usleep(200);
            continue;
        }
        if (st != MS_OK) {
            break;
        }
        if (ev.type == MS_EVENT_AU) {
            au++;
        }
        if (ev.release_id) {
            ms_ff_release(f, ev.release_id);
        }
    }
    if (last_st) {
        *last_st = st;
    }
    return au;
}

static void print_stats(ms_ff *f, const char *tag)
{
    ms_ff_buffer_stats s;
    if (ms_ff_get_buffer_stats(f, &s) == MS_OK) {
        printf("%s buffered_ms=%lld bytes=%lld peak=%lld dl=%lld planned=%d done=%d hit=%d miss=%d rebuf=%d "
               "retry=%d conn_open=%d conn_reuse=%d threads=%d\n", tag,
               (long long)s.buffered_ms, (long long)s.buffered_bytes, (long long)s.peak_bytes,
               (long long)s.bytes_downloaded, s.units_planned, s.units_done, s.cache_hits, s.cache_misses,
               s.rebuffers, s.retries, s.conn_opened, s.conn_reused, s.threads);
    }
}

static int cmd_play(const char *url, int min_au, int verify, const char *bufspec)
{
    ms_ff *f = make_ff(verify, bufspec);
    ms_status st, last = MS_OK;
    int64_t t0 = now_ms(), t_open;
    int au = 0, first = 0;
    if (!f) {
        return 1;
    }
    st = ms_ff_open(f, url);
    t_open = now_ms() - t0;
    if (st == MS_OK) {
        first = pull_au(f, 1, &last);
        au = first + pull_au(f, min_au > 1 ? min_au - 1 : 0, &last);
        print_stats(f, "STATS");
    }
    printf("RESULT play open=%s open_ms=%lld au=%d last=%s total_ms=%lld\n", ms_status_name(st),
           (long long)t_open, au, ms_status_name(last), (long long)(now_ms() - t0));
    ms_ff_destroy(f);
    return (st == MS_OK && au >= min_au) ? 0 : 1;
}

typedef struct {
    ms_ff *f;
    const char *url;
    volatile int done;
    int64_t t_done;
} pull_ctx;

static void *pull_thread(void *p)
{
    pull_ctx *c = (pull_ctx *)p;
    if (ms_ff_open(c->f, c->url) == MS_OK) {
        pull_au(c->f, 1 << 30, NULL);
    }
    c->t_done = now_ms();
    c->done = 1;
    return NULL;
}

static int cmd_cancel(const char *url, int after_ms)
{
    pull_ctx c;
    pthread_t th;
    int64_t t_cancel, lat;
    memset(&c, 0, sizeof(c));
    c.f = make_ff(-1, NULL);
    c.url = url;
    if (!c.f) {
        return 1;
    }
    pthread_create(&th, NULL, pull_thread, &c);
    usleep((useconds_t)after_ms * 1000);
    t_cancel = now_ms();
    ms_ff_cancel(c.f);
    pthread_join(th, NULL);
    lat = c.t_done - t_cancel;
    ms_ff_destroy(c.f);
    printf("RESULT cancel after=%d latency_ms=%lld\n", after_ms, (long long)lat);
    return lat < 500 ? 0 : 1;
}

static int cmd_seek(const char *url)
{
    ms_ff *f = make_ff(-1, NULL);
    ms_status st, last = MS_OK, sst = MS_ERR_STATE;
    int a1 = 0, a2 = 0;
    if (!f) {
        return 1;
    }
    st = ms_ff_open(f, url);
    if (st == MS_OK) {
        a1 = pull_au(f, 60, &last);
        sst = ms_ff_seek(f, (int64_t)6 * 90000);
        a2 = pull_au(f, 60, &last);
        print_stats(f, "STATS");
    }
    printf("RESULT seek open=%s a1=%d seek=%s a2=%d\n", ms_status_name(st), a1, ms_status_name(sst), a2);
    ms_ff_destroy(f);
    return (st == MS_OK && sst == MS_OK && a1 >= 60 && a2 >= 30) ? 0 : 1;
}

static int cmd_stress(const char *url, int n)
{
    int ok = 0;
    for (int i = 0; i < n; i++) {
        ms_ff *f = make_ff(-1, NULL);
        if (!f) {
            continue;
        }
        if (ms_ff_open(f, url) == MS_OK && pull_au(f, 10 + (i % 5) * 20, NULL) >= 10) {
            ok++;
        }
        ms_ff_destroy(f); /* 预下载线程仍在下载时直接销毁 = 切台 */
    }
    printf("RESULT stress n=%d ok=%d\n", n, ok);
    return ok == n ? 0 : 1;
}

static int cmd_nc(const char *url, const char *ca)
{
    ms_nc_req r;
    ms_nc_http *h = NULL;
    uint8_t buf[4096];
    int st, n = 0, k;
    if (ca && ca[0]) {
        ms_nc_tls_set_extra_ca(ca, NULL);
    }
    memset(&r, 0, sizeof(r));
    r.url = url;
    r.connect_timeout_ms = 3000;
    r.read_timeout_ms = 3000;
    st = ms_nc_http_open(&r, &h);
    if (st == 0 && h) {
        while ((k = ms_nc_http_read(h, buf, sizeof(buf))) > 0) {
            n += k;
        }
        printf("RESULT nc open=0 status=%d bytes=%d backend=%s\n", ms_nc_http_status(h), n, ms_nc_tls_backend());
        ms_nc_http_close(h);
    } else {
        printf("RESULT nc open=%d (blocked) backend=%s\n", st, ms_nc_tls_backend());
    }
    ms_nc_pool_clear();
    ms_nc_tls_global_cleanup();
    return st == 0 ? 0 : 1;
}

int main(int argc, char **argv)
{
    int rc = 2;
    if (argc < 3) {
        fprintf(stderr, "usage: %s play|cancel|seek|stress|nc url ...\n", argv[0]);
        return 2;
    }
    if (!strcmp(argv[1], "play")) {
        rc = cmd_play(argv[2], argc > 3 ? atoi(argv[3]) : 100, argc > 4 ? atoi(argv[4]) : -1, argc > 5 ? argv[5] : NULL);
    } else if (!strcmp(argv[1], "cancel")) {
        rc = cmd_cancel(argv[2], argc > 3 ? atoi(argv[3]) : 200);
    } else if (!strcmp(argv[1], "seek")) {
        rc = cmd_seek(argv[2]);
    } else if (!strcmp(argv[1], "stress")) {
        rc = cmd_stress(argv[2], argc > 3 ? atoi(argv[3]) : 20);
    } else if (!strcmp(argv[1], "nc")) {
        rc = cmd_nc(argv[2], argc > 3 ? argv[3] : NULL);
    }
    ms_nc_pool_clear();
    ms_nc_tls_global_cleanup();
    return rc;
}
