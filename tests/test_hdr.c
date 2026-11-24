/*
 * HEVC VUI/SEI 识别与 Android hdr-static-info 25 字节布局的宿主测试。
 * 手工比特流，不是软解、不是显示器 HDR 效果。
 * by: Long <mycjzb@gmail.com>
 */
#include "msmedia/msmedia.h"

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

typedef struct {
    uint8_t *p;
    size_t cap;
    size_t n;
    unsigned acc;
    int left;
} bw;

static void bw_init(bw *b, uint8_t *p, size_t cap)
{
    memset(b, 0, sizeof(*b));
    b->p = p;
    b->cap = cap;
}

static void bw_u(bw *b, int bits, unsigned v)
{
    int i;
    for (i = bits - 1; i >= 0; i--) {
        b->acc = (b->acc << 1) | ((v >> i) & 1u);
        b->left++;
        if (b->left == 8) {
            if (b->n < b->cap) {
                b->p[b->n++] = (uint8_t)b->acc;
            }
            b->acc = 0;
            b->left = 0;
        }
    }
}

static void bw_ue(bw *b, unsigned v)
{
    unsigned x = v + 1u;
    int z = 0;
    while ((x >> (z + 1)) != 0) {
        z++;
    }
    if (z) {
        bw_u(b, z, 0);
    }
    bw_u(b, z + 1, x);
}

static void bw_rbsp_trailing(bw *b)
{
    bw_u(b, 1, 1);
    while (b->left != 0) {
        bw_u(b, 1, 0);
    }
}

static size_t insert_epb(const uint8_t *in, size_t n, uint8_t *out, size_t cap)
{
    size_t i, o = 0, z = 0;
    for (i = 0; i < n; i++) {
        if (z >= 2 && in[i] <= 3) {
            if (o >= cap) {
                return 0;
            }
            out[o++] = 0x03;
            z = 0;
        }
        if (o >= cap) {
            return 0;
        }
        out[o++] = in[i];
        if (in[i] == 0) {
            z++;
        } else {
            z = 0;
        }
    }
    return o;
}

static size_t wrap_nal(unsigned nal_type, const uint8_t *rbsp, size_t rn,
                       uint8_t *out, size_t cap)
{
    uint8_t raw[512];
    uint8_t epb[640];
    size_t epn, o;
    if (rn + 2 > sizeof(raw)) {
        return 0;
    }
    raw[0] = (uint8_t)(nal_type << 1);
    raw[1] = 0x01;
    memcpy(raw + 2, rbsp, rn);
    epn = insert_epb(raw, rn + 2, epb, sizeof(epb));
    if (epn == 0 || epn + 4 > cap) {
        return 0;
    }
    out[0] = 0;
    out[1] = 0;
    out[2] = 0;
    out[3] = 1;
    memcpy(out + 4, epb, epn);
    o = 4 + epn;
    return o;
}

