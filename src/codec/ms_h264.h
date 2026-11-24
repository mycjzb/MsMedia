/*
 * MPEG-TS / PES 载荷上的 H.264 Annex B 增量组帧。
 * by: Long <mycjzb@gmail.com>
 *
 * 本构建支持的子集（不是全部 H.264）：
 *   - Annex B，3 或 4 字节起始码
 *   - 访问单元边界：AUD（nal_unit_type=9），或无 AUD 时按
 *     ITU-T H.264 7.4.1.2.4 比较主编码图像相关字段
 *     （frame_num、pic_parameter_set_id、nal_ref_idc 是否为 0、
 *     IdrPicFlag / idr_pic_id，以及适用的 POC 字段）。
 *     不把 first_mb_in_slice=0 当作通用边界。
 *   - 同一主图像的多 slice 合成一个 AU；不同主图像不得并成一帧
 *   - 逐行帧：SPS.frame_mbs_only_flag=1
 *   - POC 类型 0、1、2（类型 1 含 delta_pic_order_always_zero_flag
 *     为 1 或为 0 并携带 delta_pic_order_cnt）
 *   - 参数集按 ID 管理（SPS 0..31，同时保留的 PPS 上限见实现）
 *   - 由已解析的 SPS/PPS 发出 codec config（Annex B：4 字节起始码
 *     + SPS NAL + 4 字节起始码 + PPS NAL）
 *   - IDR（type=5）标记 is_keyframe=1；非 IDR 为 0。
 *     不保证非 IDR 或「带参数集的 I 帧」可以安全恢复/起播
 *   - 时间戳：PES PTS/DTS 只属于在该 PES 内开始的第一帧；
 *     同一 PES 内后续 AU 若没有新的时间戳，has_pts/has_dts 为 0，
 *     不按不存在的帧率杜撰
 *
 * 明确拒绝（不伪报为可组帧）：
 *   - field / PAFF / MBAFF（frame_mbs_only_flag=0）
 *   - FMO（num_slice_groups_minus1!=0）
 *   - separate_colour_plane_flag=1
 *   - 数据分割条带（type 2/3/4）及 MVC/SVC 等扩展 NAL（type>=14）
 *   - 把未完整的 NAL 或没有主图像的数据当成 AU
 * 中途接入：尚无 SPS/PPS，或尚未发出过 IDR 的非 IDR 图像，丢弃这些
 * 单元并继续等待，不把残片当 AU 发出，也不把会话标为失败。
 * 条带引用尚未收到的参数集同样等待，不伪报为可组帧，也不当成畸形。
 * 这里不做软解，也不重组参考帧。
 */

#ifndef MSMEDIA_H264_H
#define MSMEDIA_H264_H

#include "ms_internal.h"

#define MS_H264_SPS_ID_MAX 31
#define MS_H264_PPS_ID_MAX 255
#define MS_H264_PPS_SLOTS  32

typedef struct ms_h264_callbacks {
    void *ctx;
    ms_status (*on_config)(void *ctx, const uint8_t *data, size_t size);
    ms_status (*on_au)(void *ctx, const ms_au_frame *fr);
} ms_h264_callbacks;

typedef struct ms_h264_sps {
    int present;
    unsigned id;
    unsigned log2_max_frame_num_minus4;
    unsigned poc_type;
    unsigned log2_max_poc_lsb_minus4;
    int delta_poc_always_zero;
    int frame_mbs_only;
    int width;  /* 显示宽，含裁剪；未能读裁剪时为宏块对齐编码宽 */
    int height;
    uint8_t *nal;
    size_t nal_len;
    size_t nal_cap;
} ms_h264_sps;

typedef struct ms_h264_pps {
    int present;
    unsigned id;
    unsigned sps_id;
    int bottom_field_pic_order_in_frame;
    unsigned num_slice_groups_minus1;
    int fmo;
    uint8_t *nal;
    size_t nal_len;
    size_t nal_cap;
} ms_h264_pps;

typedef struct ms_h264_slice {
    unsigned first_mb; /* 解析但不用于边界判定 */
    unsigned slice_type;
    unsigned pps_id;
    unsigned sps_id;
    unsigned frame_num;
    unsigned nal_ref_idc;
    int idr;
    unsigned idr_pic_id;
    unsigned poc_type;
    unsigned poc_lsb;
    int has_delta_bottom;
    int delta_bottom;
    int has_delta0;
    int delta0;
    int has_delta1;
    int delta1;
} ms_h264_slice;

typedef struct ms_h264 {
    const ms_allocator *alloc;
    size_t max_au;
    ms_h264_callbacks cb;
    uint8_t *buf;
    size_t len;
    size_t cap;
    size_t parsed; /* 已处理完的 NAL 结束于下一个起始码处 */
    int have_aud;
    int have_vcl;
    int have_idr;
    int au_open;
    int have_start_ts;
    int start_has_pts;
    int start_has_dts;
    uint64_t start_pts;
    uint64_t start_dts;
    int pes_ts_available;
    int pes_has_pts;
    int pes_has_dts;
    uint64_t pes_pts;
    uint64_t pes_dts;
    int have_sps;
    int have_pps;
    int config_dirty;
    unsigned last_sps_id;
    unsigned last_pps_id;
    unsigned last_cfg_sps_id;
    unsigned last_cfg_pps_id;
    int have_last_cfg;
    int au_have_ids;
    unsigned au_sps_id;
    unsigned au_pps_id;
    ms_h264_sps sps[MS_H264_SPS_ID_MAX + 1];
    ms_h264_pps pps[MS_H264_PPS_SLOTS];
    int have_prev_slice;
    ms_h264_slice prev_slice;
    int have_prefix;   /* 上一图像最后 VCL 之后的 SPS/PPS/SEI/AUD 起点 */
    size_t prefix_off;
    int started; /* 已成功发出过 IDR，此后才发出非 IDR */
    int failed;
    ms_status fail_status;
    ms_copy_meter *meter;
} ms_h264;

void ms_h264_set_meter(ms_h264 *h, ms_copy_meter *m);

void ms_h264_init(ms_h264 *h,
                  const ms_allocator *alloc,
                  size_t max_au,
                  const ms_h264_callbacks *cb);
void ms_h264_destroy(ms_h264 *h);
void ms_h264_reset(ms_h264 *h);
void ms_h264_flush_partial(ms_h264 *h); /* 丢掉未完成 AU，保留已解析的 SPS/PPS */
/* 容错模式（仅单 so 网络输入启用）：清除失败态并丢掉未完成 AU，保留 SPS/PPS，从下一个起始码继续。 */
void ms_h264_recover(ms_h264 *h);

void ms_h264_begin_pes(ms_h264 *h, int has_pts, uint64_t pts, int has_dts, uint64_t dts);

ms_status ms_h264_push(ms_h264 *h, const uint8_t *data, size_t size, size_t *consumed);
ms_status ms_h264_drain(ms_h264 *h);
ms_status ms_h264_end(ms_h264 *h);

#endif
