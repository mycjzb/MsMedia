/* 分配、溢出检查、会话动态预算记账和状态名。
 * by: Long <mycjzb@gmail.com>
 */

#include "ms_internal.h"

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
}
