/*
 * TS 坏包过滤，说明见 ms_tsguard.h。
 * by: Long <mycjzb@gmail.com>
 */
#include "ms_tsguard.h"
#include "../core/ms_crc32.h"
#include <stdlib.h>
#include <string.h>

#define G_MAX_PMT 16
#define G_OUT_BATCH (188 * 64)
#define G_AAC_MAX   4
#define G_AAC_PKTS  64

/* AAC PID 的 PES 暂存：整包校验 ADTS 链，坏则整 PES 丢弃（核心 ADTS 组帧对坏头严格报错） */
typedef struct g_aac {
    uint16_t pid;
    int active;
    uint8_t pk[G_AAC_PKTS][188];
    int npk;
    size_t need;            /* PES 总长（0=未知） */
    size_t got;             /* 已收 PES 字节 */
} g_aac;

struct ms_tsguard {
    ms_tsguard_write_fn w;
    void *ctx;
    uint8_t in[204 * 8 + 4096];
    size_t n;
    int psz;                 /* 0 未同步 */
    uint16_t pmt[G_MAX_PMT];
    int npmt;
    uint8_t es[8192 / 8];    /* ES PID 位图 */
    uint8_t out[G_OUT_BATCH];
    size_t on;
    uint64_t dropped;
    int err;
    g_aac aac[G_AAC_MAX];
    int naac;
};

ms_tsguard *ms_tsguard_create(ms_tsguard_write_fn w, void *ctx)
{
    ms_tsguard *g = (ms_tsguard *)calloc(1, sizeof(*g));
    if (!g) return NULL;
    g->w = w;
    g->ctx = ctx;
    return g;
}

static int flush_out(ms_tsguard *g)
{
    if (g->on && !g->err) {
        if (g->w(g->ctx, g->out, g->on) != 0) g->err = -1;
    }
    g->on = 0;
    return g->err;
}

static int is_pmt(ms_tsguard *g, uint16_t pid)
{
    int i;
    for (i = 0; i < g->npmt; i++) if (g->pmt[i] == pid) return 1;
    return 0;
}

/* PUSI 的 PSI 包：首节在本包内完整时校验 CRC（坏则丢包）；其后的节逐个校验，
 * 第一个坏节起整段改写为 0xFF 填充（翻转的填充字节不会被误当成新节）。
 * 返回 1 保留（sec 指向首个完整好节，跨包时为 NULL），0 丢弃。 */
static int psi_fix(uint8_t *pl, size_t n, const uint8_t **sec, size_t *slen)
{
    size_t ptr, sl, off;
    *sec = NULL;
    if (n < 1) return 0;
    ptr = pl[0];
    if (1 + ptr + 3 > n) return ptr < n ? 1 : 0;
    off = 1 + ptr;
    if (pl[off] == 0xFF) return 1;
    sl = (size_t)(((pl[off + 1] & 0x0F) << 8) | pl[off + 2]);
    if (sl > 1021) return 0;
    if (off + 3 + sl > n) return 1; /* 跨包，交核心处理 */
    if (sl < 4 || ms_crc32_mpeg2(pl + off, 3 + sl) != 0) return 0;
    *sec = pl + off;
    *slen = 3 + sl;
    off += 3 + sl;
    while (off < n && pl[off] != 0xFF) {
        size_t l2;
        if (off + 3 > n) break;
        l2 = (size_t)(((pl[off + 1] & 0x0F) << 8) | pl[off + 2]);
        if (l2 < 4 || l2 > 1021 || off + 3 + l2 > n || ms_crc32_mpeg2(pl + off, 3 + l2) != 0) break;
        off += 3 + l2;
    }
    if (off < n) memset(pl + off, 0xFF, n - off);
    return 1;
}

