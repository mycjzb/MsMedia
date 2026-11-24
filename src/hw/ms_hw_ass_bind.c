#define _DEFAULT_SOURCE
/*
 * dlopen 可选 libmsass。失败则保持轻量去标签路径。
 * by: Long <mycjzb@gmail.com>
 */
#include "ms_hw_ass_bind.h"

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>

#include "msmedia/ms_ass_render.h"

struct ms_hw_ass_bind {
    const ms_allocator *alloc;
    ms_ass_render *r;
    int loaded;
    int has_script;
    int canvas_w;
    int canvas_h;
    char fonts_dir[96];
    char default_font[128];
    int fonts_owned; /* 本进程创建的符号链接目录，destroy 时清理链接不删字体 */
};

static void *g_dl;
static int g_dl_tried;
static ms_status (*g_create)(const ms_allocator *, const ms_ass_render_opts *, ms_ass_render **);
static void (*g_destroy)(ms_ass_render *);
static ms_status (*g_set_header)(ms_ass_render *, const uint8_t *, size_t);
static ms_status (*g_set_script)(ms_ass_render *, const uint8_t *, size_t);
static ms_status (*g_add_event)(ms_ass_render *, const uint8_t *, size_t, int, int64_t, int64_t);
static ms_status (*g_clear)(ms_ass_render *);
static ms_status (*g_set_canvas)(ms_ass_render *, int, int, int, int);
static ms_status (*g_at)(ms_ass_render *, int64_t, ms_ass_frame *);

static int file_ok(const char *p)
{
    struct stat st;
    return p != NULL && p[0] != 0 && stat(p, &st) == 0 && st.st_size > 0;
}

static int try_dl(void)
{
    if (g_dl_tried) {
        return g_dl != NULL;
    }
    g_dl_tried = 1;
    g_dl = dlopen("libmsass.so.1", RTLD_NOW | RTLD_LOCAL);
    if (g_dl == NULL) {
        g_dl = dlopen("libmsass.so", RTLD_NOW | RTLD_LOCAL);
    }
    if (g_dl == NULL) {
        return 0;
    }
#define LOAD_SYM(dst, typ, name) do { \
    dst = (typ)dlsym(g_dl, name); \
    if (dst == NULL) { dlclose(g_dl); g_dl = NULL; return 0; } \
} while (0)
    LOAD_SYM(g_create, ms_status (*)(const ms_allocator *, const ms_ass_render_opts *, ms_ass_render **), "ms_ass_render_create");
    LOAD_SYM(g_destroy, void (*)(ms_ass_render *), "ms_ass_render_destroy");
    LOAD_SYM(g_set_header, ms_status (*)(ms_ass_render *, const uint8_t *, size_t), "ms_ass_render_set_header");
    LOAD_SYM(g_set_script, ms_status (*)(ms_ass_render *, const uint8_t *, size_t), "ms_ass_render_set_script");
    LOAD_SYM(g_add_event, ms_status (*)(ms_ass_render *, const uint8_t *, size_t, int, int64_t, int64_t), "ms_ass_render_add_event");
    LOAD_SYM(g_clear, ms_status (*)(ms_ass_render *), "ms_ass_render_clear_events");
    LOAD_SYM(g_set_canvas, ms_status (*)(ms_ass_render *, int, int, int, int), "ms_ass_render_set_canvas");
    LOAD_SYM(g_at, ms_status (*)(ms_ass_render *, int64_t, ms_ass_frame *), "ms_ass_render_at");
#undef LOAD_SYM
    return 1;
}

int ms_hw_ass_bind_probe(void)
{
    return try_dl();
}

