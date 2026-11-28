/*
 * MediaCodec JNI 薄桥：缓存 jclass/jmethodID，不使用反射。
 * 选择策略见 ms_hw_classify_codec_name（参考公开 Media3 启发式，非 Media3 等价实现）。
 * 日志不得包含 URL。
 * by: Long <mycjzb@gmail.com>
 */
#ifdef __ANDROID__

#include "ms_hw_jni.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <android/log.h>
#include <pthread.h>

static JavaVM *g_vm;
static int g_ready;

static jclass g_list;
static jclass g_info;
static jmethodID g_get_count;
static jmethodID g_get_at;
static jmethodID g_get_name;
static jmethodID g_is_enc;
static jmethodID g_get_types;
#ifdef MS_HW_HAS_API29_IDS
#endif
static jmethodID g_is_hw;   /* API 29；解析失败则为 NULL */
static jmethodID g_is_sw;
static jmethodID g_is_alias;

#if 1 /* JNI MediaCodec I/O 始终编入；按 SDK 由 dispatch 选择 */
static jclass g_codec;
static jclass g_format;
static jclass g_bufinfo;
static jmethodID g_create_by_name;
static jmethodID g_configure;
static jmethodID g_start;
static jmethodID g_stop;
static jmethodID g_flush;
static jmethodID g_release;
static jmethodID g_dq_in;
static jmethodID g_q_in;
static jmethodID g_dq_out;
static jmethodID g_rel_out;
static jmethodID g_get_in_bufs;
static jmethodID g_get_out_bufs;
static jmethodID g_fmt_create;
static jmethodID g_fmt_set_str;
static jmethodID g_fmt_set_int;
static jmethodID g_fmt_set_buf;
static jfieldID g_bi_off, g_bi_size, g_bi_pts, g_bi_flags;
#endif

int ms_hw_jni_has_vm(void)
{
    return g_vm != NULL && g_ready;
}

int ms_hw_jni_onload(JavaVM *vm)
{
    JNIEnv *env = NULL;
    jclass c;

    g_vm = vm;
    g_ready = 0;
    if ((*vm)->GetEnv(vm, (void **)&env, JNI_VERSION_1_6) != JNI_OK)
        return -1;

    c = (*env)->FindClass(env, "android/media/MediaCodecList");
    if (!c)
        return -1;
    g_list = (*env)->NewGlobalRef(env, c);
    (*env)->DeleteLocalRef(env, c);
    c = (*env)->FindClass(env, "android/media/MediaCodecInfo");
    if (!c)
        return -1;
    g_info = (*env)->NewGlobalRef(env, c);
    (*env)->DeleteLocalRef(env, c);

    g_get_count = (*env)->GetStaticMethodID(env, g_list, "getCodecCount", "()I");
    g_get_at = (*env)->GetStaticMethodID(env, g_list, "getCodecInfoAt",
                                         "(I)Landroid/media/MediaCodecInfo;");
    g_get_name = (*env)->GetMethodID(env, g_info, "getName", "()Ljava/lang/String;");
    g_is_enc = (*env)->GetMethodID(env, g_info, "isEncoder", "()Z");
    g_get_types = (*env)->GetMethodID(env, g_info, "getSupportedTypes",
                                      "()[Ljava/lang/String;");
    if (!g_get_count || !g_get_at || !g_get_name || !g_is_enc || !g_get_types)
        return -1;

    /* API 29 方法：旧设备上 GetMethodID 失败是预期，清除异常后继续。 */
    g_is_hw = (*env)->GetMethodID(env, g_info, "isHardwareAccelerated", "()Z");
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        g_is_hw = NULL;
    }
    g_is_sw = (*env)->GetMethodID(env, g_info, "isSoftwareOnly", "()Z");
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        g_is_sw = NULL;
    }
    g_is_alias = (*env)->GetMethodID(env, g_info, "isAlias", "()Z");
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        g_is_alias = NULL;
    }

#if 1 /* JNI MediaCodec I/O 始终编入；按 SDK 由 dispatch 选择 */
    {
        jclass bi;
        c = (*env)->FindClass(env, "android/media/MediaCodec");
        if (!c)
            return -1;
        g_codec = (*env)->NewGlobalRef(env, c);
        (*env)->DeleteLocalRef(env, c);
        c = (*env)->FindClass(env, "android/media/MediaFormat");
        if (!c)
            return -1;
        g_format = (*env)->NewGlobalRef(env, c);
        (*env)->DeleteLocalRef(env, c);
        c = (*env)->FindClass(env, "android/media/MediaCodec$BufferInfo");
        if (!c)
            return -1;
        g_bufinfo = (*env)->NewGlobalRef(env, c);
        (*env)->DeleteLocalRef(env, c);

        g_create_by_name = (*env)->GetStaticMethodID(
            env, g_codec, "createByCodecName",
            "(Ljava/lang/String;)Landroid/media/MediaCodec;");
        g_configure = (*env)->GetMethodID(
            env, g_codec, "configure",
            "(Landroid/media/MediaFormat;Landroid/view/Surface;Landroid/media/MediaCrypto;I)V");
        g_start = (*env)->GetMethodID(env, g_codec, "start", "()V");
        g_stop = (*env)->GetMethodID(env, g_codec, "stop", "()V");
        g_flush = (*env)->GetMethodID(env, g_codec, "flush", "()V");
        g_release = (*env)->GetMethodID(env, g_codec, "release", "()V");
        g_dq_in = (*env)->GetMethodID(env, g_codec, "dequeueInputBuffer", "(J)I");
        /* queueInputBuffer(int index, int offset, int size, long ptsUs, int flags) */
        g_q_in = (*env)->GetMethodID(env, g_codec, "queueInputBuffer", "(IIIJI)V");
        g_dq_out = (*env)->GetMethodID(
            env, g_codec, "dequeueOutputBuffer",
            "(Landroid/media/MediaCodec$BufferInfo;J)I");
        g_rel_out = (*env)->GetMethodID(env, g_codec, "releaseOutputBuffer", "(IZ)V");
        /* getInputBuffers 在 API 21 弃用，仍应存在；失败则清异常，勿让 pending exception 崩 JNI_OnLoad。
         * API 21+ NDK 路径不依赖这两个方法。 */
        g_get_in_bufs = (*env)->GetMethodID(env, g_codec, "getInputBuffers",
                                            "()[Ljava/nio/ByteBuffer;");
        if (!g_get_in_bufs && (*env)->ExceptionCheck(env)) {
            (*env)->ExceptionClear(env);
        }
        g_get_out_bufs = (*env)->GetMethodID(env, g_codec, "getOutputBuffers",
                                             "()[Ljava/nio/ByteBuffer;");
        if (!g_get_out_bufs && (*env)->ExceptionCheck(env)) {
            (*env)->ExceptionClear(env);
        }
        g_fmt_create = (*env)->GetStaticMethodID(
            env, g_format, "createVideoFormat",
            "(Ljava/lang/String;II)Landroid/media/MediaFormat;");
        g_fmt_set_str = (*env)->GetMethodID(
            env, g_format, "setString", "(Ljava/lang/String;Ljava/lang/String;)V");
        (void)g_fmt_set_str;
        g_fmt_set_int = (*env)->GetMethodID(env, g_format, "setInteger",
                                            "(Ljava/lang/String;I)V");
        (void)g_fmt_set_int;
        g_fmt_set_buf = (*env)->GetMethodID(
            env, g_format, "setByteBuffer",
            "(Ljava/lang/String;Ljava/nio/ByteBuffer;)V");
        bi = g_bufinfo;
        g_bi_off = (*env)->GetFieldID(env, bi, "offset", "I");
        g_bi_size = (*env)->GetFieldID(env, bi, "size", "I");
        g_bi_pts = (*env)->GetFieldID(env, bi, "presentationTimeUs", "J");
        g_bi_flags = (*env)->GetFieldID(env, bi, "flags", "I");
        if ((*env)->ExceptionCheck(env)) {
            (*env)->ExceptionClear(env);
        }
        /* getInputBuffers 不是 OnLoad 硬条件：API 21+ 走 NDK，缺了也不该让库加载失败。 */
        if (!g_create_by_name || !g_configure || !g_start || !g_stop || !g_flush ||
            !g_release || !g_dq_in || !g_q_in || !g_dq_out || !g_rel_out ||
            !g_fmt_create || !g_fmt_set_buf || !g_bi_size)
            return -1;
    }