static int check_pkt(ms_tsguard *g, uint8_t *p)
{
    unsigned pusi = (p[1] >> 6) & 1, adapt = (p[3] >> 4) & 3;
    uint16_t pid = (uint16_t)(((p[1] & 0x1F) << 8) | p[2]);
    size_t off = 4;
    if (adapt == 0) return 0;
    if (adapt & 2) {
        size_t al = p[4];
        if (adapt == 2 && al != 183) return 0;
        if (adapt == 3 && al > 182) return 0;
        off = 5 + al;
    }
    if (pid == 0x1FFF || !(adapt & 1) || !pusi || (p[1] & 0x80)) return 1;
    if (pid == 0 || is_pmt(g, pid)) {
        const uint8_t *sec;
        size_t sl = 0;
        if (!psi_fix(p + off, 188 - off, &sec, &sl)) return 0;
        if (sec && pid == 0 && sec[0] == 0x00) {
            size_t i;
            g->npmt = 0;
            for (i = 8; i + 4 <= sl - 4 && g->npmt < G_MAX_PMT; i += 4) {
                uint16_t prog = (uint16_t)((sec[i] << 8) | sec[i + 1]);
                if (prog) g->pmt[g->npmt++] = (uint16_t)(((sec[i + 2] & 0x1F) << 8) | sec[i + 3]);
            }
        } else if (sec && sec[0] == 0x02 && sl >= 16) {
            size_t pil = (size_t)(((sec[10] & 0x0F) << 8) | sec[11]), k = 12 + pil;
            while (k + 5 <= sl - 4) {
                uint16_t ep = (uint16_t)(((sec[k + 1] & 0x1F) << 8) | sec[k + 2]);
                size_t il = (size_t)(((sec[k + 3] & 0x0F) << 8) | sec[k + 4]);
                g->es[ep >> 3] |= (uint8_t)(1u << (ep & 7));
                if (sec[k] == 0x0F) {
                    int j, have = 0;
                    for (j = 0; j < g->naac; j++) if (g->aac[j].pid == ep) have = 1;
                    if (!have && g->naac < G_AAC_MAX) { memset(&g->aac[g->naac], 0, sizeof(g->aac[0])); g->aac[g->naac++].pid = ep; }
                }
                k += 5 + il;
            }
        }
        return 1;
    }
    if (g->es[pid >> 3] & (1u << (pid & 7))) {
        const uint8_t *q = p + off;
        size_t n = 188 - off;
        if (n < 9) return n >= 3 && q[0] == 0 && q[1] == 0 && q[2] == 1;
        if (q[0] != 0 || q[1] != 0 || q[2] != 1 || q[3] < 0xBC) return 0;
        if (q[3] != 0xBE && q[3] != 0xBF && (q[6] & 0xC0) == 0x80) {
            size_t pl = (size_t)((q[4] << 8) | q[5]);
            if (pl && pl < 3u + q[8]) return 0;
            if (9u + q[8] > n && q[8] > 64) return 0;
        }
    }
    return 1;
}

static int put_out(ms_tsguard *g, const uint8_t *p)
{
    if (g->on + 188 > sizeof(g->out) && flush_out(g)) return g->err;
    memcpy(g->out + g->on, p, 188);
    g->on += 188;
    return 0;
}

static size_t pkt_payload(const uint8_t *p, const uint8_t **pl)
{
    size_t off = 4;
    if (p[3] & 0x20) off = 5u + p[4];
    if (!(p[3] & 0x10) || off >= 188) { *pl = p + 188; return 0; }
    *pl = p + off;
    return 188 - off;
}

/* 校验暂存 PES 的 ADTS 链（规则与核心 ms_adts 一致：同步、长度≥头长、单 raw block、采样率索引≤12、声道≠0） */
static int aac_pes_ok(g_aac *a)
{
    uint8_t buf[G_AAC_PKTS * 184];
    size_t n = 0, off, hl;
    int i;
    for (i = 0; i < a->npk; i++) {
        const uint8_t *pl;
        size_t k = pkt_payload(a->pk[i], &pl);
        memcpy(buf + n, pl, k);
        n += k;
    }
    if (n < 9 || buf[0] != 0 || buf[1] != 0 || buf[2] != 1) return 0;
    hl = 9u + buf[8];
    if (hl > n) return 0;
    if (a->need && n < a->need) return 0;         /* 缺包：交给核心按缺口处理也会丢，这里直接丢 */
    if (a->need && n > a->need) n = a->need;
    off = hl;
    while (off < n) {
        const uint8_t *f = buf + off;
        size_t flen, fh;
        if (n - off < 7) return 0;
        if (f[0] != 0xFF || (f[1] & 0xF6) != 0xF0) return 0;
        fh = (f[1] & 1) ? 7 : 9;
        flen = ((size_t)(f[3] & 3) << 11) | ((size_t)f[4] << 3) | (f[5] >> 5);
        if (flen < fh || (f[6] & 3) != 0 || ((f[2] >> 2) & 0x0F) > 12) return 0;
        if ((((f[2] & 1) << 2) | (f[3] >> 6)) == 0) return 0;
        if (off + flen > n) return 0;
        off += flen;
    }
    return 1;
}