static void pick_fonts(ms_hw_ass_bind *b)
{
    b->fonts_dir[0] = 0;
    b->default_font[0] = 0;
    b->fonts_owned = 0;
#ifdef __ANDROID__
    {
        static const char *and_dir = "/system/fonts";
        static const char *and_cands[] = {
            "/system/fonts/NotoSansCJK-Regular.ttc",
            "/system/fonts/NotoSansSC-Regular.otf",
            "/system/fonts/NotoSansHans-Regular.otf",
            "/system/fonts/NotoSansCJK-Regular.otf",
            "/system/fonts/DroidSansFallback.ttf",
            "/system/fonts/Roboto-Regular.ttf",
            "/system/fonts/DroidSans.ttf",
            NULL
        };
        int i;
        if (file_ok(and_dir)) {
            snprintf(b->fonts_dir, sizeof(b->fonts_dir), "%s", and_dir);
            for (i = 0; and_cands[i] != NULL; i++) {
                if (file_ok(and_cands[i])) {
                    snprintf(b->default_font, sizeof(b->default_font), "%s", and_cands[i]);
                    break;
                }
            }
            return;
        }
    }
#else
    {
    static const char *host_dir = "/usr/share/fonts/truetype/dejavu";
    static const char *host_font = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf";
    static const char *host_cjk = "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc";
    if (file_ok(host_font) && file_ok(host_cjk)) {
        char dir[80], a[128], c[128];
        snprintf(dir, sizeof(dir), "/tmp/msmedia-ass-fonts-%d", (int)getpid());
        if (mkdir(dir, 0700) == 0 || errno == EEXIST) {
            snprintf(a, sizeof(a), "%s/DejaVuSans.ttf", dir);
            snprintf(c, sizeof(c), "%s/NotoSansCJK-Regular.ttc", dir);
            unlink(a);
            unlink(c);
            if (symlink(host_font, a) == 0 && symlink(host_cjk, c) == 0) {
                snprintf(b->fonts_dir, sizeof(b->fonts_dir), "%s", dir);
                snprintf(b->default_font, sizeof(b->default_font), "%s", a);
                b->fonts_owned = 1;
                return;
            }
        }
    }
    if (file_ok(host_font)) {
        snprintf(b->fonts_dir, sizeof(b->fonts_dir), "%s", host_dir);
        snprintf(b->default_font, sizeof(b->default_font), "%s", host_font);
    }
    }
#endif
}

static void drop_owned_fonts(ms_hw_ass_bind *b)
{
    char a[128];
    if (b == NULL || !b->fonts_owned || b->fonts_dir[0] == 0) {
        return;
    }
    snprintf(a, sizeof(a), "%s/DejaVuSans.ttf", b->fonts_dir);
    unlink(a);
    snprintf(a, sizeof(a), "%s/NotoSansCJK-Regular.ttc", b->fonts_dir);
    unlink(a);
    rmdir(b->fonts_dir);
    b->fonts_owned = 0;
}

static ms_status make_renderer(ms_hw_ass_bind *b)
{
    ms_ass_render_opts opts;
    ms_status st;
    if (b->r != NULL) {
        return MS_OK;
    }
    if (!try_dl() || g_create == NULL) {
        return MS_ERR_UNSUPPORTED;
    }
    pick_fonts(b);
    /* 不直接调用 libmsass 符号，避免 libmshw NEEDED libmsass。 */
    memset(&opts, 0, sizeof(opts));
    opts.struct_size = sizeof(opts);
    opts.canvas_w = b->canvas_w > 0 ? b->canvas_w : 1280;
    opts.canvas_h = b->canvas_h > 0 ? b->canvas_h : 720;
    opts.storage_w = opts.canvas_w;
    opts.storage_h = opts.canvas_h;
    if (b->fonts_dir[0]) {
        opts.fonts_dir = b->fonts_dir;
    }
    if (b->default_font[0]) {
        opts.default_font = b->default_font;
    }
    opts.extract_fonts = 0;
    st = g_create(b->alloc, &opts, &b->r);
    if (st != MS_OK) {
        b->r = NULL;
        return st;
    }
    b->canvas_w = opts.canvas_w;
    b->canvas_h = opts.canvas_h;
    return MS_OK;
}

int ms_hw_ass_bind_loaded(const ms_hw_ass_bind *b)
{
    (void)b;
    return try_dl();
}

int ms_hw_ass_bind_has_script(const ms_hw_ass_bind *b)
{
    return b != NULL && b->has_script;
}

