/*
 * 阶段 3：AVS/AVS+（TS stream_type 0x42）自研切帧 + CSD + 硬解器挑选策略宿主单测。
 * 合成码流只用手写的 GY/T 257.1 广播类（0x48）/基准类（0x20）序列头与 B3/B6 图像头，
 * 不是真实编码器产物；挑选策略用模拟的 MediaCodecList（含海思 OMX.hisi.video.decoder.avs），
 * 不是海思真机。宿主 mock 播放器只测状态机，不是出画。
 * 用法：single_avs_test [输出 TS 路径]
 * by: Long <mycjzb@gmail.com>
 */
#include "msmedia/msmedia.h"
#include "msmedia/ms_hw.h"
#include "ms_hw_internal.h"
#include "ms_ts_synth.h"
#include "ms_vbit_fixtures.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail = 0;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, m); g_fail++; } } while (0)

/* 统计一段 ES 里图像起始码（B3/B6）个数。 */
static int count_pics(const uint8_t *d, size_t n)
{
    size_t i;
    int c = 0;
    for (i = 0; i + 3 < n; i++) {
        if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1 && (d[i + 3] == 0xB3 || d[i + 3] == 0xB6)) {
            c++;
            i += 3;
        }
    }
    return c;
}

/* 把合成 TS 里每个视频 PES 头的 flags 字节 0x80 改为 0x84（data_alignment_indicator=1）。 */
static void set_alignment(ms_synth_buf *b)
{
    size_t i;
    for (i = 0; i + 7 < b->len; i++) {
        if (b->data[i] == 0 && b->data[i + 1] == 0 && b->data[i + 2] == 1 && b->data[i + 3] == 0xE0 &&
            b->data[i + 6] == 0x80) {
            b->data[i + 6] = 0x84;
        }
    }
}

static size_t cat(uint8_t *dst, size_t off, const uint8_t *src, size_t n)
{
    memcpy(dst + off, src, n);
    return off + n;
}

/* 生成：PES0 seq+I，PES1 P，PES2 P+P（一 PES 两帧），PES3 seq+I，PES4 P。 */
static void build_ts(ms_synth_buf *b, const uint8_t *seq, size_t seqn, int aligned)
{
    ms_synth_stream st[1];
    uint8_t buf[128];
    size_t n;
    memset(st, 0, sizeof(st));
    st[0].stream_type = 0x42;
    st[0].pid = 0x111;
    ms_synth_init(b);
    ms_synth_reset_cc();
    ms_synth_pat_pmt(b, 1, 0x110, st, 1, 8, 188);
    n = cat(buf, 0, seq, seqn);
    n = cat(buf, n, k_avs_pic_i, sizeof(k_avs_pic_i));
    ms_synth_pes(b, 0x111, 0xE0, 1, 0, buf, n, 188);
    ms_synth_pes(b, 0x111, 0xE0, 1, 9000, k_avs_pic_p, sizeof(k_avs_pic_p), 188);
    n = cat(buf, 0, k_avs_pic_p, sizeof(k_avs_pic_p));
    n = cat(buf, n, k_avs_pic_p, sizeof(k_avs_pic_p));
    ms_synth_pes(b, 0x111, 0xE0, 1, 18000, buf, n, 188);
    n = cat(buf, 0, seq, seqn);
    n = cat(buf, n, k_avs_pic_i, sizeof(k_avs_pic_i));
    ms_synth_pes(b, 0x111, 0xE0, 1, 36000, buf, n, 188);
    ms_synth_pes(b, 0x111, 0xE0, 1, 45000, k_avs_pic_p, sizeof(k_avs_pic_p), 188);
    if (aligned) {
        set_alignment(b);
    }
}

typedef struct tally {
    int track_family, track_plus, cfg, au, key, bad_pics, first_seq, au_nopts, first_pts0;
    int au_plus;
    int loose; /* 随机流：不校验 CSD 尺寸 */
    ms_hw_player *player;
} tally;

