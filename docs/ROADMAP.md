# 任务路线图

by: Long <mycjzb@gmail.com>

状态日期：2026-11-21（北京时间，11:58 CST 完成 RTSP/RTP 帧合同与 RTMP 本机拉流联调；11:26 CST 完成 OpenSSL 3.5.9 LTS 静态裁剪合入 libmsff；11:12 CST 完成 libmsff 时间戳/TIMEOUT/DASH trun 正确性；22:07 CST DASH-over-HTTP / UDP-RTP 子集；21:21 CST 版本钉选与 HLS/HTTPS；20:34 CST 自写 DASH HTTP 记载仍有效）。P0 文档已落盘但许可证等项仍待确认。P1a–P1j 已有实现；P2a–P2e 及 Source/渐进定位、所有权小步已有宿主自测；P3 已有 HLS media playlist 调度、可选 `libmsnet` HTTP/HTTPS 取流首步、DASH MPD 调度最小子集，以及该调度经同一 fetch 绑定的本机受控 HTTP 闭环（非正式验收）。P1 正式 el7/四平台/硬解闭环仍未完成，**不将 P1 写为通过，不将完整 P2/P3 写成完成。** 平台无关解析小步可与正式验收分列。宿主自测不等于正式验收。TLS 库验证不等于四平台实机。四平台设备硬解出画仍未实现、未验证。Linux VAAPI 映射/探测已有宿主测试（见 `docs/HW_LINUX.md`），不是出画验收。用户补充的更广对照清单只列为「已定核心 / 待评估纳入」，见设计第 5 节，本轮不扩实现、不宣称全支持。

扩展点在 P0 定稿：音轨、字幕、seek、倍速、AI cue、清单调度都先留在公共接口里。后续阶段只实现，不推翻 ABI 重来。

## P0 设计定稿（进行中）

依赖：无。

交付：`README.md`、`docs/DESIGN.md`、`docs/ROADMAP.md`、`docs/PROGRESS.md`，以及贡献、安全和第三方说明。内容含边界、C ABI、四态矩阵、容器与清单分层、验收门槛、待确认项。

验收门槛：

- 文档互相引用的阶段、接口和「未实现」一致
- 没有把目标写成已实现
- 没有密钥和私有规则原文
- 用户书面确认设计后，P0 才算完成

已有实现代码与 `build.sh` 的部分：见 P1a。真机与四平台硬解仍未开始。

## P1a 会话与 TS 识别（进行中 / 本轮交付）

依赖：用户要求按既定设计开始写代码。许可证与第 18 节若干项仍待确认，不把 P0 写成已全部关闭。

范围：

- 公共 C ABI：会话、上限、分配器、push/pull/release、flush/reset/cancel/EOS、epoch
- 目标 codec 注册查询；级别与证据相符，枚举名字不等于已组帧
- MPEG-TS 188/192 同步、PAT/PMT 轨道发现；PES 仅内部提取，不发出 AU
- 硬解查询骨架固定返回非硬解 / 未核实
- 合成样本、碎片/畸形/上限/reset/跨会话测试；可用时 ASan/UBSan

验收门槛（本轮）：

- 合成 TS 能发出 FORMAT 与 TRACK（含 stream_type 对应的容器标识级别），不发出 H.264/AAC AU
- 恶意长度与坏 CRC、保留 adaptation、探测失败路径有测试
- reset 后 session_epoch 增加，旧 release_id 失效
- 硬解查询在无后端时不报告硬解成功

## P1b AAC ADTS 组帧（宿主开发自测已通过；非正式发版验收）

依赖：P1a 会话与 TS/PES。

范围：

- 仅 MPEG-TS 上的 AAC ADTS：完整一帧才发出 `AU`，由 ADTS 头生成 AudioSpecificConfig 作为 `CODEC_CONFIG`
- 支持子集：MPEG-2/4 ADTS（ID 0 或 1）、layer=0、protection_absent 0/1（读 7/9 字节头，不校验 CRC）、raw_data_blocks=0、sfi 0..12、通道 1..7、AOT=profile+1 的 2 字节 ASC
- 明确拒绝：LATM/LOAS、裸 AAC、raw_data_blocks!=0、channel_config=0（PCE）、index 13-15、SBR/PS 扩展 config；H.264 Annex B AU 未做
- 时间戳：PES PTS 属于在该 PES 内开始的第一帧；同一 PES 后续帧按 1024 采样 / 90 kHz 推算

