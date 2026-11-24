/*
 * 有界 TS 前缀识别/剥离自测：裸 TS、PNG/JPEG 伪装、跨分片、假 0x47、
 * 扫描超限、截断、取消。不是设备出画。
 * by: Long <mycjzb@gmail.com>
 */
#include "msmedia/msmedia.h"
#include "ms_ts_synth.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail;

#define CHECK(cond, msg)                                                       \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "  FAIL %s\n", msg);                               \
            g_fail++;                                                          \
            return -1;                                                         \
        }                                                                      \
    } while (0)

#define CHECK_ST(st, want, msg)                                                \
    do {                                                                       \
        ms_status _s = (st);                                                   \
        if (_s != (want)) {                                                    \
            fprintf(stderr, "  FAIL %s st=%d want=%d\n", msg, (int)_s,         \
                    (int)(want));                                              \
            g_fail++;                                                          \
            return -1;                                                         \
        }                                                                      \
    } while (0)

static int make_basic_ts(ms_synth_buf *b, uint16_t psz)
{
    ms_synth_stream st[2];
    memset(st, 0, sizeof(st));
    st[0].stream_type = 0x1B;
    st[0].pid = 0x101;
    st[1].stream_type = 0x0F;
    st[1].pid = 0x102;
    st[1].language[0] = 'e';
    st[1].language[1] = 'n';
    st[1].language[2] = 'g';
    ms_synth_reset_cc();
    return ms_synth_pat_pmt(b, 1, 0x100, st, 2, 8, psz);
}

static void be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

/* 最小可解析 PNG：可选填充，使总长度 > 188。不校验 CRC。 */
static int make_png(uint8_t *out, size_t cap, size_t filler, size_t *out_len,
                    int fake_sync_run)
{
    static const uint8_t sig[8] = { 0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a };
    static const uint8_t ihdr[13] = { 0, 0, 0, 1, 0, 0, 0, 1, 8, 2, 0, 0, 0 };
    size_t off = 0;
    uint32_t text_len;
    size_t i;

    if (filler < 16) {
        filler = 16;
    }
    text_len = (uint32_t)filler;
    if (8u + 12u + 13u + 12u + text_len + 12u > cap) {
        return -1;
    }
    memcpy(out, sig, 8);
    off = 8;
    be32(out + off, 13);
    memcpy(out + off + 4, "IHDR", 4);
    memcpy(out + off + 8, ihdr, 13);
    be32(out + off + 21, 0); /* crc ignored */
    off += 25;
    be32(out + off, text_len);
    memcpy(out + off + 4, "tEXt", 4);
    memset(out + off + 8, 0xA5, text_len);
    if (fake_sync_run) {
        /* 在 tEXt 填充里造 16 个 188 间距的假 0x47；不得改写 PNG 签名。 */
        {
            size_t run = off + 8; /* tEXt 数据起点 */
            for (i = 0; i < 16u; i++) {
                size_t p = run + i * 188u;
                if (p + 4 >= off + 8 + text_len) {
                    break;
                }
                out[p] = 0x47;
                out[p + 3] = 0x10; /* adapt=1 */
            }
        }
    }
    be32(out + off + 8 + text_len, 0);
    off += 12u + text_len;
    be32(out + off, 0);
    memcpy(out + off + 4, "IEND", 4);
    be32(out + off + 8, 0);
    off += 12;
    *out_len = off;
    return 0;
}

static int make_jpeg(uint8_t *out, size_t cap, size_t filler, size_t *out_len,
                     int fake_sync_run)
{
    size_t i, seglen;
    if (filler < 16) {
        filler = 16;
    }
    seglen = filler + 2;
    if (4 + seglen + 2 > cap) {
        return -1;
    }
    out[0] = 0xff;
    out[1] = 0xd8;
    out[2] = 0xff;
    out[3] = 0xfe; /* COM */
    out[4] = (uint8_t)(seglen >> 8);
    out[5] = (uint8_t)seglen;
    memset(out + 6, 0x5A, filler);
    if (fake_sync_run) {
        for (i = 0; i < 16u; i++) {
            /* 种在 COM 段载荷内，不得改写 SOI/COM 标记与长度。 */
            size_t p = 6u + i * 188u;
            if (p + 4 >= 6 + filler) {
                break;
            }
            out[p] = 0x47;
            out[p + 3] = 0x10;
        }
    }
    out[6 + filler] = 0xff;
    out[7 + filler] = 0xd9;
    *out_len = 8 + filler;
    return 0;
}

static int push_all(ms_session *s, const uint8_t *data, size_t n, size_t chunk)
{
    size_t off = 0;
    if (chunk == 0) {
        chunk = n;
    }
    while (off < n) {
        size_t c = 0, want = n - off;
        ms_status st;
        if (want > chunk) {
            want = chunk;
        }
        st = ms_session_push(s, data + off, want, &c);
        if (st != MS_OK && st != MS_LIMIT) {
            fprintf(stderr, "  push st=%d off=%zu\n", (int)st, off);
            return -1;
        }
        if (c == 0 && st == MS_LIMIT) {
            return -1;
        }
        off += c;
    }
    return 0;
}

