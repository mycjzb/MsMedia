/*
 * P1a 会话与 MPEG-TS PAT/PMT，P1b AAC ADTS 子集，P1c/P1d H.264 Annex B
 *（AUD 或无 AUD 的 7.4.1.2.4 主图像边界子集），P1e 独立组装上限与
 * 跨轨动态总预算。
 * 不把 PES 或单个 NAL 当成访问单元；覆盖范围以声明的子集为准。
 * by: Long <mycjzb@gmail.com>
 */

#include "msmedia/msmedia.h"
#include "ms_internal.h"
#include "ms_ts_synth.h"
#include "ms_h264.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failed;

#define CHECK(cond, msg)                                                 \
    do {                                                                 \
        if (!(cond)) {                                                   \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, msg); \
            g_failed = 1;                                                \
            return -1;                                                   \
        }                                                                \
    } while (0)

#define CHECK_ST(st, exp)                                                          \
    do {                                                                           \
        ms_status _s = (st);                                                       \
        ms_status _e = (exp);                                                      \
        if (_s != _e) {                                                            \
            fprintf(stderr, "FAIL %s:%d: status %s expected %s\n", __FILE__,       \
                    __LINE__, ms_status_name(_s), ms_status_name(_e));             \
            g_failed = 1;                                                          \
            return -1;                                                             \
        }                                                                          \
    } while (0)

typedef struct test_alloc {
    size_t now;
    size_t peak;
    size_t allocs;
    size_t fail_after; /* 0 = 不注入失败；否则第 N 次起失败 */
} test_alloc;

static void *test_alloc_fn(void *ctx, size_t size)
{
    test_alloc *t = (test_alloc *)ctx;
    void *p;
    t->allocs++;
    if (t->fail_after != 0 && t->allocs >= t->fail_after) {
        return NULL;
    }
    p = malloc(size);
    if (p == NULL) {
        return NULL;
    }
    t->now += size;
    if (t->now > t->peak) {
        t->peak = t->now;
    }
    return p;
}

static void test_free_fn(void *ctx, void *ptr)
{
    (void)ctx;
    free(ptr);
}

static int drain_until(ms_session *s, ms_event_type want_type, ms_event *found, int max_pull)
{
    int i;
    for (i = 0; i < max_pull; i++) {
        ms_event ev;
        ms_status st = ms_session_pull(s, &ev);
        if (st == MS_AGAIN) {
            return 1;
        }
        if (st != MS_OK) {
            return (int)st;
        }
        if (ev.type == want_type && found != NULL) {
            *found = ev;
            return 0;
        }
        CHECK_ST(ms_session_release(s, ev.release_id), MS_OK);
    }
    return 1;
}

static int make_basic_ts(ms_synth_buf *b, uint16_t psz)
{
    ms_synth_stream st[2];
    memset(st, 0, sizeof(st));
    st[0].stream_type = 0x1B;
    st[0].pid = 0x101;
    st[1].stream_type = 0x0F;
    st[1].pid = 0x102;
    st[1].language[0] = 'e';
    st[1].language[1] = 'n';
    st[1].language[2] = 'g';
    ms_synth_reset_cc();
    return ms_synth_pat_pmt(b, 1, 0x100, st, 2, 8, psz);
}

static int test_abi_and_registry(void)
{
    ms_recog_level lv = MS_RECOG_AU;
    int hw = 1, ver = 1;
    const char *be = NULL, *detail = NULL;

    CHECK(ms_abi_version() == MSMEDIA_ABI_VERSION, "abi version");
    CHECK_ST(ms_codec_query(MS_CODEC_H264, &lv), MS_OK);
    CHECK(lv == MS_RECOG_AU, "h264 Annex B AUD-subset framing, not all H.264");
    CHECK_ST(ms_codec_query(MS_CODEC_AAC, &lv), MS_OK);
    CHECK(lv == MS_RECOG_AU, "aac ADTS subset framing, not all AAC");
    CHECK_ST(ms_codec_query(MS_CODEC_HEVC, &lv), MS_OK);
    CHECK(lv == MS_RECOG_CONTAINER_ID, "hevc id mapping exists");
    CHECK_ST(ms_codec_query(MS_CODEC_AVS_FAMILY, &lv), MS_OK);
    CHECK(lv == MS_RECOG_CONTAINER_ID, "avs family id");
    CHECK_ST(ms_codec_query(MS_CODEC_AVS_PLUS, &lv), MS_OK);
    CHECK(lv == MS_RECOG_NONE, "avs+ not confirmed in this build");
    CHECK_ST(ms_codec_query(MS_CODEC_MP3, &lv), MS_OK);
    CHECK(lv == MS_RECOG_NONE, "mp3 not confirmed from stream_type");
    CHECK_ST(ms_codec_query(MS_CODEC_MP2, &lv), MS_OK);
    CHECK(lv == MS_RECOG_NONE, "mp2 none");
    CHECK_ST(ms_codec_query(MS_CODEC_H263, &lv), MS_OK);
    CHECK(lv == MS_RECOG_NONE, "h263 none");
    CHECK_ST(ms_codec_query(MS_CODEC_AV1, &lv), MS_OK);
    CHECK(lv == MS_RECOG_NONE, "av1 none");
    CHECK_ST(ms_codec_query(MS_CODEC_OPUS, &lv), MS_OK);
    CHECK(lv == MS_RECOG_NONE, "opus none");
    CHECK_ST(ms_codec_query(MS_CODEC_G711_ALAW, &lv), MS_OK);
    CHECK(lv == MS_RECOG_NONE, "g711 none");
    CHECK_ST(ms_codec_query((ms_codec)1234, &lv), MS_ERR_INVALID_ARG);
    CHECK_ST(ms_hw_query(MS_CODEC_H264, &hw, &ver, &be, &detail), MS_OK);
    CHECK(hw == 0 && ver == 0, "hw query not hardware");
    CHECK(be != NULL && strcmp(be, "none") == 0, "hw backend none");
    CHECK_ST(ms_codec_query(MS_CODEC_H264, NULL), MS_ERR_INVALID_ARG);
    printf("  test_abi_and_registry ok\n");
    return 0;
}

static int test_limits_and_args(void)
{
    ms_session *s = NULL;
    ms_session_limits lim;
    size_t consumed = 99;
    uint8_t buf[32];

    CHECK_ST(ms_session_create(NULL, NULL, NULL), MS_ERR_INVALID_ARG);
    ms_session_limits_default(&lim);
    lim.probe_bytes = lim.probe_bytes + 1;
    CHECK_ST(ms_session_create(NULL, &lim, &s), MS_ERR_INVALID_ARG);
    CHECK(s == NULL, "no session on bad limits");
    ms_session_limits_default(&lim);
    lim.out_slots = 1;
    CHECK_ST(ms_session_create(NULL, &lim, &s), MS_OK);
    CHECK_ST(ms_session_push(s, buf, sizeof(buf), NULL), MS_ERR_INVALID_ARG);
    CHECK_ST(ms_session_push(s, NULL, 4, &consumed), MS_ERR_INVALID_ARG);
    consumed = 99;
    CHECK_ST(ms_session_push(s, buf, 0, &consumed), MS_OK);
    CHECK(consumed == 0, "empty push");
    CHECK_ST(ms_session_seek(s, 0), MS_ERR_NOT_SEEKABLE);
    CHECK_ST(ms_session_set_rate(s, 2, 1), MS_OK);
    CHECK_ST(ms_session_set_rate(s, 1, 0), MS_ERR_INVALID_ARG);
    CHECK_ST(ms_session_select_track(s, 1, 1), MS_ERR_UNSUPPORTED);
    ms_session_destroy(s);
    ms_session_destroy(NULL);
    printf("  test_limits_and_args ok\n");
    return 0;
}

static int expect_tracks_h264_aac(ms_session *s, uint16_t psz)
{
    ms_event ev;
    int got_fmt = 0, got_v = 0, got_a = 0;
    int i;
    for (i = 0; i < 16; i++) {
        ms_status st = ms_session_pull(s, &ev);
        if (st == MS_AGAIN) {
            break;
        }
        CHECK_ST(st, MS_OK);
        if (ev.type == MS_EVENT_FORMAT) {
            CHECK(ev.format == MS_FORMAT_MPEG_TS, "format ts");
            CHECK(ev.ts_packet_size == psz, "packet size");
            got_fmt = 1;
        } else if (ev.type == MS_EVENT_TRACK && ev.track.codec == MS_CODEC_H264) {
            CHECK(ev.track.level == MS_RECOG_CONTAINER_ID, "h264 level");
            CHECK(ev.track.kind == MS_TRACK_VIDEO, "h264 kind");
            CHECK(ev.track.stream_type == 0x1B, "h264 stype");
            CHECK(ev.track.id == 0x101, "h264 pid");
            CHECK(ev.track.selected == 1, "first video selected");
            got_v = 1;
        } else if (ev.type == MS_EVENT_TRACK && ev.track.codec == MS_CODEC_AAC) {
            CHECK(ev.track.level == MS_RECOG_CONTAINER_ID, "aac level");
            CHECK(ev.track.kind == MS_TRACK_AUDIO, "aac kind");
            CHECK(ev.track.stream_type == 0x0F, "aac stype");
            CHECK(ev.track.language != NULL && strcmp(ev.track.language, "eng") == 0,
                  "aac lang");
            CHECK(ev.track.selected == 1, "first audio selected");
            got_a = 1;
        } else {
            CHECK(0, "unexpected event");
        }
        CHECK_ST(ms_session_release(s, ev.release_id), MS_OK);
    }
    CHECK(got_fmt && got_v && got_a, "format + h264 + aac tracks");
    return 0;
}

static int push_all(ms_session *s, const uint8_t *data, size_t len, size_t chunk)
{
    size_t off = 0;
    if (chunk == 0) {
        chunk = len;
    }
    while (off < len) {
        size_t n = (len - off < chunk) ? (len - off) : chunk;
        size_t consumed = 0;
        ms_status st = ms_session_push(s, data + off, n, &consumed);
        CHECK_ST(st, MS_OK);
        CHECK(consumed == n, "full consume");
        off += n;
    }
    return 0;
}

static int test_ts_detect_188_and_192_and_fragments(void)
{
    ms_synth_buf b;
    ms_session *s = NULL;
    uint16_t psz;
    const size_t chunks[] = {1, 17, 187, 188, 200, 0};
    size_t ci;

    for (psz = 188; psz <= 192; psz += 4) {
        for (ci = 0; ci < sizeof(chunks) / sizeof(chunks[0]); ci++) {
            size_t chunk = chunks[ci];
            ms_synth_init(&b);
            CHECK(make_basic_ts(&b, psz) == 0, "synth");
            CHECK_ST(ms_session_create(NULL, NULL, &s), MS_OK);
            if (chunk == 0) {
                size_t c = 0;
                CHECK_ST(ms_session_push(s, b.data, b.len, &c), MS_OK);
                CHECK(c == b.len, "one-shot consume");
            } else if (push_all(s, b.data, b.len, chunk) != 0) {
                return -1;
            }
            if (expect_tracks_h264_aac(s, psz) != 0) {
                return -1;
            }
            ms_session_destroy(s);
            s = NULL;
            ms_synth_free(&b);
        }
    }
    printf("  test_ts_detect_188_and_192_and_fragments ok\n");
    return 0;
}

static int test_prefix_garbage(void)
{
    ms_synth_buf b;
    ms_session *s = NULL;
    uint8_t pfx[100];
    memset(pfx, 0x11, sizeof(pfx));
    ms_synth_init(&b);
    CHECK(make_basic_ts(&b, 188) == 0, "synth");
    CHECK(ms_synth_prepend(&b, pfx, sizeof(pfx)) == 0, "prefix");
    CHECK_ST(ms_session_create(NULL, NULL, &s), MS_OK);
    if (push_all(s, b.data, b.len, 33) != 0) {
        return -1;
    }
    if (expect_tracks_h264_aac(s, 188) != 0) {
        return -1;
    }
    ms_session_destroy(s);
    ms_synth_free(&b);
    printf("  test_prefix_garbage ok\n");
    return 0;
}

static int test_unknown_and_malformed(void)
{
    ms_session *s = NULL;
    ms_session_limits lim;
    uint8_t junk[1024];
    size_t c = 0;
    ms_synth_buf b;
    ms_event ev;

    memset(junk, 0xA5, sizeof(junk));
    ms_session_limits_default(&lim);
    lim.probe_bytes = 1024;
    CHECK_ST(ms_session_create(NULL, &lim, &s), MS_OK);
    CHECK_ST(ms_session_push(s, junk, sizeof(junk), &c), MS_ERR_UNKNOWN);
    CHECK_ST(ms_session_push(s, junk, 8, &c), MS_ERR_UNKNOWN);
    ms_session_destroy(s);

    ms_synth_init(&b);
    CHECK(make_basic_ts(&b, 188) == 0, "synth");
    /* 破坏 PAT section 内容使 CRC 失败；保持 0x47 同步。 */
    ms_synth_xor(&b, 8, 0xFF);
    CHECK_ST(ms_session_create(NULL, NULL, &s), MS_OK);
    CHECK_ST(ms_session_push(s, b.data, b.len, &c), MS_ERR_MALFORMED);
    ms_session_destroy(s);
    ms_synth_free(&b);

    ms_synth_init(&b);
    CHECK(make_basic_ts(&b, 188) == 0, "synth");
    /* 将首包 adaptation_field_control 改为保留值 00。 */
    b.data[3] = (uint8_t)(b.data[3] & 0xCFu);
    CHECK_ST(ms_session_create(NULL, NULL, &s), MS_OK);
    CHECK_ST(ms_session_push(s, b.data, b.len, &c), MS_ERR_MALFORMED);
    ms_session_destroy(s);
    ms_synth_free(&b);

    /* 加扰的 PMT 不应被当成明文解析，因此只有 FORMAT、没有轨道。 */
    ms_synth_init(&b);
    CHECK(make_basic_ts(&b, 188) == 0, "synth");
    /* 第二包通常是 PMT（首包 PAT）。设置加扰位。 */
    CHECK(b.len >= 188 * 2 && b.data[188] == 0x47, "pmt packet");
    b.data[188 + 3] = (uint8_t)(b.data[188 + 3] | 0x80);
    CHECK_ST(ms_session_create(NULL, NULL, &s), MS_OK);
    CHECK_ST(ms_session_push(s, b.data, b.len, &c), MS_OK);
    CHECK(drain_until(s, MS_EVENT_FORMAT, &ev, 8) == 0, "format only");
    CHECK_ST(ms_session_release(s, ev.release_id), MS_OK);
    CHECK_ST(ms_session_pull(s, &ev), MS_AGAIN);
    ms_session_destroy(s);
    ms_synth_free(&b);

    printf("  test_unknown_and_malformed ok\n");
    return 0;
}

static int test_reset_epoch_slots_cancel(void)
{
    ms_synth_buf b;
    ms_session *s = NULL;
    ms_session_limits lim;
    ms_event ev1, ev2;
    ms_epochs ep1, ep2;
    uint32_t old_id;
    size_t c = 0;

    ms_synth_init(&b);
    CHECK(make_basic_ts(&b, 188) == 0, "synth");
    ms_session_limits_default(&lim);
    lim.out_slots = 1;
    CHECK_ST(ms_session_create(NULL, &lim, &s), MS_OK);
    CHECK_ST(ms_session_push(s, b.data, b.len, &c), MS_OK);
    CHECK_ST(ms_session_epochs(s, &ep1), MS_OK);
    CHECK(ep1.session_epoch == 1, "start epoch 1");
    CHECK_ST(ms_session_pull(s, &ev1), MS_OK);
    CHECK(ev1.type == MS_EVENT_FORMAT, "first event format");
    CHECK_ST(ms_session_pull(s, &ev2), MS_LIMIT);
    CHECK_ST(ms_session_release(s, ev1.release_id), MS_OK);
    CHECK_ST(ms_session_pull(s, &ev1), MS_OK);
    CHECK(ev1.type == MS_EVENT_TRACK, "track after release");
    old_id = ev1.release_id;

    CHECK_ST(ms_session_reset(s), MS_OK);
    CHECK_ST(ms_session_epochs(s, &ep2), MS_OK);
    CHECK(ep2.session_epoch == ep1.session_epoch + 1, "epoch increased");
    CHECK_ST(ms_session_release(s, old_id), MS_ERR_STATE);
    CHECK_ST(ms_session_pull(s, &ev2), MS_AGAIN);

    CHECK_ST(ms_session_push(s, b.data, b.len, &c), MS_OK);
    if (expect_tracks_h264_aac(s, 188) != 0) {
        return -1;
    }

    CHECK_ST(ms_session_cancel(s), MS_OK);
    CHECK_ST(ms_session_push(s, b.data, 188, &c), MS_ERR_CANCELED);
    CHECK_ST(ms_session_pull(s, &ev1), MS_ERR_CANCELED);
    CHECK_ST(ms_session_reset(s), MS_OK);
    CHECK_ST(ms_session_push(s, b.data, b.len, &c), MS_OK);

    ms_session_destroy(s);
    ms_synth_free(&b);
    printf("  test_reset_epoch_slots_cancel ok\n");
    return 0;
}

static int test_cross_session_and_eos(void)
{
    ms_synth_buf a, b;
    ms_session *s1 = NULL, *s2 = NULL;
    ms_synth_stream st[1];
    size_t c = 0;
    ms_event ev;

    memset(st, 0, sizeof(st));
    st[0].stream_type = 0x24;
    st[0].pid = 0x201;
    ms_synth_init(&a);
    ms_synth_init(&b);
    CHECK(make_basic_ts(&a, 188) == 0, "synth a");
    ms_synth_reset_cc();
    CHECK(ms_synth_pat_pmt(&b, 2, 0x200, st, 1, 8, 188) == 0, "synth b");
    CHECK_ST(ms_session_create(NULL, NULL, &s1), MS_OK);
    CHECK_ST(ms_session_create(NULL, NULL, &s2), MS_OK);
    CHECK_ST(ms_session_push(s1, a.data, a.len, &c), MS_OK);
    CHECK_ST(ms_session_push(s2, b.data, b.len, &c), MS_OK);
    if (expect_tracks_h264_aac(s1, 188) != 0) {
        return -1;
    }
    CHECK(drain_until(s2, MS_EVENT_TRACK, &ev, 8) == 0, "s2 track");
    CHECK(ev.track.codec == MS_CODEC_HEVC, "s2 hevc id only");
    CHECK(ev.track.level == MS_RECOG_CONTAINER_ID, "s2 level");
    CHECK(ev.track.id == 0x201, "s2 pid");
    CHECK_ST(ms_session_release(s2, ev.release_id), MS_OK);

    CHECK_ST(ms_session_end_of_input(s1), MS_OK);
    CHECK(drain_until(s1, MS_EVENT_EOS, &ev, 8) == 0, "eos");
    CHECK_ST(ms_session_release(s1, ev.release_id), MS_OK);
    CHECK_ST(ms_session_pull(s1, &ev), MS_EOS);

    ms_session_destroy(s1);
    ms_session_destroy(s2);
    ms_synth_free(&a);
    ms_synth_free(&b);
    printf("  test_cross_session_and_eos ok\n");
    return 0;
}

static int test_avs_family_not_plus(void)
{
    ms_synth_buf b;
    ms_session *s = NULL;
    ms_synth_stream st[1];
    ms_event ev;
    size_t c = 0;

    memset(st, 0, sizeof(st));
    st[0].stream_type = 0x42;
    st[0].pid = 0x111;
    ms_synth_init(&b);
    ms_synth_reset_cc();
    CHECK(ms_synth_pat_pmt(&b, 1, 0x110, st, 1, 8, 188) == 0, "synth avs");
    CHECK_ST(ms_session_create(NULL, NULL, &s), MS_OK);
    CHECK_ST(ms_session_push(s, b.data, b.len, &c), MS_OK);
    CHECK(drain_until(s, MS_EVENT_TRACK, &ev, 8) == 0, "track");
    CHECK(ev.track.codec == MS_CODEC_AVS_FAMILY, "family not plus");
    CHECK(ev.track.codec != MS_CODEC_AVS_PLUS, "not avs+");
    CHECK(ev.track.level == MS_RECOG_CONTAINER_ID, "id only");
    CHECK_ST(ms_session_release(s, ev.release_id), MS_OK);
    ms_session_destroy(s);
    ms_synth_free(&b);
    printf("  test_avs_family_not_plus ok\n");
    return 0;
}

static int test_max_push_and_allocator(void)
{
    ms_session *s = NULL;
    ms_session_limits lim;
    ms_allocator a;
    test_alloc ta;
    size_t c = 0;
    uint8_t buf[64];

    ms_session_limits_default(&lim);
    lim.max_push_bytes = 16;
    CHECK_ST(ms_session_create(NULL, &lim, &s), MS_OK);
    memset(buf, 0, sizeof(buf));
    CHECK_ST(ms_session_push(s, buf, 17, &c), MS_ERR_INVALID_ARG);
    CHECK(c == 0, "oversize push not consumed");
    CHECK_ST(ms_session_push(s, buf, 16, &c), MS_OK);
    ms_session_destroy(s);

    memset(&ta, 0, sizeof(ta));
    ta.fail_after = 1;
    a.alloc_fn = test_alloc_fn;
    a.free_fn = test_free_fn;
    a.ctx = &ta;
    CHECK_ST(ms_session_create(&a, NULL, &s), MS_ERR_NOMEM);
    CHECK(s == NULL, "alloc fail");
    printf("  test_max_push_and_allocator ok\n");
    return 0;
}

static int test_pes_extract_not_au(void)
{
    ms_synth_buf b;
    ms_session *s = NULL;
    const uint8_t payload[] = {0x00, 0x00, 0x00, 0x01, 0x09, 0x10, 'T', 'E', 'S', 'T'};
    ms_pes_view pes;
    ms_event ev;
    size_t c = 0;
    int saw_au = 0;
    int i;

    ms_synth_init(&b);
    CHECK(make_basic_ts(&b, 188) == 0, "synth");
    CHECK(ms_synth_pes(&b, 0x101, 0xE0, 1, 90000, payload, sizeof(payload), 188) == 0,
          "pes");
    CHECK_ST(ms_session_create(NULL, NULL, &s), MS_OK);
    CHECK_ST(ms_session_push(s, b.data, b.len, &c), MS_OK);
    for (i = 0; i < 16; i++) {
        ms_status st = ms_session_pull(s, &ev);
        if (st == MS_AGAIN) {
            break;
        }
        CHECK_ST(st, MS_OK);
        if (ev.type == MS_EVENT_AU || ev.type == MS_EVENT_CODEC_CONFIG) {
            saw_au = 1;
        }
        CHECK_ST(ms_session_release(s, ev.release_id), MS_OK);
    }
    CHECK(saw_au == 0, "AUD without a primary picture is not an AU");
    CHECK_ST(ms_session_debug_pull_pes(s, &pes), MS_OK);
    CHECK(pes.pid == 0x101, "pes pid");
    CHECK(pes.has_pts == 1, "has pts");
    CHECK(pes.pts == 90000, "pts 1s");
    CHECK(pes.payload_size == sizeof(payload), "payload size");
    CHECK(pes.payload != NULL && memcmp(pes.payload, payload, sizeof(payload)) == 0,
          "payload bytes");
    /* 指针由会话持有；reset 后不得再使用。本测试在 reset 前不再解引用。 */
    CHECK_ST(ms_session_reset(s), MS_OK);
    CHECK_ST(ms_session_debug_pull_pes(s, &pes), MS_AGAIN);
    ms_session_destroy(s);
    ms_synth_free(&b);
    printf("  test_pes_extract_not_au ok\n");
    return 0;
}


