/*
 * RTSP 拉流（纯自研，RTP over TCP 交织，适配 IP 摄像机/IPTV 网关常见形态）：
 * OPTIONS → DESCRIBE(SDP；401 时 Basic/Digest 认证，凭据取自 URL，日志不打印) → SETUP(每轨 interleaved) → PLAY，
 * 周期 GET_PARAMETER/OPTIONS 保活。负载：
 *   - MP2T（PT 33）：直接输出 TS；
 *   - H.264（RFC 6184 单 NAL/STAP-A/FU-A）、H.265（RFC 7798 单 NAL/AP/FU）、AAC（RFC 3640 mpeg4-generic hbr）：
 *     按 RTP 时间戳/marker 组 AU，转 FLV 标签（参数集取 SDP sprop-* 或带内 SPS/PPS/VPS，变化即重发配置）。
 * 其它负载诚实忽略（G.711 等在纯自研 FLV 路径不支持，见 PROGRESS）。有界：AU ≤ 8 MiB，轨 ≤ 4。
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

#define RS_MAX_TRK 4
#define RS_AU_MAX  (8u * 1024u * 1024u)

enum { PT_NONE = 0, PT_MP2T, PT_H264, PT_H265, PT_AAC };

typedef struct rs_trk {
    int kind;
    int pt;
    uint32_t clock;
    char control[512];
    int ch;                 /* interleaved RTP 通道 */
    /* AAC */
    uint8_t asc[64];
    size_t asc_n;
    int size_len, idx_len, idx_delta_len;
    /* 视频 */
    uint8_t ps[3][512];
    size_t ps_n[3];         /* H264: sps,pps；H265: vps,sps,pps */
    int ps_sent_sig;
    uint8_t *au;
    size_t au_n, au_cap;
    uint32_t au_ts;
    int au_key;
    int fu_on;
    int have_base;
    uint32_t base_ts;
} rs_trk;

typedef struct rs {
    ms_nc_conn *c;
    ms_ns_out *o;
    uint8_t rbuf[4 + 65535 + 4096]; /* 可容纳最大交织帧 */
    int roff, rlen;
    int cseq;
    char session[160];
    char user[128], pass[128];
    char realm[128], nonce[256];
    int auth;               /* 0 无 1 basic 2 digest */
    char url[1024];         /* 不含凭据的 URL */
    char base[1024];
    rs_trk t[RS_MAX_TRK];
    int nt;
    ms_flvw w;
    int ts_mode;
    int timeout_s;
    time_t last_ka;
    int64_t base_ms;
    int eof;                /* 对端正常关闭 */
    int got_media;
} rs;

/* ---- 小工具：base64 解码、MD5 ---- */
static int b64v(int c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62;
    if (c == '/' || c == '_') return 63;
    return -1;
}

static size_t b64dec(const char *s, size_t n, uint8_t *o, size_t cap)
{
    uint32_t acc = 0;
    int bits = 0;
    size_t k = 0, i;
    for (i = 0; i < n; i++) {
        int v = b64v((unsigned char)s[i]);
        if (v < 0) continue;
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (k >= cap) return 0;
            o[k++] = (uint8_t)(acc >> bits);
        }
    }
    return k;
}

static void b64enc(const uint8_t *d, size_t n, char *o, size_t cap)
{
    static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t i, k = 0;
    for (i = 0; i < n && k + 5 < cap; i += 3) {
        uint32_t v = (uint32_t)d[i] << 16 | (i + 1 < n ? (uint32_t)d[i + 1] << 8 : 0) | (i + 2 < n ? d[i + 2] : 0);
        o[k++] = t[(v >> 18) & 63];
        o[k++] = t[(v >> 12) & 63];
        o[k++] = i + 1 < n ? t[(v >> 6) & 63] : '=';
        o[k++] = i + 2 < n ? t[v & 63] : '=';
    }
    o[k] = 0;
}

