#!/usr/bin/env python3
# 精简 FFmpeg（single-slimff）源码副本补丁：去掉 ASF/WMV 与 RM/RMVB 后，让 RTSP/RTP 仍可用。
# 上游 configure 里 rtpdec_select 强制选中 asf_demuxer/rm_demuxer（RTSP 里的 WMS-ASF 负载与 Real RDT）。
# 这里只改 build/single-analysis/ffmpeg-9.1-slimff-src 副本：
#   1) configure: rtpdec_select 去掉 asf_demuxer rm_demuxer；
#   2) rtpdec.c: ASF/RDT 动态负载处理器按 CONFIG_ASF_DEMUXER / CONFIG_RM_DEMUXER 登记；
#   3) rdt.c / rtpdec_asf.c: 对应 demuxer 关闭时只留诚实失败的桩（Real/WMS 的 RTSP 流打不开，返回错误）。
# 幂等：已打过补丁则跳过。不改官方 build/ffmpeg-9.1 与冻结前缀。
# by: Long <mycjzb@gmail.com>
import sys, pathlib

src = pathlib.Path(sys.argv[1])
MARK = "/* msmedia-noasfrm */"

def patch(rel, fn):
    p = src / rel
    t = p.read_text()
    if MARK in t:
        return
    n = fn(t)
    if n == t:
        sys.exit("noasfrm: patch did not apply: " + rel)
    p.write_text(n)

def cfg(t):
    old = 'rtpdec_select="asf_demuxer mov_demuxer mpegts_demuxer rm_demuxer rtp_protocol srtp"'
    assert old in t
    return t.replace(old, 'rtpdec_select="mov_demuxer mpegts_demuxer rtp_protocol srtp" # msmedia-noasfrm', 1)

p = src / "configure"
t = p.read_text()
if "msmedia-noasfrm" not in t:
    p.write_text(cfg(t))

def rtpdec(t):
    t = t.replace('#include "libavutil/mathematics.h"', MARK + '\n#include "config_components.h"\n#include "libavutil/mathematics.h"', 1)
    for a, b in [
        ("extern const RTPDynamicProtocolHandler ff_rdt_video_handler;\n"
         "extern const RTPDynamicProtocolHandler ff_rdt_audio_handler;\n"
         "extern const RTPDynamicProtocolHandler ff_rdt_live_video_handler;\n"
         "extern const RTPDynamicProtocolHandler ff_rdt_live_audio_handler;\n", "CONFIG_RM_DEMUXER"),
        ("    &ff_ms_rtp_asf_pfa_handler,\n    &ff_ms_rtp_asf_pfv_handler,\n", "CONFIG_ASF_DEMUXER"),
        ("    &ff_rdt_video_handler,\n    &ff_rdt_audio_handler,\n"
         "    &ff_rdt_live_video_handler,\n    &ff_rdt_live_audio_handler,\n", "CONFIG_RM_DEMUXER"),
    ]:
        assert a in t, a
        t = t.replace(a, "#if " + b + "\n" + a + "#endif\n", 1)
    return t
patch("libavformat/rtpdec.c", rtpdec)

RDT_STUB = r'''
#else /* !CONFIG_RM_DEMUXER：RM/RMVB 已裁掉，Real RDT over RTSP 诚实不支持 */
RDTDemuxContext *ff_rdt_parse_open(AVFormatContext *ic, int first_stream_of_set_idx,
                                   void *priv_data, const RTPDynamicProtocolHandler *handler)
{
    return NULL;
}
void ff_rdt_parse_close(RDTDemuxContext *s)
{
}
void ff_rdt_calc_response_and_checksum(char response[41], char chksum[9], const char *challenge)
{
    response[0] = 0;
    chksum[0] = 0;
}
void ff_rdt_subscribe_rule(char *cmd, int size, int stream_nr, int rule_nr)
{
}
int ff_rdt_parse_header(const uint8_t *buf, int len, int *pset_id, int *pseq_no,
                        int *pstream_id, int *pis_keyframe, uint32_t *ptimestamp)
{
    return AVERROR_PATCHWELCOME;
}
int ff_rdt_parse_packet(RDTDemuxContext *s, AVPacket *pkt, uint8_t **buf, int len)
{
    return AVERROR_PATCHWELCOME;
}
void ff_real_parse_sdp_a_line(AVFormatContext *s, int stream_index, const char *buf)
{
}
#endif
'''

def rdt(t):
    t = t.replace('#include "avformat.h"', MARK + '\n#include "config_components.h"\n#include "avformat.h"', 1)
    t = t.replace('#include "rdt.h"\n', '#include "rdt.h"\n#if CONFIG_RM_DEMUXER\n', 1)
    return t.rstrip("\n") + "\n" + RDT_STUB
patch("libavformat/rdt.c", rdt)

ASF_STUB = r'''
#else /* !CONFIG_ASF_DEMUXER：ASF/WMV 已裁掉，WMS-ASF over RTSP 诚实不支持 */
#include "rtpdec_formats.h"
int ff_wms_parse_sdp_a_line(AVFormatContext *s, const char *p)
{
    return AVERROR_PATCHWELCOME;
}
#endif
'''

def asf(t):
    old = '#include "libavutil/base64.h"'
    assert old in t
    t = t.replace(old, MARK + '\n#include "config_components.h"\n#if CONFIG_ASF_DEMUXER\n' + old, 1)
    return t.rstrip("\n") + "\n" + ASF_STUB
patch("libavformat/rtpdec_asf.c", asf)
print("noasfrm: applied")
