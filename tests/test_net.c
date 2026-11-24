/*
 * 可选网络库受控自测：本机 HTTP/HTTPS、状态码、分片、Range、
 * 证书失败、错误主机名、超时、取消，以及 HLS / DASH 经受控 HTTP 出 AU。
 * 测试 CA 只由构建脚本写入 build 目录。非正式验收。
 * by: Long <mycjzb@gmail.com>
 */
#define _DEFAULT_SOURCE
#include "msmedia/msmedia.h"
#include "msmedia/ms_net.h"
#include "ms_ts_synth.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static int failed;

#define CHECK(cond, msg)                                                       \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, msg);      \
            failed = 1;                                                        \
            return 1;                                                          \
        }                                                                      \
    } while (0)

#define CHECK_ST(st, exp)                                                      \
    do {                                                                       \
        ms_status _s = (st), _e = (exp);                                       \
        if (_s != _e) {                                                        \
            fprintf(stderr, "FAIL %s:%d: status %s expected %s\n", __FILE__,   \
                    __LINE__, ms_status_name(_s), ms_status_name(_e));         \
            failed = 1;                                                        \
            return 1;                                                          \
        }                                                                      \
    } while (0)

#define REQUIRE(cond, msg)                                                     \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, msg);      \
            failed = 1;                                                        \
            goto done;                                                         \
        }                                                                      \
    } while (0)

#define REQUIRE_ST(st, exp)                                                    \
    do {                                                                       \
        ms_status _s = (st), _e = (exp);                                       \
        if (_s != _e) {                                                        \
            fprintf(stderr, "FAIL %s:%d: status %s expected %s\n", __FILE__,   \
                    __LINE__, ms_status_name(_s), ms_status_name(_e));         \
            failed = 1;                                                        \
            goto done;                                                         \
        }                                                                      \
    } while (0)

static int log_leaked;

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int make_adts(uint8_t *out, size_t cap, size_t *len, int sfi, int ch,
                     int nbytes, uint8_t fill)
{
    uint8_t raw[64];
    if (nbytes > (int)sizeof(raw)) {
        return -1;
    }
    memset(raw, fill, (size_t)nbytes);
    return ms_synth_adts_frame(out, cap, len, 1, sfi, ch, raw, (size_t)nbytes);
}

typedef struct test_srv {
    int lfd;
    int port;
    int stop;
    int use_tls;
    pthread_t th;
    SSL_CTX *ctx;
    const char *payload;
    size_t payload_len;
    /* HLS fixtures */
    const char *media_pl;
    const char *master_pl;
    const uint8_t *ts1;
    size_t ts1_len;
    const uint8_t *ts3;
    size_t ts3_len;
    int http_alt_port; /* for https→http 降级测试 */
    /* 受控静态资源（DASH MPD/分段等），可按需响应 Range */
    struct {
        const char *path;
        const void *data;
        size_t len;
        const char *ctype;
        int honor_range;
    } assets[24];
    int nassets;
} test_srv;

static int srv_add(test_srv *s, const char *path, const void *data, size_t len,
                   const char *ctype, int honor_range)
{
    int i;
    if (s == NULL || path == NULL || s->nassets >= (int)(sizeof(s->assets) / sizeof(s->assets[0]))) {
        return -1;
    }
    i = s->nassets;
    s->assets[i].path = path;
    s->assets[i].data = data;
    s->assets[i].len = len;
    s->assets[i].ctype = ctype != NULL ? ctype : "application/octet-stream";
    s->assets[i].honor_range = honor_range;
    s->nassets++;
    return 0;
}

static int bind_loopback(int *port_out)
{
    int fd, yes = 1;
    struct sockaddr_in a;
    socklen_t alen = sizeof(a);
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) != 0 ||
        listen(fd, 16) != 0 ||
        getsockname(fd, (struct sockaddr *)&a, &alen) != 0) {
        close(fd);
        return -1;
    }
    *port_out = ntohs(a.sin_port);
    return fd;
}

static int read_req(int fd, SSL *ssl, char *buf, size_t cap, size_t *out)
{
    size_t n = 0;
    *out = 0;
    while (n + 1 < cap) {
        int r;
        if (ssl != NULL) {
            r = SSL_read(ssl, buf + n, (int)(cap - 1 - n));
        } else {
            r = (int)recv(fd, buf + n, cap - 1 - n, 0);
        }
        if (r <= 0) {
            break;
        }
        n += (size_t)r;
        buf[n] = 0;
        if (strstr(buf, "\r\n\r\n") != NULL) {
            *out = n;
            return 0;
        }
    }
    *out = n;
    return n > 0 ? 0 : -1;
}

static int write_all(int fd, SSL *ssl, const void *p, size_t n)
{
    const uint8_t *b = (const uint8_t *)p;
    size_t off = 0;
    while (off < n) {
        int r;
        if (ssl != NULL) {
            r = SSL_write(ssl, b + off, (int)(n - off));
        } else {
            r = (int)send(fd, b + off, n - off, MSG_NOSIGNAL);
        }
        if (r <= 0) {
            return -1;
        }
        off += (size_t)r;
    }
    return 0;
}

static void send_simple(int fd, SSL *ssl, int code, const char *reason,
                        const char *ctype, const void *body, size_t blen)
{
    char hdr[256];
    int n = snprintf(hdr, sizeof(hdr),
                     "HTTP/1.1 %d %s\r\n"
                     "Content-Type: %s\r\n"
                     "Content-Length: %zu\r\n"
                     "Connection: close\r\n"
                     "\r\n",
                     code, reason, ctype, blen);
    if (n > 0) {
        write_all(fd, ssl, hdr, (size_t)n);
    }
    if (blen && body) {
        write_all(fd, ssl, body, blen);
    }
}

