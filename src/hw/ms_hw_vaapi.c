#define _DEFAULT_SOURCE
/*
 * Linux VAAPI：码流参数 → libva profile 映射，以及 DRM 设备探测。
 * 本构建不实现 slice/picture 提交或呈现，不得把探测成功写成硬解出画。
 * 无 /dev/dri、打不开设备或尚未实现解码路径时，is_hardware 必须为 0。
 * 禁止仅按 MIME/codec 枚举声称支持。运行时 dlopen libva.so.2 / libva-drm.so.2，
 * 不把 libva 链进 libmshw 的 NEEDED。
 * by: Long <mycjzb@gmail.com>
 */
#include "ms_hw_internal.h"
#include "ms_hw_vaapi_libva.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#if defined(__linux__) && !defined(__ANDROID__)
#include <dlfcn.h>
#include <dirent.h>
#include <errno.h>
#endif

#define RBSP_CAP 512

typedef struct {
    const uint8_t *p;
    size_t n;
    size_t bit;
} br_t;

static int br_u(br_t *b, unsigned nbits, unsigned *out)
{
    unsigned v = 0;
    unsigned i;
    if (out == NULL || b == NULL || nbits > 32) {
        return 0;
    }
    if (nbits == 0) {
        *out = 0;
        return 1;
    }
    for (i = 0; i < nbits; i++) {
        size_t by = b->bit / 8u;
        unsigned sh;
        if (by >= b->n) {
            return 0;
        }
        sh = 7u - (unsigned)(b->bit % 8u);
        v = (v << 1) | (unsigned)((b->p[by] >> sh) & 1u);
        b->bit++;
    }
    *out = v;
    return 1;
}

static int br_ue(br_t *b, unsigned *out)
{
    unsigned z = 0;
    unsigned one = 0;
    unsigned suffix = 0;
    while (z < 31u) {
        if (!br_u(b, 1, &one)) {
            return 0;
        }
        if (one) {
            break;
        }
        z++;
    }
    if (!one || z >= 31u) {
        return 0;
    }
    if (z == 0) {
        *out = 0;
        return 1;
    }
    if (!br_u(b, z, &suffix)) {
        return 0;
    }
    *out = ((1u << z) - 1u) + suffix;
    return 1;
}

static size_t rbsp_unescape(const uint8_t *src, size_t n, uint8_t *dst, size_t cap)
{
    size_t i = 0;
    size_t o = 0;
    if (src == NULL || dst == NULL) {
        return 0;
    }
    while (i < n && o < cap) {
        if (i + 2 < n && src[i] == 0 && src[i + 1] == 0 && src[i + 2] == 3) {
            dst[o++] = 0;
            if (o >= cap) {
                break;
            }
            dst[o++] = 0;
            i += 3;
        } else {
            dst[o++] = src[i++];
        }
    }
    return o;
}

static void copy_txt(char *buf, size_t cap, const char *s)
{
    size_t n = 0;
    if (buf == NULL || cap == 0) {
        return;
    }
    if (s != NULL) {
        while (n + 1u < cap && s[n] != 0) {
            buf[n] = s[n];
            n++;
        }
    }
    buf[n] = 0;
}

static void stream_clear(ms_hw_vaapi_stream *s, ms_codec codec)
{
    size_t sz;
    if (s == NULL) {
        return;
    }
    sz = s->struct_size;
    if (sz == 0 || sz > sizeof(*s)) {
        sz = sizeof(*s);
    }
    memset(s, 0, sz);
    s->struct_size = sz;
    s->codec = codec;
    s->va_profile = MSVAProfileNone;
    s->profile_name = "none";
}

void ms_hw_vaapi_stream_default(ms_hw_vaapi_stream *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->struct_size = sizeof(*out);
    out->va_profile = MSVAProfileNone;
    out->profile_name = "none";
}

const char *ms_hw_vaapi_profile_name(int va_profile)
{
    switch (va_profile) {
    case MSVAProfileMPEG2Simple:
        return "MPEG2Simple";
    case MSVAProfileMPEG2Main:
        return "MPEG2Main";
    case MSVAProfileMPEG4Simple:
        return "MPEG4Simple";
    case MSVAProfileMPEG4AdvancedSimple:
        return "MPEG4AdvancedSimple";
    case MSVAProfileMPEG4Main:
        return "MPEG4Main";
    case MSVAProfileH264Baseline:
        return "H264Baseline";
    case MSVAProfileH264Main:
        return "H264Main";
    case MSVAProfileH264High:
        return "H264High";
    case MSVAProfileH263Baseline:
        return "H263Baseline";
    case MSVAProfileH264ConstrainedBaseline:
        return "H264ConstrainedBaseline";
    case MSVAProfileHEVCMain:
        return "HEVCMain";
    case MSVAProfileHEVCMain10:
        return "HEVCMain10";
    case MSVAProfileHEVCMain12:
        return "HEVCMain12";
    case MSVAProfileHEVCMain422_10:
        return "HEVCMain422_10";
    case MSVAProfileHEVCMain444:
        return "HEVCMain444";
    case MSVAProfileAV1Profile0:
        return "AV1Profile0";
    case MSVAProfileAV1Profile1:
        return "AV1Profile1";
    case MSVAProfileH264High10:
        return "H264High10";
    case MSVAProfileNone:
        return "none";
    default:
        return "unknown";
    }
}

static void finish_mapped(ms_hw_vaapi_stream *s, int profile)
{
    s->va_profile = profile;
    s->profile_name = ms_hw_vaapi_profile_name(profile);
    s->mapped = (profile != MSVAProfileNone) ? 1 : 0;
}

