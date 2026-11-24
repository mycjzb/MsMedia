/* 分配、溢出检查、会话动态预算记账和状态名。
 * by: Long <mycjzb@gmail.com>
 */

#include "ms_internal.h"

#include <limits.h>

void ms_copy_meter_add(ms_copy_meter *m, int kind, size_t n)
{
    if (m == NULL || n == 0) {
        return;
    }
    if (kind == MS_COPY_IN) {
        m->copy_in_ops++;
        m->copy_in_bytes += (uint64_t)n;
    } else if (kind == MS_COPY_ASM) {
        m->copy_asm_ops++;
        m->copy_asm_bytes += (uint64_t)n;
    } else if (kind == MS_COPY_OUT) {
        m->copy_out_ops++;
        m->copy_out_bytes += (uint64_t)n;
    } else if (kind == MS_COPY_MOVE) {
        m->move_out_ops++;
        m->move_out_bytes += (uint64_t)n;
    }
}

int ms_owned_contains(const uint8_t *base, size_t cap,
                      const uint8_t *data, size_t size)
{
    size_t off;
    if (base == NULL || data == NULL || cap == 0 || size == 0) {
        return 0;
    }
    if (data < base) {
        return 0;
    }
    off = (size_t)(data - base);
    if (off > cap) {
        return 0;
    }
    if (size > cap - off) {
        return 0;
    }
    return 1;
}

int ms_size_add(size_t a, size_t b, size_t *out)
{
    if (out == NULL) {
        return -1;
    }
    if (a > (size_t)-1 - b) {
        return -1;
    }
    *out = a + b;
    return 0;
}

int ms_size_mul(size_t a, size_t b, size_t *out)
{
    if (out == NULL) {
        return -1;
    }
    if (a != 0 && b > (size_t)-1 / a) {
        return -1;
    }
    *out = a * b;
    return 0;
}


#if defined(__SIZEOF_INT128__)
int ms_u64_scale(uint64_t v, uint64_t num, uint64_t den, uint64_t *out)
{
    unsigned __int128 x;
    unsigned __int128 q;
    if (out == NULL || den == 0) {
        return -1;
    }
    x = (unsigned __int128)v * (unsigned __int128)num;
    q = x / (unsigned __int128)den;
    if (q > (unsigned __int128)UINT64_MAX) {
        return -1;
    }
    *out = (uint64_t)q;
    return 0;
}
#else
typedef struct {
    uint64_t lo;
    uint64_t hi;
} ms_u128;

static ms_u128 ms_u128_mul(uint64_t a, uint64_t b)
{
    const uint64_t m = 0xffffffffull;
    uint64_t a0 = a & m, a1 = a >> 32, b0 = b & m, b1 = b >> 32;
    uint64_t p0 = a0 * b0, p1 = a0 * b1, p2 = a1 * b0, p3 = a1 * b1;
    uint64_t mid = (p0 >> 32) + (p1 & m) + (p2 & m);
    ms_u128 r;
    r.lo = (p0 & m) | (mid << 32);
    r.hi = p3 + (p1 >> 32) + (p2 >> 32) + (mid >> 32);
    return r;
}

/* 128/64 → 64，商超出则返回 -1。 */
static int ms_u128_divu64(ms_u128 n, uint64_t den, uint64_t *out)
{
    uint64_t q, r, i;
    if (out == NULL || den == 0) {
        return -1;
    }
    if (n.hi >= den) {
        return -1;
    }
    q = 0;
    r = n.hi;
    for (i = 0; i < 64; i++) {
        uint64_t bit = (n.lo >> (63u - (unsigned)i)) & 1ull;
        uint64_t nr = (r << 1) | bit;
        /* 若 r 的最高位在移位时溢出，则 nr 必然 >= den */
        if (r & (1ull << 63)) {
            /* (2^64 + (r<<1|bit) - den)，但 r<<1 已丢失高位 */
            q = (q << 1) | 1ull;
            r = (r << 1) | bit;
            r -= den;
        } else if (nr >= den) {
            q = (q << 1) | 1ull;
            r = nr - den;
        } else {
            q = (q << 1);
            r = nr;
        }
    }
    *out = q;
    return 0;
}

int ms_u64_scale(uint64_t v, uint64_t num, uint64_t den, uint64_t *out)
{
    if (out == NULL || den == 0) {
        return -1;
    }
    return ms_u128_divu64(ms_u128_mul(v, num), den, out);
}
#endif

