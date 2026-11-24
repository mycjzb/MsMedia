/*
 * 可选 HTTP/1.1 客户端：有界缓冲、重定向、Range、超时与取消。
 * HTTPS 走 ms_tls（OpenSSL）。不链入解析核心。
 * by: Long <mycjzb@gmail.com>
 */
#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "msmedia/ms_net.h"
#include "ms_tls.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define NET_DEF_URL      4096u
#define NET_DEF_HDR      (16u * 1024u)
#define NET_DEF_BUF      (64u * 1024u)
#define NET_DEF_REDIR    5
#define NET_DEF_CONN_MS  5000
#define NET_DEF_IO_MS    15000
#define NET_ABS_URL      4096u
#define NET_ABS_HDR      (64u * 1024u)
#define NET_ABS_BUF      (1024u * 1024u)
#define NET_VISIT_MAX    8
#define NET_PUMP_SLICE   20

enum {
    PH_IDLE = 0,
    PH_RESOLVE,
    PH_CONNECT,
    PH_TLS,
    PH_SEND,
    PH_HDR,
    PH_BODY,
    PH_DONE,
    PH_FAIL
};

enum { BM_CL = 0, BM_CHUNK, BM_EOF };

enum {
    CH_SIZE = 0,
    CH_DATA,
    CH_CRLF,
    CH_TRAIL
};

struct ms_net {
    ms_allocator alloc;
    int using_default;
    ms_net_limits lim;
    char *ca_file;
    ms_tls_ctx *tls_ctx;
    int have_tls_ctx;

    ms_net_log_fn log_fn;
    void *log_ctx;

    pthread_mutex_t mu;
    pthread_cond_t cv;
    pthread_t th;
    int have_thread;
    int stop_thread;

    int job_live;
    uint32_t job_id;
    int canceled;
    ms_net_err err;
    int http_status;
    int saw_eof;

    char *url;
    size_t url_cap;
    char *path;
    size_t path_cap;
    char host[256];
    char portstr[16];
    int is_https;
    uint64_t byte_off;
    uint64_t byte_len;
    int redirects_left;
    int retries_left;
    int64_t deadline_ms;
    char visit[NET_VISIT_MAX][NET_ABS_URL];
    int nvisit;

    int fd;
    ms_tls_conn *tls;
    struct addrinfo *ai;
    struct addrinfo *ai_cur;
    int phase;

    char *snd;
    size_t snd_cap;
    size_t snd_len;
    size_t snd_off;
    char *hdr;
    size_t hdr_cap;
    size_t hdr_len;
    uint8_t *buf;
    size_t buf_cap;
    size_t buf_r;
    size_t buf_w;

    int body_mode;
    uint64_t cl_left;
    int cl_known;
    int chunk_st;
    uint64_t chunk_left;
    char chline[64];
    size_t chline_len;
};

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

static void *nalloc(ms_net *n, size_t sz)
{
    return n->alloc.alloc_fn(n->alloc.ctx, sz);
}

static void nfree(ms_net *n, void *p)
{
    if (p != NULL) {
        n->alloc.free_fn(n->alloc.ctx, p);
    }
}

static int staging_avail(const ms_net *n);

static int64_t now_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* 日志不得包含完整 URL、query 或 userinfo。 */
static void net_log(ms_net *n, int level, const char *fmt, ...)
{
    char msg[192];
    va_list ap;
    if (n == NULL || n->log_fn == NULL || fmt == NULL) {
        return;
    }
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    n->log_fn(n->log_ctx, level, msg);
}

static void block_sigpipe(void)
{
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGPIPE);
    pthread_sigmask(SIG_BLOCK, &set, NULL);
}

static ms_status map_out(ms_net_err e)
{
    switch (e) {
    case MS_NET_OK:
        return MS_OK;
    case MS_NET_AGAIN:
        return MS_AGAIN;
    case MS_NET_ERR_CANCELED:
        return MS_ERR_CANCELED;
    case MS_NET_ERR_NOMEM:
        return MS_ERR_NOMEM;
    case MS_NET_ERR_STATE:
        return MS_ERR_STATE;
    case MS_NET_ERR_INVALID_ARG:
        return MS_ERR_INVALID_ARG;
    case MS_NET_ERR_UNSUPPORTED:
        return MS_ERR_UNSUPPORTED;
    case MS_NET_ERR_MALFORMED:
        return MS_ERR_MALFORMED;
    case MS_NET_ERR_OVERFLOW:
        return MS_ERR_OVERFLOW;
    default:
        return MS_ERR_UNKNOWN;
    }
}

static void fail_job(ms_net *n, ms_net_err e)
{
    n->err = e;
    n->phase = PH_FAIL;
    n->saw_eof = 1;
}

static int ci_eq(const char *a, const char *b, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        char ca = a[i];
        char cb = b[i];
        if (ca >= 'A' && ca <= 'Z') {
            ca = (char)(ca - 'A' + 'a');
        }
        if (cb >= 'A' && cb <= 'Z') {
            cb = (char)(cb - 'A' + 'a');
        }
        if (ca != cb) {
            return 0;
        }
    }
    return 1;
}

static int has_ctl(const char *s)
{
    for (; s != NULL && *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c < 0x20 || c == 0x7f) {
            return 1;
        }
    }
    return 0;
}

static int parse_url(const char *url, int *https, char *host, size_t host_cap,
                     char *port, size_t port_cap, char *path, size_t path_cap)
{
    const char *p;
    const char *h;
    const char *slash;
    const char *col;
    size_t hl;
    int is_https;
    if (url == NULL || https == NULL || host == NULL || port == NULL ||
        path == NULL) {
        return -1;
    }
    if (has_ctl(url)) {
        return -1;
    }
    if (strncmp(url, "http://", 7) == 0) {
        is_https = 0;
        p = url + 7;
    } else if (strncmp(url, "https://", 8) == 0) {
        is_https = 1;
        p = url + 8;
    } else {
        return -1;
    }
    if (strchr(p, '@') != NULL) {
        /* 本子集不接受 userinfo，避免把口令写入头或日志。 */
        return -1;
    }
    if (p[0] == '[') {
        const char *rb = strchr(p, ']');
        if (rb == NULL || rb == p + 1) {
            return -1;
        }
        hl = (size_t)(rb - p - 1);
        if (hl == 0 || hl + 1 >= host_cap) {
            return -1;
        }
        memcpy(host, p + 1, hl);
        host[hl] = 0;
        p = rb + 1;
        if (*p == ':') {
            col = p + 1;
            slash = strpbrk(col, "/?#");
            if (slash == NULL) {
                slash = col + strlen(col);
            }
            if ((size_t)(slash - col) >= port_cap || slash == col) {
                return -1;
            }
            memcpy(port, col, (size_t)(slash - col));
            port[slash - col] = 0;
            p = slash;
        } else {
            snprintf(port, port_cap, "%s", is_https ? "443" : "80");
        }
    } else {
        slash = strpbrk(p, "/?#");
        h = p;
        if (slash == NULL) {
            slash = p + strlen(p);
        }
        col = memchr(h, ':', (size_t)(slash - h));
        if (col != NULL) {
            hl = (size_t)(col - h);
            if (hl == 0 || hl + 1 >= host_cap) {
                return -1;
            }
            memcpy(host, h, hl);
            host[hl] = 0;
            if ((size_t)(slash - col - 1) >= port_cap || slash == col + 1) {
                return -1;
            }
            memcpy(port, col + 1, (size_t)(slash - col - 1));
            port[slash - col - 1] = 0;
        } else {
            hl = (size_t)(slash - h);
            if (hl == 0 || hl + 1 >= host_cap) {
                return -1;
            }
            memcpy(host, h, hl);
            host[hl] = 0;
            snprintf(port, port_cap, "%s", is_https ? "443" : "80");
        }
        p = slash;
    }
    {
        const char *frag = strchr(p, '#');
        size_t pl;
        if (*p == 0 || *p == '#') {
            snprintf(path, path_cap, "/");
        } else if (*p == '?') {
            pl = frag ? (size_t)(frag - p) : strlen(p);
            if (pl + 2 >= path_cap) {
                return -1;
            }
            path[0] = '/';
            memcpy(path + 1, p, pl);
            path[pl + 1] = 0;
        } else {
            pl = frag ? (size_t)(frag - p) : strlen(p);
            if (pl + 1 >= path_cap) {
                return -1;
            }
            memcpy(path, p, pl);
            path[pl] = 0;
        }
    }
    *https = is_https;
    return 0;
}

