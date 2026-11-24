/*
 * 轻量 MKV / MPEG-PS → MPEG-TS 流式转封装，说明见 ms_mkvps.h。
 * by: Long <mycjzb@gmail.com>
 */
#include "ms_mkvps.h"
#include "../core/ms_crc32.h"
#include "ms_flvw.h"

#include <stdlib.h>
#include <string.h>

#define CV_MAX_ES     8
#define CV_MAX_ELEM   (16u * 1024u * 1024u)
#define CV_MAX_PEND   (2u * 1024u * 1024u)
#define CV_PMT_PID    0x1000
#define CV_ES_PID0    0x100

typedef struct cv_es {
    int used;
    int pid;
    uint8_t st;          /* TS stream_type；0=不支持（跳过） */
    uint8_t sid;         /* PES stream_id */
    uint8_t cc;
    int video;
    /* PS：键（stream_id 或 0xBD 子流 id<<8|0xBD） */
    int key;
    int strip4;          /* DVD 私有流子流头 4 字节 */
    /* MKV */
    uint64_t trackno;
    int mkv_type;        /* 1 video 2 audio */
    char codec[40];
    uint8_t *priv;
    size_t priv_n;
    uint8_t *strip_pfx;  /* ContentCompression 头剥离（算法 3）前缀 */
    size_t strip_pfx_n;
    int nal_len;         /* avcC/hvcC 长度字段字节数 */
    uint8_t *ps;         /* Annex-B 参数集（关键帧前补） */
    size_t ps_n;
    uint8_t adts[7];     /* AAC：ADTS 模板 */
    int aac;
    int flv_cfg_sent;
} cv_es;

struct ms_tsconv {
    int kind;
    ms_tsconv_write_fn w;
    void *ctx;
    int err;
    cv_es es[CV_MAX_ES];
    int nes;
    int pmt_sent;
    int pcr_idx;
    int pes_since_psi;
    int pes_out;
    int skipped;
    uint8_t pat_cc, pmt_cc;
    /* 输入缓冲 */
    uint8_t *buf;
    size_t n, cap, pos;
    uint64_t skip;       /* MKV：流式跳过剩余字节 */
    /* PS：PMT 前缓存的 PES */
    uint8_t *pend;
    size_t pend_n, pend_cap;
    int have_psm;
    uint8_t psm_type[256];
    int seen_v, seen_a;
    /* MKV */
    uint64_t tc_scale;
    uint64_t cluster_tc;
    cv_es *cur_trk;
    int in_tracks_done;
    int cur_algo;        /* ContentCompAlgo */
    uint8_t *tmp;
    size_t tmp_cap;
    /* MKV 含 HEVC 时改输出 FLV（核心 TS 仅 H.264/AAC 组帧，FLV 已支持 HEVC/AAC） */
    int flv;
    ms_flvw fw;
};

/* ---------------- TS 写出 ---------------- */
static int out(ms_tsconv *c, const uint8_t *d, size_t n)
{
    if (c->err) return c->err;
    if (c->w(c->ctx, d, n) != 0) c->err = -1;
    return c->err;
}

static int write_psi(ms_tsconv *c)
{
    uint8_t p[188], *s;
    int i, k;
    uint32_t crc;
    /* PAT */
    memset(p, 0xFF, 188);
    p[0] = 0x47; p[1] = 0x40; p[2] = 0x00; p[3] = (uint8_t)(0x10 | (c->pat_cc++ & 0x0F));
    p[4] = 0;
    s = p + 5;
    s[0] = 0x00; s[1] = 0xB0; s[2] = 13; s[3] = 0; s[4] = 1; s[5] = 0xC1; s[6] = 0; s[7] = 0;
    s[8] = 0; s[9] = 1; s[10] = (uint8_t)(0xE0 | (CV_PMT_PID >> 8)); s[11] = (uint8_t)CV_PMT_PID;
    crc = ms_crc32_mpeg2(s, 12);
    s[12] = (uint8_t)(crc >> 24); s[13] = (uint8_t)(crc >> 16); s[14] = (uint8_t)(crc >> 8); s[15] = (uint8_t)crc;
    if (out(c, p, 188)) return c->err;
    /* PMT */
    memset(p, 0xFF, 188);
    p[0] = 0x47; p[1] = (uint8_t)(0x40 | (CV_PMT_PID >> 8)); p[2] = (uint8_t)CV_PMT_PID; p[3] = (uint8_t)(0x10 | (c->pmt_cc++ & 0x0F));
    p[4] = 0;
    s = p + 5;
    k = 12;
    {
        int pcr = c->pcr_idx >= 0 ? c->es[c->pcr_idx].pid : 0x1FFF;
        s[0] = 0x02; s[3] = 0; s[4] = 1; s[5] = 0xC1; s[6] = 0; s[7] = 0;
        s[8] = (uint8_t)(0xE0 | (pcr >> 8)); s[9] = (uint8_t)pcr; s[10] = 0xF0; s[11] = 0;
    }
    for (i = 0; i < c->nes; i++) {
        cv_es *e = &c->es[i];
        if (!e->used || !e->st) continue;
        if (k + 5 + 4 > 183) break;
        s[k] = e->st; s[k + 1] = (uint8_t)(0xE0 | (e->pid >> 8)); s[k + 2] = (uint8_t)e->pid; s[k + 3] = 0xF0; s[k + 4] = 0;
        k += 5;
    }
    s[1] = (uint8_t)(0xB0 | ((k + 4 - 3) >> 8)); s[2] = (uint8_t)(k + 4 - 3);
    crc = ms_crc32_mpeg2(s, (size_t)k);
    s[k] = (uint8_t)(crc >> 24); s[k + 1] = (uint8_t)(crc >> 16); s[k + 2] = (uint8_t)(crc >> 8); s[k + 3] = (uint8_t)crc;
    c->pes_since_psi = 0;
    return out(c, p, 188);
}

