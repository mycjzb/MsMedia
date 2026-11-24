/*
 * HLS 独立预下载器实现（对标 Google Media3：HlsMediaSource 的分片加载 + DefaultLoadControl 水位）。
 * 设计说明见 ms_hls_pf.h。本文件用 FFmpeg 内部 URL 接口（ffurl_* / ff_http_do_new_request2），
 * 在 libmsplayer.so 最终链接时编译（与 tcp_ms.c / tls_android.c 同一编译方式），不进 FFmpeg 构建系统。
 * by: Long <mycjzb@gmail.com>
 */
#include "ms_hls_pf.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "libavutil/aes.h"
#include "libavutil/avstring.h"
#include "libavutil/dict.h"
#include "libavutil/mem.h"
#include "libavutil/time.h"
#include "url.h"
#include "http.h"

#define PF_MAX_PLS 8              /* 主视频 + 多音轨/字幕 rendition，够用且有界 */
#define PF_MAX_UNITS 256          /* 单个 playlist 计划上限（VOD 长片只计划前 256 个单元，随消费滚动） */
#define PF_CHUNK (32 * 1024)      /* 单次网络读 */
#define PF_COMPACT (256 * 1024)   /* 读端已消费超过它就把缓冲前移，释放内存 */
#define PF_RETRY 3                /* Media3 风格：瞬时错误最多重试 3 次 */
#define PF_ABS_MAX_BYTES ((int64_t)256 << 20)
#define PF_READER_MAGIC 0x4d534846u /* "MSHF" */
#define PF_AVIO_BUF 32768

enum { U_QUEUED = 0, U_LOADING, U_DONE, U_FAILED };

struct pf_pls;

/* 一个请求单元：一个切片，或同 URL 连续 BYTERANGE 合并后的一段（与 hls.c range_end 一致）。
 * 偏移全部相对单元起点：buf[0] 对应 base，已下载到 got，读端已消费到 rd。 */
typedef struct pf_unit {
    struct pf_unit *next;
    struct pf_pls *pl;
    char *url;
    int64_t off, end;   /* BYTERANGE 绝对区间 [off, end)；off<0 表示整文件 */
    int64_t dur_us;
    uint8_t *buf;
    size_t cap;
    int64_t base, got, rd;
    int64_t total;      /* 预期字节数，-1 未知 */
    int state;
    int err;
    int in_plan;        /* 仍在计划链表中 */
    int attached;       /* 有读端挂着 */
    volatile int dropped; /* 下载中被移出计划：立即中断 */
    int refs;           /* 链表 1 + 下载线程 1 + 读端 1 */
} pf_unit;

typedef struct pf_pls {
    struct ms_hls_pf *pf;
    int index;
    int live;
    int64_t target_us;
    pf_unit *head;              /* 当前计划（按播放顺序） */
    pf_unit *nhead, **ntail;    /* 正在构建的新计划（只在 demux 线程） */
    int nbuild;
    AVDictionary *opts;         /* hls.c 的 avio_opts（UA/headers/cookies 等） */
    pthread_t th;
    int th_ok;
    int loading;                /* 水位滞回：1=预拉中 */
    int started;                /* 已放行过首个单元（之后的等待算重缓冲） */
    pf_unit *cur;               /* 正在下载的单元 */
    URLContext *uc;             /* Keep-Alive 连接（只在下载线程使用） */
} pf_pls;

struct ms_hls_pf {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    ms_ff_buffer_config cfg;
    /* 不保存 AVFormatContext 指针：下载线程可能比 ic 活得久（avformat_close_input 先于 destroy），
     * 所以创建时就把线程要用的白/黑名单与中断回调拷贝一份（ASan 抓到过 UAF，已修）。 */
    char *wl, *bl;
    AVIOInterruptCB ic_icb;
    ms_hls_pf_opt_fn opt_fn;
    ms_hls_pf_cancel_fn cancel_fn;
    void *ctx;
    int quit;
    int refs;                   /* 创建者 1 + 每个读端 1 */
    pf_pls *pls[PF_MAX_PLS];
    int npls;
    int64_t bytes;              /* 当前缓冲占用 */
    ms_ff_buffer_stats st;
};

typedef struct pf_reader {
    uint32_t magic;
    ms_hls_pf *pf;
    pf_unit *u;
    AVIOInterruptCB icb;
    int gated;
    struct AVAES *aes;          /* AES-128 时非 NULL：复用 FFmpeg libavutil AES */
    uint8_t iv[16];
    uint8_t out[4096];
    int out_pos, out_len;
} pf_reader;

