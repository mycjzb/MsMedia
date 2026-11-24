/*
 * avcC 与 AAC AudioSpecificConfig 的共用校验。
 * BMFF 与 FLV 的配置/样本合同复用此处，不平行各写一套。
 * by: Long <mycjzb@gmail.com>
 */
#ifndef MSMEDIA_AVCC_H
#define MSMEDIA_AVCC_H

#include "msmedia/msmedia.h"

#ifdef __cplusplus
extern "C" {
#endif

ms_status ms_avcc_parse(const uint8_t *data, size_t size, uint8_t *nal_len_size);
ms_status ms_avcc_validate_sample(const uint8_t *data, size_t size,
                                  unsigned nal_len_size);
int ms_asc_ok(const uint8_t *data, size_t size);
/* 解析本构建支持的 LC-AAC ASC（AOT=2，index 0..12，声道 1..7）。失败返回 0。 */
int ms_asc_parse(const uint8_t *data, size_t size,
                 int *audio_object_type, int *sample_rate, int *channels);

#ifdef __cplusplus
}
#endif

#endif
