#define _DEFAULT_SOURCE
/*
 * dlopen 可选 libmsai。失败则 AI 字幕保持不可用，默认核心无模型/网络。
 * 不得把未加载或假供应商写成真实识别。
 * by: Long <mycjzb@gmail.com>
 */
#include "ms_hw_ai_bind.h"

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct ms_hw_ai_bind {
    const ms_allocator *alloc;
    ms_ai_sub *eng;
    int loaded;
    char fallback_status[MS_AI_STATUS_MAX];
};

static void *g_dl;
static int g_dl_tried;
static void (*g_opts_default)(ms_ai_sub_opts *);
static ms_status (*g_create)(const ms_allocator *, const ms_ai_sub_opts *, ms_ai_sub **);
static void (*g_destroy)(ms_ai_sub *);
static ms_status (*g_configure)(ms_ai_sub *, const ms_ai_sub_opts *);
static ms_status (*g_use_fake)(ms_ai_sub *);
static ms_status (*g_query_caps)(const ms_ai_sub *, ms_ai_vendor_caps *);
static ms_status (*g_set_enabled)(ms_ai_sub *, int);
static int (*g_enabled)(const ms_ai_sub *);
static void (*g_set_epochs)(ms_ai_sub *, uint32_t, uint32_t, uint32_t);
static void (*g_bump_tl)(ms_ai_sub *);
static void (*g_bump_sel)(ms_ai_sub *);
static ms_status (*g_tap)(ms_ai_sub *, const uint8_t *, size_t, int, int, int, int, int64_t);
static ms_status (*g_drive)(ms_ai_sub *);
static void (*g_flush)(ms_ai_sub *);
static ms_sub_doc *(*g_doc)(ms_ai_sub *);
static ms_status (*g_stats)(const ms_ai_sub *, ms_ai_sub_stats *);
static const char *(*g_status)(const ms_ai_sub *);

static int try_dl(void)
{
    if (g_dl_tried) {
        return g_dl != NULL;
    }
    g_dl_tried = 1;
    g_dl = dlopen("libmsai.so.1", RTLD_NOW | RTLD_LOCAL);
    if (g_dl == NULL) {
        g_dl = dlopen("libmsai.so", RTLD_NOW | RTLD_LOCAL);
    }
    if (g_dl == NULL) {
        return 0;
    }
#define LOAD_SYM(dst, typ, name) do { \
    dst = (typ)dlsym(g_dl, name); \
    if (dst == NULL) { dlclose(g_dl); g_dl = NULL; return 0; } \
} while (0)
    LOAD_SYM(g_opts_default, void (*)(ms_ai_sub_opts *), "ms_ai_sub_opts_default");
    LOAD_SYM(g_create, ms_status (*)(const ms_allocator *, const ms_ai_sub_opts *, ms_ai_sub **), "ms_ai_sub_create");
    LOAD_SYM(g_destroy, void (*)(ms_ai_sub *), "ms_ai_sub_destroy");
    LOAD_SYM(g_configure, ms_status (*)(ms_ai_sub *, const ms_ai_sub_opts *), "ms_ai_sub_configure");
    LOAD_SYM(g_use_fake, ms_status (*)(ms_ai_sub *), "ms_ai_sub_use_fake_local");
    LOAD_SYM(g_query_caps, ms_status (*)(const ms_ai_sub *, ms_ai_vendor_caps *), "ms_ai_sub_query_caps");
    LOAD_SYM(g_set_enabled, ms_status (*)(ms_ai_sub *, int), "ms_ai_sub_set_enabled");
    LOAD_SYM(g_enabled, int (*)(const ms_ai_sub *), "ms_ai_sub_enabled");
    LOAD_SYM(g_set_epochs, void (*)(ms_ai_sub *, uint32_t, uint32_t, uint32_t), "ms_ai_sub_set_epochs");
    LOAD_SYM(g_bump_tl, void (*)(ms_ai_sub *), "ms_ai_sub_bump_timeline");
    LOAD_SYM(g_bump_sel, void (*)(ms_ai_sub *), "ms_ai_sub_bump_selection");
    LOAD_SYM(g_tap, ms_status (*)(ms_ai_sub *, const uint8_t *, size_t, int, int, int, int, int64_t), "ms_ai_sub_tap_pcm");
    LOAD_SYM(g_drive, ms_status (*)(ms_ai_sub *), "ms_ai_sub_drive");
    LOAD_SYM(g_flush, void (*)(ms_ai_sub *), "ms_ai_sub_flush");
    LOAD_SYM(g_doc, ms_sub_doc *(*)(ms_ai_sub *), "ms_ai_sub_doc");
    LOAD_SYM(g_stats, ms_status (*)(const ms_ai_sub *, ms_ai_sub_stats *), "ms_ai_sub_get_stats");
    LOAD_SYM(g_status, const char *(*)(const ms_ai_sub *), "ms_ai_sub_status_text");
#undef LOAD_SYM
    return 1;
}

int ms_hw_ai_bind_probe(void)
{
    return try_dl();
}

static void tap_cb(void *ctx, const uint8_t *pcm, size_t bytes, int rate, int ch,
                   int bits, int has_pts, int64_t pts_us)
{
    ms_hw_ai_bind *b = (ms_hw_ai_bind *)ctx;
    if (b == NULL || b->eng == NULL || g_tap == NULL) {
        return;
    }
    (void)g_tap(b->eng, pcm, bytes, rate, ch, bits, has_pts, pts_us);
}

