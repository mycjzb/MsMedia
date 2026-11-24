/*
 * DVB（ETSI EN 300 743）、PGS（Blu-ray）与 VobSub 有界解析到 RGBA cue。
 * 不调用、不链接 FFmpeg 字幕解码器。不执行脚本或附件。
 * by: Long <mycjzb@gmail.com>
 */

#ifndef MS_SUB_BMP_CLUT_MAX
#define MS_SUB_BMP_CLUT_MAX 8
#endif
#ifndef MS_SUB_BMP_REG_MAX
#define MS_SUB_BMP_REG_MAX 16
#endif
#ifndef MS_SUB_BMP_OBJ_MAX
#define MS_SUB_BMP_OBJ_MAX 16
#endif
#ifndef MS_SUB_BMP_WIN_MAX
#define MS_SUB_BMP_WIN_MAX 8
#endif

typedef struct ms_bmp_clut {
    int used;
    int id;
    uint32_t have_mask[8];
    uint8_t rgba[256][4];
} ms_bmp_clut;

typedef struct ms_bmp_obj {
    int used;
    int id;
    int w, h;
    uint8_t *idx;
    size_t idx_size;
    /* PGS ODS first/continuation/last 组装：先收齐 RLE 再解码，避免半包出图。 */
    uint8_t *rle;
    size_t rle_len;
    size_t rle_need;
} ms_bmp_obj;

typedef struct ms_bmp_reg {
    int used;
    int id;
    int x, y, w, h;
    int clut_id;
    int depth;
    uint8_t fill;
    int nobj;
    int obj_id[8];
    int obj_x[8];
    int obj_y[8];
} ms_bmp_reg;

typedef struct ms_bmp_win {
    int used;
    int id;
    int x, y, w, h;
} ms_bmp_win;

typedef struct ms_bmp_comp_obj {
    int object_id;
    int window_id;
    int x, y;
    int crop_x, crop_y, crop_w, crop_h;
    int has_crop;
    int forced;
} ms_bmp_comp_obj;

typedef struct ms_sub_bmp_state {
    int have_page_filter;
    uint16_t composition_page_id;
    uint16_t ancillary_page_id;
    int canvas_w;
    int canvas_h;
    ms_bmp_clut cluts[MS_SUB_BMP_CLUT_MAX];
    ms_bmp_reg regions[MS_SUB_BMP_REG_MAX];
    ms_bmp_obj objects[MS_SUB_BMP_OBJ_MAX];
    ms_bmp_win windows[MS_SUB_BMP_WIN_MAX];
    uint8_t *acc;
    size_t acc_len;
    size_t acc_cap;
    int acc_kind; /* 0=空，否则为 ms_sub_bitmap_kind */
    uint32_t acc_track_id;
    int acc_has_start;
    int acc_has_end;
    uint64_t acc_start_90k;
    uint64_t acc_end_90k;
    int pgs_palette_id;
    int ncomp;
    ms_bmp_comp_obj comp[MS_SUB_BMP_OBJ_MAX];
    int pgs_state;
} ms_sub_bmp_state;

static int bmp_has_entry(const ms_bmp_clut *c, int id)
{
    unsigned u;
    if (id < 0 || id > 255) {
        return 0;
    }
    u = (unsigned)id;
    return (c->have_mask[u >> 5] & (1u << (u & 31u))) != 0;
}

static void bmp_set_entry(ms_bmp_clut *c, int id, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    unsigned u;
    if (id < 0 || id > 255) {
        return;
    }
    u = (unsigned)id;
    c->rgba[id][0] = r;
    c->rgba[id][1] = g;
    c->rgba[id][2] = b;
    c->rgba[id][3] = a;
    c->have_mask[u >> 5] |= (1u << (u & 31u));
}

static void ycbcr_to_rgba(int y, int cb, int cr, int t_or_a, int t_is_transparency,
                          uint8_t *r, uint8_t *g, uint8_t *b, uint8_t *a)
{
    int rr, gg, bb;
    if (y < 0) {
        y = 0;
    }
    if (y > 255) {
        y = 255;
    }
    cb -= 128;
    cr -= 128;
    rr = y + (91881 * cr) / 65536;
    gg = y - (22554 * cb + 46802 * cr) / 65536;
    bb = y + (116130 * cb) / 65536;
    if (rr < 0) { rr = 0; } else if (rr > 255) { rr = 255; }
    if (gg < 0) { gg = 0; } else if (gg > 255) { gg = 255; }
    if (bb < 0) { bb = 0; } else if (bb > 255) { bb = 255; }
    *r = (uint8_t)rr;
    *g = (uint8_t)gg;
    *b = (uint8_t)bb;
    if (t_or_a < 0) {
        t_or_a = 0;
    }
    if (t_or_a > 255) {
        t_or_a = 255;
    }
    *a = t_is_transparency ? (uint8_t)(255 - t_or_a) : (uint8_t)t_or_a;
}

static void bmp_init_default_clut(ms_bmp_clut *c)
{
    int i;
    memset(c, 0, sizeof(*c));
    c->used = 1;
    c->id = 0;
    bmp_set_entry(c, 0, 0, 0, 0, 0);
    bmp_set_entry(c, 1, 255, 255, 255, 255);
    bmp_set_entry(c, 2, 0, 0, 0, 255);
    bmp_set_entry(c, 3, 127, 127, 127, 255);
    for (i = 4; i < 16; i++) {
        bmp_set_entry(c, i,
                      (uint8_t)((i & 2) ? 255 : 0),
                      (uint8_t)((i & 4) ? 255 : 0),
                      (uint8_t)((i & 1) ? 255 : 0), 255);
    }
}

static uint16_t be16(const uint8_t *p)
{
    return (uint16_t)(((unsigned)p[0] << 8) | p[1]);
}

static uint32_t be24(const uint8_t *p)
{
    return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | (uint32_t)p[2];
}

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static void bmp_free_objects(ms_sub_doc *doc, ms_sub_bmp_state *st)
{
    int i;
    for (i = 0; i < MS_SUB_BMP_OBJ_MAX; i++) {
        if (st->objects[i].idx != NULL) {
            doc->alloc.free_fn(doc->alloc.ctx, st->objects[i].idx);
            st->objects[i].idx = NULL;
        }
        if (st->objects[i].rle != NULL) {
            doc->alloc.free_fn(doc->alloc.ctx, st->objects[i].rle);
            st->objects[i].rle = NULL;
        }
        memset(&st->objects[i], 0, sizeof(st->objects[i]));
    }
}

static void bmp_acc_reset_meta(ms_sub_bmp_state *st)
{
    st->acc_len = 0;
    st->acc_kind = 0;
    st->acc_track_id = 0;
    st->acc_has_start = 0;
    st->acc_has_end = 0;
    st->acc_start_90k = 0;
    st->acc_end_90k = 0;
}

static void dvb_skip_pes_wrapper(const uint8_t **p, size_t *n)
{
    if (p == NULL || n == NULL || *p == NULL) {
        return;
    }
    if (*n >= 2 && (*p)[0] == 0x20 && (*p)[1] == 0x00) {
        *p += 2;
        *n -= 2;
    }
}

static void bmp_acc_trim_ff(ms_sub_bmp_state *st)
{
    while (st->acc_len > 0 && st->acc[st->acc_len - 1u] == 0xFF) {
        st->acc_len--;
    }
}

static void ms_sub_bmp_state_reset(ms_sub_doc *doc)
{
    ms_sub_bmp_state *st;
    if (doc == NULL || doc->bmp_state == NULL) {
        return;
    }
    st = (ms_sub_bmp_state *)doc->bmp_state;
    bmp_free_objects(doc, st);
    if (st->acc != NULL) {
        doc->alloc.free_fn(doc->alloc.ctx, st->acc);
        st->acc = NULL;
    }
    st->acc_cap = 0;
    bmp_acc_reset_meta(st);
    memset(st->cluts, 0, sizeof(st->cluts));
    memset(st->regions, 0, sizeof(st->regions));
    memset(st->windows, 0, sizeof(st->windows));
    memset(st->comp, 0, sizeof(st->comp));
    st->ncomp = 0;
    st->canvas_w = 0;
    st->canvas_h = 0;
    st->pgs_palette_id = 0;
    st->pgs_state = 0;
}

static void ms_sub_bmp_state_free(ms_sub_doc *doc)
{
    ms_sub_bmp_state *st;
    if (doc == NULL || doc->bmp_state == NULL) {
        return;
    }
    st = (ms_sub_bmp_state *)doc->bmp_state;
    bmp_free_objects(doc, st);
    if (st->acc != NULL) {
        doc->alloc.free_fn(doc->alloc.ctx, st->acc);
        st->acc = NULL;
    }
    doc->alloc.free_fn(doc->alloc.ctx, st);
    doc->bmp_state = NULL;
}

