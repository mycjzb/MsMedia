/*
 * 解码后 PCM 修剪 carry：按输出样点消耗前缀 / skip_start / skip_end。
 * 不按「一块输出 pop 一条输入标记」。by: Long <mycjzb@gmail.com>
 */
#include "ms_hw_audio_trim.h"

#include <stddef.h>
#include <string.h>

static uint32_t u32_min(uint32_t a, uint32_t b)
{
    return a < b ? a : b;
}

static uint32_t u32_sat_add(uint32_t a, uint32_t b)
{
    if (b > 0xFFFFFFFFu - a) {
        return 0xFFFFFFFFu;
    }
    return a + b;
}

void ms_hw_pcm_trim_init(ms_hw_pcm_trim *t)
{
    if (t == NULL) {
        return;
    }
    memset(t, 0, sizeof(*t));
    t->bpf = 2;
}

void ms_hw_pcm_trim_reset(ms_hw_pcm_trim *t)
{
    size_t bpf;
    if (t == NULL) {
        return;
    }
    bpf = t->bpf;
    memset(t, 0, sizeof(*t));
    t->bpf = (bpf > 0 && bpf <= MS_HW_PCM_TRIM_BPF_MAX) ? bpf : 2;
}

ms_status ms_hw_pcm_trim_set_bpf(ms_hw_pcm_trim *t, size_t bpf)
{
    if (t == NULL || bpf == 0 || bpf > MS_HW_PCM_TRIM_BPF_MAX) {
        return MS_ERR_INVALID_ARG;
    }
    if (t->bpf == bpf) {
        return MS_OK;
    }
    if (t->hold_n > 0 || t->have_cur) {
        return MS_ERR_STATE;
    }
    t->bpf = bpf;
    return MS_OK;
}