ms_status ms_hw_ass_bind_ensure(const ms_allocator *alloc, ms_hw_ass_bind **io)
{
    ms_hw_ass_bind *b;
    if (io == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    if (*io != NULL) {
        return MS_OK;
    }
    if (!try_dl()) {
        return MS_ERR_UNSUPPORTED;
    }
    if (alloc != NULL && alloc->alloc_fn != NULL) {
        b = (ms_hw_ass_bind *)alloc->alloc_fn(alloc->ctx, sizeof(*b));
    } else {
        b = (ms_hw_ass_bind *)malloc(sizeof(*b));
    }
    if (b == NULL) {
        return MS_ERR_NOMEM;
    }
    memset(b, 0, sizeof(*b));
    b->alloc = alloc;
    b->canvas_w = 1280;
    b->canvas_h = 720;
    b->loaded = 1;
    *io = b;
    return MS_OK;
}

void ms_hw_ass_bind_destroy(ms_hw_ass_bind *b)
{
    const ms_allocator *a;
    if (b == NULL) {
        return;
    }
    a = b->alloc;
    if (b->r && g_destroy) {
        g_destroy(b->r);
        b->r = NULL;
    }
    drop_owned_fonts(b);
    if (a != NULL && a->free_fn != NULL) {
        a->free_fn(a->ctx, b);
    } else {
        free(b);
    }
}

ms_status ms_hw_ass_bind_set_header(ms_hw_ass_bind *b, const uint8_t *data, size_t size)
{
    ms_status st;
    if (b == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    st = make_renderer(b);
    if (st != MS_OK) {
        return st;
    }
    st = g_set_header(b->r, data, size);
    if (st == MS_OK) {
        b->has_script = 1;
    }
    return st;
}

ms_status ms_hw_ass_bind_set_script(ms_hw_ass_bind *b, const uint8_t *data, size_t size)
{
    ms_status st;
    if (b == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    st = make_renderer(b);
    if (st != MS_OK) {
        return st;
    }
    st = g_set_script(b->r, data, size);
    if (st == MS_OK) {
        b->has_script = 1;
    }
    return st;
}

ms_status ms_hw_ass_bind_add_event(ms_hw_ass_bind *b, const uint8_t *data, size_t size,
                                   int has_times, int64_t start_90k, int64_t end_90k)
{
    ms_status st;
    if (b == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    st = make_renderer(b);
    if (st != MS_OK) {
        return st;
    }
    st = g_add_event(b->r, data, size, has_times, start_90k, end_90k);
    if (st == MS_OK) {
        b->has_script = 1;
    }
    return st;
}

ms_status ms_hw_ass_bind_clear(ms_hw_ass_bind *b)
{
    if (b == NULL) {
        return MS_OK;
    }
    if (b->r && g_clear) {
        g_clear(b->r);
    }
    return MS_OK;
}

ms_status ms_hw_ass_bind_set_canvas(ms_hw_ass_bind *b, int w, int h)
{
    if (b == NULL || w <= 0 || h <= 0) {
        return MS_ERR_INVALID_ARG;
    }
    b->canvas_w = w;
    b->canvas_h = h;
    if (b->r && g_set_canvas) {
        return g_set_canvas(b->r, w, h, w, h);
    }
    return MS_OK;
}

ms_status ms_hw_ass_bind_frame_at(ms_hw_ass_bind *b, uint64_t time_90k, ms_hw_ass_frame *out)
{
    ms_ass_frame fr;
    ms_status st;
    if (out == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    out->struct_size = sizeof(*out);
    out->available = try_dl() ? 1 : 0;
    if (b == NULL || b->r == NULL || g_at == NULL) {
        return MS_OK;
    }
    memset(&fr, 0, sizeof(fr));
    st = g_at(b->r, (int64_t)time_90k, &fr);
    if (st != MS_OK) {
        return st;
    }
    out->available = 1;
    out->has_script = b->has_script;
    out->changed = fr.changed;
    out->x = fr.x;
    out->y = fr.y;
    out->w = fr.w;
    out->h = fr.h;
    out->rgba = fr.rgba;
    out->rgba_size = fr.rgba_size;
    out->canvas_w = fr.canvas_w;
    out->canvas_h = fr.canvas_h;
    return MS_OK;
}
