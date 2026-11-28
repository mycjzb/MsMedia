/*
 * FFmpeg AVIO 字节流：供 Media3 DataSource 拉 http(s)/tls/tcp/udp/rtp/rtmp/rtsp。
 * 不做 HLS/MPD 清单逻辑（交给 Media3）。
 */
#ifndef MSMEDIA_MS_FF_IO_H
#define MSMEDIA_MS_FF_IO_H

#include "msmedia/msmedia.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ms_ff_io ms_ff_io;

/* timeout_ms：连接+读；≤0 用 15000。 */
MSMEDIA_API ms_status ms_ff_io_open(ms_ff_io **out, const char *url, int timeout_ms);

/* headers：同 FFmpeg headers 选项格式，行以 \r\n 分隔，如 "Referer: https://a/\r\nUser-Agent: x"。
 * 仅 https 之外的取流协议用：rtmp 映射到 Connect AMF（Referer→rtmp_pageurl、swfUrl→rtmp_swfurl、
 * flashVer/Flash 形态 UA→rtmp_flashver），rtsp 只取 user_agent，udp/rtp 忽略。
 * headers 为 NULL 等价 ms_ff_io_open。 */
MSMEDIA_API ms_status ms_ff_io_open_ex(ms_ff_io **out, const char *url, int timeout_ms,
                                       const char *headers);
MSMEDIA_API void ms_ff_io_close(ms_ff_io *io);

/* >0 字节；0=EOF；<0 失败（见 ms_ff_io_last_err）。 */
MSMEDIA_API int ms_ff_io_read(ms_ff_io *io, uint8_t *buf, int size);

/* whence：0=SET 1=CUR 2=END 与 AVSEEK_* 一致；成功返回新偏移，失败 <0。 */
MSMEDIA_API int64_t ms_ff_io_seek(ms_ff_io *io, int64_t offset, int whence);

MSMEDIA_API int64_t ms_ff_io_size(ms_ff_io *io); /* <0 未知 */
MSMEDIA_API const char *ms_ff_io_last_err(const ms_ff_io *io);

#ifdef __cplusplus
}
#endif

#endif
