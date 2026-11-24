/*
 * HLS 独立预下载器（对标 Google Media3 HlsMediaSource + DefaultLoadControl）—— 内部头文件。
 * 由 ms_ff 持有：每个 playlist（主视频 / 独立音轨 rendition）一条后台下载线程 + 有界切片内存队列。
 *  - hls.c（MS_SLIM_S6 补丁）在打开当前切片前调用 ms_hls_pf_plan_*() 下发「从当前切片起」的请求单元计划；
 *  - ms_ff 的 io_open 钩子用 ms_hls_pf_open() 把切片请求接到内存队列（命中即零等待、未下完则边下边读），
 *    AES-128 切片在这里用 FFmpeg libavutil 的 AES（复用，不引入第二份实现）流式 CBC 解密并去 PKCS#7 填充；
 *  - 达到 max_buffer_ms / max_buffer_bytes 暂停预拉，跌破 min_buffer_ms 恢复（滞回）；
 *  - 首个单元边下边播，攒够 buffer_for_playback_ms 即放行；起播后单元边界数据不足记一次重缓冲，
 *    攒够 buffer_for_playback_after_rebuffer_ms 再放行；
 *  - 瞬时网络错误按 Media3 风格有限次短退避重试（50/100/200 ms，断点续传），cancel 毫秒级打断；
 *  - HTTP Keep-Alive：同一线程复用同一连接（ff_http_do_new_request2），跨主机自动重连。
 * 线程安全：一把互斥锁 + 条件变量；单元引用计数，计划替换 / 关闭 / 销毁零泄漏、无僵尸线程。
 * by: Long <mycjzb@gmail.com>
 */
#ifndef MS_HLS_PF_H
#define MS_HLS_PF_H

#include <stdint.h>
#include "libavformat/avformat.h"
#include "msmedia/ms_ff.h"

typedef struct ms_hls_pf ms_hls_pf;

/* ms_ff 提供的钩子：给预下载请求补上与播放器相同的 HTTP/TLS 选项（tls_verify、UA、headers 等）。 */
typedef void (*ms_hls_pf_opt_fn)(void *ctx, AVDictionary **opts);
/* 外部取消（ms_ff_cancel）：返回非 0 时所有下载 / 等待立即退出。 */
typedef int (*ms_hls_pf_cancel_fn)(void *ctx);

ms_hls_pf *ms_hls_pf_create(const ms_ff_buffer_config *cfg, AVFormatContext *ic,
                            ms_hls_pf_opt_fn opt_fn, ms_hls_pf_cancel_fn cancel_fn, void *ctx);
/* 停止全部线程（中断在途请求）并释放全部缓冲。仍挂着的读端由 ms_hls_pf_reader_close 收尾。 */
void ms_hls_pf_destroy(ms_hls_pf *pf);

/* 消费端：命中返回 1 并给出 *pb；未命中返回 0（调用方走原路径）；<0 为错误（如已取消）。 */
int  ms_hls_pf_open(ms_hls_pf *pf, const char *url, AVDictionary *opts, AVIOContext **pb);
/* pb 是否由本模块创建；是则 ms_hls_pf_reader_close 释放并返回 1。 */
int  ms_hls_pf_reader_close(AVIOContext *pb);

void ms_hls_pf_get_stats(ms_hls_pf *pf, ms_ff_buffer_stats *out);

/* hls.c 补丁调用（见 scripts/slimff_patch_s6_hls.py）；由 ms_ff.c 转发到本模块。 */
void ms_hls_pf_plan_begin_pf(ms_hls_pf *pf, int pls_index, int live, int64_t target_us);
void ms_hls_pf_plan_add_pf(ms_hls_pf *pf, int pls_index, const char *url, int64_t off, int64_t end, int64_t dur_us);
void ms_hls_pf_plan_end_pf(ms_hls_pf *pf, int pls_index, AVDictionary *opts);

#endif
