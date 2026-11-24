/*
 * ISO BMFF / QuickTime 共用有界箱体读取（MP4 / fMP4 / MOV 的共同基础）。
 * 解析箱头、遍历已知容器、安全跳过未知箱。
 * P2b：渐进文件在 moov 先于 mdat 时解析 sample 表并按文件偏移收获
 * H.264/AAC 样本；大 mdat 仍不整文件缓存。fMP4 仍无 AU。
 * P2c：moov 在 mdat 之后时，若声明的 mdat 载荷不超过有界回看上限且
 * 预算允许，则保留该窗并在表解析后按与 P2b 相同合同输出 AU；否则
 * 不整文件缓存，并在需要回看时给出明确错误。
 * P2d：fMP4 在 moov+mvex 之后解析 moof/trun，按与渐进路径相同合同输出
 * H.264/AAC AU；加密、缺 init、越界偏移明确报错，不吐假帧。
 * 识别只看字节 0 的合法箱头，不在缓冲里扫描四字节标签。
 * by: Long <mycjzb@gmail.com>
 */

#ifndef MSMEDIA_BMFF_H
#define MSMEDIA_BMFF_H

#include "ms_internal.h"

#define MS_BMFF_MAX_DEPTH  16
#define MS_BMFF_MAX_BRANDS 32
#define MS_BMFF_MAX_BOXES  4096
#define MS_BMFF_TYPE(a, b, c, d) \
    (((uint32_t)(uint8_t)(a) << 24) | ((uint32_t)(uint8_t)(b) << 16) | \
     ((uint32_t)(uint8_t)(c) << 8) | (uint32_t)(uint8_t)(d))

typedef struct ms_bmff_box {
    uint32_t type;
    uint32_t parent_type; /* 顶层为 0 */
    uint8_t usertype[16];
    int has_uuid;
    uint64_t offset;      /* 箱起始（流内绝对偏移） */
    uint64_t size;        /* 含头；0 表示直到 EOF（ISO size=0） */
    uint32_t header_size;
    int depth;            /* 顶层为 0 */
    int is_container;
} ms_bmff_box;

typedef struct ms_bmff_callbacks {
    void *ctx;
    /* 箱头解析完成时调用（尚未读/跳过载荷）。返回非 OK 则中止本轮。 */
    ms_status (*on_box)(void *ctx, const ms_bmff_box *box);
    /* 已能分类为 MP4 / fMP4 / MOV 时调用一次。 */
    ms_status (*on_format)(void *ctx, ms_format format,
                           uint32_t major_brand, uint32_t minor_version);
    /* 渐进路径：在 FORMAT 之后发出可用轨道与配置，再按文件偏移发出 AU。 */
    ms_status (*on_track)(void *ctx, const ms_track_info *track, int update);
    ms_status (*on_codec_config)(void *ctx, const ms_track_info *track,
                                 const uint8_t *data, size_t size,
                                 ms_sample_format sample_fmt,
                                 int time_base_num, int time_base_den);
    ms_status (*on_au)(void *ctx, const ms_track_info *track,
                       const ms_au_frame *fr);
} ms_bmff_callbacks;

typedef struct ms_bmff_parser ms_bmff_parser;

/* 探测：1=字节 0 已是合法顶层箱头；0=不是本家族；-1=还要更多字节。 */
int ms_bmff_probe(const uint8_t *data, size_t size);

int ms_bmff_is_container_type(uint32_t type);
/* 后续 sample 表 / moof-trun 可复用，本步只识别类型，不解析内容。 */
int ms_bmff_is_sample_table_type(uint32_t type);
int ms_bmff_is_fragment_type(uint32_t type);

void ms_bmff_parser_init(ms_bmff_parser *p,
                         const ms_allocator *alloc,
                         const ms_bmff_callbacks *cb);
void ms_bmff_parser_set_limits(ms_bmff_parser *p, size_t max_assembly_bytes,
                               size_t max_lookback_bytes);
void ms_bmff_parser_reset(ms_bmff_parser *p);
void ms_bmff_parser_destroy(ms_bmff_parser *p);
void ms_bmff_parser_set_meter(ms_bmff_parser *p, ms_copy_meter *m);
ms_status ms_bmff_parser_drain(ms_bmff_parser *p);
int ms_bmff_parser_need_seek(const ms_bmff_parser *p);
int ms_bmff_parser_need_unsup(const ms_bmff_parser *p);
void ms_bmff_flush_partial(ms_bmff_parser *p);
void ms_bmff_parser_set_random(ms_bmff_parser *p, int on);

enum {
    MS_BMFF_NEED_NONE = 0,
    MS_BMFF_NEED_SEQ = 1,
    MS_BMFF_NEED_JUMP = 2,
    MS_BMFF_NEED_SAMPLE = 3
};

