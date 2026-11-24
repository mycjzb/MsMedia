/*
 * 公共字节头探测 + 图片伪装头剥离（两条路线共用，内部接口，不进 C ABI v1）。
 * 不看 URL 后缀，只看字节：先剥 PNG(到 IEND+4B CRC)/JPEG(到 FFD9)/GIF(到 0x3B)/BMP(按文件长)/WebP(RIFF 长)
 * 伪装头，再在有界窗口内跳过前导脏字节，定位真正的 TS(188/192/204 连续同步)、ISO BMFF(ftyp/styp/moov/moof/
 * sidx/mdat/free/skip/emsg/prft)、FLV、MKV/EBML、MPEG-PS、HLS(#EXTM3U，含 BOM)、DASH(<MPD)、ADTS/MP3/ID3/AC3。
 * by: Long <mycjzb@gmail.com>
 */
#ifndef MS_PROBE_H
#define MS_PROBE_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef enum ms_probe_fmt {
    MS_PF_UNKNOWN = 0,
    MS_PF_TS,       /* 188 */
    MS_PF_TS192,    /* M2TS 4B 前缀 */
    MS_PF_TS204,    /* 带 16B RS */
    MS_PF_BMFF,     /* MP4 / fMP4 / m4s */
    MS_PF_FLV,
    MS_PF_MKV,
    MS_PF_PS,
    MS_PF_HLS,
    MS_PF_DASH,
    MS_PF_ADTS,
    MS_PF_MP3,
    MS_PF_ID3,
    MS_PF_AC3,
    MS_PF_LATM
} ms_probe_fmt;

typedef enum ms_probe_disguise {
    MS_PD_NONE = 0,
    MS_PD_PNG,
    MS_PD_JPEG,
    MS_PD_GIF,
    MS_PD_BMP,
    MS_PD_WEBP
} ms_probe_disguise;

typedef struct ms_probe_result {
    size_t offset;        /* 真正媒体起点（相对输入首字节） */
    size_t image_bytes;   /* 其中图片伪装头字节 */
    size_t junk_bytes;    /* 其中前导脏字节 */
    int fmt;              /* ms_probe_fmt */
    int disguise;         /* ms_probe_disguise */
} ms_probe_result;

/* 最多扫描的前导脏字节（防止把整段当脏数据吞掉）。 */
#define MS_PROBE_MAX_JUNK   (64u * 1024u)
/* 图片伪装头允许的最大字节（超出视为非伪装，交回调用方原样处理）。 */
#define MS_PROBE_MAX_IMAGE  (4u * 1024u * 1024u)

/*
 * 返回 1=已定位（r 有效）；0=还需要更多字节（eof=0 时）；-1=放弃（未识别，调用方原样透传 offset=0）。
 * eof=1 表示输入已全部给出，不会再返回 0。
 */
int ms_probe_payload(const uint8_t *d, size_t n, int eof, ms_probe_result *r);

/* 只判断 d 起点处的容器签名（强校验），不跳任何字节；不足以判断时返回 MS_PF_UNKNOWN。 */
int ms_probe_format_at(const uint8_t *d, size_t n, int eof, int *need_more);

const char *ms_probe_fmt_name(int fmt);
const char *ms_probe_disguise_name(int d);

/* FFmpeg hls.c 补丁调用的 C 入口（单 so 内解析）：返回媒体起点偏移；need_more=1 时调用方继续读。 */
int ms_probe_media_offset(const uint8_t *d, int n, int eof, int *need_more);

#ifdef __cplusplus
}
#endif
#endif
