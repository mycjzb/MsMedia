/*
 * OpenSSL 3 高层 TLS 客户端：校验证书链与主机名，SNI，最低 TLS 1.2。
 * 不提供关闭校验的开关，不降级。
 * by: Long <mycjzb@gmail.com>
 */
#include "ms_tls.h"

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include <limits.h>
#include <stdlib.h>
#include <string.h>

struct ms_tls_ctx {
    SSL_CTX *ctx;
};

struct ms_tls_conn {
    SSL *ssl;
    int want_write;
};

static void discard_err_queue(void)
{
    while (ERR_get_error() != 0) {
    }
}

ms_status ms_tls_ctx_new(const char *ca_file, ms_tls_ctx **out)
{
    ms_tls_ctx *c;
    SSL_CTX *ctx;
    if (out == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    *out = NULL;
    if (OPENSSL_init_ssl(OPENSSL_INIT_LOAD_SSL_STRINGS |
                             OPENSSL_INIT_LOAD_CRYPTO_STRINGS,
                         NULL) == 0) {
        return MS_ERR_UNKNOWN;
    }
    ctx = SSL_CTX_new(TLS_client_method());
    if (ctx == NULL) {
        discard_err_queue();
        return MS_ERR_NOMEM;
    }
    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL);
    SSL_CTX_set_default_verify_paths(ctx);
    if (ca_file != NULL && ca_file[0] != 0) {
        if (SSL_CTX_load_verify_locations(ctx, ca_file, NULL) != 1) {
            SSL_CTX_free(ctx);
            discard_err_queue();
            return MS_ERR_INVALID_ARG;
        }
    }
    if (SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION) != 1) {
        SSL_CTX_free(ctx);
        discard_err_queue();
        return MS_ERR_UNKNOWN;
    }
    SSL_CTX_set_options(ctx, SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3 | SSL_OP_NO_TLSv1 |
                                 SSL_OP_NO_TLSv1_1);
    c = (ms_tls_ctx *)calloc(1, sizeof(*c));
    if (c == NULL) {
        SSL_CTX_free(ctx);
        return MS_ERR_NOMEM;
    }
    c->ctx = ctx;
    *out = c;
    return MS_OK;
}

void ms_tls_ctx_free(ms_tls_ctx *c)
{
    if (c == NULL) {
        return;
    }
    if (c->ctx != NULL) {
        SSL_CTX_free(c->ctx);
    }
    free(c);
}

ms_status ms_tls_conn_new(ms_tls_ctx *ctx, int fd, const char *hostname,
                          ms_tls_conn **out)
{
    ms_tls_conn *s;
    SSL *ssl;
    if (out == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    *out = NULL;
    if (ctx == NULL || ctx->ctx == NULL || fd < 0 || hostname == NULL ||
        hostname[0] == 0) {
        return MS_ERR_INVALID_ARG;
    }
    ssl = SSL_new(ctx->ctx);
    if (ssl == NULL) {
        discard_err_queue();
        return MS_ERR_NOMEM;
    }
    if (SSL_set_fd(ssl, fd) != 1) {
        SSL_free(ssl);
        discard_err_queue();
        return MS_ERR_UNKNOWN;
    }
    if (SSL_set_tlsext_host_name(ssl, hostname) != 1) {
        SSL_free(ssl);
        discard_err_queue();
        return MS_ERR_UNKNOWN;
    }
    if (SSL_set1_host(ssl, hostname) != 1) {
        SSL_free(ssl);
        discard_err_queue();
        return MS_ERR_UNKNOWN;
    }
    SSL_set_connect_state(ssl);
    s = (ms_tls_conn *)calloc(1, sizeof(*s));
    if (s == NULL) {
        SSL_free(ssl);
        return MS_ERR_NOMEM;
    }
    s->ssl = ssl;
    *out = s;
    return MS_OK;
}

void ms_tls_conn_free(ms_tls_conn *s)
{
    if (s == NULL) {
        return;
    }
    if (s->ssl != NULL) {
        SSL_free(s->ssl);
    }
    free(s);
}

static ms_status map_ssl_err(ms_tls_conn *s, int n)
{
    int e = SSL_get_error(s->ssl, n);
    if (e == SSL_ERROR_WANT_READ) {
        s->want_write = 0;
        return MS_AGAIN;
    }
    if (e == SSL_ERROR_WANT_WRITE) {
        s->want_write = 1;
        return MS_AGAIN;
    }
    if (e == SSL_ERROR_ZERO_RETURN) {
        return MS_OK;
    }
    s->want_write = 0;
    discard_err_queue();
    return MS_ERR_UNKNOWN;
}

ms_status ms_tls_handshake(ms_tls_conn *s)
{
    int n;
    if (s == NULL || s->ssl == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    s->want_write = 0;
    n = SSL_connect(s->ssl);
    if (n == 1) {
        if (SSL_get_verify_result(s->ssl) != X509_V_OK) {
            return MS_ERR_UNKNOWN;
        }
        return MS_OK;
    }
    return map_ssl_err(s, n);
}

ms_status ms_tls_read(ms_tls_conn *s, void *buf, size_t len, size_t *out_n)
{
    int n;
    if (out_n != NULL) {
        *out_n = 0;
    }
    if (s == NULL || s->ssl == NULL || out_n == NULL ||
        (buf == NULL && len != 0)) {
        return MS_ERR_INVALID_ARG;
    }
    if (len == 0) {
        return MS_OK;
    }
    if (len > (size_t)INT_MAX) {
        len = (size_t)INT_MAX;
    }
    s->want_write = 0;
    n = SSL_read(s->ssl, buf, (int)len);
    if (n > 0) {
        *out_n = (size_t)n;
        return MS_OK;
    }
    if (n == 0) {
        return map_ssl_err(s, n);
    }
    return map_ssl_err(s, n);
}

ms_status ms_tls_write(ms_tls_conn *s, const void *buf, size_t len,
                       size_t *out_n)
{
    int n;
    if (out_n != NULL) {
        *out_n = 0;
    }
    if (s == NULL || s->ssl == NULL || out_n == NULL ||
        (buf == NULL && len != 0)) {
        return MS_ERR_INVALID_ARG;
    }
    if (len == 0) {
        return MS_OK;
    }
    if (len > (size_t)INT_MAX) {
        len = (size_t)INT_MAX;
    }
    s->want_write = 0;
    n = SSL_write(s->ssl, buf, (int)len);
    if (n > 0) {
        *out_n = (size_t)n;
        return MS_OK;
    }
    return map_ssl_err(s, n);
}

int ms_tls_want_write(const ms_tls_conn *s)
{
    return (s != NULL && s->want_write) ? 1 : 0;
}
