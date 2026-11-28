#define _DEFAULT_SOURCE
/*
 * 可选音频输出（AAC / AC-3 / MP3 / MP2 / Opus / FLAC / PCM）：公共状态机、有界队列、已播放帧时钟。
 * 宿主为 mock（不解释 AAC 载荷，不是设备出声）。
 * 默认拒绝系统软件解码器；不得把软件报成硬件。
 * by: Long <mycjzb@gmail.com>
 */
#include "msmedia/ms_hw.h"

#include "ms_hw_internal.h"
#include "ms_hw_audio_trim.h"
#ifdef MS_SINGLE_SO
#include "ms_hw_passthrough.h"
#else
#define MS_PT_ROUTE_SOFT 0
#endif

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MS_HW_AUDIO_SLOTS 8u
#define MS_HW_AAC_FRAME_SAMPLES 1024u

#ifdef MS_SINGLE_SO
/* 单 so 额外放行的音频：E-AC-3 / G.711 / DTS / AAC-LATM（系统优先，路线 B 无系统解码器时 FFmpeg 软解）。 */
#define MS_SINGLE_EXTRA_AUDIO(c) ((c) == MS_CODEC_EAC3 || (c) == MS_CODEC_G711_ALAW || \
                                  (c) == MS_CODEC_G711_ULAW || (c) == MS_CODEC_DTS || \
                                  (c) == MS_CODEC_AAC_LATM)
#else
#define MS_SINGLE_EXTRA_AUDIO(c) 0
#endif

static int audio_codec_supported(ms_codec codec)
{
    return codec == MS_CODEC_AAC || codec == MS_CODEC_AC3 ||
           codec == MS_CODEC_MP3 || codec == MS_CODEC_MP2 ||
           codec == MS_CODEC_OPUS || codec == MS_CODEC_FLAC ||
           MS_SINGLE_EXTRA_AUDIO(codec) ||
           codec == MS_CODEC_PCM || codec == MS_CODEC_UNKNOWN;
}

static int audio_legal_no_csd(ms_codec codec)
{
    if (MS_SINGLE_EXTRA_AUDIO(codec)) {
        return 1; /* 带内配置或固定格式，无需 csd */
    }
    return codec == MS_CODEC_AC3 || codec == MS_CODEC_MP3 || codec == MS_CODEC_MP2;
}

static uint32_t audio_default_spf(ms_codec codec)
{
    if (codec == MS_CODEC_AC3 || codec == MS_CODEC_EAC3) {
        return 1536u;
    }
    if (codec == MS_CODEC_MP2 || codec == MS_CODEC_MP3) {
        return 1152u;
    }
    if (codec == MS_CODEC_OPUS) {
        return 960u;
    }
    if (codec == MS_CODEC_DTS) {
        return 512u;
    }
    return MS_HW_AAC_FRAME_SAMPLES;
}

static const char *mock_audio_name(ms_codec codec)
{
    if (codec == MS_CODEC_AC3) {
        return "mock.ac3";
    }
    if (codec == MS_CODEC_MP3) {
        return "mock.mp3";
    }
    if (codec == MS_CODEC_MP2) {
        return "mock.mp2";
    }
    if (codec == MS_CODEC_OPUS) {
        return "mock.opus";
    }
    if (codec == MS_CODEC_FLAC) {
        return "mock.flac";
    }
    if (codec == MS_CODEC_PCM) {
        return "mock.pcm";
    }
    return "mock.aac";
}

typedef struct ms_hw_audio_slot {
    uint8_t *data;
    size_t size;
    void (*release_fn)(void *ctx, uint8_t *data, size_t size);
    void *release_ctx;
    int64_t pts_us;
    unsigned flags;
    size_t pcm_bytes;
    size_t pcm_done;
    int occupied;
    int decoded;
    uint32_t samples;
    uint32_t skip_start;
    uint32_t skip_end;
    int has_pts;
    uint32_t plat_lead; /* 本包已交给平台/mock 丢掉的 leading 样点 */
} ms_hw_audio_slot;

struct ms_hw_audio {
    const ms_allocator *alloc;
    ms_allocator owned_alloc;
    ms_hw_limits limits;
    ms_hw_create_opts opts;
    ms_hw_aac_policy aac_policy;
    ms_hw_audio_caps caps;
    char name_buf[128];
    char detail_buf[192];
    int configured;
    int started;
    int eos_in;
    int eos_out;
    int flushed;
    int canceled;
    ms_codec codec;
    int sample_rate;
    int channels;
    int pcm_bits;
    int pcm_format_changes;
    uint8_t asc[MS_AUDIO_CSD_MAX];
    size_t asc_size;
    ms_sample_format sample_fmt;
    uint32_t samples_per_frame;
    uint32_t pre_skip;
    uint32_t codec_delay;
    uint32_t seek_preroll;
    uint32_t preroll_left;       /* 剩余待从解码 PCM 头部丢掉的样点（pending start / warmup） */
    uint32_t pending_start_skip; /* 尚未从后续 PCM 头部扣完的 start skip */
    uint32_t start_fallback;     /* 流开始：若包上无 skip_start，使用 OpusHead pre-skip */
    uint32_t seek_warmup_left;   /* 中途 seek 的解码预热，不是 pre-skip */
    int platform_trims_delay;    /* Android 已把 pre-skip/seek preroll 交给编解码器 */
    uint32_t platform_lead_left; /* 宿主 mock 模拟平台已修剪的剩余 leading 样点 */
    ms_hw_pcm_trim trim;
    uint8_t *trim_out;
    size_t trim_out_cap;
#ifndef __ANDROID__
#define MS_HW_DEC_OUT_MAX 48u
    int dec_mode;
    uint32_t dec_split[8];
    unsigned dec_split_n;
    unsigned dec_merge_n;
    unsigned dec_empty_n;
    unsigned dec_delay;
    unsigned dec_delay_left;
    unsigned merge_got;
    uint8_t *merge_buf;
    uint32_t merge_nsamp;
    size_t merge_cap;
    unsigned merge_mask;
    int merge_has_pts;
    int64_t merge_pts_us;
    struct {
        uint8_t *data;
        uint32_t nsamp;
        size_t bytes;
        int has_pts;
        int64_t pts_us;
        unsigned slot_mask;
    } dout[MS_HW_DEC_OUT_MAX];
    unsigned dout_n;
    unsigned dout_head;
    uint32_t pcm_seq;
    uint8_t *aud_left;
    size_t aud_left_n;
    size_t aud_left_off;
    size_t aud_left_cap;
    unsigned aud_left_mask;
    int16_t *sink;
    size_t sink_cap;
    size_t *sink_out;
    unsigned slot_decoded_mask;
#endif
    uint64_t au_in;
    uint64_t au_submitted;
    uint64_t au_played;
    uint64_t samples_played;
    int64_t clock_us;
    uint64_t bytes_queued;
    uint64_t partial_writes;
    uint64_t backpressure;
    unsigned pending;
    size_t write_quota; /* SIZE_MAX = 不受限 */
    int quota_armed;
    ms_status last_error;
    ms_hw_audio_slot slots[MS_HW_AUDIO_SLOTS];
    unsigned in_cap;
    int paused;
    int rate_num;
    int rate_den;
    ms_hw_audio_pcm_tap_fn pcm_tap;
    void *pcm_tap_ctx;
    uint64_t track_write_bytes;
    uint64_t track_write_frames;
    int clock_in_fallback; /* 最近一次取钟走了 200ms 停滞兜底 */
    /* 音频钟停走（AudioTrack 头不前或写配额耗尽）时的单调时钟兜底，避免等钟卡住视频。 */
    int64_t clock_mono_last_change_ns;
    int64_t clock_last_raw_us;
    int64_t clock_fallback_origin_ns;
    int64_t clock_emitted_us; /* 已对外发出的钟；兜底回切到 head 时保持单调 */
    int64_t sink_last_accept_ns;
#ifdef __ANDROID__
    int android_ok;
    void *plat;
    int codec_started;
    int pcm_direct; /* PCM 直出 AudioTrack，不经 MediaCodec，不是软解例外 */
    int pt_route;    /* 阶段 4：ms_pt_route；非 SOFT 时 AU 原样或 IEC61937 封装写入直通 AudioTrack */
    int pt_encoding; /* 直通时 AudioTrack 的 ENCODING_*；0 = PCM 16 位 */
#endif
    unsigned pt_fallbacks; /* 直通打开失败后回退软解的次数（诊断） */
    unsigned pt_caps; /* 阶段 4：Java 探测的直通能力位 + 开关（MS_PT_CAP_*），默认 0 = 软解 */
};

#ifdef __ANDROID__
ms_status ms_hw_audio_android_query(ms_codec codec, int allow_sw, int api_level,
                                    ms_hw_audio_caps *out, char *name, size_t name_cap,
                                    char *detail, size_t detail_cap);

static int ensure_track(ms_hw_audio *a, int rate, int ch);
ms_status ms_hw_audio_android_open(ms_hw_audio *a, const ms_hw_audio_config *cfg);
void ms_hw_audio_android_close(ms_hw_audio *a);
ms_status ms_hw_audio_android_drive(ms_hw_audio *a);
int64_t ms_hw_audio_android_played_frames(const ms_hw_audio *a);
int ms_hw_audio_android_position_us(const ms_hw_audio *a, int64_t *pos_us, int64_t *raw_us);
void ms_hw_audio_android_sync_info(const ms_hw_audio *a, int *src, int64_t *latency_us);
void ms_hw_audio_android_flush(ms_hw_audio *a);
ms_status ms_hw_audio_android_set_rate(ms_hw_audio *a, int num, int den);
void ms_hw_audio_android_set_paused(ms_hw_audio *a, int paused);
#endif

static void *std_alloc(void *ctx, size_t n)
{
    (void)ctx;
    return malloc(n ? n : 1);
}

static void std_free(void *ctx, void *p)
{
    (void)ctx;
    free(p);
}

static void release_slot(ms_hw_audio *a, ms_hw_audio_slot *s)
{
    if (s == NULL || !s->occupied) {
        return;
    }
    if (s->release_fn != NULL) {
        s->release_fn(s->release_ctx, s->data, s->size);
    }
    if (a->bytes_queued >= s->pcm_bytes) {
        a->bytes_queued -= s->pcm_bytes;
    } else {
        a->bytes_queued = 0;
    }
    memset(s, 0, sizeof(*s));
    if (a->pending > 0) {
        a->pending--;
    }
}

static size_t audio_bpf(const ms_hw_audio *a)
{
    size_t bpf = (size_t)a->channels * (size_t)(a->pcm_bits / 8);
    return bpf ? bpf : 2;
}

static void *audio_alloc(ms_hw_audio *a, size_t n)
{
    void *p;
    if (n == 0) {
        return NULL;
    }
    if (a->alloc != NULL && a->alloc->alloc_fn != NULL) {
        p = a->alloc->alloc_fn(a->alloc->ctx, n);
    } else {
        p = malloc(n);
    }
    if (p != NULL) {
        memset(p, 0, n);
    }
    return p;
}

static void audio_xfree(ms_hw_audio *a, void *p)
{
    if (p == NULL) {
        return;
    }
    if (a->alloc != NULL && a->alloc->free_fn != NULL) {
        a->alloc->free_fn(a->alloc->ctx, p);
    } else {
        free(p);
    }
}

