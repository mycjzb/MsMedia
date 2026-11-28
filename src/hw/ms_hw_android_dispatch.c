/*
 * 按设备 SDK 选择视频硬解路径：API 21+ 且 mediandk 可用走 NDK AMediaCodec，
 * 否则走 Java MediaCodec JNI（API 18–20）。API 21+ 成功路径不得回退。
 * by: Long <mycjzb@gmail.com>
 */
#include "ms_hw_api.h"
#include "ms_hw_android_paths.h"

#ifdef __ANDROID__

ms_status ms_hw_android_query(ms_codec codec, int api_level, int allow_sw,
                              ms_hw_caps *out, char *name_buf, size_t name_cap,
                              char *detail_buf, size_t detail_cap)
{
    if (ms_hw_use_ndk_mediacodec()) {
        return ms_hw_android_ndk_query(codec, api_level, allow_sw, out,
                                       name_buf, name_cap, detail_buf, detail_cap);
    }
    return ms_hw_android_jni_query(codec, api_level, allow_sw, out,
                                   name_buf, name_cap, detail_buf, detail_cap);
}

ms_status ms_hw_android_configure(ms_hw_decoder *d, const ms_hw_config *cfg)
{
    if (ms_hw_use_ndk_mediacodec()) {
        return ms_hw_android_ndk_configure(d, cfg);
    }
    return ms_hw_android_jni_configure(d, cfg);
}

ms_status ms_hw_android_start(ms_hw_decoder *d)
{
    if (ms_hw_use_ndk_mediacodec()) {
        return ms_hw_android_ndk_start(d);
    }
    return ms_hw_android_jni_start(d);
}

ms_status ms_hw_android_submit(ms_hw_decoder *d, unsigned slot_index)
{
    if (ms_hw_use_ndk_mediacodec()) {
        return ms_hw_android_ndk_submit(d, slot_index);
    }
    return ms_hw_android_jni_submit(d, slot_index);
}

ms_status ms_hw_android_drive(ms_hw_decoder *d)
{
    if (ms_hw_use_ndk_mediacodec()) {
        return ms_hw_android_ndk_drive(d);
    }
    return ms_hw_android_jni_drive(d);
}

ms_status ms_hw_android_flush(ms_hw_decoder *d)
{
    if (ms_hw_use_ndk_mediacodec()) {
        return ms_hw_android_ndk_flush(d);
    }
    return ms_hw_android_jni_flush(d);
}

void ms_hw_android_close(ms_hw_decoder *d)
{
    if (d == NULL) {
        return;
    }
    /* close 必须与 configure 所选路径一致；用 plat 无法区分，故再看一次运行时开关。
     * 同一进程内 SDK 不会变；mediandk 可用性在首次加载后固定。 */
    if (ms_hw_use_ndk_mediacodec()) {
        ms_hw_android_ndk_close(d);
    } else {
        ms_hw_android_jni_close(d);
    }
}

ms_status ms_hw_android_copy_last_output(ms_hw_decoder *d,
                                         int *width, int *height,
                                         int *stride, int *slice_height,
                                         int *color_format,
                                         uint8_t *dst, size_t dst_cap,
                                         size_t *out_size)
{
    /* JNI 路径不把输出像素拷到 CPU（只 render 到 Surface）。 */
    if (!ms_hw_use_ndk_mediacodec()) {
        return MS_AGAIN;
    }
    return ms_hw_android_ndk_copy_last_output(d, width, height, stride, slice_height,
                                              color_format, dst, dst_cap, out_size);
}

/*
 * VSYNC 采样：调用 Java VSyncSampler（Choreographer + Display.getRefreshRate，
 * API 33+ 另用 VSyncSamplerV33 读 FrameTimeline）。对应 Media3
 * VideoFrameReleaseHelper.VSyncSampler / DisplayHelper。
 * 不在此用相邻回调帧差估算刷新率（那是上次简化，已废弃）。
 * 来源 androidx/media main@101e97c，Apache-2.0。接线 by: Long <mycjzb@gmail.com>
 */
