/*
 * 可选 FFmpeg 9.1 取流/解封装后端实现。
 * AVPacket 按轨/容器映射为公共事件；不开启 decoder/encoder。
 * by: Long <mycjzb@gmail.com>
 */
#define _DEFAULT_SOURCE
#include "msmedia/ms_ff.h"
#ifdef MS_SINGLE_SO
#include "../core/ms_probe.h"
#endif
#ifdef MS_HLS_PREFETCH
/* 阶段 6：Media3 风格 HLS 独立预下载（仅路线 B 单 so / 宿主专项测试编入）。by: Long <mycjzb@gmail.com> */
#include "ms_hls_pf.h"
#endif
#include <strings.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libavutil/avutil.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/log.h>
#include <libavutil/opt.h>
#include <libavutil/time.h>
#include <libavutil/intreadwrite.h>
#include <libavutil/mastering_display_metadata.h>
#include <libavutil/dovi_meta.h>
#ifndef MSMEDIA_NO_OPENSSL
#include <openssl/crypto.h>
#endif

#include <inttypes.h>
#include <limits.h>
#include <errno.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#define MS_FF_DEF_QUEUE (2u * 1024u * 1024u)
#define MS_FF_DEF_SLOTS 8u
#define MS_FF_DEF_TIMEOUT_MS 15000
#define MS_FF_DEF_REDIRECTS 5
#define MS_FF_MAX_SLOTS 32u
#define MS_FF_MAX_QUEUE (16u * 1024u * 1024u)
/* 关键帧常略晚于整秒目标（如 10.023s）。AVSEEK_FLAG_BACKWARD
 * 对恰好 10s 的请求会落到前一个 GOP。向前放宽 100ms 再 BACKWARD。 */
#ifndef MS_FF_SEEK_KEY_SLOP_90K
#define MS_FF_SEEK_KEY_SLOP_90K 9000
#endif

/* FFmpeg RTP 失包只打日志、不置 AV_PKT_FLAG_CORRUPT（9.1 rtpdec.c）。
 * 同线程 av_log 回调捕获官方 "RTP: missed"，拒绝当前可能残缺的 AU，再等到下一关键帧。 */
static _Thread_local ms_ff *ff_log_owner;
static int ff_av_log_hooked;


typedef enum {
    FF_ST_NEW = 0,
    FF_ST_OPEN,
    FF_ST_FAILED,
    FF_ST_CANCELED
} ff_state;

typedef struct ff_slot {
    int used;
    int handed; /* 已由 pull 交给调用方，等待 release */
    uint32_t id;
    uint8_t *buf;
    size_t buf_len;
    char lang[8];
    ms_event ev;
    uint32_t skip_samples_start;
    uint32_t skip_samples_end;
} ff_slot;

#define MS_FF_MAGIC 0x4d534646u /* "MSFF"：hls.c 补丁回调时确认 AVFormatContext.opaque 确为 ms_ff */

struct ms_ff {
    uint32_t magic;
    ms_ff_buffer_config bufcfg; /* 阶段 6：Media3 风格缓冲水位 */
    int tls_verify;             /* 播放拉流 TLS 校验：单 so OpenSSL 默认 0（忽略），其余构建 1 */
#ifdef MS_HLS_PREFETCH
    struct ms_hls_pf *pf;       /* 按需创建（首次收到 HLS 切片计划时），close_input 时销毁 */
#endif
    const ms_allocator *alloc;
    ms_allocator own_alloc;
    ms_ff_limits limits;
    ms_ff_log_fn log_fn;
    void *log_ctx;
    char ca_file[512];
    char ca_path[512];
    int allow_system_trust; /* 默认 1；0 则禁止探测系统锚 */
    atomic_int canceled;          /* 用户 cancel；可跨线程写，interrupt 只读 */
    atomic_int timed_out;         /* deadline 到期；interrupt 置位 */
    atomic_int_least64_t deadline_us; /* 0 = none；每轮阻塞 I/O 重新武装 */
    ms_status last_status;        /* 最近一次失败（超时/取消以外的 I/O 错误） */
    int64_t origin_90k;           /* 会话级统一原点，所有轨相同 */
    int origin_from_meta;         /* 1=容器/首包已锚定原点；仅允许再下调 */
    int au_emitted;               /* 已发出过 AU：此后不得再下调 origin */
    ff_state state;
    AVFormatContext *ic;
    int stream_info_err;
    int probe_mode;
    int64_t t_open_begin_us;
    int64_t dur_open_input_us;
    int64_t dur_find_stream_info_us;
    int64_t probesize_used;
    int64_t analyzeduration_used_us;
    int probe_incomplete;
    uint32_t cfg_sig[64];
    int cfg_sig_valid[64];
    int track_codec_sent[64]; /* -1=尚未发出 TRACK */
    int https_seen;
    unsigned ts_prefix_strip_ops;
    uint64_t ts_prefix_last_bytes;
    int ts_prefix_last_kind;
    int format_resolved;          /* 1=已缓存，禁止在 pull 路径再做文件头 I/O */
#if defined(MS_SINGLE_SO)
    /* 单 so：打开时顺手缓存主输入文件头，封装细分（fMP4/QT）不再 SEEK_SET 0，
     * 避免 HTTP 点播在首帧前多一次 Range 请求（可能触发重连）。 */
    unsigned char head_cache[4096];
    int head_len;
    int head_tried;
#endif
    ms_format format_cached;
    int hint_mpegts;              /* 文件头已见 0x47 同步，避免被 mpegps 低分误认 */
    int meta_format_sent;
    unsigned meta_track_sent;
    unsigned meta_cfg_sent;
    uint64_t cfg_emitted_bits;    /* 已发出 CODEC_CONFIG 的流位图（含打开后补齐） */
    int skip_to_key;              /* 残帧/失包后等到下一个安全关键帧 */
    int rtp_gap;                  /* FFmpeg RTP 抖动队列报告失包；当前包即使是 key 也先丢 */
    int eof;
    int (*orig_io_close2)(AVFormatContext *s, AVIOContext *pb);
    uint32_t next_id;
    uint64_t session_epoch;
    size_t queued_bytes;
    unsigned nslots;
    ff_slot *slots;
    /* 一个尚未入槽的包（反压） */
    AVPacket *held;
    int held_ready;
    uint64_t selection_epoch;
    unsigned timeline_epoch;
    int user_audio_sel;
    int user_sub_sel;
    int stream_selected[64];
    int defaults_applied;
    uint32_t track_uid[64]; /* 跨流唯一；HLS 的 st->id 是 playlist 下标，会冲突 */
    int pending_audio_si;   /* -1=无待确认切换；确认前仍输出已提交音轨 */
    int committed_audio_si; /* -1=无已提交音轨 */
    int pending_sub_si;     /* -1=无待确认字幕 rendition 切换 */
    int committed_sub_si;   /* -1=无已提交字幕轨（含默认 DEFAULT） */
    int pending_video_si;   /* -1=无待确认视频变体切换；提交前仍输出已提交视频 */
    int committed_video_si; /* -1=无已提交视频轨 */
    int cfg_need_resend[64];
    char lang_store[64][16];
    char name_store[64][48];
    ms_sub_doc *sub_ext;
    ms_sub_doc *sub_inband;
    int ext_on;
    uint32_t pending_skip_start;
    uint32_t pending_skip_end;
};

static void ff_log(ms_ff *f, int level, const char *msg);

/* 日志里去掉 URL，只留占位。 */
static void ff_hide_urls(char *s)
{
    char *p;
    if (s == NULL) {
        return;
    }
    p = s;
    while ((p = strstr(p, "://")) != NULL) {
        char *b = p;
        char *e;
        while (b > s) {
            char c = b[-1];
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) {
                break;
            }
            b--;
        }
        e = p + 3;
        while (*e != 0 && *e != ' ' && *e != '\t' && *e != '\n' && *e != '\r' &&
               *e != ')' && *e != '\'' && *e != '"') {
            e++;
        }
        memmove(b + 5, e, strlen(e) + 1u);
        memcpy(b, ".../ ", 5);
        p = b + 5;
    }
}

static void ff_av_log(void *ptr, int level, const char *fmt, va_list vl)
{
    if (ff_log_owner != NULL && level <= AV_LOG_WARNING && fmt != NULL) {
        char line[256];
        va_list copy;
        va_copy(copy, vl);
        vsnprintf(line, sizeof(line), fmt, copy);
        va_end(copy);
        if (strstr(line, "RTP: missed") != NULL) {
            ff_log_owner->rtp_gap = 1;
            ff_log_owner->skip_to_key = 1;
        }
        ff_hide_urls(line);
        ff_log(ff_log_owner, 0, line);
    }
    av_log_default_callback(ptr, level, fmt, vl);
}

static void ff_hook_av_log(void)
{
    if (!ff_av_log_hooked) {
        av_log_set_callback(ff_av_log);
        ff_av_log_hooked = 1;
    }
}


static ms_status averr_to_ms(int err);
static int64_t choose_time_origin(ms_ff *f);

static void *dflt_alloc(void *ctx, size_t n)
{
    (void)ctx;
    return malloc(n);
}

static void dflt_free(void *ctx, void *p)
{
    (void)ctx;
    free(p);
}

static void *ff_zalloc(ms_ff *f, size_t n)
{
    void *p;
    if (n == 0) {
        n = 1;
    }
    p = f->alloc->alloc_fn(f->alloc->ctx, n);
    if (p != NULL) {
        memset(p, 0, n);
    }
    return p;
}

static void ff_free(ms_ff *f, void *p)
{
    if (p != NULL) {
        f->alloc->free_fn(f->alloc->ctx, p);
    }
}

static void ff_log(ms_ff *f, int level, const char *msg)
{
    if (f != NULL && f->log_fn != NULL && msg != NULL) {
        f->log_fn(f->log_ctx, level, msg);
    }
}

void ms_ff_limits_default(ms_ff_limits *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->struct_size = sizeof(*out);
    out->au_queue_bytes = MS_FF_DEF_QUEUE;
    out->out_slots = MS_FF_DEF_SLOTS;
    out->io_timeout_ms = MS_FF_DEF_TIMEOUT_MS;
    out->max_redirects = MS_FF_DEF_REDIRECTS;
}

const char *ms_ff_ffmpeg_version(void)
{
    return av_version_info();
}

const char *ms_ff_ffmpeg_license(void)
{
    return avformat_license();
}

const char *ms_ff_openssl_version(void)
{
#if defined(MS_NC_TLS_OPENSSL)
    return "openssl-3.5.9-slim (netcore)";
#elif defined(MSMEDIA_NO_OPENSSL)
    return "none";
#else
    return OpenSSL_version(OPENSSL_VERSION);
#endif
}

unsigned long ms_ff_openssl_version_num(void)
{
#ifdef MSMEDIA_NO_OPENSSL
    return 0;
#else
    return OpenSSL_version_num();
#endif
}

int ms_ff_stream_info_err(const ms_ff *f)
{
    return f != NULL ? f->stream_info_err : 1;
}

static int scheme_is(const char *url, const char *scheme)
{
    size_t n;
    if (url == NULL || scheme == NULL) {
        return 0;
    }
    n = strlen(scheme);
    if (strncasecmp(url, scheme, n) != 0) {
        return 0;
    }
    return url[n] == ':' ;
}

static int url_is_https(const char *url) { return scheme_is(url, "https") || scheme_is(url, "tls"); }
static int url_is_rtmp(const char *url)
{
    return scheme_is(url, "rtmp") || scheme_is(url, "rtmps") || scheme_is(url, "rtmpt") ||
           scheme_is(url, "rtmpe");
}
static int url_is_http(const char *url) { return scheme_is(url, "http"); }
static int url_is_live_unseekable_proto(const char *url)
{
    return url_is_rtmp(url) || scheme_is(url, "udp") || scheme_is(url, "rtp") ||
           scheme_is(url, "rtsp") || scheme_is(url, "rtsps") || scheme_is(url, "srtp") ||
           scheme_is(url, "srtp-ktls");
}

static int ff_is_canceled(const ms_ff *f)
{
    return atomic_load(&f->canceled) != 0;
}

static int ff_is_timed_out(const ms_ff *f)
{
    return atomic_load(&f->timed_out) != 0;
}

static int64_t ff_mono_us(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return av_gettime_relative();
    }
    return (int64_t)ts.tv_sec * 1000000LL + (int64_t)(ts.tv_nsec / 1000L);
}

static void arm_deadline(ms_ff *f)
{
    if (f->limits.io_timeout_ms > 0) {
        int64_t us = (int64_t)f->limits.io_timeout_ms * 1000;
        atomic_store(&f->deadline_us, av_gettime_relative() + us);
    } else {
        atomic_store(&f->deadline_us, 0);
    }
}

static void arm_deadline_ms(ms_ff *f, int ms)
{
    if (ms > 0) {
        atomic_store(&f->deadline_us, av_gettime_relative() + (int64_t)ms * 1000);
    } else {
        atomic_store(&f->deadline_us, 0);
    }
}

static void disarm_deadline(ms_ff *f)
{
    atomic_store(&f->deadline_us, 0);
}

static int interrupt_cb(void *opaque)
{
    ms_ff *f = (ms_ff *)opaque;
    int64_t dl, now;
    if (f == NULL) {
        return 1;
    }
    /* 用户 cancel 优先；不在此处写 last_status（避免与 I/O 线程交错）。 */
    if (ff_is_canceled(f)) {
        return 1;
    }
    dl = atomic_load(&f->deadline_us);
    if (dl > 0) {
        now = av_gettime_relative();
        if (now >= dl) {
            atomic_store(&f->timed_out, 1);
            return 1;
        }
    }
    return 0;
}

/* 把 libav I/O 错误映射到公共状态。cancel 优先于 timeout。 */
static ms_status io_status(ms_ff *f, int err)
{
    if (ff_is_canceled(f)) {
        f->state = FF_ST_CANCELED;
        f->last_status = MS_ERR_CANCELED;
        ff_log(f, 1, "io stopped: user cancel");
        return MS_ERR_CANCELED;
    }
    if (ff_is_timed_out(f) || err == AVERROR(ETIMEDOUT)) {
        atomic_store(&f->timed_out, 1);
        f->state = FF_ST_FAILED;
        f->last_status = MS_ERR_TIMEOUT;
        ff_log(f, 1, "io stopped: timeout");
        return MS_ERR_TIMEOUT;
    }
    if (err == AVERROR_EOF) {
        return MS_EOS;
    }
    if (err == AVERROR(EAGAIN)
#ifdef EWOULDBLOCK
        || err == AVERROR(EWOULDBLOCK)
#endif
    ) {
        return MS_AGAIN;
    }
    if (err == AVERROR_EXIT) {
        /* interrupt 只用于 cancel/timeout；两者标志都未置位时不当成 cancel。 */
        f->state = FF_ST_FAILED;
        f->last_status = MS_ERR_UNKNOWN;
        ff_log(f, 0, "io interrupted without cancel or timeout");
        return MS_ERR_UNKNOWN;
    }
    return averr_to_ms(err);
}

static int check_location_downgrade(ms_ff *f, const char *url, const char *loc)
{
    if (loc == NULL || loc[0] == 0) {
        loc = url;
    }
    if (url_is_https(loc) || url_is_https(url)) {
        f->https_seen = 1;
    }
    if (url_is_http(loc) && f->https_seen) {
        ff_log(f, 0, "https to http downgrade rejected");
        return -1;
    }
    return 0;
}

#define MS_FF_PREFIX_MAGIC 0x4D535450u /* "MSTP" */
#define MS_FF_PREFIX_PEEK  ((size_t)1024 * 1024)
#define MS_FF_AVIO_BUF     4096u

typedef struct ms_ff_prefix_io {
    uint32_t magic;
    AVIOContext *inner;
    uint8_t *head;
    size_t head_len;
    size_t head_off;
    size_t skip_base; /* 内层绝对起点（已剥前缀） */
    size_t peek_end;  /* 内层已读到的绝对位置 */
} ms_ff_prefix_io;

static int url_may_strip_ts_image_prefix(const char *url)
{
    if (url == NULL || url[0] == 0) {
        return 0;
    }
    /* 数据报/会话协议不要在此剥前缀，以免打乱 RTP/UDP/RTSP/RTMP。 */
    if (scheme_is(url, "udp") || scheme_is(url, "rtp") || scheme_is(url, "srtp") ||
        scheme_is(url, "rtsp") || scheme_is(url, "rtsps") || scheme_is(url, "rtmp") ||
        scheme_is(url, "rtmps") || scheme_is(url, "rtmpt") || scheme_is(url, "sdp") ||
        scheme_is(url, "srt")) {
        return 0;
    }
    return 1;
}

static int is_png_or_jpeg_magic(const uint8_t *d, size_t n)
{
    static const uint8_t png[8] = { 0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a };
    if (d == NULL) {
        return 0;
    }
    if (n >= 8 && memcmp(d, png, 8) == 0) {
        return 1;
    }
    if (n >= 2 && d[0] == 0xff && d[1] == 0xd8) {
        return 1;
    }
#ifdef MS_SINGLE_SO
    /* 单 so：GIF/BMP/WebP 伪装头同样交给公共 ms_probe 判定（两路线共用）。 */
    if (n >= 4 && memcmp(d, "GIF8", 4) == 0) {
        return 1;
    }
    if (n >= 2 && d[0] == 'B' && d[1] == 'M') {
        return 1;
    }
    if (n >= 8 && memcmp(d, "RIFF", 4) == 0) {
        return 1; /* WEBP 在 8..11，由 ms_probe 强校验 */
    }
#endif
    return 0;
}

static int prefix_io_read(void *opaque, uint8_t *buf, int buf_size)
{
    ms_ff_prefix_io *p = (ms_ff_prefix_io *)opaque;
    int done = 0;
    int r;
    if (p == NULL || buf == NULL || buf_size <= 0) {
        return AVERROR(EINVAL);
    }
    while (done < buf_size && p->head_off < p->head_len) {
        buf[done++] = p->head[p->head_off++];
    }
    if (done < buf_size) {
        r = avio_read(p->inner, buf + done, buf_size - done);
        if (r < 0) {
            return done > 0 ? done : r;
        }
        done += r;
    }
    return done;
}

static int64_t prefix_io_seek(void *opaque, int64_t off, int whence)
{
    ms_ff_prefix_io *p = (ms_ff_prefix_io *)opaque;
    int64_t inner_sz, target, r;
    if (p == NULL) {
        return AVERROR(EINVAL);
    }
    if (whence == AVSEEK_SIZE) {
        inner_sz = avio_size(p->inner);
        if (inner_sz < 0) {
            return inner_sz;
        }
        if ((uint64_t)inner_sz < (uint64_t)p->skip_base) {
            return 0;
        }
        return inner_sz - (int64_t)p->skip_base;
    }
    if (whence == SEEK_CUR) {
        off += (int64_t)p->head_off;
        if (p->head_off >= p->head_len) {
            int64_t cur = avio_tell(p->inner);
            if (cur < 0) {
                return cur;
            }
            off += cur - (int64_t)p->peek_end;
        }
        whence = SEEK_SET;
    } else if (whence == SEEK_END) {
        inner_sz = avio_size(p->inner);
        if (inner_sz < 0) {
            return inner_sz;
        }
        off = inner_sz - (int64_t)p->skip_base + off;
        whence = SEEK_SET;
    }
    if (whence != SEEK_SET || off < 0) {
        return AVERROR(EINVAL);
    }
    target = off;
    if ((uint64_t)target <= (uint64_t)p->head_len) {
        /* 只在预读残留内回退：非 seekable 内层也能满足探测回绕。 */
        p->head_off = (size_t)target;
        if (p->inner != NULL && (p->inner->seekable & AVIO_SEEKABLE_NORMAL)) {
            r = avio_seek(p->inner, (int64_t)p->peek_end, SEEK_SET);
            if (r < 0) {
                return r;
            }
        }
        return target;
    }
    if (p->inner == NULL || !(p->inner->seekable & AVIO_SEEKABLE_NORMAL)) {
        return AVERROR(ESPIPE);
    }
    r = avio_seek(p->inner, (int64_t)p->skip_base + target, SEEK_SET);
    if (r < 0) {
        return r;
    }
    p->head_off = p->head_len;
    return target;
}

static int ms_ff_io_close(AVFormatContext *s, AVIOContext *pb)
{
    ms_ff *f = (s != NULL) ? (ms_ff *)s->opaque : NULL;
#ifdef MS_HLS_PREFETCH
    if (ms_hls_pf_reader_close(pb)) {
        return 0; /* 预下载读端：释放单元引用与 AVIO */
    }
#endif
    if (pb != NULL && pb->opaque != NULL) {
        ms_ff_prefix_io *p = (ms_ff_prefix_io *)pb->opaque;
        if (p->magic == MS_FF_PREFIX_MAGIC) {
            AVIOContext *inner = p->inner;
            AVIOContext *tmp = pb;
            p->magic = 0;
            p->inner = NULL;
            pb->opaque = NULL;
            av_freep(&pb->buffer);
            av_freep(&p->head);
            av_free(p);
            avio_context_free(&tmp);
            if (inner != NULL) {
                if (f != NULL && f->orig_io_close2 != NULL) {
                    return f->orig_io_close2(s, inner);
                }
                return avio_close(inner);
            }
            return 0;
        }
    }
    if (f != NULL && f->orig_io_close2 != NULL) {
        return f->orig_io_close2(s, pb);
    }
    return avio_close(pb);
}

static int wrap_prefix_pb(AVFormatContext *s, AVIOContext **pb, uint8_t *left,
                          size_t left_n, size_t skip_base, size_t peek_end)
{
    ms_ff_prefix_io *p;
    uint8_t *abuf;
    AVIOContext *outer;
    (void)s;
    p = (ms_ff_prefix_io *)av_mallocz(sizeof(*p));
    abuf = (uint8_t *)av_malloc(MS_FF_AVIO_BUF);
    if (p == NULL || abuf == NULL) {
        av_free(p);
        av_free(abuf);
        av_free(left);
        return AVERROR(ENOMEM);
    }
    p->magic = MS_FF_PREFIX_MAGIC;
    p->inner = *pb;
    p->head = left;
    p->head_len = left_n;
    p->head_off = 0;
    p->skip_base = skip_base;
    p->peek_end = peek_end;
    outer = avio_alloc_context(abuf, (int)MS_FF_AVIO_BUF, 0, p, prefix_io_read, NULL,
                               prefix_io_seek);
    if (outer == NULL) {
        av_free(abuf);
        av_free(p);
        av_free(left);
        return AVERROR(ENOMEM);
    }
    outer->seekable = p->inner != NULL ? p->inner->seekable : 0;
    *pb = outer;
    return 0;
}

/* 仅当魔术为 PNG/JPEG 且有界扫描确认 TS 后剥前缀。失败则尽量把已读字节还回去。 */
static int ms_ff_skip_ts_image_prefix(AVFormatContext *s, AVIOContext **pb)
{
    uint8_t *buf;
    size_t filled = 0;
    int eos = 0;
    int seekable;
    ms_ts_scan_opt opt;
    ms_ts_scan_result r;
    ms_status st;
    int force_wrap = 0; /* 非 TS 容器（含绝对偏移）必须用位置重映射包装，不能只 seek 内层 */

    if (pb == NULL || *pb == NULL) {
        return 0;
    }
    seekable = (((*pb)->seekable & AVIO_SEEKABLE_NORMAL) != 0);
    buf = (uint8_t *)av_malloc(MS_FF_PREFIX_PEEK);
    if (buf == NULL) {
        return 0; /* 剥前缀失败不阻断打开 */
    }
    /* 先只读 8 字节识别魔术。非图像立刻停，便于在 AVIO 缓冲内回撤，
     * 避免把 m3u8/MP4/FLV 读成 1MiB 再包装后丢掉 HTTP location。 */
    while (filled < MS_FF_PREFIX_PEEK) {
        size_t want;
        int n;
        if (filled < 8) {
            want = 8 - filled;
        } else if (!is_png_or_jpeg_magic(buf, filled)) {
            break;
        } else {
            want = MS_FF_PREFIX_PEEK - filled;
        }
        n = avio_read(*pb, buf + filled, (int)want);
        if (n <= 0) {
            eos = 1;
            break;
        }
        filled += (size_t)n;
        if (filled >= 8 && is_png_or_jpeg_magic(buf, filled)) {
            memset(&opt, 0, sizeof(opt));
            opt.struct_size = sizeof(opt);
            opt.max_scan = MS_FF_PREFIX_PEEK;
            memset(&r, 0, sizeof(r));
            r.struct_size = sizeof(r);
            st = ms_ts_scan(buf, filled, 0, &opt, &r);
            if (st != MS_AGAIN) {
                break;
            }
        }
    }
    if (filled >= 8 && is_png_or_jpeg_magic(buf, filled) && !eos) {
        memset(&opt, 0, sizeof(opt));
        opt.struct_size = sizeof(opt);
        opt.max_scan = MS_FF_PREFIX_PEEK;
        memset(&r, 0, sizeof(r));
        r.struct_size = sizeof(r);
        st = ms_ts_scan(buf, filled, (filled < MS_FF_PREFIX_PEEK) ? 0 : 1, &opt, &r);
    } else if (filled > 0 && is_png_or_jpeg_magic(buf, filled)) {
        memset(&opt, 0, sizeof(opt));
        opt.struct_size = sizeof(opt);
        opt.max_scan = MS_FF_PREFIX_PEEK;
        memset(&r, 0, sizeof(r));
        r.struct_size = sizeof(r);
        st = ms_ts_scan(buf, filled, 1, &opt, &r);
    } else {
        st = MS_ERR_UNKNOWN;
        memset(&r, 0, sizeof(r));
        r.struct_size = sizeof(r);
    }

#ifdef MS_SINGLE_SO
    /* 阶段 4：图像伪装头后不是 TS（MP4/FLV/MKV/PS 等）时，用公共 ms_probe 定位真正媒体起点，
     * 与路线 A 同一套判定；有界读取（≤MS_FF_PREFIX_PEEK），失败则原样回绕不阻断打开。 */
    if (!(st == MS_OK && (r.kind == MS_TS_PREFIX_PNG || r.kind == MS_TS_PREFIX_JPEG) &&
          r.ts_offset > 0) &&
        filled >= 8 && is_png_or_jpeg_magic(buf, filled)) {
        ms_probe_result pr;
        int pk;
        memset(&pr, 0, sizeof(pr));
        for (;;) {
            int n;
            pk = ms_probe_payload(buf, filled, (eos || filled >= MS_FF_PREFIX_PEEK) ? 1 : 0, &pr);
            if (pk != 0) {
                break;
            }
            n = avio_read(*pb, buf + filled, (int)(MS_FF_PREFIX_PEEK - filled));
            if (n <= 0) {
                eos = 1;
            } else {
                filled += (size_t)n;
            }
        }
        if (pk == 1 && pr.offset > 0 && pr.offset <= filled && pr.disguise != 0) {
            st = MS_OK;
            r.ts_offset = pr.offset;
            r.kind = (buf[0] == 0xff) ? MS_TS_PREFIX_JPEG : MS_TS_PREFIX_PNG;
            force_wrap = (pr.fmt != MS_PF_TS);
        }
    }
#endif
    if (st == MS_OK && (r.kind == MS_TS_PREFIX_PNG || r.kind == MS_TS_PREFIX_JPEG) &&
        r.ts_offset > 0 && r.ts_offset <= filled) {
        {
            ms_ff *ff = (s != NULL) ? (ms_ff *)s->opaque : NULL;
            if (ff != NULL) {
                ff->ts_prefix_strip_ops++;
                ff->ts_prefix_last_bytes = (uint64_t)r.ts_offset;
                ff->ts_prefix_last_kind = (int)r.kind;
            }
        }
        if (seekable && !force_wrap && avio_seek(*pb, (int64_t)r.ts_offset, SEEK_SET) >= 0) {
            av_free(buf);
            return 0;
        } else {
            uint8_t *left = NULL;
            size_t ln = filled - r.ts_offset;
            if (ln > 0) {
                left = (uint8_t *)av_malloc(ln);
                if (left == NULL) {
                    av_free(buf);
                    return 0;
                }
                memcpy(left, buf + r.ts_offset, ln);
            }
            av_free(buf);
            return wrap_prefix_pb(s, pb, left, ln, r.ts_offset, filled);
        }
    }

    /* 不是可剥的图像前缀：把已读数据还回起点。
     * 必须先做 SEEK_CUR 短回绕。若先 SEEK_SET 0，HTTP/HTTPS 会切到按次 Range，
     * 大文件在读过约数 MB 后几乎每包重连（宿主直连点播在 ~15.7s 复现）。 */
    if (avio_seek(*pb, -(int64_t)filled, SEEK_CUR) >= 0) {
        av_free(buf);
        return 0;
    }
    if (avio_seek(*pb, 0, SEEK_SET) >= 0) {
        av_free(buf);
        return 0;
    }
    {
        uint8_t *left = NULL;
        if (filled > 0) {
            left = (uint8_t *)av_malloc(filled);
            if (left == NULL) {
                av_free(buf);
                return 0;
            }
            memcpy(left, buf, filled);
        }
        av_free(buf);
        if (filled == 0) {
            av_free(left);
            return 0;
        }
        return wrap_prefix_pb(s, pb, left, filled, 0, filled);
    }
}

