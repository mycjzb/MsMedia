# MsMedia 设计说明（初稿，待确认）

by: Long <mycjzb@gmail.com>

状态：**P1a–P1i 已落地，P1 整体仍未通过。** P1d 提供 H.264 AUD 或 7.4.1.2.4 无 AUD 逐行子集组帧；P1e/P1f 为预算与固定对象；P1g 用独立编码器输出做开发交叉验证；P1h 补 AAC 余数时间戳与 TS 连续性基本安全丢弃，并做有界实流抽样；P1i 补中途接入等待 SPS/PPS/IDR 的安全恢复，以及 evq 严格 FIFO（不是软解，不是全 H.264 兼容声明，不是原生网络/HLS）。下文多数「目标」仍不是已支持功能。已实现范围以 `docs/PROGRESS.md` 为准；宿主自测不等于 el7/四平台、可播放或硬解。不得把该子集写成全部 H.264 或 P1 已验收。

内核输入字节，或调用方标明类型的协议载荷。核心识别格式、解封装、拼出完整访问单元（AU）。输出轨信息、codec config、PTS/DTS 与时基、关键帧、格式变化和不连续。设备硬解能力核实之后才把 AU 送去解码；核实不了就报告不支持。

## 1. 目标与非目标

目标：

- C11 核心，稳定 C ABI，Android、Windows、Linux、iOS 共用一份核心
- 体积小、内存有界、起播和换台路径短
- 主路径直接交出 AU。兼容用的再封装是可选适配，不作为喂硬解的必经步骤
- 能力分级可查询：容器或清单标识、码流确认、完整组帧、设备硬解。做不到的级别保持关闭
- 播放场景覆盖直播、网络点播、本地文件；控制能力覆盖倍速、快进快退和按时间定位
- 多音轨、字幕、可选 AI 字幕在公共接口中有位置

非目标：

- 播放器界面、FFmpeg、内置软解、转码
- 把调用系统解码 API 当成硬解成功
- 把 HTTP 或 TLS 放进 demux 核心；为缩小体积关闭证书校验、弱化 TLS 或自造加密
- 在核心中放入模型、云 SDK、字体引擎、ASS 排版
- 默认覆盖 WebM
- 承诺所有 QuickTime codec、所有源都不重开连接、固定 1MB 体积、倒放
- 修改任何既有产品

## 2. 模块边界与数据流

```
Source（平台）
  read / seek / size / cancel / range
  能力：本地文件 | 点播 | 直播 | 可否 seek | DVR 窗口
        |
        v
清单调度（可选，跨平台共用逻辑，默认按构建裁剪）
  HLS m3u8、MPEG-DASH MPD
  只决定分片、初始化段、rendition 和刷新
  HTTP 由系统 backend 或可选自有 HTTP backend 执行，不进核心
        |
        v
核心（纯 C，无网络、无解码、无渲染）
  格式识别 → 容器解封装 → 完整 AU / 字幕 cue
        |
        +--> 播放控制（核心外）：速率、时钟、排队、预滚裁剪
        +--> 平台硬解：核实后解码；视频走向 Surface / 纹理
        +--> 字幕渲染（核心外）
        +--> PCM tap --> 可选 AI 插件 --> 字幕 cue
```

实时插件（RTMP、RTSP、RTP、UDP）位于 Source 与核心之间，可整项裁掉。RTP 重排、丢包和 depacketize 产出基本流载荷，这个步骤不是容器 demux。UDP 上的裸 MPEG-TS 才进入 TS 解封装。

核心不创建线程，不睡眠对时，不画字，不访问网络。

## 3. C ABI（提议，待确认后才写成头文件）

头文件将来只有一个公共入口：`include/msmedia/msmedia.h`。函数返回 `ms_status`，不使用异常。`ms_abi_version` 从 1 起。分配器可由调用方提供；不提供时用 malloc/free。分配失败返回 `MS_ERR_NOMEM`，不中止进程。

建议状态码：`OK`、`AGAIN`（需要更多输入）、`EOS`、`UNSUPPORTED`、`UNKNOWN`、`MALFORMED`、`OVERFLOW`、`CANCELED`、`NOMEM`、`STATE`、`INVALID_ARG`、`LIMIT`（反压）、`NOT_SEEKABLE`。

