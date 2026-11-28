/*
 * 公共字节头探测 + 图片伪装头剥离实现，见 ms_probe.h。纯函数、无分配、有界。
 * by: Long <mycjzb@gmail.com>
 */
#include "ms_probe.h"

#include <string.h>

static uint32_t be32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static uint32_t le32(const uint8_t *p) { return ((uint32_t)p[3] << 24) | ((uint32_t)p[2] << 16) | ((uint32_t)p[1] << 8) | p[0]; }

/* ---- 图片伪装头：返回图片总长；0=不是该图片；(size_t)-1=图片未完整（需要更多字节） ---- */
#define NEED ((size_t)-1)

static size_t png_len(const uint8_t *d, size_t n)
{
    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a };
    size_t off = 8;
    if (n < 8) return memcmp(d, sig, n) == 0 ? NEED : 0;
    if (memcmp(d, sig, 8) != 0) return 0;
    for (;;) {
        uint32_t len;
        if (off + 8 > n) return NEED;
        len = be32(d + off);
        if (len > MS_PROBE_MAX_IMAGE) return 0;
        if (!memcmp(d + off + 4, "IEND", 4)) {
            if (off + 12 + len > n) return NEED;
            return off + 12 + len; /* 长度 4 + 类型 4 + 数据 + CRC 4 */
        }
        off += 12 + (size_t)len;
        if (off > MS_PROBE_MAX_IMAGE) return 0;
    }
}

static size_t jpeg_len(const uint8_t *d, size_t n)
{
    size_t off = 2;
    if (n < 3) return (n >= 1 && d[0] == 0xFF && (n < 2 || d[1] == 0xD8)) ? NEED : 0;
    if (d[0] != 0xFF || d[1] != 0xD8 || d[2] != 0xFF) return 0;
    for (;;) {
        uint8_t m;
        if (off + 2 > n) return NEED;
        if (d[off] != 0xFF) return 0;
        m = d[off + 1];
        if (m == 0xFF) { off++; continue; }
        if (m == 0xD9) return off + 2;
        if (m == 0x01 || (m >= 0xD0 && m <= 0xD7)) { off += 2; continue; }
        if (off + 4 > n) return NEED;
        {
            size_t sl = ((size_t)d[off + 2] << 8) | d[off + 3];
            if (sl < 2) return 0;
            off += 2 + sl;
            if (off > MS_PROBE_MAX_IMAGE) return 0;
            if (m == 0xDA) {
                /* 熵编码数据：找下一个非 RST、非填充 0x00 的标记 */
                for (;;) {
                    if (off + 2 > n) return NEED;
                    if (d[off] == 0xFF && d[off + 1] != 0x00 && !(d[off + 1] >= 0xD0 && d[off + 1] <= 0xD7)) break;
                    off++;
                    if (off > MS_PROBE_MAX_IMAGE) return 0;
                }
            }
        }
    }
}

static size_t gif_skip_sub(const uint8_t *d, size_t n, size_t off)
{
    for (;;) {
        if (off >= n) return NEED;
        if (d[off] == 0) return off + 1;
        off += 1 + d[off];
        if (off > MS_PROBE_MAX_IMAGE) return 0;
    }
}

static size_t gif_len(const uint8_t *d, size_t n)
{
    size_t off;
    if (n < 6) return (memcmp(d, "GIF8", n < 4 ? n : 4) == 0) ? NEED : 0;
    if (memcmp(d, "GIF87a", 6) && memcmp(d, "GIF89a", 6)) return 0;
    if (n < 13) return NEED;
    off = 13;
    if (d[10] & 0x80) off += 3u * (1u << ((d[10] & 7) + 1));
    for (;;) {
        if (off >= n) return NEED;
        if (d[off] == 0x3B) return off + 1;
        if (d[off] == 0x21) {
            if (off + 2 > n) return NEED;
            off = gif_skip_sub(d, n, off + 2);
        } else if (d[off] == 0x2C) {
            if (off + 10 > n) return NEED;
            {
                uint8_t f = d[off + 9];
                off += 10;
                if (f & 0x80) off += 3u * (1u << ((f & 7) + 1));
            }
            off += 1; /* LZW 最小码长 */
            off = gif_skip_sub(d, n, off);
        } else {
            return 0;
        }
        if (off == NEED || off == 0) return off;
    }
}

