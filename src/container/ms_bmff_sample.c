/*
 * 渐进 ISO BMFF / QuickTime 的 sample 表与顺序 AU 收获。
 * by: Long <mycjzb@gmail.com>
 */

#include "ms_bmff_sample.h"
#include "ms_avcc.h"

#include <limits.h>
#include <stdlib.h>

#define MS_BMFF_MAX_TRACKS 8

#define T_TRAK MS_BMFF_TYPE('t', 'r', 'a', 'k')
#define T_MOOV MS_BMFF_TYPE('m', 'o', 'o', 'v')
#define T_TKHD MS_BMFF_TYPE('t', 'k', 'h', 'd')
#define T_MDHD MS_BMFF_TYPE('m', 'd', 'h', 'd')
#define T_HDLR MS_BMFF_TYPE('h', 'd', 'l', 'r')
#define T_ELST MS_BMFF_TYPE('e', 'l', 's', 't')
#define T_STSD MS_BMFF_TYPE('s', 't', 's', 'd')
#define T_STTS MS_BMFF_TYPE('s', 't', 't', 's')
#define T_STSC MS_BMFF_TYPE('s', 't', 's', 'c')
#define T_STSZ MS_BMFF_TYPE('s', 't', 's', 'z')
#define T_STZ2 MS_BMFF_TYPE('s', 't', 'z', '2')
#define T_STCO MS_BMFF_TYPE('s', 't', 'c', 'o')
#define T_CO64 MS_BMFF_TYPE('c', 'o', '6', '4')
#define T_STSS MS_BMFF_TYPE('s', 't', 's', 's')
#define T_CTTS MS_BMFF_TYPE('c', 't', 't', 's')
#define T_AVC1 MS_BMFF_TYPE('a', 'v', 'c', '1')
#define T_MP4A MS_BMFF_TYPE('m', 'p', '4', 'a')
#define T_AVCC MS_BMFF_TYPE('a', 'v', 'c', 'C')
#define T_HVC1 MS_BMFF_TYPE('h', 'v', 'c', '1')
#define T_HEV1 MS_BMFF_TYPE('h', 'e', 'v', '1')
#define T_HVCC MS_BMFF_TYPE('h', 'v', 'c', 'C')
#define T_ESDS MS_BMFF_TYPE('e', 's', 'd', 's')
#define T_WAVE MS_BMFF_TYPE('w', 'a', 'v', 'e')
#define T_ENCV MS_BMFF_TYPE('e', 'n', 'c', 'v')
#define T_ENCA MS_BMFF_TYPE('e', 'n', 'c', 'a')
#define T_VIDE MS_BMFF_TYPE('v', 'i', 'd', 'e')
#define T_SOUN MS_BMFF_TYPE('s', 'o', 'u', 'n')
#define T_MOOF MS_BMFF_TYPE('m', 'o', 'o', 'f')
#define T_TRAF MS_BMFF_TYPE('t', 'r', 'a', 'f')
#define T_TREX MS_BMFF_TYPE('t', 'r', 'e', 'x')
#define T_MFHD MS_BMFF_TYPE('m', 'f', 'h', 'd')
#define T_TFHD MS_BMFF_TYPE('t', 'f', 'h', 'd')
#define T_TFDT MS_BMFF_TYPE('t', 'f', 'd', 't')
#define T_TRUN MS_BMFF_TYPE('t', 'r', 'u', 'n')
#define T_SENC MS_BMFF_TYPE('s', 'e', 'n', 'c')
#define T_SAIZ MS_BMFF_TYPE('s', 'a', 'i', 'z')
#define T_SAIO MS_BMFF_TYPE('s', 'a', 'i', 'o')

#define TFHD_BASE_DATA_OFFSET     0x000001u
#define TFHD_SAMPLE_DESC          0x000002u
#define TFHD_DEFAULT_DURATION     0x000008u
#define TFHD_DEFAULT_SIZE         0x000010u
#define TFHD_DEFAULT_FLAGS        0x000020u
#define TFHD_DURATION_IS_EMPTY    0x010000u
#define TFHD_DEFAULT_BASE_IS_MOOF 0x020000u

#define TRUN_DATA_OFFSET          0x000001u
#define TRUN_FIRST_SAMPLE_FLAGS   0x000004u
#define TRUN_SAMPLE_DURATION      0x000100u
#define TRUN_SAMPLE_SIZE          0x000200u
#define TRUN_SAMPLE_FLAGS         0x000400u
#define TRUN_SAMPLE_CTO           0x000800u

#define SAMPLE_FLAG_NON_SYNC      0x00010000u

#define MS_BMFF_MAX_FRAG_SAMPLES  4096

typedef struct {
    uint32_t count;
    uint32_t delta;
} stts_ent;

typedef struct {
    uint32_t first_chunk;
    uint32_t spc;
    uint32_t desc;
} stsc_ent;

typedef struct {
    uint32_t count;
    int32_t offset;
} ctts_ent;

typedef struct ms_bmff_track {
    int used;
    int usable;
    int announced;
    int config_emitted;
    int first_au_pending;
    ms_status err;
    uint32_t id;
    uint32_t timescale;
    uint32_t handler;
    uint32_t desc_index; /* 1-based；0 表示尚未选定 */
    ms_track_kind kind;
    ms_codec codec;
    ms_sample_format sample_fmt;
    uint8_t nal_len_size;
    uint8_t *config;
    size_t config_len;
    int has_tkhd;
    int has_mdhd;
    int has_hdlr;
    int has_stsd;
    int has_stts;
    int has_stsc;
    int has_stsz;
    int has_off;
    int has_stss;
    int has_ctts;
    int has_stz2;
    int track_enabled;
    stts_ent *stts;
    uint32_t stts_n;
    stsc_ent *stsc;
    uint32_t stsc_n;
    uint32_t *stsz;
    uint32_t stsz_n;
    uint32_t stsz_default;
    uint32_t sample_count;
    uint64_t *chunk_off;
    uint32_t chunk_n;
    uint32_t *stss;
    uint32_t stss_n;
    ctts_ent *ctts;
    uint32_t ctts_n;
    uint32_t sample_idx;
    uint32_t chunk_idx;
    uint32_t sample_in_chunk;
    uint32_t stts_i;
    uint32_t stts_left;
    uint32_t ctts_i;
    uint32_t ctts_left;
    uint64_t dts;
    uint64_t next_off;
    int cursor_ready;
    int disc;
    int has_trex;
    uint32_t trex_desc;
    uint32_t trex_duration;
    uint32_t trex_size;
    uint32_t trex_flags;
    uint64_t next_frag_dts;
    int frag_cfg_ready; /* init 的 codec/config 已可用于 moof */
} ms_bmff_track;

enum {
    ANN_IDLE = 0,
    ANN_TRACKS,
    ANN_CONFIG,
    ANN_DONE
};

struct ms_bmff_demux {
    ms_bmff_track tracks[MS_BMFF_MAX_TRACKS];
    int ntracks;
    int cur; /* -1 无当前轨 */
    int finalized;
    int need_seek;
    int unsup_moov_tail;
    int ann;
    int pending; /* 已收齐、尚未成功入队的样本 */
    uint32_t pending_ti;
    uint8_t *sbuf;
    size_t slen;
    size_t scap;
    size_t swant;
    uint64_t pending_pts;
    uint64_t pending_dts;
    int pending_key;
    int pending_fmtchg;
    int pending_disc;
    int skip_rest; /* flush 后丢弃当前样本剩余字节 */
    uint64_t skip_rest_left;
    int is_fragmented;
    int moof_before_moov;
    int moof_open;
    int moof_ready;
    int moof_need_seek;
    int moof_encrypted;
    uint64_t moof_off;
    uint64_t moof_size;
    int first_traf;
    int traf_ti;
    int traf_valid;
    int traf_encrypted;
    uint32_t traf_fs0; /* 本 traf 开始时的分片样本数，加密时回退 */
    int traf_has_tfhd;
    int traf_has_tfdt;
    uint32_t tfhd_flags;
    uint32_t tfhd_desc;
    uint32_t def_duration;
    uint32_t def_size;
    uint32_t def_flags;
    int has_base_off;
    uint64_t base_data_off;
    uint64_t tfdt_dts;
    uint64_t traf_dts;
    uint64_t traf_data_end; /* 本 traf 已布局的数据尾，供下一 traf 缺省基址 */
    uint64_t next_data_off; /* 同一 traf 内下一 trun 的缺省起点 */
    int harvest_frag; /* 当前待入队样本来自分片表 */
    struct ms_bmff_frag_sample {
        uint32_t ti;
        uint32_t size;
        uint32_t flags;
        uint64_t off;
        uint64_t dts;
        int64_t cto;
        int disc;
    } *fs;
    uint32_t fs_n;
    uint32_t fs_cap;
    uint32_t fs_i;
};

static uint16_t rd_be16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static uint32_t rd_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint64_t rd_be64(const uint8_t *p)
{
    return ((uint64_t)rd_be32(p) << 32) | (uint64_t)rd_be32(p + 4);
}

static int u64_add(uint64_t a, uint64_t b, uint64_t *out)
{
    if (a > UINT64_MAX - b) {
        return -1;
    }
    *out = a + b;
    return 0;
}

static int u64_mul(uint64_t a, uint64_t b, uint64_t *out)
{
    if (b != 0 && a > UINT64_MAX / b) {
        return -1;
    }
    *out = a * b;
    return 0;
}

static void fail_set(ms_bmff_parser *p, ms_status st)
{
    if (!p->failed) {
        p->failed = 1;
        p->fail_status = st;
    }
}

static int is_budget(const ms_allocator *a)
{
    return ms_acct_is_budget_limit(a);
}

static void *dmx_alloc(ms_bmff_parser *p, size_t n, ms_status *st)
{
    void *q = ms_mem_alloc(p->alloc, n);
    if (q == NULL) {
        *st = is_budget(p->alloc) ? MS_LIMIT : MS_ERR_NOMEM;
        if (*st != MS_LIMIT) {
            fail_set(p, *st);
        }
    }
    return q;
}

static void track_free(const ms_allocator *a, ms_bmff_track *t)
{
    if (t == NULL) {
        return;
    }
    ms_mem_free(a, t->config);
    ms_mem_free(a, t->stts);
    ms_mem_free(a, t->stsc);
    ms_mem_free(a, t->stsz);
    ms_mem_free(a, t->chunk_off);
    ms_mem_free(a, t->stss);
    ms_mem_free(a, t->ctts);
    memset(t, 0, sizeof(*t));
}

static void frag_free_samples(ms_bmff_parser *p);

void ms_bmff_demux_free(ms_bmff_parser *p)
{
    int i;
    if (p == NULL) {
        return;
    }
    if (p->dmx != NULL) {
        for (i = 0; i < MS_BMFF_MAX_TRACKS; i++) {
            track_free(p->alloc, &p->dmx->tracks[i]);
        }
        ms_mem_free(p->alloc, p->dmx->sbuf);
        p->dmx->sbuf = NULL;
        frag_free_samples(p);
        ms_mem_free(p->alloc, p->dmx);
        p->dmx = NULL;
    }
    ms_mem_free(p->alloc, p->cap_buf);
    p->cap_buf = NULL;
    p->cap_len = 0;
    p->cap_cap = 0;
    p->cap_left = 0;
    p->cap_type = 0;
    ms_mem_free(p->alloc, p->lb_buf);
    p->lb_buf = NULL;
    p->lb_len = 0;
    p->lb_cap = 0;
    p->lb_start = 0;
    p->lb_active = 0;
    p->lb_overflow = 0;
}

static ms_status dmx_ensure(ms_bmff_parser *p)
{
    if (p->dmx != NULL) {
        return MS_OK;
    }
    {
        ms_status st = MS_OK;
        p->dmx = (struct ms_bmff_demux *)dmx_alloc(p, sizeof(*p->dmx), &st);
        if (p->dmx == NULL) {
            return st;
        }
        memset(p->dmx, 0, sizeof(*p->dmx));
        p->dmx->cur = -1;
    }
    return MS_OK;
}

int ms_bmff_is_capture_type(uint32_t type)
{
    return type == T_TKHD || type == T_MDHD || type == T_HDLR ||
           type == T_ELST || type == T_STSD || type == T_STTS ||
           type == T_STSC || type == T_STSZ || type == T_STZ2 ||
           type == T_STCO || type == T_CO64 || type == T_STSS ||
           type == T_CTTS || type == T_TREX || type == T_MFHD ||
           type == T_TFHD || type == T_TFDT || type == T_TRUN ||
           type == T_SENC || type == T_SAIZ || type == T_SAIO;
}

void ms_bmff_parser_set_limits(ms_bmff_parser *p, size_t max_assembly_bytes,
                               size_t max_lookback_bytes)
{
    if (p == NULL) {
        return;
    }
    if (max_assembly_bytes == 0) {
        p->max_assembly_bytes = MS_DEFAULT_MAX_ASSEMBLY_BYTES;
    } else {
        p->max_assembly_bytes = max_assembly_bytes;
    }
    if (max_lookback_bytes == 0) {
        p->max_lookback_bytes = MS_DEFAULT_MAX_LOOKBACK_BYTES;
    } else {
        p->max_lookback_bytes = max_lookback_bytes;
    }
}

int ms_bmff_parser_need_seek(const ms_bmff_parser *p)
{
    return (p != NULL && p->dmx != NULL) ? p->dmx->need_seek : 0;
}

int ms_bmff_parser_need_unsup(const ms_bmff_parser *p)
{
    if (p == NULL || p->dmx == NULL) {
        return 0;
    }
    return p->dmx->need_seek || p->dmx->moof_encrypted;
}

static int is_frag(const ms_bmff_parser *p)
{
    return p->saw_moof || p->saw_mvex || p->saw_styp;
}

static ms_bmff_track *cur_track(ms_bmff_parser *p)
{
    if (p->dmx == NULL || p->dmx->cur < 0 ||
        p->dmx->cur >= p->dmx->ntracks) {
        return NULL;
    }
    return &p->dmx->tracks[p->dmx->cur];
}

static void track_set_err(ms_bmff_track *t, ms_status st)
{
    if (t != NULL && t->err == MS_OK) {
        t->err = st;
    }
}

