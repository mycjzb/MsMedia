/*
 * 纯自研网络输入实现，见 ms_netsrc.h。
 * 线程模型：每个 netsrc 一个后台线程（HTTP 首包探测 → 渐进下载 / 或 HLS·DASH 预加载管理；
 * 或 UDP/RTP/RTMP/RTSP 协议生产者）；播放线程只调 drive（非阻塞，把已到字节推进会话）。
 * 共享状态一把互斥锁 + 一个条件变量；取消标志被所有阻塞点 ≤100ms 轮询。
 * 不记录完整 URL（可能含私有 token）。
 * by: Long <mycjzb@gmail.com>
 */
#define _GNU_SOURCE
#include "ms_netsrc.h"
#include "ms_mkvps.h"
#include "ms_tsguard.h"
#include "../netcore/ms_netcore.h"
#include "../core/ms_probe.h"
#include "../core/ms_internal.h"
#include "../hls/ms_hls_internal.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#define NS_HEAD_MAX      (MS_PROBE_MAX_JUNK + 512u * 1024u) /* 首包探测最多预读 */
#define NS_MANIFEST_MAX  (2u * 1024u * 1024u)
#define NS_CHUNK         (32u * 1024u)
#define NS_ENT_HARD_MAX  (64u * 1024u * 1024u)               /* 单个分片硬上限 */
#define NS_ABS_MAX_CACHE (128u * 1024u * 1024u)
#define NS_HINT_MAX      32

enum { Q_QUEUED = 0, Q_RUN, Q_DONE };

typedef struct pl_ent {
    struct pl_ent *next;
    char *uri;
    uint64_t off, len;
    int is_playlist, is_init, seeded;
    uint32_t dur_ms;
    uint8_t *buf;
    size_t n, cap, rd;
    int state;
    ms_status err;
    int demand;
    uint32_t id;
    int abandon;
    int probed;
    int stale;
    int preempt;
} pl_ent;

struct ms_netsrc {
    ms_netsrc_opts o;
    char *url;
    ms_session *s;
    pthread_t th;
    int th_started;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    volatile int cancel;
    int mode;
    int is_rtp;
    /* 管道（渐进/实时流） */
    uint8_t *pipe;
    size_t pcap, prd, pn;
    int peof;
    ms_status perr;
    uint8_t *hold;
    size_t hold_n, hold_off;
    int eoi_sent;
    /* 清单 */
    uint8_t *manifest;
    size_t manifest_n;
    ms_hls *hls;
    ms_dash *dash;
    ms_hls_fetch fetch;
    int manifest_ready;
    /* 预加载缓存 */
    pl_ent *ents;
    uint32_t next_id;
    size_t cache_bytes;
    int64_t last_hint_ms;
    ms_netsrc_stats st;
    char err[160];
    /* TS 坏包过滤：0 未定 1 启用 2 关闭 */
    int guard_mode;
    ms_tsguard *guard;
    FILE *fp;               /* file:// 本地源 */
};

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void cv_wait_ms(ms_netsrc *n, int ms)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_nsec += (long)ms * 1000000L;
    while (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
    pthread_cond_timedwait(&n->cv, &n->mu, &ts);
}

static int is_canceled(ms_netsrc *n) { return __atomic_load_n(&n->cancel, __ATOMIC_ACQUIRE) != 0; }

static void set_err(ms_netsrc *n, const char *msg, int code)
{
    pthread_mutex_lock(&n->mu);
    snprintf(n->err, sizeof(n->err), "%s (%d: %s)", msg, code, ms_nc_strerror(code));
    pthread_mutex_unlock(&n->mu);
}

void ms_netsrc_opts_default(ms_netsrc_opts *o)
{
    if (!o) return;
    memset(o, 0, sizeof(*o));
    o->struct_size = sizeof(*o);
    o->preload_ahead_ms = 10000;
    o->preload_max_bytes = 16u * 1024u * 1024u;
    o->connect_timeout_ms = 8000;
    o->read_timeout_ms = 15000;
    o->pipe_bytes = 2u * 1024u * 1024u;
}

int ms_netsrc_url_supported(const char *url)
{
    static const char *k[] = { "http://", "https://", "udp://", "rtp://", "rtmp://", "rtmps://", "rtsp://", "file://" };
    size_t i;
    if (!url) return 0;
    for (i = 0; i < sizeof(k) / sizeof(k[0]); i++)
        if (strncasecmp(url, k[i], strlen(k[i])) == 0) return 1;
    return 0;
}

const char *ms_netsrc_mode_name(int m)
{
    static const char *k[] = { "pending", "progressive", "hls", "dash", "udp", "rtp", "rtmp", "rtsp" };
    return (m >= 0 && m <= MS_NS_RTSP) ? k[m] : "?";
}

/* ---------------- 管道 ---------------- */
static int pipe_write(void *ctx, const uint8_t *d, size_t len)
{
    ms_netsrc *n = (ms_netsrc *)ctx;
    pthread_mutex_lock(&n->mu);
    while (len > 0) {
        size_t space, wpos, c;
        if (is_canceled(n)) { pthread_mutex_unlock(&n->mu); return -1; }
        space = n->pcap - n->pn;
        if (space == 0) {
            /* 实时流（UDP/RTP）不能阻塞发送端太久：满了丢最旧数据并记一次不连续由 TS 层自恢复 */
            if (n->mode == MS_NS_UDP || n->mode == MS_NS_RTP) {
                size_t drop = len < n->pcap ? len : n->pcap;
                drop = (drop + 187) / 188 * 188; /* 按 TS 包整丢，减少错位 */
                if (drop > n->pn) drop = n->pn;
                n->prd = (n->prd + drop) % n->pcap;
                n->pn -= drop;
                continue;
            }
            cv_wait_ms(n, 100);
            continue;
        }
        wpos = (n->prd + n->pn) % n->pcap;
        c = len;
        if (c > space) c = space;
        if (c > n->pcap - wpos) c = n->pcap - wpos;
        memcpy(n->pipe + wpos, d, c);
        n->pn += c;
        d += c;
        len -= c;
        pthread_cond_broadcast(&n->cv);
    }
    pthread_mutex_unlock(&n->mu);
    return 0;
}

/* 生产者写入口：TS 流先过坏包过滤（首字节 0x47 判定，或渐进探测结果已设定） */
static int src_write(void *ctx, const uint8_t *d, size_t len)
{
    ms_netsrc *n = (ms_netsrc *)ctx;
    if (n->guard_mode == 0) {
        n->guard_mode = (len > 0 && d[0] == 0x47) ? 1 : 2;
        if (n->guard_mode == 1) {
            n->guard = ms_tsguard_create(pipe_write, n);
            if (!n->guard) n->guard_mode = 2;
        }
    }
    if (n->guard) {
        int r = ms_tsguard_feed(n->guard, d, len);
        pthread_mutex_lock(&n->mu);
        n->st.bad_packets = ms_tsguard_dropped(n->guard);
        pthread_mutex_unlock(&n->mu);
        return r;
    }
    return pipe_write(ctx, d, len);
}

static void pipe_finish(ms_netsrc *n, ms_status err)
{
    pthread_mutex_lock(&n->mu);
    n->peof = 1;
    n->perr = err;
    pthread_cond_broadcast(&n->cv);
    pthread_mutex_unlock(&n->mu);
}

/* ---------------- 预加载缓存 ---------------- */
static void ent_free(ms_netsrc *n, pl_ent *e)
{
    pl_ent **pp;
    for (pp = &n->ents; *pp; pp = &(*pp)->next) {
        if (*pp == e) { *pp = e->next; break; }
    }
    n->cache_bytes -= e->n;
    free(e->uri);
    free(e->buf);
    free(e);
}

static int key_eq(const pl_ent *e, const char *uri, uint64_t off, uint64_t len, int is_playlist)
{
    return e->off == off && e->len == len && e->is_playlist == is_playlist && strcmp(e->uri, uri) == 0;
}

static pl_ent *ent_new(ms_netsrc *n, const char *uri, uint64_t off, uint64_t len)
{
    pl_ent *e = (pl_ent *)calloc(1, sizeof(*e)), **pp;
    if (!e) return NULL;
    e->uri = strdup(uri);
    if (!e->uri) { free(e); return NULL; }
    e->off = off;
    e->len = len;
    for (pp = &n->ents; *pp; pp = &(*pp)->next) {}
    *pp = e; /* 追加到尾部：保持前瞻顺序 */
    return e;
}

static pl_ent *ent_by_id(ms_netsrc *n, uint32_t id)
{
    pl_ent *e;
    for (e = n->ents; e; e = e->next)
        if (e->id == id && id != 0) return e;
    return NULL;
}

static ms_status nc_to_ms(int r)
{
    switch (r) {
    case MS_NC_E_CANCELED: return MS_ERR_CANCELED;
    case MS_NC_E_NOMEM: return MS_ERR_NOMEM;
    case MS_NC_E_UNSUPPORTED: return MS_ERR_UNSUPPORTED;
    case MS_NC_E_TIMEOUT: return MS_ERR_TIMEOUT;
    default: return MS_ERR_UNKNOWN;
    }
}

static int open_http(ms_netsrc *n, const char *uri, uint64_t off, uint64_t len, ms_nc_http **out)
{
    ms_nc_req r;
    char hdr[96];
    int st;
    memset(&r, 0, sizeof(r));
    r.url = uri;
    r.max_redirects = -1;
    r.connect_timeout_ms = n->o.connect_timeout_ms;
    r.read_timeout_ms = n->o.read_timeout_ms;
    r.cancel = &n->cancel;
    r.keep_alive = 1;
    if (off || len) {
        if (len) snprintf(hdr, sizeof(hdr), "Range: bytes=%llu-%llu\n", (unsigned long long)off, (unsigned long long)(off + len - 1));
        else snprintf(hdr, sizeof(hdr), "Range: bytes=%llu-\n", (unsigned long long)off);
        r.headers = hdr;
    }
    st = ms_nc_http_open(&r, out);
    if (st != MS_NC_OK) return st;
    {
        int code = ms_nc_http_status(*out);
        if (code < 200 || code >= 300 || ((off || len) && code != 206 && off != 0)) {
            ms_nc_http_close(*out);
            *out = NULL;
            return MS_NC_E_PROTO;
        }
    }
    return MS_NC_OK;
}

/* 下载一个缓存项；调用时已解锁。返回后按状态处理。 */
static void download_ent(ms_netsrc *n, pl_ent *e, const char *uri, uint64_t off, uint64_t len)
{
    ms_nc_http *h = NULL;
    uint8_t *tmp = (uint8_t *)malloc(NS_CHUNK);
    int r = tmp ? open_http(n, uri, off, len, &h) : MS_NC_E_NOMEM;
    ms_status fin = MS_OK;
    if (r != MS_NC_OK) fin = nc_to_ms(r);
    while (fin == MS_OK) {
        int k = ms_nc_http_read(h, tmp, (int)NS_CHUNK);
        int stop = 0;
        if (k == 0) break;
        if (k < 0) { fin = nc_to_ms(k); break; }
        pthread_mutex_lock(&n->mu);
        if (e->abandon || is_canceled(n)) stop = 1;
        else if (!e->demand) {
            pl_ent *x;
            int other_demand = 0;
            for (x = n->ents; x; x = x->next)
                if (x->state == Q_QUEUED && x->demand) { other_demand = 1; break; }
            /* 有人按需等待别的分片：让路；超出内存上限：放弃本次前瞻，稍后重来 */
            if (other_demand || (n->cache_bytes + (size_t)k > n->o.preload_max_bytes && n->cache_bytes > e->n)) {
                e->preempt = 1;
                stop = 1;
            }
        }
        if (!stop) {
            if (e->n + (size_t)k > NS_ENT_HARD_MAX) {
                stop = 1;
                fin = MS_ERR_OVERFLOW;
            } else {
                if (e->n + (size_t)k > e->cap) {
                    size_t nc = e->cap ? e->cap : 64u * 1024u;
                    uint8_t *nb;
                    while (nc < e->n + (size_t)k) nc *= 2;
                    nb = (uint8_t *)realloc(e->buf, nc);
                    if (!nb) { fin = MS_ERR_NOMEM; stop = 1; }
                    else { e->buf = nb; e->cap = nc; }
                }
                if (!stop) {
                    memcpy(e->buf + e->n, tmp, (size_t)k);
                    e->n += (size_t)k;
                    n->cache_bytes += (size_t)k;
                    if (n->cache_bytes > n->st.cache_peak) n->st.cache_peak = n->cache_bytes;
                    pthread_cond_broadcast(&n->cv);
                }
            }
        }
        pthread_mutex_unlock(&n->mu);
        if (stop) {
            if (fin == MS_OK) fin = MS_ERR_CANCELED;
            break;
        }
    }
    if (h) ms_nc_http_close(h); /* 完整读完的响应回 Keep-Alive 池 */
    free(tmp);
    pthread_mutex_lock(&n->mu);
    if (e->abandon) {
        ent_free(n, e);
    } else if (e->preempt) {
        /* 让路的前瞻：丢掉部分数据，回到队列 */
        n->cache_bytes -= e->n;
        free(e->buf);
        e->buf = NULL;
        e->n = e->cap = e->rd = 0;
        e->preempt = 0;
        e->state = Q_QUEUED;
    } else {
        e->state = Q_DONE;
        e->err = fin;
        if (fin == MS_OK && !e->demand) n->st.seg_prefetched++;
    }
    pthread_cond_broadcast(&n->cv);
    pthread_mutex_unlock(&n->mu);
}

static void preload_loop(ms_netsrc *n)
{
    pthread_mutex_lock(&n->mu);
    while (!is_canceled(n)) {
        pl_ent *e, *pick = NULL;
        char *uri;
        uint64_t off, len;
        for (e = n->ents; e; e = e->next)
            if (e->state == Q_QUEUED && e->demand) { pick = e; break; }
        if (!pick) {
            for (e = n->ents; e; e = e->next) {
                if (e->state == Q_QUEUED && !e->demand) {
                    if (n->cache_bytes < n->o.preload_max_bytes || n->cache_bytes == 0) pick = e;
                    break; /* 前瞻严格按顺序：第一个排队项放不下就等 */
                }
            }
        }
        if (!pick) { cv_wait_ms(n, 100); continue; }
        pick->state = Q_RUN;
        uri = strdup(pick->uri);
        off = pick->off;
        len = pick->len;
        pthread_mutex_unlock(&n->mu);
        if (!uri) {
            pthread_mutex_lock(&n->mu);
            pick->state = Q_DONE;
            pick->err = MS_ERR_NOMEM;
            continue;
        }
        download_ent(n, pick, uri, off, len);
        free(uri);
        pthread_mutex_lock(&n->mu);
    }
    pthread_mutex_unlock(&n->mu);
}

