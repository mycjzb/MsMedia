/*
 * MsMedia 公共 C ABI。
 * by: Long <mycjzb@gmail.com>
 *
 * 本头文件是唯一对外入口。调用约定：
 * - 返回 ms_status，不使用异常；分配失败返回 MS_ERR_NOMEM，不中止进程。
 * - 一个会话同一时刻只允许一个线程调用。
 * - ms_session_push 只在调用期间借用输入指针，返回后核心不再保留该指针。
 * - ms_session_pull 给出的数据指针属于会话，在 release / flush / reset /
 *   seek / destroy 之前有效；reset 会增加 session_epoch。
 * - P1a：会话、上限、增量输入输出、reset/epoch，以及 MPEG-TS 同步和
 *   PAT/PMT 轨道发现。
 * - P1b：仅 MPEG-TS 上的 AAC ADTS 子集完整帧与 AudioSpecificConfig。
 * - P1c/P1d：MPEG-TS 上 H.264 Annex B 子集完整 AU 与 SPS/PPS config。
 *   边界为 AUD，或无 AUD 时按 ITU-T H.264 7.4.1.2.4 比较主图像字段
 *   （不是 first_mb_in_slice=0）。隔行/PAFF/MBAFF、FMO、未知参数集拒绝。
 *   不把 PES 或单个 NAL 当帧。不做软解。不是全部 H.264。
 * - 四平台硬解查询在本构建中固定返回「非硬解 / 未核实」。
 * - P2a：ISO BMFF（MP4/fMP4/MOV）共用有界箱体遍历；可发出 FORMAT。
 * - P2b：渐进 MP4/MOV（moov 先于 mdat）的 H.264/AAC 完整样本 AU。
 *   H.264 样本为 length-prefixed NAL（配置为完整 avcC）；AAC 为按
 *   sample size 定界的裸 AU（配置为 AudioSpecificConfig）。不把 TS
 *   的 Annex B / ADTS 输入假设套到本路径。
 * - P2d：fMP4 init+moof/mdat 的 H.264/AAC AU。
 * - P2e：经典 FLV（AVC/AAC）完整 AU，合同同 AVCC/AAC_RAW。
 * - P3 首步：HLS media playlist 调度（VOD/live 窗口）。分片交给已有
 *   会话解封装。不是 HTTP 客户端，不是 master/ABR/加密/LL-HLS。
 * - P3 DASH 首步：MPD 清单与分片调度的有界子集（见 ms_dash_*）。
 *   不是 ABR/DRM/多 Period 全集/CMAF LL，不是全部 DASH profile。
 */

#ifndef MSMEDIA_MSMEDIA_H
#define MSMEDIA_MSMEDIA_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 共享库构建（-DMSMEDIA_BUILD_SHARED）时，公共函数以 default 可见性导出。
 * 静态库与调用方不定义该宏，此宏为空，不影响现有 .a 与 ABI 布局。
 */
#if defined(MSMEDIA_BUILD_SHARED) && defined(__GNUC__)
#define MSMEDIA_API __attribute__((visibility("default")))
#else
#define MSMEDIA_API
#endif

#define MSMEDIA_ABI_VERSION 1u

typedef enum ms_status {
    MS_OK = 0,
    MS_AGAIN,              /* 需要更多输入，或当前没有可取事件 */
    MS_EOS,                /* 输入结束且已发出的事件已取完 */
    MS_LIMIT,              /* 反压：输出队列/槽满，或动态预算暂时用尽 */
    MS_ERR_UNSUPPORTED,    /* 明确不支持 */
    MS_ERR_UNKNOWN,        /* 探测失败或无法识别 */
    MS_ERR_MALFORMED,      /* 畸形输入，或确认格式后重同步失败 */
    MS_ERR_OVERFLOW,       /* 长度或计数溢出 */
    MS_ERR_CANCELED,       /* 已 cancel，直到 reset */
    MS_ERR_NOMEM,
    MS_ERR_STATE,          /* 当前状态不允许该操作（含过期 release） */
    MS_ERR_INVALID_ARG,
    MS_ERR_NOT_SEEKABLE,
    /* 追加在末尾，既有枚举值不变。I/O 或操作超时；不是用户 cancel。 */
    MS_ERR_TIMEOUT
} ms_status;

typedef enum ms_format {
    MS_FORMAT_UNKNOWN = 0,
    MS_FORMAT_MPEG_TS = 1,
    MS_FORMAT_MP4 = 2,     /* 渐进 ISO BMFF；P2b 可输出 moov 在前的 H264/AAC AU */
    MS_FORMAT_FMP4 = 3,    /* 分段 / moof+mdat；P2d 可输出 init 后的 H264/AAC AU */
    MS_FORMAT_MOV = 4,     /* QuickTime；P2b/P2c 可输出渐进或有界回看的 H264/AAC AU */
    MS_FORMAT_FLV = 5,     /* 经典 FLV；P2e 可输出 AVC/AAC AU */
    MS_FORMAT_HLS = 6,     /* 可选 FFmpeg 后端：HLS 清单（追加枚举，旧偏移不变） */
    MS_FORMAT_DASH = 7,    /* 可选 FFmpeg 后端：MPEG-DASH MPD（追加枚举，旧偏移不变） */
    MS_FORMAT_RTSP = 8,    /* 可选 FFmpeg 后端：RTSP 会话（追加；不是 TS） */
    MS_FORMAT_RTMP = 9,    /* 可选 FFmpeg 后端：RTMP 拉流，媒体为 FLV 标签（追加） */
    MS_FORMAT_RTP = 10,    /* 可选 FFmpeg 后端：RTP/SDP（追加；不是 TS） */
    MS_FORMAT_MATROSKA = 11, /* 可选 FFmpeg 后端：MKV（matroska demuxer，追加） */
    MS_FORMAT_WEBM = 12,   /* 与 matroska 同一 demuxer；仅在已测编码上宣传 */
    /* 加法封装：旧值不变。仅解封装，不是软解。 */
    MS_FORMAT_AVI = 13,
    MS_FORMAT_MPEGPS = 14,
    MS_FORMAT_MP3 = 15,
    MS_FORMAT_FLAC = 16,
    MS_FORMAT_WAV = 17,
    MS_FORMAT_ASF = 18,
    MS_FORMAT_RM = 19
} ms_format;

/*
 * 目标 codec 枚举。出现在枚举中不等于已经实现码流确认或 AU 组帧。
 * 真实级别必须通过 ms_codec_query 或轨道事件里的 level 查询。
 */
typedef enum ms_codec {
    MS_CODEC_UNKNOWN = 0,
    MS_CODEC_H264,
    MS_CODEC_AAC,            /* TS 路径为 ADTS；MP4/MOV 路径为裸 AU。LATM 不处理 */
    MS_CODEC_H263,
    MS_CODEC_HEVC,
    MS_CODEC_MPEG4_PART2,
    MS_CODEC_MPEG2_VIDEO,
    MS_CODEC_AVS_FAMILY,     /* TS 中的 AVS 家族标识，不得当成 AVS+ */
    MS_CODEC_AVS_PLUS,
    MS_CODEC_AV1,
    MS_CODEC_AC3,
    MS_CODEC_EAC3,
    MS_CODEC_MPEG_AUDIO,     /* TS 0x03/0x04，未确认 layer 前不得叫 MP2/MP3 */
    MS_CODEC_MP3,
    MS_CODEC_MP2,
    MS_CODEC_OPUS,
    MS_CODEC_G711_ALAW,
    MS_CODEC_G711_ULAW,
    /* P4 文本字幕（追加在末尾，既有枚举值不变）。位图/复杂样式不得冒充已支持。 */
    MS_CODEC_SUB_TEXT,     /* 无更细格式的定时文本 */
    MS_CODEC_SUB_SRT,
    MS_CODEC_SUB_WEBVTT,
    MS_CODEC_SUB_TX3G,     /* MP4 3GPP Timed Text 的纯文本子集 */
    MS_CODEC_SUB_BITMAP,   /* 未细分的位图族（XSUB/图文等仍不可播） */
    MS_CODEC_SUB_ASS,      /* 文本 ASS/SSA：有界解析与布局模型；不是完整渲染器，核心不做字体栅格 */
    /* 加法枚举，旧值不变。DVB/PGS/VobSub 为可解析的位图字幕，不是软解视频。 */
    MS_CODEC_SUB_DVB,
    MS_CODEC_SUB_PGS,
    MS_CODEC_SUB_VOBSUB,
    /* 加法：FLAC / 线性 PCM。仅解封装与直出 PCM，不是软解、不扩大 AAC 例外。 */
    MS_CODEC_FLAC,
    MS_CODEC_PCM,
    /* 加法（单 so 路线 B 音频兜底）：DTS 与 AAC-LATM(LOAS)。旧值不变；
     * 官方多 so 不产出这两个值，只有 single-slimff 的 ms_ff 映射与 FFmpeg 内置软解使用。 */
    MS_CODEC_DTS,
    MS_CODEC_AAC_LATM
} ms_codec;

/*
 * 访问单元 / 配置载荷的样本数据格式。不得凭 codec 名去猜。
 * TS 路径：H.264 为 Annex B，AAC 为 ADTS。
 * 渐进 MP4/MOV、fMP4 与经典 FLV 路径：H.264 为 avcC 长度前缀，AAC 为裸 AU。
 */
