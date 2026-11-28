/*
 * JS 宿主加密：OpenSSL AES/RSA + 经典 XXTEA（带长度字，对齐 js-rules/PHP）。
 * by: Long <mycjzb@gmail.com>
 */
#include "ms_qjs_crypto.h"

#ifndef MS_QJS_HOST_STUB
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#endif

#include <stdlib.h>
#include <string.h>

#ifndef MS_QJS_HOST_STUB

static unsigned char *aes_crypt(int enc, int cbc, const unsigned char *in, size_t in_len,
                                const unsigned char *key, size_t key_len, const unsigned char *iv,
                                size_t iv_len, size_t *out_len)
{
    EVP_CIPHER_CTX *ctx;
    const EVP_CIPHER *cip;
    unsigned char *out;
    int ol1 = 0, ol2 = 0, kn;
    *out_len = 0;
    if (!in || !key || in_len == 0) {
        return NULL;
    }
    kn = (int)key_len;
    if (kn == 16) {
        cip = cbc ? EVP_aes_128_cbc() : EVP_aes_128_ecb();
    } else if (kn == 24) {
        cip = cbc ? EVP_aes_192_cbc() : EVP_aes_192_ecb();
    } else if (kn == 32) {
        cip = cbc ? EVP_aes_256_cbc() : EVP_aes_256_ecb();
    } else {
        return NULL;
    }
    if (cbc && (!iv || iv_len < 16)) {
        return NULL;
    }
    ctx = EVP_CIPHER_CTX_new();
    if (!ctx) {
        return NULL;
    }
    out = (unsigned char *)malloc(in_len + 32);
    if (!out) {
        EVP_CIPHER_CTX_free(ctx);
        return NULL;
    }
    if (EVP_CipherInit_ex(ctx, cip, NULL, key, cbc ? iv : NULL, enc) != 1 ||
        EVP_CipherUpdate(ctx, out, &ol1, in, (int)in_len) != 1 ||
        EVP_CipherFinal_ex(ctx, out + ol1, &ol2) != 1) {
        free(out);
        EVP_CIPHER_CTX_free(ctx);
        return NULL;
    }
    EVP_CIPHER_CTX_free(ctx);
    *out_len = (size_t)(ol1 + ol2);
    return out;
}

unsigned char *ms_qjs_aes_cbc(int enc, const unsigned char *in, size_t in_len,
                              const unsigned char *key, size_t key_len, const unsigned char *iv,
                              size_t iv_len, size_t *out_len)
{
    return aes_crypt(enc, 1, in, in_len, key, key_len, iv, iv_len, out_len);
}

unsigned char *ms_qjs_aes_ecb_dec(const unsigned char *in, size_t in_len, const unsigned char *key,
                                  size_t key_len, size_t *out_len)
{
    return aes_crypt(0, 0, in, in_len, key, key_len, NULL, 0, out_len);
}

static EVP_PKEY *load_pem(const char *pem, size_t pem_len, int pub)
{
    BIO *bio;
    EVP_PKEY *k;
    char *tmp;
    if (!pem || pem_len == 0) {
        return NULL;
    }
    tmp = (char *)malloc(pem_len + 1);
    if (!tmp) {
        return NULL;
    }
    memcpy(tmp, pem, pem_len);
    tmp[pem_len] = 0;
    bio = BIO_new_mem_buf(tmp, (int)pem_len);
    if (!bio) {
        free(tmp);
        return NULL;
    }
    k = pub ? PEM_read_bio_PUBKEY(bio, NULL, NULL, NULL) : PEM_read_bio_PrivateKey(bio, NULL, NULL, NULL);
    BIO_free(bio);
    free(tmp);
    return k;
}

unsigned char *ms_qjs_rsa_pub_encrypt(const char *pem, size_t pem_len, const unsigned char *in,
                                      size_t in_len, size_t *out_len)
{
    EVP_PKEY *k;
    EVP_PKEY_CTX *c;
    unsigned char *out;
    size_t ol;
    *out_len = 0;
    k = load_pem(pem, pem_len, 1);
    if (!k) {
        return NULL;
    }
    c = EVP_PKEY_CTX_new(k, NULL);
    if (!c || EVP_PKEY_encrypt_init(c) <= 0 ||
        EVP_PKEY_CTX_set_rsa_padding(c, RSA_PKCS1_PADDING) <= 0) {
        EVP_PKEY_CTX_free(c);
        EVP_PKEY_free(k);
        return NULL;
    }
    ol = 0;
    if (EVP_PKEY_encrypt(c, NULL, &ol, in, in_len) <= 0) {
        EVP_PKEY_CTX_free(c);
        EVP_PKEY_free(k);
        return NULL;
    }
    out = (unsigned char *)malloc(ol);
    if (!out || EVP_PKEY_encrypt(c, out, &ol, in, in_len) <= 0) {
        free(out);
        EVP_PKEY_CTX_free(c);
        EVP_PKEY_free(k);
        return NULL;
    }
    EVP_PKEY_CTX_free(c);
    EVP_PKEY_free(k);
    *out_len = ol;
    return out;
}

