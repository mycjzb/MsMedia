/*
 * Soft guard：TracerPid / maps 注入针 / .text CRC；路径串走 obf。
 * by: Long <mycjzb@gmail.com>
 */
#include "guard.h"
#include "../obf/obf_strings.h"
#include "../obf/obfdec.h"

#include <stdio.h>
#include <string.h>

#if defined(__linux__) || defined(__ANDROID__)
#include <fcntl.h>
#include <unistd.h>
#endif

static const unsigned NEEDLE_HASH[] = {
    0x0F7512ABu, /* frida */
    0x27F39D58u, /* xposed */
    0xA555DDA9u, /* gdbserver */
    0xCC9964C2u, /* substrate */
};
static const unsigned char NEEDLE_LEN[] = {5, 6, 9, 9};
#define NEEDLE_N (sizeof(NEEDLE_HASH) / sizeof(NEEDLE_HASH[0]))
static const unsigned NEEDLE_CRC = 0x57bc75cau;
#define TEXT_CRC_BYTES 64u

static volatile int g_guard_fail = -1;
static volatile unsigned g_taint = 0;
static volatile unsigned g_text_crc = 0;
#if defined(__ANDROID__)
static char g_fail_reason_buf[96];
static const char *g_fail_reason = NULL;
#endif

static unsigned djb2_ci(const char *s, size_t n)
{
    unsigned h = 5381u;
    size_t i;
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c >= 'A' && c <= 'Z') {
            c = (unsigned char)(c - 'A' + 'a');
        }
        h = ((h << 5) + h) + c;
    }
    return h;
}

static unsigned crc32_bytes(const unsigned char *p, size_t n)
{
    unsigned c = 0xffffffffu;
    size_t i;
    for (i = 0; i < n; i++) {
        unsigned b;
        c ^= p[i];
        for (b = 0; b < 8; b++) {
            c = (c >> 1) ^ (0xedb88320u & (unsigned)(-(int)(c & 1u)));
        }
    }
    return c ^ 0xffffffffu;
}

static unsigned crc32_le_u32s(const unsigned *w, size_t n)
{
    unsigned char buf[4 * 8];
    size_t i, o = 0;
    if (n > 8) {
        n = 8;
    }
    for (i = 0; i < n; i++) {
        unsigned v = w[i];
        buf[o++] = (unsigned char)(v & 0xffu);
        buf[o++] = (unsigned char)((v >> 8) & 0xffu);
        buf[o++] = (unsigned char)((v >> 16) & 0xffu);
        buf[o++] = (unsigned char)((v >> 24) & 0xffu);
    }
    return crc32_bytes(buf, o);
}

static int needles_crc_ok(void)
{
    return crc32_le_u32s(NEEDLE_HASH, NEEDLE_N) == NEEDLE_CRC;
}

static unsigned text_crc_now(void)
{
    const unsigned char *p = (const unsigned char *)(void *)&ms_guard_ensure;
    return crc32_bytes(p, TEXT_CRC_BYTES);
}

static unsigned text_crc_cached(void)
{
    unsigned c = (unsigned)g_text_crc;
    if (c == 0) {
        c = text_crc_now();
        if (c == 0) {
            c = 1;
        }
        g_text_crc = c;
    }
    return c;
}

static void note_taint(unsigned bit)
{
    g_taint |= bit;
}

unsigned ms_guard_taint(void)
{
    return (unsigned)g_taint;
}

void ms_guard_mix_key(unsigned char *key, size_t n)
{
    unsigned t, tc;
    size_t i;
    if (!key || n == 0) {
        return;
    }
    t = (unsigned)g_taint;
    if (!needles_crc_ok()) {
        t |= 0x80u;
    }
    if (t == 0) {
        return;
    }
    tc = text_crc_cached();
    for (i = 0; i < n; i++) {
        unsigned char x = (unsigned char)(t + (unsigned)i * 17u);
        x = (unsigned char)(x ^ (unsigned char)((tc >> ((i & 3u) * 8u)) & 0xffu));
        key[i] = (unsigned char)(key[i] ^ x);
    }
}