验收门槛：

- 合成 ADTS 样本在隔离编译环境中通过：跨 TS/PES 边界、碎片输入、一 PES 多 AU、截断+EOS、坏长度、reset/flush、配置变化、反压不丢帧、时间戳归属
- P1a 旧测试全部回归
- `ms_codec_query(AAC)` 为 `AU`，`ms_codec_query(H264)` 仍为容器标识
- 不发出 H.264 AU / H.264 codec config

宿主开发自测（2026-11-19，北京时间）已执行；正式 el7 容器与四平台验收仍未做。

## P1c H.264 Annex B AUD 子集组帧（宿主开发自测已通过；非正式发版验收）

依赖：P1a 会话与 TS/PES，P1b 的事件/队列契约。

范围：

- 仅 MPEG-TS 上的 Annex B，以 AUD 为访问单元边界；3/4 字节起始码
- SPS/PPS 作为 codec config（Annex B：4 字节起始码 + SPS + 4 字节起始码 + PPS）
- 仅 `frame_mbs_only_flag=1` 的逐行帧；IDR 标记关键帧，不保证非 IDR 可安全起播
- PTS/DTS 只属于在该 PES 内开始的第一帧，不按不存在的帧率杜撰

明确拒绝：

- 无 AUD 的码流（不做 first_mb 通用边界）
- field / PAFF / MBAFF
- 数据分割条带与 MVC/SVC 等扩展 NAL
- 把 PES 或单个 NAL 当完整帧

宿主开发自测（2026-11-19，北京时间）已执行；正式 el7 容器与四平台验收仍未做。

## P1d H.264 无 AUD 主图像边界（宿主开发自测已通过；非正式发版验收）

依赖：P1c 的 AUD 组帧、RBSP/bitreader 与会话队列契约。

范围：

- 在现有 `ms_h264` 上增加无 AUD 路径：按 ITU-T H.264 7.4.1.2.4 比较主图像字段，不使用 `first_mb_in_slice=0` 作为通用边界
- 同一主图像多 slice 合成一个 AU；不同主图像不得合并；保留 AUD 路径
- 参数集按 ID 管理；POC 类型 0/1/2（类型 1 含 always_zero 与 delta）
- 未知 SPS/PPS、畸形语法、截断 NAL 拒绝且不越界

明确拒绝：

- field / PAFF / MBAFF、FMO、separate colour plane
- 数据分割条带与 MVC/SVC 等扩展 NAL
- 把 PES 或单个 NAL 当完整帧

宿主开发自测（2026-11-19 16:30，北京时间）已执行；正式 el7 容器与四平台验收、真实硬解出画仍未做。

## P1e 组装上限与会话动态总预算（宿主开发自测已通过；非正式发版验收）

依赖：P1a 上限合同，P1b/P1d 的 PES/ES 组帧与输出队列。

范围：

- `au_queue_bytes` 与 `max_assembly_bytes` 独立；缩小输出队列不得把合法多帧 PES 变成 `MALFORMED`
- `session_budget_bytes` 跨轨约束动态输入缓冲、输出队列载荷、PES/ES 组装、pending、参数集副本（含记账头）
- 固定会话对象（`ms_session` 本体、嵌入解析器/轨道/PSI/事件节点、create 时的输出槽表）**不计入**该预算，须在文档中写明
- `struct_size` 扩展合同：0 视为旧五字段布局；新字段追加在末尾
- `LIMIT` 可经 pull/release 缓解时允许重试；`consumed=0` 且 pull 为 `AGAIN` 时不得无限重试；单 PES/AU 超组装上限为 `MALFORMED`

宿主开发自测（2026-11-19 16:44，北京时间）已执行。P1 仍未通过，不进入 P2。

## P1f 按需分配轨道解析状态（宿主开发自测已通过；非正式发版验收）

依赖：P1e 动态预算与轨生命周期。

范围：

