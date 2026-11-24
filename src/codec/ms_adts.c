/*
 * AAC ADTS 增量组帧与 AudioSpecificConfig。
 * by: Long <mycjzb@gmail.com>
 */

#include "ms_adts.h"

static const int k_adts_rates[13] = {
    96000, 88200, 64000, 48000, 44100, 32000,
    24000, 22050, 16000, 12000, 11025, 8000, 7350
};

void ms_adts_init(ms_adts *a,
                  const ms_allocator *alloc,
                  size_t max_frame,
                  const ms_adts_callbacks *cb)
{
    memset(a, 0, sizeof(*a));
    a->alloc = alloc;
    a->max_frame = (max_frame == 0) ? MS_DEFAULT_AU_QUEUE_BYTES : max_frame;
    if (cb != NULL) {
        a->cb = *cb;
    }
}

void ms_adts_set_meter(ms_adts *a, ms_copy_meter *m)
{
    if (a != NULL) {
        a->meter = m;
    }
}

void ms_adts_destroy(ms_adts *a)
{
    if (a == NULL) {
        return;
    }
    ms_mem_free(a->alloc, a->buf);
    memset(a, 0, sizeof(*a));
}

void ms_adts_reset(ms_adts *a)
{
    const ms_allocator *alloc;
    size_t max_frame;
    ms_adts_callbacks cb;

    if (a == NULL) {
        return;
    }
    alloc = a->alloc;
    max_frame = a->max_frame;
    cb = a->cb;
    {
        ms_copy_meter *meter = a->meter;
        ms_mem_free(a->alloc, a->buf);
        memset(a, 0, sizeof(*a));
        a->alloc = alloc;
        a->max_frame = max_frame;
        a->cb = cb;
        a->meter = meter;
    }
}

void ms_adts_flush_partial(ms_adts *a)
{
    if (a == NULL) {
        return;
    }
    ms_mem_free(a->alloc, a->buf);
    a->buf = NULL;
    a->len = 0;
    a->cap = 0;
    a->have_start_ts = 0;
    a->start_has_pts = 0;
    a->start_has_dts = 0;
    a->pes_ts_available = 0;
    a->ts_rate = 0;
    a->ts_rem = 0;
    /* last config 保留 */
}

void ms_adts_recover(ms_adts *a)
{
    if (a == NULL) {
        return;
    }
    ms_adts_flush_partial(a);
    a->failed = 0;
    a->fail_status = MS_OK;
}

void ms_adts_begin_pes(ms_adts *a, int has_pts, uint64_t pts, int has_dts, uint64_t dts)
{
    if (a == NULL) {
        return;
    }
    /* 仅当当前没有未完成帧时，PES 时间戳才属于即将开始的第一帧。 */
    a->pes_ts_available = 1;
    a->pes_has_pts = has_pts;
    a->pes_pts = pts;
    a->pes_has_dts = has_dts;
    a->pes_dts = dts;
    /* len==0：上一帧的 90k 推算不得压过新 PES 自带的 PTS（44.1 kHz 等非整除时可见）。 */
    if (a->len == 0) {
        a->have_start_ts = 0;
        a->ts_rate = 0;
        a->ts_rem = 0;
    }
}

static int is_adts_sync(const uint8_t *p)
{
    /* 12-bit 0xFFF，且 layer 必须为 00。 */
    return p[0] == 0xFF && (p[1] & 0xF0) == 0xF0 && (p[1] & 0x06) == 0x00;
}

/*
 * 解析 7 字节 ADTS 固定头。成功返回 0。
 * 不够长返回 1（需要更多数据）。同步或长度不成立返回 -1。
 * 已对齐但属于本子集明确不支持的字段返回 -2。
 */