static int expect_ts_format(ms_session *s, uint16_t psz, size_t prefix)
{
    ms_event ev;
    ms_session_stats st;
    int got = 0;
    memset(&ev, 0, sizeof(ev));
    while (ms_session_pull(s, &ev) == MS_OK) {
        if (ev.type == MS_EVENT_FORMAT) {
            CHECK(ev.format == MS_FORMAT_MPEG_TS, "format ts");
            CHECK(ev.ts_packet_size == psz, "psz");
            got = 1;
        }
    }
    CHECK(got, "got format");
    memset(&st, 0, sizeof(st));
    st.struct_size = sizeof(st);
    CHECK_ST(ms_session_get_stats(s, &st), MS_OK, "stats");
    CHECK(st.ts_prefix_bytes == (uint64_t)prefix, "prefix bytes");
    (void)psz;
    return 0;
}

static int test_scan_bare_and_short(void)
{
    ms_synth_buf b;
    ms_ts_scan_opt opt;
    ms_ts_scan_result r;
    uint8_t pfx[8] = { 0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a };

    ms_synth_init(&b);
    CHECK(make_basic_ts(&b, 188) == 0, "synth");
    memset(&opt, 0, sizeof(opt));
    opt.struct_size = sizeof(opt);
    memset(&r, 0, sizeof(r));
    r.struct_size = sizeof(r);
    CHECK_ST(ms_ts_scan(b.data, b.len, 1, &opt, &r), MS_OK, "bare");
    CHECK(r.ts_offset == 0 && r.packet_size == 188, "bare off");
    CHECK(r.kind == MS_TS_PREFIX_NONE, "bare kind");

    CHECK(ms_synth_prepend(&b, pfx, sizeof(pfx)) == 0, "png sig");
    memset(&r, 0, sizeof(r));
    r.struct_size = sizeof(r);
    CHECK_ST(ms_ts_scan(b.data, b.len, 1, &opt, &r), MS_OK, "short png");
    CHECK(r.ts_offset == 8 && r.packet_size == 188, "short off");
    CHECK(r.kind == MS_TS_PREFIX_PNG, "short kind");
    ms_synth_free(&b);
    printf("  test_scan_bare_and_short ok\n");
    return 0;
}

static int session_with_prefix(const uint8_t *pfx, size_t pfx_n, uint16_t psz,
                               size_t chunk, int expect_kind)
{
    ms_synth_buf b;
    ms_session *s = NULL;
    ms_session_stats st;

    ms_synth_init(&b);
    CHECK(make_basic_ts(&b, psz) == 0, "synth");
    if (pfx_n > 0) {
        CHECK(ms_synth_prepend(&b, pfx, pfx_n) == 0, "prepend");
    }
    CHECK_ST(ms_session_create(NULL, NULL, &s), MS_OK, "create");
    if (push_all(s, b.data, b.len, chunk) != 0) {
        ms_session_destroy(s);
        ms_synth_free(&b);
        return -1;
    }
    if (expect_ts_format(s, psz, pfx_n) != 0) {
        ms_session_destroy(s);
        ms_synth_free(&b);
        return -1;
    }
    memset(&st, 0, sizeof(st));
    st.struct_size = sizeof(st);
    if (ms_session_get_stats(s, &st) != MS_OK || st.ts_prefix_kind != expect_kind) {
        fprintf(stderr, "  FAIL kind got=%d want=%d prefix=%llu\n",
                st.ts_prefix_kind, expect_kind,
                (unsigned long long)st.ts_prefix_bytes);
        g_fail++;
        ms_session_destroy(s);
        ms_synth_free(&b);
        return -1;
    }
    ms_session_destroy(s);
    ms_synth_free(&b);
    return 0;
}

static int test_session_png_jpeg_cross(void)
{
    uint8_t png[4096], jpg[4096];
    size_t pn = 0, jn = 0;
    CHECK(make_png(png, sizeof(png), 400, &pn, 0) == 0, "png");
    CHECK(pn > 188, "png >188");
    CHECK(session_with_prefix(png, pn, 188, 4096, MS_TS_PREFIX_PNG) == 0,
          "png session");
    CHECK(session_with_prefix(png, pn, 188, 1, MS_TS_PREFIX_PNG) == 0,
          "png 1-byte");
    CHECK(session_with_prefix(png, pn, 188, 17, MS_TS_PREFIX_PNG) == 0,
          "png 17-byte");
    CHECK(make_jpeg(jpg, sizeof(jpg), 400, &jn, 0) == 0, "jpg");
    CHECK(jn > 188, "jpg >188");
    CHECK(session_with_prefix(jpg, jn, 188, 33, MS_TS_PREFIX_JPEG) == 0,
          "jpeg session");
    CHECK(session_with_prefix(NULL, 0, 192, 64, MS_TS_PREFIX_NONE) == 0,
          "bare 192");
    printf("  test_session_png_jpeg_cross ok\n");
    return 0;
}

