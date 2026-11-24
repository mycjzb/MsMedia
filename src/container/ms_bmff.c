/*
 * ISO BMFF / QuickTime 有界箱体遍历。
 * by: Long <mycjzb@gmail.com>
 */

#include "ms_bmff.h"
#include "ms_bmff_sample.h"

enum {
    ST_HEADER = 0,
    ST_SKIP,
    ST_BRANDS,
    ST_CAPTURE
};

#define T_FTYP MS_BMFF_TYPE('f', 't', 'y', 'p')
#define T_STYP MS_BMFF_TYPE('s', 't', 'y', 'p')
#define T_MOOV MS_BMFF_TYPE('m', 'o', 'o', 'v')
#define T_TRAK MS_BMFF_TYPE('t', 'r', 'a', 'k')
#define T_EDTS MS_BMFF_TYPE('e', 'd', 't', 's')
#define T_MDIA MS_BMFF_TYPE('m', 'd', 'i', 'a')
#define T_MINF MS_BMFF_TYPE('m', 'i', 'n', 'f')
#define T_DINF MS_BMFF_TYPE('d', 'i', 'n', 'f')
#define T_STBL MS_BMFF_TYPE('s', 't', 'b', 'l')
#define T_MVEX MS_BMFF_TYPE('m', 'v', 'e', 'x')
#define T_MOOF MS_BMFF_TYPE('m', 'o', 'o', 'f')
#define T_TRAF MS_BMFF_TYPE('t', 'r', 'a', 'f')
#define T_MFRA MS_BMFF_TYPE('m', 'f', 'r', 'a')
#define T_MDAT MS_BMFF_TYPE('m', 'd', 'a', 't')
#define T_FREE MS_BMFF_TYPE('f', 'r', 'e', 'e')
#define T_SKIP MS_BMFF_TYPE('s', 'k', 'i', 'p')
#define T_WIDE MS_BMFF_TYPE('w', 'i', 'd', 'e')
#define T_PNOT MS_BMFF_TYPE('p', 'n', 'o', 't')
#define T_PICT MS_BMFF_TYPE('P', 'I', 'C', 'T')
#define T_UUID MS_BMFF_TYPE('u', 'u', 'i', 'd')
#define T_SIDX MS_BMFF_TYPE('s', 'i', 'd', 'x')
#define T_SSIX MS_BMFF_TYPE('s', 's', 'i', 'x')
#define T_PRFT MS_BMFF_TYPE('p', 'r', 'f', 't')
#define T_PDIN MS_BMFF_TYPE('p', 'd', 'i', 'n')
#define T_MECO MS_BMFF_TYPE('m', 'e', 'c', 'o')
#define T_META MS_BMFF_TYPE('m', 'e', 't', 'a')
#define T_JUNK MS_BMFF_TYPE('j', 'u', 'n', 'k')
#define T_IDAT MS_BMFF_TYPE('i', 'd', 'a', 't')
#define T_UDTA MS_BMFF_TYPE('u', 'd', 't', 'a')

#define T_STSD MS_BMFF_TYPE('s', 't', 's', 'd')
#define T_STTS MS_BMFF_TYPE('s', 't', 't', 's')
#define T_STSC MS_BMFF_TYPE('s', 't', 's', 'c')
#define T_STSZ MS_BMFF_TYPE('s', 't', 's', 'z')
#define T_STZ2 MS_BMFF_TYPE('s', 't', 'z', '2')
#define T_STCO MS_BMFF_TYPE('s', 't', 'c', 'o')
#define T_CO64 MS_BMFF_TYPE('c', 'o', '6', '4')
#define T_STSS MS_BMFF_TYPE('s', 't', 's', 's')
#define T_CTTS MS_BMFF_TYPE('c', 't', 't', 's')
#define T_STSH MS_BMFF_TYPE('s', 't', 's', 'h')
#define T_SDTP MS_BMFF_TYPE('s', 'd', 't', 'p')

#define T_MFHD MS_BMFF_TYPE('m', 'f', 'h', 'd')
#define T_TFHD MS_BMFF_TYPE('t', 'f', 'h', 'd')
#define T_TRUN MS_BMFF_TYPE('t', 'r', 'u', 'n')
#define T_TFDT MS_BMFF_TYPE('t', 'f', 'd', 't')
#define T_TREX MS_BMFF_TYPE('t', 'r', 'e', 'x')
#define T_TFRA MS_BMFF_TYPE('t', 'f', 'r', 'a')
#define T_MFRO MS_BMFF_TYPE('m', 'f', 'r', 'o')

#define T_QT   MS_BMFF_TYPE('q', 't', ' ', ' ')
#define T_ISOM MS_BMFF_TYPE('i', 's', 'o', 'm')
#define T_ISO2 MS_BMFF_TYPE('i', 's', 'o', '2')
#define T_ISO5 MS_BMFF_TYPE('i', 's', 'o', '5')
#define T_ISO6 MS_BMFF_TYPE('i', 's', 'o', '6')
#define T_ISO9 MS_BMFF_TYPE('i', 's', 'o', '9')
#define T_MP41 MS_BMFF_TYPE('m', 'p', '4', '1')
#define T_MP42 MS_BMFF_TYPE('m', 'p', '4', '2')
#define T_DASH MS_BMFF_TYPE('d', 'a', 's', 'h')
#define T_CMFC MS_BMFF_TYPE('c', 'm', 'f', 'c')
#define T_CMFS MS_BMFF_TYPE('c', 'm', 'f', 's')
#define T_MSDH MS_BMFF_TYPE('m', 's', 'd', 'h')
#define T_MSIX MS_BMFF_TYPE('m', 's', 'i', 'x')
#define T_AVC1 MS_BMFF_TYPE('a', 'v', 'c', '1')
#define T_HEIC MS_BMFF_TYPE('h', 'e', 'i', 'c')
#define T_HEIX MS_BMFF_TYPE('h', 'e', 'i', 'x')
#define T_AVIF MS_BMFF_TYPE('a', 'v', 'i', 'f')
#define T_AVIS MS_BMFF_TYPE('a', 'v', 'i', 's')
#define T_MIF1 MS_BMFF_TYPE('m', 'i', 'f', '1')
#define T_MSF1 MS_BMFF_TYPE('m', 's', 'f', '1')
#define T_MIAF MS_BMFF_TYPE('m', 'i', 'a', 'f')

