/*
 * QuickJS：脚本 → main(item) → URL(A) 或 body/m3u8(B)。
 * by: Long <mycjzb@gmail.com>
 */
#include "ms_qjs.h"
#include "ms_qjs_bind.h"
#include "ms_qjs_wrap.h"

#include "quickjs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(__ANDROID__)
#include <android/log.h>
#include <pthread.h>
#include <sys/time.h>
#define QLOG(...) __android_log_print(ANDROID_LOG_ERROR, "Qjs", __VA_ARGS__)
#else
#define QLOG(...) ((void)0)
#endif

#define BODY_CAP (8u << 20)
#define QJS_THR_STACK (1u << 20)

static int64_t mono_ms(void)
{
    struct timespec ts;
    memset(&ts, 0, sizeof(ts));
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
        return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
    }
#if defined(__ANDROID__)
    {
        struct timeval tv;
        memset(&tv, 0, sizeof(tv));
        gettimeofday(&tv, NULL);
        return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
    }
#else
    return (int64_t)time(NULL) * 1000;
#endif
}

static int qjs_interrupt(JSRuntime *rt, void *opaque)
{
    int64_t *deadline = (int64_t *)opaque;
    (void)rt;
    return mono_ms() >= *deadline;
}

static void qjs_log_exc(JSContext *ctx, const char *tag)
{
    JSValue exc, st;
    const char *msg, *stack;
    exc = JS_GetException(ctx);
    msg = JS_ToCString(ctx, exc);
    st = JS_GetPropertyStr(ctx, exc, "stack");
    stack = JS_IsString(st) ? JS_ToCString(ctx, st) : NULL;
    if (stack && stack[0]) {
        QLOG("%s: %s\n%s", tag, msg ? msg : "?", stack);
    } else {
        QLOG("%s: %s", tag, msg ? msg : "?");
    }
    if (msg) {
        JS_FreeCString(ctx, msg);
    }
    if (stack) {
        JS_FreeCString(ctx, stack);
    }
    JS_FreeValue(ctx, st);
    JS_FreeValue(ctx, exc);
}

void ms_qjs_result_clear(ms_qjs_result *r)
{
    if (!r) {
        return;
    }
    free(r->url);
    free(r->headers);
    free(r->content_type);
    free(r->body);
    free(r->stream_args);
    memset(r, 0, sizeof(*r));
}

static char *dup_cstr(const char *s)
{
    size_t n;
    char *p;
    if (!s) {
        return NULL;
    }
    n = strlen(s);
    p = (char *)malloc(n + 1);
    if (!p) {
        return NULL;
    }
    memcpy(p, s, n + 1);
    return p;
}

static void append_hdr(char **out, size_t *cap, size_t *len, const char *k, const char *v)
{
    size_t need;
    char *nb;
    if (!k || !v || !v[0]) {
        return;
    }
    need = *len + strlen(k) + strlen(v) + 4;
    if (need > *cap) {
        size_t nc = *cap ? *cap * 2 : 512;
        while (nc < need) {
            nc *= 2;
        }
        nb = (char *)realloc(*out, nc);
        if (!nb) {
            return;
        }
        *out = nb;
        *cap = nc;
    }
    *len += (size_t)snprintf(*out + *len, *cap - *len, "%s: %s\n", k, v);
}

static char *js_str_dup(JSContext *ctx, JSValue v)
{
    const char *s;
    char *p;
    if (!JS_IsString(v)) {
        return NULL;
    }
    s = JS_ToCString(ctx, v);
    if (!s) {
        return NULL;
    }
    p = dup_cstr(s);
    JS_FreeCString(ctx, s);
    return p;
}

