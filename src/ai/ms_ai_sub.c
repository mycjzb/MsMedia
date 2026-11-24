#define _DEFAULT_SOURCE
/*
 * 可选 AI 字幕引擎：有界 PCM 队列、异步工作线程、epoch 隔离、
 * 将 partial/final/翻译归并到 ms_sub 生成轨。默认禁用，无模型与网络。
 * 源音频与识别文本不得写入日志。
 * by: Long <mycjzb@gmail.com>
 */
#include "msmedia/ms_ai_sub.h"

#include <pthread.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef MS_AI_LIVE_MAX
#define MS_AI_LIVE_MAX 8
#endif
#ifndef MS_AI_CHUNK_BYTES_MAX
#define MS_AI_CHUNK_BYTES_MAX (64u * 1024u)
#endif

typedef struct q_chunk {
    uint8_t *pcm;
    size_t bytes;
    int sample_rate;
    int channels;
    int pcm_bits;
    int has_pts;
    int64_t pts_us;
    uint32_t session_epoch;
    uint32_t timeline_epoch;
    uint32_t selection_epoch;
    int64_t enq_mono_us;
} q_chunk;

typedef struct live_utt {
    int in_use;
    uint32_t utterance_id;
    uint32_t tr_rev;
    uint32_t asr_rev;
    int asr_kind; /* partial/final */
    int has_tr;
    int has_start;
    int has_end;
    int64_t start_90k;
    int64_t end_90k;
    int64_t delay_90k;
    uint32_t session_epoch;
    uint32_t timeline_epoch;
    uint32_t selection_epoch;
    char text[MS_SUB_CUE_TEXT_MAX + 1];
    size_t text_size;
    char tr[MS_SUB_CUE_TEXT_MAX + 1];
    size_t tr_size;
    char lang[MS_AI_LANG_MAX];
    char tr_lang[MS_AI_LANG_MAX];
} live_utt;

struct ms_ai_sub {
    ms_allocator alloc;
    ms_ai_sub_opts opts;
    ms_ai_plugin_vt plugin;
    int plugin_owned; /* use_fake_local 时由本引擎释放 */
    int enabled;
    int configured;
    int stop_req;
    int worker_running;
    pthread_t worker;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int mu_ok;
    q_chunk *q;
    unsigned q_cap;
    unsigned q_n;
    unsigned q_head;
    size_t q_bytes;
    uint32_t session_epoch;
    uint32_t timeline_epoch;
    uint32_t selection_epoch;
    ms_sub_doc *doc;
    live_utt live[MS_AI_LIVE_MAX];
    ms_ai_sub_stats stats;
    char status[MS_AI_STATUS_MAX];
};

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

static int64_t mono_us(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (int64_t)ts.tv_sec * 1000000ll + (int64_t)(ts.tv_nsec / 1000ll);
}

static void *xalloc(ms_ai_sub *s, size_t n)
{
    return s->alloc.alloc_fn(s->alloc.ctx, n);
}

static void xfree(ms_ai_sub *s, void *p)
{
    if (p != NULL) {
        s->alloc.free_fn(s->alloc.ctx, p);
    }
}

void ms_ai_sub_opts_default(ms_ai_sub_opts *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->struct_size = sizeof(*out);
    out->vendor = MS_AI_VENDOR_NONE;
    out->want_partial = 1;
    out->want_translation = 0;
    out->target_rate = 16000;
    out->target_channels = 1;
    out->queue_chunks_max = MS_AI_QUEUE_CHUNKS_DEFAULT;
    out->queue_bytes_max = MS_AI_QUEUE_BYTES_DEFAULT;
    out->timeout_ms = 8000;
}

static void copy_opts(ms_ai_sub_opts *dst, const ms_ai_sub_opts *src)
{
    ms_ai_sub_opts_default(dst);
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
    if (dst->queue_chunks_max == 0) {
        dst->queue_chunks_max = MS_AI_QUEUE_CHUNKS_DEFAULT;
    }
    if (dst->queue_bytes_max == 0) {
        dst->queue_bytes_max = MS_AI_QUEUE_BYTES_DEFAULT;
    }
}

static void set_status(ms_ai_sub *s, const char *t)
{
    if (t == NULL) {
        t = "disabled";
    }
    snprintf(s->status, sizeof(s->status), "%s", t);
    snprintf(s->stats.status, sizeof(s->stats.status), "%s", t);
}

static void free_chunk_buf(ms_ai_sub *s, q_chunk *c)
{
    if (c->pcm != NULL) {
        xfree(s, c->pcm);
        c->pcm = NULL;
    }
    if (s->q_bytes >= c->bytes) {
        s->q_bytes -= c->bytes;
    } else {
        s->q_bytes = 0;
    }
    c->bytes = 0;
}