static size_t bmp_len(const uint8_t *d, size_t n)
{
    uint32_t sz;
    if (n < 2) return (n == 1 && d[0] == 'B') ? NEED : 0;
    if (d[0] != 'B' || d[1] != 'M') return 0;
    if (n < 14) return NEED;
    sz = le32(d + 2);
    /* 保留字段须为 0，像素偏移在文件内，防误判 */
    if (sz < 26 || sz > MS_PROBE_MAX_IMAGE || le32(d + 6) != 0 || le32(d + 10) >= sz) return 0;
    return sz <= n ? sz : NEED;
}

static size_t webp_len(const uint8_t *d, size_t n)
{
    uint32_t sz;
    if (n < 12) return (memcmp(d, "RIFF", n < 4 ? n : 4) == 0) ? NEED : 0;
    if (memcmp(d, "RIFF", 4) || memcmp(d + 8, "WEBP", 4)) return 0;
    sz = le32(d + 4);
    if (sz > MS_PROBE_MAX_IMAGE) return 0;
    sz += 8 + (sz & 1);
    return sz <= n ? sz : NEED;
}

/* ---- 容器签名（强校验） ---- */
static int ts_sync(const uint8_t *d, size_t n, int eof, size_t stride, size_t first, int *need)
{
    int k, want = 5, got = 0;
    for (k = 0; k < want; k++) {
        size_t pos = first + (size_t)k * stride;
        if (pos >= n) {
            if (!eof) { *need = 1; return 0; }
            break;
        }
        if (d[pos] != 0x47) return 0;
        got++;
    }
    return got >= 2 || (eof && got >= 1 && n - first < stride + 1);
}

static int bmff_type_ok(const uint8_t *t)
{
    static const char *k[] = { "ftyp", "styp", "moov", "moof", "sidx", "mdat", "free", "skip", "emsg", "prft", "wide", "pdin" };
    size_t i;
    for (i = 0; i < sizeof(k) / sizeof(k[0]); i++)
        if (!memcmp(t, k[i], 4)) return 1;
    return 0;
}

/* 脏字节窗口内：大段可打印 ASCII 则不像 TS/M2TS（避免 JS 里 G/ORIGIN 误判） */
static int ascii_textish(const uint8_t *p, size_t n)
{
    size_t i, lim = n < 16 ? n : 16, asc = 0;
    if (lim == 0) {
        return 0;
    }
    for (i = 0; i < lim; i++) {
        if (p[i] >= 0x20 && p[i] < 0x7f) {
            asc++;
        }
    }
    return asc * 4 >= lim * 3;
}

static int adts_ok(const uint8_t *d, size_t n, int eof, int *need)
{
    size_t fl;
    if (n < 7) { if (!eof) *need = 1; return 0; }
    if (d[0] != 0xFF || (d[1] & 0xF6) != 0xF0) return 0;
    if (((d[2] >> 2) & 0xF) > 12) return 0;
    fl = ((size_t)(d[3] & 3) << 11) | ((size_t)d[4] << 3) | (d[5] >> 5);
    if (fl < 7) return 0;
    if (fl + 2 > n) { if (!eof) { *need = 1; return 0; } return 1; }
    return d[fl] == 0xFF && (d[fl + 1] & 0xF6) == 0xF0;
}

static int mp3_ok(const uint8_t *d, size_t n, int eof, int *need)
{
    static const int br1[16] = { 0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0 };
    static const int sr1[4] = { 44100, 48000, 32000, 0 };
    int bi, si;
    size_t fl;
    if (n < 4) { if (!eof) *need = 1; return 0; }
    /* 只认 MPEG-1 Layer III，避免与 ADTS/随机字节混淆 */
    if (d[0] != 0xFF || (d[1] & 0xFE) != 0xFA) return 0;
    bi = d[2] >> 4;
    si = (d[2] >> 2) & 3;
    if (!br1[bi] || !sr1[si]) return 0;
    fl = (size_t)(144000 * br1[bi] / sr1[si]) + ((d[2] >> 1) & 1);
    if (fl + 2 > n) { if (!eof) { *need = 1; return 0; } return 1; }
    return d[fl] == 0xFF && (d[fl + 1] & 0xFE) == 0xFA;
}

