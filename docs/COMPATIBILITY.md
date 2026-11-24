# 兼容性参考归类

by: Long <mycjzb@gmail.com>

来源是一张兼容性清单参考图，用作目标输入。多数项仍未实现。已落地的解析子集以 `docs/PROGRESS.md` 为准（非正式发版验收），不把子集写成该行全部场景已通过。

归类四层。列表、EPG、回看模板不进入核心。udpxy、msd_lite、rtp2httpd 通常是把组播转成 HTTP 的服务；客户端只消费它们给出的 HTTP，不在客户端里再实现这些服务端。

加密和 DRM 没有实现、也没有授权时返回不支持。解析到容器不等于解密。

音频能识别不等于设备硬解。硬解查询失败时不软解兜底。图中视频只列了 H.264/HEVC，不替换 `docs/DESIGN.md` 里的完整 codec 表。E-AC-3 和 G.711 是新增目标，仍未实现。
用户补充的更广对照清单（更多编码、封装、协议、广播、HDR）只在 `docs/DESIGN.md` / `docs/ROADMAP.md` 列为「已定核心」与「待评估纳入」；本轮不扩展那些新项的实现，不一次性宣称全支持。

| 项 | 归类 | 阶段 | 验证条件（现在都未测） |
| --- | --- | --- | --- |
| HTTP(S) 直连 MPEG-TS | 核心解析 TS；网络走第 7 节 backend | 可选 `libmsnet` 已能 GET 字节（非正式验收）；不是完整点播播放器 | 本机受控服务已测 200/206/证书与主机名校验；实路抽样不公开地址；不是四平台验收 |
| HLS TS / fMP4 | 清单调度，不是容器 demux | P3（非正式验收）：media playlist VOD/live 窗口、URI、刷新去重、DISCONTINUITY、fMP4 MAP；经 fetch 外挂请求分片后复用已有 TS/fMP4 AU。调度器有 max_bytes 总预算。可选 `libmsnet` 可将该 fetch 接到真实 HTTP/HTTPS。仍不是 master/ABR/KEY/LL-HLS | 宿主内存/文件 harness 与本机受控 HTTP 取流均已闭环出 AU；master 仍明确拒绝；不可写成可播放或硬解出画 |
| MPEG-DASH（MPD）fMP4 / TS | 清单调度，不是容器 demux | P3（非正式验收）最小子集：单 Period、单 AdaptationSet、单 Representation；SegmentBase（无 indexRange）/ List / Template（有界 `$Number$`/`$Time$`）；Initialization + media，可选字节范围；static/dynamic 有界刷新去重；复用 HLS fetch 与会话 AU。明确拒绝 ABR、ContentProtection/DRM、xlink、分轨 AS、多 Period、CMAF LL 全集、indexRange/sidx | 宿主内存/文件 harness 已闭环出 AU（缺片/截断/取消/反压/预算/拒绝路径已覆盖）；`test_net` 已将同一调度接到 `libmsnet` 做本机受控 HTTP 闭环（TS 分段：200/Range/缺片/截断/取消/反压/ContentProtection 拒绝）。不可写成全部 profile、可播放、硬解出画或四平台实机验收 |
| HLS 加密 | 可选业务；无密钥即不支持 | 未排期 | 与 AES-128、SAMPLE-AES、CENC 分开报 |
| HTTP-FLV | 核心解析经典 FLV 容器；HTTP 取流仍是网络 backend | P2e：经典 FLV 的 H264/AAC AU（非正式验收）；不是 HTTP 客户端，不是 RTMP，不是 Enhanced FLV/HEVC，不是脚本索引或 seek | 宿主 lavfi 单轨/双轨/B 帧 FLV 已对照独立 tag 与 payload/PTS/DTS/keyframe；不是可播放或硬解出画 |
| MP4 点播 | 核心解析 MP4 子集 | 渐进 moov 在前或在尾的 H264/AAC AU（有界回看或 Source 随机读）；已定稿渐进 MP4/MOV 可按 sample 表做时间定位并落到真实关键帧（非正式验收）。无 Source 且回看超限仍为 `NOT_SEEKABLE`。不是完整点播播放器，不是网络客户端 | 宿主合成与 lavfi（含 B 帧、尾 moov、`--source --max-lookback 16`）已对照独立表与 payload/PTS/DTS/keyframe；不是硬解出画 |
| fMP4 分片 | 核心解析 fMP4 子集 | P2d：init+顺序 moof/mdat 的 H264/AAC AU（非正式验收）；加密/缺 init/moof 在 moov 前明确错误或等待，不吐假帧；不是 CMAF 全集、不是解密、不是清单或 seek | 宿主 lavfi 多片/双轨/B 帧 fMP4 已对照独立表与 payload/PTS/DTS；四平台硬解未验证 |
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
| AAC ADTS、LATM/LOAS、5.1 | 核心解析 | P1b 仅 TS+ADTS 子集组帧；P1j 补 PMT 前完整帧回放、残缺尾帧不发出；P2b/P2c 渐进 MP4/MOV、P2d fMP4 与 P2e 经典 FLV 为裸 AAC AU + ASC（非正式验收）；LATM/PCE/多块/5.1 扩展仍不做 | 5.1 识别成功仍要硬解查询；组帧不等于硬解或全部 AAC |
| MP2、MP3、AC-3、E-AC-3 | 核心解析 | P4 | E-AC-3 不得标成 AC-3 |
| G.711 A-law / μ-law | 核心解析目标 | P4 | 识别不等于硬解 |
| m3u/m3u8、TXT `#genre#`、XMLTV/gzip、tvg 字段、同名多线路、多份列表 | 可选业务扩展 | 不进核心，未排期 | 样例播放列表；失败不影响 demux |
| 回看 default/append/shift/timeshift、flussonic、xc、vod | 可选业务扩展 | 不进核心，未排期 | 每种模板单独用例；无模板则不支持 |
| RTSP Basic/Digest | 协议插件的认证 | P3 才可能 | 只用 HTTPS/RTSPS 或同等保护的通道传递；重定向跨主机不转发 |


