/*
 * 见 ms_hw_mediandk.h。不把 libmediandk 写成 DT_NEEDED。
 * by: Long <mycjzb@gmail.com>
 */
#ifdef __ANDROID__

#include <dlfcn.h>
#include <pthread.h>
#include <stddef.h>

/* 先拿真实原型，不要用包装宏。 */
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#include <media/NdkMediaError.h>

#include "ms_hw_api.h"

typedef AMediaCodec *(*fn_create_by_name)(const char *);
typedef media_status_t (*fn_configure)(AMediaCodec *, const AMediaFormat *,
                                      ANativeWindow *, AMediaCrypto *, uint32_t);
typedef media_status_t (*fn_start)(AMediaCodec *);
typedef media_status_t (*fn_stop)(AMediaCodec *);
typedef media_status_t (*fn_flush)(AMediaCodec *);
typedef media_status_t (*fn_delete)(AMediaCodec *);
typedef ssize_t (*fn_dq_in)(AMediaCodec *, int64_t);
typedef ssize_t (*fn_dq_out)(AMediaCodec *, AMediaCodecBufferInfo *, int64_t);
typedef uint8_t *(*fn_get_in)(AMediaCodec *, size_t, size_t *);
typedef uint8_t *(*fn_get_out)(AMediaCodec *, size_t, size_t *);
typedef AMediaFormat *(*fn_get_of)(AMediaCodec *);
typedef media_status_t (*fn_q_in)(AMediaCodec *, size_t, long, size_t, uint64_t, uint32_t);
typedef media_status_t (*fn_rel_out)(AMediaCodec *, size_t, bool);
typedef AMediaFormat *(*fn_fmt_new)(void);
typedef media_status_t (*fn_fmt_del)(AMediaFormat *);
typedef void (*fn_fmt_sstr)(AMediaFormat *, const char *, const char *);
typedef void (*fn_fmt_si32)(AMediaFormat *, const char *, int32_t);
typedef void (*fn_fmt_sbuf)(AMediaFormat *, const char *, const void *, size_t);
typedef bool (*fn_fmt_gi32)(AMediaFormat *, const char *, int32_t *);

static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static void *g_h;
static int g_ok;
static fn_create_by_name g_create;
static fn_configure g_configure;
static fn_start g_start;
static fn_stop g_stop;
static fn_flush g_flush;
static fn_delete g_delete;
static fn_dq_in g_dq_in;
static fn_dq_out g_dq_out;
static fn_get_in g_get_in;
static fn_get_out g_get_out;
static fn_get_of g_get_of;
static fn_q_in g_q_in;
static fn_rel_out g_rel_out;
static fn_fmt_new g_fmt_new;
static fn_fmt_del g_fmt_del;
static fn_fmt_sstr g_fmt_sstr;
static fn_fmt_si32 g_fmt_si32;
static fn_fmt_sbuf g_fmt_sbuf;
static fn_fmt_gi32 g_fmt_gi32;

#define LOAD(ptr, typ, name) do { (ptr) = (typ)dlsym(g_h, (name)); } while (0)

static void mediandk_init(void)
{
    int sdk;
    g_ok = 0;
    g_h = NULL;
    sdk = ms_hw_device_sdk_int();
    /* API 18–20 没有该库，不必 dlopen。 */
    if (sdk > 0 && sdk < 21) {
        return;
    }
    g_h = dlopen("libmediandk.so", RTLD_NOW | RTLD_LOCAL);
    if (g_h == NULL) {
        return;
    }
    LOAD(g_create, fn_create_by_name, "AMediaCodec_createCodecByName");
    LOAD(g_configure, fn_configure, "AMediaCodec_configure");
    LOAD(g_start, fn_start, "AMediaCodec_start");
    LOAD(g_stop, fn_stop, "AMediaCodec_stop");
    LOAD(g_flush, fn_flush, "AMediaCodec_flush");
    LOAD(g_delete, fn_delete, "AMediaCodec_delete");
    LOAD(g_dq_in, fn_dq_in, "AMediaCodec_dequeueInputBuffer");
    LOAD(g_dq_out, fn_dq_out, "AMediaCodec_dequeueOutputBuffer");
    LOAD(g_get_in, fn_get_in, "AMediaCodec_getInputBuffer");
    LOAD(g_get_out, fn_get_out, "AMediaCodec_getOutputBuffer");
    LOAD(g_get_of, fn_get_of, "AMediaCodec_getOutputFormat");
    LOAD(g_q_in, fn_q_in, "AMediaCodec_queueInputBuffer");
    LOAD(g_rel_out, fn_rel_out, "AMediaCodec_releaseOutputBuffer");
    LOAD(g_fmt_new, fn_fmt_new, "AMediaFormat_new");
    LOAD(g_fmt_del, fn_fmt_del, "AMediaFormat_delete");
    LOAD(g_fmt_sstr, fn_fmt_sstr, "AMediaFormat_setString");
    LOAD(g_fmt_si32, fn_fmt_si32, "AMediaFormat_setInt32");
    LOAD(g_fmt_sbuf, fn_fmt_sbuf, "AMediaFormat_setBuffer");
    LOAD(g_fmt_gi32, fn_fmt_gi32, "AMediaFormat_getInt32");
    if (g_create == NULL || g_configure == NULL || g_start == NULL ||
        g_delete == NULL || g_dq_in == NULL || g_dq_out == NULL ||
        g_get_in == NULL || g_get_out == NULL || g_q_in == NULL ||
        g_rel_out == NULL || g_fmt_new == NULL || g_fmt_del == NULL ||
        g_fmt_sstr == NULL || g_fmt_si32 == NULL || g_fmt_sbuf == NULL) {
        return;
    }
    g_ok = 1;
}

