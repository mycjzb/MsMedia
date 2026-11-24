/*
 * 有界 ASS/SSA 文本解析与布局模型。不是完整渲染器：不做字体栅格、
 * 动画、绘图或合成到画面。不执行脚本、命令或 [Fonts]/[Graphics] 附件。
 * by: Long <mycjzb@gmail.com>
 */

#ifndef MS_SUB_STYLE_MAX
#define MS_SUB_STYLE_MAX 64
#endif
#ifndef MS_SUB_EVENT_LINE_MAX
#define MS_SUB_EVENT_LINE_MAX 4096
#endif
#ifndef MS_SUB_FMT_COLS_MAX
#define MS_SUB_FMT_COLS_MAX 32
#endif
#ifndef MS_SUB_COL_NAME_MAX
#define MS_SUB_COL_NAME_MAX 24
#endif

#ifndef MS_ASS_STYLE_DEFINED
typedef struct ms_ass_style {
    char name[MS_SUB_STYLE_NAME_MAX];
    int font_size;
    uint32_t color_argb;
    int bold;
    int italic;
    int alignment; /* already mapped to \an 1-9 */
    int margin_l, margin_r, margin_v;
    int used;
} ms_ass_style;
#endif

/* 这些字段加在 ms_sub_doc（由 ms_sub.c 定义）上：见 ms_sub.c 结构体。 */

static void style_name_copy(char *dst, size_t cap, const char *src, size_t n)
{
    size_t i = 0, o = 0;
    if (dst == NULL || cap == 0) {
        return;
    }
    dst[0] = 0;
    if (src == NULL) {
        return;
    }
    while (i < n && (src[i] == ' ' || src[i] == '\t')) {
        i++;
    }
    while (i < n && o + 1 < cap) {
        char c = src[i];
        if (c == 0) {
            break;
        }
        dst[o++] = c;
        i++;
    }
    while (o > 0 && (dst[o - 1] == ' ' || dst[o - 1] == '\t')) {
        o--;
    }
    dst[o] = 0;
}

static int str_eq_ci_n(const char *a, size_t na, const char *b)
{
    size_t i, nb;
    if (a == NULL || b == NULL) {
        return 0;
    }
    nb = strlen(b);
    if (na != nb) {
        return 0;
    }
    for (i = 0; i < na; i++) {
        unsigned ca = (unsigned char)a[i];
        unsigned cb = (unsigned char)b[i];
        if (ca >= 'A' && ca <= 'Z') {
            ca = (unsigned)(ca - 'A' + 'a');
        }
        if (cb >= 'A' && cb <= 'Z') {
            cb = (unsigned)(cb - 'A' + 'a');
        }
        if (ca != cb) {
            return 0;
        }
    }
    return 1;
}

static int name_eq_ci(const char *a, const char *b)
{
    if (a == NULL || b == NULL) {
        return 0;
    }
    return str_eq_ci_n(a, strlen(a), b);
}

static const char *skip_ws_p(const char *s, const char *e)
{
    while (s < e && (*s == ' ' || *s == '\t' || *s == '\r')) {
        s++;
    }
    return s;
}

static int looks_like_ass(const char *p, size_t n)
{
    line_iter it;
    const char *line;
    size_t len, seen = 0;
    int blank;
    if (p == NULL || n == 0) {
        return 0;
    }
    it.p = p;
    it.n = n;
    it.off = 0;
    while (next_line(&it, &line, &len, &blank) && seen < 64) {
        const char *s = skip_ws_p(line, line + len);
        size_t left = (size_t)((line + len) - s);
        seen++;
        if (left == 0) {
            continue;
        }
        if (starts_with_ci(s, left, "[Script Info]") ||
            starts_with_ci(s, left, "[V4+ Styles]") ||
            starts_with_ci(s, left, "[V4 Styles]") ||
            starts_with_ci(s, left, "[Events]") ||
            starts_with_ci(s, left, "Dialogue:") ||
            starts_with_ci(s, left, "Style:")) {
            return 1;
        }
    }
    return 0;
}

static int parse_ass_time(const char *s, size_t n, uint64_t *out_90k)
{
    /* H:MM:SS.cc 或 H:MM:SS.mmm；小时位数不限但结果须可放入 uint64。 */
    unsigned long long h = 0, m = 0, sec = 0, frac = 0;
    int frac_digits = 0;
    size_t i = 0;
    int parts = 0;
    unsigned long long cur = 0;
    int seen = 0;
    if (s == NULL || n == 0 || out_90k == NULL) {
        return -1;
    }
    while (i < n && (s[i] == ' ' || s[i] == '\t')) {
        i++;
    }
    if (i >= n || s[i] < '0' || s[i] > '9') {
        return -1;
    }
    while (i < n) {
        char c = s[i];
        if (c >= '0' && c <= '9') {
            cur = cur * 10ull + (unsigned long long)(c - '0');
            seen = 1;
            if (parts == 3) {
                frac_digits++;
                if (frac_digits > 3) {
                    /* 忽略过长小数，保持已有三位 */
                } else {
                    frac = cur;
                }
            }
            if (cur > 1000000000ull && parts < 3) {
                return -1;
            }
            i++;
            continue;
        }
        if (c == ':' && parts < 2 && seen) {
            if (parts == 0) {
                h = cur;
            } else if (parts == 1) {
                m = cur;
            }
            parts++;
            cur = 0;
            seen = 0;
            i++;
            continue;
        }
        if ((c == '.' || c == ',') && parts == 2 && seen) {
            sec = cur;
            parts = 3;
            cur = 0;
            frac = 0;
            frac_digits = 0;
            seen = 0;
            i++;
            continue;
        }
        break;
    }
    if (parts == 2 && seen) {
        sec = cur;
    } else if (parts == 3) {
        /* frac already captured */
        if (!seen && frac_digits == 0) {
            return -1;
        }
    } else {
        return -1;
    }
    if (m >= 60ull || sec >= 60ull) {
        return -1;
    }
    if (frac_digits == 1) {
        frac *= 100ull;
    } else if (frac_digits == 2) {
        frac *= 10ull;
    } else if (frac_digits == 0) {
        frac = 0;
    }
    if (frac >= 1000ull) {
        return -1;
    }
    /* 90 kHz：1 ms = 90，1 cs = 900 */
    *out_90k = ((h * 3600ull + m * 60ull + sec) * 1000ull + frac) * 90ull;
    return 0;
}

static int parse_int_token(const char *s, size_t n, int *out)
{
    size_t i = 0;
    int sign = 1;
    long v = 0;
    int any = 0;
    if (out == NULL) {
        return -1;
    }
    while (i < n && (s[i] == ' ' || s[i] == '\t')) {
        i++;
    }
    if (i < n && s[i] == '-') {
        sign = -1;
        i++;
    } else if (i < n && s[i] == '+') {
        i++;
    }
    while (i < n && s[i] >= '0' && s[i] <= '9') {
        any = 1;
        v = v * 10 + (s[i] - '0');
        if (v > 1000000) {
            v = 1000000;
        }
        i++;
    }
    if (!any) {
        return -1;
    }
    *out = sign * (int)v;
    return 0;
}

