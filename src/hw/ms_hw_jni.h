/*
 * JNI 薄桥（不反射）：MediaCodecList 选择，以及 API 16–20 的 MediaCodec I/O。
 * by: Long <mycjzb@gmail.com>
 */
#ifndef MSMEDIA_HW_JNI_H
#define MSMEDIA_HW_JNI_H

#include "ms_hw_internal.h"

#ifdef __ANDROID__
#include <jni.h>

int ms_hw_jni_onload(JavaVM *vm);
int ms_hw_jni_has_vm(void);
int ms_hw_jni_env(JNIEnv **env);

/* 选出 video/avc 解码器名。found_hw=1 表示分类为硬件。 */
ms_status ms_hw_jni_pick_avc(int api_level, int allow_sw,
                             char *name, size_t name_cap,
                             ms_hw_kind *kind, int *api_out,
                             char *detail, size_t detail_cap);
ms_status ms_hw_jni_pick_hevc(int api_level, int allow_sw,
                              char *name, size_t name_cap,
                              ms_hw_kind *kind, int *api_out,
                              char *detail, size_t detail_cap);
ms_status ms_hw_jni_pick_video(ms_codec codec, int api_level, int allow_sw,
                               char *name, size_t name_cap,
                               ms_hw_kind *kind, int *api_out,
                               char *detail, size_t detail_cap);
/* AVS/AVS+：MediaCodecList 动态挑选，mime_out 为所选组件实际声明的 MIME。 */
ms_status ms_hw_jni_pick_avs(int api_level, int allow_sw,
                             char *name, size_t name_cap,
                             char *mime_out, size_t mime_cap,
                             ms_hw_kind *kind, int *api_out,
                             char *detail, size_t detail_cap);
ms_status ms_hw_jni_pick_aac(int api_level, int allow_sw,
                             char *name, size_t name_cap,
                             ms_hw_kind *kind, int *api_out,
                             char *detail, size_t detail_cap);
/* allow_system_aac=1 时仅接受系统软件名（omx.google/c2.android/c2.google）。 */
ms_status ms_hw_jni_pick_aac_ex(int api_level, int allow_sw, int allow_system_aac,
                                char *name, size_t name_cap,
                                ms_hw_kind *kind, int *api_out,
                                char *detail, size_t detail_cap);
/* allow_system_aac 仅对 AAC 生效；不得把系统软件 MP3/Opus 等当成硬解。 */
ms_status ms_hw_jni_pick_audio(ms_codec codec, int api_level, int allow_sw,
                               int allow_system_aac, char *name, size_t name_cap,
                               ms_hw_kind *kind, int *api_out,
                               char *detail, size_t detail_cap);

/* 始终编进主 so：API 18–20 与 mediandk 不可用时由 dispatch 调用。 */
ms_status ms_hw_jni_codec_open(ms_hw_decoder *d, const char *name,
                               const ms_hw_config *cfg);
ms_status ms_hw_jni_codec_start(ms_hw_decoder *d);
ms_status ms_hw_jni_codec_feed(ms_hw_decoder *d, unsigned slot);
ms_status ms_hw_jni_codec_drive(ms_hw_decoder *d);
ms_status ms_hw_jni_codec_flush(ms_hw_decoder *d);
void ms_hw_jni_codec_close(ms_hw_decoder *d);

#endif /* __ANDROID__ */
#endif

/* 在 JNI_OnLoad 里缓存 VSyncSampler，避免解码线程 FindClass 找不到应用类。 */
void ms_hw_vsync_jni_onload(JNIEnv *env);