typedef struct md5c { uint32_t s[4]; uint64_t len; uint8_t buf[64]; } md5c;
#define ROL(x, c) (((x) << (c)) | ((x) >> (32 - (c))))
static void md5_blk(md5c *m, const uint8_t *p)
{
    static const uint32_t K[64] = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
        0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
        0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
        0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
        0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
        0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
        0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
        0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391 };
    static const uint8_t R[64] = { 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
        5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
        4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
        6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21 };
    uint32_t w[16], a = m->s[0], b = m->s[1], c = m->s[2], d = m->s[3];
    int i;
    for (i = 0; i < 16; i++) w[i] = (uint32_t)p[i * 4] | ((uint32_t)p[i * 4 + 1] << 8) | ((uint32_t)p[i * 4 + 2] << 16) | ((uint32_t)p[i * 4 + 3] << 24);
    for (i = 0; i < 64; i++) {
        uint32_t f, g, tmp;
        if (i < 16) { f = (b & c) | (~b & d); g = (uint32_t)i; }
        else if (i < 32) { f = (d & b) | (~d & c); g = (uint32_t)(5 * i + 1) % 16; }
        else if (i < 48) { f = b ^ c ^ d; g = (uint32_t)(3 * i + 5) % 16; }
        else { f = c ^ (b | ~d); g = (uint32_t)(7 * i) % 16; }
        tmp = d; d = c; c = b;
        b = b + ROL(a + f + K[i] + w[g], R[i]);
        a = tmp;
    }
    m->s[0] += a; m->s[1] += b; m->s[2] += c; m->s[3] += d;
}
static void md5_hex(const char *in, char out[33])
{
    md5c m;
    size_t n = strlen(in), i, r;
    uint8_t pad[128];
    uint64_t bits = (uint64_t)n * 8;
    m.s[0] = 0x67452301; m.s[1] = 0xefcdab89; m.s[2] = 0x98badcfe; m.s[3] = 0x10325476;
    for (i = 0; i + 64 <= n; i += 64) md5_blk(&m, (const uint8_t *)in + i);
    r = n - i;
    memset(pad, 0, sizeof(pad));
    memcpy(pad, in + i, r);
    pad[r] = 0x80;
    {
        size_t tot = r + 1 + 8 <= 64 ? 64 : 128;
        int k;
        for (k = 0; k < 8; k++) pad[tot - 8 + k] = (uint8_t)(bits >> (8 * k));
        md5_blk(&m, pad);
        if (tot == 128) md5_blk(&m, pad + 64);
    }
    for (i = 0; i < 16; i++) snprintf(out + 2 * i, 3, "%02x", (unsigned)((m.s[i / 4] >> (8 * (i % 4))) & 0xFF));
}

/* ---- 连接读写 ---- */
static int rs_fill(rs *r)
{
    int k;
    if (r->roff > 0 && r->roff == r->rlen) r->roff = r->rlen = 0;
    if (r->rlen == (int)sizeof(r->rbuf)) {
        memmove(r->rbuf, r->rbuf + r->roff, (size_t)(r->rlen - r->roff));
        r->rlen -= r->roff;
        r->roff = 0;
        if (r->rlen == (int)sizeof(r->rbuf)) return MS_NC_E_PROTO;
    }
    k = ms_nc_conn_read(r->c, r->rbuf + r->rlen, (int)sizeof(r->rbuf) - r->rlen);
    if (k == 0) r->eof = 1;
    if (k <= 0) return k == 0 ? MS_NC_E_IO : k;
    r->rlen += k;
    return 0;
}

static int rs_need(rs *r, int n)
{
    while (r->rlen - r->roff < n) {
        int k;
        if (r->roff > 0) {
            memmove(r->rbuf, r->rbuf + r->roff, (size_t)(r->rlen - r->roff));
            r->rlen -= r->roff;
            r->roff = 0;
        }
        if (n > (int)sizeof(r->rbuf)) return MS_NC_E_PROTO;
        if ((k = rs_fill(r)) != 0) return k;
    }
    return 0;
}

/* 读一条 RTSP 响应（头+体），返回状态码；hdr/body 写入调用方缓冲 */
static int rs_read_resp(rs *r, char *hdr, size_t hcap, char *body, size_t bcap, size_t *blen)
{
    for (;;) {
        char *e;
        int k;
        size_t hl;
        long clen = 0;
        int code = 0;
        /* 跳过交织的 RTP 数据（$ 帧） */
        if ((k = rs_need(r, 1)) != 0) return k;
        if (r->rbuf[r->roff] == '$') {
            int len;
            if ((k = rs_need(r, 4)) != 0) return k;
            len = (r->rbuf[r->roff + 2] << 8) | r->rbuf[r->roff + 3];
            if ((k = rs_need(r, 4 + len)) != 0) return k;
            r->roff += 4 + len;
            continue;
        }
        for (;;) {
            e = NULL;
            if (r->rlen - r->roff >= 4) {
                int i;
                for (i = r->roff; i + 3 < r->rlen; i++)
                    if (!memcmp(r->rbuf + i, "\r\n\r\n", 4)) { e = (char *)r->rbuf + i; break; }
            }
            if (e) break;
            if ((k = rs_need(r, r->rlen - r->roff + 1)) != 0) return k;
        }
        hl = (size_t)(e - (char *)r->rbuf - r->roff) + 4;
        if (hl >= hcap) return MS_NC_E_PROTO;
        memcpy(hdr, r->rbuf + r->roff, hl);
        hdr[hl] = 0;
        r->roff += (int)hl;
        if (sscanf(hdr, "RTSP/%*d.%*d %d", &code) != 1) return MS_NC_E_PROTO;
        {
            const char *cl = strcasestr(hdr, "\nContent-Length:");
            if (cl) clen = strtol(cl + 16, NULL, 10);
        }
        if (clen < 0 || (size_t)clen >= bcap) return MS_NC_E_PROTO;
        if (clen > 0 && (k = rs_need(r, (int)clen)) != 0) return k;
        if (body) { memcpy(body, r->rbuf + r->roff, (size_t)clen); body[clen] = 0; }
        if (blen) *blen = (size_t)clen;
        r->roff += (int)clen;
        return code;
    }
}