- 先测量后改：固定 `ms_session` 在 32 路全嵌入 H.264/ADTS 时约 140 KiB，其中绝大部分是未使用轨的参数集槽
- 轨道表仍保留最多 32 路元数据；H.264/ADTS 解析状态按首次 PES 惰性分配，计入 `session_budget_bytes`
- 固定本体与输出槽仍不计入该预算，须写明，不能声称进程 RSS 已被总预算限制
- 分配失败回滚、PMT 变更释放、reset/flush/EOS 与旧 AAC/H.264 路径保持
- 同一合成双轨样本、同一工具链与同一 budget 对比优化前后峰值；不得只把固定开销搬到动态却宣称无条件降低全部峰值

宿主开发自测（2026-11-19 16:54，北京时间）已执行。P1 仍未通过，不进入 P2。

## P1g 真实编码器输出的开发交叉验证（宿主开发自测已通过；非正式发版验收）

依赖：P1d 组帧子集，P1e/P1f 预算与会话。

范围：

- 用本机已有 ffmpeg/ffprobe 生成自有 lavfi testsrc/正弦 H.264+AAC TS，不链接进产品库，不把软件编码当产品软解
- 经公共 ABI 的 `ms_ts_inspect` 喂碎片 TS，对照 ffprobe 帧/包与独立 PES 重组、ADTS 去头载荷
- 发现的 EOS 未关闭 length=0 视频 PES、新 PES 时间戳被推算值覆盖，做最小修复并加回归
- 不得把这些编码样本夸大成全部 H.264 兼容

宿主开发自测（2026-11-19 17:06，北京时间）已执行。P1 仍未通过，不进入 P2。

## P1h AAC 余数时间戳与 TS 连续性基本安全（宿主开发自测已执行；非正式发版验收）

依赖：P1b–P1g。

范围：

- 同一 PES 内多帧 AAC 按 90 kHz 有理数余数累积，48 kHz 仍精确 +1920；新 PES PTS 重新锚定；无 PTS 不杜撰时钟
- 按 H.222.0/ISO 13818-1 小步处理 CC 间隙、合法重复与异常、纯适配不递增、显式 discontinuity、TEI；丢弃不完整 PES/ES 并发 DISCONTINUITY，不把缺片当完整 AU；不承诺 RTP 重排
- 合成针对测试 + 原 27 组回归；dev_crosscheck；有界外部 curl 实流抽样（不是产品网络/HLS 模块，不是硬解出画）
- 不得把实流抽样或 High@720p 分片写成全部 H.264 兼容或 P1 已通过

宿主开发自测（2026-11-19 17:30，北京时间）已执行。P1 仍未通过，不进入 P2。

## P1i 中途接入安全恢复与 evq 严格 FIFO（宿主开发自测已执行；非正式发版验收）

依赖：P1d 组帧子集，P1h 实流对照中暴露的问题。

范围：

- 区分等待 SPS/PPS/IDR、截取残缺与真正不支持语法；丢弃残片并在 IDR 后恢复，不把会话误标为 UNSUPPORTED，也不忽略 FMO/隔行/扩展 NAL
- evq 按入队世代严格 FIFO，保持 64 槽上限、预算、所有权与 epoch 合同；用超过 64 累计事件、交错 push-pull、背压、配置先于 AU、flush/reset 回归锁住，不得靠缩短用例躲避
- 完整原 29 组加新例，debug ASan/UBSan 与 release，dev_crosscheck，保存的实流 A/B/C 重跑

宿主开发自测（2026-11-19 17:43，北京时间）已执行。P1 仍未通过，不进入完整 P2。

## P1j PMT 前完整媒体回放与残缺尾帧（宿主开发自测已执行；非正式发版验收）

依赖：P1b ADTS 组帧，P1h/P1i 实流对照中暴露的音频计数差。

范围：

- 用独立 PES/ADTS 字节证据（载荷、文件偏移、PTS）对齐 inspect 与 ffprobe，区分「PMT 前完整帧被丢」与「截取造成的残缺尾帧」；不得把猜测当结论，不得把残缺帧当成完整 AU 发出以凑齐 ffprobe 包数
- 首次 PMT 前有界暂存未知 PID 的 TS 包（最多 128×188 字节，计入动态预算），PMT 登记轨道后再按序回放；超界丢最旧；reset/destroy/flush 释放
- 原 31 组回归 + `test_aac_pes_before_pmt_replay` + `test_aac_truncated_tail_not_au`；debug ASan/UBSan 与 release；dev_crosscheck；保存的实流 A/B/C 重跑

