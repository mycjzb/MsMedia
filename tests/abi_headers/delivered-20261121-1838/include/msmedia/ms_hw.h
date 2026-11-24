/*
 * 可选平台硬解会话（独立 libmshw，不链入 libmsmedia 核心）。
 * 本里程碑：最小完整 H.264 视频路径。后续 codec 可在同一接口扩展，
 * 本构建对非 H.264 返回 UNSUPPORTED，不默默软解。
 *
 * 统一 C 接口。Android：API 21+ 走 NDK AMediaCodec；API 16–20 走
 * JNI android.media.MediaCodec 薄桥（不反射，缓存 jclass/jmethodID）。
 * 不以 createDecoderByType 成功声称硬解。软件组件（如 OMX.google /
 * c2.android）默认拒绝。宿主 mock 只测状态机，不是设备硬解。
 *
 * 借鉴公开的 Media3 MediaCodec 选择与 Surface 输出思路，不是 Media3
 * 渲染器，也不宣称行为等同 Media3。
 *
 * by: Long <mycjzb@gmail.com>
 */
#ifndef MSMEDIA_MS_HW_H
#define MSMEDIA_MS_HW_H

#include "msmedia/msmedia.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MSMEDIA_HW_ABI 1u

typedef struct ms_hw_decoder ms_hw_decoder;

typedef enum ms_hw_kind {
    MS_HW_KIND_NONE = 0,
    MS_HW_KIND_HARDWARE,
    MS_HW_KIND_SOFTWARE,
    MS_HW_KIND_SYSTEM_UNVERIFIED
} ms_hw_kind;

/*
 * 测试专用 mock 行为（仅宿主；不得当成设备核实）。
 * 0：默认，query 如实返回非设备硬解。
 * 1：模拟「已选到硬件组件」以测生命周期。
 * 2：模拟「只有软件组件」以测拒绝。
 */
#define MS_HW_MOCK_DEFAULT 0
#define MS_HW_MOCK_AS_HW   1
#define MS_HW_MOCK_AS_SW   2
/* 宿主：非系统软件组件。即使用户允许系统 AAC，也应拒绝。设备构建忽略。 */
#define MS_HW_MOCK_AS_FOREIGN_SW 3

/* 仅 AAC。不得当作视频软解开关，也不得悄悄改写 allow_software。 */
typedef enum ms_hw_aac_policy {
    MS_HW_AAC_POLICY_HARDWARE_ONLY = 0,
    MS_HW_AAC_POLICY_ALLOW_SYSTEM = 1
} ms_hw_aac_policy;

typedef struct ms_hw_caps {
    size_t struct_size;
    ms_codec codec;
    ms_hw_kind kind;
    int is_hardware;     /* 仅 kind==HARDWARE 且软解回落已关闭时为 1 */
    int verified;        /* 1=已实际分类；0=未核实，不得报硬解成功 */
    int api_level;       /* 宿主 mock 为 0 */
    const char *backend; /* "none" / "mock" / "amediacodec" / "jni-mediacodec" */
    const char *codec_name;
    const char *detail;
} ms_hw_caps;

typedef struct ms_hw_limits {
    size_t struct_size;
    unsigned max_pending_in; /* 0=默认 8，硬上限 */
    size_t max_in_bytes;     /* 0=默认 1 MiB，在途压缩载荷硬上限 */
    unsigned max_events;     /* 0=默认 8 */
} ms_hw_limits;

typedef struct ms_hw_create_opts {
    size_t struct_size;
    int mock_mode;       /* 见 MS_HW_MOCK_*；设备构建忽略 */
    int allow_software;  /* 默认 0；1 仍不把软件报成 is_hardware */
    int api_level;       /* 0=运行时探测；分类启发式需要 */
} ms_hw_create_opts;

typedef struct ms_hw_config {
    size_t struct_size;
    ms_codec codec;              /* 本里程碑仅 MS_CODEC_H264 */
    ms_sample_format sample_fmt; /* AVCC 或 ANNEXB */
    const uint8_t *csd;          /* avcC，或含 SPS/PPS 的 Annex B */
    size_t csd_size;
    int width;                   /* 0=未知，等待 FORMAT_CHANGED */
    int height;
    int64_t time_base_num;       /* 0=默认 1 */
    int64_t time_base_den;       /* 0=默认 1000000（与 MediaCodec 微秒一致） */
    void *surface;               /* 设备上为 ANativeWindow* 或 Surface；mock 任意非 NULL 令牌 */
} ms_hw_config;