static int take_bytes(JSContext *ctx, JSValue v, unsigned char **out, size_t *out_n)
{
    size_t n = 0, off = 0, blen = 0, bpe = 1, ab = 0;
    const char *s;
    uint8_t *p;
    JSValue ta;
    *out = NULL;
    *out_n = 0;
    if (JS_IsNull(v) || JS_IsUndefined(v)) {
        return -1;
    }
    ta = JS_GetTypedArrayBuffer(ctx, v, &off, &blen, &bpe);
    if (!JS_IsException(ta)) {
        p = JS_GetArrayBuffer(ctx, &ab, ta);
        JS_FreeValue(ctx, ta);
        if (p && off + blen <= ab && blen > 0) {
            if (blen > BODY_CAP) {
                return -1;
            }
            *out = (unsigned char *)malloc(blen);
            if (!*out) {
                return -1;
            }
            memcpy(*out, p + off, blen);
            *out_n = blen;
            return 0;
        }
    } else {
        JS_FreeValue(ctx, JS_GetException(ctx));
    }
    p = JS_GetArrayBuffer(ctx, &ab, v);
    if (p && ab > 0) {
        if (ab > BODY_CAP) {
            return -1;
        }
        *out = (unsigned char *)malloc(ab);
        if (!*out) {
            return -1;
        }
        memcpy(*out, p, ab);
        *out_n = ab;
        return 0;
    }
    s = JS_ToCStringLen(ctx, &n, v);
    if (!s) {
        return -1;
    }
    if (n > BODY_CAP) {
        JS_FreeCString(ctx, s);
        return -1;
    }
    *out = (unsigned char *)malloc(n ? n : 1);
    if (!*out) {
        JS_FreeCString(ctx, s);
        return -1;
    }
    if (n) {
        memcpy(*out, s, n);
    }
    *out_n = n;
    JS_FreeCString(ctx, s);
    return 0;
}

