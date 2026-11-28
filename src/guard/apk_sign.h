#ifndef MS_APK_SIGN_H
#define MS_APK_SIGN_H

#include <jni.h>

/** ActivityThread → signatures[0] SHA-256 vs 嵌入指纹；失败 soft force_fail。 */
void ms_apk_sign_ensure(JNIEnv *env);

#endif