int ms_probe_format_at(const uint8_t *d, size_t n, int eof, int *need_more)
{
    int need = 0;
    if (need_more) *need_more = 0;
    if (n == 0) { if (need_more) *need_more = !eof; return MS_PF_UNKNOWN; }
    if (d[0] == 0x47) {
        if (ts_sync(d, n, eof, 188, 0, &need)) return MS_PF_TS;
        if (!need && ts_sync(d, n, eof, 204, 0, &need)) return MS_PF_TS204;
    }
    /* M2TS：4B 头 + 0x47。拒绝对「全可打印 ASCII」假头（JS 里 ORIGIN 的 G 曾误判） */
    if (n >= 5 && d[4] == 0x47) {
        int asc = 0, hi;
        for (hi = 0; hi < 4; hi++) {
            if (d[hi] >= 0x20 && d[hi] < 0x7f) {
                asc++;
            }
        }
        if (asc < 4 && ts_sync(d, n, eof, 192, 4, &need)) {
            return MS_PF_TS192;
        }
    }
    if (n >= 8) {
        uint32_t sz = be32(d);
        if (bmff_type_ok(d + 4) && (sz == 0 || sz == 1 || sz >= 8)) return MS_PF_BMFF;
    } else if (!eof) need = 1;
    if (n >= 9 && !memcmp(d, "FLV", 3) && d[3] == 1 && be32(d + 5) >= 9) return MS_PF_FLV;
    if (n >= 4 && d[0] == 0x1A && d[1] == 0x45 && d[2] == 0xDF && d[3] == 0xA3) return MS_PF_MKV;
    if (n >= 5 && d[0] == 0 && d[1] == 0 && d[2] == 1 && d[3] == 0xBA && ((d[4] & 0xC4) == 0x44 || (d[4] & 0xF0) == 0x20)) return MS_PF_PS;
    {
        const uint8_t *t = d;
        size_t tn = n;
        if (tn >= 3 && t[0] == 0xEF && t[1] == 0xBB && t[2] == 0xBF) { t += 3; tn -= 3; }
        if (tn >= 7 && !memcmp(t, "#EXTM3U", 7)) return MS_PF_HLS;
        if (tn >= 4 && !memcmp(t, "<MPD", 4)) return MS_PF_DASH;
        if (tn >= 5 && !memcmp(t, "<?xml", 5)) {
            size_t i, lim = tn < 1024 ? tn : 1024;
            for (i = 0; i + 4 <= lim; i++)
                if (!memcmp(t + i, "<MPD", 4) || (i + 5 <= lim && !memcmp(t + i, ":MPD", 4))) return MS_PF_DASH;
            if (!eof && tn < 1024) need = 1;
        }
    }
    if (n >= 10 && !memcmp(d, "ID3", 3) && d[3] < 5 && !(d[6] & 0x80)) return MS_PF_ID3;
    if (n >= 6 && d[0] == 0x0B && d[1] == 0x77) return MS_PF_AC3;
    if (n >= 3 && d[0] == 0x56 && (d[1] & 0xE0) == 0xE0) return MS_PF_LATM;
    if (adts_ok(d, n, eof, &need)) return MS_PF_ADTS;
    if (mp3_ok(d, n, eof, &need)) return MS_PF_MP3;
    if (need_more) *need_more = need && !eof;
    return MS_PF_UNKNOWN;
}

