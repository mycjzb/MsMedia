/*
 * 用已交付 1838 公共头编译的独立客户端：动态链接当前 libmsmedia.so /
 * libmsff.so，检查 ms_event 按值 pull 不越过旧布局，以及旧 struct_size
 * 查询与轨道枚举金丝雀。须在 ASan/UBSan 下运行。非正式四平台验收。
 * by: Long <mycjzb@gmail.com>
 */
#include "msmedia/msmedia.h"
#ifdef MS_ABI_HAVE_FF
#include "msmedia/ms_ff.h"
#endif

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__UINTPTR_MAX__)
#  define MS_ABI_PTRMAX __UINTPTR_MAX__
#elif defined(UINTPTR_MAX)
#  define MS_ABI_PTRMAX UINTPTR_MAX
#else
#  define MS_ABI_PTRMAX 0
#endif

/* 与 1838 包在本机 LP64 gcc 上测得的 sizeof(ms_event) 一致。 */
#define MS_EVENT_ABI1_LP64 176u
#define CANARY 0xA5u

static int g_fail;

static void failf(const char *msg)
{
    fprintf(stderr, "test_abi_old_header FAIL: %s\n", msg);
    g_fail = 1;
}

static void wr_be24(uint8_t *p, unsigned v)
{
    p[0] = (uint8_t)((v >> 16) & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)(v & 0xFFu);
}

static void wr_be32(uint8_t *p, unsigned v)
{
    p[0] = (uint8_t)((v >> 24) & 0xFFu);
    p[1] = (uint8_t)((v >> 16) & 0xFFu);
    p[2] = (uint8_t)((v >> 8) & 0xFFu);
    p[3] = (uint8_t)(v & 0xFFu);
}

static int add_tag(uint8_t *out, size_t cap, size_t *n, unsigned typ,
                   unsigned dts, const uint8_t *body, size_t bn)
{
    size_t need = *n + 11u + bn + 4u;
    if (need > cap) {
        return -1;
    }
    out[*n] = (uint8_t)typ;
    wr_be24(out + *n + 1, (unsigned)bn);
    wr_be24(out + *n + 4, dts & 0xFFFFFFu);
    out[*n + 7] = (uint8_t)((dts >> 24) & 0xFFu);
    wr_be24(out + *n + 8, 0);
    memcpy(out + *n + 11, body, bn);
    wr_be32(out + *n + 11 + bn, (unsigned)(11u + bn));
    *n = need;
    return 0;
}

/* 与 ms_so_abi_smoke 相同的最短合法 avcC + 长度前缀 FLV。 */
static const uint8_t k_avcc[] = {
    0x01, 0x64, 0x00, 0x1e, 0xff,
    0xe1, 0x00, 0x0b,
    0x67, 0x64, 0x00, 0x1e, 0xac, 0xc8, 0x40, 0x78, 0x9a, 0x80, 0x00,
    0x01, 0x00, 0x04,
    0x68, 0xee, 0x3c, 0x80
};

static const uint8_t k_nal[] = {
    0x00, 0x00, 0x00, 0x08,
    0x65, 0x88, 0x84, 0x00, 0x2a, 0x00, 0x00, 0x01
};

static int build_flv(uint8_t *flv, size_t cap, size_t *out_n)
{
    uint8_t vcfg[5 + sizeof(k_avcc)];
    uint8_t vnal[5 + sizeof(k_nal)];
    size_t n = 0;
    static const uint8_t hdr[] = {'F', 'L', 'V', 0x01, 0x01, 0, 0, 0, 9, 0, 0, 0, 0};
    if (cap < sizeof(hdr)) {
        return -1;
    }
    memcpy(flv, hdr, sizeof(hdr));
    n = sizeof(hdr);
    vcfg[0] = 0x17;
    vcfg[1] = 0x00;
    vcfg[2] = 0x00;
    vcfg[3] = 0x00;
    vcfg[4] = 0x00;
    memcpy(vcfg + 5, k_avcc, sizeof(k_avcc));
    vnal[0] = 0x17;
    vnal[1] = 0x01;
    vnal[2] = 0x00;
    vnal[3] = 0x00;
    vnal[4] = 0x00;
    memcpy(vnal + 5, k_nal, sizeof(k_nal));
    if (add_tag(flv, cap, &n, 9, 0, vcfg, sizeof(vcfg)) != 0) {
        return -1;
    }
    if (add_tag(flv, cap, &n, 9, 0, vnal, sizeof(vnal)) != 0) {
        return -1;
    }
    *out_n = n;
    return 0;
}

