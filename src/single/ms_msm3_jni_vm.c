/*
 * msm3 精简 JNI_OnLoad：登记 JavaVM + soft guard / apk 签名。
 * 不初始化 MediaCodecList（解封装/硬解归 Media3）。
 * by: Long <mycjzb@gmail.com>
 */
#ifdef __ANDROID__

#include "../hw/ms_hw_jni.h"
#include "../guard/guard.h"
#include "../guard/apk_sign.h"

#include <jni.h>
#include <pthread.h>

static JavaVM *g_vm;
static int g_ready;
static pthread_key_t g_detach_key;
static pthread_once_t g_detach_once = PTHREAD_ONCE_INIT;
static int g_detach_ok;

static void detach_dtor(void *p)
{
    (void)p;
    if (g_vm) {
        (void)(*g_vm)->DetachCurrentThread(g_vm);
    }
}

static void detach_key_init(void)
{
    g_detach_ok = pthread_key_create(&g_detach_key, detach_dtor) == 0;
}

int ms_hw_jni_has_vm(void)
{
    return g_vm != NULL && g_ready;
}

int ms_hw_jni_onload(JavaVM *vm)
{
    JNIEnv *env = NULL;
    g_vm = vm;
    g_ready = 0;
    if (vm == NULL) {
        return -1;
    }
    if ((*vm)->GetEnv(vm, (void **)&env, JNI_VERSION_1_6) != JNI_OK) {
        return -1;
    }
    (void)pthread_once(&g_detach_once, detach_key_init);
    g_ready = 1;
    return 0;
}

int ms_hw_jni_env(JNIEnv **env)
{
    jint att;
    if (!g_vm || !env) {
        return 0;
    }
    *env = NULL;
    att = (*g_vm)->GetEnv(g_vm, (void **)env, JNI_VERSION_1_6);
    if (att == JNI_OK) {
        return 1;
    }
    if (att != JNI_EDETACHED) {
        return 0;
    }
    if ((*g_vm)->AttachCurrentThread(g_vm, env, NULL) != 0) {
        return 0;
    }
    if (g_detach_ok) {
        (void)pthread_setspecific(g_detach_key, (void *)1);
    }
    return 1;
}

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *reserved)
{
    JNIEnv *env = NULL;
    (void)reserved;
    if (ms_hw_jni_onload(vm) != 0) {
        return JNI_ERR;
    }
    if ((*vm)->GetEnv(vm, (void **)&env, JNI_VERSION_1_6) == JNI_OK && env) {
        ms_apk_sign_ensure(env);
    }
    ms_guard_ensure();
    return JNI_VERSION_1_6;
}

#endif /* __ANDROID__ */