/* 最小 Main10 SPS + VUI 颜色描述。max_sub_layers=0，无 RPS。 */
static size_t make_sps_vui(int prim, int trc, int mat, int full_range,
                           unsigned profile, unsigned level,
                           uint8_t *out, size_t cap)
{
    uint8_t rb[128];
    bw b;
    bw_init(&b, rb, sizeof(rb));
    bw_u(&b, 4, 0); /* vps id */
    bw_u(&b, 3, 0); /* max_sub_layers_minus1 */
    bw_u(&b, 1, 1); /* temporal_id_nesting */
    bw_u(&b, 2, 0);
    bw_u(&b, 1, 0); /* tier */
    bw_u(&b, 5, profile);
    bw_u(&b, 32, 0); /* compatibility */
    bw_u(&b, 32, 0);
    bw_u(&b, 16, 0); /* constraint */
    bw_u(&b, 8, level);
    bw_ue(&b, 0); /* sps id */
    bw_ue(&b, 1); /* chroma 4:2:0 */
    bw_ue(&b, 64);
    bw_ue(&b, 64);
    bw_u(&b, 1, 0); /* no crop */
    bw_ue(&b, 2); /* 10-bit */
    bw_ue(&b, 2);
    bw_ue(&b, 0); /* log2_max_poc-4 */
    bw_u(&b, 1, 1); /* sub_layer_ordering present */
    bw_ue(&b, 0);
    bw_ue(&b, 0);
    bw_ue(&b, 0);
    bw_ue(&b, 0); /* min cb */
    bw_ue(&b, 0);
    bw_ue(&b, 0); /* min tb */
    bw_ue(&b, 0);
    bw_ue(&b, 0);
    bw_ue(&b, 0);
    bw_u(&b, 1, 0); /* scaling */
    bw_u(&b, 1, 0); /* amp */
    bw_u(&b, 1, 0); /* sao */
    bw_u(&b, 1, 0); /* pcm */
    bw_ue(&b, 0); /* num st rps */
    bw_u(&b, 1, 0); /* long term */
    bw_u(&b, 1, 0); /* temporal mvp */
    bw_u(&b, 1, 0); /* strong intra */
    bw_u(&b, 1, 1); /* vui */
    bw_u(&b, 1, 0); /* aspect */
    bw_u(&b, 1, 0); /* overscan */
    bw_u(&b, 1, 1); /* video_signal */
    bw_u(&b, 3, 5);
    bw_u(&b, 1, (unsigned)(full_range ? 1 : 0));
    bw_u(&b, 1, 1); /* colour_description */
    bw_u(&b, 8, (unsigned)prim);
    bw_u(&b, 8, (unsigned)trc);
    bw_u(&b, 8, (unsigned)mat);
    bw_rbsp_trailing(&b);
    return wrap_nal(33, rb, b.n, out, cap);
}

static void wr_be16(uint8_t *p, unsigned v)
{
    p[0] = (uint8_t)((v >> 8) & 0xFFu);
    p[1] = (uint8_t)(v & 0xFFu);
}

static void wr_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)((v >> 24) & 0xFFu);
    p[1] = (uint8_t)((v >> 16) & 0xFFu);
    p[2] = (uint8_t)((v >> 8) & 0xFFu);
    p[3] = (uint8_t)(v & 0xFFu);
}

/* BT.2020 / D65 / 1000 nit / 0.005 min，SEI 顺序 G、B、R。 */
enum {
    MS_T_GX = 8500,
    MS_T_GY = 39850,
    MS_T_BX = 6550,
    MS_T_BY = 2300,
    MS_T_RX = 35400,
    MS_T_RY = 14600,
    MS_T_WX = 15635,
    MS_T_WY = 16450,
    MS_T_MAX = 10000000u, /* 1000 cd/m^2 */
    MS_T_MIN = 50u,       /* 0.005 cd/m^2 */
    MS_T_CLL = 1000,
    MS_T_FALL = 400
};

static size_t make_sei(int with_mastering, int with_cll, int hdr10plus, int dv_t35,
                       uint8_t *out, size_t cap)
{
    uint8_t payload[96];
    uint8_t rb[128];
    size_t pn = 0, rn = 0;

    if (with_mastering) {
        payload[pn++] = 137; /* type < 255 */
        payload[pn++] = 24;  /* size */
        wr_be16(payload + pn, MS_T_GX);
        wr_be16(payload + pn + 2, MS_T_GY);
        wr_be16(payload + pn + 4, MS_T_BX);
        wr_be16(payload + pn + 6, MS_T_BY);
        wr_be16(payload + pn + 8, MS_T_RX);
        wr_be16(payload + pn + 10, MS_T_RY);
        wr_be16(payload + pn + 12, MS_T_WX);
        wr_be16(payload + pn + 14, MS_T_WY);
        wr_be32(payload + pn + 16, MS_T_MAX);
        wr_be32(payload + pn + 20, MS_T_MIN);
        pn += 24;
    }
    if (with_cll) {
        payload[pn++] = 144;
        payload[pn++] = 4;
        wr_be16(payload + pn, MS_T_CLL);
        wr_be16(payload + pn + 2, MS_T_FALL);
        pn += 4;
    }
    if (hdr10plus) {
        /* user_data_registered_itu_t_t35: country 0xB5, provider 0x003C */
        payload[pn++] = 4;
        payload[pn++] = 6;
        payload[pn++] = 0xB5;
        payload[pn++] = 0x00;
        payload[pn++] = 0x3C;
        payload[pn++] = 0x00;
        payload[pn++] = 0x01;
        payload[pn++] = 0x04;
    }
    if (dv_t35) {
        payload[pn++] = 4;
        payload[pn++] = 6;
        payload[pn++] = 0xB5;
        payload[pn++] = 0x00;
        payload[pn++] = 0x00;
        payload[pn++] = 0x00;
        payload[pn++] = 0x00;
        payload[pn++] = 0x00;
    }
    payload[pn++] = 0x80; /* rbsp trailing */
    memcpy(rb, payload, pn);
    rn = pn;
    return wrap_nal(39, rb, rn, out, cap);
}