static uint32_t ass_color_to_argb(unsigned long v, int have_alpha)
{
    unsigned a, r, g, b;
    b = (unsigned)((v >> 16) & 0xFFul);
    g = (unsigned)((v >> 8) & 0xFFul);
    r = (unsigned)(v & 0xFFul);
    if (have_alpha) {
        a = (unsigned)((v >> 24) & 0xFFul);
        /* ASS 00=不透明，FF=全透明；转为 Android/常规 A=FF 不透明。 */
        a = 255u - a;
    } else {
        a = 255u;
    }
    return (a << 24) | (r << 16) | (g << 8) | b;
}

static int parse_ass_color(const char *s, size_t n, uint32_t *out)
{
    size_t i = 0;
    unsigned long v = 0;
    int digits = 0;
    int hex = 0;
    if (out == NULL) {
        return -1;
    }
    while (i < n && (s[i] == ' ' || s[i] == '\t')) {
        i++;
    }
    if (i + 1 < n && s[i] == '&' && (s[i + 1] == 'H' || s[i + 1] == 'h')) {
        hex = 1;
        i += 2;
    } else if (i < n && (s[i] == 'H' || s[i] == 'h')) {
        hex = 1;
        i++;
    }
    if (hex) {
        while (i < n) {
            unsigned char c = (unsigned char)s[i];
            unsigned d;
            if (c >= '0' && c <= '9') {
                d = (unsigned)(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                d = (unsigned)(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                d = (unsigned)(c - 'A' + 10);
            } else {
                break;
            }
            v = (v << 4) | d;
            digits++;
            if (digits > 8) {
                return -1;
            }
            i++;
        }
        if (digits == 0) {
            return -1;
        }
        *out = ass_color_to_argb(v, digits > 6);
        return 0;
    }
    /* 十进制（SSA 常见） */
    if (i < n && s[i] == '-') {
        /* 负十进制按无符号 32 位位型解释 */
        int iv = 0;
        if (parse_int_token(s, n, &iv) != 0) {
            return -1;
        }
        v = (unsigned long)(uint32_t)(int32_t)iv;
        *out = ass_color_to_argb(v, 1);
        return 0;
    }
    if (parse_int_token(s, n, (int *)&digits) == 0 && digits >= 0) {
        /* 上面会破坏；单独扫十进制 */
    }
    {
        unsigned long dec = 0;
        int any = 0;
        i = 0;
        while (i < n && (s[i] == ' ' || s[i] == '\t')) {
            i++;
        }
        while (i < n && s[i] >= '0' && s[i] <= '9') {
            any = 1;
            dec = dec * 10ul + (unsigned long)(s[i] - '0');
            i++;
        }
        if (!any) {
            return -1;
        }
        *out = ass_color_to_argb(dec, dec > 0xFFFFFFul);
        return 0;
    }
}

static int ssa_align_to_an(int a)
{
    /* SSA: 1-3 底，5-7 顶，9-11 中；其它值尽量夹到 1-9。 */
    switch (a) {
    case 1: return 1;
    case 2: return 2;
    case 3: return 3;
    case 5: return 7;
    case 6: return 8;
    case 7: return 9;
    case 9: return 4;
    case 10: return 5;
    case 11: return 6;
    default:
        if (a >= 1 && a <= 9) {
            return a;
        }
        return 2;
    }
}

static int split_csv_limited(const char *s, size_t n, int expect,
                             const char **fs, size_t *fl, int last_rest)
{
    /* 按逗号切 expect 列；last_rest 时最后一列吃掉剩余（Text 可含逗号）。 */
    int col = 0;
    size_t i = 0, start = 0;
    if (expect <= 0 || expect > MS_SUB_FMT_COLS_MAX) {
        return -1;
    }
    while (i <= n && col < expect) {
        int end = (i == n) || (s[i] == ',' && !(last_rest && col == expect - 1));
        if (!end) {
            i++;
            continue;
        }
        fs[col] = s + start;
        fl[col] = i - start;
        col++;
        if (i == n) {
            break;
        }
        i++;
        start = i;
    }
    return col;
}

static void parse_format_line(const char *s, size_t n, char cols[][MS_SUB_COL_NAME_MAX],
                              int *ncols)
{
    const char *fs[MS_SUB_FMT_COLS_MAX];
    size_t fl[MS_SUB_FMT_COLS_MAX];
    int c, i;
    *ncols = 0;
    c = split_csv_limited(s, n, MS_SUB_FMT_COLS_MAX, fs, fl, 0);
    if (c < 0) {
        return;
    }
    for (i = 0; i < c && *ncols < MS_SUB_FMT_COLS_MAX; i++) {
        style_name_copy(cols[*ncols], MS_SUB_COL_NAME_MAX, fs[i], fl[i]);
        if (cols[*ncols][0] != 0) {
            (*ncols)++;
        }
    }
}

static int col_index(char cols[][MS_SUB_COL_NAME_MAX], int ncols, const char *name)
{
    int i;
    for (i = 0; i < ncols; i++) {
        if (name_eq_ci(cols[i], name)) {
            return i;
        }
    }
    return -1;
}

static const ms_ass_style *find_style(const ms_sub_doc *doc, const char *name)
{
    size_t i;
    if (doc == NULL) {
        return NULL;
    }
    if (name != NULL && name[0] != 0) {
        for (i = 0; i < doc->nstyles; i++) {
            if (doc->styles[i].used && strcmp(doc->styles[i].name, name) == 0) {
                return &doc->styles[i];
            }
        }
        for (i = 0; i < doc->nstyles; i++) {
            if (doc->styles[i].used && name_eq_ci(doc->styles[i].name, name)) {
                return &doc->styles[i];
            }
        }
    }
    for (i = 0; i < doc->nstyles; i++) {
        if (doc->styles[i].used && name_eq_ci(doc->styles[i].name, "Default")) {
            return &doc->styles[i];
        }
    }
    if (doc->nstyles > 0 && doc->styles[0].used) {
        return &doc->styles[0];
    }
    return NULL;
}

static void apply_style_rec(const ms_ass_style *st, ms_sub_cue *cue)
{
    if (st == NULL || cue == NULL) {
        return;
    }
    cue->font_size = st->font_size;
    if (st->font_size > 0) {
        cue->style_flags |= MS_SUB_STYLE_HAS_SIZE;
    }
    cue->color_argb = st->color_argb;
    cue->style_flags |= MS_SUB_STYLE_HAS_COLOR;
    cue->bold = st->bold != 0;
    cue->italic = st->italic != 0;
    cue->style_flags |= MS_SUB_STYLE_HAS_BOLD | MS_SUB_STYLE_HAS_ITALIC;
    if (st->alignment >= 1 && st->alignment <= 9) {
        cue->alignment = st->alignment;
        cue->style_flags |= MS_SUB_STYLE_HAS_ALIGN;
    }
    cue->margin_l = st->margin_l;
    cue->margin_r = st->margin_r;
    cue->margin_v = st->margin_v;
    cue->style_flags |= MS_SUB_STYLE_HAS_MARGIN;
    style_name_copy(cue->style_name, sizeof(cue->style_name), st->name, strlen(st->name));
    if (cue->style_name[0] != 0) {
        cue->style_flags |= MS_SUB_STYLE_HAS_NAME;
    }
}

static void mark_cap(ms_sub_cue *cue, uint32_t bit)
{
    if (cue == NULL) {
        return;
    }
    cue->cap_flags |= bit | MS_SUB_CAP_DEGRADED;
}

static void skip_paren(const char *s, size_t n, size_t *i)
{
    int depth = 0;
    if (*i < n && s[*i] == '(') {
        depth = 1;
        (*i)++;
        while (*i < n && depth > 0) {
            if (s[*i] == '(') {
                depth++;
            } else if (s[*i] == ')') {
                depth--;
            }
            (*i)++;
        }
    }
}

static void close_span_if_needed(ms_sub_cue *cue, size_t text_len)
{
    ms_sub_span *sp;
    if (cue->span_count == 0) {
        return;
    }
    sp = &cue->spans[cue->span_count - 1];
    if (sp->end == 0 && sp->start <= text_len) {
        sp->end = text_len;
    }
    if (sp->end <= sp->start) {
        cue->span_count--;
    }
}

static void open_span(ms_sub_cue *cue, size_t text_len)
{
    ms_sub_span *sp;
    if (cue->span_count >= MS_SUB_SPAN_MAX) {
        mark_cap(cue, MS_SUB_CAP_INLINE_SPAN_LIMIT);
        return;
    }
    close_span_if_needed(cue, text_len);
    if (cue->span_count >= MS_SUB_SPAN_MAX) {
        mark_cap(cue, MS_SUB_CAP_INLINE_SPAN_LIMIT);
        return;
    }
    sp = &cue->spans[cue->span_count];
    memset(sp, 0, sizeof(*sp));
    sp->start = text_len;
    sp->end = 0;
    sp->style_flags = 0;
    if (cue->style_flags & MS_SUB_STYLE_HAS_COLOR) {
        sp->color_argb = cue->color_argb;
        sp->style_flags |= MS_SUB_STYLE_HAS_COLOR;
    }
    if (cue->style_flags & MS_SUB_STYLE_HAS_SIZE) {
        sp->font_size = cue->font_size;
        sp->style_flags |= MS_SUB_STYLE_HAS_SIZE;
    }
    if (cue->style_flags & MS_SUB_STYLE_HAS_BOLD) {
        sp->bold = cue->bold;
        sp->style_flags |= MS_SUB_STYLE_HAS_BOLD;
    }
    if (cue->style_flags & MS_SUB_STYLE_HAS_ITALIC) {
        sp->italic = cue->italic;
        sp->style_flags |= MS_SUB_STYLE_HAS_ITALIC;
    }
    cue->span_count++;
}

static int utf8_out_cp(char *out, size_t cap, size_t *o, unsigned char c)
{
    /* 只写入单字节或已校验的序列；调用方保证不截断码点。 */
    if (*o + 1 >= cap) {
        return -1;
    }
    out[(*o)++] = (char)c;
    return 0;
}

static void parse_override_tags(const ms_sub_doc *doc, const char *s, size_t n,
                                ms_sub_cue *cue, char *out, size_t cap, size_t *out_n)
{
    size_t i = 0, o = 0;
    int drawing = 0;
    int had_tags = 0;
    unsigned cur_bold = (unsigned)cue->bold;
    unsigned cur_italic = (unsigned)cue->italic;
    uint32_t cur_color = cue->color_argb;
    int cur_size = cue->font_size;
    const char *base_style = cue->style_name;
    (void)base_style;
    *out_n = 0;
    if (cap == 0) {
        return;
    }
    out[0] = 0;
    while (i < n) {
        unsigned char c = (unsigned char)s[i];
        if (c == '{') {
            size_t j = i + 1;
            had_tags = 1;
            while (j < n && s[j] != '}') {
                j++;
            }
            if (j >= n) {
                /* 畸形未闭合标签：丢弃剩余覆盖，不把 '{' 当文本输出 */
                break;
            }
            {
                size_t k = i + 1;
                int style_changed = 0;
                while (k < j) {
                    if (s[k] == '\\') {
                        size_t name_s, t;
                        char tag[16];
                        size_t tn = 0;
                        k++;
                        name_s = k;
                        while (k < j && ((s[k] >= 'A' && s[k] <= 'Z') ||
                                         (s[k] >= 'a' && s[k] <= 'z'))) {
                            if (tn + 1 < sizeof(tag)) {
                                tag[tn++] = s[k];
                            }
                            k++;
                        }
                        tag[tn] = 0;
                        if (tn == 0) {
                            /* \N \n \h 或未知单字符 */
                            if (k < j && (s[k] == 'N' || s[k] == 'n')) {
                                /* 在花括号里少见；仍当换行处理 */
                                k++;
                                if (!drawing) {
                                    if (utf8_out_cp(out, cap, &o, '\n') != 0) {
                                        goto done;
                                    }
                                }
                            } else if (k < j && s[k] == 'h') {
                                k++;
                                if (!drawing) {
                                    if (utf8_out_cp(out, cap, &o, ' ') != 0) {
                                        goto done;
                                    }
                                }
                            } else {
                                k++;
                            }
                            continue;
                        }
                        if (strcmp(tag, "pos") == 0) {
                            int x = 0, y = 0;
                            skip_ws_p(s + k, s + j); /* no-op on k */
                            while (k < j && (s[k] == ' ' || s[k] == '\t')) {
                                k++;
                            }
                            if (k < j && s[k] == '(') {
                                const char *p1 = s + k + 1;
                                const char *comma = memchr(p1, ',', (size_t)(s + j - p1));
                                skip_paren(s, j, &k);
                                if (comma != NULL &&
                                    parse_int_token(p1, (size_t)(comma - p1), &x) == 0 &&
                                    parse_int_token(comma + 1, (size_t)(s + j - (comma + 1)), &y) == 0) {
                                    cue->pos_x = x;
                                    cue->pos_y = y;
                                    cue->style_flags |= MS_SUB_STYLE_HAS_POS;
                                }
                            }
                        } else if (strcmp(tag, "move") == 0) {
                            mark_cap(cue, MS_SUB_CAP_UNSUPPORTED_MOVE);
                            skip_paren(s, j, &k);
                        } else if (strcmp(tag, "t") == 0 || strcmp(tag, "frx") == 0 ||
                                   strcmp(tag, "fry") == 0 || strcmp(tag, "frz") == 0 ||
                                   strcmp(tag, "fr") == 0 || strcmp(tag, "fax") == 0 ||
                                   strcmp(tag, "fay") == 0 || strcmp(tag, "org") == 0 ||
                                   strcmp(tag, "fscx") == 0 || strcmp(tag, "fscy") == 0 ||
                                   strcmp(tag, "fsp") == 0) {
                            mark_cap(cue, MS_SUB_CAP_UNSUPPORTED_TRANSFORM);
                            skip_paren(s, j, &k);
                        } else if (strcmp(tag, "fad") == 0 || strcmp(tag, "fade") == 0) {
                            mark_cap(cue, MS_SUB_CAP_UNSUPPORTED_FADE);
                            skip_paren(s, j, &k);
                        } else if (strcmp(tag, "clip") == 0 || strcmp(tag, "iclip") == 0) {
                            mark_cap(cue, MS_SUB_CAP_UNSUPPORTED_CLIP);
                            skip_paren(s, j, &k);
                        } else if (strcmp(tag, "k") == 0 || strcmp(tag, "K") == 0 ||
                                   strcmp(tag, "kf") == 0 || strcmp(tag, "ko") == 0) {
                            mark_cap(cue, MS_SUB_CAP_UNSUPPORTED_KARAOKE);
                            while (k < j && ((s[k] >= '0' && s[k] <= '9') || s[k] == '.' ||
                                              s[k] == ' ' || s[k] == '-')) {
                                k++;
                            }
                        } else if (strcmp(tag, "be") == 0 || strcmp(tag, "blur") == 0) {
                            mark_cap(cue, MS_SUB_CAP_UNSUPPORTED_BLUR);
                            while (k < j && s[k] != '\\') {
                                k++;
                            }
                        } else if (strcmp(tag, "p") == 0) {
                            int pv = 0;
                            const char *ns = s + k;
                            size_t left = (size_t)(j - k);
                            if (parse_int_token(ns, left, &pv) == 0) {
                                drawing = (pv != 0);
                                if (drawing) {
                                    mark_cap(cue, MS_SUB_CAP_UNSUPPORTED_DRAWING);
                                }
                                while (k < j && ((s[k] >= '0' && s[k] <= '9') || s[k] == '-' ||
                                                  s[k] == ' ' || s[k] == '+')) {
                                    k++;
                                }
                            }
                        } else if (strcmp(tag, "fn") == 0) {
                            mark_cap(cue, MS_SUB_CAP_UNSUPPORTED_FONT);
                            while (k < j && s[k] != '\\') {
                                k++;
                            }
                        } else if (strcmp(tag, "q") == 0) {
                            mark_cap(cue, MS_SUB_CAP_UNSUPPORTED_WRAP);
                            while (k < j && s[k] != '\\') {
                                k++;
                            }
                        } else if (strcmp(tag, "an") == 0) {
                            int an = 0;
                            if (parse_int_token(s + k, (size_t)(j - k), &an) == 0 &&
                                an >= 1 && an <= 9) {
                                cue->alignment = an;
                                cue->style_flags |= MS_SUB_STYLE_HAS_ALIGN;
                            }
                            while (k < j && ((s[k] >= '0' && s[k] <= '9') || s[k] == ' ')) {
                                k++;
                            }
                        } else if (strcmp(tag, "a") == 0) {
                            int a = 0;
                            if (parse_int_token(s + k, (size_t)(j - k), &a) == 0) {
                                cue->alignment = ssa_align_to_an(a);
                                cue->style_flags |= MS_SUB_STYLE_HAS_ALIGN;
                            }
                            while (k < j && ((s[k] >= '0' && s[k] <= '9') || s[k] == ' ')) {
                                k++;
                            }
                        } else if (strcmp(tag, "fs") == 0) {
                            int fs = 0;
                            if (parse_int_token(s + k, (size_t)(j - k), &fs) == 0 && fs > 0) {
                                cur_size = fs;
                                cue->font_size = fs;
                                cue->style_flags |= MS_SUB_STYLE_HAS_SIZE;
                                style_changed = 1;
                            }
                            while (k < j && ((s[k] >= '0' && s[k] <= '9') || s[k] == '.' ||
                                              s[k] == ' ' || s[k] == '+' || s[k] == '-')) {
                                k++;
                            }
                        } else if (strcmp(tag, "b") == 0) {
                            int b = 1;
                            if (k < j && (s[k] == '0' || s[k] == '1' || s[k] == '-')) {
                                (void)parse_int_token(s + k, (size_t)(j - k), &b);
                            }
                            cur_bold = (b != 0);
                            cue->bold = cur_bold;
                            cue->style_flags |= MS_SUB_STYLE_HAS_BOLD;
                            style_changed = 1;
                            while (k < j && ((s[k] >= '0' && s[k] <= '9') || s[k] == '-' ||
                                              s[k] == ' ')) {
                                k++;
                            }
                        } else if (strcmp(tag, "i") == 0) {
                            int iv = 1;
                            if (k < j && (s[k] == '0' || s[k] == '1' || s[k] == '-')) {
                                (void)parse_int_token(s + k, (size_t)(j - k), &iv);
                            }
                            cur_italic = (iv != 0);
                            cue->italic = cur_italic;
                            cue->style_flags |= MS_SUB_STYLE_HAS_ITALIC;
                            style_changed = 1;
                            while (k < j && ((s[k] >= '0' && s[k] <= '9') || s[k] == '-' ||
                                              s[k] == ' ')) {
                                k++;
                            }
                        } else if (strcmp(tag, "c") == 0 || strcmp(tag, "1c") == 0) {
                            uint32_t col = 0;
                            while (k < j && (s[k] == ' ' || s[k] == '\t')) {
                                k++;
                            }
                            t = k;
                            while (t < j && s[t] != '\\') {
                                t++;
                            }
                            if (parse_ass_color(s + k, t - k, &col) == 0) {
                                cur_color = col;
                                cue->color_argb = col;
                                cue->style_flags |= MS_SUB_STYLE_HAS_COLOR;
                                style_changed = 1;
                            }
                            k = t;
                        } else if (strcmp(tag, "alpha") == 0 || strcmp(tag, "1a") == 0) {
                            uint32_t col = 0;
                            unsigned a;
                            t = k;
                            while (t < j && s[t] != '\\') {
                                t++;
                            }
                            if (parse_ass_color(s + k, t - k, &col) == 0) {
                                /* 只取 alpha 通道应用到当前色 */
                                a = (col >> 24) & 0xFFu;
                                cue->color_argb = (a << 24) | (cue->color_argb & 0x00FFFFFFu);
                                cur_color = cue->color_argb;
                                cue->style_flags |= MS_SUB_STYLE_HAS_COLOR;
                                style_changed = 1;
                            }
                            k = t;
                        } else if (strcmp(tag, "r") == 0) {
                            char rname[MS_SUB_STYLE_NAME_MAX];
                            const ms_ass_style *st;
                            t = k;
                            while (t < j && s[t] != '\\') {
                                t++;
                            }
                            style_name_copy(rname, sizeof(rname), s + k, t - k);
                            st = find_style(doc, rname[0] ? rname : cue->style_name);
                            apply_style_rec(st, cue);
                            cur_bold = (unsigned)cue->bold;
                            cur_italic = (unsigned)cue->italic;
                            cur_color = cue->color_argb;
                            cur_size = cue->font_size;
                            style_changed = 1;
                            k = t;
                        } else if (strcmp(tag, "u") == 0 || strcmp(tag, "s") == 0 ||
                                   strcmp(tag, "bord") == 0 || strcmp(tag, "shad") == 0 ||
                                   strcmp(tag, "xbord") == 0 || strcmp(tag, "ybord") == 0 ||
                                   strcmp(tag, "xshad") == 0 || strcmp(tag, "yshad") == 0 ||
                                   strcmp(tag, "2c") == 0 || strcmp(tag, "3c") == 0 ||
                                   strcmp(tag, "4c") == 0 || strcmp(tag, "2a") == 0 ||
                                   strcmp(tag, "3a") == 0 || strcmp(tag, "4a") == 0) {
                            /* 认识但不应用：边框/阴影/其它颜色，记 stripped 即可 */
                            cue->cap_flags |= MS_SUB_CAP_STRIPPED_TAGS;
                            while (k < j && s[k] != '\\') {
                                k++;
                            }
                        } else {
                            cue->cap_flags |= MS_SUB_CAP_STRIPPED_TAGS;
                            while (k < j && s[k] != '\\') {
                                k++;
                            }
                        }
                        (void)name_s;
                        (void)cur_size;
                        (void)cur_color;
                    } else {
                        k++;
                    }
                    (void)style_changed;
                }
                if (style_changed) {
                    open_span(cue, o);
                    if (cue->span_count > 0) {
                        ms_sub_span *sp = &cue->spans[cue->span_count - 1];
                        sp->bold = (int)cur_bold;
                        sp->italic = (int)cur_italic;
                        sp->color_argb = cur_color;
                        sp->font_size = cur_size;
                        sp->style_flags = 0;
                        if (cue->style_flags & MS_SUB_STYLE_HAS_BOLD) {
                            sp->style_flags |= MS_SUB_STYLE_HAS_BOLD;
                        }
                        if (cue->style_flags & MS_SUB_STYLE_HAS_ITALIC) {
                            sp->style_flags |= MS_SUB_STYLE_HAS_ITALIC;
                        }
                        if (cue->style_flags & MS_SUB_STYLE_HAS_COLOR) {
                            sp->style_flags |= MS_SUB_STYLE_HAS_COLOR;
                        }
                        if (cue->style_flags & MS_SUB_STYLE_HAS_SIZE) {
                            sp->style_flags |= MS_SUB_STYLE_HAS_SIZE;
                        }
                    }
                }
            }
            i = j + 1;
            continue;
        }
        /* 文本转义 */
        if (c == '\\' && i + 1 < n) {
            char nch = s[i + 1];
            if (nch == 'N' || nch == 'n') {
                if (!drawing) {
                    if (utf8_out_cp(out, cap, &o, '\n') != 0) {
                        goto done;
                    }
                }
                i += 2;
                continue;
            }
            if (nch == 'h') {
                if (!drawing) {
                    if (utf8_out_cp(out, cap, &o, ' ') != 0) {
                        goto done;
                    }
                }
                i += 2;
                continue;
            }
        }
        if (drawing) {
            i++;
            continue;
        }
        /* 控制字符：允许 \n \t；丢弃其它 C0 / DEL */
        if (c < 0x20 && c != '\n' && c != '\t') {
            i++;
            continue;
        }
        if (c == 0x7F) {
            i++;
            continue;
        }
        if (c < 0x80) {
            if (utf8_out_cp(out, cap, &o, c) != 0) {
                goto done;
            }
            i++;
            continue;
        }
        /* 拷贝一个 UTF-8 序列；非法则跳过该字节 */
        {
            size_t need = 0;
            size_t k;
            if ((c & 0xE0) == 0xC0) {
                need = 2;
            } else if ((c & 0xF0) == 0xE0) {
                need = 3;
            } else if ((c & 0xF8) == 0xF0) {
                need = 4;
            } else {
                i++;
                continue;
            }
            if (i + need > n) {
                break;
            }
            for (k = 1; k < need; k++) {
                if ((((unsigned char)s[i + k]) & 0xC0) != 0x80) {
                    need = 0;
                    break;
                }
            }
            if (need == 0) {
                i++;
                continue;
            }
            /* 拒绝过长编码与代理项、超平面 */
            {
                unsigned cp;
                if (need == 2) {
                    cp = ((c & 0x1Fu) << 6) | ((unsigned char)s[i + 1] & 0x3Fu);
                    if (cp < 0x80) {
                        i += 2;
                        continue;
                    }
                } else if (need == 3) {
                    cp = ((c & 0x0Fu) << 12) | (((unsigned char)s[i + 1] & 0x3Fu) << 6) |
                         ((unsigned char)s[i + 2] & 0x3Fu);
                    if (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF)) {
                        i += 3;
                        continue;
                    }
                } else {
                    cp = ((c & 0x07u) << 18) | (((unsigned char)s[i + 1] & 0x3Fu) << 12) |
                         (((unsigned char)s[i + 2] & 0x3Fu) << 6) |
                         ((unsigned char)s[i + 3] & 0x3Fu);
                    if (cp < 0x10000 || cp > 0x10FFFF) {
                        i += 4;
                        continue;
                    }
                }
            }
            if (o + need >= cap) {
                goto done;
            }
            memcpy(out + o, s + i, need);
            o += need;
            i += need;
        }
    }
done:
    close_span_if_needed(cue, o);
    if (cue->span_count == 1 && cue->spans[0].start == 0 && cue->spans[0].end == o) {
        /* 整段单一样式，不必保留 span */
        cue->span_count = 0;
    }
    if (had_tags) {
        cue->cap_flags |= MS_SUB_CAP_STRIPPED_TAGS;
    }
    out[o] = 0;
    *out_n = o;
}

static int starts_section(const char *s, size_t n, const char *name)
{
    return starts_with_ci(s, n, name);
}

static int is_skip_section(const char *s, size_t n)
{
    return starts_section(s, n, "[Fonts]") || starts_section(s, n, "[Graphics]") ||
           starts_section(s, n, "[Aegisub") || starts_section(s, n, "[sitas]");
}

static void trim_copy_val(const char *s, size_t n, const char **o, size_t *on)
{
    while (n > 0 && (s[0] == ' ' || s[0] == '\t')) {
        s++;
        n--;
    }
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r')) {
        n--;
    }
    *o = s;
    *on = n;
}

static void apply_key_value(ms_sub_doc *doc, const char *line, size_t n)
{
    const char *colon;
    const char *key, *val;
    size_t kn, vn;
    colon = memchr(line, ':', n);
    if (colon == NULL) {
        return;
    }
    trim_copy_val(line, (size_t)(colon - line), &key, &kn);
    trim_copy_val(colon + 1, (size_t)((line + n) - (colon + 1)), &val, &vn);
    if (str_eq_ci_n(key, kn, "PlayResX")) {
        int v = 0;
        if (parse_int_token(val, vn, &v) == 0 && v > 0) {
            doc->play_res_x = v;
        }
    } else if (str_eq_ci_n(key, kn, "PlayResY")) {
        int v = 0;
        if (parse_int_token(val, vn, &v) == 0 && v > 0) {
            doc->play_res_y = v;
        }
    } else if (str_eq_ci_n(key, kn, "ScriptType")) {
        /* v4.00 无 + 视为 SSA 对齐习惯 */
        if (vn >= 5 && val[0] == 'v' && val[1] == '4') {
            size_t i;
            int plus = 0;
            for (i = 0; i < vn; i++) {
                if (val[i] == '+') {
                    plus = 1;
                }
            }
            if (!plus) {
                doc->is_ssa = 1;
            } else {
                doc->is_ssa = 0;
            }
        }
    } else if (str_eq_ci_n(key, kn, "WrapStyle")) {
        (void)val;
        /* 记录但不实现换行算法 */
    }
}

