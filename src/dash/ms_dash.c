/*
 * MPEG-DASH MPD 调度：有界 XML 解析、分片展开，经既有 fetch 外挂
 * 取 Initialization/媒体，再把字节交给已有会话。不是 ABR/DRM，
 * 不是全部 DASH profile，不链接 OpenSSL/curl。
 * by: Long <mycjzb@gmail.com>
 */
#define _DEFAULT_SOURCE

#include "msmedia/msmedia.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DASH_DEF_MPD        (256u * 1024u)
#define DASH_DEF_SEGMENTS   1024u
#define DASH_DEF_URI        4096u
#define DASH_DEF_CHUNK      (16u * 1024u)
#define DASH_DEF_RETRIES    2
#define DASH_ABS_BUDGET     (4u * 1024u * 1024u)
#define DASH_MAX_DEPTH      16
#define DASH_MAX_ATTRS      32
#define DASH_NAME_MAX       128
#define DASH_URI_POOL       (64u * 1024u)

enum { ST_IDLE = 0, ST_REQ, ST_READ, ST_READY, ST_DONE, ST_FAILED };
enum { JOB_MPD = 0, JOB_INIT, JOB_SEG };
enum { MODE_NONE = 0, MODE_TEMPLATE, MODE_LIST, MODE_BASE };

typedef struct dash_seg {
    uint64_t number;
    uint64_t time;
    uint32_t uri_off;
    uint32_t uri_len;
    uint64_t byte_off;
    uint64_t byte_len;
} dash_seg;

typedef struct dash_ahdr { size_t charge; } dash_ahdr;

struct ms_dash {
    ms_allocator alloc;
    int using_default;
    ms_dash_limits lim;
    ms_session *session;
    int has_fetch;
    ms_hls_fetch fetch;
    int in_cb, canceled, failed, eos_sent;
    ms_status fail_st;
    char *base_uri;
    size_t base_cap;
    uint8_t *mpd_buf;
    size_t mpd_len, mpd_cap;
    char *uri_pool;
    size_t uri_pool_len, uri_pool_cap;
    dash_seg *segs;
    size_t segs_cap, nsegs;
    int is_dynamic, parsed;
    int has_init, init_done;
    uint32_t init_off, init_len;
    uint64_t init_byte_off, init_byte_len;
    uint64_t next_number, start_number;
    int have_next;
    int job, state;
    uint32_t fetch_id;
    int fetch_open;
    uint64_t expect_len, got_len;
    int retries_left;
    ms_hls_request cur_req;
    char *cur_uri;
    size_t cur_uri_cap;
    uint8_t *chunk;
    size_t chunk_cap;
    uint8_t *hold;
    size_t hold_len, hold_cap, hold_off;
    int pending_disc, pending_eoi;
    size_t bytes_used;
    char *attr_scratch;
    size_t attr_scratch_cap;
};

static int dash_charge(size_t n, size_t *out)
{
    if (n == 0 || n > (size_t)-1 - sizeof(dash_ahdr)) {
        return -1;
    }
    *out = n + sizeof(dash_ahdr);
    return 0;
}

static void *dash_alloc(ms_dash *d, size_t n)
{
    dash_ahdr *hdr;
    size_t charge, next;
    if (dash_charge(n, &charge) != 0) {
        return NULL;
    }
    if (charge > d->lim.max_bytes || d->bytes_used > d->lim.max_bytes - charge) {
        return NULL;
    }
    next = d->bytes_used + charge;
    if (d->alloc.alloc_fn == NULL) {
        hdr = (dash_ahdr *)malloc(charge);
    } else {
        hdr = (dash_ahdr *)d->alloc.alloc_fn(d->alloc.ctx, charge);
    }
    if (hdr == NULL) {
        return NULL;
    }
    hdr->charge = charge;
    d->bytes_used = next;
    return (void *)(hdr + 1);
}

static void dash_free(ms_dash *d, void *p)
{
    dash_ahdr *hdr;
    size_t charge;
    if (p == NULL) {
        return;
    }
    hdr = ((dash_ahdr *)p) - 1;
    charge = hdr->charge;
    if (charge > 0 && d->bytes_used >= charge) {
        d->bytes_used -= charge;
    } else {
        d->bytes_used = 0;
    }
    if (d->alloc.free_fn == NULL) {
        free(hdr);
        return;
    }
    d->alloc.free_fn(d->alloc.ctx, hdr);
}

static int dash_budget_from_limits(const ms_dash_limits *lim, size_t *out)
{
    size_t parts[8], i, n = 0;
    if (lim->max_segments > (size_t)-1 / sizeof(dash_seg)) {
        return -1;
    }
    parts[0] = sizeof(ms_dash);
    parts[1] = lim->max_mpd_bytes;
    parts[2] = lim->max_segments * sizeof(dash_seg);
    parts[3] = lim->max_uri_bytes;
    parts[4] = lim->max_uri_bytes;
    parts[5] = lim->read_chunk_bytes;
    parts[6] = DASH_URI_POOL;
    parts[7] = 8192u;
    for (i = 0; i < 8; i++) {
        if (parts[i] > (size_t)-1 - sizeof(dash_ahdr)) {
            return -1;
        }
        if (n > (size_t)-1 - (parts[i] + sizeof(dash_ahdr))) {
            return -1;
        }
        n += parts[i] + sizeof(dash_ahdr);
    }
    *out = n;
    return 0;
}

static void *sys_alloc(void *ctx, size_t n)
{
    (void)ctx;
    return malloc(n);
}

static void sys_free(void *ctx, void *p)
{
    (void)ctx;
    free(p);
}

void ms_dash_limits_default(ms_dash_limits *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->struct_size = sizeof(*out);
    out->max_mpd_bytes = DASH_DEF_MPD;
    out->max_segments = DASH_DEF_SEGMENTS;
    out->max_uri_bytes = DASH_DEF_URI;
    out->read_chunk_bytes = DASH_DEF_CHUNK;
    out->max_retries = DASH_DEF_RETRIES;
    out->max_bytes = 0;
}

static ms_status apply_limits(const ms_dash_limits *in, ms_dash_limits *out)
{
    size_t need = 0;
    ms_dash_limits_default(out);
    if (in != NULL) {
        if (in->struct_size < sizeof(size_t)) {
            return MS_ERR_INVALID_ARG;
        }
        if (in->max_mpd_bytes > 0) out->max_mpd_bytes = in->max_mpd_bytes;
        if (in->max_segments > 0) out->max_segments = in->max_segments;
        if (in->max_uri_bytes > 0) out->max_uri_bytes = in->max_uri_bytes;
        if (in->read_chunk_bytes > 0) out->read_chunk_bytes = in->read_chunk_bytes;
        if (in->max_retries >= 0) out->max_retries = in->max_retries;
        if (in->max_bytes > 0) out->max_bytes = in->max_bytes;
    }
    if (out->max_mpd_bytes > DASH_DEF_MPD * 4u || out->max_segments > 8192u ||
        out->max_uri_bytes > 16u * 1024u ||
        out->read_chunk_bytes > 1024u * 1024u) {
        return MS_ERR_INVALID_ARG;
    }
    if (dash_budget_from_limits(out, &need) != 0) {
        return MS_ERR_INVALID_ARG;
    }
    if (out->max_bytes == 0) {
        out->max_bytes = need;
    }
    if (out->max_bytes > DASH_ABS_BUDGET) {
        return MS_ERR_INVALID_ARG;
    }
    return MS_OK;
}

static ms_status fail_d(ms_dash *d, ms_status st)
{
    d->failed = 1;
    d->fail_st = st;
    d->state = ST_FAILED;
    return st;
}

static void close_fetch(ms_dash *d)
{
    if (d->fetch_open && d->fetch.cancel_fn != NULL) {
        d->in_cb = 1;
        d->fetch.cancel_fn(d->fetch.ctx, d->fetch_id);
        d->in_cb = 0;
    }
    d->fetch_open = 0;
}

