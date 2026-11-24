/*
 * 可选平台硬解会话（独立 libmshw，不链入 libmsmedia 核心）。
 * 本里程碑：H.264、HEVC、MPEG-2 Video、MPEG-4 Part 2、H.263 与 AV1 压缩 ES 硬解提交。后续 codec 可在同一接口扩展，
 * 本构建对尚未落实的 codec 返回 UNSUPPORTED，不默默软解。
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
    const char *backend; /* "none" / "mock" / "amediacodec" / "jni-mediacodec" / "vaapi" */
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
    ms_codec codec;              /* H.264/HEVC/MPEG-2/MPEG-4 Part 2/H.263/AV1/AVS+ */
    ms_sample_format sample_fmt; /* H.264: AVCC/ANNEXB；HEVC: HVCC/ANNEXB；MPEG-2: MPV；MPEG-4: MP4V；H.263: H263 可空 CSD；AV1: AV1；AVS+: CAVS，CSD 为广播类序列头 */
    const uint8_t *csd;          /* avcC / hvcC，或含参数集的 Annex B */
    size_t csd_size;
    int width;                   /* 0=未知，等待 FORMAT_CHANGED */
    int height;
    int64_t time_base_num;       /* 0=默认 1 */
    int64_t time_base_den;       /* 0=默认 1000000（与 MediaCodec 微秒一致） */
    void *surface;               /* 设备上为 ANativeWindow* 或 Surface；mock 任意非 NULL 令牌 */
    /* 加法：HDR 识别结果。struct_size 覆盖到本字段且 hdr.struct_size>0 时，
     * Android API24+ configure 写入 color / hdr-static-info。不是显示器效果。 */
    ms_hdr_info hdr;
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

/*
 * 默认查询。Android 走 MediaCodec 分类。Linux 宿主走 VAAPI DRM 探测：
 * 无设备、无法打开或尚未实现 slice 提交时 is_hardware=0、verified=1、
 * backend="vaapi"。不得把探测到设备写成已可硬解出画。mock 状态机不经本函数伪造硬解。
 */
MSMEDIA_API ms_status ms_hw_decoder_query(ms_codec codec, ms_hw_caps *out);

/*
 * Linux VAAPI：从真实码流参数映射 libva VAProfile（数值与 libva 2.22 一致），
 * 以及打开 DRM 节点 + vaInitialize/vaQueryConfigProfiles 的设备探测。
 * 解析成功不等于设备支持，更不等于已实现提交/出画。
 * 本构建 decode_implemented 恒为 0，is_hardware 恒为 0。
 * AVS+ 可解析广播类 profile_id=0x48，但 libva 无对应 profile，mapped=0。
 */
#ifndef MS_HW_VAAPI_PROFILE_MAX
#define MS_HW_VAAPI_PROFILE_MAX 32
#endif

typedef struct ms_hw_vaapi_stream {
    size_t struct_size;
    ms_codec codec;
    int va_profile;              /* libva VAProfile；未映射为 -1 */
    const char *profile_name;
    unsigned bitstream_profile;  /* profile_idc / seq_profile / profile 高半字节等 */
    unsigned constraint_flags;
    unsigned level_idc;
    unsigned chroma_format_idc;
    unsigned bit_depth;
    int mapped;                  /* 1=已映射到明确的 VAProfile，不是 MIME 猜测 */
    int complete;
} ms_hw_vaapi_stream;

typedef struct ms_hw_vaapi_probe {
    size_t struct_size;
    int libva_loaded;
    int have_device;             /* 已打开 DRM 并 vaInitialize + 查出 profile */
    int decode_implemented;      /* 本构建为 0 */
    int is_hardware;             /* 在 decode_implemented 为 0 时必须为 0 */
    int verified;
    int profile_count;
    int profiles[MS_HW_VAAPI_PROFILE_MAX];
    int profile_has_vld[MS_HW_VAAPI_PROFILE_MAX];
    int libva_major;
    int libva_minor;
    const char *backend; /* "vaapi" 或非 Linux 构建上的 "none" */
    const char *vendor;
    const char *detail;
    char vendor_buf[128];
    char detail_buf[192];
    char device_path[64];
} ms_hw_vaapi_probe;

