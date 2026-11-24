/*
 * 有界字幕解析与时间轴（SRT / WebVTT / tx3g 纯文本，ASS/SSA 布局模型，
 * 以及 DVB / PGS / VobSub 位图到有界 RGBA cue）。
 * 不做字体栅格或完整 ASS 合成，不调用 FFmpeg 解码器，不执行脚本或附件。
 * by: Long <mycjzb@gmail.com>
 */

#include "msmedia/msmedia.h"
#include <stddef.h>

#include <stdlib.h>
#include <string.h>

typedef struct ms_sub_item {
    uint32_t track_id;
    int has_start;
    int has_end;
    uint64_t start_90k;
    uint64_t end_90k;
    size_t text_off;
    size_t text_size;
    uint32_t style_flags;
    uint32_t cap_flags;
    int layer;
    int alignment;
    int font_size;
    uint32_t color_argb;
    int bold;
    int italic;
    int margin_l, margin_r, margin_v;
    int pos_x, pos_y;
    int play_res_x, play_res_y;
    char style_name[MS_SUB_STYLE_NAME_MAX];
    uint32_t span_count;
    ms_sub_span spans[MS_SUB_SPAN_MAX];
    int bitmap_kind;
    int bitmap_w, bitmap_h;
    int crop_x, crop_y, crop_w, crop_h;
    size_t bitmap_off;
    size_t bitmap_size;
    uint32_t ai_utterance_id;
    uint32_t ai_revision;
    uint32_t ai_epoch;
    int ai_kind;
    int64_t ai_delay_90k;
    char ai_lang[16];
} ms_sub_item;

#ifndef MS_SUB_STYLE_MAX
#define MS_SUB_STYLE_MAX 64
#endif
#ifndef MS_SUB_FMT_COLS_MAX
#define MS_SUB_FMT_COLS_MAX 32
#endif
#ifndef MS_SUB_COL_NAME_MAX
#define MS_SUB_COL_NAME_MAX 24
#endif

typedef struct ms_ass_style {
    char name[MS_SUB_STYLE_NAME_MAX];
    int font_size;
    uint32_t color_argb;
    int bold;
    int italic;
    int alignment;
    int margin_l, margin_r, margin_v;
    int used;
} ms_ass_style;
#define MS_ASS_STYLE_DEFINED 1

struct ms_sub_doc {
    ms_allocator alloc;
    int using_default;
    ms_sub_item *items;
    size_t nitems;
    size_t cap_items;
    char *text;
    size_t text_used;
    size_t text_cap;
    ms_sub_format fmt;
    int is_ssa;
    int play_res_x;
    int play_res_y;
    size_t nstyles;
    ms_ass_style styles[MS_SUB_STYLE_MAX];
    char event_cols[MS_SUB_FMT_COLS_MAX][MS_SUB_COL_NAME_MAX];
    int event_ncols;
    /* 位图 RGBA arena 与解码器状态（seek/clear_cues 时释放）。 */
    uint8_t *bitmaps;
    size_t bitmap_used;
    size_t bitmap_cap;
    void *bmp_state; /* ms_sub_bmp_state，在 ms_sub_bitmap.c 中分配 */
};

static void *dflt_alloc(void *ctx, size_t n)
{
    (void)ctx;
    return malloc(n);
}

static void dflt_free(void *ctx, void *p)
{
    (void)ctx;
    free(p);
}

static const ms_allocator k_dflt = { dflt_alloc, dflt_free, NULL };

void ms_sub_cue_default(ms_sub_cue *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->struct_size = sizeof(*out);
}

