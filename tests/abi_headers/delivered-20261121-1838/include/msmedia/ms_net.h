/*
 * 可选 HTTP/HTTPS 客户端（独立网络库，不链入 libmsmedia 核心）。
 * HTTP/1.1 GET；HTTPS 经系统 OpenSSL 高层 API（证书链 + 主机名/SNI，
 * 不可关闭校验，默认不低于 TLS 1.2，禁止 TLS 降级）。
 * 不是 libcurl，不是 shell 出站 curl/ffmpeg。
 * by: Long <mycjzb@gmail.com>
 */
#ifndef MSMEDIA_MS_NET_H
#define MSMEDIA_MS_NET_H

#include "msmedia/msmedia.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ms_net ms_net;

typedef struct ms_net_limits {
    size_t struct_size;
    size_t max_url_bytes;     /* 0=默认 4096 */
    size_t max_header_bytes;  /* 0=默认 16 KiB */
    size_t max_buffer_bytes;  /* 0=默认 64 KiB  Staging / 反压 */
    int max_redirects;        /* <0 默认 5 */
    int connect_timeout_ms;   /* 0=默认 5000 */
    int io_timeout_ms;        /* 0=默认 15000 */
    int max_retries;          /* <0 默认 0；仅连接失败，不重试 4xx/校验失败 */
} ms_net_limits;

typedef enum ms_net_err {
    MS_NET_OK = 0,
    MS_NET_AGAIN,
    MS_NET_ERR_TIMEOUT,
    MS_NET_ERR_TLS,           /* 证书链/主机名/握手失败 */
    MS_NET_ERR_HTTP,          /* 见 ms_net_last_http_status */
    MS_NET_ERR_REDIRECT,      /* 超限、循环或 https→http 降级 */
    MS_NET_ERR_DNS,
    MS_NET_ERR_IO,
    MS_NET_ERR_MALFORMED,
    MS_NET_ERR_OVERFLOW,
    MS_NET_ERR_CANCELED,
    MS_NET_ERR_NOMEM,
    MS_NET_ERR_STATE,
    MS_NET_ERR_INVALID_ARG,
    MS_NET_ERR_UNSUPPORTED
} ms_net_err;

typedef void (*ms_net_log_fn)(void *ctx, int level, const char *msg);

MSMEDIA_API void ms_net_limits_default(ms_net_limits *out);

MSMEDIA_API ms_status ms_net_create(const ms_allocator *alloc, const ms_net_limits *limits,
                        ms_net **out);
MSMEDIA_API void ms_net_destroy(ms_net *n);

/* 额外信任的 PEM CA 文件（测试临时 CA）。不是关闭校验。 */
MSMEDIA_API ms_status ms_net_set_ca_file(ms_net *n, const char *path);

/* 日志回调；msg 不得含完整 URL、查询或用户信息。 */
MSMEDIA_API void ms_net_set_log(ms_net *n, ms_net_log_fn fn, void *ctx);

/*
 * 可选 worker：在独立线程泵 DNS/连接/握手/读写。
 * 启动后 request/read/cancel 不在调用线程上做无限阻塞读。
 * 未启动时，read/drive 可能做有界 poll（短切片，仍受超时与 cancel 约束）。
 */
MSMEDIA_API ms_status ms_net_worker_start(ms_net *n);
MSMEDIA_API ms_status ms_net_worker_stop(ms_net *n);
MSMEDIA_API ms_status ms_net_drive(ms_net *n);

/* 启动一次 GET。byte_len=0 表示直到 EOF；否则发 Range。 */
MSMEDIA_API ms_status ms_net_get_start(ms_net *n, const char *url,
                           uint64_t byte_off, uint64_t byte_len);
MSMEDIA_API ms_status ms_net_read(ms_net *n, uint8_t *buf, size_t len, size_t *out_n);
MSMEDIA_API void ms_net_cancel(ms_net *n);

MSMEDIA_API ms_net_err ms_net_last_err(const ms_net *n);
MSMEDIA_API int ms_net_last_http_status(const ms_net *n);

/* 填 HLS fetch 外挂；ctx 为该 ms_net*。同时只支持一个在途请求。 */
MSMEDIA_API void ms_net_hls_fetch_bind(ms_net *n, ms_hls_fetch *out);

#ifdef __cplusplus
}
#endif

#endif /* MSMEDIA_MS_NET_H */
