/*
 * 极简 FLV 写出器（内部）：RTSP/RTMP/MKV/PS 等协议/容器的 H.264/HEVC/AAC 统一转成 FLV 字节流，
 * 交给核心已有的 FLV 解封装（复用，不另写一套 AU 事件发射）。HEVC 用国内扩展 CodecID 12。
 * by: Long <mycjzb@gmail.com>
 */
#ifndef MS_FLVW_H
#define MS_FLVW_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef int (*ms_flvw_write_fn)(void *ctx, const uint8_t *d, size_t n);

typedef struct ms_flvw {
    ms_flvw_write_fn write;
    void *ctx;
    int hdr_done;
} ms_flvw;

int ms_flvw_header(ms_flvw *w, int has_audio, int has_video);
/* 通用标签：body = head(nh) + payload(np)，两段写出免拷贝。 */
int ms_flvw_tag(ms_flvw *w, int type, uint32_t ts_ms, const uint8_t *head, size_t nh,
                const uint8_t *payload, size_t np);
int ms_flvw_video_config(ms_flvw *w, int hevc, uint32_t ts_ms, const uint8_t *rec, size_t n);
int ms_flvw_video_frame(ms_flvw *w, int hevc, int key, uint32_t ts_ms, int32_t cts_ms,
                        const uint8_t *lp_nals, size_t n);
int ms_flvw_aac_config(ms_flvw *w, uint32_t ts_ms, const uint8_t *asc, size_t n);
int ms_flvw_aac_frame(ms_flvw *w, uint32_t ts_ms, const uint8_t *raw, size_t n);

/* 由 SPS/PPS 生成 avcC；返回长度，0=失败。 */
size_t ms_flvw_make_avcc(const uint8_t *sps, size_t sn, const uint8_t *pps, size_t pn, uint8_t *out, size_t cap);
/* 由 VPS/SPS/PPS 生成 hvcC（4 字节长度前缀）；返回长度，0=失败。 */
size_t ms_flvw_make_hvcc(const uint8_t *vps, size_t vn, const uint8_t *sps, size_t sn,
                         const uint8_t *pps, size_t pn, uint8_t *out, size_t cap);
/* Annex-B（起始码）→ 4 字节长度前缀；返回输出长度，0=没有 NAL 或空间不足。 */
size_t ms_flvw_annexb_to_lp(const uint8_t *d, size_t n, uint8_t *out, size_t cap);
/* 遍历 Annex-B NAL：返回下一个 NAL（不含起始码），*pos 前移；无则 NULL。 */
const uint8_t *ms_flvw_next_nal(const uint8_t *d, size_t n, size_t *pos, size_t *nal_len);
#ifdef __cplusplus
}
#endif
#endif