/* 读 PES 头里的 PTS/DTS（33 位），无则返回 -1 */
static int64_t pes_ts(const uint8_t *pes, size_t n)
{
    const uint8_t *t;
    if (n < 14 || (pes[6] & 0xC0) != 0x80 || !(pes[7] & 0x80)) return -1;
    t = pes + ((pes[7] & 0xC0) == 0xC0 ? 14 : 9);
    if ((size_t)(t - pes) + 5 > n) return -1;
    return ((int64_t)(t[0] & 0x0E) << 29) | ((int64_t)t[1] << 22) | ((int64_t)(t[2] & 0xFE) << 14) | ((int64_t)t[3] << 7) | (t[4] >> 1);
}

static int write_pes_ts(ms_tsconv *c, cv_es *e, const uint8_t *pes, size_t n)
{
    size_t off = 0;
    int first = 1;
    int64_t pcr = -1;
    if (!c->pmt_sent || c->pes_since_psi >= 64) {
        if (write_psi(c)) return c->err;
        c->pmt_sent = 1;
    }
    c->pes_since_psi++;
    c->pes_out++;
    if (c->pcr_idx >= 0 && e == &c->es[c->pcr_idx]) pcr = pes_ts(pes, n);
    while (off < n) {
        uint8_t p[188];
        size_t hl = 4, room, take;
        int af = 0;
        p[0] = 0x47;
        p[1] = (uint8_t)((first ? 0x40 : 0) | (e->pid >> 8));
        p[2] = (uint8_t)e->pid;
        if (first && pcr >= 0) af = 8; /* 长度字节 + 标志 + 6 字节 PCR */
        room = 188 - hl - (af ? (size_t)af : 0);
        take = n - off < room ? n - off : room;
        if (!af && take < room) af = 1; /* 需要填充 */
        if (af) {
            size_t stuff = room - take;
            int alen;
            if (af == 1) { room = 188 - hl - 1; take = n - off < room ? n - off : room; stuff = room - take; alen = (int)stuff; }
            else alen = 7 + (int)stuff;
            p[3] = (uint8_t)(0x30 | (e->cc++ & 0x0F));
            p[4] = (uint8_t)alen;
            if (alen > 0) {
                size_t q = 5;
                p[q++] = (uint8_t)(af == 8 ? 0x10 : 0x00);
                if (af == 8) {
                    uint64_t b = (uint64_t)pcr;
                    p[q++] = (uint8_t)(b >> 25); p[q++] = (uint8_t)(b >> 17); p[q++] = (uint8_t)(b >> 9);
                    p[q++] = (uint8_t)(b >> 1); p[q++] = (uint8_t)(((b & 1) << 7) | 0x7E); p[q++] = 0;
                }
                while (q < 5 + (size_t)alen) p[q++] = 0xFF;
            }
            memcpy(p + 5 + alen, pes + off, take);
        } else {
            p[3] = (uint8_t)(0x10 | (e->cc++ & 0x0F));
            memcpy(p + 4, pes + off, take);
        }
        off += take;
        first = 0;
        if (out(c, p, 188)) return c->err;
    }
    return 0;
}

static int tmp_reserve(ms_tsconv *c, size_t need)
{
    if (need <= c->tmp_cap) return 0;
    {
        size_t nc = c->tmp_cap ? c->tmp_cap : 65536;
        uint8_t *nb;
        while (nc < need) nc *= 2;
        nb = (uint8_t *)realloc(c->tmp, nc);
        if (!nb) return -2;
        c->tmp = nb;
        c->tmp_cap = nc;
    }
    return 0;
}

static void put_ts(uint8_t *p, int marker, uint64_t v)
{
    v &= 0x1FFFFFFFFull;
    p[0] = (uint8_t)((marker << 4) | ((v >> 29) & 0x0E) | 1);
    p[1] = (uint8_t)(v >> 22); p[2] = (uint8_t)(((v >> 14) & 0xFE) | 1);
    p[3] = (uint8_t)(v >> 7); p[4] = (uint8_t)(((v << 1) & 0xFE) | 1);
}

/* 组 PES（仅 PTS）：header + body 写入 tmp，再打包成 TS */
static int emit_frame(ms_tsconv *c, cv_es *e, uint64_t pts90, const uint8_t *pre, size_t pn,
                      const uint8_t *d, size_t n)
{
    size_t tot = 14 + pn + n, pl;
    int r;
    if ((r = tmp_reserve(c, tot)) != 0) return r;
    c->tmp[0] = 0; c->tmp[1] = 0; c->tmp[2] = 1; c->tmp[3] = e->sid;
    pl = tot - 6;
    if (pl > 65535) pl = e->video ? 0 : 65535;
    if (pl == 65535 && !e->video) {
        /* 音频帧不应超过 64K，超过即截断丢弃（坏数据容错） */
        return 0;
    }
    c->tmp[4] = (uint8_t)(pl >> 8); c->tmp[5] = (uint8_t)pl;
    c->tmp[6] = 0x84; c->tmp[7] = 0x80; c->tmp[8] = 5;
    put_ts(c->tmp + 9, 2, pts90);
    if (pn) memcpy(c->tmp + 14, pre, pn);
    memcpy(c->tmp + 14 + pn, d, n);
    return write_pes_ts(c, e, c->tmp, tot);
}

/* ---------------- MPEG-PS ---------------- */
static cv_es *es_by_key(ms_tsconv *c, int key)
{
    int i;
    for (i = 0; i < c->nes; i++) if (c->es[i].used && c->es[i].key == key) return &c->es[i];
    return NULL;
}

static cv_es *es_add(ms_tsconv *c)
{
    cv_es *e;
    if (c->nes >= CV_MAX_ES) return NULL;
    e = &c->es[c->nes];
    memset(e, 0, sizeof(*e));
    e->used = 1;
    e->pid = CV_ES_PID0 + c->nes;
    c->nes++;
    return e;
}

static const uint8_t *pes_payload(const uint8_t *p, size_t n, size_t *pl)
{
    size_t h;
    if (n < 9 || (p[6] & 0xC0) != 0x80) return NULL;
    h = 9u + p[8];
    if (h > n) return NULL;
    *pl = n - h;
    return p + h;
}