static void hdr_val(const char *hdr, const char *name, char *out, size_t cap)
{
    char key[64];
    const char *p;
    size_t l;
    snprintf(key, sizeof(key), "\n%s:", name);
    out[0] = 0;
    p = strcasestr(hdr, key);
    if (!p) return;
    p += strlen(key);
    while (*p == ' ') p++;
    l = strcspn(p, "\r\n");
    if (l >= cap) l = cap - 1;
    memcpy(out, p, l);
    out[l] = 0;
}

static void param(const char *s, const char *k, char *out, size_t cap)
{
    char key[64];
    const char *p;
    size_t l;
    snprintf(key, sizeof(key), "%s=\"", k);
    out[0] = 0;
    p = strcasestr(s, key);
    if (!p) return;
    p += strlen(key);
    l = strcspn(p, "\"");
    if (l >= cap) l = cap - 1;
    memcpy(out, p, l);
    out[l] = 0;
}

static int rs_req(rs *r, const char *method, const char *uri, const char *extra, char *hdr, size_t hcap,
                  char *body, size_t bcap, size_t *blen)
{
    int attempt;
    for (attempt = 0; attempt < 2; attempt++) {
        char req[3072], auth[768];
        int n, k, code;
        auth[0] = 0;
        if (r->auth == 1) {
            char up[300], b64[420];
            snprintf(up, sizeof(up), "%s:%s", r->user, r->pass);
            b64enc((const uint8_t *)up, strlen(up), b64, sizeof(b64));
            snprintf(auth, sizeof(auth), "Authorization: Basic %s\r\n", b64);
        } else if (r->auth == 2) {
            char a1[512], a2[1200], h1[33], h2[33], rsp[33], all[400];
            snprintf(a1, sizeof(a1), "%s:%s:%s", r->user, r->realm, r->pass);
            snprintf(a2, sizeof(a2), "%s:%s", method, uri);
            md5_hex(a1, h1);
            md5_hex(a2, h2);
            snprintf(all, sizeof(all), "%s:%s:%s", h1, r->nonce, h2);
            md5_hex(all, rsp);
            snprintf(auth, sizeof(auth), "Authorization: Digest username=\"%s\", realm=\"%s\", nonce=\"%s\", uri=\"%s\", response=\"%s\"\r\n",
                     r->user, r->realm, r->nonce, uri, rsp);
        }
        n = snprintf(req, sizeof(req), "%s %s RTSP/1.0\r\nCSeq: %d\r\nUser-Agent: MsMedia/1.0\r\n%s%s%s%s%s\r\n",
                     method, uri, ++r->cseq, auth, r->session[0] ? "Session: " : "", r->session,
                     r->session[0] ? "\r\n" : "", extra ? extra : "");
        if (n <= 0 || n >= (int)sizeof(req)) return MS_NC_E_ARG;
        if ((k = ms_nc_conn_write_all(r->c, (const uint8_t *)req, n)) < 0) return k;
        code = rs_read_resp(r, hdr, hcap, body, bcap, blen);
        if (code == 401 && attempt == 0 && r->user[0]) {
            char wa[512];
            hdr_val(hdr, "WWW-Authenticate", wa, sizeof(wa));
            if (!strncasecmp(wa, "Digest", 6)) {
                param(wa, "realm", r->realm, sizeof(r->realm));
                param(wa, "nonce", r->nonce, sizeof(r->nonce));
                r->auth = 2;
            } else {
                r->auth = 1;
            }
            continue;
        }
        return code;
    }
    return 401;
}

