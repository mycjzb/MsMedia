# FFmpeg 9.1 裁剪后端迁移设计（第一里程碑）

by: Long <mycjzb@gmail.com>

状态日期：2026-11-21 11:58 CST（Asia/Shanghai）。11:58 完成本机 RTSP/RTP 帧合同与 RTMP 拉流联调；21:21 CST 为版本钉选+本地/HLS/HTTPS；第 6 节为 22:07 CST 协议联调；第 8 节为正确性修正（时间戳 origin、TIMEOUT、DASH trun 尾包）。用户授权依据 [pt67u]：因担心自写协议不完整，明确要求常用 RTSP / RTMP / UDP / RTP / HTTP / HTTPS 都支持，并「使用最新的 ffmpeg9 剪裁优化」。这不是助手建议。先前口头答复（保留统一 C 接口 + 系统硬解，取流/协议/解封装改 FFmpeg，且先核实官方 9 版本）在本步落地为可验证实物。

本文件是迁移设计。实现范围以本文「已验证 / 未验证」为准，不以 configure 开启冒充联调完成。

## 1. 官方版本实证（不以系统包或日期推断为准）

2026-11-20 核对官方页面 [download.html](https://ffmpeg.org/download.html) 与 [releases](https://ffmpeg.org/releases/)：

| 项 | 官方事实 |
| --- | --- |
| 最新稳定版 | **FFmpeg 9.1 “Gauss”**，不是 master，不是 9.0.2，不是宿主机系统包 7.1.5 |
| 发布 | 2026-11-20；9.1 分支于 2026-10-24 从 master 切出 |
| 库版本（下载页） | libavutil 61.19.100，libavcodec 63.24.100，libavformat 63.16.100 |
| tarball | https://ffmpeg.org/releases/ffmpeg-9.1.tar.xz（12557532 字节） |
| 签名 | 同目录 `ffmpeg-9.1.tar.xz.asc`；PGP 签名时间为 2026-11-20 08:18:20 CST |
| 官方签名公钥 | https://ffmpeg.org/ffmpeg-devel.asc ，指纹 `FCF986EA15E6E293A5644F10B4322F04D67658D8`（uid：FFmpeg release signing key） |
| 官方校验方式 | PGP（FFmpeg **不**发布官方 SHA-256） |
| 本地钉选 SHA-256 | `1a4d394ae63d190a526cd052eb4415c156b5c5e2df37efab41a7de0b8fde2433`（在 `gpg --verify` 报 Good signature 之后计算，写入 `third_party/ffmpeg-9.1.pin`） |
| 解包 `RELEASE` | `9.1` |
| `FFMPEG_VERSION` | `"9.1"`（prefix `include/libavutil/ffversion.h` / `av_version_info()`） |

当时官网首页新闻尚未刊登 9.1 公告（仍可见 11 月 10 日 CI 与 3 月 16 日 8.1 新闻）。**以 download.html 与 /releases/ 为权威发布源**，不以缺失首页新闻否定 9.1，也不把 master 冒充 9。

`gpg --verify` 为 Good signature，密钥来自官方 URL 且指纹与 download.html 一致。首次导入会出现「密钥未经认证 / 无法验证签名属于其声称所有者」的 GnuPG 常规信任提示，已记录，未因此跳过校验。

本机构建曾安装 `make`、`pkg-config`、`nasm`、`zlib1g-dev`、`libxml2-dev` 仅供授权的宿主自测；不替代 el7 工具链。

## 2. 架构变更（替换旧规则，保留历史事实）

直至 2026-11-20 20:34 CST，产品规则是「`libmsmedia` 不链 FFmpeg」，自写 HLS/MPD/`libmsnet` 继续扩展。该规则被用户本次明确决定替换，但 **20:00 / 20:25 / 20:34 等历史记载保持不变**，现有自写代码与未提交改动 **不删除**，作为回归对照。

新分层：

- **`libmsmedia`**：仍为自写核心与稳定 `ms_session` C ABI（`MSMEDIA_1.0`）。链接行只有 `libc`。不把 FFmpeg 链进核心。
- **`libmsnet`**：可选自写 HTTP/HTTPS，保持关闭开关与回归。
- **`libmsff`（新，可选）**：隔离的 FFmpeg 9.1 裁剪后端。把 URL 取流 / 协议 / 解封装映射到公共 `ms_event`。`ENABLE_FFMPEG=0` 可整项关闭。静态链入本树钉选的 `libavformat.a` / `libavcodec.a` / `libavutil.a`，以及钉选的官方 OpenSSL 3.5.9 LTS PIC 静态库（自 2026-11-21 11:26 CST 起；此前曾动态链系统 OpenSSL 3.5.7，见历史记载）。libxml2 与 zlib **仍为系统动态库**。测试与 `.so` 的链接行使用 **绝对路径 prefix**，禁止误链系统 `libavformat.so.61` 或系统 `libssl`/`libcrypto`。
- **系统硬解**：仍走未来平台桥；本里程碑不做软解、不声称硬解出画，不承诺 AMediaCodec 压缩输入全链路零拷贝。
- 自写 HLS/DASH/网络 **停止扩展**，保留源码与 42 组 + `test_net` 回归。

公开 ABI：保留 `ms_session_*`。`ms_format` **在末尾追加** `MS_FORMAT_HLS=6`、`MS_FORMAT_DASH=7`（旧字段偏移不变）。旧核心本步不发出这两种格式；可选后端在识别到 hls/dash demuxer 时发出。

## 3. 裁剪原则与实际 configure

入口：`./build.sh ffmpeg` 或默认构建在未设 `ENABLE_FFMPEG=0` 时调用 `scripts/ffmpeg9_build.sh`。下载按 pin 校验，**不自动升级**。完整 flags 归档在 `build/ffmpeg-9.1/configure-flags.txt`（不入库的构建树）与本文件。

默认 **禁止** `--enable-gpl`、`--enable-version3`、`--enable-nonfree`、`--enable-librtmp`。本构建实测：

```
License: LGPL version 2.1 or later
CONFIG_GPL 0
CONFIG_NONFREE 0
CONFIG_DECODERS 0
avformat_license() = "LGPL version 2.1 or later"
```

`--disable-everything` 后只打开 avutil/avcodec/avformat、network、openssl、libxml2，以及下列协议/解复用/解析/bitstream filter；关闭 programs、docs、avdevice、avfilter、swscale、swresample、全部 decoder/encoder/muxer/filter/hwaccel/indev/outdev。保留 parsers/bsfs，因为 `AVPacket` 不一定等于完整 AU，组帧与 extradata 需要它们，但 **不是软件解码器**。

本机构建实际开启（见 `build/ffmpeg-9.1/enabled-components.txt`）：

- 协议：file, crypto, fd, pipe, http, https, tls, tcp, udp, rtp, srtp, rtmp, rtmps, rtmpt，以及 RTMP 依赖自动带上的 **ffrtmphttp**。
- 解复用器：mpegts, mpegtsraw, flv, live_flv, mov（覆盖 MP4/MOV/fMP4）, hls, dash, rtsp, rtp, sdp, aac, h264，以及 2026-11-21 23:21 CST 起按需追加的 **ogg**（Opus 最小路径；format 白名单同步加入 `ogg`）。RTSP/RTP 依赖还自动带上了 **ac3 / eac3 / asf / rm** demuxer（不是 decoder；asf/rm **不**宣传为产品功能）。仍未开 matroska，也未把 raw ac3 写入白名单。
- parser：h264, hevc, mpegvideo, mpeg4video, h263, av1, aac, aac_latm，以及被依赖拉入的 **ac3** parser。2026-11-21 21:10 CST 起只追加 HEVC parser（不是 decoder）；2026-11-21 22:16 CST 起再追加 `mpegvideo` 与 `mpeg4video`（FFmpeg 9 组件名，不是 `mpeg4`）；2026-11-21 22:34 CST 起再追加 `h263` parser；2026-11-21 22:48 CST 起再追加 `av1` parser。`CONFIG_H263_DECODER=0`、`CONFIG_AV1_DECODER=0`、`CONFIG_LIBDAV1D=0`，符号表有 `ff_h263_parser`/`ff_av1_parser`、无对应 decoder。仍 `CONFIG_DECODERS=0`、`CONFIG_GPL=0`、`CONFIG_NONFREE=0`。未开 avi 或 matroska demuxer。2026-11-21 23:11 CST 起适配器对 AC-3 走合法无 CSD（不把容器 extradata 当 CSD），**未改** parser/demuxer 清单（既有 ac3 parser；mpegts/mov 已开）。未把 raw ac3 写入 format 白名单。2026-11-21 23:14–23:21 CST 起再追加 `mpegaudio` 与 `opus` parser，以及 `ogg` demuxer。`CONFIG_MPEGAUDIO_PARSER=1`、`CONFIG_OPUS_PARSER=1`、`CONFIG_OGG_DEMUXER=1`，`CONFIG_OPUS_DECODER=0`、`CONFIG_MP3_DECODER=0`、`CONFIG_VORBIS_DECODER=0`、`CONFIG_FLAC_DECODER=0`，符号表有 `ff_mpegaudio_parser`/`ff_opus_parser`/`ff_ogg_demuxer`、无对应 decoder。oggdec 为 MIT 头，oggparse* 与 opus parser 为 LGPL；启用 ogg 会编入 oggparse 辅助对象，但不打开 vorbis/flac 解码器。仍 `CONFIG_DECODERS=0`、`CONFIG_GPL=0`、`CONFIG_NONFREE=0`。未开 avi 或 matroska demuxer。宿主 `libavcodec.a` 在 AV1 切片 14,628,742 之上变为 15,080,200（先 mpegaudio 至 14,758,360，再 opus parser）；`libavformat.a` 12,750,778 → 13,806,232。Android size-opt `libavcodec.a` v7a 8,442,924 → 8,750,762，v8a 12,375,756 → 12,808,022；`libavformat.a` v7a 8,242,610 → 8,944,708，v8a 12,090,482 → 13,118,904。 2026-11-22 00:07–00:10 CST 起再追加 `cavsvideo` parser（AVS+ 广播类组帧，不是解码器）。`CONFIG_CAVSVIDEO_PARSER=1`、`CONFIG_CAVS_DECODER=0`，符号表有 `ff_cavsvideo_parser`、无 `ff_cavs_decoder`；前缀仅 `cavs_parser.o`（LGPL 2.1）。官方 cavs 解码器只支持基准类 `0x20`，即使开启也不能解码广播类 AVS+，本产品禁止启用。仍 `CONFIG_DECODERS=0`、`CONFIG_GPL=0`、`CONFIG_NONFREE=0`。宿主 `libavcodec.a` 15,080,200 → 15,177,428；`libavformat.a` 约 13,806,240。Android size-opt `libavcodec.a` v7a 8,750,762 → 8,812,690，v8a 12,808,022 → 12,898,674。未开 cavsvideo demuxer/muxer，也未把 AVS2/AVS3 当 AVS+。

- 2026-11-22 00:49–00:59 CST 起按需追加 **matroska** demuxer（`CONFIG_MATROSKA_DEMUXER=1`，`CONFIG_MATROSKA_MUXER=0`）。同一 `FFInputFormat` 名为 `matroska,webm`，扩展名含 mkv/webm/mka/mks/mk3d；产品白名单加入 `matroska,webm`。matroskadec.c 与辅助对象（riffdec、flac_picture、rmsipr、vorbiscomment、qtpalette、replaygain、dovi_isom）为 LGPL 2.1；oggparsevorbis 为 MIT 且本已随 ogg 编入。`matroska_demuxer_suggest` 的 zlib 已开，**未开 bzlib**。仍 `CONFIG_DECODERS=0`、`CONFIG_GPL=0`、`CONFIG_NONFREE=0`，无字幕/视频/音频 decoder。符号表有 `ff_matroska_demuxer`，无 muxer。宿主 `libavformat.a` 13,806,240 → **14,236,440**（+430,200）；`libavcodec.a` 基本不变 15,177,428 → 15,177,444。Android size-opt `libavformat.a` v7a 8,944,720 → **9,243,416**（+298,696），v8a 13,118,912 → **13,546,824**（+427,912）。WebM 仅用已支持 Opus 验证绑定，不宣传 VP8/VP9/Vorbis。FFmpeg 9.1 对本类 MKV 常把容器 `start_time` 记为 0，AAC priming 在 `codecpar->initial_padding`；`choose_time_origin` 纳入该字段，避免首个视频 AU 发出后负 PTS 被标 `OVERFLOW`。

- 2026-11-21 23:51 CST 起，适配器消费 `AV_PKT_DATA_SKIP_SAMPLES`（u32le 起始/尾部样点）并写入公共事件，不改 dts。有 side data 时不再用已缩短的 `pkt->duration` 推断，以免与 Ogg 双扣。本树 FFmpeg 对部分 MP4 末包不带 end SKIP、只缩短 duration：仅在完全没有 side data 且 duration&lt;TOC 时用差值作 end skip（已覆盖 1s/50ms 受控 fixture）。25ms 级 MP4 出现 SKIP end=1，不把它当合同，也不另造补偿公式。中途 MP4 seek 落在 priming 区以外时 FFmpeg **不**再给 start skip=312，seek preroll 由播放器层按 RFC 默认 3840 武装。Ogg 中途 seek 受页粒度限制。本切片未再改 parser/demuxer 清单，仍 `CONFIG_DECODERS=0`。
- bsf：extract_extradata, h264_mp4toannexb, hevc_mp4toannexb, aac_adtstoasc。
- DASH XML：`dash_demuxer_deps=libxml2`，`--enable-libxml2`。本机 libxml2 2.9.14（MIT 风格）。未重造 XML 解析。
- OpenSSL：`--enable-openssl`，指向本树钉选的官方 OpenSSL 3.5.9 LTS 静态 prefix（Apache-2.0；见第 9 节）。未启用 gnutls / mbedtls / librtmp。不得混用系统 OpenSSL 头/库。

TLS：FFmpeg 9.1 `tls.h` 中 `tls_verify` / `verify` 缺省为 1。`tls_openssl.c` 在校验开启时使用 `SSL_VERIFY_PEER|SSL_VERIFY_FAIL_IF_NO_PEER_CERT` 与 `SSL_set1_host`（`X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS`）。适配器 **额外强制写入 verify=1**，提供 `ms_ff_set_ca_file` 只增加信任锚，永不把 verify 设为 0。HTTP 跟随 Location 在协议内部完成，适配器在 `io_open` 之后读取 `location` 选项：一旦会话见过 https，拒绝 http 目标（含 302 降级与 https 清单下的 http 子资源）。无 https→http 绕过。

探测：不盲缩 `probesize`（官方默认 5000000）与 `analyzeduration`。短合成 TS 在默认参数下可能只有低 probe 分，且无 decoder 时 `codecpar` 可能缺采样率；`avformat_find_stream_info` 仍可返回 0，包仍可读。`ms_ff_stream_info_err` 暴露该返回码，禁止靠偷偷开软解来「补全」参数后再声称无软解。

包合同：按容器映射 `sample_format`（TS：AnnexB / ADTS；MP4/MOV/FLV：AVCC / AAC_RAW），PTS/DTS 使用流 `time_base`，不把缺失时间戳写成 0。关键帧传播 `AV_PKT_FLAG_KEY`（H.264 不保证可安全起播）。本阶段按 `AVPacket` 边界输出，**不把 packet 自动称为完整 AU**。所有权：拷入有界槽后 `av_packet_unref`；调用方必须 `ms_ff_release`。槽满或队列超限返回 `MS_LIMIT`。支持 cancel / flush / reset。打开超时用 `timeout`/`rw_timeout` 与 interrupt callback；本地长文件在打开完成后不保留总截止时间，以免被误杀。

## 4. 许可证（不代选 MsMedia 顶层许可证）

| 组件 | 本构建条款 | 分发义务（事实，不是法律意见） |
| --- | --- | --- |
| FFmpeg 9.1 裁剪静态库 | 官方 `LICENSE.md` 默认 LGPL v2.1+；本构建 `FFMPEG_LICENSE` 为 "LGPL version 2.1 or later"。未开 GPL/nonfree。官方称 OpenSSL「据其所知与 LGPL 兼容」，并与 GPLv2/v3 不兼容（故更不能开 GPL） | 动态或静态分发均须保留版权与许可声明，并提供 LGPL 源码（或书面有效要约）。**静态**链入 `libmsff.so` 时，通常还须提供目标文件或等价再链接条件，使接收者能用修改后的 LGPL 库再链接 |
| 钉选 OpenSSL 3.5.9 LTS（自 2026-11-21 11:26 CST 静态链入 `libmsff`） | Apache-2.0，PIC 静态 `libssl.a`/`libcrypto.a` | 保留 Apache-2.0 声明与 NOTICE；静态合入后，分发 `libmsff` 时通常须提供对应源码（或书面要约）及归属。与 FFmpeg LGPL 2.1 的组合需诚实披露，不能声称「没有任何开源义务」。旧 `libmsnet` 若保留，仍动态链系统 OpenSSL（本机 3.5.7），义务形态不同，见 `docs/THIRD_PARTY.md` |
| 系统 libxml2 2.9.14 | MIT 风格，动态链接 | 保留版权与许可声明 |
| zlib 等系统间接依赖 | 跟随发行版 | 按各组件条款 |

**不能**宣称 MsMedia 顶层许可证已选定，也不能宣称本组合「无任何开源义务」。用户若日后要改 FFmpeg 为共享库分发，义务形态会变化，需重新记录。

## 5. 构建与供应链

- pin：`third_party/ffmpeg-9.1.pin` 与 `third_party/openssl-3.5.9.pin`（入库，含 version+hash+许可来源）。缓存与解包：`third_party/cache/`（`.gitignore`，不入库）。离线缓存命中且 hash 一致则不下载，**不自动升级**。
- prefix：`build/openssl-3.5.9/prefix` 与 `build/ffmpeg-9.1/prefix`（不入库）。FFmpeg 自身 `--enable-static --disable-shared`，`PKG_CONFIG_PATH` 把钉选 OpenSSL 放在最前，并带 `-I/-L` 到该 prefix。libxml2 **不用** `pkg-config --static`（避免把 liblzma 静态绑死），由最终 `.so` 动态依赖系统 libxml2/zlib。
- 最终 `libmsff.so` 显式链 `$OPENSSL_PREFIX/lib/libssl.a` 与 `libcrypto.a`，加 `--version-script`（`local: *;`）、`--gc-sections`、`--exclude-libs,ALL`、hidden visibility；不得只改后缀。
- 不 clone 额外仓库，不登录 GitHub。
- `./build.sh` 仍是唯一编译入口；`./build.sh openssl` 只构建钉选 OpenSSL 前缀；`./build.sh ffmpeg` 先确保 OpenSSL 再构建 FFmpeg 前缀并编 `libmsff` / `test_ff_backend`。`./build.sh clean` 删除 `build/`（含 prefix），不删除 pin 与（本地）缓存。

## 6. 能力矩阵与本轮联调（2026-11-20 22:07 CST，授权 [pt71u]）

四列含义：编入 = 本裁剪 configure 已打开；宿主联调 = Bot x86_64 受控自测已跑通；跨平台编译 = 未做 el7/NDK/四平台编译；实机 = 未做。不以编入冒充联调。未更换 libxml2，未做一体静态依赖裁剪。官方 DASH 依赖 libxml2，不是「FFmpeg 自带 XML」。`libmsff.so` 去调试符号约 2.01 MiB **不含** 系统 libssl/libxml2 等动态库体积；曾经把动态文件相加得到约 11 MiB 只是估算，不是实测，不作承诺。

| 能力 | 编入裁剪 | 宿主联调 | 跨平台编译 | 实机 |
| --- | --- | --- | --- | --- |
| file: TS / MP4 / FLV | 是 | 是（21:21） | 否 | 否 |
| HTTP/HTTPS 取流、证书校验、拒绝降级 | 是 | 是（21:21；11:26 在静态 3.5.9 上回归，并补 TLS1.2/1.3×RSA-2048/ECDSA-P256 与 hostname 错） | 否 | 否 |
| HLS（多层清单、302 后基址） | 是 | 是（21:21） | 否 | 否 |
| DASH-over-HTTP（分离 A/V AdaptationSet、相对 URL、无扩展名探测、302 基址、缺片/截断、401、取消、超时） | 是（dash+libxml2+mov） | 是（22:07） | 否 | 否 |
| UDP MPEG-TS（pkt_size=1316，listen+超时） | 是（udp+mpegts） | 是，受控子集（22:07；11:58 仍回归，改用系统 ffmpeg -re 循环发送避免突发竞态） | 否 | 否 |
| RTP + SDP（H264，RFC 6184 非交错：single NAL / STAP-A / FU-A） | 是（rtp+sdp + h264 parser，无 decoder） | 是，受控子集（11:58 帧合同，见第 10 节）；format=`rtp`，不伪造成 TS；不是摄像机全集 | 否 | 否 |
| SRTP、通用摄像机 RTP 全集 | srtp 已编入 | 否 | 否 | 否 |
| RTSP（TCP interleaved / UDP transport） | 是（rtsp + tcp/udp/rtp） | 是，受控 H264 子集（11:58，第 10 节）；AAC-over-RTSP 未测；不是摄像机全集 | 否 | 否 |
| RTMP 拉流（FLV 标签） | 是（rtmp + flv/live_flv） | 是，本机 ffmpeg -listen 发布，验配置+H264/AAC 媒体包，format=`rtmp`（11:58） | 否 | 否 |
| RTMPS / RTMPT | 是 | RTMPS：是，受控本机（2026-11-21 18:36 CST：TLS 转发 + 本地 CA，校验保持开；无 CA=`UNSUPPORTED`，成功 events=36，主机名不匹配=`UNKNOWN`）。RTMPT：否。不得因 HTTPS 或静态 SSL 已过、或仅因编入 rtmps 而声称公网/设备已验收 | 否 | 否 |
| 系统硬解出画 | 否（无 decoder） | 否 | 否 | 否 |

本轮 DASH 对照：系统 ffprobe 7.1.5 的 **AVPacket** 表。视频 75 包在 PTS/DTS/size/key 上一致；FFmpeg 9.1 多发出 1 个尾部音频包（141→142），已记录，不以 7.1.5 数冒充 9.1。负 PTS（AAC priming）在 22:07 曾把 has_*=0（已由 2026-11-21 11:12 CST 的统一 origin 合同取代，见第 8 节）。不能把每个 AVPacket 写成完整 AU。无扩展名 MPD 在 `Content-Type: application/dash+xml` 下可被探测，不依赖 `.mpd` 后缀。缺片/截断不得收齐全部 75 个视频包（实测缺中片 50、截断 25）。401 失败。取消后 pull 为 CANCELED。停顿超时在 22:07 曾表现为 `MS_ERR_CANCELED`（已由 11:12 CST 的 `MS_ERR_TIMEOUT` 取代，见第 8 节）。

UDP/RTP：系统 ffmpeg 只作发送端，产品不 exec。端口临时绑定，进程退出回收（SIGTERM/SIGKILL + waitpid，子进程 PR_SET_PDEATHSIG）。不是所有摄像机兼容。

Sanitizer：`./build.sh test`（debug）对 `test_ff_backend` 与 `ms_ff.c` 适配器开 ASan/UBSan；钉选 FFmpeg 9.1 prefix 以 `-O2` 且无 asan 编译。不得声称对 libav 全覆盖。2026-11-20 22:11 CST 完整 `./build.sh test`（debug）中 `test_core` 42 组、`test_net`、`test_ff_backend`（含本轮 DASH/UDP/RTP）均通过，日志无 ASan/UBSan runtime 诊断。

同一宿主、同一编译器，`./build.sh release` 于 2026-11-20 22:10 CST 复测（gcc 14.2.0，x86_64，非正式验收，不能冒充 NDK/旧 Android）：核心 `libmsmedia.so.1.0.0` 去调试符号后仍为 **166152** 字节（text 155588 / data 920 / bss 8），NEEDED 仅 `libc.so.6`，BuildID 仍为 `efd939e4d131d6c406387f015a6707698bc6a935`；`libmsnet.so.1.0.0` 仍为 **35432** 字节，BuildID 仍为 `cb3ea5b7a042c6e827a0997e93d99b61f03ef873`；`libmsff.so.1.0.0` 去调试符号后仍为 **2105232** 字节（unstripped 9389640），`size` 仍为 text 1935486 / data 157688 / bss 44872，NEEDED 仍为 `libssl.so.3`、`libcrypto.so.3`、`libxml2.so.2`、`libz.so.1`、`libm.so.6`、`libc.so.6`（ldd 另见 libxml2 间接的 `libzstd`/`liblzma`）。因本轮负 PTS 处理变更，BuildID 现为 `992811bab811ac66030a95322d4a8e9235f029d9`（不再是 21:21 的 `03b86d17…`）。薄适配层 `libmsff.a` 现为 **19126** 字节（21:21 记载为 19190）。相对旧核心的宿主 strip 增量仍约 **1.94 MiB**（2105232−166152）。约 2.01 MiB **不含** 系统 SSL/XML 等外部动态库；约 11 MiB 静态合并数只曾按动态文件求和估算，非实测，不作承诺。未更换 libxml2，未做一体静态依赖裁剪。无新源码包。

## 7. 本轮范围外（不是新授权）

第 8 节正确性、第 9 节 OpenSSL 静态合入、第 10 节 RTSP/RTP 帧合同与 RTMP 拉流已完成（宿主自测）。已批准但**尚未实施**的后续是：统一 C 硬解 Android/Media3 与有界零拷贝/低内存计量。受控本机 RTMPS 已在 2026-11-21 18:36 CST 宿主验证（校验保持开）；RTMPT、公网/设备 RTMPS、SRTP、AAC-over-RTSP、摄像机全集仍未测。不要把未验证写成已完成。libxml2/zlib 仍动态，未做一体静态依赖裁剪。

## 8. 正确性修正（2026-11-21 11:12 CST，授权 [pt73u]）

公共 `ms_event` 布局不变，`MSMEDIA_ABI_VERSION` 仍为 1，soname 仍为 `libmsmedia.so.1`。追加兼容项：`MS_ERR_TIMEOUT`（枚举末尾，既有值不变）、`ms_rescale_q` / `ms_event_signed_ts` / `ms_event_media_time_90k`、`ms_ff_time_origin_90k`。自写 MP4/FLV 路径对负 PTS 仍为 `UNSUPPORTED` 的历史行为未改。

时间戳合同：`pts`/`dts` 仍为 uint64 相对值，等于有符号媒体时间减去**会话级统一** origin（90 kHz，所有轨相同，不按轨截 0）。`AV_NOPTS_VALUE` 才是 has_*=0；合法负值（AAC priming、elst、B 帧 DTS）保留 has_*=1，不绕成巨大 uint64。还原公式见 `ms_event_signed_ts`。缩放朝 -inf，检测溢出。origin 在 open 后按流 `start_time` 选取，首个 AU 发出前可再下调；已发出 AU 后不再改 origin。

超时：`io_timeout_ms` 每轮阻塞 I/O 重新武装 deadline；interrupt 与 `AVERROR(ETIMEDOUT)` 映射为 `MS_ERR_TIMEOUT`，不得伪装 `CANCELED`。用户 `cancel` 优先。EOF→`MS_EOS`，EAGAIN→`MS_AGAIN`，EIO 不是超时。`canceled` / `timed_out` / `deadline_us` 为原子量，供 cancel 跨线程。`find_stream_info` 探测超时或缺 metadata 仍只记录，不因此把已打开的输入判失败（无 decoder 时常见）。

DASH 尾包：独立 init+trun（含 elst，`scripts/dash_trun_ref.py`）音频 **142** 样本，末条 pts=143360、size=146、duration=640，对应 MPD 3.0s 余量；系统 ffprobe 7.1.5 为 141，属版本解封装差异，不是适配器重复或残帧。9.1 后端与 trun 在 size/PTS/DTS/key 上一致（含 priming -1024）。不按跨版本 packet 数强行相等，不声称每个 AVPacket 对所有协议都是完整 AU；本样本与 trun sample 边界一致。

宿主证据（同日 11:08–11:12 CST）：origin_90k=-1920；bframes.mp4 中 dts&lt;pts 21 次，min signed pts/dts=-1024，origin=-7200。stall `open=TIMEOUT`，cancel 仍为 `CANCELED`。`./build.sh test`（debug，适配器与测试带 ASan/UBSan；FFmpeg prefix 无 asan）42 组 `test_core`、`test_net`、`test_ff_backend` 通过，无 sanitizer runtime 诊断。四组 `dev_*crosscheck.sh` 与 `ms_so_abi_smoke`（release）通过。

体积（2026-11-21 11:11 CST，`./build.sh release`，gcc 14.2.0 x86_64，非正式验收，不能冒充 NDK/旧 Android）：

- 核心 `libmsmedia.so.1.0.0` 去调试符号后 **166168** 字节（text 157622 / data 936 / bss 8；22:10 为 166152）。NEEDED 仅 `libc.so.6`。BuildID `d84caf76b9d81eb3c5688c32732a06ef54c3a1ae`。`libmsmedia.a` **225726** 字节。
- `libmsnet.so.1.0.0` 仍为 **35432** 字节，BuildID 仍为 `cb3ea5b7a042c6e827a0997e93d99b61f03ef873`。
- `libmsff.so.1.0.0` 去调试符号后 **2109328** 字节（unstripped 9393816；22:10 为 2105232 / 9389640），`size` text 1939394 / data 157688 / bss 44872。BuildID `7df116d2015a254420d44e8521401771ca100901`。薄适配层 `libmsff.a` **23032** 字节。NEEDED 仍为 `libssl.so.3`、`libcrypto.so.3`、`libxml2.so.2`、`libz.so.1`、`libm.so.6`、`libc.so.6`（ldd 另见 libxml2 间接的 `libzstd`/`liblzma`）。相对本轮核心的 strip 增量约 **1.85 MiB**（2109328−166168）。约 2.01 MiB **不含** 系统 SSL/XML；约 11 MiB 只曾按动态文件求和估算，非实测，不作承诺。XML 仍动态，不能称全自包含。无新源码包。HEAD 仍为本地 `1092bec`，未 commit/push。


## 9. OpenSSL 3.5.9 LTS 按需静态裁剪合入（2026-11-21 11:26 CST，授权 t74/t75/t78）

不是工具自行扩范围。钉选依据 2026-11-21 官网（https://www.openssl-library.org/source/ 与 https://www.openssl.org/source/）当时仍受安全支持的版本：4.1.0、4.0.3 与 3.5.9 LTS（发布 2026-09-29，EOL 2030-04-08）。**未**因宿主系统包是 3.5.7 就升级到最新主版本 4.1；选定 3.5.9 LTS。tarball `https://www.openssl.org/source/openssl-3.5.9.tar.gz`（301 到 GitHub releases）。官方 SHA-256 `603f5602e2eef00d77fbd429d34dcd5822bb301757a1bc9cdb24c670f1eb859a`（`openssl-3.5.9.tar.gz.sha256` 与 GitHub release 一致）。PGP：`openssl-3.5.9.tar.gz.asc`，公钥 https://www.openssl-library.org/source/pubkeys.asc，主指纹 `B146647E45A7B33947AB226B2A2C87D161692D40`，本机构验 Good signature。许可 Apache-2.0。

构建：`scripts/openssl_build.sh`，入口 `./build.sh openssl`（也可由 `ffmpeg9_build.sh` 按需调用）。PIC、`no-shared`、`no-module`/`no-dso`（默认 provider 静态编进 libcrypto，不暗需系统 provider `.so` 或系统 `openssl.cnf`）。`--openssldir` 为本 prefix 下 stub `openssl.cnf`。默认信任通过符号链接指向宿主 `/etc/ssl/certs`（及 `cert.pem`）；这是**运行时宿主 CA 路径**，不是把系统 CA 打进仓库。Android / 无 `/etc/ssl/certs` 的环境未测。`ms_ff_set_ca_file` 只增加自定义锚，**没有**跳过校验开关。适配器仍强制 `tls_verify=1` 并拒绝 https→http 降级。

保留（TLS 客户端 HTTPS/RTMPS/RTSPS 必需，不能盲关）：TLS 1.2/1.3、RSA、ECDSA/EC、DH、证书链校验、主机名/SNI、安全随机、CPU asm（本机 x86_64 实测编译定义含 AES_ASM、SHA*_ASM、ECP_NISTZ256_ASM、X25519_ASM 等）。SRTP 扩展保留：FFmpeg 9.1 `tls_openssl.c` 引用 `SSL_CTX_set_tlsext_use_srtp`，初次 `no-srtp` 导致编不过，已撤回该开关。

关闭及代价：无 apps/docs/tests/engines/fips/legacy；无 SSL3/TLS1.0/TLS1.1；无 DTLS（`no-dtls`/`no-dtls1`/`no-dtls1_2`，FFmpeg 的 DTLS 路径因此未在本轮验证）；无 SRP/PSK、弱套件、压缩、zlib；无 md2/md4/rc2/rc4/rc5/idea/seed/whirlpool/sm2/sm3/sm4/aria/bf/cast/camellia/des、ts/cms/cmp。`SSL_CTX_set_min/max_proto_version(TLS1_1_VERSION)` 在本构建上仍可能返回成功，但 TLS1.1-only 握手被拒绝（`tlsv1 alert protocol version`）。不声称未来全部证书/算法/中间 CA 100% 兼容；仅覆盖下面受控矩阵。可选 Configure 项 `no-quic` / `no-autoload-config` / `enable-ec_nistp_64_gcc_128` 本轮未加上（缓存的 Configure 帮助文件当时不是完整选项表）；默认 provider 已内置，stub cnf 不加载模块。

FFmpeg 9.1 已按该 prefix 重配（`CONFIG_OPENSSL 1`，仍 `CONFIG_GPL 0` / `NONFREE 0` / `DECODERS 0`）。`ms_ff_openssl_version()` 实测 `OpenSSL 3.5.9 29 Sep 2026`，`ms_ff_openssl_version_num()` = `0x30500090`。动态导出仅公共 `ms_ff_*`（含这两个新符号）；`nm -D` 无 SSL_/EVP_/OPENSSL_ 导出。`test_ff_backend` 同样静态链同一 pin，避免进程内混用系统 3.5.7。核心 `libmsmedia` 仍不带 SSL。旧 `libmsnet` 仍动态链系统 OpenSSL 3.5.7，仅作对照，未把 SSL 拉进 core。

TLS 矩阵（测试 CA 只写 `build/*/net-ca/`，不入库；系统 `/usr/bin/openssl` 仅用于签发）：受控成功 — TLS1.2/1.3 × RSA-2048 与 ECDSA-P256（各 events=6 AU=3）；不受信（无 CA）失败；hostname/SAN 不匹配失败（status=5）；https→http 降级拒绝；TLS1.1-only 握手失败。HTTP/HLS/DASH、timeout（stall `open=TIMEOUT`）、cancel（`CANCELED`）回归通过。

算法复用（本轮只审查、不改实现、无性能对照、不宣称更快）：`--enable-openssl` **不会**自动替换 libavutil 的 AES/SHA/HMAC。本裁剪树中实际热点仍走 `av_aes_*` / `av_hmac_*` / `av_sha*`：`libavformat/crypto.c`（crypto 协议）、`hls.c` AES-128/SAMPLE-AES、`srtp.c`、`rtmpdigest.c`（SHA256 HMAC）、以及若干 mux/hash 路径。`rtmpdh.c` 在 `CONFIG_OPENSSL` 下使用 OpenSSL `BN_*` 做 DH。TLS 记录层走 OpenSSL。未对 FFmpeg 密码实现打补丁。后续可再细化算法/体积。

Sanitizer：`./build.sh test`（debug）对 `test_ff_backend` 与 `ms_ff.c` 开 ASan/UBSan；钉选 FFmpeg 9.1 与 OpenSSL 3.5.9 prefix 均为优化构建、**无 asan**。不得声称对 libav 或 libcrypto 全覆盖。2026-11-21 11:24 CST 完整 `./build.sh test`：42 组 `test_core`、`test_net`、`test_ff_backend` 通过，无 ASan/UBSan runtime 诊断。随后 release 的 `ms_so_abi_smoke` 与四组 `dev_*crosscheck.sh` 通过。

体积（2026-11-21 11:26 CST，`./build.sh release`，gcc 14.2.0 x86_64 / Debian 14.2.0-19，ELF64 x86-64，非正式验收，不能冒充 NDK/旧 Android/el7）：

- 核心 `libmsmedia.so.1.0.0` 去调试符号后仍为 **166168** 字节（text 157622 / data 936 / bss 8），BuildID 仍为 `d84caf76b9d81eb3c5688c32732a06ef54c3a1ae`，NEEDED 仅 `libc.so.6`。`libmsmedia.a` 仍为 **225726**。
- `libmsnet.so.1.0.0` 仍为 **35432** 字节，BuildID 仍为 `cb3ea5b7a042c6e827a0997e93d99b61f03ef873`；NEEDED 仍含系统 `libssl.so.3` / `libcrypto.so.3`（对照路径，未改）。
- `libmsff.so.1.0.0` 去调试符号后 **7986448** 字节（unstripped 16054352），`size` text 7362738 / data 614656 / bss 54616。BuildID `28b2d694a66df83d27b2f840e0e1a81731e2752e`。薄适配层 `libmsff.a` **23472** 字节。直接 NEEDED：**仅** `libxml2.so.2`、`libz.so.1`、`libm.so.6`、`libc.so.6`（不再有 `libssl`/`libcrypto`/`libav*`）。ldd 递归另见 libxml2 间接的 `liblzma.so.5`。
- 相对合入前同一编译器上的 strip `libmsff`（11:11 CST 的 2109328）静态 SSL/crypto **增量 = 5877120 字节（约 5.61 MiB）**。这是两次 strip 产物相减，不是把系统 `libssl.so.3`（本机 1109952）+ `libcrypto.so.3`（6521408）或 prefix 里未裁链的 `libssl.a`（1979176）+ `libcrypto.a`（10210574，合计约 11.62 MiB）相加。`--gc-sections` 去掉了静态归档中的一部分，但默认 provider 与 TLS 客户端必要算法仍占可观 .text。相对本轮核心的 strip 总量约 7.46 MiB（7986448−166168），其中含 FFmpeg 裁剪与静态 OpenSSL。约 11 MiB 旧估算仍不是实测。XML/zlib 仍在动态边界之外。不能称全自包含。静态链接**不**预设改善运行内存，本轮未测 RSS。无新源码包。HEAD 仍为本地 `1092bec`，未 commit/push。

## 10. RTSP / RTP 帧合同 / RTMP 本机联调（2026-11-21 11:58 CST）

授权：用户原话 t67「rtsp rtmp udp rtp http https都支持」、t78「继续完成任务按这些要求交付」、t73 不要每轮停。在已交付的正确性与静态 OpenSSL 钉选之上，本步只做受控本机联调与帧合同，不扩自写协议，不改已验证时间戳/timeout/OpenSSL pin，保持 `CONFIG_DECODERS=0`。无 CloudAgent / machineId / commit / push / 登录 / 最终包 / 软解。

公共 ABI 仍为 `MSMEDIA_1.0` / `MSMEDIA_ABI_VERSION=1`。在 `MS_FORMAT_DASH=7` 之后追加 `MS_FORMAT_RTSP=8`、`MS_FORMAT_RTMP=9`、`MS_FORMAT_RTP=10`（旧偏移不变）。`rtsp` 映射为 RTSP；`sdp`/`rtp` 映射为 RTP；RTMP URL（即使 iformat 是 flv/live_flv）映射为 RTMP，本地 FLV 文件仍为 FLV。不把未知格式伪造成 TS。

帧合同（独立参考，不是「可能 NAL/FU-A」）：测试专用 `scripts/rtp_h264_testutil.py`（stdlib only，产品不得依赖或 exec）把 lavfi 生成的多 slice AnnexB（`slices.264`，不入库）打成 RFC 6184 非交错包，并写出 `ref_aus.txt` / `au_XXX.bin`。本轮样本：16 个 AU、124 个 RTP 包、mtu=200，种类含 FU-A、single NAL、STAP-A，多 NAL/多 slice，marker 在 AU 末包。对照 FFmpeg 9.1 depacketizer + **H264 parser**（不是 decoder；`AVSTREAM_PARSE_FULL`）组装后的载荷：SHA-256（去起始码后的 NAL 载荷，兼容 3/4 字节起始码与 SDP sprop 重复 SPS/PPS 前缀）。

宿主实测（`test_ff_backend`，loopback 随机端口，进程可取消/timeout/`waitpid`/PR_SET_PDEATHSIG，无公开流或凭据）：

- 完整发送：format=`rtp`，CODEC_CONFIG 先到，16/16 AU 与独立参考一致，mismatch=0，首帧为关键帧。
- 重排序：16/16，mismatch=0。
- 丢 mid-FU-A（包 8，亦即该 AU 截断）：FFmpeg 9.1 `rtpdec.c` 只打日志「RTP: missed」，**不**置 `AV_PKT_FLAG_CORRUPT`。适配器同线程捕获该官方日志，丢弃当前可能残缺的包（即使带 KEY），再等到下一个安全关键帧。实测 AUs=11/16，mismatch=0，first_key=1。未核实的 packet 不作为完整 AU 送出。
- RTSP TCP interleaved 与 UDP transport：各 16/16 AU、cfg=1，format=`rtsp`。子集为 H264 RFC 6184 非交错；AAC-over-RTSP **未测**。无服务器但接受后不回复：`open=TIMEOUT`（io_timeout_ms=800），不是 CANCELED。
- RTMP：系统 ffmpeg `-listen 1` 作本机发布端（产品不 exec），拉流 format=`rtmp`，events=36，H264 AU=11，AAC AU=20，视频/音频 CODEC_CONFIG 各 1。未把 RTMP 的通用 `timeout` 键误写成「监听入站秒数」（FFmpeg 该键会隐含 listen=1 且微秒值会溢出）。
- RTMPS：受控本机已测（2026-11-21 18:36 CST，TLS 转发 + 本地 CA，校验保持开）；RTMPT **未测**。不因 HTTPS 或静态 SSL 已过、也不因仅编入 rtmps 而声称公网或设备 RTMPS 已验收。
- 阻塞 I/O 仍每轮 deadline；内存/队列上限未改；未缩小 probesize。

辅助工具仅测试使用。RTSP PLAY 服务因系统 ffmpeg 只有 listen  demuxer、不能当 PLAY 服务器，故用上述最小 loopback 脚本；不把产品与该脚本绑定。

Sanitizer：本轮 `./build.sh test`（debug）42 组 `test_core`、`test_net`、`test_ff_backend`（含 TLS 矩阵与本帧合同）通过；ASan/UBSan 仍只覆盖适配器与测试，不覆盖 libav/OpenSSL prefix。release 后 `ms_so_abi_smoke` 与四组 `dev_*crosscheck.sh` 通过。

体积（2026-11-21 11:58 CST，`./build.sh release`，gcc 14.2.0 x86_64，非正式验收）：`libmsmedia.so.1.0.0` 去调试符号后仍为 **166168** 字节（text 157718 / data 936 / bss 8；因追加格式名，BuildID 现为 `6b13748bd29ce2939112c8b4bddd6c3b5de28f9e`，不再是 11:26 的 `d84caf76…`），NEEDED 仅 libc。`libmsnet.so.1.0.0` 仍为 **35432** / `cb3ea5b7a042c6e827a0997e93d99b61f03ef873`。`libmsff.so.1.0.0` **7986520**（unstripped 16054608；相对 11:26 的 7986448 **+72**），text 7364504 / data 614696 / bss 54624，BuildID `0c556a20868e653ac7f542d0ac497d5a040049b9`，薄 `libmsff.a` **27112**。直接 NEEDED 仍仅为 libxml2、libz、libm、libc。未测 RSS，不声称静态降低运行内存。无新源码包。HEAD 仍为本地 `1092bec`，未 commit/push。下一已批准阶段：Media3 Android 真实硬解 + 低内存/零冗余 copy 计量。不要再靠扩协议全集延误硬解。

## 11. 体积优化（2026-11-21 12:19 CST，授权 t80/t82/t83）

授权：t80「要开启优化」、t82「可以看看 cjzb 的编译」、t83「tls 是 openssl」。本步只做体积优化小里程碑，不扩展 Media3 硬解。已有 `build/release` 作为基线保留（并复制到 `build/release-baseline/`，未移动原目录）。新产物在 `build/release-size`，独立 prefix 为 `build/openssl-3.5.9-size` 与 `build/ffmpeg-9.1-size`。钉选仍为官方 OpenSSL 3.5.9 LTS 与 FFmpeg 9.1；TLS 1.2+1.3、证书与主机名校验、SRTP（`tls_openssl.c`）、`CONFIG_DECODERS=0` 未改。不换 mbedTLS。不照搬旧 OpenSSL 1.1.1 的 `no-err`/`no-ocsp`/`no-ct`/`no-srtp`/`no-dh`/`no-stdio` 等以减功能冒充优化。`OPENSSL_SMALL_FOOTPRINT` 只影响表与体积/速度权衡，算法集合与校验策略不变。

CJZB 只读结论仍有效：Bot 副本混两条链；旧 OpenSSL 1.1.1 脚本在 `bs-player/scripts/build_openssl_android.sh` 等，源与产物缺失；4,214,156 字节 v7a so 属 n6+mbedTLS 链，**不能**当作 OpenSSL 版本对照。

落实的编译选项（有效 flags 以 Makefile / `ffbuild/config.mak` 中**最后一个** `-O` 为准，已断言不被后写的 `-O2`/`-O3`/`-Oz` 覆盖）：

- OpenSSL size：`-Os -ffunction-sections -fdata-sections -DOPENSSL_SMALL_FOOTPRINT`。宿主 `linux-x86_64` Makefile `CFLAGS=-Wall -O3 -fPIC -Os -ffunction-sections -fdata-sections`，最后为 `-Os`；`CPPFLAGS` 含 `OPENSSL_SMALL_FOOTPRINT`。
- FFmpeg size：`--enable-small`，`--extra-cflags` 用 `-Os` 而非 `-O2`（否则会盖掉 enable-small），`--extra-ldflags=-Wl,--gc-sections`，`CONFIG_SMALL=1`，`CONFIG_DECODERS=0`。宿主 `config.mak` 最后 `-O` 为 `-Os`。
- 适配器 / `libmsmedia` / `libmsnet` / `libmsff`：`./build.sh release-size` 使用 `-Os -DNDEBUG -ffunction-sections -fdata-sections`，隐藏符号、`--version-script`、`--gc-sections`、`--exclude-libs,ALL`（libmsff）、`strip --strip-unneeded`。入口：`./build.sh release-size`、`./build.sh test-size`。

宿主 gcc 14.2.0 x86_64，strip 后（无 `.symtab` / `.debug`；非正式验收）：

| 库 | 基线 `build/release` | size-opt `build/release-size` | 差值 |
| --- | ---: | ---: | ---: |
| `libmsmedia.so.1.0.0` | 166168（text 157718 / data 936 / bss 8；BuildID `6b13748bd29ce2939112c8b4bddd6c3b5de28f9e`） | 125112（text 114957 / data 800 / bss 8；`015d256be8f6b6dbe3c158b716a8f0dd30a9a452`） | **-41056（-24.71%）** |
| `libmsnet.so.1.0.0` | 35432（`cb3ea5b7a042c6e827a0997e93d99b61f03ef873`） | 31240（`d2673dd9e0f1ac45fca9a6362a8ae0395408d179`） | **-4192（-11.83%）** |
| `libmsff.so.1.0.0` | 7986520（unstripped 16054608；text 7364504 / data 614696 / bss 54624；`0c556a20868e653ac7f542d0ac497d5a040049b9`） | 5767128（unstripped 12012704；text 5165682 / data 595760 / bss 23744；`75cdd1e1fb9acbc080130a3e1d94afee0ac14faa`） | **-2219392（-27.79%）** |

`libmsff.so` 动态依赖两侧相同：`libxml2.so.2`、`libz.so.1`、`libm.so.6`、`libc.so.6`、`ld-linux-x86-64.so.2`，无系统 libssl/libcrypto。`libmsmedia` 仍仅 libc。`libmsnet` 仍动态链系统 OpenSSL 作对照。动态导出仍仅公共 `ms_ff_*`（`MSMEDIA_1.0`）。薄 `.a` 因 section 元数据略增，不是交付体积口径。

非正式 armeabi-v7a 同 ABI 测量（Bot 已有 NDK r29.0.14206865，API 21，`armv7a-linux-androideabi21-clang`；脚本 `scripts/android_v7a_size.sh`；**不是**四平台/旧 Android 正式验收，未在设备或模拟器上运行）：两侧功能集相同（含 DASH），测量用同一份静态 libxml2 2.13.8（sha256 `277294cb33119ab71b2bc81f2f445e9bc9435b893ad15bb2cd2b0e859a0ee84a`）链入 so，因此 v7a `libmsff.so` 的 NEEDED 为 `libz.so`、`libm.so`、`libdl.so`、`libc.so`（无 libxml2.so、无 libssl），与宿主动态 libxml2 不同，也**不可**与宿主 x86_64 字节或 mbedTLS 4,214,156 字节 so 对照。clang 在 `--enable-small` 后曾追加 `-Oz`、基线默认曾在 `-O2` 后写 `-O3`，已把目标 `-Os`/`-O2` 追加到 `config.mak` 末尾。OpenSSL android-arm 基线最后为 `-O3`（与宿主 OpenSSL 目标默认同类），size 最后为 `-Os` + `OPENSSL_SMALL_FOOTPRINT`。strip 后：基线 **5390136**，size-opt **4235076**，差值 **-1155060（-21.43%）**。ELF32 ARM，无 `.symtab`，公共符号与宿主相同。适配器在无 `__int128` 的目标走便携向 -inf 的 64×64 乘除（宿主仍用 `__int128`；便携路径曾与 `__int128` 对照 784 组一致）。v7a 未做功能回归或性能/内存测量。

回归（`./build.sh test-size`，release-size，**无** ASan/UBSan）：42 组 `test_core`、`test_net`、`test_ff_backend`（TLS 矩阵、HTTP/HLS/DASH、负时间戳 origin_90k=-7200、stall=`TIMEOUT`、刚完成的 RTSP/RTP/RTMP 帧合同）通过；`ms_so_abi_smoke` 通过；四组 `tests/dev_*crosscheck.sh` 指向 `build/release-size` 的 inspect 通过。`CONFIG_DECODERS` 仍为 0。性能与 RSS/内存**未测**，不声称 `-Os` 更快或更省运行内存。无新源码包。HEAD 仍为本地 `1092bec`，未 commit/push。本条之后的硬解小里程碑已于 2026-11-21 12:36 CST 交付宿主 mock 与非正式 NDK 交叉编译（`docs/HW_ANDROID.md`）；真实设备硬解仍未跑。

## PNG/JPEG 前缀与 HLS 内部分片（2026-11-21 16:21 CST）

`ms_ff_io_open` 在 file/http(s) 等非 UDP/RTP/RTSP/RTMP 路径上，先读 8 字节识别 PNG/JPEG 魔术，再用公共 `ms_ts_scan` 有界剥离。播放列表（m3u8）不会被整窗预读包装，以免丢掉 HTTP `location`。公开统计：`ms_ff_ts_prefix_stats`。

实证（`MS_FF_CA=build/debug/net-ca`、`MS_FF_DASH_DIR=build/dev-samples/dash-ff` 跑 `build/debug/test_ff_backend`，exit 0）：HLS 媒体播放列表的分片为 PNG+TS 时，`ops=1 last_bytes=457 kind=png`，并解出 4 个 AU。说明 **FFmpeg 打开内部 segment 时确实走了同一套 io_open 剥离**，不是只对直接 file/HTTP 资源生效。本项此前不得在无计数器的情况下宣称。file 与直接 HTTP 的 PNG+TS 同样通过。UDP/RTP 路径按设计不剥离。

## Android / 系统信任锚（2026-11-21 16:39 CST）

`https`/`tls`/`rtsps`/`rtmps` 在打开前要求信任锚：`ms_ff_set_ca_file`、`ms_ff_set_ca_path`，或（默认允许时）探测系统 CA 文件/目录。FFmpeg 9.1 OpenSSL 后端在 `verify=1` 时校验证书链与主机名（`SSL_set1_host` / IP SAN）。无锚点时返回 `MS_ERR_UNSUPPORTED`，不把 `verify` 设为 0，也不发起握手。`ms_ff_set_allow_system_trust(0)` 用于该拒绝路径的宿主测试（已通过）。Android 由应用导出 `AndroidCAStore` PEM 后设置 ca_file；静态合入的 OpenSSL 其编译期 OPENSSLDIR 在设备上不可用，不得依赖默认路径悄悄降级。

## P4 音轨/字幕（2026-11-21 20:10 CST）

加法接口：`ms_ff_list_tracks` / `ms_ff_select_track` / `ms_ff_set_external_subtitle` / `ms_ff_subtitles_at`。打开成功后即应用默认选择（第一路视频、默认或第一路音频，字幕关闭）。同容器切轨丢弃未选中音/字幕包，不重开输入；HLS 备选 rendition 仍 `UNSUPPORTED`。内嵌可播放文本作为 `MS_EVENT_SUBTITLE` 发出。`libmsff.so` 隐藏静态编入同一份 `ms_sub.c`，NEEDED 仍无 `libmsmedia`、无系统 libssl。宿主 `test_p4_ff_tracks` 用系统 ffmpeg 生成双 AAC MP4 验证切换（不是设备出声）。未开启 FFmpeg 解码器，也未加入 srt/webvtt demuxer。

2026-11-21 20:25 CST 起增加 `ms_ff_seek` / `ms_ff_duration_90k` / `ms_ff_is_seekable`：对 AVIO 可定位输入用 `av_seek_frame`（关键帧向前），时间轴与事件 PTS 同轴（相对会话 origin，含负 PTS 原点）。成功则 flush、清内嵌 cue、置 `skip_to_key`、使 CSD 再发出并增加 `timeline_epoch`。仍不启用解码器。直播/不可定位返回 `NOT_SEEKABLE`。宿主 `test_p4_ff_seek` 用可定位 MP4 验证，不是设备出画。

## HLS 字幕 rendition（2026-11-22 01:30 CST）

按需追加 `--enable-demuxer=webvtt`（LGPL 2.1，`webvttdec.c` + `subtitles.o`）。`CONFIG_WEBVTT_DEMUXER=1`，`CONFIG_WEBVTT_DECODER=0`，`CONFIG_WEBVTT_MUXER=0`，仍 `CONFIG_DECODERS=0`、`CONFIG_GPL=0`、`CONFIG_LIBASS=0`。产品 `format_whitelist` 加入 `webvtt`，否则嵌套打开分片时会拷贝父白名单而拒绝独立 VTT。官方 HLS 对 `EXT-X-MEDIA TYPE=SUBTITLES` 外链在 `init_subtitle_context()` 里强制用 webvtt 打开当前分片 URL；本轮宿主实证是独立 WebVTT playlist，不是 TS 封装外链。最小本地补丁由 `scripts/ffmpeg9_build.sh` 与双 ABI size 脚本幂等应用：`patches/ffmpeg-9.1-hls-sub-rendition-switch.patch` 在字幕 playlist 转为 needed 时不按 `cur_timestamp` 丢掉本段已开始的 cue，并关闭残留 webvtt 上下文以便关闭再启；`patches/ffmpeg-9.1-hls-sub-timeline.patch` 在 `hls_read_packet` 对字幕保留 end>seek 的进行中 cue（end 开区间）、直播首次启用从当前窗口 `start_seq_no` 重放（不改 seekable、不缓存全部历史），以及 `webvttdec.c` 解析 `X-TIMESTAMP-MAP`（映射结果换算为默认 1/1000，不改 stub 建流时基）。音视频 catch-up 与用户 `hls_read_seek` 的 A/V 丢包路径不变。不得把「关闭残留 ctx」扩到 mpegts 视频/音频 playlist。

体积（相对 matroska 切片后、打开 webvtt 之前）：宿主 `libavformat.a` 14,236,440 → 14,423,582（+187,142）；`libavcodec.a` 仍为 15,177,444。Android size-opt `libavformat.a` v7a 9,243,416 → 9,377,550，v8a 13,546,824 → 13,741,614。符号 `ff_webvtt_demuxer` 存在。未覆盖历史 `build/android-v7a/size/libmsff.so.1.0.0`（4,235,076，`7a757095…`，mtime 12:17:30 CST）。

## HLS 视频变体手动选择（2026-11-22 01:55 CST）

不新增 demuxer，不重编产品前缀代码。`ms_ff_select_track` 对视频采用与音/字幕相同的 pending 模型，但提交门控为新轨关键帧，并重发该流 CSD。跨 codec 拒绝。字幕补丁未扩展到关闭视频 mpegts ctx（实测会空指针）。宿主 `libavformat.a` 仍为 14,423,582，`libavcodec.a` 仍为 15,177,444（相对 01:30 字幕切片，仅注释）。Android size-opt 前缀未重建，只重链适配器。未覆盖历史 `build/android-v7a/size/libmsff.so.1.0.0`。仍 `CONFIG_DECODERS=0`。不做自动 ABR。
