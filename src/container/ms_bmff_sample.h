/*
 * 渐进 MP4/MOV 的 sample 表解析与 AU 收获。
 * moov 先于 mdat：按文件偏移收获。moov 在 mdat 之后：有界回看窗
 * 覆盖所需样本时按相同合同输出；否则明确报错，不整文件缓存。
 * P2d：fMP4 在 moov+mvex 初始化之后，按 moof/traf/trun 与随后 mdat
 * 输出与渐进路径相同合同的 H.264/AAC AU。不是任意 seek，也不是 FLV。
 * 样本格式：H.264 为 length-prefixed（avcC），AAC 为按 sample size
 * 定界的裸 AU；不把 TS 的 Annex B / ADTS 输入假设套到本路径。
 * by: Long <mycjzb@gmail.com>
 */

#ifndef MSMEDIA_BMFF_SAMPLE_H
#define MSMEDIA_BMFF_SAMPLE_H

#include "ms_bmff.h"

int ms_bmff_is_capture_type(uint32_t type);

void ms_bmff_demux_free(ms_bmff_parser *p);

ms_status ms_bmff_on_container_enter(ms_bmff_parser *p, uint32_t type);
ms_status ms_bmff_on_container_leave(ms_bmff_parser *p, uint32_t type);
ms_status ms_bmff_on_captured(ms_bmff_parser *p, uint32_t type,
                              const uint8_t *data, size_t size);

int ms_bmff_should_harvest(const ms_bmff_parser *p);
ms_status ms_bmff_announce(ms_bmff_parser *p);
ms_status ms_bmff_feed_mdat(ms_bmff_parser *p, const uint8_t *data, size_t size,
                            size_t *used, int eos);

/* P2c：mdat 出现在 moov 定稿前时尝试开启有界回看；失败则只跳过。 */
void ms_bmff_lookback_on_mdat(ms_bmff_parser *p, uint64_t payload, int until_eof);
void ms_bmff_lookback_feed(ms_bmff_parser *p, const uint8_t *data, size_t take);
ms_status ms_bmff_replay_lookback(ms_bmff_parser *p);
ms_status ms_bmff_on_moof_begin(ms_bmff_parser *p, uint64_t offset, uint64_t size);
int ms_bmff_parser_need_unsup(const ms_bmff_parser *p);
void ms_bmff_lookback_drop(ms_bmff_parser *p);
uint64_t ms_bmff_parser_file_off(const ms_bmff_parser *p);
int ms_bmff_fill_sample_need(const ms_bmff_parser *p, ms_bmff_need *out);

#endif /* MSMEDIA_BMFF_SAMPLE_H */
