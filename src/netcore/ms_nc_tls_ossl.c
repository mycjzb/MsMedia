/*
 * 公共网络层 TLS 泵 —— OpenSSL 3.5.9 极致裁剪版后端（路线 B 主力单 so，编译宏 MS_NC_TLS_OPENSSL）。
 *
 * 一份实现两种策略（用户明确要求「只有播放忽略，API 不忽略」）：
 *   - 严格（verify=1）：com.msmedia.net.MsHttpClient / ms_nc_conn_open 默认，且不可关闭。
 *       证书链按系统 CA 信任库校验（Android：/apex/com.android.conscrypt/cacerts 或
 *       /system/etc/security/cacerts；宿主：/etc/ssl/certs/ca-certificates.crt），
 *       另可经 ms_nc_tls_set_extra_ca() 追加信任（测试 CA / 企业 CA），主机名（含 IP 字面量）必须匹配。
 *   - 宽松（verify=0）：仅播放器拉流（FFmpeg https/tls/rtmps）默认使用，跳过证书链与主机名校验，
 *       兼容自签证书、IP 直连 HTTPS、老盒子过期根证书；仍然加密、仍发 SNI；可由播放器开关切回严格。
 *
 * 收发不走 socket BIO：自定义 BIO 把 OpenSSL 的读写转给调用方的 ms_nc_io 回调
 * （netcore 自研拨号 fd 或 FFmpeg tcp 子连接），因此取消 / 超时 / SOCKS5 / DNS 钩子全部沿用 netcore。
 * 只允许 TLS 1.2 / 1.3；SSL_CTX 进程内各缓存一份（线程安全，SSL_new 自带引用计数）。
 * by: Long <mycjzb@gmail.com>
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "ms_netcore.h"

#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

/* 单个 CA 文件读取上限（系统 bundle 约 200 KB，留足余量但有界）。 */
#define NC_CA_FILE_MAX (4 * 1024 * 1024)
#define NC_PATH_MAX 512

typedef struct nc_tls {
    SSL *ssl;
    const ms_nc_io *io; /* 每次握手/读/写前设置，BIO 回调经它收发 */
    int io_err;         /* 底层 io 返回的负值错误（超时/取消），透传给调用方 */
} nc_tls;

static pthread_mutex_t g_tls_mu = PTHREAD_MUTEX_INITIALIZER;
static SSL_CTX *g_ctx_strict;   /* 严格：系统 CA + 追加 CA，校验链与主机名 */
static int g_ca_loaded = -1;   /* 严格信任库实际载入的 CA 数（诊断用） */
static SSL_CTX *g_ctx_insecure; /* 宽松：仅播放器拉流 */
static BIO_METHOD *g_bio_meth;
static char g_extra_ca_file[NC_PATH_MAX];
static char g_extra_ca_dir[NC_PATH_MAX];

/* ---------------- 自定义 BIO：转发到 ms_nc_io ---------------- */
static int bio_rd(BIO *b, char *out, int n)
{
    nc_tls *t = (nc_tls *)BIO_get_data(b);
    int r;
    BIO_clear_retry_flags(b);
    if (t == NULL || t->io == NULL || n <= 0) return -1;
    r = t->io->rd(t->io->ctx, (uint8_t *)out, n);
    if (r < 0) {
        t->io_err = r;
        return -1;
    }
    return r; /* 0 = 对端关闭 */
}

static int bio_wr(BIO *b, const char *in, int n)
{
    nc_tls *t = (nc_tls *)BIO_get_data(b);
    int off = 0;
    BIO_clear_retry_flags(b);
    if (t == NULL || t->io == NULL) return -1;
    while (off < n) {
        int w = t->io->wr(t->io->ctx, (const uint8_t *)in + off, n - off);
        if (w <= 0) {
            t->io_err = w < 0 ? w : MS_NC_E_IO;
            return -1;
        }
        off += w;
    }
    return n;
}

static long bio_ctrl(BIO *b, int cmd, long num, void *ptr)
{
    (void)b; (void)num; (void)ptr;
    return cmd == BIO_CTRL_FLUSH ? 1 : 0;
}

