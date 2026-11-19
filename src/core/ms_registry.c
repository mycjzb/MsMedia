/*
 * 目标 codec 注册表与硬解查询。
 * 级别必须反映本构建真实能力，不能因为枚举里有名字就报可组帧。
 * by: Long <mycjzb@gmail.com>
 */

#include "ms_internal.h"

/*
 * 本构建实际能力：
 *   - MPEG-TS stream_type -> 容器标识（多数目标停在 CONTAINER_ID）
 *   - AAC：仅 MPEG-TS 上的 ADTS 子集可完整组帧（见 ms_adts.h），
 *     不是 LATM/裸 AAC/全部 AAC；ms_codec_query 对 AAC 返回 AU
 *   - H.264：仅 MPEG-TS 上 Annex B 子集可完整组帧（见 ms_h264.h）：
 *     AUD 或无 AUD 时的 7.4.1.2.4 主图像比较；不是全部 H.264
 *     （隔行/PAFF/MBAFF、FMO、未知参数集均拒绝）
 * 没有 TS 标识规则、或标识依赖尚未实现的容器/协议的目标，保持 NONE。
 */
ms_status ms_codec_query(ms_codec codec, ms_recog_level *out_level)
{
    ms_recog_level level = MS_RECOG_NONE;

    if (out_level == NULL) {
        return MS_ERR_INVALID_ARG;
    }

    switch (codec) {
    case MS_CODEC_AAC:           /* 仅 TS ADTS 子集，不是全部 AAC */
        level = MS_RECOG_AU;
        break;
    case MS_CODEC_H264:          /* 仅 TS Annex B 声明子集，不是全部 H.264 */
        level = MS_RECOG_AU;
        break;
    case MS_CODEC_HEVC:          /* TS 0x24 */
    case MS_CODEC_MPEG4_PART2:   /* TS 0x10 */
    case MS_CODEC_MPEG2_VIDEO:   /* TS 0x02 */
    case MS_CODEC_AVS_FAMILY:    /* TS 0x42 常见标识，不是 AVS+ */
    case MS_CODEC_AC3:           /* 常见 ATSC 私有 stream_type 0x81 */
    case MS_CODEC_EAC3:          /* 常见 ATSC 私有 stream_type 0x87 */
    case MS_CODEC_MPEG_AUDIO:    /* TS 0x03/0x04 */
        level = MS_RECOG_CONTAINER_ID;
        break;
    case MS_CODEC_AVS_PLUS:      /* 需要序列头档次，本阶段不做 */
    case MS_CODEC_H263:          /* 需要 FLV 等，本阶段无该容器 */
    case MS_CODEC_AV1:
    case MS_CODEC_MP3:           /* 未确认 layer，不得从 0x03/0x04 直接给出 */
    case MS_CODEC_MP2:
    case MS_CODEC_OPUS:
    case MS_CODEC_G711_ALAW:
    case MS_CODEC_G711_ULAW:
    case MS_CODEC_UNKNOWN:
        level = MS_RECOG_NONE;
        break;
    default:
        *out_level = MS_RECOG_NONE;
        return MS_ERR_INVALID_ARG;
    }

    *out_level = level;
    return MS_OK;
}

ms_status ms_hw_query(ms_codec codec,
                      int *is_hardware,
                      int *verified,
                      const char **backend,
                      const char **detail)
{
    (void)codec;
    if (is_hardware == NULL || verified == NULL) {
        return MS_ERR_INVALID_ARG;
    }
    *is_hardware = 0;
    *verified = 0;
    if (backend != NULL) {
        *backend = "none";
    }
    if (detail != NULL) {
        *detail = "no platform hardware decoder in this build";
    }
    return MS_OK;
}

/* stream_type -> 种类/codec。无法判断时 codec=UNKNOWN。0x42 不得映射为 AVS+。 */
void ms_ts_map_stream_type(uint8_t stream_type,
                           ms_codec *codec,
                           ms_track_kind *kind,
                           ms_recog_level *level)
{
    ms_codec c = MS_CODEC_UNKNOWN;
    ms_track_kind k = MS_TRACK_UNKNOWN;
    ms_recog_level lv = MS_RECOG_NONE;

    switch (stream_type) {
    case 0x02:
        c = MS_CODEC_MPEG2_VIDEO;
        k = MS_TRACK_VIDEO;
        lv = MS_RECOG_CONTAINER_ID;
        break;
    case 0x03:
    case 0x04:
        c = MS_CODEC_MPEG_AUDIO;
        k = MS_TRACK_AUDIO;
        lv = MS_RECOG_CONTAINER_ID;
        break;
    case 0x0F:
        c = MS_CODEC_AAC;
        k = MS_TRACK_AUDIO;
        lv = MS_RECOG_CONTAINER_ID;
        break;
    case 0x10:
        c = MS_CODEC_MPEG4_PART2;
        k = MS_TRACK_VIDEO;
        lv = MS_RECOG_CONTAINER_ID;
        break;
    case 0x1B:
        c = MS_CODEC_H264;
        k = MS_TRACK_VIDEO;
        lv = MS_RECOG_CONTAINER_ID;
        break;
    case 0x24:
        c = MS_CODEC_HEVC;
        k = MS_TRACK_VIDEO;
        lv = MS_RECOG_CONTAINER_ID;
        break;
    case 0x42:
        c = MS_CODEC_AVS_FAMILY;
        k = MS_TRACK_VIDEO;
        lv = MS_RECOG_CONTAINER_ID;
        break;
    case 0x81:
        c = MS_CODEC_AC3;
        k = MS_TRACK_AUDIO;
        lv = MS_RECOG_CONTAINER_ID;
        break;
    case 0x87:
        c = MS_CODEC_EAC3;
        k = MS_TRACK_AUDIO;
        lv = MS_RECOG_CONTAINER_ID;
        break;
    default:
        break;
    }

    if (codec != NULL) {
        *codec = c;
    }
    if (kind != NULL) {
        *kind = k;
    }
    if (level != NULL) {
        *level = lv;
    }
}
