/*
 * 可选完整 ASS 栅格：隔离包装钉选 libass。不进入 libmsmedia / libmsff。
 * 字幕栅格不是视频软解，不读回视频 Surface。
 * by: Long <mycjzb@gmail.com>
 */

#include "msmedia/ms_ass_render.h"

#include <ass/ass.h>
#include <stdlib.h>
#include <string.h>

#define MS_ASS_MAX_CANVAS_BYTES (16u * 1024u * 1024u)
#define MS_ASS_DEFAULT_EVENTS 4096u
#define MS_ASS_DEFAULT_W 1280
#define MS_ASS_DEFAULT_H 720
#define MS_ASS_MAX_BITMAP_CACHE_MB 32

struct ms_ass_render {
    const ms_allocator *alloc;
    ms_allocator owned_alloc;
    ASS_Library *lib;
    ASS_Renderer *renderer;
    ASS_Track *track;
    int canvas_w;
    int canvas_h;
    int storage_w;
    int storage_h;
    size_t max_canvas_bytes;
    size_t max_events;
    int extract_fonts;
    uint8_t *rgba;
    size_t rgba_size;
    int have_frame;
    int last_changed;
    int dirty_x;
    int dirty_y;
    int dirty_w;
    int dirty_h;
};

static void *std_alloc(void *ctx, size_t n)
{
    (void)ctx;
    return malloc(n);
}

static void std_free(void *ctx, void *p)
{
    (void)ctx;
    free(p);
}

static void *r_alloc(ms_ass_render *r, size_t n)
{
    if (r == NULL || r->alloc == NULL || r->alloc->alloc_fn == NULL) {
        return malloc(n);
    }
    return r->alloc->alloc_fn(r->alloc->ctx, n);
}

static void r_free(ms_ass_render *r, void *p)
{
    if (p == NULL) {
        return;
    }
    if (r == NULL || r->alloc == NULL || r->alloc->free_fn == NULL) {
        free(p);
        return;
    }
    r->alloc->free_fn(r->alloc->ctx, p);
}

static void *dup_mem(const void *src, size_t n)
{
    void *p;
    if (src == NULL || n == 0) {
        return NULL;
    }
    p = malloc(n);
    if (p == NULL) {
        return NULL;
    }
    memcpy(p, src, n);
    return p;
}