static int default_port(int https, const char *port)
{
    return (https && strcmp(port, "443") == 0) ||
           (!https && strcmp(port, "80") == 0);
}

void ms_net_limits_default(ms_net_limits *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->struct_size = sizeof(*out);
    out->max_url_bytes = NET_DEF_URL;
    out->max_header_bytes = NET_DEF_HDR;
    out->max_buffer_bytes = NET_DEF_BUF;
    out->max_redirects = NET_DEF_REDIR;
    out->connect_timeout_ms = NET_DEF_CONN_MS;
    out->io_timeout_ms = NET_DEF_IO_MS;
    out->max_retries = 0;
}

static ms_status apply_lim(const ms_net_limits *in, ms_net_limits *out)
{
    ms_net_limits d;
    ms_net_limits_default(&d);
    if (in != NULL && in->struct_size >= sizeof(size_t) &&
        in->struct_size <= sizeof(*in)) {
        memset(out, 0, sizeof(*out));
        memcpy(out, in, in->struct_size);
        out->struct_size = sizeof(*out);
    } else if (in == NULL) {
        *out = d;
    } else {
        return MS_ERR_INVALID_ARG;
    }
    if (out->max_url_bytes == 0) {
        out->max_url_bytes = d.max_url_bytes;
    }
    if (out->max_header_bytes == 0) {
        out->max_header_bytes = d.max_header_bytes;
    }
    if (out->max_buffer_bytes == 0) {
        out->max_buffer_bytes = d.max_buffer_bytes;
    }
    if (out->max_redirects < 0) {
        out->max_redirects = d.max_redirects;
    }
    if (out->connect_timeout_ms == 0) {
        out->connect_timeout_ms = d.connect_timeout_ms;
    }
    if (out->io_timeout_ms == 0) {
        out->io_timeout_ms = d.io_timeout_ms;
    }
    if (out->max_retries < 0) {
        out->max_retries = d.max_retries;
    }
    if (out->max_url_bytes > NET_ABS_URL ||
        out->max_header_bytes > NET_ABS_HDR ||
        out->max_buffer_bytes > NET_ABS_BUF ||
        out->max_redirects > 16 ||
        out->connect_timeout_ms > 120000 ||
        out->io_timeout_ms > 120000 ||
        out->max_retries > 4) {
        return MS_ERR_INVALID_ARG;
    }
    return MS_OK;
}

static void close_conn(ms_net *n)
{
    if (n->tls != NULL) {
        ms_tls_conn_free(n->tls);
        n->tls = NULL;
    }
    if (n->fd >= 0) {
        close(n->fd);
        n->fd = -1;
    }
    if (n->ai != NULL) {
        freeaddrinfo(n->ai);
        n->ai = NULL;
        n->ai_cur = NULL;
    }
}

static void reset_job_io(ms_net *n)
{
    close_conn(n);
    n->snd_len = 0;
    n->snd_off = 0;
    n->hdr_len = 0;
    n->buf_r = 0;
    n->buf_w = 0;
    n->body_mode = BM_CL;
    n->cl_left = 0;
    n->cl_known = 0;
    n->chunk_st = CH_SIZE;
    n->chunk_left = 0;
    n->chline_len = 0;
    n->saw_eof = 0;
}

static int set_nonblock(int fd)
{
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0) {
        return -1;
    }
    return fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

static int io_wait(int fd, int want_write, int timeout_ms)
{
    struct pollfd p;
    int r;
    if (fd < 0) {
        return -1;
    }
    p.fd = fd;
    p.events = want_write ? POLLOUT : POLLIN;
    p.revents = 0;
    r = poll(&p, 1, timeout_ms < 0 ? 0 : timeout_ms);
    if (r < 0) {
        if (errno == EINTR) {
            return 0;
        }
        return -1;
    }
    if (r == 0) {
        return 0;
    }
    if (p.revents & (POLLERR | POLLHUP | POLLNVAL)) {
        if (!want_write && (p.revents & POLLIN)) {
            return 1;
        }
        if (want_write && (p.revents & POLLOUT) && !(p.revents & POLLNVAL)) {
            return 1;
        }
        if (p.revents & (POLLERR | POLLNVAL)) {
            return -1;
        }
    }
    return 1;
}

static int remain_ms(const ms_net *n)
{
    int64_t r = n->deadline_ms - now_ms();
    if (r <= 0) {
        return 0;
    }
    if (r > 100000) {
        return 100000;
    }
    return (int)r;
}

static ms_status raw_write(ms_net *n, const uint8_t *p, size_t len, size_t *used)
{
    ssize_t r;
    *used = 0;
    if (len == 0) {
        return MS_OK;
    }
    r = send(n->fd, p, len, MSG_NOSIGNAL);
    if (r > 0) {
        *used = (size_t)r;
        return MS_OK;
    }
    if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
        return MS_AGAIN;
    }
    return MS_ERR_UNKNOWN;
}

static ms_status raw_read(ms_net *n, uint8_t *p, size_t len, size_t *used)
{
    ssize_t r;
    *used = 0;
    if (len == 0) {
        return MS_OK;
    }
    r = recv(n->fd, p, len, 0);
    if (r > 0) {
        *used = (size_t)r;
        return MS_OK;
    }
    if (r == 0) {
        return MS_OK;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
        return MS_AGAIN;
    }
    return MS_ERR_UNKNOWN;
}