static void drop_oldest(ms_ai_sub *s)
{
    q_chunk *c;
    if (s->q_n == 0) {
        return;
    }
    c = &s->q[s->q_head];
    free_chunk_buf(s, c);
    memset(c, 0, sizeof(*c));
    s->q_head = (s->q_head + 1u) % s->q_cap;
    s->q_n--;
    s->stats.queue_drops++;
}

static void clear_queue_locked(ms_ai_sub *s)
{
    while (s->q_n > 0) {
        q_chunk *c = &s->q[s->q_head];
        free_chunk_buf(s, c);
        memset(c, 0, sizeof(*c));
        s->q_head = (s->q_head + 1u) % (s->q_cap ? s->q_cap : 1u);
        s->q_n--;
    }
    s->q_bytes = 0;
    s->q_head = 0;
}

static void clear_live(ms_ai_sub *s)
{
    memset(s->live, 0, sizeof(s->live));
    if (s->doc != NULL) {
        ms_sub_doc_clear_cues(s->doc);
    }
}

static void fill_cue_caps(ms_sub_cue *c, int kind)
{
    c->cap_flags = MS_SUB_CAP_AI_GENERATED | MS_SUB_CAP_AI_SIMULATED;
    if (kind == MS_AI_RESULT_PARTIAL) {
        c->cap_flags |= MS_SUB_CAP_AI_PARTIAL;
        c->ai_kind = MS_AI_RESULT_PARTIAL;
    } else if (kind == MS_AI_RESULT_FINAL) {
        c->cap_flags |= MS_SUB_CAP_AI_FINAL;
        c->ai_kind = MS_AI_RESULT_FINAL;
    } else if (kind == MS_AI_RESULT_TRANSLATION) {
        c->cap_flags |= MS_SUB_CAP_AI_TRANSLATION;
        c->ai_kind = MS_AI_RESULT_TRANSLATION;
    }
}

static ms_status rebuild_doc(ms_ai_sub *s)
{
    int i;
    if (s->doc == NULL) {
        return MS_OK;
    }
    ms_sub_doc_clear_cues(s->doc);
    for (i = 0; i < MS_AI_LIVE_MAX; i++) {
        live_utt *u = &s->live[i];
        ms_sub_cue cue;
        ms_status st;
        if (!u->in_use) {
            continue;
        }
        if (u->text_size > 0) {
            ms_sub_cue_default(&cue);
            cue.track_id = MS_TRACK_ID_AI_SUB;
            cue.has_start = u->has_start;
            cue.has_end = u->has_end;
            cue.start_90k = (uint64_t)u->start_90k;
            cue.end_90k = (uint64_t)u->end_90k;
            cue.text = u->text;
            cue.text_size = u->text_size;
            fill_cue_caps(&cue, u->asr_kind);
            cue.ai_utterance_id = u->utterance_id;
            cue.ai_revision = u->asr_rev;
            cue.ai_epoch = u->timeline_epoch;
            cue.ai_delay_90k = u->delay_90k;
            snprintf(cue.ai_lang, sizeof(cue.ai_lang), "%s", u->lang);
            st = ms_sub_doc_add_cue(s->doc, &cue);
            if (st != MS_OK) {
                return st;
            }
        }
        if (u->has_tr && u->tr_size > 0) {
            ms_sub_cue_default(&cue);
            cue.track_id = MS_TRACK_ID_AI_SUB;
            cue.has_start = u->has_start;
            cue.has_end = u->has_end;
            cue.start_90k = (uint64_t)u->start_90k;
            cue.end_90k = (uint64_t)u->end_90k;
            cue.text = u->tr;
            cue.text_size = u->tr_size;
            fill_cue_caps(&cue, MS_AI_RESULT_TRANSLATION);
            cue.ai_utterance_id = u->utterance_id;
            cue.ai_revision = u->tr_rev;
            cue.ai_epoch = u->timeline_epoch;
            cue.ai_delay_90k = u->delay_90k;
            snprintf(cue.ai_lang, sizeof(cue.ai_lang), "%s", u->tr_lang);
            st = ms_sub_doc_add_cue(s->doc, &cue);
            if (st != MS_OK) {
                return st;
            }
        }
    }
    return MS_OK;
}

static live_utt *live_find_or_alloc(ms_ai_sub *s, uint32_t id)
{
    int i, free_i = -1, oldest = 0;
    uint32_t min_id = 0xFFFFFFFFu;
    for (i = 0; i < MS_AI_LIVE_MAX; i++) {
        if (s->live[i].in_use && s->live[i].utterance_id == id) {
            return &s->live[i];
        }
        if (!s->live[i].in_use && free_i < 0) {
            free_i = i;
        }
        if (s->live[i].in_use && s->live[i].utterance_id < min_id) {
            min_id = s->live[i].utterance_id;
            oldest = i;
        }
    }
    if (free_i < 0) {
        free_i = oldest;
        memset(&s->live[free_i], 0, sizeof(s->live[free_i]));
    }
    s->live[free_i].in_use = 1;
    s->live[free_i].utterance_id = id;
    return &s->live[free_i];
}