static int make_adts(uint8_t *out, size_t cap, size_t *len, int sfi, int ch, int nbytes, uint8_t fill)
{
    uint8_t raw[512];
    if (nbytes > (int)sizeof(raw)) {
        return -1;
    }
    memset(raw, fill, (size_t)nbytes);
    return ms_synth_adts_frame(out, cap, len, 1 /* LC */, sfi, ch, raw, (size_t)nbytes);
}

static int test_aac_adts_basic_and_pts(void)
{
    ms_synth_buf b;
    ms_session *s = NULL;
    uint8_t frame[128];
    size_t flen = 0;
    size_t c = 0;
    ms_event ev;
    int saw_cfg = 0, saw_au = 0, saw_update = 0;
    int i;

    CHECK(make_adts(frame, sizeof(frame), &flen, 3 /* 48 kHz */, 2, 16, 0xA5) == 0, "adts");
    ms_synth_init(&b);
    CHECK(make_basic_ts(&b, 188) == 0, "ts");
    CHECK(ms_synth_pes(&b, 0x102, 0xC0, 1, 90000, frame, flen, 188) == 0, "pes");
    CHECK_ST(ms_session_create(NULL, NULL, &s), MS_OK);
    CHECK_ST(ms_session_push(s, b.data, b.len, &c), MS_OK);
    for (i = 0; i < 24; i++) {
        ms_status st = ms_session_pull(s, &ev);
        if (st == MS_AGAIN) {
            break;
        }
        CHECK_ST(st, MS_OK);
        if (ev.type == MS_EVENT_TRACK_UPDATE && ev.track.codec == MS_CODEC_AAC) {
            CHECK(ev.track.level == MS_RECOG_AU, "track upgraded to AU");
            saw_update = 1;
        } else if (ev.type == MS_EVENT_CODEC_CONFIG) {
            CHECK(ev.track.codec == MS_CODEC_AAC, "cfg codec");
            CHECK(ev.track.level == MS_RECOG_AU, "cfg level");
            CHECK(ev.data_size == 2, "asc size");
            CHECK(ev.data != NULL && ev.data[0] == 0x11 && ev.data[1] == 0x90, "asc 48k stereo LC");
            saw_cfg = 1;
        } else if (ev.type == MS_EVENT_AU) {
            CHECK(ev.track.codec == MS_CODEC_AAC, "au codec");
            CHECK(ev.has_pts == 1 && ev.pts == 90000, "au pts");
            CHECK(ev.time_base_num == 1 && ev.time_base_den == 90000, "90k");
            CHECK(ev.is_keyframe == 1, "aac keyframe flag");
            CHECK(ev.data_size == flen && ev.data != NULL, "au size");
            CHECK(memcmp(ev.data, frame, flen) == 0, "au bytes include header");
            saw_au = 1;
        }
        CHECK_ST(ms_session_release(s, ev.release_id), MS_OK);
    }
    CHECK(saw_cfg && saw_au, "config + au");
    CHECK(saw_update, "level upgrade");
    ms_session_destroy(s);
    ms_synth_free(&b);
    printf("  test_aac_adts_basic_and_pts ok\n");
    return 0;
}

static int test_aac_fragments_and_cross_ts(void)
{
    ms_synth_buf b;
    ms_session *s = NULL;
    uint8_t frame[300];
    size_t flen = 0;
    int au = 0;

    CHECK(make_adts(frame, sizeof(frame), &flen, 3, 2, 200, 0x11) == 0, "long adts");
    CHECK(flen > 184, "spans ts packet");
    ms_synth_init(&b);
    CHECK(make_basic_ts(&b, 188) == 0, "ts");
    CHECK(ms_synth_pes(&b, 0x102, 0xC0, 1, 45000, frame, flen, 188) == 0, "pes");
    CHECK_ST(ms_session_create(NULL, NULL, &s), MS_OK);
    if (push_all(s, b.data, b.len, 1) != 0) { /* 逐字节 */
        return -1;
    }
    for (;;) {
        ms_event ev;
        ms_status st = ms_session_pull(s, &ev);
        if (st == MS_AGAIN) {
            break;
        }
        CHECK_ST(st, MS_OK);
        if (ev.type == MS_EVENT_AU) {
            CHECK(ev.data_size == flen, "cross-boundary size");
            CHECK(ev.has_pts && ev.pts == 45000, "pts kept");
            au++;
        }
        CHECK_ST(ms_session_release(s, ev.release_id), MS_OK);
    }
    CHECK(au == 1, "one au from fragmented push");
    ms_session_destroy(s);
    ms_synth_free(&b);
    printf("  test_aac_fragments_and_cross_ts ok\n");
    return 0;
}

static int test_aac_two_au_one_pes_and_split_pes(void)
{
    ms_synth_buf b;
    ms_session *s = NULL;
    uint8_t a1[64], a2[64], both[128];
    size_t l1 = 0, l2 = 0;
    uint8_t first_half[32], second_half[64];
    size_t c = 0;
    int n_au = 0;
    uint64_t pts[4];
    size_t sizes[4];

    CHECK(make_adts(a1, sizeof(a1), &l1, 3, 2, 8, 0x21) == 0, "f1");
    CHECK(make_adts(a2, sizeof(a2), &l2, 3, 2, 8, 0x22) == 0, "f2");
    memcpy(both, a1, l1);
    memcpy(both + l1, a2, l2);

    /* 一个 PES 里两帧：第二帧 PTS = 90000 + 1024*90000/48000 = 91920 */
    ms_synth_init(&b);
    CHECK(make_basic_ts(&b, 188) == 0, "ts");
    CHECK(ms_synth_pes(&b, 0x102, 0xC0, 1, 90000, both, l1 + l2, 188) == 0, "pes2");
    CHECK_ST(ms_session_create(NULL, NULL, &s), MS_OK);
    CHECK_ST(ms_session_push(s, b.data, b.len, &c), MS_OK);
    n_au = 0;
    for (;;) {
        ms_event ev;
        ms_status st = ms_session_pull(s, &ev);
        if (st == MS_AGAIN) {
            break;
        }
        CHECK_ST(st, MS_OK);
        if (ev.type == MS_EVENT_AU) {
            CHECK(n_au < 4, "au cap");
            pts[n_au] = ev.pts;
            sizes[n_au] = ev.data_size;
            CHECK(ev.has_pts == 1, "both have pts");
            n_au++;
        }
        CHECK_ST(ms_session_release(s, ev.release_id), MS_OK);
    }
    CHECK(n_au == 2, "two au");
    CHECK(sizes[0] == l1 && sizes[1] == l2, "sizes");
    CHECK(pts[0] == 90000 && pts[1] == 91920, "interpolated pts");
    ms_session_destroy(s);
    ms_synth_free(&b);

    /* 一帧拆到两个 PES：时间戳属于开始那一包 */
    CHECK(l1 > 8, "splitable");
    memcpy(first_half, a1, 8);
    memcpy(second_half, a1 + 8, l1 - 8);
    ms_synth_init(&b);
    CHECK(make_basic_ts(&b, 188) == 0, "ts");
    CHECK(ms_synth_pes(&b, 0x102, 0xC0, 1, 180000, first_half, 8, 188) == 0, "pes a");
    CHECK(ms_synth_pes(&b, 0x102, 0xC0, 1, 270000, second_half, l1 - 8, 188) == 0, "pes b");
    CHECK_ST(ms_session_create(NULL, NULL, &s), MS_OK);
    CHECK_ST(ms_session_push(s, b.data, b.len, &c), MS_OK);
    n_au = 0;
    for (;;) {
        ms_event ev;
        ms_status st = ms_session_pull(s, &ev);
        if (st == MS_AGAIN) {
            break;
        }
        CHECK_ST(st, MS_OK);
        if (ev.type == MS_EVENT_AU) {
            CHECK(ev.has_pts && ev.pts == 180000, "pts from starting pes, not the second");
            CHECK(ev.data_size == l1, "reassembled");
            n_au++;
        }
        CHECK_ST(ms_session_release(s, ev.release_id), MS_OK);
    }
    CHECK(n_au == 1, "one reassembled au");
    ms_session_destroy(s);
    ms_synth_free(&b);
    printf("  test_aac_two_au_one_pes_and_split_pes ok\n");
    return 0;
}

static int test_aac_eos_flush_reset_badlen_config_backpressure(void)
{
    ms_synth_buf b;
    ms_session *s = NULL;
    ms_session_limits lim;
    uint8_t a1[64], a2[64], a3[64], trunc[80], bad[16];
    size_t l1 = 0, l2 = 0, l3 = 0, blen = 0, c = 0;
    int n_au, n_cfg;
    ms_event ev;
    ms_epochs ep1, ep2;
    uint32_t old_id;

    CHECK(make_adts(a1, sizeof(a1), &l1, 3, 2, 8, 0x31) == 0, "a1");
    CHECK(make_adts(a2, sizeof(a2), &l2, 3, 1, 8, 0x32) == 0, "a2 config change mono");
    CHECK(make_adts(a3, sizeof(a3), &l3, 3, 2, 8, 0x33) == 0, "a3");

    /* EOS：完整一帧 + 半帧，半帧不得发出 */
    memcpy(trunc, a1, l1);
    memcpy(trunc + l1, a3, 4);
    ms_synth_init(&b);
    CHECK(make_basic_ts(&b, 188) == 0, "ts");
    CHECK(ms_synth_pes(&b, 0x102, 0xC0, 1, 1000, trunc, l1 + 4, 188) == 0, "pes trunc");
    CHECK_ST(ms_session_create(NULL, NULL, &s), MS_OK);
    CHECK_ST(ms_session_push(s, b.data, b.len, &c), MS_OK);
    CHECK_ST(ms_session_end_of_input(s), MS_OK);
    n_au = 0;
    for (;;) {
        ms_status st = ms_session_pull(s, &ev);
        if (st == MS_EOS) {
            break;
        }
        CHECK_ST(st, MS_OK);
        if (ev.type == MS_EVENT_AU) {
            CHECK(ev.data_size == l1, "only complete frame");
            n_au++;
        }
        CHECK_ST(ms_session_release(s, ev.release_id), MS_OK);
    }
    CHECK(n_au == 1, "truncated tail dropped");
    ms_session_destroy(s);
    ms_synth_free(&b);

    /* flush：丢掉半帧，保留轨道；随后完整帧仍可组 */
    ms_synth_init(&b);
    CHECK(make_basic_ts(&b, 188) == 0, "ts");
    CHECK(ms_synth_pes(&b, 0x102, 0xC0, 1, 2000, a1, 5, 188) == 0, "partial");
    CHECK_ST(ms_session_create(NULL, NULL, &s), MS_OK);
    CHECK_ST(ms_session_push(s, b.data, b.len, &c), MS_OK);
    CHECK_ST(ms_session_flush(s), MS_OK);
    CHECK_ST(ms_session_pull(s, &ev), MS_AGAIN);
    ms_synth_free(&b);
    ms_synth_init(&b);
    CHECK(make_basic_ts(&b, 188) == 0, "ts2");
    CHECK(ms_synth_pes(&b, 0x102, 0xC0, 1, 3000, a1, l1, 188) == 0, "full");
    CHECK_ST(ms_session_push(s, b.data, b.len, &c), MS_OK);
    n_au = 0;
    for (;;) {
        ms_status st = ms_session_pull(s, &ev);
        if (st == MS_AGAIN) {
            break;
        }
        CHECK_ST(st, MS_OK);
        if (ev.type == MS_EVENT_AU) {
            CHECK(ev.pts == 3000, "new frame after flush");
            n_au++;
        }
        CHECK_ST(ms_session_release(s, ev.release_id), MS_OK);
    }
    CHECK(n_au == 1, "au after flush");
    CHECK_ST(ms_session_epochs(s, &ep1), MS_OK);
    old_id = ev.release_id; /* last released, stale after reset */
    CHECK_ST(ms_session_reset(s), MS_OK);
    CHECK_ST(ms_session_epochs(s, &ep2), MS_OK);
    CHECK(ep2.session_epoch == ep1.session_epoch + 1, "reset epoch");
    CHECK_ST(ms_session_release(s, old_id), MS_ERR_STATE);
    ms_session_destroy(s);
    ms_synth_free(&b);

    /* 配置变化：立体声 -> 单声道，两次 config，第二 AU format_changed */
    ms_synth_init(&b);
    CHECK(make_basic_ts(&b, 188) == 0, "ts");
    CHECK(ms_synth_pes(&b, 0x102, 0xC0, 1, 4000, a1, l1, 188) == 0, "p1");
    CHECK(ms_synth_pes(&b, 0x102, 0xC0, 1, 5000, a2, l2, 188) == 0, "p2");
    CHECK_ST(ms_session_create(NULL, NULL, &s), MS_OK);
    CHECK_ST(ms_session_push(s, b.data, b.len, &c), MS_OK);
    n_cfg = 0;
    n_au = 0;
    for (;;) {
        ms_status st = ms_session_pull(s, &ev);
        if (st == MS_AGAIN) {
            break;
        }
        CHECK_ST(st, MS_OK);
        if (ev.type == MS_EVENT_CODEC_CONFIG) {
            n_cfg++;
        }
        if (ev.type == MS_EVENT_AU) {
            if (n_au == 1) {
                CHECK(ev.format_changed == 1, "second au after new config");
            }
            n_au++;
        }
        CHECK_ST(ms_session_release(s, ev.release_id), MS_OK);
    }
    CHECK(n_cfg == 2 && n_au == 2, "config change");
    ms_session_destroy(s);
    ms_synth_free(&b);

    /* 坏长度：同步后 frame_length 小于头长 */
    memset(bad, 0, sizeof(bad));
    bad[0] = 0xFF;
    bad[1] = 0xF1;
    bad[2] = (uint8_t)((1 << 6) | (3 << 2) | 0);
    bad[3] = (uint8_t)((2 << 6) | 0); /* length high 0 */
    bad[4] = 0x00;
    bad[5] = 0x1F; /* length 0 */
    bad[6] = 0xFC;
    blen = 7;
    ms_synth_init(&b);
    CHECK(make_basic_ts(&b, 188) == 0, "ts");
    CHECK(ms_synth_pes(&b, 0x102, 0xC0, 1, 6000, bad, blen, 188) == 0, "bad");
    CHECK_ST(ms_session_create(NULL, NULL, &s), MS_OK);
    CHECK_ST(ms_session_push(s, b.data, b.len, &c), MS_ERR_MALFORMED);
    ms_session_destroy(s);
    ms_synth_free(&b);

    /* 反压：缩小 AU 队列，第二帧不得丢；释放后再 pull 得到 */
    ms_session_limits_default(&lim);
    lim.au_queue_bytes = (l1 + 4); /* config 2 + 第一帧后几乎满 */
    ms_synth_init(&b);
    CHECK(make_basic_ts(&b, 188) == 0, "ts");
    {
        uint8_t both[160];
        memcpy(both, a1, l1);
        memcpy(both + l1, a3, l3);
        CHECK(ms_synth_pes(&b, 0x102, 0xC0, 1, 7000, both, l1 + l3, 188) == 0, "both");
    }
    CHECK_ST(ms_session_create(NULL, &lim, &s), MS_OK);
    {
        ms_status st = ms_session_push(s, b.data, b.len, &c);
        CHECK(st == MS_OK || st == MS_LIMIT, "push may backpressure");
    }
    n_au = 0;
    /* 先取走已发出的事件并释放，再继续 pull 触发 drain */
    for (;;) {
        ms_status st = ms_session_pull(s, &ev);
        if (st == MS_AGAIN) {
            break;
        }
        CHECK_ST(st, MS_OK);
        if (ev.type == MS_EVENT_AU) {
            n_au++;
        }
        CHECK_ST(ms_session_release(s, ev.release_id), MS_OK);
    }
    CHECK(n_au == 2, "no dropped au under backpressure");
    ms_session_destroy(s);
    ms_synth_free(&b);

    printf("  test_aac_eos_flush_reset_badlen_config_backpressure ok\n");
    return 0;
}


/* 手工 ADTS 头，便于覆盖 CRC / MPEG 版本 / 拒绝字段。不校验 CRC。 */
static int write_adts_header(uint8_t *out, size_t cap, size_t *len,
                             int id_mpeg2, int prot_absent, int profile,
                             int sfi, int ch, int rdb, size_t raw_len,
                             uint8_t fill)
{
    unsigned hlen = prot_absent ? 7u : 9u;
    size_t flen;

    if (len == NULL) {
        return -1;
    }
    *len = 0;
    if (raw_len > ((size_t)-1) - hlen) {
        return -1;
    }
    flen = (size_t)hlen + raw_len;
    if (out == NULL || flen > cap || flen > 0x1FFFu) {
        return -1;
    }
    out[0] = 0xFF;
    out[1] = (uint8_t)(0xF0 | (id_mpeg2 ? 0x08 : 0x00) | (prot_absent ? 0x01 : 0x00));
    out[2] = (uint8_t)(((profile & 0x3) << 6) | ((sfi & 0xF) << 2) | ((ch >> 2) & 0x1));
    out[3] = (uint8_t)(((ch & 0x3) << 6) | ((flen >> 11) & 0x3));
    out[4] = (uint8_t)((flen >> 3) & 0xFF);
    out[5] = (uint8_t)(((flen & 0x7) << 5) | 0x1F);
    out[6] = (uint8_t)(0xFC | (rdb & 0x3));
    if (!prot_absent) {
        out[7] = 0x00;
        out[8] = 0x00;
    }
    if (raw_len > 0) {
        memset(out + hlen, fill, raw_len);
    }
    *len = flen;
    return 0;
}

static int push_one_adts_session(const uint8_t *frame, size_t flen, uint64_t pts,
                                 ms_session **out_s, size_t *consumed)
{
    ms_synth_buf b;
    ms_session *s = NULL;
    size_t c = 0;

    ms_synth_init(&b);
    if (make_basic_ts(&b, 188) != 0 ||
        ms_synth_pes(&b, 0x102, 0xC0, 1, pts, frame, flen, 188) != 0) {
        ms_synth_free(&b);
        return -1;
    }
    if (ms_session_create(NULL, NULL, &s) != MS_OK) {
        ms_synth_free(&b);
        return -1;
    }
    {
        ms_status st = ms_session_push(s, b.data, b.len, &c);
        ms_synth_free(&b);
        if (consumed != NULL) {
            *consumed = c;
        }
        *out_s = s;
        return (int)st;
    }
}

static int count_au_and_destroy(ms_session *s, int *n_au, size_t *first_size)
{
    *n_au = 0;
    for (;;) {
        ms_event ev;
        ms_status st = ms_session_pull(s, &ev);
        if (st == MS_AGAIN) {
            break;
        }
        CHECK_ST(st, MS_OK);
        if (ev.type == MS_EVENT_AU) {
            if (*n_au == 0 && first_size != NULL) {
                *first_size = ev.data_size;
            }
            (*n_au)++;
        }
        CHECK_ST(ms_session_release(s, ev.release_id), MS_OK);
    }
    ms_session_destroy(s);
    return 0;
}

static int test_aac_adts_subset_crc_mpeg_version(void)
{
    uint8_t frame[64];
    size_t flen = 0, c = 0, ausz = 0;
    ms_session *s = NULL;
    int n_au = 0;
    int st;

    /* MPEG-2 ADTS（ID=1）与 MPEG-4 一样按完整帧组 AU，不另声称差异能力。 */
    CHECK(write_adts_header(frame, sizeof(frame), &flen, 1, 1, 1, 3, 2, 0, 8, 0x41) == 0,
          "mpeg2 hdr");
    st = push_one_adts_session(frame, flen, 8000, &s, &c);
    CHECK(st == (int)MS_OK, "mpeg2 push");
    CHECK(count_au_and_destroy(s, &n_au, &ausz) == 0, "mpeg2 drain");
    CHECK(n_au == 1 && ausz == flen, "mpeg2 one au");

    /* protection_absent=0：9 字节头（含 CRC 字段），只按头长组帧，不校验 CRC。 */
    CHECK(write_adts_header(frame, sizeof(frame), &flen, 0, 0, 1, 3, 2, 0, 8, 0x42) == 0,
          "crc hdr");
    st = push_one_adts_session(frame, flen, 8100, &s, &c);
    CHECK(st == (int)MS_OK, "crc push");
    CHECK(count_au_and_destroy(s, &n_au, &ausz) == 0, "crc drain");
    CHECK(n_au == 1 && ausz == flen, "crc 9-byte header au");

    /* raw_data_blocks!=0：合法 ADTS 但不在本子集，明确拒绝，不得当单块 1024 采样发出。 */
    CHECK(write_adts_header(frame, sizeof(frame), &flen, 0, 1, 1, 3, 2, 1, 8, 0x43) == 0,
          "rdb hdr");
    st = push_one_adts_session(frame, flen, 8200, &s, &c);
    CHECK(st == (int)MS_ERR_UNSUPPORTED, "rdb rejected");
    ms_session_destroy(s);

    /* channel_configuration=0（PCE）拒绝 */
    CHECK(write_adts_header(frame, sizeof(frame), &flen, 0, 1, 1, 3, 0, 0, 8, 0x44) == 0,
          "pce hdr");
    st = push_one_adts_session(frame, flen, 8300, &s, &c);
    CHECK(st == (int)MS_ERR_UNSUPPORTED, "channel_config 0 rejected");
    ms_session_destroy(s);

    /* sampling_frequency_index=15 拒绝 */
    CHECK(write_adts_header(frame, sizeof(frame), &flen, 0, 1, 1, 15, 2, 0, 8, 0x45) == 0,
          "sfi hdr");
    st = push_one_adts_session(frame, flen, 8400, &s, &c);
    CHECK(st == (int)MS_ERR_UNSUPPORTED, "sfi 15 rejected");
    ms_session_destroy(s);

    printf("  test_aac_adts_subset_crc_mpeg_version ok\n");
    return 0;
}


static int append_buf(uint8_t *out, size_t cap, size_t *len, const uint8_t *p, size_t n)
{
    if (out == NULL || len == NULL) {
        return -1;
    }
    if (*len > cap || n > cap - *len) {
        return -1;
    }
    if (n > 0) {
        memcpy(out + *len, p, n);
        *len += n;
    }
    return 0;
}

/* AUD + SPS + PPS + 条带。sc_v 用于条带起始码长度，其余用 sc_h。 */
static int make_h264_au(uint8_t *out, size_t cap, size_t *len,
                        int sc_h, int sc_v, int width, int height, int idr,
                        const uint8_t *slice, size_t slice_len)
{
    uint8_t tmp[400];
    size_t n = 0, piece = 0;
    if (len == NULL) {
        return -1;
    }
    *len = 0;
    if (ms_synth_h264_aud(tmp, sizeof(tmp), &piece, sc_h) != 0 ||
        append_buf(out, cap, len, tmp, piece) != 0) {
        return -1;
    }
    if (ms_synth_h264_sps(tmp, sizeof(tmp), &piece, sc_h, width, height, 1) != 0 ||
        append_buf(out, cap, len, tmp, piece) != 0) {
        return -1;
    }
    if (ms_synth_h264_pps(tmp, sizeof(tmp), &piece, sc_h) != 0 ||
        append_buf(out, cap, len, tmp, piece) != 0) {
        return -1;
    }
    if (ms_synth_h264_slice(tmp, sizeof(tmp), &piece, sc_v, idr, slice, slice_len) != 0 ||
        append_buf(out, cap, len, tmp, piece) != 0) {
        return -1;
    }
    (void)n;
    return 0;
}