static int parse_adts_header(const uint8_t *p, size_t n,
                             unsigned *frame_len, unsigned *hdr_len,
                             int *profile, int *sfi, int *chan)
{
    unsigned protection_absent;
    unsigned flen;
    unsigned hlen;
    int pr, idx, ch;

    if (n < 7) {
        return 1;
    }
    if (!is_adts_sync(p)) {
        return -1;
    }
    protection_absent = (unsigned)(p[1] & 0x01);
    hlen = protection_absent ? 7u : 9u;
    pr = (p[2] >> 6) & 0x03;
    idx = (p[2] >> 2) & 0x0F;
    ch = ((p[2] & 0x01) << 2) | ((p[3] >> 6) & 0x03);
    flen = ((unsigned)(p[3] & 0x03) << 11) | ((unsigned)p[4] << 3) |
           ((unsigned)(p[5] >> 5) & 0x07);
    if (hlen == 9u && n < 9u) {
        return 1;
    }
    if (flen < hlen) {
        return -1; /* 长度字段不合法 */
    }
    /* number_of_raw_data_blocks_in_frame：本子集只接受 0（一帧一块）。 */
    if ((p[6] & 0x03) != 0) {
        return -2;
    }
    if (idx > 12 || ch == 0) {
        /* sfi 13-15（含显式频率）与 channel_config=0（PCE）本子集不支持。 */
        return -2;
    }
    *frame_len = flen;
    *hdr_len = hlen;
    *profile = pr;
    *sfi = idx;
    *chan = ch;
    return 0;
}

static void make_asc(uint8_t asc[2], int aot, int sfi, int chan)
{
    /* 基础 AudioSpecificConfig，不含 SBR/PS 扩展。 */
    asc[0] = (uint8_t)(((aot & 0x1F) << 3) | ((sfi >> 1) & 0x07));
    asc[1] = (uint8_t)(((sfi & 0x01) << 7) | ((chan & 0x0F) << 3));
}

static void attach_start_ts(ms_adts *a)
{
    if (a->have_start_ts) {
        return;
    }
    if (a->pes_ts_available) {
        a->have_start_ts = 1;
        a->start_has_pts = a->pes_has_pts;
        a->start_pts = a->pes_pts;
        a->start_has_dts = a->pes_has_dts;
        a->start_dts = a->pes_dts;
        a->pes_ts_available = 0; /* 只属于这个 PES 里开始的第一帧 */
        a->ts_rate = 0;
        a->ts_rem = 0;
        return;
    }
    a->have_start_ts = 1;
    a->start_has_pts = 0;
    a->start_has_dts = 0;
    a->start_pts = 0;
    a->start_dts = 0;
    a->ts_rate = 0;
    a->ts_rem = 0;
}

static void advance_ts_after_frame(ms_adts *a, int sample_rate)
{
    uint64_t inc;
    if (!a->have_start_ts || !a->start_has_pts || sample_rate <= 0) {
        /* 无 PTS 不杜撰绝对时钟。 */
        a->have_start_ts = 0;
        a->start_has_pts = 0;
        a->start_has_dts = 0;
        a->ts_rate = 0;
        a->ts_rem = 0;
        return;
    }
    if (a->ts_rate != sample_rate) {
        /* 采样率变化清余数，不用上一采样率的余数继续累加。 */
        a->ts_rate = sample_rate;
        a->ts_rem = 0;
    }
    /* 有理数累积：与 floor(k * 90000 * 1024 / rate) 等价，不逐帧 floor 累积偏差。
     * 48 kHz 时 90000*1024 整除，每帧仍精确 +1920，余数恒为 0。 */
    a->ts_rem += 90000ull * 1024ull;
    inc = a->ts_rem / (uint64_t)sample_rate;
    a->ts_rem %= (uint64_t)sample_rate;
    a->start_pts = (a->start_pts + inc) & 0x1FFFFFFFFULL;
    if (a->start_has_dts) {
        a->start_dts = (a->start_dts + inc) & 0x1FFFFFFFFULL;
    }
}

