/*
 * 极简 FLV 写出器实现，见 ms_flvw.h。
 * by: Long <mycjzb@gmail.com>
 */
#include "ms_flvw.h"

#include <string.h>

int ms_flvw_header(ms_flvw *w, int has_audio, int has_video)
{
    uint8_t h[13] = { 'F', 'L', 'V', 1, 0, 0, 0, 0, 9, 0, 0, 0, 0 };
    if (w->hdr_done) return 0;
    h[4] = (uint8_t)((has_audio ? 4 : 0) | (has_video ? 1 : 0));
    w->hdr_done = 1;
    return w->write(w->ctx, h, sizeof(h));
}

int ms_flvw_tag(ms_flvw *w, int type, uint32_t ts, const uint8_t *head, size_t nh,
                const uint8_t *payload, size_t np)
{
    uint8_t th[11], tail[4];
    size_t ds = nh + np;
    uint32_t prev;
    int r;
    if (ds > 0xFFFFFFu) return -1;
    if (!w->hdr_done && (r = ms_flvw_header(w, 1, 1)) != 0) return r;
    th[0] = (uint8_t)type;
    th[1] = (uint8_t)(ds >> 16); th[2] = (uint8_t)(ds >> 8); th[3] = (uint8_t)ds;
    th[4] = (uint8_t)(ts >> 16); th[5] = (uint8_t)(ts >> 8); th[6] = (uint8_t)ts; th[7] = (uint8_t)(ts >> 24);
    th[8] = th[9] = th[10] = 0;
    prev = (uint32_t)(ds + 11);
    tail[0] = (uint8_t)(prev >> 24); tail[1] = (uint8_t)(prev >> 16); tail[2] = (uint8_t)(prev >> 8); tail[3] = (uint8_t)prev;
    if ((r = w->write(w->ctx, th, 11)) != 0) return r;
    if (nh && (r = w->write(w->ctx, head, nh)) != 0) return r;
    if (np && (r = w->write(w->ctx, payload, np)) != 0) return r;
    return w->write(w->ctx, tail, 4);
}

int ms_flvw_video_config(ms_flvw *w, int hevc, uint32_t ts, const uint8_t *rec, size_t n)
{
    uint8_t h[5] = { 0, 0, 0, 0, 0 };
    h[0] = (uint8_t)(0x10 | (hevc ? 12 : 7));
    return ms_flvw_tag(w, 9, ts, h, 5, rec, n);
}

int ms_flvw_video_frame(ms_flvw *w, int hevc, int key, uint32_t ts, int32_t cts, const uint8_t *lp, size_t n)
{
    uint8_t h[5];
    h[0] = (uint8_t)((key ? 0x10 : 0x20) | (hevc ? 12 : 7));
    h[1] = 1;
    h[2] = (uint8_t)((uint32_t)cts >> 16); h[3] = (uint8_t)((uint32_t)cts >> 8); h[4] = (uint8_t)cts;
    return ms_flvw_tag(w, 9, ts, h, 5, lp, n);
}

int ms_flvw_aac_config(ms_flvw *w, uint32_t ts, const uint8_t *asc, size_t n)
{
    static const uint8_t h[2] = { 0xAF, 0x00 };
    return ms_flvw_tag(w, 8, ts, h, 2, asc, n);
}

int ms_flvw_aac_frame(ms_flvw *w, uint32_t ts, const uint8_t *raw, size_t n)
{
    static const uint8_t h[2] = { 0xAF, 0x01 };
    return ms_flvw_tag(w, 8, ts, h, 2, raw, n);
}

size_t ms_flvw_make_avcc(const uint8_t *sps, size_t sn, const uint8_t *pps, size_t pn, uint8_t *o, size_t cap)
{
    size_t k = 0;
    if (!sps || !pps || sn < 4 || pn < 1 || sn > 0xFFFF || pn > 0xFFFF || cap < 11 + sn + pn) return 0;
    o[k++] = 1; o[k++] = sps[1]; o[k++] = sps[2]; o[k++] = sps[3];
    o[k++] = 0xFF; o[k++] = 0xE1;
    o[k++] = (uint8_t)(sn >> 8); o[k++] = (uint8_t)sn;
    memcpy(o + k, sps, sn); k += sn;
    o[k++] = 1;
    o[k++] = (uint8_t)(pn >> 8); o[k++] = (uint8_t)pn;
    memcpy(o + k, pps, pn); k += pn;
    return k;
}

