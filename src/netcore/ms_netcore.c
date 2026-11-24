/*
 * 公共可控网络层实现：拨号 / DNS 钩子 / SOCKS5 / TLS 泵 / HTTP/1.1。
 * 阻塞式 + poll 分片（每 100ms 检查取消），不忙等。所有缓冲有界。
 * TLS 后端二选一：MS_NC_TLS_OPENSSL=极致裁剪 OpenSSL（路线 B 主力，ms_nc_tls_ossl.c）；
 * 否则安卓系统 SSLEngine（ms_android_tls.c，JNI）。
 * 非 Android 宿主无系统 TLS：https 诚实失败（MS_NC_E_TLS），不降级为明文、不关校验。
 * by: Long <mycjzb@gmail.com>
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "ms_netcore.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#if defined(__ANDROID__) && !defined(MS_NC_TLS_OPENSSL)
#include "../ff/ms_android_tls.h"
#endif

#define NC_SLICE_MS 100
#define NC_MAX_ADDRS 8
#define NC_HDR_MAX (32 * 1024)
#define NC_URL_MAX 4096

const char *ms_nc_strerror(int e)
{
    switch (e) {
    case MS_NC_OK: return "ok";
    case MS_NC_E_ARG: return "invalid argument";
    case MS_NC_E_NOMEM: return "out of memory";
    case MS_NC_E_DNS: return "dns resolve failed";
    case MS_NC_E_CONNECT: return "connect failed";
    case MS_NC_E_TIMEOUT: return "timeout";
    case MS_NC_E_CANCELED: return "canceled";
    case MS_NC_E_IO: return "io error";
    case MS_NC_E_TLS: return "tls failed (system verification kept)";
    case MS_NC_E_PROXY: return "socks5 proxy failed";
    case MS_NC_E_PROTO: return "http protocol error";
    case MS_NC_E_REDIRECT: return "redirect refused or too many";
    case MS_NC_E_UNSUPPORTED: return "unsupported";
    default: return "error";
    }
}

/* ---------------- 配置 ---------------- */
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static ms_nc_config g_cfg;
static int g_cfg_init;
static ms_nc_dns_fn g_dns_fn;
static void *g_dns_ctx;

void ms_nc_config_default(ms_nc_config *c)
{
    if (c) {
        memset(c, 0, sizeof(*c));
        c->socks_remote_dns = 1;
    }
}

int ms_nc_configure(const ms_nc_config *c)
{
    if (c == NULL || c->socks_port < 0 || c->socks_port > 65535) {
        return MS_NC_E_ARG;
    }
    if (c->socks_host[0] && c->socks_port == 0) {
        return MS_NC_E_ARG;
    }
    pthread_mutex_lock(&g_mu);
    g_cfg = *c;
    g_cfg.dns_map[sizeof(g_cfg.dns_map) - 1] = 0;
    g_cfg.socks_host[sizeof(g_cfg.socks_host) - 1] = 0;
    g_cfg.socks_user[sizeof(g_cfg.socks_user) - 1] = 0;
    g_cfg.socks_pass[sizeof(g_cfg.socks_pass) - 1] = 0;
    g_cfg_init = 1;
    pthread_mutex_unlock(&g_mu);
    return MS_NC_OK;
}

void ms_nc_get_config(ms_nc_config *out)
{
    if (out == NULL) {
        return;
    }
    pthread_mutex_lock(&g_mu);
    if (!g_cfg_init) {
        ms_nc_config_default(&g_cfg);
        g_cfg_init = 1;
    }
    *out = g_cfg;
    pthread_mutex_unlock(&g_mu);
}

void ms_nc_set_dns_hook(ms_nc_dns_fn fn, void *ctx)
{
    pthread_mutex_lock(&g_mu);
    g_dns_fn = fn;
    g_dns_ctx = ctx;
    pthread_mutex_unlock(&g_mu);
}

/* ---------------- 时间 / 等待 ---------------- */
static int64_t mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int canceled(volatile int *cancel)
{
    return cancel != NULL && __atomic_load_n(cancel, __ATOMIC_ACQUIRE) != 0;
}

int ms_nc_wait_fd(int fd, int want_write, int timeout_ms, volatile int *cancel)
{
    int64_t end = timeout_ms >= 0 ? mono_ms() + timeout_ms : 0;
    for (;;) {
        struct pollfd pfd;
        int slice = NC_SLICE_MS, r;
        if (canceled(cancel)) {
            return MS_NC_E_CANCELED;
        }
        if (timeout_ms >= 0) {
            int64_t left = end - mono_ms();
            if (left <= 0) {
                return 0;
            }
            if (left < slice) {
                slice = (int)left;
            }
        }
        pfd.fd = fd;
        pfd.events = want_write ? POLLOUT : POLLIN;
        pfd.revents = 0;
        r = poll(&pfd, 1, slice);
        if (r > 0) {
            return 1;
        }
        if (r < 0 && errno != EINTR) {
            return MS_NC_E_IO;
        }
    }
}

/* ---------------- DNS ---------------- */
static int put_ip(const char *ip, int port, struct sockaddr_storage *out)
{
    struct sockaddr_in *a4 = (struct sockaddr_in *)out;
    struct sockaddr_in6 *a6 = (struct sockaddr_in6 *)out;
    memset(out, 0, sizeof(*out));
    if (inet_pton(AF_INET, ip, &a4->sin_addr) == 1) {
        a4->sin_family = AF_INET;
        a4->sin_port = htons((uint16_t)port);
        return 1;
    }
    if (inet_pton(AF_INET6, ip, &a6->sin6_addr) == 1) {
        a6->sin6_family = AF_INET6;
        a6->sin6_port = htons((uint16_t)port);
        return 1;
    }
    return 0;
}

/* 静态映射："host=ip1,ip2;host2=ip3"，主机名大小写不敏感。 */
static int map_lookup(const char *map, const char *host, int port,
                      struct sockaddr_storage *out, int cap)
{
    const char *p = map;
    size_t hl = strlen(host);
    int n = 0;
    while (p && *p) {
        const char *semi = strchr(p, ';');
        const char *eq = strchr(p, '=');
        const char *end = semi ? semi : p + strlen(p);
        if (eq && eq < end && (size_t)(eq - p) == hl && strncasecmp(p, host, hl) == 0) {
            const char *q = eq + 1;
            while (q < end && n < cap) {
                char ip[64];
                const char *comma = memchr(q, ',', (size_t)(end - q));
                const char *ie = comma ? comma : end;
                size_t il = (size_t)(ie - q);
                if (il > 0 && il < sizeof(ip)) {
                    memcpy(ip, q, il);
                    ip[il] = 0;
                    n += put_ip(ip, port, &out[n]);
                }
                q = comma ? comma + 1 : end;
            }
            return n;
        }
        p = semi ? semi + 1 : NULL;
    }
    return 0;
}

#ifdef __ANDROID__
static int java_dns(const char *host, int port, struct sockaddr_storage *out, int cap);
#endif