static ms_status bmp_state_ensure(ms_sub_doc *doc, ms_sub_bmp_state **out)
{
    ms_sub_bmp_state *st;
    if (doc->bmp_state == NULL) {
        st = (ms_sub_bmp_state *)doc->alloc.alloc_fn(doc->alloc.ctx, sizeof(*st));
        if (st == NULL) {
            return MS_ERR_NOMEM;
        }
        memset(st, 0, sizeof(*st));
        doc->bmp_state = st;
    }
    *out = (ms_sub_bmp_state *)doc->bmp_state;
    return MS_OK;
}

static ms_bmp_clut *bmp_clut_get(ms_sub_bmp_state *st, int id, int create)
{
    int i, free_i = -1;
    for (i = 0; i < MS_SUB_BMP_CLUT_MAX; i++) {
        if (st->cluts[i].used && st->cluts[i].id == id) {
            return &st->cluts[i];
        }
        if (!st->cluts[i].used && free_i < 0) {
            free_i = i;
        }
    }
    if (!create || free_i < 0) {
        return NULL;
    }
    memset(&st->cluts[free_i], 0, sizeof(st->cluts[free_i]));
    st->cluts[free_i].used = 1;
    st->cluts[free_i].id = id;
    return &st->cluts[free_i];
}

static ms_bmp_obj *bmp_obj_get(ms_sub_doc *doc, ms_sub_bmp_state *st, int id, int create)
{
    int i, free_i = -1;
    for (i = 0; i < MS_SUB_BMP_OBJ_MAX; i++) {
        if (st->objects[i].used && st->objects[i].id == id) {
            return &st->objects[i];
        }
        if (!st->objects[i].used && free_i < 0) {
            free_i = i;
        }
    }
    if (!create || free_i < 0) {
        return NULL;
    }
    if (st->objects[free_i].idx != NULL) {
        doc->alloc.free_fn(doc->alloc.ctx, st->objects[free_i].idx);
    }
    memset(&st->objects[free_i], 0, sizeof(st->objects[free_i]));
    st->objects[free_i].used = 1;
    st->objects[free_i].id = id;
    return &st->objects[free_i];
}

static ms_bmp_reg *bmp_reg_get(ms_sub_bmp_state *st, int id, int create)
{
    int i, free_i = -1;
    for (i = 0; i < MS_SUB_BMP_REG_MAX; i++) {
        if (st->regions[i].used && st->regions[i].id == id) {
            return &st->regions[i];
        }
        if (!st->regions[i].used && free_i < 0) {
            free_i = i;
        }
    }
    if (!create || free_i < 0) {
        return NULL;
    }
    memset(&st->regions[free_i], 0, sizeof(st->regions[free_i]));
    st->regions[free_i].used = 1;
    st->regions[free_i].id = id;
    return &st->regions[free_i];
}

static void close_open_bitmap_cues(ms_sub_doc *doc, uint32_t track_id, uint64_t end_90k)
{
    size_t i;
    for (i = 0; i < doc->nitems; i++) {
        ms_sub_item *it = &doc->items[i];
        if (it->track_id != track_id || it->bitmap_size == 0) {
            continue;
        }
        if (!it->has_end || it->end_90k > end_90k) {
            it->has_end = 1;
            it->end_90k = (end_90k < it->start_90k) ? it->start_90k : end_90k;
        }
    }
}

static void clut_lookup(const ms_bmp_clut *c, int idx, uint8_t *px)
{
    if (c != NULL && bmp_has_entry(c, idx)) {
        memcpy(px, c->rgba[idx], 4);
        return;
    }
    px[0] = px[1] = px[2] = px[3] = 0;
}

static ms_status emit_rgba_cue(ms_sub_doc *doc, uint32_t track_id, int kind,
                               int x, int y, int w, int h,
                               int crop_x, int crop_y, int crop_w, int crop_h,
                               int canvas_w, int canvas_h,
                               const uint8_t *rgba, size_t rgba_n,
                               int has_start, uint64_t start_90k,
                               int has_end, uint64_t end_90k)
{
    ms_sub_cue cue;
    if (w <= 0 || h <= 0 || rgba == NULL || rgba_n != (size_t)w * (size_t)h * 4u) {
        return MS_ERR_MALFORMED;
    }
    if (w > MS_SUB_BITMAP_WIDTH_MAX || h > MS_SUB_BITMAP_HEIGHT_MAX) {
        return MS_ERR_OVERFLOW;
    }
    ms_sub_cue_default(&cue);
    cue.track_id = track_id;
    cue.has_start = has_start;
    cue.start_90k = start_90k;
    cue.has_end = has_end;
    cue.end_90k = end_90k;
    cue.text = "";
    cue.text_size = 0;
    cue.style_flags = MS_SUB_STYLE_HAS_BITMAP | MS_SUB_STYLE_HAS_POS;
    if (canvas_w > 0 && canvas_h > 0) {
        cue.style_flags |= MS_SUB_STYLE_HAS_CANVAS;
    }
    if (crop_w > 0 && crop_h > 0) {
        cue.style_flags |= MS_SUB_STYLE_HAS_CROP;
    }
    cue.pos_x = x;
    cue.pos_y = y;
    cue.play_res_x = canvas_w;
    cue.play_res_y = canvas_h;
    cue.bitmap_kind = kind;
    cue.bitmap_w = w;
    cue.bitmap_h = h;
    cue.crop_x = crop_x;
    cue.crop_y = crop_y;
    cue.crop_w = crop_w;
    cue.crop_h = crop_h;
    cue.bitmap_rgba = rgba;
    cue.bitmap_rgba_size = rgba_n;
    return ms_sub_doc_add_cue(doc, &cue);
}

static int bmp_paint(uint8_t *line, int w, int *x, int run, int color)
{
    if (run <= 0 || line == NULL) {
        return 0;
    }
    if (*x < 0) {
        *x = 0;
    }
    while (run-- > 0 && *x < w) {
        line[(*x)++] = (uint8_t)color;
    }
    return 0;
}

static int dvb_get_bits(const uint8_t **pp, const uint8_t *end, unsigned *acc,
                        unsigned *nbits, int n)
{
    unsigned v;
    while (*nbits < (unsigned)n) {
        if (*pp >= end) {
            return -1;
        }
        *acc = (*acc << 8) | *(*pp)++;
        *nbits += 8;
    }
    *nbits -= (unsigned)n;
    v = (*acc >> *nbits) & ((1u << n) - 1u);
    return (int)v;
}

static void dvb_align_byte(unsigned *nbits)
{
    if (*nbits & 7u) {
        *nbits &= ~7u;
    }
}

static int dvb_decode_2bit_line(const uint8_t **pp, const uint8_t *end, uint8_t *line, int w)
{
    unsigned acc = 0, nbits = 0;
    int x = 0;
    while (x < w) {
        int b, run, color;
        b = dvb_get_bits(pp, end, &acc, &nbits, 2);
        if (b < 0) {
            break;
        }
        if (b != 0) {
            bmp_paint(line, w, &x, 1, b);
            continue;
        }
        b = dvb_get_bits(pp, end, &acc, &nbits, 2);
        if (b < 0) {
            break;
        }
        if (b != 0) {
            color = dvb_get_bits(pp, end, &acc, &nbits, 2);
            if (color < 0) {
                break;
            }
            bmp_paint(line, w, &x, b, color);
            continue;
        }
        b = dvb_get_bits(pp, end, &acc, &nbits, 2);
        if (b < 0) {
            break;
        }
        if (b == 0) {
            break; /* end of string */
        }
        if (b == 1) {
            bmp_paint(line, w, &x, 1, 0);
            continue;
        }
        if (b == 2) {
            run = dvb_get_bits(pp, end, &acc, &nbits, 4);
            color = dvb_get_bits(pp, end, &acc, &nbits, 2);
            if (run < 0 || color < 0) {
                break;
            }
            bmp_paint(line, w, &x, run + 9, color);
            continue;
        }
        run = dvb_get_bits(pp, end, &acc, &nbits, 8);
        color = dvb_get_bits(pp, end, &acc, &nbits, 2);
        if (run < 0 || color < 0) {
            break;
        }
        bmp_paint(line, w, &x, run + 25, color);
    }
    dvb_align_byte(&nbits);
    return x;
}

