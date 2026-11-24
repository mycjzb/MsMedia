/*
 * 当前头下的数组步进核验：ms_session_list_tracks / ms_sub_doc_cues_at。
 * 与 test_abi_old_header 链到同一进程；1838 头尚无这些加法 API。
 * by: Long <mycjzb@gmail.com>
 */
#include "msmedia/msmedia.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CANARY 0xA5u

static int canary_ok(const unsigned char *p, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        if (p[i] != CANARY) {
            return 0;
        }
    }
    return 1;
}

static void wr_be24(uint8_t *p, unsigned v)
{
    p[0] = (uint8_t)((v >> 16) & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)(v & 0xFFu);
}

static void wr_be32(uint8_t *p, unsigned v)
{
    p[0] = (uint8_t)((v >> 24) & 0xFFu);
    p[1] = (uint8_t)((v >> 16) & 0xFFu);
    p[2] = (uint8_t)((v >> 8) & 0xFFu);
    p[3] = (uint8_t)(v & 0xFFu);
}

static int add_tag(uint8_t *out, size_t cap, size_t *n, unsigned typ,
                   unsigned dts, const uint8_t *body, size_t bn)
{
    size_t need = *n + 11u + bn + 4u;
    if (need > cap) {
        return -1;
    }
    out[*n] = (uint8_t)typ;
    wr_be24(out + *n + 1, (unsigned)bn);
    wr_be24(out + *n + 4, dts & 0xFFFFFFu);
    out[*n + 7] = (uint8_t)((dts >> 24) & 0xFFu);
    wr_be24(out + *n + 8, 0);
    memcpy(out + *n + 11, body, bn);
    wr_be32(out + *n + 11 + bn, (unsigned)(11u + bn));
    *n = need;
    return 0;
}

static const uint8_t k_avcc[] = {
    0x01, 0x64, 0x00, 0x1e, 0xff,
    0xe1, 0x00, 0x0b,
    0x67, 0x64, 0x00, 0x1e, 0xac, 0xc8, 0x40, 0x78, 0x9a, 0x80, 0x00,
    0x01, 0x00, 0x04,
    0x68, 0xee, 0x3c, 0x80
};

static const uint8_t k_nal[] = {
    0x00, 0x00, 0x00, 0x08,
    0x65, 0x88, 0x84, 0x00, 0x2a, 0x00, 0x00, 0x01
};

int test_abi_stride_current(void);