/* ---- SDP ---- */
static void resolve_ctl(rs *r, const char *ctl, char *out, size_t cap)
{
    if (!ctl[0] || !strcmp(ctl, "*")) { snprintf(out, cap, "%s", r->base); return; }
    if (!strncasecmp(ctl, "rtsp://", 7)) { snprintf(out, cap, "%s", ctl); return; }
    {
        size_t bl = strlen(r->base);
        snprintf(out, cap, "%s%s%s", r->base, (bl && r->base[bl - 1] == '/') ? "" : "/", ctl);
    }
}

static void parse_sdp(rs *r, char *sdp)
{
    char *line = sdp, *next;
    rs_trk *cur = NULL;
    int pt_num = -1;
    for (; line && *line; line = next) {
        next = strpbrk(line, "\r\n");
        if (next) { while (*next == '\r' || *next == '\n') *next++ = 0; }
        if (!strncmp(line, "m=", 2)) {
            cur = NULL;
            if (r->nt < RS_MAX_TRK) {
                cur = &r->t[r->nt++];
                memset(cur, 0, sizeof(*cur));
                cur->kind = !strncmp(line + 2, "video", 5) ? 1 : !strncmp(line + 2, "audio", 5) ? 2 : 0;
                pt_num = -1;
                {
                    const char *s = strstr(line, "RTP/AVP");
                    if (s) { s = strchr(s, ' '); if (s) pt_num = atoi(s + 1); }
                }
                if (pt_num == 33) { cur->pt = PT_MP2T; cur->clock = 90000; }
            }
            continue;
        }
        if (!cur) continue;
        if (!strncmp(line, "a=control:", 10)) {
            snprintf(cur->control, sizeof(cur->control), "%s", line + 10);
        } else if (!strncmp(line, "a=rtpmap:", 9)) {
            const char *enc = strchr(line, ' ');
            if (enc) {
                enc++;
                if (!strncasecmp(enc, "H264/", 5)) cur->pt = PT_H264;
                else if (!strncasecmp(enc, "H265/", 5) || !strncasecmp(enc, "HEVC/", 5)) cur->pt = PT_H265;
                else if (!strncasecmp(enc, "MPEG4-GENERIC/", 14)) cur->pt = PT_AAC;
                else if (!strncasecmp(enc, "MP2T/", 5)) cur->pt = PT_MP2T;
                {
                    const char *sl = strchr(enc, '/');
                    if (sl) cur->clock = (uint32_t)strtoul(sl + 1, NULL, 10);
                }
            }
        } else if (!strncmp(line, "a=fmtp:", 7)) {
            char v[1024];
            const char *p;
            if (cur->pt == PT_H264 && (p = strstr(line, "sprop-parameter-sets="))) {
                const char *s = p + 21, *comma = strchr(s, ','), *end = s + strcspn(s, ";");
                if (comma && comma < end) {
                    cur->ps_n[0] = b64dec(s, (size_t)(comma - s), cur->ps[0], sizeof(cur->ps[0]));
                    cur->ps_n[1] = b64dec(comma + 1, (size_t)(end - comma - 1), cur->ps[1], sizeof(cur->ps[1]));
                }
            }
            if (cur->pt == PT_H265) {
                static const char *keys[3] = { "sprop-vps=", "sprop-sps=", "sprop-pps=" };
                int i;
                for (i = 0; i < 3; i++) {
                    if ((p = strstr(line, keys[i]))) {
                        const char *s = p + strlen(keys[i]);
                        size_t l = strcspn(s, ";,");
                        cur->ps_n[i] = b64dec(s, l, cur->ps[i], sizeof(cur->ps[i]));
                    }
                }
            }
            if (cur->pt == PT_AAC) {
                if ((p = strcasestr(line, "config="))) {
                    size_t i, l = strcspn(p + 7, "; ");
                    cur->asc_n = 0;
                    for (i = 0; i + 1 < l && cur->asc_n < sizeof(cur->asc); i += 2) {
                        char hx[3] = { p[7 + i], p[8 + i], 0 };
                        cur->asc[cur->asc_n++] = (uint8_t)strtoul(hx, NULL, 16);
                    }
                }
                cur->size_len = 13; cur->idx_len = 3; cur->idx_delta_len = 3;
                if ((p = strcasestr(line, "sizelength="))) cur->size_len = atoi(p + 11);
                if ((p = strcasestr(line, "indexlength="))) cur->idx_len = atoi(p + 12);
                if ((p = strcasestr(line, "indexdeltalength="))) cur->idx_delta_len = atoi(p + 17);
            }
            (void)v;
        }
    }
}

