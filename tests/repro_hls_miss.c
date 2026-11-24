/*
 * 独立复现：HLS 缺失分片跳过 + discontinuity 路径。
 * 只用于优化/正确性矩阵，非正式验收入口。
 * by: Long <mycjzb@gmail.com>
 */
#define _DEFAULT_SOURCE
#include "msmedia/msmedia.h"
#include "ms_ts_synth.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failed;

#define CHECK(cond, msg)                                                 \
    do {                                                                 \
        if (!(cond)) {                                                   \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, msg); \
            failed = 1;                                                  \
            return 1;                                                    \
        }                                                                \
    } while (0)

#define CHECK_ST(st, exp)                                                          \
    do {                                                                 \
        ms_status _s = (st);                                                       \
        ms_status _e = (exp);                                                      \
        if (_s != _e) {                                                            \
            fprintf(stderr, "FAIL %s:%d: status %s expected %s\n", __FILE__,       \
                    __LINE__, ms_status_name(_s), ms_status_name(_e));             \
            failed = 1;                                                            \
            return 1;                                                              \
        }                                                                          \
    } while (0)

static int make_adts(uint8_t *out, size_t cap, size_t *len, int sfi, int ch,
                     int nbytes, uint8_t fill)
{
    uint8_t raw[64];
    if (nbytes > (int)sizeof(raw)) {
        return -1;
    }
    memset(raw, fill, (size_t)nbytes);
    return ms_synth_adts_frame(out, cap, len, 1, sfi, ch, raw, (size_t)nbytes);
}

typedef struct rec {
    int nfmt, ntrk, ncfg, nau, ndisc, neos;
} rec;

typedef struct xf {
    const ms_hls_mem_item *items;
    size_t nitems;
    int nrequests;
    int nreads;
    int canceled;
    uint32_t live_id;
    int live_used;
    size_t live_off, live_end;
    const uint8_t *live_data;
    size_t live_size;
    int nreq_by[8];
    char key_uri[8][96];
    int nkeys;
} xf;

static void xf_count(xf *x, const char *uri)
{
    int i;
    if (uri == NULL) {
        return;
    }
    for (i = 0; i < x->nkeys; i++) {
        if (x->key_uri[i][0] && strcmp(x->key_uri[i], uri) == 0) {
            x->nreq_by[i]++;
            return;
        }
    }
    if (x->nkeys < 8) {
        size_t n = strlen(uri);
        if (n >= sizeof(x->key_uri[0])) {
            n = sizeof(x->key_uri[0]) - 1;
        }
        memcpy(x->key_uri[x->nkeys], uri, n);
        x->key_uri[x->nkeys][n] = 0;
        x->nreq_by[x->nkeys] = 1;
        x->nkeys++;
    }
}

static ms_status xf_request(void *ctx, const ms_hls_request *req, uint32_t *out_id)
{
    xf *x = (xf *)ctx;
    size_t i;
    if (out_id) {
        *out_id = 0;
    }
    if (!x || !req || !req->uri || !out_id) {
        return MS_ERR_INVALID_ARG;
    }
    if (x->canceled) {
        return MS_ERR_CANCELED;
    }
    for (i = 0; i < x->nitems; i++) {
        if (x->items[i].uri && strcmp(x->items[i].uri, req->uri) == 0) {
            x->live_used = 1;
            x->live_id++;
            if (x->live_id == 0) {
                x->live_id = 1;
            }
            x->live_data = x->items[i].data;
            x->live_size = x->items[i].size;
            x->live_off = (size_t)req->byte_off;
            x->live_end = req->byte_len > 0 ? x->live_off + (size_t)req->byte_len
                                            : x->live_size;
            if (x->live_end > x->live_size) {
                x->live_end = x->live_size;
            }
            *out_id = x->live_id;
            x->nrequests++;
            xf_count(x, req->uri);
            return MS_OK;
        }
    }
    return MS_ERR_UNKNOWN;
}

static ms_status xf_read(void *ctx, uint32_t id, uint8_t *buf, size_t len,
                         size_t *out_n)
{
    xf *x = (xf *)ctx;
    size_t n, left;
    if (out_n) {
        *out_n = 0;
    }
    if (!x || !out_n || (buf == NULL && len != 0)) {
        return MS_ERR_INVALID_ARG;
    }
    if (x->canceled) {
        return MS_ERR_CANCELED;
    }
    if (!x->live_used || id != x->live_id) {
        return MS_ERR_STATE;
    }
    if (x->live_off >= x->live_end) {
        return MS_OK;
    }
    left = x->live_end - x->live_off;
    n = left < len ? left : len;
    if (n && x->live_data) {
        memcpy(buf, x->live_data + x->live_off, n);
    }
    x->live_off += n;
    *out_n = n;
    x->nreads++;
    return MS_OK;
}

static void xf_cancel(void *ctx, uint32_t id)
{
    xf *x = (xf *)ctx;
    (void)id;
    if (x) {
        x->canceled = 1;
        x->live_used = 0;
    }
}

static int take_ev(ms_session *s, rec *r, ms_status *pst)
{
    ms_event ev;
    ms_status st = ms_session_pull(s, &ev);
    *pst = st;
    if (st == MS_AGAIN || st == MS_EOS) {
        if (st == MS_EOS) {
            r->neos++;
        }
        return 0;
    }
    CHECK_ST(st, MS_OK);
    if (ev.type == MS_EVENT_FORMAT) {
        r->nfmt++;
    } else if (ev.type == MS_EVENT_TRACK) {
        r->ntrk++;
    } else if (ev.type == MS_EVENT_CODEC_CONFIG) {
        r->ncfg++;
    } else if (ev.type == MS_EVENT_AU) {
        r->nau++;
    } else if (ev.type == MS_EVENT_DISCONTINUITY) {
        r->ndisc++;
    } else if (ev.type == MS_EVENT_EOS) {
        r->neos++;
    }
    if (ev.release_id != 0) {
        CHECK_ST(ms_session_release(s, ev.release_id), MS_OK);
    }
    return 0;
}

static int pump(ms_hls *h, ms_session *s, rec *r, int max_steps, ms_status *out)
{
    int i, stalled = 0;
    *out = MS_OK;
    for (i = 0; i < max_steps; i++) {
        ms_status st, pst;
        int drained = 0;
        for (;;) {
            if (take_ev(s, r, &pst) != 0) {
                return -1;
            }
            if (pst == MS_EOS) {
                *out = MS_EOS;
                return 0;
            }
            if (pst == MS_AGAIN) {
                break;
            }
            drained++;
        }
        st = ms_hls_drive(h);
        if (st == MS_OK) {
            stalled = 0;
            continue;
        }
        if (st == MS_AGAIN) {
            stalled++;
            if (stalled > 128) {
                *out = st;
                return -1;
            }
            continue;
        }
        if (st == MS_LIMIT) {
            if (take_ev(s, r, &pst) != 0) {
                return -1;
            }
            if (pst == MS_AGAIN && drained == 0) {
                stalled++;
                if (stalled > 128) {
                    *out = st;
                    return -1;
                }
            } else {
                stalled = 0;
            }
            continue;
        }
        if (st == MS_EOS) {
            while (take_ev(s, r, &pst) == 0 && pst == MS_OK) {
            }
            *out = MS_EOS;
            return 0;
        }
        *out = st;
        return 0;
    }
    fprintf(stderr, "pump exceeded steps nau=%d\n", r->nau);
    return -1;
}

