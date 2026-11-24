/*
 * 测试用命令行取帧：同一公共 demux + ms_hw_decoder_submit/drive 路径，
 * 无 Activity、ByteBuffer 输出。用于区分模拟器 goldfish 管道与产品缺陷。
 * 不把成功写成真机硬解或正式出画验收。不记录 URL。
 * by: Long <mycjzb@gmail.com>
 */
#ifdef __ANDROID__
#ifdef MS_HW_HAVE_FF

#include "msmedia/ms_hw.h"
#include "msmedia/ms_ff.h"
#include "msmedia/msmedia.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int clamp_u8(int v)
{
    if (v < 0) {
        return 0;
    }
    if (v > 255) {
        return 255;
    }
    return v;
}

static int yuv_to_ppm(const char *path, const uint8_t *src, size_t src_sz,
                      int w, int h, int stride, int slice, int cf)
{
    FILE *fp;
    int x, y;
    const uint8_t *ys, *us, *vs, *uv;
    if (w <= 0 || h <= 0 || src == NULL) {
        return -1;
    }
    if (stride <= 0) {
        stride = w;
    }
    if (slice <= 0) {
        slice = h;
    }
    fp = fopen(path, "wb");
    if (!fp) {
        return -1;
    }
    fprintf(fp, "P6\n%d %d\n255\n", w, h);
    /* 19=YUV420Planar I420；21/0x7F420888 优先按 NV12，若尺寸像 I420 则 I420。 */
    if (cf == 19 || cf == 20 ||
        (cf != 21 && cf != 39 && src_sz >= (size_t)stride * (size_t)slice * 3u / 2u &&
         cf != 0x15)) {
        size_t ysz = (size_t)stride * (size_t)slice;
        size_t cstride = (size_t)stride / 2u;
        size_t cslice = (size_t)slice / 2u;
        if (cf == 21 || cf == 39 || cf == (int)0x7F420888) {
            /* fall to NV12 below */
        } else {
            if (src_sz < ysz + cstride * cslice * 2u) {
                fclose(fp);
                return -1;
            }
            ys = src;
            us = src + ysz;
            vs = us + cstride * cslice;
            for (y = 0; y < h; y++) {
                for (x = 0; x < w; x++) {
                    int Y = ys[y * stride + x];
                    int U = us[(y / 2) * (int)cstride + (x / 2)] - 128;
                    int V = vs[(y / 2) * (int)cstride + (x / 2)] - 128;
                    int r = clamp_u8((298 * (Y - 16) + 409 * V + 128) >> 8);
                    int g = clamp_u8((298 * (Y - 16) - 100 * U - 208 * V + 128) >> 8);
                    int b = clamp_u8((298 * (Y - 16) + 516 * U + 128) >> 8);
                    fputc(r, fp);
                    fputc(g, fp);
                    fputc(b, fp);
                }
            }
            fclose(fp);
            return 0;
        }
    }
    {
        size_t ysz = (size_t)stride * (size_t)slice;
        if (src_sz < ysz + ysz / 2u) {
            fclose(fp);
            return -1;
        }
        ys = src;
        uv = src + ysz;
        for (y = 0; y < h; y++) {
            for (x = 0; x < w; x++) {
                int Y = ys[y * stride + x];
                int U = uv[(y / 2) * stride + (x & ~1)] - 128;
                int V = uv[(y / 2) * stride + (x & ~1) + 1] - 128;
                int r = clamp_u8((298 * (Y - 16) + 409 * V + 128) >> 8);
                int g = clamp_u8((298 * (Y - 16) - 100 * U - 208 * V + 128) >> 8);
                int b = clamp_u8((298 * (Y - 16) + 516 * U + 128) >> 8);
                fputc(r, fp);
                fputc(g, fp);
                fputc(b, fp);
            }
        }
    }
    fclose(fp);
    return 0;
}