void ms_default_allocator(ms_allocator *out)
{
    if (out == NULL) {
        return;
    }
    out->alloc_fn = NULL;
    out->free_fn = NULL;
    out->ctx = NULL;
}


typedef struct ms_mem_hdr {
    size_t size;
    uint32_t magic;
    uint32_t pad;
} ms_mem_hdr;

static ms_mem_acct *acct_from(const ms_allocator *a)
{
    ms_mem_acct *m;
    if (a == NULL || a->ctx == NULL) {
        return NULL;
    }
    m = (ms_mem_acct *)a->ctx;
    if (m->magic != MS_MEM_ACCT_MAGIC) {
        return NULL;
    }
    return m;
}

static size_t acct_charge(size_t payload)
{
    size_t n = 0;
    if (ms_size_add(payload, sizeof(ms_mem_hdr), &n) != 0) {
        return 0;
    }
    return n;
}

static void *acct_alloc_fn(void *ctx, size_t size)
{
    ms_mem_acct *m = (ms_mem_acct *)ctx;
    size_t charge, total;
    ms_mem_hdr *h;

    if (m == NULL || m->magic != MS_MEM_ACCT_MAGIC || size == 0) {
        return NULL;
    }
    m->last_status = MS_OK;
    m->last_was_budget = 0;
    charge = acct_charge(size);
    if (charge == 0) {
        m->last_status = MS_ERR_OVERFLOW;
        return NULL;
    }
    if (ms_size_add(m->used, charge, &total) != 0) {
        m->last_status = MS_ERR_OVERFLOW;
        return NULL;
    }
    if (m->budget != 0 && total > m->budget) {
        m->last_status = MS_LIMIT;
        m->last_was_budget = 1;
        return NULL;
    }
    h = (ms_mem_hdr *)ms_mem_alloc(&m->user, charge);
    if (h == NULL) {
        m->last_status = MS_ERR_NOMEM;
        return NULL;
    }
    h->size = size;
    h->magic = MS_MEM_HDR_MAGIC;
    h->pad = 0;
    m->used = total;
    if (m->used > m->peak) {
        m->peak = m->used;
    }
    return (void *)(h + 1);
}

static void acct_free_fn(void *ctx, void *ptr)
{
    ms_mem_acct *m = (ms_mem_acct *)ctx;
    ms_mem_hdr *h;
    size_t charge;

    if (ptr == NULL || m == NULL || m->magic != MS_MEM_ACCT_MAGIC) {
        return;
    }
    h = ((ms_mem_hdr *)ptr) - 1;
    if (h->magic != MS_MEM_HDR_MAGIC) {
        /* 不是本记账块，不应发生；不释放以免误伤。 */
        return;
    }
    charge = acct_charge(h->size);
    if (charge != 0 && m->used >= charge) {
        m->used -= charge;
    } else {
        m->used = 0;
    }
    h->magic = 0;
    ms_mem_free(&m->user, h);
}

void ms_acct_init(ms_mem_acct *m, const ms_allocator *user, size_t budget)
{
    if (m == NULL) {
        return;
    }
    memset(m, 0, sizeof(*m));
    m->magic = MS_MEM_ACCT_MAGIC;
    if (user != NULL) {
        m->user = *user;
    } else {
        ms_default_allocator(&m->user);
    }
    m->budget = budget;
    m->last_status = MS_OK;
}

void ms_acct_bind(ms_mem_acct *m, ms_allocator *iface)
{
    if (m == NULL || iface == NULL) {
        return;
    }
    iface->alloc_fn = acct_alloc_fn;
    iface->free_fn = acct_free_fn;
    iface->ctx = m;
}

int ms_acct_is(const ms_allocator *a)
{
    return acct_from(a) != NULL;
}

int ms_acct_is_budget_limit(const ms_allocator *a)
{
    const ms_mem_acct *m = acct_from(a);
    return (m != NULL && m->last_status == MS_LIMIT && m->last_was_budget);
}

void *ms_mem_alloc(const ms_allocator *a, size_t size)
{
    if (size == 0) {
        return NULL;
    }
    if (a != NULL && a->alloc_fn != NULL) {
        return a->alloc_fn(a->ctx, size);
    }
    return malloc(size);
}

void ms_mem_free(const ms_allocator *a, void *p)
{
    if (p == NULL) {
        return;
    }
    if (a != NULL && a->free_fn != NULL) {
        a->free_fn(a->ctx, p);
        return;
    }
    free(p);
}