ms_status ms_sub_doc_create(const ms_allocator *alloc, ms_sub_doc **out)
{
    ms_sub_doc *d;
    if (out == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    *out = NULL;
    if (alloc == NULL) {
        alloc = &k_dflt;
    }
    if (alloc->alloc_fn == NULL || alloc->free_fn == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    d = (ms_sub_doc *)alloc->alloc_fn(alloc->ctx, sizeof(*d));
    if (d == NULL) {
        return MS_ERR_NOMEM;
    }
    memset(d, 0, sizeof(*d));
    d->alloc = *alloc;
    d->using_default = (alloc == &k_dflt);
    *out = d;
    return MS_OK;
}

static void clear_style_table(ms_sub_doc *doc)
{
    if (doc == NULL) {
        return;
    }
    memset(doc->styles, 0, sizeof(doc->styles));
    doc->nstyles = 0;
    doc->play_res_x = 0;
    doc->play_res_y = 0;
    doc->is_ssa = 0;
    doc->fmt = MS_SUB_FORMAT_AUTO;
    memset(doc->event_cols, 0, sizeof(doc->event_cols));
    doc->event_ncols = 0;
}

static void ms_sub_bmp_state_reset(ms_sub_doc *doc);
static void ms_sub_bmp_state_free(ms_sub_doc *doc);
static ms_status parse_vobsub_blob(ms_sub_doc *doc, const uint8_t *data, size_t size,
                                   uint32_t track_id);

void ms_sub_doc_clear_cues(ms_sub_doc *doc)
{
    if (doc == NULL) {
        return;
    }
    if (doc->items != NULL) {
        doc->alloc.free_fn(doc->alloc.ctx, doc->items);
    }
    if (doc->text != NULL) {
        doc->alloc.free_fn(doc->alloc.ctx, doc->text);
    }
    if (doc->bitmaps != NULL) {
        doc->alloc.free_fn(doc->alloc.ctx, doc->bitmaps);
    }
    doc->items = NULL;
    doc->nitems = 0;
    doc->cap_items = 0;
    doc->text = NULL;
    doc->text_used = 0;
    doc->text_cap = 0;
    doc->bitmaps = NULL;
    doc->bitmap_used = 0;
    doc->bitmap_cap = 0;
    ms_sub_bmp_state_reset(doc);
}

void ms_sub_doc_clear(ms_sub_doc *doc)
{
    if (doc == NULL) {
        return;
    }
    ms_sub_doc_clear_cues(doc);
    clear_style_table(doc);
}

ms_sub_format ms_sub_doc_format(const ms_sub_doc *doc)
{
    return doc != NULL ? doc->fmt : MS_SUB_FORMAT_AUTO;
}

ms_codec ms_sub_codec_for_format(ms_sub_format fmt)
{
    switch (fmt) {
    case MS_SUB_FORMAT_SRT:
        return MS_CODEC_SUB_SRT;
    case MS_SUB_FORMAT_WEBVTT:
        return MS_CODEC_SUB_WEBVTT;
    case MS_SUB_FORMAT_ASS:
    case MS_SUB_FORMAT_SSA:
        return MS_CODEC_SUB_ASS;
    case MS_SUB_FORMAT_DVB:
        return MS_CODEC_SUB_DVB;
    case MS_SUB_FORMAT_PGS:
        return MS_CODEC_SUB_PGS;
    case MS_SUB_FORMAT_VOBSUB:
        return MS_CODEC_SUB_VOBSUB;
    default:
        return MS_CODEC_SUB_TEXT;
    }
}

void ms_sub_doc_destroy(ms_sub_doc *doc)
{
    ms_allocator a;
    if (doc == NULL) {
        return;
    }
    a = doc->alloc;
    ms_sub_doc_clear(doc);
    ms_sub_bmp_state_free(doc);
    a.free_fn(a.ctx, doc);
}

size_t ms_sub_doc_count(const ms_sub_doc *doc)
{
    return doc != NULL ? doc->nitems : 0;
}

static void fill_cue(const ms_sub_doc *doc, const ms_sub_item *it, ms_sub_cue *out)
{
    size_t sz = out->struct_size;
    if (sz == 0 || sz > sizeof(*out)) {
        sz = sizeof(*out);
    }
    memset(out, 0, sz);
    if (sz >= offsetof(ms_sub_cue, struct_size) + sizeof(size_t)) {
        out->struct_size = sz;
    }
    if (sz >= offsetof(ms_sub_cue, track_id) + sizeof(uint32_t)) {
        out->track_id = it->track_id;
    }
    if (sz >= offsetof(ms_sub_cue, has_start) + sizeof(int)) {
        out->has_start = it->has_start;
    }
    if (sz >= offsetof(ms_sub_cue, has_end) + sizeof(int)) {
        out->has_end = it->has_end;
    }
    if (sz >= offsetof(ms_sub_cue, start_90k) + sizeof(uint64_t)) {
        out->start_90k = it->start_90k;
    }
    if (sz >= offsetof(ms_sub_cue, end_90k) + sizeof(uint64_t)) {
        out->end_90k = it->end_90k;
    }
    if (sz >= offsetof(ms_sub_cue, text) + sizeof(const char *)) {
        out->text = (doc->text != NULL) ? (doc->text + it->text_off) : "";
    }
    if (sz >= offsetof(ms_sub_cue, text_size) + sizeof(size_t)) {
        out->text_size = it->text_size;
    }
    if (sz >= offsetof(ms_sub_cue, style_flags) + sizeof(uint32_t)) {
        out->style_flags = it->style_flags;
    }
    if (sz >= offsetof(ms_sub_cue, cap_flags) + sizeof(uint32_t)) {
        out->cap_flags = it->cap_flags;
    }
    if (sz >= offsetof(ms_sub_cue, layer) + sizeof(int)) {
        out->layer = it->layer;
    }
    if (sz >= offsetof(ms_sub_cue, alignment) + sizeof(int)) {
        out->alignment = it->alignment;
    }
    if (sz >= offsetof(ms_sub_cue, font_size) + sizeof(int)) {
        out->font_size = it->font_size;
    }
    if (sz >= offsetof(ms_sub_cue, color_argb) + sizeof(uint32_t)) {
        out->color_argb = it->color_argb;
    }
    if (sz >= offsetof(ms_sub_cue, bold) + sizeof(int)) {
        out->bold = it->bold;
    }
    if (sz >= offsetof(ms_sub_cue, italic) + sizeof(int)) {
        out->italic = it->italic;
    }
    if (sz >= offsetof(ms_sub_cue, margin_l) + sizeof(int)) {
        out->margin_l = it->margin_l;
    }
    if (sz >= offsetof(ms_sub_cue, margin_r) + sizeof(int)) {
        out->margin_r = it->margin_r;
    }
    if (sz >= offsetof(ms_sub_cue, margin_v) + sizeof(int)) {
        out->margin_v = it->margin_v;
    }
    if (sz >= offsetof(ms_sub_cue, pos_x) + sizeof(int)) {
        out->pos_x = it->pos_x;
    }
    if (sz >= offsetof(ms_sub_cue, pos_y) + sizeof(int)) {
        out->pos_y = it->pos_y;
    }
    if (sz >= offsetof(ms_sub_cue, play_res_x) + sizeof(int)) {
        out->play_res_x = it->play_res_x;
    }
    if (sz >= offsetof(ms_sub_cue, play_res_y) + sizeof(int)) {
        out->play_res_y = it->play_res_y;
    }
    if (sz >= offsetof(ms_sub_cue, style_name) + sizeof(out->style_name)) {
        memcpy(out->style_name, it->style_name, sizeof(out->style_name));
    }
    if (sz >= offsetof(ms_sub_cue, span_count) + sizeof(uint32_t)) {
        out->span_count = it->span_count;
        if (out->span_count > MS_SUB_SPAN_MAX) {
            out->span_count = MS_SUB_SPAN_MAX;
        }
    }
    if (sz >= offsetof(ms_sub_cue, spans) + sizeof(out->spans)) {
        memcpy(out->spans, it->spans, sizeof(out->spans));
    }
    if (sz >= offsetof(ms_sub_cue, bitmap_kind) + sizeof(int)) {
        out->bitmap_kind = it->bitmap_kind;
    }
    if (sz >= offsetof(ms_sub_cue, bitmap_w) + sizeof(int)) {
        out->bitmap_w = it->bitmap_w;
    }
    if (sz >= offsetof(ms_sub_cue, bitmap_h) + sizeof(int)) {
        out->bitmap_h = it->bitmap_h;
    }
    if (sz >= offsetof(ms_sub_cue, crop_x) + sizeof(int)) {
        out->crop_x = it->crop_x;
    }
    if (sz >= offsetof(ms_sub_cue, crop_y) + sizeof(int)) {
        out->crop_y = it->crop_y;
    }
    if (sz >= offsetof(ms_sub_cue, crop_w) + sizeof(int)) {
        out->crop_w = it->crop_w;
    }
    if (sz >= offsetof(ms_sub_cue, crop_h) + sizeof(int)) {
        out->crop_h = it->crop_h;
    }
    if (sz >= offsetof(ms_sub_cue, bitmap_rgba) + sizeof(const uint8_t *)) {
        if (it->bitmap_size > 0 && doc->bitmaps != NULL) {
            out->bitmap_rgba = doc->bitmaps + it->bitmap_off;
        } else {
            out->bitmap_rgba = NULL;
        }
    }
    if (sz >= offsetof(ms_sub_cue, bitmap_rgba_size) + sizeof(size_t)) {
        out->bitmap_rgba_size = it->bitmap_size;
    }
    if (sz >= offsetof(ms_sub_cue, ai_utterance_id) + sizeof(uint32_t)) {
        out->ai_utterance_id = it->ai_utterance_id;
    }
    if (sz >= offsetof(ms_sub_cue, ai_revision) + sizeof(uint32_t)) {
        out->ai_revision = it->ai_revision;
    }
    if (sz >= offsetof(ms_sub_cue, ai_epoch) + sizeof(uint32_t)) {
        out->ai_epoch = it->ai_epoch;
    }
    if (sz >= offsetof(ms_sub_cue, ai_kind) + sizeof(int)) {
        out->ai_kind = it->ai_kind;
    }
    if (sz >= offsetof(ms_sub_cue, ai_delay_90k) + sizeof(int64_t)) {
        out->ai_delay_90k = it->ai_delay_90k;
    }
    if (sz >= offsetof(ms_sub_cue, ai_lang) + sizeof(out->ai_lang)) {
        memcpy(out->ai_lang, it->ai_lang, sizeof(out->ai_lang));
    }
    /* 位图 cue 不得把 RGBA 字节暴露为 text。 */
    if (it->bitmap_size > 0 && it->text_size == 0) {
        if (sz >= offsetof(ms_sub_cue, text) + sizeof(const char *)) {
            out->text = "";
        }
        if (sz >= offsetof(ms_sub_cue, text_size) + sizeof(size_t)) {
            out->text_size = 0;
        }
    }
}

ms_status ms_sub_doc_get(const ms_sub_doc *doc, size_t index, ms_sub_cue *out)
{
    if (doc == NULL || out == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    if (index >= doc->nitems) {
        return MS_ERR_INVALID_ARG;
    }
    if (out->struct_size == 0) {
        out->struct_size = sizeof(*out);
    }
    fill_cue(doc, &doc->items[index], out);
    return MS_OK;
}

static int cue_active(const ms_sub_item *it, uint64_t t)
{
    if (!it->has_start) {
        return 0;
    }
    if (t < it->start_90k) {
        return 0;
    }
    if (it->has_end) {
        return t < it->end_90k;
    }
    return 1;
}

static int cue_sort_before(const ms_sub_item *a, const ms_sub_item *b, size_t ia, size_t ib)
{
    if (a->layer != b->layer) {
        return a->layer < b->layer;
    }
    if (a->start_90k != b->start_90k) {
        return a->start_90k < b->start_90k;
    }
    return ia < ib;
}

ms_status ms_sub_doc_cues_at(const ms_sub_doc *doc, uint64_t time_90k,
                             ms_sub_cue *out, size_t cap, size_t *count)
{
    size_t i, n = 0;
    size_t idx[MS_SUB_CUE_MAX];
    if (doc == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    for (i = 0; i < doc->nitems && n < MS_SUB_CUE_MAX; i++) {
        if (!cue_active(&doc->items[i], time_90k)) {
            continue;
        }
        idx[n++] = i;
    }
    /* 按 layer、start 稳定排序，供重叠 cue 自下而上绘制。 */
    for (i = 1; i < n; i++) {
        size_t v = idx[i];
        size_t j = i;
        while (j > 0 && cue_sort_before(&doc->items[v], &doc->items[idx[j - 1]],
                                        v, idx[j - 1])) {
            idx[j] = idx[j - 1];
            j--;
        }
        idx[j] = v;
    }
    if (out != NULL) {
        size_t k, w = 0;
        for (k = 0; k < n && w < cap; k++, w++) {
            ms_sub_cue *dst = &out[w];
            if (dst->struct_size == 0) {
                dst->struct_size = sizeof(*dst);
            }
            fill_cue(doc, &doc->items[idx[k]], dst);
        }
    }
    if (count != NULL) {
        *count = n;
    }
    (void)cap;
    return MS_OK;
}

static ms_status grow_items(ms_sub_doc *doc, size_t need)
{
    size_t ncap;
    ms_sub_item *ni;
    if (need <= doc->cap_items) {
        return MS_OK;
    }
    ncap = doc->cap_items == 0 ? 8 : doc->cap_items * 2;
    while (ncap < need) {
        ncap *= 2;
    }
    if (ncap > MS_SUB_CUE_MAX) {
        ncap = MS_SUB_CUE_MAX;
    }
    if (need > ncap) {
        return MS_ERR_OVERFLOW;
    }
    ni = (ms_sub_item *)doc->alloc.alloc_fn(doc->alloc.ctx, ncap * sizeof(*ni));
    if (ni == NULL) {
        return MS_ERR_NOMEM;
    }
    memset(ni, 0, ncap * sizeof(*ni));
    if (doc->items != NULL && doc->nitems > 0) {
        memcpy(ni, doc->items, doc->nitems * sizeof(*ni));
        doc->alloc.free_fn(doc->alloc.ctx, doc->items);
    }
    doc->items = ni;
    doc->cap_items = ncap;
    return MS_OK;
}

static ms_status grow_text(ms_sub_doc *doc, size_t add)
{
    size_t need, ncap;
    char *nt;
    if (add > MS_SUB_DOC_BYTES_MAX) {
        return MS_ERR_OVERFLOW;
    }
    need = doc->text_used + add;
    if (need > MS_SUB_DOC_BYTES_MAX) {
        return MS_ERR_OVERFLOW;
    }
    if (need <= doc->text_cap) {
        return MS_OK;
    }
    ncap = doc->text_cap == 0 ? 256 : doc->text_cap * 2;
    while (ncap < need) {
        ncap *= 2;
    }
    if (ncap > MS_SUB_DOC_BYTES_MAX) {
        ncap = MS_SUB_DOC_BYTES_MAX;
    }
    nt = (char *)doc->alloc.alloc_fn(doc->alloc.ctx, ncap);
    if (nt == NULL) {
        return MS_ERR_NOMEM;
    }
    memset(nt, 0, ncap);
    if (doc->text != NULL && doc->text_used > 0) {
        memcpy(nt, doc->text, doc->text_used);
        doc->alloc.free_fn(doc->alloc.ctx, doc->text);
    }
    doc->text = nt;
    doc->text_cap = ncap;
    return MS_OK;
}

static int is_utf8(const unsigned char *s, size_t n)
{
    size_t i = 0;
    while (i < n) {
        unsigned c = s[i];
        size_t need;
        if (c < 0x80) {
            i++;
            continue;
        }
        if ((c & 0xE0) == 0xC0) {
            need = 2;
        } else if ((c & 0xF0) == 0xE0) {
            need = 3;
        } else if ((c & 0xF8) == 0xF0) {
            need = 4;
        } else {
            return 0;
        }
        if (i + need > n) {
            return 0;
        }
        {
            size_t k;
            for (k = 1; k < need; k++) {
                if ((s[i + k] & 0xC0) != 0x80) {
                    return 0;
                }
            }
        }
        i += need;
    }
    return 1;
}

static size_t strip_simple_tags(const char *in, size_t n, char *out, size_t cap)
{
    size_t i = 0, o = 0;
    int in_tag = 0;
    for (i = 0; i < n; i++) {
        char c = in[i];
        if (c == '<') {
            in_tag = 1;
            continue;
        }
        if (c == '>' && in_tag) {
            in_tag = 0;
            continue;
        }
        if (in_tag) {
            continue;
        }
        if (o + 1 < cap) {
            out[o++] = c;
        } else if (cap > 0) {
            break;
        }
    }
    if (cap > 0) {
        out[o] = 0;
    }
    return o;
}

static int parse_hms(const char *s, size_t n, uint64_t *out_90k)
{
    /* 支持 [HH:]MM:SS[,.]mmm  以及 SS[,.]mmm */
    unsigned h = 0, m = 0, sec = 0, ms = 0;
    int fields = 0;
    size_t i = 0, start;
    unsigned parts[4];
    int nparts = 0;
    int ms_digits = 0;
    if (s == NULL || n == 0 || out_90k == NULL) {
        return -1;
    }
    while (i < n && (s[i] == ' ' || s[i] == '\t')) {
        i++;
    }
    start = i;
    parts[0] = 0;
    while (i < n && nparts < 4) {
        char c = s[i];
        if (c >= '0' && c <= '9') {
            parts[nparts] = parts[nparts] * 10u + (unsigned)(c - '0');
            if (nparts == 3) {
                ms_digits++;
            }
            i++;
            continue;
        }
        if ((c == ':' || c == ',' || c == '.') && nparts < 3) {
            nparts++;
            parts[nparts] = 0;
            i++;
            if (c == ',' || c == '.') {
                /* 强制进入毫秒段 */
                while (nparts < 3) {
                    nparts++;
                    parts[nparts] = 0;
                }
            }
            continue;
        }
        break;
    }
    if (i == start) {
        return -1;
    }
    nparts++;
    if (nparts == 1) {
        sec = parts[0];
    } else if (nparts == 2) {
        /* SS,mmm or MM:SS ? if separator was comma we already padded */
        m = 0;
        sec = parts[0];
        ms = parts[1];
        ms_digits = 0; /* already in parts[1] as number; pad below if needed */
    } else if (nparts == 3) {
        m = parts[0];
        sec = parts[1];
        ms = parts[2];
    } else {
        h = parts[0];
        m = parts[1];
        sec = parts[2];
        ms = parts[3];
    }
    /* 毫秒不足 3 位则按左对齐理解：1 -> 100, 12 -> 120 */
    if (ms > 0 && ms < 100 && ms_digits > 0 && ms_digits < 3) {
        while (ms_digits < 3) {
            ms *= 10u;
            ms_digits++;
        }
    } else if (ms > 0 && ms < 100 && nparts >= 2 && ms_digits == 0) {
        /* parts already combined; if user wrote ",5" treat as 500 if <10, ",50" as 500 if <100? 
           Safer: if ms<10 -> *100; else if ms<100 -> *10 */
        if (ms < 10) {
            ms *= 100u;
        } else if (ms < 100) {
            ms *= 10u;
        }
    }
    if (m >= 60 || sec >= 60 || ms >= 1000) {
        return -1;
    }
    {
        uint64_t total_ms = ((uint64_t)h * 3600ull + (uint64_t)m * 60ull + (uint64_t)sec) * 1000ull + (uint64_t)ms;
        /* 90 kHz = ms * 90 */
        if (total_ms > (UINT64_MAX / 90ull)) {
            return -1;
        }
        *out_90k = total_ms * 90ull;
    }
    (void)fields;
    return (int)(i - start);
}

static const char *find_arrow(const char *s, size_t n, size_t *arrow_len)
{
    size_t i;
    for (i = 0; i + 2 < n; i++) {
        if (s[i] == '-' && s[i + 1] == '-' && s[i + 2] == '>') {
            *arrow_len = 3;
            return s + i;
        }
    }
    return NULL;
}

static int parse_times(const char *line, size_t n, uint64_t *a, uint64_t *b)
{
    const char *arr;
    size_t alen = 0;
    int used;
    arr = find_arrow(line, n, &alen);
    if (arr == NULL) {
        return -1;
    }
    used = parse_hms(line, (size_t)(arr - line), a);
    if (used < 0) {
        return -1;
    }
    used = parse_hms(arr + alen, n - (size_t)(arr + alen - line), b);
    if (used < 0) {
        return -1;
    }
    return 0;
}

static int is_all_digits(const char *s, size_t n)
{
    size_t i;
    if (n == 0) {
        return 0;
    }
    for (i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9') {
            return 0;
        }
    }
    return 1;
}

static int starts_with_ci(const char *s, size_t n, const char *pfx)
{
    size_t i, m = strlen(pfx);
    if (n < m) {
        return 0;
    }
    for (i = 0; i < m; i++) {
        char a = s[i], b = pfx[i];
        if (a >= 'a' && a <= 'z') {
            a = (char)(a - 'a' + 'A');
        }
        if (b >= 'a' && b <= 'z') {
            b = (char)(b - 'a' + 'A');
        }
        if (a != b) {
            return 0;
        }
    }
    return 1;
}

static void skip_bom(const uint8_t **data, size_t *size)
{
    if (data == NULL || *data == NULL || size == NULL) {
        return;
    }
    if (*size >= 3 && (*data)[0] == 0xEF && (*data)[1] == 0xBB && (*data)[2] == 0xBF) {
        *data += 3;
        *size -= 3;
    }
}

typedef struct {
    const char *p;
    size_t n;
    size_t off;
} line_iter;

static int next_line(line_iter *it, const char **line, size_t *len, int *eol_blank)
{
    size_t i, start;
    if (it->off >= it->n) {
        return 0;
    }
    start = it->off;
    i = start;
    while (i < it->n && it->p[i] != '\n' && it->p[i] != '\r') {
        i++;
    }
    *line = it->p + start;
    *len = i - start;
    *eol_blank = (*len == 0);
    if (i < it->n && it->p[i] == '\r') {
        i++;
    }
    if (i < it->n && it->p[i] == '\n') {
        i++;
    }
    it->off = i;
    return 1;
}

static int cue_text_same(const ms_sub_doc *doc, const ms_sub_item *it,
                         const char *text, size_t tlen)
{
    if (it->text_size != tlen) {
        return 0;
    }
    if (tlen == 0) {
        return 1;
    }
    return memcmp(doc->text + it->text_off, text, tlen) == 0;
}

/* 跨 HLS 段或直播重载会出现同一 track/起止/文本的重复 cue；视为已在时间轴上。 */
static int cue_already_present(const ms_sub_doc *doc, uint32_t track_id,
                               int has_start, uint64_t start_90k,
                               int has_end, uint64_t end_90k,
                               const char *text, size_t tlen)
{
    size_t i;
    for (i = 0; i < doc->nitems; i++) {
        const ms_sub_item *it = &doc->items[i];
        if (it->track_id != track_id) {
            continue;
        }
        if (it->has_start != has_start || it->start_90k != start_90k) {
            continue;
        }
        if (it->has_end != has_end) {
            continue;
        }
        if (it->has_end && it->end_90k != end_90k) {
            continue;
        }
        if (it->bitmap_kind != 0) {
            continue;
        }
        if (cue_text_same(doc, it, text, tlen)) {
            return 1;
        }
    }
    return 0;
}

static ms_status add_parsed_cue(ms_sub_doc *doc, uint32_t track_id,
                                uint64_t a, uint64_t b, const char *text, size_t tlen)
{
    char tmp[MS_SUB_CUE_TEXT_MAX + 1];
    size_t out_n;
    ms_status st;
    ms_sub_item *it;
    if (tlen == 0) {
        return MS_OK; /* 跳过空 cue */
    }
    out_n = strip_simple_tags(text, tlen, tmp, sizeof(tmp));
    /* 去掉尾部空白 */
    while (out_n > 0 && (tmp[out_n - 1] == ' ' || tmp[out_n - 1] == '\t' ||
                         tmp[out_n - 1] == '\n' || tmp[out_n - 1] == '\r')) {
        out_n--;
        tmp[out_n] = 0;
    }
    if (out_n == 0) {
        return MS_OK;
    }
    if (out_n > MS_SUB_CUE_TEXT_MAX) {
        return MS_ERR_OVERFLOW;
    }
    if (!is_utf8((const unsigned char *)tmp, out_n)) {
        return MS_ERR_MALFORMED;
    }
    if (cue_already_present(doc, track_id, 1, a, 1, (b > a) ? b : a, tmp, out_n)) {
        return MS_OK;
    }
    st = grow_items(doc, doc->nitems + 1);
    if (st != MS_OK) {
        return st;
    }
    st = grow_text(doc, out_n + 1);
    if (st != MS_OK) {
        return st;
    }
    it = &doc->items[doc->nitems];
    memset(it, 0, sizeof(*it));
    it->track_id = track_id;
    it->has_start = 1;
    it->has_end = 1;
    it->start_90k = a;
    it->end_90k = (b > a) ? b : a;
    it->text_off = doc->text_used;
    it->text_size = out_n;
    memcpy(doc->text + doc->text_used, tmp, out_n);
    doc->text_used += out_n;
    doc->text[doc->text_used] = 0;
    doc->text_used += 1;
    doc->nitems++;
    return MS_OK;
}

static ms_status parse_body(ms_sub_doc *doc, const char *p, size_t n,
                            int is_vtt, uint32_t track_id, size_t *accepted)
{
    line_iter it;
    const char *line;
    size_t len;
    int blank;
    uint64_t a, b;
    char body[MS_SUB_CUE_TEXT_MAX * 2];
    size_t body_n = 0;
    int have_time = 0;
    int skip_block = 0;
    ms_status st;
    *accepted = 0;
    it.p = p;
    it.n = n;
    it.off = 0;
    while (next_line(&it, &line, &len, &blank)) {
        if (skip_block) {
            if (blank) {
                skip_block = 0;
            }
            continue;
        }
        if (!have_time) {
            if (blank) {
                continue;
            }
            if (is_vtt && (starts_with_ci(line, len, "NOTE") ||
                           starts_with_ci(line, len, "STYLE") ||
                           starts_with_ci(line, len, "REGION"))) {
                skip_block = 1;
                continue;
            }
            if (parse_times(line, len, &a, &b) == 0) {
                have_time = 1;
                body_n = 0;
                continue;
            }
            /* SRT 序号或 VTT cue id：看下一行是否是时间 */
            if (is_all_digits(line, len) || !is_vtt || len < 32) {
                /* 保留为可能的 id，时间在下一行 */
                const char *l2;
                size_t n2;
                int b2;
                size_t save = it.off;
                if (next_line(&it, &l2, &n2, &b2) && !b2 && parse_times(l2, n2, &a, &b) == 0) {
                    have_time = 1;
                    body_n = 0;
                    continue;
                }
                it.off = save;
            }
            /* 无法识别的行：跳过 */
            continue;
        }
        if (blank) {
            st = add_parsed_cue(doc, track_id, a, b, body, body_n);
            if (st == MS_ERR_OVERFLOW || st == MS_ERR_NOMEM) {
                return st;
            }
            if (st == MS_OK && body_n > 0) {
                (*accepted)++;
            }
            have_time = 0;
            body_n = 0;
            continue;
        }
        if (body_n > 0 && body_n + 1 < sizeof(body)) {
            body[body_n++] = '\n';
        }
        if (body_n + len >= sizeof(body)) {
            return MS_ERR_OVERFLOW;
        }
        memcpy(body + body_n, line, len);
        body_n += len;
    }
    if (have_time) {
        st = add_parsed_cue(doc, track_id, a, b, body, body_n);
        if (st == MS_ERR_OVERFLOW || st == MS_ERR_NOMEM) {
            return st;
        }
        if (st == MS_OK && body_n > 0) {
            (*accepted)++;
        }
    }
    return MS_OK;
}

static int looks_like_vtt(const char *p, size_t n)
{
    size_t i = 0;
    while (i < n && (p[i] == ' ' || p[i] == '\t' || p[i] == '\r' || p[i] == '\n')) {
        i++;
    }
    return starts_with_ci(p + i, n - i, "WEBVTT");
}

static int looks_like_ass(const char *p, size_t n);
static ms_status parse_ass_file(ms_sub_doc *doc, const uint8_t *data, size_t size,
                                ms_sub_format fmt, uint32_t track_id, size_t *accepted);

ms_status ms_sub_doc_parse(ms_sub_doc *doc, const uint8_t *data, size_t size,
                           ms_sub_format fmt, uint32_t track_id)
{
    const uint8_t *p = data;
    size_t n = size;
    int is_vtt = 0;
    size_t accepted = 0;
    ms_status st;
    if (doc == NULL || (size > 0 && data == NULL)) {
        return MS_ERR_INVALID_ARG;
    }
    ms_sub_doc_clear(doc);
    skip_bom(&p, &n);
    if (n == 0) {
        return MS_ERR_MALFORMED;
    }
    if (fmt == MS_SUB_FORMAT_VOBSUB ||
        (fmt == MS_SUB_FORMAT_AUTO && n >= 4 &&
         p[0] == 'M' && p[1] == 'S' && p[2] == 'V' && p[3] == 'B')) {
        return parse_vobsub_blob(doc, p, n, track_id);
    }
    if (fmt == MS_SUB_FORMAT_DVB || fmt == MS_SUB_FORMAT_PGS ||
        (fmt == MS_SUB_FORMAT_AUTO && n >= 13 && p[0] == 'P' && p[1] == 'G')) {
        ms_sub_bitmap_kind kind = MS_SUB_BITMAP_PGS;
        if (fmt == MS_SUB_FORMAT_DVB) {
            kind = MS_SUB_BITMAP_DVB;
        } else if (fmt == MS_SUB_FORMAT_PGS || (n >= 2 && p[0] == 'P' && p[1] == 'G')) {
            kind = MS_SUB_BITMAP_PGS;
        }
        return ms_sub_doc_feed_bitmap(doc, kind, track_id, p, n, 1, 0, 0, 0);
    }
    if (fmt == MS_SUB_FORMAT_WEBVTT) {
        is_vtt = 1;
        if (!looks_like_vtt((const char *)p, n)) {
            return MS_ERR_MALFORMED;
        }
    } else if (fmt == MS_SUB_FORMAT_SRT) {
        is_vtt = 0;
    } else if (fmt == MS_SUB_FORMAT_ASS || fmt == MS_SUB_FORMAT_SSA ||
               (fmt == MS_SUB_FORMAT_AUTO && looks_like_ass((const char *)p, n))) {
        size_t accepted_ass = 0;
        if (fmt != MS_SUB_FORMAT_AUTO && !looks_like_ass((const char *)p, n)) {
            return MS_ERR_MALFORMED;
        }
        st = parse_ass_file(doc, p, n, fmt, track_id, &accepted_ass);
        if (st != MS_OK) {
            ms_sub_doc_clear(doc);
            return st;
        }
        if (accepted_ass == 0) {
            ms_sub_doc_clear(doc);
            return MS_ERR_MALFORMED;
        }
        return MS_OK;
    } else {
        is_vtt = looks_like_vtt((const char *)p, n);
    }
    if (is_vtt) {
        /* 跳过 WEBVTT 头行及其后可选说明，直到空行或第一段 cue */
        line_iter it;
        const char *line;
        size_t len;
        int blank;
        it.p = (const char *)p;
        it.n = n;
        it.off = 0;
        if (next_line(&it, &line, &len, &blank)) {
            /* consume header */
        }
        /* 若下一行不是时间且不是空行，跳过直到空行 */
        {
            size_t save = it.off;
            if (next_line(&it, &line, &len, &blank) && !blank) {
                uint64_t a, b;
                if (parse_times(line, len, &a, &b) != 0 && !is_all_digits(line, len) &&
                    !starts_with_ci(line, len, "NOTE") &&
                    !starts_with_ci(line, len, "STYLE") &&
                    !starts_with_ci(line, len, "REGION")) {
                    /* header 描述行，找到空行 */
                    while (next_line(&it, &line, &len, &blank) && !blank) {
                    }
                } else {
                    it.off = save;
                }
            } else if (!blank) {
                it.off = save;
            }
        }
        p = (const uint8_t *)(it.p + it.off);
        n = it.n - it.off;
    }
    st = parse_body(doc, (const char *)p, n, is_vtt, track_id, &accepted);
    if (st != MS_OK) {
        ms_sub_doc_clear(doc);
        return st;
    }
    if (accepted == 0) {
        ms_sub_doc_clear(doc);
        return MS_ERR_MALFORMED;
    }
    doc->fmt = is_vtt ? MS_SUB_FORMAT_WEBVTT : MS_SUB_FORMAT_SRT;
    return MS_OK;
}

static int cue_prefix_has_style(const ms_sub_cue *in)
{
    if (in->struct_size < offsetof(ms_sub_cue, style_flags) + sizeof(uint32_t)) {
        return 0;
    }
    if (in->style_flags != 0) {
        return 1;
    }
    if (in->struct_size >= offsetof(ms_sub_cue, cap_flags) + sizeof(uint32_t) &&
        in->cap_flags != 0) {
        return 1;
    }
    if (in->struct_size >= offsetof(ms_sub_cue, bitmap_rgba_size) + sizeof(size_t) &&
        in->bitmap_rgba != NULL && in->bitmap_rgba_size > 0) {
        return 1;
    }
    return 0;
}

static void copy_style_to_item(ms_sub_item *it, const ms_sub_cue *in)
{
    if (!cue_prefix_has_style(in)) {
        return;
    }
    it->style_flags = in->style_flags;
    if (in->struct_size >= offsetof(ms_sub_cue, cap_flags) + sizeof(uint32_t)) {
        it->cap_flags = in->cap_flags;
    }
    if (in->struct_size >= offsetof(ms_sub_cue, layer) + sizeof(int)) {
        it->layer = in->layer;
    }
    if (in->struct_size >= offsetof(ms_sub_cue, alignment) + sizeof(int)) {
        it->alignment = in->alignment;
    }
    if (in->struct_size >= offsetof(ms_sub_cue, font_size) + sizeof(int)) {
        it->font_size = in->font_size;
    }
    if (in->struct_size >= offsetof(ms_sub_cue, color_argb) + sizeof(uint32_t)) {
        it->color_argb = in->color_argb;
    }
    if (in->struct_size >= offsetof(ms_sub_cue, bold) + sizeof(int)) {
        it->bold = in->bold;
    }
    if (in->struct_size >= offsetof(ms_sub_cue, italic) + sizeof(int)) {
        it->italic = in->italic;
    }
    if (in->struct_size >= offsetof(ms_sub_cue, margin_l) + sizeof(int)) {
        it->margin_l = in->margin_l;
    }
    if (in->struct_size >= offsetof(ms_sub_cue, margin_r) + sizeof(int)) {
        it->margin_r = in->margin_r;
    }
    if (in->struct_size >= offsetof(ms_sub_cue, margin_v) + sizeof(int)) {
        it->margin_v = in->margin_v;
    }
    if (in->struct_size >= offsetof(ms_sub_cue, pos_x) + sizeof(int)) {
        it->pos_x = in->pos_x;
    }
    if (in->struct_size >= offsetof(ms_sub_cue, pos_y) + sizeof(int)) {
        it->pos_y = in->pos_y;
    }
    if (in->struct_size >= offsetof(ms_sub_cue, play_res_x) + sizeof(int)) {
        it->play_res_x = in->play_res_x;
    }
    if (in->struct_size >= offsetof(ms_sub_cue, play_res_y) + sizeof(int)) {
        it->play_res_y = in->play_res_y;
    }
    if (in->struct_size >= offsetof(ms_sub_cue, style_name) + sizeof(in->style_name)) {
        memcpy(it->style_name, in->style_name, sizeof(it->style_name));
        it->style_name[MS_SUB_STYLE_NAME_MAX - 1] = 0;
    }
    if (in->struct_size >= offsetof(ms_sub_cue, span_count) + sizeof(uint32_t)) {
        it->span_count = in->span_count;
        if (it->span_count > MS_SUB_SPAN_MAX) {
            it->span_count = MS_SUB_SPAN_MAX;
        }
    }
    if (in->struct_size >= offsetof(ms_sub_cue, spans) + sizeof(in->spans)) {
        memcpy(it->spans, in->spans, sizeof(it->spans));
    }
    if (in->struct_size >= offsetof(ms_sub_cue, bitmap_kind) + sizeof(int)) {
        it->bitmap_kind = in->bitmap_kind;
    }
    if (in->struct_size >= offsetof(ms_sub_cue, bitmap_w) + sizeof(int)) {
        it->bitmap_w = in->bitmap_w;
    }
    if (in->struct_size >= offsetof(ms_sub_cue, bitmap_h) + sizeof(int)) {
        it->bitmap_h = in->bitmap_h;
    }
    if (in->struct_size >= offsetof(ms_sub_cue, crop_x) + sizeof(int)) {
        it->crop_x = in->crop_x;
    }
    if (in->struct_size >= offsetof(ms_sub_cue, crop_y) + sizeof(int)) {
        it->crop_y = in->crop_y;
    }
    if (in->struct_size >= offsetof(ms_sub_cue, crop_w) + sizeof(int)) {
        it->crop_w = in->crop_w;
    }
    if (in->struct_size >= offsetof(ms_sub_cue, crop_h) + sizeof(int)) {
        it->crop_h = in->crop_h;
    }
    if (in->struct_size >= offsetof(ms_sub_cue, ai_utterance_id) + sizeof(uint32_t)) {
        it->ai_utterance_id = in->ai_utterance_id;
    }
    if (in->struct_size >= offsetof(ms_sub_cue, ai_revision) + sizeof(uint32_t)) {
        it->ai_revision = in->ai_revision;
    }
    if (in->struct_size >= offsetof(ms_sub_cue, ai_epoch) + sizeof(uint32_t)) {
        it->ai_epoch = in->ai_epoch;
    }
    if (in->struct_size >= offsetof(ms_sub_cue, ai_kind) + sizeof(int)) {
        it->ai_kind = in->ai_kind;
    }
    if (in->struct_size >= offsetof(ms_sub_cue, ai_delay_90k) + sizeof(int64_t)) {
        it->ai_delay_90k = in->ai_delay_90k;
    }
    if (in->struct_size >= offsetof(ms_sub_cue, ai_lang) + sizeof(in->ai_lang)) {
        memcpy(it->ai_lang, in->ai_lang, sizeof(it->ai_lang));
        it->ai_lang[15] = 0;
    }
}

static int cue_has_bitmap(const ms_sub_cue *in)
{
    if (in == NULL) {
        return 0;
    }
    if (in->struct_size < offsetof(ms_sub_cue, bitmap_rgba_size) + sizeof(size_t)) {
        return 0;
    }
    if (in->bitmap_rgba == NULL || in->bitmap_rgba_size == 0) {
        return 0;
    }
    if (in->bitmap_w <= 0 || in->bitmap_h <= 0) {
        return 0;
    }
    return 1;
}

static ms_status grow_bitmaps(ms_sub_doc *doc, size_t add)
{
    size_t ncap;
    uint8_t *nb;
    size_t need;
    if (add == 0) {
        return MS_OK;
    }
    need = doc->bitmap_used + add;
    if (need < doc->bitmap_used) {
        return MS_ERR_OVERFLOW;
    }
    if (need > MS_SUB_BITMAP_BYTES_MAX) {
        return MS_ERR_OVERFLOW;
    }
    if (need <= doc->bitmap_cap) {
        return MS_OK;
    }
    ncap = doc->bitmap_cap == 0 ? 4096 : doc->bitmap_cap;
    while (ncap < need) {
        if (ncap > MS_SUB_BITMAP_BYTES_MAX / 2u) {
            ncap = MS_SUB_BITMAP_BYTES_MAX;
            break;
        }
        ncap *= 2u;
    }
    if (ncap < need) {
        return MS_ERR_OVERFLOW;
    }
    nb = (uint8_t *)doc->alloc.alloc_fn(doc->alloc.ctx, ncap);
    if (nb == NULL) {
        return MS_ERR_NOMEM;
    }
    if (doc->bitmaps != NULL && doc->bitmap_used > 0) {
        memcpy(nb, doc->bitmaps, doc->bitmap_used);
    }
    if (doc->bitmaps != NULL) {
        doc->alloc.free_fn(doc->alloc.ctx, doc->bitmaps);
    }
    doc->bitmaps = nb;
    doc->bitmap_cap = ncap;
    return MS_OK;
}

static ms_status add_styled_cue(ms_sub_doc *doc, const ms_sub_cue *in,
                                const char *text, size_t tlen)
{
    ms_status st;
    ms_sub_item *it;
    int has_bmp = cue_has_bitmap(in);
    if (tlen == 0 && !has_bmp) {
        return MS_OK;
    }
    if (tlen > MS_SUB_CUE_TEXT_MAX) {
        return MS_ERR_OVERFLOW;
    }
    if (tlen > 0 && !is_utf8((const unsigned char *)text, tlen)) {
        return MS_ERR_MALFORMED;
    }
    {
        uint64_t end90 = in->has_end ? in->end_90k : in->start_90k;
        if (in->has_end && end90 < in->start_90k) {
            end90 = in->start_90k;
        }
        if (!cue_has_bitmap(in) &&
            cue_already_present(doc, in->track_id, in->has_start, in->start_90k,
                                in->has_end, end90, text, tlen)) {
            return MS_OK;
        }
    }
    if (doc->nitems >= MS_SUB_CUE_MAX) {
        return MS_ERR_OVERFLOW;
    }
    st = grow_items(doc, doc->nitems + 1);
    if (st != MS_OK) {
        return st;
    }
    if (tlen > 0) {
        st = grow_text(doc, tlen + 1);
        if (st != MS_OK) {
            return st;
        }
    }
    it = &doc->items[doc->nitems];
    memset(it, 0, sizeof(*it));
    it->track_id = in->track_id;
    it->has_start = in->has_start;
    it->has_end = in->has_end;
    it->start_90k = in->start_90k;
    it->end_90k = in->has_end ? in->end_90k : in->start_90k;
    if (it->has_end && it->end_90k < it->start_90k) {
        it->end_90k = it->start_90k;
    }
    it->text_off = doc->text_used;
    it->text_size = tlen;
    if (tlen > 0) {
        memcpy(doc->text + doc->text_used, text, tlen);
        doc->text_used += tlen;
        doc->text[doc->text_used] = 0;
        doc->text_used += 1;
    } else {
        it->text_off = 0;
        it->text_size = 0;
    }
    copy_style_to_item(it, in);
    if (has_bmp) {
        size_t expect = (size_t)in->bitmap_w * (size_t)in->bitmap_h * 4u;
        if (in->bitmap_w > MS_SUB_BITMAP_WIDTH_MAX ||
            in->bitmap_h > MS_SUB_BITMAP_HEIGHT_MAX ||
            in->bitmap_rgba_size != expect) {
            /* 条目已占位前失败：回退本次 item */
            return MS_ERR_MALFORMED;
        }
        st = grow_bitmaps(doc, expect);
        if (st != MS_OK) {
            return st;
        }
        it->bitmap_off = doc->bitmap_used;
        it->bitmap_size = expect;
        it->bitmap_kind = in->bitmap_kind;
        it->bitmap_w = in->bitmap_w;
        it->bitmap_h = in->bitmap_h;
        it->crop_x = in->crop_x;
        it->crop_y = in->crop_y;
        it->crop_w = in->crop_w;
        it->crop_h = in->crop_h;
        it->style_flags |= MS_SUB_STYLE_HAS_BITMAP;
        memcpy(doc->bitmaps + doc->bitmap_used, in->bitmap_rgba, expect);
        doc->bitmap_used += expect;
    }
    doc->nitems++;
    return MS_OK;
}

ms_status ms_sub_doc_add_cue(ms_sub_doc *doc, const ms_sub_cue *in)
{
    ms_sub_cue tmp;
    const char *text;
    size_t tlen;
    size_t ncopy;
    if (doc == NULL || in == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    memset(&tmp, 0, sizeof(tmp));
    ncopy = in->struct_size;
    if (ncopy == 0 || ncopy > sizeof(tmp)) {
        ncopy = sizeof(tmp);
    }
    memcpy(&tmp, in, ncopy);
    tmp.struct_size = ncopy;
    text = tmp.text;
    tlen = tmp.text_size;
    if (text == NULL) {
        text = "";
        tlen = 0;
    }
    if (tlen == 0 && text[0] != 0) {
        tlen = strlen(text);
    }
    if (tlen > MS_SUB_CUE_TEXT_MAX) {
        return MS_ERR_OVERFLOW;
    }
    if (doc->nitems >= MS_SUB_CUE_MAX) {
        return MS_ERR_OVERFLOW;
    }
    if (cue_prefix_has_style(&tmp) || cue_has_bitmap(&tmp)) {
        return add_styled_cue(doc, &tmp, text, tlen);
    }
    return add_parsed_cue(doc, tmp.track_id, tmp.start_90k,
                          tmp.has_end ? tmp.end_90k : tmp.start_90k,
                          text, tlen);
}

static int utf16_to_utf8(int be, const uint8_t *p, size_t n, char *out, size_t cap, size_t *out_n)
{
    size_t i = 0, o = 0;
    if ((n & 1u) != 0) {
        n -= 1;
    }
    while (i + 1 < n) {
        unsigned cp = be ? ((unsigned)p[i] << 8) | p[i + 1]
                         : ((unsigned)p[i + 1] << 8) | p[i];
        i += 2;
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < n) {
            unsigned lo = be ? ((unsigned)p[i] << 8) | p[i + 1]
                             : ((unsigned)p[i + 1] << 8) | p[i];
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                cp = 0x10000u + (((cp - 0xD800u) << 10) | (lo - 0xDC00u));
                i += 2;
            }
        }
        if (cp < 0x80) {
            if (o + 1 >= cap) {
                return -1;
            }
            out[o++] = (char)cp;
        } else if (cp < 0x800) {
            if (o + 2 >= cap) {
                return -1;
            }
            out[o++] = (char)(0xC0 | (cp >> 6));
            out[o++] = (char)(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            if (o + 3 >= cap) {
                return -1;
            }
            out[o++] = (char)(0xE0 | (cp >> 12));
            out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            out[o++] = (char)(0x80 | (cp & 0x3F));
        } else {
            if (o + 4 >= cap) {
                return -1;
            }
            out[o++] = (char)(0xF0 | (cp >> 18));
            out[o++] = (char)(0x80 | ((cp >> 12) & 0x3F));
            out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            out[o++] = (char)(0x80 | (cp & 0x3F));
        }
    }
    if (cap > 0) {
        if (o >= cap) {
            o = cap - 1;
        }
        out[o] = 0;
    }
    if (out_n != NULL) {
        *out_n = o;
    }
    return 0;
}

ms_status ms_sub_tx3g_text(const uint8_t *data, size_t size,
                           char *out, size_t cap, size_t *out_size)
{
    size_t n = 0;
    const uint8_t *p = data;
    size_t remain;
    if (out_size != NULL) {
        *out_size = 0;
    }
    if ((size > 0 && data == NULL) || out == NULL || cap == 0) {
        return MS_ERR_INVALID_ARG;
    }
    out[0] = 0;
    if (size == 0) {
        return MS_OK;
    }
    /* 常见：2 字节或 4 字节大端长度 + 文本；也可能整包就是文本。 */
    remain = size;
    if (size >= 2) {
        unsigned l16 = ((unsigned)p[0] << 8) | p[1];
        if (l16 > 0 && (size_t)l16 + 2u <= size && l16 < size) {
            p += 2;
            remain = l16;
        } else if (size >= 4) {
            unsigned l32 = ((unsigned)p[0] << 24) | ((unsigned)p[1] << 16) |
                           ((unsigned)p[2] << 8) | p[3];
            if (l32 > 0 && (size_t)l32 + 4u <= size) {
                p += 4;
                remain = l32;
            }
        }
    }
    if (remain >= 2 && p[0] == 0xFE && p[1] == 0xFF) {
        if (utf16_to_utf8(1, p + 2, remain - 2, out, cap, &n) != 0) {
            return MS_ERR_OVERFLOW;
        }
    } else if (remain >= 2 && p[0] == 0xFF && p[1] == 0xFE) {
        if (utf16_to_utf8(0, p + 2, remain - 2, out, cap, &n) != 0) {
            return MS_ERR_OVERFLOW;
        }
    } else if (remain >= 2 && p[0] == 0 && p[1] != 0) {
        /* 无 BOM 的 UTF-16BE 启发式 */
        if (utf16_to_utf8(1, p, remain, out, cap, &n) != 0) {
            return MS_ERR_OVERFLOW;
        }
    } else {
        if (remain >= cap) {
            remain = cap - 1;
        }
        if (!is_utf8(p, remain)) {
            return MS_ERR_MALFORMED;
        }
        memcpy(out, p, remain);
        out[remain] = 0;
        n = remain;
    }
    if (out_size != NULL) {
        *out_size = n;
    }
    return MS_OK;
}

#include "ms_sub_bitmap.c"
#include "ms_sub_ass.c"