static int parse_h264_sps_rbsp(const uint8_t *nal, size_t n, ms_hw_vaapi_stream *s)
{
    uint8_t rbsp[RBSP_CAP];
    size_t rn;
    br_t b;
    unsigned profile_idc = 0;
    unsigned cons = 0;
    unsigned level = 0;
    unsigned sps_id = 0;
    unsigned chroma = 1;
    unsigned bit_luma = 8;
    unsigned bit_chroma = 8;
    unsigned tmp = 0;
    const uint8_t *body;
    size_t body_n;

    if (nal == NULL || n < 4) {
        return 0;
    }
    if ((nal[0] & 0x1fu) != 7u) {
        return 0;
    }
    body = nal + 1;
    body_n = n - 1;
    rn = rbsp_unescape(body, body_n, rbsp, sizeof(rbsp));
    if (rn < 3) {
        return 0;
    }
    b.p = rbsp;
    b.n = rn;
    b.bit = 0;
    if (!br_u(&b, 8, &profile_idc) || !br_u(&b, 8, &cons) || !br_u(&b, 8, &level)) {
        return 0;
    }
    if (!br_ue(&b, &sps_id)) {
        return 0;
    }
    if (profile_idc == 100 || profile_idc == 110 || profile_idc == 122 ||
        profile_idc == 244 || profile_idc == 44 || profile_idc == 83 ||
        profile_idc == 86 || profile_idc == 118 || profile_idc == 128 ||
        profile_idc == 138 || profile_idc == 139 || profile_idc == 134 ||
        profile_idc == 135) {
        unsigned sep = 0;
        unsigned qp = 0;
        unsigned scaling = 0;
        if (!br_ue(&b, &chroma)) {
            return 0;
        }
        if (chroma == 3 && !br_u(&b, 1, &sep)) {
            return 0;
        }
        if (!br_ue(&b, &bit_luma) || !br_ue(&b, &bit_chroma)) {
            return 0;
        }
        bit_luma += 8;
        bit_chroma += 8;
        if (!br_u(&b, 1, &qp) || !br_u(&b, 1, &scaling)) {
            return 0;
        }
        (void)tmp;
    }
    s->bitstream_profile = profile_idc;
    s->constraint_flags = cons;
    s->level_idc = level;
    s->chroma_format_idc = chroma;
    s->bit_depth = bit_luma > bit_chroma ? bit_luma : bit_chroma;
    s->complete = 1;

    if (profile_idc == 66) {
        /* CS1 表示 Constrained Baseline；不得把未约束 Baseline 静默改写成 Constrained。 */
        if (cons & 0x40u) {
            finish_mapped(s, MSVAProfileH264ConstrainedBaseline);
        } else {
            finish_mapped(s, MSVAProfileH264Baseline);
        }
        return 1;
    }
    if (profile_idc == 77) {
        finish_mapped(s, MSVAProfileH264Main);
        return 1;
    }
    if (profile_idc == 100) {
        if (s->bit_depth > 8) {
            finish_mapped(s, MSVAProfileH264High10);
            return 1;
        }
        if (chroma != 1) {
            /* High 4:2:2 / 4:4:4 无对应常用 VA profile，不得映射成 High。 */
            s->mapped = 0;
            s->va_profile = MSVAProfileNone;
            s->profile_name = "unmapped-h264-high-chroma";
            return 1;
        }
        finish_mapped(s, MSVAProfileH264High);
        return 1;
    }
    if (profile_idc == 110) {
        finish_mapped(s, MSVAProfileH264High10);
        return 1;
    }
    s->mapped = 0;
    s->va_profile = MSVAProfileNone;
    s->profile_name = "unmapped-h264-profile";
    return 1;
}

static const uint8_t *find_start(const uint8_t *p, const uint8_t *end, unsigned *sc);

static int take_h264_sps(const uint8_t *nal, size_t n, ms_hw_vaapi_stream *s)
{
    if (nal == NULL || n == 0) {
        return 0;
    }
    if ((nal[0] & 0x1fu) != 7u) {
        return 0;
    }
    return parse_h264_sps_rbsp(nal, n, s);
}

/* 从 avcC / Annex B / 裸 SPS 取出 type-7 NAL。只解析 profile 必要字段，
 * 不要求完整 pic_width（宿主夹具 SPS 可能短于可配置硬解所需）。 */
static int parse_h264(ms_codec codec, ms_sample_format fmt, const uint8_t *data,
                      size_t size, ms_hw_vaapi_stream *s)
{
    ms_h264_ps_set ps;
    const uint8_t *p;
    const uint8_t *end;
    (void)codec;
    if (data == NULL || size == 0) {
        return 0;
    }
    memset(&ps, 0, sizeof(ps));
    ps.struct_size = sizeof(ps);
    if (ms_h264_ps_parse(fmt, 1, data, size, 0, &ps) == MS_OK && ps.have_sps) {
        if (take_h264_sps(ps.sps, ps.sps_size, s)) {
            return 1;
        }
    }
    if (fmt == MS_SAMPLE_FMT_AVCC ||
        (fmt != MS_SAMPLE_FMT_ANNEXB && size >= 7 && data[0] == 1)) {
        size_t off;
        unsigned i, nsps, ln;
        if (size < 7) {
            return 0;
        }
        nsps = (unsigned)(data[5] & 0x1fu);
        off = 6;
        for (i = 0; i < nsps; i++) {
            if (off + 2 > size) {
                return 0;
            }
            ln = ((unsigned)data[off] << 8) | (unsigned)data[off + 1];
            off += 2;
            if (ln == 0 || off + ln > size) {
                return 0;
            }
            if (take_h264_sps(data + off, ln, s)) {
                return 1;
            }
            off += ln;
        }
    }
    p = data;
    end = data + size;
    while (p < end) {
        unsigned sc = 0;
        const uint8_t *nal = find_start(p, end, &sc);
        const uint8_t *next;
        size_t nn;
        if (nal == NULL) {
            break;
        }
        nal += sc;
        next = find_start(nal, end, &sc);
        nn = (size_t)((next ? next : end) - nal);
        if (take_h264_sps(nal, nn, s)) {
            return 1;
        }
        p = next ? next : end;
    }
    return take_h264_sps(data, size, s);
}

