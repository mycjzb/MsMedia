#ifndef MSMEDIA_MS_ASS_RENDER_H
#define MSMEDIA_MS_ASS_RENDER_H

/*
 * 可选完整 ASS/SSA 栅格渲染（隔离模块 libmsass）。
 * 不链入 libmsmedia / libmsff；未加载或未构建时能力为不可用，轻量去标签路径仍可用。
 * 喂入原始 header/events，不经过截断去标签。栅格是字幕叠加，不是视频软解，不读回视频 Surface。
 * by: Long <mycjzb@gmail.com>
 */

#include "msmedia/msmedia.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MS_ASS_RENDER_BACKEND "libass"

typedef struct ms_ass_render ms_ass_render;

typedef struct ms_ass_render_caps {
    size_t struct_size;
    int available;
    int can_stroke;
    int can_transform;
    int can_move;
    int can_fade;
    int can_karaoke;
    int can_clip;
    int can_layer;
    int can_multilingual;
    const char *backend;
    const char *detail;
} ms_ass_render_caps;

typedef struct ms_ass_render_opts {
    size_t struct_size;
    int canvas_w;
    int canvas_h;
    int storage_w; /* PlayRes/存储；0 则用 canvas */
    int storage_h;
    const char *fonts_dir;     /* 宿主已授权的系统字体目录，可含多个，冒号分隔 */
    const char *default_font;  /* 已授权的系统字体文件路径；不得为下载的不明字体 */
    const char *default_family;
    int extract_fonts;         /* 默认 0：不加载脚本附件字体 */
    int glyph_cache_max;       /* 0=libass 默认 */
    int bitmap_cache_mb;       /* 0=libass 默认；上限建议 <= 32 */
    size_t max_canvas_bytes;   /* 0=canvas_w*h*4，硬顶 16 MiB */
    size_t max_events;          /* 0=4096 */
} ms_ass_render_opts;

typedef struct ms_ass_frame {
    size_t struct_size;
    int changed; /* 0 与上次相同，1 位置可能变，2 内容可能变 */
    int x;
    int y;
    int w;
    int h;
    /* 紧密 RGBA8888（R,G,B,A），预乘否：直通非预乘。所有权在 renderer，
     * 下一次 render/flush/destroy/set_canvas 前有效。 */
    const uint8_t *rgba;
    size_t rgba_size;
    int canvas_w;
    int canvas_h;
} ms_ass_frame;

MSMEDIA_API void ms_ass_render_caps_default(ms_ass_render_caps *out);
MSMEDIA_API void ms_ass_render_opts_default(ms_ass_render_opts *out);
/* 编译并链入本模块时 available=1；仅头文件而无 .so 的客户端应查询动态符号。 */
MSMEDIA_API ms_status ms_ass_render_query_caps(ms_ass_render_caps *out);

MSMEDIA_API ms_status ms_ass_render_create(const ms_allocator *alloc,
                                           const ms_ass_render_opts *opts,
                                           ms_ass_render **out);
MSMEDIA_API void ms_ass_render_destroy(ms_ass_render *r);

/* 原始 ASS/SSA 头（Script Info + Style，或含头的完整脚本前缀）。复制后不持有调用方缓冲。 */
MSMEDIA_API ms_status ms_ass_render_set_header(ms_ass_render *r,
                                               const uint8_t *data, size_t size);
/* 完整脚本（外挂文件）。会替换当前轨。不提取附件字体，除非 opts.extract_fonts=1。 */
MSMEDIA_API ms_status ms_ass_render_set_script(ms_ass_render *r,
                                               const uint8_t *data, size_t size);
/*
 * 一条原始 Dialogue 或 Matroska 压缩事件。不得传入去标签后的纯文本。
 * 时间单位 90 kHz；has_times=0 时用事件自带时间（完整 Dialogue 行）。
 */
MSMEDIA_API ms_status ms_ass_render_add_event(ms_ass_render *r,
                                              const uint8_t *data, size_t size,
                                              int has_times, int64_t start_90k, int64_t end_90k);
/* 切轨 / seek：清空事件，保留头。 */
MSMEDIA_API ms_status ms_ass_render_clear_events(ms_ass_render *r);
MSMEDIA_API ms_status ms_ass_render_set_canvas(ms_ass_render *r, int canvas_w, int canvas_h,
                                               int storage_w, int storage_h);

/*
 * 按媒体时间（90 kHz）渲染到内部 RGBA 画布。
 * out->rgba 在下一次变更前有效。changed 反映 libass detect_change。
 */
MSMEDIA_API ms_status ms_ass_render_at(ms_ass_render *r, int64_t time_90k, ms_ass_frame *out);

#ifdef __cplusplus
}
#endif

#endif /* MSMEDIA_MS_ASS_RENDER_H */