#endif
    g_ready = 1;
    return 0;
}

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *reserved)
{
    (void)reserved;
    if (ms_hw_jni_onload(vm) != 0)
        return JNI_ERR;
    return JNI_VERSION_1_6;
}

JNIEXPORT void JNICALL JNI_OnUnload(JavaVM *vm, void *reserved)
{
    (void)vm;
    (void)reserved;
    g_ready = 0;
}

/* 由本库 Attach 的原生线程在退出时必须 Detach：Dalvik(API≤19) 发现已附着线程退出会
 * dvmAbort(SIGSEGV 0xdeadd00d)，ART 也会告警/中止。用 pthread key 析构自动 Detach；
 * Dalvik 的 threadExitCheck 会多轮让其它析构先跑，所以这里一定来得及。
 * by: Long <mycjzb@gmail.com> */
static pthread_key_t g_detach_key;
static pthread_once_t g_detach_once = PTHREAD_ONCE_INIT;
static int g_detach_ok;

static void detach_dtor(void *p)
{
    (void)p;
    if (g_vm)
        (void)(*g_vm)->DetachCurrentThread(g_vm);
}

static void detach_key_init(void)
{
    g_detach_ok = pthread_key_create(&g_detach_key, detach_dtor) == 0;
}

int ms_hw_jni_env(JNIEnv **env)
{
    jint att;

    if (!g_vm || !env)
        return 0;
    *env = NULL;
    if ((*g_vm)->GetEnv(g_vm, (void **)env, JNI_VERSION_1_6) == JNI_OK)
        return 1;
    att = (*g_vm)->AttachCurrentThread(g_vm, env, NULL);
    if (att != JNI_OK || *env == NULL)
        return 0;
    (void)pthread_once(&g_detach_once, detach_key_init);
    if (g_detach_ok)
        (void)pthread_setspecific(g_detach_key, (void *)1);
    return 1;
}

static int type_has_mime(JNIEnv *env, jobjectArray types, const char *mime)
{
    jsize n, i;
    if (!types || !mime)
        return 0;
    n = (*env)->GetArrayLength(env, types);
    for (i = 0; i < n; i++) {
        jstring s = (jstring)(*env)->GetObjectArrayElement(env, types, i);
        const char *p;
        int ok = 0;
        if (!s)
            continue;
        p = (*env)->GetStringUTFChars(env, s, NULL);
        if (p && ms_hw_ascii_ieq(p, mime))
            ok = 1;
        if (p)
            (*env)->ReleaseStringUTFChars(env, s, p);
        (*env)->DeleteLocalRef(env, s);
        if (ok)
            return 1;
    }
    return 0;
}