typedef enum ms_sample_format {
    MS_SAMPLE_FMT_UNKNOWN = 0,
    MS_SAMPLE_FMT_ANNEXB,    /* H.264 起始码 AU（MPEG-TS） */
    MS_SAMPLE_FMT_AVCC,      /* 长度前缀 NAL；CODEC_CONFIG 为完整 avcC */
    MS_SAMPLE_FMT_ADTS,      /* 带 ADTS 头的 AAC 帧（MPEG-TS） */
    MS_SAMPLE_FMT_AAC_RAW,   /* 按 sample size 定界的裸 AAC AU；CONFIG 为 ASC */
    MS_SAMPLE_FMT_HVCC,      /* 长度前缀 HEVC NAL；CODEC_CONFIG 为完整 hvcC。加法枚举，旧值不变 */
    MS_SAMPLE_FMT_MPV,       /* MPEG-1/2 起始码 ES；CODEC_CONFIG 为 sequence header(+extension) */
    MS_SAMPLE_FMT_MP4V,      /* MPEG-4 Part 2 起始码 ES；CODEC_CONFIG 为 VOS/VOL */
    MS_SAMPLE_FMT_H263,      /* H.263 短视频头 ES；通常无独立 CSD，配置在图象头 */
    MS_SAMPLE_FMT_AV1,       /* AV1 OBU / av1C；有合法 av1C 则作 csd-0，否则可走合法无 CSD */
    MS_SAMPLE_FMT_AC3,       /* ATSC A/52 AC-3 完整帧；合法无 CSD，不捏造 AAC ASC */
    MS_SAMPLE_FMT_MPEG_AUDIO,/* MPEG Audio Layer II/III 完整帧；层由 codec 区分，合法无 CSD */
    MS_SAMPLE_FMT_OPUS,      /* Opus 压缩包；CSD 为真实 OpusHead 或由真实 dOps 装箱 */
    MS_SAMPLE_FMT_CAVS,      /* AVS/AVS+ 起始码 ES；仅序列头 profile_id=0x48（GY/T 257.1 广播类）才是 AVS+ */
    MS_SAMPLE_FMT_FLAC,      /* FLAC 原生帧；CSD 为 STREAMINFO 时原样下发，不捏造 */
    MS_SAMPLE_FMT_PCM        /* 线性 PCM 交错样本；直出，不是压缩软解 */
} ms_sample_format;

typedef enum ms_recog_level {
    MS_RECOG_NONE = 0,         /* 本构建不处理该目标，或尚未识别 */
    MS_RECOG_CONTAINER_ID,     /* 仅容器 / 清单标识 */
    MS_RECOG_BITSTREAM,        /* 码流已确认 */
    MS_RECOG_AU                /* 完整 AU 组帧 */
} ms_recog_level;

typedef enum ms_track_kind {
    MS_TRACK_UNKNOWN = 0,
    MS_TRACK_VIDEO,
    MS_TRACK_AUDIO,
    MS_TRACK_SUBTITLE
} ms_track_kind;

typedef enum ms_event_type {
    MS_EVENT_FORMAT = 1,
    MS_EVENT_TRACK,
    MS_EVENT_TRACK_UPDATE,
    MS_EVENT_CODEC_CONFIG,     /* 见 ms_event.sample_format：ASC / Annex B 参数集 / avcC */
    MS_EVENT_AU,               /* 完整访问单元；载荷格式见 sample_format */
    MS_EVENT_SUBTITLE,         /* 定时文本 cue：data 为 UTF-8；pts=开始，dts=结束（同一 time_base） */
    MS_EVENT_DISCONTINUITY, /* 传输不连续；见 ms_event.discontinuity 注释 */
    MS_EVENT_EOS,
    MS_EVENT_UNSUPPORTED
} ms_event_type;

typedef struct ms_allocator {
    void *(*alloc_fn)(void *ctx, size_t size);
    void (*free_fn)(void *ctx, void *ptr);
    void *ctx;
} ms_allocator;

/*
 * 会话上限。传 NULL 使用默认值。0 也表示默认。
 * 调用方只能把各项调到不超过默认值；超过默认或超过绝对上限返回
 * MS_ERR_INVALID_ARG。绝对上限由 ms_session_limits_absolute 查询。
 *
 * 默认（待设计最终确认，已作为本实现的暂定值）：
 *   probe 1 MiB、AU 队列 2 MiB、输出槽 8、单次 push 1 MiB、
 *   格式确认后重同步 256 KiB。
 *
 * 默认（续）：单个 PES 或单个 ES 组帧器装配上限 2 MiB；
 * 会话动态总预算 16 MiB；渐进 MP4/MOV 的 moov-在-尾有界回看默认 8 MiB。
 * 绝对上限：装配 16 MiB、总预算 64 MiB、回看 16 MiB。
 * 调用方只能调低，不能高于对应默认值。
 *
 * 字段合同：
 *   前五个字段的偏移与历史布局兼容。新增字段追加在末尾。
 *   struct_size 应填 sizeof(ms_session_limits)；
 *   ms_session_limits_default / absolute 会写入。
 *   struct_size 为 0 时，按旧五字段布局解析，新字段取默认值
 *   （不读取扩展字段）。只识别不大于本构建 sizeof 的前缀。
 *
 * au_queue_bytes：只限制已发出、尚未 release 的 AU/config 载荷总和，
 * 与装配上限独立。缩小输出队列不得把合法多帧 PES 装配变成 MALFORMED。
 * max_assembly_bytes：单个 PES 组装缓冲、或单个 ES（ADTS/H.264）组帧器
 * 的上限。单个访问单元/PES 声明或组装超过该值是 MALFORMED。
 * session_budget_bytes：跨轨会话动态分配总预算，涵盖：输入缓冲、
 * 输出队列载荷、各轨 PES 组装、ES 组帧缓冲、ES pending、按需分配
 * 的轨道 ES 解析状态（H.264/ADTS 对象，含参数集槽位表）、懒分配的
 * FLV 解析器及其当前 tag / 配置缓冲、H.264
 * 参数集 NAL 副本，懒分配的 BMFF 解析器与其 sample 表 / 当前样本
 * 缓冲、moov-在-尾时的有界 mdat 回看窗，以及调试 PES 载荷
 * （预算不足时调试副本可放弃，不影响公共 AU 路径）。记账含每块内部头。
 * 固定会话对象不计入该预算：ms_session 本体（含嵌入的 TS 解析器、
 * 轨道元数据表、PSI 固定数组、事件节点元数据），以及 create 时
 * 分配的输出槽表。轨道解析状态按实际使用惰性分配并计入动态预算，
 * 不用则不占固定对象。固定本体与输出槽仍常驻，不能把动态预算当成
 * 进程 RSS 上限。
 *
 * LIMIT：此刻不能再收（输出队列满、输出槽满、或动态预算暂时用尽）。
 * 若 pull 仍能取到事件，release 后可重试。若 consumed=0 且 pull 为
 * AGAIN，则当前输入在现有限额下无法前进，不要对同一输入无限重试；
 * 可用 flush/reset 恢复，会话不因此被标为失败，已入队事件仍可取。
 * AGAIN：需要更多输入，或当前没有可取事件（不是资源上限）。
 * MALFORMED：畸形码流，或单 PES/AU 超过装配上限（drain 不能使该
 * 单元变合法）；此类错误会使会话失败。
 */
typedef struct ms_session_limits {
    size_t probe_bytes;
    size_t au_queue_bytes;
    size_t out_slots;
    size_t max_push_bytes;
    size_t max_resync_bytes;
    size_t struct_size;            /* 0=旧五字段布局 */
    size_t max_assembly_bytes;     /* 单个 PES 或单个 ES 组帧器 */
    size_t session_budget_bytes;   /* 跨轨动态总预算 */
    /*
     * max_lookback_bytes：渐进 MP4/MOV 在 moov 位于 mdat 之后时，
     * 允许保留的已见 mdat 载荷上限（有界回看）。仅可下调，不可抬升。
     * 0 表示采用默认值。声明的 mdat 载荷超过本上限或剩余会话预算时
     * 不缓存该 mdat（更不会整文件缓存）；若样本表随后需要回看这些
     * 字节，会话返回 MS_ERR_NOT_SEEKABLE 并投递 UNSUPPORTED，但不
     * 因此把会话标为失败，FORMAT / TRACK / CODEC_CONFIG 仍可取出。
     * 若已挂上可随机读且能给出大小的 Source，则可跳过 mdat 去读尾部
     * moov 并按 sample 表取样本，不依赖本回看窗。
     */
    size_t max_lookback_bytes;
} ms_session_limits;

typedef struct ms_epochs {
    uint64_t session_epoch;
    uint64_t timeline_epoch;
    uint64_t selection_epoch;
} ms_epochs;

typedef struct ms_track_info {
    uint32_t id;                 /* MPEG-TS 为 elementary PID */
    ms_track_kind kind;
    ms_codec codec;
    ms_recog_level level;
    uint16_t stream_type;        /* 原始 stream_type，没有则为 0 */
    const char *language;        /* 会话所有，可能为 NULL */
    const char *name;            /* 会话所有，可能为 NULL */
    int is_default;
    int is_forced;
    int selected;
} ms_track_info;