static int dvb_decode_4bit_line(const uint8_t **pp, const uint8_t *end, uint8_t *line, int w)
{
    unsigned acc = 0, nbits = 0;
    int x = 0;
    while (x < w) {
        int b, run, color;
        b = dvb_get_bits(pp, end, &acc, &nbits, 4);
        if (b < 0) {
            break;
        }
        if (b != 0) {
            bmp_paint(line, w, &x, 1, b);
            continue;
        }
        b = dvb_get_bits(pp, end, &acc, &nbits, 4);
        if (b < 0) {
            break;
        }
        if (b == 0) {
            break;
        }
        if ((b & 0xC) == 0) {
            bmp_paint(line, w, &x, b, 0); /* 0000 00xx, xx=1..3 of color 0 */
            continue;
        }
        if ((b & 0xC) == 0x4) {
            bmp_paint(line, w, &x, (b & 3) + 4, 0);
            continue;
        }
        if ((b & 0x8) == 0) {
            /* already handled 0x0/0x4 */
            continue;
        }
        if ((b & 0xC) == 0x8) {
            color = dvb_get_bits(pp, end, &acc, &nbits, 4);
            if (color < 0) {
                break;
            }
            bmp_paint(line, w, &x, (b & 3) + 9, color);
            continue;
        }
        /* 1100 + 8-bit run + color, or 11xx */
        if ((b & 0xF) == 0xC) {
            run = dvb_get_bits(pp, end, &acc, &nbits, 8);
            color = dvb_get_bits(pp, end, &acc, &nbits, 4);
            if (run < 0 || color < 0) {
                break;
            }
            bmp_paint(line, w, &x, run + 25, color);
            continue;
        }
        run = ((b & 3) << 4);
        run += dvb_get_bits(pp, end, &acc, &nbits, 4);
        color = dvb_get_bits(pp, end, &acc, &nbits, 4);
        if (run < 0 || color < 0) {
            break;
        }
        bmp_paint(line, w, &x, run, color);
    }
    dvb_align_byte(&nbits);
    return x;
}

static int dvb_decode_8bit_line(const uint8_t **pp, const uint8_t *end, uint8_t *line, int w)
{
    const uint8_t *p = *pp;
    int x = 0;
    while (p < end && x < w) {
        int bits = *p++;
        if (bits) {
            bmp_paint(line, w, &x, 1, bits);
            continue;
        }
        if (p >= end) {
            break;
        }
        bits = *p++;
        if ((bits & 0x80) == 0) {
            if ((bits & 0x7f) == 0) {
                break;
            }
            bmp_paint(line, w, &x, bits & 0x7f, 0);
        } else {
            int color;
            if (p >= end) {
                break;
            }
            color = *p++;
            bmp_paint(line, w, &x, bits & 0x7f, color);
        }
    }
    *pp = p;
    return x;
}

static ms_status dvb_decode_object_field(const uint8_t **pp, const uint8_t *field_end,
                                         uint8_t *idx, int w, int h, int start_line,
                                         int line_step)
{
    const uint8_t *p = *pp;
    int y = start_line;
    while (p < field_end && y < h) {
        int type = *p++;
        if (type == 0x10) {
            dvb_decode_2bit_line(&p, field_end, idx + y * w, w);
            y += line_step;
        } else if (type == 0x11) {
            dvb_decode_4bit_line(&p, field_end, idx + y * w, w);
            y += line_step;
        } else if (type == 0x12) {
            dvb_decode_8bit_line(&p, field_end, idx + y * w, w);
            y += line_step;
        } else if (type == 0x20 && p + 2 <= field_end) {
            p += 2;
        } else if (type == 0x21 && p + 4 <= field_end) {
            p += 4;
        } else if (type == 0x22 && p + 16 <= field_end) {
            p += 16;
        } else if (type == 0xF0 || type == 0x00) {
            continue;
        } else {
            break;
        }
    }
    *pp = (p > field_end) ? field_end : p;
    return MS_OK;
}

static int dvb_page_ok(const ms_sub_bmp_state *st, uint16_t page_id)
{
    if (!st->have_page_filter) {
        return 1;
    }
    return page_id == st->composition_page_id || page_id == st->ancillary_page_id;
}

static void dvb_apply_page_state(ms_sub_bmp_state *st, int page_state)
{
    if (page_state == 0x01 || page_state == 0x02) {
        memset(st->regions, 0, sizeof(st->regions));
        if (page_state == 0x02) {
            memset(st->cluts, 0, sizeof(st->cluts));
        }
    }
}

static ms_status dvb_compose(ms_sub_doc *doc, ms_sub_bmp_state *st, uint32_t track_id,
                             int has_start, uint64_t start_90k,
                             int has_end, uint64_t end_90k)
{
    int i, canvas_w, canvas_h;
    ms_bmp_clut def;
    ms_status last = MS_OK;
    canvas_w = st->canvas_w > 0 ? st->canvas_w : 720;
    canvas_h = st->canvas_h > 0 ? st->canvas_h : 576;
    bmp_init_default_clut(&def);
    close_open_bitmap_cues(doc, track_id, start_90k);
    for (i = 0; i < MS_SUB_BMP_REG_MAX; i++) {
        ms_bmp_reg *rg = &st->regions[i];
        ms_bmp_clut *cl;
        uint8_t *idx = NULL;
        uint8_t *rgba = NULL;
        size_t n;
        int k, x, y;
        ms_status stt;
        if (!rg->used || rg->w <= 0 || rg->h <= 0) {
            continue;
        }
        cl = bmp_clut_get(st, rg->clut_id, 0);
        if (cl == NULL) {
            cl = &def;
        }
        n = (size_t)rg->w * (size_t)rg->h;
        idx = (uint8_t *)doc->alloc.alloc_fn(doc->alloc.ctx, n);
        if (idx == NULL) {
            return MS_ERR_NOMEM;
        }
        memset(idx, rg->fill, n);
        for (k = 0; k < rg->nobj; k++) {
            ms_bmp_obj *ob = bmp_obj_get(doc, st, rg->obj_id[k], 0);
            if (ob == NULL || ob->idx == NULL) {
                continue;
            }
            for (y = 0; y < ob->h; y++) {
                int dy = rg->obj_y[k] + y;
                if (dy < 0 || dy >= rg->h) {
                    continue;
                }
                for (x = 0; x < ob->w; x++) {
                    int dx = rg->obj_x[k] + x;
                    uint8_t pix;
                    if (dx < 0 || dx >= rg->w) {
                        continue;
                    }
                    pix = ob->idx[y * ob->w + x];
                    if (pix != 0) {
                        idx[dy * rg->w + dx] = pix;
                    }
                }
            }
        }
        rgba = (uint8_t *)doc->alloc.alloc_fn(doc->alloc.ctx, n * 4u);
        if (rgba == NULL) {
            doc->alloc.free_fn(doc->alloc.ctx, idx);
            return MS_ERR_NOMEM;
        }
        for (y = 0; y < rg->w * rg->h; y++) {
            clut_lookup(cl, idx[y], rgba + (size_t)y * 4u);
        }
        doc->alloc.free_fn(doc->alloc.ctx, idx);
        stt = emit_rgba_cue(doc, track_id, MS_SUB_BITMAP_DVB,
                            rg->x, rg->y, rg->w, rg->h,
                            0, 0, 0, 0, canvas_w, canvas_h,
                            rgba, n * 4u,
                            has_start, start_90k, has_end, end_90k);
        doc->alloc.free_fn(doc->alloc.ctx, rgba);
        if (stt == MS_ERR_OVERFLOW || stt == MS_ERR_NOMEM) {
            return stt;
        }
        if (stt != MS_OK) {
            last = stt;
        }
    }
    return last;
}

