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
 */

#ifndef MSMEDIA_MSMEDIA_H
#define MSMEDIA_MSMEDIA_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
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
    MS_ERR_NOT_SEEKABLE
} ms_status;

typedef enum ms_format {
    MS_FORMAT_UNKNOWN = 0,
    MS_FORMAT_MPEG_TS = 1
} ms_format;

/*
 * 目标 codec 枚举。出现在枚举中不等于已经实现码流确认或 AU 组帧。
 * 真实级别必须通过 ms_codec_query 或轨道事件里的 level 查询。
 */
typedef enum ms_codec {
    MS_CODEC_UNKNOWN = 0,
    MS_CODEC_H264,
    MS_CODEC_AAC,            /* 目标为 ADTS AAC；LATM 本阶段不处理 */
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
    MS_CODEC_G711_ULAW
} ms_codec;

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
    MS_EVENT_CODEC_CONFIG,     /* AAC ASC，或 H.264 Annex B SPS/PPS */
    MS_EVENT_AU,               /* 完整 ADTS 帧，或本子集 H.264 Annex B AU */
    MS_EVENT_SUBTITLE,         /* 本阶段不会发出 */
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
 * 会话动态总预算 16 MiB。绝对上限：装配 16 MiB、总预算 64 MiB。
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
 * 的轨道 ES 解析状态（H.264/ADTS 对象，含参数集槽位表）、H.264
 * 参数集 NAL 副本，以及调试 PES 载荷（预算不足时调试副本可放弃，
 * 不影响公共 AU 路径）。记账含每块内部头。
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
    /* AU / CODEC_CONFIG。没有时间戳时 has_pts/has_dts 为 0，不用 0 冒充有效时间。 */
    int has_pts;
    int has_dts;
    uint64_t pts;                /* 通常为 90 kHz、33 位 */
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
} ms_event;

typedef struct ms_session ms_session;

uint32_t ms_abi_version(void);

const char *ms_status_name(ms_status st);
const char *ms_codec_name(ms_codec codec);
const char *ms_recog_level_name(ms_recog_level level);

void ms_session_limits_default(ms_session_limits *out);
void ms_session_limits_absolute(ms_session_limits *out);

/*
 * 查询本构建对某个目标 codec 实际达到的最高识别级别。
 * 枚举里有名字但这里返回 NONE，表示本构建不会处理。
 */
ms_status ms_codec_query(ms_codec codec, ms_recog_level *out_level);

/*
 * 平台硬解查询。本构建没有平台后端，因此 is_hardware 与 verified
 * 一定为 0，backend 为 "none"。不得把本调用的结果写成硬解成功。
 */
ms_status ms_hw_query(ms_codec codec,
                      int *is_hardware,
                      int *verified,
                      const char **backend,
                      const char **detail);

ms_status ms_session_create(const ms_allocator *alloc,
                            const ms_session_limits *limits,
                            ms_session **out);
void ms_session_destroy(ms_session *session);

/*
 * 将输入复制进会话并尽可能解析。
 * OK：size 字节全部复制（consumed == size）。
 * LIMIT：缓冲或输出队列已满，consumed 为已复制前缀，其余稍后重试；
 *        若完全无法再收，consumed 为 0。
 * 单次 size 超过 max_push_bytes：INVALID_ARG，consumed 为 0。
 */
ms_status ms_session_push(ms_session *session,
                          const uint8_t *data,
                          size_t size,
                          size_t *consumed);

ms_status ms_session_pull(ms_session *session, ms_event *out);
ms_status ms_session_release(ms_session *session, uint32_t release_id);

/* 丢掉未完成输入和未取走/未释放的输出，保留已确认的格式与轨道。不增加 session_epoch。 */
ms_status ms_session_flush(ms_session *session);

/* 回到探测前状态，增加 session_epoch，旧事件指针全部失效。 */
ms_status ms_session_reset(ms_session *session);

/* 之后的 push/pull 返回 CANCELED，直到 reset。 */
ms_status ms_session_cancel(ms_session *session);

/* 标记输入结束。先交出已完整事件，不完整 PES/section 丢弃，再发出 EOS。 */
ms_status ms_session_end_of_input(ms_session *session);

ms_status ms_session_epochs(const ms_session *session, ms_epochs *out);

/*
 * 以下接口保留在 ABI 中。本阶段：seek 返回 NOT_SEEKABLE；
 * set_rate 只记录有理数，不影响解析；select_track 返回 UNSUPPORTED。
 */
ms_status ms_session_seek(ms_session *session, int64_t time_90k);
ms_status ms_session_set_rate(ms_session *session, int num, int den);
ms_status ms_session_select_track(ms_session *session, uint32_t track_id, int on);

#ifdef __cplusplus
}
#endif

#endif /* MSMEDIA_MSMEDIA_H */
