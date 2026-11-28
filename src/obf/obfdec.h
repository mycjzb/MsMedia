#ifndef MS_OBFDEC_H
#define MS_OBFDEC_H

#include <stddef.h>

/* Rolling XOR — 算法同超级直播；密钥为本 SDK 独有（见 obfdec.c）。 */
void ms_obf_decode_k(const unsigned char *obf, size_t n, char *out, size_t cap, int kid);
void ms_obf_decode(const unsigned char *obf, size_t n, char *out, size_t cap);
void ms_obf_wipe(void *p, size_t n);

#define MS_OBF_DEC(name, buf) \
    ms_obf_decode_k(MS_OBF_##name, MS_OBF_LEN_##name, (buf), sizeof(buf), MS_OBF_KID_##name)

#endif /* MS_OBFDEC_H */