ms_status ms_bmff_on_container_enter(ms_bmff_parser *p, uint32_t type)
{
    ms_status st;
    if (type != T_TRAK) {
        return MS_OK;
    }
    st = dmx_ensure(p);
    if (st != MS_OK) {
        return st;
    }
    if (p->dmx->ntracks >= MS_BMFF_MAX_TRACKS) {
        p->dmx->cur = -1;
        return MS_OK;
    }
    p->dmx->cur = p->dmx->ntracks;
    memset(&p->dmx->tracks[p->dmx->ntracks], 0, sizeof(p->dmx->tracks[0]));
    p->dmx->tracks[p->dmx->ntracks].used = 1;
    p->dmx->tracks[p->dmx->ntracks].track_enabled = 1;
    p->dmx->ntracks++;
    return MS_OK;
}

static uint32_t sample_size_of(const ms_bmff_track *t, uint32_t idx)
{
    if (t->stsz_default != 0) {
        return t->stsz_default;
    }
    if (idx < t->stsz_n) {
        return t->stsz[idx];
    }
    return 0;
}

static uint32_t spc_for_chunk(const ms_bmff_track *t, uint32_t chunk_idx)
{
    uint32_t i;
    uint32_t spc = 0;
    uint32_t desc = 0;
    for (i = 0; i < t->stsc_n; i++) {
        if (t->stsc[i].first_chunk == 0) {
            continue;
        }
        if (chunk_idx + 1 >= t->stsc[i].first_chunk) {
            spc = t->stsc[i].spc;
            desc = t->stsc[i].desc;
        } else {
            break;
        }
    }
    (void)desc;
    return spc;
}

static int stss_is_sync(const ms_bmff_track *t, uint32_t sample_1based)
{
    uint32_t lo = 0;
    uint32_t hi;
    if (!t->has_stss) {
        return 1;
    }
    hi = t->stss_n;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (t->stss[mid] == sample_1based) {
            return 1;
        }
        if (t->stss[mid] < sample_1based) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return 0;
}

static ms_status init_cursor(ms_bmff_track *t)
{
    if (t->sample_count == 0) {
        t->cursor_ready = 0;
        return MS_OK;
    }
    t->sample_idx = 0;
    t->chunk_idx = 0;
    t->sample_in_chunk = 0;
    t->dts = 0;
    t->stts_i = 0;
    t->stts_left = (t->stts_n > 0) ? t->stts[0].count : 0;
    t->ctts_i = 0;
    t->ctts_left = (t->ctts_n > 0) ? t->ctts[0].count : 0;
    t->next_off = t->chunk_off[0];
    t->cursor_ready = 1;
    t->first_au_pending = 1;
    return MS_OK;
}

static ms_status finalize_track(ms_bmff_parser *p, ms_bmff_track *t)
{
    uint64_t n_from_stts = 0;
    uint64_t n_from_stsc = 0;
    uint64_t n_from_ctts = 0;
    uint32_t i;
    (void)p;

    {
        int frag_init = (p->saw_mvex || p->saw_styp || t->has_trex);
        int headers_ok = (t->has_tkhd && t->has_mdhd && t->has_hdlr &&
                          t->has_stsd);
        int tables_ok = (headers_ok && t->has_stts && t->has_stsc &&
                         t->has_stsz && t->has_off);
        if (t->err != MS_OK) {
            /* 表不完整时（缺 tkhd 等）把解析错误视为本轨不可用，不拖垮整文件。 */
            if (!(frag_init ? headers_ok : tables_ok)) {
                t->usable = 0;
                t->err = MS_OK;
                return MS_OK;
            }
            return t->err;
        }
        if (!(frag_init ? headers_ok : tables_ok)) {
            t->usable = 0;
            return MS_OK;
        }
    }
    if (!t->track_enabled) {
        t->usable = 0;
        return MS_OK;
    }
    if (t->has_stz2) {
        /* 本步不解析 stz2；该轨不可用，其它轨仍可输出。 */
        t->usable = 0;
        return MS_OK;
    }
    if (t->codec != MS_CODEC_H264 && t->codec != MS_CODEC_HEVC && t->codec != MS_CODEC_AAC) {
        t->usable = 0;
        return MS_OK;
    }
    if (t->timescale == 0) {
        t->err = MS_ERR_MALFORMED;
        return MS_ERR_MALFORMED;
    }
    if (t->timescale > (uint32_t)INT32_MAX) {
        t->err = MS_ERR_UNSUPPORTED;
        return MS_ERR_UNSUPPORTED;
    }
    if (t->config == NULL || t->config_len == 0) {
        t->usable = 0;
        return MS_OK;
    }
    if (p->saw_mvex || p->saw_styp || t->has_trex) {
        /* fMP4 初始化段：忽略 moov 内可能为空的 sample 表，等 moof 出帧。 */
        t->frag_cfg_ready = 1;
        t->usable = 1;
        t->cursor_ready = 0;
        t->first_au_pending = 1;
        t->next_frag_dts = 0;
        if (p->dmx != NULL) {
            p->dmx->is_fragmented = 1;
        }
        return MS_OK;
    }
    for (i = 0; i < t->stts_n; i++) {
        if (u64_add(n_from_stts, t->stts[i].count, &n_from_stts) != 0) {
            t->err = MS_ERR_OVERFLOW;
            return MS_ERR_OVERFLOW;
        }
    }
    if (n_from_stts != (uint64_t)t->sample_count) {
        t->err = MS_ERR_MALFORMED;
        return MS_ERR_MALFORMED;
    }
    if (t->sample_count == 0) {
        /* 空表（常见于 fMP4 初始化段）该轨无样本，不是畸形。 */
        t->usable = 0;
        return MS_OK;
    }
    if (t->has_ctts) {
        for (i = 0; i < t->ctts_n; i++) {
            if (u64_add(n_from_ctts, t->ctts[i].count, &n_from_ctts) != 0) {
                t->err = MS_ERR_OVERFLOW;
                return MS_ERR_OVERFLOW;
            }
        }
        if (n_from_ctts != (uint64_t)t->sample_count) {
            t->err = MS_ERR_MALFORMED;
            return MS_ERR_MALFORMED;
        }
    }
    if (t->stsc_n == 0 || t->stsc[0].first_chunk != 1) {
        t->err = MS_ERR_MALFORMED;
        return MS_ERR_MALFORMED;
    }
    for (i = 0; i < t->stsc_n; i++) {
        uint32_t start;
        uint32_t end;
        uint64_t nch;
        uint64_t add;
        if (t->stsc[i].first_chunk == 0 || t->stsc[i].spc == 0 ||
            t->stsc[i].desc == 0) {
            t->err = MS_ERR_MALFORMED;
            return MS_ERR_MALFORMED;
        }
        if (t->desc_index != 0 && t->stsc[i].desc != t->desc_index) {
            t->err = MS_ERR_UNSUPPORTED;
            return MS_ERR_UNSUPPORTED;
        }
        if (i > 0 && t->stsc[i].first_chunk <= t->stsc[i - 1].first_chunk) {
            t->err = MS_ERR_MALFORMED;
            return MS_ERR_MALFORMED;
        }
        start = t->stsc[i].first_chunk;
        end = (i + 1 < t->stsc_n) ? t->stsc[i + 1].first_chunk
                                  : (t->chunk_n + 1u);
        if (start > t->chunk_n + 1u || end < start) {
            t->err = MS_ERR_MALFORMED;
            return MS_ERR_MALFORMED;
        }
        nch = (uint64_t)end - (uint64_t)start;
        if (u64_mul(nch, t->stsc[i].spc, &add) != 0 ||
            u64_add(n_from_stsc, add, &n_from_stsc) != 0) {
            t->err = MS_ERR_OVERFLOW;
            return MS_ERR_OVERFLOW;
        }
    }
    if (n_from_stsc != (uint64_t)t->sample_count) {
        t->err = MS_ERR_MALFORMED;
        return MS_ERR_MALFORMED;
    }
    if (t->sample_count > 0 && t->chunk_n == 0) {
        t->err = MS_ERR_MALFORMED;
        return MS_ERR_MALFORMED;
    }
    /* 同轨 chunk 不得回退重叠。 */
    {
        uint64_t expect = 0;
        uint32_t c;
        uint32_t si = 0;
        for (c = 0; c < t->chunk_n; c++) {
            uint32_t spc = spc_for_chunk(t, c);
            uint64_t chsz = 0;
            uint32_t k;
            if (t->chunk_off[c] == 0) {
                t->err = MS_ERR_MALFORMED;
                return MS_ERR_MALFORMED;
            }
            if (c > 0 && t->chunk_off[c] < expect) {
                t->err = MS_ERR_MALFORMED;
                return MS_ERR_MALFORMED;
            }
            for (k = 0; k < spc; k++) {
                uint32_t sz;
                if (si >= t->sample_count) {
                    t->err = MS_ERR_MALFORMED;
                    return MS_ERR_MALFORMED;
                }
                sz = sample_size_of(t, si);
                if (sz == 0) {
                    t->err = MS_ERR_MALFORMED;
                    return MS_ERR_MALFORMED;
                }
                if (u64_add(chsz, sz, &chsz) != 0) {
                    t->err = MS_ERR_OVERFLOW;
                    return MS_ERR_OVERFLOW;
                }
                si++;
            }
            if (u64_add(t->chunk_off[c], chsz, &expect) != 0) {
                t->err = MS_ERR_OVERFLOW;
                return MS_ERR_OVERFLOW;
            }
        }
        if (si != t->sample_count) {
            t->err = MS_ERR_MALFORMED;
            return MS_ERR_MALFORMED;
        }
    }
    if (t->has_stss) {
        for (i = 0; i < t->stss_n; i++) {
            if (t->stss[i] == 0 || t->stss[i] > t->sample_count ||
                (i > 0 && t->stss[i] <= t->stss[i - 1])) {
                t->err = MS_ERR_MALFORMED;
                return MS_ERR_MALFORMED;
            }
        }
    }
    if (init_cursor(t) != MS_OK) {
        t->err = MS_ERR_MALFORMED;
        return MS_ERR_MALFORMED;
    }
    t->usable = (t->sample_count > 0) ? 1 : 0;
    return MS_OK;
}


static void lookback_free(ms_bmff_parser *p)
{
    ms_mem_free(p->alloc, p->lb_buf);
    p->lb_buf = NULL;
    p->lb_len = 0;
    p->lb_cap = 0;
    p->lb_start = 0;
    p->lb_active = 0;
}

static int lb_covers(const ms_bmff_parser *p, uint64_t off, uint32_t sz)
{
    uint64_t end;
    if (p->lb_buf == NULL || p->lb_len == 0 || sz == 0) {
        return 0;
    }
    if (off < p->lb_start) {
        return 0;
    }
    end = off + (uint64_t)sz;
    if (end < off) {
        return 0;
    }
    return end <= p->lb_start + (uint64_t)p->lb_len;
}

static int past_samples_uncovered(const ms_bmff_parser *p, const ms_bmff_track *t)
{
    uint32_t si = 0;
    uint32_t c;
    if (!t->usable || !t->cursor_ready || t->chunk_n == 0) {
        return 0;
    }
    for (c = 0; c < t->chunk_n && si < t->sample_count; c++) {
        uint32_t in_c;
        uint32_t spc = spc_for_chunk(t, c);
        uint64_t off = t->chunk_off[c];
        for (in_c = 0; in_c < spc && si < t->sample_count; in_c++, si++) {
            uint32_t sz = sample_size_of(t, si);
            if (off < p->file_off) {
                if (!lb_covers(p, off, sz)) {
                    return 1;
                }
            }
            if (u64_add(off, sz, &off) != 0) {
                return 1;
            }
        }
    }
    return 0;
}

void ms_bmff_lookback_on_mdat(ms_bmff_parser *p, uint64_t payload, int until_eof)
{
    size_t want;
    uint8_t *q;
    if (p == NULL) {
        return;
    }
    /* size=0 的 mdat 不回看。moof 已定稿则按偏移现场收获，不缓存整片。
     * 允许：渐进 moov 在尾；以及 fMP4 在 moof 定稿前见到的有界 mdat。 */
    if (until_eof) {
        return;
    }
    if (p->random_ok) {
        return;
    }
    if (p->dmx != NULL && p->dmx->moof_ready) {
        return;
    }
    if (!p->saw_moov && is_frag(p)) {
        /* moof/styp 先于 moov：不把媒体缓存到初始化完成。 */
        return;
    }
    if (p->saw_moov && !(p->dmx != NULL && p->dmx->is_fragmented &&
                         !p->dmx->moof_ready)) {
        /* 已见 moov 的渐进路径不在此回看（现场收获或 P2c 在 moov 前已开窗）。 */
        if (!(p->dmx != NULL && p->dmx->is_fragmented)) {
            return;
        }
        if (p->dmx->moof_ready) {
            return;
        }
    }
    if (!p->saw_moov && (p->dmx != NULL && p->dmx->finalized)) {
        return;
    }
    if (payload == 0 || payload > (uint64_t)p->max_lookback_bytes ||
        payload > (uint64_t)SIZE_MAX) {
        p->lb_overflow = 1;
        return;
    }
    want = (size_t)payload;
    if (p->lb_buf != NULL) {
        /* 已有窗：仅当新 mdat 紧接前一窗尾且合计仍在上限内时扩展。 */
        if (p->file_off == p->lb_start + (uint64_t)p->lb_len &&
            p->lb_len + want <= p->max_lookback_bytes) {
            q = (uint8_t *)ms_mem_alloc(p->alloc, p->lb_len + want);
            if (q == NULL) {
                p->lb_overflow = 1;
                p->lb_active = 0;
                return;
            }
            if (p->lb_len > 0) {
                memcpy(q, p->lb_buf, p->lb_len);
                ms_copy_meter_add(p->meter, MS_COPY_ASM, p->lb_len);
            }
            ms_mem_free(p->alloc, p->lb_buf);
            p->lb_buf = q;
            p->lb_cap = p->lb_len + want;
            p->lb_active = 1;
            return;
        }
        /* 非连续的第二段：保留第一窗，是否够用留到定稿再判断。 */
        p->lb_active = 0;
        return;
    }
    q = (uint8_t *)ms_mem_alloc(p->alloc, want);
    if (q == NULL) {
        /* 预算不足或 OOM：不缓存、不把解析卡在 LIMIT，后续若需要则报回看失败。 */
        p->lb_overflow = 1;
        return;
    }
    p->lb_buf = q;
    p->lb_cap = want;
    p->lb_len = 0;
    p->lb_start = p->file_off;
    p->lb_active = 1;
}

