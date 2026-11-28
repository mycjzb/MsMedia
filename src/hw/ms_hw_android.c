/*
 * Android H264/HEVC/MPEG-2/MPEG-4 Part 2 硬解：API 21+ AMediaCodec + Surface；
 * 选择走 JNI MediaCodecList。无硬件如实 UNSUPPORTED，不回退软解。
 * 不把 createDecoderByType 成功当作硬解。不 CPU 回读输出。
 * by: Long <mycjzb@gmail.com>
 */
#ifdef __ANDROID__

#include "ms_hw_internal.h"
#include "ms_hw_jni.h"

#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <android/log.h>
#include <dlfcn.h>
#include <time.h>
#include <sys/system_properties.h>
#ifndef MS_HW_LEGACY_JNI
#include <android/native_window.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#include <media/NdkMediaError.h>
#include "ms_hw_mediandk.h"
/* MediaCodec.BUFFER_FLAG_KEY_FRAME=1；NDK 21（超级直播同款）头文件未暴露 */
#ifndef AMEDIACODEC_BUFFER_FLAG_KEY_FRAME
#define AMEDIACODEC_BUFFER_FLAG_KEY_FRAME 1
#endif
#endif

static int device_api(int hint)
{
    char v[PROP_VALUE_MAX];
    int n, x;
    if (hint > 0)
        return hint;
    v[0] = 0;
    n = __system_property_get("ro.build.version.sdk", v);
    if (n > 0) {
        x = atoi(v);
        if (x > 0)
            return x;
    }
    return 21;
}

static const char *no_hw_video_msg(ms_codec codec)
{
    if (codec == MS_CODEC_HEVC) {
        return "no hardware HEVC decoder, software refused";
    }
    if (codec == MS_CODEC_MPEG2_VIDEO) {
        return "no hardware MPEG-2 decoder, software refused";
    }
    if (codec == MS_CODEC_MPEG4_PART2) {
        return "no hardware MPEG-4 Part 2 decoder, software refused";
    }
    if (codec == MS_CODEC_H263) {
        return "no hardware H.263 decoder, software refused";
    }
    if (codec == MS_CODEC_AV1) {
        return "no hardware AV1 decoder, software refused";
    }
    if (codec == MS_CODEC_AVS_PLUS) {
        return "no hardware AVS+ decoder in MediaCodecList (checked video/avs+,avs,cavs and hisi*avs names); software refused";
    }
    return "no hardware AVC decoder, software refused";
}

static ms_status pick_android_video(ms_codec codec, int api, int allow_sw,
                                    char *name, size_t name_cap,
                                    ms_hw_kind *kind, int *api_out,
                                    char *detail, size_t detail_cap)
{
    return ms_hw_jni_pick_video(codec, api, allow_sw, name, name_cap, kind, api_out,
                                detail, detail_cap);
}

#ifndef MS_HW_LEGACY_JNI

typedef struct ms_hw_and {
    AMediaCodec *codec;
    ANativeWindow *window;
    int acquired;
    /* 无 Surface 时保存最近一帧，供测试回读；Surface 路径不填。 */
    uint8_t *out_copy;
    size_t out_copy_cap;
    size_t out_copy_size;
    int out_w;
    int out_h;
    int out_stride;
    int out_slice;
    int out_color;
    /* 0=已是 Annex B；2/3/4=AVCC/HVCC 长度字段，提交前换成起始码。 */
    unsigned nal_len_size;
    int started;
    int closing;
    int fr_hooked;
    /* Media3 式出帧：太早的输出缓冲留在这里（最多 1 个），下轮 drive 再决定。
     * flush/close 后 index 失效，必须清掉。by: Long <mycjzb@gmail.com> */
    int held;
    ssize_t held_idx;
    AMediaCodecBufferInfo held_info;
} ms_hw_and;

/* MediaCodec 的 H.264 csd-0/csd-1 需要起始码。split 给出的是裸 NAL。 */
static void set_h264_csd_buffer(AMediaFormat *fmt, const char *key,
                                const uint8_t *nal, size_t n)
{
    uint8_t tmp[MS_HW_CSD_CAP + 4u];
    size_t out_n = 0;
    if (fmt == NULL || key == NULL || nal == NULL || n == 0) {
        return;
    }
    if (ms_hw_nal_with_start_code(nal, n, tmp, sizeof(tmp), &out_n) != 0) {
        return;
    }
    AMediaFormat_setBuffer(fmt, key, tmp, out_n);
}

static void fill_caps(ms_hw_caps *out, ms_codec codec, int api,
                      ms_hw_kind kind, const char *backend,
                      const char *name, char *name_buf, size_t name_cap,
                      const char *detail, char *detail_buf, size_t detail_cap)
{
    if (!out)
        return;
    out->codec = codec;
    out->kind = kind;
    out->is_hardware = (kind == MS_HW_KIND_HARDWARE);
    out->verified = 1;
    out->api_level = api;
    out->backend = backend;
    if (name && name[0] && name_buf && name_cap) {
        ms_hw_set_detail_buf(name_buf, name_cap, name);
        out->codec_name = name_buf;
    } else {
        out->codec_name = NULL;
    }
    if (detail_buf && detail_cap) {
        ms_hw_set_detail_buf(detail_buf, detail_cap, detail ? detail : "");
        out->detail = detail_buf;
    } else {
        out->detail = detail;
    }
}