## 2026-11-20 21:21 CST — 可选 FFmpeg 9.1 后端与格式枚举追加

`ms_session` 既有 ABI 保持兼容。`ms_format` 在末尾追加 `MS_FORMAT_HLS=6`、`MS_FORMAT_DASH=7`、`MS_FORMAT_RTSP=8`、`MS_FORMAT_RTMP=9`、`MS_FORMAT_RTP=10`（旧偏移不变）。可选 `libmsff` 使用独立 soname `libmsff.so.1` 与 `MSMEDIA_1.0` 版本脚本中的 `ms_ff_*` 符号，不链入 `libmsmedia.so`。2026-11-21 11:26 CST 起追加 `ms_ff_openssl_version` / `ms_ff_openssl_version_num`（仍为同一版本脚本，布局不变）；静态合入的 OpenSSL 符号不动态导出，以免与 App 自带 OpenSSL 冲突。旧核心与 `test_core` / `test_net` / 四组交叉验证仍通过。不是四平台 ABI 验收。细节见 `docs/FFMPEG_BACKEND.md`。


## 2026-11-20 22:07 CST — DASH-over-HTTP 与 UDP/RTP 子集

公共 `ms_event` 布局不变。2026-11-20 22:07 曾把合法负 PTS 标为 `has_pts=0`，该做法已废弃。2026-11-21 11:12 CST 起：相对 uint64 + 会话级 origin，合法负值可经 `ms_event_signed_ts` 还原；追加 `MS_ERR_TIMEOUT`（既有枚举值不变），ABI 版本仍为 1。2026-11-21 11:58 CST 起，RTP/SDP 映射为 `MS_FORMAT_RTP`，RTSP 为 `MS_FORMAT_RTSP`，RTMP URL 为 `MS_FORMAT_RTMP`（即使 demuxer 名是 flv），不伪造成 TS。不是四平台 ABI 验收。能力矩阵见 `docs/FFMPEG_BACKEND.md`。


## 2026-11-21 11:12 CST — 有符号时间与 TIMEOUT

`MSMEDIA_ABI_VERSION` 仍为 1，不改 `ms_event` 字段类型或偏移。新增 `MS_ERR_TIMEOUT` 与时间还原函数，旧客户端需忽略未知状态码并重新编译才能使用新符号。`io_timeout_ms` 不再返回伪装的 `CANCELED`。自写容器对负 PTS 仍为 `UNSUPPORTED`。

## 2026-11-22 00:59 CST — 加法格式枚举 Matroska/WebM

`MSMEDIA_ABI_VERSION` 仍为 1。`ms_format` 在末尾追加 `MS_FORMAT_MATROSKA=11`、`MS_FORMAT_WEBM=12`（旧偏移不变）。同一 FFmpeg demuxer 名为 `matroska,webm`；仅当 URL 可辨为 `.webm` 时映射为 `MS_FORMAT_WEBM`，否则为 `MS_FORMAT_MATROSKA`。可选 `libmsff` 白名单加入 `matroska,webm`。旧核心与既有交叉验证仍通过。不是四平台 ABI 验收。