static void handle_client(test_srv *s, int fd)
{
    char req[2048];
    size_t rn = 0;
    SSL *ssl = NULL;
    char *line, *path, *sp;
    char pathbuf[256];
    if (s->use_tls) {
        ssl = SSL_new(s->ctx);
        if (ssl == NULL) {
            close(fd);
            return;
        }
        SSL_set_fd(ssl, fd);
        if (SSL_accept(ssl) != 1) {
            SSL_free(ssl);
            close(fd);
            return;
        }
    }
    if (read_req(fd, ssl, req, sizeof(req), &rn) != 0) {
        goto done;
    }
    line = req;
    path = strchr(line, ' ');
    if (path == NULL) {
        goto done;
    }
    path++;
    sp = strchr(path, ' ');
    if (sp != NULL) {
        size_t pl = (size_t)(sp - path);
        if (pl >= sizeof(pathbuf)) {
            goto done;
        }
        memcpy(pathbuf, path, pl);
        pathbuf[pl] = 0;
        path = pathbuf;
    }
    if (strcmp(path, "/hang") == 0) {
        sleep(3);
        goto done;
    }
    if (strcmp(path, "/slow") == 0) {
        sleep(2);
        send_simple(fd, ssl, 200, "OK", "text/plain", "late", 4);
        goto done;
    }
    if (strcmp(path, "/ok") == 0) {
        send_simple(fd, ssl, 200, "OK", "text/plain", "hello-net", 9);
        goto done;
    }
    if (strcmp(path, "/chunk") == 0) {
        const char *body =
            "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n"
            "Connection: close\r\n\r\n"
            "5\r\nhello\r\n"
            "4\r\n-net\r\n"
            "0\r\n\r\n";
        write_all(fd, ssl, body, strlen(body));
        goto done;
    }
    if (strcmp(path, "/range") == 0) {
        const char *payload = s->payload ? s->payload : "ABCDEFGHIJ";
        size_t plen = s->payload ? s->payload_len : 10;
        const char *rh = strstr(req, "Range:");
        size_t off = 0, end = plen;
        if (rh != NULL && sscanf(rh, "Range: bytes=%zu-%zu", &off, &end) >= 1) {
            if (end >= plen) {
                end = plen - 1;
            }
            if (off < plen && end >= off) {
                char hdr[256];
                size_t n = end - off + 1;
                int hn = snprintf(
                    hdr, sizeof(hdr),
                    "HTTP/1.1 206 Partial Content\r\n"
                    "Content-Range: bytes %zu-%zu/%zu\r\n"
                    "Content-Length: %zu\r\n"
                    "Connection: close\r\n\r\n",
                    off, end, plen, n);
                write_all(fd, ssl, hdr, (size_t)hn);
                write_all(fd, ssl, payload + off, n);
                goto done;
            }
        }
        send_simple(fd, ssl, 200, "OK", "text/plain", payload, plen);
        goto done;
    }
    if (strcmp(path, "/e401") == 0) {
        send_simple(fd, ssl, 401, "Unauthorized", "text/plain", "no", 2);
        goto done;
    }
    if (strcmp(path, "/e403") == 0) {
        send_simple(fd, ssl, 403, "Forbidden", "text/plain", "no", 2);
        goto done;
    }
    if (strcmp(path, "/e404") == 0) {
        send_simple(fd, ssl, 404, "Not Found", "text/plain", "no", 2);
        goto done;
    }
    if (strcmp(path, "/gzip") == 0) {
        const char *b = "HTTP/1.1 200 OK\r\nContent-Encoding: gzip\r\n"
                        "Content-Length: 3\r\nConnection: close\r\n\r\n"
                        "xxx";
        write_all(fd, ssl, b, strlen(b));
        goto done;
    }
    if (strcmp(path, "/redir") == 0) {
        const char *b = "HTTP/1.1 302 Found\r\nLocation: /ok\r\n"
                        "Content-Length: 0\r\nConnection: close\r\n\r\n";
        write_all(fd, ssl, b, strlen(b));
        goto done;
    }
    if (strcmp(path, "/loop") == 0) {
        const char *b = "HTTP/1.1 302 Found\r\nLocation: /loop\r\n"
                        "Content-Length: 0\r\nConnection: close\r\n\r\n";
        write_all(fd, ssl, b, strlen(b));
        goto done;
    }
    if (strcmp(path, "/down") == 0 && s->http_alt_port > 0) {
        char b[256];
        snprintf(b, sizeof(b),
                 "HTTP/1.1 302 Found\r\n"
                 "Location: http://127.0.0.1:%d/ok\r\n"
                 "Content-Length: 0\r\nConnection: close\r\n\r\n",
                 s->http_alt_port);
        write_all(fd, ssl, b, strlen(b));
        goto done;
    }
    if (strcmp(path, "/media.m3u8") == 0 && s->media_pl) {
        send_simple(fd, ssl, 200, "OK", "application/vnd.apple.mpegurl",
                    s->media_pl, strlen(s->media_pl));
        goto done;
    }
    if (strcmp(path, "/master.m3u8") == 0 && s->master_pl) {
        send_simple(fd, ssl, 200, "OK", "application/vnd.apple.mpegurl",
                    s->master_pl, strlen(s->master_pl));
        goto done;
    }
    if (strcmp(path, "/s1.ts") == 0 && s->ts1) {
        send_simple(fd, ssl, 200, "OK", "video/mp2t", s->ts1, s->ts1_len);
        goto done;
    }
    if (strcmp(path, "/s3.ts") == 0 && s->ts3) {
        send_simple(fd, ssl, 200, "OK", "video/mp2t", s->ts3, s->ts3_len);
        goto done;
    }
    {
        int i;
        for (i = 0; i < s->nassets; i++) {
            const uint8_t *body;
            size_t blen;
            if (s->assets[i].path == NULL || strcmp(path, s->assets[i].path) != 0) {
                continue;
            }
            body = (const uint8_t *)s->assets[i].data;
            blen = s->assets[i].len;
            if (s->assets[i].honor_range) {
                const char *rh = strstr(req, "Range:");
                size_t off = 0, end = blen;
                if (rh != NULL && sscanf(rh, "Range: bytes=%zu-%zu", &off, &end) >= 1) {
                    if (blen == 0) {
                        send_simple(fd, ssl, 416, "Range Not Satisfiable",
                                    "text/plain", "no", 2);
                        goto done;
                    }
                    if (end >= blen) {
                        end = blen - 1;
                    }
                    if (off < blen && end >= off) {
                        char hdr[256];
                        size_t n = end - off + 1;
                        int hn = snprintf(
                            hdr, sizeof(hdr),
                            "HTTP/1.1 206 Partial Content\r\n"
                            "Content-Type: %s\r\n"
                            "Content-Range: bytes %zu-%zu/%zu\r\n"
                            "Content-Length: %zu\r\n"
                            "Connection: close\r\n\r\n",
                            s->assets[i].ctype, off, end, blen, n);
                        write_all(fd, ssl, hdr, (size_t)hn);
                        write_all(fd, ssl, body + off, n);
                        goto done;
                    }
                }
            }
            send_simple(fd, ssl, 200, "OK", s->assets[i].ctype, body, blen);
            goto done;
        }
    }
    send_simple(fd, ssl, 404, "Not Found", "text/plain", "no", 2);
done:
    if (ssl != NULL) {
        SSL_shutdown(ssl);
        SSL_free(ssl);
    }
    close(fd);
}

static void *srv_main(void *arg)
{
    test_srv *s = (test_srv *)arg;
    while (!s->stop) {
        struct timeval tv;
        fd_set rfds;
        int c;
        tv.tv_sec = 0;
        tv.tv_usec = 100000;
        FD_ZERO(&rfds);
        FD_SET(s->lfd, &rfds);
        if (select(s->lfd + 1, &rfds, NULL, NULL, &tv) <= 0) {
            continue;
        }
        c = accept(s->lfd, NULL, NULL);
        if (c < 0) {
            continue;
        }
        handle_client(s, c);
    }
    return NULL;
}

