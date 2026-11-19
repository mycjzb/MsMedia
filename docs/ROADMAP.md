# 任务路线图

by: Long <mycjzb@gmail.com>

状态日期：2026-11-19（北京时间）。P0 文档已落盘但许可证等项仍待确认。P1a–P1i 已有实现；P1i 为中途接入等待 SPS/PPS/IDR 的安全恢复与 evq 严格 FIFO。P1 正式 el7/四平台/硬解闭环仍未完成，**不将 P1 写为通过，不进入 P2**。宿主自测不等于正式验收。

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

宿主开发自测（2026-11-19 17:43，北京时间）已执行。P1 仍未通过，不进入 P2。

## P1 最小解析闭环与硬解接口（未完成）



依赖：P0 确认。

范围：

- C ABI 骨架、格式/codec 注册表
- 识别基础：每个目标 codec 可查询，未写组帧的返回明确级别（通常是 NONE 或仅有标识），unknown 明确返回
- 第一条可测闭环：MPEG-TS + H.264 + AAC（ADTS）。碎片、截断、坏长度、reset、时间戳、队列上限
- 平台硬解查询接口。只有骨架时必须返回「非硬解 / 未核实」，不能返回成功
- 体积、内存、首个完整关键帧 AU 的测量记录（条件写清楚，不承诺固定体积）

验收门槛：

- 合成 TS 上能 pull 出带 codec config、PTS/DTS 时基和关键帧标记的 H.264 AU，以及 AAC AU
- 注册表中其余目标 codec 有条目且级别与证据相符，不能只枚举名字
- 恶意长度不导致无界分配；reset 后旧 AU 视图不可继续使用（epoch 变化）
- 硬解查询在无设备核实时不报告硬解成功
- 上述测试有可复现命令。通过后才允许 P2

可提前放进 P1 的只有「识别查询基础 + TS/H.264/AAC 组帧」。不承诺 P1 同时完成其余 codec 的组帧。

## P2 文件容器、场景与 seek（未开始）

依赖：P1 验收。

范围：FLV、ISO BMFF（MP4 / fMP4 / MOV 子集）、直播 / 点播 / 本地文件在**单容器字节流**上的差异、Source 的 read/seek/size/cancel/range、时间轴与关键帧定位、seek 后的 epoch / flush / 预滚 / 音频裁剪 / 字幕重置。倍速接口可先接通到播放控制层，保音调外挂仍可不实现。

验收门槛：

- 至少一条可 seek 的合成 MP4（或 fMP4）按时间落到关键帧并完成预滚标记
- moov 在尾部时走 size/range 或明确返回能力不足
- 不可 seek 的直播返回不支持，而不是卡住
- FLV 与 MOV 子集的未覆盖 codec 返回 unknown 或不支持
- P1 的 TS 测试仍然通过

## P3 清单与实时插件（未开始）

依赖：P2 中 fMP4 与 TS 的组帧已验收（HLS/DASH 要喂给它们）。

顺序：先 HLS（m3u8）和 MPEG-DASH（MPD），再 RTMP、RTSP、RTP、UDP 插件。

验收门槛：

- 清单模块只调度分片和初始化段，容器解析仍走 TS 或 fMP4
- 点播与直播、不连续、音轨/字幕 rendition、变体切换有对应事件或明确的不支持
- DASH 默认路径是 fMP4；WebM 不在默认覆盖里
- RTP 重排、丢包和 depacketize 的测试不叫做容器 demux
- 插件可从构建中整项裁掉，核心链接不出现网络库和 TLS 库
- 自有 HTTPS/RTMPS/RTSPS 只链接一份 OpenSSL 封装。第一版不写第二套 TLS 后端。未实现前不引入 OpenSSL 源码或二进制
- 每个开源依赖已写入 `docs/THIRD_PARTY.md`

## P4 其余 codec、多音轨、字幕、倍速（未开始）

依赖：P1 注册表；涉及 MP4/HLS 的音轨和字幕行为还依赖 P2/P3 中相应容器。纯 TS 多音轨可以在 TS 已验收后先做。

范围：H.263、HEVC、MPEG-4 Part 2、MPEG-2 Video、AVS+、AV1、AC-3、E-AC-3、MP3、MP2、Opus、G.711 A-law/μ-law 的证据级识别与组帧（逐个合并，未做的保持低级别）。图中只列 H.264/HEVC 不删本表其余目标。多音轨与字幕的枚举、选择、关闭、动态变化。倍速播放和快进快退（相对 seek）。时间拉伸外挂可选，默认不链接。

验收门槛：

- 每合并一个 codec，矩阵四列都有测试或明确仍为「未实现」
- 普通 AVS 标识不能得到 AVS+
- 音轨切换后旧解码配置被 flush，selection epoch 变化；能不重开取流的样本不重开，HLS 备选 rendition 允许换资源
- 字幕 cue 带时间戳；核心不做字体和 ASS 排版
- 倍速路径不新增音频软解

## P5 AI 字幕（未开始，可选）

依赖：P4 的字幕 cue 接口；PCM 来自已核实的平台解码输出。

范围：独立插件。语音转写为主，翻译只留接口。无模型、无云 SDK、无供应商。

验收门槛：

- 核心库不包含模型、不发起网络、不下载权重
- 没有 PCM tap 时能力查询为不可用
- seek、换轨、换台后旧识别结果因 epoch 不匹配被丢弃
- 默认不上传；云路径未授权时不可调用

## 贯穿项（从 P1 起，未开始）

每阶段都要有：有界内存、坏包、取消/reset、相关旧测试、以及当阶段测量记录。缺编译器或真机时写未验证，不顺延成「默认通过」。

正式四平台硬解（Android MediaCodec+Surface、iOS VideoToolbox、Windows 硬件 MF/D3D11、Linux VAAPI/V4L2/厂商接口）在对应平台工具链和设备上单独验收。宿主上的功能测试不能代替该验收。

## 可选：快速换台（未开始，未单列阶段）

依赖：直播取流（P3 或平台 HTTP）之后才谈得上插件；无 IDR 的安全恢复点依附 H.264（P1）和 HEVC（P4）的组帧与参考判定。那些测试不存在之前，不声称可以安全起播。

范围只写在 `docs/DESIGN.md` 第 17 节：按下预连接在宿主控制层；FCC 为可选服务端插件，不承诺图中的电信、中兴、烽火、华为兼容；4K 0.35 秒不是验收线。

验收门槛（将来单独立项时才适用，现在全部未测）：

- 取消按下预连接后没有残留拉流
- FCC 写明突发到组播的切换、去重、时间戳和超时回退，并在指定服务端上有成功和失败记录
- 参数集齐全但参考不完整的开放 GOP / HEVC leading picture 不会被送显
- 时延记录含冷暖启动、设备、源、网络、分位数和卡顿率
