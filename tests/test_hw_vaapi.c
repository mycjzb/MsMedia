/*
 * Linux VAAPI：码流参数解析与 DRM 探测。本机无显示设备时必须诚实
 * UNSUPPORTED / is_hardware=0。不是设备硬解出画，也不是 mock 冒充。
 * by: Long <mycjzb@gmail.com>
 */
#include "msmedia/ms_hw.h"
#include "hevc_fixtures.h"
#include "ms_vbit_fixtures.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond, msg)                                                         \
    do {                                                                         \
        if (!(cond)) {                                                           \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, msg);        \
            failures++;                                                          \
            return -1;                                                           \
        }                                                                        \
    } while (0)

#define CHECK_ST(st, expect, msg)                                                \
    do {                                                                         \
        ms_status _s = (st);                                                     \
        if (_s != (expect)) {                                                    \
            fprintf(stderr, "FAIL %s:%d: %s got %d expect %d\n", __FILE__,       \
                    __LINE__, msg, (int)_s, (int)(expect));                      \
            failures++;                                                          \
            return -1;                                                           \
        }                                                                        \
    } while (0)

static const uint8_t k_avcc_cbp[] = {
    0x01, 0x42, 0xC0, 0x1E, 0xFF, 0xE1, 0x00, 0x08,
    0x67, 0x42, 0xC0, 0x1E, 0xDA, 0x01, 0x00, 0x00,
    0x01, 0x00, 0x05, 0x68, 0xCE, 0x38, 0x80, 0x00
};

/* Annex B：仅 SPS。profile / constraint / chroma / bit depth 已按字段装箱。 */
static const uint8_t k_sps_baseline[] = {
    0x00, 0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x1E, 0x80
};
static const uint8_t k_sps_main[] = {
    0x00, 0x00, 0x00, 0x01, 0x67, 0x4D, 0x40, 0x1E, 0x80
};
static const uint8_t k_sps_high[] = {
    0x00, 0x00, 0x00, 0x01, 0x67, 0x64, 0x00, 0x1E, 0xAF
};
static const uint8_t k_sps_high10[] = {
    0x00, 0x00, 0x00, 0x01, 0x67, 0x6E, 0x00, 0x1E, 0xA6, 0xC0
};
static const uint8_t k_sps_high422[] = {
    0x00, 0x00, 0x00, 0x01, 0x67, 0x7A, 0x00, 0x1E, 0xBF
};

static int expect_map(ms_codec codec, ms_sample_format fmt, const uint8_t *data,
                      size_t size, int va_profile, unsigned profile_idc,
                      const char *tag)
{
    ms_hw_vaapi_stream st;
    ms_hw_vaapi_stream_default(&st);
    CHECK_ST(ms_hw_vaapi_parse_stream(codec, fmt, data, size, &st), MS_OK, tag);
    CHECK(st.complete == 1, "complete");
    CHECK(st.mapped == 1, "mapped");
    CHECK(st.va_profile == va_profile, "va profile");
    CHECK(st.bitstream_profile == profile_idc, "bitstream profile");
    CHECK(st.profile_name != NULL && strcmp(st.profile_name, "none") != 0, "name");
    return 0;
}

static int expect_unmapped(ms_codec codec, ms_sample_format fmt, const uint8_t *data,
                           size_t size, const char *tag)
{
    ms_hw_vaapi_stream st;
    ms_hw_vaapi_stream_default(&st);
    CHECK_ST(ms_hw_vaapi_parse_stream(codec, fmt, data, size, &st), MS_OK, tag);
    CHECK(st.complete == 1, "complete but unmapped");
    CHECK(st.mapped == 0, "not mapped");
    CHECK(st.va_profile == -1, "no va profile");
    return 0;
}

