/*
 * Media3 DataSource 用的 FFmpeg AVIO 薄封装。
 * by: Long <mycjzb@gmail.com>
 */
#include "msmedia/ms_ff_io.h"

#include <libavformat/avio.h>
#include <libavformat/avformat.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>
#include <libavutil/time.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#if defined(__ANDROID__)
#include <android/log.h>
#define FFIO_TAG "FfIo"
#define FFIO_LOG(...) __android_log_print(ANDROID_LOG_WARN, FFIO_TAG, __VA_ARGS__)
#else
#define FFIO_LOG(...) ((void)0)
#endif

struct ms_ff_io {
    AVIOContext *pb;
    AVIOInterruptCB icb;
    volatile int abort;
    int64_t deadline_us;
    int timeout_ms;
    char err[96];
};

static int io_interrupt(void *opaque)
{
    ms_ff_io *io = (ms_ff_io *)opaque;
    if (io == NULL) {
        return 1;
    }
    if (io->abort) {
        return 1;
    }
    if (io->deadline_us > 0 && av_gettime_relative() > io->deadline_us) {
        return 1;
    }
    return 0;
}

/* 单播长连接：deadline 是「这次阻塞 I/O 最多等多久」，每次读写都要重新起算。
 * 不能当成整个会话的寿命，否则 rtmp/rtsp 播满 timeout 就被自己打断（EIO -5），
 * 客户端当网络错误重开，表现为每 timeout 秒断一次。 */
static void arm_deadline(ms_ff_io *io)
{
    if (io != NULL && io->timeout_ms > 0) {
        io->deadline_us = av_gettime_relative() + (int64_t)io->timeout_ms * 1000;
    }
}

static int url_has_scheme(const char *url, const char *scheme)
{
    size_t n;
    if (url == NULL || scheme == NULL) {
        return 0;
    }
    n = strlen(scheme);
    return strncasecmp(url, scheme, n) == 0;
}

static int is_rtmp_url(const char *url)
{
    return url_has_scheme(url, "rtmp://") || url_has_scheme(url, "rtmps://") ||
           url_has_scheme(url, "rtmpt://") || url_has_scheme(url, "rtmpe://");
}

static int is_rtsp_url(const char *url)
{
    return url_has_scheme(url, "rtsp://") || url_has_scheme(url, "rtsps://");
}

static void set_err(ms_ff_io *io, int averr)
{
    if (io == NULL) {
        return;
    }
    if (averr >= 0) {
        io->err[0] = 0;
        return;
    }
    av_strerror(averr, io->err, sizeof(io->err));
}

/* 大小写不敏感子串（不用 strcasestr：非标准扩展，Android 各 API 不一致）。 */
static int str_contains_ci(const char *hay, const char *needle)
{
    size_t n;
    if (hay == NULL || needle == NULL) {
        return 0;
    }
    n = strlen(needle);
    if (n == 0) {
        return 1;
    }
    for (; *hay != 0; hay++) {
        if (strncasecmp(hay, needle, n) == 0) {
            return 1;
        }
    }
    return 0;
}

/* 从 "Name: value\r\n..." 里取一个头的值（names 以 NULL 结尾，大小写不敏感）。 */
static char *header_dup(const char *headers, const char *const *names)
{
    const char *p = headers;
    int i;
    if (headers == NULL) {
        return NULL;
    }
    while (p != NULL && *p != 0) {
        const char *eol = strstr(p, "\r\n");
        size_t len, nl;
        if (eol == NULL) {
            eol = strchr(p, '\n');
        }
        len = eol != NULL ? (size_t)(eol - p) : strlen(p);
        for (i = 0; names[i] != NULL; i++) {
            const char *v;
            nl = strlen(names[i]);
            if (len <= nl || p[nl] != ':' || strncasecmp(p, names[i], nl) != 0) {
                continue;
            }
            v = p + nl + 1;
            while (*v == ' ' || *v == '\t') {
                v++;
            }
            if ((size_t)(v - p) >= len) {
                return NULL;
            }
            return av_strndup(v, len - (size_t)(v - p));
        }
        if (eol == NULL) {
            break;
        }
        p = eol + (*eol == '\r' ? 2 : 1);
    }
    return NULL;
}

/* rtmp 的 Connect AMF：FFmpeg 不认通用 headers 串（会 Option headers not found），
 * 只认 rtmp_pageurl / rtmp_swfurl / rtmp_flashver。 */
static void apply_rtmp_headers(AVDictionary **opt, const char *headers)
{
    static const char *const referer[] = {"Referer", "referer", NULL};
    static const char *const swf[] = {"swfUrl", "swfurl", "SWF", NULL};
    static const char *const flash[] = {"flashVer", "flashver", "FlashVer", NULL};
    static const char *const ua[] = {"User-Agent", "user-agent", NULL};
    char *v = header_dup(headers, referer);
    if (v != NULL && v[0] != 0) {
        av_dict_set(opt, "rtmp_pageurl", v, AV_DICT_DONT_STRDUP_VAL);
    } else {
        av_free(v);
    }
    v = header_dup(headers, swf);
    if (v != NULL && v[0] != 0) {
        av_dict_set(opt, "rtmp_swfurl", v, AV_DICT_DONT_STRDUP_VAL);
    } else {
        av_free(v);
    }
    v = header_dup(headers, flash);
    if (v == NULL) {
        char *agent = header_dup(headers, ua);
        /* 旧版页面会把 Flash 形态的 UA 当 flashVer 传 */
        if (agent != NULL && str_contains_ci(agent, "flash")) {
            v = agent;
        } else {
            av_free(agent);
        }
    }
    if (v != NULL && v[0] != 0) {
        av_dict_set(opt, "rtmp_flashver", v, AV_DICT_DONT_STRDUP_VAL);
    } else {
        av_free(v);
    }
}