ms_status ms_hw_android_query(ms_codec codec, int api_level, int allow_sw,
                              ms_hw_caps *out, char *name_buf, size_t name_cap,
                              char *detail_buf, size_t detail_cap)
{
    int api = device_api(api_level);
    char name[128];
    char detail[192];
    ms_hw_kind kind = MS_HW_KIND_NONE;
    ms_status st;

    name[0] = 0;
    detail[0] = 0;
    if (out)
        memset(out, 0, sizeof(*out)), out->struct_size = sizeof(*out);

    if (codec != MS_CODEC_UNKNOWN && !ms_video_hw_path_ready(codec)) {
        fill_caps(out, codec, api, MS_HW_KIND_NONE, "amediacodec",
                  NULL, name_buf, name_cap,
                  "configure/submit for this codec is not implemented in this milestone",
                  detail_buf, detail_cap);
        return MS_OK;
    }
    {
        ms_codec q = (codec == MS_CODEC_UNKNOWN) ? MS_CODEC_H264 : codec;
        if (!ms_hw_jni_has_vm()) {
            fill_caps(out, q, api, MS_HW_KIND_NONE, "amediacodec",
                      NULL, name_buf, name_cap,
                      "MediaCodecList requires a JVM; not claiming hardware",
                      detail_buf, detail_cap);
            return MS_OK;
        }
        st = pick_android_video(q, api, allow_sw, name, sizeof(name), &kind,
                                &api, detail, sizeof(detail));
        if (st == MS_OK) {
            fill_caps(out, q, api, kind,
                      (api >= 21) ? "amediacodec" : "jni-mediacodec",
                      name, name_buf, name_cap, detail, detail_buf, detail_cap);
            return MS_OK;
        }
        fill_caps(out, q, api,
                  (kind == MS_HW_KIND_SOFTWARE) ? MS_HW_KIND_SOFTWARE
                                                : MS_HW_KIND_NONE,
                  "amediacodec", name[0] ? name : NULL, name_buf, name_cap,
                  detail[0] ? detail : no_hw_video_msg(q),
                  detail_buf, detail_cap);
        return MS_OK;
    }
}


/* AMediaCodec_releaseOutputBufferAtTime 自 API 21（libmediandk）；dlsym 取，缺失则回退立即 render。
 * 对齐 Media3 renderOutputBufferV21：releaseTimeNs 为 System.nanoTime 基准（CLOCK_MONOTONIC）。 */
typedef media_status_t (*ms_hw_rel_at_fn)(AMediaCodec *, size_t, int64_t);

static ms_hw_rel_at_fn load_rel_at_fn(void)
{
    static ms_hw_rel_at_fn fn;
    static int tried;
    if (!tried) {
        void *h = dlopen("libmediandk.so", RTLD_NOW);
        tried = 1;
        if (h) {
            fn = (ms_hw_rel_at_fn)dlsym(h, "AMediaCodec_releaseOutputBufferAtTime");
        }
    }
    return fn;
}

/* NDK AMediaCodec_setOnFrameRenderedCallback 自 API 33；用 dlsym 避免 API 21 链接失败。
 * 官方说明：仅供参考、可能延迟/批处理，有的帧已渲染也可能无回调。不是上屏证明，不得写入 display。 */
typedef media_status_t (*ms_hw_set_fr_fn)(AMediaCodec *,
                                          void (*)(AMediaCodec *, void *, int64_t, int64_t),
                                          void *);

static void on_codec_frame_rendered(AMediaCodec *codec, void *userdata,
                                    int64_t media_time_us, int64_t system_nano)
{
    ms_hw_decoder *d = (ms_hw_decoder *)userdata;
    struct timespec ts;
    (void)codec;
    (void)media_time_us;
    (void)system_nano;
    if (d == NULL || d->stats.have_first_codec_rendered) {
        return;
    }
    {
        ms_hw_and *ap = (ms_hw_and *)d->plat;
        if (ap == NULL || ap->closing) {
            return;
        }
    }
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return;
    }
    d->stats.t_first_codec_rendered_us =
        (int64_t)ts.tv_sec * 1000000 + (int64_t)(ts.tv_nsec / 1000);
    d->stats.have_first_codec_rendered = 1;
    d->stats.codec_rendered_is_not_display = 1;
}

static ms_hw_set_fr_fn load_fr_fn(void)
{
    static ms_hw_set_fr_fn fn;
    static int looked;
    if (!looked) {
        void *h = dlopen("libmediandk.so", RTLD_NOW);
        looked = 1;
        if (h) {
            fn = (ms_hw_set_fr_fn)dlsym(h, "AMediaCodec_setOnFrameRenderedCallback");
        }
    }
    return fn;
}

static void hook_frame_rendered(ms_hw_decoder *d, AMediaCodec *codec)
{
    ms_hw_set_fr_fn fn = load_fr_fn();
    if (fn == NULL || codec == NULL || d == NULL) {
        return;
    }
    if (fn(codec, on_codec_frame_rendered, d) == AMEDIA_OK) {
        ms_hw_and *p = (ms_hw_and *)d->plat;
        if (p) {
            p->fr_hooked = 1;
        }
    }
}

static void unhook_frame_rendered(ms_hw_and *p)
{
    ms_hw_set_fr_fn fn;
    if (p == NULL || p->codec == NULL || !p->fr_hooked) {
        return;
    }
    fn = load_fr_fn();
    if (fn) {
        /* 先摘回调，避免 CodecLooper 在 stop/delete 后仍进入 userdata。 */
        (void)fn(p->codec, NULL, NULL);
    }
    p->fr_hooked = 0;
}

static void close_and(ms_hw_decoder *d)
{
    ms_hw_and *p = d ? (ms_hw_and *)d->plat : NULL;
    if (!p)
        return;
    p->closing = 1;
    if (p->codec) {
        unhook_frame_rendered(p);
        /* 不要单独 AMediaCodec_stop：stop 会拆掉 CodecLooper 内部互斥量，
         * 随后 delete/release 或仍在跑的 looper 再加锁会 FORTIFY 并可能 SIGABRT。
         * 已 start 时先 flush 归还 in-flight buffer，再只调用 delete
         *（内部 release + join looper）。未 start 同样只 delete 一次。 */
        p->held = 0;
        if (p->started) {
            (void)AMediaCodec_flush(p->codec);
        }
        p->started = 0;
        (void)AMediaCodec_delete(p->codec);
        p->codec = NULL;
    }
    if (p->window && p->acquired) {
        ANativeWindow_release(p->window);
        p->window = NULL;
    }
    if (p->out_copy) {
        ms_hw_free(d, p->out_copy);
        p->out_copy = NULL;
    }
    ms_hw_free(d, p);
    if (d)
        d->plat = NULL;
}

