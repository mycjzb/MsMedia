/*
 * 可选 AI 字幕插件框架（转写 / 翻译归并）。默认禁用。
 * 本模块不链入 libmsmedia / libmsff；核心无模型、无云 SDK、无供应商地址。
 * 不自选付费供应商，不调用第三方服务，不上传私有音频，不从密钥库取凭据。
 * 内置仅受控本地假供应商（明确标示模拟，不是真实识别）。
 * by: Long <mycjzb@gmail.com>
 */
#ifndef MSMEDIA_MS_AI_SUB_H
#define MSMEDIA_MS_AI_SUB_H

#include "msmedia/msmedia.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MS_AI_VENDOR_NONE 0
#define MS_AI_VENDOR_FAKE_LOCAL 1 /* 仅模拟；不得写成真实 ASR */

#define MS_AI_RESULT_PARTIAL 1
#define MS_AI_RESULT_FINAL 2
#define MS_AI_RESULT_TRANSLATION 3

#ifndef MS_AI_LANG_MAX
#define MS_AI_LANG_MAX 16
#endif
#ifndef MS_AI_VENDOR_ID_MAX
#define MS_AI_VENDOR_ID_MAX 32
#endif
#ifndef MS_AI_STATUS_MAX
#define MS_AI_STATUS_MAX 96
#endif
#ifndef MS_AI_QUEUE_CHUNKS_DEFAULT
#define MS_AI_QUEUE_CHUNKS_DEFAULT 16u
#endif
#ifndef MS_AI_QUEUE_BYTES_DEFAULT
#define MS_AI_QUEUE_BYTES_DEFAULT (256u * 1024u)
#endif

typedef struct ms_ai_vendor_caps {
    size_t struct_size;
    const char *id;      /* 例如 "ms-ai-fake-local" */
    const char *display; /* 必须写明 simulated / not real ASR */
    int is_simulated;
    int can_transcribe;
    int can_translate;
    int needs_network;
    int needs_model;
    int vendor;          /* MS_AI_VENDOR_* */
} ms_ai_vendor_caps;

typedef struct ms_ai_pcm_chunk {
    size_t struct_size;
    const int16_t *samples; /* 交错 s16；所有权在调用方，submit 必须立刻拷贝 */
    size_t nframes;         /* 每声道样点数 */
    int channels;
    int sample_rate;
    int pcm_bits;           /* 仅支持 16 */
    int has_pts;
    int64_t pts_us;         /* 本块第一可听样点的媒体时间 */
    uint32_t session_epoch;
    uint32_t timeline_epoch;
    uint32_t selection_epoch;
} ms_ai_pcm_chunk;

typedef struct ms_ai_result {
    size_t struct_size;
    int kind; /* MS_AI_RESULT_* */
    uint32_t utterance_id;
    uint32_t revision;
    uint32_t session_epoch;
    uint32_t timeline_epoch;
    uint32_t selection_epoch;
    int has_start;
    int has_end;
    int64_t start_90k;
    int64_t end_90k;
    int64_t delay_90k;
    const char *text; /* UTF-8，至下一次 poll/destroy 前有效 */
    size_t text_size;
    char lang[MS_AI_LANG_MAX];
    int is_simulated;
} ms_ai_result;

typedef struct ms_ai_plugin_cfg {
    size_t struct_size;
    char source_lang[MS_AI_LANG_MAX];
    char target_lang[MS_AI_LANG_MAX]; /* 空=不请求翻译 */
    int want_partial;
    int want_translation;
    uint32_t timeout_ms;
} ms_ai_plugin_cfg;

typedef struct ms_ai_plugin_vt {
    size_t struct_size;
    void *ctx;
    ms_status (*query_caps)(void *ctx, ms_ai_vendor_caps *out);
    ms_status (*start)(void *ctx, const ms_ai_plugin_cfg *cfg);
    void (*cancel)(void *ctx, uint32_t session_epoch, uint32_t timeline_epoch,
                   uint32_t selection_epoch);
    void (*stop)(void *ctx);
    /* 必须尽快返回；不得在此做网络 I/O。PCM 由插件立即拷贝。 */
    ms_status (*submit_pcm)(void *ctx, const ms_ai_pcm_chunk *chunk);
    /* 无结果时返回 MS_AGAIN；out->text 至下次 poll/stop 前有效。 */
    ms_status (*poll_result)(void *ctx, ms_ai_result *out);
} ms_ai_plugin_vt;

typedef struct ms_ai_sub_opts {
    size_t struct_size;
    int vendor; /* 默认 0=未配置 */
    char source_lang[MS_AI_LANG_MAX];
    char target_lang[MS_AI_LANG_MAX];
    int want_partial;
    int want_translation;
    int target_rate;     /* 0=保持；常见 16000 */
    int target_channels; /* 0=保持；1=单声道 */
    uint32_t queue_chunks_max;
    size_t queue_bytes_max;
    uint32_t timeout_ms;
} ms_ai_sub_opts;

