/*
 * avcC / ASC 共用校验，供渐进 MP4/MOV、fMP4 与 FLV 复用。
 * by: Long <mycjzb@gmail.com>
 */
#include "ms_avcc.h"

static uint16_t rd_be16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

ms_status ms_avcc_parse(const uint8_t *d, size_t n, uint8_t *nal_len_size)
{
    size_t off;
    unsigned nsps, npps, i;
    if (nal_len_size == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    if (d == NULL || n < 7 || d[0] != 1) {
        return MS_ERR_UNSUPPORTED;
    }
    *nal_len_size = (uint8_t)((d[4] & 3u) + 1u);
    if (*nal_len_size < 1) {
        return MS_ERR_MALFORMED;
    }
    nsps = (unsigned)(d[5] & 31u);
    off = 6;
    if (nsps == 0) {
        return MS_ERR_UNSUPPORTED;
    }
    for (i = 0; i < nsps; i++) {
        uint16_t ln;
        if (off + 2 > n) {
            return MS_ERR_MALFORMED;
        }
        ln = rd_be16(d + off);
        off += 2;
        if (off + ln > n || ln == 0) {
            return MS_ERR_MALFORMED;
        }
        off += ln;
    }
    if (off >= n) {
        return MS_ERR_MALFORMED;
    }
    npps = d[off++];
    if (npps == 0) {
        return MS_ERR_UNSUPPORTED;
    }
    for (i = 0; i < npps; i++) {
        uint16_t ln;
        if (off + 2 > n) {
            return MS_ERR_MALFORMED;
        }
        ln = rd_be16(d + off);
        off += 2;
        if (off + ln > n || ln == 0) {
            return MS_ERR_MALFORMED;
        }
        off += ln;
    }
    return MS_OK;
}

ms_status ms_avcc_validate_sample(const uint8_t *d, size_t n, unsigned ls)
{
    size_t off = 0;
    if (d == NULL || ls == 0 || ls > 4 || n == 0) {
        return MS_ERR_MALFORMED;
    }
    while (off < n) {
        uint64_t nalsz = 0;
        unsigned k;
        if (off + ls > n) {
            return MS_ERR_MALFORMED;
        }
        for (k = 0; k < ls; k++) {
            nalsz = (nalsz << 8) | (uint64_t)d[off + k];
        }
        off += ls;
        if (nalsz == 0 || off + nalsz > n) {
            return MS_ERR_MALFORMED;
        }
        off += (size_t)nalsz;
    }
    if (off != n) {
        return MS_ERR_MALFORMED;
    }
    return MS_OK;
}

int ms_asc_ok(const uint8_t *d, size_t n)
{
    unsigned aot;
    unsigned sfi;
    unsigned ch;
    if (d == NULL || n < 2) {
        return 0;
    }
    aot = (unsigned)(d[0] >> 3);
    if (aot == 31 || aot != 2) {
        return 0;
    }
    sfi = (unsigned)(((d[0] & 7u) << 1) | (d[1] >> 7));
    if (sfi >= 15) {
        return 0;
    }
    ch = (unsigned)((d[1] >> 3) & 0x0Fu);
    if (ch == 0 || ch > 7) {
        return 0;
    }
    return 1;
}

int ms_asc_parse(const uint8_t *d, size_t n,
                 int *audio_object_type, int *sample_rate, int *channels)
{
    static const int k_rate[13] = {
        96000, 88200, 64000, 48000, 44100, 32000, 24000,
        22050, 16000, 12000, 11025, 8000, 7350
    };
    unsigned aot, sfi, ch;
    if (!ms_asc_ok(d, n)) {
        return 0;
    }
    aot = (unsigned)(d[0] >> 3);
    sfi = (unsigned)(((d[0] & 7u) << 1) | (d[1] >> 7));
    ch = (unsigned)((d[1] >> 3) & 0x0Fu);
    if (sfi > 12u) {
        return 0;
    }
    if (audio_object_type) {
        *audio_object_type = (int)aot;
    }
    if (sample_rate) {
        *sample_rate = k_rate[sfi];
    }
    if (channels) {
        *channels = (int)ch;
    }
    return 1;
}

ms_status ms_aac_asc_parse(const uint8_t *data, size_t size, ms_aac_asc_info *out)
{
    int aot = 0, rate = 0, ch = 0;
    if (out == NULL || out->struct_size < sizeof(size_t)) {
        return MS_ERR_INVALID_ARG;
    }
    if (!ms_asc_parse(data, size, &aot, &rate, &ch)) {
        return (data == NULL || size == 0) ? MS_ERR_INVALID_ARG : MS_ERR_UNSUPPORTED;
    }
    if (out->struct_size > sizeof(*out)) {
        out->struct_size = sizeof(*out);
    }
    out->audio_object_type = aot;
    out->sample_rate = rate;
    out->channels = ch;
    return MS_OK;
}