static int resolve(const ms_nc_config *cfg, const char *host, int port,
                   struct sockaddr_storage *out, int cap)
{
    struct addrinfo hints, *ai = NULL, *it;
    char ps[8];
    int n = 0;
    ms_nc_dns_fn fn;
    void *ctx;

    if (put_ip(host, port, &out[0])) {
        return 1;
    }
    pthread_mutex_lock(&g_mu);
    fn = g_dns_fn;
    ctx = g_dns_ctx;
    pthread_mutex_unlock(&g_mu);
    if (fn != NULL) {
        n = fn(ctx, host, port, out, cap);
        if (n < 0) {
            return MS_NC_E_DNS;
        }
        if (n > 0) {
            return n > cap ? cap : n;
        }
    }
    n = map_lookup(cfg->dns_map, host, port, out, cap);
    if (n > 0) {
        return n;
    }
#ifdef __ANDROID__
    if (cfg->use_java_dns) {
        n = java_dns(host, port, out, cap);
        if (n < 0) {
            return MS_NC_E_DNS;
        }
        if (n > 0) {
            return n;
        }
    }
#endif
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    snprintf(ps, sizeof(ps), "%d", port);
    if (getaddrinfo(host, ps, &hints, &ai) != 0 || ai == NULL) {
        return MS_NC_E_DNS;
    }
    for (it = ai; it && n < cap; it = it->ai_next) {
        if (it->ai_addrlen <= sizeof(out[n])) {
            memset(&out[n], 0, sizeof(out[n]));
            memcpy(&out[n], it->ai_addr, it->ai_addrlen);
            n++;
        }
    }
    freeaddrinfo(ai);
    return n > 0 ? n : MS_NC_E_DNS;
}

/* ---------------- TCP ---------------- */
static socklen_t ss_len(const struct sockaddr_storage *ss)
{
    return ss->ss_family == AF_INET6 ? (socklen_t)sizeof(struct sockaddr_in6)
                                     : (socklen_t)sizeof(struct sockaddr_in);
}

static int connect_one(const struct sockaddr_storage *ss, int timeout_ms,
                       volatile int *cancel, int *out_fd)
{
    int fd, r, err = 0, fl;
    socklen_t el = sizeof(err);
    fd = socket(ss->ss_family, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return MS_NC_E_CONNECT;
    }
    fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
#ifdef SO_NOSIGPIPE
    {
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
    }
#endif
    {
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    }
    r = connect(fd, (const struct sockaddr *)ss, ss_len(ss));
    if (r != 0 && errno != EINPROGRESS) {
        close(fd);
        return MS_NC_E_CONNECT;
    }
    if (r != 0) {
        r = ms_nc_wait_fd(fd, 1, timeout_ms, cancel);
        if (r <= 0) {
            close(fd);
            return r == 0 ? MS_NC_E_TIMEOUT : r;
        }
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el) != 0 || err != 0) {
            close(fd);
            return MS_NC_E_CONNECT;
        }
    }
    *out_fd = fd;
    return MS_NC_OK;
}

static int fd_write_all(int fd, const uint8_t *p, int n, int timeout_ms, volatile int *cancel)
{
    int off = 0;
    while (off < n) {
        ssize_t w = send(fd, p + off, (size_t)(n - off), MSG_NOSIGNAL);
        if (w > 0) {
            off += (int)w;
            continue;
        }
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
            int r = ms_nc_wait_fd(fd, 1, timeout_ms, cancel);
            if (r <= 0) {
                return r == 0 ? MS_NC_E_TIMEOUT : r;
            }
            continue;
        }
        return MS_NC_E_IO;
    }
    return n;
}

static int fd_read_some(int fd, uint8_t *p, int cap, int timeout_ms, volatile int *cancel)
{
    for (;;) {
        ssize_t r = recv(fd, p, (size_t)cap, 0);
        if (r >= 0) {
            return (int)r;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            int w = ms_nc_wait_fd(fd, 0, timeout_ms, cancel);
            if (w <= 0) {
                return w == 0 ? MS_NC_E_TIMEOUT : w;
            }
            continue;
        }
        return MS_NC_E_IO;
    }
}

static int fd_read_exact(int fd, uint8_t *p, int n, int timeout_ms, volatile int *cancel)
{
    int off = 0;
    while (off < n) {
        int r = fd_read_some(fd, p + off, n - off, timeout_ms, cancel);
        if (r <= 0) {
            return r == 0 ? MS_NC_E_PROXY : r;
        }
        off += r;
    }
    return n;
}

/* RFC 1928/1929：CONNECT，可选用户名口令。remote_dns 时发域名（ATYP=3）。 */
static int socks5_connect(int fd, const ms_nc_config *cfg, const char *host, int port,
                          const struct sockaddr_storage *dst, int timeout_ms,
                          volatile int *cancel)
{
    uint8_t b[600];
    int n, r, auth = cfg->socks_user[0] != 0;
    b[0] = 5;
    b[1] = auth ? 2 : 1;
    b[2] = 0;
    b[3] = 2;
    if ((r = fd_write_all(fd, b, auth ? 4 : 3, timeout_ms, cancel)) < 0) return r;
    if ((r = fd_read_exact(fd, b, 2, timeout_ms, cancel)) < 0) return r;
    if (b[0] != 5) return MS_NC_E_PROXY;
    if (b[1] == 2) {
        size_t ul = strlen(cfg->socks_user), pl = strlen(cfg->socks_pass);
        if (!auth || ul > 255 || pl > 255) return MS_NC_E_PROXY;
        n = 0;
        b[n++] = 1;
        b[n++] = (uint8_t)ul;
        memcpy(b + n, cfg->socks_user, ul); n += (int)ul;
        b[n++] = (uint8_t)pl;
        memcpy(b + n, cfg->socks_pass, pl); n += (int)pl;
        r = fd_write_all(fd, b, n, timeout_ms, cancel);
        memset(b, 0, sizeof(b)); /* 不在栈上残留口令 */
        if (r < 0) return r;
        if ((r = fd_read_exact(fd, b, 2, timeout_ms, cancel)) < 0) return r;
        if (b[1] != 0) return MS_NC_E_PROXY;
    } else if (b[1] != 0) {
        return MS_NC_E_PROXY;
    }
    n = 0;
    b[n++] = 5; b[n++] = 1; b[n++] = 0;
    if (dst == NULL) {
        size_t hl = strlen(host);
        if (hl == 0 || hl > 255) return MS_NC_E_ARG;
        b[n++] = 3;
        b[n++] = (uint8_t)hl;
        memcpy(b + n, host, hl); n += (int)hl;
    } else if (dst->ss_family == AF_INET) {
        b[n++] = 1;
        memcpy(b + n, &((const struct sockaddr_in *)dst)->sin_addr, 4); n += 4;
    } else {
        b[n++] = 4;
        memcpy(b + n, &((const struct sockaddr_in6 *)dst)->sin6_addr, 16); n += 16;
    }
    b[n++] = (uint8_t)(port >> 8);
    b[n++] = (uint8_t)port;
    if ((r = fd_write_all(fd, b, n, timeout_ms, cancel)) < 0) return r;
    if ((r = fd_read_exact(fd, b, 4, timeout_ms, cancel)) < 0) return r;
    if (b[0] != 5 || b[1] != 0) return MS_NC_E_PROXY;
    switch (b[3]) {
    case 1: n = 4 + 2; break;
    case 4: n = 16 + 2; break;
    case 3:
        if ((r = fd_read_exact(fd, b, 1, timeout_ms, cancel)) < 0) return r;
        n = b[0] + 2;
        break;
    default: return MS_NC_E_PROXY;
    }
    if ((r = fd_read_exact(fd, b, n, timeout_ms, cancel)) < 0) return r;
    return MS_NC_OK;
}

