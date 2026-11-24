/*
 * 经典 FLV 解封装（本步子集）。
 *
 * 范围：FLV 头 + 顺序 tag，仅 SoundFormat=AAC(10) 与 CodecID=AVC(7)。
 * 配置为完整 avcC / ASC，AU 为 AVCC 长度前缀 / AAC_RAW，与 BMFF 合同相同。
 * 时间戳为 32 位扩展毫秒 DTS，合成时间是有符号 24 位；PTS=DTS+cto。
 * 负 PTS 因公共 ABI 的 uint64 无法表达，返回 UNSUPPORTED，不回绕。
 * 脚本 tag（18）按声明大小跳过，不做 AMF 解析，也不据此建立索引。
 * Enhanced FLV（视频 IsExHeader）与非 AVC/AAC 诚实拒绝，不错当成经典 AVC。
 * 不做 RTMP/网络客户端，不做 seek / 关键帧索引。
 * by: Long <mycjzb@gmail.com>
 */
#ifndef MSMEDIA_FLV_H
#define MSMEDIA_FLV_H

#include "ms_internal.h"

typedef struct ms_flv_callbacks {
    void *ctx;
    ms_status (*on_format)(void *ctx, ms_format format);
    ms_status (*on_track)(void *ctx, const ms_track_info *track, int update);
    ms_status (*on_codec_config)(void *ctx, const ms_track_info *track,
                                 const uint8_t *data, size_t size,
                                 ms_sample_format sample_fmt,
                                 int time_base_num, int time_base_den);
    ms_status (*on_au)(void *ctx, const ms_track_info *track,
                       const ms_au_frame *fr);
} ms_flv_callbacks;

typedef struct ms_flv_parser ms_flv_parser;

int ms_flv_probe(const uint8_t *data, size_t size);
size_t ms_flv_parser_size(void);

void ms_flv_parser_init(ms_flv_parser *p, const ms_allocator *alloc,
                        const ms_flv_callbacks *cb);
void ms_flv_parser_set_limits(ms_flv_parser *p, size_t max_assembly_bytes);
void ms_flv_parser_reset(ms_flv_parser *p);
void ms_flv_parser_destroy(ms_flv_parser *p);
void ms_flv_parser_set_meter(ms_flv_parser *p, ms_copy_meter *m);
void ms_flv_flush_partial(ms_flv_parser *p);
int ms_flv_parser_need_unsup(const ms_flv_parser *p);

ms_status ms_flv_parser_push(ms_flv_parser *p, const uint8_t *data,
                             size_t size, size_t *consumed, int eos);
ms_status ms_flv_parser_drain(ms_flv_parser *p);

#endif
