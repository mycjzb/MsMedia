/*
 * JNI：com.mycjzb.media.player.MsTvbus ↔ ms_tvb（libtvcore 加载器）。
 *
 * Java 侧只需给绝对路径（nativeLibraryDir + "/libtvcore.so"）。
 * 返回 1 = 引擎就绪；0 = 失败（未装 / guard / dlopen 崩）。
 */
#include "msmedia/ms_tvb.h"

#include <jni.h>
#include <stdint.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

JNIEXPORT jint JNICALL
Java_com_mycjzb_media_player_MsTvbus_nativeLoad(JNIEnv *env, jclass clazz, jstring jpath)
{
    JavaVM *vm = NULL;
    const char *path;
    int rc;
    (void)clazz;

    if (jpath == NULL) {
        return 0;
    }
    if ((*env)->GetJavaVM(env, &vm) != JNI_OK || vm == NULL) {
        return 0;
    }
    path = (*env)->GetStringUTFChars(env, jpath, NULL);
    if (path == NULL) {
        return 0;
    }
    rc = ms_tvb_load_file(vm, path);
    (*env)->ReleaseStringUTFChars(env, jpath, path);
    return (jint)(rc ? 1 : 0);
}

JNIEXPORT jint JNICALL
Java_com_mycjzb_media_player_MsTvbus_nativeReady(JNIEnv *env, jclass clazz)
{
    (void)env;
    (void)clazz;
    return (jint)(ms_tvb_ok() ? 1 : 0);
}

#ifdef __cplusplus
}
#endif
