/*
 * MPEG-2 CRC-32（ISO/IEC 13818-1），用于 PAT/PMT 校验与合成样本。
 * by: Long <mycjzb@gmail.com>
 */

#ifndef MSMEDIA_CRC32_H
#define MSMEDIA_CRC32_H

#include <stddef.h>
#include <stdint.h>

/* 多项式 0x04C11DB7，初值 0xFFFFFFFF，MSB 优先，无最终异或。 */
uint32_t ms_crc32_mpeg2(const uint8_t *data, size_t len);

#endif