/* ---------------- 公共小工具 ---------------- */
static void cv_wait_ms(ms_hls_pf *pf, int ms)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_nsec += (long)ms * 1000000L;
    ts.tv_sec += ts.tv_nsec / 1000000000L;
    ts.tv_nsec %= 1000000000L;
    pthread_cond_timedwait(&pf->cv, &pf->mu, &ts);
}

static int pf_stopping(ms_hls_pf *pf)
{
    return pf->quit || (pf->cancel_fn && pf->cancel_fn(pf->ctx));
}

/* 锁内调用：引用归零即释放单元及其缓冲。 */
static void unit_unref(ms_hls_pf *pf, pf_unit *u)
{
    if (u == NULL || --u->refs > 0) return;
    pf->bytes -= (u->got - u->base);
    av_free(u->buf);
    av_free(u->url);
    av_free(u);
}

/* 锁内调用：把单元移出计划（下载中则通知中断）。 */
static void unit_remove(ms_hls_pf *pf, pf_unit *u)
{
    if (!u->in_plan) return;
    u->in_plan = 0;
    if (u->state == U_LOADING) u->dropped = 1;
    unit_unref(pf, u);
}

/* 锁内调用：单元已下载未消费的媒体时长（微秒）。大小未知且未下完时按 0 计。 */
static int64_t unit_ahead_us(const pf_unit *u)
{
    int64_t have = u->got - u->rd;
    if (have <= 0 || u->dur_us <= 0) return 0;
    if (u->total > 0) return av_rescale(u->dur_us, have, u->total);
    if (u->state == U_DONE && u->got > 0) return av_rescale(u->dur_us, have, u->got);
    return 0;
}

static int64_t pls_ahead_us(const pf_pls *pl)
{
    int64_t s = 0;
    const pf_unit *u;
    for (u = pl->head; u; u = u->next) s += unit_ahead_us(u);
    return s;
}

static int pls_all_done(const pf_pls *pl)
{
    const pf_unit *u;
    for (u = pl->head; u; u = u->next)
        if (u->state != U_DONE && u->state != U_FAILED) return 0;
    return 1;
}

static pf_pls *pls_get(ms_hls_pf *pf, int index, int create)
{
    int i;
    for (i = 0; i < pf->npls; i++)
        if (pf->pls[i]->index == index) return pf->pls[i];
    if (!create || pf->npls >= PF_MAX_PLS) return NULL;
    pf->pls[pf->npls] = av_mallocz(sizeof(pf_pls));
    if (!pf->pls[pf->npls]) return NULL;
    pf->pls[pf->npls]->pf = pf;
    pf->pls[pf->npls]->index = index;
    pf->pls[pf->npls]->loading = 1;
    return pf->pls[pf->npls++];
}

/* ---------------- 下载线程 ---------------- */
static int dl_int_cb(void *opaque)
{
    pf_pls *pl = opaque;
    pf_unit *u = pl->cur;
    return pf_stopping(pl->pf) || (u && u->dropped);
}

/* 锁内：选下一个要下载的单元。计划首个未完成单元（马上要播）总是允许；
 * 其余按 Media3 水位滞回：预拉中达到 max_ms 或内存上限即暂停，跌破 min_ms 且内存未满才恢复。 */
static pf_unit *pick(pf_pls *pl)
{
    ms_hls_pf *pf = pl->pf;
    pf_unit *u, *first_open = NULL;
    int64_t ahead;
    for (u = pl->head; u; u = u->next) {
        if (u->state == U_DONE) continue;
        if (!first_open) first_open = u;
        if (u->state == U_QUEUED) break;
        if (u->state == U_LOADING) return NULL; /* 同一 playlist 串行下载 */
    }
    if (!u) return NULL;
    if (u == first_open && u == pl->head) return u; /* 当前切片：不受水位限制，保证边下边播 */
    ahead = pls_ahead_us(pl);
    if (pl->loading) {
        if (ahead >= (int64_t)pf->cfg.max_buffer_ms * 1000 || pf->bytes >= pf->cfg.max_buffer_bytes)
            pl->loading = 0;
    } else if (ahead < (int64_t)pf->cfg.min_buffer_ms * 1000 && pf->bytes < pf->cfg.max_buffer_bytes) {
        pl->loading = 1;
    }
    return pl->loading ? u : NULL;
}

