#!/usr/bin/env python3
# 精简 FFmpeg（single-slimff）源码副本补丁：HLS 切片不看后缀 + 图片伪装头/脏字节剥离。
#   1) hls.c 选项默认：allowed_extensions / allowed_segment_extensions = ALL，extension_picky = 0
#      （按字节探测而非 URL 后缀；.png/.jpg/.html/无后缀切片都能进）；
#   2) read_from_url：每个切片（含 EXT-X-MAP 初始化段）起点先有界预读，调用单 so 内的
#      ms_probe_media_offset（src/core/ms_probe.c，两条路线共用同一份实现）定位真正媒体起点，
#      剥掉 PNG(到 IEND+CRC)/JPEG(到 FFD9)/GIF/BMP/WebP 伪装头与前导脏字节，余量放在 pending 里续读；
#      切片切换/seek/swap 时复位，playlist 释放时 av_freep，零泄漏。
# 幂等：已打过补丁则跳过。只改 build/single-analysis/ffmpeg-9.1-slimff-src 副本。
# by: Long <mycjzb@gmail.com>
import pathlib, sys

src = pathlib.Path(sys.argv[1])
p = src / "libavformat" / "hls.c"
t = p.read_text()
MARK = "/* msmedia-hls-probe */"
if MARK in t:
    sys.exit(0)

def rep(old, new, count=1):
    global t
    assert t.count(old) >= 1, old[:80]
    t = t.replace(old, new, count)

rep("    int64_t cur_seg_offset;\n",
    "    int64_t cur_seg_offset;\n"
    "    uint8_t *ms_pend;      /* msmedia：剥离后的预读余量 */\n"
    "    int ms_pend_len, ms_pend_pos, ms_probed;\n")

rep("static void free_playlist_list(HLSContext *c)\n{\n",
    MARK + "\n"
    "int ms_probe_media_offset(const uint8_t *d, int n, int eof, int *need_more);\n"
    "#define MS_PROBE_RESET(p) do { av_freep(&(p)->ms_pend); (p)->ms_pend_len = (p)->ms_pend_pos = 0; (p)->ms_probed = 0; } while (0)\n\n"
    "static void free_playlist_list(HLSContext *c)\n{\n")
rep("        av_freep(&pls->init_sec_buf);\n", "        av_freep(&pls->init_sec_buf);\n        MS_PROBE_RESET(pls);\n")

old_rfu = """static int read_from_url(struct playlist *pls, struct segment *seg,
                         uint8_t *buf, int buf_size)
{
    int ret;

     /* limit read if the segment was only a part of a file */
    if (seg->size >= 0)
        buf_size = FFMIN(buf_size, seg->size - pls->cur_seg_offset);

    ret = avio_read(pls->input, buf, buf_size);
    if (ret > 0)
        pls->cur_seg_offset += ret;

    return ret;
}"""
new_rfu = """static int read_from_url_raw(struct playlist *pls, struct segment *seg,
                             uint8_t *buf, int buf_size)
{
    int ret;

     /* limit read if the segment was only a part of a file */
    if (seg->size >= 0)
        buf_size = FFMIN(buf_size, seg->size - pls->cur_seg_offset);

    ret = avio_read(pls->input, buf, buf_size);
    if (ret > 0)
        pls->cur_seg_offset += ret;

    return ret;
}

/* msmedia：切片起点有界预读 + 字节探测剥离伪装头；之后先吐 pending 再直读（保持 avio_read 填满语义）。 */
static int read_from_url(struct playlist *pls, struct segment *seg,
                         uint8_t *buf, int buf_size)
{
    int got = 0, ret;
    if (buf_size <= 0)
        return read_from_url_raw(pls, seg, buf, buf_size);
    if (pls->cur_seg_offset == 0 && !pls->ms_probed) {
        const int cap = 256 * 1024;
        int len = 0, eof = 0, need = 1, off;
        uint8_t *tmp = av_malloc(cap);
        MS_PROBE_RESET(pls);
        pls->ms_probed = 1;
        if (!tmp)
            return AVERROR(ENOMEM);
        while (len < cap) {
            int r = read_from_url_raw(pls, seg, tmp + len, FFMIN(cap - len, 16384));
            if (r <= 0) {
                if (r < 0 && r != AVERROR_EOF && len == 0) {
                    av_free(tmp);
                    pls->ms_probed = 0;
                    return r;
                }
                eof = 1;
                break;
            }
            len += r;
            (void)ms_probe_media_offset(tmp, len, 0, &need);
            if (!need)
                break;
        }
        off = ms_probe_media_offset(tmp, len, eof || len >= cap, &need);
        if (off < 0 || off > len)
            off = 0;
        if (off > 0)
            av_log(pls->parent, AV_LOG_VERBOSE, "msmedia: stripped %d disguise/junk bytes at segment start\\n", off);
        pls->ms_pend = tmp;
        pls->ms_pend_len = len;
        pls->ms_pend_pos = off;
    }
    if (pls->ms_pend && pls->ms_pend_pos < pls->ms_pend_len) {
        got = FFMIN(buf_size, pls->ms_pend_len - pls->ms_pend_pos);
        memcpy(buf, pls->ms_pend + pls->ms_pend_pos, got);
        pls->ms_pend_pos += got;
        if (pls->ms_pend_pos >= pls->ms_pend_len)
            av_freep(&pls->ms_pend);
        if (got == buf_size)
            return got;
    }
    ret = read_from_url_raw(pls, seg, buf + got, buf_size - got);
    if (ret < 0)
        return got > 0 ? got : ret;
    return got + ret;
}"""
rep(old_rfu, new_rfu)
# 所有切片复位点
t = t.replace("    pls->cur_seg_offset = 0;\n    return ret;\n}", "    pls->cur_seg_offset = 0;\n    MS_PROBE_RESET(pls);\n    return ret;\n}", 1)
t = t.replace("            v->cur_seg_offset = 0;\n            v->input_next_requested = 0;", "            v->cur_seg_offset = 0;\n            MS_PROBE_RESET(v);\n            v->input_next_requested = 0;", 1)
t = t.replace("        v->cur_seg_offset = 0;\n        just_opened = 1;", "        v->cur_seg_offset = 0;\n        MS_PROBE_RESET(v);\n        just_opened = 1;", 1)
t = t.replace("            pls->cur_seg_offset = 0;\n            pls->cur_init_section = NULL;", "            pls->cur_seg_offset = 0;\n            MS_PROBE_RESET(pls);\n            pls->cur_init_section = NULL;", 1)
assert t.count("MS_PROBE_RESET(") >= 7, t.count("MS_PROBE_RESET(")
# 选项默认：不看后缀
import re
t = re.sub(r'(\{"allowed_extensions",.*?\{\.str = )"[^}]*?\},', r'\1"ALL"},', t, count=1, flags=re.S)
t = re.sub(r'(\{"allowed_segment_extensions",.*?\{\.str = )"[^}]*?\},', r'\1"ALL"},', t, count=1, flags=re.S)
rep('OFFSET(extension_picky), AV_OPT_TYPE_BOOL, {.i64 = 1}', 'OFFSET(extension_picky), AV_OPT_TYPE_BOOL, {.i64 = 0}')
p.write_text(t)
print("hls-probe patched")