static ms_status pick_decoder_mime(const char *mime, const char *label,
                                   int api_level, int allow_sw, int system_sw_only,
                                   char *name, size_t name_cap,
                                   ms_hw_kind *kind, int *api_out,
                                   char *detail, size_t detail_cap)
{
    JNIEnv *env = NULL;
    jint count, i;
    char first_sw[128];
    int have_sw = 0;

    if (name && name_cap)
        name[0] = 0;
    if (kind)
        *kind = MS_HW_KIND_NONE;
    if (api_out)
        *api_out = api_level;
    first_sw[0] = 0;

    if (!ms_hw_jni_has_vm() || !ms_hw_jni_env(&env)) {
        ms_hw_set_detail_buf(detail, detail_cap,
                             "MediaCodecList requires a JVM; listing not available");
        return MS_ERR_UNSUPPORTED;
    }

    count = (*env)->CallStaticIntMethod(env, g_list, g_get_count);
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        ms_hw_set_detail_buf(detail, detail_cap, "MediaCodecList.getCodecCount failed");
        return MS_ERR_UNKNOWN;
    }

    for (i = 0; i < count; i++) {
        jobject info;
        jboolean enc;
        jobjectArray types;
        jstring jn;
        const char *nm;
        int hw_flag = -1, sw_flag = -1;
        ms_hw_kind k;

        info = (*env)->CallStaticObjectMethod(env, g_list, g_get_at, i);
        if ((*env)->ExceptionCheck(env) || !info) {
            (*env)->ExceptionClear(env);
            continue;
        }
        enc = (*env)->CallBooleanMethod(env, info, g_is_enc);
        if (enc) {
            (*env)->DeleteLocalRef(env, info);
            continue;
        }
        if (g_is_alias && api_level >= 29) {
            jboolean alias = (*env)->CallBooleanMethod(env, info, g_is_alias);
            if ((*env)->ExceptionCheck(env)) {
                (*env)->ExceptionClear(env);
            } else if (alias) {
                (*env)->DeleteLocalRef(env, info);
                continue;
            }
        }
        types = (jobjectArray)(*env)->CallObjectMethod(env, info, g_get_types);
        if (!type_has_mime(env, types, mime)) {
            if (types)
                (*env)->DeleteLocalRef(env, types);
            (*env)->DeleteLocalRef(env, info);
            continue;
        }
        if (types)
            (*env)->DeleteLocalRef(env, types);

        if (g_is_hw && g_is_sw && api_level >= 29) {
            jboolean hw = (*env)->CallBooleanMethod(env, info, g_is_hw);
            jboolean sw = (*env)->CallBooleanMethod(env, info, g_is_sw);
            if (!(*env)->ExceptionCheck(env)) {
                hw_flag = hw ? 1 : 0;
                sw_flag = sw ? 1 : 0;
            } else {
                (*env)->ExceptionClear(env);
            }
        }

        jn = (jstring)(*env)->CallObjectMethod(env, info, g_get_name);
        nm = jn ? (*env)->GetStringUTFChars(env, jn, NULL) : NULL;
        k = nm ? ms_hw_classify_codec_name(nm, api_level, hw_flag, sw_flag)
               : MS_HW_KIND_SOFTWARE;
        if (nm && k == MS_HW_KIND_HARDWARE) {
            if (name && name_cap) {
                size_t n = strlen(nm);
                if (n >= name_cap)
                    n = name_cap - 1u;
                memcpy(name, nm, n);
                name[n] = 0;
            }
            if (kind)
                *kind = MS_HW_KIND_HARDWARE;
            if (jn) {
                (*env)->ReleaseStringUTFChars(env, jn, nm);
                (*env)->DeleteLocalRef(env, jn);
            }
            (*env)->DeleteLocalRef(env, info);
            ms_hw_set_detail_buf(detail, detail_cap,
                                 "selected hardware decoder by MediaCodecList");
            return MS_OK;
        }
        if (nm && k == MS_HW_KIND_SOFTWARE && !have_sw) {
            if (system_sw_only && !ms_hw_is_system_software_codec_name(nm)) {
                if (jn && nm)
                    (*env)->ReleaseStringUTFChars(env, jn, nm);
                if (jn)
                    (*env)->DeleteLocalRef(env, jn);
                (*env)->DeleteLocalRef(env, info);
                continue;
            }
            {
                size_t n = strlen(nm);
                if (n >= sizeof(first_sw))
                    n = sizeof(first_sw) - 1u;
                memcpy(first_sw, nm, n);
                first_sw[n] = 0;
                have_sw = 1;
            }
        }
        if (jn && nm)
            (*env)->ReleaseStringUTFChars(env, jn, nm);
        if (jn)
            (*env)->DeleteLocalRef(env, jn);
        (*env)->DeleteLocalRef(env, info);
    }

    if (have_sw && (allow_sw || system_sw_only)) {
        if (name && name_cap) {
            size_t n = strlen(first_sw);
            if (n >= name_cap)
                n = name_cap - 1u;
            memcpy(name, first_sw, n);
            name[n] = 0;
        }
        if (kind)
            *kind = MS_HW_KIND_SOFTWARE;
        if (system_sw_only && !allow_sw) {
            ms_hw_set_detail_buf(detail, detail_cap, "系统 AAC，非硬件");
        } else {
            ms_hw_set_detail_buf(detail, detail_cap,
                                 "only software decoder available; allowed by option");
        }
        return MS_OK;
    }
    if (have_sw) {
        ms_hw_set_detail_buf(detail, detail_cap,
                             "refused software decoder (c2.android/omx.google or isSoftwareOnly)");
        return MS_ERR_UNSUPPORTED;
    }
    ms_hw_set_detail_buf(detail, detail_cap, "no matching decoder in MediaCodecList");
    return MS_ERR_UNSUPPORTED;
}

/* AVS/AVS+：遍历 MediaCodecList，只收集类型或名称含 avs 的解码器（最多 32 个），
 * 交给纯 C 策略 ms_hw_avs_pick_from_list 挑选；mime_out 为所选组件实际声明的 MIME。 */
#define MS_JNI_AVS_MAX 32
typedef struct ms_jni_avs_store {
    char name[MS_JNI_AVS_MAX][128];
    char types[MS_JNI_AVS_MAX][MS_HW_AVS_MAX_TYPES][64];
    ms_hw_codec_desc desc[MS_JNI_AVS_MAX];
} ms_jni_avs_store;