typedef struct ms_event {
    ms_event_type type;
    uint64_t session_epoch;
    uint64_t timeline_epoch;
    uint64_t selection_epoch;
    uint32_t release_id;         /* 取走后必须 ms_session_release */
    ms_format format;
    uint16_t ts_packet_size;     /* FORMAT 且为 TS 时为 188 或 192 */
    ms_track_info track;
    const uint8_t *data;         /* 可选视图，所有权规则同上 */
    size_t data_size;
    /*
     * AU / CODEC_CONFIG。没有时间戳（含 AV_NOPTS）时 has_pts/has_dts 为 0，
     * 不用 0 冒充有效时间。pts/dts 仍为 uint64，布局与 ABI 1 相同，不得改成
     * 有符号字段以免破坏既有二进制。
     * 合同：pts/dts 是相对会话级统一 origin 的非负差值，时基见 time_base_*。
     * 有符号媒体时间 = origin_in_tb + (int64_t)pts，见 ms_event_signed_ts。
     * 合法负值（AAC priming、B 帧 DTS、elst）必须保留 has_*=1，不得丢时间，
     * 也不得把负数写成 uint64 补码绕回。origin 对所有轨相同，不按轨截 0。
     * origin=0 且时间非负时，与旧合同一致（相对值即绝对值）。
     * 通常为 90 kHz 或容器/流时基；33 位封装回绕与合法负值不是同一件事。
     */
    int has_pts;
    int has_dts;
    uint64_t pts;
    uint64_t dts;
    int time_base_num;
    int time_base_den;
    int is_keyframe;             /* AAC：有 config 时可独立解码；H.264：仅 IDR 为 1，不保证可安全起播 */
    /*
     * discontinuity：
     *   MS_EVENT_DISCONTINUITY 上为 1。AU 上为 1 表示该轨在此之前发生过
     *   传输不连续，本 AU 是丢弃不完整 PES/ES 之后恢复的第一帧。
     * 事件语义（本阶段）：
     *   丢包（continuity_counter 非 +1 且不是合法重复）、adaptation
     *   discontinuity_indicator、TEI、加扰包打断组装、同 CC 第三包或
     *   同 CC 但载荷不同、以及字节层失同步，都会发出 DISCONTINUITY，
     *   并丢弃该 PID 上未完成的 PES/ES，不得把缺片当完整 AU 输出。
     *   能归属轨道时 ev.track 填该轨道；字节失同步等无法归属时 track 为零。
     *   不增加 timeline_epoch / selection_epoch / session_epoch
     *   （这些用于 seek、选轨和会话 reset，不是每次 CC 间隙）。
     *   其它轨道不被无故清空（H.264 参数集与 AAC 配置保留）。
     *   合法重复：连续两个同 PID、同 CC、AFC 为 01/11、载荷相同的包，
     *   丢弃第二包且不产生本事件（PCR 可不同，故只比较载荷）。
     *   adaptation_field_control=10 的纯适配包不递增 CC。
     *   current_next_indicator=0 的 PSI 已被忽略。
     *   加扰包不当作明文解析。
     *   无法恢复的输入（如 reserved AFC=00、持续无法同步）返回明确错误。
     *   不承诺对乱序到达（网络/RTP 重排）重新排序，乱序按间隙处理。
     */
    int discontinuity;
    int format_changed;          /* 本 AU 之前刚发出新的 codec config */
    int preroll;
    /*
     * sample_format：TRACK / CODEC_CONFIG / AU 上标明载荷合同。
     * 追加在结构末尾，旧字段偏移不变。未填写时为 UNKNOWN。
     */
    ms_sample_format sample_format;
} ms_event;

/*
 * AU 解码后 PCM 修剪（与 1838 已交付的 ms_event 布局分离，避免 pull
 * 向旧客户端 176 字节栈对象多写 8 字节）。start/end 为解码后样点：
 * start 丢头部，end 丢尾部（discard padding）。0 表示没有该元数据。
 * start 不是 seek preroll，也不把已偏移的负 PTS 再扣一次。
 * 须在对应事件尚未 release 时查询。struct_size 须先填写。
 */
typedef struct ms_event_trim {
    size_t struct_size;
    uint32_t skip_samples_start;
    uint32_t skip_samples_end;
} ms_event_trim;

typedef struct ms_session ms_session;

MSMEDIA_API uint32_t ms_abi_version(void);

MSMEDIA_API const char *ms_status_name(ms_status st);

/*
 * 有符号有理数缩放（向下取整，朝 -inf）：
 *   *out = floor(in * in_num * out_den / (in_den * out_num))
 * 与 ms_ff 把 origin 转到事件时基的规则一致。分母/分子须为正。
 * 溢出返回 MS_ERR_OVERFLOW。这是兼容追加的函数，不改变既有符号。
 */
MSMEDIA_API ms_status ms_rescale_q(int64_t in, int in_num, int in_den,
                                   int out_num, int out_den, int64_t *out);

/*
 * 从公共事件还原有符号媒体时间（事件 time_base）。
 * origin_90k 为会话级统一基准（如 ms_ff_time_origin_90k）；未偏移时为 0。
 * has_*=0 时不写对应输出。加法或时基转换溢出返回 OVERFLOW。
 */
MSMEDIA_API ms_status ms_event_signed_ts(const ms_event *ev, int64_t origin_90k,
                                         int *has_pts, int64_t *pts,
                                         int *has_dts, int64_t *dts);

/* 同上，输出为 90 kHz 有符号媒体时间。 */
MSMEDIA_API ms_status ms_event_media_time_90k(const ms_event *ev, int64_t origin_90k,
                                              int *has_pts, int64_t *pts_90k,
                                              int *has_dts, int64_t *dts_90k);

MSMEDIA_API void ms_event_trim_default(ms_event_trim *out);

/* 在 release 之前查询该 AU 的 PCM 修剪；非音频或无元数据时两端为 0。 */
MSMEDIA_API ms_status ms_session_event_trim(const ms_session *session,
                                            uint32_t release_id,
                                            ms_event_trim *out);

MSMEDIA_API const char *ms_codec_name(ms_codec codec);
MSMEDIA_API const char *ms_recog_level_name(ms_recog_level level);
MSMEDIA_API const char *ms_format_name(ms_format format);
MSMEDIA_API const char *ms_sample_format_name(ms_sample_format fmt);

/*
 * 解析本构建支持的 AAC AudioSpecificConfig（LC，AOT=2）。
 * 复用内部 ASC 校验，不是软解，不解释压缩载荷。
 * 加法 ABI，不改变既有符号。
 */
typedef struct ms_aac_asc_info {
    size_t struct_size;
    int audio_object_type;
    int sample_rate;
    int channels;
} ms_aac_asc_info;

MSMEDIA_API ms_status ms_aac_asc_parse(const uint8_t *data, size_t size,
                                       ms_aac_asc_info *out);

/*
 * 从 Annex B、AVCC 样本或 avcC 配置中提取本缓冲内的 SPS/PPS。
 * 仅当同一缓冲内成对且 PPS.sps_id 等于该 SPS.id 时 pair_complete=1。
 * 不与调用方旧参数集拼接。不是软解，不解释图像载荷。
 * is_csd=1 表示配置（avcC 或纯参数集）；0 表示访问单元（AVCC 为 length-prefixed）。
 * nal_len_size：AVCC 样本长度字段字节数，0 视为 4；avcC 配置由盒内字段决定。
 */
#ifndef MS_H264_PS_NAL_MAX
#define MS_H264_PS_NAL_MAX 2048
#endif
#ifndef MS_H264_PS_SLOT_MAX
#define MS_H264_PS_SLOT_MAX 4
#endif

typedef struct ms_h264_ps_set {
    size_t struct_size;
    uint8_t sps[MS_H264_PS_NAL_MAX];
    size_t sps_size;
    uint8_t pps[MS_H264_PS_NAL_MAX];
    size_t pps_size;
    unsigned sps_id;
    unsigned pps_id;
    unsigned pps_sps_id;
    int have_sps;
    int have_pps;
    int pair_complete;
} ms_h264_ps_set;

MSMEDIA_API ms_status ms_h264_ps_parse(ms_sample_format fmt, int is_csd,
                                       const uint8_t *data, size_t size,
                                       unsigned nal_len_size,
                                       ms_h264_ps_set *out);
MSMEDIA_API int ms_h264_ps_equal(const ms_h264_ps_set *a, const ms_h264_ps_set *b);
MSMEDIA_API ms_status ms_h264_ps_pack(ms_sample_format fmt, unsigned nal_len_size,
                                      const ms_h264_ps_set *in, uint8_t *out,
                                      size_t cap, size_t *out_size);
/*
 * 从 avcC / Annex B 配置或含 SPS 的样本取出显示宽高（含 frame_crop）。
 * 有 SPS 但尚不能判断尺寸时输出宽高为 0 并返回 OK。
 * 加法 ABI，不修改 ms_event / ms_h264_ps_set 布局。
 */
MSMEDIA_API ms_status ms_h264_csd_video_size(ms_sample_format fmt, int is_csd,
                                             const uint8_t *data, size_t size,
                                             unsigned nal_len_size,
                                             int *width, int *height);

/*
 * 从 Annex B、HVCC 样本或 hvcC 配置中提取本缓冲内的 VPS/SPS/PPS。
 * 仅当同一缓冲内三者齐全且 SPS.vps_id、PPS.sps_id 与之匹配时 triple_complete=1。
 * 不与调用方旧参数集拼接。不是软解，不解释图像载荷。
 * is_csd=1 表示配置（hvcC 或纯参数集）；0 表示访问单元（HVCC 为 length-prefixed）。
 */
#ifndef MS_HEVC_PS_NAL_MAX
#define MS_HEVC_PS_NAL_MAX 2048
#endif
#ifndef MS_HEVC_PS_SLOT_MAX
#define MS_HEVC_PS_SLOT_MAX 4
#endif

typedef struct ms_hevc_ps_set {
    size_t struct_size;
    uint8_t vps[MS_HEVC_PS_NAL_MAX];
    size_t vps_size;
    uint8_t sps[MS_HEVC_PS_NAL_MAX];
    size_t sps_size;
    uint8_t pps[MS_HEVC_PS_NAL_MAX];
    size_t pps_size;
    unsigned vps_id;
    unsigned sps_id;
    unsigned sps_vps_id;
    unsigned pps_id;
    unsigned pps_sps_id;
    int have_vps;
    int have_sps;
    int have_pps;
    int triple_complete;
} ms_hevc_ps_set;

typedef struct ms_hevc_au_info {
    size_t struct_size;
    int has_vcl;
    int is_irap; /* BLA/IDR/CRA 等，可作为关键帧门闩 */
    int has_vps;
    int has_sps;
    int has_pps;
    unsigned nal_count;
} ms_hevc_au_info;

MSMEDIA_API ms_status ms_hevc_ps_parse(ms_sample_format fmt, int is_csd,
                                       const uint8_t *data, size_t size,
                                       unsigned nal_len_size,
                                       ms_hevc_ps_set *out);
