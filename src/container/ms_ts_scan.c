/*
 * 有界 MPEG-TS 前缀识别：裸 TS、PNG/JPEG 头伪装、跨缓冲保留、假 0x47 抑制。
 * 不复制后续码流。
 * by: Long <mycjzb@gmail.com>
 */
#include "ms_ts_scan.h"
#include "ms_internal.h"

#include <string.h>

static const uint8_t k_png[8] = { 0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a };

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static int ascii4(const uint8_t *p, const char *s)
{
    return p[0] == (uint8_t)s[0] && p[1] == (uint8_t)s[1] &&
           p[2] == (uint8_t)s[2] && p[3] == (uint8_t)s[3];
}

/* PNG 块类型为 4 个 ASCII 字母。TS 的 0x47 长度字段后的类型几乎总是非法。 */
static int png_type_ok(const uint8_t *p)
{
    int i;
    for (i = 0; i < 4; i++) {
        unsigned char c = p[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'))) {
            return 0;
        }
    }
    return 1;
}

int ms_ts_pkt_plausible(const uint8_t *pkt, size_t avail)
{
    unsigned adapt;
    if (pkt == NULL || avail < 4 || pkt[0] != 0x47) {
        return 0;
    }
    adapt = (pkt[3] >> 4) & 0x3u;
    if (adapt == 0) {
        return 0;
    }
    if ((adapt == 2 || adapt == 3) && avail >= 5) {
        unsigned alen = pkt[4];
        if ((size_t)5u + alen > 188u) {
            return 0;
        }
    }
    return 1;
}

static int sync_run(const uint8_t *data, size_t size, size_t off, uint16_t psz,
                    int want, int require_plausible, int relax_first)
{
    int n = 0;
    size_t pos = off;
    while (n < want) {
        if (pos >= size || data[pos] != 0x47) {
            break;
        }
        /* 窗口起点的首包即使 adaptation 保留值等不可信，只要后续包对齐，
         * 仍视为从 0 开始的裸 TS，交给解析器报 MALFORMED；避免被当成前缀跳过。 */
        if (require_plausible && !(relax_first && n == 0) &&
            !ms_ts_pkt_plausible(data + pos, size - pos)) {
            break;
        }
        n++;
        pos += psz;
        if (n < want && pos >= size) {
            break;
        }
    }
    return n;
}

int ms_ts_find_sync(const uint8_t *data, size_t size,
                    size_t prefer_from, size_t *out_off, uint16_t *out_psz)
{
    uint16_t cand_psz = 0;
    size_t cand_off = 0;
    int cand_n = 0;
    uint16_t psz;

    if (data == NULL || out_off == NULL || out_psz == NULL) {
        return 0;
    }
    if (size < (size_t)MS_TS_SYNC_WANT * 188u) {
        return 0;
    }

    /* 完整图像之后才接受同步；图像内的 0x47 串一律忽略。
     * 图像已结束但后面还不够 3 包时由调用方 AGAIN，不得回退到图内假同步。 */
    if (prefer_from > 0) {
        size_t sub_off = 0;
        uint16_t sub_psz = 0;
        if (prefer_from >= size) {
            return 0;
        }
        if (ms_ts_find_sync(data + prefer_from, size - prefer_from, 0,
                            &sub_off, &sub_psz)) {
            *out_off = prefer_from + sub_off;
            *out_psz = sub_psz;
            return 1;
        }
        return 0;
    }

    for (psz = 188; ; psz = 192) {
        size_t need = (size_t)MS_TS_SYNC_WANT * (size_t)psz;
        size_t max_off, off;
        if (size < need) {
            if (psz == 192) {
                break;
            }
            continue;
        }
        max_off = size - need + 1u;
        for (off = 0; off < max_off; off++) {
            int n;
            if (data[off] != 0x47) {
                continue;
            }
            n = sync_run(data, size, off, psz, 16, 1, off == 0);
            if (n < MS_TS_SYNC_WANT) {
                continue;
            }
            if (n > cand_n || (n == cand_n && psz == 188 && cand_psz == 192)) {
                cand_n = n;
                cand_off = off;
                cand_psz = psz;
            }
        }
        if (psz == 192) {
            break;
        }
    }
    if (cand_n < MS_TS_SYNC_WANT) {
        return 0;
    }
    *out_off = cand_off;
    *out_psz = cand_psz;
    return 1;
}

size_t ms_ts_sync_keep_from(const uint8_t *data, size_t size)
{
    size_t keep = size;
    uint16_t psz;
    if (data == NULL) {
        return 0;
    }
    for (psz = 188; ; psz = 192) {
        size_t need = (size_t)MS_TS_SYNC_WANT * (size_t)psz;
        size_t off;
        size_t first_open = size;
        if (size < need) {
            first_open = 0;
        } else {
            first_open = size - need + 1u;
            for (off = 0; off < size; off++) {
                int n;
                if (data[off] != 0x47) {
                    continue;
                }
                n = sync_run(data, size, off, psz, MS_TS_SYNC_WANT, 1, off == 0);
                if (n >= MS_TS_SYNC_WANT) {
                    return 0; /* 已找到，调用方不应丢弃 */
                }
                if (off + need > size && off < first_open) {
                    first_open = off;
                }
            }
        }
        if (first_open < keep) {
            keep = first_open;
        }
        if (psz == 192) {
            break;
        }
    }
    return keep;
}

