/*
 * RTMP 拉流（纯自研，rtmp:// 与 rtmps://，后者经 netcore 系统 TLS，证书校验不可关）：
 * 简单握手 → connect → createStream → play，拆块重组消息，音视频/脚本消息与聚合消息按 FLV 标签写出，
 * 交给核心 FLV 解封装（H.264/HEVC(12 或 Enhanced hvc1)/AAC）。处理 SetChunkSize / WindowAckSize /
 * SetPeerBandwidth / Ping，按窗口回 Acknowledgement；onStatus 失败（流不存在等）诚实报错。
 * 有界：块大小 ≤ 16 MiB，消息 ≤ 16 MiB，块流 ≤ 64 路。
 * by: Long <mycjzb@gmail.com>
 */
#define _GNU_SOURCE
#include "ms_netsrc.h"
#include "ms_flvw.h"
#include "../netcore/ms_netcore.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#define RT_MAX_CS   64
#define RT_MAX_MSG  (16u * 1024u * 1024u)

typedef struct rt_cs {
    int used;
    uint32_t csid;
    uint32_t ts, delta, len, sid;
    uint8_t type;
    int ext;
    uint8_t *buf;
    uint32_t got;
    uint32_t cap;
} rt_cs;

typedef struct rt {
    ms_nc_conn *c;
    ms_ns_out *o;
    uint32_t in_chunk, out_chunk;
    uint64_t rx, last_ack;
    uint32_t win;
    rt_cs cs[RT_MAX_CS];
    uint8_t rbuf[16384];
    int roff, rlen;
    ms_flvw w;
    uint32_t stream_id;
    int got_connect, got_stream, playing;
    char err[128];
} rt;

static int rt_read(rt *r, uint8_t *d, size_t n)
{
    while (n > 0) {
        if (r->roff == r->rlen) {
            int k = ms_nc_conn_read(r->c, r->rbuf, (int)sizeof(r->rbuf));
            if (k <= 0) return k == 0 ? MS_NC_E_IO : k;
            r->roff = 0;
            r->rlen = k;
            r->rx += (uint64_t)k;
        }
        {
            size_t c = (size_t)(r->rlen - r->roff);
            if (c > n) c = n;
            memcpy(d, r->rbuf + r->roff, c);
            r->roff += (int)c;
            d += c;
            n -= c;
        }
    }
    return 0;
}