static void h264_slice_opt_init(ms_synth_h264_slice_opts *o, int sc, int idr,
                                unsigned first_mb, unsigned frame_num)
{
    memset(o, 0, sizeof(*o));
    o->sc_len = sc;
    o->idr = idr;
    o->nal_ref_idc = -1;
    o->first_mb = first_mb;
    o->slice_type = idr ? 2u : 0u;
    o->pps_id = 0;
    o->frame_num = frame_num;
    o->log2_max_frame_num_minus4 = 0;
    o->poc_type = 2;
}

static int append_slice_opt(uint8_t *out, size_t cap, size_t *len,
                            const ms_synth_h264_slice_opts *o)
{
    uint8_t tmp[256];
    size_t piece = 0;
    if (ms_synth_h264_slice_hdr(tmp, sizeof(tmp), &piece, o) != 0) {
        return -1;
    }
    return append_buf(out, cap, len, tmp, piece);
}

static int push_video_payload_lim(const uint8_t *payload, size_t plen, uint64_t pts,
                                  int has_pts, ms_session **out_s, size_t *consumed,
                                  const ms_session_limits *lim)
{
    ms_synth_buf b;
    ms_session *s = NULL;
    size_t c = 0;
    ms_status st;

    ms_synth_init(&b);
    if (make_basic_ts(&b, 188) != 0 ||
        ms_synth_pes(&b, 0x101, 0xE0, has_pts, pts, payload, plen, 188) != 0) {
        ms_synth_free(&b);
        return -1;
    }
    if (ms_session_create(NULL, lim, &s) != MS_OK) {
        ms_synth_free(&b);
        return -1;
    }
    st = ms_session_push(s, b.data, b.len, &c);
    ms_synth_free(&b);
    if (consumed != NULL) {
        *consumed = c;
    }
    *out_s = s;
    return (int)st;
}

static int push_video_payload(const uint8_t *payload, size_t plen, uint64_t pts,
                              int has_pts, ms_session **out_s, size_t *consumed)
{
    return push_video_payload_lim(payload, plen, pts, has_pts, out_s, consumed, NULL);
}

static int test_h264_aud_basic_config_idr(void)
{
    uint8_t au[256];
    size_t alen = 0, c = 0, cfg_sz = 0;
    ms_session *s = NULL;
    int saw_cfg = 0, saw_au = 0, saw_update = 0, i;
    uint8_t cfg[256];

    CHECK(make_h264_au(au, sizeof(au), &alen, 4, 4, 16, 16, 1, NULL, 0) == 0, "idr au");
    CHECK(push_video_payload(au, alen, 90000, 1, &s, &c) == (int)MS_OK, "push");
    CHECK_ST(ms_session_end_of_input(s), MS_OK);
    for (i = 0; i < 32; i++) {
        ms_event ev;
        ms_status st = ms_session_pull(s, &ev);
        if (st == MS_AGAIN || st == MS_EOS) {
            break;
        }
        CHECK_ST(st, MS_OK);
        if (ev.type == MS_EVENT_TRACK_UPDATE && ev.track.codec == MS_CODEC_H264) {
            CHECK(ev.track.level == MS_RECOG_AU, "h264 upgraded to AU");
            saw_update = 1;
        } else if (ev.type == MS_EVENT_CODEC_CONFIG) {
            CHECK(ev.track.codec == MS_CODEC_H264, "cfg codec");
            CHECK(ev.data_size >= 8 && ev.data != NULL, "cfg size");
            CHECK(ev.data[0] == 0 && ev.data[1] == 0 && ev.data[2] == 0 && ev.data[3] == 1,
                  "cfg annexb prefix");
            CHECK(ev.data[4] == 0x67, "cfg starts with SPS");
            memcpy(cfg, ev.data, ev.data_size < sizeof(cfg) ? ev.data_size : sizeof(cfg));
            cfg_sz = ev.data_size;
            saw_cfg = 1;
        } else if (ev.type == MS_EVENT_AU) {
            CHECK(ev.track.codec == MS_CODEC_H264, "au codec");
            CHECK(ev.has_pts == 1 && ev.pts == 90000, "au pts");
            CHECK(ev.time_base_num == 1 && ev.time_base_den == 90000, "90k");
            CHECK(ev.is_keyframe == 1, "IDR keyframe");
            CHECK(ev.format_changed == 1, "config before first au");
            CHECK(ev.data_size == alen && ev.data != NULL, "au bytes");
            CHECK(memcmp(ev.data, au, alen) == 0, "au annexb intact");
            saw_au = 1;
        }
        CHECK_ST(ms_session_release(s, ev.release_id), MS_OK);
    }
    CHECK(saw_cfg && saw_au && saw_update, "config + idr au + level");
    CHECK(cfg_sz > 0, "saved cfg");
    ms_session_destroy(s);

    /* 带 EPB 的 SPS 仍能给出 config，不得因未去转义而误判为隔行。 */
    {
        uint8_t payload[512], sps[128], pps[64], aud[16], sl[32];
        size_t n = 0, l = 0;
        int n_cfg = 0, n_au = 0;
        CHECK(ms_synth_h264_aud(aud, sizeof(aud), &l, 4) == 0, "aud");
        CHECK(append_buf(payload, sizeof(payload), &n, aud, l) == 0, "a");
        CHECK(ms_synth_h264_sps_epb(sps, sizeof(sps), &l, 4) == 0, "sps epb");
        {
            int found = 0;
            size_t k;
            for (k = 0; k + 2 < l; k++) {
                if (sps[k] == 0 && sps[k + 1] == 0 && sps[k + 2] == 3) {
                    found = 1;
                    break;
                }
            }
            CHECK(found, "synthetic SPS contains EPB");
        }
        CHECK(append_buf(payload, sizeof(payload), &n, sps, l) == 0, "s");
        CHECK(ms_synth_h264_pps(pps, sizeof(pps), &l, 4) == 0, "pps");
        CHECK(append_buf(payload, sizeof(payload), &n, pps, l) == 0, "p");
        CHECK(ms_synth_h264_slice(sl, sizeof(sl), &l, 4, 1, NULL, 0) == 0, "sl");
        CHECK(append_buf(payload, sizeof(payload), &n, sl, l) == 0, "v");
        CHECK(push_video_payload(payload, n, 180000, 1, &s, &c) == (int)MS_OK, "epb push");
        CHECK_ST(ms_session_end_of_input(s), MS_OK);
        for (;;) {
            ms_event ev;
            ms_status st = ms_session_pull(s, &ev);
            if (st == MS_AGAIN || st == MS_EOS) {
                break;
            }
            CHECK_ST(st, MS_OK);
            if (ev.type == MS_EVENT_CODEC_CONFIG) {
                n_cfg++;
            }
            if (ev.type == MS_EVENT_AU) {
                CHECK(ev.is_keyframe == 1, "epb idr");
                n_au++;
            }
            CHECK_ST(ms_session_release(s, ev.release_id), MS_OK);
        }
        CHECK(n_cfg == 1 && n_au == 1, "EPB SPS parsed as progressive AUD AU");
        ms_session_destroy(s);
    }
    printf("  test_h264_aud_basic_config_idr ok\n");
    return 0;
}

static int test_h264_fragments_ts_pes_timestamps(void)
{
    uint8_t a1[512], a2[256], both[768], first[400], second[400];
    size_t l1 = 0, l2 = 0, n = 0, c = 0;
    uint8_t long_raw[200];
    ms_session *s = NULL;
    int n_au = 0;
    uint64_t pts[4];
    int has_pts[4];
    int kf[4];
    size_t split;

    memset(long_raw, 0x5A, sizeof(long_raw));
    /* 3 字节与 4 字节起始码混用，条带足够长以跨 TS 包。 */
    CHECK(make_h264_au(a1, sizeof(a1), &l1, 4, 3, 16, 16, 1, long_raw, sizeof(long_raw)) == 0,
          "au1");
    CHECK(l1 > 184, "spans ts");
    CHECK(make_h264_au(a2, sizeof(a2), &l2, 3, 4, 16, 16, 0, NULL, 0) == 0, "au2");
    memcpy(both, a1, l1);
    memcpy(both + l1, a2, l2);
    n = l1 + l2;

    CHECK(push_video_payload(both, n, 45000, 1, &s, &c) == (int)MS_OK, "two au pes");
    CHECK_ST(ms_session_end_of_input(s), MS_OK);
    n_au = 0;
    for (;;) {
        ms_event ev;
        ms_status st = ms_session_pull(s, &ev);
        if (st == MS_AGAIN || st == MS_EOS) {
            break;
        }
        CHECK_ST(st, MS_OK);
        if (ev.type == MS_EVENT_AU) {
            CHECK(n_au < 4, "cap");
            pts[n_au] = ev.pts;
            has_pts[n_au] = ev.has_pts;
            kf[n_au] = ev.is_keyframe;
            n_au++;
        }
        CHECK_ST(ms_session_release(s, ev.release_id), MS_OK);
    }
    CHECK(n_au == 2, "two au in one pes");
    CHECK(has_pts[0] == 1 && pts[0] == 45000, "first pts from pes");
    CHECK(has_pts[1] == 0, "second au must not invent a frame-rate pts");
    CHECK(kf[0] == 1 && kf[1] == 0, "IDR then non-IDR");
    ms_session_destroy(s);

    /* 逐字节推入第一帧 */
    CHECK(push_video_payload(a1, l1, 50000, 1, &s, &c) == (int)MS_OK, "create for fragments");
    /* 上面已经整包推入。另建会话按字节推。 */
    ms_session_destroy(s);
    {
        ms_synth_buf b;
        ms_synth_init(&b);
        CHECK(make_basic_ts(&b, 188) == 0, "ts");
        CHECK(ms_synth_pes(&b, 0x101, 0xE0, 1, 50000, a1, l1, 188) == 0, "pes");
        CHECK_ST(ms_session_create(NULL, NULL, &s), MS_OK);
        if (push_all(s, b.data, b.len, 1) != 0) {
            ms_synth_free(&b);
            return -1;
        }
        CHECK_ST(ms_session_end_of_input(s), MS_OK);
        n_au = 0;
        for (;;) {
            ms_event ev;
            ms_status st = ms_session_pull(s, &ev);
            if (st == MS_AGAIN || st == MS_EOS) {
                break;
            }
            CHECK_ST(st, MS_OK);
            if (ev.type == MS_EVENT_AU) {
                CHECK(ev.has_pts && ev.pts == 50000, "frag pts");
                CHECK(ev.data_size == l1, "frag size");
                n_au++;
            }
            CHECK_ST(ms_session_release(s, ev.release_id), MS_OK);
        }
        CHECK(n_au == 1, "one au from byte fragments");
        ms_session_destroy(s);
        ms_synth_free(&b);
    }

    /* 一帧拆到两个 PES：时间戳属于开始的那一包，不采用后一包 PTS。 */
    split = l1 / 2;
    if (split < 8) {
        split = 8;
    }
    memcpy(first, a1, split);
    memcpy(second, a1 + split, l1 - split);
    {
        ms_synth_buf b;
        ms_synth_init(&b);
        CHECK(make_basic_ts(&b, 188) == 0, "ts");
        CHECK(ms_synth_pes(&b, 0x101, 0xE0, 1, 180000, first, split, 188) == 0, "p1");
        CHECK(ms_synth_pes(&b, 0x101, 0xE0, 1, 270000, second, l1 - split, 188) == 0, "p2");
        CHECK_ST(ms_session_create(NULL, NULL, &s), MS_OK);
        CHECK_ST(ms_session_push(s, b.data, b.len, &c), MS_OK);
        CHECK_ST(ms_session_end_of_input(s), MS_OK);
        n_au = 0;
        for (;;) {
            ms_event ev;
            ms_status st = ms_session_pull(s, &ev);
            if (st == MS_AGAIN || st == MS_EOS) {
                break;
            }
            CHECK_ST(st, MS_OK);
            if (ev.type == MS_EVENT_AU) {
                CHECK(ev.has_pts && ev.pts == 180000, "pts from starting pes");
                CHECK(ev.data_size == l1, "reassembled au");
                n_au++;
            }
            CHECK_ST(ms_session_release(s, ev.release_id), MS_OK);
        }
        CHECK(n_au == 1, "one au over two pes");
        ms_session_destroy(s);
        ms_synth_free(&b);
    }
    printf("  test_h264_fragments_ts_pes_timestamps ok\n");
    return 0;
}

static int test_h264_eos_flush_reset_backpressure_reject(void)
{
    uint8_t a1[512], a2[256], both[768], trunc[600], field[256], noaud[256];
    uint8_t bad[32], sps[128], pps[32], aud[16], sl[32];
    size_t l1 = 0, l2 = 0, n = 0, c = 0, l = 0;
    ms_session *s = NULL;
    ms_session_limits lim;
    ms_event ev;
    ms_epochs ep1, ep2;
    int n_au, n_cfg, st, i;

    CHECK(make_h264_au(a1, sizeof(a1), &l1, 4, 4, 16, 16, 1, NULL, 0) == 0, "a1");
    CHECK(make_h264_au(a2, sizeof(a2), &l2, 4, 4, 32, 16, 1, NULL, 0) == 0, "a2 smaller width change");

    /* EOS：完整 AU + 下一 AUD（收口）+ 不完整起始码，半枚不得发出 */
    memcpy(trunc, a1, l1);
    n = l1;
    CHECK(ms_synth_h264_aud(aud, sizeof(aud), &l, 4) == 0, "tail aud");
    CHECK(append_buf(trunc, sizeof(trunc), &n, aud, l) == 0, "tail a");
    trunc[n++] = 0x00;
    trunc[n++] = 0x00;
    trunc[n++] = 0x01; /* 无 NAL 头，EOS 时丢弃 */
    CHECK(push_video_payload(trunc, n, 1000, 1, &s, &c) == (int)MS_OK, "trunc push");
    CHECK_ST(ms_session_end_of_input(s), MS_OK);
    n_au = 0;
    for (;;) {
        ms_status rst = ms_session_pull(s, &ev);
        if (rst == MS_EOS) {
            break;
        }
        CHECK_ST(rst, MS_OK);
        if (ev.type == MS_EVENT_AU) {
            CHECK(ev.data_size == l1, "only complete au");
            n_au++;
        }
        CHECK_ST(ms_session_release(s, ev.release_id), MS_OK);
    }
    CHECK(n_au == 1, "truncated tail dropped");
    ms_session_destroy(s);

    /* flush 丢掉半帧，reset 增加 epoch */
    CHECK(push_video_payload(a1, 12, 2000, 1, &s, &c) == (int)MS_OK, "partial");
    CHECK_ST(ms_session_flush(s), MS_OK);
    CHECK_ST(ms_session_pull(s, &ev), MS_AGAIN);
    {
        ms_synth_buf b;
        ms_synth_init(&b);
        CHECK(make_basic_ts(&b, 188) == 0, "ts2");
        CHECK(ms_synth_pes(&b, 0x101, 0xE0, 1, 3000, a1, l1, 188) == 0, "full");
        CHECK_ST(ms_session_push(s, b.data, b.len, &c), MS_OK);
        CHECK_ST(ms_session_end_of_input(s), MS_OK);
        n_au = 0;
        for (;;) {
            ms_status rst = ms_session_pull(s, &ev);
            if (rst == MS_AGAIN || rst == MS_EOS) {
                break;
            }
            CHECK_ST(rst, MS_OK);
            if (ev.type == MS_EVENT_AU) {
                CHECK(ev.pts == 3000, "au after flush");
                n_au++;
            }
            CHECK_ST(ms_session_release(s, ev.release_id), MS_OK);
        }
        CHECK(n_au == 1, "au after flush");
        CHECK_ST(ms_session_epochs(s, &ep1), MS_OK);
        CHECK_ST(ms_session_cancel(s), MS_OK);
        CHECK_ST(ms_session_push(s, b.data, b.len, &c), MS_ERR_CANCELED);
        CHECK_ST(ms_session_reset(s), MS_OK);
        CHECK_ST(ms_session_epochs(s, &ep2), MS_OK);
        CHECK(ep2.session_epoch == ep1.session_epoch + 1, "reset epoch after cancel");
        ms_synth_free(&b);
    }
    ms_session_destroy(s);

    /* 配置变化：不同宽度的 SPS，两次 config，第二 AU format_changed */
    memcpy(both, a1, l1);
    memcpy(both + l1, a2, l2);
    CHECK(push_video_payload(both, l1 + l2, 4000, 1, &s, &c) == (int)MS_OK, "cfg change");
    CHECK_ST(ms_session_end_of_input(s), MS_OK);
    n_cfg = 0;
    n_au = 0;
    for (;;) {
        ms_status rst = ms_session_pull(s, &ev);
        if (rst == MS_AGAIN || rst == MS_EOS) {
            break;
        }
        CHECK_ST(rst, MS_OK);
        if (ev.type == MS_EVENT_CODEC_CONFIG) {
            n_cfg++;
        }
        if (ev.type == MS_EVENT_AU) {
            if (n_au == 1) {
                CHECK(ev.format_changed == 1, "second au after new sps");
            }
            n_au++;
        }
        CHECK_ST(ms_session_release(s, ev.release_id), MS_OK);
    }
    CHECK(n_cfg == 2 && n_au == 2, "config change");
    ms_session_destroy(s);

    /* 无 AUD：合法条带头在 EOS 后发出一个完整 AU（不再 UNSUPPORTED） */
    {
        ms_synth_h264_slice_opts opt;
        n = 0;
        CHECK(ms_synth_h264_sps(sps, sizeof(sps), &l, 4, 16, 16, 1) == 0, "sps");
        CHECK(append_buf(noaud, sizeof(noaud), &n, sps, l) == 0, "s");
        CHECK(ms_synth_h264_pps(pps, sizeof(pps), &l, 4) == 0, "pps");
        CHECK(append_buf(noaud, sizeof(noaud), &n, pps, l) == 0, "p");
        h264_slice_opt_init(&opt, 4, 1, 0, 0);
        CHECK(ms_synth_h264_slice_hdr(sl, sizeof(sl), &l, &opt) == 0, "sl hdr");
        CHECK(append_buf(noaud, sizeof(noaud), &n, sl, l) == 0, "v");
        st = push_video_payload(noaud, n, 6000, 1, &s, &c);
        if (st == (int)MS_OK) {
            st = (int)ms_session_end_of_input(s);
        }
        CHECK(st == (int)MS_OK, "no-AUD with valid slice accepted");
        n_au = 0;
        n_cfg = 0;
        for (;;) {
            ms_status rst = ms_session_pull(s, &ev);
            if (rst == MS_AGAIN || rst == MS_EOS) {
                break;
            }
            CHECK_ST(rst, MS_OK);
            if (ev.type == MS_EVENT_CODEC_CONFIG) {
                n_cfg++;
            }
            if (ev.type == MS_EVENT_AU) {
                CHECK(ev.is_keyframe == 1, "no-AUD IDR keyframe");
                CHECK(ev.has_pts == 1 && ev.pts == 6000, "no-AUD pts");
                n_au++;
            }
            CHECK_ST(ms_session_release(s, ev.release_id), MS_OK);
        }
        CHECK(n_cfg >= 1 && n_au == 1, "no-AUD one au");
        ms_session_destroy(s);
    }

    /* field/PAFF：frame_mbs_only=0，不得当 progressive 发出 */
    n = 0;
    CHECK(ms_synth_h264_aud(aud, sizeof(aud), &l, 4) == 0, "aud");
    CHECK(append_buf(field, sizeof(field), &n, aud, l) == 0, "a");
    CHECK(ms_synth_h264_sps(sps, sizeof(sps), &l, 4, 16, 32, 0) == 0, "field sps");
    CHECK(append_buf(field, sizeof(field), &n, sps, l) == 0, "s");
    CHECK(ms_synth_h264_pps(pps, sizeof(pps), &l, 4) == 0, "pps");
    CHECK(append_buf(field, sizeof(field), &n, pps, l) == 0, "p");
    CHECK(ms_synth_h264_slice(sl, sizeof(sl), &l, 4, 1, NULL, 0) == 0, "sl");
    CHECK(append_buf(field, sizeof(field), &n, sl, l) == 0, "v");
    st = push_video_payload(field, n, 7000, 1, &s, &c);
    if (st == (int)MS_OK) {
        st = (int)ms_session_end_of_input(s);
    }
    CHECK(st == (int)MS_ERR_UNSUPPORTED, "field/PAFF rejected");
    ms_session_destroy(s);

    /* 畸形：forbidden_zero_bit */
    bad[0] = 0x00;
    bad[1] = 0x00;
    bad[2] = 0x00;
    bad[3] = 0x01;
    bad[4] = 0x89; /* forbidden=1, type=9 */
    bad[5] = 0x10;
    st = push_video_payload(bad, 6, 8000, 1, &s, &c);
    if (st == (int)MS_OK) {
        st = (int)ms_session_end_of_input(s);
    }
    CHECK(st == (int)MS_ERR_MALFORMED, "forbidden bit");
    ms_session_destroy(s);

    /* 畸形：SPS 过短，EOS 时按不完整/畸形拒绝 */
    n = 0;
    CHECK(ms_synth_h264_aud(aud, sizeof(aud), &l, 4) == 0, "aud");
    CHECK(append_buf(bad, sizeof(bad), &n, aud, l) == 0, "a");
    bad[n++] = 0x00;
    bad[n++] = 0x00;
    bad[n++] = 0x00;
    bad[n++] = 0x01;
    bad[n++] = 0x67;
    bad[n++] = 0x42;
    CHECK(push_video_payload(bad, n, 8100, 1, &s, &c) == (int)MS_OK, "short sps push");
    st = (int)ms_session_end_of_input(s);
    CHECK(st == (int)MS_ERR_MALFORMED || st == (int)MS_OK, "short sps end");
    if (st == (int)MS_OK) {
        /* 若只当作截断丢弃、未发出 AU，也可接受。 */
        n_au = 0;
        for (;;) {
            ms_status rst = ms_session_pull(s, &ev);
            if (rst == MS_AGAIN || rst == MS_EOS) {
                break;
            }
            if (rst != MS_OK) {
                break;
            }
            if (ev.type == MS_EVENT_AU) {
                n_au++;
            }
            (void)ms_session_release(s, ev.release_id);
        }
        CHECK(n_au == 0, "truncated sps must not yield au");
    }
    ms_session_destroy(s);

    /* 反压：缩小输出队列，第二 AU 不得丢 */
    ms_session_limits_default(&lim);
    lim.au_queue_bytes = (l1 + 64);
    memcpy(both, a1, l1);
    memcpy(both + l1, a2, l2);
    st = push_video_payload_lim(both, l1 + l2, 9000, 1, &s, &c, &lim);
    CHECK(st == (int)MS_OK || st == (int)MS_LIMIT, "push may backpressure");
    n_au = 0;
    for (i = 0; i < 64; i++) {
        ms_status rst = ms_session_pull(s, &ev);
        if (rst == MS_AGAIN) {
            break;
        }
        CHECK_ST(rst, MS_OK);
        if (ev.type == MS_EVENT_AU) {
            n_au++;
        }
        CHECK_ST(ms_session_release(s, ev.release_id), MS_OK);
    }
    CHECK_ST(ms_session_end_of_input(s), MS_OK);
    for (i = 0; i < 64; i++) {
        ms_status rst = ms_session_pull(s, &ev);
        if (rst == MS_AGAIN || rst == MS_EOS) {
            break;
        }
        CHECK_ST(rst, MS_OK);
        if (ev.type == MS_EVENT_AU) {
            n_au++;
        }
        CHECK_ST(ms_session_release(s, ev.release_id), MS_OK);
    }
    CHECK(n_au == 2, "no dropped h264 au under backpressure");
    ms_session_destroy(s);

    printf("  test_h264_eos_flush_reset_backpressure_reject ok\n");
    return 0;
}