static uint8_t sniff_video(const uint8_t *d, size_t n)
{
    size_t i;
    for (i = 0; i + 4 < n && i < 256; i++) {
        if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1) {
            uint8_t b = d[i + 3], b2 = d[i + 4];
            int ht = (b >> 1) & 0x3F;
            if (b == 0xB3 || b == 0xB8 || b == 0x00) return 0x02;
            if (b == 0xB0 || b == 0xB5 || (b >= 0x20 && b <= 0x2F && b != 0x26 && b != 0x28)) {
                if (b == 0xB0 || b == 0xB5) return 0x10;
            }
            if (!(b & 0x80) && b2 == 1 && (ht == 32 || ht == 33 || ht == 34 || ht == 35 || ht == 39)) return 0x24;
            if (!(b & 0x80)) {
                int t = b & 0x1F;
                if (t == 7 || t == 9 || t == 5 || t == 1 || t == 6 || t == 8) return 0x1B;
            }
        }
    }
    return 0;
}

static uint8_t sniff_audio(const uint8_t *d, size_t n)
{
    if (n >= 2 && d[0] == 0xFF && (d[1] & 0xF6) == 0xF0) return 0x0F;  /* ADTS */
    if (n >= 2 && d[0] == 0xFF && (d[1] & 0xE0) == 0xE0) return 0x03;  /* MPEG 音频 */
    if (n >= 2 && d[0] == 0x0B && d[1] == 0x77) return n >= 6 && (d[5] >> 3) > 10 ? 0x87 : 0x81;
    if (n >= 4 && d[0] == 0x7F && d[1] == 0xFE && d[2] == 0x80 && d[3] == 0x01) return 0x82;
    return 0;
}

static int pend_add(ms_tsconv *c, const uint8_t *p, size_t n)
{
    if (c->pend_n + n + 4 > c->pend_cap) {
        size_t nc = c->pend_cap ? c->pend_cap * 2 : 262144;
        uint8_t *nb;
        while (nc < c->pend_n + n + 4) nc *= 2;
        nb = (uint8_t *)realloc(c->pend, nc);
        if (!nb) return -2;
        c->pend = nb;
        c->pend_cap = nc;
    }
    c->pend[c->pend_n] = (uint8_t)(n >> 24); c->pend[c->pend_n + 1] = (uint8_t)(n >> 16);
    c->pend[c->pend_n + 2] = (uint8_t)(n >> 8); c->pend[c->pend_n + 3] = (uint8_t)n;
    memcpy(c->pend + c->pend_n + 4, p, n);
    c->pend_n += n + 4;
    return 0;
}

static void choose_pcr(ms_tsconv *c)
{
    int i;
    c->pcr_idx = -1;
    for (i = 0; i < c->nes; i++) if (c->es[i].used && c->es[i].st && c->es[i].video) { c->pcr_idx = i; return; }
    for (i = 0; i < c->nes; i++) if (c->es[i].used && c->es[i].st) { c->pcr_idx = i; return; }
}

static int ps_emit(ms_tsconv *c, const uint8_t *p, size_t n);

static int ps_flush_pend(ms_tsconv *c)
{
    size_t off = 0;
    int r = 0;
    choose_pcr(c);
    c->pmt_sent = 0;
    if (write_psi(c)) return c->err;
    c->pmt_sent = 1;
    while (off + 4 <= c->pend_n && r == 0) {
        size_t l = ((size_t)c->pend[off] << 24) | ((size_t)c->pend[off + 1] << 16) | ((size_t)c->pend[off + 2] << 8) | c->pend[off + 3];
        r = ps_emit(c, c->pend + off + 4, l);
        off += 4 + l;
    }
    free(c->pend);
    c->pend = NULL;
    c->pend_n = c->pend_cap = 0;
    return r;
}

/* 处理一个完整 PES（PMT 已发） */
static int ps_emit(ms_tsconv *c, const uint8_t *p, size_t n)
{
    int key = p[3];
    cv_es *e;
    size_t pl;
    const uint8_t *pay = pes_payload(p, n, &pl);
    if (!pay) return 0;
    if (key == 0xBD && pl > 0 && pay[0] >= 0x80 && pay[0] <= 0x8F) key = (pay[0] << 8) | 0xBD;
    e = es_by_key(c, key);
    if (!e || !e->st) return 0;
    if (e->strip4) {
        /* 重建 PES：原头 + 去掉 4 字节子流头的负载 */
        size_t h = n - pl, tot;
        int r;
        if (pl < 4) return 0;
        tot = n - 4;
        if ((r = tmp_reserve(c, tot)) != 0) return r;
        memcpy(c->tmp, p, h);
        memcpy(c->tmp + h, pay + 4, pl - 4);
        c->tmp[4] = (uint8_t)((tot - 6) >> 8);
        c->tmp[5] = (uint8_t)(tot - 6);
        return write_pes_ts(c, e, c->tmp, tot);
    }
    return write_pes_ts(c, e, p, n);
}

static int ps_pes(ms_tsconv *c, const uint8_t *p, size_t n)
{
    int sid = p[3], key = sid;
    size_t pl;
    const uint8_t *pay = pes_payload(p, n, &pl);
    cv_es *e;
    if (!pay) return 0; /* MPEG-1 PES 头：不支持（诚实跳过） */
    if (sid == 0xBD && pl > 0 && pay[0] >= 0x80 && pay[0] <= 0x8F) key = (pay[0] << 8) | 0xBD;
    e = es_by_key(c, key);
    if (!e) {
        if (c->pmt_sent) return 0; /* PMT 已定：后来的新流忽略 */
        e = es_add(c);
        if (!e) return 0;
        e->key = key;
        e->sid = (uint8_t)sid;
        if (c->have_psm && c->psm_type[sid]) e->st = c->psm_type[sid];
        else if (sid >= 0xE0 && sid <= 0xEF) e->st = sniff_video(pay, pl);
        else if (sid >= 0xC0 && sid <= 0xDF) e->st = sniff_audio(pay, pl);
        else if (sid == 0xBD) {
            if (key != 0xBD) {
                e->strip4 = 1;
                e->st = (key >> 8) <= 0x87 ? 0x81 : 0x82;
            } else e->st = sniff_audio(pay, pl);
        }
        /* 只接受常见类型：其它（如 G.711 0x90）跳过 */
        switch (e->st) {
        case 0x02: case 0x10: case 0x1B: case 0x24: case 0x42: e->video = 1; break;
        case 0x03: case 0x04: case 0x0F: case 0x11: case 0x81: case 0x87: case 0x82: break;
        default: e->st = 0; c->skipped++; break;
        }
        if (e->st && e->video) c->seen_v = 1;
        if (e->st && !e->video) c->seen_a = 1;
    }
    if (!c->pmt_sent) {
        int r = pend_add(c, p, n);
        if (r) return r;
        if ((c->seen_v && c->seen_a) || c->pend_n >= CV_MAX_PEND) return ps_flush_pend(c);
        return 0;
    }
    return ps_emit(c, p, n);
}