static uint32_t rd_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint64_t rd_be64(const uint8_t *p)
{
    return ((uint64_t)rd_be32(p) << 32) | (uint64_t)rd_be32(p + 4);
}

static int type_chars_ok(uint32_t type)
{
    int i;
    for (i = 0; i < 4; i++) {
        unsigned c = (type >> (24 - 8 * i)) & 0xFFu;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == ' ')) {
            return 0;
        }
    }
    return 1;
}

int ms_bmff_is_container_type(uint32_t type)
{
    return type == T_MOOV || type == T_TRAK || type == T_EDTS ||
           type == T_MDIA || type == T_MINF || type == T_DINF ||
           type == T_STBL || type == T_MVEX || type == T_MOOF ||
           type == T_TRAF || type == T_MFRA || type == T_UDTA;
}

int ms_bmff_is_sample_table_type(uint32_t type)
{
    return type == T_STSD || type == T_STTS || type == T_STSC ||
           type == T_STSZ || type == T_STZ2 || type == T_STCO ||
           type == T_CO64 || type == T_STSS || type == T_CTTS ||
           type == T_STSH || type == T_SDTP;
}

int ms_bmff_is_fragment_type(uint32_t type)
{
    return type == T_MOOF || type == T_TRAF || type == T_MFHD ||
           type == T_TFHD || type == T_TRUN || type == T_TFDT ||
           type == T_TREX || type == T_STYP || type == T_SIDX ||
           type == T_TFRA || type == T_MFRO || type == T_MFRA;
}

static int is_top_level_type(uint32_t type)
{
    return type == T_FTYP || type == T_STYP || type == T_MOOV ||
           type == T_MDAT || type == T_FREE || type == T_SKIP ||
           type == T_WIDE || type == T_PNOT || type == T_PICT ||
           type == T_MOOF || type == T_SIDX || type == T_SSIX ||
           type == T_PRFT || type == T_MFRA || type == T_META ||
           type == T_MECO || type == T_UUID || type == T_PDIN ||
           type == T_JUNK || type == T_IDAT || type == T_UDTA;
}

static int is_still_image_brand(uint32_t b)
{
    return b == T_HEIC || b == T_HEIX || b == T_AVIF || b == T_AVIS ||
           b == T_MIF1 || b == T_MSF1 || b == T_MIAF;
}

static int is_frag_brand(uint32_t b)
{
    return b == T_ISO5 || b == T_ISO6 || b == T_ISO9 || b == T_DASH ||
           b == T_CMFC || b == T_CMFS || b == T_MSDH || b == T_MSIX;
}

void ms_bmff_type_str(uint32_t type, char out[5])
{
    int i;
    if (out == NULL) {
        return;
    }
    for (i = 0; i < 4; i++) {
        unsigned c = (type >> (24 - 8 * i)) & 0xFFu;
        out[i] = (c >= 0x20 && c <= 0x7E) ? (char)c : '.';
    }
    out[4] = 0;
}

typedef struct box_hdr {
    uint32_t type;
    uint8_t usertype[16];
    int has_uuid;
    uint64_t size; /* 0 = until EOF */
    uint32_t header_size;
} box_hdr;

/* 解析已凑齐的头。不足时 *need 为还要的总头长，返回 AGAIN。 */
static ms_status parse_box_header(const uint8_t *p, size_t n, size_t *need,
                                  box_hdr *h)
{
    uint32_t sz32;
    uint32_t typ;
    uint32_t hs = 8;
    uint64_t tot;

    memset(h, 0, sizeof(*h));
    if (n < 8) {
        *need = 8;
        return MS_AGAIN;
    }
    sz32 = rd_be32(p);
    typ = rd_be32(p + 4);
    /* 确认后的行走允许 QuickTime ©xyz 等非字母数字类型，按声明大小跳过。
     * 探测阶段仍要求已知顶层类型（见 ms_bmff_probe）。 */
    if (sz32 == 1u) {
        if (n < 16) {
            *need = 16;
            return MS_AGAIN;
        }
        tot = rd_be64(p + 8);
        hs = 16;
        if (tot < 16) {
            return MS_ERR_MALFORMED;
        }
    } else if (sz32 == 0u) {
        tot = 0;
        hs = 8;
    } else if (sz32 < 8u) {
        return MS_ERR_MALFORMED;
    } else {
        tot = sz32;
    }
    if (typ == T_UUID) {
        if (n < (size_t)hs + 16u) {
            *need = (size_t)hs + 16u;
            return MS_AGAIN;
        }
        if (tot != 0 && tot < (uint64_t)hs + 16u) {
            return MS_ERR_MALFORMED;
        }
        memcpy(h->usertype, p + hs, 16);
        h->has_uuid = 1;
        hs += 16;
    }
    if (tot != 0 && tot < hs) {
        return MS_ERR_MALFORMED;
    }
    h->type = typ;
    h->size = tot;
    h->header_size = hs;
    *need = hs;
    return MS_OK;
}