int main(void)
{
    ms_session *s = NULL;
    ms_hls *h = NULL;
    ms_hls_fetch fe;
    xf x;
    rec r;
    ms_status ost;
    ms_synth_buf ts;
    ms_synth_stream st;
    size_t off[5];
    uint8_t a0[64], a1[64], a2[64];
    size_t l0, l1, l2;
    char pl[512];
    ms_hls_mem_item items[6];
    int i;

    printf("repro_hls_miss: compiler/opt matrix isolated miss+skip+disc\n");
    if (make_adts(a0, sizeof(a0), &l0, 3, 2, 16, 0xA0) != 0 ||
        make_adts(a1, sizeof(a1), &l1, 3, 2, 16, 0xA1) != 0 ||
        make_adts(a2, sizeof(a2), &l2, 3, 2, 16, 0xA2) != 0) {
        fprintf(stderr, "adts failed\n");
        return 1;
    }
    memset(&ts, 0, sizeof(ts));
    ms_synth_reset_cc();
    CHECK(ms_synth_init(&ts) == 0, "init");
    memset(&st, 0, sizeof(st));
    st.stream_type = 0x0f;
    st.pid = 0x100;
    CHECK(ms_synth_pat_pmt(&ts, 1, 0x1000, &st, 1, 0, 188) == 0, "pat");
    off[0] = 0;
    {
        uint8_t *fr[3];
        size_t fl[3];
        fr[0] = a0;
        fr[1] = a1;
        fr[2] = a2;
        fl[0] = l0;
        fl[1] = l1;
        fl[2] = l2;
        for (i = 0; i < 3; i++) {
            CHECK(ms_synth_pes(&ts, 0x100, 0xc0, 1, (uint64_t)i * 1920ull,
                               fr[i], fl[i], 188) == 0,
                  "pes");
            off[i + 1] = ts.len;
        }
    }
    snprintf(pl, sizeof(pl),
             "#EXTM3U\n#EXT-X-TARGETDURATION:2\n#EXT-X-MEDIA-SEQUENCE:1\n"
             "#EXTINF:1.0,\nmem://hls/s1.ts\n"
             "#EXTINF:1.0,\nmem://hls/missing.ts\n"
             "#EXTINF:1.0,\nmem://hls/s3.ts\n"
             "#EXT-X-ENDLIST\n");
    memset(items, 0, sizeof(items));
    items[0].uri = "mem://hls/miss.m3u8";
    items[0].data = (const uint8_t *)pl;
    items[0].size = strlen(pl);
    items[1].uri = "mem://hls/s1.ts";
    items[1].data = ts.data + off[0];
    items[1].size = off[1] - off[0];
    items[2].uri = "mem://hls/s3.ts";
    items[2].data = ts.data + off[2];
    items[2].size = off[3] - off[2];

    CHECK_ST(ms_session_create(NULL, NULL, &s), MS_OK);
    CHECK_ST(ms_hls_create(NULL, NULL, &h), MS_OK);
    memset(&x, 0, sizeof(x));
    x.items = items;
    x.nitems = 3;
    memset(&fe, 0, sizeof(fe));
    fe.struct_size = sizeof(fe);
    fe.ctx = &x;
    fe.request_fn = xf_request;
    fe.read_fn = xf_read;
    fe.cancel_fn = xf_cancel;
    CHECK_ST(ms_hls_bind(h, s, &fe), MS_OK);
    CHECK_ST(ms_hls_open(h, "mem://hls/miss.m3u8"), MS_OK);
    memset(&r, 0, sizeof(r));
    CHECK(pump(h, s, &r, 4000, &ost) == 0, "pump");
    printf("RESULT ost=%s nau=%d ndisc=%d nfmt=%d ncfg=%d nreq=%d keys:",
           ms_status_name(ost), r.nau, r.ndisc, r.nfmt, r.ncfg, x.nrequests);
    for (i = 0; i < x.nkeys; i++) {
        printf(" [%s]=%d", x.key_uri[i], x.nreq_by[i]);
    }
    printf("\n");
    fflush(stdout);
    ms_hls_destroy(h);
    ms_session_destroy(s);
    ms_synth_free(&ts);
    if (ost != MS_EOS || r.nau != 2 || r.ndisc < 1) {
        fprintf(stderr, "FAIL expected EOS nau=2 ndisc>=1\n");
        return 1;
    }
    printf("repro_hls_miss ok\n");
    return failed ? 1 : 0;
}