static void be32w(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static uint32_t be32r(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static uint32_t be24r(const uint8_t *p) { return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2]; }

/* 发送一条消息（fmt0 头 + fmt3 续块），csid<64 */
static int rt_send(rt *r, int csid, uint8_t type, uint32_t sid, const uint8_t *d, uint32_t n)
{
    uint8_t h[12];
    uint32_t off = 0;
    int k;
    h[0] = (uint8_t)(csid & 0x3F);
    h[1] = h[2] = h[3] = 0;
    h[4] = (uint8_t)(n >> 16); h[5] = (uint8_t)(n >> 8); h[6] = (uint8_t)n;
    h[7] = type;
    h[8] = (uint8_t)sid; h[9] = (uint8_t)(sid >> 8); h[10] = (uint8_t)(sid >> 16); h[11] = (uint8_t)(sid >> 24);
    if ((k = ms_nc_conn_write_all(r->c, h, 12)) < 0) return k;
    while (off < n || n == 0) {
        uint32_t c = n - off > r->out_chunk ? r->out_chunk : n - off;
        if (off > 0) {
            uint8_t b = (uint8_t)(0xC0 | (csid & 0x3F));
            if ((k = ms_nc_conn_write_all(r->c, &b, 1)) < 0) return k;
        }
        if (c && (k = ms_nc_conn_write_all(r->c, d + off, (int)c)) < 0) return k;
        off += c;
        if (n == 0) break;
    }
    return 0;
}

/* ---- AMF0 ---- */
typedef struct amfb { uint8_t b[1024]; size_t n; int bad; } amfb;
static void a_put(amfb *a, const void *d, size_t n) { if (a->n + n > sizeof(a->b)) { a->bad = 1; return; } memcpy(a->b + a->n, d, n); a->n += n; }
static void a_num(amfb *a, double v)
{
    uint8_t t[9];
    uint64_t u;
    int i;
    memcpy(&u, &v, 8);
    t[0] = 0;
    for (i = 0; i < 8; i++) t[1 + i] = (uint8_t)(u >> (56 - 8 * i));
    a_put(a, t, 9);
}
static void a_str_raw(amfb *a, const char *s) { uint8_t l[2]; size_t n = strlen(s); l[0] = (uint8_t)(n >> 8); l[1] = (uint8_t)n; a_put(a, l, 2); a_put(a, s, n); }
static void a_str(amfb *a, const char *s) { uint8_t t = 2; a_put(a, &t, 1); a_str_raw(a, s); }
static void a_bool(amfb *a, int v) { uint8_t t[2] = { 1, (uint8_t)(v ? 1 : 0) }; a_put(a, t, 2); }
static void a_null(amfb *a) { uint8_t t = 5; a_put(a, &t, 1); }
static void a_obj_begin(amfb *a) { uint8_t t = 3; a_put(a, &t, 1); }
static void a_obj_end(amfb *a) { uint8_t t[3] = { 0, 0, 9 }; a_put(a, t, 3); }
static void a_kv_str(amfb *a, const char *k, const char *v) { a_str_raw(a, k); a_str(a, v); }
static void a_kv_num(amfb *a, const char *k, double v) { a_str_raw(a, k); a_num(a, v); }
static void a_kv_bool(amfb *a, const char *k, int v) { a_str_raw(a, k); a_bool(a, v); }

static double rd_num(const uint8_t *p)
{
    uint64_t u = 0;
    double v;
    int i;
    for (i = 0; i < 8; i++) u = (u << 8) | p[i];
    memcpy(&v, &u, 8);
    return v;
}

/* 跳过一个 AMF0 值，返回新偏移；-1 出错 */
static long a_skip(const uint8_t *d, size_t n, size_t off, int depth)
{
    uint8_t t;
    if (off >= n || depth > 16) return -1;
    t = d[off++];
    switch (t) {
    case 0: return off + 8 <= n ? (long)(off + 8) : -1;
    case 1: return off + 1 <= n ? (long)(off + 1) : -1;
    case 2: if (off + 2 > n) return -1; off += 2 + (size_t)((d[off] << 8) | d[off + 1]); return off <= n ? (long)off : -1;
    case 12: if (off + 4 > n) return -1; off += 4 + be32r(d + off); return off <= n ? (long)off : -1;
    case 5: case 6: return (long)off;
    case 11: return off + 10 <= n ? (long)(off + 10) : -1;
    case 8: off += 4; /* fallthrough */
    case 3:
        for (;;) {
            size_t kl;
            long r;
            if (off + 3 > n) return -1;
            kl = (size_t)((d[off] << 8) | d[off + 1]);
            if (kl == 0 && d[off + 2] == 9) return (long)(off + 3);
            off += 2 + kl;
            r = a_skip(d, n, off, depth + 1);
            if (r < 0) return -1;
            off = (size_t)r;
        }
    case 10: {
        uint32_t cnt, i;
        if (off + 4 > n) return -1;
        cnt = be32r(d + off);
        off += 4;
        for (i = 0; i < cnt; i++) {
            long r = a_skip(d, n, off, depth + 1);
            if (r < 0) return -1;
            off = (size_t)r;
        }
        return (long)off;
    }
    default: return -1;
    }
}

static int a_get_str(const uint8_t *d, size_t n, size_t off, char *out, size_t cap)
{
    size_t l;
    if (off + 3 > n || d[off] != 2) return -1;
    l = (size_t)((d[off + 1] << 8) | d[off + 2]);
    if (off + 3 + l > n) return -1;
    if (l >= cap) l = cap - 1;
    memcpy(out, d + off + 3, l);
    out[l] = 0;
    return 0;
}

/* 在 AMF 对象里找 "code" 字符串（onStatus 用） */
static void find_code(const uint8_t *d, size_t n, char *out, size_t cap)
{
    size_t i;
    out[0] = 0;
    for (i = 0; i + 8 < n; i++) {
        if (d[i] == 0 && d[i + 1] == 4 && !memcmp(d + i + 2, "code", 4) && d[i + 6] == 2) {
            a_get_str(d, n, i + 6, out, cap);
            return;
        }
    }
}

static int send_cmd_connect(rt *r, const char *app, const char *tcurl)
{
    amfb a;
    memset(&a, 0, sizeof(a));
    a_str(&a, "connect");
    a_num(&a, 1);
    a_obj_begin(&a);
    a_kv_str(&a, "app", app);
    a_kv_str(&a, "flashVer", "LNX 9,0,124,2");
    a_kv_str(&a, "tcUrl", tcurl);
    a_kv_bool(&a, "fpad", 0);
    a_kv_num(&a, "capabilities", 15);
    a_kv_num(&a, "audioCodecs", 4071);
    a_kv_num(&a, "videoCodecs", 252);
    a_kv_num(&a, "videoFunction", 1);
    a_obj_end(&a);
    if (a.bad) return MS_NC_E_ARG;
    return rt_send(r, 3, 20, 0, a.b, (uint32_t)a.n);
}

static int send_ctrl(rt *r, uint8_t type, const uint8_t *d, uint32_t n) { return rt_send(r, 2, type, 0, d, n); }

static int handle_msg(rt *r, rt_cs *c, const char *stream)
{
    const uint8_t *d = c->buf;
    uint32_t n = c->len;
    switch (c->type) {
    case 1: /* SetChunkSize */
        if (n >= 4) {
            uint32_t v = be32r(d) & 0x7FFFFFFF;
            if (v < 1 || v > RT_MAX_MSG) return MS_NC_E_PROTO;
            r->in_chunk = v;
        }
        return 0;
    case 5: /* WindowAckSize */
        if (n >= 4) r->win = be32r(d);
        return 0;
    case 6: { /* SetPeerBandwidth → 回 WindowAckSize */
        uint8_t b[4];
        be32w(b, n >= 4 ? be32r(d) : 2500000);
        return send_ctrl(r, 5, b, 4);
    }
    case 4: /* UserControl：PingRequest(6) → PingResponse(7) */
        if (n >= 6 && d[0] == 0 && d[1] == 6) {
            uint8_t b[6] = { 0, 7, d[2], d[3], d[4], d[5] };
            return send_ctrl(r, 4, b, 6);
        }
        return 0;
    case 20: { /* AMF0 命令 */
        char name[64];
        long off;
        double txn = 0;
        if (a_get_str(d, n, 0, name, sizeof(name)) != 0) return 0;
        off = a_skip(d, n, 0, 0);
        if (off > 0 && (size_t)off + 9 <= n && d[off] == 0) txn = rd_num(d + off + 1);
        if (!strcmp(name, "_result")) {
            if (txn == 1.0 && !r->got_connect) {
                amfb a;
                uint8_t b[4];
                r->got_connect = 1;
                be32w(b, 2500000);
                send_ctrl(r, 5, b, 4);
                memset(&a, 0, sizeof(a));
                a_str(&a, "createStream"); a_num(&a, 2); a_null(&a);
                return rt_send(r, 3, 20, 0, a.b, (uint32_t)a.n);
            }
            if (txn == 2.0 && !r->got_stream) {
                long o2 = a_skip(d, n, (size_t)off, 0); /* txn */
                long o3 = o2 > 0 ? a_skip(d, n, (size_t)o2, 0) : -1; /* null */
                amfb a;
                uint8_t ub[10];
                int k;
                if (o3 > 0 && (size_t)o3 + 9 <= n && d[o3] == 0) r->stream_id = (uint32_t)rd_num(d + o3 + 1);
                else r->stream_id = 1;
                r->got_stream = 1;
                memset(&a, 0, sizeof(a));
                a_str(&a, "play"); a_num(&a, 0); a_null(&a); a_str(&a, stream); a_num(&a, -2000);
                if ((k = rt_send(r, 8, 20, r->stream_id, a.b, (uint32_t)a.n)) != 0) return k;
                /* SetBufferLength(3) = 3000ms */
                ub[0] = 0; ub[1] = 3; be32w(ub + 2, r->stream_id); be32w(ub + 6, 3000);
                return send_ctrl(r, 4, ub, 10);
            }
            return 0;
        }
        if (!strcmp(name, "_error")) {
            snprintf(r->err, sizeof(r->err), "rtmp: command error (connect/createStream rejected)");
            return MS_NC_E_PROTO;
        }
        if (!strcmp(name, "onStatus")) {
            char code[96];
            find_code(d, n, code, sizeof(code));
            if (strstr(code, "Play.Start") || strstr(code, "Play.Reset")) r->playing = 1;
            if (strstr(code, "StreamNotFound") || strstr(code, ".Failed") || strstr(code, "Rejected")) {
                snprintf(r->err, sizeof(r->err), "rtmp: %s", code);
                return MS_NC_E_PROTO;
            }
            if (strstr(code, "Play.Stop") || strstr(code, "UnpublishNotify")) return 1; /* 正常结束 */
        }
        return 0;
    }
    case 8: case 9: case 18:
        if (c->type == 18 && n > 13 && d[0] == 2 && !memcmp(d + 3, "@setDataFrame", 13)) {
            long off = a_skip(d, n, 0, 0);
            if (off > 0) return ms_flvw_tag(&r->w, 18, c->ts, NULL, 0, d + off, n - (uint32_t)off) ? MS_NC_E_CANCELED : 0;
        }
        return ms_flvw_tag(&r->w, c->type, c->ts, NULL, 0, d, n) ? MS_NC_E_CANCELED : 0;
    case 22: { /* 聚合消息：内含 FLV 标签（无文件头），时间戳以消息时间为基 */
        uint32_t off = 0, base = 0;
        int first = 1;
        while (off + 11 <= n) {
            uint32_t ds = be24r(d + off + 1), ts = be24r(d + off + 4) | ((uint32_t)d[off + 7] << 24);
            if (off + 11 + ds + 4 > n) break;
            if (first) { base = ts; first = 0; }
            if (d[off] == 8 || d[off] == 9 || d[off] == 18) {
                if (ms_flvw_tag(&r->w, d[off], c->ts + (ts - base), NULL, 0, d + off + 11, ds)) return MS_NC_E_CANCELED;
            }
            off += 11 + ds + 4;
        }
        return 0;
    }
    default:
        return 0;
    }
}

static rt_cs *cs_get(rt *r, uint32_t csid)
{
    int i, fr = -1;
    for (i = 0; i < RT_MAX_CS; i++) {
        if (r->cs[i].used && r->cs[i].csid == csid) return &r->cs[i];
        if (!r->cs[i].used && fr < 0) fr = i;
    }
    if (fr < 0) return NULL;
    memset(&r->cs[fr], 0, sizeof(r->cs[fr]));
    r->cs[fr].used = 1;
    r->cs[fr].csid = csid;
    return &r->cs[fr];
}

/* 读一个块；完整消息交 handle_msg。返回 0 继续，1 正常结束，<0 错误 */
static int read_chunk(rt *r, const char *stream)
{
    uint8_t b[16];
    uint32_t csid, fmt, chunk;
    rt_cs *c;
    int k;
    if ((k = rt_read(r, b, 1)) != 0) return k;
    fmt = b[0] >> 6;
    csid = b[0] & 0x3F;
    if (csid == 0) { if ((k = rt_read(r, b + 1, 1)) != 0) return k; csid = 64u + b[1]; }
    else if (csid == 1) { if ((k = rt_read(r, b + 1, 2)) != 0) return k; csid = 64u + b[1] + 256u * b[2]; }
    c = cs_get(r, csid);
    if (!c) return MS_NC_E_PROTO;
    if (fmt <= 2) {
        static const int hl[3] = { 11, 7, 3 };
        uint32_t tsf;
        if ((k = rt_read(r, b, (size_t)hl[fmt])) != 0) return k;
        tsf = be24r(b);
        c->ext = (tsf == 0xFFFFFF);
        if (fmt <= 1) {
            c->len = be24r(b + 3);
            c->type = b[6];
            if (c->len > RT_MAX_MSG) return MS_NC_E_PROTO;
        }
        if (fmt == 0) c->sid = b[7] | (b[8] << 8) | (b[9] << 16) | ((uint32_t)b[10] << 24);
        if (c->ext) { if ((k = rt_read(r, b, 4)) != 0) return k; tsf = be32r(b); }
        if (c->got != 0) return MS_NC_E_PROTO; /* 新头不应出现在消息中途 */
        if (fmt == 0) { c->ts = tsf; c->delta = 0; }
        else { c->delta = tsf; c->ts += tsf; }
    } else {
        if (c->ext) { if ((k = rt_read(r, b, 4)) != 0) return k; }
        if (c->got == 0) c->ts += c->delta; /* fmt3 开新消息沿用增量 */
    }
    if (c->cap < c->len) {
        uint8_t *nb = (uint8_t *)realloc(c->buf, c->len ? c->len : 1);
        if (!nb) return MS_NC_E_NOMEM;
        c->buf = nb;
        c->cap = c->len;
    }
    chunk = c->len - c->got;
    if (chunk > r->in_chunk) chunk = r->in_chunk;
    if (chunk && (k = rt_read(r, c->buf + c->got, chunk)) != 0) return k;
    c->got += chunk;
    if (r->win && r->rx - r->last_ack >= r->win / 2) {
        uint8_t a[4];
        be32w(a, (uint32_t)r->rx);
        r->last_ack = r->rx;
        if ((k = send_ctrl(r, 3, a, 4)) != 0) return k;
    }
    if (c->got >= c->len) {
        c->got = 0;
        return handle_msg(r, c, stream);
    }
    return 0;
}

int ms_proto_rtmp_run(const char *url, ms_ns_out *o)
{
    rt *r;
    char host[256], app[512], stream[1024], tcurl[1024];
    const char *p, *hs, *ps, *last;
    int port, tls = 0, ret, i;
    uint8_t *c1;
    if (!strncasecmp(url, "rtmps://", 8)) { tls = 1; p = url + 8; port = 443; }
    else if (!strncasecmp(url, "rtmp://", 7)) { p = url + 7; port = 1935; }
    else return MS_NC_E_UNSUPPORTED;
    hs = p;
    ps = strchr(p, '/');
    if (!ps) { snprintf(o->err, o->err_cap, "rtmp: missing app"); return MS_NC_E_ARG; }
    {
        const char *colon = memchr(hs, ':', (size_t)(ps - hs));
        size_t hl = (size_t)((colon ? colon : ps) - hs);
        if (hl == 0 || hl >= sizeof(host) || memchr(hs, '@', (size_t)(ps - hs))) { snprintf(o->err, o->err_cap, "rtmp: bad host"); return MS_NC_E_ARG; }
        memcpy(host, hs, hl);
        host[hl] = 0;
        if (colon) port = atoi(colon + 1);
        if (port <= 0 || port > 65535) return MS_NC_E_ARG;
    }
    ps++;
    {
        const char *q = strchr(ps, '?');
        const char *e = q ? q : ps + strlen(ps);
        last = NULL;
        for (p = ps; p < e; p++) if (*p == '/') last = p;
        if (!last) { snprintf(o->err, o->err_cap, "rtmp: missing stream name"); return MS_NC_E_ARG; }
        if ((size_t)(last - ps) >= sizeof(app) || strlen(last + 1) >= sizeof(stream)) return MS_NC_E_ARG;
        memcpy(app, ps, (size_t)(last - ps));
        app[last - ps] = 0;
        snprintf(stream, sizeof(stream), "%s", last + 1);
        snprintf(tcurl, sizeof(tcurl), "%s://%s:%d/%s", tls ? "rtmps" : "rtmp", host, port, app);
    }
    r = (rt *)calloc(1, sizeof(*r));
    c1 = (uint8_t *)malloc(1537 + 3073);
    if (!r || !c1) { free(r); free(c1); return MS_NC_E_NOMEM; }
    r->o = o;
    r->in_chunk = 128;
    r->out_chunk = 128;
    r->w.write = o->write;
    r->w.ctx = o->ctx;
    ret = ms_nc_conn_open(host, port, tls, o->connect_timeout_ms, o->read_timeout_ms, o->cancel, &r->c);
    if (ret != MS_NC_OK) { snprintf(o->err, o->err_cap, "rtmp: connect failed (%s)", ms_nc_strerror(ret)); goto out; }
    /* 简单握手：C0 C1 → S0 S1 S2 → C2(=S1) */
    c1[0] = 3;
    memset(c1 + 1, 0, 8);
    {
        uint32_t x = (uint32_t)time(NULL) ^ 0x9E3779B9u;
        for (i = 9; i < 1537; i++) { x ^= x << 13; x ^= x >> 17; x ^= x << 5; c1[i] = (uint8_t)x; }
    }
    if ((ret = ms_nc_conn_write_all(r->c, c1, 1537)) < 0) goto out;
    if ((ret = rt_read(r, c1 + 1537, 3073)) != 0) goto out;
    if (c1[1537] != 3) { snprintf(o->err, o->err_cap, "rtmp: bad handshake version"); ret = MS_NC_E_PROTO; goto out; }
    if ((ret = ms_nc_conn_write_all(r->c, c1 + 1538, 1536)) < 0) goto out;
    {
        uint8_t cs[4];
        be32w(cs, 4096);
        if ((ret = send_ctrl(r, 1, cs, 4)) != 0) goto out;
        r->out_chunk = 4096;
    }
    if ((ret = send_cmd_connect(r, app, tcurl)) != 0) goto out;
    if ((ret = ms_flvw_header(&r->w, 1, 1)) != 0) { ret = MS_NC_E_CANCELED; goto out; }
    for (;;) {
        ret = read_chunk(r, stream);
        if (ret == 1) { ret = 0; break; }
        if (ret != 0) {
            if (r->err[0]) snprintf(o->err, o->err_cap, "%s", r->err);
            else if (!o->err[0]) snprintf(o->err, o->err_cap, "rtmp: io (%s)", ms_nc_strerror(ret));
            break;
        }
    }
out:
    if (r->c) ms_nc_conn_close(r->c);
    for (i = 0; i < RT_MAX_CS; i++) free(r->cs[i].buf);
    free(r);
    free(c1);
    return ret;
}