static ms_status dvb_parse_segments(ms_sub_doc *doc, ms_sub_bmp_state *st,
                                    const uint8_t *data, size_t size,
                                    uint32_t track_id,
                                    int has_start, uint64_t start_90k,
                                    int has_end, uint64_t end_90k,
                                    int *completed)
{
    const uint8_t *p = data;
    const uint8_t *end = data + size;
    int saw_end = 0;
    *completed = 0;
    if (size >= 2 && p[0] == 0x20 && p[1] == 0x00) {
        p += 2;
    }
    while (p + 6 <= end) {
        uint8_t type;
        uint16_t page, len;
        const uint8_t *seg, *seg_end;
        if (*p == 0xFF) {
            break;
        }
        if (*p != 0x0F) {
            return MS_ERR_MALFORMED;
        }
        p++;
        type = *p++;
        page = be16(p);
        p += 2;
        len = be16(p);
        p += 2;
        if (p + len > end) {
            return MS_ERR_MALFORMED;
        }
        seg = p;
        seg_end = p + len;
        p += len;
        if (!dvb_page_ok(st, page)) {
            continue;
        }
        if (type == 0x10) {
            const uint8_t *s = seg;
            int state;
            if (s + 2 > seg_end) {
                continue;
            }
            s++;
            state = (s[0] >> 2) & 0x3;
            s++;
            dvb_apply_page_state(st, state);
            while (s + 6 <= seg_end) {
                ms_bmp_reg *rg = bmp_reg_get(st, s[0], 1);
                s += 2;
                if (rg != NULL) {
                    rg->x = (int)be16(s);
                    rg->y = (int)be16(s + 2);
                }
                s += 4;
            }
        } else if (type == 0x11) {
            const uint8_t *s = seg;
            ms_bmp_reg *rg;
            int fill, depth;
            if (s + 10 > seg_end) {
                continue;
            }
            rg = bmp_reg_get(st, s[0], 1);
            if (rg == NULL) {
                continue;
            }
            fill = (s[1] >> 3) & 1;
            rg->w = (int)be16(s + 2);
            rg->h = (int)be16(s + 4);
            depth = (s[6] >> 2) & 0x7;
            rg->depth = (depth == 1) ? 2 : (depth == 2) ? 4 : 8;
            rg->clut_id = s[7];
            rg->fill = fill ? s[8] : 0;
            s += 10;
            rg->nobj = 0;
            if (rg->w <= 0 || rg->h <= 0 ||
                rg->w > MS_SUB_BITMAP_WIDTH_MAX || rg->h > MS_SUB_BITMAP_HEIGHT_MAX) {
                rg->used = 0;
                continue;
            }
            while (s + 6 <= seg_end && rg->nobj < 8) {
                int typ = (s[2] >> 6) & 3;
                rg->obj_id[rg->nobj] = (int)be16(s);
                rg->obj_x[rg->nobj] = ((s[2] & 0x0F) << 8) | s[3];
                rg->obj_y[rg->nobj] = ((s[4] & 0x0F) << 8) | s[5];
                rg->nobj++;
                s += 6;
                if ((typ == 1 || typ == 2) && s + 2 <= seg_end) {
                    s += 2;
                }
            }
        } else if (type == 0x12) {
            const uint8_t *s = seg;
            ms_bmp_clut *cl;
            if (s + 2 > seg_end) {
                continue;
            }
            cl = bmp_clut_get(st, s[0], 1);
            s += 2;
            if (cl == NULL) {
                continue;
            }
            while (s + 2 <= seg_end) {
                int eid = s[0];
                int full = (s[1] >> 1) & 1;
                int y, cr, cb, t;
                uint8_t r, g, b, a;
                s += 2;
                if (full) {
                    if (s + 4 > seg_end) {
                        break;
                    }
                    y = s[0];
                    cr = s[1];
                    cb = s[2];
                    t = s[3];
                    s += 4;
                } else {
                    if (s + 2 > seg_end) {
                        break;
                    }
                    y = s[0] & 0xFC;
                    cr = ((s[0] & 3) << 6) | ((s[1] >> 2) & 0x3C);
                    cb = (s[1] & 0x0C) << 4;
                    t = (s[1] & 3) * 85;
                    s += 2;
                }
                ycbcr_to_rgba(y, cb, cr, t, 1, &r, &g, &b, &a);
                bmp_set_entry(cl, eid, r, g, b, a);
            }
        } else if (type == 0x13) {
            const uint8_t *s = seg;
            int oid, method, top_len, bot_len, ow = 0, oh = 0, i;
            ms_bmp_obj *ob;
            uint8_t *tmp;
            if (s + 7 > seg_end) {
                continue;
            }
            oid = (int)be16(s);
            method = (s[2] >> 2) & 0x3;
            s += 3;
            if (method != 0) {
                continue;
            }
            top_len = (int)be16(s);
            bot_len = (int)be16(s + 2);
            s += 4;
            if (s + top_len + bot_len > seg_end) {
                continue;
            }
            for (i = 0; i < MS_SUB_BMP_REG_MAX; i++) {
                int k;
                if (!st->regions[i].used) {
                    continue;
                }
                for (k = 0; k < st->regions[i].nobj; k++) {
                    if (st->regions[i].obj_id[k] == oid) {
                        ow = st->regions[i].w;
                        oh = st->regions[i].h;
                    }
                }
            }
            if (oh <= 0) {
                ow = 720;
                oh = 576;
            }
            if (ow <= 0 || oh <= 0 ||
                ow > MS_SUB_BITMAP_WIDTH_MAX || oh > MS_SUB_BITMAP_HEIGHT_MAX) {
                continue;
            }
            tmp = (uint8_t *)doc->alloc.alloc_fn(doc->alloc.ctx, (size_t)ow * (size_t)oh);
            if (tmp == NULL) {
                return MS_ERR_NOMEM;
            }
            memset(tmp, 0, (size_t)ow * (size_t)oh);
            {
                const uint8_t *tp = s, *te = s + top_len;
                const uint8_t *bp = s + top_len, *be = s + top_len + bot_len;
                (void)dvb_decode_object_field(&tp, te, tmp, ow, oh, 0, 2);
                if (bot_len > 0) {
                    (void)dvb_decode_object_field(&bp, be, tmp, ow, oh, 1, 2);
                }
            }
            ob = bmp_obj_get(doc, st, oid, 1);
            if (ob == NULL) {
                doc->alloc.free_fn(doc->alloc.ctx, tmp);
                continue;
            }
            if (ob->idx != NULL) {
                doc->alloc.free_fn(doc->alloc.ctx, ob->idx);
            }
            ob->idx = tmp;
            ob->idx_size = (size_t)ow * (size_t)oh;
            ob->w = ow;
            ob->h = oh;
        } else if (type == 0x14) {
            if (seg + 5 <= seg_end) {
                st->canvas_w = (int)be16(seg + 1) + 1;
                st->canvas_h = (int)be16(seg + 3) + 1;
                if (st->canvas_w > MS_SUB_BITMAP_WIDTH_MAX) {
                    st->canvas_w = MS_SUB_BITMAP_WIDTH_MAX;
                }
                if (st->canvas_h > MS_SUB_BITMAP_HEIGHT_MAX) {
                    st->canvas_h = MS_SUB_BITMAP_HEIGHT_MAX;
                }
            }
        } else if (type == 0x80) {
            saw_end = 1;
        }
    }
    if (saw_end) {
        *completed = 1;
        return dvb_compose(doc, st, track_id, has_start, start_90k, has_end, end_90k);
    }
    return MS_OK;
}

static int pgs_rle_decode(const uint8_t *buf, size_t buf_size, uint8_t *out, int w, int h)
{
    const uint8_t *p = buf, *end = buf + buf_size;
    int pixel_count = 0, line = 0, total;
    if (w <= 0 || h <= 0) {
        return -1;
    }
    total = w * h;
    memset(out, 0, (size_t)total);
    while (p < end && line < h) {
        int color = *p++, run = 1, flags;
        if (color == 0x00) {
            if (p >= end) {
                break;
            }
            flags = *p++;
            run = flags & 0x3f;
            if (flags & 0x40) {
                if (p >= end) {
                    break;
                }
                run = (run << 8) + *p++;
            }
            color = (flags & 0x80) ? (p < end ? *p++ : 0) : 0;
        }
        if (run > 0) {
            if (pixel_count + run > total) {
                run = total - pixel_count;
            }
            if (run > 0) {
                memset(out + pixel_count, (uint8_t)color, (size_t)run);
                pixel_count += run;
            }
        } else {
            if (w > 0 && (pixel_count % w) != 0) {
                pixel_count = (pixel_count / w + 1) * w;
                if (pixel_count > total) {
                    pixel_count = total;
                }
            }
            line++;
        }
    }
    return 0;
}

static void pgs_obj_drop_bitmap(ms_sub_doc *doc, ms_bmp_obj *ob)
{
    if (ob == NULL) {
        return;
    }
    if (ob->idx != NULL) {
        doc->alloc.free_fn(doc->alloc.ctx, ob->idx);
        ob->idx = NULL;
    }
    if (ob->rle != NULL) {
        doc->alloc.free_fn(doc->alloc.ctx, ob->rle);
        ob->rle = NULL;
    }
    ob->idx_size = 0;
    ob->rle_len = 0;
    ob->rle_need = 0;
    ob->w = 0;
    ob->h = 0;
}

static ms_status pgs_obj_commit_rle(ms_sub_doc *doc, ms_bmp_obj *ob)
{
    size_t need;
    if (ob == NULL || ob->rle == NULL || ob->rle_need == 0 ||
        ob->rle_len < ob->rle_need || ob->w <= 0 || ob->h <= 0) {
        pgs_obj_drop_bitmap(doc, ob);
        if (ob != NULL) {
            ob->used = 0;
        }
        return MS_OK;
    }
    need = (size_t)ob->w * (size_t)ob->h;
    if (ob->idx != NULL) {
        doc->alloc.free_fn(doc->alloc.ctx, ob->idx);
        ob->idx = NULL;
    }
    ob->idx_size = need;
    ob->idx = (uint8_t *)doc->alloc.alloc_fn(doc->alloc.ctx, need);
    if (ob->idx == NULL) {
        pgs_obj_drop_bitmap(doc, ob);
        ob->used = 0;
        return MS_ERR_NOMEM;
    }
    if (pgs_rle_decode(ob->rle, ob->rle_len, ob->idx, ob->w, ob->h) != 0) {
        pgs_obj_drop_bitmap(doc, ob);
        ob->used = 0;
        return MS_OK;
    }
    doc->alloc.free_fn(doc->alloc.ctx, ob->rle);
    ob->rle = NULL;
    ob->rle_len = 0;
    ob->rle_need = 0;
    return MS_OK;
}

