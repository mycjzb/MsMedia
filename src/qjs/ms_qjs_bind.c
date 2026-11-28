/*
 * QuickJS 宿主 API：全局 ms；并挂 ku9/cjs 别名（参数形态兼容超级直播）。
 * HTTP → ms_nc_http；AES/RSA → OpenSSL；XXTEA → 自研。
 * by: Long <mycjzb@gmail.com>
 */
#include "ms_qjs_bind.h"
#include "ms_qjs_crypto.h"

#include "../netcore/ms_netcore.h"

#ifndef MS_QJS_HOST_STUB
#include <openssl/evp.h>
#include <openssl/rand.h>
#endif

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(__ANDROID__)
#include <android/log.h>
#define QLOG(...) __android_log_print(ANDROID_LOG_INFO, "Qjs", __VA_ARGS__)
#else
#define QLOG(...) ((void)0)
#endif

enum {
    M_LOG = 1,
    M_GET,
    M_GETBIN, /* 二进制正文 → ArrayBuffer（TS/密文勿走 UTF-8 get） */
    M_POST,
    M_REQ,
    M_QUERY,
    M_MD5,
    M_SHA1,
    M_SHA256,
    M_B64E,
    M_B64D,
    M_HEXE,
    M_HEXD,
    M_TIME,
    M_NOWMS,
    M_AES_CBC_DEC,
    M_AES_CBC_ENC,
    M_AES_ECB_DEC,
    M_XXTEA_DEC,
    M_XXTEA_ENC,
    M_RSA_ENC,
    M_RSA_SIGN,
    M_RSA_VER,
    M_HMAC_SHA256,
    M_CIPHER,
    M_URLENC,
    M_URLDEC,
    M_RAND,
    M_CACHE_GET,
    M_CACHE_SET,
};

#define CACHE_MAX 64
#define CACHE_VAL_MAX (256u * 1024u)

typedef struct {
    char *key;
    unsigned char *val;
    size_t n;
    int64_t exp_ms; /* 0=不过期 */
} cache_ent;

static cache_ent g_cache[CACHE_MAX];
static pthread_mutex_t g_cache_mu = PTHREAD_MUTEX_INITIALIZER;

static int64_t wall_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void qjs_clear_exc(JSContext *ctx)
{
    JSValue e = JS_GetException(ctx);
    JS_FreeValue(ctx, e);
}

/* 取二进制：ArrayBuffer / TypedArray 优先；否则 UTF-8 字符串（仅文本） */
static char *js_to_bytes(JSContext *ctx, JSValueConst v, size_t *out_n)
{
    size_t n = 0, off = 0, blen = 0, bpe = 1;
    uint8_t *ab;
    char *p;
    JSValue ta;
    const char *s;
    *out_n = 0;
    if (JS_IsNull(v) || JS_IsUndefined(v)) {
        return NULL;
    }
    ta = JS_GetTypedArrayBuffer(ctx, v, &off, &blen, &bpe);
    if (!JS_IsException(ta)) {
        ab = JS_GetArrayBuffer(ctx, &n, ta);
        JS_FreeValue(ctx, ta);
        if (ab && off + blen <= n) {
            p = (char *)malloc(blen + 1);
            if (!p) {
                return NULL;
            }
            memcpy(p, ab + off, blen);
            p[blen] = 0;
            *out_n = blen;
            return p;
        }
    } else {
        qjs_clear_exc(ctx);
    }
    ab = JS_GetArrayBuffer(ctx, &n, v);
    if (ab) {
        p = (char *)malloc(n + 1);
        if (!p) {
            return NULL;
        }
        memcpy(p, ab, n);
        p[n] = 0;
        *out_n = n;
        return p;
    }
    qjs_clear_exc(ctx);
    s = JS_ToCStringLen(ctx, &n, v);
    if (!s) {
        return NULL;
    }
    p = (char *)malloc(n + 1);
    if (!p) {
        JS_FreeCString(ctx, s);
        return NULL;
    }
    memcpy(p, s, n);
    p[n] = 0;
    JS_FreeCString(ctx, s);
    *out_n = n;
    return p;
}

/* HTTP JSON 等文本 */
static JSValue bytes_to_js(JSContext *ctx, const uint8_t *b, size_t n)
{
    if (!b) {
        return JS_NewString(ctx, "");
    }
    return JS_NewStringLen(ctx, (const char *)b, n);
}

/* 密钥/密文：禁止当 UTF-8 字符串（高位字节会膨胀，AES 密钥长度变 27≠16） */
static JSValue bin_to_js(JSContext *ctx, const uint8_t *b, size_t n)
{
    if (!b || n == 0) {
        return JS_NewArrayBufferCopy(ctx, (const uint8_t *)"", 0);
    }
    return JS_NewArrayBufferCopy(ctx, b, n);
}

