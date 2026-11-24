/*
 * 仅链入 libmsff.so：为隐藏编入的 ms_ts_scan 提供 ms_size_add。
 * libmsff.a 仍解析到 libmsmedia，避免与测试里的核心库重复定义。
 * by: Long <mycjzb@gmail.com>
 */
#include <stddef.h>

int ms_size_add(size_t a, size_t b, size_t *out);

int ms_size_add(size_t a, size_t b, size_t *out)
{
    if (out == NULL) {
        return -1;
    }
    if (b > ((size_t)-1) - a) {
        return -1;
    }
    *out = a + b;
    return 0;
}
