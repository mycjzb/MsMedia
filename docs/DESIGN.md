# MsMedia 设计说明（初稿，待确认）

by: Long <mycjzb@gmail.com>

状态：**P1a–P1j、P2a–P2e、Source/渐进定位、P3 首步（自写 HLS/DASH/`libmsnet`，截至 20:34 CST）已有宿主自测；2026-11-20 21:21 CST 起增加可选官方 FFmpeg 9.1 裁剪后端首个可验证接入；22:07 CST 补 DASH-over-HTTP 与 UDP/RTP 子集联调（见 `docs/FFMPEG_BACKEND.md`）。P1 整体仍未通过，P2/P3 全功能未完成，RTSP/RTMP 未联调。** P1d 提供 H.264 AUD 或 7.4.1.2.4 无 AUD 逐行子集组帧；P1e/P1f 为预算与固定对象；P1g 用独立编码器输出做开发交叉验证；P1h 补 AAC 余数时间戳与 TS 连续性基本安全丢弃，并做有界实流抽样；P1i 补中途接入等待 SPS/PPS/IDR 的安全恢复，以及 evq 严格 FIFO；P1j 补首次 PMT 前完整媒体包的有界回放，残缺 ADTS/PES 仍不作为 AU（不是软解，不是全 H.264 兼容声明，不是原生网络/HLS）。下文多数「目标」仍不是已支持功能。已实现范围以 `docs/PROGRESS.md` 为准；宿主自测不等于 el7/四平台、可播放或硬解。不得把该子集写成全部 H.264 或 P1 已验收。

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

- 播放器界面、内置软解、转码。（历史：至 2026-11-20 20:34 CST，「产品不链 FFmpeg」是非目标。2026-11-20 用户明确要求改用官方最新 FFmpeg 9 裁剪做取流/协议/解封装后，该条仅仍禁止把 FFmpeg 链进 `libmsmedia` 核心、禁止软解/转码；可选隔离后端 `libmsff` 按 `docs/FFMPEG_BACKEND.md` 接入。）
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

`ms_session_push` 只在调用期间借用输入指针，返回后核心不再保留该指针。返回值带 `consumed`。`OK` 表示本次长度全部被会话接受（未确认或仍走输入窗时会拷入 `in_buf`；已确认的 FLV/BMFF 可直接喂解析器）。`LIMIT` 表示会话缓冲已满，已复制的前缀算 consumed，其余由调用方稍后重试；缓冲一字节都不能再收时 consumed 为 0。

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

`seek(time)` 由核心根据索引找关键帧位置，再要求 Source 按偏移读取。成功则增加 `timeline_epoch`，flush 未取走队列（与 `flush` 一样会使未 release 的 outstanding 指针失效；已成功 pull 且已在调用方手中的事件仍须由调用方 release），并从该关键帧输出；目标时间之前的视频 AU 带 `preroll=1`，音频从请求时间起输出、目标之前的裁剪由控制层做，显示丢弃也由控制层做。本核心不擅自改用户解码器状态。源没有随机读或没有确定大小、或格式不在本子集时返回 `NOT_SEEKABLE`。UDP/RTP/RTMP/RTSP 以及 FFmpeg 9.1 对 `is_live` 拒绝定位的 DASH 直播保持该错误，不得假报可定位。HLS EVENT/滑动窗口可通过 `ms_ff_get_seek_range` 查询 start/end/live_edge（90 kHz，相对会话 origin）；`ms_ff_duration_90k` 对直播为 -1，不得把窗口长度当 VOD 时长。目标落在窗口之前或越过 live_edge（窗口末端减去一个目标时长/未完成末片的安全距离）时钳位并返回 OK。不另建 DVR 录制服务器，不无限本地缓存。

本步已落地的公共 Source 是小 vtable：`struct_size`、`ctx`、`size_fn`、`read_at_fn`、可选 `cancel_fn`。`set_source` 拷贝函数指针，不接管 `ctx`。缓冲区由调用方持有且仅在当次回调内有效。允许短读；偏移处已无数据时 `*out_n=0` 且 `OK`（EOF）；会阻塞时返回 `AGAIN` 且不读出字节，`ms_session_drive` 每次最多一次 `read_at`，核心不内部忙等。回调不得重入任何会话 API，且只在该会话同一线程调用。内存源与 POSIX 本地文件（`open`/`pread`/`lseek`/`close`）是开发适配器，不是网络客户端。旧 `push`/`pull` 在未挂 Source 时行为与从前一致。

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
| 清单 | HLS（m3u8）、MPEG-DASH（MPD） | P3 | HLS media playlist 与 DASH MPD 调度最小子集已有宿主自测（非正式验收）；可选 `libmsnet` 可经真实 HTTP/HTTPS 取流（HLS 与 DASH 均可经 `ms_net_hls_fetch_bind` 接到同一 fetch；DASH 的 HTTP 闭环在 `test_net` 以 TS 分段验证，fMP4 主路径仍以内存/文件 harness 覆盖）。master/ABR/加密/LL-HLS、DASH 多 Period / 分轨 AS / xlink / indexRange / CMAF LL / 全部 profile 仍明确未支持 |
| 容器 | MPEG-TS、FLV、ISO BMFF：MP4、fMP4、MOV 子集 | TS 在 P1；其余在 P2 | MPEG-TS 子集见 PROGRESS（P1a–P1j，非正式验收）；P2a 箱体基础 + P2b/P2c 渐进 MP4/MOV + P2d fMP4 init+moof/mdat + **P2e 经典 FLV** 的 H264/AAC AU（非正式验收）；**Source 随机读**与**渐进 MP4/MOV 时间定位**已在宿主自测落地，fMP4/FLV/TS 时间定位明确 `NOT_SEEKABLE` |

HLS 的目标分片是 MPEG-TS 和 fMP4。DASH 本步主路径是 fMP4，也可把 TS Representation 交给已有 TS 解析；WebM 不在默认范围。清单模块发现分片后，把字节交给对应容器解析器。禁止把 m3u8/MPD 写成一种容器 demux。P3 已实现 HLS media playlist 调度子集（VOD/live 窗口、URI、去重刷新、DISCONTINUITY、MAP），以及 DASH MPD 调度最小子集（单 Period / 单 AdaptationSet / 单 Representation，SegmentBase 无 indexRange、SegmentList、SegmentTemplate 有界 `$Number$`/`$Time$` 等，Initialization + media，可选字节范围，static/dynamic 有界刷新去重）。二者复用同一 fetch 外挂与会话不连续/反压合同；不是 master / ABR 自动选路。因会话 fMP4 路径只接受一次 Initialization/`moov`，独立音视频 AdaptationSet 与后继 Period 的新 init 明确 `UNSUPPORTED`，不会在不连续后默默拼接第二份 init。XML 使用有界自制分词，拒绝 `<!`（含 DOCTYPE/实体/CDATA）与外部实体，不引入重型 XML 库。内存/文件 fetch 仍是不链网络时的开发 harness。可选 `libmsnet` 已提供可取消的 HTTP/1.1 GET 与 OpenSSL TLS，并可接到同一 fetch 接口供 HLS 与 DASH 使用（非正式验收，非正式四平台）。`test_net` 已覆盖 DASH MPD 受控 HTTP 出 TS AU（200、Range/206、缺片/截断不连续、取消、反压、ContentProtection 拒绝）。HLS 与 DASH 调度器各有自己的 `max_bytes` 总预算（默认按当前分量上限合计，绝对值 4 MiB），不计入会话动态预算，但不能因此无上限。