/* ---------------- fetch 回调（播放线程） ---------------- */
static ms_status f_request(void *ctx, const ms_hls_request *req, uint32_t *out_id)
{
    ms_netsrc *n = (ms_netsrc *)ctx;
    pl_ent *e, *hit = NULL;
    if (!req || !req->uri || !out_id) return MS_ERR_INVALID_ARG;
    pthread_mutex_lock(&n->mu);
    for (e = n->ents; e; e = e->next) {
        if (e->id == 0 && !e->abandon && key_eq(e, req->uri, req->byte_off, req->byte_len, req->is_playlist) &&
            !(e->state == Q_DONE && e->err != MS_OK) && (!req->is_playlist || e->seeded)) {
            hit = e;
            break;
        }
    }
    if (hit) {
        if (hit->state != Q_QUEUED && !req->is_playlist) n->st.seg_hit++;
        hit->seeded = 0;
    } else {
        hit = ent_new(n, req->uri, req->byte_off, req->byte_len);
        if (!hit) { pthread_mutex_unlock(&n->mu); return MS_ERR_NOMEM; }
        hit->is_playlist = req->is_playlist;
        hit->is_init = req->is_init;
    }
    hit->demand = 1;
    hit->stale = 0;
    if (!req->is_playlist) n->st.seg_demand++;
    hit->id = ++n->next_id ? n->next_id : ++n->next_id;
    *out_id = hit->id;
    pthread_cond_broadcast(&n->cv);
    pthread_mutex_unlock(&n->mu);
    return MS_OK;
}

static ms_status f_read(void *ctx, uint32_t id, uint8_t *buf, size_t len, size_t *out_n)
{
    ms_netsrc *n = (ms_netsrc *)ctx;
    pl_ent *e;
    size_t avail, c;
    if (out_n) *out_n = 0;
    if (!out_n || (!buf && len)) return MS_ERR_INVALID_ARG;
    if (is_canceled(n)) return MS_ERR_CANCELED;
    pthread_mutex_lock(&n->mu);
    e = ent_by_id(n, id);
    if (!e) { pthread_mutex_unlock(&n->mu); return MS_OK; } /* 已读完并回收 */
    if (!e->probed && !e->is_playlist) {
        ms_probe_result pr;
        int r = ms_probe_payload(e->buf ? e->buf + e->rd : (const uint8_t *)"", e->n - e->rd, e->state == Q_DONE, &pr);
        if (r == 0 && e->state != Q_DONE) { pthread_mutex_unlock(&n->mu); return MS_AGAIN; }
        if (r == 1 && pr.offset > 0) {
            e->rd += pr.offset;
            n->st.stripped_bytes += pr.offset;
            if (pr.disguise) n->st.probe_disguise = pr.disguise;
        }
        e->probed = 1;
    }
    avail = e->n - e->rd;
    if (avail == 0) {
        if (e->state == Q_DONE) {
            ms_status err = e->err;
            ent_free(n, e);
            pthread_mutex_unlock(&n->mu);
            return err == MS_OK ? MS_OK : err;
        }
        pthread_mutex_unlock(&n->mu);
        return MS_AGAIN;
    }
    c = avail < len ? avail : len;
    memcpy(buf, e->buf + e->rd, c);
    e->rd += c;
    /* 边下边解：已消费前缀及时归还，首片/大分片也不长期占内存 */
    if (e->rd >= 256u * 1024u) {
        memmove(e->buf, e->buf + e->rd, e->n - e->rd);
        e->n -= e->rd;
        n->cache_bytes -= e->rd;
        e->rd = 0;
    }
    pthread_cond_broadcast(&n->cv);
    pthread_mutex_unlock(&n->mu);
    *out_n = c;
    return MS_OK;
}

static void f_cancel(void *ctx, uint32_t id)
{
    ms_netsrc *n = (ms_netsrc *)ctx;
    pl_ent *e;
    pthread_mutex_lock(&n->mu);
    e = ent_by_id(n, id);
    if (e) {
        if (e->state == Q_RUN) { e->abandon = 1; e->id = 0; }
        else ent_free(n, e);
    }
    pthread_cond_broadcast(&n->cv);
    pthread_mutex_unlock(&n->mu);
}

/* ---------------- 前瞻提示 ---------------- */
typedef struct hint_item { char *uri; uint64_t off, len; int is_init; uint32_t dur; } hint_item;
typedef struct hint_set { hint_item it[NS_HINT_MAX]; size_t n; } hint_set;

static void hint_cb(void *ctx, const ms_hls_request *rq, uint32_t dur)
{
    hint_set *hs = (hint_set *)ctx;
    if (hs->n >= NS_HINT_MAX) return;
    hs->it[hs->n].uri = strdup(rq->uri);
    if (!hs->it[hs->n].uri) return;
    hs->it[hs->n].off = rq->byte_off;
    hs->it[hs->n].len = rq->byte_len;
    hs->it[hs->n].is_init = rq->is_init;
    hs->it[hs->n].dur = dur;
    hs->n++;
}