static ms_status conn_write(ms_net *n, const uint8_t *p, size_t len, size_t *used)
{
    if (n->is_https) {
        if (n->tls == NULL) {
            return MS_ERR_STATE;
        }
        return ms_tls_write(n->tls, p, len, used);
    }
    return raw_write(n, p, len, used);
}

static ms_status conn_read(ms_net *n, uint8_t *p, size_t len, size_t *used)
{
    if (n->is_https) {
        if (n->tls == NULL) {
            return MS_ERR_STATE;
        }
        return ms_tls_read(n->tls, p, len, used);
    }
    return raw_read(n, p, len, used);
}

static int wait_want(ms_net *n)
{
    int ww = 0;
    if (n->is_https && n->tls != NULL) {
        ww = ms_tls_want_write(n->tls);
    }
    return ww;
}

static int ensure_tls_ctx(ms_net *n)
{
    ms_status st;
    if (n->have_tls_ctx) {
        return 0;
    }
    st = ms_tls_ctx_new(n->ca_file, &n->tls_ctx);
    if (st != MS_OK) {
        return -1;
    }
    n->have_tls_ctx = 1;
    return 0;
}

static int hdr_ci_find(const char *hdr, size_t n, const char *name,
                       const char **val, size_t *vlen)
{
    size_t nl = strlen(name);
    size_t i = 0;
    while (i < n) {
        const char *line = hdr + i;
        const char *nlpos = memchr(line, '\n', n - i);
        size_t ll = nlpos ? (size_t)(nlpos - line) : (n - i);
        const char *col;
        size_t next = i + ll + (nlpos ? 1 : 0);
        if (ll > 0 && line[ll - 1] == '\r') {
            ll--;
        }
        col = memchr(line, ':', ll);
        if (col != NULL && (size_t)(col - line) == nl && ci_eq(line, name, nl)) {
            const char *v = col + 1;
            size_t vl;
            while (v < line + ll && (*v == ' ' || *v == '\t')) {
                v++;
            }
            vl = (size_t)((line + ll) - v);
            *val = v;
            *vlen = vl;
            return 1;
        }
        i = next;
    }
    return 0;
}

static int visit_add(ms_net *n, const char *url)
{
    int i;
    size_t L = strlen(url);
    if (L >= NET_ABS_URL) {
        return -1;
    }
    for (i = 0; i < n->nvisit; i++) {
        if (strcmp(n->visit[i], url) == 0) {
            return -1;
        }
    }
    if (n->nvisit >= NET_VISIT_MAX) {
        return -1;
    }
    memcpy(n->visit[n->nvisit], url, L + 1);
    n->nvisit++;
    return 0;
}

static int build_request(ms_net *n)
{
    int nw;
    char range[80];
    char hosthdr[300];
    range[0] = 0;
    if (n->byte_len > 0) {
        uint64_t end = n->byte_off + n->byte_len;
        if (end == 0 || end - 1 < n->byte_off) {
            return -1;
        }
        snprintf(range, sizeof(range),
                 "Range: bytes=%" PRIu64 "-%" PRIu64 "\r\n", n->byte_off,
                 end - 1);
    } else if (n->byte_off > 0) {
        snprintf(range, sizeof(range), "Range: bytes=%" PRIu64 "-\r\n",
                 n->byte_off);
    }
    if (strchr(n->host, ':') != NULL) {
        if (default_port(n->is_https, n->portstr)) {
            snprintf(hosthdr, sizeof(hosthdr), "[%s]", n->host);
        } else {
            snprintf(hosthdr, sizeof(hosthdr), "[%s]:%s", n->host, n->portstr);
        }
    } else if (default_port(n->is_https, n->portstr)) {
        snprintf(hosthdr, sizeof(hosthdr), "%s", n->host);
    } else {
        snprintf(hosthdr, sizeof(hosthdr), "%s:%s", n->host, n->portstr);
    }
    nw = snprintf(n->snd, n->snd_cap,
                  "GET %s HTTP/1.1\r\n"
                  "Host: %s\r\n"
                  "User-Agent: MsMedia-net/1.0\r\n"
                  "Accept: */*\r\n"
                  "Accept-Encoding: identity\r\n"
                  "%s"
                  "Connection: close\r\n"
                  "\r\n",
                  n->path, hosthdr, range);
    if (nw < 0 || (size_t)nw >= n->snd_cap) {
        return -1;
    }
    n->snd_len = (size_t)nw;
    n->snd_off = 0;
    return 0;
}

static ms_status apply_target(ms_net *n, const char *url)
{
    if (parse_url(url, &n->is_https, n->host, sizeof(n->host), n->portstr,
                  sizeof(n->portstr), n->path, n->path_cap) != 0) {
        fail_job(n, MS_NET_ERR_INVALID_ARG);
        return map_out(n->err);
    }
    if (strlen(url) >= n->url_cap) {
        fail_job(n, MS_NET_ERR_OVERFLOW);
        return map_out(n->err);
    }
    memcpy(n->url, url, strlen(url) + 1);
    if (visit_add(n, n->url) != 0) {
        fail_job(n, MS_NET_ERR_REDIRECT);
        net_log(n, 1, "redirect loop or cap");
        return map_out(n->err);
    }
    return MS_OK;
}

static ms_status begin_connect(ms_net *n)
{
    struct addrinfo hints;
    int er;
    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;
    hints.ai_protocol = IPPROTO_TCP;
    if (n->ai != NULL) {
        freeaddrinfo(n->ai);
        n->ai = NULL;
        n->ai_cur = NULL;
    }
    er = getaddrinfo(n->host, n->portstr, &hints, &n->ai);
    if (er != 0) {
        fail_job(n, MS_NET_ERR_DNS);
        net_log(n, 1, "dns failed");
        return map_out(n->err);
    }
    n->ai_cur = n->ai;
    n->phase = PH_CONNECT;
    n->deadline_ms = now_ms() + n->lim.connect_timeout_ms;
    return MS_AGAIN;
}