int ms_nc_dial(const char *host, int port, int timeout_ms, volatile int *cancel, int *out_fd)
{
    ms_nc_config cfg;
    struct sockaddr_storage addrs[NC_MAX_ADDRS];
    int n, i, r = MS_NC_E_CONNECT, fd = -1;
    if (host == NULL || host[0] == 0 || port <= 0 || port > 65535 || out_fd == NULL) {
        return MS_NC_E_ARG;
    }
    *out_fd = -1;
    if (timeout_ms <= 0) {
        timeout_ms = 15000;
    }
    ms_nc_get_config(&cfg);
    if (cfg.socks_host[0]) {
        struct sockaddr_storage dst;
        int have_dst = 0;
        if (!cfg.socks_remote_dns) {
            n = resolve(&cfg, host, port, addrs, NC_MAX_ADDRS);
            if (n <= 0) return MS_NC_E_DNS;
            dst = addrs[0];
            have_dst = 1;
        }
        n = resolve(&cfg, cfg.socks_host, cfg.socks_port, addrs, NC_MAX_ADDRS);
        if (n <= 0) return MS_NC_E_PROXY;
        for (i = 0; i < n; i++) {
            r = connect_one(&addrs[i], timeout_ms, cancel, &fd);
            if (r == MS_NC_OK || r == MS_NC_E_CANCELED) break;
        }
        if (r != MS_NC_OK) return r == MS_NC_E_CANCELED ? r : MS_NC_E_PROXY;
        r = socks5_connect(fd, &cfg, host, port, have_dst ? &dst : NULL, timeout_ms, cancel);
        if (r != MS_NC_OK) {
            close(fd);
            return r;
        }
        *out_fd = fd;
        return MS_NC_OK;
    }
    n = resolve(&cfg, host, port, addrs, NC_MAX_ADDRS);
    if (n <= 0) {
        return MS_NC_E_DNS;
    }
    for (i = 0; i < n; i++) {
        r = connect_one(&addrs[i], timeout_ms, cancel, &fd);
        if (r == MS_NC_OK) {
            *out_fd = fd;
            return MS_NC_OK;
        }
        if (r == MS_NC_E_CANCELED) {
            break;
        }
    }
    return r;
}

/* ---------------- TLS 泵 ----------------
 * MS_NC_TLS_OPENSSL（路线 B 主力单 so）：由 ms_nc_tls_ossl.c 提供（极致裁剪 OpenSSL，播放/API 分策略）。
 * 否则 Android 走系统 SSLEngine（JNI），宿主诚实失败。by: Long <mycjzb@gmail.com> */
#if defined(MS_NC_TLS_OPENSSL)
/* 实现见 ms_nc_tls_ossl.c */
#elif defined(__ANDROID__)
int ms_nc_tls_open2(const char *hostname, int verify, void **out_eng)
{
    /* 系统 SSLEngine 后端不提供关闭校验：verify=0 也按严格处理（不放行、不降级）。 */
    (void)verify;
    return ms_nc_tls_open(hostname, out_eng);
}
const char *ms_nc_tls_backend(void) { return "android-system-tls"; }
int ms_nc_tls_set_extra_ca(const char *f, const char *d) { (void)f; (void)d; return MS_NC_E_UNSUPPORTED; }
void ms_nc_tls_global_cleanup(void) {}

int ms_nc_tls_open(const char *hostname, void **out_eng)
{
    return ms_android_tls_open(out_eng, hostname) == 0 ? MS_NC_OK : MS_NC_E_TLS;
}

void ms_nc_tls_close(void *eng)
{
    ms_android_tls_close(eng);
}

static int io_write_all(const ms_nc_io *io, const uint8_t *p, int n)
{
    int off = 0;
    while (off < n) {
        int w = io->wr(io->ctx, p + off, n - off);
        if (w <= 0) {
            return w < 0 ? w : MS_NC_E_IO;
        }
        off += w;
    }
    return n;
}

int ms_nc_tls_handshake(void *eng, const ms_nc_io *io)
{
    uint8_t buf[16384];
    int guard = 0;
    for (;;) {
        int n = 0, st, r;
        if (++guard > 4096) return MS_NC_E_TLS;
        st = ms_android_tls_handshake(eng, buf, (int)sizeof(buf), &n);
        if (st == 0) return MS_NC_OK;
        if (st == 2) {
            if (n > 0 && (r = io_write_all(io, buf, n)) < 0) return r;
            continue;
        }
        if (st == 1) {
            r = io->rd(io->ctx, buf, (int)sizeof(buf));
            if (r <= 0) return r < 0 ? r : MS_NC_E_TLS;
            if (ms_android_tls_feed_net(eng, buf, r) < 0) return MS_NC_E_TLS;
            continue;
        }
        return MS_NC_E_TLS; /* 证书/主机名校验失败都在这里，不重试、不降级 */
    }
}

int ms_nc_tls_read(void *eng, const ms_nc_io *io, uint8_t *buf, int cap)
{
    uint8_t net[16384];
    int guard = 0;
    for (;;) {
        int net_n = 0, need = 0, r;
        if (++guard > 4096) return MS_NC_E_TLS;
        r = ms_android_tls_read(eng, buf, cap, net, (int)sizeof(net), &net_n, &need);
        if (r > 0) return r;
        if (r < 0) return MS_NC_E_TLS;
        if (net_n > 0) {
            int w = io_write_all(io, net, net_n);
            if (w < 0) return w;
        }
        if (need == 1) {
            int g = io->rd(io->ctx, net, (int)sizeof(net));
            if (g == 0) return 0; /* 对端关闭 */
            if (g < 0) return g;
            if (ms_android_tls_feed_net(eng, net, g) < 0) return MS_NC_E_TLS;
        } else if (need != 2) {
            return 0; /* 引擎已关闭（close_notify） */
        }
    }
}