static int ptl_need_ext(unsigned idc, unsigned compat)
{
    unsigned ids[8] = { 4, 5, 6, 7, 8, 9, 10, 11 };
    unsigned i;
    for (i = 0; i < 8; i++) {
        if (idc == ids[i] || (compat & (1u << ids[i]))) {
            return 1;
        }
    }
    return 0;
}

static int parse_hevc_sps_rbsp(const uint8_t *nal, size_t n, ms_hw_vaapi_stream *s)
{
    uint8_t rbsp[RBSP_CAP];
    size_t rn;
    br_t b;
    unsigned nal_type;
    unsigned vps_id = 0, max_sub = 0, nest = 0;
    unsigned space = 0, tier = 0, idc = 0, bit = 0, i;
    unsigned compat = 0;
    unsigned prog = 0, interl = 0, packed = 0, frame_only = 0;
    unsigned level = 0, dummy = 0;
    unsigned sps_id = 0, chroma = 1, sep = 0, w = 0, h = 0;
    unsigned bit_luma = 8, bit_chroma = 8;
    const uint8_t *body;
    size_t body_n;
    int ext;

    if (nal == NULL || n < 5) {
        return 0;
    }
    nal_type = (nal[0] >> 1) & 0x3fu;
    if (nal_type != 33u) {
        return 0;
    }
    body = nal + 2;
    body_n = n - 2;
    rn = rbsp_unescape(body, body_n, rbsp, sizeof(rbsp));
    if (rn < 6) {
        return 0;
    }
    b.p = rbsp;
    b.n = rn;
    b.bit = 0;
    if (!br_u(&b, 4, &vps_id) || !br_u(&b, 3, &max_sub) || !br_u(&b, 1, &nest)) {
        return 0;
    }
    if (!br_u(&b, 2, &space) || !br_u(&b, 1, &tier) || !br_u(&b, 5, &idc)) {
        return 0;
    }
    for (i = 0; i < 32; i++) {
        if (!br_u(&b, 1, &bit)) {
            return 0;
        }
        if (bit) {
            compat |= (1u << i);
        }
    }
    if (!br_u(&b, 1, &prog) || !br_u(&b, 1, &interl) || !br_u(&b, 1, &packed) ||
        !br_u(&b, 1, &frame_only)) {
        return 0;
    }
    ext = ptl_need_ext(idc, compat);
    if (ext) {
        /* 12/10/8bit + chroma + mono + intra + one_pic + lower_br = 9 flags */
        for (i = 0; i < 9; i++) {
            if (!br_u(&b, 1, &dummy)) {
                return 0;
            }
        }
        if (idc == 5 || idc == 9 || idc == 10 || idc == 11 ||
            (compat & ((1u << 5) | (1u << 9) | (1u << 10) | (1u << 11)))) {
            if (!br_u(&b, 1, &dummy)) { /* max_14bit */
                return 0;
            }
            if (!br_u(&b, 32, &dummy) || !br_u(&b, 1, &dummy)) {
                return 0;
            }
        } else {
            /* reserved 34 */
            if (!br_u(&b, 32, &dummy) || !br_u(&b, 2, &dummy)) {
                return 0;
            }
        }
    } else if (idc == 2 || (compat & (1u << 2))) {
        if (!br_u(&b, 7, &dummy) || !br_u(&b, 1, &dummy) || !br_u(&b, 32, &dummy) ||
            !br_u(&b, 3, &dummy)) {
            return 0;
        }
    } else {
        /* reserved zero 43 bits */
        if (!br_u(&b, 32, &dummy) || !br_u(&b, 11, &dummy)) {
            return 0;
        }
    }
    /* general_inbld_flag or reserved */
    if (!br_u(&b, 1, &dummy)) {
        return 0;
    }
    if (!br_u(&b, 8, &level)) {
        return 0;
    }
    if (max_sub > 0) {
        unsigned flags = 0, present = 0;
        for (i = 0; i < max_sub; i++) {
            if (!br_u(&b, 1, &bit)) {
                return 0;
            }
            flags |= bit << i;
            if (!br_u(&b, 1, &bit)) { /* level present */
                return 0;
            }
            present |= bit << i;
        }
        if (max_sub < 8u) {
            if (!br_u(&b, (unsigned)(8 - max_sub) * 2u, &dummy)) {
                return 0;
            }
        }
        (void)flags;
        (void)present;
    }
    if (!br_ue(&b, &sps_id)) {
        return 0;
    }
    if (!br_ue(&b, &chroma)) {
        return 0;
    }
    if (chroma == 3 && !br_u(&b, 1, &sep)) {
        return 0;
    }
    if (!br_ue(&b, &w) || !br_ue(&b, &h)) {
        return 0;
    }
    if (!br_u(&b, 1, &bit)) { /* conformance window */
        return 0;
    }
    if (bit) {
        if (!br_ue(&b, &dummy) || !br_ue(&b, &dummy) || !br_ue(&b, &dummy) ||
            !br_ue(&b, &dummy)) {
            return 0;
        }
    }
    if (!br_ue(&b, &bit_luma) || !br_ue(&b, &bit_chroma)) {
        return 0;
    }
    bit_luma += 8;
    bit_chroma += 8;

    s->bitstream_profile = idc;
    s->constraint_flags = compat;
    s->level_idc = level;
    s->chroma_format_idc = chroma;
    s->bit_depth = bit_luma > bit_chroma ? bit_luma : bit_chroma;
    s->complete = 1;
    (void)vps_id;
    (void)nest;
    (void)space;
    (void)tier;
    (void)prog;
    (void)interl;
    (void)packed;
    (void)frame_only;
    (void)sps_id;
    (void)w;
    (void)h;

    /* 以主 profile_idc 与实际 bit depth / chroma 为准，不得仅因 compatibility
     * flag[2] 就把 8bit Main 映射成 Main10。 */
    if (chroma == 1 && s->bit_depth <= 8 && (idc == 1 || (compat & (1u << 1)))) {
        finish_mapped(s, MSVAProfileHEVCMain);
        return 1;
    }
    if (chroma == 1 && s->bit_depth <= 10 &&
        (idc == 2 || s->bit_depth == 10 || (compat & (1u << 2) && s->bit_depth == 10))) {
        finish_mapped(s, MSVAProfileHEVCMain10);
        return 1;
    }
    if (chroma == 1 && s->bit_depth >= 12) {
        finish_mapped(s, MSVAProfileHEVCMain12);
        return 1;
    }
    s->mapped = 0;
    s->va_profile = MSVAProfileNone;
    s->profile_name = "unmapped-hevc-profile";
    return 1;
}

