/*
 * com.msmedia.net.MsHttpClient 的 JNI：把公共可控网络层（ms_netcore）开放给 App 业务代码。
 * 与播放器共用同一份拨号/DNS/SOCKS5/系统 TLS/HTTP 实现，不另写网络栈。
 * 不记录 URL、请求头和请求体。
 * by: Long <mycjzb@gmail.com>
 */
#ifdef __ANDROID__
#include "ms_netcore.h"
#include <jni.h>
#include <stdlib.h>
#include <string.h>

typedef struct nc_call {
    volatile int cancel;
    ms_nc_http *h;
} nc_call;

static void throw_io(JNIEnv *env, int code)
{
    jclass c = (*env)->FindClass(env, "java/io/IOException");
    char msg[96];
    if (c == NULL) return;
    if (code == MS_NC_E_CANCELED) {
        snprintf(msg, sizeof(msg), "Canceled");
    } else {
        snprintf(msg, sizeof(msg), "msnet error %d: %s", code, ms_nc_strerror(code));
    }
    (*env)->ThrowNew(env, c, msg);
    (*env)->DeleteLocalRef(env, c);
}

static void copy_jstr(JNIEnv *env, jstring s, char *dst, size_t cap)
{
    const char *v;
    dst[0] = 0;
    if (s == NULL) return;
    v = (*env)->GetStringUTFChars(env, s, NULL);
    if (v) {
        snprintf(dst, cap, "%s", v);
        (*env)->ReleaseStringUTFChars(env, s, v);
    }
}

JNIEXPORT jint JNICALL
Java_com_msmedia_net_MsHttpClient_nativeInit(JNIEnv *env, jclass cls)
{
    (void)cls;
    return ms_nc_jni_init(env);
}

JNIEXPORT jint JNICALL
Java_com_msmedia_net_MsHttpClient_nativeConfigure(JNIEnv *env, jclass cls, jstring dnsMap,
                                                  jboolean javaDns, jstring socksHost,
                                                  jint socksPort, jstring user, jstring pass,
                                                  jboolean remoteDns)
{
    ms_nc_config c;
    int r;
    (void)cls;
    ms_nc_config_default(&c);
    copy_jstr(env, dnsMap, c.dns_map, sizeof(c.dns_map));
    copy_jstr(env, socksHost, c.socks_host, sizeof(c.socks_host));
    copy_jstr(env, user, c.socks_user, sizeof(c.socks_user));
    copy_jstr(env, pass, c.socks_pass, sizeof(c.socks_pass));
    c.use_java_dns = javaDns ? 1 : 0;
    c.socks_port = socksPort;
    c.socks_remote_dns = remoteDns ? 1 : 0;
    r = ms_nc_configure(&c);
    memset(c.socks_pass, 0, sizeof(c.socks_pass));
    return r;
}

JNIEXPORT jlong JNICALL
Java_com_msmedia_net_MsHttpClient_nativeCreate(JNIEnv *env, jclass cls)
{
    (void)env;
    (void)cls;
    return (jlong)(intptr_t)calloc(1, sizeof(nc_call));
}