### 推拉、反压、所有权

`ms_session_push` 只在调用期间借用输入指针，返回后核心不再保留该指针。返回值带 `consumed`。`OK` 表示本次长度全部复制进会话缓冲。`LIMIT` 表示会话缓冲已满，已复制的前缀算 consumed，其余由调用方稍后重试；缓冲一字节都不能再收时 consumed 为 0。

`ms_session_pull` 取出一个事件。AU 和 cue 的数据指针属于会话，在 `ms_release`、`flush`、`reset`、`seek` 或销毁之前有效。输出槽有上限。槽未释放时 pull 返回 `LIMIT`，数据留在队列里，不丢弃。push 侧也因此顶满并返回 `LIMIT`。这就是反压。

输入缓冲、探测缓冲、未完成 PES/sample、已完成 AU 的所有权都记在会话上。换台使用 `reset` 或新建会话，旧指针全部失效。

### 控制与 epoch

三个计数器，只增不减，放进每个后续事件：

| 计数器 | 何时增加 | 作用 |
| --- | --- | --- |
| `session_epoch` | 换台、`reset` | 旧频道的 AU、PCM、AI 结果作废 |
| `timeline_epoch` | seek、要求刷新的不连续 | 旧时间轴上的队列、字幕和识别结果作废 |
| `selection_epoch` | 音轨或字幕选择变化 | 旧轨的解码配置、cue、AI 结果作废 |

`flush` 丢掉未完成输入和未取走的输出，保留已确认的轨和 codec config。`reset` 回到探测前状态并增加 `session_epoch`。`cancel` 使之后的 push/pull 返回 `CANCELED`，直到 reset。核心内部没有阻塞等待；cancel 供平台取流和插件停止工作。

`seek(time)` 由核心根据索引找关键帧位置，再要求 Source seek 或 range。成功则增加 `timeline_epoch`，flush，并从该关键帧输出；目标时间之前的视频 AU 带预滚标记，音频由控制层裁到目标时间，字幕队列清空。源没有 seek/range 时返回 `NOT_SEEKABLE`。直播不可 seek 就返回这个错误。HLS/DASH 时移只允许落在源公布的窗口内，窗口外同样失败。

`set_rate` 只记录播放控制层的速率请求。核心不拉伸音频，不因此更换解码器。

轨道选择：`select_track(id | OFF)`。同一容器里已有样本时不重开 Source。样本在另一条清单或另一个 URL 上时（典型是 HLS 备选音轨 rendition），允许换资源，并在事件里说明发生了换资源。不能承诺所有源都不用重开。

### 事件

`FORMAT`、`TRACK`、`TRACK_UPDATE`、`CODEC_CONFIG`、`AU`、`SUBTITLE`、`DISCONTINUITY`、`EOS`、`UNSUPPORTED`。

`TRACK` 含：id、种类（视频/音频/字幕）、codec、四态里已达到的识别级别、语言、名称、default、forced、当前是否选中。节目中途出现或消失用 `TRACK_UPDATE`。

`AU` 含：track、codec、级别、数据指针与长度、时基有理数、PTS、DTS、`has_pts`、`has_dts`、关键帧、不连续、格式变化、预滚。没有时间戳时对应 has_* 为 0，不用 0 冒充有效时间。

`SUBTITLE` 含：时间戳、持续时间、cue 文本或图形包、内嵌文本 / 图形 / 外挂三种来源、是否最终结果。AI 插件的增量结果和最终结果也走 `SUBTITLE`，并带 `ai_partial` / `ai_final` 和三个 epoch。epoch 不匹配的结果由会话丢弃，不交给渲染。

速率不是核心解码事件。控制层读取 `set_rate` 的值和 AU 时间戳自行调度。

### 线程

一个会话同一时刻只由一个线程调用。并发使用是未定义行为，核心不试图在内存已损坏后恢复。平台解码、网络、渲染、时钟用自己的线程，进入同一会话前由调用方串行化。

## 4. 容器、清单与协议（目标，未实现）

两层分开：

