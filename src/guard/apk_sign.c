/*
 * APK 签名软校验：反射名 / fail 串走 obf（MsMedia SDK 密钥）。
 * 现钉本机 debug.keystore（androiddebugkey）SHA-256 XOR 0x5A：
 * DIYP 测包不改 App 签名、release 落 debug 时须与此一致；换正式证再改数组并 --pack 1 重编。
 * by: Long <mycjzb@gmail.com>
 */
#include "apk_sign.h"
#include "guard.h"

#include "../obf/obf_strings.h"
#include "../obf/obfstr.h"
#include "../obf/obfdec.h"

#include <string.h>

#if defined(__ANDROID__)
#include <android/log.h>
#endif

#if defined(MS_GUARD_ENABLED) && defined(__ANDROID__)
#include <openssl/sha.h>
#endif

#define MS_APK_SIGN_XOR 0x5A
/* debug.keystore androiddebugkey cert SHA-256 XOR 0x5A */
static const unsigned char EXPECT_SIGN_OBF[32] = {
    0xEE, 0xBE, 0x12, 0xA4, 0xA2, 0x8C, 0x28, 0x71,
    0x9C, 0x87, 0x56, 0xB6, 0xFD, 0x89, 0x8D, 0x6E,
    0x90, 0xB1, 0x8F, 0x29, 0xFC, 0x36, 0x55, 0xF4,
    0x10, 0x70, 0xF8, 0x20, 0x43, 0x88, 0xE8, 0xDA,
};

#define MS_GET_SIGNATURES 0x00000040

static char g_apk_fail[96];

static const char *apk_fail(const unsigned char *obf, size_t n, int kid)
{
    ms_obf_decode_k(obf, n, g_apk_fail, sizeof(g_apk_fail), kid);
    return g_apk_fail;
}

#define APK_FAIL(NAME) apk_fail(MS_OBF_##NAME, MS_OBF_LEN_##NAME, MS_OBF_KID_##NAME)

static int sha_eq_expected(const unsigned char got[32])
{
    unsigned char exp[32];
    int i;
    for (i = 0; i < 32; i++) {
        exp[i] = (unsigned char)(EXPECT_SIGN_OBF[i] ^ MS_APK_SIGN_XOR);
    }
    return memcmp(got, exp, 32) == 0;
}