static ms_status emit_ready_frame(ms_adts *a)
{
    unsigned flen = 0, hlen = 0;
    int profile = 0, sfi = 0, chan = 0;
    int pr;
    int aot;
    int rate;
    uint8_t asc[2];
    int changed = 0;
    ms_adts_frame fr;
    ms_status st;

    if (a->failed) {
        return a->fail_status;
    }
    pr = parse_adts_header(a->buf, a->len, &flen, &hlen, &profile, &sfi, &chan);
    if (pr == 1) {
        return MS_OK;
    }
    if (pr < 0) {
        return MS_OK; /* 调用方应先对齐同步字 */
    }
    if (flen > a->max_frame) {
        a->failed = 1;
        a->fail_status = MS_ERR_MALFORMED;
        return MS_ERR_MALFORMED;
    }
    if (a->len < flen) {
        return MS_OK;
    }

    aot = profile + 1;
    rate = k_adts_rates[sfi];
    make_asc(asc, aot, sfi, chan);
    if (!a->have_config || memcmp(a->last_asc, asc, 2) != 0 ||
        a->last_rate != rate || a->last_ch != chan || a->last_aot != aot) {
        changed = 1;
    }

    attach_start_ts(a);
    memset(&fr, 0, sizeof(fr));
    fr.data = a->buf;
    fr.size = flen;
    fr.has_pts = a->start_has_pts;
    fr.has_dts = a->start_has_dts;
    fr.pts = a->start_pts;
    fr.dts = a->start_dts;
    fr.format_changed = changed;
    fr.asc[0] = asc[0];
    fr.asc[1] = asc[1];
    fr.asc_size = 2;
    fr.audio_object_type = aot;
    fr.sample_rate = rate;
    fr.channels = chan;

    if (changed && a->cb.on_config != NULL) {
        st = a->cb.on_config(a->cb.ctx, &fr);
        if (st != MS_OK) {
            return st;
        }
        memcpy(a->last_asc, asc, 2);
        a->last_rate = rate;
        a->last_ch = chan;
        a->last_aot = aot;
        a->have_config = 1;
    }

    {
        int taken = 0;
        uint8_t *rem_buf = NULL;
        size_t rem_n = (a->len > flen) ? (a->len - flen) : 0;
        size_t rem_cap = 0;
        if (a->buf != NULL && flen > 0) {
            if (rem_n > 0) {
                rem_buf = (uint8_t *)ms_mem_alloc(a->alloc, rem_n);
                if (rem_buf != NULL) {
                    memcpy(rem_buf, a->buf + flen, rem_n);
                    ms_copy_meter_add(a->meter, MS_COPY_ASM, rem_n);
                    rem_cap = rem_n;
                    fr.owned_base = a->buf;
                    fr.owned_cap = a->cap;
                    fr.owned_taken = &taken;
                }
            } else {
                fr.owned_base = a->buf;
                fr.owned_cap = a->cap;
                fr.owned_taken = &taken;
            }
        }
        if (a->cb.on_au != NULL) {
            st = a->cb.on_au(a->cb.ctx, &fr);
            if (st != MS_OK) {
                if (rem_buf != NULL) {
                    ms_mem_free(a->alloc, rem_buf);
                }
                return st;
            }
        }
        if (taken) {
            a->buf = rem_buf;
            a->cap = rem_cap;
            a->len = rem_n;
        } else {
            if (rem_buf != NULL) {
                ms_mem_free(a->alloc, rem_buf);
            }
            if (a->len > flen) {
                memmove(a->buf, a->buf + flen, a->len - flen);
                a->len -= flen;
            } else {
                a->len = 0;
            }
        }
    }
    advance_ts_after_frame(a, rate);
    return MS_OK;
}