| 层 | 目标 | 阶段 | 现在 |
| --- | --- | --- | --- |
| 清单 | HLS（m3u8）、MPEG-DASH（MPD） | P3 | 未实现 |
| 容器 | MPEG-TS、FLV、ISO BMFF：MP4、fMP4、MOV 子集 | TS 在 P1；其余在 P2 | 未实现 |

HLS 的目标分片是 MPEG-TS 和 fMP4。DASH 默认只接 fMP4。WebM 不在默认范围。清单模块发现分片后，把字节交给对应容器解析器。禁止把 m3u8/MPD 写成一种容器 demux。

清单需要表达的差异：直播刷新与点播静态清单、初始化段、媒体分片、`EXT-X-DISCONTINUITY` 或 DASH 时间轴空洞、多变体/Representation、独立的音轨和字幕 rendition。直播窗口按播放列表公布的范围处理。变体切换若 codec config 变化，发格式变化并 flush 视频解码。备选 rendition 可以是另一条资源。

容器要点（实现时按此验收，现在都没写）：

- MPEG-TS：188 或带 4 字节前缀的 192；PAT/PMT；PES；P1 只组 H.264 与 ADTS AAC
- FLV：tag 时间戳、AVC/AAC sequence header、脚本里的关键帧索引（没有索引则 seek 能力为不具备）
- MP4/MOV：`stts`/`stsc`/`stsz`/`stco`/`co64`/`stss`；moov 在头或尾。moov 在尾时依赖 size 与 range/seek，否则返回能力不足。MOV 只保证与下列目标 codec 重叠的样本项，其余 sample entry 返回 unknown
- fMP4：`moof`/`mdat`、`tfdt`/`tfhd`/`trun`、`trex` 默认值；初始化段与媒体段分开喂入

加密单列，见第 13 节。能解析箱或 TS 包不等于能解密。

## 5. 编码识别矩阵（目标，未实现）

四个状态必须分开查询，禁止用一个布尔值「支持」概括：

1. 容器或清单给出的标识
2. 码流确认
3. 完整 AU 组帧
4. 设备硬解（不在核心注册表里，由平台查询）

注册表按编译进库的代码如实填写前三列。名字出现在枚举里而级别为 `NONE`，表示「这是目标标识，本构建不会处理」。探测失败且数据已经够用时返回 `UNKNOWN`。数据不够返回 `AGAIN`。

| Codec | 标识例子（目标规则） | 码流确认（目标规则） | 组帧 | 计划 | 现在 |
| --- | --- | --- | --- | --- | --- |
| H.264 | TS `0x1B`，AVC sample entry | SPS | Annex B AU；AUD 或下一主图像为界；SPS/PPS 作 config；IDR 为关键帧 | P1 组帧 | P1d 子集之上，P1i：中途接入丢弃缺参数集/首个 IDR 之前的残片，不把会话标为失败；隔行/PAFF/MBAFF/FMO 与扩展 NAL 仍明确拒绝；不是全部 H.264；非正式验收 |
| AAC | TS `0x0F` ADTS；MP4 MP4A | ADTS 或 AudioSpecificConfig | 长度字段齐全的一帧 | P1 只做 ADTS 子集（layer=0、rdb=0、sfi 0-12、ch 1-7；MPEG-2/4 ADTS）；LATM/裸流/PCE/多块不做 | P1b：MPEG-TS ADTS 子集组帧与 2 字节 ASC 已实现，并经宿主 debug ASan/UBSan 与 release 自测；不是全部 AAC；LATM 仍不做 |
| H.263 | FLV codec id 等，仅标识 | 图像起始码 | 完整图像 | P4 | 未实现 |
| HEVC | TS `0x24`，hvc1/hev1 | VPS/SPS | IRAP 为关键帧的 AU | P4 | 未实现 |
| MPEG-4 Part 2 | TS `0x10` | VOL | 完整 VOP | P4 | 未实现 |
| MPEG-2 Video | TS `0x02` | 序列头 `00 00 01 B3` 且确认为 MPEG-2 | 图像头组帧 | P4。MPEG-1 不是本目标 | 未实现 |
| AVS+ | 见下段 | 序列头中的广播增强档 | 完整图像 | P4 | 未实现 |
| AV1 | `av01` 只算标识 | OBU / codec config 记录 | 完整帧 | P4 | 未实现 |
| AC-3 | TS `0x81` 或描述符 | 同步字 `0x0B77` | 帧长字段齐全 | P4 | 未实现 |
| MP3 | TS `0x03/0x04` 只算 MPEG 音频标识 | 帧头 layer 3 | 按比特率与采样率的帧长 | P4 | 未实现 |
| MP2 | 同上，未确认 layer 前不得叫 MP2 | layer 2 | 完整帧 | P4 | 未实现 |
| Opus | 标识随 RTP 或以后单列的封装 | TOC | 一包一帧或文档规定的多帧 | P4 能力；默认容器不含 WebM | 未实现 |
| E-AC-3 | 描述符或独立流类型，不能只写成 AC-3 | 同步与 strmtyp | 按 frmsiz 的完整帧 | P4 目标。新增，未实现 | 未实现 |
| G.711 A-law / μ-law | RTP payload type 8 / 0，或 WAV/容器标识 | 载荷类型与声明的采样率一致 | 按采样率切成定时包 | P4 识别目标。新增，未实现。识别不等于硬解 | 未实现 |