宿主开发自测（2026-11-20 16:31，北京时间）已执行。P1 仍未通过。本轮不落地 P2 实现。

## P1 最小解析闭环与硬解接口（未完成）



依赖：P0 确认。

范围：

- C ABI 骨架、格式/codec 注册表
- 识别基础：每个目标 codec 可查询，未写组帧的返回明确级别（通常是 NONE 或仅有标识），unknown 明确返回
- 第一条可测闭环：MPEG-TS + H.264 + AAC（ADTS）。碎片、截断、坏长度、reset、时间戳、队列上限
- 平台硬解查询接口。只有骨架时必须返回「非硬解 / 未核实」，不能返回成功
- 最终需要统一 C 硬解调用接口与真实平台后端（**计划，本步未实现**）：Android API 21+ `AMediaCodec`；API 16–20 走 Java MediaCodec 的 JNI（禁止 Java reflection，缓存 `jclass`/`jmethodID` 为 global ref；DirectBuffer 指针不得在 queue/flush 后永久复用）；Windows Media Foundation / D3D11；iOS VideoToolbox / 平台 surface；Linux VAAPI / V4L2。参考成熟官方接口，硬解保持外挂，不软解回退。压缩输入拷贝与解码后 Surface 零回读要分开写，不保证所有芯片无条件硬解。尚未实现不得写成已实现。
- 体积、内存、首个完整关键帧 AU 的测量记录（条件写清楚，不承诺固定体积）

验收门槛：

- 合成 TS 上能 pull 出带 codec config、PTS/DTS 时基和关键帧标记的 H.264 AU，以及 AAC AU
- 注册表中其余目标 codec 有条目且级别与证据相符，不能只枚举名字
- 恶意长度不导致无界分配；reset 后旧 AU 视图不可继续使用（epoch 变化）
- 硬解查询在无设备核实时不报告硬解成功
- 上述测试有可复现命令。通过后才允许 P2

可提前放进 P1 的只有「识别查询基础 + TS/H.264/AAC 组帧」。不承诺 P1 同时完成其余 codec 的组帧。

## P2 文件容器、场景与 seek（部分小步已落地，整体未完成）

依赖：完整 P2（含 seek/Source 与可播放验收）仍依赖正式 P1 验收。经典 FLV 出帧小步已在宿主自测落地，不等于完整 P2。平台无关的解析基础设施可以单独开小步，测试与硬解/四平台验收分列，不得写成 P2 已完成或 P1 已通过。

P2a（宿主开发自测已执行，2026-11-20 16:48，北京时间；非正式发版验收）：共用 ISO BMFF 箱体读取。可发出 FORMAT。不要把箱体基础单独写成可播放。

P2b（宿主开发自测已执行，2026-11-20 17:10，北京时间；非正式发版验收）：渐进 MP4/MOV（moov 先于 mdat）的 H.264/AAC 完整样本 AU。载荷合同为 avcC 长度前缀与裸 AAC，配置先于 AU；支持 `ctts` 有符号偏移，不把 elst 改写进 PTS，不伪造时间戳。fMP4/FLV/seek 未做。不要把本步写成可播放、完整 P2 或硬解成功。

P2c（宿主开发自测已执行，2026-11-20 17:22，北京时间；非正式发版验收）：为 moov 在 mdat 之后的渐进 MP4/MOV 做有界回看（默认 8 MiB，计入会话预算，仅可下调）。窗覆盖全部已错过样本时输出与 P2b 相同合同的 AU；超限或预算不足不整文件缓存，返回 `MS_ERR_NOT_SEEKABLE` 并投递 `UNSUPPORTED`，会话不失败。合成回归（含逐字节/碎片推送、上限溢出低峰值）+ 本机 ffmpeg lavfi 的 moov-at-end MP4/MOV 按解码顺序对照 payload/PTS/DTS/keyframe/config（chunk=1/17/512/整文件）。不是任意时间 seek，也不是 Source size+range 通用随机读。