typedef struct ms_hw_buf {
    const uint8_t *data;
    size_t size;
    void (*release_fn)(void *ctx, uint8_t *data, size_t size);
    void *release_ctx;
} ms_hw_buf;

#define MS_HW_FLAG_KEY          1u
#define MS_HW_FLAG_CODEC_CONFIG 2u
#define MS_HW_FLAG_EOS          4u

typedef enum ms_hw_event_type {
    MS_HW_EVENT_OUTPUT_RENDERED = 1, /* 已交 Surface，无 CPU 像素 */
    MS_HW_EVENT_FORMAT_CHANGED,
    MS_HW_EVENT_EOS,
    MS_HW_EVENT_ERROR
} ms_hw_event_type;

typedef struct ms_hw_event {
    size_t struct_size;
    ms_hw_event_type type;
    int64_t pts;        /* 与 configure 时基相同；无则 0 */
    int has_pts;
    int width;
    int height;
    ms_status error;    /* EVENT_ERROR 时 */
    const char *detail; /* 短说明，不含 URL */
} ms_hw_event;

typedef struct ms_hw_stats {
    size_t struct_size;
    uint64_t submit_ops;
    uint64_t submit_bytes;
    uint64_t ref_hold_ops;       /* 以引用持有、未先拷到中间帧缓冲 */
    uint64_t platform_copy_ops;  /* 平台压缩输入必要 copy（如入 MediaCodec input） */
    uint64_t platform_copy_bytes;
    uint64_t staging_copy_ops;   /* 本库额外中间拷贝；目标为 0 */
    uint64_t staging_copy_bytes;
    uint64_t rendered_frames;
    uint64_t pin_peak_bytes;     /* 同时持有的压缩载荷峰值 */
    unsigned pending_in;
    size_t pending_in_bytes;
    unsigned events_queued;
    /* MediaCodec OnFrameRendered（API 33+ NDK 回调）。仍不是上屏证明，恒与 display 分列。 */
    int64_t t_first_codec_rendered_us; /* CLOCK_MONOTONIC 绝对 µs；未发生为 -1 */
    int have_first_codec_rendered;
    int codec_rendered_is_not_display; /* 恒为 1 */
} ms_hw_stats;

typedef void (*ms_hw_log_fn)(void *ctx, int level, const char *msg);

MSMEDIA_API void ms_hw_limits_default(ms_hw_limits *out);

/* 设备或宿主上的默认查询。宿主固定非硬解、verified=1、backend=mock/none。 */
MSMEDIA_API ms_status ms_hw_decoder_query(ms_codec codec, ms_hw_caps *out);

/*
 * 按组件名与可选平台标志分类。API>=29 且 hw_flag/sw_flag 已知（0/1，
 * -1=未知）时优先用标志；否则用与 Media3 MediaCodecUtil 同类的名称启发式。
 * 未知前缀且非 omx./c2. 视为 SOFTWARE，不得报硬解。
 */
MSMEDIA_API ms_hw_kind ms_hw_classify_codec_name(const char *name, int api_level,
                                                 int hw_flag, int sw_flag);

MSMEDIA_API ms_status ms_hw_decoder_create(const ms_allocator *alloc,
                                           const ms_hw_limits *limits,
                                           const ms_hw_create_opts *opts,
                                           ms_hw_decoder **out);
MSMEDIA_API void ms_hw_decoder_destroy(ms_hw_decoder *d);

MSMEDIA_API void ms_hw_decoder_set_log(ms_hw_decoder *d, ms_hw_log_fn fn, void *ctx);

/* 成功后可用 get_caps。无硬件且 allow_software=0 时返回 UNSUPPORTED。 */
MSMEDIA_API ms_status ms_hw_decoder_configure(ms_hw_decoder *d,
                                              const ms_hw_config *cfg);
MSMEDIA_API ms_status ms_hw_decoder_start(ms_hw_decoder *d);

/*
 * 提交一个完整压缩 AU（或 CODEC_CONFIG）。不复制整帧到自有中间缓冲：
 * 默认持有引用直至驱动层写入平台输入或 mock 消费，然后调用 release_fn。
 * 队列或字节硬上限用尽返回 LIMIT（反压）。
 */
