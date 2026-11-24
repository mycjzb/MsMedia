/*
 * 设备 SDK 等级与 MediaCodec 路径选择。
 * 编译目标仍是 NDK r29 API 21（r26 起最低 21，没有 android-18 sysroot）。
 * 交付清单 minSdk=18；运行时用 ro.build.version.sdk 判断，不使用 API 24 的
 * android_get_device_api_level 作为门槛。
 * by: Long <mycjzb@gmail.com>
 */
#ifndef MSMEDIA_HW_API_H
#define MSMEDIA_HW_API_H

#ifdef __cplusplus
extern "C" {
#endif

/* ro.build.version.sdk；读失败时返回 0（调用方不要默认当成 21 去链 mediandk）。 */
int ms_hw_device_sdk_int(void);

/*
 * 1：走 NDK AMediaCodec（API 21+ 且 libmediandk.so 已加载）。
 * 0：走 Java MediaCodec JNI（API 18–20，或 21+ 但 mediandk 不可用）。
 * API 21+ 成功加载 mediandk 时不得回退到 JNI。
 */
int ms_hw_use_ndk_mediacodec(void);

#ifdef __cplusplus
}
#endif

#endif