/* 锁外调用：把一块数据追加进单元（锁内做流控 + 扩容）。返回 0 成功，<0 中断/内存不足。 */
static int unit_append(pf_pls *pl, pf_unit *u, const uint8_t *d, int n)
{
    ms_hls_pf *pf = pl->pf;
    int ret = 0;
    pthread_mutex_lock(&pf->mu);
    /* 单元内流控：未消费数据达到内存上限就等读端（防整文件 BYTERANGE 把内存吃满）。 */
    while (!pf_stopping(pf) && !u->dropped && u->got - u->rd >= pf->cfg.max_buffer_bytes)
        cv_wait_ms(pf, 50);
    if (pf_stopping(pf) || u->dropped) {
        ret = AVERROR_EXIT;
    } else {
        size_t need = (size_t)(u->got - u->base) + (size_t)n;
        if (need > u->cap) {
            size_t nc = u->cap ? u->cap * 2 : 65536;
            uint8_t *nb;
            while (nc < need) nc *= 2;
            nb = av_realloc(u->buf, nc);
            if (!nb) {
                ret = AVERROR(ENOMEM);
                goto out;
            }
            u->buf = nb;
            u->cap = nc;
        }
        memcpy(u->buf + (u->got - u->base), d, n);
        u->got += n;
        pf->bytes += n;
        pf->st.bytes_downloaded += n;
        if (pf->bytes > pf->st.peak_bytes) pf->st.peak_bytes = pf->bytes;
        pthread_cond_broadcast(&pf->cv);
    }
out:
    pthread_mutex_unlock(&pf->mu);
    return ret;
}

/* 一次下载尝试：Keep-Alive 复用连接发新请求，失败则新建；断点续传（Range 从已下载处继续）。 */
static int dl_attempt(pf_pls *pl, pf_unit *u, uint8_t *tmp)
{
    ms_hls_pf *pf = pl->pf;
    AVDictionary *opts = NULL;
    AVIOInterruptCB icb = { dl_int_cb, pl };
    int64_t got0, total;
    int r;

    pthread_mutex_lock(&pf->mu);
    av_dict_copy(&opts, pl->opts, 0);
    got0 = u->got;
    pthread_mutex_unlock(&pf->mu);
    if (pf->opt_fn) pf->opt_fn(pf->ctx, &opts);
    av_dict_set(&opts, "multiple_requests", "1", 0);
    av_dict_set(&opts, "reconnect", "0", 0); /* 重试由本模块按 Media3 策略统一负责 */
    if (u->off >= 0) {
        av_dict_set_int(&opts, "offset", u->off + got0, 0);
        av_dict_set_int(&opts, "end_offset", u->end, 0);
    } else if (got0 > 0) {
        av_dict_set_int(&opts, "offset", got0, 0);
    }
    r = -1;
    if (pl->uc) {
        r = ff_http_do_new_request2(pl->uc, u->url, &opts);
        if (r < 0) {
            ffurl_closep(&pl->uc);
        } else {
            pthread_mutex_lock(&pf->mu);
            pf->st.conn_reused++;
            pthread_mutex_unlock(&pf->mu);
        }
    }
    if (!pl->uc) {
        r = ffurl_open_whitelist(&pl->uc, u->url, AVIO_FLAG_READ, &icb, &opts,
                                 pf->wl, pf->bl, NULL);
        if (r >= 0) {
            pthread_mutex_lock(&pf->mu);
            pf->st.conn_opened++;
            pthread_mutex_unlock(&pf->mu);
        }
    }
    av_dict_free(&opts);
    if (r < 0) {
        ffurl_closep(&pl->uc);
        return r;
    }
    pl->uc->interrupt_callback = icb;
    if (u->off >= 0) {
        total = u->end - u->off;
    } else {
        int64_t sz = ffurl_size(pl->uc);
        total = (sz > 0 && got0 == 0) ? sz : -1;
    }
    pthread_mutex_lock(&pf->mu);
    if (u->total < 0 && total > 0) u->total = total;
    total = u->total;
    pthread_mutex_unlock(&pf->mu);
    for (;;) {
        int64_t want = PF_CHUNK;
        if (total > 0) {
            int64_t left = total - u->got;
            if (left <= 0) break;
            if (left < want) want = left;
        }
        r = ffurl_read(pl->uc, tmp, (int)want);
        if (r == AVERROR_EOF || r == 0) {
            if (total > 0 && u->got < total) {
                ffurl_closep(&pl->uc);
                return AVERROR(EIO); /* 短读：按瞬时错误重试（续传） */
            }
            break;
        }
        if (r < 0) {
            ffurl_closep(&pl->uc);
            return r;
        }
        r = unit_append(pl, u, tmp, r);
        if (r < 0) {
            ffurl_closep(&pl->uc); /* 响应没读完，连接不能复用 */
            return r;
        }
    }
    return 0;
}