static int parse_hevc(ms_sample_format fmt, const uint8_t *data, size_t size,
                      ms_hw_vaapi_stream *s)
{
    ms_hevc_ps_set ps;
    memset(&ps, 0, sizeof(ps));
    ps.struct_size = sizeof(ps);
    if (data == NULL || size == 0) {
        return 0;
    }
    if (ms_hevc_ps_parse(fmt, 1, data, size, 0, &ps) == MS_OK && ps.have_sps) {
        return parse_hevc_sps_rbsp(ps.sps, ps.sps_size, s);
    }
    if (ms_hevc_ps_parse(MS_SAMPLE_FMT_ANNEXB, 1, data, size, 0, &ps) == MS_OK &&
        ps.have_sps) {
        return parse_hevc_sps_rbsp(ps.sps, ps.sps_size, s);
    }
    return 0;
}

static const uint8_t *find_start(const uint8_t *p, const uint8_t *end, unsigned *sc)
{
    const uint8_t *q;
    for (q = p; q + 3 <= end; q++) {
        if (q[0] == 0 && q[1] == 0 && q[2] == 1) {
            *sc = 3;
            return q;
        }
        if (q + 4 <= end && q[0] == 0 && q[1] == 0 && q[2] == 0 && q[3] == 1) {
            *sc = 4;
            return q;
        }
    }
    return NULL;
}

static int parse_mpeg2(const uint8_t *data, size_t size, ms_hw_vaapi_stream *s)
{
    const uint8_t *end = data + size;
    const uint8_t *p = data;
    int have_ext = 0;
    if (data == NULL || size < 8) {
        return 0;
    }
    while (p < end) {
        unsigned sc = 0;
        const uint8_t *nal = find_start(p, end, &sc);
        if (nal == NULL || nal + sc + 2 > end) {
            break;
        }
        if (nal[sc] == 0xB5 && (nal[sc + 1] >> 4) == 1) {
            unsigned pal = (unsigned)((nal[sc + 1] & 0x0fu) << 4) |
                           (unsigned)(nal[sc + 2] >> 4);
            unsigned profile = pal >> 4;
            unsigned level = pal & 0x0fu;
            unsigned chroma = (nal[sc + 2] >> 1) & 0x3u;
            s->bitstream_profile = profile;
            s->level_idc = level;
            s->chroma_format_idc = chroma;
            s->bit_depth = 8;
            s->complete = 1;
            have_ext = 1;
            if (profile == 5) {
                finish_mapped(s, MSVAProfileMPEG2Simple);
            } else if (profile == 4) {
                finish_mapped(s, MSVAProfileMPEG2Main);
            } else {
                s->mapped = 0;
                s->va_profile = MSVAProfileNone;
                s->profile_name = "unmapped-mpeg2-profile";
            }
            return 1;
        }
        p = nal + sc + 1;
    }
    (void)have_ext;
    return 0;
}

static int parse_mpeg4(const uint8_t *data, size_t size, ms_hw_vaapi_stream *s)
{
    const uint8_t *end = data + size;
    const uint8_t *p = data;
    if (data == NULL || size < 5) {
        return 0;
    }
    while (p < end) {
        unsigned sc = 0;
        const uint8_t *nal = find_start(p, end, &sc);
        unsigned pal;
        if (nal == NULL || nal + sc + 1 >= end) {
            break;
        }
        if (nal[sc] == 0xB0) {
            pal = nal[sc + 1];
            s->bitstream_profile = pal;
            s->level_idc = pal & 0x0fu;
            s->bit_depth = 8;
            s->chroma_format_idc = 1;
            s->complete = 1;
            /* 14496-2：0x01–0x03 Simple；0xF0–0xF5 Advanced Simple。不得把其它指示映射成 Simple。 */
            if (pal <= 0x03u) {
                finish_mapped(s, MSVAProfileMPEG4Simple);
            } else if (pal >= 0xF0u && pal <= 0xF5u) {
                finish_mapped(s, MSVAProfileMPEG4AdvancedSimple);
            } else {
                s->mapped = 0;
                s->va_profile = MSVAProfileNone;
                s->profile_name = "unmapped-mpeg4-profile";
            }
            return 1;
        }
        p = nal + sc + 1;
    }
    return 0;
}

