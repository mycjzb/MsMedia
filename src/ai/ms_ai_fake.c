/*
 * 受控本地假供应商：根据已抽到的 PCM 内容/时间戳生成明确标示的模拟文本。
 * 不是真实语音识别，不访问网络，不加载模型。
 * by: Long <mycjzb@gmail.com>
 */
#include "msmedia/ms_ai_sub.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MS_AI_FAKE_QMAX 32
#define MS_AI_FAKE_TEXT (MS_SUB_CUE_TEXT_MAX + 64)

typedef struct fake_item {
    int kind;
    uint32_t utterance_id;
    uint32_t revision;
    uint32_t session_epoch;
    uint32_t timeline_epoch;
    uint32_t selection_epoch;
    int has_start;
    int has_end;
    int64_t start_90k;
    int64_t end_90k;
    char text[MS_AI_FAKE_TEXT];
    size_t text_size;
    char lang[MS_AI_LANG_MAX];
    int hold_left;
} fake_item;

typedef struct fake_ctx {
    ms_allocator alloc;
    ms_ai_fake_opts opts;
    ms_ai_plugin_cfg cfg;
    int started;
    uint32_t next_utt;
    uint32_t sticky_used;
    fake_item q[MS_AI_FAKE_QMAX];
    unsigned q_n;
    unsigned q_head;
    char poll_text[MS_AI_FAKE_TEXT];
    int last_error;
} fake_ctx;

static void *dflt_alloc(void *ctx, size_t n)
{
    (void)ctx;
    return malloc(n ? n : 1u);
}

static void dflt_free(void *ctx, void *p)
{
    (void)ctx;
    free(p);
}

static const ms_allocator k_dflt = { dflt_alloc, dflt_free, NULL };

void ms_ai_fake_opts_default(ms_ai_fake_opts *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->struct_size = sizeof(*out);
    out->emit_partial = 1;
    out->emit_final = 1;
    out->emit_translation = 0;
}

static void copy_opts(ms_ai_fake_opts *dst, const ms_ai_fake_opts *src)
{
    ms_ai_fake_opts_default(dst);
    if (src == NULL) {
        return;
    }
    if (src->struct_size >= sizeof(*src)) {
        *dst = *src;
        dst->struct_size = sizeof(*dst);
    } else if (src->struct_size >= sizeof(size_t)) {
        memcpy(dst, src, src->struct_size);
        dst->struct_size = sizeof(*dst);
    }
}

static int64_t us_to_90k(int64_t us)
{
    return (us * 90) / 1000;
}

static ms_status q_push(fake_ctx *f, const fake_item *it)
{
    unsigned tail;
    if (f->q_n >= MS_AI_FAKE_QMAX) {
        return MS_ERR_OVERFLOW;
    }
    tail = (f->q_head + f->q_n) % MS_AI_FAKE_QMAX;
    f->q[tail] = *it;
    f->q_n++;
    return MS_OK;
}