void ms_apk_sign_ensure(JNIEnv *env)
{
#if !defined(MS_GUARD_ENABLED) || !defined(__ANDROID__)
    (void)env;
    return;
#else
    jobject app, pm, pi, sig0;
    jclass atCls, ctxCls, pmCls, piCls, sigCls;
    jmethodID mid;
    jfieldID fid;
    jstring jpkg;
    jobjectArray sigs;
    jbyteArray jder;
    jbyte *der;
    jsize len, n;
    unsigned char dig[32];

    if (!env) {
        ms_guard_force_fail(APK_FAIL(APK_SIGN_NO_ENV));
        return;
    }

    atCls = MS_JNI_FIND_CLASS(env, CLASS_ACTIVITY_THREAD);
    if (!atCls) {
        (*env)->ExceptionClear(env);
        ms_guard_force_fail(APK_FAIL(APK_SIGN_NO_AT));
        return;
    }
    mid = MS_JNI_GET_STATIC_METHOD(env, atCls, METHOD_CURRENT_APP, SIG_CURRENT_APP);
    if (!mid) {
        (*env)->ExceptionClear(env);
        ms_guard_force_fail(APK_FAIL(APK_SIGN_NO_CUR));
        return;
    }
    app = (*env)->CallStaticObjectMethod(env, atCls, mid);
    if ((*env)->ExceptionCheck(env) || !app) {
        (*env)->ExceptionClear(env);
        ms_guard_force_fail(APK_FAIL(APK_SIGN_NO_APP));
        return;
    }

    ctxCls = (*env)->GetObjectClass(env, app);
    mid = MS_JNI_GET_METHOD(env, ctxCls, METHOD_GET_PKG, SIG_GET_PKG);
    if (!mid) {
        (*env)->ExceptionClear(env);
        ms_guard_force_fail(APK_FAIL(APK_SIGN_NO_GET_PKG));
        return;
    }
    jpkg = (jstring)(*env)->CallObjectMethod(env, app, mid);
    if ((*env)->ExceptionCheck(env) || !jpkg) {
        (*env)->ExceptionClear(env);
        ms_guard_force_fail(APK_FAIL(APK_SIGN_NO_PKG));
        return;
    }

    mid = MS_JNI_GET_METHOD(env, ctxCls, METHOD_GET_PM, SIG_GET_PM);
    if (!mid) {
        (*env)->ExceptionClear(env);
        ms_guard_force_fail(APK_FAIL(APK_SIGN_NO_GET_PM));
        return;
    }
    pm = (*env)->CallObjectMethod(env, app, mid);
    if ((*env)->ExceptionCheck(env) || !pm) {
        (*env)->ExceptionClear(env);
        ms_guard_force_fail(APK_FAIL(APK_SIGN_NO_PM));
        return;
    }

    pmCls = (*env)->GetObjectClass(env, pm);
    mid = MS_JNI_GET_METHOD(env, pmCls, METHOD_GET_PI, SIG_GET_PI);
    if (!mid) {
        (*env)->ExceptionClear(env);
        ms_guard_force_fail(APK_FAIL(APK_SIGN_NO_GET_PI));
        return;
    }
    pi = (*env)->CallObjectMethod(env, pm, mid, jpkg, (jint)MS_GET_SIGNATURES);
    (*env)->DeleteLocalRef(env, jpkg);
    if ((*env)->ExceptionCheck(env) || !pi) {
        (*env)->ExceptionClear(env);
        ms_guard_force_fail(APK_FAIL(APK_SIGN_NO_PI));
        return;
    }

    piCls = (*env)->GetObjectClass(env, pi);
    fid = MS_JNI_GET_FIELD(env, piCls, FIELD_SIGNATURES, SIG_SIGNATURE_ARR);
    if (!fid) {
        (*env)->ExceptionClear(env);
        ms_guard_force_fail(APK_FAIL(APK_SIGN_NO_SIG_FIELD));
        return;
    }
    sigs = (jobjectArray)(*env)->GetObjectField(env, pi, fid);
    if ((*env)->ExceptionCheck(env) || !sigs) {
        (*env)->ExceptionClear(env);
        ms_guard_force_fail(APK_FAIL(APK_SIGN_NO_SIGS));
        return;
    }
    n = (*env)->GetArrayLength(env, sigs);
    if (n <= 0) {
        ms_guard_force_fail(APK_FAIL(APK_SIGN_EMPTY_SIGS));
        return;
    }
    sig0 = (*env)->GetObjectArrayElement(env, sigs, 0);
    if (!sig0) {
        ms_guard_force_fail(APK_FAIL(APK_SIGN_NO_SIG0));
        return;
    }

    sigCls = (*env)->GetObjectClass(env, sig0);
    mid = MS_JNI_GET_METHOD(env, sigCls, METHOD_TO_BYTE_ARRAY, SIG_TO_BYTE_ARRAY);
    if (!mid) {
        (*env)->ExceptionClear(env);
        ms_guard_force_fail(APK_FAIL(APK_SIGN_NO_TO_BYTES));
        return;
    }
    jder = (jbyteArray)(*env)->CallObjectMethod(env, sig0, mid);
    if ((*env)->ExceptionCheck(env) || !jder) {
        (*env)->ExceptionClear(env);
        ms_guard_force_fail(APK_FAIL(APK_SIGN_NO_CERT));
        return;
    }
    len = (*env)->GetArrayLength(env, jder);
    if (len <= 0 || len > 65536) {
        ms_guard_force_fail(APK_FAIL(APK_SIGN_BAD_LEN));
        return;
    }
    der = (*env)->GetByteArrayElements(env, jder, NULL);
    if (!der) {
        ms_guard_force_fail(APK_FAIL(APK_SIGN_NO_PIN));
        return;
    }
    SHA256((const unsigned char *)der, (size_t)len, dig);
    (*env)->ReleaseByteArrayElements(env, jder, der, JNI_ABORT);

    if (!sha_eq_expected(dig)) {
#if defined(__ANDROID__)
        char tag[8], msg[32];
        MS_OBF_DEC_NAME(LOG_TAG_MS, tag);
        MS_OBF_DEC_NAME(APK_SIGN_MISMATCH, msg);
        __android_log_print(ANDROID_LOG_ERROR, tag, "%s", msg);
        ms_obf_wipe(tag, sizeof(tag));
        ms_obf_wipe(msg, sizeof(msg));
#endif
        ms_guard_force_fail(APK_FAIL(APK_SIGN_MISMATCH));
    }
#endif
}
