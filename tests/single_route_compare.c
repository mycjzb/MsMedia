/*
 * 宿主双线对比（同样本）：路线 A = 自研 ms_session + ms_file_source；路线 B = ms_ff（宿主 libmsff，
 * 宿主 FFmpeg，不是安卓精简前缀）。两线都把事件喂同一个 ms_hw_player（宿主 mock 解码/音频）。
 * 记录：打开/探测耗时、首包、首个视频关键帧、首帧 mock 输出（相对创建播放器）、全文件解复用、
 * seek 50%、暂停/恢复、循环（EOS 后同一会话 seek 0 再出首帧）。mock 输出 != 设备出画；宿主耗时只做同环境横比。
 * 用法：single_route_compare <A|B> <file>
 * by: Long <mycjzb@gmail.com>
 */
#include "msmedia/msmedia.h"
#include "msmedia/ms_hw.h"
#include "msmedia/ms_ff.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

typedef struct src {
    int route;
    ms_session *s;
    ms_file_source fs;
    ms_ff *ff;
} src;

static ms_status src_open(src *x, const char *path)
{
    if (x->route == 'A') {
        ms_source so;
        memset(&x->fs, 0, sizeof(x->fs));
        x->fs.struct_size = sizeof(x->fs);
        x->fs.fd = -1;
        if (ms_file_source_open(&x->fs, path) != MS_OK) return MS_ERR_UNSUPPORTED;
        if (ms_session_create(NULL, NULL, &x->s) != MS_OK) return MS_ERR_NOMEM;
        ms_file_source_bind(&x->fs, &so);
        return ms_session_set_source(x->s, &so);
    } else {
        char url[1024];
        ms_status st = ms_ff_create(NULL, NULL, &x->ff);
        if (st != MS_OK) return st;
        (void)ms_ff_set_probe_mode(x->ff, MS_FF_PROBE_FAST_START);
        if (strstr(path, "://") != NULL) snprintf(url, sizeof(url), "%s", path);
        else snprintf(url, sizeof(url), "file:%s", path);
        return ms_ff_open(x->ff, url);
    }
}

/* 取一个事件：MS_OK / MS_EOS / 其它错误 */
static ms_status src_pull(src *x, ms_event *ev)
{
    if (x->route == 'A') {
        for (;;) {
            ms_status st = ms_session_pull(x->s, ev);
            if (st != MS_AGAIN) return st;
            st = ms_session_drive(x->s);
            if (st != MS_OK && st != MS_AGAIN) {
                if (st == MS_EOS) {
                    st = ms_session_pull(x->s, ev);
                    return st == MS_AGAIN ? MS_EOS : st;
                }
                return st;
            }
        }
    }
    {
        ms_status st;
        int spins = 0;
        do {
            st = ms_ff_pull(x->ff, ev);
        } while (st == MS_AGAIN && ++spins < 100000);
        return st;
    }
}

static void src_release(src *x, uint32_t id)
{
    if (!id) return;
    if (x->route == 'A') ms_session_release(x->s, id);
    else ms_ff_release(x->ff, id);
}

static ms_status src_seek(src *x, int64_t t90)
{
    return x->route == 'A' ? ms_session_seek(x->s, t90) : ms_ff_seek(x->ff, t90);
}

static void src_close(src *x)
{
    if (x->s) ms_session_destroy(x->s);
    if (x->route == 'A' && x->fs.fd >= 0) ms_file_source_close(&x->fs);
    if (x->ff) ms_ff_destroy(x->ff);
}

static void feed(ms_hw_player *p, const ms_event *ev)
{
    int k;
    for (k = 0; k < 1000; k++) {
        ms_status st = ms_hw_player_feed(p, ev, 0, NULL, NULL);
        if (st != MS_LIMIT && st != MS_AGAIN) break;
        ms_hw_player_drive(p);
    }
    ms_hw_player_drive(p);
}

