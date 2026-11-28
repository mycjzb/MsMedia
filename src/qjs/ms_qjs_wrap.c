/*
 * JS 脚本 unwrap：明文 / gzip / MSJS(AES-256-GCM±gzip)。
 * by: Long <mycjzb@gmail.com>
 */
#include "ms_qjs_wrap.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifndef MS_QJS_HOST_STUB
#include <openssl/evp.h>
#include <zlib.h>
#endif

#define SCRIPT_CAP (512u * 1024u)

/* QuickJS：JS_Eval 要求 input[input_len]==0（见 quickjs.h） */
static int ensure_nul(unsigned char **pp, size_t n)
{
    unsigned char *p, *nb;
    if (!pp || !*pp) {
        return -1;
    }
    p = *pp;
    nb = (unsigned char *)realloc(p, n + 1);
    if (!nb) {
        return -1;
    }
    nb[n] = 0;
    *pp = nb;
    return 0;
}

/* 密钥碎片 XOR（与超级直播 PluginKeys 不同） */
static const unsigned char kA[32] = {
        0x4A, 0x91, 0x2E, 0x7C, 0xB3, 0x18, 0xD5, 0x60, 0x0F, 0xA4, 0x57, 0xE2, 0x39, 0x8B, 0xC6, 0x1D,
        0x72, 0xFE, 0x45, 0xA0, 0x2B, 0x96, 0xDB, 0x3C, 0x81, 0xF4, 0x59, 0x0E, 0xC3, 0x68, 0xAD, 0x14};
static const unsigned char kB[32] = {
        0xE7, 0x3A, 0x9D, 0x50, 0x0B, 0xC6, 0x71, 0x24, 0xD8, 0x4F, 0xA2, 0x15, 0xB9, 0x6C, 0x01, 0xF3,
        0x5E, 0xA9, 0x34, 0xCF, 0x7A, 0x15, 0xB0, 0x4D, 0xE8, 0x63, 0x2E, 0x91, 0xD4, 0x47, 0x0A, 0xBD};
static const unsigned char kSalt[8] = {0x5A, 0xC3, 0x17, 0x8E, 0x42, 0xF1, 0x6B, 0x29};

/* "MSJS" ^ 0x5A */
static const unsigned char kMagObf[4] = {0x17, 0x09, 0x10, 0x09};

static void assemble_key(unsigned char out[32])
{
    size_t i;
    for (i = 0; i < 32; i++) {
        out[i] = (unsigned char)(kA[i] ^ kB[i] ^ kSalt[i % 8]);
    }
}

static int magic_msjs(const unsigned char *p)
{
    unsigned char m[4];
    int i, ok;
    for (i = 0; i < 4; i++) {
        m[i] = (unsigned char)(kMagObf[i] ^ 0x5A);
    }
    ok = p[0] == m[0] && p[1] == m[1] && p[2] == m[2] && p[3] == m[3];
    memset(m, 0, 4);
    return ok;
}

static uint16_t be16(const unsigned char *p)
{
    return (uint16_t)(((unsigned)p[0] << 8) | p[1]);
}

