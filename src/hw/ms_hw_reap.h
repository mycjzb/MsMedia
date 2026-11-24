/*
 * 工作线程有界回收：UI 不得在超时后直接 free 仍在运行的上下文（UAF），
 * 也不得无界 join 卡死 UI。到期则交给线程退出时自行释放。
 * 内部辅助，不是稳定公共 ABI。
 * by: Long <mycjzb@gmail.com>
 */
#ifndef MS_HW_REAP_H
#define MS_HW_REAP_H

#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 有界等待 *th_finished。
 * 返回 1：线程已结束，调用方必须 pthread_join 后释放对象。
 * 返回 0：已置 *abandoned=1，调用方必须 pthread_detach，此后不得再碰对象；
 *         线程须在退出前调用 ms_hw_reap_thread_exiting，若返回 1 则由线程释放。
 * 返回 -1：参数错误。
 * timeout_ms <= 0 时立即按「未结束」处理（仍遵守上述所有权，不直接 free）。
 */
int ms_hw_reap_wait_or_abandon(pthread_mutex_t *mu, pthread_cond_t *cv,
                               int *th_finished, int *abandoned, int timeout_ms);

/*
 * 工作线程在循环退出、return 前调用。内部加锁，置 *th_finished=1 并广播。
 * 返回 1：已被 abandon，本线程负责释放。
 * 返回 0：调用方（UI）将 join 并释放。
 */
int ms_hw_reap_thread_exiting(pthread_mutex_t *mu, pthread_cond_t *cv,
                              int *th_finished, const int *abandoned);

#ifdef __cplusplus
}
#endif

#endif