MSMEDIA_API int ms_hevc_ps_equal(const ms_hevc_ps_set *a, const ms_hevc_ps_set *b);
MSMEDIA_API ms_status ms_hevc_ps_pack(ms_sample_format fmt, unsigned nal_len_size,
                                      const ms_hevc_ps_set *in, uint8_t *out,
                                      size_t cap, size_t *out_size);
MSMEDIA_API ms_status ms_hevc_au_inspect(ms_sample_format fmt, const uint8_t *data,
                                         size_t size, unsigned nal_len_size,
                                         ms_hevc_au_info *out);
MSMEDIA_API int ms_hevc_nal_is_irap(unsigned nal_type);

/*
 * 通用视频配置 / 完整 AU / 关键帧判断（加法 ABI）。
 * MPEG-2 / MPEG-4 Part 2 / H.263 / AV1 走此模型；H.264/HEVC 仍用既有 API。
 * H.263：complete 可在 csd_size=0 时成立（宽高来自 PSC/PTYPE），不得捏造 csd-0。
 * AV1：合法 av1C 原样作 CSD；仅有帧内 sequence header 时 complete 可在 csd_size=0 成立，不得捏造 av1C。
 * 不是软解，不栅格化图像。核心自写 TS 组帧仍未覆盖这些编码。
 * by: Long <mycjzb@gmail.com>
 */
#ifndef MS_VIDEO_CSD_MAX
#define MS_VIDEO_CSD_MAX 4096
#endif

typedef enum ms_vpic_type {
    MS_VPIC_UNKNOWN = 0,
    MS_VPIC_I = 1,
    MS_VPIC_P = 2,
    MS_VPIC_B = 3,
    MS_VPIC_S = 4
} ms_vpic_type;

typedef struct ms_video_cfg {
    size_t struct_size;
    ms_codec codec;
    ms_sample_format sample_fmt;
    int complete;       /* 已收集到可配置硬解的参数（MPEG-2 序列头+扩展，或 MPEG-4 VOL） */
    int width;
    int height;
    uint32_t payload_sig; /* 仅对配置载荷做签名，用于中途变化；不是包装哈希 */
    uint8_t csd[MS_VIDEO_CSD_MAX];
    size_t csd_size;
} ms_video_cfg;

typedef struct ms_video_au_info {
    size_t struct_size;
    int is_key;
    int has_cfg;
    ms_vpic_type pic_type;
} ms_video_au_info;

/* 本构建已接线到统一 player/硬解查询-配置-提交路径的视频编码（不是设备已验收）。 */
MSMEDIA_API int ms_video_hw_path_ready(ms_codec codec);
MSMEDIA_API void ms_video_cfg_default(ms_video_cfg *out);
/*
 * 从配置缓冲或 AU 中提取序列/VOL 等参数。is_csd=1 表示 CODEC_CONFIG。
 * 超长配置返回 OVERFLOW 且 complete=0。截断/畸形返回 MALFORMED 或 complete=0。
 * 旧调用方须设置 struct_size。
 */
MSMEDIA_API ms_status ms_video_cfg_parse(ms_codec codec, ms_sample_format fmt,
                                         int is_csd, const uint8_t *data, size_t size,
                                         ms_video_cfg *out);
MSMEDIA_API int ms_video_cfg_equal(const ms_video_cfg *a, const ms_video_cfg *b);
MSMEDIA_API ms_status ms_video_au_inspect(ms_codec codec, ms_sample_format fmt,
                                          const uint8_t *data, size_t size,
                                          ms_video_au_info *out);

/*
 * 通用音频配置 / 完整压缩帧检查 / 签名（加法 ABI）。
 * AAC 仍走既有 ms_aac_asc_parse；AC-3 / MP2 / MP3 合法无 CSD（配置在帧头）；
 * Opus CSD 只接受真实 OpusHead 或把真实 MP4 dOps 装箱为 OpusHead，不得捏造字段。
 * 本层不是软解，不输出 PCM。
 */
#define MS_AUDIO_CSD_MAX 256

typedef struct ms_audio_cfg {
    size_t struct_size;
    ms_codec codec;
    ms_sample_format sample_fmt;
    int complete;
    int sample_rate;
    int channels;
    uint32_t samples_per_frame;
    uint32_t pre_skip;       /* Opus 在 48 kHz 的 codec delay / pre-skip；其它为 0 */
    uint32_t codec_delay;
    uint32_t seek_preroll;   /* 任意位置 seek 后的解码预热样点；Opus 规范默认 80ms=3840；旧 struct_size 看不到 */
    uint32_t payload_sig;
    size_t csd_size;         /* 0 表示合法无 CSD 或尚未得到真实头 */
    uint8_t csd[MS_AUDIO_CSD_MAX];
} ms_audio_cfg;

typedef struct ms_audio_au_info {
    size_t struct_size;
    int has_cfg;             /* 本 AU / 缓冲能抽出配置（含合法无 CSD 的帧头） */
    size_t frame_bytes;      /* 完整帧字节；截断时仍给出规范帧长 */
    uint32_t samples;        /* 本帧样点数（AC-3=1536；Opus 由 TOC；MPEG 按层/版本） */
    int sample_rate;
    int channels;
} ms_audio_au_info;

MSMEDIA_API void ms_audio_cfg_default(ms_audio_cfg *out);
/* 该编码是否已由本库接到统一硬解/player 路径（不是设备上一定有硬件）。 */
MSMEDIA_API int ms_audio_hw_path_ready(ms_codec codec);
MSMEDIA_API int ms_audio_cfg_equal(const ms_audio_cfg *a, const ms_audio_cfg *b);
MSMEDIA_API ms_status ms_audio_cfg_parse(ms_codec codec, ms_sample_format fmt, int is_csd,
                                         const uint8_t *data, size_t size, ms_audio_cfg *out);
MSMEDIA_API ms_status ms_audio_au_inspect(ms_codec codec, ms_sample_format fmt,
                                          const uint8_t *data, size_t size,
                                          ms_audio_au_info *out);

/*
 * 有界 MPEG-TS 前缀识别与剥离（含 PNG/JPEG 头伪装）。
 * 在不超过 max_scan 的窗口内寻找连续同步（默认 3 包，188 优先于 192）。
 * 只报告偏移，不复制后续码流。扫描超限或 EOS 仍无同步返回 UNKNOWN。
 * 需要更多字节返回 AGAIN。这不是格式确认后的重同步，也不是软解。
 */
typedef enum ms_ts_prefix_kind {
    MS_TS_PREFIX_NONE = 0,
    MS_TS_PREFIX_PNG,
    MS_TS_PREFIX_JPEG,
    MS_TS_PREFIX_OTHER
} ms_ts_prefix_kind;

typedef struct ms_ts_scan_opt {
    size_t struct_size;
    size_t max_scan; /* 0=默认 1 MiB，不得超过绝对探测上限 8 MiB */
} ms_ts_scan_opt;

typedef struct ms_ts_scan_result {
    size_t struct_size;
    size_t ts_offset;       /* 接受的同步起点（指向 0x47） */
    uint16_t packet_size;   /* 188 或 192 */
    ms_ts_prefix_kind kind;
    size_t skipped;         /* 与 ts_offset 相同，已跳过的前缀字节 */
    size_t scanned;         /* 本次检查的字节数 */
    size_t keep_from;       /* 未确认时可安全丢弃 [0, keep_from)；其余须保留 */
} ms_ts_scan_result;

MSMEDIA_API void ms_ts_scan_opt_default(ms_ts_scan_opt *out);
MSMEDIA_API const char *ms_ts_prefix_kind_name(ms_ts_prefix_kind kind);
MSMEDIA_API ms_status ms_ts_scan(const uint8_t *data, size_t size, int eos,
                                 const ms_ts_scan_opt *opt,
                                 ms_ts_scan_result *out);


MSMEDIA_API void ms_session_limits_default(ms_session_limits *out);
MSMEDIA_API void ms_session_limits_absolute(ms_session_limits *out);

/*
 * 查询本构建对某个目标 codec 实际达到的最高识别级别。
 * 枚举里有名字但这里返回 NONE，表示本构建不会处理。
 */
MSMEDIA_API ms_status ms_codec_query(ms_codec codec, ms_recog_level *out_level);

/*
 * 平台硬解查询。本构建没有平台后端，因此 is_hardware 与 verified
 * 一定为 0，backend 为 "none"。不得把本调用的结果写成硬解成功。
 */
MSMEDIA_API ms_status ms_hw_query(ms_codec codec,
                      int *is_hardware,
                      int *verified,
                      const char **backend,
                      const char **detail);

MSMEDIA_API ms_status ms_session_create(const ms_allocator *alloc,
                            const ms_session_limits *limits,
                            ms_session **out);
MSMEDIA_API void ms_session_destroy(ms_session *session);

/*
 * 将输入复制进会话并尽可能解析。
 * OK：size 字节全部复制（consumed == size）。
 * LIMIT：缓冲或输出队列已满，consumed 为已复制前缀，其余稍后重试；
 *        若完全无法再收，consumed 为 0。
 * 单次 size 超过 max_push_bytes：INVALID_ARG，consumed 为 0。
 */
MSMEDIA_API ms_status ms_session_push(ms_session *session,
                          const uint8_t *data,
                          size_t size,
                          size_t *consumed);

MSMEDIA_API ms_status ms_session_pull(ms_session *session, ms_event *out);
MSMEDIA_API ms_status ms_session_release(ms_session *session, uint32_t release_id);

/* 丢掉未完成输入和未取走/未释放的输出，保留已确认的格式与轨道。不增加 session_epoch。 */
MSMEDIA_API ms_status ms_session_flush(ms_session *session);

/* 回到探测前状态，增加 session_epoch，旧事件指针全部失效。 */
MSMEDIA_API ms_status ms_session_reset(ms_session *session);