MSMEDIA_API ms_status ms_hw_decoder_submit(ms_hw_decoder *d, const ms_hw_buf *buf,
                                           int64_t pts, unsigned flags);

MSMEDIA_API ms_status ms_hw_decoder_signal_eos(ms_hw_decoder *d);
/* seek：丢弃在途输入/事件，调用所有未完成 release_fn，平台 codec flush。 */
MSMEDIA_API ms_status ms_hw_decoder_flush(ms_hw_decoder *d);
/* 推进输出（Surface render / 格式变化 / EOS）。不把像素读回 CPU。 */
MSMEDIA_API ms_status ms_hw_decoder_drive(ms_hw_decoder *d);
MSMEDIA_API ms_status ms_hw_decoder_pull_event(ms_hw_decoder *d, ms_hw_event *out);

MSMEDIA_API ms_status ms_hw_decoder_get_caps(const ms_hw_decoder *d, ms_hw_caps *out);
MSMEDIA_API ms_status ms_hw_decoder_get_stats(const ms_hw_decoder *d, ms_hw_stats *out);
MSMEDIA_API const char *ms_hw_kind_name(ms_hw_kind kind);

/*
 * 编排：把公共压缩 AU / CODEC_CONFIG 送到硬解并出 Surface。
 * 借鉴公开的 Media3 起播门闩思路，不是 Media3 渲染器。
 * OUTPUT_RENDERED / releaseOutputBuffer 只表示已交 Surface，不是实际显示。
 * 宿主 mock 只测状态机，不得报硬解或出画。
 */
typedef struct ms_hw_player ms_hw_player;

typedef struct ms_hw_player_opts {
    size_t struct_size;
    ms_hw_create_opts decoder; /* 含 mock_mode；设备构建忽略 mock */
    ms_hw_limits limits;
    int wait_keyframe; /* -1=默认 1；start/flush/disc 后等到视频 IDR */
    /* 1=尝试 AAC 输出（默认）；0=仅视频。无硬件 AAC 时诊断拒绝并视频回退，不暗开软解。 */
    int enable_audio;
    /*
     * 加法字段（保持 ABI：只追加在末尾）。
     * allow_system_aac：默认 0=硬解 only（与既有行为一致）。
     * 1=仅允许选用 Android 系统 MediaCodec 做 AAC（常被分类为软件，如 OMX.google /
     * c2.android）+ AudioTrack，对齐 Media3 常用音频路径。不是 allow_software，
     * 视频软解仍拒绝，禁止 FFmpeg 软解。统计：audio_is_hardware 仅在真正硬件时为 1；
     * 系统软解须 audio_system_decode_is_not_hardware=1，并写明「系统 AAC，非硬件」。
     * audio_mock_mode：-1=跟随 decoder.mock_mode；否则仅作用于音频会话。设备构建忽略。
     */
    int allow_system_aac;
    int audio_mock_mode;
} ms_hw_player_opts;