int ms_guard_traced(void)
{
#if defined(__linux__) || defined(__ANDROID__)
    FILE *f;
    char line[256];
    char path[32], tag[16];
    size_t tlen;
    MS_OBF_DEC(PROC_SELF_STATUS, path);
    f = fopen(path, "r");
    ms_obf_wipe(path, sizeof(path));
    if (!f) {
        return 0;
    }
    MS_OBF_DEC(TRACER_PID, tag);
    tlen = strlen(tag);
    while (fgets(line, sizeof(line), f)) {
        if (tlen > 0 && strncmp(line, tag, tlen) == 0) {
            long pid = 0;
            if (sscanf(line + (int)tlen, "%ld", &pid) == 1 && pid > 0) {
                ms_obf_wipe(tag, sizeof(tag));
                fclose(f);
                return 1;
            }
            break;
        }
    }
    ms_obf_wipe(tag, sizeof(tag));
    fclose(f);
#endif
    return 0;
}

void ms_guard_probe_tracer(void)
{
    if (ms_guard_traced()) {
        note_taint(0x01u);
    }
}

static int maps_has_hashed_needle(void)
{
#if defined(__linux__) || defined(__ANDROID__)
    FILE *f;
    char line[512];
    char path[32];
    MS_OBF_DEC(PROC_SELF_MAPS, path);
    f = fopen(path, "r");
    ms_obf_wipe(path, sizeof(path));
    if (!f) {
        return 0;
    }
    while (fgets(line, sizeof(line), f)) {
        size_t L = strlen(line);
        size_t ni;
        for (ni = 0; ni < NEEDLE_N; ni++) {
            size_t nlen = NEEDLE_LEN[ni];
            size_t i;
            if (L < nlen) {
                continue;
            }
            for (i = 0; i + nlen <= L; i++) {
                if (djb2_ci(line + i, nlen) == NEEDLE_HASH[ni]) {
                    fclose(f);
                    return 1;
                }
            }
        }
    }
    fclose(f);
#endif
    return 0;
}

int ms_guard_hostile(void)
{
    if (!needles_crc_ok()) {
        return 1;
    }
    return maps_has_hashed_needle();
}

void ms_guard_probe_maps(void)
{
    if (ms_guard_hostile()) {
        note_taint(0x02u);
    }
}

void ms_guard_probe_integrity(void)
{
    unsigned now;
    if (!needles_crc_ok()) {
        note_taint(0x04u);
    }
    now = text_crc_now();
    if (g_text_crc == 0) {
        g_text_crc = now ? now : 1;
    } else if (now != (unsigned)g_text_crc) {
        note_taint(0x10u);
    }
}

int ms_guard_ok(void)
{
#if !defined(MS_GUARD_ENABLED)
    return 1;
#else
    if (g_guard_fail == 0) {
        return 1;
    }
    if (g_guard_fail == 1) {
        return 0;
    }
    if (!needles_crc_ok() || ms_guard_traced() || ms_guard_hostile()) {
        g_guard_fail = 1;
        return 0;
    }
    g_guard_fail = 0;
    return 1;
#endif
}

#if defined(__ANDROID__)
static char g_reason_dec[24];

static const char *obf_reason(const unsigned char *obf, size_t n, int kid)
{
    ms_obf_decode_k(obf, n, g_reason_dec, sizeof(g_reason_dec), kid);
    return g_reason_dec;
}

#define GR_FAIL(NAME) obf_reason(MS_OBF_##NAME, MS_OBF_LEN_##NAME, MS_OBF_KID_##NAME)

void ms_guard_force_fail(const char *reason)
{
#if defined(MS_GUARD_ENABLED)
    if (reason && reason[0]) {
        size_t i;
        for (i = 0; i + 1 < sizeof(g_fail_reason_buf) && reason[i]; i++) {
            g_fail_reason_buf[i] = reason[i];
        }
        g_fail_reason_buf[i] = '\0';
        g_fail_reason = g_fail_reason_buf;
    } else {
        g_fail_reason = GR_FAIL(GUARD_FORCED);
    }
    g_guard_fail = 1;
    note_taint(0x08u);
#else
    (void)reason;
#endif
}

const char *ms_guard_block_reason(void)
{
    if (g_fail_reason) {
        return g_fail_reason;
    }
    if (!needles_crc_ok()) {
        return GR_FAIL(GUARD_INTEGRITY);
    }
    if (ms_guard_traced()) {
        return GR_FAIL(GUARD_TRACER);
    }
    if (maps_has_hashed_needle()) {
        return GR_FAIL(GUARD_MAPS);
    }
    if (g_taint & 0x10u) {
        return GR_FAIL(GUARD_TEXT);
    }
    return GR_FAIL(GUARD_UNKNOWN);
}
#endif

void ms_guard_ensure(void)
{
#if defined(MS_GUARD_ENABLED)
    ms_guard_probe_integrity();
    (void)ms_guard_ok();
#endif
}