void ms_hw_android_close(ms_hw_decoder *d)
{
    close_and(d);
}


#ifndef MS_HW_LEGACY_JNI
#include <stddef.h>
#include <android/log.h>

/* API 24+ 写入 color / hdr-static-info。不做色调映射，不传 HDR10+ 动态元数据。
 * 模拟器不是真实 HDR 显示器。 */
static void apply_hdr_to_format(AMediaFormat *fmt, const ms_hw_config *cfg, int api,
                                const char *d_name, char *note, size_t note_cap)
{
    const ms_hdr_info *h;
    int std, trc, rng, prof, lvl;
    uint8_t sta[MS_HDR_STATIC_INFO_SIZE];
    size_t n = 0;
    int have_sta = 0;
    char hex[80];
    size_t i, ho = 0;
    if (fmt == NULL || cfg == NULL || api < 24) {
        return;
    }
    if (cfg->struct_size < offsetof(ms_hw_config, hdr) + sizeof(ms_hdr_info)) {
        return;
    }
    if (cfg->hdr.struct_size < offsetof(ms_hdr_info, kind) + sizeof(int)) {
        return;
    }
    h = &cfg->hdr;
    std = ms_hdr_android_color_standard(h);
    trc = ms_hdr_android_color_transfer(h);
    rng = ms_hdr_android_color_range(h);
    /* goldfish/ranchu 对 color-standard / hdr-static-info 等无完整 c2 映射，
     * 写入后 config CORRUPTED、只提交无输出。仍计算并打日志，真实设备照常写入。 */
    {
        int skip_keys = (d_name && (strstr(d_name, "goldfish") != NULL ||
                                    strstr(d_name, "ranchu") != NULL));
        if (skip_keys) {
            uint8_t sta[MS_HDR_STATIC_INFO_SIZE];
            size_t n = 0, i, ho = 0;
            char hex[80];
            hex[0] = 0;
            if (ms_hdr_pack_static_info(h, sta, sizeof(sta), &n) == MS_OK &&
                n == MS_HDR_STATIC_INFO_SIZE) {
                for (i = 0; i < n && ho + 3 < sizeof(hex); i++) {
                    ho += (size_t)snprintf(hex + ho, sizeof(hex) - ho, "%02x", sta[i]);
                }
                hex[ho] = 0;
            }
            __android_log_print(ANDROID_LOG_INFO, "MsHwPlayer",
                                "hdr configure api=%d kind=%s standard=%d transfer=%d range=%d "
                                "profile=%d level=%d static_hex=%s dv=%d "
                                "(keys computed, not applied on goldfish; no reliable c2 mapping; "
                                "not a display-effect claim)",
                                api, ms_hdr_kind_name(h->kind), std, trc, rng,
                                cfg->codec == MS_CODEC_HEVC ? ms_hdr_android_hevc_profile(h) : 0,
                                cfg->codec == MS_CODEC_HEVC ? ms_hdr_android_hevc_level(h) : 0,
                                hex[0] ? hex : "-", h->dolby_vision);
            if (note && note_cap) {
                snprintf(note, note_cap,
                         "hdr=%s keys computed, not applied on goldfish",
                         ms_hdr_kind_name(h->kind));
            }
            return;
        }
    }
    if (std) {
        AMediaFormat_setInt32(fmt, "color-standard", std);
    }
    if (trc) {
        AMediaFormat_setInt32(fmt, "color-transfer", trc);
    }
    if (rng) {
        AMediaFormat_setInt32(fmt, "color-range", rng);
    }
    if (cfg->codec == MS_CODEC_HEVC) {
        prof = ms_hdr_android_hevc_profile(h);
        lvl = ms_hdr_android_hevc_level(h);
        if (prof) {
            AMediaFormat_setInt32(fmt, "profile", prof);
        }
        if (lvl) {
            AMediaFormat_setInt32(fmt, "level", lvl);
        }
    }
    if (ms_hdr_pack_static_info(h, sta, sizeof(sta), &n) == MS_OK && n == MS_HDR_STATIC_INFO_SIZE) {
        /* goldfish 对 hdr-static-info 无 c2 映射，写入会导致 config CORRUPTED、有提交无输出。
         * 仍计算并打日志，真实设备照常写入。颜色键继续设置。 */
        int skip_static = 0;
        if (d_name && (strstr(d_name, "goldfish") != NULL || strstr(d_name, "ranchu") != NULL)) {
            skip_static = 1;
        }
        if (!skip_static) {
            AMediaFormat_setBuffer(fmt, "hdr-static-info", sta, n);
            have_sta = 1;
        } else {
            have_sta = -1; /* computed, not applied */
        }
        ho = 0;
        for (i = 0; i < n && ho + 3 < sizeof(hex); i++) {
            ho += (size_t)snprintf(hex + ho, sizeof(hex) - ho, "%02x", sta[i]);
        }
        hex[ho] = 0;
    } else {
        hex[0] = 0;
    }
    /* 明确不设置 tone-map / KEY_HDR10_PLUS_INFO。 */
    __android_log_print(ANDROID_LOG_INFO, "MsHwPlayer",
                        "hdr configure api=%d kind=%s standard=%d transfer=%d range=%d "
                        "static=%d profile=%d level=%d dv=%d hdr10plus_detected=%d "
                        "static_hex=%s%s (no tone map, no DV, hdr10+ not passed; not a display-effect claim)",
                        api, ms_hdr_kind_name(h->kind), std, trc, rng, have_sta,
                        cfg->codec == MS_CODEC_HEVC ? ms_hdr_android_hevc_profile(h) : 0,
                        cfg->codec == MS_CODEC_HEVC ? ms_hdr_android_hevc_level(h) : 0,
                        h->dolby_vision, h->have_hdr10_plus, (have_sta != 0) ? hex : "-",
                        have_sta < 0 ? " (computed, not applied on goldfish; no c2 mapping)" : "");
    if (note && note_cap) {
        snprintf(note, note_cap,
                 "hdr=%s std=%d trc=%d range=%d static=%d dv=%d (passthrough keys; not display effect)",
                 ms_hdr_kind_name(h->kind), std, trc, rng, have_sta, h->dolby_vision);
    }
}
#endif
ms_status ms_hw_android_configure(ms_hw_decoder *d, const ms_hw_config *cfg)
{
    int api;
    char name[128];
    char detail[192];
    ms_hw_kind kind = MS_HW_KIND_NONE;
    ms_status st;
    AMediaFormat *fmt;
    media_status_t ms;
    ms_hw_and *p;

    if (!d || !cfg)
        return MS_ERR_INVALID_ARG;
    api = device_api(d->opts.api_level);
    name[0] = 0;
    detail[0] = 0;
    d->width = cfg->width;
    d->height = cfg->height;
    d->sample_fmt = cfg->sample_fmt;
    if (cfg->time_base_num > 0)
        d->tb_num = cfg->time_base_num;
    if (cfg->time_base_den > 0)
        d->tb_den = cfg->time_base_den;
    d->surface = cfg->surface;
    d->video_mime[0] = 0;

    if (d->preferred_name[0]) {
        /* 测试覆盖：不走 MediaCodecList / JVM，直接 createByName。 */
        ms_hw_set_detail_buf(name, sizeof(name), d->preferred_name);
        kind = ms_hw_classify_codec_name(name, api, -1, -1);
        st = MS_OK;
        snprintf(detail, sizeof(detail),
                 "test force_decoder_name=%s kind=%s (software is not hardware)",
                 name, ms_hw_kind_name(kind));
    } else if (!ms_hw_jni_has_vm()) {
        ms_hw_set_detail(d, "MediaCodecList requires a JVM; not claiming hardware");
        d->caps.kind = MS_HW_KIND_NONE;
        d->caps.is_hardware = 0;
        d->caps.verified = 1;
        d->caps.api_level = api;
        d->caps.backend = "amediacodec";
        d->caps.codec = cfg->codec;
        return MS_ERR_UNSUPPORTED;
    } else if (cfg->codec == MS_CODEC_AVS_PLUS) {
        /* AVS+：按设备真实注册的解码器及其声明 MIME 挑选（海思 OMX.hisi.video.decoder.avs 等）。 */
        st = ms_hw_jni_pick_avs(api, d->opts.allow_software, name, sizeof(name),
                                d->video_mime, sizeof(d->video_mime), &kind, &api,
                                detail, sizeof(detail));
    } else {
        st = pick_android_video(cfg->codec, api, d->opts.allow_software, name, sizeof(name),
                                &kind, &api, detail, sizeof(detail));
    }
    d->caps.codec = cfg->codec;
    d->caps.kind = kind;
    d->caps.is_hardware = (kind == MS_HW_KIND_HARDWARE);
    d->caps.verified = 1;
    d->caps.api_level = api;
    d->caps.backend = "amediacodec";
    if (name[0]) {
        ms_hw_set_detail_buf(d->name_buf, sizeof(d->name_buf), name);
        d->caps.codec_name = d->name_buf;
    }
    if (st != MS_OK) {
        ms_hw_set_detail(d, detail[0] ? detail
                                      : no_hw_video_msg(cfg->codec));
        return MS_ERR_UNSUPPORTED;
    }
    if (kind != MS_HW_KIND_HARDWARE && !d->opts.allow_software &&
        d->preferred_name[0] == 0) {
        ms_hw_set_detail(d, "selected component is not hardware");
        d->caps.is_hardware = 0;
        return MS_ERR_UNSUPPORTED;
    }
    if (kind != MS_HW_KIND_HARDWARE)
        d->caps.is_hardware = 0;

    close_and(d);
    p = (ms_hw_and *)ms_hw_alloc(d, sizeof(*p));
    if (!p)
        return MS_ERR_NOMEM;
    memset(p, 0, sizeof(*p));
    p->window = (ANativeWindow *)cfg->surface;
    if (p->window) {
        ANativeWindow_acquire(p->window);
        p->acquired = 1;
    }
    p->codec = AMediaCodec_createCodecByName(name);
    if (!p->codec) {
        if (p->acquired)
            ANativeWindow_release(p->window);
        ms_hw_free(d, p);
        ms_hw_set_detail(d, "AMediaCodec_createCodecByName failed");
        return MS_ERR_UNSUPPORTED;
    }
    fmt = AMediaFormat_new();
    if (!fmt) {
        AMediaCodec_delete(p->codec);
        if (p->acquired)
            ANativeWindow_release(p->window);
        ms_hw_free(d, p);
        return MS_ERR_NOMEM;
    }
    {
        const char *mime = (d->video_mime[0] != 0) ? d->video_mime
                                                    : ms_hw_video_mime(cfg->codec);
        if (mime == NULL) {
            AMediaFormat_delete(fmt);
            AMediaCodec_delete(p->codec);
            if (p->acquired)
                ANativeWindow_release(p->window);
            ms_hw_free(d, p);
            ms_hw_set_detail(d, no_hw_video_msg(cfg->codec));
            return MS_ERR_UNSUPPORTED;
        }
        AMediaFormat_setString(fmt, "mime", mime);
    }
    if ((d->width <= 0 || d->height <= 0) && d->csd0_size > 0 &&
        cfg->codec == MS_CODEC_H264) {
        int w = 0, h = 0;
        /* 模拟器 goldfish / 部分机芯要求 width/height；CSD 仍由 csd-0/1 提供。 */
        if (ms_h264_csd_video_size(d->sample_fmt, 1, d->csd0, d->csd0_size,
                                   0, &w, &h) == MS_OK) {
            if (d->width <= 0 && w > 0) {
                d->width = w;
            }
            if (d->height <= 0 && h > 0) {
                d->height = h;
            }
        }
    }
    if ((d->width <= 0 || d->height <= 0) &&
        cfg->struct_size >= offsetof(ms_hw_config, hdr) + sizeof(ms_hdr_info) &&
        cfg->hdr.struct_size >= offsetof(ms_hdr_info, width) + sizeof(int)) {
        if (d->width <= 0 && cfg->hdr.width > 0)
            d->width = cfg->hdr.width;
        if (d->height <= 0 && cfg->hdr.height > 0)
            d->height = cfg->hdr.height;
    }
    if (d->width > 0)
        AMediaFormat_setInt32(fmt, "width", d->width);
    if (d->height > 0)
        AMediaFormat_setInt32(fmt, "height", d->height);
    p->nal_len_size = 0;
    if (cfg->sample_fmt == MS_SAMPLE_FMT_AVCC &&
        cfg->csd != NULL && cfg->csd_size >= 7 && cfg->csd[0] == 1) {
        unsigned ls = (unsigned)((cfg->csd[4] & 3u) + 1u);
        p->nal_len_size = (ls >= 1 && ls <= 4) ? ls : 4u;
    } else if (cfg->sample_fmt == MS_SAMPLE_FMT_HVCC &&
               cfg->csd != NULL && cfg->csd_size >= 23 && cfg->csd[0] == 1) {
        unsigned ls = (unsigned)((cfg->csd[21] & 3u) + 1u);
        p->nal_len_size = (ls >= 1 && ls <= 4) ? ls : 4u;
    }
    if (cfg->codec == MS_CODEC_H264) {
        set_h264_csd_buffer(fmt, "csd-0", d->csd0, d->csd0_size);
        set_h264_csd_buffer(fmt, "csd-1", d->csd1, d->csd1_size);
    } else {
        if (d->csd0_size)
            AMediaFormat_setBuffer(fmt, "csd-0", d->csd0, d->csd0_size);
        if (d->csd1_size)
            AMediaFormat_setBuffer(fmt, "csd-1", d->csd1, d->csd1_size);
    }
    {
        char hdr_note[192];
        hdr_note[0] = 0;
        apply_hdr_to_format(fmt, cfg, api, name, hdr_note, sizeof(hdr_note));
        if (hdr_note[0] && detail[0] == 0) {
            snprintf(detail, sizeof(detail), "%s", hdr_note);
        }
    }
    ms = AMediaCodec_configure(p->codec, fmt, p->window, NULL, 0);
    AMediaFormat_delete(fmt);
    if (ms != AMEDIA_OK) {
        AMediaCodec_delete(p->codec);
        if (p->acquired)
            ANativeWindow_release(p->window);
        ms_hw_free(d, p);
        ms_hw_set_detail(d, "AMediaCodec_configure failed");
        return MS_ERR_UNKNOWN;
    }
    d->plat = p;
    ms_hw_set_detail(d, detail[0] ? detail : "AMediaCodec configured for Surface output");
    d->st = MS_HW_ST_CONFIGURED;
    return MS_OK;
}