static const char *skip_bom_ws(const char *s, size_t *inout_n)
{
    size_t n = inout_n ? *inout_n : 0;
    if (s == NULL) {
        return NULL;
    }
    if (n >= 3 && (unsigned char)s[0] == 0xef && (unsigned char)s[1] == 0xbb &&
        (unsigned char)s[2] == 0xbf) {
        s += 3;
        n -= 3;
    }
    while (n > 0 && (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n')) {
        s++;
        n--;
    }
    if (inout_n) {
        *inout_n = n;
    }
    return s;
}

static int looks_like_dialogue(const char *s, size_t n)
{
    static const char *k[] = { "Dialogue:", "Comment:", "Style:", "Format:", "[", "Script" };
    size_t i;
    for (i = 0; i < sizeof(k) / sizeof(k[0]); i++) {
        size_t ln = strlen(k[i]);
        if (n >= ln && strncmp(s, k[i], ln) == 0) {
            return 1;
        }
        /* 大小写不敏感前缀 */
        if (n >= 9 && (s[0] == 'd' || s[0] == 'D') && ln == 9) {
            /* handled by Dialogue: literal above if exact */
        }
    }
    if (n >= 9) {
        char buf[10];
        size_t j;
        for (j = 0; j < 9; j++) {
            char c = s[j];
            if (c >= 'A' && c <= 'Z') {
                c = (char)(c - 'A' + 'a');
            }
            buf[j] = c;
        }
        buf[9] = 0;
        if (memcmp(buf, "dialogue:", 9) == 0 || memcmp(buf, "comment:", 8) == 0) {
            return 1;
        }
    }
    return 0;
}

static int64_t ms_from_90k(int64_t t)
{
    if (t < 0) {
        return 0;
    }
    /* 90 kHz -> ms，向下取整 */
    return t / 90;
}

static void apply_sizes(ms_ass_render *r)
{
    int sw = r->storage_w > 0 ? r->storage_w : r->canvas_w;
    int sh = r->storage_h > 0 ? r->storage_h : r->canvas_h;
    if (r->renderer == NULL) {
        return;
    }
    ass_set_frame_size(r->renderer, r->canvas_w, r->canvas_h);
    ass_set_storage_size(r->renderer, sw, sh);
    ass_set_pixel_aspect(r->renderer, 1.0);
}

static ms_status ensure_canvas(ms_ass_render *r)
{
    size_t need;
    uint8_t *p;
    if (r->canvas_w <= 0 || r->canvas_h <= 0) {
        return MS_ERR_INVALID_ARG;
    }
    if ((size_t)r->canvas_w > 8192 || (size_t)r->canvas_h > 8192) {
        return MS_ERR_INVALID_ARG;
    }
    need = (size_t)r->canvas_w * (size_t)r->canvas_h * 4u;
    if (need > r->max_canvas_bytes) {
        return MS_ERR_OVERFLOW;
    }
    if (r->rgba != NULL && r->rgba_size == need) {
        return MS_OK;
    }
    p = (uint8_t *)r_alloc(r, need);
    if (p == NULL) {
        return MS_ERR_NOMEM;
    }
    memset(p, 0, need);
    r_free(r, r->rgba);
    r->rgba = p;
    r->rgba_size = need;
    r->have_frame = 0;
    return MS_OK;
}

static void blend_over(uint8_t *dst, int dst_w, int dst_h,
                       const ASS_Image *img,
                       int *minx, int *miny, int *maxx, int *maxy)
{
    int x, y;
    int x0, y0, x1, y1;
    unsigned cr, cg, cb, ca;
    uint32_t color;
    const uint8_t *src;
    if (img == NULL || img->w <= 0 || img->h <= 0 || img->bitmap == NULL) {
        return;
    }
    color = img->color;
    cr = (color >> 24) & 0xffu;
    cg = (color >> 16) & 0xffu;
    cb = (color >> 8) & 0xffu;
    /* libass 颜色低字节为 ASS 透明度（0=不透明） */
    ca = 255u - (color & 0xffu);
    if (ca == 0) {
        return;
    }
    x0 = img->dst_x;
    y0 = img->dst_y;
    x1 = x0 + img->w;
    y1 = y0 + img->h;
    if (x0 < 0) {
        x0 = 0;
    }
    if (y0 < 0) {
        y0 = 0;
    }
    if (x1 > dst_w) {
        x1 = dst_w;
    }
    if (y1 > dst_h) {
        y1 = dst_h;
    }
    if (x0 >= x1 || y0 >= y1) {
        return;
    }
    if (x0 < *minx) {
        *minx = x0;
    }
    if (y0 < *miny) {
        *miny = y0;
    }
    if (x1 > *maxx) {
        *maxx = x1;
    }
    if (y1 > *maxy) {
        *maxy = y1;
    }
    for (y = y0; y < y1; y++) {
        int sx = x0 - img->dst_x;
        int sy = y - img->dst_y;
        src = img->bitmap + (size_t)sy * (size_t)img->stride + (size_t)sx;
        uint8_t *row = dst + ((size_t)y * (size_t)dst_w + (size_t)x0) * 4u;
        for (x = x0; x < x1; x++) {
            unsigned bm = *src++;
            unsigned sa = (bm * ca + 127u) / 255u;
            unsigned dr = row[0], dg = row[1], db = row[2], da = row[3];
            unsigned out_a, inv;
            if (sa == 0) {
                row += 4;
                continue;
            }
            inv = 255u - sa;
            out_a = sa + (da * inv + 127u) / 255u;
            if (out_a > 255u) {
                out_a = 255u;
            }
            if (out_a == 0) {
                row[0] = row[1] = row[2] = row[3] = 0;
            } else {
                /* 直通（非预乘）源覆盖 */
                unsigned r = cr * sa + dr * da * inv / 255u;
                unsigned g = cg * sa + dg * da * inv / 255u;
                unsigned b = cb * sa + db * da * inv / 255u;
                row[0] = (uint8_t)((r + out_a / 2u) / out_a);
                row[1] = (uint8_t)((g + out_a / 2u) / out_a);
                row[2] = (uint8_t)((b + out_a / 2u) / out_a);
                row[3] = (uint8_t)out_a;
            }
            row += 4;
        }
    }
}

static int ensure_track(ms_ass_render *r)
{
    if (r->track != NULL) {
        return 0;
    }
    r->track = ass_new_track(r->lib);
    return r->track == NULL ? -1 : 0;
}

void ms_ass_render_caps_default(ms_ass_render_caps *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->struct_size = sizeof(*out);
    out->available = 1;
    out->can_stroke = 1;
    out->can_transform = 1;
    out->can_move = 1;
    out->can_fade = 1;
    out->can_karaoke = 1;
    out->can_clip = 1;
    out->can_layer = 1;
    out->can_multilingual = 1;
    out->backend = MS_ASS_RENDER_BACKEND;
    out->detail = "optional isolated libass; not 100% of all ASS scripts";
}

void ms_ass_render_opts_default(ms_ass_render_opts *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->struct_size = sizeof(*out);
    out->canvas_w = MS_ASS_DEFAULT_W;
    out->canvas_h = MS_ASS_DEFAULT_H;
    out->extract_fonts = 0;
}

ms_status ms_ass_render_query_caps(ms_ass_render_caps *out)
{
    if (out == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    ms_ass_render_caps_default(out);
    return MS_OK;
}

ms_status ms_ass_render_create(const ms_allocator *alloc,
                               const ms_ass_render_opts *opts,
                               ms_ass_render **out)
{
    ms_ass_render *r;
    ms_ass_render_opts d;
    ms_status st;
    int glyph_max = 0;
    int bitmap_mb = 0;
    const char *fonts_dir = NULL;
    const char *default_font = NULL;
    const char *default_family = "sans-serif";

    if (out == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    *out = NULL;
    ms_ass_render_opts_default(&d);
    if (opts != NULL) {
        size_t n = opts->struct_size ? opts->struct_size : sizeof(*opts);
        if (n > sizeof(*opts)) {
            n = sizeof(*opts);
        }
        memcpy(&d, opts, n);
        d.struct_size = sizeof(d);
    }
    if (d.canvas_w <= 0) {
        d.canvas_w = MS_ASS_DEFAULT_W;
    }
    if (d.canvas_h <= 0) {
        d.canvas_h = MS_ASS_DEFAULT_H;
    }
    if (d.max_canvas_bytes == 0) {
        d.max_canvas_bytes = MS_ASS_MAX_CANVAS_BYTES;
    }
    if (d.max_canvas_bytes > MS_ASS_MAX_CANVAS_BYTES) {
        d.max_canvas_bytes = MS_ASS_MAX_CANVAS_BYTES;
    }
    if (d.max_events == 0) {
        d.max_events = MS_ASS_DEFAULT_EVENTS;
    }
    if (d.bitmap_cache_mb > MS_ASS_MAX_BITMAP_CACHE_MB) {
        d.bitmap_cache_mb = MS_ASS_MAX_BITMAP_CACHE_MB;
    }

    r = (ms_ass_render *)((alloc && alloc->alloc_fn) ? alloc->alloc_fn(alloc->ctx, sizeof(*r))
                                                     : malloc(sizeof(*r)));
    if (r == NULL) {
        return MS_ERR_NOMEM;
    }
    memset(r, 0, sizeof(*r));
    if (alloc != NULL && alloc->alloc_fn != NULL && alloc->free_fn != NULL) {
        r->owned_alloc = *alloc;
        r->alloc = &r->owned_alloc;
    } else {
        r->owned_alloc.alloc_fn = std_alloc;
        r->owned_alloc.free_fn = std_free;
        r->owned_alloc.ctx = NULL;
        r->alloc = &r->owned_alloc;
    }
    r->canvas_w = d.canvas_w;
    r->canvas_h = d.canvas_h;
    r->storage_w = d.storage_w;
    r->storage_h = d.storage_h;
    r->max_canvas_bytes = d.max_canvas_bytes;
    r->max_events = d.max_events;
    r->extract_fonts = d.extract_fonts ? 1 : 0;

    r->lib = ass_library_init();
    if (r->lib == NULL) {
        r_free(r, r);
        return MS_ERR_NOMEM;
    }
    ass_set_extract_fonts(r->lib, r->extract_fonts);
    fonts_dir = d.fonts_dir;
    default_font = d.default_font;
    if (d.default_family != NULL && d.default_family[0] != 0) {
        default_family = d.default_family;
    }
    if (fonts_dir != NULL && fonts_dir[0] != 0) {
        /* 多个目录用冒号分隔；逐个 set 会覆盖，因此取第一段给 set_fonts_dir，
         * 其余通过同样扫描约定由调用方把授权字体放在同一目录或主目录。 */
        char first[512];
        const char *colon = strchr(fonts_dir, ':');
        size_t n = colon ? (size_t)(colon - fonts_dir) : strlen(fonts_dir);
        if (n >= sizeof(first)) {
            n = sizeof(first) - 1;
        }
        memcpy(first, fonts_dir, n);
        first[n] = 0;
        ass_set_fonts_dir(r->lib, first);
        /* 后续目录：libass 只接受一个 fonts_dir；把后续路径也尝试 set（后写覆盖）。
         * 为避免覆盖，保留第一目录为主目录，调用方应把主字体放在第一个目录。 */
        (void)colon;
    }
    r->renderer = ass_renderer_init(r->lib);
    if (r->renderer == NULL) {
        ass_library_done(r->lib);
        r_free(r, r);
        return MS_ERR_NOMEM;
    }
    ass_set_hinting(r->renderer, ASS_HINTING_NONE);
    ass_set_shaper(r->renderer, ASS_SHAPING_COMPLEX);
    apply_sizes(r);
    glyph_max = d.glyph_cache_max;
    bitmap_mb = d.bitmap_cache_mb;
    if (glyph_max > 0 || bitmap_mb > 0) {
        ass_set_cache_limits(r->renderer, glyph_max, bitmap_mb);
    }
    /* ASS_FONTPROVIDER_NONE：不用 fontconfig，只用 fonts_dir + 默认文件。 */
    ass_set_fonts(r->renderer, default_font, default_family, ASS_FONTPROVIDER_NONE, NULL, 1);

    st = ensure_canvas(r);
    if (st != MS_OK) {
        ass_renderer_done(r->renderer);
        ass_library_done(r->lib);
        r_free(r, r);
        return st;
    }
    *out = r;
    return MS_OK;
}

void ms_ass_render_destroy(ms_ass_render *r)
{
    const ms_allocator *a;
    if (r == NULL) {
        return;
    }
    a = r->alloc;
    if (r->track) {
        ass_free_track(r->track);
        r->track = NULL;
    }
    if (r->renderer) {
        ass_renderer_done(r->renderer);
        r->renderer = NULL;
    }
    if (r->lib) {
        ass_library_done(r->lib);
        r->lib = NULL;
    }
    r_free(r, r->rgba);
    r->rgba = NULL;
    if (a != NULL && a->free_fn != NULL) {
        a->free_fn(a->ctx, r);
    } else {
        free(r);
    }
}

ms_status ms_ass_render_set_header(ms_ass_render *r, const uint8_t *data, size_t size)
{
    char *buf;
    if (r == NULL || (data == NULL && size != 0)) {
        return MS_ERR_INVALID_ARG;
    }
    if (size == 0) {
        return MS_ERR_INVALID_ARG;
    }
    if (r->track) {
        ass_free_track(r->track);
        r->track = NULL;
    }
    r->track = ass_new_track(r->lib);
    if (r->track == NULL) {
        return MS_ERR_NOMEM;
    }
    buf = (char *)dup_mem(data, size);
    if (buf == NULL) {
        return MS_ERR_NOMEM;
    }
    ass_process_codec_private(r->track, buf, (int)size);
    free(buf);
    r->have_frame = 0;
    return MS_OK;
}

ms_status ms_ass_render_set_script(ms_ass_render *r, const uint8_t *data, size_t size)
{
    char *buf;
    ASS_Track *t;
    if (r == NULL || data == NULL || size == 0) {
        return MS_ERR_INVALID_ARG;
    }
    buf = (char *)dup_mem(data, size + 1);
    if (buf == NULL) {
        return MS_ERR_NOMEM;
    }
    buf[size] = 0;
    t = ass_read_memory(r->lib, buf, (int)size, NULL);
    free(buf);
    if (t == NULL) {
        return MS_ERR_MALFORMED;
    }
    if (r->track) {
        ass_free_track(r->track);
    }
    r->track = t;
    r->have_frame = 0;
    return MS_OK;
}

ms_status ms_ass_render_add_event(ms_ass_render *r, const uint8_t *data, size_t size,
                                  int has_times, int64_t start_90k, int64_t end_90k)
{
    char *buf;
    const char *p;
    size_t n;
    if (r == NULL || data == NULL || size == 0) {
        return MS_ERR_INVALID_ARG;
    }
    if (ensure_track(r) != 0) {
        return MS_ERR_NOMEM;
    }
    if ((size_t)r->track->n_events >= r->max_events) {
        return MS_ERR_OVERFLOW;
    }
    n = size;
    p = skip_bom_ws((const char *)data, &n);
    if (p == NULL || n == 0) {
        return MS_ERR_INVALID_ARG;
    }
    buf = (char *)malloc(n + 2);
    if (buf == NULL) {
        return MS_ERR_NOMEM;
    }
    memcpy(buf, p, n);
    if (n == 0 || buf[n - 1] != '\n') {
        buf[n] = '\n';
        n++;
    }
    buf[n] = 0;
    if (looks_like_dialogue(p, size > n ? n : (size_t)(buf[n - 1] == '\n' ? n - 1 : n)) ||
        looks_like_dialogue(buf, n)) {
        ass_process_data(r->track, buf, (int)n);
    } else if (has_times) {
        long long st = (long long)ms_from_90k(start_90k);
        long long dur = (long long)ms_from_90k(end_90k) - st;
        if (dur < 0) {
            dur = 0;
        }
        ass_process_chunk(r->track, buf, (int)n, st, dur);
    } else {
        ass_process_data(r->track, buf, (int)n);
    }
    free(buf);
    r->have_frame = 0;
    return MS_OK;
}

ms_status ms_ass_render_clear_events(ms_ass_render *r)
{
    if (r == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    if (r->track) {
        ass_flush_events(r->track);
    }
    r->have_frame = 0;
    if (r->rgba && r->rgba_size) {
        memset(r->rgba, 0, r->rgba_size);
    }
    return MS_OK;
}

ms_status ms_ass_render_set_canvas(ms_ass_render *r, int canvas_w, int canvas_h,
                                   int storage_w, int storage_h)
{
    ms_status st;
    if (r == NULL || canvas_w <= 0 || canvas_h <= 0) {
        return MS_ERR_INVALID_ARG;
    }
    r->canvas_w = canvas_w;
    r->canvas_h = canvas_h;
    r->storage_w = storage_w;
    r->storage_h = storage_h;
    apply_sizes(r);
    st = ensure_canvas(r);
    r->have_frame = 0;
    return st;
}

ms_status ms_ass_render_at(ms_ass_render *r, int64_t time_90k, ms_ass_frame *out)
{
    ASS_Image *img;
    int changed = 0;
    long long now_ms;
    int minx, miny, maxx, maxy;
    if (r == NULL || out == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    if (r->renderer == NULL) {
        return MS_ERR_STATE;
    }
    if (ensure_track(r) != 0) {
        return MS_ERR_NOMEM;
    }
    now_ms = (long long)ms_from_90k(time_90k);
    img = ass_render_frame(r->renderer, r->track, now_ms, &changed);
    if (r->have_frame && changed == 0) {
        memset(out, 0, sizeof(*out));
        out->struct_size = sizeof(*out);
        out->changed = 0;
        out->x = r->dirty_x;
        out->y = r->dirty_y;
        out->w = r->dirty_w;
        out->h = r->dirty_h;
        out->rgba = r->rgba;
        out->rgba_size = r->rgba_size;
        out->canvas_w = r->canvas_w;
        out->canvas_h = r->canvas_h;
        return MS_OK;
    }
    memset(r->rgba, 0, r->rgba_size);
    minx = r->canvas_w;
    miny = r->canvas_h;
    maxx = 0;
    maxy = 0;
    for (; img != NULL; img = img->next) {
        blend_over(r->rgba, r->canvas_w, r->canvas_h, img, &minx, &miny, &maxx, &maxy);
    }
    if (maxx <= minx || maxy <= miny) {
        r->dirty_x = r->dirty_y = r->dirty_w = r->dirty_h = 0;
    } else {
        r->dirty_x = minx;
        r->dirty_y = miny;
        r->dirty_w = maxx - minx;
        r->dirty_h = maxy - miny;
    }
    r->have_frame = 1;
    r->last_changed = changed;
    memset(out, 0, sizeof(*out));
    out->struct_size = sizeof(*out);
    out->changed = changed;
    out->x = r->dirty_x;
    out->y = r->dirty_y;
    out->w = r->dirty_w;
    out->h = r->dirty_h;
    out->rgba = r->rgba;
    out->rgba_size = r->rgba_size;
    out->canvas_w = r->canvas_w;
    out->canvas_h = r->canvas_h;
    return MS_OK;
}