MSMEDIA_API void ms_hw_vaapi_stream_default(ms_hw_vaapi_stream *out);
MSMEDIA_API ms_status ms_hw_vaapi_parse_stream(ms_codec codec, ms_sample_format fmt,
                                               const uint8_t *data, size_t size,
                                               ms_hw_vaapi_stream *out);
MSMEDIA_API const char *ms_hw_vaapi_profile_name(int va_profile);
MSMEDIA_API void ms_hw_vaapi_probe_default(ms_hw_vaapi_probe *out);
MSMEDIA_API ms_status ms_hw_vaapi_probe_device(ms_hw_vaapi_probe *out);
MSMEDIA_API int ms_hw_vaapi_probe_has_vld(const ms_hw_vaapi_probe *p, int va_profile);

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

/*
 * 测试覆盖解码器组件名。name=NULL 或空串清除覆盖。
 * 配置前调用；软件名仍不把 is_hardware 置 1。默认不调用则行为不变。
 */
MSMEDIA_API ms_status ms_hw_decoder_set_preferred_name(ms_hw_decoder *d,
                                                       const char *name);

/*
 * 无 Surface（ByteBuffer）时，drive 后可拷最近一帧解码图像（YUV 原样）。
 * Surface 路径不保存像素，返回 AGAIN。仅测试取证，不是正式出画。
 */
MSMEDIA_API ms_status ms_hw_decoder_copy_last_output(ms_hw_decoder *d,
                                                     int *width, int *height,
                                                     int *stride, int *slice_height,
                                                     int *color_format,
                                                     uint8_t *dst, size_t dst_cap,
                                                     size_t *out_size);

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
    /*
     * 测试专用加法字段（默认 NULL）。非空则按该组件名 createByName，
     * 允许选用系统软件解码器（如 c2.android.avc.decoder / OMX.google.h264.decoder）
     * 以区分模拟器 goldfish 管道与产品路径。默认行为不变：仍只优先硬件，
     * 不把软件组件报成 is_hardware，也不放开全面视频软解。
     */
    const char *force_decoder_name;
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
    /* P4 音轨/字幕（additive）。宿主 mock 不是设备出声或上屏字幕。 */
    unsigned selection_epoch;
    uint32_t audio_track_id; /* 0=当前无选中音轨 */
    uint32_t sub_track_id;   /* 0=字幕关闭 */
    uint64_t audio_switch_count;
    uint64_t audio_au_dropped_unselected;
    int have_external_sub;
    /* P4 倍速/seek（additive）。宿主 mock 不是设备音画同步倍速或出画。 */
    int paused;
    int rate_num;
    int rate_den;
    int seekable; /* -1=未知，0=不可，1=可 */
    int64_t duration_90k; /* -1=未知 */
    int64_t position_90k;
    uint64_t timeline_epoch;
    int64_t last_seek_request_90k;
    int64_t last_seek_land_90k;
    int have_seek_land;
    /* 生命周期（additive，ABI 仍为 1）。宿主 mock 不是设备前后台/焦点验收。
     * user_paused 仅由 pause/resume 改变；音频焦点恢复不得清掉用户暂停。
     * audio_focus：0=未通知，1=GAIN，2=LOSS，3=LOSS_TRANSIENT，
     * 4=LOSS_TRANSIENT_CAN_DUCK（本实现按短暂丢失暂停，不做音量闪避）。
     * surface_bound：当前 set_surface 非空。display 仍仅 note_display。 */
    int user_paused;
    int audio_focus;
    int surface_bound;
    /* 加法：AudioTrack 累计写入与粗略音画同步。模拟器/宿主不是真机硬解或听感同步。 */
    uint64_t audio_track_write_bytes;
    uint64_t audio_track_write_frames;
    int64_t audio_track_head_frames; /* 无则为 -1 */
    int64_t video_last_pts_us;       /* 最近已交 Surface 的视频 PTS；无则为 -1 */
    int have_video_last_pts;
    int64_t av_sync_delta_us;        /* video_last_pts_us - audio_clock_us */
    int have_av_sync;
    /* 加法：Surface 刚重绑后需要向最近关键帧重送（可 seek）或等 IDR（直播）。 */
    int need_keyframe_resync;
    /* 加法：已识别的 HDR 信息（不扩展 ms_event）。kind=unknown 表示尚未识别。 */
    ms_hdr_info hdr;
} ms_hw_player_stats;