static int dl_unit(pf_pls *pl, pf_unit *u, uint8_t *tmp)
{
    ms_hls_pf *pf = pl->pf;
    int attempt = 0, r;
    for (;;) {
        r = dl_attempt(pl, u, tmp);
        if (r >= 0) return 0;
        if (r == AVERROR_EXIT || pf_stopping(pf) || u->dropped) return AVERROR_EXIT;
        if (r == AVERROR(ENOMEM) || attempt >= PF_RETRY) return r;
        /* 4xx 属于确定性错误，不重试（与 Media3 DefaultLoadErrorHandlingPolicy 一致，只重试 IO/5xx 类） */
        if (r == AVERROR_HTTP_BAD_REQUEST || r == AVERROR_HTTP_UNAUTHORIZED ||
            r == AVERROR_HTTP_FORBIDDEN || r == AVERROR_HTTP_NOT_FOUND || r == AVERROR_HTTP_OTHER_4XX)
            return r;
        {
            int64_t end = av_gettime_relative() + ((int64_t)50000 << attempt); /* 50/100/200 ms */
            while (av_gettime_relative() < end) {
                if (pf_stopping(pf) || u->dropped) return AVERROR_EXIT;
                av_usleep(10000);
            }
        }
        attempt++;
        pthread_mutex_lock(&pf->mu);
        pf->st.retries++;
        pthread_mutex_unlock(&pf->mu);
    }
}

static void *dl_thread(void *arg)
{
    pf_pls *pl = arg;
    ms_hls_pf *pf = pl->pf;
    uint8_t *tmp = av_malloc(PF_CHUNK);
    pthread_mutex_lock(&pf->mu);
    while (tmp && !pf_stopping(pf)) {
        pf_unit *u = pick(pl);
        int r;
        if (!u) {
            cv_wait_ms(pf, 100); /* 定时醒来：外部 cancel 无需额外唤醒也能在 100 ms 内退出 */
            continue;
        }
        u->state = U_LOADING;
        u->refs++;
        pl->cur = u;
        pthread_mutex_unlock(&pf->mu);
        r = dl_unit(pl, u, tmp);
        pthread_mutex_lock(&pf->mu);
        pl->cur = NULL;
        if (r == 0) {
            u->state = U_DONE;
            pf->st.units_done++;
        } else {
            u->state = U_FAILED;
            u->err = r;
        }
        pthread_cond_broadcast(&pf->cv);
        unit_unref(pf, u);
    }
    pthread_mutex_unlock(&pf->mu);
    ffurl_closep(&pl->uc);
    av_free(tmp);
    return NULL;
}

/* ---------------- 生命周期 ---------------- */
ms_hls_pf *ms_hls_pf_create(const ms_ff_buffer_config *cfg, AVFormatContext *ic,
                            ms_hls_pf_opt_fn opt_fn, ms_hls_pf_cancel_fn cancel_fn, void *ctx)
{
    ms_hls_pf *pf;
    if (cfg == NULL || ic == NULL || cfg->max_buffer_ms <= 0) return NULL;
    pf = av_mallocz(sizeof(*pf));
    if (pf == NULL) return NULL;
    if (pthread_mutex_init(&pf->mu, NULL) != 0) {
        av_free(pf);
        return NULL;
    }
    if (pthread_cond_init(&pf->cv, NULL) != 0) {
        pthread_mutex_destroy(&pf->mu);
        av_free(pf);
        return NULL;
    }
    pf->cfg = *cfg;
    /* 参数兜底：内存上限 (0,256MiB]、min<=max、门槛非负。 */
    if (pf->cfg.max_buffer_bytes <= 0) pf->cfg.max_buffer_bytes = (int64_t)16 << 20;
    if (pf->cfg.max_buffer_bytes > PF_ABS_MAX_BYTES) pf->cfg.max_buffer_bytes = PF_ABS_MAX_BYTES;
    if (pf->cfg.min_buffer_ms < 0) pf->cfg.min_buffer_ms = 0;
    if (pf->cfg.min_buffer_ms > pf->cfg.max_buffer_ms) pf->cfg.min_buffer_ms = pf->cfg.max_buffer_ms;
    if (pf->cfg.buffer_for_playback_ms < 0) pf->cfg.buffer_for_playback_ms = 0;
    if (pf->cfg.buffer_for_playback_after_rebuffer_ms < 0) pf->cfg.buffer_for_playback_after_rebuffer_ms = 0;
    if ((ic->protocol_whitelist && !(pf->wl = av_strdup(ic->protocol_whitelist))) ||
        (ic->protocol_blacklist && !(pf->bl = av_strdup(ic->protocol_blacklist)))) {
        av_freep(&pf->wl);
        pthread_cond_destroy(&pf->cv);
        pthread_mutex_destroy(&pf->mu);
        av_free(pf);
        return NULL;
    }
    pf->ic_icb = ic->interrupt_callback;
    pf->opt_fn = opt_fn;
    pf->cancel_fn = cancel_fn;
    pf->ctx = ctx;
    pf->refs = 1;
    return pf;
}