int ms_nc_tls_write(void *eng, const ms_nc_io *io, const uint8_t *buf, int n)
{
    uint8_t net[16384 + 2048];
    int off = 0;
    while (off < n) {
        int net_n = 0, chunk = n - off, used;
        if (chunk > 16384) chunk = 16384;
        used = ms_android_tls_write(eng, buf + off, chunk, net, (int)sizeof(net), &net_n);
        if (used < 0) return MS_NC_E_TLS;
        if (net_n > 0) {
            int w = io_write_all(io, net, net_n);
            if (w < 0) return w;
        }
        if (used == 0 && net_n == 0) return MS_NC_E_TLS;
        off += used;
    }
    return n;
}
#else
/* 宿主：无系统 TLS。诚实失败，不降级明文。 */
int ms_nc_tls_open2(const char *hostname, int verify, void **out_eng)
{
    (void)verify;
    return ms_nc_tls_open(hostname, out_eng);
}
const char *ms_nc_tls_backend(void) { return "none"; }
int ms_nc_tls_set_extra_ca(const char *f, const char *d) { (void)f; (void)d; return MS_NC_E_UNSUPPORTED; }
void ms_nc_tls_global_cleanup(void) {}
int ms_nc_tls_open(const char *hostname, void **out_eng)
{
    (void)hostname;
    if (out_eng) *out_eng = NULL;
    return MS_NC_E_TLS;
}
void ms_nc_tls_close(void *eng) { (void)eng; }
int ms_nc_tls_handshake(void *eng, const ms_nc_io *io) { (void)eng; (void)io; return MS_NC_E_TLS; }
int ms_nc_tls_read(void *eng, const ms_nc_io *io, uint8_t *b, int c) { (void)eng; (void)io; (void)b; (void)c; return MS_NC_E_TLS; }
int ms_nc_tls_write(void *eng, const ms_nc_io *io, const uint8_t *b, int n) { (void)eng; (void)io; (void)b; (void)n; return MS_NC_E_TLS; }
#endif

/* ---------------- 连接对象 ---------------- */
struct ms_nc_conn {
    int fd;
    void *tls;
    int io_timeout_ms;
    volatile int *cancel;
    ms_nc_io io;
};

static int conn_raw_rd(void *ctx, uint8_t *buf, int cap)
{
    ms_nc_conn *c = (ms_nc_conn *)ctx;
    return fd_read_some(c->fd, buf, cap, c->io_timeout_ms, c->cancel);
}

static int conn_raw_wr(void *ctx, const uint8_t *buf, int n)
{
    ms_nc_conn *c = (ms_nc_conn *)ctx;
    return fd_write_all(c->fd, buf, n, c->io_timeout_ms, c->cancel);
}

int ms_nc_conn_open(const char *host, int port, int use_tls, int timeout_ms,
                    int io_timeout_ms, volatile int *cancel, ms_nc_conn **out)
{
    ms_nc_conn *c;
    int r;
    if (out == NULL) return MS_NC_E_ARG;
    *out = NULL;
    c = (ms_nc_conn *)calloc(1, sizeof(*c));
    if (c == NULL) return MS_NC_E_NOMEM;
    c->fd = -1;
    c->io_timeout_ms = io_timeout_ms > 0 ? io_timeout_ms : 15000;
    c->cancel = cancel;
    c->io.ctx = c;
    c->io.rd = conn_raw_rd;
    c->io.wr = conn_raw_wr;
    r = ms_nc_dial(host, port, timeout_ms, cancel, &c->fd);
    if (r != MS_NC_OK) {
        free(c);
        return r;
    }
    if (use_tls) {
        r = ms_nc_tls_open(host, &c->tls);
        if (r == MS_NC_OK) {
            r = ms_nc_tls_handshake(c->tls, &c->io);
        }
        if (r != MS_NC_OK) {
            ms_nc_conn_close(c);
            return canceled(cancel) ? MS_NC_E_CANCELED : (r == MS_NC_E_TIMEOUT ? r : MS_NC_E_TLS);
        }
    }
    *out = c;
    return MS_NC_OK;
}

int ms_nc_conn_read(ms_nc_conn *c, uint8_t *buf, int cap)
{
    if (c == NULL || buf == NULL || cap <= 0) return MS_NC_E_ARG;
    if (c->tls) return ms_nc_tls_read(c->tls, &c->io, buf, cap);
    return conn_raw_rd(c, buf, cap);
}

int ms_nc_conn_write_all(ms_nc_conn *c, const uint8_t *buf, int n)
{
    if (c == NULL || (buf == NULL && n > 0)) return MS_NC_E_ARG;
    if (n == 0) return 0;
    if (c->tls) return ms_nc_tls_write(c->tls, &c->io, buf, n);
    return conn_raw_wr(c, buf, n);
}

void ms_nc_conn_shutdown(ms_nc_conn *c)
{
    if (c != NULL && c->fd >= 0) {
        shutdown(c->fd, SHUT_RDWR);
    }
}

void ms_nc_conn_close(ms_nc_conn *c)
{
    if (c == NULL) return;
    if (c->tls) ms_nc_tls_close(c->tls);
    if (c->fd >= 0) close(c->fd);
    free(c);
}

/* ---------------- HTTP/1.1 ---------------- */
struct ms_nc_http {
    ms_nc_conn *conn;
    int keep_alive;          /* 本次请求按 Keep-Alive 发送 */
    int pooled_conn;         /* conn 来自连接池（服务端可能已关，失败可重拨一次） */
    char pool_host[256];
    int pool_port, pool_https;
    volatile int cancel_own;
    volatile int *cancelp;   /* 指向外部或自有取消标志 */
    int status;
    char *headers;
    int64_t content_length;
    int64_t remaining;      /* 定长剩余；-1 读到关闭 */
    int chunked;
    int64_t chunk_left;     /* 当前块剩余；-1 需读块头 */
    int body_done;
    uint8_t buf[16384];     /* 已收但未消费的字节 */
    int buf_off, buf_len;
    int read_timeout_ms;
};

typedef struct nc_url {
    int https;
    char host[256];
    int port;
    char path[NC_URL_MAX];
} nc_url;

static int parse_url(const char *url, nc_url *u)
{
    const char *p, *hs, *he, *ps;
    size_t hl;
    memset(u, 0, sizeof(*u));
    if (strncasecmp(url, "http://", 7) == 0) {
        p = url + 7;
    } else if (strncasecmp(url, "https://", 8) == 0) {
        p = url + 8;
        u->https = 1;
    } else {
        return MS_NC_E_UNSUPPORTED;
    }
    for (hs = url; *hs; hs++) {
        if ((unsigned char)*hs < 0x21 || *hs == 0x7f) return MS_NC_E_ARG; /* 禁止空白/控制字符 */
    }
    hs = p;
    ps = p + strcspn(p, "/?#");
    {
        const char *at = memchr(hs, '@', (size_t)(ps - hs));
        if (at) return MS_NC_E_UNSUPPORTED; /* 不支持 URL 内嵌凭据，避免泄露 */
    }
    if (*hs == '[') {
        he = memchr(hs, ']', (size_t)(ps - hs));
        if (he == NULL) return MS_NC_E_ARG;
        hl = (size_t)(he - hs - 1);
        if (hl == 0 || hl >= sizeof(u->host)) return MS_NC_E_ARG;
        memcpy(u->host, hs + 1, hl);
        he++;
    } else {
        he = memchr(hs, ':', (size_t)(ps - hs));
        if (he == NULL) he = ps;
        hl = (size_t)(he - hs);
        if (hl == 0 || hl >= sizeof(u->host)) return MS_NC_E_ARG;
        memcpy(u->host, hs, hl);
    }
    u->port = u->https ? 443 : 80;
    if (he < ps && *he == ':') {
        char *e;
        long v = strtol(he + 1, &e, 10);
        if (e != ps || v <= 0 || v > 65535) return MS_NC_E_ARG;
        u->port = (int)v;
    }
    if (*ps == 0 || *ps == '#') {
        snprintf(u->path, sizeof(u->path), "/");
    } else if (*ps == '?') {
        if (snprintf(u->path, sizeof(u->path), "/%.*s", (int)strcspn(ps, "#"), ps) >= (int)sizeof(u->path))
            return MS_NC_E_ARG;
    } else {
        size_t pl = strcspn(ps, "#");
        if (pl >= sizeof(u->path)) return MS_NC_E_ARG;
        memcpy(u->path, ps, pl);
        u->path[pl] = 0;
    }
    return MS_NC_OK;
}

