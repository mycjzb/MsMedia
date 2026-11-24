/*
 * 解码后 PCM 修剪 carry 状态机：按真实输出样点与元数据区间消耗
 * skip_start / skip_end / 会话前缀（pre-skip、seek warmup）。
 * 不假设 1 个压缩 AU 对应 1 块 PCM 输出。宿主 mock 与 Android 必须复用。
 * 不是扬声器时钟；写出的可听样点由调用方再交给 playback head。
 * by: Long <mycjzb@gmail.com>
 */
#ifndef MSMEDIA_MS_HW_AUDIO_TRIM_H
#define MSMEDIA_MS_HW_AUDIO_TRIM_H

#include "msmedia/msmedia.h"

#include <stddef.h>
#include <stdint.h>

#ifndef MS_HW_PCM_TRIM_META_MAX
#define MS_HW_PCM_TRIM_META_MAX 32u
#endif
#ifndef MS_HW_PCM_TRIM_HOLD_MAX
#define MS_HW_PCM_TRIM_HOLD_MAX 8192u /* 未知 expected 时尾部保留上限（样点） */
#endif
#ifndef MS_HW_PCM_TRIM_BPF_MAX
#define MS_HW_PCM_TRIM_BPF_MAX 16u /* 最多 8ch s16 */
#endif

typedef struct ms_hw_pcm_trim_meta {
    size_t struct_size;
    uint32_t skip_start;
    uint32_t skip_end;
    uint32_t expected_samples; /* 0=未知：须有界保留 skip_end 再在 AU 结束时丢掉 */
    int64_t pts_us;
    int has_pts;
} ms_hw_pcm_trim_meta;

typedef struct ms_hw_pcm_trim {
    size_t bpf;
    uint32_t prefix_left;
    unsigned q_n;
    unsigned q_head;
    int have_cur;
    struct {
        uint32_t start_left;
        uint32_t skip_end;
        uint32_t expected;
        uint32_t seen;
        int64_t pts_us;
        int has_pts;
    } q[MS_HW_PCM_TRIM_META_MAX];
    uint8_t hold[MS_HW_PCM_TRIM_HOLD_MAX * MS_HW_PCM_TRIM_BPF_MAX];
    uint32_t hold_n;
    int overflow;
    /* 最近一次 feed/eos 写出的可听块：AU 起点 PTS + 块内第一可听样点下标。 */
    int out_meta_valid;
    int out_has_pts;
    int64_t out_au_pts_us;
    uint32_t out_first_idx;
    uint32_t out_nsamp;
} ms_hw_pcm_trim;

void ms_hw_pcm_trim_init(ms_hw_pcm_trim *t);
void ms_hw_pcm_trim_reset(ms_hw_pcm_trim *t);
ms_status ms_hw_pcm_trim_set_bpf(ms_hw_pcm_trim *t, size_t bpf);

ms_status ms_hw_pcm_trim_add_prefix(ms_hw_pcm_trim *t, uint32_t samples);
ms_status ms_hw_pcm_trim_push(ms_hw_pcm_trim *t, const ms_hw_pcm_trim_meta *m);

/*
 * 喂入一块解码 PCM（样点为单位）。nsamp=0 不消耗元数据（空输出）。
 * 可选输出块起始 PTS：与当前/下一 AU 对齐或报告不匹配。
 * dst 须能容纳本次可听输出（建议 cap >= (nsamp + hold) * bpf）。
 */
ms_status ms_hw_pcm_trim_feed(ms_hw_pcm_trim *t,
                              const uint8_t *src, uint32_t nsamp,
                              int has_pts, int64_t pts_us,
                              uint8_t *dst, size_t dst_cap, size_t *out_bytes);

/* EOS：对未完成 AU 应用剩余 end skip，排空 hold 中的可听样点。 */
ms_status ms_hw_pcm_trim_eos(ms_hw_pcm_trim *t,
                             uint8_t *dst, size_t dst_cap, size_t *out_bytes);

unsigned ms_hw_pcm_trim_pending_meta(const ms_hw_pcm_trim *t);
uint32_t ms_hw_pcm_trim_hold_samples(const ms_hw_pcm_trim *t);
uint32_t ms_hw_pcm_trim_prefix_left(const ms_hw_pcm_trim *t);
/* 本次 feed/eos 首块可听样点对应的 AU PTS 与样点下标。无效时 has_pts=0 且 nsamp=0。 */
void ms_hw_pcm_trim_last_out(const ms_hw_pcm_trim *t, int *has_pts,
                             int64_t *au_pts_us, uint32_t *first_idx,
                             uint32_t *nsamp);


/* 仅宿主测试：改变 mock「解码」输出形态，仍走本状态机，不另写修剪算法。 */
#define MS_HW_AUDIO_DEC_NATURAL 0
#define MS_HW_AUDIO_DEC_SPLIT 1
#define MS_HW_AUDIO_DEC_MERGE 2
#define MS_HW_AUDIO_DEC_EMPTY_FIRST 3
#define MS_HW_AUDIO_DEC_DELAY 4

typedef struct ms_hw_audio_dec_pat {
    size_t struct_size;
    int mode;
    uint32_t split[8];
    unsigned split_n;
    unsigned merge_n;
    unsigned empty_n;
    unsigned delay_drives;
} ms_hw_audio_dec_pat;

struct ms_hw_audio;
ms_status ms_hw_audio_debug_set_dec_pat(struct ms_hw_audio *a,
                                        const ms_hw_audio_dec_pat *p);
void ms_hw_audio_debug_set_pcm_sink(struct ms_hw_audio *a, int16_t *frames,
                                    size_t cap_frames, size_t *out_frames);
/* 宿主测试：打开后模拟平台已修剪 pre-skip/seek preroll（与 Android 路径同旗标）。
 * 须在首包提交前调用。设备路径返回 UNSUPPORTED。 */
ms_status ms_hw_audio_debug_set_platform_trims_delay(struct ms_hw_audio *a, int on);
/* 宿主反例：把 last_change 往回拨，触发 200ms 停转兜底；不进入产品 ABI。 */
void ms_hw_audio_debug_backdate_clock_change(struct ms_hw_audio *a, int64_t ns_ago);
/* 宿主反例：直接把已发出钟抬到大于 raw，验证 head 恢复时不回退。 */
void ms_hw_audio_debug_force_emitted_clock(struct ms_hw_audio *a, int64_t emitted_us);
int64_t ms_hw_audio_debug_mono_ns(void);
void ms_hw_audio_debug_clock_info(const struct ms_hw_audio *a, int *paused,
                                  int *quota_armed, int64_t *mono_ns,
                                  int64_t *last_raw_us, int64_t *last_chg_ns,
                                  int64_t *emitted_us, int64_t *origin_ns);

#endif /* MSMEDIA_MS_HW_AUDIO_TRIM_H */