static int bio_create(BIO *b)
{
    BIO_set_init(b, 1);
    return 1;
}

/* ---------------- CA 信任库（严格模式） ---------------- */
/* 读整个文件到内存（有界）；返回 malloc 缓冲，调用方 free。 */
static char *read_file_bounded(const char *path, size_t *out_n)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    struct stat st;
    char *buf;
    size_t off = 0;
    if (fd < 0) return NULL;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0 || st.st_size > NC_CA_FILE_MAX) {
        close(fd);
        return NULL;
    }
    buf = (char *)malloc((size_t)st.st_size);
    if (buf == NULL) {
        close(fd);
        return NULL;
    }
    while (off < (size_t)st.st_size) {
        ssize_t r = read(fd, buf + off, (size_t)st.st_size - off);
        if (r <= 0) break;
        off += (size_t)r;
    }
    close(fd);
    *out_n = off;
    return buf;
}

/* 把一个 PEM 文件里的全部证书加入 store（Android cacerts 每文件一张、附带文本说明，PEM 读取会跳过）。 */
static int store_add_pem_file(X509_STORE *st, const char *path)
{
    size_t n = 0;
    char *buf = read_file_bounded(path, &n);
    BIO *mb;
    X509 *x;
    int added = 0;
    if (buf == NULL) return 0;
    mb = BIO_new_mem_buf(buf, (int)n);
    if (mb != NULL) {
        while ((x = PEM_read_bio_X509(mb, NULL, NULL, NULL)) != NULL) {
            if (X509_STORE_add_cert(st, x) == 1) added++;
            X509_free(x);
        }
        BIO_free(mb);
    }
    ERR_clear_error(); /* 读到文件尾的“无起始行”属正常结束 */
    free(buf);
    return added;
}

static int store_add_dir(X509_STORE *st, const char *dir)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    char path[NC_PATH_MAX * 2];
    int added = 0;
    if (d == NULL) return 0;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        added += store_add_pem_file(st, path);
    }
    closedir(d);
    return added;
}

/* 系统 CA：Android 14 起以 APEX 内的 conscrypt 为准，老系统用 /system/etc/security/cacerts。 */
static int store_add_system(X509_STORE *st)
{
#ifdef __ANDROID__
    int n = store_add_dir(st, "/apex/com.android.conscrypt/cacerts");
    if (n == 0) n = store_add_dir(st, "/system/etc/security/cacerts");
    return n;
#else
    int n = store_add_pem_file(st, "/etc/ssl/certs/ca-certificates.crt");
    if (n == 0) n = store_add_pem_file(st, "/etc/pki/tls/certs/ca-bundle.crt");
    if (n == 0) n = store_add_pem_file(st, "/etc/ssl/cert.pem");
    return n;
#endif
}

/* 公共：两种策略共用的 CTX 基础配置（只开 TLS1.2/1.3、关压缩/重协商）。 */
static SSL_CTX *ctx_new_common(void)
{
    SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
    if (ctx == NULL) return NULL;
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
    SSL_CTX_set_options(ctx, SSL_OP_NO_COMPRESSION | SSL_OP_NO_RENEGOTIATION);
    SSL_CTX_set_mode(ctx, SSL_MODE_AUTO_RETRY);
    /* 只协商裁剪后保留的组：P-256 优先（RFC 8446 强制实现），P-384 兜底；X25519 已随 no-ecx 裁掉。 */
    (void)SSL_CTX_set1_groups_list(ctx, "P-256:P-384");
    return ctx;
}