ms_status ms_hw_android_start(ms_hw_decoder *d)
{
    ms_hw_and *p = d ? (ms_hw_and *)d->plat : NULL;
    media_status_t ms;
    ms_hw_event ev;
    if (!p || !p->codec)
        return MS_ERR_STATE;
    ms = AMediaCodec_start(p->codec);
    if (ms != AMEDIA_OK) {
        ms_hw_set_detail(d, "AMediaCodec_start failed");
        d->st = MS_HW_ST_ERROR;
        return MS_ERR_UNKNOWN;
    }
    p->started = 1;
    if (p->window) {
        hook_frame_rendered(d, p->codec);
    }
    d->st = MS_HW_ST_STARTED;
    memset(&ev, 0, sizeof(ev));
    ev.struct_size = sizeof(ev);
    ev.type = MS_HW_EVENT_FORMAT_CHANGED;
    ev.width = d->width;
    ev.height = d->height;
    ev.detail = "codec started";
    (void)ms_hw_push_event(d, &ev);
    return MS_OK;
}

ms_status ms_hw_android_submit(ms_hw_decoder *d, unsigned slot_index)
{
    ms_hw_and *p = d ? (ms_hw_and *)d->plat : NULL;
    ms_hw_slot *s;
    ssize_t idx;
    size_t cap = 0;
    uint8_t *dst;
    uint32_t flags = 0;
    media_status_t ms;

    if (!p || !p->codec || slot_index >= d->in_cap)
        return MS_ERR_STATE;
    s = &d->in_slots[slot_index];
    if (!s->occupied)
        return MS_OK;
    idx = AMediaCodec_dequeueInputBuffer(p->codec, 0);
    if (idx < 0)
        return MS_AGAIN;
    dst = AMediaCodec_getInputBuffer(p->codec, (size_t)idx, &cap);
    if (!dst)
        return MS_ERR_OVERFLOW;
    {
        size_t qsize = s->size;
        /* flush 后重送 CSD 与 JNI 路径相同：不得把 avcC/起始码配置块
         * 当成长度前缀 AU 做 avcc_au_to_annexb（会 OVERFLOW，seek 后不出帧）。 */
        if (s->flags & MS_HW_FLAG_CODEC_CONFIG) {
            if (cap < s->size)
                return MS_ERR_OVERFLOW;
            if (s->size)
                memcpy(dst, s->data, s->size);
        } else if (p->nal_len_size > 0 && s->data != NULL && s->size > 0) {
            size_t conv = 0;
            if (ms_hw_avcc_au_to_annexb(s->data, s->size, p->nal_len_size, dst, cap,
                                  &conv) != 0) {
                return MS_ERR_OVERFLOW;
            }
            qsize = conv;
        } else {
            if (cap < s->size)
                return MS_ERR_OVERFLOW;
            if (s->size)
                memcpy(dst, s->data, s->size);
        }
        d->stats.platform_copy_ops++;
        d->stats.platform_copy_bytes += qsize;
        if (s->flags & MS_HW_FLAG_KEY)
            flags |= AMEDIACODEC_BUFFER_FLAG_KEY_FRAME;
        if (s->flags & MS_HW_FLAG_CODEC_CONFIG)
            flags |= AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG;
        if (s->flags & MS_HW_FLAG_EOS)
            flags |= AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM;
        ms = AMediaCodec_queueInputBuffer(p->codec, (size_t)idx, 0, qsize,
                                          (uint64_t)s->pts, flags);
    }
    if (ms != AMEDIA_OK) {
        ms_hw_set_detail(d, "queueInputBuffer failed");
        return MS_ERR_UNKNOWN;
    }
    ms_hw_consume_slot(d, slot_index);
    return MS_OK;
}