/* MPEG-1 系统流 PES 头 → MPEG-2 PES 头（PTS/DTS 编码相同，直接搬） */
static int ps_pes_mpeg1(ms_tsconv *c, const uint8_t *p, size_t n)
{
    size_t off = 6, tsl = 0, pl, tot;
    uint8_t *q;
    int r, k = 0;
    while (off < n && p[off] == 0xFF && k++ < 16) off++;
    if (off < n && (p[off] & 0xC0) == 0x40) off += 2;
    if (off >= n) return 0;
    if ((p[off] & 0xF0) == 0x20) tsl = 5;
    else if ((p[off] & 0xF0) == 0x30) tsl = 10;
    else if (p[off] == 0x0F) { off++; tsl = 0; }
    else return 0; /* 头坏：丢弃 */
    if (off + tsl > n) return 0;
    pl = n - off - tsl;
    tot = 9 + tsl + pl;
    q = (uint8_t *)malloc(tot);
    if (!q) return -2;
    q[0] = 0; q[1] = 0; q[2] = 1; q[3] = p[3];
    q[4] = (uint8_t)((tot - 6) >> 8); q[5] = (uint8_t)(tot - 6);
    q[6] = 0x80;
    q[7] = tsl == 10 ? 0xC0 : tsl == 5 ? 0x80 : 0x00;
    q[8] = (uint8_t)tsl;
    if (tsl) memcpy(q + 9, p + off, tsl);
    memcpy(q + 9 + tsl, p + off + tsl, pl);
    r = (tot - 6) <= 65535 ? ps_pes(c, q, tot) : 0;
    free(q);
    return r;
}

static void ps_psm(ms_tsconv *c, const uint8_t *p, size_t n)
{
    size_t pil, off, end, esl;
    if (n < 16) return;
    pil = (size_t)((p[8] << 8) | p[9]);
    off = 10 + pil;
    if (off + 2 > n) return;
    esl = (size_t)((p[off] << 8) | p[off + 1]);
    off += 2;
    end = off + esl;
    if (end > n) end = n;
    while (off + 4 <= end) {
        uint8_t st = p[off], id = p[off + 1];
        size_t il = (size_t)((p[off + 2] << 8) | p[off + 3]);
        c->psm_type[id] = st;
        off += 4 + il;
    }
    c->have_psm = 1;
}

static int ps_parse(ms_tsconv *c, int final)
{
    for (;;) {
        const uint8_t *b = c->buf + c->pos;
        size_t avail = c->n - c->pos, need;
        uint8_t id;
        if (avail < 4) return 0;
        if (!(b[0] == 0 && b[1] == 0 && b[2] == 1)) {
            /* 重同步：找下一个起始码 */
            size_t i;
            for (i = 1; i + 3 <= avail; i++) if (b[i] == 0 && b[i + 1] == 0 && b[i + 2] == 1) break;
            c->pos += i;
            continue;
        }
        id = b[3];
        if (id == 0xBA) {
            if (avail < 14) return 0;
            need = (b[4] & 0xC0) == 0x40 ? 14u + (b[13] & 7) : 12u;
        } else if (id == 0xB9) {
            need = 4;
        } else if (id >= 0xBB) {
            if (avail < 6) return 0;
            need = 6u + (size_t)((b[4] << 8) | b[5]);
            if (need == 6 && id >= 0xE0 && id <= 0xEF) {
                /* 长度 0 的视频 PES：到下一个 pack/PES 起始码为止 */
                size_t i;
                for (i = 6; i + 4 <= avail; i++)
                    if (b[i] == 0 && b[i + 1] == 0 && b[i + 2] == 1 && (b[i + 3] == 0xBA || b[i + 3] >= 0xBC)) break;
                if (i + 4 > avail && !final) return 0;
                need = i + 4 > avail ? avail : i;
            }
        } else {
            c->pos += 1; /* 非 PS 层起始码（ES 内部），跳过 */
            continue;
        }
        if (avail < need) return 0;
        if (id == 0xBC) ps_psm(c, b, need);
        else if (id == 0xBD || (id >= 0xC0 && id <= 0xEF)) {
            int r;
            if (need >= 7 && (b[6] & 0xC0) != 0x80) r = ps_pes_mpeg1(c, b, need);
            else r = ps_pes(c, b, need);
            if (r) return r;
        }
        c->pos += need;
        if (c->err) return c->err;
    }
}

/* ---------------- MKV ---------------- */
#define ID_EBML        0x1A45DFA3u
#define ID_SEGMENT     0x18538067u
#define ID_INFO        0x1549A966u
#define ID_TCSCALE     0x2AD7B1u
#define ID_TRACKS      0x1654AE6Bu
#define ID_TRACKENTRY  0xAEu
#define ID_TRACKNUM    0xD7u
#define ID_TRACKTYPE   0x83u
#define ID_CODECID     0x86u
#define ID_CODECPRIV   0x63A2u
#define ID_CENCS       0x6D80u
#define ID_CENC        0x6240u
#define ID_CCOMP       0x5034u
#define ID_CCALGO      0x4254u
#define ID_CCSET       0x4255u
#define ID_CLUSTER     0x1F43B675u
#define ID_TIMECODE    0xE7u
#define ID_SIMPLEBLOCK 0xA3u
#define ID_BLOCKGROUP  0xA0u
#define ID_BLOCK       0xA1u