void ms_bmff_lookback_feed(ms_bmff_parser *p, const uint8_t *data, size_t take)
{
    size_t room;
    if (p == NULL || !p->lb_active || p->lb_buf == NULL || take == 0) {
        return;
    }
    room = (p->lb_len < p->lb_cap) ? (p->lb_cap - p->lb_len) : 0;
    if (take > room) {
        take = room;
        p->lb_overflow = 1;
    }
    if (take == 0) {
        return;
    }
    memcpy(p->lb_buf + p->lb_len, data, take);
    ms_copy_meter_add(p->meter, MS_COPY_ASM, take);
    p->lb_len += take;
    if (p->lb_len >= p->lb_cap) {
        p->lb_active = 0;
    }
}


static void frag_reset_moof(ms_bmff_parser *p)
{
    struct ms_bmff_demux *d;
    if (p == NULL || p->dmx == NULL) {
        return;
    }
    d = p->dmx;
    d->moof_open = 0;
    d->moof_ready = 0;
    d->moof_need_seek = 0;
    /* moof_encrypted 跨片保持，直至 reset，避免后续清片丢掉 UNSUPPORTED。 */
    d->first_traf = 1;
    d->traf_ti = -1;
    d->traf_valid = 0;
    d->traf_encrypted = 0;
    d->traf_fs0 = 0;
    d->traf_has_tfhd = 0;
    d->traf_has_tfdt = 0;
    d->has_base_off = 0;
    d->fs_n = 0;
    d->fs_i = 0;
    d->harvest_frag = 0;
}

static void frag_free_samples(ms_bmff_parser *p)
{
    if (p == NULL || p->dmx == NULL) {
        return;
    }
    ms_mem_free(p->alloc, p->dmx->fs);
    p->dmx->fs = NULL;
    p->dmx->fs_cap = 0;
    p->dmx->fs_n = 0;
    p->dmx->fs_i = 0;
}

static ms_bmff_track *track_by_id(struct ms_bmff_demux *d, uint32_t id)
{
    int i;
    if (d == NULL || id == 0) {
        return NULL;
    }
    for (i = 0; i < d->ntracks; i++) {
        if (d->tracks[i].used && d->tracks[i].has_tkhd &&
            d->tracks[i].id == id) {
            return &d->tracks[i];
        }
    }
    return NULL;
}

static int track_index(struct ms_bmff_demux *d, const ms_bmff_track *t)
{
    int i;
    if (d == NULL || t == NULL) {
        return -1;
    }
    for (i = 0; i < d->ntracks; i++) {
        if (&d->tracks[i] == t) {
            return i;
        }
    }
    return -1;
}

static int track_frag_ready(const ms_bmff_track *t)
{
    return t != NULL && t->frag_cfg_ready && t->usable &&
           (t->codec == MS_CODEC_H264 || t->codec == MS_CODEC_HEVC || t->codec == MS_CODEC_AAC) &&
           t->config != NULL && t->config_len > 0 && t->timescale != 0;
}

static int i64_off_to_u64(uint64_t base, int32_t delta, uint64_t *out)
{
    if (delta >= 0) {
        return u64_add(base, (uint64_t)delta, out);
    }
    {
        uint64_t sub = (uint64_t)(-(int64_t)delta);
        if (base < sub) {
            return -1;
        }
        *out = base - sub;
    }
    return 0;
}

static ms_status frag_reserve(ms_bmff_parser *p, uint32_t add)
{
    struct ms_bmff_demux *d = p->dmx;
    uint32_t need;
    struct ms_bmff_frag_sample *q;
    ms_status st = MS_OK;
    if (add == 0) {
        return MS_OK;
    }
    if (d->fs_n > MS_BMFF_MAX_FRAG_SAMPLES ||
        add > MS_BMFF_MAX_FRAG_SAMPLES - d->fs_n) {
        return MS_ERR_UNSUPPORTED;
    }
    need = d->fs_n + add;
    if (need <= d->fs_cap) {
        return MS_OK;
    }
    if (need < 16) {
        need = 16;
    } else if (d->fs_cap > 0 && need < d->fs_cap * 2) {
        need = d->fs_cap * 2;
        if (need > MS_BMFF_MAX_FRAG_SAMPLES) {
            need = MS_BMFF_MAX_FRAG_SAMPLES;
        }
    }
    q = (struct ms_bmff_frag_sample *)dmx_alloc(
        p, (size_t)need * sizeof(*q), &st);
    if (q == NULL) {
        /* 预算不足时不把整段 moof 展开，也不卡在 LIMIT 里吞掉后续箱。 */
        return (st == MS_LIMIT) ? MS_ERR_UNSUPPORTED : st;
    }
    if (d->fs != NULL && d->fs_n > 0) {
        memcpy(q, d->fs, (size_t)d->fs_n * sizeof(*q));
    }
    ms_mem_free(p->alloc, d->fs);
    d->fs = q;
    d->fs_cap = need;
    return MS_OK;
}