static int take_result(JSContext *ctx, JSValue ret, ms_qjs_result *out)
{
    const char *s;
    size_t hcap = 0, hlen = 0;
    ms_qjs_result_clear(out);

    if (JS_IsString(ret)) {
        s = JS_ToCString(ctx, ret);
        if (!s || !s[0]) {
            if (s) {
                JS_FreeCString(ctx, s);
            }
            return -1;
        }
        out->kind = MS_QJS_KIND_URL;
        out->url = dup_cstr(s);
        JS_FreeCString(ctx, s);
        return out->url ? 0 : -1;
    }
    if (!JS_IsObject(ret)) {
        return -1;
    }

    /* B：m3u8 / body 优先 */
    {
        JSValue m3 = JS_GetPropertyStr(ctx, ret, "m3u8");
        JSValue body = JS_GetPropertyStr(ctx, ret, "body");
        JSValue content = JS_GetPropertyStr(ctx, ret, "content");
        JSValue ct = JS_GetPropertyStr(ctx, ret, "contentType");
        JSValue ct2 = JS_GetPropertyStr(ctx, ret, "content_type");
        JSValue mime = JS_GetPropertyStr(ctx, ret, "mime");
        int got = 0;

        if (!JS_IsUndefined(m3) && !JS_IsNull(m3)) {
            if (take_bytes(ctx, m3, &out->body, &out->body_len) == 0 && out->body_len > 0) {
                out->kind = MS_QJS_KIND_BODY;
                out->content_type = dup_cstr("application/vnd.apple.mpegurl");
                got = 1;
            }
        } else if (!JS_IsUndefined(body) && !JS_IsNull(body)) {
            if (take_bytes(ctx, body, &out->body, &out->body_len) == 0 && out->body_len > 0) {
                out->kind = MS_QJS_KIND_BODY;
                got = 1;
            }
        } else if (!JS_IsUndefined(content) && !JS_IsNull(content)) {
            if (take_bytes(ctx, content, &out->body, &out->body_len) == 0 && out->body_len > 0) {
                out->kind = MS_QJS_KIND_BODY;
                got = 1;
            }
        }
        if (got && !out->content_type) {
            if (JS_IsString(ct)) {
                out->content_type = js_str_dup(ctx, ct);
            } else if (JS_IsString(ct2)) {
                out->content_type = js_str_dup(ctx, ct2);
            } else if (JS_IsString(mime)) {
                out->content_type = js_str_dup(ctx, mime);
            }
        }
        JS_FreeValue(ctx, m3);
        JS_FreeValue(ctx, body);
        JS_FreeValue(ctx, content);
        JS_FreeValue(ctx, ct);
        JS_FreeValue(ctx, ct2);
        JS_FreeValue(ctx, mime);
        if (got) {
            /* B 也可带 url（相对片基址）与 headers */
            JSValue u = JS_GetPropertyStr(ctx, ret, "url");
            if (JS_IsString(u)) {
                out->url = js_str_dup(ctx, u);
            }
            JS_FreeValue(ctx, u);
            goto headers;
        }
    }

    /* A：url / playUrl / urls[0] */
    {
        JSValue u = JS_GetPropertyStr(ctx, ret, "url");
        if (JS_IsUndefined(u) || JS_IsNull(u)) {
            JS_FreeValue(ctx, u);
            u = JS_GetPropertyStr(ctx, ret, "playUrl");
        }
        if (JS_IsUndefined(u) || JS_IsNull(u)) {
            JS_FreeValue(ctx, u);
            u = JS_GetPropertyStr(ctx, ret, "playurl");
        }
        if (JS_IsUndefined(u) || JS_IsNull(u)) {
            JSValue urls = JS_GetPropertyStr(ctx, ret, "urls");
            if (JS_IsArray(ctx, urls)) {
                JSValue u0 = JS_GetPropertyUint32(ctx, urls, 0);
                if (JS_IsObject(u0)) {
                    JS_FreeValue(ctx, u);
                    u = JS_GetPropertyStr(ctx, u0, "url");
                }
                JS_FreeValue(ctx, u0);
            }
            JS_FreeValue(ctx, urls);
        }
        if (JS_IsString(u)) {
            out->url = js_str_dup(ctx, u);
            out->kind = MS_QJS_KIND_URL;
        }
        JS_FreeValue(ctx, u);
        if (!out->url || !out->url[0]) {
            ms_qjs_result_clear(out);
            return -1;
        }
    }

headers:
    {
        JSValue r = JS_GetPropertyStr(ctx, ret, "referer");
        JSValue ua = JS_GetPropertyStr(ctx, ret, "userAgent");
        JSValue ua2 = JS_GetPropertyStr(ctx, ret, "ua");
        JSValue hdrs = JS_GetPropertyStr(ctx, ret, "headers");
        if (JS_IsString(r)) {
            s = JS_ToCString(ctx, r);
            if (s) {
                append_hdr(&out->headers, &hcap, &hlen, "Referer", s);
                JS_FreeCString(ctx, s);
            }
        }
        if (JS_IsString(ua)) {
            s = JS_ToCString(ctx, ua);
            if (s) {
                append_hdr(&out->headers, &hcap, &hlen, "User-Agent", s);
                JS_FreeCString(ctx, s);
            }
        } else if (JS_IsString(ua2)) {
            s = JS_ToCString(ctx, ua2);
            if (s) {
                append_hdr(&out->headers, &hcap, &hlen, "User-Agent", s);
                JS_FreeCString(ctx, s);
            }
        }
        if (JS_IsObject(hdrs)) {
            JSPropertyEnum *ptab = NULL;
            uint32_t plen = 0, i;
            if (!JS_GetOwnPropertyNames(ctx, &ptab, &plen, hdrs,
                                        JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY)) {
                for (i = 0; i < plen; i++) {
                    const char *k = JS_AtomToCString(ctx, ptab[i].atom);
                    JSValue v = JS_GetProperty(ctx, hdrs, ptab[i].atom);
                    const char *vs = JS_ToCString(ctx, v);
                    if (k && vs) {
                        append_hdr(&out->headers, &hcap, &hlen, k, vs);
                    }
                    if (k) {
                        JS_FreeCString(ctx, k);
                    }
                    if (vs) {
                        JS_FreeCString(ctx, vs);
                    }
                    JS_FreeValue(ctx, v);
                }
                js_free(ctx, ptab);
            }
        }
        JS_FreeValue(ctx, r);
        JS_FreeValue(ctx, ua);
        JS_FreeValue(ctx, ua2);
        JS_FreeValue(ctx, hdrs);
    }

    /* stream / streamArgs：分片二次处理走 stream_call */
    {
        JSValue st = JS_GetPropertyStr(ctx, ret, "stream");
        JSValue st2 = JS_GetPropertyStr(ctx, ret, "streamHook");
        JSValue args = JS_GetPropertyStr(ctx, ret, "streamArgs");
        JSValue args2 = JS_GetPropertyStr(ctx, ret, "stream_args");
        JSValue args3 = JS_GetPropertyStr(ctx, ret, "args");
        int32_t iv = 0;

        if (JS_IsBool(st)) {
            out->stream = JS_ToBool(ctx, st) ? 1 : 0;
        } else if (JS_IsNumber(st) && JS_ToInt32(ctx, &iv, st) == 0) {
            out->stream = iv != 0;
        } else if (JS_IsString(st)) {
            s = JS_ToCString(ctx, st);
            out->stream = (s && s[0] && !(s[0] == '0' && s[1] == 0)) ? 1 : 0;
            if (s) {
                JS_FreeCString(ctx, s);
            }
        } else if (JS_IsBool(st2)) {
            out->stream = JS_ToBool(ctx, st2) ? 1 : 0;
        } else if (JS_IsString(st2)) {
            s = JS_ToCString(ctx, st2);
            out->stream = (s && s[0]) ? 1 : 0;
            if (s) {
                JS_FreeCString(ctx, s);
            }
        }
        {
            JSValue a = JS_UNDEFINED;
            if (!JS_IsUndefined(args) && !JS_IsNull(args)) {
                a = args;
            } else if (!JS_IsUndefined(args2) && !JS_IsNull(args2)) {
                a = args2;
            } else if (!JS_IsUndefined(args3) && !JS_IsNull(args3)) {
                a = args3;
            }
            if (JS_IsString(a)) {
                out->stream_args = js_str_dup(ctx, a);
            } else if (JS_IsObject(a)) {
                JSValue j = JS_JSONStringify(ctx, a, JS_UNDEFINED, JS_UNDEFINED);
                if (JS_IsString(j)) {
                    out->stream_args = js_str_dup(ctx, j);
                }
                JS_FreeValue(ctx, j);
            }
            if (out->stream_args && out->stream_args[0]) {
                out->stream = 1;
            }
        }
        JS_FreeValue(ctx, st);
        JS_FreeValue(ctx, st2);
        JS_FreeValue(ctx, args);
        JS_FreeValue(ctx, args2);
        JS_FreeValue(ctx, args3);
    }

    if (out->kind == MS_QJS_KIND_BODY) {
        return out->body && out->body_len ? 0 : -1;
    }
    return out->url && out->url[0] ? 0 : -1;
}