static int hdr_obj_to_buf(JSContext *ctx, JSValueConst h, char *out, size_t cap)
{
    JSPropertyEnum *ptab = NULL;
    uint32_t plen = 0, i;
    size_t o = 0;
    out[0] = 0;
    if (!JS_IsObject(h)) {
        return 0;
    }
    if (JS_GetOwnPropertyNames(ctx, &ptab, &plen, h, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY)) {
        return -1;
    }
    for (i = 0; i < plen; i++) {
        const char *k, *v;
        JSValue val;
        k = JS_AtomToCString(ctx, ptab[i].atom);
        val = JS_GetProperty(ctx, h, ptab[i].atom);
        v = JS_ToCString(ctx, val);
        if (k && v && o + strlen(k) + strlen(v) + 4 < cap) {
            o += (size_t)snprintf(out + o, cap - o, "%s: %s\n", k, v);
        }
        if (k) {
            JS_FreeCString(ctx, k);
        }
        if (v) {
            JS_FreeCString(ctx, v);
        }
        JS_FreeValue(ctx, val);
    }
    js_free(ctx, ptab);
    return 0;
}

static int http_body(const char *method, const char *url, const char *headers,
                     const uint8_t *body, size_t body_len, uint8_t **out, size_t *out_len,
                     int *status_out)
{
    ms_nc_req rq;
    ms_nc_http *h = NULL;
    uint8_t *buf = NULL;
    size_t cap = 0, n = 0;
    int r;
    memset(&rq, 0, sizeof(rq));
    rq.method = method && method[0] ? method : "GET";
    rq.url = url;
    rq.headers = headers;
    rq.body = body;
    rq.body_len = body_len;
    rq.connect_timeout_ms = 15000;
    rq.read_timeout_ms = 15000;
    rq.max_redirects = 5;
    rq.tls_insecure = 1; /* 解析脚本侧常对 CDN；与播放 tls 习惯一致 */
    if (ms_nc_http_open(&rq, &h) != 0 || !h) {
        return -1;
    }
    if (status_out) {
        *status_out = ms_nc_http_status(h);
    }
    for (;;) {
        if (n + 8192 + 1 > cap) {
            size_t nc = cap ? cap * 2 : 16384;
            uint8_t *nb;
            if (nc > (2u << 20)) {
                free(buf);
                ms_nc_http_close(h);
                return -1;
            }
            nb = (uint8_t *)realloc(buf, nc);
            if (!nb) {
                free(buf);
                ms_nc_http_close(h);
                return -1;
            }
            buf = nb;
            cap = nc;
        }
        r = ms_nc_http_read(h, buf + n, (int)(cap - n - 1));
        if (r < 0) {
            free(buf);
            ms_nc_http_close(h);
            return -1;
        }
        if (r == 0) {
            break;
        }
        n += (size_t)r;
    }
    ms_nc_http_close(h);
    if (buf) {
        buf[n] = 0;
    }
    *out = buf;
    *out_len = n;
    return 0;
}