int ms_bmff_probe(const uint8_t *data, size_t size)
{
    box_hdr h;
    size_t need = 8;
    ms_status st;

    if (data == NULL) {
        return 0;
    }
    if (size < 8) {
        return -1;
    }
    if (rd_be32(data) == 1u && size < 16) {
        if (!type_chars_ok(rd_be32(data + 4)) ||
            !is_top_level_type(rd_be32(data + 4))) {
            return 0;
        }
        return -1;
    }
    st = parse_box_header(data, size, &need, &h);
    if (st == MS_AGAIN) {
        if (type_chars_ok(rd_be32(data + 4)) &&
            is_top_level_type(rd_be32(data + 4))) {
            return -1;
        }
        return 0;
    }
    if (st != MS_OK) {
        return 0;
    }
    if (!is_top_level_type(h.type)) {
        return 0;
    }
    return 1;
}

static void clear_walk(ms_bmff_parser *p)
{
    p->confirmed = 0;
    p->format_emitted = 0;
    p->failed = 0;
    p->finished = 0;
    p->fail_status = MS_OK;
    p->format = MS_FORMAT_UNKNOWN;
    p->major_brand = 0;
    p->minor_version = 0;
    p->nbrands = 0;
    p->saw_ftyp = 0;
    p->saw_styp = 0;
    p->saw_moov = 0;
    p->saw_mdat = 0;
    p->saw_moof = 0;
    p->saw_mvex = 0;
    p->still_image = 0;
    p->truncated_mdat = 0;
    p->file_off = 0;
    p->bytes_skipped = 0;
    p->nboxes = 0;
    p->max_depth_seen = 0;
    p->sp = 0;
    p->state = ST_HEADER;
    p->hdr_need = 8;
    p->hdr_got = 0;
    p->skip_left = 0;
    p->skip_to_eof = 0;
    p->skip_is_mdat = 0;
    p->body_left = 0;
    p->body_kind = 0;
    p->body_is_styp = 0;
    p->brand_phase = 0;
    p->brand_tmp_got = 0;
}

void ms_bmff_parser_init(ms_bmff_parser *p,
                         const ms_allocator *alloc,
                         const ms_bmff_callbacks *cb)
{
    if (p == NULL) {
        return;
    }
    memset(p, 0, sizeof(*p));
    p->alloc = alloc;
    if (cb != NULL) {
        p->cb = *cb;
    }
    p->max_assembly_bytes = MS_DEFAULT_MAX_ASSEMBLY_BYTES;
    p->max_lookback_bytes = MS_DEFAULT_MAX_LOOKBACK_BYTES;
    clear_walk(p);
}

void ms_bmff_parser_set_meter(ms_bmff_parser *p, ms_copy_meter *m)
{
    if (p != NULL) {
        p->meter = m;
    }
}

void ms_bmff_parser_reset(ms_bmff_parser *p)
{
    const ms_allocator *a;
    ms_bmff_callbacks cb;
    size_t max_as;
    size_t max_lb;
    if (p == NULL) {
        return;
    }
    a = p->alloc;
    cb = p->cb;
    max_as = p->max_assembly_bytes;
    max_lb = p->max_lookback_bytes;
    {
        ms_copy_meter *meter = p->meter;
        ms_bmff_demux_free(p);
        memset(p, 0, sizeof(*p));
        p->alloc = a;
        p->cb = cb;
        p->max_assembly_bytes = (max_as != 0) ? max_as : MS_DEFAULT_MAX_ASSEMBLY_BYTES;
        p->max_lookback_bytes = (max_lb != 0) ? max_lb : MS_DEFAULT_MAX_LOOKBACK_BYTES;
        p->meter = meter;
        clear_walk(p);
    }
}

void ms_bmff_parser_destroy(ms_bmff_parser *p)
{
    if (p == NULL) {
        return;
    }
    ms_bmff_demux_free(p);
    memset(p, 0, sizeof(*p));
}

static void fail_p(ms_bmff_parser *p, ms_status st)
{
    p->failed = 1;
    p->fail_status = st;
}

static uint32_t parent_type_of(const ms_bmff_parser *p)
{
    if (p->sp <= 0) {
        return 0;
    }
    return p->stack[p->sp - 1].type;
}

static int depth_of(const ms_bmff_parser *p)
{
    return p->sp;
}

static ms_status close_finished_parents(ms_bmff_parser *p)
{
    while (p->sp > 0) {
        uint64_t end = p->stack[p->sp - 1].end;
        if (end == UINT64_MAX) {
            break;
        }
        if (p->file_off < end) {
            break;
        }
        if (p->file_off > end) {
            fail_p(p, MS_ERR_MALFORMED);
            return MS_ERR_MALFORMED;
        }
        {
            uint32_t typ = p->stack[p->sp - 1].type;
            ms_status st;
            p->sp--;
            st = ms_bmff_on_container_leave(p, typ);
            if (st != MS_OK) {
                if (st != MS_LIMIT) {
                    fail_p(p, st);
                }
                return st;
            }
        }
    }
    return MS_OK;
}