static int vint_len(uint8_t b)
{
    int l = 1;
    uint8_t m = 0x80;
    while (l <= 8 && !(b & m)) { m >>= 1; l++; }
    return l;
}

/* 读元素 ID（保留长度标记位），返回字节数；0=需更多；-1=无效 */
static int rd_id(const uint8_t *d, size_t n, uint32_t *id)
{
    int l, i;
    if (n < 1) return 0;
    l = vint_len(d[0]);
    if (l > 4) return -1;
    if (n < (size_t)l) return 0;
    *id = 0;
    for (i = 0; i < l; i++) *id = (*id << 8) | d[i];
    return l;
}

/* 读长度（去掉标记位），未知长度返回 UINT64_MAX */
static int rd_size(const uint8_t *d, size_t n, uint64_t *sz)
{
    int l, i, allone;
    uint64_t v;
    if (n < 1) return 0;
    l = vint_len(d[0]);
    if (l > 8) return -1;
    if (n < (size_t)l) return 0;
    v = d[0] & (0xFF >> l);
    allone = v == (uint64_t)(0xFF >> l);
    for (i = 1; i < l; i++) { v = (v << 8) | d[i]; if (d[i] != 0xFF) allone = 0; }
    *sz = allone ? UINT64_MAX : v;
    return l;
}

static uint64_t rd_uint(const uint8_t *d, size_t n)
{
    uint64_t v = 0;
    size_t i;
    for (i = 0; i < n && i < 8; i++) v = (v << 8) | d[i];
    return v;
}

static int is_master(uint32_t id)
{
    return id == ID_SEGMENT || id == ID_INFO || id == ID_TRACKS || id == ID_TRACKENTRY || id == ID_CLUSTER ||
           id == ID_BLOCKGROUP || id == ID_CENCS || id == ID_CENC || id == ID_CCOMP;
}

static int is_needed_leaf(uint32_t id)
{
    return id == ID_TCSCALE || id == ID_TRACKNUM || id == ID_TRACKTYPE || id == ID_CODECID || id == ID_CODECPRIV ||
           id == ID_CCALGO || id == ID_CCSET || id == ID_TIMECODE || id == ID_SIMPLEBLOCK || id == ID_BLOCK;
}

static void append_ps(cv_es *e, const uint8_t *nal, size_t n)
{
    uint8_t *nb = (uint8_t *)realloc(e->ps, e->ps_n + 4 + n);
    if (!nb) return;
    e->ps = nb;
    e->ps[e->ps_n] = 0; e->ps[e->ps_n + 1] = 0; e->ps[e->ps_n + 2] = 0; e->ps[e->ps_n + 3] = 1;
    memcpy(e->ps + e->ps_n + 4, nal, n);
    e->ps_n += 4 + n;
}

static void mkv_finalize_track(ms_tsconv *c, cv_es *e)
{
    const char *id = e->codec;
    e->st = 0;
    if (!strcmp(id, "V_MPEG4/ISO/AVC")) {
        e->st = 0x1B; e->video = 1; e->sid = 0xE0;
        if (e->priv_n >= 7) {
            const uint8_t *p = e->priv;
            size_t off = 6, i, cnt;
            e->nal_len = (p[4] & 3) + 1;
            cnt = p[5] & 0x1F;
            for (i = 0; i < cnt && off + 2 <= e->priv_n; i++) {
                size_t l = (size_t)((p[off] << 8) | p[off + 1]);
                if (off + 2 + l > e->priv_n) break;
                append_ps(e, p + off + 2, l);
                off += 2 + l;
            }
            if (off < e->priv_n) {
                cnt = p[off++];
                for (i = 0; i < cnt && off + 2 <= e->priv_n; i++) {
                    size_t l = (size_t)((p[off] << 8) | p[off + 1]);
                    if (off + 2 + l > e->priv_n) break;
                    append_ps(e, p + off + 2, l);
                    off += 2 + l;
                }
            }
        } else e->nal_len = 4;
    } else if (!strcmp(id, "V_MPEGH/ISO/HEVC")) {
        e->st = 0x24; e->video = 1; e->sid = 0xE0;
        if (e->priv_n >= 23) {
            const uint8_t *p = e->priv;
            size_t off = 23, a, na = p[22];
            e->nal_len = (p[21] & 3) + 1;
            for (a = 0; a < na && off + 3 <= e->priv_n; a++) {
                size_t k, cnt = (size_t)((p[off + 1] << 8) | p[off + 2]);
                off += 3;
                for (k = 0; k < cnt && off + 2 <= e->priv_n; k++) {
                    size_t l = (size_t)((p[off] << 8) | p[off + 1]);
                    if (off + 2 + l > e->priv_n) break;
                    append_ps(e, p + off + 2, l);
                    off += 2 + l;
                }
            }
        } else e->nal_len = 4;
    } else if (!strcmp(id, "V_MPEG2") || !strcmp(id, "V_MPEG1")) {
        e->st = 0x02; e->video = 1; e->sid = 0xE0;
    } else if (!strncmp(id, "A_AAC", 5)) {
        e->st = 0x0F; e->sid = 0xC0; e->aac = 1;
        if (e->priv_n >= 2) {
            int aot = e->priv[0] >> 3, sri = ((e->priv[0] & 7) << 1) | (e->priv[1] >> 7), ch = (e->priv[1] >> 3) & 0x0F;
            if (aot == 5 || aot == 29 || aot > 4 || aot < 1) aot = 2;
            e->adts[0] = 0xFF; e->adts[1] = 0xF1;
            e->adts[2] = (uint8_t)(((aot - 1) << 6) | (sri << 2) | (ch >> 2));
            e->adts[3] = (uint8_t)((ch & 3) << 6);
        } else e->st = 0;
    } else if (!strcmp(id, "A_AC3")) { e->st = 0x81; e->sid = 0xBD; }
    else if (!strcmp(id, "A_EAC3")) { e->st = 0x87; e->sid = 0xBD; }
    else if (!strcmp(id, "A_DTS")) { e->st = 0x82; e->sid = 0xBD; }
    else if (!strcmp(id, "A_MPEG/L2") || !strcmp(id, "A_MPEG/L3") || !strcmp(id, "A_MPEG/L1")) { e->st = 0x03; e->sid = 0xC0; }
    if (!e->st) c->skipped++;
    else if (e->video) c->seen_v = 1;
    else c->seen_a = 1;
}