typedef struct ms_bmff_need {
    int kind;
    uint64_t offset;
    size_t length;
} ms_bmff_need;

/* have_size=0 时不建议 JUMP。seq_hint 为顺序块建议长度。 */
ms_status ms_bmff_parser_need(const ms_bmff_parser *p, int have_size,
                              uint64_t src_size, size_t seq_hint,
                              ms_bmff_need *out);
ms_status ms_bmff_parser_skip_to(ms_bmff_parser *p, uint64_t off);
uint64_t ms_bmff_parser_file_off(const ms_bmff_parser *p);
ms_status ms_bmff_supply_sample(ms_bmff_parser *p, uint64_t off,
                                const uint8_t *data, size_t n, size_t *used);
int ms_bmff_parser_time_seekable(const ms_bmff_parser *p);
ms_status ms_bmff_parser_seek_time(ms_bmff_parser *p, int64_t time_90k,
                                   int64_t *land_90k);

ms_status ms_bmff_parser_push(ms_bmff_parser *p,
                              const uint8_t *data,
                              size_t size,
                              size_t *consumed,
                              int eos);

ms_format ms_bmff_parser_format(const ms_bmff_parser *p);
uint32_t ms_bmff_parser_major_brand(const ms_bmff_parser *p);
int ms_bmff_parser_brands(const ms_bmff_parser *p,
                          uint32_t *out, int cap);
int ms_bmff_parser_box_count(const ms_bmff_parser *p);
int ms_bmff_parser_max_depth(const ms_bmff_parser *p);
uint64_t ms_bmff_parser_bytes_skipped(const ms_bmff_parser *p);
int ms_bmff_parser_saw_moov(const ms_bmff_parser *p);
int ms_bmff_parser_saw_mdat(const ms_bmff_parser *p);
int ms_bmff_parser_saw_moof(const ms_bmff_parser *p);
int ms_bmff_parser_saw_mvex(const ms_bmff_parser *p);
int ms_bmff_parser_truncated_mdat(const ms_bmff_parser *p);
int ms_bmff_parser_confirmed(const ms_bmff_parser *p);

/* 供调试打印。out 至少 5 字节。 */
void ms_bmff_type_str(uint32_t type, char out[5]);

/*
 * 解析器本体。嵌入或懒分配均可；字段不是稳定 ABI。
 * 固定嵌入时不计入会话动态预算；懒分配则计入。
 */
struct ms_bmff_parser {
    const ms_allocator *alloc;
    ms_bmff_callbacks cb;
    int confirmed;
    int format_emitted;
    int failed;
    int finished;
    ms_status fail_status;
    ms_format format;
    uint32_t major_brand;
    uint32_t minor_version;
    uint32_t brands[MS_BMFF_MAX_BRANDS];
    int nbrands;
    int saw_ftyp;
    int saw_styp;
    int saw_moov;
    int saw_mdat;
    int saw_moof;
    int saw_mvex;
    int still_image;
    int truncated_mdat;
    uint64_t file_off;
    uint64_t bytes_skipped;
    int nboxes;
    int max_depth_seen;
    int sp;
    struct {
        uint32_t type;
        uint64_t end; /* UINT64_MAX=直到 EOF */
    } stack[MS_BMFF_MAX_DEPTH];
    int state;
    uint8_t hdr[32];
    size_t hdr_need;
    size_t hdr_got;
    uint64_t skip_left;
    int skip_to_eof;
    int skip_is_mdat;
    /* ftyp/styp 体按 4/8 字节流式读，不缓存整箱。 */
    uint64_t body_left;
    int body_kind; /* 0 无，1 ftyp/styp */
    int body_is_styp;
    size_t brand_phase; /* 已读体字节 */
    uint8_t brand_tmp[8];
    size_t brand_tmp_got;
    /* P2b：有界捕获 sample 表箱体，并在 mdat 中按偏移收获当前样本。 */
    size_t max_assembly_bytes;
    size_t max_lookback_bytes;
    /* 有界回看窗：覆盖 moov 之前已见且尺寸合规的 mdat 载荷。 */
    uint8_t *lb_buf;
    size_t lb_len;
    size_t lb_cap;
    uint64_t lb_start;
    int lb_active;
    int lb_overflow;
    struct ms_bmff_demux *dmx;
    uint32_t cap_type;
    uint64_t cap_left;
    uint8_t *cap_buf;
    size_t cap_len;
    size_t cap_cap;
    ms_copy_meter *meter;
    int random_ok; /* 已挂可随机读 Source：跳过尾 mdat、按偏移取样本 */
    int64_t seek_req_90k;
    int64_t seek_land_90k;
    int have_seek_land;
};

#endif /* MSMEDIA_BMFF_H */