MSMEDIA_API void ms_hw_player_opts_default(ms_hw_player_opts *out);
MSMEDIA_API ms_status ms_hw_player_create(const ms_allocator *alloc,
                                          const ms_hw_player_opts *opts,
                                          ms_hw_player **out);
MSMEDIA_API void ms_hw_player_destroy(ms_hw_player *p);
MSMEDIA_API void ms_hw_player_set_log(ms_hw_player *p, ms_hw_log_fn fn, void *ctx);
MSMEDIA_API ms_status ms_hw_player_set_surface(ms_hw_player *p, void *surface);
/* 测试专用：须在首次 configure 之前调用。默认不调用。 */
MSMEDIA_API ms_status ms_hw_player_set_force_decoder_name(ms_hw_player *p,
                                                          const char *name);
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
/*
 * 在 ms_hw_player_feed 之前为即将送入的音频 AU 设置 PCM 修剪。
 * 只作用于下一次 feed 的音频 AU，用后清零。不改变 feed 的既有签名。
 */
MSMEDIA_API ms_status ms_hw_player_note_event_trim(ms_hw_player *p,
                                                   uint32_t skip_samples_start,
                                                   uint32_t skip_samples_end);

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
/* 释放视频与音频解码器及在途引用，保留 CSD/轨道；用于切轨或显式拆会话。
 * Surface 丢失/重绑请用 set_surface(NULL)/set_surface(新表面)：只拆视频解码器，保留音频与会话。 */
MSMEDIA_API ms_status ms_hw_player_release_decoder(ms_hw_player *p);
MSMEDIA_API ms_status ms_hw_player_get_stats(const ms_hw_player *p,
                                             ms_hw_player_stats *out);
MSMEDIA_API ms_status ms_hw_player_get_caps(const ms_hw_player *p, ms_hw_caps *out);
/* 仅应用在其认定的真实上屏时调用。编解码、releaseOutputBuffer、
 * 单次 Choreographer 回调、OnFrameRendered 均不得调用本函数。 */
MSMEDIA_API ms_status ms_hw_player_note_display(ms_hw_player *p, int64_t pts,
                                                int has_pts);
/*
 * P4：枚举/选择音轨与文本字幕（加法 ABI，MSMEDIA_HW_ABI 仍为 1）。
 * 播放中切音轨会关闭并 flush 旧音频会话、丢弃 ASC，等待新的 CODEC_CONFIG
 * 后重建时钟；不是设备出声验收。字幕关闭或 flush/seek 清空在途内嵌 cue。
 * 位图可选中枚举，打开播放返回 UNSUPPORTED。
 * ASS/SSA 可选择进入统一 cue（布局模型，非完整渲染，非设备出画验收）。
 * language/name 指针在下一次 feed/select/destroy 前有效。
 * by: Long <mycjzb@gmail.com>
 */
MSMEDIA_API ms_status ms_hw_player_list_tracks(const ms_hw_player *p,
                                               ms_track_info *out, size_t cap,
                                               size_t *count);