static ms_status fake_caps(void *ctx, ms_ai_vendor_caps *out)
{
    size_t sz;
    (void)ctx;
    if (out == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    sz = out->struct_size ? out->struct_size : sizeof(*out);
    if (sz > sizeof(*out)) {
        sz = sizeof(*out);
    }
    memset(out, 0, sz);
    out->struct_size = sz;
    if (sz >= offsetof(ms_ai_vendor_caps, id) + sizeof(const char *)) {
        out->id = "ms-ai-fake-local";
    }
    if (sz >= offsetof(ms_ai_vendor_caps, display) + sizeof(const char *)) {
        out->display = "simulated local fake vendor (not real ASR)";
    }
    if (sz >= offsetof(ms_ai_vendor_caps, is_simulated) + sizeof(int)) {
        out->is_simulated = 1;
    }
    if (sz >= offsetof(ms_ai_vendor_caps, can_transcribe) + sizeof(int)) {
        out->can_transcribe = 1;
    }
    if (sz >= offsetof(ms_ai_vendor_caps, can_translate) + sizeof(int)) {
        out->can_translate = 1;
    }
    if (sz >= offsetof(ms_ai_vendor_caps, needs_network) + sizeof(int)) {
        out->needs_network = 0;
    }
    if (sz >= offsetof(ms_ai_vendor_caps, needs_model) + sizeof(int)) {
        out->needs_model = 0;
    }
    if (sz >= offsetof(ms_ai_vendor_caps, vendor) + sizeof(int)) {
        out->vendor = MS_AI_VENDOR_FAKE_LOCAL;
    }
    return MS_OK;
}

static ms_status fake_start(void *ctx, const ms_ai_plugin_cfg *cfg)
{
    fake_ctx *f = (fake_ctx *)ctx;
    if (f == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    memset(&f->cfg, 0, sizeof(f->cfg));
    f->cfg.struct_size = sizeof(f->cfg);
    if (cfg != NULL) {
        size_t n = cfg->struct_size;
        if (n == 0 || n > sizeof(f->cfg)) {
            n = sizeof(f->cfg);
        }
        memcpy(&f->cfg, cfg, n);
        f->cfg.struct_size = sizeof(f->cfg);
    }
    f->started = 1;
    f->last_error = 0;
    return MS_OK;
}

static void fake_cancel(void *ctx, uint32_t session_epoch, uint32_t timeline_epoch,
                        uint32_t selection_epoch)
{
    fake_ctx *f = (fake_ctx *)ctx;
    unsigned i, w = 0;
    if (f == NULL) {
        return;
    }
    for (i = 0; i < f->q_n; i++) {
        fake_item *it = &f->q[(f->q_head + i) % MS_AI_FAKE_QMAX];
        if (it->session_epoch != session_epoch ||
            it->timeline_epoch != timeline_epoch ||
            it->selection_epoch != selection_epoch) {
            continue;
        }
        /* 丢弃仍属于被取消世代的条目 */
    }
    (void)w;
    (void)session_epoch;
    (void)timeline_epoch;
    (void)selection_epoch;
    if (f->opts.ignore_cancel) {
        return;
    }
    /* 默认清空在途模拟结果。ignore_cancel 用于验证引擎仍按 epoch 丢弃迟到回调。 */
    f->q_n = 0;
    f->q_head = 0;
}

static void fake_stop(void *ctx)
{
    fake_ctx *f = (fake_ctx *)ctx;
    if (f == NULL) {
        return;
    }
    f->started = 0;
    f->q_n = 0;
    f->q_head = 0;
}

static void fill_sim_text(char *dst, size_t cap, const char *tag,
                          const ms_ai_pcm_chunk *ch, int16_t first, int16_t last)
{
    int64_t pts = ch->has_pts ? ch->pts_us : -1;
    snprintf(dst, cap,
             "SIM-%s pcm0=%d pcmN=%d n=%zu pts_us=%lld rate=%d ch=%d",
             tag, (int)first, (int)last, ch->nframes, (long long)pts,
             ch->sample_rate, ch->channels);
}

static ms_status enqueue_kind(fake_ctx *f, const ms_ai_pcm_chunk *ch,
                              int kind, uint32_t utt, uint32_t rev,
                              const char *tag, const char *lang,
                              int16_t first, int16_t last, int hold)
{
    fake_item it;
    int64_t dur_90k = 0;
    memset(&it, 0, sizeof(it));
    it.kind = kind;
    it.utterance_id = utt;
    it.revision = rev;
    it.session_epoch = ch->session_epoch;
    it.timeline_epoch = ch->timeline_epoch;
    it.selection_epoch = ch->selection_epoch;
    it.has_start = ch->has_pts;
    it.has_end = ch->has_pts;
    it.start_90k = ch->has_pts ? us_to_90k(ch->pts_us) : 0;
    if (ch->sample_rate > 0) {
        dur_90k = (int64_t)((ch->nframes * 90000ull) / (unsigned)ch->sample_rate);
    }
    it.end_90k = it.start_90k + dur_90k;
    if (f->opts.force_oversize) {
        memset(it.text, 'A', sizeof(it.text) - 1u);
        it.text[sizeof(it.text) - 1u] = 0;
        it.text_size = sizeof(it.text) - 1u;
    } else {
        fill_sim_text(it.text, sizeof(it.text), tag, ch, first, last);
        it.text_size = strlen(it.text);
    }
    if (lang != NULL) {
        snprintf(it.lang, sizeof(it.lang), "%s", lang);
    }
    it.hold_left = hold;
    return q_push(f, &it);
}

static ms_status fake_submit(void *ctx, const ms_ai_pcm_chunk *chunk)
{
    fake_ctx *f = (fake_ctx *)ctx;
    int16_t first = 0, last = 0;
    uint32_t utt;
    int hold;
    ms_status st;
    const char *tr_lang;
    if (f == NULL || chunk == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    if (!f->started) {
        return MS_ERR_STATE;
    }
    if (f->opts.force_error) {
        f->last_error = 1;
        return MS_ERR_UNKNOWN;
    }
    if (chunk->pcm_bits != 0 && chunk->pcm_bits != 16) {
        return MS_ERR_UNSUPPORTED;
    }
    if (chunk->samples != NULL && chunk->nframes > 0 && chunk->channels > 0) {
        first = chunk->samples[0];
        last = chunk->samples[(chunk->nframes - 1u) * (size_t)chunk->channels];
    }
    if (f->opts.sticky_id != 0) {
        utt = f->opts.sticky_id;
        f->sticky_used++;
    } else {
        f->next_utt++;
        if (f->next_utt == 0) {
            f->next_utt = 1;
        }
        utt = f->next_utt;
    }
    hold = f->opts.hold_polls;
    if (f->opts.reorder) {
        if (f->opts.emit_final) {
            st = enqueue_kind(f, chunk, MS_AI_RESULT_FINAL, utt, 2, "F",
                              f->cfg.source_lang, first, last, hold);
            if (st != MS_OK) {
                return st;
            }
        }
        if (f->opts.emit_partial) {
            st = enqueue_kind(f, chunk, MS_AI_RESULT_PARTIAL, utt, 1, "P",
                              f->cfg.source_lang, first, last, hold);
            if (st != MS_OK) {
                return st;
            }
        }
    } else {
        if (f->opts.emit_partial) {
            st = enqueue_kind(f, chunk, MS_AI_RESULT_PARTIAL, utt, 1, "P",
                              f->cfg.source_lang, first, last, hold);
            if (st != MS_OK) {
                return st;
            }
        }
        if (f->opts.emit_final) {
            st = enqueue_kind(f, chunk, MS_AI_RESULT_FINAL, utt, 2, "F",
                              f->cfg.source_lang, first, last, hold);
            if (st != MS_OK) {
                return st;
            }
        }
    }
    tr_lang = f->opts.target_lang[0] ? f->opts.target_lang : f->cfg.target_lang;
    if (f->opts.emit_translation && tr_lang[0] != 0) {
        fake_item it;
        memset(&it, 0, sizeof(it));
        it.kind = MS_AI_RESULT_TRANSLATION;
        it.utterance_id = utt;
        it.revision = 3;
        it.session_epoch = chunk->session_epoch;
        it.timeline_epoch = chunk->timeline_epoch;
        it.selection_epoch = chunk->selection_epoch;
        it.has_start = chunk->has_pts;
        it.has_end = chunk->has_pts;
        it.start_90k = chunk->has_pts ? us_to_90k(chunk->pts_us) : 0;
        if (chunk->sample_rate > 0) {
            it.end_90k = it.start_90k +
                         (int64_t)((chunk->nframes * 90000ull) / (unsigned)chunk->sample_rate);
        }
        if (f->opts.force_oversize) {
            memset(it.text, 'B', sizeof(it.text) - 1u);
            it.text[sizeof(it.text) - 1u] = 0;
            it.text_size = sizeof(it.text) - 1u;
        } else {
            snprintf(it.text, sizeof(it.text),
                     "SIM-TR[%s] pcm0=%d n=%zu pts_us=%lld",
                     tr_lang, (int)first, chunk->nframes,
                     (long long)(chunk->has_pts ? chunk->pts_us : -1));
            it.text_size = strlen(it.text);
        }
        snprintf(it.lang, sizeof(it.lang), "%s", tr_lang);
        it.hold_left = hold;
        st = q_push(f, &it);
        if (st != MS_OK) {
            return st;
        }
    }
    return MS_OK;
}

static ms_status fake_poll(void *ctx, ms_ai_result *out)
{
    fake_ctx *f = (fake_ctx *)ctx;
    unsigned i;
    if (f == NULL || out == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    if (f->last_error) {
        f->last_error = 0;
        return MS_ERR_UNKNOWN;
    }
    for (i = 0; i < f->q_n; i++) {
        unsigned ix = (f->q_head + i) % MS_AI_FAKE_QMAX;
        fake_item *it = &f->q[ix];
        if (it->hold_left > 0) {
            it->hold_left--;
            continue;
        }
        {
            size_t sz = out->struct_size ? out->struct_size : sizeof(*out);
            if (sz > sizeof(*out)) {
                sz = sizeof(*out);
            }
            memset(out, 0, sz);
            out->struct_size = sz;
            snprintf(f->poll_text, sizeof(f->poll_text), "%s", it->text);
            out->kind = it->kind;
            out->utterance_id = it->utterance_id;
            out->revision = it->revision;
            out->session_epoch = it->session_epoch;
            out->timeline_epoch = it->timeline_epoch;
            out->selection_epoch = it->selection_epoch;
            out->has_start = it->has_start;
            out->has_end = it->has_end;
            out->start_90k = it->start_90k;
            out->end_90k = it->end_90k;
            out->text = f->poll_text;
            out->text_size = it->text_size;
            memcpy(out->lang, it->lang, sizeof(out->lang));
            out->is_simulated = 1;
        }
        /* 弹出该条目，保持剩余顺序 */
        if (i == 0) {
            f->q_head = (f->q_head + 1u) % MS_AI_FAKE_QMAX;
            f->q_n--;
        } else {
            unsigned k;
            for (k = i; k + 1u < f->q_n; k++) {
                f->q[(f->q_head + k) % MS_AI_FAKE_QMAX] =
                    f->q[(f->q_head + k + 1u) % MS_AI_FAKE_QMAX];
            }
            f->q_n--;
        }
        return MS_OK;
    }
    return MS_AGAIN;
}

ms_status ms_ai_fake_vendor_init(const ms_allocator *alloc,
                                 const ms_ai_fake_opts *opts,
                                 ms_ai_plugin_vt *out)
{
    fake_ctx *f;
    if (out == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    if (alloc == NULL) {
        alloc = &k_dflt;
    }
    if (alloc->alloc_fn == NULL || alloc->free_fn == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    f = (fake_ctx *)alloc->alloc_fn(alloc->ctx, sizeof(*f));
    if (f == NULL) {
        return MS_ERR_NOMEM;
    }
    memset(f, 0, sizeof(*f));
    f->alloc = *alloc;
    copy_opts(&f->opts, opts);
    f->next_utt = 0;
    out->struct_size = sizeof(*out);
    out->ctx = f;
    out->query_caps = fake_caps;
    out->start = fake_start;
    out->cancel = fake_cancel;
    out->stop = fake_stop;
    out->submit_pcm = fake_submit;
    out->poll_result = fake_poll;
    return MS_OK;
}

void ms_ai_fake_vendor_free(ms_ai_plugin_vt *vt)
{
    fake_ctx *f;
    if (vt == NULL || vt->ctx == NULL) {
        return;
    }
    f = (fake_ctx *)vt->ctx;
    if (f->alloc.free_fn != NULL) {
        f->alloc.free_fn(f->alloc.ctx, f);
    }
    memset(vt, 0, sizeof(*vt));
}

ms_status ms_ai_fake_set_opts(ms_ai_plugin_vt *vt, const ms_ai_fake_opts *opts)
{
    fake_ctx *f;
    if (vt == NULL || vt->ctx == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    f = (fake_ctx *)vt->ctx;
    copy_opts(&f->opts, opts);
    return MS_OK;
}
