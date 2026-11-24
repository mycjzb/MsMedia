/*
 * 宿主 netcore（共用网络层）测试：HTTP GET/POST/自定义头/chunked/重定向/循环重定向上限/
 * 读到关闭/超时/取消（跨线程）/DNS 映射/DNS 原生钩子/SOCKS5（无认证+认证+远端域名解析）/
 * 头注入拒绝/URL 内嵌凭据拒绝/https 在宿主诚实返回 TLS 不可用（设备上走系统 SSLEngine）。
 * 本地服务由 tests/single_netcore_servers.py 提供，端口从 argv 传入。
 * by: Long <mycjzb@gmail.com>
 */
#include "../src/netcore/ms_netcore.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

static int fails;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL %s\n", m); fails++; } else { printf("ok %s\n", m); } } while (0)

static int hp, s5p, s5ap, stp;

static int fetch(const char *method, const char *path_or_url, const char *hdrs, const char *body,
                 char *out, int cap, int *status, int max_redir)
{
    char url[512];
    ms_nc_req r;
    ms_nc_http *h = NULL;
    int st, n = 0;
    if (strstr(path_or_url, "://")) snprintf(url, sizeof(url), "%s", path_or_url);
    else snprintf(url, sizeof(url), "http://127.0.0.1:%d%s", hp, path_or_url);
    memset(&r, 0, sizeof(r));
    r.method = method;
    r.url = url;
    r.headers = hdrs;
    r.body = (const uint8_t *)body;
    r.body_len = body ? strlen(body) : 0;
    r.connect_timeout_ms = 3000;
    r.read_timeout_ms = 3000;
    r.max_redirects = max_redir;
    st = ms_nc_http_open(&r, &h);
    if (st != MS_NC_OK) return st;
    if (status) *status = ms_nc_http_status(h);
    for (;;) {
        int k = ms_nc_http_read(h, (uint8_t *)out + n, cap - 1 - n);
        if (k <= 0) { if (k < 0) n = k; break; }
        n += k;
        if (n >= cap - 1) break;
    }
    if (n >= 0) out[n] = 0;
    ms_nc_http_close(h);
    return n;
}

static int hook_calls;
static int my_dns(void *ctx, const char *host, int port, struct sockaddr_storage *out, int cap)
{
    struct sockaddr_in *sa = (struct sockaddr_in *)out;
    (void)ctx;
    if (strcmp(host, "hooked.test") != 0 || cap < 1) return 0; /* 交给下一级 */
    hook_calls++;
    memset(sa, 0, sizeof(*sa));
    sa->sin_family = AF_INET;
    sa->sin_port = htons((unsigned short)port);
    sa->sin_addr.s_addr = htonl(0x7f000001);
    return 1;
}

typedef struct { ms_nc_http *h; volatile int *flag; } cancel_arg;
static void *cancel_th(void *a)
{
    cancel_arg *c = (cancel_arg *)a;
    usleep(300 * 1000);
    if (c->flag) *c->flag = 1;
    if (c->h) ms_nc_http_cancel(c->h);
    return NULL;
}

int main(int argc, char **argv)
{
    static char buf[200000];
    int st = 0, n;
    ms_nc_config cfg;
    if (argc < 5) return 2;
    hp = atoi(argv[1]); s5p = atoi(argv[2]); s5ap = atoi(argv[3]); stp = atoi(argv[4]);

    n = fetch("GET", "/plain", NULL, NULL, buf, sizeof(buf), &st, -1);
    CHECK(n == 1300 && st == 200 && strncmp(buf, "hello-netcore", 13) == 0, "GET content-length body");
    n = fetch("GET", "/chunked", NULL, NULL, buf, sizeof(buf), &st, -1);
    CHECK(n == 5008 && strncmp(buf, "abcdefgh", 8) == 0, "GET chunked");
    n = fetch("POST", "/echo", "Content-Type: text/plain\n", "payload-123", buf, sizeof(buf), &st, -1);
    CHECK(n > 0 && strcmp(buf, "echo:payload-123") == 0, "POST body echo");
    n = fetch("GET", "/hdr", "X-Custom: v42\n", NULL, buf, sizeof(buf), &st, -1);
    CHECK(n > 0 && strcmp(buf, "custom=v42") == 0, "custom header sent");
    n = fetch("GET", "/redir", NULL, NULL, buf, sizeof(buf), &st, -1);
    CHECK(n == 1300 && st == 200, "302 redirect followed");
    n = fetch("GET", "/redir", NULL, NULL, buf, sizeof(buf), &st, 0);
    CHECK(n == 0 && st == 302, "redirect not followed when max_redirects=0");
    n = fetch("GET", "/loop", NULL, NULL, buf, sizeof(buf), &st, -1);
    CHECK(n == MS_NC_E_REDIRECT, "redirect loop capped");
    n = fetch("GET", "/close", NULL, NULL, buf, sizeof(buf), &st, -1);
    CHECK(n == 11 && strcmp(buf, "until-close") == 0, "read until close");
    n = fetch("GET", "/plain", "X-Bad: a\r\nInjected: 1\n", NULL, buf, sizeof(buf), &st, -1);
    CHECK(n == MS_NC_E_ARG, "CR header injection rejected");
    n = fetch("GET", "/plain", "Host: evil\n", NULL, buf, sizeof(buf), &st, -1);
    CHECK(n == MS_NC_E_ARG, "Host override rejected");
    {
        char u[256];
        snprintf(u, sizeof(u), "http://user:pw@127.0.0.1:%d/plain", hp);
        n = fetch("GET", u, NULL, NULL, buf, sizeof(buf), &st, -1);
        CHECK(n == MS_NC_E_UNSUPPORTED, "credentials in URL rejected");
    }
    n = fetch("GET", "https://127.0.0.1:1/x", NULL, NULL, buf, sizeof(buf), &st, -1);
    CHECK(n == MS_NC_E_TLS || n == MS_NC_E_CONNECT, "https on host: no fake TLS (TLS/connect error)");

    /* Keep-Alive 连接池：定长/chunked 完整读完后复用；读到关闭的响应不回池 */
    {
        int o0, r0, o1, r1, k, okall = 1;
        ms_nc_pool_stats(&o0, &r0);
        for (k = 0; k < 4; k++) {
            ms_nc_req r; ms_nc_http *h = NULL; char u[128]; int m, tot = 0;
            snprintf(u, sizeof(u), "http://127.0.0.1:%d/%s", hp, (k & 1) ? "chunked" : "plain");
            memset(&r, 0, sizeof(r)); r.url = u; r.max_redirects = -1; r.keep_alive = 1;
            if (ms_nc_http_open(&r, &h) != MS_NC_OK) { okall = 0; break; }
            do { m = ms_nc_http_read(h, (uint8_t *)buf, 4096); if (m > 0) tot += m; } while (m > 0);
            if (m < 0 || tot != ((k & 1) ? 5008 : 1300)) okall = 0;
            ms_nc_http_close(h);
        }
        ms_nc_pool_stats(&o1, &r1);
        printf("keepalive opened=%d reused=%d\n", o1 - o0, r1 - r0);
        CHECK(okall && o1 - o0 == 1 && r1 - r0 == 3, "keep-alive: 4 requests over 1 connection");
        {
            ms_nc_req r; ms_nc_http *h = NULL; char u[128]; int m;
            snprintf(u, sizeof(u), "http://127.0.0.1:%d/close", hp);
            memset(&r, 0, sizeof(r)); r.url = u; r.max_redirects = -1; r.keep_alive = 1;
            CHECK(ms_nc_http_open(&r, &h) == MS_NC_OK, "keep-alive open read-until-close");
            do { m = ms_nc_http_read(h, (uint8_t *)buf, 4096); } while (m > 0);
            ms_nc_http_close(h);
        }
        {
            ms_nc_req r; ms_nc_http *h = NULL; char u[128]; int m, o2, r2;
            ms_nc_pool_stats(&o1, &r1);
            snprintf(u, sizeof(u), "http://127.0.0.1:%d/plain", hp);
            memset(&r, 0, sizeof(r)); r.url = u; r.max_redirects = -1; r.keep_alive = 1;
            CHECK(ms_nc_http_open(&r, &h) == MS_NC_OK, "keep-alive after close-delimited");
            /* 只读一半就关：不得回池（残留响应体会污染下个请求） */
            m = ms_nc_http_read(h, (uint8_t *)buf, 100);
            ms_nc_http_close(h);
            memset(&r, 0, sizeof(r)); r.url = u; r.max_redirects = -1; r.keep_alive = 1;
            h = NULL;
            CHECK(ms_nc_http_open(&r, &h) == MS_NC_OK, "keep-alive after partial read");
            { int tot = 0; do { m = ms_nc_http_read(h, (uint8_t *)buf, 4096); if (m > 0) tot += m; } while (m > 0);
              CHECK(tot == 1300 && strncmp(buf, "hello-netcore", 13) == 0, "partial-read conn not reused (clean body)"); }
            ms_nc_http_close(h);
            ms_nc_pool_stats(&o2, &r2);
            printf("keepalive2 opened=%d reused=%d\n", o2 - o1, r2 - r1);
        }
        ms_nc_pool_clear();
    }
    /* 读超时 */
    {
        ms_nc_req r; ms_nc_http *h = NULL; char u[128]; int k, tot = 0;
        snprintf(u, sizeof(u), "http://127.0.0.1:%d/slow", hp);
        memset(&r, 0, sizeof(r)); r.url = u; r.read_timeout_ms = 100; r.connect_timeout_ms = 2000; r.max_redirects = -1;
        CHECK(ms_nc_http_open(&r, &h) == MS_NC_OK, "slow open");
        do { k = ms_nc_http_read(h, (uint8_t *)buf, 100); if (k > 0) tot += k; } while (k > 0);
        CHECK(k == MS_NC_E_TIMEOUT, "read timeout reported");
        ms_nc_http_close(h);
    }
    /* 跨线程取消（读取中） */
    {
        ms_nc_req r; ms_nc_http *h = NULL; char u[128]; int k; pthread_t t; cancel_arg ca;
        snprintf(u, sizeof(u), "http://127.0.0.1:%d/slow", hp);
        memset(&r, 0, sizeof(r)); r.url = u; r.read_timeout_ms = 10000; r.max_redirects = -1;
        CHECK(ms_nc_http_open(&r, &h) == MS_NC_OK, "slow open for cancel");
        ca.h = h; ca.flag = NULL;
        pthread_create(&t, NULL, cancel_th, &ca);
        do { k = ms_nc_http_read(h, (uint8_t *)buf, 100); } while (k > 0);
        pthread_join(t, NULL);
        CHECK(k == MS_NC_E_CANCELED || k == MS_NC_E_IO, "cancel from other thread unblocks read");
        ms_nc_http_close(h);
    }
    /* 打开前取消标志 */
    {
        volatile int cf = 1; ms_nc_req r; ms_nc_http *h = NULL; char u[128];
        snprintf(u, sizeof(u), "http://127.0.0.1:%d/plain", hp);
        memset(&r, 0, sizeof(r)); r.url = u; r.cancel = &cf; r.max_redirects = -1;
        CHECK(ms_nc_http_open(&r, &h) == MS_NC_E_CANCELED && h == NULL, "pre-set cancel flag aborts open");
    }
    /* DNS 映射 + 原生钩子 */
    ms_nc_config_default(&cfg);
    snprintf(cfg.dns_map, sizeof(cfg.dns_map), "media.test=127.0.0.1;other.test=10.255.255.1");
    CHECK(ms_nc_configure(&cfg) == MS_NC_OK, "configure dns map");
    {
        char u[128]; snprintf(u, sizeof(u), "http://media.test:%d/plain", hp);
        n = fetch("GET", u, NULL, NULL, buf, sizeof(buf), &st, -1);
        CHECK(n == 1300, "dns map host override");
    }
    ms_nc_set_dns_hook(my_dns, NULL);
    {
        char u[128]; snprintf(u, sizeof(u), "http://hooked.test:%d/plain", hp);
        n = fetch("GET", u, NULL, NULL, buf, sizeof(buf), &st, -1);
        CHECK(n == 1300 && hook_calls > 0, "native dns hook used");
    }
    ms_nc_set_dns_hook(NULL, NULL);
    /* SOCKS5 无认证，远端解析（域名 ATYP=3） */
    ms_nc_config_default(&cfg);
    snprintf(cfg.socks_host, sizeof(cfg.socks_host), "127.0.0.1");
    cfg.socks_port = s5p;
    cfg.socks_remote_dns = 1;
    CHECK(ms_nc_configure(&cfg) == MS_NC_OK, "configure socks5");
    {
        char u[128]; snprintf(u, sizeof(u), "http://mapped.test:%d/plain", hp);
        n = fetch("GET", u, NULL, NULL, buf, sizeof(buf), &st, -1);
        CHECK(n == 1300, "socks5 remote-dns GET");
    }
    /* SOCKS5 用户名密码 */
    snprintf(cfg.socks_user, sizeof(cfg.socks_user), "user");
    snprintf(cfg.socks_pass, sizeof(cfg.socks_pass), "pass");
    cfg.socks_port = s5ap;
    ms_nc_configure(&cfg);
    n = fetch("GET", "/plain", NULL, NULL, buf, sizeof(buf), &st, -1);
    CHECK(n == 1300, "socks5 user/pass GET");
    snprintf(cfg.socks_pass, sizeof(cfg.socks_pass), "wrong");
    ms_nc_configure(&cfg);
    n = fetch("GET", "/plain", NULL, NULL, buf, sizeof(buf), &st, -1);
    CHECK(n == MS_NC_E_PROXY, "socks5 bad password rejected");
    ms_nc_config_default(&cfg);
    ms_nc_configure(&cfg);
    {
        char u[128]; snprintf(u, sizeof(u), "http://127.0.0.1:%d/", stp);
        n = fetch("GET", u, NULL, NULL, buf, sizeof(buf), &st, -1);
        printf("socks stats: %s\n", n > 0 ? buf : "?");
    }
    printf("%s fails=%d\n", fails ? "NETCORE_FAIL" : "NETCORE_PASS", fails);
    return fails ? 1 : 0;
}