typedef struct ms_hw_player_stats {
    size_t struct_size;
    int64_t t_start_us;             /* 管线局部原点：create 或最近一次 flush 的 CLOCK_MONOTONIC 绝对 µs。不是用户点播起点。 */
    int64_t t_first_packet_us;      /* 相对 t_start 的管线局部计时；未发生为 -1。不含 mark_request 之前与 open/probe 若发生在 t_start 之后才计入。不得称为直播端到端延迟。 */
    int64_t t_first_decodable_au_us;
    int64_t t_first_submit_us;
    int64_t t_first_output_us;      /* 相对 t_start：交 Surface / mock 输出，不是显示 */
    int64_t t_first_display_us;     /* 相对 t_start：仅 note_display；否则 -1 */
    int64_t t_first_codec_rendered_us; /* 相对 t_start：OnFrameRendered，不是显示 */
    int have_first_packet;
    int have_first_decodable_au;
    int have_first_submit;
    int have_first_output;
    int have_first_display;
    int have_first_codec_rendered;
    int codec_rendered_is_not_display; /* 恒为 1 */
    const char *display_evidence; /* "none" 或 "app_note_display"；不是 choreographer */
    uint64_t cfg_accepted;
    uint64_t video_au_in;
    uint64_t video_au_dropped_nokey;
    uint64_t video_au_dropped_preroll;
    uint64_t video_au_submitted;
    uint64_t video_au_backpressure; /* LIMIT 次数，事件未消费，不计入 in */
    uint64_t ref_hold_ops;
    uint64_t platform_copy_ops;
    uint64_t platform_copy_bytes;
    uint64_t staging_copy_ops;
    uint64_t staging_copy_bytes;
    unsigned pending_in;
    int waiting_keyframe;
    int configured;
    int started;
    int eos_in;
    int eos_out;
    int canceled;
    int flushed;
    unsigned epoch;
    ms_hw_kind kind;
    int is_hardware;                /* 宿主 mock 恒为 0 */
    int output_is_not_display;      /* 恒为 1 */
    const char *backend;
    ms_status last_error;
    /* 以下为请求原点计时（additive，ABI 仍为 1）。相对 t_request_us。
     * 须在 open/probe/拉流之前调用 mark_request。包含建连、探流与等待关键帧。
     * 不是实际显示；display 仍仅 note_display。 */
    int64_t t_request_us; /* CLOCK_MONOTONIC 绝对；未 mark 为 -1 */
    int have_request;
    int64_t t_req_first_packet_us;
    int64_t t_req_first_decodable_au_us;
    int64_t t_req_first_submit_us;
    int64_t t_req_first_output_us;
    int64_t t_req_first_display_us;
    int64_t t_req_first_codec_rendered_us;
    int have_req_first_packet;
    int have_req_first_decodable_au;
    int have_req_first_submit;
    int have_req_first_output;
    int have_req_first_display;
    int have_req_first_codec_rendered;
    /* 请求拆分（additive）。均相对 t_request_us；未发生为 -1。 */
    int64_t t_worker_start_us;           /* 绝对；未 mark 为 -1 */
    int have_worker_start;
    int64_t t_req_worker_start_us;       /* 排队/旧会话取消之后，worker 开始本任务 */
    int64_t t_req_open_input_done_us;    /* open_input 结束（含建连/等首包若阻塞在 open） */
    int64_t t_req_find_stream_info_done_us;
    int64_t dur_open_input_us;
    int64_t dur_find_stream_info_us;
    int have_open_phases;
    /* 以下为音频/同步（additive）。宿主 mock 不是设备出声；display 仍仅 note_display。
     * audio_is_hardware 仅在 kind==HARDWARE 时为 1；mock 与系统软解恒为 0。 */
    ms_status audio_status;
    int audio_unsupported; /* 1=已诊断拒绝（仅软件 AAC 等），视频继续 */
    int have_audio;
    int audio_configured;
    int audio_is_hardware;
    int audio_system_decode_is_not_hardware;
    ms_hw_kind audio_kind;
    const char *audio_backend;
    const char *audio_detail;
    uint64_t audio_au_in;
    uint64_t audio_au_submitted;
    uint64_t audio_au_played;
    uint64_t audio_au_dropped_unsupported;
    uint64_t audio_backpressure;
    uint64_t audio_partial_writes;
    uint64_t audio_samples_played;
    int64_t audio_clock_us; /* 来自已播放帧，不是已排队字节 */
    int audio_sample_rate;
    int audio_channels;
    int audio_pcm_format_changes;
    uint64_t video_au_late;
    uint64_t video_au_held;
    uint64_t video_au_dropped_late;
    int sync_has_audio_clock;
    int sync_fallback_no_audio;
    /* 中途 AU 内 SPS/PPS（无独立 CODEC_CONFIG） */
    uint64_t video_csd_inband;            /* 从 AU 应用了新的成对配置 */
    uint64_t video_csd_inband_same;       /* AU 内参数集与已应用相同，不重建 */
    uint64_t video_csd_pending_abandoned; /* 有界等待仍不成对后放弃 */
} ms_hw_player_stats;

MSMEDIA_API void ms_hw_player_opts_default(ms_hw_player_opts *out);
MSMEDIA_API ms_status ms_hw_player_create(const ms_allocator *alloc,
                                          const ms_hw_player_opts *opts,
                                          ms_hw_player **out);
MSMEDIA_API void ms_hw_player_destroy(ms_hw_player *p);
MSMEDIA_API void ms_hw_player_set_log(ms_hw_player *p, ms_hw_log_fn fn, void *ctx);
MSMEDIA_API ms_status ms_hw_player_set_surface(ms_hw_player *p, void *surface);
/*
 * 标记一次播放请求的 CLOCK_MONOTONIC 原点。必须在 open/probe/读文件/拉流之前调用。
 * 不重置 t_start 与 t_first_* 管线局部计时。flush 重置局部计时，不改请求原点。
 * 再次调用开始新的请求窗口并清零 t_req_*。新请求同时清除 canceled，以便取消后重新开播。
 */