static ms_format classify(const ms_bmff_parser *p)
{
    if (p->still_image && !p->saw_moov && !p->saw_moof) {
        return MS_FORMAT_UNKNOWN;
    }
    if (p->saw_moof || p->saw_styp || p->saw_mvex) {
        return MS_FORMAT_FMP4;
    }
    if (p->major_brand == T_QT) {
        return MS_FORMAT_MOV;
    }
    {
        int i;
        for (i = 0; i < p->nbrands; i++) {
            if (p->brands[i] == T_QT) {
                return MS_FORMAT_MOV;
            }
        }
    }
    if (p->saw_ftyp) {
        return MS_FORMAT_MP4;
    }
    if (p->saw_moov || p->saw_mdat) {
        return MS_FORMAT_MOV;
    }
    return MS_FORMAT_UNKNOWN;
}

static int stack_has(const ms_bmff_parser *p, uint32_t type)
{
    int i;
    for (i = 0; i < p->sp; i++) {
        if (p->stack[i].type == type) {
            return 1;
        }
    }
    return 0;
}

static int ready_to_emit(const ms_bmff_parser *p)
{
    if (classify(p) == MS_FORMAT_UNKNOWN) {
        return 0;
    }
    /* moof/styp/mvex 一旦出现即可定为 fMP4。moov 要等该箱走完，
     * 以免把随后的 mvex 漏掉、过早报成普通 MP4。 */
    if (p->saw_moof || p->saw_styp || p->saw_mvex) {
        return 1;
    }
    if (p->saw_moov && !stack_has(p, T_MOOV)) {
        return 1;
    }
    return 0;
}

static ms_status emit_format_if_ready(ms_bmff_parser *p, int force)
{
    ms_format fmt;
    if (p->format_emitted) {
        return MS_OK;
    }
    fmt = classify(p);
    if (fmt == MS_FORMAT_UNKNOWN) {
        if (force && p->still_image) {
            fail_p(p, MS_ERR_UNSUPPORTED);
            return MS_ERR_UNSUPPORTED;
        }
        return MS_OK;
    }
    if (!force && !ready_to_emit(p)) {
        return MS_OK;
    }
    p->format = fmt;
    p->confirmed = 1;
    p->format_emitted = 1;
    if (p->cb.on_format != NULL) {
        ms_status st = p->cb.on_format(p->cb.ctx, fmt, p->major_brand,
                                       p->minor_version);
        if (st != MS_OK) {
            return st;
        }
    }
    return ms_bmff_announce(p);
}

static ms_status add_brand(ms_bmff_parser *p, uint32_t b)
{
    if (is_still_image_brand(b)) {
        p->still_image = 1;
    }
    if (is_frag_brand(b) && !p->saw_ftyp /* not used */) {
        (void)0;
    }
    if (is_frag_brand(b)) {
        /* 品牌只作辅助；fMP4 仍优先看 moof/mvex/styp。记录即可。 */
    }
    if (p->nbrands < MS_BMFF_MAX_BRANDS) {
        p->brands[p->nbrands++] = b;
    }
    return MS_OK;
}

static ms_status begin_skip(ms_bmff_parser *p, uint64_t payload, int until_eof,
                            int is_mdat)
{
    p->state = ST_SKIP;
    p->skip_to_eof = until_eof;
    p->skip_is_mdat = is_mdat;
    p->skip_left = until_eof ? 0 : payload;
    return MS_OK;
}

static ms_status finish_skip_if_done(ms_bmff_parser *p)
{
    if (p->state != ST_SKIP) {
        return MS_OK;
    }
    if (p->skip_to_eof) {
        return MS_OK;
    }
    if (p->skip_left != 0) {
        return MS_OK;
    }
    p->state = ST_HEADER;
    p->hdr_got = 0;
    p->hdr_need = 8;
    return close_finished_parents(p);
}

static ms_status enter_container(ms_bmff_parser *p, uint32_t type, uint64_t end)
{
    if (p->sp >= MS_BMFF_MAX_DEPTH) {
        fail_p(p, MS_ERR_OVERFLOW);
        return MS_ERR_OVERFLOW;
    }
    p->stack[p->sp].type = type;
    p->stack[p->sp].end = end;
    p->sp++;
    if (p->sp > p->max_depth_seen) {
        p->max_depth_seen = p->sp;
    }
    p->state = ST_HEADER;
    p->hdr_got = 0;
    p->hdr_need = 8;
    return ms_bmff_on_container_enter(p, type);
}

static ms_status start_brands(ms_bmff_parser *p, uint64_t payload, int is_styp)
{
    p->state = ST_BRANDS;
    p->body_kind = 1;
    p->body_is_styp = is_styp;
    p->body_left = payload;
    p->brand_phase = 0;
    p->brand_tmp_got = 0;
    if (is_styp) {
        p->saw_styp = 1;
    } else {
        p->saw_ftyp = 1;
    }
    return MS_OK;
}


static ms_status begin_capture(ms_bmff_parser *p, uint32_t type, uint64_t payload)
{
    size_t need;
    if (payload > p->max_assembly_bytes || payload > (uint64_t)SIZE_MAX) {
        fail_p(p, MS_ERR_MALFORMED);
        return MS_ERR_MALFORMED;
    }
    need = (size_t)payload;
    if (need > p->cap_cap) {
        uint8_t *nbuf = (uint8_t *)ms_mem_alloc(p->alloc, need ? need : 1);
        if (nbuf == NULL) {
            ms_status st = ms_acct_is_budget_limit(p->alloc) ? MS_LIMIT
                                                             : MS_ERR_NOMEM;
            if (st != MS_LIMIT) {
                fail_p(p, st);
            }
            return st;
        }
        ms_mem_free(p->alloc, p->cap_buf);
        p->cap_buf = nbuf;
        p->cap_cap = need ? need : 1;
    }
    p->state = ST_CAPTURE;
    p->cap_type = type;
    p->cap_left = payload;
    p->cap_len = 0;
    if (payload == 0) {
        ms_status st = ms_bmff_on_captured(p, type, p->cap_buf, 0);
        p->state = ST_HEADER;
        p->hdr_got = 0;
        p->hdr_need = 8;
        if (st != MS_OK) {
            return st;
        }
        return close_finished_parents(p);
    }
    return MS_OK;
}