P2d（宿主开发自测已执行，2026-11-20 17:40，北京时间；非正式发版验收）：fMP4 init + 顺序 moof/mdat 的 H.264/AAC 完整 AU，合同与渐进路径相同（`AVCC`/`AAC_RAW`，配置先于 AU，媒体时间线 DTS 与 DTS+cto，解码序）。覆盖 trex/tfhd/tfdt/trun 默认值与 flag、基址/data_offset、多 traf/trun、跨片时间线；加密/缺 init/moof 在 moov 前明确报错或等待，不吐假帧。合成回归 + 本机 ffmpeg lavfi 多片/双轨/B 帧 fMP4，独立 moof/trun 表与 ffprobe/payload 对照（chunk=1/17/512/整文件）。不是全部 fMP4/CMAF，不是解密，不是可播放或硬解成功。

P2e（宿主开发自测已执行，2026-11-20 18:05 CST，Asia/Shanghai；非正式发版验收）：经典 FLV 的 H.264 AVC / AAC 完整 AU，载荷合同与 BMFF 相同（`AVCC`/`AAC_RAW`，配置先于 AU，32 位扩展毫秒 DTS 与 DTS+有符号 24 位 cto，解码序）。校验 tag 大小 / prevTagSize / StreamID / 头 flags；脚本跳过且不做无界 AMF；Enhanced FLV 与非 AVC/AAC 诚实拒绝；负 PTS 不回绕。本轮保持既有 copy-push 所有权合同。合成回归（含异常 tag、截断、脚本垫片、反压、EOS、reset 后改喂 TS）+ 本机 ffmpeg lavfi 单视频/单音频/双轨/B 帧 FLV，独立 tag 表与 ffprobe 对照 payload/PTS/DTS/keyframe/config（chunk=1/17/512/整文件）。另有链接 `libmsmedia.so` 的公共 ABI 冒烟（该程序不加 sanitizer）。不是 RTMP/HTTP-FLV 客户端，不是 Enhanced FLV，不是可播放或硬解成功。

有测量的缓冲所有权小步（宿主开发自测已执行，2026-11-20 18:27 CST，Asia/Shanghai；非正式发版验收）：保留旧 copy-push 入参 ABI；完整 AU 在会话独占块中时入队改为所有权移交（FLV body 视图、BMFF `sbuf`、TS H.264/ADTS 先拷剩余再移交）；配置与碎片组帧仍复制。新增 `ms_session_get_stats`（`struct_size`，导出版本仍为 `MSMEDIA_1.0`，ABI 版本仍为 1）。复制次数/字节与 FORMAT/TRACK/CONFIG/首可解码 AU 阶段可测；解析就绪不是首画面。事件不能活过会话；预算在移交时不丢。不宣称全链路零拷贝。公开 retain/borrow/`push_owned`、共享引用碎片、硬解表面未做。

Source 随机读与渐进 MP4/MOV 时间定位已在宿主自测落地（2026-11-20 18:48 CST），非正式发版验收。零拷贝与硬解表面路径的设计边界见 DESIGN 专节：不虚称全链路零拷贝，不把压缩输入零拷贝与解码后 GPU 零回读混为一谈，不宣称 MediaCodec 输入恒为零拷贝。不承诺固定毫秒。

范围（完整 P2，仍未完成）：ISO BMFF / 经典 FLV 的已落地子集之外的能力、直播 / 点播 / 本地文件在**单容器字节流**上的差异、Source 的 read/seek/size/cancel/range、时间轴与关键帧定位、seek 后的 epoch / flush / 预滚 / 音频裁剪 / 字幕重置。倍速接口可先接通到播放控制层，保音调外挂仍可不实现。零拷贝所有权与硬解出画也不在本阶段宣称完成。

验收门槛：

- 至少一条可 seek 的合成 MP4（或 fMP4）按时间落到关键帧并完成预滚标记
- moov 在尾部：有界回看已能在上限内出 AU；超出上限、无 Source 时仍明确 `NOT_SEEKABLE`。已挂可随机读且有确定大小的 Source 时可跳过 mdat 读尾 moov 并按表取样本，不依赖整段回看。完整 P2 的网络 range / 其它格式任意 seek / 可播放验收仍未完
- 不可 seek 的直播返回不支持，而不是卡住
- FLV 与 MOV 子集的未覆盖 codec 返回 unknown 或不支持
- P1 的 TS 测试仍然通过

## P3 清单与实时插件（已完成 media playlist 调度、可选 HTTP/HTTPS 首步与 DASH MPD 最小子集，其余未开始）

