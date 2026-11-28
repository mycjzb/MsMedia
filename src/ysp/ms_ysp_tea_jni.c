/*
 * JNI：n.a o/c/f/u ↔ ms_m1_*（短名；open 查 ms_guard_ok）。
 */
#include "ysp_tea.h"
#include "../guard/guard.h"

#include <jni.h>
#include <stdint.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

static jbyteArray out_bytes(JNIEnv *env, uint8_t *out, size_t olen)
{
    jbyteArray arr;
    if (!out || olen == 0) {
        free(out);
        return (*env)->NewByteArray(env, 0);
    }
    arr = (*env)->NewByteArray(env, (jsize)olen);
    if (arr) {
        (*env)->SetByteArrayRegion(env, arr, 0, (jsize)olen, (const jbyte *)out);
    }
    free(out);
    return arr;
}

JNIEXPORT jlong JNICALL
Java_com_mycjzb_media_player_n_a_o(JNIEnv *env, jclass clazz, jint mode)
{
    (void)env;
    (void)clazz;
    ms_guard_ensure();
    if (!ms_guard_ok()) {
        return 0;
    }
    if (mode != MS_M1_MODE_1 && mode != MS_M1_MODE_2) {
        return 0;
    }
    return (jlong)(intptr_t)ms_m1_create((int)mode);
}

JNIEXPORT void JNICALL
Java_com_mycjzb_media_player_n_a_c(JNIEnv *env, jclass clazz, jlong handle)
{
    (void)env;
    (void)clazz;
    if (handle) {
        ms_m1_destroy((m1 *)(intptr_t)handle);
    }
}

JNIEXPORT jbyteArray JNICALL
Java_com_mycjzb_media_player_n_a_f(JNIEnv *env, jclass clazz, jlong handle,
                                   jbyteArray jin, jint off, jint len)
{
    m1 *st;
    uint8_t *out = NULL;
    size_t olen = 0;
    jbyte *in;
    int rc;
    (void)clazz;
    if (!handle || !jin || len <= 0) {
        return (*env)->NewByteArray(env, 0);
    }
    if (off < 0 || len < 0 || (jsize)(off + len) > (*env)->GetArrayLength(env, jin)) {
        return NULL;
    }
    if (!ms_guard_ok()) {
        return NULL;
    }
    st = (m1 *)(intptr_t)handle;
    in = (*env)->GetByteArrayElements(env, jin, NULL);
    if (!in) {
        return NULL;
    }
    rc = ms_m1_feed(st, (const uint8_t *)(in + off), (size_t)len, &out, &olen);
    (*env)->ReleaseByteArrayElements(env, jin, in, JNI_ABORT);
    if (rc < 0) {
        free(out);
        return NULL;
    }
    return out_bytes(env, out, olen);
}

JNIEXPORT jbyteArray JNICALL
Java_com_mycjzb_media_player_n_a_u(JNIEnv *env, jclass clazz, jlong handle)
{
    uint8_t *out = NULL;
    size_t olen = 0;
    int rc;
    (void)clazz;
    if (!handle) {
        return (*env)->NewByteArray(env, 0);
    }
    if (!ms_guard_ok()) {
        return NULL;
    }
    rc = ms_m1_flush((m1 *)(intptr_t)handle, &out, &olen);
    if (rc < 0) {
        free(out);
        return NULL;
    }
    return out_bytes(env, out, olen);
}

#ifdef __cplusplus
}
#endif
