/*
 * 核心内部声明。不对外安装，不是稳定 ABI。
 * by: Long <mycjzb@gmail.com>
 */

#ifndef MSMEDIA_INTERNAL_H
#define MSMEDIA_INTERNAL_H

#include "msmedia/msmedia.h"

#include <stdlib.h>
#include <string.h>

#define MS_DEFAULT_PROBE_BYTES      ((size_t)1024 * 1024)
#define MS_DEFAULT_AU_QUEUE_BYTES   ((size_t)2 * 1024 * 1024)
#define MS_DEFAULT_OUT_SLOTS        ((size_t)8)
#define MS_DEFAULT_MAX_PUSH_BYTES   ((size_t)1024 * 1024)
#define MS_DEFAULT_MAX_RESYNC_BYTES ((size_t)256 * 1024)
#define MS_DEFAULT_MAX_ASSEMBLY_BYTES ((size_t)2 * 1024 * 1024)
#define MS_DEFAULT_SESSION_BUDGET_BYTES ((size_t)16 * 1024 * 1024)

#define MS_ABS_PROBE_BYTES          ((size_t)8 * 1024 * 1024)
#define MS_ABS_AU_QUEUE_BYTES       ((size_t)16 * 1024 * 1024)
#define MS_ABS_OUT_SLOTS            ((size_t)64)
#define MS_ABS_MAX_PUSH_BYTES       ((size_t)8 * 1024 * 1024)
#define MS_ABS_MAX_RESYNC_BYTES     ((size_t)1 * 1024 * 1024)
#define MS_ABS_MAX_ASSEMBLY_BYTES   ((size_t)16 * 1024 * 1024)
#define MS_ABS_SESSION_BUDGET_BYTES ((size_t)64 * 1024 * 1024)

#define MS_MEM_ACCT_MAGIC           0x4D534131u /* "MSA1" */
#define MS_MEM_HDR_MAGIC            0x4D534831u /* "MSH1" */

#define MS_PENDING_EVENT_MAX        64
#define MS_TS_MAX_TRACKS            32
#define MS_PSI_MAX_SECTION          1024
#define MS_PES_HEADER_MAX           256

/* 未确认包长度的 PES 增长上限：不超过 max_pes_bytes（默认与默认 AU 队列相同），且至少能放下包头。 */
#define MS_TS_SYNC_NEEDED           3

#if defined(__GNUC__)
#define MS_UNUSED __attribute__((unused))
#else
#define MS_UNUSED
#endif

/*
 * 内部访问单元视图。AAC 与 H.264 共用，不是把 PES 或单个 NAL 直接当帧。
 * config 不在这里；codec config 走独立回调。
 */
typedef struct ms_au_frame {
    const uint8_t *data;
    size_t size;
    int has_pts;
    int has_dts;
    uint64_t pts;
    uint64_t dts;
    int format_changed;
    int is_keyframe;
    int discontinuity; /* 该轨此前发生过传输不连续，本 AU 为恢复后第一帧时为 1 */
} ms_au_frame;

int ms_size_add(size_t a, size_t b, size_t *out);
int ms_size_mul(size_t a, size_t b, size_t *out);

void *ms_mem_alloc(const ms_allocator *a, size_t size);
void ms_mem_free(const ms_allocator *a, void *p);

/*
 * 会话动态预算记账。used 含每块的头+载荷。
 * 不涵盖固定会话对象（ms_session 本体、嵌入的解析器/轨道/PSI/事件节点、
 * 以及 create 时分配的输出槽表）。
 */
typedef struct ms_mem_acct {
    uint32_t magic;
    ms_allocator user;
    size_t used;
    size_t peak;
    size_t budget;
    ms_status last_status;
    int last_was_budget; /* 1：LIMIT 来自总预算；0：来自硬上限或其它 */
} ms_mem_acct;

void ms_acct_init(ms_mem_acct *m, const ms_allocator *user, size_t budget);
void ms_acct_bind(ms_mem_acct *m, ms_allocator *iface);
int ms_acct_is(const ms_allocator *a);
int ms_acct_is_budget_limit(const ms_allocator *a);

/* 将 *buf 扩到至少 need 字节，容量不超过 hard_cap。need==0 时不分配。
 * need>hard_cap 返回 LIMIT（last_was_budget=0）；预算不足返回 LIMIT
 * （last_was_budget=1），旧缓冲不变。 */
ms_status ms_buf_reserve(const ms_allocator *a,
                         uint8_t **buf,
                         size_t *cap,
                         size_t need,
                         size_t hard_cap);

void ms_default_allocator(ms_allocator *out);

typedef struct ms_pes_view {
    uint16_t pid;
    ms_codec codec;
    int has_pts;
    int has_dts;
    uint64_t pts; /* 33-bit，90 kHz */
    uint64_t dts;
    const uint8_t *payload;
    size_t payload_size;
} ms_pes_view;

/*
 * 内部调试接口：取出一个已组装完整的 PES（不是 AU）。
 * 不进入公共头文件，调用方不得把它当作 H.264/AAC 组帧已完成。
 */
ms_status ms_session_debug_pull_pes(ms_session *session, ms_pes_view *out);

/* 动态预算的已用/峰值/上限（实测记账，不含固定会话对象）。 */
void ms_session_debug_budget(const ms_session *session,
                             size_t *used, size_t *peak, size_t *budget);
/* 输出队列中尚有未取走/可释放事件时，LIMIT 通常可通过 pull/release 缓解。 */
int ms_session_debug_limit_relievable(const ms_session *session);

void ms_ts_map_stream_type(uint8_t stream_type,
                           ms_codec *codec,
                           ms_track_kind *kind,
                           ms_recog_level *level);

#endif /* MSMEDIA_INTERNAL_H */