/* 去防竞争字节，取前 cap 字节 RBSP。 */
static size_t rbsp(const uint8_t *d, size_t n, uint8_t *o, size_t cap)
{
    size_t i, k = 0, z = 0;
    for (i = 0; i < n && k < cap; i++) {
        if (z >= 2 && d[i] == 3) { z = 0; continue; }
        o[k++] = d[i];
        z = d[i] == 0 ? z + 1 : 0;
    }
    return k;
}

size_t ms_flvw_make_hvcc(const uint8_t *vps, size_t vn, const uint8_t *sps, size_t sn,
                         const uint8_t *pps, size_t pn, uint8_t *o, size_t cap)
{
    uint8_t r[16];
    size_t k = 0, need = 23 + 3 * 5 + vn + sn + pn;
    const uint8_t *arr[3];
    size_t al[3];
    int i;
    if (!vps || !sps || !pps || sn < 16 || cap < need || vn > 0xFFFF || sn > 0xFFFF || pn > 0xFFFF) return 0;
    /* SPS：2B NAL 头 + 1B(vps_id/max_sub_layers/nesting) 后是 12B profile_tier_level */
    if (rbsp(sps, sn, r, sizeof(r)) < 15) return 0;
    o[k++] = 1;
    o[k++] = r[3];                        /* profile_space/tier/profile_idc */
    memcpy(o + k, r + 4, 4); k += 4;      /* 兼容标志 */
    memcpy(o + k, r + 8, 6); k += 6;      /* 约束标志 */
    o[k++] = r[14];                       /* level_idc */
    o[k++] = 0xF0; o[k++] = 0x00;         /* min_spatial_segmentation */
    o[k++] = 0xFC;                        /* parallelismType 未知 */
    o[k++] = 0xFD;                        /* chroma 4:2:0 */
    o[k++] = 0xF8; o[k++] = 0xF8;         /* 8bit（实际位深由 SPS 决定，解码器以 SPS 为准） */
    o[k++] = 0; o[k++] = 0;               /* avgFrameRate */
    o[k++] = 0x0F;                        /* lengthSizeMinusOne=3 */
    o[k++] = 3;
    arr[0] = vps; al[0] = vn; arr[1] = sps; al[1] = sn; arr[2] = pps; al[2] = pn;
    for (i = 0; i < 3; i++) {
        o[k++] = (uint8_t)(0x80 | (32 + i));
        o[k++] = 0; o[k++] = 1;
        o[k++] = (uint8_t)(al[i] >> 8); o[k++] = (uint8_t)al[i];
        memcpy(o + k, arr[i], al[i]); k += al[i];
    }
    return k;
}

const uint8_t *ms_flvw_next_nal(const uint8_t *d, size_t n, size_t *pos, size_t *nl)
{
    size_t i = *pos, s, e;
    while (i + 3 <= n && !(d[i] == 0 && d[i + 1] == 0 && (d[i + 2] == 1 || (i + 4 <= n && d[i + 2] == 0 && d[i + 3] == 1)))) i++;
    if (i + 3 > n) { *pos = n; return NULL; }
    s = i + (d[i + 2] == 1 ? 3 : 4);
    e = s;
    while (e + 3 <= n && !(d[e] == 0 && d[e + 1] == 0 && (d[e + 2] == 1 || (e + 4 <= n && d[e + 2] == 0 && d[e + 3] == 1)))) e++;
    if (e + 3 > n) e = n;
    *pos = e;
    while (e > s && d[e - 1] == 0) e--; /* 去尾随 0（trailing_zero_8bits） */
    *nl = e - s;
    return *nl ? d + s : ms_flvw_next_nal(d, n, pos, nl);
}

size_t ms_flvw_annexb_to_lp(const uint8_t *d, size_t n, uint8_t *o, size_t cap)
{
    size_t pos = 0, k = 0, nl;
    const uint8_t *nal;
    while ((nal = ms_flvw_next_nal(d, n, &pos, &nl)) != NULL) {
        if (k + 4 + nl > cap) return 0;
        o[k++] = (uint8_t)(nl >> 24); o[k++] = (uint8_t)(nl >> 16); o[k++] = (uint8_t)(nl >> 8); o[k++] = (uint8_t)nl;
        memcpy(o + k, nal, nl);
        k += nl;
    }
    return k;
}
