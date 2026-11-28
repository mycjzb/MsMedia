/*
 * NDK 以 API 21 为 compile target，会把符号记成强 UND xxx@LIBC。
 * API 18（Android 4.3）libc：无 versioned atof/signal；rand 虽有但 @LIBC 版本对不上会 dlopen 失败；
 * arc4random_buf 要到更高 API 才有。本文件链进 libmsplayer.so 内提供实现。
 * map local:* 不对外导出。只补当前 so 强引用且老盒会挂的符号。
 * by: Long <mycjzb@gmail.com>
 */
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

double atof(const char *nptr)
{
    return strtod(nptr != NULL ? nptr : "", NULL);
}

#ifdef signal
#undef signal
#endif

void (*signal(int sig, void (*handler)(int)))(int)
{
    struct sigaction sa;
    struct sigaction oa;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handler;
    /* API 18 无 sigemptyset：bionic 的 sigset_t 是位图，清零即可。 */
    if (sigaction(sig, &sa, &oa) != 0) {
        return SIG_ERR;
    }
    return oa.sa_handler;
}

/* POSIX 同款 LCG；避免引用 rand@LIBC（老盒 dlopen 找不到） */
static uint32_t g_rand_state = 1u;

int rand(void)
{
    g_rand_state = g_rand_state * 1103515245u + 12345u;
    return (int)((g_rand_state >> 16) & 0x7fffu);
}

void srand(unsigned int seed)
{
    g_rand_state = seed ? seed : 1u;
}

void arc4random_buf(void *buf, size_t n)
{
    unsigned char *p = (unsigned char *)buf;
    size_t i;
    for (i = 0; i < n; i++) {
        p[i] = (unsigned char)(rand() & 0xff);
    }
}
