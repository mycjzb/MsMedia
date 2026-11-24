/*
 * TS 同步扫描内部入口。实现见 ms_ts_scan.c，供解析器与公共 ABI 共用。
 * by: Long <mycjzb@gmail.com>
 */
#ifndef MSMEDIA_TS_SCAN_H
#define MSMEDIA_TS_SCAN_H

#include "msmedia/msmedia.h"

#define MS_TS_SYNC_WANT 3

int ms_ts_pkt_plausible(const uint8_t *pkt, size_t avail);
int ms_ts_find_sync(const uint8_t *data, size_t size,
                    size_t prefer_from, size_t *out_off, uint16_t *out_psz);
size_t ms_ts_sync_keep_from(const uint8_t *data, size_t size);
ms_ts_prefix_kind ms_ts_detect_prefix_kind(const uint8_t *data, size_t size);
size_t ms_ts_image_end(const uint8_t *data, size_t size, ms_ts_prefix_kind kind);

#endif