unsigned char *ms_qjs_rsa_sign(const char *pem, size_t pem_len, const char *algo,
                               const unsigned char *in, size_t in_len, size_t *out_len)
{
    EVP_PKEY *k;
    EVP_MD_CTX *md;
    const EVP_MD *m;
    unsigned char *sig;
    size_t sl = 0;
    *out_len = 0;
    if (!algo) {
        algo = "sha256";
    }
    m = EVP_get_digestbyname(algo);
    if (!m) {
        return NULL;
    }
    k = load_pem(pem, pem_len, 0);
    if (!k) {
        return NULL;
    }
    md = EVP_MD_CTX_new();
    if (!md || EVP_DigestSignInit(md, NULL, m, NULL, k) <= 0 ||
        EVP_DigestSignUpdate(md, in, in_len) <= 0 || EVP_DigestSignFinal(md, NULL, &sl) <= 0) {
        EVP_MD_CTX_free(md);
        EVP_PKEY_free(k);
        return NULL;
    }
    sig = (unsigned char *)malloc(sl);
    if (!sig || EVP_DigestSignFinal(md, sig, &sl) <= 0) {
        free(sig);
        EVP_MD_CTX_free(md);
        EVP_PKEY_free(k);
        return NULL;
    }
    EVP_MD_CTX_free(md);
    EVP_PKEY_free(k);
    *out_len = sl;
    return sig;
}

int ms_qjs_rsa_verify(const char *pem, size_t pem_len, const char *algo, const unsigned char *in,
                      size_t in_len, const unsigned char *sig, size_t sig_len)
{
    EVP_PKEY *k;
    EVP_MD_CTX *md;
    const EVP_MD *m;
    int ok = 0;
    if (!algo) {
        algo = "sha256";
    }
    m = EVP_get_digestbyname(algo);
    if (!m) {
        return 0;
    }
    k = load_pem(pem, pem_len, 1);
    if (!k) {
        return 0;
    }
    md = EVP_MD_CTX_new();
    if (md && EVP_DigestVerifyInit(md, NULL, m, NULL, k) > 0 &&
        EVP_DigestVerifyUpdate(md, in, in_len) > 0 &&
        EVP_DigestVerifyFinal(md, sig, sig_len) > 0) {
        ok = 1;
    }
    EVP_MD_CTX_free(md);
    EVP_PKEY_free(k);
    return ok;
}

unsigned char *ms_qjs_hmac_sha256(const unsigned char *key, size_t key_len,
                                  const unsigned char *msg, size_t msg_len, size_t *out_len)
{
    unsigned char *out;
    unsigned int ol = 0;
    *out_len = 0;
    if (!key || !msg) {
        return NULL;
    }
    out = (unsigned char *)malloc(32);
    if (!out) {
        return NULL;
    }
    if (!HMAC(EVP_sha256(), key, (int)key_len, msg, msg_len, out, &ol) || ol != 32) {
        free(out);
        return NULL;
    }
    *out_len = 32;
    return out;
}

static int ieq(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') {
            ca = (char)(ca - 'A' + 'a');
        }
        if (cb >= 'A' && cb <= 'Z') {
            cb = (char)(cb - 'A' + 'a');
        }
        if (ca != cb) {
            return 0;
        }
    }
    return *a == 0 && *b == 0;
}

