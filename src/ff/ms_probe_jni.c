/*
 * JNI：com.mycjzb.media.player.MsProbe ↔ ms_probe_media_offset（剥伪装头）。
 */
#include "../core/ms_probe.h"
#include <jni.h>
#include <stdint.h>

JNIEXPORT jint JNICALL
Java_com_mycjzb_media_player_MsProbe_nativeMediaOffset(JNIEnv *env, jclass clazz,
                                                     jbyteArray jbuf, jint len, jboolean eof)
{
    jbyte *p;
    int need = 0, off;
    (void)clazz;
    if (jbuf == NULL || len <= 0) return 0;
    if (len > (*env)->GetArrayLength(env, jbuf)) return 0;
    p = (*env)->GetByteArrayElements(env, jbuf, NULL);
    if (p == NULL) return 0;
    off = ms_probe_media_offset((const uint8_t *)p, (int)len, eof ? 1 : 0, &need);
    (*env)->ReleaseByteArrayElements(env, jbuf, p, JNI_ABORT);
    if (need) return -1; /* 还要更多字节 */
    return off;
}
