/*
 * 合成合法 / 畸形 MPEG-TS 样本，仅供测试使用。
 * by: Long <mycjzb@gmail.com>
 */

#ifndef MSMEDIA_TS_SYNTH_H
#define MSMEDIA_TS_SYNTH_H

#include <stddef.h>
#include <stdint.h>

#define MS_SYNTH_MAX 64

typedef struct ms_synth_stream {
    uint8_t stream_type;
    uint16_t pid;
    char language[4]; /* 可选，空表示无描述符 */
    const uint8_t *es_descriptor; /* 加法：可选 ES 描述符（不含 tag 前的长度包装已含 tag+len） */
    uint16_t es_descriptor_size;
} ms_synth_stream;

typedef struct ms_synth_buf {
    uint8_t *data;
    size_t len;
    size_t cap;
} ms_synth_buf;

int ms_synth_init(ms_synth_buf *b);
void ms_synth_free(ms_synth_buf *b);

/* 写入含 PAT + PMT + 填充空包的可同步 TS。packet_size 为 188 或 192。 */
int ms_synth_pat_pmt(ms_synth_buf *b,
                     uint16_t program_number,
                     uint16_t pmt_pid,
                     const ms_synth_stream *streams,
                     int nstreams,
                     int null_packets,
                     uint16_t packet_size);

/* 同上，指定 PAT/PMT version（0..31，current_next=1）。 */
int ms_synth_pat_pmt_version(ms_synth_buf *b,
                             uint16_t program_number,
                             uint16_t pmt_pid,
                             const ms_synth_stream *streams,
                             int nstreams,
                             int null_packets,
                             uint16_t packet_size,
                             unsigned version);

/* 加法：可带 PMT program_info 描述符（已含 tag+len，例如 0x05 0x04 'HDMV'）。 */
int ms_synth_pat_pmt_info(ms_synth_buf *b,
                          uint16_t program_number,
                          uint16_t pmt_pid,
                          const ms_synth_stream *streams,
                          int nstreams,
                          int null_packets,
                          uint16_t packet_size,
                          unsigned version,
                          const uint8_t *program_info,
                          uint16_t program_info_size);

/* 追加一条带可选 PTS 的 PES（payload 原样放入）。 */
int ms_synth_pes(ms_synth_buf *b,
                 uint16_t pid,
                 uint8_t stream_id,
                 int has_pts,
                 uint64_t pts,
                 const uint8_t *payload,
                 size_t payload_len,
                 uint16_t packet_size);

/* 同上，但 PES_packet_length 写 0（常见于视频 PES，需下一 PUSI 或 EOS 结束）。 */
int ms_synth_pes_unbounded(ms_synth_buf *b,
                           uint16_t pid,
                           uint8_t stream_id,
                           int has_pts,
                           uint64_t pts,
                           const uint8_t *payload,
                           size_t payload_len,
                           uint16_t packet_size);

/* 在已有字节前插入垃圾前缀。 */
int ms_synth_prepend(ms_synth_buf *b, const uint8_t *pfx, size_t pfx_len);

/* 破坏指定偏移的一个字节，用于畸形测试。 */
void ms_synth_xor(ms_synth_buf *b, size_t off, uint8_t mask);

/* 仅在新样本开始时调用；同一条流的 PAT/PMT/PES 之间不要复位。 */
void ms_synth_reset_cc(void);
void ms_synth_set_next_cc(uint16_t pid, unsigned cc);
unsigned ms_synth_peek_cc(uint16_t pid);

int ms_synth_append_188(ms_synth_buf *b, const uint8_t pkt[188], uint16_t packet_size);
int ms_synth_insert_unit(ms_synth_buf *b, size_t index, const uint8_t pkt[188],
                         uint16_t packet_size);
int ms_synth_delete_unit(ms_synth_buf *b, size_t index, uint16_t packet_size);
int ms_synth_adapt_only(ms_synth_buf *b, uint16_t pid, unsigned cc, int disc_ind,
                        uint16_t packet_size);
