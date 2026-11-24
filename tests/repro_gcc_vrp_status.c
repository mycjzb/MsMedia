/*
 * 最小工具链证据：与 ms_session_signal_discontinuity 同源的返回码处理。
 * evq 类函数只返回 {0, 3, 9}（OK/LIMIT/NOMEM）时，gcc 14.2 -O2 tree-vrp
 * 可能把「st 属于 {0,1,3} 则 (AGAIN?OK:st) 否则原样返回」误编成
 * 用 0x9 做成员位测试且成功时返回 9。
 * 不是产品测试。
 * by: Long <mycjzb@gmail.com>
 */
#include <stdio.h>

#define ST_OK 0
#define ST_AGAIN 1
#define ST_LIMIT 3
#define ST_NOMEM 9

#if defined(__clang__) || defined(__INTEL_COMPILER)
#  define NOINLINE __attribute__((noinline))
#else
#  define NOINLINE __attribute__((noinline, noclone))
#endif

static int g_ret;

/* 与 evq_enqueue 相同的可见返回集合：OK / LIMIT / NOMEM，无 AGAIN。 */
NOINLINE int fake_enqueue(void)
{
    int r = g_ret;
    if (r == ST_OK) {
        return ST_OK;
    }
    if (r == ST_LIMIT) {
        return ST_LIMIT;
    }
    return ST_NOMEM;
}

/* 与 session disc 入队后的返回码处理同构。 */
NOINLINE int map_old(void)
{
    int st = fake_enqueue();
    if (st != ST_OK && st != ST_LIMIT && st != ST_AGAIN) {
        return st;
    }
    return (st == ST_AGAIN) ? ST_OK : st;
}

/* 拟定修复。 */
NOINLINE int map_new(void)
{
    int st = fake_enqueue();
    if (st == ST_AGAIN) {
        st = ST_OK;
    }
    return st;
}

static int expect_map(int st)
{
    /* enqueue 只产生 0/3/9，映射后应保持原值（无 AGAIN）。 */
    if (st != ST_OK && st != ST_LIMIT) {
        st = ST_NOMEM;
    }
    return (st == ST_AGAIN) ? ST_OK : st;
}

int main(void)
{
    const int ins[] = {ST_OK, ST_LIMIT, ST_NOMEM};
    int i, bad_old = 0, bad_new = 0;
    printf("repro_gcc_vrp_status\n");
    for (i = 0; i < (int)(sizeof(ins) / sizeof(ins[0])); i++) {
        int in = ins[i];
        int exp = expect_map(in);
        int o, n;
        g_ret = in;
        o = map_old();
        g_ret = in;
        n = map_new();
        printf("in=%d expect=%d old=%d new=%d%s%s\n", in, exp, o, n,
               (o != exp) ? " OLD_WRONG" : "",
               (n != exp) ? " NEW_WRONG" : "");
        if (o != exp) {
            bad_old++;
        }
        if (n != exp) {
            bad_new++;
        }
    }
    printf("summary old_wrong=%d new_wrong=%d\n", bad_old, bad_new);
    return bad_new ? 2 : 0;
}
