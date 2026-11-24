/*
 * 可选 FFmpeg 9.1 取流/解封装后端（独立库，不链入 libmsmedia 核心）。
 * 映射到公共 ms_event；保留 ms_session API 不变。系统硬解不在本库。
 * 无软件解码/编码/转码。非正式四平台验收。
 * by: Long <mycjzb@gmail.com>
 */
#ifndef MSMEDIA_MS_FF_H
#define MSMEDIA_MS_FF_H

#include "msmedia/msmedia.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ms_ff ms_ff;

typedef struct ms_ff_limits {
    size_t struct_size;
    size_t au_queue_bytes; /* 0=默认 2 MiB；未 release 的 AU/config 载荷上限 */
    size_t out_slots;      /* 0=默认 8 */
    int io_timeout_ms;     /* 0=默认 15000；负数表示不设超时（本地文件可用） */
    int max_redirects;     /* <0 默认 5 */
    /* 0=MS_FF_PROBE_DEFAULT 官方 probesize/analyzeduration；
     * 1=MS_FF_PROBE_FAST_START 有界探测（非 0、不关音轨），缺轨参数也可继续读包。 */
    int probe_mode;
} ms_ff_limits;

typedef void (*ms_ff_log_fn)(void *ctx, int level, const char *msg);

#define MS_FF_PROBE_DEFAULT 0
#define MS_FF_PROBE_FAST_START 1

/* FAST_START 有界值：不是 0，不是关闭音轨。官方默认 analyzeduration 为 5s。 */
#define MS_FF_FAST_ANALYZEDURATION_US 400000
#define MS_FF_FAST_PROBESIZE 524288
#define MS_FF_FAST_MAX_PROBE_PACKETS 48
#define MS_FF_FAST_PROBE_DEADLINE_MS 500

MSMEDIA_API void ms_ff_limits_default(ms_ff_limits *out);

MSMEDIA_API ms_status ms_ff_create(const ms_allocator *alloc, const ms_ff_limits *limits,
                                   ms_ff **out);
MSMEDIA_API void ms_ff_destroy(ms_ff *f);

/* 额外信任的 PEM CA 文件（受控测试临时 CA 或系统导出的 PEM 包）。不是关闭校验，也不能把 verify 设为 0。 */
MSMEDIA_API ms_status ms_ff_set_ca_file(ms_ff *f, const char *path);

/* OpenSSL CApath（hashed 证书目录）。FFmpeg 选项只有 ca_file 时，通过 SSL_CERT_DIR 交给默认校验证书库。不是关闭校验。 */
MSMEDIA_API ms_status ms_ff_set_ca_path(ms_ff *f, const char *dir);

/*
 * 是否允许探测本机系统信任锚（默认 1）。
 * 0：仅使用 set_ca_file / set_ca_path；用于验证「无 CA 则拒绝 TLS」。
 */
MSMEDIA_API ms_status ms_ff_set_allow_system_trust(ms_ff *f, int allow);

/* 当前是否已有可用信任锚（已配置可读 ca_file / ca_path，或允许并探测到系统路径）。 */
MSMEDIA_API int ms_ff_trust_ready(const ms_ff *f);

/* https / tls / rtsps / rtmps 需要信任锚。host 校验由 FFmpeg OpenSSL 后端在 verify=1 时执行。 */
MSMEDIA_API int ms_ff_url_needs_tls_trust(const char *url);

/* 日志回调；msg 不得含完整 URL、查询或用户信息。level：0=error 1=warn 2=info */
MSMEDIA_API void ms_ff_set_log(ms_ff *f, ms_ff_log_fn fn, void *ctx);

/*
 * 打开 URL。支持本构建裁剪启用的协议（file/http/https/rtsp/rtmp/udp/rtp 等），
 * 启用不等于已经联调通过。https 保持证书链与主机名校验；https→http 降级会被拒绝。
 */
/* 须在 open 前调用。DEFAULT 保持稳健默认；FAST_START 仅缩短探测上界。 */
MSMEDIA_API ms_status ms_ff_set_probe_mode(ms_ff *f, int mode);

MSMEDIA_API ms_status ms_ff_open(ms_ff *f, const char *url);

typedef struct ms_ff_open_stats {
    size_t struct_size;
    int64_t t_open_begin_us;        /* CLOCK_MONOTONIC 绝对；未 open 为 -1 */
    int64_t dur_open_input_us;      /* avformat_open_input；未发生 -1 */
    int64_t dur_find_stream_info_us; /* avformat_find_stream_info；未发生 -1 */
    int probe_mode;
    int stream_info_err;
    int64_t probesize_used;         /* FAST 为设定值；DEFAULT 为 -1（官方默认） */
    int64_t analyzeduration_used_us;
    /* 1=find_stream_info 被中断或返回错误，但输入上下文仍可尝试读包。
     * 0=探测按 FFmpeg 返回成功。不得把本标志当成「打开失败」或「已完整识别全部轨道」。 */
    int probe_incomplete;
} ms_ff_open_stats;

MSMEDIA_API ms_status ms_ff_get_open_stats(const ms_ff *f, ms_ff_open_stats *out);

MSMEDIA_API ms_status ms_ff_pull(ms_ff *f, ms_event *out);
MSMEDIA_API ms_status ms_ff_release(ms_ff *f, uint32_t release_id);
MSMEDIA_API ms_status ms_ff_cancel(ms_ff *f);
MSMEDIA_API ms_status ms_ff_flush(ms_ff *f);
MSMEDIA_API ms_status ms_ff_reset(ms_ff *f);

/* 官方裁剪库版本与许可字符串（进程内常量，不要 free）。 */
MSMEDIA_API const char *ms_ff_ffmpeg_version(void);
MSMEDIA_API const char *ms_ff_ffmpeg_license(void);

/* 静态合入的钉选 OpenSSL 版本（进程内常量，不要 free）。 */
MSMEDIA_API const char *ms_ff_openssl_version(void);
MSMEDIA_API unsigned long ms_ff_openssl_version_num(void);

/*
 * avformat_find_stream_info 的返回码（0 成功，负为 AVERROR）。
 * 用于诚实报告「探测是否在无 decoder 时完成」；不得据此偷偷开启软解。
 * 探测中断后若上下文仍可读，open 仍可返回 OK，但 probe_incomplete=1。
 * 尚未 open 时为 1（哨兵）。
 */
MSMEDIA_API int ms_ff_stream_info_err(const ms_ff *f);

/*
 * 会话级统一时间原点（90 kHz）。有符号媒体时间 = origin + 相对 pts/dts 换算。
 * 对所有轨道相同，不按轨截 0。未 open 时为 0。reset 后清零。
 */
MSMEDIA_API int64_t ms_ff_time_origin_90k(const ms_ff *f);

/*
 * 本会话 io_open 路径上实际剥离的 PNG/JPEG TS 前缀次数与最近一次偏移。
 * 用于实证 HLS 内部分片是否走剥离；未剥离时 ops=0，不得据此宣称已剥。
 * kind 为 ms_ts_prefix_kind。尚未 open 时全 0。
 */
MSMEDIA_API void ms_ff_ts_prefix_stats(const ms_ff *f, unsigned *strip_ops,
                                       uint64_t *last_bytes, int *last_kind);

#ifdef __cplusplus
}
#endif

#endif /* MSMEDIA_MS_FF_H */