static int parse_h263(const uint8_t *data, size_t size, ms_hw_vaapi_stream *s)
{
    unsigned ptype;
    unsigned src;
    if (data == NULL || size < 5) {
        return 0;
    }
    /* PSC 22 bit = 0x000080 >> 对齐后的短视频头。夹具为 00 00 80 02 .. */
    if (data[0] != 0 || data[1] != 0 || (data[2] & 0xFCu) != 0x80u) {
        return 0;
    }
    /* TR 8 bits 后接 PTYPE。字节 2 低 2 位起。 */
    ptype = ((unsigned)(data[2] & 0x03u) << 8) | (unsigned)data[3];
    if (size >= 5) {
        ptype = (ptype << 8) | (unsigned)data[4];
    }
    /* 更直接：标准封装 00 00 80 02 xx，PTYPE 从 bit 22 之后。
     * 字节：0000 0000 | 0000 0000 | 1000 00 TR[7:6] | TR[5:0] PTYPE[1:0] | PTYPE[rest]
     * 夹具 00 00 80 02 04：TR=0，PTYPE 起始于 0x02 的低 2 位与后续。
     * 常用实现：byte2=0x80, byte3 低位含源格式。夹具 SQCIF I = 00 00 80 02 04 00
     * PTYPE bits (from T-REC): bit1-2 always 10, bit3 split, bit4-6 source format.
     * 在 00 00 80 02 04 中，从 PSC 后 8 bit TR=0（在 0x02 的高 6 位?）需按 bit 对齐。
     */
    src = (data[4] >> 2) & 0x07u;
    /* 核对夹具：SQCIF I 0x04 → 0000 0100，若源格式在 bit 4-6 of PTYPE.
     * 已验证测试夹具 source：SQCIF=001, QCIF=010, CIF=011, PLUS=111。
     * 0x04>>2 = 1 SQCIF；0x08>>2=2 QCIF；0x0c>>2=3 CIF；0x1c>>2=7 PLUS。吻合。 */
    (void)ptype;
    s->bitstream_profile = src;
    s->bit_depth = 8;
    s->chroma_format_idc = 1;
    s->complete = 1;
    if (src == 7u) {
        s->mapped = 0;
        s->va_profile = MSVAProfileNone;
        s->profile_name = "unmapped-h263-plusptype";
        return 1;
    }
    if (src >= 1u && src <= 6u) {
        finish_mapped(s, MSVAProfileH263Baseline);
        return 1;
    }
    s->mapped = 0;
    s->va_profile = MSVAProfileNone;
    s->profile_name = "unmapped-h263";
    return 1;
}

static int parse_av1(const uint8_t *data, size_t size, ms_hw_vaapi_stream *s)
{
    unsigned profile = 0;
    unsigned level = 0;
    if (data == NULL || size < 2) {
        return 0;
    }
    /* av1C：marker=1, version>=1 的第一字节 0x8x。 */
    if ((data[0] & 0x80u) != 0 && (data[0] & 0x7fu) >= 1u) {
        profile = (data[1] >> 5) & 0x07u;
        level = data[1] & 0x1fu;
    } else if (data[0] == 0x0a) {
        /* 裸 sequence header OBU：obu_type=1, 其后 length，再 seq_profile f(3)。
         * 夹具 0x0a 0x0a 0x00 0x00 0x00 0x02 ... 需要按 OBU 头解析。
         * 简化：若首字节是 SH OBU 且含 size 字段，跳到 payload 读 3 bit。
         * 0x0a = 0000 1010：forbidden=0, type=1 (SH), ext=0, has_size=1。
         * leb128 size 后为 payload。夹具第二字节 0x0a 作为 size=10。
         * payload 起始 data[2]，seq_profile 在 payload 前 3 bit。
         * 但夹具 0x0a 0x0a 0x00 ... payload 0x00 → profile 0。
         */
        unsigned i = 1;
        unsigned size_v = 0, shift = 0;
        while (i < size && shift < 32) {
            unsigned b = data[i++];
            size_v |= (b & 0x7fu) << shift;
            if ((b & 0x80u) == 0) {
                break;
            }
            shift += 7;
        }
        (void)size_v;
        if (i >= size) {
            return 0;
        }
        profile = (data[i] >> 5) & 0x07u;
        level = 0;
    } else {
        return 0;
    }
    s->bitstream_profile = profile;
    s->level_idc = level;
    s->bit_depth = 8;
    s->chroma_format_idc = 1;
    s->complete = 1;
    if (profile == 0) {
        finish_mapped(s, MSVAProfileAV1Profile0);
        return 1;
    }
    if (profile == 1) {
        finish_mapped(s, MSVAProfileAV1Profile1);
        return 1;
    }
    s->mapped = 0;
    s->va_profile = MSVAProfileNone;
    s->profile_name = "unmapped-av1-profile";
    return 1;
}

static int parse_avsplus(const uint8_t *data, size_t size, ms_hw_vaapi_stream *s)
{
    const uint8_t *end = data + size;
    const uint8_t *p = data;
    if (data == NULL || size < 8) {
        return 0;
    }
    while (p < end) {
        unsigned sc = 0;
        const uint8_t *nal = find_start(p, end, &sc);
        unsigned profile;
        if (nal == NULL || nal + sc + 1 >= end) {
            break;
        }
        if (nal[sc] == 0xB0) {
            profile = nal[sc + 1];
            s->bitstream_profile = profile;
            s->bit_depth = 8;
            s->chroma_format_idc = 1;
            if (profile != 0x48u) {
                /* 不是广播类 AVS+，不得映射。 */
                s->complete = 0;
                s->mapped = 0;
                s->va_profile = MSVAProfileNone;
                s->profile_name = "not-avs-plus";
                return 0;
            }
            s->complete = 1;
            /* libva 2.22 无 AVS+ profile。解析完整但不声称有硬解映射。 */
            s->mapped = 0;
            s->va_profile = MSVAProfileNone;
            s->profile_name = "unmapped-avs-plus-no-va-profile";
            return 1;
        }
        p = nal + sc + 1;
    }
    return 0;
}

ms_status ms_hw_vaapi_parse_stream(ms_codec codec, ms_sample_format fmt,
                                   const uint8_t *data, size_t size,
                                   ms_hw_vaapi_stream *out)
{
    int ok = 0;
    if (out == NULL || out->struct_size < sizeof(size_t)) {
        return MS_ERR_INVALID_ARG;
    }
    if (out->struct_size > sizeof(*out)) {
        out->struct_size = sizeof(*out);
    }
    stream_clear(out, codec);
    if (data == NULL && size != 0) {
        return MS_ERR_INVALID_ARG;
    }
    switch (codec) {
    case MS_CODEC_H264:
        ok = parse_h264(codec, fmt, data, size, out);
        break;
    case MS_CODEC_HEVC:
        ok = parse_hevc(fmt, data, size, out);
        break;
    case MS_CODEC_MPEG2_VIDEO:
        ok = parse_mpeg2(data, size, out);
        break;
    case MS_CODEC_MPEG4_PART2:
        ok = parse_mpeg4(data, size, out);
        break;
    case MS_CODEC_H263:
        ok = parse_h263(data, size, out);
        break;
    case MS_CODEC_AV1:
        ok = parse_av1(data, size, out);
        break;
    case MS_CODEC_AVS_PLUS:
        ok = parse_avsplus(data, size, out);
        break;
    default:
        out->profile_name = "unsupported-codec";
        return MS_ERR_UNSUPPORTED;
    }
    if (!ok || !out->complete) {
        return MS_ERR_MALFORMED;
    }
    return MS_OK;
}