static int test_fake_sync_inside_image(void)
{
    uint8_t png[8192], jpg[8192];
    size_t pn = 0, jn = 0;
    CHECK(make_png(png, sizeof(png), 3200, &pn, 1) == 0, "png fake");
    {
        ms_synth_buf b;
        ms_ts_scan_opt opt;
        ms_ts_scan_result r;
        ms_synth_init(&b);
        CHECK(make_basic_ts(&b, 188) == 0, "synth scan");
        CHECK(ms_synth_prepend(&b, png, pn) == 0, "prepend scan");
        memset(&opt, 0, sizeof(opt));
        opt.struct_size = sizeof(opt);
        memset(&r, 0, sizeof(r));
        r.struct_size = sizeof(r);
        CHECK_ST(ms_ts_scan(b.data, b.len, 1, &opt, &r), MS_OK, "scan fake png");
        if (r.ts_offset != pn || r.kind != MS_TS_PREFIX_PNG) {
            fprintf(stderr, "  debug scan off=%zu want=%zu kind=%d psz=%u scanned=%zu\n",
                    r.ts_offset, pn, (int)r.kind, (unsigned)r.packet_size, r.scanned);
            g_fail++;
            ms_synth_free(&b);
            return -1;
        }
        ms_synth_free(&b);
    }
    CHECK(session_with_prefix(png, pn, 188, 256, MS_TS_PREFIX_PNG) == 0,
          "png fake sync rejected");
    CHECK(make_jpeg(jpg, sizeof(jpg), 3200, &jn, 1) == 0, "jpg fake");
    CHECK(session_with_prefix(jpg, jn, 188, 256, MS_TS_PREFIX_JPEG) == 0,
          "jpeg fake sync rejected");
    printf("  test_fake_sync_inside_image ok\n");
    return 0;
}

static int test_scan_limit_trunc_cancel(void)
{
    ms_session *s = NULL;
    ms_session_limits lim;
    uint8_t junk[2048];
    size_t c = 0;
    ms_synth_buf b;
    uint8_t png[512];
    size_t pn = 0;

    memset(junk, 0x11, sizeof(junk));
    ms_session_limits_default(&lim);
    lim.probe_bytes = 2048;
    CHECK_ST(ms_session_create(NULL, &lim, &s), MS_OK, "create lim");
    CHECK_ST(ms_session_push(s, junk, sizeof(junk), &c), MS_ERR_UNKNOWN, "scan cap");
    ms_session_destroy(s);

    CHECK(make_png(png, sizeof(png), 32, &pn, 0) == 0, "tiny png");
    CHECK_ST(ms_session_create(NULL, NULL, &s), MS_OK, "create trunc");
    CHECK_ST(ms_session_push(s, png, 20, &c), MS_OK, "short png"); /* 不足同步 */
    CHECK_ST(ms_session_end_of_input(s), MS_ERR_UNKNOWN, "eos unknown");
    ms_session_destroy(s);

    ms_synth_init(&b);
    CHECK(make_basic_ts(&b, 188) == 0, "synth");
    CHECK(ms_synth_prepend(&b, png, pn) == 0, "pre");
    CHECK_ST(ms_session_create(NULL, NULL, &s), MS_OK, "create cancel");
    CHECK_ST(ms_session_push(s, b.data, 30, &c), MS_OK, "prefix part");
    CHECK_ST(ms_session_cancel(s), MS_OK, "cancel");
    CHECK_ST(ms_session_push(s, b.data + 30, 40, &c), MS_ERR_CANCELED, "canceled push");
    ms_session_destroy(s);
    ms_synth_free(&b);
    printf("  test_scan_limit_trunc_cancel ok\n");
    return 0;
}

static int test_scan_again_and_names(void)
{
    uint8_t buf[200];
    ms_ts_scan_opt opt;
    ms_ts_scan_result r;
    memset(buf, 0x22, sizeof(buf));
    memset(&opt, 0, sizeof(opt));
    opt.struct_size = sizeof(opt);
    memset(&r, 0, sizeof(r));
    r.struct_size = sizeof(r);
    CHECK_ST(ms_ts_scan(buf, sizeof(buf), 0, &opt, &r), MS_AGAIN, "again");
    CHECK(strcmp(ms_ts_prefix_kind_name(MS_TS_PREFIX_PNG), "png") == 0, "name");
    printf("  test_scan_again_and_names ok\n");
    return 0;
}

int main(void)
{
    if (test_scan_bare_and_short() != 0 || test_session_png_jpeg_cross() != 0 ||
        test_fake_sync_inside_image() != 0 ||
        test_scan_limit_trunc_cancel() != 0 ||
        test_scan_again_and_names() != 0) {
        fprintf(stderr, "test_ts_scan failed (%d)\n", g_fail);
        return 1;
    }
    printf("test_ts_scan ok\n");
    return 0;
}
