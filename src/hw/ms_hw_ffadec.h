/*
 * 路线 B 单 so：AC-3 / E-AC-3 / MP2 FFmpeg 轻量软解到 s16 PCM（仅系统无解码器时）。
 * by: Long <mycjzb@gmail.com>
 */
#ifndef MS_HW_FFADEC_H
#define MS_HW_FFADEC_H
#include "msmedia/msmedia.h"
#include <stddef.h>
#include <stdint.h>

typedef struct ms_ffadec ms_ffadec;
int  ms_ffadec_supported(ms_codec codec);
int  ms_ffadec_open(ms_codec codec, int rate, int ch, ms_ffadec **out);
int  ms_ffadec_out_rate(const ms_ffadec *d);
int  ms_ffadec_out_channels(const ms_ffadec *d);
int  ms_ffadec_decode(ms_ffadec *d, const uint8_t *au, size_t n, const uint8_t **pcm, size_t *pcm_n);
void ms_ffadec_flush(ms_ffadec *d);
void ms_ffadec_close(ms_ffadec *d);
#endif