清单需要表达的差异：直播刷新与点播静态清单、初始化段、媒体分片、`EXT-X-DISCONTINUITY` 或 DASH 时间轴空洞、多变体/Representation、独立的音轨和字幕 rendition。直播窗口按播放列表公布的范围处理。变体切换若 codec config 变化，发格式变化并 flush 视频解码。备选 rendition 可以是另一条资源。

容器要点（实现时按此验收；MPEG-TS、ISO BMFF 子集与经典 FLV 已部分落地，seek / Source 仍未写）：

- MPEG-TS：188 或带 4 字节前缀的 192；PAT/PMT；PES；P1 只组 H.264 与 ADTS AAC。P1j：首次 PMT 前到达的媒体包有界暂存（最多 128 个 188 字节包，计入动态预算）并在轨道登记后按序回放；超过上界丢最旧包；残缺 ADTS/PES 不作为 AU。
- FLV：P2e 已按此实现经典 FLV（见下方专节）：32 位扩展时间戳、有符号 24 位合成时间、AVC/AAC sequence header 与完整 AU；脚本 tag 跳过且**不做** AMF/关键帧索引，故 seek 能力仍为不具备。Enhanced FLV/HEVC 诚实拒绝。不是 RTMP 客户端。
- MP4/MOV：P2a 共用箱体遍历已落地；P2b 对 **moov 先于 mdat** 的渐进文件解析 sample 表并按文件偏移输出完整 H.264/AAC 样本（载荷合同见下）。P2c：当 **moov 位于确定大小的 mdat 之后** 时，若声明的 mdat 载荷不超过 `max_lookback_bytes`（默认 8 MiB，绝对上限 16 MiB，仅可下调）且动态预算允许，则保留有界回看窗，表定稿后按与 P2b 相同合同输出 AU，然后释放该窗；超出上限或预算则跳过、不整文件缓存，需要回看时返回 `MS_ERR_NOT_SEEKABLE` 并投递 `UNSUPPORTED`，会话不因此失败，FORMAT/TRACK/CONFIG 仍可取出。`size=0` 的 mdat（直到 EOF）后面不可能再有 moov，不回看。H.264 AU 保持 length-prefixed，配置为完整 avcC；AAC AU 为裸帧，配置为 ASC。时间戳为媒体时间线的 DTS 与 DTS+ctts，**不把 elst 映射进 uint64 PTS**。`stz2`、加密 `encv`/`enca`、非 LC AAC 明确拒绝或该轨不可用。其余 sample entry 该轨不可用。任意时间 seek / Source size+range 仍未做。
- fMP4：P2a 可识别 `styp`/`moof`/`mvex` 并标为 fMP4；空初始化表不视为畸形。P2d 在 init（含 `mvex`/`trex` 与 `avc1`/`avcC`、`mp4a`/`esds`）出现于首个可用 moof 之前的前提下，按顺序解析 `tfhd`/`tfdt`/`trun`，输出与渐进路径相同合同的 H.264/AAC AU。子集与错误合同见下方「P2d」节。不是全部 fMP4/CMAF，也不是解密或清单调度。

加密单列，见第 13 节。能解析箱或 TS 包不等于能解密。

### P2a ISO BMFF 箱体基础（已按此实现，非正式验收）

识别：只检查字节 0 是否为合法顶层箱头（已知类型 + 合法 32/64 位 size），禁止在缓冲中扫描四字节标签。`size=1` 读 64 位 largesize；`size=0` 表示直到 EOF，且只允许出现在顶层；`size` 小于箱头为畸形。增量输入不足箱头时等待，不把半截头当完整箱。

遍历：白名单容器（`moov`/`trak`/`mdia`/`minf`/`stbl`/`moof`/`traf`/`mvex` 等）按子箱递归，深度上限 16、箱数上限 4096；未知箱（含 QuickTime `udta` 中的 © 类型）按声明大小跳过。`mdat`/`idat` 及大体箱用 skip 计数消耗输入，不把整文件缓存进会话缓冲。确认后输入窗约 256 字节，仅容纳箱头。

分类：`qt  ` 品牌或无 ftyp 的 moov/mdat → `MS_FORMAT_MOV`；见到 `moof`/`styp`/`mvex` → `MS_FORMAT_FMP4`；其余有 ftyp → `MS_FORMAT_MP4`。`heic`/`avif` 等静止图像品牌若不在本子集则 `UNSUPPORTED`。`FORMAT` 在 `moov` 走完（以免漏掉其中的 `mvex`）或已见到 fMP4 标志时发出一次。本步不发出 TRACK/AU，不可播放。

为后续留下可复用的类型判断：`ms_bmff_is_sample_table_type`、`ms_bmff_is_fragment_type`、`ms_bmff_is_container_type`。

### P2b 渐进 MP4/MOV H.264/AAC 样本输出（已按此实现，非正式验收）

范围：仅 moov 完整出现在第一份样本数据之前的渐进 MP4/MOV。配置事件先于该轨 AU。样本按文件偏移（各轨 chunk 交织）的解码存储顺序发出，不重排为显示顺序。单样本装配不超过 `max_assembly_bytes`，表与当前样本计入会话动态预算，不把整个 mdat 留在内存。`ctts` 能正确处理有符号偏移；若合成 PTS 为负则 `UNSUPPORTED`，不回绕伪造。表长度、乘法/累计溢出、描述符嵌套、跨表一致性、数据偏移均做有界检查。复用公共事件队列的严格 FIFO、epoch、flush/reset 与反压。

样本格式合同写在 `ms_event.sample_format`（结构末尾追加，旧字段偏移不变）：TS 路径为 `ANNEXB`/`ADTS`；本路径为 `AVCC`/`AAC_RAW`。不得凭 codec 名猜测载荷形态。

本步明确不做：moov 在尾的回看、任意 seek、fMP4 分片、FLV、软解、把 avcC 样本改写成 Annex B 再冒充 TS 合同。

### P2c moov 在尾的有界回看（已按此实现，非正式验收）

当第一份可用样本的文件偏移落在当前读取位置之前，且文件不是 fMP4 时：仅在 mdat 声明大小已知、不超过回看上限、且 `ms_mem_alloc` 成功时保留该载荷；分配失败当作溢出跳过，不把解析卡在 `LIMIT`，也不为超限 mdat 整文件缓存。表定稿后若所有已错过样本都完整落在回看窗内，则按绝对偏移收获，合同与 P2b 相同；只要有一个已过样本未被覆盖，则不输出部分 AU，释放回看窗，并明确报错。超过上限的 3 MiB 级 dummy mdat 仍须跳过且峰值远小于载荷。本步不是任意 seek，也不覆盖 `moof`/`trun` 或 FLV。

### P2d fMP4 init + moof/mdat 的 H.264/AAC AU（已按此实现，非正式验收）