static void mkv_tracks_done(ms_tsconv *c)
{
    int i, vs = 0, as = 0;
    if (c->in_tracks_done) return;
    c->in_tracks_done = 1;
    for (i = 0; i < c->nes; i++) {
        cv_es *e = &c->es[i];
        mkv_finalize_track(c, e);
        if (e->st && e->video) e->sid = (uint8_t)(0xE0 + vs++);
        else if (e->st && e->sid == 0xC0) e->sid = (uint8_t)(0xC0 + as++);
    }
    choose_pcr(c);
    {
        int hevc = 0, other = 0, ha = 0, hv = 0;
        for (i = 0; i < c->nes; i++) {
            cv_es *e = &c->es[i];
            if (!e->st) continue;
            if (e->st == 0x24) hevc = 1;
            else if (e->st != 0x1B && !e->aac) other = 1;
            if (e->video) hv = 1; else ha = 1;
        }
        if (hevc) {
            c->flv = 1;
            c->fw.write = c->w;
            c->fw.ctx = c->ctx;
            /* FLV 只带 H.264/HEVC + AAC，其它轨（AC-3 等）在此模式下跳过 */
            for (i = 0; i < c->nes; i++) {
                cv_es *e = &c->es[i];
                if (e->st && e->st != 0x24 && e->st != 0x1B && !e->aac) { e->st = 0; c->skipped++; }
            }
            (void)other;
            if (ms_flvw_header(&c->fw, ha, hv) != 0) c->err = -1;
        }
    }
}

/* FLV 模式：长度前缀帧直接写（NAL 长度非 4 时改写为 4） */
static int flv_frame(ms_tsconv *c, cv_es *e, uint32_t ms, const uint8_t *d, size_t n)
{
    int r;
    if (e->aac) {
        if (!e->flv_cfg_sent) {
            if (ms_flvw_aac_config(&c->fw, ms, e->priv, e->priv_n)) return c->err = -1;
            e->flv_cfg_sent = 1;
        }
        return ms_flvw_aac_frame(&c->fw, ms, d, n) ? (c->err = -1) : 0;
    }
    {
        int hevc = e->st == 0x24, key = 0, nl = e->nal_len;
        size_t off = 0, o = 0;
        uint8_t *lp;
        if (!e->flv_cfg_sent) {
            if (!e->priv_n) return 0;
            /* 帧统一改写为 4 字节 NAL 长度，配置里的 lengthSizeMinusOne 同步为 3 */
            if (hevc && e->priv_n > 21) e->priv[21] |= 3;
            if (!hevc && e->priv_n > 4) e->priv[4] |= 3;
            if (ms_flvw_video_config(&c->fw, hevc, ms, e->priv, e->priv_n)) return c->err = -1;
            e->flv_cfg_sent = 1;
        }
        lp = (uint8_t *)malloc(n / 1 + 4 * (n / 2 + 1));
        if (!lp) return -2;
        while (off + (size_t)nl <= n) {
            size_t l = 0;
            int i;
            for (i = 0; i < nl; i++) l = (l << 8) | d[off + i];
            off += (size_t)nl;
            if (l > n - off) break;
            if (l) {
                int t = hevc ? ((d[off] >> 1) & 0x3F) : (d[off] & 0x1F);
                if ((hevc && t >= 16 && t <= 21) || (!hevc && t == 5)) key = 1;
                lp[o] = (uint8_t)(l >> 24); lp[o + 1] = (uint8_t)(l >> 16); lp[o + 2] = (uint8_t)(l >> 8); lp[o + 3] = (uint8_t)l;
                memcpy(lp + o + 4, d + off, l);
                o += 4 + l;
            }
            off += l;
        }
        r = o ? ms_flvw_video_frame(&c->fw, hevc, key, ms, 0, lp, o) : 0;
        free(lp);
        return r ? (c->err = -1) : 0;
    }
}