static int srv_start(test_srv *s, int use_tls, const char *cert, const char *key)
{
    memset(s, 0, sizeof(*s));
    s->lfd = bind_loopback(&s->port);
    if (s->lfd < 0) {
        return -1;
    }
    s->use_tls = use_tls;
    if (use_tls) {
        s->ctx = SSL_CTX_new(TLS_server_method());
        if (s->ctx == NULL ||
            SSL_CTX_use_certificate_file(s->ctx, cert, SSL_FILETYPE_PEM) != 1 ||
            SSL_CTX_use_PrivateKey_file(s->ctx, key, SSL_FILETYPE_PEM) != 1) {
            if (s->ctx) {
                SSL_CTX_free(s->ctx);
            }
            close(s->lfd);
            return -1;
        }
        SSL_CTX_set_min_proto_version(s->ctx, TLS1_2_VERSION);
    }
    s->stop = 0;
    if (pthread_create(&s->th, NULL, srv_main, s) != 0) {
        if (s->ctx) {
            SSL_CTX_free(s->ctx);
        }
        close(s->lfd);
        return -1;
    }
    return 0;
}

static void srv_stop(test_srv *s)
{
    if (s->lfd < 0) {
        return;
    }
    s->stop = 1;
    shutdown(s->lfd, SHUT_RDWR);
    pthread_join(s->th, NULL);
    close(s->lfd);
    s->lfd = -1;
    if (s->ctx) {
        SSL_CTX_free(s->ctx);
        s->ctx = NULL;
    }
}

static int read_all(ms_net *n, uint8_t *out, size_t cap, size_t *got, int max_ms)
{
    int64_t dl = now_ms() + max_ms;
    *got = 0;
    for (;;) {
        size_t nread = 0;
        ms_status st;
        if (*got >= cap) {
            return 0;
        }
        st = ms_net_read(n, out + *got, cap - *got, &nread);
        if (st == MS_OK && nread == 0) {
            return 0;
        }
        if (st == MS_OK) {
            *got += nread;
            continue;
        }
        if (st == MS_AGAIN) {
            if (now_ms() >= dl) {
                return -2;
            }
            usleep(3000);
            continue;
        }
        return (int)st;
    }
}

static int start_get(ms_net *n, const char *url)
{
    ms_status st = ms_net_get_start(n, url, 0, 0);
    if (st != MS_OK) {
        fprintf(stderr, "FAIL %s:%d: get_start %s\n", __FILE__, __LINE__,
                ms_status_name(st));
        failed = 1;
        return 1;
    }
    return 0;
}

static int log_looks_sensitive(const char *msg)
{
    return strstr(msg, "http://") != NULL || strstr(msg, "https://") != NULL ||
           strstr(msg, "://") != NULL;
}

static void net_log_print(void *ctx, int level, const char *msg)
{
    (void)ctx;
    if (msg != NULL && log_looks_sensitive(msg)) {
        fprintf(stderr, "FAIL net log leaked URL-like text (level %d)\n", level);
        log_leaked = 1;
        failed = 1;
        return;
    }
    if (level > 0) {
        fprintf(stderr, "net-log[%d]: %s\n", level, msg);
    }
}

static int test_http_plain(test_srv *hs)
{
    ms_net *n = NULL;
    ms_net_limits lim;
    uint8_t buf[256];
    size_t got = 0;
    char url[128];
    ms_net_limits_default(&lim);
    lim.connect_timeout_ms = 2000;
    lim.io_timeout_ms = 2000;
    REQUIRE_ST(ms_net_create(NULL, &lim, &n), MS_OK);
    ms_net_set_log(n, net_log_print, NULL);
    REQUIRE_ST(ms_net_worker_start(n), MS_OK);

    snprintf(url, sizeof(url), "http://127.0.0.1:%d/ok", hs->port);
    REQUIRE(start_get(n, url) == 0, "get ok");
    REQUIRE(read_all(n, buf, sizeof(buf), &got, 2000) == 0, "read ok");
    REQUIRE(got == 9 && memcmp(buf, "hello-net", 9) == 0, "200 body");
    REQUIRE(ms_net_last_http_status(n) == 200, "status 200");

    snprintf(url, sizeof(url), "http://127.0.0.1:%d/chunk", hs->port);
    REQUIRE_ST(ms_net_get_start(n, url, 0, 0), MS_OK);
    REQUIRE(read_all(n, buf, sizeof(buf), &got, 2000) == 0, "chunk read");
    REQUIRE(got == 9 && memcmp(buf, "hello-net", 9) == 0, "chunk body");

    snprintf(url, sizeof(url), "http://127.0.0.1:%d/range", hs->port);
    REQUIRE_ST(ms_net_get_start(n, url, 2, 4), MS_OK);
    REQUIRE(read_all(n, buf, sizeof(buf), &got, 2000) == 0, "range read");
    REQUIRE(ms_net_last_http_status(n) == 206, "206");
    REQUIRE(got == 4 && memcmp(buf, "CDEF", 4) == 0, "range body");

    snprintf(url, sizeof(url), "http://127.0.0.1:%d/redir", hs->port);
    REQUIRE_ST(ms_net_get_start(n, url, 0, 0), MS_OK);
    REQUIRE(read_all(n, buf, sizeof(buf), &got, 2000) == 0, "redir");
    REQUIRE(got == 9 && memcmp(buf, "hello-net", 9) == 0, "redir body");

    snprintf(url, sizeof(url), "http://127.0.0.1:%d/loop", hs->port);
    REQUIRE_ST(ms_net_get_start(n, url, 0, 0), MS_OK);
    REQUIRE(read_all(n, buf, sizeof(buf), &got, 2000) != 0, "loop should fail");
    REQUIRE(ms_net_last_err(n) == MS_NET_ERR_REDIRECT, "loop err");

    snprintf(url, sizeof(url), "http://127.0.0.1:%d/e401", hs->port);
    REQUIRE_ST(ms_net_get_start(n, url, 0, 0), MS_OK);
    REQUIRE(read_all(n, buf, sizeof(buf), &got, 2000) != 0, "401 fail");
    REQUIRE(ms_net_last_http_status(n) == 401, "401");
    REQUIRE(ms_net_last_err(n) == MS_NET_ERR_HTTP, "401 err");

    snprintf(url, sizeof(url), "http://127.0.0.1:%d/e403", hs->port);
    REQUIRE_ST(ms_net_get_start(n, url, 0, 0), MS_OK);
    REQUIRE(read_all(n, buf, sizeof(buf), &got, 2000) != 0, "403 fail");
    REQUIRE(ms_net_last_http_status(n) == 403, "403");

    snprintf(url, sizeof(url), "http://127.0.0.1:%d/e404", hs->port);
    REQUIRE_ST(ms_net_get_start(n, url, 0, 0), MS_OK);
    REQUIRE(read_all(n, buf, sizeof(buf), &got, 2000) != 0, "404 fail");
    REQUIRE(ms_net_last_http_status(n) == 404, "404");

    snprintf(url, sizeof(url), "http://127.0.0.1:%d/gzip", hs->port);
    REQUIRE_ST(ms_net_get_start(n, url, 0, 0), MS_OK);
    REQUIRE(read_all(n, buf, sizeof(buf), &got, 2000) != 0, "gzip fail");
    REQUIRE(ms_net_last_err(n) == MS_NET_ERR_UNSUPPORTED, "gzip err");

    printf("  http plain 200/206/chunked/redir/loop/401/403/404/encoding ok\n");
done:
    ms_net_destroy(n);
    return failed;
}