JNIEXPORT jint JNICALL
Java_com_msmedia_net_MsHttpClient_nativeExecute(JNIEnv *env, jclass cls, jlong handle,
                                                jstring method, jstring url, jstring headers,
                                                jbyteArray body, jint connectMs, jint readMs,
                                                jint maxRedirects)
{
    nc_call *c = (nc_call *)(intptr_t)handle;
    ms_nc_req rq;
    const char *m = NULL, *u = NULL, *hd = NULL;
    jbyte *b = NULL;
    jsize blen = 0;
    int r;
    (void)cls;
    if (c == NULL || url == NULL) {
        throw_io(env, MS_NC_E_ARG);
        return -1;
    }
    if (c->h != NULL) {
        throw_io(env, MS_NC_E_ARG); /* 一个 Call 只能执行一次 */
        return -1;
    }
    memset(&rq, 0, sizeof(rq));
    if (method) m = (*env)->GetStringUTFChars(env, method, NULL);
    u = (*env)->GetStringUTFChars(env, url, NULL);
    if (headers) hd = (*env)->GetStringUTFChars(env, headers, NULL);
    if (body) {
        blen = (*env)->GetArrayLength(env, body);
        b = (*env)->GetByteArrayElements(env, body, NULL);
    }
    rq.method = m;
    rq.url = u;
    rq.headers = hd;
    rq.body = (const uint8_t *)b;
    rq.body_len = (size_t)(blen > 0 ? blen : 0);
    rq.connect_timeout_ms = connectMs;
    rq.read_timeout_ms = readMs;
    rq.max_redirects = maxRedirects;
    rq.cancel = &c->cancel;
    r = (u != NULL) ? ms_nc_http_open(&rq, &c->h) : MS_NC_E_NOMEM;
    if (b) (*env)->ReleaseByteArrayElements(env, body, b, JNI_ABORT);
    if (hd) (*env)->ReleaseStringUTFChars(env, headers, hd);
    if (u) (*env)->ReleaseStringUTFChars(env, url, u);
    if (m) (*env)->ReleaseStringUTFChars(env, method, m);
    if (r != MS_NC_OK) {
        c->h = NULL;
        throw_io(env, r);
        return -1;
    }
    return ms_nc_http_status(c->h);
}

JNIEXPORT jstring JNICALL
Java_com_msmedia_net_MsHttpClient_nativeHeaders(JNIEnv *env, jclass cls, jlong handle)
{
    nc_call *c = (nc_call *)(intptr_t)handle;
    (void)cls;
    return (*env)->NewStringUTF(env, (c && c->h) ? ms_nc_http_headers(c->h) : "");
}

JNIEXPORT jlong JNICALL
Java_com_msmedia_net_MsHttpClient_nativeContentLength(JNIEnv *env, jclass cls, jlong handle)
{
    nc_call *c = (nc_call *)(intptr_t)handle;
    (void)env;
    (void)cls;
    return (c && c->h) ? (jlong)ms_nc_http_content_length(c->h) : -1;
}

JNIEXPORT jint JNICALL
Java_com_msmedia_net_MsHttpClient_nativeRead(JNIEnv *env, jclass cls, jlong handle,
                                             jbyteArray buf, jint off, jint len)
{
    nc_call *c = (nc_call *)(intptr_t)handle;
    uint8_t tmp[16384];
    int r;
    jsize alen;
    (void)cls;
    if (c == NULL || c->h == NULL || buf == NULL) {
        throw_io(env, MS_NC_E_ARG);
        return -1;
    }
    alen = (*env)->GetArrayLength(env, buf);
    if (off < 0 || len < 0 || off > alen || len > alen - off) {
        throw_io(env, MS_NC_E_ARG);
        return -1;
    }
    if (len == 0) return 0;
    if (len > (jint)sizeof(tmp)) len = (jint)sizeof(tmp);
    r = ms_nc_http_read(c->h, tmp, len);
    if (r < 0) {
        throw_io(env, r);
        return -1;
    }
    if (r == 0) return -1; /* 与 InputStream 约定一致 */
    (*env)->SetByteArrayRegion(env, buf, off, r, (const jbyte *)tmp);
    return r;
}

/* 任意线程可调：只置标志，正在进行的拨号/读写 <=100ms 内返回 Canceled。 */
JNIEXPORT void JNICALL
Java_com_msmedia_net_MsHttpClient_nativeCancel(JNIEnv *env, jclass cls, jlong handle)
{
    nc_call *c = (nc_call *)(intptr_t)handle;
    (void)env;
    (void)cls;
    if (c) __atomic_store_n(&c->cancel, 1, __ATOMIC_RELEASE);
}

/* 只能在没有并发 execute/read 时调用（Java 侧加锁保证）。 */
JNIEXPORT void JNICALL
Java_com_msmedia_net_MsHttpClient_nativeRelease(JNIEnv *env, jclass cls, jlong handle)
{
    nc_call *c = (nc_call *)(intptr_t)handle;
    (void)env;
    (void)cls;
    if (c == NULL) return;
    ms_nc_http_close(c->h);
    free(c);
}
#endif
