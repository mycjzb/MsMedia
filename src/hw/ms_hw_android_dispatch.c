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
 * VSYNC 采样：精简移植 Media3 VideoFrameReleaseHelper.VSyncSampler。
 * Media3 在独立 HandlerThread 上用 Choreographer 每 VSYNC_SAMPLE_UPDATE_PERIOD_MS=500ms
 * 采一次 vsync 时间，刷新周期取 Display.getRefreshRate。这里无 Java 侧，改为：
 *   - 进程级一个 ALooper 线程 + AChoreographer（API 24+，dlsym 取）；
 *   - 每 500ms 采一帧，紧接着再采下一帧，两帧差即刷新周期（跳帧时差值偏大，按 1.5 倍门限拒收，
 *     连续 4 次不符才接受，以适应刷新率真的变了）；
 *   - 32 位进程的 postFrameCallbackDelayed 回调参数是 long（32 位会截断纳秒），
 *     只用 API 29 的 postFrameCallbackDelayed64；64 位进程两者皆可。
 * 不可用（API 21–23 或符号缺失）时 get 返回 0，出帧只做 releaseTimeNs 定时，不做 VSYNC 吸附。
 * 线程常驻（引用计数归零只停止重投回调），避免在 Choreographer 仍有挂起回调时拆 Looper。
 * by: Long <mycjzb@gmail.com>
 */
#include <android/looper.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdint.h>

#define MS_VSYNC_SAMPLE_MS 500 /* Media3 VSYNC_SAMPLE_UPDATE_PERIOD_MS */

typedef struct AChoreographer AChoreographer;
typedef void (*ms_ac_cb_long)(long frame_time_ns, void *data);
typedef void (*ms_ac_cb_64)(int64_t frame_time_ns, void *data);
typedef AChoreographer *(*ms_ac_get_fn)(void);
typedef void (*ms_ac_post_long_fn)(AChoreographer *, ms_ac_cb_long, void *, long);
typedef void (*ms_ac_post_64_fn)(AChoreographer *, ms_ac_cb_64, void *, uint32_t);

static pthread_mutex_t g_vs_mu = PTHREAD_MUTEX_INITIALIZER;
static int g_vs_refs;
static int g_vs_started;
static int g_vs_posted;
static int g_vs_measure;    /* 1=下一次回调用于量周期 */
static int g_vs_reject;
static int64_t g_vs_prev_ns;
static int64_t g_vs_time_ns;
static int64_t g_vs_period_ns;
static ALooper *g_vs_looper;
static AChoreographer *g_vs_ch;
static ms_ac_get_fn g_vs_get;
static ms_ac_post_long_fn g_vs_post_long;
static ms_ac_post_64_fn g_vs_post_64;

static void vs_post_locked(long delay_ms);

static void vs_on_frame(int64_t t)
{
    pthread_mutex_lock(&g_vs_mu);
    g_vs_posted = 0;
    if (g_vs_measure && g_vs_prev_ns > 0 && t > g_vs_prev_ns) {
        int64_t d = t - g_vs_prev_ns;
        if (d >= 4000000ll && d <= 50000000ll) {
            if (g_vs_period_ns == 0 || d <= g_vs_period_ns * 3 / 2 || ++g_vs_reject >= 4) {
                g_vs_period_ns = d;
                g_vs_reject = 0;
            }
        }
    }
    g_vs_time_ns = t;
    g_vs_prev_ns = t;
    if (g_vs_refs > 0) {
        /* 交替：量周期用的紧邻帧（0ms），然后间隔 500ms 再采。 */
        g_vs_measure = !g_vs_measure;
        vs_post_locked(g_vs_measure ? 0 : MS_VSYNC_SAMPLE_MS);
    } else {
        g_vs_measure = 0;
        g_vs_prev_ns = 0;
    }
    pthread_mutex_unlock(&g_vs_mu);
}

static void vs_cb_long(long t, void *data)
{
    (void)data;
    vs_on_frame((int64_t)t);
}