/* ---- 输出 ---- */
static uint32_t to_ms(rs *r, rs_trk *t, uint32_t ts)
{
    (void)r;
    if (!t->have_base) { t->have_base = 1; t->base_ts = ts; }
    return t->clock ? (uint32_t)((uint64_t)(uint32_t)(ts - t->base_ts) * 1000u / t->clock) : 0;
}

static int emit_video_cfg(rs *r, rs_trk *t)
{
    uint8_t rec[1700];
    size_t n;
    int hevc = t->pt == PT_H265, sig;
    if (hevc) n = ms_flvw_make_hvcc(t->ps[0], t->ps_n[0], t->ps[1], t->ps_n[1], t->ps[2], t->ps_n[2], rec, sizeof(rec));
    else n = ms_flvw_make_avcc(t->ps[0], t->ps_n[0], t->ps[1], t->ps_n[1], rec, sizeof(rec));
    if (!n) return 0;
    sig = (int)(n * 131u + rec[n / 2] * 7u + rec[n - 1]);
    if (sig == t->ps_sent_sig) return 0;
    t->ps_sent_sig = sig;
    return ms_flvw_video_config(&r->w, hevc, 0, rec, n);
}

static int au_append_nal(rs_trk *t, const uint8_t *d, size_t n)
{
    int hevc = t->pt == PT_H265;
    int ty = hevc ? ((d[0] >> 1) & 0x3F) : (d[0] & 0x1F);
    if (n == 0) return 0;
    /* 参数集：存起来（带内更新），不进 AU */
    if (!hevc && (ty == 7 || ty == 8)) {
        int i = ty == 7 ? 0 : 1;
        if (n <= sizeof(t->ps[i])) { memcpy(t->ps[i], d, n); t->ps_n[i] = n; }
        return 0;
    }
    if (hevc && ty >= 32 && ty <= 34) {
        int i = ty - 32;
        if (n <= sizeof(t->ps[i])) { memcpy(t->ps[i], d, n); t->ps_n[i] = n; }
        return 0;
    }
    if ((!hevc && ty == 5) || (hevc && ty >= 16 && ty <= 21)) t->au_key = 1;
    if (t->au_n + 4 + n > RS_AU_MAX) return -1;
    if (t->au_n + 4 + n > t->au_cap) {
        size_t nc = t->au_cap ? t->au_cap * 2 : 256u * 1024u;
        uint8_t *nb;
        while (nc < t->au_n + 4 + n) nc *= 2;
        nb = (uint8_t *)realloc(t->au, nc);
        if (!nb) return -1;
        t->au = nb;
        t->au_cap = nc;
    }
    t->au[t->au_n++] = (uint8_t)(n >> 24); t->au[t->au_n++] = (uint8_t)(n >> 16);
    t->au[t->au_n++] = (uint8_t)(n >> 8); t->au[t->au_n++] = (uint8_t)n;
    memcpy(t->au + t->au_n, d, n);
    t->au_n += n;
    return 0;
}

static int flush_au(rs *r, rs_trk *t)
{
    int k = 0;
    if (t->au_n) {
        if ((k = emit_video_cfg(r, t)) != 0) return k;
        if (t->ps_sent_sig) /* 没有参数集不送帧（解码器无法初始化） */
            k = ms_flvw_video_frame(&r->w, t->pt == PT_H265, t->au_key, to_ms(r, t, t->au_ts), 0, t->au, t->au_n);
    }
    t->au_n = 0;
    t->au_key = 0;
    return k;
}