void ms_hw_vaapi_probe_default(ms_hw_vaapi_probe *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->struct_size = sizeof(*out);
    out->decode_implemented = 0;
    out->is_hardware = 0;
    out->verified = 0;
    out->backend = "vaapi";
    out->vendor = NULL;
    out->detail = "";
}

#if defined(__linux__) && !defined(__ANDROID__)

static int name_is_dri_node(const char *name, const char *pfx)
{
    size_t n, p;
    if (name == NULL || pfx == NULL) {
        return 0;
    }
    p = strlen(pfx);
    n = strlen(name);
    if (n <= p || n > 32) {
        return 0;
    }
    return strncmp(name, pfx, p) == 0;
}

typedef struct {
    int ready;
    int loaded;
    void *h_va;
    void *h_drm;
    ms_va_get_display_drm_fn get_display_drm;
    ms_va_initialize_fn initialize;
    ms_va_terminate_fn terminate;
    ms_va_query_vendor_string_fn vendor;
    ms_va_error_str_fn error_str;
    ms_va_max_num_profiles_fn max_profiles;
    ms_va_max_num_entrypoints_fn max_entrypoints;
    ms_va_query_config_profiles_fn query_profiles;
    ms_va_query_config_entrypoints_fn query_entrypoints;
} va_api;

static va_api g_va;

static int va_load(char *err, size_t cap)
{
    if (g_va.ready) {
        return g_va.loaded;
    }
    g_va.h_va = dlopen("libva.so.2", RTLD_NOW | RTLD_LOCAL);
    g_va.h_drm = dlopen("libva-drm.so.2", RTLD_NOW | RTLD_LOCAL);
    if (g_va.h_va == NULL || g_va.h_drm == NULL) {
        snprintf(err, cap, "libva dlopen failed: va=%s drm=%s",
                 g_va.h_va ? "ok" : dlerror(),
                 g_va.h_drm ? "ok" : dlerror());
        g_va.ready = 1;
        g_va.loaded = 0;
        return 0;
    }
    g_va.get_display_drm = (ms_va_get_display_drm_fn)dlsym(g_va.h_drm, "vaGetDisplayDRM");
    g_va.initialize = (ms_va_initialize_fn)dlsym(g_va.h_va, "vaInitialize");
    g_va.terminate = (ms_va_terminate_fn)dlsym(g_va.h_va, "vaTerminate");
    g_va.vendor = (ms_va_query_vendor_string_fn)dlsym(g_va.h_va, "vaQueryVendorString");
    g_va.error_str = (ms_va_error_str_fn)dlsym(g_va.h_va, "vaErrorStr");
    g_va.max_profiles = (ms_va_max_num_profiles_fn)dlsym(g_va.h_va, "vaMaxNumProfiles");
    g_va.max_entrypoints = (ms_va_max_num_entrypoints_fn)dlsym(g_va.h_va, "vaMaxNumEntrypoints");
    g_va.query_profiles = (ms_va_query_config_profiles_fn)dlsym(g_va.h_va, "vaQueryConfigProfiles");
    g_va.query_entrypoints =
        (ms_va_query_config_entrypoints_fn)dlsym(g_va.h_va, "vaQueryConfigEntrypoints");
    if (g_va.get_display_drm == NULL || g_va.initialize == NULL || g_va.terminate == NULL ||
        g_va.max_profiles == NULL || g_va.query_profiles == NULL ||
        g_va.query_entrypoints == NULL) {
        snprintf(err, cap, "libva symbol missing");
        g_va.ready = 1;
        g_va.loaded = 0;
        return 0;
    }
    g_va.ready = 1;
    g_va.loaded = 1;
    if (err && cap) {
        err[0] = 0;
    }
    return 1;
}

static int try_open_node(const char *path)
{
    int fd;
    if (path == NULL) {
        return -1;
    }
    fd = open(path, O_RDWR | O_CLOEXEC);
    return fd;
}

static int open_first_drm(char *path_out, size_t cap)
{
    DIR *dir;
    struct dirent *ent;
    int fd = -1;
    char path[64];
    int i;

    dir = opendir("/dev/dri");
    if (dir == NULL) {
        if (path_out && cap) {
            path_out[0] = 0;
        }
        return -1;
    }
    /* 优先 render 节点 */
    while ((ent = readdir(dir)) != NULL) {
        if (!name_is_dri_node(ent->d_name, "renderD")) {
            continue;
        }
        copy_txt(path, sizeof(path), "/dev/dri/");
        /* name_is_dri_node 已限制长度，拼接不会截断。 */
        memcpy(path + 9, ent->d_name, strlen(ent->d_name) + 1);
        fd = try_open_node(path);
        if (fd >= 0) {
            if (path_out && cap) {
                snprintf(path_out, cap, "%s", path);
            }
            closedir(dir);
            return fd;
        }
    }
    rewinddir(dir);
    while ((ent = readdir(dir)) != NULL) {
        if (!name_is_dri_node(ent->d_name, "card")) {
            continue;
        }
        copy_txt(path, sizeof(path), "/dev/dri/");
        memcpy(path + 9, ent->d_name, strlen(ent->d_name) + 1);
        fd = try_open_node(path);
        if (fd >= 0) {
            if (path_out && cap) {
                snprintf(path_out, cap, "%s", path);
            }
            closedir(dir);
            return fd;
        }
    }
    closedir(dir);
    /* 目录存在但打不开时，仍尝试常见固定路径以便把 errno 记入说明 */
    for (i = 128; i < 144 && fd < 0; i++) {
        snprintf(path, sizeof(path), "/dev/dri/renderD%d", i);
        fd = try_open_node(path);
        if (fd >= 0 && path_out && cap) {
            snprintf(path_out, cap, "%s", path);
        }
    }
    if (path_out && cap && fd < 0) {
        path_out[0] = 0;
    }
    return fd;
}

