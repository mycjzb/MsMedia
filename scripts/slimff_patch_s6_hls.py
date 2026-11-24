#!/usr/bin/env python3
# 阶段 6：精简 FFmpeg HLS 对标 Google Media3（ExoPlayer）补丁 —— 全部在 MS_SLIM_S6 宏守卫内，
# 未定义该宏时与 s5 源码逐字等价（s5 前缀仍可原样复现）。幂等：已打过则跳过。
#   1) open_input：打开当前媒体切片前，把「从当前切片起的后续切片计划」交给单 so 内的预下载器
#      （src/ff/ms_hls_pf.c）：按 hls.c 自己的 range_end 规则把连续 BYTERANGE 切片合并成同一个请求单元，
#      AES-128 切片单独成单元（密文预下载，消费时用 libavutil AES 流式解密），只计划 http(s) 切片；
#   2) reload_playlist：直播 m3u8 刷新失败不再立刻报错退出（此前 API 34 偶发 -1004 的同类根因：
#      一次瞬时 connect refused 就终止播放），按 Media3 风格有限次退避重试（3 次，200/400/800 ms，
#      不超过 0.5*targetDuration），全程可被 interrupt 立即打断；无更新时仍按 0.5*targetDuration 退避刷新；
#   3) 切片打开失败：seg_max_retry 默认 0→3，每次重试前短退避 50/100/200 ms（可打断），不再一失败就跳片。
# by: Long <mycjzb@gmail.com>
import pathlib, sys

src = pathlib.Path(sys.argv[1])
p = src / "libavformat" / "hls.c"
t = p.read_text()
MARK = "/* msmedia-s6-hls-media3 */"
if MARK in t:
    print("s6-hls already patched")
    sys.exit(0)

def rep(old, new, count=1):
    global t
    assert t.count(old) == count, (t.count(old), old[:100])
    t = t.replace(old, new)

# ---- 声明（预下载器在 libmsplayer.so 内实现；签名只用基础类型，避免 FFmpeg 构建依赖单 so 头文件） ----
rep("static void free_playlist_list(HLSContext *c)\n{\n",
    MARK + "\n"
    "#ifdef MS_SLIM_S6\n"
    "int  ms_hls_pf_enabled(AVFormatContext *s);\n"
    "void ms_hls_pf_plan_begin(AVFormatContext *s, int pls_index, int live, int64_t target_us);\n"
    "void ms_hls_pf_plan_add(AVFormatContext *s, int pls_index, const char *url, int64_t off, int64_t end, int64_t dur_us);\n"
    "void ms_hls_pf_plan_end(AVFormatContext *s, int pls_index, AVDictionary *opts);\n"
    "/* 可被 interrupt 打断的睡眠：返回 AVERROR_EXIT 表示已取消。 */\n"
    "static int ms_s6_sleep(AVIOInterruptCB *cb, int64_t us)\n"
    "{\n"
    "    int64_t end = av_gettime_relative() + us;\n"
    "    while (av_gettime_relative() < end) {\n"
    "        if (ff_check_interrupt(cb))\n"
    "            return AVERROR_EXIT;\n"
    "        av_usleep(FFMIN(20000, FFMAX(1, end - av_gettime_relative())));\n"
    "    }\n"
    "    return 0;\n"
    "}\n"
    "#define MS_HLS_SEG_RETRY_DEFAULT 3\n"
    "#else\n"
    "#define MS_HLS_SEG_RETRY_DEFAULT 0\n"
    "#endif\n\n"
    "static void free_playlist_list(HLSContext *c)\n{\n")