static int on_video(rs *r, rs_trk *t, const uint8_t *p, size_t n, uint32_t ts, int marker)
{
    int hevc = t->pt == PT_H265, ty, k;
    size_t hl = hevc ? 2 : 1;
    if (n < hl + 1) return 0;
    if (t->au_n && ts != t->au_ts) { if ((k = flush_au(r, t)) != 0) return k; }
    t->au_ts = ts;
    ty = hevc ? ((p[0] >> 1) & 0x3F) : (p[0] & 0x1F);
    if ((!hevc && ty == 24) || (hevc && ty == 48)) { /* STAP-A / AP */
        size_t off = hl;
        while (off + 2 <= n) {
            size_t l = (size_t)((p[off] << 8) | p[off + 1]);
            off += 2;
            if (off + l > n) break;
            if (au_append_nal(t, p + off, l) != 0) return MS_NC_E_NOMEM;
            off += l;
        }
    } else if ((!hevc && ty == 28) || (hevc && ty == 49)) { /* FU-A / FU */
        uint8_t fh = p[hl];
        int s = fh & 0x80, e = fh & 0x40;
        if (s) {
            uint8_t nh[2];
            size_t nl = hevc ? 2 : 1;
            if (hevc) { nh[0] = (uint8_t)((p[0] & 0x81) | ((fh & 0x3F) << 1)); nh[1] = p[1]; }
            else nh[0] = (uint8_t)((p[0] & 0xE0) | (fh & 0x1F));
            /* 先放长度占位，片段累加后回填 */
            if (t->au_n + 4 + nl > t->au_cap) {
                size_t nc = t->au_cap ? t->au_cap * 2 : 256u * 1024u;
                uint8_t *nb = (uint8_t *)realloc(t->au, nc);
                if (!nb) return MS_NC_E_NOMEM;
                t->au = nb; t->au_cap = nc;
            }
            t->fu_on = (int)t->au_n + 1;
            t->au_n += 4;
            memcpy(t->au + t->au_n, nh, nl);
            t->au_n += nl;
            {
                int fty = hevc ? (fh & 0x3F) : (fh & 0x1F);
                if ((!hevc && fty == 5) || (hevc && fty >= 16 && fty <= 21)) t->au_key = 1;
            }
        }
        if (t->fu_on) {
            size_t pl = n - hl - 1;
            if (t->au_n + pl > RS_AU_MAX) return MS_NC_E_PROTO;
            if (t->au_n + pl > t->au_cap) {
                size_t nc = t->au_cap * 2;
                uint8_t *nb;
                while (nc < t->au_n + pl) nc *= 2;
                nb = (uint8_t *)realloc(t->au, nc);
                if (!nb) return MS_NC_E_NOMEM;
                t->au = nb; t->au_cap = nc;
            }
            memcpy(t->au + t->au_n, p + hl + 1, pl);
            t->au_n += pl;
            if (e) {
                size_t start = (size_t)(t->fu_on - 1), l = t->au_n - start - 4;
                t->au[start] = (uint8_t)(l >> 24); t->au[start + 1] = (uint8_t)(l >> 16);
                t->au[start + 2] = (uint8_t)(l >> 8); t->au[start + 3] = (uint8_t)l;
                t->fu_on = 0;
            }
        }
    } else {
        if (au_append_nal(t, p, n) != 0) return MS_NC_E_NOMEM;
    }
    if (marker && !t->fu_on) return flush_au(r, t);
    return 0;
}

static int on_aac(rs *r, rs_trk *t, const uint8_t *p, size_t n, uint32_t ts)
{
    uint32_t hbits, hbytes, off, i = 0, cnt;
    int k;
    if (!t->ps_sent_sig && t->asc_n) {
        if ((k = ms_flvw_aac_config(&r->w, 0, t->asc, t->asc_n)) != 0) return k;
        t->ps_sent_sig = 1;
    }
    if (!t->ps_sent_sig || n < 2) return 0;
    hbits = (uint32_t)((p[0] << 8) | p[1]);
    hbytes = (hbits + 7) / 8;
    if (2 + hbytes > n || t->size_len <= 0 || t->size_len + t->idx_len > 32) return 0;
    cnt = hbits / (uint32_t)(t->size_len + t->idx_len);
    off = 2 + hbytes;
    for (i = 0; i < cnt; i++) {
        uint32_t bitpos = i * (uint32_t)(t->size_len + t->idx_len), v = 0, b;
        for (b = 0; b < (uint32_t)t->size_len; b++) {
            uint32_t bp = bitpos + b;
            v = (v << 1) | ((p[2 + bp / 8] >> (7 - bp % 8)) & 1u);
        }
        if (off + v > n) break;
        /* 每帧 1024 采样：按采样率推算时间戳 */
        if ((k = ms_flvw_aac_frame(&r->w, to_ms(r, t, ts + i * 1024u), p + off, v)) != 0) return k;
        off += v;
    }
    return 0;
}