static int epochs_match(const ms_ai_sub *s, uint32_t sess, uint32_t tl, uint32_t sel)
{
    return sess == s->session_epoch && tl == s->timeline_epoch &&
           sel == s->selection_epoch;
}

static ms_status merge_result(ms_ai_sub *s, const ms_ai_result *r)
{
    live_utt *u;
    const char *text;
    size_t tlen;
    if (r == NULL || r->is_simulated == 0) {
        /* 即使未来实接，本切片也只接受带标示的结果进入测试路径；
         * 未标示的结果仍归并，但不假装已完成真实识别。 */
    }
    if (!epochs_match(s, r->session_epoch, r->timeline_epoch, r->selection_epoch)) {
        s->stats.results_dropped_epoch++;
        return MS_OK;
    }
    text = r->text != NULL ? r->text : "";
    tlen = r->text_size;
    if (tlen == 0 && text[0] != 0) {
        tlen = strlen(text);
    }
    if (tlen > MS_SUB_CUE_TEXT_MAX) {
        s->stats.results_dropped_oversize++;
        return MS_OK;
    }
    u = live_find_or_alloc(s, r->utterance_id != 0 ? r->utterance_id : 1u);
    if (u == NULL) {
        return MS_ERR_OVERFLOW;
    }
    u->session_epoch = r->session_epoch;
    u->timeline_epoch = r->timeline_epoch;
    u->selection_epoch = r->selection_epoch;
    if (r->has_start) {
        u->has_start = 1;
        u->start_90k = r->start_90k;
    }
    if (r->has_end) {
        u->has_end = 1;
        u->end_90k = r->end_90k;
    }
    u->delay_90k = r->delay_90k;
    if (r->kind == MS_AI_RESULT_TRANSLATION) {
        if (r->revision < u->tr_rev) {
            return MS_OK; /* 乱序旧翻译 */
        }
        memcpy(u->tr, text, tlen);
        u->tr[tlen] = 0;
        u->tr_size = tlen;
        u->has_tr = 1;
        u->tr_rev = r->revision;
        memcpy(u->tr_lang, r->lang, sizeof(u->tr_lang));
        u->tr_lang[MS_AI_LANG_MAX - 1] = 0;
    } else {
        /* final 不被更旧或同级 partial 降级 */
        if (u->asr_kind == MS_AI_RESULT_FINAL && r->kind == MS_AI_RESULT_PARTIAL) {
            return MS_OK;
        }
        if (r->revision < u->asr_rev && r->kind == u->asr_kind) {
            return MS_OK;
        }
        memcpy(u->text, text, tlen);
        u->text[tlen] = 0;
        u->text_size = tlen;
        u->asr_rev = r->revision;
        u->asr_kind = r->kind;
        memcpy(u->lang, r->lang, sizeof(u->lang));
        u->lang[MS_AI_LANG_MAX - 1] = 0;
    }
    s->stats.results_merged++;
    return rebuild_doc(s);
}

static ms_status prepare_submit(ms_ai_sub *s, const q_chunk *c, int16_t **out_s16,
                                size_t *out_n, int *out_rate, int *out_ch)
{
    const int16_t *src = (const int16_t *)c->pcm;
    size_t nbytes = c->bytes;
    int ch = c->channels > 0 ? c->channels : 1;
    size_t bpf = (size_t)ch * 2u;
    size_t nframes = bpf ? (nbytes / bpf) : 0;
    int want_ch = s->opts.target_channels;
    int want_rate = s->opts.target_rate;
    int16_t *mono = NULL;
    int16_t *rs = NULL;
    size_t mono_n = 0, rs_n = 0;
    ms_status st;
    *out_s16 = NULL;
    *out_n = 0;
    *out_rate = c->sample_rate;
    *out_ch = ch;
    if (c->pcm_bits != 16 || nframes == 0) {
        return MS_ERR_UNSUPPORTED;
    }
    if (want_ch == 1 && ch != 1) {
        mono = (int16_t *)xalloc(s, nframes * sizeof(int16_t));
        if (mono == NULL) {
            return MS_ERR_NOMEM;
        }
        st = ms_ai_pcm_s16_downmix_mono(src, ch, nframes, mono, &mono_n);
        if (st != MS_OK) {
            xfree(s, mono);
            return st;
        }
        src = mono;
        ch = 1;
        nframes = mono_n;
    }
    if (want_rate > 0 && c->sample_rate > 0 && want_rate != c->sample_rate && ch == 1) {
        /* 上限：按比例 + 2 */
        size_t cap = (size_t)(((uint64_t)nframes * (uint64_t)want_rate) /
                              (uint64_t)c->sample_rate) + 4u;
        if (cap < nframes) {
            cap = nframes + 4u;
        }
        rs = (int16_t *)xalloc(s, cap * sizeof(int16_t));
        if (rs == NULL) {
            xfree(s, mono);
            return MS_ERR_NOMEM;
        }
        st = ms_ai_pcm_s16_resample_linear(src, nframes, c->sample_rate, want_rate,
                                           rs, cap, &rs_n);
        if (st != MS_OK) {
            xfree(s, rs);
            xfree(s, mono);
            return st;
        }
        if (mono != NULL) {
            xfree(s, mono);
            mono = NULL;
        }
        src = rs;
        nframes = rs_n;
        *out_rate = want_rate;
    }
    if (src == (const int16_t *)c->pcm) {
        /* 交给插件前仍拷贝，避免队列项被释放 */
        int16_t *cp = (int16_t *)xalloc(s, nframes * sizeof(int16_t) * (size_t)ch);
        if (cp == NULL) {
            return MS_ERR_NOMEM;
        }
        memcpy(cp, src, nframes * sizeof(int16_t) * (size_t)ch);
        *out_s16 = cp;
    } else if (rs != NULL) {
        *out_s16 = rs;
    } else {
        *out_s16 = mono;
    }
    *out_n = nframes;
    *out_ch = ch;
    return MS_OK;
}

