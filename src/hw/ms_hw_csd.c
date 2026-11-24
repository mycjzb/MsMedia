/*
 * 从 avcC / Annex B 取出 H.264 SPS/PPS，或从 hvcC / Annex B 取出 HEVC
 * VPS+SPS+PPS（csd-0 为带起始码的拼接，csd-1 为空），或把 MPEG-2/MPEG-4
 * 序列头/VOL 原样放入 csd-0（csd-1 为空），供 MediaCodec。
 * H.263 通常无 CSD，csd-0/csd-1 均为空，不得捏造。
 * 只做有界拷贝，不分配整帧缓冲。
 * by: Long <mycjzb@gmail.com>
 */
#include "ms_hw_internal.h"

#include <string.h>

static ms_status copy_nal(const uint8_t *p, size_t n, uint8_t *dst, size_t *out, size_t cap)
{
    if (n == 0 || n > cap) {
        return MS_ERR_OVERFLOW;
    }
    memcpy(dst, p, n);
    *out = n;
    return MS_OK;
}

static const uint8_t *start_code(const uint8_t *p, const uint8_t *end, size_t *sc)
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

static ms_status split_annexb(const uint8_t *csd, size_t n,
                              uint8_t *csd0, size_t *n0, size_t cap0,
                              uint8_t *csd1, size_t *n1, size_t cap1)
{
    const uint8_t *end = csd + n;
    const uint8_t *p = csd;
    *n0 = 0;
    *n1 = 0;
    while (p < end) {
        size_t sc = 0;
        const uint8_t *nal = start_code(p, end, &sc);
        const uint8_t *next;
        size_t nn;
        unsigned typ;
        if (nal == NULL) {
            break;
        }
        nal += sc;
        next = start_code(nal, end, &sc);
        nn = (size_t)((next ? next : end) - nal);
        if (nn == 0) {
            p = next ? next : end;
            continue;
        }
        typ = nal[0] & 0x1fu;
        if (typ == 7 && *n0 == 0) {
            if (copy_nal(nal, nn, csd0, n0, cap0) != MS_OK) {
                return MS_ERR_OVERFLOW;
            }
        } else if (typ == 8 && *n1 == 0) {
            if (copy_nal(nal, nn, csd1, n1, cap1) != MS_OK) {
                return MS_ERR_OVERFLOW;
            }
        }
        p = next ? next : end;
    }
    if (*n0 == 0) {
        return MS_ERR_MALFORMED;
    }
    return MS_OK;
}

static ms_status split_avcc(const uint8_t *d, size_t n,
                            uint8_t *csd0, size_t *n0, size_t cap0,
                            uint8_t *csd1, size_t *n1, size_t cap1)
{
    size_t off;
    unsigned i, nSPS, nPPS;
    *n0 = 0;
    *n1 = 0;
    if (n < 7 || d[0] != 1) {
        return MS_ERR_MALFORMED;
    }
    off = 5;
    nSPS = (unsigned)(d[off++] & 0x1fu);
    for (i = 0; i < nSPS; i++) {
        unsigned ln;
        if (off + 2 > n) {
            return MS_ERR_MALFORMED;
        }
        ln = ((unsigned)d[off] << 8) | (unsigned)d[off + 1];
        off += 2;
        if (off + ln > n) {
            return MS_ERR_MALFORMED;
        }
        if (*n0 == 0) {
            if (copy_nal(d + off, ln, csd0, n0, cap0) != MS_OK) {
                return MS_ERR_OVERFLOW;
            }
        }
        off += ln;
    }
    if (off >= n) {
        return (*n0 > 0) ? MS_OK : MS_ERR_MALFORMED;
    }
    nPPS = (unsigned)d[off++];
    for (i = 0; i < nPPS; i++) {
        unsigned ln;
        if (off + 2 > n) {
            return MS_ERR_MALFORMED;
        }
        ln = ((unsigned)d[off] << 8) | (unsigned)d[off + 1];
        off += 2;
        if (off + ln > n) {
            return MS_ERR_MALFORMED;
        }
        if (*n1 == 0) {
            if (copy_nal(d + off, ln, csd1, n1, cap1) != MS_OK) {
                return MS_ERR_OVERFLOW;
            }
        }
        off += ln;
    }
    if (*n0 == 0) {
        return MS_ERR_MALFORMED;
    }
    return MS_OK;
}