int ms_probe_payload(const uint8_t *d, size_t n, int eof, ms_probe_result *r)
{
    size_t off = 0, img = 0, lim, i;
    int dis = MS_PD_NONE, fmt, need;
    if (!d || !r) return -1;
    memset(r, 0, sizeof(*r));
    /* 1) 图片伪装头（可能多张叠加，最多 4 张） */
    for (i = 0; i < 4; i++) {
        size_t (*fn[5])(const uint8_t *, size_t) = { png_len, jpeg_len, gif_len, bmp_len, webp_len };
        static const int kinds[5] = { MS_PD_PNG, MS_PD_JPEG, MS_PD_GIF, MS_PD_BMP, MS_PD_WEBP };
        size_t k, L = 0;
        int hit = 0;
        for (k = 0; k < 5; k++) {
            L = fn[k](d + off, n - off);
            if (L == NEED) {
                if (!eof) return 0;
                L = 0; /* EOF 时图未完整：放弃剥离该图 */
                continue;
            }
            if (L) { hit = 1; if (dis == MS_PD_NONE) dis = kinds[k]; break; }
        }
        if (!hit) break;
        /* RIFF 也可能是 WAV/AVI：只有 WEBP 标记才算伪装，已在 webp_len 中校验 */
        off += L;
        img += L;
    }
    /* 2) 起点直接是容器 */
    fmt = ms_probe_format_at(d + off, n - off, eof, &need);
    if (fmt != MS_PF_UNKNOWN) goto done;
    if (need) return 0;
    /* 3) 有界跳过前导脏字节 */
    lim = n - off;
    if (lim > MS_PROBE_MAX_JUNK) lim = MS_PROBE_MAX_JUNK;
    for (i = 1; i < lim; i++) {
        const uint8_t *p = d + off + i;
        uint8_t c = p[0];
        int nm = 0;
        /* 只在可能的签名首字节上做强校验 */
        if (!(c == 0x47 || c == 'F' || c == 0x1A || c == 0 || c == '#' || c == '<' || c == 0xEF ||
              (i + 4 < lim && p[4] == 0x47) || (i + 8 <= n - off && bmff_type_ok(p + 4))))
            continue;
        if (c == 0 && !(i + 8 <= n - off && bmff_type_ok(p + 4)) && !(n - off - i >= 4 && p[1] == 0 && p[2] == 1 && p[3] == 0xBA))
            continue;
        fmt = ms_probe_format_at(p, n - off - i, eof, &nm);
        if (fmt == MS_PF_ADTS || fmt == MS_PF_MP3 || fmt == MS_PF_AC3 || fmt == MS_PF_LATM || fmt == MS_PF_ID3)
            fmt = MS_PF_UNKNOWN; /* 脏字节里不认弱签名裸音频 */
        if (fmt == MS_PF_HLS || fmt == MS_PF_DASH)
            fmt = MS_PF_UNKNOWN; /* JS 字符串常含 #EXTM3U / <MPD */
        if ((fmt == MS_PF_TS || fmt == MS_PF_TS192 || fmt == MS_PF_TS204 || fmt == MS_PF_BMFF
             || fmt == MS_PF_FLV)
            && ascii_textish(p, n - off - i)) {
            fmt = MS_PF_UNKNOWN; /* 可打印文本里的 G/skip/free/FLV 假签名 */
        }
        if (fmt != MS_PF_UNKNOWN) {
            r->junk_bytes = i;
            off += i;
            goto done;
        }
        if (nm) return 0;
    }
    if (!eof && n - off < MS_PROBE_MAX_JUNK) return 0;
    if (img) { fmt = MS_PF_UNKNOWN; goto done; } /* 剥了图但没认出容器：仍返回剥图后的起点 */
    return -1;
done:
    r->offset = off;
    r->image_bytes = img;
    r->fmt = fmt;
    r->disguise = dis;
    return 1;
}

int ms_probe_media_offset(const uint8_t *d, int n, int eof, int *need_more)
{
    ms_probe_result r;
    int rc;
    if (need_more) *need_more = 0;
    if (!d || n <= 0) return 0;
    rc = ms_probe_payload(d, (size_t)n, eof, &r);
    if (rc == 0) { if (need_more) *need_more = 1; return 0; }
    if (rc < 0) return 0;
    return (int)r.offset;
}

const char *ms_probe_fmt_name(int f)
{
    static const char *k[] = { "unknown", "ts", "ts192", "ts204", "bmff", "flv", "mkv", "ps", "hls", "dash",
                               "adts", "mp3", "id3", "ac3", "latm" };
    return (f >= 0 && f <= MS_PF_LATM) ? k[f] : "unknown";
}

const char *ms_probe_disguise_name(int dd)
{
    static const char *k[] = { "none", "png", "jpeg", "gif", "bmp", "webp" };
    return (dd >= 0 && dd <= MS_PD_WEBP) ? k[dd] : "none";
}