static int on_rtp(rs *r, int ch, const uint8_t *d, size_t n)
{
    int i, cc, off;
    uint32_t ts;
    rs_trk *t = NULL;
    for (i = 0; i < r->nt; i++) if (r->t[i].ch == ch && r->t[i].pt != PT_NONE) { t = &r->t[i]; break; }
    if (!t || n < 12 || (d[0] >> 6) != 2) return 0;
    cc = d[0] & 0x0F;
    off = 12 + 4 * cc;
    if (d[0] & 0x10) {
        if ((size_t)off + 4 > n) return 0;
        off += 4 + 4 * ((d[off + 2] << 8) | d[off + 3]);
    }
    if ((size_t)off > n) return 0;
    if (d[0] & 0x20) { if (d[n - 1] > n - (size_t)off) return 0; n -= d[n - 1]; }
    ts = ((uint32_t)d[4] << 24) | ((uint32_t)d[5] << 16) | ((uint32_t)d[6] << 8) | d[7];
    switch (t->pt) {
    case PT_MP2T: return r->o->write(r->o->ctx, d + off, n - (size_t)off) ? MS_NC_E_CANCELED : 0;
    case PT_H264: case PT_H265: return on_video(r, t, d + off, n - (size_t)off, ts, (d[1] & 0x80) != 0);
    case PT_AAC: return on_aac(r, t, d + off, n - (size_t)off, ts);
    default: return 0;
    }
}