static int is_ws(unsigned char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static int ci_eq(const char *a, size_t an, const char *b)
{
    size_t bn = strlen(b), i;
    if (an != bn) {
        return 0;
    }
    for (i = 0; i < an; i++) {
        char ca = a[i], cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
        if (ca != cb) return 0;
    }
    return 1;
}

static const char *local_name(const char *n, size_t nlen, size_t *out_len)
{
    size_t i;
    for (i = 0; i < nlen; i++) {
        if (n[i] == ':') {
            *out_len = nlen - (i + 1);
            return n + i + 1;
        }
    }
    *out_len = nlen;
    return n;
}

static int parse_u64_s(const char *s, size_t n, uint64_t *out)
{
    uint64_t v = 0;
    size_t i;
    if (n == 0) return -1;
    for (i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9') return -1;
        if (v > (UINT64_MAX - (uint64_t)(s[i] - '0')) / 10u) return -1;
        v = v * 10u + (uint64_t)(s[i] - '0');
    }
    *out = v;
    return 0;
}

static int parse_iso_dur_us(const char *s, size_t n, uint64_t *out)
{
    size_t i;
    uint64_t us = 0;
    int seen_t = 0, any = 0;
    if (n == 0 || s[0] != 'P') return -1;
    i = 1;
    while (i < n) {
        uint64_t num = 0, frac = 0;
        int digits = 0, frac_digits = 0;
        unsigned char unit;
        if (s[i] == 'T') { seen_t = 1; i++; continue; }
        while (i < n && s[i] >= '0' && s[i] <= '9') {
            if (num > (UINT64_MAX - (uint64_t)(s[i] - '0')) / 10u) return -1;
            num = num * 10u + (uint64_t)(s[i] - '0');
            digits++;
            i++;
        }
        if (i < n && s[i] == '.') {
            i++;
            while (i < n && s[i] >= '0' && s[i] <= '9') {
                if (frac_digits < 6) {
                    frac = frac * 10u + (uint64_t)(s[i] - '0');
                    frac_digits++;
                }
                i++;
            }
            while (frac_digits < 6) { frac *= 10u; frac_digits++; }
        }
        if (digits == 0 || i >= n) return -1;
        unit = (unsigned char)s[i++];
        if (!seen_t && unit == 'Y') {
            if (num > UINT64_MAX / (365ull * 24ull * 3600ull * 1000000ull)) return -1;
            us += num * 365ull * 24ull * 3600ull * 1000000ull;
        } else if (!seen_t && unit == 'M') {
            if (num > UINT64_MAX / (30ull * 24ull * 3600ull * 1000000ull)) return -1;
            us += num * 30ull * 24ull * 3600ull * 1000000ull;
        } else if (!seen_t && unit == 'D') {
            if (num > UINT64_MAX / (24ull * 3600ull * 1000000ull)) return -1;
            us += num * 24ull * 3600ull * 1000000ull;
        } else if (seen_t && unit == 'M') {
            if (num > UINT64_MAX / (60ull * 1000000ull)) return -1;
            us += num * 60ull * 1000000ull + frac * 60ull;
        } else if (unit == 'H') {
            if (!seen_t) return -1;
            if (num > UINT64_MAX / (3600ull * 1000000ull)) return -1;
            us += num * 3600ull * 1000000ull + frac * 3600ull;
        } else if (unit == 'S') {
            if (!seen_t) return -1;
            if (num > UINT64_MAX / 1000000ull) return -1;
            us += num * 1000000ull + frac;
        } else {
            return -1;
        }
        any = 1;
    }
    if (!any) return -1;
    *out = us;
    return 0;
}

static int parse_byte_range(const char *s, size_t n, uint64_t *off, uint64_t *len)
{
    size_t dash = 0;
    uint64_t a = 0, b = 0;
    while (dash < n && s[dash] != '-') dash++;
    if (dash == 0 || dash + 1 >= n) return -1;
    if (parse_u64_s(s, dash, &a) != 0 ||
        parse_u64_s(s + dash + 1, n - dash - 1, &b) != 0) return -1;
    if (b < a) return -1;
    *off = a;
    *len = b - a + 1;
    return 0;
}

/* 只展开 XML 预定义实体与数值字符引用，拒绝其它实体（防放大/外部实体）。 */
static int xml_unescape(const char *s, size_t n, char *out, size_t cap, size_t *out_n)
{
    size_t i = 0, o = 0;
    while (i < n) {
        if (s[i] != '&') {
            if (o >= cap) return -1;
            out[o++] = s[i++];
            continue;
        }
        if (i + 4 <= n && memcmp(s + i, "&lt;", 4) == 0) {
            if (o >= cap) return -1;
            out[o++] = '<'; i += 4;
        } else if (i + 4 <= n && memcmp(s + i, "&gt;", 4) == 0) {
            if (o >= cap) return -1;
            out[o++] = '>'; i += 4;
        } else if (i + 5 <= n && memcmp(s + i, "&amp;", 5) == 0) {
            if (o >= cap) return -1;
            out[o++] = '&'; i += 5;
        } else if (i + 6 <= n && memcmp(s + i, "&apos;", 6) == 0) {
            if (o >= cap) return -1;
            out[o++] = '\''; i += 6;
        } else if (i + 6 <= n && memcmp(s + i, "&quot;", 6) == 0) {
            if (o >= cap) return -1;
            out[o++] = '"'; i += 6;
        } else if (i + 3 <= n && s[i + 1] == '#') {
            uint64_t cp = 0;
            size_t j = i + 2;
            int hex = 0;
            if (j < n && (s[j] == 'x' || s[j] == 'X')) { hex = 1; j++; }
            if (j >= n) return -1;
            while (j < n && s[j] != ';') {
                unsigned char c = (unsigned char)s[j];
                uint64_t dig;
                if (hex) {
                    if (c >= '0' && c <= '9') dig = (uint64_t)(c - '0');
                    else if (c >= 'a' && c <= 'f') dig = (uint64_t)(c - 'a' + 10);
                    else if (c >= 'A' && c <= 'F') dig = (uint64_t)(c - 'A' + 10);
                    else return -1;
                    if (cp > (0x10FFFFull - dig) / 16u) return -1;
                    cp = cp * 16u + dig;
                } else {
                    if (c < '0' || c > '9') return -1;
                    if (cp > (0x10FFFFull - (uint64_t)(c - '0')) / 10u) return -1;
                    cp = cp * 10u + (uint64_t)(c - '0');
                }
                j++;
            }
            if (j >= n || s[j] != ';' || (hex && j == i + 3) || (!hex && j == i + 2))
                return -1;
            if (cp == 0 || (cp >= 0xD800ull && cp <= 0xDFFFull) || cp > 0x10FFFFull)
                return -1;
            if (cp < 0x80ull) {
                if (o >= cap) return -1;
                out[o++] = (char)cp;
            } else if (cp < 0x800ull) {
                if (o + 1 >= cap) return -1;
                out[o++] = (char)(0xC0u | (cp >> 6));
                out[o++] = (char)(0x80u | (cp & 0x3Fu));
            } else if (cp < 0x10000ull) {
                if (o + 2 >= cap) return -1;
                out[o++] = (char)(0xE0u | (cp >> 12));
                out[o++] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
                out[o++] = (char)(0x80u | (cp & 0x3Fu));
            } else {
                if (o + 3 >= cap) return -1;
                out[o++] = (char)(0xF0u | (cp >> 18));
                out[o++] = (char)(0x80u | ((cp >> 12) & 0x3Fu));
                out[o++] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
                out[o++] = (char)(0x80u | (cp & 0x3Fu));
            }
            i = j + 1;
        } else {
            return -1;
        }
    }
    *out_n = o;
    return 0;
}

typedef struct xml_attr {
    const char *name; size_t nlen;
    const char *val; size_t vlen;
} xml_attr;

enum { XT_EOF = 0, XT_START, XT_END, XT_EMPTY, XT_TEXT, XT_ERR };

typedef struct xml_cur {
    const char *p, *end;
    int kind;
    const char *name;
    size_t nlen;
    xml_attr attrs[DASH_MAX_ATTRS];
    int nattrs;
    const char *text;
    size_t tlen;
    int depth;
    int err_forbidden;
    char *scratch;
    size_t scratch_cap, scratch_used;
} xml_cur;

static void xml_skip_ws(xml_cur *x)
{
    while (x->p < x->end && is_ws((unsigned char)*x->p)) x->p++;
}

static int xml_skip_comment(xml_cur *x)
{
    const char *q;
    if (x->end - x->p < 4 || memcmp(x->p, "<!--", 4) != 0) return -1;
    q = x->p + 4;
    while (q + 2 < x->end) {
        if (q[0] == '-' && q[1] == '-') {
            if (q + 2 < x->end && q[2] == '>') { x->p = q + 3; return 0; }
            return -1;
        }
        q++;
    }
    return -1;
}

static int xml_name(xml_cur *x, const char **name, size_t *nlen)
{
    const char *s = x->p;
    if (s >= x->end) return -1;
    if (!((*s >= 'A' && *s <= 'Z') || (*s >= 'a' && *s <= 'z') || *s == '_'))
        return -1;
    s++;
    while (s < x->end &&
           ((*s >= 'A' && *s <= 'Z') || (*s >= 'a' && *s <= 'z') ||
            (*s >= '0' && *s <= '9') || *s == '_' || *s == '-' ||
            *s == '.' || *s == ':')) s++;
    *name = x->p;
    *nlen = (size_t)(s - x->p);
    if (*nlen == 0 || *nlen > DASH_NAME_MAX) return -1;
    x->p = s;
    return 0;
}

static int xml_parse_attrs(xml_cur *x)
{
    x->nattrs = 0;
    for (;;) {
        const char *nm; size_t nl, raw_n, dec_n;
        char q, *dst;
        const char *vs;
        xml_skip_ws(x);
        if (x->p >= x->end) return -1;
        if (*x->p == '/' || *x->p == '>') return 0;
        if (xml_name(x, &nm, &nl) != 0) return -1;
        xml_skip_ws(x);
        if (x->p >= x->end || *x->p != '=') return -1;
        x->p++;
        xml_skip_ws(x);
        if (x->p >= x->end || (*x->p != '"' && *x->p != '\'')) return -1;
        q = *x->p++;
        vs = x->p;
        while (x->p < x->end && *x->p != q) {
            if ((unsigned char)*x->p < 0x20 && *x->p != '\t') return -1;
            x->p++;
        }
        if (x->p >= x->end) return -1;
        raw_n = (size_t)(x->p - vs);
        x->p++;
        if (x->scratch_used >= x->scratch_cap) return -1;
        dst = x->scratch + x->scratch_used;
        if (xml_unescape(vs, raw_n, dst, x->scratch_cap - x->scratch_used, &dec_n) != 0)
            return -1;
        if (x->scratch_used + dec_n + 1 > x->scratch_cap) return -1;
        dst[dec_n] = 0;
        x->scratch_used += dec_n + 1;
        if (x->nattrs >= DASH_MAX_ATTRS) return -1;
        x->attrs[x->nattrs].name = nm;
        x->attrs[x->nattrs].nlen = nl;
        x->attrs[x->nattrs].val = dst;
        x->attrs[x->nattrs].vlen = dec_n;
        x->nattrs++;
    }
}

static int xml_has_xlink(const xml_cur *x)
{
    int i;
    for (i = 0; i < x->nattrs; i++) {
        if (x->attrs[i].nlen >= 6 && ci_eq(x->attrs[i].name, 6, "xlink:"))
            return 1;
    }
    return 0;
}

static int xml_find_attr(const xml_cur *x, const char *want, const char **val, size_t *vlen)
{
    int i;
    for (i = 0; i < x->nattrs; i++) {
        size_t ln = 0;
        const char *loc = local_name(x->attrs[i].name, x->attrs[i].nlen, &ln);
        if (ci_eq(loc, ln, want)) {
            *val = x->attrs[i].val;
            *vlen = x->attrs[i].vlen;
            return 1;
        }
    }
    return 0;
}

static int xml_next(xml_cur *x)
{
    x->kind = XT_ERR;
    x->name = NULL; x->nlen = 0; x->nattrs = 0;
    x->text = NULL; x->tlen = 0;
    if (x->p >= x->end) { x->kind = XT_EOF; return XT_EOF; }
    if (*x->p != '<') {
        const char *s = x->p;
        while (x->p < x->end && *x->p != '<') {
            if (*x->p == 0) { x->err_forbidden = 1; return XT_ERR; }
            x->p++;
        }
        x->text = s; x->tlen = (size_t)(x->p - s);
        x->kind = XT_TEXT;
        return XT_TEXT;
    }
    if (x->end - x->p >= 4 && memcmp(x->p, "<!--", 4) == 0) {
        if (xml_skip_comment(x) != 0) return XT_ERR;
        return xml_next(x);
    }
    if (x->end - x->p >= 2 && x->p[1] == '!') {
        x->err_forbidden = 1;
        return XT_ERR;
    }
    if (x->end - x->p >= 2 && x->p[1] == '?') {
        const char *q = x->p + 2;
        while (q + 1 < x->end && !(q[0] == '?' && q[1] == '>')) q++;
        if (q + 1 >= x->end) return XT_ERR;
        x->p = q + 2;
        return xml_next(x);
    }
    x->p++;
    if (x->p < x->end && *x->p == '/') {
        x->p++;
        if (xml_name(x, &x->name, &x->nlen) != 0) return XT_ERR;
        xml_skip_ws(x);
        if (x->p >= x->end || *x->p != '>') return XT_ERR;
        x->p++;
        if (x->depth <= 0) return XT_ERR;
        x->depth--;
        x->kind = XT_END;
        return XT_END;
    }
    if (xml_name(x, &x->name, &x->nlen) != 0) return XT_ERR;
    x->scratch_used = 0;
    if (xml_parse_attrs(x) != 0) return XT_ERR;
    xml_skip_ws(x);
    if (x->p < x->end && *x->p == '/') {
        x->p++;
        xml_skip_ws(x);
        if (x->p >= x->end || *x->p != '>') return XT_ERR;
        x->p++;
        x->kind = XT_EMPTY;
        return XT_EMPTY;
    }
    if (x->p >= x->end || *x->p != '>') return XT_ERR;
    x->p++;
    if (x->depth >= DASH_MAX_DEPTH) return XT_ERR;
    x->depth++;
    x->kind = XT_START;
    return XT_START;
}

static int kind_av(const char *mime, size_t mlen, const char *ctype, size_t clen)
{
    if (ci_eq(ctype, clen, "text") || ci_eq(ctype, clen, "image")) return 3;
    if (ci_eq(mime, mlen, "application/ttml+xml")) return 3;
    if (mlen >= 5 && ci_eq(mime, 5, "text/")) return 3;
    if (ci_eq(ctype, clen, "video") || ci_eq(mime, mlen, "video/mp4") ||
        ci_eq(mime, mlen, "video/mp2t") || ci_eq(mime, mlen, "video/MP2T") ||
        (mlen >= 6 && ci_eq(mime, 6, "video/"))) return 1;
    if (ci_eq(ctype, clen, "audio") || ci_eq(mime, mlen, "audio/mp4") ||
        ci_eq(mime, mlen, "audio/mp2t") ||
        (mlen >= 6 && ci_eq(mime, 6, "audio/"))) return 2;
    if (ci_eq(mime, mlen, "application/mp4") && clen == 0) return 3;
    if (clen == 0 && mlen == 0) return 0;
    return 3;
}

typedef struct tml {
    int present, mode;
    char media[512], init[512];
    uint64_t duration, timescale, start_number, pto;
    int has_duration, has_start, has_timeline;
    uint64_t tl_t[256], tl_d[256], tl_r[256];
    int tl_n, list_n;
    char list_media[256][256];
    uint64_t list_off[256], list_len[256];
    int list_has_range[256];
    char list_init[512];
    uint64_t list_init_off, list_init_len;
    int list_has_init, list_init_has_range;
    char base_init[512];
    uint64_t base_init_off, base_init_len;
    int base_has_init, base_init_has_range, base_has_index;
} tml;

typedef struct parse_out {
    int is_dynamic, has_mpd_dur, has_period_dur;
    uint64_t mpd_dur_us, period_dur_us;
    int nperiods, nav_as, nrep_chosen;
    int saw_protect, saw_xlink, saw_other, kind;
    char rep_id[128];
    uint64_t bandwidth;
    char base_url[2048];
    tml seg;
} parse_out;

static void tml_clear(tml *t)
{
    memset(t, 0, sizeof(*t));
    t->timescale = 1;
    t->start_number = 1;
}

static int copy_str(char *dst, size_t cap, const char *s, size_t n)
{
    if (n + 1 > cap) return -1;
    memcpy(dst, s, n);
    dst[n] = 0;
    return 0;
}

static int append_text(char *dst, size_t cap, const char *s, size_t n)
{
    size_t cur = strlen(dst);
    while (n > 0 && is_ws((unsigned char)s[0])) { s++; n--; }
    while (n > 0 && is_ws((unsigned char)s[n - 1])) n--;
    if (n == 0) return 0;
    if (cur + n + 1 > cap) return -1;
    memcpy(dst + cur, s, n);
    dst[cur + n] = 0;
    return 0;
}

static int resolve_copy(const char *base, const char *ref, char *out, size_t cap)
{
    if (ref == NULL || ref[0] == 0) {
        if (base == NULL || base[0] == 0) return -1;
        if (strlen(base) + 1 > cap) return -1;
        memcpy(out, base, strlen(base) + 1);
        return 0;
    }
    if (ms_hls_resolve_uri(base && base[0] ? base : "", ref, out, cap) != MS_OK)
        return -1;
    return 0;
}

static int fill_seginfo(xml_cur *x, tml *t, int which)
{
    const char *v; size_t vn;
    if (xml_has_xlink(x)) return -2;
    t->present = 1;
    if (which == MODE_TEMPLATE) {
        t->mode = MODE_TEMPLATE;
        if (xml_find_attr(x, "media", &v, &vn) && copy_str(t->media, sizeof(t->media), v, vn) != 0)
            return -1;
        if (xml_find_attr(x, "initialization", &v, &vn) &&
            copy_str(t->init, sizeof(t->init), v, vn) != 0) return -1;
        if (xml_find_attr(x, "duration", &v, &vn)) {
            if (parse_u64_s(v, vn, &t->duration) != 0) return -1;
            t->has_duration = 1;
        }
        if (xml_find_attr(x, "timescale", &v, &vn)) {
            if (parse_u64_s(v, vn, &t->timescale) != 0 || t->timescale == 0) return -1;
        }
        if (xml_find_attr(x, "startNumber", &v, &vn)) {
            if (parse_u64_s(v, vn, &t->start_number) != 0) return -1;
            t->has_start = 1;
        }
        if (xml_find_attr(x, "presentationTimeOffset", &v, &vn) &&
            parse_u64_s(v, vn, &t->pto) != 0) return -1;
    } else if (which == MODE_LIST) {
        t->mode = MODE_LIST;
        if (xml_find_attr(x, "duration", &v, &vn)) {
            if (parse_u64_s(v, vn, &t->duration) != 0) return -1;
            t->has_duration = 1;
        }
        if (xml_find_attr(x, "timescale", &v, &vn)) {
            if (parse_u64_s(v, vn, &t->timescale) != 0 || t->timescale == 0) return -1;
        }
        if (xml_find_attr(x, "startNumber", &v, &vn)) {
            if (parse_u64_s(v, vn, &t->start_number) != 0) return -1;
            t->has_start = 1;
        }
    } else if (which == MODE_BASE) {
        t->mode = MODE_BASE;
        if (xml_find_attr(x, "timescale", &v, &vn)) {
            if (parse_u64_s(v, vn, &t->timescale) != 0 || t->timescale == 0) return -1;
        }
        if (xml_find_attr(x, "indexRange", &v, &vn) && vn > 0) t->base_has_index = 1;
        if (xml_find_attr(x, "presentationTimeOffset", &v, &vn) &&
            parse_u64_s(v, vn, &t->pto) != 0) return -1;
    }
    return 0;
}

static int fill_init_or_url(xml_cur *x, tml *t, int is_init)
{
    const char *v; size_t vn;
    uint64_t off = 0, len = 0;
    int has_r = 0;
    char tmp[512];
    if (xml_has_xlink(x)) return -2;
    tmp[0] = 0;
    if (xml_find_attr(x, is_init ? "sourceURL" : "media", &v, &vn)) {
        if (copy_str(tmp, sizeof(tmp), v, vn) != 0) return -1;
    }
    if (xml_find_attr(x, is_init ? "range" : "mediaRange", &v, &vn) && vn > 0) {
        if (parse_byte_range(v, vn, &off, &len) != 0) return -1;
        has_r = 1;
    }
    if (is_init) {
        if (t->mode == MODE_LIST || t->mode == 0) {
            if (t->mode == 0) t->mode = MODE_LIST;
            if (copy_str(t->list_init, sizeof(t->list_init), tmp, strlen(tmp)) != 0)
                return -1;
            t->list_init_off = off; t->list_init_len = len;
            t->list_has_init = 1; t->list_init_has_range = has_r;
        }
        if (t->mode == MODE_BASE) {
            if (copy_str(t->base_init, sizeof(t->base_init), tmp, strlen(tmp)) != 0)
                return -1;
            t->base_init_off = off; t->base_init_len = len;
            t->base_has_init = 1; t->base_init_has_range = has_r;
        }
        if (t->mode == MODE_TEMPLATE && tmp[0] && t->init[0] == 0) {
            if (copy_str(t->init, sizeof(t->init), tmp, strlen(tmp)) != 0) return -1;
        }
        t->present = 1;
        return 0;
    }
    if (t->list_n >= 256) return -1;
    if (copy_str(t->list_media[t->list_n], sizeof(t->list_media[0]), tmp, strlen(tmp)) != 0)
        return -1;
    t->list_off[t->list_n] = off;
    t->list_len[t->list_n] = len;
    t->list_has_range[t->list_n] = has_r;
    t->list_n++;
    t->present = 1;
    if (t->mode == 0) t->mode = MODE_LIST;
    return 0;
}

static int fill_s(xml_cur *x, tml *t)
{
    const char *v; size_t vn;
    uint64_t tt = 0, dd = 0, rr = 0;
    if (t->tl_n >= 256) return -1;
    if (xml_find_attr(x, "t", &v, &vn)) {
        if (parse_u64_s(v, vn, &tt) != 0) return -1;
    } else if (t->tl_n == 0) {
        tt = t->pto;
    } else {
        uint64_t prev = t->tl_t[t->tl_n - 1];
        uint64_t span = t->tl_d[t->tl_n - 1] * (t->tl_r[t->tl_n - 1] + 1u);
        if (prev > UINT64_MAX - span) return -1;
        tt = prev + span;
    }
    if (!xml_find_attr(x, "d", &v, &vn) || parse_u64_s(v, vn, &dd) != 0 || dd == 0)
        return -1;
    if (xml_find_attr(x, "r", &v, &vn)) {
        if (v[0] == '-' || parse_u64_s(v, vn, &rr) != 0) return -1;
    }
    t->tl_t[t->tl_n] = tt;
    t->tl_d[t->tl_n] = dd;
    t->tl_r[t->tl_n] = rr;
    t->tl_n++;
    t->has_timeline = 1;
    t->present = 1;
    return 0;
}

static int parse_mpd_xml(ms_dash *d, parse_out *o)
{
    xml_cur x;
    char stack[DASH_MAX_DEPTH][DASH_NAME_MAX];
    size_t slen[DASH_MAX_DEPTH];
    int sp = 0, k, rc;
    char mpd_base[1024], per_base[1024], as_base[1024], rep_base[1024];
    tml as_tml, rep_tml;
    int in_as = 0, in_rep = 0, in_period = 0;
    int as_kind = 0, as_reps = 0, cur_kind = 0;
    char as_mime[128], as_ctype[64], as_codecs[128];
    char textbuf[2048];
    int collecting_text = 0;

    memset(o, 0, sizeof(*o));
    tml_clear(&as_tml);
    tml_clear(&rep_tml);
    mpd_base[0] = per_base[0] = as_base[0] = rep_base[0] = 0;
    as_mime[0] = as_ctype[0] = as_codecs[0] = 0;
    memset(&x, 0, sizeof(x));
    x.p = (const char *)d->mpd_buf;
    x.end = x.p + d->mpd_len;
    x.scratch = d->attr_scratch;
    x.scratch_cap = d->attr_scratch_cap;
    if (d->mpd_len == 0) return -1;
    if (memchr(d->mpd_buf, 0, d->mpd_len) != NULL) return -3;
    if (d->mpd_len >= 3 && (unsigned char)d->mpd_buf[0] == 0xEF &&
        (unsigned char)d->mpd_buf[1] == 0xBB &&
        (unsigned char)d->mpd_buf[2] == 0xBF) {
        x.p += 3;
    }
    while ((k = xml_next(&x)) != XT_EOF) {
        size_t ln = 0;
        const char *loc;
        if (k == XT_ERR) return x.err_forbidden ? -3 : -1;
        if (k == XT_TEXT) {
            if (collecting_text) {
                char tmp[2048];
                size_t dn = 0;
                if (xml_unescape(x.text, x.tlen, tmp, sizeof(tmp) - 1, &dn) != 0)
                    return -3;
                tmp[dn] = 0;
                if (append_text(textbuf, sizeof(textbuf), tmp, dn) != 0) return -1;
            }
            continue;
        }
        loc = (x.name != NULL) ? local_name(x.name, x.nlen, &ln) : "";
        if (xml_has_xlink(&x) && (k == XT_START || k == XT_EMPTY)) o->saw_xlink = 1;
        if ((k == XT_START || k == XT_EMPTY) && ci_eq(loc, ln, "ContentProtection"))
            o->saw_protect = 1;
        if (k == XT_START || k == XT_EMPTY) {
            if (ci_eq(loc, ln, "MPD")) {
                const char *v; size_t vn;
                if (xml_find_attr(&x, "type", &v, &vn) && ci_eq(v, vn, "dynamic"))
                    o->is_dynamic = 1;
                if (xml_find_attr(&x, "mediaPresentationDuration", &v, &vn)) {
                    if (parse_iso_dur_us(v, vn, &o->mpd_dur_us) != 0) return -1;
                    o->has_mpd_dur = 1;
                }
            } else if (ci_eq(loc, ln, "Period")) {
                o->nperiods++;
                in_period = 1;
                per_base[0] = 0;
                if (o->nperiods == 1) {
                    const char *v; size_t vn;
                    if (xml_find_attr(&x, "duration", &v, &vn)) {
                        if (parse_iso_dur_us(v, vn, &o->period_dur_us) != 0) return -1;
                        o->has_period_dur = 1;
                    }
                }
            } else if (ci_eq(loc, ln, "AdaptationSet")) {
                const char *v; size_t vn;
                in_as = 1; as_reps = 0; as_kind = 0;
                tml_clear(&as_tml);
                as_base[0] = as_mime[0] = as_ctype[0] = as_codecs[0] = 0;
                if (xml_find_attr(&x, "mimeType", &v, &vn))
                    copy_str(as_mime, sizeof(as_mime), v, vn);
                if (xml_find_attr(&x, "contentType", &v, &vn))
                    copy_str(as_ctype, sizeof(as_ctype), v, vn);
                if (xml_find_attr(&x, "codecs", &v, &vn))
                    copy_str(as_codecs, sizeof(as_codecs), v, vn);
                as_kind = kind_av(as_mime, strlen(as_mime), as_ctype, strlen(as_ctype));
            } else if (ci_eq(loc, ln, "Representation")) {
                const char *v; size_t vn;
                in_rep = 1; as_reps++;
                tml_clear(&rep_tml);
                rep_tml = as_tml;
                rep_base[0] = 0;
                cur_kind = as_kind;
                if (xml_find_attr(&x, "mimeType", &v, &vn))
                    cur_kind = kind_av(v, vn, as_ctype, strlen(as_ctype));
                if (xml_find_attr(&x, "bandwidth", &v, &vn))
                    (void)parse_u64_s(v, vn, &o->bandwidth);
                if (xml_find_attr(&x, "id", &v, &vn) && o->nrep_chosen == 0)
                    copy_str(o->rep_id, sizeof(o->rep_id), v, vn);
            } else if (ci_eq(loc, ln, "SegmentTemplate")) {
                tml *tg = in_rep ? &rep_tml : &as_tml;
                rc = fill_seginfo(&x, tg, MODE_TEMPLATE);
                if (rc == -2) o->saw_xlink = 1;
                else if (rc != 0) return -1;
            } else if (ci_eq(loc, ln, "SegmentList")) {
                tml *tg = in_rep ? &rep_tml : &as_tml;
                rc = fill_seginfo(&x, tg, MODE_LIST);
                if (rc == -2) o->saw_xlink = 1;
                else if (rc != 0) return -1;
            } else if (ci_eq(loc, ln, "SegmentBase")) {
                tml *tg = in_rep ? &rep_tml : &as_tml;
                rc = fill_seginfo(&x, tg, MODE_BASE);
                if (rc == -2) o->saw_xlink = 1;
                else if (rc != 0) return -1;
            } else if (ci_eq(loc, ln, "Initialization")) {
                tml *tg = in_rep ? &rep_tml : &as_tml;
                rc = fill_init_or_url(&x, tg, 1);
                if (rc == -2) o->saw_xlink = 1;
                else if (rc != 0) return -1;
            } else if (ci_eq(loc, ln, "SegmentURL")) {
                tml *tg = in_rep ? &rep_tml : &as_tml;
                rc = fill_init_or_url(&x, tg, 0);
                if (rc == -2) o->saw_xlink = 1;
                else if (rc != 0) return -1;
            } else if (ci_eq(loc, ln, "S")) {
                tml *tg = in_rep ? &rep_tml : &as_tml;
                if (fill_s(&x, tg) != 0) return -1;
            } else if (ci_eq(loc, ln, "BaseURL")) {
                collecting_text = 1;
                textbuf[0] = 0;
            }
        }
        if (k == XT_START) {
            if (sp >= DASH_MAX_DEPTH || ln >= DASH_NAME_MAX) return -1;
            memcpy(stack[sp], loc, ln);
            stack[sp][ln] = 0;
            slen[sp] = ln;
            sp++;
        }
        if (k == XT_END || k == XT_EMPTY) {
            if (ci_eq(loc, ln, "BaseURL") && collecting_text) {
                char resolved[2048];
                const char *parent;
                if (in_rep)
                    parent = rep_base[0] ? rep_base : (as_base[0] ? as_base :
                             (per_base[0] ? per_base : (mpd_base[0] ? mpd_base : d->base_uri)));
                else if (in_as)
                    parent = as_base[0] ? as_base : (per_base[0] ? per_base :
                             (mpd_base[0] ? mpd_base : d->base_uri));
                else if (in_period)
                    parent = per_base[0] ? per_base :
                             (mpd_base[0] ? mpd_base : d->base_uri);
                else
                    parent = mpd_base[0] ? mpd_base : d->base_uri;
                if (resolve_copy(parent, textbuf, resolved, sizeof(resolved)) != 0)
                    return -1;
                if (in_rep) copy_str(rep_base, sizeof(rep_base), resolved, strlen(resolved));
                else if (in_as) copy_str(as_base, sizeof(as_base), resolved, strlen(resolved));
                else if (in_period) copy_str(per_base, sizeof(per_base), resolved, strlen(resolved));
                else copy_str(mpd_base, sizeof(mpd_base), resolved, strlen(resolved));
                collecting_text = 0;
            }
            if (ci_eq(loc, ln, "Representation") && in_rep) {
                if (as_reps == 1 && (cur_kind == 1 || cur_kind == 2) &&
                    o->nrep_chosen == 0 && in_period && o->nperiods == 1) {
                    const char *b;
                    o->nrep_chosen = 1;
                    o->kind = cur_kind;
                    o->seg = rep_tml;
                    b = rep_base[0] ? rep_base : (as_base[0] ? as_base :
                        (per_base[0] ? per_base : (mpd_base[0] ? mpd_base : d->base_uri)));
                    copy_str(o->base_url, sizeof(o->base_url), b, strlen(b));
                }
                in_rep = 0;
            }
            if (ci_eq(loc, ln, "AdaptationSet") && in_as) {
                if (as_kind == 3 || cur_kind == 3) o->saw_other = 1;
                if (as_kind == 1 || as_kind == 2 ||
                    (as_reps > 0 && (cur_kind == 1 || cur_kind == 2))) {
                    o->nav_as++;
                    if (as_reps > 1) o->nrep_chosen = 99;
                }
                in_as = 0;
            }
            if (ci_eq(loc, ln, "Period")) in_period = 0;
            if (k == XT_END) {
                if (sp <= 0) return -1;
                sp--;
                if (slen[sp] != ln || memcmp(stack[sp], loc, ln) != 0) return -1;
            }
        }
    }
    if (x.depth != 0) return -1;
    if (!o->has_period_dur && o->has_mpd_dur) {
        o->period_dur_us = o->mpd_dur_us;
        o->has_period_dur = 1;
    }
    return 0;
}

static int pool_put(ms_dash *d, const char *s, uint32_t *off, uint32_t *len)
{
    size_t n = strlen(s);
    if (n > 0xFFFFFFFFu || d->uri_pool_len + n + 1 > d->uri_pool_cap) return -1;
    *off = (uint32_t)d->uri_pool_len;
    *len = (uint32_t)n;
    memcpy(d->uri_pool + d->uri_pool_len, s, n + 1);
    d->uri_pool_len += n + 1;
    return 0;
}

static int expand_one(const char *tmpl, const char *rep_id, uint64_t bw,
                      uint64_t number, uint64_t tmv, char *out, size_t cap,
                      int *unknown)
{
    size_t i = 0, o = 0, n = strlen(tmpl);
    *unknown = 0;
    while (i < n) {
        if (tmpl[i] != '$') {
            if (o + 1 >= cap) return -1;
            out[o++] = tmpl[i++];
            continue;
        }
        if (i + 1 < n && tmpl[i + 1] == '$') {
            if (o + 1 >= cap) return -1;
            out[o++] = '$';
            i += 2;
            continue;
        }
        {
            size_t j = i + 1, il = 0;
            char ident[32], fmt[16], tmp[64];
            int has_fmt = 0, nw, is_str = 0;
            uint64_t val = 0;
            const char *sval = NULL;
            while (j < n && tmpl[j] != '$' && tmpl[j] != '%') {
                if (il + 1 >= sizeof(ident)) return -1;
                ident[il++] = tmpl[j++];
            }
            ident[il] = 0;
            if (j < n && tmpl[j] == '%') {
                size_t f = 0;
                j++; has_fmt = 1;
                fmt[f++] = '%';
                while (j < n && tmpl[j] != '$') {
                    if (f + 2 >= sizeof(fmt)) return -1;
                    fmt[f++] = tmpl[j++];
                }
                fmt[f] = 0;
                if (f < 2 || (fmt[f - 1] != 'd' && fmt[f - 1] != 'i' && fmt[f - 1] != 'u'))
                    return -1;
                fmt[f - 1] = 'l'; fmt[f] = 'l'; fmt[f + 1] = 'u'; fmt[f + 2] = 0;
            }
            if (j >= n || tmpl[j] != '$') return -1;
            j++;
            if (strcmp(ident, "Number") == 0) val = number;
            else if (strcmp(ident, "Time") == 0) val = tmv;
            else if (strcmp(ident, "Bandwidth") == 0) val = bw;
            else if (strcmp(ident, "RepresentationID") == 0) {
                is_str = 1; sval = rep_id ? rep_id : "";
                if (has_fmt) return -1;
            } else {
                *unknown = 1;
                return -1;
            }
            if (is_str) nw = snprintf(tmp, sizeof(tmp), "%s", sval);
            else if (has_fmt) nw = snprintf(tmp, sizeof(tmp), fmt, (unsigned long long)val);
            else nw = snprintf(tmp, sizeof(tmp), "%llu", (unsigned long long)val);
            if (nw < 0 || (size_t)nw >= sizeof(tmp) || o + (size_t)nw >= cap) return -1;
            memcpy(out + o, tmp, (size_t)nw);
            o += (size_t)nw;
            i = j;
        }
    }
    if (o >= cap) return -1;
    out[o] = 0;
    return 0;
}

static ms_status build_window(ms_dash *d, const parse_out *o)
{
    char resolved[4096], expanded[4096];
    const tml *t = &o->seg;
    int unknown = 0;
    uint32_t off, len;
    d->nsegs = 0;
    d->uri_pool_len = 0;
    d->has_init = 0;
    d->is_dynamic = o->is_dynamic;
    d->start_number = t->has_start ? t->start_number : 1u;
    if (t->base_has_index) return MS_ERR_UNSUPPORTED;
    if (t->mode == MODE_TEMPLATE && t->init[0]) {
        if (expand_one(t->init, o->rep_id, o->bandwidth, d->start_number, t->pto,
                       expanded, sizeof(expanded), &unknown) != 0)
            return unknown ? MS_ERR_UNSUPPORTED : MS_ERR_OVERFLOW;
        if (resolve_copy(o->base_url, expanded, resolved, sizeof(resolved)) != 0)
            return MS_ERR_OVERFLOW;
        if (pool_put(d, resolved, &off, &len) != 0) return MS_ERR_OVERFLOW;
        d->has_init = 1; d->init_off = off; d->init_len = len;
        d->init_byte_off = 0; d->init_byte_len = 0;
    } else if ((t->mode == MODE_LIST && t->list_has_init) ||
               (t->mode == MODE_BASE && t->base_has_init)) {
        const char *iu = (t->mode == MODE_BASE) ? t->base_init : t->list_init;
        int has_r = (t->mode == MODE_BASE) ? t->base_init_has_range : t->list_init_has_range;
        uint64_t ioff = (t->mode == MODE_BASE) ? t->base_init_off : t->list_init_off;
        uint64_t ilen = (t->mode == MODE_BASE) ? t->base_init_len : t->list_init_len;
        if (iu[0] == 0) {
            if (resolve_copy(o->base_url, "", resolved, sizeof(resolved)) != 0)
                return MS_ERR_OVERFLOW;
        } else if (resolve_copy(o->base_url, iu, resolved, sizeof(resolved)) != 0) {
            return MS_ERR_OVERFLOW;
        }
        if (pool_put(d, resolved, &off, &len) != 0) return MS_ERR_OVERFLOW;
        d->has_init = 1; d->init_off = off; d->init_len = len;
        d->init_byte_off = has_r ? ioff : 0;
        d->init_byte_len = has_r ? ilen : 0;
    }
    if (t->mode == MODE_TEMPLATE) {
        if (t->media[0] == 0) return MS_ERR_UNSUPPORTED;
        if (t->has_timeline) {
            int i;
            uint64_t num = d->start_number;
            for (i = 0; i < t->tl_n; i++) {
                uint64_t r;
                for (r = 0; r <= t->tl_r[i]; r++) {
                    uint64_t tm;
                    dash_seg *sg;
                    if (t->tl_t[i] > UINT64_MAX - t->tl_d[i] * r) return MS_ERR_OVERFLOW;
                    tm = t->tl_t[i] + t->tl_d[i] * r;
                    if (d->nsegs >= d->segs_cap) return MS_ERR_OVERFLOW;
                    if (expand_one(t->media, o->rep_id, o->bandwidth, num, tm,
                                   expanded, sizeof(expanded), &unknown) != 0)
                        return unknown ? MS_ERR_UNSUPPORTED : MS_ERR_OVERFLOW;
                    if (resolve_copy(o->base_url, expanded, resolved, sizeof(resolved)) != 0 ||
                        pool_put(d, resolved, &off, &len) != 0)
                        return MS_ERR_OVERFLOW;
                    sg = &d->segs[d->nsegs++];
                    memset(sg, 0, sizeof(*sg));
                    sg->number = num; sg->time = tm;
                    sg->uri_off = off; sg->uri_len = len;
                    num++;
                }
            }
        } else {
            uint64_t count, i, timescale = t->timescale ? t->timescale : 1u;
            if (!t->has_duration || t->duration == 0)
                return o->is_dynamic ? MS_OK : MS_ERR_UNSUPPORTED;
            if (o->has_period_dur) {
                uint64_t ticks;
                if (timescale > UINT64_MAX / (o->period_dur_us + 1u)) return MS_ERR_OVERFLOW;
                ticks = (o->period_dur_us / 1000000ull) * timescale;
                ticks += ((o->period_dur_us % 1000000ull) * timescale) / 1000000ull;
                count = (ticks + t->duration - 1u) / t->duration;
                if (count == 0) count = 1;
            } else if (o->is_dynamic) {
                return MS_OK;
            } else {
                return MS_ERR_UNSUPPORTED;
            }
            if (count > d->segs_cap) return MS_ERR_OVERFLOW;
            for (i = 0; i < count; i++) {
                uint64_t num = d->start_number + i;
                uint64_t tm = t->pto + i * t->duration;
                dash_seg *sg;
                if (expand_one(t->media, o->rep_id, o->bandwidth, num, tm,
                               expanded, sizeof(expanded), &unknown) != 0)
                    return unknown ? MS_ERR_UNSUPPORTED : MS_ERR_OVERFLOW;
                if (resolve_copy(o->base_url, expanded, resolved, sizeof(resolved)) != 0 ||
                    pool_put(d, resolved, &off, &len) != 0)
                    return MS_ERR_OVERFLOW;
                sg = &d->segs[d->nsegs++];
                memset(sg, 0, sizeof(*sg));
                sg->number = num; sg->time = tm;
                sg->uri_off = off; sg->uri_len = len;
            }
        }
        return MS_OK;
    }
    if (t->mode == MODE_LIST) {
        int i;
        if (t->list_n == 0) return o->is_dynamic ? MS_OK : MS_ERR_UNSUPPORTED;
        if ((size_t)t->list_n > d->segs_cap) return MS_ERR_OVERFLOW;
        for (i = 0; i < t->list_n; i++) {
            dash_seg *sg;
            const char *m = t->list_media[i];
            if (m[0] == 0) {
                if (resolve_copy(o->base_url, "", resolved, sizeof(resolved)) != 0)
                    return MS_ERR_OVERFLOW;
            } else if (resolve_copy(o->base_url, m, resolved, sizeof(resolved)) != 0) {
                return MS_ERR_OVERFLOW;
            }
            if (pool_put(d, resolved, &off, &len) != 0) return MS_ERR_OVERFLOW;
            sg = &d->segs[d->nsegs++];
            memset(sg, 0, sizeof(*sg));
            sg->number = d->start_number + (uint64_t)i;
            sg->uri_off = off; sg->uri_len = len;
            if (t->list_has_range[i]) {
                sg->byte_off = t->list_off[i];
                sg->byte_len = t->list_len[i];
            }
        }
        return MS_OK;
    }
    if (t->mode == MODE_BASE) {
        dash_seg *sg;
        if (resolve_copy(o->base_url, "", resolved, sizeof(resolved)) != 0)
            return MS_ERR_OVERFLOW;
        if (pool_put(d, resolved, &off, &len) != 0) return MS_ERR_OVERFLOW;
        if (d->nsegs >= d->segs_cap) return MS_ERR_OVERFLOW;
        sg = &d->segs[d->nsegs++];
        memset(sg, 0, sizeof(*sg));
        sg->number = d->start_number;
        sg->uri_off = off; sg->uri_len = len;
        if (d->has_init && d->init_byte_len > 0) {
            if (d->init_byte_off > UINT64_MAX - d->init_byte_len) return MS_ERR_OVERFLOW;
            sg->byte_off = d->init_byte_off + d->init_byte_len;
            sg->byte_len = 0;
        }
        return MS_OK;
    }
    return MS_ERR_UNSUPPORTED;
}

static ms_status apply_parsed(ms_dash *d, const parse_out *o)
{
    ms_status st;
    if (o->saw_xlink || o->saw_protect || o->saw_other)
        return fail_d(d, MS_ERR_UNSUPPORTED);
    if (o->nperiods != 1) return fail_d(d, MS_ERR_UNSUPPORTED);
    if (o->nav_as != 1 || o->nrep_chosen != 1)
        return fail_d(d, MS_ERR_UNSUPPORTED);
    if (o->seg.mode == MODE_NONE || !o->seg.present)
        return fail_d(d, MS_ERR_UNSUPPORTED);
    st = build_window(d, o);
    if (st != MS_OK) return fail_d(d, st);
    d->parsed = 1;
    return MS_OK;
}

static ms_status parse_mpd(ms_dash *d)
{
    parse_out o;
    int rc;
    memset(&o, 0, sizeof(o));
    d->parsed = 0;
    rc = parse_mpd_xml(d, &o);
    if (rc != 0) return fail_d(d, MS_ERR_MALFORMED);
    return apply_parsed(d, &o);
}

static ms_status emit_disc(ms_dash *d)
{
    ms_status st = ms_session_signal_discontinuity(d->session);
    if (st == MS_LIMIT) { d->pending_disc = 1; return MS_LIMIT; }
    if (st != MS_OK && st != MS_AGAIN) return fail_d(d, st);
    d->pending_disc = 0;
    return MS_OK;
}

static ms_status start_request(ms_dash *d, int job, const char *uri,
                               int has_seq, uint64_t seq, int is_init,
                               uint64_t boff, uint64_t blen)
{
    ms_status st;
    uint32_t id = 0;
    size_t ulen = strlen(uri);
    if (ulen >= d->cur_uri_cap) return fail_d(d, MS_ERR_OVERFLOW);
    memcpy(d->cur_uri, uri, ulen + 1);
    memset(&d->cur_req, 0, sizeof(d->cur_req));
    d->cur_req.struct_size = sizeof(d->cur_req);
    d->cur_req.uri = d->cur_uri;
    d->cur_req.has_seq = has_seq;
    d->cur_req.media_seq = seq;
    d->cur_req.is_playlist = (job == JOB_MPD);
    d->cur_req.is_init = is_init;
    d->cur_req.byte_off = boff;
    d->cur_req.byte_len = blen;
    d->in_cb = 1;
    st = d->fetch.request_fn(d->fetch.ctx, &d->cur_req, &id);
    d->in_cb = 0;
    if (st == MS_AGAIN) { d->job = job; d->state = ST_REQ; return MS_AGAIN; }
    d->job = job;
    if (st != MS_OK) {
        if (d->retries_left > 0) { d->retries_left--; d->state = ST_REQ; return MS_AGAIN; }
        if (job == JOB_MPD || job == JOB_INIT) return fail_d(d, st);
        {
            ms_status ds = emit_disc(d);
            if (has_seq) { d->next_number = seq + 1; d->have_next = 1; }
            d->state = ST_READY;
            return (ds == MS_OK || ds == MS_LIMIT) ? ds : fail_d(d, ds);
        }
    }
    d->fetch_id = id; d->fetch_open = 1; d->job = job; d->state = ST_READ;
    d->got_len = 0; d->expect_len = blen;
    if (job == JOB_MPD) d->mpd_len = 0;
    d->hold_len = 0; d->hold_off = 0;
    return MS_OK;
}

static ms_status finish_resource(ms_dash *d, int truncated)
{
    d->fetch_open = 0;
    if (d->job == JOB_MPD) {
        ms_status st = parse_mpd(d);
        size_t n;
        if (st != MS_OK) return st;
        n = strlen(d->cur_uri);
        if (n >= d->base_cap) return fail_d(d, MS_ERR_OVERFLOW);
        memcpy(d->base_uri, d->cur_uri, n + 1);
        d->state = ST_READY;
        return MS_OK;
    }
    if (truncated) {
        ms_status st = emit_disc(d);
        if (st == MS_LIMIT) return MS_LIMIT;
        if (d->job == JOB_SEG && d->cur_req.has_seq) {
            d->next_number = d->cur_req.media_seq + 1;
            d->have_next = 1;
        }
        d->state = ST_READY;
        return MS_OK;
    }
    if (d->job == JOB_INIT) { d->init_done = 1; d->state = ST_READY; return MS_OK; }
    if (d->job == JOB_SEG && d->cur_req.has_seq) {
        d->next_number = d->cur_req.media_seq + 1;
        d->have_next = 1;
    }
    d->state = ST_READY;
    return MS_OK;
}

static dash_seg *first_available(ms_dash *d)
{
    size_t i;
    for (i = 0; i < d->nsegs; i++) {
        if (!d->have_next || d->segs[i].number >= d->next_number)
            return &d->segs[i];
    }
    return NULL;
}

static ms_status begin_next_job(ms_dash *d)
{
    dash_seg *sg;
    ms_status st;
    char uri[4096];
    if (d->pending_disc) {
        st = emit_disc(d);
        if (st != MS_OK) return st;
    }
    if (d->pending_eoi) {
        st = ms_session_end_of_input(d->session);
        if (st == MS_LIMIT) return MS_LIMIT;
        if (st != MS_OK && st != MS_AGAIN && st != MS_ERR_NOT_SEEKABLE)
            return fail_d(d, st);
        d->pending_eoi = 0; d->eos_sent = 1; d->state = ST_DONE;
        return MS_EOS;
    }
    if (d->has_init && !d->init_done) {
        size_t n = d->init_len;
        if (n >= sizeof(uri)) return fail_d(d, MS_ERR_OVERFLOW);
        memcpy(uri, d->uri_pool + d->init_off, n); uri[n] = 0;
        d->retries_left = d->lim.max_retries;
        return start_request(d, JOB_INIT, uri, 0, 0, 1, d->init_byte_off, d->init_byte_len);
    }
    sg = first_available(d);
    if (sg == NULL) {
        if (!d->is_dynamic) {
            st = ms_session_end_of_input(d->session);
            if (st == MS_LIMIT) { d->pending_eoi = 1; return MS_LIMIT; }
            if (st != MS_OK && st != MS_AGAIN && st != MS_ERR_NOT_SEEKABLE)
                return fail_d(d, st);
            d->eos_sent = 1; d->state = ST_DONE;
            return MS_EOS;
        }
        d->retries_left = d->lim.max_retries;
        return start_request(d, JOB_MPD, d->base_uri, 0, 0, 0, 0, 0);
    }
    if (d->have_next && sg->number != d->next_number) {
        st = emit_disc(d);
        if (st == MS_LIMIT) return MS_LIMIT;
    }
    {
        size_t n = sg->uri_len;
        if (n >= sizeof(uri)) return fail_d(d, MS_ERR_OVERFLOW);
        memcpy(uri, d->uri_pool + sg->uri_off, n); uri[n] = 0;
    }
    d->retries_left = d->lim.max_retries;
    return start_request(d, JOB_SEG, uri, 1, sg->number, 0, sg->byte_off, sg->byte_len);
}

static ms_status do_read(ms_dash *d)
{
    size_t n = 0, used = 0, want = d->chunk_cap;
    ms_status st;
    if (d->hold_len > d->hold_off) {
        size_t left = d->hold_len - d->hold_off;
        used = 0;
        st = ms_session_push(d->session, d->hold + d->hold_off, left, &used);
        d->hold_off += used;
        if (d->hold_off >= d->hold_len) { d->hold_len = 0; d->hold_off = 0; }
        if (st == MS_LIMIT) return MS_LIMIT;
        if (st != MS_OK && st != MS_AGAIN) return fail_d(d, st);
        return MS_OK;
    }
    if (d->expect_len > 0 && d->got_len >= d->expect_len)
        return finish_resource(d, 0);
    if (d->expect_len > 0 && (d->expect_len - d->got_len) < (uint64_t)want)
        want = (size_t)(d->expect_len - d->got_len);
    d->in_cb = 1;
    st = d->fetch.read_fn(d->fetch.ctx, d->fetch_id, d->chunk, want, &n);
    d->in_cb = 0;
    if (st == MS_AGAIN) return MS_AGAIN;
    if (st == MS_ERR_CANCELED) { d->canceled = 1; return MS_ERR_CANCELED; }
    if (st != MS_OK) {
        close_fetch(d);
        if (d->retries_left > 0) { d->retries_left--; d->state = ST_REQ; return MS_AGAIN; }
        if (d->job == JOB_MPD || d->job == JOB_INIT) return fail_d(d, st);
        st = emit_disc(d);
        if (d->cur_req.has_seq) { d->next_number = d->cur_req.media_seq + 1; d->have_next = 1; }
        d->state = ST_READY;
        return (st == MS_OK || st == MS_LIMIT) ? st : fail_d(d, st);
    }
    if (n == 0) {
        int trunc = (d->expect_len > 0 && d->got_len < d->expect_len);
        if (d->job == JOB_MPD && d->mpd_len == 0) return fail_d(d, MS_ERR_MALFORMED);
        return finish_resource(d, trunc);
    }
    d->got_len += n;
    if (d->job == JOB_MPD) {
        if (d->mpd_len + n > d->mpd_cap) return fail_d(d, MS_ERR_OVERFLOW);
        memcpy(d->mpd_buf + d->mpd_len, d->chunk, n);
        d->mpd_len += n;
        return MS_OK;
    }
    used = 0;
    st = ms_session_push(d->session, d->chunk, n, &used);
    if (used < n) {
        size_t left = n - used;
        if (d->hold != NULL && d->hold_cap < left) {
            dash_free(d, d->hold); d->hold = NULL; d->hold_cap = 0;
        }
        if (d->hold == NULL) {
            d->hold = (uint8_t *)dash_alloc(d, left);
            if (d->hold == NULL) return fail_d(d, MS_ERR_NOMEM);
            d->hold_cap = left;
        }
        memcpy(d->hold, d->chunk + used, left);
        d->hold_len = left; d->hold_off = 0;
    }
    if (st == MS_LIMIT) return MS_LIMIT;
    if (st != MS_OK && st != MS_AGAIN) return fail_d(d, st);
    return MS_OK;
}

ms_status ms_dash_create(const ms_allocator *alloc, const ms_dash_limits *limits,
                         ms_dash **out)
{
    ms_dash *d;
    ms_dash_limits lim;
    ms_status st;
    if (out == NULL) return MS_ERR_INVALID_ARG;
    *out = NULL;
    st = apply_limits(limits, &lim);
    if (st != MS_OK) return st;
    d = (ms_dash *)calloc(1, sizeof(*d));
    if (d == NULL) return MS_ERR_NOMEM;
    if (alloc != NULL && alloc->alloc_fn != NULL) {
        d->alloc = *alloc;
        d->using_default = 0;
    } else {
        d->alloc.ctx = NULL;
        d->alloc.alloc_fn = sys_alloc;
        d->alloc.free_fn = sys_free;
        d->using_default = 1;
    }
    d->lim = lim;
    d->bytes_used = sizeof(*d);
    if (d->bytes_used > d->lim.max_bytes) { free(d); return MS_ERR_NOMEM; }
    d->base_cap = lim.max_uri_bytes;
    d->mpd_cap = lim.max_mpd_bytes;
    d->segs_cap = lim.max_segments;
    d->chunk_cap = lim.read_chunk_bytes;
    d->cur_uri_cap = lim.max_uri_bytes;
    d->uri_pool_cap = DASH_URI_POOL;
    d->attr_scratch_cap = 8192u;
    d->base_uri = (char *)dash_alloc(d, d->base_cap);
    d->mpd_buf = (uint8_t *)dash_alloc(d, d->mpd_cap);
    d->segs = (dash_seg *)dash_alloc(d, d->segs_cap * sizeof(dash_seg));
    d->chunk = (uint8_t *)dash_alloc(d, d->chunk_cap);
    d->cur_uri = (char *)dash_alloc(d, d->cur_uri_cap);
    d->uri_pool = (char *)dash_alloc(d, d->uri_pool_cap);
    d->attr_scratch = (char *)dash_alloc(d, d->attr_scratch_cap);
    if (d->base_uri == NULL || d->mpd_buf == NULL || d->segs == NULL ||
        d->chunk == NULL || d->cur_uri == NULL || d->uri_pool == NULL ||
        d->attr_scratch == NULL) {
        ms_dash_destroy(d);
        return MS_ERR_NOMEM;
    }
    d->state = ST_IDLE;
    *out = d;
    return MS_OK;
}

void ms_dash_destroy(ms_dash *d)
{
    if (d == NULL) return;
    close_fetch(d);
    dash_free(d, d->hold);
    dash_free(d, d->base_uri);
    dash_free(d, d->mpd_buf);
    dash_free(d, d->segs);
    dash_free(d, d->chunk);
    dash_free(d, d->cur_uri);
    dash_free(d, d->uri_pool);
    dash_free(d, d->attr_scratch);
    free(d);
}

ms_status ms_dash_bind(ms_dash *d, ms_session *session, const ms_hls_fetch *fetch)
{
    if (d == NULL || session == NULL || fetch == NULL) return MS_ERR_INVALID_ARG;
    if (fetch->struct_size < sizeof(*fetch) || fetch->request_fn == NULL ||
        fetch->read_fn == NULL) return MS_ERR_INVALID_ARG;
    if (d->state != ST_IDLE) return MS_ERR_STATE;
    d->session = session;
    d->fetch = *fetch;
    d->has_fetch = 1;
    return MS_OK;
}

ms_status ms_dash_open(ms_dash *d, const char *mpd_uri)
{
    size_t n;
    if (d == NULL || mpd_uri == NULL || mpd_uri[0] == 0) return MS_ERR_INVALID_ARG;
    if (!d->has_fetch || d->session == NULL) return MS_ERR_STATE;
    if (d->state != ST_IDLE) return MS_ERR_STATE;
    n = strlen(mpd_uri);
    if (n >= d->base_cap) return MS_ERR_OVERFLOW;
    memcpy(d->base_uri, mpd_uri, n + 1);
    d->retries_left = d->lim.max_retries;
    d->state = ST_REQ;
    d->job = JOB_MPD;
    d->canceled = 0; d->failed = 0; d->eos_sent = 0;
    d->init_done = 0; d->have_next = 0; d->next_number = 0; d->parsed = 0;
    return MS_OK;
}

ms_status ms_dash_cancel(ms_dash *d)
{
    if (d == NULL) return MS_ERR_INVALID_ARG;
    d->canceled = 1;
    close_fetch(d);
    return MS_OK;
}

ms_status ms_dash_drive(ms_dash *d)
{
    ms_status st;
    if (d == NULL) return MS_ERR_INVALID_ARG;
    if (d->in_cb) return MS_ERR_STATE;
    if (d->canceled) return MS_ERR_CANCELED;
    if (d->failed) return d->fail_st;
    if (!d->has_fetch || d->session == NULL) return MS_ERR_STATE;
    if (d->state == ST_DONE && d->eos_sent) return MS_EOS;
    if (d->state == ST_IDLE) return MS_ERR_STATE;
    if (d->state == ST_REQ) {
        if (d->job == JOB_MPD) {
            st = start_request(d, JOB_MPD,
                               d->fetch_open ? d->cur_uri : d->base_uri,
                               0, 0, 0, 0, 0);
        } else {
            st = start_request(d, d->job, d->cur_uri, d->cur_req.has_seq,
                               d->cur_req.media_seq, d->cur_req.is_init,
                               d->cur_req.byte_off, d->cur_req.byte_len);
        }
        if (st == MS_AGAIN || st == MS_OK || st == MS_LIMIT) return st;
        if (d->job == JOB_SEG) {
            ms_status ds = emit_disc(d);
            if (d->cur_req.has_seq) {
                d->next_number = d->cur_req.media_seq + 1;
                d->have_next = 1;
            }
            d->state = ST_READY;
            return (ds == MS_OK || ds == MS_LIMIT) ? ds : fail_d(d, ds);
        }
        return fail_d(d, st);
    }
    if (d->state == ST_READ) return do_read(d);
    if (d->state == ST_READY) return begin_next_job(d);
    return MS_ERR_STATE;
}