# ---- 1) 预下载计划：在 open_input 发请求前（range_end/segment_continues 已定义在其之前） ----
rep("""    av_log(pls->parent, AV_LOG_VERBOSE, "HLS request for url '%s', offset %"PRId64", playlist %d\\n",
           seg->url, seg->url_offset, pls->index);
""", """    av_log(pls->parent, AV_LOG_VERBOSE, "HLS request for url '%s', offset %"PRId64", playlist %d\\n",
           seg->url, seg->url_offset, pls->index);
#ifdef MS_SLIM_S6
    /* msmedia s6：只在打开「当前媒体切片」时下发计划（初始化段/密钥不走预下载）。
     * 单元划分与 range_end 一致：同 URL 连续 BYTERANGE 且无加密合并为一个请求，保证消费端请求逐字命中。 */
    if (seg == current_segment(pls) && ms_hls_pf_enabled(pls->parent)) {
        int64_t i = pls->cur_seq_no - pls->start_seq_no;
        ms_hls_pf_plan_begin(pls->parent, pls->index, !pls->finished, pls->target_duration);
        while (i >= 0 && i < pls->n_segments) {
            struct segment *u = pls->segments[i];
            int64_t uoff = -1, uend = -1, udur = u->duration;
            if (u->size >= 0) {
                uoff = u->url_offset;
                while (i + 1 < pls->n_segments && segment_continues(pls->segments[i], pls->segments[i + 1], INT64_MAX)) {
                    i++;
                    udur += pls->segments[i]->duration;
                }
                uend = pls->segments[i]->url_offset + pls->segments[i]->size;
            }
            if (u->key_type != KEY_SAMPLE_AES && av_strstart(u->url, "http", NULL))
                ms_hls_pf_plan_add(pls->parent, pls->index, u->url, uoff, uend, udur);
            else
                break; /* 非 http(s) / SAMPLE-AES：后面不再预下载，交回 FFmpeg 原路径 */
            i++;
        }
        ms_hls_pf_plan_end(pls->parent, pls->index, c->avio_opts);
    }
#endif
""")

# ---- 2) 直播 m3u8 刷新失败：Media3 风格退避重试 ----
rep("""        if (!v->finished &&
            av_gettime_relative() - v->last_load_time >= reload_interval) {
            if ((ret = parse_playlist(c, v->url, v, NULL)) < 0) {
                if (ret != AVERROR_EXIT)
                    av_log(v->parent, AV_LOG_WARNING, "Failed to reload playlist %d\\n",
                           v->index);
                return ret;
            }""", """        if (!v->finished &&
            av_gettime_relative() - v->last_load_time >= reload_interval) {
#ifdef MS_SLIM_S6
            int ms_try = 0;
            while ((ret = parse_playlist(c, v->url, v, NULL)) < 0 && ret != AVERROR_EXIT && ms_try < 3) {
                /* 200/400/800 ms，且不超过 0.5*targetDuration（至少 100 ms） */
                int64_t bo = FFMIN((int64_t)200000 << ms_try, FFMAX(v->target_duration / 2, 100000));
                av_log(v->parent, AV_LOG_WARNING, "msmedia: reload playlist %d failed (%s), retry %d in %"PRId64" ms\\n",
                       v->index, av_err2str(ret), ms_try + 1, bo / 1000);
                if (ms_s6_sleep(c->interrupt_callback, bo) < 0)
                    return AVERROR_EXIT;
                ms_try++;
            }
            if (ret < 0) {
#else
            if ((ret = parse_playlist(c, v->url, v, NULL)) < 0) {
#endif
                if (ret != AVERROR_EXIT)
                    av_log(v->parent, AV_LOG_WARNING, "Failed to reload playlist %d\\n",
                           v->index);
                return ret;
            }""")

# ---- 3) 切片打开失败：短退避后重试 ----
rep("""            } else {
                segment_retries++;
            }
            goto restart;""", """            } else {
                segment_retries++;
#ifdef MS_SLIM_S6
                /* Media3 风格短退避：50/100/200 ms，可被 cancel 立即打断 */
                if (ms_s6_sleep(c->interrupt_callback, (int64_t)25000 << FFMIN(segment_retries, 3)) < 0)
                    return AVERROR_EXIT;
#endif
            }
            goto restart;""")
rep("OFFSET(seg_max_retry), AV_OPT_TYPE_INT, {.i64 = 0}", "OFFSET(seg_max_retry), AV_OPT_TYPE_INT, {.i64 = MS_HLS_SEG_RETRY_DEFAULT}")
p.write_text(t)
print("s6-hls patched")