AVS+ 不是 AVS2。TS 里常见的 AVS 流类型或「AVS」四字符，只够记成 AVS 家族标识，级别停在标识，codec 不得标成 AVS+。只有序列头档次符合 AVS+（广播增强）才升到码流确认。遇到 AVS2 时返回「非目标：AVS2」，禁止映射成 AVS+。

P1 会把上表每个 id 放进查询函数。除 H.264 与 ADTS AAC 外，级别保持 `NONE`，直到该 codec 的测试合并。参考图里只出现 H.264/HEVC，不缩小这张表。Main10、4K、隔行、PAFF/MBAFF、AAC 5.1、LATM/LOAS 是档次或封装差异，规则在 `docs/COMPATIBILITY.md`。音频识别成功也不等于设备硬解。

## 6. 硬解与禁止软解

平台查询结果字段：`is_hardware`、`verified`、`backend`、说明字符串。`is_hardware = 1` 必须同时满足：选用的解码器被强制为硬件，且回落软件的路径已关闭或已被核实不会发生。只调用 configure / decode 而不能证明这一点时，`is_hardware` 必须为 0，`verified` 为 0。

| 平台 | 目标后端 | 核实要求 |
| --- | --- | --- |
| Android | MediaCodec + Surface | 以硬件组件为准；`c2.android` / `OMX.google` 一类软件组件不能报硬解。API 能提供硬件标志时必须采用 |
| iOS | VideoToolbox | 使用「要求硬件解码器」的属性。只「允许」硬件、失败后仍可软解的属性不够 |
| Windows | 硬件 Media Foundation + D3D11 | 只选硬件 MFT。只有软件 MFT 时返回不支持 |
| Linux | VAAPI、V4L2 或厂商硬解设备 | 打开到硬件设备并查到 profile 才算。没有设备则不支持 |

骨架和桌面宿主上的空实现固定返回非硬解。

音频：系统解码器经常是软件。查询结果使用单独种类：`HARDWARE`、`SOFTWARE`、`SYSTEM_UNVERIFIED`、`NONE`。只有 `HARDWARE` 可以叫做硬解。默认策略是拒绝 `SOFTWARE` 和 `SYSTEM_UNVERIFIED`，不为了出声自动改走软解。若将来要系统音频解码，必须是另一个默认不链接的入口，名字里写明软件或未核实。

Media3 只可能出现在 Android 宿主，不进入核心。

倍速的时间拉伸是可选音频后处理，输入是已经解码的 PCM，不是解码器，见第 11 节。它不能成为引入 AAC/MP3 软解的理由。

## 7. 网络与四平台分工

核心不链接网络，也不链接 TLS。取流使用同一组接口：`read`、`seek`、`size`、`cancel`、`range`、超时。backend 分两类，用编译开关裁剪。一次默认发布只带其中一套，不把系统栈和自有栈强制打进同一个包。

| backend | 职责 | 现在 |
| --- | --- | --- |
| 系统 | Android 平台 HTTP、iOS URLSession、Windows WinHTTP、Linux 宿主网络。TLS 用各系统自带栈 | 未实现 |
| 自有 | HTTP/1.1，以及 RTMP(S)、RTSP(S)。第一版只接 OpenSSL | 未实现 |

