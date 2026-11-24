/*
 * ms_hw_android.c 双编译产物：NDK 与 JNI 两套实现的内部入口。
 * 公共 ms_hw_android_* 在 ms_hw_android_dispatch.c 按 SDK 选择。
 * by: Long <mycjzb@gmail.com>
 */
#ifndef MSMEDIA_HW_ANDROID_PATHS_H
#define MSMEDIA_HW_ANDROID_PATHS_H

#include "ms_hw_internal.h"

ms_status ms_hw_android_ndk_query(ms_codec codec, int api_level, int allow_sw,
                                  ms_hw_caps *out, char *name_buf, size_t name_cap,
                                  char *detail_buf, size_t detail_cap);
ms_status ms_hw_android_ndk_configure(ms_hw_decoder *d, const ms_hw_config *cfg);
ms_status ms_hw_android_ndk_start(ms_hw_decoder *d);
ms_status ms_hw_android_ndk_submit(ms_hw_decoder *d, unsigned slot_index);
ms_status ms_hw_android_ndk_drive(ms_hw_decoder *d);
ms_status ms_hw_android_ndk_flush(ms_hw_decoder *d);
void ms_hw_android_ndk_close(ms_hw_decoder *d);
ms_status ms_hw_android_ndk_copy_last_output(ms_hw_decoder *d,
                                             int *width, int *height,
                                             int *stride, int *slice_height,
                                             int *color_format,
                                             uint8_t *dst, size_t dst_cap,
                                             size_t *out_size);

ms_status ms_hw_android_jni_query(ms_codec codec, int api_level, int allow_sw,
                                  ms_hw_caps *out, char *name_buf, size_t name_cap,
                                  char *detail_buf, size_t detail_cap);
ms_status ms_hw_android_jni_configure(ms_hw_decoder *d, const ms_hw_config *cfg);
ms_status ms_hw_android_jni_start(ms_hw_decoder *d);
ms_status ms_hw_android_jni_submit(ms_hw_decoder *d, unsigned slot_index);
ms_status ms_hw_android_jni_drive(ms_hw_decoder *d);
ms_status ms_hw_android_jni_flush(ms_hw_decoder *d);
void ms_hw_android_jni_close(ms_hw_decoder *d);

#endif
