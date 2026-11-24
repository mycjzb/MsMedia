/*
 * ms_probe 验收：图片伪装头（PNG/JPEG/GIF/BMP/WebP，含双图叠加）+ 前导脏字节 + 真实 TS/MP4/FLV/MKV、
 * M2TS(192)、BOM HLS、XML DASH；整块/逐段喂入结果一致；剥离后送 ms_session 的 AU 数与干净文件相同；
 * 随机字节模糊不崩。ASan/UBSan/LSan 下运行。
 * by: Long <mycjzb@gmail.com>
 */
#include "msmedia/msmedia.h"
#include "ms_probe.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned char *slurp(const char *p, size_t *n)
{
    FILE *f = fopen(p, "rb");
    unsigned char *b;
    long l;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); l = ftell(f); fseek(f, 0, SEEK_SET);
    b = malloc((size_t)l + 1);
    if (fread(b, 1, (size_t)l, f) != (size_t)l) { fclose(f); free(b); return NULL; }
    fclose(f);
    *n = (size_t)l;
    return b;
}

static int count_aus(const unsigned char *d, size_t n)
{
    ms_session *s = NULL;
    size_t off = 0;
    int au = 0, guard = 0;
    if (ms_session_create(NULL, NULL, &s) != MS_OK) return -1;
    for (;;) {
        ms_event ev;
        ms_status st;
        while (off < n) {
            size_t used = 0, chunk = n - off > 65536 ? 65536 : n - off;
            st = ms_session_push(s, d + off, chunk, &used);
            off += used;
            if (used == 0 || st == MS_LIMIT) break;
        }
        if (off >= n && guard == 0) { (void)ms_session_end_of_input(s); guard = 1; }
        memset(&ev, 0, sizeof(ev));
        st = ms_session_pull(s, &ev);
        if (st == MS_OK) {
            if (ev.type == MS_EVENT_AU) au++;
            if (ev.release_id) ms_session_release(s, ev.release_id);
            if (ev.type == MS_EVENT_EOS) break;
            continue;
        }
        if (guard && st != MS_AGAIN && st != MS_LIMIT) break;
        if (guard && ++guard > 100000) break;
    }
    ms_session_destroy(s);
    return au;
}

int main(int argc, char **argv)
{
    char line[512], path[1024];
    FILE *m;
    int fails = 0, cases = 0, i;
    const char *dir = argv[1], *samples = argv[2];
    snprintf(path, sizeof(path), "%s/MANIFEST", dir);
    m = fopen(path, "r");
    if (!m) return 2;
    while (fgets(line, sizeof(line), m)) {
        char name[256], fmt[32], img[32], clean[256];
        size_t want, n = 0, step;
        unsigned char *d;
        ms_probe_result r, r2;
        int rc;
        if (sscanf(line, "%255s %zu %31s %31s %255s", name, &want, fmt, img, clean) != 5) continue;
        snprintf(path, sizeof(path), "%s/%s", dir, name);
        d = slurp(path, &n);
        if (!d) { printf("MISSING %s\n", name); fails++; continue; }
        cases++;
        rc = ms_probe_payload(d, n, 0, &r);
        if (rc != 1 || r.offset != want || strcmp(ms_probe_fmt_name(r.fmt), fmt) || strcmp(ms_probe_disguise_name(r.disguise), img)) {
            printf("FAIL %s rc=%d off=%zu want=%zu fmt=%s/%s img=%s/%s\n", name, rc, r.offset, want,
                   ms_probe_fmt_name(r.fmt), fmt, ms_probe_disguise_name(r.disguise), img);
            fails++;
            free(d);
            continue;
        }
        /* 逐段增长喂入：要么还要数据，要么与整块结果一致 */
        for (step = 1; step <= n; step += (step < 400 ? 7 : 997)) {
            rc = ms_probe_payload(d, step, 0, &r2);
            if (rc == 0) continue;
            if (rc != 1 || r2.offset != r.offset || r2.fmt != r.fmt) {
                printf("FAIL-INC %s at %zu rc=%d off=%zu fmt=%s\n", name, step, rc, r2.offset, ms_probe_fmt_name(r2.fmt));
                fails++;
            }
            break;
        }
        if (strcmp(clean, "-") && (!strcmp(fmt, "ts") || !strcmp(fmt, "bmff") || !strcmp(fmt, "flv"))) {
            size_t cn = 0;
            unsigned char *c;
            int a, b;
            snprintf(path, sizeof(path), "%s/%s", samples, clean);
            c = slurp(path, &cn);
            a = count_aus(c, cn);
            b = count_aus(d + r.offset, n - r.offset);
            if (a <= 0 || a != b) { printf("FAIL-AU %s clean=%d stripped=%d\n", name, a, b); fails++; }
            else if (!strcmp(img, "png") || !strcmp(img, "none")) printf("AU_OK %s au=%d\n", name, b);
            free(c);
        }
        printf("OK %s off=%zu fmt=%s img=%s junk=%zu\n", name, r.offset, ms_probe_fmt_name(r.fmt), ms_probe_disguise_name(r.disguise), r.junk_bytes);
        free(d);
    }
    fclose(m);
    {
        unsigned long long x = 0x9E3779B97F4A7C15ull;
        unsigned char buf[4096];
        for (i = 0; i < 20000; i++) {
            size_t k, n = (size_t)(x % sizeof(buf));
            ms_probe_result r;
            for (k = 0; k < n; k++) { x ^= x << 13; x ^= x >> 7; x ^= x << 17; buf[k] = (unsigned char)x; }
            if (n > 8 && (x & 3) == 0) memcpy(buf, "\x89PNG\r\n\x1a\n", 8);
            if (n > 3 && (x & 3) == 1) memcpy(buf, "\xff\xd8\xff", 3);
            if (n > 6 && (x & 7) == 2) memcpy(buf, "GIF89a", 6);
            (void)ms_probe_payload(buf, n, (int)(x & 1), &r);
            (void)ms_probe_media_offset(buf, (int)n, 1, NULL);
        }
    }
    printf("%s cases=%d fails=%d fuzz=20000\n", fails ? "PROBE_TEST_FAIL" : "PROBE_TEST_PASS", cases, fails);
    return fails ? 1 : 0;
}