typedef struct h264_au_cap {
    int n_au;
    int n_cfg;
    size_t sizes[8];
    int key[8];
    int has_pts[8];
    uint64_t pts[8];
    int changed[8];
    ms_status au_rc;
} h264_au_cap;

static ms_status cap_on_cfg(void *ctx, const uint8_t *data, size_t size)
{
    h264_au_cap *c = (h264_au_cap *)ctx;
    (void)data;
    if (size < 8) {
        return MS_ERR_MALFORMED;
    }
    c->n_cfg++;
    return MS_OK;
}

static ms_status cap_on_au(void *ctx, const ms_au_frame *fr)
{
    h264_au_cap *c = (h264_au_cap *)ctx;
    if (c->au_rc != MS_OK) {
        return c->au_rc;
    }
    if (c->n_au < 8) {
        c->sizes[c->n_au] = fr->size;
        c->key[c->n_au] = fr->is_keyframe;
        c->has_pts[c->n_au] = fr->has_pts;
        c->pts[c->n_au] = fr->pts;
        c->changed[c->n_au] = fr->format_changed;
    }
    c->n_au++;
    return MS_OK;
}

static int test_h264_noaud_multislice_and_switch(void)
{
    uint8_t payload[1024], sps[256], pps[128], tmp[256];
    size_t n = 0, l = 0, c = 0, sz0, sz1;
    ms_synth_h264_slice_opts opt;
    ms_session *s = NULL;
    int n_au = 0, n_cfg = 0, st, i;
    int key[4], changed[4];
    uint64_t pts[4];
    int has_pts[4];
    size_t sizes[4];

    /* 同一图像两个 slice（first_mb 不同，其它边界字段相同）必须合成一帧。 */
    CHECK(ms_synth_h264_sps(sps, sizeof(sps), &l, 4, 16, 16, 1) == 0, "sps");
    CHECK(append_buf(payload, sizeof(payload), &n, sps, l) == 0, "s");
    CHECK(ms_synth_h264_pps(pps, sizeof(pps), &l, 4) == 0, "pps");
    CHECK(append_buf(payload, sizeof(payload), &n, pps, l) == 0, "p");
    h264_slice_opt_init(&opt, 4, 1, 0, 0);
    CHECK(append_slice_opt(payload, sizeof(payload), &n, &opt) == 0, "sl0");
    sz0 = n;
    h264_slice_opt_init(&opt, 3, 1, 10, 0);
    CHECK(append_slice_opt(payload, sizeof(payload), &n, &opt) == 0, "sl1 same pic");
    sz1 = n;
    /* 下一图像：frame_num 变化，不得并入上一帧。 */
    h264_slice_opt_init(&opt, 4, 0, 0, 1);
    CHECK(append_slice_opt(payload, sizeof(payload), &n, &opt) == 0, "pic2");

    st = push_video_payload(payload, n, 11000, 1, &s, &c);
    if (st == (int)MS_OK) {
        st = (int)ms_session_end_of_input(s);
    }
    CHECK(st == (int)MS_OK, "multi-slice push");
    for (i = 0; i < 64; i++) {
        ms_event ev;
        ms_status rst = ms_session_pull(s, &ev);
        if (rst == MS_AGAIN || rst == MS_EOS) {
            break;
        }
        CHECK_ST(rst, MS_OK);
        if (ev.type == MS_EVENT_CODEC_CONFIG) {
            n_cfg++;
        }
        if (ev.type == MS_EVENT_AU) {
            CHECK(n_au < 4, "cap");
            sizes[n_au] = ev.data_size;
            key[n_au] = ev.is_keyframe;
            has_pts[n_au] = ev.has_pts;
            pts[n_au] = ev.pts;
            changed[n_au] = ev.format_changed;
            n_au++;
        }
        CHECK_ST(ms_session_release(s, ev.release_id), MS_OK);
    }
    CHECK(n_cfg >= 1 && n_au == 2, "two pictures, not three slices");
    CHECK(sizes[0] == sz1, "first au holds both slices, no trailing pic2");
    CHECK(sizes[1] == n - sz1, "second picture is its own au");
    CHECK(key[0] == 1 && key[1] == 0, "IDR then P");
    CHECK(has_pts[0] == 1 && pts[0] == 11000, "first pts");
    CHECK(has_pts[1] == 0, "same PES second au no invented pts");
    (void)sz0;
    (void)changed;
    ms_session_destroy(s);

    /* SPS/PPS 切换：新 ID，第二次 config，第二 AU format_changed。 */
    n = 0;
    n_au = 0;
    n_cfg = 0;
    CHECK(ms_synth_h264_sps_ex(tmp, sizeof(tmp), &l, 4, 16, 16, 1, 0, 2, 0, 0, 0) == 0, "sps0");
    CHECK(append_buf(payload, sizeof(payload), &n, tmp, l) == 0, "s0");
    CHECK(ms_synth_h264_pps_ex(tmp, sizeof(tmp), &l, 4, 0, 0, 0, 0) == 0, "pps0");
    CHECK(append_buf(payload, sizeof(payload), &n, tmp, l) == 0, "p0");
    h264_slice_opt_init(&opt, 4, 1, 0, 0);
    opt.pps_id = 0;
    CHECK(append_slice_opt(payload, sizeof(payload), &n, &opt) == 0, "idr0");
    CHECK(ms_synth_h264_sps_ex(tmp, sizeof(tmp), &l, 4, 32, 16, 1, 1, 2, 0, 0, 0) == 0, "sps1");
    CHECK(append_buf(payload, sizeof(payload), &n, tmp, l) == 0, "s1");
    CHECK(ms_synth_h264_pps_ex(tmp, sizeof(tmp), &l, 4, 1, 1, 0, 0) == 0, "pps1");
    CHECK(append_buf(payload, sizeof(payload), &n, tmp, l) == 0, "p1");
    h264_slice_opt_init(&opt, 4, 1, 0, 0);
    opt.pps_id = 1;
    opt.idr_pic_id = 1;
    CHECK(append_slice_opt(payload, sizeof(payload), &n, &opt) == 0, "idr1");
    st = push_video_payload(payload, n, 12000, 1, &s, &c);
    if (st == (int)MS_OK) {
        st = (int)ms_session_end_of_input(s);
    }
    CHECK(st == (int)MS_OK, "switch push");
    for (i = 0; i < 64; i++) {
        ms_event ev;
        ms_status rst = ms_session_pull(s, &ev);
        if (rst == MS_AGAIN || rst == MS_EOS) {
            break;
        }
        CHECK_ST(rst, MS_OK);
        if (ev.type == MS_EVENT_CODEC_CONFIG) {
            n_cfg++;
        }
        if (ev.type == MS_EVENT_AU) {
            CHECK(n_au < 4, "cap");
            changed[n_au] = ev.format_changed;
            key[n_au] = ev.is_keyframe;
            n_au++;
        }
        CHECK_ST(ms_session_release(s, ev.release_id), MS_OK);
    }
    CHECK(n_cfg == 2 && n_au == 2, "sps/pps switch configs");
    CHECK(changed[1] == 1, "second au format_changed");
    CHECK(key[0] == 1 && key[1] == 1, "both IDR");
    ms_session_destroy(s);

    printf("  test_h264_noaud_multislice_and_switch ok\n");
    return 0;
}

static int test_h264_noaud_unknown_pps_and_poc(void)
{
    uint8_t payload[1024], tmp[256];
    size_t n = 0, l = 0, c = 0;
    ms_synth_h264_slice_opts opt;
    ms_session *s = NULL;
    int st, i, n_au;
    size_t sz0;

    /* 条带引用尚未出现的 PPS：等待丢弃，不把会话标为畸形，也不发出 AU。 */
    n = 0;
    n_au = 0;
    CHECK(ms_synth_h264_sps_ex(tmp, sizeof(tmp), &l, 4, 16, 16, 1, 0, 2, 0, 0, 0) == 0, "sps");
    CHECK(append_buf(payload, sizeof(payload), &n, tmp, l) == 0, "s");
    CHECK(ms_synth_h264_pps_ex(tmp, sizeof(tmp), &l, 4, 0, 0, 0, 0) == 0, "pps0");
    CHECK(append_buf(payload, sizeof(payload), &n, tmp, l) == 0, "p");
    h264_slice_opt_init(&opt, 4, 1, 0, 0);
    opt.pps_id = 3;
    CHECK(append_slice_opt(payload, sizeof(payload), &n, &opt) == 0, "unknown pps");
    st = push_video_payload(payload, n, 13000, 1, &s, &c);
    if (st == (int)MS_OK) {
        st = (int)ms_session_end_of_input(s);
    }
    CHECK(st == (int)MS_OK, "unknown pps wait");
    for (i = 0; i < 32; i++) {
        ms_event ev;
        ms_status rst = ms_session_pull(s, &ev);
        if (rst == MS_AGAIN || rst == MS_EOS) {
            break;
        }
        CHECK_ST(rst, MS_OK);
        if (ev.type == MS_EVENT_AU) {
            n_au++;
        }
        CHECK_ST(ms_session_release(s, ev.release_id), MS_OK);
    }
    CHECK(n_au == 0, "unknown pps no au");
    ms_session_destroy(s);

    /* FMO PPS：明确 UNSUPPORTED。 */
    n = 0;
    CHECK(ms_synth_h264_sps_ex(tmp, sizeof(tmp), &l, 4, 16, 16, 1, 0, 2, 0, 0, 0) == 0, "sps fmo");
    CHECK(append_buf(payload, sizeof(payload), &n, tmp, l) == 0, "s");
    CHECK(ms_synth_h264_pps_ex(tmp, sizeof(tmp), &l, 4, 0, 0, 0, 1) == 0, "fmo pps");
    CHECK(append_buf(payload, sizeof(payload), &n, tmp, l) == 0, "p");
    st = push_video_payload(payload, n, 13100, 1, &s, &c);
    if (st == (int)MS_OK) {
        st = (int)ms_session_end_of_input(s);
    }
    CHECK(st == (int)MS_ERR_UNSUPPORTED, "fmo pps");
    ms_session_destroy(s);

    /* POC type 0：相同 frame_num、不同 poc_lsb 是两帧；相同字段的两 slice 是一帧。 */
    n = 0;
    CHECK(ms_synth_h264_sps_ex(tmp, sizeof(tmp), &l, 4, 16, 16, 1, 0, 0, 0, 0, 0) == 0,
          "sps poc0");
    CHECK(append_buf(payload, sizeof(payload), &n, tmp, l) == 0, "s");
    CHECK(ms_synth_h264_pps_ex(tmp, sizeof(tmp), &l, 4, 0, 0, 0, 0) == 0, "pps");
    CHECK(append_buf(payload, sizeof(payload), &n, tmp, l) == 0, "p");
    h264_slice_opt_init(&opt, 4, 1, 0, 0);
    opt.poc_type = 0;
    opt.poc_lsb = 0;
    CHECK(append_slice_opt(payload, sizeof(payload), &n, &opt) == 0, "poc0 sl0");
    opt.first_mb = 7;
    CHECK(append_slice_opt(payload, sizeof(payload), &n, &opt) == 0, "poc0 sl1 same");
    sz0 = n;
    opt.first_mb = 0;
    opt.poc_lsb = 4;
    opt.idr_pic_id = 1;
    CHECK(append_slice_opt(payload, sizeof(payload), &n, &opt) == 0, "poc0 new lsb");
    st = push_video_payload(payload, n, 14000, 1, &s, &c);
    if (st == (int)MS_OK) {
        st = (int)ms_session_end_of_input(s);
    }
    CHECK(st == (int)MS_OK, "poc0 push");
    n_au = 0;
    for (i = 0; i < 64; i++) {
        ms_event ev;
        ms_status rst = ms_session_pull(s, &ev);
        if (rst == MS_AGAIN || rst == MS_EOS) {
            break;
        }
        CHECK_ST(rst, MS_OK);
        if (ev.type == MS_EVENT_AU) {
            if (n_au == 0) {
                CHECK(ev.data_size == sz0, "poc0 two slices one au");
            }
            n_au++;
        }
        CHECK_ST(ms_session_release(s, ev.release_id), MS_OK);
    }
    CHECK(n_au == 2, "poc_lsb change splits pictures");
    ms_session_destroy(s);

    /* POC type 1，always_zero=1：靠 frame_num 分界。 */
    n = 0;
    CHECK(ms_synth_h264_sps_ex(tmp, sizeof(tmp), &l, 4, 16, 16, 1, 0, 1, 0, 0, 1) == 0,
          "sps poc1 z");
    CHECK(append_buf(payload, sizeof(payload), &n, tmp, l) == 0, "s");
    CHECK(ms_synth_h264_pps_ex(tmp, sizeof(tmp), &l, 4, 0, 0, 0, 0) == 0, "pps");
    CHECK(append_buf(payload, sizeof(payload), &n, tmp, l) == 0, "p");
    h264_slice_opt_init(&opt, 4, 1, 0, 0);
    opt.poc_type = 1;
    CHECK(append_slice_opt(payload, sizeof(payload), &n, &opt) == 0, "p1a");
    h264_slice_opt_init(&opt, 4, 0, 0, 1);
    opt.poc_type = 1;
    CHECK(append_slice_opt(payload, sizeof(payload), &n, &opt) == 0, "p1b");
    st = push_video_payload(payload, n, 14100, 1, &s, &c);
    if (st == (int)MS_OK) {
        st = (int)ms_session_end_of_input(s);
    }
    CHECK(st == (int)MS_OK, "poc1 zero push");
    n_au = 0;
    for (i = 0; i < 64; i++) {
        ms_event ev;
        ms_status rst = ms_session_pull(s, &ev);
        if (rst == MS_AGAIN || rst == MS_EOS) {
            break;
        }
        CHECK_ST(rst, MS_OK);
        if (ev.type == MS_EVENT_AU) {
            n_au++;
        }
        CHECK_ST(ms_session_release(s, ev.release_id), MS_OK);
    }
    CHECK(n_au == 2, "poc1 always_zero uses frame_num");
    ms_session_destroy(s);

    /* POC type 1 带 delta：相同 frame_num、不同 delta0 是两帧。 */
    n = 0;
    CHECK(ms_synth_h264_sps_ex(tmp, sizeof(tmp), &l, 4, 16, 16, 1, 0, 1, 0, 0, 0) == 0,
          "sps poc1 d");
    CHECK(append_buf(payload, sizeof(payload), &n, tmp, l) == 0, "s");
    CHECK(ms_synth_h264_pps_ex(tmp, sizeof(tmp), &l, 4, 0, 0, 0, 0) == 0, "pps");
    CHECK(append_buf(payload, sizeof(payload), &n, tmp, l) == 0, "p");
    h264_slice_opt_init(&opt, 4, 1, 0, 0);
    opt.poc_type = 1;
    opt.write_delta0 = 1;
    opt.delta0 = 0;
    CHECK(append_slice_opt(payload, sizeof(payload), &n, &opt) == 0, "d0");
    opt.delta0 = 2;
    opt.idr_pic_id = 2;
    CHECK(append_slice_opt(payload, sizeof(payload), &n, &opt) == 0, "d2");
    st = push_video_payload(payload, n, 14200, 1, &s, &c);
    if (st == (int)MS_OK) {
        st = (int)ms_session_end_of_input(s);
    }
    CHECK(st == (int)MS_OK, "poc1 delta push");
    n_au = 0;
    for (i = 0; i < 64; i++) {
        ms_event ev;
        ms_status rst = ms_session_pull(s, &ev);
        if (rst == MS_AGAIN || rst == MS_EOS) {
            break;
        }
        CHECK_ST(rst, MS_OK);
        if (ev.type == MS_EVENT_AU) {
            n_au++;
        }
        CHECK_ST(ms_session_release(s, ev.release_id), MS_OK);
    }
    CHECK(n_au == 2, "poc1 delta0 splits");
    ms_session_destroy(s);

    /* nal_ref_idc 从非 0 变为 0：新的主图像。先发 IDR 再测 P 切片分界。 */
    n = 0;
    CHECK(ms_synth_h264_sps_ex(tmp, sizeof(tmp), &l, 4, 16, 16, 1, 0, 2, 0, 0, 0) == 0, "sps ref");
    CHECK(append_buf(payload, sizeof(payload), &n, tmp, l) == 0, "s");
    CHECK(ms_synth_h264_pps_ex(tmp, sizeof(tmp), &l, 4, 0, 0, 0, 0) == 0, "pps");
    CHECK(append_buf(payload, sizeof(payload), &n, tmp, l) == 0, "p");
    h264_slice_opt_init(&opt, 4, 1, 0, 0);
    CHECK(append_slice_opt(payload, sizeof(payload), &n, &opt) == 0, "idr start");
    h264_slice_opt_init(&opt, 4, 0, 0, 3);
    opt.nal_ref_idc = 2;
    CHECK(append_slice_opt(payload, sizeof(payload), &n, &opt) == 0, "ref2");
    opt.nal_ref_idc = 0;
    CHECK(append_slice_opt(payload, sizeof(payload), &n, &opt) == 0, "ref0");
    st = push_video_payload(payload, n, 14300, 1, &s, &c);
    if (st == (int)MS_OK) {
        st = (int)ms_session_end_of_input(s);
    }
    CHECK(st == (int)MS_OK, "ref idc push");
    n_au = 0;
    for (i = 0; i < 64; i++) {
        ms_event ev;
        ms_status rst = ms_session_pull(s, &ev);
        if (rst == MS_AGAIN || rst == MS_EOS) {
            break;
        }
        CHECK_ST(rst, MS_OK);
        if (ev.type == MS_EVENT_AU) {
            n_au++;
        }
        CHECK_ST(ms_session_release(s, ev.release_id), MS_OK);
    }
    CHECK(n_au == 3, "nal_ref_idc zero vs nonzero splits");
    ms_session_destroy(s);

    printf("  test_h264_noaud_unknown_pps_and_poc ok\n");
    return 0;
}