static void pf_unref(ms_hls_pf *pf)
{
    int last;
    pthread_mutex_lock(&pf->mu);
    last = (--pf->refs == 0);
    pthread_mutex_unlock(&pf->mu);
    if (!last) return;
    pthread_cond_destroy(&pf->cv);
    pthread_mutex_destroy(&pf->mu);
    av_freep(&pf->wl);
    av_freep(&pf->bl);
    av_free(pf);
}

void ms_hls_pf_destroy(ms_hls_pf *pf)
{
    int i;
    if (pf == NULL) return;
    pthread_mutex_lock(&pf->mu);
    pf->quit = 1;
    pthread_cond_broadcast(&pf->cv);
    pthread_mutex_unlock(&pf->mu);
    for (i = 0; i < pf->npls; i++)
        if (pf->pls[i]->th_ok) pthread_join(pf->pls[i]->th, NULL); /* 不留僵尸线程 */
    pthread_mutex_lock(&pf->mu);
    for (i = 0; i < pf->npls; i++) {
        pf_pls *pl = pf->pls[i];
        while (pl->head) {
            pf_unit *u = pl->head;
            pl->head = u->next;
            unit_remove(pf, u);
        }
        while (pl->nhead) {
            pf_unit *u = pl->nhead;
            pl->nhead = u->next;
            unit_unref(pf, u);
        }
        av_dict_free(&pl->opts);
        av_free(pl);
        pf->pls[i] = NULL;
    }
    pf->npls = 0;
    pthread_mutex_unlock(&pf->mu);
    pf_unref(pf);
}

/* ---------------- 计划（hls.c 补丁经 ms_ff 转发调用，均在 demux 线程） ---------------- */
void ms_hls_pf_plan_begin_pf(ms_hls_pf *pf, int pls_index, int live, int64_t target_us)
{
    pf_pls *pl;
    if (pf == NULL) return;
    pthread_mutex_lock(&pf->mu);
    pl = pls_get(pf, pls_index, 1);
    if (pl) {
        while (pl->nhead) { /* 上一次未提交的残留（理论上不会有） */
            pf_unit *u = pl->nhead;
            pl->nhead = u->next;
            unit_unref(pf, u);
        }
        pl->ntail = &pl->nhead;
        pl->nbuild = 0;
        pl->live = live;
        pl->target_us = target_us;
    }
    pthread_mutex_unlock(&pf->mu);
}

void ms_hls_pf_plan_add_pf(ms_hls_pf *pf, int pls_index, const char *url, int64_t off, int64_t end, int64_t dur_us)
{
    pf_pls *pl;
    pf_unit *u;
    if (pf == NULL || url == NULL) return;
    pthread_mutex_lock(&pf->mu);
    pl = pls_get(pf, pls_index, 0);
    if (pl && pl->ntail && pl->nbuild < PF_MAX_UNITS && (u = av_mallocz(sizeof(*u))) != NULL) {
        u->url = av_strdup(url);
        if (!u->url) {
            av_free(u);
        } else {
            u->pl = pl;
            u->off = off;
            u->end = end;
            u->dur_us = dur_us;
            u->total = (off >= 0 && end > off) ? end - off : -1;
            u->refs = 1;
            u->in_plan = 1;
            *pl->ntail = u;
            pl->ntail = &u->next;
            pl->nbuild++;
        }
    }
    pthread_mutex_unlock(&pf->mu);
}

static int unit_same(const pf_unit *a, const pf_unit *b)
{
    return a->off == b->off && a->end == b->end && !strcmp(a->url, b->url);
}