static int mkv_frame(ms_tsconv *c, cv_es *e, uint64_t pts90, const uint8_t *d, size_t n)
{
    if (!e->st || n == 0) return 0;
    if (c->flv && !e->strip_pfx_n) return flv_frame(c, e, (uint32_t)(pts90 / 90u), d, n);
    if (e->strip_pfx_n) {
        int r;
        /* 头剥离：前缀 + 数据，再按普通帧处理（递归一层，前缀清零避免重复） */
        {
            uint8_t *joined = (uint8_t *)malloc(e->strip_pfx_n + n);
            size_t pn = e->strip_pfx_n;
            uint8_t *pfx = e->strip_pfx;
            if (!joined) return -2;
            memcpy(joined, pfx, pn);
            memcpy(joined + pn, d, n);
            e->strip_pfx_n = 0;
            r = mkv_frame(c, e, pts90, joined, pn + n);
            e->strip_pfx_n = pn;
            free(joined);
            return r;
        }
    }
    if (e->aac) {
        uint8_t h[7];
        size_t fl = n + 7;
        if (fl > 0x1FFF) return 0;
        memcpy(h, e->adts, 7);
        h[3] = (uint8_t)((h[3] & 0xC0) | (fl >> 11));
        h[4] = (uint8_t)(fl >> 3);
        h[5] = (uint8_t)(((fl & 7) << 5) | 0x1F);
        h[6] = 0xFC;
        return emit_frame(c, e, pts90, h, 7, d, n);
    }
    if (e->st == 0x1B || e->st == 0x24) {
        /* 长度前缀 → Annex-B；关键帧前补参数集 */
        size_t off = 0, o = 0, cap = n + e->ps_n + 64;
        int key = 0, r, nl = e->nal_len;
        uint8_t *ab;
        while (off + (size_t)nl <= n) {
            size_t l = 0;
            int i;
            for (i = 0; i < nl; i++) l = (l << 8) | d[off + i];
            off += (size_t)nl;
            if (l > n - off) break;
            if (l > 0) {
                int t = e->st == 0x1B ? (d[off] & 0x1F) : ((d[off] >> 1) & 0x3F);
                if ((e->st == 0x1B && t == 5) || (e->st == 0x24 && t >= 16 && t <= 21)) key = 1;
                cap += 4;
            }
            off += l;
        }
        cap += 8;
        ab = (uint8_t *)malloc(cap);
        if (!ab) return -2;
        /* AU 分隔符：TS 里按 AUD 定界（与 FFmpeg mpegts 复用器一致） */
        if (e->st == 0x1B) { static const uint8_t aud[6] = { 0, 0, 0, 1, 0x09, 0xF0 }; memcpy(ab, aud, 6); o = 6; }
        else { static const uint8_t aud[7] = { 0, 0, 0, 1, 0x46, 0x01, 0x50 }; memcpy(ab, aud, 7); o = 7; }
        if (key && e->ps_n) { memcpy(ab + o, e->ps, e->ps_n); o += e->ps_n; }
        off = 0;
        while (off + (size_t)nl <= n) {
            size_t l = 0;
            int i;
            for (i = 0; i < nl; i++) l = (l << 8) | d[off + i];
            off += (size_t)nl;
            if (l > n - off) break;
            if (l > 0 && o + 4 + l <= cap) {
                ab[o] = 0; ab[o + 1] = 0; ab[o + 2] = 0; ab[o + 3] = 1;
                memcpy(ab + o + 4, d + off, l);
                o += 4 + l;
            }
            off += l;
        }
        r = o > 7 ? emit_frame(c, e, pts90, NULL, 0, ab, o) : 0;
        free(ab);
        return r;
    }
    return emit_frame(c, e, pts90, NULL, 0, d, n);
}

static int mkv_block(ms_tsconv *c, const uint8_t *d, size_t n)
{
    uint64_t tn;
    int l, i, lacing, nf;
    int16_t rel;
    int64_t ticks;
    uint64_t pts90;
    cv_es *e = NULL;
    size_t off, sizes[256];
    if (!c->in_tracks_done) mkv_tracks_done(c);
    l = rd_size(d, n, &tn);
    if (l <= 0 || (size_t)l + 3 > n) return 0;
    for (i = 0; i < c->nes; i++) if (c->es[i].trackno == tn) { e = &c->es[i]; break; }
    if (!e || !e->st) return 0;
    rel = (int16_t)((d[l] << 8) | d[l + 1]);
    lacing = (d[l + 2] >> 1) & 3;
    off = (size_t)l + 3;
    ticks = (int64_t)c->cluster_tc + rel;
    if (ticks < 0) ticks = 0;
    {
        /* tc_scale ns → 90 kHz：ticks * scale * 9 / 100000（拆分避免 64 位溢出，不用 __int128 以兼容 32 位 ABI） */
        uint64_t q = c->tc_scale / 100000u, rm = c->tc_scale % 100000u;
        pts90 = (uint64_t)ticks * q * 9u + (uint64_t)ticks * rm * 9u / 100000u;
    }
    if (lacing == 0) return mkv_frame(c, e, pts90, d + off, n - off);
    if (off >= n) return 0;
    nf = d[off++] + 1;
    if (lacing == 1) { /* Xiph */
        size_t tot = 0;
        for (i = 0; i < nf - 1; i++) {
            size_t s = 0;
            for (;;) {
                if (off >= n) return 0;
                s += d[off];
                if (d[off++] != 0xFF) break;
            }
            sizes[i] = s;
            tot += s;
        }
        if (tot > n - off) return 0;
        sizes[nf - 1] = n - off - tot;
    } else if (lacing == 3) { /* EBML */
        uint64_t v;
        size_t tot;
        int k = rd_size(d + off, n - off, &v);
        if (k <= 0 || v == UINT64_MAX) return 0;
        off += (size_t)k;
        sizes[0] = (size_t)v;
        tot = sizes[0];
        for (i = 1; i < nf - 1; i++) {
            uint64_t raw;
            int64_t sv;
            k = rd_size(d + off, n - off, &raw);
            if (k <= 0) return 0;
            sv = (int64_t)raw - (((int64_t)1 << (7 * k - 1)) - 1);
            off += (size_t)k;
            if ((int64_t)sizes[i - 1] + sv < 0) return 0;
            sizes[i] = (size_t)((int64_t)sizes[i - 1] + sv);
            tot += sizes[i];
        }
        if (tot > n - off) return 0;
        sizes[nf - 1] = n - off - tot;
    } else { /* 固定 */
        size_t s = (n - off) / (size_t)nf;
        for (i = 0; i < nf; i++) sizes[i] = s;
    }
    for (i = 0; i < nf; i++) {
        int r;
        if (sizes[i] > n - off) return 0;
        /* 一个块内多帧共用时间戳：音频（lacing 只用于音频）由解码器按帧长推进 */
        r = mkv_frame(c, e, pts90, d + off, sizes[i]);
        if (r) return r;
        off += sizes[i];
    }
    return 0;
}

