/*
 * libmediandk.so 运行时加载（dlopen/dlsym），避免 libmshw NEEDED 该库。
 * 边界：libmediandk 是 API 21 引入的；API 18–20 加载失败，调用方须走 JNI MediaCodec。
 * 本头在包含 NDK 媒体头之后、调用 AMedia* 之前 include，用宏把调用转到包装函数。
 * KEY_* 用官方字符串字面量，不依赖 libmediandk 里的 extern 指针（那些也是 NEEDED 符号）。
 * by: Long <mycjzb@gmail.com>
 */
#ifndef MSMEDIA_HW_MEDIANDK_H
#define MSMEDIA_HW_MEDIANDK_H

#ifdef __ANDROID__

#include <stdbool.h>
#include <sys/types.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#include <media/NdkMediaError.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 1：API 21+ 且 dlopen+必要符号成功。可重复调用。 */
int ms_hw_mediandk_available(void);

AMediaCodec *ms_hw_AMediaCodec_createCodecByName(const char *name);
media_status_t ms_hw_AMediaCodec_configure(AMediaCodec *codec, const AMediaFormat *fmt,
                                          ANativeWindow *window, AMediaCrypto *crypto,
                                          uint32_t flags);
media_status_t ms_hw_AMediaCodec_start(AMediaCodec *codec);
media_status_t ms_hw_AMediaCodec_stop(AMediaCodec *codec);
media_status_t ms_hw_AMediaCodec_flush(AMediaCodec *codec);
media_status_t ms_hw_AMediaCodec_delete(AMediaCodec *codec);
ssize_t ms_hw_AMediaCodec_dequeueInputBuffer(AMediaCodec *codec, int64_t timeoutUs);
ssize_t ms_hw_AMediaCodec_dequeueOutputBuffer(AMediaCodec *codec, AMediaCodecBufferInfo *info,
                                              int64_t timeoutUs);
uint8_t *ms_hw_AMediaCodec_getInputBuffer(AMediaCodec *codec, size_t idx, size_t *out_size);
uint8_t *ms_hw_AMediaCodec_getOutputBuffer(AMediaCodec *codec, size_t idx, size_t *out_size);
AMediaFormat *ms_hw_AMediaCodec_getOutputFormat(AMediaCodec *codec);
media_status_t ms_hw_AMediaCodec_queueInputBuffer(AMediaCodec *codec, size_t idx,
                                                 off_t offset, size_t size,
                                                 uint64_t time, uint32_t flags);
media_status_t ms_hw_AMediaCodec_releaseOutputBuffer(AMediaCodec *codec, size_t idx,
                                                     bool render);
AMediaFormat *ms_hw_AMediaFormat_new(void);
media_status_t ms_hw_AMediaFormat_delete(AMediaFormat *fmt);
void ms_hw_AMediaFormat_setString(AMediaFormat *fmt, const char *name, const char *value);
void ms_hw_AMediaFormat_setInt32(AMediaFormat *fmt, const char *name, int32_t value);
void ms_hw_AMediaFormat_setBuffer(AMediaFormat *fmt, const char *name, const void *data, size_t size);
bool ms_hw_AMediaFormat_getInt32(AMediaFormat *fmt, const char *name, int32_t *out);

#ifdef __cplusplus
}
#endif

#define AMediaCodec_createCodecByName ms_hw_AMediaCodec_createCodecByName
#define AMediaCodec_configure ms_hw_AMediaCodec_configure
#define AMediaCodec_start ms_hw_AMediaCodec_start
#define AMediaCodec_stop ms_hw_AMediaCodec_stop
#define AMediaCodec_flush ms_hw_AMediaCodec_flush
#define AMediaCodec_delete ms_hw_AMediaCodec_delete
#define AMediaCodec_dequeueInputBuffer ms_hw_AMediaCodec_dequeueInputBuffer
#define AMediaCodec_dequeueOutputBuffer ms_hw_AMediaCodec_dequeueOutputBuffer
#define AMediaCodec_getInputBuffer ms_hw_AMediaCodec_getInputBuffer
#define AMediaCodec_getOutputBuffer ms_hw_AMediaCodec_getOutputBuffer
#define AMediaCodec_getOutputFormat ms_hw_AMediaCodec_getOutputFormat
#define AMediaCodec_queueInputBuffer ms_hw_AMediaCodec_queueInputBuffer
#define AMediaCodec_releaseOutputBuffer ms_hw_AMediaCodec_releaseOutputBuffer
#define AMediaFormat_new ms_hw_AMediaFormat_new
#define AMediaFormat_delete ms_hw_AMediaFormat_delete
#define AMediaFormat_setString ms_hw_AMediaFormat_setString
#define AMediaFormat_setInt32 ms_hw_AMediaFormat_setInt32
#define AMediaFormat_setBuffer ms_hw_AMediaFormat_setBuffer
#define AMediaFormat_getInt32 ms_hw_AMediaFormat_getInt32

#ifdef AMEDIAFORMAT_KEY_MIME
#undef AMEDIAFORMAT_KEY_MIME
#endif
#ifdef AMEDIAFORMAT_KEY_SAMPLE_RATE
#undef AMEDIAFORMAT_KEY_SAMPLE_RATE
#endif
#ifdef AMEDIAFORMAT_KEY_CHANNEL_COUNT
#undef AMEDIAFORMAT_KEY_CHANNEL_COUNT
#endif
#define AMEDIAFORMAT_KEY_MIME "mime"
#define AMEDIAFORMAT_KEY_SAMPLE_RATE "sample-rate"
#define AMEDIAFORMAT_KEY_CHANNEL_COUNT "channel-count"

#endif /* __ANDROID__ */
#endif