static void process_chunk(ms_ai_sub *s, q_chunk *c)
{
    int16_t *pcm = NULL;
    size_t nframes = 0;
    int rate = 0, ch = 0;
    ms_ai_pcm_chunk chunk;
    ms_status st;
    if (s->plugin.submit_pcm == NULL) {
        return;
    }
    if (s->opts.timeout_ms > 0 && c->enq_mono_us > 0) {
        int64_t age = mono_us() - c->enq_mono_us;
        if (age > (int64_t)s->opts.timeout_ms * 1000ll) {
            s->stats.results_dropped_timeout++;
            return;
        }
    }
    if (!epochs_match(s, c->session_epoch, c->timeline_epoch, c->selection_epoch)) {
        s->stats.results_dropped_epoch++;
        return;
    }
    st = prepare_submit(s, c, &pcm, &nframes, &rate, &ch);
    if (st != MS_OK) {
        s->stats.plugin_errors++;
        return;
    }
    memset(&chunk, 0, sizeof(chunk));
    chunk.struct_size = sizeof(chunk);
    chunk.samples = pcm;
    chunk.nframes = nframes;
    chunk.channels = ch;
    chunk.sample_rate = rate;
    chunk.pcm_bits = 16;
    chunk.has_pts = c->has_pts;
    chunk.pts_us = c->pts_us;
    chunk.session_epoch = c->session_epoch;
    chunk.timeline_epoch = c->timeline_epoch;
    chunk.selection_epoch = c->selection_epoch;
    st = s->plugin.submit_pcm(s->plugin.ctx, &chunk);
    xfree(s, pcm);
    if (st != MS_OK) {
        s->stats.plugin_errors++;
    }
}

static void *worker_main(void *arg)
{
    ms_ai_sub *s = (ms_ai_sub *)arg;
    pthread_mutex_lock(&s->mu);
    while (!s->stop_req) {
        if (s->q_n == 0) {
            pthread_cond_wait(&s->cv, &s->mu);
            continue;
        }
        {
            q_chunk c = s->q[s->q_head];
            s->q[s->q_head].pcm = NULL;
            s->q[s->q_head].bytes = 0;
            if (s->q_bytes >= c.bytes) {
                s->q_bytes -= c.bytes;
            } else {
                s->q_bytes = 0;
            }
            s->q_head = (s->q_head + 1u) % s->q_cap;
            s->q_n--;
            pthread_mutex_unlock(&s->mu);
            process_chunk(s, &c);
            if (c.pcm != NULL) {
                s->alloc.free_fn(s->alloc.ctx, c.pcm);
            }
            pthread_mutex_lock(&s->mu);
        }
    }
    pthread_mutex_unlock(&s->mu);
    return NULL;
}

static ms_status start_worker_locked(ms_ai_sub *s)
{
    int rc;
    if (s->worker_running) {
        return MS_OK;
    }
    s->stop_req = 0;
    rc = pthread_create(&s->worker, NULL, worker_main, s);
    if (rc != 0) {
        return MS_ERR_UNKNOWN;
    }
    s->worker_running = 1;
    s->stats.worker_running = 1;
    return MS_OK;
}

