/*
 * 已解码 s16 PCM 的转单声与线性重采样。不是解码器，不接触压缩码流。
 * by: Long <mycjzb@gmail.com>
 */
#include "msmedia/ms_ai_sub.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

ms_status ms_ai_pcm_s16_downmix_mono(const int16_t *in, int channels,
                                     size_t nframes, int16_t *out,
                                     size_t *out_nframes)
{
    size_t i;
    int c;
    if (out_nframes != NULL) {
        *out_nframes = 0;
    }
    if (in == NULL || out == NULL || channels <= 0 || nframes > (SIZE_MAX / 8u)) {
        return MS_ERR_INVALID_ARG;
    }
    if (channels == 1) {
        if (in != out) {
            memcpy(out, in, nframes * sizeof(int16_t));
        }
        if (out_nframes != NULL) {
            *out_nframes = nframes;
        }
        return MS_OK;
    }
    for (i = 0; i < nframes; i++) {
        int32_t acc = 0;
        for (c = 0; c < channels; c++) {
            acc += in[i * (size_t)channels + (size_t)c];
        }
        if (acc >= 0) {
            out[i] = (int16_t)((acc + channels / 2) / channels);
        } else {
            out[i] = (int16_t)((acc - channels / 2) / channels);
        }
    }
    if (out_nframes != NULL) {
        *out_nframes = nframes;
    }
    return MS_OK;
}

ms_status ms_ai_pcm_s16_resample_linear(const int16_t *in, size_t nin,
                                        int in_rate, int out_rate,
                                        int16_t *out, size_t out_cap,
                                        size_t *nout)
{
    size_t i, want;
    if (nout != NULL) {
        *nout = 0;
    }
    if (in == NULL || out == NULL || in_rate <= 0 || out_rate <= 0) {
        return MS_ERR_INVALID_ARG;
    }
    if (nin == 0) {
        return MS_OK;
    }
    if (in_rate == out_rate) {
        if (out_cap < nin) {
            return MS_ERR_OVERFLOW;
        }
        if (in != out) {
            memcpy(out, in, nin * sizeof(int16_t));
        }
        if (nout != NULL) {
            *nout = nin;
        }
        return MS_OK;
    }
    /* want = floor((nin-1) * out / in) + 1，覆盖首末样点。 */
    want = (size_t)(((uint64_t)(nin - 1u) * (uint64_t)out_rate) / (uint64_t)in_rate) + 1u;
    if (want > out_cap) {
        return MS_ERR_OVERFLOW;
    }
    for (i = 0; i < want; i++) {
        uint64_t pos = ((uint64_t)i * (uint64_t)in_rate);
        size_t idx = (size_t)(pos / (uint64_t)out_rate);
        uint64_t frac = pos % (uint64_t)out_rate;
        int16_t a, b;
        int32_t v;
        if (idx >= nin) {
            idx = nin - 1u;
        }
        a = in[idx];
        b = (idx + 1u < nin) ? in[idx + 1u] : a;
        if (frac == 0) {
            out[i] = a;
        } else {
            v = (int32_t)a * (int32_t)((uint64_t)out_rate - frac) + (int32_t)b * (int32_t)frac;
            if (v >= 0) {
                out[i] = (int16_t)((v + out_rate / 2) / out_rate);
            } else {
                out[i] = (int16_t)((v - out_rate / 2) / out_rate);
            }
        }
    }
    if (nout != NULL) {
        *nout = want;
    }
    return MS_OK;
}
