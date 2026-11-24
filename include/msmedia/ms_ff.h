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

/*
 * 阶段 6：HLS 缓冲水位（对标 Google Media3 DefaultLoadControl），须在 open 前设置。
 *  - min_buffer_ms：预下载的媒体时长跌破它就恢复预拉（默认 15000）；
 *  - max_buffer_ms：攒够即暂停预拉（默认 30000）；0 = 关闭独立预下载（回到 FFmpeg 原生边下边播）；
 *  - buffer_for_playback_ms：起播门槛（默认 1500）：首片边下边播，攒够这么多（或首片下完）即放行送帧；
 *  - buffer_for_playback_after_rebuffer_ms：重缓冲恢复门槛（默认 3000），防一卡一顿；
 *  - max_buffer_bytes：预下载内存上限（默认 16 MiB，防低内存电视盒子 OOM；绝对上限 256 MiB）。
 * 直播：只预下载当前 m3u8 已列出的切片，水位自然被直播窗口限制。by: Long <mycjzb@gmail.com>
 */
typedef struct ms_ff_buffer_config {
    int min_buffer_ms;
    int max_buffer_ms;
    int buffer_for_playback_ms;
    int buffer_for_playback_after_rebuffer_ms;
    int64_t max_buffer_bytes;
} ms_ff_buffer_config;

typedef struct ms_ff_buffer_stats {
    int64_t buffered_ms;       /* 当前已下载未消费的媒体时长（各 playlist 最小值） */
    int64_t buffered_bytes;    /* 当前预下载占用内存 */
    int64_t peak_bytes;        /* 峰值内存 */
    int64_t bytes_downloaded;  /* 预下载累计字节 */
    int units_planned;         /* 计划过的请求单元数（去重） */
    int units_done;            /* 下载完成的单元数 */
    int cache_hits;            /* 切片请求命中预下载 */
    int cache_misses;          /* 未命中（交回 FFmpeg 直连） */
    int rebuffers;             /* 起播后因数据不足等待的次数 */
    int retries;               /* 瞬时网络错误退避重试次数 */
    int conn_opened;           /* 新建连接数 */
    int conn_reused;           /* Keep-Alive 复用次数 */
    int threads;               /* 预下载线程数（每个 playlist 一个） */
} ms_ff_buffer_stats;

MSMEDIA_API void ms_ff_buffer_config_default(ms_ff_buffer_config *c);
MSMEDIA_API ms_status ms_ff_set_buffer_config(ms_ff *f, const ms_ff_buffer_config *c);
MSMEDIA_API ms_status ms_ff_get_buffer_stats(ms_ff *f, ms_ff_buffer_stats *out);

/*
 * 播放器拉流 TLS 校验策略（用户定调：只有播放忽略，业务 API 不忽略）。
 * 单 so（OpenSSL 后端）默认 0 = 忽略证书链与主机名（兼容自签 / IP 直连 / 过期根证书）；1 = 严格。
 * 与 com.mycjzb.media.player.MsHttpClient 无关（它永远严格）。须在 open 前设置。
 */
MSMEDIA_API ms_status ms_ff_set_tls_verify(ms_ff *f, int verify);

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

/* 在 release 之前查询该 AU 的 PCM 修剪；见 ms_event_trim。 */
MSMEDIA_API ms_status ms_ff_event_trim(const ms_ff *f, uint32_t release_id,
                                       ms_event_trim *out);

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

/* P4：轨道枚举/选择与外挂文本字幕。不打开 FFmpeg 解码器。 */
MSMEDIA_API ms_status ms_ff_list_tracks(const ms_ff *f, ms_track_info *out,
                                        size_t cap, size_t *count);
/* 音轨/HLS 外链字幕/HLS 视频变体：OK 表示请求已接受，不是已完成切换。
 * 音/字幕在读到该轨包后提交；视频变体等到新轨关键帧才提交（重发 CSD、
 * 不复用旧尺寸/参数、抬 selection_epoch 不抬 timeline_epoch）。
 * 跨 codec 视频变体本切片返回 UNSUPPORTED，不写成切换完成。
 * 不允许关闭最后一条视频轨。失败/EOF 撤回 pending，不拆会话、不把新轨标为已选。
 * 本调用不做网络 I/O。无效 id 立即返回 INVALID_ARG 且不改选择。
 * 不做自动 ABR：只按调用方手动选择。 */