/* 单次打开。rtsp_transport 只对 rtsp 源设置（NULL=不设，交给 FFmpeg）。 */
static int open_once(ms_ff_io *io, const char *url, const char *rtsp_transport,
                     const char *headers)
{
    AVDictionary *opt = NULL;
    char tbuf[32];
    int ret;

    snprintf(tbuf, sizeof(tbuf), "%d", io->timeout_ms * 1000);
    av_dict_set(&opt, "rw_timeout", tbuf, 0);
    /* rtmp 的 timeout 选项单位是 listen_timeout：>0 会置 rt->listen=1，
     * tcp 带 ?listen 去 bind 服务器地址 → EADDRNOTAVAIL(-99)。rtmp 一律不传。
     * listen_timeout 对任何协议都是 imply listen，也不传。
     * 其余协议（udp/rtp/rtsp）的 timeout 单位是微秒。 */
    if (!is_rtmp_url(url)) {
        av_dict_set(&opt, "timeout", tbuf, 0);
    }
    if (rtsp_transport != NULL) {
        av_dict_set(&opt, "rtsp_transport", rtsp_transport, 0);
    }
    if (headers != NULL && headers[0] != 0) {
        if (is_rtmp_url(url)) {
            apply_rtmp_headers(&opt, headers);
        } else if (is_rtsp_url(url)) {
            static const char *const ua[] = {"User-Agent", "user-agent", NULL};
            char *agent = header_dup(headers, ua);
            /* rtsp 只有 user_agent，没有 headers 选项 */
            if (agent != NULL && agent[0] != 0) {
                av_dict_set(&opt, "user_agent", agent, AV_DICT_DONT_STRDUP_VAL);
            } else {
                av_free(agent);
            }
        }
        /* udp/rtp：两个选项都不存在，一律不设 */
    }
    arm_deadline(io);
    ret = avio_open2(&io->pb, url, AVIO_FLAG_READ, &io->icb, &opt);
    av_dict_free(&opt);
    return ret;
}

ms_status ms_ff_io_open_ex(ms_ff_io **out, const char *url, int timeout_ms,
                            const char *headers)
{
    ms_ff_io *io;
    int ret;
    if (out == NULL || url == NULL || url[0] == 0) {
        return MS_ERR_INVALID_ARG;
    }
    *out = NULL;
    io = (ms_ff_io *)calloc(1, sizeof(*io));
    if (io == NULL) {
        return MS_ERR_NOMEM;
    }
    if (timeout_ms <= 0) {
        timeout_ms = 15000;
    }
    io->timeout_ms = timeout_ms;
    io->icb.callback = io_interrupt;
    io->icb.opaque = io;
    /* RTSP 先 TCP（交织在控制通道，穿 NAT/防火墙稳），失败再退 UDP。 */
    ret = open_once(io, url, is_rtsp_url(url) ? "tcp" : NULL, headers);
    if (ret < 0 && is_rtsp_url(url)) {
        avio_closep(&io->pb);
        io->deadline_us = 0;
        ret = open_once(io, url, "udp", headers);
    }
    if (ret < 0) {
        set_err(io, ret);
        FFIO_LOG("open fail ret=%d (%s) timeout=%d url=%s", ret, io->err, timeout_ms, url);
        free(io);
        return MS_ERR_UNKNOWN;
    }
    *out = io;
    return MS_OK;
}

ms_status ms_ff_io_open(ms_ff_io **out, const char *url, int timeout_ms)
{
    return ms_ff_io_open_ex(out, url, timeout_ms, NULL);
}

void ms_ff_io_close(ms_ff_io *io)
{
    if (io == NULL) {
        return;
    }
    io->abort = 1;
    if (io->pb != NULL) {
        avio_closep(&io->pb);
    }
    free(io);
}

int ms_ff_io_read(ms_ff_io *io, uint8_t *buf, int size)
{
    int n;
    if (io == NULL || io->pb == NULL || buf == NULL || size <= 0) {
        return -1;
    }
    arm_deadline(io);
    n = avio_read(io->pb, buf, size);
    if (n == AVERROR_EOF) {
        return 0;
    }
    if (n < 0) {
        set_err(io, n);
        return n;
    }
    return n;
}

int64_t ms_ff_io_seek(ms_ff_io *io, int64_t offset, int whence)
{
    int64_t r;
    if (io == NULL || io->pb == NULL) {
        return -1;
    }
    if (whence == AVSEEK_SIZE) {
        arm_deadline(io);
        return avio_size(io->pb);
    }
    arm_deadline(io);
    r = avio_seek(io->pb, offset, whence);
    if (r < 0) {
        set_err(io, (int)r);
    }
    return r;
}

int64_t ms_ff_io_size(ms_ff_io *io)
{
    if (io == NULL || io->pb == NULL) {
        return -1;
    }
    return avio_size(io->pb);
}

const char *ms_ff_io_last_err(const ms_ff_io *io)
{
    if (io == NULL || io->err[0] == 0) {
        return "";
    }
    return io->err;
}