static unsigned rd_le16(const uint8_t *p)
{
    return (unsigned)p[0] | ((unsigned)p[1] << 8);
}

static int test_pq_vui_sei_and_static_info(void)
{
    uint8_t buf[1024];
    size_t n = 0, k, sta_n = 0;
    uint8_t sta[MS_HDR_STATIC_INFO_SIZE];
    ms_hdr_info hi;
    n = make_sps_vui(9, 16, 9, 0, 2, 150, buf, sizeof(buf));
    CHECK(n > 8, "pq sps generated");
    k = make_sei(1, 1, 0, 0, buf + n, sizeof(buf) - n);
    CHECK(k > 8, "sei generated");
    n += k;
    CHECK_ST(ms_hdr_parse_hevc(MS_SAMPLE_FMT_ANNEXB, 0, buf, n, &hi), MS_OK, "parse pq");
    CHECK(hi.kind == MS_HDR_KIND_HDR10, "kind hdr10");
    CHECK(hi.color_primaries == 9 && hi.color_transfer == 16 && hi.color_matrix == 9,
          "bt2020 pq matrix");
    CHECK(hi.color_range == 1, "limited range");
    CHECK(hi.bit_depth == 10, "10bit");
    CHECK(hi.width == 64 && hi.height == 64, "sps size");
    CHECK(hi.profile_idc == 2 && hi.level_idc == 150, "main10 level5");
    CHECK(hi.from_vui && hi.from_sei, "from vui+sei");
    CHECK(hi.have_mastering && hi.have_cll, "mastering+cll");
    CHECK(hi.display_primaries_x[0] == MS_T_GX && hi.display_primaries_y[0] == MS_T_GY, "G");
    CHECK(hi.display_primaries_x[1] == MS_T_BX && hi.display_primaries_y[1] == MS_T_BY, "B");
    CHECK(hi.display_primaries_x[2] == MS_T_RX && hi.display_primaries_y[2] == MS_T_RY, "R");
    CHECK(hi.white_point_x == MS_T_WX && hi.white_point_y == MS_T_WY, "white");
    CHECK(hi.max_display_mastering_luminance == MS_T_MAX, "max lum");
    CHECK(hi.min_display_mastering_luminance == MS_T_MIN, "min lum");
    CHECK(hi.max_content_light_level == MS_T_CLL, "cll");
    CHECK(hi.max_frame_average_light_level == MS_T_FALL, "fall");
    CHECK_ST(ms_hdr_pack_static_info(&hi, sta, sizeof(sta), &sta_n), MS_OK, "pack");
    CHECK(sta_n == 25 && sta[0] == 1, "type1 25 bytes");
    /* Android / CTA-861.3 为 R、G、B */
    CHECK(rd_le16(sta + 1) == MS_T_RX && rd_le16(sta + 3) == MS_T_RY, "pack R");
    CHECK(rd_le16(sta + 5) == MS_T_GX && rd_le16(sta + 7) == MS_T_GY, "pack G");
    CHECK(rd_le16(sta + 9) == MS_T_BX && rd_le16(sta + 11) == MS_T_BY, "pack B");
    CHECK(rd_le16(sta + 13) == MS_T_WX && rd_le16(sta + 15) == MS_T_WY, "pack white");
    CHECK(rd_le16(sta + 17) == 1000, "max nits 1 cd/m2");
    CHECK(rd_le16(sta + 19) == 50, "min 0.0001 units");
    CHECK(rd_le16(sta + 21) == 1000 && rd_le16(sta + 23) == 400, "cll/fall");
    CHECK(ms_hdr_android_color_standard(&hi) == 6, "standard bt2020");
    CHECK(ms_hdr_android_color_transfer(&hi) == 6, "transfer st2084");
    CHECK(ms_hdr_android_color_range(&hi) == 2, "range limited");
    CHECK(ms_hdr_android_hevc_profile(&hi) == 2, "Main10 (HDR10 uses color-transfer)");
    CHECK(ms_hdr_android_hevc_level(&hi) == 16384, "level 5.0 main");
    CHECK(strcmp(ms_hdr_kind_name(hi.kind), "hdr10") == 0, "name");
    {
        uint8_t tiny[24];
        size_t zn = 0;
        CHECK_ST(ms_hdr_pack_static_info(&hi, tiny, sizeof(tiny), &zn), MS_ERR_OVERFLOW,
                 "pack overflow");
    }
    printf("  ok pq vui/sei + static-info layout\n");
    return 0;
}