MSMEDIA_API ms_status ms_hw_player_select_track(ms_hw_player *p, uint32_t track_id,
                                                int on);
MSMEDIA_API ms_status ms_hw_player_set_external_subtitle(ms_hw_player *p,
                                                         const uint8_t *data,
                                                         size_t size,
                                                         ms_sub_format fmt);
MSMEDIA_API ms_status ms_hw_player_subtitles_at(const ms_hw_player *p,
                                                uint64_t time_90k,
                                                ms_sub_cue *out, size_t cap,
                                                size_t *count);
/* 最近一次带 PTS 的音/视频 AU 换算的 90 kHz 媒体时间；没有则为 0。 */
MSMEDIA_API uint64_t ms_hw_player_last_media_90k(const ms_hw_player *p);
/*
 * P4 倍速与相对/绝对 seek（加法 ABI）。
 * 倍速必须走平台音频能力：旧 API 不能跟上则 UNSUPPORTED，
 * 不得只改视频时钟却称为音画同步倍速。负速率（倒放）不支持。
 * 0 或 den<=0 为 INVALID_ARG（暂停请用 pause）。
 * seek 后 flush 音视频、清空在途字幕、等待关键帧并保留 CSD；
 * 相对 seek 检查溢出、负值钳到 0；VOD 按时长钳位。HLS EVENT/滑动窗口
 * 用 note_seek_range 的 start/live_edge 钳位，不得把窗口长度当 VOD 时长。
 * 不可定位源（UDP/RTP/RTMP 等）为 NOT_SEEKABLE。
 * 宿主 mock 可记账并测时钟连续性，但 caps.mock_is_not_device=1。
 * by: Long <mycjzb@gmail.com>
 */
typedef struct ms_hw_rate_caps {
    size_t struct_size;
    int can_apply;       /* 本会话能否套用该能力集内的速率 */
    int can_sync_av;     /* 1=平台能带动音频；mock 为 0 */
    int can_keep_pitch;  /* 1=PlaybackParams 等可保音调（通常 API 23+） */
    int min_num;
    int min_den;
    int max_num;
    int max_den;
    int api_level;       /* 宿主 mock 为 0 */
    int mock_is_not_device; /* 宿主恒为 1 */
    const char *backend;
    const char *detail;
} ms_hw_rate_caps;

MSMEDIA_API ms_status ms_hw_player_get_rate_caps(const ms_hw_player *p,
                                                 ms_hw_rate_caps *out);
MSMEDIA_API ms_status ms_hw_player_set_rate(ms_hw_player *p, int num, int den);
MSMEDIA_API ms_status ms_hw_player_get_rate(const ms_hw_player *p, int *num, int *den);
MSMEDIA_API ms_status ms_hw_player_pause(ms_hw_player *p);
MSMEDIA_API ms_status ms_hw_player_resume(ms_hw_player *p);
MSMEDIA_API int ms_hw_player_is_paused(const ms_hw_player *p);
/* 用户按钮暂停，不含焦点/生命周期暂停。 */
MSMEDIA_API int ms_hw_player_is_user_paused(const ms_hw_player *p);
/*
 * 系统音频焦点变化（加法 ABI）。GAIN 仅在未用户暂停时恢复。
 * LOSS / TRANSIENT / TRANSIENT_CAN_DUCK 均进入有效暂停（不做音量闪避）。
 * 不是设备焦点验收。
 */
#define MS_HW_AUDIO_FOCUS_NONE 0
#define MS_HW_AUDIO_FOCUS_GAIN 1
#define MS_HW_AUDIO_FOCUS_LOSS 2
#define MS_HW_AUDIO_FOCUS_LOSS_TRANSIENT 3
#define MS_HW_AUDIO_FOCUS_LOSS_TRANSIENT_CAN_DUCK 4
MSMEDIA_API ms_status ms_hw_player_notify_audio_focus(ms_hw_player *p, int focus_event);
MSMEDIA_API ms_status ms_hw_player_set_volume(ms_hw_player *p, float left, float right);
MSMEDIA_API ms_status ms_hw_player_video_size(const ms_hw_player *p, int *w, int *h);