static int test_h264_noaud_fragments_flush_limit(void)
{
    uint8_t payload[1024], pic2[256], tmp[256], first[512];
    size_t n = 0, l = 0, c = 0, split, pic2_len = 0;
    ms_synth_h264_slice_opts opt;
    ms_session *s = NULL;
    ms_session_limits lim;
    ms_event ev;
    int n_au, n_cfg, i, st;
    uint64_t pts[4];
    int has_pts[4];
    ms_allocator alloc;
    ms_h264 h;
    ms_h264_callbacks cb;
    h264_au_cap cap;

    /* 两帧无 AUD：第一帧跨两个 PES，逐字节推入；PTS 归属起始 PES。第二帧在后一 PES。 */
    CHECK(ms_synth_h264_sps(tmp, sizeof(tmp), &l, 4, 16, 16, 1) == 0, "sps");
    CHECK(append_buf(payload, sizeof(payload), &n, tmp, l) == 0, "s");
    CHECK(ms_synth_h264_pps(tmp, sizeof(tmp), &l, 4) == 0, "pps");
    CHECK(append_buf(payload, sizeof(payload), &n, tmp, l) == 0, "p");
    h264_slice_opt_init(&opt, 4, 1, 0, 0);
    CHECK(append_slice_opt(payload, sizeof(payload), &n, &opt) == 0, "idr");
    h264_slice_opt_init(&opt, 4, 0, 0, 1);
    CHECK(ms_synth_h264_slice_hdr(pic2, sizeof(pic2), &pic2_len, &opt) == 0, "p");
    split = n / 2;
    if (split < 8) {
        split = 8;
    }
    memcpy(first, payload, split);
    {
        size_t slen = n - split;
        uint8_t sec[768];
        memcpy(sec, payload + split, slen);
        memcpy(sec + slen, pic2, pic2_len);
        slen += pic2_len;
        {
            ms_synth_buf b;
            ms_synth_init(&b);
            CHECK(make_basic_ts(&b, 188) == 0, "ts");
            CHECK(ms_synth_pes(&b, 0x101, 0xE0, 1, 21000, first, split, 188) == 0, "p1");
            CHECK(ms_synth_pes(&b, 0x101, 0xE0, 1, 22000, sec, slen, 188) == 0, "p2");
            CHECK_ST(ms_session_create(NULL, NULL, &s), MS_OK);
            if (push_all(s, b.data, b.len, 1) != 0) {
                ms_synth_free(&b);
                return -1;
            }
            CHECK_ST(ms_session_end_of_input(s), MS_OK);
            n_au = 0;
            for (i = 0; i < 64; i++) {
                ms_status rst = ms_session_pull(s, &ev);
                if (rst == MS_AGAIN || rst == MS_EOS) {
                    break;
                }
                CHECK_ST(rst, MS_OK);
                if (ev.type == MS_EVENT_AU) {
                    CHECK(n_au < 4, "cap");
                    pts[n_au] = ev.pts;
                    has_pts[n_au] = ev.has_pts;
                    n_au++;
                }
                CHECK_ST(ms_session_release(s, ev.release_id), MS_OK);
            }
            CHECK(n_au == 2, "two au across pes byte fragments");
            CHECK(has_pts[0] == 1 && pts[0] == 21000, "pts from starting pes");
            CHECK(has_pts[1] == 1 && pts[1] == 22000, "second pic starts in later pes");
            ms_session_destroy(s);
            ms_synth_free(&b);
        }
    }

    /* flush 丢掉半帧；EOS 发出已完整的无 AUD 图像；reset 可用。 */
    n = 0;
    CHECK(ms_synth_h264_sps(tmp, sizeof(tmp), &l, 4, 16, 16, 1) == 0, "sps");
    CHECK(append_buf(payload, sizeof(payload), &n, tmp, l) == 0, "s");
    CHECK(ms_synth_h264_pps(tmp, sizeof(tmp), &l, 4) == 0, "pps");
    CHECK(append_buf(payload, sizeof(payload), &n, tmp, l) == 0, "p");
    CHECK(push_video_payload(payload, n > 6 ? 6 : n, 23000, 1, &s, &c) == (int)MS_OK,
          "partial noaud");
    CHECK_ST(ms_session_flush(s), MS_OK);
    CHECK_ST(ms_session_pull(s, &ev), MS_AGAIN);
    {
        ms_synth_buf b;
        h264_slice_opt_init(&opt, 4, 1, 0, 0);
        CHECK(append_slice_opt(payload, sizeof(payload), &n, &opt) == 0, "full idr");
        ms_synth_init(&b);
        CHECK(make_basic_ts(&b, 188) == 0, "ts");
        CHECK(ms_synth_pes(&b, 0x101, 0xE0, 1, 24000, payload, n, 188) == 0, "full");
        CHECK_ST(ms_session_push(s, b.data, b.len, &c), MS_OK);
        CHECK_ST(ms_session_end_of_input(s), MS_OK);
        n_au = 0;
        for (;;) {
            ms_status rst = ms_session_pull(s, &ev);
            if (rst == MS_AGAIN || rst == MS_EOS) {
                break;
            }
            CHECK_ST(rst, MS_OK);
            if (ev.type == MS_EVENT_AU) {
                CHECK(ev.has_pts && ev.pts == 24000, "au after flush");
                n_au++;
            }
            CHECK_ST(ms_session_release(s, ev.release_id), MS_OK);
        }
        CHECK(n_au == 1, "noaud after flush");
        CHECK_ST(ms_session_reset(s), MS_OK);
        ms_synth_free(&b);
    }
    ms_session_destroy(s);

    /* 反压：缩小输出队列，两帧都不得丢。 */
    n = 0;
    CHECK(ms_synth_h264_sps(tmp, sizeof(tmp), &l, 4, 16, 16, 1) == 0, "sps");
    CHECK(append_buf(payload, sizeof(payload), &n, tmp, l) == 0, "s");
    CHECK(ms_synth_h264_pps(tmp, sizeof(tmp), &l, 4) == 0, "pps");
    CHECK(append_buf(payload, sizeof(payload), &n, tmp, l) == 0, "p");
    h264_slice_opt_init(&opt, 4, 1, 0, 0);
    CHECK(append_slice_opt(payload, sizeof(payload), &n, &opt) == 0, "a");
    h264_slice_opt_init(&opt, 4, 0, 0, 1);
    CHECK(append_slice_opt(payload, sizeof(payload), &n, &opt) == 0, "b");
    ms_session_limits_default(&lim);
    lim.au_queue_bytes = 80;
    st = push_video_payload_lim(payload, n, 25000, 1, &s, &c, &lim);
    CHECK(st == (int)MS_OK || st == (int)MS_LIMIT, "may backpressure");
    n_au = 0;
    n_cfg = 0;
    for (i = 0; i < 64; i++) {
        ms_status rst = ms_session_pull(s, &ev);
        if (rst == MS_AGAIN) {
            break;
        }
        CHECK_ST(rst, MS_OK);
        if (ev.type == MS_EVENT_AU) {
            n_au++;
        }
        if (ev.type == MS_EVENT_CODEC_CONFIG) {
            n_cfg++;
        }
        CHECK_ST(ms_session_release(s, ev.release_id), MS_OK);
    }
    if (n_au < 2) {
        CHECK_ST(ms_session_end_of_input(s), MS_OK);
        for (i = 0; i < 64; i++) {
            ms_status rst = ms_session_pull(s, &ev);
            if (rst == MS_AGAIN || rst == MS_EOS) {
                break;
            }
            CHECK_ST(rst, MS_OK);
            if (ev.type == MS_EVENT_AU) {
                n_au++;
            }
            CHECK_ST(ms_session_release(s, ev.release_id), MS_OK);
        }
    } else {
        (void)ms_session_end_of_input(s);
    }
    CHECK(n_au == 2, "no dropped no-AUD au under backpressure");
    (void)n_cfg;
    ms_session_destroy(s);

    /* 直接组帧器：装配上限、截断坏长度、畸形不越界。 */
    ms_default_allocator(&alloc);
    memset(&cap, 0, sizeof(cap));
    cap.au_rc = MS_OK;
    memset(&cb, 0, sizeof(cb));
    cb.ctx = &cap;
    cb.on_config = cap_on_cfg;
    cb.on_au = cap_on_au;
    ms_h264_init(&h, &alloc, 48, &cb);
    n = 0;
    CHECK(ms_synth_h264_sps(tmp, sizeof(tmp), &l, 4, 16, 16, 1) == 0, "sps");
    CHECK(append_buf(payload, sizeof(payload), &n, tmp, l) == 0, "s");
    CHECK(ms_synth_h264_pps(tmp, sizeof(tmp), &l, 4) == 0, "pps");
    CHECK(append_buf(payload, sizeof(payload), &n, tmp, l) == 0, "p");
    /* 用超长占位尾把 AU 顶过 48 字节上限。 */
    {
        uint8_t sl[128];
        size_t sln = 0;
        /* 手工：起始码 + 头 + 大载荷，迫使超过 max_au。 */
        sl[sln++] = 0;
        sl[sln++] = 0;
        sl[sln++] = 0;
        sl[sln++] = 1;
        sl[sln++] = 0x65;
        memset(sl + sln, 0x80, 80);
        sln += 80;
        CHECK(append_buf(payload, sizeof(payload), &n, sl, sln) == 0, "huge");
    }
    st = (int)ms_h264_push(&h, payload, n, &c);
    CHECK(st == (int)MS_ERR_MALFORMED, "assembler max_au overflow");
    ms_h264_destroy(&h);

    memset(&cap, 0, sizeof(cap));
    cap.au_rc = MS_OK;
    ms_h264_init(&h, &alloc, 4096, &cb);
    n = 0;
    CHECK(ms_synth_h264_sps(tmp, sizeof(tmp), &l, 4, 16, 16, 1) == 0, "sps");
    CHECK(append_buf(payload, sizeof(payload), &n, tmp, l) == 0, "s");
    CHECK(ms_synth_h264_pps(tmp, sizeof(tmp), &l, 4) == 0, "pps");
    CHECK(append_buf(payload, sizeof(payload), &n, tmp, l) == 0, "p");
    payload[n++] = 0x00;
    payload[n++] = 0x00;
    payload[n++] = 0x01; /* 截断起始码，无 NAL 头 */
    CHECK_ST(ms_h264_push(&h, payload, n, &c), MS_OK);
    CHECK_ST(ms_h264_end(&h), MS_OK);
    CHECK(cap.n_au == 0, "truncated start code yields no au");
    ms_h264_destroy(&h);

    memset(&cap, 0, sizeof(cap));
    cap.au_rc = MS_OK;
    ms_h264_init(&h, &alloc, 4096, &cb);
    /* 畸形：VCL 条带头过短（无 AUD） */
    n = 0;
    CHECK(ms_synth_h264_sps(tmp, sizeof(tmp), &l, 4, 16, 16, 1) == 0, "sps");
    CHECK(append_buf(payload, sizeof(payload), &n, tmp, l) == 0, "s");
    CHECK(ms_synth_h264_pps(tmp, sizeof(tmp), &l, 4) == 0, "pps");
    CHECK(append_buf(payload, sizeof(payload), &n, tmp, l) == 0, "p");
    payload[n++] = 0x00;
    payload[n++] = 0x00;
    payload[n++] = 0x00;
    payload[n++] = 0x01;
    payload[n++] = 0x65;
    payload[n++] = 0x00; /* 不够一个条带头 */
    st = (int)ms_h264_push(&h, payload, n, &c);
    if (st == (int)MS_OK) {
        st = (int)ms_h264_end(&h);
    }
    CHECK(st == (int)MS_ERR_MALFORMED, "short slice header");
    ms_h264_destroy(&h);

    printf("  test_h264_noaud_fragments_flush_limit ok\n");
    return 0;
}


typedef struct live_alloc {
    size_t live;
    size_t peak;
    size_t allocs;
    size_t fail_after;
    size_t first_size;
    size_t second_size;
} live_alloc;

typedef struct live_hdr {
    size_t size;
} live_hdr;

static void *live_alloc_fn(void *ctx, size_t size)
{
    live_alloc *t = (live_alloc *)ctx;
    live_hdr *h;
    t->allocs++;
    if (t->fail_after != 0 && t->allocs >= t->fail_after) {
        return NULL;
    }
    h = (live_hdr *)malloc(sizeof(*h) + size);
    if (h == NULL) {
        return NULL;
    }
    h->size = size;
    t->live += size;
    if (t->live > t->peak) {
        t->peak = t->live;
    }
    if (t->first_size == 0) {
        t->first_size = size;
    } else if (t->second_size == 0) {
        t->second_size = size;
    }
    return (void *)(h + 1);
}

static void live_free_fn(void *ctx, void *ptr)
{
    live_alloc *t = (live_alloc *)ctx;
    live_hdr *h;
    if (ptr == NULL) {
        return;
    }
    h = ((live_hdr *)ptr) - 1;
    if (t->live >= h->size) {
        t->live -= h->size;
    } else {
        t->live = 0;
    }
    free(h);
}

static void live_setup(live_alloc *la, ms_allocator *a)
{
    memset(la, 0, sizeof(*la));
    a->alloc_fn = live_alloc_fn;
    a->free_fn = live_free_fn;
    a->ctx = la;
}

static int test_session_limits_budget_contract(void)
{
    ms_session *s = NULL;
    ms_session_limits lim, defn, abs;
    size_t used = 0, peak = 0, budget = 0;

    ms_session_limits_default(&defn);
    ms_session_limits_absolute(&abs);
    CHECK(defn.struct_size == sizeof(defn), "default struct_size");
    CHECK(defn.max_assembly_bytes == (size_t)2 * 1024 * 1024, "default assembly 2MiB");
    CHECK(defn.session_budget_bytes == (size_t)16 * 1024 * 1024, "default budget 16MiB");
    CHECK(abs.max_assembly_bytes == (size_t)16 * 1024 * 1024, "abs assembly");
    CHECK(abs.session_budget_bytes == (size_t)64 * 1024 * 1024, "abs budget");
    CHECK(defn.max_assembly_bytes <= abs.max_assembly_bytes, "assembly default <= abs");

    lim = defn;
    lim.max_assembly_bytes = defn.max_assembly_bytes + 1;
    CHECK_ST(ms_session_create(NULL, &lim, &s), MS_ERR_INVALID_ARG);
    CHECK(s == NULL, "assembly above default rejected");
    lim = defn;
    lim.session_budget_bytes = defn.session_budget_bytes + 1;
    CHECK_ST(ms_session_create(NULL, &lim, &s), MS_ERR_INVALID_ARG);

    /* struct_size=0：新字段即使被写入也不应生效。 */
    lim = defn;
    lim.struct_size = 0;
    lim.session_budget_bytes = 4096;
    lim.max_assembly_bytes = 128;
    lim.au_queue_bytes = 4096;
    CHECK_ST(ms_session_create(NULL, &lim, &s), MS_OK);
    ms_session_debug_budget(s, &used, &peak, &budget);
    CHECK(budget == defn.session_budget_bytes, "struct_size 0 ignores new fields");
    CHECK(used == 0, "no dynamic buffers at create");
    ms_session_destroy(s);

    printf("  test_session_limits_budget_contract ok\n");
    return 0;
}

static int test_budget_assembly_queue_and_backpressure(void)
{
    uint8_t a1[512], a2[256], both[768];
    size_t l1 = 0, l2 = 0, c = 0;
    ms_session *s = NULL;
    ms_session_limits lim;
    ms_event ev;
    int n_au, i, st;

    CHECK(make_h264_au(a1, sizeof(a1), &l1, 4, 4, 16, 16, 1, NULL, 0) == 0, "a1");
    CHECK(make_h264_au(a2, sizeof(a2), &l2, 4, 4, 16, 16, 0, NULL, 0) == 0, "a2");
    memcpy(both, a1, l1);
    memcpy(both + l1, a2, l2);

    /* 缩小输出队列，保持默认装配/总预算：两帧不得丢，不得 MALFORMED。 */
    ms_session_limits_default(&lim);
    lim.au_queue_bytes = (l1 + 64);
    st = push_video_payload_lim(both, l1 + l2, 31000, 1, &s, &c, &lim);
    CHECK(st == (int)MS_OK || st == (int)MS_LIMIT, "queue backpressure not malformed");
    n_au = 0;
    for (i = 0; i < 64; i++) {
        ms_status rst = ms_session_pull(s, &ev);
        if (rst == MS_AGAIN) {
            break;
        }
        CHECK_ST(rst, MS_OK);
        if (ev.type == MS_EVENT_AU) {
            n_au++;
        }
        CHECK_ST(ms_session_release(s, ev.release_id), MS_OK);
    }
    if (n_au < 2) {
        CHECK_ST(ms_session_end_of_input(s), MS_OK);
        for (i = 0; i < 64; i++) {
            ms_status rst = ms_session_pull(s, &ev);
            if (rst == MS_AGAIN || rst == MS_EOS) {
                break;
            }
            CHECK_ST(rst, MS_OK);
            if (ev.type == MS_EVENT_AU) {
                n_au++;
            }
            CHECK_ST(ms_session_release(s, ev.release_id), MS_OK);
        }
    }
    CHECK(n_au == 2, "no dropped frame with shrunken queue");
    ms_session_destroy(s);

    /* 单个 PES/AU 超过装配上限：MALFORMED，与输出队列无关。 */
    {
        uint8_t raw[200];
        uint8_t au[512];
        size_t alen = 0;
        memset(raw, 0x5A, sizeof(raw));
        CHECK(make_h264_au(au, sizeof(au), &alen, 4, 4, 16, 16, 1, raw, sizeof(raw)) == 0,
              "large au");
        CHECK(alen > 64, "payload exceeds tiny assembly");
        ms_session_limits_default(&lim);
        lim.max_assembly_bytes = 64;
        lim.au_queue_bytes = 64 * 1024;
        st = push_video_payload_lim(au, alen, 32000, 1, &s, &c, &lim);
        if (st == (int)MS_OK) {
            st = (int)ms_session_end_of_input(s);
        }
        CHECK(st == (int)MS_ERR_MALFORMED, "over assembly cap is malformed");
        ms_session_destroy(s);
    }

    printf("  test_budget_assembly_queue_and_backpressure ok\n");
    return 0;
}

static int test_budget_multitrack_peak_replace_alloc(void)
{
    live_alloc la;
    ms_allocator alloc;
    ms_session *s = NULL;
    ms_session_limits lim;
    ms_synth_buf b;
    uint8_t vpay[512], apay[512], raw[200], adts_raw[200];
    size_t vlen = 0, alen = 0, c = 0;
    size_t used = 0, peak = 0, budget = 0, used2 = 0, peak2 = 0;
    size_t used_flush = 0;
    size_t session_obj = 0, slots_obj = 0;
    ms_event ev;
    int n_au, i, st;
    size_t fail_at;

    memset(raw, 0x3C, sizeof(raw));
    memset(adts_raw, 0x11, sizeof(adts_raw));
    CHECK(make_h264_au(vpay, sizeof(vpay), &vlen, 4, 4, 16, 16, 1, raw, sizeof(raw)) == 0,
          "vp");
    CHECK(ms_synth_adts_frame(apay, sizeof(apay), &alen, 1, 3, 2, adts_raw, sizeof(adts_raw)) == 0,
          "ap");

    live_setup(&la, &alloc);
    ms_session_limits_default(&lim);
    lim.session_budget_bytes = 64 * 1024;
    lim.max_assembly_bytes = 32 * 1024;
    lim.au_queue_bytes = 16 * 1024;
    CHECK_ST(ms_session_create(&alloc, &lim, &s), MS_OK);
    session_obj = la.first_size;
    /* 第二次分配是输出槽表（在动态记账之前）。 */
    CHECK(la.allocs >= 2, "session + slots");
    slots_obj = (la.live > session_obj) ? (size_t)0 : 0;
    (void)slots_obj;
    CHECK(session_obj >= 4096 && session_obj < 50000, "fixed session without 32 embedded assemblers");
    ms_session_debug_budget(s, &used, &peak, &budget);
    CHECK(used == 0 && peak == 0, "budget unused at create");
    CHECK(budget == 64 * 1024, "budget applied");

    ms_synth_init(&b);
    CHECK(make_basic_ts(&b, 188) == 0, "ts");
    CHECK(ms_synth_pes(&b, 0x101, 0xE0, 1, 41000, vpay, vlen, 188) == 0, "vpes");
    CHECK(ms_synth_pes(&b, 0x102, 0xC0, 1, 41100, apay, alen, 188) == 0, "apes");
    st = (int)ms_session_push(s, b.data, b.len, &c);
    CHECK(st == (int)MS_OK || st == (int)MS_LIMIT, "multi-track push");
    if (st == (int)MS_OK || st == (int)MS_LIMIT) {
        (void)ms_session_end_of_input(s);
    }
    n_au = 0;
    for (i = 0; i < 64; i++) {
        ms_status rst = ms_session_pull(s, &ev);
        if (rst == MS_AGAIN || rst == MS_EOS) {
            break;
        }
        if (rst == MS_LIMIT) {
            break;
        }
        CHECK_ST(rst, MS_OK);
        if (ev.type == MS_EVENT_AU) {
            n_au++;
        }
        CHECK_ST(ms_session_release(s, ev.release_id), MS_OK);
    }
    CHECK(n_au >= 2, "video and audio au under budget");
    ms_session_debug_budget(s, &used, &peak, &budget);
    CHECK(peak >= used, "accounted peak >= used");
    CHECK(la.peak >= la.live, "allocator peak >= live");
    CHECK(la.peak >= session_obj + used, "heap peak covers fixed + accounted");
    CHECK_ST(ms_session_flush(s), MS_OK);
    ms_session_debug_budget(s, &used_flush, NULL, NULL);
    CHECK(used_flush <= used, "flush releases assembly/input");
    CHECK_ST(ms_session_reset(s), MS_OK);
    ms_session_debug_budget(s, &used2, &peak2, NULL);
    CHECK(used2 == 0, "reset returns accounted used to 0");
    CHECK(peak2 >= peak, "peak is not reset downward");
    printf("  measured (this run): session_object=%zu slots=%zu "
           "accounted_used_after_au=%zu accounted_peak=%zu "
           "alloc_live_peak=%zu budget=%zu flush_used=%zu reset_used=%zu\n",
           session_obj, la.second_size, used, peak, la.peak, budget,
           used_flush, used2);
    ms_synth_free(&b);
    ms_session_destroy(s);
    CHECK(la.live == 0, "destroy frees live heap");

    /* 小预算：无可排空事件时 LIMIT 不可靠 drain 缓解。 */
    live_setup(&la, &alloc);
    ms_session_limits_default(&lim);
    lim.session_budget_bytes = 512;
    lim.max_assembly_bytes = 32 * 1024;
    lim.probe_bytes = 16 * 1024;
    CHECK_ST(ms_session_create(&alloc, &lim, &s), MS_OK);
    ms_synth_init(&b);
    CHECK(make_basic_ts(&b, 188) == 0, "ts2");
    CHECK(ms_synth_pes(&b, 0x101, 0xE0, 1, 42000, vpay, vlen, 188) == 0, "v2");
    st = (int)ms_session_push(s, b.data, b.len, &c);
    CHECK(st == (int)MS_LIMIT || st == (int)MS_OK || st == (int)MS_ERR_NOMEM,
          "tiny budget may limit");
    if (st == (int)MS_LIMIT) {
        CHECK(ms_session_debug_limit_relievable(s) == 0 || 1, "query relievable");
        if (!ms_session_debug_limit_relievable(s)) {
            ms_status pst = ms_session_pull(s, &ev);
            CHECK(pst == MS_AGAIN || pst == MS_OK, "no-progress or leftover ev");
            if (pst == MS_AGAIN) {
                size_t c2 = 99;
                ms_status st2 = ms_session_push(s, b.data + c, b.len - c, &c2);
                CHECK(st2 == MS_LIMIT, "retry same input still LIMIT");
                CHECK(c2 == 0, "consumed 0, do not spin");
            } else {
                CHECK_ST(ms_session_release(s, ev.release_id), MS_OK);
            }
        }
        CHECK_ST(ms_session_flush(s), MS_OK);
        CHECK_ST(ms_session_reset(s), MS_OK);
    }
    ms_synth_free(&b);
    ms_session_destroy(s);

    /* 多轨累计：预算只够一路较大装配时，第二路 LIMIT，flush 后旧会话可用。 */
    {
        uint8_t bigv[512], biga[512], vraw[220], araw[220];
        size_t blv = 0, bla = 0;
        memset(vraw, 0x21, sizeof(vraw));
        memset(araw, 0x22, sizeof(araw));
        CHECK(make_h264_au(bigv, sizeof(bigv), &blv, 4, 4, 16, 16, 1, vraw, sizeof(vraw)) == 0,
              "bigv");
        CHECK(ms_synth_adts_frame(biga, sizeof(biga), &bla, 1, 3, 2, araw, sizeof(araw)) == 0,
              "biga");
        live_setup(&la, &alloc);
        ms_session_limits_default(&lim);
        lim.max_assembly_bytes = 8 * 1024;
        lim.au_queue_bytes = 8 * 1024;
        /* 预算小于两路 PES/ES 同时装配，但够一路加 PAT/PMT。 */
        lim.session_budget_bytes = 1536;
        CHECK_ST(ms_session_create(&alloc, &lim, &s), MS_OK);
        ms_synth_init(&b);
        CHECK(make_basic_ts(&b, 188) == 0, "ts3");
        CHECK(ms_synth_pes(&b, 0x101, 0xE0, 1, 43000, bigv, blv, 188) == 0, "bv");
        CHECK(ms_synth_pes(&b, 0x102, 0xC0, 1, 43100, biga, bla, 188) == 0, "ba");
        st = (int)ms_session_push(s, b.data, b.len, &c);
        CHECK(st == (int)MS_LIMIT || st == (int)MS_OK, "cumulative budget");
        /* 已入队事件仍可取（旧状态可用）。 */
        for (i = 0; i < 32; i++) {
            ms_status rst = ms_session_pull(s, &ev);
            if (rst == MS_AGAIN || rst == MS_EOS) {
                break;
            }
            if (rst != MS_OK) {
                break;
            }
            CHECK_ST(ms_session_release(s, ev.release_id), MS_OK);
        }
        CHECK_ST(ms_session_flush(s), MS_OK);
        CHECK_ST(ms_session_reset(s), MS_OK);
        ms_session_debug_budget(s, &used, NULL, NULL);
        CHECK(used == 0, "reset after cumulative LIMIT");
        ms_synth_free(&b);
        ms_session_destroy(s);
    }

    /* 参数集更换：先小 SPS，flush 后换更大 SPS；峰值被测到。 */
    live_setup(&la, &alloc);
    ms_session_limits_default(&lim);
    lim.session_budget_bytes = 32 * 1024;
    CHECK_ST(ms_session_create(&alloc, &lim, &s), MS_OK);
    {
        uint8_t payload[800], tmp[400];
        size_t n = 0, l = 0, used_a, peak_a, used_b, peak_b, live_peak_a;
        ms_synth_h264_slice_opts opt;
        ms_synth_init(&b);
        CHECK(make_basic_ts(&b, 188) == 0, "ts p");
        CHECK(ms_synth_h264_aud(tmp, sizeof(tmp), &l, 4) == 0, "aud");
        CHECK(append_buf(payload, sizeof(payload), &n, tmp, l) == 0, "a");
        CHECK(ms_synth_h264_sps(tmp, sizeof(tmp), &l, 4, 16, 16, 1) == 0, "sps");
        CHECK(append_buf(payload, sizeof(payload), &n, tmp, l) == 0, "s");
        CHECK(ms_synth_h264_pps(tmp, sizeof(tmp), &l, 4) == 0, "pps");
        CHECK(append_buf(payload, sizeof(payload), &n, tmp, l) == 0, "p");
        h264_slice_opt_init(&opt, 4, 1, 0, 0);
        CHECK(append_slice_opt(payload, sizeof(payload), &n, &opt) == 0, "sl");
        /* 再跟一个 AUD，收口最后一枚 NAL，避免 end_of_input 后不能再 push。 */
        CHECK(ms_synth_h264_aud(tmp, sizeof(tmp), &l, 4) == 0, "tail aud");
        CHECK(append_buf(payload, sizeof(payload), &n, tmp, l) == 0, "ta");
        CHECK(ms_synth_pes(&b, 0x101, 0xE0, 1, 45000, payload, n, 188) == 0, "p1");
        CHECK_ST(ms_session_push(s, b.data, b.len, &c), MS_OK);
        for (i = 0; i < 32; i++) {
            ms_status rst = ms_session_pull(s, &ev);
            if (rst == MS_AGAIN || rst == MS_EOS) {
                break;
            }
            CHECK_ST(rst, MS_OK);
            CHECK_ST(ms_session_release(s, ev.release_id), MS_OK);
        }
        CHECK_ST(ms_session_flush(s), MS_OK);
        ms_session_debug_budget(s, &used_a, &peak_a, NULL);
        live_peak_a = la.peak;
        CHECK(used_a > 0, "param sets remain after flush");
        /* 更大的含 EPB 的 SPS 替换同 id；只追加 PES，不复位 CC、不重送 PAT。 */
        ms_synth_free(&b);
        ms_synth_init(&b);
        n = 0;
        CHECK(ms_synth_h264_aud(tmp, sizeof(tmp), &l, 4) == 0, "aud2");
        CHECK(append_buf(payload, sizeof(payload), &n, tmp, l) == 0, "a2");
        CHECK(ms_synth_h264_sps_epb(tmp, sizeof(tmp), &l, 4) == 0, "big sps");
        CHECK(append_buf(payload, sizeof(payload), &n, tmp, l) == 0, "sb");
        CHECK(ms_synth_h264_pps(tmp, sizeof(tmp), &l, 4) == 0, "pps2");
        CHECK(append_buf(payload, sizeof(payload), &n, tmp, l) == 0, "p2");
        h264_slice_opt_init(&opt, 4, 1, 0, 0);
        CHECK(append_slice_opt(payload, sizeof(payload), &n, &opt) == 0, "sl2");
        CHECK(ms_synth_h264_aud(tmp, sizeof(tmp), &l, 4) == 0, "tail2");
        CHECK(append_buf(payload, sizeof(payload), &n, tmp, l) == 0, "t2");
        CHECK(ms_synth_pes(&b, 0x101, 0xE0, 1, 45100, payload, n, 188) == 0, "p2pes");
        st = (int)ms_session_push(s, b.data, b.len, &c);
        CHECK(st == (int)MS_OK || st == (int)MS_LIMIT, "replace sps");
        if (st == (int)MS_OK) {
            CHECK_ST(ms_session_end_of_input(s), MS_OK);
        }
        ms_session_debug_budget(s, &used_b, &peak_b, NULL);
        CHECK(peak_b >= used_a, "accounted peak measured after replace");
        CHECK(la.peak >= live_peak_a, "allocator peak measured (may include transient)");
        (void)used_b;
        ms_synth_free(&b);
    }
    ms_session_destroy(s);

    /* 分配失败回滚：已入队事件仍可取，会话未粘滞失败。 */
    live_setup(&la, &alloc);
    ms_session_limits_default(&lim);
    CHECK_ST(ms_session_create(&alloc, &lim, &s), MS_OK);
    fail_at = la.allocs;
    ms_synth_init(&b);
    CHECK(make_basic_ts(&b, 188) == 0, "ts f");
    CHECK(ms_synth_pes(&b, 0x101, 0xE0, 1, 46000, vpay, vlen, 188) == 0, "vf");
    CHECK_ST(ms_session_push(s, b.data, b.len, &c), MS_OK);
    CHECK_ST(ms_session_end_of_input(s), MS_OK);
    /* 先取走部分，再让后续分配失败。 */
    n_au = 0;
    for (i = 0; i < 8; i++) {
        ms_status rst = ms_session_pull(s, &ev);
        if (rst != MS_OK) {
            break;
        }
        if (ev.type == MS_EVENT_AU) {
            n_au++;
        }
        CHECK_ST(ms_session_release(s, ev.release_id), MS_OK);
    }
    CHECK(n_au >= 1, "have au before inject fail");
    CHECK_ST(ms_session_reset(s), MS_OK);
    fail_at = la.allocs + 1;
    la.fail_after = fail_at;
    ms_synth_free(&b);
    ms_synth_init(&b);
    CHECK(make_basic_ts(&b, 188) == 0, "ts f2");
    CHECK(ms_synth_pes(&b, 0x101, 0xE0, 1, 46100, vpay, vlen, 188) == 0, "vf2");
    st = (int)ms_session_push(s, b.data, b.len, &c);
    CHECK(st == (int)MS_ERR_NOMEM || st == (int)MS_OK || st == (int)MS_LIMIT,
          "inject alloc fail");
    /* 不要求粘滞失败；reset 必须仍可用。 */
    la.fail_after = 0;
    CHECK_ST(ms_session_reset(s), MS_OK);
    ms_synth_free(&b);
    ms_synth_init(&b);
    CHECK(make_basic_ts(&b, 188) == 0, "ts f3");
    CHECK(ms_synth_pes(&b, 0x101, 0xE0, 1, 46200, vpay, vlen, 188) == 0, "vf3");
    CHECK_ST(ms_session_push(s, b.data, b.len, &c), MS_OK);
    CHECK_ST(ms_session_end_of_input(s), MS_OK);
    n_au = 0;
    for (i = 0; i < 32; i++) {
        ms_status rst = ms_session_pull(s, &ev);
        if (rst == MS_AGAIN || rst == MS_EOS) {
            break;
        }
        CHECK_ST(rst, MS_OK);
        if (ev.type == MS_EVENT_AU) {
            n_au++;
        }
        CHECK_ST(ms_session_release(s, ev.release_id), MS_OK);
    }
    CHECK(n_au >= 1, "usable after alloc fail rollback");
    ms_synth_free(&b);
    ms_session_destroy(s);

    printf("  test_budget_multitrack_peak_replace_alloc ok\n");
    return 0;
}

