/*
 * 宿主 netsrc 测试：URL → ms_netsrc → ms_session，统计音视频 AU、预加载/Keep-Alive 统计，
 * 支持取消时延测量。ASan/UBSan/LSan 下运行，零泄漏。
 * 用法：single_netsrc_test <url> <min_video_au> <min_audio_au> [ahead_ms] [max_bytes] [cancel_after_ms] [max_ms]
 * 输出一行 RESULT ...；不打印完整 URL（只打印方案）。
 * by: Long <mycjzb@gmail.com>
 */
#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "msmedia/msmedia.h"
#include "netsrc/ms_netsrc.h"
#include "netcore/ms_netcore.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int64_t now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

int main(int argc, char **argv)
{
    ms_session *s = NULL;
    ms_netsrc *n = NULL;
    ms_netsrc_opts o;
    ms_netsrc_stats st;
    const char *url;
    int min_v, min_a, cancel_after = -1, max_ms = 20000, va = 0, aa = 0, eos = 0, cfg = 0, ok;
    int64_t t0, first_au = -1, cancel_t = -1, cancel_dur = -1;
    ms_status r = MS_OK, last = MS_OK;
    char scheme[16];
    if (argc < 4) return 2;
    url = argv[1];
    min_v = atoi(argv[2]);
    min_a = atoi(argv[3]);
    ms_netsrc_opts_default(&o);
    if (argc > 4 && atoi(argv[4]) > 0) o.preload_ahead_ms = atoi(argv[4]);
    if (argc > 5 && atol(argv[5]) > 0) o.preload_max_bytes = (size_t)atol(argv[5]);
    if (argc > 6) cancel_after = atoi(argv[6]);
    if (argc > 7) max_ms = atoi(argv[7]);
    o.read_timeout_ms = 5000;
    {
        size_t l = strcspn(url, ":");
        if (l >= sizeof(scheme)) l = sizeof(scheme) - 1;
        memcpy(scheme, url, l);
        scheme[l] = 0;
    }
    if (ms_session_create(NULL, NULL, &s) != MS_OK) return 3;
    t0 = now_ms();
    if (ms_netsrc_open(url, &o, s, &n) != MS_OK) { ms_session_destroy(s); return 4; }
    for (;;) {
        ms_event ev;
        int progressed = 0;
        if (cancel_after >= 0 && cancel_t < 0 && now_ms() - t0 >= cancel_after) {
            cancel_t = now_ms();
            ms_netsrc_cancel(n);
        }
        r = ms_netsrc_drive(n);
        if (r == MS_OK) progressed = 1;
        else if (r != MS_AGAIN && r != MS_LIMIT && r != MS_EOS) { last = r; break; }
        for (;;) {
            ms_status p = ms_session_pull(s, &ev);
            if (p != MS_OK) break;
            progressed = 1;
            if (ev.type == MS_EVENT_AU) {
                if (first_au < 0) first_au = now_ms() - t0;
                if (ev.track.kind == MS_TRACK_VIDEO) va++;
                else if (ev.track.kind == MS_TRACK_AUDIO) aa++;
            } else if (ev.type == MS_EVENT_CODEC_CONFIG) cfg++;
            if (ev.release_id) ms_session_release(s, ev.release_id);
            if (ev.type == MS_EVENT_EOS) eos = 1;
        }
        if (eos) break;
        if (now_ms() - t0 > max_ms) { last = MS_ERR_TIMEOUT; break; }
        if (!progressed) usleep(2000);
    }
    ms_netsrc_get_stats(n, &st);
    if (last != MS_OK && last != MS_ERR_CANCELED) printf("ERRMSG %s\n", ms_netsrc_last_error(n));
    {
        int64_t c0 = now_ms();
        ms_netsrc_close(n);
        if (cancel_t >= 0) cancel_dur = now_ms() - cancel_t;
        else cancel_dur = now_ms() - c0;
    }
    ms_session_destroy(s);
    ms_nc_pool_clear();
    if (cancel_after >= 0)
        ok = last == MS_ERR_CANCELED && cancel_dur < 1000;
    else
        ok = va >= min_v && aa >= min_a && (eos || (last == MS_ERR_TIMEOUT && max_ms < 20000));
    printf("RESULT %s scheme=%s mode=%s probe=%d disguise=%d stripped=%llu video_au=%d audio_au=%d cfg=%d eos=%d last=%s "
           "first_au_ms=%lld seg_demand=%d seg_prefetched=%d seg_hit=%d cache_peak=%zu http_opened=%d http_reused=%d bad_pkts=%llu close_ms=%lld\n",
           ok ? "PASS" : "FAIL", scheme, ms_netsrc_mode_name(st.mode), st.probe_fmt, st.probe_disguise,
           (unsigned long long)st.stripped_bytes, va, aa, cfg, eos, ms_status_name(last), (long long)first_au,
           st.seg_demand, st.seg_prefetched, st.seg_hit, st.cache_peak, st.http_opened, st.http_reused, (unsigned long long)st.bad_packets, (long long)cancel_dur);
    return ok ? 0 : 1;
}