依赖：HLS/DASH 要把分片交给已有 TS/fMP4 组帧；完整 P2 可播放验收仍未完，不阻塞本调度小步的宿主自测。

P3 首步（宿主开发自测已执行，2026-11-20 19:18 CST，Asia/Shanghai；非正式发版验收）：HLS media playlist 调度。`ms_hls_drive` 每次最多发起或读取一个 fetch，或把已读字节有界推进会话；AGAIN 不忙等。VOD ENDLIST 与无 ENDLIST 的直播窗口、MEDIA-SEQUENCE、RFC 3986 基址/相对/query、有界刷新去重、`EXT-X-DISCONTINUITY`、`EXT-X-MAP` 与可选 BYTERANGE。分片失败或序号跳跃会 `ms_session_signal_discontinuity`，不把残缺装配与下一片简单拼接。master（STREAM-INF / EXT-X-MEDIA 等）本步明确 `UNSUPPORTED`（不做半成品首变体挑选）。`EXT-X-KEY` METHOD 非 NONE、LL-HLS、替代音轨/字幕、ABR 明确不支持，不把密文当媒体。内存/文件 fetch 为开发 harness，不是 HTTP/TLS 客户端，不链接 curl。合成与 harness 回归覆盖排序/时间线、重复片、窗口滑动、重试预算、取消、反压、截断；原 40 组与交叉脚本、`.so` smoke 回归。随后（2026-11-20 19:35 CST）已在源码侧规避 gcc 14.2 `-O2` 将缺失分片 + `signal_discontinuity` 误报 `NOMEM` 的问题，去掉全局 `-fno-tree-vrp`，并为调度器增加 `max_bytes` 总预算。2026-11-20 20:00 CST 起，可选 `libmsnet` 提供自有 HTTP/1.1 GET 与系统 OpenSSL TLS（证书链+主机名/SNI，不可关闭校验，最低 TLS 1.2），并接到既有 HLS fetch；本机受控 HTTP/HTTPS 服务已验证 200/206/chunked/重定向/循环/401/403/404/编码拒绝/超时/取消/证书失败/错误主机名/降级拒绝，以及 media playlist 经真实 HTTP 出 TS AU、master 仍 `UNSUPPORTED`。内存/文件 fetch 仍可用于不链网络的 harness。不是可播放或硬解出画，不是四平台或 el7 验收。

2026-11-20 20:25 CST 起，P3 增加 MPEG-DASH（MPD）调度最小子集：`ms_dash_*` 复用既有 `ms_hls_fetch`、会话 fMP4（主）与 TS AU 路径、不连续信号、反压与自记账 `max_bytes`（绝对值 4 MiB）。解析常见 static/dynamic MPD 的单 Period、单 AdaptationSet、单 Representation；SegmentBase（无 indexRange）/ SegmentList / SegmentTemplate（有界 `$Number$` / `$Time$` 等）；Initialization + media，可选字节范围；有界清单刷新与去重；缺片/截断/序号跳跃发不连续，不拼接残缺。XML 为有界自制分词，拒绝 DOCTYPE/外部实体。明确拒绝且不默默支持：多变体 ABR、ContentProtection/DRM、xlink、字幕/交替轨、独立 A/V AdaptationSet、多 Period、indexRange/sidx、CMAF LL 全集。宿主 `test_dash_mpd` 以受控 MPD + 分段出真实 AU，并覆盖拒绝、缺片、截断、取消、反压、预算与动态去重。2026-11-20 20:34 CST 起，`test_net` 把 `ms_dash_*` 接到已有 `ms_net_hls_fetch_bind`：本机受控 HTTP 服务 MPD + TS 分段，覆盖 200 VOD、SegmentList mediaRange（206）、缺片/截断不连续、取消、反压，以及 ContentProtection 明确拒绝。fMP4 主路径仍以 `test_core` 内存/文件 harness 覆盖，本 HTTP 闭环用已有 TS 合成样本（产品不链 FFmpeg，固件不入库）。不是全部 DASH profile，不是可播放或硬解出画，不是四平台实机验收。 2026-11-20 21:21 CST 起，用户明确要求常用 RTSP/RTMP/UDP/RTP/HTTP/HTTPS 都支持并使用最新 FFmpeg 9 裁剪优化；自写 HLS/MPD/网络停止扩展但保留回归。可选 `libmsff` 钉选官方 9.1，本地 TS/MP4/FLV 与受控 HTTP+HLS/HTTPS 已在宿主验证。2026-11-20 22:07 CST（[pt71u]）起，可选 `libmsff` 已在宿主验证受控 DASH-over-HTTP（分离 A/V、相对 URL、无扩展名、302 基址、缺片/截断/401/取消/超时）以及 UDP MPEG-TS 与 RTP/SDP H264 子集。RTSP/RTMP 仍未联调。2026-11-21 11:12 CST 修正时间戳 origin 与 TIMEOUT，并用独立 trun 核实 DASH 尾包。2026-11-21 11:26 CST 起，`libmsff` 静态合入钉选 OpenSSL 3.5.9 LTS，不再 NEEDED 系统 libssl/libcrypto；libxml2/z 仍动态。2026-11-21 11:58 CST 已在宿主完成 RTSP TCP/UDP、RTP 帧合同与 RTMP 拉流联调（RTMPS 未测）。2026-11-21 12:19 CST 完成体积优化小里程碑（`./build.sh release-size`；保留既有 release 作基线；不换 mbedTLS）。详见 `docs/FFMPEG_BACKEND.md`。本条之后的硬解小里程碑已于 2026-11-21 12:36 CST 交付宿主 mock 与非正式 NDK 交叉编译（`docs/HW_ANDROID.md`）；真实设备硬解仍未跑。

