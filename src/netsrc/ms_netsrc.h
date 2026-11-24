/*
 * 纯自研网络输入（路线 A 单 so 播放 URL 用；内部接口，不进 C ABI v1）。
 * 一个 URL → 一个 ms_session 的字节流：
 *   - http/https：独立下载线程边下边解；首包按字节探测（不看后缀），剥图片伪装头/脏字节；
 *     内容是 #EXTM3U → HLS（含主清单选路、#EXT-X-MAP + .m4s、伪装后缀切片），<MPD → DASH；
 *     HLS/DASH 下该线程转为独立预加载管理线程：按「提前缓存秒数」前瞻预取后续分片，
 *     总内存上限防 OOM，首片流式边下边解，HTTP Keep-Alive 复用，切台/停止即时取消零泄漏。
 *   - udp://（裸 TS 组播/单播）、rtp://（RTP/MP2T 剥头出 TS）
 *   - rtmp://（握手+connect+play，拆块重组为 FLV 交给已有 FLV 解封装）
 *   - rtsp://（OPTIONS/DESCRIBE/SETUP 交织 TCP/PLAY；MP2T 负载出 TS，H.264/H.265/AAC 负载重组为 FLV）
 * by: Long <mycjzb@gmail.com>
 */
#ifndef MS_NETSRC_H
#define MS_NETSRC_H
#include "msmedia/msmedia.h"
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct ms_netsrc_opts {
    size_t struct_size;
    int preload_ahead_ms;       /* 提前缓存秒数（毫秒）；默认 10000，对标 Media3 */
    size_t preload_max_bytes;   /* 预加载总内存上限；默认 16 MiB（绝对上限 128 MiB） */
    int connect_timeout_ms;     /* 默认 8000 */
    int read_timeout_ms;        /* 默认 15000 */
    size_t pipe_bytes;          /* 渐进/实时流管道容量；默认 2 MiB */
} ms_netsrc_opts;

typedef struct ms_netsrc_stats {
    int mode;                   /* ms_netsrc_mode */
    int probe_fmt;              /* ms_probe_fmt */
    int probe_disguise;
    uint64_t stripped_bytes;    /* 首包剥掉的伪装/脏字节 */
    uint64_t bytes_in;          /* 推进会话的字节 */
    int seg_demand;             /* 播放方按需请求的分片数 */
    int seg_prefetched;         /* 预加载线程提前下载完成的分片数 */
    int seg_hit;                /* 按需请求时已在预加载缓存中（含下载中）的次数 */
    size_t cache_bytes;         /* 当前缓存字节 */
    size_t cache_peak;          /* 峰值缓存字节 */
    uint32_t ahead_ms;          /* 当前已缓存的前瞻时长 */
    int http_opened;            /* Keep-Alive 统计：新建连接 */
    int http_reused;            /* Keep-Alive 统计：复用 */
    uint64_t bad_packets;       /* TS 坏包过滤丢弃数（追加字段） */
} ms_netsrc_stats;

typedef enum ms_netsrc_mode {
    MS_NS_PENDING = 0,
    MS_NS_PROGRESSIVE,
    MS_NS_HLS,
    MS_NS_DASH,
    MS_NS_UDP,
    MS_NS_RTP,
    MS_NS_RTMP,
    MS_NS_RTSP
} ms_netsrc_mode;

typedef struct ms_netsrc ms_netsrc;

void ms_netsrc_opts_default(ms_netsrc_opts *o);
/* 是否为本模块可处理的 URL 方案。 */
int ms_netsrc_url_supported(const char *url);
/* 立即返回（连接在后台线程进行）；session 由调用方拥有，生命周期须长于 netsrc。 */
ms_status ms_netsrc_open(const char *url, const ms_netsrc_opts *o, ms_session *s, ms_netsrc **out);
/* 把已到的字节推进会话：OK=有进展；AGAIN=暂时无数据；LIMIT=会话反压（先 pull）；EOS=输入结束；其它=错误。 */
ms_status ms_netsrc_drive(ms_netsrc *n);
/* 线程安全：任意线程调用，阻塞中的网络读写在 ≤100ms 内返回。 */
void ms_netsrc_cancel(ms_netsrc *n);
/* 取消并回收线程与全部缓存。 */
void ms_netsrc_close(ms_netsrc *n);
void ms_netsrc_get_stats(ms_netsrc *n, ms_netsrc_stats *out);
const char *ms_netsrc_mode_name(int mode);
/* 最近一次错误的简短说明（不含 URL）。 */
const char *ms_netsrc_last_error(ms_netsrc *n);

/* ---- 协议生产者共用的输出接口（udp/rtp/rtmp/rtsp 实现文件使用） ---- */
typedef struct ms_ns_out {
    void *ctx;
    /* 阻塞写入管道（满则等待），返回 0 成功，<0 取消/错误 */
    int (*write)(void *ctx, const uint8_t *d, size_t n);
    volatile int *cancel;
    int connect_timeout_ms;
    int read_timeout_ms;
    char *err;          /* 错误说明缓冲（不含 URL） */
    size_t err_cap;
} ms_ns_out;

int ms_proto_udp_run(const char *url, int is_rtp, ms_ns_out *o);
int ms_proto_rtmp_run(const char *url, ms_ns_out *o);
int ms_proto_rtsp_run(const char *url, ms_ns_out *o);

#ifdef __cplusplus
}
#endif
#endif