MSMEDIA_API ms_status ms_ff_select_track(ms_ff *f, uint32_t track_id, int on);
MSMEDIA_API ms_status ms_ff_set_external_subtitle(ms_ff *f, const uint8_t *data,
                                                  size_t size, ms_sub_format fmt);
MSMEDIA_API ms_status ms_ff_subtitles_at(const ms_ff *f, uint64_t time_90k,
                                         ms_sub_cue *out, size_t cap, size_t *count);
/*
 * 定位到 90 kHz 媒体时间（与事件 PTS 同轴，相对会话 origin）。
 * 使用 av_seek_frame（关键帧向前），不启用 FFmpeg 解码器。
 * 不可定位（UDP/RTP/RTMP 等，或源未给出窗口）返回 NOT_SEEKABLE。
 * 负值为 INVALID_ARG。目标落在窗口之前或越过 live_edge 时钳位后定位，返回 OK。
 * 成功则 flush 槽位/在途包/内嵌 cue，eof 清除，timeline_epoch 增加。
 * 不改用户音轨/字幕/视频变体选择。
 */
MSMEDIA_API ms_status ms_ff_seek(ms_ff *f, int64_t time_90k);
/* 格式时长（90 kHz）；未知或直播（EVENT/滑动窗口）为 -1。
 * 不得把时移窗口长度当成 VOD 总时长。窗口请用 ms_ff_get_seek_range。 */
MSMEDIA_API int64_t ms_ff_duration_90k(const ms_ff *f);
/* 1=可定位（含 HLS EVENT/滑动窗口内定位），0=不可。未 open 为 0。
 * UDP/RTP/RTMP/RTSP 与 FFmpeg 不能定位的 DASH 直播为 0。 */
MSMEDIA_API int ms_ff_is_seekable(const ms_ff *f);

#define MS_FF_SEEK_KIND_UNKNOWN 0
#define MS_FF_SEEK_KIND_VOD 1
#define MS_FF_SEEK_KIND_EVENT 2
#define MS_FF_SEEK_KIND_SLIDING 3
#define MS_FF_SEEK_KIND_UNSEEKABLE 4

/* 可定位范围（90 kHz，相对会话 origin，与事件 PTS 同轴）。加法结构。
 * start/end 为当前窗口；live_edge = end - 一个分片安全距离（不跳未完成末片）。
 * 窗口滑动后须重新查询。过期（<start）与越过 live_edge 的 seek 钳位并返回 OK。 */
typedef struct ms_ff_seek_range {
    size_t struct_size;
    int seekable;
    int kind;
    int64_t start_90k;
    int64_t end_90k;       /* 未知为 -1 */
    int64_t live_edge_90k;
    int64_t safety_90k;
    int64_t duration_90k;  /* 仅 VOD；直播 -1 */
} ms_ff_seek_range;

MSMEDIA_API ms_status ms_ff_get_seek_range(const ms_ff *f, ms_ff_seek_range *out);
/* 定位到当前 live_edge（VOD 则为 end）。不可定位返回 NOT_SEEKABLE。 */
MSMEDIA_API ms_status ms_ff_seek_live_edge(ms_ff *f);

/* 从当前已选（或第一条）视频轨的 codecpar / side data 取色度与 mastering/CLL。
 * MP4/MKV 容器信息也走本接口。AV1/VP9 若 ff 已给出 color 则一并填入，
 * 本实现不解析其码流 VUI，未做独立验证。加法 ABI。 */
MSMEDIA_API ms_status ms_ff_get_hdr_info(const ms_ff *f, ms_hdr_info *out);

#ifdef __cplusplus
}
#endif

#endif /* MSMEDIA_MS_FF_H */
