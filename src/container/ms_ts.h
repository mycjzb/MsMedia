/*
 * MPEG-TS 同步、PAT/PMT 发现，以及可选的 PES 组装。
 * PES 载荷本身不是访问单元。AAC 只在 ADTS 组出完整帧后发 AU；
 * H.264 只在 Annex B 子集组出完整访问单元后发 AU，不把 PES 或单个 NAL 当帧。
 * 轨道表最多 MS_TS_MAX_TRACKS；H.264/ADTS 解析状态按实际使用的轨道惰性分配，
 * 计入会话动态预算，不用则不占固定会话对象。
 * 首次 PMT 前到达的媒体包有界暂存并在 PMT 登记轨道后回放；不把残缺 ADTS/PES 当 AU。
 * by: Long <mycjzb@gmail.com>
 */

#ifndef MSMEDIA_TS_H
#define MSMEDIA_TS_H

#include "ms_internal.h"
#include "ms_adts.h"
#include "ms_h264.h"

#define MS_PES_DONE_MAX 8
/* 首次 PMT 之前暂存的 TS 包（188 字节体），PMT 到达后按序回放。环形有界。 */
#define MS_TS_PRE_PMT_PACKETS 128
#define MS_TS_PRE_PMT_PKT 188

typedef struct ms_ts_callbacks {
    void *ctx;
    /* 以下回调返回 MS_OK 或错误；MS_LIMIT 表示事件队列满，已产生的 AU 不得丢弃。 */
    ms_status (*on_format)(void *ctx, uint16_t packet_size);
    ms_status (*on_track)(void *ctx, const ms_track_info *track, int update);
    /* track 可为 NULL（字节层失同步等无法归属单轨）。 */
    ms_status (*on_discontinuity)(void *ctx, const ms_track_info *track);
    ms_status (*on_codec_config)(void *ctx, const ms_track_info *track,
                                 const uint8_t *data, size_t size);
    ms_status (*on_au)(void *ctx, const ms_track_info *track, const ms_au_frame *fr);
} ms_ts_callbacks;

typedef struct ms_psi_acc {
    uint8_t buf[MS_PSI_MAX_SECTION];
    size_t len;
    size_t need; /* 0 表示还没读到 section_length */
    int active;
    int have_cc;
    uint8_t last_cc;
    int dup_seen;
    uint8_t last_plen;
    uint64_t last_tag;
} ms_psi_acc;

typedef struct ms_pes_acc {
    int in_use;
    uint16_t pid;
    uint8_t *buf;
    size_t len;
    size_t cap;
    size_t need; /* 0 表示长度未知（PES_packet_length=0） */
    int have_cc;
    uint8_t last_cc;
    int dup_seen;          /* 上一载荷包已见一个合法同 CC 重复 */
    uint8_t last_plen;
    uint64_t last_tag;     /* 上一载荷的 FNV-1a，用于区分合法重复与异常 */
} ms_pes_acc;

typedef struct ms_es_pending {
    uint8_t *data;
    size_t len;
    size_t off;
    int begun;
    int has_pts;
    int has_dts;
    uint64_t pts;
    uint64_t dts;
} ms_es_pending;

typedef struct ms_ts_track {
    int in_use;
    uint16_t pid;
    uint8_t stream_type;
    ms_codec codec;
    ms_track_kind kind;
    ms_recog_level level;
    char language[4];
    int selected;
    int announced;
    int level_upgraded;
    /* 按 codec 惰性分配，不用的轨道不占解析状态；指针由 parser->alloc 持有。 */
    ms_adts *adts;
    int adts_inited;
    ms_h264 *h264;
    int h264_inited;
    ms_es_pending pending;
    void *parser; /* 回指 ms_ts_parser，供 ADTS 回调填轨道信息 */
    int pending_disc;     /* 下一完整 AU 的 ev.discontinuity 置 1 */
    int disc_event_open;  /* 本轮不连续已发事件，恢复前不重复刷屏 */
    /* AVS/AVS+（stream_type 0x42）起始码切帧：按图像起始码 B3/B6 切 AU，
     * 序列头 B0 及其扩展/用户数据并入后随的 I 帧 AU；缓冲由 parser->alloc 持有。 */
    uint8_t *avs_au;
    size_t avs_len;
    size_t avs_cap;
    int avs_has_pic;
    int avs_key;
    int avs_has_seq;
    int avs_has_pts;
    int avs_has_dts;
    uint64_t avs_pts;
    uint64_t avs_dts;
    uint32_t avs_cfg_sig;  /* 已上送序列头签名；0=未上送（不连续/flush 后清零以重发） */
} ms_ts_track;

