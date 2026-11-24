/*
 * 宿主 mock：验证状态机、引用释放、反压、flush/EOS 边界。
 * 不解码像素，不声称设备硬解。
 * by: Long <mycjzb@gmail.com>
 */
#include "ms_hw_internal.h"

#include <string.h>


static const char *mock_comp_name(ms_codec codec, int hw)
{
    if (codec == MS_CODEC_HEVC) {
        return hw ? "mock.hevc.hw" : "mock.hevc.sw";
    }
    if (codec == MS_CODEC_MPEG2_VIDEO) {
        return hw ? "mock.mpeg2.hw" : "mock.mpeg2.sw";
    }
    if (codec == MS_CODEC_MPEG4_PART2) {
        return hw ? "mock.mpeg4.hw" : "mock.mpeg4.sw";
    }
    if (codec == MS_CODEC_H263) {
        return hw ? "mock.h263.hw" : "mock.h263.sw";
    }
    if (codec == MS_CODEC_AV1) {
        return hw ? "mock.av1.hw" : "mock.av1.sw";
    }
    if (codec == MS_CODEC_AVS_PLUS) {
        return hw ? "mock.avsplus.hw" : "mock.avsplus.sw";
    }
    return hw ? "mock.h264.hw" : "mock.h264.sw";
}

ms_status ms_hw_mock_configure(ms_hw_decoder *d, const ms_hw_config *cfg)
{
    d->caps.codec = cfg->codec;
    d->caps.api_level = 0;
    d->caps.backend = "mock";
    d->caps.verified = 1;
    d->surface = cfg->surface;
    d->sample_fmt = cfg->sample_fmt;
    d->width = cfg->width;
    d->height = cfg->height;
    d->tb_num = cfg->time_base_num > 0 ? cfg->time_base_num : 1;
    d->tb_den = cfg->time_base_den > 0 ? cfg->time_base_den : 1000000;
    if (d->opts.mock_mode == MS_HW_MOCK_AS_SW) {
        d->caps.kind = MS_HW_KIND_SOFTWARE;
        d->caps.is_hardware = 0;
        strncpy(d->name_buf,
                mock_comp_name(cfg->codec, 0),
                sizeof(d->name_buf) - 1u);
        d->caps.codec_name = d->name_buf;
        ms_hw_set_detail(d, "mock software-only; refused");
        if (!d->opts.allow_software) {
            return MS_ERR_UNSUPPORTED;
        }
        /* allow_software 仍不得把 is_hardware 置 1 */
        d->st = MS_HW_ST_CONFIGURED;
        return MS_OK;
    }
    if (d->opts.mock_mode == MS_HW_MOCK_AS_HW) {
        d->caps.kind = MS_HW_KIND_HARDWARE;
        d->caps.is_hardware = 1;
        strncpy(d->name_buf,
                mock_comp_name(cfg->codec, 1),
                sizeof(d->name_buf) - 1u);
        d->caps.codec_name = d->name_buf;
        ms_hw_set_detail(d, "simulated hardware for lifecycle tests; not a device");
        d->st = MS_HW_ST_CONFIGURED;
        return MS_OK;
    }
    d->caps.kind = MS_HW_KIND_NONE;
    d->caps.is_hardware = 0;
    d->caps.codec_name = NULL;
    ms_hw_set_detail(d, "host mock default is not device hardware");
    return MS_ERR_UNSUPPORTED;
}

ms_status ms_hw_mock_start(ms_hw_decoder *d)
{
    ms_hw_event ev;
    d->st = MS_HW_ST_STARTED;
    memset(&ev, 0, sizeof(ev));
    ev.struct_size = sizeof(ev);
    ev.type = MS_HW_EVENT_FORMAT_CHANGED;
    ev.width = d->width;
    ev.height = d->height;
    ev.detail = "mock format";
    (void)ms_hw_push_event(d, &ev);
    return MS_OK;
}

ms_status ms_hw_mock_flush(ms_hw_decoder *d)
{
    (void)d;
    return MS_OK;
}

static unsigned first_occupied(const ms_hw_decoder *d)
{
    unsigned i;
    for (i = 0; i < d->in_cap; i++) {
        if (d->in_slots[i].occupied) {
            return i;
        }
    }
    return d->in_cap;
}

ms_status ms_hw_mock_drive(ms_hw_decoder *d)
{
    unsigned i = first_occupied(d);
    ms_hw_event ev;
    ms_hw_slot *s;
    if (i >= d->in_cap) {
        if (d->st == MS_HW_ST_DRAINING && d->eos_sent) {
            memset(&ev, 0, sizeof(ev));
            ev.struct_size = sizeof(ev);
            ev.type = MS_HW_EVENT_EOS;
            ev.detail = "mock eos";
            if (ms_hw_push_event(d, &ev) == MS_OK) {
                d->st = MS_HW_ST_EOS;
            }
            return MS_OK;
        }
        return MS_AGAIN;
    }
    s = &d->in_slots[i];
    if (s->flags & MS_HW_FLAG_EOS) {
        d->in_bytes -= s->size;
        d->in_n--;
        ms_hw_release_slot(s);
        d->stats.pending_in = d->in_n;
        d->stats.pending_in_bytes = d->in_bytes;
        memset(&ev, 0, sizeof(ev));
        ev.struct_size = sizeof(ev);
        ev.type = MS_HW_EVENT_EOS;
        ev.detail = "mock eos";
        if (ms_hw_push_event(d, &ev) != MS_OK) {
            return MS_LIMIT;
        }
        d->st = MS_HW_ST_EOS;
        return MS_OK;
    }
    if (s->flags & MS_HW_FLAG_CODEC_CONFIG) {
        /* 配置已在 configure 消化；这里只释放引用，不渲染。 */
        d->in_bytes -= s->size;
        d->in_n--;
        ms_hw_release_slot(s);
        d->stats.pending_in = d->in_n;
        d->stats.pending_in_bytes = d->in_bytes;
        return MS_OK;
    }
    /* 无平台 copy、无中间整帧缓冲：直接视为已交 Surface。 */
    memset(&ev, 0, sizeof(ev));
    ev.struct_size = sizeof(ev);
    ev.type = MS_HW_EVENT_OUTPUT_RENDERED;
    ev.has_pts = 1;
    ev.pts = s->pts;
    ev.width = d->width;
    ev.height = d->height;
    ev.detail = "mock render, no pixel readback";
    if (ms_hw_push_event(d, &ev) != MS_OK) {
        return MS_LIMIT;
    }
    d->stats.rendered_frames++;
    d->in_bytes -= s->size;
    d->in_n--;
    ms_hw_release_slot(s);
    d->stats.pending_in = d->in_n;
    d->stats.pending_in_bytes = d->in_bytes;
    return MS_OK;
}
