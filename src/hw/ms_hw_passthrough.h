/*
 * ms_hw_passthrough.h — AC-3/E-AC-3/DTS 音频直通（对齐 Media3 AudioCapabilities 思路）。
 * 纯 C、无 JNI，可宿主单测：路由决策、每帧采样数解析、IEC 61937 封装。
 * 真实 HDMI/光纤功放出声必须在设备上验收，宿主测试只证明数据格式与回退逻辑。
 * by: Long <mycjzb@gmail.com>
 */
#ifndef MS_HW_PASSTHROUGH_H
#define MS_HW_PASSTHROUGH_H

#include <stddef.h>
#include <stdint.h>

#include "msmedia/msmedia.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Java AudioFormat.ENCODING_* 常量（API 21+；IEC61937 为 API 24+）。 */
#define MS_PT_ENC_AC3 5
#define MS_PT_ENC_E_AC3 6
#define MS_PT_ENC_DTS 7
#define MS_PT_ENC_IEC61937 13

/* 能力位：由 Java 层探测后下发（与 MediaPlayer.java 保持一致）。 */
#define MS_PT_CAP_AC3 0x01u
#define MS_PT_CAP_E_AC3 0x02u
#define MS_PT_CAP_DTS 0x04u
#define MS_PT_CAP_IEC61937 0x08u
#define MS_PT_CAP_ENABLED 0x80000000u /* 应用层开关；无此位一律软解 */

typedef enum ms_pt_route {
    MS_PT_ROUTE_SOFT = 0, /* 不直通：系统/内置软解出 PCM */
    MS_PT_ROUTE_RAW = 1,  /* AudioTrack 直接用 ENCODING_AC3/E_AC3/DTS 写原始帧 */
    MS_PT_ROUTE_IEC = 2   /* AudioTrack ENCODING_IEC61937，写 IEC 61937 突发帧 */
} ms_pt_route;

/* 根据开关、编码、能力位与 API 级别决定路由。优先原始编码，其次 IEC61937。 */
ms_pt_route ms_pt_pick_route(ms_codec codec, unsigned caps, int api_level, int *out_encoding);

/* 一个 AU（可能含多个同步帧）所代表的 PCM 采样数；无法解析返回 0。 */
int ms_pt_frame_samples(ms_codec codec, const uint8_t *data, size_t n);

/*
 * IEC 61937 封装器（AC-3 每帧一个突发；E-AC-3 聚合 1536 采样、4 倍采样率 6144 帧一个突发；DTS 按块数选类型）。
 * 输出为 16 位小端立体声样本字节流，可直接写入 ENCODING_IEC61937 的 AudioTrack。
 */
typedef struct ms_pt_iec {
    ms_codec codec;
    uint8_t *acc;     /* E-AC-3 聚合缓冲 */
    size_t acc_n, acc_cap;
    int acc_samples;
    uint8_t *out;     /* 最近一次输出突发 */
    size_t out_cap;
} ms_pt_iec;

int ms_pt_iec_init(ms_pt_iec *w, ms_codec codec);
void ms_pt_iec_free(ms_pt_iec *w);
/* ms_pt_iec_rate_mul：IEC61937 AudioTrack 采样率相对流采样率的倍数（E-AC-3 为 4，其余 1）。 */
int ms_pt_iec_rate_mul(ms_codec codec);
/* 喂一个 AU。返回 1 表示 out/out_n 给出一个完整突发（含 PCM 帧数 frames）；
 * 0 表示仍在聚合；<0 表示帧非法或过大（调用方丢弃该 AU，不中断播放）。 */
int ms_pt_iec_feed(ms_pt_iec *w, const uint8_t *au, size_t n,
                   const uint8_t **out, size_t *out_n, int *frames);

#ifdef __cplusplus
}
#endif
#endif