static void apply_http_persistent_opts(AVDictionary **options)
{
    if (options == NULL) {
        return;
    }
    /* 点播 Range/断连后复用连接；不关 TLS 校验。失败则后续读包报错。 */
    av_dict_set(options, "multiple_requests", "1", 0);
    av_dict_set(options, "reconnect", "1", 0);
    av_dict_set(options, "reconnect_streamed", "1", 0);
    av_dict_set(options, "reconnect_on_network_error", "1", 0);
    av_dict_set(options, "reconnect_delay_max", "2", 0);
}

static int buf_looks_like_mpegts(const uint8_t *b, size_t n)
{
    unsigned pi, si;
    const int sizes[2] = {188, 192};
    if (b == NULL) {
        return 0;
    }
    for (si = 0; si < 2; si++) {
        int ps = sizes[si];
        int off = (ps == 192) ? 4 : 0;
        if (n < (size_t)ps * 3) {
            continue;
        }
        for (pi = 0; pi < 3; pi++) {
            if (b[(size_t)pi * (size_t)ps + (size_t)off] != 0x47) {
                break;
            }
        }
        if (pi == 3) {
            return 1;
        }
    }
    return 0;
}

/* 已开 mpegps 后，短 AAC TS 可能被 mpeg 探针以 25 分误认。若头上已是
 * 连续 188/192 同步字节，打开时强制 mpegts。只多读 576 字节并短回绕。 */
static void ms_ff_hint_mpegts_from_pb(ms_ff *f, AVIOContext *pb)
{
    uint8_t buf[192 * 3];
    int n;
    if (f == NULL || pb == NULL) {
        return;
    }
    if ((pb->seekable & AVIO_SEEKABLE_NORMAL) == 0) {
        return;
    }
    n = avio_read(pb, buf, (int)sizeof(buf));
    if (n <= 0) {
        return;
    }
    if (buf_looks_like_mpegts(buf, (size_t)n)) {
        f->hint_mpegts = 1;
    }
    if (avio_seek(pb, -(int64_t)n, SEEK_CUR) < 0) {
        (void)avio_seek(pb, 0, SEEK_SET);
    }
}

static int ms_ff_io_open(AVFormatContext *s, AVIOContext **pb, const char *url,
                         int flags, AVDictionary **options)
{
    ms_ff *f = s != NULL ? (ms_ff *)s->opaque : NULL;
    int ret;
    uint8_t *loc = NULL;
    if (f == NULL || pb == NULL) {
        return AVERROR(EINVAL);
    }
    if (url_is_http(url) && f->https_seen) {
        ff_log(f, 0, "https to http child url rejected");
        return AVERROR(EPERM);
    }
    if (url_is_https(url)) {
        f->https_seen = 1;
    }
    if (options != NULL) {
        av_dict_set(options, "verify", f->tls_verify ? "1" : "0", 0);
        av_dict_set(options, "tls_verify", f->tls_verify ? "1" : "0", 0);
#ifdef MS_HLS_PREFETCH
        /* 阶段 6：HLS 切片请求优先接到预下载队列（命中即零等待；未下完边下边读；AES-128 在读端解密）。 */
        if (f->pf != NULL && (flags & AVIO_FLAG_WRITE) == 0) {
            int hit = ms_hls_pf_open(f->pf, url, *options, pb);
            if (hit != 0) {
                return hit > 0 ? 0 : hit;
            }
        }
#endif
        if (f->ca_file[0] != 0) {
            av_dict_set(options, "ca_file", f->ca_file, 0);
            av_dict_set(options, "cafile", f->ca_file, 0);
        }
        if (url_is_http(url) || url_is_https(url)) {
            apply_http_persistent_opts(options);
        }
    }
    ret = avio_open2(pb, url, flags, &s->interrupt_callback, options);
    if (ret < 0) {
        return ret;
    }
    if (*pb != NULL &&
        av_opt_get(*pb, "location", AV_OPT_SEARCH_CHILDREN, &loc) >= 0 && loc != NULL) {
        if (check_location_downgrade(f, url, (const char *)loc) != 0) {
            av_free(loc);
            avio_closep(pb);
            return AVERROR(EPERM);
        }
        av_free(loc);
    } else if (check_location_downgrade(f, url, url) != 0) {
        avio_closep(pb);
        return AVERROR(EPERM);
    }
    if ((flags & AVIO_FLAG_WRITE) == 0 && url_may_strip_ts_image_prefix(url)) {
        ret = ms_ff_skip_ts_image_prefix(s, pb);
        if (ret < 0) {
            if (*pb != NULL) {
                ms_ff_io_close(s, *pb);
                *pb = NULL;
            }
            return ret;
        }
        if (*pb != NULL) {
            ms_ff_hint_mpegts_from_pb(f, *pb);
            /* init_input 在 io_open 之后若 iformat 已设则不再探测。 */
            if (f->hint_mpegts && s->iformat == NULL) {
                const AVInputFormat *tsfmt = av_find_input_format("mpegts");
                if (tsfmt != NULL) {
                    s->iformat = tsfmt;
                }
            }
        }
    }
#if defined(MS_SINGLE_SO)
    /* 只缓存主输入（首个打开的 URL）。读一次再 SEEK_CUR 回绕：数据仍在 AVIO
     * 缓冲内，不产生新请求。放在图像前缀剥离之后，逻辑起点仍为 0 才缓存。 */
    if ((flags & AVIO_FLAG_WRITE) == 0 && !f->head_tried && *pb != NULL &&
        (url_is_http(url) || url_is_https(url))) {
        /* 只对 HTTP/HTTPS：UDP/RTP 等低码率直播凑满 4 KB 会拖慢起播；本地文件回绕本就便宜。 */
        int n;
        f->head_tried = 1;
        if (avio_tell(*pb) == 0) {
            n = avio_read(*pb, f->head_cache, (int)sizeof(f->head_cache));
            if (n > 0) {
                if (avio_seek(*pb, -(int64_t)n, SEEK_CUR) == 0) {
                    f->head_len = n;
                } else if (avio_seek(*pb, 0, SEEK_SET) != 0) {
                    ms_ff_io_close(s, *pb);
                    *pb = NULL;
                    return AVERROR(EIO);
                }
            }
        }
    }
#endif
    return 0;
}

static ms_codec map_codec(enum AVCodecID id)
{
    switch (id) {
    case AV_CODEC_ID_H264:
        return MS_CODEC_H264;
    case AV_CODEC_ID_AAC:
        return MS_CODEC_AAC;
    case AV_CODEC_ID_H263:
        return MS_CODEC_H263;
    case AV_CODEC_ID_HEVC:
        return MS_CODEC_HEVC;
    case AV_CODEC_ID_MPEG4:
        return MS_CODEC_MPEG4_PART2;
    case AV_CODEC_ID_MPEG2VIDEO:
        return MS_CODEC_MPEG2_VIDEO;
    case AV_CODEC_ID_AV1:
        return MS_CODEC_AV1;
    case AV_CODEC_ID_CAVS:
        return MS_CODEC_AVS_FAMILY; /* 仅标识；广播类 0x48 再升格为 AVS+ */
#if defined(AV_CODEC_ID_AVS2)
    case AV_CODEC_ID_AVS2:
        return MS_CODEC_UNKNOWN; /* 非目标：AVS2，禁止映射成 AVS+ */
#endif
#if defined(AV_CODEC_ID_AVS3)
    case AV_CODEC_ID_AVS3:
        return MS_CODEC_UNKNOWN; /* 非目标：AVS3，禁止映射成 AVS+ */
#endif
    case AV_CODEC_ID_AC3:
        return MS_CODEC_AC3;
    case AV_CODEC_ID_EAC3:
        return MS_CODEC_EAC3;
    case AV_CODEC_ID_MP3:
        return MS_CODEC_MP3;
    case AV_CODEC_ID_MP2:
        return MS_CODEC_MP2;
    case AV_CODEC_ID_OPUS:
        return MS_CODEC_OPUS;
    case AV_CODEC_ID_PCM_ALAW:
        return MS_CODEC_G711_ALAW;
    case AV_CODEC_ID_PCM_MULAW:
        return MS_CODEC_G711_ULAW;
#ifdef MS_SINGLE_FFADEC
    case AV_CODEC_ID_DTS:
        return MS_CODEC_DTS;        /* 仅单 so 路线 B：系统 audio/vnd.dts 或 FFmpeg dca 软解 */
    case AV_CODEC_ID_AAC_LATM:
        return MS_CODEC_AAC_LATM;   /* 仅单 so 路线 B：FFmpeg aac_latm 软解（系统不吃 LOAS） */
#endif
    case AV_CODEC_ID_FLAC:
        return MS_CODEC_FLAC;
    case AV_CODEC_ID_PCM_S16LE:
    case AV_CODEC_ID_PCM_S16BE:
    case AV_CODEC_ID_PCM_U8:
    case AV_CODEC_ID_PCM_S24LE:
    case AV_CODEC_ID_PCM_S24BE:
    case AV_CODEC_ID_PCM_S32LE:
    case AV_CODEC_ID_PCM_S32BE:
    case AV_CODEC_ID_PCM_F32LE:
    case AV_CODEC_ID_PCM_F32BE:
    case AV_CODEC_ID_PCM_F64LE:
    case AV_CODEC_ID_PCM_F64BE:
    case AV_CODEC_ID_PCM_S24DAUD:
    case AV_CODEC_ID_PCM_U16LE:
    case AV_CODEC_ID_PCM_U16BE:
        return MS_CODEC_PCM;
    case AV_CODEC_ID_SUBRIP:
#if defined(AV_CODEC_ID_SRT)
    case AV_CODEC_ID_SRT:
#endif
        return MS_CODEC_SUB_SRT;
    case AV_CODEC_ID_WEBVTT:
        return MS_CODEC_SUB_WEBVTT;
    case AV_CODEC_ID_MOV_TEXT:
        return MS_CODEC_SUB_TX3G;
    case AV_CODEC_ID_TEXT:
        return MS_CODEC_SUB_TEXT;
    case AV_CODEC_ID_ASS:
    case AV_CODEC_ID_SSA:
        return MS_CODEC_SUB_ASS;
    case AV_CODEC_ID_DVB_SUBTITLE:
        return MS_CODEC_SUB_DVB;
    case AV_CODEC_ID_HDMV_PGS_SUBTITLE:
        return MS_CODEC_SUB_PGS;
    case AV_CODEC_ID_DVD_SUBTITLE:
        return MS_CODEC_SUB_VOBSUB;
    case AV_CODEC_ID_XSUB:
    case AV_CODEC_ID_DVB_TELETEXT:
        return MS_CODEC_SUB_BITMAP;
    default:
        return MS_CODEC_UNKNOWN;
    }
}

static int annexb_start(const uint8_t *p, size_t n)
{
    if (p == NULL || n < 3) {
        return 0;
    }
    if (p[0] == 0 && p[1] == 0 && p[2] == 1) {
        return 1;
    }
    if (n >= 4 && p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 1) {
        return 1;
    }
    return 0;
}

static ms_sample_format sample_fmt_for(const AVStream *st, const AVPacket *pkt)
{
    enum AVCodecID id;
    const uint8_t *extra;
    int extra_size;
    const uint8_t *pay = NULL;
    int pay_size = 0;
    if (st == NULL || st->codecpar == NULL) {
        return MS_SAMPLE_FMT_UNKNOWN;
    }
    id = st->codecpar->codec_id;
    extra = st->codecpar->extradata;
    extra_size = st->codecpar->extradata_size;
    if (pkt != NULL) {
        pay = pkt->data;
        pay_size = pkt->size;
    }
    if (id == AV_CODEC_ID_H264) {
        if (extra_size > 0 && extra != NULL && extra[0] == 1) {
            return MS_SAMPLE_FMT_AVCC;
        }
        if (annexb_start(extra, (size_t)extra_size) || annexb_start(pay, (size_t)pay_size)) {
            return MS_SAMPLE_FMT_ANNEXB;
        }
        /* 长度前缀常见于 MP4/FLV，即便 extradata 暂缺也按容器偏向 AVCC */
        if (st->codecpar->codec_tag == MKTAG('a', 'v', 'c', '1') ||
            st->codecpar->codec_tag == MKTAG('A', 'V', 'C', '1')) {
            return MS_SAMPLE_FMT_AVCC;
        }
        return MS_SAMPLE_FMT_ANNEXB;
    }
    if (id == AV_CODEC_ID_HEVC) {
        if (extra_size > 0 && extra != NULL && extra[0] == 1) {
            return MS_SAMPLE_FMT_HVCC;
        }
        if (annexb_start(extra, (size_t)extra_size) || annexb_start(pay, (size_t)pay_size)) {
            return MS_SAMPLE_FMT_ANNEXB;
        }
        if (st->codecpar->codec_tag == MKTAG('h', 'v', 'c', '1') ||
            st->codecpar->codec_tag == MKTAG('H', 'V', 'C', '1') ||
            st->codecpar->codec_tag == MKTAG('h', 'e', 'v', '1') ||
            st->codecpar->codec_tag == MKTAG('H', 'E', 'V', '1')) {
            return MS_SAMPLE_FMT_HVCC;
        }
        return MS_SAMPLE_FMT_ANNEXB;
    }
    if (id == AV_CODEC_ID_MPEG2VIDEO || id == AV_CODEC_ID_MPEG1VIDEO) {
        return MS_SAMPLE_FMT_MPV;
    }
    if (id == AV_CODEC_ID_MPEG4) {
        return MS_SAMPLE_FMT_MP4V;
    }
    if (id == AV_CODEC_ID_H263 || id == AV_CODEC_ID_H263P) {
        return MS_SAMPLE_FMT_H263;
    }
    if (id == AV_CODEC_ID_AV1) {
        return MS_SAMPLE_FMT_AV1;
    }
    if (id == AV_CODEC_ID_CAVS) {
        return MS_SAMPLE_FMT_CAVS;
    }
    if (id == AV_CODEC_ID_AAC) {
        if (pay_size >= 2 && pay[0] == 0xff && (pay[1] & 0xf0) == 0xf0) {
            return MS_SAMPLE_FMT_ADTS;
        }
        if (extra_size > 0) {
            return MS_SAMPLE_FMT_AAC_RAW;
        }
        return MS_SAMPLE_FMT_ADTS;
    }
    if (id == AV_CODEC_ID_AC3) {
        return MS_SAMPLE_FMT_AC3;
    }
    if (id == AV_CODEC_ID_MP2 || id == AV_CODEC_ID_MP3) {
        return MS_SAMPLE_FMT_MPEG_AUDIO;
    }
    if (id == AV_CODEC_ID_OPUS) {
        return MS_SAMPLE_FMT_OPUS;
    }
    if (id == AV_CODEC_ID_FLAC) {
        return MS_SAMPLE_FMT_FLAC;
    }
    if (id == AV_CODEC_ID_PCM_S16LE || id == AV_CODEC_ID_PCM_S16BE ||
        id == AV_CODEC_ID_PCM_U8 || id == AV_CODEC_ID_PCM_S24LE ||
        id == AV_CODEC_ID_PCM_S24BE || id == AV_CODEC_ID_PCM_S32LE ||
        id == AV_CODEC_ID_PCM_S32BE || id == AV_CODEC_ID_PCM_F32LE ||
        id == AV_CODEC_ID_PCM_F32BE || id == AV_CODEC_ID_PCM_F64LE ||
        id == AV_CODEC_ID_PCM_F64BE || id == AV_CODEC_ID_PCM_U16LE ||
        id == AV_CODEC_ID_PCM_U16BE) {
        return MS_SAMPLE_FMT_PCM;
    }
    return MS_SAMPLE_FMT_UNKNOWN;
}

#if defined(MS_SINGLE_SO)
/* 读取已缓存的文件头；没有缓存返回 -1，调用方退回旧路径。 */
static int ff_head_cached(AVFormatContext *ic, const unsigned char **out)
{
    ms_ff *owner = (ic != NULL) ? (ms_ff *)ic->opaque : NULL;
    if (owner == NULL || owner->head_len <= 0) {
        return -1;
    }
    *out = owner->head_cache;
    return owner->head_len;
}
#endif

static int is_fragmented_mp4(AVFormatContext *ic)
{
    int64_t pos;
    unsigned char buf[4096];
    int n, i;
#if defined(MS_SINGLE_SO)
    {
        const unsigned char *h = NULL;
        int hl = ff_head_cached(ic, &h);
        if (hl >= 0) {
            if (hl < 16) {
                return 0;
            }
            for (i = 0; i + 8 <= hl; i++) {
                if (h[i] == 'm' && h[i + 1] == 'o' && h[i + 2] == 'o' && h[i + 3] == 'f') {
                    return 1;
                }
            }
            return 0;
        }
    }
#endif
    if (ic == NULL || ic->pb == NULL || ic->pb->seekable == 0) {
        return 0;
    }
    pos = avio_tell(ic->pb);
    if (pos < 0) {
        return 0;
    }
    if (avio_seek(ic->pb, 0, SEEK_SET) < 0) {
        return 0;
    }
    n = avio_read(ic->pb, buf, (int)sizeof(buf));
    avio_seek(ic->pb, pos, SEEK_SET);
    if (n < 16) {
        return 0;
    }
    for (i = 0; i + 8 <= n; i++) {
        if (buf[i] == 'm' && buf[i + 1] == 'o' && buf[i + 2] == 'o' && buf[i + 3] == 'f') {
            return 1;
        }
    }
    return 0;
}

static int ftyp_is_qt(AVFormatContext *ic)
{
    int64_t pos;
    unsigned char buf[32];
    int n;
#if defined(MS_SINGLE_SO)
    {
        const unsigned char *h = NULL;
        int hl = ff_head_cached(ic, &h);
        if (hl >= 0) {
            return (hl >= 12 && memcmp(h + 4, "ftyp", 4) == 0 && memcmp(h + 8, "qt  ", 4) == 0) ? 1 : 0;
        }
    }
#endif
    if (ic == NULL || ic->pb == NULL || ic->pb->seekable == 0) {
        return 0;
    }
    pos = avio_tell(ic->pb);
    if (pos < 0 || avio_seek(ic->pb, 0, SEEK_SET) < 0) {
        return 0;
    }
    n = avio_read(ic->pb, buf, (int)sizeof(buf));
    avio_seek(ic->pb, pos, SEEK_SET);
    if (n >= 12 && memcmp(buf + 4, "ftyp", 4) == 0 && memcmp(buf + 8, "qt  ", 4) == 0) {
        return 1;
    }
    return 0;
}

static ms_format map_format(AVFormatContext *ic)
{
    const char *n;
    ms_ff *owner = NULL;
    ms_format fmt = MS_FORMAT_UNKNOWN;
    if (ic == NULL || ic->iformat == NULL || ic->iformat->name == NULL) {
        return MS_FORMAT_UNKNOWN;
    }
    /* is_fragmented_mp4 / ftyp_is_qt 会 SEEK_SET 0 再 seek 回当前偏移。
     * 每个 AU 都调用一次会把 HTTP 点播打成按包 Range 重连，约 15.7s / 3MB
     * 后几乎停住（宿主直连已复现）。打开后封装类型不再变，只探测一次。 */
    owner = (ms_ff *)ic->opaque;
    if (owner != NULL && owner->format_resolved) {
        return owner->format_cached;
    }
    n = ic->iformat->name;
    if (strstr(n, "mpegts") != NULL) {
        fmt = MS_FORMAT_MPEG_TS;
    } else if (strcmp(n, "hls") == 0) {
        fmt = MS_FORMAT_HLS;
    } else if (strcmp(n, "dash") == 0) {
        fmt = MS_FORMAT_DASH;
    } else if (strcmp(n, "rtsp") == 0) {
        fmt = MS_FORMAT_RTSP;
    } else if (strcmp(n, "sdp") == 0 || strcmp(n, "rtp") == 0) {
        fmt = MS_FORMAT_RTP;
    } else if (strcmp(n, "flv") == 0 || strcmp(n, "live_flv") == 0) {
        /* RTMP 协议上的媒体仍是 FLV 标签；用独立枚举标明拉流会话，不伪造成 TS。 */
        fmt = url_is_rtmp(ic->url) ? MS_FORMAT_RTMP : MS_FORMAT_FLV;
    } else if (strstr(n, "mov") != NULL || strstr(n, "mp4") != NULL) {
        if (is_fragmented_mp4(ic)) {
            fmt = MS_FORMAT_FMP4;
        } else if (ftyp_is_qt(ic)) {
            fmt = MS_FORMAT_MOV;
        } else {
            fmt = MS_FORMAT_MP4;
        }
    } else if (strstr(n, "matroska") != NULL || strstr(n, "webm") != NULL) {
        /* 同一 FFInputFormat 名为 "matroska,webm"。仅当 URL 可辨为 .webm 时标 WEBM。 */
        const char *url = ic->url;
        fmt = MS_FORMAT_MATROSKA;
        if (url != NULL) {
            const char *q, *dot;
            size_t nurl;
            char tmp[8];
            unsigned i;
            q = url;
            while (*q && *q != '?' && *q != '#') {
                q++;
            }
            nurl = (size_t)(q - url);
            if (nurl >= 5) {
                dot = url + nurl - 5;
                if (dot[0] == '.' || (nurl >= 6 && (dot[-1] == '.'))) {
                    if (dot[0] != '.') {
                        dot--;
                    }
                    for (i = 0; i < 5 && dot[i]; i++) {
                        char c = dot[i];
                        if (c >= 'A' && c <= 'Z') {
                            c = (char)(c - 'A' + 'a');
                        }
                        tmp[i] = c;
                    }
                    tmp[i] = 0;
                    if (strcmp(tmp, ".webm") == 0) {
                        fmt = MS_FORMAT_WEBM;
                    }
                }
            }
        }
    } else if (strcmp(n, "avi") == 0) {
        fmt = MS_FORMAT_AVI;
    } else if (strcmp(n, "mpeg") == 0 || strstr(n, "mpegps") != NULL) {
        fmt = MS_FORMAT_MPEGPS;
    } else if (strcmp(n, "mp3") == 0) {
        fmt = MS_FORMAT_MP3;
    } else if (strcmp(n, "flac") == 0) {
        fmt = MS_FORMAT_FLAC;
    } else if (strcmp(n, "wav") == 0) {
        fmt = MS_FORMAT_WAV;
    } else if (strcmp(n, "asf") == 0 || strstr(n, "asf") != NULL) {
        fmt = MS_FORMAT_ASF;
    } else if (strcmp(n, "rm") == 0 || strncmp(n, "rm,", 3) == 0) {
        fmt = MS_FORMAT_RM;
    }
    if (owner != NULL) {
        owner->format_cached = fmt;
        owner->format_resolved = 1;
    }
    return fmt;
}

static ms_status averr_to_ms(int err)
{
    if (err >= 0) {
        return MS_OK;
    }
    if (err == AVERROR(EPERM) || err == AVERROR(EACCES)) {
        return MS_ERR_UNSUPPORTED;
    }
    if (err == AVERROR(ENOMEM)) {
        return MS_ERR_NOMEM;
    }
    if (err == AVERROR(ETIMEDOUT)) {
        return MS_ERR_TIMEOUT;
    }
    /* EIO 不是超时；可能是连接重置或协议错误。 */
    if (err == AVERROR(EIO)) {
        return MS_ERR_UNKNOWN;
    }
    if (err == AVERROR_EXIT) {
        return MS_ERR_UNKNOWN;
    }
    if (err == AVERROR_HTTP_BAD_REQUEST || err == AVERROR_HTTP_UNAUTHORIZED ||
        err == AVERROR_HTTP_FORBIDDEN || err == AVERROR_HTTP_NOT_FOUND ||
        err == AVERROR_HTTP_OTHER_4XX || err == AVERROR_HTTP_SERVER_ERROR) {
        return MS_ERR_UNKNOWN;
    }
    if (err == AVERROR_INVALIDDATA) {
        return MS_ERR_MALFORMED;
    }
    if (err == AVERROR_PROTOCOL_NOT_FOUND || err == AVERROR_DEMUXER_NOT_FOUND ||
        err == AVERROR_PATCHWELCOME) {
        return MS_ERR_UNSUPPORTED;
    }
    return MS_ERR_UNKNOWN;
}