static int aac_finish(ms_tsguard *g, g_aac *a)
{
    int i, ok;
    if (!a->active) return 0;
    ok = aac_pes_ok(a);
    for (i = 0; i < a->npk; i++) {
        if (ok) { if (put_out(g, a->pk[i])) return g->err; }
        else g->dropped++;
    }
    a->active = 0;
    a->npk = 0;
    a->got = 0;
    a->need = 0;
    return 0;
}

static int emit(ms_tsguard *g, const uint8_t *p)
{
    uint8_t tmp[188];
    uint16_t pid;
    int i;
    memcpy(tmp, p, 188);
    if (!check_pkt(g, tmp)) { g->dropped++; return 0; }
    pid = (uint16_t)(((tmp[1] & 0x1F) << 8) | tmp[2]);
    for (i = 0; i < g->naac; i++) {
        g_aac *a = &g->aac[i];
        const uint8_t *pl;
        size_t k;
        if (a->pid != pid) continue;
        if (tmp[1] & 0x80) { /* TEI：交核心处理（丢弃并发不连续） */
            if (aac_finish(g, a)) return g->err;
            return put_out(g, tmp);
        }
        k = pkt_payload(tmp, &pl);
        if (tmp[1] & 0x40) {
            if (aac_finish(g, a)) return g->err;
            a->active = 1;
            a->need = (k >= 6) ? (size_t)((pl[4] << 8) | pl[5]) : 0;
            if (a->need) a->need += 6;
        } else if (!a->active) {
            g->dropped++; /* 没有起始的续包：丢 */
            return 0;
        }
        if (a->npk >= G_AAC_PKTS) { a->npk = 0; a->active = 0; g->dropped++; return 0; }
        memcpy(a->pk[a->npk++], tmp, 188);
        a->got += k;
        if (a->need && a->got >= a->need) return aac_finish(g, a);
        return 0;
    }
    return put_out(g, tmp);
}

static int try_sync(const uint8_t *d, size_t n, size_t *off, int *psz)
{
    static const int sz[3] = { 188, 192, 204 };
    size_t i;
    int k;
    for (i = 0; i < n; i++) {
        for (k = 0; k < 3; k++) {
            size_t s = (size_t)sz[k], b = sz[k] == 192 ? 4 : 0;
            if (i + b + 2 * s + 1 > n) continue;
            if (d[i + b] == 0x47 && d[i + b + s] == 0x47 && d[i + b + 2 * s] == 0x47) {
                *off = i;
                *psz = sz[k];
                return 1;
            }
        }
    }
    return 0;
}

int ms_tsguard_feed(ms_tsguard *g, const uint8_t *d, size_t n)
{
    while (n > 0 || g->n > 0) {
        size_t take = sizeof(g->in) - g->n, pos = 0;
        if (take > n) take = n;
        memcpy(g->in + g->n, d, take);
        g->n += take;
        d += take;
        n -= take;
        for (;;) {
            size_t b, avail = g->n - pos;
            if (!g->psz) {
                size_t off;
                if (!try_sync(g->in + pos, avail, &off, &g->psz)) {
                    /* 保留尾部可能的同步起点 */
                    if (avail > 204 * 3) { g->dropped++; pos = g->n - 204 * 3; }
                    break;
                }
                pos += off;
                continue;
            }
            b = g->psz == 192 ? 4 : 0;
            if (avail < (size_t)g->psz) break;
            if (g->in[pos + b] != 0x47) { g->psz = 0; g->dropped++; continue; }
            if (emit(g, g->in + pos + b)) return g->err;
            pos += (size_t)g->psz;
        }
        memmove(g->in, g->in + pos, g->n - pos);
        g->n -= pos;
        if (take == 0) break;
    }
    return flush_out(g);
}

int ms_tsguard_flush(ms_tsguard *g)
{
    int i;
    for (i = 0; i < g->naac; i++) if (aac_finish(g, &g->aac[i])) return g->err;
    return flush_out(g);
}

void ms_tsguard_destroy(ms_tsguard *g)
{
    free(g);
}

uint64_t ms_tsguard_dropped(const ms_tsguard *g)
{
    return g ? g->dropped : 0;
}
