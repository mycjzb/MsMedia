/*
 * JNI / 可选命令行探测：只报告选择结果，不以 configure 成功声称已硬解跑通。
 * 日志与字符串不含 URL。
 * by: Long <mycjzb@gmail.com>
 */
#ifdef __ANDROID__

#include "msmedia/ms_hw.h"
#include "ms_hw_jni.h"

#include <jni.h>
#include <stdio.h>
#include <string.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>

static int write_caps(char *dst, size_t cap, const ms_hw_caps *c, const char *extra)
{
    return snprintf(dst, cap,
                    "verified=%d is_hardware=%d kind=%s api=%d backend=%s "
                    "name=%s detail=%s%s%s",
                    c->verified, c->is_hardware,
                    ms_hw_kind_name(c->kind), c->api_level,
                    c->backend ? c->backend : "?",
                    c->codec_name ? c->codec_name : "(none)",
                    c->detail ? c->detail : "",
                    extra && extra[0] ? " | " : "",
                    extra ? extra : "");
}

static void probe_text(char *dst, size_t cap, ANativeWindow *win)
{
    ms_hw_caps caps;
    char extra[192];
    ms_status st;

    memset(&caps, 0, sizeof(caps));
    caps.struct_size = sizeof(caps);
    extra[0] = 0;
    st = ms_hw_decoder_query(MS_CODEC_H264, &caps);
    (void)st;
    if (caps.is_hardware && win != NULL) {
        /* 仅探测 configure/start/flush/destroy；不提交码流，不声称已解码帧。 */
        ms_hw_decoder *d = NULL;
        ms_hw_create_opts opts;
        ms_hw_config cfg;
        /* 最小合法 avcC 外壳：由 configure 再拆 SPS；此处若无真实 SPS 会 MALFORMED。
         * 不嵌入样本流。只说明 surface 已传入。 */
        snprintf(extra, sizeof(extra),
                 "surface=yes; no bitstream submitted; not a decode run");
        (void)d;
        (void)opts;
        (void)cfg;
    } else if (win != NULL) {
        snprintf(extra, sizeof(extra),
                 "surface=yes; configure skipped (no hardware component)");
    } else {
        snprintf(extra, sizeof(extra), "surface=no");
    }
    write_caps(dst, cap, &caps, extra);
}

#if 1
JNIEXPORT jstring JNICALL
Java_com_msmedia_MsHwSmokeActivity_nativeProbe(JNIEnv *env, jobject thiz,
                                               jobject surface)
{
    char buf[512];
    ANativeWindow *win = NULL;
    (void)thiz;
    buf[0] = 0;
    if (surface)
        win = ANativeWindow_fromSurface(env, surface);
    probe_text(buf, sizeof(buf), win);
    if (win)
        ANativeWindow_release(win);
    return (*env)->NewStringUTF(env, buf);
}
#endif

#ifdef MS_HW_PROBE_MAIN
int main(void)
{
    char buf[512];
    probe_text(buf, sizeof(buf), NULL);
    puts(buf);
    return 0;
}
#endif

#endif