static void clear_slot(ms_ff *f, ff_slot *sl)
{
    if (sl->buf != NULL) {
        if (f->queued_bytes >= sl->buf_len) {
            f->queued_bytes -= sl->buf_len;
        } else {
            f->queued_bytes = 0;
        }
        ff_free(f, sl->buf);
        sl->buf = NULL;
    }
    sl->buf_len = 0;
    sl->used = 0;
    sl->handed = 0;
    sl->id = 0;
    sl->lang[0] = 0;
    sl->skip_samples_start = 0;
    sl->skip_samples_end = 0;
    memset(&sl->ev, 0, sizeof(sl->ev));
}

static void release_all_slots(ms_ff *f)
{
    unsigned i;
    if (f->slots == NULL) {
        return;
    }
    for (i = 0; i < f->nslots; i++) {
        if (f->slots[i].used) {
            clear_slot(f, &f->slots[i]);
        }
    }
}

static ff_slot *find_free_slot(ms_ff *f)
{
    unsigned i;
    for (i = 0; i < f->nslots; i++) {
        if (!f->slots[i].used) {
            return &f->slots[i];
        }
    }
    return NULL;
}

static ms_status enqueue_event(ms_ff *f, ms_event ev, const uint8_t *data, size_t len,
                               const char *lang)
{
    ff_slot *sl;
    if (len > 0 && f->queued_bytes + len > f->limits.au_queue_bytes) {
        return MS_LIMIT;
    }
    sl = find_free_slot(f);
    if (sl == NULL) {
        return MS_LIMIT;
    }
    if (len > 0) {
        sl->buf = (uint8_t *)ff_zalloc(f, len);
        if (sl->buf == NULL) {
            return MS_ERR_NOMEM;
        }
        memcpy(sl->buf, data, len);
        sl->buf_len = len;
        f->queued_bytes += len;
        ev.data = sl->buf;
        ev.data_size = len;
    } else {
        ev.data = NULL;
        ev.data_size = 0;
    }
    if (lang != NULL && lang[0] != 0) {
        snprintf(sl->lang, sizeof(sl->lang), "%s", lang);
        ev.track.language = sl->lang;
    } else {
        ev.track.language = NULL;
    }
    ev.track.name = NULL;
    ev.session_epoch = f->session_epoch;
    ev.timeline_epoch = f->timeline_epoch;
    ev.selection_epoch = f->selection_epoch;
    f->next_id++;
    if (f->next_id == 0) {
        f->next_id = 1;
    }
    ev.release_id = f->next_id;
    sl->id = ev.release_id;
    sl->ev = ev;
    sl->skip_samples_start = f->pending_skip_start;
    sl->skip_samples_end = f->pending_skip_end;
    f->pending_skip_start = 0;
    f->pending_skip_end = 0;
    sl->used = 1;
    sl->handed = 0;
    return MS_OK;
}

static void ff_assign_track_ids(ms_ff *f);
static void ff_drop_kind_slots(ms_ff *f, enum AVMediaType typ, uint32_t only_id, int any_sub);
static void ff_apply_discard(ms_ff *f);


static ms_codec cavs_refine_from(const uint8_t *data, size_t n)
{
    ms_video_cfg cfg;
    if (data == NULL || n == 0) {
        return MS_CODEC_AVS_FAMILY;
    }
    ms_video_cfg_default(&cfg);
    if (ms_video_cfg_parse(MS_CODEC_AVS_PLUS, MS_SAMPLE_FMT_CAVS, 1, data, n,
                           &cfg) == MS_OK && cfg.complete) {
        return MS_CODEC_AVS_PLUS;
    }
    return MS_CODEC_AVS_FAMILY;
}

static void fill_track(ms_ff *f, ms_track_info *ti, const AVStream *st)
{
    memset(ti, 0, sizeof(*ti));
    if (st == NULL || st->codecpar == NULL) {
        return;
    }
    if (f != NULL) {
        ff_assign_track_ids(f);
    }
    if (f != NULL && st->index >= 0 && (unsigned)st->index < 64u &&
        f->track_uid[st->index] != 0) {
        ti->id = f->track_uid[st->index];
    } else {
        ti->id = st->id > 0 ? (uint32_t)st->id : (uint32_t)st->index + 1u;
    }
    switch (st->codecpar->codec_type) {
    case AVMEDIA_TYPE_VIDEO:
        ti->kind = MS_TRACK_VIDEO;
        break;
    case AVMEDIA_TYPE_AUDIO:
        ti->kind = MS_TRACK_AUDIO;
        break;
    case AVMEDIA_TYPE_SUBTITLE:
        ti->kind = MS_TRACK_SUBTITLE;
        break;
    default:
        ti->kind = MS_TRACK_UNKNOWN;
        break;
    }
    ti->codec = map_codec(st->codecpar->codec_id);
    if (ti->codec == MS_CODEC_AVS_FAMILY &&
        st->codecpar->extradata != NULL && st->codecpar->extradata_size > 0) {
        ti->codec = cavs_refine_from(st->codecpar->extradata,
                                     (size_t)st->codecpar->extradata_size);
    }
    ti->level = (ti->codec == MS_CODEC_AVS_PLUS) ? MS_RECOG_AU : MS_RECOG_BITSTREAM;
    ti->stream_type = 0;
}

static int ff_sub_playable(ms_codec c)
{
    return c == MS_CODEC_SUB_TEXT || c == MS_CODEC_SUB_SRT ||
           c == MS_CODEC_SUB_WEBVTT || c == MS_CODEC_SUB_TX3G ||
           c == MS_CODEC_SUB_ASS || c == MS_CODEC_SUB_DVB ||
           c == MS_CODEC_SUB_PGS || c == MS_CODEC_SUB_VOBSUB;
}

static void ff_copy_meta(ms_ff *f, unsigned si, const AVStream *st, ms_track_info *ti)
{
    AVDictionaryEntry *e;
    if (si >= 64u || st == NULL || ti == NULL) {
        return;
    }
    f->lang_store[si][0] = 0;
    f->name_store[si][0] = 0;
    if (st->metadata != NULL) {
        e = av_dict_get(st->metadata, "language", NULL, 0);
        if (e != NULL && e->value != NULL) {
            snprintf(f->lang_store[si], sizeof(f->lang_store[si]), "%s", e->value);
        }
        e = av_dict_get(st->metadata, "title", NULL, 0);
        if (e == NULL) {
            /* FFmpeg HLS 把 EXT-X-MEDIA NAME 写到 comment */
            e = av_dict_get(st->metadata, "comment", NULL, 0);
        }
        if (e == NULL) {
            e = av_dict_get(st->metadata, "handler_name", NULL, 0);
        }
        if (e != NULL && e->value != NULL) {
            snprintf(f->name_store[si], sizeof(f->name_store[si]), "%s", e->value);
        }
    }
    if (st->codecpar != NULL && st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
        char extra[40];
        int w = st->codecpar->width;
        int h = st->codecpar->height;
        int64_t br = 0;
        AVDictionaryEntry *vb = av_dict_get(st->metadata, "variant_bitrate", NULL, 0);
        if (vb == NULL && f->ic != NULL) {
            int pi, sj;
            for (pi = 0; pi < (int)f->ic->nb_programs && vb == NULL; pi++) {
                const AVProgram *pr = f->ic->programs[pi];
                if (pr == NULL) {
                    continue;
                }
                for (sj = 0; sj < (int)pr->nb_stream_indexes; sj++) {
                    if ((int)pr->stream_index[sj] == (int)si) {
                        vb = av_dict_get(pr->metadata, "variant_bitrate", NULL, 0);
                        break;
                    }
                }
            }
        }
        if (vb != NULL && vb->value != NULL) {
            br = strtoll(vb->value, NULL, 10);
        }
        extra[0] = 0;
        if (w > 0 && h > 0 && br > 0) {
            snprintf(extra, sizeof(extra), "%dx%d %ukbps", w, h,
                     (unsigned)((br + 500) / 1000));
        } else if (w > 0 && h > 0) {
            snprintf(extra, sizeof(extra), "%dx%d", w, h);
        } else if (br > 0) {
            snprintf(extra, sizeof(extra), "%ukbps",
                     (unsigned)((br + 500) / 1000));
        }
        if (extra[0] != 0) {
            if (f->name_store[si][0] == 0) {
                snprintf(f->name_store[si], sizeof(f->name_store[si]), "%s", extra);
            } else {
                size_t used = strlen(f->name_store[si]);
                if (used + 1 + strlen(extra) < sizeof(f->name_store[si])) {
                    snprintf(f->name_store[si] + used,
                             sizeof(f->name_store[si]) - used, " %s", extra);
                }
            }
        }
    }
    ti->language = f->lang_store[si][0] ? f->lang_store[si] : NULL;
    ti->name = f->name_store[si][0] ? f->name_store[si] : NULL;
    ti->is_default = (st->disposition & AV_DISPOSITION_DEFAULT) ? 1 : 0;
    ti->is_forced = (st->disposition & AV_DISPOSITION_FORCED) ? 1 : 0;
    ti->selected = (si < 64u) ? f->stream_selected[si] : 0;
}

static void ff_apply_default_selection(ms_ff *f)
{
    unsigned i, n;
    int first_v = -1, first_a = -1, def_a = -1, def_s = -1, forced_s = -1;
    if (f == NULL || f->ic == NULL || f->defaults_applied) {
        return;
    }
    n = (unsigned)f->ic->nb_streams;
    if (n > 64u) {
        n = 64u;
    }
    memset(f->stream_selected, 0, sizeof(f->stream_selected));
    for (i = 0; i < n; i++) {
        AVStream *st = f->ic->streams[i];
        if (st == NULL || st->codecpar == NULL) {
            continue;
        }
        if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && first_v < 0) {
            first_v = (int)i;
        }
        if (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            if (first_a < 0) {
                first_a = (int)i;
            }
            if ((st->disposition & AV_DISPOSITION_DEFAULT) && def_a < 0) {
                def_a = (int)i;
            }
        }
        if (st->codecpar->codec_type == AVMEDIA_TYPE_SUBTITLE) {
            ms_codec sc = map_codec(st->codecpar->codec_id);
            if (ff_sub_playable(sc)) {
                if ((st->disposition & AV_DISPOSITION_DEFAULT) && def_s < 0) {
                    def_s = (int)i;
                }
                if ((st->disposition & AV_DISPOSITION_FORCED) && forced_s < 0) {
                    forced_s = (int)i;
                }
            }
        }
    }
    if (first_v >= 0) {
        f->stream_selected[first_v] = 1;
        f->committed_video_si = first_v;
    } else {
        f->committed_video_si = -1;
    }
    if (!f->user_audio_sel) {
        int a = (def_a >= 0) ? def_a : first_a;
        if (a >= 0) {
            f->stream_selected[a] = 1;
            f->committed_audio_si = a;
        } else {
            f->committed_audio_si = -1;
        }
    }
    if (!f->user_sub_sel) {
        /* DEFAULT=YES 的外链字幕自动选中；否则 FORCED。AUTOSELECT 单独不自动打开。 */
        int s = (def_s >= 0) ? def_s : forced_s;
        if (s >= 0) {
            f->stream_selected[s] = 1;
            f->committed_sub_si = s;
            f->user_sub_sel = 1; /* 使 ms_ff_subtitles_at 返回内嵌 cue */
        } else {
            f->committed_sub_si = -1;
        }
    }
    f->defaults_applied = 1;
    f->pending_audio_si = -1;
    f->pending_sub_si = -1;
    f->pending_video_si = -1;
    ff_assign_track_ids(f);
    ff_apply_discard(f);
}

static int ff_stream_selected(const ms_ff *f, const AVStream *st)
{
    unsigned si;
    if (f == NULL || st == NULL || f->ic == NULL) {
        return 0;
    }
    si = (unsigned)st->index;
    if (si >= 64u) {
        return 0;
    }
    return f->stream_selected[si];
}

static void ff_assign_track_ids(ms_ff *f)
{
    unsigned i, n, k;
    if (f == NULL || f->ic == NULL) {
        return;
    }
    n = (unsigned)f->ic->nb_streams;
    if (n > 64u) {
        n = 64u;
    }
    for (i = 0; i < n; i++) {
        uint32_t cand;
        int bump;
        const AVStream *st;
        if (f->track_uid[i] != 0) {
            continue;
        }
        st = f->ic->streams[i];
        cand = 0;
        if (st != NULL && st->id > 0) {
            cand = (uint32_t)st->id;
        }
        if (cand == 0) {
            cand = i + 1u;
        }
        do {
            bump = 0;
            if (cand == 0 || cand == MS_TRACK_ID_EXTERNAL_SUB) {
                cand++;
                bump = 1;
                continue;
            }
            for (k = 0; k < n; k++) {
                if (k != i && f->track_uid[k] == cand) {
                    cand++;
                    bump = 1;
                    break;
                }
            }
        } while (bump);
        f->track_uid[i] = cand;
    }
}

static uint32_t ff_track_id(const ms_ff *f, const AVStream *st)
{
    if (st == NULL) {
        return 0;
    }
    if (f != NULL && st->index >= 0 && (unsigned)st->index < 64u) {
        if (f->track_uid[st->index] == 0) {
            ff_assign_track_ids((ms_ff *)f);
        }
        if (f->track_uid[st->index] != 0) {
            return f->track_uid[st->index];
        }
    }
    return st->id > 0 ? (uint32_t)st->id : (uint32_t)st->index + 1u;
}

static int ff_find_stream(const ms_ff *f, uint32_t id)
{
    unsigned i, n;
    if (f == NULL || f->ic == NULL) {
        return -1;
    }
    ff_assign_track_ids((ms_ff *)f);
    n = (unsigned)f->ic->nb_streams;
    for (i = 0; i < n; i++) {
        if (ff_track_id(f, f->ic->streams[i]) == id) {
            return (int)i;
        }
    }
    return -1;
}

static void ff_apply_discard(ms_ff *f)
{
    int i, n;
    if (f == NULL || f->ic == NULL) {
        return;
    }
    n = (int)f->ic->nb_streams;
    for (i = 0; i < n && i < 64; i++) {
        AVStream *st = f->ic->streams[i];
        int keep = 0;
        enum AVMediaType typ;
        if (st == NULL || st->codecpar == NULL) {
            continue;
        }
        typ = st->codecpar->codec_type;
        if (typ == AVMEDIA_TYPE_VIDEO) {
            keep = f->stream_selected[i] || (f->pending_video_si == i) ||
                   !f->defaults_applied;
        } else if (typ == AVMEDIA_TYPE_AUDIO) {
            keep = f->stream_selected[i] || (f->pending_audio_si == i);
        } else if (typ == AVMEDIA_TYPE_SUBTITLE) {
            /* 同一容器内的可播字幕始终 demux（MKV 稀疏 SRT/ASS）。
             * HLS/DASH 外链 rendition 仍只读选中轨，避免未选中清单被拉起来。 */
            keep = f->stream_selected[i] || (f->pending_sub_si == i);
            if (!keep && ff_sub_playable(map_codec(st->codecpar->codec_id))) {
                ms_format fmt = map_format(f->ic);
                if (fmt != MS_FORMAT_HLS && fmt != MS_FORMAT_DASH &&
                    fmt != MS_FORMAT_RTSP && fmt != MS_FORMAT_RTP &&
                    fmt != MS_FORMAT_RTMP) {
                    keep = 1;
                }
            }
        } else {
            keep = f->stream_selected[i];
        }
        st->discard = keep ? AVDISCARD_DEFAULT : AVDISCARD_ALL;
    }
}

static void ff_commit_audio(ms_ff *f, int idx, int on)
{
    int i, n;
    if (f == NULL || f->ic == NULL) {
        return;
    }
    n = (int)f->ic->nb_streams;
    for (i = 0; i < n && i < 64; i++) {
        int want;
        if (f->ic->streams[i] == NULL || f->ic->streams[i]->codecpar == NULL ||
            f->ic->streams[i]->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) {
            continue;
        }
        want = on && i == idx;
        if (f->stream_selected[i] != want) {
            ff_drop_kind_slots(f, AVMEDIA_TYPE_AUDIO,
                               ff_track_id(f, f->ic->streams[i]), 0);
            f->stream_selected[i] = want ? 1 : 0;
        }
    }
    f->pending_audio_si = -1;
    f->committed_audio_si = (on && idx >= 0) ? idx : -1;
    f->selection_epoch++;
    if (f->selection_epoch == 0) {
        f->selection_epoch = 1;
    }
    if (on && idx >= 0 && idx < 64) {
        f->cfg_need_resend[idx] = 1;
        f->cfg_sig_valid[idx] = 0;
    }
    ff_apply_discard(f);
}

static void ff_commit_sub(ms_ff *f, int idx, int on)
{
    int i, n;
    if (f == NULL || f->ic == NULL) {
        return;
    }
    n = (int)f->ic->nb_streams;
    for (i = 0; i < n && i < 64; i++) {
        int want;
        if (f->ic->streams[i] == NULL || f->ic->streams[i]->codecpar == NULL ||
            f->ic->streams[i]->codecpar->codec_type != AVMEDIA_TYPE_SUBTITLE) {
            continue;
        }
        want = on && i == idx;
        if (f->stream_selected[i] != want) {
            ff_drop_kind_slots(f, AVMEDIA_TYPE_SUBTITLE,
                               ff_track_id(f, f->ic->streams[i]), 1);
            f->stream_selected[i] = want ? 1 : 0;
        }
    }
    f->stream_selected[63] = 0; /* 外挂与内嵌互斥 */
    f->pending_sub_si = -1;
    f->committed_sub_si = (on && idx >= 0) ? idx : -1;
    f->user_sub_sel = (on && idx >= 0) ? 1 : f->user_sub_sel;
    if (!on) {
        f->user_sub_sel = f->ext_on && f->stream_selected[63] ? 1 : 0;
    }
    f->selection_epoch++;
    if (f->selection_epoch == 0) {
        f->selection_epoch = 1;
    }
    /* 切轨保留 inband 缓存，按选中轨过滤显示；稀疏文字包不会再来。 */
    ff_apply_discard(f);
}

/* 旧视频 playlist 内复用的音轨会随变体 discard 一起关掉：回退到新变体内复用音轨。
 * 独立 AUDIO rendition（与旧视频不同 playlist id）保持用户选择。 */
static void ff_maybe_fallback_muxed_audio(ms_ff *f, int old_v, int new_v)
{
    int old_a, new_a = -1, i, n, old_pl = -1, new_pl = -1;
    enum AVCodecID want_id = AV_CODEC_ID_NONE;
    if (f == NULL || f->ic == NULL || new_v < 0 || new_v >= (int)f->ic->nb_streams) {
        return;
    }
    old_a = f->committed_audio_si;
    if (old_a < 0 && f->pending_audio_si < 0) {
        return;
    }
    if (old_v >= 0 && old_v < (int)f->ic->nb_streams &&
        f->ic->streams[old_v] != NULL) {
        old_pl = f->ic->streams[old_v]->id;
    }
    if (f->ic->streams[new_v] != NULL) {
        new_pl = f->ic->streams[new_v]->id;
    }
    if (old_a >= 0 && old_a < (int)f->ic->nb_streams &&
        f->ic->streams[old_a] != NULL) {
        /* 独立备选音轨：playlist id 与旧视频不同，保持用户选择。 */
        if (old_pl < 0 || f->ic->streams[old_a]->id != old_pl) {
            return;
        }
        if (f->ic->streams[old_a]->codecpar != NULL) {
            want_id = f->ic->streams[old_a]->codecpar->codec_id;
        }
    }
    n = (int)f->ic->nb_streams;
    for (i = 0; i < n && i < 64; i++) {
        AVStream *st = f->ic->streams[i];
        if (st == NULL || st->codecpar == NULL ||
            st->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) {
            continue;
        }
        if (new_pl >= 0 && st->id != new_pl) {
            continue;
        }
        if (want_id != AV_CODEC_ID_NONE && st->codecpar->codec_id != want_id) {
            if (new_a < 0) {
                new_a = i; /* 先记下同 playlist 的音轨，优先同 codec 再覆盖 */
            }
            continue;
        }
        new_a = i;
        if (want_id == AV_CODEC_ID_NONE || st->codecpar->codec_id == want_id) {
            break;
        }
    }
    if (new_a >= 0 && new_a != old_a) {
        /* 武装待确认：读到新变体音包后再提交并重发 CSD，不把未读到的轨写成已选。 */
        f->pending_audio_si = new_a;
        ff_log(f, 1, "video variant: muxed audio fallback armed");
    } else if (new_a < 0 && old_a >= 0 && old_pl >= 0 &&
               f->ic->streams[old_a] != NULL &&
               f->ic->streams[old_a]->id == old_pl) {
        ff_log(f, 0, "video variant: no muxed audio on new variant; clearing audio");
        ff_commit_audio(f, -1, 0);
    }
}

static void ff_commit_video(ms_ff *f, int idx, int on)
{
    int i, n, old_v;
    if (f == NULL || f->ic == NULL) {
        return;
    }
    old_v = f->committed_video_si;
    n = (int)f->ic->nb_streams;
    for (i = 0; i < n && i < 64; i++) {
        int want;
        if (f->ic->streams[i] == NULL || f->ic->streams[i]->codecpar == NULL ||
            f->ic->streams[i]->codecpar->codec_type != AVMEDIA_TYPE_VIDEO) {
            continue;
        }
        want = on && i == idx;
        if (f->stream_selected[i] != want) {
            ff_drop_kind_slots(f, AVMEDIA_TYPE_VIDEO,
                               ff_track_id(f, f->ic->streams[i]), 0);
            f->stream_selected[i] = want ? 1 : 0;
        }
    }
    f->pending_video_si = -1;
    f->committed_video_si = (on && idx >= 0) ? idx : -1;
    f->selection_epoch++;
    if (f->selection_epoch == 0) {
        f->selection_epoch = 1;
    }
    if (on && idx >= 0 && idx < 64) {
        f->cfg_need_resend[idx] = 1;
        f->cfg_sig_valid[idx] = 0;
    }
    if (on && idx >= 0) {
        ff_maybe_fallback_muxed_audio(f, old_v, idx);
    }
    ff_apply_discard(f);
}


static void ff_drop_kind_slots(ms_ff *f, enum AVMediaType typ, uint32_t only_id, int any_sub)
{
    unsigned i;
    for (i = 0; i < f->nslots; i++) {
        ms_event_type ty;
        if (!f->slots[i].used) {
            continue;
        }
        ty = f->slots[i].ev.type;
        if (ty != MS_EVENT_AU && ty != MS_EVENT_CODEC_CONFIG && ty != MS_EVENT_SUBTITLE) {
            continue;
        }
        if (any_sub && ty == MS_EVENT_SUBTITLE) {
            /* release via existing path: mark unused after free buf */
            if (f->slots[i].buf != NULL) {
                f->queued_bytes -= f->slots[i].buf_len;
                ff_free(f, f->slots[i].buf);
            }
            memset(&f->slots[i], 0, sizeof(f->slots[i]));
            continue;
        }
        if (only_id != 0 && f->slots[i].ev.track.id == only_id) {
            if (f->slots[i].buf != NULL) {
                f->queued_bytes -= f->slots[i].buf_len;
                ff_free(f, f->slots[i].buf);
            }
            memset(&f->slots[i], 0, sizeof(f->slots[i]));
        }
    }
    (void)typ;
}



static uint32_t extra_sig(const uint8_t *p, int n)
{
    uint32_t s = (uint32_t)n * 16777619u;
    int i;
    if (p == NULL || n <= 0) {
        return 0;
    }
    for (i = 0; i < n; i++) {
        s = (s * 16777619u) ^ p[i];
    }
    return s;
}

/* open_input 成功后，find_stream_info 被我们的 FAST deadline / interrupt
 * 打断时，FFmpeg 常把 pb->error 写成 AVERROR_EXIT 并置 eof_reached。
 * 这不等于套接字或 demuxer 已毁，不能据此把部分探测失败当成打开失败。
 * 硬 I/O 错误（EIO / ECONNRESET 等）才视为上下文不可再读。 */
static int avio_err_is_probe_interrupt(int err)
{
    return err == AVERROR_EXIT || err == AVERROR(ETIMEDOUT) || err == AVERROR(EINTR);
}

static int input_context_present(const ms_ff *f)
{
    const AVInputFormat *fmt;
    if (f == NULL || f->ic == NULL) {
        return 0;
    }
    if (f->ic->pb != NULL) {
        return 1;
    }
    fmt = f->ic->iformat;
    if (fmt != NULL && (fmt->flags & AVFMT_NOFILE) != 0) {
        return 1;
    }
    return 0;
}

/* 清掉探测 interrupt 留下的软错误，使后续 av_read_frame 可继续。
 * 返回 1 表示上下文可继续读；0 表示硬错误或上下文已不在。 */
static int recover_probe_io(ms_ff *f)
{
    AVIOContext *pb;
    if (!input_context_present(f)) {
        return 0;
    }
    pb = f->ic->pb;
    if (pb == NULL) {
        return 1; /* AVFMT_NOFILE */
    }
    if (pb->error >= 0) {
        return 1;
    }
    if (ff_is_canceled(f)) {
        return 0;
    }
    if (avio_err_is_probe_interrupt(pb->error)) {
        pb->error = 0;
        pb->eof_reached = 0;
        return 1;
    }
    return 0;
}

/* 关闭输入但保留本次 open 的阶段计时，避免失败路径把诊断值冲成 -1。 */
static void close_input_keep_open_stats(ms_ff *f);

static void close_input(ms_ff *f)
{
    if (f->held != NULL) {
        av_packet_free(&f->held);
        f->held = NULL;
    }
    f->held_ready = 0;
    if (f->ic != NULL) {
        avformat_close_input(&f->ic);
        f->ic = NULL;
    }
#ifdef MS_HLS_PREFETCH
    /* 读端已随 avformat_close_input 关闭；再停下载线程（中断在途请求）并释放全部预下载缓冲。 */
    if (f->pf != NULL) {
        ms_hls_pf_destroy(f->pf);
        f->pf = NULL;
    }
#endif
    f->meta_format_sent = 0;
    f->meta_track_sent = 0;
    f->meta_cfg_sent = 0;
    f->probe_incomplete = 0;
    memset(f->cfg_sig, 0, sizeof(f->cfg_sig));
    memset(f->cfg_sig_valid, 0, sizeof(f->cfg_sig_valid));
    {
        unsigned zi;
        for (zi = 0; zi < 64u; zi++) {
            f->track_codec_sent[zi] = -1;
        }
    }
    f->eof = 0;
    f->https_seen = 0;
    f->format_resolved = 0;
    f->format_cached = MS_FORMAT_UNKNOWN;
    f->hint_mpegts = 0;
    f->stream_info_err = 1;
    f->t_open_begin_us = -1;
    f->dur_open_input_us = -1;
    f->dur_find_stream_info_us = -1;
    f->probesize_used = -1;
    f->analyzeduration_used_us = -1;
    f->au_emitted = 0;
    f->origin_90k = 0;
    f->origin_from_meta = 0;
    f->cfg_emitted_bits = 0;
    f->skip_to_key = 0;
    f->rtp_gap = 0;
}