void ms_synth_pkt_set_cc(uint8_t *pkt, unsigned cc);
void ms_synth_pkt_set_tei(uint8_t *pkt, int tei);
void ms_synth_pkt_set_scrambling(uint8_t *pkt, unsigned scr);
int ms_synth_pkt_add_disc_indicator(uint8_t *pkt);

/* 合成长度字段合法的 ADTS 帧（载荷不保证可解码，只用于组帧测试）。 */
int ms_synth_adts_frame(uint8_t *out, size_t cap, size_t *out_len,
                        int profile, int sfi, int channels,
                        const uint8_t *raw, size_t raw_len);

#endif

/* 合成 Annex B NAL（自动插入 emulation prevention）。sc_len 为 3 或 4。 */
int ms_synth_annexb_nal(uint8_t *out, size_t cap, size_t *out_len,
                        int sc_len, uint8_t nal_hdr,
                        const uint8_t *rbsp, size_t rbsp_len);

/*
 * 合成 Baseline 逐行 SPS（或 frame_mbs_only=0 的隔行/PAFF 样本）。
 * width/height 为像素，须为 16 的倍数。载荷不保证可解码，只用于组帧/拒绝测试。
 */
int ms_synth_h264_sps(uint8_t *out, size_t cap, size_t *out_len,
                      int sc_len, int width, int height, int frame_mbs_only);

int ms_synth_h264_pps(uint8_t *out, size_t cap, size_t *out_len, int sc_len);
int ms_synth_h264_aud(uint8_t *out, size_t cap, size_t *out_len, int sc_len);
int ms_synth_h264_slice(uint8_t *out, size_t cap, size_t *out_len,
                        int sc_len, int idr, const uint8_t *raw, size_t raw_len);

/* 含 emulation prevention 字节的逐行 SPS，用于验证 RBSP 去转义。 */
int ms_synth_h264_sps_epb(uint8_t *out, size_t cap, size_t *out_len, int sc_len);

/* 指定 sps_id 与 POC 类型的 SPS。poc_type 为 0/1/2；类型 1 用 delta_always_zero。 */
int ms_synth_h264_sps_ex(uint8_t *out, size_t cap, size_t *out_len,
                         int sc_len, int width, int height, int frame_mbs_only,
                         unsigned sps_id, unsigned poc_type,
                         unsigned log2_max_frame_num_minus4,
                         unsigned log2_max_poc_lsb_minus4,
                         int delta_always_zero);

/* 指定 pps_id/sps_id；bottom_field 控制是否在条带中写 delta_pic_order_cnt_bottom。
 * slice_groups_minus1!=0 用于 FMO 拒绝样本。 */
int ms_synth_h264_pps_ex(uint8_t *out, size_t cap, size_t *out_len, int sc_len,
                         unsigned pps_id, unsigned sps_id,
                         int bottom_field, unsigned slice_groups_minus1);

/*
 * 写出可解析的条带头（用于无 AUD 主图像判定）。
 * nal_ref_idc：<0 则 IDR 用 3、非 IDR 用 2。
 * 按 poc_type 写入适用 POC 字段。载荷不保证可解码。
 */
typedef struct ms_synth_h264_slice_opts {
    int sc_len;
    int idr;
    int nal_ref_idc;
    unsigned first_mb;
    unsigned slice_type;
    unsigned pps_id;
    unsigned frame_num;
    unsigned log2_max_frame_num_minus4;
    unsigned poc_type;
    unsigned log2_max_poc_lsb_minus4;
    unsigned poc_lsb;
    int write_delta_bottom;
    int delta_bottom;
    int write_delta0;
    int delta0;
    int write_delta1;
    int delta1;
    unsigned idr_pic_id;
} ms_synth_h264_slice_opts;

int ms_synth_h264_slice_hdr(uint8_t *out, size_t cap, size_t *out_len,
                            const ms_synth_h264_slice_opts *opt);