static ms_status try_connect(ms_net *n)
{
    int fd, rc, err, elen;
    if (n->fd < 0) {
        if (n->ai_cur == NULL) {
            fail_job(n, MS_NET_ERR_IO);
            return map_out(n->err);
        }
        fd = socket(n->ai_cur->ai_family, n->ai_cur->ai_socktype,
                    n->ai_cur->ai_protocol);
        if (fd < 0) {
            n->ai_cur = n->ai_cur->ai_next;
            return n->ai_cur ? MS_AGAIN : (fail_job(n, MS_NET_ERR_IO), map_out(n->err));
        }
        if (set_nonblock(fd) != 0) {
            close(fd);
            fail_job(n, MS_NET_ERR_IO);
            return map_out(n->err);
        }
        n->fd = fd;
        rc = connect(fd, n->ai_cur->ai_addr, n->ai_cur->ai_addrlen);
        if (rc == 0) {
            goto connected;
        }
        if (errno != EINPROGRESS && errno != EINTR) {
            net_log(n, 1, "connect errno %d", errno);
            close(n->fd);
            n->fd = -1;
            n->ai_cur = n->ai_cur->ai_next;
            return n->ai_cur ? MS_AGAIN : (fail_job(n, MS_NET_ERR_IO), map_out(n->err));
        }
        return MS_AGAIN;
    }
    elen = (int)sizeof(err);
    err = 0;
    if (getsockopt(n->fd, SOL_SOCKET, SO_ERROR, &err, (socklen_t *)&elen) != 0 ||
        err != 0) {
        net_log(n, 1, "so_error %d", err);
        close(n->fd);
        n->fd = -1;
        n->ai_cur = n->ai_cur ? n->ai_cur->ai_next : NULL;
        return n->ai_cur ? MS_AGAIN : (fail_job(n, MS_NET_ERR_IO), map_out(n->err));
    }
connected:
    if (n->ai != NULL) {
        freeaddrinfo(n->ai);
        n->ai = NULL;
        n->ai_cur = NULL;
    }
    n->deadline_ms = now_ms() + n->lim.io_timeout_ms;
    if (n->is_https) {
        if (ensure_tls_ctx(n) != 0) {
            fail_job(n, MS_NET_ERR_TLS);
            return map_out(n->err);
        }
        if (ms_tls_conn_new(n->tls_ctx, n->fd, n->host, &n->tls) != MS_OK) {
            fail_job(n, MS_NET_ERR_TLS);
            return map_out(n->err);
        }
        n->phase = PH_TLS;
        return MS_AGAIN;
    }
    if (build_request(n) != 0) {
        fail_job(n, MS_NET_ERR_OVERFLOW);
        return map_out(n->err);
    }
    n->phase = PH_SEND;
    return MS_AGAIN;
}

static int parse_hex_u64(const char *s, size_t n, uint64_t *out)
{
    uint64_t v = 0;
    size_t i;
    if (n == 0) {
        return -1;
    }
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        int d;
        if (c >= '0' && c <= '9') {
            d = c - '0';
        } else if (c >= 'a' && c <= 'f') {
            d = c - 'a' + 10;
        } else if (c >= 'A' && c <= 'F') {
            d = c - 'A' + 10;
        } else {
            return -1;
        }
        if (v > (UINT64_MAX >> 4)) {
            return -1;
        }
        v = (v << 4) + (uint64_t)d;
    }
    *out = v;
    return 0;
}

static ms_status handle_status_and_headers(ms_net *n)
{
    char *line_end;
    int ver_maj, ver_min, status;
    const char *v;
    size_t vl;
    char *blank;
    size_t hlen;
    int chunked = 0;
    int has_cl = 0;
    uint64_t cl = 0;

    blank = strstr(n->hdr, "\r\n\r\n");
    if (blank == NULL) {
        fail_job(n, MS_NET_ERR_MALFORMED);
        return map_out(n->err);
    }
    hlen = (size_t)(blank - n->hdr) + 4;
    line_end = strstr(n->hdr, "\r\n");
    if (line_end == NULL) {
        fail_job(n, MS_NET_ERR_MALFORMED);
        return map_out(n->err);
    }
    *line_end = 0;
    if (sscanf(n->hdr, "HTTP/%d.%d %d", &ver_maj, &ver_min, &status) != 3) {
        *line_end = '\r';
        fail_job(n, MS_NET_ERR_MALFORMED);
        return map_out(n->err);
    }
    *line_end = '\r';
    n->http_status = status;
    if (hdr_ci_find(n->hdr, hlen, "transfer-encoding", &v, &vl)) {
        if (vl >= 7 && ci_eq(v, "chunked", 7)) {
            chunked = 1;
        } else if (vl != 8 || !ci_eq(v, "identity", 8)) {
            fail_job(n, MS_NET_ERR_UNSUPPORTED);
            net_log(n, 1, "unsupported transfer-encoding");
            return map_out(n->err);
        }
    }
    if (hdr_ci_find(n->hdr, hlen, "content-encoding", &v, &vl)) {
        if (vl != 8 || !ci_eq(v, "identity", 8)) {
            fail_job(n, MS_NET_ERR_UNSUPPORTED);
            net_log(n, 1, "unsupported content-encoding");
            return map_out(n->err);
        }
    }
    if (hdr_ci_find(n->hdr, hlen, "content-length", &v, &vl)) {
        char tmp[32];
        char *end = NULL;
        if (vl >= sizeof(tmp) || vl == 0) {
            fail_job(n, MS_NET_ERR_MALFORMED);
            return map_out(n->err);
        }
        memcpy(tmp, v, vl);
        tmp[vl] = 0;
        cl = strtoull(tmp, &end, 10);
        if (end == tmp) {
            fail_job(n, MS_NET_ERR_MALFORMED);
            return map_out(n->err);
        }
        has_cl = 1;
    }
    if (chunked && has_cl) {
        /* 拒绝同时出现，避免走私歧义。 */
        fail_job(n, MS_NET_ERR_MALFORMED);
        return map_out(n->err);
    }

    if (status >= 300 && status < 400) {
        char loc[NET_ABS_URL];
        char resolved[NET_ABS_URL];
        int was_https = n->is_https;
        int new_https;
        char dummy_host[256], dummy_port[16];
        if (status != 301 && status != 302 && status != 303 && status != 307 &&
            status != 308) {
            fail_job(n, MS_NET_ERR_HTTP);
            net_log(n, 1, "http status %d", status);
            return map_out(n->err);
        }
        if (n->redirects_left <= 0) {
            fail_job(n, MS_NET_ERR_REDIRECT);
            net_log(n, 1, "too many redirects");
            return map_out(n->err);
        }
        if (!hdr_ci_find(n->hdr, hlen, "location", &v, &vl) || vl == 0 ||
            vl >= sizeof(loc)) {
            fail_job(n, MS_NET_ERR_MALFORMED);
            return map_out(n->err);
        }
        memcpy(loc, v, vl);
        loc[vl] = 0;
        if (has_ctl(loc) ||
            ms_hls_resolve_uri(n->url, loc, resolved, sizeof(resolved)) !=
                MS_OK) {
            fail_job(n, MS_NET_ERR_REDIRECT);
            return map_out(n->err);
        }
        if (parse_url(resolved, &new_https, dummy_host, sizeof(dummy_host),
                      dummy_port, sizeof(dummy_port), n->path,
                      n->path_cap) != 0) {
            fail_job(n, MS_NET_ERR_REDIRECT);
            return map_out(n->err);
        }
        if (was_https && !new_https) {
            fail_job(n, MS_NET_ERR_REDIRECT);
            net_log(n, 1, "https to http redirect denied");
            return map_out(n->err);
        }
        n->redirects_left--;
        reset_job_io(n);
        if (apply_target(n, resolved) != MS_OK) {
            return map_out(n->err);
        }
        n->phase = PH_RESOLVE;
        n->deadline_ms = now_ms() + n->lim.connect_timeout_ms;
        return MS_AGAIN;
    }

    if (status == 401 || status == 403) {
        fail_job(n, MS_NET_ERR_HTTP);
        net_log(n, 1, "http status %d (no auth bypass)", status);
        return map_out(n->err);
    }
    if (status != 200 && status != 206) {
        fail_job(n, MS_NET_ERR_HTTP);
        net_log(n, 1, "http status %d", status);
        return map_out(n->err);
    }
    if (n->byte_len > 0 && status == 200) {
        /* 服务器忽略 Range 时仍接受 200，从流开头读，由调用方处理。 */
        net_log(n, 0, "range requested but status 200");
    }

    /* 把头后已读入的载荷移到 staging。 */
    {
        size_t extra = n->hdr_len - hlen;
        if (extra > 0) {
            if (extra > n->buf_cap) {
                fail_job(n, MS_NET_ERR_OVERFLOW);
                return map_out(n->err);
            }
            memcpy(n->buf, n->hdr + hlen, extra);
            n->buf_r = 0;
            n->buf_w = extra;
        }
        n->hdr_len = hlen;
    }
    if (chunked) {
        n->body_mode = BM_CHUNK;
        n->chunk_st = CH_SIZE;
        n->chunk_left = 0;
        n->chline_len = 0;
    } else if (has_cl) {
        n->body_mode = BM_CL;
        n->cl_known = 1;
        n->cl_left = cl;
    } else {
        n->body_mode = BM_EOF;
    }
    n->phase = PH_BODY;
    if (n->body_mode == BM_CL && n->cl_left == 0 && n->buf_w == n->buf_r) {
        n->phase = PH_DONE;
        n->saw_eof = 1;
        n->job_live = 1;
    }
    return MS_OK;
}