void ms_hls_pf_plan_end_pf(ms_hls_pf *pf, int pls_index, AVDictionary *opts)
{
    pf_pls *pl;
    pf_unit *n, *o, *merged = NULL, **mt = &merged;
    if (pf == NULL) return;
    pthread_mutex_lock(&pf->mu);
    pl = pls_get(pf, pls_index, 0);
    if (pl == NULL || pl->ntail == NULL) goto out;
    /* 新计划逐项复用已下载 / 下载中的旧单元（切台内续播不丢缓冲），旧计划其余单元移出（在途下载被中断）。 */
    while ((n = pl->nhead) != NULL) {
        pl->nhead = n->next;
        n->next = NULL;
        for (o = pl->head; o; o = o->next)
            if (o->in_plan && !o->attached && unit_same(o, n)) break;
        if (o) {
            pf_unit **pp = &pl->head;
            while (*pp != o) pp = &(*pp)->next;
            *pp = o->next; /* 从旧链摘下（保留 in_plan 与引用） */
            o->next = NULL;
            unit_unref(pf, n);
            n = o;
        } else {
            pf->st.units_planned++;
        }
        *mt = n;
        mt = &n->next;
    }
    while ((o = pl->head) != NULL) {
        pl->head = o->next;
        o->next = NULL;
        if (o->attached) {
            o->in_plan = 0; /* 读端还挂着：只移出计划，由读端关闭时释放 */
            unit_unref(pf, o);
        } else {
            unit_remove(pf, o);
        }
    }
    pl->head = merged;
    pl->ntail = NULL;
    av_dict_free(&pl->opts);
    av_dict_copy(&pl->opts, opts, 0);
    if (!pl->th_ok && !pf->quit) {
        if (pthread_create(&pl->th, NULL, dl_thread, pl) == 0) {
            pl->th_ok = 1;
            pf->st.threads++;
        }
    }
    pthread_cond_broadcast(&pf->cv);
out:
    pthread_mutex_unlock(&pf->mu);
}

/* ---------------- 读端 ---------------- */
/* 锁内：单元是否可放行（Media3 起播 / 重缓冲门槛）。大小未知时有数据即放行（直播 chunked）。 */
static int unit_ready(const pf_unit *u, int thr_ms)
{
    const pf_pls *pl = u->pl;
    if (u->state == U_DONE || u->state == U_FAILED) return 1;
    if (u->total < 0) return u->got > u->rd;
    if (pls_ahead_us(pl) >= (int64_t)thr_ms * 1000) return 1;
    return pls_all_done(pl);
}

/* 锁内：等到至少 need 字节可读或单元结束；首读做起播/重缓冲门槛。返回 0 或 AVERROR_EXIT。 */
static int rd_wait(pf_reader *r, int64_t need)
{
    ms_hls_pf *pf = r->pf;
    pf_unit *u = r->u;
    if (!r->gated) {
        pf_pls *pl = u->pl;
        int thr = pl->started ? pf->cfg.buffer_for_playback_after_rebuffer_ms : pf->cfg.buffer_for_playback_ms;
        r->gated = 1;
        if (!unit_ready(u, thr)) {
            if (pl->started) pf->st.rebuffers++;
            while (!unit_ready(u, thr)) {
                if (pf->quit || ff_check_interrupt(&r->icb)) return AVERROR_EXIT;
                cv_wait_ms(pf, 20);
            }
        }
        pl->started = 1;
    }
    while (u->got - u->rd < need && u->state != U_DONE && u->state != U_FAILED) {
        if (pf->quit || ff_check_interrupt(&r->icb)) return AVERROR_EXIT;
        cv_wait_ms(pf, 20);
    }
    return 0;
}

/* 锁内：读端前移后压缩缓冲，及时把内存还给水位。 */
static void rd_compact(ms_hls_pf *pf, pf_unit *u)
{
    int64_t drop = u->rd - u->base;
    if (drop < PF_COMPACT || drop * 2 < u->got - u->base) return;
    memmove(u->buf, u->buf + drop, (size_t)(u->got - u->rd));
    u->base = u->rd;
    pf->bytes -= drop;
}

static int rd_read_plain(pf_reader *r, uint8_t *buf, int n)
{
    ms_hls_pf *pf = r->pf;
    pf_unit *u = r->u;
    int64_t avail;
    int k, ret;
    pthread_mutex_lock(&pf->mu);
    ret = rd_wait(r, 1);
    if (ret < 0) goto out;
    avail = u->got - u->rd;
    if (avail <= 0) {
        ret = u->state == U_DONE ? AVERROR_EOF : (u->err < 0 ? u->err : AVERROR(EIO));
        goto out;
    }
    k = (int)FFMIN((int64_t)n, avail);
    memcpy(buf, u->buf + (u->rd - u->base), k);
    u->rd += k;
    rd_compact(pf, u);
    pthread_cond_broadcast(&pf->cv);
    ret = k;
out:
    pthread_mutex_unlock(&pf->mu);
    return ret;
}