ms_ts_prefix_kind ms_ts_detect_prefix_kind(const uint8_t *data, size_t size)
{
    if (data == NULL || size == 0) {
        return MS_TS_PREFIX_NONE;
    }
    if (size >= 8 && memcmp(data, k_png, 8) == 0) {
        return MS_TS_PREFIX_PNG;
    }
    if (size >= 2 && data[0] == 0xff && data[1] == 0xd8) {
        return MS_TS_PREFIX_JPEG;
    }
    if (size >= 1 && data[0] == 0x47) {
        return MS_TS_PREFIX_NONE;
    }
    return MS_TS_PREFIX_OTHER;
}

/*
 * 返回「不再属于当前 PNG」的第一字节。
 * *complete=1 仅当 IEND 已完整出现。
 * 块头未齐或块体未齐：仍视为图内，返回 size（本窗口不再往后搜）。
 * 长度非法：结构在此断开（常见于仅 8 字节签名后直接跟 TS），从该处搜同步。
 */
static size_t png_search_from(const uint8_t *data, size_t size, int *complete)
{
    size_t off = 8;
    if (complete) {
        *complete = 0;
    }
    if (size < 8 || memcmp(data, k_png, 8) != 0) {
        return 0;
    }
    if (size == 8) {
        return size;
    }
    while (off < size) {
        uint32_t len;
        size_t next;
        if (off + 4u > size) {
            return size;
        }
        len = be32(data + off);
        if (len > 0x7fffffffu) {
            return off;
        }
        if (off + 8u > size) {
            return size;
        }
        if (!png_type_ok(data + off + 4)) {
            return off;
        }
        if (ms_size_add(off, 12u, &next) != 0 ||
            ms_size_add(next, (size_t)len, &next) != 0) {
            return off;
        }
        if (next > size) {
            return size;
        }
        if (ascii4(data + off + 4, "IEND")) {
            if (complete) {
                *complete = 1;
            }
            return next;
        }
        off = next;
    }
    return size;
}

static size_t png_end(const uint8_t *data, size_t size)
{
    int complete = 0;
    size_t n = png_search_from(data, size, &complete);
    return complete ? n : 0;
}

/* 同 png_search_from：完整 EOI 置 *complete；标记未齐视为图内；非法标记处断开。 */
static size_t jpeg_search_from(const uint8_t *data, size_t size, int *complete)
{
    size_t off;
    int in_ecs = 0;
    if (complete) {
        *complete = 0;
    }
    if (size < 2 || data[0] != 0xff || data[1] != 0xd8) {
        return 0;
    }
    off = 2;
    if (off >= size) {
        return size;
    }
    while (off < size) {
        if (!in_ecs) {
            uint8_t m;
            uint32_t seglen;
            if (data[off] != 0xff) {
                return off;
            }
            while (off < size && data[off] == 0xff) {
                off++;
            }
            if (off >= size) {
                return size;
            }
            m = data[off++];
            if (m == 0xd8) {
                return off - 1u;
            }
            if (m == 0xd9) {
                if (complete) {
                    *complete = 1;
                }
                return off;
            }
            if (m == 0x01 || (m >= 0xd0 && m <= 0xd7)) {
                continue;
            }
            if (off + 2 > size) {
                return size;
            }
            seglen = ((uint32_t)data[off] << 8) | (uint32_t)data[off + 1];
            if (seglen < 2) {
                return off;
            }
            if (off + seglen > size) {
                return size;
            }
            off += seglen;
            if (m == 0xda) {
                in_ecs = 1;
            }
            continue;
        }
        if (data[off] != 0xff) {
            off++;
            continue;
        }
        if (off + 1 >= size) {
            return size;
        }
        if (data[off + 1] == 0x00 ||
            (data[off + 1] >= 0xd0 && data[off + 1] <= 0xd7)) {
            off += 2;
            continue;
        }
        if (data[off + 1] == 0xd9) {
            if (complete) {
                *complete = 1;
            }
            return off + 2;
        }
        in_ecs = 0;
    }
    return size;
}

static size_t jpeg_end(const uint8_t *data, size_t size)
{
    int complete = 0;
    size_t n = jpeg_search_from(data, size, &complete);
    return complete ? n : 0;
}

size_t ms_ts_image_end(const uint8_t *data, size_t size, ms_ts_prefix_kind kind)
{
    if (kind == MS_TS_PREFIX_PNG) {
        return png_end(data, size);
    }
    if (kind == MS_TS_PREFIX_JPEG) {
        return jpeg_end(data, size);
    }
    return 0;
}

