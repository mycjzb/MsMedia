/*
 * 公共可控网络层（单 so 内只此一份）：自研 TCP 拨号 + 自定义 DNS 钩子 + SOCKS5 代理 +
 * HTTP/1.1 客户端；HTTPS 只在已连好的 socket 上叠加安卓系统 TLS（JNI SSLEngine），
 * 证书链与主机名校验不可关闭，0 字节 OpenSSL。
 * 复用方：FFmpeg tcp/tls 协议覆盖（播放器）、com.mycjzb.media.player.MsHttpClient（App 业务）。
 * 不记录完整 URL（可能含私有 token）。
 * by: Long <mycjzb@gmail.com>
 */
#ifndef MS_NETCORE_H
#define MS_NETCORE_H

#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 错误码（负值）。 */
#define MS_NC_OK          0
#define MS_NC_E_ARG      -1
#define MS_NC_E_NOMEM    -2
#define MS_NC_E_DNS      -3
#define MS_NC_E_CONNECT  -4
#define MS_NC_E_TIMEOUT  -5
#define MS_NC_E_CANCELED -6
#define MS_NC_E_IO       -7
#define MS_NC_E_TLS      -8
#define MS_NC_E_PROXY    -9
#define MS_NC_E_PROTO    -10
#define MS_NC_E_REDIRECT -11
#define MS_NC_E_UNSUPPORTED -12

const char *ms_nc_strerror(int e);

/* ---- 自定义 DNS ----
 * 原生钩子：返回写入 out 的地址个数（0=交给下一级），负值=解析失败。
 * 解析顺序：原生钩子 -> 静态映射表 -> Java 解析器（MsHttpClient.Dns）-> 系统 getaddrinfo。 */
typedef int (*ms_nc_dns_fn)(void *ctx, const char *host, int port,
                            struct sockaddr_storage *out, int cap);
void ms_nc_set_dns_hook(ms_nc_dns_fn fn, void *ctx);

/* 全局配置（进程内播放器与 MsHttpClient 共用）。dns_map 形如 "a.com=1.2.3.4,5.6.7.8;b.com=::1"。
 * socks_host 为空表示不走代理；socks_remote_dns=1 把域名交给代理解析（socks5h）。 */
typedef struct ms_nc_config {
    char dns_map[1024];
    int use_java_dns;
    char socks_host[256];
    int socks_port;
    char socks_user[128];
    char socks_pass[128];
    int socks_remote_dns;
} ms_nc_config;

void ms_nc_config_default(ms_nc_config *c);
int  ms_nc_configure(const ms_nc_config *c);
void ms_nc_get_config(ms_nc_config *out);

/* ---- 连接 ----
 * cancel 可为 NULL；非 NULL 时置 1 即中止（拨号/读/写在 <=100ms 内返回 CANCELED）。 */
typedef struct ms_nc_conn ms_nc_conn;

int  ms_nc_dial(const char *host, int port, int timeout_ms,
                volatile int *cancel, int *out_fd);
int  ms_nc_conn_open(const char *host, int port, int use_tls, int timeout_ms,
                     int io_timeout_ms, volatile int *cancel, ms_nc_conn **out);
int  ms_nc_conn_read(ms_nc_conn *c, uint8_t *buf, int cap);   /* >0 字节，0=EOF，<0 错误 */
int  ms_nc_conn_write_all(ms_nc_conn *c, const uint8_t *buf, int n);
void ms_nc_conn_shutdown(ms_nc_conn *c);                      /* 线程安全，用于取消 */
void ms_nc_conn_close(ms_nc_conn *c);

/* 等待 fd 可读/可写；返回 1 就绪，0 超时，负值错误/取消。timeout_ms<0 为不限。 */
int  ms_nc_wait_fd(int fd, int want_write, int timeout_ms, volatile int *cancel);