static int test_timeout_cancel(test_srv *hs)
{
    ms_net *n = NULL;
    ms_net_limits lim;
    uint8_t buf[32];
    size_t got = 0;
    char url[128];
    int rc;
    ms_net_limits_default(&lim);
    lim.connect_timeout_ms = 400;
    lim.io_timeout_ms = 400;
    REQUIRE_ST(ms_net_create(NULL, &lim, &n), MS_OK);
    REQUIRE_ST(ms_net_worker_start(n), MS_OK);
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/hang", hs->port);
    REQUIRE_ST(ms_net_get_start(n, url, 0, 0), MS_OK);
    rc = read_all(n, buf, sizeof(buf), &got, 2000);
    REQUIRE(rc != 0, "hang should timeout");
    REQUIRE(ms_net_last_err(n) == MS_NET_ERR_TIMEOUT, "timeout err");
    ms_net_destroy(n);
    n = NULL;

    REQUIRE_ST(ms_net_create(NULL, &lim, &n), MS_OK);
    REQUIRE_ST(ms_net_worker_start(n), MS_OK);
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/slow", hs->port);
    REQUIRE_ST(ms_net_get_start(n, url, 0, 0), MS_OK);
    usleep(20000);
    ms_net_cancel(n);
    rc = read_all(n, buf, sizeof(buf), &got, 1000);
    REQUIRE(rc != 0, "cancel should fail read");
    REQUIRE(ms_net_last_err(n) == MS_NET_ERR_CANCELED ||
                ms_net_read(n, buf, 1, &got) == MS_ERR_CANCELED,
            "canceled");
    printf("  timeout and cancel ok\n");
done:
    ms_net_destroy(n);
    return failed;
}

static int test_tls(test_srv *ts, test_srv *hs, const char *ca, const char *certdir)
{
    ms_net *n = NULL;
    ms_net_limits lim;
    uint8_t buf[128];
    size_t got = 0;
    char url[192];
    int rc;
    (void)certdir;
    ms_net_limits_default(&lim);
    lim.connect_timeout_ms = 3000;
    lim.io_timeout_ms = 3000;
    REQUIRE_ST(ms_net_create(NULL, &lim, &n), MS_OK);
    REQUIRE_ST(ms_net_set_ca_file(n, ca), MS_OK);
    REQUIRE_ST(ms_net_worker_start(n), MS_OK);

    snprintf(url, sizeof(url), "https://localhost:%d/ok", ts->port);
    REQUIRE_ST(ms_net_get_start(n, url, 0, 0), MS_OK);
    REQUIRE(read_all(n, buf, sizeof(buf), &got, 3000) == 0, "https 200");
    REQUIRE(got == 9 && memcmp(buf, "hello-net", 9) == 0, "https body");

    /* 错误主机名：证书是 localhost，用 127.0.0.1 作为主机名应失败（无 IP SAN 匹配该用法时：
     * 本测试证书含 DNS:localhost 与 IP:127.0.0.1，故改用错误 DNS 名。 */
    ms_net_destroy(n);
    n = NULL;
    REQUIRE_ST(ms_net_create(NULL, &lim, &n), MS_OK);
    REQUIRE_ST(ms_net_set_ca_file(n, ca), MS_OK);
    REQUIRE_ST(ms_net_worker_start(n), MS_OK);
    snprintf(url, sizeof(url), "https://wrong.example:%d/ok", ts->port);
    /* 连 wrong.example 会 DNS 失败；改用连接 IP 但 Host/SNI 为错误名做不到用 URL 的 IP。
     * 使用 /etc/hosts 不可靠。用证书不信任的路径测坏证；主机名用 localhost 连错站较难。
     * 这里用「未加 CA 的信任库」测证书失败，并用 HTTPS URL 的 IP + 非 localhost 主机：
     * 我们单独测「无 CA」与「错误 SNI」——后者通过连 localhost 端口但证书 CN 不匹配需另一证书。
     * 用 127.0.0.1 作为 URL 主机：证书有 IP SAN，会通过。改用「无 CA」做失败。 */
    REQUIRE_ST(ms_net_get_start(n, url, 0, 0), MS_OK);
    rc = read_all(n, buf, sizeof(buf), &got, 3000);
    REQUIRE(rc != 0, "wrong host should fail (dns or tls)");
    ms_net_destroy(n);
    n = NULL;

    /* 证书失败：不加载测试 CA，默认信任库不应信任临时 CA */
    REQUIRE_ST(ms_net_create(NULL, &lim, &n), MS_OK);
    REQUIRE_ST(ms_net_worker_start(n), MS_OK);
    snprintf(url, sizeof(url), "https://localhost:%d/ok", ts->port);
    REQUIRE_ST(ms_net_get_start(n, url, 0, 0), MS_OK);
    rc = read_all(n, buf, sizeof(buf), &got, 3000);
    REQUIRE(rc != 0, "untrusted ca should fail");
    REQUIRE(ms_net_last_err(n) == MS_NET_ERR_TLS, "tls err");
    ms_net_destroy(n);
    n = NULL;

    /* https→http 降级拒绝 */
    REQUIRE_ST(ms_net_create(NULL, &lim, &n), MS_OK);
    REQUIRE_ST(ms_net_set_ca_file(n, ca), MS_OK);
    REQUIRE_ST(ms_net_worker_start(n), MS_OK);
    ts->http_alt_port = hs->port;
    snprintf(url, sizeof(url), "https://localhost:%d/down", ts->port);
    REQUIRE_ST(ms_net_get_start(n, url, 0, 0), MS_OK);
    rc = read_all(n, buf, sizeof(buf), &got, 3000);
    REQUIRE(rc != 0, "downgrade denied");
    REQUIRE(ms_net_last_err(n) == MS_NET_ERR_REDIRECT, "downgrade err");
    ts->http_alt_port = 0;
    printf("  tls 200 / untrusted cert / downgrade ok\n");
done:
    ms_net_destroy(n);
    return failed;
}

typedef struct rec {
    int nau, nfmt, ndisc, ncfg;
} rec;

static int take_ev(ms_session *s, rec *r, ms_status *pst)
{
    ms_event ev;
    ms_status st = ms_session_pull(s, &ev);
    *pst = st;
    if (st == MS_AGAIN || st == MS_EOS) {
        return 0;
    }
    CHECK_ST(st, MS_OK);
    if (ev.type == MS_EVENT_AU) {
        r->nau++;
    } else if (ev.type == MS_EVENT_FORMAT) {
        r->nfmt++;
    } else if (ev.type == MS_EVENT_CODEC_CONFIG) {
        r->ncfg++;
    } else if (ev.type == MS_EVENT_DISCONTINUITY) {
        r->ndisc++;
    }
    if (ev.release_id != 0) {
        CHECK_ST(ms_session_release(s, ev.release_id), MS_OK);
    }
    return 0;
}

