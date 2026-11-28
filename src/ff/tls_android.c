/*
 * FFmpeg "tls" URL 协议后端（播放器拉流专用：https / tls / rtmps）。
 * 握手/读写统一走公共网络层的 TLS 泵 ms_nc_tls_*（与 MsHttpClient 同一份实现）；
 * 底层字节通道是 FFmpeg 的 tcp 子连接（已被 tcp_ms.c 覆盖为自研拨号）。
 * 证书策略：OpenSSL 后端按 FFmpeg tls_verify；ms_ff 默认 0=忽略证书（播放），API 另走严格。
 * 不支持 listen / DTLS。by: Long <mycjzb@gmail.com>
 * 在 libmsplayer.so 最终链接时编译（slimff FFmpeg 构建系统不编译本文件）。
 * by: Long <mycjzb@gmail.com>
 */
#include "network.h"
#include "url.h"
#include "tls.h"
#include "libavutil/opt.h"
#include "libavutil/avstring.h"
#include "../../src/netcore/ms_netcore.h"

typedef struct TLSContext {
    const AVClass *class;
    TLSShared tls_shared;
    void *eng;
    URLContext *parent;
} TLSContext;

static int io_rd(void *ctx, uint8_t *buf, int cap)
{
    TLSContext *c = ctx;
    for (;;) {
        int r = ffurl_read(c->tls_shared.tcp, buf, cap);
        if (r == AVERROR(EAGAIN)) continue;
        if (r == AVERROR_EOF) return 0;
        return r < 0 ? MS_NC_E_IO : r;
    }
}

static int io_wr(void *ctx, const uint8_t *buf, int n)
{
    TLSContext *c = ctx;
    for (;;) {
        int r = ffurl_write(c->tls_shared.tcp, buf, n);
        if (r == AVERROR(EAGAIN)) continue;
        return r < 0 ? MS_NC_E_IO : r;
    }
}

static int tls_close(URLContext *h)
{
    TLSContext *c = h->priv_data;
    if (c->eng) {
        ms_nc_tls_close(c->eng);
        c->eng = NULL;
    }
    if (c->tls_shared.tcp) {
        ffurl_closep(&c->tls_shared.tcp);
    }
    return 0;
}

static int tls_open(URLContext *h, const char *uri, int flags, AVDictionary **options)
{
    TLSContext *c = h->priv_data;
    TLSShared *s = &c->tls_shared;
    const char *host;
    ms_nc_io io = { c, io_rd, io_wr };
    int ret;

    (void)flags;
    if (s->listen || s->is_dtls) {
        return AVERROR(ENOSYS);
    }
    if ((ret = ff_tls_open_underlying(s, h, uri, options)) < 0) {
        return ret;
    }
    host = (s->host && s->host[0]) ? s->host : s->underlying_host;
    if (host == NULL || host[0] == 0) {
        tls_close(h);
        return AVERROR(EINVAL);
    }
    if (ms_nc_tls_open2(host, s->verify, &c->eng) != MS_NC_OK) {
        tls_close(h);
        return AVERROR(EIO);
    }
    {
        int hr = ms_nc_tls_handshake(c->eng, &io);
        if (hr != MS_NC_OK) {
            av_log(h, AV_LOG_ERROR, "tls: handshake/verification failed (verify=%d)\n", s->verify);
            tls_close(h);
            /* 证书链 / 主机名 / 协议协商失败属于确定性错误：返回 EPROTO（不在 ms_ff 的瞬时错误列表里），
             * 上层不做退避重试、立即报错；只有底层收发失败（连接被重置等）才按 EIO 走重试。by: Long <mycjzb@gmail.com> */
            return hr == MS_NC_E_TLS ? AVERROR(EPROTO) : AVERROR(EIO);
        }
    }
    return 0;
}

static int tls_read(URLContext *h, uint8_t *buf, int size)
{
    TLSContext *c = h->priv_data;
    ms_nc_io io = { c, io_rd, io_wr };
    int r = ms_nc_tls_read(c->eng, &io, buf, size);
    if (r == 0) return AVERROR_EOF;
    return r < 0 ? AVERROR(EIO) : r;
}

static int tls_write(URLContext *h, const uint8_t *buf, int size)
{
    TLSContext *c = h->priv_data;
    ms_nc_io io = { c, io_rd, io_wr };
    int r = ms_nc_tls_write(c->eng, &io, buf, size);
    return r < 0 ? AVERROR(EIO) : r;
}

static int tls_get_file_handle(URLContext *h)
{
    TLSContext *c = h->priv_data;
    return ffurl_get_file_handle(c->tls_shared.tcp);
}

static int tls_get_short_seek(URLContext *h)
{
    TLSContext *c = h->priv_data;
    return ffurl_get_short_seek(c->tls_shared.tcp);
}

static const AVOption options[] = {
    TLS_COMMON_OPTIONS(TLSContext, tls_shared),
    { NULL }
};

static const AVClass tls_class = {
    .class_name = "tls",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
};

const URLProtocol ff_tls_protocol = {
    .name                = "tls",
    .url_open2           = tls_open,
    .url_read            = tls_read,
    .url_write           = tls_write,
    .url_close           = tls_close,
    .url_get_file_handle = tls_get_file_handle,
    .url_get_short_seek  = tls_get_short_seek,
    .priv_data_size      = sizeof(TLSContext),
    .flags               = URL_PROTOCOL_FLAG_NETWORK,
    .priv_data_class     = &tls_class,
};