static const char *k_names[] = {
    "c2.android.avc.decoder",
    "OMX.google.h264.decoder",
    "c2.goldfish.h264.decoder",
    NULL
};

int main(int argc, char **argv)
{
    const char *path = NULL;
    const char *out_ppm = "/data/local/tmp/mshw-dump/frame.ppm";
    const char *force = getenv("MSMEDIA_FORCE_DECODER");
    const char *url_storage;
    char url[768];
    ms_ff *ff = NULL;
    ms_ff_limits lim;
    ms_hw_decoder *dec = NULL;
    ms_hw_create_opts copts;
    ms_hw_limits dlim;
    ms_hw_config cfg;
    uint8_t *csd = NULL;
    size_t csd_size = 0;
    ms_sample_format sfmt = MS_SAMPLE_FMT_UNKNOWN;
    int configured = 0, got = 0, i, name_i = 0;
    int au_in = 0, submitted = 0;
    const char *use_name = force;
    ms_status st;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--file") == 0 && i + 1 < argc) {
            path = argv[++i];
        } else if (strcmp(argv[i], "--out") == 0 && i + 1 < argc) {
            out_ppm = argv[++i];
        } else if (strcmp(argv[i], "--codec-name") == 0 && i + 1 < argc) {
            use_name = argv[++i];
        }
    }
    if (path == NULL || path[0] == 0) {
        fprintf(stderr, "usage: mshw_frame_dump --file PATH [--codec-name NAME] [--out PPM]\n");
        return 2;
    }
    /* 管道/超时下 stdout 全缓冲会导致被杀后日志为空；测试工具改为无缓冲。 */
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    /* 只记本地路径尾，不写完整 URL。 */
    {
        const char *slash = strrchr(path, '/');
        fprintf(stdout, "mshw_frame_dump file=%s (local path tail only)\n",
                slash ? slash + 1 : path);
    }
    if (strncmp(path, "file:", 5) == 0) {
        snprintf(url, sizeof(url), "%s", path);
    } else {
        snprintf(url, sizeof(url), "file:%s", path);
    }
    url_storage = url;
    (void)url_storage;

    ms_ff_limits_default(&lim);
    lim.struct_size = sizeof(lim);
    lim.io_timeout_ms = -1;
    if (ms_ff_create(NULL, &lim, &ff) != MS_OK) {
        fprintf(stderr, "ms_ff_create failed\n");
        return 1;
    }
    st = ms_ff_open(ff, url);
    if (st != MS_OK) {
        fprintf(stderr, "ms_ff_open failed: %s\n", ms_status_name(st));
        ms_ff_destroy(ff);
        return 1;
    }

    memset(&copts, 0, sizeof(copts));
    copts.struct_size = sizeof(copts);
    copts.allow_software = 1; /* 测试工具：允许系统软件组件，不报硬解 */
    ms_hw_limits_default(&dlim);
    dlim.struct_size = sizeof(dlim);
    if (ms_hw_decoder_create(NULL, &dlim, &copts, &dec) != MS_OK) {
        fprintf(stderr, "ms_hw_decoder_create failed\n");
        ms_ff_destroy(ff);
        return 1;
    }

    for (;;) {
        ms_event ev;
        memset(&ev, 0, sizeof(ev));
        st = ms_ff_pull(ff, &ev);
        if (st == MS_EOS) {
            break;
        }
        if (st == MS_AGAIN) {
            if (configured) {
                (void)ms_hw_decoder_drive(dec);
            }
            continue;
        }
        if (st != MS_OK) {
            fprintf(stderr, "ms_ff_pull: %s\n", ms_status_name(st));
            break;
        }
        if (ev.track.codec != MS_CODEC_H264 && ev.track.kind != MS_TRACK_VIDEO) {
            if (ev.release_id) {
                (void)ms_ff_release(ff, ev.release_id);
            }
            continue;
        }
        if (ev.track.codec != MS_CODEC_H264 && ev.track.codec != MS_CODEC_UNKNOWN) {
            if (ev.release_id) {
                (void)ms_ff_release(ff, ev.release_id);
            }
            continue;
        }
        if (ev.type == MS_EVENT_CODEC_CONFIG && ev.data != NULL && ev.data_size > 0 &&
            csd == NULL) {
            csd = (uint8_t *)malloc(ev.data_size);
            if (csd) {
                memcpy(csd, ev.data, ev.data_size);
                csd_size = ev.data_size;
                sfmt = ev.sample_format;
            }
            if (ev.release_id) {
                (void)ms_ff_release(ff, ev.release_id);
            }
            continue;
        }
        if (ev.type != MS_EVENT_AU) {
            if (ev.release_id) {
                (void)ms_ff_release(ff, ev.release_id);
            }
            continue;
        }
        if (!configured) {
            int w = 0, h = 0;
            const char *names[5];
            int ni = 0, tried = 0;
            if (use_name && use_name[0]) {
                names[ni++] = use_name;
            } else {
                while (k_names[name_i]) {
                    names[ni++] = k_names[name_i++];
                }
            }
            names[ni] = NULL;
            if (csd && csd_size) {
                (void)ms_h264_csd_video_size(sfmt, 1, csd, csd_size, 0, &w, &h);
            }
            memset(&cfg, 0, sizeof(cfg));
            cfg.struct_size = sizeof(cfg);
            cfg.codec = MS_CODEC_H264;
            cfg.sample_fmt = (sfmt != MS_SAMPLE_FMT_UNKNOWN) ? sfmt : MS_SAMPLE_FMT_AVCC;
            cfg.csd = csd;
            cfg.csd_size = csd_size;
            cfg.width = w;
            cfg.height = h;
            cfg.time_base_num = 1;
            cfg.time_base_den = 1000000;
            cfg.surface = NULL;
            fprintf(stdout, "csd_bytes=%zu fmt=%d wxh=%dx%d\n",
                    csd_size, (int)cfg.sample_fmt, w, h);
            for (tried = 0; names[tried]; tried++) {
                (void)ms_hw_decoder_set_preferred_name(dec, names[tried]);
                st = ms_hw_decoder_configure(dec, &cfg);
                if (st == MS_OK) {
                    use_name = names[tried];
                    break;
                }
                fprintf(stderr, "configure %s: %s detail may be in caps\n",
                        names[tried], ms_status_name(st));
            }
            if (st != MS_OK) {
                fprintf(stderr, "all configure attempts failed\n");
                if (ev.release_id) {
                    (void)ms_ff_release(ff, ev.release_id);
                }
                break;
            }
            st = ms_hw_decoder_start(dec);
            if (st != MS_OK) {
                fprintf(stderr, "start failed: %s\n", ms_status_name(st));
                if (ev.release_id) {
                    (void)ms_ff_release(ff, ev.release_id);
                }
                break;
            }
            {
                ms_hw_caps caps;
                memset(&caps, 0, sizeof(caps));
                caps.struct_size = sizeof(caps);
                (void)ms_hw_decoder_get_caps(dec, &caps);
                fprintf(stdout,
                        "configured name=%s kind=%s is_hardware=%d (software is not hardware) "
                        "wxh=%dx%d\n",
                        caps.codec_name ? caps.codec_name : "?",
                        ms_hw_kind_name(caps.kind), caps.is_hardware, w, h);
            }
            configured = 1;
        }
        au_in++;
        {
            ms_hw_buf buf;
            unsigned flags = ev.is_keyframe ? MS_HW_FLAG_KEY : 0;
            int64_t pts = 0;
            int tries;
            memset(&buf, 0, sizeof(buf));
            buf.data = ev.data;
            buf.size = ev.data_size;
            if (ev.has_pts) {
                /* 粗略：若 time_base 已知则换成微秒，否则用原值。 */
                if (ev.time_base_den > 0 && ev.time_base_num > 0) {
                    pts = (int64_t)((ev.pts * ev.time_base_num * 1000000ull) /
                                    (uint64_t)ev.time_base_den);
                } else {
                    pts = (int64_t)ev.pts;
                }
            }
            for (tries = 0; tries < 200; tries++) {
                st = ms_hw_decoder_submit(dec, &buf, pts, flags);
                if (st != MS_LIMIT && st != MS_AGAIN) {
                    break;
                }
                (void)ms_hw_decoder_drive(dec);
                usleep(5000);
            }
            if (st == MS_OK) {
                submitted++;
            }
        }
        if (ev.release_id) {
            (void)ms_ff_release(ff, ev.release_id);
        }
        if (configured) {
            int dw = 0, dh = 0, stride = 0, slice = 0, cf = 0;
            size_t need = 0;
            (void)ms_hw_decoder_drive(dec);
            if (ms_hw_decoder_copy_last_output(dec, &dw, &dh, &stride, &slice, &cf,
                                               NULL, 0, &need) == MS_OK &&
                need > 0) {
                uint8_t *raw = (uint8_t *)malloc(need);
                if (raw &&
                    ms_hw_decoder_copy_last_output(dec, &dw, &dh, &stride, &slice, &cf,
                                                   raw, need, &need) == MS_OK) {
                    if (yuv_to_ppm(out_ppm, raw, need, dw, dh, stride, slice, cf) == 0) {
                        fprintf(stdout,
                                "got frame %dx%d color=%d stride=%d slice=%d bytes=%zu "
                                "au_in=%d submitted=%d codec=%s out=%s\n",
                                dw, dh, cf, stride, slice, need, au_in, submitted,
                                use_name ? use_name : "?", out_ppm);
                        got = 1;
                        free(raw);
                        break;
                    }
                    fprintf(stderr, "yuv_to_ppm failed color=%d size=%zu %dx%d\n",
                            cf, need, dw, dh);
                }
                free(raw);
            }
        }
        if (au_in >= 80 && !got) {
            (void)ms_hw_decoder_signal_eos(dec);
        }
        if (au_in >= 120) {
            break;
        }
    }
    if (configured && !got) {
        int k;
        (void)ms_hw_decoder_signal_eos(dec);
        for (k = 0; k < 100 && !got; k++) {
            int dw = 0, dh = 0, stride = 0, slice = 0, cf = 0;
            size_t need = 0;
            (void)ms_hw_decoder_drive(dec);
            if (ms_hw_decoder_copy_last_output(dec, &dw, &dh, &stride, &slice, &cf,
                                               NULL, 0, &need) == MS_OK &&
                need > 0) {
                uint8_t *raw = (uint8_t *)malloc(need);
                if (raw &&
                    ms_hw_decoder_copy_last_output(dec, &dw, &dh, &stride, &slice, &cf,
                                                   raw, need, &need) == MS_OK &&
                    yuv_to_ppm(out_ppm, raw, need, dw, dh, stride, slice, cf) == 0) {
                    fprintf(stdout,
                            "got frame after eos %dx%d color=%d au_in=%d submitted=%d out=%s\n",
                            dw, dh, cf, au_in, submitted, out_ppm);
                    got = 1;
                }
                free(raw);
            }
            usleep(8000);
        }
    }
    fprintf(stdout, "done got=%d au_in=%d submitted=%d codec=%s\n",
            got, au_in, submitted, use_name ? use_name : "?");
    fflush(stdout);
    if (dec) {
        ms_hw_decoder_destroy(dec);
    }
    if (ff) {
        ms_ff_destroy(ff);
    }
    free(csd);
    return got ? 0 : 1;
}

#else
int main(void)
{
    fprintf(stderr, "mshw_frame_dump requires MS_HW_HAVE_FF\n");
    return 2;
}
#endif
#else
int main(void)
{
    return 2;
}
#endif