static SSL_CTX *ctx_get(int verify)
{
    SSL_CTX *ctx;
    pthread_mutex_lock(&g_tls_mu);
    if (g_bio_meth == NULL) {
        g_bio_meth = BIO_meth_new(BIO_get_new_index() | BIO_TYPE_SOURCE_SINK, "ms_nc_io");
        if (g_bio_meth != NULL) {
            BIO_meth_set_read(g_bio_meth, bio_rd);
            BIO_meth_set_write(g_bio_meth, bio_wr);
            BIO_meth_set_ctrl(g_bio_meth, bio_ctrl);
            BIO_meth_set_create(g_bio_meth, bio_create);
        }
    }
    if (verify && g_ctx_strict == NULL) {
        SSL_CTX *c = ctx_new_common();
        if (c != NULL) {
            X509_STORE *st = SSL_CTX_get_cert_store(c);
            int n = store_add_system(st);
            if (g_extra_ca_file[0]) n += store_add_pem_file(st, g_extra_ca_file);
            if (g_extra_ca_dir[0]) n += store_add_dir(st, g_extra_ca_dir);
            /* 信任库为空也照样严格：任何证书都会校验失败，绝不放行。 */
            g_ca_loaded = n;
            SSL_CTX_set_verify(c, SSL_VERIFY_PEER, NULL);
        }
        g_ctx_strict = c;
    } else if (!verify && g_ctx_insecure == NULL) {
        SSL_CTX *c = ctx_new_common();
        if (c != NULL) SSL_CTX_set_verify(c, SSL_VERIFY_NONE, NULL);
        g_ctx_insecure = c;
    }
    ctx = verify ? g_ctx_strict : g_ctx_insecure;
    if (ctx != NULL) SSL_CTX_up_ref(ctx);
    pthread_mutex_unlock(&g_tls_mu);
    return ctx;
}

int ms_nc_tls_set_extra_ca(const char *ca_file, const char *ca_dir)
{
    if ((ca_file && strlen(ca_file) >= NC_PATH_MAX) || (ca_dir && strlen(ca_dir) >= NC_PATH_MAX)) {
        return MS_NC_E_ARG;
    }
    pthread_mutex_lock(&g_tls_mu);
    snprintf(g_extra_ca_file, sizeof(g_extra_ca_file), "%s", ca_file ? ca_file : "");
    snprintf(g_extra_ca_dir, sizeof(g_extra_ca_dir), "%s", ca_dir ? ca_dir : "");
    /* 丢弃缓存的严格 CTX，下次连接按新信任库重建（在途连接各自持有引用，不受影响）。 */
    SSL_CTX_free(g_ctx_strict);
    g_ctx_strict = NULL;
    pthread_mutex_unlock(&g_tls_mu);
    return MS_NC_OK;
}

void ms_nc_tls_global_cleanup(void)
{
    pthread_mutex_lock(&g_tls_mu);
    SSL_CTX_free(g_ctx_strict);
    SSL_CTX_free(g_ctx_insecure);
    BIO_meth_free(g_bio_meth);
    g_ctx_strict = g_ctx_insecure = NULL;
    g_bio_meth = NULL;
    pthread_mutex_unlock(&g_tls_mu);
}

const char *ms_nc_tls_backend(void)
{
    return "openssl-3.5.9-slim";
}

/* ---------------- TLS 泵 ---------------- */
/* 诊断：设置环境变量 MS_NC_TLS_DEBUG=1 时，握手失败把校验结果打到 stderr（只有错误文本与 CA 数量，不含 URL）。 */
static void tls_debug_fail(nc_tls *t)
{
    if (getenv("MS_NC_TLS_DEBUG") == NULL) return;
    {
        unsigned long e = ERR_peek_error(); /* no-err 构建没有文案，打 lib/reason 数字，可对照 include/openssl 下的 err 头 */
        fprintf(stderr, "ms_nc_tls: handshake failed verify=%ld (%s) ca_loaded=%d io_err=%d err_lib=%d err_reason=%d\n",
                (long)SSL_get_verify_result(t->ssl), X509_verify_cert_error_string(SSL_get_verify_result(t->ssl)),
                g_ca_loaded, t->io_err, ERR_GET_LIB(e), ERR_GET_REASON(e));
    }
}

static int host_is_ip(const char *h)
{
    return strchr(h, ':') != NULL || strspn(h, "0123456789.") == strlen(h);
}