static int test_budget_malformed_seed_stress(void)
{
    /* 固定种子、有限次数，不是正式 fuzz。 */
    const uint32_t seed0 = 0x4D534D31u;
    uint32_t rng = seed0;
    ms_session *s = NULL;
    ms_session_limits lim;
    live_alloc la;
    ms_allocator alloc;
    int i, pkt;
    size_t used = 0;

    live_setup(&la, &alloc);
    ms_session_limits_default(&lim);
    lim.session_budget_bytes = 32 * 1024;
    lim.max_assembly_bytes = 8 * 1024;
    lim.au_queue_bytes = 8 * 1024;
    lim.probe_bytes = 8 * 1024;
    CHECK_ST(ms_session_create(&alloc, &lim, &s), MS_OK);
    for (i = 0; i < 48; i++) {
        uint8_t pktbuf[188 * 3];
        size_t c = 0;
        ms_status st;
        for (pkt = 0; pkt < 3; pkt++) {
            size_t j;
            uint8_t *p = pktbuf + (size_t)pkt * 188u;
            p[0] = ((rng = rng ^ (rng << 13), rng = rng ^ (rng >> 17),
                     rng = rng ^ (rng << 5), rng) & 3u) == 0
                       ? 0x47
                       : (uint8_t)(rng & 0xFFu);
            for (j = 1; j < 188; j++) {
                rng ^= rng << 13;
                rng ^= rng >> 17;
                rng ^= rng << 5;
                p[j] = (uint8_t)(rng & 0xFFu);
            }
        }
        st = ms_session_push(s, pktbuf, sizeof(pktbuf), &c);
        if (st == MS_LIMIT) {
            ms_event ev;
            ms_status pst = ms_session_pull(s, &ev);
            if (pst == MS_OK) {
                (void)ms_session_release(s, ev.release_id);
            } else if (pst == MS_AGAIN && c == 0) {
                CHECK_ST(ms_session_reset(s), MS_OK);
            }
        } else if (st != MS_OK && st != MS_AGAIN && st != MS_ERR_MALFORMED &&
                   st != MS_ERR_UNKNOWN && st != MS_ERR_UNSUPPORTED &&
                   st != MS_ERR_OVERFLOW && st != MS_ERR_NOMEM) {
            CHECK(0, "unexpected status in seeded stress");
        }
        if (st == MS_ERR_MALFORMED || st == MS_ERR_UNKNOWN ||
            st == MS_ERR_UNSUPPORTED || st == MS_ERR_OVERFLOW) {
            CHECK_ST(ms_session_reset(s), MS_OK);
        }
    }
    ms_session_debug_budget(s, &used, NULL, NULL);
    CHECK_ST(ms_session_reset(s), MS_OK);
    ms_session_debug_budget(s, &used, NULL, NULL);
    CHECK(used == 0, "stress reset baseline");
    ms_session_destroy(s);
    CHECK(la.live == 0, "stress destroy");
    printf("  test_budget_malformed_seed_stress ok\n");
    return 0;
}


static int test_lazy_track_state_alloc(void)
{
    live_alloc la;
    ms_allocator alloc;
    ms_session *s = NULL;
    ms_session_limits lim;
    ms_synth_buf b;
    uint8_t vpay[512], apay[512], raw[200], adts_raw[200];
    size_t vlen = 0, alen = 0, c = 0;
    size_t used = 0, peak = 0, budget = 0;
    size_t used_pmt = 0, used_flush = 0, used_v = 0, used_va = 0;
    size_t used_pmt2 = 0, live_after_reset = 0;
    size_t session_obj = 0, slots_obj = 0;
    size_t baseline_live = 0;
    ms_event ev;
    int i, st, n_track, n_au, r, same_pid_kept;
    ms_synth_stream streams[2];

    /* 与 P1e 同一合成双轨样本、同一工具链、同一 budget=65536 的基线：
     * session_object=142584 accounted_used_after_au=6723 accounted_peak=7241
     * alloc_live_peak=149921 flush_used=544 reset_used=0
     * 本阶段把 32 路嵌入的 H.264/ADTS 改为按需分配后，固定对象下降、
     * 记账峰值上升约 3808（sizeof(ms_h264)+sizeof(ms_adts)+记账头），
     * 双轨样本的 allocator 总峰值下降。32 路全开时总峰值可能相近或略高。 */

    memset(raw, 0x3C, sizeof(raw));
    memset(adts_raw, 0x11, sizeof(adts_raw));
    CHECK(make_h264_au(vpay, sizeof(vpay), &vlen, 4, 4, 16, 16, 1, raw, sizeof(raw)) == 0,
          "vp");
    CHECK(ms_synth_adts_frame(apay, sizeof(apay), &alen, 1, 3, 2, adts_raw, sizeof(adts_raw)) == 0,
          "ap");

    live_setup(&la, &alloc);
    ms_session_limits_default(&lim);
    lim.session_budget_bytes = 64 * 1024;
    lim.max_assembly_bytes = 32 * 1024;
    lim.au_queue_bytes = 16 * 1024;
    CHECK_ST(ms_session_create(&alloc, &lim, &s), MS_OK);
    session_obj = la.first_size;
    slots_obj = la.second_size;
    baseline_live = la.live;
    CHECK(session_obj < 50000, "lazy tracks shrink fixed session");
    CHECK(session_obj + slots_obj == baseline_live, "create only session+slots");
    ms_session_debug_budget(s, &used, &peak, &budget);
    CHECK(used == 0 && peak == 0, "no dynamic at create");

    /* 仅 PAT/PMT：登记轨道但不分配解析状态。 */
    ms_synth_init(&b);
    CHECK(make_basic_ts(&b, 188) == 0, "pmt only");
    st = (int)ms_session_push(s, b.data, b.len, &c);
    CHECK(st == (int)MS_OK, "pmt push");
    n_track = 0;
    for (i = 0; i < 32; i++) {
        ms_status rst = ms_session_pull(s, &ev);
        if (rst == MS_AGAIN) {
            break;
        }
        CHECK_ST(rst, MS_OK);
        if (ev.type == MS_EVENT_TRACK) {
            n_track++;
        }
        CHECK_ST(ms_session_release(s, ev.release_id), MS_OK);
    }
    CHECK(n_track >= 2, "tracks announced before codec state");
    ms_session_debug_budget(s, &used_pmt, NULL, NULL);
    CHECK(used_pmt < 16384, "pmt-only does not allocate 32 assemblers");
    CHECK_ST(ms_session_flush(s), MS_OK);
    ms_session_debug_budget(s, &used_flush, NULL, NULL);
    CHECK(used_flush == 0, "flush after pmt-only frees in_buf, no codec objects");
    CHECK(la.live == baseline_live, "live back to session+slots after pmt flush");
    ms_synth_free(&b);

    /* 首个视频 PES 才分配 H.264 状态（含参数集槽）。 */
    ms_synth_init(&b);
    CHECK(ms_synth_pes(&b, 0x101, 0xE0, 1, 41000, vpay, vlen, 188) == 0, "vpes");
    st = (int)ms_session_push(s, b.data, b.len, &c);
    CHECK(st == (int)MS_OK || st == (int)MS_LIMIT, "video pes");
    n_au = 0;
    for (i = 0; i < 32; i++) {
        ms_status rst = ms_session_pull(s, &ev);
        if (rst == MS_AGAIN || rst == MS_EOS || rst == MS_LIMIT) {
            break;
        }
        CHECK_ST(rst, MS_OK);
        if (ev.type == MS_EVENT_AU) {
            n_au++;
        }
        CHECK_ST(ms_session_release(s, ev.release_id), MS_OK);
    }
    ms_session_debug_budget(s, &used_v, NULL, NULL);
    CHECK(used_v >= 3500, "h264 object counted in budget");
    ms_synth_free(&b);

    ms_synth_init(&b);
    CHECK(ms_synth_pes(&b, 0x102, 0xC0, 1, 41100, apay, alen, 188) == 0, "apes");
    st = (int)ms_session_push(s, b.data, b.len, &c);
    CHECK(st == (int)MS_OK || st == (int)MS_LIMIT, "audio pes");
    for (i = 0; i < 32; i++) {
        ms_status rst = ms_session_pull(s, &ev);
        if (rst == MS_AGAIN || rst == MS_EOS || rst == MS_LIMIT) {
            break;
        }
        CHECK_ST(rst, MS_OK);
        CHECK_ST(ms_session_release(s, ev.release_id), MS_OK);
    }
    ms_session_debug_budget(s, &used_va, NULL, NULL);
    CHECK(used_va >= used_v + 160, "adts object counted on second track");
    ms_synth_free(&b);

    /* 重复 reset：记账回零，再推同一双轨流仍可用。 */
    for (r = 0; r < 3; r++) {
        CHECK_ST(ms_session_reset(s), MS_OK);
        ms_session_debug_budget(s, &used, NULL, NULL);
        CHECK(used == 0, "reset used 0");
        CHECK(la.live == baseline_live, "reset live is session+slots");
        ms_synth_init(&b);
        CHECK(make_basic_ts(&b, 188) == 0, "reset ts");
        CHECK(ms_synth_pes(&b, 0x101, 0xE0, 1, 41000, vpay, vlen, 188) == 0, "rv");
        CHECK(ms_synth_pes(&b, 0x102, 0xC0, 1, 41100, apay, alen, 188) == 0, "ra");
        c = 0;
        st = (int)ms_session_push(s, b.data, b.len, &c);
        CHECK(st == (int)MS_OK || st == (int)MS_LIMIT, "reset push");
        if (st == (int)MS_OK) {
            CHECK_ST(ms_session_end_of_input(s), MS_OK);
        }
        n_au = 0;
        for (i = 0; i < 64; i++) {
            ms_status rst = ms_session_pull(s, &ev);
            if (rst == MS_AGAIN || rst == MS_EOS || rst == MS_LIMIT) {
                break;
            }
            CHECK_ST(rst, MS_OK);
            if (ev.type == MS_EVENT_AU) {
                n_au++;
            }
            CHECK_ST(ms_session_release(s, ev.release_id), MS_OK);
        }
        CHECK(n_au >= 2, "au after repeated reset");
        ms_synth_free(&b);
    }

    /* 循环以 EOS 结束；reset 后再装配双轨，便于测 flush 保留对象和 PMT 变更。 */
    CHECK_ST(ms_session_reset(s), MS_OK);
    ms_synth_init(&b);
    CHECK(make_basic_ts(&b, 188) == 0, "pre-pmt ts");
    CHECK(ms_synth_pes(&b, 0x101, 0xE0, 1, 41000, vpay, vlen, 188) == 0, "pv");
    CHECK(ms_synth_pes(&b, 0x102, 0xC0, 1, 41100, apay, alen, 188) == 0, "pa");
    c = 0;
    st = (int)ms_session_push(s, b.data, b.len, &c);
    CHECK(st == (int)MS_OK || st == (int)MS_LIMIT, "pre-pmt push");
    for (i = 0; i < 64; i++) {
        ms_status rst = ms_session_pull(s, &ev);
        if (rst == MS_AGAIN || rst == MS_LIMIT) {
            break;
        }
        CHECK_ST(rst, MS_OK);
        CHECK_ST(ms_session_release(s, ev.release_id), MS_OK);
    }
    ms_synth_free(&b);
    CHECK_ST(ms_session_flush(s), MS_OK);
    ms_session_debug_budget(s, &used_flush, NULL, NULL);
    CHECK(used_flush >= 3500, "flush keeps codec objects");

    /* PMT 版本变化：释放旧解析状态（含未再出现的音频轨）。 */
    memset(streams, 0, sizeof(streams));
    streams[0].pid = 0x101;
    streams[0].stream_type = 0x1B;
    ms_synth_init(&b);
    CHECK(ms_synth_pat_pmt_version(&b, 1, 0x100, streams, 1, 0, 188, 1) == 0,
          "pmt v1 video only");
    c = 0;
    st = (int)ms_session_push(s, b.data, b.len, &c);
    CHECK(st == (int)MS_OK || st == (int)MS_LIMIT, "pmt update");
    same_pid_kept = 0;
    for (i = 0; i < 32; i++) {
        ms_status rst = ms_session_pull(s, &ev);
        if (rst == MS_AGAIN || rst == MS_LIMIT) {
            break;
        }
        CHECK_ST(rst, MS_OK);
        if (ev.type == MS_EVENT_TRACK && ev.track.id == 0x101) {
            same_pid_kept = 1;
        }
        CHECK_ST(ms_session_release(s, ev.release_id), MS_OK);
    }
    CHECK(same_pid_kept, "pmt update re-announces remaining pid");
    CHECK_ST(ms_session_flush(s), MS_OK);
    ms_session_debug_budget(s, &used_pmt2, NULL, NULL);
    CHECK(used_pmt2 < used_flush, "pmt change releases previous codec objects");
    CHECK(used_pmt2 < 1024, "no assembler until next pes after pmt change");
    ms_synth_free(&b);

    /* 同 PID 再次出现：再推视频 PES，仍可组帧。 */
    ms_synth_init(&b);
    CHECK(ms_synth_pes(&b, 0x101, 0xE0, 1, 42000, vpay, vlen, 188) == 0, "v again");
    c = 0;
    st = (int)ms_session_push(s, b.data, b.len, &c);
    CHECK(st == (int)MS_OK || st == (int)MS_LIMIT, "same pid after pmt");
    n_au = 0;
    for (i = 0; i < 32; i++) {
        ms_status rst = ms_session_pull(s, &ev);
        if (rst == MS_AGAIN || rst == MS_EOS || rst == MS_LIMIT) {
            break;
        }
        CHECK_ST(rst, MS_OK);
        if (ev.type == MS_EVENT_AU) {
            n_au++;
        }
        CHECK_ST(ms_session_release(s, ev.release_id), MS_OK);
    }
    (void)n_au;
    ms_session_debug_budget(s, &used_v, NULL, NULL);
    CHECK(used_v >= 3500, "h264 realloc after pmt change");
    ms_synth_free(&b);
    CHECK_ST(ms_session_reset(s), MS_OK);
    live_after_reset = la.live;
    CHECK(live_after_reset == baseline_live, "reset after pmt change");
    ms_session_destroy(s);
    CHECK(la.live == 0, "destroy");

    /* 失败注入：预算够 PMT 不够 H.264 对象；回滚、不泄漏、reset 可用。 */
    live_setup(&la, &alloc);
    ms_session_limits_default(&lim);
    lim.session_budget_bytes = 3072;
    lim.max_assembly_bytes = 32 * 1024;
    lim.au_queue_bytes = 16 * 1024;
    CHECK_ST(ms_session_create(&alloc, &lim, &s), MS_OK);
    baseline_live = la.live;
    ms_synth_init(&b);
    CHECK(make_basic_ts(&b, 188) == 0, "fail pmt");
    c = 0;
    st = (int)ms_session_push(s, b.data, b.len, &c);
    CHECK(st == (int)MS_OK, "pmt under 3072");
    n_track = 0;
    for (i = 0; i < 32; i++) {
        ms_status rst = ms_session_pull(s, &ev);
        if (rst == MS_AGAIN || rst == MS_LIMIT) {
            break;
        }
        CHECK_ST(rst, MS_OK);
        if (ev.type == MS_EVENT_TRACK) {
            n_track++;
        }
        CHECK_ST(ms_session_release(s, ev.release_id), MS_OK);
    }
    CHECK(n_track >= 2, "pmt tracks before inject");
    CHECK_ST(ms_session_flush(s), MS_OK);
    ms_session_debug_budget(s, &used, NULL, NULL);
    CHECK(used == 0, "flush before fail inject");
    ms_synth_free(&b);
    ms_synth_init(&b);
    CHECK(ms_synth_pes(&b, 0x101, 0xE0, 1, 43000, vpay, vlen, 188) == 0, "fail vpes");
    c = 0;
    st = (int)ms_session_push(s, b.data, b.len, &c);
    CHECK(st == (int)MS_LIMIT, "h264 object exceeds remaining budget");
    CHECK(ms_session_debug_limit_relievable(s) == 0, "object size not drained");
    ms_session_debug_budget(s, &used, NULL, NULL);
    CHECK(used < 3500, "h264 object rolled back; leftover in_buf/pes may remain");
    CHECK_ST(ms_session_flush(s), MS_OK);
    CHECK_ST(ms_session_reset(s), MS_OK);
    CHECK(la.live == baseline_live, "flush/reset frees leftover after budget reject");
    ms_session_debug_budget(s, &used, NULL, NULL);
    CHECK(used == 0, "reset after limit");
    ms_synth_free(&b);
    ms_session_destroy(s);
    CHECK(la.live == 0, "destroy after limit");

    /* allocator 失败注入：在首个 codec 对象处返回 NULL。 */
    live_setup(&la, &alloc);
    ms_session_limits_default(&lim);
    lim.session_budget_bytes = 64 * 1024;
    CHECK_ST(ms_session_create(&alloc, &lim, &s), MS_OK);
    baseline_live = la.live;
    ms_synth_init(&b);
    CHECK(make_basic_ts(&b, 188) == 0, "nomem pmt");
    c = 0;
    CHECK_ST(ms_session_push(s, b.data, b.len, &c), MS_OK);
    for (i = 0; i < 32; i++) {
        ms_status rst = ms_session_pull(s, &ev);
        if (rst == MS_AGAIN) {
            break;
        }
        CHECK_ST(rst, MS_OK);
        CHECK_ST(ms_session_release(s, ev.release_id), MS_OK);
    }
    CHECK_ST(ms_session_flush(s), MS_OK);
    CHECK(la.live == baseline_live, "pmt flushed");
    la.fail_after = la.allocs + 1;
    ms_synth_free(&b);
    ms_synth_init(&b);
    CHECK(ms_synth_pes(&b, 0x101, 0xE0, 1, 44000, vpay, vlen, 188) == 0, "nomem pes");
    c = 0;
    st = (int)ms_session_push(s, b.data, b.len, &c);
    CHECK(st == (int)MS_ERR_NOMEM || st == (int)MS_LIMIT, "codec alloc fail");
    ms_session_debug_budget(s, &used, NULL, NULL);
    CHECK(used == 0 || used < 4096, "partial pes may leave in_buf, no codec obj");
    CHECK_ST(ms_session_flush(s), MS_OK);
    CHECK_ST(ms_session_reset(s), MS_OK);
    CHECK(la.live == baseline_live, "nomem rolled back");
    ms_synth_free(&b);
    ms_session_destroy(s);
    CHECK(la.live == 0, "destroy after nomem");

    printf("  test_lazy_track_state_alloc ok (session_object=%zu slots=%zu "
           "p1e_session=142584 p1e_live_peak=149921 p1e_accounted_peak=7241 "
           "budget=65536)\n",
           session_obj, slots_obj);
    return 0;
}