/* AES-128-CBC 流式解密：留最后一个块到单元下完再解，以去掉 PKCS#7 填充（与 FFmpeg crypto 协议语义一致）。 */
static int rd_read_aes(pf_reader *r, uint8_t *buf, int n)
{
    ms_hls_pf *pf = r->pf;
    pf_unit *u = r->u;
    int ret = 0;
    if (r->out_pos >= r->out_len) {
        int64_t avail, blocks;
        int done;
        pthread_mutex_lock(&pf->mu);
        for (;;) {
            ret = rd_wait(r, 32);
            if (ret < 0) break;
            avail = u->got - u->rd;
            done = (u->state == U_DONE);
            if (u->state == U_FAILED && avail < 32) {
                ret = u->err < 0 ? u->err : AVERROR(EIO);
                break;
            }
            blocks = done ? avail / 16 : avail / 16 - 1;
            if (blocks <= 0) {
                if (done) {
                    ret = AVERROR_EOF;
                    break;
                }
                cv_wait_ms(pf, 20);
                continue;
            }
            if (blocks > (int64_t)(sizeof(r->out) / 16)) blocks = sizeof(r->out) / 16;
            av_aes_crypt(r->aes, r->out, u->buf + (u->rd - u->base), (int)blocks, r->iv, 1);
            u->rd += blocks * 16;
            r->out_pos = 0;
            r->out_len = (int)blocks * 16;
            if (done && u->got - u->rd < 16) {
                int pad = r->out[r->out_len - 1];
                if (pad >= 1 && pad <= 16 && pad <= r->out_len) r->out_len -= pad;
            }
            rd_compact(pf, u);
            pthread_cond_broadcast(&pf->cv);
            ret = 0;
            break;
        }
        pthread_mutex_unlock(&pf->mu);
        if (ret < 0) return ret;
        if (r->out_len == 0) return AVERROR_EOF;
    }
    ret = FFMIN(n, r->out_len - r->out_pos);
    memcpy(buf, r->out + r->out_pos, ret);
    r->out_pos += ret;
    return ret;
}

static int rd_read(void *opaque, uint8_t *buf, int n)
{
    pf_reader *r = opaque;
    if (r == NULL || r->magic != PF_READER_MAGIC || n <= 0) return AVERROR(EINVAL);
    return r->aes ? rd_read_aes(r, buf, n) : rd_read_plain(r, buf, n);
}

static int hex16(const char *s, uint8_t out[16])
{
    int i;
    if (s == NULL || strlen(s) != 32) return -1;
    for (i = 0; i < 32; i++) {
        int c = s[i], v;
        if (c >= '0' && c <= '9') v = c - '0';
        else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
        else return -1;
        if (i & 1) out[i / 2] |= (uint8_t)v;
        else out[i / 2] = (uint8_t)(v << 4);
    }
    return 0;
}