static int mkv_leaf(ms_tsconv *c, uint32_t id, const uint8_t *d, size_t n)
{
    cv_es *e = c->cur_trk;
    switch (id) {
    case ID_TCSCALE: c->tc_scale = rd_uint(d, n); if (!c->tc_scale) c->tc_scale = 1000000; return 0;
    case ID_TRACKNUM: if (e) e->trackno = rd_uint(d, n); return 0;
    case ID_TRACKTYPE: if (e) e->mkv_type = (int)rd_uint(d, n); return 0;
    case ID_CODECID:
        if (e) { size_t l = n < sizeof(e->codec) - 1 ? n : sizeof(e->codec) - 1; memcpy(e->codec, d, l); e->codec[l] = 0; }
        return 0;
    case ID_CODECPRIV:
        if (e && n <= 1u << 20) { free(e->priv); e->priv = (uint8_t *)malloc(n ? n : 1); if (e->priv) { memcpy(e->priv, d, n); e->priv_n = n; } }
        return 0;
    case ID_CCALGO: c->cur_algo = (int)rd_uint(d, n); if (e && c->cur_algo != 3) e->codec[0] = '!'; return 0;
    case ID_CCSET:
        if (e && c->cur_algo == 3 && n <= 64) { free(e->strip_pfx); e->strip_pfx = (uint8_t *)malloc(n ? n : 1); if (e->strip_pfx) { memcpy(e->strip_pfx, d, n); e->strip_pfx_n = n; } }
        return 0;
    case ID_TIMECODE: c->cluster_tc = rd_uint(d, n); return 0;
    case ID_SIMPLEBLOCK: case ID_BLOCK: return mkv_block(c, d, n);
    default: return 0;
    }
}

static int mkv_parse(ms_tsconv *c, int final)
{
    (void)final;
    for (;;) {
        const uint8_t *b = c->buf + c->pos;
        size_t avail = c->n - c->pos;
        uint32_t id;
        uint64_t sz;
        int il, sl;
        if (c->skip) {
            size_t k = c->skip < avail ? (size_t)c->skip : avail;
            c->pos += k;
            c->skip -= k;
            if (c->skip) return 0;
            continue;
        }
        if (avail == 0) return 0;
        il = rd_id(b, avail, &id);
        if (il == 0) return 0;
        if (il > 0) {
            sl = rd_size(b + il, avail - (size_t)il, &sz);
            if (sl == 0) return 0;
        } else sl = -1;
        if (il < 0 || sl < 0 || (!is_master(id) && sz == UINT64_MAX)) {
            /* 无效：向前找 Cluster 重同步 */
            size_t i;
            for (i = 1; i + 4 <= avail; i++)
                if (b[i] == 0x1F && b[i + 1] == 0x43 && b[i + 2] == 0xB6 && b[i + 3] == 0x75) break;
            c->pos += i;
            if (i + 4 > avail) return 0;
            continue;
        }
        if (is_master(id)) {
            c->pos += (size_t)(il + sl);
            if (id == ID_TRACKENTRY) {
                cv_es *e = es_add(c);
                c->cur_trk = e;
                c->cur_algo = 0;
            } else if (id == ID_CLUSTER) {
                if (!c->in_tracks_done) mkv_tracks_done(c);
                c->cur_trk = NULL;
            }
            continue;
        }
        if (is_needed_leaf(id)) {
            size_t tot = (size_t)il + (size_t)sl + (size_t)sz;
            int r;
            if (sz > CV_MAX_ELEM) { c->pos += (size_t)(il + sl); c->skip = sz; continue; }
            if (avail < tot) return 0;
            r = mkv_leaf(c, id, b + il + sl, (size_t)sz);
            if (r) return r;
            c->pos += tot;
            if (c->err) return c->err;
            continue;
        }
        /* 其它元素（EBML 头、SeekHead、Cues、Tags、附件……）流式跳过 */
        c->pos += (size_t)(il + sl);
        c->skip = sz;
    }
}

/* ---------------- 公共 ---------------- */
ms_tsconv *ms_tsconv_create(int kind, ms_tsconv_write_fn w, void *ctx)
{
    ms_tsconv *c;
    if ((kind != MS_TSCONV_MKV && kind != MS_TSCONV_PS) || !w) return NULL;
    c = (ms_tsconv *)calloc(1, sizeof(*c));
    if (!c) return NULL;
    c->kind = kind;
    c->w = w;
    c->ctx = ctx;
    c->pcr_idx = -1;
    c->tc_scale = 1000000;
    return c;
}

static int run(ms_tsconv *c, int final)
{
    int r = c->kind == MS_TSCONV_MKV ? mkv_parse(c, final) : ps_parse(c, final);
    /* 压缩缓冲 */
    if (c->pos > 0) {
        memmove(c->buf, c->buf + c->pos, c->n - c->pos);
        c->n -= c->pos;
        c->pos = 0;
    }
    return r ? r : c->err;
}

int ms_tsconv_feed(ms_tsconv *c, const uint8_t *d, size_t n)
{
    while (n > 0) {
        size_t take;
        int r;
        if (c->err) return c->err;
        if (c->n == c->cap) {
            size_t nc = c->cap ? c->cap * 2 : 262144;
            uint8_t *nb;
            if (nc > CV_MAX_ELEM + 65536) {
                /* 单元素超界：丢弃缓冲重同步（坏数据容错） */
                c->n = 0;
                c->pos = 0;
                nc = c->cap;
            } else {
                nb = (uint8_t *)realloc(c->buf, nc);
                if (!nb) return -2;
                c->buf = nb;
                c->cap = nc;
            }
        }
        take = c->cap - c->n < n ? c->cap - c->n : n;
        memcpy(c->buf + c->n, d, take);
        c->n += take;
        d += take;
        n -= take;
        if ((r = run(c, 0)) != 0) return r;
    }
    return 0;
}

int ms_tsconv_end(ms_tsconv *c)
{
    int r = run(c, 1);
    if (r) return r;
    if (c->kind == MS_TSCONV_PS && !c->pmt_sent && c->pend_n) return ps_flush_pend(c);
    return c->err;
}

void ms_tsconv_destroy(ms_tsconv *c)
{
    int i;
    if (!c) return;
    for (i = 0; i < CV_MAX_ES; i++) {
        free(c->es[i].priv);
        free(c->es[i].ps);
        free(c->es[i].strip_pfx);
    }
    free(c->buf);
    free(c->pend);
    free(c->tmp);
    free(c);
}

void ms_tsconv_stats(const ms_tsconv *c, int *pes_out, int *skipped_tracks)
{
    if (pes_out) *pes_out = c ? c->pes_out : 0;
    if (skipped_tracks) *skipped_tracks = c ? c->skipped : 0;
}