static ms_status pgs_obj_append_rle(ms_sub_doc *doc, ms_bmp_obj *ob,
                                    const uint8_t *src, size_t n, int last)
{
    size_t room, take;
    if (ob == NULL || ob->rle == NULL || ob->rle_need == 0) {
        return MS_OK;
    }
    room = (ob->rle_len < ob->rle_need) ? (ob->rle_need - ob->rle_len) : 0;
    take = n < room ? n : room;
    if (take > 0) {
        memcpy(ob->rle + ob->rle_len, src, take);
        ob->rle_len += take;
    }
    if (ob->rle_len >= ob->rle_need || last) {
        if (ob->rle_len < ob->rle_need) {
            /* 声明了 last 但仍缺片：诚实丢弃，不把半幅当完整对象。 */
            pgs_obj_drop_bitmap(doc, ob);
            ob->used = 0;
            return MS_OK;
        }
        return pgs_obj_commit_rle(doc, ob);
    }
    return MS_OK;
}

static ms_status pgs_handle_seg(ms_sub_doc *doc, ms_sub_bmp_state *st,
                                uint8_t type, const uint8_t *seg, const uint8_t *seg_end,
                                int *saw_end)
{
    if (type == 0x16) { /* PCS */
        const uint8_t *s = seg;
        int state, n, i;
        if (s + 11 > seg_end) {
            return MS_OK;
        }
        st->canvas_w = (int)be16(s);
        st->canvas_h = (int)be16(s + 2);
        state = s[7];
        st->pgs_state = state;
        if (state == 0x80) {
            bmp_free_objects(doc, st);
            memset(st->cluts, 0, sizeof(st->cluts));
            memset(st->windows, 0, sizeof(st->windows));
        }
        st->pgs_palette_id = s[9];
        n = s[10];
        s += 11;
        st->ncomp = 0;
        for (i = 0; i < n && st->ncomp < MS_SUB_BMP_OBJ_MAX; i++) {
            ms_bmp_comp_obj *co;
            int flags;
            if (s + 8 > seg_end) {
                break;
            }
            co = &st->comp[st->ncomp];
            memset(co, 0, sizeof(*co));
            co->object_id = (int)be16(s);
            co->window_id = s[2];
            flags = s[3];
            co->forced = (flags & 0x40) ? 1 : 0;
            co->has_crop = (flags & 0x80) ? 1 : 0;
            co->x = (int)be16(s + 4);
            co->y = (int)be16(s + 6);
            s += 8;
            if (co->has_crop) {
                if (s + 8 > seg_end) {
                    break;
                }
                co->crop_x = (int)be16(s);
                co->crop_y = (int)be16(s + 2);
                co->crop_w = (int)be16(s + 4);
                co->crop_h = (int)be16(s + 6);
                s += 8;
            }
            st->ncomp++;
        }
        return MS_OK;
    }
    if (type == 0x17) { /* WDS */
        int n, i;
        const uint8_t *s;
        if (seg >= seg_end) {
            return MS_OK;
        }
        n = seg[0];
        s = seg + 1;
        memset(st->windows, 0, sizeof(st->windows));
        for (i = 0; i < n && i < MS_SUB_BMP_WIN_MAX && s + 9 <= seg_end; i++) {
            st->windows[i].used = 1;
            st->windows[i].id = s[0];
            st->windows[i].x = (int)be16(s + 1);
            st->windows[i].y = (int)be16(s + 3);
            st->windows[i].w = (int)be16(s + 5);
            st->windows[i].h = (int)be16(s + 7);
            s += 9;
        }
        return MS_OK;
    }
    if (type == 0x14) { /* PDS */
        const uint8_t *s;
        ms_bmp_clut *cl;
        int i, n;
        if (seg + 2 > seg_end) {
            return MS_OK;
        }
        cl = bmp_clut_get(st, seg[0], 1);
        s = seg + 2;
        n = (int)((seg_end - s) / 5);
        if (cl != NULL) {
            for (i = 0; i < n; i++) {
                uint8_t r, g, b, a;
                ycbcr_to_rgba(s[1], s[3], s[2], s[4], 0, &r, &g, &b, &a);
                bmp_set_entry(cl, s[0], r, g, b, a);
                s += 5;
            }
        }
        return MS_OK;
    }
    if (type == 0x15) { /* ODS：first / continuation / last */
        const uint8_t *s;
        int oid, seq, w, h, last;
        uint32_t obj_data_len;
        size_t rle_need, avail;
        ms_bmp_obj *ob;
        if (seg + 4 > seg_end) {
            return MS_OK;
        }
        oid = (int)be16(seg);
        seq = seg[3];
        last = (seq & 0x40) != 0;
        if (!(seq & 0x80)) {
            /* 续包：必须已有 first 的 RLE 缓冲，否则缺片跳过。 */
            if (seg + 4 > seg_end) {
                return MS_OK;
            }
            ob = bmp_obj_get(doc, st, oid, 0);
            if (ob == NULL || ob->rle == NULL || ob->rle_need == 0) {
                return MS_OK;
            }
            return pgs_obj_append_rle(doc, ob, seg + 4, (size_t)(seg_end - (seg + 4)), last);
        }
        if (seg + 11 > seg_end) {
            return MS_OK;
        }
        obj_data_len = be24(seg + 4);
        if (obj_data_len < 4) {
            return MS_OK;
        }
        rle_need = (size_t)obj_data_len - 4u;
        s = seg + 4 + 3;
        w = (int)be16(s);
        h = (int)be16(s + 2);
        s += 4;
        if (w <= 0 || h <= 0) {
            return MS_OK;
        }
        if (w > MS_SUB_BITMAP_WIDTH_MAX || h > MS_SUB_BITMAP_HEIGHT_MAX ||
            rle_need > MS_SUB_BITMAP_ACC_MAX) {
            ob = bmp_obj_get(doc, st, oid, 0);
            if (ob != NULL) {
                pgs_obj_drop_bitmap(doc, ob);
                ob->used = 0;
            }
            return MS_ERR_OVERFLOW;
        }
        ob = bmp_obj_get(doc, st, oid, 1);
        if (ob == NULL) {
            return MS_OK;
        }
        pgs_obj_drop_bitmap(doc, ob);
        ob->w = w;
        ob->h = h;
        ob->rle_need = rle_need;
        ob->rle_len = 0;
        if (rle_need == 0) {
            /* 空对象：不发明像素。 */
            ob->used = 0;
            return MS_OK;
        }
        ob->rle = (uint8_t *)doc->alloc.alloc_fn(doc->alloc.ctx, rle_need);
        if (ob->rle == NULL) {
            ob->used = 0;
            return MS_ERR_NOMEM;
        }
        avail = (size_t)(seg_end - s);
        return pgs_obj_append_rle(doc, ob, s, avail, last);
    }
    if (type == 0x80) {
        *saw_end = 1;
    }
    return MS_OK;
}

static ms_status pgs_compose(ms_sub_doc *doc, ms_sub_bmp_state *st, uint32_t track_id,
                             int has_start, uint64_t start_90k,
                             int has_end, uint64_t end_90k)
{
    int i, canvas_w, canvas_h;
    ms_bmp_clut *cl;
    canvas_w = st->canvas_w > 0 ? st->canvas_w : 1920;
    canvas_h = st->canvas_h > 0 ? st->canvas_h : 1080;
    close_open_bitmap_cues(doc, track_id, start_90k);
    if (st->ncomp == 0) {
        return MS_OK;
    }
    cl = bmp_clut_get(st, st->pgs_palette_id, 0);
    if (cl == NULL) {
        return MS_ERR_MALFORMED;
    }
    for (i = 0; i < st->ncomp; i++) {
        ms_bmp_comp_obj *co = &st->comp[i];
        ms_bmp_obj *ob = bmp_obj_get(doc, st, co->object_id, 0);
        uint8_t *rgba;
        size_t n;
        int w, h, sx, sy, sw, sh, x, y;
        ms_status stt;
        if (ob == NULL || ob->idx == NULL) {
            continue;
        }
        w = ob->w;
        h = ob->h;
        sx = 0;
        sy = 0;
        sw = w;
        sh = h;
        if (co->has_crop && co->crop_w > 0 && co->crop_h > 0) {
            sx = co->crop_x;
            sy = co->crop_y;
            sw = co->crop_w;
            sh = co->crop_h;
            if (sx < 0) { sx = 0; }
            if (sy < 0) { sy = 0; }
            if (sx + sw > w) { sw = w - sx; }
            if (sy + sh > h) { sh = h - sy; }
        }
        if (sw <= 0 || sh <= 0) {
            continue;
        }
        n = (size_t)sw * (size_t)sh * 4u;
        rgba = (uint8_t *)doc->alloc.alloc_fn(doc->alloc.ctx, n);
        if (rgba == NULL) {
            return MS_ERR_NOMEM;
        }
        for (y = 0; y < sh; y++) {
            for (x = 0; x < sw; x++) {
                clut_lookup(cl, ob->idx[(sy + y) * w + (sx + x)],
                            rgba + ((size_t)y * (size_t)sw + (size_t)x) * 4u);
            }
        }
        stt = emit_rgba_cue(doc, track_id, MS_SUB_BITMAP_PGS,
                            co->x, co->y, sw, sh,
                            co->has_crop ? co->crop_x : 0,
                            co->has_crop ? co->crop_y : 0,
                            co->has_crop ? sw : 0,
                            co->has_crop ? sh : 0,
                            canvas_w, canvas_h, rgba, n,
                            has_start, start_90k, has_end, end_90k);
        doc->alloc.free_fn(doc->alloc.ctx, rgba);
        if (stt == MS_ERR_OVERFLOW || stt == MS_ERR_NOMEM) {
            return stt;
        }
    }
    return MS_OK;
}