/* 之后的 push/pull 返回 CANCELED，直到 reset。 */
MSMEDIA_API ms_status ms_session_cancel(ms_session *session);

/* 标记输入结束。先交出已完整事件，不完整 PES/section 丢弃，再发出 EOS。 */
MSMEDIA_API ms_status ms_session_end_of_input(ms_session *session);

/*
 * 清单或传输不连续：丢弃未完成装配（不清空已入队事件），发出
 * DISCONTINUITY，并增加 timeline_epoch。不修改用户解码器状态。
 */
MSMEDIA_API ms_status ms_session_signal_discontinuity(ms_session *session);

MSMEDIA_API ms_status ms_session_epochs(const ms_session *session, ms_epochs *out);

/*
 * 复制计数与启动阶段观测。struct_size 必须先填 sizeof(*out)；
 * 只识别不大于本构建 sizeof 的前缀，旧调用方不调用本函数不受影响。
 *
 * 阶段字节是会话累计输入（各次 push 的 consumed 之和）在首次到达
 * 该事件时的快照：FORMAT → TRACK → CODEC_CONFIG → 可独立解码 AU
 * （is_keyframe，配置已先发出）以及首个视频关键 AU。这是解析就绪，
 * 不是屏幕首画面，也不含网络 RTT 或硬解 submit（本构建无硬解）。
 *
 * copy_in：输入窗拷贝；copy_asm：跨包/跨 tag 组帧必要拷贝；
 * copy_out：入队时仍 memcpy 的 AU/config；move_out：完整样本所有权
 * 移交、无载荷 memcpy。baseline_out_bytes = copy_out + move_out，
 * 即若未做移交时出队拷贝的对照值。不宣称全链路零拷贝。
 *
 * 未取走事件的指针在 flush / reset / seek / destroy 之后失效，
 * 不能活过会话。cancel 不释放已 pull 的事件，destroy 会释放。
 */
typedef struct ms_session_stats {
    size_t struct_size;
    uint64_t bytes_in;
    uint64_t copy_in_ops;
    uint64_t copy_in_bytes;
    uint64_t copy_asm_ops;
    uint64_t copy_asm_bytes;
    uint64_t copy_out_ops;
    uint64_t copy_out_bytes;
    uint64_t move_out_ops;
    uint64_t move_out_bytes;
    uint64_t baseline_out_bytes;
    int has_format;
    int has_track;
    int has_config;
    int has_key_au;
    int has_video_key_au;
    uint64_t bytes_at_format;
    uint64_t bytes_at_track;
    uint64_t bytes_at_config;
    uint64_t bytes_at_key_au;
    uint64_t bytes_at_video_key_au;
    int has_seek_land;
    int64_t seek_request_90k;
    int64_t seek_land_90k;
    /* 追加：TS 前缀剥离（旧调用方用更小 struct_size 看不到） */
    uint64_t ts_prefix_bytes;
    int ts_prefix_kind; /* ms_ts_prefix_kind */
} ms_session_stats;

MSMEDIA_API ms_status ms_session_get_stats(const ms_session *session,
                                           ms_session_stats *out);

/*
 * Source：由调用方提供的随机读插件。核心只在会话线程里回调，且一次
 * 回调不得再进入同一会话（包括 push/pull/drive/seek/destroy）。
 * 函数指针在 set_source 时被拷贝；ctx 所有权仍在调用方。
 *
 * size_fn：成功时写出确定大小。AGAIN 表示此刻未知（可稍后重试）。
 *          NOT_SEEKABLE 表示没有大小（只能顺序读，不能跳过尾 moov）。
 * read_at_fn：从绝对偏移读入调用方缓冲区（仅在本次调用内有效，源不得
 *          保留该指针）。允许短读；*out_n 为本次实际字节。偏移处已无
 *          数据时 *out_n=0 且返回 OK（EOF）。会阻塞时返回 AGAIN 且
 *          *out_n=0，核心不会在内部忙等。错误/CANCELED 按状态返回。
 * cancel_fn：可选。会话 cancel 时调用，使随后的 read_at/size 返回
 *          CANCELED。可为空。
 *
 * 旧 push/pull 仍可用；未挂 Source 时行为与从前一致。挂上之后也仍可
 * push，但时间定位与跳过尾 mdat 只走 drive/read_at。
 */
typedef struct ms_source {
    size_t struct_size;
    void *ctx;
    ms_status (*size_fn)(void *ctx, uint64_t *out_size);
    ms_status (*read_at_fn)(void *ctx, uint64_t offset, uint8_t *buf,
                            size_t len, size_t *out_n);
    void (*cancel_fn)(void *ctx);
} ms_source;

MSMEDIA_API ms_status ms_session_set_source(ms_session *session,
                                            const ms_source *src);

/*
 * 从已挂 Source 推进一步：最多一次 read_at，读取长度有界
 *（不超过 max_push_bytes，样本不超过 max_assembly_bytes）。
 * 不在内部循环等待。AGAIN=源暂时没有字节；LIMIT=反压，pull/release
 * 后重试；EOS=源已结束且已完整的事件已可取。
 */
MSMEDIA_API ms_status ms_session_drive(ms_session *session);

/*
 * 内存源（开发适配器）：包装调用方已有缓冲，不拷贝载荷。
 * 可注入短读、AGAIN、失败，供测试；不是网络客户端。
 */
typedef struct ms_mem_source {
    size_t struct_size;
    const uint8_t *data;
    size_t size;
    size_t max_read;  /* 0 表示不限制 */
    int again_left;   /* 随后若干次 read_at 返回 AGAIN */
    int fail_after;   /* -1 永不失败；0 下一次失败；>0 再成功若干次后失败 */
    ms_status fail_st;
    int nreads;
    int canceled;
} ms_mem_source;

MSMEDIA_API void ms_mem_source_init(ms_mem_source *m, const uint8_t *data,
                                    size_t size);
MSMEDIA_API void ms_mem_source_bind(ms_mem_source *m, ms_source *out);

/*
 * 本地文件源（开发适配器）：POSIX open/pread/fstat。可能阻塞在磁盘
 * I/O，但不在核心里重试或等待网络。不是 HTTP/RTMP 客户端。
 */
typedef struct ms_file_source {
    size_t struct_size;
    int fd;
    uint64_t size;
    int canceled;
} ms_file_source;

MSMEDIA_API ms_status ms_file_source_open(ms_file_source *f, const char *path);
MSMEDIA_API void ms_file_source_close(ms_file_source *f);
MSMEDIA_API void ms_file_source_bind(ms_file_source *f, ms_source *out);

/*
 * 时间定位：仅渐进 MP4/MOV，且已挂可随机读 Source、sample 表已定稿。
 * time_90k 是请求时间（90 kHz 媒体时间线，与 PTS 同一轴，不含 elst）。
 * 视频落到目标之前（含等于）最近的可独立解码同步样本；没有 stss 时
 * 每个样本都视为可同步。不伪造 IDR。音频从落地时间对应的首个样本
 * 开始输出，目标时间之前的音频裁剪由控制层做；目标之前的视频 AU
 * 带 preroll=1，显示丢弃也由控制层做。成功则增加 timeline_epoch，
 * flush 旧队列与 outstanding 事件指针，并重发 CODEC_CONFIG。
 * 失败（含尚未定稿返回 AGAIN、非本子集返回 NOT_SEEKABLE、非法时间
 * 返回 INVALID_ARG）不改 epoch、不 flush。
 * fMP4 / FLV / MPEG-TS 任意时间定位本步返回 NOT_SEEKABLE，不是桩成功。
 * set_rate 只记录有理数（倍速听感本切片不做）。
 * select_track：枚举到的音轨/文本字幕可选择或关闭（on=0）。
 * 同 kind 同时只选一条；成功且状态变化时增加 selection_epoch，
 * flush 队列中旧轨的 AU/CONFIG/SUBTITLE（不拆视频），并在有缓存时
 * 为新音轨重发 CODEC_CONFIG。样本仍在当前字节流时不重开 Source。
 * HLS 备选 rendition（另一 URL）本切片仍返回 UNSUPPORTED。
 * 位图/ASS 字幕可出现在列表中，但 on=1 返回 UNSUPPORTED。
 * 未知 id 为 INVALID_ARG。旧调用方仍链同一符号。
 */
MSMEDIA_API ms_status ms_session_seek(ms_session *session, int64_t time_90k);
MSMEDIA_API ms_status ms_session_set_rate(ms_session *session, int num, int den);
MSMEDIA_API ms_status ms_session_select_track(ms_session *session, uint32_t track_id, int on);

/*
 * HLS media playlist 调度（P3 首步可验证子集）。
 * 只决定请求哪一份清单/MAP/分片，并把读到的字节 push 进已绑定会话。
 * HTTP/TLS 由调用方 fetch 外挂执行；本模块不链接 curl，也不忙等。
 * 支持：M3U8 media playlist 的 VOD（ENDLIST）与无 ENDLIST 的直播窗口、
 * MEDIA-SEQUENCE、EXTINF 时长、RFC 3986 基址/相对/query 解析、有界刷新
 * 去重、EXT-X-DISCONTINUITY、EXT-X-MAP（fMP4 init）。
 * 不做：master 变体选择（见到 STREAM-INF/EXT-X-MEDIA 等明确 UNSUPPORTED）、
 * EXT-X-KEY（METHOD 非 NONE）/DRM、LL-HLS（PART/PRELOAD-HINT/SKIP 等）、
 * 替代音轨/字幕、ABR。不把密文当媒体。缺失分片或序号跳跃不会把残缺
 * 装配与下一片简单拼接，而是不连续处理。
 *
 * ms_hls_drive 每次最多发起或读取一个 fetch，或把已读字节有界地推进
 * 会话。AGAIN=外挂暂时没有字节或直播等待刷新；LIMIT=会话反压，
 * pull/release 后重试；EOS=VOD 已结束且会话 end_of_input 已调用。
 * 回调不得重入本调度器或所绑定会话的 API。同一对象同一时刻只一个线程。
 */