static void stop_worker_locked(ms_ai_sub *s)
{
    if (!s->worker_running) {
        return;
    }
    s->stop_req = 1;
    pthread_cond_signal(&s->cv);
    pthread_mutex_unlock(&s->mu);
    pthread_join(s->worker, NULL);
    pthread_mutex_lock(&s->mu);
    s->worker_running = 0;
    s->stats.worker_running = 0;
}

static void refresh_status(ms_ai_sub *s)
{
    if (!s->enabled) {
        set_status(s, s->configured ? "disabled" : "unconfigured");
        return;
    }
    if (!s->configured || s->plugin.submit_pcm == NULL) {
        set_status(s, "enabled-no-vendor");
        return;
    }
    if (s->opts.vendor == MS_AI_VENDOR_FAKE_LOCAL || s->stats.is_simulated) {
        set_status(s, "simulated-local-fake (not real ASR)");
        return;
    }
    set_status(s, "plugin-registered (vendor integration unverified)");
}

static ms_status start_plugin_locked(ms_ai_sub *s)
{
    ms_ai_plugin_cfg cfg;
    ms_status st;
    if (s->plugin.start == NULL) {
        return MS_ERR_STATE;
    }
    memset(&cfg, 0, sizeof(cfg));
    cfg.struct_size = sizeof(cfg);
    memcpy(cfg.source_lang, s->opts.source_lang, sizeof(cfg.source_lang));
    memcpy(cfg.target_lang, s->opts.target_lang, sizeof(cfg.target_lang));
    cfg.want_partial = s->opts.want_partial;
    cfg.want_translation = s->opts.want_translation || s->opts.target_lang[0] != 0;
    cfg.timeout_ms = s->opts.timeout_ms;
    st = s->plugin.start(s->plugin.ctx, &cfg);
    return st;
}