static size_t reserve_ncap(size_t cap, size_t need, size_t hard_cap)
{
    size_t ncap = (cap == 0) ? 256u : cap;
    while (ncap < need) {
        if (ncap > hard_cap / 2u) {
            ncap = hard_cap;
            break;
        }
        ncap *= 2u;
    }
    if (ncap > hard_cap) {
        ncap = hard_cap;
    }
    return ncap;
}

ms_status ms_buf_reserve(const ms_allocator *a,
                         uint8_t **buf,
                         size_t *cap,
                         size_t need,
                         size_t hard_cap)
{
    uint8_t *nbuf;
    size_t ncap;
    ms_mem_acct *m;

    if (buf == NULL || cap == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    if (need == 0) {
        return MS_OK;
    }
    m = acct_from(a);
    if (m != NULL) {
        m->last_status = MS_OK;
        m->last_was_budget = 0;
    }
    if (need > hard_cap) {
        if (m != NULL) {
            m->last_status = MS_LIMIT;
            m->last_was_budget = 0;
        }
        return MS_LIMIT;
    }
    if (*buf != NULL && *cap >= need) {
        return MS_OK;
    }
    ncap = reserve_ncap(*cap, need, hard_cap);
    if (ncap < need) {
        if (m != NULL) {
            m->last_status = MS_LIMIT;
            m->last_was_budget = 0;
        }
        return MS_LIMIT;
    }
    if (m != NULL) {
        size_t old_charge = 0;
        size_t new_charge;
        size_t next_used;
        ms_mem_hdr *h;
        if (*buf != NULL) {
            ms_mem_hdr *oh = ((ms_mem_hdr *)(*buf)) - 1;
            if (oh->magic != MS_MEM_HDR_MAGIC) {
                return MS_ERR_STATE;
            }
            old_charge = acct_charge(oh->size);
            if (old_charge == 0) {
                m->last_status = MS_ERR_OVERFLOW;
                return MS_ERR_OVERFLOW;
            }
        }
        new_charge = acct_charge(ncap);
        if (new_charge == 0) {
            m->last_status = MS_ERR_OVERFLOW;
            return MS_ERR_OVERFLOW;
        }
        if (m->used < old_charge) {
            return MS_ERR_STATE;
        }
        if (ms_size_add(m->used - old_charge, new_charge, &next_used) != 0) {
            m->last_status = MS_ERR_OVERFLOW;
            return MS_ERR_OVERFLOW;
        }
        if (m->budget != 0 && next_used > m->budget) {
            m->last_status = MS_LIMIT;
            m->last_was_budget = 1;
            return MS_LIMIT;
        }
        /* 先分配再释放：失败时旧缓冲保持可用。短暂堆峰值可为 old+new。 */
        h = (ms_mem_hdr *)ms_mem_alloc(&m->user, new_charge);
        if (h == NULL) {
            m->last_status = MS_ERR_NOMEM;
            return MS_ERR_NOMEM;
        }
        h->size = ncap;
        h->magic = MS_MEM_HDR_MAGIC;
        h->pad = 0;
        nbuf = (uint8_t *)(h + 1);
        if (*buf != NULL && *cap > 0) {
            memcpy(nbuf, *buf, *cap);
            acct_free_fn(m, *buf);
        }
        if (ms_size_add(m->used, new_charge, &m->used) != 0) {
            /* used 已在 free 时扣除 old；此处不应溢出。 */
            ms_mem_free(&m->user, h);
            *buf = NULL;
            *cap = 0;
            m->last_status = MS_ERR_OVERFLOW;
            return MS_ERR_OVERFLOW;
        }
        if (m->used > m->peak) {
            m->peak = m->used;
        }
        *buf = nbuf;
        *cap = ncap;
        m->last_status = MS_OK;
        m->last_was_budget = 0;
        return MS_OK;
    }
    nbuf = (uint8_t *)ms_mem_alloc(a, ncap);
    if (nbuf == NULL) {
        return MS_ERR_NOMEM;
    }
    if (*buf != NULL && *cap > 0) {
        memcpy(nbuf, *buf, *cap);
        ms_mem_free(a, *buf);
    }
    *buf = nbuf;
    *cap = ncap;
    return MS_OK;
}

uint32_t ms_abi_version(void)
{
    return MSMEDIA_ABI_VERSION;
}

const char *ms_status_name(ms_status st)
{
    switch (st) {
    case MS_OK:
        return "OK";
    case MS_AGAIN:
        return "AGAIN";
    case MS_EOS:
        return "EOS";
    case MS_LIMIT:
        return "LIMIT";
    case MS_ERR_UNSUPPORTED:
        return "UNSUPPORTED";
    case MS_ERR_UNKNOWN:
        return "UNKNOWN";
    case MS_ERR_MALFORMED:
        return "MALFORMED";
    case MS_ERR_OVERFLOW:
        return "OVERFLOW";
    case MS_ERR_CANCELED:
        return "CANCELED";
    case MS_ERR_NOMEM:
        return "NOMEM";
    case MS_ERR_STATE:
        return "STATE";
    case MS_ERR_INVALID_ARG:
        return "INVALID_ARG";
    case MS_ERR_NOT_SEEKABLE:
        return "NOT_SEEKABLE";
    case MS_ERR_TIMEOUT:
        return "TIMEOUT";
    default:
        return "INVALID_STATUS";
    }
}

const char *ms_codec_name(ms_codec codec)
{
    switch (codec) {
    case MS_CODEC_UNKNOWN:
        return "unknown";
    case MS_CODEC_H264:
        return "h264";
    case MS_CODEC_AAC:
        return "aac";
    case MS_CODEC_H263:
        return "h263";
    case MS_CODEC_HEVC:
        return "hevc";
    case MS_CODEC_MPEG4_PART2:
        return "mpeg4p2";
    case MS_CODEC_MPEG2_VIDEO:
        return "mpeg2video";
    case MS_CODEC_AVS_FAMILY:
        return "avs-family";
    case MS_CODEC_AVS_PLUS:
        return "avs+";
    case MS_CODEC_AV1:
        return "av1";
    case MS_CODEC_AC3:
        return "ac3";
    case MS_CODEC_EAC3:
        return "eac3";
    case MS_CODEC_MPEG_AUDIO:
        return "mpeg-audio";
    case MS_CODEC_MP3:
        return "mp3";
    case MS_CODEC_MP2:
        return "mp2";
    case MS_CODEC_OPUS:
        return "opus";
    case MS_CODEC_G711_ALAW:
        return "g711-alaw";
    case MS_CODEC_G711_ULAW:
        return "g711-ulaw";
    case MS_CODEC_SUB_TEXT:
        return "sub-text";
    case MS_CODEC_SUB_SRT:
        return "sub-srt";
    case MS_CODEC_SUB_WEBVTT:
        return "sub-webvtt";
    case MS_CODEC_SUB_TX3G:
        return "sub-tx3g";
    case MS_CODEC_SUB_BITMAP:
        return "sub-bitmap";
    case MS_CODEC_SUB_ASS:
        return "sub-ass";
    case MS_CODEC_FLAC:
        return "flac";
    case MS_CODEC_PCM:
        return "pcm";
    case MS_CODEC_DTS:
        return "dts";
    case MS_CODEC_AAC_LATM:
        return "aac-latm";
    default:
        return "invalid-codec";
    }
}

const char *ms_recog_level_name(ms_recog_level level)
{
    switch (level) {
    case MS_RECOG_NONE:
        return "none";
    case MS_RECOG_CONTAINER_ID:
        return "container-id";
    case MS_RECOG_BITSTREAM:
        return "bitstream";
    case MS_RECOG_AU:
        return "au";
    default:
        return "invalid-level";
    }
}

void ms_session_limits_default(ms_session_limits *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->probe_bytes = MS_DEFAULT_PROBE_BYTES;
    out->au_queue_bytes = MS_DEFAULT_AU_QUEUE_BYTES;
    out->out_slots = MS_DEFAULT_OUT_SLOTS;
    out->max_push_bytes = MS_DEFAULT_MAX_PUSH_BYTES;
    out->max_resync_bytes = MS_DEFAULT_MAX_RESYNC_BYTES;
    out->struct_size = sizeof(*out);
    out->max_assembly_bytes = MS_DEFAULT_MAX_ASSEMBLY_BYTES;
    out->session_budget_bytes = MS_DEFAULT_SESSION_BUDGET_BYTES;
    out->max_lookback_bytes = MS_DEFAULT_MAX_LOOKBACK_BYTES;
}

void ms_session_limits_absolute(ms_session_limits *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->probe_bytes = MS_ABS_PROBE_BYTES;
    out->au_queue_bytes = MS_ABS_AU_QUEUE_BYTES;
    out->out_slots = MS_ABS_OUT_SLOTS;
    out->max_push_bytes = MS_ABS_MAX_PUSH_BYTES;
    out->max_resync_bytes = MS_ABS_MAX_RESYNC_BYTES;
    out->struct_size = sizeof(*out);
    out->max_assembly_bytes = MS_ABS_MAX_ASSEMBLY_BYTES;
    out->session_budget_bytes = MS_ABS_SESSION_BUDGET_BYTES;
    out->max_lookback_bytes = MS_ABS_MAX_LOOKBACK_BYTES;
}

const char *ms_format_name(ms_format format)
{
    switch (format) {
    case MS_FORMAT_UNKNOWN:
        return "unknown";
    case MS_FORMAT_MPEG_TS:
        return "mpeg_ts";
    case MS_FORMAT_MP4:
        return "mp4";
    case MS_FORMAT_FMP4:
        return "fmp4";
    case MS_FORMAT_MOV:
        return "mov";
    case MS_FORMAT_FLV:
        return "flv";
    case MS_FORMAT_HLS:
        return "hls";
    case MS_FORMAT_DASH:
        return "dash";
    case MS_FORMAT_RTSP:
        return "rtsp";
    case MS_FORMAT_RTMP:
        return "rtmp";
    case MS_FORMAT_RTP:
        return "rtp";
    case MS_FORMAT_MATROSKA:
        return "matroska";
    case MS_FORMAT_WEBM:
        return "webm";
    case MS_FORMAT_AVI:
        return "avi";
    case MS_FORMAT_MPEGPS:
        return "mpegps";
    case MS_FORMAT_MP3:
        return "mp3";
    case MS_FORMAT_FLAC:
        return "flac";
    case MS_FORMAT_WAV:
        return "wav";
    case MS_FORMAT_ASF:
        return "asf";
    case MS_FORMAT_RM:
        return "rm";
    default:
        return "invalid_format";
    }
}

const char *ms_sample_format_name(ms_sample_format fmt)
{
    switch (fmt) {
    case MS_SAMPLE_FMT_UNKNOWN:
        return "unknown";
    case MS_SAMPLE_FMT_ANNEXB:
        return "annexb";
    case MS_SAMPLE_FMT_AVCC:
        return "avcc";
    case MS_SAMPLE_FMT_ADTS:
        return "adts";
    case MS_SAMPLE_FMT_AAC_RAW:
        return "aac_raw";
    case MS_SAMPLE_FMT_HVCC:
        return "hvcc";
    case MS_SAMPLE_FMT_MPV:
        return "mpv";
    case MS_SAMPLE_FMT_MP4V:
        return "mp4v";
    case MS_SAMPLE_FMT_H263:
        return "h263";
    case MS_SAMPLE_FMT_AV1:
        return "av1";
    case MS_SAMPLE_FMT_AC3:
        return "ac3";
    case MS_SAMPLE_FMT_MPEG_AUDIO:
        return "mpeg_audio";
    case MS_SAMPLE_FMT_OPUS:
        return "opus";
    case MS_SAMPLE_FMT_CAVS:
        return "cavs";
    case MS_SAMPLE_FMT_FLAC:
        return "flac";
    case MS_SAMPLE_FMT_PCM:
        return "pcm";
    default:
        return "invalid_sample_fmt";
    }
}

ms_status ms_rescale_q(int64_t in, int in_num, int in_den, int out_num, int out_den,
                       int64_t *out)
{
#if defined(__SIZEOF_INT128__)
    __int128 num, den, n, q, r;
    if (out == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    *out = 0;
    if (in_num <= 0 || in_den <= 0 || out_num <= 0 || out_den <= 0) {
        return MS_ERR_INVALID_ARG;
    }
    num = (__int128)in_num * (unsigned)out_den;
    den = (__int128)in_den * (unsigned)out_num;
    if (den <= 0) {
        return MS_ERR_INVALID_ARG;
    }
    n = (__int128)in * num;
    q = n / den;
    r = n % den;
    if (n < 0 && r != 0) {
        q -= 1;
    }
    if (q > (__int128)INT64_MAX || q < (__int128)INT64_MIN) {
        return MS_ERR_OVERFLOW;
    }
    *out = (int64_t)q;
    return MS_OK;
#else
    uint64_t unum, uden, uq, rem_unused;
    int neg, adj;
    ms_u128 nabs;
    if (out == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    *out = 0;
    (void)rem_unused;
    if (in_num <= 0 || in_den <= 0 || out_num <= 0 || out_den <= 0) {
        return MS_ERR_INVALID_ARG;
    }
    unum = (uint64_t)in_num * (uint64_t)(unsigned)out_den;
    uden = (uint64_t)in_den * (uint64_t)(unsigned)out_num;
    if (uden == 0) {
        return MS_ERR_INVALID_ARG;
    }
    neg = (in < 0);
    nabs = ms_u128_mul(neg ? (uint64_t)(-(in + 1)) + 1ull : (uint64_t)in, unum);
    if (ms_u128_divu64(nabs, uden, &uq) != 0 || uq > (uint64_t)INT64_MAX) {
        return MS_ERR_OVERFLOW;
    }
    /* 朝 -inf：负数且不能整除时减 1。用 uq*uden 与 |n| 比较。 */
    adj = 0;
    if (neg) {
        ms_u128 back = ms_u128_mul(uq, uden);
        if (back.lo != nabs.lo || back.hi != nabs.hi) {
            adj = 1;
        }
    }
    if (neg) {
        if (uq == 0 && adj) {
            *out = -1;
        } else if (uq > (uint64_t)INT64_MAX) {
            return MS_ERR_OVERFLOW;
        } else {
            *out = -(int64_t)uq - adj;
        }
    } else {
        *out = (int64_t)uq;
    }
    return MS_OK;
#endif
}

static ms_status signed_one(int has, uint64_t rel, int64_t origin_tb, int *has_out,
                            int64_t *val_out)
{
    int64_t sum;
    if (has_out != NULL) {
        *has_out = has ? 1 : 0;
    }
    if (!has) {
        return MS_OK;
    }
    if (rel > (uint64_t)INT64_MAX) {
        return MS_ERR_OVERFLOW;
    }
    sum = origin_tb + (int64_t)rel;
    if ((origin_tb > 0 && (int64_t)rel > 0 && sum < origin_tb) ||
        (origin_tb < 0 && (int64_t)rel < 0 && sum > origin_tb)) {
        return MS_ERR_OVERFLOW;
    }
    if (val_out != NULL) {
        *val_out = sum;
    }
    return MS_OK;
}

static ms_status origin_in_event_tb(const ms_event *ev, int64_t origin_90k, int64_t *origin_tb)
{
    if (origin_90k == 0) {
        *origin_tb = 0;
        return MS_OK;
    }
    if (ev->time_base_num <= 0 || ev->time_base_den <= 0) {
        return MS_ERR_INVALID_ARG;
    }
    return ms_rescale_q(origin_90k, 1, 90000, ev->time_base_num, ev->time_base_den, origin_tb);
}

ms_status ms_event_signed_ts(const ms_event *ev, int64_t origin_90k, int *has_pts, int64_t *pts,
                             int *has_dts, int64_t *dts)
{
    int64_t origin_tb = 0;
    ms_status st;
    if (ev == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    st = origin_in_event_tb(ev, origin_90k, &origin_tb);
    if (st != MS_OK) {
        return st;
    }
    st = signed_one(ev->has_pts, ev->pts, origin_tb, has_pts, pts);
    if (st != MS_OK) {
        return st;
    }
    return signed_one(ev->has_dts, ev->dts, origin_tb, has_dts, dts);
}

ms_status ms_event_media_time_90k(const ms_event *ev, int64_t origin_90k, int *has_pts,
                                  int64_t *pts_90k, int *has_dts, int64_t *dts_90k)
{
    int hp = 0, hd = 0;
    int64_t pts = 0, dts = 0, p90 = 0, d90 = 0;
    ms_status st;
    st = ms_event_signed_ts(ev, origin_90k, &hp, &pts, &hd, &dts);
    if (st != MS_OK) {
        return st;
    }
    if (has_pts != NULL) {
        *has_pts = hp;
    }
    if (has_dts != NULL) {
        *has_dts = hd;
    }
    if (hp) {
        if (ev->time_base_num <= 0 || ev->time_base_den <= 0) {
            return MS_ERR_INVALID_ARG;
        }
        st = ms_rescale_q(pts, ev->time_base_num, ev->time_base_den, 1, 90000, &p90);
        if (st != MS_OK) {
            return st;
        }
        if (pts_90k != NULL) {
            *pts_90k = p90;
        }
    }
    if (hd) {
        if (ev->time_base_num <= 0 || ev->time_base_den <= 0) {
            return MS_ERR_INVALID_ARG;
        }
        st = ms_rescale_q(dts, ev->time_base_num, ev->time_base_den, 1, 90000, &d90);
        if (st != MS_OK) {
            return st;
        }
        if (dts_90k != NULL) {
            *dts_90k = d90;
        }
    }
    return MS_OK;
}
