/*
 * 按设备 SDK 选择视频硬解路径：API 21+ 且 mediandk 可用走 NDK AMediaCodec，
 * 否则走 Java MediaCodec JNI（API 18–20）。API 21+ 成功路径不得回退。
 * by: Long <mycjzb@gmail.com>
 */
#include "ms_hw_api.h"
#include "ms_hw_android_paths.h"

#ifdef __ANDROID__

ms_status ms_hw_android_query(ms_codec codec, int api_level, int allow_sw,
                              ms_hw_caps *out, char *name_buf, size_t name_cap,
                              char *detail_buf, size_t detail_cap)
{
    if (ms_hw_use_ndk_mediacodec()) {
        return ms_hw_android_ndk_query(codec, api_level, allow_sw, out,
                                       name_buf, name_cap, detail_buf, detail_cap);
    }
    return ms_hw_android_jni_query(codec, api_level, allow_sw, out,
                                   name_buf, name_cap, detail_buf, detail_cap);
}

ms_status ms_hw_android_configure(ms_hw_decoder *d, const ms_hw_config *cfg)
{
    if (ms_hw_use_ndk_mediacodec()) {
        return ms_hw_android_ndk_configure(d, cfg);
    }
    return ms_hw_android_jni_configure(d, cfg);
}

ms_status ms_hw_android_start(ms_hw_decoder *d)
{
    if (ms_hw_use_ndk_mediacodec()) {
        return ms_hw_android_ndk_start(d);
    }
    return ms_hw_android_jni_start(d);
}

ms_status ms_hw_android_submit(ms_hw_decoder *d, unsigned slot_index)
{
    if (ms_hw_use_ndk_mediacodec()) {
        return ms_hw_android_ndk_submit(d, slot_index);
    }
    return ms_hw_android_jni_submit(d, slot_index);
}

ms_status ms_hw_android_drive(ms_hw_decoder *d)
{
    if (ms_hw_use_ndk_mediacodec()) {
        return ms_hw_android_ndk_drive(d);
    }
    return ms_hw_android_jni_drive(d);
}

ms_status ms_hw_android_flush(ms_hw_decoder *d)
{
    if (ms_hw_use_ndk_mediacodec()) {
        return ms_hw_android_ndk_flush(d);
    }
    return ms_hw_android_jni_flush(d);
}

void ms_hw_android_close(ms_hw_decoder *d)
{
    if (d == NULL) {
        return;
    }
    /* close 必须与 configure 所选路径一致；用 plat 无法区分，故再看一次运行时开关。
     * 同一进程内 SDK 不会变；mediandk 可用性在首次加载后固定。 */
    if (ms_hw_use_ndk_mediacodec()) {
        ms_hw_android_ndk_close(d);
    } else {
        ms_hw_android_jni_close(d);
    }
}

ms_status ms_hw_android_copy_last_output(ms_hw_decoder *d,
                                         int *width, int *height,
                                         int *stride, int *slice_height,
                                         int *color_format,
                                         uint8_t *dst, size_t dst_cap,
                                         size_t *out_size)
{
    /* JNI 路径不把输出像素拷到 CPU（只 render 到 Surface）。 */
    if (!ms_hw_use_ndk_mediacodec()) {
        return MS_AGAIN;
    }
    return ms_hw_android_ndk_copy_last_output(d, width, height, stride, slice_height,
                                              color_format, dst, dst_cap, out_size);
}

#endif /* __ANDROID__ */