#include "ms_hw_jni.h"
#include <jni.h>
#include <stdint.h>
#include <limits.h>

static jclass g_vs_cl;
static jmethodID g_vs_start, g_vs_stop, g_vs_time, g_vs_dur;
static int g_vs_refs;

static int vs_resolve(JNIEnv *env)
{
    jclass cl;
    if (g_vs_cl != NULL && g_vs_start != NULL && g_vs_stop != NULL &&
        g_vs_time != NULL && g_vs_dur != NULL) {
        return 1;
    }
    cl = (*env)->FindClass(env, "com/mycjzb/media/player/media3/VSyncSampler");
    if (cl == NULL) {
        (*env)->ExceptionClear(env);
        return 0;
    }
    g_vs_cl = (*env)->NewGlobalRef(env, cl);
    (*env)->DeleteLocalRef(env, cl);
    if (g_vs_cl == NULL) {
        return 0;
    }
    g_vs_start = (*env)->GetStaticMethodID(env, g_vs_cl, "start", "()V");
    g_vs_stop = (*env)->GetStaticMethodID(env, g_vs_cl, "stop", "()V");
    g_vs_time = (*env)->GetStaticMethodID(env, g_vs_cl, "sampledVsyncTimeNs", "()J");
    g_vs_dur = (*env)->GetStaticMethodID(env, g_vs_cl, "vsyncDurationNs", "()J");
    if ((*env)->ExceptionCheck(env) || !g_vs_start || !g_vs_stop || !g_vs_time || !g_vs_dur) {
        (*env)->ExceptionClear(env);
        g_vs_start = g_vs_stop = g_vs_time = g_vs_dur = NULL;
        return 0;
    }
    return 1;
}

void ms_hw_vsync_jni_onload(JNIEnv *env)
{
    /* JNI_OnLoad 时 ClassLoader 正确，在此缓存类与方法。 */
    (void)vs_resolve(env);
}

void ms_hw_vsync_acquire(void)
{
    JNIEnv *env = NULL;
    if (!ms_hw_jni_env(&env) || env == NULL || !vs_resolve(env)) {
        return;
    }
    g_vs_refs++;
    (*env)->CallStaticVoidMethod(env, g_vs_cl, g_vs_start);
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        if (g_vs_refs > 0) {
            g_vs_refs--;
        }
    }
}

void ms_hw_vsync_release(void)
{
    JNIEnv *env = NULL;
    if (g_vs_refs > 0) {
        g_vs_refs--;
    }
    if (!ms_hw_jni_env(&env) || env == NULL || !vs_resolve(env)) {
        return;
    }
    (*env)->CallStaticVoidMethod(env, g_vs_cl, g_vs_stop);
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
    }
}

int ms_hw_vsync_get(int64_t *vsync_ns, int64_t *period_ns)
{
    JNIEnv *env = NULL;
    int64_t t, d;
    if (vsync_ns == NULL || period_ns == NULL) {
        return 0;
    }
    if (g_vs_refs <= 0 || !ms_hw_jni_env(&env) || env == NULL || !vs_resolve(env)) {
        return 0;
    }
    t = (int64_t)(*env)->CallStaticLongMethod(env, g_vs_cl, g_vs_time);
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        return 0;
    }
    d = (int64_t)(*env)->CallStaticLongMethod(env, g_vs_cl, g_vs_dur);
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        return 0;
    }
    /* VSyncSampler 用 TIME_UNSET 表示尚未采到时间戳。
     * 刷新率尚未读到时按 60Hz 回退（与常见默认显示一致），仍做 VSYNC 吸附。 */
    if (t == 0 || t == (INT64_MIN + 1)) {
        return 0;
    }
    if (d <= 0 || d == (INT64_MIN + 1)) {
        d = 16666666ll;
    }
    *vsync_ns = t;
    *period_ns = d;
    return 1;
}

#endif /* __ANDROID__ */
