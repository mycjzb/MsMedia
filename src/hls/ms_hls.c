/*
 * HLS media playlist 调度：解析清单、解析 URI、经 fetch 外挂取分片，
 * 再把字节交给已有会话解封装。不是 HTTP 客户端，不是 DASH。
 * by: Long <mycjzb@gmail.com>
 */
#define _DEFAULT_SOURCE

#include "msmedia/msmedia.h"

#include <fcntl.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#define HLS_DEF_PLAYLIST    (256u * 1024u)
#define HLS_DEF_SEGMENTS    1024u
#define HLS_DEF_URI         4096u
#define HLS_DEF_CHUNK       (16u * 1024u)
#define HLS_DEF_RETRIES     2
#define HLS_ABS_BUDGET      (4u * 1024u * 1024u)

enum {
    ST_IDLE = 0,
    ST_REQ,
    ST_READ,
    ST_READY,
    ST_DONE,
    ST_FAILED
};

enum {
    JOB_PLAYLIST = 0,
    JOB_INIT,
    JOB_SEG
};

typedef struct hls_seg {
    uint64_t seq;
    uint32_t uri_off;
    uint32_t uri_len;
    uint64_t byte_off;
    uint64_t byte_len;
    int disc;
    int has_map;
    uint32_t map_off;
    uint32_t map_len;
    uint64_t map_byte_off;
    uint64_t map_byte_len;
    uint32_t dur_ms;      /* EXTINF 时长（毫秒），供预加载器按秒数前瞻 */
} hls_seg;

struct ms_hls {
    ms_allocator alloc;
    int using_default;
    ms_hls_limits lim;
    ms_session *session;
    int has_fetch;
    ms_hls_fetch fetch;
    int in_cb;
    int canceled;
    int failed;
    ms_status fail_st;
    int eos_sent;

    char *base_uri;
    size_t base_cap;

    uint8_t *pl_buf;
    size_t pl_len;
    size_t pl_cap;
    int pl_complete;

    hls_seg *segs;
    size_t segs_cap;
    size_t nsegs;
    uint64_t media_seq0;
    int have_seq0;
    int have_target;
    int endlist;
    int version;
    int saw_extm3u;
    int saw_master;
    int saw_media;
    int saw_key_bad;
    int saw_llhls;
    int saw_unsupported;

    uint64_t next_seq;
    int have_next;
    int init_done;
    uint32_t last_map_off;
    uint32_t last_map_len;
    uint64_t last_map_offb;
    uint64_t last_map_lenb;
    int have_map;

    int job;
    int state;
    uint32_t fetch_id;
    int fetch_open;
    uint64_t expect_len; /* 0 unknown */
    uint64_t got_len;
    int retries_left;
    ms_hls_request cur_req;
    char cur_uri[HLS_DEF_URI]; /* may be smaller than max_uri; use heap if needed */
    char *uri_scratch;
    size_t uri_scratch_cap;

    uint8_t *chunk;
    size_t chunk_cap;
    uint8_t *hold;
    size_t hold_len;
    size_t hold_cap;
    size_t hold_off;

    int pending_disc;
    int pending_eoi;

    size_t bytes_used; /* 调度器自身占用，含对象与带头堆块 */
};

typedef struct hls_ahdr {
    size_t charge;
} hls_ahdr;

static int hls_charge(size_t n, size_t *out)
{
    if (n == 0 || n > (size_t)-1 - sizeof(hls_ahdr)) {
        return -1;
    }
    *out = n + sizeof(hls_ahdr);
    return 0;
}

static void *hls_alloc(ms_hls *h, size_t n)
{
    hls_ahdr *hdr;
    size_t charge;
    size_t next;
    if (hls_charge(n, &charge) != 0) {
        return NULL;
    }
    if (charge > h->lim.max_bytes || h->bytes_used > h->lim.max_bytes - charge) {
        return NULL;
    }
    next = h->bytes_used + charge;
    if (h->alloc.alloc_fn == NULL) {
        hdr = (hls_ahdr *)malloc(charge);
    } else {
        hdr = (hls_ahdr *)h->alloc.alloc_fn(h->alloc.ctx, charge);
    }
    if (hdr == NULL) {
        return NULL;
    }
    hdr->charge = charge;
    h->bytes_used = next;
    return (void *)(hdr + 1);
}

static void hls_free(ms_hls *h, void *p)
{
    hls_ahdr *hdr;
    size_t charge;
    if (p == NULL) {
        return;
    }
    hdr = ((hls_ahdr *)p) - 1;
    charge = hdr->charge;
    if (charge > 0 && h->bytes_used >= charge) {
        h->bytes_used -= charge;
    } else {
        h->bytes_used = 0;
    }
    if (h->alloc.free_fn == NULL) {
        free(hdr);
        return;
    }
    h->alloc.free_fn(h->alloc.ctx, hdr);
}