int ms_proto_rtsp_run(const char *url, ms_ns_out *o)
{
    rs *r;
    char host[256], *hdr = NULL, *body = NULL;
    const char *p, *ps, *at;
    int port = 554, ret = 0, i, code, nch = 0, has_ts = 0, has_v = 0, has_a = 0;
    size_t blen = 0;
    r = (rs *)calloc(1, sizeof(*r));
    hdr = (char *)malloc(8192);
    body = (char *)malloc(65536);
    if (!r || !hdr || !body) { free(r); free(hdr); free(body); return MS_NC_E_NOMEM; }
    r->o = o;
    r->w.write = o->write;
    r->w.ctx = o->ctx;
    r->timeout_s = 60;
    p = url + 7;
    ps = p + strcspn(p, "/?");
    at = memchr(p, '@', (size_t)(ps - p));
    if (at) {
        const char *colon = memchr(p, ':', (size_t)(at - p));
        size_t ul = (size_t)((colon ? colon : at) - p);
        if (ul < sizeof(r->user)) { memcpy(r->user, p, ul); r->user[ul] = 0; }
        if (colon && (size_t)(at - colon - 1) < sizeof(r->pass)) { memcpy(r->pass, colon + 1, (size_t)(at - colon - 1)); r->pass[at - colon - 1] = 0; }
        p = at + 1;
    }
    {
        const char *colon = memchr(p, ':', (size_t)(ps - p));
        size_t hl = (size_t)((colon ? colon : ps) - p);
        if (hl == 0 || hl >= sizeof(host)) { ret = MS_NC_E_ARG; goto out; }
        memcpy(host, p, hl);
        host[hl] = 0;
        if (colon) port = atoi(colon + 1);
    }
    snprintf(r->url, sizeof(r->url), "rtsp://%.*s%s", (int)(ps - p), p, ps);
    snprintf(r->base, sizeof(r->base), "%s", r->url);
    ret = ms_nc_conn_open(host, port, 0, o->connect_timeout_ms, o->read_timeout_ms, o->cancel, &r->c);
    if (ret != MS_NC_OK) { snprintf(o->err, o->err_cap, "rtsp: connect failed (%s)", ms_nc_strerror(ret)); goto out; }
    code = rs_req(r, "OPTIONS", r->url, NULL, hdr, 8192, body, 65536, &blen);
    if (code < 0) { ret = code; goto out; }
    code = rs_req(r, "DESCRIBE", r->url, "Accept: application/sdp\r\n", hdr, 8192, body, 65536, &blen);
    if (code != 200) {
        snprintf(o->err, o->err_cap, "rtsp: DESCRIBE status %d", code);
        ret = code < 0 ? code : MS_NC_E_PROTO;
        goto out;
    }
    {
        char cb[1024];
        hdr_val(hdr, "Content-Base", cb, sizeof(cb));
        if (!cb[0]) hdr_val(hdr, "Content-Location", cb, sizeof(cb));
        if (cb[0]) snprintf(r->base, sizeof(r->base), "%s", cb);
    }
    parse_sdp(r, body);
    for (i = 0; i < r->nt; i++) if (r->t[i].pt == PT_MP2T) has_ts = 1;
    for (i = 0; i < r->nt; i++) {
        rs_trk *t = &r->t[i];
        char cu[1100], tr[128], sess[160];
        if (t->pt == PT_NONE || (has_ts && t->pt != PT_MP2T)) { t->pt = PT_NONE; continue; }
        if (t->pt == PT_AAC && (!t->asc_n || !t->clock)) { t->pt = PT_NONE; continue; }
        resolve_ctl(r, t->control, cu, sizeof(cu));
        t->ch = nch;
        snprintf(tr, sizeof(tr), "Transport: RTP/AVP/TCP;unicast;interleaved=%d-%d\r\n", nch, nch + 1);
        code = rs_req(r, "SETUP", cu, tr, hdr, 8192, body, 65536, &blen);
        if (code != 200) { snprintf(o->err, o->err_cap, "rtsp: SETUP status %d", code); ret = code < 0 ? code : MS_NC_E_PROTO; goto out; }
        hdr_val(hdr, "Session", sess, sizeof(sess));
        {
            char *semi = strchr(sess, ';');
            const char *to = strstr(sess, "timeout=");
            if (to) { int v = atoi(to + 8); if (v >= 5 && v < 3600) r->timeout_s = v; }
            if (semi) *semi = 0;
        }
        if (sess[0]) snprintf(r->session, sizeof(r->session), "%s", sess);
        {
            char trr[256];
            const char *il;
            hdr_val(hdr, "Transport", trr, sizeof(trr));
            il = strstr(trr, "interleaved=");
            if (il) t->ch = atoi(il + 12);
        }
        nch += 2;
        if (t->kind == 1 || t->pt == PT_H264 || t->pt == PT_H265) has_v = 1;
        if (t->pt == PT_AAC) has_a = 1;
    }
    if (nch == 0) { snprintf(o->err, o->err_cap, "rtsp: no supported track (need MP2T/H264/H265/AAC)"); ret = MS_NC_E_UNSUPPORTED; goto out; }
    code = rs_req(r, "PLAY", r->base, "Range: npt=0.000-\r\n", hdr, 8192, body, 65536, &blen);
    if (code != 200) { snprintf(o->err, o->err_cap, "rtsp: PLAY status %d", code); ret = code < 0 ? code : MS_NC_E_PROTO; goto out; }
    if (!has_ts && ms_flvw_header(&r->w, has_a, has_v) != 0) { ret = MS_NC_E_CANCELED; goto out; }
    r->last_ka = time(NULL);
    for (;;) {
        int k, len, ch;
        if (time(NULL) - r->last_ka >= r->timeout_s / 2) {
            char req[1400];
            int n = snprintf(req, sizeof(req), "GET_PARAMETER %s RTSP/1.0\r\nCSeq: %d\r\nSession: %s\r\n\r\n", r->base, ++r->cseq, r->session);
            if (n > 0 && n < (int)sizeof(req) && ms_nc_conn_write_all(r->c, (const uint8_t *)req, n) < 0) { ret = MS_NC_E_IO; break; }
            r->last_ka = time(NULL);
        }
        if ((k = rs_need(r, 1)) != 0) { ret = k; break; }
        if (r->rbuf[r->roff] != '$') {
            /* 交织的 RTSP 响应（保活回复等）：读掉 */
            code = rs_read_resp(r, hdr, 8192, body, 65536, &blen);
            if (code < 0) { ret = code; break; }
            continue;
        }
        if ((k = rs_need(r, 4)) != 0) { ret = k; break; }
        ch = r->rbuf[r->roff + 1];
        len = (r->rbuf[r->roff + 2] << 8) | r->rbuf[r->roff + 3];
        if ((k = rs_need(r, 4 + len)) != 0) { ret = k; break; }
        if ((ch & 1) == 0) {
            r->got_media = 1;
            k = on_rtp(r, ch, r->rbuf + r->roff + 4, (size_t)len);
            if (k != 0) { ret = k; break; }
        }
        r->roff += 4 + len;
    }
    /* 播放后对端正常关闭（点播结束）：冲出未完成的视频 AU，按流结束处理 */
    if (ret == MS_NC_E_IO && r->eof && r->got_media) {
        for (i = 0; i < r->nt; i++)
            if ((r->t[i].pt == PT_H264 || r->t[i].pt == PT_H265) && !r->t[i].fu_on) (void)flush_au(r, &r->t[i]);
        ret = 0;
    } else if (ret == MS_NC_E_IO && !o->err[0]) snprintf(o->err, o->err_cap, "rtsp: connection closed");
out:
    if (r->c) {
        if (r->session[0]) {
            char req[1400];
            int n = snprintf(req, sizeof(req), "TEARDOWN %s RTSP/1.0\r\nCSeq: %d\r\nSession: %s\r\n\r\n", r->base, ++r->cseq, r->session);
            if (n > 0 && n < (int)sizeof(req)) (void)ms_nc_conn_write_all(r->c, (const uint8_t *)req, n);
        }
        ms_nc_conn_close(r->c);
    }
    for (i = 0; i < RS_MAX_TRK; i++) free(r->t[i].au);
    free(r);
    free(hdr);
    free(body);
    return ret;
}