## 2026-11-22 03:03 CST — ms_event 修剪侧通道（版本仍为 1）

已交付 1838 的 `ms_event` 没有 `struct_size`。在该结构尾部追加 `skip_samples_start/end` 会使 `pull` 向旧客户端 176 字节对象多写 8 字节，这不是兼容追加。已恢复 1838 布局；修剪改为带 `struct_size` 的 `ms_event_trim`，由 `ms_session_event_trim` / `ms_ff_event_trim` 在 `release` 前按 `release_id` 查询，播放器经加法 `ms_hw_player_note_event_trim` 接收。`MSMEDIA_ABI_VERSION` 与 `MSMEDIA_1.0` 符号版本仍为 1，旧符号保留。独立旧头客户端 + 当前 .so + ASan 金丝雀已在宿主验证。不是四平台 ABI 验收。

## 2026-11-22 03:25 CST — trim sidecar 保持（版本仍为 1）

本轮只收口播放路径上的 PCM trim carry，不改 1838 `ms_event` 布局，也不改 `ms_event_trim` / `ms_session_event_trim` / `ms_ff_event_trim` / `ms_hw_player_note_event_trim` 接口。`MSMEDIA_ABI_VERSION` 仍为 1。旧头动态兼容专项再次通过（`sizeof(ms_event)=176`，金丝雀完好，`abi=1`）。不是四平台 ABI 验收。


## 2026-11-22 03:51 CST — 直播窗口定位加法 API（版本仍为 1）

`MSMEDIA_ABI_VERSION` 与 `MSMEDIA_1.0` 仍为 1。在 `ms_ff` / `ms_hw_player` 末尾追加 `ms_ff_get_seek_range`、`ms_ff_seek_live_edge`、`ms_hw_player_note_seek_range` / `get_seek_range` / `seek_live_edge`，结构带 `struct_size`，旧符号与 1838 `ms_event` 布局、trim sidecar 均未改。`ms_ff_is_seekable` 对 HLS EVENT/滑动窗口现为可定位；`ms_ff_duration_90k` 对直播保持 -1。旧头动态客户端不引用新符号；`test_abi_old_header` 于本轮官方矩阵再次通过（`sizeof(ms_event)=176`，金丝雀完好，`abi=1`）。不是四平台 ABI 验收。

## 2026-11-22 04:16 CST — 可选完整 ASS 栅格（版本仍为 1）

`MSMEDIA_ABI_VERSION` 与 `MSMEDIA_1.0` 仍为 1。新增独立头 `include/msmedia/ms_ass_render.h` 与可选 so `libmsass`（符号版本仍 `MSMEDIA_1.0`），以及 `ms_hw_player_ass_*` 加法接口。核心 `libmsmedia` / `libmsff` 不 NEEDED `libmsass`。旧头动态客户端不引用新符号；`test_abi_old_header` 于本轮官方矩阵再次通过（`sizeof(ms_event)=176`，金丝雀完好，`abi=1`）。不是四平台 ABI 验收。

## 2026-11-22 04:50 CST — 可选 AI 字幕加法 ABI（版本仍为 1）

`MSMEDIA_ABI_VERSION` 与 `MSMEDIA_1.0` 仍为 1。在 `ms_sub_cue` 位图字段之后追加 `ai_*` 字段，以及 `MS_SUB_CAP_AI_*` 与 `MS_TRACK_ID_AI_SUB`；旧 `struct_size` 停在 bitmap 之后则不得读写。新增独立头 `include/msmedia/ms_ai_sub.h` 与可选 so `libmsai`（符号版本仍 `MSMEDIA_1.0`），以及 `ms_hw_audio_set_pcm_tap` 与 `ms_hw_player_ai_*` 加法接口。核心 `libmsmedia` / `libmsff` 不 NEEDED `libmsai`；`libmshw` 仅 dlopen。1838 `ms_event` 布局未改。旧头动态客户端不引用新符号；`test_abi_old_header` 于本轮官方矩阵再次通过（`sizeof(ms_event)=176`，金丝雀完好，`abi=1`）。不是四平台 ABI 验收。

2026-11-22 05:18 CST：加法 `ms_hw_vaapi_*` 与 Linux 默认查询行为已进入 `MSMEDIA_HW_1.0`（ABI 版本仍为 1）。1838 旧头客户端仍动态通过（`sizeof(ms_event)=176`）。Linux 无设备时硬解查询为非硬解，不是该行格式已在设备上通过。Windows / iOS 行仍未实现。
