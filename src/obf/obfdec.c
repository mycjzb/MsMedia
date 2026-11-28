/*
 * Rolling XOR 解码（形同超级直播 obfdec；密钥勿复用直播包）。
 * by: Long <mycjzb@gmail.com>
 */
#include "obfdec.h"

#include <string.h>

/* MsMedia SDK 专用；与 bs-player 双 bank 不同。 */
static const unsigned char MS_OBF_KEYS[2][8] = {
    {0x3E, 0x91, 0xC7, 0x2A, 0xF5, 0x68, 0x0D, 0xB4},
    {0x82, 0x19, 0xDE, 0x47, 0xA3, 0x5C, 0xE6, 0x70},
};

void ms_obf_decode_k(const unsigned char *obf, size_t n, char *out, size_t cap, int kid)
{
    const unsigned char *key;
    size_t i;
    if (!out || cap == 0) {
        return;
    }
    if (!obf || n == 0) {
        out[0] = '\0';
        return;
    }
    if (n >= cap) {
        n = cap - 1;
    }
    key = MS_OBF_KEYS[kid & 1];
    for (i = 0; i < n; i++) {
        out[i] = (char)(obf[i] ^ key[i % 8]);
    }
    out[n] = '\0';
}

void ms_obf_decode(const unsigned char *obf, size_t n, char *out, size_t cap)
{
    ms_obf_decode_k(obf, n, out, cap, 0);
}

void ms_obf_wipe(void *p, size_t n)
{
    volatile unsigned char *v;
    size_t i;
    if (!p || n == 0) {
        return;
    }
    v = (volatile unsigned char *)p;
    for (i = 0; i < n; i++) {
        v[i] = 0;
    }
}