解析核心不链接 OpenSSL。自有网络第一版统一用 OpenSSL，不并行再写 mbedTLS、wolfSSL 或第二套 TLS 后端。可以有一层薄封装供 HTTP、RTMP、RTSP 共用，但不做成可插拔的多后端框架。

用户比较过 OpenSSL 与 mbedTLS，认为 OpenSSL 更稳定，并需要复用 OpenSSL 的算法。这是用户自己的经验，不是本项目做出的普遍对比测试。wolfSSL 未测，不纳入第一版。

OpenSSL 只提供 TLS 和算法，不提供 HTTP。HTTPS、RTMPS、RTSPS 共用同一套 TLS 封装和连接 I/O，证书、主机名、SNI 只写一处。摘要、HMAC、加解密在协议需要时调用 OpenSSL 仍在维护的公开高层 API，不自造算法，也不把 OpenSSL 源码抄进仓库。协议若遗留旧摘要，只在该协议路径上调用对应 API，并与 TLS 的协议版本和套件策略分开。不为了旧协议打开 legacy provider，也不因此放宽 TLS。

依赖关系：

- `core`：无套接字、无 HTTP、无 OpenSSL
- `net`：可选。HTTP/1.1、连接、取消、超时，以及一份 OpenSSL 封装
- RTMP、RTSP：需要 TLS 或摘要时调用这份封装，不再各链一份 OpenSSL

静态或动态按部署选一种。体积和裁剪效果以实测为准，不预写数字，也不指定未经核对的「当前最新版」。选型原则是仍受安全维护的 OpenSSL 版本；具体版本号在引入时核对后写入 `docs/THIRD_PARTY.md`。

系统网络 backend 仍然保留，作为不链 OpenSSL 的替代构建。默认包只打一套网络，不把系统栈和自有 OpenSSL 栈强制打在一起。此节是设计定稿，不是已经编码。

HTTP 初期只做 HTTP/1.1。HTTP/2 和 HTTP/3 在没有完整实现与测试前不列入支持。

自有 HTTP 与 TLS 尚未实现，实现时必须同时满足：

- 响应体长度、分块解码、内容压缩都有上限，超出即失败
- 重定向有次数上限；跨主机时去掉 Authorization、Cookie 和用户信息
- 连接复用只在同一主机且证书身份匹配时发生
- Range 供点播 seek
- 超时、取消、代理、DNS、IPv4/IPv6
- 证书链和主机名严格校验，带 SNI；信任库的来源和更新方式写进构建说明
- 不为体积关闭校验，不启用弱协议或弱套件，不自造加密

OpenSSL 还没有放进仓库。引入时登记版本、许可证、关闭开关，并跟随该版本的安全更新。MsMedia 自己的许可证也还没选定。

| 职责 | Android | iOS | Windows | Linux |
| --- | --- | --- | --- | --- |
| HTTP/HTTPS | 上表两种 backend 之二 | 同左 | 同左 | 同左 |
| 硬解 | 见第 6 节 | 见第 6 节 | 见第 6 节 | 见第 6 节 |
| 渲染 | Surface | 平台图形对象 | D3D11 纹理 | 硬件表面；渲染模块在核心外 |
| 时钟 | 宿主时钟 | 宿主时钟 | 宿主时钟 | 宿主时钟 |

核心输出时间戳。对时、丢帧和等待都属于播放控制。PCR 或分片时间可以另作时钟样本事件，核心不因为等 PCR 而扣住已经完整的 AU。

## 8. RTMP / RTSP / RTP / UDP

各自独立插件，默认不编译。允许以后采用许可兼容的开源实现或自写，但必须登记来源、许可证和依赖。RTMPS 与 RTSPS 使用第 7 节的共用 TLS，不再各链一份 OpenSSL。

RTSP 是会话和控制，媒体在 RTP。RTP 插件负责会话、重排、丢包和 depacketize。UDP 插件负责套接字；载荷是 TS 时交给 TS 解析，载荷是 RTP 时交给 RTP 插件。RTMP 消息流同样在插件内拆成音视频载荷后再进核心。

## 9. 缓冲、畸形输入、日志与模糊测试