/* 告知已知时长/可否定位（-1 时长=未知）。通常在源 open 之后调用。 */
MSMEDIA_API ms_status ms_hw_player_note_timeline(ms_hw_player *p, int seekable,
                                                 int64_t duration_90k);
MSMEDIA_API int64_t ms_hw_player_position_90k(const ms_hw_player *p);
MSMEDIA_API int64_t ms_hw_player_duration_90k(const ms_hw_player *p);
MSMEDIA_API int ms_hw_player_is_seekable(const ms_hw_player *p);
/* 绝对定位（90 kHz 媒体轴，与 PTS 同轴，相对会话 origin）。负值为 INVALID_ARG。 */
MSMEDIA_API ms_status ms_hw_player_seek(ms_hw_player *p, int64_t time_90k);
/* 相对当前 position 的定位（快进/快退，不是倒放）。 */
MSMEDIA_API ms_status ms_hw_player_seek_relative(ms_hw_player *p, int64_t delta_90k);

/* 加法：源端可定位窗口（与 ms_ff_seek_range 字段同义，90 kHz 相对 origin）。 */
#ifndef MS_HW_SEEK_KIND_UNKNOWN
#define MS_HW_SEEK_KIND_UNKNOWN 0
#define MS_HW_SEEK_KIND_VOD 1
#define MS_HW_SEEK_KIND_EVENT 2
#define MS_HW_SEEK_KIND_SLIDING 3
#define MS_HW_SEEK_KIND_UNSEEKABLE 4
#endif
typedef struct ms_hw_seek_range {
    size_t struct_size;
    int seekable;
    int kind;
    int64_t start_90k;
    int64_t end_90k;
    int64_t live_edge_90k;
    int64_t safety_90k;
    int64_t duration_90k;
} ms_hw_seek_range;
MSMEDIA_API ms_status ms_hw_player_note_seek_range(ms_hw_player *p,
                                                   const ms_hw_seek_range *r);
MSMEDIA_API ms_status ms_hw_player_get_seek_range(const ms_hw_player *p,
                                                  ms_hw_seek_range *out);
MSMEDIA_API ms_status ms_hw_player_seek_live_edge(ms_hw_player *p);

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
    ms_codec codec;              /* AAC / AC3 / MP3 / MP2 / OPUS；旧调用方仍只填 AAC */
    ms_sample_format sample_fmt; /* AAC: ADTS 或 AAC_RAW；其它：对应 MS_SAMPLE_FMT_* */
    const uint8_t *asc;          /* CSD：AAC 为 AudioSpecificConfig；Opus 为真实 OpusHead；
                                  * AC-3 / MP2 / MP3 合法空。不捏造 AAC ASC。 */
    size_t asc_size;
    int sample_rate; /* 0=从 CSD / 帧头读取 */
    int channels;    /* 0=从 CSD / 帧头读取 */
    /* 以下为加法字段：仅当 struct_size 覆盖到对应字段时才读取。 */
    uint32_t samples_per_frame; /* 0 = 由编码默认（AAC-LC 1024，AC-3 1536） */
    uint32_t pre_skip;          /* Opus 在 48 kHz 的 pre-skip / codec delay */
    uint32_t codec_delay;       /* 与 pre_skip 同义或容器提供的 codec delay */
    uint32_t seek_preroll;      /* 中途 seek 解码预热（Opus 默认 3840）；不是 pre-skip */
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
    int paused;
    int rate_num;
    int rate_den;
    /* 加法：AudioTrack.write 累计与 playbackHeadPosition。宿主 mock 为 0 / -1。
     * 不是扬声器实听证明。 */
    uint64_t track_write_bytes;
    uint64_t track_write_frames; /* PCM 帧（每通道一样点） */
    int64_t track_head_frames;   /* 扩展后的 getPlaybackHeadPosition；无则为 -1 */
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