ms_status ms_hw_jni_pick_avs(int api_level, int allow_sw,
                             char *name, size_t name_cap,
                             char *mime_out, size_t mime_cap,
                             ms_hw_kind *kind, int *api_out,
                             char *detail, size_t detail_cap)
{
    JNIEnv *env = NULL;
    jint count, i;
    size_t n = 0;
    ms_jni_avs_store *st;
    ms_status rc;

    if (name && name_cap)
        name[0] = 0;
    if (mime_out && mime_cap)
        mime_out[0] = 0;
    if (kind)
        *kind = MS_HW_KIND_NONE;
    if (api_out)
        *api_out = api_level;
    if (!ms_hw_jni_has_vm() || !ms_hw_jni_env(&env)) {
        ms_hw_set_detail_buf(detail, detail_cap,
                             "MediaCodecList requires a JVM; listing not available");
        return MS_ERR_UNSUPPORTED;
    }
    st = (ms_jni_avs_store *)calloc(1, sizeof(*st));
    if (st == NULL)
        return MS_ERR_NOMEM;
    count = (*env)->CallStaticIntMethod(env, g_list, g_get_count);
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        free(st);
        ms_hw_set_detail_buf(detail, detail_cap, "MediaCodecList.getCodecCount failed");
        return MS_ERR_UNKNOWN;
    }
    for (i = 0; i < count && n < MS_JNI_AVS_MAX; i++) {
        jobject info;
        jobjectArray types;
        jstring jn;
        const char *nm;
        jsize nt, j;
        int any_avs = 0;
        ms_hw_codec_desc *d = &st->desc[n];

        info = (*env)->CallStaticObjectMethod(env, g_list, g_get_at, i);
        if ((*env)->ExceptionCheck(env) || !info) {
            (*env)->ExceptionClear(env);
            continue;
        }
        if ((*env)->CallBooleanMethod(env, info, g_is_enc)) {
            (*env)->DeleteLocalRef(env, info);
            continue;
        }
        memset(d, 0, sizeof(*d));
        d->hw_flag = -1;
        d->sw_flag = -1;
        if (g_is_alias && api_level >= 29) {
            jboolean alias = (*env)->CallBooleanMethod(env, info, g_is_alias);
            if ((*env)->ExceptionCheck(env))
                (*env)->ExceptionClear(env);
            else
                d->is_alias = alias ? 1 : 0;
        }
        if (g_is_hw && g_is_sw && api_level >= 29) {
            jboolean hw = (*env)->CallBooleanMethod(env, info, g_is_hw);
            jboolean sw = (*env)->CallBooleanMethod(env, info, g_is_sw);
            if (!(*env)->ExceptionCheck(env)) {
                d->hw_flag = hw ? 1 : 0;
                d->sw_flag = sw ? 1 : 0;
            } else {
                (*env)->ExceptionClear(env);
            }
        }
        jn = (jstring)(*env)->CallObjectMethod(env, info, g_get_name);
        nm = jn ? (*env)->GetStringUTFChars(env, jn, NULL) : NULL;
        if (nm) {
            snprintf(st->name[n], sizeof(st->name[n]), "%s", nm);
            if (ms_hw_ascii_icontains(nm, "avs"))
                any_avs = 1;
            (*env)->ReleaseStringUTFChars(env, jn, nm);
        }
        if (jn)
            (*env)->DeleteLocalRef(env, jn);
        types = (jobjectArray)(*env)->CallObjectMethod(env, info, g_get_types);
        if ((*env)->ExceptionCheck(env)) {
            (*env)->ExceptionClear(env);
            types = NULL;
        }
        nt = types ? (*env)->GetArrayLength(env, types) : 0;
        for (j = 0; j < nt && d->ntypes < MS_HW_AVS_MAX_TYPES; j++) {
            jstring s = (jstring)(*env)->GetObjectArrayElement(env, types, j);
            const char *c = s ? (*env)->GetStringUTFChars(env, s, NULL) : NULL;
            if (c) {
                snprintf(st->types[n][d->ntypes], sizeof(st->types[n][0]), "%s", c);
                if (ms_hw_ascii_icontains(c, "avs"))
                    any_avs = 1;
                d->types[d->ntypes] = st->types[n][d->ntypes];
                d->ntypes++;
                (*env)->ReleaseStringUTFChars(env, s, c);
            }
            if (s)
                (*env)->DeleteLocalRef(env, s);
        }
        if (types)
            (*env)->DeleteLocalRef(env, types);
        (*env)->DeleteLocalRef(env, info);
        if (!any_avs || st->name[n][0] == 0)
            continue;
        d->name = st->name[n];
        n++;
    }
    rc = ms_hw_avs_pick_from_list(st->desc, n, api_level, allow_sw, name, name_cap,
                                  mime_out, mime_cap, kind, detail, detail_cap);
    free(st);
    return rc;
}

ms_status ms_hw_jni_pick_avc(int api_level, int allow_sw,
                             char *name, size_t name_cap,
                             ms_hw_kind *kind, int *api_out,
                             char *detail, size_t detail_cap)
{
    return pick_decoder_mime("video/avc", "AVC", api_level, allow_sw, 0, name, name_cap,
                             kind, api_out, detail, detail_cap);
}

ms_status ms_hw_jni_pick_hevc(int api_level, int allow_sw,
                              char *name, size_t name_cap,
                              ms_hw_kind *kind, int *api_out,
                              char *detail, size_t detail_cap)
{
    return pick_decoder_mime("video/hevc", "HEVC", api_level, allow_sw, 0, name, name_cap,
                             kind, api_out, detail, detail_cap);
}

ms_status ms_hw_jni_pick_video(ms_codec codec, int api_level, int allow_sw,
                               char *name, size_t name_cap,
                               ms_hw_kind *kind, int *api_out,
                               char *detail, size_t detail_cap)
{
    if (!ms_video_hw_path_ready(codec)) {
        return MS_ERR_UNSUPPORTED;
    }
    if (codec == MS_CODEC_AVS_PLUS) {
        char mime[64];
        return ms_hw_jni_pick_avs(api_level, allow_sw, name, name_cap, mime, sizeof(mime),
                                  kind, api_out, detail, detail_cap);
    }
    if (ms_hw_video_mime(codec) == NULL) {
        /* 无 AOSP 标准 MIME，不拿 video/avc 或自造类型去 enumerate。 */
        return MS_ERR_UNSUPPORTED;
    }
    return pick_decoder_mime(ms_hw_video_mime(codec), ms_hw_video_label(codec),
                             api_level, allow_sw, 0, name, name_cap, kind, api_out,
                             detail, detail_cap);
}

ms_status ms_hw_jni_pick_aac(int api_level, int allow_sw,
                             char *name, size_t name_cap,
                             ms_hw_kind *kind, int *api_out,
                             char *detail, size_t detail_cap)
{
    return ms_hw_jni_pick_aac_ex(api_level, allow_sw, 0, name, name_cap, kind, api_out,
                                 detail, detail_cap);
}

ms_status ms_hw_jni_pick_aac_ex(int api_level, int allow_sw, int allow_system_aac,
                                char *name, size_t name_cap,
                                ms_hw_kind *kind, int *api_out,
                                char *detail, size_t detail_cap)
{
    int system_only = (allow_system_aac && !allow_sw) ? 1 : 0;
    return pick_decoder_mime("audio/mp4a-latm", "AAC", api_level, allow_sw, system_only,
                             name, name_cap, kind, api_out, detail, detail_cap);
}