int main(int argc, char **argv)
{
    src x;
    ms_hw_player *p = NULL;
    ms_hw_player_opts o;
    ms_hw_player_stats ps;
    double t0, t_open = -1, t_first_pkt = -1, t_first_key = -1, t_first_out = -1, t_end = -1;
    double t_seek = -1, t_loop_out = -1;
    long n_au = 0, n_vid = 0, n_aud = 0, n_sub = 0;
    int vcodec = 0, acodec = 0, scodec = 0, seek_ok = 0, loop_ok = 0, pause_ok = 0;
    int64_t last_pts = 0, seek_land = -1;
    ms_status st, endst = MS_OK;
    ms_event ev;

    if (argc < 3) return 2;
    memset(&x, 0, sizeof(x));
    x.route = argv[1][0];
    x.fs.fd = -1;

    t0 = now_ms();
    ms_hw_player_opts_default(&o);
    o.decoder.mock_mode = 1;
    o.audio_mock_mode = 1;
    if (ms_hw_player_create(NULL, &o, &p) != MS_OK) return 3;
    {
        static int surface_token; /* 宿主 mock 的 Surface 占位 */
        (void)ms_hw_player_set_surface(p, &surface_token);
    }
    (void)ms_hw_player_mark_request(p);
    st = src_open(&x, argv[2]);
    t_open = now_ms() - t0;
    if (st != MS_OK) {
        printf("route=%c file=%s OPEN_FAIL status=%s open_ms=%.2f\n", x.route, argv[2], ms_status_name(st), t_open);
        src_close(&x);
        ms_hw_player_destroy(p);
        return 0;
    }
    for (;;) {
        memset(&ev, 0, sizeof(ev));
        st = src_pull(&x, &ev);
        if (st != MS_OK) { endst = st; break; }
        if (ev.type == MS_EVENT_AU || ev.type == MS_EVENT_SUBTITLE) {
            if (t_first_pkt < 0) t_first_pkt = now_ms() - t0;
            n_au++;
            if (ev.track.kind == MS_TRACK_VIDEO) {
                n_vid++; vcodec = ev.track.codec;
                if (ev.is_keyframe && t_first_key < 0) t_first_key = now_ms() - t0;
                if (ev.has_pts) last_pts = (int64_t)ev.pts;
            } else if (ev.track.kind == MS_TRACK_AUDIO) {
                n_aud++; acodec = ev.track.codec;
            } else if (ev.track.kind == MS_TRACK_SUBTITLE) {
                n_sub++; scodec = ev.track.codec;
            }
        }
        feed(p, &ev);
        if (t_first_out < 0) {
            memset(&ps, 0, sizeof(ps)); ps.struct_size = sizeof(ps);
            if (ms_hw_player_get_stats(p, &ps) == MS_OK && ps.have_first_output) t_first_out = now_ms() - t0;
        }
        src_release(&x, ev.release_id);
        if (n_au == 50) {
            /* 暂停/恢复：mock 播放器状态切换 */
            pause_ok = ms_hw_player_pause(p) == MS_OK && ms_hw_player_is_paused(p) == 1 &&
                       ms_hw_player_resume(p) == MS_OK && ms_hw_player_is_paused(p) == 0;
        }
    }
    t_end = now_ms() - t0;
    if (getenv("MS_COMPARE_NO_SEEK") != NULL) {
        /* 单次建连核验：只做 打开+探测+整段解复用，不做 seek/循环 */
        printf("route=%c file=%s end=%s open_ms=%.2f first_pkt_ms=%.2f first_key_ms=%.2f demux_all_ms=%.2f au=%ld\n",
               x.route, argv[2], ms_status_name(endst), t_open, t_first_pkt, t_first_key, t_end, n_au);
        src_close(&x);
        ms_hw_player_destroy(p);
        return 0;
    }
    /* seek 到 ~50%（按最后视频 PTS 粗估；时间原点差异由各自实现处理） */
    {
        double ts0 = now_ms();
        int64_t target = last_pts > 0 ? last_pts / 2 : 90000;
        ms_hw_player_flush(p);
        st = src_seek(&x, target);
        if (st == MS_OK) {
            int k;
            for (k = 0; k < 2000; k++) {
                memset(&ev, 0, sizeof(ev));
                if (src_pull(&x, &ev) != MS_OK) break;
                if (ev.type == MS_EVENT_AU && ev.track.kind == MS_TRACK_VIDEO && ev.has_pts) {
                    seek_land = (int64_t)ev.pts;
                    seek_ok = 1;
                    src_release(&x, ev.release_id);
                    break;
                }
                if (ev.type == MS_EVENT_AU && ev.track.kind == MS_TRACK_AUDIO && n_vid == 0 && ev.has_pts) {
                    seek_land = (int64_t)ev.pts; seek_ok = 1; src_release(&x, ev.release_id); break;
                }
                src_release(&x, ev.release_id);
            }
        }
        t_seek = now_ms() - ts0;
        /* 循环：同一会话 seek 0，再取首个关键帧（不重开文件/连接） */
        ts0 = now_ms();
        if (src_seek(&x, 0) == MS_OK) {
            int k;
            for (k = 0; k < 2000; k++) {
                memset(&ev, 0, sizeof(ev));
                if (src_pull(&x, &ev) != MS_OK) break;
                if (ev.type == MS_EVENT_AU && (ev.track.kind == MS_TRACK_VIDEO ? ev.is_keyframe : n_vid == 0)) {
                    loop_ok = 1; src_release(&x, ev.release_id); break;
                }
                src_release(&x, ev.release_id);
            }
        }
        t_loop_out = now_ms() - ts0;
    }
    memset(&ps, 0, sizeof(ps)); ps.struct_size = sizeof(ps);
    ms_hw_player_get_stats(p, &ps);
    printf("route=%c file=%s end=%s open_ms=%.2f first_pkt_ms=%.2f first_key_ms=%.2f first_mock_out_ms=%.2f "
           "demux_all_ms=%.2f au=%ld v=%ld a=%ld s=%ld vcodec=%s acodec=%s scodec=%s pause_resume=%d "
           "seek50=%d seek_ms=%.2f seek_land_90k=%lld loop_seek0=%d loop_ms=%.2f "
           "req_first_pkt_us=%lld req_first_key_us=%lld req_first_submit_us=%lld req_first_output_us=%lld\n",
           x.route, argv[2], ms_status_name(endst), t_open, t_first_pkt, t_first_key, t_first_out, t_end,
           n_au, n_vid, n_aud, n_sub, ms_codec_name((ms_codec)vcodec), ms_codec_name((ms_codec)acodec),
           ms_codec_name((ms_codec)scodec), pause_ok, seek_ok, t_seek, (long long)seek_land, loop_ok, t_loop_out,
           (long long)(ps.have_req_first_packet ? ps.t_req_first_packet_us : -1),
           (long long)(ps.have_req_first_decodable_au ? ps.t_req_first_decodable_au_us : -1),
           (long long)(ps.have_req_first_submit ? ps.t_req_first_submit_us : -1),
           (long long)(ps.have_req_first_output ? ps.t_req_first_output_us : -1));
    src_close(&x);
    ms_hw_player_destroy(p);
    return 0;
}