static int hdr_has(const char *headers, const char *name)
{
    size_t nl = strlen(name);
    const char *p = headers;
    while (p && *p) {
        if (strncasecmp(p, name, nl) == 0 && p[nl] == ':') return 1;
        p = strchr(p, '\n');
        if (p) p++;
    }
    return 0;
}

/* 用户头：逐行 "K: V"，禁止 CR、禁止无冒号行、禁止覆盖 Host/Content-Length/Transfer-Encoding。 */
static int append_user_headers(char *dst, size_t cap, size_t *len, const char *headers)
{
    const char *p = headers;
    while (p && *p) {
        const char *e = strchr(p, '\n');
        size_t l = e ? (size_t)(e - p) : strlen(p);
        if (l > 0) {
            const char *colon = memchr(p, ':', l);
            size_t i;
            if (colon == NULL || colon == p) return MS_NC_E_ARG;
            for (i = 0; i < l; i++) {
                unsigned char ch = (unsigned char)p[i];
                if (ch == '\r' || ch == 0 || (ch < 0x20 && ch != '\t')) return MS_NC_E_ARG;
            }
            if ((size_t)(colon - p) == 4 && strncasecmp(p, "Host", 4) == 0) return MS_NC_E_ARG;
            if ((size_t)(colon - p) == 14 && strncasecmp(p, "Content-Length", 14) == 0) return MS_NC_E_ARG;
            if ((size_t)(colon - p) == 17 && strncasecmp(p, "Transfer-Encoding", 17) == 0) return MS_NC_E_ARG;
            if (*len + l + 2 >= cap) return MS_NC_E_ARG;
            memcpy(dst + *len, p, l);
            *len += l;
            dst[(*len)++] = '\r';
            dst[(*len)++] = '\n';
        }
        p = e ? e + 1 : NULL;
    }
    return MS_NC_OK;
}

static int fill_buf(ms_nc_http *h)
{
    int r;
    if (h->buf_off > 0 && h->buf_off == h->buf_len) {
        h->buf_off = h->buf_len = 0;
    }
    if (h->buf_len == (int)sizeof(h->buf)) {
        if (h->buf_off == 0) return MS_NC_E_PROTO;
        memmove(h->buf, h->buf + h->buf_off, (size_t)(h->buf_len - h->buf_off));
        h->buf_len -= h->buf_off;
        h->buf_off = 0;
    }
    r = ms_nc_conn_read(h->conn, h->buf + h->buf_len, (int)sizeof(h->buf) - h->buf_len);
    if (r > 0) h->buf_len += r;
    if (r < 0 && canceled(h->cancelp)) return MS_NC_E_CANCELED;
    return r;
}

/* 读一行（不含 CRLF）到 line，最长 cap-1。 */
static int read_line(ms_nc_http *h, char *line, int cap)
{
    for (;;) {
        int i;
        for (i = h->buf_off; i < h->buf_len; i++) {
            if (h->buf[i] == '\n') {
                int l = i - h->buf_off;
                if (l > 0 && h->buf[i - 1] == '\r') l--;
                if (l >= cap) return MS_NC_E_PROTO;
                memcpy(line, h->buf + h->buf_off, (size_t)l);
                line[l] = 0;
                h->buf_off = i + 1;
                return l;
            }
        }
        if (h->buf_len - h->buf_off >= cap) return MS_NC_E_PROTO;
        i = fill_buf(h);
        if (i == 0) return MS_NC_E_PROTO;
        if (i < 0) return i;
    }
}

static const char *hdr_get(const char *hdrs, const char *name, size_t *vlen)
{
    size_t nl = strlen(name);
    const char *p = hdrs;
    while (p && *p) {
        const char *e = strstr(p, "\r\n");
        if (strncasecmp(p, name, nl) == 0 && p[nl] == ':') {
            const char *v = p + nl + 1;
            while (*v == ' ' || *v == '\t') v++;
            *vlen = e ? (size_t)(e - v) : strlen(v);
            return v;
        }
        p = e ? e + 2 : NULL;
    }
    return NULL;
}

static int read_response_head(ms_nc_http *h)
{
    char line[8192];
    size_t hlen = 0, cap = 4096;
    int r, v1, v2, code;
    do {
        r = read_line(h, line, (int)sizeof(line));
        if (r < 0) return r;
        if (sscanf(line, "HTTP/%d.%d %d", &v1, &v2, &code) != 3 || code < 100 || code > 999) {
            return MS_NC_E_PROTO;
        }
        free(h->headers);
        h->headers = (char *)malloc(cap);
        if (h->headers == NULL) return MS_NC_E_NOMEM;
        hlen = 0;
        h->headers[0] = 0;
        for (;;) {
            r = read_line(h, line, (int)sizeof(line));
            if (r < 0) return r;
            if (r == 0) break;
            if (hlen + (size_t)r + 3 > NC_HDR_MAX) return MS_NC_E_PROTO;
            if (hlen + (size_t)r + 3 > cap) {
                char *nb;
                while (hlen + (size_t)r + 3 > cap) cap *= 2;
                nb = (char *)realloc(h->headers, cap);
                if (nb == NULL) return MS_NC_E_NOMEM;
                h->headers = nb;
            }
            memcpy(h->headers + hlen, line, (size_t)r);
            hlen += (size_t)r;
            h->headers[hlen++] = '\r';
            h->headers[hlen++] = '\n';
            h->headers[hlen] = 0;
        }
    } while (code >= 100 && code < 200); /* 跳过 1xx */
    h->status = code;
    return MS_NC_OK;
}

static void setup_body(ms_nc_http *h, const char *method)
{
    size_t vl = 0;
    const char *v;
    h->content_length = -1;
    h->remaining = -1;
    h->chunked = 0;
    h->chunk_left = -1;
    h->body_done = 0;
    if (strcasecmp(method, "HEAD") == 0 || h->status == 204 || h->status == 304) {
        h->body_done = 1;
        h->content_length = 0;
        return;
    }
    v = hdr_get(h->headers, "Transfer-Encoding", &vl);
    if (v && vl >= 7 && strncasecmp(v + vl - 7, "chunked", 7) == 0) {
        h->chunked = 1;
        return;
    }
    v = hdr_get(h->headers, "Content-Length", &vl);
    if (v) {
        char tmp[32];
        char *e;
        long long n;
        if (vl == 0 || vl >= sizeof(tmp)) return;
        memcpy(tmp, v, vl);
        tmp[vl] = 0;
        n = strtoll(tmp, &e, 10);
        if (*e == 0 && n >= 0) {
            h->content_length = n;
            h->remaining = n;
            if (n == 0) h->body_done = 1;
        }
    }
}

