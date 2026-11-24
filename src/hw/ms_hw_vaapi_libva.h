/*
 * 最小 libva / libva-drm 公开 ABI 子集，仅供运行时 dlopen 使用。
 * 不把 libva 源码或完整头文件纳入本仓库，也不在链接时 NEEDED libva。
 *
 * 下列类型、枚举数值与函数原型取自 Intel libva 2.22.0 的
 * va/va.h 与 va/va_drm.h（MIT）。完整版权声明如下，按许可证保留。
 *
 * Copyright (c) 2007-2009 Intel Corporation. All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.
 * IN NO EVENT SHALL INTEL AND/OR ITS SUPPLIERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR
 * THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * by: Long <mycjzb@gmail.com>
 */
#ifndef MSMEDIA_MS_HW_VAAPI_LIBVA_H
#define MSMEDIA_MS_HW_VAAPI_LIBVA_H

typedef void *MSVADisplay;
typedef int MSVAStatus;

#define MSVA_STATUS_SUCCESS 0x00000000

typedef enum {
    MSVAProfileNone = -1,
    MSVAProfileMPEG2Simple = 0,
    MSVAProfileMPEG2Main = 1,
    MSVAProfileMPEG4Simple = 2,
    MSVAProfileMPEG4AdvancedSimple = 3,
    MSVAProfileMPEG4Main = 4,
    MSVAProfileH264Baseline = 5, /* deprecated in libva; 仅用于未约束 Baseline 的所需值 */
    MSVAProfileH264Main = 6,
    MSVAProfileH264High = 7,
    MSVAProfileVC1Simple = 8,
    MSVAProfileVC1Main = 9,
    MSVAProfileVC1Advanced = 10,
    MSVAProfileH263Baseline = 11,
    MSVAProfileJPEGBaseline = 12,
    MSVAProfileH264ConstrainedBaseline = 13,
    MSVAProfileVP8Version0_3 = 14,
    MSVAProfileH264MultiviewHigh = 15,
    MSVAProfileH264StereoHigh = 16,
    MSVAProfileHEVCMain = 17,
    MSVAProfileHEVCMain10 = 18,
    MSVAProfileVP9Profile0 = 19,
    MSVAProfileVP9Profile1 = 20,
    MSVAProfileVP9Profile2 = 21,
    MSVAProfileVP9Profile3 = 22,
    MSVAProfileHEVCMain12 = 23,
    MSVAProfileHEVCMain422_10 = 24,
    MSVAProfileHEVCMain422_12 = 25,
    MSVAProfileHEVCMain444 = 26,
    MSVAProfileHEVCMain444_10 = 27,
    MSVAProfileHEVCMain444_12 = 28,
    MSVAProfileHEVCSccMain = 29,
    MSVAProfileHEVCSccMain10 = 30,
    MSVAProfileHEVCSccMain444 = 31,
    MSVAProfileAV1Profile0 = 32,
    MSVAProfileAV1Profile1 = 33,
    MSVAProfileHEVCSccMain444_10 = 34,
    MSVAProfileProtected = 35,
    MSVAProfileH264High10 = 36,
    MSVAProfileVVCMain10 = 37,
    MSVAProfileVVCMultilayerMain10 = 38
} MSVAProfile;

typedef enum {
    MSVAEntrypointVLD = 1
} MSVAEntrypoint;

typedef MSVADisplay (*ms_va_get_display_drm_fn)(int fd);
typedef MSVAStatus (*ms_va_initialize_fn)(MSVADisplay dpy, int *major, int *minor);
typedef MSVAStatus (*ms_va_terminate_fn)(MSVADisplay dpy);
typedef const char *(*ms_va_query_vendor_string_fn)(MSVADisplay dpy);
typedef const char *(*ms_va_error_str_fn)(MSVAStatus status);
typedef int (*ms_va_max_num_profiles_fn)(MSVADisplay dpy);
typedef int (*ms_va_max_num_entrypoints_fn)(MSVADisplay dpy);
typedef MSVAStatus (*ms_va_query_config_profiles_fn)(MSVADisplay dpy,
                                                     MSVAProfile *list,
                                                     int *num);
typedef MSVAStatus (*ms_va_query_config_entrypoints_fn)(MSVADisplay dpy,
                                                        MSVAProfile profile,
                                                        MSVAEntrypoint *list,
                                                        int *num);

#endif