int ms_hls_pf_open(ms_hls_pf *pf, const char *url, AVDictionary *opts, AVIOContext **pb)
{
    const char *inner = url;
    const AVDictionaryEntry *e;
    int64_t off = -1, end = -1;
    int crypto = 0, i;
    pf_unit *u = NULL;
    pf_reader *r;
    uint8_t key[16];
    uint8_t *iobuf;

    if (pf == NULL || url == NULL || pb == NULL) return 0;
    if (!strncmp(url, "crypto+", 7) || !strncmp(url, "crypto:", 7)) {
        inner = url + 7;
        crypto = 1;
    }
    if (strncmp(inner, "http", 4)) return 0;
    if ((e = av_dict_get(opts, "offset", NULL, 0)) != NULL) off = strtoll(e->value, NULL, 10);
    if ((e = av_dict_get(opts, "end_offset", NULL, 0)) != NULL) end = strtoll(e->value, NULL, 10);
    if (off <= 0 && end < 0) off = -1;
    r = av_mallocz(sizeof(*r));
    if (r == NULL) return AVERROR(ENOMEM);
    if (crypto) {
        if (hex16(av_dict_get(opts, "key", NULL, 0) ? av_dict_get(opts, "key", NULL, 0)->value : NULL, key) < 0 ||
            hex16(av_dict_get(opts, "iv", NULL, 0) ? av_dict_get(opts, "iv", NULL, 0)->value : NULL, r->iv) < 0 ||
            (r->aes = av_aes_alloc()) == NULL || av_aes_init(r->aes, key, 128, 1) < 0) {
            av_free(r->aes);
            av_free(r);
            return 0; /* 参数不全：交回 FFmpeg crypto 协议原路径 */
        }
    }
    pthread_mutex_lock(&pf->mu);
    for (i = 0; i < pf->npls && !u; i++) {
        pf_unit *c;
        for (c = pf->pls[i]->head; c; c = c->next) {
            if (c->in_plan && !c->attached && c->off == off && (off < 0 || c->end == end) && !strcmp(c->url, inner)) {
                u = c;
                break;
            }
        }
    }
    if (u && u->state == U_FAILED && u->got == 0) {
        /* 预下载已彻底失败且无数据：移出计划，交回 FFmpeg 直连（其自身还有 seg_max_retry 兜底） */
        pf_unit **pp = &u->pl->head;
        while (*pp && *pp != u) pp = &(*pp)->next;
        if (*pp) *pp = u->next;
        u->next = NULL;
        unit_remove(pf, u);
        u = NULL;
    }
    if (u == NULL) {
        pf->st.cache_misses++;
        pthread_mutex_unlock(&pf->mu);
        av_free(r->aes);
        av_free(r);
        return 0;
    }
    /* 命中：计划中排在它前面的单元已被跳过（seek / 跳片），立即释放。 */
    while (u->pl->head && u->pl->head != u) {
        pf_unit *s = u->pl->head;
        u->pl->head = s->next;
        s->next = NULL;
        unit_remove(pf, s);
    }
    u->attached = 1;
    u->refs++;
    pf->refs++;
    pf->st.cache_hits++;
    pthread_cond_broadcast(&pf->cv);
    pthread_mutex_unlock(&pf->mu);

    r->magic = PF_READER_MAGIC;
    r->pf = pf;
    r->u = u;
    r->icb = pf->ic_icb;
    iobuf = av_malloc(PF_AVIO_BUF);
    *pb = iobuf ? avio_alloc_context(iobuf, PF_AVIO_BUF, 0, r, rd_read, NULL, NULL) : NULL;
    if (*pb == NULL) {
        av_free(iobuf);
        pthread_mutex_lock(&pf->mu);
        u->attached = 0;
        unit_unref(pf, u);
        pthread_mutex_unlock(&pf->mu);
        av_free(r->aes);
        av_free(r);
        pf_unref(pf);
        return AVERROR(ENOMEM);
    }
    (*pb)->seekable = 0;
    return 1;
}

int ms_hls_pf_reader_close(AVIOContext *pb)
{
    pf_reader *r;
    ms_hls_pf *pf;
    pf_unit *u;
    if (pb == NULL || pb->read_packet != rd_read) return 0;
    r = pb->opaque;
    if (r == NULL || r->magic != PF_READER_MAGIC) return 0;
    pf = r->pf;
    u = r->u;
    pthread_mutex_lock(&pf->mu);
    u->attached = 0;
    if (u->in_plan) {
        /* 消费完毕（或中途关闭）：移出计划并释放缓冲 */
        pf_unit **pp = &u->pl->head;
        while (*pp && *pp != u) pp = &(*pp)->next;
        if (*pp) *pp = u->next;
        u->next = NULL;
        unit_remove(pf, u);
    }
    unit_unref(pf, u);
    pthread_cond_broadcast(&pf->cv);
    pthread_mutex_unlock(&pf->mu);
    r->magic = 0;
    av_free(r->aes);
    av_free(r);
    av_freep(&pb->buffer);
    avio_context_free(&pb);
    pf_unref(pf);
    return 1;
}

void ms_hls_pf_get_stats(ms_hls_pf *pf, ms_ff_buffer_stats *out)
{
    int i;
    int64_t mn = -1;
    if (out == NULL) return;
    memset(out, 0, sizeof(*out));
    if (pf == NULL) return;
    pthread_mutex_lock(&pf->mu);
    *out = pf->st;
    for (i = 0; i < pf->npls; i++) {
        int64_t a = pls_ahead_us(pf->pls[i]);
        if (mn < 0 || a < mn) mn = a;
    }
    out->buffered_ms = mn < 0 ? 0 : mn / 1000;
    out->buffered_bytes = pf->bytes;
    pthread_mutex_unlock(&pf->mu);
}