#if defined(__ANDROID__)
typedef struct {
    const char *script;
    size_t script_len;
    const char *item_url;
    ms_qjs_result *out;
    int rc;
} qjs_eval_job;

static int eval_ex_run(const char *script, size_t script_len, const char *item_url,
                       ms_qjs_result *out);

static void *eval_ex_thr(void *arg)
{
    qjs_eval_job *j = (qjs_eval_job *)arg;
    j->rc = eval_ex_run(j->script, j->script_len, j->item_url, j->out);
    return NULL;
}

int ms_qjs_eval_ex(const char *script, size_t script_len, const char *item_url, ms_qjs_result *out)
{
    qjs_eval_job job;
    pthread_attr_t attr;
    pthread_t th;
    int pr;
    char *script_copy = NULL;
    char *url_copy = NULL;

    if (out) {
        memset(out, 0, sizeof(*out));
    }
    if (!script || script_len == 0 || script_len > (512u * 1024u) || !item_url || !out) {
        return -1;
    }
    /* JNI 数组/字符串指针不跨线程：拷到堆再交给 worker */
    script_copy = (char *)malloc(script_len);
    url_copy = strdup(item_url);
    if (!script_copy || !url_copy) {
        free(script_copy);
        free(url_copy);
        return eval_ex_run(script, script_len, item_url, out);
    }
    memcpy(script_copy, script, script_len);
    job.script = script_copy;
    job.script_len = script_len;
    job.item_url = url_copy;
    job.out = out;
    job.rc = -1;
    /* Exo 播放线程 C 栈紧，大脚本编译易 SyntaxError: stack overflow → 独立 1MiB 栈 */
    if (pthread_attr_init(&attr) == 0) {
        pthread_attr_setstacksize(&attr, QJS_THR_STACK);
        pr = pthread_create(&th, &attr, eval_ex_thr, &job);
        pthread_attr_destroy(&attr);
        if (pr == 0) {
            pthread_join(th, NULL);
            free(script_copy);
            free(url_copy);
            return job.rc;
        }
    }
    pr = eval_ex_run(script_copy, script_len, url_copy, out);
    free(script_copy);
    free(url_copy);
    return pr;
}

static int eval_ex_run(const char *script, size_t script_len, const char *item_url,
                       ms_qjs_result *out)