static void close_input_keep_open_stats(ms_ff *f)
{
    int64_t t0 = f->t_open_begin_us;
    int64_t d0 = f->dur_open_input_us;
    int64_t d1 = f->dur_find_stream_info_us;
    int64_t ps = f->probesize_used;
    int64_t ad = f->analyzeduration_used_us;
    int si = f->stream_info_err;
    int pi = f->probe_incomplete;
    close_input(f);
    f->t_open_begin_us = t0;
    f->dur_open_input_us = d0;
    f->dur_find_stream_info_us = d1;
    f->probesize_used = ps;
    f->analyzeduration_used_us = ad;
    f->stream_info_err = si;
    f->probe_incomplete = pi;
}

ms_status ms_ff_create(const ms_allocator *alloc, const ms_ff_limits *limits, ms_ff **out)
{
    ms_ff *f;
    ms_allocator tmp;
    ms_ff_limits lim;
    unsigned nslots;
    if (out == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    *out = NULL;
    if (alloc == NULL || alloc->alloc_fn == NULL || alloc->free_fn == NULL) {
        tmp.alloc_fn = dflt_alloc;
        tmp.free_fn = dflt_free;
        tmp.ctx = NULL;
        alloc = &tmp;
    }
    ms_ff_limits_default(&lim);
    if (limits != NULL) {
        if (limits->struct_size != 0 && limits->struct_size < sizeof(size_t)) {
            return MS_ERR_INVALID_ARG;
        }
        if (limits->au_queue_bytes != 0) {
            if (limits->au_queue_bytes > MS_FF_MAX_QUEUE) {
                return MS_ERR_INVALID_ARG;
            }
            lim.au_queue_bytes = limits->au_queue_bytes;
        }
        if (limits->out_slots != 0) {
            if (limits->out_slots > MS_FF_MAX_SLOTS) {
                return MS_ERR_INVALID_ARG;
            }
            lim.out_slots = limits->out_slots;
        }
        if (limits->io_timeout_ms != 0) {
            lim.io_timeout_ms = limits->io_timeout_ms;
        }
        if (limits->max_redirects >= 0) {
            lim.max_redirects = limits->max_redirects;
        }
        if (limits->struct_size >=
                offsetof(ms_ff_limits, probe_mode) + sizeof(int) &&
            (limits->probe_mode == MS_FF_PROBE_DEFAULT ||
             limits->probe_mode == MS_FF_PROBE_FAST_START)) {
            lim.probe_mode = limits->probe_mode;
        }
    }
    f = (ms_ff *)alloc->alloc_fn(alloc->ctx, sizeof(*f));
    if (f == NULL) {
        return MS_ERR_NOMEM;
    }
    memset(f, 0, sizeof(*f));
    f->pending_audio_si = -1;
    f->committed_audio_si = -1;
    f->pending_sub_si = -1;
    f->committed_sub_si = -1;
    f->pending_video_si = -1;
    f->committed_video_si = -1;
    if (alloc == &tmp) {
        f->own_alloc = tmp;
        f->alloc = &f->own_alloc;
    } else {
        f->alloc = alloc;
    }
    f->limits = lim;
    f->magic = MS_FF_MAGIC;
    ms_ff_buffer_config_default(&f->bufcfg);
#ifdef MS_NC_TLS_OPENSSL
    f->tls_verify = 0; /* 用户定调：播放拉流默认忽略证书；业务 API（MsHttpClient）不受影响 */
#else
    f->tls_verify = 1;
#endif
    f->allow_system_trust = 1;
    f->stream_info_err = 1;
    f->probe_mode = lim.probe_mode;
    f->t_open_begin_us = -1;
    f->dur_open_input_us = -1;
    f->dur_find_stream_info_us = -1;
    f->probe_incomplete = 0;
    memset(f->cfg_sig, 0, sizeof(f->cfg_sig));
    memset(f->cfg_sig_valid, 0, sizeof(f->cfg_sig_valid));
    {
        unsigned zi;
        for (zi = 0; zi < 64u; zi++) {
            f->track_codec_sent[zi] = -1;
        }
    }
    f->probesize_used = -1;
    f->analyzeduration_used_us = -1;
    f->next_id = 1;
    f->session_epoch = 1;
    f->selection_epoch = 1;
    f->timeline_epoch = 1;
    nslots = (unsigned)lim.out_slots;
    f->slots = (ff_slot *)f->alloc->alloc_fn(f->alloc->ctx, nslots * sizeof(ff_slot));
    if (f->slots == NULL) {
        f->alloc->free_fn(f->alloc->ctx, f);
        return MS_ERR_NOMEM;
    }
    memset(f->slots, 0, nslots * sizeof(ff_slot));
    f->nslots = nslots;
    *out = f;
    return MS_OK;
}

void ms_ff_destroy(ms_ff *f)
{
    const ms_allocator *a;
    if (f == NULL) {
        return;
    }
    a = f->alloc;
    close_input(f);
    release_all_slots(f);
    if (f->sub_ext != NULL) {
        ms_sub_doc_destroy(f->sub_ext);
        f->sub_ext = NULL;
    }
    if (f->sub_inband != NULL) {
        ms_sub_doc_destroy(f->sub_inband);
        f->sub_inband = NULL;
    }
    ff_free(f, f->slots);
    a->free_fn(a->ctx, f);
}

ms_status ms_ff_set_ca_file(ms_ff *f, const char *path)
{
    if (f == NULL || path == NULL || path[0] == 0) {
        return MS_ERR_INVALID_ARG;
    }
    if (f->state != FF_ST_NEW) {
        return MS_ERR_STATE;
    }
    if (strlen(path) >= sizeof(f->ca_file)) {
        return MS_ERR_OVERFLOW;
    }
    snprintf(f->ca_file, sizeof(f->ca_file), "%s", path);
    return MS_OK;
}

static int path_is_readable_file(const char *p)
{
    struct stat st;
    if (p == NULL || p[0] == 0) {
        return 0;
    }
    if (stat(p, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0) {
        return 0;
    }
    return access(p, R_OK) == 0;
}

static int path_is_usable_cadir(const char *p)
{
    DIR *d;
    struct dirent *e;
    int n = 0;
    if (p == NULL || p[0] == 0) {
        return 0;
    }
    d = opendir(p);
    if (d == NULL) {
        return 0;
    }
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') {
            continue;
        }
        n++;
        if (n > 0) {
            break;
        }
    }
    closedir(d);
    return n > 0;
}

int ms_ff_url_needs_tls_trust(const char *url)
{
    return scheme_is(url, "https") || scheme_is(url, "tls") ||
           scheme_is(url, "rtsps") || scheme_is(url, "rtmps");
}

static int probe_system_trust(char *out_file, size_t file_cap, char *out_dir, size_t dir_cap)
{
    static const char *files[] = {
#ifndef __ANDROID__
        "/etc/ssl/certs/ca-certificates.crt",
        "/etc/pki/tls/certs/ca-bundle.crt",
        "/etc/ssl/cert.pem",
#endif
        NULL
    };
    static const char *dirs[] = {
#ifdef __ANDROID__
        "/apex/com.android.conscrypt/cacerts",
        "/system/etc/security/cacerts",
#else
        "/etc/ssl/certs",
#endif
        NULL
    };
    size_t i;
    if (out_file && file_cap) {
        out_file[0] = 0;
    }
    if (out_dir && dir_cap) {
        out_dir[0] = 0;
    }
    for (i = 0; files[i] != NULL; i++) {
        if (path_is_readable_file(files[i])) {
            if (out_file && file_cap) {
                snprintf(out_file, file_cap, "%s", files[i]);
            }
            return 1;
        }
    }
    for (i = 0; dirs[i] != NULL; i++) {
        if (path_is_usable_cadir(dirs[i])) {
            if (out_dir && dir_cap) {
                snprintf(out_dir, dir_cap, "%s", dirs[i]);
            }
            return 1;
        }
    }
    return 0;
}

int ms_ff_trust_ready(const ms_ff *f)
{
    if (f == NULL) {
        return 0;
    }
    if (path_is_readable_file(f->ca_file)) {
        return 1;
    }
    if (path_is_usable_cadir(f->ca_path)) {
        return 1;
    }
    if (!f->allow_system_trust) {
        return 0;
    }
#if defined(MSMEDIA_NO_OPENSSL) && defined(__ANDROID__)
    /* 路线 B：系统 TrustManager / 默认 CA，不依赖 OpenSSL 证书包路径。 */
    return 1;
#else
    {
        char sys_file[512], sys_dir[512];
        return probe_system_trust(sys_file, sizeof(sys_file), sys_dir, sizeof(sys_dir));
    }
#endif
}

ms_status ms_ff_set_ca_path(ms_ff *f, const char *dir)
{
    if (f == NULL || dir == NULL || dir[0] == 0) {
        return MS_ERR_INVALID_ARG;
    }
    if (f->state != FF_ST_NEW) {
        return MS_ERR_STATE;
    }
    if (strlen(dir) >= sizeof(f->ca_path)) {
        return MS_ERR_OVERFLOW;
    }
    if (!path_is_usable_cadir(dir)) {
        return MS_ERR_INVALID_ARG;
    }
    snprintf(f->ca_path, sizeof(f->ca_path), "%s", dir);
    return MS_OK;
}