static ms_status feed_pending(ms_hw_decoder *d)
{
    unsigned i;
    ms_status last = MS_AGAIN;
    for (i = 0; i < d->in_cap; i++) {
        if (!d->in_slots[i].occupied)
            continue;
        last = ms_hw_android_submit(d, i);
        if (last != MS_OK && last != MS_AGAIN && last != MS_LIMIT)
            return last;
    }
    return last;
}

/* 对应 ExoPlayer 2.x（release-v2）MediaCodecVideoRenderer.renderOutputBuffer：
 * API < 21 无 releaseOutputBuffer(index, ns) 时，earlyUs 在 (11ms, 30ms] 睡眠后再 render。 */
static void maybe_sleep_until_release(int64_t release_ns)
{
    struct timespec ts;
    int64_t now_ns, early_us;
    if (release_ns <= 0) {
        return;
    }
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return;
    }
    now_ns = (int64_t)ts.tv_sec * 1000000000ll + (int64_t)ts.tv_nsec;
    early_us = (release_ns - now_ns) / 1000ll;
    if (early_us > 11000ll && early_us <= 30000ll) {
        unsigned long ms = (unsigned long)((early_us - 10000ll) / 1000ll);
        if (ms > 0) {
            usleep((useconds_t)(ms * 1000ul));
        }
    }
}


