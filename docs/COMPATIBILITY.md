# 兼容性参考归类

by: Long <mycjzb@gmail.com>

来源是一张兼容性清单参考图，用作目标输入。**每一项都未实现、未验证。** 记录将来的用例，不保证所有运营商或所有服务端。

归类四层。列表、EPG、回看模板不进入核心。udpxy、msd_lite、rtp2httpd 通常是把组播转成 HTTP 的服务；客户端只消费它们给出的 HTTP，不在客户端里再实现这些服务端。

加密和 DRM 没有实现、也没有授权时返回不支持。解析到容器不等于解密。

音频能识别不等于设备硬解。硬解查询失败时不软解兜底。图中视频只列了 H.264/HEVC，不替换 `docs/DESIGN.md` 里的完整 codec 表。E-AC-3 和 G.711 是新增目标，仍未实现。

| 项 | 归类 | 阶段 | 验证条件（现在都未测） |
| --- | --- | --- | --- |
| HTTP(S) 直连 MPEG-TS | 核心解析 TS；网络走第 7 节 backend | P1 容器 + 网络阶段 | 合成或自有 TS；HTTPS 校验证书和主机名 |
| HLS TS / fMP4 | 清单调度，不是容器 demux | P3，依赖 P1/P2 | 点播、直播、初始化段、不连续 |
| HLS 加密 | 可选业务；无密钥即不支持 | 未排期 | 与 AES-128、SAMPLE-AES、CENC 分开报 |
| HTTP-FLV | 核心解析 FLV | P2 | tag、sequence header、时间戳 |
| MP4 点播 | 核心解析 MP4 子集 | P2 | moov 位置、Range、关键帧 seek |
| RTMP、RTMPS | 协议插件 | P3 | RTMPS 走共用 TLS，不另链 OpenSSL |
| Enhanced RTMP（HEVC） | 协议插件；HEVC 组帧在 P4 | P3 承载，P4 才组帧 | 未组帧前不得报 HEVC 可播 |
| RTSP、RTSPS、TCP/UDP | 协议插件 | P3 | 重定向不跨主机带凭证；Digest 不写进日志 |
| MPEG-TS over RTP | 协议插件，然后 TS 核心 | P3 | 重排/丢包测试不叫做容器 demux |
| UDP/RTP 组播、SSM、UDP 单播 | 协议插件 | P3 | 指定源地址；离开组播组即取消 |
| FCC | 可选插件 | 未单列，见设计第 17 节 | 突发到组播、去重、超时回退 |
| HTTP 302、RTSP 重定向、自定义跳转 | 网络适配 | 与所选 HTTP/RTSP backend 一起 | 次数上限；跨主机去掉凭据 |
| 按主机设置请求头、自定义 UA/Referer | 网络适配 | 同上 | 日志不记完整鉴权头 |
| 运营商 IPTV 网络 | 网络环境用例 | 未验证 | 单独样例，不写成通用兼容 |
| udpxy、msd_lite、rtp2httpd | 网络适配：当作上游 HTTP | 随 HTTP backend | 不嵌入这些服务端 |
| H.264/AVC，含 PAFF、MBAFF、1080i | 核心解析目标之一 | P1d：AUD 或 7.4.1.2.4 无 AUD 的逐行子集（宿主自测已过，非正式验收）；PAFF/MBAFF/FMO 明确拒绝 | 不得把该子集写成全部 H.264 或隔行可安全出画；组帧不等于硬解 |
| HEVC Main10、4K 50/60 | 核心解析目标；硬解另查 | 组帧 P4；硬解看设备 | 档次/等级/帧率写进查询，不靠宣传数字 |
| AAC ADTS、LATM/LOAS、5.1 | 核心解析 | P1b 仅 TS+ADTS 子集组帧（宿主自测已过，非正式验收）；LATM/PCE/多块/5.1 扩展仍不做 | 5.1 识别成功仍要硬解查询；组帧不等于硬解或全部 AAC |
| MP2、MP3、AC-3、E-AC-3 | 核心解析 | P4 | E-AC-3 不得标成 AC-3 |
| G.711 A-law / μ-law | 核心解析目标 | P4 | 识别不等于硬解 |
| m3u/m3u8、TXT `#genre#`、XMLTV/gzip、tvg 字段、同名多线路、多份列表 | 可选业务扩展 | 不进核心，未排期 | 样例播放列表；失败不影响 demux |
| 回看 default/append/shift/timeshift、flussonic、xc、vod | 可选业务扩展 | 不进核心，未排期 | 每种模板单独用例；无模板则不支持 |
| RTSP Basic/Digest | 协议插件的认证 | P3 才可能 | 只用 HTTPS/RTSPS 或同等保护的通道传递；重定向跨主机不转发 |