顺序：评估会话多 Initialization（否则不能宣称分轨 AS / Period 切换）；代理/复用等 HTTP 后续与 RTMP、RTSP、RTP、UDP 插件仍待做。不要顺便把 ABR/DRM 写成已支持。

验收门槛：

- 清单模块只调度分片和初始化段，容器解析仍走 TS 或 fMP4
- 点播与直播、不连续、音轨/字幕 rendition、变体切换有对应事件或明确的不支持
- DASH 默认路径是 fMP4；WebM 不在默认覆盖里
- RTP 重排、丢包和 depacketize 的测试不叫做容器 demux
- 插件可从构建中整项裁掉，核心链接不出现网络库和 TLS 库
- 自有 HTTPS/RTMPS/RTSPS 只链接一份 OpenSSL 封装（当前仅 HTTP/HTTPS 路径使用；RTMPS/RTSPS 未实现）。第一版不写第二套 TLS 后端。不把 OpenSSL 源码或二进制打进仓库，只链系统库；核心链接行仍不得出现网络库和 TLS 库
- 每个开源依赖已写入 `docs/THIRD_PARTY.md`

## P4 其余 codec、多音轨、字幕、倍速（音轨/文本字幕与倍速/相对 seek 切片已在宿主落地；其余编码与备选 rendition/ASS 渲染未完成）

依赖：P1 注册表；涉及 MP4/HLS 的音轨和字幕行为还依赖 P2/P3 中相应容器。纯 TS 多音轨可以在 TS 已验收后先做。

范围：H.263、HEVC、MPEG-4 Part 2、MPEG-2 Video、AVS+、AV1、AC-3、E-AC-3、MP3、MP2、Opus、G.711 A-law/μ-law 的证据级识别与组帧（逐个合并，未做的保持低级别）。图中只列 H.264/HEVC 不删本表其余目标。多音轨与字幕的枚举、选择、关闭、动态变化。倍速播放和快进快退（相对 seek）。时间拉伸外挂可选，默认不链接。

2026-11-21 20:10 CST（Asia/Shanghai）切片（宿主事件路径，非正式验收，非设备出声/上屏）：同一容器内音轨枚举/元数据/选择与播放中切换（flush 旧音频配置、重建时钟、`selection_epoch++`，不重开 Source）；文本字幕枚举/选择/关闭、SRT/WebVTT 有界解析与时间轴、seek/flush 清在途 cue；已接到 Android JNI 与测试 APK 的叠加 TextView / 选择控件。位图与 ASS/SSA 可枚举、选择播放为 `UNSUPPORTED`。HLS 备选 rendition、倍速、完整 seek 控制、其余 codec 仍未开始。不得把宿主 mock 或 APK 编译写成设备已听见或看见。