static ms_status ensure_trim_out(ms_hw_audio *a, size_t need)
{
    uint8_t *nbuf;
    if (a == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    if (need < 64u) {
        need = 64u;
    }
    if (a->trim_out_cap >= need) {
        return MS_OK;
    }
    nbuf = (uint8_t *)audio_alloc(a, need);
    if (nbuf == NULL) {
        return MS_ERR_NOMEM;
    }
    audio_xfree(a, a->trim_out);
    a->trim_out = nbuf;
    a->trim_out_cap = need;
    return MS_OK;
}

static ms_status push_meta_for_slot(ms_hw_audio *a, ms_hw_audio_slot *s)
{
    ms_hw_pcm_trim_meta m;
    ms_status st;
    if (ms_hw_pcm_trim_pending_meta(&a->trim) >= MS_HW_PCM_TRIM_META_MAX) {
        return MS_LIMIT;
    }
    memset(&m, 0, sizeof(m));
    m.struct_size = sizeof(m);
    s->plat_lead = 0;
    if (a->platform_trims_delay) {
        /* 平台已按 csd-1/csd-2 从本包输出去掉 leading。本层用「压缩样点减去
         * 已交给平台的 leading」作 expected，AU 按样点计数结束，从而在缺 PTS
         * 或时间戳相同时仍能扣 skip_end。不得把完整压缩长度当 expected。 */
        uint32_t ns = s->samples;
        uint32_t lead = 0;
        m.skip_start = 0;
        if (a->platform_lead_left > 0 && ns > 0) {
            lead = a->platform_lead_left;
            if (lead > ns) {
                lead = ns;
            }
            a->platform_lead_left -= lead;
            ns -= lead;
        }
        s->plat_lead = lead;
        if (ns == 0) {
            /* 整包已被平台作为 delay/preroll 去掉：不入队 meta，避免 expected=0 粘连。 */
            a->preroll_left = a->platform_lead_left;
            return MS_OK;
        }
        m.expected_samples = ns;
        m.skip_end = s->skip_end;
    } else {
        if (s->skip_start > 0) {
            m.skip_start = s->skip_start;
            a->start_fallback = 0;
            a->seek_warmup_left = 0;
        } else if (a->start_fallback > 0) {
            st = ms_hw_pcm_trim_add_prefix(&a->trim, a->start_fallback);
            if (st != MS_OK) {
                return st;
            }
            a->start_fallback = 0;
        } else if (a->seek_warmup_left > 0) {
            st = ms_hw_pcm_trim_add_prefix(&a->trim, a->seek_warmup_left);
            if (st != MS_OK) {
                return st;
            }
            a->seek_warmup_left = 0;
        }
        m.skip_end = s->skip_end;
        m.expected_samples = s->samples;
    }
    m.has_pts = s->has_pts;
    m.pts_us = s->pts_us;
    st = ms_hw_pcm_trim_push(&a->trim, &m);
    a->preroll_left = ms_hw_pcm_trim_prefix_left(&a->trim);
    return st;
}

static void audio_tap_trimmed_pts(ms_hw_audio *a, const uint8_t *pcm, size_t bytes,
                                 int has_pts, int64_t au_pts_us, uint32_t first_idx)
{
    int64_t pts_us = 0;
    if (a->pcm_tap == NULL || bytes == 0 || pcm == NULL) {
        return;
    }
    if (has_pts && a->sample_rate > 0) {
        pts_us = au_pts_us + ((int64_t)first_idx * 1000000ll) / a->sample_rate;
    }
    a->pcm_tap(a->pcm_tap_ctx, pcm, bytes, a->sample_rate, a->channels,
               a->pcm_bits ? a->pcm_bits : 16, has_pts, pts_us);
}

#ifndef __ANDROID__
/* 仅宿主 mock 路径调用；Android 走 audio_tap_trimmed_pts。 */
static void audio_tap_trimmed(ms_hw_audio *a, const uint8_t *pcm, size_t bytes)
{
    int has_pts = 0;
    int64_t au_pts = 0;
    uint32_t first_idx = 0, nsamp = 0;
    if (a->pcm_tap == NULL || bytes == 0) {
        return;
    }
    ms_hw_pcm_trim_last_out(&a->trim, &has_pts, &au_pts, &first_idx, &nsamp);
    audio_tap_trimmed_pts(a, pcm, bytes, has_pts, au_pts, first_idx);
    (void)nsamp;
}

static ms_status ensure_host_buf(ms_hw_audio *a, uint8_t **p, size_t *cap, size_t need)
{
    uint8_t *nbuf;
    if (need <= *cap) {
        return MS_OK;
    }
    nbuf = (uint8_t *)audio_alloc(a, need);
    if (nbuf == NULL) {
        return MS_ERR_NOMEM;
    }
    if (*p != NULL && *cap > 0) {
        memcpy(nbuf, *p, *cap);
    }
    audio_xfree(a, *p);
    *p = nbuf;
    *cap = need;
    return MS_OK;
}

static void host_clear_dec_queues(ms_hw_audio *a)
{
    unsigned i;
    for (i = 0; i < MS_HW_DEC_OUT_MAX; i++) {
        audio_xfree(a, a->dout[i].data);
        memset(&a->dout[i], 0, sizeof(a->dout[i]));
    }
    a->dout_n = 0;
    a->dout_head = 0;
    a->merge_nsamp = 0;
    a->merge_got = 0;
    a->merge_mask = 0;
    a->merge_has_pts = 0;
    a->merge_pts_us = 0;
    a->aud_left_n = 0;
    a->aud_left_off = 0;
    a->aud_left_mask = 0;
    a->dec_delay_left = a->dec_delay;
    a->slot_decoded_mask = 0;
}

static void fill_inc_pcm(uint8_t *dst, uint32_t nsamp, unsigned ch, uint32_t start)
{
    uint32_t i, c;
    int16_t *p = (int16_t *)(void *)dst;
    if (ch == 0) {
        ch = 1;
    }
    for (i = 0; i < nsamp; i++) {
        int16_t v = (int16_t)(start + i);
        for (c = 0; c < ch; c++) {
            *p++ = v;
        }
    }
}

static ms_status push_dout(ms_hw_audio *a, const uint8_t *pcm, uint32_t nsamp,
                           int has_pts, int64_t pts_us, unsigned mask)
{
    unsigned ix;
    size_t bpf = audio_bpf(a);
    size_t bytes;
    if (a->dout_n >= MS_HW_DEC_OUT_MAX) {
        return MS_LIMIT;
    }
    bytes = (size_t)nsamp * bpf;
    ix = (a->dout_head + a->dout_n) % MS_HW_DEC_OUT_MAX;
    memset(&a->dout[ix], 0, sizeof(a->dout[ix]));
    if (bytes > 0) {
        a->dout[ix].data = (uint8_t *)audio_alloc(a, bytes);
        if (a->dout[ix].data == NULL) {
            return MS_ERR_NOMEM;
        }
        if (pcm != NULL) {
            memcpy(a->dout[ix].data, pcm, bytes);
        }
    }
    a->dout[ix].nsamp = nsamp;
    a->dout[ix].bytes = bytes;
    a->dout[ix].has_pts = has_pts;
    a->dout[ix].pts_us = pts_us;
    a->dout[ix].slot_mask = mask;
    a->dout_n++;
    return MS_OK;
}

static uint32_t slot_nsamp(ms_hw_audio *a, const ms_hw_audio_slot *s)
{
    uint32_t n = s->samples;
    if (n == 0) {
        n = a->samples_per_frame ? a->samples_per_frame : audio_default_spf(a->codec);
    }
    return n;
}

static ms_status decode_slot(ms_hw_audio *a, unsigned si)
{
    ms_hw_audio_slot *s = &a->slots[si];
    unsigned ch = a->channels ? (unsigned)a->channels : 1u;
    size_t bpf = audio_bpf(a);
    uint32_t ns;
    uint32_t start_seq;
    ms_status st;
    uint8_t *tmp;
    size_t bytes;
    unsigned mask = (1u << si);

    if (!s->occupied || s->decoded) {
        return MS_OK;
    }
    ns = slot_nsamp(a, s);
    start_seq = a->pcm_seq;
    st = push_meta_for_slot(a, s);
    if (st != MS_OK) {
        return st;
    }
    bytes = (size_t)ns * bpf;
    tmp = (uint8_t *)audio_alloc(a, bytes ? bytes : 1u);
    if (tmp == NULL) {
        return MS_ERR_NOMEM;
    }
    fill_inc_pcm(tmp, ns, ch, start_seq);
    a->pcm_seq += ns;
    s->decoded = 1;
#ifndef __ANDROID__
    /* 模拟 MediaCodec 已按 csd-1/csd-2 去掉 pre-skip / seek preroll。
     * pcm_seq 仍按完整 TOC 前进，以便后续包的递增样点与未修剪路径对齐。 */
    if (a->platform_trims_delay && s->plat_lead > 0 && ns > 0) {
        uint32_t lead = s->plat_lead;
        if (lead > ns) {
            lead = ns;
        }
        if (lead > 0) {
            if (lead < ns) {
                memmove(tmp, tmp + (size_t)lead * bpf, (size_t)(ns - lead) * bpf);
            }
            ns -= lead;
            bytes = (size_t)ns * bpf;
        }
    }
    if (ns == 0) {
        /* push_meta 已因整包 leading 而不入队；空输出不得再喂修剪机。 */
        audio_xfree(a, tmp);
        return MS_OK;
    }
#endif

    if (a->dec_mode == MS_HW_AUDIO_DEC_EMPTY_FIRST && a->dec_empty_n > 0) {
        st = push_dout(a, NULL, 0, s->has_pts, s->pts_us, 0);
        if (st != MS_OK) {
            audio_xfree(a, tmp);
            return st;
        }
        a->dec_empty_n--;
    }

    if (a->dec_mode == MS_HW_AUDIO_DEC_SPLIT && a->dec_split_n > 0) {
        uint32_t off = 0;
        unsigned k;
        for (k = 0; k < a->dec_split_n && off < ns; k++) {
            uint32_t chunk = a->dec_split[k];
            int last;
            if (chunk > ns - off) {
                chunk = ns - off;
            }
            if (chunk == 0) {
                continue;
            }
            off += chunk;
            last = (k + 1u == a->dec_split_n) || (off >= ns);
            st = push_dout(a, tmp + (size_t)(off - chunk) * bpf, chunk,
                           (k == 0) ? s->has_pts : 0, s->pts_us,
                           last ? mask : 0u);
            if (st != MS_OK) {
                audio_xfree(a, tmp);
                return st;
            }
        }
        if (off < ns) {
            st = push_dout(a, tmp + (size_t)off * bpf, ns - off, 0, s->pts_us, mask);
            if (st != MS_OK) {
                audio_xfree(a, tmp);
                return st;
            }
        }
        audio_xfree(a, tmp);
        return MS_OK;
    }

    if (a->dec_mode == MS_HW_AUDIO_DEC_MERGE && a->dec_merge_n > 1u) {
        size_t need = (size_t)(a->merge_nsamp + ns) * bpf;
        st = ensure_host_buf(a, &a->merge_buf, &a->merge_cap, need);
        if (st != MS_OK) {
            audio_xfree(a, tmp);
            return st;
        }
        memcpy(a->merge_buf + (size_t)a->merge_nsamp * bpf, tmp, bytes);
        if (a->merge_got == 0) {
            a->merge_has_pts = s->has_pts;
            a->merge_pts_us = s->pts_us;
        }
        a->merge_nsamp += ns;
        a->merge_got++;
        a->merge_mask |= mask;
        audio_xfree(a, tmp);
        if (a->merge_got >= a->dec_merge_n) {
            st = push_dout(a, a->merge_buf, a->merge_nsamp, a->merge_has_pts,
                           a->merge_pts_us, a->merge_mask);
            a->merge_nsamp = 0;
            a->merge_got = 0;
            a->merge_mask = 0;
            return st;
        }
        return MS_OK;
    }

    st = push_dout(a, tmp, ns, s->has_pts, s->pts_us, mask);
    audio_xfree(a, tmp);
    return st;
}

static void release_mask(ms_hw_audio *a, unsigned mask)
{
    unsigned i;
    for (i = 0; i < a->in_cap && i < 32u; i++) {
        if ((mask & (1u << i)) != 0 && a->slots[i].occupied) {
            a->au_played++;
            release_slot(a, &a->slots[i]);
        }
    }
}

static void sink_write(ms_hw_audio *a, const uint8_t *p, size_t n)
{
    size_t bpf = audio_bpf(a);
    size_t frames, i, ch;
    const int16_t *s;
    if (a->sink == NULL || a->sink_out == NULL || bpf < 2u || n < bpf) {
        return;
    }
    frames = n / bpf;
    ch = bpf / 2u;
    s = (const int16_t *)(const void *)p;
    for (i = 0; i < frames && *a->sink_out < a->sink_cap; i++) {
        a->sink[(*a->sink_out)++] = s[i * ch];
    }
}

static ms_status append_aud(ms_hw_audio *a, const uint8_t *p, size_t n, unsigned mask)
{
    ms_status st;
    if (a->aud_left_off > 0) {
        if (a->aud_left_n > a->aud_left_off) {
            memmove(a->aud_left, a->aud_left + a->aud_left_off,
                    a->aud_left_n - a->aud_left_off);
            a->aud_left_n -= a->aud_left_off;
        } else {
            a->aud_left_n = 0;
        }
        a->aud_left_off = 0;
    }
    st = ensure_host_buf(a, &a->aud_left, &a->aud_left_cap, a->aud_left_n + n + 1u);
    if (st != MS_OK) {
        return st;
    }
    if (n > 0 && p != NULL) {
        memcpy(a->aud_left + a->aud_left_n, p, n);
        a->aud_left_n += n;
    }
    a->aud_left_mask |= mask;
    return MS_OK;
}

static ms_status write_leftover(ms_hw_audio *a)
{
    size_t left, take, bpf;
    uint32_t ns;
    unsigned m;
    if (a->aud_left_off >= a->aud_left_n) {
        m = a->aud_left_mask;
        a->aud_left_off = 0;
        a->aud_left_n = 0;
        if (m != 0) {
            a->aud_left_mask = 0;
            release_mask(a, m);
        }
        return MS_OK;
    }
    bpf = audio_bpf(a);
    left = a->aud_left_n - a->aud_left_off;
    if (a->quota_armed && a->write_quota == 0) {
        return MS_OK;
    }
    take = left;
    if (a->quota_armed && take > a->write_quota) {
        take = a->write_quota;
    }
    if (bpf > 0) {
        take -= take % bpf;
    }
    if (take == 0) {
        a->partial_writes++;
        return MS_OK;
    }
    sink_write(a, a->aud_left + a->aud_left_off, take);
    a->aud_left_off += take;
    if (a->quota_armed) {
        a->write_quota -= take;
    }
    ns = (uint32_t)(take / bpf);
    a->samples_played += ns;
    /* 由累计可听样点重算，避免分次写的整除截断使时钟偏离整包值。 */
    if (a->sample_rate > 0) {
        a->clock_us = (int64_t)((a->samples_played * 1000000ull) /
                                (unsigned)a->sample_rate);
    }
    if (a->aud_left_off < a->aud_left_n) {
        a->partial_writes++;
        return MS_OK;
    }
    m = a->aud_left_mask;
    a->aud_left_off = 0;
    a->aud_left_n = 0;
    a->aud_left_mask = 0;
    release_mask(a, m);
    return MS_OK;
}

static ms_status feed_one_dout(ms_hw_audio *a)
{
    unsigned ix;
    ms_status st;
    size_t out_bytes = 0;
    size_t bpf;
    size_t need;
    if (a->dout_n == 0) {
        return MS_OK;
    }
    ix = a->dout_head;
    bpf = audio_bpf(a);
    need = (size_t)a->dout[ix].nsamp * bpf +
           (size_t)MS_HW_PCM_TRIM_HOLD_MAX * MS_HW_PCM_TRIM_BPF_MAX;
    st = ensure_trim_out(a, need);
    if (st != MS_OK) {
        return st;
    }
    st = ms_hw_pcm_trim_feed(&a->trim, a->dout[ix].data, a->dout[ix].nsamp,
                             a->dout[ix].has_pts, a->dout[ix].pts_us,
                             a->trim_out, a->trim_out_cap, &out_bytes);
    if (st != MS_OK) {
        return st;
    }
    st = append_aud(a, a->trim_out, out_bytes, a->dout[ix].slot_mask);
    if (st == MS_OK && out_bytes > 0) {
        audio_tap_trimmed(a, a->trim_out, out_bytes);
    }
    audio_xfree(a, a->dout[ix].data);
    memset(&a->dout[ix], 0, sizeof(a->dout[ix]));
    a->dout_head = (a->dout_head + 1u) % MS_HW_DEC_OUT_MAX;
    a->dout_n--;
    a->preroll_left = ms_hw_pcm_trim_prefix_left(&a->trim);
    return st;
}

static void recount_queued(ms_hw_audio *a)
{
    size_t n = 0;
    unsigned i;
    if (a->aud_left_n > a->aud_left_off) {
        n += a->aud_left_n - a->aud_left_off;
    }
    for (i = 0; i < a->in_cap; i++) {
        if (a->slots[i].occupied && !a->slots[i].decoded) {
            n += a->slots[i].pcm_bytes;
        }
    }
    a->bytes_queued = n;
}
#endif /* !__ANDROID__ */


static void drop_all_pending(ms_hw_audio *a)
{
    unsigned i;
    for (i = 0; i < a->in_cap; i++) {
        release_slot(a, &a->slots[i]);
    }
    a->pending = 0;
    a->bytes_queued = 0;
    ms_hw_pcm_trim_reset(&a->trim);
#ifndef __ANDROID__
    host_clear_dec_queues(a);
    a->pcm_seq = 0;
#endif
}

static void fill_host_query(ms_hw_audio_caps *out, ms_codec codec, int mock_mode,
                            int allow_sw, ms_hw_aac_policy policy)
{
    const char *lab = ms_hw_audio_label(codec == MS_CODEC_UNKNOWN ? MS_CODEC_AAC : codec);
    memset(out, 0, sizeof(*out));
    out->struct_size = sizeof(*out);
    out->codec = (codec == MS_CODEC_UNKNOWN) ? MS_CODEC_AAC : codec;
    out->api_level = 0;
    out->verified = 1;
    out->backend = "mock";
    out->codec_name = mock_audio_name(out->codec);
    if (mock_mode == MS_HW_MOCK_AS_FOREIGN_SW) {
        out->kind = MS_HW_KIND_SOFTWARE;
        out->is_hardware = 0;
        out->system_decode_is_not_hardware = 1;
        out->codec_name = (out->codec == MS_CODEC_AAC) ? "mock.foreign.aac" : "mock.foreign.sw";
        out->detail = "host mock: non-system software codec, refused";
        (void)lab;
    } else if (mock_mode == MS_HW_MOCK_AS_SW) {
        out->kind = MS_HW_KIND_SOFTWARE;
        out->is_hardware = 0;
        out->system_decode_is_not_hardware = 1;
        if (out->codec == MS_CODEC_AAC) {
            out->codec_name = "OMX.google.aac.decoder";
        } else if (out->codec == MS_CODEC_MP3) {
            out->codec_name = "OMX.google.mp3.decoder";
        } else {
            out->codec_name = "mock.sw";
        }
        if (out->codec == MS_CODEC_AAC && policy == MS_HW_AAC_POLICY_ALLOW_SYSTEM) {
            out->detail = "系统 AAC，非硬件";
        } else if (allow_sw) {
            out->detail = "host mock software decoder; not hardware (allowed by option)";
        } else {
            out->detail = "host mock: software decoder refused (not hardware)";
        }
    } else if (mock_mode == MS_HW_MOCK_AS_HW) {
        out->kind = MS_HW_KIND_HARDWARE;
        /* 宿主即使模拟「选到硬件组件」也不得报设备硬解。 */
        out->is_hardware = 0;
        out->system_decode_is_not_hardware = 0;
        out->detail = "host mock as-hw lifecycle; not a device hardware decoder";
    } else {
        out->kind = MS_HW_KIND_NONE;
        out->is_hardware = 0;
        out->system_decode_is_not_hardware = 0;
        out->detail = "host mock default: not device hardware audio";
    }
}

ms_status ms_hw_audio_query(ms_codec codec, ms_hw_audio_caps *out)
{
    if (out == NULL || out->struct_size < sizeof(size_t)) {
        return MS_ERR_INVALID_ARG;
    }
    if (!audio_codec_supported(codec)) {
        size_t sz = out->struct_size;
        if (sz > sizeof(*out)) {
            sz = sizeof(*out);
        }
        memset(out, 0, sz);
        out->struct_size = sz;
        out->codec = codec;
        out->kind = MS_HW_KIND_NONE;
        out->backend = "none";
        out->detail = "audio codec not in hardware path scope";
        out->verified = 1;
        return MS_ERR_UNSUPPORTED;
    }
#ifdef __ANDROID__
    {
        ms_hw_audio_caps tmp;
        char name[128], detail[192];
        ms_status st;
        memset(&tmp, 0, sizeof(tmp));
        name[0] = 0;
        detail[0] = 0;
        st = ms_hw_audio_android_query((codec == MS_CODEC_UNKNOWN) ? MS_CODEC_AAC : codec,
                                       0, 0, &tmp, name, sizeof(name),
                                       detail, sizeof(detail));
        if (out->struct_size > sizeof(*out)) {
            out->struct_size = sizeof(*out);
        } else if (out->struct_size < sizeof(*out)) {
            /* 截断拷贝由调用方 struct_size 限制：先填临时再 memcpy */
        }
        {
            size_t sz = out->struct_size;
            if (sz > sizeof(tmp)) {
                sz = sizeof(tmp);
            }
            memcpy(out, &tmp, sz);
            out->struct_size = sz;
        }
        return st;
    }
#else
    {
        ms_hw_audio_caps tmp;
        size_t sz;
        fill_host_query(&tmp, codec, MS_HW_MOCK_DEFAULT, 0, MS_HW_AAC_POLICY_HARDWARE_ONLY);
        sz = out->struct_size;
        if (sz > sizeof(tmp)) {
            sz = sizeof(tmp);
        }
        memcpy(out, &tmp, sz);
        out->struct_size = sz;
        return MS_OK;
    }
#endif
}

ms_status ms_hw_audio_create(const ms_allocator *alloc, const ms_hw_limits *limits,
                             const ms_hw_create_opts *opts, ms_hw_audio **out)
{
    ms_hw_audio *a;
    ms_hw_limits lim;
    ms_hw_create_opts o;
    if (out == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    *out = NULL;
    memset(&lim, 0, sizeof(lim));
    lim.struct_size = sizeof(lim);
    if (limits != NULL) {
        size_t n = limits->struct_size;
        if (n < sizeof(limits->struct_size)) {
            return MS_ERR_INVALID_ARG;
        }
        if (n > sizeof(lim)) {
            n = sizeof(lim);
        }
        memcpy(&lim, limits, n);
        lim.struct_size = sizeof(lim);
    }
    memset(&o, 0, sizeof(o));
    o.struct_size = sizeof(o);
    if (opts != NULL) {
        size_t n = opts->struct_size;
        if (n < sizeof(opts->struct_size)) {
            return MS_ERR_INVALID_ARG;
        }
        if (n > sizeof(o)) {
            n = sizeof(o);
        }
        memcpy(&o, opts, n);
        o.struct_size = sizeof(o);
    }
    a = (ms_hw_audio *)(alloc && alloc->alloc_fn ? alloc->alloc_fn(alloc->ctx, sizeof(*a))
                                                 : malloc(sizeof(*a)));
    if (a == NULL) {
        return MS_ERR_NOMEM;
    }
    memset(a, 0, sizeof(*a));
    if (alloc != NULL && alloc->alloc_fn != NULL && alloc->free_fn != NULL) {
        a->alloc = alloc;
    } else {
        a->owned_alloc.alloc_fn = std_alloc;
        a->owned_alloc.free_fn = std_free;
        a->alloc = &a->owned_alloc;
    }
    a->limits = lim;
    a->opts = o;
    a->in_cap = lim.max_pending_in > 0 ? lim.max_pending_in : MS_HW_AUDIO_SLOTS;
    if (a->in_cap > MS_HW_AUDIO_SLOTS) {
        a->in_cap = MS_HW_AUDIO_SLOTS;
    }
    a->pcm_bits = 16;
    a->codec = MS_CODEC_AAC;
    a->samples_per_frame = 0;
    a->pre_skip = 0;
    a->codec_delay = 0;
    a->preroll_left = 0;
    a->write_quota = (size_t)-1;
    a->quota_armed = 0;
    fill_host_query(&a->caps, a->codec, o.mock_mode, o.allow_software, a->aac_policy);
    a->rate_num = 1;
    a->rate_den = 1;
    a->paused = 0;
    ms_hw_pcm_trim_init(&a->trim);
    *out = a;
    return MS_OK;
}


ms_status ms_hw_audio_set_aac_policy(ms_hw_audio *a, ms_hw_aac_policy policy)
{
    if (a == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    if (policy != MS_HW_AAC_POLICY_HARDWARE_ONLY &&
        policy != MS_HW_AAC_POLICY_ALLOW_SYSTEM) {
        return MS_ERR_INVALID_ARG;
    }
    if (a->configured || a->started) {
        return MS_ERR_STATE;
    }
    a->aac_policy = policy;
    fill_host_query(&a->caps, a->codec, a->opts.mock_mode, a->opts.allow_software, a->aac_policy);
    return MS_OK;
}

ms_hw_aac_policy ms_hw_audio_get_aac_policy(const ms_hw_audio *a)
{
    if (a == NULL) {
        return MS_HW_AAC_POLICY_HARDWARE_ONLY;
    }
    return a->aac_policy;
}

void ms_hw_audio_destroy(ms_hw_audio *a)
{
    const ms_allocator *al;
    if (a == NULL) {
        return;
    }
#ifdef __ANDROID__
    ms_hw_audio_android_close(a);
#endif
    drop_all_pending(a);
#ifndef __ANDROID__
    host_clear_dec_queues(a);
    audio_xfree(a, a->merge_buf);
    a->merge_buf = NULL;
    a->merge_cap = 0;
    audio_xfree(a, a->aud_left);
    a->aud_left = NULL;
    a->aud_left_cap = 0;
#endif
    audio_xfree(a, a->trim_out);
    a->trim_out = NULL;
    a->trim_out_cap = 0;
    al = a->alloc;
    if (al == &a->owned_alloc) {
        free(a);
    } else if (al != NULL && al->free_fn != NULL) {
        al->free_fn(al->ctx, a);
    } else {
        free(a);
    }
}

static int cfg_has_field(const ms_hw_audio_config *cfg, size_t off, size_t fld)
{
    return cfg != NULL && cfg->struct_size >= off + fld;
}

static ms_status apply_asc(ms_hw_audio *a, const ms_hw_audio_config *cfg)
{
    int rate = 0, ch = 0;
    uint32_t spf = 0, pre = 0, delay = 0, spr = 0;
    ms_codec codec = cfg->codec;
    if (!audio_codec_supported(codec) || codec == MS_CODEC_UNKNOWN) {
        if (codec == MS_CODEC_UNKNOWN) {
            codec = MS_CODEC_AAC; /* 旧调用方可省略，保持 AAC */
        } else {
            return MS_ERR_UNSUPPORTED;
        }
    }
    if (cfg_has_field(cfg, offsetof(ms_hw_audio_config, samples_per_frame),
                      sizeof(cfg->samples_per_frame))) {
        spf = cfg->samples_per_frame;
    }
    if (cfg_has_field(cfg, offsetof(ms_hw_audio_config, pre_skip),
                      sizeof(cfg->pre_skip))) {
        pre = cfg->pre_skip;
    }
    if (cfg_has_field(cfg, offsetof(ms_hw_audio_config, codec_delay),
                      sizeof(cfg->codec_delay))) {
        delay = cfg->codec_delay;
    }
    if (cfg_has_field(cfg, offsetof(ms_hw_audio_config, seek_preroll),
                      sizeof(cfg->seek_preroll))) {
        spr = cfg->seek_preroll;
    }
    if (cfg->asc != NULL && cfg->asc_size > 0) {
        ms_audio_cfg parsed;
        ms_status pst;
        if (cfg->asc_size > sizeof(a->asc)) {
            return MS_ERR_OVERFLOW;
        }
        ms_audio_cfg_default(&parsed);
        pst = ms_audio_cfg_parse(codec, cfg->sample_fmt, 1, cfg->asc,
                                 cfg->asc_size, &parsed);
        if (pst != MS_OK || !parsed.complete) {
            /* AAC 保持原入口：解析失败记 UNSUPPORTED，与既有测试一致 */
            if (codec == MS_CODEC_AAC) {
                ms_aac_asc_info info;
                memset(&info, 0, sizeof(info));
                info.struct_size = sizeof(info);
                if (ms_aac_asc_parse(cfg->asc, cfg->asc_size, &info) != MS_OK) {
                    return MS_ERR_UNSUPPORTED;
                }
                rate = info.sample_rate;
                ch = info.channels;
            } else {
                return (pst == MS_OK) ? MS_ERR_MALFORMED : pst;
            }
        } else {
            rate = parsed.sample_rate;
            ch = parsed.channels;
            if (spf == 0) {
                spf = parsed.samples_per_frame;
            }
            if (pre == 0) {
                pre = parsed.pre_skip;
            }
            if (delay == 0) {
                delay = parsed.codec_delay;
            }
            if (spr == 0) {
                spr = parsed.seek_preroll;
            }
            if (parsed.csd_size > 0) {
                memcpy(a->asc, parsed.csd, parsed.csd_size);
                a->asc_size = parsed.csd_size;
            } else {
                memcpy(a->asc, cfg->asc, cfg->asc_size);
                a->asc_size = cfg->asc_size;
            }
        }
        if (a->asc_size == 0) {
            memcpy(a->asc, cfg->asc, cfg->asc_size);
            a->asc_size = cfg->asc_size;
        }
    } else if (audio_legal_no_csd(codec) || codec == MS_CODEC_AAC ||
               codec == MS_CODEC_FLAC || codec == MS_CODEC_PCM) {
        if (cfg->sample_rate > 0 && cfg->channels > 0) {
            rate = cfg->sample_rate;
            ch = cfg->channels;
            a->asc_size = 0;
            if (codec == MS_CODEC_FLAC && cfg->asc != NULL && cfg->asc_size > 0 &&
                cfg->asc_size <= sizeof(a->asc)) {
                memcpy(a->asc, cfg->asc, cfg->asc_size);
                a->asc_size = cfg->asc_size;
            }
        } else {
            return MS_ERR_INVALID_ARG;
        }
    } else if (codec == MS_CODEC_OPUS) {
        /* 没有真实 OpusHead 不得捏造，也不能只靠采样率配置硬解。 */
        return MS_ERR_INVALID_ARG;
    } else {
        return MS_ERR_INVALID_ARG;
    }
    if (cfg->sample_rate > 0) {
        rate = cfg->sample_rate;
    }
    if (cfg->channels > 0) {
        ch = cfg->channels;
    }
    if (rate <= 0 || ch <= 0) {
        return MS_ERR_UNSUPPORTED;
    }
    if (a->configured && (a->sample_rate != rate || a->channels != ch ||
                          a->codec != codec)) {
        drop_all_pending(a);
        a->pcm_format_changes++;
    }
    a->codec = codec;
    a->sample_rate = rate;
    a->channels = ch;
    if (spf == 0) {
        spf = audio_default_spf(codec);
    }
    a->samples_per_frame = spf;
    if (delay != 0 && pre == 0) {
        pre = delay;
    }
    a->pre_skip = pre;
    a->codec_delay = delay != 0 ? delay : pre;
    if (spr == 0 && codec == MS_CODEC_OPUS) {
        spr = 3840; /* RFC 7845 默认 80 ms，与 pre-skip 分开 */
    }
    a->seek_preroll = spr;
    a->pending_start_skip = 0;
    a->seek_warmup_left = 0;
    a->preroll_left = 0;
    /* 平台若已把 delay 交给编解码器，本层不再用头部 pre-skip 做 fallback */
    a->start_fallback = a->platform_trims_delay ? 0 : a->pre_skip;
    a->platform_lead_left = a->platform_trims_delay ? a->pre_skip : 0;
    if (cfg->sample_fmt != MS_SAMPLE_FMT_UNKNOWN) {
        a->sample_fmt = cfg->sample_fmt;
    }
    {
        size_t bpf = audio_bpf(a);
        ms_status st;
        /* 与 pending_start_skip=0 对齐：重新配置时不把未用完的 start 前缀带到新流。 */
        ms_hw_pcm_trim_reset(&a->trim);
#ifndef __ANDROID__
        if (a->pending == 0) {
            host_clear_dec_queues(a);
            a->pcm_seq = 0;
        }
#endif
        st = ms_hw_pcm_trim_set_bpf(&a->trim, bpf);
        if (st != MS_OK) {
            return st;
        }
        st = ensure_trim_out(a, (size_t)MS_HW_PCM_TRIM_HOLD_MAX * MS_HW_PCM_TRIM_BPF_MAX);
        if (st != MS_OK) {
            return st;
        }
    }
    return MS_OK;
}

ms_status ms_hw_audio_configure(ms_hw_audio *a, const ms_hw_audio_config *cfg)
{
    ms_status st;
    if (a == NULL || cfg == NULL || cfg->struct_size < sizeof(size_t)) {
        return MS_ERR_INVALID_ARG;
    }
    if (a->canceled) {
        return MS_ERR_CANCELED;
    }
#ifdef __ANDROID__
    st = ms_hw_audio_android_open(a, cfg);
    if (st != MS_OK) {
        a->last_error = st;
        return st;
    }
    st = apply_asc(a, cfg);
    if (st != MS_OK) {
        ms_hw_audio_android_close(a);
        a->last_error = st;
        return st;
    }
#ifdef MS_SINGLE_SO
    if (a->pt_route == MS_PT_ROUTE_IEC) {
        /* IEC61937 轨道按立体声、(E-AC-3 为 4 倍) 采样率计帧，时钟据此换算。 */
        a->sample_rate = cfg->sample_rate * ms_pt_iec_rate_mul(cfg->codec);
        a->channels = 2;
    }
#endif
    a->configured = 1;
    return MS_OK;
#else
    if (a->opts.mock_mode == MS_HW_MOCK_AS_FOREIGN_SW && !a->opts.allow_software) {
        fill_host_query(&a->caps, a->codec, MS_HW_MOCK_AS_FOREIGN_SW, 0, a->aac_policy);
        a->last_error = MS_ERR_UNSUPPORTED;
        return MS_ERR_UNSUPPORTED;
    }
    if (a->opts.mock_mode == MS_HW_MOCK_AS_SW &&
        !((cfg->codec == MS_CODEC_AAC || cfg->codec == MS_CODEC_UNKNOWN) &&
          a->aac_policy == MS_HW_AAC_POLICY_ALLOW_SYSTEM) &&
        !a->opts.allow_software) {
        fill_host_query(&a->caps, (cfg->codec == MS_CODEC_UNKNOWN) ? MS_CODEC_AAC
                                                                  : cfg->codec,
                        MS_HW_MOCK_AS_SW, 0, a->aac_policy);
        a->last_error = MS_ERR_UNSUPPORTED;
        return MS_ERR_UNSUPPORTED;
    }
    if (a->opts.mock_mode == MS_HW_MOCK_DEFAULT) {
        /* 默认查询如实非硬解；生命周期测试请用 MOCK_AS_HW 或显式允许系统 AAC。 */
        fill_host_query(&a->caps, (cfg->codec == MS_CODEC_UNKNOWN) ? MS_CODEC_AAC
                                                                  : cfg->codec,
                        MS_HW_MOCK_DEFAULT, 0, a->aac_policy);
        a->caps.detail = "host mock default refuses to pretend hardware audio output";
        a->last_error = MS_ERR_UNSUPPORTED;
        return MS_ERR_UNSUPPORTED;
    }
    st = apply_asc(a, cfg);
    if (st != MS_OK) {
        a->last_error = st;
        return st;
    }
    fill_host_query(&a->caps, a->codec, a->opts.mock_mode, a->opts.allow_software, a->aac_policy);
    a->configured = 1;
    return MS_OK;
#endif
}

ms_status ms_hw_audio_start(ms_hw_audio *a)
{
    if (a == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    if (a->canceled) {
        return MS_ERR_CANCELED;
    }
    if (!a->configured) {
        return MS_ERR_STATE;
    }
    a->started = 1;
    return MS_OK;
}

ms_status ms_hw_audio_submit_au(ms_hw_audio *a, const ms_hw_audio_au *au)
{
    unsigned i;
    ms_hw_audio_slot *s = NULL;
    size_t pcm;
    const ms_hw_buf *buf;
    unsigned flags;
    int64_t pts_us;
    if (a == NULL || au == NULL || au->struct_size < sizeof(size_t) || au->buf == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    buf = au->buf;
    flags = au->flags;
    pts_us = au->pts_us;
    if (a->canceled) {
        return MS_ERR_CANCELED;
    }
    if (!a->configured || !a->started) {
        a->last_error = MS_ERR_STATE;
        return MS_ERR_STATE;
    }
    if (a->pending >= a->in_cap) {
        a->backpressure++;
        return MS_LIMIT;
    }
    for (i = 0; i < a->in_cap; i++) {
        if (!a->slots[i].occupied) {
            s = &a->slots[i];
            break;
        }
    }
    if (s == NULL) {
        a->backpressure++;
        return MS_LIMIT;
    }
    {
        uint32_t spf = a->samples_per_frame;
        if (a->codec == MS_CODEC_PCM && buf->size > 0) {
            size_t bpf = (size_t)(a->channels > 0 ? a->channels : 1) *
                         (size_t)((a->pcm_bits ? a->pcm_bits : 16) / 8);
            pcm = buf->size;
            s->samples = bpf ? (uint32_t)(pcm / bpf) : 0;
        } else {
            if (spf == 0) {
                spf = audio_default_spf(a->codec);
            }
            if (buf->data != NULL && buf->size > 0) {
                ms_audio_au_info info;
                memset(&info, 0, sizeof(info));
                info.struct_size = sizeof(info);
                if (ms_audio_au_inspect(a->codec, a->sample_fmt, buf->data, buf->size,
                                        &info) == MS_OK && info.samples > 0) {
                    spf = info.samples;
                }
            }
            s->samples = spf;
            pcm = (size_t)spf * (size_t)a->channels * (size_t)(a->pcm_bits / 8);
        }
    }
    s->data = (uint8_t *)(void *)(uintptr_t)buf->data;
    s->size = buf->size;
    s->release_fn = buf->release_fn;
    s->release_ctx = buf->release_ctx;
    s->pts_us = pts_us;
    s->has_pts = 1;
    if (au->struct_size >= offsetof(ms_hw_audio_au, has_pts) + sizeof(int)) {
        s->has_pts = au->has_pts ? 1 : 0;
    }
    s->flags = flags;
    s->pcm_bytes = pcm;
    s->pcm_done = 0;
    s->occupied = 1;
    s->skip_start = 0;
    s->skip_end = 0;
    if (au->struct_size >= offsetof(ms_hw_audio_au, skip_samples_end) + sizeof(uint32_t)) {
        s->skip_start = au->skip_samples_start;
        s->skip_end = au->skip_samples_end;
    }
    a->pending++;
    a->au_in++;
    a->au_submitted++;
    a->bytes_queued += pcm;
    return MS_OK;
}

ms_status ms_hw_audio_submit(ms_hw_audio *a, const ms_hw_buf *buf, int64_t pts_us,
                             unsigned flags)
{
    ms_hw_audio_au au;
    memset(&au, 0, sizeof(au));
    au.struct_size = sizeof(au);
    au.buf = buf;
    au.pts_us = pts_us;
    au.has_pts = 1;
    au.flags = flags;
    return ms_hw_audio_submit_au(a, &au);
}

ms_status ms_hw_audio_note_seek(ms_hw_audio *a, int64_t media_us)
{
    if (a == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    if (a->canceled) {
        return MS_ERR_CANCELED;
    }
    a->pending_start_skip = 0;
    if (media_us <= 0) {
        a->start_fallback = a->platform_trims_delay ? 0 : a->pre_skip;
        a->seek_warmup_left = 0;
        a->platform_lead_left = a->platform_trims_delay ? a->pre_skip : 0;
    } else {
        a->start_fallback = 0;
        a->seek_warmup_left = a->platform_trims_delay ? 0 : a->seek_preroll;
        a->platform_lead_left = a->platform_trims_delay ? a->seek_preroll : 0;
    }
    a->preroll_left = a->start_fallback + a->seek_warmup_left;
    return MS_OK;
}


/* 宿主 mock 额度单位是 PCM 字节（16-bit * 声道 * 采样），不是压缩 AU 字节。设备路径忽略本函数。 */
ms_status ms_hw_audio_add_write_quota(ms_hw_audio *a, size_t pcm_bytes)
{
    if (a == NULL) {
        return MS_ERR_INVALID_ARG;
    }
#ifdef __ANDROID__
    (void)pcm_bytes;
    return MS_OK; /* 设备路径由 AudioTrack 自身反压，忽略测试配额 */
#else
    if (!a->quota_armed) {
        a->write_quota = 0;
        a->quota_armed = 1;
    }
    a->write_quota += pcm_bytes;
    return MS_OK;
#endif
}

ms_status ms_hw_audio_drive(ms_hw_audio *a)
{
    if (a == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    if (a->canceled) {
        return MS_ERR_CANCELED;
    }
    if (a->paused) {
        return MS_OK;
    }
#ifdef __ANDROID__
    return ms_hw_audio_android_drive(a);
#else
    unsigned i;
    ms_status st;
    if (!a->started) {
        return MS_OK;
    }
    /* 延迟输出：本轮不解码，模拟 MediaCodec 尚未产出。 */
    if (a->dec_mode == MS_HW_AUDIO_DEC_DELAY && a->dec_delay_left > 0) {
        a->dec_delay_left--;
    } else {
        for (i = 0; i < a->in_cap; i++) {
            if (!a->slots[i].occupied || a->slots[i].decoded) {
                continue;
            }
            st = decode_slot(a, i);
            if (st == MS_LIMIT) {
                break;
            }
            if (st != MS_OK) {
                a->last_error = st;
                return st;
            }
        }
        /* EOS 时冲出不足 merge_n 的剩余合并缓冲。 */
        if (a->eos_in && a->merge_got > 0) {
            st = push_dout(a, a->merge_buf, a->merge_nsamp, a->merge_has_pts,
                           a->merge_pts_us, a->merge_mask);
            a->merge_nsamp = 0;
            a->merge_got = 0;
            a->merge_mask = 0;
            if (st != MS_OK) {
                a->last_error = st;
                return st;
            }
        }
    }
    /* 先喂入已解码块到共享修剪机，再按配额写已修剪可听 PCM。 */
    while (a->dout_n > 0) {
        st = feed_one_dout(a);
        if (st != MS_OK) {
            a->last_error = st;
            return st;
        }
    }
    if (a->eos_in && a->pending == 0 && a->dout_n == 0 && a->merge_got == 0) {
        size_t out_bytes = 0;
        st = ensure_trim_out(a, (size_t)MS_HW_PCM_TRIM_HOLD_MAX * MS_HW_PCM_TRIM_BPF_MAX);
        if (st != MS_OK) {
            a->last_error = st;
            return st;
        }
        st = ms_hw_pcm_trim_eos(&a->trim, a->trim_out, a->trim_out_cap, &out_bytes);
        if (st != MS_OK) {
            a->last_error = st;
            return st;
        }
        if (out_bytes > 0) {
            st = append_aud(a, a->trim_out, out_bytes, 0);
            if (st != MS_OK) {
                a->last_error = st;
                return st;
            }
            audio_tap_trimmed(a, a->trim_out, out_bytes);
        }
    }
    st = write_leftover(a);
    if (st != MS_OK) {
        a->last_error = st;
        return st;
    }
    recount_queued(a);
    if (a->eos_in && a->pending == 0 && a->dout_n == 0 &&
        a->aud_left_n <= a->aud_left_off && a->merge_got == 0) {
        a->eos_out = 1;
    }
    return MS_OK;
#endif
}

ms_status ms_hw_audio_signal_eos(ms_hw_audio *a)
{
    if (a == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    if (a->canceled) {
        return MS_ERR_CANCELED;
    }
    a->eos_in = 1;
    return ms_hw_audio_drive(a);
}

ms_status ms_hw_audio_flush(ms_hw_audio *a)
{
    if (a == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    if (a->canceled) {
        return MS_ERR_CANCELED;
    }
    drop_all_pending(a);
    a->eos_in = 0;
    a->eos_out = 0;
    a->flushed = 1;
    a->clock_us = 0;
    a->samples_played = 0;
    a->au_played = 0;
    a->pending_start_skip = 0;
    a->seek_warmup_left = 0;
    a->platform_lead_left = 0;
    /* 不恢复已消耗的 start_fallback；未消耗的（刚 configure）保留。
     * 中途 seek 的 warmup 由随后的 note_seek 重新武装，避免一次 flush 把剩余预热带到非 seek 路径。 */
    a->preroll_left = a->start_fallback;
    /* flush 后恢复为默认不限额；下一次 add_write_quota 再重新武装。 */
    a->write_quota = 0;
    a->quota_armed = 0;
    a->clock_fallback_origin_ns = 0;
    a->clock_mono_last_change_ns = 0;
    a->clock_last_raw_us = 0;
    a->clock_emitted_us = 0;
    a->sink_last_accept_ns = 0;
#ifdef __ANDROID__
    ms_hw_audio_android_flush(a);
#endif
    return MS_OK;
}

ms_status ms_hw_audio_cancel(ms_hw_audio *a)
{
    if (a == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    a->canceled = 1;
    drop_all_pending(a);
#ifdef __ANDROID__
    ms_hw_audio_android_close(a);
#endif
    return MS_OK;
}

ms_status ms_hw_audio_get_caps(const ms_hw_audio *a, ms_hw_audio_caps *out)
{
    size_t sz;
    if (a == NULL || out == NULL || out->struct_size < sizeof(size_t)) {
        return MS_ERR_INVALID_ARG;
    }
    sz = out->struct_size;
    if (sz > sizeof(*out)) {
        sz = sizeof(*out);
    }
    memcpy(out, &a->caps, sz);
    out->struct_size = sz;
    return MS_OK;
}

ms_status ms_hw_audio_get_stats(const ms_hw_audio *a, ms_hw_audio_stats *out)
{
    ms_hw_audio_stats tmp;
    size_t sz;
    if (a == NULL || out == NULL || out->struct_size < sizeof(size_t)) {
        return MS_ERR_INVALID_ARG;
    }
    memset(&tmp, 0, sizeof(tmp));
    tmp.struct_size = sizeof(tmp);
    tmp.au_in = a->au_in;
    tmp.au_submitted = a->au_submitted;
    tmp.au_played = a->au_played;
    tmp.samples_played = a->samples_played;
    tmp.clock_us = a->clock_us;
#ifdef __ANDROID__
    {
        int64_t fr = ms_hw_audio_android_played_frames(a);
        if (fr >= 0 && a->sample_rate > 0) {
            tmp.samples_played = (uint64_t)fr;
            tmp.clock_us = (fr * 1000000ll) / a->sample_rate;
            tmp.au_played = (uint64_t)fr / (a->samples_per_frame
                                               ? a->samples_per_frame
                                               : audio_default_spf(a->codec));
        }
    }
#endif
    tmp.bytes_queued = a->bytes_queued;
    tmp.partial_writes = a->partial_writes;
    tmp.backpressure = a->backpressure;
    tmp.sample_rate = a->sample_rate;
    tmp.channels = a->channels;
    tmp.pcm_bits = a->pcm_bits;
    tmp.pcm_format_changes = a->pcm_format_changes;
    tmp.configured = a->configured;
    tmp.started = a->started;
    tmp.eos_in = a->eos_in;
    tmp.eos_out = a->eos_out;
    tmp.flushed = a->flushed;
    tmp.canceled = a->canceled;
    tmp.pending = a->pending;
    tmp.is_hardware = a->caps.is_hardware;
    tmp.system_decode_is_not_hardware = a->caps.system_decode_is_not_hardware;
    tmp.verified = a->caps.verified;
    tmp.kind = a->caps.kind;
    tmp.backend = a->caps.backend;
    tmp.detail = a->caps.detail;
    tmp.last_error = a->last_error;
    tmp.paused = a->paused;
    tmp.rate_num = a->rate_num > 0 ? a->rate_num : 1;
    tmp.rate_den = a->rate_den > 0 ? a->rate_den : 1;
    tmp.track_write_bytes = a->track_write_bytes;
    tmp.track_write_frames = a->track_write_frames;
    tmp.track_head_frames = -1;
#ifdef __ANDROID__
    {
        int64_t fr = ms_hw_audio_android_played_frames(a);
        if (fr >= 0) {
            tmp.track_head_frames = fr;
        }
    }
#endif
    {
        int64_t clk = 0;
        if (ms_hw_audio_get_clock_us(a, &clk) == MS_OK) {
            tmp.clock_us = clk;
        }
    }
    sz = out->struct_size;
    if (sz > sizeof(tmp)) {
        sz = sizeof(tmp);
    }
    memcpy(out, &tmp, sz);
    out->struct_size = sz;
    return MS_OK;
}

#define MS_HW_AUDIO_CLOCK_STALL_NS 200000000ll /* 200ms 头/钟不动则兜底 */

static int64_t audio_mono_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (int64_t)ts.tv_sec * 1000000000ll + (int64_t)ts.tv_nsec;
}

static int64_t audio_clock_hold_emitted(ms_hw_audio *a, int64_t cand)
{
    if (a->clock_emitted_us > 0 && cand < a->clock_emitted_us) {
        return a->clock_emitted_us;
    }
    if (cand > a->clock_emitted_us) {
        a->clock_emitted_us = cand;
    }
    return cand;
}

/* raw_us：原始播放头（只用来判定输出是否停滞）；est_us：要发出的时钟值
 * （Android 上为 Media3 式时间戳/平滑估算，其他平台与 raw_us 相同）。 */
static int64_t audio_clock_with_fallback(const ms_hw_audio *ca, int64_t raw_us, int64_t est_us)
{
    ms_hw_audio *a = (ms_hw_audio *)(void *)ca;
    int64_t now;
    int64_t out;
    if (a == NULL) {
        return raw_us;
    }
    if (a->paused || a->quota_armed) {
        a->clock_fallback_origin_ns = 0;
        /* 暂停/配额路径不以墙钟前进，但仍不得低于已发出的兜底钟。 */
        return audio_clock_hold_emitted(a, est_us);
    }
    now = audio_mono_ns();
    if (now <= 0) {
        return audio_clock_hold_emitted(a, est_us);
    }
    if (raw_us != a->clock_last_raw_us) {
        a->clock_last_raw_us = raw_us;
        a->clock_mono_last_change_ns = now;
        if (est_us >= a->clock_emitted_us) {
            a->clock_fallback_origin_ns = 0;
            a->clock_emitted_us = est_us;
            return est_us;
        }
        /* head 刚动但仍落后于已发出的兜底钟：保持单调，直到 head 追上。 */
        return a->clock_emitted_us;
    }
    if (a->clock_mono_last_change_ns == 0) {
        a->clock_mono_last_change_ns = now;
        return audio_clock_hold_emitted(a, est_us);
    }
    if (now - a->clock_mono_last_change_ns < MS_HW_AUDIO_CLOCK_STALL_NS) {
        /* head 未满 200ms 不动：Media3 的平滑估算本身随系统时钟走，直接用估算值。 */
        return audio_clock_hold_emitted(a, est_us);
    }
    /* 输出设备停转（如 AVD -no-audio）：用单调时钟兜底，让呈现/位置继续前进。
     * origin 锚在 last_change，使第一次兜底读数包含已停转的时段，而不是从 raw 再起跳。 */
    if (a->clock_fallback_origin_ns == 0) {
        int64_t base = a->clock_mono_last_change_ns;
        if (base <= 0 || base > now) {
            base = now;
        }
        a->clock_fallback_origin_ns = base - raw_us * 1000ll;
    }
    a->clock_in_fallback = 1;
    {
        int64_t wall = (now - a->clock_fallback_origin_ns) / 1000ll;
        out = wall > raw_us ? wall : raw_us;
        return audio_clock_hold_emitted(a, out);
    }
}

ms_status ms_hw_audio_get_clock_us(const ms_hw_audio *a, int64_t *clock_us)
{
    int64_t raw = 0;
    if (a == NULL || clock_us == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    ((ms_hw_audio *)(void *)a)->clock_in_fallback = 0;
#ifdef __ANDROID__
    {
        int64_t est = 0;
        if (ms_hw_audio_android_position_us(a, &est, &raw) == 0) {
            *clock_us = audio_clock_with_fallback(a, raw, est);
            return MS_OK;
        }
    }
#endif
    raw = a->clock_us;
    *clock_us = audio_clock_with_fallback(a, raw, raw);
    return MS_OK;
}

void ms_hw_audio_sync_info(const ms_hw_audio *a, int *src, int64_t *latency_us)
{
    if (src) {
        *src = 0;
    }
    if (latency_us) {
        *latency_us = 0;
    }
    if (a == NULL) {
        return;
    }
#ifdef __ANDROID__
    ms_hw_audio_android_sync_info(a, src, latency_us);
#endif
    if (a->clock_in_fallback && src) {
        *src = 4;
    }
}

void ms_hw_audio_debug_backdate_clock_change(ms_hw_audio *a, int64_t ns_ago)
{
    int64_t now;
    if (a == NULL || ns_ago <= 0) {
        return;
    }
    now = audio_mono_ns();
    if (now <= 0) {
        return;
    }
    if (ns_ago > now) {
        ns_ago = now;
    }
    a->clock_mono_last_change_ns = now - ns_ago;
}

void ms_hw_audio_debug_force_emitted_clock(ms_hw_audio *a, int64_t emitted_us)
{
    if (a == NULL) {
        return;
    }
    if (emitted_us < 0) {
        emitted_us = 0;
    }
    a->clock_emitted_us = emitted_us;
}

int64_t ms_hw_audio_debug_mono_ns(void)
{
    return audio_mono_ns();
}

void ms_hw_audio_debug_clock_info(const ms_hw_audio *a, int *paused,
                                  int *quota_armed, int64_t *mono_ns,
                                  int64_t *last_raw_us, int64_t *last_chg_ns,
                                  int64_t *emitted_us, int64_t *origin_ns)
{
    if (paused != NULL) {
        *paused = (a != NULL) ? a->paused : -1;
    }
    if (quota_armed != NULL) {
        *quota_armed = (a != NULL) ? a->quota_armed : -1;
    }
    if (mono_ns != NULL) {
        *mono_ns = audio_mono_ns();
    }
    if (last_raw_us != NULL) {
        *last_raw_us = (a != NULL) ? a->clock_last_raw_us : 0;
    }
    if (last_chg_ns != NULL) {
        *last_chg_ns = (a != NULL) ? a->clock_mono_last_change_ns : 0;
    }
    if (emitted_us != NULL) {
        *emitted_us = (a != NULL) ? a->clock_emitted_us : 0;
    }
    if (origin_ns != NULL) {
        *origin_ns = (a != NULL) ? a->clock_fallback_origin_ns : 0;
    }
}

#ifdef __ANDROID__
#include "ms_hw_jni.h"
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#include "ms_hw_mediandk.h"
#include "ms_hw_api.h"
#include "ms_hw_bionic18.h"
#include <jni.h>
#include <android/api-level.h>
#include <errno.h>
#include <pthread.h>

#ifndef MS_HW_AUDIO_MIME
#define MS_HW_AUDIO_MIME "audio/mp4a-latm"
#endif

/* 系统音频解码放行范围：多 so 官方构建保持「只放行 AAC」；单 so 放开到系统常见音频
 * （AAC/MP3/FLAC/Opus/AC-3/E-AC-3/MP2），有硬解用硬解，没有则走系统软解。视频仍只硬解。 */
#ifdef MS_SINGLE_SO
#define MS_SYS_AUDIO_OK(c) ((c) == MS_CODEC_AAC || (c) == MS_CODEC_MP3 || (c) == MS_CODEC_FLAC || \
                            (c) == MS_CODEC_OPUS || (c) == MS_CODEC_AC3 || (c) == MS_CODEC_EAC3 || \
                            (c) == MS_CODEC_MP2 || (c) == MS_CODEC_G711_ALAW || \
                            (c) == MS_CODEC_G711_ULAW || (c) == MS_CODEC_DTS)
#else
#define MS_SYS_AUDIO_OK(c) ((c) == MS_CODEC_AAC)
#endif
#ifdef MS_SINGLE_FFADEC
#include "ms_hw_ffadec.h"
#endif

typedef struct audio_plat {
    AMediaCodec *codec;
    jobject track; /* global AudioTrack */
    float vol_left;
    float vol_right;
    int vol_set;
    void *ffadec; /* 单 so 路线 B：AC-3/E-AC-3/MP2 FFmpeg 软解（系统无解码器时） */
#ifdef MS_SINGLE_SO
    ms_pt_iec iec; /* 直通 IEC61937 封装状态 */
    int iec_on;
    int pt_left_frames; /* 直通：pcm_left 写完时再计入的 PCM 帧数 */
#endif
    int track_ready;
    int input_eos;
    uint32_t head_raw;   /* 上次 getPlaybackHeadPosition，按无符号 32 位 */
    uint32_t head_wraps; /* 2^32 帧回绕次数 */
    int head_valid;
    uint8_t *pcm_left;   /* 部分 write 剩余 PCM 字节，不是压缩 AU */
    size_t pcm_left_off;
    size_t pcm_left_n;
    size_t pcm_left_cap;
    /* 独立写线程：goldfish / -no-audio 可能忽略 WRITE_NON_BLOCKING，
     * 不能在 drive 线程里直接调用 AudioTrack.write。 */
    pthread_mutex_t wmu;
    pthread_cond_t wcv;
    pthread_t wth;
    int w_inited;
    int w_started;
    int w_stop;
    int w_busy;
    int w_unclaimed;
    unsigned w_gen;
    unsigned w_req_gen;
    int w_n;
    int w_wr;
    uint8_t w_buf[8192];
    /* API 18–20：Java MediaCodec（无 libmediandk）。API 21+ 用上面的 AMediaCodec*。 */
    int jni_mode;
    jobject jcodec;
    jobject j_in_bufs;
    jobject j_out_bufs;
    jobject j_bufinfo;
    /* Media3 AudioTrackPositionTracker / AudioTimestampPoller 精简移植的状态（见 at_* 函数）。 */
    int at_sdk;
    int at_buf_bytes;     /* AudioTrack 缓冲字节（getMinBufferSize 结果） */
    int at_frame_bytes;   /* PCM 每帧字节；直通为 0（Media3 非 PCM 不扣延迟） */
    int64_t at_wr_frames; /* 自上次 flush/新建以来写入帧数，位置上限 */
    jmethodID at_m_ts;
    jmethodID at_m_lat;
    int at_lat_dead;      /* getLatency 不存在或调用失败，不再尝试 */
    jobject at_ts_obj;    /* 全局引用 android.media.AudioTimestamp */
    jfieldID at_f_frame;
    jfieldID at_f_nano;
    int at_ts_dead;
    int at_state;
    int64_t at_init_sys_us, at_last_poll_us, at_interval_us;
    int64_t at_init_frames, at_init_ts_sys_us;
    int64_t at_ts_frames, at_ts_sys_us, at_ts_raw_last, at_ts_wraps;
    int64_t at_ph_off[10];
    int at_ph_n, at_ph_i;
    int64_t at_ph_last_us, at_ph_smooth;
    int64_t at_lat_us, at_lat_last_us;
    int64_t at_last_sys_us, at_last_pos_us;
    int at_src;
} audio_plat;

#define MS_HW_AUDIO_PCM_LEFT_MAX ((size_t)64 * 1024)

static void plat_clear_pcm_left(audio_plat *plat)
{
    if (plat == NULL) {
        return;
    }
    free(plat->pcm_left);
    plat->pcm_left = NULL;
    plat->pcm_left_off = 0;
    plat->pcm_left_n = 0;
    plat->pcm_left_cap = 0;
}

static void at_reset(audio_plat *plat, int all);

static void plat_reset_head(audio_plat *plat)
{
    if (plat == NULL) {
        return;
    }
    plat->head_raw = 0;
    plat->head_wraps = 0;
    plat->head_valid = 0;
    plat->at_wr_frames = 0;
    at_reset(plat, 1);
}

static void plat_writer_stop(audio_plat *plat);
static void plat_writer_destroy(audio_plat *plat);
static void plat_writer_drop(audio_plat *plat);

static JavaVM *audio_vm(void)
{
    /* 复用 JNI_OnLoad 里登记的 VM：经 ms_hw_jni_env 取 JNIEnv。 */
    return NULL;
}

ms_status ms_hw_audio_android_query(ms_codec codec, int allow_sw, int api_level,
                                    ms_hw_audio_caps *out, char *name, size_t name_cap,
                                    char *detail, size_t detail_cap)
{
    ms_hw_kind kind = MS_HW_KIND_NONE;
    ms_status st;
    int api = api_level;
    if (out == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    out->struct_size = sizeof(*out);
    out->codec = (codec == MS_CODEC_UNKNOWN) ? MS_CODEC_AAC : codec;
    out->backend = "mediacodec-audiotrack";
    out->verified = 1;
    if (!audio_codec_supported(codec)) {
        out->backend = "none";
        out->detail = "audio codec not in hardware path scope";
        return MS_ERR_UNSUPPORTED;
    }
    st = ms_hw_jni_pick_audio(out->codec, api, allow_sw, 0, name, name_cap, &kind,
                              &api, detail, detail_cap);
    out->api_level = api;
    out->kind = kind;
    out->codec_name = (name && name[0]) ? name : "";
    out->detail = (detail && detail[0]) ? detail : out->detail;
    if (st != MS_OK) {
        out->is_hardware = 0;
        out->system_decode_is_not_hardware = (kind == MS_HW_KIND_SOFTWARE ||
                                              kind == MS_HW_KIND_SYSTEM_UNVERIFIED);
        if (kind == MS_HW_KIND_SOFTWARE) {
            out->detail = detail && detail[0]
                              ? detail
                              : "no hardware audio decoder, software refused";
        }
        return MS_ERR_UNSUPPORTED;
    }
    out->is_hardware = (kind == MS_HW_KIND_HARDWARE);
    out->system_decode_is_not_hardware = !out->is_hardware;
    if (kind != MS_HW_KIND_HARDWARE && !allow_sw) {
        out->is_hardware = 0;
        out->system_decode_is_not_hardware = 1;
        out->detail = "no hardware audio decoder, software refused";
        return MS_ERR_UNSUPPORTED;
    }
    return MS_OK;
}

/* --- API 18–20 JNI MediaCodec（音频 AAC 等）--- */
static void audio_jni_clear_exc(JNIEnv *env)
{
    if (env != NULL && (*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
    }
}

static void audio_jni_drop_codec(audio_plat *plat)
{
    JNIEnv *env = NULL;
    if (plat == NULL) {
        return;
    }
    if (ms_hw_jni_env(&env) && env != NULL) {
        if (plat->jcodec != NULL) {
            jclass cl = (*env)->GetObjectClass(env, plat->jcodec);
            jmethodID rel = cl ? (*env)->GetMethodID(env, cl, "release", "()V") : NULL;
            if (rel) {
                (*env)->CallVoidMethod(env, plat->jcodec, rel);
                audio_jni_clear_exc(env);
            }
            if (cl) {
                (*env)->DeleteLocalRef(env, cl);
            }
            (*env)->DeleteGlobalRef(env, plat->jcodec);
        }
        if (plat->j_in_bufs) {
            (*env)->DeleteGlobalRef(env, plat->j_in_bufs);
        }
        if (plat->j_out_bufs) {
            (*env)->DeleteGlobalRef(env, plat->j_out_bufs);
        }
        if (plat->j_bufinfo) {
            (*env)->DeleteGlobalRef(env, plat->j_bufinfo);
        }
    }
    plat->jcodec = NULL;
    plat->j_in_bufs = NULL;
    plat->j_out_bufs = NULL;
    plat->j_bufinfo = NULL;
    plat->jni_mode = 0;
}

static int audio_jni_refresh_bufs(audio_plat *plat, JNIEnv *env, int which)
{
    jclass cl;
    jmethodID mid;
    jobject arr;
    if (plat == NULL || env == NULL || plat->jcodec == NULL) {
        return -1;
    }
    cl = (*env)->GetObjectClass(env, plat->jcodec);
    if (!cl) {
        return -1;
    }
    /* getInputBuffers / getOutputBuffers：API 16–20 主路径；21 起已弃用，本函数只在 JNI 模式调用。 */
    mid = (*env)->GetMethodID(env, cl,
                              (which == 0) ? "getInputBuffers" : "getOutputBuffers",
                              "()[Ljava/nio/ByteBuffer;");
    if (!mid) {
        audio_jni_clear_exc(env);
        (*env)->DeleteLocalRef(env, cl);
        return -1;
    }
    arr = (*env)->CallObjectMethod(env, plat->jcodec, mid);
    audio_jni_clear_exc(env);
    (*env)->DeleteLocalRef(env, cl);
    if (arr == NULL) {
        return -1;
    }
    if (which == 0) {
        if (plat->j_in_bufs) {
            (*env)->DeleteGlobalRef(env, plat->j_in_bufs);
        }
        plat->j_in_bufs = (*env)->NewGlobalRef(env, arr);
    } else {
        if (plat->j_out_bufs) {
            (*env)->DeleteGlobalRef(env, plat->j_out_bufs);
        }
        plat->j_out_bufs = (*env)->NewGlobalRef(env, arr);
    }
    (*env)->DeleteLocalRef(env, arr);
    return 0;
}

static int audio_jni_open(ms_hw_audio *a, audio_plat *plat, const char *name,
                          const ms_hw_audio_config *cfg)
{
    JNIEnv *env = NULL;
    jclass ccl, fcl, bicl;
    jmethodID create, cfgm, start, fmt_new, set_int, set_buf, bi_ctor;
    jstring jn, jmime, jk;
    jobject codec, fmt, csd, bi;
    const char *mime;
    if (!ms_hw_jni_env(&env) || env == NULL || name == NULL || plat == NULL || cfg == NULL) {
        return -1;
    }
    ccl = (*env)->FindClass(env, "android/media/MediaCodec");
    fcl = (*env)->FindClass(env, "android/media/MediaFormat");
    bicl = (*env)->FindClass(env, "android/media/MediaCodec$BufferInfo");
    if (!ccl || !fcl || !bicl) {
        audio_jni_clear_exc(env);
        return -1;
    }
    create = (*env)->GetStaticMethodID(env, ccl, "createByCodecName",
                                       "(Ljava/lang/String;)Landroid/media/MediaCodec;");
    cfgm = (*env)->GetMethodID(env, ccl, "configure",
        "(Landroid/media/MediaFormat;Landroid/view/Surface;Landroid/media/MediaCrypto;I)V");
    start = (*env)->GetMethodID(env, ccl, "start", "()V");
    fmt_new = (*env)->GetStaticMethodID(env, fcl, "createAudioFormat",
        "(Ljava/lang/String;II)Landroid/media/MediaFormat;");
    set_int = (*env)->GetMethodID(env, fcl, "setInteger", "(Ljava/lang/String;I)V");
    set_buf = (*env)->GetMethodID(env, fcl, "setByteBuffer",
                                  "(Ljava/lang/String;Ljava/nio/ByteBuffer;)V");
    bi_ctor = (*env)->GetMethodID(env, bicl, "<init>", "()V");
    if (!create || !cfgm || !start || !fmt_new || !set_int || !set_buf || !bi_ctor) {
        audio_jni_clear_exc(env);
        return -1;
    }
    mime = ms_hw_audio_mime(cfg->codec == MS_CODEC_UNKNOWN ? MS_CODEC_AAC : cfg->codec);
    if (mime == NULL) {
        return -1;
    }
    jn = (*env)->NewStringUTF(env, name);
    codec = (*env)->CallStaticObjectMethod(env, ccl, create, jn);
    (*env)->DeleteLocalRef(env, jn);
    if (!codec || (*env)->ExceptionCheck(env)) {
        audio_jni_clear_exc(env);
        return -1;
    }
    jmime = (*env)->NewStringUTF(env, mime);
    fmt = (*env)->CallStaticObjectMethod(env, fcl, fmt_new, jmime,
                                         (jint)(cfg->sample_rate > 0 ? cfg->sample_rate : 48000),
                                         (jint)(cfg->channels > 0 ? cfg->channels : 2));
    (*env)->DeleteLocalRef(env, jmime);
    if (!fmt || (*env)->ExceptionCheck(env)) {
        audio_jni_clear_exc(env);
        (*env)->DeleteLocalRef(env, codec);
        return -1;
    }
    if (cfg->asc != NULL && cfg->asc_size > 0) {
        csd = (*env)->NewDirectByteBuffer(env, (void *)cfg->asc, (jlong)cfg->asc_size);
        jk = (*env)->NewStringUTF(env, "csd-0");
        if (csd && jk) {
            (*env)->CallVoidMethod(env, fmt, set_buf, jk, csd);
            audio_jni_clear_exc(env);
        }
        if (jk) {
            (*env)->DeleteLocalRef(env, jk);
        }
        if (csd) {
            (*env)->DeleteLocalRef(env, csd);
        }
    }
    (*env)->CallVoidMethod(env, codec, cfgm, fmt, NULL, NULL, 0);
    (*env)->DeleteLocalRef(env, fmt);
    if ((*env)->ExceptionCheck(env)) {
        audio_jni_clear_exc(env);
        (*env)->DeleteLocalRef(env, codec);
        return -1;
    }
    (*env)->CallVoidMethod(env, codec, start);
    if ((*env)->ExceptionCheck(env)) {
        audio_jni_clear_exc(env);
        (*env)->DeleteLocalRef(env, codec);
        return -1;
    }
    bi = (*env)->NewObject(env, bicl, bi_ctor);
    plat->jni_mode = 1;
    plat->jcodec = (*env)->NewGlobalRef(env, codec);
    plat->j_bufinfo = bi ? (*env)->NewGlobalRef(env, bi) : NULL;
    if (bi) {
        (*env)->DeleteLocalRef(env, bi);
    }
    (*env)->DeleteLocalRef(env, codec);
    (void)audio_jni_refresh_bufs(plat, env, 0);
    (void)audio_jni_refresh_bufs(plat, env, 1);
    (void)a;
    return (plat->jcodec && plat->j_bufinfo && plat->j_in_bufs) ? 0 : -1;
}

static ssize_t audio_mc_dq_in(audio_plat *plat, int64_t timeout_us)
{
    JNIEnv *env = NULL;
    jclass cl;
    jmethodID mid;
    jint idx;
    if (plat == NULL) {
        return -1;
    }
    if (!plat->jni_mode) {
        return AMediaCodec_dequeueInputBuffer(plat->codec, timeout_us);
    }
    if (!ms_hw_jni_env(&env) || env == NULL || plat->jcodec == NULL) {
        return -1;
    }
    cl = (*env)->GetObjectClass(env, plat->jcodec);
    mid = cl ? (*env)->GetMethodID(env, cl, "dequeueInputBuffer", "(J)I") : NULL;
    if (!mid) {
        audio_jni_clear_exc(env);
        if (cl) {
            (*env)->DeleteLocalRef(env, cl);
        }
        return -1;
    }
    idx = (*env)->CallIntMethod(env, plat->jcodec, mid, (jlong)timeout_us);
    audio_jni_clear_exc(env);
    (*env)->DeleteLocalRef(env, cl);
    return (ssize_t)idx;
}

static uint8_t *audio_mc_get_in(audio_plat *plat, size_t idx, size_t *cap)
{
    JNIEnv *env = NULL;
    jobject buf;
    void *p;
    jlong c;
    if (plat == NULL) {
        return NULL;
    }
    if (!plat->jni_mode) {
        return AMediaCodec_getInputBuffer(plat->codec, idx, cap);
    }
    if (!ms_hw_jni_env(&env) || env == NULL || plat->j_in_bufs == NULL) {
        return NULL;
    }
    buf = (*env)->GetObjectArrayElement(env, plat->j_in_bufs, (jint)idx);
    if (!buf) {
        audio_jni_clear_exc(env);
        return NULL;
    }
    p = (*env)->GetDirectBufferAddress(env, buf);
    c = (*env)->GetDirectBufferCapacity(env, buf);
    (*env)->DeleteLocalRef(env, buf);
    if (cap) {
        *cap = (c > 0) ? (size_t)c : 0;
    }
    return (uint8_t *)p;
}

static media_status_t audio_mc_q_in(audio_plat *plat, size_t idx, off_t off, size_t size,
                                    uint64_t time, uint32_t flags)
{
    JNIEnv *env = NULL;
    jclass cl;
    jmethodID mid;
    if (plat == NULL) {
        return AMEDIA_ERROR_UNKNOWN;
    }
    if (!plat->jni_mode) {
        return AMediaCodec_queueInputBuffer(plat->codec, idx, off, size, time, flags);
    }
    if (!ms_hw_jni_env(&env) || env == NULL || plat->jcodec == NULL) {
        return AMEDIA_ERROR_UNKNOWN;
    }
    cl = (*env)->GetObjectClass(env, plat->jcodec);
    mid = cl ? (*env)->GetMethodID(env, cl, "queueInputBuffer", "(IIIJI)V") : NULL;
    if (!mid) {
        audio_jni_clear_exc(env);
        if (cl) {
            (*env)->DeleteLocalRef(env, cl);
        }
        return AMEDIA_ERROR_UNKNOWN;
    }
    /* Java：queueInputBuffer(index, offset, size, presentationTimeUs, flags) */
    (*env)->CallVoidMethod(env, plat->jcodec, mid, (jint)idx, (jint)off, (jint)size,
                           (jlong)time, (jint)flags);
    audio_jni_clear_exc(env);
    (*env)->DeleteLocalRef(env, cl);
    return AMEDIA_OK;
}

static ssize_t audio_mc_dq_out(audio_plat *plat, AMediaCodecBufferInfo *info, int64_t timeout_us)
{
    JNIEnv *env = NULL;
    jclass cl, bicl;
    jmethodID mid;
    jint idx;
    if (plat == NULL || info == NULL) {
        return -1;
    }
    if (!plat->jni_mode) {
        return AMediaCodec_dequeueOutputBuffer(plat->codec, info, timeout_us);
    }
    if (!ms_hw_jni_env(&env) || env == NULL || plat->jcodec == NULL || plat->j_bufinfo == NULL) {
        return -1;
    }
    cl = (*env)->GetObjectClass(env, plat->jcodec);
    mid = cl ? (*env)->GetMethodID(env, cl, "dequeueOutputBuffer",
                                   "(Landroid/media/MediaCodec$BufferInfo;J)I") : NULL;
    if (!mid) {
        audio_jni_clear_exc(env);
        if (cl) {
            (*env)->DeleteLocalRef(env, cl);
        }
        return -1;
    }
    idx = (*env)->CallIntMethod(env, plat->jcodec, mid, plat->j_bufinfo, (jlong)timeout_us);
    audio_jni_clear_exc(env);
    (*env)->DeleteLocalRef(env, cl);
    if (idx >= 0) {
        bicl = (*env)->GetObjectClass(env, plat->j_bufinfo);
        if (bicl) {
            jfieldID fo = (*env)->GetFieldID(env, bicl, "offset", "I");
            jfieldID fs = (*env)->GetFieldID(env, bicl, "size", "I");
            jfieldID ft = (*env)->GetFieldID(env, bicl, "presentationTimeUs", "J");
            jfieldID ff = (*env)->GetFieldID(env, bicl, "flags", "I");
            info->offset = fo ? (*env)->GetIntField(env, plat->j_bufinfo, fo) : 0;
            info->size = fs ? (*env)->GetIntField(env, plat->j_bufinfo, fs) : 0;
            info->presentationTimeUs = ft ? (*env)->GetLongField(env, plat->j_bufinfo, ft) : 0;
            info->flags = ff ? (uint32_t)(*env)->GetIntField(env, plat->j_bufinfo, ff) : 0;
            (*env)->DeleteLocalRef(env, bicl);
        }
    }
    if (idx == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) {
        (void)audio_jni_refresh_bufs(plat, env, 0);
        (void)audio_jni_refresh_bufs(plat, env, 1);
    }
    return (ssize_t)idx;
}

static uint8_t *audio_mc_get_out(audio_plat *plat, size_t idx, size_t *cap)
{
    JNIEnv *env = NULL;
    jobject buf;
    void *p;
    jlong c;
    if (plat == NULL) {
        return NULL;
    }
    if (!plat->jni_mode) {
        return AMediaCodec_getOutputBuffer(plat->codec, idx, cap);
    }
    if (!ms_hw_jni_env(&env) || env == NULL || plat->j_out_bufs == NULL) {
        return NULL;
    }
    buf = (*env)->GetObjectArrayElement(env, plat->j_out_bufs, (jint)idx);
    if (!buf) {
        audio_jni_clear_exc(env);
        (void)audio_jni_refresh_bufs(plat, env, 1);
        buf = plat->j_out_bufs ? (*env)->GetObjectArrayElement(env, plat->j_out_bufs, (jint)idx) : NULL;
        if (!buf) {
            return NULL;
        }
    }
    p = (*env)->GetDirectBufferAddress(env, buf);
    c = (*env)->GetDirectBufferCapacity(env, buf);
    (*env)->DeleteLocalRef(env, buf);
    if (cap) {
        *cap = (c > 0) ? (size_t)c : 0;
    }
    return (uint8_t *)p;
}

static media_status_t audio_mc_rel_out(audio_plat *plat, size_t idx, int render)
{
    JNIEnv *env = NULL;
    jclass cl;
    jmethodID mid;
    if (plat == NULL) {
        return AMEDIA_ERROR_UNKNOWN;
    }
    if (!plat->jni_mode) {
        return AMediaCodec_releaseOutputBuffer(plat->codec, idx, render ? 1 : 0);
    }
    if (!ms_hw_jni_env(&env) || env == NULL || plat->jcodec == NULL) {
        return AMEDIA_ERROR_UNKNOWN;
    }
    cl = (*env)->GetObjectClass(env, plat->jcodec);
    mid = cl ? (*env)->GetMethodID(env, cl, "releaseOutputBuffer", "(IZ)V") : NULL;
    if (!mid) {
        audio_jni_clear_exc(env);
        if (cl) {
            (*env)->DeleteLocalRef(env, cl);
        }
        return AMEDIA_ERROR_UNKNOWN;
    }
    (*env)->CallVoidMethod(env, plat->jcodec, mid, (jint)idx, (jboolean)(render ? JNI_TRUE : JNI_FALSE));
    audio_jni_clear_exc(env);
    (*env)->DeleteLocalRef(env, cl);
    return AMEDIA_OK;
}

static int audio_mc_on_format(audio_plat *plat, int32_t *rate, int32_t *ch)
{
    JNIEnv *env = NULL;
    if (plat == NULL) {
        return -1;
    }
    if (!plat->jni_mode) {
        AMediaFormat *of = AMediaCodec_getOutputFormat(plat->codec);
        if (of) {
            if (rate) {
                AMediaFormat_getInt32(of, AMEDIAFORMAT_KEY_SAMPLE_RATE, rate);
            }
            if (ch) {
                AMediaFormat_getInt32(of, AMEDIAFORMAT_KEY_CHANNEL_COUNT, ch);
            }
            AMediaFormat_delete(of);
            return 0;
        }
        return -1;
    }
    if (!ms_hw_jni_env(&env) || env == NULL || plat->jcodec == NULL) {
        return -1;
    }
    {
        jclass cl = (*env)->GetObjectClass(env, plat->jcodec);
        jclass fcl;
        jmethodID getf, geti;
        jobject fmt;
        jstring k;
        getf = cl ? (*env)->GetMethodID(env, cl, "getOutputFormat",
                                        "()Landroid/media/MediaFormat;") : NULL;
        fmt = getf ? (*env)->CallObjectMethod(env, plat->jcodec, getf) : NULL;
        audio_jni_clear_exc(env);
        if (cl) {
            (*env)->DeleteLocalRef(env, cl);
        }
        if (!fmt) {
            return -1;
        }
        fcl = (*env)->GetObjectClass(env, fmt);
        geti = fcl ? (*env)->GetMethodID(env, fcl, "getInteger", "(Ljava/lang/String;)I") : NULL;
        if (geti && rate) {
            k = (*env)->NewStringUTF(env, "sample-rate");
            *rate = (*env)->CallIntMethod(env, fmt, geti, k);
            audio_jni_clear_exc(env);
            (*env)->DeleteLocalRef(env, k);
        }
        if (geti && ch) {
            k = (*env)->NewStringUTF(env, "channel-count");
            *ch = (*env)->CallIntMethod(env, fmt, geti, k);
            audio_jni_clear_exc(env);
            (*env)->DeleteLocalRef(env, k);
        }
        if (fcl) {
            (*env)->DeleteLocalRef(env, fcl);
        }
        (*env)->DeleteLocalRef(env, fmt);
        (void)audio_jni_refresh_bufs(plat, env, 1);
    }
    return 0;
}

ms_status ms_hw_audio_android_open(ms_hw_audio *a, const ms_hw_audio_config *cfg)
{
    char name[128], detail[192];
    ms_hw_kind kind = MS_HW_KIND_NONE;
    int api = a->opts.api_level;
    ms_status st;
    AMediaFormat *fmt;
    media_status_t mst;
    audio_plat *plat;
    name[0] = 0;
    detail[0] = 0;
    /* PCM 直出 AudioTrack：不查询解码器，不走系统 AAC 例外。 */
    if (cfg != NULL && cfg->codec == MS_CODEC_PCM) {
        plat = (audio_plat *)calloc(1, sizeof(*plat));
        if (plat == NULL) {
            return MS_ERR_NOMEM;
        }
        a->caps.codec = MS_CODEC_PCM;
        a->caps.api_level = api;
        a->caps.backend = "audiotrack-pcm";
        a->caps.verified = 1;
        a->caps.kind = MS_HW_KIND_NONE;
        a->caps.is_hardware = 0;
        a->caps.system_decode_is_not_hardware = 0;
        a->caps.codec_name = "direct-pcm";
        a->plat = plat;
        a->android_ok = 1;
        a->codec_started = 0;
        a->pcm_direct = 1;
        if (ensure_track(a, cfg->sample_rate > 0 ? cfg->sample_rate : a->sample_rate,
                         cfg->channels > 0 ? cfg->channels : a->channels) != 0) {
            ms_hw_audio_android_close(a);
            snprintf(a->detail_buf, sizeof(a->detail_buf),
                     "PCM AudioTrack open failed");
            a->caps.detail = a->detail_buf;
            return MS_ERR_UNSUPPORTED;
        }
        snprintf(a->detail_buf, sizeof(a->detail_buf),
                 "PCM direct AudioTrack; not a decoder (device output unverified)");
        a->caps.detail = a->detail_buf;
        return MS_OK;
    }
#ifdef MS_SINGLE_SO
    /* 阶段 4 直通：开关打开且设备声明支持时，跳过软解，原始帧/IEC61937 直送 AudioTrack；
     * 打不开轨道就静默回退到下面的系统/内置软解，不出静音、不报错。 */
    if (cfg != NULL && a->pt_caps != 0) {
        int enc = 0;
        ms_pt_route r = ms_pt_pick_route(cfg->codec, a->pt_caps, ms_hw_device_sdk_int(), &enc);
        if (r != MS_PT_ROUTE_SOFT && cfg->sample_rate > 0) {
            plat = (audio_plat *)calloc(1, sizeof(*plat));
            if (plat == NULL) {
                return MS_ERR_NOMEM;
            }
            a->plat = plat;
            a->android_ok = 1;
            a->codec_started = 0;
            a->pcm_direct = 1;
            a->pcm_bits = 16;
            a->pt_route = (int)r;
            a->pt_encoding = enc;
            a->sample_rate = cfg->sample_rate;
            a->channels = cfg->channels > 0 ? cfg->channels : 2;
            if (r == MS_PT_ROUTE_IEC) {
                a->sample_rate = cfg->sample_rate * ms_pt_iec_rate_mul(cfg->codec);
                a->channels = 2;
                if (ms_pt_iec_init(&plat->iec, cfg->codec) == 0) {
                    plat->iec_on = 1;
                }
            }
            if (plat->iec_on == (r == MS_PT_ROUTE_IEC) &&
                ensure_track(a, a->sample_rate, a->channels) == 0) {
                a->caps.codec = cfg->codec;
                a->caps.api_level = api;
                a->caps.backend = (r == MS_PT_ROUTE_IEC) ? "passthrough-iec61937" : "passthrough-raw";
                a->caps.verified = 1;
                a->caps.kind = MS_HW_KIND_NONE;
                a->caps.is_hardware = 0;
                a->caps.system_decode_is_not_hardware = 0;
                a->caps.codec_name = "passthrough";
                snprintf(a->detail_buf, sizeof(a->detail_buf),
                         "audio passthrough (%s, encoding=%d); real HDMI/SPDIF sink unverified",
                         r == MS_PT_ROUTE_IEC ? "IEC61937" : "raw", enc);
                a->caps.detail = a->detail_buf;
                return MS_OK;
            }
            /* 直通失败：拆掉轨道与状态，回退软解。 */
            ms_hw_audio_android_close(a);
            a->pt_route = MS_PT_ROUTE_SOFT;
            a->pt_encoding = 0;
            a->pcm_direct = 0;
            a->android_ok = 0;
            a->pt_fallbacks++;
            if (cfg->sample_rate > 0) {
                a->sample_rate = cfg->sample_rate;
            }
            if (cfg->channels > 0) {
                a->channels = cfg->channels;
            }
        }
    }
#endif
    {
        ms_codec qc = (cfg != NULL && cfg->codec != MS_CODEC_UNKNOWN) ? cfg->codec
                                                                      : MS_CODEC_AAC;
        int allow_sys = (MS_SYS_AUDIO_OK(qc) &&
                         a->aac_policy == MS_HW_AAC_POLICY_ALLOW_SYSTEM);
        st = ms_hw_jni_pick_audio(qc, api, a->opts.allow_software, allow_sys,
                                  name, sizeof(name), &kind, &api, detail,
                                  sizeof(detail));
        a->caps.codec = qc;
    }
    a->caps.api_level = api;
    a->caps.backend = "mediacodec-audiotrack";
    a->caps.verified = 1;
    a->caps.kind = kind;
    if (name[0]) {
        snprintf(a->name_buf, sizeof(a->name_buf), "%s", name);
        a->caps.codec_name = a->name_buf;
    }
    snprintf(a->detail_buf, sizeof(a->detail_buf), "%s", detail[0] ? detail : "");
    a->caps.detail = a->detail_buf;
    a->caps.is_hardware = (kind == MS_HW_KIND_HARDWARE);
    a->caps.system_decode_is_not_hardware = !a->caps.is_hardware;
    if (st != MS_OK ||
        (kind != MS_HW_KIND_HARDWARE &&
         !(MS_SYS_AUDIO_OK(a->caps.codec) &&
           a->aac_policy == MS_HW_AAC_POLICY_ALLOW_SYSTEM) &&
         !a->opts.allow_software)) {
#ifdef MS_SINGLE_FFADEC
        /* 系统没有该音频解码器：AC-3/E-AC-3/MP2/G.711/DTS/AAC-LATM 用 FFmpeg 轻量软解出 PCM 直送 AudioTrack。 */
        if (ms_ffadec_supported(a->caps.codec) &&
            a->aac_policy == MS_HW_AAC_POLICY_ALLOW_SYSTEM) {
            ms_ffadec *fd = NULL;
            int rate = (cfg != NULL && cfg->sample_rate > 0) ? cfg->sample_rate : a->sample_rate;
            int ch = (cfg != NULL && cfg->channels > 0) ? cfg->channels : a->channels;
            if (ms_ffadec_open(a->caps.codec, rate, ch, &fd) == 0) {
                plat = (audio_plat *)calloc(1, sizeof(*plat));
                if (plat == NULL) {
                    ms_ffadec_close(fd);
                    return MS_ERR_NOMEM;
                }
                plat->ffadec = fd;
                a->plat = plat;
                a->android_ok = 1;
                a->codec_started = 0;
                a->pcm_direct = 1;
                a->pcm_bits = 16;
                a->sample_rate = ms_ffadec_out_rate(fd);
                a->channels = ms_ffadec_out_channels(fd);
                a->caps.backend = "ffmpeg-sw-audio+audiotrack";
                a->caps.codec_name = "ffmpeg-builtin";
                a->caps.kind = MS_HW_KIND_SOFTWARE;
                a->caps.is_hardware = 0;
                a->caps.system_decode_is_not_hardware = 1;
                if (ensure_track(a, a->sample_rate, a->channels) != 0) {
                    ms_hw_audio_android_close(a);
                    snprintf(a->detail_buf, sizeof(a->detail_buf), "ffmpeg audio: AudioTrack open failed");
                    a->caps.detail = a->detail_buf;
                    return MS_ERR_UNSUPPORTED;
                }
                snprintf(a->detail_buf, sizeof(a->detail_buf),
                         "FFmpeg 内置音频软解（系统无此解码器），非硬件；device output unverified");
                a->caps.detail = a->detail_buf;
                return MS_OK;
            }
        }
#endif
        a->caps.is_hardware = 0;
        a->caps.system_decode_is_not_hardware = 1;
        if (a->detail_buf[0] == 0) {
            snprintf(a->detail_buf, sizeof(a->detail_buf),
                     "no hardware audio decoder, software refused");
        }
        return MS_ERR_UNSUPPORTED;
    }
    /* 即便允许系统 AAC 或 allow_software，也不得把软件报成硬件。 */
    if (kind != MS_HW_KIND_HARDWARE) {
        a->caps.is_hardware = 0;
        a->caps.system_decode_is_not_hardware = 1;
        if (MS_SYS_AUDIO_OK(a->caps.codec) &&
            a->aac_policy == MS_HW_AAC_POLICY_ALLOW_SYSTEM) {
            snprintf(a->detail_buf, sizeof(a->detail_buf),
                     a->caps.codec == MS_CODEC_AAC ? "系统 AAC，非硬件" : "系统音频解码，非硬件");
            a->caps.detail = a->detail_buf;
        }
    }
    plat = (audio_plat *)calloc(1, sizeof(*plat));
    if (plat == NULL) {
        return MS_ERR_NOMEM;
    }
    /* API 18–20 无 libmediandk，走 Java MediaCodec；21+ 保持 NDK 路径。 */
    if (!ms_hw_use_ndk_mediacodec()) {
        if (audio_jni_open(a, plat, name, cfg) != 0) {
            audio_jni_drop_codec(plat);
            free(plat);
            snprintf(a->detail_buf, sizeof(a->detail_buf),
                     "JNI MediaCodec audio open failed (API 18-20 path)");
            return MS_ERR_UNSUPPORTED;
        }
        a->codec_started = 1;
        a->plat = plat;
        a->android_ok = 1;
        if (kind != MS_HW_KIND_HARDWARE &&
            a->aac_policy == MS_HW_AAC_POLICY_ALLOW_SYSTEM) {
            snprintf(a->detail_buf, sizeof(a->detail_buf),
                     "系统 AAC，非硬件");
        } else {
            snprintf(a->detail_buf, sizeof(a->detail_buf),
                     "JNI MediaCodec+AudioTrack (API 18-20); device output unverified");
        }
        a->caps.detail = a->detail_buf;
        return MS_OK;
    }
    plat->codec = AMediaCodec_createCodecByName(name);
    if (plat->codec == NULL) {
        free(plat);
        snprintf(a->detail_buf, sizeof(a->detail_buf), "AMediaCodec_createCodecByName failed");
        return MS_ERR_UNSUPPORTED;
    }
    fmt = AMediaFormat_new();
    if (fmt == NULL) {
        AMediaCodec_delete(plat->codec);
        free(plat);
        return MS_ERR_NOMEM;
    }
    AMediaFormat_setString(fmt, AMEDIAFORMAT_KEY_MIME,
                           ms_hw_audio_mime(cfg->codec == MS_CODEC_UNKNOWN
                                                ? MS_CODEC_AAC
                                                : cfg->codec));
    if (cfg->sample_rate > 0) {
        AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_SAMPLE_RATE, cfg->sample_rate);
    }
    if (cfg->channels > 0) {
        AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_CHANNEL_COUNT, cfg->channels);
    }
    if (cfg->asc != NULL && cfg->asc_size > 0) {
        AMediaFormat_setBuffer(fmt, "csd-0", cfg->asc, cfg->asc_size);
    }
    if (cfg->codec == MS_CODEC_OPUS && cfg->asc != NULL && cfg->asc_size > 0) {
        uint32_t pre = 0, spr = 3840;
        uint64_t delay_ns, preroll_ns;
        if (cfg->struct_size >= offsetof(ms_hw_audio_config, pre_skip) + sizeof(uint32_t)) {
            pre = cfg->pre_skip;
        }
        if (cfg->struct_size >= offsetof(ms_hw_audio_config, seek_preroll) + sizeof(uint32_t) &&
            cfg->seek_preroll > 0) {
            spr = cfg->seek_preroll;
        }
        /* MediaCodec 合同：csd-1/csd-2 为原生字节序 uint64 纳秒，覆盖头里的 pre-skip。
         * SoftOpus：时间戳 0 丢 codec delay，非 0 丢 seek preroll。
         * 已交给编解码器后本层不得再对同一段 PCM 做 start/warmup 修剪，也不能再设 encoder-delay。 */
        delay_ns = ((uint64_t)pre * 1000000000ull) / 48000ull;
        preroll_ns = ((uint64_t)spr * 1000000000ull) / 48000ull;
        AMediaFormat_setBuffer(fmt, "csd-1", &delay_ns, sizeof(delay_ns));
        AMediaFormat_setBuffer(fmt, "csd-2", &preroll_ns, sizeof(preroll_ns));
        a->platform_trims_delay = 1;
    }
    mst = AMediaCodec_configure(plat->codec, fmt, NULL, NULL, 0);
    AMediaFormat_delete(fmt);
    if (mst != AMEDIA_OK) {
        AMediaCodec_delete(plat->codec);
        free(plat);
        snprintf(a->detail_buf, sizeof(a->detail_buf), "AMediaCodec_configure failed");
        return MS_ERR_UNSUPPORTED;
    }
    mst = AMediaCodec_start(plat->codec);
    if (mst != AMEDIA_OK) {
        (void)AMediaCodec_delete(plat->codec);
        free(plat);
        snprintf(a->detail_buf, sizeof(a->detail_buf), "AMediaCodec_start failed");
        return MS_ERR_UNKNOWN;
    }
    a->codec_started = 1;
    a->plat = plat;
    a->android_ok = 1;
    if (kind != MS_HW_KIND_HARDWARE &&
        a->aac_policy == MS_HW_AAC_POLICY_ALLOW_SYSTEM) {
        snprintf(a->detail_buf, sizeof(a->detail_buf),
                 "系统 AAC，非硬件");
    } else {
        snprintf(a->detail_buf, sizeof(a->detail_buf),
                 "MediaCodec+AudioTrack path configured; device output unverified");
    }
    a->caps.detail = a->detail_buf;
    return MS_OK;
}

void ms_hw_audio_android_close(ms_hw_audio *a)
{
    audio_plat *plat;
    if (a == NULL || a->plat == NULL) {
        return;
    }
    plat = (audio_plat *)a->plat;
    /* 先 pause AudioTrack，避免仍在 write 时拆解码器。
     * 不要在 delete 前单独 AMediaCodec_stop：stop 会拆 CodecLooper
     * 互斥量，随后 delete 再加锁会 FORTIFY。delete 内部会 release。 */
    if (plat->track) {
        JNIEnv *env = NULL;
        if (ms_hw_jni_env(&env) && env) {
            jclass cl = (*env)->GetObjectClass(env, plat->track);
            jmethodID pause = cl ? (*env)->GetMethodID(env, cl, "pause", "()V") : NULL;
            if (pause) {
                (*env)->CallVoidMethod(env, plat->track, pause);
                (*env)->ExceptionClear(env);
            }
            if (cl) {
                (*env)->DeleteLocalRef(env, cl);
            }
        }
    }
    /* pause 之后再加入写线程，避免 AudioTrack.write 卡住 destroy。 */
    plat_writer_stop(plat);
#ifdef MS_SINGLE_SO
    if (plat->iec_on) {
        ms_pt_iec_free(&plat->iec);
        plat->iec_on = 0;
    }
#endif
#ifdef MS_SINGLE_FFADEC
    if (plat->ffadec != NULL) {
        ms_ffadec_close((ms_ffadec *)plat->ffadec);
        plat->ffadec = NULL;
    }
#endif
    if (plat->jni_mode) {
        audio_jni_drop_codec(plat);
        a->codec_started = 0;
    } else if (plat->codec) {
        if (a->codec_started) {
            (void)AMediaCodec_flush(plat->codec);
        }
        a->codec_started = 0;
        (void)AMediaCodec_delete(plat->codec);
        plat->codec = NULL;
    }
    if (plat->track) {
        JNIEnv *env = NULL;
        if (ms_hw_jni_env(&env) && env) {
            jclass cl = (*env)->GetObjectClass(env, plat->track);
            jmethodID rel = cl ? (*env)->GetMethodID(env, cl, "release", "()V") : NULL;
            if (rel) {
                (*env)->CallVoidMethod(env, plat->track, rel);
                (*env)->ExceptionClear(env);
            }
            if (cl) {
                (*env)->DeleteLocalRef(env, cl);
            }
            (*env)->DeleteGlobalRef(env, plat->track);
            if (plat->at_ts_obj) {
                (*env)->DeleteGlobalRef(env, plat->at_ts_obj);
            }
        }
        plat->at_ts_obj = NULL;
        plat->at_m_ts = NULL;
        plat->track = NULL;
        plat->track_ready = 0;
    }
    plat_clear_pcm_left(plat);
    plat_reset_head(plat);
    plat_writer_destroy(plat);
    free(plat);
    a->plat = NULL;
    a->android_ok = 0;
}

static int ensure_track(ms_hw_audio *a, int rate, int ch)
{
    audio_plat *plat = (audio_plat *)a->plat;
    JNIEnv *env = NULL;
    jclass cl;
    jmethodID ctor, play, getmin;
    jobject obj;
    jint stream = 3; /* STREAM_MUSIC */
    jint fmt = (ch >= 2) ? 12 : 4; /* CHANNEL_OUT_STEREO / MONO */
    jint enc = a->pt_encoding > 0 ? a->pt_encoding : 2; /* 直通 ENCODING_* / ENCODING_PCM_16BIT */
    jint mode = 1; /* MODE_STREAM */
    jint minbuf;
    if (plat == NULL) {
        return -1;
    }
    if (plat->track_ready) {
        return 0;
    }
#ifdef MS_SINGLE_SO
    if (a->pt_route == MS_PT_ROUTE_RAW) {
        /* 原始直通按真实声道布局：5.1=CHANNEL_OUT_5POINT1(252)，7.1=CHANNEL_OUT_7POINT1_SURROUND(6396)。 */
        fmt = (ch >= 8) ? 6396 : (ch >= 6) ? 252 : 12;
    } else if (a->pt_route == MS_PT_ROUTE_IEC) {
        fmt = 12;
    }
#endif
    if (!ms_hw_jni_env(&env) || env == NULL) {
        snprintf(a->detail_buf, sizeof(a->detail_buf),
                 "AudioTrack requires a JVM; audio output not available");
        return -1;
    }
    cl = (*env)->FindClass(env, "android/media/AudioTrack");
    if (!cl) {
        (*env)->ExceptionClear(env);
        return -1;
    }
    getmin = (*env)->GetStaticMethodID(env, cl, "getMinBufferSize", "(III)I");
    ctor = (*env)->GetMethodID(env, cl, "<init>", "(IIIIII)V");
    play = (*env)->GetMethodID(env, cl, "play", "()V");
    if (!getmin || !ctor || !play) {
        (*env)->DeleteLocalRef(env, cl);
        return -1;
    }
    minbuf = (*env)->CallStaticIntMethod(env, cl, getmin, rate, fmt, enc);
    if ((*env)->ExceptionCheck(env) || minbuf <= 0) {
        (*env)->ExceptionClear(env);
        (*env)->DeleteLocalRef(env, cl);
        return -1;
    }
    plat->at_buf_bytes = (int)minbuf;
    plat->at_frame_bytes = (enc == 2) ? ((ch >= 2) ? 4 : 2) : 0; /* 仅 PCM16 立体声/单声道 */
    plat->at_sdk = ms_hw_device_sdk_int();
    obj = (*env)->NewObject(env, cl, ctor, stream, rate, fmt, enc, minbuf, mode);
    if (!obj || (*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        (*env)->DeleteLocalRef(env, cl);
        return -1;
    }
    (*env)->CallVoidMethod(env, obj, play);
    (*env)->ExceptionClear(env);
    plat->track = (*env)->NewGlobalRef(env, obj);
    (*env)->DeleteLocalRef(env, obj);
    (*env)->DeleteLocalRef(env, cl);
    plat->track_ready = 1;
    plat->at_wr_frames = 0;
    at_reset(plat, 1); /* 新轨道：位置跟踪从 INITIALIZING 开始 */
    if (plat->vol_set) {
        (void)ms_hw_audio_set_volume(a, plat->vol_left, plat->vol_right);
    }
    if (a->rate_num > 0 && a->rate_den > 0 &&
        (a->rate_num != 1 || a->rate_den != 1)) {
        (void)ms_hw_audio_android_set_rate(a, a->rate_num, a->rate_den);
    }
    if (a->paused) {
        ms_hw_audio_android_set_paused(a, 1);
    }
    return 0;
}

#ifndef MS_HW_AUDIOTRACK_WRITE_NON_BLOCKING
#define MS_HW_AUDIOTRACK_WRITE_NON_BLOCKING 1 /* AudioTrack.WRITE_NON_BLOCKING, API 21+ */
#endif
#ifndef MS_HW_AUDIOTRACK_WRITE_MAX
#define MS_HW_AUDIOTRACK_WRITE_MAX 8192
#endif

/* API 21+：write([BIII)I + WRITE_NON_BLOCKING。
 * API 18–20：无 writeMode / ByteBuffer write，只用 write([BII)I（可能阻塞，已在写线程）。 */
static jint track_write_jni(JNIEnv *env, jobject track, const uint8_t *data, int n)
{
    jbyteArray arr;
    jclass cl;
    jmethodID mid;
    jint wr;
    int sdk;
    if (!env || !track || !data || n <= 0) {
        return 0;
    }
    cl = (*env)->GetObjectClass(env, track);
    if (!cl) {
        return 0;
    }
    sdk = ms_hw_device_sdk_int();
    arr = (*env)->NewByteArray(env, n);
    if (!arr) {
        (*env)->DeleteLocalRef(env, cl);
        return 0;
    }
    (*env)->SetByteArrayRegion(env, arr, 0, n, (const jbyte *)data);
    if (sdk >= 21) {
        mid = (*env)->GetMethodID(env, cl, "write", "([BIII)I");
        if (!mid) {
            (*env)->ExceptionClear(env);
            (*env)->DeleteLocalRef(env, arr);
            (*env)->DeleteLocalRef(env, cl);
            return 0;
        }
        wr = (*env)->CallIntMethod(env, track, mid, arr, 0, n,
                                   MS_HW_AUDIOTRACK_WRITE_NON_BLOCKING);
    } else {
        mid = (*env)->GetMethodID(env, cl, "write", "([BII)I");
        if (!mid) {
            (*env)->ExceptionClear(env);
            (*env)->DeleteLocalRef(env, arr);
            (*env)->DeleteLocalRef(env, cl);
            return 0;
        }
        wr = (*env)->CallIntMethod(env, track, mid, arr, 0, n);
    }
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        wr = 0;
    }
    (*env)->DeleteLocalRef(env, arr);
    (*env)->DeleteLocalRef(env, cl);
    return wr;
}

static void *plat_writer_main(void *arg)
{
    audio_plat *plat = (audio_plat *)arg;
    JNIEnv *env = NULL;
    if (plat == NULL) {
        return NULL;
    }
    if (!ms_hw_jni_env(&env) || env == NULL) {
        return NULL;
    }
    pthread_mutex_lock(&plat->wmu);
    for (;;) {
        unsigned gen;
        int n;
        jobject track;
        jint wr;
        uint8_t local[8192];
        while (!plat->w_stop && plat->w_n <= 0) {
            pthread_cond_wait(&plat->wcv, &plat->wmu);
        }
        if (plat->w_stop && plat->w_n <= 0) {
            break;
        }
        n = plat->w_n;
        gen = plat->w_req_gen;
        if (n > (int)sizeof(local)) {
            n = (int)sizeof(local);
        }
        memcpy(local, plat->w_buf, (size_t)n);
        track = plat->track;
        pthread_mutex_unlock(&plat->wmu);
        wr = track_write_jni(env, track, local, n);
        if (wr < 0) {
            wr = 0;
        }
        pthread_mutex_lock(&plat->wmu);
        if (gen == plat->w_gen) {
            plat->w_wr = (int)wr;
            plat->w_unclaimed = 1;
        }
        plat->w_n = 0;
        plat->w_busy = 0;
        pthread_cond_broadcast(&plat->wcv);
    }
    pthread_mutex_unlock(&plat->wmu);
    return NULL;
}

static int plat_writer_ensure(audio_plat *plat)
{
    pthread_condattr_t ca;
    if (plat == NULL) {
        return -1;
    }
    if (plat->w_started) {
        return 0;
    }
    if (!plat->w_inited) {
        pthread_mutex_init(&plat->wmu, NULL);
        pthread_condattr_init(&ca);
        /* pthread_condattr_setclock 是 API 21 才进入 bionic；18–20 用默认 CLOCK_REALTIME。
         * 必须弱引用：强 UND 会让 API 18 在调用前就加载失败。 */
        if (ms_hw_device_sdk_int() >= 21 && pthread_condattr_setclock != NULL) {
            pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
        }
        pthread_cond_init(&plat->wcv, &ca);
        pthread_condattr_destroy(&ca);
        plat->w_inited = 1;
    }
    plat->w_stop = 0;
    plat->w_busy = 0;
    plat->w_unclaimed = 0;
    plat->w_n = 0;
    if (pthread_create(&plat->wth, NULL, plat_writer_main, plat) != 0) {
        return -1;
    }
    plat->w_started = 1;
    return 0;
}

static void plat_writer_drop(audio_plat *plat)
{
    if (plat == NULL || !plat->w_inited) {
        return;
    }
    pthread_mutex_lock(&plat->wmu);
    plat->w_gen++;
    plat->w_unclaimed = 0;
    plat->w_wr = 0;
    pthread_mutex_unlock(&plat->wmu);
}

static void plat_writer_stop(audio_plat *plat)
{
    if (plat == NULL || !plat->w_started) {
        return;
    }
    pthread_mutex_lock(&plat->wmu);
    plat->w_stop = 1;
    plat->w_gen++;
    plat->w_n = 0;
    pthread_cond_broadcast(&plat->wcv);
    pthread_mutex_unlock(&plat->wmu);
    pthread_join(plat->wth, NULL);
    plat->w_started = 0;
    plat->w_busy = 0;
    plat->w_unclaimed = 0;
}

static void plat_writer_destroy(audio_plat *plat)
{
    if (plat == NULL) {
        return;
    }
    plat_writer_stop(plat);
    if (plat->w_inited) {
        pthread_cond_destroy(&plat->wcv);
        pthread_mutex_destroy(&plat->wmu);
        plat->w_inited = 0;
    }
}

/* 不在 drive 线程上阻塞：最多等 2ms。进行中的 write 返回 0，结果稍后取走。
 * 调用方必须在结果未取走之前保持同一段 PCM（通常已在 leftover 里）。 */
static jint track_write(audio_plat *plat, const uint8_t *data, int n)
{
    struct timespec ts;
    jint wr;
    int chunk;
    if (plat == NULL || !data || n <= 0) {
        return 0;
    }
    if (plat_writer_ensure(plat) != 0) {
        return 0;
    }
    chunk = n;
    if (chunk > MS_HW_AUDIOTRACK_WRITE_MAX) {
        chunk = MS_HW_AUDIOTRACK_WRITE_MAX;
    }
    pthread_mutex_lock(&plat->wmu);
    if (plat->w_unclaimed) {
        wr = (jint)plat->w_wr;
        plat->w_unclaimed = 0;
        pthread_mutex_unlock(&plat->wmu);
        return wr;
    }
    if (plat->w_busy) {
        pthread_mutex_unlock(&plat->wmu);
        return 0;
    }
    memcpy(plat->w_buf, data, (size_t)chunk);
    plat->w_n = chunk;
    plat->w_req_gen = plat->w_gen;
    plat->w_busy = 1;
    plat->w_unclaimed = 0;
    pthread_cond_signal(&plat->wcv);
    /* 与 cond 时钟一致：API 21+ 单调钟，API 18–20 为 CLOCK_REALTIME。 */
    if (clock_gettime(ms_hw_device_sdk_int() >= 21 ? CLOCK_MONOTONIC : CLOCK_REALTIME,
                      &ts) != 0) {
        pthread_mutex_unlock(&plat->wmu);
        return 0;
    }
    ts.tv_nsec += 2000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec += 1;
        ts.tv_nsec -= 1000000000L;
    }
    while (plat->w_busy) {
        int rc = pthread_cond_timedwait(&plat->wcv, &plat->wmu, &ts);
        if (rc == ETIMEDOUT) {
            break;
        }
    }
    if (plat->w_busy) {
        pthread_mutex_unlock(&plat->wmu);
        return 0;
    }
    wr = (jint)plat->w_wr;
    plat->w_unclaimed = 0;
    pthread_mutex_unlock(&plat->wmu);
    return wr;
}

int64_t ms_hw_audio_android_played_frames(const ms_hw_audio *a)
{
    audio_plat *plat;
    JNIEnv *env = NULL;
    jclass cl;
    jmethodID mid;
    jint pos;
    if (a == NULL || a->plat == NULL) {
        return -1;
    }
    plat = (audio_plat *)a->plat;
    if (!plat->track_ready || !plat->track) {
        return -1;
    }
    if (!ms_hw_jni_env(&env) || env == NULL) {
        return -1;
    }
    cl = (*env)->GetObjectClass(env, plat->track);
    /* getTimestamp 是 API 19 才有；本路径统一用 playbackHeadPosition（API 3+），
     * 以便 API 18 与 19+ 行为一致，不做 19 专用时间戳。 */
    mid = cl ? (*env)->GetMethodID(env, cl, "getPlaybackHeadPosition", "()I") : NULL;
    if (!mid) {
        if (cl) {
            (*env)->DeleteLocalRef(env, cl);
        }
        return -1;
    }
    pos = (*env)->CallIntMethod(env, plat->track, mid);
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        if (cl) {
            (*env)->DeleteLocalRef(env, cl);
        }
        return -1;
    }
    if (cl) {
        (*env)->DeleteLocalRef(env, cl);
    }
    {
        /* getPlaybackHeadPosition 是无符号 32 位，每 2^32 帧回绕；JNI 以 jint 返回。 */
        uint32_t raw = (uint32_t)pos;
        if (plat->head_valid && raw < plat->head_raw) {
            plat->head_wraps++;
        }
        plat->head_raw = raw;
        plat->head_valid = 1;
        return ((int64_t)plat->head_wraps << 32) + (int64_t)raw;
    }
}


/*
 * ---- 音频位置：Media3 AudioTrackPositionTracker + AudioTimestampPoller 精简移植 ----
 * 来源 androidx/media main@101e97c（2026-11-24 核对），常量逐一对应：
 *   AudioTimestampPoller: FAST_POLL_INTERVAL_US=10_000, SLOW_POLL_INTERVAL_US=10_000_000,
 *     ERROR_POLL_INTERVAL_US=500_000, INITIALIZING_DURATION_US=500_000,
 *     MAX_POSITION_DRIFT_ADVANCING_TIMESTAMP_US=1000, WAIT_FOR_ADVANCE_DURATION_US=2_000_000,
 *     MAX_AUDIO_TIMESTAMP_OFFSET_US=5s；状态 INITIALIZING/TIMESTAMP/TIMESTAMP_ADVANCING/NO_TIMESTAMP/ERROR。
 *   AudioTrackPositionTracker: MAX_LATENCY_US=10s, MAX_POSITION_DRIFT_FOR_SMOOTHING_US=1s,
 *     MAX_POSITION_SMOOTHING_SPEED_CHANGE_PERCENT=10, MAX_PLAYHEAD_OFFSET_COUNT=10,
 *     MIN_PLAYHEAD_OFFSET_SAMPLE_INTERVAL_US=30_000, MIN_LATENCY_SAMPLE_INTERVAL_US=50_0000(=500ms)。
 * 规则：只有时间戳“确认在前进”(TIMESTAMP_ADVANCING) 才用 getTimestamp 外推；否则用
 *   系统时钟 + 10 个 head 偏移样本平均（head 粒度约 20ms，平滑防抖），并减去
 *   getLatency()*1000 - 缓冲时长（隐藏方法，取不到就为 0；仅 PCM）。时间戳模式不扣延迟。
 * API 18 无 getTimestamp：恒走平滑 head。位置不超过已写帧；暂停/flush/恢复时重置（Media3 pause/start 亦重置）。
 * 未移植：PLAYSTATE_STOPPED 后的模拟位置、onPositionAdvancing 回调、各类错误上报监听。
 * by: Long <mycjzb@gmail.com>
 */
enum { AT_INIT = 0, AT_TS, AT_TS_ADV, AT_NO_TS, AT_ERR };
#define AT_FAST_POLL_US 10000ll
#define AT_SLOW_POLL_US 10000000ll
#define AT_ERROR_POLL_US 500000ll
#define AT_INIT_DUR_US 500000ll
#define AT_ADV_DRIFT_US 1000ll
#define AT_WAIT_ADV_US 2000000ll
#define AT_MAX_TS_OFFSET_US 5000000ll
#define AT_MAX_LATENCY_US 10000000ll
#define AT_SMOOTH_DRIFT_US 1000000ll
#define AT_SMOOTH_PCT 10
#define AT_PH_COUNT 10
#define AT_PH_INTERVAL_US 30000ll
#define AT_LAT_INTERVAL_US 500000ll

static int64_t at_now_us(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (int64_t)ts.tv_sec * 1000000ll + (int64_t)ts.tv_nsec / 1000ll;
}

static void at_set_state(audio_plat *plat, int st)
{
    plat->at_state = st;
    switch (st) {
    case AT_INIT:
        plat->at_last_poll_us = 0;
        plat->at_init_frames = -1;
        plat->at_init_ts_sys_us = 0;
        plat->at_init_sys_us = at_now_us();
        plat->at_interval_us = AT_FAST_POLL_US;
        break;
    case AT_TS:
        plat->at_interval_us = AT_FAST_POLL_US;
        break;
    case AT_TS_ADV:
    case AT_NO_TS:
        plat->at_interval_us = AT_SLOW_POLL_US;
        break;
    default:
        plat->at_interval_us = AT_ERROR_POLL_US;
        break;
    }
}

/* all=1：flush/新建（帧计数归零）；all=0：暂停/恢复（只重置同步参数与轮询器）。 */
static void at_reset(audio_plat *plat, int all)
{
    if (plat == NULL) {
        return;
    }
    plat->at_ph_n = 0;
    plat->at_ph_i = 0;
    plat->at_ph_last_us = 0;
    plat->at_ph_smooth = 0;
    plat->at_last_sys_us = 0;
    plat->at_last_pos_us = 0;
    if (all) {
        plat->at_ts_raw_last = 0;
        plat->at_ts_wraps = 0;
        plat->at_ts_frames = 0;
        plat->at_lat_last_us = 0;
    }
    at_set_state(plat, AT_INIT);
}

static int64_t at_frames_to_us(int64_t fr, int rate)
{
    return rate > 0 ? (fr * 1000000ll) / rate : 0;
}

static int64_t at_ts_pos_from(int64_t frames, int64_t ts_sys_us, int64_t now_us, double speed,
                              int rate)
{
    /* computeTimestampPositionUs：时间戳帧位置 + 之后经过的（换算为媒体时长）时间 */
    return at_frames_to_us(frames, rate) + (int64_t)((double)(now_us - ts_sys_us) * speed);
}

static int64_t at_head_estimate(audio_plat *plat, int64_t now_us, int64_t head_us, double speed)
{
    int64_t pos;
    if (plat->at_ph_n == 0) {
        pos = head_us;
    } else {
        pos = (int64_t)((double)(now_us + plat->at_ph_smooth) * speed);
    }
    pos -= plat->at_lat_us;
    return pos > 0 ? pos : 0;
}

static int at_poll_timestamp(audio_plat *plat, JNIEnv *env, jobject track)
{
    jboolean ok;
    if (plat->at_ts_dead || plat->at_sdk < 19) {
        return 0;
    }
    if (plat->at_m_ts == NULL) {
        jclass tcl = (*env)->FindClass(env, "android/media/AudioTimestamp");
        jclass cl = (*env)->GetObjectClass(env, track);
        jmethodID ctor;
        jobject o;
        if (tcl == NULL || cl == NULL) {
            (*env)->ExceptionClear(env);
            plat->at_ts_dead = 1;
            return 0;
        }
        ctor = (*env)->GetMethodID(env, tcl, "<init>", "()V");
        plat->at_f_frame = (*env)->GetFieldID(env, tcl, "framePosition", "J");
        plat->at_f_nano = (*env)->GetFieldID(env, tcl, "nanoTime", "J");
        plat->at_m_ts = (*env)->GetMethodID(env, cl, "getTimestamp",
                                            "(Landroid/media/AudioTimestamp;)Z");
        o = (ctor != NULL) ? (*env)->NewObject(env, tcl, ctor) : NULL;
        if ((*env)->ExceptionCheck(env) || o == NULL || plat->at_m_ts == NULL ||
            plat->at_f_frame == NULL || plat->at_f_nano == NULL) {
            (*env)->ExceptionClear(env);
            plat->at_ts_dead = 1;
            plat->at_m_ts = NULL;
        } else {
            plat->at_ts_obj = (*env)->NewGlobalRef(env, o);
        }
        if (o) {
            (*env)->DeleteLocalRef(env, o);
        }
        (*env)->DeleteLocalRef(env, tcl);
        (*env)->DeleteLocalRef(env, cl);
        if (plat->at_ts_dead || plat->at_ts_obj == NULL) {
            plat->at_ts_dead = 1;
            return 0;
        }
    }
    ok = (*env)->CallBooleanMethod(env, track, plat->at_m_ts, plat->at_ts_obj);
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        return 0;
    }
    if (!ok) {
        return 0;
    }
    {
        /* AudioTimestampWrapper.maybeUpdateTimestamp：回绕计数（flush 时整体重置）。 */
        int64_t raw = (int64_t)(*env)->GetLongField(env, plat->at_ts_obj, plat->at_f_frame);
        int64_t nano = (int64_t)(*env)->GetLongField(env, plat->at_ts_obj, plat->at_f_nano);
        if (plat->at_ts_raw_last > raw) {
            plat->at_ts_wraps++;
        }
        plat->at_ts_raw_last = raw;
        plat->at_ts_frames = raw + (plat->at_ts_wraps << 32);
        plat->at_ts_sys_us = nano / 1000ll;
    }
    return 1;
}

/* AudioTimestampPoller.maybePollTimestamp */
static void at_maybe_poll(audio_plat *plat, JNIEnv *env, jobject track, int64_t now_us,
                          double speed, int rate, int64_t head_est_us, int force)
{
    int upd;
    if (!force && now_us - plat->at_last_poll_us < plat->at_interval_us) {
        return;
    }
    plat->at_last_poll_us = now_us;
    upd = at_poll_timestamp(plat, env, track);
    if (upd) {
        /* checkTimestampIsPlausibleAndUpdateErrorState */
        int64_t tpos = at_ts_pos_from(plat->at_ts_frames, plat->at_ts_sys_us, now_us, speed, rate);
        int64_t d1 = plat->at_ts_sys_us - now_us;
        int64_t d2 = tpos - head_est_us;
        if (d1 < 0) {
            d1 = -d1;
        }
        if (d2 < 0) {
            d2 = -d2;
        }
        if (d1 > AT_MAX_TS_OFFSET_US || d2 > AT_MAX_TS_OFFSET_US) {
            at_set_state(plat, AT_ERR);
        } else if (plat->at_state == AT_ERR) {
            at_set_state(plat, AT_INIT);
        }
    }
    switch (plat->at_state) {
    case AT_INIT:
        if (upd) {
            if (plat->at_ts_sys_us >= plat->at_init_sys_us) {
                plat->at_init_frames = plat->at_ts_frames;
                plat->at_init_ts_sys_us = plat->at_ts_sys_us;
                at_set_state(plat, AT_TS);
            }
        } else if (now_us - plat->at_init_sys_us > AT_INIT_DUR_US) {
            at_set_state(plat, AT_NO_TS);
        }
        break;
    case AT_TS:
        if (upd) {
            int adv = 0;
            if (plat->at_ts_frames > plat->at_init_frames) {
                int64_t a0 = at_ts_pos_from(plat->at_init_frames, plat->at_init_ts_sys_us,
                                            now_us, speed, rate);
                int64_t a1 = at_ts_pos_from(plat->at_ts_frames, plat->at_ts_sys_us,
                                            now_us, speed, rate);
                int64_t dr = a1 - a0;
                if (dr < 0) {
                    dr = -dr;
                }
                adv = dr < AT_ADV_DRIFT_US;
            }
            if (adv) {
                at_set_state(plat, AT_TS_ADV);
            } else if (now_us - plat->at_init_sys_us > AT_WAIT_ADV_US) {
                at_set_state(plat, AT_NO_TS);
            } else {
                plat->at_init_frames = plat->at_ts_frames;
                plat->at_init_ts_sys_us = plat->at_ts_sys_us;
            }
        } else {
            at_set_state(plat, AT_INIT);
        }
        break;
    case AT_TS_ADV:
        if (!upd) {
            at_set_state(plat, AT_INIT);
        }
        break;
    case AT_NO_TS:
        if (upd) {
            at_set_state(plat, AT_INIT);
        }
        break;
    default:
        break;
    }
}

/* maybeUpdateLatency：隐藏方法 AudioTrack.getLatency()（毫秒，含缓冲），减去缓冲时长。 */
static int at_maybe_update_latency(audio_plat *plat, JNIEnv *env, jobject track, int64_t now_us,
                                   int rate)
{
    int64_t prev = plat->at_lat_us;
    if (plat->at_frame_bytes <= 0 || plat->at_lat_dead || rate <= 0 ||
        now_us - plat->at_lat_last_us < AT_LAT_INTERVAL_US) {
        return 0;
    }
    plat->at_lat_last_us = now_us;
    if (plat->at_m_lat == NULL) {
        jclass cl = (*env)->GetObjectClass(env, track);
        plat->at_m_lat = cl ? (*env)->GetMethodID(env, cl, "getLatency", "()I") : NULL;
        if ((*env)->ExceptionCheck(env) || plat->at_m_lat == NULL) {
            (*env)->ExceptionClear(env);
            plat->at_m_lat = NULL;
            plat->at_lat_dead = 1;
        }
        if (cl) {
            (*env)->DeleteLocalRef(env, cl);
        }
        if (plat->at_lat_dead) {
            return 0;
        }
    }
    {
        jint ms = (*env)->CallIntMethod(env, track, plat->at_m_lat);
        int64_t buf_us;
        if ((*env)->ExceptionCheck(env)) {
            (*env)->ExceptionClear(env);
            plat->at_lat_dead = 1;
            plat->at_lat_us = 0;
            return prev != 0;
        }
        buf_us = at_frames_to_us((int64_t)(plat->at_buf_bytes / plat->at_frame_bytes), rate);
        plat->at_lat_us = (int64_t)ms * 1000ll - buf_us;
        if (plat->at_lat_us < 0) {
            plat->at_lat_us = 0;
        }
        if (plat->at_lat_us > AT_MAX_LATENCY_US) {
            plat->at_lat_us = 0;
        }
    }
    return prev != plat->at_lat_us;
}

int ms_hw_audio_android_position_us(const ms_hw_audio *ca, int64_t *pos_us, int64_t *raw_us)
{
    ms_hw_audio *a = (ms_hw_audio *)(void *)ca;
    audio_plat *plat;
    JNIEnv *env = NULL;
    int64_t fr, head_us, now_us, pos;
    int rate;
    int use_ts;
    double speed = 1.0;
    if (a == NULL || a->plat == NULL || a->sample_rate <= 0) {
        return -1;
    }
    plat = (audio_plat *)a->plat;
    fr = ms_hw_audio_android_played_frames(a);
    if (fr < 0 || !ms_hw_jni_env(&env) || env == NULL) {
        return -1;
    }
    rate = a->sample_rate;
    head_us = at_frames_to_us(fr, rate);
    *raw_us = head_us;
    now_us = at_now_us();
    if (plat->at_sdk >= 23 && a->rate_num > 0 && a->rate_den > 0) {
        speed = (double)a->rate_num / (double)a->rate_den; /* PlaybackParams 速度 */
    }
    if (!a->paused && !a->quota_armed) {
        /* maybeSampleSyncParams（仅 PLAYSTATE_PLAYING） */
        if (now_us - plat->at_ph_last_us >= AT_PH_INTERVAL_US) {
            int i;
            if (head_us == 0) {
                goto pos_out; /* 尚未出声：Media3 直接返回，不测延迟也不轮询时间戳 */
            }
            plat->at_ph_off[plat->at_ph_i] = (int64_t)((double)head_us / speed) - now_us;
            plat->at_ph_i = (plat->at_ph_i + 1) % AT_PH_COUNT;
            if (plat->at_ph_n < AT_PH_COUNT) {
                plat->at_ph_n++;
            }
            plat->at_ph_last_us = now_us;
            plat->at_ph_smooth = 0;
            for (i = 0; i < plat->at_ph_n; i++) {
                plat->at_ph_smooth += plat->at_ph_off[i] / plat->at_ph_n;
            }
        }
        {
            int lat_upd = at_maybe_update_latency(plat, env, plat->track, now_us, rate);
            at_maybe_poll(plat, env, plat->track, now_us, speed, rate,
                          at_head_estimate(plat, now_us, head_us, speed), lat_upd);
        }
    }
pos_out:
    use_ts = (plat->at_state == AT_TS_ADV) && !a->paused;
    pos = use_ts ? at_ts_pos_from(plat->at_ts_frames, plat->at_ts_sys_us, now_us, speed, rate)
                 : (a->paused ? head_us : at_head_estimate(plat, now_us, head_us, speed));
    plat->at_src = use_ts ? 3 : (plat->at_ph_n > 0 && !a->paused ? 2 : 1);
    if (plat->at_wr_frames > 0) {
        int64_t max_us = at_frames_to_us(plat->at_wr_frames, rate);
        if (pos >= max_us) {
            /* 不超过已写入：重置同步参数与轮询器（Media3 同） */
            at_reset(plat, 0);
            *pos_us = max_us;
            return 0;
        }
    }
    if (!a->paused && plat->at_last_sys_us != 0) {
        /* 位置平滑：漂移 <1s 且有变化时，限制相对预期 ±10% 的速度变化 */
        int64_t el = now_us - plat->at_last_sys_us;
        int64_t pdiff = pos - plat->at_last_pos_us;
        int64_t exp_diff = (int64_t)((double)el * speed);
        int64_t exp_pos = plat->at_last_pos_us + exp_diff;
        int64_t drift = exp_pos - pos;
        if (drift < 0) {
            drift = -drift;
        }
        if (pdiff != 0 && drift < AT_SMOOTH_DRIFT_US) {
            int64_t maxd = exp_diff * AT_SMOOTH_PCT / 100;
            if (pos < exp_pos - maxd) {
                pos = exp_pos - maxd;
            } else if (pos > exp_pos + maxd) {
                pos = exp_pos + maxd;
            }
        }
    }
    if (!a->paused) {
        plat->at_last_sys_us = now_us;
        plat->at_last_pos_us = pos;
    }
    *pos_us = pos;
    return 0;
}

void ms_hw_audio_android_sync_info(const ms_hw_audio *a, int *src, int64_t *latency_us)
{
    audio_plat *plat = (a != NULL) ? (audio_plat *)a->plat : NULL;
    if (plat == NULL || !plat->track_ready) {
        return;
    }
    if (src) {
        *src = plat->at_src;
    }
    if (latency_us) {
        *latency_us = plat->at_lat_us;
    }
}


void ms_hw_audio_android_flush(ms_hw_audio *a)
{
    audio_plat *plat;
    JNIEnv *env = NULL;
    if (a == NULL || a->plat == NULL) {
        return;
    }
    plat = (audio_plat *)a->plat;
    plat_writer_drop(plat);
    plat_clear_pcm_left(plat);
#ifdef MS_SINGLE_SO
    plat->pt_left_frames = 0;
    if (plat->iec_on) {
        ms_codec ic = plat->iec.codec;
        ms_pt_iec_free(&plat->iec);
        plat->iec_on = (ms_pt_iec_init(&plat->iec, ic) == 0); /* flush 丢弃 E-AC-3 聚合残帧 */
    }
#endif
    plat_reset_head(plat);
    if (plat->jni_mode && plat->jcodec) {
        JNIEnv *e2 = NULL;
        if (ms_hw_jni_env(&e2) && e2) {
            jclass cl = (*e2)->GetObjectClass(e2, plat->jcodec);
            jmethodID flc = cl ? (*e2)->GetMethodID(e2, cl, "flush", "()V") : NULL;
            if (flc) {
                (*e2)->CallVoidMethod(e2, plat->jcodec, flc);
                audio_jni_clear_exc(e2);
            }
            if (cl) {
                (*e2)->DeleteLocalRef(e2, cl);
            }
            (void)audio_jni_refresh_bufs(plat, e2, 0);
            (void)audio_jni_refresh_bufs(plat, e2, 1);
        }
    } else if (plat->codec) {
        AMediaCodec_flush(plat->codec);
    }
    if (plat->track_ready && plat->track && ms_hw_jni_env(&env) && env) {
        jclass cl = (*env)->GetObjectClass(env, plat->track);
        jmethodID pause = cl ? (*env)->GetMethodID(env, cl, "pause", "()V") : NULL;
        jmethodID fl = cl ? (*env)->GetMethodID(env, cl, "flush", "()V") : NULL;
        jmethodID play = cl ? (*env)->GetMethodID(env, cl, "play", "()V") : NULL;
        /* 处于 play 时 flush 可能被忽略；先 pause 再 flush，播放头归零。 */
        if (pause) {
            (*env)->CallVoidMethod(env, plat->track, pause);
            (*env)->ExceptionClear(env);
        }
        if (fl) {
            (*env)->CallVoidMethod(env, plat->track, fl);
            (*env)->ExceptionClear(env);
        }
        if (play && !a->paused) {
            (*env)->CallVoidMethod(env, plat->track, play);
            (*env)->ExceptionClear(env);
        }
        if (cl) {
            (*env)->DeleteLocalRef(env, cl);
        }
    }
}

static void note_track_write(ms_hw_audio *a, int wr)
{
    unsigned bpf;
    if (a == NULL || wr <= 0) {
        return;
    }
    a->track_write_bytes += (uint64_t)wr;
    bpf = 0;
    if (a->channels > 0 && a->pcm_bits >= 8) {
        bpf = (unsigned)a->channels * ((unsigned)a->pcm_bits / 8u);
    }
    if (bpf > 0) {
        a->track_write_frames += (uint64_t)wr / (uint64_t)bpf;
        if (a->plat) {
            ((audio_plat *)a->plat)->at_wr_frames += (int64_t)((uint64_t)wr / (uint64_t)bpf);
        }
    }
}

static int audio_sink_is_stalled(ms_hw_audio *a)
{
    int64_t now;
    if (a == NULL || a->paused) {
        return 0;
    }
    now = audio_mono_ns();
    if (now <= 0) {
        return 0;
    }
    if (a->sink_last_accept_ns == 0) {
        a->sink_last_accept_ns = now;
        return 0;
    }
    return (now - a->sink_last_accept_ns) >= MS_HW_AUDIO_CLOCK_STALL_NS;
}

static int plat_write_pcm(ms_hw_audio *a, JNIEnv *env, const uint8_t *data, size_t n)
{
    audio_plat *plat = (audio_plat *)a->plat;
    jint wr;
    if (data == NULL || n == 0 || !plat || !plat->track) {
        return 0;
    }
    wr = track_write(plat, data, (int)n);
    if (wr < 0) {
        wr = 0;
    }
    if (wr > 0) {
        a->sink_last_accept_ns = audio_mono_ns();
    } else if (audio_sink_is_stalled(a)) {
        /* 输出设备长时间不消费（如 AVD -no-audio）：丢弃本段 PCM 并计入写入，
         * 以免解码器输出排满、解复用停在音频 LIMIT，视频也被拖死。 */
        wr = (jint)n;
        note_track_write(a, (int)wr);
        return 0;
    }
    note_track_write(a, (int)wr);
    if ((size_t)wr < n) {
        size_t left = n - (size_t)wr;
        a->partial_writes++;
        if (left > MS_HW_AUDIO_PCM_LEFT_MAX) {
            left = MS_HW_AUDIO_PCM_LEFT_MAX;
        }
        plat_clear_pcm_left(plat);
        plat->pcm_left = (uint8_t *)malloc(left);
        if (plat->pcm_left == NULL) {
            return -1;
        }
        memcpy(plat->pcm_left, data + (size_t)wr, left);
        plat->pcm_left_cap = left;
        plat->pcm_left_n = left;
        plat->pcm_left_off = 0;
        return 1;
    }
    return 0;
}

#ifdef MS_SINGLE_SO
/* 直通写出：原始帧或 IEC61937 突发；部分写剩余放 pcm_left，写完再计入帧数（时钟按 PCM 帧）。
 * 坏帧直接丢弃（计 au_played），不打断播放。 */
static void pt_note_frames(ms_hw_audio *a, int wr, int frames)
{
    a->track_write_bytes += (uint64_t)(wr > 0 ? wr : 0);
    if (frames > 0) {
        a->track_write_frames += (uint64_t)frames;
        if (a->plat) {
            ((audio_plat *)a->plat)->at_wr_frames += frames;
        }
    }
}

static ms_status pt_drive(ms_hw_audio *a, audio_plat *plat)
{
    unsigned i;
    for (i = 0; i < a->in_cap; i++) {
        ms_hw_audio_slot *s = &a->slots[i];
        const uint8_t *buf = NULL;
        size_t bn = 0;
        int frames = 0;
        jint wr;
        if (plat->pcm_left != NULL && plat->pcm_left_off < plat->pcm_left_n) {
            size_t left = plat->pcm_left_n - plat->pcm_left_off;
            wr = track_write(plat, plat->pcm_left + plat->pcm_left_off, (int)left);
            if (wr <= 0 && audio_sink_is_stalled(a)) {
                wr = (jint)left;
            }
            if (wr <= 0) {
                break;
            }
            a->sink_last_accept_ns = audio_mono_ns();
            plat->pcm_left_off += (size_t)wr;
            if (plat->pcm_left_off < plat->pcm_left_n) {
                pt_note_frames(a, (int)wr, 0);
                break;
            }
            pt_note_frames(a, (int)wr, plat->pt_left_frames);
            plat->pt_left_frames = 0;
            plat_clear_pcm_left(plat);
        }
        if (!s->occupied) {
            continue;
        }
        if (s->data == NULL || s->size == 0) {
            release_slot(a, s);
            continue;
        }
        if (plat->iec_on) {
            int r = ms_pt_iec_feed(&plat->iec, s->data, s->size, &buf, &bn, &frames);
            if (r <= 0) {
                /* 0：E-AC-3 聚合中；<0：坏帧丢弃。 */
                a->au_played++;
                release_slot(a, s);
                continue;
            }
        } else {
            frames = ms_pt_frame_samples(a->caps.codec, s->data, s->size);
            if (frames <= 0) {
                a->au_played++;
                release_slot(a, s);
                continue;
            }
            buf = s->data;
            bn = s->size;
        }
        wr = track_write(plat, buf, (int)bn);
        if (wr < 0) {
            wr = 0;
        }
        if (wr == 0 && audio_sink_is_stalled(a)) {
            wr = (jint)bn;
        }
        if (wr == 0 && !plat->iec_on) {
            break; /* 原始帧未被接受：保留槽位下次再写 */
        }
        if (wr > 0) {
            a->sink_last_accept_ns = audio_mono_ns();
        }
        if ((size_t)wr < bn) {
            size_t left = bn - (size_t)wr;
            a->partial_writes++;
            plat_clear_pcm_left(plat);
            plat->pcm_left = (uint8_t *)malloc(left);
            if (plat->pcm_left != NULL) {
                memcpy(plat->pcm_left, buf + (size_t)wr, left);
                plat->pcm_left_cap = left;
                plat->pcm_left_n = left;
                plat->pcm_left_off = 0;
                plat->pt_left_frames = frames;
                pt_note_frames(a, (int)wr, 0);
            } else {
                pt_note_frames(a, (int)wr, frames);
            }
        } else {
            pt_note_frames(a, (int)wr, frames);
        }
        a->au_played++;
        release_slot(a, s);
        if (plat->pcm_left != NULL) {
            break;
        }
    }
    return MS_OK;
}
#endif

ms_status ms_hw_audio_android_drive(ms_hw_audio *a)
{
    audio_plat *plat;
    unsigned i;
    if (a == NULL || a->plat == NULL) {
        return MS_OK;
    }
    plat = (audio_plat *)a->plat;
    if (a->pcm_direct) {
        if (!plat->track_ready &&
            ensure_track(a, a->sample_rate, a->channels) != 0) {
            a->last_error = MS_ERR_UNSUPPORTED;
            return MS_ERR_UNSUPPORTED;
        }
#ifdef MS_SINGLE_SO
        if (a->pt_route != MS_PT_ROUTE_SOFT) {
            return pt_drive(a, plat);
        }
#endif
#ifdef MS_SINGLE_FFADEC
        if (plat->ffadec != NULL) {
            /* 先写完上次部分写剩余的 PCM，再解下一个 AU；有界，不丢不重。 */
            for (i = 0; i < a->in_cap; i++) {
                ms_hw_audio_slot *s = &a->slots[i];
                const uint8_t *pcm = NULL;
                size_t pn = 0;
                if (plat->pcm_left != NULL && plat->pcm_left_off < plat->pcm_left_n) {
                    size_t left = plat->pcm_left_n - plat->pcm_left_off;
                    jint wr = track_write(plat, plat->pcm_left + plat->pcm_left_off, (int)left);
                    if (wr <= 0 && audio_sink_is_stalled(a)) {
                        wr = (jint)left;
                    }
                    if (wr <= 0) {
                        break;
                    }
                    a->sink_last_accept_ns = audio_mono_ns();
                    note_track_write(a, (int)wr);
                    plat->pcm_left_off += (size_t)wr;
                    if (plat->pcm_left_off < plat->pcm_left_n) {
                        break;
                    }
                    plat_clear_pcm_left(plat);
                }
                if (!s->occupied) {
                    continue;
                }
                if (s->data != NULL && s->size > 0 &&
                    ms_ffadec_decode((ms_ffadec *)plat->ffadec, s->data, s->size, &pcm, &pn) == 0 &&
                    pn > 0) {
                    jint wr = track_write(plat, pcm, (int)pn);
                    if (wr < 0) {
                        wr = 0;
                    }
                    if (wr == 0 && audio_sink_is_stalled(a)) {
                        wr = (jint)pn;
                    }
                    if (wr > 0) {
                        a->sink_last_accept_ns = audio_mono_ns();
                        note_track_write(a, (int)wr);
                    }
                    if ((size_t)wr < pn) {
                        size_t left = pn - (size_t)wr;
                        a->partial_writes++;
                        if (left > MS_HW_AUDIO_PCM_LEFT_MAX) {
                            left = MS_HW_AUDIO_PCM_LEFT_MAX;
                        }
                        plat_clear_pcm_left(plat);
                        plat->pcm_left = (uint8_t *)malloc(left);
                        if (plat->pcm_left != NULL) {
                            memcpy(plat->pcm_left, pcm + (size_t)wr, left);
                            plat->pcm_left_cap = left;
                            plat->pcm_left_n = left;
                            plat->pcm_left_off = 0;
                        }
                    }
                }
                a->au_played++;
                release_slot(a, s);
            }
            return MS_OK;
        }
#endif
        for (i = 0; i < a->in_cap; i++) {
            ms_hw_audio_slot *s = &a->slots[i];
            if (!s->occupied || s->data == NULL || s->size == 0) {
                if (s->occupied) {
                    release_slot(a, s);
                }
                continue;
            }
            {
                jint wr = track_write(plat, s->data, (int)s->size);
                if (wr < 0) {
                    wr = 0;
                }
                if (wr > 0) {
                    a->sink_last_accept_ns = audio_mono_ns();
                } else if (audio_sink_is_stalled(a)) {
                    /* 与压缩音频的 plat_write_pcm 相同：AVD -no-audio 时
                     * 设备不消费，仍计入写出，避免 write_frames 一直为 0。 */
                    wr = (jint)s->size;
                    note_track_write(a, (int)wr);
                    a->au_played++;
                    release_slot(a, s);
                    continue;
                } else {
                    /* 尚未超时，留着槽位下次再写。 */
                    break;
                }
                note_track_write(a, (int)wr);
                if ((size_t)wr < s->size) {
                    a->partial_writes++;
                }
            }
            a->au_played++;
            release_slot(a, s);
        }
        return MS_OK;
    }
    if (plat->codec == NULL && !plat->jni_mode) {
        return MS_OK;
    }
    /* leftover 存在且输出设备已停：先丢弃再继续取解码输出。不得把 discard
     * 绑在 JNI write 成功路径上，否则写线程卡住时 write_frames/解码器都停住。 */
    if (plat->pcm_left != NULL && plat->pcm_left_off < plat->pcm_left_n &&
        audio_sink_is_stalled(a)) {
        size_t left = plat->pcm_left_n - plat->pcm_left_off;
        plat_writer_drop(plat);
        if (left > 0) {
            note_track_write(a, (int)left);
        }
        plat_clear_pcm_left(plat);
    }
    if (plat->pcm_left != NULL && plat->pcm_left_off < plat->pcm_left_n) {
        JNIEnv *env = NULL;
        if (ms_hw_jni_env(&env) && env && plat->track) {
            const uint8_t *p = plat->pcm_left + plat->pcm_left_off;
            size_t left = plat->pcm_left_n - plat->pcm_left_off;
            jint wr = track_write(plat, p, (int)left);
            if (wr < 0) {
                wr = 0;
            }
            if (wr > 0) {
                a->sink_last_accept_ns = audio_mono_ns();
            } else if (audio_sink_is_stalled(a)) {
                wr = (jint)left;
            }
            note_track_write(a, (int)wr);
            if ((size_t)wr < left) {
                a->partial_writes++;
            }
            plat->pcm_left_off += (size_t)((wr > 0) ? wr : 0);
            if (plat->pcm_left_off >= plat->pcm_left_n) {
                plat_clear_pcm_left(plat);
            }
        }
    }
    /* 提交待处理压缩 AU 到 MediaCodec 输入。 */
    for (i = 0; i < a->in_cap; i++) {
        ms_hw_audio_slot *s = &a->slots[i];
        ssize_t ix;
        size_t cap = 0;
        uint8_t *dst;
        if (!s->occupied) {
            continue;
        }
        if (ms_hw_pcm_trim_pending_meta(&a->trim) >= MS_HW_PCM_TRIM_META_MAX) {
            break;
        }
        ix = audio_mc_dq_in(plat, 0);
        if (ix < 0) {
            break;
        }
        dst = audio_mc_get_in(plat, (size_t)ix, &cap);
        if (dst == NULL || cap < s->size) {
            break;
        }
        if (s->size > 0 && s->data != NULL) {
            memcpy(dst, s->data, s->size);
        }
        {
            ms_status pst = push_meta_for_slot(a, s);
            if (pst != MS_OK) {
                a->last_error = pst;
                return pst;
            }
        }
        audio_mc_q_in(plat, (size_t)ix, 0, s->size,
                                      (uint64_t)(s->pts_us < 0 ? 0 : s->pts_us), 0);
        a->au_played++; /* 先计提交到平台；真正时钟仍用 playback head */
        release_slot(a, s);
    }
    /* 取 PCM 并写入 AudioTrack；部分 write 计入 partial_writes。
     * leftover 未排空时不再取新输出，避免覆盖未写出的已修剪 PCM。 */
    if (plat->pcm_left != NULL && plat->pcm_left_off < plat->pcm_left_n) {
        return MS_OK;
    }
    for (;;) {
        AMediaCodecBufferInfo info;
        ssize_t ox = audio_mc_dq_out(plat, &info, 0);
        size_t cap = 0;
        uint8_t *src;
        JNIEnv *env = NULL;
        if (ox == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) {
            continue;
        }
        if (ox == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
            int32_t rate = a->sample_rate, ch = a->channels;
            (void)audio_mc_on_format(plat, &rate, &ch);
            if (rate > 0 && ch > 0 && (rate != a->sample_rate || ch != a->channels)) {
                size_t bpf;
                ms_status fst;
                a->sample_rate = rate;
                a->channels = ch;
                a->pcm_format_changes++;
                bpf = audio_bpf(a);
                fst = ms_hw_pcm_trim_set_bpf(&a->trim, bpf);
                if (fst != MS_OK) {
                    a->last_error = fst;
                    return fst;
                }
            }
            if (ensure_track(a, a->sample_rate, a->channels) != 0) {
                a->last_error = MS_ERR_UNSUPPORTED;
                return MS_ERR_UNSUPPORTED;
            }
            continue;
        }
        if (ox == AMEDIACODEC_INFO_TRY_AGAIN_LATER || ox < 0) {
            break;
        }
        if (!plat->track_ready && ensure_track(a, a->sample_rate, a->channels) != 0) {
            audio_mc_rel_out(plat, (size_t)ox, 0);
            a->last_error = MS_ERR_UNSUPPORTED;
            return MS_ERR_UNSUPPORTED;
        }
        src = audio_mc_get_out(plat, (size_t)ox, &cap);
        {
            const uint8_t *pcm = (src && info.size > 0) ? (src + info.offset) : NULL;
            size_t nbytes = (src && info.size > 0) ? (size_t)info.size : 0;
            size_t bpf = audio_bpf(a);
            uint32_t nsamp = (bpf > 0) ? (uint32_t)(nbytes / bpf) : 0;
            size_t out_bytes = 0;
            size_t need;
            ms_status tst;
            need = nbytes + (size_t)MS_HW_PCM_TRIM_HOLD_MAX * MS_HW_PCM_TRIM_BPF_MAX;
            tst = ensure_trim_out(a, need);
            if (tst != MS_OK) {
                audio_mc_rel_out(plat, (size_t)ox, 0);
                a->last_error = tst;
                return tst;
            }
            /* 空输出也 feed：nsamp=0 不 pop 元数据。不要按输出块 pop 一条输入标记。 */
            tst = ms_hw_pcm_trim_feed(&a->trim, pcm, nsamp,
                                      1, (int64_t)info.presentationTimeUs,
                                      a->trim_out, a->trim_out_cap, &out_bytes);
            if (tst != MS_OK) {
                audio_mc_rel_out(plat, (size_t)ox, 0);
                a->last_error = tst;
                return tst;
            }
            {
                int tap_has = 0;
                int64_t tap_au = 0;
                uint32_t tap_idx = 0, tap_ns = 0;
                if (out_bytes > 0) {
                    ms_hw_pcm_trim_last_out(&a->trim, &tap_has, &tap_au, &tap_idx, &tap_ns);
                }
                if ((info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) != 0) {
                    size_t more = 0;
                    tst = ms_hw_pcm_trim_eos(&a->trim, a->trim_out + out_bytes,
                                             a->trim_out_cap - out_bytes, &more);
                    if (tst != MS_OK) {
                        audio_mc_rel_out(plat, (size_t)ox, 0);
                        a->last_error = tst;
                        return tst;
                    }
                    if (out_bytes == 0 && more > 0) {
                        ms_hw_pcm_trim_last_out(&a->trim, &tap_has, &tap_au, &tap_idx, &tap_ns);
                    }
                    out_bytes += more;
                }
                if (out_bytes > 0) {
                    audio_tap_trimmed_pts(a, a->trim_out, out_bytes, tap_has, tap_au, tap_idx);
                }
                (void)tap_ns;
            }
            if (out_bytes > 0 && ms_hw_jni_env(&env) && env && plat->track) {
                if (plat_write_pcm(a, env, a->trim_out, out_bytes) > 0) {
                    audio_mc_rel_out(plat, (size_t)ox, 0);
                    if (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) {
                        a->eos_out = 1;
                    }
                    break;
                }
            }
        }
        audio_mc_rel_out(plat, (size_t)ox, 0);
        if (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) {
            a->eos_out = 1;
            break;
        }
    }
    if (a->eos_in && a->pending == 0) {
        ssize_t ix = audio_mc_dq_in(plat, 0);
        if (ix >= 0) {
            audio_mc_q_in(plat, (size_t)ix, 0, 0, 0,
                                          AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
        }
    }
    (void)audio_vm;
    return MS_OK;
}

ms_status ms_hw_audio_android_set_rate(ms_hw_audio *a, int num, int den)
{
    audio_plat *plat;
    JNIEnv *env = NULL;
    jclass cl;
    int api;
    float speed;
    if (a == NULL || num <= 0 || den <= 0) {
        return MS_ERR_INVALID_ARG;
    }
    plat = (audio_plat *)a->plat;
    if (a->pt_route != MS_PT_ROUTE_SOFT && (num != den)) {
        return MS_ERR_UNSUPPORTED; /* 直通码流不能变速，交由上层回退或拒绝 */
    }
    if (plat == NULL || !plat->track_ready || plat->track == NULL) {
        return MS_OK; /* 速率记下，轨道就绪后再套用 */
    }
    if (!ms_hw_jni_env(&env) || env == NULL) {
        return MS_ERR_UNSUPPORTED;
    }
    api = a->caps.api_level > 0 ? a->caps.api_level : a->opts.api_level;
    speed = (float)num / (float)den;
    cl = (*env)->GetObjectClass(env, plat->track);
    if (cl == NULL) {
        return MS_ERR_UNSUPPORTED;
    }
    if (api >= 23) {
        jclass ppc = (*env)->FindClass(env, "android/media/PlaybackParams");
        jmethodID ctor, setSpeed, setPitch, setPP;
        jobject pp;
        if (ppc == NULL) {
            (*env)->ExceptionClear(env);
            (*env)->DeleteLocalRef(env, cl);
            return MS_ERR_UNSUPPORTED;
        }
        ctor = (*env)->GetMethodID(env, ppc, "<init>", "()V");
        setSpeed = (*env)->GetMethodID(env, ppc, "setSpeed",
                                       "(F)Landroid/media/PlaybackParams;");
        setPitch = (*env)->GetMethodID(env, ppc, "setPitch",
                                       "(F)Landroid/media/PlaybackParams;");
        setPP = (*env)->GetMethodID(env, cl, "setPlaybackParams",
                                    "(Landroid/media/PlaybackParams;)V");
        if (!ctor || !setSpeed || !setPitch || !setPP) {
            (*env)->ExceptionClear(env);
            (*env)->DeleteLocalRef(env, ppc);
            (*env)->DeleteLocalRef(env, cl);
            return MS_ERR_UNSUPPORTED;
        }
        pp = (*env)->NewObject(env, ppc, ctor);
        if (pp == NULL || (*env)->ExceptionCheck(env)) {
            (*env)->ExceptionClear(env);
            (*env)->DeleteLocalRef(env, ppc);
            (*env)->DeleteLocalRef(env, cl);
            return MS_ERR_UNSUPPORTED;
        }
        pp = (*env)->CallObjectMethod(env, pp, setSpeed, (jfloat)speed);
        if (pp == NULL || (*env)->ExceptionCheck(env)) {
            (*env)->ExceptionClear(env);
            (*env)->DeleteLocalRef(env, ppc);
            (*env)->DeleteLocalRef(env, cl);
            return MS_ERR_UNSUPPORTED;
        }
        pp = (*env)->CallObjectMethod(env, pp, setPitch, (jfloat)1.0f);
        if (pp == NULL || (*env)->ExceptionCheck(env)) {
            (*env)->ExceptionClear(env);
            (*env)->DeleteLocalRef(env, ppc);
            (*env)->DeleteLocalRef(env, cl);
            return MS_ERR_UNSUPPORTED;
        }
        (*env)->CallVoidMethod(env, plat->track, setPP, pp);
        if ((*env)->ExceptionCheck(env)) {
            (*env)->ExceptionClear(env);
            (*env)->DeleteLocalRef(env, pp);
            (*env)->DeleteLocalRef(env, ppc);
            (*env)->DeleteLocalRef(env, cl);
            return MS_ERR_UNSUPPORTED;
        }
        (*env)->DeleteLocalRef(env, pp);
        (*env)->DeleteLocalRef(env, ppc);
        (*env)->DeleteLocalRef(env, cl);
        return MS_OK;
    } else {
        jmethodID mid = (*env)->GetMethodID(env, cl, "setPlaybackRate", "(I)I");
        jint want, got;
        int sr = a->sample_rate > 0 ? a->sample_rate : 48000;
        if (!mid) {
            (*env)->ExceptionClear(env);
            (*env)->DeleteLocalRef(env, cl);
            return MS_ERR_UNSUPPORTED;
        }
        /* 旧 API：变调改采样率，范围通常 1Hz～2×native。超出则诚实失败。 */
        if (speed > 2.0f + 1e-4f || (int)((float)sr * speed) < 1) {
            (*env)->DeleteLocalRef(env, cl);
            return MS_ERR_UNSUPPORTED;
        }
        want = (jint)((float)sr * speed);
        if (want < 1) {
            want = 1;
        }
        got = (*env)->CallIntMethod(env, plat->track, mid, want);
        if ((*env)->ExceptionCheck(env) || got <= 0) {
            (*env)->ExceptionClear(env);
            (*env)->DeleteLocalRef(env, cl);
            return MS_ERR_UNSUPPORTED;
        }
        (*env)->DeleteLocalRef(env, cl);
        return MS_OK;
    }
}

void ms_hw_audio_android_set_paused(ms_hw_audio *a, int paused)
{
    audio_plat *plat;
    JNIEnv *env = NULL;
    jclass cl;
    jmethodID mid;
    if (a == NULL || a->plat == NULL) {
        return;
    }
    plat = (audio_plat *)a->plat;
    if (!plat->track_ready || plat->track == NULL) {
        return;
    }
    if (!ms_hw_jni_env(&env) || env == NULL) {
        return;
    }
    cl = (*env)->GetObjectClass(env, plat->track);
    mid = cl ? (*env)->GetMethodID(env, cl, paused ? "pause" : "play", "()V") : NULL;
    if (mid) {
        (*env)->CallVoidMethod(env, plat->track, mid);
        (*env)->ExceptionClear(env);
    } else if (cl) {
        (*env)->ExceptionClear(env);
    }
    if (cl) {
        (*env)->DeleteLocalRef(env, cl);
    }
    at_reset(plat, 0); /* Media3 pause()/start() 均重置同步参数与时间戳轮询器 */
}

#endif /* __ANDROID__ */


static int rate_cmp(int n1, int d1, int n2, int d2)
{
#if defined(__SIZEOF_INT128__)
    __int128 a = (__int128)n1 * (d2);
    __int128 b = (__int128)n2 * (d1);
    return (a < b) ? -1 : (a > b) ? 1 : 0;
#else
    double a = (double)n1 / (double)d1;
    double b = (double)n2 / (double)d2;
    if (a < b) {
        return -1;
    }
    if (a > b) {
        return 1;
    }
    return 0;
#endif
}

static void fill_rate_caps(int api_level, int is_mock, ms_hw_rate_caps *out)
{
    memset(out, 0, sizeof(*out));
    out->struct_size = sizeof(*out);
    out->can_apply = 1;
    out->api_level = api_level;
    out->mock_is_not_device = is_mock ? 1 : 0;
    if (is_mock) {
        out->can_sync_av = 0;
        out->can_keep_pitch = 0;
        out->min_num = 1;
        out->min_den = 8;
        out->max_num = 8;
        out->max_den = 1;
        out->backend = "mock";
        out->detail = "host mock rate bookkeeping only; not device A/V-synced speed";
        return;
    }
    if (api_level >= 23) {
        out->can_sync_av = 1;
        out->can_keep_pitch = 1;
        out->min_num = 1;
        out->min_den = 4;
        out->max_num = 2;
        out->max_den = 1;
        out->backend = "audiotrack-playbackparams";
        out->detail = "API 23+ AudioTrack.setPlaybackParams (keep pitch); out-of-range or rejection is UNSUPPORTED, not video-clock-only";
        return;
    }
    out->can_sync_av = 1;
    out->can_keep_pitch = 0;
    out->min_num = 1;
    out->min_den = 2;
    out->max_num = 2;
    out->max_den = 1;
    out->backend = "audiotrack-setplaybackrate";
    out->detail = "API <23 AudioTrack.setPlaybackRate is pitch-changing, typically up to 2x; not time-stretch; unsupported rates error";
}

ms_status ms_hw_audio_get_rate_caps(const ms_hw_audio *a, ms_hw_rate_caps *out)
{
    ms_hw_rate_caps tmp;
    size_t sz;
    int api;
    int mock;
    if (a == NULL || out == NULL || out->struct_size < sizeof(size_t)) {
        return MS_ERR_INVALID_ARG;
    }
#ifdef __ANDROID__
    api = a->caps.api_level > 0 ? a->caps.api_level : a->opts.api_level;
    mock = 0;
#else
    api = 0;
    mock = 1;
    (void)api;
#endif
    fill_rate_caps(api, mock, &tmp);
    sz = out->struct_size;
    if (sz > sizeof(tmp)) {
        sz = sizeof(tmp);
    }
    memcpy(out, &tmp, sz);
    out->struct_size = sz;
    return MS_OK;
}

ms_status ms_hw_audio_set_rate(ms_hw_audio *a, int num, int den)
{
    ms_hw_rate_caps caps;
    ms_status st;
    if (a == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    if (num <= 0 || den <= 0) {
        return MS_ERR_INVALID_ARG;
    }
    memset(&caps, 0, sizeof(caps));
    caps.struct_size = sizeof(caps);
    st = ms_hw_audio_get_rate_caps(a, &caps);
    if (st != MS_OK) {
        return st;
    }
    if (rate_cmp(num, den, caps.min_num, caps.min_den) < 0 ||
        rate_cmp(num, den, caps.max_num, caps.max_den) > 0) {
        return MS_ERR_UNSUPPORTED;
    }
#ifdef __ANDROID__
    st = ms_hw_audio_android_set_rate(a, num, den);
    if (st != MS_OK) {
        return st;
    }
#endif
    a->rate_num = num;
    a->rate_den = den;
    return MS_OK;
}

ms_status ms_hw_audio_pause(ms_hw_audio *a)
{
    if (a == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    a->paused = 1;
    a->clock_fallback_origin_ns = 0;
#ifdef __ANDROID__
    ms_hw_audio_android_set_paused(a, 1);
#endif
    return MS_OK;
}

ms_status ms_hw_audio_resume(ms_hw_audio *a)
{
    if (a == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    a->paused = 0;
    a->clock_fallback_origin_ns = 0;
    a->clock_mono_last_change_ns = audio_mono_ns();
#ifdef __ANDROID__
    ms_hw_audio_android_set_paused(a, 0);
#endif
    return MS_OK;
}


ms_status ms_hw_audio_debug_set_dec_pat(ms_hw_audio *a, const ms_hw_audio_dec_pat *p)
{
#ifdef __ANDROID__
    (void)a;
    (void)p;
    return MS_ERR_UNSUPPORTED;
#else
    if (a == NULL || p == NULL || p->struct_size < sizeof(size_t)) {
        return MS_ERR_INVALID_ARG;
    }
    a->dec_mode = 0;
    a->dec_split_n = 0;
    a->dec_merge_n = 0;
    a->dec_empty_n = 0;
    a->dec_delay = 0;
    memset(a->dec_split, 0, sizeof(a->dec_split));
    if (p->struct_size >= offsetof(ms_hw_audio_dec_pat, mode) + sizeof(p->mode)) {
        a->dec_mode = p->mode;
    }
    if (p->struct_size >= offsetof(ms_hw_audio_dec_pat, split_n) + sizeof(p->split_n)) {
        unsigned n = p->split_n;
        if (n > 8u) {
            n = 8u;
        }
        a->dec_split_n = n;
        memcpy(a->dec_split, p->split, n * sizeof(p->split[0]));
    }
    if (p->struct_size >= offsetof(ms_hw_audio_dec_pat, merge_n) + sizeof(p->merge_n)) {
        a->dec_merge_n = p->merge_n;
    }
    if (p->struct_size >= offsetof(ms_hw_audio_dec_pat, empty_n) + sizeof(p->empty_n)) {
        a->dec_empty_n = p->empty_n;
    }
    if (p->struct_size >= offsetof(ms_hw_audio_dec_pat, delay_drives) +
        sizeof(p->delay_drives)) {
        a->dec_delay = p->delay_drives;
    }
    a->dec_delay_left = a->dec_delay;
    return MS_OK;
#endif
}

void ms_hw_audio_debug_set_pcm_sink(ms_hw_audio *a, int16_t *frames,
                                    size_t cap_frames, size_t *out_frames)
{
#ifdef __ANDROID__
    (void)a;
    (void)frames;
    (void)cap_frames;
    (void)out_frames;
#else
    if (a == NULL) {
        return;
    }
    a->sink = frames;
    a->sink_cap = cap_frames;
    a->sink_out = out_frames;
    if (out_frames != NULL) {
        *out_frames = 0;
    }
#endif
}


ms_status ms_hw_audio_debug_set_platform_trims_delay(ms_hw_audio *a, int on)
{
#ifdef __ANDROID__
    (void)a;
    (void)on;
    return MS_ERR_UNSUPPORTED;
#else
    if (a == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    if (on) {
        uint32_t xfer = a->start_fallback;
        if (a->seek_warmup_left > 0xFFFFFFFFu - xfer) {
            xfer = 0xFFFFFFFFu;
        } else {
            xfer += a->seek_warmup_left;
        }
        a->platform_trims_delay = 1;
        if (a->platform_lead_left > 0xFFFFFFFFu - xfer) {
            a->platform_lead_left = 0xFFFFFFFFu;
        } else {
            a->platform_lead_left += xfer;
        }
        a->start_fallback = 0;
        a->seek_warmup_left = 0;
        a->preroll_left = 0;
    } else {
        a->platform_trims_delay = 0;
    }
    return MS_OK;
#endif
}

void ms_hw_audio_set_pcm_tap(ms_hw_audio *a, ms_hw_audio_pcm_tap_fn fn, void *ctx)
{
    if (a == NULL) {
        return;
    }
    a->pcm_tap = fn;
    a->pcm_tap_ctx = ctx;
}


/* 加法：把系统 MediaPlayer.setVolume 落到 AudioTrack；默认 1/1。宿主无 AudioTrack，只接受参数。 */
ms_status ms_hw_audio_set_volume(ms_hw_audio *a, float left, float right)
{
#ifndef __ANDROID__
    (void)left;
    (void)right;
    return a == NULL ? MS_ERR_INVALID_ARG : MS_OK;
#else
    audio_plat *plat;
    JNIEnv *env = NULL;
    if (a == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    if (left < 0.f) left = 0.f;
    if (right < 0.f) right = 0.f;
    if (left > 1.f) left = 1.f;
    if (right > 1.f) right = 1.f;
    plat = (audio_plat *)a->plat;
    if (plat == NULL) {
        return MS_OK;
    }
    plat->vol_left = left;
    plat->vol_right = right;
    plat->vol_set = 1;
    if (plat->track && ms_hw_jni_env(&env) && env) {
        jclass cl = (*env)->GetObjectClass(env, plat->track);
        jmethodID mid = NULL;
        if (cl) {
            mid = (*env)->GetMethodID(env, cl, "setStereoVolume", "(FF)I");
            if (mid) {
                (*env)->CallIntMethod(env, plat->track, mid, left, right);
                (*env)->ExceptionClear(env);
            } else {
                (*env)->ExceptionClear(env);
                mid = (*env)->GetMethodID(env, cl, "setVolume", "(F)V");
                if (mid) {
                    float v = left < right ? left : right;
                    (*env)->CallVoidMethod(env, plat->track, mid, v);
                    (*env)->ExceptionClear(env);
                } else {
                    (*env)->ExceptionClear(env);
                }
            }
            (*env)->DeleteLocalRef(env, cl);
        }
    }
    return MS_OK;
#endif
}

/* 阶段 4（内部，单 so 用）：下发 Java 层探测的直通能力位。须在 configure 之前调用；
 * caps 不含 MS_PT_CAP_ENABLED 即关闭直通（默认）。宿主只记录，不出声。 */
void ms_hw_audio_set_passthrough_caps(ms_hw_audio *a, unsigned caps)
{
    if (a != NULL) {
        a->pt_caps = caps;
    }
}

/* 当前是否处于直通（0=软解，1=原始帧，2=IEC61937），以及失败回退次数。 */
int ms_hw_audio_passthrough_route(const ms_hw_audio *a, unsigned *fallbacks)
{
    if (fallbacks != NULL) {
        *fallbacks = (a != NULL) ? a->pt_fallbacks : 0;
    }
#ifdef __ANDROID__
    return (a != NULL) ? a->pt_route : 0;
#else
    return 0;
#endif
}