ms_status ms_hw_jni_pick_audio(ms_codec codec, int api_level, int allow_sw,
                               int allow_system_aac, char *name, size_t name_cap,
                               ms_hw_kind *kind, int *api_out,
                               char *detail, size_t detail_cap)
{
    int system_only = 0;
    int sw = allow_sw;
    /* 系统软件例外仅 AAC；其它编码即使调用方打开该开关也不放行软件 codec。 */
    if (codec == MS_CODEC_AAC) {
        system_only = (allow_system_aac && !allow_sw) ? 1 : 0;
    } else {
        system_only = 0;
    }
    return pick_decoder_mime(ms_hw_audio_mime(codec), ms_hw_audio_label(codec),
                             api_level, sw, system_only, name, name_cap, kind,
                             api_out, detail, detail_cap);
}

#if 1 /* JNI MediaCodec I/O 始终编入；按 SDK 由 dispatch 选择 */
/*
 * API 16–20：全部 I/O 走 MediaCodec JNI（getInputBuffers / getOutputBuffers）。
 * 不读回输出像素；releaseOutputBuffer(index, render=true)。
 */
typedef struct {
    jobject codec;
    jobject surface;
    jobject in_bufs;
    jobject out_bufs; /* 仅刷新缓存，不把像素拷到 CPU */
    jobject bufinfo;
    /* MediaFormat.setByteBuffer 可能只持有引用，CSD 必须活到 codec 释放。 */
    uint8_t csd0_sc[MS_HW_CSD_CAP + 4u];
    size_t csd0_sc_n;
    uint8_t csd1_sc[MS_HW_CSD_CAP + 4u];
    size_t csd1_sc_n;
    /* 0=已是 Annex B；2/3/4=AVCC/HVCC 长度字段，提交前换成起始码。 */
    unsigned nal_len_size;
    /* API 18–20 出帧：无 releaseOutputBuffer(int,long)，太早的缓冲留 1 个，到点再 render=true。
     * flush/close 后 index 失效必须清。by: Long <mycjzb@gmail.com> */
    int held;
    jint held_idx;
    jint held_flags;
    jlong held_pts;
} ms_hw_jni_codec;

static void clear_exc(JNIEnv *env)
{
    if ((*env)->ExceptionCheck(env))
        (*env)->ExceptionClear(env);
}

ms_status ms_hw_jni_codec_open(ms_hw_decoder *d, const char *name,
                               const ms_hw_config *cfg)
{
    JNIEnv *env = NULL;
    jstring jn, mime, k0, k1;
    jobject codec, fmt, csd0, csd1, surf;
    ms_hw_jni_codec *jc;

    if (!ms_hw_jni_env(&env) || !name)
        return MS_ERR_UNSUPPORTED;
    jc = (ms_hw_jni_codec *)ms_hw_alloc(d, sizeof(*jc));
    if (!jc)
        return MS_ERR_NOMEM;
    memset(jc, 0, sizeof(*jc));

    jn = (*env)->NewStringUTF(env, name);
    codec = (*env)->CallStaticObjectMethod(env, g_codec, g_create_by_name, jn);
    (*env)->DeleteLocalRef(env, jn);
    if ((*env)->ExceptionCheck(env) || !codec) {
        clear_exc(env);
        ms_hw_free(d, jc);
        return MS_ERR_UNSUPPORTED;
    }
    {
        const char *ms = (d->video_mime[0] != 0) ? d->video_mime
                                                 : ms_hw_video_mime(cfg ? cfg->codec : MS_CODEC_H264);
        if (ms == NULL) {
            (*env)->CallVoidMethod(env, codec, g_release);
            clear_exc(env);
            (*env)->DeleteLocalRef(env, codec);
            ms_hw_free(d, jc);
            return MS_ERR_UNSUPPORTED;
        }
        mime = (*env)->NewStringUTF(env, ms);
    }
    fmt = (*env)->CallStaticObjectMethod(env, g_format, g_fmt_create, mime,
                                         (jint)d->width, (jint)d->height);
    (*env)->DeleteLocalRef(env, mime);
    if (!fmt) {
        (*env)->CallVoidMethod(env, codec, g_release);
        clear_exc(env);
        (*env)->DeleteLocalRef(env, codec);
        ms_hw_free(d, jc);
        return MS_ERR_UNKNOWN;
    }
    /* 与 NDK 路径一致：H.264 split 给出裸 NAL，MediaCodec 要起始码。
     * ByteBuffer 指向 jc 内缓冲，避免 setByteBuffer 只持引用时栈/临时区失效。 */
    jc->csd0_sc_n = 0;
    jc->csd1_sc_n = 0;
    jc->nal_len_size = 0;
    if (cfg && cfg->sample_fmt == MS_SAMPLE_FMT_AVCC &&
        cfg->csd != NULL && cfg->csd_size >= 7 && cfg->csd[0] == 1) {
        unsigned ls = (unsigned)((cfg->csd[4] & 3u) + 1u);
        jc->nal_len_size = (ls >= 1 && ls <= 4) ? ls : 4u;
    } else if (cfg && cfg->sample_fmt == MS_SAMPLE_FMT_HVCC &&
               cfg->csd != NULL && cfg->csd_size >= 23 && cfg->csd[0] == 1) {
        unsigned ls = (unsigned)((cfg->csd[21] & 3u) + 1u);
        jc->nal_len_size = (ls >= 1 && ls <= 4) ? ls : 4u;
    }
    if (d->csd0_size) {
        if (ms_hw_nal_with_start_code(d->csd0, d->csd0_size, jc->csd0_sc,
                                      sizeof(jc->csd0_sc), &jc->csd0_sc_n) != 0) {
            (*env)->CallVoidMethod(env, codec, g_release);
            clear_exc(env);
            (*env)->DeleteLocalRef(env, fmt);
            (*env)->DeleteLocalRef(env, codec);
            ms_hw_free(d, jc);
            return MS_ERR_OVERFLOW;
        }
        csd0 = (*env)->NewDirectByteBuffer(env, jc->csd0_sc, (jlong)jc->csd0_sc_n);
        k0 = (*env)->NewStringUTF(env, "csd-0");
        (*env)->CallVoidMethod(env, fmt, g_fmt_set_buf, k0, csd0);
        (*env)->DeleteLocalRef(env, k0);
        (*env)->DeleteLocalRef(env, csd0);
    }
    if (d->csd1_size) {
        if (ms_hw_nal_with_start_code(d->csd1, d->csd1_size, jc->csd1_sc,
                                      sizeof(jc->csd1_sc), &jc->csd1_sc_n) != 0) {
            (*env)->CallVoidMethod(env, codec, g_release);
            clear_exc(env);
            (*env)->DeleteLocalRef(env, fmt);
            (*env)->DeleteLocalRef(env, codec);
            ms_hw_free(d, jc);
            return MS_ERR_OVERFLOW;
        }
        csd1 = (*env)->NewDirectByteBuffer(env, jc->csd1_sc, (jlong)jc->csd1_sc_n);
        k1 = (*env)->NewStringUTF(env, "csd-1");
        (*env)->CallVoidMethod(env, fmt, g_fmt_set_buf, k1, csd1);
        (*env)->DeleteLocalRef(env, k1);
        (*env)->DeleteLocalRef(env, csd1);
    }
    surf = cfg && cfg->surface ? (jobject)cfg->surface : NULL;
    (*env)->CallVoidMethod(env, codec, g_configure, fmt, surf, NULL, 0);
    (*env)->DeleteLocalRef(env, fmt);
    if ((*env)->ExceptionCheck(env)) {
        clear_exc(env);
        (*env)->CallVoidMethod(env, codec, g_release);
        clear_exc(env);
        (*env)->DeleteLocalRef(env, codec);
        ms_hw_free(d, jc);
        return MS_ERR_UNKNOWN;
    }
    jc->codec = (*env)->NewGlobalRef(env, codec);
    if (surf)
        jc->surface = (*env)->NewGlobalRef(env, surf);
    (*env)->DeleteLocalRef(env, codec);
    jc->bufinfo = (*env)->NewGlobalRef(env, (*env)->NewObject(
        env, g_bufinfo, (*env)->GetMethodID(env, g_bufinfo, "<init>", "()V")));
    d->plat = jc;
    return MS_OK;
}

