#define _DEFAULT_SOURCE
/*
 * 可选完整 ASS 渲染宿主像素证据：多时间点真实栅格，不是只断言非空。
 * 只用本机已授权系统字体（符号链接到临时目录，不入库、不下载）。
 * 宿主渲染不是设备视觉验收，也不承诺 100% 全部 ASS 特效。
 * by: Long <mycjzb@gmail.com>
 */
#include "msmedia/ms_ass_render.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>

static int g_fail;

static void fail_msg(const char *msg)
{
    fprintf(stderr, "test_ass_render FAIL: %s\n", msg);
    g_fail = 1;
}

static const char *k_dejavu = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf";
static const char *k_noto = "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc";

static const char k_script[] =
    "[Script Info]\n"
    "ScriptType: v4.00+\n"
    "PlayResX: 640\n"
    "PlayResY: 360\n"
    "WrapStyle: 0\n"
    "ScaledBorderAndShadow: yes\n"
    "YCbCr Matrix: None\n"
    "\n"
    "[V4+ Styles]\n"
    "Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, "
    "BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, "
    "BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding\n"
    "Style: Default,DejaVu Sans,48,&H00FFFFFF,&H000000FF,&H00000000,&H00000000,"
    "0,0,0,0,100,100,0,0,1,0,0,2,10,10,10,1\n"
    "Style: Stroke,DejaVu Sans,48,&H000000FF,&H000000FF,&H00FFFFFF,&H00000000,"
    "0,0,0,0,100,100,0,0,1,6,0,2,10,10,10,1\n"
    "Style: CJK,Noto Sans CJK SC,48,&H00FFFFFF,&H000000FF,&H00000000,&H00000000,"
    "0,0,0,0,100,100,0,0,1,2,0,2,10,10,10,1\n"
    "\n"
    "[Events]\n"
    "Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n"
    "Dialogue: 0,0:00:00.00,0:00:00.80,Default,,0,0,0,,{\\pos(320,50)}LATIN\n"
    "Dialogue: 0,0:00:00.80,0:00:01.60,Stroke,,0,0,0,,{\\pos(320,50)\\bord6\\3c&HFFFFFF&\\1c&H0000FF&}STROKE\n"
    "Dialogue: 0,0:00:01.60,0:00:02.40,Default,,0,0,0,,{\\move(80,200,560,200,0,800)}MOVE\n"
    "Dialogue: 0,0:00:02.40,0:00:03.40,Default,,0,0,0,,{\\pos(320,200)\\fad(400,400)\\1c&H00FFFFFF&}FADE\n"
    "Dialogue: 0,0:00:03.40,0:00:05.00,Default,,0,0,0,,{\\pos(320,200)\\k50}KA{\\k50}RA{\\k50}OKE\n"
    "Dialogue: 0,0:00:05.00,0:00:05.80,Default,,0,0,0,,{\\pos(80,200)\\clip(0,0,160,360)}CLIPWIDE\n"
    "Dialogue: 0,0:00:05.80,0:00:06.60,Default,,0,0,0,,{\\pos(320,200)\\1c&H000000FF&\\bord0}XXXX\n"
    "Dialogue: 1,0:00:05.80,0:00:06.60,Default,,0,0,0,,{\\pos(320,200)\\1c&H00FFFFFF&\\bord0}XXXX\n"
    "Dialogue: 0,0:00:06.60,0:00:07.40,Default,,0,0,0,,{\\pos(320,200)\\t(0,800,\\fscx40\\fscy40)}SCALE\n"
    "Dialogue: 0,0:00:07.40,0:00:08.40,CJK,,0,0,0,,{\\pos(320,200)\\fnNoto Sans CJK SC}中文测试ABC\n";

typedef struct ink_stats {
    int count;
    int minx, miny, maxx, maxy;
    int64_t sum_a, sum_r, sum_g, sum_b;
    int64_t sum_x, sum_y;
    int whiteish;
    int reddish;
    int max_a;
} ink_stats;