unsigned char *ms_qjs_cipher(const char *name, int enc, const unsigned char *in, size_t in_len,
                             const unsigned char *key, size_t key_len, const unsigned char *iv,
                             size_t iv_len, int padding, size_t *out_len)
{
    const EVP_CIPHER *cip = NULL;
    EVP_CIPHER_CTX *ctx;
    unsigned char *out;
    int ol1 = 0, ol2 = 0, kn;
    int is_ctr = 0, is_ecb = 0;
    *out_len = 0;
    if (!name || !in || !key) {
        return NULL;
    }
    if (ieq(name, "aes-128-ecb")) {
        cip = EVP_aes_128_ecb();
        is_ecb = 1;
        kn = 16;
    } else if (ieq(name, "aes-128-cbc")) {
        cip = EVP_aes_128_cbc();
        kn = 16;
    } else if (ieq(name, "aes-128-ctr")) {
        cip = EVP_aes_128_ctr();
        is_ctr = 1;
        kn = 16;
    } else if (ieq(name, "aes-256-ecb")) {
        cip = EVP_aes_256_ecb();
        is_ecb = 1;
        kn = 32;
    } else if (ieq(name, "aes-256-cbc")) {
        cip = EVP_aes_256_cbc();
        kn = 32;
    } else if (ieq(name, "aes-256-ctr")) {
        cip = EVP_aes_256_ctr();
        is_ctr = 1;
        kn = 32;
    } else {
        return NULL;
    }
    if ((int)key_len < kn) {
        return NULL;
    }
    if (!is_ecb && (!iv || iv_len < 16)) {
        return NULL;
    }
    if (!is_ctr && !padding && (in_len % 16) != 0) {
        return NULL;
    }
    ctx = EVP_CIPHER_CTX_new();
    if (!ctx) {
        return NULL;
    }
    out = (unsigned char *)malloc(in_len + 32);
    if (!out) {
        EVP_CIPHER_CTX_free(ctx);
        return NULL;
    }
    if (EVP_CipherInit_ex(ctx, cip, NULL, key, is_ecb ? NULL : iv, enc) != 1) {
        free(out);
        EVP_CIPHER_CTX_free(ctx);
        return NULL;
    }
    if (!padding || is_ctr) {
        EVP_CIPHER_CTX_set_padding(ctx, 0);
    }
    if (EVP_CipherUpdate(ctx, out, &ol1, in, (int)in_len) != 1 ||
        EVP_CipherFinal_ex(ctx, out + ol1, &ol2) != 1) {
        free(out);
        EVP_CIPHER_CTX_free(ctx);
        return NULL;
    }
    EVP_CIPHER_CTX_free(ctx);
    *out_len = (size_t)(ol1 + ol2);
    return out;
}

#else /* MS_QJS_HOST_STUB */

unsigned char *ms_qjs_aes_cbc(int enc, const unsigned char *in, size_t in_len,
                              const unsigned char *key, size_t key_len, const unsigned char *iv,
                              size_t iv_len, size_t *out_len)
{
    (void)enc;
    (void)in;
    (void)in_len;
    (void)key;
    (void)key_len;
    (void)iv;
    (void)iv_len;
    *out_len = 0;
    return NULL;
}
unsigned char *ms_qjs_aes_ecb_dec(const unsigned char *in, size_t in_len, const unsigned char *key,
                                  size_t key_len, size_t *out_len)
{
    (void)in;
    (void)in_len;
    (void)key;
    (void)key_len;
    *out_len = 0;
    return NULL;
}
unsigned char *ms_qjs_rsa_pub_encrypt(const char *pem, size_t pem_len, const unsigned char *in,
                                      size_t in_len, size_t *out_len)
{
    (void)pem;
    (void)pem_len;
    (void)in;
    (void)in_len;
    *out_len = 0;
    return NULL;
}
unsigned char *ms_qjs_rsa_sign(const char *pem, size_t pem_len, const char *algo,
                               const unsigned char *in, size_t in_len, size_t *out_len)
{
    (void)pem;
    (void)pem_len;
    (void)algo;
    (void)in;
    (void)in_len;
    *out_len = 0;
    return NULL;
}
int ms_qjs_rsa_verify(const char *pem, size_t pem_len, const char *algo, const unsigned char *in,
                      size_t in_len, const unsigned char *sig, size_t sig_len)
{
    (void)pem;
    (void)pem_len;
    (void)algo;
    (void)in;
    (void)in_len;
    (void)sig;
    (void)sig_len;
    return 0;
}
unsigned char *ms_qjs_hmac_sha256(const unsigned char *key, size_t key_len,
                                  const unsigned char *msg, size_t msg_len, size_t *out_len)
{
    (void)key;
    (void)key_len;
    (void)msg;
    (void)msg_len;
    *out_len = 0;
    return NULL;
}
unsigned char *ms_qjs_cipher(const char *name, int enc, const unsigned char *in, size_t in_len,
                             const unsigned char *key, size_t key_len, const unsigned char *iv,
                             size_t iv_len, int padding, size_t *out_len)
{
    (void)name;
    (void)enc;
    (void)in;
    (void)in_len;
    (void)key;
    (void)key_len;
    (void)iv;
    (void)iv_len;
    (void)padding;
    *out_len = 0;
    return NULL;
}

#endif /* MS_QJS_HOST_STUB */