typedef struct ms_ai_sub_stats {
    size_t struct_size;
    int enabled;
    int configured;
    int is_simulated;
    int worker_running;
    uint64_t pcm_taps;
    uint64_t pcm_bytes;
    uint64_t pcm_frames;
    uint64_t queue_drops;
    uint64_t results_merged;
    uint64_t results_dropped_epoch;
    uint64_t results_dropped_timeout;
    uint64_t results_dropped_oversize;
    uint64_t plugin_errors;
    uint32_t session_epoch;
    uint32_t timeline_epoch;
    uint32_t selection_epoch;
    int last_tap_has_pts;
    int64_t last_tap_pts_us;
    int last_tap_rate;
    int last_tap_ch;
    size_t last_tap_nframes;
    int16_t last_tap_first; /* 仅供受控测试核对内容，不得写入日志 */
    int16_t last_tap_last;
    int vendor;
    char vendor_id[MS_AI_VENDOR_ID_MAX];
    char status[MS_AI_STATUS_MAX];
} ms_ai_sub_stats;

typedef struct ms_ai_sub ms_ai_sub;

/* 可验证的公共 PCM 辅助：不是解码器，只处理已解码 s16。 */
MSMEDIA_API ms_status ms_ai_pcm_s16_downmix_mono(const int16_t *in, int channels,
                                                 size_t nframes, int16_t *out,
                                                 size_t *out_nframes);
MSMEDIA_API ms_status ms_ai_pcm_s16_resample_linear(const int16_t *in, size_t nin,
                                                    int in_rate, int out_rate,
                                                    int16_t *out, size_t out_cap,
                                                    size_t *nout);

MSMEDIA_API void ms_ai_sub_opts_default(ms_ai_sub_opts *out);
MSMEDIA_API ms_status ms_ai_sub_create(const ms_allocator *alloc,
                                       const ms_ai_sub_opts *opts,
                                       ms_ai_sub **out);
MSMEDIA_API void ms_ai_sub_destroy(ms_ai_sub *s);

MSMEDIA_API ms_status ms_ai_sub_configure(ms_ai_sub *s, const ms_ai_sub_opts *opts);
/* 注册外部/测试插件。引擎取得 vt 拷贝；ctx 生命周期由调用方或 fake 包装负责。 */
MSMEDIA_API ms_status ms_ai_sub_set_plugin(ms_ai_sub *s, const ms_ai_plugin_vt *vt);
/* 内置受控假供应商。明确模拟，不是真实识别。 */
MSMEDIA_API ms_status ms_ai_sub_use_fake_local(ms_ai_sub *s);

MSMEDIA_API ms_status ms_ai_sub_query_caps(const ms_ai_sub *s, ms_ai_vendor_caps *out);
MSMEDIA_API ms_status ms_ai_sub_set_enabled(ms_ai_sub *s, int on);
MSMEDIA_API int ms_ai_sub_enabled(const ms_ai_sub *s);

MSMEDIA_API void ms_ai_sub_set_epochs(ms_ai_sub *s, uint32_t session_epoch,
                                      uint32_t timeline_epoch,
                                      uint32_t selection_epoch);
MSMEDIA_API void ms_ai_sub_bump_timeline(ms_ai_sub *s);
MSMEDIA_API void ms_ai_sub_bump_selection(ms_ai_sub *s);

/*
 * 非阻塞 tap：仅拷贝入有界队列。队列满时按实时策略丢弃最旧块。
 * 禁用/未配置时立即返回且不拷贝、不启动线程。
 */
MSMEDIA_API ms_status ms_ai_sub_tap_pcm(ms_ai_sub *s, const uint8_t *pcm, size_t bytes,
                                        int sample_rate, int channels, int pcm_bits,
                                        int has_pts, int64_t pts_us);

/* 在播放器驱动线程调用：收集插件结果并归并到生成轨。不得记录源文本。 */
MSMEDIA_API ms_status ms_ai_sub_drive(ms_ai_sub *s);

/* 取消在途、清空队列与生成轨 cue（seek/flush/切音轨后调用）。 */
MSMEDIA_API void ms_ai_sub_flush(ms_ai_sub *s);

MSMEDIA_API ms_sub_doc *ms_ai_sub_doc(ms_ai_sub *s);
MSMEDIA_API ms_status ms_ai_sub_get_stats(const ms_ai_sub *s, ms_ai_sub_stats *out);
/* 短状态，不含源音频/文本。 */
MSMEDIA_API const char *ms_ai_sub_status_text(const ms_ai_sub *s);

/* ---- 仅受控假供应商（模拟数据）---- */
typedef struct ms_ai_fake_opts {
    size_t struct_size;
    int emit_partial;
    int emit_final;
    int emit_translation;
    int reorder;          /* 先 final 后 partial，测试乱序归并 */
    int force_error;
    int force_oversize;
    int hold_polls;       /* 结果滞留 N 次 poll 再发出（超时/迟到） */
    int ignore_cancel;    /* 测试：取消后仍投递旧世代，验证引擎丢弃 */
    uint32_t sticky_id;   /* 非 0 则多块共用 utterance */
    char target_lang[MS_AI_LANG_MAX];
} ms_ai_fake_opts;

MSMEDIA_API void ms_ai_fake_opts_default(ms_ai_fake_opts *out);
MSMEDIA_API ms_status ms_ai_fake_vendor_init(const ms_allocator *alloc,
                                             const ms_ai_fake_opts *opts,
                                             ms_ai_plugin_vt *out);
MSMEDIA_API void ms_ai_fake_vendor_free(ms_ai_plugin_vt *vt);
MSMEDIA_API ms_status ms_ai_fake_set_opts(ms_ai_plugin_vt *vt, const ms_ai_fake_opts *opts);

#ifdef __cplusplus
}
#endif

#endif /* MSMEDIA_MS_AI_SUB_H */