/* ---- Keep-Alive 连接池：最多 NC_POOL_MAX 条空闲连接，空闲超过 NC_POOL_IDLE_MS 丢弃 ---- */
#define NC_POOL_MAX 4
#define NC_POOL_IDLE_MS 15000
typedef struct nc_pool_ent {
    ms_nc_conn *conn;
    char host[256];
    int port, https;
    int64_t since_ms;
} nc_pool_ent;
static nc_pool_ent g_pool[NC_POOL_MAX];
static pthread_mutex_t g_pool_mu = PTHREAD_MUTEX_INITIALIZER;
static int g_pool_opened, g_pool_reused;

static int64_t nc_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static ms_nc_conn *pool_take(const char *host, int port, int https)
{
    int i;
    ms_nc_conn *c = NULL, *stale[NC_POOL_MAX];
    int ns = 0;
    int64_t now = nc_now_ms();
    pthread_mutex_lock(&g_pool_mu);
    for (i = 0; i < NC_POOL_MAX; i++) {
        nc_pool_ent *e = &g_pool[i];
        if (!e->conn) continue;
        if (now - e->since_ms > NC_POOL_IDLE_MS) {
            stale[ns++] = e->conn;
            e->conn = NULL;
            continue;
        }
        if (!c && e->port == port && e->https == https && strcmp(e->host, host) == 0) {
            c = e->conn;
            e->conn = NULL;
        }
    }
    if (c) g_pool_reused++;
    pthread_mutex_unlock(&g_pool_mu);
    for (i = 0; i < ns; i++) ms_nc_conn_close(stale[i]);
    return c;
}

static void pool_put(ms_nc_conn *c, const char *host, int port, int https)
{
    int i, slot = -1;
    int64_t oldest = 0;
    ms_nc_conn *evict = NULL;
    c->cancel = NULL; /* 旧请求的取消标志随请求释放，回池前断开 */
    pthread_mutex_lock(&g_pool_mu);
    for (i = 0; i < NC_POOL_MAX; i++) {
        if (!g_pool[i].conn) { slot = i; break; }
        if (slot < 0 || g_pool[i].since_ms < oldest) { slot = i; oldest = g_pool[i].since_ms; }
    }
    evict = g_pool[slot].conn;
    g_pool[slot].conn = c;
    snprintf(g_pool[slot].host, sizeof(g_pool[slot].host), "%s", host);
    g_pool[slot].port = port;
    g_pool[slot].https = https;
    g_pool[slot].since_ms = nc_now_ms();
    pthread_mutex_unlock(&g_pool_mu);
    if (evict) ms_nc_conn_close(evict);
}

void ms_nc_pool_stats(int *opened, int *reused)
{
    pthread_mutex_lock(&g_pool_mu);
    if (opened) *opened = g_pool_opened;
    if (reused) *reused = g_pool_reused;
    pthread_mutex_unlock(&g_pool_mu);
}

void ms_nc_pool_clear(void)
{
    int i;
    ms_nc_conn *all[NC_POOL_MAX];
    pthread_mutex_lock(&g_pool_mu);
    for (i = 0; i < NC_POOL_MAX; i++) { all[i] = g_pool[i].conn; g_pool[i].conn = NULL; }
    pthread_mutex_unlock(&g_pool_mu);
    for (i = 0; i < NC_POOL_MAX; i++) ms_nc_conn_close(all[i]);
}

static int http_once(ms_nc_http *h, const ms_nc_req *req, const char *method,
                     const nc_url *u, int send_body)
{
    char *rq;
    size_t len = 0, cap = 4096 + strlen(u->path) + (req->headers ? strlen(req->headers) : 0);
    int r, ct = req->connect_timeout_ms > 0 ? req->connect_timeout_ms : 15000;
    char hostport[300];
    rq = (char *)malloc(cap);
    if (rq == NULL) return MS_NC_E_NOMEM;
    if (strchr(u->host, ':')) {
        snprintf(hostport, sizeof(hostport), "[%s]", u->host);
    } else {
        snprintf(hostport, sizeof(hostport), "%s", u->host);
    }
    if ((u->https && u->port != 443) || (!u->https && u->port != 80)) {
        size_t hl = strlen(hostport);
        snprintf(hostport + hl, sizeof(hostport) - hl, ":%d", u->port);
    }
    len = (size_t)snprintf(rq, cap, "%s %s HTTP/1.1\r\nHost: %s\r\n", method, u->path, hostport);
    if (req->headers && (r = append_user_headers(rq, cap - 256, &len, req->headers)) != MS_NC_OK) {
        free(rq);
        return r;
    }
    if (!req->headers || !hdr_has(req->headers, "User-Agent")) {
        len += (size_t)snprintf(rq + len, cap - len, "User-Agent: MsMedia/1.0\r\n");
    }
    if (!req->headers || !hdr_has(req->headers, "Accept-Encoding")) {
        len += (size_t)snprintf(rq + len, cap - len, "Accept-Encoding: identity\r\n");
    }
    if (send_body && (req->body_len > 0 || strcasecmp(method, "POST") == 0 ||
                      strcasecmp(method, "PUT") == 0 || strcasecmp(method, "PATCH") == 0)) {
        len += (size_t)snprintf(rq + len, cap - len, "Content-Length: %zu\r\n", send_body ? req->body_len : 0);
    }
    len += (size_t)snprintf(rq + len, cap - len, h->keep_alive ? "Connection: keep-alive\r\n\r\n"
                                                                    : "Connection: close\r\n\r\n");
    if (len >= cap) {
        free(rq);
        return MS_NC_E_ARG;
    }
    h->pooled_conn = 0;
    if (h->keep_alive && !(send_body && req->body_len > 0)) {
        h->conn = pool_take(u->host, u->port, u->https);
        if (h->conn) {
            h->pooled_conn = 1;
            h->conn->cancel = h->cancelp;
            h->conn->io_timeout_ms = h->read_timeout_ms;
        }
    }
    if (!h->conn) {
        r = ms_nc_conn_open(u->host, u->port, u->https, ct, h->read_timeout_ms, h->cancelp, &h->conn);
        if (r != MS_NC_OK) {
            free(rq);
            return r;
        }
        pthread_mutex_lock(&g_pool_mu);
        g_pool_opened++;
        pthread_mutex_unlock(&g_pool_mu);
    }
    snprintf(h->pool_host, sizeof(h->pool_host), "%s", u->host);
    h->pool_port = u->port;
    h->pool_https = u->https;
    r = ms_nc_conn_write_all(h->conn, (const uint8_t *)rq, (int)len);
    if (r < 0 && h->pooled_conn && !canceled(h->cancelp)) {
        /* 池里的连接已被服务端关闭：重拨一次 */
        ms_nc_conn_close(h->conn);
        h->conn = NULL;
        h->pooled_conn = 0;
        r = ms_nc_conn_open(u->host, u->port, u->https, ct, h->read_timeout_ms, h->cancelp, &h->conn);
        if (r == MS_NC_OK) {
            pthread_mutex_lock(&g_pool_mu);
            g_pool_opened++;
            pthread_mutex_unlock(&g_pool_mu);
            r = ms_nc_conn_write_all(h->conn, (const uint8_t *)rq, (int)len);
        }
    }
    if (r < 0 || h->conn == NULL) {
        free(rq);
        return canceled(h->cancelp) ? MS_NC_E_CANCELED : (r < 0 ? r : MS_NC_E_CONNECT);
    }
    if (send_body && req->body_len > 0) {
        size_t off = 0;
        while (off < req->body_len) {
            int chunk = (int)((req->body_len - off) > 65536 ? 65536 : (req->body_len - off));
            r = ms_nc_conn_write_all(h->conn, req->body + off, chunk);
            if (r < 0) {
                free(rq);
                return canceled(h->cancelp) ? MS_NC_E_CANCELED : r;
            }
            off += (size_t)chunk;
        }
    }
    r = read_response_head(h);
    if (r != MS_NC_OK && h->pooled_conn && !canceled(h->cancelp) && h->buf_len == 0) {
        /* 复用连接在读响应前被对端关闭（典型 keep-alive 竞态）：新连接重发一次，无请求体才重发 */
        ms_nc_conn_close(h->conn);
        h->conn = NULL;
        h->pooled_conn = 0;
        h->buf_off = h->buf_len = 0;
        r = ms_nc_conn_open(u->host, u->port, u->https, ct, h->read_timeout_ms, h->cancelp, &h->conn);
        if (r == MS_NC_OK) {
            pthread_mutex_lock(&g_pool_mu);
            g_pool_opened++;
            pthread_mutex_unlock(&g_pool_mu);
            r = ms_nc_conn_write_all(h->conn, (const uint8_t *)rq, (int)len);
            if (r >= 0) r = read_response_head(h);
        }
    }
    free(rq);
    if (r != MS_NC_OK) return canceled(h->cancelp) ? MS_NC_E_CANCELED : r;
    setup_body(h, method);
    return MS_NC_OK;
}

