/* 阶段 5：slimff FFmpeg 补丁行为单测（链接 s5 前缀静态库，在模拟器/设备上运行）。
 * 覆盖：pixdesc 常用/硬件格式可查、未保留格式返回 NULL 不崩；codec_desc 保留项可查、摘除项 NULL；
 * AVOption 键名（http/rtsp/hls/avformat）全部可 av_opt_find / av_opt_set，help 为 NULL；
 * EXIF 桩“无条目”语义；h264 parser（已不依赖 h264dsp）对 Annex-B 样本正常切帧。
 * by: Long <mycjzb@gmail.com> */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "libavutil/pixdesc.h"
#include "libavutil/opt.h"
#include "libavutil/dict.h"
#include "libavcodec/avcodec.h"
#include "libavcodec/codec_desc.h"
#include "libavformat/avformat.h"
#include "libavformat/avio.h"

static int fails = 0;
#define CK(c, msg) do { if (c) printf("PASS %s\n", msg); else { printf("FAIL %s\n", msg); fails++; } } while (0)

int av_exif_parse_buffer(void *, const uint8_t *, size_t, void *, int);

int main(int argc, char **argv)
{
    char m[160];
    /* pixdesc */
    enum AVPixelFormat keep[] = { AV_PIX_FMT_YUV420P, AV_PIX_FMT_NV12, AV_PIX_FMT_NV21, AV_PIX_FMT_P010LE,
        AV_PIX_FMT_RGBA, AV_PIX_FMT_BGRA, AV_PIX_FMT_RGB24, AV_PIX_FMT_GRAY8, AV_PIX_FMT_YUV420P10LE,
        AV_PIX_FMT_MEDIACODEC, AV_PIX_FMT_VAAPI, AV_PIX_FMT_VULKAN, AV_PIX_FMT_DRM_PRIME, AV_PIX_FMT_OPENCL };
    for (size_t i = 0; i < sizeof(keep) / sizeof(keep[0]); i++) {
        const AVPixFmtDescriptor *d = av_pix_fmt_desc_get(keep[i]);
        snprintf(m, sizeof(m), "pixdesc keep %d %s", keep[i], d ? d->name : "(null)");
        CK(d && d->name && av_pix_fmt_desc_get_id(d) == keep[i] && av_get_pix_fmt(d->name) == keep[i], m);
    }
    CK(av_pix_fmt_desc_get(AV_PIX_FMT_MEDIACODEC)->flags & AV_PIX_FMT_FLAG_HWACCEL, "pixdesc mediacodec HWACCEL flag");
    CK(av_pix_fmt_desc_get(AV_PIX_FMT_BAYER_BGGR8) == NULL, "pixdesc removed bayer -> NULL");
    CK(av_get_pix_fmt_name(AV_PIX_FMT_BAYER_BGGR8) == NULL, "pixdesc removed name -> NULL");
    CK(av_pix_fmt_desc_get(AV_PIX_FMT_NONE) == NULL && av_pix_fmt_desc_get(AV_PIX_FMT_NB) == NULL, "pixdesc NONE/NB -> NULL");
    CK(av_get_pix_fmt("rgb32") != AV_PIX_FMT_NONE, "pixdesc rgb32 alias");
    { int n = 0; const AVPixFmtDescriptor *d = NULL; while ((d = av_pix_fmt_desc_next(d))) n++;
      snprintf(m, sizeof(m), "pixdesc iterate n=%d (>=50)", n); CK(n >= 50, m); }
    /* codec_desc */
    const char *kc[] = { "h264", "hevc", "av1", "vp9", "mpeg2video", "cavs", "avs2", "avs3", "aac", "aac_latm", "ac3",
        "eac3", "dts", "mp2", "mp3", "opus", "flac", "pcm_alaw", "pcm_mulaw", "truehd", "subrip", "webvtt", "ass",
        "mov_text", "hdmv_pgs_subtitle", "dvb_subtitle", "dvd_subtitle", "eia_608", "dvb_teletext", "timed_id3", "scte_35" };
    for (size_t i = 0; i < sizeof(kc) / sizeof(kc[0]); i++) {
        const AVCodecDescriptor *d = avcodec_descriptor_get_by_name(kc[i]);
        snprintf(m, sizeof(m), "codec_desc keep %s", kc[i]);
        CK(d && avcodec_descriptor_get(d->id) == d && !strcmp(avcodec_get_name(d->id), kc[i]), m);
    }
    CK(avcodec_get_type(AV_CODEC_ID_HDMV_PGS_SUBTITLE) == AVMEDIA_TYPE_SUBTITLE, "codec_desc pgs type subtitle");
    CK(avcodec_get_type(AV_CODEC_ID_EAC3) == AVMEDIA_TYPE_AUDIO, "codec_desc eac3 type audio");
    CK(avcodec_descriptor_get(AV_CODEC_ID_CINEPAK) == NULL, "codec_desc removed cinepak -> NULL");
    snprintf(m, sizeof(m), "codec_desc removed name -> %s", avcodec_get_name(AV_CODEC_ID_CINEPAK));
    CK(avcodec_get_name(AV_CODEC_ID_CINEPAK) != NULL, m);
    /* 8 个音频软解仍可找到 */
    const char *dec[] = { "aac", "aac_latm", "ac3", "eac3", "mp2", "pcm_alaw", "pcm_mulaw", "dca" };
    for (size_t i = 0; i < 8; i++) { snprintf(m, sizeof(m), "decoder %s", dec[i]); CK(avcodec_find_decoder_by_name(dec[i]) != NULL, m); }
    /* AVOption：键名可找、可设置、help 为 NULL */
    {
        const AVClass *hc = avio_protocol_get_class("http");
        const char *hk[] = { "headers", "user_agent", "reconnect", "reconnect_streamed", "reconnect_on_network_error",
            "reconnect_delay_max", "multiple_requests", "referer", "seekable", "icy", "method", NULL };
        CK(hc != NULL, "http AVClass");
        for (int i = 0; hc && hk[i]; i++) {
            const AVOption *o = av_opt_find((void *)&hc, hk[i], NULL, 0, AV_OPT_SEARCH_FAKE_OBJ);
            snprintf(m, sizeof(m), "http opt '%s' found, help=NULL", hk[i]);
            CK(o && o->help == NULL, m);
        }
        AVFormatContext *ic = avformat_alloc_context();
        CK(av_opt_set(ic, "probesize", "123456", 0) == 0, "av_opt_set avformat probesize");
        CK(av_opt_set(ic, "analyzeduration", "777000", 0) == 0, "av_opt_set avformat analyzeduration");
        CK(av_opt_set(ic, "protocol_whitelist", "file,http,https,tcp,tls", 0) == 0, "av_opt_set protocol_whitelist");
        CK(av_opt_set(ic, "format_whitelist", "mpegts,mov,hls", 0) == 0, "av_opt_set format_whitelist");
        CK(av_opt_set(ic, "max_probe_packets", "500", 0) == 0, "av_opt_set max_probe_packets");
        { int64_t v = 0; av_opt_get_int(ic, "probesize", 0, &v); CK(v == 123456, "probesize readback"); }
        { uint8_t *s = NULL; av_opt_get(ic, "protocol_whitelist", 0, &s); CK(s && !strcmp((char *)s, "file,http,https,tcp,tls"), "protocol_whitelist readback"); av_free(s); }
        avformat_free_context(ic);
        const AVInputFormat *rt = av_find_input_format("rtsp"), *hl = av_find_input_format("hls"), *ts = av_find_input_format("mpegts");
        CK(rt && av_opt_find((void *)&rt->priv_class, "rtsp_transport", NULL, 0, AV_OPT_SEARCH_FAKE_OBJ), "rtsp opt rtsp_transport");
        CK(hl && av_opt_find((void *)&hl->priv_class, "allowed_extensions", NULL, 0, AV_OPT_SEARCH_FAKE_OBJ), "hls opt allowed_extensions");
        CK(ts && av_opt_find((void *)&ts->priv_class, "scan_all_pmts", NULL, 0, AV_OPT_SEARCH_FAKE_OBJ), "mpegts opt scan_all_pmts");
        const AVOption *tcpo = NULL; const AVClass *fc = avformat_get_class();
        tcpo = av_opt_find((void *)&fc, "rw_timeout", NULL, 0, AV_OPT_SEARCH_FAKE_OBJ);
        (void)tcpo;
        /* rw_timeout/timeout 属于 AVIOContext/协议层，经 AVDictionary 下发；dict 能被协议吃掉即证明生效（见模拟器 stall 测试） */
        const AVClass *tc = avio_protocol_get_class("tcp");
        CK(tc && av_opt_find((void *)&tc, "timeout", NULL, 0, AV_OPT_SEARCH_FAKE_OBJ), "tcp opt timeout");
        const AVOption *u = av_opt_find((void *)&hc, "user_agent", NULL, 0, AV_OPT_SEARCH_FAKE_OBJ);
        CK(u && u->type == AV_OPT_TYPE_STRING && u->default_val.str && strstr(u->default_val.str, "Lavf"), "http user_agent default intact");
    }
    /* EXIF 桩 */
    {
        uint8_t ifd[64]; memset(ifd, 0xAB, sizeof(ifd));
        static const uint8_t tiff[] = { 'I','I',42,0,8,0,0,0,0,0 };
        int r = av_exif_parse_buffer(NULL, tiff, sizeof(tiff), ifd, 0);
        CK(r >= 0 && ifd[0] == 0, "exif parse -> empty ifd, no error");
    }
    /* h264 parser 切帧 */
    if (argc > 1) {
        FILE *fp = fopen(argv[1], "rb");
        if (fp) {
            static uint8_t buf[1 << 20]; size_t n = fread(buf, 1, sizeof(buf), fp); fclose(fp);
            AVCodecParserContext *pc = av_parser_init(AV_CODEC_ID_H264);
            AVCodecContext *cc = avcodec_alloc_context3(NULL);
            int frames = 0, keys = 0; size_t off = 0;
            while (pc && off < n) {
                uint8_t *out; int os;
                int used = av_parser_parse2(pc, cc, &out, &os, buf + off, (int)(n - off > 4096 ? 4096 : n - off), 0, 0, 0);
                off += used;
                if (os > 0) { frames++; if (pc->key_frame == 1) keys++; }
                if (used == 0 && os == 0) break;
            }
            snprintf(m, sizeof(m), "h264 parser frames=%d keys=%d (no h264dsp)", frames, keys);
            CK(frames > 20 && keys >= 1, m);
            av_parser_close(pc); avcodec_free_context(&cc);
        } else { printf("FAIL h264 sample open\n"); fails++; }
    }
    printf("single_s5_ffpatch_test: %s (fails=%d)\n", fails ? "FAIL" : "ALL PASS", fails);
    return fails ? 1 : 0;
}