static int hls_pump(ms_hls *h, ms_session *s, rec *r, int max_steps, ms_status *out)
{
    int i, stall = 0;
    *out = MS_OK;
    for (i = 0; i < max_steps; i++) {
        ms_status st, pst;
        for (;;) {
            if (take_ev(s, r, &pst) != 0) {
                return -1;
            }
            if (pst == MS_EOS) {
                *out = MS_EOS;
                return 0;
            }
            if (pst == MS_AGAIN) {
                break;
            }
        }
        st = ms_hls_drive(h);
        if (st == MS_OK) {
            stall = 0;
            continue;
        }
        if (st == MS_AGAIN || st == MS_LIMIT) {
            stall++;
            if (stall > 800) {
                *out = st;
                return -1;
            }
            usleep(3000);
            continue;
        }
        if (st == MS_EOS) {
            *out = MS_EOS;
            return 0;
        }
        *out = st;
        return 0;
    }
    return -1;
}

static int test_hls_http(test_srv *hs)
{
    ms_session *s = NULL;
    ms_hls *h = NULL;
    ms_net *n = NULL;
    ms_hls_fetch fe;
    rec r;
    ms_status ost;
    char url[128];
    char master[128];
    ms_net_limits nlim;
    ms_net_limits_default(&nlim);
    nlim.io_timeout_ms = 4000;
    REQUIRE_ST(ms_net_create(NULL, &nlim, &n), MS_OK);
    REQUIRE_ST(ms_net_worker_start(n), MS_OK);
    REQUIRE_ST(ms_session_create(NULL, NULL, &s), MS_OK);
    REQUIRE_ST(ms_hls_create(NULL, NULL, &h), MS_OK);
    ms_net_hls_fetch_bind(n, &fe);
    REQUIRE_ST(ms_hls_bind(h, s, &fe), MS_OK);
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/media.m3u8", hs->port);
    REQUIRE_ST(ms_hls_open(h, url), MS_OK);
    memset(&r, 0, sizeof(r));
    REQUIRE(hls_pump(h, s, &r, 8000, &ost) == 0, "hls pump");
    REQUIRE(ost == MS_EOS, "hls eos");
    REQUIRE(r.nau >= 1 && r.nfmt >= 1, "hls au");
    ms_hls_destroy(h);
    ms_session_destroy(s);
    ms_net_destroy(n);
    h = NULL;
    s = NULL;
    n = NULL;

    /* master 仍明确拒绝 */
    REQUIRE_ST(ms_net_create(NULL, &nlim, &n), MS_OK);
    REQUIRE_ST(ms_net_worker_start(n), MS_OK);
    REQUIRE_ST(ms_session_create(NULL, NULL, &s), MS_OK);
    REQUIRE_ST(ms_hls_create(NULL, NULL, &h), MS_OK);
    ms_net_hls_fetch_bind(n, &fe);
    REQUIRE_ST(ms_hls_bind(h, s, &fe), MS_OK);
    snprintf(master, sizeof(master), "http://127.0.0.1:%d/master.m3u8", hs->port);
    REQUIRE_ST(ms_hls_open(h, master), MS_OK);
    memset(&r, 0, sizeof(r));
    REQUIRE(hls_pump(h, s, &r, 2000, &ost) == 0 || ost == MS_ERR_UNSUPPORTED,
            "master pump");
    REQUIRE(ost == MS_ERR_UNSUPPORTED, "master rejected");
    printf("  hls media via http -> AU ok; master rejected\n");
done:
    ms_hls_destroy(h);
    ms_session_destroy(s);
    ms_net_destroy(n);
    return failed;
}

/* 用第二张「错误主机名」证书：CN=wrong.example，不含 localhost。 */
static int test_bad_hostname(const char *ca, const char *wrong_cert,
                             const char *wrong_key)
{
    test_srv ws;
    ms_net *n = NULL;
    ms_net_limits lim;
    uint8_t buf[64];
    size_t got = 0;
    char url[192];
    int rc;
    memset(&ws, 0, sizeof(ws));
    if (srv_start(&ws, 1, wrong_cert, wrong_key) != 0) {
        fprintf(stderr, "FAIL cannot start wrong-host tls server\n");
        failed = 1;
        return 1;
    }
    ms_net_limits_default(&lim);
    lim.connect_timeout_ms = 3000;
    lim.io_timeout_ms = 3000;
    REQUIRE_ST(ms_net_create(NULL, &lim, &n), MS_OK);
    REQUIRE_ST(ms_net_set_ca_file(n, ca), MS_OK);
    REQUIRE_ST(ms_net_worker_start(n), MS_OK);
    snprintf(url, sizeof(url), "https://localhost:%d/ok", ws.port);
    REQUIRE_ST(ms_net_get_start(n, url, 0, 0), MS_OK);
    rc = read_all(n, buf, sizeof(buf), &got, 3000);
    REQUIRE(rc != 0, "hostname mismatch should fail");
    REQUIRE(ms_net_last_err(n) == MS_NET_ERR_TLS, "hostname tls err");
    printf("  tls hostname mismatch ok\n");
done:
    ms_net_destroy(n);
    srv_stop(&ws);
    return failed;
}

static int dash_pump(ms_dash *d, ms_session *s, rec *r, int max_steps,
                     ms_status *out)
{
    int i, stall = 0;
    *out = MS_OK;
    for (i = 0; i < max_steps; i++) {
        ms_status st, pst;
        for (;;) {
            if (take_ev(s, r, &pst) != 0) {
                return -1;
            }
            if (pst == MS_EOS) {
                *out = MS_EOS;
                return 0;
            }
            if (pst == MS_AGAIN) {
                break;
            }
        }
        st = ms_dash_drive(d);
        if (st == MS_OK) {
            stall = 0;
            continue;
        }
        if (st == MS_AGAIN || st == MS_LIMIT) {
            stall++;
            if (stall > 800) {
                *out = st;
                return -1;
            }
            usleep(3000);
            continue;
        }
        if (st == MS_EOS) {
            *out = MS_EOS;
            return 0;
        }
        *out = st;
        return 0;
    }
    return -1;
}