static int test_parse_h264(void)
{
    CHECK(expect_map(MS_CODEC_H264, MS_SAMPLE_FMT_AVCC, k_avcc_cbp, sizeof(k_avcc_cbp),
                     13 /* ConstrainedBaseline */, 66, "avcc cbp") == 0,
          "avcc cbp");
    CHECK(expect_map(MS_CODEC_H264, MS_SAMPLE_FMT_ANNEXB, k_sps_baseline,
                     sizeof(k_sps_baseline), 5 /* deprecated Baseline */, 66,
                     "unconstrained baseline") == 0,
          "baseline");
    CHECK(expect_map(MS_CODEC_H264, MS_SAMPLE_FMT_ANNEXB, k_sps_main, sizeof(k_sps_main),
                     6, 77, "main") == 0,
          "main");
    CHECK(expect_map(MS_CODEC_H264, MS_SAMPLE_FMT_ANNEXB, k_sps_high, sizeof(k_sps_high),
                     7, 100, "high") == 0,
          "high");
    CHECK(expect_map(MS_CODEC_H264, MS_SAMPLE_FMT_ANNEXB, k_sps_high10,
                     sizeof(k_sps_high10), 36, 110, "high10") == 0,
          "high10");
    CHECK(expect_unmapped(MS_CODEC_H264, MS_SAMPLE_FMT_ANNEXB, k_sps_high422,
                          sizeof(k_sps_high422), "high422") == 0,
          "high422 unmapped");
    {
        ms_hw_vaapi_stream st;
        uint8_t junk[8] = { 0x00, 0x00, 0x00, 0x01, 0x65, 0x88, 0x00, 0x00 };
        ms_hw_vaapi_stream_default(&st);
        CHECK_ST(ms_hw_vaapi_parse_stream(MS_CODEC_H264, MS_SAMPLE_FMT_ANNEXB, junk,
                                          sizeof(junk), &st),
                 MS_ERR_MALFORMED, "slice nal is not a profile");
        CHECK(st.mapped == 0, "junk not mapped");
        ms_hw_vaapi_stream_default(&st);
        CHECK_ST(ms_hw_vaapi_parse_stream(MS_CODEC_H264, MS_SAMPLE_FMT_AVCC, NULL, 0,
                                          &st),
                 MS_ERR_MALFORMED, "empty not mime-mapped");
    }
    printf("  test_parse_h264 ok\n");
    return 0;
}

static int test_parse_other(void)
{
    uint8_t hevc_csd[128];
    size_t n = 0;
    CHECK(hevc_annexb_csd(hevc_csd, sizeof(hevc_csd), &n, k_hevc_vps_16,
                          sizeof(k_hevc_vps_16), k_hevc_sps_16, sizeof(k_hevc_sps_16),
                          k_hevc_pps_16, sizeof(k_hevc_pps_16)) == 0,
          "hevc csd pack");
    CHECK(expect_map(MS_CODEC_HEVC, MS_SAMPLE_FMT_ANNEXB, hevc_csd, n, 17, 1,
                     "hevc main") == 0,
          "hevc main");
    CHECK(expect_map(MS_CODEC_MPEG2_VIDEO, MS_SAMPLE_FMT_MPV, k_mpv_csd_16,
                     sizeof(k_mpv_csd_16), 1, 4, "mpeg2 main") == 0,
          "mpeg2");
    CHECK(expect_map(MS_CODEC_MPEG4_PART2, MS_SAMPLE_FMT_MP4V, k_mp4v_csd_16,
                     sizeof(k_mp4v_csd_16), 2, 1, "mpeg4 simple") == 0,
          "mpeg4");
    CHECK(expect_map(MS_CODEC_H263, MS_SAMPLE_FMT_H263, k_h263_qcif_i,
                     sizeof(k_h263_qcif_i), 11, 2, "h263 baseline") == 0,
          "h263");
    CHECK(expect_unmapped(MS_CODEC_H263, MS_SAMPLE_FMT_H263, k_h263_plusptype,
                          sizeof(k_h263_plusptype), "h263 plus") == 0,
          "h263 plus");
    CHECK(expect_map(MS_CODEC_AV1, MS_SAMPLE_FMT_AV1, k_av1_av1c_64, sizeof(k_av1_av1c_64),
                     32, 0, "av1c p0") == 0,
          "av1 av1c");
    CHECK(expect_map(MS_CODEC_AV1, MS_SAMPLE_FMT_AV1, k_av1_sh_64, sizeof(k_av1_sh_64), 32,
                     0, "av1 sh") == 0,
          "av1 sh");
    CHECK(expect_unmapped(MS_CODEC_AVS_PLUS, MS_SAMPLE_FMT_CAVS, k_avsplus_seq_16,
                          sizeof(k_avsplus_seq_16), "avs+ no va profile") == 0,
          "avs+");
    {
        ms_hw_vaapi_stream st;
        ms_hw_vaapi_stream_default(&st);
        CHECK_ST(ms_hw_vaapi_parse_stream(MS_CODEC_AVS_PLUS, MS_SAMPLE_FMT_CAVS,
                                          k_avs_jizhun_seq_16, sizeof(k_avs_jizhun_seq_16),
                                          &st),
                 MS_ERR_MALFORMED, "jizhun is not avs+");
    }
    printf("  test_parse_other ok\n");
    return 0;
}