/* XXTEA：两端都编（无 OpenSSL 依赖） */
#define XXTEA_DELTA 0x9e3779b9u
#define MX (((z >> 5 ^ y << 2) + (y >> 3 ^ z << 4)) ^ ((sum ^ y) + (key[(p & 3) ^ e] ^ z)))

static void xxtea_bytes_to_uint(const unsigned char *in, size_t n, unsigned *out, int inc_len)
{
    size_t i;
    for (i = 0; i < n; i++) {
        out[i >> 2] |= (unsigned)in[i] << ((i & 3) << 3);
    }
    if (inc_len) {
        out[n >> 2] = (unsigned)n;
    }
}

static void xxtea_uint_to_bytes(const unsigned *v, size_t n, unsigned char *out)
{
    size_t i;
    for (i = 0; i < n; i++) {
        out[i] = (unsigned char)(v[i >> 2] >> ((i & 3) << 3));
    }
}

static void xxtea_key(const unsigned char *key, size_t key_len, unsigned k[4])
{
    size_t i;
    memset(k, 0, 16);
    for (i = 0; i < key_len && i < 16; i++) {
        ((unsigned char *)k)[i] = key[i];
    }
}

static int xxtea_encrypt_u(unsigned *v, int n, const unsigned *key)
{
    unsigned y, z, sum, e;
    int p, q;
    if (n < 2) {
        return -1;
    }
    z = v[n - 1];
    sum = 0;
    q = 6 + 52 / n;
    while (q-- > 0) {
        sum += XXTEA_DELTA;
        e = (sum >> 2) & 3;
        for (p = 0; p < n - 1; p++) {
            y = v[p + 1];
            z = v[p] += MX;
        }
        y = v[0];
        z = v[n - 1] += MX;
    }
    return 0;
}

static int xxtea_decrypt_u(unsigned *v, int n, const unsigned *key)
{
    unsigned y, z, sum, e;
    int p, q;
    if (n < 2) {
        return -1;
    }
    y = v[0];
    q = 6 + 52 / n;
    sum = (unsigned)q * XXTEA_DELTA;
    while (sum != 0) {
        e = (sum >> 2) & 3;
        for (p = n - 1; p > 0; p--) {
            z = v[p - 1];
            y = v[p] -= MX;
        }
        z = v[n - 1];
        y = v[0] -= MX;
        sum -= XXTEA_DELTA;
    }
    return 0;
}

unsigned char *ms_qjs_xxtea_enc(const unsigned char *in, size_t in_len, const unsigned char *key,
                                size_t key_len, size_t *out_len)
{
    unsigned k[4], *v;
    size_t n;
    unsigned char *out;
    int nn;
    *out_len = 0;
    if (!in || in_len == 0 || !key) {
        return NULL;
    }
    n = ((in_len + 3) >> 2) + 1;
    v = (unsigned *)calloc(n, sizeof(unsigned));
    if (!v) {
        return NULL;
    }
    xxtea_bytes_to_uint(in, in_len, v, 1);
    xxtea_key(key, key_len, k);
    nn = (int)n;
    if (xxtea_encrypt_u(v, nn, k) != 0) {
        free(v);
        return NULL;
    }
    *out_len = n * 4;
    out = (unsigned char *)malloc(*out_len);
    if (!out) {
        free(v);
        *out_len = 0;
        return NULL;
    }
    xxtea_uint_to_bytes(v, *out_len, out);
    free(v);
    return out;
}

unsigned char *ms_qjs_xxtea_dec(const unsigned char *in, size_t in_len, const unsigned char *key,
                                size_t key_len, size_t *out_len)
{
    unsigned k[4], *v;
    size_t n, m;
    unsigned char *out;
    int nn;
    *out_len = 0;
    if (!in || in_len < 8 || (in_len & 3) || !key) {
        return NULL;
    }
    n = in_len >> 2;
    v = (unsigned *)calloc(n, sizeof(unsigned));
    if (!v) {
        return NULL;
    }
    xxtea_bytes_to_uint(in, in_len, v, 0);
    xxtea_key(key, key_len, k);
    nn = (int)n;
    if (xxtea_decrypt_u(v, nn, k) != 0) {
        free(v);
        return NULL;
    }
    m = v[n - 1];
    if (m > in_len || m == 0) {
        free(v);
        return NULL;
    }
    out = (unsigned char *)malloc(m + 1);
    if (!out) {
        free(v);
        return NULL;
    }
    xxtea_uint_to_bytes(v, m, out);
    out[m] = 0;
    free(v);
    *out_len = m;
    return out;
}