ms_status ms_bmff_on_moof_begin(ms_bmff_parser *p, uint64_t offset, uint64_t size)
{
    struct ms_bmff_demux *d;
    ms_status st;
    if (p == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    st = dmx_ensure(p);
    if (st != MS_OK) {
        return st;
    }
    d = p->dmx;
    if (!d->finalized) {
        d->moof_before_moov = 1;
    }
    /* 上一片尚未收完：若仍有未覆盖样本则记回看失败，不输出残片。 */
    if (d->moof_ready && d->fs_i < d->fs_n) {
        d->need_seek = 1;
        d->moof_need_seek = 1;
    }
    frag_reset_moof(p);
    d->fs_n = 0;
    d->fs_i = 0;
    d->moof_off = offset;
    d->moof_size = size;
    d->moof_open = 1;
    d->first_traf = 1;
    d->traf_data_end = offset;
    d->is_fragmented = 1;
    return MS_OK;
}

static ms_status parse_trex(ms_bmff_parser *p, const uint8_t *d, size_t n)
{
    uint32_t id;
    ms_bmff_track *t;
    /* 过短或版本未知：忽略本 trex，不把整文件判失败（识别测试可只用占位）。 */
    if (n < 24 || d[0] != 0) {
        return MS_OK;
    }
    id = rd_be32(d + 4);
    t = track_by_id(p->dmx, id);
    if (t == NULL) {
        /* 轨可能尚未登记：记下待 finalize 绑定。暂存在第一条空 trex 槽。 */
        int i;
        for (i = 0; i < p->dmx->ntracks; i++) {
            if (!p->dmx->tracks[i].has_trex && !p->dmx->tracks[i].has_tkhd) {
                break;
            }
        }
        /* moov 里 trex 通常在 trak 之后；找不到就忽略（finalize 后不可用）。 */
        return MS_OK;
    }
    t->trex_desc = rd_be32(d + 8);
    t->trex_duration = rd_be32(d + 12);
    t->trex_size = rd_be32(d + 16);
    t->trex_flags = rd_be32(d + 20);
    t->has_trex = 1;
    return MS_OK;
}

static ms_status parse_tfhd(ms_bmff_parser *p, const uint8_t *d, size_t n)
{
    struct ms_bmff_demux *dx = p->dmx;
    uint32_t flags;
    uint32_t id;
    size_t o = 8;
    ms_bmff_track *t;
    if (dx == NULL || n < 8) {
        return MS_ERR_MALFORMED;
    }
    if (d[0] != 0) {
        return MS_ERR_UNSUPPORTED;
    }
    flags = ((uint32_t)d[1] << 16) | ((uint32_t)d[2] << 8) | (uint32_t)d[3];
    id = rd_be32(d + 4);
    t = track_by_id(dx, id);
    dx->traf_has_tfhd = 1;
    dx->tfhd_flags = flags;
    dx->traf_ti = t ? track_index(dx, t) : -1;
    dx->traf_valid = 0;
    dx->traf_encrypted = 0;
    dx->traf_fs0 = dx->fs_n;
    dx->def_duration = (t != NULL && t->has_trex) ? t->trex_duration : 0;
    dx->def_size = (t != NULL && t->has_trex) ? t->trex_size : 0;
    dx->def_flags = (t != NULL && t->has_trex) ? t->trex_flags : 0;
    dx->tfhd_desc = (t != NULL && t->has_trex && t->trex_desc != 0)
                        ? t->trex_desc
                        : 1u;
    dx->has_base_off = 0;
    dx->base_data_off = 0;
    if (flags & TFHD_BASE_DATA_OFFSET) {
        if (o + 8 > n) {
            return MS_ERR_MALFORMED;
        }
        dx->base_data_off = rd_be64(d + o);
        dx->has_base_off = 1;
        o += 8;
    }
    if (flags & TFHD_SAMPLE_DESC) {
        if (o + 4 > n) {
            return MS_ERR_MALFORMED;
        }
        dx->tfhd_desc = rd_be32(d + o);
        o += 4;
    }
    if (flags & TFHD_DEFAULT_DURATION) {
        if (o + 4 > n) {
            return MS_ERR_MALFORMED;
        }
        dx->def_duration = rd_be32(d + o);
        o += 4;
    }
    if (flags & TFHD_DEFAULT_SIZE) {
        if (o + 4 > n) {
            return MS_ERR_MALFORMED;
        }
        dx->def_size = rd_be32(d + o);
        o += 4;
    }
    if (flags & TFHD_DEFAULT_FLAGS) {
        if (o + 4 > n) {
            return MS_ERR_MALFORMED;
        }
        dx->def_flags = rd_be32(d + o);
        o += 4;
    }
    (void)o;
    if (t == NULL || !track_frag_ready(t)) {
        /* 未知轨或不在本子集：跳过本 traf，不吐假帧。 */
        dx->traf_valid = 0;
        return MS_OK;
    }
    if (t->desc_index != 0 && dx->tfhd_desc != 0 &&
        dx->tfhd_desc != t->desc_index) {
        dx->traf_valid = 0;
        return MS_OK;
    }
    if (flags & TFHD_DURATION_IS_EMPTY) {
        dx->traf_valid = 0;
        return MS_OK;
    }
    /* 缺省基址 */
    if (dx->has_base_off) {
        /* already set */
    } else if (flags & TFHD_DEFAULT_BASE_IS_MOOF) {
        dx->base_data_off = dx->moof_off;
        dx->has_base_off = 1;
    } else if (dx->first_traf) {
        dx->base_data_off = dx->moof_off;
        dx->has_base_off = 1;
    } else {
        dx->base_data_off = dx->traf_data_end;
        dx->has_base_off = 1;
    }
    dx->next_data_off = dx->base_data_off;
    dx->traf_valid = 1;
    dx->traf_has_tfdt = 0;
    dx->tfdt_dts = t->next_frag_dts;
    dx->traf_dts = t->next_frag_dts;
    return MS_OK;
}

static ms_status parse_tfdt(ms_bmff_parser *p, const uint8_t *d, size_t n)
{
    struct ms_bmff_demux *dx = p->dmx;
    uint8_t ver;
    uint64_t ts;
    if (dx == NULL || n < 8) {
        return MS_ERR_MALFORMED;
    }
    ver = d[0];
    if (ver == 1) {
        if (n < 12) {
            return MS_ERR_MALFORMED;
        }
        ts = rd_be64(d + 4);
    } else if (ver == 0) {
        ts = (uint64_t)rd_be32(d + 4);
    } else {
        return MS_ERR_UNSUPPORTED;
    }
    dx->traf_has_tfdt = 1;
    dx->tfdt_dts = ts;
    dx->traf_dts = ts;
    return MS_OK;
}

static ms_status parse_trun(ms_bmff_parser *p, const uint8_t *d, size_t n)
{
    struct ms_bmff_demux *dx = p->dmx;
    uint8_t ver;
    uint32_t flags;
    uint32_t count;
    size_t o;
    size_t row;
    uint32_t i;
    int32_t data_off = 0;
    int has_data_off = 0;
    uint32_t first_flags;
    int has_first = 0;
    ms_status st;
    ms_bmff_track *t;
    uint64_t run_off;
    int disc0;
    if (dx == NULL || !dx->moof_open) {
        return MS_OK;
    }
    if (!dx->traf_valid || dx->traf_encrypted) {
        return MS_OK;
    }
    if (n < 8) {
        return MS_ERR_MALFORMED;
    }
    ver = d[0];
    if (ver > 1) {
        return MS_ERR_UNSUPPORTED;
    }
    flags = ((uint32_t)d[1] << 16) | ((uint32_t)d[2] << 8) | (uint32_t)d[3];
    count = rd_be32(d + 4);
    o = 8;
    if (flags & TRUN_DATA_OFFSET) {
        if (o + 4 > n) {
            return MS_ERR_MALFORMED;
        }
        data_off = (int32_t)rd_be32(d + o);
        has_data_off = 1;
        o += 4;
    }
    first_flags = dx->def_flags;
    if (flags & TRUN_FIRST_SAMPLE_FLAGS) {
        if (o + 4 > n) {
            return MS_ERR_MALFORMED;
        }
        first_flags = rd_be32(d + o);
        has_first = 1;
        o += 4;
    }
    row = 0;
    if (flags & TRUN_SAMPLE_DURATION) {
        row += 4;
    }
    if (flags & TRUN_SAMPLE_SIZE) {
        row += 4;
    }
    if (flags & TRUN_SAMPLE_FLAGS) {
        row += 4;
    }
    if (flags & TRUN_SAMPLE_CTO) {
        row += 4;
    }
    if (count > 0) {
        uint64_t need;
        if (u64_mul((uint64_t)count, (uint64_t)row, &need) != 0 ||
            need > (uint64_t)(n - o) || o + (size_t)need > n) {
            return MS_ERR_MALFORMED;
        }
    }
    if (count == 0) {
        return MS_OK;
    }
    if (dx->traf_ti < 0 || dx->traf_ti >= dx->ntracks) {
        return MS_OK;
    }
    t = &dx->tracks[dx->traf_ti];
    if (has_data_off) {
        if (i64_off_to_u64(dx->base_data_off, data_off, &run_off) != 0) {
            return MS_ERR_MALFORMED;
        }
    } else {
        run_off = dx->next_data_off;
    }
    st = frag_reserve(p, count);
    if (st != MS_OK) {
        dx->traf_valid = 0;
        dx->fs_n = 0;
        return (st == MS_LIMIT) ? MS_ERR_UNSUPPORTED : st;
    }
    disc0 = 0;
    if (dx->traf_has_tfdt && t->next_frag_dts != 0 &&
        dx->tfdt_dts != t->next_frag_dts && dx->fs_n == 0) {
        disc0 = 1;
    }
    if (dx->traf_has_tfdt && dx->fs_n == 0) {
        dx->traf_dts = dx->tfdt_dts;
    }
    for (i = 0; i < count; i++) {
        uint32_t dur = dx->def_duration;
        uint32_t sz = dx->def_size;
        uint32_t smp_flags = dx->def_flags;
        int64_t cto = 0;
        struct ms_bmff_frag_sample *s;
        if (has_first && i == 0) {
            smp_flags = first_flags;
        }
        if (flags & TRUN_SAMPLE_DURATION) {
            dur = rd_be32(d + o);
            o += 4;
        }
        if (flags & TRUN_SAMPLE_SIZE) {
            sz = rd_be32(d + o);
            o += 4;
        }
        if (flags & TRUN_SAMPLE_FLAGS) {
            smp_flags = rd_be32(d + o);
            o += 4;
        }
        if (flags & TRUN_SAMPLE_CTO) {
            uint32_t raw = rd_be32(d + o);
            o += 4;
            if (ver == 1) {
                cto = (int32_t)raw;
            } else {
                cto = (int64_t)raw;
            }
        }
        if (sz == 0) {
            dx->traf_valid = 0;
            dx->fs_n = 0;
            return MS_ERR_MALFORMED;
        }
        s = &dx->fs[dx->fs_n];
        s->ti = (uint32_t)dx->traf_ti;
        s->size = sz;
        s->flags = smp_flags;
        s->off = run_off;
        s->dts = dx->traf_dts;
        s->cto = cto;
        s->disc = (i == 0) ? disc0 : 0;
        if (u64_add(run_off, (uint64_t)sz, &run_off) != 0 ||
            u64_add(dx->traf_dts, (uint64_t)dur, &dx->traf_dts) != 0) {
            dx->traf_valid = 0;
            dx->fs_n = 0;
            return MS_ERR_OVERFLOW;
        }
        dx->fs_n++;
        disc0 = 0;
    }
    dx->next_data_off = run_off;
    if (run_off > dx->traf_data_end) {
        dx->traf_data_end = run_off;
    }
    t->next_frag_dts = dx->traf_dts;
    return MS_OK;
}

static int frag_sample_cmp(const void *a, const void *b)
{
    const struct ms_bmff_frag_sample *x = (const struct ms_bmff_frag_sample *)a;
    const struct ms_bmff_frag_sample *y = (const struct ms_bmff_frag_sample *)b;
    if (x->off < y->off) {
        return -1;
    }
    if (x->off > y->off) {
        return 1;
    }
    if (x->ti < y->ti) {
        return -1;
    }
    if (x->ti > y->ti) {
        return 1;
    }
    return 0;
}

static ms_status finalize_moof(ms_bmff_parser *p)
{
    struct ms_bmff_demux *d = p->dmx;
    uint32_t i;
    if (d == NULL) {
        return MS_OK;
    }
    d->moof_open = 0;
    if (d->moof_before_moov && !d->finalized) {
        /* 等 moov；本片样本没有轨配置，丢弃表，不吐假帧。 */
        d->fs_n = 0;
        d->moof_ready = 0;
        return MS_OK;
    }
    if (d->fs_n == 0) {
        d->moof_ready = 0;
        return MS_OK;
    }
    qsort(d->fs, d->fs_n, sizeof(d->fs[0]), frag_sample_cmp);
    d->fs_i = 0;
    d->moof_need_seek = 0;
    for (i = 0; i < d->fs_n; i++) {
        if (d->fs[i].off < p->file_off) {
            if (!lb_covers(p, d->fs[i].off, d->fs[i].size)) {
                d->moof_need_seek = 1;
                break;
            }
        }
    }
    if (d->moof_need_seek) {
        /* 不输出本片部分 AU。 */
        d->fs_n = 0;
        d->moof_ready = 0;
        d->need_seek = 1;
        lookback_free(p);
        return MS_OK;
    }
    d->moof_ready = 1;
    if (p->format_emitted) {
        return ms_bmff_announce(p);
    }
    return MS_OK;
}

static ms_status finalize_moov(ms_bmff_parser *p)
{
    int i;
    ms_status st;
    struct ms_bmff_demux *d;
    if (p->dmx == NULL) {
        return MS_OK;
    }
    d = p->dmx;
    if (d->finalized) {
        return MS_OK;
    }
    for (i = 0; i < d->ntracks; i++) {
        st = finalize_track(p, &d->tracks[i]);
        if (st != MS_OK && st != MS_LIMIT) {
            fail_set(p, st);
            return st;
        }
        if (st == MS_LIMIT) {
            return st;
        }
    }
    d->finalized = 1;
    if (d->is_fragmented || p->saw_mvex || p->saw_styp) {
        d->is_fragmented = 1;
        if (d->moof_before_moov) {
            /* 先见到 moof/mdat 再见到 moov：媒体已跳过，不能吐假帧。 */
            d->need_seek = 1;
            lookback_free(p);
        }
        d->ann = ANN_TRACKS;
        return MS_OK;
    }
    if (is_frag(p) && !d->is_fragmented) {
        /* 仅有 moof、没有 mvex/可用 init：不按渐进表出帧。 */
        lookback_free(p);
        return MS_OK;
    }
    /* moov 已走完：已过位置的样本若未被回看窗完整覆盖，则需要回看。
     * 可随机读时按 stco 取样本，不置 need_seek。 */
    d->need_seek = 0;
    if (!p->random_ok) {
        for (i = 0; i < d->ntracks; i++) {
            ms_bmff_track *tr = &d->tracks[i];
            if (!tr->usable || !tr->cursor_ready) {
                continue;
            }
            if (past_samples_uncovered(p, tr)) {
                d->need_seek = 1;
            }
        }
        if (d->need_seek) {
            /* 不完整则不输出部分 AU，释放回看窗。 */
            lookback_free(p);
        }
    } else {
        lookback_free(p);
    }
    d->ann = ANN_TRACKS;
    return MS_OK;
}

ms_status ms_bmff_on_container_leave(ms_bmff_parser *p, uint32_t type)
{
    if (type == T_TRAK) {
        if (p->dmx != NULL) {
            p->dmx->cur = -1;
        }
        return MS_OK;
    }
    if (type == T_MOOV) {
        return finalize_moov(p);
    }
    if (type == T_MOOF) {
        return finalize_moof(p);
    }
    if (type == T_TRAF) {
        if (p->dmx != NULL) {
            p->dmx->first_traf = 0;
            p->dmx->traf_valid = 0;
            p->dmx->traf_ti = -1;
        }
        return MS_OK;
    }
    return MS_OK;
}

/* ---------------- 箱体内容解析 ---------------- */

static int parse_expandable(const uint8_t *p, size_t n, uint32_t *len, size_t *hdr)
{
    size_t i = 0;
    uint32_t v = 0;
    for (;;) {
        if (i >= n || i >= 4) {
            return -1;
        }
        if (v > (0xFFFFFFFFu >> 7)) {
            return -1;
        }
        v = (v << 7) | (uint32_t)(p[i] & 0x7Fu);
        i++;
        if ((p[i - 1] & 0x80u) == 0) {
            break;
        }
    }
    *len = v;
    *hdr = i;
    return 0;
}

static ms_status walk_esds(const uint8_t *d, size_t n, int depth,
                           const uint8_t **asc, size_t *asc_n, int *oti,
                           int *found_oti)
{
    size_t off = 0;
    if (depth > 8) {
        return MS_ERR_MALFORMED;
    }
    while (off < n) {
        uint8_t tag;
        uint32_t len;
        size_t hdr;
        const uint8_t *body;
        if (off >= n) {
            break;
        }
        tag = d[off];
        if (parse_expandable(d + off + 1, n - off - 1, &len, &hdr) != 0) {
            return MS_ERR_MALFORMED;
        }
        if (1u + hdr > n - off) {
            return MS_ERR_MALFORMED;
        }
        if ((uint64_t)len > (uint64_t)(n - off - 1u - hdr)) {
            return MS_ERR_MALFORMED;
        }
        body = d + off + 1 + hdr;
        if (tag == 0x03) {
            size_t b = 0;
            uint8_t flags;
            if (len < 3) {
                return MS_ERR_MALFORMED;
            }
            flags = body[2];
            b = 3;
            if (flags & 0x80u) {
                if (b + 2 > len) {
                    return MS_ERR_MALFORMED;
                }
                b += 2;
            }
            if (flags & 0x40u) {
                uint8_t ul;
                if (b >= len) {
                    return MS_ERR_MALFORMED;
                }
                ul = body[b++];
                if (b + ul > len) {
                    return MS_ERR_MALFORMED;
                }
                b += ul;
            }
            if (flags & 0x20u) {
                if (b + 2 > len) {
                    return MS_ERR_MALFORMED;
                }
                b += 2;
            }
            {
                ms_status st = walk_esds(body + b, len - (uint32_t)b, depth + 1,
                                         asc, asc_n, oti, found_oti);
                if (st != MS_OK) {
                    return st;
                }
            }
        } else if (tag == 0x04) {
            if (len < 13) {
                return MS_ERR_MALFORMED;
            }
            *oti = (int)body[0];
            *found_oti = 1;
            {
                ms_status st = walk_esds(body + 13, len - 13, depth + 1,
                                         asc, asc_n, oti, found_oti);
                if (st != MS_OK) {
                    return st;
                }
            }
        } else if (tag == 0x05) {
            *asc = body;
            *asc_n = len;
        }
        off += 1 + hdr + len;
    }
    return MS_OK;
}

static ms_status parse_esds_box(const uint8_t *d, size_t n, const uint8_t **asc,
                                size_t *asc_n)
{
    int oti = 0;
    int found = 0;
    ms_status st;
    if (n < 4) {
        return MS_ERR_MALFORMED;
    }
    /* FullBox 之后是描述符树。 */
    st = walk_esds(d + 4, n - 4, 0, asc, asc_n, &oti, &found);
    if (st != MS_OK) {
        return st;
    }
    if (!found || *asc == NULL || *asc_n == 0) {
        return MS_ERR_MALFORMED;
    }
    if (!(oti == 0x40 || oti == 0x66 || oti == 0x67 || oti == 0x68)) {
        return MS_ERR_UNSUPPORTED;
    }
    if (!ms_asc_ok(*asc, *asc_n)) {
        return MS_ERR_UNSUPPORTED;
    }
    return MS_OK;
}

static int find_child(const uint8_t *d, size_t n, uint32_t typ,
                      const uint8_t **body, size_t *blen)
{
    size_t off = 0;
    while (off + 8 <= n) {
        uint32_t sz = rd_be32(d + off);
        uint32_t tp = rd_be32(d + off + 4);
        uint32_t hdr = 8;
        uint64_t end;
        if (sz == 1) {
            if (off + 16 > n) {
                return -1;
            }
            end = rd_be64(d + off + 8);
            hdr = 16;
            if (end < hdr) {
                return -1;
            }
        } else if (sz == 0) {
            end = (uint64_t)(n - off);
        } else {
            end = sz;
        }
        if (end < hdr || off + end > n) {
            return -1;
        }
        if (tp == typ) {
            *body = d + off + hdr;
            *blen = (size_t)(end - hdr);
            return 1;
        }
        off += (size_t)end;
    }
    if (off != n && off < n) {
        /* 尾部不足 8 字节：畸形。 */
        if (off + 8 > n && typ != 0) {
            return (off == n) ? 0 : -1;
        }
    }
    return 0;
}

static ms_status copy_mem(ms_bmff_parser *p, uint8_t **dst, size_t *dn,
                          const uint8_t *s, size_t n)
{
    ms_status st = MS_OK;
    uint8_t *q;
    ms_mem_free(p->alloc, *dst);
    *dst = NULL;
    *dn = 0;
    q = (uint8_t *)dmx_alloc(p, n, &st);
    if (q == NULL) {
        return st;
    }
    memcpy(q, s, n);
    *dst = q;
    *dn = n;
    return MS_OK;
}

static ms_status parse_avc1(ms_bmff_parser *p, ms_bmff_track *t,
                            const uint8_t *d, size_t n)
{
    const uint8_t *avcc;
    size_t alen;
    int fr;
    ms_status st;
    if (n < 78) {
        return MS_ERR_MALFORMED;
    }
    fr = find_child(d + 78, n - 78, T_AVCC, &avcc, &alen);
    if (fr < 0) {
        return MS_ERR_MALFORMED;
    }
    if (fr == 0) {
        return MS_ERR_UNSUPPORTED;
    }
    st = ms_avcc_parse(avcc, alen, &t->nal_len_size);
    if (st != MS_OK) {
        return st;
    }
    st = copy_mem(p, &t->config, &t->config_len, avcc, alen);
    if (st != MS_OK) {
        return st;
    }
    t->codec = MS_CODEC_H264;
    t->sample_fmt = MS_SAMPLE_FMT_AVCC;
    t->kind = MS_TRACK_VIDEO;
    return MS_OK;
}

/* HEVC（hvc1 参数集在 hvcC / hev1 参数集可在带内）：长度前缀 NAL，CSD 为 hvcC 原样。
 * by: Long <mycjzb@gmail.com> */
static ms_status parse_hvc1(ms_bmff_parser *p, ms_bmff_track *t,
                            const uint8_t *d, size_t n)
{
    const uint8_t *hvcc;
    size_t hlen;
    int fr;
    ms_status st;
    if (n < 78) {
        return MS_ERR_MALFORMED;
    }
    fr = find_child(d + 78, n - 78, T_HVCC, &hvcc, &hlen);
    if (fr < 0) {
        return MS_ERR_MALFORMED;
    }
    if (fr == 0) {
        return MS_ERR_UNSUPPORTED;
    }
    if (hlen < 23 || hvcc[0] != 1) {
        return MS_ERR_MALFORMED;
    }
    st = copy_mem(p, &t->config, &t->config_len, hvcc, hlen);
    if (st != MS_OK) {
        return st;
    }
    t->nal_len_size = (uint8_t)((hvcc[21] & 3u) + 1u);
    if (t->nal_len_size == 3) {
        return MS_ERR_UNSUPPORTED;
    }
    t->codec = MS_CODEC_HEVC;
    t->sample_fmt = MS_SAMPLE_FMT_HVCC;
    t->kind = MS_TRACK_VIDEO;
    return MS_OK;
}

static ms_status parse_mp4a(ms_bmff_parser *p, ms_bmff_track *t,
                            const uint8_t *d, size_t n)
{
    uint16_t ver;
    size_t pre;
    const uint8_t *es;
    size_t elen;
    const uint8_t *asc = NULL;
    size_t ascn = 0;
    int fr;
    ms_status st;
    if (n < 28) {
        return MS_ERR_MALFORMED;
    }
    ver = rd_be16(d + 8);
    if (ver == 0) {
        pre = 28;
    } else if (ver == 1) {
        pre = 44;
    } else if (ver == 2) {
        pre = 64;
    } else {
        return MS_ERR_UNSUPPORTED;
    }
    if (n < pre) {
        return MS_ERR_MALFORMED;
    }
    fr = find_child(d + pre, n - pre, T_ESDS, &es, &elen);
    if (fr == 0) {
        const uint8_t *wave;
        size_t wlen;
        int wfr = find_child(d + pre, n - pre, T_WAVE, &wave, &wlen);
        if (wfr < 0) {
            return MS_ERR_MALFORMED;
        }
        if (wfr == 1) {
            fr = find_child(wave, wlen, T_ESDS, &es, &elen);
        }
    }
    if (fr < 0) {
        return MS_ERR_MALFORMED;
    }
    if (fr == 0) {
        return MS_ERR_UNSUPPORTED;
    }
    st = parse_esds_box(es, elen, &asc, &ascn);
    if (st != MS_OK) {
        return st;
    }
    st = copy_mem(p, &t->config, &t->config_len, asc, ascn);
    if (st != MS_OK) {
        return st;
    }
    t->codec = MS_CODEC_AAC;
    t->sample_fmt = MS_SAMPLE_FMT_AAC_RAW;
    t->kind = MS_TRACK_AUDIO;
    return MS_OK;
}

static ms_status parse_tkhd(ms_bmff_track *t, const uint8_t *d, size_t n)
{
    uint8_t ver;
    uint32_t flags;
    if (n < 16) {
        return MS_ERR_MALFORMED;
    }
    ver = d[0];
    flags = ((uint32_t)d[1] << 16) | ((uint32_t)d[2] << 8) | (uint32_t)d[3];
    t->track_enabled = (flags & 1u) ? 1 : 0;
    if (ver == 1) {
        if (n < 24) {
            return MS_ERR_MALFORMED;
        }
        t->id = rd_be32(d + 20);
    } else if (ver == 0) {
        t->id = rd_be32(d + 12);
    } else {
        return MS_ERR_UNSUPPORTED;
    }
    t->has_tkhd = 1;
    return MS_OK;
}

static ms_status parse_mdhd(ms_bmff_track *t, const uint8_t *d, size_t n)
{
    uint8_t ver;
    if (n < 20) {
        return MS_ERR_MALFORMED;
    }
    ver = d[0];
    if (ver == 1) {
        if (n < 32) {
            return MS_ERR_MALFORMED;
        }
        t->timescale = rd_be32(d + 20);
    } else if (ver == 0) {
        t->timescale = rd_be32(d + 12);
    } else {
        return MS_ERR_UNSUPPORTED;
    }
    if (t->timescale == 0) {
        return MS_ERR_MALFORMED;
    }
    t->has_mdhd = 1;
    return MS_OK;
}

static ms_status parse_hdlr(ms_bmff_track *t, const uint8_t *d, size_t n)
{
    if (n < 12) {
        return MS_ERR_MALFORMED;
    }
    t->handler = rd_be32(d + 8);
    if (t->handler == T_VIDE) {
        t->kind = MS_TRACK_VIDEO;
    } else if (t->handler == T_SOUN) {
        t->kind = MS_TRACK_AUDIO;
    }
    t->has_hdlr = 1;
    return MS_OK;
}

static ms_status parse_stsd(ms_bmff_parser *p, ms_bmff_track *t,
                           const uint8_t *d, size_t n)
{
    uint32_t count;
    size_t off;
    uint32_t i;
    if (n < 8) {
        return MS_ERR_MALFORMED;
    }
    if (d[0] != 0) {
        return MS_ERR_UNSUPPORTED;
    }
    count = rd_be32(d + 4);
    off = 8;
    for (i = 0; i < count; i++) {
        uint32_t sz;
        uint32_t typ;
        if (off + 8 > n) {
            return MS_ERR_MALFORMED;
        }
        sz = rd_be32(d + off);
        typ = rd_be32(d + off + 4);
        if (sz < 8 || off + sz > n) {
            return MS_ERR_MALFORMED;
        }
        if (typ == T_ENCV || typ == T_ENCA) {
            return MS_ERR_UNSUPPORTED;
        }
        if (t->codec == MS_CODEC_UNKNOWN) {
            ms_status st = MS_OK;
            if (typ == T_AVC1) {
                st = parse_avc1(p, t, d + off + 8, sz - 8);
                if (st == MS_OK) {
                    t->desc_index = i + 1;
                }
            } else if (typ == T_HVC1 || typ == T_HEV1) {
                st = parse_hvc1(p, t, d + off + 8, sz - 8);
                if (st == MS_OK) {
                    t->desc_index = i + 1;
                }
            } else if (typ == T_MP4A) {
                st = parse_mp4a(p, t, d + off + 8, sz - 8);
                if (st == MS_OK) {
                    t->desc_index = i + 1;
                }
            } else {
                st = MS_OK; /* 未知条目：继续看下一项 */
            }
            if (st != MS_OK && st != MS_ERR_UNSUPPORTED) {
                return st;
            }
        }
        off += sz;
    }
    if (off != n) {
        /* 允许填充；不允许越过。已由 sz 检查。 */
    }
    t->has_stsd = 1;
    return MS_OK;
}

static ms_status alloc_u32(ms_bmff_parser *p, uint32_t **out, uint32_t count)
{
    size_t bytes;
    ms_status st = MS_OK;
    if (ms_size_mul((size_t)count, sizeof(uint32_t), &bytes) != 0) {
        fail_set(p, MS_ERR_OVERFLOW);
        return MS_ERR_OVERFLOW;
    }
    *out = (uint32_t *)dmx_alloc(p, bytes, &st);
    return (*out == NULL && count > 0) ? st : MS_OK;
}

static ms_status parse_stts(ms_bmff_parser *p, ms_bmff_track *t,
                           const uint8_t *d, size_t n)
{
    uint32_t count, i;
    size_t need;
    ms_status st = MS_OK;
    if (n < 8 || d[0] != 0) {
        return (n < 8) ? MS_ERR_MALFORMED : MS_ERR_UNSUPPORTED;
    }
    count = rd_be32(d + 4);
    if (ms_size_mul((size_t)count, 8, &need) != 0 || need > n - 8) {
        return MS_ERR_MALFORMED;
    }
    t->stts = (stts_ent *)dmx_alloc(p, (count == 0) ? 1 : count * sizeof(*t->stts),
                                    &st);
    if (count > 0 && t->stts == NULL) {
        return st;
    }
    for (i = 0; i < count; i++) {
        t->stts[i].count = rd_be32(d + 8 + 8 * i);
        t->stts[i].delta = rd_be32(d + 12 + 8 * i);
    }
    t->stts_n = count;
    t->has_stts = 1;
    return MS_OK;
}

static ms_status parse_stsc(ms_bmff_parser *p, ms_bmff_track *t,
                           const uint8_t *d, size_t n)
{
    uint32_t count, i;
    size_t need;
    ms_status st = MS_OK;
    if (n < 8 || d[0] != 0) {
        return (n < 8) ? MS_ERR_MALFORMED : MS_ERR_UNSUPPORTED;
    }
    count = rd_be32(d + 4);
    if (ms_size_mul((size_t)count, 12, &need) != 0 || need > n - 8) {
        return MS_ERR_MALFORMED;
    }
    t->stsc = (stsc_ent *)dmx_alloc(p, (count == 0) ? 1 : count * sizeof(*t->stsc),
                                    &st);
    if (count > 0 && t->stsc == NULL) {
        return st;
    }
    for (i = 0; i < count; i++) {
        t->stsc[i].first_chunk = rd_be32(d + 8 + 12 * i);
        t->stsc[i].spc = rd_be32(d + 12 + 12 * i);
        t->stsc[i].desc = rd_be32(d + 16 + 12 * i);
    }
    t->stsc_n = count;
    t->has_stsc = 1;
    return MS_OK;
}

static ms_status parse_stsz(ms_bmff_parser *p, ms_bmff_track *t,
                           const uint8_t *d, size_t n)
{
    uint32_t defv, count, i;
    size_t need;
    if (n < 12 || d[0] != 0) {
        return (n < 12) ? MS_ERR_MALFORMED : MS_ERR_UNSUPPORTED;
    }
    defv = rd_be32(d + 4);
    count = rd_be32(d + 8);
    t->stsz_default = defv;
    t->sample_count = count;
    if (defv == 0) {
        ms_status st = MS_OK;
        if (ms_size_mul((size_t)count, 4, &need) != 0 || need > n - 12) {
            return MS_ERR_MALFORMED;
        }
        if (count > 0) {
            t->stsz = (uint32_t *)dmx_alloc(p, need, &st);
            if (t->stsz == NULL) {
                return st;
            }
            for (i = 0; i < count; i++) {
                t->stsz[i] = rd_be32(d + 12 + 4 * i);
            }
        }
        t->stsz_n = count;
    }
    t->has_stsz = 1;
    return MS_OK;
}

static ms_status parse_stco(ms_bmff_parser *p, ms_bmff_track *t,
                           const uint8_t *d, size_t n, int is64)
{
    uint32_t count, i;
    size_t esz = is64 ? 8u : 4u;
    size_t need;
    ms_status st = MS_OK;
    if (t->has_off) {
        return MS_ERR_MALFORMED;
    }
    if (n < 8 || d[0] != 0) {
        return (n < 8) ? MS_ERR_MALFORMED : MS_ERR_UNSUPPORTED;
    }
    count = rd_be32(d + 4);
    if (ms_size_mul((size_t)count, esz, &need) != 0 || need > n - 8) {
        return MS_ERR_MALFORMED;
    }
    if (count > 0) {
        size_t bytes;
        if (ms_size_mul((size_t)count, sizeof(uint64_t), &bytes) != 0) {
            fail_set(p, MS_ERR_OVERFLOW);
            return MS_ERR_OVERFLOW;
        }
        t->chunk_off = (uint64_t *)dmx_alloc(p, bytes, &st);
        if (t->chunk_off == NULL) {
            return st;
        }
        for (i = 0; i < count; i++) {
            t->chunk_off[i] = is64 ? rd_be64(d + 8 + 8 * i)
                                   : (uint64_t)rd_be32(d + 8 + 4 * i);
        }
    }
    t->chunk_n = count;
    t->has_off = 1;
    return MS_OK;
}

static ms_status parse_stss(ms_bmff_parser *p, ms_bmff_track *t,
                           const uint8_t *d, size_t n)
{
    uint32_t count, i;
    size_t need;
    ms_status st;
    if (n < 8 || d[0] != 0) {
        return (n < 8) ? MS_ERR_MALFORMED : MS_ERR_UNSUPPORTED;
    }
    count = rd_be32(d + 4);
    if (ms_size_mul((size_t)count, 4, &need) != 0 || need > n - 8) {
        return MS_ERR_MALFORMED;
    }
    st = alloc_u32(p, &t->stss, count);
    if (st != MS_OK) {
        return st;
    }
    for (i = 0; i < count; i++) {
        t->stss[i] = rd_be32(d + 8 + 4 * i);
    }
    t->stss_n = count;
    t->has_stss = 1;
    return MS_OK;
}

static ms_status parse_ctts(ms_bmff_parser *p, ms_bmff_track *t,
                           const uint8_t *d, size_t n)
{
    uint8_t ver;
    uint32_t count, i;
    size_t need;
    ms_status st = MS_OK;
    if (n < 8) {
        return MS_ERR_MALFORMED;
    }
    ver = d[0];
    if (ver > 1) {
        return MS_ERR_UNSUPPORTED;
    }
    count = rd_be32(d + 4);
    if (ms_size_mul((size_t)count, 8, &need) != 0 || need > n - 8) {
        return MS_ERR_MALFORMED;
    }
    t->ctts = (ctts_ent *)dmx_alloc(p, (count == 0) ? 1 : count * sizeof(*t->ctts),
                                    &st);
    if (count > 0 && t->ctts == NULL) {
        return st;
    }
    for (i = 0; i < count; i++) {
        uint32_t raw;
        t->ctts[i].count = rd_be32(d + 8 + 8 * i);
        raw = rd_be32(d + 12 + 8 * i);
        if (ver == 1) {
            t->ctts[i].offset = (int32_t)raw;
        } else if (raw > (uint32_t)INT32_MAX) {
            /* v0 无符号偏移超过有符号范围：本步用 int32 累加会截断，拒绝。 */
            return MS_ERR_UNSUPPORTED;
        } else {
            t->ctts[i].offset = (int32_t)raw;
        }
    }
    t->ctts_n = count;
    t->has_ctts = 1;
    return MS_OK;
}

ms_status ms_bmff_on_captured(ms_bmff_parser *p, uint32_t type,
                              const uint8_t *data, size_t size)
{
    ms_bmff_track *t;
    ms_status st = MS_OK;
    if (type == T_TREX || type == T_MFHD || type == T_TFHD ||
        type == T_TFDT || type == T_TRUN || type == T_SENC ||
        type == T_SAIZ || type == T_SAIO) {
        if (p->dmx == NULL) {
            st = dmx_ensure(p);
            if (st != MS_OK) {
                return st;
            }
        }
        if (type == T_TREX) {
            return parse_trex(p, data, size);
        }
        if (type == T_MFHD) {
            return MS_OK;
        }
        if (type == T_SENC || type == T_SAIZ || type == T_SAIO) {
            if (p->dmx != NULL) {
                /* senc 常在 trun 之后：丢掉已登记的本 traf 样本，不把密文当帧。 */
                if (p->dmx->fs_n > p->dmx->traf_fs0) {
                    p->dmx->fs_n = p->dmx->traf_fs0;
                }
                p->dmx->traf_encrypted = 1;
                p->dmx->traf_valid = 0;
                p->dmx->moof_encrypted = 1;
            }
            return MS_OK;
        }
        if (type == T_TFHD) {
            return parse_tfhd(p, data, size);
        }
        if (type == T_TFDT) {
            return parse_tfdt(p, data, size);
        }
        if (type == T_TRUN) {
            st = parse_trun(p, data, size);
            if (st != MS_OK && st != MS_LIMIT) {
                fail_set(p, st);
                return st;
            }
            return st;
        }
    }
    if (type == T_STZ2) {
        t = cur_track(p);
        if (t != NULL) {
            t->has_stz2 = 1;
        }
        return MS_OK;
    }
    t = cur_track(p);
    if (t == NULL) {
        return MS_OK;
    }
    if (t->err != MS_OK) {
        return MS_OK;
    }
    switch (type) {
    case T_TKHD:
        st = parse_tkhd(t, data, size);
        break;
    case T_MDHD:
        st = parse_mdhd(t, data, size);
        break;
    case T_HDLR:
        st = parse_hdlr(t, data, size);
        break;
    case T_ELST:
        /* 本步时间戳用媒体解码/合成时间，不把 elst 映射进 uint64 PTS。 */
        st = MS_OK;
        break;
    case T_STSD:
        st = parse_stsd(p, t, data, size);
        break;
    case T_STTS:
        if (t->has_stts) {
            st = MS_ERR_MALFORMED;
        } else {
            st = parse_stts(p, t, data, size);
        }
        break;
    case T_STSC:
        if (t->has_stsc) {
            st = MS_ERR_MALFORMED;
        } else {
            st = parse_stsc(p, t, data, size);
        }
        break;
    case T_STSZ:
        if (t->has_stsz) {
            st = MS_ERR_MALFORMED;
        } else {
            st = parse_stsz(p, t, data, size);
        }
        break;
    case T_STCO:
        st = parse_stco(p, t, data, size, 0);
        break;
    case T_CO64:
        st = parse_stco(p, t, data, size, 1);
        break;
    case T_STSS:
        if (t->has_stss) {
            st = MS_ERR_MALFORMED;
        } else {
            st = parse_stss(p, t, data, size);
        }
        break;
    case T_CTTS:
        if (t->has_ctts) {
            st = MS_ERR_MALFORMED;
        } else {
            st = parse_ctts(p, t, data, size);
        }
        break;
    default:
        st = MS_OK;
        break;
    }
    if (st != MS_OK && st != MS_LIMIT) {
        track_set_err(t, st);
        /* 解析错误先记在轨上，finalize 时再决定是否升级为会话失败。 */
        return MS_OK;
    }
    return st;
}

static void fill_track_info(const ms_bmff_track *t, ms_track_info *out)
{
    memset(out, 0, sizeof(*out));
    out->id = t->id;
    out->kind = t->kind;
    out->codec = t->codec;
    out->level = MS_RECOG_AU;
    out->stream_type = 0;
    out->language = NULL;
    out->name = NULL;
    out->is_default = 0;
    out->is_forced = 0;
    out->selected = 1;
}

static ms_status emit_one_track(ms_bmff_parser *p, ms_bmff_track *t)
{
    ms_track_info info;
    if (p->cb.on_track == NULL) {
        t->announced = 1;
        return MS_OK;
    }
    fill_track_info(t, &info);
    {
        ms_status st = p->cb.on_track(p->cb.ctx, &info, 0);
        if (st == MS_OK) {
            t->announced = 1;
        }
        return st;
    }
}

static ms_status emit_one_config(ms_bmff_parser *p, ms_bmff_track *t)
{
    ms_track_info info;
    fill_track_info(t, &info);
    if (p->cb.on_codec_config == NULL) {
        t->config_emitted = 1;
        return MS_OK;
    }
    {
        ms_status st = p->cb.on_codec_config(p->cb.ctx, &info,
                                             t->config, t->config_len,
                                             t->sample_fmt, 1,
                                             (int)t->timescale);
        if (st == MS_OK) {
            t->config_emitted = 1;
        }
        return st;
    }
}

ms_status ms_bmff_announce(ms_bmff_parser *p)
{
    struct ms_bmff_demux *d;
    int i;
    if (p == NULL || p->failed) {
        return p ? p->fail_status : MS_ERR_INVALID_ARG;
    }
    if (p->dmx == NULL || !p->dmx->finalized) {
        return MS_OK;
    }
    d = p->dmx;
    if (d->ann == ANN_IDLE) {
        d->ann = ANN_TRACKS;
    }
    if (d->ann == ANN_TRACKS) {
        for (i = 0; i < d->ntracks; i++) {
            ms_bmff_track *t = &d->tracks[i];
            if (!t->usable || t->announced) {
                continue;
            }
            {
                ms_status st = emit_one_track(p, t);
                if (st != MS_OK) {
                    return st;
                }
            }
        }
        d->ann = ANN_CONFIG;
    }
    if (d->ann == ANN_CONFIG) {
        for (i = 0; i < d->ntracks; i++) {
            ms_bmff_track *t = &d->tracks[i];
            if (!t->usable || t->config_emitted) {
                continue;
            }
            {
                ms_status st = emit_one_config(p, t);
                if (st != MS_OK) {
                    return st;
                }
            }
        }
        d->ann = ANN_DONE;
    }
    return ms_bmff_replay_lookback(p);
}

int ms_bmff_should_harvest(const ms_bmff_parser *p)
{
    int i;
    if (p == NULL || p->dmx == NULL || !p->dmx->finalized) {
        return 0;
    }
    if (p->dmx->is_fragmented || is_frag(p)) {
        return p->dmx->moof_ready && !p->dmx->moof_need_seek &&
               p->dmx->fs_n > p->dmx->fs_i;
    }
    if (p->dmx->need_seek) {
        return 0;
    }
    for (i = 0; i < p->dmx->ntracks; i++) {
        if (p->dmx->tracks[i].usable) {
            return 1;
        }
    }
    return 0;
}

static int pick_next(struct ms_bmff_demux *d, uint32_t *ti)
{
    int i;
    int found = 0;
    uint64_t best = 0;
    uint32_t best_i = 0;
    for (i = 0; i < d->ntracks; i++) {
        ms_bmff_track *t = &d->tracks[i];
        if (!t->usable || !t->cursor_ready) {
            continue;
        }
        if (t->sample_idx >= t->sample_count) {
            continue;
        }
        if (!found || t->next_off < best ||
            (t->next_off == best && (uint32_t)i < best_i)) {
            found = 1;
            best = t->next_off;
            best_i = (uint32_t)i;
        }
    }
    if (!found) {
        return 0;
    }
    *ti = best_i;
    return 1;
}

static int64_t ctts_offset(const ms_bmff_track *t)
{
    if (!t->has_ctts || t->ctts_i >= t->ctts_n) {
        return 0;
    }
    return (int64_t)t->ctts[t->ctts_i].offset;
}

static ms_status advance_track(ms_bmff_track *t)
{
    uint32_t sz = sample_size_of(t, t->sample_idx);
    uint64_t nxt;
    uint32_t spc;
    uint32_t delta;
    if (t->stts_i >= t->stts_n || t->stts_left == 0) {
        return MS_ERR_MALFORMED;
    }
    delta = t->stts[t->stts_i].delta;
    if (u64_add(t->dts, delta, &t->dts) != 0) {
        return MS_ERR_OVERFLOW;
    }
    t->stts_left--;
    if (t->stts_left == 0) {
        t->stts_i++;
        if (t->stts_i < t->stts_n) {
            t->stts_left = t->stts[t->stts_i].count;
        }
    }
    if (t->has_ctts) {
        if (t->ctts_i >= t->ctts_n || t->ctts_left == 0) {
            return MS_ERR_MALFORMED;
        }
        t->ctts_left--;
        if (t->ctts_left == 0) {
            t->ctts_i++;
            if (t->ctts_i < t->ctts_n) {
                t->ctts_left = t->ctts[t->ctts_i].count;
            }
        }
    }
    if (u64_add(t->next_off, sz, &nxt) != 0) {
        return MS_ERR_OVERFLOW;
    }
    t->sample_idx++;
    t->sample_in_chunk++;
    spc = spc_for_chunk(t, t->chunk_idx);
    if (spc == 0) {
        return MS_ERR_MALFORMED;
    }
    if (t->sample_in_chunk >= spc) {
        t->chunk_idx++;
        t->sample_in_chunk = 0;
        if (t->sample_idx < t->sample_count) {
            if (t->chunk_idx >= t->chunk_n) {
                return MS_ERR_MALFORMED;
            }
            t->next_off = t->chunk_off[t->chunk_idx];
        } else {
            t->next_off = nxt;
        }
    } else {
        t->next_off = nxt;
    }
    return MS_OK;
}

static ms_status emit_pending(ms_bmff_parser *p)
{
    struct ms_bmff_demux *d = p->dmx;
    ms_bmff_track *t;
    ms_track_info info;
    ms_au_frame fr;
    ms_status st;
    int taken = 0;
    if (d == NULL || !d->pending) {
        return MS_OK;
    }
    t = &d->tracks[d->pending_ti];
    fill_track_info(t, &info);
    memset(&fr, 0, sizeof(fr));
    fr.data = d->sbuf;
    fr.size = d->slen;
    fr.owned_base = d->sbuf;
    fr.owned_cap = d->scap;
    fr.owned_taken = &taken;
    fr.has_pts = 1;
    fr.has_dts = 1;
    fr.pts = d->pending_pts;
    fr.dts = d->pending_dts;
    fr.format_changed = d->pending_fmtchg;
    fr.is_keyframe = d->pending_key;
    fr.discontinuity = d->pending_disc;
    fr.preroll = 0;
    if (p->have_seek_land && t->kind == MS_TRACK_VIDEO) {
        uint64_t pts90 = 0;
        if (t->timescale > 0 &&
            ms_u64_scale(d->pending_pts, 90000ull, (uint64_t)t->timescale,
                         &pts90) == 0) {
            if ((int64_t)pts90 < p->seek_req_90k) {
                fr.preroll = 1;
            }
        }
    }
    fr.sample_format = t->sample_fmt;
    fr.time_base_num = 1;
    fr.time_base_den = (int)t->timescale;
    if (p->cb.on_au == NULL) {
        d->pending = 0;
        d->slen = 0;
        t->first_au_pending = 0;
        t->disc = 0;
        if (d->harvest_frag) {
            d->fs_i++;
            if (d->fs_i >= d->fs_n) {
                d->moof_ready = 0;
                d->harvest_frag = 0;
            }
            return MS_OK;
        }
        return advance_track(t);
    }
    st = p->cb.on_au(p->cb.ctx, &info, &fr);
    if (st == MS_OK) {
        if (taken) {
            d->sbuf = NULL;
            d->scap = 0;
        }
        d->pending = 0;
        d->slen = 0;
        t->first_au_pending = 0;
        t->disc = 0;
        if (d->harvest_frag) {
            d->fs_i++;
            if (d->fs_i >= d->fs_n) {
                d->moof_ready = 0;
                d->harvest_frag = 0;
            }
            return MS_OK;
        }
        st = advance_track(t);
        if (st != MS_OK) {
            fail_set(p, st);
            return st;
        }
    }
    return st;
}

static ms_status finish_sample(ms_bmff_parser *p, uint32_t ti)
{
    struct ms_bmff_demux *d = p->dmx;
    ms_bmff_track *t = &d->tracks[ti];
    int64_t pts;
    ms_status st;
    if (t->codec == MS_CODEC_H264 || t->codec == MS_CODEC_HEVC) {
        st = ms_avcc_validate_sample(d->sbuf, d->slen, t->nal_len_size);
        if (st != MS_OK) {
            fail_set(p, st);
            return st;
        }
    }
    if (t->dts > (uint64_t)INT64_MAX) {
        fail_set(p, MS_ERR_UNSUPPORTED);
        return MS_ERR_UNSUPPORTED;
    }
    pts = (int64_t)t->dts + ctts_offset(t);
    if (pts < 0) {
        fail_set(p, MS_ERR_UNSUPPORTED);
        return MS_ERR_UNSUPPORTED;
    }
    d->pending = 1;
    d->pending_ti = ti;
    d->pending_pts = (uint64_t)pts;
    d->pending_dts = t->dts;
    d->pending_key = (t->codec == MS_CODEC_AAC)
                         ? 1
                         : stss_is_sync(t, t->sample_idx + 1);
    d->pending_fmtchg = t->first_au_pending ? 1 : 0;
    d->pending_disc = t->disc;
    /* emit 成功时 emit_pending 会推进游标；LIMIT 时保留 pending，不推进。 */
    d->harvest_frag = 0;
    return emit_pending(p);
}

static ms_status finish_frag_sample(ms_bmff_parser *p)
{
    struct ms_bmff_demux *d = p->dmx;
    struct ms_bmff_frag_sample *s;
    ms_bmff_track *t;
    int64_t pts;
    ms_status st;
    if (d == NULL || d->fs_i >= d->fs_n) {
        return MS_ERR_STATE;
    }
    s = &d->fs[d->fs_i];
    if (s->ti >= (uint32_t)d->ntracks) {
        fail_set(p, MS_ERR_MALFORMED);
        return MS_ERR_MALFORMED;
    }
    t = &d->tracks[s->ti];
    if (t->codec == MS_CODEC_H264 || t->codec == MS_CODEC_HEVC) {
        st = ms_avcc_validate_sample(d->sbuf, d->slen, t->nal_len_size);
        if (st != MS_OK) {
            fail_set(p, st);
            return st;
        }
    }
    if (s->dts > (uint64_t)INT64_MAX) {
        fail_set(p, MS_ERR_UNSUPPORTED);
        return MS_ERR_UNSUPPORTED;
    }
    pts = (int64_t)s->dts + s->cto;
    if (pts < 0) {
        fail_set(p, MS_ERR_UNSUPPORTED);
        return MS_ERR_UNSUPPORTED;
    }
    d->pending = 1;
    d->pending_ti = s->ti;
    d->pending_pts = (uint64_t)pts;
    d->pending_dts = s->dts;
    d->pending_key = (t->codec == MS_CODEC_AAC)
                         ? 1
                         : ((s->flags & SAMPLE_FLAG_NON_SYNC) ? 0 : 1);
    d->pending_fmtchg = t->first_au_pending ? 1 : 0;
    d->pending_disc = s->disc || t->disc;
    d->harvest_frag = 1;
    return emit_pending(p);
}

static ms_status ensure_sbuf(ms_bmff_parser *p, size_t want)
{
    struct ms_bmff_demux *d = p->dmx;
    if (d->sbuf != NULL && d->scap >= want) {
        return MS_OK;
    }
    {
        uint8_t *q;
        ms_status st = MS_OK;
        q = (uint8_t *)dmx_alloc(p, want, &st);
        if (q == NULL) {
            return st;
        }
        if (d->sbuf != NULL && d->slen > 0) {
            memcpy(q, d->sbuf, d->slen);
            ms_copy_meter_add(p->meter, MS_COPY_ASM, d->slen);
        }
        ms_mem_free(p->alloc, d->sbuf);
        d->sbuf = q;
        d->scap = want;
    }
    return MS_OK;
}

static ms_status harvest_lookback_one(ms_bmff_parser *p)
{
    struct ms_bmff_demux *d = p->dmx;
    uint32_t best_i = 0;
    uint64_t best_off = UINT64_MAX;
    int found = 0;
    int i;
    ms_bmff_track *t;
    uint32_t sz;
    ms_status st;
    if (d == NULL || p->lb_buf == NULL || d->need_seek) {
        return MS_AGAIN;
    }
    st = emit_pending(p);
    if (st != MS_OK) {
        return st;
    }
    if (d->moof_ready && d->fs_i < d->fs_n) {
        struct ms_bmff_frag_sample *s = &d->fs[d->fs_i];
        if (lb_covers(p, s->off, s->size)) {
            if ((size_t)s->size > p->max_assembly_bytes) {
                fail_set(p, MS_ERR_MALFORMED);
                return MS_ERR_MALFORMED;
            }
            st = ensure_sbuf(p, (size_t)s->size);
            if (st != MS_OK) {
                return st;
            }
            memcpy(d->sbuf, p->lb_buf + (size_t)(s->off - p->lb_start),
                   (size_t)s->size);
            ms_copy_meter_add(p->meter, MS_COPY_ASM, (size_t)s->size);
            d->slen = (size_t)s->size;
            d->swant = 0;
            return finish_frag_sample(p);
        }
        /* 样本还在前方：等现场 mdat，保留回看窗。 */
        return MS_AGAIN;
    }
    for (i = 0; i < d->ntracks; i++) {
        t = &d->tracks[i];
        if (!t->usable || !t->cursor_ready || t->sample_idx >= t->sample_count) {
            continue;
        }
        sz = sample_size_of(t, t->sample_idx);
        if (sz == 0) {
            fail_set(p, MS_ERR_MALFORMED);
            return MS_ERR_MALFORMED;
        }
        if (!lb_covers(p, t->next_off, sz)) {
            continue;
        }
        if (!found || t->next_off < best_off) {
            found = 1;
            best_off = t->next_off;
            best_i = (uint32_t)i;
        }
    }
    if (!found) {
        lookback_free(p);
        return MS_AGAIN;
    }
    t = &d->tracks[best_i];
    sz = sample_size_of(t, t->sample_idx);
    if ((size_t)sz > p->max_assembly_bytes) {
        fail_set(p, MS_ERR_MALFORMED);
        return MS_ERR_MALFORMED;
    }
    st = ensure_sbuf(p, (size_t)sz);
    if (st != MS_OK) {
        return st;
    }
    memcpy(d->sbuf, p->lb_buf + (size_t)(t->next_off - p->lb_start), sz);
    ms_copy_meter_add(p->meter, MS_COPY_ASM, (size_t)sz);
    d->slen = (size_t)sz;
    d->swant = 0;
    return finish_sample(p, best_i);
}

ms_status ms_bmff_replay_lookback(ms_bmff_parser *p)
{
    if (p == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    if (p->failed) {
        return p->fail_status;
    }
    if (p->dmx == NULL || !p->dmx->finalized || p->dmx->need_seek ||
        p->lb_buf == NULL) {
        return MS_OK;
    }
    if (is_frag(p) && !(p->dmx->moof_ready && p->dmx->fs_i < p->dmx->fs_n)) {
        return MS_OK;
    }
    if (p->dmx->ann != ANN_DONE) {
        return MS_OK;
    }
    for (;;) {
        ms_status st = harvest_lookback_one(p);
        if (st == MS_AGAIN) {
            return MS_OK;
        }
        if (st != MS_OK) {
            return st;
        }
    }
}

static ms_status feed_frag_mdat(ms_bmff_parser *p, const uint8_t *data,
                                size_t size, size_t *used, int eos)
{
    struct ms_bmff_demux *d = p->dmx;
    size_t pos = 0;
    ms_status st;
    *used = 0;
    st = ms_bmff_announce(p);
    if (st != MS_OK) {
        return st;
    }
    st = emit_pending(p);
    if (st != MS_OK) {
        return st;
    }
    while (pos < size || (eos && pos == size)) {
        struct ms_bmff_frag_sample *s;
        uint32_t sz;
        if (d->skip_rest) {
            size_t take;
            if (d->skip_rest_left == 0) {
                d->skip_rest = 0;
                continue;
            }
            if (pos >= size) {
                break;
            }
            take = size - pos;
            if ((uint64_t)take > d->skip_rest_left) {
                take = (size_t)d->skip_rest_left;
            }
            if (!p->skip_to_eof && (uint64_t)take > p->skip_left) {
                take = (size_t)p->skip_left;
            }
            pos += take;
            p->file_off += take;
            p->bytes_skipped += take;
            d->skip_rest_left -= take;
            if (!p->skip_to_eof) {
                p->skip_left -= take;
            }
            *used = pos;
            if (d->skip_rest_left == 0) {
                d->skip_rest = 0;
            }
            continue;
        }
        if (d->pending) {
            st = emit_pending(p);
            if (st != MS_OK) {
                *used = pos;
                return st;
            }
        }
        if (d->swant > 0 && d->slen < d->swant) {
            size_t need = d->swant - d->slen;
            size_t take;
            if (pos >= size) {
                break;
            }
            take = size - pos;
            if (take > need) {
                take = need;
            }
            if (!p->skip_to_eof && (uint64_t)take > p->skip_left) {
                take = (size_t)p->skip_left;
            }
            memcpy(d->sbuf + d->slen, data + pos, take);
            ms_copy_meter_add(p->meter, MS_COPY_ASM, take);
            d->slen += take;
            pos += take;
            p->file_off += take;
            if (!p->skip_to_eof) {
                p->skip_left -= take;
            }
            *used = pos;
            if (d->slen < d->swant) {
                break;
            }
            st = finish_frag_sample(p);
            d->swant = 0;
            if (st != MS_OK) {
                return st;
            }
            continue;
        }
        if (!d->moof_ready || d->fs_i >= d->fs_n) {
            d->moof_ready = 0;
            *used = pos;
            return MS_OK;
        }
        s = &d->fs[d->fs_i];
        if (p->file_off > s->off) {
            if (lb_covers(p, s->off, s->size)) {
                if ((size_t)s->size > p->max_assembly_bytes) {
                    fail_set(p, MS_ERR_MALFORMED);
                    *used = pos;
                    return MS_ERR_MALFORMED;
                }
                st = ensure_sbuf(p, (size_t)s->size);
                if (st != MS_OK) {
                    *used = pos;
                    return st;
                }
                memcpy(d->sbuf, p->lb_buf + (size_t)(s->off - p->lb_start),
                       (size_t)s->size);
                ms_copy_meter_add(p->meter, MS_COPY_ASM, (size_t)s->size);
                d->slen = (size_t)s->size;
                d->swant = 0;
                st = finish_frag_sample(p);
                if (st != MS_OK) {
                    *used = pos;
                    return st;
                }
                continue;
            }
            fail_set(p, MS_ERR_MALFORMED);
            *used = pos;
            return MS_ERR_MALFORMED;
        }
        if (p->file_off < s->off) {
            uint64_t gap = s->off - p->file_off;
            size_t take;
            if (pos >= size) {
                break;
            }
            take = size - pos;
            if ((uint64_t)take > gap) {
                take = (size_t)gap;
            }
            if (!p->skip_to_eof && (uint64_t)take > p->skip_left) {
                take = (size_t)p->skip_left;
            }
            pos += take;
            p->file_off += take;
            p->bytes_skipped += take;
            if (!p->skip_to_eof) {
                p->skip_left -= take;
            }
            *used = pos;
            continue;
        }
        sz = s->size;
        if (sz == 0 || (size_t)sz > p->max_assembly_bytes) {
            fail_set(p, MS_ERR_MALFORMED);
            *used = pos;
            return MS_ERR_MALFORMED;
        }
        st = ensure_sbuf(p, (size_t)sz);
        if (st != MS_OK) {
            *used = pos;
            return st;
        }
        d->slen = 0;
        d->swant = (size_t)sz;
        d->pending_ti = s->ti;
        d->harvest_frag = 1;
        if (pos == size && !eos) {
            *used = pos;
            return MS_OK;
        }
        if (pos == size && eos) {
            d->swant = 0;
            *used = pos;
            return MS_OK;
        }
    }
    *used = pos;
    if (eos && d->swant > 0 && d->slen < d->swant) {
        d->swant = 0;
        d->slen = 0;
        p->truncated_mdat = 1;
    }
    return MS_OK;
}

ms_status ms_bmff_feed_mdat(ms_bmff_parser *p, const uint8_t *data, size_t size,
                            size_t *used, int eos)
{
    struct ms_bmff_demux *d;
    size_t pos = 0;
    ms_status st;
    *used = 0;
    d = p->dmx;
    if (d != NULL && (d->is_fragmented || d->moof_ready)) {
        return feed_frag_mdat(p, data, size, used, eos);
    }
    st = ms_bmff_announce(p);
    if (st != MS_OK) {
        return st;
    }
    st = emit_pending(p);
    if (st != MS_OK) {
        return st;
    }
    /* 若上次入队 LIMIT，advance 尚未发生：先推进再继续。
     * emit_pending 成功后 pending=0；若之前 LIMIT 则本处成功后仍要 advance。
     * finish_sample 在 LIMIT 时不 advance；因此 emit 成功后若游标仍停在
     * 同一份已发出的样本，需要在 emit_pending 内... 已改为 LIMIT 不 advance，
     * 成功路径 finish_sample 已 advance。这里 emit_pending 若刚成功，是
     * 从 drain 重试进来的，必须 advance。用 pending 从 1→0 来判断。 */
    while (pos < size || (eos && pos == size)) {
        uint32_t ti;
        ms_bmff_track *t;
        uint32_t sz;
        if (d->skip_rest) {
            size_t take;
            if (d->skip_rest_left == 0) {
                d->skip_rest = 0;
                continue;
            }
            if (pos >= size) {
                break;
            }
            take = size - pos;
            if ((uint64_t)take > d->skip_rest_left) {
                take = (size_t)d->skip_rest_left;
            }
            if (!p->skip_to_eof && (uint64_t)take > p->skip_left) {
                take = (size_t)p->skip_left;
            }
            pos += take;
            p->file_off += take;
            p->bytes_skipped += take;
            d->skip_rest_left -= take;
            if (!p->skip_to_eof) {
                p->skip_left -= take;
            }
            *used = pos;
            if (d->skip_rest_left == 0) {
                d->skip_rest = 0;
            }
            continue;
        }
        if (d->pending) {
            st = emit_pending(p);
            if (st != MS_OK) {
                *used = pos;
                return st;
            }
        }
        if (d->swant > 0 && d->slen < d->swant) {
            size_t need = d->swant - d->slen;
            size_t take;
            if (pos >= size) {
                break;
            }
            take = size - pos;
            if (take > need) {
                take = need;
            }
            if (!p->skip_to_eof && (uint64_t)take > p->skip_left) {
                take = (size_t)p->skip_left;
            }
            memcpy(d->sbuf + d->slen, data + pos, take);
            ms_copy_meter_add(p->meter, MS_COPY_ASM, take);
            d->slen += take;
            pos += take;
            p->file_off += take;
            if (!p->skip_to_eof) {
                p->skip_left -= take;
            }
            *used = pos;
            if (d->slen < d->swant) {
                break;
            }
            st = finish_sample(p, d->pending_ti);
            d->swant = 0;
            if (st != MS_OK) {
                return st;
            }
            continue;
        }
        if (!pick_next(d, &ti)) {
            /* 再无样本：剩余 mdat 按普通跳过交给调用方。 */
            *used = pos;
            return MS_OK;
        }
        t = &d->tracks[ti];
        if (p->file_off > t->next_off) {
            fail_set(p, MS_ERR_MALFORMED);
            *used = pos;
            return MS_ERR_MALFORMED;
        }
        if (p->file_off < t->next_off) {
            uint64_t gap = t->next_off - p->file_off;
            size_t take;
            if (pos >= size) {
                break;
            }
            take = size - pos;
            if ((uint64_t)take > gap) {
                take = (size_t)gap;
            }
            if (!p->skip_to_eof && (uint64_t)take > p->skip_left) {
                take = (size_t)p->skip_left;
            }
            pos += take;
            p->file_off += take;
            p->bytes_skipped += take;
            if (!p->skip_to_eof) {
                p->skip_left -= take;
            }
            *used = pos;
            continue;
        }
        /* file_off == next_off：开始收当前样本。 */
        sz = sample_size_of(t, t->sample_idx);
        if (sz == 0) {
            fail_set(p, MS_ERR_MALFORMED);
            *used = pos;
            return MS_ERR_MALFORMED;
        }
        if ((size_t)sz > p->max_assembly_bytes) {
            fail_set(p, MS_ERR_MALFORMED);
            *used = pos;
            return MS_ERR_MALFORMED;
        }
        st = ensure_sbuf(p, (size_t)sz);
        if (st != MS_OK) {
            *used = pos;
            return st;
        }
        d->slen = 0;
        d->swant = (size_t)sz;
        d->pending_ti = ti;
        if (pos == size && !eos) {
            *used = pos;
            return MS_OK;
        }
        if (pos == size && eos) {
            /* 样本一个字节都还没到就 EOS：不发出。 */
            d->swant = 0;
            *used = pos;
            return MS_OK;
        }
    }
    *used = pos;
    if (eos && d->swant > 0 && d->slen < d->swant) {
        /* 截断样本不当完整 AU。 */
        d->swant = 0;
        d->slen = 0;
        p->truncated_mdat = 1;
    }
    return MS_OK;
}

void ms_bmff_flush_partial(ms_bmff_parser *p)
{
    struct ms_bmff_demux *d;
    if (p == NULL || p->dmx == NULL) {
        return;
    }
    d = p->dmx;
    if (d->pending) {
        /* 已收齐待入队：flush 丢掉未取走输出的同时丢掉待入队样本。 */
        d->pending = 0;
        if (d->harvest_frag) {
            d->fs_i++;
            if (d->fs_i >= d->fs_n) {
                d->moof_ready = 0;
                d->harvest_frag = 0;
            }
        } else {
            ms_status st = advance_track(&d->tracks[d->pending_ti]);
            (void)st;
        }
        d->tracks[d->pending_ti].disc = 1;
        d->slen = 0;
        d->swant = 0;
        return;
    }
    if (d->swant > 0 && d->slen > 0 && d->slen < d->swant) {
        d->skip_rest = 1;
        d->skip_rest_left = (uint64_t)(d->swant - d->slen);
        d->tracks[d->pending_ti].disc = 1;
        if (d->harvest_frag) {
            d->fs_i++;
            if (d->fs_i >= d->fs_n) {
                d->moof_ready = 0;
                d->harvest_frag = 0;
            }
        } else {
            ms_status st = advance_track(&d->tracks[d->pending_ti]);
            (void)st;
        }
        d->slen = 0;
        d->swant = 0;
    }
}

ms_status ms_bmff_parser_drain(ms_bmff_parser *p)
{
    ms_status st;
    if (p == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    if (p->failed) {
        return p->fail_status;
    }
    st = ms_bmff_announce(p);
    if (st != MS_OK) {
        return st;
    }
    if (p->dmx != NULL && p->dmx->pending) {
        st = emit_pending(p);
        if (st != MS_OK) {
            return st;
        }
    }
    return ms_bmff_replay_lookback(p);
}


void ms_bmff_lookback_drop(ms_bmff_parser *p)
{
    lookback_free(p);
}

uint64_t ms_bmff_parser_file_off(const ms_bmff_parser *p)
{
    return (p != NULL) ? p->file_off : 0;
}

static int sample_times(const ms_bmff_track *t, uint32_t idx,
                        uint64_t *dts_out, int64_t *pts_out)
{
    uint64_t dts = 0;
    uint32_t n = 0;
    uint32_t i;
    uint32_t left;
    uint32_t ci = 0;
    uint32_t cleft = (t->ctts_n > 0) ? t->ctts[0].count : 0;
    int32_t cto = 0;

    if (idx >= t->sample_count) {
        return -1;
    }
    for (i = 0; i < t->stts_n; i++) {
        for (left = t->stts[i].count; left > 0; left--) {
            if (n == idx) {
                if (t->has_ctts) {
                    if (ci >= t->ctts_n) {
                        return -1;
                    }
                    cto = t->ctts[ci].offset;
                }
                *dts_out = dts;
                *pts_out = (int64_t)dts + (int64_t)cto;
                return 0;
            }
            if (u64_add(dts, t->stts[i].delta, &dts) != 0) {
                return -1;
            }
            n++;
            if (t->has_ctts) {
                if (ci >= t->ctts_n) {
                    return -1;
                }
                if (cleft == 0) {
                    return -1;
                }
                cleft--;
                if (cleft == 0) {
                    ci++;
                    if (ci < t->ctts_n) {
                        cleft = t->ctts[ci].count;
                    }
                }
            }
        }
    }
    return -1;
}

static ms_status cursor_to_index(ms_bmff_track *t, uint32_t idx)
{
    ms_status st;
    if (idx > t->sample_count) {
        return MS_ERR_INVALID_ARG;
    }
    st = init_cursor(t);
    if (st != MS_OK) {
        return st;
    }
    if (t->sample_count == 0) {
        return MS_OK;
    }
    while (t->sample_idx < idx) {
        st = advance_track(t);
        if (st != MS_OK) {
            return st;
        }
    }
    t->first_au_pending = 1;
    return MS_OK;
}

static int to_90k(uint32_t timescale, uint64_t media, int64_t *out)
{
    uint64_t v;
    if (timescale == 0) {
        return -1;
    }
    if (ms_u64_scale(media, 90000ull, (uint64_t)timescale, &v) != 0) {
        return -1;
    }
    if (v > (uint64_t)INT64_MAX) {
        return -1;
    }
    *out = (int64_t)v;
    return 0;
}

int ms_bmff_parser_time_seekable(const ms_bmff_parser *p)
{
    if (p == NULL || p->dmx == NULL || p->failed) {
        return 0;
    }
    if (!p->dmx->finalized || p->dmx->is_fragmented) {
        return 0;
    }
    if (p->format != MS_FORMAT_MP4 && p->format != MS_FORMAT_MOV) {
        return 0;
    }
    return 1;
}

int ms_bmff_fill_sample_need(const ms_bmff_parser *p, ms_bmff_need *out)
{
    struct ms_bmff_demux *d;
    uint32_t ti;
    ms_bmff_track *t;
    uint32_t sz;
    uint64_t off;

    if (p == NULL || out == NULL || p->dmx == NULL) {
        return 0;
    }
    d = p->dmx;
    if (!d->finalized || d->is_fragmented || d->pending) {
        return 0;
    }
    if (!p->random_ok) {
        return 0;
    }
    if (d->need_seek) {
        return 0;
    }
    if (d->swant > 0 && d->slen < d->swant) {
        t = &d->tracks[d->pending_ti];
        off = t->next_off;
        if (u64_add(off, (uint64_t)d->slen, &off) != 0) {
            return 0;
        }
        out->kind = MS_BMFF_NEED_SAMPLE;
        out->offset = off;
        out->length = d->swant - d->slen;
        return 1;
    }
    if (!pick_next(d, &ti)) {
        return 0;
    }
    t = &d->tracks[ti];
    sz = sample_size_of(t, t->sample_idx);
    if (sz == 0) {
        return 0;
    }
    out->kind = MS_BMFF_NEED_SAMPLE;
    out->offset = t->next_off;
    out->length = (size_t)sz;
    return 1;
}

ms_status ms_bmff_supply_sample(ms_bmff_parser *p, uint64_t off,
                                const uint8_t *data, size_t n, size_t *used)
{
    struct ms_bmff_demux *d;
    ms_bmff_track *t;
    uint32_t ti;
    uint32_t sz;
    size_t need;
    size_t take;
    ms_status st;
    uint64_t expect;

    if (used != NULL) {
        *used = 0;
    }
    if (p == NULL || used == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    if (n > 0 && data == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    if (p->failed) {
        return p->fail_status;
    }
    d = p->dmx;
    if (d == NULL) {
        return MS_ERR_STATE;
    }
    if (d->pending) {
        return emit_pending(p);
    }
    st = ms_bmff_announce(p);
    if (st != MS_OK) {
        return st;
    }
    if (d->swant == 0 || d->slen >= d->swant) {
        if (!pick_next(d, &ti)) {
            return MS_AGAIN;
        }
        t = &d->tracks[ti];
        if (t->next_off != off) {
            return MS_ERR_STATE;
        }
        sz = sample_size_of(t, t->sample_idx);
        if (sz == 0 || (size_t)sz > p->max_assembly_bytes) {
            fail_set(p, MS_ERR_MALFORMED);
            return MS_ERR_MALFORMED;
        }
        st = ensure_sbuf(p, (size_t)sz);
        if (st != MS_OK) {
            return st;
        }
        d->swant = (size_t)sz;
        d->slen = 0;
        d->pending_ti = ti;
        d->harvest_frag = 0;
    }
    t = &d->tracks[d->pending_ti];
    expect = t->next_off;
    if (u64_add(expect, (uint64_t)d->slen, &expect) != 0 || expect != off) {
        return MS_ERR_STATE;
    }
    need = d->swant - d->slen;
    take = (n < need) ? n : need;
    if (take > 0) {
        memcpy(d->sbuf + d->slen, data, take);
        ms_copy_meter_add(p->meter, MS_COPY_ASM, take);
        d->slen += take;
    }
    *used = take;
    if (d->slen < d->swant) {
        return MS_OK;
    }
    d->swant = 0;
    return finish_sample(p, d->pending_ti);
}

static void drop_pending_keep_cursor(struct ms_bmff_demux *d)
{
    d->pending = 0;
    d->slen = 0;
    d->swant = 0;
    d->skip_rest = 0;
    d->skip_rest_left = 0;
    d->harvest_frag = 0;
}

ms_status ms_bmff_parser_seek_time(ms_bmff_parser *p, int64_t time_90k,
                                   int64_t *land_90k)
{
    struct ms_bmff_demux *d;
    ms_bmff_track saved[MS_BMFF_MAX_TRACKS];
    uint32_t start_idx[MS_BMFF_MAX_TRACKS];
    int i;
    int vi = -1;
    int ai = -1;
    int64_t land = 0;
    uint64_t tgt;
    ms_status st;
    int saved_pending;
    int saved_ann;
    size_t saved_slen;
    size_t saved_swant;

    if (p == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    if (time_90k < 0) {
        return MS_ERR_INVALID_ARG;
    }
    if (!ms_bmff_parser_time_seekable(p)) {
        if (p->dmx == NULL || !p->dmx->finalized) {
            return MS_AGAIN;
        }
        return MS_ERR_NOT_SEEKABLE;
    }
    d = p->dmx;
    for (i = 0; i < d->ntracks; i++) {
        start_idx[i] = d->tracks[i].sample_count;
        if (!d->tracks[i].usable || !d->tracks[i].cursor_ready) {
            continue;
        }
        if (vi < 0 && d->tracks[i].kind == MS_TRACK_VIDEO) {
            vi = i;
        } else if (ai < 0 && d->tracks[i].kind == MS_TRACK_AUDIO) {
            ai = i;
        }
    }
    if (vi < 0 && ai < 0) {
        return MS_ERR_NOT_SEEKABLE;
    }
    if (vi >= 0) {
        ms_bmff_track *t = &d->tracks[vi];
        uint32_t best = UINT32_MAX;
        uint32_t first_sync = UINT32_MAX;
        uint32_t si;
        if (t->has_stss && t->stss_n == 0) {
            return MS_ERR_NOT_SEEKABLE;
        }
        if (t->timescale == 0) {
            return MS_ERR_UNSUPPORTED;
        }
        if (ms_u64_scale((uint64_t)time_90k, (uint64_t)t->timescale, 90000ull,
                         &tgt) != 0) {
            /* 请求时间过大：落到最后一个同步样本。 */
            tgt = UINT64_MAX;
        }
        for (si = 0; si < t->sample_count; si++) {
            uint64_t dts = 0;
            int64_t pts = 0;
            if (!stss_is_sync(t, si + 1)) {
                continue;
            }
            if (sample_times(t, si, &dts, &pts) != 0) {
                return MS_ERR_MALFORMED;
            }
            if (pts < 0) {
                continue;
            }
            if (first_sync == UINT32_MAX) {
                first_sync = si;
            }
            if ((uint64_t)pts <= tgt) {
                best = si;
            }
        }
        if (best == UINT32_MAX) {
            if (first_sync == UINT32_MAX) {
                return MS_ERR_NOT_SEEKABLE;
            }
            best = first_sync;
        }
        {
            uint64_t dts = 0;
            int64_t pts = 0;
            if (sample_times(t, best, &dts, &pts) != 0 || pts < 0) {
                return MS_ERR_MALFORMED;
            }
            if (to_90k(t->timescale, (uint64_t)pts, &land) != 0) {
                return MS_ERR_OVERFLOW;
            }
        }
        start_idx[vi] = best;
        if (ai >= 0) {
            ms_bmff_track *at = &d->tracks[ai];
            uint32_t aj;
            uint32_t chosen = at->sample_count;
            for (aj = 0; aj < at->sample_count; aj++) {
                uint64_t dts = 0;
                int64_t pts = 0;
                int64_t p90;
                if (sample_times(at, aj, &dts, &pts) != 0) {
                    return MS_ERR_MALFORMED;
                }
                if (pts < 0) {
                    continue;
                }
                if (to_90k(at->timescale, (uint64_t)pts, &p90) != 0) {
                    return MS_ERR_OVERFLOW;
                }
                /* 音频从请求时间起出；请求之前的裁剪由控制层做。 */
                if (p90 >= time_90k) {
                    chosen = aj;
                    break;
                }
            }
            start_idx[ai] = chosen;
        }
    } else {
        ms_bmff_track *at = &d->tracks[ai];
        uint32_t si;
        uint32_t best = UINT32_MAX;
        if (at->timescale == 0) {
            return MS_ERR_UNSUPPORTED;
        }
        if (ms_u64_scale((uint64_t)time_90k, (uint64_t)at->timescale, 90000ull,
                         &tgt) != 0) {
            tgt = UINT64_MAX;
        }
        for (si = 0; si < at->sample_count; si++) {
            uint64_t dts = 0;
            int64_t pts = 0;
            if (sample_times(at, si, &dts, &pts) != 0) {
                return MS_ERR_MALFORMED;
            }
            if (pts < 0) {
                continue;
            }
            if ((uint64_t)pts <= tgt) {
                best = si;
            } else {
                break;
            }
        }
        if (best == UINT32_MAX) {
            if (at->sample_count == 0) {
                return MS_ERR_NOT_SEEKABLE;
            }
            best = 0;
        }
        {
            uint64_t dts = 0;
            int64_t pts = 0;
            if (sample_times(at, best, &dts, &pts) != 0 || pts < 0) {
                return MS_ERR_MALFORMED;
            }
            if (to_90k(at->timescale, (uint64_t)pts, &land) != 0) {
                return MS_ERR_OVERFLOW;
            }
        }
        start_idx[ai] = best;
    }

    /* 计算完成后再改游标；失败则回滚。 */
    memcpy(saved, d->tracks, sizeof(saved));
    saved_pending = d->pending;
    saved_ann = d->ann;
    saved_slen = d->slen;
    saved_swant = d->swant;
    drop_pending_keep_cursor(d);
    for (i = 0; i < d->ntracks; i++) {
        if (!d->tracks[i].usable) {
            continue;
        }
        st = cursor_to_index(&d->tracks[i], start_idx[i]);
        if (st != MS_OK) {
            memcpy(d->tracks, saved, sizeof(saved));
            d->pending = saved_pending;
            d->ann = saved_ann;
            d->slen = saved_slen;
            d->swant = saved_swant;
            return st;
        }
    }
    d->ann = ANN_CONFIG;
    for (i = 0; i < d->ntracks; i++) {
        d->tracks[i].config_emitted = 0;
        if (d->tracks[i].usable && d->tracks[i].sample_idx < d->tracks[i].sample_count) {
            d->tracks[i].first_au_pending = 1;
        }
    }
    p->have_seek_land = 1;
    p->seek_req_90k = time_90k;
    p->seek_land_90k = land;
    if (land_90k != NULL) {
        *land_90k = land;
    }
    return MS_OK;
}