static ms_status feed_capture(ms_bmff_parser *p, const uint8_t *data, size_t size,
                              size_t *used, int eos)
{
    size_t take;
    *used = 0;
    if (p->cap_left == 0) {
        p->state = ST_HEADER;
        p->hdr_got = 0;
        p->hdr_need = 8;
        return close_finished_parents(p);
    }
    take = size;
    if ((uint64_t)take > p->cap_left) {
        take = (size_t)p->cap_left;
    }
    if (take > 0) {
        memcpy(p->cap_buf + p->cap_len, data, take);
        p->cap_len += take;
        p->cap_left -= take;
        p->file_off += take;
        *used = take;
    }
    if (p->cap_left == 0) {
        ms_status st = ms_bmff_on_captured(p, p->cap_type, p->cap_buf,
                                           p->cap_len);
        p->state = ST_HEADER;
        p->hdr_got = 0;
        p->hdr_need = 8;
        p->cap_len = 0;
        p->cap_type = 0;
        if (st != MS_OK) {
            return st;
        }
        st = close_finished_parents(p);
        if (st != MS_OK) {
            return st;
        }
        return emit_format_if_ready(p, 0);
    }
    if (eos) {
        fail_p(p, MS_ERR_MALFORMED);
        return MS_ERR_MALFORMED;
    }
    return MS_OK;
}

static ms_status handle_header(ms_bmff_parser *p, const box_hdr *h)
{
    ms_bmff_box box;
    uint64_t payload;
    uint64_t end;
    int until_eof = (h->size == 0);
    uint32_t parent = parent_type_of(p);
    int depth = depth_of(p);
    ms_status st;

    if (p->nboxes >= MS_BMFF_MAX_BOXES) {
        fail_p(p, MS_ERR_OVERFLOW);
        return MS_ERR_OVERFLOW;
    }
    if (until_eof) {
        end = UINT64_MAX;
        payload = 0;
    } else {
        if (h->size < h->header_size) {
            fail_p(p, MS_ERR_MALFORMED);
            return MS_ERR_MALFORMED;
        }
        payload = h->size - h->header_size;
        if (p->file_off > UINT64_MAX - (h->size - h->header_size)) {
            fail_p(p, MS_ERR_OVERFLOW);
            return MS_ERR_OVERFLOW;
        }
        end = p->file_off + payload;
    }
    if (p->sp > 0 && p->stack[p->sp - 1].end != UINT64_MAX) {
        if (until_eof || end > p->stack[p->sp - 1].end) {
            fail_p(p, MS_ERR_MALFORMED);
            return MS_ERR_MALFORMED;
        }
    }
    /* size=0 只允许出现在顶层（ISO：直到文件末尾，后面不能再有兄弟箱）。 */
    if (until_eof && p->sp != 0) {
        fail_p(p, MS_ERR_MALFORMED);
        return MS_ERR_MALFORMED;
    }

    if (h->type == T_MOOV) {
        p->saw_moov = 1;
    } else if (h->type == T_MDAT) {
        p->saw_mdat = 1;
    } else if (h->type == T_MOOF) {
        p->saw_moof = 1;
    } else if (h->type == T_MVEX) {
        p->saw_mvex = 1;
    } else if (h->type == T_STYP) {
        p->saw_styp = 1;
    }

    memset(&box, 0, sizeof(box));
    box.type = h->type;
    box.parent_type = parent;
    box.has_uuid = h->has_uuid;
    if (h->has_uuid) {
        memcpy(box.usertype, h->usertype, 16);
    }
    box.offset = p->file_off - h->header_size;
    box.size = h->size;
    box.header_size = h->header_size;
    box.depth = depth;
    box.is_container = ms_bmff_is_container_type(h->type);
    p->nboxes++;
    if (depth > p->max_depth_seen) {
        p->max_depth_seen = depth;
    }
    if (p->cb.on_box != NULL) {
        st = p->cb.on_box(p->cb.ctx, &box);
        if (st != MS_OK) {
            return st;
        }
    }
    if (h->type == T_MOOF) {
        st = ms_bmff_on_moof_begin(p, box.offset, box.size);
        if (st != MS_OK) {
            return st;
        }
    }

    /* size=0 的 moov/moof/ftyp：没有明确长度且后面不能再有 moov，本步不缓冲到 EOF。 */
    if (until_eof && (h->type == T_MOOV || h->type == T_MOOF ||
                      h->type == T_FTYP || h->type == T_STYP ||
                      ms_bmff_is_container_type(h->type))) {
        fail_p(p, MS_ERR_UNSUPPORTED);
        return MS_ERR_UNSUPPORTED;
    }

    if (h->type == T_FTYP || h->type == T_STYP) {
        return start_brands(p, payload, h->type == T_STYP);
    }
    if (ms_bmff_is_container_type(h->type)) {
        return enter_container(p, h->type, end);
    }
    if (ms_bmff_is_capture_type(h->type)) {
        if (until_eof) {
            fail_p(p, MS_ERR_UNSUPPORTED);
            return MS_ERR_UNSUPPORTED;
        }
        return begin_capture(p, h->type, payload);
    }
    if (h->type == T_MDAT) {
        ms_bmff_lookback_on_mdat(p, payload, until_eof);
    }
    return begin_skip(p, payload, until_eof, h->type == T_MDAT);
}