static int hls_budget_from_limits(const ms_hls_limits *lim, size_t *out)
{
    size_t n;
    size_t parts[7];
    size_t i;
    size_t segs;
    if (lim->max_segments > (size_t)-1 / sizeof(hls_seg)) {
        return -1;
    }
    segs = lim->max_segments * sizeof(hls_seg);
    parts[0] = sizeof(struct ms_hls);
    parts[1] = segs;
    parts[2] = lim->max_playlist_bytes;
    parts[3] = lim->read_chunk_bytes;
    parts[4] = lim->max_uri_bytes;
    parts[5] = lim->max_uri_bytes;
    parts[6] = lim->read_chunk_bytes; /* hold 上限：一块残留 */
    n = 8u * sizeof(hls_ahdr);
    for (i = 0; i < 7; i++) {
        if (n > (size_t)-1 - parts[i]) {
            return -1;
        }
        n += parts[i];
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

void ms_hls_limits_default(ms_hls_limits *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->struct_size = sizeof(*out);
    out->max_playlist_bytes = HLS_DEF_PLAYLIST;
    out->max_segments = HLS_DEF_SEGMENTS;
    out->max_uri_bytes = HLS_DEF_URI;
    out->read_chunk_bytes = HLS_DEF_CHUNK;
    out->max_retries = HLS_DEF_RETRIES;
}

static ms_status apply_limits(const ms_hls_limits *in, ms_hls_limits *out)
{
    ms_hls_limits d;
    ms_hls_limits_default(&d);
    if (in != NULL && in->struct_size >= sizeof(size_t) &&
        in->struct_size <= sizeof(*in)) {
        size_t n = in->struct_size;
        memset(out, 0, sizeof(*out));
        memcpy(out, in, n);
        out->struct_size = sizeof(*out);
    } else if (in == NULL) {
        *out = d;
    } else {
        return MS_ERR_INVALID_ARG;
    }
    if (out->max_playlist_bytes == 0) {
        out->max_playlist_bytes = d.max_playlist_bytes;
    }
    if (out->max_segments == 0) {
        out->max_segments = d.max_segments;
    }
    if (out->max_uri_bytes == 0) {
        out->max_uri_bytes = d.max_uri_bytes;
    }
    if (out->read_chunk_bytes == 0) {
        out->read_chunk_bytes = d.read_chunk_bytes;
    }
    if (out->max_retries < 0) {
        out->max_retries = d.max_retries;
    }
    if (out->max_playlist_bytes > d.max_playlist_bytes ||
        out->max_segments > d.max_segments ||
        out->max_uri_bytes > d.max_uri_bytes ||
        out->read_chunk_bytes > (1u * 1024u * 1024u)) {
        return MS_ERR_INVALID_ARG;
    }
    {
        size_t need;
        if (hls_budget_from_limits(out, &need) != 0) {
            return MS_ERR_OVERFLOW;
        }
        if (out->max_bytes == 0) {
            out->max_bytes = need;
        }
        if (out->max_bytes > HLS_ABS_BUDGET) {
            return MS_ERR_INVALID_ARG;
        }
    }
    return MS_OK;
}

/* ---------------- URI (RFC 3986) ---------------- */

typedef struct uri_parts {
    const char *scheme;
    size_t scheme_len;
    const char *auth;
    size_t auth_len;
    const char *path;
    size_t path_len;
    const char *query;
    size_t query_len;
    int has_query;
    int has_auth;
} uri_parts;

static int is_scheme_char(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.';
}

static void parse_uri(const char *s, uri_parts *u)
{
    const char *p;
    const char *hash;
    size_t n;
    memset(u, 0, sizeof(*u));
    if (s == NULL) {
        return;
    }
    hash = strchr(s, '#');
    n = (hash != NULL) ? (size_t)(hash - s) : strlen(s);
    p = s;
    if (n >= 2 && is_scheme_char(p[0])) {
        size_t i = 1;
        while (i < n && is_scheme_char(p[i])) {
            i++;
        }
        if (i < n && p[i] == ':') {
            u->scheme = p;
            u->scheme_len = i;
            p += i + 1;
            n -= i + 1;
        }
    }
    if (n >= 2 && p[0] == '/' && p[1] == '/') {
        const char *a = p + 2;
        const char *slash;
        size_t alen = n - 2;
        slash = memchr(a, '/', alen);
        u->has_auth = 1;
        u->auth = a;
        if (slash != NULL) {
            u->auth_len = (size_t)(slash - a);
            p = slash;
            n = (size_t)((s + (hash ? (size_t)(hash - s) : strlen(s))) - slash);
        } else {
            const char *q = memchr(a, '?', alen);
            u->auth_len = q ? (size_t)(q - a) : alen;
            p = a + u->auth_len;
            n = alen - u->auth_len;
        }
    }
    {
        const char *q = memchr(p, '?', n);
        u->path = p;
        if (q != NULL) {
            u->path_len = (size_t)(q - p);
            u->has_query = 1;
            u->query = q + 1;
            u->query_len = n - u->path_len - 1;
        } else {
            u->path_len = n;
        }
    }
}

static size_t remove_dot_segments(char *path, size_t n)
{
    char *in = path;
    char *out = path;
    char *end = path + n;
    while (in < end) {
        if (in[0] == '.' && (in + 1 == end || in[1] == '/')) {
            in += (in + 1 < end && in[1] == '/') ? 2 : 1;
            continue;
        }
        if (in[0] == '.' && in + 1 < end && in[1] == '.' &&
            (in + 2 == end || in[2] == '/')) {
            if (out > path) {
                out--;
                if (out > path && *out == '/') {
                    out--;
                }
                while (out > path && *out != '/') {
                    out--;
                }
                if (out == path && *out != '/') {
                    /* keep */
                } else if (out == path && *out == '/') {
                    out++;
                } else {
                    out++;
                }
            }
            in += (in + 2 < end && in[2] == '/') ? 3 : 2;
            continue;
        }
        *out++ = *in++;
    }
    return (size_t)(out - path);
}

static int append_slice(char *out, size_t cap, size_t *len,
                        const char *s, size_t n)
{
    if (n > cap - *len - 1) {
        return -1;
    }
    if (n > 0) {
        memcpy(out + *len, s, n);
        *len += n;
    }
    out[*len] = 0;
    return 0;
}

ms_status ms_hls_resolve_uri(const char *base, const char *ref,
                             char *out, size_t cap)
{
    uri_parts b, r;
    size_t len = 0;
    char pathbuf[4096];
    size_t plen = 0;

    if (out == NULL || cap < 2) {
        return MS_ERR_INVALID_ARG;
    }
    out[0] = 0;
    if (ref == NULL || ref[0] == 0) {
        if (base == NULL) {
            return MS_ERR_INVALID_ARG;
        }
        ref = "";
    }
    parse_uri(base, &b);
    parse_uri(ref, &r);
    if (r.scheme_len > 0) {
        /* 绝对引用 */
        if (append_slice(out, cap, &len, r.scheme, r.scheme_len) != 0 ||
            append_slice(out, cap, &len, ":", 1) != 0) {
            return MS_ERR_OVERFLOW;
        }
        if (r.has_auth) {
            if (append_slice(out, cap, &len, "//", 2) != 0 ||
                append_slice(out, cap, &len, r.auth, r.auth_len) != 0) {
                return MS_ERR_OVERFLOW;
            }
        }
        if (plen + r.path_len >= sizeof(pathbuf)) {
            return MS_ERR_OVERFLOW;
        }
        if (r.path_len > 0) {
            memcpy(pathbuf, r.path, r.path_len);
        }
        plen = remove_dot_segments(pathbuf, r.path_len);
        if (append_slice(out, cap, &len, pathbuf, plen) != 0) {
            return MS_ERR_OVERFLOW;
        }
        if (r.has_query) {
            if (append_slice(out, cap, &len, "?", 1) != 0 ||
                append_slice(out, cap, &len, r.query, r.query_len) != 0) {
                return MS_ERR_OVERFLOW;
            }
        }
        return MS_OK;
    }
    if (b.scheme_len > 0) {
        if (append_slice(out, cap, &len, b.scheme, b.scheme_len) != 0 ||
            append_slice(out, cap, &len, ":", 1) != 0) {
            return MS_ERR_OVERFLOW;
        }
    }
    if (r.has_auth) {
        if (append_slice(out, cap, &len, "//", 2) != 0 ||
            append_slice(out, cap, &len, r.auth, r.auth_len) != 0) {
            return MS_ERR_OVERFLOW;
        }
        if (r.path_len >= sizeof(pathbuf)) {
            return MS_ERR_OVERFLOW;
        }
        memcpy(pathbuf, r.path, r.path_len);
        plen = remove_dot_segments(pathbuf, r.path_len);
        if (append_slice(out, cap, &len, pathbuf, plen) != 0) {
            return MS_ERR_OVERFLOW;
        }
        if (r.has_query) {
            if (append_slice(out, cap, &len, "?", 1) != 0 ||
                append_slice(out, cap, &len, r.query, r.query_len) != 0) {
                return MS_ERR_OVERFLOW;
            }
        }
        return MS_OK;
    }
    if (b.has_auth) {
        if (append_slice(out, cap, &len, "//", 2) != 0 ||
            append_slice(out, cap, &len, b.auth, b.auth_len) != 0) {
            return MS_ERR_OVERFLOW;
        }
    }
    /* merge path（RFC 3986 5.2.2）：空 path 沿用基址 path。 */
    if (r.path_len == 0) {
        if (b.path_len >= sizeof(pathbuf)) {
            return MS_ERR_OVERFLOW;
        }
        if (b.path_len > 0) {
            memcpy(pathbuf, b.path, b.path_len);
        }
        plen = b.path_len;
    } else if (r.path[0] == '/') {
        if (r.path_len >= sizeof(pathbuf)) {
            return MS_ERR_OVERFLOW;
        }
        memcpy(pathbuf, r.path, r.path_len);
        plen = remove_dot_segments(pathbuf, r.path_len);
    } else {
        /* base path minus last segment, plus ref path */
        size_t bpl = b.path_len;
        size_t slash = 0;
        size_t i;
        for (i = 0; i < bpl; i++) {
            if (b.path[i] == '/') {
                slash = i + 1;
            }
        }
        if (slash + r.path_len >= sizeof(pathbuf)) {
            return MS_ERR_OVERFLOW;
        }
        if (slash > 0) {
            memcpy(pathbuf, b.path, slash);
        }
        memcpy(pathbuf + slash, r.path, r.path_len);
        plen = remove_dot_segments(pathbuf, slash + r.path_len);
    }
    if (append_slice(out, cap, &len, pathbuf, plen) != 0) {
        return MS_ERR_OVERFLOW;
    }
    if (r.has_query) {
        if (append_slice(out, cap, &len, "?", 1) != 0 ||
            append_slice(out, cap, &len, r.query, r.query_len) != 0) {
            return MS_ERR_OVERFLOW;
        }
    } else if (r.path_len == 0 && b.has_query) {
        if (append_slice(out, cap, &len, "?", 1) != 0 ||
            append_slice(out, cap, &len, b.query, b.query_len) != 0) {
            return MS_ERR_OVERFLOW;
        }
    }
    if (len == 0) {
        return MS_ERR_INVALID_ARG;
    }
    return MS_OK;
}

/* ---------------- playlist parse ---------------- */

static void skip_ws(const char **p, const char *end)
{
    while (*p < end && (**p == ' ' || **p == '\t')) {
        (*p)++;
    }
}

static int starts_tag(const char *s, const char *end, const char *tag)
{
    size_t n = strlen(tag);
    return (size_t)(end - s) >= n && memcmp(s, tag, n) == 0;
}

static int parse_u64(const char *s, const char *end, uint64_t *out)
{
    uint64_t v = 0;
    int any = 0;
    while (s < end && *s >= '0' && *s <= '9') {
        if (v > (UINT64_MAX - (uint64_t)(*s - '0')) / 10u) {
            return -1;
        }
        v = v * 10u + (uint64_t)(*s - '0');
        s++;
        any = 1;
    }
    if (!any) {
        return -1;
    }
    *out = v;
    return 0;
}

static int attr_find(const char *s, const char *end, const char *key,
                     const char **vs, const char **ve, int *quoted)
{
    size_t klen = strlen(key);
    const char *p = s;
    *quoted = 0;
    while (p < end) {
        skip_ws(&p, end);
        if (p + klen < end && memcmp(p, key, klen) == 0 && p[klen] == '=') {
            p += klen + 1;
            if (p < end && *p == '"') {
                const char *q;
                p++;
                q = memchr(p, '"', (size_t)(end - p));
                if (q == NULL) {
                    return -1;
                }
                *vs = p;
                *ve = q;
                *quoted = 1;
                return 0;
            }
            {
                const char *q = p;
                while (q < end && *q != ',') {
                    q++;
                }
                *vs = p;
                *ve = q;
                return 0;
            }
        }
        /* skip this attr */
        if (p < end && *p == '"') {
            p++;
            while (p < end && *p != '"') {
                p++;
            }
            if (p < end) {
                p++;
            }
        }
        while (p < end && *p != ',') {
            p++;
        }
        if (p < end && *p == ',') {
            p++;
        }
    }
    return -1;
}

static int parse_byterange(const char *s, const char *end,
                           uint64_t *len, uint64_t *off)
{
    const char *at;
    *off = 0;
    at = memchr(s, '@', (size_t)(end - s));
    if (parse_u64(s, at ? at : end, len) != 0) {
        return -1;
    }
    if (at != NULL) {
        if (parse_u64(at + 1, end, off) != 0) {
            return -1;
        }
    }
    return 0;
}

static void reset_pl(ms_hls *h)
{
    h->nsegs = 0;
    h->media_seq0 = 0;
    h->have_seq0 = 0;
    h->have_target = 0;
    h->endlist = 0;
    h->version = 0;
    h->saw_extm3u = 0;
    h->saw_master = 0;
    h->saw_media = 0;
    h->saw_key_bad = 0;
    h->saw_llhls = 0;
    h->saw_unsupported = 0;
}

static ms_status fail_h(ms_hls *h, ms_status st)
{
    h->failed = 1;
    h->fail_st = st;
    h->state = ST_FAILED;
    return st;
}

static ms_status parse_playlist(ms_hls *h)
{
    const char *p = (const char *)h->pl_buf;
    const char *end = p + h->pl_len;
    uint64_t seq;
    int pend_disc = 0;
    int pend_br = 0;
    uint32_t pend_dur_ms = 0;
    uint64_t br_len = 0, br_off = 0;
    int have_map = 0;
    uint32_t map_off = 0, map_len = 0;
    uint64_t map_boff = 0, map_blen = 0;
    int first_line = 1;

    reset_pl(h);
    /* BOM */
    if (h->pl_len >= 3 && (unsigned char)p[0] == 0xEF &&
        (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF) {
        p += 3;
    }
    seq = 0;
    while (p < end) {
        const char *ls = p;
        const char *le;
        while (p < end && *p != '\n' && *p != '\r') {
            p++;
        }
        le = p;
        if (p < end && *p == '\r') {
            p++;
        }
        if (p < end && *p == '\n') {
            p++;
        }
        while (ls < le && (le[-1] == ' ' || le[-1] == '\t')) {
            le--;
        }
        while (ls < le && (*ls == ' ' || *ls == '\t')) {
            ls++;
        }
        if (ls >= le) {
            continue;
        }
        if (first_line) {
            first_line = 0;
            if (!starts_tag(ls, le, "#EXTM3U")) {
                return fail_h(h, MS_ERR_MALFORMED);
            }
            h->saw_extm3u = 1;
            continue;
        }
        if (ls[0] == '#' && !starts_tag(ls, le, "#EXT")) {
            continue;
        }
        if (starts_tag(ls, le, "#EXT-X-VERSION:")) {
            uint64_t v = 0;
            if (parse_u64(ls + 15, le, &v) != 0) {
                return fail_h(h, MS_ERR_MALFORMED);
            }
            h->version = (int)v;
            continue;
        }
        if (starts_tag(ls, le, "#EXT-X-TARGETDURATION:")) {
            uint64_t v = 0;
            if (parse_u64(ls + 22, le, &v) != 0) {
                return fail_h(h, MS_ERR_MALFORMED);
            }
            h->have_target = 1;
            (void)v;
            continue;
        }
        if (starts_tag(ls, le, "#EXT-X-MEDIA-SEQUENCE:")) {
            if (parse_u64(ls + 22, le, &seq) != 0) {
                return fail_h(h, MS_ERR_MALFORMED);
            }
            h->media_seq0 = seq;
            h->have_seq0 = 1;
            continue;
        }
        if (starts_tag(ls, le, "#EXT-X-ENDLIST")) {
            h->endlist = 1;
            continue;
        }
        if (starts_tag(ls, le, "#EXT-X-DISCONTINUITY") &&
            (ls + 20 == le || ls[20] == ':' || ls[20] == ' ' || ls[20] == '\t')) {
            /* 不把 DISCONTINUITY-SEQUENCE 当成分片不连续标记 */
            pend_disc = 1;
            continue;
        }
        if (starts_tag(ls, le, "#EXT-X-BYTERANGE:")) {
            if (parse_byterange(ls + 17, le, &br_len, &br_off) != 0) {
                return fail_h(h, MS_ERR_MALFORMED);
            }
            pend_br = 1;
            continue;
        }
        if (starts_tag(ls, le, "#EXT-X-MAP:")) {
            const char *vs, *ve;
            int q = 0;
            if (attr_find(ls + 11, le, "URI", &vs, &ve, &q) != 0) {
                return fail_h(h, MS_ERR_MALFORMED);
            }
            map_off = (uint32_t)((const uint8_t *)vs - h->pl_buf);
            map_len = (uint32_t)(ve - vs);
            map_boff = 0;
            map_blen = 0;
            if (attr_find(ls + 11, le, "BYTERANGE", &vs, &ve, &q) == 0) {
                if (parse_byterange(vs, ve, &map_blen, &map_boff) != 0) {
                    return fail_h(h, MS_ERR_MALFORMED);
                }
            }
            have_map = 1;
            continue;
        }
        if (starts_tag(ls, le, "#EXT-X-KEY:")) {
            const char *vs, *ve;
            int q = 0;
            if (attr_find(ls + 11, le, "METHOD", &vs, &ve, &q) != 0) {
                return fail_h(h, MS_ERR_MALFORMED);
            }
            if (!((ve - vs) == 4 && memcmp(vs, "NONE", 4) == 0)) {
                h->saw_key_bad = 1;
                h->saw_unsupported = 1;
            }
            continue;
        }
        if (starts_tag(ls, le, "#EXT-X-STREAM-INF") ||
            starts_tag(ls, le, "#EXT-X-I-FRAME-STREAM-INF") ||
            starts_tag(ls, le, "#EXT-X-MEDIA:") ||
            starts_tag(ls, le, "#EXT-X-SESSION-KEY") ||
            starts_tag(ls, le, "#EXT-X-SESSION-DATA")) {
            h->saw_master = 1;
            continue;
        }
        if (starts_tag(ls, le, "#EXT-X-PART") ||
            starts_tag(ls, le, "#EXT-X-PRELOAD-HINT") ||
            starts_tag(ls, le, "#EXT-X-SKIP") ||
            starts_tag(ls, le, "#EXT-X-SERVER-CONTROL") ||
            starts_tag(ls, le, "#EXT-X-RENDITION-REPORT")) {
            h->saw_llhls = 1;
            h->saw_unsupported = 1;
            continue;
        }
        if (starts_tag(ls, le, "#EXT-X-I-FRAMES-ONLY") ||
            starts_tag(ls, le, "#EXT-X-DEFINE") ||
            starts_tag(ls, le, "#EXT-X-KEYFORMAT") ||
            starts_tag(ls, le, "#EXT-X-DATERANGE")) {
            /* DATERANGE 可忽略；DEFINE/I-FRAMES 不支持 */
            if (!starts_tag(ls, le, "#EXT-X-DATERANGE")) {
                h->saw_unsupported = 1;
            }
            continue;
        }
        if (starts_tag(ls, le, "#EXTINF:")) {
            /* 时长：整数部分 + 至多 3 位小数，转毫秒（不用浮点库，保持 API18 libc 依赖不变） */
            const char *q = ls + 8;
            uint32_t ip = 0, fp = 0, fd = 0;
            while (q < le && *q >= '0' && *q <= '9' && ip < 100000000u) ip = ip * 10u + (uint32_t)(*q++ - '0');
            if (q < le && *q == '.') {
                q++;
                while (q < le && *q >= '0' && *q <= '9') { if (fd < 3) { fp = fp * 10u + (uint32_t)(*q - '0'); fd++; } q++; }
            }
            while (fd < 3) { fp *= 10u; fd++; }
            pend_dur_ms = ip > 4000000u ? 0xFFFFFFFFu : ip * 1000u + fp;
            h->saw_media = 1;
            /* 下一非标签行是 URI */
            continue;
        }
        if (starts_tag(ls, le, "#EXT")) {
            /* 其它标签忽略 */
            continue;
        }
        /* URI line */
        if (ls[0] == '#') {
            continue;
        }
        {
            hls_seg *sg;
            if (h->nsegs >= h->lim.max_segments) {
                return fail_h(h, MS_ERR_OVERFLOW);
            }
            if (h->nsegs >= h->segs_cap) {
                return fail_h(h, MS_ERR_STATE);
            }
            sg = &h->segs[h->nsegs];
            memset(sg, 0, sizeof(*sg));
            sg->seq = h->have_seq0 ? (h->media_seq0 + (uint64_t)h->nsegs) : seq;
            sg->uri_off = (uint32_t)((const uint8_t *)ls - h->pl_buf);
            sg->uri_len = (uint32_t)(le - ls);
            if (pend_br) {
                sg->byte_off = br_off;
                sg->byte_len = br_len;
                pend_br = 0;
            }
            sg->disc = pend_disc;
            pend_disc = 0;
            sg->dur_ms = pend_dur_ms;
            pend_dur_ms = 0;
            if (have_map) {
                sg->has_map = 1;
                sg->map_off = map_off;
                sg->map_len = map_len;
                sg->map_byte_off = map_boff;
                sg->map_byte_len = map_blen;
            }
            h->nsegs++;
            h->saw_media = 1;
            if (!h->have_seq0) {
                seq++;
            }
        }
    }
    if (!h->saw_extm3u) {
        return fail_h(h, MS_ERR_MALFORMED);
    }
    if (h->saw_key_bad || h->saw_llhls ||
        (h->saw_unsupported && !h->saw_media)) {
        return fail_h(h, MS_ERR_UNSUPPORTED);
    }
    if (h->saw_master && !h->saw_media) {
        return fail_h(h, MS_ERR_UNSUPPORTED);
    }
    if (h->saw_master && h->saw_media) {
        return fail_h(h, MS_ERR_UNSUPPORTED);
    }
    if (h->saw_unsupported) {
        return fail_h(h, MS_ERR_UNSUPPORTED);
    }
    if (h->nsegs == 0) {
        return fail_h(h, MS_ERR_MALFORMED);
    }
    if (!h->have_target) {
        return fail_h(h, MS_ERR_MALFORMED);
    }
    return MS_OK;
}

/* ---------------- object ---------------- */

static void close_fetch(ms_hls *h)
{
    if (h->fetch_open && h->has_fetch && h->fetch.cancel_fn != NULL) {
        h->in_cb = 1;
        h->fetch.cancel_fn(h->fetch.ctx, h->fetch_id);
        h->in_cb = 0;
    }
    h->fetch_open = 0;
}

ms_status ms_hls_create(const ms_allocator *alloc, const ms_hls_limits *limits,
                        ms_hls **out)
{
    ms_hls *h;
    ms_hls_limits lim;
    ms_status st;
    if (out == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    *out = NULL;
    st = apply_limits(limits, &lim);
    if (st != MS_OK) {
        return st;
    }
    if (alloc != NULL && (alloc->alloc_fn == NULL || alloc->free_fn == NULL)) {
        return MS_ERR_INVALID_ARG;
    }
    if (alloc != NULL) {
        h = (ms_hls *)alloc->alloc_fn(alloc->ctx, sizeof(*h));
    } else {
        h = (ms_hls *)malloc(sizeof(*h));
    }
    if (h == NULL) {
        return MS_ERR_NOMEM;
    }
    memset(h, 0, sizeof(*h));
    if (alloc != NULL) {
        h->alloc = *alloc;
        h->using_default = 0;
    } else {
        h->alloc.alloc_fn = sys_alloc;
        h->alloc.free_fn = sys_free;
        h->using_default = 1;
    }
    h->lim = lim;
    h->bytes_used = sizeof(*h);
    if (h->bytes_used > h->lim.max_bytes) {
        ms_hls_destroy(h);
        return MS_ERR_NOMEM;
    }
    h->segs = (hls_seg *)hls_alloc(h, sizeof(hls_seg) * lim.max_segments);
    h->pl_buf = (uint8_t *)hls_alloc(h, lim.max_playlist_bytes);
    h->chunk = (uint8_t *)hls_alloc(h, lim.read_chunk_bytes);
    h->uri_scratch = (char *)hls_alloc(h, lim.max_uri_bytes);
    h->base_uri = (char *)hls_alloc(h, lim.max_uri_bytes);
    if (h->segs == NULL || h->pl_buf == NULL || h->chunk == NULL ||
        h->uri_scratch == NULL || h->base_uri == NULL) {
        ms_hls_destroy(h);
        return MS_ERR_NOMEM;
    }
    h->segs_cap = lim.max_segments;
    h->pl_cap = lim.max_playlist_bytes;
    h->chunk_cap = lim.read_chunk_bytes;
    h->uri_scratch_cap = lim.max_uri_bytes;
    h->base_cap = lim.max_uri_bytes;
    *out = h;
    return MS_OK;
}

void ms_hls_destroy(ms_hls *h)
{
    if (h == NULL) {
        return;
    }
    close_fetch(h);
    hls_free(h, h->segs);
    hls_free(h, h->pl_buf);
    hls_free(h, h->chunk);
    hls_free(h, h->uri_scratch);
    hls_free(h, h->base_uri);
    hls_free(h, h->hold);
    if (h->using_default) {
        free(h);
    } else {
        h->alloc.free_fn(h->alloc.ctx, h);
    }
}

ms_status ms_hls_bind(ms_hls *h, ms_session *session, const ms_hls_fetch *fetch)
{
    if (h == NULL || session == NULL || fetch == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    if (h->in_cb) {
        return MS_ERR_STATE;
    }
    if (h->canceled) {
        return MS_ERR_CANCELED;
    }
    if (fetch->struct_size < sizeof(size_t) || fetch->request_fn == NULL ||
        fetch->read_fn == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    memset(&h->fetch, 0, sizeof(h->fetch));
    h->fetch.struct_size = sizeof(h->fetch);
    h->fetch.ctx = fetch->ctx;
    h->fetch.request_fn = fetch->request_fn;
    h->fetch.read_fn = fetch->read_fn;
    if (fetch->struct_size >= sizeof(*fetch)) {
        h->fetch.cancel_fn = fetch->cancel_fn;
    }
    h->has_fetch = 1;
    h->session = session;
    return MS_OK;
}

ms_status ms_hls_open(ms_hls *h, const char *playlist_uri)
{
    size_t n;
    if (h == NULL || playlist_uri == NULL || playlist_uri[0] == 0) {
        return MS_ERR_INVALID_ARG;
    }
    if (h->in_cb) {
        return MS_ERR_STATE;
    }
    if (h->canceled) {
        return MS_ERR_CANCELED;
    }
    if (!h->has_fetch || h->session == NULL) {
        return MS_ERR_STATE;
    }
    n = strlen(playlist_uri);
    if (n >= h->base_cap) {
        return MS_ERR_OVERFLOW;
    }
    close_fetch(h);
    memcpy(h->base_uri, playlist_uri, n + 1);
    h->pl_len = 0;
    h->pl_complete = 0;
    h->nsegs = 0;
    h->failed = 0;
    h->fail_st = MS_OK;
    h->eos_sent = 0;
    h->have_next = 0;
    h->init_done = 0;
    h->have_map = 0;
    h->hold_len = 0;
    h->hold_off = 0;
    h->pending_disc = 0;
    h->pending_eoi = 0;
    h->job = JOB_PLAYLIST;
    h->state = ST_REQ;
    h->retries_left = h->lim.max_retries;
    h->expect_len = 0;
    h->got_len = 0;
    return MS_OK;
}

ms_status ms_hls_cancel(ms_hls *h)
{
    if (h == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    if (h->in_cb) {
        return MS_ERR_STATE;
    }
    h->canceled = 1;
    close_fetch(h);
    return MS_OK;
}

static ms_status resolve_into(ms_hls *h, const char *raw, size_t raw_len,
                              char *out, size_t cap)
{
    char tmp[4096];
    if (raw_len >= sizeof(tmp)) {
        return MS_ERR_OVERFLOW;
    }
    memcpy(tmp, raw, raw_len);
    tmp[raw_len] = 0;
    return ms_hls_resolve_uri(h->base_uri, tmp, out, cap);
}

static hls_seg *find_seq(ms_hls *h, uint64_t seq)
{
    size_t i;
    for (i = 0; i < h->nsegs; i++) {
        if (h->segs[i].seq == seq) {
            return &h->segs[i];
        }
    }
    return NULL;
}

static hls_seg *first_available(ms_hls *h, uint64_t *seq)
{
    if (h->nsegs == 0) {
        return NULL;
    }
    if (!h->have_next) {
        *seq = h->segs[0].seq;
        return &h->segs[0];
    }
    if (find_seq(h, h->next_seq) != NULL) {
        *seq = h->next_seq;
        return find_seq(h, h->next_seq);
    }
    /* 窗口滑过或序号跳跃：从当前窗口不小于 next 的第一项开始 */
    {
        size_t i;
        for (i = 0; i < h->nsegs; i++) {
            if (h->segs[i].seq >= h->next_seq) {
                *seq = h->segs[i].seq;
                return &h->segs[i];
            }
        }
    }
    return NULL;
}

static ms_status emit_disc(ms_hls *h)
{
    ms_status st = ms_session_signal_discontinuity(h->session);
    if (st == MS_LIMIT) {
        h->pending_disc = 1;
        return MS_LIMIT;
    }
    h->pending_disc = 0;
    return st;
}

static ms_status start_request(ms_hls *h, int job, const char *uri,
                               int has_seq, uint64_t seq, int is_init,
                               uint64_t boff, uint64_t blen)
{
    ms_status st;
    ms_hls_request req;
    uint32_t id = 0;
    size_t ulen = strlen(uri);
    if (ulen >= sizeof(h->cur_uri)) {
        return fail_h(h, MS_ERR_OVERFLOW);
    }
    memcpy(h->cur_uri, uri, ulen + 1);
    memset(&req, 0, sizeof(req));
    req.struct_size = sizeof(req);
    req.uri = h->cur_uri;
    req.has_seq = has_seq;
    req.media_seq = seq;
    req.is_playlist = (job == JOB_PLAYLIST);
    req.is_init = is_init;
    req.byte_off = boff;
    req.byte_len = blen;
    h->cur_req = req;
    h->cur_req.uri = h->cur_uri;
    h->in_cb = 1;
    st = h->fetch.request_fn(h->fetch.ctx, &h->cur_req, &id);
    h->in_cb = 0;
    if (st == MS_AGAIN) {
        h->job = job;
        h->state = ST_REQ;
        return MS_AGAIN;
    }
    if (st != MS_OK) {
        if (h->retries_left > 0) {
            h->retries_left--;
            h->state = ST_REQ;
            return MS_AGAIN;
        }
        return st;
    }
    h->fetch_id = id;
    h->fetch_open = 1;
    h->job = job;
    h->state = ST_READ;
    h->got_len = 0;
    h->expect_len = blen;
    if (job == JOB_PLAYLIST) {
        h->pl_len = 0;
        h->pl_complete = 0;
    }
    h->hold_len = 0;
    h->hold_off = 0;
    return MS_OK;
}

static ms_status push_bytes(ms_hls *h, const uint8_t *data, size_t n,
                            size_t *used)
{
    return ms_session_push(h->session, data, n, used);
}

static ms_status finish_resource(ms_hls *h, int truncated)
{
    h->fetch_open = 0;
    if (h->job == JOB_PLAYLIST) {
        ms_status st;
        h->pl_complete = 1;
        st = parse_playlist(h);
        if (st != MS_OK) {
            return st;
        }
        /* 更新基址为清单自身，供相对 URI */
        {
            size_t n = strlen(h->cur_uri);
            if (n >= h->base_cap) {
                return fail_h(h, MS_ERR_OVERFLOW);
            }
            memcpy(h->base_uri, h->cur_uri, n + 1);
        }
        h->state = ST_READY;
        return MS_OK;
    }
    if (truncated) {
        ms_status st = emit_disc(h);
        if (st == MS_LIMIT) {
            return MS_LIMIT;
        }
        if (h->job == JOB_SEG && h->cur_req.has_seq) {
            h->next_seq = h->cur_req.media_seq + 1;
            h->have_next = 1;
        }
        h->state = ST_READY;
        return MS_OK;
    }
    if (h->job == JOB_INIT) {
        h->init_done = 1;
        h->have_map = 1;
        /* last_map_* 在发起 INIT 时已记录，此处不得清掉，否则会反复拉同一 MAP。 */
        h->state = ST_READY;
        return MS_OK;
    }
    if (h->job == JOB_SEG && h->cur_req.has_seq) {
        h->next_seq = h->cur_req.media_seq + 1;
        h->have_next = 1;
    }
    h->state = ST_READY;
    return MS_OK;
}

static int map_changed(const ms_hls *h, const hls_seg *sg)
{
    if (!sg->has_map) {
        return 0;
    }
    if (!h->init_done) {
        return 1;
    }
    if (sg->map_len != h->last_map_len ||
        sg->map_byte_off != h->last_map_offb ||
        sg->map_byte_len != h->last_map_lenb) {
        return 1;
    }
    if (sg->map_len > 0 &&
        memcmp(h->pl_buf + sg->map_off, h->pl_buf + h->last_map_off,
               sg->map_len) != 0) {
        return 1;
    }
    return 0;
}

static ms_status begin_next_job(ms_hls *h)
{
    uint64_t seq = 0;
    hls_seg *sg;
    ms_status st;

    if (h->pending_disc) {
        st = emit_disc(h);
        if (st != MS_OK) {
            return st;
        }
    }
    if (h->pending_eoi) {
        st = ms_session_end_of_input(h->session);
        if (st == MS_LIMIT) {
            return MS_LIMIT;
        }
        if (st != MS_OK && st != MS_AGAIN && st != MS_ERR_NOT_SEEKABLE) {
            return fail_h(h, st);
        }
        h->pending_eoi = 0;
        h->eos_sent = 1;
        h->state = ST_DONE;
        return MS_EOS;
    }

    sg = first_available(h, &seq);
    if (sg == NULL) {
        if (h->endlist) {
            st = ms_session_end_of_input(h->session);
            if (st == MS_LIMIT) {
                h->pending_eoi = 1;
                return MS_LIMIT;
            }
            if (st != MS_OK && st != MS_AGAIN && st != MS_ERR_NOT_SEEKABLE) {
                return fail_h(h, st);
            }
            h->eos_sent = 1;
            h->state = ST_DONE;
            return MS_EOS;
        }
        /* 直播：刷新清单 */
        h->retries_left = h->lim.max_retries;
        return start_request(h, JOB_PLAYLIST, h->base_uri, 0, 0, 0, 0, 0);
    }
    if (h->have_next && seq != h->next_seq) {
        /* 跳跃或窗口滑过未取到的片 */
        st = emit_disc(h);
        if (st == MS_LIMIT) {
            return MS_LIMIT;
        }
    }
    if (sg->disc && !(h->have_next && seq != h->next_seq)) {
        st = emit_disc(h);
        if (st == MS_LIMIT) {
            return MS_LIMIT;
        }
    } else if (sg->disc) {
        /* 已因跳跃发过一次 disc */
    }
    if (sg->has_map && map_changed(h, sg)) {
        st = resolve_into(h, (const char *)(h->pl_buf + sg->map_off),
                          sg->map_len, h->uri_scratch, h->uri_scratch_cap);
        if (st != MS_OK) {
            return fail_h(h, st);
        }
        h->last_map_off = sg->map_off;
        h->last_map_len = sg->map_len;
        h->last_map_offb = sg->map_byte_off;
        h->last_map_lenb = sg->map_byte_len;
        h->retries_left = h->lim.max_retries;
        return start_request(h, JOB_INIT, h->uri_scratch, 0, 0, 1,
                             sg->map_byte_off, sg->map_byte_len);
    }
    st = resolve_into(h, (const char *)(h->pl_buf + sg->uri_off),
                      sg->uri_len, h->uri_scratch, h->uri_scratch_cap);
    if (st != MS_OK) {
        return fail_h(h, st);
    }
    h->retries_left = h->lim.max_retries;
    return start_request(h, JOB_SEG, h->uri_scratch, 1, sg->seq, 0,
                         sg->byte_off, sg->byte_len);
}

static ms_status do_read(ms_hls *h)
{
    size_t n = 0;
    size_t used = 0;
    ms_status st;
    size_t want = h->chunk_cap;

    if (h->hold_len > h->hold_off) {
        size_t left = h->hold_len - h->hold_off;
        used = 0;
        st = push_bytes(h, h->hold + h->hold_off, left, &used);
        h->hold_off += used;
        if (h->hold_off >= h->hold_len) {
            h->hold_len = 0;
            h->hold_off = 0;
        }
        if (st == MS_LIMIT) {
            return MS_LIMIT;
        }
        if (st != MS_OK && st != MS_AGAIN) {
            return fail_h(h, st);
        }
        return MS_OK;
    }

    if (h->expect_len > 0 && h->got_len >= h->expect_len) {
        return finish_resource(h, 0);
    }
    if (h->expect_len > 0 && (h->expect_len - h->got_len) < (uint64_t)want) {
        want = (size_t)(h->expect_len - h->got_len);
    }
    h->in_cb = 1;
    st = h->fetch.read_fn(h->fetch.ctx, h->fetch_id, h->chunk, want, &n);
    h->in_cb = 0;
    if (st == MS_AGAIN) {
        return MS_AGAIN;
    }
    if (st == MS_ERR_CANCELED) {
        h->canceled = 1;
        return MS_ERR_CANCELED;
    }
    if (st != MS_OK) {
        close_fetch(h);
        if (h->retries_left > 0) {
            h->retries_left--;
            h->state = ST_REQ;
            return MS_AGAIN;
        }
        if (h->job == JOB_PLAYLIST || h->job == JOB_INIT) {
            return fail_h(h, st);
        }
        /* 分片失败：不拼接，当作缺失 */
        st = emit_disc(h);
        if (h->cur_req.has_seq) {
            h->next_seq = h->cur_req.media_seq + 1;
            h->have_next = 1;
        }
        h->state = ST_READY;
        return (st == MS_OK || st == MS_LIMIT) ? st : fail_h(h, st);
    }
    if (n == 0) {
        int trunc = 0;
        if (h->expect_len > 0 && h->got_len < h->expect_len) {
            trunc = 1;
        }
        if (h->job == JOB_PLAYLIST && h->pl_len == 0) {
            return fail_h(h, MS_ERR_MALFORMED);
        }
        return finish_resource(h, trunc);
    }
    h->got_len += n;
    if (h->job == JOB_PLAYLIST) {
        if (h->pl_len + n > h->pl_cap) {
            return fail_h(h, MS_ERR_OVERFLOW);
        }
        memcpy(h->pl_buf + h->pl_len, h->chunk, n);
        h->pl_len += n;
        return MS_OK;
    }
    used = 0;
    st = push_bytes(h, h->chunk, n, &used);
    if (used < n) {
        size_t left = n - used;
        if (h->hold != NULL && h->hold_cap < left) {
            hls_free(h, h->hold);
            h->hold = NULL;
            h->hold_cap = 0;
        }
        if (h->hold == NULL) {
            h->hold = (uint8_t *)hls_alloc(h, left);
            if (h->hold == NULL) {
                return fail_h(h, MS_ERR_NOMEM);
            }
            h->hold_cap = left;
        }
        memcpy(h->hold, h->chunk + used, left);
        h->hold_len = left;
        h->hold_off = 0;
    }
    if (st == MS_LIMIT) {
        return MS_LIMIT;
    }
    if (st != MS_OK && st != MS_AGAIN) {
        return fail_h(h, st);
    }
    return MS_OK;
}

ms_status ms_hls_drive(ms_hls *h)
{
    ms_status st;
    if (h == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    if (h->in_cb) {
        return MS_ERR_STATE;
    }
    if (h->canceled) {
        return MS_ERR_CANCELED;
    }
    if (h->failed) {
        return h->fail_st;
    }
    if (!h->has_fetch || h->session == NULL) {
        return MS_ERR_STATE;
    }
    if (h->state == ST_DONE && h->eos_sent) {
        return MS_EOS;
    }
    if (h->state == ST_IDLE) {
        return MS_ERR_STATE;
    }
    if (h->state == ST_REQ) {
        if (h->job == JOB_PLAYLIST) {
            st = start_request(h, JOB_PLAYLIST,
                               h->fetch_open ? h->cur_uri : h->base_uri,
                               0, 0, 0, 0, 0);
        } else {
            st = start_request(h, h->job, h->cur_uri, h->cur_req.has_seq,
                               h->cur_req.media_seq, h->cur_req.is_init,
                               h->cur_req.byte_off, h->cur_req.byte_len);
        }
        if (st == MS_AGAIN || st == MS_OK) {
            return st;
        }
        if (st == MS_LIMIT) {
            return st;
        }
        /* 分片请求耗尽重试：跳过该片并发不连续，不把整路判死。 */
        if (h->job == JOB_SEG) {
            ms_status ds = emit_disc(h);
            if (h->cur_req.has_seq) {
                h->next_seq = h->cur_req.media_seq + 1;
                h->have_next = 1;
            }
            h->state = ST_READY;
            return (ds == MS_OK || ds == MS_LIMIT) ? ds : fail_h(h, ds);
        }
        return fail_h(h, st);
    }
    if (h->state == ST_READ) {
        return do_read(h);
    }
    if (h->state == ST_READY) {
        return begin_next_job(h);
    }
    return MS_ERR_STATE;
}

/* ---------------- mem fetch ---------------- */

void ms_hls_mem_fetch_init(ms_hls_mem_fetch *m, const ms_hls_mem_item *items,
                           size_t nitems)
{
    if (m == NULL) {
        return;
    }
    memset(m, 0, sizeof(*m));
    m->struct_size = sizeof(*m);
    m->items = items;
    m->nitems = nitems;
    m->fail_after = -1;
    m->fail_st = MS_ERR_UNKNOWN;
}

static ms_status mem_request(void *ctx, const ms_hls_request *req,
                             uint32_t *out_id)
{
    ms_hls_mem_fetch *m = (ms_hls_mem_fetch *)ctx;
    size_t i;
    if (out_id != NULL) {
        *out_id = 0;
    }
    if (m == NULL || req == NULL || req->uri == NULL || out_id == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    if (m->canceled) {
        return MS_ERR_CANCELED;
    }
    if (m->again_left > 0) {
        m->again_left--;
        return MS_AGAIN;
    }
    if (m->fail_after >= 0) {
        if (m->fail_after == 0) {
            return (m->fail_st != MS_OK) ? m->fail_st : MS_ERR_UNKNOWN;
        }
        m->fail_after--;
    }
    for (i = 0; i < m->nitems; i++) {
        if (m->items[i].uri != NULL && strcmp(m->items[i].uri, req->uri) == 0) {
            m->live_used = 1;
            m->live_id++;
            if (m->live_id == 0) {
                m->live_id = 1;
            }
            m->live_idx = i;
            m->live_off = (size_t)req->byte_off;
            if (req->byte_len > 0) {
                m->live_end = m->live_off + (size_t)req->byte_len;
            } else {
                m->live_end = m->items[i].size;
            }
            if (m->live_end > m->items[i].size) {
                m->live_end = m->items[i].size;
            }
            *out_id = m->live_id;
            m->nrequests++;
            return MS_OK;
        }
    }
    return MS_ERR_UNKNOWN;
}

static ms_status mem_read(void *ctx, uint32_t id, uint8_t *buf, size_t len,
                          size_t *out_n)
{
    ms_hls_mem_fetch *m = (ms_hls_mem_fetch *)ctx;
    size_t left;
    size_t n;
    if (out_n != NULL) {
        *out_n = 0;
    }
    if (m == NULL || out_n == NULL || (buf == NULL && len != 0)) {
        return MS_ERR_INVALID_ARG;
    }
    if (m->canceled) {
        return MS_ERR_CANCELED;
    }
    if (!m->live_used || id != m->live_id) {
        return MS_ERR_STATE;
    }
    if (m->again_left > 0) {
        m->again_left--;
        return MS_AGAIN;
    }
    if (m->fail_after >= 0) {
        if (m->fail_after == 0) {
            return (m->fail_st != MS_OK) ? m->fail_st : MS_ERR_UNKNOWN;
        }
        m->fail_after--;
    }
    if (m->live_off >= m->live_end) {
        return MS_OK;
    }
    left = m->live_end - m->live_off;
    n = (left < len) ? left : len;
    if (m->max_read > 0 && n > m->max_read) {
        n = m->max_read;
    }
    if (n > 0 && m->items[m->live_idx].data != NULL) {
        memcpy(buf, m->items[m->live_idx].data + m->live_off, n);
    }
    m->live_off += n;
    *out_n = n;
    m->nreads++;
    return MS_OK;
}

static void mem_cancel(void *ctx, uint32_t id)
{
    ms_hls_mem_fetch *m = (ms_hls_mem_fetch *)ctx;
    (void)id;
    if (m != NULL) {
        m->canceled = 1;
        m->live_used = 0;
    }
}

void ms_hls_mem_fetch_bind(ms_hls_mem_fetch *m, ms_hls_fetch *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->struct_size = sizeof(*out);
    out->ctx = m;
    out->request_fn = mem_request;
    out->read_fn = mem_read;
    out->cancel_fn = mem_cancel;
}

/* ---------------- file fetch ---------------- */

void ms_hls_file_fetch_init(ms_hls_file_fetch *f)
{
    if (f == NULL) {
        return;
    }
    memset(f, 0, sizeof(*f));
    f->struct_size = sizeof(*f);
    f->live_fd = -1;
}

static const char *file_path_from_uri(const char *uri)
{
    if (uri == NULL) {
        return NULL;
    }
    if (strncmp(uri, "file://", 7) == 0) {
        const char *p = uri + 7;
        if (p[0] == '/' && p[1] == '/') {
            /* file://host/path — 仅接受空 host */
            p += 2;
        }
        return p;
    }
    return uri;
}

static ms_status file_request(void *ctx, const ms_hls_request *req,
                              uint32_t *out_id)
{
    ms_hls_file_fetch *f = (ms_hls_file_fetch *)ctx;
    const char *path;
    int fd;
    off_t end;
    if (out_id != NULL) {
        *out_id = 0;
    }
    if (f == NULL || req == NULL || req->uri == NULL || out_id == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    if (f->canceled) {
        return MS_ERR_CANCELED;
    }
    path = file_path_from_uri(req->uri);
    if (path == NULL || path[0] != '/') {
        return MS_ERR_INVALID_ARG;
    }
    if (f->live_fd >= 0) {
        close(f->live_fd);
        f->live_fd = -1;
    }
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return MS_ERR_UNKNOWN;
    }
    end = lseek(fd, 0, SEEK_END);
    if (end < 0) {
        close(fd);
        return MS_ERR_UNKNOWN;
    }
    f->live_fd = fd;
    f->live_off = req->byte_off;
    if (req->byte_len > 0) {
        f->live_end = req->byte_off + req->byte_len;
    } else {
        f->live_end = (uint64_t)end;
    }
    if (f->live_end > (uint64_t)end) {
        f->live_end = (uint64_t)end;
    }
    f->live_id++;
    if (f->live_id == 0) {
        f->live_id = 1;
    }
    f->live_used = 1;
    *out_id = f->live_id;
    f->nrequests++;
    return MS_OK;
}

static ms_status file_read(void *ctx, uint32_t id, uint8_t *buf, size_t len,
                           size_t *out_n)
{
    ms_hls_file_fetch *f = (ms_hls_file_fetch *)ctx;
    uint64_t left;
    size_t nwant;
    ssize_t n;
    if (out_n != NULL) {
        *out_n = 0;
    }
    if (f == NULL || out_n == NULL || (buf == NULL && len != 0)) {
        return MS_ERR_INVALID_ARG;
    }
    if (f->canceled) {
        return MS_ERR_CANCELED;
    }
    if (!f->live_used || id != f->live_id || f->live_fd < 0) {
        return MS_ERR_STATE;
    }
    if (f->live_off >= f->live_end) {
        if (f->live_fd >= 0) {
            close(f->live_fd);
            f->live_fd = -1;
        }
        return MS_OK;
    }
    left = f->live_end - f->live_off;
    nwant = (left < (uint64_t)len) ? (size_t)left : len;
    if (f->max_read > 0 && nwant > f->max_read) {
        nwant = f->max_read;
    }
    n = pread(f->live_fd, buf, nwant, (off_t)f->live_off);
    if (n < 0) {
        return MS_ERR_UNKNOWN;
    }
    f->live_off += (uint64_t)n;
    *out_n = (size_t)n;
    return MS_OK;
}

static void file_cancel(void *ctx, uint32_t id)
{
    ms_hls_file_fetch *f = (ms_hls_file_fetch *)ctx;
    (void)id;
    if (f == NULL) {
        return;
    }
    f->canceled = 1;
    if (f->live_fd >= 0) {
        close(f->live_fd);
        f->live_fd = -1;
    }
    f->live_used = 0;
}

void ms_hls_file_fetch_bind(ms_hls_file_fetch *f, ms_hls_fetch *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->struct_size = sizeof(*out);
    out->ctx = f;
    out->request_fn = file_request;
    out->read_fn = file_read;
    out->cancel_fn = file_cancel;
}

/* ---- 内部（不进公共头/导出表）：预加载器前瞻 ----
 * 从「下一个要取的分片」起按播放顺序列出后续请求（MAP 变化时先列初始化段），
 * 不改调度状态；URI 已按当前清单基址解析。返回列出的条目数。
 * by: Long <mycjzb@gmail.com> */
#include "ms_hls_internal.h"

size_t ms_hls_upcoming(ms_hls *h, size_t max_items, ms_hls_upcoming_fn fn, void *ctx)
{
    uint64_t seq = 0;
    hls_seg *sg;
    size_t i, start, n = 0;
    uint32_t moff, mlen;
    uint64_t mboff, mblen;
    int have_map;
    char *uri;
    if (h == NULL || fn == NULL || h->failed || h->nsegs == 0) return 0;
    sg = first_available(h, &seq);
    if (sg == NULL) return 0;
    start = (size_t)(sg - h->segs);
    uri = (char *)malloc(h->uri_scratch_cap ? h->uri_scratch_cap : HLS_DEF_URI);
    if (uri == NULL) return 0;
    moff = h->last_map_off; mlen = h->last_map_len; mboff = h->last_map_offb; mblen = h->last_map_lenb;
    have_map = h->have_map;
    for (i = start; i < h->nsegs && n < max_items; i++) {
        ms_hls_request rq;
        hls_seg *s = &h->segs[i];
        size_t cap = h->uri_scratch_cap ? h->uri_scratch_cap : HLS_DEF_URI;
        if (s->has_map && (!have_map || s->map_off != moff || s->map_len != mlen ||
                           s->map_byte_off != mboff || s->map_byte_len != mblen)) {
            if (resolve_into(h, (const char *)(h->pl_buf + s->map_off), s->map_len, uri, cap) != MS_OK) break;
            memset(&rq, 0, sizeof(rq));
            rq.struct_size = sizeof(rq);
            rq.uri = uri;
            rq.is_init = 1;
            rq.byte_off = s->map_byte_off;
            rq.byte_len = s->map_byte_len;
            fn(ctx, &rq, 0);
            n++;
            have_map = 1;
            moff = s->map_off; mlen = s->map_len; mboff = s->map_byte_off; mblen = s->map_byte_len;
        }
        if (resolve_into(h, (const char *)(h->pl_buf + s->uri_off), s->uri_len, uri, cap) != MS_OK) break;
        memset(&rq, 0, sizeof(rq));
        rq.struct_size = sizeof(rq);
        rq.uri = uri;
        rq.has_seq = 1;
        rq.media_seq = s->seq;
        rq.byte_off = s->byte_off;
        rq.byte_len = s->byte_len;
        fn(ctx, &rq, s->dur_ms);
        n++;
    }
    free(uri);
    return n;
}

int ms_hls_is_master(const ms_hls *h)
{
    return h != NULL && h->saw_master && !h->saw_media;
}