#else
int ms_qjs_eval_ex(const char *script, size_t script_len, const char *item_url, ms_qjs_result *out)
#endif
{
    JSRuntime *rt;
    JSContext *ctx;
    JSValue global, ev, main, item, ret;
    int64_t deadline;
    int rc = -1;
    char page[1024];
    char name[256];
    unsigned char *plain = NULL;
    size_t plain_n = 0;

    if (out) {
        memset(out, 0, sizeof(*out));
    }
    if (!script || script_len == 0 || script_len > (512u * 1024u) || !item_url || !out) {
        return -1;
    }
    if (ms_qjs_unwrap((const unsigned char *)script, script_len, &plain, &plain_n) != 0) {
        return -1;
    }
    script = (const char *)plain;
    script_len = plain_n;

    page[0] = 0;
    name[0] = 0;
    {
        const char *q = strchr(item_url, '?');
        if (q) {
            const char *p = strstr(q, "page=");
            const char *id = strstr(q, "id=");
            if (p) {
                p += 5;
                {
                    const char *amp = strchr(p, '&');
                    size_t n = amp ? (size_t)(amp - p) : strlen(p);
                    if (n >= sizeof(page)) {
                        n = sizeof(page) - 1;
                    }
                    memcpy(page, p, n);
                    page[n] = 0;
                }
            }
            if (id) {
                id += 3;
                {
                    const char *amp = strchr(id, '&');
                    size_t n = amp ? (size_t)(amp - id) : strlen(id);
                    if (n >= sizeof(name)) {
                        n = sizeof(name) - 1;
                    }
                    memcpy(name, id, n);
                    name[n] = 0;
                }
            }
        }
    }

    /* YSP 等多段 HTTP：单请求超时 15s×3 易超 20s → interrupted 误报 */
    deadline = mono_ms() + 90000;
    rt = JS_NewRuntime();
    if (!rt) {
        free(plain);
        return -1;
    }
    JS_SetMemoryLimit(rt, 16u << 20);
    /* Exo 播放线程剩余 C 栈紧：256KiB 限额易在大脚本编译期 SyntaxError: stack overflow */
    JS_SetMaxStackSize(rt, 1u << 20);
    ctx = JS_NewContext(rt);
    if (!ctx) {
        free(plain);
        JS_FreeRuntime(rt);
        return -1;
    }

    global = JS_GetGlobalObject(ctx);
    ms_qjs_bind(ctx, global);

    /* 编译期不挂 interrupt，避免与栈探测误伤 */
    ev = JS_Eval(ctx, script, script_len, item_url, JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(ev)) {
        qjs_log_exc(ctx, "eval exception");
        if (script_len >= 8) {
            const unsigned char *t = (const unsigned char *)script + script_len - 8;
            QLOG("eval script_len=%zu tail=%02x%02x%02x%02x%02x%02x%02x%02x", script_len, t[0],
                 t[1], t[2], t[3], t[4], t[5], t[6], t[7]);
        } else {
            QLOG("eval script_len=%zu", script_len);
        }
        JS_FreeValue(ctx, ev);
        goto done;
    }
    JS_FreeValue(ctx, ev);

    main = JS_GetPropertyStr(ctx, global, "main");
    if (!JS_IsFunction(ctx, main)) {
        QLOG("main() missing");
        JS_FreeValue(ctx, main);
        goto done;
    }
    JS_SetInterruptHandler(rt, qjs_interrupt, &deadline);
    item = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, item, "url", JS_NewString(ctx, item_url));
    JS_SetPropertyStr(ctx, item, "name", JS_NewString(ctx, name));
    JS_SetPropertyStr(ctx, item, "id", JS_NewString(ctx, name));
    JS_SetPropertyStr(ctx, item, "pageUrl", JS_NewString(ctx, page[0] ? page : item_url));
    ret = JS_Call(ctx, main, global, 1, &item);
    JS_FreeValue(ctx, item);
    JS_FreeValue(ctx, main);
    if (JS_IsException(ret)) {
        qjs_log_exc(ctx, "main() exception");
        JS_FreeValue(ctx, ret);
        goto done;
    }
    rc = take_result(ctx, ret, out);
    JS_FreeValue(ctx, ret);

done:
    free(plain);
    JS_FreeValue(ctx, global);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    if (rc != 0) {
        ms_qjs_result_clear(out);
    }
    return rc;
}