ms_status ms_hw_android_drive(ms_hw_decoder *d)
{
    ms_hw_and *p = d ? (ms_hw_and *)d->plat : NULL;
    AMediaCodecBufferInfo info;
    ssize_t idx;
    ms_status st;

    if (!p || !p->codec)
        return MS_ERR_STATE;
    st = feed_pending(d);
    if (st != MS_OK && st != MS_AGAIN && st != MS_LIMIT)
        return st;
    memset(&info, 0, sizeof(info));
    if (p->held) {
        /* 上轮判定“太早”的缓冲：不再 dequeue，直接重新判定。 */
        idx = p->held_idx;
        info = p->held_info;
        p->held = 0;
        goto have_output;
    }
    /* 非 0 超时：部分软件/goldfish 解码器在 timeout=0 的空转下长期不输出。10ms 仍非阻塞级。
     * FORMAT_CHANGED 后同一次 drive 再取一帧，避免测试路径多一轮才看见输出。 */
    {
        int attempt;
        int saw_fmt = 0;
        idx = AMEDIACODEC_INFO_TRY_AGAIN_LATER;
        for (attempt = 0; attempt < 4; attempt++) {
            idx = AMediaCodec_dequeueOutputBuffer(p->codec, &info, 10000);
            if (idx == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
                AMediaFormat *fmt = AMediaCodec_getOutputFormat(p->codec);
                int32_t w = d->width, h = d->height;
                int32_t stride = 0, slice = 0, cf = 0;
                ms_hw_event ev;
                saw_fmt = 1;
                if (fmt) {
                    AMediaFormat_getInt32(fmt, "width", &w);
                    AMediaFormat_getInt32(fmt, "height", &h);
                    (void)AMediaFormat_getInt32(fmt, "stride", &stride);
                    (void)AMediaFormat_getInt32(fmt, "slice-height", &slice);
                    (void)AMediaFormat_getInt32(fmt, "color-format", &cf);
                    AMediaFormat_delete(fmt);
                    d->width = w;
                    d->height = h;
                    if (stride > 0) {
                        p->out_stride = (int)stride;
                    }
                    if (slice > 0) {
                        p->out_slice = (int)slice;
                    }
                    if (cf != 0) {
                        p->out_color = (int)cf;
                    }
                    p->out_w = w;
                    p->out_h = h;
                }
                memset(&ev, 0, sizeof(ev));
                ev.struct_size = sizeof(ev);
                ev.type = MS_HW_EVENT_FORMAT_CHANGED;
                ev.width = d->width;
                ev.height = d->height;
                ev.detail = "output format changed";
                (void)ms_hw_push_event(d, &ev);
                continue;
            }
            if (idx == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) {
                continue;
            }
            break;
        }
        if (idx == AMEDIACODEC_INFO_TRY_AGAIN_LATER || idx < 0) {
            return saw_fmt ? MS_OK : MS_AGAIN;
        }
    }
have_output:
    /* Surface：render=true 且不回读像素。无窗口：拷一帧后 render=false，供测试取证。
     * 有出帧回调时按 Media3 VideoFrameReleaseControl 决定：立即/定时/丢弃/稍后。 */

    if (p->window && d->release_fn) {
        int64_t rel_ns = 0;
        int is_eos = (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) != 0;
        ms_hw_rel_at_fn at_fn = load_rel_at_fn();
        ms_hw_release_action act = d->release_fn(d->release_fn_ctx,
                                                 (int64_t)info.presentationTimeUs,
                                                 is_eos, at_fn != NULL, &rel_ns);
        if (act == MS_HW_REL_LATER) {
            p->held = 1;
            p->held_idx = idx;
            p->held_info = info;
            return MS_AGAIN;
        }
        if (act == MS_HW_REL_DROP) {
            if (AMediaCodec_releaseOutputBuffer(p->codec, (size_t)idx, 0) != AMEDIA_OK) {
                ms_hw_set_detail(d, "releaseOutputBuffer(drop) failed");
                return MS_ERR_UNKNOWN;
            }
            goto after_release; /* 丢帧不推 OUTPUT_RENDERED */
        }
        if (act == MS_HW_REL_AT && at_fn != NULL) {
            if (at_fn(p->codec, (size_t)idx, rel_ns) != AMEDIA_OK) {
                ms_hw_set_detail(d, "releaseOutputBufferAtTime failed");
                return MS_ERR_UNKNOWN;
            }
        } else {
            maybe_sleep_until_release(rel_ns);
            if (AMediaCodec_releaseOutputBuffer(p->codec, (size_t)idx, 1) != AMEDIA_OK) {
                ms_hw_set_detail(d, "releaseOutputBuffer failed");
                return MS_ERR_UNKNOWN;
            }
        }
    } else if (!p->window) {
        size_t cap = 0;
        uint8_t *ob = AMediaCodec_getOutputBuffer(p->codec, (size_t)idx, &cap);
        size_t off = (size_t)info.offset;
        size_t sz = (size_t)info.size;
        if (ob != NULL && sz > 0 && off < cap && sz <= cap - off) {
            if (p->out_copy_cap < sz) {
                uint8_t *nb = (uint8_t *)ms_hw_alloc(d, sz);
                if (nb) {
                    if (p->out_copy) {
                        ms_hw_free(d, p->out_copy);
                    }
                    p->out_copy = nb;
                    p->out_copy_cap = sz;
                }
            }
            if (p->out_copy && p->out_copy_cap >= sz) {
                memcpy(p->out_copy, ob + off, sz);
                p->out_copy_size = sz;
                if (d->width > 0) {
                    p->out_w = d->width;
                }
                if (d->height > 0) {
                    p->out_h = d->height;
                }
            }
        }
        if (AMediaCodec_releaseOutputBuffer(p->codec, (size_t)idx, 0) != AMEDIA_OK) {
            ms_hw_set_detail(d, "releaseOutputBuffer failed");
            return MS_ERR_UNKNOWN;
        }
    } else if (AMediaCodec_releaseOutputBuffer(p->codec, (size_t)idx, 1) != AMEDIA_OK) {
        ms_hw_set_detail(d, "releaseOutputBuffer failed");
        return MS_ERR_UNKNOWN;
    }
    {
        ms_hw_event ev;
        memset(&ev, 0, sizeof(ev));
        ev.struct_size = sizeof(ev);
        ev.type = MS_HW_EVENT_OUTPUT_RENDERED;
        ev.pts = (int64_t)info.presentationTimeUs;
        ev.has_pts = 1;
        ev.width = d->width;
        ev.height = d->height;
        d->stats.rendered_frames++;
        st = ms_hw_push_event(d, &ev);
        if (st != MS_OK)
            return st;
    }
after_release:
    if (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) {
        ms_hw_event ev;
        memset(&ev, 0, sizeof(ev));
        ev.struct_size = sizeof(ev);
        ev.type = MS_HW_EVENT_EOS;
        (void)ms_hw_push_event(d, &ev);
        d->st = MS_HW_ST_EOS;
    }
    return MS_OK;
}