typedef struct ms_hls ms_hls;

typedef struct ms_hls_limits {
    size_t struct_size;
    size_t max_playlist_bytes; /* 0=默认 256 KiB */
    size_t max_segments;       /* 0=默认 1024，仅当前窗口 */
    size_t max_uri_bytes;      /* 0=默认 4096 */
    size_t read_chunk_bytes;   /* 0=默认 16 KiB */
    int max_retries;           /* <0 默认 2（共 3 次尝试） */
    size_t max_bytes;          /* 0=默认：当前分量上限合计（含对象与 hold）；绝对值 4 MiB */
} ms_hls_limits;

MSMEDIA_API void ms_hls_limits_default(ms_hls_limits *out);

typedef struct ms_hls_request {
    size_t struct_size;
    const char *uri;     /* 已解析的绝对 URI / 路径，作为 fetch 键 */
    uint64_t media_seq;
    int has_seq;
    int is_playlist;
    int is_init;
    uint64_t byte_off;   /* BYTERANGE 起点；无则 0 */
    uint64_t byte_len;   /* 0 表示直到 EOF */
} ms_hls_request;

typedef struct ms_hls_fetch {
    size_t struct_size;
    void *ctx;
    ms_status (*request_fn)(void *ctx, const ms_hls_request *req,
                            uint32_t *out_id);
    ms_status (*read_fn)(void *ctx, uint32_t id, uint8_t *buf, size_t len,
                         size_t *out_n);
    void (*cancel_fn)(void *ctx, uint32_t id);
} ms_hls_fetch;

MSMEDIA_API ms_status ms_hls_create(const ms_allocator *alloc,
                                    const ms_hls_limits *limits,
                                    ms_hls **out);
MSMEDIA_API void ms_hls_destroy(ms_hls *h);
MSMEDIA_API ms_status ms_hls_bind(ms_hls *h, ms_session *session,
                                  const ms_hls_fetch *fetch);
MSMEDIA_API ms_status ms_hls_open(ms_hls *h, const char *playlist_uri);
MSMEDIA_API ms_status ms_hls_drive(ms_hls *h);
MSMEDIA_API ms_status ms_hls_cancel(ms_hls *h);

/* 按 RFC 3986 解析相对引用；去掉 fragment。out 由调用方提供。 */
MSMEDIA_API ms_status ms_hls_resolve_uri(const char *base, const char *ref,
                                         char *out, size_t cap);

/*
 * 内存 fetch 开发适配器：按解析后的 URI 键查找调用方缓冲，不拷贝载荷。
 * 可注入短读 / AGAIN / 失败。不是 HTTP 客户端。
 */
typedef struct ms_hls_mem_item {
    const char *uri;
    const uint8_t *data;
    size_t size;
} ms_hls_mem_item;

typedef struct ms_hls_mem_fetch {
    size_t struct_size;
    const ms_hls_mem_item *items;
    size_t nitems;
    size_t max_read;
    int again_left;
    int fail_after; /* -1 永不失败 */
    ms_status fail_st;
    int nreads;
    int nrequests;
    int canceled;
    uint32_t live_id;
    int live_used;
    size_t live_off;
    size_t live_end;
    size_t live_idx;
} ms_hls_mem_fetch;

MSMEDIA_API void ms_hls_mem_fetch_init(ms_hls_mem_fetch *m,
                                       const ms_hls_mem_item *items,
                                       size_t nitems);
MSMEDIA_API void ms_hls_mem_fetch_bind(ms_hls_mem_fetch *m, ms_hls_fetch *out);

/*
 * 本地文件 fetch 开发适配器：URI 为绝对路径或 file:。可能阻塞在磁盘，
 * 但不在调度器里重试网络。不是 HTTP 客户端。
 */
typedef struct ms_hls_file_fetch {
    size_t struct_size;
    size_t max_read;
    int canceled;
    uint32_t live_id;
    int live_fd;
    uint64_t live_off;
    uint64_t live_end;
    int live_used;
    int nrequests;
} ms_hls_file_fetch;

MSMEDIA_API void ms_hls_file_fetch_init(ms_hls_file_fetch *f);
MSMEDIA_API void ms_hls_file_fetch_bind(ms_hls_file_fetch *f, ms_hls_fetch *out);

/*
 * MPEG-DASH MPD 调度（P3 可验证最小子集）。
 * 只决定请求哪一份 MPD / Initialization / 媒体分片，并把字节 push
 * 进已绑定会话。HTTP/TLS 复用调用方提供的 ms_hls_fetch 外挂
 * （含内存/文件 harness 与可选 libmsnet）；本模块不链接 OpenSSL 或 curl。
 *
 * 支持：常见 static/dynamic、单个 Period、单个 AdaptationSet 上的
 * 单个 Representation（同一 Representation 内复用的视频+音频，或单轨）。
 * SegmentTemplate（$Number$ / $Time$ / $RepresentationID$ / $Bandwidth$，
 * 含可选宽度如 $Number%05d$；可选 SegmentTimeline）、SegmentList、
 * SegmentBase（Initialization 范围之后余下同一资源，不解析 sidx）、
 * Initialization + media、可选字节范围、BaseURL 继承、有界动态刷新去重。
 *
 * 明确拒绝（UNSUPPORTED，不把密文当媒体，不做半成品选路）：
 * 多 Representation（ABR）、ContentProtection / 加密、字幕或交替轨、
 * 音视频分在两个 AdaptationSet（各带独立 moov；会话 fMP4 只接受一份
 * 初始化段）、xlink 远程外挂、第二个及以后的 Period（不做广告插播/
 * 多 Period 重初始化）、CMAF 低延迟全集、未知 $Identifier$。
 *
 * 调度器自记账内存，max_bytes 有绝对值上限（默认按分量合计，绝对
 * 4 MiB），不占用会话动态预算。XML 只解析所需字段，拒绝 DOCTYPE、
 * 实体声明、外部实体与非预定义实体展开。
 *
 * ms_dash_drive 合同与 ms_hls_drive 相同：每次最多一个 fetch 或有界
 * 推进会话。回调不得重入。同一对象同一时刻只一个线程。
 */
typedef struct ms_dash ms_dash;

typedef struct ms_dash_limits {
    size_t struct_size;
    size_t max_mpd_bytes;     /* 0=默认 256 KiB */
    size_t max_segments;      /* 0=默认 1024，当前窗口 */
    size_t max_uri_bytes;     /* 0=默认 4096 */
    size_t read_chunk_bytes;  /* 0=默认 16 KiB */
    int max_retries;          /* <0 默认 2（共 3 次尝试） */
    size_t max_bytes;         /* 0=默认分量合计；绝对值 4 MiB */
} ms_dash_limits;

MSMEDIA_API void ms_dash_limits_default(ms_dash_limits *out);

MSMEDIA_API ms_status ms_dash_create(const ms_allocator *alloc,
                                     const ms_dash_limits *limits,
                                     ms_dash **out);
MSMEDIA_API void ms_dash_destroy(ms_dash *d);
MSMEDIA_API ms_status ms_dash_bind(ms_dash *d, ms_session *session,
                                   const ms_hls_fetch *fetch);
MSMEDIA_API ms_status ms_dash_open(ms_dash *d, const char *mpd_uri);
MSMEDIA_API ms_status ms_dash_drive(ms_dash *d);
MSMEDIA_API ms_status ms_dash_cancel(ms_dash *d);


/*
 * P4 音轨枚举与文本字幕（加法 ABI，MSMEDIA_ABI_VERSION 仍为 1）。
 * 外挂 SRT / WebVTT：本模块有界解析，不走 FFmpeg 解码器。
 * 内嵌 tx3g 仅提取 UTF-8/UTF-16 文本，忽略样式盒。
 * ASS/SSA：有界解析 Format/Style/Dialogue、常见覆盖标签与布局字段，
 * 进入同一 cue 时间轴；不是完整 ASS 渲染器（无字体栅格、无动画/绘图合成）。
 * 位图字幕：DVB（ETSI EN 300 743）与 PGS（Blu-ray）解析为有界 RGBA cue，
 * 并接入同一时间轴 / seek / flush；VobSub（idx+sub）若实现则同样走该路径。
 * XSUB / 图文电视等仍为族内不可播。不是 FFmpeg 软解码器，不执行脚本。
 * 完整 ASS 渲染（字体/动画/绘图合成）仍未做，不得把去标签文本称为完整渲染。
 * by: Long <mycjzb@gmail.com>
 */
#ifndef MS_SUB_CUE_TEXT_MAX
#define MS_SUB_CUE_TEXT_MAX 1024
#endif
#ifndef MS_SUB_CUE_MAX
#define MS_SUB_CUE_MAX 2048
#endif
#ifndef MS_SUB_DOC_BYTES_MAX
#define MS_SUB_DOC_BYTES_MAX 262144
#endif
#ifndef MS_SUB_STYLE_NAME_MAX
#define MS_SUB_STYLE_NAME_MAX 32
#endif
#ifndef MS_SUB_SPAN_MAX
#define MS_SUB_SPAN_MAX 8
#endif
#ifndef MS_SUB_BITMAP_WIDTH_MAX
#define MS_SUB_BITMAP_WIDTH_MAX 1920
#endif
#ifndef MS_SUB_BITMAP_HEIGHT_MAX
#define MS_SUB_BITMAP_HEIGHT_MAX 1080
#endif
#ifndef MS_SUB_BITMAP_BYTES_MAX
#define MS_SUB_BITMAP_BYTES_MAX (4u * 1024u * 1024u) /* 单个 doc 的 RGBA 总预算 */
#endif
#ifndef MS_SUB_BITMAP_ACC_MAX
#define MS_SUB_BITMAP_ACC_MAX 65536 /* 跨 PES 组装上限 */
#endif
/* 外挂字幕的虚拟轨道 id（不与 MPEG-TS PID / 流 index 冲突）。 */
#define MS_TRACK_ID_EXTERNAL_SUB 0xFFFF0001u
/* 可选 AI 生成字幕轨（不自动选中，不得覆盖原字幕选择）。 */
#define MS_TRACK_ID_AI_SUB 0xFFFF0002u