static ms_status feed_brands(ms_bmff_parser *p, const uint8_t *data, size_t size,
                             size_t *used, int eos)
{
    size_t off = 0;
    *used = 0;
    while (off < size && p->body_left > 0) {
        size_t take;
        size_t want = (p->brand_phase < 8) ? (8 - p->brand_tmp_got)
                                           : (4 - p->brand_tmp_got);
        if (want == 0) {
            want = 4;
        }
        if ((uint64_t)want > p->body_left) {
            want = (size_t)p->body_left;
        }
        take = size - off;
        if (take > want) {
            take = want;
        }
        memcpy(p->brand_tmp + p->brand_tmp_got, data + off, take);
        p->brand_tmp_got += take;
        off += take;
        p->body_left -= take;
        p->file_off += take;
        p->brand_phase += take;
        if (p->brand_phase <= 8 && p->brand_tmp_got == 8) {
            uint32_t major = rd_be32(p->brand_tmp);
            uint32_t minor = rd_be32(p->brand_tmp + 4);
            if (!p->body_is_styp || p->major_brand == 0) {
                p->major_brand = major;
                p->minor_version = minor;
            }
            (void)add_brand(p, major);
            p->brand_tmp_got = 0;
        } else if (p->brand_phase > 8 && p->brand_tmp_got == 4) {
            (void)add_brand(p, rd_be32(p->brand_tmp));
            p->brand_tmp_got = 0;
        }
        /* 体未按 4 字节对齐且已结束：多余 1–3 字节视为畸形。 */
        if (p->body_left == 0 && p->brand_tmp_got != 0) {
            fail_p(p, MS_ERR_MALFORMED);
            *used = off;
            return MS_ERR_MALFORMED;
        }
    }
    *used = off;
    if (p->body_left == 0) {
        p->state = ST_HEADER;
        p->hdr_got = 0;
        p->hdr_need = 8;
        p->body_kind = 0;
        {
            ms_status st = close_finished_parents(p);
            if (st != MS_OK) {
                return st;
            }
        }
        return emit_format_if_ready(p, 0);
    }
    if (eos) {
        fail_p(p, MS_ERR_MALFORMED);
        return MS_ERR_MALFORMED;
    }
    return MS_OK;
}

static ms_status feed_skip(ms_bmff_parser *p, const uint8_t *data, size_t size,
                           size_t *used, int eos)
{
    size_t take;
    size_t already = 0;
    *used = 0;
    if (p->skip_is_mdat && ms_bmff_should_harvest(p)) {
        ms_status st = ms_bmff_feed_mdat(p, data, size, &already, eos);
        *used = already;
        if (st != MS_OK) {
            return st;
        }
        if (!p->skip_to_eof && p->skip_left == 0) {
            return finish_skip_if_done(p);
        }
        if (already == size) {
            return MS_OK;
        }
        data += already;
        size -= already;
    }
    if (p->skip_to_eof) {
        take = size;
        p->file_off += take;
        p->bytes_skipped += take;
        *used = already + take;
        if (eos) {
            p->skip_to_eof = 0;
            p->state = ST_HEADER;
            if (p->skip_is_mdat) {
                p->truncated_mdat = 0; /* 声明到 EOF，完整读到 EOS 不算截断 */
            }
            return close_finished_parents(p);
        }
        return MS_OK;
    }
    if (p->skip_left == 0) {
        return finish_skip_if_done(p);
    }
    take = size;
    if ((uint64_t)take > p->skip_left) {
        take = (size_t)p->skip_left;
    }
    if (p->skip_is_mdat && p->lb_active) {
        ms_bmff_lookback_feed(p, data, take);
    }
    p->skip_left -= take;
    p->file_off += take;
    p->bytes_skipped += take;
    *used = already + take;
    if (p->skip_left == 0) {
        p->lb_active = 0;
        return finish_skip_if_done(p);
    }
    if (eos) {
        if (p->skip_is_mdat) {
            p->truncated_mdat = 1;
            p->skip_left = 0;
            p->state = ST_HEADER;
            return close_finished_parents(p);
        }
        fail_p(p, MS_ERR_MALFORMED);
        return MS_ERR_MALFORMED;
    }
    return MS_OK;
}

static ms_status push_ok(const ms_bmff_parser *p)
{
    if (ms_bmff_parser_need_seek(p)) {
        return MS_ERR_NOT_SEEKABLE;
    }
    return MS_OK;
}

