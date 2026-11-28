/*
 * JNI FindClass / GetMethodID 等：密文解码 → 调用 → wipe。
 * by: Long <mycjzb@gmail.com>
 */
#include "obfstr.h"
#include "obfdec.h"

jclass ms_jni_find_class(JNIEnv *env, const unsigned char *obf, size_t n, int kid)
{
    char buf[512];
    jclass cls;
    ms_obf_decode_k(obf, n, buf, sizeof(buf), kid);
    cls = (*env)->FindClass(env, buf);
    ms_obf_wipe(buf, sizeof(buf));
    return cls;
}

jstring ms_jni_new_string(JNIEnv *env, const unsigned char *obf, size_t n, int kid)
{
    char buf[512];
    jstring s;
    ms_obf_decode_k(obf, n, buf, sizeof(buf), kid);
    s = (*env)->NewStringUTF(env, buf);
    ms_obf_wipe(buf, sizeof(buf));
    return s;
}

jmethodID ms_jni_get_method(JNIEnv *env, jclass cls,
                            const unsigned char *name, size_t name_len, int name_kid,
                            const unsigned char *sig, size_t sig_len, int sig_kid)
{
    char nbuf[128], sbuf[256];
    jmethodID mid;
    ms_obf_decode_k(name, name_len, nbuf, sizeof(nbuf), name_kid);
    ms_obf_decode_k(sig, sig_len, sbuf, sizeof(sbuf), sig_kid);
    mid = (*env)->GetMethodID(env, cls, nbuf, sbuf);
    ms_obf_wipe(nbuf, sizeof(nbuf));
    ms_obf_wipe(sbuf, sizeof(sbuf));
    return mid;
}

jmethodID ms_jni_get_static_method(JNIEnv *env, jclass cls,
                                   const unsigned char *name, size_t name_len, int name_kid,
                                   const unsigned char *sig, size_t sig_len, int sig_kid)
{
    char nbuf[128], sbuf[256];
    jmethodID mid;
    ms_obf_decode_k(name, name_len, nbuf, sizeof(nbuf), name_kid);
    ms_obf_decode_k(sig, sig_len, sbuf, sizeof(sbuf), sig_kid);
    mid = (*env)->GetStaticMethodID(env, cls, nbuf, sbuf);
    ms_obf_wipe(nbuf, sizeof(nbuf));
    ms_obf_wipe(sbuf, sizeof(sbuf));
    return mid;
}

jfieldID ms_jni_get_field(JNIEnv *env, jclass cls,
                          const unsigned char *name, size_t name_len, int name_kid,
                          const unsigned char *sig, size_t sig_len, int sig_kid)
{
    char nbuf[128], sbuf[128];
    jfieldID fid;
    ms_obf_decode_k(name, name_len, nbuf, sizeof(nbuf), name_kid);
    ms_obf_decode_k(sig, sig_len, sbuf, sizeof(sbuf), sig_kid);
    fid = (*env)->GetFieldID(env, cls, nbuf, sbuf);
    ms_obf_wipe(nbuf, sizeof(nbuf));
    ms_obf_wipe(sbuf, sizeof(sbuf));
    return fid;
}