/*
 * 带本包 start/end skip 的提交（加法）。旧 ms_hw_audio_submit 等价于 skip 全 0。
 * skip_* 是解码后 PCM 样点，不是压缩字节，也不得写入 dts。
 */
typedef struct ms_hw_audio_au {
    size_t struct_size;
    const ms_hw_buf *buf;
    int64_t pts_us;
    unsigned flags;
    uint32_t skip_samples_start;
    uint32_t skip_samples_end;
    int has_pts; /* 加法。旧调用方 struct_size 不含本字段时，视为已提供 pts_us */
} ms_hw_audio_au;

MSMEDIA_API ms_status ms_hw_audio_submit_au(ms_hw_audio *a, const ms_hw_audio_au *au);

/*
 * 告知一次 seek 落点（微秒，会话媒体时间，0 表示呈现起点）。
 * flush 只清空队列与时钟，不会自动把 pre-skip 再扣一次。
 * media_us<=0：下一包若无 skip_start，才用头部 pre-skip 作为流开始修剪。
 * media_us>0：武装 seek_preroll 解码预热，不武装 pre-skip。
 */
MSMEDIA_API ms_status ms_hw_audio_note_seek(ms_hw_audio *a, int64_t media_us);


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
MSMEDIA_API ms_status ms_hw_audio_set_rate(ms_hw_audio *a, int num, int den);
MSMEDIA_API ms_status ms_hw_audio_get_rate_caps(const ms_hw_audio *a, ms_hw_rate_caps *out);
MSMEDIA_API ms_status ms_hw_audio_pause(ms_hw_audio *a);
MSMEDIA_API ms_status ms_hw_audio_resume(ms_hw_audio *a);
/* 可选：修剪后可听 PCM 的非阻塞 tap。fn=NULL 取消。禁用时零成本。
 * 不得在回调中阻塞解码 / AudioTrack / UI。不是软件解码入口。 */
typedef void (*ms_hw_audio_pcm_tap_fn)(void *ctx, const uint8_t *pcm, size_t bytes,
                                       int sample_rate, int channels, int pcm_bits,
                                       int has_pts, int64_t pts_us);
MSMEDIA_API void ms_hw_audio_set_pcm_tap(ms_hw_audio *a, ms_hw_audio_pcm_tap_fn fn, void *ctx);
/* 在 configure 之前设置。默认 HARDWARE_ONLY。ALLOW_SYSTEM 仅放行系统 AAC，
 * 不是视频软解，也不把系统软解写成 is_hardware。 */
MSMEDIA_API ms_status ms_hw_audio_set_aac_policy(ms_hw_audio *a, ms_hw_aac_policy policy);
/* 加法：AudioTrack 左右声道音量 0..1（对齐 MediaPlayer.setVolume）。宿主只校验参数。 */
MSMEDIA_API ms_status ms_hw_audio_set_volume(ms_hw_audio *a, float left, float right);
MSMEDIA_API ms_hw_aac_policy ms_hw_audio_get_aac_policy(const ms_hw_audio *a);
/* Android 系统软件解码器名前缀：OMX.google / c2.android / c2.google。 */
MSMEDIA_API int ms_hw_is_system_software_codec_name(const char *name);

/* 后台遗留工作线程上限（进程内）。达到上限时拒绝新建，返回可诊断错误，
 * 不只依赖 detach。单播放器仍只有一条工作线程。 */
#define MS_HW_ABANDONED_WORKER_CAP 2
MSMEDIA_API int ms_hw_abandoned_worker_count(void);
MSMEDIA_API int ms_hw_abandoned_worker_cap(void);
MSMEDIA_API int ms_hw_can_spawn_worker(void); /* 1=可以新建；0=已达遗留上限 */