ms_status ms_ff_set_allow_system_trust(ms_ff *f, int allow)
{
    if (f == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    if (f->state != FF_ST_NEW) {
        return MS_ERR_STATE;
    }
    f->allow_system_trust = allow ? 1 : 0;
    return MS_OK;
}

static void apply_tls_trust_env(ms_ff *f)
{
    char sys_file[512], sys_dir[512];
    const char *dir = NULL;
    if (f == NULL) {
        return;
    }
    if (f->ca_path[0] != 0 && path_is_usable_cadir(f->ca_path)) {
        dir = f->ca_path;
    } else if (f->ca_file[0] == 0 && f->allow_system_trust &&
               probe_system_trust(sys_file, sizeof(sys_file), sys_dir, sizeof(sys_dir))) {
        if (sys_file[0] != 0 && f->ca_file[0] == 0) {
            snprintf(f->ca_file, sizeof(f->ca_file), "%s", sys_file);
        }
        if (sys_dir[0] != 0) {
            dir = sys_dir;
        }
    }
    if (dir != NULL) {
#ifndef MSMEDIA_NO_OPENSSL
        /* FFmpeg OpenSSL 后端只有 ca_file 选项；目录锚交给 OpenSSL 默认路径。 */
        (void)setenv("SSL_CERT_DIR", dir, 1);
#else
        (void)dir;
#endif
    }
#ifndef MSMEDIA_NO_OPENSSL
    /* 再设 SSL_CERT_FILE：部分 OpenSSL 构建不从 FFmpeg ca_file 选项接到 CTX。 */
    if (f->ca_file[0] != 0) {
        (void)setenv("SSL_CERT_FILE", f->ca_file, 1);
    }
#endif
}

void ms_ff_set_log(ms_ff *f, ms_ff_log_fn fn, void *ctx)
{
    if (f == NULL) {
        return;
    }
    f->log_fn = fn;
    f->log_ctx = ctx;
}

ms_status ms_ff_set_probe_mode(ms_ff *f, int mode)
{
    if (f == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    if (f->state != FF_ST_NEW) {
        return MS_ERR_STATE;
    }
    if (mode != MS_FF_PROBE_DEFAULT && mode != MS_FF_PROBE_FAST_START) {
        return MS_ERR_INVALID_ARG;
    }
    f->probe_mode = mode;
    f->limits.probe_mode = mode;
    return MS_OK;
}

ms_status ms_ff_get_open_stats(const ms_ff *f, ms_ff_open_stats *out)
{
    size_t sz;
    if (f == NULL || out == NULL || out->struct_size < sizeof(size_t)) {
        return MS_ERR_INVALID_ARG;
    }
    sz = out->struct_size;
    if (sz > sizeof(*out)) {
        sz = sizeof(*out);
    }
    memset(out, 0, sz);
    out->struct_size = sz;
#define FF_COPY_FIELD(field)                                                 \
    do {                                                                     \
        if (sz >= offsetof(ms_ff_open_stats, field) + sizeof(out->field)) {  \
            out->field = f->field;                                           \
        }                                                                    \
    } while (0)
    FF_COPY_FIELD(t_open_begin_us);
    FF_COPY_FIELD(dur_open_input_us);
    FF_COPY_FIELD(dur_find_stream_info_us);
    FF_COPY_FIELD(probe_mode);
    FF_COPY_FIELD(stream_info_err);
    FF_COPY_FIELD(probesize_used);
    FF_COPY_FIELD(analyzeduration_used_us);
    FF_COPY_FIELD(probe_incomplete);
#undef FF_COPY_FIELD
    return MS_OK;
}


static int ff_hls_needs_probe_rewind(const ms_ff *f)
{
    unsigned i;
    int nsub = 0, nvid = 0;
    if (f == NULL || f->ic == NULL || f->ic->iformat == NULL ||
        f->ic->iformat->name == NULL) {
        return 0;
    }
    if (strcmp(f->ic->iformat->name, "hls") != 0) {
        return 0;
    }
    if (f->ic->nb_programs > 1) {
        return 1;
    }
    for (i = 0; i < f->ic->nb_streams; i++) {
        const AVStream *st = f->ic->streams[i];
        if (st == NULL || st->codecpar == NULL) {
            continue;
        }
        if (st->codecpar->codec_type == AVMEDIA_TYPE_SUBTITLE) {
            nsub++;
        }
        if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            nvid++;
        }
    }
    return (nsub > 0 || nvid > 1);
}

static void ff_rewind_hls_subs_after_probe(ms_ff *f)
{
    int si = -1, i, ret;
    if (!ff_hls_needs_probe_rewind(f)) {
        return;
    }
    for (i = 0; i < (int)f->ic->nb_streams; i++) {
        const AVStream *st = f->ic->streams[i];
        if (st != NULL && st->codecpar != NULL &&
            st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            si = i;
            break;
        }
    }
    if (si < 0) {
        si = 0;
    }
    /* find_stream_info 会临时打开外链 WebVTT 并把短 playlist 整段读完，
     * cur_seq_no 被推过末段后播放阶段再也拿不到 cue。回到 0 且带 ANY，
     * 避免视频 start_time 晚于 0 时丢掉片头字幕；直播不可 seek 时不否决打开。 */
    arm_deadline(f);
    ff_hook_av_log();
    ff_log_owner = f;
    ret = av_seek_frame(f->ic, si, 0, AVSEEK_FLAG_ANY | AVSEEK_FLAG_BACKWARD);
    ff_log_owner = NULL;
    disarm_deadline(f);
    if (ret < 0) {
        ff_log(f, 1, "hls subtitle rewind after probe skipped (not seekable)");
        return;
    }
    if (f->held != NULL) {
        av_packet_unref(f->held);
        f->held_ready = 0;
    }
    f->eof = 0;
    ff_apply_discard(f);
}

/* 公共：配置一个新的 AVFormatContext（opaque / interrupt / io 钩子），首次打开与重试共用。 */
static int setup_ic(ms_ff *f)
{
    f->ic = avformat_alloc_context();
    if (f->ic == NULL) {
        return AVERROR(ENOMEM);
    }
    f->ic->opaque = f;
    f->ic->interrupt_callback.callback = interrupt_cb;
    f->ic->interrupt_callback.opaque = f;
    f->ic->io_open = ms_ff_io_open;
    f->orig_io_close2 = f->ic->io_close2;
    f->ic->io_close2 = ms_ff_io_close;
    return 0;
}

/* 瞬时网络错误（对端拒绝/重置/超时/不可达/5xx）：值得按 Media3 风格退避重试；4xx、协议/格式错误不重试。 */
static int is_transient_net_err(int e)
{
    return e == AVERROR(ECONNREFUSED) || e == AVERROR(ECONNRESET) || e == AVERROR(ETIMEDOUT) ||
           e == AVERROR(EPIPE) || e == AVERROR(ENETUNREACH) || e == AVERROR(EHOSTUNREACH) ||
           e == AVERROR(EIO) || e == AVERROR(EAGAIN) || e == AVERROR_HTTP_SERVER_ERROR;
}

/*
 * 阶段 6：主输入（m3u8 / 直链）打开的 Media3 风格重试。
 * 根治 API 34 模拟器 slimff HLS「3 次 1 次 -1004」：日志实证为打开 m3u8 时 TCP connect 被拒
 * （测试服务器 accept 队列被前一个系统播放器的并发连接占满，模拟器 NAT 直接回 RST），
 * FFmpeg http reconnect 在 1 s 内连试 3 次即放弃 → avformat_open_input -111 → MEDIA_ERROR_IO(-1004)。
 * 现在对瞬时错误再退避重试 3 次（300/700/1500 ms，可被 cancel / 总 deadline 立即打断），
 * 网络本身瞬断也能自愈；确定性错误（4xx / 格式不认识）照旧立即失败。by: Long <mycjzb@gmail.com>
 */
static int open_input_with_retry(ms_ff *f, const char *url, AVDictionary **opt)
{
    static const int backoff_ms[] = { 300, 700, 1500 };
    AVDictionary *saved = NULL;
    int ret, attempt = 0;
    int net = !scheme_is(url, "file") && strstr(url, "://") != NULL;
    if (net) {
        av_dict_copy(&saved, *opt, 0);
    }
    for (;;) {
        ret = avformat_open_input(&f->ic, url, NULL, opt);
        if (ret >= 0 || !net || attempt >= (int)(sizeof(backoff_ms) / sizeof(backoff_ms[0])) ||
            !is_transient_net_err(ret) || ff_is_canceled(f) || ff_is_timed_out(f)) {
            break;
        }
        f->ic = NULL; /* avformat_open_input 失败时已释放 context */
        {
            char lbuf[96];
            snprintf(lbuf, sizeof(lbuf), "open transient error %d, media3-style retry %d in %d ms",
                     ret, attempt + 1, backoff_ms[attempt]);
            ff_log(f, 1, lbuf);
        }
        {
            int64_t end = av_gettime_relative() + (int64_t)backoff_ms[attempt] * 1000;
            while (av_gettime_relative() < end && !ff_is_canceled(f)) {
                av_usleep(10000);
            }
        }
        if (ff_is_canceled(f)) {
            break;
        }
        attempt++;
        av_dict_free(opt);
        av_dict_copy(opt, saved, 0);
        arm_deadline(f); /* 每次重试重新武装 io 超时 */
        if (setup_ic(f) < 0) {
            ret = AVERROR(ENOMEM);
            break;
        }
    }
    av_dict_free(&saved);
    return ret;
}

ms_status ms_ff_open(ms_ff *f, const char *url)
{
    AVDictionary *opt = NULL;
    char redirects[16];
    char timeout[32];
    int ret;
    if (f == NULL || url == NULL || url[0] == 0) {
        return MS_ERR_INVALID_ARG;
    }
    if (f->state != FF_ST_NEW) {
        return MS_ERR_STATE;
    }
    if (ff_is_canceled(f)) {
        f->state = FF_ST_CANCELED;
        f->last_status = MS_ERR_CANCELED;
        return MS_ERR_CANCELED;
    }
    if (url_is_https(url)) {
        f->https_seen = 1;
    }
    if (ms_ff_url_needs_tls_trust(url) && f->tls_verify) {
        if (!ms_ff_trust_ready(f)) {
            ff_log(f, 0, "TLS trust anchors missing; verification stays required; not opening");
            f->state = FF_ST_FAILED;
            f->last_status = MS_ERR_UNSUPPORTED;
            return MS_ERR_UNSUPPORTED;
        }
        apply_tls_trust_env(f);
    }
    if (setup_ic(f) < 0) {
        return MS_ERR_NOMEM;
    }
    /* DEFAULT：不改官方 probesize/analyzeduration。
     * FAST_START：有界值（非 0），不关音轨；探测截止用独立短 deadline，
     * 不用 io_timeout/read_timeout 充当起播预算。 */
    if (f->probe_mode == MS_FF_PROBE_FAST_START) {
        char nbuf[32];
        snprintf(nbuf, sizeof(nbuf), "%d", MS_FF_FAST_ANALYZEDURATION_US);
        av_dict_set(&opt, "analyzeduration", nbuf, 0);
        snprintf(nbuf, sizeof(nbuf), "%d", MS_FF_FAST_PROBESIZE);
        av_dict_set(&opt, "probesize", nbuf, 0);
        snprintf(nbuf, sizeof(nbuf), "%d", MS_FF_FAST_MAX_PROBE_PACKETS);
        av_dict_set(&opt, "max_probe_packets", nbuf, 0);
        av_dict_set(&opt, "scan_all_pmts", "1", 0);
        f->probesize_used = MS_FF_FAST_PROBESIZE;
        f->analyzeduration_used_us = MS_FF_FAST_ANALYZEDURATION_US;
    } else {
        f->probesize_used = -1;
        f->analyzeduration_used_us = -1;
    }
#ifdef MS_SINGLE_SO
    /* s6：HTTP(S)/HLS/DASH 归 Media3+JNI；此处只保留非 HTTP 协议与容器。 */
    av_dict_set(&opt, "protocol_whitelist",
                "file,crypto,data,tls,tcp,udp,rtp,rtmp,rtmps,rtsp,pipe,fd",
                0);
    av_dict_set(&opt, "format_whitelist",
                "mpegts,mpegtsraw,flv,live_flv,mov,mp4,m4a,3gp,3g2,mj2,rtsp,rtp,sdp,aac,h264,hevc,matroska,webm,webvtt,avi,mpegps,mpeg,mp3,flac,wav,ac3,eac3,dts,srt,ass",
                0);
#else
    av_dict_set(&opt, "protocol_whitelist",
                "file,crypto,data,http,https,tls,tcp,udp,rtp,srtp,rtmp,rtmps,rtmpt,rtsp,pipe,fd",
                0);
    av_dict_set(&opt, "format_whitelist",
                "mpegts,mpegtsraw,flv,live_flv,mov,mp4,m4a,3gp,3g2,mj2,hls,dash,rtsp,rtp,sdp,aac,h264,hevc,ogg,matroska,webm,webvtt,avi,mpegps,mpeg,mp3,flac,wav,asf,rm,ac3,eac3,srt,ass,microdvd,vobsub",
                0);
#endif
    av_dict_set(&opt, "verify", f->tls_verify ? "1" : "0", 0);
    av_dict_set(&opt, "tls_verify", f->tls_verify ? "1" : "0", 0);
    if (f->ca_file[0] != 0) {
        av_dict_set(&opt, "ca_file", f->ca_file, 0);
        av_dict_set(&opt, "cafile", f->ca_file, 0);
    }
    snprintf(redirects, sizeof(redirects), "%d", f->limits.max_redirects);
    av_dict_set(&opt, "max_redirects", redirects, 0);
    if (f->limits.io_timeout_ms > 0) {
        int64_t us = (int64_t)f->limits.io_timeout_ms * 1000;
        snprintf(timeout, sizeof(timeout), "%" PRId64, us);
        /* RTMP 的 timeout 选项是「监听入站连接的秒数」且会隐含 listen=1，
         * 不能把通用微秒 rw_timeout 写进这个键。客户端只用 rw_timeout。 */
        if (!url_is_rtmp(url)) {
            av_dict_set(&opt, "timeout", timeout, 0);
        }
        av_dict_set(&opt, "rw_timeout", timeout, 0);
    }
    if (url_is_http(url) || url_is_https(url)) {
        apply_http_persistent_opts(&opt);
#if defined(MS_SINGLE_SO)
        /* 起播红线：MPEG-TS 在 find_stream_info 里为算时长会 seek 到文件尾再回到头，
         * 网络点播首帧前多两次 Range 请求（服务端不保活时就是重连）。
         * 单 so 网络输入跳过按 PTS 估时长，退回码率估算；本地文件不受影响。 */
        av_dict_set(&opt, "skip_estimate_duration_from_pts", "1", 0);
#endif
    }
    {
        const char *tr = getenv("MS_FF_RTSP_TRANSPORT");
        if (tr != NULL && (strcmp(tr, "tcp") == 0 || strcmp(tr, "udp") == 0)) {
            av_dict_set(&opt, "rtsp_transport", tr, 0);
        }
    }
    f->t_open_begin_us = ff_mono_us();
    f->probe_incomplete = 0;
    f->dur_open_input_us = -1;
    f->dur_find_stream_info_us = -1;
    arm_deadline(f);
    ff_hook_av_log();
    ff_log_owner = f;
    ret = open_input_with_retry(f, url, &opt);
    f->dur_open_input_us = ff_mono_us() - f->t_open_begin_us;
    if (f->dur_open_input_us < 0) {
        f->dur_open_input_us = 0;
    }
    ff_log_owner = NULL;
    av_dict_free(&opt);
    if (ret < 0) {
        char abuf[64];
        /* avformat_open_input 失败时会自己释放我们传入的 context */
        f->ic = NULL;
        f->state = FF_ST_FAILED;
        av_strerror(ret, abuf, sizeof(abuf));
        ff_hide_urls(abuf);
        {
            char lbuf[128];
            snprintf(lbuf, sizeof(lbuf), "avformat_open_input failed: %s (%d)", abuf, ret);
            ff_log(f, 0, lbuf);
        }
        return io_status(f, ret);
    }
    /* 再次确认最终 location（首个 HTTP 会话内跳转）。 */
    if (f->ic->pb != NULL) {
        uint8_t *loc = NULL;
        if (av_opt_get(f->ic->pb, "location", AV_OPT_SEARCH_CHILDREN, &loc) >= 0 && loc != NULL) {
            if (check_location_downgrade(f, url, (const char *)loc) != 0) {
                av_free(loc);
                close_input(f);
                f->state = FF_ST_FAILED;
                return MS_ERR_UNSUPPORTED;
            }
            av_free(loc);
        }
    }
    /* 无 decoder 探测。失败不偷偷启用软解，只记录错误并仍尝试读包。
     * FAST_START 使用短探测 deadline，避免把 io_timeout 当成起播预算。
     * DEFAULT 仍用同一轮 open 的 io_timeout，避免无界阻塞。 */
    if (f->probe_mode == MS_FF_PROBE_FAST_START) {
        arm_deadline_ms(f, MS_FF_FAST_PROBE_DEADLINE_MS);
    }
    {
        int64_t t_info0 = ff_mono_us();
        ff_hook_av_log();
        ff_log_owner = f;
        f->stream_info_err = avformat_find_stream_info(f->ic, NULL);
        ff_log_owner = NULL;
        f->dur_find_stream_info_us = ff_mono_us() - t_info0;
        if (f->dur_find_stream_info_us < 0) {
            f->dur_find_stream_info_us = 0;
        }
    }
    if (ff_is_canceled(f)) {
        close_input(f);
        f->state = FF_ST_CANCELED;
        f->last_status = MS_ERR_CANCELED;
        return MS_ERR_CANCELED;
    }
    {
        int probe_to = ff_is_timed_out(f);
        /* 探测 deadline / 不完整 metadata 不是「全部识别成功」。
         * 仅当 AVFormatContext 仍可继续读时才保持打开，并设置 probe_incomplete。
         * 上下文已不可用则返回可诊断错误，不把部分失败当成成功。 */
        if (f->stream_info_err < 0 || probe_to) {
            f->probe_incomplete = 1;
        }
        /* 软 interrupt（EXIT/ETIMEDOUT/EINTR）清掉后可继续读。
         * 若探测已成功或已识别到流，即使 pb 随后出现硬错误也保持打开，
         * 由后续 av_read_frame 报告 I/O 失败，避免把已缓冲的包当成 open 失败。
         * 探测失败且上下文不在/不可恢复才返回可诊断错误。 */
        if (!input_context_present(f)) {
            ms_status st;
            ff_log(f, 0, "find_stream_info left no input context; not treating as open success");
            st = io_status(f, f->stream_info_err < 0 ? f->stream_info_err : AVERROR_EXIT);
            close_input_keep_open_stats(f);
            f->state = FF_ST_FAILED;
            f->last_status = (st == MS_OK) ? MS_ERR_UNKNOWN : st;
            return f->last_status;
        }
        if (!recover_probe_io(f)) {
            int have_streams = (f->ic != NULL && f->ic->nb_streams > 0);
            if (!(f->stream_info_err >= 0 && have_streams)) {
                ms_status st;
                int perr = (f->ic != NULL && f->ic->pb != NULL) ? f->ic->pb->error : 0;
                char lbuf[180];
                snprintf(lbuf, sizeof(lbuf),
                         "find_stream_info left unusable input (pb_error=%d stream_info_err=%d); not treating as open success",
                         perr, f->stream_info_err);
                ff_log(f, 0, lbuf);
                st = io_status(f, f->stream_info_err < 0 ? f->stream_info_err : (perr < 0 ? perr : AVERROR_EXIT));
                close_input_keep_open_stats(f);
                f->state = FF_ST_FAILED;
                f->last_status = (st == MS_OK) ? MS_ERR_UNKNOWN : st;
                return f->last_status;
            }
            ff_log(f, 1, "probe identified streams; keeping context open despite later pb I/O error");
        }
        if (f->stream_info_err < 0) {
            ff_log(f, 1, "find_stream_info incomplete without decoders; packets may still be read");
        }
        if (probe_to) {
            /* 探测短 deadline 到期不是后续读包的永久超时。 */
            atomic_store(&f->timed_out, 0);
            disarm_deadline(f);
        }
    }
    f->held = av_packet_alloc();
    if (f->held == NULL) {
        close_input(f);
        f->state = FF_ST_FAILED;
        return MS_ERR_NOMEM;
    }
    f->au_emitted = 0;
    f->state = FF_ST_OPEN;
    disarm_deadline(f); /* 之后每次 av_read_frame 单独武装，避免整段总超时 */
    ff_apply_default_selection(f);
    /* 先选中默认字幕再回绕：探测消耗发生在 find_stream_info，与是否 DEFAULT 无关。 */
    ff_rewind_hls_subs_after_probe(f);
    f->origin_90k = choose_time_origin(f);
    return MS_OK;
}

static int produce_meta(ms_ff *f)
{
    ms_event ev;
    ms_status st;
    unsigned i, n, si;
    if (!f->meta_format_sent) {
        memset(&ev, 0, sizeof(ev));
        ev.type = MS_EVENT_FORMAT;
        ev.format = map_format(f->ic);
        if (ev.format == MS_FORMAT_MPEG_TS) {
            ev.ts_packet_size = 188;
        }
        st = enqueue_event(f, ev, NULL, 0, NULL);
        if (st != MS_OK) {
            return st;
        }
        f->meta_format_sent = 1;
        return 1;
    }
    n = f->ic != NULL ? f->ic->nb_streams : 0;
    while (f->meta_track_sent < n) {
        AVStream *stv = f->ic->streams[f->meta_track_sent];
        memset(&ev, 0, sizeof(ev));
        ev.type = MS_EVENT_TRACK;
        ev.format = map_format(f->ic);
        fill_track(f, &ev.track, stv);
        ev.sample_format = sample_fmt_for(stv, NULL);
        ev.track.level = MS_RECOG_BITSTREAM;
        ff_apply_default_selection(f);
        ff_copy_meta(f, f->meta_track_sent, stv, &ev.track);
        st = enqueue_event(f, ev, NULL, 0, ev.track.language);
        if (st != MS_OK) {
            return st;
        }
        if (f->meta_track_sent < 64u) {
            f->track_codec_sent[f->meta_track_sent] = (int)ev.track.codec;
        }
        f->meta_track_sent++;
        return 1;
    }
    while (f->meta_cfg_sent < n) {
        AVStream *stv = f->ic->streams[f->meta_cfg_sent];
        const uint8_t *extra;
        int extra_size;
        f->meta_cfg_sent++;
        if (stv == NULL || stv->codecpar == NULL) {
            continue;
        }
        extra = stv->codecpar->extradata;
        extra_size = stv->codecpar->extradata_size;
        if (extra == NULL || extra_size <= 0) {
            continue;
        }
        {
            ms_codec cc = map_codec(stv->codecpar->codec_id);
            if (cc == MS_CODEC_AC3 || cc == MS_CODEC_MP3 || cc == MS_CODEC_MP2) {
                /* 合法无 CSD：不把容器 dac3/等 extradata 当下发 CSD，也不捏造 AAC ASC。 */
                continue;
            }
            if (cc == MS_CODEC_OPUS) {
                ms_audio_cfg ac;
                ms_audio_cfg_default(&ac);
                if (ms_audio_cfg_parse(cc, MS_SAMPLE_FMT_OPUS, 1, extra,
                                       (size_t)extra_size, &ac) != MS_OK ||
                    !ac.complete || ac.csd_size == 0) {
                    continue; /* 没有真实 OpusHead/dOps 则不下发，不捏造 */
                }
                extra = ac.csd;
                extra_size = (int)ac.csd_size;
                memset(&ev, 0, sizeof(ev));
                ev.type = MS_EVENT_CODEC_CONFIG;
                ev.format = map_format(f->ic);
                fill_track(f, &ev.track, stv);
                ev.track.level = MS_RECOG_AU;
                ev.sample_format = sample_fmt_for(stv, NULL);
                ev.is_keyframe = 1;
                st = enqueue_event(f, ev, extra, (size_t)extra_size, NULL);
                if (st != MS_OK) {
                    f->meta_cfg_sent--;
                    return st;
                }
                si = f->meta_cfg_sent - 1u;
                if (si < 64u) {
                    f->cfg_emitted_bits |= (1ull << si);
                    f->cfg_sig[si] = extra_sig(extra, extra_size);
                    f->cfg_sig_valid[si] = 1;
                    f->cfg_need_resend[si] = 0;
                }
                return 1;
            }
        }
        memset(&ev, 0, sizeof(ev));
        ev.type = MS_EVENT_CODEC_CONFIG;
        ev.format = map_format(f->ic);
        fill_track(f, &ev.track, stv);
        ev.track.level = MS_RECOG_AU;
        ev.sample_format = sample_fmt_for(stv, NULL);
        ev.is_keyframe = 1;
        st = enqueue_event(f, ev, extra, (size_t)extra_size, NULL);
        if (st != MS_OK) {
            f->meta_cfg_sent--;
            return st;
        }
        si = f->meta_cfg_sent - 1u;
        if (si < 64u) {
            f->cfg_emitted_bits |= (1ull << si);
            f->cfg_sig[si] = extra_sig(extra, extra_size);
            f->cfg_sig_valid[si] = 1;
            f->cfg_need_resend[si] = 0;
        }
        return 1;
    }
    (void)i;
    return 0;
}


#if !defined(__SIZEOF_INT128__)
/* 无 128 位整型的目标（如 armeabi-v7a）：64×64 乘后向 -inf 除。x86_64 仍走 __int128。 */
static void ms_ff_u64_mul(uint64_t a, uint64_t b, uint64_t *hi, uint64_t *lo)
{
    uint64_t a0 = (uint32_t)a;
    uint64_t a1 = a >> 32;
    uint64_t b0 = (uint32_t)b;
    uint64_t b1 = b >> 32;
    uint64_t p0 = a0 * b0;
    uint64_t p1 = a0 * b1;
    uint64_t p2 = a1 * b0;
    uint64_t p3 = a1 * b1;
    uint64_t mid = (p0 >> 32) + (uint32_t)p1 + (uint32_t)p2;
    *lo = (p0 & 0xffffffffull) | (mid << 32);
    *hi = p3 + (p1 >> 32) + (p2 >> 32) + (mid >> 32);
}

static uint64_t ms_ff_u128_divu64(uint64_t n_hi, uint64_t n_lo, uint64_t den, uint64_t *rem)
{
    /* 前置条件：n_hi < den，商可放入 64 位。 */
    uint64_t r = n_hi;
    uint64_t q = 0;
    int i;
    for (i = 0; i < 64; i++) {
        uint64_t rh = r >> 63;
        r = (r << 1) | (n_lo >> 63);
        n_lo <<= 1;
        q <<= 1;
        if (rh || r >= den) {
            r -= den;
            q |= 1ull;
        }
    }
    *rem = r;
    return q;
}

static int64_t ms_ff_floor_muldiv(int64_t v, int64_t num, int64_t den)
{
    int sign = 1;
    uint64_t au, bu, du, hi, lo, q, r;
    if (v < 0) {
        sign = -sign;
        au = (uint64_t)(-(v + 1)) + 1ull;
    } else {
        au = (uint64_t)v;
    }
    if (num < 0) {
        sign = -sign;
        bu = (uint64_t)(-(num + 1)) + 1ull;
    } else {
        bu = (uint64_t)num;
    }
    if (den < 0) {
        sign = -sign;
        du = (uint64_t)(-(den + 1)) + 1ull;
    } else {
        du = (uint64_t)den;
    }
    ms_ff_u64_mul(au, bu, &hi, &lo);
    if (hi >= du) {
        return (sign > 0) ? INT64_MAX : INT64_MIN;
    }
    q = ms_ff_u128_divu64(hi, lo, du, &r);
    if (sign < 0) {
        if (r != 0ull) {
            if (q == UINT64_MAX) {
                return INT64_MIN;
            }
            q += 1ull;
        }
        if (q > (uint64_t)INT64_MAX + 1ull) {
            return INT64_MIN;
        }
        if (q == (uint64_t)INT64_MAX + 1ull) {
            return INT64_MIN;
        }
        return -(int64_t)q;
    }
    if (q > (uint64_t)INT64_MAX) {
        return INT64_MAX;
    }
    return (int64_t)q;
}
#endif

static int64_t rescale_down(int64_t v, AVRational from, AVRational to)
{
    /* 与公共 ms_rescale_q 相同：朝 -inf。libmsff 不链 libmsmedia。 */
    if (from.num <= 0 || from.den <= 0 || to.num <= 0 || to.den <= 0) {
        return v;
    }
#if defined(__SIZEOF_INT128__)
    {
        __int128 num, den, n, q, r;
        num = (__int128)from.num * to.den;
        den = (__int128)from.den * to.num;
        if (den <= 0) {
            return v;
        }
        n = (__int128)v * num;
        q = n / den;
        r = n % den;
        if (n < 0 && r != 0) {
            q -= 1;
        }
        if (q > (__int128)INT64_MAX) {
            return INT64_MAX;
        }
        if (q < (__int128)INT64_MIN) {
            return INT64_MIN;
        }
        return (int64_t)q;
    }
#else
    {
        int64_t num = (int64_t)from.num * (int64_t)to.den;
        int64_t den = (int64_t)from.den * (int64_t)to.num;
        if (den <= 0) {
            return v;
        }
        return ms_ff_floor_muldiv(v, num, den);
    }
#endif
}

static int64_t choose_time_origin(ms_ff *f)
{
    int64_t origin = 0;
    int have = 0;
    unsigned i;
    AVRational tb90 = {1, 90000};
    if (f == NULL || f->ic == NULL) {
        return 0;
    }
    /* 取各轨/容器 start_time 与 priming 的最小值作原点。
     * 旧逻辑只接受 t<0，直播 HLS 绝对 PTS（数万秒）原点一直为 0 →
     * 视频相对时间巨大、音频钟从 0 走 → MsSync 永久 held（出首帧后卡死）。 */
    if (f->ic->start_time != AV_NOPTS_VALUE) {
        origin = rescale_down(f->ic->start_time, AV_TIME_BASE_Q, tb90);
        have = 1;
    }
    for (i = 0; i < f->ic->nb_streams; i++) {
        AVStream *st = f->ic->streams[i];
        int64_t t;
        if (st == NULL || st->time_base.num <= 0 || st->time_base.den <= 0) {
            continue;
        }
        if (st->start_time != AV_NOPTS_VALUE) {
            t = rescale_down(st->start_time, st->time_base, tb90);
            if (!have || t < origin) {
                origin = t;
                have = 1;
            }
        }
        /* lavf 9 的 matroska 等容器常把 start_time 记为 0，实际 priming 在 initial_padding。
         * 若不纳入 origin，首个视频 AU 发出后 AAC/Opus 负 PTS 会被当成越界 OVERFLOW。 */
        if (st->codecpar != NULL && st->codecpar->initial_padding > 0 &&
            st->codecpar->sample_rate > 0) {
            AVRational sr = {1, st->codecpar->sample_rate};
            t = rescale_down(-(int64_t)st->codecpar->initial_padding, sr, tb90);
            if (!have || t < origin) {
                origin = t;
                have = 1;
            }
        }
    }
    f->origin_from_meta = have;
    return have ? origin : 0;
}

int64_t ms_ff_time_origin_90k(const ms_ff *f)
{
    return f != NULL ? f->origin_90k : 0;
}

void ms_ff_ts_prefix_stats(const ms_ff *f, unsigned *strip_ops,
                           uint64_t *last_bytes, int *last_kind)
{
    if (strip_ops) {
        *strip_ops = f != NULL ? f->ts_prefix_strip_ops : 0;
    }
    if (last_bytes) {
        *last_bytes = f != NULL ? f->ts_prefix_last_bytes : 0;
    }
    if (last_kind) {
        *last_kind = f != NULL ? f->ts_prefix_last_kind : 0;
    }
}

static int origin_tb_for_stream(const ms_ff *f, const AVStream *stv, int64_t *out)
{
    AVRational tb90 = {1, 90000};
    if (stv == NULL || stv->time_base.num <= 0 || stv->time_base.den <= 0) {
        *out = 0;
        return f->origin_90k == 0 ? 0 : -1;
    }
    *out = rescale_down(f->origin_90k, tb90, stv->time_base);
    return 0;
}

static int maybe_extend_origin(ms_ff *f, const AVStream *stv, int64_t ts)
{
    AVRational tb90 = {1, 90000};
    int64_t t90;
    if (ts == AV_NOPTS_VALUE || f->au_emitted) {
        return 0;
    }
    t90 = rescale_down(ts, stv->time_base, tb90);
    if (!f->origin_from_meta) {
        /* 探测未给出 start_time（快开 HLS 常见）：首包 PTS 锚定，后续包仅可下调。 */
        f->origin_90k = t90;
        f->origin_from_meta = 1;
        return 0;
    }
    if (t90 < f->origin_90k) {
        f->origin_90k = t90;
    }
    return 0;
}


/* 把 AV_PKT_DATA_SKIP_SAMPLES 拷到事件。start/end 是解码后 PCM 样点，不写入 dts。
 * 有 side data 时不再用 pkt->duration 推断，以免与 Ogg 已缩短的 duration 双扣。
 * MP4 常把末包 duration 缩短却不带 discard_padding：仅在完全没有 side data 时，
 * 把 (TOC样点 - duration换算样点) 记为 end skip。这是本裁剪 FFmpeg 的已知缺口补偿，
 * 不是 invent 一个与元数据冲突的数。 */
static void fill_audio_skip(ms_ff *f, const AVPacket *pkt, const AVStream *stv)
{
    size_t sz = 0;
    const uint8_t *sd;
    if (f == NULL) {
        return;
    }
    f->pending_skip_start = 0;
    f->pending_skip_end = 0;
    if (pkt == NULL) {
        return;
    }
    sd = av_packet_get_side_data(pkt, AV_PKT_DATA_SKIP_SAMPLES, &sz);
    if (sd != NULL && sz >= 8) {
        f->pending_skip_start = AV_RL32(sd);
        f->pending_skip_end = AV_RL32(sd + 4);
        return;
    }
    if (stv == NULL || stv->codecpar == NULL ||
        stv->codecpar->codec_id != AV_CODEC_ID_OPUS ||
        pkt->duration <= 0 || pkt->data == NULL || pkt->size <= 0) {
        return;
    }
    {
        ms_audio_au_info info;
        int64_t dur_samp;
        memset(&info, 0, sizeof(info));
        info.struct_size = sizeof(info);
        if (ms_audio_au_inspect(MS_CODEC_OPUS, MS_SAMPLE_FMT_OPUS,
                                pkt->data, (size_t)pkt->size, &info) != MS_OK ||
            info.samples == 0) {
            return;
        }
        if (stv->time_base.num > 0 && stv->time_base.den > 0) {
            dur_samp = av_rescale_q(pkt->duration, stv->time_base, (AVRational){1, 48000});
        } else {
            dur_samp = pkt->duration;
        }
        if (dur_samp > 0 && dur_samp < (int64_t)info.samples && dur_samp <= 5760) {
            f->pending_skip_end = info.samples - (uint32_t)dur_samp;
        }
    }
}

static ms_status fill_packet_ts(ms_ff *f, const AVPacket *pkt, const AVStream *stv, ms_event *ev)
{
    int64_t origin_tb = 0;
    if (stv->time_base.num > 0 && stv->time_base.den > 0) {
        ev->time_base_num = stv->time_base.num;
        ev->time_base_den = stv->time_base.den;
    }
    if (pkt->pts != AV_NOPTS_VALUE) {
        if (maybe_extend_origin(f, stv, pkt->pts) != 0) {
            ff_log(f, 0, "pts below session origin after first AU");
            return MS_ERR_OVERFLOW;
        }
    }
    if (pkt->dts != AV_NOPTS_VALUE) {
        if (maybe_extend_origin(f, stv, pkt->dts) != 0) {
            ff_log(f, 0, "dts below session origin after first AU");
            return MS_ERR_OVERFLOW;
        }
    }
    if (origin_tb_for_stream(f, stv, &origin_tb) != 0) {
        return MS_ERR_INVALID_ARG;
    }
    if (pkt->pts != AV_NOPTS_VALUE) {
        int64_t rel;
        if (pkt->pts < origin_tb) {
            ff_log(f, 0, "pts below unified origin");
            return MS_ERR_OVERFLOW;
        }
        rel = pkt->pts - origin_tb;
        ev->has_pts = 1;
        ev->pts = (uint64_t)rel;
        if (pkt->pts < 0) {
            ev->preroll = 1;
        }
    }
    if (pkt->dts != AV_NOPTS_VALUE) {
        int64_t rel;
        if (pkt->dts < origin_tb) {
            ff_log(f, 0, "dts below unified origin");
            return MS_ERR_OVERFLOW;
        }
        rel = pkt->dts - origin_tb;
        ev->has_dts = 1;
        ev->dts = (uint64_t)rel;
        /* 负 DTS 对视频是 B 帧重排，不是 priming/preroll。
         * 若标成 preroll，硬解会话会丢掉首个 IDR，随后 GOP 内非关键帧被 waiting_key 丢弃
         * （设备验收中 p4_seek.mp4 出现 au 一半 drop_nokey、首帧不出画）。 */
        if (pkt->dts < 0 && ev->preroll == 0 &&
            stv != NULL && stv->codecpar != NULL &&
            stv->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            ev->preroll = 1;
        }
    }
    return MS_OK;
}

static int packet_is_complete_enough(const AVPacket *pkt)
{
    /* AVPacket 不一定天然等于完整 AU。RTP/RTSP 路径依赖 FFmpeg 的 depacketizer
     * 与 parser（非 decoder）组帧后再到这里。
     * 直播 MPEG-TS 常被 lavf 标 AV_PKT_FLAG_CORRUPT（PES/连续计数），Media3 仍送样；
     * 真失包走 rtp_gap→skip_to_key，不在此因 CORRUPT 拒包。 */
    return pkt != NULL && pkt->data != NULL && pkt->size > 0;
}


static unsigned hevc_len_size(const AVStream *st)
{
    const uint8_t *extra;
    int n;
    if (st == NULL || st->codecpar == NULL) {
        return 4u;
    }
    extra = st->codecpar->extradata;
    n = st->codecpar->extradata_size;
    if (extra != NULL && n >= 23 && extra[0] == 1) {
        return (unsigned)((extra[21] & 3u) + 1u);
    }
    return 4u;
}

static uint32_t hevc_ps_sig(const ms_hevc_ps_set *ps)
{
    uint32_t s = 0x811c9dc5u;
    size_t i;
    if (ps == NULL || !ps->triple_complete) {
        return 0;
    }
    for (i = 0; i < ps->vps_size; i++) {
        s ^= ps->vps[i];
        s *= 16777619u;
    }
    for (i = 0; i < ps->sps_size; i++) {
        s ^= ps->sps[i];
        s *= 16777619u;
    }
    for (i = 0; i < ps->pps_size; i++) {
        s ^= ps->pps[i];
        s *= 16777619u;
    }
    return s ? s : 1u;
}

static int collect_hevc_cfg_from_au(ms_sample_format fmt, const uint8_t *d, int n,
                                    unsigned ls, uint8_t *out, int cap, uint32_t *sig)
{
    ms_hevc_ps_set ps;
    size_t w = 0;
    if (d == NULL || n <= 0 || out == NULL) {
        return 0;
    }
    memset(&ps, 0, sizeof(ps));
    ps.struct_size = sizeof(ps);
    if (ms_hevc_ps_parse(fmt, 0, d, (size_t)n, ls, &ps) != MS_OK || !ps.triple_complete) {
        return 0;
    }
    if (ms_hevc_ps_pack(fmt == MS_SAMPLE_FMT_HVCC ? MS_SAMPLE_FMT_HVCC
                                                  : MS_SAMPLE_FMT_ANNEXB,
                        ls ? ls : 4u, &ps, out, (size_t)cap, &w) != MS_OK) {
        return 0;
    }
    if (sig) {
        *sig = hevc_ps_sig(&ps);
    }
    return (int)w;
}

/* 直播 TS 在 FAST_START 后常不置 AV_PKT_FLAG_KEY；扫 AnnexB/AVCC 找 IDR(5)。 */
static int h264_pkt_is_key(ms_sample_format fmt, const AVPacket *pkt, unsigned ls)
{
    const uint8_t *d;
    int n, i;
    if (pkt == NULL || pkt->data == NULL || pkt->size <= 0) {
        return 0;
    }
    if ((pkt->flags & AV_PKT_FLAG_KEY) != 0) {
        return 1;
    }
    d = pkt->data;
    n = pkt->size;
    if (fmt == MS_SAMPLE_FMT_AVCC && ls >= 1 && ls <= 4) {
        i = 0;
        while (i + (int)ls < n) {
            unsigned sz = 0;
            int j, nal;
            for (j = 0; j < (int)ls; j++) {
                sz = (sz << 8) | d[i + j];
            }
            i += (int)ls;
            if (sz == 0 || i + (int)sz > n) {
                break;
            }
            nal = d[i] & 0x1f;
            if (nal == 5) {
                return 1;
            }
            i += (int)sz;
        }
        return 0;
    }
    i = 0;
    while (i + 4 <= n) {
        int sc, nal;
        if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1) {
            sc = 3;
        } else if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 0 && d[i + 3] == 1) {
            sc = 4;
        } else {
            i++;
            continue;
        }
        nal = i + sc;
        if (nal < n && (d[nal] & 0x1f) == 5) {
            return 1;
        }
        i = nal + 1;
    }
    return 0;
}

static int hevc_pkt_is_key(ms_sample_format fmt, const AVPacket *pkt, unsigned ls)
{
    ms_hevc_au_info info;
    if (pkt == NULL) {
        return 0;
    }
    if ((pkt->flags & AV_PKT_FLAG_KEY) != 0) {
        return 1;
    }
    memset(&info, 0, sizeof(info));
    info.struct_size = sizeof(info);
    if (ms_hevc_au_inspect(fmt, pkt->data, (size_t)pkt->size, ls, &info) == MS_OK &&
        info.is_irap) {
        return 1;
    }
    return 0;
}

/* 从 Annex B AU 收集 SPS(7)+PPS(8)，供探测未给出 extradata 时补发 CODEC_CONFIG。
 * 不作为完整 in-band 参数集解析器；没有成对 SPS/PPS 则返回 0。 */
static int vbit_pkt_is_key(ms_codec codec, ms_sample_format fmt, const AVPacket *pkt)
{
    ms_video_au_info info;
    if (pkt == NULL || pkt->data == NULL || pkt->size <= 0) {
        return 0;
    }
    if ((pkt->flags & AV_PKT_FLAG_KEY) != 0) {
        return 1;
    }
    if (!ms_video_hw_path_ready(codec)) {
        return 0;
    }
    memset(&info, 0, sizeof(info));
    info.struct_size = sizeof(info);
    if (ms_video_au_inspect(codec, fmt, pkt->data, (size_t)pkt->size, &info) != MS_OK) {
        return 0;
    }
    return info.is_key ? 1 : 0;
}