static void ink_scan(const ms_ass_frame *fr, int x0, int y0, int x1, int y1, ink_stats *s)
{
    int x, y;
    memset(s, 0, sizeof(*s));
    s->minx = 1 << 30;
    s->miny = 1 << 30;
    s->maxx = -1;
    s->maxy = -1;
    if (fr == NULL || fr->rgba == NULL) {
        return;
    }
    if (x0 < 0) {
        x0 = 0;
    }
    if (y0 < 0) {
        y0 = 0;
    }
    if (x1 > fr->canvas_w) {
        x1 = fr->canvas_w;
    }
    if (y1 > fr->canvas_h) {
        y1 = fr->canvas_h;
    }
    for (y = y0; y < y1; y++) {
        const uint8_t *row = fr->rgba + ((size_t)y * (size_t)fr->canvas_w) * 4u;
        for (x = x0; x < x1; x++) {
            const uint8_t *p = row + (size_t)x * 4u;
            unsigned a = p[3];
            unsigned r = p[0], g = p[1], b = p[2];
            if (a < 16) {
                continue;
            }
            s->count++;
            s->sum_a += a;
            s->sum_r += r;
            s->sum_g += g;
            s->sum_b += b;
            s->sum_x += x;
            s->sum_y += y;
            if (a > (unsigned)s->max_a) {
                s->max_a = (int)a;
            }
            if (x < s->minx) {
                s->minx = x;
            }
            if (y < s->miny) {
                s->miny = y;
            }
            if (x > s->maxx) {
                s->maxx = x;
            }
            if (y > s->maxy) {
                s->maxy = y;
            }
            if (r > 180 && g > 180 && b > 180) {
                s->whiteish++;
            }
            if (r > 160 && g < 80 && b < 80) {
                s->reddish++;
            }
        }
    }
}

static int cx_of(const ink_stats *s)
{
    if (s->count <= 0) {
        return -1;
    }
    return (int)(s->sum_x / s->count);
}

static int mean_a(const ink_stats *s)
{
    if (s->count <= 0) {
        return 0;
    }
    return (int)(s->sum_a / s->count);
}

static void log_ink(const char *tag, int64_t t90, const ms_ass_frame *fr, const ink_stats *s)
{
    fprintf(stderr,
            "  %s t=%lld changed=%d dirty=%d,%d %dx%d canvas=%dx%d ink=%d "
            "bbox=%d,%d-%d,%d cx=%d meanA=%d maxA=%d white=%d red=%d\n",
            tag, (long long)t90, fr ? fr->changed : -1,
            fr ? fr->x : 0, fr ? fr->y : 0, fr ? fr->w : 0, fr ? fr->h : 0,
            fr ? fr->canvas_w : 0, fr ? fr->canvas_h : 0, s->count,
            s->minx, s->miny, s->maxx, s->maxy, cx_of(s), mean_a(s), s->max_a,
            s->whiteish, s->reddish);
}

static int render_at(ms_ass_render *r, int64_t t90, const char *tag, ms_ass_frame *fr, ink_stats *s)
{
    ms_status st = ms_ass_render_at(r, t90, fr);
    if (st != MS_OK) {
        fprintf(stderr, "  %s render status %d\n", tag, (int)st);
        fail_msg(tag);
        return -1;
    }
    if (fr->rgba == NULL || fr->rgba_size < (size_t)fr->canvas_w * (size_t)fr->canvas_h * 4u) {
        fail_msg("rgba ownership");
        return -1;
    }
    ink_scan(fr, 0, 0, fr->canvas_w, fr->canvas_h, s);
    log_ink(tag, t90, fr, s);
    return 0;
}

static int exists(const char *p)
{
    struct stat st;
    return p != NULL && stat(p, &st) == 0 && st.st_size > 0;
}

static int prep_fonts(char *dir, size_t n)
{
    char a[512], b[512];
    if (snprintf(dir, n, "/tmp/msmedia-ass-fonts-%d", (int)getpid()) >= (int)n) {
        return -1;
    }
    if (mkdir(dir, 0700) != 0 && errno != EEXIST) {
        return -1;
    }
    if (!exists(k_dejavu)) {
        fprintf(stderr, "missing system font %s\n", k_dejavu);
        return -1;
    }
    snprintf(a, sizeof(a), "%s/DejaVuSans.ttf", dir);
    unlink(a);
    if (symlink(k_dejavu, a) != 0) {
        return -1;
    }
    if (exists(k_noto)) {
        snprintf(b, sizeof(b), "%s/NotoSansCJK-Regular.ttc", dir);
        unlink(b);
        if (symlink(k_noto, b) != 0) {
            return -1;
        }
    } else {
        fprintf(stderr, "warn: no Noto CJK at %s; multilingual case may fail\n", k_noto);
    }
    return 0;
}