可支持子集：初始化段（`ftyp`/`styp` + 含 `mvex`/`trex` 的 `moov`，`stsd` 为 `avc1`/`avcC` 和/或 `mp4a`/`esds`，空 sample 表允许）必须出现在首个可用 `moof` 之前；随后顺序消费 `moof`（`mfhd`，一个或多个带 `tfhd`/`tfdt`/`trun` 的 `traf`）与对应 `mdat`。`tfhd` 缺省继承 `trex`；`tfdt` v0/v1；`trun` 可带 duration/size/flags/composition offset 或使用缺省；composition offset v0 无符号、v1 有符号；`sample_is_non_sync`（`0x00010000`）与 first-sample-flags；`data_offset` 为有符号 int32。基址优先级：`base-data-offset-present`，否则 `default-base-is-moof`，否则第一 traf 隐式为 moof 起点、后续 traf 为上一轨数据尾；同一 traf 内后续无 offset 的 trun 从 `next_data_off` 续接。多 traf/trun；跨片沿用轨配置，`tfdt` 给出该 traf 首个 DTS，缺省则续 `next_frag_dts`。样本按文件偏移（解码/存储序）发出，PTS = DTS + cto，不把 B 帧显示序当解码序，不把 elst 映射进 uint64 PTS。每片定稿后释放当前分片样本表（上限 4096 条），不整文件缓存、不保留无界碎片索引。`sidx` 当叶子跳过。若 moov 同时带样本表与 `mvex`，本子集忽略 moov 样本表，只走 moof。若某片 mdat 出现在其 moof 之前且仍落在 P2c 回看窗内，可按绝对偏移收获；moof 已就绪时走在线收获，不再为该片开回看。

明确错误/等待，不吐假帧：`moof` 先于 `moov` 时不缓存媒体，moov 到达后记 `need_seek` 并投递 `UNSUPPORTED`，不输出 AU；未知 `track_ID` 或 sample description 不一致则跳过该 traf；`senc`/`saiz`/`saio`（含出现在 trun 之后）丢弃该 traf 已登记样本，会话投递 `UNSUPPORTED`（标志跨片保持到 reset）；`duration-is-empty` 跳过该 traf；样本 size 为 0 为 `MALFORMED`；样本数/算术溢出为 `UNSUPPORTED` 或 `OVERFLOW`（预算失败不把解析卡在 `LIMIT`）；中途 EOS 不输出残缺 AU；合成 PTS 为负则 `UNSUPPORTED`，不回绕。加密 `encv`/`enca` 的轨本来就不可用。增量碎片、反压、flush/reset/EOS 复用既有队列与预算。

本步明确不做：FLV（FLV 见 P2e）、任意时间 seek / Source size+range、解密、清单/CMAF 全集、软解、硬解出画、把 avcC 改写成 Annex B。四平台硬解未验证。

### P2e 经典 FLV H.264/AAC AU（已按此实现，非正式验收）

范围：文件头为 `"FLV"` + version 1，flags 仅音频/视频位（保留位在发出 FORMAT 后为 `UNSUPPORTED`），DataOffset 9..4096 并跳过垫片。`prevTagSize` 必须匹配（首个为 0），`StreamID` 必须为 0。只处理 tag 8（音频）、9（视频）、18（脚本）；脚本与未知类型按声明大小跳过，**不做 AMF 解析**、不建关键帧索引、不做 seek。SoundFormat=10 的 AAC：packet 0 为 ASC，packet 1 为 `AAC_RAW`。CodecID=7 的 AVC：packet 0 为完整 avcC，packet 1 为长度前缀 NALU（`AVCC`），packet 2 为 end-of-sequence，**不是 AU**。视频 `IsExHeader`（首字节 bit7）视为 Enhanced FLV，诚实拒绝，不得当成经典 AVC；音频不能用同一 bit 判断 Enhanced（AAC 格式高半字节已是 `0xA`）。配置解析复用 `ms_avcc_parse` / `ms_avcc_validate_sample` / `ms_asc_ok`，不平行重写。时间戳为 32 位扩展毫秒 DTS，合成时间为有符号 24 位，PTS=DTS+cto，`time_base` 为 1/1000；负 PTS 因公共 ABI 的 uint64 无法表达，返回 `UNSUPPORTED`，不回绕。配置事件先于该轨 AU；缺配置不吐 AU 并保持不支持；配置变化发新 CONFIG，下一 AU 带 `format_changed`。轨道 id：视频 1、音频 2。只输出完整合法 AU。不做 RTMP/网络客户端。

反压与寿命：完整媒体 tag 在 `ST_EMIT` 保留到入队成功，LIMIT 时不丢弃、不重解析同一 tag；`ms_session_pull` 在队列腾出空间后会 drain 并继续消化 `in_buf` 残留。`end_of_input` 若因反压返回 LIMIT，不得清空尚未消费的输入。脚本 tag 不以 `dataSize` 分配缓冲。中途 EOS 丢弃半截 tag，不把残片当 AU，也不因此判畸形。复用既有事件队列、epoch、flush/reset 与预算合同。

本步明确不做：Enhanced FLV / HEVC / 非 AVC·AAC、脚本关键帧索引、任意时间 seek、RTMP、把现行 copy-push ABI 改成裸指针「零拷贝」。不是可播放或硬解出画。四平台硬解未验证。

### 数据路径拷贝与有测量的缓冲所有权（已落地安全子集，非正式验收）

目标最终是：完整 AU 优先以缓冲区引用交付（retain/release），硬解视频尽量走平台表面/GPU 显示且不回读 CPU。但跨包组帧、必要的格式转换、以及平台强制复制都有边界，**不得虚称全链路零拷贝**。本轮 FLV 出帧**不推翻**已有 ownership/queue 合同，也不把 `memcpy` 改成易悬空的裸指针。

当前逐段复制现状：

1. 格式未确认或数据仍进入 `in_buf` 时，`ms_session_push` 把调用方字节拷入会话输入窗。
2. 已确认的 FLV 与 BMFF 走直接喂解析器，后续字节不再进 `in_buf`（已规避这一段冗余拷贝）。
3. 跨多次 push 组帧时，FLV 把完整媒体 tag 拷入解析器 body；TS 的 PES/ADTS/H.264 装配、BMFF 样本收获同样会聚合拷贝。FLV 脚本/未知 tag 只做跳过计数，不为声明的 `dataSize` 分配。
4. 完整 AU 若已在会话记账分配器的独占块中（FLV tag body 的载荷视图、BMFF 整样本 `sbuf`、TS H.264/ADTS 装配缓冲在交出前把剩余碎片拷走），`evq` 移交该块所有权：`n->data` 为可释放块首，`ev.data` 可以是块内视图，不再对载荷 `memcpy`。配置、无法满足移交条件的碎片，以及内部 `force_copy` 对照路径，仍拷入队列自有缓冲。这是现行 **copy-push** 入参合同的兼容扩展，不是调用方输入指针借用。
5. 事件指针在 `release` / `flush` / `reset` / `seek` / `destroy` 之前有效。未取走或已 pull 未 release 的事件**不能活过会话**；`cancel` 不释放已 pull 的事件，但随后 `pull` 返回 `CANCELED`，`destroy` 仍回收。同一会话同时只由一个线程进入。
6. 有界预算：移交不二次分配、不二次计费，块仍计在会话预算中直到 release/flush/reset/destroy。入队仍受 `au_queue_bytes` 与输出槽限制；LIMIT 时不置 `owned_taken`，所有权留在解析器以便重试。不得丢记账。
7. 若将来做 Annex B ↔ AVCC 等格式转换，或某些平台硬解输入要求线性拷贝，还会再复制一次。