int test_abi_stride_current(void)
{
    uint8_t flv[512];
    uint8_t vcfg[5 + sizeof(k_avcc)];
    uint8_t vnal[5 + sizeof(k_nal)];
    size_t n = 0, consumed = 0, tcount = 0, ccount = 0;
    static const uint8_t hdr[] = {'F', 'L', 'V', 0x01, 0x01, 0, 0, 0, 9, 0, 0, 0, 0};
    ms_session *s = NULL;
    ms_session_limits lim;
    ms_sub_doc *doc = NULL;
    ms_status st;
    int rc = 0;
    static const char *vtt =
        "WEBVTT\n\n"
        "00:00:00.000 --> 00:00:02.000\nhello\n\n"
        "00:00:01.000 --> 00:00:03.000\nworld\n";

    memcpy(flv, hdr, sizeof(hdr));
    n = sizeof(hdr);
    vcfg[0] = 0x17; vcfg[1] = 0x00; vcfg[2] = 0; vcfg[3] = 0; vcfg[4] = 0;
    memcpy(vcfg + 5, k_avcc, sizeof(k_avcc));
    vnal[0] = 0x17; vnal[1] = 0x01; vnal[2] = 0; vnal[3] = 0; vnal[4] = 0;
    memcpy(vnal + 5, k_nal, sizeof(k_nal));
    if (add_tag(flv, sizeof(flv), &n, 9, 0, vcfg, sizeof(vcfg)) != 0 ||
        add_tag(flv, sizeof(flv), &n, 9, 0, vnal, sizeof(vnal)) != 0) {
        fprintf(stderr, "stride: flv build failed\n");
        return -1;
    }

    memset(&lim, 0, sizeof(lim));
    lim.struct_size = sizeof(lim);
    ms_session_limits_default(&lim);
    if (ms_session_create(NULL, &lim, &s) != MS_OK || s == NULL) {
        fprintf(stderr, "stride: session create failed\n");
        return -1;
    }
    (void)ms_session_push(s, flv, n, &consumed);
    (void)ms_session_end_of_input(s);
    /* 拉完事件，目录才会填满。 */
    for (;;) {
        ms_event ev;
        memset(&ev, 0, sizeof(ev));
        st = ms_session_pull(s, &ev);
        if (st != MS_OK) {
            break;
        }
        if (ev.release_id) {
            (void)ms_session_release(s, ev.release_id);
        }
    }

    {
        struct {
            ms_track_info tr[4];
            unsigned char after[32];
        } box;
        memset(&box, 0, sizeof(box));
        memset(box.after, CANARY, sizeof(box.after));
        st = ms_session_list_tracks(s, box.tr, 4, &tcount);
        if (st != MS_OK || tcount < 1) {
            fprintf(stderr, "stride: list_tracks failed st=%d n=%zu\n", (int)st, tcount);
            rc = -1;
        }
        if (!canary_ok(box.after, sizeof(box.after))) {
            fprintf(stderr, "stride: list_tracks overflowed canary (stride?)\n");
            rc = -1;
        }
    }
    ms_session_destroy(s);

    st = ms_sub_doc_create(NULL, &doc);
    if (st != MS_OK || doc == NULL) {
        fprintf(stderr, "stride: sub_doc_create failed\n");
        return -1;
    }
    st = ms_sub_doc_parse(doc, (const uint8_t *)vtt, strlen(vtt),
                           MS_SUB_FORMAT_WEBVTT, 1);
    if (st != MS_OK && st != MS_LIMIT) {
        fprintf(stderr, "stride: vtt parse failed %d\n", (int)st);
        ms_sub_doc_destroy(doc);
        return -1;
    }
    {
        struct {
            ms_sub_cue cues[4];
            unsigned char after[32];
        } box;
        size_t i;
        memset(&box, 0, sizeof(box));
        memset(box.after, CANARY, sizeof(box.after));
        for (i = 0; i < 4; i++) {
            box.cues[i].struct_size = sizeof(box.cues[i]);
        }
        st = ms_sub_doc_cues_at(doc, 90u * 1500u, box.cues, 4, &ccount);
        if (st != MS_OK || ccount < 1) {
            fprintf(stderr, "stride: cues_at failed st=%d n=%zu\n", (int)st, ccount);
            rc = -1;
        }
        if (!canary_ok(box.after, sizeof(box.after))) {
            fprintf(stderr, "stride: cues_at overflowed canary (stride?)\n");
            rc = -1;
        }
    }
    /* 旧 struct_size 前缀不得越过调用方对象。 */
    if (ms_sub_doc_count(doc) > 0) {
        struct {
            size_t struct_size;
            unsigned char after[32];
        } tiny;
        memset(&tiny, 0, sizeof(tiny));
        memset(tiny.after, CANARY, sizeof(tiny.after));
        tiny.struct_size = sizeof(size_t);
        if (ms_sub_doc_get(doc, 0, (ms_sub_cue *)&tiny) != MS_OK) {
            fprintf(stderr, "stride: tiny cue get failed\n");
            rc = -1;
        }
        if (!canary_ok(tiny.after, sizeof(tiny.after))) {
            fprintf(stderr, "stride: tiny struct_size cue overflowed canary\n");
            rc = -1;
        }
    }
    ms_sub_doc_destroy(doc);
    if (rc == 0) {
        printf("  current-header stride: list_tracks=%zu cues_at=%zu ok\n",
               tcount, ccount);
    }
    return rc;
}