static int test_probe_and_query(void)
{
    ms_hw_vaapi_probe pr;
    ms_hw_caps caps;
    ms_hw_vaapi_probe_default(&pr);
    CHECK_ST(ms_hw_vaapi_probe_device(&pr), MS_OK, "probe");
    CHECK(pr.verified == 1, "probe verified");
    CHECK(pr.is_hardware == 0, "probe not hardware");
    CHECK(pr.decode_implemented == 0, "decode not implemented");
    CHECK(pr.have_device == 0, "this host has no usable DRM device");
    CHECK(pr.backend != NULL && strcmp(pr.backend, "vaapi") == 0, "backend vaapi");
    CHECK(pr.detail != NULL && strstr(pr.detail, "not implemented") != NULL,
          "probe detail");
    /* 本机无 /dev/dri：应明确写出，而不是假装已打开设备。 */
    CHECK(pr.device_path[0] == 0, "no device path");
    CHECK(strstr(pr.detail, "/dev/dri") != NULL || strstr(pr.detail, "DRM") != NULL,
          "mentions missing DRM");
    CHECK(pr.libva_loaded == 1, "libva.so.2 should load on this host");

    memset(&caps, 0, sizeof(caps));
    caps.struct_size = sizeof(caps);
    CHECK_ST(ms_hw_decoder_query(MS_CODEC_H264, &caps), MS_OK, "query h264");
    CHECK(caps.is_hardware == 0 && caps.verified == 1, "query not hw");
    CHECK(caps.kind == MS_HW_KIND_NONE, "kind none");
    CHECK(caps.backend != NULL && strcmp(caps.backend, "vaapi") == 0, "query backend");
    memset(&caps, 0, sizeof(caps));
    caps.struct_size = sizeof(caps);
    CHECK_ST(ms_hw_decoder_query(MS_CODEC_AVS_PLUS, &caps), MS_OK, "query avs");
    CHECK(caps.is_hardware == 0, "avs not hw by mime");
    CHECK(ms_hw_vaapi_probe_has_vld(&pr, 13) == 0, "no vld without device");
    printf("  test_probe_and_query ok (libva_loaded=%d have_device=%d)\n",
           pr.libva_loaded, pr.have_device);
    return 0;
}

static int test_configure_default(void)
{
    ms_hw_decoder *d = NULL;
    ms_hw_create_opts o;
    ms_hw_config cfg;
    ms_hw_caps caps;
    int token = 1;
    memset(&o, 0, sizeof(o));
    o.struct_size = sizeof(o);
    o.mock_mode = MS_HW_MOCK_DEFAULT;
    CHECK_ST(ms_hw_decoder_create(NULL, NULL, &o, &d), MS_OK, "create");
    memset(&cfg, 0, sizeof(cfg));
    cfg.struct_size = sizeof(cfg);
    cfg.codec = MS_CODEC_H264;
    cfg.sample_fmt = MS_SAMPLE_FMT_AVCC;
    cfg.csd = k_avcc_cbp;
    cfg.csd_size = sizeof(k_avcc_cbp);
    cfg.width = 16;
    cfg.height = 16;
    cfg.surface = &token;
    CHECK_ST(ms_hw_decoder_configure(d, &cfg), MS_ERR_UNSUPPORTED, "default vaapi refuse");
    memset(&caps, 0, sizeof(caps));
    caps.struct_size = sizeof(caps);
    CHECK_ST(ms_hw_decoder_get_caps(d, &caps), MS_OK, "caps");
    CHECK(caps.is_hardware == 0, "configure not hardware");
    CHECK(caps.verified == 1, "verified");
    CHECK(caps.backend != NULL && strcmp(caps.backend, "vaapi") == 0, "vaapi backend");
    CHECK(caps.detail != NULL && strstr(caps.detail, "H264ConstrainedBaseline") != NULL,
          "detail has parsed profile");
    CHECK(strstr(caps.detail, "not implemented") != NULL ||
              strstr(caps.detail, "no /dev/dri") != NULL ||
              strstr(caps.detail, "DRM") != NULL,
          "detail explains refusal");
    ms_hw_decoder_destroy(d);

    /* mock 路径仍独立，不得被 VAAPI 探测替换。 */
    d = NULL;
    o.mock_mode = MS_HW_MOCK_AS_HW;
    CHECK_ST(ms_hw_decoder_create(NULL, NULL, &o, &d), MS_OK, "create mock");
    CHECK_ST(ms_hw_decoder_configure(d, &cfg), MS_OK, "mock still works");
    memset(&caps, 0, sizeof(caps));
    caps.struct_size = sizeof(caps);
    CHECK_ST(ms_hw_decoder_get_caps(d, &caps), MS_OK, "mock caps");
    CHECK(caps.backend != NULL && strcmp(caps.backend, "mock") == 0, "mock backend");
    CHECK(caps.is_hardware == 1, "mock as hw flag");
    CHECK(caps.detail != NULL && strstr(caps.detail, "not a device") != NULL,
          "mock not a device");
    ms_hw_decoder_destroy(d);
    printf("  test_configure_default ok\n");
    return 0;
}

int main(void)
{
    if (test_parse_h264() != 0 || test_parse_other() != 0 ||
        test_probe_and_query() != 0 || test_configure_default() != 0) {
        fprintf(stderr, "test_hw_vaapi FAILED (%d)\n", failures);
        return 1;
    }
    if (failures != 0) {
        fprintf(stderr, "test_hw_vaapi FAILED (%d)\n", failures);
        return 1;
    }
    printf("test_hw_vaapi ok (parse+probe; not device decode or display)\n");
    return 0;
}
