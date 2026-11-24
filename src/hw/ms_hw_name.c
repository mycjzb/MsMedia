/*
 * 解码器组件名分类。名称启发式与公开的 Media3 MediaCodecUtil
 * isSoftwareOnly / isHardwareAccelerated（API < 29）同类，不是抄录其源码，
 * 也不等同 Media3 渲染器。API >= 29 优先采用平台 isHardwareAccelerated /
 * isSoftwareOnly 标志。
 * by: Long <mycjzb@gmail.com>
 */
#include "ms_hw_internal.h"

#include <string.h>

int ms_hw_ascii_ieq(const char *a, const char *b)
{
    unsigned char ca, cb;
    if (a == NULL || b == NULL) {
        return a == b;
    }
    while (*a && *b) {
        ca = (unsigned char)*a++;
        cb = (unsigned char)*b++;
        if (ca >= 'A' && ca <= 'Z') ca = (unsigned char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (unsigned char)(cb - 'A' + 'a');
        if (ca != cb) {
            return 0;
        }
    }
    return *a == 0 && *b == 0;
}

int ms_hw_ascii_ieq_prefix(const char *s, const char *pfx)
{
    unsigned char a, b;
    if (s == NULL || pfx == NULL) {
        return 0;
    }
    while (*pfx) {
        a = (unsigned char)*s++;
        b = (unsigned char)*pfx++;
        if (a >= 'A' && a <= 'Z') {
            a = (unsigned char)(a - 'A' + 'a');
        }
        if (b >= 'A' && b <= 'Z') {
            b = (unsigned char)(b - 'A' + 'a');
        }
        if (a != b) {
            return 0;
        }
    }
    return 1;
}

int ms_hw_ascii_icontains(const char *s, const char *needle)
{
    size_t i, j, n, m;
    if (s == NULL || needle == NULL) {
        return 0;
    }
    n = strlen(s);
    m = strlen(needle);
    if (m == 0 || m > n) {
        return 0;
    }
    for (i = 0; i + m <= n; i++) {
        int ok = 1;
        for (j = 0; j < m; j++) {
            unsigned char a = (unsigned char)s[i + j];
            unsigned char b = (unsigned char)needle[j];
            if (a >= 'A' && a <= 'Z') {
                a = (unsigned char)(a - 'A' + 'a');
            }
            if (b >= 'A' && b <= 'Z') {
                b = (unsigned char)(b - 'A' + 'a');
            }
            if (a != b) {
                ok = 0;
                break;
            }
        }
        if (ok) {
            return 1;
        }
    }
    return 0;
}

int ms_hw_is_system_software_codec_name(const char *name)
{
    if (name == NULL || name[0] == 0) {
        return 0;
    }
    /* 仅 Android 系统/Google 软件组件，不含 omx.ffmpeg 等第三方软解。 */
    return ms_hw_ascii_ieq_prefix(name, "omx.google.") ||
           ms_hw_ascii_ieq_prefix(name, "c2.android.") ||
           ms_hw_ascii_ieq_prefix(name, "c2.google.");
}

ms_hw_kind ms_hw_classify_codec_name(const char *name, int api_level,
                                     int hw_flag, int sw_flag)
{
    if (name == NULL || name[0] == 0) {
        return MS_HW_KIND_NONE;
    }
    if (api_level >= 29 && (hw_flag == 0 || hw_flag == 1) &&
        (sw_flag == 0 || sw_flag == 1)) {
        if (hw_flag == 1 && sw_flag == 0) {
            return MS_HW_KIND_HARDWARE;
        }
        if (sw_flag == 1) {
            return MS_HW_KIND_SOFTWARE;
        }
        return MS_HW_KIND_SYSTEM_UNVERIFIED;
    }
    /* 与 Media3 公开启发式同类的软件名（小写比较）。 */
    if (ms_hw_ascii_ieq_prefix(name, "omx.google.") ||
        ms_hw_ascii_ieq_prefix(name, "c2.android.") ||
        ms_hw_ascii_ieq_prefix(name, "c2.google.") ||
        ms_hw_ascii_ieq_prefix(name, "omx.ffmpeg.") ||
        ms_hw_ascii_ieq_prefix(name, "omx.k3.ffmpeg.") ||
        ms_hw_ascii_ieq_prefix(name, "omx.avcodec.") ||
        ms_hw_ascii_ieq_prefix(name, "omx.qcom.video.decoder.hevcswvdec") ||
        ms_hw_ascii_ieq_prefix(name, "omx.qti.video.decoder.hevcswvdec") ||
        ms_hw_ascii_ieq_prefix(name, "c2.qti.hevc.decoder.sw") ||
        (ms_hw_ascii_ieq_prefix(name, "omx.sec.") &&
         ms_hw_ascii_icontains(name, ".sw."))) {
        return MS_HW_KIND_SOFTWARE;
    }
    if (ms_hw_ascii_ieq_prefix(name, "omx.") ||
        ms_hw_ascii_ieq_prefix(name, "c2.")) {
        return MS_HW_KIND_HARDWARE;
    }
    /* 未知命名空间：按软件/未核实处理，不得报硬解。 */
    return MS_HW_KIND_SOFTWARE;
}

const char *ms_hw_kind_name(ms_hw_kind kind)
{
    switch (kind) {
    case MS_HW_KIND_NONE:
        return "none";
    case MS_HW_KIND_HARDWARE:
        return "hardware";
    case MS_HW_KIND_SOFTWARE:
        return "software";
    case MS_HW_KIND_SYSTEM_UNVERIFIED:
        return "system_unverified";
    default:
        return "unknown";
    }
}

const char *ms_hw_video_mime(ms_codec codec)
{
    if (codec == MS_CODEC_HEVC) {
        return "video/hevc";
    }
    if (codec == MS_CODEC_MPEG2_VIDEO) {
        return "video/mpeg2";
    }
    if (codec == MS_CODEC_MPEG4_PART2) {
        return "video/mp4v-es";
    }
    if (codec == MS_CODEC_H263) {
        return "video/3gpp";
    }
    if (codec == MS_CODEC_AV1) {
        return "video/av01";
    }
    /* AOSP MediaFormat / 官方 Supported media formats 无 AVS+ MIME。
     * 不得把 OpenHarmony 的 "video/cavs" 或其它 OEM 名写成 Android 普遍存在的类型。 */
    if (codec == MS_CODEC_AVS_PLUS) {
        return NULL;
    }
    return "video/avc";
}

const char *ms_hw_video_label(ms_codec codec)
{
    if (codec == MS_CODEC_HEVC) {
        return "HEVC";
    }
    if (codec == MS_CODEC_MPEG2_VIDEO) {
        return "MPEG-2";
    }
    if (codec == MS_CODEC_MPEG4_PART2) {
        return "MPEG-4";
    }
    if (codec == MS_CODEC_H263) {
        return "H.263";
    }
    if (codec == MS_CODEC_AV1) {
        return "AV1";
    }
    if (codec == MS_CODEC_H264) {
        return "AVC";
    }
    if (codec == MS_CODEC_AVS_PLUS) {
        return "AVS+";
    }
    return "video";
}

const char *ms_hw_audio_mime(ms_codec codec)
{
    if (codec == MS_CODEC_AC3) {
        return "audio/ac3";
    }
#ifdef MS_SINGLE_SO
    if (codec == MS_CODEC_EAC3) {
        return "audio/eac3";
    }
    if (codec == MS_CODEC_DTS) {
        return "audio/vnd.dts";
    }
    /* AAC-LATM：系统 MediaCodec 只吃裸 AAC，不给 mime，直接走 FFmpeg aac_latm 兜底 */
#endif
    if (codec == MS_CODEC_MP3) {
        return "audio/mpeg";
    }
    if (codec == MS_CODEC_MP2) {
        return "audio/mpeg-L2";
    }
    if (codec == MS_CODEC_OPUS) {
        return "audio/opus";
    }
    if (codec == MS_CODEC_FLAC) {
        return "audio/flac";
    }
    return "audio/mp4a-latm";
}

const char *ms_hw_audio_label(ms_codec codec)
{
    if (codec == MS_CODEC_AC3) {
        return "AC-3";
    }
    if (codec == MS_CODEC_MP3) {
        return "MP3";
    }
    if (codec == MS_CODEC_MP2) {
        return "MP2";
    }
    if (codec == MS_CODEC_OPUS) {
        return "Opus";
    }
    if (codec == MS_CODEC_AAC) {
        return "AAC";
    }
    if (codec == MS_CODEC_FLAC) {
        return "FLAC";
    }
    if (codec == MS_CODEC_PCM) {
        return "PCM";
    }
    return "audio";
}

/* ---- AVS/AVS+ 解码器挑选策略 ----
 * 海思 Hi3798/Hi3796 等机顶盒 SoC 规格书写明支持 AVS 基准/AVS+（AVS-P16）硬解，
 * 但 AOSP 没有标准 MIME，各厂 media_codecs.xml 的写法不一（video/avs、video/cavs、
 * video/avs+ 等，均未在真机核实）。所以不写死 MIME：按设备实际声明挑选，并用该 MIME 配置。 */
static const char *const k_avs_mimes[] = {
    "video/avs+", "video/avsplus", "video/avs-plus", "video/x-avs+",
    "video/avs", "video/cavs", "video/x-avs", "video/x-cavs",
};

static int avs_is_next_gen(const char *s)
{
    return ms_hw_ascii_icontains(s, "avs2") || ms_hw_ascii_icontains(s, "avs3");
}

int ms_hw_avs_mime_rank(const char *mime)
{
    size_t i;
    if (mime == NULL || mime[0] == 0 || avs_is_next_gen(mime)) {
        return 0;
    }
    for (i = 0; i < sizeof(k_avs_mimes) / sizeof(k_avs_mimes[0]); i++) {
        if (ms_hw_ascii_ieq(mime, k_avs_mimes[i])) {
            return (int)i + 1;
        }
    }
    return 0;
}

/* 名称兜底：组件名同时含 hisi 与 avs（排除 avs2/avs3），取其声明里第一个含 avs 的类型；
 * 都不含 avs 时取第一个类型（厂商私有 MIME），仍以设备声明为准。 */
static int avs_name_fallback(const ms_hw_codec_desc *c, const char **mime_out)
{
    unsigned j;
    if (c->name == NULL || avs_is_next_gen(c->name) ||
        !ms_hw_ascii_icontains(c->name, "hisi") || !ms_hw_ascii_icontains(c->name, "avs")) {
        return 0;
    }
    for (j = 0; j < c->ntypes && j < MS_HW_AVS_MAX_TYPES; j++) {
        if (c->types[j] != NULL && ms_hw_ascii_icontains(c->types[j], "avs") &&
            !avs_is_next_gen(c->types[j])) {
            *mime_out = c->types[j];
            return 1;
        }
    }
    if (c->ntypes > 0 && c->types[0] != NULL && c->types[0][0] != 0 &&
        !avs_is_next_gen(c->types[0])) {
        *mime_out = c->types[0];
        return 1;
    }
    return 0;
}

static void avs_copy(char *dst, size_t cap, const char *src)
{
    size_t n;
    if (dst == NULL || cap == 0) {
        return;
    }
    n = src ? strlen(src) : 0;
    if (n >= cap) {
        n = cap - 1u;
    }
    if (n) {
        memcpy(dst, src, n);
    }
    dst[n] = 0;
}

ms_status ms_hw_avs_pick_from_list(const ms_hw_codec_desc *list, size_t n, int api_level,
                                   int allow_sw, char *name, size_t name_cap,
                                   char *mime, size_t mime_cap, ms_hw_kind *kind,
                                   char *detail, size_t detail_cap)
{
    size_t i;
    int best_hw = -1, best_sw = -1, best_hw_rank = 1 << 20, best_sw_rank = 1 << 20;
    const char *hw_mime = NULL, *sw_mime = NULL;

    avs_copy(name, name_cap, "");
    avs_copy(mime, mime_cap, "");
    if (kind) {
        *kind = MS_HW_KIND_NONE;
    }
    for (i = 0; list != NULL && i < n; i++) {
        const ms_hw_codec_desc *c = &list[i];
        const char *m = NULL;
        int rank = 1 << 20;
        unsigned j;
        ms_hw_kind k;
        if (c->name == NULL || c->is_encoder || c->is_alias) {
            continue;
        }
        for (j = 0; j < c->ntypes && j < MS_HW_AVS_MAX_TYPES; j++) {
            int r = ms_hw_avs_mime_rank(c->types[j]);
            if (r > 0 && r < rank) {
                rank = r;
                m = c->types[j];
            }
        }
        if (m == NULL) {
            /* 名称兜底排在所有标准写法之后。 */
            if (!avs_name_fallback(c, &m)) {
                continue;
            }
            rank = 100;
        }
        k = ms_hw_classify_codec_name(c->name, api_level, c->hw_flag, c->sw_flag);
        if (k == MS_HW_KIND_HARDWARE) {
            /* 同优先级下海思组件优先（目标机型）。 */
            int r2 = rank * 2 + (ms_hw_ascii_icontains(c->name, "hisi") ? 0 : 1);
            if (r2 < best_hw_rank) {
                best_hw_rank = r2;
                best_hw = (int)i;
                hw_mime = m;
            }
        } else if (rank < best_sw_rank) {
            best_sw_rank = rank;
            best_sw = (int)i;
            sw_mime = m;
        }
    }
    if (best_hw >= 0) {
        avs_copy(name, name_cap, list[best_hw].name);
        avs_copy(mime, mime_cap, hw_mime);
        if (kind) {
            *kind = MS_HW_KIND_HARDWARE;
        }
        avs_copy(detail, detail_cap, "selected hardware AVS/AVS+ decoder by MediaCodecList (declared MIME)");
        return MS_OK;
    }
    if (best_sw >= 0 && allow_sw) {
        avs_copy(name, name_cap, list[best_sw].name);
        avs_copy(mime, mime_cap, sw_mime);
        if (kind) {
            *kind = MS_HW_KIND_SOFTWARE;
        }
        avs_copy(detail, detail_cap, "only software AVS decoder; allowed by option");
        return MS_OK;
    }
    if (best_sw >= 0) {
        avs_copy(name, name_cap, list[best_sw].name);
        if (kind) {
            *kind = MS_HW_KIND_SOFTWARE;
        }
        avs_copy(detail, detail_cap, "refused software AVS decoder; no hardware AVS+ in MediaCodecList");
        return MS_ERR_UNSUPPORTED;
    }
    avs_copy(detail, detail_cap,
             "no AVS/AVS+ decoder in MediaCodecList (checked video/avs+,avs,cavs and hisi*avs names)");
    return MS_ERR_UNSUPPORTED;
}
