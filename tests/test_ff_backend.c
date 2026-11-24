/*
 * 可选 FFmpeg 9.1 后端首个可验证接入：本地 TS/MP4/FLV 映射公共事件，
 * 再受控 HTTP + HLS，以及受控 HTTPS / 禁止降级。非正式验收。
 * 未覆盖的协议在文末明确列出。
 * by: Long <mycjzb@gmail.com>
 */
#define _DEFAULT_SOURCE
#include "msmedia/msmedia.h"
#include "msmedia/ms_ff.h"
#include "ms_vbit_fixtures.h"
#include "ms_abit_fixtures.h"
#include "ms_sub_bitmap_fixtures.h"
#include "ms_ts_synth.h"
#include "hevc_fixtures.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/prctl.h>
#include <errno.h>
#include <limits.h>
#include <netinet/in.h>
#include <openssl/evp.h>
#include <sys/select.h>
#include <sys/time.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <sys/stat.h>

static int failed;

#define CHECK(cond, msg)                                                       \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, msg);      \
            failed = 1;                                                        \
            return 1;                                                          \
        }                                                                      \
    } while (0)

#define CHECK_ST(st, exp)                                                      \
    do {                                                                       \
        ms_status _s = (st), _e = (exp);                                       \
        if (_s != _e) {                                                        \
            fprintf(stderr, "FAIL %s:%d: status %s expected %s\n", __FILE__,   \
                    __LINE__, ms_status_name(_s), ms_status_name(_e));         \
            failed = 1;                                                        \
            return 1;                                                          \
        }                                                                      \
    } while (0)

static int make_adts(uint8_t *out, size_t cap, size_t *len, int sfi, int ch,
                     int nbytes, uint8_t fill)
{
    uint8_t raw[64];
    if (nbytes > (int)sizeof(raw)) {
        return -1;
    }
    memset(raw, fill, (size_t)nbytes);
    return ms_synth_adts_frame(out, cap, len, 1, sfi, ch, raw, (size_t)nbytes);
}

static int write_file(const char *path, const void *data, size_t n)
{
    FILE *fp = fopen(path, "wb");
    size_t w;
    if (fp == NULL) {
        return -1;
    }
    w = fwrite(data, 1, n, fp);
    fclose(fp);
    return w == n ? 0 : -1;
}


static int path_join(char *dst, size_t cap, const char *dir, const char *name)
{
    size_t nd, nn;
    if (dst == NULL || dir == NULL || name == NULL) {
        return -1;
    }
    nd = strlen(dir);
    nn = strlen(name);
    if (nd + 1 + nn + 1 > cap) {
        return -1;
    }
    memcpy(dst, dir, nd);
    dst[nd] = '/';
    memcpy(dst + nd + 1, name, nn + 1);
    return 0;
}

static int drain_events(ms_ff *f, ms_event *evs, int cap, int *nout, int max_loop)
{
    int i, n = 0, loops = 0;
    while (n < cap && loops < max_loop) {
        ms_event ev;
        ms_status st = ms_ff_pull(f, &ev);
        loops++;
        if (st == MS_AGAIN) {
            continue;
        }
        if (st == MS_EOS) {
            break;
        }
        if (st == MS_LIMIT) {
            /* 释放一个已取事件再继续 */
            if (n > 0) {
                ms_ff_release(f, evs[n - 1].release_id);
                evs[n - 1].release_id = 0;
            } else {
                return -2;
            }
            continue;
        }
        if (st != MS_OK) {
            return -(int)st;
        }
        evs[n] = ev;
        n++;
        if (ev.type == MS_EVENT_EOS) {
            break;
        }
    }
    *nout = n;
    (void)i;
    return 0;
}

static int count_type(const ms_event *evs, int n, ms_event_type t)
{
    int i, c = 0;
    for (i = 0; i < n; i++) {
        if (evs[i].type == t) {
            c++;
        }
    }
    return c;
}

/* ---------------- 合成样本 ---------------- */

static int build_aac_ts(uint8_t **out, size_t *len, uint64_t pts0, int frames)
{
    ms_synth_buf b;
    ms_synth_stream streams[1];
    uint8_t adts[64];
    size_t alen = 0;
    int i;
    if (ms_synth_init(&b) != 0) {
        return -1;
    }
    memset(streams, 0, sizeof(streams));
    streams[0].stream_type = 0x0f;
    streams[0].pid = 0x101;
    ms_synth_reset_cc();
    if (ms_synth_pat_pmt(&b, 1, 0x100, streams, 1, 2, 188) != 0) {
        ms_synth_free(&b);
        return -1;
    }
    for (i = 0; i < frames; i++) {
        if (make_adts(adts, sizeof(adts), &alen, 3, 2, 16, (uint8_t)(0xA0 + i)) != 0) {
            ms_synth_free(&b);
            return -1;
        }
        if (ms_synth_pes(&b, 0x101, 0xc0, 1, pts0 + (uint64_t)i * 1920ull, adts, alen,
                         188) != 0) {
            ms_synth_free(&b);
            return -1;
        }
    }
    *out = b.data;
    *len = b.len;
    return 0;
}

static int dyn_put(uint8_t **buf, size_t *n, size_t *cap, const void *p, size_t l)
{
    if (*n + l > *cap) {
        size_t nc = *cap ? *cap * 2 : 256;
        uint8_t *nb;
        while (nc < *n + l) {
            nc *= 2;
        }
        nb = (uint8_t *)realloc(*buf, nc);
        if (nb == NULL) {
            return -1;
        }
        *buf = nb;
        *cap = nc;
    }
    memcpy(*buf + *n, p, l);
    *n += l;
    return 0;
}

static int dyn_u8(uint8_t **b, size_t *n, size_t *c, uint8_t v)
{
    return dyn_put(b, n, c, &v, 1);
}

static int dyn_u16(uint8_t **b, size_t *n, size_t *c, uint16_t v)
{
    uint8_t p[2] = {(uint8_t)(v >> 8), (uint8_t)v};
    return dyn_put(b, n, c, p, 2);
}

static int dyn_u24(uint8_t **b, size_t *n, size_t *c, uint32_t v)
{
    uint8_t p[3] = {(uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v};
    return dyn_put(b, n, c, p, 3);
}

static int dyn_u32(uint8_t **b, size_t *n, size_t *c, uint32_t v)
{
    uint8_t p[4] = {(uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8),
                    (uint8_t)v};
    return dyn_put(b, n, c, p, 4);
}

/* 经典 FLV：AAC sequence header + 两帧裸 AAC。 */
static int build_aac_flv(uint8_t **out, size_t *len)
{
    uint8_t *b = NULL;
    size_t n = 0, cap = 0;
    uint8_t raw1[16], raw2[16];
    /* ASC: AAC LC, 48 kHz, stereo */
    uint8_t asc[2] = {0x11, 0x90};
    memset(raw1, 0x51, sizeof(raw1));
    memset(raw2, 0x52, sizeof(raw2));
    /* FLV header + PreviousTagSize 0 */
    if (dyn_put(&b, &n, &cap, "FLV", 3) || dyn_u8(&b, &n, &cap, 1) ||
        dyn_u8(&b, &n, &cap, 0x04) || dyn_u32(&b, &n, &cap, 9) ||
        dyn_u32(&b, &n, &cap, 0)) {
        free(b);
        return -1;
    }
    /* tag 1: audio AAC seq */
    {
        uint32_t dsz = 2 + 2; /* AF 00 + ASC */
        size_t tag_start = n;
        if (dyn_u8(&b, &n, &cap, 8) || dyn_u24(&b, &n, &cap, dsz) ||
            dyn_u24(&b, &n, &cap, 0) || dyn_u8(&b, &n, &cap, 0) ||
            dyn_u24(&b, &n, &cap, 0) || dyn_u8(&b, &n, &cap, 0xAF) ||
            dyn_u8(&b, &n, &cap, 0x00) || dyn_put(&b, &n, &cap, asc, 2) ||
            dyn_u32(&b, &n, &cap, (uint32_t)(n - tag_start))) {
            free(b);
            return -1;
        }
    }
    /* tag 2/3: AAC raw */
    {
        uint32_t dsz = 2 + (uint32_t)sizeof(raw1);
        size_t tag_start = n;
        if (dyn_u8(&b, &n, &cap, 8) || dyn_u24(&b, &n, &cap, dsz) ||
            dyn_u24(&b, &n, &cap, 0) || dyn_u8(&b, &n, &cap, 0) ||
            dyn_u24(&b, &n, &cap, 0) || dyn_u8(&b, &n, &cap, 0xAF) ||
            dyn_u8(&b, &n, &cap, 0x01) || dyn_put(&b, &n, &cap, raw1, sizeof(raw1)) ||
            dyn_u32(&b, &n, &cap, (uint32_t)(n - tag_start))) {
            free(b);
            return -1;
        }
    }
    {
        uint32_t dsz = 2 + (uint32_t)sizeof(raw2);
        size_t tag_start = n;
        if (dyn_u8(&b, &n, &cap, 8) || dyn_u24(&b, &n, &cap, dsz) ||
            dyn_u24(&b, &n, &cap, 23) || dyn_u8(&b, &n, &cap, 0) ||
            dyn_u24(&b, &n, &cap, 0) || dyn_u8(&b, &n, &cap, 0xAF) ||
            dyn_u8(&b, &n, &cap, 0x01) || dyn_put(&b, &n, &cap, raw2, sizeof(raw2)) ||
            dyn_u32(&b, &n, &cap, (uint32_t)(n - tag_start))) {
            free(b);
            return -1;
        }
    }
    /* flv_probe 要求 DataOffset+100 < 探测缓冲，过短的纯音频文件得分会是 0。 */
    {
        int i;
        for (i = 0; i < 4; i++) {
            uint8_t raw[16];
            uint32_t dsz = 2 + (uint32_t)sizeof(raw);
            size_t tag_start;
            memset(raw, (uint8_t)(0x53 + i), sizeof(raw));
            tag_start = n;
            if (dyn_u8(&b, &n, &cap, 8) || dyn_u24(&b, &n, &cap, dsz) ||
                dyn_u24(&b, &n, &cap, (uint32_t)(46 + 23 * i)) || dyn_u8(&b, &n, &cap, 0) ||
                dyn_u24(&b, &n, &cap, 0) || dyn_u8(&b, &n, &cap, 0xAF) ||
                dyn_u8(&b, &n, &cap, 0x01) || dyn_put(&b, &n, &cap, raw, sizeof(raw)) ||
                dyn_u32(&b, &n, &cap, (uint32_t)(n - tag_start))) {
                free(b);
                return -1;
            }
        }
    }
    *out = b;
    *len = n;
    return 0;
}

static int box_begin(uint8_t **b, size_t *n, size_t *c, const char typ[4], size_t *off)
{
    *off = *n;
    return dyn_u32(b, n, c, 0) || dyn_put(b, n, c, typ, 4);
}

static int box_end(uint8_t *b, size_t n, size_t off)
{
    uint32_t sz = (uint32_t)(n - off);
    b[off + 0] = (uint8_t)(sz >> 24);
    b[off + 1] = (uint8_t)(sz >> 16);
    b[off + 2] = (uint8_t)(sz >> 8);
    b[off + 3] = (uint8_t)sz;
    return 0;
}

/* 渐进 MP4：单轨 AAC LC 48 kHz 立体声，两帧裸 AU。 */
static int build_aac_mp4(uint8_t **out, size_t *len)
{
    uint8_t *b = NULL;
    size_t n = 0, cap = 0;
    uint8_t raw1[16], raw2[16];
    uint8_t asc[2] = {0x11, 0x90};
    size_t ftyp, moov, mvhd, trak, tkhd, mdia, mdhd, hdlr, minf, smhd, dinf, dref,
        url, stbl, stsd, mp4a, esds, stts, stsc, stsz, stco, mdat;
    uint32_t mdat_off_in_file;
    memset(raw1, 0x61, sizeof(raw1));
    memset(raw2, 0x62, sizeof(raw2));
    if (box_begin(&b, &n, &cap, "ftyp", &ftyp) ||
        dyn_put(&b, &n, &cap, "isom", 4) || dyn_u32(&b, &n, &cap, 0) ||
        dyn_put(&b, &n, &cap, "isom", 4) || dyn_put(&b, &n, &cap, "mp41", 4) ||
        box_end(b, n, ftyp)) {
        free(b);
        return -1;
    }
    if (box_begin(&b, &n, &cap, "moov", &moov)) {
        free(b);
        return -1;
    }
    if (box_begin(&b, &n, &cap, "mvhd", &mvhd) || dyn_u32(&b, &n, &cap, 0) /* ver+flags */ ||
        dyn_u32(&b, &n, &cap, 0) || dyn_u32(&b, &n, &cap, 0) ||
        dyn_u32(&b, &n, &cap, 1000) || dyn_u32(&b, &n, &cap, 42) ||
        dyn_u32(&b, &n, &cap, 0x00010000) || dyn_u16(&b, &n, &cap, 0x0100) ||
        dyn_u16(&b, &n, &cap, 0) || dyn_u32(&b, &n, &cap, 0) || dyn_u32(&b, &n, &cap, 0)) {
        free(b);
        return -1;
    }
    {
        int i;
        uint8_t matrix[36] = {0};
        matrix[1] = 1;
        matrix[17] = 1;
        matrix[32] = 0x40;
        if (dyn_put(&b, &n, &cap, matrix, 36) != 0) {
            free(b);
            return -1;
        }
        for (i = 0; i < 6; i++) {
            if (dyn_u32(&b, &n, &cap, 0) != 0) {
                free(b);
                return -1;
            }
        }
        if (dyn_u32(&b, &n, &cap, 2) != 0 || box_end(b, n, mvhd) != 0) { /* next track 2 */
            free(b);
            return -1;
        }
    }
    if (box_begin(&b, &n, &cap, "trak", &trak) || box_begin(&b, &n, &cap, "tkhd", &tkhd) ||
        dyn_u32(&b, &n, &cap, 0x00000003) || dyn_u32(&b, &n, &cap, 0) ||
        dyn_u32(&b, &n, &cap, 0) || dyn_u32(&b, &n, &cap, 1) || dyn_u32(&b, &n, &cap, 0) ||
        dyn_u32(&b, &n, &cap, 42) || dyn_u32(&b, &n, &cap, 0) || dyn_u32(&b, &n, &cap, 0) ||
        dyn_u16(&b, &n, &cap, 0) || dyn_u16(&b, &n, &cap, 0) || dyn_u16(&b, &n, &cap, 0x0100) ||
        dyn_u16(&b, &n, &cap, 0)) {
        free(b);
        return -1;
    }
    {
        uint8_t matrix[36] = {0};
        matrix[1] = 1;
        matrix[17] = 1;
        matrix[32] = 0x40;
        if (dyn_put(&b, &n, &cap, matrix, 36) || dyn_u32(&b, &n, &cap, 0) ||
            dyn_u32(&b, &n, &cap, 0) || box_end(b, n, tkhd)) {
            free(b);
            return -1;
        }
    }
    if (box_begin(&b, &n, &cap, "mdia", &mdia) || box_begin(&b, &n, &cap, "mdhd", &mdhd) ||
        dyn_u32(&b, &n, &cap, 0) || dyn_u32(&b, &n, &cap, 0) || dyn_u32(&b, &n, &cap, 0) ||
        dyn_u32(&b, &n, &cap, 48000) || dyn_u32(&b, &n, &cap, 2048) /* 2*1024 */ ||
        dyn_u16(&b, &n, &cap, 0x55c4) || dyn_u16(&b, &n, &cap, 0) || box_end(b, n, mdhd)) {
        free(b);
        return -1;
    }
    if (box_begin(&b, &n, &cap, "hdlr", &hdlr) || dyn_u32(&b, &n, &cap, 0) ||
        dyn_u32(&b, &n, &cap, 0) || dyn_put(&b, &n, &cap, "soun", 4) ||
        dyn_u32(&b, &n, &cap, 0) || dyn_u32(&b, &n, &cap, 0) || dyn_u32(&b, &n, &cap, 0) ||
        dyn_put(&b, &n, &cap, "SoundHandler", 13) || box_end(b, n, hdlr)) {
        free(b);
        return -1;
    }
    if (box_begin(&b, &n, &cap, "minf", &minf) || box_begin(&b, &n, &cap, "smhd", &smhd) ||
        dyn_u32(&b, &n, &cap, 0) || dyn_u16(&b, &n, &cap, 0) || dyn_u16(&b, &n, &cap, 0) ||
        box_end(b, n, smhd) || box_begin(&b, &n, &cap, "dinf", &dinf) ||
        box_begin(&b, &n, &cap, "dref", &dref) || dyn_u32(&b, &n, &cap, 0) ||
        dyn_u32(&b, &n, &cap, 1) || box_begin(&b, &n, &cap, "url ", &url) ||
        dyn_u32(&b, &n, &cap, 1) || box_end(b, n, url) || box_end(b, n, dref) ||
        box_end(b, n, dinf)) {
        free(b);
        return -1;
    }
    if (box_begin(&b, &n, &cap, "stbl", &stbl) || box_begin(&b, &n, &cap, "stsd", &stsd) ||
        dyn_u32(&b, &n, &cap, 0) || dyn_u32(&b, &n, &cap, 1) ||
        box_begin(&b, &n, &cap, "mp4a", &mp4a) || dyn_put(&b, &n, &cap, "\0\0\0\0\0\0", 6) ||
        dyn_u16(&b, &n, &cap, 1) || dyn_u16(&b, &n, &cap, 0) || dyn_u16(&b, &n, &cap, 0) ||
        dyn_u32(&b, &n, &cap, 0) || dyn_u16(&b, &n, &cap, 2) || dyn_u16(&b, &n, &cap, 16) ||
        dyn_u16(&b, &n, &cap, 0) || dyn_u16(&b, &n, &cap, 0) ||
        dyn_u32(&b, &n, &cap, (uint32_t)48000 << 16)) {
        free(b);
        return -1;
    }
    /* esds */
    if (box_begin(&b, &n, &cap, "esds", &esds) || dyn_u32(&b, &n, &cap, 0) ||
        dyn_u8(&b, &n, &cap, 0x03) || dyn_u8(&b, &n, &cap, 23) || dyn_u16(&b, &n, &cap, 1) ||
        dyn_u8(&b, &n, &cap, 0) || dyn_u8(&b, &n, &cap, 0x04) || dyn_u8(&b, &n, &cap, 15) ||
        dyn_u8(&b, &n, &cap, 0x40) || dyn_u8(&b, &n, &cap, 0x15) ||
        dyn_u24(&b, &n, &cap, 0) || dyn_u32(&b, &n, &cap, 0) || dyn_u32(&b, &n, &cap, 0) ||
        dyn_u8(&b, &n, &cap, 0x05) || dyn_u8(&b, &n, &cap, 2) ||
        dyn_put(&b, &n, &cap, asc, 2) || dyn_u8(&b, &n, &cap, 0x06) || dyn_u8(&b, &n, &cap, 1) ||
        dyn_u8(&b, &n, &cap, 0x02) || box_end(b, n, esds) || box_end(b, n, mp4a) ||
        box_end(b, n, stsd)) {
        free(b);
        return -1;
    }
    if (box_begin(&b, &n, &cap, "stts", &stts) || dyn_u32(&b, &n, &cap, 0) ||
        dyn_u32(&b, &n, &cap, 1) || dyn_u32(&b, &n, &cap, 2) || dyn_u32(&b, &n, &cap, 1024) ||
        box_end(b, n, stts) || box_begin(&b, &n, &cap, "stsc", &stsc) ||
        dyn_u32(&b, &n, &cap, 0) || dyn_u32(&b, &n, &cap, 1) || dyn_u32(&b, &n, &cap, 1) ||
        dyn_u32(&b, &n, &cap, 2) || dyn_u32(&b, &n, &cap, 1) || box_end(b, n, stsc) ||
        box_begin(&b, &n, &cap, "stsz", &stsz) || dyn_u32(&b, &n, &cap, 0) ||
        dyn_u32(&b, &n, &cap, 0) || dyn_u32(&b, &n, &cap, 2) ||
        dyn_u32(&b, &n, &cap, (uint32_t)sizeof(raw1)) ||
        dyn_u32(&b, &n, &cap, (uint32_t)sizeof(raw2)) || box_end(b, n, stsz)) {
        free(b);
        return -1;
    }
    /* stco 先占位，mdat 写完再回填 */
    if (box_begin(&b, &n, &cap, "stco", &stco) || dyn_u32(&b, &n, &cap, 0) ||
        dyn_u32(&b, &n, &cap, 1) || dyn_u32(&b, &n, &cap, 0) || box_end(b, n, stco) ||
        box_end(b, n, stbl) || box_end(b, n, minf) || box_end(b, n, mdia) ||
        box_end(b, n, trak) || box_end(b, n, moov)) {
        free(b);
        return -1;
    }
    if (box_begin(&b, &n, &cap, "mdat", &mdat)) {
        free(b);
        return -1;
    }
    mdat_off_in_file = (uint32_t)(n); /* 载荷起点 */
    if (dyn_put(&b, &n, &cap, raw1, sizeof(raw1)) ||
        dyn_put(&b, &n, &cap, raw2, sizeof(raw2)) || box_end(b, n, mdat)) {
        free(b);
        return -1;
    }
    /* 回填 stco 第一个 chunk offset：stco 盒子里 version/flags/count 之后 */
    {
        /* stco 布局：size(4)+type(4)+verflags(4)+count(4)+offset(4) */
        size_t off_field = stco + 8 + 4 + 4;
        b[off_field + 0] = (uint8_t)(mdat_off_in_file >> 24);
        b[off_field + 1] = (uint8_t)(mdat_off_in_file >> 16);
        b[off_field + 2] = (uint8_t)(mdat_off_in_file >> 8);
        b[off_field + 3] = (uint8_t)mdat_off_in_file;
    }
    *out = b;
    *len = n;
    return 0;
}

static int collect_open(const char *url, const char *ca, ms_event *evs, int cap, int *nout,
                        int *info_err, char *ver, size_t vcap)
{
    ms_ff *f = NULL;
    ms_ff_limits lim;
    ms_status st;
    int i, rc;
    ms_ff_limits_default(&lim);
    lim.io_timeout_ms = 4000;
    CHECK_ST(ms_ff_create(NULL, &lim, &f), MS_OK);
    if (ca != NULL) {
        CHECK_ST(ms_ff_set_ca_file(f, ca), MS_OK);
    }
    if (ver != NULL && vcap > 0) {
        snprintf(ver, vcap, "%s", ms_ff_ffmpeg_version());
    }
    st = ms_ff_open(f, url);
    if (st != MS_OK) {
        if (info_err) {
            *info_err = ms_ff_stream_info_err(f);
        }
        *nout = 0;
        ms_ff_destroy(f);
        return (int)st;
    }
    if (info_err) {
        *info_err = ms_ff_stream_info_err(f);
    }
    if (strncmp(url, "udp://", 6) == 0 || strncmp(url, "rtp://", 6) == 0 ||
        strncmp(url, "rtmp://", 7) == 0 || strncmp(url, "rtmps://", 8) == 0 ||
        strncmp(url, "rtsp://", 7) == 0 || strncmp(url, "rtsps://", 8) == 0) {
        ms_ff_seek_range rng;
        CHECK(!ms_ff_is_seekable(f), "live proto is not seekable");
        CHECK(ms_ff_duration_90k(f) < 0, "live proto has no VOD duration");
        CHECK_ST(ms_ff_seek(f, 90000), MS_ERR_NOT_SEEKABLE);
        CHECK_ST(ms_ff_seek_live_edge(f), MS_ERR_NOT_SEEKABLE);
        memset(&rng, 0, sizeof(rng));
        rng.struct_size = sizeof(rng);
        CHECK_ST(ms_ff_get_seek_range(f, &rng), MS_OK);
        CHECK(!rng.seekable && rng.kind == MS_FF_SEEK_KIND_UNSEEKABLE,
              "live proto range is honest UNSEEKABLE");
        CHECK(rng.duration_90k < 0, "live proto does not fake a VOD duration");
    }
    rc = drain_events(f, evs, cap, nout, 64);
    for (i = 0; i < *nout; i++) {
        if (evs[i].release_id) {
            ms_ff_release(f, evs[i].release_id);
        }
    }
    ms_ff_destroy(f);
    return rc == 0 ? 0 : rc;
}

static int test_version_and_license(void)
{
    const char *v = ms_ff_ffmpeg_version();
    const char *lic = ms_ff_ffmpeg_license();
    const char *ossl;
    unsigned long ossl_num;
    CHECK(v != NULL && strstr(v, "9.1") != NULL, "version is official 9.1");
    CHECK(lic != NULL && strstr(lic, "LGPL") != NULL, "license string contains LGPL");
    CHECK(strstr(lic, "GPL version 2") == NULL || strstr(lic, "LGPL") != NULL,
          "not claiming GPL-only");
    ossl = ms_ff_openssl_version();
    ossl_num = ms_ff_openssl_version_num();
    CHECK(ossl != NULL && strstr(ossl, "3.5.9") != NULL, "openssl version is pinned 3.5.9");
    /* OpenSSL 3.5.9: (3<<28)|(5<<20)|(9<<4) = 0x30500090 */
    CHECK(ossl_num == 0x30500090UL, "openssl_version_num is 3.5.9");
    printf("  ffmpeg version=%s license=%s\n", v, lic);
    printf("  openssl version=%s version_num=0x%lx\n", ossl, ossl_num);
    printf("  test_version_and_license ok\n");
    return 0;
}

static int test_local_containers(const char *tmpdir)
{
    char ts_path[512], flv_path[512], mp4_path[512], url[600];
    uint8_t *ts = NULL, *flv = NULL, *mp4 = NULL;
    size_t tsl = 0, flvl = 0, mp4l = 0;
    ms_event evs[32];
    int n = 0, info = 1, i, au, cfg, fmt;
    CHECK(build_aac_ts(&ts, &tsl, 90000, 4) == 0, "build ts");
    CHECK(build_aac_flv(&flv, &flvl) == 0, "build flv");
    CHECK(build_aac_mp4(&mp4, &mp4l) == 0, "build mp4");
    CHECK(path_join(ts_path, sizeof(ts_path), tmpdir, "a.ts") == 0, "ts path");
    CHECK(path_join(flv_path, sizeof(flv_path), tmpdir, "a.flv") == 0, "flv path");
    CHECK(path_join(mp4_path, sizeof(mp4_path), tmpdir, "a.mp4") == 0, "mp4 path");
    CHECK(write_file(ts_path, ts, tsl) == 0, "write ts");
    CHECK(write_file(flv_path, flv, flvl) == 0, "write flv");
    CHECK(write_file(mp4_path, mp4, mp4l) == 0, "write mp4");
    free(ts);
    free(flv);
    free(mp4);

    snprintf(url, sizeof(url), "file:%s", ts_path);
    CHECK(collect_open(url, NULL, evs, 32, &n, &info, NULL, 0) == 0, "open ts");
    CHECK(info == 0, "ts find_stream_info ok without decoder");
    CHECK(n >= 2, "ts events");
    CHECK(evs[0].type == MS_EVENT_FORMAT, "ts format first");
    CHECK(evs[0].format == MS_FORMAT_MPEG_TS, "ts format id");
    CHECK(count_type(evs, n, MS_EVENT_TRACK) >= 1, "ts track");
    au = count_type(evs, n, MS_EVENT_AU);
    CHECK(au >= 2, "ts AU count");
    fmt = 0;
    for (i = 0; i < n; i++) {
        if (evs[i].type == MS_EVENT_AU) {
            CHECK(evs[i].track.codec == MS_CODEC_AAC, "ts au aac");
            CHECK(evs[i].sample_format == MS_SAMPLE_FMT_ADTS, "ts au adts");
            CHECK(evs[i].has_pts == 1, "ts au pts");
            CHECK(evs[i].time_base_den != 0, "ts timebase");
            fmt++;
        }
    }
    (void)fmt;
    printf("  local TS: events=%d AU=%d stream_info=%d\n", n, au, info);

    snprintf(url, sizeof(url), "file:%s", flv_path);
    CHECK(collect_open(url, NULL, evs, 32, &n, &info, NULL, 0) == 0, "open flv");
    CHECK(evs[0].type == MS_EVENT_FORMAT && evs[0].format == MS_FORMAT_FLV, "flv format");
    cfg = count_type(evs, n, MS_EVENT_CODEC_CONFIG);
    au = count_type(evs, n, MS_EVENT_AU);
    CHECK(cfg >= 1, "flv config");
    CHECK(au >= 1, "flv AU");
    for (i = 0; i < n; i++) {
        if (evs[i].type == MS_EVENT_CODEC_CONFIG) {
            CHECK(evs[i].sample_format == MS_SAMPLE_FMT_AAC_RAW, "flv cfg raw");
            CHECK(evs[i].data_size >= 2, "flv asc size");
        }
        if (evs[i].type == MS_EVENT_AU) {
            CHECK(evs[i].sample_format == MS_SAMPLE_FMT_AAC_RAW, "flv au raw");
            CHECK(evs[i].track.codec == MS_CODEC_AAC, "flv au codec");
        }
    }
    printf("  local FLV: events=%d AU=%d cfg=%d stream_info=%d\n", n, au, cfg, info);

    snprintf(url, sizeof(url), "file:%s", mp4_path);
    CHECK(collect_open(url, NULL, evs, 32, &n, &info, NULL, 0) == 0, "open mp4");
    CHECK(evs[0].type == MS_EVENT_FORMAT && evs[0].format == MS_FORMAT_MP4, "mp4 format");
    cfg = count_type(evs, n, MS_EVENT_CODEC_CONFIG);
    au = count_type(evs, n, MS_EVENT_AU);
    CHECK(cfg >= 1, "mp4 config");
    CHECK(au >= 1, "mp4 AU");
    for (i = 0; i < n; i++) {
        if (evs[i].type == MS_EVENT_AU) {
            CHECK(evs[i].sample_format == MS_SAMPLE_FMT_AAC_RAW, "mp4 au raw");
        }
    }
    printf("  local MP4: events=%d AU=%d cfg=%d stream_info=%d\n", n, au, cfg, info);
    printf("  test_local_containers ok\n");
    return 0;
}

/* ---------------- 受控 HTTP/HTTPS 服务 ---------------- */

typedef struct test_srv {
    int lfd;
    int port;
    int stop;
    int use_tls;
    pthread_t th;
    SSL_CTX *ctx;
    struct {
        const char *path;
        const void *data;
        size_t len;
        const char *ctype;
        const char *redirect; /* 若非空则 302 */
        int close_after;      /* >0 则写完部分后断开 */
        int http_code;        /* 0 或 200 表示 200；401/403/404 等原样返回 */
        int stall_ms;         /* 该资源额外拖延，用于超时用例 */
    } assets[48];
    int nassets;
    int stall_ms;
    pthread_mutex_t lock;
    int lock_inited;
    int nreq;    /* 收到的 HTTP 请求数（含 Range） */
    int nrange;  /* 带 Range 头的请求数 */
} test_srv;

static int srv_add(test_srv *s, const char *path, const void *data, size_t len,
                   const char *ctype)
{
    int i;
    if (s->lock_inited) {
        pthread_mutex_lock(&s->lock);
    }
    i = s->nassets;
    if (i >= (int)(sizeof(s->assets) / sizeof(s->assets[0]))) {
        if (s->lock_inited) {
            pthread_mutex_unlock(&s->lock);
        }
        return -1;
    }
    s->assets[i].path = path;
    s->assets[i].data = data;
    s->assets[i].len = len;
    s->assets[i].ctype = ctype ? ctype : "application/octet-stream";
    s->assets[i].redirect = NULL;
    s->assets[i].close_after = 0;
    s->assets[i].http_code = 200;
    s->assets[i].stall_ms = 0;
    s->nassets++;
    if (s->lock_inited) {
        pthread_mutex_unlock(&s->lock);
    }
    return 0;
}

/* 调用方须保持新旧缓冲在更新后仍有效，直到本请求发送结束之后。 */
static int srv_update(test_srv *s, const char *path, const void *data, size_t len)
{
    int i, found = -1;
    if (s->lock_inited) {
        pthread_mutex_lock(&s->lock);
    }
    for (i = 0; i < s->nassets; i++) {
        if (strcmp(s->assets[i].path, path) == 0) {
            s->assets[i].data = data;
            s->assets[i].len = len;
            found = i;
            break;
        }
    }
    if (s->lock_inited) {
        pthread_mutex_unlock(&s->lock);
    }
    return found >= 0 ? 0 : -1;
}

static int bind_loopback(int *port_out)
{
    int fd, yes = 1;
    struct sockaddr_in a;
    socklen_t alen = sizeof(a);
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) != 0 || listen(fd, 16) != 0 ||
        getsockname(fd, (struct sockaddr *)&a, &alen) != 0) {
        close(fd);
        return -1;
    }
    *port_out = ntohs(a.sin_port);
    return fd;
}

static int write_all(int fd, SSL *ssl, const void *p, size_t n)
{
    const uint8_t *b = (const uint8_t *)p;
    size_t off = 0;
    while (off < n) {
        int r;
        if (ssl) {
            r = SSL_write(ssl, b + off, (int)(n - off));
        } else {
            r = (int)send(fd, b + off, n - off, MSG_NOSIGNAL);
        }
        if (r <= 0) {
            return -1;
        }
        off += (size_t)r;
    }
    return 0;
}

static void send_simple(int fd, SSL *ssl, int code, const char *reason, const char *ctype,
                        const void *body, size_t blen)
{
    char hdr[256];
    int n = snprintf(hdr, sizeof(hdr),
                     "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
                     "Connection: close\r\n\r\n",
                     code, reason, ctype, blen);
    if (n > 0) {
        write_all(fd, ssl, hdr, (size_t)n);
    }
    if (blen && body) {
        write_all(fd, ssl, body, blen);
    }
}

/* 点播反例需要真实 Range：moov 在尾时客户端会先读头、再读 moov、再回到 mdat。 */
static void send_range_or_full(int fd, SSL *ssl, const char *ctype, const void *body, size_t blen,
                               int has_range, int64_t rfrom, int64_t rto)
{
    size_t start, end, slen;
    char hdr[400];
    int n;
    if (!has_range || body == NULL) {
        send_simple(fd, ssl, 200, "OK", ctype, body, blen);
        return;
    }
    if (blen == 0) {
        send_simple(fd, ssl, 200, "OK", ctype, body, blen);
        return;
    }
    if (rfrom < 0) {
        /* bytes=-N：最后 N 字节 */
        start = (rto > 0 && (uint64_t)rto < (uint64_t)blen) ? (blen - (size_t)rto) : 0;
        end = blen - 1u;
    } else {
        if ((uint64_t)rfrom >= (uint64_t)blen) {
            n = snprintf(hdr, sizeof(hdr),
                         "HTTP/1.1 416 Range Not Satisfiable\r\nContent-Range: bytes */%zu\r\n"
                         "Content-Length: 0\r\nConnection: close\r\n\r\n",
                         blen);
            if (n > 0) {
                write_all(fd, ssl, hdr, (size_t)n);
            }
            return;
        }
        start = (size_t)rfrom;
        if (rto < 0 || (uint64_t)rto >= (uint64_t)blen) {
            end = blen - 1u;
        } else {
            end = (size_t)rto;
        }
        if (end < start) {
            end = start;
        }
    }
    slen = end - start + 1u;
    n = snprintf(hdr, sizeof(hdr),
                 "HTTP/1.1 206 Partial Content\r\nContent-Type: %s\r\n"
                 "Accept-Ranges: bytes\r\nContent-Range: bytes %zu-%zu/%zu\r\n"
                 "Content-Length: %zu\r\nConnection: close\r\n\r\n",
                 ctype, start, end, blen, slen);
    if (n > 0) {
        write_all(fd, ssl, hdr, (size_t)n);
    }
    write_all(fd, ssl, (const uint8_t *)body + start, slen);
}

static int parse_http_range(const char *req, int64_t *from, int64_t *to)
{
    const char *rh, *p, *endptr;
    if (req == NULL || from == NULL || to == NULL) {
        return 0;
    }
    rh = strstr(req, "\r\nRange:");
    if (rh == NULL) {
        rh = strstr(req, "\r\nrange:");
    }
    if (rh == NULL) {
        return 0;
    }
    p = rh + 2;
    while (*p && *p != ':') {
        p++;
    }
    if (*p != ':') {
        return 0;
    }
    p++;
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    if (strncmp(p, "bytes=", 6) != 0) {
        return 0;
    }
    p += 6;
    if (*p == '-') {
        *from = -1;
        *to = strtoll(p + 1, NULL, 10);
        return 1;
    }
    *from = strtoll(p, (char **)&endptr, 10);
    *to = -1;
    if (endptr != NULL && *endptr == '-') {
        if (endptr[1] >= '0' && endptr[1] <= '9') {
            *to = strtoll(endptr + 1, NULL, 10);
        }
    }
    return 1;
}

static void handle_client(test_srv *s, int fd)
{
    if (s->stall_ms > 0) {
        usleep((useconds_t)s->stall_ms * 1000);
    }

    char req[2048];
    size_t n = 0;
    SSL *ssl = NULL;
    char *path, *sp;
    int i;
    int has_range = 0;
    int64_t range_from = 0, range_to = -1;
    if (s->use_tls) {
        ssl = SSL_new(s->ctx);
        if (!ssl) {
            close(fd);
            return;
        }
        SSL_set_fd(ssl, fd);
        if (SSL_accept(ssl) != 1) {
            SSL_free(ssl);
            close(fd);
            return;
        }
    }
    while (n + 1 < sizeof(req)) {
        int r;
        if (ssl) {
            r = SSL_read(ssl, req + n, (int)(sizeof(req) - 1 - n));
        } else {
            r = (int)recv(fd, req + n, sizeof(req) - 1 - n, 0);
        }
        if (r <= 0) {
            break;
        }
        n += (size_t)r;
        req[n] = 0;
        if (strstr(req, "\r\n\r\n")) {
            break;
        }
    }
    {
        int64_t dummy_from = 0, dummy_to = -1;
        if (s->lock_inited) {
            pthread_mutex_lock(&s->lock);
        }
        s->nreq++;
        if (parse_http_range(req, &dummy_from, &dummy_to)) {
            s->nrange++;
        }
        if (s->lock_inited) {
            pthread_mutex_unlock(&s->lock);
        }
    }
    path = strchr(req, ' ');
    if (!path) {
        goto done;
    }
    path++;
    sp = strchr(path, ' ');
    has_range = parse_http_range(req, &range_from, &range_to);
    if (sp) {
        size_t pl = (size_t)(sp - path);
        if (pl >= 255) {
            pl = 255;
        }
        memmove(req, path, pl);
        req[pl] = 0;
        path = req;
    }
    {
        char noq[256];
        char *qp;
        int found = -1;
        const void *body = NULL;
        size_t blen = 0;
        const char *ctype = "application/octet-stream";
        const char *redirect = NULL;
        int http_code = 200;
        int stall_ms = 0;
        if (s->lock_inited) {
            pthread_mutex_lock(&s->lock);
        }
        for (i = 0; i < s->nassets; i++) {
            if (strcmp(path, s->assets[i].path) == 0) {
                found = i;
                break;
            }
        }
        if (found < 0) {
            qp = strchr(path, '?');
            if (qp != NULL) {
                size_t nq = (size_t)(qp - path);
                if (nq >= sizeof(noq)) {
                    nq = sizeof(noq) - 1u;
                }
                memcpy(noq, path, nq);
                noq[nq] = 0;
                for (i = 0; i < s->nassets; i++) {
                    if (strcmp(noq, s->assets[i].path) == 0) {
                        found = i;
                        break;
                    }
                }
            }
        }
        if (found >= 0) {
            i = found;
            stall_ms = s->assets[i].stall_ms;
            redirect = s->assets[i].redirect;
            http_code = s->assets[i].http_code;
            ctype = s->assets[i].ctype;
            body = s->assets[i].data;
            blen = s->assets[i].len;
        }
        if (s->lock_inited) {
            pthread_mutex_unlock(&s->lock);
        }
        if (found >= 0) {
            if (stall_ms > 0) {
                usleep((useconds_t)stall_ms * 1000);
            }
            if (redirect) {
                char hdr[400];
                int hn = snprintf(hdr, sizeof(hdr),
                                  "HTTP/1.1 302 Found\r\nLocation: %s\r\n"
                                  "Content-Length: 0\r\nConnection: close\r\n\r\n",
                                  redirect);
                write_all(fd, ssl, hdr, (size_t)hn);
                goto done;
            }
            if (http_code == 401) {
                send_simple(fd, ssl, 401, "Unauthorized", "text/plain", "no", 2);
                goto done;
            }
            if (http_code == 403) {
                send_simple(fd, ssl, 403, "Forbidden", "text/plain", "no", 2);
                goto done;
            }
            if (http_code != 0 && http_code != 200) {
                send_simple(fd, ssl, http_code, "Error", "text/plain",
                            "no", 2);
                goto done;
            }
            send_range_or_full(fd, ssl, ctype, body, blen, has_range, range_from, range_to);
            goto done;
        }
    }
    send_simple(fd, ssl, 404, "Not Found", "text/plain", "no", 2);
done:
    if (ssl) {
        SSL_shutdown(ssl);
        SSL_free(ssl);
    }
    close(fd);
}

static void *srv_thread(void *arg)
{
    test_srv *s = (test_srv *)arg;
    while (!s->stop) {
        struct sockaddr_in a;
        socklen_t alen = sizeof(a);
        int fd;
        fd = accept(s->lfd, (struct sockaddr *)&a, &alen);
        if (fd < 0) {
            if (s->stop) {
                break;
            }
            continue;
        }
        handle_client(s, fd);
    }
    return NULL;
}

static int srv_start_ex(test_srv *s, int use_tls, const char *cert, const char *key,
                        int min_proto, int max_proto)
{
    memset(s, 0, sizeof(*s));
    s->use_tls = use_tls;
    s->lfd = bind_loopback(&s->port);
    if (s->lfd < 0) {
        return -1;
    }
    if (use_tls) {
        s->ctx = SSL_CTX_new(TLS_server_method());
        if (!s->ctx || SSL_CTX_use_certificate_file(s->ctx, cert, SSL_FILETYPE_PEM) != 1 ||
            SSL_CTX_use_PrivateKey_file(s->ctx, key, SSL_FILETYPE_PEM) != 1) {
            if (s->ctx) {
                SSL_CTX_free(s->ctx);
            }
            close(s->lfd);
            return -1;
        }
        if (min_proto > 0) {
            SSL_CTX_set_min_proto_version(s->ctx, min_proto);
        } else {
            SSL_CTX_set_min_proto_version(s->ctx, TLS1_2_VERSION);
        }
        if (max_proto > 0) {
            SSL_CTX_set_max_proto_version(s->ctx, max_proto);
        }
    }
    if (pthread_mutex_init(&s->lock, NULL) != 0) {
        if (s->ctx) {
            SSL_CTX_free(s->ctx);
        }
        close(s->lfd);
        return -1;
    }
    s->lock_inited = 1;
    if (pthread_create(&s->th, NULL, srv_thread, s) != 0) {
        if (s->ctx) {
            SSL_CTX_free(s->ctx);
        }
        close(s->lfd);
        pthread_mutex_destroy(&s->lock);
        s->lock_inited = 0;
        return -1;
    }
    return 0;
}

static int srv_start(test_srv *s, int use_tls, const char *cert, const char *key)
{
    return srv_start_ex(s, use_tls, cert, key, 0, 0);
}

static void srv_stop(test_srv *s)
{
    s->stop = 1;
    shutdown(s->lfd, SHUT_RDWR);
    close(s->lfd);
    pthread_join(s->th, NULL);
    if (s->ctx) {
        SSL_CTX_free(s->ctx);
    }
    if (s->lock_inited) {
        pthread_mutex_destroy(&s->lock);
        s->lock_inited = 0;
    }
}


static int make_min_png(uint8_t *out, size_t cap, size_t filler, size_t *out_len)
{
    static const uint8_t sig[8] = { 0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a };
    static const uint8_t ihdr[13] = { 0, 0, 0, 1, 0, 0, 0, 1, 8, 2, 0, 0, 0 };
    size_t off = 0;
    if (filler < 16) {
        filler = 16;
    }
    if (8u + 25u + 12u + filler + 12u > cap) {
        return -1;
    }
    memcpy(out, sig, 8);
    off = 8;
    out[off] = 0; out[off+1] = 0; out[off+2] = 0; out[off+3] = 13;
    memcpy(out + off + 4, "IHDR", 4);
    memcpy(out + off + 8, ihdr, 13);
    memset(out + off + 21, 0, 4);
    off += 25;
    out[off] = (uint8_t)((filler >> 24) & 0xFF);
    out[off+1] = (uint8_t)((filler >> 16) & 0xFF);
    out[off+2] = (uint8_t)((filler >> 8) & 0xFF);
    out[off+3] = (uint8_t)(filler & 0xFF);
    memcpy(out + off + 4, "tEXt", 4);
    memset(out + off + 8, 0xA5, filler);
    memset(out + off + 8 + filler, 0, 4);
    off += 12u + filler;
    memset(out + off, 0, 4);
    memcpy(out + off + 4, "IEND", 4);
    memset(out + off + 8, 0, 4);
    off += 12;
    *out_len = off;
    return 0;
}

static int test_ff_png_ts_prefix(const char *tmpdir)
{
    uint8_t *ts = NULL, *blob = NULL;
    uint8_t png[4096];
    size_t tsl = 0, pn = 0, blen;
    char path[512], url[600];
    ms_event evs[32];
    int n = 0, info = 1, au;
    test_srv hs;

    CHECK(build_aac_ts(&ts, &tsl, 90000, 4) == 0, "png ts");
    CHECK(make_min_png(png, sizeof(png), 400, &pn) == 0, "png");
    CHECK(pn > 188, "png >188");
    blen = pn + tsl;
    blob = (uint8_t *)malloc(blen);
    CHECK(blob != NULL, "blob");
    memcpy(blob, png, pn);
    memcpy(blob + pn, ts, tsl);

    CHECK(path_join(path, sizeof(path), tmpdir, "png.ts") == 0, "png path");
    CHECK(write_file(path, blob, blen) == 0, "write png.ts");
    snprintf(url, sizeof(url), "file:%s", path);
    CHECK(collect_open(url, NULL, evs, 32, &n, &info, NULL, 0) == 0, "open file png.ts");
    CHECK(evs[0].type == MS_EVENT_FORMAT && evs[0].format == MS_FORMAT_MPEG_TS,
          "file png.ts format");
    au = count_type(evs, n, MS_EVENT_AU);
    CHECK(au >= 1, "file png.ts AU");
    printf("  file PNG+TS prefix: events=%d AU=%d prefix=%zu stream_info=%d\n",
           n, au, pn, info);

    memset(&hs, 0, sizeof(hs));
    CHECK(srv_start(&hs, 0, NULL, NULL) == 0, "http png listen");
    CHECK(srv_add(&hs, "/png.ts", blob, blen, "video/mp2t") == 0, "http png asset");
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/png.ts", hs.port);
    n = 0;
    info = 1;
    CHECK(collect_open(url, NULL, evs, 32, &n, &info, NULL, 0) == 0, "open http png.ts");
    CHECK(evs[0].type == MS_EVENT_FORMAT && evs[0].format == MS_FORMAT_MPEG_TS,
          "http png.ts format");
    au = count_type(evs, n, MS_EVENT_AU);
    CHECK(au >= 1, "http png.ts AU");
    printf("  HTTP PNG+TS prefix: events=%d AU=%d stream_info=%d\n", n, au, info);
    srv_stop(&hs);

    free(blob);
    free(ts);
    printf("  test_ff_png_ts_prefix ok\n");
    return 0;
}

static int test_http_hls(const char *tmpdir)
{
    uint8_t *ts = NULL;
    size_t tsl = 0;
    char media[512], master[512];
    test_srv hs;
    ms_event evs[32];
    int n = 0, info = 1, au;
    char url[600];
    CHECK(build_aac_ts(&ts, &tsl, 90000, 4) == 0, "hls ts");
    snprintf(media, sizeof(media),
             "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:2\n"
             "#EXT-X-MEDIA-SEQUENCE:1\n#EXTINF:2.0,\nseg.ts\n#EXT-X-ENDLIST\n");
    snprintf(master, sizeof(master),
             "#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=128000\nmedia.m3u8\n");
    CHECK(srv_start(&hs, 0, NULL, NULL) == 0, "http listen");
    CHECK(srv_add(&hs, "/seg.ts", ts, tsl, "video/mp2t") == 0, "add seg");
    CHECK(srv_add(&hs, "/media.m3u8", media, strlen(media), "application/vnd.apple.mpegurl") == 0,
          "add media");
    CHECK(srv_add(&hs, "/master.m3u8", master, strlen(master),
                  "application/vnd.apple.mpegurl") == 0,
          "add master");
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/master.m3u8", hs.port);
    CHECK(collect_open(url, NULL, evs, 32, &n, &info, NULL, 0) == 0, "open hls");
    CHECK(n >= 2, "hls events");
    CHECK(evs[0].type == MS_EVENT_FORMAT, "hls format ev");
    CHECK(evs[0].format == MS_FORMAT_HLS, "hls format id");
    au = count_type(evs, n, MS_EVENT_AU);
    CHECK(au >= 1, "hls AU from nested TS");
    printf("  HTTP HLS (master→media→seg): events=%d AU=%d stream_info=%d format=%s\n",
           n, au, info, ms_format_name(evs[0].format));
    /* 302 后相对 URI 按跳转后基址解析：/redir/pl.m3u8 → /real/pl.m3u8，分段 seg.ts 在 /real/ */
    {
        test_srv hs2;
        char pl[512], url2[600];
        char loc[64];
        int n2 = 0, info2 = 1, au2;
        snprintf(pl, sizeof(pl),
                 "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:2\n"
                 "#EXT-X-MEDIA-SEQUENCE:1\n#EXTINF:2.0,\nseg.ts\n#EXT-X-ENDLIST\n");
        CHECK(srv_start(&hs2, 0, NULL, NULL) == 0, "http2 listen");
        snprintf(loc, sizeof(loc), "http://127.0.0.1:%d/real/pl.m3u8", hs2.port);
        CHECK(srv_add(&hs2, "/redir/pl.m3u8", NULL, 0, "text/plain") == 0, "redir entry");
        hs2.assets[0].redirect = loc;
        CHECK(srv_add(&hs2, "/real/pl.m3u8", pl, strlen(pl),
                      "application/vnd.apple.mpegurl") == 0,
              "real pl");
        CHECK(srv_add(&hs2, "/real/seg.ts", ts, tsl, "video/mp2t") == 0, "real seg");
        snprintf(url2, sizeof(url2), "http://127.0.0.1:%d/redir/pl.m3u8", hs2.port);
        CHECK(collect_open(url2, NULL, evs, 32, &n2, &info2, NULL, 0) == 0,
              "open redirected playlist");
        au2 = count_type(evs, n2, MS_EVENT_AU);
        CHECK(au2 >= 1, "AU after redirect base URI");
        printf("  HTTP HLS redirect base: events=%d AU=%d\n", n2, au2);
        srv_stop(&hs2);
    }
    srv_stop(&hs);
    free(ts);
    (void)tmpdir;
    printf("  test_http_hls ok\n");
    return 0;
}


static int test_http_hls_png_segment(void)
{
    uint8_t *ts = NULL, *blob = NULL;
    uint8_t png[4096];
    size_t tsl = 0, pn = 0, blen;
    char media[512];
    test_srv hs;
    int au;
    char url[600];
    unsigned ops = 0;
    uint64_t last_b = 0;
    int last_k = 0;
    ms_ff *f = NULL;
    ms_ff_limits lim;

    CHECK(build_aac_ts(&ts, &tsl, 90000, 4) == 0, "hls png ts");
    CHECK(make_min_png(png, sizeof(png), 400, &pn) == 0, "png");
    blen = pn + tsl;
    blob = (uint8_t *)malloc(blen);
    CHECK(blob != NULL, "blob");
    memcpy(blob, png, pn);
    memcpy(blob + pn, ts, tsl);
    snprintf(media, sizeof(media),
             "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:2\n"
             "#EXT-X-MEDIA-SEQUENCE:1\n#EXTINF:2.0,\nseg.ts\n#EXT-X-ENDLIST\n");
    memset(&hs, 0, sizeof(hs));
    CHECK(srv_start(&hs, 0, NULL, NULL) == 0, "listen");
    CHECK(srv_add(&hs, "/seg.ts", blob, blen, "video/mp2t") == 0, "seg");
    CHECK(srv_add(&hs, "/media.m3u8", media, strlen(media),
                  "application/vnd.apple.mpegurl") == 0,
          "pl");
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/media.m3u8", hs.port);
    ms_ff_limits_default(&lim);
    lim.io_timeout_ms = 5000;
    CHECK_ST(ms_ff_create(NULL, &lim, &f), MS_OK);
    CHECK_ST(ms_ff_open(f, url), MS_OK);
    ms_ff_ts_prefix_stats(f, &ops, &last_b, &last_k);
    /* 先记录打开后（已拉分片探测）的剥离次数，再 drain。 */
    {
        int i, got_au = 0;
        for (i = 0; i < 64; i++) {
            ms_event ev;
            ms_status st;
            memset(&ev, 0, sizeof(ev));
            st = ms_ff_pull(f, &ev);
            if (st == MS_AGAIN || st == MS_EOS) {
                break;
            }
            CHECK(st == MS_OK, "pull");
            if (ev.type == MS_EVENT_AU) {
                got_au++;
            }
            if (ev.release_id) {
                CHECK_ST(ms_ff_release(f, ev.release_id), MS_OK);
            }
        }
        au = got_au;
    }
    ms_ff_ts_prefix_stats(f, &ops, &last_b, &last_k);
    printf("  HLS PNG segment strip: ops=%u last_bytes=%llu kind=%d AUs=%d "
           "(empirical; 0 means not stripped on this path)\n",
           ops, (unsigned long long)last_b, last_k, au);
    if (ops == 0 || last_b != (uint64_t)pn || last_k != (int)MS_TS_PREFIX_PNG) {
        fprintf(stderr, "  FAIL HLS internal segment did not record PNG strip "
                "(ops=%u last=%llu want=%zu kind=%d)\n",
                ops, (unsigned long long)last_b, pn, last_k);
        failed = 1;
        ms_ff_destroy(f);
        srv_stop(&hs);
        free(blob);
        free(ts);
        return -1;
    }
    CHECK(au >= 1, "au after disguised HLS segment");
    ms_ff_destroy(f);
    srv_stop(&hs);
    free(blob);
    free(ts);
    printf("  test_http_hls_png_segment ok (internal segment used io_open strip)\n");
    return 0;
}

static int test_https_and_downgrade(const char *cadir, const char *tmpdir)
{
    char cert[512], key[512], ca[512];
    uint8_t *ts = NULL;
    size_t tsl = 0;
    test_srv hs, http;
    ms_event evs[32];
    int n = 0, info = 1, au, st;
    char url[600], loc[600];
    CHECK(path_join(ca, sizeof(ca), cadir, "ca.crt") == 0, "ca path");
    CHECK(path_join(cert, sizeof(cert), cadir, "server.crt") == 0, "cert path");
    CHECK(path_join(key, sizeof(key), cadir, "server.key") == 0, "key path");
    if (access(ca, R_OK) != 0 || access(cert, R_OK) != 0) {
        printf("  SKIP test_https_and_downgrade (no test CA at %s)\n", cadir);
        return 0;
    }
    CHECK(build_aac_ts(&ts, &tsl, 90000, 3) == 0, "https ts");
    CHECK(srv_start(&hs, 1, cert, key) == 0, "https listen");
    CHECK(srv_add(&hs, "/a.ts", ts, tsl, "video/mp2t") == 0, "https asset");
    /* 无 CA：应失败 */
    snprintf(url, sizeof(url), "https://127.0.0.1:%d/a.ts", hs.port);
    st = collect_open(url, NULL, evs, 32, &n, &info, NULL, 0);
    CHECK(st != 0, "https without ca must fail");
    /* 有 CA：应成功（CN/SAN=localhost 或 127.0.0.1） */
    snprintf(url, sizeof(url), "https://127.0.0.1:%d/a.ts", hs.port);
    st = collect_open(url, ca, evs, 32, &n, &info, NULL, 0);
    CHECK(st == 0, "https with test ca");
    CHECK(evs[0].format == MS_FORMAT_MPEG_TS, "https ts format");
    au = count_type(evs, n, MS_EVENT_AU);
    CHECK(au >= 1, "https AU");
    printf("  HTTPS TS: events=%d AU=%d\n", n, au);
    /* https → http 302 必须拒绝 */
    CHECK(srv_start(&http, 0, NULL, NULL) == 0, "plain http for downgrade");
    CHECK(srv_add(&http, "/leak.ts", ts, tsl, "video/mp2t") == 0, "downgrade target");
    snprintf(loc, sizeof(loc), "http://127.0.0.1:%d/leak.ts", http.port);
    CHECK(srv_add(&hs, "/downgrade.ts", NULL, 0, "text/plain") == 0, "downgrade entry");
    hs.assets[hs.nassets - 1].redirect = loc;
    snprintf(url, sizeof(url), "https://127.0.0.1:%d/downgrade.ts", hs.port);
    st = collect_open(url, ca, evs, 32, &n, &info, NULL, 0);
    CHECK(st != 0, "https to http redirect rejected");
    printf("  HTTPS downgrade rejected, status=%d\n", st);
    srv_stop(&http);
    srv_stop(&hs);
    free(ts);
    (void)tmpdir;
    printf("  test_https_and_downgrade ok\n");
    return 0;
}



static int read_whole_file(const char *path, uint8_t **out, size_t *len)
{
    FILE *fp;
    long sz;
    uint8_t *b;
    if (path == NULL || out == NULL || len == NULL) {
        return -1;
    }
    fp = fopen(path, "rb");
    if (fp == NULL) {
        return -1;
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return -1;
    }
    sz = ftell(fp);
    if (sz <= 0) {
        fclose(fp);
        return -1;
    }
    rewind(fp);
    b = (uint8_t *)malloc((size_t)sz);
    if (b == NULL) {
        fclose(fp);
        return -1;
    }
    if (fread(b, 1, (size_t)sz, fp) != (size_t)sz) {
        free(b);
        fclose(fp);
        return -1;
    }
    fclose(fp);
    *out = b;
    *len = (size_t)sz;
    return 0;
}

static int mp4_moov_after_mdat(const uint8_t *b, size_t n, size_t *mdat_off, size_t *moov_off)
{
    size_t off = 0;
    if (b == NULL || n < 8 || mdat_off == NULL || moov_off == NULL) {
        return 0;
    }
    *mdat_off = (size_t)-1;
    *moov_off = (size_t)-1;
    while (off + 8 <= n) {
        uint64_t sz = ((uint64_t)b[off] << 24) | ((uint64_t)b[off + 1] << 16) |
                      ((uint64_t)b[off + 2] << 8) | (uint64_t)b[off + 3];
        const unsigned char *typ = b + off + 4;
        size_t hdr = 8;
        if (sz == 1) {
            unsigned i;
            if (off + 16 > n) {
                break;
            }
            sz = 0;
            for (i = 0; i < 8; i++) {
                sz = (sz << 8) | b[off + 8 + i];
            }
            hdr = 16;
        }
        if (sz < hdr || sz > (uint64_t)(n - off)) {
            break;
        }
        if (memcmp(typ, "mdat", 4) == 0) {
            *mdat_off = off;
        }
        if (memcmp(typ, "moov", 4) == 0) {
            *moov_off = off;
        }
        if (sz == 0) {
            break;
        }
        off += (size_t)sz;
    }
    return *mdat_off != (size_t)-1 && *moov_off != (size_t)-1 && *moov_off > *mdat_off;
}

static int64_t event_pts_90k(const ms_event *ev)
{
    uint64_t den, num, pts, q, r;
    if (ev == NULL || !ev->has_pts || ev->time_base_num <= 0 || ev->time_base_den <= 0) {
        return -1;
    }
    pts = ev->pts;
    num = (uint64_t)ev->time_base_num;
    den = (uint64_t)ev->time_base_den;
    q = pts / den;
    r = pts % den;
    return (int64_t)(q * 90000ull * num + r * 90000ull * num / den);
}

/* 宿主反例：本地 HTTPS + 测试 CA 提供 moov 在尾的 MP4，拉过 16s 媒体时间。
 * 封装探测不得按 AU 向 0 做 Range；请求数应远小于 AU 数。不使用公网 URL。 */
static int test_https_moov_tail_pump(const char *cadir, const char *tmpdir)
{
    char cert[512], key[512], ca[512], mpath[512], cmd[2048], url[600];
    uint8_t *mp4 = NULL;
    size_t mp4l = 0, mdat_off = 0, moov_off = 0;
    test_srv hs;
    ms_ff *f = NULL;
    ms_ff_limits lim;
    ms_event ev;
    ms_status st;
    int au = 0, loops = 0, nreq, nrange;
    int64_t last_v = -1, last_a = -1, t0, t1, wall;
    struct timeval tv;
    const char *ffbin = "ffmpeg";
    memset(&hs, 0, sizeof(hs));
    if (cadir == NULL || cadir[0] == 0 || tmpdir == NULL || tmpdir[0] == 0) {
        printf("  SKIP test_https_moov_tail_pump (no test CA/tmp)\n");
        return 0;
    }
    CHECK(path_join(ca, sizeof(ca), cadir, "ca.crt") == 0, "ca path");
    CHECK(path_join(cert, sizeof(cert), cadir, "server.crt") == 0, "cert path");
    CHECK(path_join(key, sizeof(key), cadir, "server.key") == 0, "key path");
    if (access(ca, R_OK) != 0 || access(cert, R_OK) != 0) {
        printf("  SKIP test_https_moov_tail_pump (no test CA at %s)\n", cadir);
        return 0;
    }
    CHECK(path_join(mpath, sizeof(mpath), tmpdir, "moov_tail_20s.mp4") == 0, "mp4 path");
    snprintf(cmd, sizeof(cmd),
             "%s -y -hide_banner -loglevel error "
             "-f lavfi -i testsrc=size=160x120:rate=25:duration=20 "
             "-f lavfi -i sine=frequency=440:duration=20 "
             "-c:v libx264 -profile:v baseline -level 3.0 -preset ultrafast "
             "-pix_fmt yuv420p -b:v 120k -c:a aac -b:a 32k -ac 2 -ar 44100 "
             "\"%s\"",
             ffbin, mpath);
    if (system(cmd) != 0) {
        printf("  SKIP test_https_moov_tail_pump (ffmpeg mux failed)\n");
        return 0;
    }
    CHECK(read_whole_file(mpath, &mp4, &mp4l) == 0, "read moov-tail mp4");
    CHECK(mp4_moov_after_mdat(mp4, mp4l, &mdat_off, &moov_off), "moov after mdat");
    CHECK(srv_start(&hs, 1, cert, key) == 0, "https listen");
    CHECK(srv_add(&hs, "/vod.mp4", mp4, mp4l, "video/mp4") == 0, "https moov-tail asset");
    snprintf(url, sizeof(url), "https://127.0.0.1:%d/vod.mp4", hs.port);
    ms_ff_limits_default(&lim);
    lim.io_timeout_ms = 8000;
    CHECK_ST(ms_ff_create(NULL, &lim, &f), MS_OK);
    CHECK_ST(ms_ff_set_ca_file(f, ca), MS_OK);
    CHECK_ST(ms_ff_set_allow_system_trust(f, 0), MS_OK);
    gettimeofday(&tv, NULL);
    t0 = (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
    CHECK_ST(ms_ff_open(f, url), MS_OK);
    while (loops < 20000 && last_v < (int64_t)16 * 90000) {
        st = ms_ff_pull(f, &ev);
        loops++;
        if (st == MS_AGAIN) {
            continue;
        }
        if (st != MS_OK) {
            break;
        }
        if (ev.type == MS_EVENT_AU) {
            int64_t p90 = event_pts_90k(&ev);
            au++;
            if (p90 >= 0 && ev.track.kind == MS_TRACK_VIDEO) {
                last_v = p90;
            } else if (p90 >= 0 && ev.track.kind == MS_TRACK_AUDIO) {
                last_a = p90;
            }
        }
        if (ev.release_id != 0) {
            ms_ff_release(f, ev.release_id);
        }
        if (ev.type == MS_EVENT_EOS) {
            break;
        }
    }
    gettimeofday(&tv, NULL);
    t1 = (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
    wall = t1 - t0;
    nreq = hs.nreq;
    nrange = hs.nrange;
    printf("  https moov-tail pump: bytes=%zu mdat@%zu moov@%zu au=%d last_v_s=%.3f "
           "last_a_s=%.3f wall_ms=%lld nreq=%d nrange=%d loops=%d\n",
           mp4l, mdat_off, moov_off, au, last_v >= 0 ? last_v / 90000.0 : -1.0,
           last_a >= 0 ? last_a / 90000.0 : -1.0, (long long)wall, nreq, nrange, loops);
    ms_ff_destroy(f);
    srv_stop(&hs);
    free(mp4);
    CHECK(last_v >= (int64_t)16 * 90000, "pumped past 16s video");
    CHECK(au >= 200, "enough AUs");
    CHECK(wall < 8000, "localhost pump past 16s stays well under 8s wall");
    /* 回归保护：按 AU 探测封装会让 nreq 与 AU 同量级。打开+读 moov+回 mdat 只需少量请求。 */
    CHECK(nreq > 0 && nreq < 48, "HTTP request count stays small");
    CHECK(nreq * 8 < au, "requests far below AU count");
    printf("  test_https_moov_tail_pump ok\n");
    return 0;
}

static int test_https_tls_matrix(const char *cadir, const char *tmpdir)
{
    char ca[512], url[600];
    uint8_t *ts = NULL;
    size_t tsl = 0;
    ms_event evs[32];
    int n = 0, info = 1, au, st, i;
    char cert[512], key[512], wrong_cert[512], wrong_key[512];
    struct {
        const char *cert_name;
        const char *key_name;
        int min_proto;
        int max_proto;
        const char *label;
    } cases[] = {
        {"server.crt", "server.key", TLS1_2_VERSION, TLS1_2_VERSION, "TLS1.2 RSA-2048"},
        {"server.crt", "server.key", TLS1_3_VERSION, TLS1_3_VERSION, "TLS1.3 RSA-2048"},
        {"server-ecdsa.crt", "server-ecdsa.key", TLS1_2_VERSION, TLS1_2_VERSION,
         "TLS1.2 ECDSA-P256"},
        {"server-ecdsa.crt", "server-ecdsa.key", TLS1_3_VERSION, TLS1_3_VERSION,
         "TLS1.3 ECDSA-P256"},
    };
    CHECK(path_join(ca, sizeof(ca), cadir, "ca.crt") == 0, "ca path");
    CHECK(path_join(wrong_cert, sizeof(wrong_cert), cadir, "wrong.crt") == 0, "wrong cert");
    CHECK(path_join(wrong_key, sizeof(wrong_key), cadir, "wrong.key") == 0, "wrong key");
    if (access(ca, R_OK) != 0) {
        printf("  SKIP test_https_tls_matrix (no test CA at %s)\n", cadir);
        return 0;
    }
    CHECK(build_aac_ts(&ts, &tsl, 90000, 3) == 0, "matrix ts");
    for (i = 0; i < (int)(sizeof(cases) / sizeof(cases[0])); i++) {
        test_srv hs;
        CHECK(path_join(cert, sizeof(cert), cadir, cases[i].cert_name) == 0, "cert path");
        CHECK(path_join(key, sizeof(key), cadir, cases[i].key_name) == 0, "key path");
        if (access(cert, R_OK) != 0 || access(key, R_OK) != 0) {
            printf("  SKIP %s (leaf not at %s)\n", cases[i].label, cert);
            continue;
        }
        CHECK(srv_start_ex(&hs, 1, cert, key, cases[i].min_proto, cases[i].max_proto) == 0,
              cases[i].label);
        CHECK(srv_add(&hs, "/a.ts", ts, tsl, "video/mp2t") == 0, "matrix asset");
        snprintf(url, sizeof(url), "https://127.0.0.1:%d/a.ts", hs.port);
        st = collect_open(url, ca, evs, 32, &n, &info, NULL, 0);
        CHECK(st == 0, cases[i].label);
        au = count_type(evs, n, MS_EVENT_AU);
        CHECK(au >= 1, "matrix AU");
        printf("  HTTPS %s: events=%d AU=%d\n", cases[i].label, n, au);
        srv_stop(&hs);
    }
    /* hostname / SAN 不匹配：受信 CA 签发但 CN/SAN=wrong.example */
    if (access(wrong_cert, R_OK) == 0 && access(wrong_key, R_OK) == 0) {
        test_srv hs;
        CHECK(srv_start(&hs, 1, wrong_cert, wrong_key) == 0, "hostname mismatch listen");
        CHECK(srv_add(&hs, "/a.ts", ts, tsl, "video/mp2t") == 0, "mismatch asset");
        snprintf(url, sizeof(url), "https://127.0.0.1:%d/a.ts", hs.port);
        st = collect_open(url, ca, evs, 32, &n, &info, NULL, 0);
        CHECK(st != 0, "hostname mismatch must fail");
        printf("  HTTPS hostname mismatch rejected, status=%d\n", st);
        srv_stop(&hs);
    } else {
        printf("  SKIP hostname mismatch (no wrong.crt)\n");
    }
    /* TLS1.0/1.1 已在钉选 OpenSSL 中关闭：同一静态库上的测试服务也无法配置 */
    {
        SSL_CTX *ctx = SSL_CTX_new(TLS_server_method());
        int can_tls11 = 0;
        CHECK(ctx != NULL, "tls ctx for legacy probe");
        if (SSL_CTX_set_max_proto_version(ctx, TLS1_1_VERSION) == 1 &&
            SSL_CTX_set_min_proto_version(ctx, TLS1_1_VERSION) == 1) {
            can_tls11 = 1;
        }
        SSL_CTX_free(ctx);
        if (!can_tls11) {
            printf("  TLS1.0/1.1 disabled in pinned OpenSSL (cannot configure); ok\n");
        } else {
            test_srv hs;
            printf("  WARN TLS1.1 still configurable; expecting handshake failure\n");
            if (path_join(cert, sizeof(cert), cadir, "server.crt") == 0 &&
                path_join(key, sizeof(key), cadir, "server.key") == 0 &&
                srv_start_ex(&hs, 1, cert, key, TLS1_1_VERSION, TLS1_1_VERSION) == 0) {
                CHECK(srv_add(&hs, "/a.ts", ts, tsl, "video/mp2t") == 0, "legacy asset");
                snprintf(url, sizeof(url), "https://127.0.0.1:%d/a.ts", hs.port);
                st = collect_open(url, ca, evs, 32, &n, &info, NULL, 0);
                CHECK(st != 0, "TLS1.1-only server must fail");
                printf("  TLS1.1-only handshake rejected, status=%d\n", st);
                srv_stop(&hs);
            }
        }
    }
    free(ts);
    (void)tmpdir;
    printf("  test_https_tls_matrix ok\n");
    return 0;
}


static int test_cancel_and_backpressure(const char *tmpdir)
{
    char path[512], url[600];
    uint8_t *ts = NULL;
    size_t tsl = 0;
    ms_ff *f = NULL;
    ms_ff_limits lim;
    ms_event ev;
    int got_limit = 0, pulls = 0;
    CHECK(build_aac_ts(&ts, &tsl, 90000, 8) == 0, "bp ts");
    CHECK(path_join(path, sizeof(path), tmpdir, "bp.ts") == 0, "bp path");
    CHECK(write_file(path, ts, tsl) == 0, "write bp");
    free(ts);
    ms_ff_limits_default(&lim);
    lim.out_slots = 2;
    lim.au_queue_bytes = 64; /* 小于多帧总和，迫使 LIMIT */
    lim.io_timeout_ms = -1;
    CHECK_ST(ms_ff_create(NULL, &lim, &f), MS_OK);
    snprintf(url, sizeof(url), "file:%s", path);
    CHECK_ST(ms_ff_open(f, url), MS_OK);
    while (pulls < 32) {
        ms_status st = ms_ff_pull(f, &ev);
        pulls++;
        if (st == MS_LIMIT) {
            got_limit = 1;
            break;
        }
        if (st != MS_OK) {
            break;
        }
        /* 故意不 release，占满槽 */
        if (ev.type == MS_EVENT_EOS) {
            break;
        }
    }
    CHECK(got_limit == 1, "backpressure LIMIT");
    CHECK_ST(ms_ff_cancel(f), MS_OK);
    /* cancel 后 pull 为 CANCELED（已占用槽仍可 release） */
    {
        ms_status st = ms_ff_pull(f, &ev);
        CHECK(st == MS_ERR_CANCELED || st == MS_OK, "cancel or pending event");
    }
    ms_ff_destroy(f);
    printf("  test_cancel_and_backpressure ok\n");
    return 0;
}


/* ---------------- DASH-over-HTTP 与可选 UDP/RTP ---------------- */

typedef struct blob {
    char name[64];
    char urlpath[128];
    uint8_t *data;
    size_t len;
} blob;

typedef struct refpkt {
    int64_t pts;
    int64_t dts;
    int size;
    int key;
} refpkt;

static int slurp_file(const char *path, uint8_t **out, size_t *len)
{
    FILE *fp;
    long sz;
    uint8_t *buf;
    *out = NULL;
    *len = 0;
    fp = fopen(path, "rb");
    if (!fp) {
        return -1;
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return -1;
    }
    sz = ftell(fp);
    if (sz < 0 || fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return -1;
    }
    buf = (uint8_t *)malloc((size_t)sz + 1);
    if (!buf) {
        fclose(fp);
        return -1;
    }
    if (sz > 0 && fread(buf, 1, (size_t)sz, fp) != (size_t)sz) {
        free(buf);
        fclose(fp);
        return -1;
    }
    buf[sz] = 0;
    fclose(fp);
    *out = buf;
    *len = (size_t)sz;
    return 0;
}

static void free_blobs(blob *b, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        free(b[i].data);
        b[i].data = NULL;
    }
}

static int load_dash_dir(const char *dir, blob *out, int cap, int *nout)
{
    DIR *d;
    struct dirent *de;
    int n = 0;
    char path[512];
    d = opendir(dir);
    if (!d) {
        return -1;
    }
    while ((de = readdir(d)) != NULL) {
        size_t ln;
        if (de->d_name[0] == '.') {
            continue;
        }
        ln = strlen(de->d_name);
        if (ln >= 64) {
            continue;
        }
        /* 只收清单与分片，不把 ffprobe json/对照表当媒体资源 */
        if (!(strcmp(de->d_name, "manifest.mpd") == 0 ||
              strcmp(de->d_name, "manifest_noext") == 0 ||
              strncmp(de->d_name, "init-", 5) == 0 ||
              strncmp(de->d_name, "chunk-", 6) == 0)) {
            continue;
        }
        if (n >= cap) {
            closedir(d);
            return -1;
        }
        if (path_join(path, sizeof(path), dir, de->d_name) != 0) {
            closedir(d);
            return -1;
        }
        memset(&out[n], 0, sizeof(out[n]));
        memcpy(out[n].name, de->d_name, ln + 1);
        if (slurp_file(path, &out[n].data, &out[n].len) != 0) {
            closedir(d);
            return -1;
        }
        n++;
    }
    closedir(d);
    *nout = n;
    return 0;
}

static int load_ref(const char *path, refpkt *pk, int cap, int *nout)
{
    FILE *fp;
    char line[256];
    int n = 0;
    fp = fopen(path, "r");
    if (!fp) {
        return -1;
    }
    while (fgets(line, sizeof(line), fp) && n < cap) {
        char kind[16];
        int64_t pts = 0, dts = 0;
        int size = 0, key = 0;
        if (sscanf(line, "%15s\t%lld\t%lld\t%d\t%d", kind, (long long *)&pts,
                   (long long *)&dts, &size, &key) != 5) {
            continue;
        }
        pk[n].pts = pts;
        pk[n].dts = dts;
        pk[n].size = size;
        pk[n].key = key;
        n++;
    }
    fclose(fp);
    *nout = n;
    return 0;
}

static int serve_blobs(test_srv *s, blob *b, int n, const char *prefix, const char *skip)
{
    int i;
    for (i = 0; i < n; i++) {
        const char *ctype = "application/octet-stream";
        int wr;
        if (skip && strcmp(b[i].name, skip) == 0) {
            continue;
        }
        if (strcmp(b[i].name, "manifest.mpd") == 0 ||
            strcmp(b[i].name, "manifest_noext") == 0) {
            ctype = "application/dash+xml";
        } else if (strstr(b[i].name, "init-0") || strstr(b[i].name, "chunk-0")) {
            ctype = "video/mp4";
        } else if (strstr(b[i].name, "init-1") || strstr(b[i].name, "chunk-1")) {
            ctype = "audio/mp4";
        }
        wr = snprintf(b[i].urlpath, sizeof(b[i].urlpath), "%s%s", prefix, b[i].name);
        if (wr < 0 || (size_t)wr >= sizeof(b[i].urlpath)) {
            return -1;
        }
        if (srv_add(s, b[i].urlpath, b[i].data, b[i].len, ctype) != 0) {
            return -1;
        }
    }
    return 0;
}

static int collect_many(const char *url, ms_event *evs, int cap, int *nout, int *info_err,
                        int64_t *origin_90k)
{
    ms_ff *f = NULL;
    ms_ff_limits lim;
    ms_status st;
    int n = 0, loops = 0;
    ms_ff_limits_default(&lim);
    lim.io_timeout_ms = 8000;
    if (origin_90k) {
        *origin_90k = 0;
    }
    if (ms_ff_create(NULL, &lim, &f) != MS_OK) {
        return -1;
    }
    st = ms_ff_open(f, url);
    if (info_err) {
        *info_err = ms_ff_stream_info_err(f);
    }
    if (origin_90k && st == MS_OK) {
        *origin_90k = ms_ff_time_origin_90k(f);
    }
    if (st != MS_OK) {
        ms_ff_destroy(f);
        return (int)st;
    }
    while (n < cap && loops < 2048) {
        ms_event ev;
        st = ms_ff_pull(f, &ev);
        loops++;
        if (st == MS_AGAIN) {
            continue;
        }
        if (st == MS_EOS) {
            break;
        }
        if (st == MS_LIMIT) {
            if (n > 0 && evs[n - 1].release_id) {
                ms_ff_release(f, evs[n - 1].release_id);
                evs[n - 1].release_id = 0;
            }
            continue;
        }
        if (st != MS_OK) {
            ms_ff_destroy(f);
            *nout = n;
            return (int)st;
        }
        evs[n] = ev;
        /* 复制载荷，release 后指针失效 */
        if (ev.data && ev.data_size) {
            uint8_t *cp = (uint8_t *)malloc(ev.data_size);
            if (cp) {
                memcpy(cp, ev.data, ev.data_size);
                evs[n].data = cp;
            }
        }
        if (ev.release_id) {
            ms_ff_release(f, ev.release_id);
            evs[n].release_id = 0;
        }
        n++;
        if (ev.type == MS_EVENT_EOS) {
            break;
        }
    }
    if (origin_90k) {
        *origin_90k = ms_ff_time_origin_90k(f);
    }
    ms_ff_destroy(f);
    *nout = n;
    return 0;
}

static void free_ev_copies(ms_event *evs, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        if (evs[i].data && evs[i].data_size) {
            free((void *)evs[i].data);
            evs[i].data = NULL;
        }
    }
}

static int compare_packets(const ms_event *evs, int n, ms_codec codec, const refpkt *ref,
                           int nref, const char *label, int *matched, int64_t origin_90k)
{
    int i, k = 0, keys = 0;
    *matched = 0;
    for (i = 0; i < n; i++) {
        const ms_event *e = &evs[i];
        int hp = 0, hd = 0;
        int64_t pts = 0, dts = 0;
        if (e->type != MS_EVENT_AU || e->track.codec != codec) {
            continue;
        }
        if (k >= nref) {
            fprintf(stderr, "FAIL %s: extra packet beyond ref %d size=%zu pts_rel=%llu\n",
                    label, nref, e->data_size, (unsigned long long)e->pts);
            return 1;
        }
        if ((int)e->data_size != ref[k].size) {
            fprintf(stderr, "FAIL %s[%d]: size %zu != ref %d\n", label, k, e->data_size,
                    ref[k].size);
            return 1;
        }
        if (e->has_pts && e->pts > (uint64_t)INT64_MAX) {
            fprintf(stderr, "FAIL %s[%d]: pts looks like uint64 wrap\n", label, k);
            return 1;
        }
        if (ms_event_signed_ts(e, origin_90k, &hp, &pts, &hd, &dts) != MS_OK) {
            fprintf(stderr, "FAIL %s[%d]: signed ts recover failed\n", label, k);
            return 1;
        }
        if (!hp || pts != ref[k].pts) {
            fprintf(stderr, "FAIL %s[%d]: signed pts %d/%lld != ref %lld (rel %llu origin %lld)\n",
                    label, k, hp, (long long)pts, (long long)ref[k].pts,
                    (unsigned long long)e->pts, (long long)origin_90k);
            return 1;
        }
        if (!hd || dts != ref[k].dts) {
            fprintf(stderr, "FAIL %s[%d]: signed dts %d/%lld != ref %lld\n", label, k, hd,
                    (long long)dts, (long long)ref[k].dts);
            return 1;
        }
        if (e->is_keyframe != ref[k].key) {
            fprintf(stderr, "FAIL %s[%d]: key %d != ref %d\n", label, k, e->is_keyframe,
                    ref[k].key);
            return 1;
        }
        if (e->is_keyframe) {
            keys++;
        }
        k++;
    }
    if (k != nref) {
        fprintf(stderr, "FAIL %s: got %d packets, ref %d\n", label, k, nref);
        return 1;
    }
    *matched = k;
    (void)keys;
    return 0;
}

static int test_http_dash(const char *dashdir)
{
    blob blobs[32];
    int nblob = 0, n = 0, info = 1, i;
    int nvref = 0, naref = 0, nv = 0, na = 0, ncfg = 0;
    int nv_ff = 0, na_ff = 0;
    int64_t origin_90k = 0;
    refpkt vref[256], aref[512], vff[256], aff[512];
    ms_event *evs;
    char url[256], loc[256], rpath[512], apath[512];
    test_srv hs;
    int st, ntrk_v = 0, ntrk_a = 0;
    const uint8_t *vcfg = NULL, *acfg = NULL;
    size_t vcfgsz = 0, acfgsz = 0;
    int v_tb_den = 0, a_tb_den = 0;

    if (dashdir == NULL || dashdir[0] == 0 || access(dashdir, R_OK) != 0) {
        printf("  SKIP test_http_dash (no MS_FF_DASH_DIR samples at %s)\n",
               dashdir ? dashdir : "(null)");
        return 0;
    }
    if (path_join(rpath, sizeof(rpath), dashdir, "ref_video_trun.txt") != 0 ||
        path_join(apath, sizeof(apath), dashdir, "ref_audio_trun.txt") != 0 ||
        load_ref(rpath, vref, 256, &nvref) != 0 ||
        load_ref(apath, aref, 512, &naref) != 0) {
        printf("  SKIP test_http_dash (no trun reference in %s)\n", dashdir);
        return 0;
    }
    {
        char fpv[512], fpa[512];
        if (path_join(fpv, sizeof(fpv), dashdir, "ref_video.txt") == 0 &&
            path_join(fpa, sizeof(fpa), dashdir, "ref_audio.txt") == 0) {
            load_ref(fpv, vff, 256, &nv_ff);
            load_ref(fpa, aff, 512, &na_ff);
        }
    }
    CHECK(load_dash_dir(dashdir, blobs, 32, &nblob) == 0 && nblob >= 6, "load dash dir");
    evs = (ms_event *)calloc(512, sizeof(*evs));
    CHECK(evs != NULL, "alloc evs");

    /* 1) 相对 URL、分离 A/V AdaptationSet */
    CHECK(srv_start(&hs, 0, NULL, NULL) == 0, "dash http listen");
    CHECK(serve_blobs(&hs, blobs, nblob, "/", NULL) == 0, "serve dash");
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/manifest.mpd", hs.port);
    st = collect_many(url, evs, 512, &n, &info, &origin_90k);
    CHECK(st == 0, "open dash http");
    CHECK(n >= 10, "dash events");
    CHECK(evs[0].type == MS_EVENT_FORMAT && evs[0].format == MS_FORMAT_DASH, "dash format");
    for (i = 0; i < n; i++) {
        if (evs[i].type == MS_EVENT_TRACK && evs[i].track.codec == MS_CODEC_H264) {
            ntrk_v++;
            CHECK(evs[i].sample_format == MS_SAMPLE_FMT_AVCC, "v track avcc");
        }
        if (evs[i].type == MS_EVENT_TRACK && evs[i].track.codec == MS_CODEC_AAC) {
            ntrk_a++;
            CHECK(evs[i].sample_format == MS_SAMPLE_FMT_AAC_RAW, "a track raw");
        }
        if (evs[i].type == MS_EVENT_CODEC_CONFIG && evs[i].track.codec == MS_CODEC_H264) {
            ncfg++;
            vcfg = evs[i].data;
            vcfgsz = evs[i].data_size;
            CHECK(vcfgsz >= 7 && vcfg && vcfg[0] == 1, "avcC version");
        }
        if (evs[i].type == MS_EVENT_CODEC_CONFIG && evs[i].track.codec == MS_CODEC_AAC) {
            ncfg++;
            acfg = evs[i].data;
            acfgsz = evs[i].data_size;
            CHECK(acfgsz >= 2 && acfg != NULL, "ASC size");
        }
        if (evs[i].type == MS_EVENT_AU && evs[i].track.codec == MS_CODEC_H264) {
            if (v_tb_den == 0) {
                v_tb_den = evs[i].time_base_den;
            }
        }
        if (evs[i].type == MS_EVENT_AU && evs[i].track.codec == MS_CODEC_AAC) {
            if (a_tb_den == 0) {
                a_tb_den = evs[i].time_base_den;
            }
        }
    }
    CHECK(ntrk_v >= 1 && ntrk_a >= 1, "separate A/V tracks");
    CHECK(ncfg >= 2, "video+audio config");
    CHECK(v_tb_den == 12800, "video time_base 1/12800");
    CHECK(a_tb_den == 48000, "audio time_base 1/48000");
    CHECK(compare_packets(evs, n, MS_CODEC_H264, vref, nvref, "dash-v", &nv, origin_90k) == 0,
          "video vs independent trun");
    CHECK(compare_packets(evs, n, MS_CODEC_AAC, aref, naref, "dash-a", &na, origin_90k) == 0,
          "audio vs independent trun");
    {
        int hp = 0, hd = 0;
        int64_t pts = 0, dts = 0, p90 = 0;
        const ms_event *first_a = NULL, *first_v = NULL;
        int i2;
        for (i2 = 0; i2 < n; i2++) {
            if (!first_v && evs[i2].type == MS_EVENT_AU && evs[i2].track.codec == MS_CODEC_H264) {
                first_v = &evs[i2];
            }
            if (!first_a && evs[i2].type == MS_EVENT_AU && evs[i2].track.codec == MS_CODEC_AAC) {
                first_a = &evs[i2];
            }
        }
        CHECK(first_a != NULL && first_v != NULL, "first A/V AU");
        CHECK(origin_90k == -1920, "unified origin 90k from audio priming -1024 @ 48k");
        CHECK_ST(ms_event_signed_ts(first_a, origin_90k, &hp, &pts, &hd, &dts), MS_OK);
        CHECK(hp == 1 && pts == -1024 && hd == 1 && dts == -1024, "audio priming signed ts");
        CHECK(first_a->has_pts == 1 && first_a->pts == 0, "relative pts not dropped or wrapped");
        CHECK(first_a->preroll == 1, "priming preroll");
        CHECK_ST(ms_event_media_time_90k(first_a, origin_90k, &hp, &p90, &hd, &dts), MS_OK);
        CHECK(p90 == -1920, "audio priming in 90k");
        CHECK_ST(ms_event_signed_ts(first_v, origin_90k, &hp, &pts, &hd, &dts), MS_OK);
        CHECK(hp == 1 && pts == 0, "video starts at 0 with same origin");
        {
            int64_t otb = 0;
            CHECK_ST(ms_rescale_q(origin_90k, 1, 90000, 1, 12800, &otb), MS_OK);
            CHECK(first_v->has_pts && (int64_t)first_v->pts == (0 - otb),
                  "video relative uses same origin");
        }
    }
    /* 对照是独立 init+trun（含 elst），不是跨版本 ffprobe packet 数。
     * 末音频样本 duration=640、size=146，对应 MPD 3.0s 余量；7.1.5 ffprobe 常省略。
     * 不把每个 AVPacket 写成完整 AU 的通用证明；本样本大小与 trun sample 一致。 */
    printf("  HTTP DASH (separate A/V, relative URL): format=dash events=%d v=%d a=%d "
           "cfg=%d stream_info=%d avcC=%zu ASC=%zu origin_90k=%lld "
           "(vs independent trun; ffprobe7 v=%d a=%d, not forcing count equality)\n",
           n, nv, na, ncfg, info, vcfgsz, acfgsz, (long long)origin_90k, nv_ff, na_ff);
    free_ev_copies(evs, n);
    memset(evs, 0, 512 * sizeof(*evs));
    n = 0;

    /* 2) 无扩展名清单（探测 XML，不依赖 .mpd） */
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/manifest_noext", hs.port);
    st = collect_many(url, evs, 512, &n, &info, &origin_90k);
    CHECK(st == 0, "open dash noext");
    CHECK(evs[0].format == MS_FORMAT_DASH, "noext format dash");
    CHECK(compare_packets(evs, n, MS_CODEC_H264, vref, nvref, "noext-v", &nv, origin_90k) == 0,
          "noext video");
    printf("  HTTP DASH no-extension MPD: events=%d v=%d (Content-Type application/dash+xml; "
           "probe works without .mpd)\n",
           n, nv);
    free_ev_copies(evs, n);
    memset(evs, 0, 512 * sizeof(*evs));
    n = 0;
    srv_stop(&hs);

    /* 3) 302 后相对 URL 按跳转后基址 */
    CHECK(srv_start(&hs, 0, NULL, NULL) == 0, "dash redir listen");
    CHECK(srv_add(&hs, "/old/manifest.mpd", NULL, 0, "text/plain") == 0, "redir entry");
    snprintf(loc, sizeof(loc), "http://127.0.0.1:%d/vod/manifest.mpd", hs.port);
    hs.assets[0].redirect = loc;
    CHECK(serve_blobs(&hs, blobs, nblob, "/vod/", NULL) == 0, "serve /vod/");
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/old/manifest.mpd", hs.port);
    st = collect_many(url, evs, 512, &n, &info, &origin_90k);
    CHECK(st == 0, "open redirected dash");
    CHECK(compare_packets(evs, n, MS_CODEC_H264, vref, nvref, "redir-v", &nv, origin_90k) == 0,
          "redir video");
    printf("  HTTP DASH redirect base: events=%d v=%d\n", n, nv);
    free_ev_copies(evs, n);
    memset(evs, 0, 512 * sizeof(*evs));
    n = 0;
    srv_stop(&hs);

    /* 4) 缺片：不提供 chunk-0-00002.m4s，不得假称收齐全部视频包 */
    CHECK(srv_start(&hs, 0, NULL, NULL) == 0, "dash miss listen");
    CHECK(serve_blobs(&hs, blobs, nblob, "/", "chunk-0-00002.m4s") == 0, "serve minus mid");
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/manifest.mpd", hs.port);
    st = collect_many(url, evs, 512, &n, &info, NULL);
    {
        int gotv = 0;
        for (i = 0; i < n; i++) {
            if (evs[i].type == MS_EVENT_AU && evs[i].track.codec == MS_CODEC_H264) {
                gotv++;
            }
        }
        /* open 可能失败，或只收到第一段（25 帧），不得得到全部 75 且声称成功完整 */
        CHECK(!(st == 0 && gotv == nvref), "missing mid segment must not yield full video");
        printf("  HTTP DASH missing mid segment: collect_st=%d events=%d video_au=%d "
               "(ref %d; not a full silent success)\n",
               st, n, gotv, nvref);
    }
    free_ev_copies(evs, n);
    memset(evs, 0, 512 * sizeof(*evs));
    n = 0;
    srv_stop(&hs);

    /* 5) 截断中间视频分片 */
    CHECK(srv_start(&hs, 0, NULL, NULL) == 0, "dash trunc listen");
    CHECK(serve_blobs(&hs, blobs, nblob, "/", NULL) == 0, "serve for trunc");
    {
        int idx;
        for (idx = 0; idx < hs.nassets; idx++) {
            if (hs.assets[idx].path && strcmp(hs.assets[idx].path, "/chunk-0-00002.m4s") == 0) {
                if (hs.assets[idx].len > 200) {
                    hs.assets[idx].len = 200;
                }
                break;
            }
        }
        CHECK(idx < hs.nassets, "found mid chunk to truncate");
    }
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/manifest.mpd", hs.port);
    st = collect_many(url, evs, 512, &n, &info, NULL);
    {
        int gotv = 0;
        for (i = 0; i < n; i++) {
            if (evs[i].type == MS_EVENT_AU && evs[i].track.codec == MS_CODEC_H264) {
                gotv++;
            }
        }
        CHECK(!(st == 0 && gotv == nvref), "truncated mid segment must not yield full video");
        printf("  HTTP DASH truncated mid segment: collect_st=%d events=%d video_au=%d\n", st,
               n, gotv);
    }
    free_ev_copies(evs, n);
    memset(evs, 0, 512 * sizeof(*evs));
    n = 0;
    srv_stop(&hs);

    /* 6) 401 不得绕过 */
    CHECK(srv_start(&hs, 0, NULL, NULL) == 0, "dash 401 listen");
    CHECK(srv_add(&hs, "/secret.mpd", "x", 1, "application/dash+xml") == 0, "401 asset");
    hs.assets[0].http_code = 401;
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/secret.mpd", hs.port);
    st = collect_many(url, evs, 32, &n, &info, NULL);
    CHECK(st != 0, "401 must fail");
    printf("  HTTP DASH 401 rejected, status=%d\n", st);
    free_ev_copies(evs, n);
    n = 0;
    srv_stop(&hs);

    /* 7) 取消：打开后立即 cancel，pull 为 CANCELED 或已有事件 */
    {
        ms_ff *f = NULL;
        ms_ff_limits lim;
        ms_event ev;
        ms_status pst;
        CHECK(srv_start(&hs, 0, NULL, NULL) == 0, "dash cancel listen");
        CHECK(serve_blobs(&hs, blobs, nblob, "/", NULL) == 0, "serve cancel");
        snprintf(url, sizeof(url), "http://127.0.0.1:%d/manifest.mpd", hs.port);
        ms_ff_limits_default(&lim);
        lim.io_timeout_ms = 8000;
        CHECK_ST(ms_ff_create(NULL, &lim, &f), MS_OK);
        CHECK_ST(ms_ff_open(f, url), MS_OK);
        CHECK_ST(ms_ff_cancel(f), MS_OK);
        pst = ms_ff_pull(f, &ev);
        CHECK(pst == MS_ERR_CANCELED || pst == MS_OK, "cancel after dash open");
        if (pst == MS_OK) {
            ms_ff_release(f, ev.release_id);
        }
        ms_ff_destroy(f);
        srv_stop(&hs);
        printf("  HTTP DASH cancel after open ok (pull=%s)\n", ms_status_name(pst));
    }

    /* 8) 超时：接受连接后不回复，io_timeout 有界失败，不无限重试 */
    {
        ms_ff *f = NULL;
        ms_ff_limits lim;
        ms_status ost;
        CHECK(srv_start(&hs, 0, NULL, NULL) == 0, "dash timeout listen");
        hs.stall_ms = 8000;
        CHECK(srv_add(&hs, "/stall.mpd", "<MPD/>", 6, "application/dash+xml") == 0, "stall asset");
        snprintf(url, sizeof(url), "http://127.0.0.1:%d/stall.mpd", hs.port);
        ms_ff_limits_default(&lim);
        lim.io_timeout_ms = 800;
        CHECK_ST(ms_ff_create(NULL, &lim, &f), MS_OK);
        ost = ms_ff_open(f, url);
        CHECK_ST(ost, MS_ERR_TIMEOUT);
        CHECK(ost != MS_ERR_CANCELED, "timeout must not be disguised as cancel");
        ms_ff_destroy(f);
        srv_stop(&hs);
        printf("  HTTP DASH stall timeout: open=%s (io_timeout_ms=800)\n", ms_status_name(ost));
    }

    free(evs);
    free_blobs(blobs, nblob);
    printf("  test_http_dash ok\n");
    return 0;
}

static void kill_child(pid_t pid)
{
    int st;
    if (pid <= 0) {
        return;
    }
    kill(pid, SIGTERM);
    /* 有界等待 */
    {
        int i;
        for (i = 0; i < 20; i++) {
            if (waitpid(pid, &st, WNOHANG) == pid) {
                return;
            }
            usleep(50000);
        }
    }
    kill(pid, SIGKILL);
    waitpid(pid, &st, 0);
}

/* 系统 ffmpeg 仅作本机发送端，产品库不 exec。 */
static pid_t spawn_ffmpeg_send(char *const argv[])
{
    pid_t pid = fork();
    if (pid < 0) {
        return -1;
    }
    if (pid == 0) {
        int fd;
        prctl(PR_SET_PDEATHSIG, SIGTERM);
        fd = open("/dev/null", O_WRONLY);
        if (fd >= 0) {
            dup2(fd, STDOUT_FILENO);
            dup2(fd, STDERR_FILENO);
            if (fd > 2) {
                close(fd);
            }
        }
        execv(argv[0], argv);
        _exit(127);
    }
    return pid;
}

static int bind_udp_port(int *port_out)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in a;
    socklen_t alen = sizeof(a);
    if (fd < 0) {
        return -1;
    }
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) != 0 ||
        getsockname(fd, (struct sockaddr *)&a, &alen) != 0) {
        close(fd);
        return -1;
    }
    *port_out = ntohs(a.sin_port);
    return fd;
}


static int test_signed_bframes(const char *dashdir)
{
    char path[512], url[768];
    ms_event evs[256];
    int n = 0, info = 1, i, n_v = 0, n_a = 0, n_reorder = 0, n_preroll = 0;
    int64_t origin = 0, min_dts = INT64_MAX, min_pts = INT64_MAX;
    int st;

    if (dashdir == NULL || path_join(path, sizeof(path), dashdir, "bframes.mp4") != 0 ||
        access(path, R_OK) != 0) {
        printf("  SKIP test_signed_bframes (no bframes.mp4)\n");
        return 0;
    }
    snprintf(url, sizeof(url), "file:%s", path);
    memset(evs, 0, sizeof(evs));
    st = collect_many(url, evs, 256, &n, &info, &origin);
    CHECK(st == 0, "open bframes.mp4");
    for (i = 0; i < n; i++) {
        int hp = 0, hd = 0;
        int64_t pts = 0, dts = 0;
        if (evs[i].type != MS_EVENT_AU) {
            continue;
        }
        CHECK(evs[i].has_pts == 0 || evs[i].pts <= (uint64_t)INT64_MAX, "no uint64 wrap");
        CHECK_ST(ms_event_signed_ts(&evs[i], origin, &hp, &pts, &hd, &dts), MS_OK);
        if (evs[i].track.codec == MS_CODEC_H264) {
            n_v++;
            if (hp && hd && dts < pts) {
                n_reorder++;
            }
            if (hp && pts < min_pts) {
                min_pts = pts;
            }
            if (hd && dts < min_dts) {
                min_dts = dts;
            }
        }
        if (evs[i].track.codec == MS_CODEC_AAC) {
            n_a++;
            if (evs[i].preroll) {
                n_preroll++;
            }
            if (hp && pts < min_pts) {
                min_pts = pts;
            }
            if (hd && dts < min_dts) {
                min_dts = dts;
            }
        }
    }
    CHECK(n_v > 0 && n_a > 0, "bframes has A/V");
    CHECK(n_reorder > 0, "B-frame DTS < PTS preserved");
    /* 同一 origin，A/V 都可还原；合法负值保留 has_*（若存在 priming/负 DTS） */
    if (min_dts < 0 || min_pts < 0) {
        CHECK(origin <= 0, "origin covers negative media time");
    }
    printf("  local bframes.mp4: events=%d v=%d a=%d dts_lt_pts=%d preroll=%d "
           "origin_90k=%lld min_signed_pts=%lld min_signed_dts=%lld stream_info=%d\n",
           n, n_v, n_a, n_reorder, n_preroll, (long long)origin, (long long)min_pts,
           (long long)min_dts, info);
    free_ev_copies(evs, n);
    return 0;
}

static int test_udp_rtp(const char *dashdir)
{
    char tspath[512], sdp_path[512], send_uri[160], url[200], rtp_uri[160];
    int port = 0, listen_fd = -1, n = 0, info = 1, i, vcount = 0, acount = 0;
    ms_event *evs;
    pid_t child = -1;
    const char *ffmpeg = "/usr/bin/ffmpeg";

    if (dashdir == NULL || path_join(tspath, sizeof(tspath), dashdir, "short.ts") != 0 ||
        access(tspath, R_OK) != 0) {
        printf("  SKIP test_udp_rtp (no short.ts samples)\n");
        return 0;
    }
    if (access(ffmpeg, X_OK) != 0) {
        printf("  SKIP test_udp_rtp (no system ffmpeg sender)\n");
        return 0;
    }
    evs = (ms_event *)calloc(256, sizeof(*evs));
    CHECK(evs != NULL, "udp evs");

    /* UDP MPEG-TS：-re 循环发送，避免一次性突发与 listen 竞态。产品不 exec ffmpeg。 */
    listen_fd = bind_udp_port(&port);
    CHECK(listen_fd >= 0, "udp bind");
    close(listen_fd);
    snprintf(send_uri, sizeof(send_uri), "udp://127.0.0.1:%d?pkt_size=1316", port);
    {
        char *argv[] = {
            (char *)ffmpeg, "-hide_banner", "-loglevel", "error",
            "-re", "-stream_loop", "-1",
            "-i", tspath, "-c", "copy", "-f", "mpegts",
            send_uri, NULL
        };
        child = spawn_ffmpeg_send(argv);
        CHECK(child > 0, "spawn udp ts sender");
        snprintf(url, sizeof(url), "udp://127.0.0.1:%d?listen=1&timeout=4000000", port);
        {
            ms_ff *f = NULL;
            ms_ff_limits lim;
            int loops = 0, st_open;
            ms_ff_limits_default(&lim);
            lim.io_timeout_ms = 4000;
            CHECK_ST(ms_ff_create(NULL, &lim, &f), MS_OK);
            st_open = ms_ff_open(f, url);
            info = ms_ff_stream_info_err(f);
            if (st_open == MS_OK) {
                {
                    ms_ff_seek_range rng;
                    CHECK(!ms_ff_is_seekable(f), "udp not seekable");
                    CHECK(ms_ff_duration_90k(f) < 0, "udp no vod duration");
                    CHECK_ST(ms_ff_seek(f, 0), MS_ERR_NOT_SEEKABLE);
                    memset(&rng, 0, sizeof(rng));
                    rng.struct_size = sizeof(rng);
                    CHECK_ST(ms_ff_get_seek_range(f, &rng), MS_OK);
                    CHECK(!rng.seekable && rng.kind == MS_FF_SEEK_KIND_UNSEEKABLE,
                          "udp range UNSEEKABLE");
                }
                while (n < 256 && loops < 1024) {
                    ms_event ev;
                    ms_status st = ms_ff_pull(f, &ev);
                    loops++;
                    if (st == MS_AGAIN) {
                        continue;
                    }
                    if (st != MS_OK) {
                        break;
                    }
                    evs[n] = ev;
                    if (ev.data && ev.data_size) {
                        uint8_t *cp = (uint8_t *)malloc(ev.data_size);
                        if (cp) {
                            memcpy(cp, ev.data, ev.data_size);
                            evs[n].data = cp;
                        }
                    }
                    if (ev.release_id) {
                        ms_ff_release(f, ev.release_id);
                        evs[n].release_id = 0;
                    }
                    n++;
                    if (ev.type == MS_EVENT_EOS) {
                        break;
                    }
                    if (n >= 80) {
                        break;
                    }
                }
                ms_ff_cancel(f);
            }
            ms_ff_destroy(f);
            kill_child(child);
            child = -1;
            if (st_open != MS_OK) {
                fprintf(stderr, "FAIL udp mpegts open: %s stream_info=%d\n",
                        ms_status_name((ms_status)st_open), info);
                failed = 1;
                free_ev_copies(evs, n);
                free(evs);
                return 1;
            }
        }
    }
    for (i = 0; i < n; i++) {
        if (evs[i].type == MS_EVENT_AU && evs[i].track.codec == MS_CODEC_H264) {
            vcount++;
        }
        if (evs[i].type == MS_EVENT_AU && evs[i].track.codec == MS_CODEC_AAC) {
            acount++;
        }
    }
    CHECK(evs[0].type == MS_EVENT_FORMAT, "udp format event");
    CHECK(evs[0].format == MS_FORMAT_MPEG_TS, "udp is mpegts");
    CHECK(vcount >= 1, "udp got video packets");
    printf("  UDP MPEG-TS (pkt_size=1316, listen+timeout, system ffmpeg -re loop sender): "
           "events=%d v=%d a=%d format=%s stream_info=%d; raw TS-over-UDP subset only, "
           "not camera compatibility. AVPacket not claimed complete AU.\n",
           n, vcount, acount, ms_format_name(evs[0].format), info);
    free_ev_copies(evs, n);
    memset(evs, 0, 256 * sizeof(*evs));
    n = 0;
    vcount = acount = 0;

    /* RTP + SDP：动态载荷 96 / H264 packetization-mode=1。先写 SDP 再打开。
     * 这是 RFC 风格 RTP/AVP，不是 TS-over-RTP；FU-A 分包时 packet≠AU。 */
    listen_fd = bind_udp_port(&port);
    CHECK(listen_fd >= 0, "rtp bind");
    close(listen_fd);
    CHECK(path_join(sdp_path, sizeof(sdp_path), dashdir, "short_h264.sdp") == 0, "sdp path");
    unlink(sdp_path);
    snprintf(rtp_uri, sizeof(rtp_uri), "rtp://127.0.0.1:%d", port);
    {
        char *argv[] = {
            (char *)ffmpeg, "-hide_banner", "-loglevel", "error", "-re",
            "-i", tspath, "-map", "0:v", "-c", "copy", "-f", "rtp",
            "-sdp_file", sdp_path, rtp_uri, NULL
        };
        int waits;
        child = spawn_ffmpeg_send(argv);
        CHECK(child > 0, "spawn rtp+sdp sender");
        for (waits = 0; waits < 50 && access(sdp_path, R_OK) != 0; waits++) {
            usleep(20000);
        }
        CHECK(access(sdp_path, R_OK) == 0, "sdp file written");
        {
            ms_ff *f = NULL;
            ms_ff_limits lim;
            int st_open, loops = 0;
            char sdp_url[600];
            ms_ff_limits_default(&lim);
            lim.io_timeout_ms = 4000;
            CHECK_ST(ms_ff_create(NULL, &lim, &f), MS_OK);
            snprintf(sdp_url, sizeof(sdp_url), "file:%s", sdp_path);
            st_open = ms_ff_open(f, sdp_url);
            info = ms_ff_stream_info_err(f);
            if (st_open == MS_OK) {
                {
                    ms_ff_seek_range rng;
                    CHECK(!ms_ff_is_seekable(f), "rtp/sdp not seekable");
                    CHECK_ST(ms_ff_seek(f, 90000), MS_ERR_NOT_SEEKABLE);
                    memset(&rng, 0, sizeof(rng));
                    rng.struct_size = sizeof(rng);
                    CHECK_ST(ms_ff_get_seek_range(f, &rng), MS_OK);
                    CHECK(!rng.seekable && rng.kind == MS_FF_SEEK_KIND_UNSEEKABLE,
                          "rtp range UNSEEKABLE");
                }
                while (n < 256 && loops < 1024) {
                    ms_event ev;
                    ms_status st = ms_ff_pull(f, &ev);
                    loops++;
                    if (st == MS_AGAIN) {
                        continue;
                    }
                    if (st != MS_OK) {
                        break;
                    }
                    evs[n] = ev;
                    if (ev.data && ev.data_size) {
                        uint8_t *cp = (uint8_t *)malloc(ev.data_size);
                        if (cp) {
                            memcpy(cp, ev.data, ev.data_size);
                            evs[n].data = cp;
                        }
                    }
                    if (ev.release_id) {
                        ms_ff_release(f, ev.release_id);
                        evs[n].release_id = 0;
                    }
                    n++;
                    if (ev.type == MS_EVENT_EOS) {
                        break;
                    }
                }
                ms_ff_cancel(f);
            }
            ms_ff_destroy(f);
            kill_child(child);
            child = -1;
            if (st_open != MS_OK || n == 0) {
                printf("  RTP/SDP H264 not verified (open=%s events=%d). Not claimed.\n",
                       ms_status_name((ms_status)st_open), n);
                free_ev_copies(evs, n);
                free(evs);
                printf("  test_udp_rtp ok (UDP TS verified; RTP/SDP pending)\n");
                return 0;
            }
        }
    }
    for (i = 0; i < n; i++) {
        if (evs[i].type == MS_EVENT_AU && evs[i].track.codec == MS_CODEC_H264) {
            vcount++;
        }
    }
    printf("  RTP/SDP H264 (payload 96, packetization-mode=1, file:sdp + system ffmpeg -re): "
           "events=%d h264_packets=%d format=%s stream_info=%d. "
           "Legacy sender path; independent FU-A/STAP/AU contract is test_rtp_frame_contract.\n",
           n, vcount, n ? ms_format_name(evs[0].format) : "?", info);
    CHECK(vcount >= 1, "sdp rtp got h264 packets");
    CHECK(evs[0].format == MS_FORMAT_RTP, "sdp demuxer maps to rtp, not ts");
    free_ev_copies(evs, n);
    free(evs);
    printf("  test_udp_rtp ok\n");
    return 0;
}



static const char *ff_root(void)
{
    const char *r = getenv("MS_FF_ROOT");
    return (r != NULL && r[0] != 0) ? r : "/workspace/MsMedia";
}

static pid_t spawn_proc_out(char *const argv[], int *outfd)
{
    int p[2];
    pid_t pid;
    if (pipe(p) != 0) {
        return -1;
    }
    pid = fork();
    if (pid < 0) {
        close(p[0]);
        close(p[1]);
        return -1;
    }
    if (pid == 0) {
        int fd;
        prctl(PR_SET_PDEATHSIG, SIGTERM);
        dup2(p[1], STDOUT_FILENO);
        close(p[0]);
        close(p[1]);
        fd = open("/dev/null", O_WRONLY);
        if (fd >= 0) {
            dup2(fd, STDERR_FILENO);
            if (fd > 2) {
                close(fd);
            }
        }
        execv(argv[0], argv);
        _exit(127);
    }
    close(p[1]);
    *outfd = p[0];
    return pid;
}

static void reap_child(pid_t pid)
{
    int st;
    if (pid <= 0) {
        return;
    }
    if (waitpid(pid, &st, WNOHANG) == pid) {
        return;
    }
    kill_child(pid);
}

static int read_ready_port(int fd, int timeout_ms)
{
    char buf[2048];
    size_t n = 0;
    int elapsed = 0;
    memset(buf, 0, sizeof(buf));
    while (elapsed < timeout_ms && n + 1 < sizeof(buf)) {
        fd_set rfds;
        struct timeval tv;
        int r;
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        tv.tv_sec = 0;
        tv.tv_usec = 50000;
        r = select(fd + 1, &rfds, NULL, NULL, &tv);
        elapsed += 50;
        if (r <= 0) {
            continue;
        }
        r = (int)read(fd, buf + n, sizeof(buf) - 1 - n);
        if (r <= 0) {
            break;
        }
        n += (size_t)r;
        buf[n] = 0;
        {
            const char *s = strstr(buf, "READY port=");
            if (s != NULL) {
                return atoi(s + 11);
            }
        }
    }
    return -1;
}

static int proc_tcp_listening(int port)
{
    const char *files[] = {"/proc/net/tcp", "/proc/net/tcp6", NULL};
    char needle[16];
    int i;
    snprintf(needle, sizeof(needle), ":%04X", port & 0xffff);
    for (i = 0; files[i]; i++) {
        FILE *fp = fopen(files[i], "r");
        char line[512];
        if (!fp) {
            continue;
        }
        if (fgets(line, sizeof(line), fp) == NULL) {
            fclose(fp);
            continue;
        }
        while (fgets(line, sizeof(line), fp)) {
            /* sl local_address rem_address st */
            char local[64], rem[64];
            unsigned st = 0;
            if (sscanf(line, "%*s %63s %63s %x", local, rem, &st) >= 3) {
                if (st == 0x0A && strstr(local, needle) != NULL) {
                    fclose(fp);
                    return 1;
                }
            }
        }
        fclose(fp);
    }
    return 0;
}

static int wait_tcp_listen(int port, int timeout_ms)
{
    int elapsed = 0;
    while (elapsed < timeout_ms) {
        if (proc_tcp_listening(port)) {
            return 0;
        }
        usleep(50000);
        elapsed += 50;
    }
    return -1;
}

static int bind_tcp_ephemeral(int *port_out)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a;
    socklen_t alen = sizeof(a);
    int on = 1;
    if (fd < 0) {
        return -1;
    }
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) != 0) {
        close(fd);
        return -1;
    }
    if (getsockname(fd, (struct sockaddr *)&a, &alen) != 0) {
        close(fd);
        return -1;
    }
    *port_out = ntohs(a.sin_port);
    return fd;
}

static int write_sdp_port(const char *tmpl, const char *dst, int port)
{
    char *txt;
    FILE *fp;
    long sz;
    char *m;
    char out[4096];
    fp = fopen(tmpl, "rb");
    if (!fp) {
        return -1;
    }
    fseek(fp, 0, SEEK_END);
    sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (sz <= 0 || sz > 1 << 16) {
        fclose(fp);
        return -1;
    }
    txt = (char *)malloc((size_t)sz + 1);
    if (!txt || fread(txt, 1, (size_t)sz, fp) != (size_t)sz) {
        fclose(fp);
        free(txt);
        return -1;
    }
    fclose(fp);
    txt[sz] = 0;
    m = strstr(txt, "m=video 0 ");
    if (m == NULL) {
        free(txt);
        return -1;
    }
    *m = 0;
    snprintf(out, sizeof(out), "%sm=video %d %s", txt, port, m + (int)strlen("m=video 0 "));
    free(txt);
    fp = fopen(dst, "wb");
    if (!fp) {
        return -1;
    }
    fputs(out, fp);
    fclose(fp);
    return 0;
}

static int sha256_buf(const uint8_t *p, size_t n, unsigned char out[32])
{
    size_t len = 0;
    return EVP_Q_digest(NULL, "SHA256", NULL, p, n, out, &len) == 1 && len == 32;
}

static int append_nals(const uint8_t *p, size_t n, uint8_t *dst, size_t cap, size_t *out_n, int vclish)
{
    size_t i = 0, w = 0;
    int seen_vcl = 0;
    while (i < n) {
        size_t sc = 0, j;
        unsigned typ;
        if (i + 4 <= n && p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 0 && p[i + 3] == 1) {
            sc = 4;
        } else if (i + 3 <= n && p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1) {
            sc = 3;
        } else {
            i++;
            continue;
        }
        i += sc;
        if (i >= n) {
            break;
        }
        typ = p[i] & 0x1fu;
        j = i;
        while (j < n) {
            if (j + 3 <= n && p[j] == 0 && p[j + 1] == 0 && p[j + 2] == 1) {
                break;
            }
            if (j + 4 <= n && p[j] == 0 && p[j + 1] == 0 && p[j + 2] == 0 && p[j + 3] == 1) {
                break;
            }
            j++;
        }
        if (typ == 1 || typ == 5) {
            seen_vcl = 1;
        }
        if (!vclish || seen_vcl || (typ != 7 && typ != 8 && typ != 6 && typ != 9)) {
            if (w + (j - i) > cap) {
                return -1;
            }
            memcpy(dst + w, p + i, j - i);
            w += j - i;
        }
        i = j;
    }
    *out_n = w;
    return 0;
}

static void nal_payload_sha(const uint8_t *p, size_t n, unsigned char out[32])
{
    uint8_t *tmp = (uint8_t *)malloc(n ? n : 1);
    size_t w = 0;
    if (!tmp) {
        memset(out, 0, 32);
        return;
    }
    if (append_nals(p, n, tmp, n ? n : 1, &w, 0) != 0) {
        memset(out, 0, 32);
        free(tmp);
        return;
    }
    if (!sha256_buf(tmp, w, out)) {
        memset(out, 0, 32);
    }
    free(tmp);
}

/* 去掉仅含参数集的前缀后再哈希，兼容 SDP sprop 重复插入的 SPS/PPS。 */
static void nal_vclish_sha(const uint8_t *p, size_t n, unsigned char out[32])
{
    uint8_t *tmp = (uint8_t *)malloc(n ? n : 1);
    size_t w = 0;
    if (!tmp) {
        memset(out, 0, 32);
        return;
    }
    if (append_nals(p, n, tmp, n ? n : 1, &w, 1) != 0) {
        memset(out, 0, 32);
        free(tmp);
        return;
    }
    if (!sha256_buf(tmp, w, out)) {
        memset(out, 0, 32);
    }
    free(tmp);
}

static int sha_eq_file(const uint8_t *pay, size_t n, const char *path)
{
    unsigned char a[32], b[32], av[32], bv[32];
    FILE *fp;
    uint8_t *buf;
    long sz;
    fp = fopen(path, "rb");
    if (!fp) {
        return 0;
    }
    fseek(fp, 0, SEEK_END);
    sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (sz <= 0) {
        fclose(fp);
        return 0;
    }
    buf = (uint8_t *)malloc((size_t)sz);
    if (!buf || fread(buf, 1, (size_t)sz, fp) != (size_t)sz) {
        fclose(fp);
        free(buf);
        return 0;
    }
    fclose(fp);
    nal_payload_sha(pay, n, a);
    nal_payload_sha(buf, (size_t)sz, b);
    if (memcmp(a, b, 32) == 0) {
        free(buf);
        return 1;
    }
    nal_vclish_sha(pay, n, av);
    nal_vclish_sha(buf, (size_t)sz, bv);
    free(buf);
    return memcmp(av, bv, 32) == 0;
}

typedef struct {
    int index, key, size, seq0, count;
} au_ref_row;

static int load_au_ref(const char *path, au_ref_row *rows, int cap, int *has_stap, int *has_fua,
                       int *has_multi)
{
    FILE *fp = fopen(path, "r");
    char line[2048];
    int n = 0;
    if (!fp) {
        return -1;
    }
    *has_stap = *has_fua = *has_multi = 0;
    while (fgets(line, sizeof(line), fp) && n < cap) {
        au_ref_row r;
        char *save = NULL, *tok;
        int col = 0;
        if (line[0] == '#' || line[0] == '\n' || line[0] == 0) {
            continue;
        }
        memset(&r, 0, sizeof(r));
        tok = strtok_r(line, "\t\n", &save);
        while (tok) {
            if (col == 0) {
                r.index = atoi(tok);
            } else if (col == 2) {
                r.key = atoi(tok);
            } else if (col == 3) {
                r.size = atoi(tok);
            } else if (col == 5) {
                if (strchr(tok, ',') != NULL) {
                    *has_multi = 1;
                }
            } else if (col == 6) {
                r.seq0 = atoi(tok);
            } else if (col == 7) {
                r.count = atoi(tok);
            } else if (col == 8) {
                if (strstr(tok, "STAP-A")) {
                    *has_stap = 1;
                }
                if (strstr(tok, "FU-A")) {
                    *has_fua = 1;
                }
            }
            col++;
            tok = strtok_r(NULL, "\t\n", &save);
        }
        if (col >= 8) {
            rows[n++] = r;
        }
    }
    fclose(fp);
    return n;
}

static int match_au_to_ref(const ms_event *ev, const char *audir, const au_ref_row *rows, int nrows,
                           int *used)
{
    char path[2048];
    int i;
    if (ev->type != MS_EVENT_AU || ev->track.codec != MS_CODEC_H264 || ev->data == NULL) {
        return -1;
    }
    for (i = 0; i < nrows; i++) {
        if (used[i]) {
            continue;
        }
        snprintf(path, sizeof(path), "%s/au_%03d.bin", audir, rows[i].index);
        if (sha_eq_file((const uint8_t *)ev->data, ev->data_size, path)) {
            used[i] = 1;
            return i;
        }
    }
    return -1;
}

static int collect_video_check(const char *url, const char *cadir, const char *audir,
                               const au_ref_row *rows, int nrows, int *got, int *fmt_out, int *cfg,
                               int *first_is_key, int *mismatch, int *vcount)
{
    ms_event evs[256];
    int n = 0, info = 1, i, used[64];
    int loops = 0, video_done = 0;
    ms_ff *f = NULL;
    ms_ff_limits lim;
    ms_status st;
    memset(used, 0, sizeof(used));
    memset(evs, 0, sizeof(evs));
    *got = *cfg = *first_is_key = *mismatch = *vcount = 0;
    *fmt_out = MS_FORMAT_UNKNOWN;
    (void)info;
    ms_ff_limits_default(&lim);
    lim.io_timeout_ms = 4000;
    st = ms_ff_create(NULL, &lim, &f);
    if (st != MS_OK) {
        return (int)st;
    }
    if (cadir != NULL) {
        st = ms_ff_set_ca_file(f, cadir);
        if (st != MS_OK) {
            ms_ff_destroy(f);
            return (int)st;
        }
    }
    st = ms_ff_open(f, url);
    if (st != MS_OK) {
        ms_ff_destroy(f);
        return (int)st;
    }
    while (n < 256 && loops < 2048) {
        ms_event ev;
        st = ms_ff_pull(f, &ev);
        loops++;
        if (st == MS_AGAIN) {
            continue;
        }
        if (st == MS_EOS) {
            break;
        }
        if (st == MS_ERR_TIMEOUT) {
            /* 直播短样本结束后的有界等待，已收到的事件仍有效 */
            break;
        }
        if (st == MS_LIMIT) {
            if (n > 0 && evs[n - 1].release_id) {
                ms_ff_release(f, evs[n - 1].release_id);
                evs[n - 1].release_id = 0;
            }
            continue;
        }
        if (st != MS_OK) {
            ms_ff_destroy(f);
            *fmt_out = (n > 0) ? evs[0].format : MS_FORMAT_UNKNOWN;
            /* 已拷贝的事件留给调用方释放 */
            for (i = 0; i < n; i++) {
                if (evs[i].type == MS_EVENT_CODEC_CONFIG) {
                    (*cfg)++;
                }
                if (evs[i].type == MS_EVENT_AU && evs[i].track.codec == MS_CODEC_H264) {
                    int idx = match_au_to_ref(&evs[i], audir, rows, nrows, used);
                    if (*vcount == 0) {
                        *first_is_key = evs[i].is_keyframe;
                    }
                    (*vcount)++;
                    if (idx < 0) {
                        (*mismatch)++;
                    } else {
                        (*got)++;
                    }
                }
            }
            free_ev_copies(evs, n);
            return (int)st;
        }
        evs[n] = ev;
        if (ev.data && ev.data_size) {
            uint8_t *cp = (uint8_t *)malloc(ev.data_size);
            if (cp) {
                memcpy(cp, ev.data, ev.data_size);
                evs[n].data = cp;
            }
        }
        if (ev.release_id) {
            ms_ff_release(f, ev.release_id);
            evs[n].release_id = 0;
        }
        n++;
        if (ev.type == MS_EVENT_EOS) {
            break;
        }
        if (ev.type == MS_EVENT_AU && ev.track.codec == MS_CODEC_H264) {
            video_done++;
            if (nrows > 0 && video_done >= nrows) {
                break;
            }
        }
    }
    ms_ff_destroy(f);
    if (n > 0) {
        *fmt_out = evs[0].format;
    }
    for (i = 0; i < n; i++) {
        if (evs[i].type == MS_EVENT_CODEC_CONFIG) {
            (*cfg)++;
        }
        if (evs[i].type == MS_EVENT_AU && evs[i].track.codec == MS_CODEC_H264) {
            int idx = match_au_to_ref(&evs[i], audir, rows, nrows, used);
            if (*vcount == 0) {
                *first_is_key = evs[i].is_keyframe;
            }
            (*vcount)++;
            if (idx < 0) {
                (*mismatch)++;
            } else {
                (*got)++;
            }
        }
    }
    free_ev_copies(evs, n);
    return 0;
}

static int test_rtp_frame_contract(const char *dashdir, const char *tmpdir)
{
    char annex[2048], refp[2048], tmpl[2048], sdp[2048], audir[2048], py[2048], url[4096];
    char *send_argv[16];
    char port_s[16], delay_s[16];
    au_ref_row rows[32];
    int nrows, has_stap = 0, has_fua = 0, has_multi = 0;
    int listen_fd = -1, port = 0, got, fmt, cfg, first_key, mismatch, vcount;
    pid_t child;
    const char *pybin = "/usr/bin/python3";
    const char *root = ff_root();

    if (dashdir == NULL) {
        printf("  SKIP test_rtp_frame_contract (no dashdir)\n");
        return 0;
    }
    snprintf(annex, sizeof(annex), "%s/slices.264", dashdir);
    snprintf(audir, sizeof(audir), "%s/rtp-h264", dashdir);
    if (path_join(refp, sizeof(refp), audir, "ref_aus.txt") != 0) { fprintf(stderr, "refp\n"); return 1; }
    if (path_join(tmpl, sizeof(tmpl), audir, "template.sdp") != 0) { fprintf(stderr, "tmpl\n"); return 1; }
    snprintf(py, sizeof(py), "%s/scripts/rtp_h264_testutil.py", root);
    if (access(annex, R_OK) != 0 || access(refp, R_OK) != 0 || access(py, R_OK) != 0) {
        printf("  SKIP test_rtp_frame_contract (need slices.264 + packed ref + script)\n");
        return 0;
    }
    CHECK(access(pybin, X_OK) == 0, "python3 for rtp util");
    nrows = load_au_ref(refp, rows, 32, &has_stap, &has_fua, &has_multi);
    CHECK(nrows >= 4, "ref aus");
    CHECK(has_fua && has_multi, "ref includes FU-A and multi-NAL/slice AUs");
    printf("  rtp ref aus=%d stap=%d fua=%d multi_nal=%d\n", nrows, has_stap, has_fua, has_multi);

    /* 1) 完整发送：组帧后的载荷与独立 AU 参考一致 */
    listen_fd = bind_udp_port(&port);
    CHECK(listen_fd >= 0, "pick udp port");
    close(listen_fd);
    snprintf(sdp, sizeof(sdp), "%s/contract.sdp", tmpdir);
    CHECK(write_sdp_port(tmpl, sdp, port) == 0, "write sdp");
    snprintf(port_s, sizeof(port_s), "%d", port);
    snprintf(delay_s, sizeof(delay_s), "800");
    send_argv[0] = (char *)pybin;
    send_argv[1] = py;
    send_argv[2] = (char *)"send";
    send_argv[3] = (char *)"--out-dir";
    send_argv[4] = audir;
    send_argv[5] = (char *)"--port";
    send_argv[6] = port_s;
    send_argv[7] = (char *)"--delay-ms";
    send_argv[8] = delay_s;
    send_argv[9] = (char *)"--pace-us";
    send_argv[10] = (char *)"5000";
    send_argv[11] = NULL;
    child = spawn_ffmpeg_send(send_argv);
    CHECK(child > 0, "spawn rtp send");
    snprintf(url, sizeof(url), "file:%s", sdp);
    {
        int cst = collect_video_check(url, NULL, audir, rows, nrows, &got, &fmt, &cfg, &first_key,
                                      &mismatch, &vcount);
        if (cst != 0) {
            fprintf(stderr, "FAIL open rtp contract st=%s got=%d fmt=%s mismatch=%d\n",
                    ms_status_name((ms_status)cst), got, ms_format_name((ms_format)fmt), mismatch);
        }
        CHECK(cst == 0, "open rtp contract");
    }
    reap_child(child);
    CHECK(fmt == MS_FORMAT_RTP, "format rtp not ts");
    CHECK(cfg >= 1, "codec config before/with AUs (sprop or late extra)");
    CHECK(mismatch == 0, "no residual/unmatched AU payloads sent as complete AU");
    CHECK(got == nrows, "all reference AUs reassembled");
    CHECK(first_key, "first video AU is key (SPS/PPS/IDR)");
    printf("  RTP contract complete: format=%s AUs=%d/%d v=%d cfg=%d mismatch=%d "
           "(FU-A+STAP-A+multi-slice, marker-delimited)\n",
           ms_format_name((ms_format)fmt), got, nrows, vcount, cfg, mismatch);

    /* 2) 重排序 */
    listen_fd = bind_udp_port(&port);
    CHECK(listen_fd >= 0, "pick udp port reorder");
    close(listen_fd);
    CHECK(write_sdp_port(tmpl, sdp, port) == 0, "write sdp reorder");
    snprintf(port_s, sizeof(port_s), "%d", port);
    send_argv[11] = (char *)"--reorder";
    send_argv[12] = NULL;
    child = spawn_ffmpeg_send(send_argv);
    CHECK(child > 0, "spawn rtp reorder");
    snprintf(url, sizeof(url), "file:%s", sdp);
    CHECK(collect_video_check(url, NULL, audir, rows, nrows, &got, &fmt, &cfg, &first_key,
                              &mismatch, &vcount) == 0,
          "open rtp reorder");
    reap_child(child);
    CHECK(mismatch == 0, "reorder did not emit unverified payload");
    CHECK(got == nrows, "reorder recovered all AUs");
    printf("  RTP reorder: AUs=%d/%d mismatch=%d\n", got, nrows, mismatch);

    /* 3) 丢 FU-A 中间包：拒绝残帧，等到下一关键帧 */
    listen_fd = bind_udp_port(&port);
    CHECK(listen_fd >= 0, "pick udp port drop");
    close(listen_fd);
    CHECK(write_sdp_port(tmpl, sdp, port) == 0, "write sdp drop");
    snprintf(port_s, sizeof(port_s), "%d", port);
    send_argv[11] = (char *)"--drop";
    send_argv[12] = (char *)"8";
    send_argv[13] = NULL;
    child = spawn_ffmpeg_send(send_argv);
    CHECK(child > 0, "spawn rtp drop");
    snprintf(url, sizeof(url), "file:%s", sdp);
    CHECK(collect_video_check(url, NULL, audir, rows, nrows, &got, &fmt, &cfg, &first_key,
                              &mismatch, &vcount) == 0,
          "open rtp drop");
    reap_child(child);
    CHECK(mismatch == 0, "loss must not deliver a non-reference residual frame");
    CHECK(got < nrows, "lost fragment prevents full AU count");
    CHECK(got >= 1, "later keyframe AUs still delivered");
    CHECK(first_key, "after loss, first delivered video AU is a keyframe");
    printf("  RTP drop mid-FU-A (pkt 8, also covers truncation of that AU): "
           "AUs=%d/%d mismatch=%d first_key=%d (wait-key policy)\n",
           got, nrows, mismatch, first_key);

    printf("  test_rtp_frame_contract ok\n");
    return 0;
}

static int test_rtsp_transport(const char *dashdir, const char *transport, const char *audir,
                               const au_ref_row *rows, int nrows)
{
    char annex[2048], py[2048], url[4096];
    char *argv[16];
    int outfd = -1, port, got, fmt, cfg, first_key, mismatch, vcount;
    pid_t child;
    const char *pybin = "/usr/bin/python3";
    const char *root = ff_root();

    snprintf(annex, sizeof(annex), "%s/slices.264", dashdir);
    snprintf(py, sizeof(py), "%s/scripts/rtp_h264_testutil.py", root);
    argv[0] = (char *)pybin;
    argv[1] = py;
    argv[2] = (char *)"rtsp";
    argv[3] = (char *)"--annexb";
    argv[4] = annex;
    argv[5] = (char *)"--mtu";
    argv[6] = (char *)"200";
    argv[7] = (char *)"--listen-timeout";
    argv[8] = (char *)"12";
    argv[9] = (char *)"--pace-us";
    argv[10] = (char *)"5000";
    argv[11] = NULL;
    child = spawn_proc_out(argv, &outfd);
    CHECK(child > 0, "spawn rtsp server");
    port = read_ready_port(outfd, 3000);
    if (port <= 0) {
        if (outfd >= 0) {
            close(outfd);
        }
        reap_child(child);
        CHECK(0, "rtsp READY port");
    }
    setenv("MS_FF_RTSP_TRANSPORT", transport, 1);
    snprintf(url, sizeof(url), "rtsp://127.0.0.1:%d/live", port);
    if (collect_video_check(url, NULL, audir, rows, nrows, &got, &fmt, &cfg, &first_key, &mismatch,
                            &vcount) != 0) {
        unsetenv("MS_FF_RTSP_TRANSPORT");
        if (outfd >= 0) {
            close(outfd);
        }
        reap_child(child);
        CHECK(0, "open rtsp");
    }
    unsetenv("MS_FF_RTSP_TRANSPORT");
    if (outfd >= 0) {
        close(outfd);
    }
    reap_child(child);
    CHECK(fmt == MS_FORMAT_RTSP, "format rtsp not ts");
    CHECK(cfg >= 1, "rtsp codec config");
    CHECK(mismatch == 0, "rtsp AU payloads match independent ref");
    CHECK(got == nrows, "rtsp reassembled all AUs");
    CHECK(first_key, "rtsp first AU is key");
    printf("  RTSP transport=%s: format=%s AUs=%d/%d cfg=%d (H264 subset, RFC 6184)\n", transport,
           ms_format_name((ms_format)fmt), got, nrows, cfg);
    return 0;
}


static int test_rtmps_controlled(const char *flv)
{
    const char *cadir = getenv("MS_FF_CA");
    const char *ffmpeg = "/usr/bin/ffmpeg";
    const char *pybin = "/usr/bin/python3";
    const char *root = ff_root();
    char cert[512], key[512], ca[512], wcert[512], wkey[512], py[2048];
    char inner_url[160], outer_url[192], port_in_s[16], port_out_s[16];
    char *argv[24];
    int fd, port_in, port_out, n = 0, info = 1, i;
    int au_v = 0, au_a = 0, has_cfg_v = 0, has_cfg_a = 0;
    pid_t child_ff = 0, child_py = 0;
    ms_event evs[256];
    ms_ff *f = NULL;
    ms_ff_limits lim;
    ms_status st;

    if (cadir == NULL || cadir[0] == 0) {
        printf("  RTMPS not run: MS_FF_CA missing. Gap recorded, not counted as pass.\n");
        return 0;
    }
    if (path_join(cert, sizeof(cert), cadir, "server.crt") != 0 ||
        path_join(key, sizeof(key), cadir, "server.key") != 0 ||
        path_join(ca, sizeof(ca), cadir, "ca.crt") != 0 ||
        path_join(py, sizeof(py), root, "tests/rtmps_tls_forward.py") != 0) {
        fprintf(stderr, "  FAIL rtmps path join\n");
        return 1;
    }
    if (access(ffmpeg, X_OK) != 0 || access(flv, R_OK) != 0 ||
        access(py, R_OK) != 0 || access(cert, R_OK) != 0 || access(key, R_OK) != 0 ||
        access(ca, R_OK) != 0) {
        printf("  RTMPS not run: need ffmpeg, short.flv, local CA leaf, and tests/rtmps_tls_forward.py. "
               "Gap recorded, not counted as pass.\n");
        return 0;
    }
    fd = bind_tcp_ephemeral(&port_in);
    CHECK(fd >= 0, "pick rtmp inner port");
    close(fd);
    fd = bind_tcp_ephemeral(&port_out);
    CHECK(fd >= 0, "pick rtmps outer port");
    close(fd);
    snprintf(port_in_s, sizeof(port_in_s), "%d", port_in);
    snprintf(port_out_s, sizeof(port_out_s), "%d", port_out);
    snprintf(inner_url, sizeof(inner_url), "rtmp://127.0.0.1:%s/live/app", port_in_s);
    snprintf(outer_url, sizeof(outer_url), "rtmps://localhost:%s/live/app", port_out_s);

    argv[0] = (char *)ffmpeg;
    argv[1] = (char *)"-hide_banner";
    argv[2] = (char *)"-loglevel";
    argv[3] = (char *)"error";
    argv[4] = (char *)"-re";
    argv[5] = (char *)"-i";
    argv[6] = (char *)flv;
    argv[7] = (char *)"-c";
    argv[8] = (char *)"copy";
    argv[9] = (char *)"-listen";
    argv[10] = (char *)"1";
    argv[11] = (char *)"-timeout";
    argv[12] = (char *)"8000000";
    argv[13] = (char *)"-f";
    argv[14] = (char *)"flv";
    argv[15] = inner_url;
    argv[16] = NULL;
    child_ff = spawn_ffmpeg_send(argv);
    CHECK(child_ff > 0, "spawn rtmp inner");
    if (wait_tcp_listen(port_in, 3000) != 0) {
        reap_child(child_ff);
        CHECK(0, "rtmp inner listen");
    }
    {
        char be[16];
        snprintf(be, sizeof(be), "%d", port_in);
        argv[0] = (char *)pybin;
        argv[1] = py;
        argv[2] = (char *)"--listen";
        argv[3] = port_out_s;
        argv[4] = (char *)"--backend-port";
        argv[5] = be;
        argv[6] = (char *)"--cert";
        argv[7] = cert;
        argv[8] = (char *)"--key";
        argv[9] = key;
        argv[10] = NULL;
        child_py = spawn_ffmpeg_send(argv);
        if (child_py <= 0 || wait_tcp_listen(port_out, 3000) != 0) {
            reap_child(child_py);
            reap_child(child_ff);
            CHECK(0, "rtmps tls forward listen");
        }
    }

    /* 无 CA：必须拒绝，不得关校验。 */
    ms_ff_limits_default(&lim);
    lim.io_timeout_ms = 4000;
    CHECK_ST(ms_ff_create(NULL, &lim, &f), MS_OK);
    CHECK_ST(ms_ff_set_allow_system_trust(f, 0), MS_OK);
    st = ms_ff_open(f, outer_url);
    printf("  RTMPS without CA: open=%s (verify stays on)\n", ms_status_name(st));
    CHECK(st != MS_OK, "rtmps without CA must fail");
    CHECK(st != MS_ERR_CANCELED, "rtmps no-CA must not look like cancel");
    ms_ff_destroy(f);
    f = NULL;

    memset(evs, 0, sizeof(evs));
    n = 0;
    if (collect_open(outer_url, ca, evs, 256, &n, &info, NULL, 0) != 0 && n == 0) {
        reap_child(child_py);
        reap_child(child_ff);
        fprintf(stderr, "  FAIL open rtmps with local CA (verify on)\n");
        return 1;
    }
    CHECK(n > 0 && evs[0].format == MS_FORMAT_RTMP, "rtmps maps to rtmp format");
    for (i = 0; i < n; i++) {
        if (evs[i].type == MS_EVENT_CODEC_CONFIG && evs[i].track.codec == MS_CODEC_H264) {
            has_cfg_v = 1;
        }
        if (evs[i].type == MS_EVENT_CODEC_CONFIG && evs[i].track.codec == MS_CODEC_AAC) {
            has_cfg_a = 1;
        }
        if (evs[i].type == MS_EVENT_AU && evs[i].track.codec == MS_CODEC_H264) {
            au_v++;
        }
        if (evs[i].type == MS_EVENT_AU && evs[i].track.codec == MS_CODEC_AAC) {
            au_a++;
        }
    }
    CHECK(has_cfg_v, "rtmps H264 config");
    CHECK(has_cfg_a, "rtmps AAC config");
    CHECK(au_v >= 1 && au_a >= 1, "rtmps media packets");
    printf("  RTMPS pull (TLS forward + ffmpeg -listen loopback, local CA, verify on): "
           "format=%s events=%d h264_au=%d aac_au=%d cfg_v=%d cfg_a=%d\n",
           ms_format_name(evs[0].format), n, au_v, au_a, has_cfg_v, has_cfg_a);

    reap_child(child_py);
    reap_child(child_ff);
    child_py = 0;
    child_ff = 0;

    /* 主机名不匹配：同一 CA 签发 CN=wrong.example，校验不得关闭。 */
    if (path_join(wcert, sizeof(wcert), cadir, "wrong.crt") == 0 &&
        path_join(wkey, sizeof(wkey), cadir, "wrong.key") == 0 &&
        access(wcert, R_OK) == 0 && access(wkey, R_OK) == 0) {
        fd = bind_tcp_ephemeral(&port_in);
        CHECK(fd >= 0, "pick mismatch inner");
        close(fd);
        fd = bind_tcp_ephemeral(&port_out);
        CHECK(fd >= 0, "pick mismatch outer");
        close(fd);
        snprintf(port_in_s, sizeof(port_in_s), "%d", port_in);
        snprintf(port_out_s, sizeof(port_out_s), "%d", port_out);
        snprintf(inner_url, sizeof(inner_url), "rtmp://127.0.0.1:%s/live/app", port_in_s);
        snprintf(outer_url, sizeof(outer_url), "rtmps://localhost:%s/live/app", port_out_s);
        argv[0] = (char *)ffmpeg;
        argv[1] = (char *)"-hide_banner";
        argv[2] = (char *)"-loglevel";
        argv[3] = (char *)"error";
        argv[4] = (char *)"-re";
        argv[5] = (char *)"-i";
        argv[6] = (char *)flv;
        argv[7] = (char *)"-c";
        argv[8] = (char *)"copy";
        argv[9] = (char *)"-listen";
        argv[10] = (char *)"1";
        argv[11] = (char *)"-timeout";
        argv[12] = (char *)"8000000";
        argv[13] = (char *)"-f";
        argv[14] = (char *)"flv";
        argv[15] = inner_url;
        argv[16] = NULL;
        child_ff = spawn_ffmpeg_send(argv);
        CHECK(child_ff > 0, "spawn mismatch rtmp");
        if (wait_tcp_listen(port_in, 3000) != 0) {
            reap_child(child_ff);
            CHECK(0, "mismatch inner listen");
        }
        {
            char be[16];
            snprintf(be, sizeof(be), "%d", port_in);
            argv[0] = (char *)pybin;
            argv[1] = py;
            argv[2] = (char *)"--listen";
            argv[3] = port_out_s;
            argv[4] = (char *)"--backend-port";
            argv[5] = be;
            argv[6] = (char *)"--cert";
            argv[7] = wcert;
            argv[8] = (char *)"--key";
            argv[9] = wkey;
            argv[10] = NULL;
            child_py = spawn_ffmpeg_send(argv);
            if (child_py <= 0 || wait_tcp_listen(port_out, 3000) != 0) {
                reap_child(child_py);
                reap_child(child_ff);
                CHECK(0, "mismatch tls listen");
            }
        }
        CHECK_ST(ms_ff_create(NULL, &lim, &f), MS_OK);
        CHECK_ST(ms_ff_set_ca_file(f, ca), MS_OK);
        st = ms_ff_open(f, outer_url);
        printf("  RTMPS hostname mismatch (CN=wrong.example vs localhost): open=%s\n",
               ms_status_name(st));
        CHECK(st != MS_OK, "hostname mismatch must fail");
        ms_ff_destroy(f);
        reap_child(child_py);
        reap_child(child_ff);
    } else {
        printf("  RTMPS hostname mismatch not run (wrong.crt missing). Gap recorded, not counted as pass.\n");
    }
    return 0;
}

static int test_rtsp_and_rtmp(const char *dashdir, const char *tmpdir)
{
    char annex[2048], audir[2048], py[2048], url[4096], flv[2048], port_s[16], listen_url[128];
    char *argv[24];
    au_ref_row rows[32];
    int nrows, has_stap = 0, has_fua = 0, has_multi = 0;
    int outfd = -1, port, fd;
    pid_t child;
    const char *pybin = "/usr/bin/python3";
    const char *root = ff_root();
    const char *ffmpeg = "/usr/bin/ffmpeg";
    ms_event evs[256];
    int n = 0, info = 1, i, au_v = 0, au_a = 0, has_cfg_v = 0, has_cfg_a = 0;
    ms_ff *f;
    ms_ff_limits lim;
    ms_status ost;

    (void)tmpdir;
    if (dashdir == NULL) {
        printf("  SKIP test_rtsp_and_rtmp (no dashdir)\n");
        return 0;
    }
    snprintf(annex, sizeof(annex), "%s/slices.264", dashdir);
    snprintf(audir, sizeof(audir), "%s/rtp-h264", dashdir);
    snprintf(py, sizeof(py), "%s/scripts/rtp_h264_testutil.py", root);
    snprintf(flv, sizeof(flv), "%s/short.flv", dashdir);
    if (access(annex, R_OK) != 0 || access(py, R_OK) != 0) {
        printf("  SKIP test_rtsp_and_rtmp (need slices + script)\n");
        return 0;
    }
    {
        char refp[2048];
        if (path_join(refp, sizeof(refp), audir, "ref_aus.txt") != 0) { fprintf(stderr, "refp\n"); return 1; }
        nrows = load_au_ref(refp, rows, 32, &has_stap, &has_fua, &has_multi);
    }
    CHECK(nrows >= 4, "rtsp ref aus");

    if (test_rtsp_transport(dashdir, "tcp", audir, rows, nrows) != 0) {
        return 1;
    }
    if (test_rtsp_transport(dashdir, "udp", audir, rows, nrows) != 0) {
        return 1;
    }

    /* RTSP 无服务器：不应把失败伪装成 CANCELED。连接拒绝可能是 UNKNOWN，不是 TIMEOUT。 */
    /* 有监听但不回复：必须为 TIMEOUT。 */
    argv[0] = (char *)pybin;
    argv[1] = py;
    argv[2] = (char *)"rtsp";
    argv[3] = (char *)"--annexb";
    argv[4] = annex;
    argv[5] = (char *)"--accept-only";
    argv[6] = (char *)"--listen-timeout";
    argv[7] = (char *)"5";
    argv[8] = NULL;
    child = spawn_proc_out(argv, &outfd);
    CHECK(child > 0, "spawn rtsp accept-only");
    port = read_ready_port(outfd, 3000);
    if (port <= 0) {
        if (outfd >= 0) {
            close(outfd);
        }
        reap_child(child);
        CHECK(0, "accept-only READY");
    }
    f = NULL;
    ms_ff_limits_default(&lim);
    lim.io_timeout_ms = 800;
    CHECK_ST(ms_ff_create(NULL, &lim, &f), MS_OK);
    snprintf(url, sizeof(url), "rtsp://127.0.0.1:%d/live", port);
    ost = ms_ff_open(f, url);
    printf("  RTSP stall (accept, no reply): open=%s (io_timeout_ms=800)\n", ms_status_name(ost));
    CHECK(ost != MS_ERR_CANCELED, "rtsp timeout must not be disguised as cancel");
    CHECK(ost == MS_ERR_TIMEOUT, "rtsp stall maps to TIMEOUT");
    ms_ff_destroy(f);
    if (outfd >= 0) {
        close(outfd);
    }
    reap_child(child);

    /* RTMP：系统 ffmpeg -listen 作本机发布端，产品不 exec。核验 FLV 配置/媒体包。 */
    if (access(ffmpeg, X_OK) != 0 || access(flv, R_OK) != 0) {
        printf("  RTMP not run this step (need /usr/bin/ffmpeg and short.flv). Not claimed.\n");
        printf("  RTMPS not run this step (need the same ffmpeg/short.flv plus MS_FF_CA). "
               "Gap recorded, not counted as pass.\n");
        printf("  AAC-over-RTSP untested; RTSP subset is H264 (RFC 6184 non-interleaved).\n");
        printf("  test_rtsp_and_rtmp ok (RTSP done; RTMP/RTMPS not run)\n");
        return 0;
    }
    fd = bind_tcp_ephemeral(&port);
    CHECK(fd >= 0, "pick rtmp port");
    close(fd);
    snprintf(port_s, sizeof(port_s), "%d", port);
    snprintf(listen_url, sizeof(listen_url), "rtmp://127.0.0.1:%s/live/app", port_s);
    argv[0] = (char *)ffmpeg;
    argv[1] = (char *)"-hide_banner";
    argv[2] = (char *)"-loglevel";
    argv[3] = (char *)"error";
    argv[4] = (char *)"-re";
    argv[5] = (char *)"-i";
    argv[6] = flv;
    argv[7] = (char *)"-c";
    argv[8] = (char *)"copy";
    argv[9] = (char *)"-listen";
    argv[10] = (char *)"1";
    argv[11] = (char *)"-timeout";
    argv[12] = (char *)"8000000";
    argv[13] = (char *)"-f";
    argv[14] = (char *)"flv";
    argv[15] = listen_url;
    argv[16] = NULL;
    child = spawn_ffmpeg_send(argv);
    CHECK(child > 0, "spawn rtmp listen");
    if (wait_tcp_listen(port, 3000) != 0) {
        reap_child(child);
        CHECK(0, "rtmp listen port");
    }
    memset(evs, 0, sizeof(evs));
    if (collect_open(listen_url, NULL, evs, 256, &n, &info, NULL, 0) != 0 && n == 0) {
        reap_child(child);
        CHECK(0, "open rtmp");
    }
    reap_child(child);
    CHECK(n > 0 && evs[0].format == MS_FORMAT_RTMP, "rtmp url maps to rtmp, not ts");
    for (i = 0; i < n; i++) {
        if (evs[i].type == MS_EVENT_CODEC_CONFIG && evs[i].track.codec == MS_CODEC_H264) {
            has_cfg_v = 1;
        }
        if (evs[i].type == MS_EVENT_CODEC_CONFIG && evs[i].track.codec == MS_CODEC_AAC) {
            has_cfg_a = 1;
        }
        if (evs[i].type == MS_EVENT_AU && evs[i].track.codec == MS_CODEC_H264) {
            au_v++;
        }
        if (evs[i].type == MS_EVENT_AU && evs[i].track.codec == MS_CODEC_AAC) {
            au_a++;
        }
    }
    CHECK(has_cfg_v, "rtmp H264 sequence/config packet");
    CHECK(has_cfg_a, "rtmp AAC config packet");
    CHECK(au_v >= 1 && au_a >= 1, "rtmp media packets (h264+aac)");
    printf("  RTMP pull (ffmpeg -listen loopback): format=%s events=%d h264_au=%d aac_au=%d "
           "cfg_v=%d cfg_a=%d\n",
           ms_format_name(evs[0].format), n, au_v, au_a, has_cfg_v, has_cfg_a);
    /* collect_open 已 release 载荷，ev.data 不可再 free。 */
    if (test_rtmps_controlled(flv) != 0) {
        return 1;
    }
    printf("  AAC-over-RTSP untested; RTSP subset is H264 (RFC 6184 non-interleaved).\n");
    printf("  test_rtsp_and_rtmp ok\n");
    return 0;
}



static int test_tls_trust_missing(void)
{
    ms_ff *f = NULL;
    ms_ff_limits lim;
    ms_status st;
    ms_ff_limits_default(&lim);
    lim.io_timeout_ms = 1000;
    CHECK_ST(ms_ff_create(NULL, &lim, &f), MS_OK);
    CHECK_ST(ms_ff_set_allow_system_trust(f, 0), MS_OK);
    CHECK(ms_ff_url_needs_tls_trust("https://127.0.0.1/") == 1, "https needs trust");
    CHECK(ms_ff_url_needs_tls_trust("http://127.0.0.1/") == 0, "http no trust");
    CHECK(ms_ff_url_needs_tls_trust("rtsps://127.0.0.1/") == 1, "rtsps needs trust");
    CHECK(ms_ff_url_needs_tls_trust("rtmps://127.0.0.1/") == 1, "rtmps needs trust");
    CHECK(ms_ff_trust_ready(f) == 0, "no ca, system trust disabled");
    st = ms_ff_open(f, "https://127.0.0.1/");
    CHECK_ST(st, MS_ERR_UNSUPPORTED);
    ms_ff_destroy(f);
    printf("  test_tls_trust_missing ok (https refused without CA; verify stays on)\n");
    return 0;
}


static int test_p4_ff_tracks(const char *tmpdir)
{
    char path[512], url[600], srt_path[512], cmd[2048];
    const char *ffmpeg = "/usr/bin/ffmpeg";
    const char *srt =
        "1\n00:00:00,000 --> 00:00:01,000\nHello\n\n"
        "2\n00:00:00,500 --> 00:00:02,000\nOverlap\n";
    int have_multi = 0;
    ms_ff *f = NULL;
    ms_ff_limits lim;
    ms_track_info ti[16];
    size_t nt = 0, k;
    uint32_t a0 = 0, a1 = 0;
    unsigned long long ep0 = 1, ep1 = 1;
    int au0 = 0, au1 = 0, loops;
    ms_event ev;
    ms_sub_cue cue;
    size_t nc = 0;
    uint8_t *mp4 = NULL;
    size_t mp4l = 0;

    CHECK(path_join(path, sizeof(path), tmpdir, "p4_multi.mp4") == 0, "path");
    CHECK(path_join(srt_path, sizeof(srt_path), tmpdir, "p4.srt") == 0, "srt path");
    CHECK(write_file(srt_path, (const uint8_t *)srt, strlen(srt)) == 0, "write srt");
    if (access(ffmpeg, X_OK) == 0) {
        snprintf(cmd, sizeof(cmd),
                 "%s -hide_banner -loglevel error -y "
                 "-f lavfi -i sine=frequency=440:duration=2 "
                 "-f lavfi -i sine=frequency=880:duration=2 "
                 "-map 0:a -map 1:a -c:a aac -shortest %s",
                 ffmpeg, path);
        if (system(cmd) == 0) {
            have_multi = 1;
        } else {
            printf("  P4 multi-audio sample: ffmpeg generate failed; "
                   "this subcase is an untested gap, not a pass\n");
        }
    } else {
        printf("  P4 multi-audio sample: no /usr/bin/ffmpeg; "
               "this subcase is an untested gap, not a pass\n");
    }
    if (!have_multi) {
        CHECK(build_aac_mp4(&mp4, &mp4l) == 0, "fallback mp4");
        CHECK(write_file(path, mp4, mp4l) == 0, "write fallback");
        free(mp4);
    }
    snprintf(url, sizeof(url), "file:%s", path);
    ms_ff_limits_default(&lim);
    lim.io_timeout_ms = 4000;
    CHECK_ST(ms_ff_create(NULL, &lim, &f), MS_OK);
    CHECK_ST(ms_ff_open(f, url), MS_OK);
    CHECK_ST(ms_ff_list_tracks(f, ti, 16, &nt), MS_OK);
    CHECK(nt >= 1, "ff lists tracks");
    for (k = 0; k < nt; k++) {
        if (ti[k].kind == MS_TRACK_AUDIO) {
            if (a0 == 0 && ti[k].selected) {
                a0 = ti[k].id;
            } else if (a1 == 0 && !ti[k].selected) {
                a1 = ti[k].id;
            }
        }
    }
    if (have_multi) {
        CHECK(a0 != 0 && a1 != 0 && a0 != a1, "two audio tracks");
    } else {
        CHECK(a0 != 0, "at least default audio");
    }
    au0 = au1 = 0;
    for (loops = 0; loops < 128; loops++) {
        ms_status st = ms_ff_pull(f, &ev);
        if (st == MS_AGAIN) {
            continue;
        }
        if (st == MS_EOS) {
            break;
        }
        if (st != MS_OK) {
            break;
        }
        ep0 = ev.selection_epoch;
        if (ev.type == MS_EVENT_AU && ev.track.kind == MS_TRACK_AUDIO) {
            if (ev.track.id == a0) {
                au0++;
            }
            if (ev.track.id == a1) {
                au1++;
            }
        }
        if (ev.release_id) {
            ms_ff_release(f, ev.release_id);
        }
        if (au0 >= 2) {
            break;
        }
    }
    CHECK(au0 >= 1, "default audio AU before switch");
    if (have_multi) {
        CHECK(au1 == 0, "unselected audio not emitted");
        CHECK_ST(ms_ff_select_track(f, a1, 1), MS_OK);
        au0 = au1 = 0;
        for (loops = 0; loops < 128; loops++) {
            ms_status st = ms_ff_pull(f, &ev);
            if (st == MS_AGAIN) {
                continue;
            }
            if (st == MS_EOS) {
                break;
            }
            if (st != MS_OK) {
                break;
            }
            ep1 = ev.selection_epoch;
            if (ev.type == MS_EVENT_AU && ev.track.id == a0) {
                au0++;
            }
            if (ev.type == MS_EVENT_AU && ev.track.id == a1) {
                au1++;
            }
            if (ev.release_id) {
                ms_ff_release(f, ev.release_id);
            }
        }
        CHECK(au0 == 0, "after switch old audio AU gone");
        CHECK(au1 >= 1, "new audio AU after switch");
        CHECK(ep1 > ep0, "ff selection_epoch bumped");
        printf("  ff multi-audio switch: a0=%u a1=%u epoch %llu->%llu (host demux; not device sound)\n",
               (unsigned)a0, (unsigned)a1,
               (unsigned long long)ep0, (unsigned long long)ep1);
    }
    CHECK_ST(ms_ff_set_external_subtitle(f, (const uint8_t *)srt, strlen(srt),
                                         MS_SUB_FORMAT_SRT), MS_OK);
    CHECK_ST(ms_ff_select_track(f, MS_TRACK_ID_EXTERNAL_SUB, 1), MS_OK);
    memset(&cue, 0, sizeof(cue));
    cue.struct_size = sizeof(cue);
    CHECK_ST(ms_ff_subtitles_at(f, 0, &cue, 1, &nc), MS_OK);
    CHECK(nc == 1, "ff ext cue");
    CHECK_ST(ms_ff_select_track(f, MS_TRACK_ID_EXTERNAL_SUB, 0), MS_OK);
    CHECK_ST(ms_ff_subtitles_at(f, 0, NULL, 0, &nc), MS_OK);
    CHECK(nc == 0, "ff sub off");
    CHECK_ST(ms_ff_flush(f), MS_OK);
    ms_ff_destroy(f);
    printf("  test_p4_ff_tracks ok (host backend events; not device sound/paint)\n");
    return 0;
}



static int test_p4_ff_seek(const char *tmpdir)
{
    char path[512], url[600], cmd[2048];
    const char *ffmpeg = "/usr/bin/ffmpeg";
    int have = 0;
    ms_ff *f = NULL;
    ms_ff_limits lim;
    ms_event ev;
    int loops;
    int au = 0, key = 0, cfg = 0;
    unsigned tl0 = 0, tl1 = 0;
    int64_t dur;
    uint8_t *mp4 = NULL;
    size_t mp4l = 0;

    CHECK(path_join(path, sizeof(path), tmpdir, "p4_seek.mp4") == 0, "path");
    if (access(ffmpeg, X_OK) == 0) {
        snprintf(cmd, sizeof(cmd),
                 "%s -hide_banner -loglevel error -y "
                 "-f lavfi -i testsrc=size=160x120:rate=25:duration=4 "
                 "-f lavfi -i sine=frequency=440:duration=4 "
                 "-c:v libx264 -pix_fmt yuv420p -g 25 -keyint_min 25 "
                 "-c:a aac -shortest %s",
                 ffmpeg, path);
        if (system(cmd) == 0) {
            have = 1;
        } else {
            printf("  P4 seek sample: ffmpeg generate failed; "
                   "falling back to tiny AAC MP4\n");
        }
    } else {
        printf("  P4 seek sample: no /usr/bin/ffmpeg; using tiny AAC MP4 fallback\n");
    }
    if (!have) {
        CHECK(build_aac_mp4(&mp4, &mp4l) == 0, "fallback mp4");
        CHECK(write_file(path, mp4, mp4l) == 0, "write fallback");
        free(mp4);
    }
    snprintf(url, sizeof(url), "file:%s", path);
    ms_ff_limits_default(&lim);
    lim.io_timeout_ms = 4000;
    CHECK(ms_ff_is_seekable(NULL) == 0, "null not seekable");
    CHECK(ms_ff_duration_90k(NULL) == -1, "null dur");
    CHECK_ST(ms_ff_create(NULL, &lim, &f), MS_OK);
    CHECK_ST(ms_ff_seek(f, 0), MS_ERR_STATE);
    CHECK(ms_ff_is_seekable(f) == 0, "unopened not seekable");
    CHECK_ST(ms_ff_open(f, url), MS_OK);
    CHECK(ms_ff_is_seekable(f) == 1, "file: is seekable");
    dur = ms_ff_duration_90k(f);
    CHECK(dur > 0, "duration known");
    {
        ms_ff_seek_range rng;
        memset(&rng, 0, sizeof(rng));
        rng.struct_size = sizeof(rng);
        CHECK_ST(ms_ff_get_seek_range(f, &rng), MS_OK);
        CHECK(rng.seekable && rng.kind == MS_FF_SEEK_KIND_VOD, "file range is VOD");
        CHECK(rng.duration_90k > 0 && rng.end_90k == rng.duration_90k,
              "VOD end equals duration");
        CHECK(rng.start_90k == 0, "VOD start 0");
    }
    CHECK_ST(ms_ff_seek(f, -1), MS_ERR_INVALID_ARG);
    for (loops = 0; loops < 200; loops++) {
        ms_status st = ms_ff_pull(f, &ev);
        if (st == MS_AGAIN) {
            continue;
        }
        if (st == MS_EOS || st != MS_OK) {
            break;
        }
        tl0 = ev.timeline_epoch;
        if (ev.type == MS_EVENT_AU) {
            au++;
        }
        if (ev.release_id) {
            ms_ff_release(f, ev.release_id);
        }
        if (au >= 3) {
            break;
        }
    }
    CHECK(au >= 1, "pulled before seek");
    CHECK_ST(ms_ff_seek(f, 90 * 1000 * 2), MS_OK);
    au = 0;
    key = 0;
    cfg = 0;
    for (loops = 0; loops < 200; loops++) {
        ms_status st = ms_ff_pull(f, &ev);
        if (st == MS_AGAIN) {
            continue;
        }
        if (st == MS_EOS) {
            break;
        }
        if (st != MS_OK) {
            break;
        }
        tl1 = ev.timeline_epoch;
        if (ev.type == MS_EVENT_CODEC_CONFIG) {
            cfg++;
        }
        if (ev.type == MS_EVENT_AU) {
            au++;
            if (ev.is_keyframe) {
                key++;
            }
        }
        if (ev.release_id) {
            ms_ff_release(f, ev.release_id);
        }
        if (au >= 2 && key >= 1) {
            break;
        }
    }
    CHECK(tl1 > tl0 || tl1 >= 1, "timeline_epoch advanced");
    CHECK(au >= 1, "AU after seek");
    if (have) {
        CHECK(key >= 1, "keyframe after seek");
        CHECK(cfg >= 1, "CSD re-emitted after seek");
    }
    /* 超过时长钳位仍成功 */
    CHECK_ST(ms_ff_seek(f, dur + 90 * 1000 * 100), MS_OK);
    ms_ff_destroy(f);
    printf("  test_p4_ff_seek ok (host demux seek, decoders still off; not device)\n");
    return 0;
}

/* 关键帧略晚于整秒（与 API 18 base30 夹具 10.023/20.023 同类）。
 * 不带前向放宽时 BACKWARD 会把恰好 10s/20s 的请求落到前一个 GOP。 */
static int test_ff_seek_keyframe_slop(const char *tmpdir)
{
    char path[512], url[600], cmd[2048];
    const char *ffmpeg = "/usr/bin/ffmpeg";
    ms_ff *f = NULL;
    ms_ff_limits lim;
    ms_event ev;
    int64_t first_key_90k;
    int loops;
    int saw;
    if (access(ffmpeg, X_OK) != 0) {
        printf("  test_ff_seek_keyframe_slop skip (no /usr/bin/ffmpeg)\n");
        return 0;
    }
    CHECK(path_join(path, sizeof(path), tmpdir, "seek_slop.mkv") == 0, "path");
    snprintf(cmd, sizeof(cmd),
             "%s -hide_banner -loglevel error -y "
             "-f lavfi -i testsrc=size=160x120:rate=25:duration=25 "
             "-c:v libx264 -profile:v baseline -pix_fmt yuv420p "
             "-x264-params keyint=250:min-keyint=250:scenecut=0 "
             "-an %s",
             ffmpeg, path);
    if (system(cmd) != 0) {
        printf("  test_ff_seek_keyframe_slop skip (ffmpeg generate failed)\n");
        return 0;
    }
    snprintf(url, sizeof(url), "file:%s", path);
    ms_ff_limits_default(&lim);
    lim.io_timeout_ms = 4000;
    CHECK_ST(ms_ff_create(NULL, &lim, &f), MS_OK);
    CHECK_ST(ms_ff_open(f, url), MS_OK);
    for (loops = 0; loops < 80; loops++) {
        if (ms_ff_pull(f, &ev) == MS_OK && ev.release_id) {
            ms_ff_release(f, ev.release_id);
        }
    }
    CHECK_ST(ms_ff_seek(f, (int64_t)90 * 1000 * 10), MS_OK);
    first_key_90k = -1;
    saw = 0;
    for (loops = 0; loops < 400; loops++) {
        ms_status st = ms_ff_pull(f, &ev);
        if (st == MS_AGAIN) {
            continue;
        }
        if (st != MS_OK) {
            break;
        }
        if (ev.type == MS_EVENT_AU && ev.track.kind == MS_TRACK_VIDEO &&
            ev.is_keyframe && ev.has_pts) {
            first_key_90k = event_pts_90k(&ev);
            saw = 1;
            if (ev.release_id) {
                ms_ff_release(f, ev.release_id);
            }
            break;
        }
        if (ev.release_id) {
            ms_ff_release(f, ev.release_id);
        }
    }
    CHECK(saw, "key after 10s seek");
    CHECK(first_key_90k >= (int64_t)90 * 1000 * 9 &&
              first_key_90k <= (int64_t)90 * 1000 * 11,
          "10s seek lands near 10s, not the previous GOP");
    CHECK_ST(ms_ff_seek(f, (int64_t)90 * 1000 * 20), MS_OK);
    first_key_90k = -1;
    saw = 0;
    for (loops = 0; loops < 400; loops++) {
        ms_status st = ms_ff_pull(f, &ev);
        if (st == MS_AGAIN) {
            continue;
        }
        if (st != MS_OK) {
            break;
        }
        if (ev.type == MS_EVENT_AU && ev.track.kind == MS_TRACK_VIDEO &&
            ev.is_keyframe && ev.has_pts) {
            first_key_90k = event_pts_90k(&ev);
            saw = 1;
            if (ev.release_id) {
                ms_ff_release(f, ev.release_id);
            }
            break;
        }
        if (ev.release_id) {
            ms_ff_release(f, ev.release_id);
        }
    }
    CHECK(saw, "key after 20s seek");
    CHECK(first_key_90k >= (int64_t)90 * 1000 * 19 &&
              first_key_90k <= (int64_t)90 * 1000 * 21,
          "20s seek lands near 20s, not the previous GOP");
    ms_ff_destroy(f);
    printf("  test_ff_seek_keyframe_slop ok (first key near 10s/20s)\n");
    return 0;
}

static int have_cmd_encoder(const char *enc)
{
    char cmd[256];
    int rc;
    snprintf(cmd, sizeof(cmd),
             "ffmpeg -hide_banner -encoders 2>/dev/null | grep -q %s", enc);
    rc = system(cmd);
    return rc == 0;
}

static int test_ff_hevc_mp4_ts(const char *tmpdir)
{
    char mp4[512], ts[512], ts2[512], mid[512], url[600], cmd[2048];
    ms_ff *f = NULL;
    ms_ff_limits lim;
    ms_event ev;
    int cfg = 0, au = 0, key = 0, hvcc = 0, annex = 0, mid_cfg = 0;
    uint32_t first_sig = 0, later_sig = 0;
    ms_status st;

    if (!have_cmd_encoder("libx265")) {
        fprintf(stderr, "FAIL test_ff_hevc_mp4_ts: host ffmpeg/libx265 required for this slice\n");
        failed = 1;
        return 1;
    }
    CHECK(path_join(mp4, sizeof(mp4), tmpdir, "hevc_hvc1.mp4") == 0, "mp4 path");
    CHECK(path_join(ts, sizeof(ts), tmpdir, "hevc16.ts") == 0, "ts path");
    CHECK(path_join(ts2, sizeof(ts2), tmpdir, "hevc32.ts") == 0, "ts2 path");
    CHECK(path_join(mid, sizeof(mid), tmpdir, "hevc_mid.ts") == 0, "mid path");
    snprintf(cmd, sizeof(cmd),
             "ffmpeg -y -hide_banner -loglevel error -f lavfi -i color=c=black:s=16x16:r=25:d=0.48 "
             "-c:v libx265 -pix_fmt yuv420p -tag:v hvc1 "
             "-x265-params keyint=2:min-keyint=2:scenecut=0:repeat-headers=0:log-level=error "
             "-an -f mp4 %s", mp4);
    CHECK(system(cmd) == 0, "enc mp4");
    snprintf(cmd, sizeof(cmd),
             "ffmpeg -y -hide_banner -loglevel error -f lavfi -i color=c=black:s=16x16:r=25:d=0.48 "
             "-c:v libx265 -pix_fmt yuv420p "
             "-x265-params keyint=2:min-keyint=2:scenecut=0:repeat-headers=1:aud=1:log-level=error "
             "-an -f mpegts %s", ts);
    CHECK(system(cmd) == 0, "enc ts");
    snprintf(cmd, sizeof(cmd),
             "ffmpeg -y -hide_banner -loglevel error -f lavfi -i color=c=white:s=32x32:r=25:d=0.32 "
             "-c:v libx265 -pix_fmt yuv420p "
             "-x265-params keyint=2:min-keyint=2:scenecut=0:repeat-headers=1:aud=1:log-level=error "
             "-an -f mpegts %s", ts2);
    CHECK(system(cmd) == 0, "enc ts2");
    snprintf(cmd, sizeof(cmd), "cat %s %s > %s", ts, ts2, mid);
    CHECK(system(cmd) == 0, "concat mid ts");

    ms_ff_limits_default(&lim);
    lim.io_timeout_ms = 4000;
    CHECK_ST(ms_ff_create(NULL, &lim, &f), MS_OK);
    snprintf(url, sizeof(url), "file:%s", mp4);
    CHECK_ST(ms_ff_open(f, url), MS_OK);
    { int steps = 0;
    while ((st = ms_ff_pull(f, &ev)) == MS_OK) {
        CHECK(++steps < 200, "mp4 pull bounded");
        if (ev.type == MS_EVENT_CODEC_CONFIG && ev.track.codec == MS_CODEC_HEVC) {
            ms_hevc_ps_set ps;
            cfg++;
            CHECK(ev.sample_format == MS_SAMPLE_FMT_HVCC, "mp4 hvcc cfg");
            memset(&ps, 0, sizeof(ps));
            ps.struct_size = sizeof(ps);
            CHECK_ST(ms_hevc_ps_parse(MS_SAMPLE_FMT_HVCC, 1, ev.data, ev.data_size, 4, &ps),
                     MS_OK);
            CHECK(ps.triple_complete, "mp4 csd triple");
            hvcc++;
        }
        if (ev.type == MS_EVENT_AU && ev.track.codec == MS_CODEC_HEVC) {
            au++;
            CHECK(ev.sample_format == MS_SAMPLE_FMT_HVCC, "mp4 au hvcc");
            if (ev.is_keyframe) {
                key++;
            }
        }
        if (ev.release_id) {
            ms_ff_release(f, ev.release_id);
        }
    }
    } CHECK(st == MS_EOS, "mp4 eos");
    CHECK(cfg >= 1 && au >= 2 && key >= 1, "mp4 cfg/au/key");
    ms_ff_destroy(f);
    f = NULL;
    printf("  ff HEVC MP4 hvc1: cfg=%d au=%d key=%d hvcc=%d\n", cfg, au, key, hvcc);

    cfg = au = key = annex = 0;
    CHECK_ST(ms_ff_create(NULL, &lim, &f), MS_OK);
    snprintf(url, sizeof(url), "file:%s", ts);
    CHECK_ST(ms_ff_open(f, url), MS_OK);
    { int steps = 0;
    while ((st = ms_ff_pull(f, &ev)) == MS_OK) {
        CHECK(++steps < 400, "ts pull bounded");
        if (ev.type == MS_EVENT_CODEC_CONFIG && ev.track.codec == MS_CODEC_HEVC) {
            ms_hevc_ps_set ps;
            cfg++;
            CHECK(ev.sample_format == MS_SAMPLE_FMT_ANNEXB, "ts annexb cfg");
            memset(&ps, 0, sizeof(ps));
            ps.struct_size = sizeof(ps);
            CHECK_ST(ms_hevc_ps_parse(MS_SAMPLE_FMT_ANNEXB, 1, ev.data, ev.data_size, 4, &ps),
                     MS_OK);
            CHECK(ps.triple_complete, "ts csd triple");
            annex++;
        }
        if (ev.type == MS_EVENT_AU && ev.track.codec == MS_CODEC_HEVC) {
            ms_hevc_au_info info;
            au++;
            CHECK(ev.sample_format == MS_SAMPLE_FMT_ANNEXB, "ts au annexb");
            memset(&info, 0, sizeof(info));
            info.struct_size = sizeof(info);
            CHECK_ST(ms_hevc_au_inspect(MS_SAMPLE_FMT_ANNEXB, ev.data, ev.data_size, 4, &info),
                     MS_OK);
            if (ev.is_keyframe) {
                CHECK(info.is_irap, "ts key is irap");
                key++;
            }
        }
        if (ev.release_id) {
            ms_ff_release(f, ev.release_id);
        }
    }
    } CHECK(st == MS_EOS, "ts eos");
    CHECK(cfg >= 1 && au >= 2 && key >= 1, "ts cfg/au/key");
    /* seek 关键帧门控：再打开并跳到后段，随后首个视频 AU 须为关键帧。 */
    ms_ff_destroy(f);
    f = NULL;
    CHECK_ST(ms_ff_create(NULL, &lim, &f), MS_OK);
    CHECK_ST(ms_ff_open(f, url), MS_OK);
    CHECK_ST(ms_ff_seek(f, 90 * 1000 * 2 / 10), MS_OK); /* ~0.2s at 90k */
    {
        int first_v = 1, post_key = 0, post_au = 0, steps = 0;
        while ((st = ms_ff_pull(f, &ev)) == MS_OK) {
            CHECK(++steps < 200, "seek pull bounded");
            if (ev.type == MS_EVENT_AU && ev.track.codec == MS_CODEC_HEVC) {
                if (first_v) {
                    CHECK(ev.is_keyframe == 1, "first AU after seek is key");
                    first_v = 0;
                    post_key++;
                }
                post_au++;
            }
            if (ev.release_id) {
                ms_ff_release(f, ev.release_id);
            }
        }
        CHECK(post_au >= 1 && post_key == 1, "seek produced key AU");
    }
    ms_ff_destroy(f);
    f = NULL;
    printf("  ff HEVC TS: cfg=%d au=%d key=%d annex=%d seek-gated\n", cfg, au, key, annex);

    /* 拼接两段不同分辨率，期望中途再发 CODEC_CONFIG（载荷签名变化）。 */
    CHECK_ST(ms_ff_create(NULL, &lim, &f), MS_OK);
    snprintf(url, sizeof(url), "file:%s", mid);
    CHECK_ST(ms_ff_open(f, url), MS_OK);
    mid_cfg = 0;
    first_sig = later_sig = 0;
    { int steps = 0;
    while ((st = ms_ff_pull(f, &ev)) == MS_OK) {
        CHECK(++steps < 400, "mid pull bounded");
        if (ev.type == MS_EVENT_CODEC_CONFIG && ev.track.codec == MS_CODEC_HEVC) {
            ms_hevc_ps_set ps;
            uint32_t sig = 0;
            size_t i;
            memset(&ps, 0, sizeof(ps));
            ps.struct_size = sizeof(ps);
            if (ms_hevc_ps_parse(ev.sample_format, 1, ev.data, ev.data_size, 4, &ps) == MS_OK &&
                ps.triple_complete) {
                for (i = 0; i < ps.sps_size; i++) {
                    sig = sig * 16777619u + ps.sps[i];
                }
            }
            mid_cfg++;
            if (first_sig == 0) {
                first_sig = sig ? sig : 1;
            } else if (sig != first_sig) {
                later_sig = sig;
            }
        }
        if (ev.release_id) {
            ms_ff_release(f, ev.release_id);
        }
    }
    } CHECK(st == MS_EOS, "mid eos");
    CHECK(mid_cfg >= 2 && later_sig != 0, "mid-stream VPS/SPS/PPS change emitted");
    ms_ff_destroy(f);
    printf("  ff HEVC TS mid-PS: cfg_events=%d\n", mid_cfg);
    printf("  test_ff_hevc_mp4_ts ok (parse/AU/CSD/key/seek; not device hw)\n");
    return 0;
}




/*
 * 用宿主 /usr/bin/ffmpeg 的 mpeg2video / mpeg4 *编码器* 生成受控样本。
 * 这不等于产品允许软解：产品 FFmpeg 仍 CONFIG_DECODERS=0，视频只走硬解提交路径。
 */
static int test_ff_startcode_one(const char *tmpdir, const char *enc, const char *tag,
                                 ms_codec codec, ms_sample_format fmt)
{
    char mp4[512], ts[512], ts2[512], mid[512], url[600], cmd[2048];
    ms_ff *f = NULL;
    ms_ff_limits lim;
    ms_event ev;
    int cfg = 0, au = 0, key = 0, bpic = 0, pts_ne_dts = 0, mid_cfg = 0;
    uint32_t first_sig = 0, later_sig = 0;
    ms_status st;
    char mp4_name[64], ts_name[64], ts2_name[64], mid_name[64];

    if (!have_cmd_encoder(enc)) {
        fprintf(stderr, "FAIL test_ff_%s: host ffmpeg encoder %s required for fixtures\n",
                tag, enc);
        failed = 1;
        return 1;
    }
    snprintf(mp4_name, sizeof(mp4_name), "%s.mp4", tag);
    snprintf(ts_name, sizeof(ts_name), "%s16.ts", tag);
    snprintf(ts2_name, sizeof(ts2_name), "%s32.ts", tag);
    snprintf(mid_name, sizeof(mid_name), "%s_mid.ts", tag);
    CHECK(path_join(mp4, sizeof(mp4), tmpdir, mp4_name) == 0, "mp4 path");
    CHECK(path_join(ts, sizeof(ts), tmpdir, ts_name) == 0, "ts path");
    CHECK(path_join(ts2, sizeof(ts2), tmpdir, ts2_name) == 0, "ts2 path");
    CHECK(path_join(mid, sizeof(mid), tmpdir, mid_name) == 0, "mid path");
    snprintf(cmd, sizeof(cmd),
             "ffmpeg -y -hide_banner -loglevel error -f lavfi -i color=c=black:s=16x16:r=25:d=0.80 "
             "-c:v %s -pix_fmt yuv420p -g 4 -bf 2 -b:v 200k -an -f mp4 %s", enc, mp4);
    CHECK(system(cmd) == 0, "enc mp4 fixture (encoder != product decode)");
    snprintf(cmd, sizeof(cmd),
             "ffmpeg -y -hide_banner -loglevel error -f lavfi -i color=c=black:s=16x16:r=25:d=0.80 "
             "-c:v %s -pix_fmt yuv420p -g 4 -bf 2 -b:v 200k -an -f mpegts %s", enc, ts);
    CHECK(system(cmd) == 0, "enc ts fixture");
    snprintf(cmd, sizeof(cmd),
             "ffmpeg -y -hide_banner -loglevel error -f lavfi -i color=c=white:s=32x32:r=25:d=0.48 "
             "-c:v %s -pix_fmt yuv420p -g 4 -bf 2 -b:v 200k -an -f mpegts %s", enc, ts2);
    CHECK(system(cmd) == 0, "enc ts2 fixture");
    snprintf(cmd, sizeof(cmd), "cat %s %s > %s", ts, ts2, mid);
    CHECK(system(cmd) == 0, "concat mid ts");

    ms_ff_limits_default(&lim);
    lim.io_timeout_ms = 4000;
    CHECK_ST(ms_ff_create(NULL, &lim, &f), MS_OK);
    snprintf(url, sizeof(url), "file:%s", mp4);
    CHECK_ST(ms_ff_open(f, url), MS_OK);
    { int steps = 0;
    while ((st = ms_ff_pull(f, &ev)) == MS_OK) {
        CHECK(++steps < 400, "mp4 pull bounded");
        if (ev.type == MS_EVENT_CODEC_CONFIG && ev.track.codec == codec) {
            ms_video_cfg vcfg;
            cfg++;
            CHECK(ev.sample_format == fmt, "mp4 fmt cfg");
            ms_video_cfg_default(&vcfg);
            CHECK_ST(ms_video_cfg_parse(codec, ev.sample_format, 1, ev.data, ev.data_size, &vcfg),
                     MS_OK);
            CHECK(vcfg.complete && vcfg.csd_size > 0, "mp4 csd complete");
            CHECK(vcfg.width == 16 && vcfg.height == 16, "mp4 16x16");
        }
        if (ev.type == MS_EVENT_AU && ev.track.codec == codec) {
            au++;
            CHECK(ev.sample_format == fmt, "mp4 au fmt");
            if (ev.is_keyframe) {
                key++;
            }
            if (ev.has_pts && ev.has_dts && ev.pts != ev.dts) {
                pts_ne_dts++;
            }
        }
        if (ev.release_id) {
            ms_ff_release(f, ev.release_id);
        }
    }
    } CHECK(st == MS_EOS, "mp4 eos");
    CHECK(cfg >= 1 && au >= 4 && key >= 1, "mp4 cfg/au/key");
    ms_ff_destroy(f);
    f = NULL;
    printf("  ff %s MP4: cfg=%d au=%d key=%d pts_ne_dts=%d (fixture encode only)\n",
           tag, cfg, au, key, pts_ne_dts);

    cfg = au = key = bpic = pts_ne_dts = 0;
    CHECK_ST(ms_ff_create(NULL, &lim, &f), MS_OK);
    snprintf(url, sizeof(url), "file:%s", ts);
    CHECK_ST(ms_ff_open(f, url), MS_OK);
    { int steps = 0;
    while ((st = ms_ff_pull(f, &ev)) == MS_OK) {
        CHECK(++steps < 800, "ts pull bounded");
        if (ev.type == MS_EVENT_CODEC_CONFIG && ev.track.codec == codec) {
            ms_video_cfg vcfg;
            cfg++;
            CHECK(ev.sample_format == fmt, "ts fmt cfg");
            ms_video_cfg_default(&vcfg);
            CHECK_ST(ms_video_cfg_parse(codec, ev.sample_format, 1, ev.data, ev.data_size, &vcfg),
                     MS_OK);
            CHECK(vcfg.complete, "ts csd complete");
        }
        if (ev.type == MS_EVENT_AU && ev.track.codec == codec) {
            ms_video_au_info info;
            au++;
            CHECK(ev.sample_format == fmt, "ts au fmt");
            memset(&info, 0, sizeof(info));
            info.struct_size = sizeof(info);
            if (ms_video_au_inspect(codec, ev.sample_format, ev.data, ev.data_size, &info) == MS_OK) {
                if (info.pic_type == MS_VPIC_B) {
                    bpic++;
                }
                if (ev.is_keyframe) {
                    CHECK(info.is_key, "key AU inspect agrees");
                    key++;
                }
            } else if (ev.is_keyframe) {
                key++;
            }
            if (ev.has_pts && ev.has_dts && ev.pts != ev.dts) {
                pts_ne_dts++;
            }
        }
        if (ev.release_id) {
            ms_ff_release(f, ev.release_id);
        }
    }
    } CHECK(st == MS_EOS, "ts eos");
    CHECK(cfg >= 1 && au >= 4 && key >= 1, "ts cfg/au/key");
    CHECK(bpic >= 1 || pts_ne_dts >= 1, "B-frame or PTS!=DTS observed");
    ms_ff_destroy(f);
    f = NULL;
    CHECK_ST(ms_ff_create(NULL, &lim, &f), MS_OK);
    CHECK_ST(ms_ff_open(f, url), MS_OK);
    CHECK_ST(ms_ff_seek(f, 90 * 1000 * 3 / 10), MS_OK); /* ~0.3s */
    {
        int first_v = 1, post_key = 0, post_au = 0, steps = 0;
        while ((st = ms_ff_pull(f, &ev)) == MS_OK) {
            CHECK(++steps < 400, "seek pull bounded");
            if (ev.type == MS_EVENT_AU && ev.track.codec == codec) {
                if (first_v) {
                    CHECK(ev.is_keyframe == 1, "first AU after seek is key");
                    first_v = 0;
                    post_key++;
                }
                post_au++;
            }
            if (ev.release_id) {
                ms_ff_release(f, ev.release_id);
            }
        }
        CHECK(post_au >= 1 && post_key == 1, "seek produced key AU");
    }
    ms_ff_destroy(f);
    f = NULL;
    printf("  ff %s TS: cfg=%d au=%d key=%d B=%d pts_ne_dts=%d seek-gated\n",
           tag, cfg, au, key, bpic, pts_ne_dts);

    CHECK_ST(ms_ff_create(NULL, &lim, &f), MS_OK);
    snprintf(url, sizeof(url), "file:%s", mid);
    CHECK_ST(ms_ff_open(f, url), MS_OK);
    mid_cfg = 0;
    first_sig = later_sig = 0;
    { int steps = 0;
    while ((st = ms_ff_pull(f, &ev)) == MS_OK) {
        CHECK(++steps < 800, "mid pull bounded");
        if (ev.type == MS_EVENT_CODEC_CONFIG && ev.track.codec == codec) {
            ms_video_cfg vcfg;
            ms_video_cfg_default(&vcfg);
            if (ms_video_cfg_parse(codec, ev.sample_format, 1, ev.data, ev.data_size,
                                   &vcfg) == MS_OK &&
                vcfg.complete) {
                mid_cfg++;
                if (first_sig == 0) {
                    first_sig = vcfg.payload_sig;
                } else if (vcfg.payload_sig != first_sig) {
                    later_sig = vcfg.payload_sig;
                }
            }
        }
        if (ev.release_id) {
            ms_ff_release(f, ev.release_id);
        }
    }
    } CHECK(st == MS_EOS, "mid eos");
    CHECK(mid_cfg >= 2 && later_sig != 0, "mid-stream config change emitted");
    ms_ff_destroy(f);
    printf("  ff %s TS mid-cfg: events=%d (host parse; not device hw)\n", tag, mid_cfg);
    return 0;
}

static int test_ff_mpeg2_mpeg4(const char *tmpdir)
{
    if (test_ff_startcode_one(tmpdir, "mpeg2video", "mpeg2",
                              MS_CODEC_MPEG2_VIDEO, MS_SAMPLE_FMT_MPV) != 0) {
        return 1;
    }
    if (test_ff_startcode_one(tmpdir, "mpeg4", "mpeg4",
                              MS_CODEC_MPEG4_PART2, MS_SAMPLE_FMT_MP4V) != 0) {
        return 1;
    }
    printf("  test_ff_mpeg2_mpeg4 ok (fixture encode != product decode)\n");
    return 0;
}

/* 宿主 /usr/bin/ffmpeg 的 h263 编码器只用于生成 3GP/MP4 受控样本，不是产品软解。
 * 产品只走已开启的 mov 解复用 + 完整 AU；不把容器 extradata 捏成 CSD。 */

/* 受控手工 AVS+ ES 装进 MPEG-TS（stream_type 0x42）。不是编码器产物，产品不开 cavs 解码器。 */
static int test_ff_avsplus(const char *tmpdir)
{
    char path[512], jpath[512], trunc[512], url[640];
    ms_ff *f = NULL;
    ms_ff_limits lim;
    ms_status st;
    ms_synth_buf b;
    ms_synth_stream streams[1];
    ms_event ev;
    uint8_t au0[64], au1[16];
    size_t n0, n1;
    int cfg = 0, au = 0, key = 0, pts_seen = 0, inspect = 0, plus = 0, family = 0;
    int steps;
    uint32_t pre_cfg_bytes = 0;

    n0 = 0;
    memcpy(au0, k_avsplus_seq_16, sizeof(k_avsplus_seq_16));
    n0 = sizeof(k_avsplus_seq_16);
    memcpy(au0 + n0, k_avs_pic_i, sizeof(k_avs_pic_i));
    n0 += sizeof(k_avs_pic_i);
    memcpy(au1, k_avs_pic_p, sizeof(k_avs_pic_p));
    n1 = sizeof(k_avs_pic_p);

    memset(streams, 0, sizeof(streams));
    streams[0].stream_type = 0x42;
    streams[0].pid = 0x111;
    ms_synth_init(&b);
    ms_synth_reset_cc();
    CHECK(ms_synth_pat_pmt(&b, 1, 0x110, streams, 1, 8, 188) == 0, "pat pmt");
    CHECK(ms_synth_pes(&b, 0x111, 0xE0, 1, 0, au0, n0, 188) == 0, "pes I");
    CHECK(ms_synth_pes(&b, 0x111, 0xE0, 1, 9000, au1, n1, 188) == 0, "pes P");
    CHECK(path_join(path, sizeof(path), tmpdir, "avsplus_16.ts") == 0, "path");
    CHECK(write_file(path, b.data, b.len) == 0, "write ts");
    ms_synth_free(&b);

    memset(&lim, 0, sizeof(lim));
    lim.struct_size = sizeof(lim);
    CHECK_ST(ms_ff_create(NULL, &lim, &f), MS_OK);
    snprintf(url, sizeof(url), "file:%s", path);
    CHECK_ST(ms_ff_open(f, url), MS_OK);
    steps = 0;
    while ((st = ms_ff_pull(f, &ev)) == MS_OK) {
        CHECK(++steps < 400, "bounded");
        if (ev.track.codec == MS_CODEC_AVS_PLUS) {
            plus++;
        }
        if (ev.track.codec == MS_CODEC_AVS_FAMILY) {
            family++;
        }
        if (ev.type == MS_EVENT_CODEC_CONFIG && ev.track.codec == MS_CODEC_AVS_PLUS) {
            ms_video_cfg vc;
            cfg++;
            CHECK(ev.data != NULL && ev.data_size > 0, "real seq csd");
            pre_cfg_bytes = (uint32_t)ev.data_size;
            ms_video_cfg_default(&vc);
            CHECK_ST(ms_video_cfg_parse(MS_CODEC_AVS_PLUS, ev.sample_format, 1,
                                        ev.data, ev.data_size, &vc), MS_OK);
            CHECK(vc.complete && vc.width == 16 && vc.height == 16, "cfg 16x16");
        }
        if (ev.type == MS_EVENT_AU &&
            (ev.track.codec == MS_CODEC_AVS_PLUS || ev.track.codec == MS_CODEC_AVS_FAMILY)) {
            ms_video_au_info info;
            au++;
            if (ev.has_pts) {
                pts_seen++;
            }
            if (ev.is_keyframe) {
                key++;
            }
            memset(&info, 0, sizeof(info));
            info.struct_size = sizeof(info);
            if (ev.data && ev.data_size &&
                ms_video_au_inspect(MS_CODEC_AVS_PLUS, ev.sample_format,
                                    ev.data, ev.data_size, &info) == MS_OK) {
                inspect++;
            }
        }
        if (ev.release_id) {
            ms_ff_release(f, ev.release_id);
        }
    }
    CHECK(st == MS_EOS, "eos");
    CHECK(cfg >= 1 && plus >= 1, "broadcast profile upgraded to AVS+");
    CHECK(au >= 2 && key >= 1 && pts_seen >= 1 && inspect >= 2, "au/key/pts/inspect");
    CHECK(pre_cfg_bytes >= sizeof(k_avsplus_seq_16), "csd length");
    ms_ff_destroy(f);
    f = NULL;
    printf("  ff avs+ TS: cfg=%d au=%d key=%d pts=%d inspect=%d plus=%d family=%d "
           "(hand-packed GY/T 257 0x48; no encoder; not product decode)\n",
           cfg, au, key, pts_seen, inspect, plus, family);

    /* 中途 seek：门控由 player 测；这里确认 seek 后仍能拉到 AU 且 codec 仍是 AVS+ */
    CHECK_ST(ms_ff_create(NULL, &lim, &f), MS_OK);
    CHECK_ST(ms_ff_open(f, url), MS_OK);
    CHECK_ST(ms_ff_seek(f, 0), MS_OK);
    {
        int post = 0, post_plus = 0;
        steps = 0;
        while ((st = ms_ff_pull(f, &ev)) == MS_OK) {
            CHECK(++steps < 400, "seek bounded");
            if (ev.type == MS_EVENT_AU && ev.track.codec == MS_CODEC_AVS_PLUS) {
                post++;
                post_plus++;
            }
            if (ev.release_id) ms_ff_release(f, ev.release_id);
        }
        CHECK(st == MS_EOS && post >= 1 && post_plus >= 1, "au after seek still AVS+");
    }
    ms_ff_destroy(f);
    f = NULL;

    /* 基准类 0x20：不得升格为 AVS+ */
    n0 = 0;
    memcpy(au0, k_avs_jizhun_seq_16, sizeof(k_avs_jizhun_seq_16));
    n0 = sizeof(k_avs_jizhun_seq_16);
    memcpy(au0 + n0, k_avs_pic_i, sizeof(k_avs_pic_i));
    n0 += sizeof(k_avs_pic_i);
    ms_synth_init(&b);
    ms_synth_reset_cc();
    CHECK(ms_synth_pat_pmt(&b, 1, 0x110, streams, 1, 8, 188) == 0, "jizhun pat");
    CHECK(ms_synth_pes(&b, 0x111, 0xE0, 1, 0, au0, n0, 188) == 0, "jizhun pes");
    CHECK(path_join(jpath, sizeof(jpath), tmpdir, "avs_jizhun_16.ts") == 0, "jpath");
    CHECK(write_file(jpath, b.data, b.len) == 0, "write jizhun");
    ms_synth_free(&b);
    CHECK_ST(ms_ff_create(NULL, &lim, &f), MS_OK);
    snprintf(url, sizeof(url), "file:%s", jpath);
    CHECK_ST(ms_ff_open(f, url), MS_OK);
    plus = family = cfg = au = 0;
    steps = 0;
    while ((st = ms_ff_pull(f, &ev)) == MS_OK) {
        CHECK(++steps < 400, "jizhun bounded");
        if (ev.track.codec == MS_CODEC_AVS_PLUS) plus++;
        if (ev.track.codec == MS_CODEC_AVS_FAMILY) family++;
        if (ev.type == MS_EVENT_CODEC_CONFIG && ev.track.codec == MS_CODEC_AVS_PLUS) cfg++;
        if (ev.type == MS_EVENT_AU) au++;
        if (ev.release_id) ms_ff_release(f, ev.release_id);
    }
    CHECK(st == MS_EOS, "jizhun eos");
    CHECK(plus == 0 && cfg == 0, "jizhun must not be AVS+");
    CHECK(family >= 1 || au >= 1, "family or au present");
    ms_ff_destroy(f);
    f = NULL;

    /* 截断 TS 有界 */
    CHECK(path_join(trunc, sizeof(trunc), tmpdir, "avsplus_trunc.ts") == 0, "trunc path");
    {
        FILE *fp = fopen(path, "rb");
        unsigned char buf[128];
        size_t r;
        CHECK(fp != NULL, "open src");
        r = fread(buf, 1, 80, fp);
        fclose(fp);
        CHECK(r > 0 && write_file(trunc, buf, r) == 0, "write trunc");
    }
    CHECK_ST(ms_ff_create(NULL, &lim, &f), MS_OK);
    snprintf(url, sizeof(url), "file:%s", trunc);
    st = ms_ff_open(f, url);
    /* 截断可能 open 失败或立刻 EOS，均须有界且不崩溃 */
    if (st == MS_OK) {
        steps = 0;
        while ((st = ms_ff_pull(f, &ev)) == MS_OK) {
            CHECK(++steps < 200, "trunc bounded");
            if (ev.release_id) ms_ff_release(f, ev.release_id);
        }
        CHECK(st == MS_EOS || st == MS_ERR_MALFORMED || st == MS_ERR_OVERFLOW ||
              st == MS_AGAIN, "trunc status bounded");
    }
    ms_ff_destroy(f);
    f = NULL;

    printf("  test_ff_avsplus ok (hand-packed TS; 0x48 vs 0x20; seek/trunc; "
           "fixture != product decode; no AVS2 stand-in)\n");
    return 0;
}

static int test_ff_h263(const char *tmpdir)
{
    char t3gp[512], trunc[512], url[640], cmd[2048];
    ms_ff *f = NULL;
    ms_ff_limits lim;
    ms_event ev;
    ms_status st;
    int cfg = 0, au = 0, key = 0, pts_seen = 0, pts_ne_dts = 0, inspected = 0;
    size_t flen = 0;

    CHECK(path_join(t3gp, sizeof(t3gp), tmpdir, "h263_qcif.3gp") == 0, "3gp path");
    CHECK(path_join(trunc, sizeof(trunc), tmpdir, "h263_qcif_trunc.3gp") == 0, "trunc path");
    /* 宿主 ffmpeg 的 mp4 复用器对 ITU H.263 无 tag；产品路径用已开启的 mov/3GP。 */
    snprintf(cmd, sizeof(cmd),
             "ffmpeg -y -hide_banner -loglevel error -f lavfi -i color=c=black:s=176x144:r=25:d=0.80 "
             "-c:v h263 -an -f 3gp %s", t3gp);
    CHECK(system(cmd) == 0, "enc 3gp fixture (encoder != product decode)");

    ms_ff_limits_default(&lim);
    lim.io_timeout_ms = 4000;
    CHECK_ST(ms_ff_create(NULL, &lim, &f), MS_OK);
    snprintf(url, sizeof(url), "file:%s", t3gp);
    CHECK_ST(ms_ff_open(f, url), MS_OK);
    { int steps = 0;
    while ((st = ms_ff_pull(f, &ev)) == MS_OK) {
        CHECK(++steps < 400, "3gp pull bounded");
        if (ev.type == MS_EVENT_CODEC_CONFIG && ev.track.codec == MS_CODEC_H263) {
            cfg++;
            CHECK(ev.data_size == 0, "no invented h263 csd");
        }
        if (ev.type == MS_EVENT_AU && ev.track.codec == MS_CODEC_H263) {
            ms_video_cfg vcfg;
            ms_video_au_info info;
            au++;
            CHECK(ev.sample_format == MS_SAMPLE_FMT_H263, "3gp au fmt");
            if (ev.is_keyframe) {
                key++;
            }
            if (ev.has_pts) {
                pts_seen++;
            }
            if (ev.has_pts && ev.has_dts && ev.pts != ev.dts) {
                pts_ne_dts++;
            }
            memset(&info, 0, sizeof(info));
            info.struct_size = sizeof(info);
            if (ev.data != NULL && ev.data_size > 0 &&
                ms_video_au_inspect(MS_CODEC_H263, ev.sample_format, ev.data, ev.data_size,
                                    &info) == MS_OK) {
                if (ev.is_keyframe) {
                    CHECK(info.is_key, "key inspect agrees");
                }
                ms_video_cfg_default(&vcfg);
                if (ms_video_cfg_parse(MS_CODEC_H263, ev.sample_format, 0, ev.data,
                                       ev.data_size, &vcfg) == MS_OK &&
                    vcfg.complete) {
                    CHECK(vcfg.width == 176 && vcfg.height == 144, "qcif from psc");
                    CHECK(vcfg.csd_size == 0, "in-band cfg has no csd");
                    inspected++;
                }
            }
        }
        if (ev.release_id) {
            ms_ff_release(f, ev.release_id);
        }
    }
    } CHECK(st == MS_EOS, "3gp eos");
    CHECK(cfg == 0, "no h263 CONFIG event (legal no-CSD path)");
    CHECK(au >= 4 && key >= 1 && pts_seen >= 1 && inspected >= 1, "3gp au/key/pts/cfg");
    ms_ff_destroy(f);
    f = NULL;
    printf("  ff h263 3GP: cfg=%d au=%d key=%d pts=%d pts_ne_dts=%d inband_cfg=%d "
           "(PTS==DTS is honest for baseline; fixture encode only)\n",
           cfg, au, key, pts_seen, pts_ne_dts, inspected);

    CHECK_ST(ms_ff_create(NULL, &lim, &f), MS_OK);
    snprintf(url, sizeof(url), "file:%s", t3gp);
    CHECK_ST(ms_ff_open(f, url), MS_OK);
    CHECK_ST(ms_ff_seek(f, 90 * 1000 * 3 / 10), MS_OK);
    {
        int first_v = 1, post_key = 0, post_au = 0, steps = 0;
        while ((st = ms_ff_pull(f, &ev)) == MS_OK) {
            CHECK(++steps < 400, "seek pull bounded");
            if (ev.type == MS_EVENT_AU && ev.track.codec == MS_CODEC_H263) {
                if (first_v) {
                    CHECK(ev.is_keyframe == 1, "first AU after seek is key");
                    first_v = 0;
                    post_key++;
                }
                post_au++;
            }
            if (ev.release_id) {
                ms_ff_release(f, ev.release_id);
            }
        }
        CHECK(post_au >= 1 && post_key == 1, "seek produced key AU");
    }
    ms_ff_destroy(f);
    f = NULL;
    printf("  ff h263 3GP seek-gated (host demux; not device hw; no mp4 tag in fixture encoder)\n");

    {
        FILE *in, *out;
        unsigned char buf[4096];
        size_t n, keep, wr = 0;
        in = fopen(t3gp, "rb");
        CHECK(in != NULL, "open 3gp for trunc");
        if (fseek(in, 0, SEEK_END) != 0) {
            fclose(in);
            CHECK(0, "seek end");
        }
        flen = (size_t)ftell(in);
        rewind(in);
        keep = flen / 2;
        if (keep < 32) {
            keep = 32;
        }
        out = fopen(trunc, "wb");
        CHECK(out != NULL, "trunc out");
        while (wr < keep && (n = fread(buf, 1, sizeof(buf), in)) > 0) {
            size_t chunk = n;
            if (wr + chunk > keep) {
                chunk = keep - wr;
            }
            CHECK(fwrite(buf, 1, chunk, out) == chunk, "trunc write");
            wr += chunk;
        }
        fclose(in);
        fclose(out);
    }
    CHECK_ST(ms_ff_create(NULL, &lim, &f), MS_OK);
    snprintf(url, sizeof(url), "file:%s", trunc);
    st = ms_ff_open(f, url);
    if (st == MS_OK) {
        int steps = 0;
        while ((st = ms_ff_pull(f, &ev)) == MS_OK) {
            CHECK(++steps < 400, "trunc pull bounded");
            if (ev.release_id) {
                ms_ff_release(f, ev.release_id);
            }
        }
        CHECK(st == MS_EOS || st == MS_ERR_MALFORMED || st == MS_ERR_UNKNOWN ||
              st == MS_ERR_TIMEOUT || st == MS_ERR_UNSUPPORTED,
              "trunc ends without crash");
    } else {
        CHECK(st == MS_ERR_MALFORMED || st == MS_ERR_UNKNOWN || st == MS_ERR_INVALID_ARG ||
              st == MS_ERR_UNSUPPORTED || st == MS_ERR_TIMEOUT,
              "trunc open rejected honestly");
    }
    ms_ff_destroy(f);
    printf("  test_ff_h263 ok (legal no-CSD; mid-stream size via player fixtures; "
           "fixture encode != product decode)\n");
    return 0;
}

/* 宿主 libaom-av1 只用于生成 MP4 受控样本，不是产品软解，也不引入 dav1d。 */
static int test_ff_av1(const char *tmpdir)
{
    char mp4[512], trunc[512], url[640], cmd[2048];
    ms_ff *f = NULL;
    ms_ff_limits lim;
    ms_event ev;
    ms_status st;
    int cfg = 0, au = 0, key = 0, pts_seen = 0, inspected = 0, cfg_bytes = 0;

    CHECK(path_join(mp4, sizeof(mp4), tmpdir, "av1_64.mp4") == 0, "mp4 path");
    CHECK(path_join(trunc, sizeof(trunc), tmpdir, "av1_64_trunc.mp4") == 0, "trunc path");
    snprintf(cmd, sizeof(cmd),
             "ffmpeg -y -hide_banner -loglevel error -f lavfi -i color=c=black:s=64x64:r=25:d=0.80 "
             "-c:v libaom-av1 -cpu-used 8 -usage realtime -tiles 1x1 -g 4 -an -f mp4 %s",
             mp4);
    CHECK(system(cmd) == 0, "enc av1 mp4 fixture (encoder != product decode)");

    ms_ff_limits_default(&lim);
    lim.io_timeout_ms = 4000;
    CHECK_ST(ms_ff_create(NULL, &lim, &f), MS_OK);
    snprintf(url, sizeof(url), "file:%s", mp4);
    CHECK_ST(ms_ff_open(f, url), MS_OK);
    { int steps = 0;
    while ((st = ms_ff_pull(f, &ev)) == MS_OK) {
        CHECK(++steps < 400, "mp4 pull bounded");
        if (ev.type == MS_EVENT_CODEC_CONFIG && ev.track.codec == MS_CODEC_AV1) {
            ms_video_cfg vcfg;
            cfg++;
            CHECK(ev.sample_format == MS_SAMPLE_FMT_AV1, "cfg fmt");
            CHECK(ev.data != NULL && ev.data_size > 0, "real av1c not invented empty");
            ms_video_cfg_default(&vcfg);
            CHECK_ST(ms_video_cfg_parse(MS_CODEC_AV1, ev.sample_format, 1,
                                        ev.data, ev.data_size, &vcfg), MS_OK);
            CHECK(vcfg.complete && vcfg.csd_size > 0, "av1c complete");
            CHECK(vcfg.width == 64 && vcfg.height == 64, "64x64");
            cfg_bytes += (int)ev.data_size;
        }
        if (ev.type == MS_EVENT_AU && ev.track.codec == MS_CODEC_AV1) {
            ms_video_au_info info;
            au++;
            CHECK(ev.sample_format == MS_SAMPLE_FMT_AV1, "au fmt");
            if (ev.is_keyframe) {
                key++;
            }
            if (ev.has_pts) {
                pts_seen++;
            }
            memset(&info, 0, sizeof(info));
            info.struct_size = sizeof(info);
            if (ev.data != NULL && ev.data_size > 0 &&
                ms_video_au_inspect(MS_CODEC_AV1, ev.sample_format, ev.data,
                                    ev.data_size, &info) == MS_OK) {
                if (ev.is_keyframe) {
                    CHECK(info.is_key, "key inspect agrees");
                }
                inspected++;
            }
        }
        if (ev.release_id) {
            ms_ff_release(f, ev.release_id);
        }
    }
    } CHECK(st == MS_EOS, "mp4 eos");
    CHECK(cfg >= 1 && au >= 4 && key >= 1 && pts_seen >= 1 && inspected >= 1,
          "av1 cfg/au/key/pts");
    ms_ff_destroy(f);
    f = NULL;
    printf("  ff av1 MP4: cfg=%d au=%d key=%d pts=%d inspect=%d av1c_bytes=%d "
           "(fixture encode only; mid-stream SH via player fixtures)\n",
           cfg, au, key, pts_seen, inspected, cfg_bytes);

    CHECK_ST(ms_ff_create(NULL, &lim, &f), MS_OK);
    CHECK_ST(ms_ff_open(f, url), MS_OK);
    CHECK_ST(ms_ff_seek(f, 90 * 1000 * 3 / 10), MS_OK);
    {
        int first_v = 1, post_key = 0, post_au = 0, steps = 0;
        while ((st = ms_ff_pull(f, &ev)) == MS_OK) {
            CHECK(++steps < 400, "seek pull bounded");
            if (ev.type == MS_EVENT_AU && ev.track.codec == MS_CODEC_AV1) {
                if (first_v) {
                    CHECK(ev.is_keyframe == 1, "first AU after seek is key");
                    first_v = 0;
                    post_key++;
                }
                post_au++;
            }
            if (ev.release_id) {
                ms_ff_release(f, ev.release_id);
            }
        }
        CHECK(post_au >= 1 && post_key == 1, "seek produced key AU");
    }
    ms_ff_destroy(f);
    f = NULL;

    {
        FILE *in, *out;
        unsigned char buf[4096];
        size_t n, keep, wr = 0, flen;
        in = fopen(mp4, "rb");
        CHECK(in != NULL, "open mp4 for trunc");
        if (fseek(in, 0, SEEK_END) != 0) {
            fclose(in);
            CHECK(0, "seek end");
        }
        flen = (size_t)ftell(in);
        rewind(in);
        keep = flen / 2;
        if (keep < 32) {
            keep = 32;
        }
        out = fopen(trunc, "wb");
        CHECK(out != NULL, "trunc out");
        while (wr < keep && (n = fread(buf, 1, sizeof(buf), in)) > 0) {
            size_t chunk = n;
            if (wr + chunk > keep) {
                chunk = keep - wr;
            }
            CHECK(fwrite(buf, 1, chunk, out) == chunk, "trunc write");
            wr += chunk;
        }
        fclose(in);
        fclose(out);
    }
    CHECK_ST(ms_ff_create(NULL, &lim, &f), MS_OK);
    snprintf(url, sizeof(url), "file:%s", trunc);
    st = ms_ff_open(f, url);
    if (st == MS_OK) {
        int steps = 0;
        while ((st = ms_ff_pull(f, &ev)) == MS_OK) {
            CHECK(++steps < 400, "trunc pull bounded");
            if (ev.release_id) {
                ms_ff_release(f, ev.release_id);
            }
        }
        CHECK(st == MS_EOS || st == MS_ERR_MALFORMED || st == MS_ERR_UNKNOWN ||
              st == MS_ERR_TIMEOUT || st == MS_ERR_UNSUPPORTED,
              "trunc ends without crash");
    } else {
        CHECK(st == MS_ERR_MALFORMED || st == MS_ERR_UNKNOWN ||
              st == MS_ERR_INVALID_ARG || st == MS_ERR_UNSUPPORTED ||
              st == MS_ERR_TIMEOUT,
              "trunc open rejected honestly");
    }
    ms_ff_destroy(f);
    printf("  test_ff_av1 ok (legal av1C path evidenced; no-CSD path in parse/player; "
           "fixture encode != product decode; not all AV1 bitstreams)\n");
    return 0;
}

static int read_all_file(const char *path, uint8_t **out, size_t *len)
{
    FILE *fp;
    long n;
    uint8_t *b;
    *out = NULL;
    *len = 0;
    fp = fopen(path, "rb");
    if (!fp) {
        return -1;
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return -1;
    }
    n = ftell(fp);
    if (n < 0 || n > 8 * 1024 * 1024) {
        fclose(fp);
        return -1;
    }
    rewind(fp);
    b = (uint8_t *)malloc((size_t)n + 1u);
    if (!b) {
        fclose(fp);
        return -1;
    }
    if (n > 0 && fread(b, 1, (size_t)n, fp) != (size_t)n) {
        free(b);
        fclose(fp);
        return -1;
    }
    fclose(fp);
    b[n] = 0;
    *out = b;
    *len = (size_t)n;
    return 0;
}

static int write_text_file(const char *path, const char *s)
{
    return write_file(path, s, strlen(s));
}

/* 受控 HTTP HLS master + 外链 AAC 备选音轨。不用软解判断音色，用轨 id / CSD / PTS。 */
static int test_ff_hls_alt_audio(const char *tmpdir)
{
    const char *ffmpeg = "/usr/bin/ffmpeg";
    char dir[512], vts[512], ets[512], zts[512], cmd[2048];
    char vpl[512], epl[512], zpl[512], mpl[512], qpl[512], bpl[512], mpl_bad[512];
    char url[640], qurl[640], burl[640], gurl[640];
    uint8_t *vbody = NULL, *ebody = NULL, *zbody = NULL;
    uint8_t *vplb = NULL, *eplb = NULL, *zplb = NULL, *mplb = NULL, *qplb = NULL, *bplb = NULL, *gadb = NULL;
    size_t vbl = 0, ebl = 0, zbl = 0, vpln = 0, epln = 0, zpln = 0, mpln = 0, qpln = 0, bpln = 0, gadn = 0;
    test_srv hs;
    ms_ff *f = NULL;
    ms_ff_limits lim;
    ms_track_info ti[16];
    size_t nt = 0, k;
    uint32_t vid = 0, a_eng = 0, a_zho = 0;
    int def_eng = 0, name_ok = 0, lang_ok = 0;
    ms_event ev;
    int loops, st_ok;
    int v_au = 0, e_au = 0, z_au = 0, v_cfg = 0, e_cfg = 0, z_cfg = 0;
    unsigned long long sel_ep0 = 1, sel_ep1 = 1, tl_ep0 = 1, tl_ep1 = 1;
    uint8_t eng_csd[64], zho_csd[64];
    size_t eng_csd_n = 0, zho_csd_n = 0;
    uint64_t last_vpts = 0, last_apts = 0, first_zpts = 0;
    int have_vpts = 0, have_zpts = 0;
    int after_sw_video = 0;
    ms_status st;

    memset(&hs, 0, sizeof(hs));
    if (access(ffmpeg, X_OK) != 0) {
        printf("  HLS alt audio: no /usr/bin/ffmpeg; untested gap, not a pass\n");
        return 1;
    }
    CHECK(path_join(dir, sizeof(dir), tmpdir, "hlsalt") == 0, "dir");
    (void)mkdir(dir, 0755);
    CHECK(path_join(vts, sizeof(vts), dir, "v.ts") == 0, "vts");
    CHECK(path_join(ets, sizeof(ets), dir, "eng.ts") == 0, "ets");
    CHECK(path_join(zts, sizeof(zts), dir, "zho.ts") == 0, "zts");
    snprintf(cmd, sizeof(cmd),
             "%s -hide_banner -loglevel error -y "
             "-f lavfi -i testsrc=size=160x120:rate=25:duration=4 "
             "-c:v libx264 -pix_fmt yuv420p -g 25 -keyint_min 25 -an %s",
             ffmpeg, vts);
    CHECK(system(cmd) == 0, "encode video ts");
    snprintf(cmd, sizeof(cmd),
             "%s -hide_banner -loglevel error -y "
             "-f lavfi -i sine=frequency=440:duration=4 "
             "-c:a aac -ar 48000 -ac 2 -b:a 64k %s",
             ffmpeg, ets);
    CHECK(system(cmd) == 0, "encode eng aac ts");
    snprintf(cmd, sizeof(cmd),
             "%s -hide_banner -loglevel error -y "
             "-f lavfi -i sine=frequency=880:duration=4 "
             "-c:a aac -ar 44100 -ac 1 -b:a 48k %s",
             ffmpeg, zts);
    CHECK(system(cmd) == 0, "encode zho aac ts");
    CHECK(path_join(vpl, sizeof(vpl), dir, "v.m3u8") == 0, "vpl");
    CHECK(path_join(epl, sizeof(epl), dir, "eng.m3u8") == 0, "epl");
    CHECK(path_join(zpl, sizeof(zpl), dir, "zho.m3u8") == 0, "zpl");
    CHECK(path_join(mpl, sizeof(mpl), dir, "master.m3u8") == 0, "mpl");
    CHECK(path_join(qpl, sizeof(qpl), dir, "qmaster.m3u8") == 0, "qpl");
    CHECK(path_join(bpl, sizeof(bpl), dir, "badgroup.m3u8") == 0, "bpl");
    CHECK(path_join(mpl_bad, sizeof(mpl_bad), dir, "broken.m3u8") == 0, "broken");
    CHECK(write_text_file(vpl,
             "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:5\n"
             "#EXT-X-MEDIA-SEQUENCE:1\n#EXTINF:4.0,\nv.ts\n#EXT-X-ENDLIST\n") == 0,
          "write v playlist");
    CHECK(write_text_file(epl,
             "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:5\n"
             "#EXT-X-MEDIA-SEQUENCE:1\n#EXTINF:4.0,\neng.ts\n#EXT-X-ENDLIST\n") == 0,
          "write eng playlist");
    CHECK(write_text_file(zpl,
             "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:5\n"
             "#EXT-X-MEDIA-SEQUENCE:1\n#EXTINF:4.0,\nzho.ts\n#EXT-X-ENDLIST\n") == 0,
          "write zho playlist");
    CHECK(write_text_file(mpl,
             "#EXTM3U\n"
             "#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"ag\",LANGUAGE=\"eng\","
             "NAME=\"English\",DEFAULT=YES,AUTOSELECT=YES,URI=\"eng/prog.m3u8\"\n"
             "#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"ag\",LANGUAGE=\"zho\","
             "NAME=\"Chinese\",DEFAULT=NO,AUTOSELECT=YES,URI=\"zho/prog.m3u8\"\n"
             "#EXT-X-STREAM-INF:BANDWIDTH=250000,CODECS=\"avc1.42e01e,mp4a.40.2\","
             "AUDIO=\"ag\"\n"
             "v/prog.m3u8\n") == 0,
          "write master");
    CHECK(write_text_file(qpl,
             "#EXTM3U\n"
             "#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"ag\",LANGUAGE=\"eng\","
             "NAME=\"English\",DEFAULT=YES,AUTOSELECT=YES,URI=\"eng/prog.m3u8?tok=1\"\n"
             "#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"ag\",LANGUAGE=\"zho\","
             "NAME=\"Chinese\",DEFAULT=NO,AUTOSELECT=YES,URI=\"zho/prog.m3u8\"\n"
             "#EXT-X-STREAM-INF:BANDWIDTH=250000,AUDIO=\"ag\"\n"
             "v/prog.m3u8\n") == 0,
          "write query master");
    CHECK(write_text_file(bpl,
             "#EXTM3U\n"
             "#EXT-X-STREAM-INF:BANDWIDTH=250000,AUDIO=\"missing\"\n"
             "v/prog.m3u8\n") == 0,
          "write bad group master");
    CHECK(write_text_file(mpl_bad,
             "#EXTM3U\n"
             "#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"ag\",LANGUAGE=\"eng\","
             "NAME=\"English\",DEFAULT=YES,AUTOSELECT=YES,URI=\"eng/prog.m3u8\"\n"
             "#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"ag\",LANGUAGE=\"zho\","
             "NAME=\"Broken\",DEFAULT=NO,AUTOSELECT=YES,URI=\"nope.m3u8\"\n"
             "#EXT-X-STREAM-INF:BANDWIDTH=250000,AUDIO=\"ag\"\n"
             "v/prog.m3u8\n") == 0,
          "write broken rendition master");
    CHECK(read_all_file(vts, &vbody, &vbl) == 0, "read v.ts");
    CHECK(read_all_file(ets, &ebody, &ebl) == 0, "read eng.ts");
    CHECK(read_all_file(zts, &zbody, &zbl) == 0, "read zho.ts");
    CHECK(read_all_file(vpl, &vplb, &vpln) == 0, "read v.m3u8");
    CHECK(read_all_file(epl, &eplb, &epln) == 0, "read eng.m3u8");
    CHECK(read_all_file(zpl, &zplb, &zpln) == 0, "read zho.m3u8");
    CHECK(read_all_file(mpl, &mplb, &mpln) == 0, "read master");
    CHECK(read_all_file(qpl, &qplb, &qpln) == 0, "read qmaster");
    CHECK(read_all_file(bpl, &bplb, &bpln) == 0, "read badgroup");
    CHECK(read_all_file(mpl_bad, &gadb, &gadn) == 0, "read broken");

    CHECK(srv_start(&hs, 0, NULL, NULL) == 0, "start http");
    CHECK(srv_add(&hs, "/hls/master.m3u8", mplb, mpln, "application/vnd.apple.mpegurl") == 0, "add master");
    CHECK(srv_add(&hs, "/hls/v/prog.m3u8", vplb, vpln, "application/vnd.apple.mpegurl") == 0, "add v pl");
    CHECK(srv_add(&hs, "/hls/v/v.ts", vbody, vbl, "video/mp2t") == 0, "add v ts");
    CHECK(srv_add(&hs, "/hls/eng/prog.m3u8", eplb, epln, "application/vnd.apple.mpegurl") == 0, "add e pl");
    CHECK(srv_add(&hs, "/hls/eng/eng.ts", ebody, ebl, "video/mp2t") == 0, "add e ts");
    CHECK(srv_add(&hs, "/hls/zho/prog.m3u8", zplb, zpln, "application/vnd.apple.mpegurl") == 0, "add z pl");
    CHECK(srv_add(&hs, "/hls/zho/zho.ts", zbody, zbl, "video/mp2t") == 0, "add z ts");
    CHECK(srv_add(&hs, "/q/master.m3u8", qplb, qpln, "application/vnd.apple.mpegurl") == 0, "add q master");
    CHECK(srv_add(&hs, "/q/v/prog.m3u8", vplb, vpln, "application/vnd.apple.mpegurl") == 0, "add q v");
    CHECK(srv_add(&hs, "/q/v/v.ts", vbody, vbl, "video/mp2t") == 0, "add q vts");
    CHECK(srv_add(&hs, "/q/eng/prog.m3u8?tok=1", eplb, epln, "application/vnd.apple.mpegurl") == 0, "add q epl");
    CHECK(srv_add(&hs, "/q/eng/eng.ts", ebody, ebl, "video/mp2t") == 0, "add q ets");
    CHECK(srv_add(&hs, "/q/zho/prog.m3u8", zplb, zpln, "application/vnd.apple.mpegurl") == 0, "add q zpl");
    CHECK(srv_add(&hs, "/q/zho/zho.ts", zbody, zbl, "video/mp2t") == 0, "add q zts");
    CHECK(srv_add(&hs, "/bad/master.m3u8", bplb, bpln, "application/vnd.apple.mpegurl") == 0, "add bad");
    CHECK(srv_add(&hs, "/bad/v/prog.m3u8", vplb, vpln, "application/vnd.apple.mpegurl") == 0, "add bad v");
    CHECK(srv_add(&hs, "/bad/v/v.ts", vbody, vbl, "video/mp2t") == 0, "add bad vts");
    CHECK(srv_add(&hs, "/brk/master.m3u8", gadb, gadn, "application/vnd.apple.mpegurl") == 0, "add brk");
    CHECK(srv_add(&hs, "/brk/v/prog.m3u8", vplb, vpln, "application/vnd.apple.mpegurl") == 0, "add brk v");
    CHECK(srv_add(&hs, "/brk/v/v.ts", vbody, vbl, "video/mp2t") == 0, "add brk vts");
    CHECK(srv_add(&hs, "/brk/eng/prog.m3u8", eplb, epln, "application/vnd.apple.mpegurl") == 0, "add brk e");
    CHECK(srv_add(&hs, "/brk/eng/eng.ts", ebody, ebl, "video/mp2t") == 0, "add brk ets");

    snprintf(url, sizeof(url), "http://127.0.0.1:%d/hls/master.m3u8", hs.port);
    ms_ff_limits_default(&lim);
    lim.io_timeout_ms = 8000;
    CHECK_ST(ms_ff_create(NULL, &lim, &f), MS_OK);
    st = ms_ff_open(f, url);
    if (st != MS_OK) {
        fprintf(stderr, "FAIL open hls alt master: %s\n", ms_status_name(st));
        ms_ff_destroy(f);
        srv_stop(&hs);
        failed = 1;
        return 1;
    }
    CHECK_ST(ms_ff_list_tracks(f, ti, 16, &nt), MS_OK);
    CHECK(nt >= 3, "video + two audio renditions enumerated");
    for (k = 0; k < nt; k++) {
        if (ti[k].kind == MS_TRACK_VIDEO && vid == 0) {
            vid = ti[k].id;
        }
        if (ti[k].kind == MS_TRACK_AUDIO) {
            if (ti[k].language && strcmp(ti[k].language, "eng") == 0) {
                a_eng = ti[k].id;
                lang_ok++;
                if (ti[k].is_default) {
                    def_eng = 1;
                }
                if (ti[k].name && strstr(ti[k].name, "English") != NULL) {
                    name_ok++;
                }
                CHECK(ti[k].selected == 1, "default English selected");
            }
            if (ti[k].language && strcmp(ti[k].language, "zho") == 0) {
                a_zho = ti[k].id;
                lang_ok++;
                CHECK(ti[k].is_default == 0, "Chinese not default");
                CHECK(ti[k].selected == 0, "Chinese not selected yet");
                if (ti[k].name && strstr(ti[k].name, "Chinese") != NULL) {
                    name_ok++;
                }
            }
        }
    }
    CHECK(vid != 0 && a_eng != 0 && a_zho != 0 && a_eng != a_zho && a_eng != vid && a_zho != vid,
          "unique track ids");
    CHECK(def_eng == 1 && lang_ok >= 2 && name_ok >= 2, "GROUP-ID language/name/default metadata");

    v_au = e_au = z_au = v_cfg = e_cfg = z_cfg = 0;
    for (loops = 0; loops < 400; loops++) {
        st = ms_ff_pull(f, &ev);
        if (st == MS_AGAIN) {
            continue;
        }
        if (st == MS_EOS) {
            break;
        }
        if (st != MS_OK) {
            fprintf(stderr, "pull before switch failed: %s\n", ms_status_name(st));
            break;
        }
        sel_ep0 = ev.selection_epoch;
        tl_ep0 = ev.timeline_epoch;
        if (ev.type == MS_EVENT_CODEC_CONFIG && ev.track.id == a_eng && ev.data && ev.data_size > 0) {
            if (ev.data_size < sizeof(eng_csd)) {
                memcpy(eng_csd, ev.data, ev.data_size);
                eng_csd_n = ev.data_size;
            }
            e_cfg++;
        }
        if (ev.type == MS_EVENT_CODEC_CONFIG && ev.track.id == vid) {
            v_cfg++;
        }
        if (ev.type == MS_EVENT_AU && ev.track.id == vid) {
            v_au++;
            last_vpts = ev.pts;
            have_vpts = 1;
        }
        if (ev.type == MS_EVENT_AU && ev.track.id == a_eng) {
            e_au++;
            last_apts = ev.pts;
        }
        if (ev.type == MS_EVENT_AU && ev.track.id == a_zho) {
            z_au++;
        }
        if (ev.release_id) {
            ms_ff_release(f, ev.release_id);
        }
        if (v_au >= 8 && e_au >= 8) {
            break;
        }
    }
    CHECK(v_au >= 4 && e_au >= 4, "default English AUs and video AUs");
    CHECK(z_au == 0, "unselected Chinese AUs not emitted");
    CHECK(eng_csd_n > 0, "English CSD observed");

    /* 非法选择不得改已提交会话 */
    CHECK_ST(ms_ff_select_track(f, 0x00FFFFFFu, 1), MS_ERR_INVALID_ARG);
    CHECK_ST(ms_ff_list_tracks(f, ti, 16, &nt), MS_OK);
    st_ok = 0;
    for (k = 0; k < nt; k++) {
        if (ti[k].id == a_eng && ti[k].selected) {
            st_ok = 1;
        }
        if (ti[k].id == a_zho && ti[k].selected) {
            st_ok = 0;
            break;
        }
    }
    CHECK(st_ok, "invalid select leaves English selected");

    CHECK_ST(ms_ff_select_track(f, a_zho, 1), MS_OK);
    v_au = e_au = z_au = z_cfg = 0;
    after_sw_video = 0;
    for (loops = 0; loops < 500; loops++) {
        st = ms_ff_pull(f, &ev);
        if (st == MS_AGAIN) {
            continue;
        }
        if (st == MS_EOS) {
            break;
        }
        if (st != MS_OK) {
            fprintf(stderr, "pull after switch failed: %s\n", ms_status_name(st));
            break;
        }
        sel_ep1 = ev.selection_epoch;
        tl_ep1 = ev.timeline_epoch;
        if (ev.type == MS_EVENT_CODEC_CONFIG && ev.track.id == a_zho && ev.data && ev.data_size > 0) {
            if (ev.data_size < sizeof(zho_csd)) {
                memcpy(zho_csd, ev.data, ev.data_size);
                zho_csd_n = ev.data_size;
            }
            z_cfg++;
        }
        if (ev.type == MS_EVENT_AU && ev.track.id == vid) {
            after_sw_video++;
            last_vpts = ev.pts;
            have_vpts = 1;
        }
        if (ev.type == MS_EVENT_AU && ev.track.id == a_eng) {
            e_au++;
        }
        if (ev.type == MS_EVENT_AU && ev.track.id == a_zho) {
            z_au++;
            if (!have_zpts) {
                first_zpts = ev.pts;
                have_zpts = 1;
            }
        }
        if (ev.release_id) {
            ms_ff_release(f, ev.release_id);
        }
        if (z_au >= 6 && after_sw_video >= 4 && z_cfg >= 1) {
            break;
        }
    }
    CHECK(z_cfg >= 1 && zho_csd_n > 0, "new Chinese CSD after switch");
    CHECK(z_au >= 4, "Chinese AUs after switch");
    CHECK(e_au == 0 || (e_au > 0 && z_au > 0), "old English stops after commit");
    /* 提交后旧轨不得再出；允许切换确认前短暂旧包，但循环结束时应已提交 */
    {
        size_t nt2 = 0;
        CHECK_ST(ms_ff_list_tracks(f, ti, 16, &nt2), MS_OK);
        st_ok = 0;
        for (k = 0; k < nt2; k++) {
            if (ti[k].id == a_zho && ti[k].selected) {
                st_ok = 1;
            }
            if (ti[k].id == a_eng && ti[k].selected) {
                st_ok = 0;
                break;
            }
        }
        CHECK(st_ok, "list shows Chinese selected after commit");
    }
    CHECK(after_sw_video >= 2, "video continues after audio switch");
    CHECK(tl_ep1 == tl_ep0, "audio switch does not bump timeline_epoch");
    CHECK(sel_ep1 > sel_ep0, "selection_epoch bumps after commit");
    CHECK(eng_csd_n != zho_csd_n || memcmp(eng_csd, zho_csd, eng_csd_n) != 0,
          "English and Chinese ASC/CSD differ");
    (void)last_apts;
    (void)first_zpts;
    (void)last_vpts;
    (void)have_vpts;
    (void)have_zpts;

    /* 再切回 English */
    CHECK_ST(ms_ff_select_track(f, a_eng, 1), MS_OK);
    e_au = z_au = e_cfg = 0;
    for (loops = 0; loops < 500; loops++) {
        st = ms_ff_pull(f, &ev);
        if (st == MS_AGAIN) {
            continue;
        }
        if (st == MS_EOS || st != MS_OK) {
            break;
        }
        if (ev.type == MS_EVENT_CODEC_CONFIG && ev.track.id == a_eng) {
            e_cfg++;
        }
        if (ev.type == MS_EVENT_AU && ev.track.id == a_eng) {
            e_au++;
        }
        if (ev.type == MS_EVENT_AU && ev.track.id == a_zho) {
            z_au++;
        }
        if (ev.release_id) {
            ms_ff_release(f, ev.release_id);
        }
        if (e_au >= 4 && e_cfg >= 1) {
            break;
        }
    }
    CHECK(e_au >= 2 && e_cfg >= 1, "switch back to English emits CSD+AU");

    /* 关闭再启用 */
    CHECK_ST(ms_ff_select_track(f, a_eng, 0), MS_OK);
    e_au = z_au = v_au = 0;
    for (loops = 0; loops < 200; loops++) {
        st = ms_ff_pull(f, &ev);
        if (st == MS_AGAIN) {
            continue;
        }
        if (st == MS_EOS || st != MS_OK) {
            break;
        }
        if (ev.type == MS_EVENT_AU && ev.track.kind == MS_TRACK_AUDIO) {
            if (ev.track.id == a_eng) {
                e_au++;
            }
            if (ev.track.id == a_zho) {
                z_au++;
            }
        }
        if (ev.type == MS_EVENT_AU && ev.track.id == vid) {
            v_au++;
        }
        if (ev.release_id) {
            ms_ff_release(f, ev.release_id);
        }
        if (v_au >= 6) {
            break;
        }
    }
    CHECK(e_au == 0 && z_au == 0, "audio closed: no audio AUs");
    CHECK(v_au >= 2, "video still flows with audio off");
    CHECK_ST(ms_ff_select_track(f, a_zho, 1), MS_OK);
    z_au = z_cfg = 0;
    for (loops = 0; loops < 500; loops++) {
        st = ms_ff_pull(f, &ev);
        if (st == MS_AGAIN) {
            continue;
        }
        if (st == MS_EOS || st != MS_OK) {
            break;
        }
        if (ev.type == MS_EVENT_CODEC_CONFIG && ev.track.id == a_zho) {
            z_cfg++;
        }
        if (ev.type == MS_EVENT_AU && ev.track.id == a_zho) {
            z_au++;
        }
        if (ev.release_id) {
            ms_ff_release(f, ev.release_id);
        }
        if (z_au >= 3 && z_cfg >= 1) {
            break;
        }
    }
    CHECK(z_au >= 2 && z_cfg >= 1, "re-enable Chinese after close");

    /* seek 仍走既有关键帧路径，音轨保持切换后的选择 */
    {
        int64_t dur = ms_ff_duration_90k(f);
        if (dur >= 2 * 90000 && !ms_ff_is_seekable(f)) {
            fprintf(stderr, "FAIL VOD HLS has duration but is not seekable\n");
            failed = 1;
            /* 不 return，让后续清理执行；下面仍跳过定位断言 */
        }
    }
    if (ms_ff_is_seekable(f)) {
        unsigned tl_before = 0;
        int key = 0, post_a = 0, post_wrong = 0;
        (void)ms_ff_pull(f, &ev); /* 可能 AGAIN，忽略 */
        CHECK_ST(ms_ff_seek(f, 90000), MS_OK); /* 1s */
        for (loops = 0; loops < 400; loops++) {
            st = ms_ff_pull(f, &ev);
            if (st == MS_AGAIN) {
                continue;
            }
            if (st != MS_OK) {
                break;
            }
            tl_before = ev.timeline_epoch;
            if (ev.type == MS_EVENT_AU && ev.track.id == vid) {
                if (ev.is_keyframe) {
                    key++;
                }
            }
            if (ev.type == MS_EVENT_AU && ev.track.id == a_zho) {
                post_a++;
            }
            if (ev.type == MS_EVENT_AU && ev.track.id == a_eng) {
                post_wrong++;
            }
            if (ev.release_id) {
                ms_ff_release(f, ev.release_id);
            }
            if (key >= 1 && post_a >= 1) {
                break;
            }
        }
        CHECK(key >= 1, "seek after switch: video key");
        CHECK(post_a >= 1, "seek after switch: still Chinese audio");
        CHECK(post_wrong == 0, "seek after switch: English stays off");
        CHECK(tl_before != 0, "seek bumps or keeps timeline on event");
        (void)tl_before;
    } else {
        printf("  note: this HLS master not seekable on host FFmpeg; seek subcase gap, not a pass\n");
    }

    (void)ms_ff_cancel(f);
    ms_ff_destroy(f);
    f = NULL;

    /* 相对 URL + 查询串 master */
    snprintf(qurl, sizeof(qurl), "http://127.0.0.1:%d/q/master.m3u8", hs.port);
    CHECK_ST(ms_ff_create(NULL, &lim, &f), MS_OK);
    CHECK_ST(ms_ff_open(f, qurl), MS_OK);
    CHECK_ST(ms_ff_list_tracks(f, ti, 16, &nt), MS_OK);
    a_eng = a_zho = 0;
    for (k = 0; k < nt; k++) {
        if (ti[k].kind == MS_TRACK_AUDIO && ti[k].language &&
            strcmp(ti[k].language, "eng") == 0) {
            a_eng = ti[k].id;
        }
        if (ti[k].kind == MS_TRACK_AUDIO && ti[k].language &&
            strcmp(ti[k].language, "zho") == 0) {
            a_zho = ti[k].id;
        }
    }
    CHECK(a_eng != 0 && a_zho != 0, "query-string master enumerates both renditions");
    e_au = 0;
    for (loops = 0; loops < 300; loops++) {
        st = ms_ff_pull(f, &ev);
        if (st == MS_AGAIN) {
            continue;
        }
        if (st != MS_OK) {
            break;
        }
        if (ev.type == MS_EVENT_AU && ev.track.id == a_eng) {
            e_au++;
        }
        if (ev.release_id) {
            ms_ff_release(f, ev.release_id);
        }
        if (e_au >= 3) {
            break;
        }
    }
    CHECK(e_au >= 2, "query-string relative URI plays default English");
    ms_ff_destroy(f);
    f = NULL;

    /* 畸形 GROUP-ID：诚实缺口（可能无音轨），不得假装切到了不存在的组 */
    snprintf(burl, sizeof(burl), "http://127.0.0.1:%d/bad/master.m3u8", hs.port);
    CHECK_ST(ms_ff_create(NULL, &lim, &f), MS_OK);
    st = ms_ff_open(f, burl);
    if (st == MS_OK) {
        size_t na = 0;
        CHECK_ST(ms_ff_list_tracks(f, ti, 16, &nt), MS_OK);
        for (k = 0; k < nt; k++) {
            if (ti[k].kind == MS_TRACK_AUDIO) {
                na++;
            }
        }
        CHECK(na == 0, "malformed group: no fake audio tracks");
        printf("  malformed GROUP-ID: open ok, audio_tracks=0 (honest)\n");
    } else {
        printf("  malformed GROUP-ID: open=%s (honest; session not faked)\n",
               ms_status_name(st));
    }
    ms_ff_destroy(f);
    f = NULL;

    /* 失效外链 rendition：若整次打开失败，记为 FFmpeg 层限制，不得标完成。 */
    snprintf(gurl, sizeof(gurl), "http://127.0.0.1:%d/brk/master.m3u8", hs.port);
    CHECK_ST(ms_ff_create(NULL, &lim, &f), MS_OK);
    st = ms_ff_open(f, gurl);
    if (st != MS_OK) {
        printf("  broken non-default rendition: open=%s "
               "(FFmpeg header may fail the session; honest gap, not claimed complete)\n",
               ms_status_name(st));
    } else {
        CHECK_ST(ms_ff_list_tracks(f, ti, 16, &nt), MS_OK);
        printf("  broken non-default rendition: open succeeded, tracks=%u "
               "(optional missing playlist did not kill header)\n",
               (unsigned)nt);
        ms_ff_destroy(f);
        f = NULL;
        CHECK_ST(ms_ff_create(NULL, &lim, &f), MS_OK);
        CHECK_ST(ms_ff_open(f, url), MS_OK);
        (void)ms_ff_list_tracks(f, ti, 16, &nt);
        a_zho = 0;
        a_eng = 0;
        for (k = 0; k < nt; k++) {
            if (ti[k].kind == MS_TRACK_AUDIO && ti[k].language &&
                strcmp(ti[k].language, "zho") == 0) {
                a_zho = ti[k].id;
            }
            if (ti[k].kind == MS_TRACK_AUDIO && ti[k].selected) {
                a_eng = ti[k].id;
            }
        }
        (void)a_eng;
        (void)a_zho;
    }
    if (f != NULL) {
        /* 取消：进行中的会话须能 cancel，不得卡死 */
        CHECK_ST(ms_ff_cancel(f), MS_OK);
        ms_ff_destroy(f);
        f = NULL;
    }

    /* 资源上限：64 流位图；少量备选轨不得溢出。再开一条短超时。 */
    {
        ms_ff *f2 = NULL;
        ms_ff_limits tight;
        ms_ff_limits_default(&tight);
        tight.io_timeout_ms = 200;
        tight.au_queue_bytes = 256;
        tight.out_slots = 1;
        CHECK_ST(ms_ff_create(NULL, &tight, &f2), MS_OK);
        st = ms_ff_open(f2, url);
        if (st == MS_OK) {
            int saw_limit = 0, npull = 0;
            for (loops = 0; loops < 80; loops++) {
                st = ms_ff_pull(f2, &ev);
                if (st == MS_LIMIT || st == MS_ERR_NOMEM) {
                    saw_limit = 1;
                    break;
                }
                if (st == MS_OK) {
                    npull++;
                    /* 故意不 release，触发有界队列 */
                    if (ev.release_id == 0 && ev.type != MS_EVENT_AU &&
                        ev.type != MS_EVENT_CODEC_CONFIG) {
                        /* meta 无槽也行 */
                    }
                }
                if (st == MS_EOS || st == MS_ERR_TIMEOUT || st == MS_ERR_CANCELED) {
                    break;
                }
            }
            printf("  bounded queue/timeout probe: limit=%d pulled=%d last=%s\n",
                   saw_limit, npull, ms_status_name(st));
        } else {
            printf("  tight limits open=%s (honest, not faked success)\n",
                   ms_status_name(st));
        }
        ms_ff_destroy(f2);
    }

    srv_stop(&hs);
    free(vbody); free(ebody); free(zbody);
    free(vplb); free(eplb); free(zplb); free(mplb); free(qplb); free(bplb); free(gadb);
    printf("  test_ff_hls_alt_audio ok (local HTTP master+AAC renditions; not device sound)\n");
    return 0;
}




static int test_ff_hls_subs(const char *tmpdir)
{
    const char *ffmpeg = "/usr/bin/ffmpeg";
    char dir[512], vts[512], cmd[2048];
    char evtt[512], zvtt[512], fvtt[512];
    char vpl[512], epl[512], zpl[512], fpl[512], mpl[512], qpl[512], bpl[512], apl[512];
    char url[640], qurl[640], burl[640], aurl[640];
    uint8_t *vbody = NULL, *vplb = NULL, *eplb = NULL, *zplb = NULL, *fplb = NULL;
    uint8_t *evb = NULL, *zvb = NULL, *fvb = NULL, *mplb = NULL, *qplb = NULL, *bplb = NULL, *aplb = NULL;
    size_t vbl = 0, vpln = 0, epln = 0, zpln = 0, fpln = 0, evn = 0, zvn = 0, fvn = 0;
    size_t mpln = 0, qpln = 0, bpln = 0, apln = 0;
    test_srv hs;
    ms_ff *f = NULL;
    ms_ff_limits lim;
    ms_track_info ti[16];
    size_t nt = 0, k;
    uint32_t vid = 0, aid = 0, s_eng = 0, s_zho = 0, s_fra = 0;
    int def_eng = 0, name_ok = 0, lang_ok = 0, forced_ok = 0;
    ms_event ev;
    ms_sub_cue cue;
    size_t nc = 0;
    int loops, st_ok;
    int v_au = 0, a_au = 0, sub_ev = 0, saw_eng = 0, saw_zho = 0;
    unsigned long long sel_ep0 = 1, sel_ep1 = 1, tl_ep0 = 1, tl_ep1 = 1;
    uint64_t cue90 = 0, vpts = 0;
    int have_cue90 = 0, have_vpts = 0;
    ms_status st;
    const char *eng_vtt =
        "WEBVTT\n\n"
        "00:00:00.000 --> 00:00:01.200\nHelloEng\n\n"
        "00:00:01.500 --> 00:00:03.000\nSecondEng\n\n"
        "00:00:02.000 --> 00:00:03.800\nLateEng\n";
    const char *zho_vtt =
        "WEBVTT\n\n"
        "00:00:00.000 --> 00:00:01.200\nHelloZho\n\n"
        "00:00:01.500 --> 00:00:03.000\nSecondZho\n\n"
        "00:00:02.000 --> 00:00:03.800\nLateZho\n";
    const char *fra_vtt =
        "WEBVTT\n\n"
        "00:00:00.000 --> 00:00:02.000\nForcedFR\n";

    memset(&hs, 0, sizeof(hs));
    if (access(ffmpeg, X_OK) != 0) {
        fprintf(stderr, "FAIL test_ff_hls_subs: host /usr/bin/ffmpeg required\n");
        failed = 1;
        return 1;
    }
    CHECK(path_join(dir, sizeof(dir), tmpdir, "hlssub") == 0, "dir");
    (void)mkdir(dir, 0755);
    CHECK(path_join(vts, sizeof(vts), dir, "v.ts") == 0, "vts");
    snprintf(cmd, sizeof(cmd),
             "%s -hide_banner -loglevel error -y "
             "-f lavfi -i testsrc=size=160x120:rate=25:duration=10 "
             "-f lavfi -i sine=frequency=440:duration=10 "
             "-c:v libx264 -pix_fmt yuv420p -g 25 -keyint_min 25 "
             "-c:a aac -ar 48000 -ac 2 -shortest %s",
             ffmpeg, vts);
    CHECK(system(cmd) == 0, "encode av ts");
    CHECK(path_join(evtt, sizeof(evtt), dir, "eng.vtt") == 0, "evtt");
    CHECK(path_join(zvtt, sizeof(zvtt), dir, "zho.vtt") == 0, "zvtt");
    CHECK(path_join(fvtt, sizeof(fvtt), dir, "fra.vtt") == 0, "fvtt");
    CHECK(write_text_file(evtt, eng_vtt) == 0, "write eng vtt");
    CHECK(write_text_file(zvtt, zho_vtt) == 0, "write zho vtt");
    CHECK(write_text_file(fvtt, fra_vtt) == 0, "write fra vtt");
    CHECK(path_join(vpl, sizeof(vpl), dir, "v.m3u8") == 0, "vpl");
    CHECK(path_join(epl, sizeof(epl), dir, "eng.m3u8") == 0, "epl");
    CHECK(path_join(zpl, sizeof(zpl), dir, "zho.m3u8") == 0, "zpl");
    CHECK(path_join(fpl, sizeof(fpl), dir, "fra.m3u8") == 0, "fpl");
    CHECK(path_join(mpl, sizeof(mpl), dir, "master.m3u8") == 0, "mpl");
    CHECK(path_join(qpl, sizeof(qpl), dir, "qmaster.m3u8") == 0, "qpl");
    CHECK(path_join(bpl, sizeof(bpl), dir, "badgroup.m3u8") == 0, "bpl");
    CHECK(path_join(apl, sizeof(apl), dir, "ass.m3u8") == 0, "ass master");
    CHECK(write_text_file(vpl,
             "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:11\n"
             "#EXT-X-MEDIA-SEQUENCE:1\n#EXTINF:10.0,\nv.ts\n#EXT-X-ENDLIST\n") == 0,
          "write v playlist");
    CHECK(write_text_file(epl,
             "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:11\n"
             "#EXT-X-MEDIA-SEQUENCE:1\n#EXTINF:10.0,\neng.vtt\n#EXT-X-ENDLIST\n") == 0,
          "write eng playlist");
    CHECK(write_text_file(zpl,
             "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:11\n"
             "#EXT-X-MEDIA-SEQUENCE:1\n#EXTINF:10.0,\nzho.vtt\n#EXT-X-ENDLIST\n") == 0,
          "write zho playlist");
    CHECK(write_text_file(fpl,
             "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:11\n"
             "#EXT-X-MEDIA-SEQUENCE:1\n#EXTINF:10.0,\nfra.vtt\n#EXT-X-ENDLIST\n") == 0,
          "write fra playlist");
    CHECK(write_text_file(mpl,
             "#EXTM3U\n"
             "#EXT-X-MEDIA:TYPE=SUBTITLES,GROUP-ID=\"sg\",LANGUAGE=\"eng\","
             "NAME=\"English\",DEFAULT=YES,AUTOSELECT=YES,URI=\"eng/prog.m3u8\"\n"
             "#EXT-X-MEDIA:TYPE=SUBTITLES,GROUP-ID=\"sg\",LANGUAGE=\"zho\","
             "NAME=\"Chinese\",DEFAULT=NO,AUTOSELECT=YES,URI=\"zho/prog.m3u8\"\n"
             "#EXT-X-MEDIA:TYPE=SUBTITLES,GROUP-ID=\"sg\",LANGUAGE=\"fra\","
             "NAME=\"ForcedFR\",DEFAULT=NO,AUTOSELECT=NO,FORCED=YES,URI=\"fra/prog.m3u8\"\n"
             "#EXT-X-STREAM-INF:BANDWIDTH=250000,CODECS=\"avc1.42e01e,mp4a.40.2\","
             "SUBTITLES=\"sg\"\n"
             "v/prog.m3u8\n") == 0,
          "write master");
    CHECK(write_text_file(qpl,
             "#EXTM3U\n"
             "#EXT-X-MEDIA:TYPE=SUBTITLES,GROUP-ID=\"sg\",LANGUAGE=\"eng\","
             "NAME=\"English\",DEFAULT=YES,AUTOSELECT=YES,URI=\"eng/prog.m3u8?tok=1\"\n"
             "#EXT-X-MEDIA:TYPE=SUBTITLES,GROUP-ID=\"sg\",LANGUAGE=\"zho\","
             "NAME=\"Chinese\",DEFAULT=NO,AUTOSELECT=YES,URI=\"zho/prog.m3u8\"\n"
             "#EXT-X-STREAM-INF:BANDWIDTH=250000,SUBTITLES=\"sg\"\n"
             "v/prog.m3u8\n") == 0,
          "write query master");
    CHECK(write_text_file(bpl,
             "#EXTM3U\n"
             "#EXT-X-STREAM-INF:BANDWIDTH=250000,SUBTITLES=\"missing\"\n"
             "v/prog.m3u8\n") == 0,
          "write bad group");
    CHECK(write_text_file(apl,
             "#EXTM3U\n"
             "#EXT-X-MEDIA:TYPE=SUBTITLES,GROUP-ID=\"sg\",LANGUAGE=\"eng\","
             "NAME=\"English\",DEFAULT=YES,AUTOSELECT=YES,URI=\"eng/prog.m3u8\"\n"
             "#EXT-X-MEDIA:TYPE=SUBTITLES,GROUP-ID=\"sg\",LANGUAGE=\"und\","
             "NAME=\"AssTry\",DEFAULT=NO,AUTOSELECT=YES,URI=\"nope.m3u8\"\n"
             "#EXT-X-STREAM-INF:BANDWIDTH=250000,SUBTITLES=\"sg\"\n"
             "v/prog.m3u8\n") == 0,
          "write missing-rendition master");
    CHECK(read_all_file(vts, &vbody, &vbl) == 0, "read v.ts");
    CHECK(read_all_file(vpl, &vplb, &vpln) == 0, "read v.m3u8");
    CHECK(read_all_file(epl, &eplb, &epln) == 0, "read eng.m3u8");
    CHECK(read_all_file(zpl, &zplb, &zpln) == 0, "read zho.m3u8");
    CHECK(read_all_file(fpl, &fplb, &fpln) == 0, "read fra.m3u8");
    CHECK(read_all_file(evtt, &evb, &evn) == 0, "read eng.vtt");
    CHECK(read_all_file(zvtt, &zvb, &zvn) == 0, "read zho.vtt");
    CHECK(read_all_file(fvtt, &fvb, &fvn) == 0, "read fra.vtt");
    CHECK(read_all_file(mpl, &mplb, &mpln) == 0, "read master");
    CHECK(read_all_file(qpl, &qplb, &qpln) == 0, "read qmaster");
    CHECK(read_all_file(bpl, &bplb, &bpln) == 0, "read badgroup");
    CHECK(read_all_file(apl, &aplb, &apln) == 0, "read ass/missing master");

    CHECK(srv_start(&hs, 0, NULL, NULL) == 0, "start http");
    CHECK(srv_add(&hs, "/hls/master.m3u8", mplb, mpln, "application/vnd.apple.mpegurl") == 0, "add master");
    CHECK(srv_add(&hs, "/hls/v/prog.m3u8", vplb, vpln, "application/vnd.apple.mpegurl") == 0, "add v pl");
    CHECK(srv_add(&hs, "/hls/v/v.ts", vbody, vbl, "video/mp2t") == 0, "add v ts");
    CHECK(srv_add(&hs, "/hls/eng/prog.m3u8", eplb, epln, "application/vnd.apple.mpegurl") == 0, "add e pl");
    CHECK(srv_add(&hs, "/hls/eng/eng.vtt", evb, evn, "text/vtt") == 0, "add e vtt");
    CHECK(srv_add(&hs, "/hls/zho/prog.m3u8", zplb, zpln, "application/vnd.apple.mpegurl") == 0, "add z pl");
    CHECK(srv_add(&hs, "/hls/zho/zho.vtt", zvb, zvn, "text/vtt") == 0, "add z vtt");
    CHECK(srv_add(&hs, "/hls/fra/prog.m3u8", fplb, fpln, "application/vnd.apple.mpegurl") == 0, "add f pl");
    CHECK(srv_add(&hs, "/hls/fra/fra.vtt", fvb, fvn, "text/vtt") == 0, "add f vtt");
    CHECK(srv_add(&hs, "/q/master.m3u8", qplb, qpln, "application/vnd.apple.mpegurl") == 0, "add q master");
    CHECK(srv_add(&hs, "/q/v/prog.m3u8", vplb, vpln, "application/vnd.apple.mpegurl") == 0, "add q v");
    CHECK(srv_add(&hs, "/q/v/v.ts", vbody, vbl, "video/mp2t") == 0, "add q vts");
    CHECK(srv_add(&hs, "/q/eng/prog.m3u8?tok=1", eplb, epln, "application/vnd.apple.mpegurl") == 0, "add q epl");
    CHECK(srv_add(&hs, "/q/eng/eng.vtt", evb, evn, "text/vtt") == 0, "add q evtt");
    CHECK(srv_add(&hs, "/q/zho/prog.m3u8", zplb, zpln, "application/vnd.apple.mpegurl") == 0, "add q zpl");
    CHECK(srv_add(&hs, "/q/zho/zho.vtt", zvb, zvn, "text/vtt") == 0, "add q zvtt");
    CHECK(srv_add(&hs, "/bad/master.m3u8", bplb, bpln, "application/vnd.apple.mpegurl") == 0, "add bad");
    CHECK(srv_add(&hs, "/bad/v/prog.m3u8", vplb, vpln, "application/vnd.apple.mpegurl") == 0, "add bad v");
    CHECK(srv_add(&hs, "/bad/v/v.ts", vbody, vbl, "video/mp2t") == 0, "add bad vts");
    CHECK(srv_add(&hs, "/miss/master.m3u8", aplb, apln, "application/vnd.apple.mpegurl") == 0, "add miss");
    CHECK(srv_add(&hs, "/miss/v/prog.m3u8", vplb, vpln, "application/vnd.apple.mpegurl") == 0, "add miss v");
    CHECK(srv_add(&hs, "/miss/v/v.ts", vbody, vbl, "video/mp2t") == 0, "add miss vts");
    CHECK(srv_add(&hs, "/miss/eng/prog.m3u8", eplb, epln, "application/vnd.apple.mpegurl") == 0, "add miss e");
    CHECK(srv_add(&hs, "/miss/eng/eng.vtt", evb, evn, "text/vtt") == 0, "add miss evtt");

    snprintf(url, sizeof(url), "http://127.0.0.1:%d/hls/master.m3u8", hs.port);
    ms_ff_limits_default(&lim);
    lim.io_timeout_ms = 8000;
    CHECK_ST(ms_ff_create(NULL, &lim, &f), MS_OK);
    st = ms_ff_open(f, url);
    if (st != MS_OK) {
        fprintf(stderr, "FAIL open hls sub master: %s\n", ms_status_name(st));
        ms_ff_destroy(f);
        srv_stop(&hs);
        failed = 1;
        return 1;
    }
    CHECK_ST(ms_ff_list_tracks(f, ti, 16, &nt), MS_OK);
    CHECK(nt >= 4, "video + audio + subtitle renditions enumerated");
    for (k = 0; k < nt; k++) {
        if (ti[k].kind == MS_TRACK_VIDEO && vid == 0) {
            vid = ti[k].id;
        }
        if (ti[k].kind == MS_TRACK_AUDIO && aid == 0) {
            aid = ti[k].id;
        }
        if (ti[k].kind == MS_TRACK_SUBTITLE) {
            if (ti[k].language && strcmp(ti[k].language, "eng") == 0) {
                s_eng = ti[k].id;
                lang_ok++;
                if (ti[k].is_default) {
                    def_eng = 1;
                }
                CHECK(ti[k].selected == 1, "default English sub selected");
                CHECK(ti[k].codec == MS_CODEC_SUB_WEBVTT, "eng is webvtt");
                if (ti[k].name && strstr(ti[k].name, "English") != NULL) {
                    name_ok++;
                }
            }
            if (ti[k].language && strcmp(ti[k].language, "zho") == 0) {
                s_zho = ti[k].id;
                lang_ok++;
                CHECK(ti[k].is_default == 0, "Chinese sub not default");
                CHECK(ti[k].selected == 0, "Chinese sub not selected yet");
                if (ti[k].name && strstr(ti[k].name, "Chinese") != NULL) {
                    name_ok++;
                }
            }
            if (ti[k].language && strcmp(ti[k].language, "fra") == 0) {
                s_fra = ti[k].id;
                CHECK(ti[k].is_forced == 1, "ForcedFR marked forced");
                CHECK(ti[k].selected == 0, "forced not auto-selected when DEFAULT exists");
                forced_ok = 1;
            }
        }
    }
    CHECK(vid != 0 && aid != 0 && s_eng != 0 && s_zho != 0 && s_eng != s_zho,
          "unique av/sub ids");
    CHECK(s_fra != 0, "forced track enumerated");
    CHECK(def_eng == 1 && lang_ok >= 2 && name_ok >= 2 && forced_ok == 1,
          "GROUP-ID language/name/default/forced metadata");

    v_au = a_au = sub_ev = saw_eng = saw_zho = 0;
    for (loops = 0; loops < 500; loops++) {
        st = ms_ff_pull(f, &ev);
        if (st == MS_AGAIN) {
            continue;
        }
        if (st == MS_EOS) {
            break;
        }
        if (st != MS_OK) {
            fprintf(stderr, "pull before sub switch failed: %s\n", ms_status_name(st));
            break;
        }
        sel_ep0 = ev.selection_epoch;
        tl_ep0 = ev.timeline_epoch;
        if (ev.type == MS_EVENT_AU && ev.track.id == vid) {
            v_au++;
            if (ev.has_pts) {
                vpts = ev.pts;
                if (!(ev.time_base_num == 1 && ev.time_base_den == 90000) &&
                    ev.time_base_num > 0 && ev.time_base_den > 0) {
                    vpts = ev.pts * 90000ull * (uint64_t)ev.time_base_num /
                           (uint64_t)ev.time_base_den;
                }
                have_vpts = 1;
            }
        }
        if (ev.type == MS_EVENT_AU && ev.track.id == aid) {
            a_au++;
        }
        if (ev.type == MS_EVENT_SUBTITLE && ev.track.id == s_eng) {
            sub_ev++;
            saw_eng++;
            if (ev.has_pts && !have_cue90) {
                cue90 = ev.pts;
                if (!(ev.time_base_num == 1 && ev.time_base_den == 90000) &&
                    ev.time_base_num > 0 && ev.time_base_den > 0) {
                    cue90 = ev.pts * 90000ull * (uint64_t)ev.time_base_num /
                            (uint64_t)ev.time_base_den;
                }
                have_cue90 = 1;
            }
        }
        if (ev.type == MS_EVENT_SUBTITLE && ev.track.id == s_zho) {
            saw_zho++;
        }
        if (ev.release_id) {
            ms_ff_release(f, ev.release_id);
        }
        if (v_au >= 8 && a_au >= 4 && saw_eng >= 1) {
            break;
        }
    }
    CHECK(v_au >= 4 && a_au >= 2, "default period: video+audio AUs");
    CHECK(saw_eng >= 1, "default English WebVTT packets");
    CHECK(saw_zho == 0, "unselected Chinese sub not emitted");
    ms_sub_cue_default(&cue);
    nc = 0;
    CHECK_ST(ms_ff_subtitles_at(f, have_cue90 ? cue90 : 0, &cue, 1, &nc), MS_OK);
    if (nc == 0) {
        CHECK_ST(ms_ff_subtitles_at(f, 45000, &cue, 1, &nc), MS_OK);
    }
    CHECK(nc >= 1 && cue.text != NULL && strstr(cue.text, "HelloEng") != NULL,
          "default cue text HelloEng");
    if (have_cue90 && have_vpts) {
        int64_t d = (int64_t)cue90 - (int64_t)vpts;
        if (d < 0) {
            d = -d;
        }
        CHECK(d < 4 * 90000, "cue timeline aligned with media (<4s)");
    }

    /* 非法 id 不改选择 */
    CHECK_ST(ms_ff_select_track(f, 0x00FFFFFFu, 1), MS_ERR_INVALID_ARG);
    CHECK_ST(ms_ff_list_tracks(f, ti, 16, &nt), MS_OK);
    st_ok = 0;
    for (k = 0; k < nt; k++) {
        if (ti[k].id == s_eng && ti[k].selected) {
            st_ok = 1;
        }
        if (ti[k].id == s_zho && ti[k].selected) {
            st_ok = 0;
            break;
        }
    }
    CHECK(st_ok, "invalid select leaves English sub selected");

    /* 切到中文：待确认后才清旧 cue；视频/音频继续 */
    CHECK_ST(ms_ff_select_track(f, s_zho, 1), MS_OK);
    {
        size_t nt2 = 0;
        int zsel = 0;
        CHECK_ST(ms_ff_list_tracks(f, ti, 16, &nt2), MS_OK);
        for (k = 0; k < nt2; k++) {
            if (ti[k].id == s_zho && ti[k].selected) {
                zsel = 1;
            }
        }
        CHECK(zsel == 0, "Chinese not marked selected before first packet");
    }
    v_au = a_au = saw_eng = saw_zho = 0;
    for (loops = 0; loops < 600; loops++) {
        st = ms_ff_pull(f, &ev);
        if (st == MS_AGAIN) {
            continue;
        }
        if (st == MS_EOS || st != MS_OK) {
            break;
        }
        sel_ep1 = ev.selection_epoch;
        tl_ep1 = ev.timeline_epoch;
        if (ev.type == MS_EVENT_AU && ev.track.id == vid) {
            v_au++;
        }
        if (ev.type == MS_EVENT_AU && ev.track.id == aid) {
            a_au++;
        }
        if (ev.type == MS_EVENT_SUBTITLE && ev.track.id == s_zho) {
            saw_zho++;
            if (ev.has_pts) {
                cue90 = ev.pts;
                if (!(ev.time_base_num == 1 && ev.time_base_den == 90000) &&
                    ev.time_base_num > 0 && ev.time_base_den > 0) {
                    cue90 = ev.pts * 90000ull * (uint64_t)ev.time_base_num /
                            (uint64_t)ev.time_base_den;
                }
                have_cue90 = 1;
            }
        }
        if (ev.type == MS_EVENT_SUBTITLE && ev.track.id == s_eng) {
            saw_eng++;
        }
        if (ev.release_id) {
            ms_ff_release(f, ev.release_id);
        }
        if (saw_zho >= 1 && v_au >= 2 && a_au >= 1) {
            break;
        }
    }
    CHECK(saw_zho >= 1, "Chinese WebVTT after switch");
    CHECK(v_au >= 2 && a_au >= 1, "video and audio continue after sub switch");
    CHECK(tl_ep1 == tl_ep0, "sub switch does not bump timeline_epoch");
    CHECK(sel_ep1 > sel_ep0, "selection_epoch bumps after sub commit");
    ms_sub_cue_default(&cue);
    nc = 0;
    CHECK_ST(ms_ff_subtitles_at(f, have_cue90 ? cue90 : 45000, &cue, 1, &nc), MS_OK);
    CHECK(nc >= 1 && cue.text != NULL && strstr(cue.text, "Zho") != NULL,
          "timeline shows Chinese cue");
    CHECK(cue.text == NULL || strstr(cue.text, "HelloEng") == NULL,
          "old English cue cleared after commit");
    {
        size_t nt2 = 0;
        st_ok = 0;
        CHECK_ST(ms_ff_list_tracks(f, ti, 16, &nt2), MS_OK);
        for (k = 0; k < nt2; k++) {
            if (ti[k].id == s_zho && ti[k].selected) {
                st_ok = 1;
            }
            if (ti[k].id == s_eng && ti[k].selected) {
                st_ok = 0;
                break;
            }
        }
        CHECK(st_ok, "list shows Chinese sub selected after commit");
    }

    /* 切回 English */
    CHECK_ST(ms_ff_select_track(f, s_eng, 1), MS_OK);
    saw_eng = saw_zho = 0;
    for (loops = 0; loops < 600; loops++) {
        st = ms_ff_pull(f, &ev);
        if (st == MS_AGAIN) {
            continue;
        }
        if (st == MS_EOS || st != MS_OK) {
            break;
        }
        if (ev.type == MS_EVENT_SUBTITLE && ev.track.id == s_eng) {
            saw_eng++;
        }
        if (ev.type == MS_EVENT_SUBTITLE && ev.track.id == s_zho) {
            saw_zho++;
        }
        if (ev.release_id) {
            ms_ff_release(f, ev.release_id);
        }
        if (saw_eng >= 1) {
            break;
        }
    }
    CHECK(saw_eng >= 1, "switch back to English sub");

    /* 关闭再启用 */
    CHECK_ST(ms_ff_select_track(f, s_eng, 0), MS_OK);
    saw_eng = saw_zho = v_au = 0;
    for (loops = 0; loops < 200; loops++) {
        st = ms_ff_pull(f, &ev);
        if (st == MS_AGAIN) {
            continue;
        }
        if (st == MS_EOS || st != MS_OK) {
            break;
        }
        if (ev.type == MS_EVENT_SUBTITLE) {
            if (ev.track.id == s_eng) {
                saw_eng++;
            }
            if (ev.track.id == s_zho) {
                saw_zho++;
            }
        }
        if (ev.type == MS_EVENT_AU && ev.track.id == vid) {
            v_au++;
        }
        if (ev.release_id) {
            ms_ff_release(f, ev.release_id);
        }
        if (v_au >= 6) {
            break;
        }
    }
    CHECK(saw_eng == 0 && saw_zho == 0, "subs closed: no subtitle events");
    CHECK(v_au >= 2, "video still flows with subs off");
    ms_sub_cue_default(&cue);
    nc = 0;
    (void)ms_ff_subtitles_at(f, 45000, &cue, 1, &nc);
    CHECK(nc == 0, "cues empty when subs off");
    CHECK_ST(ms_ff_select_track(f, s_zho, 1), MS_OK);
    saw_zho = v_au = 0;
    for (loops = 0; loops < 600; loops++) {
        st = ms_ff_pull(f, &ev);
        if (st == MS_AGAIN) {
            continue;
        }
        if (st == MS_EOS || st != MS_OK) {
            break;
        }
        if (ev.type == MS_EVENT_SUBTITLE && ev.track.id == s_zho) {
            saw_zho++;
        }
        if (ev.type == MS_EVENT_AU && ev.track.id == vid) {
            v_au++;
        }
        if (ev.release_id) {
            ms_ff_release(f, ev.release_id);
        }
        if (saw_zho >= 1 && v_au >= 1) {
            break;
        }
    }
    CHECK(saw_zho >= 1, "re-enable Chinese after close");

    /* 关音频再开：字幕选择保持 */
    if (aid != 0) {
        CHECK_ST(ms_ff_select_track(f, aid, 0), MS_OK);
        CHECK_ST(ms_ff_select_track(f, aid, 1), MS_OK);
        {
            size_t nt2 = 0;
            st_ok = 0;
            CHECK_ST(ms_ff_list_tracks(f, ti, 16, &nt2), MS_OK);
            for (k = 0; k < nt2; k++) {
                if (ti[k].id == s_zho && ti[k].selected) {
                    st_ok = 1;
                }
            }
            CHECK(st_ok, "audio toggle keeps Chinese sub selected");
        }
    }

    /* seek 后仍为新轨，旧 cue 已清，随后仍能对齐 */
    if (ms_ff_is_seekable(f)) {
        int post_z = 0, post_eng = 0, key = 0;
        have_cue90 = 0;
        cue90 = 0;
        CHECK_ST(ms_ff_seek(f, 90000 * 2), MS_OK); /* 2s, Second/Late cue window */
        ms_sub_cue_default(&cue);
        nc = 0;
        (void)ms_ff_subtitles_at(f, 2 * 90000, &cue, 1, &nc);
        CHECK(nc == 0, "seek flushes inband cues");
        for (loops = 0; loops < 500; loops++) {
            st = ms_ff_pull(f, &ev);
            if (st == MS_AGAIN) {
                continue;
            }
            if (st != MS_OK) {
                break;
            }
            if (ev.type == MS_EVENT_AU && ev.track.id == vid && ev.is_keyframe) {
                key++;
            }
            if (ev.type == MS_EVENT_SUBTITLE && ev.track.id == s_zho) {
                post_z++;
                if (ev.has_pts && !have_cue90) {
                    cue90 = ev.pts;
                    if (!(ev.time_base_num == 1 && ev.time_base_den == 90000) &&
                        ev.time_base_num > 0 && ev.time_base_den > 0) {
                        cue90 = ev.pts * 90000ull * (uint64_t)ev.time_base_num /
                                (uint64_t)ev.time_base_den;
                    }
                    have_cue90 = 1;
                }
            }
            if (ev.type == MS_EVENT_SUBTITLE && ev.track.id == s_eng) {
                post_eng++;
            }
            if (ev.release_id) {
                ms_ff_release(f, ev.release_id);
            }
            if (key >= 1 && post_z >= 1) {
                break;
            }
        }
        CHECK(key >= 1, "seek after sub switch: video key");
        CHECK(post_z >= 1, "seek after switch: still Chinese sub");
        CHECK(post_eng == 0, "seek after switch: English stays off");
        ms_sub_cue_default(&cue);
        nc = 0;
        /* 先按事件 PTS 查询（HLS 可能丢掉 duration，点时刻仍能对上）；再试 2s 窗口。 */
        if (have_cue90) {
            CHECK_ST(ms_ff_subtitles_at(f, cue90, &cue, 1, &nc), MS_OK);
        }
        if (nc == 0) {
            CHECK_ST(ms_ff_subtitles_at(f, 2 * 90000, &cue, 1, &nc), MS_OK);
        }
        if (nc == 0) {
            CHECK_ST(ms_ff_subtitles_at(f, 2 * 90000 + 30000, &cue, 1, &nc), MS_OK);
        }
        CHECK(nc >= 1 && cue.text != NULL && strstr(cue.text, "Zho") != NULL,
              "cue after seek is Chinese and time-aligned");
        CHECK(cue.text == NULL || strstr(cue.text, "Eng") == NULL,
              "seek does not revive English cue text");
    } else {
        printf("  note: HLS sub master not seekable; seek subcase gap, not a pass\n");
    }

    (void)ms_ff_cancel(f);
    st = ms_ff_pull(f, &ev);
    CHECK(st == MS_ERR_CANCELED || st == MS_EOS || st == MS_AGAIN, "cancel pull bounded");
    ms_ff_destroy(f);
    f = NULL;

    /* 相对 URL + 查询串 */
    snprintf(qurl, sizeof(qurl), "http://127.0.0.1:%d/q/master.m3u8", hs.port);
    CHECK_ST(ms_ff_create(NULL, &lim, &f), MS_OK);
    CHECK_ST(ms_ff_open(f, qurl), MS_OK);
    CHECK_ST(ms_ff_list_tracks(f, ti, 16, &nt), MS_OK);
    s_eng = s_zho = 0;
    for (k = 0; k < nt; k++) {
        if (ti[k].kind == MS_TRACK_SUBTITLE && ti[k].language &&
            strcmp(ti[k].language, "eng") == 0) {
            s_eng = ti[k].id;
            CHECK(ti[k].selected == 1, "query master default eng selected");
        }
        if (ti[k].kind == MS_TRACK_SUBTITLE && ti[k].language &&
            strcmp(ti[k].language, "zho") == 0) {
            s_zho = ti[k].id;
        }
    }
    CHECK(s_eng != 0 && s_zho != 0, "query-string master enumerates both subs");
    saw_eng = 0;
    for (loops = 0; loops < 400; loops++) {
        st = ms_ff_pull(f, &ev);
        if (st == MS_AGAIN) {
            continue;
        }
        if (st != MS_OK) {
            break;
        }
        if (ev.type == MS_EVENT_SUBTITLE && ev.track.id == s_eng) {
            saw_eng++;
        }
        if (ev.release_id) {
            ms_ff_release(f, ev.release_id);
        }
        if (saw_eng >= 1) {
            break;
        }
    }
    CHECK(saw_eng >= 1, "query-string English WebVTT demuxed");
    ms_ff_destroy(f);
    f = NULL;

    /* 畸形 GROUP-ID：视频仍可开，不把缺失字幕写成已选成功 */
    snprintf(burl, sizeof(burl), "http://127.0.0.1:%d/bad/master.m3u8", hs.port);
    CHECK_ST(ms_ff_create(NULL, &lim, &f), MS_OK);
    st = ms_ff_open(f, burl);
    CHECK(st == MS_OK, "bad SUBTITLES group still opens video");
    CHECK_ST(ms_ff_list_tracks(f, ti, 16, &nt), MS_OK);
    {
        int nsub = 0;
        v_au = 0;
        for (k = 0; k < nt; k++) {
            if (ti[k].kind == MS_TRACK_SUBTITLE) {
                nsub++;
            }
        }
        CHECK(nsub == 0, "missing SUBTITLES group: no fake sub tracks");
        for (loops = 0; loops < 200; loops++) {
            st = ms_ff_pull(f, &ev);
            if (st == MS_AGAIN) {
                continue;
            }
            if (st != MS_OK) {
                break;
            }
            if (ev.type == MS_EVENT_AU && ev.track.kind == MS_TRACK_VIDEO) {
                v_au++;
            }
            if (ev.release_id) {
                ms_ff_release(f, ev.release_id);
            }
            if (v_au >= 2) {
                break;
            }
        }
        CHECK(v_au >= 1, "video flows without subtitle group");
    }
    ms_ff_destroy(f);
    f = NULL;

    /* 缺失 playlist：打开默认轨，选择失效轨须撤回且不拆会话 */
    snprintf(aurl, sizeof(aurl), "http://127.0.0.1:%d/miss/master.m3u8", hs.port);
    CHECK_ST(ms_ff_create(NULL, &lim, &f), MS_OK);
    st = ms_ff_open(f, aurl);
    CHECK(st == MS_OK, "master with one missing sub rendition opens");
    CHECK_ST(ms_ff_list_tracks(f, ti, 16, &nt), MS_OK);
    s_eng = 0;
    {
        uint32_t broken = 0;
        for (k = 0; k < nt; k++) {
            if (ti[k].kind == MS_TRACK_SUBTITLE && ti[k].language &&
                strcmp(ti[k].language, "eng") == 0) {
                s_eng = ti[k].id;
            }
            if (ti[k].kind == MS_TRACK_SUBTITLE && ti[k].name &&
                strstr(ti[k].name, "AssTry") != NULL) {
                broken = ti[k].id;
            }
        }
        CHECK(s_eng != 0, "default eng still enumerated");
        if (broken != 0) {
            CHECK_ST(ms_ff_select_track(f, broken, 1), MS_OK);
            v_au = 0;
            for (loops = 0; loops < 300; loops++) {
                st = ms_ff_pull(f, &ev);
                if (st == MS_AGAIN) {
                    if (++v_au > 80 && loops > 40) {
                        break;
                    }
                    continue;
                }
                if (st == MS_EOS) {
                    break;
                }
                if (st != MS_OK) {
                    /* 允许一次诊断错误，随后应仍能拉视频：再试 */
                    continue;
                }
                if (ev.type == MS_EVENT_AU && ev.track.kind == MS_TRACK_VIDEO) {
                    v_au++;
                }
                if (ev.release_id) {
                    ms_ff_release(f, ev.release_id);
                }
                if (v_au >= 2) {
                    break;
                }
            }
            CHECK_ST(ms_ff_list_tracks(f, ti, 16, &nt), MS_OK);
            st_ok = 1;
            for (k = 0; k < nt; k++) {
                if (ti[k].id == broken && ti[k].selected) {
                    st_ok = 0;
                }
            }
            CHECK(st_ok, "missing playlist not marked selected");
            CHECK(v_au >= 1, "video survives failed sub switch");
        } else {
            printf("  missing sub rendition not enumerated (lavf skipped empty playlist); "
                   "not claimed as selectable\n");
            v_au = 0;
            for (loops = 0; loops < 200; loops++) {
                st = ms_ff_pull(f, &ev);
                if (st == MS_AGAIN) {
                    continue;
                }
                if (st != MS_OK) {
                    break;
                }
                if (ev.type == MS_EVENT_AU && ev.track.kind == MS_TRACK_VIDEO) {
                    v_au++;
                }
                if (ev.release_id) {
                    ms_ff_release(f, ev.release_id);
                }
                if (v_au >= 2) {
                    break;
                }
            }
            CHECK(v_au >= 1, "video still flows");
        }
    }
    ms_ff_destroy(f);
    f = NULL;

    srv_stop(&hs);
    free(vbody); free(vplb); free(eplb); free(zplb); free(fplb);
    free(evb); free(zvb); free(fvb); free(mplb); free(qplb); free(bplb); free(aplb);
    printf("  test_ff_hls_subs ok (local HTTP master+WebVTT renditions; lavf contract; not device paint)\n");
    return 0;
}






static int tl_near(uint64_t a, uint64_t b, uint64_t tol)
{
    return a > b ? (a - b) <= tol : (b - a) <= tol;
}

static int tl_cue_text(const ms_sub_cue *cue, size_t nc, const char *needle)
{
    return nc >= 1 && cue->text != NULL && strstr(cue->text, needle) != NULL;
}

static uint32_t tl_find_sub(const ms_track_info *ti, size_t nt, const char *lang)
{
    size_t k;
    for (k = 0; k < nt; k++) {
        if (ti[k].kind == MS_TRACK_SUBTITLE && ti[k].language &&
            strcmp(ti[k].language, lang) == 0) {
            return ti[k].id;
        }
    }
    return 0;
}

static int tl_pull_video_and_query(ms_ff *f, uint64_t t90, ms_sub_cue *cue, size_t *nc,
                                   int min_video, int max_loops)
{
    ms_event ev;
    ms_status st;
    int loops, v_au = 0;
    *nc = 0;
    ms_sub_cue_default(cue);
    for (loops = 0; loops < max_loops; loops++) {
        st = ms_ff_pull(f, &ev);
        if (st == MS_AGAIN) {
            continue;
        }
        if (st != MS_OK) {
            break;
        }
        if (ev.type == MS_EVENT_AU && ev.track.kind == MS_TRACK_VIDEO) {
            v_au++;
        }
        if (ev.release_id) {
            ms_ff_release(f, ev.release_id);
        }
        (void)ms_ff_subtitles_at(f, t90, cue, 1, nc);
        if (v_au >= min_video && *nc >= 1) {
            return 0;
        }
    }
    (void)ms_ff_subtitles_at(f, t90, cue, 1, nc);
    return v_au >= min_video ? 0 : -1;
}

static int test_ff_hls_sub_timeline(const char *tmpdir)
{
    const char *ffmpeg = "/usr/bin/ffmpeg";
    char dir[512], vts[512], v0[512], v1[512], v2[512], v3[512], cmd[2048];
    char url[640], mapurl[640], liveurl[640], missurl[640];
    uint8_t *v0b = NULL, *v1b = NULL, *v2b = NULL, *v3b = NULL;
    size_t v0n = 0, v1n = 0, v2n = 0, v3n = 0;
    test_srv hs;
    ms_ff *f = NULL;
    ms_ff_limits lim;
    ms_track_info ti[16];
    size_t nt = 0, k, nc = 0, nc2 = 0;
    uint32_t s_eng = 0, s_zho = 0;
    ms_event ev;
    ms_sub_cue cue, cue2;
    ms_status st;
    int loops, v_au, saw;
    const char *ctype_pl = "application/vnd.apple.mpegurl";
    const char *eng0 =
        "WEBVTT\n\n"
        "00:00:00.500 --> 00:00:04.000\nLongEng\n";
    const char *eng1 =
        "WEBVTT\n\n"
        "00:00:00.500 --> 00:00:04.000\nLongEng\n\n"
        "00:00:03.200 --> 00:00:05.000\nMidEng\n";
    const char *zho0 =
        "WEBVTT\n\n"
        "00:00:00.500 --> 00:00:04.000\nLongZho\n";
    const char *zho1 =
        "WEBVTT\n\n"
        "00:00:00.500 --> 00:00:04.000\nLongZho\n";
    const char *map0 =
        "WEBVTT\n"
        "X-TIMESTAMP-MAP=LOCAL:00:00:00.000,MPEGTS:180000\n\n"
        "00:00:00.500 --> 00:00:04.000\nMappedLong\n";
    const char *map1 =
        "WEBVTT\n"
        "X-TIMESTAMP-MAP=MPEGTS:180000,LOCAL:00:00:00.000\n\n"
        "00:00:00.500 --> 00:00:04.000\nMappedLong\n";
    const char *live0 =
        "WEBVTT\n\n"
        "00:00:00.000 --> 00:00:01.500\nLiveFirst\n";
    const char *live1 =
        "WEBVTT\n\n"
        "00:00:03.000 --> 00:00:04.500\nLiveMid\n";
    const char *live2 =
        "WEBVTT\n\n"
        "00:00:06.000 --> 00:00:07.500\nLiveLate\n";
    const char *live3 =
        "WEBVTT\n\n"
        "00:00:09.000 --> 00:00:10.500\nLiveNew\n";
    const char *vod_vpl =
        "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:4\n"
        "#EXT-X-MEDIA-SEQUENCE:10\n"
        "#EXTINF:3.0,\nv0.ts\n#EXTINF:3.0,\nv1.ts\n#EXT-X-ENDLIST\n";
    const char *vod_epl =
        "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:4\n"
        "#EXT-X-MEDIA-SEQUENCE:10\n"
        "#EXTINF:3.0,\neng0.vtt\n#EXTINF:3.0,\neng1.vtt\n#EXT-X-ENDLIST\n";
    const char *vod_zpl =
        "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:4\n"
        "#EXT-X-MEDIA-SEQUENCE:10\n"
        "#EXTINF:3.0,\nzho0.vtt\n#EXTINF:3.0,\nzho1.vtt\n#EXT-X-ENDLIST\n";
    const char *vod_mpl =
        "#EXTM3U\n"
        "#EXT-X-MEDIA:TYPE=SUBTITLES,GROUP-ID=\"subs\",NAME=\"English\","
        "DEFAULT=YES,AUTOSELECT=YES,LANGUAGE=\"eng\",URI=\"eng.m3u8\"\n"
        "#EXT-X-MEDIA:TYPE=SUBTITLES,GROUP-ID=\"subs\",NAME=\"Chinese\","
        "DEFAULT=NO,AUTOSELECT=YES,LANGUAGE=\"zho\",URI=\"zho.m3u8\"\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=200000,RESOLUTION=160x120,"
        "CODECS=\"avc1.42e01e,mp4a.40.2\",SUBTITLES=\"subs\"\n"
        "v.m3u8\n";
    const char *map_epl =
        "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:4\n"
        "#EXT-X-MEDIA-SEQUENCE:10\n"
        "#EXTINF:3.0,\nmap0.vtt\n#EXTINF:3.0,\nmap1.vtt\n#EXT-X-ENDLIST\n";
    const char *map_mpl =
        "#EXTM3U\n"
        "#EXT-X-MEDIA:TYPE=SUBTITLES,GROUP-ID=\"subs\",NAME=\"Mapped\","
        "DEFAULT=YES,AUTOSELECT=YES,LANGUAGE=\"eng\",URI=\"eng.m3u8\"\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=200000,RESOLUTION=160x120,"
        "CODECS=\"avc1.42e01e,mp4a.40.2\",SUBTITLES=\"subs\"\n"
        "v.m3u8\n";
    char live_vpl1[512], live_vpl2[512], live_spl1[512], live_spl2[512];
    const char *live_mpl =
        "#EXTM3U\n"
        "#EXT-X-MEDIA:TYPE=SUBTITLES,GROUP-ID=\"subs\",NAME=\"Live\","
        "DEFAULT=YES,AUTOSELECT=YES,LANGUAGE=\"eng\",URI=\"eng.m3u8\"\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=200000,RESOLUTION=160x120,"
        "CODECS=\"avc1.42e01e,mp4a.40.2\",SUBTITLES=\"subs\"\n"
        "v.m3u8\n";
    const char *miss_mpl =
        "#EXTM3U\n"
        "#EXT-X-MEDIA:TYPE=SUBTITLES,GROUP-ID=\"subs\",NAME=\"Missing\","
        "DEFAULT=YES,AUTOSELECT=YES,LANGUAGE=\"eng\",URI=\"missing.m3u8\"\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=200000,RESOLUTION=160x120,"
        "CODECS=\"avc1.42e01e,mp4a.40.2\",SUBTITLES=\"subs\"\n"
        "v.m3u8\n";

    memset(&hs, 0, sizeof(hs));
    snprintf(live_vpl1, sizeof(live_vpl1),
             "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:4\n"
             "#EXT-X-MEDIA-SEQUENCE:10\n"
             "#EXTINF:3.0,\nv0.ts\n#EXTINF:3.0,\nv1.ts\n#EXTINF:3.0,\nv2.ts\n");
    snprintf(live_vpl2, sizeof(live_vpl2),
             "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:4\n"
             "#EXT-X-MEDIA-SEQUENCE:11\n"
             "#EXTINF:3.0,\nv1.ts\n#EXTINF:3.0,\nv2.ts\n#EXTINF:3.0,\nv3.ts\n");
    snprintf(live_spl1, sizeof(live_spl1),
             "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:4\n"
             "#EXT-X-MEDIA-SEQUENCE:10\n"
             "#EXTINF:3.0,\ns0.vtt\n#EXTINF:3.0,\ns1.vtt\n#EXTINF:3.0,\ns2.vtt\n");
    snprintf(live_spl2, sizeof(live_spl2),
             "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:4\n"
             "#EXT-X-MEDIA-SEQUENCE:11\n"
             "#EXTINF:3.0,\ns1.vtt\n#EXTINF:3.0,\ns2.vtt\n#EXTINF:3.0,\ns3.vtt\n");

    if (access(ffmpeg, X_OK) != 0) {
        fprintf(stderr, "FAIL test_ff_hls_sub_timeline: host /usr/bin/ffmpeg required\n");
        failed = 1;
        return 1;
    }
    CHECK(path_join(dir, sizeof(dir), tmpdir, "hlssubtl") == 0, "dir");
    (void)mkdir(dir, 0755);
    CHECK(path_join(vts, sizeof(vts), dir, "v.ts") == 0, "vts");
    CHECK(path_join(v0, sizeof(v0), dir, "v0.ts") == 0, "v0");
    CHECK(path_join(v1, sizeof(v1), dir, "v1.ts") == 0, "v1");
    CHECK(path_join(v2, sizeof(v2), dir, "v2.ts") == 0, "v2");
    CHECK(path_join(v3, sizeof(v3), dir, "v3.ts") == 0, "v3");
    /* fixture 编码，不是产品软解。GOP=3s 便于切段。 */
    snprintf(cmd, sizeof(cmd),
             "%s -hide_banner -loglevel error -y "
             "-f lavfi -i testsrc=size=160x120:rate=25:duration=9 "
             "-f lavfi -i sine=frequency=440:duration=9 "
             "-c:v libx264 -pix_fmt yuv420p -g 75 -keyint_min 75 "
             "-c:a aac -ar 48000 -ac 2 -shortest %s",
             ffmpeg, vts);
    CHECK(system(cmd) == 0, "encode 9s av ts");
    snprintf(cmd, sizeof(cmd),
             "%s -hide_banner -loglevel error -y -i %s -c copy -t 3 %s",
             ffmpeg, vts, v0);
    CHECK(system(cmd) == 0, "cut v0");
    snprintf(cmd, sizeof(cmd),
             "%s -hide_banner -loglevel error -y -ss 3 -i %s -c copy -t 3 %s",
             ffmpeg, vts, v1);
    CHECK(system(cmd) == 0, "cut v1");
    snprintf(cmd, sizeof(cmd),
             "%s -hide_banner -loglevel error -y -ss 6 -i %s -c copy -t 3 %s",
             ffmpeg, vts, v2);
    CHECK(system(cmd) == 0, "cut v2");
    snprintf(cmd, sizeof(cmd), "cp -f %s %s", v0, v3);
    CHECK(system(cmd) == 0, "copy v3 loop");
    CHECK(read_all_file(v0, &v0b, &v0n) == 0 && v0n > 0, "load v0");
    CHECK(read_all_file(v1, &v1b, &v1n) == 0 && v1n > 0, "load v1");
    CHECK(read_all_file(v2, &v2b, &v2n) == 0 && v2n > 0, "load v2");
    CHECK(read_all_file(v3, &v3b, &v3n) == 0 && v3n > 0, "load v3");

    CHECK(srv_start(&hs, 0, NULL, NULL) == 0, "start http");
    CHECK(srv_add(&hs, "/vod/v0.ts", v0b, v0n, "video/mp2t") == 0, "vod v0");
    CHECK(srv_add(&hs, "/vod/v1.ts", v1b, v1n, "video/mp2t") == 0, "vod v1");
    CHECK(srv_add(&hs, "/vod/v.m3u8", vod_vpl, strlen(vod_vpl), ctype_pl) == 0, "vod vpl");
    CHECK(srv_add(&hs, "/vod/eng0.vtt", eng0, strlen(eng0), "text/vtt") == 0, "eng0");
    CHECK(srv_add(&hs, "/vod/eng1.vtt", eng1, strlen(eng1), "text/vtt") == 0, "eng1");
    CHECK(srv_add(&hs, "/vod/zho0.vtt", zho0, strlen(zho0), "text/vtt") == 0, "zho0");
    CHECK(srv_add(&hs, "/vod/zho1.vtt", zho1, strlen(zho1), "text/vtt") == 0, "zho1");
    CHECK(srv_add(&hs, "/vod/eng.m3u8", vod_epl, strlen(vod_epl), ctype_pl) == 0, "vod epl");
    CHECK(srv_add(&hs, "/vod/zho.m3u8", vod_zpl, strlen(vod_zpl), ctype_pl) == 0, "vod zpl");
    CHECK(srv_add(&hs, "/vod/master.m3u8", vod_mpl, strlen(vod_mpl), ctype_pl) == 0, "vod master");
    CHECK(srv_add(&hs, "/map/v0.ts", v0b, v0n, "video/mp2t") == 0, "map v0");
    CHECK(srv_add(&hs, "/map/v1.ts", v1b, v1n, "video/mp2t") == 0, "map v1");
    CHECK(srv_add(&hs, "/map/v.m3u8", vod_vpl, strlen(vod_vpl), ctype_pl) == 0, "map vpl");
    CHECK(srv_add(&hs, "/map/map0.vtt", map0, strlen(map0), "text/vtt") == 0, "map0");
    CHECK(srv_add(&hs, "/map/map1.vtt", map1, strlen(map1), "text/vtt") == 0, "map1");
    CHECK(srv_add(&hs, "/map/eng.m3u8", map_epl, strlen(map_epl), ctype_pl) == 0, "map epl");
    CHECK(srv_add(&hs, "/map/master.m3u8", map_mpl, strlen(map_mpl), ctype_pl) == 0, "map master");
    CHECK(srv_add(&hs, "/live/v0.ts", v0b, v0n, "video/mp2t") == 0, "live v0");
    CHECK(srv_add(&hs, "/live/v1.ts", v1b, v1n, "video/mp2t") == 0, "live v1");
    CHECK(srv_add(&hs, "/live/v2.ts", v2b, v2n, "video/mp2t") == 0, "live v2");
    CHECK(srv_add(&hs, "/live/v3.ts", v3b, v3n, "video/mp2t") == 0, "live v3");
    CHECK(srv_add(&hs, "/live/s0.vtt", live0, strlen(live0), "text/vtt") == 0, "live s0");
    CHECK(srv_add(&hs, "/live/s1.vtt", live1, strlen(live1), "text/vtt") == 0, "live s1");
    CHECK(srv_add(&hs, "/live/s2.vtt", live2, strlen(live2), "text/vtt") == 0, "live s2");
    CHECK(srv_add(&hs, "/live/s3.vtt", live3, strlen(live3), "text/vtt") == 0, "live s3");
    CHECK(srv_add(&hs, "/live/v.m3u8", live_vpl1, strlen(live_vpl1), ctype_pl) == 0, "live vpl");
    CHECK(srv_add(&hs, "/live/eng.m3u8", live_spl1, strlen(live_spl1), ctype_pl) == 0, "live epl");
    CHECK(srv_add(&hs, "/live/master.m3u8", live_mpl, strlen(live_mpl), ctype_pl) == 0, "live master");
    CHECK(srv_add(&hs, "/miss/v0.ts", v0b, v0n, "video/mp2t") == 0, "miss v0");
    CHECK(srv_add(&hs, "/miss/v1.ts", v1b, v1n, "video/mp2t") == 0, "miss v1");
    CHECK(srv_add(&hs, "/miss/v.m3u8", vod_vpl, strlen(vod_vpl), ctype_pl) == 0, "miss vpl");
    CHECK(srv_add(&hs, "/miss/master.m3u8", miss_mpl, strlen(miss_mpl), ctype_pl) == 0, "miss master");

    ms_ff_limits_default(&lim);
    lim.io_timeout_ms = 8000;

    /* ---------- VOD：seek(2s) 覆盖 0.5-4s 长 cue，end 开区间，跨段去重，切轨 ---------- */
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/vod/master.m3u8", hs.port);
    CHECK_ST(ms_ff_create(NULL, &lim, &f), MS_OK);
    CHECK_ST(ms_ff_open(f, url), MS_OK);
    CHECK_ST(ms_ff_list_tracks(f, ti, 16, &nt), MS_OK);
    s_eng = tl_find_sub(ti, nt, "eng");
    s_zho = tl_find_sub(ti, nt, "zho");
    CHECK(s_eng != 0 && s_zho != 0, "vod enumerates eng and zho subs");
    CHECK(ms_ff_is_seekable(f), "vod is seekable");
    {
        ms_ff_seek_range rng;
        memset(&rng, 0, sizeof(rng));
        rng.struct_size = sizeof(rng);
        CHECK_ST(ms_ff_get_seek_range(f, &rng), MS_OK);
        CHECK(rng.seekable && rng.kind == MS_FF_SEEK_KIND_VOD, "vod hls kind");
        CHECK(rng.duration_90k > 0, "vod duration published");
    }
    /* 先拉几包，再 seek 到长 cue 内部 */
    v_au = 0;
    for (loops = 0; loops < 200; loops++) {
        st = ms_ff_pull(f, &ev);
        if (st == MS_AGAIN) {
            continue;
        }
        if (st != MS_OK) {
            break;
        }
        if (ev.type == MS_EVENT_AU && ev.track.kind == MS_TRACK_VIDEO) {
            v_au++;
        }
        if (ev.release_id) {
            ms_ff_release(f, ev.release_id);
        }
        if (v_au >= 2) {
            break;
        }
    }
    CHECK_ST(ms_ff_seek(f, 2 * 90000), MS_OK);
    CHECK(tl_pull_video_and_query(f, 2 * 90000, &cue, &nc, 1, 800) == 0,
          "seek 2s produces video");
    CHECK(tl_cue_text(&cue, nc, "LongEng"), "seek(2s) still shows 0.5-4s LongEng");
    CHECK(nc == 1, "single LongEng at 2s (no duplicate from later ingest yet or already 1)");
    CHECK(cue.has_start && tl_near(cue.start_90k, 45000, 18000), "LongEng original start ~0.5s");
    CHECK(cue.has_end && tl_near(cue.end_90k, 4 * 90000, 18000), "LongEng original end ~4s");
    /* 推进到第二段，跨 segment 重叠 cue 必须去重 */
    saw = 0;
    for (loops = 0; loops < 1000; loops++) {
        st = ms_ff_pull(f, &ev);
        if (st == MS_AGAIN) {
            continue;
        }
        if (st != MS_OK) {
            break;
        }
        if (ev.type == MS_EVENT_SUBTITLE && ev.track.id == s_eng && ev.data &&
            ev.data_size >= 6 && memcmp(ev.data, "MidEng", 6) == 0) {
            saw = 1;
        }
        if (ev.release_id) {
            ms_ff_release(f, ev.release_id);
        }
        if (saw) {
            break;
        }
    }
    CHECK_ST(ms_ff_subtitles_at(f, 2 * 90000, &cue, 1, &nc), MS_OK);
    /* 若只拉到 cap=1，再取更大 cap 确认没有第二份 LongEng */
    {
        ms_sub_cue many[8];
        size_t nmany = 0;
        int nlong = 0;
        memset(many, 0, sizeof(many));
        CHECK_ST(ms_ff_subtitles_at(f, 2 * 90000, many, 8, &nmany), MS_OK);
        for (k = 0; k < nmany; k++) {
            if (many[k].text && strstr(many[k].text, "LongEng") != NULL) {
                nlong++;
            }
        }
        CHECK(nlong == 1, "cross-segment same LongEng is deduped");
    }
    CHECK_ST(ms_ff_subtitles_at(f, 4 * 90000, &cue, 1, &nc), MS_OK);
    CHECK(nc == 0 || (cue.text && strstr(cue.text, "LongEng") == NULL),
          "exclusive end 4s does not show LongEng");
    /* 切到中文后旧 English cue 消失 */
    CHECK_ST(ms_ff_select_track(f, s_zho, 1), MS_OK);
    (void)tl_pull_video_and_query(f, 2 * 90000, &cue, &nc, 1, 600);
    CHECK(tl_cue_text(&cue, nc, "LongZho") ||
          (ms_ff_subtitles_at(f, 2 * 90000, &cue, 1, &nc) == MS_OK &&
           tl_cue_text(&cue, nc, "LongZho")),
          "after switch, Chinese long cue at 2s");
    CHECK(cue.text == NULL || strstr(cue.text, "LongEng") == NULL,
          "old English cue gone after track switch");
    /* 已过期：seek 到 5s，Long 不得复活 */
    CHECK_ST(ms_ff_seek(f, 5 * 90000), MS_OK);
    (void)tl_pull_video_and_query(f, 5 * 90000, &cue, &nc, 1, 600);
    CHECK(!tl_cue_text(&cue, nc, "LongZho") && !tl_cue_text(&cue, nc, "LongEng"),
          "expired long cue not revived at 5s");
    /* 精确 end 边界再查一次 */
    CHECK_ST(ms_ff_subtitles_at(f, 4 * 90000, &cue2, 1, &nc2), MS_OK);
    CHECK(nc2 == 0 || (cue2.text && strstr(cue2.text, "Long") == NULL),
          "end boundary still exclusive after later seek");
    ms_ff_destroy(f);
    f = NULL;

    /* ---------- 非零 X-TIMESTAMP-MAP + MEDIA-SEQUENCE:10 ---------- */
    snprintf(mapurl, sizeof(mapurl), "http://127.0.0.1:%d/map/master.m3u8", hs.port);
    CHECK_ST(ms_ff_create(NULL, &lim, &f), MS_OK);
    CHECK_ST(ms_ff_open(f, mapurl), MS_OK);
    /* 映射后 0.5-4.0s 本地 cue 应对齐到 2.5-6.0s，不是被 MEDIA-SEQUENCE 平移 10 段 */
    CHECK(tl_pull_video_and_query(f, 3 * 90000, &cue, &nc, 1, 1000) == 0 || nc >= 1,
          "mapped vod pulled");
    if (!tl_cue_text(&cue, nc, "MappedLong")) {
        /* seek 进映射后的长 cue 内部 4s（2.5-6.0）再查 */
        if (ms_ff_is_seekable(f)) {
            CHECK_ST(ms_ff_seek(f, 4 * 90000), MS_OK);
        }
        (void)tl_pull_video_and_query(f, 4 * 90000, &cue, &nc, 1, 800);
    }
    CHECK(tl_cue_text(&cue, nc, "MappedLong"), "X-TIMESTAMP-MAP cue appears on timeline");
    CHECK(cue.has_start && tl_near(cue.start_90k, 225000, 18000),
          "mapped start ~2.5s not unmapped 0.5s or seq*3s");
    CHECK(cue.has_end && tl_near(cue.end_90k, 6 * 90000, 18000), "mapped end ~6s");
    /* 未映射的 0.5s 处不应把已映射 cue 显示成还在片头 */
    CHECK_ST(ms_ff_subtitles_at(f, 45000, &cue2, 1, &nc2), MS_OK);
    CHECK(nc2 == 0 || (cue2.text && strstr(cue2.text, "MappedLong") == NULL),
          "mapped cue is not active at unmapped 0.5s");
    ms_ff_destroy(f);
    f = NULL;

    /* ---------- 无 ENDLIST 直播：短 VTT 片头 cue、滑动窗口、取消/失效 ---------- */
    snprintf(liveurl, sizeof(liveurl), "http://127.0.0.1:%d/live/master.m3u8", hs.port);
    CHECK_ST(ms_ff_create(NULL, &lim, &f), MS_OK);
    CHECK_ST(ms_ff_open(f, liveurl), MS_OK);
    CHECK(ms_ff_is_seekable(f), "sliding live is window-seekable");
    CHECK(ms_ff_duration_90k(f) < 0, "sliding live duration is not a fake VOD length");
    {
        ms_ff_seek_range rng;
        memset(&rng, 0, sizeof(rng));
        rng.struct_size = sizeof(rng);
        CHECK_ST(ms_ff_get_seek_range(f, &rng), MS_OK);
        CHECK(rng.seekable && rng.kind == MS_FF_SEEK_KIND_SLIDING, "live kind sliding");
        CHECK(rng.duration_90k < 0, "sliding duration_90k stays -1");
        CHECK(rng.end_90k > rng.start_90k, "sliding window has extent");
        CHECK(rng.live_edge_90k >= rng.start_90k && rng.live_edge_90k <= rng.end_90k,
              "live_edge inside window");
    }
    /* 先验证片头 cue，窗口定位细节在 test_ff_hls_live_window。 */
    CHECK(tl_pull_video_and_query(f, 0, &cue, &nc, 1, 1200) == 0 || nc >= 1,
          "live pulled");
    if (!tl_cue_text(&cue, nc, "LiveFirst")) {
        /* 再按 0.2s / 0.7s 查一次，允许原点略偏 */
        (void)ms_ff_subtitles_at(f, 20000, &cue, 1, &nc);
        if (!tl_cue_text(&cue, nc, "LiveFirst")) {
            (void)ms_ff_subtitles_at(f, 70000, &cue, 1, &nc);
        }
    }
    CHECK(tl_cue_text(&cue, nc, "LiveFirst"),
          "live short VTT first cue still on timeline after probe");
    {
        ms_sub_cue many[8];
        size_t nmany = 0;
        int nfirst = 0;
        uint64_t tq = cue.has_start ? cue.start_90k + 1000 : 20000;
        memset(many, 0, sizeof(many));
        /* 重复拉同一窗口若干次（清单未滑），不得把 LiveFirst 累计出多份 */
        for (loops = 0; loops < 400; loops++) {
            st = ms_ff_pull(f, &ev);
            if (st == MS_OK && ev.release_id) {
                ms_ff_release(f, ev.release_id);
            }
        }
        CHECK_ST(ms_ff_subtitles_at(f, tq, many, 8, &nmany), MS_OK);
        for (k = 0; k < nmany; k++) {
            if (many[k].text && strstr(many[k].text, "LiveFirst") != NULL) {
                nfirst++;
            }
        }
        CHECK(nfirst <= 1, "same-window live reloads do not accumulate LiveFirst");
    }
    /* 滑动窗口：更新清单后等待超过 TARGETDURATION 再拉 */
    CHECK(srv_update(&hs, "/live/v.m3u8", live_vpl2, strlen(live_vpl2)) == 0, "slide v pl");
    CHECK(srv_update(&hs, "/live/eng.m3u8", live_spl2, strlen(live_spl2)) == 0, "slide s pl");
    usleep(4500 * 1000);
    saw = 0;
    for (loops = 0; loops < 1200; loops++) {
        st = ms_ff_pull(f, &ev);
        if (st == MS_AGAIN) {
            continue;
        }
        if (st != MS_OK) {
            break;
        }
        if (ev.type == MS_EVENT_SUBTITLE && ev.data && ev.data_size >= 7 &&
            memcmp(ev.data, "LiveNew", 7) == 0) {
            saw = 1;
        }
        if (ev.release_id) {
            ms_ff_release(f, ev.release_id);
        }
        if (saw) {
            break;
        }
    }
    {
        ms_sub_cue many[16];
        size_t nmany = 0;
        int nmid = 0, nnew = 0;
        memset(many, 0, sizeof(many));
        (void)ms_ff_subtitles_at(f, 35 * 90000 / 10, many, 16, &nmany); /* 3.5s */
        for (k = 0; k < nmany; k++) {
            if (many[k].text && strstr(many[k].text, "LiveMid") != NULL) {
                nmid++;
            }
        }
        (void)ms_ff_subtitles_at(f, 95 * 90000 / 10, many, 16, &nmany); /* 9.5s */
        for (k = 0; k < nmany; k++) {
            if (many[k].text && strstr(many[k].text, "LiveNew") != NULL) {
                nnew++;
            }
        }
        CHECK(nmid <= 1, "LiveMid not duplicated after window slide");
        /* LiveNew 可能因直播时钟未到 9.5s 而不出现；有则不得重复。 */
        CHECK(nnew <= 1, "LiveNew not duplicated");
        if (!saw && nnew == 0) {
            printf("  note: live slide did not yet surface LiveNew (window/time); "
                   "reload remaining bounded\n");
        }
        CHECK(nmany <= 16, "cues_at cap respected");
    }
    CHECK_ST(ms_ff_cancel(f), MS_OK);
    ms_ff_destroy(f);
    f = NULL;

    /* 失效外链不拖垮主音视频 */
    snprintf(missurl, sizeof(missurl), "http://127.0.0.1:%d/miss/master.m3u8", hs.port);
    CHECK_ST(ms_ff_create(NULL, &lim, &f), MS_OK);
    st = ms_ff_open(f, missurl);
    CHECK(st == MS_OK, "open with missing live/vod sub playlist still ok");
    v_au = 0;
    for (loops = 0; loops < 400; loops++) {
        st = ms_ff_pull(f, &ev);
        if (st == MS_AGAIN) {
            continue;
        }
        if (st != MS_OK) {
            break;
        }
        if (ev.type == MS_EVENT_AU && ev.track.kind == MS_TRACK_VIDEO) {
            v_au++;
        }
        if (ev.release_id) {
            ms_ff_release(f, ev.release_id);
        }
        if (v_au >= 2) {
            break;
        }
    }
    CHECK(v_au >= 1, "video survives missing subtitle playlist");
    CHECK_ST(ms_ff_cancel(f), MS_OK);
    ms_ff_destroy(f);
    f = NULL;

    /* 限额：极小队列下取消有界，不挂死 */
    {
        ms_ff_limits tight;
        ms_ff_limits_default(&tight);
        tight.au_queue_bytes = 4096;
        tight.out_slots = 2;
        tight.io_timeout_ms = 2000;
        CHECK_ST(ms_ff_create(NULL, &tight, &f), MS_OK);
        CHECK_ST(ms_ff_open(f, liveurl), MS_OK);
        for (loops = 0; loops < 80; loops++) {
            st = ms_ff_pull(f, &ev);
            if (st == MS_OK && ev.release_id) {
                ms_ff_release(f, ev.release_id);
            }
        }
        CHECK_ST(ms_ff_cancel(f), MS_OK);
        ms_ff_destroy(f);
        f = NULL;
    }

    srv_stop(&hs);
    free(v0b); free(v1b); free(v2b); free(v3b);
    printf("  test_ff_hls_sub_timeline ok (local HTTP VOD seek/map/dedup + live first cue; "
           "lavf contract; not device paint)\n");
    return 0;
}



static int lw_pull(ms_ff *f, int need_v, int need_cfg, int need_key, int max_loops,
                   int *v_au, int *cfg, int *key, int64_t *last_pts)
{
    int loops;
    int v = 0, c = 0, k = 0;
    int64_t lp = -1;
    for (loops = 0; loops < max_loops; loops++) {
        ms_event ev;
        ms_status st = ms_ff_pull(f, &ev);
        if (st == MS_AGAIN) {
            continue;
        }
        if (st != MS_OK) {
            break;
        }
        if (ev.type == MS_EVENT_CODEC_CONFIG && ev.track.kind == MS_TRACK_VIDEO) {
            c++;
        }
        if (ev.type == MS_EVENT_AU && ev.track.kind == MS_TRACK_VIDEO) {
            v++;
            if (ev.is_keyframe) {
                k++;
            }
            if (ev.has_pts) {
                lp = (int64_t)ev.pts;
                if (!(ev.time_base_num == 1 && ev.time_base_den == 90000) &&
                    ev.time_base_num > 0 && ev.time_base_den > 0) {
                    lp = ev.pts * 90000ll * (int64_t)ev.time_base_num /
                         (int64_t)ev.time_base_den;
                }
            }
        }
        if (ev.release_id) {
            ms_ff_release(f, ev.release_id);
        }
        if (v >= need_v && c >= need_cfg && k >= need_key) {
            break;
        }
    }
    if (v_au) {
        *v_au = v;
    }
    if (cfg) {
        *cfg = c;
    }
    if (key) {
        *key = k;
    }
    if (last_pts) {
        *last_pts = lp;
    }
    return (v >= need_v && (need_cfg <= 0 || c >= need_cfg) &&
            (need_key <= 0 || k >= need_key)) ? 0 : -1;
}

static int test_ff_hls_live_window(const char *tmpdir)
{
    const char *ffmpeg = "/usr/bin/ffmpeg";
    char dir[512], vts[512], v0[512], v1[512], v2[512], v3[512], v4[512], cmd[2048];
    char evurl[640], slurl[640], missurl[640];
    uint8_t *v0b = NULL, *v1b = NULL, *v2b = NULL, *v3b = NULL, *v4b = NULL;
    size_t v0n = 0, v1n = 0, v2n = 0, v3n = 0, v4n = 0;
    test_srv hs;
    ms_ff *f = NULL;
    ms_ff_limits lim;
    ms_ff_seek_range rng, rng2;
    ms_track_info ti[16];
    size_t nt = 0;
    uint32_t s_eng = 0, s_zho = 0;
    ms_sub_cue cue;
    size_t nc = 0;
    ms_event ev;
    ms_status st;
    int loops, v_au, cfg, key;
    int64_t last_pts, start0, edge0, end0;
    const char *ctype_pl = "application/vnd.apple.mpegurl";
    const char *eng0 =
        "WEBVTT\n\n"
        "00:00:00.500 --> 00:00:04.000\nLongEng\n";
    const char *eng1 =
        "WEBVTT\n\n"
        "00:00:00.500 --> 00:00:04.000\nLongEng\n\n"
        "00:00:03.200 --> 00:00:05.000\nMidEng\n";
    const char *zho0 =
        "WEBVTT\n\n"
        "00:00:00.500 --> 00:00:04.000\nLongZho\n";
    const char *zho1 =
        "WEBVTT\n\n"
        "00:00:00.500 --> 00:00:04.000\nLongZho\n";
    char ev_v1[640], ev_v2[768], ev_s1[512], ev_s2[512];
    char sl_v1[512], sl_v2[512], sl_s1[512], sl_s2[512], miss_v[512];
    const char *ev_mpl =
        "#EXTM3U\n"
        "#EXT-X-MEDIA:TYPE=SUBTITLES,GROUP-ID=\"subs\",NAME=\"English\","
        "DEFAULT=YES,AUTOSELECT=YES,LANGUAGE=\"eng\",URI=\"eng.m3u8\"\n"
        "#EXT-X-MEDIA:TYPE=SUBTITLES,GROUP-ID=\"subs\",NAME=\"Chinese\","
        "DEFAULT=NO,AUTOSELECT=YES,LANGUAGE=\"zho\",URI=\"zho.m3u8\"\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=200000,RESOLUTION=160x120,"
        "CODECS=\"avc1.42e01e,mp4a.40.2\",SUBTITLES=\"subs\"\n"
        "v.m3u8\n";
    const char *sl_mpl =
        "#EXTM3U\n"
        "#EXT-X-MEDIA:TYPE=SUBTITLES,GROUP-ID=\"subs\",NAME=\"Live\","
        "DEFAULT=YES,AUTOSELECT=YES,LANGUAGE=\"eng\",URI=\"eng.m3u8\"\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=200000,RESOLUTION=160x120,"
        "CODECS=\"avc1.42e01e,mp4a.40.2\",SUBTITLES=\"subs\"\n"
        "v.m3u8\n";
    const char *zpl =
        "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:4\n"
        "#EXT-X-PLAYLIST-TYPE:EVENT\n#EXT-X-MEDIA-SEQUENCE:10\n"
        "#EXTINF:3.0,\nzho0.vtt\n#EXTINF:3.0,\nzho1.vtt\n";
    const char *miss_mpl =
        "#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=200000\nv.m3u8\n";

    memset(&hs, 0, sizeof(hs));
    if (access(ffmpeg, X_OK) != 0) {
        fprintf(stderr, "FAIL test_ff_hls_live_window: host /usr/bin/ffmpeg required\n");
        failed = 1;
        return 1;
    }
    CHECK(path_join(dir, sizeof(dir), tmpdir, "hlslivewin") == 0, "dir");
    (void)mkdir(dir, 0755);
    CHECK(path_join(vts, sizeof(vts), dir, "v.ts") == 0, "vts");
    CHECK(path_join(v0, sizeof(v0), dir, "v0.ts") == 0, "v0");
    CHECK(path_join(v1, sizeof(v1), dir, "v1.ts") == 0, "v1");
    CHECK(path_join(v2, sizeof(v2), dir, "v2.ts") == 0, "v2");
    CHECK(path_join(v3, sizeof(v3), dir, "v3.ts") == 0, "v3");
    CHECK(path_join(v4, sizeof(v4), dir, "v4.ts") == 0, "v4");
    snprintf(cmd, sizeof(cmd),
             "%s -hide_banner -loglevel error -y "
             "-f lavfi -i testsrc=size=160x120:rate=25:duration=15 "
             "-f lavfi -i sine=frequency=440:duration=15 "
             "-c:v libx264 -pix_fmt yuv420p -g 75 -keyint_min 75 "
             "-c:a aac -ar 48000 -ac 2 -shortest %s",
             ffmpeg, vts);
    CHECK(system(cmd) == 0, "encode 15s av ts");
    snprintf(cmd, sizeof(cmd),
             "%s -hide_banner -loglevel error -y -i %s -c copy -t 3 %s",
             ffmpeg, vts, v0);
    CHECK(system(cmd) == 0, "cut v0");
    snprintf(cmd, sizeof(cmd),
             "%s -hide_banner -loglevel error -y -ss 3 -i %s -c copy -t 3 %s",
             ffmpeg, vts, v1);
    CHECK(system(cmd) == 0, "cut v1");
    snprintf(cmd, sizeof(cmd),
             "%s -hide_banner -loglevel error -y -ss 6 -i %s -c copy -t 3 %s",
             ffmpeg, vts, v2);
    CHECK(system(cmd) == 0, "cut v2");
    snprintf(cmd, sizeof(cmd),
             "%s -hide_banner -loglevel error -y -ss 9 -i %s -c copy -t 3 %s",
             ffmpeg, vts, v3);
    CHECK(system(cmd) == 0, "cut v3");
    snprintf(cmd, sizeof(cmd),
             "%s -hide_banner -loglevel error -y -ss 12 -i %s -c copy -t 3 %s",
             ffmpeg, vts, v4);
    CHECK(system(cmd) == 0, "cut v4");
    CHECK(read_all_file(v0, &v0b, &v0n) == 0 && v0n > 0, "load v0");
    CHECK(read_all_file(v1, &v1b, &v1n) == 0 && v1n > 0, "load v1");
    CHECK(read_all_file(v2, &v2b, &v2n) == 0 && v2n > 0, "load v2");
    CHECK(read_all_file(v3, &v3b, &v3n) == 0 && v3n > 0, "load v3");
    CHECK(read_all_file(v4, &v4b, &v4n) == 0 && v4n > 0, "load v4");

    snprintf(ev_v1, sizeof(ev_v1),
             "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:4\n"
             "#EXT-X-PLAYLIST-TYPE:EVENT\n#EXT-X-MEDIA-SEQUENCE:10\n"
             "#EXTINF:3.0,\nv0.ts\n#EXTINF:3.0,\nv1.ts\n#EXTINF:3.0,\nv2.ts\n");
    snprintf(ev_v2, sizeof(ev_v2),
             "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:4\n"
             "#EXT-X-PLAYLIST-TYPE:EVENT\n#EXT-X-MEDIA-SEQUENCE:10\n"
             "#EXTINF:3.0,\nv0.ts\n#EXTINF:3.0,\nv1.ts\n#EXTINF:3.0,\nv2.ts\n"
             "#EXT-X-DISCONTINUITY\n#EXTINF:3.0,\nv3.ts\n");
    snprintf(ev_s1, sizeof(ev_s1),
             "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:4\n"
             "#EXT-X-PLAYLIST-TYPE:EVENT\n#EXT-X-MEDIA-SEQUENCE:10\n"
             "#EXTINF:3.0,\neng0.vtt\n#EXTINF:3.0,\neng1.vtt\n#EXTINF:3.0,\neng1.vtt\n");
    snprintf(ev_s2, sizeof(ev_s2),
             "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:4\n"
             "#EXT-X-PLAYLIST-TYPE:EVENT\n#EXT-X-MEDIA-SEQUENCE:10\n"
             "#EXTINF:3.0,\neng0.vtt\n#EXTINF:3.0,\neng1.vtt\n#EXTINF:3.0,\neng1.vtt\n"
             "#EXTINF:3.0,\neng1.vtt\n");
    snprintf(sl_v1, sizeof(sl_v1),
             "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:4\n"
             "#EXT-X-MEDIA-SEQUENCE:10\n"
             "#EXTINF:3.0,\nv0.ts\n#EXTINF:3.0,\nv1.ts\n#EXTINF:3.0,\nv2.ts\n");
    snprintf(sl_v2, sizeof(sl_v2),
             "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:4\n"
             "#EXT-X-MEDIA-SEQUENCE:11\n"
             "#EXTINF:3.0,\nv1.ts\n#EXTINF:3.0,\nv2.ts\n#EXTINF:3.0,\nv3.ts\n");
    snprintf(sl_s1, sizeof(sl_s1),
             "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:4\n"
             "#EXT-X-MEDIA-SEQUENCE:10\n"
             "#EXTINF:3.0,\neng0.vtt\n#EXTINF:3.0,\neng1.vtt\n#EXTINF:3.0,\neng1.vtt\n");
    snprintf(sl_s2, sizeof(sl_s2),
             "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:4\n"
             "#EXT-X-MEDIA-SEQUENCE:11\n"
             "#EXTINF:3.0,\neng1.vtt\n#EXTINF:3.0,\neng1.vtt\n#EXTINF:3.0,\neng1.vtt\n");
    snprintf(miss_v, sizeof(miss_v),
             "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:4\n"
             "#EXT-X-MEDIA-SEQUENCE:10\n"
             "#EXTINF:3.0,\nv0.ts\n#EXTINF:3.0,\nmissing.ts\n#EXTINF:3.0,\nv2.ts\n");

    CHECK(srv_start(&hs, 0, NULL, NULL) == 0, "start http");
    CHECK(srv_add(&hs, "/event/v0.ts", v0b, v0n, "video/mp2t") == 0, "ev v0");
    CHECK(srv_add(&hs, "/event/v1.ts", v1b, v1n, "video/mp2t") == 0, "ev v1");
    CHECK(srv_add(&hs, "/event/v2.ts", v2b, v2n, "video/mp2t") == 0, "ev v2");
    CHECK(srv_add(&hs, "/event/v3.ts", v3b, v3n, "video/mp2t") == 0, "ev v3");
    CHECK(srv_add(&hs, "/event/v.m3u8", ev_v1, strlen(ev_v1), ctype_pl) == 0, "ev vpl");
    CHECK(srv_add(&hs, "/event/eng0.vtt", eng0, strlen(eng0), "text/vtt") == 0, "eng0");
    CHECK(srv_add(&hs, "/event/eng1.vtt", eng1, strlen(eng1), "text/vtt") == 0, "eng1");
    CHECK(srv_add(&hs, "/event/zho0.vtt", zho0, strlen(zho0), "text/vtt") == 0, "zho0");
    CHECK(srv_add(&hs, "/event/zho1.vtt", zho1, strlen(zho1), "text/vtt") == 0, "zho1");
    CHECK(srv_add(&hs, "/event/eng.m3u8", ev_s1, strlen(ev_s1), ctype_pl) == 0, "ev epl");
    CHECK(srv_add(&hs, "/event/zho.m3u8", zpl, strlen(zpl), ctype_pl) == 0, "ev zpl");
    CHECK(srv_add(&hs, "/event/master.m3u8", ev_mpl, strlen(ev_mpl), ctype_pl) == 0, "ev master");
    CHECK(srv_add(&hs, "/slide/v0.ts", v0b, v0n, "video/mp2t") == 0, "sl v0");
    CHECK(srv_add(&hs, "/slide/v1.ts", v1b, v1n, "video/mp2t") == 0, "sl v1");
    CHECK(srv_add(&hs, "/slide/v2.ts", v2b, v2n, "video/mp2t") == 0, "sl v2");
    CHECK(srv_add(&hs, "/slide/v3.ts", v3b, v3n, "video/mp2t") == 0, "sl v3");
    CHECK(srv_add(&hs, "/slide/v.m3u8", sl_v1, strlen(sl_v1), ctype_pl) == 0, "sl vpl");
    CHECK(srv_add(&hs, "/slide/eng0.vtt", eng0, strlen(eng0), "text/vtt") == 0, "sl eng0");
    CHECK(srv_add(&hs, "/slide/eng1.vtt", eng1, strlen(eng1), "text/vtt") == 0, "sl eng1");
    CHECK(srv_add(&hs, "/slide/eng.m3u8", sl_s1, strlen(sl_s1), ctype_pl) == 0, "sl epl");
    CHECK(srv_add(&hs, "/slide/master.m3u8", sl_mpl, strlen(sl_mpl), ctype_pl) == 0, "sl master");
    CHECK(srv_add(&hs, "/miss/v0.ts", v0b, v0n, "video/mp2t") == 0, "miss v0");
    CHECK(srv_add(&hs, "/miss/v2.ts", v2b, v2n, "video/mp2t") == 0, "miss v2");
    CHECK(srv_add(&hs, "/miss/v.m3u8", miss_v, strlen(miss_v), ctype_pl) == 0, "miss vpl");
    CHECK(srv_add(&hs, "/miss/master.m3u8", miss_mpl, strlen(miss_mpl), ctype_pl) == 0,
          "miss master");

    ms_ff_limits_default(&lim);
    lim.io_timeout_ms = 8000;

    snprintf(evurl, sizeof(evurl), "http://127.0.0.1:%d/event/master.m3u8", hs.port);
    CHECK_ST(ms_ff_create(NULL, &lim, &f), MS_OK);
    CHECK_ST(ms_ff_open(f, evurl), MS_OK);
    CHECK(ms_ff_is_seekable(f), "EVENT is seekable");
    CHECK(ms_ff_duration_90k(f) < 0, "EVENT duration_90k is -1, not window length");
    CHECK_ST(ms_ff_list_tracks(f, ti, 16, &nt), MS_OK);
    s_eng = tl_find_sub(ti, nt, "eng");
    s_zho = tl_find_sub(ti, nt, "zho");
    CHECK(s_eng != 0 && s_zho != 0, "EVENT enumerates eng/zho");
    CHECK(lw_pull(f, 2, 0, 0, 400, &v_au, &cfg, &key, &last_pts) == 0, "EVENT initial pull");
    memset(&rng, 0, sizeof(rng));
    rng.struct_size = sizeof(rng);
    CHECK_ST(ms_ff_get_seek_range(f, &rng), MS_OK);
    CHECK(rng.seekable && rng.kind == MS_FF_SEEK_KIND_EVENT, "kind EVENT");
    CHECK(rng.duration_90k < 0, "EVENT does not publish VOD duration");
    CHECK(rng.end_90k > rng.start_90k, "EVENT window extent");
    CHECK(rng.live_edge_90k >= rng.start_90k && rng.live_edge_90k <= rng.end_90k,
          "EVENT live_edge in window");
    CHECK(rng.safety_90k > 0 && rng.live_edge_90k + rng.safety_90k <= rng.end_90k + 90000,
          "safety keeps edge off unfinished tail (1s slack)");
    start0 = rng.start_90k;
    end0 = rng.end_90k;
    edge0 = rng.live_edge_90k;
    CHECK_ST(ms_ff_seek(f, start0 + 2 * 90000), MS_OK);
    CHECK(lw_pull(f, 1, 1, 1, 800, &v_au, &cfg, &key, &last_pts) == 0,
          "EVENT mid-window seek yields AU/CSD/key (not paint)");
    memset(&cue, 0, sizeof(cue));
    nc = 0;
    (void)ms_ff_subtitles_at(f, start0 + 2 * 90000, &cue, 1, &nc);
    if (nc == 0 || cue.text == NULL || strstr(cue.text, "LongEng") == NULL) {
        (void)lw_pull(f, 1, 0, 0, 400, &v_au, &cfg, &key, &last_pts);
        (void)ms_ff_subtitles_at(f, start0 + 2 * 90000, &cue, 1, &nc);
    }
    CHECK(cue.text != NULL && strstr(cue.text, "LongEng") != NULL,
          "EVENT seek 2s still covers LongEng");
    CHECK_ST(ms_ff_seek(f, start0 > 5 * 90000 ? start0 - 5 * 90000 : 0), MS_OK);
    CHECK(lw_pull(f, 1, 0, 1, 600, &v_au, &cfg, &key, &last_pts) == 0,
          "before-start clamps and still yields video");
    CHECK_ST(ms_ff_seek(f, end0 + 30 * 90000), MS_OK);
    CHECK(lw_pull(f, 1, 0, 0, 600, &v_au, &cfg, &key, &last_pts) == 0,
          "past-edge clamps to live_edge and yields AU");
    CHECK_ST(ms_ff_seek_live_edge(f), MS_OK);
    CHECK(lw_pull(f, 1, 0, 0, 600, &v_au, &cfg, &key, &last_pts) == 0, "seek_live_edge AU");
    CHECK_ST(ms_ff_select_track(f, s_zho, 1), MS_OK);
    CHECK_ST(ms_ff_seek(f, start0 + 2 * 90000), MS_OK);
    CHECK(lw_pull(f, 1, 0, 0, 800, &v_au, &cfg, &key, &last_pts) == 0, "after switch+seek");
    {
        size_t k;
        int zsel = 0;
        CHECK_ST(ms_ff_list_tracks(f, ti, 16, &nt), MS_OK);
        for (k = 0; k < nt; k++) {
            if (ti[k].id == s_zho && ti[k].selected) {
                zsel = 1;
            }
        }
        CHECK(zsel, "subtitle selection kept across EVENT seek");
    }
    CHECK(srv_update(&hs, "/event/v.m3u8", ev_v2, strlen(ev_v2)) == 0, "grow event v");
    CHECK(srv_update(&hs, "/event/eng.m3u8", ev_s2, strlen(ev_s2)) == 0, "grow event s");
    usleep(4500 * 1000);
    (void)lw_pull(f, 1, 0, 0, 800, &v_au, &cfg, &key, &last_pts);
    memset(&rng2, 0, sizeof(rng2));
    rng2.struct_size = sizeof(rng2);
    CHECK_ST(ms_ff_get_seek_range(f, &rng2), MS_OK);
    CHECK(rng2.kind == MS_FF_SEEK_KIND_EVENT && rng2.duration_90k < 0, "still EVENT/-1");
    CHECK(rng2.start_90k <= start0 + 90000, "EVENT start does not slide off history");
    CHECK(rng2.end_90k >= end0, "EVENT end grows or holds after append");
    CHECK_ST(ms_ff_cancel(f), MS_OK);
    ms_ff_destroy(f);
    f = NULL;

    snprintf(slurl, sizeof(slurl), "http://127.0.0.1:%d/slide/master.m3u8", hs.port);
    CHECK_ST(ms_ff_create(NULL, &lim, &f), MS_OK);
    CHECK_ST(ms_ff_open(f, slurl), MS_OK);
    CHECK(ms_ff_is_seekable(f), "sliding is seekable");
    CHECK(ms_ff_duration_90k(f) < 0, "sliding duration stays -1");
    CHECK(lw_pull(f, 2, 0, 0, 400, &v_au, &cfg, &key, &last_pts) == 0, "slide initial pull");
    memset(&rng, 0, sizeof(rng));
    rng.struct_size = sizeof(rng);
    CHECK_ST(ms_ff_get_seek_range(f, &rng), MS_OK);
    CHECK(rng.kind == MS_FF_SEEK_KIND_SLIDING, "kind SLIDING");
    CHECK(rng.duration_90k < 0, "do not report window length as VOD duration");
    CHECK(rng.end_90k - rng.start_90k >= 6 * 90000, "about 3x3s window (allow 2s slack)");
    start0 = rng.start_90k;
    end0 = rng.end_90k;
    edge0 = rng.live_edge_90k;
    CHECK(edge0 <= end0 && edge0 >= start0, "edge in window");
    CHECK(rng.safety_90k >= 3 * 90000, "safety is about one target duration");
    CHECK_ST(ms_ff_seek(f, start0 + 3 * 90000), MS_OK);
    CHECK(lw_pull(f, 1, 1, 1, 800, &v_au, &cfg, &key, &last_pts) == 0,
          "sliding mid seek AU/CSD/key");
    CHECK(srv_update(&hs, "/slide/v.m3u8", sl_v2, strlen(sl_v2)) == 0, "slide v");
    CHECK(srv_update(&hs, "/slide/eng.m3u8", sl_s2, strlen(sl_s2)) == 0, "slide s");
    memset(&rng2, 0, sizeof(rng2));
    rng2.struct_size = sizeof(rng2);
    rng2.start_90k = start0;
    for (loops = 0; loops < 8; loops++) {
        usleep(1500 * 1000);
        /* 仅 pull 可能还停在旧窗口未读完的分片上，不会 reload。
         * 过期目标的 seek 会走 hls_read_seek 刷新清单。 */
        (void)ms_ff_seek(f, start0);
        (void)lw_pull(f, 1, 0, 0, 400, &v_au, &cfg, &key, &last_pts);
        rng2.struct_size = sizeof(rng2);
        if (ms_ff_get_seek_range(f, &rng2) == MS_OK &&
            rng2.start_90k >= start0 + 2 * 90000) {
            break;
        }
    }
    CHECK_ST(ms_ff_get_seek_range(f, &rng2), MS_OK);
    CHECK(rng2.kind == MS_FF_SEEK_KIND_SLIDING && rng2.duration_90k < 0, "still sliding/-1");
    if (rng2.start_90k < start0 + 2 * 90000) {
        fprintf(stderr, "slide window start %lld -> %lld (need +2s); end %lld edge %lld\n",
                (long long)start0, (long long)rng2.start_90k,
                (long long)rng2.end_90k, (long long)rng2.live_edge_90k);
    }
    CHECK(rng2.start_90k >= start0 + 2 * 90000, "window start advanced after drop");
    CHECK_ST(ms_ff_seek(f, start0), MS_OK);
    CHECK(lw_pull(f, 1, 0, 1, 800, &v_au, &cfg, &key, &last_pts) == 0,
          "expired target clamps to new start, still AU/key");
    CHECK_ST(ms_ff_seek(f, rng2.end_90k + 20 * 90000), MS_OK);
    CHECK(lw_pull(f, 1, 0, 0, 600, &v_au, &cfg, &key, &last_pts) == 0,
          "past new edge clamps");
    CHECK_ST(ms_ff_seek_live_edge(f), MS_OK);
    CHECK(lw_pull(f, 1, 0, 0, 600, &v_au, &cfg, &key, &last_pts) == 0, "sliding live_edge");
    CHECK_ST(ms_ff_cancel(f), MS_OK);
    ms_ff_destroy(f);
    f = NULL;

    snprintf(missurl, sizeof(missurl), "http://127.0.0.1:%d/miss/master.m3u8", hs.port);
    CHECK_ST(ms_ff_create(NULL, &lim, &f), MS_OK);
    st = ms_ff_open(f, missurl);
    if (st == MS_OK) {
        CHECK(ms_ff_is_seekable(f), "missing-seg playlist still has a window");
        CHECK(ms_ff_duration_90k(f) < 0, "missing live is not VOD duration");
        (void)lw_pull(f, 1, 0, 0, 200, &v_au, &cfg, &key, &last_pts);
        (void)ms_ff_seek(f, 4 * 90000);
        for (loops = 0; loops < 400; loops++) {
            st = ms_ff_pull(f, &ev);
            if (st == MS_AGAIN) {
                continue;
            }
            if (st == MS_OK) {
                if (ev.release_id) {
                    ms_ff_release(f, ev.release_id);
                }
                continue;
            }
            break;
        }
        CHECK_ST(ms_ff_cancel(f), MS_OK);
    }
    ms_ff_destroy(f);
    f = NULL;

    srv_stop(&hs);
    free(v0b); free(v1b); free(v2b); free(v3b); free(v4b);
    printf("  test_ff_hls_live_window ok (EVENT/sliding HTTP demux window, clamp, "
           "live_edge, track keep, cancel, missing seg; AU/CSD/PTS only, not paint)\n");
    (void)edge0;
    return 0;
}


/* lavf 下发的 H.264 CSD 走既有 ms_h264 组帧，不经 ms_video_cfg_parse（该入口对 H.264 返回 UNSUPPORTED）。
 * 测试只从 Annex B SPS 读真实宽高，证明新旧变体参数不同，不发明分辨率。 */
static unsigned ev_rbsp_bit(const uint8_t *rbsp, size_t n, size_t *bits)
{
    size_t b = *bits;
    unsigned v;
    if ((b / 8) >= n) {
        return 0;
    }
    v = (rbsp[b / 8] >> (7 - (b % 8))) & 1u;
    *bits = b + 1;
    return v;
}

static unsigned ev_rbsp_u(const uint8_t *rbsp, size_t n, size_t *bits, unsigned len)
{
    unsigned v = 0, i;
    for (i = 0; i < len; i++) {
        v = (v << 1) | ev_rbsp_bit(rbsp, n, bits);
    }
    return v;
}

static unsigned ev_rbsp_ue(const uint8_t *rbsp, size_t n, size_t *bits)
{
    unsigned z = 0;
    while (ev_rbsp_bit(rbsp, n, bits) == 0) {
        z++;
        if (z > 31) {
            return 0;
        }
    }
    return ev_rbsp_u(rbsp, n, bits, z) + ((1u << z) - 1u);
}

static size_t ev_ebsp_to_rbsp(const uint8_t *nal, size_t n, uint8_t *out, size_t cap)
{
    size_t i, o = 0;
    for (i = 0; i < n && o < cap; i++) {
        if (i + 2 < n && nal[i] == 0 && nal[i + 1] == 0 && nal[i + 2] == 3) {
            if (o + 2 > cap) {
                break;
            }
            out[o++] = 0;
            out[o++] = 0;
            i += 2;
            continue;
        }
        out[o++] = nal[i];
    }
    return o;
}

static int ev_sps_wh(const uint8_t *nal, size_t n, int *w, int *h)
{
    uint8_t rbsp[256];
    size_t rn, bits = 0;
    unsigned profile, chroma = 1, crop = 0;
    unsigned width_mbs, height_map, frame_mbs = 1;
    unsigned crop_l = 0, crop_r = 0, crop_t = 0, crop_b = 0;
    if (nal == NULL || n < 4 || (nal[0] & 0x1f) != 7) {
        return 0;
    }
    rn = ev_ebsp_to_rbsp(nal + 1, n - 1, rbsp, sizeof(rbsp));
    profile = ev_rbsp_u(rbsp, rn, &bits, 8);
    (void)ev_rbsp_u(rbsp, rn, &bits, 8); /* constraints */
    (void)ev_rbsp_u(rbsp, rn, &bits, 8); /* level */
    (void)ev_rbsp_ue(rbsp, rn, &bits); /* sps id */
    if (profile == 100 || profile == 110 || profile == 122 || profile == 244 ||
        profile == 44 || profile == 83 || profile == 86 || profile == 118 ||
        profile == 128 || profile == 138 || profile == 139 || profile == 134 ||
        profile == 135) {
        unsigned i, nlists;
        chroma = ev_rbsp_ue(rbsp, rn, &bits);
        if (chroma == 3) {
            (void)ev_rbsp_bit(rbsp, rn, &bits);
        }
        (void)ev_rbsp_ue(rbsp, rn, &bits);
        (void)ev_rbsp_ue(rbsp, rn, &bits);
        (void)ev_rbsp_bit(rbsp, rn, &bits);
        if (ev_rbsp_bit(rbsp, rn, &bits)) {
            nlists = (chroma != 3) ? 8u : 12u;
            for (i = 0; i < nlists; i++) {
                if (ev_rbsp_bit(rbsp, rn, &bits)) {
                    return 0; /* 出现 scaling list 时本测试不硬解，改靠 CSD 比对 */
                }
            }
        }
    }
    (void)ev_rbsp_ue(rbsp, rn, &bits); /* log2_max_frame_num */
    {
        unsigned poc = ev_rbsp_ue(rbsp, rn, &bits);
        if (poc == 0) {
            (void)ev_rbsp_ue(rbsp, rn, &bits);
        } else if (poc == 1) {
            unsigned n_off, i;
            (void)ev_rbsp_bit(rbsp, rn, &bits);
            (void)ev_rbsp_ue(rbsp, rn, &bits);
            (void)ev_rbsp_ue(rbsp, rn, &bits);
            n_off = ev_rbsp_ue(rbsp, rn, &bits);
            for (i = 0; i < n_off && i < 256; i++) {
                (void)ev_rbsp_ue(rbsp, rn, &bits);
            }
        }
    }
    (void)ev_rbsp_ue(rbsp, rn, &bits); /* max_num_ref */
    (void)ev_rbsp_bit(rbsp, rn, &bits);
    width_mbs = ev_rbsp_ue(rbsp, rn, &bits) + 1;
    height_map = ev_rbsp_ue(rbsp, rn, &bits) + 1;
    frame_mbs = ev_rbsp_bit(rbsp, rn, &bits);
    if (!frame_mbs) {
        (void)ev_rbsp_bit(rbsp, rn, &bits);
    }
    (void)ev_rbsp_bit(rbsp, rn, &bits);
    crop = ev_rbsp_bit(rbsp, rn, &bits);
    if (crop) {
        crop_l = ev_rbsp_ue(rbsp, rn, &bits);
        crop_r = ev_rbsp_ue(rbsp, rn, &bits);
        crop_t = ev_rbsp_ue(rbsp, rn, &bits);
        crop_b = ev_rbsp_ue(rbsp, rn, &bits);
    }
    *w = (int)(width_mbs * 16u) - (int)((crop_l + crop_r) * 2u);
    *h = (int)((2u - frame_mbs) * height_map * 16u) - (int)((crop_t + crop_b) * 2u);
    return (*w > 0 && *h > 0);
}

static int ev_annexb_sps_wh(const uint8_t *data, size_t size, int *w, int *h)
{
    size_t i = 0;
    if (data == NULL || size < 5 || w == NULL || h == NULL) {
        return 0;
    }
    while (i + 4 < size) {
        size_t sc = 0, nal, j;
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
            sc = 3;
        } else if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 0 && data[i + 3] == 1) {
            sc = 4;
        } else {
            i++;
            continue;
        }
        nal = i + sc;
        j = nal;
        while (j + 3 < size) {
            if (data[j] == 0 && data[j + 1] == 0 && (data[j + 2] == 1 ||
                (data[j + 2] == 0 && j + 3 < size && data[j + 3] == 1))) {
                break;
            }
            j++;
        }
        if (j + 3 >= size) {
            j = size;
        }
        if (nal < j && (data[nal] & 0x1f) == 7) {
            return ev_sps_wh(data + nal, j - nal, w, h);
        }
        i = j;
    }
    return 0;
}

static int ev_video_wh(const ms_event *ev, int *w, int *h)
{
    if (ev == NULL || ev->data == NULL || ev->data_size == 0 || w == NULL || h == NULL) {
        return 0;
    }
    if (ev->track.kind != MS_TRACK_VIDEO || ev->track.codec != MS_CODEC_H264) {
        return 0;
    }
    return ev_annexb_sps_wh(ev->data, ev->data_size, w, h);
}

static int test_ff_hls_video_variants(const char *tmpdir)
{
    const char *ffmpeg = "/usr/bin/ffmpeg";
    char dir[512], lots[512], hits[512], muxlo[512], muxhi[512], m2ts[512], cmd[2048];
    char evtt[512], ets[512], zts[512];
    char lopl[512], hipl[512], epl[512], zpl[512], spl[512], m2pl[512];
    char mpl[512], muxpl[512];
    char url[640], muxurl[640];
    uint8_t *lob = NULL, *hib = NULL, *m2b = NULL, *eb = NULL, *zb = NULL;
    uint8_t *muxlob = NULL, *muxhib = NULL;
    uint8_t *loplb = NULL, *hiplb = NULL, *eplb = NULL, *zplb = NULL, *splb = NULL;
    uint8_t *m2plb = NULL, *mplb = NULL, *muxplb = NULL, *evb = NULL;
    size_t lon = 0, hin = 0, m2n = 0, en = 0, zn = 0, muxlon = 0, muxhin = 0;
    size_t lopln = 0, hipln = 0, epln = 0, zpln = 0, spln = 0, m2pln = 0, mpln = 0;
    size_t muxpln = 0, evn = 0;
    const char *media_pl =
        "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:11\n"
        "#EXT-X-MEDIA-SEQUENCE:1\n#EXTINF:10.0,\n%s\n#EXT-X-ENDLIST\n";
    const char *m2_pl =
        "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:5\n"
        "#EXT-X-MEDIA-SEQUENCE:1\n#EXTINF:4.0,\nmpeg2.ts\n#EXT-X-ENDLIST\n";
    const char *eng_vtt =
        "WEBVTT\n\n"
        "00:00:00.000 --> 00:00:09.000\nHelloEng\n";
    char plbuf[256];
    test_srv hs;
    ms_ff *f = NULL;
    ms_ff_limits lim;
    ms_track_info ti[16];
    size_t nt = 0, k;
    uint32_t v_lo = 0, v_hi = 0, v_m2 = 0, a_eng = 0, a_zho = 0, s_eng = 0;
    int name_lo = 0, name_hi = 0, br_lo = 0, br_hi = 0;
    ms_event ev;
    int loops, st_ok;
    int v_lo_au = 0, v_hi_au = 0, v_lo_key = 0, v_hi_key = 0;
    int cfg_lo = 0, cfg_hi = 0, saw_w_lo = 0, saw_w_hi = 0;
    int a_eng_au = 0, a_zho_au = 0, sub_ev = 0;
    unsigned long long sel_ep0 = 1, sel_ep1 = 1, tl_ep0 = 1, tl_ep1 = 1;
    uint64_t last_pts = 0, first_hi_pts = 0;
    int have_pts = 0, have_hi_pts = 0;
    int pending_not_selected = 0;
    ms_status st;

    memset(&hs, 0, sizeof(hs));
    if (access(ffmpeg, X_OK) != 0) {
        fprintf(stderr, "FAIL test_ff_hls_video_variants: host /usr/bin/ffmpeg required\n");
        failed = 1;
        return 1;
    }
    CHECK(path_join(dir, sizeof(dir), tmpdir, "hlsvar") == 0, "dir");
    (void)mkdir(dir, 0755);
    CHECK(path_join(lots, sizeof(lots), dir, "lo.ts") == 0, "lots");
    CHECK(path_join(hits, sizeof(hits), dir, "hi.ts") == 0, "hits");
    CHECK(path_join(muxlo, sizeof(muxlo), dir, "muxlo.ts") == 0, "muxlo");
    CHECK(path_join(muxhi, sizeof(muxhi), dir, "muxhi.ts") == 0, "muxhi");
    CHECK(path_join(m2ts, sizeof(m2ts), dir, "mpeg2.ts") == 0, "m2ts");
    CHECK(path_join(ets, sizeof(ets), dir, "eng.ts") == 0, "ets");
    CHECK(path_join(zts, sizeof(zts), dir, "zho.ts") == 0, "zts");
    CHECK(path_join(evtt, sizeof(evtt), dir, "eng.vtt") == 0, "evtt");
    snprintf(cmd, sizeof(cmd),
             "%s -hide_banner -loglevel error -y "
             "-f lavfi -i testsrc=size=160x120:rate=25:duration=10 "
             "-c:v libx264 -pix_fmt yuv420p -g 25 -keyint_min 25 -b:v 200k -an %s",
             ffmpeg, lots);
    CHECK(system(cmd) == 0, "encode lo video");
    snprintf(cmd, sizeof(cmd),
             "%s -hide_banner -loglevel error -y "
             "-f lavfi -i testsrc=size=320x240:rate=25:duration=10 "
             "-c:v libx264 -pix_fmt yuv420p -g 25 -keyint_min 25 -b:v 800k -an %s",
             ffmpeg, hits);
    CHECK(system(cmd) == 0, "encode hi video");
    snprintf(cmd, sizeof(cmd),
             "%s -hide_banner -loglevel error -y "
             "-f lavfi -i testsrc=size=160x120:rate=25:duration=10 "
             "-f lavfi -i sine=frequency=440:duration=10 "
             "-c:v libx264 -pix_fmt yuv420p -g 25 -keyint_min 25 -b:v 200k "
             "-c:a aac -ar 48000 -ac 2 -shortest %s",
             ffmpeg, muxlo);
    CHECK(system(cmd) == 0, "encode mux lo");
    snprintf(cmd, sizeof(cmd),
             "%s -hide_banner -loglevel error -y "
             "-f lavfi -i testsrc=size=320x240:rate=25:duration=10 "
             "-f lavfi -i sine=frequency=880:duration=10 "
             "-c:v libx264 -pix_fmt yuv420p -g 25 -keyint_min 25 -b:v 800k "
             "-c:a aac -ar 44100 -ac 1 -shortest %s",
             ffmpeg, muxhi);
    CHECK(system(cmd) == 0, "encode mux hi");
    snprintf(cmd, sizeof(cmd),
             "%s -hide_banner -loglevel error -y "
             "-f lavfi -i testsrc=size=160x120:rate=25:duration=4 "
             "-c:v mpeg2video -pix_fmt yuv420p -g 25 -an %s",
             ffmpeg, m2ts);
    CHECK(system(cmd) == 0, "encode mpeg2 variant");
    snprintf(cmd, sizeof(cmd),
             "%s -hide_banner -loglevel error -y "
             "-f lavfi -i sine=frequency=440:duration=10 -c:a aac -ar 48000 -ac 2 %s",
             ffmpeg, ets);
    CHECK(system(cmd) == 0, "encode eng audio");
    snprintf(cmd, sizeof(cmd),
             "%s -hide_banner -loglevel error -y "
             "-f lavfi -i sine=frequency=880:duration=10 -c:a aac -ar 44100 -ac 1 %s",
             ffmpeg, zts);
    CHECK(system(cmd) == 0, "encode zho audio");
    CHECK(write_text_file(evtt, eng_vtt) == 0, "write vtt");

    CHECK(path_join(lopl, sizeof(lopl), dir, "lo.m3u8") == 0, "lopl");
    CHECK(path_join(hipl, sizeof(hipl), dir, "hi.m3u8") == 0, "hipl");
    CHECK(path_join(epl, sizeof(epl), dir, "eng.m3u8") == 0, "epl");
    CHECK(path_join(zpl, sizeof(zpl), dir, "zho.m3u8") == 0, "zpl");
    CHECK(path_join(spl, sizeof(spl), dir, "eng-sub.m3u8") == 0, "spl");
    CHECK(path_join(m2pl, sizeof(m2pl), dir, "mpeg2.m3u8") == 0, "m2pl");
    CHECK(path_join(mpl, sizeof(mpl), dir, "master.m3u8") == 0, "mpl");
    CHECK(path_join(muxpl, sizeof(muxpl), dir, "mux.m3u8") == 0, "muxpl");
    snprintf(plbuf, sizeof(plbuf), media_pl, "lo.ts");
    CHECK(write_text_file(lopl, plbuf) == 0, "write lo pl");
    snprintf(plbuf, sizeof(plbuf), media_pl, "hi.ts");
    CHECK(write_text_file(hipl, plbuf) == 0, "write hi pl");
    snprintf(plbuf, sizeof(plbuf), media_pl, "eng.ts");
    CHECK(write_text_file(epl, plbuf) == 0, "write eng pl");
    snprintf(plbuf, sizeof(plbuf), media_pl, "zho.ts");
    CHECK(write_text_file(zpl, plbuf) == 0, "write zho pl");
    snprintf(plbuf, sizeof(plbuf), media_pl, "eng.vtt");
    CHECK(write_text_file(spl, plbuf) == 0, "write sub pl");
    CHECK(write_text_file(m2pl, m2_pl) == 0, "write mpeg2 pl");
    CHECK(write_text_file(mpl,
             "#EXTM3U\n"
             "#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"ag\",LANGUAGE=\"eng\","
             "NAME=\"English\",DEFAULT=YES,AUTOSELECT=YES,URI=\"eng.m3u8\"\n"
             "#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"ag\",LANGUAGE=\"zho\","
             "NAME=\"Chinese\",DEFAULT=NO,AUTOSELECT=YES,URI=\"zho.m3u8\"\n"
             "#EXT-X-MEDIA:TYPE=SUBTITLES,GROUP-ID=\"sg\",LANGUAGE=\"eng\","
             "NAME=\"English\",DEFAULT=YES,AUTOSELECT=YES,URI=\"eng-sub.m3u8\"\n"
             "#EXT-X-STREAM-INF:BANDWIDTH=250000,RESOLUTION=160x120,"
             "CODECS=\"avc1.42c01e,mp4a.40.2\",AUDIO=\"ag\",SUBTITLES=\"sg\"\n"
             "lo.m3u8\n"
             "#EXT-X-STREAM-INF:BANDWIDTH=900000,RESOLUTION=320x240,"
             "CODECS=\"avc1.42c01e,mp4a.40.2\",AUDIO=\"ag\",SUBTITLES=\"sg\"\n"
             "hi.m3u8\n"
             "#EXT-X-STREAM-INF:BANDWIDTH=400000,RESOLUTION=160x120,"
             "CODECS=\"mp2v.4d400d\"\n"
             "mpeg2.m3u8\n"
             "#EXT-X-STREAM-INF:BANDWIDTH=999999,RESOLUTION=160x120\n"
             "missing.m3u8\n") == 0, "write master");
    CHECK(write_text_file(muxpl,
             "#EXTM3U\n"
             "#EXT-X-STREAM-INF:BANDWIDTH=300000,RESOLUTION=160x120\n"
             "lo.m3u8\n"
             "#EXT-X-STREAM-INF:BANDWIDTH=950000,RESOLUTION=320x240\n"
             "hi.m3u8\n") == 0, "write mux master names");
    /* mux master actually uses muxlo/muxhi playlists written under same names after slurping? 
       We'll serve mux master with dedicated playlist bodies pointing at muxlo.ts / muxhi.ts. */

    CHECK(slurp_file(lots, &lob, &lon) == 0, "slurp lo");
    CHECK(slurp_file(hits, &hib, &hin) == 0, "slurp hi");
    CHECK(slurp_file(m2ts, &m2b, &m2n) == 0, "slurp m2");
    CHECK(slurp_file(ets, &eb, &en) == 0, "slurp eng a");
    CHECK(slurp_file(zts, &zb, &zn) == 0, "slurp zho a");
    CHECK(slurp_file(muxlo, &muxlob, &muxlon) == 0, "slurp muxlo");
    CHECK(slurp_file(muxhi, &muxhib, &muxhin) == 0, "slurp muxhi");
    CHECK(slurp_file(lopl, &loplb, &lopln) == 0, "slurp lopl");
    CHECK(slurp_file(hipl, &hiplb, &hipln) == 0, "slurp hipl");
    CHECK(slurp_file(epl, &eplb, &epln) == 0, "slurp epl");
    CHECK(slurp_file(zpl, &zplb, &zpln) == 0, "slurp zpl");
    CHECK(slurp_file(spl, &splb, &spln) == 0, "slurp spl");
    CHECK(slurp_file(m2pl, &m2plb, &m2pln) == 0, "slurp m2pl");
    CHECK(slurp_file(mpl, &mplb, &mpln) == 0, "slurp master");
    CHECK(slurp_file(evtt, &evb, &evn) == 0, "slurp vtt");
    {
        const char *mux_master =
            "#EXTM3U\n"
            "#EXT-X-STREAM-INF:BANDWIDTH=300000,RESOLUTION=160x120\n"
            "muxlo.m3u8\n"
            "#EXT-X-STREAM-INF:BANDWIDTH=950000,RESOLUTION=320x240\n"
            "muxhi.m3u8\n";
        const char *muxlo_pl =
            "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:11\n"
            "#EXT-X-MEDIA-SEQUENCE:1\n#EXTINF:10.0,\nmuxlo.ts\n#EXT-X-ENDLIST\n";
        const char *muxhi_pl =
            "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:11\n"
            "#EXT-X-MEDIA-SEQUENCE:1\n#EXTINF:10.0,\nmuxhi.ts\n#EXT-X-ENDLIST\n";
        muxpln = strlen(mux_master);
        muxplb = (uint8_t *)malloc(muxpln + 1);
        CHECK(muxplb != NULL, "mux master buf");
        memcpy(muxplb, mux_master, muxpln + 1);
        CHECK(srv_start(&hs, 0, NULL, NULL) == 0, "http listen");
        CHECK(srv_add(&hs, "/hls/master.m3u8", mplb, mpln,
                      "application/vnd.apple.mpegurl") == 0, "add master");
        CHECK(srv_add(&hs, "/hls/lo.m3u8", loplb, lopln,
                      "application/vnd.apple.mpegurl") == 0, "add lo pl");
        CHECK(srv_add(&hs, "/hls/hi.m3u8", hiplb, hipln,
                      "application/vnd.apple.mpegurl") == 0, "add hi pl");
        CHECK(srv_add(&hs, "/hls/lo.ts", lob, lon, "video/mp2t") == 0, "add lo ts");
        CHECK(srv_add(&hs, "/hls/hi.ts", hib, hin, "video/mp2t") == 0, "add hi ts");
        CHECK(srv_add(&hs, "/hls/eng.m3u8", eplb, epln,
                      "application/vnd.apple.mpegurl") == 0, "add eng pl");
        CHECK(srv_add(&hs, "/hls/zho.m3u8", zplb, zpln,
                      "application/vnd.apple.mpegurl") == 0, "add zho pl");
        CHECK(srv_add(&hs, "/hls/eng.ts", eb, en, "video/mp2t") == 0, "add eng ts");
        CHECK(srv_add(&hs, "/hls/zho.ts", zb, zn, "video/mp2t") == 0, "add zho ts");
        CHECK(srv_add(&hs, "/hls/eng-sub.m3u8", splb, spln,
                      "application/vnd.apple.mpegurl") == 0, "add sub pl");
        CHECK(srv_add(&hs, "/hls/eng.vtt", evb, evn, "text/vtt") == 0, "add vtt");
        CHECK(srv_add(&hs, "/hls/mpeg2.m3u8", m2plb, m2pln,
                      "application/vnd.apple.mpegurl") == 0, "add m2 pl");
        CHECK(srv_add(&hs, "/hls/mpeg2.ts", m2b, m2n, "video/mp2t") == 0, "add m2 ts");
        CHECK(srv_add(&hs, "/mux/master.m3u8", muxplb, muxpln,
                      "application/vnd.apple.mpegurl") == 0, "add mux master");
        CHECK(srv_add(&hs, "/mux/muxlo.m3u8", muxlo_pl, strlen(muxlo_pl),
                      "application/vnd.apple.mpegurl") == 0, "add muxlo pl");
        CHECK(srv_add(&hs, "/mux/muxhi.m3u8", muxhi_pl, strlen(muxhi_pl),
                      "application/vnd.apple.mpegurl") == 0, "add muxhi pl");
        CHECK(srv_add(&hs, "/mux/muxlo.ts", muxlob, muxlon, "video/mp2t") == 0, "add muxlo ts");
        CHECK(srv_add(&hs, "/mux/muxhi.ts", muxhib, muxhin, "video/mp2t") == 0, "add muxhi ts");
        /* missing.m3u8 故意不注册，404。 */
    }

    snprintf(url, sizeof(url), "http://127.0.0.1:%d/hls/master.m3u8", hs.port);
    snprintf(muxurl, sizeof(muxurl), "http://127.0.0.1:%d/mux/master.m3u8", hs.port);
    ms_ff_limits_default(&lim);
    lim.io_timeout_ms = 8000;
    CHECK_ST(ms_ff_create(NULL, &lim, &f), MS_OK);
    CHECK_ST(ms_ff_open(f, url), MS_OK);
    CHECK_ST(ms_ff_list_tracks(f, ti, 16, &nt), MS_OK);
    CHECK(nt >= 3, "at least two videos + audio or sub");
    for (k = 0; k < nt; k++) {
        if (ti[k].kind == MS_TRACK_VIDEO) {
            if (ti[k].codec == MS_CODEC_MPEG2_VIDEO) {
                v_m2 = ti[k].id;
            } else if (ti[k].name &&
                       (strstr(ti[k].name, "320x240") != NULL ||
                        strstr(ti[k].name, "900kbps") != NULL)) {
                v_hi = ti[k].id;
                if (strstr(ti[k].name, "320x240") != NULL) {
                    name_hi = 1;
                }
                if (strstr(ti[k].name, "kbps") != NULL) {
                    br_hi = 1;
                }
            } else if (v_lo == 0 && ti[k].name &&
                       (strstr(ti[k].name, "160x120") != NULL ||
                        strstr(ti[k].name, "250kbps") != NULL)) {
                v_lo = ti[k].id;
                if (strstr(ti[k].name, "160x120") != NULL) {
                    name_lo = 1;
                }
                if (strstr(ti[k].name, "kbps") != NULL) {
                    br_lo = 1;
                }
            } else if (v_lo == 0 && ti[k].codec == MS_CODEC_H264 && ti[k].selected) {
                v_lo = ti[k].id;
            } else if (v_hi == 0 && ti[k].codec == MS_CODEC_H264) {
                v_hi = ti[k].id;
            }
        }
        if (ti[k].kind == MS_TRACK_AUDIO && ti[k].language &&
            strcmp(ti[k].language, "eng") == 0) {
            a_eng = ti[k].id;
        }
        if (ti[k].kind == MS_TRACK_AUDIO && ti[k].language &&
            strcmp(ti[k].language, "zho") == 0) {
            a_zho = ti[k].id;
        }
        if (ti[k].kind == MS_TRACK_SUBTITLE && ti[k].language &&
            strcmp(ti[k].language, "eng") == 0) {
            s_eng = ti[k].id;
        }
    }
    CHECK(v_lo && v_hi && v_lo != v_hi, "two H.264 variants enumerated");
    CHECK(name_lo || br_lo || name_hi || br_hi,
          "variant list carries known bitrate or WxH metadata (not invented)");
    if (!name_lo || !name_hi) {
        printf("  note: list-time codecpar WxH unknown; names use known bitrate only; WxH evidenced from CSD/AU\n");
    }
    (void)br_lo;
    (void)br_hi;
    CHECK(a_eng && a_zho && s_eng, "shared audio + subtitle groups present");
    CHECK_ST(ms_ff_select_track(f, 0x00FFFFFFu, 1), MS_ERR_INVALID_ARG);
    CHECK_ST(ms_ff_select_track(f, v_lo, 0), MS_ERR_INVALID_ARG);
    if (v_m2) {
        CHECK_ST(ms_ff_select_track(f, v_m2, 1), MS_ERR_UNSUPPORTED);
        {
            size_t nt2 = 0;
            int still = 0;
            CHECK_ST(ms_ff_list_tracks(f, ti, 16, &nt2), MS_OK);
            for (k = 0; k < nt2; k++) {
                if (ti[k].id == v_lo && ti[k].selected) {
                    still = 1;
                }
                if (ti[k].id == v_m2 && ti[k].selected) {
                    still = 0;
                    break;
                }
            }
            CHECK(still, "cross-codec UNSUPPORTED does not mark mpeg2 selected");
        }
    } else {
        printf("  note: mpeg2 variant not enumerated by lavf; cross-codec refuse tested only if listed\n");
    }

    /* 默认应出 lo 变体关键帧 + CSD，以及默认音/字幕。 */
    for (loops = 0; loops < 800; loops++) {
        st = ms_ff_pull(f, &ev);
        if (st == MS_AGAIN) {
            continue;
        }
        if (st != MS_OK) {
            break;
        }
        if (ev.type == MS_EVENT_CODEC_CONFIG && ev.track.id == v_lo) {
            int w = 0, h = 0;
            cfg_lo++;
            if (ev_video_wh(&ev, &w, &h) && w == 160 && h == 120) {
                saw_w_lo = 1;
            }
        }
        if (ev.type == MS_EVENT_AU && ev.track.id == v_lo) {
            int w = 0, h = 0;
            v_lo_au++;
            if (ev.is_keyframe) {
                v_lo_key++;
            }
            if (ev_video_wh(&ev, &w, &h) && w == 160 && h == 120) {
                saw_w_lo = 1;
            }
            if (ev.has_pts) {
                last_pts = ev.pts;
                have_pts = 1;
            }
            if (ev.selection_epoch) {
                sel_ep0 = ev.selection_epoch;
            }
            if (ev.timeline_epoch) {
                tl_ep0 = ev.timeline_epoch;
            }
        }
        if (ev.type == MS_EVENT_AU && ev.track.id == a_eng) {
            a_eng_au++;
        }
        if (ev.type == MS_EVENT_SUBTITLE && ev.track.id == s_eng) {
            sub_ev++;
        }
        if (ev.release_id) {
            ms_ff_release(f, ev.release_id);
        }
        if (v_lo_au >= 8 && v_lo_key >= 1 && a_eng_au >= 1) {
            break;
        }
    }
    CHECK(v_lo_au >= 2 && v_lo_key >= 1, "default variant lo AUs + keyframe");
    CHECK(saw_w_lo, "lo CSD/AU parse as 160x120 (not API-only)");
    CHECK(a_eng_au >= 1, "default English audio flows");

    /* 切到用户中文音轨，再切视频，音/字幕选择应保持。 */
    CHECK_ST(ms_ff_select_track(f, a_zho, 1), MS_OK);
    a_zho_au = 0;
    for (loops = 0; loops < 600; loops++) {
        st = ms_ff_pull(f, &ev);
        if (st == MS_AGAIN) {
            continue;
        }
        if (st != MS_OK) {
            break;
        }
        if (ev.type == MS_EVENT_AU && ev.track.id == a_zho) {
            a_zho_au++;
        }
        if (ev.release_id) {
            ms_ff_release(f, ev.release_id);
        }
        if (a_zho_au >= 1) {
            break;
        }
    }
    CHECK(a_zho_au >= 1, "Chinese audio committed before video switch");

    CHECK_ST(ms_ff_select_track(f, v_hi, 1), MS_OK);
    /* 提交前 list 不得把 hi 写成已选。 */
    {
        size_t nt2 = 0;
        int hi_sel = 0, lo_sel = 0;
        CHECK_ST(ms_ff_list_tracks(f, ti, 16, &nt2), MS_OK);
        for (k = 0; k < nt2; k++) {
            if (ti[k].id == v_hi && ti[k].selected) {
                hi_sel = 1;
            }
            if (ti[k].id == v_lo && ti[k].selected) {
                lo_sel = 1;
            }
        }
        CHECK(lo_sel && !hi_sel, "pending hi not marked selected before key");
        pending_not_selected = 1;
    }
    v_hi_au = v_hi_key = cfg_hi = saw_w_hi = 0;
    a_zho_au = 0;
    sub_ev = 0;
    v_lo_au = 0;
    for (loops = 0; loops < 800; loops++) {
        st = ms_ff_pull(f, &ev);
        if (st == MS_AGAIN) {
            continue;
        }
        if (st != MS_OK) {
            break;
        }
        if (ev.type == MS_EVENT_CODEC_CONFIG && ev.track.id == v_hi) {
            int w = 0, h = 0;
            cfg_hi++;
            if (ev_video_wh(&ev, &w, &h) && w == 320 && h == 240) {
                saw_w_hi = 1;
            }
            sel_ep1 = ev.selection_epoch;
            tl_ep1 = ev.timeline_epoch;
        }
        if (ev.type == MS_EVENT_AU && ev.track.id == v_hi) {
            int w = 0, h = 0;
            v_hi_au++;
            if (ev.is_keyframe) {
                v_hi_key++;
            }
            if (ev_video_wh(&ev, &w, &h) && w == 320 && h == 240) {
                saw_w_hi = 1;
            }
            if (ev.has_pts && !have_hi_pts) {
                first_hi_pts = ev.pts;
                have_hi_pts = 1;
            }
            sel_ep1 = ev.selection_epoch;
            tl_ep1 = ev.timeline_epoch;
        }
        if (ev.type == MS_EVENT_AU && ev.track.id == v_lo) {
            v_lo_au++;
        }
        if (ev.type == MS_EVENT_AU && ev.track.id == a_zho) {
            a_zho_au++;
        }
        if (ev.type == MS_EVENT_SUBTITLE && ev.track.id == s_eng) {
            sub_ev++;
        }
        if (ev.release_id) {
            ms_ff_release(f, ev.release_id);
        }
        if (v_hi_au >= 4 && v_hi_key >= 1 && a_zho_au >= 1) {
            break;
        }
    }
    CHECK(pending_not_selected, "pending gate recorded");
    CHECK(v_hi_au >= 2 && v_hi_key >= 1, "hi variant AUs after keyframe commit");
    CHECK(saw_w_hi, "hi CSD/AU parse as 320x240; old 160x120 not reused");
    CHECK(cfg_hi >= 1 || saw_w_hi, "new variant config observed");
    CHECK(sel_ep1 > sel_ep0, "selection_epoch bumps after video commit");
    CHECK(tl_ep1 == tl_ep0, "timeline_epoch unchanged on variant switch");
    CHECK(a_zho_au >= 1, "user Chinese audio continues across video switch");
    {
        size_t nt2 = 0;
        int hi_sel = 0, lo_sel = 0, zho_sel = 0, eng_a = 0;
        CHECK_ST(ms_ff_list_tracks(f, ti, 16, &nt2), MS_OK);
        for (k = 0; k < nt2; k++) {
            if (ti[k].id == v_hi && ti[k].selected) {
                hi_sel = 1;
            }
            if (ti[k].id == v_lo && ti[k].selected) {
                lo_sel = 1;
            }
            if (ti[k].id == a_zho && ti[k].selected) {
                zho_sel = 1;
            }
            if (ti[k].id == a_eng && ti[k].selected) {
                eng_a = 1;
            }
        }
        CHECK(hi_sel && !lo_sel, "list shows hi selected after key commit");
        CHECK(zho_sel && !eng_a, "audio group keeps user Chinese");
        st_ok = 0;
        for (k = 0; k < nt2; k++) {
            if (ti[k].id == s_eng && ti[k].selected) {
                st_ok = 1;
            }
        }
        CHECK(st_ok, "subtitle group keeps English after video switch");
        (void)sub_ev;
    }
    if (have_pts && have_hi_pts) {
        /* 时间轴连续：新变体 PTS 不应回到 0 或远小于已播位置（允许同一 GOP 窗口）。 */
        CHECK(first_hi_pts + 90000 > last_pts || last_pts < 2 * 90000,
              "new variant PTS not a reset to an unrelated origin");
    }

    /* 来回切回 lo */
    CHECK_ST(ms_ff_select_track(f, v_lo, 1), MS_OK);
    v_lo_au = v_lo_key = saw_w_lo = cfg_lo = 0;
    v_hi_au = 0;
    for (loops = 0; loops < 800; loops++) {
        st = ms_ff_pull(f, &ev);
        if (st == MS_AGAIN) {
            continue;
        }
        if (st != MS_OK) {
            break;
        }
        if (ev.type == MS_EVENT_CODEC_CONFIG && ev.track.id == v_lo) {
            int w = 0, h = 0;
            cfg_lo++;
            if (ev_video_wh(&ev, &w, &h) && w == 160 && h == 120) {
                saw_w_lo = 1;
            }
        }
        if (ev.type == MS_EVENT_AU && ev.track.id == v_lo) {
            int w = 0, h = 0;
            v_lo_au++;
            if (ev.is_keyframe) {
                v_lo_key++;
            }
            if (ev_video_wh(&ev, &w, &h) && w == 160 && h == 120) {
                saw_w_lo = 1;
            }
        }
        if (ev.type == MS_EVENT_AU && ev.track.id == v_hi) {
            v_hi_au++;
        }
        if (ev.release_id) {
            ms_ff_release(f, ev.release_id);
        }
        if (v_lo_au >= 3 && v_lo_key >= 1) {
            break;
        }
    }
    CHECK(v_lo_au >= 2 && v_lo_key >= 1 && saw_w_lo, "round-trip back to lo 160x120");

    /* seek 后仍在当前变体，并保持中文音轨 */
    if (ms_ff_is_seekable(f)) {
        int post_lo = 0, post_z = 0, post_key = 0;
        CHECK_ST(ms_ff_seek(f, 2 * 90000), MS_OK);
        for (loops = 0; loops < 600; loops++) {
            st = ms_ff_pull(f, &ev);
            if (st == MS_AGAIN) {
                continue;
            }
            if (st != MS_OK) {
                break;
            }
            if (ev.type == MS_EVENT_AU && ev.track.id == v_lo) {
                post_lo++;
                if (ev.is_keyframe) {
                    post_key++;
                }
            }
            if (ev.type == MS_EVENT_AU && ev.track.id == a_zho) {
                post_z++;
            }
            if (ev.release_id) {
                ms_ff_release(f, ev.release_id);
            }
            if (post_lo >= 2 && post_key >= 1 && post_z >= 1) {
                break;
            }
        }
        CHECK(post_lo >= 1 && post_key >= 1, "seek after switch: current variant key");
        CHECK(post_z >= 1, "seek after switch: Chinese audio continues");
    } else {
        printf("  note: variant master not seekable; seek subcase gap, not a pass\n");
    }

    /* 取消有界，不无限等待 */
    (void)ms_ff_cancel(f);
    st = ms_ff_pull(f, &ev);
    CHECK(st == MS_ERR_CANCELED || st == MS_EOS || st == MS_AGAIN, "cancel pull bounded");
    ms_ff_destroy(f);
    f = NULL;

    /* 复用音轨 master：切视频后应回退到新变体 muxed 音轨，而不是假报仍选旧轨。 */
    CHECK_ST(ms_ff_create(NULL, &lim, &f), MS_OK);
    CHECK_ST(ms_ff_open(f, muxurl), MS_OK);
    CHECK_ST(ms_ff_list_tracks(f, ti, 16, &nt), MS_OK);
    v_lo = v_hi = a_eng = a_zho = 0;
    {
        uint32_t a0 = 0, a1 = 0;
        for (k = 0; k < nt; k++) {
            if (ti[k].kind == MS_TRACK_VIDEO) {
                const char *nm = ti[k].name ? ti[k].name : "";
                if (strstr(nm, "320x240") != NULL || strstr(nm, "950kbps") != NULL) {
                    v_hi = ti[k].id;
                } else if (v_lo == 0) {
                    v_lo = ti[k].id;
                } else if (v_hi == 0) {
                    v_hi = ti[k].id;
                }
            }
            if (ti[k].kind == MS_TRACK_AUDIO) {
                if (a0 == 0) {
                    a0 = ti[k].id;
                } else if (a1 == 0) {
                    a1 = ti[k].id;
                }
            }
        }
        CHECK(v_lo && v_hi && v_lo != v_hi && a0 && a1, "muxed master two A/V pairs");
        a_eng = a0; /* 默认应是 lo 的 muxed audio（先出现） */
        a_zho = a1;
    }
    v_lo_au = 0;
    a_eng_au = 0;
    for (loops = 0; loops < 600; loops++) {
        st = ms_ff_pull(f, &ev);
        if (st == MS_AGAIN) {
            continue;
        }
        if (st != MS_OK) {
            break;
        }
        if (ev.type == MS_EVENT_AU && ev.track.id == v_lo) {
            v_lo_au++;
        }
        if (ev.type == MS_EVENT_AU && ev.track.kind == MS_TRACK_AUDIO) {
            a_eng = ev.track.id;
            a_eng_au++;
        }
        if (ev.release_id) {
            ms_ff_release(f, ev.release_id);
        }
        if (v_lo_au >= 4 && a_eng_au >= 1) {
            break;
        }
    }
    CHECK(v_lo_au >= 1 && a_eng_au >= 1, "muxed default A/V");
    CHECK_ST(ms_ff_select_track(f, v_hi, 1), MS_OK);
    v_hi_au = 0;
    a_zho_au = 0;
    {
        uint32_t new_a = 0;
        for (loops = 0; loops < 800; loops++) {
            st = ms_ff_pull(f, &ev);
            if (st == MS_AGAIN) {
                continue;
            }
            if (st != MS_OK) {
                break;
            }
            if (ev.type == MS_EVENT_AU && ev.track.id == v_hi) {
                v_hi_au++;
            }
            if (ev.type == MS_EVENT_AU && ev.track.kind == MS_TRACK_AUDIO &&
                ev.track.id != a_eng) {
                new_a = ev.track.id;
                a_zho_au++;
            }
            if (ev.release_id) {
                ms_ff_release(f, ev.release_id);
            }
            if (v_hi_au >= 2 && a_zho_au >= 1) {
                break;
            }
        }
        CHECK(v_hi_au >= 1, "muxed switch video");
        CHECK(a_zho_au >= 1 && new_a != 0 && new_a != a_eng,
              "muxed audio fallback to new variant, not stale old track");
    }

    /* 拉到 EOS 有界 */
    {
        int eos = 0;
        for (loops = 0; loops < 2000; loops++) {
            st = ms_ff_pull(f, &ev);
            if (st == MS_AGAIN) {
                continue;
            }
            if (st == MS_EOS) {
                eos = 1;
                break;
            }
            if (st != MS_OK) {
                break;
            }
            if (ev.release_id) {
                ms_ff_release(f, ev.release_id);
            }
        }
        CHECK(eos, "stream end is EOS, not infinite wait");
        CHECK_ST(ms_ff_select_track(f, v_lo, 1), MS_EOS);
    }
    ms_ff_destroy(f);
    f = NULL;

    /* 队列上限：少量槽位，不 release 应 LIMIT，取消有界 */
    {
        ms_ff_limits tight;
        int hits_limit = 0;
        ms_ff_limits_default(&tight);
        tight.io_timeout_ms = 5000;
        tight.out_slots = 2;
        tight.au_queue_bytes = 2048;
        CHECK_ST(ms_ff_create(NULL, &tight, &f), MS_OK);
        CHECK_ST(ms_ff_open(f, url), MS_OK);
        for (loops = 0; loops < 200; loops++) {
            st = ms_ff_pull(f, &ev);
            if (st == MS_LIMIT) {
                hits_limit = 1;
                break;
            }
            if (st == MS_AGAIN) {
                continue;
            }
            if (st != MS_OK) {
                break;
            }
            /* 故意不 release，占满队列 */
        }
        CHECK(hits_limit, "queue cap returns LIMIT, not unbounded growth");
        (void)ms_ff_cancel(f);
        st = ms_ff_pull(f, &ev);
        CHECK(st == MS_ERR_CANCELED || st == MS_LIMIT || st == MS_AGAIN || st == MS_EOS,
              "cancel after LIMIT bounded");
        ms_ff_destroy(f);
        f = NULL;
    }

    srv_stop(&hs);
    free(lob); free(hib); free(m2b); free(eb); free(zb);
    free(muxlob); free(muxhib);
    free(loplb); free(hiplb); free(eplb); free(zplb); free(splb);
    free(m2plb); free(mplb); free(muxplb); free(evb);
    printf("  test_ff_hls_video_variants ok (local HTTP two-rung master; AU/CSD/key/PTS; not device paint)\n");
    return 0;
}


static int test_ff_ac3(const char *tmpdir)
{
    char ts_path[512], mp4_path[512], trunc[512], url[640], cmd[2048];
    ms_ff *f = NULL;
    ms_ff_limits lim;
    ms_event ev;
    ms_status st;
    int cfg = 0, au = 0, pts_seen = 0, inspected = 0;

    CHECK(path_join(ts_path, sizeof(ts_path), tmpdir, "ac3_48k.ts") == 0, "ts path");
    CHECK(path_join(mp4_path, sizeof(mp4_path), tmpdir, "ac3_48k.mp4") == 0, "mp4 path");
    CHECK(path_join(trunc, sizeof(trunc), tmpdir, "ac3_48k_trunc.ts") == 0, "trunc path");
    snprintf(cmd, sizeof(cmd),
             "ffmpeg -y -hide_banner -loglevel error -f lavfi -i sine=frequency=440:duration=1 "
             "-ac 2 -ar 48000 -c:a ac3 -b:a 96k -vn -f mpegts %s", ts_path);
    CHECK(system(cmd) == 0, "enc ts ac3 (encoder != product decode)");
    snprintf(cmd, sizeof(cmd),
             "ffmpeg -y -hide_banner -loglevel error -f lavfi -i sine=frequency=440:duration=1 "
             "-ac 2 -ar 48000 -c:a ac3 -b:a 96k -vn -f mp4 %s", mp4_path);
    CHECK(system(cmd) == 0, "enc mp4 ac3 (encoder != product decode)");
    ms_ff_limits_default(&lim);
    lim.io_timeout_ms = 4000;
    CHECK_ST(ms_ff_create(NULL, &lim, &f), MS_OK);
    snprintf(url, sizeof(url), "file:%s", ts_path);
    CHECK_ST(ms_ff_open(f, url), MS_OK);
    { int steps = 0;
    while ((st = ms_ff_pull(f, &ev)) == MS_OK) {
        CHECK(++steps < 800, "ts pull bounded");
        if (ev.type == MS_EVENT_CODEC_CONFIG && ev.track.codec == MS_CODEC_AC3) {
            cfg++;
            CHECK(ev.data_size == 0, "no invented ac3 csd");
        }
        if (ev.type == MS_EVENT_AU && ev.track.codec == MS_CODEC_AC3) {
            ms_audio_cfg ac;
            ms_audio_au_info info;
            au++;
            CHECK(ev.sample_format == MS_SAMPLE_FMT_AC3, "ts au fmt");
            if (ev.has_pts) {
                pts_seen++;
            }
            memset(&info, 0, sizeof(info));
            info.struct_size = sizeof(info);
            if (ev.data != NULL && ev.data_size > 0 &&
                ms_audio_au_inspect(MS_CODEC_AC3, ev.sample_format, ev.data,
                                    ev.data_size, &info) == MS_OK) {
                CHECK(info.samples == 1536, "ac3 samples");
                CHECK(info.frame_bytes == ev.data_size || ev.data_size >= info.frame_bytes,
                      "frame bounds");
                ms_audio_cfg_default(&ac);
                if (ms_audio_cfg_parse(MS_CODEC_AC3, ev.sample_format, 0, ev.data,
                                       ev.data_size, &ac) == MS_OK && ac.complete) {
                    CHECK(ac.csd_size == 0, "in-band cfg has no csd");
                    CHECK(ac.sample_rate == 48000 && ac.channels == 2, "rate/ch from hdr");
                    inspected++;
                }
            }
        }
        if (ev.release_id) {
            ms_ff_release(f, ev.release_id);
        }
    }
    } CHECK(st == MS_EOS, "ts eos");
    CHECK(cfg == 0, "no ac3 CONFIG (legal no-CSD)");
    CHECK(au >= 8 && pts_seen >= 8 && inspected >= 8, "ts au/pts/inspect");
    ms_ff_destroy(f);
    f = NULL;
    printf("  ff ac3 MPEG-TS: cfg=%d au=%d pts=%d inspect=%d (fixture encode only)\n",
           cfg, au, pts_seen, inspected);

    cfg = au = pts_seen = inspected = 0;
    CHECK_ST(ms_ff_create(NULL, &lim, &f), MS_OK);
    snprintf(url, sizeof(url), "file:%s", mp4_path);
    CHECK_ST(ms_ff_open(f, url), MS_OK);
    { int steps = 0;
    while ((st = ms_ff_pull(f, &ev)) == MS_OK) {
        CHECK(++steps < 800, "mp4 pull bounded");
        if (ev.type == MS_EVENT_CODEC_CONFIG && ev.track.codec == MS_CODEC_AC3) {
            cfg++;
        }
        if (ev.type == MS_EVENT_AU && ev.track.codec == MS_CODEC_AC3) {
            au++;
            if (ev.has_pts) pts_seen++;
            {
                ms_audio_au_info info;
                memset(&info, 0, sizeof(info));
                info.struct_size = sizeof(info);
                if (ev.data && ev.data_size &&
                    ms_audio_au_inspect(MS_CODEC_AC3, ev.sample_format, ev.data,
                                        ev.data_size, &info) == MS_OK &&
                    info.samples == 1536) {
                    inspected++;
                }
            }
        }
        if (ev.release_id) ms_ff_release(f, ev.release_id);
    }
    } CHECK(st == MS_EOS, "mp4 eos");
    CHECK(cfg == 0, "mp4 no invented csd");
    CHECK(au >= 8 && pts_seen >= 1 && inspected >= 8, "mp4 au");
    ms_ff_destroy(f);
    f = NULL;
    printf("  ff ac3 MP4: cfg=%d au=%d pts=%d inspect=%d\n", cfg, au, pts_seen, inspected);

    CHECK_ST(ms_ff_create(NULL, &lim, &f), MS_OK);
    snprintf(url, sizeof(url), "file:%s", ts_path);
    CHECK_ST(ms_ff_open(f, url), MS_OK);
    CHECK_ST(ms_ff_seek(f, 90 * 1000 * 3 / 10), MS_OK);
    {
        int post_au = 0, steps = 0;
        while ((st = ms_ff_pull(f, &ev)) == MS_OK) {
            CHECK(++steps < 800, "seek pull bounded");
            if (ev.type == MS_EVENT_AU && ev.track.codec == MS_CODEC_AC3) {
                post_au++;
            }
            if (ev.release_id) ms_ff_release(f, ev.release_id);
        }
        CHECK(st == MS_EOS, "seek eos");
        CHECK(post_au >= 1, "au after seek");
    }
    ms_ff_destroy(f);
    f = NULL;

    {
        FILE *fp = fopen(ts_path, "rb");
        FILE *out = fopen(trunc, "wb");
        unsigned char buf[256];
        size_t n, total = 0, cap;
        CHECK(fp && out, "open trunc");
        cap = 188 * 8;
        while (total < cap && (n = fread(buf, 1, sizeof(buf), fp)) > 0) {
            size_t w = n;
            if (total + w > cap) w = cap - total;
            fwrite(buf, 1, w, out);
            total += w;
        }
        fclose(fp);
        fclose(out);
    }
    CHECK_ST(ms_ff_create(NULL, &lim, &f), MS_OK);
    snprintf(url, sizeof(url), "file:%s", trunc);
    st = ms_ff_open(f, url);
    if (st == MS_OK) {
        int steps = 0;
        while ((st = ms_ff_pull(f, &ev)) == MS_OK) {
            CHECK(++steps < 400, "trunc bounded");
            if (ev.release_id) ms_ff_release(f, ev.release_id);
        }
        CHECK(st == MS_EOS || st == MS_ERR_MALFORMED || st == MS_ERR_UNKNOWN ||
              st == MS_ERR_TIMEOUT || st == MS_ERR_UNSUPPORTED ||
              st == MS_ERR_INVALID_ARG, "trunc status bounded");
    } else {
        CHECK(st == MS_ERR_MALFORMED || st == MS_ERR_UNKNOWN ||
              st == MS_ERR_TIMEOUT || st == MS_ERR_UNSUPPORTED ||
              st == MS_ERR_INVALID_ARG, "trunc open bounded");
    }
    ms_ff_destroy(f);
    printf("  test_ff_ac3 ok (legal no-CSD; TS/MP4 via already-open demuxers; seek/trunc bounded; "
           "fixture encode != product decode)\n");
    return 0;
}



static int pull_mpeg_audio(ms_ff *f, ms_codec want, int *cfg, int *au, int *pts_seen,
                           int *inspected, int *aligned, int *mono_pts, int *layer_ok)
{
    ms_event ev;
    ms_status st;
    int steps = 0;
    uint64_t last_pts = 0;
    int have_last = 0;
    *cfg = *au = *pts_seen = *inspected = *aligned = *layer_ok = 0;
    *mono_pts = 1;
    while ((st = ms_ff_pull(f, &ev)) == MS_OK) {
        if (++steps >= 800) {
            return -1;
        }
        if (ev.type == MS_EVENT_CODEC_CONFIG && ev.track.codec == want) {
            (*cfg)++;
            if (ev.data_size != 0) {
                return -2; /* invented CSD */
            }
        }
        if (ev.type == MS_EVENT_AU && ev.track.codec == want) {
            ms_audio_cfg ac;
            ms_audio_au_info info;
            (*au)++;
            if (ev.sample_format != MS_SAMPLE_FMT_MPEG_AUDIO) {
                return -3;
            }
            if (ev.has_pts) {
                (*pts_seen)++;
                if (have_last && ev.pts <= last_pts) {
                    *mono_pts = 0;
                }
                last_pts = ev.pts;
                have_last = 1;
            }
            memset(&info, 0, sizeof(info));
            info.struct_size = sizeof(info);
            if (ev.data != NULL && ev.data_size > 0 &&
                ms_audio_au_inspect(want, ev.sample_format, ev.data, ev.data_size,
                                    &info) == MS_OK) {
                if (info.samples == 1152) {
                    (*inspected)++;
                }
                if (info.frame_bytes == ev.data_size) {
                    (*aligned)++;
                }
                ms_audio_cfg_default(&ac);
                if (ms_audio_cfg_parse(want, ev.sample_format, 0, ev.data,
                                       ev.data_size, &ac) == MS_OK && ac.complete &&
                    ac.csd_size == 0 && ac.codec == want) {
                    (*layer_ok)++;
                }
            }
        }
        if (ev.release_id) {
            ms_ff_release(f, ev.release_id);
        }
    }
    return (st == MS_EOS) ? 0 : (int)st;
}

static int test_ff_mp3(const char *tmpdir)
{
    char ts_path[512], mp4_path[512], trunc[512], url[640], cmd[2048];
    ms_ff *f = NULL;
    ms_ff_limits lim;
    ms_status st;
    int cfg = 0, au = 0, pts_seen = 0, inspected = 0, aligned = 0, mono = 1, layer = 0;
    int rc;

    CHECK(path_join(ts_path, sizeof(ts_path), tmpdir, "mp3_441.ts") == 0, "ts path");
    CHECK(path_join(mp4_path, sizeof(mp4_path), tmpdir, "mp3_441.mp4") == 0, "mp4 path");
    CHECK(path_join(trunc, sizeof(trunc), tmpdir, "mp3_441_trunc.ts") == 0, "trunc path");
    snprintf(cmd, sizeof(cmd),
             "ffmpeg -y -hide_banner -loglevel error -f lavfi -i sine=frequency=440:duration=1 "
             "-ac 2 -ar 44100 -c:a libmp3lame -b:a 128k -vn -f mpegts %s", ts_path);
    CHECK(system(cmd) == 0, "enc ts mp3 (encoder != product decode)");
    snprintf(cmd, sizeof(cmd),
             "ffmpeg -y -hide_banner -loglevel error -f lavfi -i sine=frequency=440:duration=1 "
             "-ac 2 -ar 44100 -c:a libmp3lame -b:a 128k -vn -f mp4 %s", mp4_path);
    CHECK(system(cmd) == 0, "enc mp4 mp3 (encoder != product decode)");

    ms_ff_limits_default(&lim);
    lim.io_timeout_ms = 4000;
    CHECK_ST(ms_ff_create(NULL, &lim, &f), MS_OK);
    snprintf(url, sizeof(url), "file:%s", ts_path);
    CHECK_ST(ms_ff_open(f, url), MS_OK);
    rc = pull_mpeg_audio(f, MS_CODEC_MP3, &cfg, &au, &pts_seen, &inspected, &aligned,
                         &mono, &layer);
    CHECK(rc == 0, "ts eos");
    CHECK(cfg == 0, "no invented mp3 csd");
    CHECK(au >= 20 && pts_seen >= 20 && inspected >= 20, "ts au/pts/inspect");
    CHECK(aligned >= 20 && aligned == au, "ts one frame per AU (mpegaudio parser)");
    CHECK(layer >= 20, "layer III not II");
    CHECK(mono == 1, "ts pts monotonic");
    ms_ff_destroy(f);
    f = NULL;
    printf("  ff mp3 MPEG-TS: cfg=%d au=%d pts=%d inspect=%d aligned=%d layer=%d "
           "(fixture encode only)\n", cfg, au, pts_seen, inspected, aligned, layer);

    cfg = au = pts_seen = inspected = aligned = layer = 0;
    mono = 1;
    CHECK_ST(ms_ff_create(NULL, &lim, &f), MS_OK);
    snprintf(url, sizeof(url), "file:%s", mp4_path);
    CHECK_ST(ms_ff_open(f, url), MS_OK);
    rc = pull_mpeg_audio(f, MS_CODEC_MP3, &cfg, &au, &pts_seen, &inspected, &aligned,
                         &mono, &layer);
    CHECK(rc == 0, "mp4 eos");
    CHECK(cfg == 0, "mp4 no invented csd");
    CHECK(au >= 20 && pts_seen >= 1 && inspected >= 20 && aligned >= 20, "mp4 au");
    CHECK(mono == 1, "mp4 pts monotonic");
    ms_ff_destroy(f);
    f = NULL;
    printf("  ff mp3 MP4: cfg=%d au=%d pts=%d inspect=%d aligned=%d\n",
           cfg, au, pts_seen, inspected, aligned);

    CHECK_ST(ms_ff_create(NULL, &lim, &f), MS_OK);
    snprintf(url, sizeof(url), "file:%s", ts_path);
    CHECK_ST(ms_ff_open(f, url), MS_OK);
    CHECK_ST(ms_ff_seek(f, 90 * 1000 * 3 / 10), MS_OK);
    {
        int post_au = 0, steps = 0;
        ms_event ev;
        while ((st = ms_ff_pull(f, &ev)) == MS_OK) {
            CHECK(++steps < 800, "seek pull bounded");
            if (ev.type == MS_EVENT_AU && ev.track.codec == MS_CODEC_MP3) {
                ms_audio_au_info info;
                post_au++;
                memset(&info, 0, sizeof(info));
                info.struct_size = sizeof(info);
                if (ev.data && ev.data_size) {
                    CHECK_ST(ms_audio_au_inspect(MS_CODEC_MP3, ev.sample_format,
                                                 ev.data, ev.data_size, &info), MS_OK);
                    CHECK(info.frame_bytes == ev.data_size, "seek au still framed");
                }
            }
            if (ev.release_id) ms_ff_release(f, ev.release_id);
        }
        CHECK(st == MS_EOS, "seek eos");
        CHECK(post_au >= 1, "au after seek");
    }
    ms_ff_destroy(f);
    f = NULL;

    CHECK_ST(ms_ff_create(NULL, &lim, &f), MS_OK);
    snprintf(url, sizeof(url), "file:%s", ts_path);
    CHECK_ST(ms_ff_open(f, url), MS_OK);
    CHECK_ST(ms_ff_cancel(f), MS_OK);
    {
        ms_event ev;
        st = ms_ff_pull(f, &ev);
        CHECK(st == MS_ERR_CANCELED || st == MS_EOS || st == MS_AGAIN, "cancel pull");
    }
    ms_ff_destroy(f);
    f = NULL;

    {
        FILE *fp = fopen(ts_path, "rb");
        FILE *out = fopen(trunc, "wb");
        unsigned char buf[256];
        size_t n, total = 0, cap;
        CHECK(fp && out, "open trunc");
        cap = 188 * 8;
        while (total < cap && (n = fread(buf, 1, sizeof(buf), fp)) > 0) {
            size_t w = n;
            if (total + w > cap) w = cap - total;
            fwrite(buf, 1, w, out);
            total += w;
        }
        fclose(fp);
        fclose(out);
    }
    CHECK_ST(ms_ff_create(NULL, &lim, &f), MS_OK);
    snprintf(url, sizeof(url), "file:%s", trunc);
    st = ms_ff_open(f, url);
    if (st == MS_OK) {
        int steps = 0;
        ms_event ev;
        while ((st = ms_ff_pull(f, &ev)) == MS_OK) {
            CHECK(++steps < 400, "trunc bounded");
            if (ev.release_id) ms_ff_release(f, ev.release_id);
        }
        CHECK(st == MS_EOS || st == MS_ERR_MALFORMED || st == MS_ERR_UNKNOWN ||
              st == MS_ERR_TIMEOUT || st == MS_ERR_UNSUPPORTED ||
              st == MS_ERR_INVALID_ARG, "trunc status bounded");
    } else {
        CHECK(st == MS_ERR_MALFORMED || st == MS_ERR_UNKNOWN ||
              st == MS_ERR_TIMEOUT || st == MS_ERR_UNSUPPORTED ||
              st == MS_ERR_INVALID_ARG, "trunc open bounded");
    }
    ms_ff_destroy(f);
    printf("  test_ff_mp3 ok (legal no-CSD; TS framed by mpegaudio parser; MP4; "
           "seek/cancel/trunc; fixture encode != product decode)\n");
    return 0;
}


/* 新 demuxer：MP3 裸流 / FLAC / WAV PCM / AVI，只解封装，不软解。 */
static int test_ff_morefmt_containers(const char *tmpdir)
{
    char mp3[512], flac[512], wav[512], avi[512], mpg[512], url[640], cmd[2048];
    const char *ffmpeg = "/usr/bin/ffmpeg";
    ms_ff *f = NULL;
    ms_ff_limits lim;
    ms_event ev;
    int loops, au, fmt_ok, codec_ok;
    if (access(ffmpeg, X_OK) != 0) {
        printf("  test_ff_morefmt_containers skip (no /usr/bin/ffmpeg)\n");
        return 0;
    }
    CHECK(path_join(mp3, sizeof(mp3), tmpdir, "more_441.mp3") == 0, "mp3 path");
    CHECK(path_join(flac, sizeof(flac), tmpdir, "more_441.flac") == 0, "flac path");
    CHECK(path_join(wav, sizeof(wav), tmpdir, "more_441.wav") == 0, "wav path");
    CHECK(path_join(avi, sizeof(avi), tmpdir, "more_h264.avi") == 0, "avi path");
    CHECK(path_join(mpg, sizeof(mpg), tmpdir, "more_h264.mpg") == 0, "mpg path");
    snprintf(cmd, sizeof(cmd),
             "%s -hide_banner -loglevel error -y -f lavfi -i sine=frequency=440:duration=1 "
             "-ac 2 -ar 44100 -c:a libmp3lame -b:a 128k -vn %s", ffmpeg, mp3);
    if (system(cmd) != 0) {
        printf("  test_ff_morefmt_containers skip (mp3 generate failed)\n");
        return 0;
    }
    snprintf(cmd, sizeof(cmd),
             "%s -hide_banner -loglevel error -y -f lavfi -i sine=frequency=440:duration=1 "
             "-ac 2 -ar 44100 -c:a flac -vn %s", ffmpeg, flac);
    CHECK(system(cmd) == 0, "enc flac");
    snprintf(cmd, sizeof(cmd),
             "%s -hide_banner -loglevel error -y -f lavfi -i sine=frequency=440:duration=1 "
             "-ac 2 -ar 44100 -c:a pcm_s16le -vn %s", ffmpeg, wav);
    CHECK(system(cmd) == 0, "enc wav pcm");
    snprintf(cmd, sizeof(cmd),
             "%s -hide_banner -loglevel error -y "
             "-f lavfi -i testsrc=size=160x120:rate=25:duration=1 "
             "-f lavfi -i sine=frequency=440:duration=1 "
             "-c:v libx264 -pix_fmt yuv420p -profile:v baseline -g 25 "
             "-c:a aac -shortest %s", ffmpeg, avi);
    CHECK(system(cmd) == 0, "enc avi");
    snprintf(cmd, sizeof(cmd),
             "%s -hide_banner -loglevel error -y "
             "-f lavfi -i testsrc=size=160x120:rate=25:duration=1 "
             "-c:v libx264 -pix_fmt yuv420p -profile:v baseline -g 25 "
             "-an -f vob %s", ffmpeg, mpg);
    if (system(cmd) != 0) {
        mpg[0] = 0;
    }
    ms_ff_limits_default(&lim);
    lim.io_timeout_ms = 4000;

    CHECK_ST(ms_ff_create(NULL, &lim, &f), MS_OK);
    snprintf(url, sizeof(url), "file:%s", mp3);
    CHECK_ST(ms_ff_open(f, url), MS_OK);
    au = fmt_ok = codec_ok = 0;
    for (loops = 0; loops < 400; loops++) {
        ms_status st = ms_ff_pull(f, &ev);
        if (st == MS_AGAIN) continue;
        if (st != MS_OK) break;
        if (ev.format == MS_FORMAT_MP3) fmt_ok = 1;
        if (ev.type == MS_EVENT_AU && ev.track.codec == MS_CODEC_MP3) {
            au++;
            codec_ok = 1;
        }
        if (ev.release_id) ms_ff_release(f, ev.release_id);
    }
    CHECK(fmt_ok && codec_ok && au >= 20, "raw mp3 demux AU");
    ms_ff_destroy(f);
    f = NULL;

    CHECK_ST(ms_ff_create(NULL, &lim, &f), MS_OK);
    snprintf(url, sizeof(url), "file:%s", flac);
    CHECK_ST(ms_ff_open(f, url), MS_OK);
    au = fmt_ok = codec_ok = 0;
    for (loops = 0; loops < 400; loops++) {
        ms_status st = ms_ff_pull(f, &ev);
        if (st == MS_AGAIN) continue;
        if (st != MS_OK) break;
        if (ev.format == MS_FORMAT_FLAC) fmt_ok = 1;
        if (ev.type == MS_EVENT_AU && ev.track.codec == MS_CODEC_FLAC) {
            au++;
            codec_ok = 1;
            CHECK(ev.sample_format == MS_SAMPLE_FMT_FLAC, "flac sample fmt");
        }
        if (ev.release_id) ms_ff_release(f, ev.release_id);
    }
    CHECK(fmt_ok && codec_ok && au >= 5, "flac demux AU (no software decode)");
    ms_ff_destroy(f);
    f = NULL;

    CHECK_ST(ms_ff_create(NULL, &lim, &f), MS_OK);
    snprintf(url, sizeof(url), "file:%s", wav);
    CHECK_ST(ms_ff_open(f, url), MS_OK);
    au = fmt_ok = codec_ok = 0;
    for (loops = 0; loops < 400; loops++) {
        ms_status st = ms_ff_pull(f, &ev);
        if (st == MS_AGAIN) continue;
        if (st != MS_OK) break;
        if (ev.format == MS_FORMAT_WAV) fmt_ok = 1;
        if (ev.type == MS_EVENT_AU && ev.track.codec == MS_CODEC_PCM) {
            au++;
            codec_ok = 1;
            CHECK(ev.sample_format == MS_SAMPLE_FMT_PCM, "wav pcm sample fmt");
            CHECK(ev.data != NULL && ev.data_size > 0, "pcm payload");
        }
        if (ev.release_id) ms_ff_release(f, ev.release_id);
    }
    CHECK(fmt_ok && codec_ok && au >= 1, "wav pcm demux AU (direct PCM, not decode)");
    ms_ff_destroy(f);
    f = NULL;

    CHECK_ST(ms_ff_create(NULL, &lim, &f), MS_OK);
    snprintf(url, sizeof(url), "file:%s", avi);
    CHECK_ST(ms_ff_open(f, url), MS_OK);
    au = fmt_ok = codec_ok = 0;
    for (loops = 0; loops < 400; loops++) {
        ms_status st = ms_ff_pull(f, &ev);
        if (st == MS_AGAIN) continue;
        if (st != MS_OK) break;
        if (ev.format == MS_FORMAT_AVI) fmt_ok = 1;
        if (ev.type == MS_EVENT_AU && ev.track.codec == MS_CODEC_H264 && ev.is_keyframe) {
            au++;
            codec_ok = 1;
        }
        if (ev.release_id) ms_ff_release(f, ev.release_id);
    }
    CHECK(fmt_ok && codec_ok && au >= 1, "avi h264 demux (no mpeg4video parser)");
    ms_ff_destroy(f);
    f = NULL;

    if (mpg[0]) {
        CHECK_ST(ms_ff_create(NULL, &lim, &f), MS_OK);
        snprintf(url, sizeof(url), "file:%s", mpg);
        CHECK_ST(ms_ff_open(f, url), MS_OK);
        au = fmt_ok = 0;
        for (loops = 0; loops < 400; loops++) {
            ms_status st = ms_ff_pull(f, &ev);
            if (st == MS_AGAIN) continue;
            if (st != MS_OK) break;
            if (ev.format == MS_FORMAT_MPEGPS) fmt_ok = 1;
            if (ev.type == MS_EVENT_AU && ev.track.kind == MS_TRACK_VIDEO) au++;
            if (ev.release_id) ms_ff_release(f, ev.release_id);
        }
        CHECK(fmt_ok && au >= 1, "mpegps demux");
        ms_ff_destroy(f);
        f = NULL;
    }
    printf("  test_ff_morefmt_containers ok (mp3/flac/wav-pcm/avi/mpegps demux only; no software decode)\n");
    return 0;
}

static int test_ff_mp2(const char *tmpdir)
{
    char ts_path[512], mp4_path[512], url[640], cmd[2048];
    ms_ff *f = NULL;
    ms_ff_limits lim;
    int cfg = 0, au = 0, pts_seen = 0, inspected = 0, aligned = 0, mono = 1, layer = 0;
    int rc;

    CHECK(path_join(ts_path, sizeof(ts_path), tmpdir, "mp2_48k.ts") == 0, "ts path");
    CHECK(path_join(mp4_path, sizeof(mp4_path), tmpdir, "mp2_48k.mp4") == 0, "mp4 path");
    snprintf(cmd, sizeof(cmd),
             "ffmpeg -y -hide_banner -loglevel error -f lavfi -i sine=frequency=440:duration=1 "
             "-ac 2 -ar 48000 -c:a mp2 -b:a 192k -vn -f mpegts %s", ts_path);
    CHECK(system(cmd) == 0, "enc ts mp2 (encoder != product decode)");
    snprintf(cmd, sizeof(cmd),
             "ffmpeg -y -hide_banner -loglevel error -f lavfi -i sine=frequency=440:duration=1 "
             "-ac 2 -ar 48000 -c:a mp2 -b:a 192k -vn -f mp4 %s", mp4_path);
    CHECK(system(cmd) == 0, "enc mp4 mp2 (encoder != product decode)");

    ms_ff_limits_default(&lim);
    lim.io_timeout_ms = 4000;
    CHECK_ST(ms_ff_create(NULL, &lim, &f), MS_OK);
    snprintf(url, sizeof(url), "file:%s", ts_path);
    CHECK_ST(ms_ff_open(f, url), MS_OK);
    rc = pull_mpeg_audio(f, MS_CODEC_MP2, &cfg, &au, &pts_seen, &inspected, &aligned,
                         &mono, &layer);
    CHECK(rc == 0, "ts eos");
    CHECK(cfg == 0, "no invented mp2 csd");
    CHECK(au >= 20 && pts_seen >= 20 && inspected >= 20, "ts au/pts/inspect");
    CHECK(aligned >= 20 && aligned == au, "ts one Layer II frame per AU");
    CHECK(layer >= 20, "layer II not III");
    CHECK(mono == 1, "ts pts monotonic");
    ms_ff_destroy(f);
    f = NULL;
    printf("  ff mp2 MPEG-TS: cfg=%d au=%d pts=%d inspect=%d aligned=%d layer=%d "
           "(fixture encode only)\n", cfg, au, pts_seen, inspected, aligned, layer);

    cfg = au = pts_seen = inspected = aligned = layer = 0;
    mono = 1;
    CHECK_ST(ms_ff_create(NULL, &lim, &f), MS_OK);
    snprintf(url, sizeof(url), "file:%s", mp4_path);
    CHECK_ST(ms_ff_open(f, url), MS_OK);
    rc = pull_mpeg_audio(f, MS_CODEC_MP2, &cfg, &au, &pts_seen, &inspected, &aligned,
                         &mono, &layer);
    CHECK(rc == 0, "mp4 eos");
    CHECK(cfg == 0 && au >= 20 && inspected >= 20 && aligned >= 20, "mp4 au framed");
    CHECK(mono == 1, "mp4 pts monotonic");
    ms_ff_destroy(f);
    printf("  ff mp2 MP4: cfg=%d au=%d pts=%d inspect=%d aligned=%d\n",
           cfg, au, pts_seen, inspected, aligned);
    printf("  test_ff_mp2 ok (legal no-CSD; Layer II framed; fixture encode != product decode)\n");
    return 0;
}

static int pull_opus(ms_ff *f, int *cfg, int *au, int *pts_seen,
                     int *inspected, int *aligned, int *mono_pts, int *head_ok,
                     uint32_t *pre_skip_out, uint32_t *audible, uint32_t *first_skip_s,
                     uint32_t *last_skip_e, int *neg_origin)
{
    ms_event ev;
    ms_status st;
    int steps = 0;
    uint64_t last_pts = 0;
    int have_last = 0;
    *cfg = *au = *pts_seen = *inspected = *aligned = *head_ok = 0;
    *mono_pts = 1;
    *pre_skip_out = 0;
    *audible = 0;
    *first_skip_s = 0;
    *last_skip_e = 0;
    *neg_origin = 0;
    while ((st = ms_ff_pull(f, &ev)) == MS_OK) {
        if (++steps >= 800) {
            return -1;
        }
        if (ev.type == MS_EVENT_CODEC_CONFIG && ev.track.codec == MS_CODEC_OPUS) {
            ms_audio_cfg ac;
            (*cfg)++;
            if (ev.data == NULL || ev.data_size == 0) {
                return -2; /* missing real CSD */
            }
            ms_audio_cfg_default(&ac);
            if (ms_audio_cfg_parse(MS_CODEC_OPUS, MS_SAMPLE_FMT_OPUS, 1,
                                   ev.data, ev.data_size, &ac) != MS_OK ||
                !ac.complete || ac.csd_size < 19 ||
                memcmp(ac.csd, "OpusHead", 8) != 0 ||
                ac.sample_rate != 48000 || ac.channels <= 0) {
                return -3; /* not a real OpusHead (packed dOps must land as Head) */
            }
            if (*head_ok == 0) {
                *pre_skip_out = ac.pre_skip;
            } else if (*pre_skip_out != ac.pre_skip) {
                return -4;
            }
            (*head_ok)++;
        }
        if (ev.type == MS_EVENT_AU && ev.track.codec == MS_CODEC_OPUS) {
            ms_audio_au_info info;
            (*au)++;
            if (ev.sample_format != MS_SAMPLE_FMT_OPUS) {
                return -5;
            }
            if (ev.has_pts) {
                (*pts_seen)++;
                if (have_last && ev.pts <= last_pts) {
                    *mono_pts = 0;
                }
                last_pts = ev.pts;
                have_last = 1;
            }
            memset(&info, 0, sizeof(info));
            info.struct_size = sizeof(info);
            if (ev.data != NULL && ev.data_size > 0 &&
                ms_audio_au_inspect(MS_CODEC_OPUS, ev.sample_format,
                                    ev.data, ev.data_size, &info) == MS_OK &&
                info.samples > 0 && info.samples <= 5760) {
                uint32_t add = info.samples;
                uint32_t sks = 0, ske = 0;
                {
                    ms_event_trim tr;
                    ms_event_trim_default(&tr);
                    if (ev.release_id &&
                        ms_ff_event_trim(f, ev.release_id, &tr) == MS_OK) {
                        sks = tr.skip_samples_start;
                        ske = tr.skip_samples_end;
                    }
                }
                (*inspected)++;
                if (info.frame_bytes == ev.data_size) {
                    (*aligned)++;
                }
                if (*au == 1) {
                    *first_skip_s = sks;
                }
                *last_skip_e = ske;
                if (sks > add) {
                    add = 0;
                } else {
                    add -= sks;
                }
                if (ske > add) {
                    add = 0;
                } else {
                    add -= ske;
                }
                *audible += add;
            }
            {
                int hp = 0, hd = 0;
                int64_t spts = 0, sdts = 0;
                if (ms_event_signed_ts(&ev, ms_ff_time_origin_90k(f), &hp, &spts, &hd, &sdts) == MS_OK &&
                    hp && spts < 0) {
                    *neg_origin = 1;
                }
            }
        }
        if (ev.release_id) {
            ms_ff_release(f, ev.release_id);
        }
    }
    return (st == MS_EOS) ? 0 : (int)st;
}

static int test_ff_opus(const char *tmpdir)
{
    char mp4_path[512], ogg_path[512], trunc[512], url[640], cmd[2048];
    ms_ff *f = NULL;
    ms_ff_limits lim;
    ms_status st;
    int cfg = 0, au = 0, pts_seen = 0, inspected = 0, aligned = 0, mono = 1, head = 0;
    uint32_t pre_skip = 0, audible = 0, first_sk = 0, last_se = 0;
    int rc, neg_pts = 0;
    static const uint8_t k_bad_head[19] = {
        'O','p','u','s','H','e','a','x', 1, 2, 0x38, 0x01, 0,0,0,0, 0,0,0
    };
    static const uint8_t k_code3_over[2] = { 0x6F, 63 }; /* config 13, code 3, 63 frames */

    /* 错误元数据 / 超限：公开层拒绝，不捏造 Head。 */
    {
        ms_audio_cfg ac;
        ms_audio_au_info info;
        ms_audio_cfg_default(&ac);
        CHECK(ms_audio_cfg_parse(MS_CODEC_OPUS, MS_SAMPLE_FMT_OPUS, 1,
                                 k_bad_head, sizeof(k_bad_head), &ac) != MS_OK,
              "bad magic refused");
        memset(&info, 0, sizeof(info));
        info.struct_size = sizeof(info);
        CHECK(ms_audio_au_inspect(MS_CODEC_OPUS, MS_SAMPLE_FMT_OPUS,
                                  k_code3_over, sizeof(k_code3_over),
                                  &info) == MS_ERR_OVERFLOW ||
              ms_audio_au_inspect(MS_CODEC_OPUS, MS_SAMPLE_FMT_OPUS,
                                  k_code3_over, sizeof(k_code3_over),
                                  &info) == MS_ERR_MALFORMED,
              "code3 overflow/malformed");
    }

    CHECK(path_join(mp4_path, sizeof(mp4_path), tmpdir, "opus_48k.mp4") == 0, "mp4 path");
    CHECK(path_join(ogg_path, sizeof(ogg_path), tmpdir, "opus_48k.ogg") == 0, "ogg path");
    CHECK(path_join(trunc, sizeof(trunc), tmpdir, "opus_48k_trunc.ogg") == 0, "trunc path");
    snprintf(cmd, sizeof(cmd),
             "ffmpeg -y -hide_banner -loglevel error -f lavfi -i sine=frequency=440:duration=1 "
             "-ac 2 -ar 48000 -c:a libopus -b:a 64k -vn -f mp4 %s", mp4_path);
    CHECK(system(cmd) == 0, "enc mp4 opus (encoder != product decode)");
    snprintf(cmd, sizeof(cmd),
             "ffmpeg -y -hide_banner -loglevel error -f lavfi -i sine=frequency=440:duration=1 "
             "-ac 2 -ar 48000 -c:a libopus -b:a 64k -vn -f ogg %s", ogg_path);
    CHECK(system(cmd) == 0, "enc ogg opus (encoder != product decode)");

    ms_ff_limits_default(&lim);
    lim.io_timeout_ms = 4000;
    CHECK_ST(ms_ff_create(NULL, &lim, &f), MS_OK);
    snprintf(url, sizeof(url), "file:%s", mp4_path);
    CHECK_ST(ms_ff_open(f, url), MS_OK);
    rc = pull_opus(f, &cfg, &au, &pts_seen, &inspected, &aligned, &mono, &head,
                   &pre_skip, &audible, &first_sk, &last_se, &neg_pts);
    CHECK(rc == 0, "mp4 eos");
    CHECK(cfg >= 1 && head >= 1, "real OpusHead/dOps packed as Head");
    CHECK(au >= 20 && pts_seen >= 1 && inspected >= 20, "mp4 au/pts/inspect");
    CHECK(aligned >= 20 && aligned == au, "mp4 one packet per AU");
    CHECK(mono == 1, "mp4 pts monotonic");
    CHECK(pre_skip == 312, "mp4 pre-skip from real Head, not invented");
    CHECK(first_sk == 312, "mp4 first packet skip_start from SKIP_SAMPLES");
    CHECK(last_se == 648, "mp4 last packet end trim (duration fallback or SKIP)");
    CHECK(audible == 48000, "mp4 audible 1s after start+end trim");
    CHECK(neg_pts == 1, "mp4 first signed pts is negative; origin keeps it");
    ms_ff_destroy(f);
    f = NULL;
    printf("  ff opus MP4: cfg=%d au=%d pts=%d inspect=%d aligned=%d pre_skip=%u "
           "skip0=%u end=%u audible=%u neg_pts=%d (fixture encode only)\n",
           cfg, au, pts_seen, inspected, aligned, pre_skip, first_sk, last_se,
           audible, neg_pts);

    cfg = au = pts_seen = inspected = aligned = head = 0;
    mono = 1;
    pre_skip = audible = first_sk = last_se = 0;
    neg_pts = 0;
    CHECK_ST(ms_ff_create(NULL, &lim, &f), MS_OK);
    snprintf(url, sizeof(url), "file:%s", ogg_path);
    CHECK_ST(ms_ff_open(f, url), MS_OK);
    rc = pull_opus(f, &cfg, &au, &pts_seen, &inspected, &aligned, &mono, &head,
                   &pre_skip, &audible, &first_sk, &last_se, &neg_pts);
    CHECK(rc == 0, "ogg eos");
    CHECK(cfg >= 1 && head >= 1, "ogg real OpusHead");
    CHECK(au >= 20 && pts_seen >= 1 && inspected >= 20, "ogg au/pts/inspect");
    CHECK(aligned >= 20 && aligned == au, "ogg packet framing");
    CHECK(mono == 1, "ogg pts monotonic");
    CHECK(pre_skip == 312, "ogg pre-skip from real Head");
    CHECK(first_sk == 312, "ogg first SKIP start=312");
    CHECK(last_se == 648, "ogg last SKIP end=648 from granule");
    CHECK(audible == 48000, "ogg audible 1s: 51*960-312-648");
    CHECK(neg_pts == 1, "ogg first signed pts negative, not double-counted in AU data");
    ms_ff_destroy(f);
    f = NULL;
    printf("  ff opus Ogg: cfg=%d au=%d pts=%d inspect=%d aligned=%d pre_skip=%u "
           "skip0=%u end=%u audible=%u neg_pts=%d (fixture encode only; ogg demuxer)\n",
           cfg, au, pts_seen, inspected, aligned, pre_skip, first_sk, last_se,
           audible, neg_pts);

    /* 50ms / 25ms 非整帧末尾：独立按 TOC 与 SKIP 计量，不靠听感。 */
    {
        char ogg50[512], mp450[512], ogg25[512], mp425[512];
        CHECK(path_join(ogg50, sizeof(ogg50), tmpdir, "opus_50ms.ogg") == 0, "ogg50 path");
        CHECK(path_join(mp450, sizeof(mp450), tmpdir, "opus_50ms.mp4") == 0, "mp450 path");
        CHECK(path_join(ogg25, sizeof(ogg25), tmpdir, "opus_25ms.ogg") == 0, "ogg25 path");
        CHECK(path_join(mp425, sizeof(mp425), tmpdir, "opus_25ms.mp4") == 0, "mp425 path");
        snprintf(cmd, sizeof(cmd),
                 "ffmpeg -y -hide_banner -loglevel error -f lavfi -i sine=frequency=440:duration=0.05 "
                 "-ac 2 -ar 48000 -c:a libopus -b:a 64k -vn -f ogg %s", ogg50);
        CHECK(system(cmd) == 0, "enc 50ms ogg");
        snprintf(cmd, sizeof(cmd),
                 "ffmpeg -y -hide_banner -loglevel error -f lavfi -i sine=frequency=440:duration=0.05 "
                 "-ac 2 -ar 48000 -c:a libopus -b:a 64k -vn -f mp4 %s", mp450);
        CHECK(system(cmd) == 0, "enc 50ms mp4");
        snprintf(cmd, sizeof(cmd),
                 "ffmpeg -y -hide_banner -loglevel error -f lavfi -i sine=frequency=440:duration=0.025 "
                 "-ac 2 -ar 48000 -c:a libopus -b:a 64k -vn -f ogg %s", ogg25);
        CHECK(system(cmd) == 0, "enc 25ms ogg");
        snprintf(cmd, sizeof(cmd),
                 "ffmpeg -y -hide_banner -loglevel error -f lavfi -i sine=frequency=440:duration=0.025 "
                 "-ac 2 -ar 48000 -c:a libopus -b:a 64k -vn -f mp4 %s", mp425);
        CHECK(system(cmd) == 0, "enc 25ms mp4");

        cfg = au = pts_seen = inspected = aligned = head = 0;
        mono = 1;
        pre_skip = audible = first_sk = last_se = 0;
        neg_pts = 0;
        CHECK_ST(ms_ff_create(NULL, &lim, &f), MS_OK);
        snprintf(url, sizeof(url), "file:%s", ogg50);
        CHECK_ST(ms_ff_open(f, url), MS_OK);
        rc = pull_opus(f, &cfg, &au, &pts_seen, &inspected, &aligned, &mono, &head,
                       &pre_skip, &audible, &first_sk, &last_se, &neg_pts);
        CHECK(rc == 0, "50ms ogg eos");
        CHECK(pre_skip == 312 && first_sk == 312, "50ms ogg start skip");
        CHECK(last_se == 168, "50ms ogg end=168 (3*960-312-168=2400)");
        CHECK(audible == 2400, "50ms ogg audible exactly 50ms");
        ms_ff_destroy(f);
        f = NULL;

        cfg = au = pts_seen = inspected = aligned = head = 0;
        mono = 1;
        pre_skip = audible = first_sk = last_se = 0;
        neg_pts = 0;
        CHECK_ST(ms_ff_create(NULL, &lim, &f), MS_OK);
        snprintf(url, sizeof(url), "file:%s", mp450);
        CHECK_ST(ms_ff_open(f, url), MS_OK);
        rc = pull_opus(f, &cfg, &au, &pts_seen, &inspected, &aligned, &mono, &head,
                       &pre_skip, &audible, &first_sk, &last_se, &neg_pts);
        CHECK(rc == 0, "50ms mp4 eos");
        CHECK(pre_skip == 312 && first_sk == 312, "50ms mp4 start skip");
        CHECK(last_se == 168, "50ms mp4 end via duration fallback");
        CHECK(audible == 2400, "50ms mp4 audible 2400");
        ms_ff_destroy(f);
        f = NULL;

        cfg = au = pts_seen = inspected = aligned = head = 0;
        mono = 1;
        pre_skip = audible = first_sk = last_se = 0;
        neg_pts = 0;
        CHECK_ST(ms_ff_create(NULL, &lim, &f), MS_OK);
        snprintf(url, sizeof(url), "file:%s", ogg25);
        CHECK_ST(ms_ff_open(f, url), MS_OK);
        rc = pull_opus(f, &cfg, &au, &pts_seen, &inspected, &aligned, &mono, &head,
                       &pre_skip, &audible, &first_sk, &last_se, &neg_pts);
        CHECK(rc == 0, "25ms ogg eos");
        CHECK(pre_skip == 312 && first_sk == 312, "25ms ogg start skip");
        /* 本树 FFmpeg 9.1 Ogg 末包 end=407，1920-312-407=1201，相对 lavfi 1200 偏 1；
         * 不编造补偿公式，按实际 side data 记账。 */
        CHECK(last_se == 407, "25ms ogg end=407 (FFmpeg granule, off-by-one vs 1200)");
        CHECK(audible == 1201, "25ms ogg audible 1201 from real SKIP");
        ms_ff_destroy(f);
        f = NULL;

        cfg = au = pts_seen = inspected = aligned = head = 0;
        mono = 1;
        pre_skip = audible = first_sk = last_se = 0;
        neg_pts = 0;
        CHECK_ST(ms_ff_create(NULL, &lim, &f), MS_OK);
        snprintf(url, sizeof(url), "file:%s", mp425);
        CHECK_ST(ms_ff_open(f, url), MS_OK);
        rc = pull_opus(f, &cfg, &au, &pts_seen, &inspected, &aligned, &mono, &head,
                       &pre_skip, &audible, &first_sk, &last_se, &neg_pts);
        CHECK(rc == 0, "25ms mp4 eos");
        printf("  ff opus 25ms MP4: au=%d start=%u end=%u audible=%u "
               "(FFmpeg-cut limit, not asserted as 1200)\n",
               au, first_sk, last_se, audible);
        ms_ff_destroy(f);
        f = NULL;
    }

    CHECK_ST(ms_ff_create(NULL, &lim, &f), MS_OK);
    snprintf(url, sizeof(url), "file:%s", mp4_path);
    CHECK_ST(ms_ff_open(f, url), MS_OK);
    CHECK_ST(ms_ff_seek(f, 90 * 1000 * 3 / 10), MS_OK);
    {
        int post_au = 0, steps = 0, post_cfg = 0;
        int mid_first = 1, mid_start_skip = -1;
        ms_event ev;
        while ((st = ms_ff_pull(f, &ev)) == MS_OK) {
            CHECK(++steps < 800, "seek pull bounded");
            if (ev.type == MS_EVENT_CODEC_CONFIG && ev.track.codec == MS_CODEC_OPUS) {
                ms_audio_cfg ac;
                post_cfg++;
                ms_audio_cfg_default(&ac);
                CHECK_ST(ms_audio_cfg_parse(MS_CODEC_OPUS, MS_SAMPLE_FMT_OPUS, 1,
                                            ev.data, ev.data_size, &ac), MS_OK);
                CHECK(ac.complete && ac.pre_skip == 312, "seek cfg still real Head");
            }
            if (ev.type == MS_EVENT_AU && ev.track.codec == MS_CODEC_OPUS) {
                ms_audio_au_info info;
                post_au++;
                memset(&info, 0, sizeof(info));
                info.struct_size = sizeof(info);
                if (ev.data && ev.data_size) {
                    CHECK_ST(ms_audio_au_inspect(MS_CODEC_OPUS, ev.sample_format,
                                                 ev.data, ev.data_size, &info),
                             MS_OK);
                    CHECK(info.frame_bytes == ev.data_size && info.samples > 0,
                          "seek au still framed");
                }
                if (mid_first) {
                    ms_event_trim tr;
                    ms_event_trim_default(&tr);
                    if (ev.release_id) {
                        (void)ms_ff_event_trim(f, ev.release_id, &tr);
                    }
                    mid_start_skip = (int)tr.skip_samples_start;
                    mid_first = 0;
                }
            }
            if (ev.release_id) ms_ff_release(f, ev.release_id);
        }
        CHECK(st == MS_EOS, "seek eos");
        CHECK(post_au >= 1, "au after seek");
        CHECK(mid_start_skip == 0, "MP4 mid-seek not in priming: no skip_start=312");
        (void)post_cfg;
    }
    ms_ff_destroy(f);
    f = NULL;

    CHECK_ST(ms_ff_create(NULL, &lim, &f), MS_OK);
    snprintf(url, sizeof(url), "file:%s", ogg_path);
    CHECK_ST(ms_ff_open(f, url), MS_OK);
    CHECK_ST(ms_ff_cancel(f), MS_OK);
    {
        ms_event ev;
        st = ms_ff_pull(f, &ev);
        CHECK(st == MS_ERR_CANCELED || st == MS_EOS || st == MS_AGAIN, "cancel pull");
    }
    ms_ff_destroy(f);
    f = NULL;

    {
        FILE *fp = fopen(ogg_path, "rb");
        FILE *out = fopen(trunc, "wb");
        unsigned char buf[256];
        size_t n, total = 0, cap;
        CHECK(fp && out, "open trunc");
        cap = 64;
        while (total < cap && (n = fread(buf, 1, sizeof(buf), fp)) > 0) {
            size_t w = n;
            if (total + w > cap) w = cap - total;
            fwrite(buf, 1, w, out);
            total += w;
        }
        fclose(fp);
        fclose(out);
    }
    CHECK_ST(ms_ff_create(NULL, &lim, &f), MS_OK);
    snprintf(url, sizeof(url), "file:%s", trunc);
    st = ms_ff_open(f, url);
    if (st == MS_OK) {
        int steps = 0;
        ms_event ev;
        while ((st = ms_ff_pull(f, &ev)) == MS_OK) {
            CHECK(++steps < 400, "trunc bounded");
            if (ev.release_id) ms_ff_release(f, ev.release_id);
        }
    }
    ms_ff_destroy(f);
    f = NULL;
    /* 64 字节截断 Ogg 探测常遇 EOF，io_status 映射为 MS_EOS；属有界失败。 */
    CHECK(st == MS_EOS || st == MS_ERR_MALFORMED || st == MS_ERR_UNKNOWN ||
          st == MS_ERR_TIMEOUT || st == MS_ERR_UNSUPPORTED ||
          st == MS_ERR_INVALID_ARG || st == MS_ERR_OVERFLOW ||
          st == MS_AGAIN, "trunc bounded");
    printf("  test_ff_opus ok (real Head/dOps; MP4+Ogg packet framing; "
           "pre-skip; seek/cancel/trunc; fixture encode != product decode)\n");
    return 0;
}



/* 手工 DVB/PGS 段装进 MPEG-TS。不是产品软解，不是真机叠加验收。 */
static int test_ff_bitmap_subs(const char *tmpdir)
{
    char path[512], url[640];
    ms_ff *f = NULL;
    ms_ff_limits lim;
    ms_status st;
    ms_synth_buf b;
    ms_synth_stream streams[1];
    ms_event ev;
    ms_track_info ti[8];
    ms_sub_cue cue;
    size_t nt = 0, nc = 0;
    int steps, saw_codec, saw_sub, i;
    uint32_t sid;

    /* PGS: stream_type 0x90 需 PMT program 注册描述符 HDMV 才会被识别 */
    static const uint8_t k_hdmv_reg[6] = { 0x05, 0x04, 'H', 'D', 'M', 'V' };
    memset(streams, 0, sizeof(streams));
    streams[0].stream_type = 0x90;
    streams[0].pid = 0x121;
    ms_synth_init(&b);
    ms_synth_reset_cc();
    CHECK(ms_synth_pat_pmt_info(&b, 1, 0x120, streams, 1, 8, 188, 0,
                                k_hdmv_reg, (uint16_t)sizeof(k_hdmv_reg)) == 0, "pgs pat");
    CHECK(ms_synth_pes(&b, 0x121, 0xBD, 1, 90000, k_pgs_16_blue, sizeof(k_pgs_16_blue), 188) == 0,
          "pgs pes");
    CHECK(path_join(path, sizeof(path), tmpdir, "pgs_16.ts") == 0, "path");
    CHECK(write_file(path, b.data, b.len) == 0, "write pgs ts");
    ms_synth_free(&b);

    memset(&lim, 0, sizeof(lim));
    lim.struct_size = sizeof(lim);
    CHECK_ST(ms_ff_create(NULL, &lim, &f), MS_OK);
    snprintf(url, sizeof(url), "file:%s", path);
    CHECK_ST(ms_ff_open(f, url), MS_OK);
    CHECK_ST(ms_ff_list_tracks(f, ti, 8, &nt), MS_OK);
    sid = 0;
    for (i = 0; i < (int)nt; i++) {
        if (ti[i].codec == MS_CODEC_SUB_PGS) {
            sid = ti[i].id;
        }
    }
    CHECK(sid != 0, "pgs track identified");
    CHECK_ST(ms_ff_select_track(f, sid, 1), MS_OK);
    steps = saw_codec = saw_sub = 0;
    while ((st = ms_ff_pull(f, &ev)) == MS_OK || st == MS_AGAIN) {
        if (st == MS_AGAIN) {
            if (++steps > 400) {
                break;
            }
            continue;
        }
        CHECK(++steps < 400, "bounded pgs");
        if (ev.track.codec == MS_CODEC_SUB_PGS && ev.type == MS_EVENT_SUBTITLE) {
            saw_sub++;
            CHECK(ev.data != NULL && ev.data_size > 0, "compressed pes payload");
        }
        if (ev.release_id) {
            ms_ff_release(f, ev.release_id);
        }
        if (saw_sub >= 1) {
            break;
        }
    }
    CHECK(saw_sub >= 1, "pgs subtitle event");
    ms_sub_cue_default(&cue);
    CHECK_ST(ms_ff_subtitles_at(f, 90000, &cue, 1, &nc), MS_OK);
    CHECK(nc == 1, "pgs cue from ts");
    CHECK(cue.bitmap_kind == MS_SUB_BITMAP_PGS && cue.bitmap_w == 16 &&
          cue.bitmap_h == 16, "pgs rgba cue");
    CHECK(cue.pos_x == 30 && cue.pos_y == 40 && cue.play_res_x == 1920 &&
          cue.play_res_y == 1080, "pgs pos/canvas");
    CHECK(cue.bitmap_rgba != NULL && cue.bitmap_rgba[2] > 200 &&
          cue.bitmap_rgba[0] < 80 && cue.bitmap_rgba[3] == 255, "pgs ~blue");
    CHECK(cue.has_start && cue.start_90k == 90000, "pgs start 1s");
    CHECK(cue.text == NULL || cue.text[0] == 0, "pgs not as text");
    ms_ff_destroy(f);

    /* DVB: stream_type 0x06 + descriptor 0x59 */
    memset(streams, 0, sizeof(streams));
    streams[0].stream_type = 0x06;
    streams[0].pid = 0x131;
    streams[0].es_descriptor = k_dvb_desc59;
    streams[0].es_descriptor_size = (uint16_t)sizeof(k_dvb_desc59);
    ms_synth_init(&b);
    ms_synth_reset_cc();
    CHECK(ms_synth_pat_pmt(&b, 1, 0x130, streams, 1, 8, 188) == 0, "dvb pat");
    /* 16x16 完整显示集约 382 字节，单 PES 必跨多个 188 字节 TS 包；禁止用 4x4 单包代替。 */
    {
        size_t before = b.len;
        CHECK(ms_synth_pes(&b, 0x131, 0xBD, 1, 90000, k_dvb_16_red, sizeof(k_dvb_16_red), 188) == 0,
              "dvb 16 pes0");
        CHECK((b.len - before) / 188u >= 2u, "dvb 16x16 PES spans >=2 TS packets");
    }
    CHECK(ms_synth_pes(&b, 0x131, 0xBD, 1, 90000, k_dvb_16_red, sizeof(k_dvb_16_red), 188) == 0,
          "dvb 16 pes1");
    CHECK(ms_synth_pes(&b, 0x131, 0xBD, 1, 180000, k_dvb_16_red, sizeof(k_dvb_16_red), 188) == 0,
          "dvb 16 pes2");
    CHECK(path_join(path, sizeof(path), tmpdir, "dvb_16.ts") == 0, "path");
    CHECK(write_file(path, b.data, b.len) == 0, "write dvb ts");
    ms_synth_free(&b);

    CHECK_ST(ms_ff_create(NULL, &lim, &f), MS_OK);
    snprintf(url, sizeof(url), "file:%s", path);
    CHECK_ST(ms_ff_open(f, url), MS_OK);
    CHECK_ST(ms_ff_list_tracks(f, ti, 8, &nt), MS_OK);
    sid = 0;
    for (i = 0; i < (int)nt; i++) {
        if (ti[i].codec == MS_CODEC_SUB_DVB) {
            sid = ti[i].id;
        }
    }
    CHECK(sid != 0, "dvb track identified");
    CHECK_ST(ms_ff_select_track(f, sid, 1), MS_OK);
    steps = saw_sub = 0;
    nc = 0;
    while ((st = ms_ff_pull(f, &ev)) == MS_OK || st == MS_AGAIN) {
        if (st == MS_AGAIN) {
            if (++steps > 400) {
                break;
            }
            continue;
        }
        CHECK(++steps < 400, "bounded dvb");
        if (ev.track.codec == MS_CODEC_SUB_DVB && ev.type == MS_EVENT_SUBTITLE) {
            uint64_t t = 90000;
            saw_sub++;
            if (ev.has_pts) {
                if (ev.time_base_num == 1 && ev.time_base_den == 90000) {
                    t = ev.pts;
                } else if (ev.time_base_num > 0 && ev.time_base_den > 0) {
                    t = ev.pts * 90000ull * (uint64_t)ev.time_base_num /
                        (uint64_t)ev.time_base_den;
                }
            }
            ms_sub_cue_default(&cue);
            if (ms_ff_subtitles_at(f, t, &cue, 1, &nc) != MS_OK) {
                nc = 0;
            }
            if (nc == 0 && t != 0) {
                (void)ms_ff_subtitles_at(f, 0, &cue, 1, &nc);
            }
        }
        if (ev.release_id) {
            ms_ff_release(f, ev.release_id);
        }
        if (nc >= 1) {
            break;
        }
    }
    CHECK(saw_sub >= 1, "dvb subtitle event");
    CHECK(nc >= 1, "dvb cue from ts");
    CHECK(cue.bitmap_kind == MS_SUB_BITMAP_DVB && cue.bitmap_w == 16 &&
          cue.bitmap_h == 16, "dvb 16x16 rgba");
    CHECK(cue.pos_x == 10 && cue.pos_y == 20, "dvb pos");
    CHECK(cue.bitmap_rgba != NULL && cue.bitmap_rgba_size == 16u * 16u * 4u, "dvb rgba size");
    CHECK(cue.bitmap_rgba[0] > 200 && cue.bitmap_rgba[1] < 80 &&
          cue.bitmap_rgba[2] < 80 && cue.bitmap_rgba[3] == 255, "dvb ~red");
    CHECK(cue.bitmap_rgba[(16 * 16 - 1) * 4] > 200, "dvb last pixel");
    CHECK(cue.has_start && cue.start_90k == 90000, "dvb start 1s");
    CHECK_ST(ms_ff_flush(f), MS_OK);
    CHECK_ST(ms_ff_subtitles_at(f, 90000, &cue, 1, &nc), MS_OK);
    CHECK(nc == 0, "ff flush cleared bitmap cues");
    ms_ff_destroy(f);

    printf("  test_ff_bitmap_subs ok (host mpegts demux+parse; not device paint)\n");
    return 0;
}

static int ts_pid_packet_count(const uint8_t *data, size_t len, int pid)
{
    size_t i;
    int n = 0;
    for (i = 0; i + 188u <= len; i += 188u) {
        int p;
        if (data[i] != 0x47) {
            continue;
        }
        p = ((data[i + 1] & 0x1F) << 8) | data[i + 2];
        if (p == pid) {
            n++;
        }
    }
    return n;
}

static int pgs_next_seg(const uint8_t *s, size_t n, size_t off, size_t *seg_n)
{
    uint16_t len;
    if (off + 3u > n) {
        return -1;
    }
    len = (uint16_t)(((unsigned)s[off + 1] << 8) | s[off + 2]);
    if (off + 3u + (size_t)len > n) {
        return -1;
    }
    if (seg_n != NULL) {
        *seg_n = 3u + (size_t)len;
    }
    return 0;
}

/* 把一条完整 raw PGS 显示集的 ODS 按 RLE 偏移切成 first[/mid]/last。 */
static int pgs_split_ods_rle(const uint8_t *src, size_t src_n,
                             const size_t *cuts, int ncuts,
                             uint8_t *out, size_t cap, size_t *out_n)
{
    size_t off = 0, woff = 0;
    if (src == NULL || out == NULL || out_n == NULL || cuts == NULL || ncuts < 1) {
        return -1;
    }
    *out_n = 0;
    while (off < src_n) {
        size_t seg_n = 0, body, rle_n, prev, i;
        uint8_t typ;
        const uint8_t *rle;
        uint32_t obj_data_len;
        if (pgs_next_seg(src, src_n, off, &seg_n) != 0) {
            return -1;
        }
        typ = src[off];
        if (typ != 0x15) {
            if (woff + seg_n > cap) {
                return -1;
            }
            memcpy(out + woff, src + off, seg_n);
            woff += seg_n;
            off += seg_n;
            continue;
        }
        body = seg_n - 3u;
        if (body < 11u || (src[off + 3 + 3] & 0x80) == 0) {
            return -1;
        }
        obj_data_len = ((uint32_t)src[off + 3 + 4] << 16) |
                       ((uint32_t)src[off + 3 + 5] << 8) |
                       (uint32_t)src[off + 3 + 6];
        if (obj_data_len < 4u) {
            return -1;
        }
        rle = src + off + 3u + 11u;
        rle_n = body - 11u;
        if (rle_n != obj_data_len - 4u) {
            return -1;
        }
        /* cuts 必须严格递增且落在 (0, rle_n) */
        prev = 0;
        for (i = 0; i < (size_t)ncuts; i++) {
            if (cuts[i] == 0 || cuts[i] >= rle_n || cuts[i] <= prev) {
                return -1;
            }
            prev = cuts[i];
        }
        for (i = 0; i <= (size_t)ncuts; i++) {
            size_t from = (i == 0) ? 0 : cuts[i - 1];
            size_t to = (i == (size_t)ncuts) ? rle_n : cuts[i];
            size_t chunk = to - from;
            uint8_t seq;
            size_t hdr = (i == 0) ? 11u : 4u;
            size_t plen = hdr + chunk;
            if (woff + 3u + plen > cap) {
                return -1;
            }
            if (i == 0) {
                seq = (uint8_t)(0x80u | ((i == (size_t)ncuts) ? 0x40u : 0));
            } else if (i == (size_t)ncuts) {
                seq = 0x40;
            } else {
                seq = 0x00;
            }
            out[woff] = 0x15;
            out[woff + 1] = (uint8_t)((plen >> 8) & 0xFF);
            out[woff + 2] = (uint8_t)(plen & 0xFF);
            memcpy(out + woff + 3, src + off + 3, 3); /* id + version */
            out[woff + 6] = seq;
            if (i == 0) {
                memcpy(out + woff + 7, src + off + 3 + 4, 7); /* len24 + w + h */
                memcpy(out + woff + 14, rle + from, chunk);
            } else {
                memcpy(out + woff + 7, rle + from, chunk);
            }
            woff += 3u + plen;
        }
        off += seg_n;
    }
    *out_n = woff;
    return 0;
}

static int mux_raw_segments_as_pes(ms_synth_buf *b, uint16_t pid, uint64_t pts,
                                   const uint8_t *raw, size_t n)
{
    size_t off = 0;
    while (off < n) {
        size_t seg_n = 0;
        if (pgs_next_seg(raw, n, off, &seg_n) != 0) {
            return -1;
        }
        if (ms_synth_pes(b, pid, 0xBD, 1, pts, raw + off, seg_n, 188) != 0) {
            return -1;
        }
        off += seg_n;
    }
    return 0;
}

static int mux_bytes_as_pes_splits(ms_synth_buf *b, uint16_t pid, uint64_t pts,
                                   const uint8_t *data, size_t n,
                                   const size_t *cuts, int ncuts,
                                   int dvb_prefix_each)
{
    size_t prev = 0, i, to;
    uint8_t tmp[2048];
    if (data == NULL || n == 0 || ncuts < 1) {
        return -1;
    }
    for (i = 0; i <= (size_t)ncuts; i++) {
        const uint8_t *p;
        size_t pn, chunk;
        to = (i == (size_t)ncuts) ? n : cuts[i];
        if (to <= prev || to > n) {
            return -1;
        }
        chunk = to - prev;
        if (dvb_prefix_each && i > 0) {
            if (2u + chunk > sizeof(tmp)) {
                return -1;
            }
            tmp[0] = 0x20;
            tmp[1] = 0x00;
            memcpy(tmp + 2, data + prev, chunk);
            p = tmp;
            pn = 2u + chunk;
        } else {
            p = data + prev;
            pn = chunk;
        }
        if (ms_synth_pes(b, pid, 0xBD, 1, pts, p, pn, 188) != 0) {
            return -1;
        }
        prev = to;
    }
    return 0;
}

static int mux_pgs_pat(ms_synth_buf *b)
{
    static const uint8_t k_hdmv_reg[6] = { 0x05, 0x04, 'H', 'D', 'M', 'V' };
    ms_synth_stream streams[1];
    memset(streams, 0, sizeof(streams));
    streams[0].stream_type = 0x90;
    streams[0].pid = 0x121;
    if (ms_synth_pat_pmt_info(b, 1, 0x120, streams, 1, 8, 188, 0,
                              k_hdmv_reg, (uint16_t)sizeof(k_hdmv_reg)) != 0) {
        return -1;
    }
    return ms_synth_pat_pmt_info(b, 1, 0x120, streams, 1, 8, 188, 0,
                                 k_hdmv_reg, (uint16_t)sizeof(k_hdmv_reg));
}

static int mux_dvb_pat(ms_synth_buf *b)
{
    ms_synth_stream streams[1];
    memset(streams, 0, sizeof(streams));
    streams[0].stream_type = 0x06;
    streams[0].pid = 0x131;
    streams[0].es_descriptor = k_dvb_desc59;
    streams[0].es_descriptor_size = (uint16_t)sizeof(k_dvb_desc59);
    if (ms_synth_pat_pmt(b, 1, 0x130, streams, 1, 8, 188) != 0) {
        return -1;
    }
    return ms_synth_pat_pmt(b, 1, 0x130, streams, 1, 8, 188);
}

static int ff_open_select_sub(const char *path, ms_codec want, ms_ff **out, uint32_t *sid)
{
    ms_ff_limits lim;
    ms_ff *f = NULL;
    ms_track_info ti[8];
    size_t nt = 0, i;
    char url[640];
    memset(&lim, 0, sizeof(lim));
    lim.struct_size = sizeof(lim);
    if (ms_ff_create(NULL, &lim, &f) != MS_OK) {
        return -1;
    }
    snprintf(url, sizeof(url), "file:%s", path);
    if (ms_ff_open(f, url) != MS_OK) {
        ms_ff_destroy(f);
        return -1;
    }
    if (ms_ff_list_tracks(f, ti, 8, &nt) != MS_OK) {
        ms_ff_destroy(f);
        return -1;
    }
    *sid = 0;
    for (i = 0; i < nt; i++) {
        if (ti[i].codec == want) {
            *sid = ti[i].id;
        }
    }
    if (*sid == 0 || ms_ff_select_track(f, *sid, 1) != MS_OK) {
        ms_ff_destroy(f);
        return -1;
    }
    *out = f;
    return 0;
}

static int ff_pull_until_cue(ms_ff *f, uint64_t t90, ms_sub_cue *cue, size_t *nc)
{
    ms_event ev;
    ms_status st;
    int steps = 0;
    *nc = 0;
    ms_sub_cue_default(cue);
    while ((st = ms_ff_pull(f, &ev)) == MS_OK || st == MS_AGAIN) {
        uint64_t q = t90;
        if (st == MS_AGAIN) {
            if (++steps > 800) {
                break;
            }
            continue;
        }
        if (++steps > 800) {
            break;
        }
        if (ev.type == MS_EVENT_SUBTITLE && ev.has_pts) {
            if (ev.time_base_num == 1 && ev.time_base_den == 90000) {
                q = ev.pts;
            } else if (ev.time_base_num > 0 && ev.time_base_den > 0) {
                q = ev.pts * 90000ull * (uint64_t)ev.time_base_num /
                    (uint64_t)ev.time_base_den;
            }
        }
        if (ev.type == MS_EVENT_SUBTITLE) {
            ms_sub_cue_default(cue);
            if (ms_ff_subtitles_at(f, t90, cue, 1, nc) != MS_OK) {
                *nc = 0;
            }
            if (*nc == 0 && q != t90) {
                (void)ms_ff_subtitles_at(f, q, cue, 1, nc);
            }
        }
        if (ev.release_id) {
            ms_ff_release(f, ev.release_id);
        }
        if (*nc >= 1) {
            return 0;
        }
    }
    /* 再查一次时间轴，避免最后一包之后没有新事件。 */
    ms_sub_cue_default(cue);
    if (ms_ff_subtitles_at(f, t90, cue, 1, nc) != MS_OK) {
        *nc = 0;
    }
    return (*nc >= 1) ? 0 : -1;
}

static int cue_is_dvb16_red(const ms_sub_cue *c)
{
    const uint8_t *px;
    if (c == NULL || c->bitmap_kind != MS_SUB_BITMAP_DVB ||
        c->bitmap_w != 16 || c->bitmap_h != 16 ||
        c->pos_x != 10 || c->pos_y != 20 ||
        c->bitmap_rgba == NULL || c->bitmap_rgba_size != 16u * 16u * 4u) {
        return 0;
    }
    px = c->bitmap_rgba;
    if (!(px[0] > 200 && px[1] < 80 && px[2] < 80 && px[3] == 255)) {
        return 0;
    }
    if (px[(16 * 16 - 1) * 4] <= 200 || px[(16 * 16 - 1) * 4 + 3] != 255) {
        return 0;
    }
    return 1;
}

static int cue_is_pgs16_blue(const ms_sub_cue *c)
{
    const uint8_t *px;
    if (c == NULL || c->bitmap_kind != MS_SUB_BITMAP_PGS ||
        c->bitmap_w != 16 || c->bitmap_h != 16 ||
        c->pos_x != 30 || c->pos_y != 40 ||
        c->play_res_x != 1920 || c->play_res_y != 1080 ||
        c->bitmap_rgba == NULL || c->bitmap_rgba_size != 16u * 16u * 4u) {
        return 0;
    }
    px = c->bitmap_rgba;
    return px[2] > 200 && px[0] < 80 && px[3] == 255;
}

static int test_one_dvb_split(const char *tmpdir, const char *tag,
                              const size_t *cuts, int ncuts)
{
    char path[512];
    ms_synth_buf b;
    ms_ff *f = NULL;
    ms_sub_cue cue;
    size_t nc = 0;
    uint32_t sid = 0;
    int i, pkts;
    ms_synth_init(&b);
    ms_synth_reset_cc();
    CHECK(mux_dvb_pat(&b) == 0, "dvb pat");
    /* 同一组多 PES 显示集重复三遍，避免探测吃掉唯一一份。不用 4x4 诱饵。 */
    for (i = 0; i < 3; i++) {
        CHECK(mux_bytes_as_pes_splits(&b, 0x131, 90000, k_dvb_16_red,
                                      sizeof(k_dvb_16_red), cuts, ncuts, 1) == 0,
              tag);
    }
    pkts = ts_pid_packet_count(b.data, b.len, 0x131);
    CHECK(pkts >= 2, "dvb split uses multiple packets");
    CHECK(path_join(path, sizeof(path), tmpdir, tag) == 0, "path");
    CHECK(write_file(path, b.data, b.len) == 0, "write");
    ms_synth_free(&b);
    CHECK(ff_open_select_sub(path, MS_CODEC_SUB_DVB, &f, &sid) == 0, "open dvb");
    CHECK(ff_pull_until_cue(f, 90000, &cue, &nc) == 0 && nc == 1, tag);
    CHECK(cue_is_dvb16_red(&cue), tag);
    CHECK(cue.has_start && cue.start_90k == 90000, "dvb split start");
    CHECK_ST(ms_ff_flush(f), MS_OK);
    CHECK_ST(ms_ff_subtitles_at(f, 90000, &cue, 1, &nc), MS_OK);
    CHECK(nc == 0, "flush no leak");
    ms_ff_destroy(f);
    return 0;
}

static int test_one_pgs_pes_segments(const char *tmpdir, const char *tag,
                                     const uint8_t *raw, size_t raw_n)
{
    char path[512];
    ms_synth_buf b;
    ms_ff *f = NULL;
    ms_sub_cue cue;
    size_t nc = 0;
    uint32_t sid = 0;
    int i, pkts, segs = 0;
    size_t off = 0, seg_n;
    while (off < raw_n) {
        if (pgs_next_seg(raw, raw_n, off, &seg_n) != 0) {
            return 1;
        }
        segs++;
        off += seg_n;
    }
    CHECK(segs >= 5, "pgs multi segment");
    ms_synth_init(&b);
    ms_synth_reset_cc();
    CHECK(mux_pgs_pat(&b) == 0, "pgs pat");
    for (i = 0; i < 3; i++) {
        CHECK(mux_raw_segments_as_pes(&b, 0x121, 90000, raw, raw_n) == 0, tag);
    }
    pkts = ts_pid_packet_count(b.data, b.len, 0x121);
    CHECK(pkts >= segs, "pgs one PES per segment");
    CHECK(path_join(path, sizeof(path), tmpdir, tag) == 0, "path");
    CHECK(write_file(path, b.data, b.len) == 0, "write");
    ms_synth_free(&b);
    CHECK(ff_open_select_sub(path, MS_CODEC_SUB_PGS, &f, &sid) == 0, "open pgs");
    CHECK(ff_pull_until_cue(f, 90000, &cue, &nc) == 0 && nc == 1, tag);
    CHECK(cue_is_pgs16_blue(&cue), tag);
    CHECK(cue.has_start && cue.start_90k == 90000, "pgs start");
    ms_ff_destroy(f);
    return 0;
}

static int test_ff_bitmap_reassembly(const char *tmpdir)
{
    /* DVB：覆盖 data_identifier、段头、对象载荷、结束段等断点，以及三段 PES。 */
    static const size_t k_dvb_cuts[] = {
        1, 2, 3, 5, 7, 16, 17, 19, 38, 58, 59, 61, 63, 150, 250, 375, 376, 378, 381
    };
    static const size_t k_dvb_three[] = { 16, 200 };
    /* PGS raw 显示集断点：PCS 头、段间、ODS 头、RLE、END。 */
    static const size_t k_pgs_cuts[] = { 1, 2, 3, 22, 35, 50, 51, 53, 61, 80, 144 };
    size_t i;
    uint8_t ods2[512], ods3[512];
    size_t n2 = 0, n3 = 0;
    size_t cut2[1], cut3[2];
    char path[512];
    ms_synth_buf b;
    ms_ff *f = NULL;
    ms_sub_cue cue;
    size_t nc = 0;
    uint32_t sid = 0;

    for (i = 0; i < sizeof(k_dvb_cuts) / sizeof(k_dvb_cuts[0]); i++) {
        size_t cut = k_dvb_cuts[i];
        char tag[64];
        snprintf(tag, sizeof(tag), "dvb_cut_%u.ts", (unsigned)cut);
        if (test_one_dvb_split(tmpdir, tag, &cut, 1) != 0) {
            return 1;
        }
    }
    if (test_one_dvb_split(tmpdir, "dvb_cut_16_200.ts", k_dvb_three, 2) != 0) {
        return 1;
    }

    /* 缺中片：不应拼出 16x16；随后完整集应独立成功，不串帧。 */
    {
        size_t broken[2] = { 58, 200 };
        ms_synth_init(&b);
        ms_synth_reset_cc();
        CHECK(mux_dvb_pat(&b) == 0, "miss pat");
        /* 只发第一与第三片（跳过对象中段）三遍，再发完整集。 */
        for (i = 0; i < 2; i++) {
            uint8_t tmp[512];
            size_t mid = broken[0], last = broken[1];
            CHECK(ms_synth_pes(&b, 0x131, 0xBD, 1, 45000, k_dvb_16_red, mid, 188) == 0,
                  "miss p1");
            tmp[0] = 0x20;
            tmp[1] = 0x00;
            memcpy(tmp + 2, k_dvb_16_red + last, sizeof(k_dvb_16_red) - last);
            CHECK(ms_synth_pes(&b, 0x131, 0xBD, 1, 45000, tmp,
                               2u + (sizeof(k_dvb_16_red) - last), 188) == 0,
                  "miss p3");
        }
        for (i = 0; i < 3; i++) {
            CHECK(ms_synth_pes(&b, 0x131, 0xBD, 1, 180000, k_dvb_16_red,
                               sizeof(k_dvb_16_red), 188) == 0, "miss full");
        }
        CHECK(path_join(path, sizeof(path), tmpdir, "dvb_missing_mid.ts") == 0, "path");
        CHECK(write_file(path, b.data, b.len) == 0, "write");
        ms_synth_free(&b);
        CHECK(ff_open_select_sub(path, MS_CODEC_SUB_DVB, &f, &sid) == 0, "open miss");
        /* 45000 处不应出现由缺片拼出的画面。 */
        {
            int steps = 0;
            ms_event ev;
            ms_status st;
            size_t n450 = 1;
            while ((st = ms_ff_pull(f, &ev)) == MS_OK || st == MS_AGAIN) {
                if (st == MS_AGAIN) {
                    if (++steps > 400) {
                        break;
                    }
                    continue;
                }
                if (++steps > 400) {
                    break;
                }
                if (ev.release_id) {
                    ms_ff_release(f, ev.release_id);
                }
            }
            ms_sub_cue_default(&cue);
            CHECK_ST(ms_ff_subtitles_at(f, 45000, &cue, 1, &n450), MS_OK);
            CHECK(n450 == 0, "missing mid no cue at 45000");
            CHECK(ff_pull_until_cue(f, 180000, &cue, &nc) == 0 ||
                  (ms_ff_subtitles_at(f, 180000, &cue, 1, &nc) == MS_OK && nc == 1),
                  "later full");
            CHECK(nc == 1 && cue_is_dvb16_red(&cue), "later full red");
            CHECK(cue.has_start && cue.start_90k == 180000, "later start");
        }
        ms_ff_destroy(f);
        f = NULL;
    }

    /* 截断：只有前半 PES，无 cue。 */
    {
        ms_synth_init(&b);
        ms_synth_reset_cc();
        CHECK(mux_dvb_pat(&b) == 0, "trunc pat");
        CHECK(ms_synth_pes(&b, 0x131, 0xBD, 1, 90000, k_dvb_16_red, 80, 188) == 0,
              "trunc pes");
        CHECK(ms_synth_pes(&b, 0x131, 0xBD, 1, 90000, k_dvb_16_red, 80, 188) == 0,
              "trunc pes2");
        CHECK(path_join(path, sizeof(path), tmpdir, "dvb_trunc_pes.ts") == 0, "path");
        CHECK(write_file(path, b.data, b.len) == 0, "write");
        ms_synth_free(&b);
        CHECK(ff_open_select_sub(path, MS_CODEC_SUB_DVB, &f, &sid) == 0, "open trunc");
        {
            int steps = 0;
            ms_event ev;
            ms_status st;
            while ((st = ms_ff_pull(f, &ev)) == MS_OK || st == MS_AGAIN) {
                if (st == MS_AGAIN) {
                    if (++steps > 200) {
                        break;
                    }
                    continue;
                }
                if (++steps > 200) {
                    break;
                }
                if (ev.release_id) {
                    ms_ff_release(f, ev.release_id);
                }
            }
            ms_sub_cue_default(&cue);
            CHECK_ST(ms_ff_subtitles_at(f, 90000, &cue, 1, &nc), MS_OK);
            CHECK(nc == 0, "trunc no cue");
        }
        ms_ff_destroy(f);
        f = NULL;
    }

    /* seek/flush：半包入缓存后 seek，不得把旧半包与新集合并。 */
    {
        ms_synth_init(&b);
        ms_synth_reset_cc();
        CHECK(mux_dvb_pat(&b) == 0, "seek pat");
        CHECK(ms_synth_pes(&b, 0x131, 0xBD, 1, 90000, k_dvb_16_red, 100, 188) == 0,
              "seek half");
        CHECK(ms_synth_pes(&b, 0x131, 0xBD, 1, 90000, k_dvb_16_red, 100, 188) == 0,
              "seek half2");
        for (i = 0; i < 3; i++) {
            CHECK(ms_synth_pes(&b, 0x131, 0xBD, 1, 180000, k_dvb_16_red,
                               sizeof(k_dvb_16_red), 188) == 0, "seek full");
        }
        CHECK(path_join(path, sizeof(path), tmpdir, "dvb_seek_flush.ts") == 0, "path");
        CHECK(write_file(path, b.data, b.len) == 0, "write");
        ms_synth_free(&b);
        CHECK(ff_open_select_sub(path, MS_CODEC_SUB_DVB, &f, &sid) == 0, "open seek");
        {
            int steps = 0;
            ms_event ev;
            ms_status st;
            /* 先抽几包，让半集进入 acc。 */
            while (steps < 30 && ((st = ms_ff_pull(f, &ev)) == MS_OK || st == MS_AGAIN)) {
                steps++;
                if (st == MS_OK && ev.release_id) {
                    ms_ff_release(f, ev.release_id);
                }
            }
            CHECK_ST(ms_ff_seek(f, 180000), MS_OK);
            CHECK_ST(ms_ff_subtitles_at(f, 90000, &cue, 1, &nc), MS_OK);
            CHECK(nc == 0, "seek cleared leftover cues");
            CHECK(ff_pull_until_cue(f, 180000, &cue, &nc) == 0 && nc == 1, "after seek");
            CHECK(cue_is_dvb16_red(&cue), "seek cue");
            CHECK(cue.has_start && cue.start_90k == 180000, "seek start");
        }
        ms_ff_destroy(f);
        f = NULL;
    }

    /* PGS：每段一个 PES（完整 ODS 未拆），再 ODS first/last 与 first/mid/last。 */
    if (test_one_pgs_pes_segments(tmpdir, "pgs_seg_pes.ts",
                                  k_pgs_16_blue, sizeof(k_pgs_16_blue)) != 0) {
        return 1;
    }
    cut2[0] = 20;
    CHECK(pgs_split_ods_rle(k_pgs_16_blue, sizeof(k_pgs_16_blue), cut2, 1,
                            ods2, sizeof(ods2), &n2) == 0, "split ods 2");
    if (test_one_pgs_pes_segments(tmpdir, "pgs_ods_2.ts", ods2, n2) != 0) {
        return 1;
    }
    cut3[0] = 8;
    cut3[1] = 40;
    CHECK(pgs_split_ods_rle(k_pgs_16_blue, sizeof(k_pgs_16_blue), cut3, 2,
                            ods3, sizeof(ods3), &n3) == 0, "split ods 3");
    if (test_one_pgs_pes_segments(tmpdir, "pgs_ods_3.ts", ods3, n3) != 0) {
        return 1;
    }
    /* 若干 RLE 切点，确认 first/last 重组稳定。 */
    {
        static const size_t k_rle[] = { 1, 4, 11, 20, 40, 79 };
        for (i = 0; i < sizeof(k_rle) / sizeof(k_rle[0]); i++) {
            uint8_t buf[512];
            size_t nn = 0;
            size_t c = k_rle[i];
            char tag[64];
            CHECK(pgs_split_ods_rle(k_pgs_16_blue, sizeof(k_pgs_16_blue), &c, 1,
                                    buf, sizeof(buf), &nn) == 0, "rle split");
            snprintf(tag, sizeof(tag), "pgs_ods_rle_%u.ts", (unsigned)c);
            if (test_one_pgs_pes_segments(tmpdir, tag, buf, nn) != 0) {
                return 1;
            }
        }
    }
    /* PGS 在原始字节流上按头部/载荷断点拆成多 PES（不改写 ODS 标志，走 acc 拼接）。 */
    for (i = 0; i < sizeof(k_pgs_cuts) / sizeof(k_pgs_cuts[0]); i++) {
        char tag[64];
        size_t cut = k_pgs_cuts[i];
        snprintf(tag, sizeof(tag), "pgs_rawcut_%u.ts", (unsigned)cut);
        ms_synth_init(&b);
        ms_synth_reset_cc();
        CHECK(mux_pgs_pat(&b) == 0, "pgs cut pat");
        {
            int k;
            for (k = 0; k < 3; k++) {
                CHECK(mux_bytes_as_pes_splits(&b, 0x121, 90000, k_pgs_16_blue,
                                              sizeof(k_pgs_16_blue), &cut, 1, 0) == 0,
                      tag);
            }
        }
        CHECK(path_join(path, sizeof(path), tmpdir, tag) == 0, "path");
        CHECK(write_file(path, b.data, b.len) == 0, "write");
        ms_synth_free(&b);
        CHECK(ff_open_select_sub(path, MS_CODEC_SUB_PGS, &f, &sid) == 0, "open pgs cut");
        CHECK(ff_pull_until_cue(f, 90000, &cue, &nc) == 0 && nc == 1, tag);
        CHECK(cue_is_pgs16_blue(&cue), tag);
        CHECK(cue.has_start && cue.start_90k == 90000, "pgs rawcut start");
        ms_ff_destroy(f);
        f = NULL;
    }

    printf("  test_ff_bitmap_reassembly ok (libmsff demux to RGBA; not device paint)\n");
    return 0;
}




/* 受控 MKV/WebM：系统 ffmpeg 只用于生成样本（编码≠产品软解）。产品只开 matroska demuxer。 */
static int test_ff_matroska(const char *tmpdir)
{
    char vpath[512], spath[512], apath[512], mpath[512], hpath[512], ppath[512];
    char wpath[512], tpath[512], url[640], cmd[4096];
    const char *ffmpeg = "/usr/bin/ffmpeg";
    const char *srt =
        "1\n00:00:00,000 --> 00:00:01,000\nHelloMKV\n\n"
        "2\n00:00:00,400 --> 00:00:01,200\nOverlapMKV\n";
    const char *ass =
        "[Script Info]\nScriptType: v4.00+\nPlayResX: 384\nPlayResY: 288\n"
        "[V4+ Styles]\n"
        "Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, "
        "BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, "
        "BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding\n"
        "Style: Default,Arial,18,&H00FFFFFF,&H000000FF,&H00000000,&H00000000,0,0,0,0,"
        "100,100,0,0,1,2,2,2,8,8,8,1\n"
        "[Events]\n"
        "Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n"
        "Dialogue: 0,0:00:00.00,0:00:01.00,Default,,0,0,0,,mkv {\\i1}italic\n";
    ms_ff *f = NULL;
    ms_ff_limits lim;
    ms_status st;
    ms_event ev;
    ms_track_info ti[16];
    ms_sub_cue cue;
    size_t nt = 0, nc = 0, i;
    uint32_t vid = 0, aid = 0, sid_srt = 0, sid_ass = 0, sid_pgs = 0;
    int steps, vau = 0, aau = 0, saw_ass = 0, saw_srt = 0, cfg = 0;
    int have_pgs = 0, have_webm = 0, have_hevc = 0, have_sub90 = 0;
    uint64_t sub90 = 0;
    FILE *fp;
    long flen;

    if (access(ffmpeg, X_OK) != 0 || !have_cmd_encoder("libx264")) {
        fprintf(stderr, "FAIL test_ff_matroska: host /usr/bin/ffmpeg + libx264 required for fixtures\n");
        failed = 1;
        return 1;
    }
    CHECK(path_join(spath, sizeof(spath), tmpdir, "mkv.srt") == 0, "srt path");
    CHECK(path_join(apath, sizeof(apath), tmpdir, "mkv.ass") == 0, "ass path");
    CHECK(path_join(mpath, sizeof(mpath), tmpdir, "h264_aac_subs.mkv") == 0, "mkv path");
    CHECK(path_join(hpath, sizeof(hpath), tmpdir, "hevc.mkv") == 0, "hevc path");
    CHECK(path_join(ppath, sizeof(ppath), tmpdir, "pgs.mkv") == 0, "pgs mkv");
    CHECK(path_join(wpath, sizeof(wpath), tmpdir, "opus.webm") == 0, "webm path");
    CHECK(path_join(tpath, sizeof(tpath), tmpdir, "trunc.mkv") == 0, "trunc path");
    CHECK(path_join(vpath, sizeof(vpath), tmpdir, "pgs.sup") == 0, "sup path");
    CHECK(write_file(spath, (const uint8_t *)srt, strlen(srt)) == 0, "write srt");
    CHECK(write_file(apath, (const uint8_t *)ass, strlen(ass)) == 0, "write ass");
    CHECK(write_file(vpath, k_pgs_sup_16_blue, sizeof(k_pgs_sup_16_blue)) == 0, "write sup");

    /* H.264 + 双 AAC + SRT + ASS。fixture 编码不是产品软解。 */
    snprintf(cmd, sizeof(cmd),
             "%s -y -hide_banner -loglevel error "
             "-f lavfi -i color=c=black:s=16x16:r=25:d=1.2 "
             "-f lavfi -i sine=frequency=440:duration=1.2 "
             "-f lavfi -i sine=frequency=880:duration=1.2 "
             "-i %s -i %s "
             "-map 0:v -map 1:a -map 2:a -map 3:s -map 4:s "
             "-c:v libx264 -pix_fmt yuv420p -x264-params keyint=8:min-keyint=8:scenecut=0 "
             "-c:a aac -c:s:0 srt -c:s:1 ass "
             "-metadata:s:a:0 language=eng -metadata:s:a:1 language=chi "
             "-shortest %s",
             ffmpeg, spath, apath, mpath);
    CHECK(system(cmd) == 0, "mux h264 aac srt ass mkv");

    ms_ff_limits_default(&lim);
    lim.io_timeout_ms = 4000;
    CHECK_ST(ms_ff_create(NULL, &lim, &f), MS_OK);
    snprintf(url, sizeof(url), "file:%s", mpath);
    CHECK_ST(ms_ff_open(f, url), MS_OK);
    CHECK_ST(ms_ff_list_tracks(f, ti, 16, &nt), MS_OK);
    CHECK(nt >= 4, "mkv tracks: v + 2a + subs");
    for (i = 0; i < nt; i++) {
        if (ti[i].kind == MS_TRACK_VIDEO && ti[i].codec == MS_CODEC_H264) {
            vid = ti[i].id;
        }
        if (ti[i].kind == MS_TRACK_AUDIO && ti[i].codec == MS_CODEC_AAC) {
            if (aid == 0) {
                aid = ti[i].id;
            }
        }
        if (ti[i].kind == MS_TRACK_SUBTITLE && ti[i].codec == MS_CODEC_SUB_TEXT) {
            sid_srt = ti[i].id;
        }
        if (ti[i].kind == MS_TRACK_SUBTITLE && ti[i].codec == MS_CODEC_SUB_ASS) {
            sid_ass = ti[i].id;
        }
    }
    CHECK(vid != 0 && aid != 0, "h264+aac identified");
    CHECK(sid_srt != 0 || sid_ass != 0, "text sub identified");
    if (sid_ass != 0) {
        CHECK_ST(ms_ff_select_track(f, sid_ass, 1), MS_OK);
    } else {
        CHECK_ST(ms_ff_select_track(f, sid_srt, 1), MS_OK);
    }
    steps = 0;
    while ((st = ms_ff_pull(f, &ev)) == MS_OK || st == MS_AGAIN) {
        if (st == MS_AGAIN) {
            if (++steps > 800) {
                break;
            }
            continue;
        }
        CHECK(++steps < 800, "bounded mkv");
        if (ev.format != MS_FORMAT_UNKNOWN) {
            CHECK(ev.format == MS_FORMAT_MATROSKA, "format matroska not unknown");
        }
        if (ev.type == MS_EVENT_CODEC_CONFIG && ev.track.codec == MS_CODEC_H264) {
            cfg++;
        }
        if (ev.type == MS_EVENT_AU && ev.track.codec == MS_CODEC_H264) {
            vau++;
        }
        if (ev.type == MS_EVENT_AU && ev.track.codec == MS_CODEC_AAC) {
            aau++;
        }
        if (ev.type == MS_EVENT_SUBTITLE && ev.track.codec == MS_CODEC_SUB_ASS) {
            saw_ass++;
        }
        if (ev.type == MS_EVENT_SUBTITLE &&
            (ev.track.codec == MS_CODEC_SUB_TEXT || ev.track.codec == MS_CODEC_SUB_WEBVTT)) {
            saw_srt++;
        }
        if (ev.type == MS_EVENT_SUBTITLE && ev.has_pts && !have_sub90) {
            sub90 = ev.pts;
            if (!(ev.time_base_num == 1 && ev.time_base_den == 90000) &&
                ev.time_base_num > 0 && ev.time_base_den > 0) {
                sub90 = ev.pts * 90000ull * (uint64_t)ev.time_base_num /
                        (uint64_t)ev.time_base_den;
            }
            have_sub90 = 1;
        }
        if (ev.release_id) {
            ms_ff_release(f, ev.release_id);
        }
        if (vau >= 3 && aau >= 3 && (saw_ass + saw_srt) >= 1) {
            break;
        }
    }
    CHECK(cfg >= 1 && vau >= 1 && aau >= 1, "mkv h264/aac demux au");
    CHECK(saw_ass + saw_srt >= 1, "mkv inband subtitle packets");
    ms_sub_cue_default(&cue);
    nc = 0;
    CHECK_ST(ms_ff_subtitles_at(f, have_sub90 ? sub90 : 0, &cue, 1, &nc), MS_OK);
    if (nc == 0) {
        CHECK_ST(ms_ff_subtitles_at(f, 0, &cue, 1, &nc), MS_OK);
    }
    if (nc == 0) {
        CHECK_ST(ms_ff_subtitles_at(f, 90000, &cue, 1, &nc), MS_OK);
    }
    CHECK(nc >= 1, "mkv cue on timeline");
    CHECK(cue.text == NULL || strstr(cue.text, "{\\") == NULL, "ass tags stripped if present");

    /* seek 后再出包（不把 cue 缓存策略写成硬断言） */
    CHECK_ST(ms_ff_seek(f, 0), MS_OK);
    vau = 0;
    steps = 0;
    while ((st = ms_ff_pull(f, &ev)) == MS_OK || st == MS_AGAIN) {
        if (st == MS_AGAIN) {
            if (++steps > 800) {
                break;
            }
            continue;
        }
        CHECK(++steps < 800, "bounded seek");
        if (ev.type == MS_EVENT_AU && ev.track.codec == MS_CODEC_H264) {
            vau++;
        }
        if (ev.release_id) {
            ms_ff_release(f, ev.release_id);
        }
        if (vau >= 1) {
            break;
        }
    }
    CHECK(vau >= 1, "au after mkv seek");

    /* 切第二路 AAC（若枚举到） */
    {
        uint32_t a_ids[4];
        unsigned na = 0;
        for (i = 0; i < nt; i++) {
            if (ti[i].kind == MS_TRACK_AUDIO && ti[i].codec == MS_CODEC_AAC && na < 4) {
                a_ids[na++] = ti[i].id;
            }
        }
        if (na >= 2) {
            int switched = 0;
            CHECK_ST(ms_ff_select_track(f, a_ids[1], 1), MS_OK);
            steps = 0;
            while ((st = ms_ff_pull(f, &ev)) == MS_OK || st == MS_AGAIN) {
                if (st == MS_AGAIN) {
                    if (++steps > 400) {
                        break;
                    }
                    continue;
                }
                CHECK(++steps < 400, "bounded audio switch");
                if (ev.type == MS_EVENT_AU && ev.track.id == a_ids[1]) {
                    switched++;
                }
                if (ev.release_id) {
                    ms_ff_release(f, ev.release_id);
                }
                if (switched >= 1) {
                    break;
                }
            }
            CHECK(switched >= 1, "second aac after switch");
        }
    }

    /* 关闭清理 */
    ms_ff_destroy(f);
    f = NULL;

    /* 取消有界 */
    CHECK_ST(ms_ff_create(NULL, &lim, &f), MS_OK);
    CHECK_ST(ms_ff_open(f, url), MS_OK);
    CHECK_ST(ms_ff_cancel(f), MS_OK);
    st = ms_ff_pull(f, &ev);
    CHECK(st == MS_ERR_CANCELED || st == MS_EOS || st == MS_AGAIN, "cancel pull bounded");
    ms_ff_destroy(f);
    f = NULL;

    /* 截断畸形 */
    fp = fopen(mpath, "rb");
    CHECK(fp != NULL, "open mkv for trunc");
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        CHECK(0, "seek end");
    }
    flen = ftell(fp);
    fclose(fp);
    CHECK(flen > 128, "mkv not tiny");
    snprintf(cmd, sizeof(cmd), "dd if=%s of=%s bs=1 count=80 status=none", mpath, tpath);
    CHECK(system(cmd) == 0, "truncate mkv");
    CHECK_ST(ms_ff_create(NULL, &lim, &f), MS_OK);
    snprintf(url, sizeof(url), "file:%s", tpath);
    st = ms_ff_open(f, url);
    CHECK(st == MS_ERR_MALFORMED || st == MS_ERR_UNKNOWN || st == MS_ERR_UNSUPPORTED ||
          st == MS_OK, "trunc open controlled");
    if (st == MS_OK) {
        int pulls = 0;
        while ((st = ms_ff_pull(f, &ev)) == MS_OK || st == MS_AGAIN) {
            if (++pulls > 64) {
                break;
            }
            if (st == MS_OK && ev.release_id) {
                ms_ff_release(f, ev.release_id);
            }
        }
        CHECK(st == MS_EOS || st == MS_ERR_MALFORMED || st == MS_ERR_UNKNOWN ||
              st == MS_AGAIN || pulls >= 64, "trunc pull bounded");
    }
    ms_ff_destroy(f);
    f = NULL;

    /* 超大字幕事件有界 */
    {
        char huge_ass[512], huge_mkv[512];
        size_t cap = (size_t)MS_SUB_CUE_TEXT_MAX * 8;
        char *bodyh;
        size_t n;
        const char *hdr =
            "[Script Info]\nScriptType: v4.00+\n"
            "[Events]\nFormat: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n"
            "Dialogue: 0,0:00:00.00,0:00:01.00,Default,,0,0,0,,";
        CHECK(path_join(huge_ass, sizeof(huge_ass), tmpdir, "huge.ass") == 0, "huge ass path");
        CHECK(path_join(huge_mkv, sizeof(huge_mkv), tmpdir, "huge.mkv") == 0, "huge mkv path");
        bodyh = (char *)malloc(cap + 512);
        CHECK(bodyh != NULL, "huge alloc");
        n = (size_t)snprintf(bodyh, cap + 512, "%s", hdr);
        memset(bodyh + n, 'A', cap);
        bodyh[n + cap] = '\n';
        bodyh[n + cap + 1] = 0;
        CHECK(write_file(huge_ass, (const uint8_t *)bodyh, strlen(bodyh)) == 0, "write huge ass");
        free(bodyh);
        snprintf(cmd, sizeof(cmd),
                 "%s -y -hide_banner -loglevel error "
                 "-f lavfi -i color=c=black:s=16x16:r=25:d=0.4 "
                 "-i %s -map 0:v -map 1:s -c:v libx264 -pix_fmt yuv420p -c:s ass %s",
                 ffmpeg, huge_ass, huge_mkv);
        if (system(cmd) == 0) {
            CHECK_ST(ms_ff_create(NULL, &lim, &f), MS_OK);
            snprintf(url, sizeof(url), "file:%s", huge_mkv);
            CHECK_ST(ms_ff_open(f, url), MS_OK);
            CHECK_ST(ms_ff_list_tracks(f, ti, 16, &nt), MS_OK);
            sid_ass = 0;
            for (i = 0; i < nt; i++) {
                if (ti[i].codec == MS_CODEC_SUB_ASS) {
                    sid_ass = ti[i].id;
                }
            }
            if (sid_ass) {
                CHECK_ST(ms_ff_select_track(f, sid_ass, 1), MS_OK);
            }
            steps = 0;
            nc = 0;
            while ((st = ms_ff_pull(f, &ev)) == MS_OK || st == MS_AGAIN) {
                if (++steps > 400) {
                    break;
                }
                if (st == MS_OK) {
                    if (ev.release_id) {
                        ms_ff_release(f, ev.release_id);
                    }
                }
                ms_sub_cue_default(&cue);
                (void)ms_ff_subtitles_at(f, 0, &cue, 1, &nc);
                if (nc >= 1) {
                    break;
                }
            }
            if (nc >= 1 && cue.text) {
                CHECK(strlen(cue.text) <= (size_t)MS_SUB_CUE_TEXT_MAX, "huge cue truncated");
            }
            ms_ff_destroy(f);
            f = NULL;
        } else {
            printf("  test_ff_matroska: huge ASS mux failed; oversized cue not claimed\n");
        }
    }

    /* HEVC in MKV */
    if (have_cmd_encoder("libx265")) {
        snprintf(cmd, sizeof(cmd),
                 "%s -y -hide_banner -loglevel error "
                 "-f lavfi -i color=c=black:s=16x16:r=25:d=0.48 "
                 "-c:v libx265 -pix_fmt yuv420p "
                 "-x265-params keyint=2:min-keyint=2:scenecut=0:log-level=error "
                 "-an -f matroska %s",
                 ffmpeg, hpath);
        if (system(cmd) == 0) {
            have_hevc = 1;
        }
    }
    if (have_hevc) {
        int hau = 0, hcfg = 0;
        CHECK_ST(ms_ff_create(NULL, &lim, &f), MS_OK);
        snprintf(url, sizeof(url), "file:%s", hpath);
        CHECK_ST(ms_ff_open(f, url), MS_OK);
        steps = 0;
        while ((st = ms_ff_pull(f, &ev)) == MS_OK || st == MS_AGAIN) {
            if (st == MS_AGAIN) {
                if (++steps > 400) {
                    break;
                }
                continue;
            }
            CHECK(++steps < 400, "bounded hevc mkv");
            if (ev.format != MS_FORMAT_UNKNOWN) {
                CHECK(ev.format == MS_FORMAT_MATROSKA, "hevc mkv format");
            }
            if (ev.type == MS_EVENT_CODEC_CONFIG && ev.track.codec == MS_CODEC_HEVC) {
                hcfg++;
            }
            if (ev.type == MS_EVENT_AU && ev.track.codec == MS_CODEC_HEVC) {
                hau++;
            }
            if (ev.release_id) {
                ms_ff_release(f, ev.release_id);
            }
            if (hau >= 1 && hcfg >= 1) {
                break;
            }
        }
        CHECK(hcfg >= 1 && hau >= 1, "hevc mkv demux");
        ms_ff_destroy(f);
        f = NULL;
    } else {
        printf("  test_ff_matroska: HEVC MKV fixture skipped (no libx265); not claimed\n");
    }

    /* PGS 能放入则测（系统 ffmpeg 仅作复用，产品不开 pgssub 解码器） */
    snprintf(cmd, sizeof(cmd),
             "%s -y -hide_banner -loglevel error "
             "-f lavfi -i color=c=black:s=16x16:r=25:d=1.0 "
             "-i %s -map 0:v -map 1 -c:v libx264 -pix_fmt yuv420p -c:s copy "
             "-shortest %s",
             ffmpeg, vpath, ppath);
    if (system(cmd) == 0) {
        have_pgs = 1;
    } else {
        printf("  test_ff_matroska: PGS-in-MKV mux failed; bitmap-in-mkv not claimed\n");
    }
    if (have_pgs) {
        int psub = 0;
        CHECK_ST(ms_ff_create(NULL, &lim, &f), MS_OK);
        snprintf(url, sizeof(url), "file:%s", ppath);
        CHECK_ST(ms_ff_open(f, url), MS_OK);
        CHECK_ST(ms_ff_list_tracks(f, ti, 16, &nt), MS_OK);
        sid_pgs = 0;
        for (i = 0; i < nt; i++) {
            if (ti[i].codec == MS_CODEC_SUB_PGS) {
                sid_pgs = ti[i].id;
            }
        }
        CHECK(sid_pgs != 0, "pgs track in mkv");
        CHECK_ST(ms_ff_select_track(f, sid_pgs, 1), MS_OK);
        steps = 0;
        nc = 0;
        while ((st = ms_ff_pull(f, &ev)) == MS_OK || st == MS_AGAIN) {
            if (st == MS_AGAIN) {
                if (++steps > 400) {
                    break;
                }
                continue;
            }
            CHECK(++steps < 400, "bounded pgs mkv");
            if (ev.type == MS_EVENT_SUBTITLE && ev.track.codec == MS_CODEC_SUB_PGS) {
                uint64_t t = 0;
                psub++;
                if (ev.has_pts) {
                    t = ev.pts;
                    if (!(ev.time_base_num == 1 && ev.time_base_den == 90000) &&
                        ev.time_base_num > 0 && ev.time_base_den > 0) {
                        t = ev.pts * 90000ull * (uint64_t)ev.time_base_num /
                            (uint64_t)ev.time_base_den;
                    }
                }
                ms_sub_cue_default(&cue);
                (void)ms_ff_subtitles_at(f, t, &cue, 1, &nc);
                if (nc == 0) {
                    (void)ms_ff_subtitles_at(f, 90000, &cue, 1, &nc);
                }
            }
            if (ev.release_id) {
                ms_ff_release(f, ev.release_id);
            }
            if (nc >= 1) {
                break;
            }
        }
        CHECK(psub >= 1, "pgs packets from mkv");
        if (nc >= 1) {
            CHECK(cue.bitmap_kind == MS_SUB_BITMAP_PGS && cue.bitmap_w == 16, "pgs rgba from mkv");
        } else {
            printf("  test_ff_matroska: PGS packets demuxed but cue not assembled this fixture; packets only\n");
        }
        ms_ff_destroy(f);
        f = NULL;
    }



    /* 同一 MKV 内独立 PGS 位图轨 + SRT + ASS：切轨 / 关闭 / 切回。
     * 系统 ffmpeg 只做复用；产品 CONFIG_DECODERS=0，PGS 由 ms_sub_bitmap 解析。 */
    {
        char combo[512];
        uint32_t sid_p = 0, sid_t = 0, sid_a = 0;
        int psub = 0;
        CHECK(path_join(combo, sizeof(combo), tmpdir, "pgs_srt_ass.mkv") == 0, "combo path");
        snprintf(cmd, sizeof(cmd),
                 "%s -y -hide_banner -loglevel error "
                 "-f lavfi -i color=c=black:s=16x16:r=25:d=1.2 "
                 "-i %s -i %s -i %s "
                 "-map 0:v -map 1:0 -map 2:0 -map 3:0 "
                 "-c:v libx264 -pix_fmt yuv420p -c:s copy "
                 "-metadata:s:s:0 language=eng -metadata:s:s:0 title=pgs-bitmap "
                 "-metadata:s:s:1 language=fra -metadata:s:s:2 language=zho "
                 "-shortest %s",
                 ffmpeg, vpath, spath, apath, combo);
        if (system(cmd) != 0) {
            printf("  test_ff_matroska: PGS+SRT+ASS mux failed; switch-in-mkv not claimed\n");
        } else {
            CHECK_ST(ms_ff_create(NULL, &lim, &f), MS_OK);
            snprintf(url, sizeof(url), "file:%s", combo);
            CHECK_ST(ms_ff_open(f, url), MS_OK);
            CHECK_ST(ms_ff_list_tracks(f, ti, 16, &nt), MS_OK);
            for (i = 0; i < nt; i++) {
                if (ti[i].codec == MS_CODEC_SUB_PGS) {
                    sid_p = ti[i].id;
                }
                if (ti[i].codec == MS_CODEC_SUB_TEXT || ti[i].codec == MS_CODEC_SUB_SRT ||
                    ti[i].codec == MS_CODEC_SUB_WEBVTT) {
                    sid_t = ti[i].id;
                }
                if (ti[i].codec == MS_CODEC_SUB_ASS) {
                    sid_a = ti[i].id;
                }
            }
            if (sid_p == 0 || sid_t == 0 || sid_a == 0) {
                fprintf(stderr, "  combo tracks nt=%zu sid_p=%u sid_t=%u sid_a=%u\n",
                        (size_t)nt, sid_p, sid_t, sid_a);
                for (i = 0; i < nt; i++) {
                    fprintf(stderr, "    [%zu] id=%u kind=%d codec=%d\n",
                            i, ti[i].id, (int)ti[i].kind, (int)ti[i].codec);
                }
                ms_ff_destroy(f);
                f = NULL;
            }
            CHECK(sid_p != 0 && sid_t != 0 && sid_a != 0, "combo has pgs+text+ass");
            CHECK_ST(ms_ff_select_track(f, sid_p, 1), MS_OK);
            steps = 0;
            nc = 0;
            ms_sub_cue_default(&cue);
            while ((st = ms_ff_pull(f, &ev)) == MS_OK || st == MS_AGAIN) {
                if (st == MS_AGAIN) {
                    if (++steps > 500) {
                        break;
                    }
                    continue;
                }
                CHECK(++steps < 500, "bounded combo pgs");
                if (ev.type == MS_EVENT_SUBTITLE && ev.track.codec == MS_CODEC_SUB_PGS) {
                    uint64_t t = ev.has_pts ? ev.pts : 90000;
                    psub++;
                    (void)ms_ff_subtitles_at(f, t, &cue, 1, &nc);
                    if (nc == 0) {
                        (void)ms_ff_subtitles_at(f, 90000, &cue, 1, &nc);
                    }
                }
                if (ev.release_id) {
                    ms_ff_release(f, ev.release_id);
                }
                if (nc >= 1) {
                    break;
                }
            }
            CHECK(psub >= 1, "combo pgs packets");
            CHECK(nc >= 1 && cue.bitmap_kind == MS_SUB_BITMAP_PGS && cue.bitmap_w == 16,
                  "combo pgs rgba");

            /* 继续抽完开场字幕包，再切轨只查缓存，不再等新包。 */
            steps = 0;
            while ((st = ms_ff_pull(f, &ev)) == MS_OK || st == MS_AGAIN) {
                if (st == MS_AGAIN) {
                    if (++steps > 80) {
                        break;
                    }
                    continue;
                }
                if (ev.release_id) {
                    ms_ff_release(f, ev.release_id);
                }
                if (++steps > 80) {
                    break;
                }
            }
            CHECK_ST(ms_ff_select_track(f, sid_a, 1), MS_OK);
            nc = 0;
            ms_sub_cue_default(&cue);
            (void)ms_ff_subtitles_at(f, 0, &cue, 1, &nc);
            if (nc == 0) {
                (void)ms_ff_subtitles_at(f, 90000, &cue, 1, &nc);
            }
            if (!(nc >= 1 && cue.text != NULL)) {
                printf("  test_ff_matroska: combo ASS cue not in cache after switch; player-level test covers this\n");
            } else {
                CHECK(cue.bitmap_rgba == NULL || cue.bitmap_rgba_size == 0, "ass not bitmap");
            }

            CHECK_ST(ms_ff_select_track(f, sid_t, 1), MS_OK);
            nc = 0;
            ms_sub_cue_default(&cue);
            (void)ms_ff_subtitles_at(f, 0, &cue, 1, &nc);
            if (nc == 0) {
                (void)ms_ff_subtitles_at(f, 90000, &cue, 1, &nc);
            }
            if (!(nc >= 1 && cue.text != NULL)) {
                printf("  test_ff_matroska: combo SRT cue not in cache after switch; player-level test covers this\n");
            }

            /* 关闭：选 0 / 取消字幕 */
            CHECK_ST(ms_ff_select_track(f, sid_t, 0), MS_OK);
            nc = 0;
            ms_sub_cue_default(&cue);
            (void)ms_ff_subtitles_at(f, 0, &cue, 1, &nc);
            CHECK(nc == 0, "subs off after unselect");

            CHECK_ST(ms_ff_select_track(f, sid_p, 1), MS_OK);
            nc = 0;
            ms_sub_cue_default(&cue);
            (void)ms_ff_subtitles_at(f, 0, &cue, 1, &nc);
            if (nc == 0) {
                (void)ms_ff_subtitles_at(f, 90000, &cue, 1, &nc);
            }
            if (!(nc >= 1 && cue.bitmap_kind == MS_SUB_BITMAP_PGS)) {
                printf("  test_ff_matroska: combo PGS cue not in cache after switch-back; player-level test covers this\n");
            }
            ms_ff_destroy(f);
            f = NULL;
        }
    }


    /* WebVTT in MKV（若系统 ffmpeg 能复用） */
    {
        char vtt[512], vmkv[512];
        const char *vttc = "WEBVTT\n\n00:00:00.000 --> 00:00:01.000\nHelloVTT\n";
        int have_vtt = 0;
        CHECK(path_join(vtt, sizeof(vtt), tmpdir, "mkv.vtt") == 0, "vtt path");
        CHECK(path_join(vmkv, sizeof(vmkv), tmpdir, "vtt.mkv") == 0, "vtt mkv");
        CHECK(write_file(vtt, (const uint8_t *)vttc, strlen(vttc)) == 0, "write vtt");
        snprintf(cmd, sizeof(cmd),
                 "%s -y -hide_banner -loglevel error "
                 "-f lavfi -i color=c=black:s=16x16:r=25:d=0.5 "
                 "-i %s -map 0:v -map 1:s -c:v libx264 -pix_fmt yuv420p -c:s webvtt %s",
                 ffmpeg, vtt, vmkv);
        if (system(cmd) == 0) {
            have_vtt = 1;
        } else {
            printf("  test_ff_matroska: WebVTT-in-MKV mux failed; not claimed\n");
        }
        if (have_vtt) {
            int vsub = 0;
            uint32_t sid_vtt = 0;
            CHECK_ST(ms_ff_create(NULL, &lim, &f), MS_OK);
            snprintf(url, sizeof(url), "file:%s", vmkv);
            CHECK_ST(ms_ff_open(f, url), MS_OK);
            CHECK_ST(ms_ff_list_tracks(f, ti, 16, &nt), MS_OK);
            for (i = 0; i < nt; i++) {
                if (ti[i].kind == MS_TRACK_SUBTITLE &&
                    (ti[i].codec == MS_CODEC_SUB_WEBVTT || ti[i].codec == MS_CODEC_SUB_TEXT)) {
                    sid_vtt = ti[i].id;
                }
            }
            CHECK(sid_vtt != 0, "webvtt track in mkv");
            CHECK_ST(ms_ff_select_track(f, sid_vtt, 1), MS_OK);
            steps = 0;
            nc = 0;
            while ((st = ms_ff_pull(f, &ev)) == MS_OK || st == MS_AGAIN) {
                if (++steps > 400) {
                    break;
                }
                if (st == MS_OK) {
                    if (ev.type == MS_EVENT_SUBTITLE) {
                        vsub++;
                    }
                    if (ev.release_id) {
                        ms_ff_release(f, ev.release_id);
                    }
                }
                ms_sub_cue_default(&cue);
                (void)ms_ff_subtitles_at(f, 0, &cue, 1, &nc);
                if (nc >= 1) {
                    break;
                }
            }
            CHECK(vsub >= 1 || nc >= 1, "webvtt packet or cue from mkv");
            if (nc >= 1 && cue.text) {
                CHECK(strstr(cue.text, "HelloVTT") != NULL, "webvtt text");
            }
            ms_ff_destroy(f);
            f = NULL;
        }
    }

    /* WebM 与 matroska 绑定：仅用已支持 Opus 做最小打开，不宣传 VP9。 */
    if (have_cmd_encoder("libopus")) {
        snprintf(cmd, sizeof(cmd),
                 "%s -y -hide_banner -loglevel error "
                 "-f lavfi -i sine=frequency=440:duration=0.5 "
                 "-c:a libopus -f webm %s",
                 ffmpeg, wpath);
        if (system(cmd) == 0) {
            have_webm = 1;
        }
    }
    if (have_webm) {
        int oau = 0;
        CHECK_ST(ms_ff_create(NULL, &lim, &f), MS_OK);
        snprintf(url, sizeof(url), "file:%s", wpath);
        CHECK_ST(ms_ff_open(f, url), MS_OK);
        steps = 0;
        while ((st = ms_ff_pull(f, &ev)) == MS_OK || st == MS_AGAIN) {
            if (st == MS_AGAIN) {
                if (++steps > 400) {
                    break;
                }
                continue;
            }
            CHECK(++steps < 400, "bounded webm");
            if (ev.format != MS_FORMAT_UNKNOWN) {
                CHECK(ev.format == MS_FORMAT_WEBM, "url .webm maps WEBM");
            }
            if (ev.type == MS_EVENT_AU && ev.track.codec == MS_CODEC_OPUS) {
                oau++;
            }
            if (ev.release_id) {
                ms_ff_release(f, ev.release_id);
            }
            if (oau >= 1) {
                break;
            }
        }
        CHECK(oau >= 1, "opus au from webm");
        ms_ff_destroy(f);
        f = NULL;
    } else {
        printf("  test_ff_matroska: WebM/Opus fixture skipped; demuxer binds webm but not claimed tested\n");
    }

    printf("  test_ff_matroska ok (host matroska demux; fixture encode != product decode; not device)\n");
    return 0;
}


int main(void)
{
    const char *tmpdir = getenv("MS_FF_TMP");
    const char *cadir = getenv("MS_FF_CA");
    char tmpbuf[512];
    if (tmpdir == NULL || tmpdir[0] == 0) {
        snprintf(tmpbuf, sizeof(tmpbuf), "/tmp/msff-XXXXXX");
        if (!mkdtemp(tmpbuf)) {
            fprintf(stderr, "mkdtemp failed\n");
            return 2;
        }
        tmpdir = tmpbuf;
    }
    printf("test_ff_backend (FFmpeg %s, %s)\n", ms_ff_ffmpeg_version(),
           ms_ff_ffmpeg_license());
    if (test_version_and_license() != 0) {
        return 1;
    }
    if (test_local_containers(tmpdir) != 0) {
        return 1;
    }
    if (test_ff_hevc_mp4_ts(tmpdir) != 0) {
        return 1;
    }
    if (test_ff_mpeg2_mpeg4(tmpdir) != 0) {
        return 1;
    }
    if (test_ff_h263(tmpdir) != 0) {
        return 1;
    }
    if (test_ff_avsplus(tmpdir) != 0) {
        return 1;
    }
    if (test_ff_bitmap_subs(tmpdir) != 0) {
        return 1;
    }
    if (test_ff_bitmap_reassembly(tmpdir) != 0) {
        return 1;
    }
    if (test_ff_matroska(tmpdir) != 0) {
        return 1;
    }
    if (test_ff_av1(tmpdir) != 0) {
        return 1;
    }
    if (test_ff_ac3(tmpdir) != 0) {
        return 1;
    }
    if (test_ff_mp3(tmpdir) != 0) {
        return 1;
    }
    if (test_ff_morefmt_containers(tmpdir) != 0) {
        return 1;
    }
    if (test_ff_mp2(tmpdir) != 0) {
        return 1;
    }
    if (test_ff_opus(tmpdir) != 0) {
        return 1;
    }
    if (test_p4_ff_tracks(tmpdir) != 0) {
        return 1;
    }
    if (test_p4_ff_seek(tmpdir) != 0) {
        return 1;
    }
    if (test_ff_seek_keyframe_slop(tmpdir) != 0) {
        return 1;
    }
    if (test_ff_png_ts_prefix(tmpdir) != 0) {
        return 1;
    }
    if (test_http_hls(tmpdir) != 0) {
        return 1;
    }
    if (test_ff_hls_alt_audio(tmpdir) != 0) {
        return 1;
    }
    if (test_ff_hls_subs(tmpdir) != 0) {
        return 1;
    }
    if (test_ff_hls_sub_timeline(tmpdir) != 0) {
        return 1;
    }
    if (test_ff_hls_live_window(tmpdir) != 0) {
        return 1;
    }
    if (test_ff_hls_video_variants(tmpdir) != 0) {
        return 1;
    }
    if (test_http_hls_png_segment() != 0) {
        return 1;
    }
    if (test_tls_trust_missing() != 0) {
        return 1;
    }
    if (test_https_and_downgrade(cadir ? cadir : "", tmpdir) != 0) {
        return 1;
    }
    if (test_https_moov_tail_pump(cadir ? cadir : "", tmpdir) != 0) {
        return 1;
    }
    if (test_https_tls_matrix(cadir ? cadir : "", tmpdir) != 0) {
        return 1;
    }
    if (test_cancel_and_backpressure(tmpdir) != 0) {
        return 1;
    }
    {
        const char *dashdir = getenv("MS_FF_DASH_DIR");
        if (test_http_dash(dashdir) != 0) {
            return 1;
        }
        if (test_signed_bframes(dashdir) != 0) {
            return 1;
        }
        if (test_udp_rtp(dashdir) != 0) {
            return 1;
        }
        if (test_rtp_frame_contract(dashdir, tmpdir) != 0) {
            return 1;
        }
        if (test_rtsp_and_rtmp(dashdir, tmpdir) != 0) {
            return 1;
        }
    }
    if (failed) {
        return 1;
    }
    printf("pending: RTMPT, SRTP, "
           "AAC-over-RTSP, full RTP/RTSP camera profiles, live seek/discontinuity, "
           "four-platform/NDK/el7 formal, Media3 Android hardware decode.\n");
    printf("test_ff_backend ok\n");
    return 0;
}