/* 相对 Location 解析：绝对 URL 原样；"/" 开头换路径；其余相对当前目录。 */
static int resolve_location(const nc_url *base, const char *loc, size_t ll, char *out, size_t cap)
{
    char tmp[NC_URL_MAX];
    if (ll == 0 || ll >= sizeof(tmp)) return MS_NC_E_REDIRECT;
    memcpy(tmp, loc, ll);
    tmp[ll] = 0;
    if (strncasecmp(tmp, "http://", 7) == 0 || strncasecmp(tmp, "https://", 8) == 0) {
        return snprintf(out, cap, "%s", tmp) < (int)cap ? MS_NC_OK : MS_NC_E_REDIRECT;
    }
    {
        char hp[300];
        if (strchr(base->host, ':')) snprintf(hp, sizeof(hp), "[%s]:%d", base->host, base->port);
        else snprintf(hp, sizeof(hp), "%s:%d", base->host, base->port);
        if (tmp[0] == '/' && tmp[1] == '/') {
            return snprintf(out, cap, "%s:%s", base->https ? "https" : "http", tmp) < (int)cap ? MS_NC_OK : MS_NC_E_REDIRECT;
        }
        if (tmp[0] == '/') {
            return snprintf(out, cap, "%s://%s%s", base->https ? "https" : "http", hp, tmp) < (int)cap ? MS_NC_OK : MS_NC_E_REDIRECT;
        }
        {
            const char *slash = strrchr(base->path, '/');
            int dl = slash ? (int)(slash - base->path) + 1 : 1;
            return snprintf(out, cap, "%s://%s%.*s%s", base->https ? "https" : "http", hp, dl,
                            slash ? base->path : "/", tmp) < (int)cap ? MS_NC_OK : MS_NC_E_REDIRECT;
        }
    }
}

int ms_nc_http_open(const ms_nc_req *req, ms_nc_http **out)
{
    ms_nc_http *h;
    char *url;
    const char *method;
    int hops = 0, max_hops, r, send_body = 1, was_https = 0;
    nc_url *u;
    if (req == NULL || req->url == NULL || out == NULL) return MS_NC_E_ARG;
    *out = NULL;
    method = req->method ? req->method : "GET";
    {
        const char *m;
        if (method[0] == 0 || strlen(method) > 16) return MS_NC_E_ARG;
        for (m = method; *m; m++) if (*m < 'A' || *m > 'Z') return MS_NC_E_ARG;
    }
    if (strlen(req->url) >= NC_URL_MAX) return MS_NC_E_ARG;
    max_hops = req->max_redirects < 0 ? 5 : req->max_redirects;
    h = (ms_nc_http *)calloc(1, sizeof(*h));
    url = (char *)malloc(NC_URL_MAX);
    u = (nc_url *)malloc(sizeof(*u));
    if (h == NULL || url == NULL || u == NULL) {
        free(h); free(url); free(u);
        return MS_NC_E_NOMEM;
    }
    h->read_timeout_ms = req->read_timeout_ms > 0 ? req->read_timeout_ms : 15000;
    h->cancelp = req->cancel ? req->cancel : &h->cancel_own;
    h->keep_alive = req->keep_alive ? 1 : 0;
    snprintf(url, NC_URL_MAX, "%s", req->url);
    *out = h; /* 先交出句柄：打开过程中别的线程即可 cancel */
    for (;;) {
        size_t ll = 0;
        const char *loc;
        r = parse_url(url, u);
        if (r != MS_NC_OK) break;
        if (was_https && !u->https) {
            r = MS_NC_E_REDIRECT; /* 禁止 https -> http 降级 */
            break;
        }
        was_https = was_https || u->https;
        r = http_once(h, req, method, u, send_body);
        if (r != MS_NC_OK) break;
        if (!(h->status == 301 || h->status == 302 || h->status == 303 ||
              h->status == 307 || h->status == 308) || max_hops == 0) {
            break;
        }
        loc = hdr_get(h->headers, "Location", &ll);
        if (loc == NULL) break;
        if (++hops > max_hops) {
            r = MS_NC_E_REDIRECT;
            break;
        }
        {
            char *nu = (char *)malloc(NC_URL_MAX);
            if (nu == NULL) { r = MS_NC_E_NOMEM; break; }
            r = resolve_location(u, loc, ll, nu, NC_URL_MAX);
            if (r == MS_NC_OK) memcpy(url, nu, NC_URL_MAX);
            free(nu);
            if (r != MS_NC_OK) break;
        }
        if (h->status == 303 || ((h->status == 301 || h->status == 302) &&
                                 strcasecmp(method, "POST") == 0)) {
            method = "GET";
            send_body = 0;
        }
        ms_nc_conn_close(h->conn);
        h->conn = NULL;
        h->buf_off = h->buf_len = 0;
        if (canceled(h->cancelp)) { r = MS_NC_E_CANCELED; break; }
    }
    free(url);
    free(u);
    if (r != MS_NC_OK) {
        *out = NULL;
        ms_nc_http_close(h);
        return r;
    }
    return MS_NC_OK;
}