MSMEDIA_API ms_status ms_hw_player_mark_request(ms_hw_player *p);
/* 工作线程取出排队任务、即将 open 之前。不改请求原点。用于拆分 enqueue vs worker start。 */
MSMEDIA_API ms_status ms_hw_player_mark_worker_start(ms_hw_player *p);
/* 记录 ms_ff_open 内 open_input / find_stream_info 耗时，并换算为相对请求原点的完成时刻。 */
MSMEDIA_API ms_status ms_hw_player_note_open_phases(ms_hw_player *p,
                                                    int64_t dur_open_input_us,
                                                    int64_t dur_find_stream_info_us);

/*
 * 喂入一条公共事件。返回 OK 表示本事件已被消费：若提供 release_fn，
 * 由播放器在提交完成或丢弃后调用；返回 LIMIT/AGAIN/错误时所有权仍在调用方。
 * origin_90k 与 ms_ff_time_origin_90k 相同，没有则为 0。
 */
MSMEDIA_API ms_status ms_hw_player_feed(ms_hw_player *p, const ms_event *ev,
                                        int64_t origin_90k,
                                        void (*release_fn)(void *ctx, uint8_t *data,
                                                           size_t size),
                                        void *release_ctx);
MSMEDIA_API ms_status ms_hw_player_drive(ms_hw_player *p);
MSMEDIA_API ms_status ms_hw_player_pull_event(ms_hw_player *p, ms_hw_event *out);
MSMEDIA_API ms_status ms_hw_player_signal_eos(ms_hw_player *p);
MSMEDIA_API ms_status ms_hw_player_flush(ms_hw_player *p);
MSMEDIA_API ms_status ms_hw_player_cancel(ms_hw_player *p);
/* 释放解码器与在途引用，保留 CSD/轨道；用于切换或 Surface 销毁后再 set_surface。 */
MSMEDIA_API ms_status ms_hw_player_release_decoder(ms_hw_player *p);
MSMEDIA_API ms_status ms_hw_player_get_stats(const ms_hw_player *p,
                                             ms_hw_player_stats *out);
MSMEDIA_API ms_status ms_hw_player_get_caps(const ms_hw_player *p, ms_hw_caps *out);
/* 仅应用在其认定的真实上屏时调用。编解码、releaseOutputBuffer、
 * 单次 Choreographer 回调、OnFrameRendered 均不得调用本函数。 */
MSMEDIA_API ms_status ms_hw_player_note_display(ms_hw_player *p, int64_t pts,
                                                int has_pts);

/*
 * 可选 AAC 音频输出（独立于视频硬解会话，由播放器编排）。
 * 默认硬解 only：系统软件 AAC（OMX.google / c2.android 等）返回 UNSUPPORTED，
 * 不得把软件解码报成 is_hardware，也不得悄悄把 allow_software 变成全面软解。
 * 显式 ms_hw_audio_set_aac_policy(ALLOW_SYSTEM) 后，可选用 Android 系统
 * MediaCodec（可被分类为软件）+ AudioTrack；HE-AAC 仍 UNSUPPORTED。
 * 宿主 mock 只测配置/PCM/时钟/反压/部分写入/EOS/flush/取消，不解码 AAC 载荷，
 * 不是设备出声。时钟只按已播放帧累计，不用排队字节。
 */
typedef struct ms_hw_audio ms_hw_audio;

typedef struct ms_hw_audio_caps {
    size_t struct_size;
    ms_codec codec;
    ms_hw_kind kind;
    int is_hardware; /* 仅 kind==HARDWARE 时为 1 */
    int verified;
    int api_level;
    int system_decode_is_not_hardware; /* SOFTWARE / SYSTEM_UNVERIFIED 为 1 */
    const char *backend; /* "none" / "mock" / "mediacodec-audiotrack" */
    const char *codec_name;
    const char *detail;
} ms_hw_audio_caps;

typedef struct ms_hw_audio_config {
    size_t struct_size;
    ms_codec codec;              /* 仅 MS_CODEC_AAC */
    ms_sample_format sample_fmt; /* ADTS 或 AAC_RAW */
    const uint8_t *asc;
    size_t asc_size;
    int sample_rate; /* 0=从 ASC 读取 */
    int channels;    /* 0=从 ASC 读取 */
} ms_hw_audio_config;

