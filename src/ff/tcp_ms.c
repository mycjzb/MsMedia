/*
 * FFmpeg "tcp" URL 协议覆盖：拨号走公共可控网络层 ms_nc_dial（自定义 DNS 钩子 / 静态映射 /
 * Java 解析器 / SOCKS5），让播放器（http/https/rtmp/rtsp 控制连接）与 MsHttpClient 共用同一套网络核心。
 * 只做客户端：listen 模式诚实返回 ENOSYS。在 libmsplayer.so 最终链接时编译，
 * 先于 libavformat.a 出现，静态库里的 tcp.o 不会被拉入（其只定义 ff_tcp_protocol 一个外部符号）。
 * by: Long <mycjzb@gmail.com>
 */
#include "libavutil/avstring.h"
#include "libavutil/parseutils.h"
#include "libavformat/avformat.h"
#include <string.h>
#include "libavutil/opt.h"
#include "libavutil/time.h"
#include "network.h"
#include "url.h"
#include "../../src/netcore/ms_netcore.h"

#include <sys/socket.h>
#include <unistd.h>

typedef struct MsTcpContext {
    const AVClass *class;
    int fd;
    int listen;
    int64_t rw_timeout;      /* 微秒，-1 不限 */
    int64_t listen_timeout;
    int tcp_nodelay;
    volatile int cancel;
} MsTcpContext;

#define OFFSET(x) offsetof(MsTcpContext, x)
#define D AV_OPT_FLAG_DECODING_PARAM
#define E AV_OPT_FLAG_ENCODING_PARAM
static const AVOption ms_tcp_options[] = {
    { "listen", "Listen for incoming connections (unsupported)", OFFSET(listen), AV_OPT_TYPE_INT, { .i64 = 0 }, 0, 2, .flags = D|E },
    { "timeout", "socket I/O timeout (microseconds)", OFFSET(rw_timeout), AV_OPT_TYPE_INT64, { .i64 = -1 }, -1, INT64_MAX, .flags = D|E },
    { "listen_timeout", "unused", OFFSET(listen_timeout), AV_OPT_TYPE_INT64, { .i64 = -1 }, -1, INT64_MAX, .flags = D|E },
    { "tcp_nodelay", "always on", OFFSET(tcp_nodelay), AV_OPT_TYPE_BOOL, { .i64 = 1 }, 0, 1, .flags = D|E },
    { NULL }
};

static const AVClass ms_tcp_class = {
    .class_name = "tcp",
    .item_name  = av_default_item_name,
    .option     = ms_tcp_options,
    .version    = LIBAVUTIL_VERSION_INT,
};

/* 拨号是阻塞的：用一个短轮询线程去看 FFmpeg 中断回调太重；改为在拨号前后检查，
 * 且拨号自身有超时。中断回调置位后，读写在 <=100ms 内返回 AVERROR_EXIT。 */
static int map_err(int r)
{
    switch (r) {
    case MS_NC_E_TIMEOUT: return AVERROR(ETIMEDOUT);
    case MS_NC_E_CANCELED: return AVERROR_EXIT;
    case MS_NC_E_DNS: return AVERROR(EIO);
    case MS_NC_E_PROXY: return AVERROR(ECONNREFUSED);
    case MS_NC_E_CONNECT: return AVERROR(ECONNREFUSED);
    default: return AVERROR(EIO);
    }
}