int ms_nc_tls_open2(const char *hostname, int verify, void **out_eng)
{
    nc_tls *t;
    SSL_CTX *ctx;
    BIO *b;
    if (out_eng == NULL) return MS_NC_E_ARG;
    *out_eng = NULL;
    if (hostname == NULL || hostname[0] == 0) return MS_NC_E_ARG;
    ctx = ctx_get(verify ? 1 : 0);
    if (ctx == NULL || g_bio_meth == NULL) {
        SSL_CTX_free(ctx);
        return MS_NC_E_TLS;
    }
    t = (nc_tls *)calloc(1, sizeof(*t));
    if (t == NULL) {
        SSL_CTX_free(ctx);
        return MS_NC_E_NOMEM;
    }
    t->ssl = SSL_new(ctx);
    SSL_CTX_free(ctx); /* SSL 自己持有引用 */
    if (t->ssl == NULL) goto fail;
    b = BIO_new(g_bio_meth);
    if (b == NULL) goto fail;
    BIO_set_data(b, t);
    SSL_set_bio(t->ssl, b, b);
    if (!host_is_ip(hostname)) {
        (void)SSL_set_tlsext_host_name(t->ssl, hostname); /* SNI：两种策略都发 */
    }
    if (verify && SSL_set1_host(t->ssl, hostname) != 1) goto fail; /* 主机名 / IP 校验 */
    SSL_set_connect_state(t->ssl);
    *out_eng = t;
    return MS_NC_OK;
fail:
    if (t->ssl) SSL_free(t->ssl);
    free(t);
    ERR_clear_error();
    return MS_NC_E_TLS;
}

int ms_nc_tls_open(const char *hostname, void **out_eng)
{
    /* 公共 API（MsHttpClient / ms_nc_conn）：永远严格校验。 */
    return ms_nc_tls_open2(hostname, 1, out_eng);
}

void ms_nc_tls_close(void *eng)
{
    nc_tls *t = (nc_tls *)eng;
    if (t == NULL) return;
    /* 不发 close_notify：底层连接可能已被取消关闭，避免关闭阶段阻塞。 */
    SSL_free(t->ssl);
    free(t);
    ERR_clear_error();
}

/* 公共：把 SSL_* 返回值换成 netcore 错误码（优先透传底层 io 的超时/取消）。 */
static int map_err(nc_tls *t, int r)
{
    int e = SSL_get_error(t->ssl, r);
    ERR_clear_error();
    if (e == SSL_ERROR_ZERO_RETURN) return 0;
    if (t->io_err < 0) return t->io_err;
    if (e == SSL_ERROR_SYSCALL) return 0; /* 对端直接断开，按 EOF */
    return MS_NC_E_TLS;
}

int ms_nc_tls_handshake(void *eng, const ms_nc_io *io)
{
    nc_tls *t = (nc_tls *)eng;
    int r;
    if (t == NULL || io == NULL) return MS_NC_E_ARG;
    t->io = io;
    t->io_err = 0;
    r = SSL_do_handshake(t->ssl);
    if (r == 1) return MS_NC_OK;
    tls_debug_fail(t);
    r = map_err(t, r);
    return r < 0 ? r : MS_NC_E_TLS; /* 证书链 / 主机名不通过都在这里：不重试、不降级 */
}

int ms_nc_tls_read(void *eng, const ms_nc_io *io, uint8_t *buf, int cap)
{
    nc_tls *t = (nc_tls *)eng;
    int r;
    if (t == NULL || io == NULL || buf == NULL || cap <= 0) return MS_NC_E_ARG;
    t->io = io;
    t->io_err = 0;
    r = SSL_read(t->ssl, buf, cap);
    return r > 0 ? r : map_err(t, r);
}

int ms_nc_tls_write(void *eng, const ms_nc_io *io, const uint8_t *buf, int n)
{
    nc_tls *t = (nc_tls *)eng;
    int r;
    if (t == NULL || io == NULL || (buf == NULL && n > 0)) return MS_NC_E_ARG;
    if (n == 0) return 0;
    t->io = io;
    t->io_err = 0;
    r = SSL_write(t->ssl, buf, n); /* AUTO_RETRY + 阻塞 BIO：成功即全部写完 */
    if (r > 0) return r;
    r = map_err(t, r);
    return r < 0 ? r : MS_NC_E_TLS;
}
