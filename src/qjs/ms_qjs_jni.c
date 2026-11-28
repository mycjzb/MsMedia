/*
 * JNI n.b：
 *   e(script, url) → Object[]{ kind, url, headers, body, contentType, stream, streamArgs }
 *   s(script, url, argsJson) → byte[]（stream_call）
 * by: Long <mycjzb@gmail.com>
 */
#include "ms_qjs.h"
#include "../guard/guard.h"

#include <jni.h>
#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

JNIEXPORT jobjectArray JNICALL
Java_com_mycjzb_media_player_n_b_e(JNIEnv *env, jclass clazz, jbyteArray jscript, jstring jurl)
{
    const char *url;
    jbyte *script;
    jsize n;
    ms_qjs_result r;
    jobjectArray arr;
    jclass objCls;
    jclass intCls;
    jobject kindObj;
    jmethodID intValueOf;
    (void)clazz;

    ms_guard_ensure();
    if (!ms_guard_ok()) {
        return NULL;
    }
    if (!jscript || !jurl) {
        return NULL;
    }
    n = (*env)->GetArrayLength(env, jscript);
    if (n <= 0 || n > 512 * 1024) {
        return NULL;
    }
    script = (*env)->GetByteArrayElements(env, jscript, NULL);
    url = (*env)->GetStringUTFChars(env, jurl, NULL);
    if (!script || !url) {
        if (script) {
            (*env)->ReleaseByteArrayElements(env, jscript, script, JNI_ABORT);
        }
        if (url) {
            (*env)->ReleaseStringUTFChars(env, jurl, url);
        }
        return NULL;
    }
    memset(&r, 0, sizeof(r));
    if (ms_qjs_eval_ex((const char *)script, (size_t)n, url, &r) != 0) {
        (*env)->ReleaseByteArrayElements(env, jscript, script, JNI_ABORT);
        (*env)->ReleaseStringUTFChars(env, jurl, url);
        return NULL;
    }
    (*env)->ReleaseByteArrayElements(env, jscript, script, JNI_ABORT);
    (*env)->ReleaseStringUTFChars(env, jurl, url);

    objCls = (*env)->FindClass(env, "java/lang/Object");
    intCls = (*env)->FindClass(env, "java/lang/Integer");
    if (!objCls || !intCls) {
        ms_qjs_result_clear(&r);
        return NULL;
    }
    intValueOf = (*env)->GetStaticMethodID(env, intCls, "valueOf", "(I)Ljava/lang/Integer;");
    arr = (*env)->NewObjectArray(env, 7, objCls, NULL);
    if (!arr || !intValueOf) {
        ms_qjs_result_clear(&r);
        return NULL;
    }
    kindObj = (*env)->CallStaticObjectMethod(env, intCls, intValueOf, (jint)r.kind);
    (*env)->SetObjectArrayElement(env, arr, 0, kindObj);
    (*env)->SetObjectArrayElement(env, arr, 1, r.url ? (*env)->NewStringUTF(env, r.url) : NULL);
    (*env)->SetObjectArrayElement(env, arr, 2,
                                  r.headers ? (*env)->NewStringUTF(env, r.headers) : NULL);
    if (r.body && r.body_len > 0) {
        jbyteArray jb = (*env)->NewByteArray(env, (jsize)r.body_len);
        if (jb) {
            (*env)->SetByteArrayRegion(env, jb, 0, (jsize)r.body_len, (jbyte *)r.body);
            (*env)->SetObjectArrayElement(env, arr, 3, jb);
        }
    }
    (*env)->SetObjectArrayElement(
            env, arr, 4, r.content_type ? (*env)->NewStringUTF(env, r.content_type) : NULL);
    (*env)->SetObjectArrayElement(env, arr, 5,
                                  (*env)->CallStaticObjectMethod(env, intCls, intValueOf,
                                                                 (jint)(r.stream ? 1 : 0)));
    (*env)->SetObjectArrayElement(
            env, arr, 6, r.stream_args ? (*env)->NewStringUTF(env, r.stream_args) : NULL);
    ms_qjs_result_clear(&r);
    return arr;
}

JNIEXPORT jbyteArray JNICALL
Java_com_mycjzb_media_player_n_b_s(JNIEnv *env, jclass clazz, jbyteArray jscript, jstring jurl,
                                   jstring jargs)
{
    const char *url;
    const char *args;
    jbyte *script;
    jsize n;
    unsigned char *out = NULL;
    size_t ol = 0;
    jbyteArray ret;
    (void)clazz;

    ms_guard_ensure();
    if (!ms_guard_ok()) {
        return NULL;
    }
    if (!jscript || !jurl) {
        return NULL;
    }
    n = (*env)->GetArrayLength(env, jscript);
    if (n <= 0 || n > 512 * 1024) {
        return NULL;
    }
    script = (*env)->GetByteArrayElements(env, jscript, NULL);
    url = (*env)->GetStringUTFChars(env, jurl, NULL);
    args = jargs ? (*env)->GetStringUTFChars(env, jargs, NULL) : NULL;
    if (!script || !url) {
        if (script) {
            (*env)->ReleaseByteArrayElements(env, jscript, script, JNI_ABORT);
        }
        if (url) {
            (*env)->ReleaseStringUTFChars(env, jurl, url);
        }
        if (args) {
            (*env)->ReleaseStringUTFChars(env, jargs, args);
        }
        return NULL;
    }
    if (ms_qjs_stream_call((const char *)script, (size_t)n, url, args, &out, &ol) != 0 || !out ||
        ol == 0) {
        free(out);
        (*env)->ReleaseByteArrayElements(env, jscript, script, JNI_ABORT);
        (*env)->ReleaseStringUTFChars(env, jurl, url);
        if (args) {
            (*env)->ReleaseStringUTFChars(env, jargs, args);
        }
        return NULL;
    }
    (*env)->ReleaseByteArrayElements(env, jscript, script, JNI_ABORT);
    (*env)->ReleaseStringUTFChars(env, jurl, url);
    if (args) {
        (*env)->ReleaseStringUTFChars(env, jargs, args);
    }
    ret = (*env)->NewByteArray(env, (jsize)ol);
    if (ret) {
        (*env)->SetByteArrayRegion(env, ret, 0, (jsize)ol, (jbyte *)out);
    }
    free(out);
    return ret;
}

#ifdef __cplusplus
}
#endif