建议默认上限（P1e 已按暂定值实现，仍待最终确认；调用方只能调低）：探测 1 MiB、AU 队列 2 MiB、输出槽 8、单次 push 1 MiB、确认后重同步 256 KiB、单个 PES/ES 组装 2 MiB、会话动态总预算 16 MiB。绝对上限：探测 8 MiB、AU 队列 16 MiB、槽 64、单次 push 8 MiB、重同步 1 MiB、组装 16 MiB、总预算 64 MiB。单 PES/AU 超过组装上限为 `MALFORMED`；动态预算或输出队列暂时用尽为 `LIMIT`。声明长度不得导致无界分配。

格式确认后的重同步必须有最大字节数。超过则失败并停止吞数据。TS 探测阶段只在探测上限内寻找同步周期。

EOS：先交出已完整的 AU，尾部不完整的单元丢弃并带告警，然后 `EOS`。不把半帧当成 AU。

日志回调默认空。常规日志不带凭据和完整载荷。发行构建不编译十六进制转储。

模糊测试从 P1 起挂在 push/pull/reset 上，输入和会话都有上限。ASan/UBSan 是诊断构建。宿主或临时容器里的结果要注明工具链，不能写成四平台正式发版。缺某平台工具链就写未验证。

## 10. 音轨与字幕

音轨：枚举、语言、名称、default、选择与关闭、节目中途增减。切换时更换 codec config、flush 音频解码器、让控制层重新对齐时钟，并增加 `selection_epoch`。样本仍来自当前字节流时不重开取流。HLS 备选 rendition 这类情况允许，也必须，切换到另一条资源。

字幕三类：

| 种类 | 核心做什么 | 核心不做什么 |
| --- | --- | --- |
| 内嵌文本 | 输出带时间戳的文本 cue。首个子集在 P4 再定（待确认） | 排版、字体、描边 |
| 图形字幕 | 输出带时间戳的图形包（例如位图区域） | 把图形合成到画面 |
| 外挂 | 独立文件或 URL 的 cue 包，时间映射到当前时间轴 | 下载外挂（由平台 Source 读入） |

ASS/SSA 的复杂排版若要做，只能在外部渲染器。核心至多把带时间的文本或原始包交出去。P4 之前字幕接口只有事件形状，没有解析器。forced 和 default 来自容器或清单的标记，供宿主选择；核心不因为 forced 就自动盖住用户的「关闭」。

动态变化走 `TRACK_UPDATE`。seek 和换台清空尚未显示的 cue。

## 11. 播放场景、seek 与倍速

Source 能力：`read`、`seek`、`size`、`cancel`、`range`，以及本地 / 点播 / 直播、可否 seek、DVR 窗口起止。核心负责索引、时间轴和关键帧位置。速率、主时钟和送显调度在播放控制层。

| 场景 | 定位 |
| --- | --- |
| 本地文件 | 有 size 时可以做尾部 moov 和关键帧索引 |
| 网络点播 | 优先 range；没有 range 则 seek 能力为不具备 |
| 直播 | 源不提供时移则 `NOT_SEEKABLE` |
| HLS/DASH 时移 | 只在源给出的窗口内定位 |

快进和快退是相对当前时间的 seek，不是倒放。本设计不实现倒放。

seek 成功之后的固定顺序：`timeline_epoch` 增加、flush、从目标前的关键帧预滚、音频裁到目标 PTS、字幕清空、进行中的 AI 结果作废。

倍速：

- 控制层按速率解释时间戳，视频 AU 仍来自硬解
- 核心和默认构建不包含音频 time-stretch
- time-stretch 若以后存在，是可选外挂，输入为 PCM，用于保音调，不解码压缩音频
- 未启用该外挂时的听感策略是待确认项。可选方案只有：视频变速且音频静音、或明确启用「变调」后处理（改变播放采样率，仍不解码）。两条都不得链接软解

## 12. AI 字幕（可选插件，未实现）

插件不链进默认核心，核心不包含模型、云 SDK 和供应商地址。当前不选择供应商，不调用外部 API，不下载模型。

输入是平台硬解（或已诚实分类的音频解码）之后的 PCM tap，附带采样率、声道布局和 PTS。插件自己重采样。压缩基本流不直接送去识别。硬解路径拿不到 PCM 时，能力查询返回 AI 字幕不可用，核心禁止为此加软解。