static ms_status pgs_parse_segments(ms_sub_doc *doc, ms_sub_bmp_state *st,
                                    const uint8_t *data, size_t size,
                                    uint32_t track_id,
                                    int has_start, uint64_t start_90k,
                                    int has_end, uint64_t end_90k,
                                    int *completed)
{
    const uint8_t *q = data, *qend = data + size;
    int saw_end = 0;
    int is_sup = (size >= 13 && data[0] == 'P' && data[1] == 'G');
    *completed = 0;
    if (is_sup) {
        while (q + 13 <= qend && q[0] == 'P' && q[1] == 'G') {
            uint8_t type;
            uint16_t len;
            const uint8_t *seg, *seg_end;
            ms_status stt;
            if (!has_start) {
                start_90k = be32(q + 2);
                has_start = 1;
            }
            type = q[10];
            len = be16(q + 11);
            q += 13;
            if (q + len > qend) {
                return MS_ERR_MALFORMED;
            }
            seg = q;
            seg_end = q + len;
            q += len;
            stt = pgs_handle_seg(doc, st, type, seg, seg_end, &saw_end);
            if (stt != MS_OK) {
                return stt;
            }
        }
    } else {
        while (q + 3 <= qend) {
            uint8_t type = q[0];
            uint16_t len = be16(q + 1);
            const uint8_t *seg, *seg_end;
            ms_status stt;
            if (type == 0xFF) {
                break;
            }
            q += 3;
            if (q + len > qend) {
                return MS_ERR_MALFORMED;
            }
            seg = q;
            seg_end = q + len;
            q += len;
            stt = pgs_handle_seg(doc, st, type, seg, seg_end, &saw_end);
            if (stt != MS_OK) {
                return stt;
            }
        }
    }
    if (saw_end) {
        *completed = 1;
        return pgs_compose(doc, st, track_id, has_start, start_90k, has_end, end_90k);
    }
    return MS_OK;
}

static int vob_get_nibble(const uint8_t *p, size_t n, size_t *bit_off, int *out)
{
    size_t byte = *bit_off / 8;
    if (byte >= n) {
        return -1;
    }
    if ((*bit_off & 4u) == 0) {
        *out = (p[byte] >> 4) & 0xF;
    } else {
        *out = p[byte] & 0xF;
    }
    *bit_off += 4;
    return 0;
}

static int vob_decode_rle_line(const uint8_t *p, size_t n, size_t *off, uint8_t *line, int w)
{
    int x = 0;
    if ((*off & 7u) != 0) {
        *off = (*off + 7u) & ~(size_t)7u;
    }
    while (x < w) {
        int nib, v, run, color, nbits = 0;
        unsigned acc = 0;
        while (nbits < 16) {
            if (vob_get_nibble(p, n, off, &nib) != 0) {
                return x;
            }
            acc = (acc << 4) | (unsigned)nib;
            nbits += 4;
            if (acc != 0) {
                if (nbits == 4) {
                    break;
                }
                if (nbits == 8 && acc >= 0x4) {
                    break;
                }
                if (nbits == 12 && acc >= 0x10) {
                    break;
                }
                if (nbits == 16) {
                    break;
                }
            }
        }
        v = (int)acc;
        if (v == 0 && nbits == 16) {
            run = w - x;
            color = 0;
        } else {
            color = v & 3;
            run = v >> 2;
            if (run == 0) {
                run = w - x;
            }
        }
        bmp_paint(line, w, &x, run, color);
    }
    if ((*off & 7u) != 0) {
        *off = (*off + 7u) & ~(size_t)7u;
    }
    return x;
}

static ms_status vob_decode_bitmap(ms_sub_doc *doc, const uint8_t *spu, size_t spu_size,
                                   const uint8_t pal_rgb[16][3],
                                   uint32_t track_id,
                                   int has_start, uint64_t start_90k,
                                   int has_end, uint64_t end_90k)
{
    size_t pack_size, ctrl_off, i;
    const uint8_t *d = spu;
    int x = 0, y = 0, w = 0, h = 0, field1 = 0, field2 = 0, yy;
    int alpha[4] = { 0, 15, 15, 15 };
    int col[4] = { 0, 1, 2, 3 };
    uint8_t *idx, *rgba;
    ms_status stt;
    if (spu_size < 4) {
        return MS_ERR_MALFORMED;
    }
    pack_size = ((size_t)spu[0] << 8) | spu[1];
    ctrl_off = ((size_t)spu[2] << 8) | spu[3];
    if (pack_size == 0 || pack_size > spu_size) {
        pack_size = spu_size;
    }
    if (ctrl_off >= pack_size) {
        return MS_ERR_MALFORMED;
    }
    i = ctrl_off;
    while (i + 1 < pack_size) {
        int cmd = d[i++];
        if (cmd == 0x00 || cmd == 0x01 || cmd == 0x02) {
            continue;
        }
        if (cmd == 0x03 && i + 2 <= pack_size) {
            col[3] = (d[i] >> 4) & 0xF;
            col[2] = d[i] & 0xF;
            col[1] = (d[i + 1] >> 4) & 0xF;
            col[0] = d[i + 1] & 0xF;
            i += 2;
            continue;
        }
        if (cmd == 0x04 && i + 2 <= pack_size) {
            alpha[3] = (d[i] >> 4) & 0xF;
            alpha[2] = d[i] & 0xF;
            alpha[1] = (d[i + 1] >> 4) & 0xF;
            alpha[0] = d[i + 1] & 0xF;
            i += 2;
            continue;
        }
        if (cmd == 0x05 && i + 6 <= pack_size) {
            x = (d[i] << 4) | (d[i + 1] >> 4);
            w = (((d[i + 1] & 0xF) << 8) | d[i + 2]) - x + 1;
            y = (d[i + 3] << 4) | (d[i + 4] >> 4);
            h = (((d[i + 4] & 0xF) << 8) | d[i + 5]) - y + 1;
            i += 6;
            continue;
        }
        if (cmd == 0x06 && i + 4 <= pack_size) {
            field1 = (d[i] << 8) | d[i + 1];
            field2 = (d[i + 2] << 8) | d[i + 3];
            i += 4;
            continue;
        }
        if (cmd == 0xFF) {
            break;
        }
        break;
    }
    if (w <= 0 || h <= 0 || w > MS_SUB_BITMAP_WIDTH_MAX || h > MS_SUB_BITMAP_HEIGHT_MAX) {
        return MS_ERR_MALFORMED;
    }
    if (field1 <= 0 || field2 <= 0 || (size_t)field1 >= pack_size || (size_t)field2 >= pack_size) {
        return MS_ERR_MALFORMED;
    }
    idx = (uint8_t *)doc->alloc.alloc_fn(doc->alloc.ctx, (size_t)w * (size_t)h);
    if (idx == NULL) {
        return MS_ERR_NOMEM;
    }
    memset(idx, 0, (size_t)w * (size_t)h);
    {
        size_t off = (size_t)field1 * 8u;
        for (yy = 0; yy < h; yy += 2) {
            vob_decode_rle_line(d, pack_size, &off, idx + yy * w, w);
        }
        off = (size_t)field2 * 8u;
        for (yy = 1; yy < h; yy += 2) {
            vob_decode_rle_line(d, pack_size, &off, idx + yy * w, w);
        }
    }
    rgba = (uint8_t *)doc->alloc.alloc_fn(doc->alloc.ctx, (size_t)w * (size_t)h * 4u);
    if (rgba == NULL) {
        doc->alloc.free_fn(doc->alloc.ctx, idx);
        return MS_ERR_NOMEM;
    }
    for (yy = 0; yy < w * h; yy++) {
        int ci = idx[yy] & 3;
        int pal = col[ci] & 15;
        rgba[yy * 4] = pal_rgb[pal][0];
        rgba[yy * 4 + 1] = pal_rgb[pal][1];
        rgba[yy * 4 + 2] = pal_rgb[pal][2];
        rgba[yy * 4 + 3] = (uint8_t)(alpha[ci] * 17);
    }
    doc->alloc.free_fn(doc->alloc.ctx, idx);
    stt = emit_rgba_cue(doc, track_id, MS_SUB_BITMAP_VOBSUB,
                        x, y, w, h, 0, 0, 0, 0, 720, 480,
                        rgba, (size_t)w * (size_t)h * 4u,
                        has_start, start_90k, has_end, end_90k);
    doc->alloc.free_fn(doc->alloc.ctx, rgba);
    return stt;
}

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int parse_dec(const char **pp)
{
    const char *p = *pp;
    int v = 0, any = 0;
    while (*p >= '0' && *p <= '9') {
        v = v * 10 + (*p - '0');
        p++;
        any = 1;
    }
    *pp = p;
    return any ? v : -1;
}