static void add_style_from_line(ms_sub_doc *doc, char cols[][MS_SUB_COL_NAME_MAX],
                                int ncols, const char *body, size_t n, int ssa)
{
    const char *fs[MS_SUB_FMT_COLS_MAX];
    size_t fl[MS_SUB_FMT_COLS_MAX];
    int c, idx;
    ms_ass_style *st;
    if (doc->nstyles >= MS_SUB_STYLE_MAX) {
        return;
    }
    if (ncols <= 0) {
        /* 使用默认列 */
        return;
    }
    c = split_csv_limited(body, n, ncols, fs, fl, 0);
    if (c <= 0) {
        return;
    }
    st = &doc->styles[doc->nstyles];
    memset(st, 0, sizeof(*st));
    st->font_size = 18;
    st->color_argb = 0xFFFFFFFFu;
    st->alignment = 2;
    idx = col_index(cols, ncols, "Name");
    if (idx >= 0 && idx < c) {
        style_name_copy(st->name, sizeof(st->name), fs[idx], fl[idx]);
    }
    if (st->name[0] == 0) {
        return;
    }
    idx = col_index(cols, ncols, "Fontsize");
    if (idx >= 0 && idx < c) {
        int fsiz = 0;
        if (parse_int_token(fs[idx], fl[idx], &fsiz) == 0 && fsiz > 0) {
            st->font_size = fsiz;
        }
    }
    idx = col_index(cols, ncols, "PrimaryColour");
    if (idx >= 0 && idx < c) {
        uint32_t col = 0;
        if (parse_ass_color(fs[idx], fl[idx], &col) == 0) {
            st->color_argb = col;
        }
    }
    idx = col_index(cols, ncols, "Bold");
    if (idx >= 0 && idx < c) {
        int b = 0;
        if (parse_int_token(fs[idx], fl[idx], &b) == 0) {
            st->bold = (b != 0);
        }
    }
    idx = col_index(cols, ncols, "Italic");
    if (idx >= 0 && idx < c) {
        int b = 0;
        if (parse_int_token(fs[idx], fl[idx], &b) == 0) {
            st->italic = (b != 0);
        }
    }
    idx = col_index(cols, ncols, "Alignment");
    if (idx >= 0 && idx < c) {
        int a = 0;
        if (parse_int_token(fs[idx], fl[idx], &a) == 0) {
            st->alignment = ssa ? ssa_align_to_an(a) : ((a >= 1 && a <= 9) ? a : 2);
        }
    }
    idx = col_index(cols, ncols, "MarginL");
    if (idx >= 0 && idx < c) {
        (void)parse_int_token(fs[idx], fl[idx], &st->margin_l);
    }
    idx = col_index(cols, ncols, "MarginR");
    if (idx >= 0 && idx < c) {
        (void)parse_int_token(fs[idx], fl[idx], &st->margin_r);
    }
    idx = col_index(cols, ncols, "MarginV");
    if (idx >= 0 && idx < c) {
        (void)parse_int_token(fs[idx], fl[idx], &st->margin_v);
    }
    st->used = 1;
    doc->nstyles++;
}