static void apply_hints(ms_netsrc *n)
{
    hint_set hs;
    size_t i;
    uint32_t cum = 0, have_ms = 0;
    pl_ent *e, *nx;
    hs.n = 0;
    (void)ms_hls_upcoming(n->hls, NS_HINT_MAX, hint_cb, &hs);
    pthread_mutex_lock(&n->mu);
    for (e = n->ents; e; e = e->next)
        if (!e->demand && !e->is_playlist) e->stale = 1;
    for (i = 0; i < hs.n; i++) {
        pl_ent *f = NULL;
        if (i > 1 && cum >= (uint32_t)n->o.preload_ahead_ms) break;
        for (e = n->ents; e; e = e->next)
            if (!e->abandon && key_eq(e, hs.it[i].uri, hs.it[i].off, hs.it[i].len, 0)) { f = e; break; }
        if (!f) {
            f = ent_new(n, hs.it[i].uri, hs.it[i].off, hs.it[i].len);
            if (!f) break;
            f->is_init = hs.it[i].is_init;
            f->dur_ms = hs.it[i].dur;
        }
        f->stale = 0;
        if (f->state == Q_DONE && f->err == MS_OK) have_ms += hs.it[i].dur;
        cum += hs.it[i].dur;
    }
    for (e = n->ents; e; e = nx) {
        nx = e->next;
        if (e->stale && !e->demand) {
            if (e->state == Q_RUN) e->abandon = 1;
            else ent_free(n, e);
        }
    }
    n->st.ahead_ms = have_ms;
    pthread_cond_broadcast(&n->cv);
    pthread_mutex_unlock(&n->mu);
    for (i = 0; i < hs.n; i++) free(hs.it[i].uri);
}

/* ---------------- 后台线程 ---------------- */
/* 主读取源：http(s) 或本地文件（file://，用于本地 MKV/PS/伪装头/坏包容错） */
static int main_open(ms_netsrc *n, ms_nc_http **h)
{
    if (!strncmp(n->url, "file://", 7)) {
        n->fp = fopen(n->url + 7, "rb");
        return n->fp ? MS_NC_OK : MS_NC_E_IO;
    }
    return open_http(n, n->url, 0, 0, h);
}

static int main_read(ms_netsrc *n, ms_nc_http *h, uint8_t *buf, int cap)
{
    if (n->fp) {
        size_t k;
        if (is_canceled(n)) return MS_NC_E_CANCELED;
        k = fread(buf, 1, (size_t)cap, n->fp);
        if (k == 0 && ferror(n->fp)) return MS_NC_E_IO;
        return (int)k;
    }
    return ms_nc_http_read(h, buf, cap);
}

static void main_close(ms_netsrc *n, ms_nc_http *h)
{
    if (n->fp) {
        fclose(n->fp);
        n->fp = NULL;
        return;
    }
    if (h) ms_nc_http_close(h);
}