static int skip_word(const char **pp, const char *w)
{
    const char *p = *pp;
    size_t n = strlen(w);
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    if (strncmp(p, w, n) != 0) {
        return 0;
    }
    *pp = p + n;
    return 1;
}

static ms_status parse_idx_and_subs(ms_sub_doc *doc,
                                    const uint8_t *idx, size_t idx_size,
                                    const uint8_t *sub, size_t sub_size,
                                    uint32_t track_id)
{
    char *text, *line;
    uint8_t pal[16][3];
    int npal = 0, nts = 0, i;
    uint64_t starts[64];
    size_t pos[64];
    memset(pal, 0, sizeof(pal));
    if (idx == NULL || idx_size == 0 || sub == NULL || sub_size == 0) {
        return MS_ERR_INVALID_ARG;
    }
    if (idx_size > MS_SUB_DOC_BYTES_MAX || sub_size > 2u * 1024u * 1024u) {
        return MS_ERR_OVERFLOW;
    }
    text = (char *)doc->alloc.alloc_fn(doc->alloc.ctx, idx_size + 1);
    if (text == NULL) {
        return MS_ERR_NOMEM;
    }
    memcpy(text, idx, idx_size);
    text[idx_size] = 0;
    line = text;
    while (line != NULL && *line) {
        char *nl = strchr(line, '\n');
        const char *p;
        if (nl) {
            *nl = 0;
            if (nl > line && nl[-1] == '\r') {
                nl[-1] = 0;
            }
        }
        p = line;
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        if (strncmp(p, "palette:", 8) == 0) {
            const char *q = p + 8;
            npal = 0;
            while (*q && npal < 16) {
                int r1, r2, g1, g2, b1, b2;
                while (*q == ' ' || *q == ',') {
                    q++;
                }
                if (!q[0] || !q[1] || !q[2] || !q[3] || !q[4] || !q[5]) {
                    break;
                }
                r1 = hexval(q[0]); r2 = hexval(q[1]);
                g1 = hexval(q[2]); g2 = hexval(q[3]);
                b1 = hexval(q[4]); b2 = hexval(q[5]);
                if (r1 < 0 || r2 < 0 || g1 < 0 || g2 < 0 || b1 < 0 || b2 < 0) {
                    break;
                }
                pal[npal][0] = (uint8_t)((r1 << 4) | r2);
                pal[npal][1] = (uint8_t)((g1 << 4) | g2);
                pal[npal][2] = (uint8_t)((b1 << 4) | b2);
                npal++;
                q += 6;
            }
        } else if (strncmp(p, "timestamp:", 10) == 0 && nts < 64) {
            const char *q = p + 10;
            int hh, mm, ss, cs;
            unsigned long fp = 0;
            while (*q == ' ') { q++; }
            hh = parse_dec(&q);
            if (*q == ':') { q++; }
            mm = parse_dec(&q);
            if (*q == ':') { q++; }
            ss = parse_dec(&q);
            if (*q == ':') { q++; }
            cs = parse_dec(&q);
            if (skip_word(&q, ",") && skip_word(&q, "filepos:")) {
                while (*q == ' ') { q++; }
                while (*q) {
                    int hv = hexval(*q);
                    if (hv < 0) {
                        break;
                    }
                    fp = (fp << 4) | (unsigned)hv;
                    q++;
                }
                if (hh >= 0 && mm >= 0 && ss >= 0 && cs >= 0) {
                    uint64_t t = ((uint64_t)hh * 3600u + (uint64_t)mm * 60u + (uint64_t)ss) * 90000ull;
                    /* 第四段常见为百分之一秒 */
                    t += (cs < 100) ? (uint64_t)cs * 900ull : (uint64_t)cs * 90ull;
                    starts[nts] = t;
                    pos[nts] = (size_t)fp;
                    nts++;
                }
            }
        }
        line = nl ? nl + 1 : NULL;
    }
    doc->alloc.free_fn(doc->alloc.ctx, text);
    if (nts == 0 || npal == 0) {
        return MS_ERR_MALFORMED;
    }
    doc->fmt = MS_SUB_FORMAT_VOBSUB;
    for (i = 0; i < nts; i++) {
        size_t off = pos[i];
        size_t next = (i + 1 < nts) ? pos[i + 1] : sub_size;
        uint64_t t0 = starts[i];
        uint64_t t1 = (i + 1 < nts) ? starts[i + 1] : (t0 + 90000ull);
        const uint8_t *q, *qe;
        ms_status stt;
        if (off >= sub_size || next > sub_size || next <= off) {
            continue;
        }
        q = sub + off;
        qe = sub + next;
        if (qe - q >= 9 && q[0] == 0 && q[1] == 0 && q[2] == 1 && q[3] == 0xBD) {
            int hlen = q[8];
            q += 9 + hlen;
        }
        if (q >= qe) {
            continue;
        }
        stt = vob_decode_bitmap(doc, q, (size_t)(qe - q), pal, track_id, 1, t0, 1, t1);
        if (stt == MS_ERR_OVERFLOW || stt == MS_ERR_NOMEM) {
            return stt;
        }
    }
    return doc->nitems == 0 ? MS_ERR_MALFORMED : MS_OK;
}