static void reset_styles(ms_sub_doc *doc, int keep_cues)
{
    (void)keep_cues;
    memset(doc->styles, 0, sizeof(doc->styles));
    doc->nstyles = 0;
    doc->play_res_x = 0;
    doc->play_res_y = 0;
    /* is_ssa / fmt 由调用方决定 */
}

static void parse_header_body(ms_sub_doc *doc, const char *p, size_t n)
{
    line_iter it;
    const char *line;
    size_t len;
    int blank;
    int section = 0; /* 0 none, 1 info, 2 styles, 3 events, 4 skip */
    char style_cols[MS_SUB_FMT_COLS_MAX][MS_SUB_COL_NAME_MAX];
    int style_ncols = 0;
    char event_cols[MS_SUB_FMT_COLS_MAX][MS_SUB_COL_NAME_MAX];
    int event_ncols = 0;
    int ssa_styles = doc->is_ssa;
    memset(style_cols, 0, sizeof(style_cols));
    memset(event_cols, 0, sizeof(event_cols));
    it.p = p;
    it.n = n;
    it.off = 0;
    while (next_line(&it, &line, &len, &blank)) {
        const char *s = skip_ws_p(line, line + len);
        size_t left = (size_t)((line + len) - s);
        if (left == 0 || s[0] == ';' || (left >= 1 && s[0] == '!')) {
            continue;
        }
        if (s[0] == '[') {
            if (starts_section(s, left, "[Script Info]")) {
                section = 1;
            } else if (starts_section(s, left, "[V4+ Styles]")) {
                section = 2;
                ssa_styles = 0;
                doc->is_ssa = 0;
            } else if (starts_section(s, left, "[V4 Styles]")) {
                section = 2;
                ssa_styles = 1;
                doc->is_ssa = 1;
            } else if (starts_section(s, left, "[Events]")) {
                section = 3;
            } else if (is_skip_section(s, left)) {
                section = 4;
            } else {
                section = 4;
            }
            continue;
        }
        if (section == 4) {
            continue;
        }
        if (section == 1) {
            apply_key_value(doc, s, left);
            continue;
        }
        if (section == 2) {
            if (starts_with_ci(s, left, "Format:")) {
                const char *b = skip_ws_p(s + 7, s + left);
                parse_format_line(b, (size_t)((s + left) - b), style_cols, &style_ncols);
            } else if (starts_with_ci(s, left, "Style:")) {
                const char *b = skip_ws_p(s + 6, s + left);
                if (style_ncols == 0) {
                    /* 缺省 ASS 列名 */
                    const char *def =
                        "Name,Fontname,Fontsize,PrimaryColour,SecondaryColour,OutlineColour,"
                        "BackColour,Bold,Italic,Underline,StrikeOut,ScaleX,ScaleY,Spacing,"
                        "Angle,BorderStyle,Outline,Shadow,Alignment,MarginL,MarginR,MarginV,"
                        "Encoding";
                    parse_format_line(def, strlen(def), style_cols, &style_ncols);
                }
                add_style_from_line(doc, style_cols, style_ncols, b,
                                    (size_t)((s + left) - b), ssa_styles);
            }
            continue;
        }
        if (section == 3 && starts_with_ci(s, left, "Format:")) {
            const char *b = skip_ws_p(s + 7, s + left);
            parse_format_line(b, (size_t)((s + left) - b), event_cols, &event_ncols);
            memcpy(doc->event_cols, event_cols, sizeof(event_cols));
            doc->event_ncols = event_ncols;
        }
    }
}