验收门槛：

- 每合并一个 codec，矩阵四列都有测试或明确仍为「未实现」
- 2026-11-21 21:10 CST：HEVC 已按证据级在宿主落地（见 FEATURE_MATRIX / PROGRESS）；其余 P4 编码仍未合并。
- 2026-11-21 21:29 CST：HLS 备选 AUDIO 外链已在 libmsff 宿主落地（见 FEATURE_MATRIX / PROGRESS）。
- 2026-11-22 01:30 CST：HLS 字幕 rendition（独立 WebVTT 外链）已在 libmsff 宿主落地。2026-11-22 01:55 CST：HLS 视频变体手动选择（同 codec 两档）已在宿主落地，不承诺自动 ABR。2026-11-22 02:38 CST：HLS 字幕 seek 覆盖进行中 cue 与直播短 VTT 片头已在宿主闭环，设备未验；完整 ASS 渲染仍为欠账
- 普通 AVS 标识不能得到 AVS+
- 音轨切换后旧解码配置被 flush，selection epoch 变化；能不重开取流的样本不重开，HLS 备选 rendition 允许换资源
- 字幕 cue 带时间戳；核心不做字体和 ASS 排版
- 倍速路径不新增音频软解

## P5 AI 字幕（可选；宿主框架已落地，真实供应商未指定）

依赖：P4 的字幕 cue 接口；PCM 来自已核实的平台解码输出（修剪后可听样点）。

范围：独立插件框架。语音转写为主；翻译更新归并到同一生成轨，仍不选定翻译供应商。核心无模型、无云 SDK、无供应商地址。不得把假供应商测试写成真实识别验收。

验收门槛：

- 核心库不包含模型、不发起网络、不下载权重（宿主已满足；`libmsmedia`/`libmsff` 不 NEEDED `libmsai`）
- 没有 PCM tap 或未配置供应商时能力查询为不可用；禁用时不采集（宿主已测）
- seek、flush、切音轨后旧识别结果因 epoch 不匹配被丢弃（宿主已测）
- 默认不上传；云路径未授权时不可调用（本切片无云路径，未接真实供应商）
- 真实供应商、凭据、上云授权与真机识别效果：仍待用户后续决定，未验

## 贯穿项（从 P1 起，未开始）

每阶段都要有：有界内存、坏包、取消/reset、相关旧测试、以及当阶段测量记录。缺编译器或真机时写未验证，不顺延成「默认通过」。

正式四平台硬解（Android MediaCodec+Surface、iOS VideoToolbox、Windows 硬件 MF/D3D11、Linux VAAPI/V4L2/厂商接口）在对应平台工具链和设备上单独验收。宿主上的功能测试不能代替该验收。 2026-11-21 12:36 CST 已交付可选 Android H264 路径的代码、宿主 mock 测试与非正式 NDK 交叉编译/探测 APK（`docs/HW_ANDROID.md`）；这仍不是设备硬解验收，也不是四平台验收。2026-11-22 05:17 CST 已交付 Linux VAAPI 码流参数映射与 DRM 探测（`docs/HW_LINUX.md`，官方 ASan/UBSan 与 `test_hw_vaapi` 已通过）；本机无 `/dev/dri`，slice 提交未实现，不得写成 Linux 硬解出画。本机无 Windows/iOS 工具链，该两平台本轮未写未编译代码。

## 可选：快速换台（未开始，未单列阶段）

依赖：直播取流（P3 或平台 HTTP）之后才谈得上插件；无 IDR 的安全恢复点依附 H.264（P1）和 HEVC（P4）的组帧与参考判定。那些测试不存在之前，不声称可以安全起播。

范围只写在 `docs/DESIGN.md` 第 17 节：按下预连接在宿主控制层；FCC 为可选服务端插件，不承诺图中的电信、中兴、烽火、华为兼容；4K 0.35 秒不是验收线。

验收门槛（将来单独立项时才适用，现在全部未测）：

- 取消按下预连接后没有残留拉流
- FCC 写明突发到组播的切换、去重、时间戳和超时回退，并在指定服务端上有成功和失败记录
- 参数集齐全但参考不完整的开放 GOP / HEVC leading picture 不会被送显
- 时延记录含冷暖启动、设备、源、网络、分位数和卡顿率