ms_status ms_hw_jni_codec_start(ms_hw_decoder *d)
{
    JNIEnv *env = NULL;
    ms_hw_jni_codec *jc = d ? (ms_hw_jni_codec *)d->plat : NULL;
    jobject arr;
    if (!jc || !ms_hw_jni_env(&env))
        return MS_ERR_STATE;
    (*env)->CallVoidMethod(env, jc->codec, g_start);
    if ((*env)->ExceptionCheck(env)) {
        clear_exc(env);
        return MS_ERR_UNKNOWN;
    }
    arr = (*env)->CallObjectMethod(env, jc->codec, g_get_in_bufs);
    if ((*env)->ExceptionCheck(env)) {
        clear_exc(env);
        arr = NULL;
    }
    if (arr) {
        if (jc->in_bufs)
            (*env)->DeleteGlobalRef(env, jc->in_bufs);
        jc->in_bufs = (*env)->NewGlobalRef(env, arr);
        (*env)->DeleteLocalRef(env, arr);
    }
    if (g_get_out_bufs) {
        arr = (*env)->CallObjectMethod(env, jc->codec, g_get_out_bufs);
        if ((*env)->ExceptionCheck(env)) {
            clear_exc(env);
            arr = NULL;
        }
        if (arr) {
            if (jc->out_bufs)
                (*env)->DeleteGlobalRef(env, jc->out_bufs);
            jc->out_bufs = (*env)->NewGlobalRef(env, arr);
            (*env)->DeleteLocalRef(env, arr);
        }
    }
    return MS_OK;
}