static void *http_main(void *arg)
{
    ms_netsrc *n = (ms_netsrc *)arg;
    ms_nc_http *h = NULL;
    uint8_t *head = NULL;
    size_t hn = 0, hcap = 64u * 1024u;
    int r, eof = 0, pr_rc = 0;
    ms_tsconv *conv = NULL;
    ms_probe_result pr;
    memset(&pr, 0, sizeof(pr));
    r = main_open(n, &h);
    if (r != MS_NC_OK) {
        set_err(n, "http open failed", r);
        pthread_mutex_lock(&n->mu);
        n->mode = MS_NS_PROGRESSIVE;
        pthread_mutex_unlock(&n->mu);
        pipe_finish(n, nc_to_ms(r));
        return NULL;
    }
    head = (uint8_t *)malloc(hcap);
    if (!head) { main_close(n, h); pipe_finish(n, MS_ERR_NOMEM); return NULL; }
    /* 首包探测：不看 URL 后缀 */
    for (;;) {
        int k;
        if (hn == hcap) {
            uint8_t *nb;
            if (hcap >= NS_HEAD_MAX) break;
            nb = (uint8_t *)realloc(head, hcap * 2);
            if (!nb) break;
            head = nb;
            hcap *= 2;
        }
        k = main_read(n, h, head + hn, (int)(hcap - hn));
        if (k == 0) { eof = 1; break; }
        if (k < 0) {
            set_err(n, "http read failed", k);
            main_close(n, h);
            free(head);
            pthread_mutex_lock(&n->mu);
            n->mode = MS_NS_PROGRESSIVE;
            pthread_mutex_unlock(&n->mu);
            pipe_finish(n, nc_to_ms(k));
            return NULL;
        }
        hn += (size_t)k;
        pr_rc = ms_probe_payload(head, hn, 0, &pr);
        if (pr_rc != 0) break;
    }
    if (pr_rc == 0 || eof) pr_rc = ms_probe_payload(head, hn, 1, &pr);
    if (pr_rc != 1) { memset(&pr, 0, sizeof(pr)); pr.fmt = MS_PF_UNKNOWN; }
    pthread_mutex_lock(&n->mu);
    n->st.probe_fmt = pr.fmt;
    n->st.probe_disguise = pr.disguise;
    n->st.stripped_bytes = pr.offset;
    pthread_mutex_unlock(&n->mu);
    if ((pr.fmt == MS_PF_HLS || pr.fmt == MS_PF_DASH) && n->fp) {
        /* 本地清单文件：分片相对路径无法经 HTTP 取，诚实不支持 */
        main_close(n, h);
        free(head);
        pthread_mutex_lock(&n->mu);
        snprintf(n->err, sizeof(n->err), "local manifest file not supported");
        n->mode = MS_NS_PROGRESSIVE;
        pthread_mutex_unlock(&n->mu);
        pipe_finish(n, MS_ERR_UNSUPPORTED);
        return NULL;
    }
    if (pr.fmt == MS_PF_HLS || pr.fmt == MS_PF_DASH) {
        /* 读完清单（有界），之后本线程转为预加载管理线程 */
        while (!eof && hn < NS_MANIFEST_MAX) {
            int k;
            if (hn == hcap) {
                uint8_t *nb = (uint8_t *)realloc(head, hcap * 2);
                if (!nb) break;
                head = nb;
                hcap *= 2;
            }
            k = main_read(n, h, head + hn, (int)(hcap - hn));
            if (k == 0) { eof = 1; break; }
            if (k < 0) break;
            hn += (size_t)k;
        }
        main_close(n, h);
        pthread_mutex_lock(&n->mu);
        if (!eof) {
            snprintf(n->err, sizeof(n->err), "manifest too large or truncated");
            n->mode = MS_NS_PROGRESSIVE;
            n->peof = 1;
            n->perr = MS_ERR_MALFORMED;
            free(head);
            pthread_cond_broadcast(&n->cv);
            pthread_mutex_unlock(&n->mu);
            return NULL;
        }
        n->manifest = head;
        n->manifest_n = hn;
        n->mode = pr.fmt == MS_PF_HLS ? MS_NS_HLS : MS_NS_DASH;
        n->st.mode = n->mode;
        pthread_cond_broadcast(&n->cv);
        pthread_mutex_unlock(&n->mu);
        preload_loop(n);
        return NULL;
    }
    pthread_mutex_lock(&n->mu);
    n->mode = MS_NS_PROGRESSIVE;
    n->st.mode = n->mode;
    pthread_mutex_unlock(&n->mu);
    /* MKV / MPEG-PS：流式转封装成 TS，复用核心 TS 解封装 */
    n->guard_mode = (pr.fmt == MS_PF_TS || pr.fmt == MS_PF_TS192 || pr.fmt == MS_PF_TS204) ? 0 : 2;
    if (n->guard_mode == 0) {
        n->guard = ms_tsguard_create(pipe_write, n);
        n->guard_mode = n->guard ? 1 : 2;
    }
    if (pr.fmt == MS_PF_MKV || pr.fmt == MS_PF_PS) {
        conv = ms_tsconv_create(pr.fmt == MS_PF_MKV ? MS_TSCONV_MKV : MS_TSCONV_PS, pipe_write, n);
        if (!conv) { main_close(n, h); free(head); pipe_finish(n, MS_ERR_NOMEM); return NULL; }
    }
    r = conv ? ms_tsconv_feed(conv, head + pr.offset, hn - pr.offset) : src_write(n, head + pr.offset, hn - pr.offset);
    free(head);
    while (r == 0 && !eof) {
        uint8_t buf[16384];
        int k = main_read(n, h, buf, (int)sizeof(buf));
        if (k == 0) break;
        if (k < 0) {
            set_err(n, "http read failed", k);
            main_close(n, h);
            ms_tsconv_destroy(conv);
            pipe_finish(n, nc_to_ms(k));
            return NULL;
        }
        r = conv ? ms_tsconv_feed(conv, buf, (size_t)k) : src_write(n, buf, (size_t)k);
    }
    if (r == 0 && conv) r = ms_tsconv_end(conv);
    ms_tsconv_destroy(conv);
    main_close(n, h);
    pipe_finish(n, r == 0 ? MS_OK : MS_ERR_CANCELED);
    return NULL;
}

static void *proto_main(void *arg)
{
    ms_netsrc *n = (ms_netsrc *)arg;
    ms_ns_out o;
    int r;
    char eb[128];
    memset(&o, 0, sizeof(o));
    eb[0] = 0;
    o.ctx = n;
    o.write = src_write;
    o.cancel = &n->cancel;
    o.connect_timeout_ms = n->o.connect_timeout_ms;
    o.read_timeout_ms = n->o.read_timeout_ms;
    o.err = eb;
    o.err_cap = sizeof(eb);
    if (n->mode == MS_NS_UDP || n->mode == MS_NS_RTP) r = ms_proto_udp_run(n->url, n->mode == MS_NS_RTP, &o);
    else if (n->mode == MS_NS_RTMP) r = ms_proto_rtmp_run(n->url, &o);
    else r = ms_proto_rtsp_run(n->url, &o);
    if (r != 0 && eb[0]) {
        pthread_mutex_lock(&n->mu);
        snprintf(n->err, sizeof(n->err), "%s", eb);
        pthread_mutex_unlock(&n->mu);
    }
    pipe_finish(n, r == 0 ? MS_OK : (r == MS_NC_E_UNSUPPORTED ? MS_ERR_UNSUPPORTED : (is_canceled(n) ? MS_ERR_CANCELED : MS_ERR_UNKNOWN)));
    return NULL;
}