static JSValue api_magic(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                         int magic)
{
    (void)this_val;
    switch (magic) {
    case M_LOG: {
        int i;
        char line[512];
        size_t o = 0;
        line[0] = 0;
        for (i = 0; i < argc; i++) {
            const char *s = JS_ToCString(ctx, argv[i]);
            if (!s) {
                continue;
            }
            if (o + strlen(s) + 2 < sizeof(line)) {
                o += (size_t)snprintf(line + o, sizeof(line) - o, "%s%s", i ? " " : "", s);
            }
            JS_FreeCString(ctx, s);
        }
        QLOG("%s", line);
        return JS_UNDEFINED;
    }
    case M_GET:
    case M_GETBIN:
    case M_POST: {
        const char *url;
        char hdr[2048];
        uint8_t *body = NULL;
        size_t blen = 0, rn = 0;
        uint8_t *resp = NULL;
        JSValue ret;
        const char *method = magic == M_POST ? "POST" : "GET";
        if (argc < 1 || !(url = JS_ToCString(ctx, argv[0]))) {
            return magic == M_GETBIN ? bin_to_js(ctx, NULL, 0) : JS_NewString(ctx, "");
        }
        hdr[0] = 0;
        if (argc >= 2 && (magic == M_GET || magic == M_GETBIN)) {
            hdr_obj_to_buf(ctx, argv[1], hdr, sizeof(hdr));
        }
        if (magic == M_POST) {
            if (argc >= 2) {
                body = (uint8_t *)js_to_bytes(ctx, argv[1], &blen);
            }
            if (argc >= 3) {
                hdr_obj_to_buf(ctx, argv[2], hdr, sizeof(hdr));
            }
        }
        if (http_body(method, url, hdr[0] ? hdr : NULL, body, blen, &resp, &rn, NULL) != 0) {
            free(body);
            JS_FreeCString(ctx, url);
            return magic == M_GETBIN ? bin_to_js(ctx, NULL, 0) : JS_NewString(ctx, "");
        }
        free(body);
        JS_FreeCString(ctx, url);
        ret = magic == M_GETBIN ? bin_to_js(ctx, resp, rn) : bytes_to_js(ctx, resp, rn);
        free(resp);
        return ret;
    }
    case M_REQ: {
        /* 两种参数序：
         *  A) url, method?, headers?, body?     （ku9.request / 旧 ms.request）
         *  B) method, url, headers?, body?      （超级直播 ms.http_request）
         */
        const char *url = NULL, *method = "GET";
        int method_owned = 0, url_owned = 0;
        char hdr[2048];
        uint8_t *body = NULL;
        size_t blen = 0, rn = 0;
        uint8_t *resp = NULL;
        int status = 0;
        JSValue obj, jbody;
        hdr[0] = 0;
        if (argc < 1) {
            return JS_NULL;
        }
        if (argc >= 2 && JS_IsString(argv[0]) && JS_IsString(argv[1])) {
            const char *a0 = JS_ToCString(ctx, argv[0]);
            const char *a1 = JS_ToCString(ctx, argv[1]);
            int meth_first = 0;
            if (a0 && a1) {
                char m0[16];
                size_t i, n = strlen(a0);
                if (n > 0 && n < sizeof(m0)) {
                    for (i = 0; i < n; i++) {
                        char c = a0[i];
                        m0[i] = (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
                    }
                    m0[n] = 0;
                    if ((!strcmp(m0, "GET") || !strcmp(m0, "POST") || !strcmp(m0, "PUT") ||
                         !strcmp(m0, "DELETE") || !strcmp(m0, "HEAD") || !strcmp(m0, "PATCH") ||
                         !strcmp(m0, "OPTIONS")) &&
                        strstr(a1, "://")) {
                        meth_first = 1;
                    }
                }
            }
            if (meth_first) {
                method = a0;
                method_owned = 1;
                url = a1;
                url_owned = 1;
                if (argc >= 3) {
                    hdr_obj_to_buf(ctx, argv[2], hdr, sizeof(hdr));
                }
                if (argc >= 4) {
                    body = (uint8_t *)js_to_bytes(ctx, argv[3], &blen);
                }
            } else {
                url = a0;
                url_owned = a0 != NULL;
                method = a1;
                method_owned = a1 != NULL;
                if (argc >= 3) {
                    hdr_obj_to_buf(ctx, argv[2], hdr, sizeof(hdr));
                }
                if (argc >= 4) {
                    body = (uint8_t *)js_to_bytes(ctx, argv[3], &blen);
                }
            }
        } else if (!(url = JS_ToCString(ctx, argv[0]))) {
            return JS_NULL;
        } else {
            url_owned = 1;
            if (argc >= 2 && JS_IsObject(argv[1])) {
                JSValue m = JS_GetPropertyStr(ctx, argv[1], "method");
                JSValue h = JS_GetPropertyStr(ctx, argv[1], "headers");
                JSValue b = JS_GetPropertyStr(ctx, argv[1], "body");
                JSValue d = JS_GetPropertyStr(ctx, argv[1], "data");
                if (JS_IsString(m)) {
                    method = JS_ToCString(ctx, m);
                    method_owned = method != NULL;
                }
                hdr_obj_to_buf(ctx, h, hdr, sizeof(hdr));
                if (!JS_IsUndefined(b)) {
                    body = (uint8_t *)js_to_bytes(ctx, b, &blen);
                } else if (!JS_IsUndefined(d)) {
                    body = (uint8_t *)js_to_bytes(ctx, d, &blen);
                }
                JS_FreeValue(ctx, m);
                JS_FreeValue(ctx, h);
                JS_FreeValue(ctx, b);
                JS_FreeValue(ctx, d);
            }
        }
        if (!url) {
            free(body);
            if (method_owned) {
                JS_FreeCString(ctx, method);
            }
            return JS_NULL;
        }
        if (http_body(method ? method : "GET", url, hdr[0] ? hdr : NULL, body, blen, &resp, &rn,
                      &status) != 0) {
            free(body);
            if (method_owned) {
                JS_FreeCString(ctx, method);
            }
            if (url_owned) {
                JS_FreeCString(ctx, url);
            }
            return JS_NULL;
        }
        free(body);
        obj = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, obj, "code", JS_NewInt32(ctx, status));
        JS_SetPropertyStr(ctx, obj, "status", JS_NewInt32(ctx, status));
        jbody = bytes_to_js(ctx, resp, rn);
        JS_SetPropertyStr(ctx, obj, "body", jbody);
        JS_SetPropertyStr(ctx, obj, "url", JS_NewString(ctx, url));
        free(resp);
        if (method_owned) {
            JS_FreeCString(ctx, method);
        }
        if (url_owned) {
            JS_FreeCString(ctx, url);
        }
        return obj;
    }
    case M_QUERY: {
        const char *u, *key, *q, *p;
        size_t klen;
        char out[1024];
        out[0] = 0;
        if (argc < 2) {
            return JS_NewString(ctx, "");
        }
        u = JS_ToCString(ctx, argv[0]);
        key = JS_ToCString(ctx, argv[1]);
        if (!u || !key) {
            if (u) {
                JS_FreeCString(ctx, u);
            }
            if (key) {
                JS_FreeCString(ctx, key);
            }
            return JS_NewString(ctx, "");
        }
        q = strchr(u, '?');
        klen = strlen(key);
        if (q) {
            p = q + 1;
            while (*p) {
                if (strncmp(p, key, klen) == 0 && p[klen] == '=') {
                    const char *e = p + klen + 1;
                    const char *amp = strchr(e, '&');
                    size_t n = amp ? (size_t)(amp - e) : strlen(e);
                    if (n >= sizeof(out)) {
                        n = sizeof(out) - 1;
                    }
                    memcpy(out, e, n);
                    out[n] = 0;
                    break;
                }
                p = strchr(p, '&');
                if (!p) {
                    break;
                }
                p++;
            }
        }
        JS_FreeCString(ctx, u);
        JS_FreeCString(ctx, key);
        return JS_NewString(ctx, out);
    }
    case M_MD5:
    case M_SHA1:
    case M_SHA256: {
        size_t n = 0;
        char *data = argc >= 1 ? js_to_bytes(ctx, argv[0], &n) : NULL;
        char hex[65];
        unsigned char dig[32];
        unsigned dn = 0;
        unsigned i;
        if (!data) {
            return JS_NewString(ctx, "");
        }
#ifdef MS_QJS_HOST_STUB
        /* 宿主自检不链 OpenSSL：填固定长度假摘要，仅保证 API 可调 */
        dn = magic == M_MD5 ? 16u : (magic == M_SHA1 ? 20u : 32u);
        memset(dig, 0xab, dn);
        (void)n;
#else
        /* slim OpenSSL 开 OPENSSL_NO_DEPRECATED，禁用 MD5()/SHA*()，走 EVP */
        {
            const EVP_MD *md = magic == M_MD5 ? EVP_md5()
                    : (magic == M_SHA1 ? EVP_sha1() : EVP_sha256());
            unsigned int outl = 0;
            dn = magic == M_MD5 ? 16u : (magic == M_SHA1 ? 20u : 32u);
            if (!md || EVP_Digest(data, n, dig, &outl, md, NULL) != 1) {
                free(data);
                return JS_NewString(ctx, "");
            }
            if (outl > 0 && outl <= dn) {
                dn = outl;
            }
        }
#endif
        free(data);
        for (i = 0; i < dn; i++) {
            sprintf(hex + i * 2, "%02x", dig[i]);
        }
        return JS_NewString(ctx, hex);
    }
    case M_TIME:
        return JS_NewInt64(ctx, (int64_t)time(NULL));
    case M_NOWMS: {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        return JS_NewInt64(ctx, (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
    }
    case M_B64E:
    case M_B64D:
    case M_HEXE:
    case M_HEXD: {
        size_t n = 0;
        char *data = argc >= 1 ? js_to_bytes(ctx, argv[0], &n) : NULL;
        JSValue ret = JS_NULL;
        if (!data) {
            return JS_NULL;
        }
        if (magic == M_HEXE) {
            char *hex = (char *)malloc(n * 2 + 1);
            size_t i;
            if (!hex) {
                free(data);
                return JS_NULL;
            }
            for (i = 0; i < n; i++) {
                sprintf(hex + i * 2, "%02x", (unsigned char)data[i]);
            }
            ret = JS_NewString(ctx, hex);
            free(hex);
        } else if (magic == M_HEXD) {
            size_t i, o = 0;
            uint8_t *bin;
            if (n % 2) {
                free(data);
                return JS_NULL;
            }
            bin = (uint8_t *)malloc(n / 2 + 1);
            if (!bin) {
                free(data);
                return JS_NULL;
            }
            for (i = 0; i + 1 < n; i += 2) {
                unsigned v = 0;
                sscanf(data + i, "%2x", &v);
                bin[o++] = (uint8_t)v;
            }
            ret = bin_to_js(ctx, bin, o);
            free(bin);
#ifdef MS_QJS_HOST_STUB
        } else {
            ret = JS_NewString(ctx, magic == M_B64E ? "" : "");
#else
        } else if (magic == M_B64E) {
            int outl = 4 * ((int)n + 2) / 3 + 4;
            char *b64 = (char *)malloc((size_t)outl);
            int w;
            if (!b64) {
                free(data);
                return JS_NULL;
            }
            w = EVP_EncodeBlock((unsigned char *)b64, (unsigned char *)data, (int)n);
            if (w < 0) {
                free(b64);
                free(data);
                return JS_NULL;
            }
            b64[w] = 0;
            ret = JS_NewString(ctx, b64);
            free(b64);
        } else {
            int outl = (int)n;
            unsigned char *bin = (unsigned char *)malloc((size_t)outl + 1);
            int w;
            if (!bin) {
                free(data);
                return JS_NULL;
            }
            w = EVP_DecodeBlock(bin, (unsigned char *)data, (int)n);
            if (w < 0) {
                free(bin);
                free(data);
                return JS_NULL;
            }
            while (w > 0 && n > 0 && data[n - 1] == '=') {
                w--;
                n--;
            }
            ret = bin_to_js(ctx, bin, (size_t)w);
            free(bin);
#endif
        }
        free(data);
        return ret;
    }
    case M_AES_CBC_DEC:
    case M_AES_CBC_ENC:
    case M_AES_ECB_DEC:
    case M_XXTEA_DEC:
    case M_XXTEA_ENC:
    case M_RSA_ENC:
    case M_RSA_SIGN:
    case M_RSA_VER: {
        size_t n0 = 0, n1 = 0, n2 = 0, ol = 0;
        char *a0 = argc >= 1 ? js_to_bytes(ctx, argv[0], &n0) : NULL;
        char *a1 = argc >= 2 ? js_to_bytes(ctx, argv[1], &n1) : NULL;
        char *a2 = argc >= 3 ? js_to_bytes(ctx, argv[2], &n2) : NULL;
        unsigned char *out = NULL;
        JSValue ret = JS_NULL;
        if (magic == M_AES_CBC_DEC || magic == M_AES_CBC_ENC) {
            out = ms_qjs_aes_cbc(magic == M_AES_CBC_ENC ? 1 : 0, (unsigned char *)a0, n0,
                                 (unsigned char *)a1, n1, (unsigned char *)a2, n2, &ol);
        } else if (magic == M_AES_ECB_DEC) {
            out = ms_qjs_aes_ecb_dec((unsigned char *)a0, n0, (unsigned char *)a1, n1, &ol);
        } else if (magic == M_XXTEA_DEC) {
            out = ms_qjs_xxtea_dec((unsigned char *)a0, n0, (unsigned char *)a1, n1, &ol);
        } else if (magic == M_XXTEA_ENC) {
            out = ms_qjs_xxtea_enc((unsigned char *)a0, n0, (unsigned char *)a1, n1, &ol);
        } else if (magic == M_RSA_ENC) {
            out = ms_qjs_rsa_pub_encrypt(a0, n0, (unsigned char *)a1, n1, &ol);
        } else if (magic == M_RSA_SIGN) {
            if (argc >= 3) {
                out = ms_qjs_rsa_sign(a0, n0, a1 && n1 ? a1 : "sha256", (unsigned char *)a2, n2,
                                      &ol);
            } else {
                out = ms_qjs_rsa_sign(a0, n0, "sha256", (unsigned char *)a1, n1, &ol);
            }
        } else if (magic == M_RSA_VER) {
            int ok = 0;
            char *a3 = argc >= 4 ? js_to_bytes(ctx, argv[3], &ol) : NULL;
            /* verify(pem, algo, data, sig) — 若缺 algo 则 (pem, data, sig) */
            if (argc >= 4) {
                ok = ms_qjs_rsa_verify(a0, n0, a1, (unsigned char *)a2, n2, (unsigned char *)a3, ol);
            } else {
                ok = ms_qjs_rsa_verify(a0, n0, "sha256", (unsigned char *)a1, n1,
                                       (unsigned char *)a2, n2);
            }
            free(a3);
            free(a0);
            free(a1);
            free(a2);
            return JS_NewBool(ctx, ok);
        }
        free(a0);
        free(a1);
        free(a2);
        if (!out) {
            return JS_NULL;
        }
        ret = bin_to_js(ctx, out, ol);
        free(out);
        return ret;
    }
    case M_HMAC_SHA256: {
        size_t n0 = 0, n1 = 0, ol = 0;
        char *key = argc >= 1 ? js_to_bytes(ctx, argv[0], &n0) : NULL;
        char *msg = argc >= 2 ? js_to_bytes(ctx, argv[1], &n1) : NULL;
        unsigned char *raw;
        char hex[65];
        unsigned i;
        if (!key || !msg) {
            free(key);
            free(msg);
            return JS_NewString(ctx, "");
        }
        raw = ms_qjs_hmac_sha256((unsigned char *)key, n0, (unsigned char *)msg, n1, &ol);
        free(key);
        free(msg);
        if (!raw || ol != 32) {
            free(raw);
            return JS_NewString(ctx, "");
        }
        for (i = 0; i < 32; i++) {
            sprintf(hex + i * 2, "%02x", raw[i]);
        }
        free(raw);
        return JS_NewString(ctx, hex);
    }
    case M_CIPHER: {
        /* cipher(name, mode, data, key, iv?, opts?) */
        size_t nn = 0, nmode = 0, nd = 0, nk = 0, ni = 0, ol = 0;
        char *name = argc >= 1 ? js_to_bytes(ctx, argv[0], &nn) : NULL;
        char *mode = argc >= 2 ? js_to_bytes(ctx, argv[1], &nmode) : NULL;
        char *data = argc >= 3 ? js_to_bytes(ctx, argv[2], &nd) : NULL;
        char *key = argc >= 4 ? js_to_bytes(ctx, argv[3], &nk) : NULL;
        char *iv = argc >= 5 ? js_to_bytes(ctx, argv[4], &ni) : NULL;
        int enc = 0, padding = 1;
        unsigned char *out;
        JSValue ret;
        if (mode && nmode > 0 && (mode[0] == 'e' || mode[0] == 'E')) {
            enc = 1;
        }
        if (argc >= 6 && JS_IsObject(argv[5])) {
            JSValue p = JS_GetPropertyStr(ctx, argv[5], "padding");
            if (JS_IsBool(p)) {
                padding = JS_ToBool(ctx, p) ? 1 : 0;
            }
            JS_FreeValue(ctx, p);
        }
        out = (name && data && key)
                      ? ms_qjs_cipher(name, enc, (unsigned char *)data, nd, (unsigned char *)key, nk,
                                      (unsigned char *)iv, ni, padding, &ol)
                      : NULL;
        free(name);
        free(mode);
        free(data);
        free(key);
        free(iv);
        if (!out) {
            return JS_NULL;
        }
        ret = bin_to_js(ctx, out, ol);
        free(out);
        return ret;
    }
    case M_URLENC: {
        size_t n = 0, i, o = 0;
        char *in = argc >= 1 ? js_to_bytes(ctx, argv[0], &n) : NULL;
        char *out;
        JSValue ret;
        if (!in) {
            return JS_NewString(ctx, "");
        }
        out = (char *)malloc(n * 3 + 1);
        if (!out) {
            free(in);
            return JS_NULL;
        }
        for (i = 0; i < n; i++) {
            unsigned char c = (unsigned char)in[i];
            if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                c == '-' || c == '_' || c == '.' || c == '~') {
                out[o++] = (char)c;
            } else if (c == ' ') {
                out[o++] = '+';
            } else {
                sprintf(out + o, "%%%02X", c);
                o += 3;
            }
        }
        out[o] = 0;
        ret = JS_NewStringLen(ctx, out, o);
        free(out);
        free(in);
        return ret;
    }
    case M_URLDEC: {
        size_t n = 0, i, o = 0;
        char *in = argc >= 1 ? js_to_bytes(ctx, argv[0], &n) : NULL;
        char *out;
        JSValue ret;
        if (!in) {
            return JS_NewString(ctx, "");
        }
        out = (char *)malloc(n + 1);
        if (!out) {
            free(in);
            return JS_NULL;
        }
        for (i = 0; i < n; i++) {
            if (in[i] == '+' ) {
                out[o++] = ' ';
            } else if (in[i] == '%' && i + 2 < n) {
                unsigned v = 0;
                sscanf(in + i + 1, "%2x", &v);
                out[o++] = (char)v;
                i += 2;
            } else {
                out[o++] = in[i];
            }
        }
        out[o] = 0;
        ret = JS_NewStringLen(ctx, out, o);
        free(out);
        free(in);
        return ret;
    }
    case M_RAND: {
        int32_t want = 16;
        unsigned char *buf;
        size_t i;
        JSValue ret;
        if (argc >= 1) {
            JS_ToInt32(ctx, &want, argv[0]);
        }
        if (want <= 0 || want > (1 << 20)) {
            return JS_NULL;
        }
        buf = (unsigned char *)malloc((size_t)want);
        if (!buf) {
            return JS_NULL;
        }
#ifdef MS_QJS_HOST_STUB
        for (i = 0; i < (size_t)want; i++) {
            buf[i] = (unsigned char)(i * 17 + 3);
        }
#else
        if (RAND_bytes(buf, want) != 1) {
            /* 不用 libc rand（API18 对不上 rand@LIBC）；固定填充仅保接口 */
            for (i = 0; i < (size_t)want; i++) {
                buf[i] = (unsigned char)(i * 17 + 3);
            }
        }
#endif
        ret = bin_to_js(ctx, buf, (size_t)want);
        free(buf);
        return ret;
    }
    case M_CACHE_GET: {
        size_t nk = 0;
        char *key = argc >= 1 ? js_to_bytes(ctx, argv[0], &nk) : NULL;
        JSValue ret = JS_NULL;
        int i;
        int64_t now = wall_ms();
        if (!key) {
            return JS_NULL;
        }
        pthread_mutex_lock(&g_cache_mu);
        for (i = 0; i < CACHE_MAX; i++) {
            if (g_cache[i].key && strcmp(g_cache[i].key, key) == 0) {
                if (g_cache[i].exp_ms && g_cache[i].exp_ms < now) {
                    free(g_cache[i].key);
                    free(g_cache[i].val);
                    memset(&g_cache[i], 0, sizeof(g_cache[i]));
                    break;
                }
                ret = JS_NewStringLen(ctx, (char *)g_cache[i].val, g_cache[i].n);
                break;
            }
        }
        pthread_mutex_unlock(&g_cache_mu);
        free(key);
        return ret;
    }
    case M_CACHE_SET: {
        size_t nk = 0, nv = 0;
        char *key = argc >= 1 ? js_to_bytes(ctx, argv[0], &nk) : NULL;
        char *val = argc >= 2 ? js_to_bytes(ctx, argv[1], &nv) : NULL;
        int64_t ttl = 0;
        int i, slot = -1;
        if (argc >= 3) {
            JS_ToInt64(ctx, &ttl, argv[2]);
        }
        if (!key || !val || nv > CACHE_VAL_MAX) {
            free(key);
            free(val);
            return JS_UNDEFINED;
        }
        pthread_mutex_lock(&g_cache_mu);
        for (i = 0; i < CACHE_MAX; i++) {
            if (g_cache[i].key && strcmp(g_cache[i].key, key) == 0) {
                free(g_cache[i].val);
                g_cache[i].val = (unsigned char *)val;
                g_cache[i].n = nv;
                g_cache[i].exp_ms = ttl > 0 ? wall_ms() + ttl : 0;
                val = NULL;
                slot = i;
                break;
            }
            if (slot < 0 && !g_cache[i].key) {
                slot = i;
            }
        }
        if (val && slot >= 0) {
            free(g_cache[slot].key);
            free(g_cache[slot].val);
            g_cache[slot].key = key;
            g_cache[slot].val = (unsigned char *)val;
            g_cache[slot].n = nv;
            g_cache[slot].exp_ms = ttl > 0 ? wall_ms() + ttl : 0;
            key = NULL;
            val = NULL;
        } else if (val && slot < 0) {
            /* 满：覆盖 0 */
            free(g_cache[0].key);
            free(g_cache[0].val);
            g_cache[0].key = key;
            g_cache[0].val = (unsigned char *)val;
            g_cache[0].n = nv;
            g_cache[0].exp_ms = ttl > 0 ? wall_ms() + ttl : 0;
            key = NULL;
            val = NULL;
        }
        pthread_mutex_unlock(&g_cache_mu);
        free(key);
        free(val);
        return JS_UNDEFINED;
    }
    default:
        return JS_UNDEFINED;
    }
}

static void bind1(JSContext *ctx, JSValue obj, const char *name, int magic, int len)
{
    JS_SetPropertyStr(ctx, obj, name,
                      JS_NewCFunctionMagic(ctx, api_magic, name, len, JS_CFUNC_generic_magic, magic));
}

void ms_qjs_bind(JSContext *ctx, JSValue global)
{
    JSValue ms = JS_NewObject(ctx);
    JSValue ku9 = JS_NewObject(ctx);

    /* 网络 / 日志；ku9/cjs 同参别名（超级直播脚本可直接跑） */
    bind1(ctx, ms, "log", M_LOG, 1);
    bind1(ctx, ms, "get", M_GET, 1);
    bind1(ctx, ms, "http_get", M_GET, 1);
    bind1(ctx, ms, "httpGet", M_GET, 1);
    bind1(ctx, ms, "download", M_GET, 1);
    bind1(ctx, ms, "getbin", M_GETBIN, 1);
    bind1(ctx, ms, "getBin", M_GETBIN, 1);
    bind1(ctx, ms, "get_bin", M_GETBIN, 1);
    bind1(ctx, ms, "post", M_POST, 2);
    bind1(ctx, ms, "request", M_REQ, 2);
    bind1(ctx, ms, "http_request", M_REQ, 2);
    bind1(ctx, ms, "httpRequest", M_REQ, 2);
    bind1(ctx, ms, "getQuery", M_QUERY, 2);

    bind1(ctx, ku9, "log", M_LOG, 1);
    bind1(ctx, ku9, "get", M_GET, 1);
    bind1(ctx, ku9, "post", M_POST, 2);
    bind1(ctx, ku9, "request", M_REQ, 2);
    bind1(ctx, ku9, "getQuery", M_QUERY, 2);
    bind1(ctx, ku9, "getCache", M_CACHE_GET, 1);
    bind1(ctx, ku9, "setCache", M_CACHE_SET, 2);
    bind1(ctx, ku9, "md5", M_MD5, 1);
    bind1(ctx, ku9, "sha1", M_SHA1, 1);
    bind1(ctx, ku9, "sha256", M_SHA256, 1);
    bind1(ctx, ku9, "hex_decode", M_HEXD, 1);
    bind1(ctx, ku9, "hex_encode", M_HEXE, 1);
    bind1(ctx, ku9, "b64_decode", M_B64D, 1);
    bind1(ctx, ku9, "b64_encode", M_B64E, 1);
    bind1(ctx, ku9, "urlencode", M_URLENC, 1);
    bind1(ctx, ku9, "urldecode", M_URLDEC, 1);
    bind1(ctx, ku9, "now_ms", M_NOWMS, 0);
    bind1(ctx, ku9, "time", M_TIME, 0);

    bind1(ctx, ms, "md5", M_MD5, 1);
    bind1(ctx, ms, "sha1", M_SHA1, 1);
    bind1(ctx, ms, "sha256", M_SHA256, 1);
    bind1(ctx, ms, "b64_encode", M_B64E, 1);
    bind1(ctx, ms, "b64_decode", M_B64D, 1);
    bind1(ctx, ms, "hex_encode", M_HEXE, 1);
    bind1(ctx, ms, "hex_decode", M_HEXD, 1);
    bind1(ctx, ms, "bin2hex", M_HEXE, 1);
    bind1(ctx, ms, "hex2bin", M_HEXD, 1);
    bind1(ctx, ms, "time", M_TIME, 0);
    bind1(ctx, ms, "current_time_sec", M_TIME, 0);
    bind1(ctx, ms, "current_time_millis", M_NOWMS, 0);
    bind1(ctx, ms, "now_ms", M_NOWMS, 0);

    bind1(ctx, ms, "aes_cbc_decrypt", M_AES_CBC_DEC, 3);
    bind1(ctx, ms, "aes_cbc_encrypt", M_AES_CBC_ENC, 3);
    bind1(ctx, ms, "aes_ecb_decrypt", M_AES_ECB_DEC, 2);
    bind1(ctx, ms, "xxtea_decrypt", M_XXTEA_DEC, 2);
    bind1(ctx, ms, "xxtea_encrypt", M_XXTEA_ENC, 2);
    bind1(ctx, ms, "rsa_public_encrypt", M_RSA_ENC, 2);
    bind1(ctx, ms, "rsaPublicEncrypt", M_RSA_ENC, 2);
    bind1(ctx, ms, "rsa_sign", M_RSA_SIGN, 2);
    bind1(ctx, ms, "rsaSign", M_RSA_SIGN, 2);
    bind1(ctx, ms, "rsa_verify", M_RSA_VER, 3);
    bind1(ctx, ms, "rsaVerify", M_RSA_VER, 3);
    bind1(ctx, ms, "hmac_sha256", M_HMAC_SHA256, 2);
    bind1(ctx, ms, "hmacSha256", M_HMAC_SHA256, 2);
    bind1(ctx, ms, "cipher", M_CIPHER, 4);
    bind1(ctx, ms, "urlencode", M_URLENC, 1);
    bind1(ctx, ms, "urldecode", M_URLDEC, 1);
    bind1(ctx, ms, "random_bytes", M_RAND, 1);
    bind1(ctx, ms, "randomBytes", M_RAND, 1);
    bind1(ctx, ms, "getCache", M_CACHE_GET, 1);
    bind1(ctx, ms, "setCache", M_CACHE_SET, 2); /* (key, val, ttlMs?) */
    bind1(ctx, ms, "get_cache", M_CACHE_GET, 1);
    bind1(ctx, ms, "set_cache", M_CACHE_SET, 2);

    bind1(ctx, ms, "aesCbcDecrypt", M_AES_CBC_DEC, 3);
    bind1(ctx, ms, "aesCbcEncrypt", M_AES_CBC_ENC, 3);
    bind1(ctx, ms, "aesEcbDecrypt", M_AES_ECB_DEC, 2);
    bind1(ctx, ms, "xxteaDecrypt", M_XXTEA_DEC, 2);
    bind1(ctx, ms, "xxteaEncrypt", M_XXTEA_ENC, 2);
    bind1(ctx, ms, "b64Encode", M_B64E, 1);
    bind1(ctx, ms, "b64Decode", M_B64D, 1);
    bind1(ctx, ms, "hexEncode", M_HEXE, 1);
    bind1(ctx, ms, "hexDecode", M_HEXD, 1);

    JS_SetPropertyStr(ctx, global, "ms", ms);
    JS_SetPropertyStr(ctx, global, "ku9", ku9);
    JS_SetPropertyStr(ctx, global, "cjs", JS_DupValue(ctx, ku9));
    /* 浏览器常见全局 */
    {
        JSValue enc = JS_GetPropertyStr(ctx, ms, "urlencode");
        JSValue dec = JS_GetPropertyStr(ctx, ms, "urldecode");
        JS_SetPropertyStr(ctx, global, "encodeURIComponent", JS_DupValue(ctx, enc));
        JS_SetPropertyStr(ctx, global, "decodeURIComponent", JS_DupValue(ctx, dec));
        JS_FreeValue(ctx, enc);
        JS_FreeValue(ctx, dec);
    }
}