/* 可选完整 ASS 栅格（libmsass，dlopen）。未加载时 available/has_script=0，轻量路径仍可用。
 * 栅格不是视频软解，不读回视频 Surface。加法 ABI，旧头客户端不依赖这些符号。 */
typedef struct ms_hw_ass_frame {
    size_t struct_size;
    int available;
    int has_script;
    int changed;
    int x;
    int y;
    int w;
    int h;
    const uint8_t *rgba;
    size_t rgba_size;
    int canvas_w;
    int canvas_h;
} ms_hw_ass_frame;

MSMEDIA_API int ms_hw_player_ass_render_loaded(const ms_hw_player *p);
MSMEDIA_API int ms_hw_player_ass_has_script(const ms_hw_player *p);
MSMEDIA_API ms_status ms_hw_player_ass_set_canvas(ms_hw_player *p, int canvas_w, int canvas_h);
MSMEDIA_API ms_status ms_hw_player_ass_frame_at(ms_hw_player *p, uint64_t time_90k,
                                                ms_hw_ass_frame *out);

/* 可选 AI 字幕插件（libmsai，dlopen）。默认禁用，无供应商、无模型、无网络。
 * vendor=1 仅为受控本地假供应商（模拟，不是真实识别）；其它值尚未指定，返回 UNSUPPORTED。
 * JNI/测试 APK 不得伪报已接某商业 AI。加法 ABI。 */
MSMEDIA_API int ms_hw_player_ai_module_loaded(const ms_hw_player *p);
MSMEDIA_API const char *ms_hw_player_ai_status(const ms_hw_player *p);
MSMEDIA_API ms_status ms_hw_player_ai_configure(ms_hw_player *p, int vendor,
                                                const char *source_lang,
                                                const char *target_lang);
MSMEDIA_API ms_status ms_hw_player_ai_set_enabled(ms_hw_player *p, int on);
MSMEDIA_API int ms_hw_player_ai_enabled(const ms_hw_player *p);

/* Surface 重建后：可 seek 时 take 成功并给出当前位置（90 kHz），调用方应对该点
 * 做向最近关键帧的内部 seek/重送；直播不可 seek 时返回 0，只等 IDR。
 * take 成功只取走内部 seek 目标，need_keyframe_resync 仍保持到出一帧。加法 ABI。 */
MSMEDIA_API int ms_hw_player_take_surface_resync(ms_hw_player *p, int64_t *resync_90k);
MSMEDIA_API int ms_hw_player_has_presented_video(const ms_hw_player *p);
MSMEDIA_API int ms_hw_player_surface_bound(const ms_hw_player *p);
/* 立即关闭全部已选中的带内/外挂字幕（不含 AI 轨），并清空 cues / ASS 绑定。加法 ABI。 */
MSMEDIA_API ms_status ms_hw_player_clear_subtitles(ms_hw_player *p);

/* 宿主测试：给内部音频会话增加可写 PCM 配额。0 表示本次起不再写入直到再增加。
 * 未调用则不受限。设备路径忽略。用于验证音频钟停走时视频路径不得死锁。加法 ABI。 */
MSMEDIA_API ms_status ms_hw_player_debug_audio_add_write_quota(ms_hw_player *p,
                                                               size_t pcm_bytes);

/* 加法查询：合并码流 VUI/SEI 与容器（ms_ff_get_hdr_info）后的 HDR 信息。
 * 不改 ABI 版本，不写入 ms_event。set 为合并（已有字段不覆盖）。 */
MSMEDIA_API ms_status ms_hw_player_set_hdr_info(ms_hw_player *p, const ms_hdr_info *info);
MSMEDIA_API ms_status ms_hw_player_get_hdr_info(const ms_hw_player *p, ms_hdr_info *out);

#ifdef __cplusplus
}
#endif

#endif