static int collect_annexb_sps_pps(const uint8_t *d, int n, uint8_t *out, int cap)
{
    int i = 0, w = 0, have_sps = 0, have_pps = 0;
    if (d == NULL || n <= 4 || out == NULL || cap < 8) {
        return 0;
    }
    while (i + 4 <= n) {
        int sc, nal, end, type, need;
        if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1) {
            sc = 3;
        } else if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 0 && d[i + 3] == 1) {
            sc = 4;
        } else {
            i++;
            continue;
        }
        nal = i + sc;
        end = nal;
        while (end + 3 <= n) {
            if (d[end] == 0 && d[end + 1] == 0 &&
                (d[end + 2] == 1 || (end + 3 < n && d[end + 2] == 0 && d[end + 3] == 1))) {
                break;
            }
            end++;
        }
        if (end + 3 > n) {
            end = n;
        }
        if (nal < n) {
            type = d[nal] & 0x1f;
            if ((type == 7 || type == 8) && end > nal) {
                need = 4 + (end - nal);
                if (w + need > cap) {
                    return 0;
                }
                out[w++] = 0;
                out[w++] = 0;
                out[w++] = 0;
                out[w++] = 1;
                memcpy(out + w, d + nal, (size_t)(end - nal));
                w += (end - nal);
                if (type == 7) {
                    have_sps = 1;
                }
                if (type == 8) {
                    have_pps = 1;
                }
            }
        }
        i = end;
    }
    return (have_sps && have_pps) ? w : 0;
}


/* 从 ADTS 帧头拼 2 字节 AudioSpecificConfig（与 ms_adts 子集一致：拒绝 sfi>12 与 chan=0）。 */
static int aac_asc_from_adts(const uint8_t *p, int n, uint8_t asc[2])
{
    int profile, sfi, chan, hlen, aot;
    if (p == NULL || n < 7 || asc == NULL) {
        return 0;
    }
    if (p[0] != 0xff || (p[1] & 0xf0) != 0xf0) {
        return 0;
    }
    hlen = (p[1] & 0x01) ? 7 : 9;
    if (n < hlen) {
        return 0;
    }
    profile = (p[2] >> 6) & 0x03;
    sfi = (p[2] >> 2) & 0x0f;
    chan = ((p[2] & 0x01) << 2) | ((p[3] >> 6) & 0x03);
    if (sfi > 12 || chan == 0) {
        return 0;
    }
    aot = profile + 1;
    asc[0] = (uint8_t)(((aot & 0x1f) << 3) | ((sfi >> 1) & 0x07));
    asc[1] = (uint8_t)(((sfi & 0x01) << 7) | ((chan & 0x0f) << 3));
    return 2;
}

static ms_status produce_packet(ms_ff *f)
{
    int ret;
    AVStream *stv;
    ms_event ev;
    ms_status st;
    if (f->eof) {
        return MS_EOS;
    }
    if (!f->held_ready) {
        av_packet_unref(f->held);
        arm_deadline(f);
        ff_hook_av_log();
        ff_log_owner = f;
        ret = av_read_frame(f->ic, f->held);
        ff_log_owner = NULL;
        disarm_deadline(f);
        if (ret == AVERROR_EOF) {
            if (f->pending_video_si >= 0) {
                ff_log(f, 0, "video variant switch ended before keyframe; keeping previous track");
                f->pending_video_si = -1;
                ff_apply_discard(f);
            }
            if (f->pending_audio_si >= 0) {
                ff_log(f, 0, "alternate audio switch ended before commit; keeping previous track");
                f->pending_audio_si = -1;
                ff_apply_discard(f);
            }
            if (f->pending_sub_si >= 0) {
                ff_log(f, 0, "alternate subtitle switch ended before commit; keeping previous track");
                f->pending_sub_si = -1;
                ff_apply_discard(f);
            }
            f->eof = 1;
            return MS_EOS;
        }
        if (ret < 0) {
            if (f->pending_audio_si >= 0) {
                /* 切换失败不得拆掉已打开会话，也不把新轨写成已选中。 */
                ff_log(f, 0, "alternate audio switch failed; keeping previous track");
                f->pending_audio_si = -1;
                ff_apply_discard(f);
            }
            if (f->pending_sub_si >= 0) {
                ff_log(f, 0, "alternate subtitle switch failed; keeping previous track");
                f->pending_sub_si = -1;
                ff_apply_discard(f);
                /* 字幕外链失败不把整次 pull 写成会话死亡，视频/音频继续。 */
                return MS_AGAIN;
            }
            if (f->pending_video_si >= 0) {
                ff_log(f, 0, "video variant switch failed; keeping previous track");
                f->pending_video_si = -1;
                ff_apply_discard(f);
                return MS_AGAIN;
            }
            return io_status(f, ret);
        }
        f->held_ready = 1;
    }
    /* 直播 TS 的 CORRUPT 多半是 lavf 误伤；Media3 不因此全局等关键帧。
     * 真 RTP 失包仍由 rtp_gap 置 skip_to_key。此处只清旗标继续送硬解。 */
    if (f->held->flags & AV_PKT_FLAG_CORRUPT) {
        f->held->flags &= ~AV_PKT_FLAG_CORRUPT;
    }
    if (!packet_is_complete_enough(f->held)) {
        av_packet_unref(f->held);
        f->held_ready = 0;
        return MS_AGAIN;
    }
    if (f->held->stream_index < 0 ||
        (unsigned)f->held->stream_index >= f->ic->nb_streams) {
        av_packet_unref(f->held);
        f->held_ready = 0;
        return MS_AGAIN;
    }
    stv = f->ic->streams[f->held->stream_index];
    ff_apply_default_selection(f);
    if (stv->codecpar != NULL &&
        stv->codecpar->codec_type == AVMEDIA_TYPE_AUDIO &&
        f->pending_audio_si >= 0 &&
        f->held->stream_index == f->pending_audio_si) {
        /* 新备选音轨已读到包：提交切换，停止旧轨输出并重发配置。 */
        ff_commit_audio(f, f->pending_audio_si, 1);
    }
    if (stv->codecpar != NULL &&
        stv->codecpar->codec_type == AVMEDIA_TYPE_SUBTITLE &&
        f->pending_sub_si >= 0 &&
        f->held->stream_index == f->pending_sub_si) {
        /* 新字幕 rendition 已读到包：提交切换，清空旧 cue，不重开视频/音频。 */
        ff_commit_sub(f, f->pending_sub_si, 1);
    }
    if (stv->codecpar != NULL &&
        stv->codecpar->codec_type == AVMEDIA_TYPE_VIDEO &&
        f->pending_video_si >= 0 &&
        f->held->stream_index == f->pending_video_si) {
        int is_key = (f->held->flags & AV_PKT_FLAG_KEY) != 0;
        if (!is_key && stv->codecpar->codec_id == AV_CODEC_ID_HEVC) {
            is_key = hevc_pkt_is_key(sample_fmt_for(stv, f->held), f->held,
                                     hevc_len_size(stv));
        }
        if (!is_key) {
            ms_track_info kti;
            memset(&kti, 0, sizeof(kti));
            fill_track(f, &kti, stv);
            if (kti.codec == MS_CODEC_MPEG2_VIDEO ||
                kti.codec == MS_CODEC_MPEG4_PART2 ||
                kti.codec == MS_CODEC_H263 ||
                kti.codec == MS_CODEC_AV1 ||
                kti.codec == MS_CODEC_AVS_PLUS ||
                kti.codec == MS_CODEC_AVS_FAMILY) {
                is_key = vbit_pkt_is_key(kti.codec, sample_fmt_for(stv, f->held),
                                         f->held);
            }
        }
        if (is_key) {
            /* 新变体关键帧：提交、重发 CSD、不复用旧尺寸/参数，不抬 timeline_epoch。 */
            ff_commit_video(f, f->pending_video_si, 1);
        } else {
            av_packet_unref(f->held);
            f->held_ready = 0;
            return MS_AGAIN;
        }
    }
    if (!ff_stream_selected(f, stv) && stv->codecpar != NULL) {
        /* 未选中音/视频丢包。同一容器内可播字幕仍入队；HLS/DASH 外链不入队。 */
        enum AVMediaType typ = stv->codecpar->codec_type;
        ms_codec sc = map_codec(stv->codecpar->codec_id);
        ms_format fmt = map_format(f->ic);
        int muxed_sub = (typ == AVMEDIA_TYPE_SUBTITLE && ff_sub_playable(sc) &&
                         fmt != MS_FORMAT_HLS && fmt != MS_FORMAT_DASH &&
                         fmt != MS_FORMAT_RTSP && fmt != MS_FORMAT_RTP &&
                         fmt != MS_FORMAT_RTMP);
        if (!muxed_sub) {
            av_packet_unref(f->held);
            f->held_ready = 0;
            return MS_AGAIN;
        }
    }
    if (stv->codecpar != NULL && stv->codecpar->codec_type == AVMEDIA_TYPE_SUBTITLE) {
        ms_codec sc = map_codec(stv->codecpar->codec_id);
        char text[MS_SUB_CUE_TEXT_MAX + 1];
        size_t tlen = 0;
        ms_sub_cue cue;
        uint64_t start90 = 0, end90 = 0;
        if (!ff_sub_playable(sc)) {
            av_packet_unref(f->held);
            f->held_ready = 0;
            return MS_AGAIN;
        }
        if (sc == MS_CODEC_SUB_DVB || sc == MS_CODEC_SUB_PGS || sc == MS_CODEC_SUB_VOBSUB) {
            const uint8_t *extra = stv->codecpar != NULL ? stv->codecpar->extradata : NULL;
            int extra_size = stv->codecpar != NULL ? stv->codecpar->extradata_size : 0;
            unsigned si = (unsigned)f->held->stream_index;
            ms_sub_bitmap_kind kind = MS_SUB_BITMAP_DVB;
            if (sc == MS_CODEC_SUB_PGS) {
                kind = MS_SUB_BITMAP_PGS;
            } else if (sc == MS_CODEC_SUB_VOBSUB) {
                kind = MS_SUB_BITMAP_VOBSUB;
            }
            if (si < 64u && extra != NULL && extra_size > 0 &&
                (f->cfg_emitted_bits & (1ull << si)) == 0) {
                memset(&ev, 0, sizeof(ev));
                ev.type = MS_EVENT_CODEC_CONFIG;
                ev.format = map_format(f->ic);
                fill_track(f, &ev.track, stv);
                ff_copy_meta(f, si, stv, &ev.track);
                ev.track.level = MS_RECOG_AU;
                ev.track.codec = sc;
                if (f->sub_inband == NULL) {
                    (void)ms_sub_doc_create(f->alloc, &f->sub_inband);
                }
                if (f->sub_inband != NULL) {
                    (void)ms_sub_doc_set_bitmap_header(f->sub_inband, kind, extra, (size_t)extra_size);
                }
                f->cfg_emitted_bits |= (1ull << si);
                st = enqueue_event(f, ev, extra, (size_t)extra_size, ev.track.language);
                return st;
            }
            if (f->sub_inband == NULL) {
                (void)ms_sub_doc_create(f->alloc, &f->sub_inband);
            }
            if (f->sub_inband != NULL && extra != NULL && extra_size > 0) {
                (void)ms_sub_doc_set_bitmap_header(f->sub_inband, kind, extra, (size_t)extra_size);
            }
            memset(&ev, 0, sizeof(ev));
            ev.type = MS_EVENT_SUBTITLE;
            ev.format = map_format(f->ic);
            fill_track(f, &ev.track, stv);
            ff_copy_meta(f, (unsigned)stv->index, stv, &ev.track);
            ev.track.level = MS_RECOG_AU;
            ev.track.codec = sc;
            if (stv->time_base.num > 0 && stv->time_base.den > 0) {
                ev.time_base_num = stv->time_base.num;
                ev.time_base_den = stv->time_base.den;
            }
            st = fill_packet_ts(f, f->held, stv, &ev);
            if (st != MS_OK) {
                av_packet_unref(f->held);
                f->held_ready = 0;
                return st;
            }
            ev.has_dts = ev.has_pts;
            if (ev.has_pts && f->held->duration > 0) {
                ev.dts = ev.pts + (uint64_t)f->held->duration;
                ev.has_dts = 1;
            }
            if (ev.has_pts) {
                if (ev.time_base_num == 1 && ev.time_base_den == 90000) {
                    start90 = ev.pts;
                } else if (ev.time_base_num > 0 && ev.time_base_den > 0) {
                    start90 = ev.pts * 90000ull * (uint64_t)ev.time_base_num /
                              (uint64_t)ev.time_base_den;
                }
            }
            end90 = start90;
            if (ev.has_dts) {
                if (ev.time_base_num == 1 && ev.time_base_den == 90000) {
                    end90 = ev.dts;
                } else if (ev.time_base_num > 0 && ev.time_base_den > 0) {
                    end90 = ev.dts * 90000ull * (uint64_t)ev.time_base_num /
                            (uint64_t)ev.time_base_den;
                }
            }
            /* 无时长则保持开放，等下一显示集关闭；end==start 会让 cues_at 为空。 */
            {
                int has_end = ev.has_dts && end90 > start90;
                if (f->sub_inband != NULL) {
                    (void)ms_sub_doc_feed_bitmap(f->sub_inband, kind, ev.track.id,
                                                 f->held->data, (size_t)f->held->size,
                                                 ev.has_pts, start90, has_end, end90);
                }
            }
            /* 事件载荷为压缩段，供播放器再解析；不得当文字。 */
            st = enqueue_event(f, ev, f->held->data, (size_t)f->held->size, ev.track.language);
            av_packet_unref(f->held);
            f->held_ready = 0;
            return st;
        }
        if (sc == MS_CODEC_SUB_ASS) {
            const uint8_t *extra = stv->codecpar != NULL ? stv->codecpar->extradata : NULL;
            int extra_size = stv->codecpar != NULL ? stv->codecpar->extradata_size : 0;
            unsigned si = (unsigned)f->held->stream_index;
            if (si < 64u && extra != NULL && extra_size > 0 &&
                (f->cfg_emitted_bits & (1ull << si)) == 0) {
                memset(&ev, 0, sizeof(ev));
                ev.type = MS_EVENT_CODEC_CONFIG;
                ev.format = map_format(f->ic);
                fill_track(f, &ev.track, stv);
                ff_copy_meta(f, si, stv, &ev.track);
                ev.track.level = MS_RECOG_AU;
                ev.track.codec = sc;
                if (f->sub_inband == NULL) {
                    (void)ms_sub_doc_create(f->alloc, &f->sub_inband);
                }
                if (f->sub_inband != NULL) {
                    (void)ms_sub_doc_set_ass_header(f->sub_inband, extra, (size_t)extra_size);
                }
                f->cfg_emitted_bits |= (1ull << si);
                st = enqueue_event(f, ev, extra, (size_t)extra_size, ev.track.language);
                /* 保留当前字幕包到下一轮再解析。 */
                return st;
            }
            if (f->sub_inband == NULL) {
                (void)ms_sub_doc_create(f->alloc, &f->sub_inband);
            }
            if (f->sub_inband != NULL && extra != NULL && extra_size > 0) {
                (void)ms_sub_doc_set_ass_header(f->sub_inband, extra, (size_t)extra_size);
            }
        }
        if (sc == MS_CODEC_SUB_TX3G || sc == MS_CODEC_SUB_TEXT) {
            if (ms_sub_tx3g_text(f->held->data, (size_t)f->held->size, text, sizeof(text), &tlen) != MS_OK) {
                av_packet_unref(f->held);
                f->held_ready = 0;
                return MS_AGAIN;
            }
        } else if (sc == MS_CODEC_SUB_ASS) {
            /* 下面在填完时间后再解析；此处先占位，避免把标签当纯文本。 */
            tlen = 0;
            text[0] = 0;
        } else {
            /* SRT/WebVTT 包常为纯文本行；有界拷贝。 */
            tlen = (size_t)f->held->size;
            if (tlen >= sizeof(text)) {
                tlen = sizeof(text) - 1;
            }
            memcpy(text, f->held->data, tlen);
            text[tlen] = 0;
        }
        memset(&ev, 0, sizeof(ev));
        ev.type = MS_EVENT_SUBTITLE;
        ev.format = map_format(f->ic);
        fill_track(f, &ev.track, stv);
        ff_copy_meta(f, (unsigned)stv->index, stv, &ev.track);
        ev.track.level = MS_RECOG_AU;
        ev.track.codec = sc;
        if (stv->time_base.num > 0 && stv->time_base.den > 0) {
            ev.time_base_num = stv->time_base.num;
            ev.time_base_den = stv->time_base.den;
        }
        st = fill_packet_ts(f, f->held, stv, &ev);
        if (st != MS_OK) {
            av_packet_unref(f->held);
            f->held_ready = 0;
            return st;
        }
        ev.has_dts = ev.has_pts;
        if (ev.has_pts && f->held->duration > 0) {
            ev.dts = ev.pts + (uint64_t)f->held->duration;
            ev.has_dts = 1;
        }
        if (f->sub_inband == NULL) {
            (void)ms_sub_doc_create(f->alloc, &f->sub_inband);
        }
        ms_sub_cue_default(&cue);
        cue.track_id = ev.track.id;
        cue.has_start = ev.has_pts;
        cue.has_end = ev.has_dts;
        /* 文档时间轴用 90 kHz：若事件时基不是 90k，粗略换算。 */
        if (ev.has_pts) {
            if (ev.time_base_num == 1 && ev.time_base_den == 90000) {
                start90 = ev.pts;
            } else if (ev.time_base_num > 0 && ev.time_base_den > 0) {
                start90 = ev.pts * 90000ull * (uint64_t)ev.time_base_num / (uint64_t)ev.time_base_den;
            }
        }
        end90 = start90;
        if (ev.has_dts) {
            if (ev.time_base_num == 1 && ev.time_base_den == 90000) {
                end90 = ev.dts;
            } else if (ev.time_base_num > 0 && ev.time_base_den > 0) {
                end90 = ev.dts * 90000ull * (uint64_t)ev.time_base_num / (uint64_t)ev.time_base_den;
            }
        }
        cue.start_90k = start90;
        cue.end_90k = end90;
        if (sc == MS_CODEC_SUB_ASS) {
            char parsed[MS_SUB_CUE_TEXT_MAX + 1];
            ms_status pst;
            cue.track_id = ev.track.id;
            pst = ms_sub_ass_parse_event(f->sub_inband, f->held->data, (size_t)f->held->size,
                                         ev.has_pts, start90, ev.has_dts, end90,
                                         &cue, parsed, sizeof(parsed));
            if (pst != MS_OK || cue.text_size == 0) {
                av_packet_unref(f->held);
                f->held_ready = 0;
                return MS_AGAIN;
            }
            memcpy(text, parsed, cue.text_size);
            text[cue.text_size] = 0;
            tlen = cue.text_size;
            cue.text = text;
            cue.text_size = tlen;
            if (f->sub_inband != NULL) {
                (void)ms_sub_doc_add_cue(f->sub_inband, &cue);
            }
            /* 事件载荷用原始包，便于播放器按 ASS 再解析样式。 */
            st = enqueue_event(f, ev, f->held->data, (size_t)f->held->size, ev.track.language);
        } else {
            cue.text = text;
            cue.text_size = tlen;
            if (f->sub_inband != NULL) {
                (void)ms_sub_doc_add_cue(f->sub_inband, &cue);
            }
            st = enqueue_event(f, ev, (const uint8_t *)text, tlen, ev.track.language);
        }
        av_packet_unref(f->held);
        f->held_ready = 0;
        return st;
    }
    if (f->rtp_gap) {
        /* 失包后当前包可能仍带 KEY（残缺 IDR），不能当作安全关键帧送硬解。 */
        ff_log(f, 1, "rtp loss: drop residual, wait for next keyframe");
        f->rtp_gap = 0;
        f->skip_to_key = 1;
        av_packet_unref(f->held);
        f->held_ready = 0;
        return MS_AGAIN;
    }
    if (f->skip_to_key) {
        int is_key = (f->held->flags & AV_PKT_FLAG_KEY) != 0;
        if (!is_key && stv->codecpar != NULL &&
            stv->codecpar->codec_id == AV_CODEC_ID_H264) {
            ms_sample_format sf = sample_fmt_for(stv, f->held);
            unsigned als = 4u;
            if (sf == MS_SAMPLE_FMT_AVCC && stv->codecpar->extradata != NULL &&
                stv->codecpar->extradata_size >= 5) {
                als = (unsigned)((stv->codecpar->extradata[4] & 3u) + 1u);
            }
            is_key = h264_pkt_is_key(sf, f->held, als);
        }
        if (!is_key && stv->codecpar != NULL &&
            stv->codecpar->codec_id == AV_CODEC_ID_HEVC) {
            is_key = hevc_pkt_is_key(sample_fmt_for(stv, f->held), f->held,
                                     hevc_len_size(stv));
        }
        if (!is_key) {
            ms_track_info kti;
            memset(&kti, 0, sizeof(kti));
            fill_track(f, &kti, stv);
            if (kti.codec == MS_CODEC_MPEG2_VIDEO ||
                kti.codec == MS_CODEC_MPEG4_PART2 ||
                kti.codec == MS_CODEC_H263 ||
                kti.codec == MS_CODEC_AV1 ||
                kti.codec == MS_CODEC_AVS_PLUS ||
                kti.codec == MS_CODEC_AVS_FAMILY) {
                is_key = vbit_pkt_is_key(kti.codec, sample_fmt_for(stv, f->held),
                                         f->held);
            }
        }
        if (!is_key) {
            av_packet_unref(f->held);
            f->held_ready = 0;
            return MS_AGAIN;
        }
        f->skip_to_key = 0;
    }
    /* 迟迟出现的 PMT/轨：produce_meta 会因 nb_streams 增长补发 TRACK。
     * 已发 TRACK 后 codec 从未知变为可识别时发 TRACK_UPDATE（事件无采样率字段，
     * 不是完整音频参数通知；音频输出未实现）。
     * 打开时或之后 extradata 变化则发/再发 CODEC_CONFIG，AU 留到下次。 */
    {
        unsigned si = (unsigned)f->held->stream_index;
        const uint8_t *extra = stv->codecpar != NULL ? stv->codecpar->extradata : NULL;
        int extra_size = stv->codecpar != NULL ? stv->codecpar->extradata_size : 0;
        ms_track_info tip;
        int cur_codec;
        memset(&tip, 0, sizeof(tip));
        fill_track(f, &tip, stv);
        cur_codec = (int)tip.codec;
        if (si < 64u && f->track_codec_sent[si] >= 0 &&
            f->track_codec_sent[si] != cur_codec) {
            memset(&ev, 0, sizeof(ev));
            ev.type = MS_EVENT_TRACK_UPDATE;
            ev.format = map_format(f->ic);
            fill_track(f, &ev.track, stv);
            ev.track.level = MS_RECOG_BITSTREAM;
            ev.sample_format = sample_fmt_for(stv, NULL);
            st = enqueue_event(f, ev, NULL, 0, NULL);
            if (st != MS_OK) {
                return st;
            }
            f->track_codec_sent[si] = cur_codec;
            return MS_OK;
        }
        {
            uint8_t spspp[16 + 3 * MS_HEVC_PS_NAL_MAX];
            uint8_t aasc[2];
            int spn = 0, from_au = 0;
            if ((extra == NULL || extra_size <= 0) && f->held != NULL &&
                tip.codec == MS_CODEC_H264) {
                spn = collect_annexb_sps_pps(f->held->data, f->held->size, spspp,
                                             (int)sizeof(spspp));
                if (spn > 0) {
                    extra = spspp;
                    extra_size = spn;
                    from_au = 1;
                }
            }
            (void)from_au;
            if ((extra == NULL || extra_size <= 0) && f->held != NULL &&
                tip.codec == MS_CODEC_AAC) {
                int an = aac_asc_from_adts(f->held->data, (int)f->held->size, aasc);
                if (an == 2) {
                    extra = aasc;
                    extra_size = 2;
                }
            }
            /* PCM 通常没有 lavf extradata：把容器采样率/声道/位深打成 'MSPM' 块再发 CODEC_CONFIG。不是软解。 */
#ifdef MS_SINGLE_FFADEC
            /* 单 so 额外音频：不下发容器 extradata，统一用 MSPM 带采样率/声道（解码器自带带内配置）。 */
            if ((tip.codec == MS_CODEC_EAC3 || tip.codec == MS_CODEC_G711_ALAW ||
                 tip.codec == MS_CODEC_G711_ULAW || tip.codec == MS_CODEC_DTS ||
                 tip.codec == MS_CODEC_AAC_LATM) &&
                stv->codecpar != NULL && stv->codecpar->sample_rate > 0) {
                extra = NULL;
                extra_size = 0;
            }
#endif
            if ((tip.codec == MS_CODEC_PCM
#ifdef MS_SINGLE_FFADEC
                 || tip.codec == MS_CODEC_EAC3 || tip.codec == MS_CODEC_G711_ALAW ||
                 tip.codec == MS_CODEC_G711_ULAW || tip.codec == MS_CODEC_DTS ||
                 tip.codec == MS_CODEC_AAC_LATM
#endif
                 ) && (extra == NULL || extra_size <= 0) &&
                stv->codecpar != NULL && stv->codecpar->sample_rate > 0) {
                int ch = 0, bits = 16;
                uint32_t rate;
                ch = stv->codecpar->ch_layout.nb_channels;
                if (ch <= 0) {
                    ch = 2;
                }
                if (stv->codecpar->bits_per_coded_sample > 0) {
                    bits = stv->codecpar->bits_per_coded_sample;
                } else if (stv->codecpar->bits_per_raw_sample > 0) {
                    bits = stv->codecpar->bits_per_raw_sample;
                }
                if (tip.codec != MS_CODEC_PCM) {
                    bits = 16; /* 压缩音频只借 MSPM 带采样率/声道，位深按软解输出 s16 */
                }
                rate = (uint32_t)stv->codecpar->sample_rate;
                spspp[0] = 'M'; spspp[1] = 'S'; spspp[2] = 'P'; spspp[3] = 'M';
                spspp[4] = (uint8_t)(rate);
                spspp[5] = (uint8_t)(rate >> 8);
                spspp[6] = (uint8_t)(rate >> 16);
                spspp[7] = (uint8_t)(rate >> 24);
                spspp[8] = (uint8_t)(ch);
                spspp[9] = (uint8_t)((unsigned)ch >> 8);
                spspp[10] = (uint8_t)(bits);
                spspp[11] = (uint8_t)((unsigned)bits >> 8);
                extra = spspp;
                extra_size = 12;
            }
            if (tip.codec == MS_CODEC_HEVC && si < 64u && f->held != NULL) {
                uint32_t psig = 0;
                int hsp = collect_hevc_cfg_from_au(sample_fmt_for(stv, f->held),
                                                   f->held->data, f->held->size,
                                                   hevc_len_size(stv), spspp,
                                                   (int)sizeof(spspp), &psig);
                if (hsp > 0 &&
                    (!f->cfg_sig_valid[si] || f->cfg_sig[si] != psig)) {
                    /* 带内三联体变化（或首次）：用 AU 载荷签名，不再回读包装。 */
                    memset(&ev, 0, sizeof(ev));
                    ev.type = MS_EVENT_CODEC_CONFIG;
                    ev.format = map_format(f->ic);
                    fill_track(f, &ev.track, stv);
                    ev.track.level = MS_RECOG_AU;
                    ev.sample_format = (sample_fmt_for(stv, f->held) == MS_SAMPLE_FMT_HVCC)
                                           ? MS_SAMPLE_FMT_HVCC
                                           : MS_SAMPLE_FMT_ANNEXB;
                    ev.is_keyframe = 1;
                    st = enqueue_event(f, ev, spspp, (size_t)hsp, NULL);
                    if (st != MS_OK) {
                        return st;
                    }
                    f->cfg_emitted_bits |= (1ull << si);
                    f->cfg_sig[si] = psig;
                    f->cfg_sig_valid[si] = 1;
                    return MS_OK;
                }
                if (hsp > 0) {
                    /* 已发出相同参数集：不要再用 codecpar 包装去重发。 */
                    extra = NULL;
                    extra_size = 0;
                }
            }
            if ((tip.codec == MS_CODEC_MPEG2_VIDEO ||
                 tip.codec == MS_CODEC_MPEG4_PART2 ||
                 tip.codec == MS_CODEC_H263 ||
                 tip.codec == MS_CODEC_AV1 ||
                 tip.codec == MS_CODEC_AVS_PLUS ||
                 tip.codec == MS_CODEC_AVS_FAMILY) &&
                si < 64u && f->held != NULL) {
                ms_video_cfg vcfg;
                ms_codec vcodec = (tip.codec == MS_CODEC_AVS_FAMILY)
                                      ? MS_CODEC_AVS_PLUS : tip.codec;
                ms_video_cfg_default(&vcfg);
                if (ms_video_cfg_parse(vcodec, sample_fmt_for(stv, f->held), 0,
                                       f->held->data, (size_t)f->held->size,
                                       &vcfg) == MS_OK &&
                    vcfg.complete &&
                    (!f->cfg_sig_valid[si] || f->cfg_sig[si] != vcfg.payload_sig)) {
                    if (vcfg.csd_size > 0) {
                        memset(&ev, 0, sizeof(ev));
                        ev.type = MS_EVENT_CODEC_CONFIG;
                        ev.format = map_format(f->ic);
                        fill_track(f, &ev.track, stv);
                        ev.track.codec = vcfg.codec;
                        ev.track.level = MS_RECOG_AU;
                        ev.sample_format = sample_fmt_for(stv, f->held);
                        ev.is_keyframe = 1;
                        st = enqueue_event(f, ev, vcfg.csd, vcfg.csd_size, NULL);
                        if (st != MS_OK) {
                            return st;
                        }
                        f->cfg_emitted_bits |= (1ull << si);
                        f->cfg_sig[si] = vcfg.payload_sig;
                        f->cfg_sig_valid[si] = 1;
                        return MS_OK;
                    }
                    /* H.263 等合法无 CSD：只记签名，不捏造 CONFIG 载荷。 */
                    f->cfg_sig[si] = vcfg.payload_sig;
                    f->cfg_sig_valid[si] = 1;
                }
                if (vcfg.complete) {
                    extra = NULL;
                    extra_size = 0;
                }
            }
            if ((tip.codec == MS_CODEC_AC3 || tip.codec == MS_CODEC_MP3 ||
                 tip.codec == MS_CODEC_MP2 || tip.codec == MS_CODEC_OPUS) &&
                si < 64u && f->held != NULL) {
                ms_audio_cfg ac;
                ms_audio_cfg_default(&ac);
                if (ms_audio_cfg_parse(tip.codec, sample_fmt_for(stv, f->held), 0,
                                       f->held->data, (size_t)f->held->size,
                                       &ac) == MS_OK &&
                    ac.complete &&
                    (!f->cfg_sig_valid[si] || f->cfg_sig[si] != ac.payload_sig)) {
                    if (ac.csd_size > 0) {
                        memset(&ev, 0, sizeof(ev));
                        ev.type = MS_EVENT_CODEC_CONFIG;
                        ev.format = map_format(f->ic);
                        fill_track(f, &ev.track, stv);
                        ev.track.level = MS_RECOG_AU;
                        ev.sample_format = sample_fmt_for(stv, f->held);
                        ev.is_keyframe = 1;
                        st = enqueue_event(f, ev, ac.csd, ac.csd_size, NULL);
                        if (st != MS_OK) {
                            return st;
                        }
                        f->cfg_emitted_bits |= (1ull << si);
                        f->cfg_sig[si] = ac.payload_sig;
                        f->cfg_sig_valid[si] = 1;
                        return MS_OK;
                    }
                    /* AC-3 / MP2 / MP3 合法无 CSD：只记签名，不捏造 CONFIG。 */
                    f->cfg_sig[si] = ac.payload_sig;
                    f->cfg_sig_valid[si] = 1;
                }
                if (tip.codec == MS_CODEC_AC3 || tip.codec == MS_CODEC_MP3 ||
                    tip.codec == MS_CODEC_MP2) {
                    extra = NULL;
                    extra_size = 0;
                }
            }
            if (extra != NULL && extra_size > 0 && si < 64u) {
                uint32_t sig = extra_sig(extra, extra_size);
                uint8_t opus_csd[MS_AUDIO_CSD_MAX];
                if (tip.codec == MS_CODEC_OPUS) {
                    ms_audio_cfg ac;
                    ms_audio_cfg_default(&ac);
                    if (ms_audio_cfg_parse(tip.codec, MS_SAMPLE_FMT_OPUS, 1,
                                           extra, (size_t)extra_size,
                                           &ac) == MS_OK && ac.complete &&
                        ac.csd_size > 0 && ac.csd_size <= sizeof(opus_csd)) {
                        memcpy(opus_csd, ac.csd, ac.csd_size);
                        extra = opus_csd;
                        extra_size = (int)ac.csd_size;
                        sig = ac.payload_sig;
                    } else {
                        /* 不是真实 OpusHead/dOps：不下发，不捏造。 */
                        extra = NULL;
                        extra_size = 0;
                    }
                }
                if (tip.codec == MS_CODEC_MPEG2_VIDEO ||
                    tip.codec == MS_CODEC_MPEG4_PART2 ||
                    tip.codec == MS_CODEC_H263 ||
                    tip.codec == MS_CODEC_AV1 ||
                    tip.codec == MS_CODEC_AVS_PLUS ||
                    tip.codec == MS_CODEC_AVS_FAMILY) {
                    ms_video_cfg vcfg;
                    ms_codec vcodec = (tip.codec == MS_CODEC_AVS_FAMILY)
                                          ? MS_CODEC_AVS_PLUS : tip.codec;
                    ms_video_cfg_default(&vcfg);
                    if (ms_video_cfg_parse(vcodec, sample_fmt_for(stv, NULL), 1,
                                           extra, (size_t)extra_size, &vcfg) == MS_OK &&
                        vcfg.complete) {
                        sig = vcfg.payload_sig;
                    } else if (f->cfg_sig_valid[si]) {
                        extra = NULL;
                        extra_size = 0;
                    }
                    if (tip.codec == MS_CODEC_H263) {
                        /* 不把未知/容器 extradata 当作 MediaCodec CSD 下发。 */
                        extra = NULL;
                        extra_size = 0;
                    }
                }
                if (tip.codec == MS_CODEC_HEVC) {
                    ms_hevc_ps_set parsed;
                    memset(&parsed, 0, sizeof(parsed));
                    parsed.struct_size = sizeof(parsed);
                    if (ms_hevc_ps_parse(sample_fmt_for(stv, NULL), 1, extra,
                                         (size_t)extra_size, hevc_len_size(stv),
                                         &parsed) == MS_OK &&
                        parsed.triple_complete) {
                        sig = hevc_ps_sig(&parsed);
                    } else if (f->cfg_sig_valid[si]) {
                        extra = NULL;
                        extra_size = 0;
                    }
                }
                if (extra != NULL && extra_size > 0 &&
                    (f->cfg_need_resend[si] ||
                     !f->cfg_sig_valid[si] || f->cfg_sig[si] != sig)) {
                    memset(&ev, 0, sizeof(ev));
                    ev.type = MS_EVENT_CODEC_CONFIG;
                    ev.format = map_format(f->ic);
                    fill_track(f, &ev.track, stv);
                    ev.track.level = MS_RECOG_AU;
                    ev.sample_format = sample_fmt_for(stv, NULL);
                    ev.is_keyframe = 1;
                    st = enqueue_event(f, ev, extra, (size_t)extra_size, NULL);
                    if (st != MS_OK) {
                        return st;
                    }
                    f->cfg_emitted_bits |= (1ull << si);
                    f->cfg_sig[si] = sig;
                    f->cfg_sig_valid[si] = 1;
                    f->cfg_need_resend[si] = 0;
                    return MS_OK; /* held 仍就绪，下一轮发 AU */
                }
            }
        }
    }
    memset(&ev, 0, sizeof(ev));
    ev.type = MS_EVENT_AU;
    ev.format = map_format(f->ic);
    fill_track(f, &ev.track, stv);
    ev.track.level = MS_RECOG_AU;
    ev.sample_format = sample_fmt_for(stv, f->held);
    ev.is_keyframe = (f->held->flags & AV_PKT_FLAG_KEY) ? 1 : 0;
    if (!ev.is_keyframe && stv->codecpar != NULL &&
        stv->codecpar->codec_id == AV_CODEC_ID_H264) {
        unsigned als = 4u;
        if (ev.sample_format == MS_SAMPLE_FMT_AVCC && stv->codecpar->extradata != NULL &&
            stv->codecpar->extradata_size >= 5) {
            als = (unsigned)((stv->codecpar->extradata[4] & 3u) + 1u);
        }
        ev.is_keyframe = h264_pkt_is_key(ev.sample_format, f->held, als);
    }
    if (!ev.is_keyframe && stv->codecpar != NULL &&
        stv->codecpar->codec_id == AV_CODEC_ID_HEVC) {
        ev.is_keyframe = hevc_pkt_is_key(ev.sample_format, f->held, hevc_len_size(stv));
    }
    if (ev.track.codec == MS_CODEC_AVS_FAMILY || ev.track.codec == MS_CODEC_AVS_PLUS) {
        ev.track.codec = cavs_refine_from(f->held->data, (size_t)f->held->size);
        if (ev.track.codec == MS_CODEC_AVS_PLUS) {
            ev.track.level = MS_RECOG_AU;
        }
    }
    if (!ev.is_keyframe && (ev.track.codec == MS_CODEC_MPEG2_VIDEO ||
                            ev.track.codec == MS_CODEC_MPEG4_PART2 ||
                            ev.track.codec == MS_CODEC_H263 ||
                            ev.track.codec == MS_CODEC_AV1 ||
                            ev.track.codec == MS_CODEC_AVS_PLUS ||
                            ev.track.codec == MS_CODEC_AVS_FAMILY)) {
        ev.is_keyframe = vbit_pkt_is_key(ev.track.codec, ev.sample_format, f->held);
    }
    if (stv->time_base.num > 0 && stv->time_base.den > 0) {
        ev.time_base_num = stv->time_base.num;
        ev.time_base_den = stv->time_base.den;
    }
    st = fill_packet_ts(f, f->held, stv, &ev);
    if (st != MS_OK) {
        av_packet_unref(f->held);
        f->held_ready = 0;
        f->state = FF_ST_FAILED;
        f->last_status = st;
        return st;
    }
    if (ev.track.kind == MS_TRACK_AUDIO) {
        fill_audio_skip(f, f->held, stv);
    }
    st = enqueue_event(f, ev, f->held->data, (size_t)f->held->size, NULL);
    if (st == MS_OK) {
        f->au_emitted = 1;
        av_packet_unref(f->held);
        f->held_ready = 0;
    }
    return st;
}

