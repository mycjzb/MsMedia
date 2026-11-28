/*
 * libtvcore.so（TVBus / SOPCast 引擎）加载器。
 *
 * 为什么不直接 System.loadLibrary：
 *   1. tvcore 会校验宿主 App 签名（16 字节 memcmp），必须在它的 JNI_OnLoad 之前
 *      把 GOT[memcmp] 换成 hook，否则校验失败；
 *   2. dlopen 不会自动调 JNI_OnLoad，所以由本模块手动调用并 RegisterNatives；
 *   3. 部分设备 / Berberis 上 dlopen 会 SIGBUS/SIGABRT，需要软捕获，不能拖死主 so。
 *
 * 用法（Java 侧 MsTvbus.nativeLoad）：
 *   ms_tvb_load_file(vm, nativeLibraryDir + "/libtvcore.so")
 * 幂等：已就绪返回 1。
 */
#ifndef MSMEDIA_MS_TVB_H
#define MSMEDIA_MS_TVB_H

#include <jni.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 从本地 ELF 路径 dlopen + hook memcmp + JNI_OnLoad + RegisterNatives。成功返回 1。 */
int ms_tvb_load_file(JavaVM *vm, const char *path);

/** 1 = 已就绪；0 = 未装 / 失败（含 16KB Berberis SIGBUS soft-fail）。 */
int ms_tvb_ok(void);

#ifdef __cplusplus
}
#endif

#endif /* MSMEDIA_MS_TVB_H */