ms_status ms_hw_csd_split(ms_sample_format fmt, const uint8_t *csd, size_t n,
                          uint8_t *csd0, size_t *n0, size_t cap0,
                          uint8_t *csd1, size_t *n1, size_t cap1)
{
    if (csd == NULL || n == 0 || csd0 == NULL || n0 == NULL || csd1 == NULL || n1 == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    if (fmt == MS_SAMPLE_FMT_AVCC) {
        return split_avcc(csd, n, csd0, n0, cap0, csd1, n1, cap1);
    }
    if (fmt == MS_SAMPLE_FMT_ANNEXB) {
        return split_annexb(csd, n, csd0, n0, cap0, csd1, n1, cap1);
    }
    return MS_ERR_UNSUPPORTED;
}

ms_status ms_hw_csd_split_for_codec(ms_codec codec, ms_sample_format fmt,
                                    const uint8_t *csd, size_t n,
                                    uint8_t *csd0, size_t *n0, size_t cap0,
                                    uint8_t *csd1, size_t *n1, size_t cap1)
{
    ms_hevc_ps_set ps;
    size_t packed = 0;
    unsigned ls = 4;
    if (codec == MS_CODEC_H263) {
        if (n0 == NULL || n1 == NULL) {
            return MS_ERR_INVALID_ARG;
        }
        if (n != 0) {
            return MS_ERR_INVALID_ARG;
        }
        *n0 = 0;
        *n1 = 0;
        return MS_OK;
    }
    if (codec == MS_CODEC_AV1) {
        if (n0 == NULL || n1 == NULL) {
            return MS_ERR_INVALID_ARG;
        }
        if (n == 0) {
            *n0 = 0;
            *n1 = 0;
            return MS_OK; /* 合法无 CSD */
        }
    }
    if (codec == MS_CODEC_MPEG2_VIDEO || codec == MS_CODEC_MPEG4_PART2 ||
        codec == MS_CODEC_AV1 || codec == MS_CODEC_AVS_PLUS) {
        if (csd == NULL || n == 0 || csd0 == NULL || n0 == NULL || csd1 == NULL || n1 == NULL) {
            return MS_ERR_INVALID_ARG;
        }
        *n0 = 0;
        *n1 = 0;
        if (n > cap0) {
            return MS_ERR_OVERFLOW;
        }
        memcpy(csd0, csd, n);
        *n0 = n;
        return MS_OK;
    }
    if (codec != MS_CODEC_HEVC) {
        return ms_hw_csd_split(fmt, csd, n, csd0, n0, cap0, csd1, n1, cap1);
    }
    if (csd == NULL || n == 0 || csd0 == NULL || n0 == NULL || csd1 == NULL || n1 == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    *n0 = 0;
    *n1 = 0;
    if (fmt == MS_SAMPLE_FMT_AVCC) {
        return MS_ERR_UNSUPPORTED;
    }
    if (fmt == MS_SAMPLE_FMT_HVCC && n >= 23 && csd[0] == 1) {
        ls = (unsigned)((csd[21] & 3u) + 1u);
    }
    memset(&ps, 0, sizeof(ps));
    ps.struct_size = sizeof(ps);
    if (ms_hevc_ps_parse(fmt, 1, csd, n, ls, &ps) != MS_OK || !ps.triple_complete) {
        return MS_ERR_MALFORMED;
    }
    if (ms_hevc_ps_pack(MS_SAMPLE_FMT_ANNEXB, 4, &ps, csd0, cap0, &packed) != MS_OK) {
        return MS_ERR_OVERFLOW;
    }
    *n0 = packed;
    *n1 = 0;
    return MS_OK;
}

int ms_hw_nal_with_start_code(const uint8_t *nal, size_t n,
                              uint8_t *dst, size_t dst_cap, size_t *out_n)
{
    if (nal == NULL || dst == NULL || out_n == NULL || n == 0) {
        return -1;
    }
    if (n >= 3 && nal[0] == 0 && nal[1] == 0 && nal[2] == 1) {
        if (n > dst_cap) {
            return -1;
        }
        memcpy(dst, nal, n);
        *out_n = n;
        return 0;
    }
    if (n >= 4 && nal[0] == 0 && nal[1] == 0 && nal[2] == 0 && nal[3] == 1) {
        if (n > dst_cap) {
            return -1;
        }
        memcpy(dst, nal, n);
        *out_n = n;
        return 0;
    }
    if (n + 4u > dst_cap) {
        return -1;
    }
    dst[0] = 0;
    dst[1] = 0;
    dst[2] = 0;
    dst[3] = 1;
    memcpy(dst + 4, nal, n);
    *out_n = n + 4u;
    return 0;
}

int ms_hw_avcc_au_to_annexb(const uint8_t *src, size_t src_n, unsigned ls,
                            uint8_t *dst, size_t dst_cap, size_t *out_n)
{
    size_t off = 0, wr = 0;
    if (src == NULL || dst == NULL || out_n == NULL || ls < 1 || ls > 4) {
        return -1;
    }
    /* 已是起始码则原样。 */
    if (src_n >= 3 && src[0] == 0 && src[1] == 0 &&
        (src[2] == 1 || (src_n >= 4 && src[2] == 0 && src[3] == 1))) {
        if (src_n > dst_cap) {
            return -1;
        }
        memcpy(dst, src, src_n);
        *out_n = src_n;
        return 0;
    }
    while (off < src_n) {
        size_t ln = 0, i;
        if (off + ls > src_n) {
            return -1;
        }
        for (i = 0; i < ls; i++) {
            ln = (ln << 8) | (size_t)src[off + i];
        }
        off += ls;
        if (ln > src_n - off) {
            return -1;
        }
        if (wr + 4u + ln > dst_cap) {
            return -1;
        }
        dst[wr + 0] = 0;
        dst[wr + 1] = 0;
        dst[wr + 2] = 0;
        dst[wr + 3] = 1;
        if (ln > 0) {
            memcpy(dst + wr + 4, src + off, ln);
        }
        wr += 4u + ln;
        off += ln;
    }
    *out_n = wr;
    return 0;
}