int ms_nc_http_status(const ms_nc_http *h) { return h ? h->status : 0; }
const char *ms_nc_http_headers(const ms_nc_http *h) { return (h && h->headers) ? h->headers : ""; }
int64_t ms_nc_http_content_length(const ms_nc_http *h) { return h ? h->content_length : -1; }

static int take_buffered(ms_nc_http *h, uint8_t *buf, int cap)
{
    int n = h->buf_len - h->buf_off;
    if (n > cap) n = cap;
    if (n > 0) {
        memcpy(buf, h->buf + h->buf_off, (size_t)n);
        h->buf_off += n;
    }
    return n;
}

int ms_nc_http_read(ms_nc_http *h, uint8_t *buf, int cap)
{
    int r;
    if (h == NULL || buf == NULL || cap <= 0) return MS_NC_E_ARG;
    if (canceled(h->cancelp)) return MS_NC_E_CANCELED;
    if (h->body_done) return 0;
    if (h->chunked) {
        if (h->chunk_left <= 0) {
            char line[256];
            char *e;
            unsigned long long n;
            if (h->chunk_left == 0) {
                r = read_line(h, line, (int)sizeof(line)); /* 块尾 CRLF */
                if (r < 0) return r;
            }
            r = read_line(h, line, (int)sizeof(line));
            if (r < 0) return r;
            n = strtoull(line, &e, 16);
            if (e == line) return MS_NC_E_PROTO;
            if (n == 0) {
                /* 跳过 trailer 直到空行 */
                do { r = read_line(h, line, (int)sizeof(line)); } while (r > 0);
                h->body_done = 1;
                return r < 0 ? r : 0;
            }
            h->chunk_left = (int64_t)n;
        }
        if (cap > h->chunk_left) cap = (int)h->chunk_left;
        if (h->buf_off == h->buf_len) {
            r = fill_buf(h);
            if (r == 0) return MS_NC_E_PROTO;
            if (r < 0) return r;
        }
        r = take_buffered(h, buf, cap);
        h->chunk_left -= r;
        return r;
    }
    if (h->remaining >= 0 && cap > h->remaining) cap = (int)h->remaining;
    if (h->buf_off < h->buf_len) {
        r = take_buffered(h, buf, cap);
    } else {
        r = ms_nc_conn_read(h->conn, buf, cap);
        if (r < 0 && canceled(h->cancelp)) return MS_NC_E_CANCELED;
        if (r == 0) {
            h->body_done = 1;
            return h->remaining > 0 ? MS_NC_E_PROTO : 0; /* 定长未读满即截断 */
        }
        if (r < 0) return r;
    }
    if (h->remaining >= 0) {
        h->remaining -= r;
        if (h->remaining == 0) h->body_done = 1;
    }
    return r;
}

void ms_nc_http_cancel(ms_nc_http *h)
{
    if (h == NULL) return;
    __atomic_store_n(h->cancelp, 1, __ATOMIC_RELEASE);
    /* 不在这里动 conn（另一线程可能正在 close）；poll 分片 100ms 内会看到取消标志。 */
}

/* 响应体已完整读完、服务端未要求关闭、缓冲无多余字节、未取消 → 连接可回池复用。 */
static int http_reusable(const ms_nc_http *h)
{
    size_t vl = 0;
    const char *v;
    if (!h->keep_alive || !h->conn || !h->body_done || h->buf_off != h->buf_len) return 0;
    if (h->cancelp && __atomic_load_n(h->cancelp, __ATOMIC_ACQUIRE)) return 0;
    if (!h->chunked && h->content_length < 0) return 0; /* 读到关闭才结束的响应不能复用 */
    v = hdr_get(h->headers ? h->headers : "", "Connection", &vl);
    if (v && vl >= 5 && strncasecmp(v, "close", 5) == 0) return 0;
    return 1;
}

void ms_nc_http_close(ms_nc_http *h)
{
    if (h == NULL) return;
    if (http_reusable(h)) {
        pool_put(h->conn, h->pool_host, h->pool_port, h->pool_https);
        h->conn = NULL;
    }
    ms_nc_conn_close(h->conn);
    free(h->headers);
    free(h);
}

/* ---------------- Android JNI：缓存应用类 + Java DNS ---------------- */
#ifdef __ANDROID__
#include "../hw/ms_hw_jni.h"

static jclass g_tls_cls;
static jclass g_net_cls;
static jmethodID g_resolve_mid;

int ms_nc_jni_init(JNIEnv *env)
{
    jclass c;
    if (env == NULL) return -1;
    pthread_mutex_lock(&g_mu);
    if (g_tls_cls == NULL) {
        c = (*env)->FindClass(env, "com/msmedia/MsSystemTls");
        if (c) {
            g_tls_cls = (jclass)(*env)->NewGlobalRef(env, c);
            (*env)->DeleteLocalRef(env, c);
        } else {
            (*env)->ExceptionClear(env);
        }
    }
    if (g_net_cls == NULL) {
        c = (*env)->FindClass(env, "com/msmedia/net/MsHttpClient");
        if (c) {
            g_net_cls = (jclass)(*env)->NewGlobalRef(env, c);
            g_resolve_mid = (*env)->GetStaticMethodID(env, g_net_cls, "dnsResolveFromNative",
                                                      "(Ljava/lang/String;)[Ljava/lang/String;");
            if (g_resolve_mid == NULL) (*env)->ExceptionClear(env);
            (*env)->DeleteLocalRef(env, c);
        } else {
            (*env)->ExceptionClear(env);
        }
    }
    pthread_mutex_unlock(&g_mu);
    return g_tls_cls ? 0 : -1;
}

jclass ms_nc_jni_tls_class(void)
{
    return g_tls_cls;
}

static int java_dns(const char *host, int port, struct sockaddr_storage *out, int cap)
{
    JNIEnv *env = NULL;
    jstring jh;
    jobjectArray arr;
    int n = 0, i, len;
    if (g_net_cls == NULL || g_resolve_mid == NULL) return 0;
    if (!ms_hw_jni_has_vm() || !ms_hw_jni_env(&env) || env == NULL) return 0;
    jh = (*env)->NewStringUTF(env, host);
    if (jh == NULL) { (*env)->ExceptionClear(env); return 0; }
    arr = (jobjectArray)(*env)->CallStaticObjectMethod(env, g_net_cls, g_resolve_mid, jh);
    (*env)->DeleteLocalRef(env, jh);
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        return MS_NC_E_DNS;
    }
    if (arr == NULL) return 0;
    len = (*env)->GetArrayLength(env, arr);
    for (i = 0; i < len && n < cap; i++) {
        jstring s = (jstring)(*env)->GetObjectArrayElement(env, arr, i);
        if (s) {
            const char *ip = (*env)->GetStringUTFChars(env, s, NULL);
            if (ip) {
                n += put_ip(ip, port, &out[n]);
                (*env)->ReleaseStringUTFChars(env, s, ip);
            }
            (*env)->DeleteLocalRef(env, s);
        }
    }
    (*env)->DeleteLocalRef(env, arr);
    return n;
}
#endif