ms_status ms_hw_jni_codec_feed(ms_hw_decoder *d, unsigned slot)
{
    JNIEnv *env = NULL;
    ms_hw_jni_codec *jc = d ? (ms_hw_jni_codec *)d->plat : NULL;
    jint idx;
    jobject buf;
    void *dst;
    jlong cap;
    size_t n;
    jint flags = 0;
    ms_hw_slot *s;

    if (!jc || slot >= d->in_cap || !ms_hw_jni_env(&env))
        return MS_ERR_STATE;
    s = &d->in_slots[slot];
    if (!s->occupied)
        return MS_OK;
    idx = (*env)->CallIntMethod(env, jc->codec, g_dq_in, (jlong)0);
    if ((*env)->ExceptionCheck(env)) {
        clear_exc(env);
        return MS_ERR_UNKNOWN;
    }
    if (idx < 0)
        return MS_AGAIN;
    buf = (*env)->GetObjectArrayElement(env, jc->in_bufs, idx);
    if (!buf)
        return MS_ERR_UNKNOWN;
    dst = (*env)->GetDirectBufferAddress(env, buf);
    cap = (*env)->GetDirectBufferCapacity(env, buf);
    n = s->size;
    if (!dst || cap <= 0) {
        (*env)->DeleteLocalRef(env, buf);
        return MS_ERR_OVERFLOW;
    }
    /* flush 后重送 CSD：必须排队 configure 时的带起始码 SPS/PPS。
     * 不得把 avcC 配置块当成长度前缀 AU 做 avcc_au_to_annexb（会 OVERFLOW）。 */
    if (s->flags & MS_HW_FLAG_CODEC_CONFIG) {
        flags = 2; /* BUFFER_FLAG_CODEC_CONFIG */
        if (jc->csd0_sc_n > 0) {
            if (cap < (jlong)jc->csd0_sc_n) {
                (*env)->DeleteLocalRef(env, buf);
                return MS_ERR_OVERFLOW;
            }
            memcpy(dst, jc->csd0_sc, jc->csd0_sc_n);
            n = jc->csd0_sc_n;
            (*env)->CallVoidMethod(env, jc->codec, g_q_in, idx, 0, (jint)n,
                                   (jlong)0, flags);
            (*env)->DeleteLocalRef(env, buf);
            if ((*env)->ExceptionCheck(env)) {
                clear_exc(env);
                return MS_ERR_UNKNOWN;
            }
            d->stats.platform_copy_ops++;
            d->stats.platform_copy_bytes += n;
            if (jc->csd1_sc_n > 0) {
                jint idx2 = (*env)->CallIntMethod(env, jc->codec, g_dq_in, (jlong)10000);
                jobject buf2;
                void *dst2;
                jlong cap2;
                if ((*env)->ExceptionCheck(env)) {
                    clear_exc(env);
                    ms_hw_consume_slot(d, slot);
                    return MS_ERR_UNKNOWN;
                }
                if (idx2 < 0) {
                    /* csd-0 已入队。等不到第二块输入则先结束，避免重复送 SPS。 */
                    ms_hw_consume_slot(d, slot);
                    return MS_OK;
                }
                buf2 = (*env)->GetObjectArrayElement(env, jc->in_bufs, idx2);
                if (!buf2) {
                    ms_hw_consume_slot(d, slot);
                    return MS_ERR_UNKNOWN;
                }
                dst2 = (*env)->GetDirectBufferAddress(env, buf2);
                cap2 = (*env)->GetDirectBufferCapacity(env, buf2);
                if (!dst2 || cap2 < (jlong)jc->csd1_sc_n) {
                    (*env)->DeleteLocalRef(env, buf2);
                    ms_hw_consume_slot(d, slot);
                    return MS_ERR_OVERFLOW;
                }
                memcpy(dst2, jc->csd1_sc, jc->csd1_sc_n);
                (*env)->CallVoidMethod(env, jc->codec, g_q_in, idx2, 0,
                                       (jint)jc->csd1_sc_n, (jlong)0, flags);
                (*env)->DeleteLocalRef(env, buf2);
                if ((*env)->ExceptionCheck(env)) {
                    clear_exc(env);
                    ms_hw_consume_slot(d, slot);
                    return MS_ERR_UNKNOWN;
                }
                d->stats.platform_copy_ops++;
                d->stats.platform_copy_bytes += jc->csd1_sc_n;
            }
            ms_hw_consume_slot(d, slot);
            return MS_OK;
        }
        /* 无缓存 SPS/PPS：原样拷贝，不做 AU 转换。 */
        if (cap < (jlong)n) {
            (*env)->DeleteLocalRef(env, buf);
            return MS_ERR_OVERFLOW;
        }
        if (n)
            memcpy(dst, s->data, n);
        (*env)->CallVoidMethod(env, jc->codec, g_q_in, idx, 0, (jint)n,
                               (jlong)s->pts, flags);
        (*env)->DeleteLocalRef(env, buf);
        if ((*env)->ExceptionCheck(env)) {
            clear_exc(env);
            return MS_ERR_UNKNOWN;
        }
        d->stats.platform_copy_ops++;
        d->stats.platform_copy_bytes += n;
        ms_hw_consume_slot(d, slot);
        return MS_OK;
    }
    if (jc->nal_len_size > 0 && s->data != NULL && s->size > 0) {
        size_t conv = 0;
        if (ms_hw_avcc_au_to_annexb(s->data, s->size, jc->nal_len_size,
                                    (uint8_t *)dst, (size_t)cap, &conv) != 0) {
            (*env)->DeleteLocalRef(env, buf);
            return MS_ERR_OVERFLOW;
        }
        n = conv;
    } else {
        if (cap < (jlong)n) {
            (*env)->DeleteLocalRef(env, buf);
            return MS_ERR_OVERFLOW;
        }
        if (n)
            memcpy(dst, s->data, n);
    }
    d->stats.platform_copy_ops++;
    d->stats.platform_copy_bytes += n;
    if (s->flags & MS_HW_FLAG_KEY)
        flags |= 1; /* BUFFER_FLAG_SYNC_FRAME / KEY_FRAME */
    if (s->flags & MS_HW_FLAG_EOS)
        flags |= 4;
    (*env)->CallVoidMethod(env, jc->codec, g_q_in, idx, 0, (jint)n,
                           (jlong)s->pts, flags);
    (*env)->DeleteLocalRef(env, buf);
    if ((*env)->ExceptionCheck(env)) {
        clear_exc(env);
        return MS_ERR_UNKNOWN;
    }
    ms_hw_consume_slot(d, slot);
    return MS_OK;
}