ms_status ms_hw_ai_bind_ensure(const ms_allocator *alloc, ms_hw_ai_bind **io)
{
    ms_hw_ai_bind *b;
    ms_ai_sub_opts opts;
    ms_status st;
    if (io == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    if (*io != NULL) {
        return MS_OK;
    }
    if (alloc == NULL || alloc->alloc_fn == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    b = (ms_hw_ai_bind *)alloc->alloc_fn(alloc->ctx, sizeof(*b));
    if (b == NULL) {
        return MS_ERR_NOMEM;
    }
    memset(b, 0, sizeof(*b));
    b->alloc = alloc;
    snprintf(b->fallback_status, sizeof(b->fallback_status), "module-not-loaded");
    if (!try_dl()) {
        *io = b;
        return MS_OK; /* 允许存在空绑定；enabled 会失败 */
    }
    memset(&opts, 0, sizeof(opts));
    if (g_opts_default != NULL) {
        g_opts_default(&opts);
    } else {
        opts.struct_size = sizeof(opts);
    }
    st = g_create(alloc, &opts, &b->eng);
    if (st != MS_OK) {
        alloc->free_fn(alloc->ctx, b);
        return st;
    }
    b->loaded = 1;
    *io = b;
    return MS_OK;
}

void ms_hw_ai_bind_destroy(ms_hw_ai_bind *b)
{
    const ms_allocator *a;
    if (b == NULL) {
        return;
    }
    a = b->alloc;
    if (b->eng != NULL && g_destroy != NULL) {
        g_destroy(b->eng);
        b->eng = NULL;
    }
    if (a != NULL && a->free_fn != NULL) {
        a->free_fn(a->ctx, b);
    }
}

int ms_hw_ai_bind_loaded(const ms_hw_ai_bind *b)
{
    return b != NULL && b->loaded && b->eng != NULL;
}

ms_status ms_hw_ai_bind_configure(ms_hw_ai_bind *b, const ms_ai_sub_opts *opts)
{
    if (!ms_hw_ai_bind_loaded(b) || g_configure == NULL) {
        return MS_ERR_UNSUPPORTED;
    }
    return g_configure(b->eng, opts);
}

ms_status ms_hw_ai_bind_use_fake_local(ms_hw_ai_bind *b)
{
    if (!ms_hw_ai_bind_loaded(b) || g_use_fake == NULL) {
        return MS_ERR_UNSUPPORTED;
    }
    return g_use_fake(b->eng);
}

ms_status ms_hw_ai_bind_set_enabled(ms_hw_ai_bind *b, int on)
{
    if (!ms_hw_ai_bind_loaded(b) || g_set_enabled == NULL) {
        return on ? MS_ERR_UNSUPPORTED : MS_OK;
    }
    return g_set_enabled(b->eng, on);
}

int ms_hw_ai_bind_enabled(const ms_hw_ai_bind *b)
{
    if (!ms_hw_ai_bind_loaded(b) || g_enabled == NULL) {
        return 0;
    }
    return g_enabled(b->eng);
}

void ms_hw_ai_bind_attach_audio(ms_hw_ai_bind *b, struct ms_hw_audio *a)
{
    if (a == NULL) {
        return;
    }
    if (b != NULL && ms_hw_ai_bind_enabled(b)) {
        ms_hw_audio_set_pcm_tap(a, tap_cb, b);
    } else {
        ms_hw_audio_set_pcm_tap(a, NULL, NULL);
    }
}

void ms_hw_ai_bind_set_epochs(ms_hw_ai_bind *b, uint32_t session, uint32_t timeline,
                              uint32_t selection)
{
    if (!ms_hw_ai_bind_loaded(b) || g_set_epochs == NULL) {
        return;
    }
    g_set_epochs(b->eng, session, timeline, selection);
}

void ms_hw_ai_bind_bump_timeline(ms_hw_ai_bind *b)
{
    if (!ms_hw_ai_bind_loaded(b) || g_bump_tl == NULL) {
        return;
    }
    g_bump_tl(b->eng);
}

void ms_hw_ai_bind_bump_selection(ms_hw_ai_bind *b)
{
    if (!ms_hw_ai_bind_loaded(b) || g_bump_sel == NULL) {
        return;
    }
    g_bump_sel(b->eng);
}

ms_status ms_hw_ai_bind_drive(ms_hw_ai_bind *b)
{
    if (!ms_hw_ai_bind_loaded(b) || g_drive == NULL) {
        return MS_OK;
    }
    return g_drive(b->eng);
}

void ms_hw_ai_bind_flush(ms_hw_ai_bind *b)
{
    if (!ms_hw_ai_bind_loaded(b) || g_flush == NULL) {
        return;
    }
    g_flush(b->eng);
}

ms_sub_doc *ms_hw_ai_bind_doc(ms_hw_ai_bind *b)
{
    if (!ms_hw_ai_bind_loaded(b) || g_doc == NULL) {
        return NULL;
    }
    return g_doc(b->eng);
}

const char *ms_hw_ai_bind_status(const ms_hw_ai_bind *b)
{
    if (b == NULL) {
        return "none";
    }
    if (!ms_hw_ai_bind_loaded(b) || g_status == NULL) {
        return b->fallback_status;
    }
    return g_status(b->eng);
}

ms_status ms_hw_ai_bind_query_caps(const ms_hw_ai_bind *b, ms_ai_vendor_caps *out)
{
    if (!ms_hw_ai_bind_loaded(b) || g_query_caps == NULL) {
        return MS_ERR_UNSUPPORTED;
    }
    return g_query_caps(b->eng, out);
}

ms_status ms_hw_ai_bind_get_stats(const ms_hw_ai_bind *b, ms_ai_sub_stats *out)
{
    if (!ms_hw_ai_bind_loaded(b) || g_stats == NULL) {
        return MS_ERR_UNSUPPORTED;
    }
    return g_stats(b->eng, out);
}

ms_ai_sub *ms_hw_ai_bind_engine(ms_hw_ai_bind *b)
{
    return b != NULL ? b->eng : NULL;
}
