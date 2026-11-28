/* 媒体分片流式解密（内部短名；对外协议 query 仍由 Java/JS 拼）。 */
#ifndef MS_YSP_TEA_H
#define MS_YSP_TEA_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct m1 m1;

enum {
    MS_M1_MODE_0 = 0,
    MS_M1_MODE_1 = 1, /* 原 ysp-tea=1 */
    MS_M1_MODE_2 = 2  /* 原 ysp-tea=2 */
};

m1 *ms_m1_create(int mode);
void ms_m1_destroy(m1 *s);
void ms_m1_set_strides(m1 *s, int idr_stride, int p_stride);
int ms_m1_feed(m1 *s, const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len);
int ms_m1_flush(m1 *s, uint8_t **out, size_t *out_len);
int ms_m1_seg(int mode, const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif
