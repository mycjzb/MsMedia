/*
 * 安卓系统 TLS：经 JNI 调用 com.msmedia.MsSystemTls（SSLEngine）。
 * 默认 TrustManager 使用系统 CA；主机名用 HTTPS 算法或 HostnameVerifier 校验。
 * 不提供关闭校验的接口。非 Android 或无 JVM 时打开失败。
 * by: Long <mycjzb@gmail.com>
 */
#include "ms_android_tls.h"

#ifdef __ANDROID__
#include <jni.h>
#include "ms_hw_jni.h"
#include "../netcore/ms_netcore.h"
#include <stdlib.h>
#include <string.h>

typedef struct ms_android_tls {
    jobject obj;
} ms_android_tls;

static int tls_env(JNIEnv **env)
{
    return ms_hw_jni_has_vm() && ms_hw_jni_env(env) && *env != NULL;
}

static jmethodID mid_of(JNIEnv *env, jobject obj, const char *name, const char *sig, jclass *cl_out)
{
    jclass cl = (*env)->GetObjectClass(env, obj);
    jmethodID mid;
    if (cl_out) {
        *cl_out = cl;
    }
    if (cl == NULL) {
        return NULL;
    }
    mid = (*env)->GetMethodID(env, cl, name, sig);
    if (mid == NULL) {
        (*env)->ExceptionClear(env);
    }
    return mid;
}

int ms_android_tls_open(void **out, const char *hostname)
{
    JNIEnv *env = NULL;
    jclass cl;
    jmethodID ctor;
    jstring jhost;
    jobject obj;
    ms_android_tls *t;

    if (out == NULL || hostname == NULL || hostname[0] == 0) {
        return -1;
    }
    *out = NULL;
    if (!tls_env(&env)) {
        return -1;
    }
    /* 工作线程（FFmpeg/网络线程）FindClass 只看得到系统类：优先用 Java 线程上缓存的全局类引用。 */
    if (ms_nc_jni_tls_class() != NULL) {
        cl = (jclass)(*env)->NewLocalRef(env, ms_nc_jni_tls_class());
    } else {
        cl = (*env)->FindClass(env, "com/msmedia/MsSystemTls");
    }
    if (cl == NULL) {
        (*env)->ExceptionClear(env);
        return -1;
    }
    ctor = (*env)->GetMethodID(env, cl, "<init>", "(Ljava/lang/String;)V");
    if (ctor == NULL) {
        (*env)->ExceptionClear(env);
        (*env)->DeleteLocalRef(env, cl);
        return -1;
    }
    jhost = (*env)->NewStringUTF(env, hostname);
    if (jhost == NULL) {
        (*env)->ExceptionClear(env);
        (*env)->DeleteLocalRef(env, cl);
        return -1;
    }
    obj = (*env)->NewObject(env, cl, ctor, jhost);
    (*env)->DeleteLocalRef(env, jhost);
    if (obj == NULL || (*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        (*env)->DeleteLocalRef(env, cl);
        return -1;
    }
    t = (ms_android_tls *)calloc(1, sizeof(*t));
    if (t == NULL) {
        (*env)->DeleteLocalRef(env, obj);
        (*env)->DeleteLocalRef(env, cl);
        return -1;
    }
    t->obj = (*env)->NewGlobalRef(env, obj);
    (*env)->DeleteLocalRef(env, obj);
    (*env)->DeleteLocalRef(env, cl);
    if (t->obj == NULL) {
        free(t);
        return -1;
    }
    *out = t;
    return 0;
}

void ms_android_tls_close(void *eng)
{
    ms_android_tls *t = (ms_android_tls *)eng;
    JNIEnv *env = NULL;
    jclass cl = NULL;
    jmethodID mid;
    if (t == NULL) {
        return;
    }
    if (tls_env(&env) && t->obj) {
        mid = mid_of(env, t->obj, "close", "()V", &cl);
        if (mid) {
            (*env)->CallVoidMethod(env, t->obj, mid);
            (*env)->ExceptionClear(env);
        }
        if (cl) {
            (*env)->DeleteLocalRef(env, cl);
        }
        (*env)->DeleteGlobalRef(env, t->obj);
    }
    free(t);
}

static int take_net(JNIEnv *env, jobject obj, unsigned char *buf, int cap, int *n)
{
    jclass cl = NULL;
    jmethodID mid;
    jbyteArray jbuf;
    jint got;
    *n = 0;
    mid = mid_of(env, obj, "takeNet", "([B)I", &cl);
    if (!mid) {
        if (cl) (*env)->DeleteLocalRef(env, cl);
        return -1;
    }
    jbuf = (*env)->NewByteArray(env, cap);
    if (!jbuf) {
        (*env)->DeleteLocalRef(env, cl);
        return -1;
    }
    got = (*env)->CallIntMethod(env, obj, mid, jbuf);
    if ((*env)->ExceptionCheck(env) || got < 0) {
        (*env)->ExceptionClear(env);
        (*env)->DeleteLocalRef(env, jbuf);
        (*env)->DeleteLocalRef(env, cl);
        return -1;
    }
    if (got > cap) {
        got = cap;
    }
    if (got > 0) {
        (*env)->GetByteArrayRegion(env, jbuf, 0, got, (jbyte *)buf);
        *n = got;
    }
    (*env)->DeleteLocalRef(env, jbuf);
    (*env)->DeleteLocalRef(env, cl);
    return 0;
}

int ms_android_tls_handshake(void *eng, unsigned char *buf, int cap, int *n)
{
    ms_android_tls *t = (ms_android_tls *)eng;
    JNIEnv *env = NULL;
    jclass cl = NULL;
    jmethodID mid;
    jint st;
    if (t == NULL || buf == NULL || n == NULL || cap <= 0) {
        return -1;
    }
    *n = 0;
    if (!tls_env(&env)) {
        return -1;
    }
    mid = mid_of(env, t->obj, "handshake", "()I", &cl);
    if (!mid) {
        if (cl) (*env)->DeleteLocalRef(env, cl);
        return -1;
    }
    st = (*env)->CallIntMethod(env, t->obj, mid);
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        (*env)->DeleteLocalRef(env, cl);
        return -1;
    }
    (*env)->DeleteLocalRef(env, cl);
    if (st == 2) {
        if (take_net(env, t->obj, buf, cap, n) < 0) {
            return -1;
        }
    }
    return (int)st;
}

int ms_android_tls_feed_net(void *eng, const unsigned char *buf, int n)
{
    ms_android_tls *t = (ms_android_tls *)eng;
    JNIEnv *env = NULL;
    jclass cl = NULL;
    jmethodID mid;
    jbyteArray jbuf;
    jint st;
    if (t == NULL || buf == NULL || n <= 0) {
        return -1;
    }
    if (!tls_env(&env)) {
        return -1;
    }
    mid = mid_of(env, t->obj, "feedNet", "([B)I", &cl);
    if (!mid) {
        if (cl) (*env)->DeleteLocalRef(env, cl);
        return -1;
    }
    jbuf = (*env)->NewByteArray(env, n);
    if (!jbuf) {
        (*env)->DeleteLocalRef(env, cl);
        return -1;
    }
    (*env)->SetByteArrayRegion(env, jbuf, 0, n, (const jbyte *)buf);
    st = (*env)->CallIntMethod(env, t->obj, mid, jbuf);
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        st = -1;
    }
    (*env)->DeleteLocalRef(env, jbuf);
    (*env)->DeleteLocalRef(env, cl);
    return (int)st;
}

int ms_android_tls_read(void *eng, unsigned char *buf, int cap,
                        unsigned char *net, int net_cap, int *net_n, int *need_net)
{
    ms_android_tls *t = (ms_android_tls *)eng;
    JNIEnv *env = NULL;
    jclass cl = NULL;
    jmethodID mid;
    jbyteArray japp;
    jint app_n;
    if (t == NULL || buf == NULL || cap <= 0 || net_n == NULL || need_net == NULL) {
        return -1;
    }
    *net_n = 0;
    *need_net = 0;
    if (!tls_env(&env)) {
        return -1;
    }
    mid = mid_of(env, t->obj, "readApp", "([B)I", &cl);
    if (!mid) {
        if (cl) (*env)->DeleteLocalRef(env, cl);
        return -1;
    }
    japp = (*env)->NewByteArray(env, cap);
    if (!japp) {
        (*env)->DeleteLocalRef(env, cl);
        return -1;
    }
    app_n = (*env)->CallIntMethod(env, t->obj, mid, japp);
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        (*env)->DeleteLocalRef(env, japp);
        (*env)->DeleteLocalRef(env, cl);
        return -1;
    }
    if (app_n > 0) {
        if (app_n > cap) {
            app_n = cap;
        }
        (*env)->GetByteArrayRegion(env, japp, 0, app_n, (jbyte *)buf);
        (*env)->DeleteLocalRef(env, japp);
        (*env)->DeleteLocalRef(env, cl);
        return (int)app_n;
    }
    (*env)->DeleteLocalRef(env, japp);
    (*env)->DeleteLocalRef(env, cl);
    if (app_n < 0) {
        return -1;
    }
    /* 0：需要网络 I/O。needNet()：1=读 TCP，2=写 TCP。 */
    mid = mid_of(env, t->obj, "needNet", "()I", &cl);
    if (mid) {
        *need_net = (int)(*env)->CallIntMethod(env, t->obj, mid);
        (*env)->ExceptionClear(env);
    }
    if (cl) {
        (*env)->DeleteLocalRef(env, cl);
    }
    if (*need_net == 2 && net != NULL && net_cap > 0) {
        if (take_net(env, t->obj, net, net_cap, net_n) < 0) {
            return -1;
        }
    }
    return 0;
}

int ms_android_tls_write(void *eng, const unsigned char *buf, int n,
                         unsigned char *net, int net_cap, int *net_n)
{
    ms_android_tls *t = (ms_android_tls *)eng;
    JNIEnv *env = NULL;
    jclass cl = NULL;
    jmethodID mid;
    jbyteArray japp;
    jint used;
    if (t == NULL || buf == NULL || n <= 0 || net == NULL || net_n == NULL || net_cap <= 0) {
        return -1;
    }
    *net_n = 0;
    if (!tls_env(&env)) {
        return -1;
    }
    mid = mid_of(env, t->obj, "writeApp", "([B)I", &cl);
    if (!mid) {
        if (cl) (*env)->DeleteLocalRef(env, cl);
        return -1;
    }
    japp = (*env)->NewByteArray(env, n);
    if (!japp) {
        (*env)->DeleteLocalRef(env, cl);
        return -1;
    }
    (*env)->SetByteArrayRegion(env, japp, 0, n, (const jbyte *)buf);
    used = (*env)->CallIntMethod(env, t->obj, mid, japp);
    if ((*env)->ExceptionCheck(env) || used < 0) {
        (*env)->ExceptionClear(env);
        (*env)->DeleteLocalRef(env, japp);
        (*env)->DeleteLocalRef(env, cl);
        return -1;
    }
    (*env)->DeleteLocalRef(env, japp);
    (*env)->DeleteLocalRef(env, cl);
    if (take_net(env, t->obj, net, net_cap, net_n) < 0) {
        return -1;
    }
    return (int)used;
}

#else /* !__ANDROID__ */

int ms_android_tls_open(void **out, const char *hostname)
{
    (void)hostname;
    if (out) {
        *out = NULL;
    }
    return -1;
}
void ms_android_tls_close(void *eng) { (void)eng; }
int ms_android_tls_handshake(void *eng, unsigned char *buf, int cap, int *n)
{
    (void)eng; (void)buf; (void)cap;
    if (n) *n = 0;
    return -1;
}
int ms_android_tls_feed_net(void *eng, const unsigned char *buf, int n)
{
    (void)eng; (void)buf; (void)n;
    return -1;
}
int ms_android_tls_read(void *eng, unsigned char *buf, int cap,
                        unsigned char *net, int net_cap, int *net_n, int *need_net)
{
    (void)eng; (void)buf; (void)cap; (void)net; (void)net_cap;
    if (net_n) *net_n = 0;
    if (need_net) *need_net = 0;
    return -1;
}
int ms_android_tls_write(void *eng, const unsigned char *buf, int n,
                         unsigned char *net, int net_cap, int *net_n)
{
    (void)eng; (void)buf; (void)n; (void)net; (void)net_cap;
    if (net_n) *net_n = 0;
    return -1;
}

#endif /* __ANDROID__ */