公共只读统计：`ms_session_get_stats`（调用方先填 `struct_size`，只复制已知前缀；`MSMEDIA_ABI_VERSION` 仍为 1）。字段含 `copy_in` / `copy_asm` / `copy_out` / `move_out` 次数与字节，以及 `baseline_out_bytes = copy_out_bytes + move_out_bytes`（即若该次出队都走 memcpy 的对照值）。启动阶段快照为首次 `FORMAT` / `TRACK` / `CODEC_CONFIG` / 可解码关键 AU（`is_keyframe`）/ 首个视频关键 AU 时的累计 `bytes_in`（各次 push 的 consumed 之和）。整文件一次推入时，后段阶段可能落在同一窗口，不能当成「读到第 N 字节才就绪」；分块推入更细。这是解析就绪，**不是**屏幕首画面，也不含网络 RTT 或硬解 submit。`reset` 清零统计；`flush` 保留累计（同一会话继续）。墙钟若测，只放在测试/inspect 外包层，本核心不提供，且有噪声，不承诺固定毫秒。

本步明确不做、记入后续：公开的跨会话 `retain` / 借用调用方输入 / `push_owned`、共享引用计数的碎片零拷贝、硬解表面路径。不得用裸指针冒充零拷贝，也不得宣称全平台全链路零拷贝。

硬解显示路径与统一 C 调用接口（**均未实现**，按平台实际能力单独写，不得提前宣称已实现或已出画）：

- 最终产品要有一套统一的 C 硬解调用接口，后端按平台外挂，参考各平台成熟官方 API。查询骨架在无后端时必须返回「非硬解 / 未核实」。不软解回退。
- **压缩输入拷贝** 与 **解码后 Surface/GPU 零 CPU 回读** 是两件不同的事，禁止混为一谈，也不保证所有芯片无条件硬解。
- Android API 21+：`AMediaCodec`（NdkMediaCodec）。输出可配置到 Surface，避免把帧读回 CPU；输入侧通常仍要写入 codec 的 input buffer，**不得宣称输入恒为零拷贝**。
- Android API 16–20：经 JNI 调用 Java `MediaCodec`。禁止 Java reflection；创建时查找并缓存 `jclass` / `jmethodID` 为 global ref。`ByteBuffer` / DirectBuffer 在 `queueInput` / `queueOutput` / `flush` / `stop` 之后不得把旧指针当作仍有效而永久复用，必须按官方缓冲区生命周期重新取得。
- Windows：Media Foundation / D3D11 纹理。
- iOS：VideoToolbox / 平台 surface（或 IOSurface / 压缩样本直接显示路径）。
- Linux：VAAPI / V4L2（可叠加 DMABUF）。
- 四平台硬解查询与出画均未实现、未验证。本步只把上述计划写入设计与路线图。


### 启动路径与快速加载（设计已记；阶段观测已随所有权小步落地）

用户目标是尽快进入可播放加载，但**不承诺固定毫秒**。第三方公开接口与许可证表仍仅作思路参考。阶段字节与复制计数已由 `ms_session_get_stats` 与 `ms_ts_inspect` 输出；未增加空的 `set_latency` 接口。

#### 来源与许可证

只参考官方公开文档与公开头文件/API 参考中的**调用方式与生命周期**，不把其源码拷进 git 仓库。2026-11-20 21:21 CST 以前「不把 FFmpeg 链入产品」适用于全部产物；此后用户明确决定允许可选 `libmsff` 链入钉选的官方 FFmpeg 9.1 裁剪库，但仍不把 FFmpeg 链入 `libmsmedia` 核心，也不链 VLC、mpv 或 Media3。本库许可证尚未选定，尤其禁止复制 GPL 代码。

