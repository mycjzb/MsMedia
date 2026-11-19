/*
 * 极小 SHA-256，仅供 tests/ms_ts_inspect 等开发工具使用，不链入产品库。
 * 实现按 FIPS 180-4，无外部依赖。
 * by: Long <mycjzb@gmail.com>
 */
#ifndef MSMEDIA_TEST_SHA256_H
#define MSMEDIA_TEST_SHA256_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef struct ms_sha256 {
    uint32_t h[8];
    uint64_t nbits;
    uint8_t buf[64];
    size_t len;
} ms_sha256;

static uint32_t ms_sha256_rotr(uint32_t x, int n)
{
    return (x >> n) | (x << (32 - n));
}

static void ms_sha256_init(ms_sha256 *s)
{
    static const uint32_t iv[8] = {
        0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
        0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u
    };
    memset(s, 0, sizeof(*s));
    memcpy(s->h, iv, sizeof(iv));
}

static void ms_sha256_block(ms_sha256 *s, const uint8_t blk[64])
{
    static const uint32_t K[64] = {
        0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u,
        0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
        0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u,
        0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
        0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
        0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
        0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
        0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
        0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au,
        0x5b9cca4fu, 0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
        0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
    };
    uint32_t w[64], a, b, c, d, e, f, g, h;
    int i;

    for (i = 0; i < 16; i++) {
        w[i] = ((uint32_t)blk[i * 4] << 24) | ((uint32_t)blk[i * 4 + 1] << 16) |
               ((uint32_t)blk[i * 4 + 2] << 8) | (uint32_t)blk[i * 4 + 3];
    }
    for (i = 16; i < 64; i++) {
        uint32_t s0 = ms_sha256_rotr(w[i - 15], 7) ^ ms_sha256_rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ms_sha256_rotr(w[i - 2], 17) ^ ms_sha256_rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = s->h[0]; b = s->h[1]; c = s->h[2]; d = s->h[3];
    e = s->h[4]; f = s->h[5]; g = s->h[6]; h = s->h[7];
    for (i = 0; i < 64; i++) {
        uint32_t S1 = ms_sha256_rotr(e, 6) ^ ms_sha256_rotr(e, 11) ^ ms_sha256_rotr(e, 25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t t1 = h + S1 + ch + K[i] + w[i];
        uint32_t S0 = ms_sha256_rotr(a, 2) ^ ms_sha256_rotr(a, 13) ^ ms_sha256_rotr(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + maj;
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d;
    s->h[4] += e; s->h[5] += f; s->h[6] += g; s->h[7] += h;
}

static void ms_sha256_update(ms_sha256 *s, const uint8_t *data, size_t n)
{
    while (n > 0) {
        size_t room = 64 - s->len;
        size_t take = (n < room) ? n : room;
        memcpy(s->buf + s->len, data, take);
        s->len += take;
        data += take;
        n -= take;
        if (s->len == 64) {
            ms_sha256_block(s, s->buf);
            s->nbits += 512;
            s->len = 0;
        }
    }
}

static void ms_sha256_final(ms_sha256 *s, uint8_t out[32])
{
    int i;
    s->nbits += (uint64_t)s->len * 8u;
    s->buf[s->len++] = 0x80;
    if (s->len > 56) {
        while (s->len < 64) {
            s->buf[s->len++] = 0;
        }
        ms_sha256_block(s, s->buf);
        s->len = 0;
    }
    while (s->len < 56) {
        s->buf[s->len++] = 0;
    }
    for (i = 7; i >= 0; i--) {
        s->buf[s->len++] = (uint8_t)((s->nbits >> (i * 8)) & 0xFFu);
    }
    ms_sha256_block(s, s->buf);
    for (i = 0; i < 8; i++) {
        out[i * 4] = (uint8_t)(s->h[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(s->h[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(s->h[i] >> 8);
        out[i * 4 + 3] = (uint8_t)(s->h[i]);
    }
}

static void ms_sha256_hex(const uint8_t *data, size_t n, char out[65])
{
    static const char *hex = "0123456789abcdef";
    uint8_t dig[32];
    ms_sha256 s;
    int i;
    ms_sha256_init(&s);
    if (data != NULL && n > 0) {
        ms_sha256_update(&s, data, n);
    }
    ms_sha256_final(&s, dig);
    for (i = 0; i < 32; i++) {
        out[i * 2] = hex[(dig[i] >> 4) & 0xF];
        out[i * 2 + 1] = hex[dig[i] & 0xF];
    }
    out[64] = 0;
}

#endif