static size_t staging_space(const ms_net *n)
{
    return n->buf_cap - n->buf_w;
}

static void staging_compact(ms_net *n)
{
    if (n->buf_r == 0) {
        return;
    }
    if (n->buf_r >= n->buf_w) {
        n->buf_r = 0;
        n->buf_w = 0;
        return;
    }
    memmove(n->buf, n->buf + n->buf_r, n->buf_w - n->buf_r);
    n->buf_w -= n->buf_r;
    n->buf_r = 0;
}

static ms_status recv_into_staging(ms_net *n)
{
    size_t space, got = 0;
    ms_status st;
    staging_compact(n);
    space = staging_space(n);
    if (space == 0) {
        return MS_LIMIT;
    }
    st = conn_read(n, n->buf + n->buf_w, space, &got);
    if (st == MS_AGAIN) {
        return MS_AGAIN;
    }
    if (st != MS_OK) {
        fail_job(n, MS_NET_ERR_IO);
        return map_out(n->err);
    }
    if (got == 0) {
        /* peer close */
        if (n->phase == PH_HDR) {
            fail_job(n, MS_NET_ERR_MALFORMED);
            return map_out(n->err);
        }
        if (n->body_mode == BM_CL && n->cl_known &&
            (size_t)staging_avail(n) < n->cl_left) {
            fail_job(n, MS_NET_ERR_MALFORMED);
            net_log(n, 1, "truncated content-length");
            return map_out(n->err);
        }
        if (n->body_mode == BM_CHUNK && n->chunk_st != CH_TRAIL &&
            !(n->chunk_st == CH_DATA && n->chunk_left == 0)) {
            /* 未正常 0 块结束 */
            fail_job(n, MS_NET_ERR_MALFORMED);
            return map_out(n->err);
        }
        n->phase = PH_DONE;
        n->saw_eof = 1;
        return MS_OK;
    }
    n->buf_w += got;
    return MS_OK;
}

static ms_status feed_headers(ms_net *n)
{
    /* 把头从 socket 读到 hdr，直到空行。若 staging 里有数据（不应在 hdr 阶段）*/
    size_t got = 0;
    ms_status st;
    size_t space;
    if (n->hdr_len + 4 >= n->hdr_cap) {
        fail_job(n, MS_NET_ERR_OVERFLOW);
        return map_out(n->err);
    }
    space = n->hdr_cap - 1 - n->hdr_len;
    st = conn_read(n, (uint8_t *)(n->hdr + n->hdr_len), space, &got);
    if (st == MS_AGAIN) {
        return MS_AGAIN;
    }
    if (st != MS_OK) {
        fail_job(n, MS_NET_ERR_IO);
        return map_out(n->err);
    }
    if (got == 0) {
        fail_job(n, MS_NET_ERR_MALFORMED);
        return map_out(n->err);
    }
    n->hdr_len += got;
    n->hdr[n->hdr_len] = 0;
    if (strstr(n->hdr, "\r\n\r\n") != NULL) {
        return handle_status_and_headers(n);
    }
    if (n->hdr_len >= n->lim.max_header_bytes) {
        fail_job(n, MS_NET_ERR_OVERFLOW);
        return map_out(n->err);
    }
    return MS_AGAIN;
}

static int staging_avail(const ms_net *n)
{
    return (n->buf_w > n->buf_r) ? (int)(n->buf_w - n->buf_r) : 0;
}

static void staging_eat(ms_net *n, size_t k)
{
    n->buf_r += k;
    if (n->buf_r >= n->buf_w) {
        n->buf_r = 0;
        n->buf_w = 0;
    }
}

static ms_status parse_chunk_meta(ms_net *n)
{
    /* 消耗 staging 中的块大小行 / CRLF / trailers，把数据块留给用户。 */
    for (;;) {
        if (n->chunk_st == CH_DATA) {
            return MS_OK;
        }
        if (n->chunk_st == CH_TRAIL) {
            /* 等到最后 CRLF */
            uint8_t *p = n->buf + n->buf_r;
            size_t av = (size_t)staging_avail(n);
            uint8_t *nl;
            if (av < 2) {
                return MS_AGAIN;
            }
            /* 简化：直到单独的 \r\n */
            if (av >= 2 && p[0] == '\r' && p[1] == '\n') {
                staging_eat(n, 2);
                n->phase = PH_DONE;
                n->saw_eof = 1;
                return MS_OK;
            }
            nl = memchr(p, '\n', av);
            if (nl == NULL) {
                if (av > 512) {
                    fail_job(n, MS_NET_ERR_MALFORMED);
                    return map_out(n->err);
                }
                return MS_AGAIN;
            }
            staging_eat(n, (size_t)(nl - p + 1));
            continue;
        }
        if (n->chunk_st == CH_CRLF) {
            size_t av = (size_t)staging_avail(n);
            uint8_t *p = n->buf + n->buf_r;
            if (av < 2) {
                return MS_AGAIN;
            }
            if (p[0] != '\r' || p[1] != '\n') {
                fail_job(n, MS_NET_ERR_MALFORMED);
                return map_out(n->err);
            }
            staging_eat(n, 2);
            n->chunk_st = CH_SIZE;
            continue;
        }
        /* CH_SIZE */
        {
            size_t av = (size_t)staging_avail(n);
            uint8_t *p = n->buf + n->buf_r;
            uint8_t *nl = memchr(p, '\n', av);
            size_t ln;
            const char *hex;
            size_t hexn;
            uint64_t sz;
            if (nl == NULL) {
                if (av > 62) {
                    fail_job(n, MS_NET_ERR_MALFORMED);
                    return map_out(n->err);
                }
                return MS_AGAIN;
            }
            ln = (size_t)(nl - p + 1);
            hex = (const char *)p;
            hexn = ln;
            if (hexn >= 1 && hex[hexn - 1] == '\n') {
                hexn--;
            }
            if (hexn >= 1 && hex[hexn - 1] == '\r') {
                hexn--;
            }
            {
                const char *semi = memchr(hex, ';', hexn);
                if (semi != NULL) {
                    hexn = (size_t)(semi - hex);
                }
            }
            if (parse_hex_u64(hex, hexn, &sz) != 0) {
                fail_job(n, MS_NET_ERR_MALFORMED);
                return map_out(n->err);
            }
            staging_eat(n, ln);
            n->chunk_left = sz;
            if (sz == 0) {
                n->chunk_st = CH_TRAIL;
                continue;
            }
            n->chunk_st = CH_DATA;
            return MS_OK;
        }
    }
}

static ms_status pump_body(ms_net *n)
{
    ms_status st;
    if (n->phase != PH_BODY) {
        return MS_OK;
    }
    if (n->body_mode == BM_CHUNK) {
        st = parse_chunk_meta(n);
        if (st != MS_OK && st != MS_AGAIN) {
            return st;
        }
    }
    if (staging_space(n) > 0 && n->phase == PH_BODY) {
        int need = 1;
        if (n->body_mode == BM_CL &&
            (n->cl_left == 0 ||
             (n->cl_known && (size_t)staging_avail(n) >= n->cl_left))) {
            need = 0;
        }
        if (n->body_mode == BM_CHUNK && n->chunk_st == CH_DATA &&
            (size_t)staging_avail(n) >= n->chunk_left) {
            need = 0;
        }
        if (need && staging_space(n) > 0) {
            st = recv_into_staging(n);
            if (st != MS_OK && st != MS_AGAIN && st != MS_LIMIT) {
                return st;
            }
        }
        if (n->body_mode == BM_CHUNK) {
            st = parse_chunk_meta(n);
            if (st != MS_OK && st != MS_AGAIN) {
                return st;
            }
        }
    }
    if (n->body_mode == BM_CL && n->cl_left == 0 &&
        (size_t)staging_avail(n) == 0) {
        n->phase = PH_DONE;
        n->saw_eof = 1;
    }
    return MS_OK;
}

static ms_status drive_core(ms_net *n, int wait_ms)
{
    ms_status st;
    int slice;
    int hops = 0;
    if (n->canceled) {
        fail_job(n, MS_NET_ERR_CANCELED);
        return MS_ERR_CANCELED;
    }
    if (!n->job_live) {
        return MS_ERR_STATE;
    }
    if (n->phase == PH_FAIL) {
        return map_out(n->err);
    }
    if (n->phase == PH_DONE) {
        return MS_OK;
    }
    if (n->deadline_ms != 0 && now_ms() >= n->deadline_ms) {
        fail_job(n, MS_NET_ERR_TIMEOUT);
        net_log(n, 1, "timeout");
        return map_out(n->err);
    }
    slice = remain_ms(n);
    if (wait_ms >= 0 && wait_ms < slice) {
        slice = wait_ms;
    }

again:
    if (hops++ > 8) {
        return MS_AGAIN;
    }
    if (n->phase == PH_RESOLVE) {
        st = begin_connect(n);
        if (st != MS_AGAIN && st != MS_OK) {
            return st;
        }
    }

    if (n->phase == PH_CONNECT) {
        if (n->fd >= 0) {
            int w = io_wait(n->fd, 1, slice);
            if (w < 0) {
                fail_job(n, MS_NET_ERR_IO);
                return map_out(n->err);
            }
            if (w == 0 && n->fd >= 0) {
                if (remain_ms(n) <= 0) {
                    fail_job(n, MS_NET_ERR_TIMEOUT);
                    net_log(n, 1, "connect timeout");
                    return map_out(n->err);
                }
                return MS_AGAIN;
            }
        }
        st = try_connect(n);
        if (st != MS_AGAIN && st != MS_OK) {
            return st;
        }
        if (n->phase == PH_CONNECT) {
            return MS_AGAIN;
        }
    }

    if (n->phase == PH_TLS) {
        int w;
        st = ms_tls_handshake(n->tls);
        if (st == MS_AGAIN) {
            w = io_wait(n->fd, wait_want(n), slice);
            if (w < 0) {
                fail_job(n, MS_NET_ERR_TLS);
                return map_out(n->err);
            }
            if (remain_ms(n) <= 0) {
                fail_job(n, MS_NET_ERR_TIMEOUT);
                return map_out(n->err);
            }
            return MS_AGAIN;
        }
        if (st != MS_OK) {
            fail_job(n, MS_NET_ERR_TLS);
            net_log(n, 1, "tls handshake or verify failed");
            return map_out(n->err);
        }
        if (build_request(n) != 0) {
            fail_job(n, MS_NET_ERR_OVERFLOW);
            return map_out(n->err);
        }
        n->phase = PH_SEND;
        n->deadline_ms = now_ms() + n->lim.io_timeout_ms;
    }

    if (n->phase == PH_SEND) {
        while (n->snd_off < n->snd_len) {
            size_t used = 0;
            st = conn_write(n, (const uint8_t *)(n->snd + n->snd_off),
                            n->snd_len - n->snd_off, &used);
            if (st == MS_AGAIN) {
                int w = io_wait(n->fd, 1, slice);
                if (w < 0) {
                    fail_job(n, MS_NET_ERR_IO);
                    return map_out(n->err);
                }
                if (remain_ms(n) <= 0) {
                    fail_job(n, MS_NET_ERR_TIMEOUT);
                    return map_out(n->err);
                }
                return MS_AGAIN;
            }
            if (st != MS_OK) {
                fail_job(n, MS_NET_ERR_IO);
                return map_out(n->err);
            }
            n->snd_off += used;
        }
        n->phase = PH_HDR;
        n->hdr_len = 0;
        n->hdr[0] = 0;
    }

    if (n->phase == PH_HDR) {
        for (;;) {
            st = feed_headers(n);
            if (n->phase != PH_HDR) {
                /* 例如 3xx 已改写为新连接，勿再 poll 已关闭的 fd。 */
                if (st != MS_OK && st != MS_AGAIN) {
                    return st;
                }
                break;
            }
            if (st == MS_AGAIN) {
                int w = io_wait(n->fd, wait_want(n), slice);
                if (w < 0) {
                    fail_job(n, MS_NET_ERR_IO);
                    return map_out(n->err);
                }
                if (remain_ms(n) <= 0) {
                    fail_job(n, MS_NET_ERR_TIMEOUT);
                    return map_out(n->err);
                }
                return MS_AGAIN;
            }
            if (st != MS_OK && st != MS_AGAIN) {
                return st;
            }
        }
    }
    if (n->phase == PH_RESOLVE || n->phase == PH_CONNECT ||
        n->phase == PH_TLS || n->phase == PH_SEND) {
        goto again;
    }

    if (n->phase == PH_BODY) {
        st = pump_body(n);
        if (st == MS_AGAIN) {
            if (staging_space(n) == 0) {
                return MS_OK; /* 反压：等 read 消耗 */
            }
            {
                int w = io_wait(n->fd, wait_want(n), slice);
                if (w < 0) {
                    fail_job(n, MS_NET_ERR_IO);
                    return map_out(n->err);
                }
                if (remain_ms(n) <= 0) {
                    fail_job(n, MS_NET_ERR_TIMEOUT);
                    return map_out(n->err);
                }
            }
            return MS_AGAIN;
        }
        if (st != MS_OK && st != MS_LIMIT) {
            return st;
        }
    }
    if (n->phase == PH_FAIL) {
        return map_out(n->err);
    }
    return MS_OK;
}

static void *worker_main(void *arg)
{
    ms_net *n = (ms_net *)arg;
    block_sigpipe();
    pthread_mutex_lock(&n->mu);
    while (!n->stop_thread) {
        if (n->job_live && n->phase != PH_DONE && n->phase != PH_FAIL &&
            n->phase != PH_IDLE) {
            (void)drive_core(n, NET_PUMP_SLICE);
            if (n->job_live && n->phase != PH_DONE && n->phase != PH_FAIL) {
                /* 再循环 */
            } else {
                pthread_cond_broadcast(&n->cv);
            }
            pthread_mutex_unlock(&n->mu);
            usleep(1000);
            pthread_mutex_lock(&n->mu);
            continue;
        }
        {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 50 * 1000 * 1000;
            if (ts.tv_nsec >= 1000000000) {
                ts.tv_sec++;
                ts.tv_nsec -= 1000000000;
            }
            pthread_cond_timedwait(&n->cv, &n->mu, &ts);
        }
    }
    pthread_mutex_unlock(&n->mu);
    return NULL;
}

ms_status ms_net_create(const ms_allocator *alloc, const ms_net_limits *limits,
                        ms_net **out)
{
    ms_net *n;
    ms_net_limits lim;
    ms_status st;
    if (out == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    *out = NULL;
    st = apply_lim(limits, &lim);
    if (st != MS_OK) {
        return st;
    }
    if (alloc != NULL && (alloc->alloc_fn == NULL || alloc->free_fn == NULL)) {
        return MS_ERR_INVALID_ARG;
    }
    if (alloc != NULL) {
        n = (ms_net *)alloc->alloc_fn(alloc->ctx, sizeof(*n));
    } else {
        n = (ms_net *)malloc(sizeof(*n));
    }
    if (n == NULL) {
        return MS_ERR_NOMEM;
    }
    memset(n, 0, sizeof(*n));
    n->fd = -1;
    if (alloc != NULL) {
        n->alloc = *alloc;
        n->using_default = 0;
    } else {
        n->alloc.alloc_fn = sys_alloc;
        n->alloc.free_fn = sys_free;
        n->using_default = 1;
    }
    n->lim = lim;
    n->url_cap = lim.max_url_bytes;
    n->path_cap = lim.max_url_bytes;
    n->snd_cap = 1024 + lim.max_url_bytes;
    n->hdr_cap = lim.max_header_bytes + 4;
    n->buf_cap = lim.max_buffer_bytes;
    pthread_mutex_init(&n->mu, NULL);
    pthread_cond_init(&n->cv, NULL);
    n->url = (char *)nalloc(n, n->url_cap);
    n->path = (char *)nalloc(n, n->path_cap);
    n->snd = (char *)nalloc(n, n->snd_cap);
    n->hdr = (char *)nalloc(n, n->hdr_cap);
    n->buf = (uint8_t *)nalloc(n, n->buf_cap);
    if (n->url == NULL || n->path == NULL || n->snd == NULL || n->hdr == NULL ||
        n->buf == NULL) {
        ms_net_destroy(n);
        return MS_ERR_NOMEM;
    }
    *out = n;
    return MS_OK;
}

void ms_net_destroy(ms_net *n)
{
    if (n == NULL) {
        return;
    }
    ms_net_worker_stop(n);
    close_conn(n);
    nfree(n, n->url);
    nfree(n, n->path);
    nfree(n, n->snd);
    nfree(n, n->hdr);
    nfree(n, n->buf);
    nfree(n, n->ca_file);
    if (n->tls_ctx != NULL) {
        ms_tls_ctx_free(n->tls_ctx);
    }
    pthread_mutex_destroy(&n->mu);
    pthread_cond_destroy(&n->cv);
    if (n->using_default) {
        free(n);
    } else {
        n->alloc.free_fn(n->alloc.ctx, n);
    }
}

ms_status ms_net_set_ca_file(ms_net *n, const char *path)
{
    size_t L;
    char *copy;
    if (n == NULL || path == NULL || path[0] == 0) {
        return MS_ERR_INVALID_ARG;
    }
    L = strlen(path);
    copy = (char *)nalloc(n, L + 1);
    if (copy == NULL) {
        return MS_ERR_NOMEM;
    }
    memcpy(copy, path, L + 1);
    pthread_mutex_lock(&n->mu);
    nfree(n, n->ca_file);
    n->ca_file = copy;
    if (n->have_tls_ctx) {
        ms_tls_ctx_free(n->tls_ctx);
        n->tls_ctx = NULL;
        n->have_tls_ctx = 0;
    }
    pthread_mutex_unlock(&n->mu);
    return MS_OK;
}

void ms_net_set_log(ms_net *n, ms_net_log_fn fn, void *ctx)
{
    if (n == NULL) {
        return;
    }
    pthread_mutex_lock(&n->mu);
    n->log_fn = fn;
    n->log_ctx = ctx;
    pthread_mutex_unlock(&n->mu);
}

ms_status ms_net_worker_start(ms_net *n)
{
    int rc;
    if (n == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    pthread_mutex_lock(&n->mu);
    if (n->have_thread) {
        pthread_mutex_unlock(&n->mu);
        return MS_OK;
    }
    n->stop_thread = 0;
    rc = pthread_create(&n->th, NULL, worker_main, n);
    if (rc != 0) {
        pthread_mutex_unlock(&n->mu);
        return MS_ERR_UNKNOWN;
    }
    n->have_thread = 1;
    pthread_mutex_unlock(&n->mu);
    return MS_OK;
}

ms_status ms_net_worker_stop(ms_net *n)
{
    if (n == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    pthread_mutex_lock(&n->mu);
    if (!n->have_thread) {
        pthread_mutex_unlock(&n->mu);
        return MS_OK;
    }
    n->stop_thread = 1;
    pthread_cond_broadcast(&n->cv);
    pthread_mutex_unlock(&n->mu);
    pthread_join(n->th, NULL);
    pthread_mutex_lock(&n->mu);
    n->have_thread = 0;
    pthread_mutex_unlock(&n->mu);
    return MS_OK;
}

ms_status ms_net_drive(ms_net *n)
{
    ms_status st;
    if (n == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    block_sigpipe();
    pthread_mutex_lock(&n->mu);
    if (!n->job_live) {
        pthread_mutex_unlock(&n->mu);
        return MS_ERR_STATE;
    }
    st = drive_core(n, NET_PUMP_SLICE);
    pthread_mutex_unlock(&n->mu);
    return st;
}

ms_status ms_net_get_start(ms_net *n, const char *url, uint64_t byte_off,
                           uint64_t byte_len)
{
    ms_status st;
    if (n == NULL || url == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    if (has_ctl(url) || strlen(url) >= n->url_cap) {
        return MS_ERR_INVALID_ARG;
    }
    pthread_mutex_lock(&n->mu);
    if (n->job_live && n->phase != PH_DONE && n->phase != PH_FAIL) {
        pthread_mutex_unlock(&n->mu);
        return MS_ERR_STATE;
    }
    reset_job_io(n);
    n->err = MS_NET_OK;
    n->http_status = 0;
    n->canceled = 0;
    n->byte_off = byte_off;
    n->byte_len = byte_len;
    n->redirects_left = n->lim.max_redirects;
    n->retries_left = n->lim.max_retries;
    n->nvisit = 0;
    n->job_id++;
    if (n->job_id == 0) {
        n->job_id = 1;
    }
    n->job_live = 1;
    n->phase = PH_RESOLVE;
    n->deadline_ms = now_ms() + n->lim.connect_timeout_ms;
    st = apply_target(n, url);
    if (st != MS_OK) {
        n->job_live = 0;
        pthread_mutex_unlock(&n->mu);
        return st;
    }
    pthread_cond_broadcast(&n->cv);
    pthread_mutex_unlock(&n->mu);
    return MS_OK;
}

ms_status ms_net_read(ms_net *n, uint8_t *buf, size_t len, size_t *out_n)
{
    size_t take;
    size_t av;
    if (out_n != NULL) {
        *out_n = 0;
    }
    if (n == NULL || out_n == NULL || (buf == NULL && len != 0)) {
        return MS_ERR_INVALID_ARG;
    }
    pthread_mutex_lock(&n->mu);
    if (!n->job_live) {
        pthread_mutex_unlock(&n->mu);
        return MS_ERR_STATE;
    }
    if (n->canceled) {
        fail_job(n, MS_NET_ERR_CANCELED);
        pthread_mutex_unlock(&n->mu);
        return MS_ERR_CANCELED;
    }
    if (n->phase == PH_FAIL) {
        ms_status st = map_out(n->err);
        pthread_mutex_unlock(&n->mu);
        return st;
    }
    /* 无 worker 时在此做有界泵送，避免无限阻塞。 */
    if (!n->have_thread && n->phase != PH_DONE && n->phase != PH_FAIL) {
        (void)drive_core(n, NET_PUMP_SLICE);
    }
    if (n->phase == PH_FAIL) {
        ms_status st = map_out(n->err);
        pthread_mutex_unlock(&n->mu);
        return st;
    }
    if (n->body_mode == BM_CHUNK && n->phase == PH_BODY) {
        (void)parse_chunk_meta(n);
    }
    av = (size_t)staging_avail(n);
    if (n->body_mode == BM_CHUNK && n->chunk_st == CH_DATA && av > n->chunk_left) {
        av = (size_t)n->chunk_left;
    }
    if (n->body_mode == BM_CL && n->cl_known && av > n->cl_left) {
        av = (size_t)n->cl_left;
    }
    if (av == 0) {
        if (n->phase == PH_DONE || n->saw_eof) {
            pthread_mutex_unlock(&n->mu);
            return MS_OK;
        }
        pthread_mutex_unlock(&n->mu);
        return MS_AGAIN;
    }
    take = av < len ? av : len;
    if (take > 0 && buf != NULL) {
        memcpy(buf, n->buf + n->buf_r, take);
    }
    staging_eat(n, take);
    if (n->body_mode == BM_CL && n->cl_known) {
        n->cl_left -= take;
        if (n->cl_left == 0 && staging_avail(n) == 0) {
            n->phase = PH_DONE;
            n->saw_eof = 1;
        }
    }
    if (n->body_mode == BM_CHUNK && n->chunk_st == CH_DATA) {
        n->chunk_left -= take;
        if (n->chunk_left == 0) {
            n->chunk_st = CH_CRLF;
        }
    }
    *out_n = take;
    pthread_cond_broadcast(&n->cv);
    pthread_mutex_unlock(&n->mu);
    return MS_OK;
}

void ms_net_cancel(ms_net *n)
{
    if (n == NULL) {
        return;
    }
    pthread_mutex_lock(&n->mu);
    n->canceled = 1;
    if (n->fd >= 0) {
        shutdown(n->fd, SHUT_RDWR);
    }
    fail_job(n, MS_NET_ERR_CANCELED);
    pthread_cond_broadcast(&n->cv);
    pthread_mutex_unlock(&n->mu);
}

ms_net_err ms_net_last_err(const ms_net *n)
{
    return n != NULL ? n->err : MS_NET_ERR_INVALID_ARG;
}

int ms_net_last_http_status(const ms_net *n)
{
    return n != NULL ? n->http_status : 0;
}

static ms_status fetch_request(void *ctx, const ms_hls_request *req,
                               uint32_t *out_id)
{
    ms_net *n = (ms_net *)ctx;
    ms_status st;
    if (out_id != NULL) {
        *out_id = 0;
    }
    if (n == NULL || req == NULL || req->uri == NULL || out_id == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    st = ms_net_get_start(n, req->uri, req->byte_off, req->byte_len);
    if (st != MS_OK) {
        return st;
    }
    pthread_mutex_lock(&n->mu);
    *out_id = n->job_id;
    pthread_mutex_unlock(&n->mu);
    return MS_OK;
}

static ms_status fetch_read(void *ctx, uint32_t id, uint8_t *buf, size_t len,
                            size_t *out_n)
{
    ms_net *n = (ms_net *)ctx;
    if (n == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    if (id != 0 && id != n->job_id) {
        return MS_ERR_STATE;
    }
    return ms_net_read(n, buf, len, out_n);
}

static void fetch_cancel(void *ctx, uint32_t id)
{
    ms_net *n = (ms_net *)ctx;
    (void)id;
    ms_net_cancel(n);
}

void ms_net_hls_fetch_bind(ms_net *n, ms_hls_fetch *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->struct_size = sizeof(*out);
    out->ctx = n;
    out->request_fn = fetch_request;
    out->read_fn = fetch_read;
    out->cancel_fn = fetch_cancel;
}