static int canary_ok(const unsigned char *p, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        if (p[i] != CANARY) {
            return 0;
        }
    }
    return 1;
}

/* 当前头加法 API 的步进核验（独立 TU，用当前头编译）。 */
int test_abi_stride_current(void);

int main(int argc, char **argv)
{
    uint8_t flv[512];
    size_t flv_n = 0;
    ms_session *s = NULL;
    ms_session_limits lim;
    ms_status st;
    size_t consumed = 0;
    int nfmt = 0, ntrk = 0, ncfg = 0, nau = 0, neos = 0;
    int i;
    ms_track_info tracks[8];
    unsigned char track_canary[16];
    uint32_t abi;

    (void)argc;
    (void)argv;
    printf("test_abi_old_header: sizeof(ms_event)=%zu (1838 headers)\n",
           sizeof(ms_event));
#if MS_ABI_PTRMAX == 0xffffffffffffffffull
    if (sizeof(ms_event) != MS_EVENT_ABI1_LP64) {
        failf("1838 LP64 sizeof(ms_event) is not 176");
    }
#endif
    abi = ms_abi_version();
    if (abi != 1u) {
        failf("MSMEDIA_ABI_VERSION contract is still 1; library must not bump it");
        fprintf(stderr, "  got abi=%u\n", (unsigned)abi);
    }

    if (build_flv(flv, sizeof(flv), &flv_n) != 0) {
        failf("build_flv");
        return 1;
    }

    memset(&lim, 0, sizeof(lim));
    lim.struct_size = sizeof(lim);
    ms_session_limits_default(&lim);
    st = ms_session_create(NULL, &lim, &s);
    if (st != MS_OK || s == NULL) {
        failf("session_create");
        return 1;
    }
    st = ms_session_push(s, flv, flv_n, &consumed);
    if (st != MS_OK && st != MS_AGAIN) {
        failf("push");
        ms_session_destroy(s);
        return 1;
    }
    (void)ms_session_end_of_input(s);

    memset(tracks, 0, sizeof(tracks));
    memset(track_canary, CANARY, sizeof(track_canary));

    for (i = 0; i < 64; i++) {
        struct {
            unsigned char before[32];
            ms_event ev;
            unsigned char after[32];
        } box;
        memset(box.before, CANARY, sizeof(box.before));
        memset(&box.ev, 0x5A, sizeof(box.ev));
        memset(box.after, CANARY, sizeof(box.after));
        st = ms_session_pull(s, &box.ev);
        if (!canary_ok(box.before, sizeof(box.before)) ||
            !canary_ok(box.after, sizeof(box.after))) {
            failf("ms_session_pull wrote past 1838 ms_event (canary)");
            break;
        }
        if (st == MS_AGAIN || st == MS_EOS) {
            break;
        }
        if (st != MS_OK) {
            failf("session_pull unexpected status");
            break;
        }
        if (box.ev.type == MS_EVENT_FORMAT) {
            nfmt++;
        } else if (box.ev.type == MS_EVENT_TRACK) {
            if (ntrk < (int)(sizeof(tracks) / sizeof(tracks[0]))) {
                tracks[ntrk] = box.ev.track;
            }
            ntrk++;
        } else if (box.ev.type == MS_EVENT_CODEC_CONFIG) {
            ncfg++;
        } else if (box.ev.type == MS_EVENT_AU) {
            nau++;
        } else if (box.ev.type == MS_EVENT_EOS) {
            neos++;
        }
        if (box.ev.release_id) {
            (void)ms_session_release(s, box.ev.release_id);
        }
    }
    if (nfmt < 1 || ntrk < 1 || ncfg < 1 || nau < 1) {
        failf("expected FORMAT/TRACK/CONFIG/AU from FLV fixture");
        fprintf(stderr, "  fmt=%d trk=%d cfg=%d au=%d\n", nfmt, ntrk, ncfg, nau);
    }
    if (!canary_ok(track_canary, sizeof(track_canary))) {
        failf("track_info copies overflowed canary");
    }
    if (ntrk >= 1 && tracks[0].kind != MS_TRACK_VIDEO) {
        failf("first TRACK should be video");
    }

    {
        struct {
            ms_session_stats st;
            unsigned char after[32];
        } sb;
        memset(&sb, 0x5A, sizeof(sb));
        memset(sb.after, CANARY, sizeof(sb.after));
        sb.st.struct_size = sizeof(sb.st);
        st = ms_session_get_stats(s, &sb.st);
        if (st != MS_OK) {
            failf("get_stats old sizeof");
        }
        if (!canary_ok(sb.after, sizeof(sb.after))) {
            failf("get_stats wrote past 1838 ms_session_stats");
        }
        if (sb.st.struct_size != sizeof(sb.st) && sb.st.has_format == 0) {
            /* 允许实现写回调用方 size 或库 size；至少必须看到已填字段。 */
        }
        if (!sb.st.has_format || !sb.st.has_track || !sb.st.has_config) {
            failf("get_stats missing expected startup flags under old struct_size");
        }
    }
    /* 更小的 struct_size（只到 has_format）不得越过调用方对象。 */
    {
        struct {
            size_t struct_size;
            uint64_t bytes_in;
            unsigned char pad[8];
            unsigned char after[32];
        } tiny;
        memset(&tiny, 0, sizeof(tiny));
        memset(tiny.after, CANARY, sizeof(tiny.after));
        memset(tiny.pad, CANARY, sizeof(tiny.pad));
        tiny.struct_size = sizeof(size_t) + sizeof(uint64_t);
        st = ms_session_get_stats(s, (ms_session_stats *)&tiny);
        if (st != MS_OK) {
            failf("get_stats tiny struct_size");
        }
        if (!canary_ok(tiny.after, sizeof(tiny.after)) ||
            !canary_ok(tiny.pad, sizeof(tiny.pad))) {
            failf("get_stats tiny struct_size overflowed canary");
        }
    }

    ms_session_destroy(s);
    s = NULL;

#ifdef MS_ABI_HAVE_FF
    if (argc >= 2 && argv[1] != NULL && argv[1][0] != 0) {
        ms_ff *f = NULL;
        ms_ff_limits flim;
        int pulls = 0, ff_au = 0;
        memset(&flim, 0, sizeof(flim));
        flim.struct_size = sizeof(flim);
        st = ms_ff_create(NULL, &flim, &f);
        if (st != MS_OK || f == NULL) {
            failf("ms_ff_create");
        } else {
            st = ms_ff_open(f, argv[1]);
            if (st != MS_OK) {
                failf("ms_ff_open fixture");
                fprintf(stderr, "  open status=%d url_len=%zu\n", (int)st, strlen(argv[1]));
            } else {
                for (i = 0; i < 128; i++) {
                    struct {
                        unsigned char before[32];
                        ms_event ev;
                        unsigned char after[32];
                    } box;
                    memset(box.before, CANARY, sizeof(box.before));
                    memset(&box.ev, 0, sizeof(box.ev));
                    memset(box.after, CANARY, sizeof(box.after));
                    st = ms_ff_pull(f, &box.ev);
                    if (!canary_ok(box.before, sizeof(box.before)) ||
                        !canary_ok(box.after, sizeof(box.after))) {
                        failf("ms_ff_pull wrote past 1838 ms_event (canary)");
                        break;
                    }
                    if (st != MS_OK) {
                        break;
                    }
                    pulls++;
                    if (box.ev.type == MS_EVENT_AU) {
                        ff_au++;
                    }
                    if (box.ev.release_id) {
                        (void)ms_ff_release(f, box.ev.release_id);
                    }
                }
                if (pulls < 1) {
                    failf("ms_ff_pull produced no events");
                }
            }
            ms_ff_destroy(f);
        }
        printf("  ff pulls=%d au=%d\n", pulls, ff_au);
    }
#else
    (void)argc;
#endif

    if (test_abi_stride_current() != 0) {
        failf("current-header stride checks");
    }

    if (g_fail) {
        fprintf(stderr, "test_abi_old_header: FAILED\n");
        return 1;
    }
    printf("test_abi_old_header: ok (1838 headers + current .so, canary intact, abi=%u)\n",
           (unsigned)abi);
    return 0;
}