/* 受控本机 HTTP：DASH MPD + 分段经 libmsnet 出 AU。不扩 ABR/分轨/多 Period。 */
static int test_dash_http(test_srv *hs, const uint8_t *ts, size_t off2,
                          size_t tslen, const uint8_t *ts_miss2, size_t miss2_len)
{
    ms_session *s = NULL;
    ms_dash *d = NULL;
    ms_net *n = NULL;
    ms_hls_fetch fe;
    rec r;
    ms_status ost;
    char url[160];
    char mpd_vod[768];
    char mpd_range[768];
    char mpd_miss[768];
    char mpd_trunc[768];
    char mpd_drm[768];
    ms_net_limits nlim;
    ms_dash_limits dlim;
    const char *ctype_mpd = "application/dash+xml";
    const char *ctype_ts = "video/mp2t";

    ms_net_limits_default(&nlim);
    nlim.io_timeout_ms = 4000;
    ms_dash_limits_default(&dlim);
    dlim.max_retries = 0; /* 缺片尽快走不连续，不把 404 重试拉长 */

    snprintf(mpd_vod, sizeof(mpd_vod),
             "<MPD type=\"static\" mediaPresentationDuration=\"PT2S\">"
             "<Period duration=\"PT2S\">"
             "<AdaptationSet mimeType=\"video/mp2t\" contentType=\"video\">"
             "<Representation id=\"r1\" bandwidth=\"1\">"
             "<SegmentTemplate timescale=\"1\" duration=\"1\" startNumber=\"1\" "
             "media=\"seg-$Number$.ts\"/>"
             "</Representation></AdaptationSet></Period></MPD>");
    snprintf(mpd_range, sizeof(mpd_range),
             "<MPD type=\"static\" mediaPresentationDuration=\"PT2S\">"
             "<Period duration=\"PT2S\">"
             "<AdaptationSet mimeType=\"video/mp2t\" contentType=\"video\">"
             "<Representation id=\"r1\" bandwidth=\"1\">"
             "<SegmentList timescale=\"1\" duration=\"1\">"
             "<SegmentURL media=\"all.ts\" mediaRange=\"0-%zu\"/>"
             "<SegmentURL media=\"all.ts\" mediaRange=\"%zu-%zu\"/>"
             "</SegmentList></Representation></AdaptationSet></Period></MPD>",
             off2 > 0 ? off2 - 1 : 0, off2, tslen > 0 ? tslen - 1 : 0);
    snprintf(mpd_miss, sizeof(mpd_miss),
             "<MPD type=\"static\" mediaPresentationDuration=\"PT2S\">"
             "<Period duration=\"PT2S\">"
             "<AdaptationSet mimeType=\"video/mp2t\" contentType=\"video\">"
             "<Representation id=\"r1\" bandwidth=\"1\">"
             "<SegmentTemplate timescale=\"1\" duration=\"1\" startNumber=\"1\" "
             "media=\"miss-$Number$.ts\"/>"
             "</Representation></AdaptationSet></Period></MPD>");
    snprintf(mpd_trunc, sizeof(mpd_trunc),
             "<MPD type=\"static\" mediaPresentationDuration=\"PT2S\">"
             "<Period duration=\"PT2S\">"
             "<AdaptationSet mimeType=\"video/mp2t\" contentType=\"video\">"
             "<Representation id=\"r1\" bandwidth=\"1\">"
             "<SegmentList timescale=\"1\" duration=\"1\">"
             "<SegmentURL media=\"short.ts\" mediaRange=\"0-99999\"/>"
             "<SegmentURL media=\"full2.ts\"/>"
             "</SegmentList></Representation></AdaptationSet></Period></MPD>");
    snprintf(mpd_drm, sizeof(mpd_drm),
             "<MPD type=\"static\" mediaPresentationDuration=\"PT1S\">"
             "<Period>"
             "<AdaptationSet mimeType=\"video/mp2t\">"
             "<ContentProtection schemeIdUri=\"urn:uuid:00000000-0000-0000-0000-000000000000\"/>"
             "<Representation id=\"a\" bandwidth=\"1\">"
             "<SegmentTemplate media=\"seg-$Number$.ts\" duration=\"1\" timescale=\"1\"/>"
             "</Representation></AdaptationSet></Period></MPD>");

    if (srv_add(hs, "/dash/vod.mpd", mpd_vod, strlen(mpd_vod), ctype_mpd, 0) != 0 ||
        srv_add(hs, "/dash/range.mpd", mpd_range, strlen(mpd_range), ctype_mpd, 0) != 0 ||
        srv_add(hs, "/dash/miss.mpd", mpd_miss, strlen(mpd_miss), ctype_mpd, 0) != 0 ||
        srv_add(hs, "/dash/trunc.mpd", mpd_trunc, strlen(mpd_trunc), ctype_mpd, 0) != 0 ||
        srv_add(hs, "/dash/drm.mpd", mpd_drm, strlen(mpd_drm), ctype_mpd, 0) != 0 ||
        srv_add(hs, "/dash/seg-1.ts", ts, off2, ctype_ts, 0) != 0 ||
        srv_add(hs, "/dash/seg-2.ts", ts + off2, tslen - off2, ctype_ts, 0) != 0 ||
        srv_add(hs, "/dash/all.ts", ts, tslen, ctype_ts, 1) != 0 ||
        srv_add(hs, "/dash/short.ts", ts, off2, ctype_ts, 1) != 0 ||
        srv_add(hs, "/dash/full2.ts", ts_miss2, miss2_len, ctype_ts, 0) != 0 ||
        /* miss-1 故意不注册，404；miss-2 为自包含 PAT/PMT+第二帧 */
        srv_add(hs, "/dash/miss-2.ts", ts_miss2, miss2_len, ctype_ts, 0) != 0) {
        fprintf(stderr, "dash asset register fail\n");
        failed = 1;
        return 1;
    }

    /* 1) 200 VOD：SegmentTemplate → 真实 AU */
    REQUIRE_ST(ms_net_create(NULL, &nlim, &n), MS_OK);
    REQUIRE_ST(ms_net_worker_start(n), MS_OK);
    REQUIRE_ST(ms_session_create(NULL, NULL, &s), MS_OK);
    REQUIRE_ST(ms_dash_create(NULL, &dlim, &d), MS_OK);
    ms_net_hls_fetch_bind(n, &fe);
    REQUIRE_ST(ms_dash_bind(d, s, &fe), MS_OK);
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/dash/vod.mpd", hs->port);
    REQUIRE_ST(ms_dash_open(d, url), MS_OK);
    memset(&r, 0, sizeof(r));
    REQUIRE(dash_pump(d, s, &r, 8000, &ost) == 0, "dash vod pump");
    REQUIRE_ST(ost, MS_EOS);
    REQUIRE(r.nau >= 1 && r.nfmt >= 1, "dash vod au");
    ms_dash_destroy(d);
    ms_session_destroy(s);
    ms_net_destroy(n);
    d = NULL;
    s = NULL;
    n = NULL;

    /* 2) 可选 Range：同一资源上两个 mediaRange，经 206 出 AU */
    REQUIRE_ST(ms_net_create(NULL, &nlim, &n), MS_OK);
    REQUIRE_ST(ms_net_worker_start(n), MS_OK);
    REQUIRE_ST(ms_session_create(NULL, NULL, &s), MS_OK);
    REQUIRE_ST(ms_dash_create(NULL, &dlim, &d), MS_OK);
    ms_net_hls_fetch_bind(n, &fe);
    REQUIRE_ST(ms_dash_bind(d, s, &fe), MS_OK);
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/dash/range.mpd", hs->port);
    REQUIRE_ST(ms_dash_open(d, url), MS_OK);
    memset(&r, 0, sizeof(r));
    REQUIRE(dash_pump(d, s, &r, 8000, &ost) == 0, "dash range pump");
    REQUIRE_ST(ost, MS_EOS);
    REQUIRE(r.nau >= 1 && r.nfmt >= 1, "dash range au");
    ms_dash_destroy(d);
    ms_session_destroy(s);
    ms_net_destroy(n);
    d = NULL;
    s = NULL;
    n = NULL;

    /* 3) 缺片：404 不拼接，发不连续后播放自包含第二段 */
    REQUIRE_ST(ms_net_create(NULL, &nlim, &n), MS_OK);
    REQUIRE_ST(ms_net_worker_start(n), MS_OK);
    REQUIRE_ST(ms_session_create(NULL, NULL, &s), MS_OK);
    REQUIRE_ST(ms_dash_create(NULL, &dlim, &d), MS_OK);
    ms_net_hls_fetch_bind(n, &fe);
    REQUIRE_ST(ms_dash_bind(d, s, &fe), MS_OK);
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/dash/miss.mpd", hs->port);
    REQUIRE_ST(ms_dash_open(d, url), MS_OK);
    memset(&r, 0, sizeof(r));
    REQUIRE(dash_pump(d, s, &r, 8000, &ost) == 0, "dash miss pump");
    REQUIRE_ST(ost, MS_EOS);
    REQUIRE(r.ndisc >= 1, "dash miss disc");
    REQUIRE(r.nau >= 1, "dash miss second au");
    ms_dash_destroy(d);
    ms_session_destroy(s);
    ms_net_destroy(n);
    d = NULL;
    s = NULL;
    n = NULL;

    /* 4) 截断：声明 mediaRange 远大于实体，发不连续后继续第二段 */
    REQUIRE_ST(ms_net_create(NULL, &nlim, &n), MS_OK);
    REQUIRE_ST(ms_net_worker_start(n), MS_OK);
    REQUIRE_ST(ms_session_create(NULL, NULL, &s), MS_OK);
    REQUIRE_ST(ms_dash_create(NULL, &dlim, &d), MS_OK);
    ms_net_hls_fetch_bind(n, &fe);
    REQUIRE_ST(ms_dash_bind(d, s, &fe), MS_OK);
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/dash/trunc.mpd", hs->port);
    REQUIRE_ST(ms_dash_open(d, url), MS_OK);
    memset(&r, 0, sizeof(r));
    REQUIRE(dash_pump(d, s, &r, 8000, &ost) == 0, "dash trunc pump");
    REQUIRE_ST(ost, MS_EOS);
    REQUIRE(r.ndisc >= 1, "dash trunc disc");
    REQUIRE(r.nau >= 1, "dash trunc still au");
    ms_dash_destroy(d);
    ms_session_destroy(s);
    ms_net_destroy(n);
    d = NULL;
    s = NULL;
    n = NULL;

    /* 5) 取消 */
    REQUIRE_ST(ms_net_create(NULL, &nlim, &n), MS_OK);
    REQUIRE_ST(ms_net_worker_start(n), MS_OK);
    REQUIRE_ST(ms_session_create(NULL, NULL, &s), MS_OK);
    REQUIRE_ST(ms_dash_create(NULL, &dlim, &d), MS_OK);
    ms_net_hls_fetch_bind(n, &fe);
    REQUIRE_ST(ms_dash_bind(d, s, &fe), MS_OK);
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/dash/vod.mpd", hs->port);
    REQUIRE_ST(ms_dash_open(d, url), MS_OK);
    (void)ms_dash_drive(d);
    REQUIRE_ST(ms_dash_cancel(d), MS_OK);
    REQUIRE_ST(ms_dash_drive(d), MS_ERR_CANCELED);
    ms_dash_destroy(d);
    ms_session_destroy(s);
    ms_net_destroy(n);
    d = NULL;
    s = NULL;
    n = NULL;

    /* 6) 反压：多帧单段 + 小 AU 队列必须出现 LIMIT，且最终收齐 */
    {
        ms_session_limits slim;
        int saw_limit = 0;
        int step, fi;
        ms_synth_buf tsbp;
        ms_synth_stream stbp;
        char mpd_bp[512];
        uint8_t abp[64];
        size_t lbp;
        memset(&tsbp, 0, sizeof(tsbp));
        ms_synth_reset_cc();
        REQUIRE(ms_synth_init(&tsbp) == 0, "bp ts init");
        memset(&stbp, 0, sizeof(stbp));
        stbp.stream_type = 0x0f;
        stbp.pid = 0x100;
        REQUIRE(ms_synth_pat_pmt(&tsbp, 1, 0x1000, &stbp, 1, 0, 188) == 0, "bp pat");
        for (fi = 0; fi < 8; fi++) {
            REQUIRE(make_adts(abp, sizeof(abp), &lbp, 3, 2, 16, (uint8_t)(0xB0 + fi)) == 0,
                    "bp adts");
            REQUIRE(ms_synth_pes(&tsbp, 0x100, 0xc0, 1, (uint64_t)fi * 1920, abp, lbp, 188) == 0,
                    "bp pes");
        }
        snprintf(mpd_bp, sizeof(mpd_bp),
                 "<MPD type=\"static\" mediaPresentationDuration=\"PT8S\">"
                 "<Period duration=\"PT8S\">"
                 "<AdaptationSet mimeType=\"video/mp2t\" contentType=\"video\">"
                 "<Representation id=\"r1\" bandwidth=\"1\">"
                 "<SegmentTemplate timescale=\"1\" duration=\"8\" startNumber=\"1\" "
                 "media=\"bp-$Number$.ts\"/>"
                 "</Representation></AdaptationSet></Period></MPD>");
        REQUIRE(srv_add(hs, "/dash/bp.mpd", mpd_bp, strlen(mpd_bp), ctype_mpd, 0) == 0,
                "bp mpd");
        REQUIRE(srv_add(hs, "/dash/bp-1.ts", tsbp.data, tsbp.len, ctype_ts, 0) == 0,
                "bp ts");
        ms_session_limits_default(&slim);
        slim.au_queue_bytes = 48;
        REQUIRE_ST(ms_net_create(NULL, &nlim, &n), MS_OK);
        REQUIRE_ST(ms_net_worker_start(n), MS_OK);
        REQUIRE_ST(ms_session_create(NULL, &slim, &s), MS_OK);
        REQUIRE_ST(ms_dash_create(NULL, &dlim, &d), MS_OK);
        ms_net_hls_fetch_bind(n, &fe);
        REQUIRE_ST(ms_dash_bind(d, s, &fe), MS_OK);
        snprintf(url, sizeof(url), "http://127.0.0.1:%d/dash/bp.mpd", hs->port);
        REQUIRE_ST(ms_dash_open(d, url), MS_OK);
        memset(&r, 0, sizeof(r));
        ost = MS_OK;
        for (step = 0; step < 8000; step++) {
            ms_status st, pst;
            st = ms_dash_drive(d);
            if (st == MS_LIMIT) {
                saw_limit = 1;
                if (take_ev(s, &r, &pst) != 0) {
                    failed = 1;
                    break;
                }
                while (pst == MS_OK) {
                    if (take_ev(s, &r, &pst) != 0) {
                        failed = 1;
                        break;
                    }
                }
                continue;
            }
            if (st == MS_AGAIN) {
                usleep(3000);
                continue;
            }
            if (st == MS_EOS) {
                int guard = 0;
                while (guard++ < 64) {
                    if (take_ev(s, &r, &pst) != 0) {
                        break;
                    }
                    if (pst == MS_EOS || pst == MS_AGAIN) {
                        break;
                    }
                }
                ost = MS_EOS;
                break;
            }
            if (st != MS_OK) {
                ost = st;
                break;
            }
        }
        REQUIRE_ST(ost, MS_EOS);
        REQUIRE(saw_limit == 1, "dash http backpressure LIMIT");
        REQUIRE(r.nau >= 8, "dash bp all au");
        ms_dash_destroy(d);
        ms_session_destroy(s);
        ms_net_destroy(n);
        d = NULL;
        s = NULL;
        n = NULL;
        ms_synth_free(&tsbp);
    }

    /* 7) ContentProtection 明确拒绝，不把密文当媒体 */
    REQUIRE_ST(ms_net_create(NULL, &nlim, &n), MS_OK);
    REQUIRE_ST(ms_net_worker_start(n), MS_OK);
    REQUIRE_ST(ms_session_create(NULL, NULL, &s), MS_OK);
    REQUIRE_ST(ms_dash_create(NULL, &dlim, &d), MS_OK);
    ms_net_hls_fetch_bind(n, &fe);
    REQUIRE_ST(ms_dash_bind(d, s, &fe), MS_OK);
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/dash/drm.mpd", hs->port);
    REQUIRE_ST(ms_dash_open(d, url), MS_OK);
    memset(&r, 0, sizeof(r));
    REQUIRE(dash_pump(d, s, &r, 2000, &ost) == 0 || ost == MS_ERR_UNSUPPORTED,
            "dash drm pump");
    REQUIRE_ST(ost, MS_ERR_UNSUPPORTED);
    REQUIRE(r.nau == 0, "dash drm no au");
    printf("  dash via http -> AU ok; range/miss/trunc/cancel/backpressure/drm ok\n");
done:
    ms_dash_destroy(d);
    ms_session_destroy(s);
    ms_net_destroy(n);
    return failed;
}