识别异步进行。增量 cue 与最终 cue 都带 epoch 和「估计延迟」，供渲染对齐时间轴。seek、换轨、换台：取消在途识别，丢掉 epoch 不匹配的回调。

本地文件可以在用户同意后预生成并缓存文本。直播只滚动生成。默认不上传音频。云服务必须由用户另行明确选择并授权；密钥和费用放在插件配置里，不进入核心日志。翻译只预留「在最终文本上再产出一种语言 cue」的事件，P5 不接具体翻译实现。

## 13. 加密与 DRM

下列各项彼此不同，都必须单独列出支持级别：

- HLS AES-128 分片加密
- HLS SAMPLE-AES
- CENC 以及各 DRM 系统

未实现或没有调用方授权（密钥、许可证响应）时返回 `UNSUPPORTED`。容器结构解析成功只说明箱或分片可读，不说明样本可解码。核心默认不带 DRM 栈。

## 14. 体积、内存与首屏怎么量

P1 起记录，不设「必须小于 1MB」的承诺。

- 体积：静态库，写明编译器及版本、优化级别、strip、开启的格式开关、目标 ABI。只比较同一组条件
- 内存：测试分配器统计探测峰值、队列达到上限时的峰值、reset 之后是否回到基线。峰值必须受队列上限约束，不随「已拒绝的输入」增长
- 首屏：在进程内对合成样本度量两个字节数：得到 codec config 时已消耗的输入，以及第一个完整关键帧 AU 时已消耗的输入。这不是屏幕上出现像素的时间。像素时间要等平台渲染存在后再单独定义

宿主测量结果标注为宿主。四平台正式体积以各平台工具链为准。

## 15. 构建目录

P1a 已添加公共头、核心源码、`tests/` 和根目录 `build.sh`。尚未添加平台桥、网络模块或协议插件目录中的实现。

```
include/msmedia/msmedia.h
src/core/          纯 C
src/container/     ts，以及后续 flv、bmff
src/codec/
src/platform/      android ios windows linux 薄桥
src/net/           可选。自有 HTTP/1.1 与一份 OpenSSL 封装，不链进 core
plugins/           hls dash rtmp rtsp rtp udp ai subtitle
tests/ examples/ docs/
build.sh
```

`build.sh` 使用 `set -euo pipefail`。标准目标为 `default`、`debug`、`release`、`release upx`（两个参数；`release-upx` 为别名）、`clean`。另有可选的 `test`（按 debug 编译并运行 `test_core`）。`release upx` 只允许通过 `/usr/local/bin/pack`（其次 `/home/long/bin/pack`）压缩可执行文件；工具不存在则该目标失败并明确报错。不对静态库或共享库做 UPX。核心链接行不出现网络库、TLS 库和解码库。

## 16. 和路线图的对应

| 设计里的接口 | 实现阶段 | 现在 |
| --- | --- | --- |
| push/pull、epoch、注册表、TS 同步与 PAT/PMT | P1a | 会话与 TS 识别/轨道发现已实现；H.264/AAC AU 见下行与 P1b/P1d |
| H.264 完整 AU 组帧 | P1 | P1d 子集 + P1i 中途接入等待 SPS/PPS/IDR；不把 PES/NAL 或 first_mb 当通用边界；隔行/FMO 仍拒绝；不是安全出画判定 |
| AAC ADTS 完整 AU 与 AudioSpecificConfig | P1b | 子集已实现并经宿主自测通过；非 el7/四平台/可播放验收 |
| 硬解查询骨架 | P1a | 本构建固定返回非硬解/未核实 |
| 体积/内存/首个 AU 测量 | P1 | P1f 已测固定对象与双轨峰值；P1g–P1i 另有交叉验证、余数/连续性、中途接入与 evq FIFO 回归及有界实流抽样（见 PROGRESS）。不是四平台或首个真实关键帧出画测量；P1 未通过 |
| FLV、MP4、fMP4、MOV 子集、文件/点播/直播 Source、seek。速率只保存数值，不处理听感 | P2 | 未实现 |
| HLS、DASH，然后 RTMP/RTSP/RTP/UDP | P3 | 未实现 |
| 其余 codec 组帧、多音轨、字幕、倍速听感 | P4 | 未实现 |
| AI 转写/翻译插件 | P5 | 未实现 |

