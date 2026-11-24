/*
 * MPEG-TS / PES 载荷上的 AAC ADTS 增量组帧。
 * by: Long <mycjzb@gmail.com>
 *
 * 本构建支持的子集（不是全部 AAC）：
 *   - MPEG-2 或 MPEG-4 ADTS（ID=1 或 0）、同步字 0xFFF、layer=0
 *   - protection_absent 0（9 字节头，含 CRC 字段）或 1（7 字节头）
 *     本模块读取头长以组帧，不校验 CRC 值
 *   - number_of_raw_data_blocks_in_frame 必须为 0（单块）
 *   - 声明长度齐全的完整一帧才作为 AU
 *   - 由 ADTS 头生成 2 字节 AudioSpecificConfig（AOT = profile+1）
 *   - sampling_frequency_index 0..12，channel_configuration 1..7
 *   - 同一 PES 内后续 AU 的时间戳按每帧 1024 个采样、90 kHz 有理数
 *     余数累积推算（48 kHz 仍为精确 +1920；44.1 kHz 不逐帧向下取整累积）
 *   - 新 PES 的 PTS 重新锚定；reset/flush/不连续/采样率变化清余数；
 *     无 PTS 不杜撰绝对时钟
 *
 * 明确拒绝（不伪报为可组帧）：LATM/LOAS、无 ADTS 的裸 AAC、
 * raw_data_blocks!=0、channel_configuration=0（PCE）、index 13-15、
 * SBR/PS 扩展 config、不完整头或未满长度的数据。
 * 这里不做软解，也不解释 AAC 压缩载荷。
 */

#ifndef MSMEDIA_ADTS_H
#define MSMEDIA_ADTS_H

#include "ms_internal.h"

typedef struct ms_adts_frame {
    const uint8_t *data;
    size_t size;
    int has_pts;
    int has_dts;
    uint64_t pts;
    uint64_t dts;
    int format_changed;
    uint8_t asc[2];
    size_t asc_size;
    int audio_object_type;
    int sample_rate;
    int channels;
    uint8_t *owned_base;
    size_t owned_cap;
    int *owned_taken;
} ms_adts_frame;

typedef struct ms_adts_callbacks {
    void *ctx;
    ms_status (*on_config)(void *ctx, const ms_adts_frame *fr);
    ms_status (*on_au)(void *ctx, const ms_adts_frame *fr);
} ms_adts_callbacks;

typedef struct ms_adts {
    const ms_allocator *alloc;
    size_t max_frame;
    ms_adts_callbacks cb;
    uint8_t *buf;
    size_t len;
    size_t cap;
    int have_start_ts;
    int start_has_pts;
    int start_has_dts;
    uint64_t start_pts;
    uint64_t start_dts;
    /* rem += 90000*1024; inc = rem/rate; rem %= rate。新 PES PTS 锚定 rem=0。 */
    int ts_rate;
    uint64_t ts_rem;
    int pes_ts_available;
    int pes_has_pts;
    int pes_has_dts;
    uint64_t pes_pts;
    uint64_t pes_dts;
    int have_config;
    uint8_t last_asc[2];
    int last_rate;
    int last_ch;
    int last_aot;
    int failed;
    ms_status fail_status;
    ms_copy_meter *meter;
} ms_adts;

void ms_adts_set_meter(ms_adts *a, ms_copy_meter *m);

void ms_adts_init(ms_adts *a,
                  const ms_allocator *alloc,
                  size_t max_frame,
                  const ms_adts_callbacks *cb);
void ms_adts_destroy(ms_adts *a);
void ms_adts_reset(ms_adts *a);          /* 丢掉缓冲和 config */
void ms_adts_flush_partial(ms_adts *a);  /* 丢掉未完成/未发出的字节，保留 config */
/* 容错模式（仅单 so 网络输入启用）：清除失败态并丢掉未完成字节，保留 config，按同步字重新对齐。 */
void ms_adts_recover(ms_adts *a);

/* 新的 PES 载荷开始。未完成的上一帧仍沿用它开始时的时间戳。 */
void ms_adts_begin_pes(ms_adts *a, int has_pts, uint64_t pts, int has_dts, uint64_t dts);

/* 追加载荷并尽可能发出完整 AU。LIMIT 时已完整但未发出的帧留在缓冲里，不丢弃。 */
ms_status ms_adts_push(ms_adts *a, const uint8_t *data, size_t size, size_t *consumed);

/* 再尝试发出已完整的帧（反压解除后）。 */
ms_status ms_adts_drain(ms_adts *a);

/* 输入结束：发出已完整帧，丢弃半帧，不把半帧当成 AU。 */
ms_status ms_adts_end(ms_adts *a);

#endif