int main(int argc, char **argv)
{
    const char *cadir;
    char ca[512], cert[512], key[512], wcert[512], wkey[512];
    test_srv http, https;
    ms_synth_buf ts;
    uint8_t a0[64], a1[64];
    size_t l0, l1, off1, off2;
    char media_pl[512], master_pl[256];
    ms_synth_stream st;
    int rc = 0;

    if (argc < 2) {
        fprintf(stderr, "usage: test_net <net-ca-dir>\n");
        return 2;
    }
    cadir = argv[1];
    snprintf(ca, sizeof(ca), "%s/ca.crt", cadir);
    snprintf(cert, sizeof(cert), "%s/server.crt", cadir);
    snprintf(key, sizeof(key), "%s/server.key", cadir);
    snprintf(wcert, sizeof(wcert), "%s/wrong.crt", cadir);
    snprintf(wkey, sizeof(wkey), "%s/wrong.key", cadir);

    OPENSSL_init_ssl(0, NULL);
    printf("test_net: controlled HTTP/HTTPS + HLS/DASH fetch\n");

    if (make_adts(a0, sizeof(a0), &l0, 3, 2, 16, 0xA0) != 0 ||
        make_adts(a1, sizeof(a1), &l1, 3, 2, 16, 0xA1) != 0) {
        fprintf(stderr, "adts fail\n");
        return 1;
    }
    memset(&ts, 0, sizeof(ts));
    ms_synth_reset_cc();
    if (ms_synth_init(&ts) != 0) {
        return 1;
    }
    memset(&st, 0, sizeof(st));
    st.stream_type = 0x0f;
    st.pid = 0x100;
    if (ms_synth_pat_pmt(&ts, 1, 0x1000, &st, 1, 0, 188) != 0) {
        return 1;
    }
    off1 = ts.len;
    if (ms_synth_pes(&ts, 0x100, 0xc0, 1, 0, a0, l0, 188) != 0) {
        return 1;
    }
    off2 = ts.len;
    if (ms_synth_pes(&ts, 0x100, 0xc0, 1, 1920, a1, l1, 188) != 0) {
        return 1;
    }

    memset(&http, 0, sizeof(http));
    if (srv_start(&http, 0, NULL, NULL) != 0) {
        fprintf(stderr, "http server fail\n");
        return 1;
    }
    snprintf(media_pl, sizeof(media_pl),
             "#EXTM3U\n#EXT-X-TARGETDURATION:2\n#EXT-X-MEDIA-SEQUENCE:1\n"
             "#EXTINF:1.0,\nhttp://127.0.0.1:%d/s1.ts\n"
             "#EXTINF:1.0,\nhttp://127.0.0.1:%d/s3.ts\n"
             "#EXT-X-ENDLIST\n",
             http.port, http.port);
    snprintf(master_pl, sizeof(master_pl),
             "#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=100000\n"
             "http://127.0.0.1:%d/media.m3u8\n",
             http.port);
    http.media_pl = media_pl;
    http.master_pl = master_pl;
    http.ts1 = ts.data;
    http.ts1_len = off2;
    http.ts3 = ts.data + off1;
    http.ts3_len = ts.len - off1;
    http.payload = "ABCDEFGHIJ";
    http.payload_len = 10;

    if (srv_start(&https, 1, cert, key) != 0) {
        fprintf(stderr, "https server fail (certs in %s?)\n", cadir);
        srv_stop(&http);
        ms_synth_free(&ts);
        return 1;
    }

    {
        ms_synth_buf ts2;
        ms_synth_stream st2;
        memset(&ts2, 0, sizeof(ts2));
        ms_synth_reset_cc();
        if (ms_synth_init(&ts2) != 0) {
            srv_stop(&https);
            srv_stop(&http);
            ms_synth_free(&ts);
            return 1;
        }
        memset(&st2, 0, sizeof(st2));
        st2.stream_type = 0x0f;
        st2.pid = 0x100;
        if (ms_synth_pat_pmt(&ts2, 1, 0x1000, &st2, 1, 0, 188) != 0 ||
            ms_synth_pes(&ts2, 0x100, 0xc0, 1, 1920, a1, l1, 188) != 0) {
            srv_stop(&https);
            srv_stop(&http);
            ms_synth_free(&ts2);
            ms_synth_free(&ts);
            return 1;
        }

        rc |= test_http_plain(&http);
        rc |= test_timeout_cancel(&http);
        rc |= test_tls(&https, &http, ca, cadir);
        rc |= test_bad_hostname(ca, wcert, wkey);
        rc |= test_hls_http(&http);
        rc |= test_dash_http(&http, ts.data, off2, ts.len, ts2.data, ts2.len);

        srv_stop(&https);
        srv_stop(&http);
        ms_synth_free(&ts2);
        ms_synth_free(&ts);
    }
    if (log_leaked) {
        fprintf(stderr, "FAIL: net log contained URL-like text\n");
        failed = 1;
    }
    if (failed || rc) {
        fprintf(stderr, "test_net FAILED\n");
        return 1;
    }
    printf("test_net ok\n");
    return 0;
}