static void run_session(const ms_synth_buf *b, tally *t, int flush_and_repeat)
{
    ms_session *s = NULL;
    ms_event ev;
    size_t c = 0;
    int pass, guard = 0;
    ms_hw_player *keep_player = t->player;
    int keep_loose = t->loose;
    memset(t, 0, sizeof(*t));
    t->player = keep_player;
    t->loose = keep_loose;
    if (ms_session_create(NULL, NULL, &s) != MS_OK) {
        CHECK(0, "session create");
        return;
    }
    for (pass = 0; pass < (flush_and_repeat ? 2 : 1); pass++) {
        size_t off = 0;
        if (pass == 1) {
            CHECK(ms_session_flush(s) == MS_OK, "flush");
        }
        /* 每 188 字节推一次并即时拉取（模拟边收边播） */
        while (off < b->len) {
            size_t n = b->len - off > 188 ? 188 : b->len - off;
            ms_status st = ms_session_push(s, b->data + off, n, &c);
            CHECK(st == MS_OK || st == MS_AGAIN, "push");
            off += (c > 0) ? c : n;
            while (ms_session_pull(s, &ev) == MS_OK && ++guard < 10000) {
                if (ev.type == MS_EVENT_TRACK || ev.type == MS_EVENT_TRACK_UPDATE) {
                    if (ev.track.codec == MS_CODEC_AVS_FAMILY) t->track_family++;
                    if (ev.track.codec == MS_CODEC_AVS_PLUS) t->track_plus++;
                } else if (ev.type == MS_EVENT_CODEC_CONFIG && ev.track.codec == MS_CODEC_AVS_PLUS) {
                    ms_video_cfg vc;
                    ms_video_cfg_default(&vc);
                    t->cfg++;
                    CHECK(ev.sample_format == MS_SAMPLE_FMT_CAVS, "cfg fmt cavs");
                    CHECK(t->loose || (ms_video_cfg_parse(MS_CODEC_AVS_PLUS, MS_SAMPLE_FMT_CAVS, 1, ev.data,
                                             ev.data_size, &vc) == MS_OK && vc.complete &&
                          vc.width == 16 && vc.height == 16), "csd parses 16x16 broadcast");
                    CHECK(ev.data_size >= 4 && ev.data[3] == 0xB0, "csd starts with B0");
                } else if (ev.type == MS_EVENT_AU) {
                    t->au++;
                    if (ev.track.codec == MS_CODEC_AVS_PLUS) t->au_plus++;
                    if (ev.is_keyframe) t->key++;
                    if (count_pics(ev.data, ev.data_size) != 1) t->bad_pics++;
                    if (t->au == 1 && ev.data_size >= 4 && ev.data[3] == 0xB0) t->first_seq = 1;
                    if (t->au == 1 && ev.has_pts && ev.pts == 0) t->first_pts0 = 1;
                    if (!ev.has_pts) t->au_nopts++;
                    CHECK(ev.sample_format == MS_SAMPLE_FMT_CAVS, "au fmt cavs");
                }
                if (t->player != NULL) {
                    int k;
                    for (k = 0; k < 100; k++) {
                        ms_status fs = ms_hw_player_feed(t->player, &ev, 0, NULL, NULL);
                        if (fs != MS_LIMIT && fs != MS_AGAIN) break;
                        ms_hw_player_drive(t->player);
                    }
                    ms_hw_player_drive(t->player);
                }
                if (ev.release_id) ms_session_release(s, ev.release_id);
            }
        }
        /* EOS：送出暂存末帧 */
        if (pass == (flush_and_repeat ? 1 : 0)) {
            (void)ms_session_end_of_input(s);
        }
        (void)ms_session_drive(s);
        while (ms_session_pull(s, &ev) == MS_OK && ++guard < 10000) {
            if (ev.type == MS_EVENT_AU) {
                t->au++;
                if (ev.is_keyframe) t->key++;
                if (count_pics(ev.data, ev.data_size) != 1) t->bad_pics++;
                if (!ev.has_pts) t->au_nopts++;
            } else if (ev.type == MS_EVENT_CODEC_CONFIG && ev.track.codec == MS_CODEC_AVS_PLUS) {
                t->cfg++;
            }
            if (ev.release_id) ms_session_release(s, ev.release_id);
        }
    }
    ms_session_destroy(s);
}