static int ms_tcp_open(URLContext *h, const char *uri, int flags)
{
    MsTcpContext *s = h->priv_data;
    char host[256], path[1024], proto[16];
    int port = -1, r, timeout_ms;
    (void)flags;
    s->fd = -1;
    if (s->listen) {
        av_log(h, AV_LOG_ERROR, "ms tcp: listen mode unsupported\n");
        return AVERROR(ENOSYS);
    }
    av_url_split(proto, sizeof(proto), NULL, 0, host, sizeof(host), &port, path, sizeof(path), uri);
    if (strcmp(proto, "tcp") || port <= 0 || port >= 65536 || host[0] == 0) {
        return AVERROR(EINVAL);
    }
    {
        /* 与官方 tcp 一致：URL 查询串里的 timeout=（微秒）也生效 */
        const char *q = strchr(uri, '?');
        char buf[64];
        if (q && av_find_info_tag(buf, sizeof(buf), "timeout", q)) {
            s->rw_timeout = strtoll(buf, NULL, 10);
        }
        if (q && av_find_info_tag(buf, sizeof(buf), "listen", q)) {
            if (strtol(buf, NULL, 10) != 0) return AVERROR(ENOSYS);
        }
    }
    if (s->rw_timeout >= 0) {
        h->rw_timeout = s->rw_timeout;
    }
    timeout_ms = h->rw_timeout > 0 ? (int)(h->rw_timeout / 1000) : 15000;
    if (timeout_ms <= 0) timeout_ms = 1;
    if (ff_check_interrupt(&h->interrupt_callback)) return AVERROR_EXIT;
    r = ms_nc_dial(host, port, timeout_ms, &s->cancel, &s->fd);
    if (r != MS_NC_OK) {
        av_log(h, AV_LOG_ERROR, "ms tcp: dial failed: %s\n", ms_nc_strerror(r));
        return map_err(r);
    }
    h->is_streamed = 1;
    return 0;
}

/* 等待就绪：分片调用 ms_nc_wait_fd，同时检查 FFmpeg 中断回调与 rw_timeout。 */
static int ms_tcp_wait(URLContext *h, int fd, int write)
{
    int64_t deadline = h->rw_timeout > 0 ? av_gettime_relative() + h->rw_timeout : 0;
    for (;;) {
        int r;
        if (ff_check_interrupt(&h->interrupt_callback)) return AVERROR_EXIT;
        r = ms_nc_wait_fd(fd, write, 100, NULL);
        if (r > 0) return 0;
        if (r < 0) return AVERROR(EIO);
        if (deadline && av_gettime_relative() > deadline) return AVERROR(ETIMEDOUT);
    }
}

static int ms_tcp_read(URLContext *h, uint8_t *buf, int size)
{
    MsTcpContext *s = h->priv_data;
    for (;;) {
        ssize_t r = recv(s->fd, buf, (size_t)size, 0);
        if (r >= 0) return r == 0 ? AVERROR_EOF : (int)r;
        if (errno == EINTR) continue;
        if (errno != EAGAIN && errno != EWOULDBLOCK) return AVERROR(errno);
        if (h->flags & AVIO_FLAG_NONBLOCK) return AVERROR(EAGAIN);
        r = ms_tcp_wait(h, s->fd, 0);
        if (r < 0) return (int)r;
    }
}

static int ms_tcp_write(URLContext *h, const uint8_t *buf, int size)
{
    MsTcpContext *s = h->priv_data;
    for (;;) {
        ssize_t r = send(s->fd, buf, (size_t)size, MSG_NOSIGNAL);
        if (r >= 0) return (int)r;
        if (errno == EINTR) continue;
        if (errno != EAGAIN && errno != EWOULDBLOCK) return AVERROR(errno);
        if (h->flags & AVIO_FLAG_NONBLOCK) return AVERROR(EAGAIN);
        r = ms_tcp_wait(h, s->fd, 1);
        if (r < 0) return (int)r;
    }
}

static int ms_tcp_shutdown(URLContext *h, int flags)
{
    MsTcpContext *s = h->priv_data;
    int how;
    if ((flags & AVIO_FLAG_WRITE) && (flags & AVIO_FLAG_READ)) how = SHUT_RDWR;
    else if (flags & AVIO_FLAG_WRITE) how = SHUT_WR;
    else how = SHUT_RD;
    return shutdown(s->fd, how);
}

static int ms_tcp_close(URLContext *h)
{
    MsTcpContext *s = h->priv_data;
    if (s->fd >= 0) {
        close(s->fd);
        s->fd = -1;
    }
    return 0;
}

static int ms_tcp_get_file_handle(URLContext *h)
{
    MsTcpContext *s = h->priv_data;
    return s->fd;
}

const URLProtocol ff_tcp_protocol = {
    .name                = "tcp",
    .url_open            = ms_tcp_open,
    .url_read            = ms_tcp_read,
    .url_write           = ms_tcp_write,
    .url_close           = ms_tcp_close,
    .url_get_file_handle = ms_tcp_get_file_handle,
    .url_shutdown        = ms_tcp_shutdown,
    .priv_data_size      = sizeof(MsTcpContext),
    .flags               = URL_PROTOCOL_FLAG_NETWORK,
    .priv_data_class     = &ms_tcp_class,
};
