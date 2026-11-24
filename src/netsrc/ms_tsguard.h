/*
 * TS 坏包过滤（纯自研单 so 网络输入用；内部接口）：
 * 核心 TS 解封装按合同对畸形包严格报错；直播/组播常见的比特翻转、截断包、插入垃圾在送入核心前由本过滤器处理：
 *   - 188/192/204 自动同步（连续 3 个 0x47 确认），192 去前缀、204 去 RS 校验，统一输出 188；
 *   - 丢弃：adaptation_field_control=0、适配域长度非法、PSI（PAT/PMT）单包节 CRC 错、
 *           ES PID 的 PUSI 包不以 00 00 01 起始或 PES 长度/头长自相矛盾；
 *   - 丢包后核心看到 CC 跳变，按既有规则丢弃不完整单元并发 DISCONTINUITY，播放不中断。
 * by: Long <mycjzb@gmail.com>
 */
#ifndef MS_TSGUARD_H
#define MS_TSGUARD_H
#include <stddef.h>
#include <stdint.h>

typedef int (*ms_tsguard_write_fn)(void *ctx, const uint8_t *d, size_t n);
typedef struct ms_tsguard ms_tsguard;

ms_tsguard *ms_tsguard_create(ms_tsguard_write_fn w, void *ctx);
int ms_tsguard_feed(ms_tsguard *g, const uint8_t *d, size_t n);
int ms_tsguard_flush(ms_tsguard *g);
void ms_tsguard_destroy(ms_tsguard *g);
uint64_t ms_tsguard_dropped(const ms_tsguard *g);
#endif