static void vs_cb_64(int64_t t, void *data)
{
    (void)data;
    vs_on_frame(t);
}

static void vs_post_locked(long delay_ms)
{
    if (g_vs_ch == NULL || g_vs_posted) {
        return;
    }
    if (g_vs_post_64 != NULL) {
        g_vs_post_64(g_vs_ch, vs_cb_64, NULL, (uint32_t)delay_ms);
    } else if (g_vs_post_long != NULL) {
        g_vs_post_long(g_vs_ch, vs_cb_long, NULL, delay_ms);
    } else {
        return;
    }
    g_vs_posted = 1;
}

static void *vs_thread(void *arg)
{
    ALooper *lp;
    (void)arg;
    lp = ALooper_prepare(0);
    if (lp == NULL) {
        return NULL;
    }
    ALooper_acquire(lp);
    pthread_mutex_lock(&g_vs_mu);
    g_vs_looper = lp;
    g_vs_ch = g_vs_get();
    pthread_mutex_unlock(&g_vs_mu);
    for (;;) {
        pthread_mutex_lock(&g_vs_mu);
        if (g_vs_refs > 0 && !g_vs_posted) {
            g_vs_measure = 0;
            g_vs_prev_ns = 0;
            vs_post_locked(0);
        }
        pthread_mutex_unlock(&g_vs_mu);
        (void)ALooper_pollOnce(-1, NULL, NULL, NULL);
    }
    return NULL;
}

static int vs_load_locked(void)
{
    void *h;
    if (g_vs_get != NULL) {
        return 1;
    }
    h = dlopen("libandroid.so", RTLD_NOW);
    if (h == NULL) {
        return 0;
    }
    g_vs_post_64 = (ms_ac_post_64_fn)dlsym(h, "AChoreographer_postFrameCallbackDelayed64");
    if (sizeof(long) >= 8) {
        g_vs_post_long = (ms_ac_post_long_fn)dlsym(h, "AChoreographer_postFrameCallbackDelayed");
    }
    if (g_vs_post_64 == NULL && g_vs_post_long == NULL) {
        return 0;
    }
    g_vs_get = (ms_ac_get_fn)dlsym(h, "AChoreographer_getInstance");
    return g_vs_get != NULL;
}

void ms_hw_vsync_acquire(void)
{
    pthread_mutex_lock(&g_vs_mu);
    g_vs_refs++;
    if (!g_vs_started) {
        g_vs_started = 1; /* 只尝试一次；失败则永久按“无 VSYNC”处理 */
        if (vs_load_locked()) {
            pthread_t th;
            pthread_attr_t at;
            pthread_attr_init(&at);
            pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
            if (pthread_create(&th, &at, vs_thread, NULL) != 0) {
                g_vs_get = NULL;
            }
            pthread_attr_destroy(&at);
        }
    } else if (g_vs_looper != NULL) {
        ALooper_wake(g_vs_looper); /* 让常驻线程重新投递回调 */
    }
    pthread_mutex_unlock(&g_vs_mu);
}

void ms_hw_vsync_release(void)
{
    pthread_mutex_lock(&g_vs_mu);
    if (g_vs_refs > 0) {
        g_vs_refs--;
    }
    if (g_vs_refs == 0) {
        g_vs_time_ns = 0; /* 停采后旧值不再可信 */
    }
    pthread_mutex_unlock(&g_vs_mu);
}

int ms_hw_vsync_get(int64_t *vsync_ns, int64_t *period_ns)
{
    int ok;
    pthread_mutex_lock(&g_vs_mu);
    ok = (g_vs_refs > 0 && g_vs_time_ns > 0 && g_vs_period_ns > 0);
    if (ok) {
        *vsync_ns = g_vs_time_ns;
        *period_ns = g_vs_period_ns;
    }
    pthread_mutex_unlock(&g_vs_mu);
    return ok;
}

#endif /* __ANDROID__ */