typedef struct ms_hw_audio_stats {
    size_t struct_size;
    uint64_t au_in;
    uint64_t au_submitted;
    uint64_t au_played;      /* 已完成输出的 AAC 帧数，时钟来源 */
    uint64_t samples_played; /* 已播放采样（LC 每帧 1024） */
    int64_t clock_us;        /* 由已播放采样累计，不是 queued bytes */
    uint64_t bytes_queued;   /* 仅诊断，不得当时钟 */
    uint64_t partial_writes;
    uint64_t backpressure;
    int sample_rate;
    int channels;
    int pcm_bits; /* 16 */
    int pcm_format_changes;
    int configured;
    int started;
    int eos_in;
    int eos_out;
    int flushed;
    int canceled;
    unsigned pending;
    int is_hardware;
    int system_decode_is_not_hardware;
    int verified;
    ms_hw_kind kind;
    const char *backend;
    const char *detail;
    ms_status last_error;
} ms_hw_audio_stats;

MSMEDIA_API ms_status ms_hw_audio_query(ms_codec codec, ms_hw_audio_caps *out);
MSMEDIA_API ms_status ms_hw_audio_create(const ms_allocator *alloc,
                                         const ms_hw_limits *limits,
                                         const ms_hw_create_opts *opts,
                                         ms_hw_audio **out);
MSMEDIA_API void ms_hw_audio_destroy(ms_hw_audio *a);
MSMEDIA_API ms_status ms_hw_audio_configure(ms_hw_audio *a,
                                            const ms_hw_audio_config *cfg);
MSMEDIA_API ms_status ms_hw_audio_start(ms_hw_audio *a);
MSMEDIA_API ms_status ms_hw_audio_submit(ms_hw_audio *a, const ms_hw_buf *buf,
                                         int64_t pts_us, unsigned flags);
MSMEDIA_API ms_status ms_hw_audio_drive(ms_hw_audio *a);
/* 宿主/测试：限制后续 drive 可写入的 PCM 字节（模拟 AudioTrack 部分 write）。
 * 0 表示本次起不再写入直到再次增加额度。未调用则额度不受限。设备路径忽略此函数。 */
MSMEDIA_API ms_status ms_hw_audio_add_write_quota(ms_hw_audio *a, size_t pcm_bytes);
MSMEDIA_API ms_status ms_hw_audio_signal_eos(ms_hw_audio *a);
MSMEDIA_API ms_status ms_hw_audio_flush(ms_hw_audio *a);
MSMEDIA_API ms_status ms_hw_audio_cancel(ms_hw_audio *a);
MSMEDIA_API ms_status ms_hw_audio_get_caps(const ms_hw_audio *a, ms_hw_audio_caps *out);
MSMEDIA_API ms_status ms_hw_audio_get_stats(const ms_hw_audio *a, ms_hw_audio_stats *out);
MSMEDIA_API ms_status ms_hw_audio_get_clock_us(const ms_hw_audio *a, int64_t *clock_us);
/* 在 configure 之前设置。默认 HARDWARE_ONLY。ALLOW_SYSTEM 仅放行系统 AAC，
 * 不是视频软解，也不把系统软解写成 is_hardware。 */
MSMEDIA_API ms_status ms_hw_audio_set_aac_policy(ms_hw_audio *a, ms_hw_aac_policy policy);
MSMEDIA_API ms_hw_aac_policy ms_hw_audio_get_aac_policy(const ms_hw_audio *a);
/* Android 系统软件解码器名前缀：OMX.google / c2.android / c2.google。 */
MSMEDIA_API int ms_hw_is_system_software_codec_name(const char *name);

/* 后台遗留工作线程上限（进程内）。达到上限时拒绝新建，返回可诊断错误，
 * 不只依赖 detach。单播放器仍只有一条工作线程。 */
#define MS_HW_ABANDONED_WORKER_CAP 2
MSMEDIA_API int ms_hw_abandoned_worker_count(void);
MSMEDIA_API int ms_hw_abandoned_worker_cap(void);
MSMEDIA_API int ms_hw_can_spawn_worker(void); /* 1=可以新建；0=已达遗留上限 */

#ifdef __cplusplus
}
#endif

#endif