static int query_on_fd(int fd, ms_hw_vaapi_probe *out, char *err, size_t err_cap)
{
    MSVADisplay dpy;
    int major = 0, minor = 0;
    MSVAStatus st;
    int max_p, n = 0, i;
    MSVAProfile *list;
    const char *vendor = NULL;

    dpy = g_va.get_display_drm(fd);
    if (dpy == NULL) {
        snprintf(err, err_cap, "vaGetDisplayDRM returned null");
        return 0;
    }
    st = g_va.initialize(dpy, &major, &minor);
    if (st != MSVA_STATUS_SUCCESS) {
        snprintf(err, err_cap, "vaInitialize failed (%d)", (int)st);
        if (g_va.terminate) {
            (void)g_va.terminate(dpy);
        }
        return 0;
    }
    out->libva_major = major;
    out->libva_minor = minor;
    if (g_va.vendor != NULL) {
        vendor = g_va.vendor(dpy);
        if (vendor != NULL) {
            snprintf(out->vendor_buf, sizeof(out->vendor_buf), "%s", vendor);
            out->vendor = out->vendor_buf;
        }
    }
    max_p = g_va.max_profiles(dpy);
    if (max_p <= 0 || max_p > 128) {
        max_p = 32;
    }
    list = (MSVAProfile *)calloc((size_t)max_p, sizeof(*list));
    if (list == NULL) {
        (void)g_va.terminate(dpy);
        snprintf(err, err_cap, "profile list alloc failed");
        return 0;
    }
    st = g_va.query_profiles(dpy, list, &n);
    if (st != MSVA_STATUS_SUCCESS) {
        free(list);
        (void)g_va.terminate(dpy);
        snprintf(err, err_cap, "vaQueryConfigProfiles failed (%d)", (int)st);
        return 0;
    }
    if (n > MS_HW_VAAPI_PROFILE_MAX) {
        n = MS_HW_VAAPI_PROFILE_MAX;
    }
    out->profile_count = n;
    for (i = 0; i < n; i++) {
        MSVAEntrypoint ep[16];
        int ne = 0, j, has_vld = 0;
        int max_e = 16;
        out->profiles[i] = (int)list[i];
        if (g_va.max_entrypoints != NULL) {
            max_e = g_va.max_entrypoints(dpy);
            if (max_e <= 0 || max_e > 16) {
                max_e = 16;
            }
        }
        st = g_va.query_entrypoints(dpy, list[i], ep, &ne);
        if (st == MSVA_STATUS_SUCCESS) {
            if (ne > max_e) {
                ne = max_e;
            }
            for (j = 0; j < ne; j++) {
                if (ep[j] == MSVAEntrypointVLD) {
                    has_vld = 1;
                    break;
                }
            }
        }
        out->profile_has_vld[i] = has_vld;
    }
    free(list);
    (void)g_va.terminate(dpy);
    out->have_device = 1;
    return 1;
}

ms_status ms_hw_vaapi_probe_device(ms_hw_vaapi_probe *out)
{
    char load_err[160];
    char open_note[160];
    int fd;
    size_t sz;

    if (out == NULL || out->struct_size < sizeof(size_t)) {
        return MS_ERR_INVALID_ARG;
    }
    sz = out->struct_size;
    if (sz > sizeof(*out)) {
        sz = sizeof(*out);
    }
    memset(out, 0, sz);
    out->struct_size = sz;
    out->decode_implemented = 0;
    out->is_hardware = 0;
    out->verified = 1;
    out->backend = "vaapi";
    out->vendor = NULL;
    out->detail = out->detail_buf;
    load_err[0] = 0;
    open_note[0] = 0;

    if (!va_load(load_err, sizeof(load_err))) {
        out->libva_loaded = 0;
        {
            char tmp[320];
            snprintf(tmp, sizeof(tmp), "%s; VAAPI decode/submit not implemented",
                     load_err[0] ? load_err : "libva not loaded");
            copy_txt(out->detail_buf, sizeof(out->detail_buf), tmp);
        }
        return MS_OK;
    }
    out->libva_loaded = 1;
    fd = open_first_drm(out->device_path, sizeof(out->device_path));
    if (fd < 0) {
        if (access("/dev/dri", F_OK) != 0) {
            snprintf(out->detail_buf, sizeof(out->detail_buf),
                     "no /dev/dri DRM node; VAAPI decode/submit not implemented");
        } else {
            snprintf(out->detail_buf, sizeof(out->detail_buf),
                     "DRM node present but not openable (errno %d); "
                     "VAAPI decode/submit not implemented",
                     errno);
        }
        return MS_OK;
    }
    if (!query_on_fd(fd, out, open_note, sizeof(open_note))) {
        close(fd);
        {
            char tmp[320];
            snprintf(tmp, sizeof(tmp),
                     "opened %s but VA init/query failed: %s; decode/submit not implemented",
                     out->device_path[0] ? out->device_path : "drm",
                     open_note[0] ? open_note : "unknown");
            copy_txt(out->detail_buf, sizeof(out->detail_buf), tmp);
        }
        out->have_device = 0;
        return MS_OK;
    }
    close(fd);
    /* 即使设备与 profile 都查到，本构建仍未实现提交/出画，不得置 is_hardware。 */
    {
        char tmp[320];
        snprintf(tmp, sizeof(tmp),
                 "VAAPI device %s vendor=%s profiles=%d; decode/submit not implemented",
                 out->device_path[0] ? out->device_path : "?",
                 out->vendor ? out->vendor : "unknown", out->profile_count);
        copy_txt(out->detail_buf, sizeof(out->detail_buf), tmp);
    }
    return MS_OK;
}

