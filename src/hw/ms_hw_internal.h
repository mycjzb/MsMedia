/*
 * libmshw 内部。不安装、不导出。
 * by: Long <mycjzb@gmail.com>
 */
#ifndef MSMEDIA_HW_INTERNAL_H
#define MSMEDIA_HW_INTERNAL_H

#include "msmedia/ms_hw.h"

#define MS_HW_DEFAULT_PENDING 8u
#define MS_HW_DEFAULT_IN_BYTES (1024u * 1024u)
#define MS_HW_DEFAULT_EVENTS 8u
#define MS_HW_ABS_PENDING 32u
#define MS_HW_ABS_IN_BYTES (8u * 1024u * 1024u)
#define MS_HW_ABS_EVENTS 32u
#define MS_HW_CSD_CAP (8u * 1024u)

typedef enum ms_hw_state {
    MS_HW_ST_IDLE = 0,
    MS_HW_ST_CONFIGURED,
    MS_HW_ST_STARTED,
    MS_HW_ST_DRAINING,
    MS_HW_ST_EOS,
    MS_HW_ST_ERROR,
    MS_HW_ST_DEAD
} ms_hw_state;

/*
 * 出帧时机回调（对齐 Media3 VideoFrameReleaseControl 的 FRAME_RELEASE_*）。
 * 只有 Android Surface 路径（NDK / Java MediaCodec）会调用；mock/VAAPI 不用。
 * 回调在 decoder drive 线程里执行，不得再调用本解码器的 drive/flush。
 * by: Long <mycjzb@gmail.com>
 */
typedef enum ms_hw_release_action {
    MS_HW_REL_NOW = 0,   /* 立即 render（首帧、暂停重绑、无音频、强制释放） */
    MS_HW_REL_AT = 1,    /* releaseOutputBuffer(index, releaseTimeNs)，仅 API 21+ */
    MS_HW_REL_DROP = 2,  /* 迟到：releaseOutputBuffer(index, false)，不上屏 */
    MS_HW_REL_LATER = 3  /* 太早：保留该输出缓冲，下轮 drive 再问 */
} ms_hw_release_action;
typedef ms_hw_release_action (*ms_hw_release_fn)(void *ctx, int64_t pts_us, int is_eos,
                                                 int can_schedule, int64_t *release_ns);

typedef struct ms_hw_slot {
    uint8_t *data;
    size_t size;
    void (*release_fn)(void *ctx, uint8_t *data, size_t size);
    void *release_ctx;
    int64_t pts;
    unsigned flags;
    int occupied;
} ms_hw_slot;

struct ms_hw_decoder {
    const ms_allocator *alloc;
    ms_allocator owned_alloc;
    ms_hw_limits limits;
    ms_hw_create_opts opts;
    ms_hw_state st;
    ms_hw_caps caps;
    char name_buf[128];
    char preferred_name[128]; /* 测试覆盖；空=默认挑选 */
    char video_mime[64];      /* AVS/AVS+ 等无标准 MIME 时：所选组件实际声明的 MIME；空=按 codec 标准 MIME */
    char detail_buf[192];
    char ev_detail_buf[192];
    void *surface;
    ms_sample_format sample_fmt;
    int width;
    int height;
    int64_t tb_num;
    int64_t tb_den;
    uint8_t csd0[MS_HW_CSD_CAP];
    size_t csd0_size;
    uint8_t csd1[MS_HW_CSD_CAP];
    size_t csd1_size;
    ms_hw_slot *in_slots;
    unsigned in_cap;
    unsigned in_n;
    size_t in_bytes;
    ms_hw_event *evq;
    unsigned ev_cap;
    unsigned ev_n;
    unsigned ev_r;
    ms_hw_stats stats;
    ms_hw_log_fn log_fn;
    void *log_ctx;
    int eos_sent;
    /* 出帧时机回调（NULL=旧行为：一出来就 render）。can_schedule 由平台层告知回调。 */
    ms_hw_release_fn release_fn;
    void *release_fn_ctx;
#ifdef __ANDROID__
    void *plat;
#endif
};

/* 音频钟来源与输出延迟（见 ms_hw_player_stats.audio_clock_source）。宿主恒为 0。 */
void ms_hw_audio_sync_info(const ms_hw_audio *a, int *src, int64_t *latency_us);

#ifdef __ANDROID__
/* VSYNC 采样（参照 Media3 VideoFrameReleaseHelper.VSyncSampler，精简版）：
 * 进程级一个 ALooper 线程 + AChoreographer（API 24+；32 位进程需 API 29 的 64 位回调）。
 * acquire/release 引用计数；不可用时 get 返回 0，调用方按未对齐处理。 */
void ms_hw_vsync_acquire(void);
void ms_hw_vsync_release(void);
int ms_hw_vsync_get(int64_t *vsync_ns, int64_t *period_ns);
#endif