static int test_ts_framing(const char *out_path)
{
    ms_synth_buf b;
    tally t;
    int aligned;
    for (aligned = 0; aligned <= 1; aligned++) {
        build_ts(&b, k_avsplus_seq_16, sizeof(k_avsplus_seq_16), aligned);
        if (aligned && out_path != NULL) {
            FILE *fp = fopen(out_path, "wb");
            if (fp) {
                fwrite(b.data, 1, b.len, fp);
                fclose(fp);
            }
        }
        run_session(&b, &t, 0);
        printf("avs+ ts aligned=%d: family=%d plus=%d cfg=%d au=%d key=%d bad=%d first_seq=%d nopts=%d pts0=%d\n",
               aligned, t.track_family, t.track_plus, t.cfg, t.au, t.key, t.bad_pics, t.first_seq,
               t.au_nopts, t.first_pts0);
        CHECK(t.track_family >= 1, "pmt announces avs family first");
        CHECK(t.track_plus >= 1, "upgraded to AVS+ by 0x48 seq header");
        CHECK(t.cfg == 1, "same seq header -> exactly one CODEC_CONFIG");
        CHECK(t.au == 6, "6 pictures -> 6 AUs (P+P PES split)");
        CHECK(t.key == 2, "2 I pictures keyframe");
        CHECK(t.bad_pics == 0, "each AU has exactly one picture");
        CHECK(t.first_seq == 1, "first AU carries B0 seq header");
        CHECK(t.first_pts0 == 1, "first AU pts=0");
        CHECK(t.au_nopts == 1, "2nd picture of shared PES has no pts");
        /* flush 后重推：序列头须再次上送 */
        run_session(&b, &t, 1);
        printf("avs+ ts aligned=%d flush+repeat: cfg=%d au=%d key=%d\n", aligned, t.cfg, t.au, t.key);
        CHECK(t.cfg == 2, "CODEC_CONFIG re-sent after flush");
        CHECK(t.bad_pics == 0, "no torn AU after flush");
        ms_synth_free(&b);
    }
    /* 基准类 0x20：不得升格 */
    build_ts(&b, k_avs_jizhun_seq_16, sizeof(k_avs_jizhun_seq_16), 1);
    run_session(&b, &t, 0);
    printf("avs jizhun: family=%d plus=%d cfg=%d au=%d au_plus=%d\n", t.track_family, t.track_plus,
           t.cfg, t.au, t.au_plus);
    CHECK(t.track_plus == 0 && t.cfg == 0 && t.au_plus == 0, "jizhun stays family, no AVS+ cfg");
    CHECK(t.au == 6, "jizhun AUs still framed (player refuses honestly)");
    ms_synth_free(&b);
    return 0;
}

static int test_player_mock(void)
{
    ms_synth_buf b;
    tally t;
    ms_hw_player_opts o;
    ms_hw_player_stats st;
    static int token = 7;
    ms_hw_player *p = NULL;
    ms_hw_player_opts_default(&o);
    o.decoder.mock_mode = 1;
    o.audio_mock_mode = 1;
    CHECK(ms_hw_player_create(NULL, &o, &p) == MS_OK, "player");
    if (p == NULL) return 1;
    (void)ms_hw_player_set_surface(p, &token);
    (void)ms_hw_player_mark_request(p);
    build_ts(&b, k_avsplus_seq_16, sizeof(k_avsplus_seq_16), 1);
    memset(&t, 0, sizeof(t));
    t.player = p;
    run_session(&b, &t, 0);
    memset(&st, 0, sizeof(st));
    st.struct_size = sizeof(st);
    CHECK(ms_hw_player_get_stats(p, &st) == MS_OK, "stats");
    printf("avs+ mock player: configured=%d submitted=%llu first_output=%d req_first_output_us=%lld\n",
           st.configured, (unsigned long long)st.video_au_submitted, st.have_first_output,
           (long long)(st.have_req_first_output ? st.t_req_first_output_us : -1));
    CHECK(st.configured == 1, "mock configured AVS+ from TS CSD");
    CHECK(st.video_au_submitted >= 2, "AUs submitted");
    CHECK(st.have_first_output == 1, "mock first output");
    CHECK(ms_hw_player_seek(p, 0) == MS_OK, "player seek");
    ms_hw_player_destroy(p);
    ms_synth_free(&b);
    return 0;
}