typedef struct ms_pes_done {
    int in_use;
    uint16_t pid;
    ms_codec codec;
    int has_pts;
    int has_dts;
    uint64_t pts;
    uint64_t dts;
    uint8_t *payload;
    size_t payload_size;
} ms_pes_done;

typedef struct ms_ts_parser {
    const ms_allocator *alloc;
    size_t probe_bytes;
    size_t max_resync_bytes;
    size_t max_pes_bytes; /* 单个 PES / ADTS 组装上限；与输出 AU 队列分开 */
    ms_ts_callbacks cb;

    int confirmed;
    int failed;
    ms_status fail_status;
    int format_emitted;
    uint16_t packet_size; /* 188 或 192 */
    size_t resync_spent;
    size_t prefix_bytes;           /* 确认前已跳过的伪装/垃圾前缀 */
    ms_ts_prefix_kind prefix_kind;

    int have_pat;
    int have_pmt;
    uint8_t pat_version;
    uint8_t pmt_version;
    uint16_t program_number;
    uint16_t pmt_pid;

    ms_psi_acc pat;
    ms_psi_acc pmt;

    ms_ts_track tracks[MS_TS_MAX_TRACKS];
    int ntracks;
    int selected_video;
    int selected_audio;

    ms_pes_acc pes[MS_TS_MAX_TRACKS];
    ms_pes_done done[MS_PES_DONE_MAX];

    /* 中途接入：媒体 PES 可能早于 PAT/PMT 到达。有界暂存后回放，计入动态预算。 */
    uint8_t *pre_pmt;
    size_t pre_pmt_cap;
    size_t pre_pmt_count;
    size_t pre_pmt_sent;
    int pre_pmt_replayed;
    int pre_pmt_replaying;
    ms_copy_meter *meter;
    /* 容错模式（追加；默认 0 = 严格合同不变）：坏包/坏帧丢弃并标不连续，不让整路失败。 */
    int tolerant;
    uint64_t tolerated;
} ms_ts_parser;

void ms_ts_parser_init(ms_ts_parser *p,
                       const ms_allocator *alloc,
                       size_t probe_bytes,
                       size_t max_resync_bytes,
                       size_t max_pes_bytes,
                       const ms_ts_callbacks *cb);

void ms_ts_parser_reset(ms_ts_parser *p); /* 释放 PES 缓冲，回到未探测 */
void ms_ts_parser_flush_partial(ms_ts_parser *p); /* 丢未完成 PSI/PES，保留 PAT/PMT */
void ms_ts_parser_destroy(ms_ts_parser *p);
void ms_ts_parser_set_meter(ms_ts_parser *p, ms_copy_meter *m);

/*
 * 从缓冲前端尽量解析。*consumed 为已处理前缀。
 * 缓冲由调用方持有；解析器不保留 data 指针。
 */
ms_status ms_ts_parser_push(ms_ts_parser *p,
                            const uint8_t *data,
                            size_t size,
                            size_t *consumed,
                            int eos);

ms_status ms_ts_parser_pull_pes(ms_ts_parser *p, ms_pes_view *out);

/* 在不追加 TS 的情况下排空 ADTS（反压解除后由 pull 调用）。 */
ms_status ms_ts_parser_drain(ms_ts_parser *p);

#endif