static ms_status drain_buf(ms_adts *a)
{
    ms_status st;

    if (a->failed) {
        return a->fail_status;
    }
    while (a->len > 0) {
        unsigned flen = 0, hlen = 0;
        int profile = 0, sfi = 0, chan = 0;
        int pr;

        /* 丢掉前导非同步字节，但不越过可能尚未到齐的头。 */
        while (a->len >= 2 && !is_adts_sync(a->buf)) {
            memmove(a->buf, a->buf + 1, a->len - 1);
            a->len -= 1;
            /* 错位后，当前缓冲里的帧不再沿用旧的 start 时间戳。 */
            a->have_start_ts = 0;
        }
        if (a->len < 7) {
            return MS_OK;
        }
        if (!is_adts_sync(a->buf)) {
            /* 不足判断或仍不是同步，等待更多字节。 */
            return MS_OK;
        }
        pr = parse_adts_header(a->buf, a->len, &flen, &hlen, &profile, &sfi, &chan);
        if (pr == 1) {
            return MS_OK;
        }
        if (pr < 0) {
            /* 已对齐同步：非法长度走 MALFORMED；已识别但不在子集内走 UNSUPPORTED。 */
            a->failed = 1;
            a->fail_status = (pr == -2) ? MS_ERR_UNSUPPORTED : MS_ERR_MALFORMED;
            return a->fail_status;
        }
        if (flen > a->max_frame) {
            a->failed = 1;
            a->fail_status = MS_ERR_MALFORMED;
            return MS_ERR_MALFORMED;
        }
        if (a->len < flen) {
            attach_start_ts(a); /* 帧已开始，锁定其时间戳 */
            return MS_OK;
        }
        st = emit_ready_frame(a);
        if (st != MS_OK) {
            return st;
        }
    }
    return MS_OK;
}

ms_status ms_adts_drain(ms_adts *a)
{
    if (a == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    return drain_buf(a);
}

ms_status ms_adts_push(ms_adts *a, const uint8_t *data, size_t size, size_t *consumed)
{
    ms_status st;
    size_t need;
    size_t accepted;

    if (consumed != NULL) {
        *consumed = 0;
    }
    if (a == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    if (a->failed) {
        return a->fail_status;
    }
    if (size > 0 && data == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    st = drain_buf(a);
    if (st != MS_OK) {
        return st;
    }
    accepted = 0;
    while (size > 0) {
        size_t room;
        size_t chunk;

        st = drain_buf(a);
        if (st != MS_OK) {
            if (consumed != NULL) {
                *consumed = accepted;
            }
            return st;
        }
        if (a->len > a->max_frame) {
            a->failed = 1;
            a->fail_status = MS_ERR_MALFORMED;
            return MS_ERR_MALFORMED;
        }
        room = a->max_frame - a->len;
        if (room == 0) {
            a->failed = 1;
            a->fail_status = MS_ERR_MALFORMED;
            return MS_ERR_MALFORMED;
        }
        chunk = (size < room) ? size : room;
        if (ms_size_add(a->len, chunk, &need) != 0) {
            a->failed = 1;
            a->fail_status = MS_ERR_OVERFLOW;
            return MS_ERR_OVERFLOW;
        }
        st = ms_buf_reserve(a->alloc, &a->buf, &a->cap, need, a->max_frame);
        if (st != MS_OK) {
            if (st == MS_LIMIT && ms_acct_is_budget_limit(a->alloc)) {
                return MS_LIMIT;
            }
            if (st == MS_LIMIT) {
                a->failed = 1;
                a->fail_status = MS_ERR_MALFORMED;
                return MS_ERR_MALFORMED;
            }
            /* NOMEM：不标记失败，旧缓冲仍可用。 */
            return st;
        }
        memcpy(a->buf + a->len, data, chunk);
        ms_copy_meter_add(a->meter, MS_COPY_ASM, chunk);
        a->len = need;
        if (a->len >= 2 && is_adts_sync(a->buf)) {
            attach_start_ts(a);
        }
        data += chunk;
        size -= chunk;
        accepted += chunk;
        if (consumed != NULL) {
            *consumed = accepted;
        }
    }
    st = drain_buf(a);
    if (consumed != NULL) {
        *consumed = accepted;
    }
    return st;
}

ms_status ms_adts_end(ms_adts *a)
{
    ms_status st;
    if (a == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    if (a->failed) {
        return a->fail_status;
    }
    st = drain_buf(a);
    if (st != MS_OK) {
        return st;
    }
    /* 尾部不完整的 ADTS 丢弃，不作为 AU。 */
    a->len = 0;
    a->have_start_ts = 0;
    a->pes_ts_available = 0;
    return MS_OK;
}