ms_status ms_hw_android_flush(ms_hw_decoder *d)
{
    ms_hw_and *p = d ? (ms_hw_and *)d->plat : NULL;
    if (!p || !p->codec)
        return MS_OK;
    p->held = 0; /* flush 后所有输出 index 失效 */
    if (AMediaCodec_flush(p->codec) != AMEDIA_OK) {
        ms_hw_set_detail(d, "AMediaCodec_flush failed");
        return MS_ERR_UNKNOWN;
    }
    return MS_OK;
}


ms_status ms_hw_android_copy_last_output(ms_hw_decoder *d,
                                         int *width, int *height,
                                         int *stride, int *slice_height,
                                         int *color_format,
                                         uint8_t *dst, size_t dst_cap,
                                         size_t *out_size)
{
    ms_hw_and *p = d ? (ms_hw_and *)d->plat : NULL;
    if (d == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    if (p == NULL || p->window || p->out_copy == NULL || p->out_copy_size == 0) {
        return MS_AGAIN;
    }
    if (width) {
        *width = p->out_w;
    }
    if (height) {
        *height = p->out_h;
    }
    if (stride) {
        *stride = p->out_stride;
    }
    if (slice_height) {
        *slice_height = p->out_slice;
    }
    if (color_format) {
        *color_format = p->out_color;
    }
    if (out_size) {
        *out_size = p->out_copy_size;
    }
    if (dst == NULL) {
        return MS_OK;
    }
    if (dst_cap < p->out_copy_size) {
        return MS_ERR_OVERFLOW;
    }
    memcpy(dst, p->out_copy, p->out_copy_size);
    return MS_OK;
}

ms_status ms_hw_decoder_copy_last_output(ms_hw_decoder *d,
                                         int *width, int *height,
                                         int *stride, int *slice_height,
                                         int *color_format,
                                         uint8_t *dst, size_t dst_cap,
                                         size_t *out_size)
{
    return ms_hw_android_copy_last_output(d, width, height, stride, slice_height,
                                          color_format, dst, dst_cap, out_size);
}

#else /* MS_HW_LEGACY_JNI：API 16–20 全部走 JNI MediaCodec */

ms_status ms_hw_android_query(ms_codec codec, int api_level, int allow_sw,
                              ms_hw_caps *out, char *name_buf, size_t name_cap,
                              char *detail_buf, size_t detail_cap)
{
    int api = api_level > 0 ? api_level : device_api(0);
    char name[128], detail[192];
    ms_hw_kind kind = MS_HW_KIND_NONE;
    ms_status st;

    if (out)
        memset(out, 0, sizeof(*out)), out->struct_size = sizeof(*out);
    if (codec != MS_CODEC_UNKNOWN && !ms_video_hw_path_ready(codec)) {
        if (out) {
            out->codec = codec;
            out->verified = 1;
            out->api_level = api;
            out->backend = "jni-mediacodec";
            out->detail = "configure/submit for this codec is not implemented in this milestone";
        }
        return MS_OK;
    }
    if (!ms_hw_jni_has_vm()) {
        if (out) {
            out->codec = (codec == MS_CODEC_HEVC) ? MS_CODEC_HEVC : MS_CODEC_H264;
            out->verified = 1;
            out->api_level = api;
            out->backend = "jni-mediacodec";
            if (detail_buf) {
                ms_hw_set_detail_buf(detail_buf, detail_cap,
                                     "MediaCodecList requires a JVM; not claiming hardware");
                out->detail = detail_buf;
            }
        }
        return MS_OK;
    }
    name[0] = 0;
    detail[0] = 0;
    {
        ms_codec q = (codec == MS_CODEC_UNKNOWN) ? MS_CODEC_H264 : codec;
        st = pick_android_video(q, api, allow_sw, name, sizeof(name), &kind, &api,
                                detail, sizeof(detail));
        if (out) {
            out->codec = q;
            out->kind = kind;
            out->is_hardware = (kind == MS_HW_KIND_HARDWARE);
            out->verified = 1;
            out->api_level = api;
            out->backend = "jni-mediacodec";
            if (name[0] && name_buf) {
                ms_hw_set_detail_buf(name_buf, name_cap, name);
                out->codec_name = name_buf;
            }
            if (detail_buf) {
                ms_hw_set_detail_buf(detail_buf, detail_cap,
                                     detail[0] ? detail : no_hw_video_msg(q));
                out->detail = detail_buf;
            }
        }
    }
    (void)st;
    return MS_OK;
}

ms_status ms_hw_android_configure(ms_hw_decoder *d, const ms_hw_config *cfg)
{
    int api;
    char name[128], detail[192];
    ms_hw_kind kind = MS_HW_KIND_NONE;
    ms_status st;

    api = d->opts.api_level > 0 ? d->opts.api_level
                                : device_api(0);
    name[0] = 0;
    detail[0] = 0;
    d->width = cfg->width;
    d->height = cfg->height;
    d->surface = cfg->surface;
    d->sample_fmt = cfg->sample_fmt;
    d->video_mime[0] = 0;
    /* 与 NDK 路径对齐：测试用 preferred_name / force_decoder 可点名组件，
     * 即便是 OMX.google 软解也不在这里拒绝。默认 allow_software 仍为 0，
     * 未点名时 API 18 AVD 仅有 OMX.google.h264.decoder 将如实 UNSUPPORTED。
     * 软解不是硬件。 */
    if (d->preferred_name[0]) {
        ms_hw_set_detail_buf(name, sizeof(name), d->preferred_name);
        kind = ms_hw_classify_codec_name(name, api, -1, -1);
        st = MS_OK;
        snprintf(detail, sizeof(detail),
                 "test force_decoder_name=%s kind=%s (software is not hardware)",
                 name, ms_hw_kind_name(kind));
    } else if (cfg->codec == MS_CODEC_AVS_PLUS) {
        st = ms_hw_jni_pick_avs(api, d->opts.allow_software, name, sizeof(name),
                                d->video_mime, sizeof(d->video_mime), &kind, &api,
                                detail, sizeof(detail));
    } else {
        st = pick_android_video(cfg->codec, api, d->opts.allow_software, name, sizeof(name),
                                &kind, &api, detail, sizeof(detail));
    }
    d->caps.codec = cfg->codec;
    d->caps.kind = kind;
    d->caps.is_hardware = (kind == MS_HW_KIND_HARDWARE);
    d->caps.verified = 1;
    d->caps.api_level = api;
    d->caps.backend = "jni-mediacodec";
    if (name[0]) {
        ms_hw_set_detail_buf(d->name_buf, sizeof(d->name_buf), name);
        d->caps.codec_name = d->name_buf;
    }
    if (st != MS_OK ||
        (kind != MS_HW_KIND_HARDWARE && !d->opts.allow_software &&
         d->preferred_name[0] == 0)) {
        ms_hw_set_detail(d, detail[0] ? detail
                                      : no_hw_video_msg(cfg->codec));
        d->caps.is_hardware = 0;
        __android_log_print(ANDROID_LOG_INFO, "MsHwPlayer",
                            "jni configure refused: %s",
                            d->caps.detail ? d->caps.detail : "unsupported");
        return MS_ERR_UNSUPPORTED;
    }
    if (kind != MS_HW_KIND_HARDWARE)
        d->caps.is_hardware = 0;
    st = ms_hw_jni_codec_open(d, name, cfg);
    if (st != MS_OK) {
        ms_hw_set_detail(d, "MediaCodec.createByCodecName/configure failed");
        return st;
    }
    ms_hw_set_detail(d, "JNI MediaCodec configured (API 16-20 path)");
    d->st = MS_HW_ST_CONFIGURED;
    return MS_OK;
}

ms_status ms_hw_android_start(ms_hw_decoder *d)
{
    ms_status st = ms_hw_jni_codec_start(d);
    if (st != MS_OK)
        return st;
    d->st = MS_HW_ST_STARTED;
    {
        ms_hw_event ev;
        memset(&ev, 0, sizeof(ev));
        ev.struct_size = sizeof(ev);
        ev.type = MS_HW_EVENT_FORMAT_CHANGED;
        ev.width = d->width;
        ev.height = d->height;
        (void)ms_hw_push_event(d, &ev);
    }
    return MS_OK;
}

ms_status ms_hw_android_submit(ms_hw_decoder *d, unsigned slot_index)
{
    return ms_hw_jni_codec_feed(d, slot_index);
}

ms_status ms_hw_android_drive(ms_hw_decoder *d)
{
    return ms_hw_jni_codec_drive(d);
}

ms_status ms_hw_android_flush(ms_hw_decoder *d)
{
    return ms_hw_jni_codec_flush(d);
}

void ms_hw_android_close(ms_hw_decoder *d)
{
    ms_hw_jni_codec_close(d);
}

#endif /* MS_HW_LEGACY_JNI */

#else
typedef int ms_hw_android_unused;
#endif