ms_status ms_sub_doc_set_bitmap_header(ms_sub_doc *doc, ms_sub_bitmap_kind kind,
                                       const uint8_t *data, size_t size)
{
    ms_sub_bmp_state *st;
    ms_status stt;
    if (doc == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    (void)kind;
    stt = bmp_state_ensure(doc, &st);
    if (stt != MS_OK) {
        return stt;
    }
    if (data != NULL && size >= 4) {
        st->composition_page_id = be16(data);
        st->ancillary_page_id = be16(data + 2);
        st->have_page_filter = 1;
    } else {
        st->have_page_filter = 0;
    }
    return MS_OK;
}

static ms_status acc_append(ms_sub_doc *doc, ms_sub_bmp_state *st,
                            const uint8_t *data, size_t size)
{
    size_t need;
    if (size == 0) {
        return MS_OK;
    }
    need = st->acc_len + size;
    if (need < st->acc_len || need > MS_SUB_BITMAP_ACC_MAX) {
        st->acc_len = 0;
        return MS_ERR_OVERFLOW;
    }
    if (need > st->acc_cap) {
        uint8_t *nb;
        size_t ncap = st->acc_cap == 0 ? 4096 : st->acc_cap;
        while (ncap < need) {
            if (ncap > MS_SUB_BITMAP_ACC_MAX / 2u) {
                ncap = MS_SUB_BITMAP_ACC_MAX;
                break;
            }
            ncap *= 2u;
        }
        if (ncap < need) {
            st->acc_len = 0;
            return MS_ERR_OVERFLOW;
        }
        nb = (uint8_t *)doc->alloc.alloc_fn(doc->alloc.ctx, ncap);
        if (nb == NULL) {
            return MS_ERR_NOMEM;
        }
        if (st->acc != NULL && st->acc_len > 0) {
            memcpy(nb, st->acc, st->acc_len);
        }
        if (st->acc != NULL) {
            doc->alloc.free_fn(doc->alloc.ctx, st->acc);
        }
        st->acc = nb;
        st->acc_cap = ncap;
    }
    memcpy(st->acc + st->acc_len, data, size);
    st->acc_len += size;
    return MS_OK;
}

static int dvb_has_end(const uint8_t *data, size_t size, int *complete_ok)
{
    const uint8_t *q = data, *qe = data + size;
    int has_term = 0;
    *complete_ok = 1;
    if (size >= 2 && q[0] == 0x20 && q[1] == 0x00) {
        q += 2;
    }
    while (q + 6 <= qe) {
        uint16_t len;
        if (*q == 0xFF) {
            break;
        }
        if (*q != 0x0F) {
            *complete_ok = 0;
            return 0;
        }
        len = be16(q + 4);
        if (q + 6 + len > qe) {
            *complete_ok = 0;
            return 0;
        }
        if (q[1] == 0x80) {
            has_term = 1;
        }
        q += 6 + len;
    }
    return has_term;
}

static int pgs_has_end(const uint8_t *data, size_t size, int *complete_ok)
{
    const uint8_t *q = data, *qe = data + size;
    int has_term = 0;
    *complete_ok = 1;
    if (size >= 13 && data[0] == 'P' && data[1] == 'G') {
        while (q + 13 <= qe && q[0] == 'P' && q[1] == 'G') {
            uint16_t len = be16(q + 11);
            if (q + 13 + len > qe) {
                *complete_ok = 0;
                return 0;
            }
            if (q[10] == 0x80) {
                has_term = 1;
            }
            q += 13 + len;
        }
        return has_term;
    }
    while (q + 3 <= qe) {
        uint16_t len;
        if (q[0] == 0xFF) {
            break;
        }
        len = be16(q + 1);
        if (q + 3 + len > qe) {
            *complete_ok = 0;
            return 0;
        }
        if (q[0] == 0x80) {
            has_term = 1;
        }
        q += 3 + len;
    }
    return has_term;
}

static int dvb_packet_is_self_contained(const uint8_t *data, size_t size)
{
    int complete_ok = 0;
    const uint8_t *q;
    size_t n;
    if (data == NULL || size == 0 || !dvb_has_end(data, size, &complete_ok)) {
        return 0;
    }
    q = data;
    n = size;
    dvb_skip_pes_wrapper(&q, &n);
    /* 只有以 page composition 开头且含结束段的包才是独立显示集。
     * 仅含 region/object/END 的续 PES 不能丢掉 acc 里的页状态。 */
    if (n < 6 || q[0] != 0x0F || q[1] != 0x10) {
        return 0;
    }
    return 1;
}

static int pgs_packet_is_self_contained(const uint8_t *data, size_t size)
{
    int complete_ok = 0;
    if (data == NULL || size == 0 || !pgs_has_end(data, size, &complete_ok)) {
        return 0;
    }
    if (size >= 13 && data[0] == 'P' && data[1] == 'G') {
        return data[10] == 0x16;
    }
    return data[0] == 0x16;
}

static int bmp_has_display_end(ms_sub_bitmap_kind kind, const uint8_t *data,
                               size_t size, int *complete_ok)
{
    if (kind == MS_SUB_BITMAP_DVB) {
        return dvb_has_end(data, size, complete_ok);
    }
    return pgs_has_end(data, size, complete_ok);
}

ms_status ms_sub_doc_feed_bitmap(ms_sub_doc *doc, ms_sub_bitmap_kind kind,
                                 uint32_t track_id,
                                 const uint8_t *data, size_t size,
                                 int has_start, uint64_t start_90k,
                                 int has_end, uint64_t end_90k)
{
    ms_sub_bmp_state *st;
    ms_status stt;
    int completed = 0, complete_ok = 0;
    const uint8_t *use;
    size_t use_n;
    int use_has_start, use_has_end;
    uint64_t use_start, use_end;
    if (doc == NULL || (data == NULL && size != 0)) {
        return MS_ERR_INVALID_ARG;
    }
    if (kind != MS_SUB_BITMAP_DVB && kind != MS_SUB_BITMAP_PGS &&
        kind != MS_SUB_BITMAP_VOBSUB) {
        return MS_ERR_UNSUPPORTED;
    }
    if (size > MS_SUB_BITMAP_ACC_MAX) {
        if (doc->bmp_state != NULL) {
            bmp_acc_reset_meta((ms_sub_bmp_state *)doc->bmp_state);
        }
        return MS_ERR_OVERFLOW;
    }
    stt = bmp_state_ensure(doc, &st);
    if (stt != MS_OK) {
        return stt;
    }
    if (kind == MS_SUB_BITMAP_VOBSUB) {
        /* 内嵌无 idx 调色板的裸包不能发明颜色。 */
        return MS_ERR_UNSUPPORTED;
    }
    /* 切轨：丢弃另一轨残留，避免串帧。 */
    if (st->acc_len > 0 && st->acc_track_id != 0 && st->acc_track_id != track_id) {
        bmp_acc_reset_meta(st);
    }
    /* 新的完整显示集到来时丢掉半截旧集，不把两集拼在一起。 */
    if (st->acc_len > 0) {
        if (kind == MS_SUB_BITMAP_DVB && dvb_packet_is_self_contained(data, size)) {
            bmp_acc_reset_meta(st);
        } else if (kind == MS_SUB_BITMAP_PGS &&
                   pgs_packet_is_self_contained(data, size)) {
            bmp_acc_reset_meta(st);
        }
    }
    use = data;
    use_n = size;
    use_has_start = has_start;
    use_start = start_90k;
    use_has_end = has_end;
    use_end = end_90k;
    /*
     * lavf/mpegts 的契约：一个 AVPacket 对应一个 PES。DVB 一个 PES 可跨多个
     * TS 包并带 0x20 0x00 前缀；PGS 常见「一段一个 PES」。若 END 单独成包，
     * 必须先并入 acc，不能把 END 当完整集解析而丢掉前面的 PCS/ODS。
     */
    if (st->acc_len > 0 || !bmp_has_display_end(kind, data, size, &complete_ok)) {
        const uint8_t *p = data;
        size_t n = size;
        if (kind == MS_SUB_BITMAP_DVB && st->acc_len > 0) {
            bmp_acc_trim_ff(st);
            dvb_skip_pes_wrapper(&p, &n);
        }
        if (st->acc_len == 0) {
            st->acc_kind = (int)kind;
            st->acc_track_id = track_id;
            st->acc_has_start = has_start;
            st->acc_start_90k = start_90k;
            st->acc_has_end = has_end;
            st->acc_end_90k = end_90k;
        } else if (has_end && (!st->acc_has_end || end_90k > st->acc_end_90k)) {
            st->acc_has_end = 1;
            st->acc_end_90k = end_90k;
        }
        stt = acc_append(doc, st, p, n);
        if (stt != MS_OK) {
            bmp_acc_reset_meta(st);
            return stt;
        }
        if (!bmp_has_display_end(kind, st->acc, st->acc_len, &complete_ok)) {
            return MS_OK;
        }
        use = st->acc;
        use_n = st->acc_len;
        use_has_start = st->acc_has_start;
        use_start = st->acc_start_90k;
        use_has_end = st->acc_has_end;
        use_end = st->acc_end_90k;
    }
    if (kind == MS_SUB_BITMAP_DVB) {
        stt = dvb_parse_segments(doc, st, use, use_n, track_id,
                                 use_has_start, use_start, use_has_end, use_end,
                                 &completed);
        bmp_acc_reset_meta(st);
        if (completed && stt == MS_OK) {
            doc->fmt = MS_SUB_FORMAT_DVB;
        }
        return stt;
    }
    stt = pgs_parse_segments(doc, st, use, use_n, track_id,
                             use_has_start, use_start, use_has_end, use_end,
                             &completed);
    bmp_acc_reset_meta(st);
    if (completed && stt == MS_OK) {
        doc->fmt = MS_SUB_FORMAT_PGS;
    }
    return stt;
}

ms_status ms_sub_doc_parse_vobsub(ms_sub_doc *doc,
                                  const uint8_t *idx, size_t idx_size,
                                  const uint8_t *sub, size_t sub_size,
                                  uint32_t track_id)
{
    if (doc == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    ms_sub_doc_clear(doc);
    return parse_idx_and_subs(doc, idx, idx_size, sub, sub_size, track_id);
}

static ms_status parse_vobsub_blob(ms_sub_doc *doc, const uint8_t *data, size_t size,
                                   uint32_t track_id)
{
    uint32_t idx_len;
    if (data == NULL || size < 8) {
        return MS_ERR_MALFORMED;
    }
    if (!(data[0] == 'M' && data[1] == 'S' && data[2] == 'V' && data[3] == 'B')) {
        return MS_ERR_MALFORMED;
    }
    idx_len = (uint32_t)data[4] | ((uint32_t)data[5] << 8) |
              ((uint32_t)data[6] << 16) | ((uint32_t)data[7] << 24);
    if ((size_t)8u + idx_len > size) {
        return MS_ERR_MALFORMED;
    }
    return ms_sub_doc_parse_vobsub(doc, data + 8, idx_len,
                                   data + 8 + idx_len, size - 8 - idx_len, track_id);
}