ms_status ms_bmff_parser_push(ms_bmff_parser *p,
                              const uint8_t *data,
                              size_t size,
                              size_t *consumed,
                              int eos)
{
    size_t pos = 0;
    ms_status st;

    if (consumed == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    *consumed = 0;
    if (p == NULL || (data == NULL && size != 0)) {
        return MS_ERR_INVALID_ARG;
    }
    if (p->failed) {
        return p->fail_status;
    }
    if (p->finished && size > 0) {
        return MS_ERR_STATE;
    }

    p->confirmed = 1;

    while (pos < size || (eos && pos == size)) {
        if (p->state == ST_SKIP) {
            size_t used = 0;
            st = feed_skip(p, data + pos, size - pos, &used, eos && pos + used == size);
            pos += used;
            *consumed = pos;
            if (st != MS_OK) {
                return st;
            }
            if (p->state == ST_SKIP) {
                if (eos && pos == size) {
                    break;
                }
                if (pos == size) {
                    return push_ok(p);
                }
                continue;
            }
            st = emit_format_if_ready(p, 0);
            if (st != MS_OK) {
                return st;
            }
            if (pos == size && !eos) {
                return push_ok(p);
            }
        }
        if (p->state == ST_BRANDS) {
            size_t used = 0;
            st = feed_brands(p, data + pos, size - pos, &used, eos && pos + used == size);
            pos += used;
            *consumed = pos;
            if (st != MS_OK) {
                return st;
            }
            if (p->state == ST_BRANDS) {
                return push_ok(p);
            }
            if (pos == size && !eos) {
                return push_ok(p);
            }
        }
        if (p->state == ST_CAPTURE) {
            size_t used = 0;
            st = feed_capture(p, data + pos, size - pos, &used, eos && pos + used == size);
            pos += used;
            *consumed = pos;
            if (st != MS_OK) {
                return st;
            }
            if (p->state == ST_CAPTURE) {
                return push_ok(p);
            }
            st = ms_bmff_announce(p);
            if (st != MS_OK) {
                return st;
            }
            if (pos == size && !eos) {
                return push_ok(p);
            }
        }
        if (p->state == ST_HEADER) {
            size_t have;
            size_t need;
            box_hdr h;

            st = close_finished_parents(p);
            if (st != MS_OK) {
                *consumed = pos;
                return st;
            }
            /* 父箱恰好结束，继续同层或更上层。 */
            if (p->sp > 0 && p->stack[p->sp - 1].end != UINT64_MAX &&
                p->file_off == p->stack[p->sp - 1].end) {
                /* close_finished_parents 应收掉；防御。 */
                continue;
            }
            if (p->sp == 0 && p->nboxes > 0 && eos && pos == size &&
                p->hdr_got == 0) {
                break;
            }
            need = (p->hdr_need > 0) ? p->hdr_need : 8;
            have = p->hdr_got;
            if (have < need && pos < size) {
                size_t take = need - have;
                if (take > size - pos) {
                    take = size - pos;
                }
                memcpy(p->hdr + have, data + pos, take);
                p->hdr_got = have + take;
                pos += take;
                p->file_off += take;
                *consumed = pos;
                have = p->hdr_got;
            }
            if (have < 8) {
                if (eos) {
                    if (have == 0 && p->sp == 0) {
                        break;
                    }
                    fail_p(p, MS_ERR_MALFORMED);
                    return MS_ERR_MALFORMED;
                }
                return push_ok(p);
            }
            st = parse_box_header(p->hdr, have, &need, &h);
            if (st == MS_AGAIN) {
                p->hdr_need = need;
                if (have < need && pos < size) {
                    continue;
                }
                if (eos) {
                    fail_p(p, MS_ERR_MALFORMED);
                    return MS_ERR_MALFORMED;
                }
                return push_ok(p);
            }
            if (st != MS_OK) {
                fail_p(p, st);
                return st;
            }
            if (have != h.header_size) {
                /* parse 可能在 uuid 时把 need 抬到 24/32；继续凑。 */
                if (have < h.header_size) {
                    p->hdr_need = h.header_size;
                    if (eos) {
                        fail_p(p, MS_ERR_MALFORMED);
                        return MS_ERR_MALFORMED;
                    }
                    return push_ok(p);
                }
            }
            /* 头已从流中计入 file_off；handle 里 box.offset = file_off - header */
            p->hdr_got = 0;
            p->hdr_need = 8;
            st = handle_header(p, &h);
            *consumed = pos;
            if (st != MS_OK) {
                return st;
            }
            st = emit_format_if_ready(p, 0);
            if (st != MS_OK) {
                return st;
            }
            if (pos == size && !eos) {
                return push_ok(p);
            }
        }
        if (eos && pos == size && p->state == ST_HEADER && p->hdr_got == 0) {
            break;
        }
        if (!eos && pos == size) {
            return push_ok(p);
        }
    }

    if (eos) {
        if (p->state == ST_SKIP && p->skip_to_eof) {
            p->skip_to_eof = 0;
            p->state = ST_HEADER;
        }
        if (p->state == ST_SKIP && p->skip_left > 0) {
            if (p->skip_is_mdat) {
                p->truncated_mdat = 1;
                p->skip_left = 0;
                p->state = ST_HEADER;
            } else {
                fail_p(p, MS_ERR_MALFORMED);
                return MS_ERR_MALFORMED;
            }
        }
        if (p->state == ST_BRANDS || p->state == ST_CAPTURE || p->hdr_got > 0) {
            fail_p(p, MS_ERR_MALFORMED);
            return MS_ERR_MALFORMED;
        }
        st = ms_bmff_announce(p);
        if (st != MS_OK) {
            return st;
        }
        /* 未闭合且声明了明确结束偏移的容器：截断。 */
        if (p->sp > 0) {
            int i;
            for (i = 0; i < p->sp; i++) {
                if (p->stack[i].end != UINT64_MAX && p->file_off < p->stack[i].end) {
                    fail_p(p, MS_ERR_MALFORMED);
                    return MS_ERR_MALFORMED;
                }
            }
            p->sp = 0;
        }
        st = emit_format_if_ready(p, 1);
        if (st != MS_OK) {
            return st;
        }
        if (!p->format_emitted) {
            fail_p(p, p->still_image ? MS_ERR_UNSUPPORTED : MS_ERR_UNKNOWN);
            return p->fail_status;
        }
        p->finished = 1;
    }
    *consumed = pos;
    return push_ok(p);
}

ms_format ms_bmff_parser_format(const ms_bmff_parser *p)
{
    return (p != NULL) ? p->format : MS_FORMAT_UNKNOWN;
}

uint32_t ms_bmff_parser_major_brand(const ms_bmff_parser *p)
{
    return (p != NULL) ? p->major_brand : 0;
}

int ms_bmff_parser_brands(const ms_bmff_parser *p, uint32_t *out, int cap)
{
    int n;
    if (p == NULL) {
        return 0;
    }
    n = p->nbrands;
    if (out != NULL && cap > 0) {
        if (n > cap) {
            n = cap;
        }
        memcpy(out, p->brands, (size_t)n * sizeof(uint32_t));
    }
    return p->nbrands;
}

int ms_bmff_parser_box_count(const ms_bmff_parser *p)
{
    return (p != NULL) ? p->nboxes : 0;
}

int ms_bmff_parser_max_depth(const ms_bmff_parser *p)
{
    return (p != NULL) ? p->max_depth_seen : 0;
}

uint64_t ms_bmff_parser_bytes_skipped(const ms_bmff_parser *p)
{
    return (p != NULL) ? p->bytes_skipped : 0;
}

int ms_bmff_parser_saw_moov(const ms_bmff_parser *p)
{
    return (p != NULL) ? p->saw_moov : 0;
}

int ms_bmff_parser_saw_mdat(const ms_bmff_parser *p)
{
    return (p != NULL) ? p->saw_mdat : 0;
}

int ms_bmff_parser_saw_moof(const ms_bmff_parser *p)
{
    return (p != NULL) ? p->saw_moof : 0;
}

int ms_bmff_parser_saw_mvex(const ms_bmff_parser *p)
{
    return (p != NULL) ? p->saw_mvex : 0;
}

int ms_bmff_parser_truncated_mdat(const ms_bmff_parser *p)
{
    return (p != NULL) ? p->truncated_mdat : 0;
}

int ms_bmff_parser_confirmed(const ms_bmff_parser *p)
{
    return (p != NULL) ? p->confirmed : 0;
}

void ms_bmff_parser_set_random(ms_bmff_parser *p, int on)
{
    if (p != NULL) {
        p->random_ok = on ? 1 : 0;
    }
}

ms_status ms_bmff_parser_skip_to(ms_bmff_parser *p, uint64_t off)
{
    uint64_t end;

    if (p == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    if (p->failed) {
        return p->fail_status;
    }
    if (p->state != ST_SKIP || p->skip_to_eof) {
        return MS_ERR_STATE;
    }
    if (p->skip_left > UINT64_MAX - p->file_off) {
        fail_p(p, MS_ERR_OVERFLOW);
        return MS_ERR_OVERFLOW;
    }
    end = p->file_off + p->skip_left;
    if (off != end) {
        return MS_ERR_STATE;
    }
    if (p->bytes_skipped > UINT64_MAX - p->skip_left) {
        fail_p(p, MS_ERR_OVERFLOW);
        return MS_ERR_OVERFLOW;
    }
    p->bytes_skipped += p->skip_left;
    p->file_off = off;
    p->skip_left = 0;
    ms_bmff_lookback_drop(p);
    return finish_skip_if_done(p);
}

ms_status ms_bmff_parser_need(const ms_bmff_parser *p, int have_size,
                              uint64_t src_size, size_t seq_hint,
                              ms_bmff_need *out)
{
    if (out == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    out->kind = MS_BMFF_NEED_NONE;
    if (p == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    if (p->failed) {
        return p->fail_status;
    }
    if (seq_hint == 0) {
        seq_hint = 4096;
    }
    if (ms_bmff_fill_sample_need(p, out)) {
        return MS_OK;
    }
    if (p->state == ST_SKIP && !p->skip_to_eof && p->random_ok && have_size &&
        p->skip_left > 0) {
        uint64_t end;
        if (p->skip_left <= UINT64_MAX - p->file_off) {
            end = p->file_off + p->skip_left;
            /* 已定稿且还能取样本时不要跳过当前 mdat。 */
            if (!(p->skip_is_mdat && ms_bmff_fill_sample_need(p, out))) {
                if (end <= src_size) {
                    out->kind = MS_BMFF_NEED_JUMP;
                    out->offset = end;
                    out->length = 0;
                    return MS_OK;
                }
            } else {
                return MS_OK;
            }
        }
    }
    if (p->finished) {
        out->kind = MS_BMFF_NEED_NONE;
        return MS_OK;
    }
    out->kind = MS_BMFF_NEED_SEQ;
    out->offset = p->file_off;
    out->length = seq_hint;
    if (p->state == ST_SKIP && !p->skip_to_eof && p->skip_left > 0 &&
        (uint64_t)out->length > p->skip_left) {
        out->length = (size_t)p->skip_left;
    }
    if (p->state == ST_HEADER && p->hdr_need > p->hdr_got) {
        size_t rem = p->hdr_need - p->hdr_got;
        if (out->length < rem) {
            out->length = rem;
        }
    }
    if (have_size && out->offset >= src_size) {
        out->kind = MS_BMFF_NEED_NONE;
        out->length = 0;
    } else if (have_size && src_size - out->offset < (uint64_t)out->length) {
        out->length = (size_t)(src_size - out->offset);
    }
    return MS_OK;
}
