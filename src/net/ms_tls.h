/*
 * OpenSSL 高层封装：仅可选网络库内部使用。
 * by: Long <mycjzb@gmail.com>
 */
#ifndef MS_NET_TLS_H
#define MS_NET_TLS_H

#include "msmedia/msmedia.h"

struct ms_tls_ctx;
struct ms_tls_conn;

typedef struct ms_tls_ctx ms_tls_ctx;
typedef struct ms_tls_conn ms_tls_conn;

ms_status ms_tls_ctx_new(const char *ca_file, ms_tls_ctx **out);
void ms_tls_ctx_free(ms_tls_ctx *c);

/* hostname 用于 SNI 与证书主机名校验，不得为空。 */
ms_status ms_tls_conn_new(ms_tls_ctx *ctx, int fd, const char *hostname,
                          ms_tls_conn **out);
void ms_tls_conn_free(ms_tls_conn *s);

/* 握手/读写：MS_AGAIN 表示 WANT_READ/WRITE。 */
ms_status ms_tls_handshake(ms_tls_conn *s);
ms_status ms_tls_read(ms_tls_conn *s, void *buf, size_t len, size_t *out_n);
ms_status ms_tls_write(ms_tls_conn *s, const void *buf, size_t len,
                       size_t *out_n);
int ms_tls_want_write(const ms_tls_conn *s);

#endif