void ms_ts_scan_opt_default(ms_ts_scan_opt *out)
{
    if (out == NULL) {
        return;
    }
    out->struct_size = sizeof(*out);
    out->max_scan = 0;
}

const char *ms_ts_prefix_kind_name(ms_ts_prefix_kind kind)
{
    switch (kind) {
    case MS_TS_PREFIX_NONE:
        return "none";
    case MS_TS_PREFIX_PNG:
        return "png";
    case MS_TS_PREFIX_JPEG:
        return "jpeg";
    case MS_TS_PREFIX_OTHER:
        return "other";
    default:
        return "unknown";
    }
}

static size_t clamp_scan(size_t req)
{
    if (req == 0) {
        return MS_DEFAULT_PROBE_BYTES;
    }
    if (req > MS_ABS_PROBE_BYTES) {
        return MS_ABS_PROBE_BYTES;
    }
    return req;
}

ms_status ms_ts_scan(const uint8_t *data, size_t size, int eos,
                     const ms_ts_scan_opt *opt, ms_ts_scan_result *out)
{
    ms_ts_scan_opt o;
    size_t cap, win, off = 0, prefer = 0, keep;
    uint16_t psz = 0;
    ms_ts_prefix_kind kind;
    size_t ncopy;

    if (out == NULL || out->struct_size < sizeof(out->struct_size)) {
        return MS_ERR_INVALID_ARG;
    }
    if (size > 0 && data == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    memset(&o, 0, sizeof(o));
    o.struct_size = sizeof(o);
    if (opt != NULL) {
        ncopy = opt->struct_size;
        if (ncopy > sizeof(o)) {
            ncopy = sizeof(o);
        }
        if (ncopy >= sizeof(opt->struct_size)) {
            memcpy(&o, opt, ncopy);
        }
    }
    cap = clamp_scan(o.max_scan);
    win = size;
    if (win > cap) {
        win = cap;
    }
    kind = ms_ts_detect_prefix_kind(data, win);
    {
        int complete = 0;
        if (kind == MS_TS_PREFIX_PNG) {
            prefer = png_search_from(data, win, &complete);
        } else if (kind == MS_TS_PREFIX_JPEG) {
            prefer = jpeg_search_from(data, win, &complete);
        } else {
            prefer = 0;
        }
        /* 图未结束时 prefer==win，find_sync 不会在图内假 0x47 上提前确认。 */
        (void)complete;
    }
    keep = ms_ts_sync_keep_from(data, win);
    if (ms_ts_find_sync(data, win, prefer, &off, &psz)) {
        ms_ts_scan_result tmp;
        memset(&tmp, 0, sizeof(tmp));
        tmp.struct_size = out->struct_size;
        tmp.ts_offset = off;
        tmp.packet_size = psz;
        tmp.kind = (off == 0) ? MS_TS_PREFIX_NONE : kind;
        if (tmp.kind == MS_TS_PREFIX_NONE && off > 0) {
            /* 192 包时间戳头（off < 192）不是伪装前缀；其它偏移才是 OTHER。 */
            if (!(psz == 192 && off < 192)) {
                tmp.kind = MS_TS_PREFIX_OTHER;
            }
        }
        tmp.skipped = off;
        tmp.scanned = win;
        tmp.keep_from = off;
        ncopy = out->struct_size;
        if (ncopy > sizeof(tmp)) {
            ncopy = sizeof(tmp);
        }
        memcpy(out, &tmp, ncopy);
        out->struct_size = tmp.struct_size;
        return MS_OK;
    }
    {
        ms_ts_scan_result tmp;
        memset(&tmp, 0, sizeof(tmp));
        tmp.struct_size = out->struct_size;
        tmp.kind = kind;
        tmp.scanned = win;
        /* 图未结束（含块体未齐）不得丢签名；完整图像可丢到 IEND/EOI。
         * 结构已在签名后断开（仅魔术+TS）时 prefer < win，允许从断点继续。 */
        if (kind == MS_TS_PREFIX_PNG || kind == MS_TS_PREFIX_JPEG) {
            size_t ended = ms_ts_image_end(data, win, kind);
            if (ended > 0) {
                tmp.keep_from = ended;
            } else if (prefer > 0 && prefer < win) {
                tmp.keep_from = prefer;
            } else {
                tmp.keep_from = 0;
            }
        } else if (prefer > 0) {
            tmp.keep_from = prefer;
        } else {
            tmp.keep_from = keep;
        }
        ncopy = out->struct_size;
        if (ncopy > sizeof(tmp)) {
            ncopy = sizeof(tmp);
        }
        memcpy(out, &tmp, ncopy);
        out->struct_size = tmp.struct_size;
    }
    if (size >= cap || eos) {
        return MS_ERR_UNKNOWN;
    }
    return MS_AGAIN;
}
