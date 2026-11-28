#ifndef MS_OBFSTR_H
#define MS_OBFSTR_H

#include <jni.h>
#include <stddef.h>

#include "obfdec.h"

#ifdef __cplusplus
extern "C" {
#endif

jclass ms_jni_find_class(JNIEnv *env, const unsigned char *obf, size_t n, int kid);
jstring ms_jni_new_string(JNIEnv *env, const unsigned char *obf, size_t n, int kid);
jmethodID ms_jni_get_method(JNIEnv *env, jclass cls,
                            const unsigned char *name, size_t name_len, int name_kid,
                            const unsigned char *sig, size_t sig_len, int sig_kid);
jmethodID ms_jni_get_static_method(JNIEnv *env, jclass cls,
                                   const unsigned char *name, size_t name_len, int name_kid,
                                   const unsigned char *sig, size_t sig_len, int sig_kid);
jfieldID ms_jni_get_field(JNIEnv *env, jclass cls,
                          const unsigned char *name, size_t name_len, int name_kid,
                          const unsigned char *sig, size_t sig_len, int sig_kid);

#define MS_JNI_FIND_CLASS(env, name) \
    ms_jni_find_class((env), MS_OBF_##name, MS_OBF_LEN_##name, MS_OBF_KID_##name)
#define MS_JNI_NEW_STRING(env, name) \
    ms_jni_new_string((env), MS_OBF_##name, MS_OBF_LEN_##name, MS_OBF_KID_##name)
#define MS_JNI_GET_METHOD(env, cls, name, sig) \
    ms_jni_get_method((env), (cls), MS_OBF_##name, MS_OBF_LEN_##name, MS_OBF_KID_##name, \
                      MS_OBF_##sig, MS_OBF_LEN_##sig, MS_OBF_KID_##sig)
#define MS_JNI_GET_STATIC_METHOD(env, cls, name, sig) \
    ms_jni_get_static_method((env), (cls), MS_OBF_##name, MS_OBF_LEN_##name, MS_OBF_KID_##name, \
                             MS_OBF_##sig, MS_OBF_LEN_##sig, MS_OBF_KID_##sig)
#define MS_JNI_GET_FIELD(env, cls, name, sig) \
    ms_jni_get_field((env), (cls), MS_OBF_##name, MS_OBF_LEN_##name, MS_OBF_KID_##name, \
                     MS_OBF_##sig, MS_OBF_LEN_##sig, MS_OBF_KID_##sig)
#define MS_OBF_DEC_NAME(name, buf) \
    ms_obf_decode_k(MS_OBF_##name, MS_OBF_LEN_##name, (buf), sizeof(buf), MS_OBF_KID_##name)

#ifdef __cplusplus
}
#endif

#endif /* MS_OBFSTR_H */
