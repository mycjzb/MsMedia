/*
 * JNI：com.mycjzb.media.player.FfAdec ↔ ms_ffadec（Media3 音频软解兜底）。
 */
#include "../hw/ms_hw_ffadec.h"

#include <jni.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

JNIEXPORT jboolean JNICALL
Java_com_mycjzb_media_player_FfAdec_nativeAvailable(JNIEnv *env, jclass clazz)
{
    (void)env;
    (void)clazz;
    return (jboolean)(ms_ffadec_supported(MS_CODEC_AC3) ? JNI_TRUE : JNI_FALSE);
}

JNIEXPORT jboolean JNICALL
Java_com_mycjzb_media_player_FfAdec_nativeSupports(JNIEnv *env, jclass clazz, jint codec)
{
    (void)env;
    (void)clazz;
    return (jboolean)(ms_ffadec_supported((ms_codec)codec) ? JNI_TRUE : JNI_FALSE);
}

JNIEXPORT jlong JNICALL
Java_com_mycjzb_media_player_FfAdec_nativeOpen(JNIEnv *env, jclass clazz, jint codec,
                                             jint rate, jint ch)
{
    ms_ffadec *d = NULL;
    (void)env;
    (void)clazz;
    if (ms_ffadec_open((ms_codec)codec, (int)rate, (int)ch, &d) != 0 || d == NULL) {
        return 0;
    }
    return (jlong)(intptr_t)d;
}

JNIEXPORT jlong JNICALL
Java_com_mycjzb_media_player_FfAdec_nativeOpenEx(JNIEnv *env, jclass clazz, jint codec,
                                                 jint rate, jint ch, jbyteArray jextra)
{
    ms_ffadec *d = NULL;
    const uint8_t *extra = NULL;
    size_t extra_n = 0;
    jbyte *pinned = NULL;
    jlong ret;
    (void)clazz;
    if (jextra != NULL) {
        jsize n = (*env)->GetArrayLength(env, jextra);
        if (n > 0 && n < (1 << 20)) {
            pinned = (*env)->GetByteArrayElements(env, jextra, NULL);
            if (pinned == NULL) return 0;
            extra = (const uint8_t *)pinned;
            extra_n = (size_t)n;
        }
    }
    if (ms_ffadec_open_ex((ms_codec)codec, (int)rate, (int)ch, extra, extra_n, &d) != 0 || d == NULL) {
        ret = 0;
    } else {
        ret = (jlong)(intptr_t)d;
    }
    if (pinned != NULL) {
        (*env)->ReleaseByteArrayElements(env, jextra, pinned, JNI_ABORT);
    }
    return ret;
}

JNIEXPORT void JNICALL
Java_com_mycjzb_media_player_FfAdec_nativeClose(JNIEnv *env, jclass clazz, jlong h)
{
    (void)env;
    (void)clazz;
    if (h != 0) {
        ms_ffadec_close((ms_ffadec *)(intptr_t)h);
    }
}

JNIEXPORT void JNICALL
Java_com_mycjzb_media_player_FfAdec_nativeFlush(JNIEnv *env, jclass clazz, jlong h)
{
    (void)env;
    (void)clazz;
    if (h != 0) {
        ms_ffadec_flush((ms_ffadec *)(intptr_t)h);
    }
}

JNIEXPORT jint JNICALL
Java_com_mycjzb_media_player_FfAdec_nativeOutRate(JNIEnv *env, jclass clazz, jlong h)
{
    (void)env;
    (void)clazz;
    return h == 0 ? 0 : ms_ffadec_out_rate((const ms_ffadec *)(intptr_t)h);
}

JNIEXPORT jint JNICALL
Java_com_mycjzb_media_player_FfAdec_nativeOutChannels(JNIEnv *env, jclass clazz, jlong h)
{
    (void)env;
    (void)clazz;
    return h == 0 ? 0 : ms_ffadec_out_channels((const ms_ffadec *)(intptr_t)h);
}

/* 解一帧：写入 direct ByteBuffer，返回写入字节数；0=无 PCM；负=失败。 */
JNIEXPORT jint JNICALL
Java_com_mycjzb_media_player_FfAdec_nativeDecode(JNIEnv *env, jclass clazz, jlong h,
                                               jobject jin, jint in_off, jint in_len,
                                               jobject jout, jint out_cap)
{
    ms_ffadec *d;
    const uint8_t *au;
    const uint8_t *pcm = NULL;
    size_t pcm_n = 0;
    uint8_t *out;
    (void)clazz;
    if (h == 0 || jin == NULL || jout == NULL || in_len <= 0 || out_cap <= 0) {
        return -1;
    }
    d = (ms_ffadec *)(intptr_t)h;
    au = (const uint8_t *)(*env)->GetDirectBufferAddress(env, jin);
    out = (uint8_t *)(*env)->GetDirectBufferAddress(env, jout);
    if (au == NULL || out == NULL) {
        return -1;
    }
    if (ms_ffadec_decode(d, au + in_off, (size_t)in_len, &pcm, &pcm_n) != 0) {
        return -1;
    }
    if (pcm_n == 0 || pcm == NULL) {
        return 0;
    }
    if (pcm_n > (size_t)out_cap) {
        return -2; /* 输出缓冲不够，调用方扩容再试 */
    }
    memcpy(out, pcm, pcm_n);
    return (jint)pcm_n;
}

#ifdef __cplusplus
}
#endif