/* ---------------- 公共入口 ---------------- */
ms_status ms_netsrc_open(const char *url, const ms_netsrc_opts *o, ms_session *s, ms_netsrc **out)
{
    ms_netsrc *n;
    void *(*fn)(void *) = http_main;
    if (!url || !s || !out) return MS_ERR_INVALID_ARG;
    *out = NULL;
    if (!ms_netsrc_url_supported(url)) return MS_ERR_UNSUPPORTED;
    n = (ms_netsrc *)calloc(1, sizeof(*n));
    if (!n) return MS_ERR_NOMEM;
    ms_netsrc_opts_default(&n->o);
    if (o) {
        if (o->preload_ahead_ms > 0) n->o.preload_ahead_ms = o->preload_ahead_ms;
        if (o->preload_max_bytes > 0) n->o.preload_max_bytes = o->preload_max_bytes > NS_ABS_MAX_CACHE ? NS_ABS_MAX_CACHE : o->preload_max_bytes;
        if (o->connect_timeout_ms > 0) n->o.connect_timeout_ms = o->connect_timeout_ms;
        if (o->read_timeout_ms > 0) n->o.read_timeout_ms = o->read_timeout_ms;
        if (o->pipe_bytes >= 64u * 1024u) n->o.pipe_bytes = o->pipe_bytes;
    }
    n->url = strdup(url);
    n->pcap = n->o.pipe_bytes;
    n->pipe = (uint8_t *)malloc(n->pcap);
    n->hold = (uint8_t *)malloc(64u * 1024u);
    if (!n->url || !n->pipe || !n->hold) {
        free(n->url); free(n->pipe); free(n->hold); free(n);
        return MS_ERR_NOMEM;
    }
    n->s = s;
    ms_session_set_ts_tolerant(s, 1); /* 网络输入：TS 坏包/坏帧容错，不整路失败 */
    pthread_mutex_init(&n->mu, NULL);
    pthread_cond_init(&n->cv, NULL);
    n->fetch.struct_size = sizeof(n->fetch);
    n->fetch.ctx = n;
    n->fetch.request_fn = f_request;
    n->fetch.read_fn = f_read;
    n->fetch.cancel_fn = f_cancel;
    if (!strncasecmp(url, "udp://", 6)) { n->mode = MS_NS_UDP; fn = proto_main; }
    else if (!strncasecmp(url, "rtp://", 6)) { n->mode = MS_NS_RTP; fn = proto_main; }
    else if (!strncasecmp(url, "rtmp://", 7) || !strncasecmp(url, "rtmps://", 8)) { n->mode = MS_NS_RTMP; fn = proto_main; }
    else if (!strncasecmp(url, "rtsp://", 7)) { n->mode = MS_NS_RTSP; fn = proto_main; }
    n->st.mode = n->mode;
    if (pthread_create(&n->th, NULL, fn, n) != 0) {
        pthread_cond_destroy(&n->cv);
        pthread_mutex_destroy(&n->mu);
        free(n->url); free(n->pipe); free(n->hold); free(n);
        return MS_ERR_NOMEM;
    }
    n->th_started = 1;
    *out = n;
    return MS_OK;
}

/* 从主清单挑一个变体：取第一个 STREAM-INF（HLS 作者约定的起播档，起播最快）。 */
static int pick_variant(const uint8_t *m, size_t mn, char *out, size_t cap)
{
    const char *p = (const char *)m, *end = p + mn;
    int want = 0;
    while (p < end) {
        const char *ls = p, *le;
        while (p < end && *p != '\n' && *p != '\r') p++;
        le = p;
        while (p < end && (*p == '\n' || *p == '\r')) p++;
        if (le - ls >= 18 && !memcmp(ls, "#EXT-X-STREAM-INF:", 18)) { want = 1; continue; }
        if (want && le > ls && ls[0] != '#') {
            size_t l = (size_t)(le - ls);
            if (l >= cap) return -1;
            memcpy(out, ls, l);
            out[l] = 0;
            return 0;
        }
    }
    return -1;
}

/* 有界子串查找（不用 memmem，兼容 API18 bionic） */
static int ns_find(const uint8_t *h, size_t hn, const char *nd, size_t nn)
{
    size_t i;
    if (nn == 0 || hn < nn) return 0;
    for (i = 0; i + nn <= hn; i++)
        if (h[i] == (uint8_t)nd[0] && memcmp(h + i, nd, nn) == 0) return 1;
    return 0;
}

static int is_master(const uint8_t *m, size_t mn)
{
    return ns_find(m, mn, "#EXT-X-STREAM-INF:", 18) && !ns_find(m, mn, "#EXTINF:", 8);
}

static ms_status start_manifest(ms_netsrc *n)
{
    ms_status st;
    char *open_uri = n->url;
    char *var = NULL;
    pl_ent *seed;
    pthread_mutex_lock(&n->mu);
    if (n->mode == MS_NS_HLS && is_master(n->manifest, n->manifest_n)) {
        char *ref = (char *)malloc(4096);
        var = (char *)malloc(4096);
        if (!ref || !var || pick_variant(n->manifest, n->manifest_n, ref, 4096) != 0 ||
            ms_hls_resolve_uri(n->url, ref, var, 4096) != MS_OK) {
            free(ref); free(var);
            snprintf(n->err, sizeof(n->err), "hls master playlist: no usable variant");
            pthread_mutex_unlock(&n->mu);
            return MS_ERR_UNSUPPORTED;
        }
        free(ref);
        open_uri = var;
    } else {
        /* 首包已下好的清单作为缓存种子：ms_hls/ms_dash 打开时零额外请求 */
        seed = ent_new(n, n->url, 0, 0);
        if (!seed) { pthread_mutex_unlock(&n->mu); return MS_ERR_NOMEM; }
        seed->is_playlist = 1;
        seed->seeded = 1;
        seed->buf = n->manifest;
        seed->n = seed->cap = n->manifest_n;
        n->cache_bytes += n->manifest_n;
        seed->state = Q_DONE;
        seed->err = MS_OK;
        n->manifest = NULL;
    }
    pthread_mutex_unlock(&n->mu);
    if (n->mode == MS_NS_HLS) {
        st = ms_hls_create(NULL, NULL, &n->hls);
        if (st == MS_OK) st = ms_hls_bind(n->hls, n->s, &n->fetch);
        if (st == MS_OK) st = ms_hls_open(n->hls, open_uri);
    } else {
        st = ms_dash_create(NULL, NULL, &n->dash);
        if (st == MS_OK) st = ms_dash_bind(n->dash, n->s, &n->fetch);
        if (st == MS_OK) st = ms_dash_open(n->dash, open_uri);
    }
    free(var);
    n->manifest_ready = 1;
    return st;
}