ms_status ms_ff_pull(ms_ff *f, ms_event *out)
{
    unsigned i;
    ms_status st;
    if (f == NULL || out == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    if (ff_is_canceled(f) || f->state == FF_ST_CANCELED) {
        return MS_ERR_CANCELED;
    }
    if (f->state == FF_ST_FAILED) {
        return f->last_status != MS_OK ? f->last_status : MS_ERR_STATE;
    }
    if (f->state != FF_ST_OPEN) {
        return MS_ERR_STATE;
    }
    for (;;) {
        for (i = 0; i < f->nslots; i++) {
            if (f->slots[i].used && !f->slots[i].handed) {
                f->slots[i].handed = 1;
                *out = f->slots[i].ev;
                return MS_OK;
            }
        }
        if (!f->meta_format_sent ||
            (f->ic != NULL && (f->meta_track_sent < f->ic->nb_streams ||
                               f->meta_cfg_sent < f->ic->nb_streams))) {
            st = (ms_status)produce_meta(f);
            if (st == 1) {
                continue;
            }
            if (st == MS_LIMIT) {
                return MS_LIMIT;
            }
            if (st != 0 && st != MS_OK) {
                return st;
            }
        }
        st = produce_packet(f);
        if (st == MS_OK) {
            continue;
        }
        if (st == MS_AGAIN) {
            continue;
        }
        if (st == MS_EOS) {
            ms_event ev;
            if (f->eof >= 2) {
                return MS_EOS;
            }
            memset(&ev, 0, sizeof(ev));
            ev.type = MS_EVENT_EOS;
            ev.format = map_format(f->ic);
            if (enqueue_event(f, ev, NULL, 0, NULL) == MS_OK) {
                f->eof = 2;
                continue;
            }
            return MS_EOS;
        }
        if (st == MS_LIMIT) {
            return MS_LIMIT;
        }
        return st;
    }
}

ms_status ms_ff_release(ms_ff *f, uint32_t release_id)
{
    unsigned i;
    if (f == NULL || release_id == 0) {
        return MS_ERR_INVALID_ARG;
    }
    for (i = 0; i < f->nslots; i++) {
        if (f->slots[i].used && f->slots[i].id == release_id) {
            clear_slot(f, &f->slots[i]);
            return MS_OK;
        }
    }
    return MS_ERR_STATE;
}

ms_status ms_ff_event_trim(const ms_ff *f, uint32_t release_id, ms_event_trim *out)
{
    unsigned i;
    size_t want;
    ms_event_trim tmp;
    size_t n;
    if (f == NULL || release_id == 0 || out == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    want = out->struct_size;
    if (want < sizeof(out->struct_size)) {
        return MS_ERR_INVALID_ARG;
    }
    for (i = 0; i < f->nslots; i++) {
        if (f->slots[i].used && f->slots[i].id == release_id) {
            memset(&tmp, 0, sizeof(tmp));
            tmp.struct_size = want;
            tmp.skip_samples_start = f->slots[i].skip_samples_start;
            tmp.skip_samples_end = f->slots[i].skip_samples_end;
            n = want;
            if (n > sizeof(tmp)) {
                n = sizeof(tmp);
            }
            memcpy(out, &tmp, n);
            out->struct_size = want;
            return MS_OK;
        }
    }
    return MS_ERR_STATE;
}

ms_status ms_ff_cancel(ms_ff *f)
{
    if (f == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    atomic_store(&f->canceled, 1);
    f->last_status = MS_ERR_CANCELED;
    if (f->state == FF_ST_NEW) {
        f->state = FF_ST_CANCELED;
    }
    return MS_OK;
}

ms_status ms_ff_flush(ms_ff *f)
{
    if (f == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    release_all_slots(f);
    if (f->held != NULL) {
        av_packet_unref(f->held);
    }
    f->held_ready = 0;
    if (f->sub_inband != NULL) {
        ms_sub_doc_clear_cues(f->sub_inband);
    }
    return MS_OK;
}

static int default_seek_stream(const ms_ff *f)
{
    unsigned i;
    int audio = -1;
    if (f == NULL || f->ic == NULL) {
        return -1;
    }
    for (i = 0; i < f->ic->nb_streams; i++) {
        AVStream *st = f->ic->streams[i];
        if (st == NULL || st->codecpar == NULL) {
            continue;
        }
        if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            return (int)i;
        }
        if (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && audio < 0) {
            audio = (int)i;
        }
    }
    return audio;
}


static int ff_name_is(const AVFormatContext *ic, const char *name)
{
    return ic != NULL && ic->iformat != NULL && ic->iformat->name != NULL &&
           strcmp(ic->iformat->name, name) == 0;
}

static int64_t avtb_to_rel90(const ms_ff *f, int64_t avtb)
{
    AVRational tb90 = {1, 90000};
    int64_t t90 = rescale_down(avtb, AV_TIME_BASE_Q, tb90);
    if (f != NULL && t90 >= f->origin_90k) {
        return t90 - f->origin_90k;
    }
    if (f != NULL) {
        return t90 - f->origin_90k; /* 允许负相对值由调用方钳到 start */
    }
    return t90;
}

static int64_t meta_i64(const AVFormatContext *ic, const char *key, int *ok)
{
    AVDictionaryEntry *e;
    *ok = 0;
    if (ic == NULL || ic->metadata == NULL) {
        return 0;
    }
    e = av_dict_get(ic->metadata, key, NULL, 0);
    if (e == NULL || e->value == NULL) {
        return 0;
    }
    *ok = 1;
    return strtoll(e->value, NULL, 10);
}

static void fill_seek_range(const ms_ff *f, ms_ff_seek_range *out)
{
    const AVFormatContext *ic;
    const char *url;
    int ok_s = 0, ok_e = 0, ok_td = 0;
    int64_t start_av = 0, end_av = 0, td_av = 0;
    AVDictionaryEntry *kind_e;
    const char *kind_s = NULL;
    memset(out, 0, sizeof(*out));
    out->struct_size = sizeof(*out);
    out->end_90k = -1;
    out->live_edge_90k = -1;
    out->duration_90k = -1;
    out->kind = MS_FF_SEEK_KIND_UNSEEKABLE;
    if (f == NULL || f->ic == NULL || f->state != FF_ST_OPEN) {
        return;
    }
    ic = f->ic;
    url = ic->url;
    if (url_is_live_unseekable_proto(url) || ff_name_is(ic, "rtsp") ||
        ff_name_is(ic, "rtp") || ff_name_is(ic, "sdp") ||
        ff_name_is(ic, "live_flv")) {
        out->kind = MS_FF_SEEK_KIND_UNSEEKABLE;
        return;
    }
    if ((ic->ctx_flags & AVFMTCTX_UNSEEKABLE) != 0) {
        out->kind = MS_FF_SEEK_KIND_UNSEEKABLE;
        return;
    }
    if (ff_name_is(ic, "dash")) {
        /* FFmpeg 9.1 dash_read_seek 拒绝 is_live；无时长即直播，诚实不可定位。 */
        if (ic->duration == AV_NOPTS_VALUE || ic->duration <= 0) {
            out->kind = MS_FF_SEEK_KIND_UNSEEKABLE;
            return;
        }
        out->seekable = 1;
        out->kind = MS_FF_SEEK_KIND_VOD;
        out->start_90k = 0;
        out->duration_90k = rescale_down(ic->duration, AV_TIME_BASE_Q,
                                         (AVRational){1, 90000});
        out->end_90k = out->duration_90k;
        out->live_edge_90k = out->end_90k;
        out->safety_90k = 0;
        return;
    }
    if (ff_name_is(ic, "hls")) {
        kind_e = av_dict_get(ic->metadata, "msmedia.hls.playlist", NULL, 0);
        kind_s = (kind_e != NULL) ? kind_e->value : NULL;
        start_av = meta_i64(ic, "msmedia.hls.window_start", &ok_s);
        end_av = meta_i64(ic, "msmedia.hls.window_end", &ok_e);
        td_av = meta_i64(ic, "msmedia.hls.target_duration", &ok_td);
        if (!ok_s && ic->start_time != AV_NOPTS_VALUE && ic->start_time > 0) {
            start_av = ic->start_time;
            ok_s = 1;
        }
        if (kind_s != NULL && strcmp(kind_s, "vod") == 0) {
            out->kind = MS_FF_SEEK_KIND_VOD;
        } else if (kind_s != NULL && strcmp(kind_s, "event") == 0) {
            out->kind = MS_FF_SEEK_KIND_EVENT;
        } else if (kind_s != NULL && strcmp(kind_s, "live") == 0) {
            out->kind = MS_FF_SEEK_KIND_SLIDING;
        } else if (ic->duration != AV_NOPTS_VALUE && ic->duration > 0) {
            /* 无元数据：有时长视为 VOD/EVENT 可定位，时长仍只在 finished 时公开为 duration。
             * 未 finished 的 EVENT 有 duration，但本层仍把 duration_90k 报 -1。 */
            out->kind = MS_FF_SEEK_KIND_EVENT; /* 保守：不把可能增长的清单当 VOD 总时长 */
        } else {
            out->kind = MS_FF_SEEK_KIND_SLIDING;
        }
        out->seekable = 1;
        out->start_90k = avtb_to_rel90(f, ok_s ? start_av : 0);
        if (out->start_90k < 0) {
            out->start_90k = 0;
        }
        if (ok_e) {
            out->end_90k = avtb_to_rel90(f, end_av);
        } else if (ic->duration != AV_NOPTS_VALUE && ic->duration > 0) {
            out->end_90k = avtb_to_rel90(f, (ok_s ? start_av : 0) + ic->duration);
        }
        if (out->end_90k < out->start_90k && out->end_90k >= 0) {
            out->end_90k = out->start_90k;
        }
        if (ok_td && td_av > 0) {
            out->safety_90k = avtb_to_rel90(f, td_av) - avtb_to_rel90(f, 0);
            if (out->safety_90k < 0) {
                out->safety_90k = rescale_down(td_av, AV_TIME_BASE_Q,
                                               (AVRational){1, 90000});
            }
        } else if (out->end_90k > out->start_90k) {
            /* 无目标时长时用一个分片的保守估计：不跳到窗口最末端。 */
            out->safety_90k = 0;
        }
        if (out->kind == MS_FF_SEEK_KIND_VOD) {
            if (ic->duration != AV_NOPTS_VALUE && ic->duration > 0) {
                out->duration_90k = rescale_down(ic->duration, AV_TIME_BASE_Q,
                                                 (AVRational){1, 90000});
            }
            out->live_edge_90k = out->end_90k;
            out->safety_90k = 0;
        } else {
            out->duration_90k = -1;
            if (out->end_90k >= 0) {
                int64_t edge = out->end_90k;
                if (out->safety_90k > 0 && edge - out->start_90k > out->safety_90k) {
                    edge -= out->safety_90k;
                } else if (out->safety_90k == 0 && out->end_90k > out->start_90k) {
                    /* 至少不要落在最后一个未完成分片上：若窗口长于 start，留 1us 也可；
                     * 这里在未知 target 时不乱减，由调用 seek 时 FFmpeg 再对齐关键帧。
                     * 有 end 且 window>0 时，减一个极小安全距离不够；保持 end 但文档说明
                     * 需要 metadata。前缀重建后 target 会有。 */
                    edge = out->end_90k;
                }
                out->live_edge_90k = edge;
            }
        }
        return;
    }
    if (ic->pb != NULL && (ic->pb->seekable & AVIO_SEEKABLE_NORMAL) != 0) {
        out->seekable = 1;
        out->kind = MS_FF_SEEK_KIND_VOD;
        out->start_90k = 0;
        if (ic->duration != AV_NOPTS_VALUE && ic->duration > 0) {
            out->duration_90k = rescale_down(ic->duration, AV_TIME_BASE_Q,
                                             (AVRational){1, 90000});
            out->end_90k = out->duration_90k;
            out->live_edge_90k = out->end_90k;
        }
        return;
    }
    out->kind = MS_FF_SEEK_KIND_UNSEEKABLE;
}

int ms_ff_is_seekable(const ms_ff *f)
{
    ms_ff_seek_range r;
    if (f == NULL || f->ic == NULL || f->state != FF_ST_OPEN) {
        return 0;
    }
    fill_seek_range(f, &r);
    return r.seekable != 0;
}

int64_t ms_ff_duration_90k(const ms_ff *f)
{
    ms_ff_seek_range r;
    if (f == NULL || f->ic == NULL || f->state != FF_ST_OPEN) {
        return -1;
    }
    fill_seek_range(f, &r);
    return r.duration_90k;
}

ms_status ms_ff_get_seek_range(const ms_ff *f, ms_ff_seek_range *out)
{
    ms_ff_seek_range tmp;
    size_t n;
    if (f == NULL || out == NULL || out->struct_size < sizeof(size_t)) {
        return MS_ERR_INVALID_ARG;
    }
    if (f->state != FF_ST_OPEN || f->ic == NULL) {
        return MS_ERR_STATE;
    }
    fill_seek_range(f, &tmp);
    n = out->struct_size;
    if (n > sizeof(tmp)) {
        n = sizeof(tmp);
    }
    tmp.struct_size = out->struct_size;
    memcpy(out, &tmp, n);
    return MS_OK;
}

static int64_t clamp_seek_target(const ms_ff_seek_range *r, int64_t time_90k)
{
    if (r->seekable && r->start_90k >= 0 && time_90k < r->start_90k) {
        return r->start_90k;
    }
    if (r->seekable && r->live_edge_90k >= 0 && time_90k > r->live_edge_90k) {
        return r->live_edge_90k;
    }
    /* VOD 已知时长且无 live_edge 时仍钳 end/duration */
    if (r->kind == MS_FF_SEEK_KIND_VOD && r->duration_90k >= 0 &&
        time_90k > r->duration_90k) {
        return r->duration_90k;
    }
    return time_90k;
}

ms_status ms_ff_seek_live_edge(ms_ff *f)
{
    ms_ff_seek_range r;
    if (f == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    if (f->state != FF_ST_OPEN || f->ic == NULL) {
        return MS_ERR_STATE;
    }
    fill_seek_range(f, &r);
    if (!r.seekable) {
        return MS_ERR_NOT_SEEKABLE;
    }
    if (r.live_edge_90k >= 0) {
        return ms_ff_seek(f, r.live_edge_90k);
    }
    if (r.end_90k >= 0) {
        return ms_ff_seek(f, r.end_90k);
    }
    return MS_ERR_NOT_SEEKABLE;
}

ms_status ms_ff_seek(ms_ff *f, int64_t time_90k)
{
    int si;
    int ret;
    int64_t dur;
    int64_t ts;
    int64_t origin_tb = 0;
    int64_t off;
    AVRational tb90 = {1, 90000};
    AVStream *st;
    unsigned i;
    if (f == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    if (time_90k < 0) {
        return MS_ERR_INVALID_ARG;
    }
    if (f->state != FF_ST_OPEN || f->ic == NULL) {
        return MS_ERR_STATE;
    }
    if (!ms_ff_is_seekable(f)) {
        return MS_ERR_NOT_SEEKABLE;
    }
    {
        ms_ff_seek_range rng;
        int64_t want = time_90k;
        fill_seek_range(f, &rng);
        /* 先向前放宽再钳位：避免 10.000 错过 10.023 关键帧。
         * 目标 0 不加放宽，以免短片（如 100ms 内只有片头 I 帧）被 skip_to_key 跳过。 */
        if (want > 0 && want <= INT64_MAX - (int64_t)MS_FF_SEEK_KEY_SLOP_90K) {
            want += (int64_t)MS_FF_SEEK_KEY_SLOP_90K;
        }
        time_90k = clamp_seek_target(&rng, want);
        (void)dur;
    }
    si = default_seek_stream(f);
    ts = 0;
    if (si >= 0 && (unsigned)si < f->ic->nb_streams && f->ic->streams[si] != NULL) {
        st = f->ic->streams[si];
        if (origin_tb_for_stream(f, st, &origin_tb) != 0) {
            origin_tb = 0;
        }
        off = rescale_down(time_90k, tb90, st->time_base);
        if (origin_tb > 0 && off > INT64_MAX - origin_tb) {
            ts = INT64_MAX;
        } else if (origin_tb < 0 && off < INT64_MIN - origin_tb) {
            ts = INT64_MIN;
        } else {
            ts = origin_tb + off;
        }
        ret = av_seek_frame(f->ic, si, ts, AVSEEK_FLAG_BACKWARD);
    } else {
        off = rescale_down(time_90k, tb90, AV_TIME_BASE_Q);
        if (f->origin_90k != 0) {
            int64_t o = rescale_down(f->origin_90k, tb90, AV_TIME_BASE_Q);
            if (o > 0 && off > INT64_MAX - o) {
                off = INT64_MAX;
            } else {
                off += o;
            }
        }
        ret = av_seek_frame(f->ic, -1, off, AVSEEK_FLAG_BACKWARD);
    }
    if (ret < 0) {
        /* 回退到默认流任意时间戳 */
        int64_t abs90;
        if (f->origin_90k > 0 && time_90k > INT64_MAX - f->origin_90k) {
            abs90 = INT64_MAX;
        } else {
            abs90 = f->origin_90k + time_90k;
        }
        ret = av_seek_frame(f->ic, -1, rescale_down(abs90, tb90, AV_TIME_BASE_Q),
                            AVSEEK_FLAG_BACKWARD);
    }
    if (ret < 0) {
        ff_log(f, 0, "seek failed");
        return MS_ERR_UNKNOWN;
    }
    (void)ms_ff_flush(f);
    f->eof = 0;
    f->skip_to_key = 1;
    f->timeline_epoch++;
    if (f->timeline_epoch == 0) {
        f->timeline_epoch = 1;
    }
    for (i = 0; i < 64; i++) {
        f->cfg_sig_valid[i] = 0;
        f->cfg_need_resend[i] = 1;
    }
    return MS_OK;
}

ms_status ms_ff_reset(ms_ff *f)
{
    if (f == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    close_input(f);
    release_all_slots(f);
    atomic_store(&f->canceled, 0);
    atomic_store(&f->timed_out, 0);
    disarm_deadline(f);
    f->last_status = MS_OK;
    f->origin_90k = 0;
    f->origin_from_meta = 0;
    f->au_emitted = 0;
    f->state = FF_ST_NEW;
    f->session_epoch++;
    f->selection_epoch = 1;
    f->timeline_epoch = 1;
    f->user_audio_sel = 0;
    f->user_sub_sel = 0;
    f->defaults_applied = 0;
    memset(f->stream_selected, 0, sizeof(f->stream_selected));
    memset(f->track_uid, 0, sizeof(f->track_uid));
    memset(f->cfg_need_resend, 0, sizeof(f->cfg_need_resend));
    f->pending_audio_si = -1;
    f->committed_audio_si = -1;
    f->pending_sub_si = -1;
    f->committed_sub_si = -1;
    f->pending_video_si = -1;
    f->committed_video_si = -1;
    if (f->sub_inband != NULL) {
        ms_sub_doc_destroy(f->sub_inband);
        f->sub_inband = NULL;
    }
    if (f->sub_ext != NULL) {
        ms_sub_doc_destroy(f->sub_ext);
        f->sub_ext = NULL;
    }
    f->ext_on = 0;
    return MS_OK;
}

ms_status ms_ff_list_tracks(const ms_ff *f, ms_track_info *out, size_t cap, size_t *count)
{
    unsigned i, n, written = 0, total = 0;
    if (f == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    if (f->ic != NULL) {
        ff_apply_default_selection((ms_ff *)f);
        n = (unsigned)f->ic->nb_streams;
        for (i = 0; i < n; i++) {
            ms_track_info ti;
            memset(&ti, 0, sizeof(ti));
            fill_track((ms_ff *)f, &ti, f->ic->streams[i]);
            ff_copy_meta((ms_ff *)f, i, f->ic->streams[i], &ti);
            if (out != NULL && written < cap) {
                out[written++] = ti;
            }
            total++;
        }
    }
    if (f->ext_on) {
        total++;
        if (out != NULL && written < cap) {
            memset(&out[written], 0, sizeof(out[0]));
            out[written].id = MS_TRACK_ID_EXTERNAL_SUB;
            out[written].kind = MS_TRACK_SUBTITLE;
            out[written].codec = ms_sub_codec_for_format(
                f->sub_ext ? ms_sub_doc_format(f->sub_ext) : MS_SUB_FORMAT_AUTO);
            out[written].level = MS_RECOG_AU;
            out[written].name = "external";
            out[written].selected = f->stream_selected[63]; /* 复用最高位表示外挂选中 */
            written++;
        }
    }
    if (count != NULL) {
        *count = total;
    }
    (void)cap;
    return MS_OK;
}

ms_status ms_ff_select_track(ms_ff *f, uint32_t track_id, int on)
{
    int idx, i, n, changed = 0;
    enum AVMediaType typ;
    ms_codec codec;
    if (f == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    if (track_id == MS_TRACK_ID_EXTERNAL_SUB) {
        if (!f->ext_on) {
            return MS_ERR_INVALID_ARG;
        }
        f->selection_epoch++;
        if (f->selection_epoch == 0) {
            f->selection_epoch = 1;
        }
        f->user_sub_sel = 1;
        /* 关闭其它字幕流 */
        if (f->ic != NULL) {
            n = (int)f->ic->nb_streams;
            for (i = 0; i < n && i < 64; i++) {
                if (f->ic->streams[i]->codecpar != NULL &&
                    f->ic->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_SUBTITLE &&
                    f->stream_selected[i]) {
                    f->stream_selected[i] = 0;
                    changed = 1;
                }
            }
        }
        f->stream_selected[63] = on ? 1 : 0;
        if (!on && f->sub_inband != NULL) {
            ms_sub_doc_clear_cues(f->sub_inband);
        }
        ff_drop_kind_slots(f, AVMEDIA_TYPE_SUBTITLE, 0, 1);
        (void)changed;
        return MS_OK;
    }
    idx = ff_find_stream(f, track_id);
    if (idx < 0 || f->ic == NULL || idx >= 64) {
        return MS_ERR_INVALID_ARG;
    }
    typ = f->ic->streams[idx]->codecpar != NULL ?
          f->ic->streams[idx]->codecpar->codec_type : AVMEDIA_TYPE_UNKNOWN;
    codec = map_codec(f->ic->streams[idx]->codecpar != NULL ?
                      f->ic->streams[idx]->codecpar->codec_id : AV_CODEC_ID_NONE);
    if (on && typ == AVMEDIA_TYPE_SUBTITLE && !ff_sub_playable(codec)) {
        return MS_ERR_UNSUPPORTED;
    }
    if (typ == AVMEDIA_TYPE_AUDIO) {
        f->user_audio_sel = 1;
        if (on) {
            if (idx == f->committed_audio_si && f->pending_audio_si < 0) {
                return MS_OK; /* 已在播该轨，不是伪报切换成功 */
            }
            /* 只武装待确认切换：真正读到新轨包后再停止旧输出。失败则撤回 pending。 */
            f->pending_audio_si = idx;
            ff_apply_discard(f);
            return MS_OK;
        }
        if (f->committed_audio_si < 0 && f->pending_audio_si < 0) {
            return MS_OK;
        }
        ff_commit_audio(f, -1, 0);
        return MS_OK;
    }
    if (typ == AVMEDIA_TYPE_SUBTITLE) {
        f->user_sub_sel = 1;
        if (on) {
            if (idx == f->committed_sub_si && f->pending_sub_si < 0) {
                return MS_OK; /* 已在播该字幕，不是伪报切换成功 */
            }
            f->stream_selected[63] = 0;
            {
                ms_format fmt = map_format(f->ic);
                /* HLS/DASH 外链要等新清单第一包再提交；同一容器内可立即提交（缓存已有 cue）。 */
                if (fmt == MS_FORMAT_HLS || fmt == MS_FORMAT_DASH ||
                    fmt == MS_FORMAT_RTSP || fmt == MS_FORMAT_RTP ||
                    fmt == MS_FORMAT_RTMP) {
                    f->pending_sub_si = idx;
                    ff_apply_discard(f);
                } else {
                    ff_commit_sub(f, idx, 1);
                }
            }
            return MS_OK;
        }
        if (f->committed_sub_si < 0 && f->pending_sub_si < 0 &&
            !f->stream_selected[idx]) {
            return MS_OK;
        }
        ff_commit_sub(f, -1, 0);
        return MS_OK;
    }
    if (typ == AVMEDIA_TYPE_VIDEO) {
        if (!on) {
            /* 不允许关掉最后一条视频轨后假装成功。 */
            return MS_ERR_INVALID_ARG;
        }
        if (f->eof) {
            return MS_EOS;
        }
        if (codec == MS_CODEC_UNKNOWN) {
            return MS_ERR_UNSUPPORTED;
        }
        if (f->committed_video_si >= 0 && f->committed_video_si < 64 &&
            f->ic->streams[f->committed_video_si] != NULL &&
            f->ic->streams[f->committed_video_si]->codecpar != NULL) {
            ms_codec cur = map_codec(
                f->ic->streams[f->committed_video_si]->codecpar->codec_id);
            if (cur != MS_CODEC_UNKNOWN && cur != codec) {
                /* 跨 codec 变体切换本切片明确拒绝，不把 UNSUPPORTED 写成切换完成。 */
                return MS_ERR_UNSUPPORTED;
            }
        }
        if (idx == f->committed_video_si && f->pending_video_si < 0) {
            return MS_OK; /* 已在播该变体，不是伪报切换成功 */
        }
        /* 只武装待确认：等到新轨关键帧再提交并重发 CSD。本调用不做网络 I/O。 */
        f->pending_video_si = idx;
        ff_apply_discard(f);
        return MS_OK;
    }
    if ((on && f->stream_selected[idx]) || (!on && !f->stream_selected[idx])) {
        return MS_OK;
    }
    f->selection_epoch++;
    if (f->selection_epoch == 0) {
        f->selection_epoch = 1;
    }
    if (typ == AVMEDIA_TYPE_SUBTITLE) {
        f->user_sub_sel = 1;
        f->stream_selected[63] = 0;
    }
    n = (int)f->ic->nb_streams;
    for (i = 0; i < n && i < 64; i++) {
        int want = 0;
        if (f->ic->streams[i]->codecpar == NULL ||
            f->ic->streams[i]->codecpar->codec_type != typ) {
            continue;
        }
        want = on && i == idx;
        if (f->stream_selected[i] != want) {
            ff_drop_kind_slots(f, typ, ff_track_id(f, f->ic->streams[i]), typ == AVMEDIA_TYPE_SUBTITLE);
            f->stream_selected[i] = want;
        }
    }
    ff_apply_discard(f);
    return MS_OK;
}

ms_status ms_ff_set_external_subtitle(ms_ff *f, const uint8_t *data, size_t size,
                                      ms_sub_format fmt)
{
    ms_status st;
    if (f == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    if (data == NULL || size == 0) {
        if (f->sub_ext != NULL) {
            ms_sub_doc_destroy(f->sub_ext);
            f->sub_ext = NULL;
        }
        f->ext_on = 0;
        f->stream_selected[63] = 0;
        return MS_OK;
    }
    if (f->sub_ext == NULL) {
        st = ms_sub_doc_create(f->alloc, &f->sub_ext);
        if (st != MS_OK) {
            return st;
        }
    }
    st = ms_sub_doc_parse(f->sub_ext, data, size, fmt, MS_TRACK_ID_EXTERNAL_SUB);
    if (st != MS_OK) {
        return st;
    }
    f->ext_on = 1;
    return MS_OK;
}

ms_status ms_ff_subtitles_at(const ms_ff *f, uint64_t time_90k,
                             ms_sub_cue *out, size_t cap, size_t *count)
{
    size_t n = 0, add = 0;
    ms_status st;
    if (f == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    if (f->ext_on && f->stream_selected[63] && f->sub_ext != NULL) {
        st = ms_sub_doc_cues_at(f->sub_ext, time_90k, out, cap, &add);
        if (st != MS_OK) {
            return st;
        }
        n += add;
    }
    if (f->sub_inband != NULL && f->user_sub_sel) {
        ms_sub_cue tmp[32];
        ms_sub_cue *dst = (out != NULL && n < cap) ? out + n : NULL;
        size_t left = (out != NULL && n < cap) ? cap - n : 0;
        uint32_t sid = 0;
        size_t r, w, got = 0;
        for (r = 0; r < 32; r++) {
            ms_sub_cue_default(&tmp[r]);
        }
        st = ms_sub_doc_cues_at(f->sub_inband, time_90k, tmp, 32, &got);
        if (st != MS_OK) {
            return st;
        }
        if (got > 32) {
            got = 32;
        }
        if (f->committed_sub_si >= 0 && f->ic != NULL &&
            f->committed_sub_si < (int)f->ic->nb_streams) {
            sid = ff_track_id(f, f->ic->streams[f->committed_sub_si]);
        }
        w = 0;
        for (r = 0; r < got; r++) {
            if (sid != 0 && tmp[r].track_id != sid) {
                continue;
            }
            if (dst != NULL && w < left) {
                dst[w] = tmp[r];
            }
            w++;
        }
        n += w;
    }
    if (count != NULL) {
        *count = n;
    }
    return MS_OK;
}

/* 从 codecpar / side data 取色度与 mastering。AV1/VP9 仅当 ff 已给出 color 时透传，
 * 不解析其码流，未做独立验证。加法 ABI。
 * by: Long <mycjzb@gmail.com>
 */
static unsigned chroma_to_sei(AVRational q)
{
    double v = av_q2d(q) * 50000.0;
    if (v < 0) {
        v = 0;
    }
    if (v > 65535.0) {
        v = 65535.0;
    }
    return (unsigned)(v + 0.5);
}

static uint32_t lum_to_sei(AVRational q)
{
    double v = av_q2d(q) * 10000.0;
    if (v < 0) {
        v = 0;
    }
    if (v > 4294967295.0) {
        v = 4294967295.0;
    }
    return (uint32_t)(v + 0.5);
}

static const AVPacketSideData *ff_side(const AVPacketSideData *sd, int n, enum AVPacketSideDataType typ)
{
    if (sd == NULL || n <= 0) {
        return NULL;
    }
    return av_packet_side_data_get(sd, n, typ);
}

static void fill_hdr_from_par(const AVCodecParameters *par, ms_hdr_info *out)
{
    const AVPacketSideData *sd;
    unsigned tag;
    if (par == NULL || out == NULL) {
        return;
    }
    if (par->color_primaries != AVCOL_PRI_UNSPECIFIED &&
        par->color_primaries != AVCOL_PRI_RESERVED0) {
        out->color_primaries = (int)par->color_primaries;
        out->have_color = 1;
        out->from_container = 1;
    }
    if (par->color_trc != AVCOL_TRC_UNSPECIFIED &&
        par->color_trc != AVCOL_TRC_RESERVED0) {
        out->color_transfer = (int)par->color_trc;
        out->have_color = 1;
        out->from_container = 1;
    }
    if (par->color_space != AVCOL_SPC_UNSPECIFIED &&
        par->color_space != AVCOL_SPC_RESERVED) {
        out->color_matrix = (int)par->color_space;
        out->have_color = 1;
        out->from_container = 1;
    }
    if (par->color_range == AVCOL_RANGE_MPEG) {
        out->color_range = 1;
    } else if (par->color_range == AVCOL_RANGE_JPEG) {
        out->color_range = 2;
    }
    if (par->bits_per_raw_sample > 0) {
        out->bit_depth = par->bits_per_raw_sample;
    }
    if (par->width > 0) {
        out->width = par->width;
        out->from_container = 1;
    }
    if (par->height > 0) {
        out->height = par->height;
        out->from_container = 1;
    }
    sd = ff_side(par->coded_side_data, par->nb_coded_side_data,
                 AV_PKT_DATA_MASTERING_DISPLAY_METADATA);
    if (sd != NULL && sd->data != NULL && sd->size >= (int)sizeof(AVMasteringDisplayMetadata)) {
        const AVMasteringDisplayMetadata *m = (const AVMasteringDisplayMetadata *)sd->data;
        /* ff 为 R、G、B；本结构与 HEVC SEI 一致，存 G、B、R。 */
        if (m->has_primaries) {
            out->display_primaries_x[0] = (uint16_t)chroma_to_sei(m->display_primaries[1][0]);
            out->display_primaries_y[0] = (uint16_t)chroma_to_sei(m->display_primaries[1][1]);
            out->display_primaries_x[1] = (uint16_t)chroma_to_sei(m->display_primaries[2][0]);
            out->display_primaries_y[1] = (uint16_t)chroma_to_sei(m->display_primaries[2][1]);
            out->display_primaries_x[2] = (uint16_t)chroma_to_sei(m->display_primaries[0][0]);
            out->display_primaries_y[2] = (uint16_t)chroma_to_sei(m->display_primaries[0][1]);
            out->white_point_x = (uint16_t)chroma_to_sei(m->white_point[0]);
            out->white_point_y = (uint16_t)chroma_to_sei(m->white_point[1]);
            out->have_mastering = 1;
            out->from_container = 1;
        }
        if (m->has_luminance) {
            out->max_display_mastering_luminance = lum_to_sei(m->max_luminance);
            out->min_display_mastering_luminance = lum_to_sei(m->min_luminance);
            out->have_mastering = 1;
            out->from_container = 1;
        }
    }
    sd = ff_side(par->coded_side_data, par->nb_coded_side_data,
                 AV_PKT_DATA_CONTENT_LIGHT_LEVEL);
    if (sd != NULL && sd->data != NULL && sd->size >= (int)sizeof(AVContentLightMetadata)) {
        const AVContentLightMetadata *c = (const AVContentLightMetadata *)sd->data;
        out->max_content_light_level = (uint16_t)(c->MaxCLL > 65535u ? 65535u : c->MaxCLL);
        out->max_frame_average_light_level = (uint16_t)(c->MaxFALL > 65535u ? 65535u : c->MaxFALL);
        out->have_cll = 1;
        out->from_container = 1;
    }
    sd = ff_side(par->coded_side_data, par->nb_coded_side_data, AV_PKT_DATA_DOVI_CONF);
    if (sd != NULL && sd->size > 0) {
        out->dolby_vision = 1;
    }
    sd = ff_side(par->coded_side_data, par->nb_coded_side_data, AV_PKT_DATA_DYNAMIC_HDR10_PLUS);
    if (sd != NULL && sd->size > 0) {
        out->have_hdr10_plus = 1; /* 仅识别，不向 MediaCodec 透传动态元数据，未验证 */
    }
    tag = par->codec_tag;
    if (tag == MKTAG('d', 'v', 'h', '1') || tag == MKTAG('d', 'v', 'h', 'e') ||
        tag == MKTAG('d', 'v', 'a', 'v') || tag == MKTAG('d', 'a', 'v', '1') ||
        tag == MKTAG('d', 'v', 'a', '1') || tag == MKTAG('D', 'V', 'H', '1') ||
        tag == MKTAG('D', 'V', 'H', 'E')) {
        out->dolby_vision = 1;
    }
}

ms_status ms_ff_get_hdr_info(const ms_ff *f, ms_hdr_info *out)
{
    int si = -1, i;
    const AVStream *st;
    const AVCodecParameters *par;
    ms_hdr_info extra;
    if (out == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    ms_hdr_info_default(out);
    if (f == NULL || f->ic == NULL) {
        return MS_ERR_STATE;
    }
    if (f->committed_video_si >= 0 && f->committed_video_si < (int)f->ic->nb_streams) {
        si = f->committed_video_si;
    } else {
        for (i = 0; i < (int)f->ic->nb_streams; i++) {
            st = f->ic->streams[i];
            if (st != NULL && st->codecpar != NULL &&
                st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                si = i;
                break;
            }
        }
    }
    if (si < 0 || si >= (int)f->ic->nb_streams) {
        return MS_ERR_STATE;
    }
    st = f->ic->streams[si];
    if (st == NULL || st->codecpar == NULL) {
        return MS_ERR_STATE;
    }
    par = st->codecpar;
    fill_hdr_from_par(par, out);
    /* HEVC 再解析 extradata VUI/SEI，与容器信息合并。 */
    if (par->codec_id == AV_CODEC_ID_HEVC && par->extradata != NULL &&
        par->extradata_size > 0) {
        ms_hdr_info_default(&extra);
        if (ms_hdr_parse_hevc(MS_SAMPLE_FMT_HVCC, 1, par->extradata,
                              (size_t)par->extradata_size, &extra) == MS_OK) {
            ms_hdr_merge(out, &extra);
        }
        if (!out->have_color && !out->from_vui) {
            if (ms_hdr_parse_hevc(MS_SAMPLE_FMT_ANNEXB, 1, par->extradata,
                                  (size_t)par->extradata_size, &extra) == MS_OK) {
                ms_hdr_merge(out, &extra);
            }
        }
    }
    /* AV1/VP9：只采用 codecpar 已给出的 color/side data，不解析码流（未验证）。 */
    ms_hdr_classify(out);
    return MS_OK;
}

/* ---------------- 阶段 6：Media3 风格缓冲水位 / 播放 TLS 策略 / HLS 预下载转发 ----------------
 * by: Long <mycjzb@gmail.com> */
void ms_ff_buffer_config_default(ms_ff_buffer_config *c)
{
    if (c == NULL) {
        return;
    }
    c->min_buffer_ms = 15000;
    c->max_buffer_ms = 30000;
    c->buffer_for_playback_ms = 1500;
    c->buffer_for_playback_after_rebuffer_ms = 3000;
    c->max_buffer_bytes = (int64_t)16 << 20;
}

ms_status ms_ff_set_buffer_config(ms_ff *f, const ms_ff_buffer_config *c)
{
    if (f == NULL || c == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    if (f->state != FF_ST_NEW) {
        return MS_ERR_STATE;
    }
    if (c->max_buffer_ms < 0 || c->min_buffer_ms < 0 || c->buffer_for_playback_ms < 0 ||
        c->buffer_for_playback_after_rebuffer_ms < 0 || c->max_buffer_bytes < 0 ||
        c->max_buffer_bytes > ((int64_t)256 << 20) ||
        (c->max_buffer_ms > 0 && c->min_buffer_ms > c->max_buffer_ms)) {
        return MS_ERR_INVALID_ARG;
    }
    f->bufcfg = *c;
    if (f->bufcfg.max_buffer_bytes == 0) {
        f->bufcfg.max_buffer_bytes = (int64_t)16 << 20;
    }
    return MS_OK;
}

ms_status ms_ff_set_tls_verify(ms_ff *f, int verify)
{
    if (f == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    if (f->state != FF_ST_NEW) {
        return MS_ERR_STATE;
    }
    f->tls_verify = verify ? 1 : 0;
    return MS_OK;
}

ms_status ms_ff_get_buffer_stats(ms_ff *f, ms_ff_buffer_stats *out)
{
    if (f == NULL || out == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
#ifdef MS_HLS_PREFETCH
    ms_hls_pf_get_stats(f->pf, out);
    return MS_OK;
#else
    return MS_ERR_UNSUPPORTED;
#endif
}

#ifdef MS_HLS_PREFETCH
/* 预下载请求补选项：TLS 策略与主连接一致；rw_timeout 用 io 超时，防死连接卡住预拉。 */
static void pf_opt_cb(void *ctx, AVDictionary **opts)
{
    ms_ff *f = (ms_ff *)ctx;
    av_dict_set(opts, "verify", f->tls_verify ? "1" : "0", 0);
    av_dict_set(opts, "tls_verify", f->tls_verify ? "1" : "0", 0);
    if (f->limits.io_timeout_ms > 0) {
        av_dict_set_int(opts, "rw_timeout", (int64_t)f->limits.io_timeout_ms * 1000, 0);
    }
}

static int pf_cancel_cb(void *ctx)
{
    return ff_is_canceled((ms_ff *)ctx);
}

/* hls.c 回调：AVFormatContext.opaque 必须是 ms_ff（魔数校验），否则视为未启用。 */
static ms_ff *ff_from_ic(AVFormatContext *s)
{
    ms_ff *f = s != NULL ? (ms_ff *)s->opaque : NULL;
    return (f != NULL && f->magic == MS_FF_MAGIC && f->ic == s) ? f : NULL;
}

int ms_hls_pf_enabled(AVFormatContext *s)
{
    ms_ff *f = ff_from_ic(s);
    return f != NULL && f->bufcfg.max_buffer_ms > 0 && !ff_is_canceled(f);
}

void ms_hls_pf_plan_begin(AVFormatContext *s, int pls_index, int live, int64_t target_us)
{
    ms_ff *f = ff_from_ic(s);
    if (f == NULL) {
        return;
    }
    if (f->pf == NULL) {
        f->pf = ms_hls_pf_create(&f->bufcfg, s, pf_opt_cb, pf_cancel_cb, f);
    }
    ms_hls_pf_plan_begin_pf(f->pf, pls_index, live, target_us);
}

void ms_hls_pf_plan_add(AVFormatContext *s, int pls_index, const char *url, int64_t off, int64_t end, int64_t dur_us)
{
    ms_ff *f = ff_from_ic(s);
    if (f != NULL) {
        ms_hls_pf_plan_add_pf(f->pf, pls_index, url, off, end, dur_us);
    }
}

void ms_hls_pf_plan_end(AVFormatContext *s, int pls_index, AVDictionary *opts)
{
    ms_ff *f = ff_from_ic(s);
    if (f != NULL) {
        ms_hls_pf_plan_end_pf(f->pf, pls_index, opts);
    }
}
#endif