static void default_event_format(ms_sub_doc *doc)
{
    if (doc->event_ncols > 0) {
        return;
    }
    if (doc->is_ssa) {
        const char *def = "Marked,Start,End,Style,Name,MarginL,MarginR,MarginV,Effect,Text";
        parse_format_line(def, strlen(def), doc->event_cols, &doc->event_ncols);
    } else {
        const char *def = "Layer,Start,End,Style,Name,MarginL,MarginR,MarginV,Effect,Text";
        parse_format_line(def, strlen(def), doc->event_cols, &doc->event_ncols);
    }
}

static int field_by_name(const ms_sub_doc *doc, const char **fs, const size_t *fl, int c,
                         const char *name, const char **out, size_t *outn)
{
    int idx;
    char local[MS_SUB_FMT_COLS_MAX][MS_SUB_COL_NAME_MAX];
    int ln = 0;
    char (*cols)[MS_SUB_COL_NAME_MAX];
    int ncols;
    if (doc != NULL && doc->event_ncols > 0) {
        cols = (char (*)[MS_SUB_COL_NAME_MAX])(void *)doc->event_cols;
        ncols = doc->event_ncols;
    } else {
        const char *def = "Layer,Start,End,Style,Name,MarginL,MarginR,MarginV,Effect,Text";
        memset(local, 0, sizeof(local));
        parse_format_line(def, strlen(def), local, &ln);
        cols = local;
        ncols = ln;
    }
    idx = col_index(cols, ncols, name);
    if (idx < 0 || idx >= c) {
        return 0;
    }
    *out = fs[idx];
    *outn = fl[idx];
    return 1;
}

