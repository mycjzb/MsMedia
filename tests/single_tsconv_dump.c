/* 宿主工具：MKV/PS 文件 → ms_tsconv → TS 文件（调试与差分用）。by: Long <mycjzb@gmail.com> */
#include "netsrc/ms_mkvps.h"
#include "core/ms_probe.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int wr(void *ctx, const uint8_t *d, size_t n) { return fwrite(d, 1, n, (FILE *)ctx) == n ? 0 : -1; }
int main(int argc, char **argv)
{
    FILE *in, *out;
    ms_tsconv *c;
    uint8_t buf[7777];
    size_t k;
    int kind, r = 0, pes, sk;
    if (argc < 4) return 2;
    kind = !strcmp(argv[1], "mkv") ? MS_TSCONV_MKV : MS_TSCONV_PS;
    in = fopen(argv[2], "rb"); out = fopen(argv[3], "wb");
    if (!in || !out) return 3;
    c = ms_tsconv_create(kind, wr, out);
    while (r == 0 && (k = fread(buf, 1, sizeof(buf), in)) > 0) r = ms_tsconv_feed(c, buf, k);
    if (r == 0) r = ms_tsconv_end(c);
    ms_tsconv_stats(c, &pes, &sk);
    printf("TSCONV r=%d pes=%d skipped=%d\n", r, pes, sk);
    ms_tsconv_destroy(c);
    fclose(in); fclose(out);
    return r != 0;
}
