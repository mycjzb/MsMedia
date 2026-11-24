/*
 * ms_hls 内部扩展（不进公共头、不进导出表、不改 C ABI v1）：供自研预加载器前瞻。
 * by: Long <mycjzb@gmail.com>
 */
#ifndef MS_HLS_INTERNAL_H
#define MS_HLS_INTERNAL_H
#include "msmedia/msmedia.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef void (*ms_hls_upcoming_fn)(void *ctx, const ms_hls_request *req, uint32_t dur_ms);
size_t ms_hls_upcoming(ms_hls *h, size_t max_items, ms_hls_upcoming_fn fn, void *ctx);
/* 清单是多码率主清单（只有 STREAM-INF，没有分片）。 */
int ms_hls_is_master(const ms_hls *h);
#ifdef __cplusplus
}
#endif
#endif