/* ---- TLS 泵（唯一实现）：在任意字节通道上驱动系统 TLS 引擎 ----
 * 播放器 FFmpeg tls 协议与 ms_nc_conn 都用它。io 回调返回 >0 字节，0=EOF，<0 错误。 */
typedef struct ms_nc_io {
    void *ctx;
    int (*rd)(void *ctx, uint8_t *buf, int cap);
    int (*wr)(void *ctx, const uint8_t *buf, int n);
} ms_nc_io;

/* 严格校验（证书链 + 主机名，公共 API / MsHttpClient 专用，不可关闭）。 */
int  ms_nc_tls_open(const char *hostname, void **out_eng);
/* verify=0 只给播放器拉流使用：跳过证书链与主机名校验（OpenSSL 后端）；系统 TLS 后端忽略该参数仍严格。 */
int  ms_nc_tls_open2(const char *hostname, int verify, void **out_eng);
/* 严格模式额外信任的 CA（PEM 文件 / 目录，可为 NULL）；不是关闭校验。系统 TLS 后端返回 UNSUPPORTED。 */
int  ms_nc_tls_set_extra_ca(const char *ca_file, const char *ca_dir);
/* 释放进程级 TLS 缓存（测试收尾 / LSan 用）。 */
void ms_nc_tls_global_cleanup(void);
const char *ms_nc_tls_backend(void);
void ms_nc_tls_close(void *eng);
int  ms_nc_tls_handshake(void *eng, const ms_nc_io *io);
int  ms_nc_tls_read(void *eng, const ms_nc_io *io, uint8_t *buf, int cap);
int  ms_nc_tls_write(void *eng, const ms_nc_io *io, const uint8_t *buf, int n);

/* ---- HTTP/1.1 客户端 ---- */
typedef struct ms_nc_http ms_nc_http;

typedef struct ms_nc_req {
    const char *method;       /* GET/POST/PUT/DELETE/HEAD/PATCH；NULL=GET */
    const char *url;          /* http:// 或 https:// */
    const char *headers;      /* "K: V\n" 多行；禁止 CR，非法行拒绝 */
    const uint8_t *body;
    size_t body_len;
    int connect_timeout_ms;   /* <=0 默认 15000 */
    int read_timeout_ms;      /* <=0 默认 15000 */
    int max_redirects;        /* <0 默认 5；0=不跟随 */
    volatile int *cancel;     /* 可选：外部取消标志（打开前/打开中/读取中均可置 1） */
    int keep_alive;           /* 追加：1=HTTP/1.1 Keep-Alive，响应完整读完后连接回池复用（播放器分片/清单用） */
} ms_nc_req;

/* Keep-Alive 连接池统计（测试/诊断）：opened=新建 TCP(+TLS) 次数，reused=复用次数。 */
void ms_nc_pool_stats(int *opened, int *reused);
/* 关闭并清空连接池（切台/退出时可调用；进程退出前调用可让 LSan 零残留）。 */
void ms_nc_pool_clear(void);

int  ms_nc_http_open(const ms_nc_req *req, ms_nc_http **out);
int  ms_nc_http_status(const ms_nc_http *h);
const char *ms_nc_http_headers(const ms_nc_http *h); /* 原始响应头（不含状态行），\r\n 分隔 */
int64_t ms_nc_http_content_length(const ms_nc_http *h); /* -1 未知 */
int  ms_nc_http_read(ms_nc_http *h, uint8_t *buf, int cap); /* >0 字节，0=结束，<0 错误 */
void ms_nc_http_cancel(ms_nc_http *h);
void ms_nc_http_close(ms_nc_http *h);

#ifdef __ANDROID__
#include <jni.h>
/* 在 Java 线程上调用一次（MediaPlayer / MsHttpClient 静态初始化），缓存应用类。
 * 原生工作线程 FindClass 只能看到系统类，必须先缓存。 */
int ms_nc_jni_init(JNIEnv *env);
jclass ms_nc_jni_tls_class(void);
#endif

#ifdef __cplusplus
}
#endif
#endif
