/*
 * 路线 A（纯自研）TS DVB 位图字幕端到端：合成 TS（stream_type 0x06 + 0x59 描述符，
 * 16x16 显示集跨多个 TS 包）-> ms_session 解复用 -> ms_hw_player（宿主 mock）
 * -> ms_hw_player_subtitles_at 取 RGBA cue。宿主 mock 不是设备上屏验收。
 * by: Long <mycjzb@gmail.com>
 */
#include "msmedia/msmedia.h"
#include "msmedia/ms_hw.h"
#include "ms_ts_synth.h"
#include "ms_sub_bitmap_fixtures.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int fails;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL %s\n", m); fails++; } else { printf("ok %s\n", m); } } while (0)

int main(void)
{
    ms_synth_buf b;
    ms_synth_stream streams[1];
    ms_session *s = NULL;
    ms_hw_player *p = NULL;
    ms_hw_player_opts o;
    ms_track_info ti[8];
    size_t nt = 0, off = 0, i;
    uint32_t sid = 0;
    int saw_sub = 0, steps = 0;
    ms_sub_cue cue;
    size_t nc = 0;

    memset(streams, 0, sizeof(streams));
    streams[0].stream_type = 0x06;
    streams[0].pid = 0x131;
    streams[0].es_descriptor = k_dvb_desc59;
    streams[0].es_descriptor_size = (uint16_t)sizeof(k_dvb_desc59);
    ms_synth_init(&b);
    ms_synth_reset_cc();
    if (ms_synth_pat_pmt(&b, 1, 0x130, streams, 1, 8, 188) != 0) return 2;
    ms_synth_pes(&b, 0x131, 0xBD, 1, 90000, k_dvb_16_red, sizeof(k_dvb_16_red), 188);
    ms_synth_pes(&b, 0x131, 0xBD, 1, 180000, k_dvb_16_red, sizeof(k_dvb_16_red), 188);
    ms_synth_pes(&b, 0x131, 0xBD, 1, 270000, k_dvb_16_red, sizeof(k_dvb_16_red), 188);

    if (ms_session_create(NULL, NULL, &s) != MS_OK) return 2;
    ms_hw_player_opts_default(&o);
    o.decoder.mock_mode = 1;
    o.audio_mock_mode = 1;
    CHECK(ms_hw_player_create(NULL, &o, &p) == MS_OK, "hw player (host mock) create");
    /* 推/拉交错：会话事件队列有界，只推不拉会反压（used=0）。 */
    for (;;) {
        size_t used = 0;
        ms_status pst = MS_OK;
        if (off < b.len) {
            size_t chunk = b.len - off > 188 ? 188 : b.len - off; /* 按包推，模拟流式 */
            pst = ms_session_push(s, b.data + off, chunk, &used);
            off += used;
            if (off >= b.len) ms_session_end_of_input(s);
        }
        if (!sid && ms_session_list_tracks(s, ti, 8, &nt) == MS_OK) {
            for (i = 0; i < nt; i++) {
                if (ti[i].codec == MS_CODEC_SUB_DVB) {
                    sid = ti[i].id;
                    printf("track id=%u kind=%d codec=%d\n", ti[i].id, ti[i].kind, ti[i].codec);
                    CHECK(ms_session_select_track(s, sid, 1) == MS_OK, "select dvb track");
                    if (p) ms_hw_player_select_track(p, sid, 1);
                }
            }
        }
        for (;;) {
            ms_event ev;
            ms_status st;
            memset(&ev, 0, sizeof(ev));
            st = ms_session_pull(s, &ev);
            if (st != MS_OK) break;
            steps++; if (getenv("DVB_DBG")) printf("ev type=%d codec=%d tid=%u size=%zu\n", (int)ev.type, (int)ev.track.codec, ev.track.id, ev.data_size);
            if (ev.type == MS_EVENT_SUBTITLE && ev.track.codec == MS_CODEC_SUB_DVB) saw_sub++;
            if (p) ms_hw_player_feed(p, &ev, 0, NULL, NULL);
            if (ev.release_id) ms_session_release(s, ev.release_id);
        }
        if (off >= b.len) break;
        if (used == 0 && pst != MS_OK && pst != MS_AGAIN && pst != MS_LIMIT) break;
        if (steps > 10000) break;
    }
    CHECK(sid != 0, "in-house TS: DVB track identified as MS_CODEC_SUB_DVB");
    CHECK(saw_sub >= 1, "session emitted DVB SUBTITLE events");
    ms_sub_cue_default(&cue);
    if (p) ms_hw_player_subtitles_at(p, 90000, &cue, 1, &nc);
    CHECK(nc == 1, "player cue at 1s");
    if (nc == 1) {
        CHECK(cue.bitmap_kind == MS_SUB_BITMAP_DVB && cue.bitmap_w == 16 && cue.bitmap_h == 16, "dvb 16x16 rgba");
        CHECK(cue.pos_x == 10 && cue.pos_y == 20, "dvb pos 10,20");
        CHECK(cue.bitmap_rgba && cue.bitmap_rgba[0] > 200 && cue.bitmap_rgba[1] < 80, "dvb ~red");
    }
    printf("saw_sub=%d steps=%d\n", saw_sub, steps);
    if (p) ms_hw_player_destroy(p);
    ms_session_destroy(s);
    ms_synth_free(&b);
    printf("%s fails=%d\n", fails ? "DVB_E2E_FAIL" : "DVB_E2E_PASS", fails);
    return fails ? 1 : 0;
}
