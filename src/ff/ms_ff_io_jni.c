/*
 * JNI：com.mycjzb.media.player.FfIo ↔ ms_ff_io（Media3 DataSource 用）。
 */
#include "msmedia/ms_ff_io.h"

#include <jni.h>
#include <stdint.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

JNIEXPORT jlong JNICALL
Java_com_mycjzb_media_player_FfIo_nativeOpen(JNIEnv *env, jclass clazz, jstring jurl,
                                           jint timeout_ms)
{
    const char *url;
    ms_ff_io *io = NULL;
    ms_status st;
    (void)clazz;
    if (jurl == NULL) {
        return 0;
    }
    url = (*env)->GetStringUTFChars(env, jurl, NULL);
    if (url == NULL) {
        return 0;
    }
    st = ms_ff_io_open(&io, url, (int)timeout_ms);
    (*env)->ReleaseStringUTFChars(env, jurl, url);
    if (st != MS_OK || io == NULL) {
        return 0;
    }
    return (jlong)(intptr_t)io;
}

JNIEXPORT jlong JNICALL
Java_com_mycjzb_media_player_FfIo_nativeOpenEx(JNIEnv *env, jclass clazz, jstring jurl,
                                               jint timeout_ms, jstring jheaders)
{
    const char *url;
    const char *headers = NULL;
    ms_ff_io *io = NULL;
    ms_status st;
    (void)clazz;
    if (jurl == NULL) {
        return 0;
    }
    url = (*env)->GetStringUTFChars(env, jurl, NULL);
    if (url == NULL) {
        return 0;
    }
    if (jheaders != NULL) {
        headers = (*env)->GetStringUTFChars(env, jheaders, NULL);
    }
    st = ms_ff_io_open_ex(&io, url, (int)timeout_ms, headers);
    if (headers != NULL) {
        (*env)->ReleaseStringUTFChars(env, jheaders, headers);
    }
    (*env)->ReleaseStringUTFChars(env, jurl, url);
    if (st != MS_OK || io == NULL) {
        return 0;
    }
    return (jlong)(intptr_t)io;
}

JNIEXPORT void JNICALL
Java_com_mycjzb_media_player_FfIo_nativeClose(JNIEnv *env, jclass clazz, jlong h)
{
    (void)env;
    (void)clazz;
    if (h != 0) {
        ms_ff_io_close((ms_ff_io *)(intptr_t)h);
    }
}

JNIEXPORT jint JNICALL
Java_com_mycjzb_media_player_FfIo_nativeRead(JNIEnv *env, jclass clazz, jlong h,
                                           jbyteArray jbuf, jint off, jint len)
{
    ms_ff_io *io;
    jbyte *p;
    int n;
    (void)clazz;
    if (h == 0 || jbuf == NULL || len <= 0) {
        return -1;
    }
    io = (ms_ff_io *)(intptr_t)h;
    p = (*env)->GetByteArrayElements(env, jbuf, NULL);
    if (p == NULL) {
        return -1;
    }
    n = ms_ff_io_read(io, (uint8_t *)(p + off), (int)len);
    (*env)->ReleaseByteArrayElements(env, jbuf, p, 0);
    return n;
}

JNIEXPORT jlong JNICALL
Java_com_mycjzb_media_player_FfIo_nativeSeek(JNIEnv *env, jclass clazz, jlong h, jlong off,
                                           jint whence)
{
    (void)env;
    (void)clazz;
    if (h == 0) {
        return -1;
    }
    return (jlong)ms_ff_io_seek((ms_ff_io *)(intptr_t)h, (int64_t)off, (int)whence);
}

JNIEXPORT jlong JNICALL
Java_com_mycjzb_media_player_FfIo_nativeSize(JNIEnv *env, jclass clazz, jlong h)
{
    (void)env;
    (void)clazz;
    if (h == 0) {
        return -1;
    }
    return (jlong)ms_ff_io_size((ms_ff_io *)(intptr_t)h);
}

JNIEXPORT jstring JNICALL
Java_com_mycjzb_media_player_FfIo_nativeLastErr(JNIEnv *env, jclass clazz, jlong h)
{
    const char *e;
    (void)clazz;
    e = h != 0 ? ms_ff_io_last_err((ms_ff_io *)(intptr_t)h) : "";
    return (*env)->NewStringUTF(env, e != NULL ? e : "");
}

#ifdef __cplusplus
}
#endif