int ms_hw_mediandk_available(void)
{
    (void)pthread_once(&g_once, mediandk_init);
    return g_ok;
}

static int ready(void)
{
    return ms_hw_mediandk_available();
}

AMediaCodec *ms_hw_AMediaCodec_createCodecByName(const char *name)
{
    if (!ready() || name == NULL) {
        return NULL;
    }
    return g_create(name);
}

media_status_t ms_hw_AMediaCodec_configure(AMediaCodec *codec, const AMediaFormat *fmt,
                                          ANativeWindow *window, AMediaCrypto *crypto,
                                          uint32_t flags)
{
    if (!ready() || codec == NULL) {
        return AMEDIA_ERROR_UNKNOWN;
    }
    return g_configure(codec, fmt, window, crypto, flags);
}

media_status_t ms_hw_AMediaCodec_start(AMediaCodec *codec)
{
    if (!ready() || codec == NULL || g_start == NULL) {
        return AMEDIA_ERROR_UNKNOWN;
    }
    return g_start(codec);
}

media_status_t ms_hw_AMediaCodec_stop(AMediaCodec *codec)
{
    if (!ready() || codec == NULL || g_stop == NULL) {
        return AMEDIA_ERROR_UNKNOWN;
    }
    return g_stop(codec);
}

media_status_t ms_hw_AMediaCodec_flush(AMediaCodec *codec)
{
    if (!ready() || codec == NULL || g_flush == NULL) {
        return AMEDIA_ERROR_UNKNOWN;
    }
    return g_flush(codec);
}

media_status_t ms_hw_AMediaCodec_delete(AMediaCodec *codec)
{
    if (!ready() || codec == NULL || g_delete == NULL) {
        return AMEDIA_ERROR_UNKNOWN;
    }
    return g_delete(codec);
}

ssize_t ms_hw_AMediaCodec_dequeueInputBuffer(AMediaCodec *codec, int64_t timeoutUs)
{
    if (!ready() || codec == NULL) {
        return (ssize_t)AMEDIACODEC_INFO_TRY_AGAIN_LATER;
    }
    return g_dq_in(codec, timeoutUs);
}

ssize_t ms_hw_AMediaCodec_dequeueOutputBuffer(AMediaCodec *codec, AMediaCodecBufferInfo *info,
                                              int64_t timeoutUs)
{
    if (!ready() || codec == NULL) {
        return (ssize_t)AMEDIACODEC_INFO_TRY_AGAIN_LATER;
    }
    return g_dq_out(codec, info, timeoutUs);
}

uint8_t *ms_hw_AMediaCodec_getInputBuffer(AMediaCodec *codec, size_t idx, size_t *out_size)
{
    if (!ready() || codec == NULL) {
        return NULL;
    }
    return g_get_in(codec, idx, out_size);
}

uint8_t *ms_hw_AMediaCodec_getOutputBuffer(AMediaCodec *codec, size_t idx, size_t *out_size)
{
    if (!ready() || codec == NULL) {
        return NULL;
    }
    return g_get_out(codec, idx, out_size);
}

AMediaFormat *ms_hw_AMediaCodec_getOutputFormat(AMediaCodec *codec)
{
    if (!ready() || codec == NULL || g_get_of == NULL) {
        return NULL;
    }
    return g_get_of(codec);
}

media_status_t ms_hw_AMediaCodec_queueInputBuffer(AMediaCodec *codec, size_t idx,
                                                 off_t offset, size_t size,
                                                 uint64_t time, uint32_t flags)
{
    if (!ready() || codec == NULL) {
        return AMEDIA_ERROR_UNKNOWN;
    }
    return g_q_in(codec, idx, (long)offset, size, time, flags);
}

media_status_t ms_hw_AMediaCodec_releaseOutputBuffer(AMediaCodec *codec, size_t idx,
                                                     bool render)
{
    if (!ready() || codec == NULL) {
        return AMEDIA_ERROR_UNKNOWN;
    }
    return g_rel_out(codec, idx, render);
}

AMediaFormat *ms_hw_AMediaFormat_new(void)
{
    if (!ready()) {
        return NULL;
    }
    return g_fmt_new();
}

media_status_t ms_hw_AMediaFormat_delete(AMediaFormat *fmt)
{
    if (!ready() || fmt == NULL || g_fmt_del == NULL) {
        return AMEDIA_ERROR_UNKNOWN;
    }
    return g_fmt_del(fmt);
}

void ms_hw_AMediaFormat_setString(AMediaFormat *fmt, const char *name, const char *value)
{
    if (!ready() || fmt == NULL || g_fmt_sstr == NULL) {
        return;
    }
    g_fmt_sstr(fmt, name, value);
}

void ms_hw_AMediaFormat_setInt32(AMediaFormat *fmt, const char *name, int32_t value)
{
    if (!ready() || fmt == NULL || g_fmt_si32 == NULL) {
        return;
    }
    g_fmt_si32(fmt, name, value);
}

void ms_hw_AMediaFormat_setBuffer(AMediaFormat *fmt, const char *name, const void *data, size_t size)
{
    if (!ready() || fmt == NULL || g_fmt_sbuf == NULL) {
        return;
    }
    g_fmt_sbuf(fmt, name, data, size);
}

bool ms_hw_AMediaFormat_getInt32(AMediaFormat *fmt, const char *name, int32_t *out)
{
    if (!ready() || fmt == NULL || g_fmt_gi32 == NULL) {
        return 0;
    }
    return g_fmt_gi32(fmt, name, out);
}

#endif /* __ANDROID__ */