static uint32_t be32(const unsigned char *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static int has_main(const unsigned char *p, size_t n)
{
    size_t i;
    if (!p || n < 12) {
        return 0;
    }
    for (i = 0; i + 13 <= n; i++) {
        if (memcmp(p + i, "function main", 13) == 0) {
            return 1;
        }
        if (i + 19 <= n && memcmp(p + i, "async function main", 19) == 0) {
            return 1;
        }
    }
    return 0;
}

int ms_qjs_looks_wrapped(const unsigned char *in, size_t in_len)
{
    size_t i = 0;
    if (!in || in_len == 0) {
        return 0;
    }
    if (in_len >= 3 && in[0] == 0xef && in[1] == 0xbb && in[2] == 0xbf) {
        i = 3;
    }
    while (i < in_len && (in[i] == ' ' || in[i] == '\t' || in[i] == '\r' || in[i] == '\n')) {
        i++;
    }
    if (i >= in_len) {
        return 0;
    }
    if (in_len - i >= 2 && in[i] == 0x1f && in[i + 1] == 0x8b) {
        return 1;
    }
    if (in_len - i >= 4 && magic_msjs(in + i)) {
        return 1;
    }
    return has_main(in + i, in_len - i);
}

#ifndef MS_QJS_HOST_STUB

static unsigned char *gunzip_alloc(const unsigned char *in, size_t in_len, size_t *out_len)
{
    z_stream zs;
    unsigned char *out = NULL;
    size_t cap = in_len * 4 + 64;
    int ret;
    memset(&zs, 0, sizeof(zs));
    if (inflateInit2(&zs, 15 + 32) != Z_OK) {
        return NULL;
    }
    out = (unsigned char *)malloc(cap);
    if (!out) {
        inflateEnd(&zs);
        return NULL;
    }
    zs.next_in = (Bytef *)in;
    zs.avail_in = (uInt)in_len;
    for (;;) {
        if (zs.total_out + 64 >= cap) {
            unsigned char *nb;
            cap *= 2;
            if (cap > SCRIPT_CAP + 64) {
                free(out);
                inflateEnd(&zs);
                return NULL;
            }
            nb = (unsigned char *)realloc(out, cap);
            if (!nb) {
                free(out);
                inflateEnd(&zs);
                return NULL;
            }
            out = nb;
        }
        zs.next_out = out + zs.total_out;
        zs.avail_out = (uInt)(cap - zs.total_out);
        ret = inflate(&zs, Z_NO_FLUSH);
        if (ret == Z_STREAM_END) {
            break;
        }
        if (ret != Z_OK) {
            free(out);
            inflateEnd(&zs);
            return NULL;
        }
    }
    inflateEnd(&zs);
    if (zs.total_out == 0 || zs.total_out > SCRIPT_CAP) {
        free(out);
        return NULL;
    }
    if (ensure_nul(&out, zs.total_out) != 0) {
        free(out);
        return NULL;
    }
    *out_len = zs.total_out;
    return out;
}

static unsigned char *msjs_decrypt(const unsigned char *blob, size_t blob_len, size_t *out_len)
{
    unsigned char key[32], *plain = NULL, *finalp = NULL;
    size_t clen, plain_len = 0, final_len = 0;
    uint16_t flags;
    const unsigned char *iv, *ct, *tag;
    EVP_CIPHER_CTX *ctx;
    int ol1 = 0, ol2 = 0;
    *out_len = 0;
    if (blob_len < 4 + 2 + 2 + 12 + 4 + 16) {
        return NULL;
    }
    if (!magic_msjs(blob)) {
        return NULL;
    }
    if (be16(blob + 4) != 1) {
        return NULL; /* 只认 ver=1 */
    }
    flags = be16(blob + 6);
    if ((flags & 1) == 0) {
        return NULL; /* 只要 GCM */
    }
    iv = blob + 8;
    clen = be32(blob + 20);
    if (clen == 0 || 24 + clen + 16 > blob_len) {
        return NULL;
    }
    ct = blob + 24;
    tag = ct + clen;
    assemble_key(key);
    plain = (unsigned char *)malloc(clen + 1);
    if (!plain) {
        memset(key, 0, 32);
        return NULL;
    }
    ctx = EVP_CIPHER_CTX_new();
    if (!ctx || EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL) != 1 ||
        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, 12, NULL) != 1 ||
        EVP_DecryptInit_ex(ctx, NULL, NULL, key, iv) != 1 ||
        EVP_DecryptUpdate(ctx, plain, &ol1, ct, (int)clen) != 1 ||
        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, 16, (void *)tag) != 1 ||
        EVP_DecryptFinal_ex(ctx, plain + ol1, &ol2) != 1) {
        free(plain);
        if (ctx) {
            EVP_CIPHER_CTX_free(ctx);
        }
        memset(key, 0, 32);
        return NULL;
    }
    EVP_CIPHER_CTX_free(ctx);
    memset(key, 0, 32);
    plain_len = (size_t)(ol1 + ol2);
    if (flags & 4) {
        finalp = gunzip_alloc(plain, plain_len, &final_len);
        free(plain);
        if (!finalp) {
            return NULL;
        }
        *out_len = final_len;
        return finalp; /* gunzip_alloc 已 NUL */
    }
    if (plain_len > SCRIPT_CAP) {
        free(plain);
        return NULL;
    }
    plain[plain_len] = 0; /* malloc(clen+1) */
    *out_len = plain_len;
    return plain;
}

int ms_qjs_unwrap(const unsigned char *in, size_t in_len, unsigned char **out, size_t *out_len)
{
    size_t i = 0;
    unsigned char *p;
    size_t n;
    *out = NULL;
    *out_len = 0;
    if (!in || in_len == 0 || in_len > SCRIPT_CAP || !out || !out_len) {
        return -1;
    }
    if (in_len >= 3 && in[0] == 0xef && in[1] == 0xbb && in[2] == 0xbf) {
        i = 3;
    }
    while (i < in_len && (in[i] == ' ' || in[i] == '\t' || in[i] == '\r' || in[i] == '\n')) {
        i++;
    }
    if (i >= in_len) {
        return -1;
    }
    if (in_len - i >= 4 && magic_msjs(in + i)) {
        p = msjs_decrypt(in + i, in_len - i, &n);
        if (!p || !has_main(p, n)) {
            free(p);
            return -1;
        }
        *out = p;
        *out_len = n;
        return 0;
    }
    if (in_len - i >= 2 && in[i] == 0x1f && in[i + 1] == 0x8b) {
        p = gunzip_alloc(in + i, in_len - i, &n);
        if (!p || !has_main(p, n)) {
            free(p);
            return -1;
        }
        *out = p;
        *out_len = n;
        return 0;
    }
    /* 明文 */
    if (!has_main(in + i, in_len - i) || in_len - i > SCRIPT_CAP) {
        return -1;
    }
    n = in_len - i;
    p = (unsigned char *)malloc(n + 1);
    if (!p) {
        return -1;
    }
    memcpy(p, in + i, n);
    p[n] = 0;
    *out = p;
    *out_len = n;
    return 0;
}

#else /* HOST_STUB：只放行明文 */

int ms_qjs_unwrap(const unsigned char *in, size_t in_len, unsigned char **out, size_t *out_len)
{
    size_t i = 0;
    unsigned char *p;
    size_t n;
    *out = NULL;
    *out_len = 0;
    if (!in || in_len == 0 || in_len > SCRIPT_CAP || !out || !out_len) {
        return -1;
    }
    if (in_len >= 3 && in[0] == 0xef && in[1] == 0xbb && in[2] == 0xbf) {
        i = 3;
    }
    while (i < in_len && (in[i] == ' ' || in[i] == '\t' || in[i] == '\r' || in[i] == '\n')) {
        i++;
    }
    if (!has_main(in + i, in_len - i) || in_len - i > SCRIPT_CAP) {
        return -1;
    }
    n = in_len - i;
    p = (unsigned char *)malloc(n + 1);
    if (!p) {
        return -1;
    }
    memcpy(p, in + i, n);
    p[n] = 0;
    *out = p;
    *out_len = n;
    return 0;
}

#endif