typedef enum ms_sub_format {
    MS_SUB_FORMAT_AUTO = 0,
    MS_SUB_FORMAT_SRT = 1,
    MS_SUB_FORMAT_WEBVTT = 2,
    MS_SUB_FORMAT_ASS = 3,  /* 加法枚举，旧值不变 */
    MS_SUB_FORMAT_SSA = 4,
    MS_SUB_FORMAT_DVB = 5,     /* 内嵌 DVB 段；不是外挂纯文本 */
    MS_SUB_FORMAT_PGS = 6,     /* 内嵌 PGS 或 .sup 段 */
    MS_SUB_FORMAT_VOBSUB = 7   /* idx+sub 组合块（见 ms_sub_doc_parse_vobsub） */
} ms_sub_format;

/* 位图种类（cue 加法字段）。0 表示文本 cue，无位图。 */
typedef enum ms_sub_bitmap_kind {
    MS_SUB_BITMAP_NONE = 0,
    MS_SUB_BITMAP_DVB = 1,
    MS_SUB_BITMAP_PGS = 2,
    MS_SUB_BITMAP_VOBSUB = 3
} ms_sub_bitmap_kind;

/* 已解析的一般样式（加法标志，旧调用方不得假设存在）。 */
#define MS_SUB_STYLE_HAS_COLOR  (1u << 0)
#define MS_SUB_STYLE_HAS_SIZE   (1u << 1)
#define MS_SUB_STYLE_HAS_BOLD   (1u << 2)
#define MS_SUB_STYLE_HAS_ITALIC (1u << 3)
#define MS_SUB_STYLE_HAS_ALIGN  (1u << 4)
#define MS_SUB_STYLE_HAS_MARGIN (1u << 5)
#define MS_SUB_STYLE_HAS_POS    (1u << 6)
#define MS_SUB_STYLE_HAS_LAYER  (1u << 7)
#define MS_SUB_STYLE_HAS_NAME    (1u << 8)
#define MS_SUB_STYLE_HAS_BITMAP  (1u << 9)
#define MS_SUB_STYLE_HAS_CROP    (1u << 10)
#define MS_SUB_STYLE_HAS_CANVAS  (1u << 11)

/*
 * 能力/降级标识。出现未支持特效时置位对应位并置 DEGRADED。
 * 不得把仅去标签的纯文本称为完整 ASS 渲染。
 */
#define MS_SUB_CAP_DEGRADED               (1u << 0)
#define MS_SUB_CAP_STRIPPED_TAGS          (1u << 1)
#define MS_SUB_CAP_UNSUPPORTED_MOVE       (1u << 2)
#define MS_SUB_CAP_UNSUPPORTED_TRANSFORM  (1u << 3)
#define MS_SUB_CAP_UNSUPPORTED_FADE       (1u << 4)
#define MS_SUB_CAP_UNSUPPORTED_KARAOKE    (1u << 5)
#define MS_SUB_CAP_UNSUPPORTED_DRAWING    (1u << 6)
#define MS_SUB_CAP_UNSUPPORTED_CLIP       (1u << 7)
#define MS_SUB_CAP_UNSUPPORTED_BLUR       (1u << 8)
#define MS_SUB_CAP_UNSUPPORTED_EFFECT     (1u << 9)
#define MS_SUB_CAP_UNSUPPORTED_FONT       (1u << 10)
#define MS_SUB_CAP_UNSUPPORTED_WRAP       (1u << 11)
#define MS_SUB_CAP_INLINE_SPAN_LIMIT      (1u << 12)
#define MS_SUB_CAP_NO_INLINE_SPANS        (1u << 13)
#define MS_SUB_CAP_AI_GENERATED           (1u << 14)
#define MS_SUB_CAP_AI_PARTIAL             (1u << 15)
#define MS_SUB_CAP_AI_FINAL               (1u << 16)
#define MS_SUB_CAP_AI_TRANSLATION         (1u << 17)
#define MS_SUB_CAP_AI_SIMULATED           (1u << 18)

typedef struct ms_sub_span {
    size_t start; /* 可见 UTF-8 文本中的字节偏移，含 start、不含 end */
    size_t end;
    uint32_t style_flags;
    uint32_t color_argb; /* 0xAARRGGBB，A=0xFF 为不透明 */
    int font_size;
    int bold;
    int italic;
} ms_sub_span;

typedef struct ms_sub_cue {
    size_t struct_size;
    uint32_t track_id;
    int has_start;
    int has_end;
    uint64_t start_90k;
    uint64_t end_90k;
    const char *text; /* UTF-8，已去覆盖标签；所有权在 ms_sub_doc，下一次变更前有效 */
    size_t text_size;
    /* 以下为加法字段。旧调用方 struct_size 停在 text_size 之后则不得读写。 */
    uint32_t style_flags;
    uint32_t cap_flags;
    int layer;
    int alignment; /* ASS \an 1-9；0=未设定（缺省按底中理解，但不虚报 HAS_ALIGN） */
    int font_size;
    uint32_t color_argb;
    int bold;
    int italic;
    int margin_l;
    int margin_r;
    int margin_v;
    int pos_x;
    int pos_y;
    int play_res_x;
    int play_res_y;
    char style_name[MS_SUB_STYLE_NAME_MAX];
    uint32_t span_count;
    ms_sub_span spans[MS_SUB_SPAN_MAX];
    /* 以下为位图加法字段。旧调用方 struct_size 停在 spans 之后则不得读写。
     * bitmap_rgba 为紧密打包的 RGBA8888（每像素 R,G,B,A），所有权在 ms_sub_doc，
     * 下一次变更（clear/add/feed/destroy）前有效。文本字段不得指向这些字节。 */
    int bitmap_kind; /* ms_sub_bitmap_kind */
    int bitmap_w;
    int bitmap_h;
    int crop_x;
    int crop_y;
    int crop_w;
    int crop_h;
    const uint8_t *bitmap_rgba;
    size_t bitmap_rgba_size; /* 应为 w*h*4 */
    /* 以下为可选 AI 生成轨加法字段。旧调用方 struct_size 停在 bitmap 之后则不得读写。 */
    uint32_t ai_utterance_id;
    uint32_t ai_revision;
    uint32_t ai_epoch;
    int ai_kind; /* 1=partial 2=final 3=translation */
    int64_t ai_delay_90k;
    char ai_lang[16];
} ms_sub_cue;

typedef struct ms_sub_doc ms_sub_doc;

MSMEDIA_API void ms_sub_cue_default(ms_sub_cue *out);
MSMEDIA_API ms_status ms_sub_doc_create(const ms_allocator *alloc, ms_sub_doc **out);
MSMEDIA_API void ms_sub_doc_destroy(ms_sub_doc *doc);
/* 清空 cue、样式表与脚本分辨率（解析失败回滚 / destroy 使用）。 */
MSMEDIA_API void ms_sub_doc_clear(ms_sub_doc *doc);
/*
 * 只清空 cue 文本与条目，保留已加载的 ASS 样式表与 PlayRes。
 * 供 seek / 关闭字幕轨使用，避免内嵌事件在重新选中后丢失 Style。
 */
MSMEDIA_API void ms_sub_doc_clear_cues(ms_sub_doc *doc);
MSMEDIA_API ms_sub_format ms_sub_doc_format(const ms_sub_doc *doc);
/* AUTO 没有已解析格式时返回 MS_CODEC_SUB_TEXT。SSA 与 ASS 都映射为 MS_CODEC_SUB_ASS。 */
MSMEDIA_API ms_codec ms_sub_codec_for_format(ms_sub_format fmt);
/*
 * 解析完整 SRT、WebVTT 或 ASS/SSA 文本（可带 UTF-8 BOM）。
 * fmt=AUTO 时先认 WEBVTT 头，再认 ASS/SSA 脚本标记，再试 SRT。
 * 超限（条数/单条/总字节）返回 OVERFLOW 且不保留半成品。
 * 个别坏 cue 跳过；若整份无法识别则 MALFORMED。
 * 忽略 WebVTT STYLE/REGION/NOTE 与箭头后的排版设定，不宣称 WebVTT 样式支持。
 * ASS/SSA：解析 Script Info / Style / Dialogue；忽略 [Fonts]/[Graphics]，不执行其中数据。
 * 得到的是布局模型字段与去标签文本，不是完整渲染。
 */
MSMEDIA_API ms_status ms_sub_doc_parse(ms_sub_doc *doc, const uint8_t *data, size_t size,
                                       ms_sub_format fmt, uint32_t track_id);
/*
 * 将 ASS/SSA 头（Script Info 与 Style，或完整脚本中的头部分）写入 doc。
 * 不清除已有 cue。供容器内嵌：头通常在 extradata / CODEC_CONFIG。
 * 忽略嵌入字体与图形，不执行脚本或命令。
 */
MSMEDIA_API ms_status ms_sub_doc_set_ass_header(ms_sub_doc *doc,
                                                const uint8_t *data, size_t size);
/*
 * 解析一条 Dialogue、Matroska/FFmpeg 压缩事件，或仅含覆盖标签的文本。
 * 事件自带 Start/End 时用之，否则用传入的时间（90 kHz）。
 * 可见文本写入 text_buf（去标签 UTF-8）；out->text 指向 text_buf。
 * 调用方须先设置 out->struct_size（0 视为当前头大小）。
 * 未支持特效只置 cap_flags，不把结果标成完整渲染。
 */