static void cleanup_fonts(const char *dir)
{
    char a[512];
    if (dir == NULL || dir[0] == 0) {
        return;
    }
    snprintf(a, sizeof(a), "%s/DejaVuSans.ttf", dir);
    unlink(a);
    snprintf(a, sizeof(a), "%s/NotoSansCJK-Regular.ttc", dir);
    unlink(a);
    rmdir(dir);
}

int main(void)
{
    char fonts_dir[256];
    char default_font[512];
    ms_ass_render_opts opts;
    ms_ass_render_caps caps;
    ms_ass_render *r = NULL;
    ms_ass_frame fr;
    ink_stats s, s2;
    ms_status st;
    int64_t t;
    char tmp[64];

    fonts_dir[0] = 0;
    if (prep_fonts(fonts_dir, sizeof(fonts_dir)) != 0) {
        fail_msg("prep system fonts");
        return 1;
    }
    snprintf(default_font, sizeof(default_font), "%s/DejaVuSans.ttf", fonts_dir);

    memset(&caps, 0, sizeof(caps));
    if (ms_ass_render_query_caps(&caps) != MS_OK || !caps.available ||
        caps.can_stroke == 0 || caps.can_move == 0 || caps.can_fade == 0 ||
        caps.can_karaoke == 0 || caps.can_clip == 0 || caps.can_layer == 0) {
        fail_msg("query_caps");
    }
    if (caps.backend == NULL || strstr(caps.backend, "libass") == NULL) {
        fail_msg("backend name");
    }

    ms_ass_render_opts_default(&opts);
    opts.canvas_w = 640;
    opts.canvas_h = 360;
    opts.storage_w = 640;
    opts.storage_h = 360;
    opts.fonts_dir = fonts_dir;
    opts.default_font = default_font;
    opts.default_family = "DejaVu Sans";
    opts.extract_fonts = 0;
    opts.max_events = 64;

    st = ms_ass_render_create(NULL, &opts, &r);
    if (st != MS_OK || r == NULL) {
        fprintf(stderr, "create st=%d\n", (int)st);
        fail_msg("create");
        cleanup_fonts(fonts_dir);
        return 1;
    }

    /* 生命周期：调用方缓冲在 set_script 后可改写。 */
    {
        char *copy = (char *)malloc(sizeof(k_script));
        if (copy == NULL) {
            fail_msg("nomem");
            ms_ass_render_destroy(r);
            cleanup_fonts(fonts_dir);
            return 1;
        }
        memcpy(copy, k_script, sizeof(k_script));
        st = ms_ass_render_set_script(r, (const uint8_t *)copy, strlen(copy));
        memset(copy, 'X', strlen(copy));
        free(copy);
        if (st != MS_OK) {
            fail_msg("set_script");
        }
    }

    /* 1) 文字栅格 */
    t = 36000; /* 0.40s */
    memset(&fr, 0, sizeof(fr));
    if (render_at(r, t, "raster", &fr, &s) == 0) {
        if (s.count < 80 || s.max_a < 80 || fr.w < 8 || fr.h < 8) {
            fail_msg("raster: expected solid latin ink and dirty rect");
        }
        if (s.miny > 120) {
            fail_msg("raster: expected ink near top pos y=50");
        }
    }

    /* 2) 描边：红字白边（不是只非空） */
    t = 108000; /* 1.20s */
    if (render_at(r, t, "stroke", &fr, &s) == 0) {
        if (s.reddish < 20) {
            fail_msg("stroke: expected red fill pixels");
        }
        if (s.whiteish < 20) {
            fail_msg("stroke: expected white/light outline pixels");
        }
    }

    /* 3) 移动：bbox/质心随时间右移 */
    if (render_at(r, 162000, "move-start", &fr, &s) == 0 &&
        render_at(r, 207000, "move-end", &fr, &s2) == 0) {
        int c1 = cx_of(&s), c2 = cx_of(&s2);
        if (s.count < 20 || s2.count < 20) {
            fail_msg("move: missing ink");
        } else if (c2 - c1 < 80) {
            fprintf(stderr, "  move cx %d -> %d (want end much righter)\n", c1, c2);
            fail_msg("move: centroid did not travel");
        }
    }

    /* 4) 淡入淡出：早期平均 alpha 低于中段 */
    if (render_at(r, 220500, "fade-early", &fr, &s) == 0 &&
        render_at(r, 261000, "fade-mid", &fr, &s2) == 0) {
        int a1 = mean_a(&s), a2 = mean_a(&s2);
        if (s2.count < 20) {
            fail_msg("fade: mid missing ink");
        } else if (a2 - a1 < 20 && s.max_a >= a2) {
            /* 若早期几乎看不见，count 可能很少：也算淡入成立 */
            if (s.count > 10 && a2 - a1 < 15) {
                fprintf(stderr, "  fade meanA %d -> %d\n", a1, a2);
                fail_msg("fade: mid alpha not higher than early");
            }
        }
        if (s.count >= s2.count && a1 >= a2 && s2.count > 0) {
            fail_msg("fade: early not weaker than mid");
        }
    }

    /* 5) karaoke：左半与右半在不同音节时颜色/墨量分布变化 */
    if (render_at(r, 333000, "kara-1", &fr, &s) == 0 &&
        render_at(r, 423000, "kara-3", &fr, &s2) == 0) {
        ink_stats left1, right1, left3, right3;
        if (s.count < 20 || s2.count < 20 || s.maxx <= s.minx) {
            fail_msg("karaoke: missing ink");
        } else {
            int mid = (s.minx + s.maxx) / 2;
            int mid3 = (s2.minx + s2.maxx) / 2;
            ink_scan(&fr, 0, 0, 1, 1, &left1); /* placeholder, re-render */
            /* 重新取两帧分区。先 kara-1 */
            if (ms_ass_render_at(r, 333000, &fr) == MS_OK) {
                ink_scan(&fr, s.minx, s.miny, mid, s.maxy + 1, &left1);
                ink_scan(&fr, mid, s.miny, s.maxx + 1, s.maxy + 1, &right1);
            } else {
                memset(&left1, 0, sizeof(left1));
                memset(&right1, 0, sizeof(right1));
            }
            if (ms_ass_render_at(r, 423000, &fr) == MS_OK) {
                ink_scan(&fr, s2.minx, s2.miny, mid3, s2.maxy + 1, &left3);
                ink_scan(&fr, mid3, s2.miny, s2.maxx + 1, s2.maxy + 1, &right3);
            } else {
                memset(&left3, 0, sizeof(left3));
                memset(&right3, 0, sizeof(right3));
            }
            fprintf(stderr, "  kara halves red/white L1=%d/%d R1=%d/%d L3=%d/%d R3=%d/%d\n",
                    left1.reddish, left1.whiteish, right1.reddish, right1.whiteish,
                    left3.reddish, left3.whiteish, right3.reddish, right3.whiteish);
            /* 次要色是红、主色是白：第一音节唱完后左侧更白，早期左侧更红或与右侧不同。 */
            if (left1.reddish == left3.reddish && left1.whiteish == left3.whiteish &&
                right1.reddish == right3.reddish && right1.whiteish == right3.whiteish &&
                left1.count == left3.count) {
                fail_msg("karaoke: left/right color distribution unchanged");
            }
        }
    }

    /* 6) clip：右侧大半画布应无墨 */
    if (render_at(r, 486000, "clip", &fr, &s) == 0) {
        ink_stats right;
        ink_scan(&fr, 180, 0, fr.canvas_w, fr.canvas_h, &right);
        fprintf(stderr, "  clip right-of-180 ink=%d (total %d, maxx=%d)\n",
                right.count, s.count, s.maxx);
        if (s.count < 20) {
            fail_msg("clip: missing ink in clip region");
        }
        if (right.count > 8 || s.maxx > 160) {
            fail_msg("clip: ink leaked outside clip box");
        }
    }

    /* 7) 分层：上层白色盖住下层红色，中心应偏白 */
    if (render_at(r, 558000, "layer", &fr, &s) == 0) {
        if (s.count < 20) {
            fail_msg("layer: missing ink");
        } else if (s.whiteish < 10) {
            fail_msg("layer: expected top-layer white ink");
        }
        /* 若只有下层，会几乎全是红。允许少量描边残留。 */
        if (s.reddish > s.whiteish * 4 && s.whiteish < 8) {
            fail_msg("layer: red bottom layer appears to cover white top");
        }
    }

    /* 8) 变换：缩放后墨量/bbox 变小 */
    if (render_at(r, 603000, "xf-start", &fr, &s) == 0 &&
        render_at(r, 657000, "xf-end", &fr, &s2) == 0) {
        int w1 = s.maxx - s.minx, w2 = s2.maxx - s2.minx;
        int h1 = s.maxy - s.miny, h2 = s2.maxy - s2.miny;
        if (s.count < 20 || s2.count < 8) {
            fail_msg("transform: missing ink");
        } else if (s2.count >= s.count && w2 >= w1 && h2 >= h1) {
            fprintf(stderr, "  scale bbox %dx%d -> %dx%d count %d -> %d\n",
                    w1, h1, w2, h2, s.count, s2.count);
            fail_msg("transform: expected shrink from \\t fscx/fscy");
        }
    }

    /* 9) 多语言 CJK + Latin */
    if (render_at(r, 711000, "cjk", &fr, &s) == 0) {
        if (s.count < 120) {
            fail_msg("cjk: expected dense CJK+Latin ink (Noto + DejaVu)");
        }
        if (fr.w < 40) {
            fail_msg("cjk: bbox too narrow for 中文测试ABC");
        }
    }

    /* 清空后不应再有事件墨 */
    if (ms_ass_render_clear_events(r) != MS_OK) {
        fail_msg("clear_events");
    }
    if (render_at(r, 36000, "after-clear", &fr, &s) == 0) {
        if (s.count != 0 || fr.w != 0) {
            fail_msg("clear_events left ink");
        }
    }

    /* 限额：独立 renderer */
    {
        ms_ass_render *r2 = NULL;
        int i;
        const char *hdr =
            "[Script Info]\nScriptType: v4.00+\nPlayResX: 640\nPlayResY: 360\n"
            "[V4+ Styles]\nFormat: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, "
            "OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, "
            "Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, "
            "MarginV, Encoding\n"
            "Style: Default,DejaVu Sans,20,&H00FFFFFF,&H000000FF,&H00000000,&H00000000,"
            "0,0,0,0,100,100,0,0,1,0,0,2,0,0,0,1\n"
            "[Events]\nFormat: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n";
        opts.max_events = 2;
        if (ms_ass_render_create(NULL, &opts, &r2) != MS_OK) {
            fail_msg("create limit renderer");
        } else if (ms_ass_render_set_header(r2, (const uint8_t *)hdr, strlen(hdr)) != MS_OK) {
            fail_msg("set_header");
        } else {
            for (i = 0; i < 2; i++) {
                snprintf(tmp, sizeof(tmp),
                         "Dialogue: 0,0:00:00.00,0:00:01.00,Default,,0,0,0,,E%d\n", i);
                if (ms_ass_render_add_event(r2, (const uint8_t *)tmp, strlen(tmp), 0, 0, 0) != MS_OK) {
                    fail_msg("add_event under cap");
                }
            }
            snprintf(tmp, sizeof(tmp), "Dialogue: 0,0:00:00.00,0:00:01.00,Default,,0,0,0,,E2\n");
            if (ms_ass_render_add_event(r2, (const uint8_t *)tmp, strlen(tmp), 0, 0, 0) !=
                MS_ERR_OVERFLOW) {
                fail_msg("expected OVERFLOW at event cap");
            }
        }
        ms_ass_render_destroy(r2);
    }

    ms_ass_render_destroy(r);
    cleanup_fonts(fonts_dir);

    if (g_fail) {
        fprintf(stderr, "test_ass_render: FAILED (host pixel evidence, not device visual)\n");
        return 1;
    }
    fprintf(stderr, "test_ass_render: PASS (host pixel evidence, not device visual; not 100%% of all ASS)\n");
    return 0;
}