ms_status ms_hw_pcm_trim_add_prefix(ms_hw_pcm_trim *t, uint32_t samples)
{
    if (t == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    if (t->overflow) {
        return MS_ERR_OVERFLOW;
    }
    t->prefix_left = u32_sat_add(t->prefix_left, samples);
    return MS_OK;
}

ms_status ms_hw_pcm_trim_push(ms_hw_pcm_trim *t, const ms_hw_pcm_trim_meta *m)
{
    unsigned idx;
    uint32_t start, end, exp;
    if (t == NULL || m == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    if (m->struct_size < offsetof(ms_hw_pcm_trim_meta, expected_samples) +
                         sizeof(uint32_t)) {
        return MS_ERR_INVALID_ARG;
    }
    if (t->overflow) {
        return MS_ERR_OVERFLOW;
    }
    if (t->q_n >= MS_HW_PCM_TRIM_META_MAX) {
        return MS_LIMIT;
    }
    start = m->skip_start;
    end = m->skip_end;
    exp = m->expected_samples;
    idx = (t->q_head + t->q_n) % MS_HW_PCM_TRIM_META_MAX;
    memset(&t->q[idx], 0, sizeof(t->q[idx]));
    t->q[idx].start_left = start;
    t->q[idx].skip_end = end;
    t->q[idx].expected = exp;
    t->q[idx].seen = 0;
    if (m->struct_size >= offsetof(ms_hw_pcm_trim_meta, has_pts) + sizeof(int) &&
        m->has_pts) {
        t->q[idx].has_pts = 1;
        t->q[idx].pts_us = m->pts_us;
    }
    t->q_n++;
    return MS_OK;
}

static unsigned cur_idx(const ms_hw_pcm_trim *t)
{
    return t->q_head;
}

static void pop_cur(ms_hw_pcm_trim *t)
{
    if (t->q_n == 0) {
        t->have_cur = 0;
        return;
    }
    t->q_head = (t->q_head + 1u) % MS_HW_PCM_TRIM_META_MAX;
    t->q_n--;
    t->have_cur = 0;
}

static unsigned cur_idx(const ms_hw_pcm_trim *t);

static void note_emit(ms_hw_pcm_trim *t, uint32_t first_idx, uint32_t nsamp)
{
    if (nsamp == 0) {
        return;
    }
    if (!t->out_meta_valid) {
        if (t->have_cur && t->q_n > 0) {
            t->out_has_pts = t->q[cur_idx(t)].has_pts;
            t->out_au_pts_us = t->q[cur_idx(t)].pts_us;
        } else {
            t->out_has_pts = 0;
            t->out_au_pts_us = 0;
        }
        t->out_first_idx = first_idx;
        t->out_meta_valid = 1;
    }
    t->out_nsamp += nsamp;
}

static void begin_out(ms_hw_pcm_trim *t)
{
    t->out_meta_valid = 0;
    t->out_has_pts = 0;
    t->out_au_pts_us = 0;
    t->out_first_idx = 0;
    t->out_nsamp = 0;
}

void ms_hw_pcm_trim_last_out(const ms_hw_pcm_trim *t, int *has_pts,
                             int64_t *au_pts_us, uint32_t *first_idx,
                             uint32_t *nsamp)
{
    int valid = (t != NULL && t->out_meta_valid);
    if (has_pts != NULL) {
        *has_pts = valid ? t->out_has_pts : 0;
    }
    if (au_pts_us != NULL) {
        *au_pts_us = valid ? t->out_au_pts_us : 0;
    }
    if (first_idx != NULL) {
        *first_idx = valid ? t->out_first_idx : 0;
    }
    if (nsamp != NULL) {
        *nsamp = valid ? t->out_nsamp : 0;
    }
}

static ms_status emit_bytes(ms_hw_pcm_trim *t, const uint8_t *src, uint32_t nsamp,
                            uint8_t *dst, size_t dst_cap, size_t *used,
                            uint32_t first_idx)
{
    size_t need;
    if (nsamp == 0) {
        return MS_OK;
    }
    if (t->bpf == 0) {
        return MS_ERR_STATE;
    }
    need = (size_t)nsamp * t->bpf;
    if (*used > dst_cap || need > dst_cap - *used) {
        return MS_LIMIT;
    }
    if (dst != NULL && src != NULL) {
        memcpy(dst + *used, src, need);
    }
    *used += need;
    note_emit(t, first_idx, nsamp);
    return MS_OK;
}

static ms_status hold_append(ms_hw_pcm_trim *t, const uint8_t *src, uint32_t nsamp)
{
    size_t need, room;
    if (nsamp == 0) {
        return MS_OK;
    }
    if (t->hold_n > MS_HW_PCM_TRIM_HOLD_MAX ||
        nsamp > MS_HW_PCM_TRIM_HOLD_MAX - t->hold_n) {
        t->overflow = 1;
        return MS_ERR_OVERFLOW;
    }
    need = (size_t)nsamp * t->bpf;
    room = (size_t)(MS_HW_PCM_TRIM_HOLD_MAX - t->hold_n) * t->bpf;
    if (need > room) {
        t->overflow = 1;
        return MS_ERR_OVERFLOW;
    }
    memcpy(t->hold + (size_t)t->hold_n * t->bpf, src, need);
    t->hold_n += nsamp;
    return MS_OK;
}

static void hold_drop_front(ms_hw_pcm_trim *t, uint32_t nsamp)
{
    size_t bpf = t->bpf;
    if (nsamp == 0) {
        return;
    }
    if (nsamp >= t->hold_n) {
        t->hold_n = 0;
        return;
    }
    memmove(t->hold, t->hold + (size_t)nsamp * bpf,
            (size_t)(t->hold_n - nsamp) * bpf);
    t->hold_n -= nsamp;
}

/* 未知 expected：hold 只保留尾部 skip_end，更早的样点可听。 */
static ms_status hold_release_audible(ms_hw_pcm_trim *t, uint32_t keep_end,
                                      uint8_t *dst, size_t dst_cap, size_t *used)
{
    uint32_t emit;
    ms_status st;
    if (t->hold_n <= keep_end) {
        return MS_OK;
    }
    emit = t->hold_n - keep_end;
    {
        uint32_t first = 0;
        if (t->have_cur && t->q_n > 0 && t->q[cur_idx(t)].seen >= t->hold_n) {
            first = t->q[cur_idx(t)].seen - t->hold_n;
        }
        st = emit_bytes(t, t->hold, emit, dst, dst_cap, used, first);
    }
    if (st != MS_OK) {
        return st;
    }
    hold_drop_front(t, emit);
    return MS_OK;
}

static ms_status finalize_cur(ms_hw_pcm_trim *t, uint8_t *dst, size_t dst_cap,
                              size_t *used)
{
    ms_status st;
    uint32_t keep;
    if (!t->have_cur || t->q_n == 0) {
        t->have_cur = 0;
        return MS_OK;
    }
    keep = t->q[cur_idx(t)].skip_end;
    /* 未用完的 start 结转到后续前缀，多包 skip 可跨 AU。 */
    if (t->q[cur_idx(t)].start_left > 0) {
        t->prefix_left = u32_sat_add(t->prefix_left, t->q[cur_idx(t)].start_left);
    }
    if (t->hold_n > 0) {
        st = hold_release_audible(t, keep, dst, dst_cap, used);
        if (st != MS_OK) {
            return st;
        }
        /* 尾部 padding 丢掉，不写出。 */
        t->hold_n = 0;
    }
    pop_cur(t);
    return MS_OK;
}

static ms_status ensure_cur(ms_hw_pcm_trim *t)
{
    if (t->have_cur && t->q_n > 0) {
        return MS_OK;
    }
    if (t->q_n == 0) {
        return MS_AGAIN;
    }
    t->have_cur = 1;
    return MS_OK;
}

static int peek_next(const ms_hw_pcm_trim *t, int *has_pts, int64_t *pts)
{
    unsigned idx;
    if (t->q_n < 2) {
        return 0;
    }
    idx = (t->q_head + 1u) % MS_HW_PCM_TRIM_META_MAX;
    *has_pts = t->q[idx].has_pts;
    *pts = t->q[idx].pts_us;
    return 1;
}

static ms_status maybe_pts_switch(ms_hw_pcm_trim *t, int has_pts, int64_t pts_us,
                                  uint8_t *dst, size_t dst_cap, size_t *used)
{
    int n_has = 0;
    int64_t n_pts = 0;
    if (!has_pts || !t->have_cur || t->q_n == 0) {
        return MS_OK;
    }
    if (t->q[cur_idx(t)].has_pts && pts_us < t->q[cur_idx(t)].pts_us &&
        t->q[cur_idx(t)].seen > 0) {
        /* 输出时间戳早于当前 AU：视为重排/不匹配，不静默错扣。 */
        return MS_ERR_STATE;
    }
    if (t->q[cur_idx(t)].seen == 0) {
        return MS_OK;
    }
    if (peek_next(t, &n_has, &n_pts) && n_has &&
        pts_us == n_pts &&
        (!t->q[cur_idx(t)].has_pts || pts_us != t->q[cur_idx(t)].pts_us)) {
        return finalize_cur(t, dst, dst_cap, used);
    }
    return MS_OK;
}

/* 已知 expected：样点区间 [start, expected-end) 可听。start 已在进入 body 前扣完。 */
static uint32_t audible_end(const ms_hw_pcm_trim *t)
{
    unsigned i = cur_idx(t);
    uint32_t exp = t->q[i].expected;
    uint32_t end = t->q[i].skip_end;
    if (exp == 0) {
        return 0xFFFFFFFFu;
    }
    if (end >= exp) {
        return 0;
    }
    return exp - end;
}

static int au_done(const ms_hw_pcm_trim *t)
{
    unsigned i;
    if (!t->have_cur || t->q_n == 0) {
        return 1;
    }
    i = cur_idx(t);
    if (t->q[i].expected > 0 && t->q[i].seen >= t->q[i].expected) {
        return 1;
    }
    return 0;
}

ms_status ms_hw_pcm_trim_feed(ms_hw_pcm_trim *t,
                              const uint8_t *src, uint32_t nsamp,
                              int has_pts, int64_t pts_us,
                              uint8_t *dst, size_t dst_cap, size_t *out_bytes)
{
    size_t used = 0;
    uint32_t off = 0;
    ms_status st;
    if (t == NULL || out_bytes == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    *out_bytes = 0;
    if (t->overflow) {
        return MS_ERR_OVERFLOW;
    }
    if (t->bpf == 0 || t->bpf > MS_HW_PCM_TRIM_BPF_MAX) {
        return MS_ERR_STATE;
    }
    if (nsamp > 0 && src == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    if (nsamp == 0) {
        return MS_OK;
    }
    begin_out(t);
    st = maybe_pts_switch(t, has_pts, pts_us, dst, dst_cap, &used);
    if (st != MS_OK) {
        return st;
    }
    while (off < nsamp) {
        uint32_t left = nsamp - off;
        const uint8_t *p = src + (size_t)off * t->bpf;
        if (t->prefix_left > 0) {
            uint32_t drop, room = 0xFFFFFFFFu;
            /* 前缀（pre-skip / seek warmup）仍计入当前 AU 的 expected，
             * 避免把下一包 PCM 误接到未完成的 AU。 */
            st = ensure_cur(t);
            if (st == MS_AGAIN) {
                drop = u32_min(left, t->prefix_left);
                t->prefix_left -= drop;
                off += drop;
                continue;
            }
            if (st != MS_OK) {
                return st;
            }
            if (t->q[cur_idx(t)].expected > 0) {
                uint32_t seen = t->q[cur_idx(t)].seen;
                uint32_t exp = t->q[cur_idx(t)].expected;
                room = (seen < exp) ? (exp - seen) : 0u;
            }
            drop = u32_min(left, t->prefix_left);
            drop = u32_min(drop, room);
            if (drop == 0u) {
                st = finalize_cur(t, dst, dst_cap, &used);
                if (st != MS_OK) {
                    return st;
                }
                continue;
            }
            t->prefix_left -= drop;
            t->q[cur_idx(t)].seen = u32_sat_add(t->q[cur_idx(t)].seen, drop);
            off += drop;
            if (au_done(t)) {
                st = finalize_cur(t, dst, dst_cap, &used);
                if (st != MS_OK) {
                    return st;
                }
            }
            continue;
        }
        st = ensure_cur(t);
        if (st == MS_AGAIN) {
            /* 没有对应元数据的多余 PCM：诚实失败，不静默当可听。 */
            return MS_ERR_STATE;
        }
        if (st != MS_OK) {
            return st;
        }
        st = maybe_pts_switch(t, has_pts && off == 0, pts_us, dst, dst_cap, &used);
        if (st != MS_OK) {
            return st;
        }
        if (!t->have_cur) {
            continue;
        }
        if (t->q[cur_idx(t)].start_left > 0) {
            uint32_t drop = u32_min(left, t->q[cur_idx(t)].start_left);
            t->q[cur_idx(t)].start_left -= drop;
            t->q[cur_idx(t)].seen = u32_sat_add(t->q[cur_idx(t)].seen, drop);
            off += drop;
            if (au_done(t)) {
                st = finalize_cur(t, dst, dst_cap, &used);
                if (st != MS_OK) {
                    return st;
                }
            }
            continue;
        }
        if (t->q[cur_idx(t)].expected > 0) {
            uint32_t seen = t->q[cur_idx(t)].seen;
            uint32_t exp = t->q[cur_idx(t)].expected;
            uint32_t aend = audible_end(t);
            uint32_t room, take;
            if (seen >= exp) {
                st = finalize_cur(t, dst, dst_cap, &used);
                if (st != MS_OK) {
                    return st;
                }
                continue;
            }
            room = exp - seen;
            take = u32_min(left, room);
            if (seen < aend) {
                uint32_t emit = u32_min(take, aend - seen);
                st = emit_bytes(t, p, emit, dst, dst_cap, &used, seen);
                if (st != MS_OK) {
                    return st;
                }
                /* 其余 take-emit 落在 end padding，丢弃。 */
            }
            t->q[cur_idx(t)].seen = u32_sat_add(seen, take);
            off += take;
            if (au_done(t)) {
                st = finalize_cur(t, dst, dst_cap, &used);
                if (st != MS_OK) {
                    return st;
                }
            }
            continue;
        }
        /* expected 未知：body 进入有界 hold，只保留尾部 skip_end。 */
        {
            uint32_t take = left;
            st = hold_append(t, p, take);
            if (st != MS_OK) {
                return st;
            }
            t->q[cur_idx(t)].seen = u32_sat_add(t->q[cur_idx(t)].seen, take);
            off += take;
            st = hold_release_audible(t, t->q[cur_idx(t)].skip_end, dst, dst_cap,
                                      &used);
            if (st != MS_OK) {
                return st;
            }
        }
    }
    *out_bytes = used;
    return MS_OK;
}

ms_status ms_hw_pcm_trim_eos(ms_hw_pcm_trim *t,
                             uint8_t *dst, size_t dst_cap, size_t *out_bytes)
{
    size_t used = 0;
    ms_status st;
    if (t == NULL || out_bytes == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    *out_bytes = 0;
    if (t->overflow) {
        return MS_ERR_OVERFLOW;
    }
    begin_out(t);
    while (t->have_cur || t->q_n > 0) {
        if (!t->have_cur) {
            st = ensure_cur(t);
            if (st == MS_AGAIN) {
                break;
            }
            if (st != MS_OK) {
                return st;
            }
        }
        st = finalize_cur(t, dst, dst_cap, &used);
        if (st != MS_OK) {
            return st;
        }
    }
    /* 无 meta 的残留 hold：不应出现；丢掉以免把未修剪尾部写出。 */
    t->hold_n = 0;
    t->prefix_left = 0;
    *out_bytes = used;
    return MS_OK;
}

unsigned ms_hw_pcm_trim_pending_meta(const ms_hw_pcm_trim *t)
{
    return t != NULL ? t->q_n : 0u;
}

uint32_t ms_hw_pcm_trim_hold_samples(const ms_hw_pcm_trim *t)
{
    return t != NULL ? t->hold_n : 0u;
}

uint32_t ms_hw_pcm_trim_prefix_left(const ms_hw_pcm_trim *t)
{
    return t != NULL ? t->prefix_left : 0u;
}
