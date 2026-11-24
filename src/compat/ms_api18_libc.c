/*
 * NDK r29 以 API 21 为最低 compile target，FFmpeg/OpenSSL/libxml2 前缀按该 sysroot 链接，
 * 会把仅 API 21 才进入 bionic 的符号记成强 UND（atof@LIBC、signal@LIBC）。
 * API 18（Android 4.3）的 libc 没有这两项（有 strtod 与 sigaction）。
 * 本文件链进 libmsff.so，在 DSO 内提供实现，使 libmsff 能在 API 18 加载。
 * 不改编、不覆盖已冻结的 size-opt 前缀；getentropy 已是弱 UND，不必再包。
 * API 21+ 上这两个符号由本 DSO 内部实现（atof→strtod，signal→sigaction），
 * libmsff.map 的 local:* 不对外导出它们。
 * 边界：只补 API 18 libc 缺、且当前 libmsff 强引用的符号。
 * 不用 sigemptyset / sigaddset / bsd_signal：前两者也是 API 21 才导出；
 * bsd_signal 在 r29 的 64 位 API 21 sysroot 中不存在。sigset_t 清零等价于 sigemptyset。
 * by: Long <mycjzb@gmail.com>
 */
#include <signal.h>
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