阶段门槛和「未通过不进入下一阶段」写在 `docs/ROADMAP.md`。

## 17. 快速换台（外部参考，未实现，未验证）

本节来自一张快速换台参考图，只用来约束以后的设计。它不是本项目的实现，也不是测试证据。图里的运营商、设备品牌和出画时间都没有在本项目里验证过。

按下即连属于宿主界面和播放控制层的可选能力，不进入核心。手指按下可以提前发起取流；松手才进入播放；取消或滑走必须取消还没采用的连接。控制层限制并发路数和流量。

FCC 是依赖服务端的可选插件，默认不编译，不链入核心。图中写的电信、中兴、烽火、华为两类支持均未验证，本设计不承诺与其中任何一种兼容。以后若做，必须单独写明并在真实服务端上验证：单播突发到组播的切换点、重叠数据如何去重、时间戳如何标成连续或不连续（必要时增加 `timeline_epoch`）、超时后回退到普通起播或明确失败。这些条件没满足时，能力查询为不可用。

没有 IDR 时，不能因为一帧 I 图像带了参数集就当作可以安全出画。H.264 还要判定恢复点以及参考帧是否齐全；开放 GOP 里参考不完整的前导帧不能送显。HEVC 要把 IDR、CRA、BLA 分开，并分别处理 RASL/RADL 等 leading pictures。证据不够就继续等到下一个安全恢复点，这段时间保持未出画。完整判定尚未实现。P1i 只在组帧层丢弃尚无 SPS/PPS 的残片以及尚未发出过 IDR 的图像，避免把它们作为 AU 交给下游；这不等于开放 GOP 参考齐全或可以安全出画/硬解。

图中「4K HEVC Main10、50 帧/秒、36 Mbps、电视盒子 0.35 秒出画」是他方宣传。它只可当作以后写测试条件时的对照样本，不是本项目实测，也不是通用指标。真要测量时，记录计时起止（按下、首包、安全恢复点、首帧送显）、冷启动还是热启动、设备、片源、网络、至少中位数和高分位、以及卡顿率。单次 0.35 秒不能写成达标。

这一节不改变 P0–P5 的完成定义。详见路线图里的可选项。

## 18. 待确认项

1. 许可证选哪一种。选定前没有 LICENSE 文件，也不公开实现代码。
2. 探测、队列、输出槽、单次 push 的默认上限是否采用第 9 节的建议值。**P1a/P1e 暂定采用**：探测 1 MiB、AU 队列 2 MiB、输出槽 8、单次 push 1 MiB；P1e 另增组装 2 MiB、动态总预算 16 MiB。调用方只能调低。仍待最终确认。
3. TS 在格式确认之后，有界重同步的字节上限，以及用尽之后是会话失败还是只丢弃当前包。**P1a 暂定**：默认重同步窗口 256 KiB（只能调低，绝对上限 1 MiB）；用尽后会话失败并返回 `MALFORMED`，不再继续吞数据。仍待最终确认。
4. 未启用保音调外挂时，倍速音频采用静音还是允许变调后处理。
5. P4 第一种文本字幕子集的顺序（外挂 SRT/WebVTT、内嵌 WebVTT、tx3g 等）。
6. Opus 在不做 WebM 的前提下，第一入口是否仅为 RTP 插件。
7. 系统音频解码是否永远不提供，还是允许一个默认不编译、名称写明软件/未核实的独立入口。
8. 版权声明里的法律主体名称是否就用作者名 Long。本文不编造公司名。
9. 安全联系邮箱是否就用作者公开邮箱，还是另设。
10. 按下预连接和 FCC 插件是否要做。默认都不进核心；在第 17 节的验证条件完成前不写支持。
11. 自有网络的 TLS 已定为 OpenSSL，不并行第二套 TLS 库。仍待确认的是默认包启用自有栈还是系统栈。确认前不引入 OpenSSL 源码，本轮也未实现网络模块。

作者署名已按要求写入：by: Long <mycjzb@gmail.com>。