static ms_status parse_event_fields(const ms_sub_doc *doc, const char *body, size_t n,
                                    int has_start, uint64_t start_90k,
                                    int has_end, uint64_t end_90k,
                                    ms_sub_cue *cue, char *text_buf, size_t text_cap)
{
    const char *fs[MS_SUB_FMT_COLS_MAX];
    size_t fl[MS_SUB_FMT_COLS_MAX];
    int ncols, got, packed;
    const char *text_s = body;
    size_t text_n = n;
    const char *tmp;
    size_t tn;
    const ms_ass_style *st;
    char sname[MS_SUB_STYLE_NAME_MAX];
    size_t out_n = 0;
    uint64_t a = start_90k, b = end_90k;
    int ev_has_start = has_start, ev_has_end = has_end;
    if (cue == NULL || text_buf == NULL || text_cap == 0) {
        return MS_ERR_INVALID_ARG;
    }
    if (n > MS_SUB_EVENT_LINE_MAX) {
        n = MS_SUB_EVENT_LINE_MAX;
    }
    packed = 0;
    {
        /* 压缩事件：ReadOrder,Layer,Style,... 前两列是整数且第二列不是时间。 */
        const char *afs[3];
        size_t afl[3];
        int ac = split_csv_limited(body, n, 3, afs, afl, 0);
        int i0 = 0, i1 = 0;
        uint64_t tt;
        if (ac >= 3 && parse_int_token(afs[0], afl[0], &i0) == 0 &&
            parse_int_token(afs[1], afl[1], &i1) == 0 &&
            parse_ass_time(afs[1], afl[1], &tt) != 0) {
            packed = 1;
        }
    }
    ncols = (doc != NULL && doc->event_ncols > 0) ? doc->event_ncols : 0;
    if (ncols <= 0) {
        ncols = 10;
    }
    got = packed ? 0 : split_csv_limited(body, n, ncols, fs, fl, 1);
    if (packed) {
        const char *pfs[9];
        size_t pfl[9];
        int pc = split_csv_limited(body, n, 9, pfs, pfl, 1);
        if (pc >= 9) {
            int layer = 0;
            (void)parse_int_token(pfs[1], pfl[1], &layer);
            cue->layer = layer;
            cue->style_flags |= MS_SUB_STYLE_HAS_LAYER;
            style_name_copy(sname, sizeof(sname), pfs[2], pfl[2]);
            st = find_style(doc, sname);
            apply_style_rec(st, cue);
            {
                int ml = 0, mr = 0, mv = 0;
                if (parse_int_token(pfs[4], pfl[4], &ml) == 0 && ml != 0) {
                    cue->margin_l = ml;
                }
                if (parse_int_token(pfs[5], pfl[5], &mr) == 0 && mr != 0) {
                    cue->margin_r = mr;
                }
                if (parse_int_token(pfs[6], pfl[6], &mv) == 0 && mv != 0) {
                    cue->margin_v = mv;
                }
                cue->style_flags |= MS_SUB_STYLE_HAS_MARGIN;
            }
            if (pfl[7] > 0) {
                const char *e = skip_ws_p(pfs[7], pfs[7] + pfl[7]);
                if ((size_t)((pfs[7] + pfl[7]) - e) > 0) {
                    mark_cap(cue, MS_SUB_CAP_UNSUPPORTED_EFFECT);
                }
            }
            text_s = pfs[8];
            text_n = pfl[8];
        } else {
            text_s = body;
            text_n = n;
        }
    } else if (got < 3) {
        /* 当作纯覆盖文本 */
        text_s = body;
        text_n = n;
    } else {
        int has_text = 0;
        const char *text_field = NULL;
        size_t text_field_n = 0;
        if (field_by_name(doc, fs, fl, got, "Text", &tmp, &tn)) {
            has_text = 1;
            text_field = tmp;
            text_field_n = tn;
        }
            if (field_by_name(doc, fs, fl, got, "Layer", &tmp, &tn)) {
                int layer = 0;
                if (parse_int_token(tmp, tn, &layer) == 0) {
                    cue->layer = layer;
                    cue->style_flags |= MS_SUB_STYLE_HAS_LAYER;
                }
            }
            if (field_by_name(doc, fs, fl, got, "Start", &tmp, &tn)) {
                if (parse_ass_time(tmp, tn, &a) == 0) {
                    ev_has_start = 1;
                } else {
                    return MS_ERR_MALFORMED;
                }
            }
            if (field_by_name(doc, fs, fl, got, "End", &tmp, &tn)) {
                if (parse_ass_time(tmp, tn, &b) == 0) {
                    ev_has_end = 1;
                } else {
                    return MS_ERR_MALFORMED;
                }
            }
            sname[0] = 0;
            if (field_by_name(doc, fs, fl, got, "Style", &tmp, &tn)) {
                style_name_copy(sname, sizeof(sname), tmp, tn);
            }
            st = find_style(doc, sname);
            apply_style_rec(st, cue);
            if (field_by_name(doc, fs, fl, got, "MarginL", &tmp, &tn)) {
                int ml = 0;
                if (parse_int_token(tmp, tn, &ml) == 0 && ml != 0) {
                    cue->margin_l = ml;
                }
                cue->style_flags |= MS_SUB_STYLE_HAS_MARGIN;
            }
            if (field_by_name(doc, fs, fl, got, "MarginR", &tmp, &tn)) {
                int mr = 0;
                if (parse_int_token(tmp, tn, &mr) == 0 && mr != 0) {
                    cue->margin_r = mr;
                }
                cue->style_flags |= MS_SUB_STYLE_HAS_MARGIN;
            }
            if (field_by_name(doc, fs, fl, got, "MarginV", &tmp, &tn)) {
                int mv = 0;
                if (parse_int_token(tmp, tn, &mv) == 0 && mv != 0) {
                    cue->margin_v = mv;
                }
                cue->style_flags |= MS_SUB_STYLE_HAS_MARGIN;
            }
            if (field_by_name(doc, fs, fl, got, "Effect", &tmp, &tn)) {
                const char *e = skip_ws_p(tmp, tmp + tn);
                if ((size_t)((tmp + tn) - e) > 0) {
                    mark_cap(cue, MS_SUB_CAP_UNSUPPORTED_EFFECT);
                }
            }
            if (has_text) {
                text_s = text_field;
                text_n = text_field_n;
            } else if (got > 0) {
                text_s = fs[got - 1];
                text_n = fl[got - 1];
            }
    }
    if (doc != NULL) {
        cue->play_res_x = doc->play_res_x;
        cue->play_res_y = doc->play_res_y;
    }
    parse_override_tags(doc, text_s, text_n, cue, text_buf, text_cap, &out_n);
    if (out_n == 0) {
        return MS_OK; /* 空文本：调用方跳过 */
    }
    cue->text = text_buf;
    cue->text_size = out_n;
    cue->has_start = ev_has_start;
    cue->has_end = ev_has_end;
    cue->start_90k = a;
    cue->end_90k = ev_has_end ? b : a;
    return MS_OK;
}