/* 粗暴随机：随机 PES 载荷（含伪起始码）不得越界/泄漏。 */
static int test_fuzz(void)
{
    unsigned seed = 12345u;
    int round;
    for (round = 0; round < 300; round++) {
        ms_synth_buf b;
        ms_synth_stream st[1];
        uint8_t pay[600];
        size_t n, i;
        tally t;
        memset(st, 0, sizeof(st));
        st[0].stream_type = 0x42;
        st[0].pid = 0x111;
        ms_synth_init(&b);
        ms_synth_reset_cc();
        ms_synth_pat_pmt(&b, 1, 0x110, st, 1, 8, 188);
        for (i = 0; i < 6; i++) {
            size_t j;
            seed = seed * 1103515245u + 12345u;
            n = 1 + (seed >> 8) % sizeof(pay);
            for (j = 0; j < n; j++) {
                seed = seed * 1103515245u + 12345u;
                pay[j] = (uint8_t)(seed >> 16);
                if ((seed & 31u) == 0 && j + 4 < n) {
                    static const uint8_t scs[] = { 0xB0, 0xB3, 0xB6, 0xB7, 0xB5, 0xB1 };
                    pay[j] = 0; pay[j + 1] = 0; pay[j + 2] = 1;
                    pay[j + 3] = scs[(seed >> 5) % 6];
                    j += 3;
                }
            }
            if (round % 3 == 0 && n > sizeof(k_avsplus_seq_16)) {
                memcpy(pay, k_avsplus_seq_16, sizeof(k_avsplus_seq_16));
            }
            ms_synth_pes(&b, 0x111, 0xE0, (int)(seed & 1u), (uint64_t)i * 3600u, pay, n, 188);
        }
        if (round & 1) set_alignment(&b);
        memset(&t, 0, sizeof(t));
        t.loose = 1;
        run_session(&b, &t, round % 5 == 0);
        ms_synth_free(&b);
    }
    printf("avs fuzz: 300 rounds done\n");
    return 0;
}

/* 挑选策略：模拟 MediaCodecList。 */
static ms_status pick(const ms_hw_codec_desc *l, size_t n, int api, int allow_sw, char *name, char *mime,
                      ms_hw_kind *k)
{
    char detail[192];
    return ms_hw_avs_pick_from_list(l, n, api, allow_sw, name, 128, mime, 64, k, detail, sizeof(detail));
}