static ms_status drive_pipe(ms_netsrc *n)
{
    size_t used = 0;
    ms_status st;
    if (n->hold_off >= n->hold_n) {
        size_t take, c1;
        pthread_mutex_lock(&n->mu);
        if (n->pn == 0) {
            int eof = n->peof;
            ms_status err = n->perr;
            pthread_mutex_unlock(&n->mu);
            if (!eof) return MS_AGAIN;
            if (err != MS_OK && err != MS_ERR_CANCELED) return err;
            if (err == MS_ERR_CANCELED) return MS_ERR_CANCELED;
            if (!n->eoi_sent) {
                st = ms_session_end_of_input(n->s);
                if (st == MS_LIMIT) return MS_LIMIT;
                n->eoi_sent = 1;
            }
            return MS_EOS;
        }
        take = n->pn < 64u * 1024u ? n->pn : 64u * 1024u;
        c1 = n->pcap - n->prd;
        if (c1 > take) c1 = take;
        memcpy(n->hold, n->pipe + n->prd, c1);
        if (take > c1) memcpy(n->hold + c1, n->pipe, take - c1);
        n->prd = (n->prd + take) % n->pcap;
        n->pn -= take;
        pthread_cond_broadcast(&n->cv);
        pthread_mutex_unlock(&n->mu);
        n->hold_n = take;
        n->hold_off = 0;
    }
    st = ms_session_push(n->s, n->hold + n->hold_off, n->hold_n - n->hold_off, &used);
    n->hold_off += used;
    n->st.bytes_in += used;
    if (st == MS_OK) return MS_OK;
    if (st == MS_LIMIT) return used ? MS_OK : MS_LIMIT;
    return st;
}

ms_status ms_netsrc_drive(ms_netsrc *n)
{
    int mode;
    ms_status st;
    if (!n) return MS_ERR_INVALID_ARG;
    if (is_canceled(n)) return MS_ERR_CANCELED;
    pthread_mutex_lock(&n->mu);
    mode = n->mode;
    pthread_mutex_unlock(&n->mu);
    switch (mode) {
    case MS_NS_PENDING:
        return MS_AGAIN;
    case MS_NS_HLS:
    case MS_NS_DASH:
        if (!n->manifest_ready) {
            st = start_manifest(n);
            if (st != MS_OK) return st;
        }
        if (n->hls) {
            int64_t t = now_ms();
            st = ms_hls_drive(n->hls);
            if (t - n->last_hint_ms >= 20 || st == MS_AGAIN) {
                n->last_hint_ms = t;
                apply_hints(n);
            }
        } else {
            st = ms_dash_drive(n->dash);
        }
        return st;
    default:
        return drive_pipe(n);
    }
}

void ms_netsrc_cancel(ms_netsrc *n)
{
    if (!n) return;
    __atomic_store_n(&n->cancel, 1, __ATOMIC_RELEASE);
    pthread_mutex_lock(&n->mu);
    pthread_cond_broadcast(&n->cv);
    pthread_mutex_unlock(&n->mu);
}

void ms_netsrc_close(ms_netsrc *n)
{
    if (!n) return;
    ms_netsrc_cancel(n);
    if (n->th_started) pthread_join(n->th, NULL);
    if (n->hls) ms_hls_destroy(n->hls);
    if (n->dash) ms_dash_destroy(n->dash);
    while (n->ents) ent_free(n, n->ents);
    free(n->manifest);
    free(n->pipe);
    ms_tsguard_destroy(n->guard);
    free(n->hold);
    free(n->url);
    pthread_cond_destroy(&n->cv);
    pthread_mutex_destroy(&n->mu);
    free(n);
}

void ms_netsrc_get_stats(ms_netsrc *n, ms_netsrc_stats *out)
{
    if (!n || !out) return;
    pthread_mutex_lock(&n->mu);
    *out = n->st;
    out->mode = n->mode;
    out->cache_bytes = n->cache_bytes;
    pthread_mutex_unlock(&n->mu);
    ms_nc_pool_stats(&out->http_opened, &out->http_reused);
}

const char *ms_netsrc_last_error(ms_netsrc *n)
{
    return (n && n->err[0]) ? n->err : "";
}