static const char *strip_dialogue_prefix(const char *s, size_t n, size_t *out_n, int *is_comment)
{
    const char *p = skip_ws_p(s, s + n);
    size_t left = (size_t)((s + n) - p);
    *is_comment = 0;
    if (starts_with_ci(p, left, "Dialogue:")) {
        p = skip_ws_p(p + 9, s + n);
        *out_n = (size_t)((s + n) - p);
        return p;
    }
    if (starts_with_ci(p, left, "Comment:")) {
        *is_comment = 1;
        *out_n = 0;
        return p;
    }
    *out_n = (size_t)((s + n) - p);
    return p;
}

ms_status ms_sub_doc_set_ass_header(ms_sub_doc *doc, const uint8_t *data, size_t size)
{
    const uint8_t *p = data;
    size_t n = size;
    if (doc == NULL || (size > 0 && data == NULL)) {
        return MS_ERR_INVALID_ARG;
    }
    if (size == 0) {
        return MS_OK;
    }
    if (size > MS_SUB_DOC_BYTES_MAX) {
        return MS_ERR_OVERFLOW;
    }
    skip_bom(&p, &n);
    reset_styles(doc, 1);
    if (doc->fmt != MS_SUB_FORMAT_SSA) {
        doc->fmt = MS_SUB_FORMAT_ASS;
    }
    parse_header_body(doc, (const char *)p, n);
    default_event_format(doc);
    return MS_OK;
}

ms_status ms_sub_ass_parse_event(const ms_sub_doc *doc, const uint8_t *data, size_t size,
                                 int has_start, uint64_t start_90k,
                                 int has_end, uint64_t end_90k,
                                 ms_sub_cue *out, char *text_buf, size_t text_cap)
{
    const char *body;
    size_t bn;
    int is_comment = 0;
    size_t sz;
    ms_sub_cue tmp;
    ms_status st;
    if ((size > 0 && data == NULL) || out == NULL || text_buf == NULL || text_cap == 0) {
        return MS_ERR_INVALID_ARG;
    }
    sz = out->struct_size;
    memset(&tmp, 0, sizeof(tmp));
    tmp.struct_size = (sz == 0 || sz > sizeof(tmp)) ? sizeof(tmp) : sz;
    if (out->track_id != 0) {
        tmp.track_id = out->track_id;
    }
    text_buf[0] = 0;
    if (size == 0) {
        return MS_ERR_MALFORMED;
    }
    if (size > MS_SUB_EVENT_LINE_MAX) {
        size = MS_SUB_EVENT_LINE_MAX;
    }
    body = strip_dialogue_prefix((const char *)data, size, &bn, &is_comment);
    if (is_comment) {
        return MS_OK;
    }
    st = parse_event_fields(doc, body, bn, has_start, start_90k, has_end, end_90k,
                            &tmp, text_buf, text_cap);
    if (st != MS_OK) {
        return st;
    }
    {
        size_t cpy = tmp.struct_size;
        uint32_t saved_tid = out->track_id;
        if (cpy == 0 || cpy > sizeof(*out)) {
            cpy = sizeof(*out);
        }
        memcpy(out, &tmp, cpy);
        out->struct_size = cpy;
        if (saved_tid != 0 && out->track_id == 0) {
            out->track_id = saved_tid;
        }
        out->text = text_buf;
    }
    return MS_OK;
}

static ms_status parse_ass_file(ms_sub_doc *doc, const uint8_t *data, size_t size,
                                ms_sub_format fmt, uint32_t track_id, size_t *accepted)
{
    line_iter it;
    const char *line;
    size_t len;
    int blank;
    int section = 0;
    int in_events = 0;
    ms_status st;
    const uint8_t *p = data;
    size_t n = size;
    *accepted = 0;
    skip_bom(&p, &n);
    doc->fmt = (fmt == MS_SUB_FORMAT_SSA) ? MS_SUB_FORMAT_SSA : MS_SUB_FORMAT_ASS;
    doc->is_ssa = (doc->fmt == MS_SUB_FORMAT_SSA);
    st = ms_sub_doc_set_ass_header(doc, p, n);
    if (st != MS_OK) {
        return st;
    }
    default_event_format(doc);
    it.p = (const char *)p;
    it.n = n;
    it.off = 0;
    while (next_line(&it, &line, &len, &blank)) {
        const char *s = skip_ws_p(line, line + len);
        size_t left = (size_t)((line + len) - s);
        if (left == 0) {
            continue;
        }
        if (s[0] == '[') {
            in_events = starts_section(s, left, "[Events]");
            section = in_events ? 3 : (is_skip_section(s, left) ? 4 : 1);
            continue;
        }
        if (!in_events || section == 4) {
            continue;
        }
        if (starts_with_ci(s, left, "Format:") || starts_with_ci(s, left, "Comment:")) {
            continue;
        }
        if (starts_with_ci(s, left, "Dialogue:")) {
            char text[MS_SUB_CUE_TEXT_MAX + 1];
            ms_sub_cue cue;
            ms_sub_cue_default(&cue);
            cue.track_id = track_id;
            st = ms_sub_ass_parse_event(doc, (const uint8_t *)s, left,
                                        0, 0, 0, 0, &cue, text, sizeof(text));
            if (st == MS_ERR_OVERFLOW || st == MS_ERR_NOMEM) {
                return st;
            }
            if (st != MS_OK || cue.text_size == 0) {
                continue;
            }
            st = ms_sub_doc_add_cue(doc, &cue);
            if (st == MS_ERR_OVERFLOW || st == MS_ERR_NOMEM) {
                return st;
            }
            if (st == MS_OK) {
                (*accepted)++;
            }
        }
    }
    (void)section;
    return MS_OK;
}