static int test_hlg_vui(void)
{
    uint8_t buf[512];
    size_t n;
    ms_hdr_info hi;
    uint8_t sta[25];
    size_t sta_n = 0;
    n = make_sps_vui(9, 18, 9, 1, 2, 153, buf, sizeof(buf));
    CHECK(n > 8, "hlg sps");
    CHECK_ST(ms_hdr_parse_hevc(MS_SAMPLE_FMT_ANNEXB, 1, buf, n, &hi), MS_OK, "parse hlg");
    CHECK(hi.kind == MS_HDR_KIND_HLG, "kind hlg");
    CHECK(hi.color_transfer == 18 && hi.color_primaries == 9, "hlg bt2020");
    CHECK(hi.color_range == 2, "full range");
    CHECK(ms_hdr_android_color_transfer(&hi) == 7, "transfer hlg");
    CHECK(ms_hdr_android_color_range(&hi) == 1, "android full");
    CHECK(ms_hdr_android_hevc_profile(&hi) == 2, "Main10");
    CHECK(ms_hdr_android_hevc_level(&hi) == 65536, "level 5.1");
    CHECK_ST(ms_hdr_pack_static_info(&hi, sta, sizeof(sta), &sta_n), MS_AGAIN,
             "hlg no static");
    CHECK(strcmp(ms_hdr_kind_name(hi.kind), "hlg") == 0, "name hlg");
    printf("  ok hlg vui (no static-info)\n");
    return 0;
}

static int test_len_pref_and_hvcc(void)
{
    uint8_t annex[512], lenb[512], hvcc[640];
    size_t n, nal_n, off;
    ms_hdr_info a, b, c;
    n = make_sps_vui(9, 16, 9, 0, 2, 150, annex, sizeof(annex));
    CHECK(n > 6 && annex[0] == 0 && annex[3] == 1, "annex start");
    nal_n = n - 4;
    /* 4 字节长度前缀 AU */
    lenb[0] = (uint8_t)((nal_n >> 24) & 0xFFu);
    lenb[1] = (uint8_t)((nal_n >> 16) & 0xFFu);
    lenb[2] = (uint8_t)((nal_n >> 8) & 0xFFu);
    lenb[3] = (uint8_t)(nal_n & 0xFFu);
    memcpy(lenb + 4, annex + 4, nal_n);
    CHECK_ST(ms_hdr_parse_hevc(MS_SAMPLE_FMT_HVCC, 0, lenb, 4 + nal_n, &b), MS_OK,
             "len parse");
    CHECK(b.kind == MS_HDR_KIND_HDR10, "len hdr10");
    /* 最小 hvcC：version=1，lengthSize=4，1 个 SPS 数组 */
    memset(hvcc, 0, sizeof(hvcc));
    hvcc[0] = 1;
    hvcc[21] = 0xFF; /* reserved + lengthSizeMinusOne=3 */
    hvcc[22] = 1;    /* one array */
    hvcc[23] = (uint8_t)(0x80u | 33u);
    hvcc[24] = 0;
    hvcc[25] = 1; /* num nal */
    hvcc[26] = (uint8_t)((nal_n >> 8) & 0xFFu);
    hvcc[27] = (uint8_t)(nal_n & 0xFFu);
    memcpy(hvcc + 28, annex + 4, nal_n);
    off = 28 + nal_n;
    CHECK_ST(ms_hdr_parse_hevc(MS_SAMPLE_FMT_HVCC, 1, hvcc, off, &c), MS_OK, "hvcc parse");
    CHECK(c.kind == MS_HDR_KIND_HDR10 && c.color_transfer == 16, "hvcc pq");
    CHECK_ST(ms_hdr_parse_hevc(MS_SAMPLE_FMT_ANNEXB, 0, annex, n, &a), MS_OK, "annex");
    CHECK(a.color_primaries == c.color_primaries, "hvcc matches annex");
    printf("  ok length-prefix + hvcC\n");
    return 0;
}