static int test_unbounded_pes_eos_and_pts_wrap(void)
{
    ms_session *s = NULL;
    ms_synth_buf b;
    uint8_t au1[256], au2[256], raw[32];
    size_t l1 = 0, l2 = 0, c = 0;
    ms_event ev;
    int i, n_au = 0, n_key = 0;
    uint64_t pts[8];
    int has[8];
    const uint64_t near = 0x1FFFFFFFFULL - 3000ull;
    const uint64_t wrap_pts = (near + 6000ull) & 0x1FFFFFFFFULL;

    memset(raw, 0x5A, sizeof(raw));
    CHECK(make_h264_au(au1, sizeof(au1), &l1, 4, 4, 16, 16, 1, raw, sizeof(raw)) == 0, "au1");
    CHECK(make_h264_au(au2, sizeof(au2), &l2, 4, 4, 16, 16, 0, raw, sizeof(raw)) == 0, "au2");

    /* length=0 的两枚视频 PES：中间靠下一 PUSI 关闭，最后一枚靠 EOS。 */
    CHECK_ST(ms_session_create(NULL, NULL, &s), MS_OK);
    ms_synth_init(&b);
    CHECK(make_basic_ts(&b, 188) == 0, "ts");
    CHECK(ms_synth_pes_unbounded(&b, 0x101, 0xE0, 1, 90000, au1, l1, 188) == 0, "pes0");
    CHECK(ms_synth_pes_unbounded(&b, 0x101, 0xE0, 1, 96000, au2, l2, 188) == 0, "pes1");
    c = 0;
    CHECK_ST(ms_session_push(s, b.data, b.len, &c), MS_OK);
    CHECK_ST(ms_session_end_of_input(s), MS_OK);
    n_au = 0;
    n_key = 0;
    for (i = 0; i < 64; i++) {
        ms_status st = ms_session_pull(s, &ev);
        if (st == MS_AGAIN || st == MS_EOS) {
            break;
        }
        CHECK_ST(st, MS_OK);
        if (ev.type == MS_EVENT_AU && ev.track.codec == MS_CODEC_H264) {
            if (n_au < 8) {
                has[n_au] = ev.has_pts;
                pts[n_au] = ev.pts;
            }
            if (ev.is_keyframe) {
                n_key++;
            }
            n_au++;
        }
        CHECK_ST(ms_session_release(s, ev.release_id), MS_OK);
    }
    CHECK(n_au == 2, "eos closes last unbounded video pes");
    CHECK(n_key == 1, "first au is idr");
    CHECK(has[0] && pts[0] == 90000, "first pts");
    CHECK(has[1] && pts[1] == 96000, "second pts");
    ms_synth_free(&b);
    ms_session_destroy(s);

    /* 33 位 90 kHz 回绕：原样给出模 2^33 的 PTS，不自行展开。 */
    CHECK_ST(ms_session_create(NULL, NULL, &s), MS_OK);
    ms_synth_init(&b);
    CHECK(make_basic_ts(&b, 188) == 0, "ts2");
    CHECK(ms_synth_pes_unbounded(&b, 0x101, 0xE0, 1, near, au1, l1, 188) == 0, "w0");
    CHECK(ms_synth_pes_unbounded(&b, 0x101, 0xE0, 1, wrap_pts, au2, l2, 188) == 0, "w1");
    c = 0;
    CHECK_ST(ms_session_push(s, b.data, b.len, &c), MS_OK);
    CHECK_ST(ms_session_end_of_input(s), MS_OK);
    n_au = 0;
    for (i = 0; i < 64; i++) {
        ms_status st = ms_session_pull(s, &ev);
        if (st == MS_AGAIN || st == MS_EOS) {
            break;
        }
        CHECK_ST(st, MS_OK);
        if (ev.type == MS_EVENT_AU && ev.track.codec == MS_CODEC_H264) {
            if (n_au < 8) {
                has[n_au] = ev.has_pts;
                pts[n_au] = ev.pts;
            }
            n_au++;
        }
        CHECK_ST(ms_session_release(s, ev.release_id), MS_OK);
    }
    CHECK(n_au == 2, "wrap still two au");
    CHECK(has[0] && pts[0] == near, "pts before wrap");
    CHECK(has[1] && pts[1] == wrap_pts, "pts after wrap is 33-bit");
    CHECK(pts[1] < pts[0], "raw pts drops at wrap");
    ms_synth_free(&b);
    ms_session_destroy(s);

    /* 每 PES 一帧 44.1 kHz ADTS：必须用该 PES 的 PTS，不能沿用上一帧向下取整推算。 */
    {
        uint8_t a1[64], a2[64];
        size_t la1 = 0, la2 = 0;
        uint64_t p0 = 100000, p1 = 102090;
        CHECK(make_adts(a1, sizeof(a1), &la1, 4 /* 44.1 kHz */, 1, 8, 0x41) == 0, "441-1");
        CHECK(make_adts(a2, sizeof(a2), &la2, 4, 1, 8, 0x42) == 0, "441-2");
        CHECK_ST(ms_session_create(NULL, NULL, &s), MS_OK);
        ms_synth_init(&b);
        CHECK(make_basic_ts(&b, 188) == 0, "ts3");
        CHECK(ms_synth_pes(&b, 0x102, 0xC0, 1, p0, a1, la1, 188) == 0, "a-pes0");
        CHECK(ms_synth_pes(&b, 0x102, 0xC0, 1, p1, a2, la2, 188) == 0, "a-pes1");
        c = 0;
        CHECK_ST(ms_session_push(s, b.data, b.len, &c), MS_OK);
        CHECK_ST(ms_session_end_of_input(s), MS_OK);
        n_au = 0;
        for (i = 0; i < 64; i++) {
            ms_status st = ms_session_pull(s, &ev);
            if (st == MS_AGAIN || st == MS_EOS) {
                break;
            }
            CHECK_ST(st, MS_OK);
            if (ev.type == MS_EVENT_AU && ev.track.codec == MS_CODEC_AAC) {
                if (n_au < 8) {
                    has[n_au] = ev.has_pts;
                    pts[n_au] = ev.pts;
                }
                n_au++;
            }
            CHECK_ST(ms_session_release(s, ev.release_id), MS_OK);
        }
        CHECK(n_au == 2, "two 44.1k au");
        CHECK(has[0] && pts[0] == p0, "first 44.1k pes pts");
        CHECK(has[1] && pts[1] == p1, "second uses new pes pts not 2089 increment");
        ms_synth_free(&b);
        ms_session_destroy(s);
    }

    printf("  test_unbounded_pes_eos_and_pts_wrap ok\n");
    return 0;
}

static uint16_t p1h_pid(const uint8_t *u)
{
    return (uint16_t)(((u[1] & 0x1F) << 8) | u[2]);
}

static int p1h_list_pid(const ms_synth_buf *b, uint16_t pid, size_t *idx, int cap)
{
    size_t i, n = b->len / 188u;
    int k = 0;
    for (i = 0; i < n; i++) {
        if (p1h_pid(b->data + i * 188u) == pid) {
            if (k < cap) {
                idx[k] = i;
            }
            k++;
        }
    }
    return k;
}

static int p1h_copy(ms_synth_buf *dst, const ms_synth_buf *src)
{
    ms_synth_init(dst);
    if (src->len == 0) {
        return 0;
    }
    dst->data = (uint8_t *)malloc(src->len);
    if (dst->data == NULL) {
        return -1;
    }
    memcpy(dst->data, src->data, src->len);
    dst->len = src->len;
    dst->cap = src->len;
    return 0;
}

typedef struct p1h_drain {
    int n_au_a;
    int n_au_v;
    int n_disc;
    int n_cfg_a;
    int n_cfg_v;
    int bad;
    uint64_t tlepoch;
    int tlepoch_set;
    uint64_t pts_a[160];
    int has_a[160];
    int disc_a[160];
    uint64_t pts_v[16];
    int has_v[16];
    int key_v[16];
    int disc_v[16];
    uint32_t disc_id[16];
    size_t sz_a[16];
} p1h_drain;

static int p1h_push_end(ms_session **s, const ms_synth_buf *b)
{
    size_t c = 0;
    ms_status st;
    CHECK_ST(ms_session_create(NULL, NULL, s), MS_OK);
    st = ms_session_push(*s, b->data, b->len, &c);
    /* 多 AU 一次入队可能先碰到默认 8 槽 LIMIT；随后 pull 会 drain 剩余帧。 */
    CHECK(st == MS_OK || st == MS_LIMIT, "push ok or backpressure");
    CHECK(c == b->len || st == MS_LIMIT, "consumed or backpressure");
    st = ms_session_end_of_input(*s);
    CHECK(st == MS_OK || st == MS_LIMIT, "eos ok or backpressure");
    return 0;
}

static int p1h_collect_more(ms_session *s, p1h_drain *d)
{
    int i;
    for (i = 0; i < 512; i++) {
        ms_event ev;
        ms_status st = ms_session_pull(s, &ev);
        if (st == MS_AGAIN || st == MS_EOS) {
            break;
        }
        if (st != MS_OK) {
            d->bad = (int)st;
            return -1;
        }
        if (!d->tlepoch_set) {
            d->tlepoch = ev.timeline_epoch;
            d->tlepoch_set = 1;
        }
        CHECK(ev.timeline_epoch == d->tlepoch, "timeline_epoch unchanged");
        if (ev.type == MS_EVENT_DISCONTINUITY) {
            if (d->n_disc < 16) {
                d->disc_id[d->n_disc] = ev.track.id;
            }
            CHECK(ev.discontinuity == 1, "disc event flag");
            d->n_disc++;
        } else if (ev.type == MS_EVENT_AU && ev.track.codec == MS_CODEC_AAC) {
            if (d->n_au_a < 160) {
                d->pts_a[d->n_au_a] = ev.pts;
                d->has_a[d->n_au_a] = ev.has_pts;
                d->disc_a[d->n_au_a] = ev.discontinuity;
            }
            if (d->n_au_a < 16) {
                d->sz_a[d->n_au_a] = ev.data_size;
            }
            d->n_au_a++;
        } else if (ev.type == MS_EVENT_AU && ev.track.codec == MS_CODEC_H264) {
            if (d->n_au_v < 16) {
                d->pts_v[d->n_au_v] = ev.pts;
                d->has_v[d->n_au_v] = ev.has_pts;
                d->key_v[d->n_au_v] = ev.is_keyframe;
                d->disc_v[d->n_au_v] = ev.discontinuity;
            }
            d->n_au_v++;
        } else if (ev.type == MS_EVENT_CODEC_CONFIG) {
            if (ev.track.codec == MS_CODEC_AAC) {
                d->n_cfg_a++;
            } else if (ev.track.codec == MS_CODEC_H264) {
                d->n_cfg_v++;
            }
        }
        CHECK_ST(ms_session_release(s, ev.release_id), MS_OK);
    }
    return 0;
}

static int p1h_collect(ms_session *s, p1h_drain *d)
{
    memset(d, 0, sizeof(*d));
    return p1h_collect_more(s, d);
}

/* 交错 push / pull：超过 evq 容量时边取边送，避免只靠缩短用例绕过 LIMIT。 */
static int p1h_feed(ms_session **s, const ms_synth_buf *b, p1h_drain *d)
{
    size_t off = 0;
    int spins = 0;
    ms_status st;

    memset(d, 0, sizeof(*d));
    CHECK_ST(ms_session_create(NULL, NULL, s), MS_OK);
    while (off < b->len && spins < 100000) {
        size_t c = 0;
        spins++;
        st = ms_session_push(*s, b->data + off, b->len - off, &c);
        CHECK(st == MS_OK || st == MS_LIMIT, "feed push");
        off += c;
        if (p1h_collect_more(*s, d) != 0) {
            return -1;
        }
        if (st == MS_OK && c == 0 && off < b->len) {
            fprintf(stderr, "feed stalled off=%zu len=%zu\n", off, b->len);
            return -1;
        }
        if (st == MS_LIMIT && c == 0) {
            continue;
        }
    }
    CHECK(off == b->len, "feed consumed all");
    for (spins = 0; spins < 100000; spins++) {
        st = ms_session_end_of_input(*s);
        if (p1h_collect_more(*s, d) != 0) {
            return -1;
        }
        if (st == MS_OK) {
            return 0;
        }
        CHECK(st == MS_LIMIT, "feed eos backpressure");
    }
    return -1;
}

static int p1h_build_av(ms_synth_buf *b, unsigned a_start_cc, int a1_bytes, int a2_bytes)
{
    uint8_t a1[1024], a2[1024], v1[256], v2[256];
    size_t l1 = 0, l2 = 0, lv1 = 0, lv2 = 0;
    ms_synth_init(b);
    CHECK(make_basic_ts(b, 188) == 0, "patpmt");
    CHECK(make_h264_au(v1, sizeof(v1), &lv1, 4, 4, 16, 16, 1, NULL, 0) == 0, "v1");
    CHECK(ms_synth_pes(b, 0x101, 0xE0, 1, 10000, v1, lv1, 188) == 0, "vpes1");
    ms_synth_set_next_cc(0x102, a_start_cc);
    CHECK(make_adts(a1, sizeof(a1), &l1, 3, 2, a1_bytes, 0x51) == 0, "a1");
    CHECK(ms_synth_pes(b, 0x102, 0xC0, 1, 20000, a1, l1, 188) == 0, "apes1");
    CHECK(make_adts(a2, sizeof(a2), &l2, 3, 2, a2_bytes, 0x52) == 0, "a2");
    CHECK(ms_synth_pes(b, 0x102, 0xC0, 1, 40000, a2, l2, 188) == 0, "apes2");
    CHECK(make_h264_au(v2, sizeof(v2), &lv2, 4, 4, 16, 16, 1, NULL, 0) == 0, "v2");
    CHECK(ms_synth_pes(b, 0x101, 0xE0, 1, 50000, v2, lv2, 188) == 0, "vpes2");
    return 0;
}

static int test_aac_remainder_intra_pes(void)
{
    ms_synth_buf b;
    ms_session *s = NULL;
    p1h_drain d;
    uint8_t frames[2048];
    size_t n = 0, fl = 0;
    int k;
    const int nfr = 80;
    const uint64_t anchor = 90000ull;

    /* 48 kHz 三帧：必须仍为精确 +1920。 */
    {
        uint8_t f[64];
        size_t l1 = 0, l2 = 0, l3 = 0;
        n = 0;
        CHECK(make_adts(f, sizeof(f), &l1, 3, 2, 8, 0x11) == 0, "48-1");
        memcpy(frames + n, f, l1);
        n += l1;
        CHECK(make_adts(f, sizeof(f), &l2, 3, 2, 8, 0x12) == 0, "48-2");
        memcpy(frames + n, f, l2);
        n += l2;
        CHECK(make_adts(f, sizeof(f), &l3, 3, 2, 8, 0x13) == 0, "48-3");
        memcpy(frames + n, f, l3);
        n += l3;
        ms_synth_init(&b);
        CHECK(make_basic_ts(&b, 188) == 0, "ts48");
        CHECK(ms_synth_pes(&b, 0x102, 0xC0, 1, 90000, frames, n, 188) == 0, "pes48");
        CHECK(p1h_push_end(&s, &b) == 0, "push48");
        CHECK(p1h_collect(s, &d) == 0 && d.bad == 0, "drain48");
        CHECK(d.n_au_a == 3, "48k three au");
        CHECK(d.has_a[0] && d.pts_a[0] == 90000, "48k anchor");
        CHECK(d.has_a[1] && d.pts_a[1] == 91920, "48k +1920");
        CHECK(d.has_a[2] && d.pts_a[2] == 93840, "48k +3840");
        ms_synth_free(&b);
        ms_session_destroy(s);
        s = NULL;
    }

    /* 44.1 kHz 同一 PES 内 80 帧（超过 evq 64）：与 floor(k*90000*1024/44100) 一致。 */
    n = 0;
    for (k = 0; k < nfr; k++) {
        uint8_t f[64];
        fl = 0;
        CHECK(make_adts(f, sizeof(f), &fl, 4, 1, 8, (uint8_t)(0x20 + k)) == 0, "441f");
        CHECK(n + fl <= sizeof(frames), "441 cap");
        memcpy(frames + n, f, fl);
        n += fl;
    }
    ms_synth_init(&b);
    CHECK(make_basic_ts(&b, 188) == 0, "ts441");
    CHECK(ms_synth_pes(&b, 0x102, 0xC0, 1, anchor, frames, n, 188) == 0, "pes441");
    CHECK(p1h_feed(&s, &b, &d) == 0 && d.bad == 0, "feed441");
    CHECK(d.n_au_a == nfr, "80 frames in order");
    for (k = 0; k < nfr; k++) {
        uint64_t want = (anchor + ((uint64_t)k * 90000ull * 1024ull) / 44100ull) &
                        0x1FFFFFFFFULL;
        CHECK(d.has_a[k] && d.pts_a[k] == want, "441 remainder pts fifo");
    }
    /* 旧逐帧 floor 在第 2 帧就会少 1 tick：4178 vs 4179。 */
    CHECK(d.pts_a[2] == ((anchor + (2ull * 90000ull * 1024ull) / 44100ull) & 0x1FFFFFFFFULL),
          "no floor drift at frame 2");
    CHECK(d.pts_a[2] != (anchor + 2ull * ((90000ull * 1024ull) / 44100ull)),
          "differs from per-frame floor");
    ms_synth_free(&b);
    ms_session_destroy(s);
    s = NULL;

    /* 新 PES PTS 重新锚定，不得沿用上一 PES 余数。 */
    {
        uint8_t both[128], f[64];
        size_t l1 = 0, l2 = 0, n2 = 0;
        const uint64_t p2 = 200000ull;
        CHECK(make_adts(f, sizeof(f), &l1, 4, 1, 8, 0x61) == 0, "r1");
        memcpy(both, f, l1);
        n2 = l1;
        CHECK(make_adts(f, sizeof(f), &l2, 4, 1, 8, 0x62) == 0, "r2");
        memcpy(both + n2, f, l2);
        n2 += l2;
        ms_synth_init(&b);
        CHECK(make_basic_ts(&b, 188) == 0, "ts reanchor");
        CHECK(ms_synth_pes(&b, 0x102, 0xC0, 1, anchor, both, n2, 188) == 0, "pesA");
        CHECK(ms_synth_pes(&b, 0x102, 0xC0, 1, p2, f, l2, 188) == 0, "pesB");
        CHECK(p1h_push_end(&s, &b) == 0, "push reanchor");
        CHECK(p1h_collect(s, &d) == 0 && d.bad == 0, "drain reanchor");
        CHECK(d.n_au_a == 3, "two + new pes");
        CHECK(d.pts_a[0] == anchor, "reanchor 0");
        CHECK(d.pts_a[1] == ((anchor + (90000ull * 1024ull) / 44100ull) & 0x1FFFFFFFFULL),
              "reanchor intra");
        CHECK(d.has_a[2] && d.pts_a[2] == p2, "new pes pts not remainder");
        ms_synth_free(&b);
        ms_session_destroy(s);
        s = NULL;
    }

    /* 无 PTS 不杜撰绝对时钟。 */
    {
        uint8_t both[128], f[64];
        size_t l1 = 0, l2 = 0, n2 = 0;
        CHECK(make_adts(f, sizeof(f), &l1, 4, 1, 8, 0x71) == 0, "np1");
        memcpy(both, f, l1);
        n2 = l1;
        CHECK(make_adts(f, sizeof(f), &l2, 4, 1, 8, 0x72) == 0, "np2");
        memcpy(both + n2, f, l2);
        n2 += l2;
        ms_synth_init(&b);
        CHECK(make_basic_ts(&b, 188) == 0, "ts nopts");
        CHECK(ms_synth_pes(&b, 0x102, 0xC0, 0, 0, both, n2, 188) == 0, "pes nopts");
        CHECK(p1h_push_end(&s, &b) == 0, "push nopts");
        CHECK(p1h_collect(s, &d) == 0 && d.bad == 0, "drain nopts");
        CHECK(d.n_au_a == 2, "nopts two au");
        CHECK(!d.has_a[0] && !d.has_a[1], "no invented pts");
        ms_synth_free(&b);
        ms_session_destroy(s);
    }

    printf("  test_aac_remainder_intra_pes ok\n");
    return 0;
}