static int test_pick(void)
{
    char name[128], mime[64];
    ms_hw_kind k;
    ms_status st;
    ms_hw_codec_desc hisi[3];
    memset(hisi, 0, sizeof(hisi));
    hisi[0].name = "OMX.google.h264.decoder"; hisi[0].types[0] = "video/avc"; hisi[0].ntypes = 1;
    hisi[0].hw_flag = hisi[0].sw_flag = -1;
    hisi[1].name = "OMX.hisi.video.decoder.avc"; hisi[1].types[0] = "video/avc"; hisi[1].ntypes = 1;
    hisi[1].hw_flag = hisi[1].sw_flag = -1;
    hisi[2].name = "OMX.hisi.video.decoder.avs"; hisi[2].types[0] = "video/avs"; hisi[2].ntypes = 1;
    hisi[2].hw_flag = hisi[2].sw_flag = -1;
    st = pick(hisi, 3, 23, 0, name, mime, &k);
    printf("pick hisi(api23): st=%s name=%s mime=%s kind=%s\n", ms_status_name(st), name, mime, ms_hw_kind_name(k));
    CHECK(st == MS_OK && strcmp(name, "OMX.hisi.video.decoder.avs") == 0 && strcmp(mime, "video/avs") == 0 &&
          k == MS_HW_KIND_HARDWARE, "hisi avs hw picked with declared MIME");

    /* video/cavs 写法 + 多 MIME：AVS+ 写法优先 */
    {
        ms_hw_codec_desc l[2];
        memset(l, 0, sizeof(l));
        l[0].name = "OMX.vendor.video.decoder.cavs"; l[0].types[0] = "video/cavs"; l[0].ntypes = 1;
        l[0].hw_flag = l[0].sw_flag = -1;
        l[1].name = "c2.vendor.avsplus.decoder"; l[1].types[0] = "video/mpeg2"; l[1].types[1] = "video/avs+";
        l[1].ntypes = 2; l[1].hw_flag = l[1].sw_flag = -1;
        st = pick(l, 2, 28, 0, name, mime, &k);
        CHECK(st == MS_OK && strcmp(name, "c2.vendor.avsplus.decoder") == 0 && strcmp(mime, "video/avs+") == 0,
              "video/avs+ preferred over video/cavs");
        l[1].types[1] = "video/hevc";
        st = pick(l, 2, 28, 0, name, mime, &k);
        CHECK(st == MS_OK && strcmp(mime, "video/cavs") == 0, "video/cavs accepted");
    }
    /* 名称兜底：海思私有 MIME */
    {
        ms_hw_codec_desc l[1];
        memset(l, 0, sizeof(l));
        l[0].name = "OMX.hisi.video.decoder.avsplus"; l[0].types[0] = "video/x-hisi-vendor"; l[0].ntypes = 1;
        l[0].hw_flag = l[0].sw_flag = -1;
        st = pick(l, 1, 19, 0, name, mime, &k);
        CHECK(st == MS_OK && strcmp(mime, "video/x-hisi-vendor") == 0, "hisi+avs name fallback uses declared MIME");
    }
    /* 只有软件 / AVS2 / 编码器 / 别名：诚实 UNSUPPORTED */
    {
        ms_hw_codec_desc l[4];
        memset(l, 0, sizeof(l));
        l[0].name = "OMX.google.avs.decoder"; l[0].types[0] = "video/avs"; l[0].ntypes = 1;
        l[0].hw_flag = l[0].sw_flag = -1;
        l[1].name = "OMX.hisi.video.decoder.avs2"; l[1].types[0] = "video/avs2"; l[1].ntypes = 1;
        l[1].hw_flag = l[1].sw_flag = -1;
        l[2].name = "OMX.hisi.video.encoder.avs"; l[2].types[0] = "video/avs"; l[2].ntypes = 1;
        l[2].is_encoder = 1; l[2].hw_flag = l[2].sw_flag = -1;
        l[3].name = "OMX.hisi.video.decoder.avs.alias"; l[3].types[0] = "video/avs"; l[3].ntypes = 1;
        l[3].is_alias = 1; l[3].hw_flag = l[3].sw_flag = -1;
        st = pick(l, 4, 30, 0, name, mime, &k);
        CHECK(st == MS_ERR_UNSUPPORTED && mime[0] == 0, "software/avs2/encoder/alias -> UNSUPPORTED");
        st = pick(l, 4, 30, 1, name, mime, &k);
        CHECK(st == MS_OK && k == MS_HW_KIND_SOFTWARE && strcmp(name, "OMX.google.avs.decoder") == 0,
              "allow_sw picks software explicitly");
    }
    /* API 29+：isSoftwareOnly=1 的海思名也不算硬件 */
    {
        ms_hw_codec_desc l[1];
        memset(l, 0, sizeof(l));
        l[0].name = "c2.hisi.avs.decoder"; l[0].types[0] = "video/avs"; l[0].ntypes = 1;
        l[0].hw_flag = 0; l[0].sw_flag = 1;
        st = pick(l, 1, 30, 0, name, mime, &k);
        CHECK(st == MS_ERR_UNSUPPORTED, "api29 sw flag respected");
    }
    /* 空表（模拟器）：UNSUPPORTED */
    st = pick(NULL, 0, 34, 0, name, mime, &k);
    CHECK(st == MS_ERR_UNSUPPORTED && name[0] == 0, "empty list unsupported");
    CHECK(ms_hw_avs_mime_rank("VIDEO/AVS") > 0 && ms_hw_avs_mime_rank("video/avs2") == 0 &&
          ms_hw_avs_mime_rank("video/avs3") == 0 && ms_hw_avs_mime_rank(NULL) == 0, "mime rank");
    return 0;
}

int main(int argc, char **argv)
{
    test_ts_framing(argc > 1 ? argv[1] : NULL);
    test_player_mock();
    test_pick();
    test_fuzz();
    printf(g_fail ? "AVS_TEST_FAIL fails=%d\n" : "AVS_TEST_PASS fails=%d\n", g_fail);
    return g_fail ? 1 : 0;
}