void *ms_hw_alloc(ms_hw_decoder *d, size_t n);
void ms_hw_free(ms_hw_decoder *d, void *p);
void ms_hw_log(ms_hw_decoder *d, int level, const char *msg);
void ms_hw_set_detail(ms_hw_decoder *d, const char *s);
void ms_hw_set_detail_buf(char *buf, size_t cap, const char *s);
void ms_hw_consume_slot(ms_hw_decoder *d, unsigned i);
void ms_hw_caps_clear(ms_hw_caps *c);
void ms_hw_release_slot(ms_hw_slot *s);
void ms_hw_drop_all_input(ms_hw_decoder *d);
void ms_hw_drop_events(ms_hw_decoder *d);
ms_status ms_hw_push_event(ms_hw_decoder *d, const ms_hw_event *ev);
void ms_hw_note_pin(ms_hw_decoder *d);
int ms_hw_ascii_ieq(const char *a, const char *b);
int ms_hw_ascii_ieq_prefix(const char *s, const char *pfx);
int ms_hw_ascii_icontains(const char *s, const char *needle);
const char *ms_hw_video_mime(ms_codec codec);

/* AVS/AVS+ 解码器挑选（纯 C，宿主可测）：AOSP 无标准 AVS MIME，
 * 由 JNI 遍历 MediaCodecList 填入设备真实注册的解码器，再按此策略挑选。 */
#define MS_HW_AVS_MAX_TYPES 16
typedef struct ms_hw_codec_desc {
    const char *name;
    const char *types[MS_HW_AVS_MAX_TYPES];
    unsigned ntypes;
    int is_encoder;
    int is_alias;
    int hw_flag;  /* API 29+ isHardwareAccelerated；-1=未知 */
    int sw_flag;  /* API 29+ isSoftwareOnly；-1=未知 */
} ms_hw_codec_desc;
/* MIME 偏好：返回 1..N（越小越优先），0=不是 AVS/AVS+ MIME（AVS2/AVS3 一律 0）。 */
int ms_hw_avs_mime_rank(const char *mime);
/* 成功时 name/mime 为所选组件名与其实际声明的 MIME。无硬件时 UNSUPPORTED（allow_sw 才放行软件）。 */
ms_status ms_hw_avs_pick_from_list(const ms_hw_codec_desc *list, size_t n, int api_level,
                                   int allow_sw, char *name, size_t name_cap,
                                   char *mime, size_t mime_cap, ms_hw_kind *kind,
                                   char *detail, size_t detail_cap);
const char *ms_hw_video_label(ms_codec codec);
const char *ms_hw_audio_mime(ms_codec codec);
const char *ms_hw_audio_label(ms_codec codec);

ms_status ms_hw_csd_split(ms_sample_format fmt, const uint8_t *csd, size_t n,
                          uint8_t *csd0, size_t *n0, size_t cap0,
                          uint8_t *csd1, size_t *n1, size_t cap1);
ms_status ms_hw_csd_split_for_codec(ms_codec codec, ms_sample_format fmt,
                                    const uint8_t *csd, size_t n,
                                    uint8_t *csd0, size_t *n0, size_t cap0,
                                    uint8_t *csd1, size_t *n1, size_t cap1);

/* 裸 NAL 前缀 00 00 00 01；已是起始码则原样拷贝。成功返回 0。 */
int ms_hw_nal_with_start_code(const uint8_t *nal, size_t n,
                              uint8_t *dst, size_t dst_cap, size_t *out_n);
/* 长度前缀 AU 写成 00 00 00 01 + NAL；已是起始码则原样。成功返回 0。 */
int ms_hw_avcc_au_to_annexb(const uint8_t *src, size_t src_n, unsigned ls,
                            uint8_t *dst, size_t dst_cap, size_t *out_n);

ms_status ms_hw_mock_configure(ms_hw_decoder *d, const ms_hw_config *cfg);
ms_status ms_hw_vaapi_query(ms_codec codec, ms_hw_caps *out);
ms_status ms_hw_vaapi_configure(ms_hw_decoder *d, const ms_hw_config *cfg);
ms_status ms_hw_mock_start(ms_hw_decoder *d);
ms_status ms_hw_mock_drive(ms_hw_decoder *d);
ms_status ms_hw_mock_flush(ms_hw_decoder *d);

#ifdef __ANDROID__
ms_status ms_hw_android_query(ms_codec codec, int api_level, int allow_sw,
                              ms_hw_caps *out, char *name_buf, size_t name_cap,
                              char *detail_buf, size_t detail_cap);
ms_status ms_hw_android_configure(ms_hw_decoder *d, const ms_hw_config *cfg);
ms_status ms_hw_android_start(ms_hw_decoder *d);
ms_status ms_hw_android_submit(ms_hw_decoder *d, unsigned slot_index);
ms_status ms_hw_android_drive(ms_hw_decoder *d);
ms_status ms_hw_android_flush(ms_hw_decoder *d);
void ms_hw_android_close(ms_hw_decoder *d);
#endif

/* 阶段 4 音频直通（内部，不导出）：caps 为 MS_PT_CAP_* 位，见 ms_hw_passthrough.h。
 * by: Long <mycjzb@gmail.com> */
void ms_hw_audio_set_passthrough_caps(ms_hw_audio *a, unsigned caps);
int ms_hw_audio_passthrough_route(const ms_hw_audio *a, unsigned *fallbacks);
void ms_hw_player_set_passthrough_caps(ms_hw_player *p, unsigned caps);
int ms_hw_player_passthrough_route(ms_hw_player *p, unsigned *fallbacks);

#endif

#ifdef __ANDROID__
ms_status ms_hw_android_copy_last_output(ms_hw_decoder *d,
                                         int *width, int *height,
                                         int *stride, int *slice_height,
                                         int *color_format,
                                         uint8_t *dst, size_t dst_cap,
                                         size_t *out_size);
#endif