| 项目 | 许可证（以项目官方说明为准） | 公开接口与文档 | 可借鉴的思路 |
| --- | --- | --- | --- |
| AndroidX Media3（含 ExoPlayer） | Apache License 2.0（[androidx/media LICENSE](https://github.com/androidx/media/blob/release/LICENSE)，文档站 [developer.android.com/media/media3](https://developer.android.com/media/media3)） | [Extractor](https://developer.android.com/reference/androidx/media3/extractor/Extractor)（`sniff` / `read` / `seek` / `release`）、[TrackOutput](https://developer.android.com/reference/androidx/media3/extractor/TrackOutput)（`format` 先于 `sampleData`/`sampleMetadata`）、[SampleQueue](https://developer.android.com/reference/androidx/media3/exoplayer/source/SampleQueue)、[ProgressiveMediaSource](https://github.com/androidx/media/blob/release/libraries/exoplayer/src/main/java/androidx/media3/exoplayer/source/ProgressiveMediaSource.java)、[DefaultLoadControl](https://developer.android.com/reference/androidx/media3/exoplayer/DefaultLoadControl)、[DefaultLivePlaybackSpeedControl](https://developer.android.com/reference/androidx/media3/exoplayer/DefaultLivePlaybackSpeedControl) | 增量 sniff；`read` 每次只推进一小段；样本队列持有寿命；开播由 LoadControl 根据已缓冲时长/字节决定；直播用目标 live offset 调速；随机读只落到 SeekMap 给出的合法点 |
| FFmpeg libavformat / libavcodec | 主体为 LGPL-2.1-or-later，部分可选组件为 GPL（[FFmpeg README / License](https://ffmpeg.org/doxygen/trunk/md_README.html)） | [Demuxing](https://ffmpeg.org/doxygen/trunk/group__lavf__decoding.html)：`av_probe_input_buffer` / `av_probe_input_buffer2`、`av_read_frame`（成功的 `AVPacket` 引用计数，用 `av_packet_unref` 释放）；[send/receive API](https://ffmpeg.org/doxygen/trunk/group__lavc__encdec.html)：`avcodec_send_packet` / `avcodec_receive_frame`；格式选项 `probesize` / `analyzeduration` / `max_probe_packets`（[FFmpeg Formats](https://www.ffmpeg.org/ffmpeg-formats.html)） | 探测有上界，分数不足则扩大再试；包是引用而非裸指针；解码 send/receive 解耦，`EAGAIN` 表示要换方向；打开前配置探测上限；推荐引用计数以免解码器再拷一份 |
| libVLC / libVLCcore | 引擎为 LGPLv2.1-or-later；VLC 播放器应用仍为 GPLv2-or-later（[VideoLAN 声明](https://www.videolan.org/press/lgpl-libvlc.html)、[LibVLC Wiki](https://wiki.videolan.org/LibVLC)） | 公共 C API：`libvlc_media` / `libvlc_media_player`；`libvlc_media_add_option` 可向该媒体附加读取相关选项。维护者曾说明 `network-caching` 一类内部配置**不是**跨 libVLC 版本的便携保证 | 引擎与应用分开；输入缓存是策略而不是 demux ABI；不要把内部 CLI 选项当成稳定公共接口搬进本库 |
| mpv | 默认 GPLv2-or-later，`-Dgpl=false` 时为 LGPLv2.1-or-later（[mpv README](https://github.com/mpv-player/mpv)） | 官方手册 [mpv.io/manual](https://mpv.io/manual/stable/)：`--demuxer-lavf-probesize` / `--demuxer-lavf-analyzeduration` / `--demuxer-lavf-probescore`、demuxer cache / `--demuxer-max-bytes`、`--profile=low-latency`、`--vd-lavc-dr`、`--video-latency-hacks` | 探测大小与分数可调但有下限；低延迟是关闭「为稳态质量而多缓冲」的一组选项，会牺牲稳健性；手册不承诺固定时延；direct rendering 是解码到 GPU，与压缩包零拷贝不是同一件事 |

参考数值（例如 Media3 `DefaultLoadControl` 的默认开播缓冲约 1000 ms、网络稳态最小/最大缓冲 50000 ms，或 FFmpeg 默认探测字节/分析时长）**只说明他人如何分开「尽快开播」和「稳态可靠性」**，不是本库的保证，也不写入本库 ABI 默认值，除非后续小步有本项目实测支撑。

#### 对 MsMedia 的设计含义（要落地的原则，不是已实现的新接口）

- **结构与生命周期**：本库已有会话 `push`/`pull`、epoch、`flush`/`reset`/`release`，对应「源字节 → 容器提取 → 事件队列 → 调用方」分层。继续保持单会话合同，不为此新增空的播放器对象。控制层（取流、硬解、渲染、时钟）仍在核心之外。
- **packet/sample 引用**：已落地的是会话内独占块移交到事件队列（唯一所有权，不是共享引用计数）。`ms_session_release` / flush / reset / destroy 释放该块。公开的 `retain` / 借用调用方输入 / `push_owned` 仍未做。不得用指向调用方临时缓冲的裸指针冒充引用。事件不能活过会话。
- **增量、有界探测**：已有 FLV 在短前缀确认、BMFF 合法箱头、TS 同步、确认后输入窗约 256 字节、不足则等待。应继续「尽早确认、到达上限停止或报 `UNKNOWN`」，禁止无限吸数据。也**禁止为了更快而盲目缩小探测窗**，以免 TS 确认不足、BMFF 半截箱头、或 FLV 头垫片未读完就误判。
- **参数集与安全随机接入**：已有合同是配置先于 AU（P1i 中途接入等待 SPS/PPS/IDR；FLV/BMFF 缺配置不吐 AU）。「可解码」不是「看到任意一片 NAL」。不能为缩短启动路径输出不可解码残片，也不能忽略关键帧接入。
- **decoder submit**：公开的解码提交模型是配置就绪之后再提交压缩 AU（FFmpeg send/receive；平台硬解的 input buffer / Surface 路径）。**硬解尚未实现**，本构建 `ms_hw_query` 固定为非硬解/未核实。因此不得编造「硬解 submit」或「首画面」时间。将来这些指标只在平台桥定义，并与「会话已 pull 出可解码关键 AU」分开统计。
- **直播低延迟 / 点播可靠性**：成熟播放器把「开播所需最小缓冲」和「稳态目标缓冲」分成可配置策略（Media3 LoadControl / LivePlaybackSpeedControl；mpv 的 low-latency profile 与默认 cache）。本库日后若做策略开关，必须落到已有反压、`au_queue`、会话预算和开播条件上（至少已有可解码关键 AU；额外预滚由控制层按场景决定）。Source/网络未落地前，**本步不增加空的 `set_latency` 一类接口**。

#### 可测启动路径（复用现有事件、consumed 与预算统计）

从会话第一次 `ms_session_push` 收到首字节起，按事件阶段计，而不是按屏幕像素：

1. 首字节进入：该次 `push` 的 `consumed > 0`
2. 容器确认：`MS_EVENT_FORMAT`
3. 轨道可见：至少一条 `MS_EVENT_TRACK`
4. 配置就绪：目标轨 `MS_EVENT_CODEC_CONFIG`（avcC / ASC / SPS·PPS 等）
5. 可解码关键 AU：该轨 `MS_EVENT_AU`；视频须 `is_keyframe`，纯音频为配置之后的首个完整帧

每个阶段记录：累计输入字节（各次 `push` 的 `consumed` 之和）、该阶段是否到达、可选的宿主墙钟（由测试程序在 `push`/`pull` 外包一层计时）。内存峰值继续用已有 `ms_session_debug_budget`（非正式调试入口）。这与第 14 节「得到 codec config / 第一个完整关键帧 AU 时已消耗的输入」一致，并补上 FORMAT/TRACK 中间点。

明确不测、不虚造：屏幕首画面时间（须等平台渲染）；硬解 submit / 出画；网络首包 RTT（须等 Source）。用户要快，但报告只写实测值与测量条件，**不承诺「必须小于 N 毫秒」**。

测量纪律：必须写明样本与生成方式、机器与 OS、debug 或 release、是否启用 sanitizer、chunk 大小、进程/页缓存冷热。只在同一组条件下比较。`ms_ts_inspect` 已打印复制计数与启动阶段；墙钟未写入核心，若在外包层测量须声明噪声。实测数字见 PROGRESS，不得外推为全平台全链路零拷贝。


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
| AAC | TS `0x0F` ADTS；MP4 MP4A | ADTS 或 AudioSpecificConfig | 长度字段齐全的一帧 | P1 只做 ADTS 子集（layer=0、rdb=0、sfi 0-12、ch 1-7；MPEG-2/4 ADTS）；LATM/裸流/PCE/多块不做 | P1b 子集组帧之上，P1j：PMT 前完整 ADTS 回放发出；EOS 残缺帧不作为 AU；不是全部 AAC；LATM 仍不做；非正式验收 |
| H.263 | FLV codec id 等，仅标识 | 图像起始码 | 完整图像 | P4 | 未实现 |
| HEVC | TS `0x24`，hvc1/hev1 | VPS/SPS/PPS（须成套匹配） | IRAP 为关键帧的 AU | P4 | 宿主已落地（libmsff MP4/TS + 公共 `ms_hevc_*` + 硬解配置/提交/mock）；自写 TS 仍为容器标识；设备未验；无 FFmpeg 软解 |
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

### 已定核心与待评估纳入

用户补充了更广的对照清单（更多编码、封装、协议、广播、HDR）。**只在本设计与路线图中分列，本轮不扩展这些新项的实现，也不一次性宣称全支持。** 「已定核心」是既有目标承诺，不等于当前已实现。

**已定核心**（仍以本节与第 4、7、8、10、11 节及 `docs/COMPATIBILITY.md` 既有表为准）：

- 容器：MPEG-TS（188/192）、FLV、ISO BMFF 的 MP4 / fMP4 / MOV 子集
- 清单：HLS（TS 与 fMP4 分片）、DASH（默认 fMP4）；WebM 不在默认范围
- 协议插件（第 7–8 节）：HTTP(S) 直连、RTMP/RTMPS、RTSP/RTSPS、MPEG-TS over RTP、UDP/RTP 组播与单播等
- 编码（证据级、逐个合并）：H.264 与 AAC 已有子集；其余见上表（H.263、HEVC、MPEG-4 Part 2、MPEG-2 Video、AVS+、AV1、AC-3、E-AC-3、MP3、MP2、Opus、G.711）。表中未实现项不得写成已支持
- 能力：有界内存与稳定 C ABI、Source 的 read/seek/size/cancel/range（seek 本身尚未落地）、硬解插件（禁止产品软解）、多音轨/字幕枚举与选择

**待评估纳入**（补充清单中超出已定核心、尚未决定是否进入默认范围的类别；评估前不排期、不实现、不宣传）：

- 更多编码：未写入上表的编码族（例如更新一代视频/音频对象），不得因为出现在对照图里就当作核心目标
- 更多封装：默认范围以外的容器（例如 WebM/MKV 及其他未列封装）
- 更多协议：未写入第 7–8 节的传输/推流变体
- 广播：节目列表、EPG、回看模板等已明确不进核心；其它广播表、信令或插播标志需单独评估
- HDR 其余项：Dolby Vision 编码配置与 HDR10+ 动态元数据透传仍不纳入（识别到 DV 则报告不支持并回落基础层；HDR10+ 若检出亦不向 MediaCodec 传递，未验证）。显示器 HDR 效果、真机硬解出画不是本项验收。

本轮已授权并实现（Android 主线，识别与透传，不是显示效果验收）：HEVC VUI/SEI 与 ff codecpar/side data 的 HDR10（PQ=16）/ HLG（18）识别；Android API24+ MediaFormat 的 color-standard/transfer/range 与 25 字节 hdr-static-info；加法 `ms_hdr_*` / `ms_ff_get_hdr_info` / `ms_hw_player_{get,set}_hdr_info`，不改 `ms_event` 与 ABI 版本。AV1/VP9 仅当 ff 已给出 color 时透传，码流解析未验证。


## 6. 硬解与禁止软解

平台查询结果字段：`is_hardware`、`verified`、`backend`、说明字符串。`is_hardware = 1` 必须同时满足：选用的解码器被强制为硬件，且回落软件的路径已关闭或已被核实不会发生。只调用 configure / decode 而不能证明这一点时，`is_hardware` 必须为 0，`verified` 为 0。

| 平台 | 目标后端 | 核实要求 |
| --- | --- | --- |
| Android | MediaCodec + Surface | 以硬件组件为准；`c2.android` / `OMX.google` 一类软件组件不能报硬解。API 能提供硬件标志时必须采用 |
| iOS | VideoToolbox | 使用「要求硬件解码器」的属性。只「允许」硬件、失败后仍可软解的属性不够 |
| Windows | 硬件 Media Foundation + D3D11 | 只选硬件 MFT。只有软件 MFT 时返回不支持 |
| Linux | VAAPI、V4L2 或厂商硬解设备 | 打开到硬件设备并查到 profile 才算。没有设备则不支持 |

骨架和桌面宿主上的空实现固定返回非硬解。2026-11-22 05:17 CST 起，Linux 宿主默认查询与 `mock_mode=0` 的配置改为走可选 VAAPI 探测与码流 profile 映射（`docs/HW_LINUX.md`）：无 DRM 设备或尚未实现 slice 提交时 `is_hardware=0`、`verified=1`，不得只按 MIME 声称支持。这仍不是设备出画。Windows / iOS 适配仍未实现。

音频：系统解码器经常是软件。查询结果使用单独种类：`HARDWARE`、`SOFTWARE`、`SYSTEM_UNVERIFIED`、`NONE`。只有 `HARDWARE` 可以叫做硬解。默认策略是拒绝 `SOFTWARE` 和 `SYSTEM_UNVERIFIED`，不为了出声自动改走软解。若将来要系统音频解码，必须是另一个默认不链接的入口，名字里写明软件或未核实。

Media3 只可能出现在 Android 宿主，不进入核心。2026-11-21 12:36 CST 起，可选 `libmshw`（`include/msmedia/ms_hw.h`，`MSMEDIA_HW_1.0`）提供统一 C 会话；核心 `ms_hw_query` 仍为非硬解桩，`MSMEDIA_ABI_VERSION` 仍为 1。Android 选择策略与 Surface I/O 见 `docs/HW_ANDROID.md`。宿主 mock 与 NDK r29 API 21 v7a 交叉编译已完成；**真实设备/模拟器硬解未跑**，不得把编译成功写成硬解验收。

倍速的时间拉伸是可选音频后处理，输入是已经解码的 PCM，不是解码器，见第 11 节。它不能成为引入 AAC/MP3 软解的理由。

## 7. 网络与四平台分工

核心不链接网络，也不链接 TLS。取流使用同一组接口：`read`、`seek`、`size`、`cancel`、`range`、超时。backend 分两类，用编译开关裁剪。一次默认发布只带其中一套，不把系统栈和自有栈强制打进同一个包。

| backend | 职责 | 现在 |
| --- | --- | --- |
| 系统 | Android 平台 HTTP、iOS URLSession、Windows WinHTTP、Linux 宿主网络。TLS 用各系统自带栈 | 未实现 |
| 自有 | HTTP/1.1，以及 RTMP(S)、RTSP(S)。第一版只接 OpenSSL | HTTP/1.1 GET + TLS 子集已有宿主自测（可选 `libmsnet`，非正式验收）；RTMP/RTSP 未实现 |

解析核心不链接 OpenSSL。自有网络第一版统一用 OpenSSL，不并行再写 mbedTLS、wolfSSL 或第二套 TLS 后端。可以有一层薄封装供 HTTP、RTMP、RTSP 共用，但不做成可插拔的多后端框架。

用户比较过 OpenSSL 与 mbedTLS，认为 OpenSSL 更稳定，并需要复用 OpenSSL 的算法。这是用户自己的经验，不是本项目做出的普遍对比测试。wolfSSL 未测，不纳入第一版。

OpenSSL 只提供 TLS 和算法，不提供 HTTP。HTTPS、RTMPS、RTSPS 共用同一套 TLS 封装和连接 I/O，证书、主机名、SNI 只写一处。摘要、HMAC、加解密在协议需要时调用 OpenSSL 仍在维护的公开高层 API，不自造算法，也不把 OpenSSL 源码抄进仓库。协议若遗留旧摘要，只在该协议路径上调用对应 API，并与 TLS 的协议版本和套件策略分开。不为了旧协议打开 legacy provider，也不因此放宽 TLS。

依赖关系：

- `core`：无套接字、无 HTTP、无 OpenSSL
- `net`：可选。HTTP/1.1、连接、取消、超时，以及一份 OpenSSL 封装
- RTMP、RTSP：需要 TLS 或摘要时调用这份封装，不再各链一份 OpenSSL

静态或动态按部署选一种。体积和裁剪效果以实测为准，不预写数字，也不指定未经核对的「当前最新版」。选型原则是仍受安全维护的 OpenSSL 版本；具体版本号在引入时核对后写入 `docs/THIRD_PARTY.md`。

系统网络 backend 仍然保留，作为不链 OpenSSL 的替代构建，本轮未实现。默认核心不链网络；可选 `libmsnet` 需显式链接，可用 `ENABLE_NET=0` 整项关闭。不把系统栈和自有 OpenSSL 栈强制打进同一个核心包。

HTTP 初期只做 HTTP/1.1。HTTP/2 和 HTTP/3 在没有完整实现与测试前不列入支持。

自有 HTTP/1.1 GET 与 TLS 的最小子集已在可选 `libmsnet` 落地（2026-11-20 20:00 CST 宿主自测，非正式验收），必须继续满足、且本步已按此实现的部分：

- 响应体按 Content-Length、chunked 或连接 EOF 分界，均受缓冲区上限约束；同时出现 Content-Length 与 chunked 则失败。不实现内容压缩：请求 `Accept-Encoding: identity`，非 identity 的 `Content-Encoding` 明确 `UNSUPPORTED`，不静默inflate
- 重定向最多 5 次，并做访问列表循环检测；拒绝 URL userinfo；https→http 降级拒绝。本步不发送 Authorization/Cookie，因此也没有跨主机转发凭证
- 本步使用 `Connection: close`，不做连接复用
- 设置 `byte_off`/`byte_len` 时发送 Range；服务器返回 206 为部分内容；若忽略 Range 而返回 200，从流开头读，由调用方处理
- 连接/读写超时与 `cancel`；可选 worker 线程泵 DNS/连接/握手/IO，避免在调用线程上做不可取消的无限同步读。未启动 worker 时，`read`/`drive` 只做有界 poll 切片（约 20ms），仍受超时与取消约束。`getaddrinfo` 可能阻塞，应放在 worker 上。本步无代理配置
- 证书链与主机名严格校验（`SSL_VERIFY_PEER`、`SSL_set1_host`、SNI）；默认信任库来自系统 OpenSSL `SSL_CTX_set_default_verify_paths`，测试可附加 PEM CA 文件，**没有**关闭校验的公共开关。最低 TLS 1.2，关闭 SSLv2/3 与 TLS 1.0/1.1。不为体积关闭校验，不默认 TLS 降级，不自造加密
- 401/403 及其它非 200/206 为错误，不重试凭据、不绕过访问控制。网络层默认重试次数为 0（HLS 调度另有自己的重试预算）
- 日志不得包含完整 URL、query 或 userinfo

尚未实现、不得写成已支持：HTTP/2 与 HTTP/3、POST 等其它方法、代理、连接复用、内容压缩、Cookie/鉴权头、系统平台 HTTP backend、RTMP/RTSP。OpenSSL **源码不进仓库**；本机以系统库链接，版本与许可证见 `docs/THIRD_PARTY.md`。MsMedia 自己的许可证也还没选定。

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
| 内嵌文本 | 输出带时间戳的文本 cue。外挂 SRT/WebVTT（自写有界解析）；内嵌 tx3g/mov_text 提取 UTF-8/UTF-16 纯文本；ASS/SSA 进入同一 `ms_sub` 时间轴（见下）。可选隔离 `libmsass` 可按媒体时间栅格完整特效到 RGBA 叠加（不合成进视频帧、不读回 Surface）。`MS_EVENT_SUBTITLE` 的 `pts`=开始、`dts`=结束（同一 time_base）。 | 把字幕烧进视频帧；设备视觉验收；承诺 100% 全部 ASS |
| 图形字幕 | 可枚举（如 DVB/PGS/teletext 映射为 `MS_CODEC_SUB_BITMAP`）。选择播放返回 `UNSUPPORTED`。 | 把图形合成到画面；不得称为已支持渲染 |
| 外挂 | 调用方读入字节后经 `ms_*_set_external_subtitle` 解析（含 ASS/SSA）；虚拟 id `MS_TRACK_ID_EXTERNAL_SUB`，默认不自动选中。时间映射到 90 kHz 媒体轴。 | 下载网络外挂（不在核心里发起 HTTP） |

ASS/SSA（`MS_CODEC_SUB_ASS`）仍保留轻量去标签/布局模型（`cap_flags` 诚实标识未支持特效）。完整栅格是可选隔离模块 `libmsass`（钉选 libass 等，不链入 `libmsmedia`/`libmsff`，不改变顶层许可证，产品 FFmpeg 仍 `CONFIG_LIBASS=0`、`CONFIG_DECODERS=0`）。喂原始 header/events，不经截断去标签；附件字体默认关闭；只用已授权系统字体。不得把轻量路径称为完整渲染，也不得把宿主像素证据写成设备视觉验收，更不承诺 100% 全部 ASS。matroska 与 webvtt demuxer 已按需打开（不是 ass/srt demuxer，也不是 decoder）。forced 和 default 来自容器或清单的标记，供宿主选择；核心不因为 forced 就自动盖住用户的「关闭」。HLS 备选 AUDIO、字幕（独立 WebVTT 外链）与视频变体手动选择已由可选 `libmsff` 在宿主落地（设备未验）；自写 `ms_hls` master 与自动 ABR 仍为欠账。同容器内切换不重开 Source。

动态变化走 `TRACK_UPDATE`。seek 和换台清空尚未显示的 cue。

## 11. 播放场景、seek 与倍速

Source 能力：`read`、`seek`、`size`、`cancel`、`range`，以及本地 / 点播 / 直播、可否 seek、DVR 窗口起止。核心负责索引、时间轴和关键帧位置。速率、主时钟和送显调度在播放控制层。

| 场景 | 定位 |
| --- | --- |
| 本地文件 | 有 size 时可以做尾部 moov 和关键帧索引 |
| 网络点播 | 优先 range；没有 range 则 seek 能力为不具备 |
| 直播 | 源不提供时移则 `NOT_SEEKABLE` |
| HLS EVENT/滑动窗口时移 | 只在源公布的窗口内定位；live_edge 留安全距离。DASH 直播因 lavf 拒绝定位而保持 `NOT_SEEKABLE` |

快进和快退是相对当前时间的 seek，不是倒放。本设计不实现倒放。

seek 成功之后的固定顺序：`timeline_epoch` 增加、flush、从目标前的关键帧预滚、音频裁到目标 PTS、字幕清空、进行中的 AI 结果作废。

倍速（2026-11-21 20:25 CST 已在宿主落地控制层，非正式设备验收）：

- 控制层按速率解释时间戳，视频 AU 仍来自硬解
- 核心和默认构建不包含音频 time-stretch 外挂
- time-stretch 若以后存在，是可选外挂，输入为 PCM，用于保音调，不解码压缩音频
- 听感必须走平台音频能力，并在 `ms_hw_rate_caps` 中诚实报告：
  - Android API 23+：`AudioTrack.setPlaybackParams`，可保音调；本实现宣称尝试范围 1/4–2×
  - Android API 21–22：仅 `setPlaybackRate`（变调），典型上限 2×；不得称为现代保音调倍速
  - 宿主 mock：只做状态/时钟记账，`can_sync_av=0` 且 `mock_is_not_device=1`
  - 超范围、旧 API 无法套用、或尚无音频会话（非 mock）时返回 `UNSUPPORTED`，不得只改视频时钟却称为音画同步倍速
- 负速率（倒放）不支持。暂停用 `pause`/`resume`，不用速率 0
- 改速率、pause/resume、seek/flush、EOS、切音轨不得破坏时钟连续性（改速不清零；seek 保留速率并在新音频会话上重套用）

## 12. AI 字幕（可选插件框架；真实供应商未指定）

插件不链进默认核心（可选 `libmsai`，播放器 `dlopen`），核心不包含模型、云 SDK 和供应商地址。当前不选择付费供应商，不调用外部 API，不下载模型，不从密钥库取凭据。默认禁用。不得把受控假供应商或 mock 写成真实识别完成。

输入是平台硬解（或已诚实分类的音频解码）之后、输出修剪完成的 PCM tap，附带采样率、声道和第一可听样点的媒体时间戳。若插件需要单声道或重采样，使用本模块可验证的公共辅助（`ms_ai_pcm_s16_downmix_mono` / `ms_ai_pcm_s16_resample_linear`），不是第二个解码器。压缩基本流不直接送去识别。硬解路径拿不到 PCM 时，能力查询返回不可用，核心禁止为此加软解。Tap 不得阻塞解码、AudioTrack 或 UI。禁用或未配置时不采集、不传输、不建额外工作线程。

识别异步进行。有界队列、实时丢弃最旧块、任务取消与生命周期由引擎管理。增量（partial）、最终（final）与翻译更新都带 session/timeline/selection epoch，归并到现有 `ms_sub` 生成轨（`MS_TRACK_ID_AI_SUB`）。该轨默认不选中，语言/开关不得覆盖原字幕选择。seek、flush、切音轨后取消在途工作，丢掉 epoch 不匹配的迟到回调。源音频与文本不得写入日志。

本地文件可以在用户同意后预生成并缓存文本（本切片未做预生成缓存）。直播只滚动生成。默认不上传音频。云服务必须由用户另行明确选择并授权；密钥和费用放在插件配置里，不进入核心。翻译归并路径已实现（同一生成轨上的翻译 cue）；仍不接具体翻译供应商。内置仅「ms-ai-fake-local」受控模拟供应商，供闭环测试。

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
- 首屏：在进程内对合成样本度量两个字节数：得到 codec config 时已消耗的输入，以及第一个完整关键帧 AU 时已消耗的输入。这不是屏幕上出现像素的时间。像素时间要等平台渲染存在后再单独定义。更完整的启动阶段（首字节→FORMAT→TRACK→CONFIG→可解码关键 AU）、墙钟测量条件、以及不得虚造硬解 submit/首画面的边界，见第 4 节「启动路径与快速加载」。不承诺固定毫秒。

宿主测量结果标注为宿主。四平台正式体积以各平台工具链为准。2026-11-21 12:19 CST 的宿主 `release` 与 `release-size` 对照，以及非正式 armeabi-v7a 同 ABI 对照（条件、动态依赖、不可跨 ABI / 不可与 mbedTLS so 相比）写在 `docs/FFMPEG_BACKEND.md` 第 11 节；不是正式 NDK 验收，也未测运行内存或性能。

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
| AAC ADTS 完整 AU 与 AudioSpecificConfig | P1b | 子集已实现；P1j 补 PMT 前完整帧回放与残缺尾帧不发出；经宿主自测通过；非 el7/四平台/可播放验收 |
| 硬解查询骨架 | P1a | 本构建固定返回非硬解/未核实 |
| 体积/内存/首个 AU 测量 | P1 / P2a / P2b | P1f 已测固定对象与双轨峰值；P1g–P1j、P2a 箱体与 P2b 渐进 AU 另有测量（见 PROGRESS）。不是四平台或硬解出画测量；P1 未通过 |
| FLV、MP4、fMP4、MOV 子集、文件/点播/直播 Source、seek。速率只保存数值，不处理听感 | P2 | P2a–P2d 箱体/渐进/回看/fMP4 与 **P2e 经典 FLV** 的 H264/AAC AU 已落地（非正式验收）；完整 AU 入队所有权移交与 `ms_session_get_stats` 已落地。任意时间 seek / Source 未做。不代替正式 P1 通过，不宣称 P2 完成 |
| 启动路径阶段统计、直播低延迟/点播可靠性策略 | 后续小步 | 阶段观测已随所有权小步落地；直播/点播缓冲策略与空的 `set_latency` 仍未做 |
| HLS、DASH，然后 RTMP/RTSP/RTP/UDP | P3 | 自写 HLS/DASH/`libmsnet` 子集截至 20:34 CST 已落地并保留作回归。2026-11-20 21:21 CST 起，取流/协议/解封装改走可选官方 FFmpeg 9.1 裁剪后端：本地 TS/MP4/FLV 与受控 HTTP+HLS/HTTPS 已验证；RTSP/RTMP/UDP/RTP 与 DASH-over-HTTP 仅 configure 开启、尚未联调。不是 ABR/DRM，不是六类协议完成 |
| 其余 codec 组帧、多音轨、字幕、倍速听感 | P4 | HEVC、libmsff HLS 备选 AUDIO 与字幕（独立 WebVTT 外链）、H.263/MPEG-4/MPEG-2/AVS+/AV1 与 AC-3/MP3/MP2/Opus、位图与 matroska 宿主已落地（设备未验）；完整 ASS 栅格为可选 `libmsass`（宿主已测像素证据，设备未验）；HLS 视频变体仅为手动选择，不是自动 ABR |
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
11. 自有网络的 TLS 已定为 OpenSSL，不并行第二套 TLS 库。可选 `libmsnet` 已按此实现 HTTP/1.1 + TLS 子集，链的是系统 OpenSSL，不把源码引入仓库。仍待确认的是默认发行包是否附带该可选库，还是只提供不链 OpenSSL 的系统栈；系统 HTTP backend 尚未实现。确认前仍不向核心打入两套网络。

作者署名已按要求写入：by: Long <mycjzb@gmail.com>。


## 19. 可选 FFmpeg 9.1 后端（2026-11-20 21:21 CST）

用户明确改变架构后的迁移设计、官方版本实证、裁剪 flags、TLS/许可证与已验证边界见 `docs/FFMPEG_BACKEND.md`。核心 `ms_session` API 保持兼容；`ms_format` 末尾追加 HLS/DASH。自写协议代码不删除、不再扩展。

2026-11-21 11:12 CST 已修正 `libmsff` 时间戳（统一 origin，不再把合法负 PTS 标 has=0）与 `MS_ERR_TIMEOUT`，并用独立 trun 表核实 DASH 尾包。2026-11-21 11:26 CST 已把官方钉选 OpenSSL 3.5.9 LTS 按需静态裁剪合入 `libmsff`（符号隐藏，不再 NEEDED 系统 libssl/libcrypto）；`libmsmedia` 仍不链 SSL，旧 `libmsnet` 仍动态链系统 OpenSSL 作对照。2026-11-21 11:58 CST 已在宿主完成 RTSP（TCP interleaved/UDP）与 RTP 帧合同（独立 AU 参考，含 FU-A/STAP-A/重排序/失包等关键帧）以及 RTMP 拉流（FLV 配置/媒体包）受控联调；RTMPS/RTMPT、AAC-over-RTSP、摄像机全集未测。已批准但尚未实施：Android/Media3 硬解与内存/复制计量。不要写成 ABR、DRM、硬解出画或四平台已完成。能力矩阵见 `docs/FFMPEG_BACKEND.md`。
