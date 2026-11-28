#ifndef MS_QJS_CRYPTO_H
#define MS_QJS_CRYPTO_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 返回 malloc 缓冲，失败 NULL；调用方 free。 */

unsigned char *ms_qjs_aes_cbc(int enc, const unsigned char *in, size_t in_len,
                              const unsigned char *key, size_t key_len,
                              const unsigned char *iv, size_t iv_len, size_t *out_len);
unsigned char *ms_qjs_aes_ecb_dec(const unsigned char *in, size_t in_len,
                                  const unsigned char *key, size_t key_len, size_t *out_len);
unsigned char *ms_qjs_xxtea_dec(const unsigned char *in, size_t in_len,
                                const unsigned char *key, size_t key_len, size_t *out_len);
unsigned char *ms_qjs_xxtea_enc(const unsigned char *in, size_t in_len,
                                const unsigned char *key, size_t key_len, size_t *out_len);
unsigned char *ms_qjs_rsa_pub_encrypt(const char *pem, size_t pem_len,
                                      const unsigned char *in, size_t in_len, size_t *out_len);
unsigned char *ms_qjs_rsa_sign(const char *pem, size_t pem_len, const char *algo,
                               const unsigned char *in, size_t in_len, size_t *out_len);
int ms_qjs_rsa_verify(const char *pem, size_t pem_len, const char *algo,
                      const unsigned char *in, size_t in_len, const unsigned char *sig,
                      size_t sig_len);
/* 返回 32 字节原始 HMAC；失败 NULL */
unsigned char *ms_qjs_hmac_sha256(const unsigned char *key, size_t key_len,
                                  const unsigned char *msg, size_t msg_len, size_t *out_len);

/* name: aes-128-ecb|cbc|ctr / aes-256-*；enc=1 加密；padding=0 对齐 js-rules opts.padding=false */
unsigned char *ms_qjs_cipher(const char *name, int enc, const unsigned char *in, size_t in_len,
                             const unsigned char *key, size_t key_len, const unsigned char *iv,
                             size_t iv_len, int padding, size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif
