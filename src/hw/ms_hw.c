/*
 * 可选硬解会话：公共状态机、有界队列、引用持有与计量。
 * 宿主默认走 Linux VAAPI 探测（无设备或未实现提交时拒绝，is_hardware=0）；mock_mode 仍走 mock。Android 走 AMediaCodec 或 JNI 薄桥。
 * by: Long <mycjzb@gmail.com>
 */
#include "ms_hw_internal.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static void *std_alloc(void *ctx, size_t n)
{
    (void)ctx;
    return malloc(n);
}

static void std_free(void *ctx, void *p)
{
    (void)ctx;
    free(p);
}

void *ms_hw_alloc(ms_hw_decoder *d, size_t n)
{
    if (d == NULL || d->alloc == NULL || d->alloc->alloc_fn == NULL) {
        return malloc(n);
    }
    return d->alloc->alloc_fn(d->alloc->ctx, n);
}

void ms_hw_free(ms_hw_decoder *d, void *p)
{
    if (p == NULL) {
        return;
    }
    if (d == NULL || d->alloc == NULL || d->alloc->free_fn == NULL) {
        free(p);
        return;
    }
    d->alloc->free_fn(d->alloc->ctx, p);
}

void ms_hw_log(ms_hw_decoder *d, int level, const char *msg)
{
    if (d != NULL && d->log_fn != NULL && msg != NULL) {
        d->log_fn(d->log_ctx, level, msg);
    }
}

void ms_hw_set_detail_buf(char *buf, size_t cap, const char *s)
{
    size_t n;
    if (buf == NULL || cap == 0) {
        return;
    }
    if (s == NULL) {
        buf[0] = 0;
        return;
    }
    n = strlen(s);
    if (n >= cap) {
        n = cap - 1u;
    }
    memcpy(buf, s, n);
    buf[n] = 0;
}

void ms_hw_consume_slot(ms_hw_decoder *d, unsigned i)
{
    size_t n;
    if (d == NULL || d->in_slots == NULL || i >= d->in_cap ||
        !d->in_slots[i].occupied) {
        return;
    }
    n = d->in_slots[i].size;
    ms_hw_release_slot(&d->in_slots[i]);
    if (d->in_n > 0) {
        d->in_n--;
    }
    if (d->in_bytes >= n) {
        d->in_bytes -= n;
    } else {
        d->in_bytes = 0;
    }
    d->stats.pending_in = d->in_n;
    d->stats.pending_in_bytes = d->in_bytes;
}

/* 对应 MediaCodecRenderer.skipSource：只跳过尚未 queue 到编解码器的输入。 */
int ms_hw_decoder_skip_source(ms_hw_decoder *d, int64_t positionUs)
{
    unsigned i;
    int n = 0;
    if (d == NULL || d->in_slots == NULL) {
        return 0;
    }
    for (i = 0; i < d->in_cap; i++) {
        if (d->in_slots[i].occupied && d->in_slots[i].pts < positionUs) {
            ms_hw_consume_slot(d, i);
            n++;
        }
    }
    return n;
}

void ms_hw_set_detail(ms_hw_decoder *d, const char *s)
{
    size_t n;
    if (d == NULL) {
        return;
    }
    if (s == NULL) {
        d->detail_buf[0] = 0;
        d->caps.detail = d->detail_buf;
        return;
    }
    n = strlen(s);
    if (n >= sizeof(d->detail_buf)) {
        n = sizeof(d->detail_buf) - 1u;
    }
    memcpy(d->detail_buf, s, n);
    d->detail_buf[n] = 0;
    d->caps.detail = d->detail_buf;
}

void ms_hw_caps_clear(ms_hw_caps *c)
{
    const char *none = "none";
    if (c == NULL) {
        return;
    }
    c->codec = MS_CODEC_UNKNOWN;
    c->kind = MS_HW_KIND_NONE;
    c->is_hardware = 0;
    c->verified = 0;
    c->api_level = 0;
    c->backend = none;
    c->codec_name = NULL;
    c->detail = "no hardware decoder";
}

void ms_hw_release_slot(ms_hw_slot *s)
{
    if (s == NULL || !s->occupied) {
        return;
    }
    if (s->release_fn != NULL) {
        s->release_fn(s->release_ctx, s->data, s->size);
    }
    s->data = NULL;
    s->size = 0;
    s->release_fn = NULL;
    s->release_ctx = NULL;
    s->occupied = 0;
}

void ms_hw_drop_all_input(ms_hw_decoder *d)
{
    unsigned i;
    if (d == NULL || d->in_slots == NULL) {
        return;
    }
    for (i = 0; i < d->in_cap; i++) {
        ms_hw_release_slot(&d->in_slots[i]);
    }
    d->in_n = 0;
    d->in_bytes = 0;
    d->stats.pending_in = 0;
    d->stats.pending_in_bytes = 0;
}

void ms_hw_drop_events(ms_hw_decoder *d)
{
    if (d == NULL) {
        return;
    }
    d->ev_n = 0;
    d->ev_r = 0;
    d->stats.events_queued = 0;
}

void ms_hw_note_pin(ms_hw_decoder *d)
{
    if (d != NULL && d->in_bytes > d->stats.pin_peak_bytes) {
        d->stats.pin_peak_bytes = d->in_bytes;
    }
}

ms_status ms_hw_push_event(ms_hw_decoder *d, const ms_hw_event *ev)
{
    unsigned w;
    if (d == NULL || ev == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    if (d->ev_n >= d->ev_cap) {
        return MS_LIMIT;
    }
    w = (d->ev_r + d->ev_n) % d->ev_cap;
    d->evq[w] = *ev;
    d->evq[w].struct_size = sizeof(ms_hw_event);
    d->ev_n++;
    d->stats.events_queued = d->ev_n;
    return MS_OK;
}

void ms_hw_limits_default(ms_hw_limits *out)
{
    if (out == NULL) {
        return;
    }
    out->struct_size = sizeof(*out);
    out->max_pending_in = MS_HW_DEFAULT_PENDING;
    out->max_in_bytes = MS_HW_DEFAULT_IN_BYTES;
    out->max_events = MS_HW_DEFAULT_EVENTS;
}

static int apply_limits(const ms_hw_limits *in, ms_hw_limits *out)
{
    ms_hw_limits d;
    ms_hw_limits_default(&d);
    if (in != NULL) {
        if (in->struct_size < sizeof(size_t)) {
            return -1;
        }
        if (in->max_pending_in != 0) {
            d.max_pending_in = in->max_pending_in;
        }
        if (in->max_in_bytes != 0) {
            d.max_in_bytes = in->max_in_bytes;
        }
        if (in->max_events != 0) {
            d.max_events = in->max_events;
        }
    }
    if (d.max_pending_in > MS_HW_ABS_PENDING ||
        d.max_in_bytes > MS_HW_ABS_IN_BYTES ||
        d.max_events > MS_HW_ABS_EVENTS ||
        d.max_pending_in == 0 || d.max_events == 0 || d.max_in_bytes == 0) {
        return -1;
    }
    *out = d;
    out->struct_size = sizeof(*out);
    return 0;
}

static void copy_caps(ms_hw_caps *dst, const ms_hw_caps *src)
{
    size_t sz;
    if (dst == NULL || src == NULL) {
        return;
    }
    sz = dst->struct_size;
    if (sz == 0 || sz > sizeof(*dst)) {
        sz = sizeof(*dst);
    }
    memcpy(dst, src, sz);
    dst->struct_size = sz;
}

ms_status ms_hw_decoder_query(ms_codec codec, ms_hw_caps *out)
{
    if (out == NULL || out->struct_size < sizeof(size_t)) {
        return MS_ERR_INVALID_ARG;
    }
    if (out->struct_size > sizeof(*out)) {
        out->struct_size = sizeof(*out);
    }
#ifdef __ANDROID__
    {
        ms_hw_caps tmp;
        char name[128];
        char detail[192];
        ms_status st;
        memset(&tmp, 0, sizeof(tmp));
        tmp.struct_size = sizeof(tmp);
        name[0] = 0;
        detail[0] = 0;
        st = ms_hw_android_query(codec, 0, 0, &tmp, name, sizeof(name),
                                 detail, sizeof(detail));
        if (st != MS_OK && st != MS_ERR_UNSUPPORTED) {
            return st;
        }
        copy_caps(out, &tmp);
        return MS_OK;
    }
#else
    return ms_hw_vaapi_query(codec, out);
#endif
}

ms_status ms_hw_decoder_create(const ms_allocator *alloc,
                               const ms_hw_limits *limits,
                               const ms_hw_create_opts *opts,
                               ms_hw_decoder **out)
{
    ms_hw_decoder *d;
    ms_hw_limits lim;
    if (out == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    *out = NULL;
    if (apply_limits(limits, &lim) != 0) {
        return MS_ERR_INVALID_ARG;
    }
    d = calloc(1, sizeof(*d));
    if (d == NULL) {
        return MS_ERR_NOMEM;
    }
    if (alloc != NULL && alloc->alloc_fn != NULL && alloc->free_fn != NULL) {
        d->alloc = alloc;
    } else {
        d->owned_alloc.alloc_fn = std_alloc;
        d->owned_alloc.free_fn = std_free;
        d->owned_alloc.ctx = NULL;
        d->alloc = &d->owned_alloc;
    }
    d->limits = lim;
    if (opts != NULL) {
        d->opts = *opts;
        d->opts.struct_size = sizeof(d->opts);
    } else {
        memset(&d->opts, 0, sizeof(d->opts));
        d->opts.struct_size = sizeof(d->opts);
    }
    d->in_cap = lim.max_pending_in;
    d->ev_cap = lim.max_events;
    d->in_slots = ms_hw_alloc(d, sizeof(ms_hw_slot) * d->in_cap);
    d->evq = ms_hw_alloc(d, sizeof(ms_hw_event) * d->ev_cap);
    if (d->in_slots == NULL || d->evq == NULL) {
        ms_hw_free(d, d->in_slots);
        ms_hw_free(d, d->evq);
        free(d);
        return MS_ERR_NOMEM;
    }
    memset(d->in_slots, 0, sizeof(ms_hw_slot) * d->in_cap);
    memset(d->evq, 0, sizeof(ms_hw_event) * d->ev_cap);
    d->st = MS_HW_ST_IDLE;
    d->tb_num = 1;
    d->tb_den = 1000000;
    d->caps.struct_size = sizeof(d->caps);
    d->caps.backend = "mock";
    d->caps.verified = 1;
    d->caps.kind = MS_HW_KIND_NONE;
    d->caps.detail = d->detail_buf;
    ms_hw_set_detail(d, "created, not configured");
#ifndef __ANDROID__
    d->caps.backend = "mock";
#endif
    d->stats.struct_size = sizeof(d->stats);
    d->stats.t_first_codec_rendered_us = -1;
    d->stats.have_first_codec_rendered = 0;
    d->stats.codec_rendered_is_not_display = 1;
    *out = d;
    return MS_OK;
}

void ms_hw_decoder_destroy(ms_hw_decoder *d)
{
    if (d == NULL) {
        return;
    }
#ifdef __ANDROID__
    ms_hw_android_close(d);
#endif
    ms_hw_drop_all_input(d);
    ms_hw_drop_events(d);
    ms_hw_free(d, d->in_slots);
    ms_hw_free(d, d->evq);
    d->st = MS_HW_ST_DEAD;
    free(d);
}

void ms_hw_decoder_set_log(ms_hw_decoder *d, ms_hw_log_fn fn, void *ctx)
{
    if (d == NULL) {
        return;
    }
    d->log_fn = fn;
    d->log_ctx = ctx;
}

ms_status ms_hw_decoder_configure(ms_hw_decoder *d, const ms_hw_config *cfg)
{
    if (d == NULL || cfg == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    if (d->st != MS_HW_ST_IDLE && d->st != MS_HW_ST_CONFIGURED) {
        return MS_ERR_STATE;
    }
    if (!ms_video_hw_path_ready(cfg->codec)) {
        ms_hw_set_detail(d, "configure/submit for this codec is not implemented");
        return MS_ERR_UNSUPPORTED;
    }
    if (cfg->codec == MS_CODEC_H264 &&
        cfg->sample_fmt != MS_SAMPLE_FMT_AVCC &&
        cfg->sample_fmt != MS_SAMPLE_FMT_ANNEXB) {
        return MS_ERR_INVALID_ARG;
    }
    if (cfg->codec == MS_CODEC_HEVC &&
        cfg->sample_fmt != MS_SAMPLE_FMT_HVCC &&
        cfg->sample_fmt != MS_SAMPLE_FMT_ANNEXB) {
        return MS_ERR_INVALID_ARG;
    }
    if (cfg->codec == MS_CODEC_MPEG2_VIDEO && cfg->sample_fmt != MS_SAMPLE_FMT_MPV) {
        return MS_ERR_INVALID_ARG;
    }
    if (cfg->codec == MS_CODEC_MPEG4_PART2 && cfg->sample_fmt != MS_SAMPLE_FMT_MP4V) {
        return MS_ERR_INVALID_ARG;
    }
    if (cfg->codec == MS_CODEC_H263 && cfg->sample_fmt != MS_SAMPLE_FMT_H263) {
        return MS_ERR_INVALID_ARG;
    }
    if (cfg->codec == MS_CODEC_AV1 && cfg->sample_fmt != MS_SAMPLE_FMT_AV1) {
        return MS_ERR_INVALID_ARG;
    }
    if (cfg->codec == MS_CODEC_AVS_PLUS && cfg->sample_fmt != MS_SAMPLE_FMT_CAVS) {
        return MS_ERR_INVALID_ARG;
    }
    if (cfg->codec == MS_CODEC_H263) {
        if (cfg->width <= 0 || cfg->height <= 0) {
            return MS_ERR_INVALID_ARG;
        }
        /* 合法无 CSD：不要求 csd 指针。若调用方传入非空缓冲，只接受空长度。 */
        if (cfg->csd_size != 0) {
            return MS_ERR_INVALID_ARG;
        }
    } else if (cfg->codec == MS_CODEC_AV1) {
        if (cfg->width <= 0 || cfg->height <= 0) {
            return MS_ERR_INVALID_ARG;
        }
        /* 允许合法 av1C，也允许合法无 CSD（帧内 sequence header）。不得在此捏造 av1C。 */
        if (cfg->csd_size != 0 && cfg->csd == NULL) {
            return MS_ERR_INVALID_ARG;
        }
    } else if (cfg->csd == NULL || cfg->csd_size == 0) {
        return MS_ERR_INVALID_ARG;
    }
    if (cfg->surface == NULL) {
#ifndef __ANDROID__
        ms_hw_set_detail(d, "video output requires a Surface / mock token");
        return MS_ERR_INVALID_ARG;
#else
        /* Android 允许无 Surface 的 ByteBuffer 测试路径；默认播放器仍绑定 Surface。 */
#endif
    }
    if (ms_hw_csd_split_for_codec(cfg->codec, cfg->sample_fmt, cfg->csd, cfg->csd_size,
                                  d->csd0, &d->csd0_size, sizeof(d->csd0),
                                  d->csd1, &d->csd1_size, sizeof(d->csd1)) != MS_OK) {
        ms_hw_set_detail(d, "CSD missing parameter sets or overflow");
        return MS_ERR_MALFORMED;
    }
#ifdef __ANDROID__
    return ms_hw_android_configure(d, cfg);
#else
    if (d->opts.mock_mode != MS_HW_MOCK_DEFAULT) {
        return ms_hw_mock_configure(d, cfg);
    }
    return ms_hw_vaapi_configure(d, cfg);
#endif
}

ms_status ms_hw_decoder_start(ms_hw_decoder *d)
{
    if (d == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    if (d->st != MS_HW_ST_CONFIGURED) {
        return MS_ERR_STATE;
    }
#ifdef __ANDROID__
    return ms_hw_android_start(d);
#else
    return ms_hw_mock_start(d);
#endif
}

static int find_free_slot(ms_hw_decoder *d, unsigned *idx)
{
    unsigned i;
    for (i = 0; i < d->in_cap; i++) {
        if (!d->in_slots[i].occupied) {
            *idx = i;
            return 1;
        }
    }
    return 0;
}

ms_status ms_hw_decoder_submit(ms_hw_decoder *d, const ms_hw_buf *buf,
                               int64_t pts, unsigned flags)
{
    unsigned idx = 0;
    if (d == NULL || buf == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    if (d->st != MS_HW_ST_STARTED && d->st != MS_HW_ST_DRAINING) {
        return MS_ERR_STATE;
    }
    if (d->st == MS_HW_ST_DRAINING && (flags & MS_HW_FLAG_EOS) == 0) {
        return MS_ERR_STATE;
    }
    if (buf->data == NULL || buf->size == 0) {
        if ((flags & MS_HW_FLAG_EOS) == 0) {
            return MS_ERR_INVALID_ARG;
        }
    }
    if (buf->size > d->limits.max_in_bytes) {
        return MS_ERR_OVERFLOW;
    }
    if (d->in_n >= d->in_cap || d->in_bytes + buf->size > d->limits.max_in_bytes) {
        return MS_LIMIT;
    }
    if (!find_free_slot(d, &idx)) {
        return MS_LIMIT;
    }
    d->in_slots[idx].data = (uint8_t *)(uintptr_t)buf->data;
    d->in_slots[idx].size = buf->size;
    d->in_slots[idx].release_fn = buf->release_fn;
    d->in_slots[idx].release_ctx = buf->release_ctx;
    d->in_slots[idx].pts = pts;
    d->in_slots[idx].flags = flags;
    d->in_slots[idx].occupied = 1;
    d->in_n++;
    d->in_bytes += buf->size;
    d->stats.submit_ops++;
    d->stats.submit_bytes += buf->size;
    d->stats.ref_hold_ops++;
    d->stats.pending_in = d->in_n;
    d->stats.pending_in_bytes = d->in_bytes;
    ms_hw_note_pin(d);
    if (flags & MS_HW_FLAG_EOS) {
        d->st = MS_HW_ST_DRAINING;
        d->eos_sent = 1;
    }
#ifdef __ANDROID__
    {
        ms_status st = ms_hw_android_submit(d, idx);
        if (st != MS_OK && st != MS_AGAIN && st != MS_LIMIT) {
            return st;
        }
    }
#endif
    return MS_OK;
}

ms_status ms_hw_decoder_signal_eos(ms_hw_decoder *d)
{
    ms_hw_buf empty;
    if (d == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    if (d->st == MS_HW_ST_EOS || d->eos_sent) {
        return MS_OK;
    }
    if (d->st != MS_HW_ST_STARTED && d->st != MS_HW_ST_DRAINING) {
        return MS_ERR_STATE;
    }
    memset(&empty, 0, sizeof(empty));
    /* EOS 可用空槽标记；若队列满则 LIMIT。 */
    empty.data = (const uint8_t *)"";
    empty.size = 0;
    return ms_hw_decoder_submit(d, &empty, 0, MS_HW_FLAG_EOS);
}

ms_status ms_hw_decoder_flush(ms_hw_decoder *d)
{
    if (d == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    if (d->st != MS_HW_ST_STARTED && d->st != MS_HW_ST_DRAINING &&
        d->st != MS_HW_ST_EOS && d->st != MS_HW_ST_CONFIGURED) {
        return MS_ERR_STATE;
    }
#ifdef __ANDROID__
    {
        ms_status st = ms_hw_android_flush(d);
        if (st != MS_OK) {
            return st;
        }
    }
#else
    {
        ms_status st = ms_hw_mock_flush(d);
        if (st != MS_OK) {
            return st;
        }
    }
#endif
    ms_hw_drop_all_input(d);
    ms_hw_drop_events(d);
    d->eos_sent = 0;
    if (d->st != MS_HW_ST_CONFIGURED) {
        d->st = MS_HW_ST_STARTED;
    }
    return MS_OK;
}

ms_status ms_hw_decoder_drive(ms_hw_decoder *d)
{
    if (d == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    if (d->st == MS_HW_ST_EOS) {
        return MS_OK; /* 幂等：EOS 后再 drive 不是错误 */
    }
    if (d->st == MS_HW_ST_IDLE || d->st == MS_HW_ST_CONFIGURED) {
        /* 已创建但未 start：拉流循环里提前 drive 是合法空操作，不得记 STATE。 */
        return MS_OK;
    }
    if (d->st != MS_HW_ST_STARTED && d->st != MS_HW_ST_DRAINING) {
        return MS_ERR_STATE;
    }
#ifdef __ANDROID__
    return ms_hw_android_drive(d);
#else
    return ms_hw_mock_drive(d);
#endif
}

ms_status ms_hw_decoder_pull_event(ms_hw_decoder *d, ms_hw_event *out)
{
    ms_hw_event ev;
    size_t n;
    if (d == NULL || out == NULL || out->struct_size < sizeof(size_t)) {
        return MS_ERR_INVALID_ARG;
    }
    if (d->ev_n == 0) {
        return MS_AGAIN;
    }
    ev = d->evq[d->ev_r];
    d->ev_r = (d->ev_r + 1u) % d->ev_cap;
    d->ev_n--;
    d->stats.events_queued = d->ev_n;
    n = out->struct_size;
    if (n > sizeof(ev)) {
        n = sizeof(ev);
    }
    memcpy(out, &ev, n);
    out->struct_size = n;
    return MS_OK;
}

ms_status ms_hw_decoder_get_caps(const ms_hw_decoder *d, ms_hw_caps *out)
{
    if (d == NULL || out == NULL || out->struct_size < sizeof(size_t)) {
        return MS_ERR_INVALID_ARG;
    }
    copy_caps(out, &d->caps);
    return MS_OK;
}

ms_status ms_hw_decoder_get_stats(const ms_hw_decoder *d, ms_hw_stats *out)
{
    size_t n;
    if (d == NULL || out == NULL || out->struct_size < sizeof(size_t)) {
        return MS_ERR_INVALID_ARG;
    }
    n = out->struct_size;
    if (n > sizeof(d->stats)) {
        n = sizeof(d->stats);
    }
    memcpy(out, &d->stats, n);
    out->struct_size = n;
    return MS_OK;
}


ms_status ms_hw_decoder_set_preferred_name(ms_hw_decoder *d, const char *name)
{
    if (d == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    if (name == NULL || name[0] == 0) {
        d->preferred_name[0] = 0;
        return MS_OK;
    }
    ms_hw_set_detail_buf(d->preferred_name, sizeof(d->preferred_name), name);
    return MS_OK;
}

#ifndef __ANDROID__
ms_status ms_hw_decoder_copy_last_output(ms_hw_decoder *d,
                                         int *width, int *height,
                                         int *stride, int *slice_height,
                                         int *color_format,
                                         uint8_t *dst, size_t dst_cap,
                                         size_t *out_size)
{
    (void)d;
    (void)width;
    (void)height;
    (void)stride;
    (void)slice_height;
    (void)color_format;
    (void)dst;
    (void)dst_cap;
    (void)out_size;
    return MS_ERR_UNSUPPORTED;
}
#endif
