#ifndef MS_HW_BIONIC18_H
#define MS_HW_BIONIC18_H

/*
 * NDK r29 最低 compile target 为 API 21，直接调用下列函数会在 libmshw.so 留下强 UND，
 * API 18–20 的 bionic 尚未导出，动态加载会失败。弱引用后按地址是否为空回落。
 * 边界：
 *   pthread_condattr_setclock — API 21；缺则 cond 保持默认 CLOCK_REALTIME。
 *   pthread_mutex_timedlock   — API 21；缺则用 trylock + nanosleep 轮询到超时。
 * API 21+ 解析到 libc 实符号，不得走回落。
 * by: Long <mycjzb@gmail.com>
 */
#ifdef __ANDROID__
#include <pthread.h>
#include <time.h>

#pragma weak pthread_condattr_setclock
#pragma weak pthread_mutex_timedlock

#endif /* __ANDROID__ */
#endif /* MS_HW_BIONIC18_H */
