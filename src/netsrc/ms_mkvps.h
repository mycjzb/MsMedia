/*
 * 轻量 MKV / MPEG-PS → MPEG-TS 流式转封装（纯自研单 so 用；内部接口）。
 * 转成 TS 后复用核心 TS 解封装与全部已支持编码（H.264/HEVC/MPEG-2 视频；AAC(ADTS)/AC-3/E-AC-3/DTS/MP2/MP3 音频）。
 * - MKV：EBML 流式解析（未知长度 Segment/Cluster），SimpleBlock/BlockGroup，无/Xiph/EBML/固定 lacing；
 *        avcC/hvcC → Annex-B 并在关键帧前补参数集；AAC 原始帧补 ADTS 头。Cues/附件/标签等大元素流式跳过不缓存。
 * - PS：pack/system header/PSM，视频 E0-EF、音频 C0-DF、私有流 BD（DVD 子流头 AC-3/DTS 剥离），
 *       无 PSM 时按负载起始码嗅探编码；PES 原样（或剥子流头后）重打成 TS。
 * 有界：单元素 ≤ 16 MiB，未发 PMT 前缓存 ≤ 2 MiB；坏数据跳过重同步，不中止。
 * by: Long <mycjzb@gmail.com>
 */
#ifndef MS_MKVPS_H
#define MS_MKVPS_H
#include <stddef.h>
#include <stdint.h>

typedef int (*ms_tsconv_write_fn)(void *ctx, const uint8_t *d, size_t n);
typedef struct ms_tsconv ms_tsconv;

enum { MS_TSCONV_MKV = 1, MS_TSCONV_PS = 2 };

ms_tsconv *ms_tsconv_create(int kind, ms_tsconv_write_fn w, void *ctx);
/* 返回 0 成功；<0 写出失败（取消）或内存不足 */
int ms_tsconv_feed(ms_tsconv *c, const uint8_t *d, size_t n);
int ms_tsconv_end(ms_tsconv *c);
void ms_tsconv_destroy(ms_tsconv *c);
/* 统计：输出的 PES 数与跳过的未支持轨数 */
void ms_tsconv_stats(const ms_tsconv *c, int *pes_out, int *skipped_tracks);

#endif