static int test_merge_and_detect(void)
{
    ms_hdr_info dst, src, empty;
    uint8_t buf[256], sta[25];
    size_t n, sta_n = 0;
    ms_hdr_info_default(&dst);
    ms_hdr_info_default(&src);
    src.have_color = 1;
    src.color_transfer = 16;
    src.color_primaries = 9;
    src.from_container = 1;
    ms_hdr_classify(&src);
    ms_hdr_merge(&dst, &src);
    CHECK(dst.kind == MS_HDR_KIND_HDR10 && dst.from_container, "merge container");
    n = make_sei(1, 0, 1, 1, buf, sizeof(buf));
    CHECK(n > 0, "sei+plus+dv");
    CHECK_ST(ms_hdr_parse_hevc(MS_SAMPLE_FMT_ANNEXB, 0, buf, n, &src), MS_OK, "parse sei");
    CHECK(src.have_mastering && src.have_hdr10_plus && src.dolby_vision, "plus+dv flags");
    /* HDR10+ 仅识别，不因动态元数据改变 pack；DV 只报告 */
    ms_hdr_merge(&dst, &src);
    CHECK(dst.have_mastering && dst.have_hdr10_plus && dst.dolby_vision, "merged flags");
    CHECK_ST(ms_hdr_pack_static_info(&dst, sta, sizeof(sta), &sta_n), MS_OK, "pack after merge");
    ms_hdr_info_default(&empty);
    CHECK_ST(ms_hdr_pack_static_info(&empty, sta, sizeof(sta), &sta_n), MS_AGAIN, "empty");
    CHECK_ST(ms_hdr_parse_hevc(MS_SAMPLE_FMT_ANNEXB, 0, NULL, 0, &empty),
             MS_ERR_INVALID_ARG, "null data");
    printf("  ok merge / hdr10+ detect / dv detect (no passthrough claim)\n");
    return 0;
}

static int test_sdr_bt709(void)
{
    uint8_t buf[512];
    size_t n;
    ms_hdr_info hi;
    n = make_sps_vui(1, 1, 1, 0, 1, 123, buf, sizeof(buf));
    CHECK(n > 8, "sdr sps");
    CHECK_ST(ms_hdr_parse_hevc(MS_SAMPLE_FMT_ANNEXB, 0, buf, n, &hi), MS_OK, "parse sdr");
    CHECK(hi.kind == MS_HDR_KIND_SDR, "kind sdr");
    CHECK(ms_hdr_android_color_standard(&hi) == 1, "bt709");
    CHECK(ms_hdr_android_color_transfer(&hi) == 3, "sdr video");
    /* 手写 SPS 仍为 10bit，profile 走 Main10；不是 8bit Main。 */
    CHECK(ms_hdr_android_hevc_profile(&hi) == 2, "10bit Main10");
    printf("  ok sdr bt709\n");
    return 0;
}

int main(void)
{
    printf("test_hdr: handwritten HEVC VUI/SEI (not a display test)\n");
    if (test_pq_vui_sei_and_static_info() != 0) {
        fprintf(stderr, "pq/static failed\n");
    }
    if (test_hlg_vui() != 0) {
        fprintf(stderr, "hlg failed\n");
    }
    if (test_len_pref_and_hvcc() != 0) {
        fprintf(stderr, "len/hvcc failed\n");
    }
    if (test_merge_and_detect() != 0) {
        fprintf(stderr, "merge/detect failed\n");
    }
    if (test_sdr_bt709() != 0) {
        fprintf(stderr, "sdr failed\n");
    }
    if (g_fail) {
        fprintf(stderr, "test_hdr: %d FAIL\n", g_fail);
        return 1;
    }
    printf("test_hdr: all passed\n");
    return 0;
}
