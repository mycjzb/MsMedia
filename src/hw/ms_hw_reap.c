#define _DEFAULT_SOURCE
/*
 * 见 ms_hw_reap.h。
 * by: Long <mycjzb@gmail.com>
 */
#include "ms_hw_reap.h"
#include "msmedia/ms_hw.h"

#include <errno.h>
#include <stdatomic.h>
#include <time.h>

static atomic_int g_abandoned_live;

int ms_hw_abandoned_worker_count(void)
{
    int n = atomic_load(&g_abandoned_live);
    return n > 0 ? n : 0;
}

int ms_hw_abandoned_worker_cap(void)
{
    return MS_HW_ABANDONED_WORKER_CAP;
}

int ms_hw_can_spawn_worker(void)
{
    return atomic_load(&g_abandoned_live) < MS_HW_ABANDONED_WORKER_CAP;
}

int ms_hw_reap_wait_or_abandon(pthread_mutex_t *mu, pthread_cond_t *cv,
                               int *th_finished, int *abandoned, int timeout_ms)
{
    struct timespec ts;
    int rc;

    if (mu == NULL || cv == NULL || th_finished == NULL || abandoned == NULL) {
        return -1;
    }
    pthread_mutex_lock(mu);
    rc = 0;
    if (timeout_ms > 0 && !*th_finished) {
        if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
            pthread_mutex_unlock(mu);
            return -1;
        }
        ts.tv_sec += (time_t)(timeout_ms / 1000);
        ts.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec += 1;
            ts.tv_nsec -= 1000000000L;
        }
        while (!*th_finished && rc == 0) {
            rc = pthread_cond_timedwait(cv, mu, &ts);
        }
    }
    if (*th_finished) {
        pthread_mutex_unlock(mu);
        return 1;
    }
    *abandoned = 1;
    atomic_fetch_add(&g_abandoned_live, 1);
    pthread_mutex_unlock(mu);
    return 0;
}

int ms_hw_reap_thread_exiting(pthread_mutex_t *mu, pthread_cond_t *cv,
                              int *th_finished, const int *abandoned)
{
    int own;

    if (mu == NULL || cv == NULL || th_finished == NULL || abandoned == NULL) {
        return -1;
    }
    pthread_mutex_lock(mu);
    *th_finished = 1;
    own = *abandoned;
    pthread_cond_broadcast(cv);
    pthread_mutex_unlock(mu);
    if (own) {
        int left = atomic_fetch_sub(&g_abandoned_live, 1);
        if (left <= 0) {
            atomic_store(&g_abandoned_live, 0);
        }
    }
    return own ? 1 : 0;
}