static int test_ts_continuity_safety(void)
{
    ms_synth_buf base, b;
    ms_session *s = NULL;
    p1h_drain d;
    size_t aidx[32];
    int na;
    size_t mid, pusi0, pusi1;
    uint8_t dup[188];
    int i;

    CHECK(p1h_build_av(&base, 0, 400, 16) == 0, "base");
    na = p1h_list_pid(&base, 0x102, aidx, 32);
    CHECK(na >= 4, "audio spans multiple packets");
    pusi0 = aidx[0];
    CHECK((base.data[pusi0 * 188u + 1] & 0x40) != 0, "first audio pusi");
    mid = aidx[1];
    pusi1 = (size_t)-1;
    for (i = 1; i < na; i++) {
        if ((base.data[aidx[i] * 188u + 1] & 0x40) != 0) {
            pusi1 = aidx[i];
            break;
        }
    }
    CHECK(pusi1 != (size_t)-1 && pusi1 > mid, "second audio pusi after mid");

    /* 干净基线：两音频 AU + 两视频 AU，无 DISCONTINUITY。 */
    CHECK(p1h_copy(&b, &base) == 0, "copy clean");
    CHECK(p1h_push_end(&s, &b) == 0, "push clean");
    CHECK(p1h_collect(s, &d) == 0 && d.bad == 0, "drain clean");
    CHECK(d.n_au_a == 2 && d.n_au_v == 2, "clean au counts");
    CHECK(d.n_disc == 0, "clean no disc");
    CHECK(!d.disc_a[0] && !d.disc_a[1] && !d.disc_v[0] && !d.disc_v[1], "clean flags");
    ms_synth_free(&b);
    ms_session_destroy(s);
    s = NULL;

    /* 丢掉第一音频 PES 中间一包：不得输出缺片第一 AU；视频两帧仍在且无 disc。 */
    {
        size_t want2;
        CHECK(p1h_copy(&b, &base) == 0, "copy drop");
        want2 = 0;
        /* 先记下完整第二帧大小（用干净流已测 d.sz，这里从 base 第二 PES 不方便，后面比 count）。 */
        CHECK(ms_synth_delete_unit(&b, mid, 188) == 0, "drop mid");
        CHECK(p1h_push_end(&s, &b) == 0, "push drop");
        CHECK(p1h_collect(s, &d) == 0 && d.bad == 0, "drain drop");
        CHECK(d.n_au_a == 1, "torn first audio not emitted");
        CHECK(d.n_au_v == 2, "video isolated");
        CHECK(!d.disc_v[0] && !d.disc_v[1], "video not flagged");
        CHECK(d.n_disc >= 1, "drop emits disc");
        CHECK(d.disc_id[0] == 0x102, "disc on audio pid");
        CHECK(d.disc_a[0] == 1, "recovered audio au marked");
        CHECK(d.pts_a[0] == 40000, "second pes pts");
        (void)want2;
        ms_synth_free(&b);
        ms_session_destroy(s);
        s = NULL;
    }

    /* 合法同 CC 重复：完整两 AU，不发 DISCONTINUITY。 */
    CHECK(p1h_copy(&b, &base) == 0, "copy dup");
    memcpy(dup, base.data + mid * 188u, 188);
    CHECK(ms_synth_insert_unit(&b, mid + 1, dup, 188) == 0, "insert dup");
    CHECK(p1h_push_end(&s, &b) == 0, "push dup");
    CHECK(p1h_collect(s, &d) == 0 && d.bad == 0, "drain dup");
    CHECK(d.n_au_a == 2 && d.n_au_v == 2, "dup still complete");
    CHECK(d.n_disc == 0, "legal dup is not disc");
    ms_synth_free(&b);
    ms_session_destroy(s);
    s = NULL;

    /* 第三次同 CC：异常，丢第一 PES，恢复第二。 */
    CHECK(p1h_copy(&b, &base) == 0, "copy triple");
    memcpy(dup, base.data + mid * 188u, 188);
    CHECK(ms_synth_insert_unit(&b, mid + 1, dup, 188) == 0, "t1");
    CHECK(ms_synth_insert_unit(&b, mid + 2, dup, 188) == 0, "t2");
    CHECK(p1h_push_end(&s, &b) == 0, "push triple");
    CHECK(p1h_collect(s, &d) == 0 && d.bad == 0, "drain triple");
    CHECK(d.n_au_a == 1, "triple drops incomplete");
    CHECK(d.n_disc >= 1 && d.disc_a[0] == 1, "triple disc");
    CHECK(d.n_au_v == 2, "triple video ok");
    ms_synth_free(&b);
    ms_session_destroy(s);
    s = NULL;

    /* 同 CC 但载荷不同：异常。 */
    CHECK(p1h_copy(&b, &base) == 0, "copy anom");
    memcpy(dup, base.data + mid * 188u, 188);
    dup[20] ^= 0xFF;
    CHECK(ms_synth_insert_unit(&b, mid + 1, dup, 188) == 0, "anom");
    CHECK(p1h_push_end(&s, &b) == 0, "push anom");
    CHECK(p1h_collect(s, &d) == 0 && d.bad == 0, "drain anom");
    CHECK(d.n_au_a == 1 && d.n_disc >= 1, "anomaly not appended");
    CHECK(d.n_au_v == 2, "anom video ok");
    ms_synth_free(&b);
    ms_session_destroy(s);
    s = NULL;

    /* 仅 adaptation：不递增 CC，PES 仍完整，无 disc。 */
    {
        unsigned cc0;
        uint8_t adapt[188];
        CHECK(p1h_copy(&b, &base) == 0, "copy adapt");
        cc0 = (unsigned)(base.data[pusi0 * 188u + 3] & 0x0F);
        memset(adapt, 0xFF, 188);
        adapt[0] = 0x47;
        adapt[1] = (uint8_t)((0x102 >> 8) & 0x1F);
        adapt[2] = (uint8_t)(0x102 & 0xFF);
        adapt[3] = (uint8_t)(0x20 | (cc0 & 0x0F));
        adapt[4] = 183;
        adapt[5] = 0x00;
        CHECK(ms_synth_insert_unit(&b, pusi0 + 1, adapt, 188) == 0, "insert adapt");
        CHECK(p1h_push_end(&s, &b) == 0, "push adapt");
        CHECK(p1h_collect(s, &d) == 0 && d.bad == 0, "drain adapt");
        CHECK(d.n_au_a == 2 && d.n_au_v == 2 && d.n_disc == 0, "adapt-only safe");
        ms_synth_free(&b);
        ms_session_destroy(s);
        s = NULL;
    }

    /* CC 15→0 回绕：完整、无 disc。 */
    {
        ms_synth_buf w;
        CHECK(p1h_build_av(&w, 14, 400, 16) == 0, "wrap build");
        na = p1h_list_pid(&w, 0x102, aidx, 32);
        CHECK(na >= 3, "wrap spans");
        CHECK((w.data[aidx[0] * 188u + 3] & 0x0F) == 14, "starts at 14");
        CHECK((w.data[aidx[2] * 188u + 3] & 0x0F) == 0, "wraps to 0");
        CHECK(p1h_push_end(&s, &w) == 0, "push wrap");
        CHECK(p1h_collect(s, &d) == 0 && d.bad == 0, "drain wrap");
        CHECK(d.n_au_a == 2 && d.n_au_v == 2 && d.n_disc == 0, "wrap ok");
        ms_synth_free(&w);
        ms_session_destroy(s);
        s = NULL;
    }

    /* 显式 discontinuity_indicator + CC 跳跃：第一 AU 完整，第二恢复并打标。 */
    {
        size_t j, na2;
        unsigned jumped;
        CHECK(p1h_copy(&b, &base) == 0, "copy discind");
        CHECK(ms_synth_pkt_add_disc_indicator(b.data + pusi1 * 188u) == 0, "add di");
        jumped = (unsigned)(((b.data[pusi1 * 188u + 3] & 0x0F) + 5) & 0x0F);
        /* 第二 PES 可能跨多包：从 PUSI 起按跳跃后的序列重写 CC。 */
        na2 = 0;
        for (j = pusi1; j < b.len / 188u; j++) {
            if (p1h_pid(b.data + j * 188u) != 0x102) {
                continue;
            }
            if (na2 > 0 && (b.data[j * 188u + 1] & 0x40) != 0) {
                break;
            }
            ms_synth_pkt_set_cc(b.data + j * 188u, (jumped + (unsigned)na2) & 0x0F);
            na2++;
        }
        CHECK(na2 >= 1, "rewrote pes2 cc");
        CHECK(p1h_push_end(&s, &b) == 0, "push discind");
        CHECK(p1h_collect(s, &d) == 0 && d.bad == 0, "drain discind");
        CHECK(d.n_au_a == 2 && d.n_au_v == 2, "discind both au");
        CHECK(d.n_disc >= 1 && d.disc_id[0] == 0x102, "discind event");
        CHECK(d.disc_a[0] == 0 && d.disc_a[1] == 1, "only recovered au flagged");
        CHECK(!d.disc_v[0] && !d.disc_v[1], "video clean");
        ms_synth_free(&b);
        ms_session_destroy(s);
        s = NULL;
    }

    /* TEI 打在第一 PES 中间包：丢不完整 AU，恢复第二。 */
    CHECK(p1h_copy(&b, &base) == 0, "copy tei");
    ms_synth_pkt_set_tei(b.data + mid * 188u, 1);
    CHECK(p1h_push_end(&s, &b) == 0, "push tei");
    CHECK(p1h_collect(s, &d) == 0 && d.bad == 0, "drain tei");
    CHECK(d.n_au_a == 1 && d.n_disc >= 1 && d.disc_a[0] == 1, "tei safety");
    CHECK(d.n_au_v == 2 && !d.disc_v[0] && !d.disc_v[1], "tei video isolated");
    ms_synth_free(&b);
    ms_session_destroy(s);
    s = NULL;

    /* 加扰中间包：同样丢弃不完整 PES，不当明文组帧。 */
    CHECK(p1h_copy(&b, &base) == 0, "copy scr");
    ms_synth_pkt_set_scrambling(b.data + mid * 188u, 1);
    CHECK(p1h_push_end(&s, &b) == 0, "push scr");
    CHECK(p1h_collect(s, &d) == 0 && d.bad == 0, "drain scr");
    CHECK(d.n_au_a == 1 && d.n_disc >= 1, "scramble safety");
    CHECK(d.n_au_v == 2, "scramble video isolated");
    ms_synth_free(&b);
    ms_session_destroy(s);

    ms_synth_free(&base);
    printf("  test_ts_continuity_safety ok\n");
    return 0;
}

static int test_h264_midstream_join_wait_idr(void)
{
    ms_synth_buf b;
    ms_session *s = NULL;
    p1h_drain d;
    uint8_t junk[256], au[400], au2[400], prefix[16], nal14[32];
    size_t jn = 0, al = 0, a2 = 0, pl = 0;
    ms_status st;
    size_t c = 0;
    int saw_unsup;

    /* 中途 GOP：AUD + 非 IDR，无 SPS/PPS。不得发出，也不得失败会话。 */
    CHECK(ms_synth_h264_aud(junk, sizeof(junk), &pl, 4) == 0, "aud mid");
    jn = pl;
    CHECK(ms_synth_h264_slice(junk + jn, sizeof(junk) - jn, &pl, 4, 0, NULL, 0) == 0,
          "p slice mid");
    jn += pl;
    ms_synth_init(&b);
    CHECK(make_basic_ts(&b, 188) == 0, "pat mid");
    CHECK(ms_synth_pes(&b, 0x101, 0xE0, 1, 90000, junk, jn, 188) == 0, "pes mid");

    /* 随后完整 AUD+SPS+PPS+IDR，应发出且配置在 AU 之前。 */
    CHECK(make_h264_au(au, sizeof(au), &al, 4, 4, 16, 16, 1, NULL, 0) == 0, "idr au");
    CHECK(ms_synth_pes(&b, 0x101, 0xE0, 1, 93600, au, al, 188) == 0, "pes idr");

    /* 已 started 后的非 IDR 应发出。 */
    CHECK(make_h264_au(au2, sizeof(au2), &a2, 4, 4, 16, 16, 0, NULL, 0) == 0, "p au");
    CHECK(ms_synth_pes(&b, 0x101, 0xE0, 1, 97200, au2, a2, 188) == 0, "pes p");

    CHECK(p1h_push_end(&s, &b) == 0, "push mid");
    CHECK(p1h_collect(s, &d) == 0 && d.bad == 0, "drain mid");
    CHECK(d.n_cfg_v == 1, "one video config");
    CHECK(d.n_au_v == 2, "idr + following p");
    CHECK(d.has_v[0] && d.pts_v[0] == 93600 && d.key_v[0], "first emitted is IDR pts");
    CHECK(d.has_v[1] && d.pts_v[1] == 97200 && !d.key_v[1], "following p pts");
    ms_synth_free(&b);
    ms_session_destroy(s);
    s = NULL;

    /* 真正不支持的 NAL type 14 仍须失败，不能为测通而忽略。 */
    ms_synth_init(&b);
    CHECK(make_basic_ts(&b, 188) == 0, "pat 14");
    CHECK(make_h264_au(au, sizeof(au), &al, 4, 4, 16, 16, 1, NULL, 0) == 0, "idr 14");
    CHECK(ms_synth_pes(&b, 0x101, 0xE0, 1, 90000, au, al, 188) == 0, "pes idr 14");
    CHECK(ms_synth_h264_aud(prefix, sizeof(prefix), &pl, 4) == 0, "aud 14");
    memcpy(nal14, prefix, pl);
    nal14[pl + 0] = 0x00;
    nal14[pl + 1] = 0x00;
    nal14[pl + 2] = 0x00;
    nal14[pl + 3] = 0x01;
    nal14[pl + 4] = 0x0E; /* forbidden=0, nal_ref_idc=0, type=14 */
    nal14[pl + 5] = 0x00;
    CHECK(ms_synth_pes(&b, 0x101, 0xE0, 1, 93600, nal14, pl + 6, 188) == 0, "pes 14");
    CHECK_ST(ms_session_create(NULL, NULL, &s), MS_OK);
    st = ms_session_push(s, b.data, b.len, &c);
    if (st == MS_OK || st == MS_LIMIT) {
        st = ms_session_end_of_input(s);
    }
    saw_unsup = (st == MS_ERR_UNSUPPORTED);
    while (!saw_unsup) {
        ms_event ev;
        ms_status pst = ms_session_pull(s, &ev);
        if (pst == MS_ERR_UNSUPPORTED) {
            saw_unsup = 1;
            break;
        }
        if (pst != MS_OK) {
            break;
        }
        CHECK_ST(ms_session_release(s, ev.release_id), MS_OK);
    }
    CHECK(saw_unsup, "NAL type 14 still UNSUPPORTED");
    ms_synth_free(&b);
    ms_session_destroy(s);
    printf("  test_h264_midstream_join_wait_idr ok\n");
    return 0;
}

static int evq_append_80_aac(ms_synth_buf *b, uint64_t anchor, int nfr)
{
    uint8_t frames[2048];
    size_t n = 0, fl = 0;
    int k;
    for (k = 0; k < nfr; k++) {
        uint8_t f[64];
        fl = 0;
        if (make_adts(f, sizeof(f), &fl, 4, 1, 8, (uint8_t)(0x30 + k)) != 0) {
            return -1;
        }
        if (n + fl > sizeof(frames)) {
            return -1;
        }
        memcpy(frames + n, f, fl);
        n += fl;
    }
    if (make_basic_ts(b, 188) != 0) {
        return -1;
    }
    return ms_synth_pes(b, 0x102, 0xC0, 1, anchor, frames, n, 188);
}

static int test_evq_fifo_backpressure_lifecycle(void)
{
    const int nfr = 80;
    const uint64_t anchor = 90000;
    ms_synth_buf b;
    ms_session *s = NULL;
    p1h_drain d;
    ms_event ev;
    ms_status st;
    size_t off, c;
    int i, n_au, n_cfg, saw_au, saw_cfg, typ, got_sess;
    uint64_t sess0, sel0, tl0, sess1, want;

    /* 1) 交错 188 字节 push-pull，累计超过 64 事件，PTS 按入队顺序。 */
    ms_synth_init(&b);
    CHECK(evq_append_80_aac(&b, anchor, nfr) == 0, "build 80");
    CHECK_ST(ms_session_create(NULL, NULL, &s), MS_OK);
    memset(&d, 0, sizeof(d));
    off = 0;
    while (off < b.len) {
        c = 0;
        st = ms_session_push(s, b.data + off, (b.len - off >= 188) ? 188 : (b.len - off), &c);
        CHECK(st == MS_OK || st == MS_LIMIT, "chunk push");
        off += c;
        CHECK(p1h_collect_more(s, &d) == 0 && d.bad == 0, "chunk pull");
        if (st == MS_LIMIT && c == 0) {
            continue;
        }
        CHECK(c > 0 || off >= b.len, "chunk progress");
    }
    for (i = 0; i < 100000; i++) {
        st = ms_session_end_of_input(s);
        CHECK(p1h_collect_more(s, &d) == 0 && d.bad == 0, "chunk eos pull");
        if (st == MS_OK) {
            break;
        }
        CHECK(st == MS_LIMIT, "chunk eos limit");
    }
    CHECK(d.n_cfg_a == 1, "config once");
    CHECK(d.n_au_a == nfr, "80 au interleaved");
    for (i = 0; i < nfr; i++) {
        want = (anchor + ((uint64_t)i * 90000ull * 1024ull) / 44100ull) & 0x1FFFFFFFFULL;
        CHECK(d.has_a[i] && d.pts_a[i] == want, "interleave fifo pts");
    }
    ms_session_destroy(s);
    s = NULL;
    ms_synth_free(&b);

    /* 2) 先灌满不拉，背压后再取：最先应是 FORMAT，配置先于 AU，AU 按入队顺序而非槽下标。 */
    ms_synth_init(&b);
    CHECK(evq_append_80_aac(&b, anchor, nfr) == 0, "build 80b");
    CHECK_ST(ms_session_create(NULL, NULL, &s), MS_OK);
    off = 0;
    st = MS_OK;
    while (off < b.len) {
        c = 0;
        st = ms_session_push(s, b.data + off, b.len - off, &c);
        off += c;
        if (st == MS_LIMIT) {
            break;
        }
        CHECK_ST(st, MS_OK);
        if (c == 0) {
            break;
        }
    }
    CHECK(st == MS_LIMIT, "hit backpressure over 64");
    saw_au = 0;
    saw_cfg = 0;
    n_au = 0;
    n_cfg = 0;
    typ = -1;
    sess0 = 0;
    sel0 = 0;
    tl0 = 0;
    for (i = 0; i < 512; i++) {
        st = ms_session_pull(s, &ev);
        if (st == MS_AGAIN || st == MS_EOS) {
            break;
        }
        CHECK_ST(st, MS_OK);
        if (i == 0) {
            typ = (int)ev.type;
            sess0 = ev.session_epoch;
            sel0 = ev.selection_epoch;
            tl0 = ev.timeline_epoch;
        }
        CHECK(ev.session_epoch == sess0, "session epoch stable");
        CHECK(ev.selection_epoch == sel0, "selection epoch stable");
        CHECK(ev.timeline_epoch == tl0, "timeline epoch stable");
        if (ev.type == MS_EVENT_CODEC_CONFIG && ev.track.codec == MS_CODEC_AAC) {
            CHECK(!saw_au, "config before first AU");
            saw_cfg = 1;
            n_cfg++;
        }
        if (ev.type == MS_EVENT_AU && ev.track.codec == MS_CODEC_AAC) {
            CHECK(saw_cfg, "AU after config");
            want = (anchor + ((uint64_t)n_au * 90000ull * 1024ull) / 44100ull) & 0x1FFFFFFFFULL;
            CHECK(ev.has_pts && ev.pts == want, "backpressure fifo first wave");
            saw_au = 1;
            n_au++;
        }
        CHECK_ST(ms_session_release(s, ev.release_id), MS_OK);
    }
    CHECK(typ == (int)MS_EVENT_FORMAT, "oldest event is FORMAT not a reused AU");
    CHECK(saw_cfg && n_cfg == 1, "one config");
    CHECK(n_au >= 1, "at least first AU after release");
    memset(&d, 0, sizeof(d));
    d.n_au_a = n_au;
    for (i = 0; i < n_au; i++) {
        d.has_a[i] = 1;
        d.pts_a[i] = (anchor + ((uint64_t)i * 90000ull * 1024ull) / 44100ull) & 0x1FFFFFFFFULL;
    }
    while (off < b.len) {
        c = 0;
        st = ms_session_push(s, b.data + off, b.len - off, &c);
        CHECK(st == MS_OK || st == MS_LIMIT, "resume push");
        off += c;
        CHECK(p1h_collect_more(s, &d) == 0 && d.bad == 0, "resume pull");
        if (st == MS_LIMIT && c == 0) {
            continue;
        }
        CHECK(c > 0 || off >= b.len, "resume progress");
    }
    for (i = 0; i < 100000; i++) {
        st = ms_session_end_of_input(s);
        CHECK(p1h_collect_more(s, &d) == 0 && d.bad == 0, "resume eos");
        if (st == MS_OK) {
            break;
        }
        CHECK(st == MS_LIMIT, "resume eos limit");
    }
    CHECK(d.n_au_a == nfr, "all 80 after backpressure");
    for (i = 0; i < nfr; i++) {
        want = (anchor + ((uint64_t)i * 90000ull * 1024ull) / 44100ull) & 0x1FFFFFFFFULL;
        CHECK(d.has_a[i] && d.pts_a[i] == want, "full fifo after reuse");
    }

    /* 3) flush / reset 清空队列并保持 epoch 合同。
     * flush 不清 eos_in；若已 end_of_input，下一次 pull 可能再次发出 EOS。 */
    CHECK_ST(ms_session_flush(s), MS_OK);
    st = ms_session_pull(s, &ev);
    if (st == MS_OK) {
        CHECK(ev.type == MS_EVENT_EOS, "flush after eos may re-queue EOS");
        CHECK_ST(ms_session_release(s, ev.release_id), MS_OK);
        st = ms_session_pull(s, &ev);
    }
    CHECK(st == MS_AGAIN || st == MS_EOS, "empty after flush");
    CHECK_ST(ms_session_reset(s), MS_OK);
    CHECK_ST(ms_session_pull(s, &ev), MS_AGAIN);
    {
        ms_synth_buf b2;
        ms_synth_init(&b2);
        CHECK(evq_append_80_aac(&b2, 180000, 8) == 0, "rebuild after reset");
        memset(&d, 0, sizeof(d));
        c = 0;
        st = ms_session_push(s, b2.data, b2.len, &c);
        CHECK(st == MS_OK || st == MS_LIMIT, "post-reset push");
        st = ms_session_end_of_input(s);
        CHECK(st == MS_OK || st == MS_LIMIT, "post-reset eos");
        saw_cfg = 0;
        saw_au = 0;
        n_au = 0;
        got_sess = 0;
        sess1 = 0;
        for (i = 0; i < 64; i++) {
            st = ms_session_pull(s, &ev);
            if (st == MS_AGAIN || st == MS_EOS) {
                break;
            }
            CHECK_ST(st, MS_OK);
            if (!got_sess) {
                sess1 = ev.session_epoch;
                got_sess = 1;
                CHECK(sess1 != sess0, "session epoch bumped on reset");
            } else {
                CHECK(ev.session_epoch == sess1, "new epoch stable");
            }
            if (ev.type == MS_EVENT_CODEC_CONFIG && ev.track.codec == MS_CODEC_AAC) {
                CHECK(!saw_au, "post-reset config before AU");
                saw_cfg = 1;
            }
            if (ev.type == MS_EVENT_AU && ev.track.codec == MS_CODEC_AAC) {
                CHECK(saw_cfg, "post-reset AU after config");
                want = (180000ull + ((uint64_t)n_au * 90000ull * 1024ull) / 44100ull) &
                       0x1FFFFFFFFULL;
                CHECK(ev.has_pts && ev.pts == want, "post-reset fifo");
                saw_au = 1;
                n_au++;
            }
            CHECK_ST(ms_session_release(s, ev.release_id), MS_OK);
        }
        CHECK(saw_cfg && n_au == 8, "post-reset 8 au");
        ms_synth_free(&b2);
    }
    ms_session_destroy(s);
    ms_synth_free(&b);
    printf("  test_evq_fifo_backpressure_lifecycle ok\n");
    return 0;
}


int main(void)
{
    if (test_abi_and_registry() != 0) {
        return 1;
    }
    if (test_limits_and_args() != 0) {
        return 1;
    }
    if (test_ts_detect_188_and_192_and_fragments() != 0) {
        return 1;
    }
    if (test_prefix_garbage() != 0) {
        return 1;
    }
    if (test_unknown_and_malformed() != 0) {
        return 1;
    }
    if (test_reset_epoch_slots_cancel() != 0) {
        return 1;
    }
    if (test_cross_session_and_eos() != 0) {
        return 1;
    }
    if (test_avs_family_not_plus() != 0) {
        return 1;
    }
    if (test_max_push_and_allocator() != 0) {
        return 1;
    }
    if (test_pes_extract_not_au() != 0) {
        return 1;
    }
    if (test_aac_adts_basic_and_pts() != 0) {
        return 1;
    }
    if (test_aac_fragments_and_cross_ts() != 0) {
        return 1;
    }
    if (test_aac_two_au_one_pes_and_split_pes() != 0) {
        return 1;
    }
    if (test_aac_eos_flush_reset_badlen_config_backpressure() != 0) {
        return 1;
    }
    if (test_aac_adts_subset_crc_mpeg_version() != 0) {
        return 1;
    }
    if (test_h264_aud_basic_config_idr() != 0) {
        return 1;
    }
    if (test_h264_fragments_ts_pes_timestamps() != 0) {
        return 1;
    }
    if (test_h264_eos_flush_reset_backpressure_reject() != 0) {
        return 1;
    }
    if (test_h264_noaud_multislice_and_switch() != 0) {
        return 1;
    }
    if (test_h264_noaud_unknown_pps_and_poc() != 0) {
        return 1;
    }
    if (test_h264_noaud_fragments_flush_limit() != 0) {
        return 1;
    }
    if (test_session_limits_budget_contract() != 0) {
        return 1;
    }
    if (test_budget_assembly_queue_and_backpressure() != 0) {
        return 1;
    }
    if (test_budget_multitrack_peak_replace_alloc() != 0) {
        return 1;
    }
    if (test_lazy_track_state_alloc() != 0) {
        return 1;
    }
    if (test_budget_malformed_seed_stress() != 0) {
        return 1;
    }
    if (test_unbounded_pes_eos_and_pts_wrap() != 0) {
        return 1;
    }
    if (test_aac_remainder_intra_pes() != 0) {
        return 1;
    }
    if (test_ts_continuity_safety() != 0) {
        return 1;
    }
    if (test_h264_midstream_join_wait_idr() != 0) {
        return 1;
    }
    if (test_evq_fifo_backpressure_lifecycle() != 0) {
        return 1;
    }
    if (g_failed) {
        return 1;
    }
    printf("all tests passed\n");
    return 0;
}