#else /* !Linux host */

ms_status ms_hw_vaapi_probe_device(ms_hw_vaapi_probe *out)
{
    size_t sz;
    if (out == NULL || out->struct_size < sizeof(size_t)) {
        return MS_ERR_INVALID_ARG;
    }
    sz = out->struct_size;
    if (sz > sizeof(*out)) {
        sz = sizeof(*out);
    }
    memset(out, 0, sz);
    out->struct_size = sz;
    out->decode_implemented = 0;
    out->is_hardware = 0;
    out->verified = 1;
    out->libva_loaded = 0;
    out->have_device = 0;
    out->backend = "none";
    out->vendor = NULL;
    out->detail = out->detail_buf;
    snprintf(out->detail_buf, sizeof(out->detail_buf),
             "VAAPI probe not built for this platform; decode/submit not implemented");
    return MS_OK;
}

#endif

int ms_hw_vaapi_probe_has_vld(const ms_hw_vaapi_probe *p, int va_profile)
{
    int i;
    if (p == NULL || !p->have_device) {
        return 0;
    }
    for (i = 0; i < p->profile_count && i < MS_HW_VAAPI_PROFILE_MAX; i++) {
        if (p->profiles[i] == va_profile) {
            return p->profile_has_vld[i] ? 1 : 0;
        }
    }
    return 0;
}

static char g_query_detail[192];

static void fill_query_caps(ms_codec codec, const ms_hw_vaapi_probe *pr, ms_hw_caps *out)
{
    ms_hw_caps tmp;
    size_t sz = out->struct_size;
    memset(&tmp, 0, sizeof(tmp));
    tmp.struct_size = sizeof(tmp);
    tmp.codec = codec;
    tmp.kind = MS_HW_KIND_NONE;
    tmp.is_hardware = 0;
    tmp.verified = 1;
    tmp.api_level = 0;
    tmp.backend = (pr->backend != NULL) ? pr->backend : "vaapi";
    tmp.codec_name = NULL;
    copy_txt(g_query_detail, sizeof(g_query_detail),
             pr->detail ? pr->detail : "VAAPI probe complete");
    tmp.detail = g_query_detail;
    if (sz == 0 || sz > sizeof(*out)) {
        sz = sizeof(*out);
    }
    memcpy(out, &tmp, sz);
    out->struct_size = sz;
}

ms_status ms_hw_vaapi_query(ms_codec codec, ms_hw_caps *out)
{
    ms_hw_vaapi_probe pr;
    ms_status st;
    if (out == NULL || out->struct_size < sizeof(size_t)) {
        return MS_ERR_INVALID_ARG;
    }
    ms_hw_vaapi_probe_default(&pr);
    st = ms_hw_vaapi_probe_device(&pr);
    if (st != MS_OK) {
        return st;
    }
    fill_query_caps(codec, &pr, out);
    return MS_OK;
}

ms_status ms_hw_vaapi_configure(ms_hw_decoder *d, const ms_hw_config *cfg)
{
    ms_hw_vaapi_stream st;
    ms_hw_vaapi_probe pr;
    ms_status pst;
    char note[192];

    if (d == NULL || cfg == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    d->caps.codec = cfg->codec;
    d->caps.api_level = 0;
    d->caps.backend = "vaapi";
    d->caps.verified = 1;
    d->caps.kind = MS_HW_KIND_NONE;
    d->caps.is_hardware = 0;
    d->caps.codec_name = NULL;
    d->surface = cfg->surface;
    d->sample_fmt = cfg->sample_fmt;
    d->width = cfg->width;
    d->height = cfg->height;
    d->tb_num = cfg->time_base_num > 0 ? cfg->time_base_num : 1;
    d->tb_den = cfg->time_base_den > 0 ? cfg->time_base_den : 1000000;

    ms_hw_vaapi_stream_default(&st);
    pst = ms_hw_vaapi_parse_stream(cfg->codec, cfg->sample_fmt, cfg->csd, cfg->csd_size, &st);
    if (pst != MS_OK) {
        ms_hw_set_detail(d, "VAAPI refused: bitstream parameters could not be parsed");
        return (pst == MS_ERR_UNSUPPORTED) ? MS_ERR_UNSUPPORTED : MS_ERR_MALFORMED;
    }

    ms_hw_vaapi_probe_default(&pr);
    (void)ms_hw_vaapi_probe_device(&pr);

    if (!st.mapped) {
        snprintf(note, sizeof(note),
                 "parsed %s profile_idc=%u but no VA profile mapping; decode not implemented",
                 st.profile_name ? st.profile_name : "?", st.bitstream_profile);
        ms_hw_set_detail(d, note);
        return MS_ERR_UNSUPPORTED;
    }
    if (!pr.have_device) {
        snprintf(note, sizeof(note),
                 "parsed %s; %s",
                 st.profile_name ? st.profile_name : "?",
                 pr.detail ? pr.detail : "no VAAPI device");
        ms_hw_set_detail(d, note);
        return MS_ERR_UNSUPPORTED;
    }
    if (!ms_hw_vaapi_probe_has_vld(&pr, st.va_profile)) {
        snprintf(note, sizeof(note),
                 "parsed %s but device has no VLD entry for it; decode not implemented",
                 st.profile_name ? st.profile_name : "?");
        ms_hw_set_detail(d, note);
        return MS_ERR_UNSUPPORTED;
    }
    snprintf(note, sizeof(note),
             "parsed %s on %s; VAAPI decode/submit not implemented",
             st.profile_name ? st.profile_name : "?",
             pr.device_path[0] ? pr.device_path : "device");
    ms_hw_set_detail(d, note);
    return MS_ERR_UNSUPPORTED;
}