MSMEDIA_API ms_status ms_sub_ass_parse_event(const ms_sub_doc *doc,
                                             const uint8_t *data, size_t size,
                                             int has_start, uint64_t start_90k,
                                             int has_end, uint64_t end_90k,
                                             ms_sub_cue *out, char *text_buf, size_t text_cap);
/*
 * 可选 DVB 头（通常为 FFmpeg extradata：composition_page_id 与 ancillary_page_id，各 16 位大端）。
 * 不清除已有 cue。非法长度忽略页过滤（接受所有页）。
 */
MSMEDIA_API ms_status ms_sub_doc_set_bitmap_header(ms_sub_doc *doc, ms_sub_bitmap_kind kind,
                                                   const uint8_t *data, size_t size);
/*
 * 喂入一包压缩位图字幕（DVB 段、PGS 段或 .sup、DVD 子图）。有状态，跨包组装。
 * 完整显示集结束时追加一条或多条 RGBA cue（位置在 pos_x/pos_y，画布在 play_res_*）。
 * 新显示集/清空会关闭同轨上尚未结束的位图 cue。超限 OVERFLOW 且保留已有条目，
 * 不把旧画面当作新显示集的结果。ABI 不变。
 * 组装覆盖：DVB 跨 PES（每 PES 的 0x20 0x00 前缀在续包时剥除）与段中部切分；
 * PGS ODS first/continuation/last 与多段显示集。seek/clear/切轨清空有界缓存。
 * 畸形/截断/超大图幅/调色板缺失：跳过该集或该对象，返回 MALFORMED 或 OK（已跳过），不崩溃。
 * 不执行脚本。time 为 90 kHz。
 */
MSMEDIA_API ms_status ms_sub_doc_feed_bitmap(ms_sub_doc *doc, ms_sub_bitmap_kind kind,
                                             uint32_t track_id,
                                             const uint8_t *data, size_t size,
                                             int has_start, uint64_t start_90k,
                                             int has_end, uint64_t end_90k);
/*
 * 解析 VobSub：idx 文本 + sub 字节。成功后 cue 已在 doc 中。
 * 也可以把组合块交给 ms_sub_doc_parse(fmt=VOBSUB)：魔数 "MSVB" + u32le idx_len + idx + sub。
 */
MSMEDIA_API ms_status ms_sub_doc_parse_vobsub(ms_sub_doc *doc,
                                              const uint8_t *idx, size_t idx_size,
                                              const uint8_t *sub, size_t sub_size,
                                              uint32_t track_id);
/* 追加一条 cue（内嵌路径）。超限 OVERFLOW，已有条目保留。只拷贝 in->struct_size。
 * 允许无文本的位图 cue（须带 HAS_BITMAP 与合法 RGBA）。 */
MSMEDIA_API ms_status ms_sub_doc_add_cue(ms_sub_doc *doc, const ms_sub_cue *in);
MSMEDIA_API size_t ms_sub_doc_count(const ms_sub_doc *doc);
MSMEDIA_API ms_status ms_sub_doc_get(const ms_sub_doc *doc, size_t index, ms_sub_cue *out);
/*
 * 取出 start<=time_90k<end 的 cue（无 end 则 start<=t）。
 * 多条重叠按 layer 升序、再按 start 排序。cap=0 只写 count。out 可为 NULL。
 */
MSMEDIA_API ms_status ms_sub_doc_cues_at(const ms_sub_doc *doc, uint64_t time_90k,
                                         ms_sub_cue *out, size_t cap, size_t *count);
/* 从 tx3g/mov_text 样本提取纯文本（长度前缀或整包 UTF-8/UTF-16）。不是样式排版。 */
MSMEDIA_API ms_status ms_sub_tx3g_text(const uint8_t *data, size_t size,
                                       char *out, size_t cap, size_t *out_size);

/*
 * 列出本会话已发现的轨道（含未选中与外挂字幕）。
 * language/name 指针在下一次 push/select/reset 前有效。
 */
MSMEDIA_API ms_status ms_session_list_tracks(const ms_session *session,
                                             ms_track_info *out, size_t cap, size_t *count);
/*
 * 加载或清除外挂 SRT/WebVTT/ASS/SSA（data=NULL 或 size=0 为清除）。
 * 成功后出现 id=MS_TRACK_ID_EXTERNAL_SUB 的字幕轨，默认不自动选中。
 * 不下载网络外挂（由调用方读入字节）。
 */
MSMEDIA_API ms_status ms_session_set_external_subtitle(ms_session *session,
                                                       const uint8_t *data, size_t size,
                                                       ms_sub_format fmt);
/* 按媒体时间（90 kHz，与 PTS 同轴）查询当前选中字幕的活动 cue。 */
MSMEDIA_API ms_status ms_session_subtitles_at(const ms_session *session, uint64_t time_90k,
                                              ms_sub_cue *out, size_t cap, size_t *count);


/*
 * HDR 识别（加法 ABI，版本仍为 1）。不写入 ms_event / ms_track_info，以免破坏
 * 1838 旧头 176 字节布局。从 HEVC VUI/SEI 或调用方合并的容器信息得到。
 * 识别与透传不是显示器 HDR 效果验收。
 * by: Long <mycjzb@gmail.com>
 */
#ifndef MS_HDR_STATIC_INFO_SIZE
#define MS_HDR_STATIC_INFO_SIZE 25
#endif

typedef enum ms_hdr_kind {
    MS_HDR_KIND_UNKNOWN = 0,
    MS_HDR_KIND_SDR = 1,
    MS_HDR_KIND_HDR10 = 2,
    MS_HDR_KIND_HLG = 3
} ms_hdr_kind;

typedef struct ms_hdr_info {
    size_t struct_size;
    ms_hdr_kind kind;
    int have_color;          /* 已有 primaries/transfer/matrix 之一 */
    int color_primaries;     /* H.265 Table E.3；9=BT.2020；0=未给出 */
    int color_transfer;      /* 16=PQ (ST 2084)，18=HLG；0=未给出 */
    int color_matrix;        /* 9=BT.2020 NCL，10=BT.2020 CL；0=未给出 */
    int color_range;         /* 0=未指定，1=limited，2=full（与常见 ISO 习惯一致） */
    int bit_depth;           /* luma 位深；0=未知 */
    int chroma_format_idc;
    unsigned profile_idc;    /* HEVC general_profile_idc；2=Main 10 */
    unsigned level_idc;      /* general_level_idc，如 150=5.0 */
    unsigned tier_flag;      /* 0=Main，1=High */
    int have_mastering;
    /* SEI / ST 2086：色度单位 0.00002，亮度单位 0.0001 cd/m^2。
     * 阵列顺序与 HEVC SEI 相同：G、B、R。打包 Android 缓冲时再改成 R、G、B。 */
    uint16_t display_primaries_x[3];
    uint16_t display_primaries_y[3];
    uint16_t white_point_x;
    uint16_t white_point_y;
    uint32_t max_display_mastering_luminance;
    uint32_t min_display_mastering_luminance;
    int have_cll;
    uint16_t max_content_light_level;
    uint16_t max_frame_average_light_level;
    int have_hdr10_plus;     /* 已识别动态元数据；本实现不向 MediaCodec 透传，未验证 */
    int dolby_vision;        /* 已识别；本实现不配置 DV，回落基础层（若可） */
    int from_vui;
    int from_sei;
    int from_container;      /* codecpar / 容器 side data */
    int width;               /* SPS / codecpar 的像素宽，0=未知；供 MediaFormat */
    int height;
} ms_hdr_info;

MSMEDIA_API void ms_hdr_info_default(ms_hdr_info *out);
MSMEDIA_API const char *ms_hdr_kind_name(ms_hdr_kind kind);
/* 把 src 补进 dst：已有字段不覆盖，除非 src 给出更明确的 kind/mastering/CLL。 */
MSMEDIA_API void ms_hdr_merge(ms_hdr_info *dst, const ms_hdr_info *src);
/* 按 transfer 分类：16=HDR10，18=HLG，其余在有颜色信息时为 SDR。 */
MSMEDIA_API void ms_hdr_classify(ms_hdr_info *info);

/*
 * 从 HEVC 配置或访问单元解析 VUI / SEI。支持 Annex B、hvcC 配置、长度前缀 AU。
 * 不是软解。成功时至少填 kind（可能仍为 UNKNOWN）。
 */
MSMEDIA_API ms_status ms_hdr_parse_hevc(ms_sample_format fmt, int is_csd,
                                        const uint8_t *data, size_t size,
                                        ms_hdr_info *out);

/*
 * 打包 Android MediaFormat KEY_HDR_STATIC_INFO（API 24+）所用的 25 字节小端缓冲：
 *  byte0=1（Static Metadata Type 1），随后 RGB 序色度、白点、最大/最小亮度
 *  （最大亮度为 1 cd/m^2 单位的 uint16，最小为 0.0001 cd/m^2 的 uint16）、CLL/FALL。
 *  cap 必须 >= 25。无 mastering 且无 CLL 时返回 MS_AGAIN。
 */
MSMEDIA_API ms_status ms_hdr_pack_static_info(const ms_hdr_info *info,
                                              uint8_t *out, size_t cap, size_t *out_size);

/* 映射到 Android MediaFormat 整型常量（API 24+）。未知返回 0。 */
MSMEDIA_API int ms_hdr_android_color_standard(const ms_hdr_info *info);
MSMEDIA_API int ms_hdr_android_color_transfer(const ms_hdr_info *info);
MSMEDIA_API int ms_hdr_android_color_range(const ms_hdr_info *info);
/* HEVC MediaCodec profile/level；未知为 0。10bit/HDR10/HLG 用 Main10(2)；颜色键表达 PQ/HLG。 */
MSMEDIA_API int ms_hdr_android_hevc_profile(const ms_hdr_info *info);
MSMEDIA_API int ms_hdr_android_hevc_level(const ms_hdr_info *info);

#ifdef __cplusplus
}
#endif

#endif /* MSMEDIA_MSMEDIA_H */