int ms_qjs_eval(const char *script, size_t script_len, const char *item_url, char *out_url,
                size_t out_url_cap, char *out_hdr, size_t out_hdr_cap)
{
    ms_qjs_result r;
    size_t n;
    if (out_url && out_url_cap) {
        out_url[0] = 0;
    }
    if (out_hdr && out_hdr_cap) {
        out_hdr[0] = 0;
    }
    if (ms_qjs_eval_ex(script, script_len, item_url, &r) != 0) {
        return -1;
    }
    if (r.kind != MS_QJS_KIND_URL || !r.url) {
        ms_qjs_result_clear(&r);
        return -1;
    }
    n = strlen(r.url);
    if (n >= out_url_cap) {
        n = out_url_cap - 1;
    }
    memcpy(out_url, r.url, n);
    out_url[n] = 0;
    if (out_hdr && out_hdr_cap && r.headers) {
        n = strlen(r.headers);
        if (n >= out_hdr_cap) {
            n = out_hdr_cap - 1;
        }
        memcpy(out_hdr, r.headers, n);
        out_hdr[n] = 0;
    }
    ms_qjs_result_clear(&r);
    return 0;
}

#define STREAM_CAP (32u << 20)

int ms_qjs_stream_call(const char *script, size_t script_len, const char *url,
                       const char *args_json, unsigned char **out, size_t *out_len)
{
    JSRuntime *rt;
    JSContext *ctx;
    JSValue global, ev, fn, args[2], ret;
    int64_t deadline;
    int rc = -1;
    unsigned char *plain = NULL;
    size_t plain_n = 0;

    if (out) {
        *out = NULL;
    }
    if (out_len) {
        *out_len = 0;
    }
    if (!script || script_len == 0 || script_len > (512u * 1024u) || !url || !url[0] || !out ||
        !out_len) {
        return -1;
    }
    if (ms_qjs_unwrap((const unsigned char *)script, script_len, &plain, &plain_n) != 0) {
        return -1;
    }
    script = (const char *)plain;
    script_len = plain_n;

    deadline = mono_ms() + 90000;
    rt = JS_NewRuntime();
    if (!rt) {
        free(plain);
        return -1;
    }
    JS_SetMemoryLimit(rt, 32u << 20);
    JS_SetMaxStackSize(rt, 1u << 20);
    ctx = JS_NewContext(rt);
    if (!ctx) {
        free(plain);
        JS_FreeRuntime(rt);
        return -1;
    }

    global = JS_GetGlobalObject(ctx);
    ms_qjs_bind(ctx, global);
    ev = JS_Eval(ctx, script, script_len, "<stream>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(ev)) {
        qjs_log_exc(ctx, "stream eval exception");
        JS_FreeValue(ctx, ev);
        goto done;
    }
    JS_FreeValue(ctx, ev);

    fn = JS_GetPropertyStr(ctx, global, "stream_call");
    if (!JS_IsFunction(ctx, fn)) {
        QLOG("stream_call() missing");
        JS_FreeValue(ctx, fn);
        goto done;
    }
    JS_SetInterruptHandler(rt, qjs_interrupt, &deadline);
    args[0] = JS_NewString(ctx, url);
    if (args_json && args_json[0]) {
        args[1] = JS_ParseJSON(ctx, args_json, strlen(args_json), "<args>");
        if (JS_IsException(args[1])) {
            JS_FreeValue(ctx, args[1]);
            args[1] = JS_NewObject(ctx);
        }
    } else {
        args[1] = JS_NewObject(ctx);
    }
    ret = JS_Call(ctx, fn, global, 2, args);
    JS_FreeValue(ctx, args[0]);
    JS_FreeValue(ctx, args[1]);
    JS_FreeValue(ctx, fn);
    if (JS_IsException(ret)) {
        qjs_log_exc(ctx, "stream_call exception");
        JS_FreeValue(ctx, ret);
        goto done;
    }
    if (take_bytes(ctx, ret, out, out_len) != 0 || !*out || *out_len == 0 ||
        *out_len > STREAM_CAP) {
        free(*out);
        *out = NULL;
        *out_len = 0;
        JS_FreeValue(ctx, ret);
        goto done;
    }
    JS_FreeValue(ctx, ret);
    rc = 0;

done:
    free(plain);
    JS_FreeValue(ctx, global);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    return rc;
}