ms_status ms_hw_jni_codec_drive(ms_hw_decoder *d)
{
    JNIEnv *env = NULL;
    ms_hw_jni_codec *jc = d ? (ms_hw_jni_codec *)d->plat : NULL;
    jint idx, flags;
    unsigned i;
    ms_status st = MS_AGAIN;

    jlong pts_v;

    if (!jc || !ms_hw_jni_env(&env))
        return MS_ERR_STATE;
    for (i = 0; i < d->in_cap; i++) {
        if (!d->in_slots[i].occupied)
            continue;
        st = ms_hw_jni_codec_feed(d, i);
        if (st == MS_ERR_UNKNOWN || st == MS_ERR_OVERFLOW)
            return st;
    }
    if (jc->held) {
        idx = jc->held_idx;
        flags = jc->held_flags;
        pts_v = jc->held_pts;
        jc->held = 0;
        goto have_output;
    }
    /* 与 NDK 路径一致：timeout=0 时 OMX.google / 部分软解可能一直 TRY_AGAIN、有提交无输出。
     * 10ms 仍非阻塞级。FORMAT/BUFFERS_CHANGED 后同一次 drive 再取，避免空转一轮。 */
    {
        int attempt;
        idx = -1;
        for (attempt = 0; attempt < 4; attempt++) {
            idx = (*env)->CallIntMethod(env, jc->codec, g_dq_out, jc->bufinfo,
                                        (jlong)10000);
            if ((*env)->ExceptionCheck(env)) {
                clear_exc(env);
                return MS_ERR_UNKNOWN;
            }
            if (idx == -2) { /* INFO_OUTPUT_FORMAT_CHANGED */
                ms_hw_event ev;
                memset(&ev, 0, sizeof(ev));
                ev.struct_size = sizeof(ev);
                ev.type = MS_HW_EVENT_FORMAT_CHANGED;
                ev.width = d->width;
                ev.height = d->height;
                (void)ms_hw_push_event(d, &ev);
                continue;
            }
            if (idx == -3) { /* INFO_OUTPUT_BUFFERS_CHANGED */
                jobject arr = NULL;
                if (g_get_out_bufs) {
                    arr = (*env)->CallObjectMethod(env, jc->codec, g_get_out_bufs);
                    if ((*env)->ExceptionCheck(env)) {
                        clear_exc(env);
                        arr = NULL;
                    }
                }
                if (arr) {
                    if (jc->out_bufs)
                        (*env)->DeleteGlobalRef(env, jc->out_bufs);
                    jc->out_bufs = (*env)->NewGlobalRef(env, arr);
                    (*env)->DeleteLocalRef(env, arr);
                }
                continue;
            }
            break;
        }
    }
    if (idx == -1) /* INFO_TRY_AGAIN_LATER */
        return MS_AGAIN;
    if (idx < 0)
        return MS_AGAIN;
    flags = (*env)->GetIntField(env, jc->bufinfo, g_bi_flags);
    pts_v = (*env)->GetLongField(env, jc->bufinfo, g_bi_pts);
have_output:
    if (d->release_fn && jc->surface) {
        /* 无定时释放：回调只会给 NOW/DROP/LATER（can_schedule=0）。 */
        int64_t rel_ns = 0;
        ms_hw_release_action act = d->release_fn(d->release_fn_ctx, (int64_t)pts_v,
                                                 (flags & 4) != 0, 0, &rel_ns);
        if (act == MS_HW_REL_LATER) {
            jc->held = 1;
            jc->held_idx = idx;
            jc->held_flags = flags;
            jc->held_pts = pts_v;
            return MS_AGAIN;
        }
        (*env)->CallVoidMethod(env, jc->codec, g_rel_out, idx,
                               act == MS_HW_REL_DROP ? JNI_FALSE : JNI_TRUE);
        if ((*env)->ExceptionCheck(env)) {
            clear_exc(env);
            return MS_ERR_UNKNOWN;
        }
        if (act == MS_HW_REL_DROP)
            goto after_release; /* 丢帧不推 OUTPUT_RENDERED */
    } else {
        (*env)->CallVoidMethod(env, jc->codec, g_rel_out, idx, JNI_TRUE);
        if ((*env)->ExceptionCheck(env)) {
            clear_exc(env);
            return MS_ERR_UNKNOWN;
        }
    }
    {
        ms_hw_event ev;
        memset(&ev, 0, sizeof(ev));
        ev.struct_size = sizeof(ev);
        ev.type = MS_HW_EVENT_OUTPUT_RENDERED;
        ev.pts = pts_v;
        ev.has_pts = 1; /* bufinfo.presentationTimeUs 有效；原先漏标导致 API 18 不参与同步统计 */
        ev.width = d->width;
        ev.height = d->height;
        d->stats.rendered_frames++;
        if (ms_hw_push_event(d, &ev) != MS_OK)
            return MS_LIMIT;
    }
after_release:
    if (flags & 4) { /* BUFFER_FLAG_END_OF_STREAM */
        ms_hw_event ev;
        memset(&ev, 0, sizeof(ev));
        ev.struct_size = sizeof(ev);
        ev.type = MS_HW_EVENT_EOS;
        (void)ms_hw_push_event(d, &ev);
        d->st = MS_HW_ST_EOS;
    }
    return MS_OK;
}

ms_status ms_hw_jni_codec_flush(ms_hw_decoder *d)
{
    JNIEnv *env = NULL;
    ms_hw_jni_codec *jc = d ? (ms_hw_jni_codec *)d->plat : NULL;
    if (!jc || !ms_hw_jni_env(&env))
        return MS_ERR_STATE;
    jc->held = 0; /* flush 后输出 index 失效 */
    (*env)->CallVoidMethod(env, jc->codec, g_flush);
    if ((*env)->ExceptionCheck(env)) {
        clear_exc(env);
        return MS_ERR_UNKNOWN;
    }
    /* API 16–20：flush 会使 getInputBuffers/getOutputBuffers 数组失效。
     * NDK AMediaCodec_flush 无此缓存问题。必须两边都重新取。 */
    if (g_get_in_bufs) {
        jobject arr = (*env)->CallObjectMethod(env, jc->codec, g_get_in_bufs);
        if ((*env)->ExceptionCheck(env)) {
            clear_exc(env);
        } else if (arr) {
            if (jc->in_bufs)
                (*env)->DeleteGlobalRef(env, jc->in_bufs);
            jc->in_bufs = (*env)->NewGlobalRef(env, arr);
            (*env)->DeleteLocalRef(env, arr);
        }
    }
    if (g_get_out_bufs) {
        jobject arr = (*env)->CallObjectMethod(env, jc->codec, g_get_out_bufs);
        if ((*env)->ExceptionCheck(env)) {
            clear_exc(env);
        } else if (arr) {
            if (jc->out_bufs)
                (*env)->DeleteGlobalRef(env, jc->out_bufs);
            jc->out_bufs = (*env)->NewGlobalRef(env, arr);
            (*env)->DeleteLocalRef(env, arr);
        }
    }
    return MS_OK;
}

void ms_hw_jni_codec_close(ms_hw_decoder *d)
{
    JNIEnv *env = NULL;
    ms_hw_jni_codec *jc = d ? (ms_hw_jni_codec *)d->plat : NULL;
    if (!jc)
        return;
    jc->held = 0;
    if (ms_hw_jni_env(&env) && jc->codec) {
        (*env)->CallVoidMethod(env, jc->codec, g_stop);
        clear_exc(env);
        (*env)->CallVoidMethod(env, jc->codec, g_release);
        clear_exc(env);
        (*env)->DeleteGlobalRef(env, jc->codec);
        if (jc->in_bufs)
            (*env)->DeleteGlobalRef(env, jc->in_bufs);
        if (jc->out_bufs)
            (*env)->DeleteGlobalRef(env, jc->out_bufs);
        if (jc->bufinfo)
            (*env)->DeleteGlobalRef(env, jc->bufinfo);
        if (jc->surface)
            (*env)->DeleteGlobalRef(env, jc->surface);
    }
    ms_hw_free(d, jc);
    d->plat = NULL;
}
#endif /* MS_HW_LEGACY_JNI */

#else
typedef int ms_hw_jni_unused;
#endif