ms_status ms_ai_sub_create(const ms_allocator *alloc, const ms_ai_sub_opts *opts,
                           ms_ai_sub **out)
{
    ms_ai_sub *s;
    unsigned cap;
    if (out == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    *out = NULL;
    if (alloc == NULL) {
        alloc = &k_dflt;
    }
    if (alloc->alloc_fn == NULL || alloc->free_fn == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    s = (ms_ai_sub *)alloc->alloc_fn(alloc->ctx, sizeof(*s));
    if (s == NULL) {
        return MS_ERR_NOMEM;
    }
    memset(s, 0, sizeof(*s));
    s->alloc = *alloc;
    copy_opts(&s->opts, opts);
    cap = s->opts.queue_chunks_max;
    if (cap < 2u) {
        cap = 2u;
    }
    s->q = (q_chunk *)xalloc(s, (size_t)cap * sizeof(*s->q));
    if (s->q == NULL) {
        alloc->free_fn(alloc->ctx, s);
        return MS_ERR_NOMEM;
    }
    memset(s->q, 0, (size_t)cap * sizeof(*s->q));
    s->q_cap = cap;
    s->session_epoch = 1;
    s->timeline_epoch = 1;
    s->selection_epoch = 1;
    if (ms_sub_doc_create(&s->alloc, &s->doc) != MS_OK) {
        xfree(s, s->q);
        alloc->free_fn(alloc->ctx, s);
        return MS_ERR_NOMEM;
    }
    if (pthread_mutex_init(&s->mu, NULL) != 0) {
        ms_sub_doc_destroy(s->doc);
        xfree(s, s->q);
        alloc->free_fn(alloc->ctx, s);
        return MS_ERR_UNKNOWN;
    }
    if (pthread_cond_init(&s->cv, NULL) != 0) {
        pthread_mutex_destroy(&s->mu);
        ms_sub_doc_destroy(s->doc);
        xfree(s, s->q);
        alloc->free_fn(alloc->ctx, s);
        return MS_ERR_UNKNOWN;
    }
    s->mu_ok = 1;
    s->stats.struct_size = sizeof(s->stats);
    s->stats.session_epoch = 1;
    s->stats.timeline_epoch = 1;
    s->stats.selection_epoch = 1;
    set_status(s, "unconfigured");
    /* 若 opts 指定假供应商，仍须显式 use_fake_local / set_plugin，避免默认启用。 */
    *out = s;
    return MS_OK;
}

void ms_ai_sub_destroy(ms_ai_sub *s)
{
    if (s == NULL) {
        return;
    }
    if (s->mu_ok) {
        pthread_mutex_lock(&s->mu);
        s->enabled = 0;
        stop_worker_locked(s);
        if (s->plugin.stop != NULL && s->plugin.ctx != NULL) {
            s->plugin.stop(s->plugin.ctx);
        }
        if (s->plugin_owned) {
            ms_ai_plugin_vt vt = s->plugin;
            s->plugin.ctx = NULL;
            s->plugin.stop = NULL;
            s->plugin.cancel = NULL;
            pthread_mutex_unlock(&s->mu);
            ms_ai_fake_vendor_free(&vt);
            pthread_mutex_lock(&s->mu);
            memset(&s->plugin, 0, sizeof(s->plugin));
            s->plugin_owned = 0;
        } else {
            memset(&s->plugin, 0, sizeof(s->plugin));
        }
        clear_queue_locked(s);
        pthread_mutex_unlock(&s->mu);
    }
    if (s->doc != NULL) {
        ms_sub_doc_destroy(s->doc);
        s->doc = NULL;
    }
    xfree(s, s->q);
    if (s->mu_ok) {
        pthread_mutex_destroy(&s->mu);
        pthread_cond_destroy(&s->cv);
    }
    s->alloc.free_fn(s->alloc.ctx, s);
}

ms_status ms_ai_sub_configure(ms_ai_sub *s, const ms_ai_sub_opts *opts)
{
    if (s == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    pthread_mutex_lock(&s->mu);
    copy_opts(&s->opts, opts);
    if (s->enabled) {
        pthread_mutex_unlock(&s->mu);
        return MS_ERR_STATE;
    }
    pthread_mutex_unlock(&s->mu);
    return MS_OK;
}

ms_status ms_ai_sub_set_plugin(ms_ai_sub *s, const ms_ai_plugin_vt *vt)
{
    ms_ai_vendor_caps caps;
    if (s == NULL || vt == NULL || vt->submit_pcm == NULL || vt->poll_result == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    pthread_mutex_lock(&s->mu);
    if (s->enabled) {
        pthread_mutex_unlock(&s->mu);
        return MS_ERR_STATE;
    }
    if (s->plugin_owned) {
        ms_ai_plugin_vt old = s->plugin;
        pthread_mutex_unlock(&s->mu);
        ms_ai_fake_vendor_free(&old);
        pthread_mutex_lock(&s->mu);
        s->plugin_owned = 0;
    }
    s->plugin = *vt;
    s->plugin.struct_size = sizeof(s->plugin);
    s->configured = 1;
    s->stats.configured = 1;
    s->stats.is_simulated = 0;
    s->stats.vendor = s->opts.vendor;
    s->stats.vendor_id[0] = 0;
    memset(&caps, 0, sizeof(caps));
    caps.struct_size = sizeof(caps);
    if (s->plugin.query_caps != NULL &&
        s->plugin.query_caps(s->plugin.ctx, &caps) == MS_OK) {
        s->stats.is_simulated = caps.is_simulated;
        if (caps.vendor != 0) {
            s->stats.vendor = caps.vendor;
            s->opts.vendor = caps.vendor;
        }
        if (caps.id != NULL) {
            snprintf(s->stats.vendor_id, sizeof(s->stats.vendor_id), "%s", caps.id);
        }
    }
    refresh_status(s);
    pthread_mutex_unlock(&s->mu);
    return MS_OK;
}

ms_status ms_ai_sub_use_fake_local(ms_ai_sub *s)
{
    ms_ai_plugin_vt vt;
    ms_ai_fake_opts fo;
    ms_status st;
    if (s == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    ms_ai_fake_opts_default(&fo);
    fo.emit_translation = (s->opts.want_translation || s->opts.target_lang[0] != 0);
    if (s->opts.target_lang[0]) {
        snprintf(fo.target_lang, sizeof(fo.target_lang), "%s", s->opts.target_lang);
    }
    memset(&vt, 0, sizeof(vt));
    st = ms_ai_fake_vendor_init(&s->alloc, &fo, &vt);
    if (st != MS_OK) {
        return st;
    }
    st = ms_ai_sub_set_plugin(s, &vt);
    if (st != MS_OK) {
        ms_ai_fake_vendor_free(&vt);
        return st;
    }
    pthread_mutex_lock(&s->mu);
    s->plugin_owned = 1;
    s->opts.vendor = MS_AI_VENDOR_FAKE_LOCAL;
    s->stats.vendor = MS_AI_VENDOR_FAKE_LOCAL;
    s->stats.is_simulated = 1;
    snprintf(s->stats.vendor_id, sizeof(s->stats.vendor_id), "ms-ai-fake-local");
    refresh_status(s);
    pthread_mutex_unlock(&s->mu);
    return MS_OK;
}

ms_status ms_ai_sub_query_caps(const ms_ai_sub *s, ms_ai_vendor_caps *out)
{
    if (s == NULL || out == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    if (s->plugin.query_caps == NULL) {
        size_t sz = out->struct_size ? out->struct_size : sizeof(*out);
        if (sz > sizeof(*out)) {
            sz = sizeof(*out);
        }
        memset(out, 0, sz);
        out->struct_size = sz;
        out->id = "none";
        out->display = "no vendor configured";
        return MS_OK;
    }
    return s->plugin.query_caps(s->plugin.ctx, out);
}

ms_status ms_ai_sub_set_enabled(ms_ai_sub *s, int on)
{
    ms_status st = MS_OK;
    if (s == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    pthread_mutex_lock(&s->mu);
    if (!on) {
        s->enabled = 0;
        s->stats.enabled = 0;
        stop_worker_locked(s);
        if (s->plugin.cancel != NULL) {
            s->plugin.cancel(s->plugin.ctx, s->session_epoch, s->timeline_epoch,
                             s->selection_epoch);
        }
        if (s->plugin.stop != NULL) {
            s->plugin.stop(s->plugin.ctx);
        }
        clear_queue_locked(s);
        clear_live(s);
        refresh_status(s);
        pthread_mutex_unlock(&s->mu);
        return MS_OK;
    }
    if (!s->configured || s->plugin.submit_pcm == NULL) {
        pthread_mutex_unlock(&s->mu);
        return MS_ERR_STATE;
    }
    s->session_epoch++;
    if (s->session_epoch == 0) {
        s->session_epoch = 1;
    }
    s->stats.session_epoch = s->session_epoch;
    st = start_plugin_locked(s);
    if (st != MS_OK) {
        pthread_mutex_unlock(&s->mu);
        return st;
    }
    st = start_worker_locked(s);
    if (st != MS_OK) {
        if (s->plugin.stop != NULL) {
            s->plugin.stop(s->plugin.ctx);
        }
        pthread_mutex_unlock(&s->mu);
        return st;
    }
    s->enabled = 1;
    s->stats.enabled = 1;
    refresh_status(s);
    pthread_mutex_unlock(&s->mu);
    return MS_OK;
}

int ms_ai_sub_enabled(const ms_ai_sub *s)
{
    return s != NULL && s->enabled;
}

void ms_ai_sub_set_epochs(ms_ai_sub *s, uint32_t session_epoch,
                          uint32_t timeline_epoch, uint32_t selection_epoch)
{
    if (s == NULL) {
        return;
    }
    pthread_mutex_lock(&s->mu);
    if (session_epoch != 0) {
        s->session_epoch = session_epoch;
    }
    s->timeline_epoch = timeline_epoch != 0 ? timeline_epoch : s->timeline_epoch;
    s->selection_epoch = selection_epoch != 0 ? selection_epoch : s->selection_epoch;
    s->stats.session_epoch = s->session_epoch;
    s->stats.timeline_epoch = s->timeline_epoch;
    s->stats.selection_epoch = s->selection_epoch;
    pthread_mutex_unlock(&s->mu);
}

static void bump_one(uint32_t *v)
{
    (*v)++;
    if (*v == 0) {
        *v = 1;
    }
}

void ms_ai_sub_bump_timeline(ms_ai_sub *s)
{
    if (s == NULL) {
        return;
    }
    pthread_mutex_lock(&s->mu);
    bump_one(&s->timeline_epoch);
    s->stats.timeline_epoch = s->timeline_epoch;
    clear_queue_locked(s);
    clear_live(s);
    if (s->plugin.cancel != NULL) {
        s->plugin.cancel(s->plugin.ctx, s->session_epoch, s->timeline_epoch,
                         s->selection_epoch);
    }
    pthread_mutex_unlock(&s->mu);
}

void ms_ai_sub_bump_selection(ms_ai_sub *s)
{
    if (s == NULL) {
        return;
    }
    pthread_mutex_lock(&s->mu);
    bump_one(&s->selection_epoch);
    s->stats.selection_epoch = s->selection_epoch;
    clear_queue_locked(s);
    clear_live(s);
    if (s->plugin.cancel != NULL) {
        s->plugin.cancel(s->plugin.ctx, s->session_epoch, s->timeline_epoch,
                         s->selection_epoch);
    }
    pthread_mutex_unlock(&s->mu);
}

ms_status ms_ai_sub_tap_pcm(ms_ai_sub *s, const uint8_t *pcm, size_t bytes,
                            int sample_rate, int channels, int pcm_bits,
                            int has_pts, int64_t pts_us)
{
    q_chunk *slot;
    uint8_t *copy;
    unsigned tail;
    size_t maxb;
    if (s == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    if (pcm == NULL || bytes == 0) {
        return MS_OK;
    }
    pthread_mutex_lock(&s->mu);
    if (!s->enabled || !s->configured) {
        pthread_mutex_unlock(&s->mu);
        return MS_OK;
    }
    if (pcm_bits != 16 || channels <= 0 || sample_rate <= 0) {
        pthread_mutex_unlock(&s->mu);
        return MS_ERR_UNSUPPORTED;
    }
    maxb = s->opts.queue_bytes_max ? s->opts.queue_bytes_max : MS_AI_QUEUE_BYTES_DEFAULT;
    if (bytes > MS_AI_CHUNK_BYTES_MAX || bytes > maxb) {
        s->stats.queue_drops++;
        pthread_mutex_unlock(&s->mu);
        return MS_OK;
    }
    while (s->q_n >= s->q_cap || s->q_bytes + bytes > maxb) {
        if (s->q_n == 0) {
            s->stats.queue_drops++;
            pthread_mutex_unlock(&s->mu);
            return MS_OK;
        }
        drop_oldest(s);
    }
    copy = (uint8_t *)xalloc(s, bytes);
    if (copy == NULL) {
        pthread_mutex_unlock(&s->mu);
        return MS_ERR_NOMEM;
    }
    memcpy(copy, pcm, bytes);
    tail = (s->q_head + s->q_n) % s->q_cap;
    slot = &s->q[tail];
    memset(slot, 0, sizeof(*slot));
    slot->pcm = copy;
    slot->bytes = bytes;
    slot->sample_rate = sample_rate;
    slot->channels = channels;
    slot->pcm_bits = pcm_bits;
    slot->has_pts = has_pts;
    slot->pts_us = pts_us;
    slot->session_epoch = s->session_epoch;
    slot->timeline_epoch = s->timeline_epoch;
    slot->selection_epoch = s->selection_epoch;
    slot->enq_mono_us = mono_us();
    s->q_n++;
    s->q_bytes += bytes;
    s->stats.pcm_taps++;
    s->stats.pcm_bytes += bytes;
    {
        size_t bpf = (size_t)channels * 2u;
        size_t nf = bpf ? (bytes / bpf) : 0;
        const int16_t *p16 = (const int16_t *)pcm;
        s->stats.pcm_frames += nf;
        s->stats.last_tap_has_pts = has_pts;
        s->stats.last_tap_pts_us = pts_us;
        s->stats.last_tap_rate = sample_rate;
        s->stats.last_tap_ch = channels;
        s->stats.last_tap_nframes = nf;
        if (nf > 0) {
            s->stats.last_tap_first = p16[0];
            s->stats.last_tap_last = p16[(nf - 1u) * (size_t)channels];
        }
    }
    pthread_cond_signal(&s->cv);
    pthread_mutex_unlock(&s->mu);
    return MS_OK;
}

ms_status ms_ai_sub_drive(ms_ai_sub *s)
{
    int spins;
    if (s == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    pthread_mutex_lock(&s->mu);
    if (!s->enabled || s->plugin.poll_result == NULL) {
        pthread_mutex_unlock(&s->mu);
        return MS_OK;
    }
    for (spins = 0; spins < 16; spins++) {
        ms_ai_result r;
        ms_status st;
        memset(&r, 0, sizeof(r));
        r.struct_size = sizeof(r);
        /* poll 在锁内：假供应商无阻塞。实接插件不得在 poll 中做网络。 */
        st = s->plugin.poll_result(s->plugin.ctx, &r);
        if (st == MS_AGAIN) {
            break;
        }
        if (st != MS_OK) {
            s->stats.plugin_errors++;
            break;
        }
        st = merge_result(s, &r);
        if (st != MS_OK) {
            pthread_mutex_unlock(&s->mu);
            return st;
        }
    }
    pthread_mutex_unlock(&s->mu);
    return MS_OK;
}

void ms_ai_sub_flush(ms_ai_sub *s)
{
    if (s == NULL) {
        return;
    }
    pthread_mutex_lock(&s->mu);
    clear_queue_locked(s);
    clear_live(s);
    if (s->plugin.cancel != NULL) {
        s->plugin.cancel(s->plugin.ctx, s->session_epoch, s->timeline_epoch,
                         s->selection_epoch);
    }
    pthread_mutex_unlock(&s->mu);
}

ms_sub_doc *ms_ai_sub_doc(ms_ai_sub *s)
{
    return s != NULL ? s->doc : NULL;
}

ms_status ms_ai_sub_get_stats(const ms_ai_sub *s, ms_ai_sub_stats *out)
{
    size_t sz;
    if (s == NULL || out == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    sz = out->struct_size ? out->struct_size : sizeof(*out);
    if (sz > sizeof(*out)) {
        sz = sizeof(*out);
    }
    /* const 下取锁：结构体 mu 逻辑上可变，这里用去除 const 的锁。 */
    pthread_mutex_lock((pthread_mutex_t *)&s->mu);
    memcpy(out, &s->stats, sz);
    out->struct_size = sz;
    if (sz >= offsetof(ms_ai_sub_stats, status) + sizeof(out->status)) {
        memcpy(out->status, s->status, sizeof(out->status));
    }
    pthread_mutex_unlock((pthread_mutex_t *)&s->mu);
    return MS_OK;
}

const char *ms_ai_sub_status_text(const ms_ai_sub *s)
{
    if (s == NULL) {
        return "none";
    }
    return s->status;
}
