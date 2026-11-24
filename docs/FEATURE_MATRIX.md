# MsMedia 功能闭环矩阵

by: Long <mycjzb@gmail.com>

日期：2026-11-22 05:18 CST（Asia/Shanghai）；表头能力仍含 2026-11-21 与当日位图/AVS+/HLS 时间轴、累积正确性/ABI 审查、PCM trim、ASS、P5 AI 框架，以及本轮 Linux VAAPI 码流映射与 DRM 探测。本表整理当前已落地、产品刻意边界与承诺尚未兑现的能力。**返回 `UNSUPPORTED` 不等于功能完成。** 宿主编译、mock、无设备探测与非正式 NDK 交叉编译不是 el7 / 四平台 / 真机硬解、出画或出声验收。不是项目完成声明。

状态用语：

- **宿主已落地**：Bot 本机有可复现测试或构建证据，仍非正式发版验收。
- **设备未验**：代码或 APK 已接线，但没有真机/共享 AVD 证据。
- **承诺尚未兑现**：路线图或既定架构要求实现，当前未完成；即便接口会返回 `UNSUPPORTED`，也不得标为完成。
- **产品刻意边界**：明确不做或永久拒绝；`UNSUPPORTED` 在此是诚实拒绝，不是欠账。

## 1. 取流与容器

| 能力 | 状态 | 说明 |
| --- | --- | --- |
| 自写 TS / 渐进 MP4/MOV / fMP4 子集 / 经典 FLV 的 H.264/AAC AU | 宿主已落地（保留备用） | 非正式验收。通用协议与较新编码以官方 lavf（`libmsff`）为准；**不把备用自写路径尚未对等的 HEVC/MPEG-2/4/H.263/AV1/AVS+ 组帧列为必须开发** |
| Source 随机读与渐进 MP4/MOV 时间定位 | 宿主已落地 | 会话 `ms_session_seek` 仅渐进样本表；其它容器 `NOT_SEEKABLE` |
| 自写 HLS media playlist / DASH 最小子集 | 宿主已落地（保留备用） | 通用 HLS/DASH 以官方 lavf 为准；不要求备用 `ms_hls`/`ms_dash` 追平官方路径。master / ABR / 加密等为明确拒绝 |
| 可选 `libmsff`：file/http/https/udp/rtp/rtsp/rtmp/rtmps + 官方 demux | 宿主已落地 | 钉选 FFmpeg 9.1，`CONFIG_DECODERS=0`；校验不可关 |
| Matroska / MKV（`libmsff` 官方 matroska demuxer；内嵌已支持编码与字幕） | 宿主已落地 / 设备未验 | 只按需打开 `--enable-demuxer=matroska`（LGPL 2.1；同一 `FFInputFormat` 名为 `matroska,webm`，扩展名含 mkv/webm/mka/mks/mk3d）。白名单同步加入 `matroska,webm`。加法 `MS_FORMAT_MATROSKA=11`、`MS_FORMAT_WEBM=12`（仅 URL 可辨 `.webm` 时标 WEBM），`MSMEDIA_ABI_VERSION` 仍为 1。复用既有轨道枚举/选择、时间轴、seek、切音轨、字幕 `ms_sub` 路径，不平行重写。已测受控 fixture（系统 ffmpeg 只作复用/编码，**不等于产品软解**）：H.264+双 AAC+SRT+ASS、HEVC、WebVTT、PGS 位图（`-c copy` 入 MKV 后走现有位图路径得到 16×16 RGBA cue）、截断/超大 ASS 文本 cue 有界、取消、关闭清理。FFmpeg 9.1 对本类 MKV 常把 `start_time` 记为 0，AAC/Opus priming 在 `initial_padding`；会话 origin 纳入该字段，以免首个视频 AU 之后负 PTS 被当成 `OVERFLOW`。**WebM** 仅用已支持 Opus 验证与 matroska 的绑定，**不宣传 VP8/VP9/Vorbis，也不把未测的 mk3d/mka/mks 写成已验收**。未开 matroska muxer、bzlib、任何 decoder。宿主 release-size 必须使用 `CONFIG_MATROSKA_DEMUXER=1` 的前缀：历史 `build/ffmpeg-9.1-size`（2026-11-21 12:05 CST）缺该 demuxer，已用隔离 `build/ffmpeg-9.1-size-audit-20261122` 收口（PROGRESS §43），不得只看被改写的 flags 日志。完整 ASS 栅格见第 4 节可选 `libmsass`（宿主像素证据已有，不是设备视觉验收；去标签路径仍不是完整渲染）。VP9 / E-AC-3 未扩承诺。设备未验 |
| FFmpeg 软件解码 / 编码 | 产品刻意边界 | 禁止启用；不得用软解补齐硬解缺口 |
| GPL / nonfree 组件 | 产品刻意边界 | 官方配置保持关闭 |
| HLS 备选 rendition 音轨（`EXT-X-MEDIA TYPE=AUDIO` 外链，经 `libmsff` / 官方 HLS demuxer） | 宿主已落地 / 设备未验 | 复用 FFmpeg AVProgram/AVStream/rendition，不平行写清单调度。覆盖相对 URI、查询串、GROUP-ID、语言/名称/DEFAULT、默认选择、来回切换、关闭再启、VOD seek。切换待确认后才停旧输出并重发 CSD；失败不拆会话、不把新轨标已选。JNI 异步入队，网络在 worker。自写 `ms_hls` master 仍拒绝。字幕 rendition 与视频变体见下行。mock/解析不是设备出声 |
| HLS 备选字幕 rendition（`EXT-X-MEDIA TYPE=SUBTITLES` 外链，经 `libmsff` / 官方 HLS demuxer） | 宿主已落地 / 设备未验 | 对标已落地的备选 AUDIO：复用官方 FFmpeg HLS 的 AVProgram/AVStream/rendition，不另写清单调度。覆盖相对 URI、查询串、GROUP-ID、LANGUAGE/NAME/DEFAULT/AUTOSELECT/FORCED、默认选择（`DEFAULT=YES` 可播则自动选，否则 `FORCED`；单独 `AUTOSELECT` 不自动打开）、来回切换、关闭再启、非法 id、畸形 GROUP-ID、缺失 playlist、超时取消、有界队列、VOD seek 后仍为新轨。WebVTT 独立 vtt playlist 真实 demux 到统一 cue 并与媒体时间对齐（校验 cue 文本与时间，不只 API 返回值）。切字幕不无故重开视频、不毁掉音频会话；pending 后才提交，失败撤回、不伪报成功。JNI 仍异步入队，网络在 worker。官方 lavf 对 `TYPE=SUBTITLES` 外链强制走 `webvtt` demuxer 打开**分片 URL**；已测的是独立 WebVTT playlist，**不是** MPEG-TS 封装的 WebVTT 外链，也不是主视频 TS 带内 WebVTT。清单里若出现 ASS/SRT 外链，不宣传为已验收（打开阶段仍按 webvtt 探测，能力不够须诚实）。打开后对含字幕流的 VOD 仍以 `AVSEEK_FLAG_ANY|BACKWARD` 回绕到 0。用户 seek 走官方 `hls_read_seek`，但字幕包在 cue 终点（开区间）仍大于 seek 点时保留，已过期 cue 不复活；长 cue 续显原 start/end。跨段相同 cue 在 `ms_sub` 去重。`X-TIMESTAMP-MAP` 由补丁把 LOCAL/MPEGTS 映到媒体时间（保持 webvtt 1/1000 时基以免与 HLS stub 主流转冲突），非零 `MEDIA-SEQUENCE` 不把时间平移成段号。短 VTT 片头 cue 在直播探测后仍进入时间轴。字幕 playlist 首次启用时从当前滑动窗口 `start_seq_no` 重开（该路径未改）。HLS EVENT/滑动窗口的定位见第 5 节，不另建 DVR、不无限本地缓存。补丁：`patches/ffmpeg-9.1-hls-sub-rendition-switch.patch`（关闭残留 webvtt ctx、字幕不 catch-up 丢包）与 `patches/ffmpeg-9.1-hls-sub-timeline.patch`（seek 覆盖进行中 cue、直播窗口重放、X-TIMESTAMP-MAP），由宿主与双 ABI size 脚本幂等应用。不得把关闭残留 ctx 扩到视频/音频 playlist（空 ctx 会崩）。只开 `--enable-demuxer=webvtt`（`CONFIG_WEBVTT_DEMUXER=1`，`CONFIG_WEBVTT_DECODER=0`/`MUXER=0`），白名单加入 `webvtt`，`CONFIG_DECODERS=0`，产品 FFmpeg 仍 `CONFIG_LIBASS=0`。完整栅格若启用走隔离 `libmsass`，不链进 libmsff。fixture 编码不等于产品软解。设备未验；发出 cue 不等于画面已绘制字幕 |
| HLS 视频变体 / 多码率手动选择（master `EXT-X-STREAM-INF`，经 `libmsff` / 官方 HLS demuxer） | 宿主已落地 / 设备未验 | 只做既定的手动选择，**不承诺自动 ABR**。复用官方 AVProgram/AVStream/playlist 与音/字幕同一套 pending-commit，不平行写 master 调度。枚举只填实际已知元数据：打开时 codecpar 宽高常未知，名称可带 `variant_bitrate`；切换后从真实 CSD/AU 的 Annex B SPS 核验 160×120 / 320×240，不只 API 返回值。同 codec（已测 H.264）来回切换：等新轨关键帧才提交，重发 CSD、不复用旧尺寸/参数、抬 `selection_epoch` 不抬 `timeline_epoch`。跨 codec 本切片明确 `UNSUPPORTED`，不得把该返回写成全部切换完成。不允许关闭最后一条视频轨。关联 AUDIO/SUBTITLES 组保持用户选择；无独立音组、音轨复用在旧变体 TS 内时，回退到新变体的 muxed 音轨（已测），不得假装仍选旧轨。失败/EOF/超时撤回 pending，取消与队列上限有界，不伪报成功、不无限等待。JNI 只入队，网络在 worker。未改字幕补丁去关闭视频 mpegts ctx（关闭后 `av_read_frame(pls->ctx)` 空指针）；音视频仍走官方 catch-up。前缀 libavformat 相对上一切片未改产品代码，Android 只重链适配器。fixture 编码不等于产品软解。设备未验；mock 提交不是真机出画 |
| HLS AES / SAMPLE-AES、DASH DRM/CENC | 产品刻意边界（本阶段） | 明确拒绝，不把密文当媒体 |
| LL-HLS、完整 CMAF 低延时、多 Period ABR | 承诺尚未兑现 / 部分为边界 | 见 ROADMAP P3；不得因拒绝写成完成 |
| 四平台网络栈与 el7 正式构建 | 承诺尚未兑现 | 仅 Bot 宿主编译自测 |

## 2. 视频解码与出画

| 能力 | 状态 | 说明 |
| --- | --- | --- |
| 公共硬解状态机 + 宿主 mock | 宿主已落地 | mock 不是设备硬解 |
| Android MediaCodec 视频路径（测试 APK） | 设备未验 | 非正式 NDK r29 API 21 双 ABI 已交叉编译；未装机、未出画 |
| 视频软件解码 | 产品刻意边界 | 拒绝；`allow_software` 不得变成全面软解 |
| HEVC（FFmpeg MP4/TS AU/CSD、中途 VPS/SPS/PPS、seek 关键帧门控、NDK/JNI 配置与提交、宿主 mock） | 宿主已落地 / 设备未验 | 公共 `ms_hevc_*` 有界解析；`CONFIG_DECODERS=0`；视频只走硬解/mock 提交。自写 TS 对 `0x24` 仍为容器标识，未接 AU 装配器。`ms_codec_query(HEVC)` 仍为 `CONTAINER_ID`。无硬件时能力/错误须诚实，mock 不是设备硬解或出画 |
| MPEG-2 Video / MPEG-4 Part 2（FFmpeg 已开容器到完整压缩 AU + 真实 CSD/PTS-DTS、中途配置、seek 关键帧门控、统一 player 与 Android NDK/JNI 查询-配置-提交、宿主 mock） | 宿主已落地 / 设备未验 | 公共 `ms_video_*` 通用配置/AU/关键帧模型，避免每编码再复制一套。加法 `MS_SAMPLE_FMT_MPV`/`MP4V`。MIME：`video/mpeg2`、`video/mp4v-es`。前缀只追加 `mpegvideo` 与 `mpeg4video` parser，`CONFIG_DECODERS=0`，无软解回退。自写 TS 对 `0x02`/`0x10` 仍为容器标识，`ms_codec_query` 仍为 `CONTAINER_ID`。测试 fixture 用宿主 ffmpeg 编码器生成，不等于产品允许解码。无硬件须诚实 `UNSUPPORTED`；mock 不是设备硬解或出画 |
| H.263（FFmpeg 已开 mov/3GP 到完整压缩 AU + 帧头宽高、PTS、关键帧、seek 门控、合法无 CSD 配置、统一 player 与 Android NDK/JNI `video/3gpp` 查询-配置-提交、宿主 mock） | 宿主已落地 / 设备未验 | 复用 `ms_video_*`，加法 `MS_SAMPLE_FMT_H263`。配置来自 ITU-T 短视频头 PSC/PTYPE，**不捏造 csd-0**；MediaCodec 在已知宽高时允许空 CSD。已覆盖基线 I/P 与标准尺寸（SQCIF/QCIF/CIF/4CIF/16CIF）。**未覆盖** PLUSPTYPE/自定义尺寸、Sorenson Spark（`FLV1` 不映射为 H.263）。产品解复用走已开启的 mov/3GP（`s263`）；宿主 fixture 编码器的 mp4 复用器无 H.263 tag，测试不用 MP4 容器。基线常见 PTS==DTS，是诚实现象。中途尺寸变化在 player 手工 QCIF/CIF AU 上验证。自写路径 `ms_codec_query(H263)` 仍为 `NONE`，未开 avi demuxer。只追加 `h263` parser，`CONFIG_H263_DECODER=0`、`CONFIG_DECODERS=0`。fixture 编码不等于产品解码。无硬件须诚实 `UNSUPPORTED`；mock 不是设备硬解或出画 |
| AV1（FFmpeg 已开 mov/MP4 `av01` 到完整压缩 AU + 合法 av1C/帧内 sequence header、PTS、关键帧、seek 门控、中途 SH 变化、统一 player 与 Android NDK/JNI `video/av01` 查询-配置-提交、宿主 mock） | 宿主已落地（有范围）/ 设备未验 | 复用 `ms_video_*`，加法 `MS_SAMPLE_FMT_AV1`。合法 av1C 原样作 csd-0；仅有帧内 SH 时走合法无 CSD（不捏造 av1C）。已覆盖：OBU 步行、temporal/frame unit 计数、Main 无 decoder_model/initial_display_delay 的 SH 宽高、KEY/SWITCH 关键帧；player 手工 64×64→32×32 中途 SH。**不是全部合法 AV1 码流**：带 decoder_model 或 initial_display_delay 的 SH 保守失败；show_existing 不当作新随机接入关键帧；matroska 已开但本轮未把 AV1-in-MKV 当合同；自写 `ms_codec_query(AV1)` 仍为 `NONE`；空间分层/操作点选择/Annex-B 跨包重组等未声称。只追加 `av1` parser，无 dav1d、`CONFIG_AV1_DECODER=0`、`CONFIG_DECODERS=0`。fixture 用宿主 libaom-av1 编码，不等于产品解码。无硬件须诚实 `UNSUPPORTED`；mock 不是设备硬解或出画 |
| AVS+（广电广播类，FFmpeg 名 `cavs`；已开 MPEG-TS 到完整压缩 AU + 真实序列头 CSD/PTS/关键帧、seek 门控、统一 player 与 Android NDK/JNI 查询-配置-提交、宿主 mock） | 宿主已落地 / 设备未验 | 复用 `ms_video_*`，加法 `MS_SAMPLE_FMT_CAVS`，`MSMEDIA_ABI_VERSION` 仍为 1。只把 GY/T 257.1–257.2 **广播类 profile_id `0x48`** 标为 `MS_CODEC_AVS_PLUS`；基准类 `0x20` 保持 `MS_CODEC_AVS_FAMILY` 并返回 `UNSUPPORTED`，不升级。TS `stream_type 0x42` 仍只是族标识，须见广播类序列头才升 PLUS。`AV_CODEC_ID_AVS2`/`AVS3` 映射为 `UNKNOWN`，不得冒充 AVS+。完整 AU 走 `libmsff` mpegts + `cavsvideo` parser；自写 `ms_codec_query(AVS_PLUS)` 仍为 `NONE`，未做自写组帧。 只追加 `--enable-parser=cavsvideo`（LGPL，仅 `cavs_parser.o`），`CONFIG_CAVSVIDEO_PARSER=1`、`CONFIG_CAVS_DECODER=0`、`CONFIG_DECODERS=0`；官方 `cavs` 解码器本身只认 `0x20`，本产品禁止启用，也不得用软件 cavs 冒充。 本箱无 `/ms/build/ffmpeg`、无合法 AVS+ 编码器；fixture 按 GY/T 257.2 表 2 手工打包序列头与起止码，**不是工具编码，不是产品解码**，不用 AVS2/xavs/系统 ffmpeg 充数。 Android AOSP `MediaFormat` 与官方 Supported media formats **无** AVS/CAVS MIME；`ms_hw_video_mime(AVS_PLUS)` 返回 NULL，JNI/NDK 在创建 MediaCodec 之前诚实 `UNSUPPORTED`，不回落到 `video/avc`，不把 OpenHarmony `"video/cavs"` 写成普遍存在的 MediaCodec type。平台设备常无 AVS+ 硬件（特定 SoC 路径不是本 APK 的主张）。无硬件不得软解回退；宿主 mock 可配置/提交，**不是设备硬解或出画**。 |
| VP9 等其余未做视频 | 承诺尚未兑现 | 本轮未把 VP9/E-AC-3 扩为新承诺。不得只加枚举/映射或把 AVS+ 的无硬件 `UNSUPPORTED` 写成完成 |
| 把 `OUTPUT_RENDERED` / `releaseOutputBuffer` / OnFrameRendered 当做出画 | 产品刻意边界 | 文档与统计明确「不是显示」 |

## 3. 音频

| 能力 | 状态 | 说明 |
| --- | --- | --- |
| 可选 AAC 输出状态机、时钟、反压 | 宿主已落地 | mock 不是设备出声 |
| 系统 AAC 有界例外（`allow_system_aac`） | 宿主已落地 / 设备未验 | 库默认硬解 only；测试 APK 显式允许并标明「系统 AAC，非硬件」；HE-AAC 仍拒绝 |
| 同容器多音轨枚举与切换 | 宿主已落地 / 设备未验 | 切轨 flush 旧会话、等新 CSD、重建时钟；HLS 备选 AUDIO 外链见第 1 节（libmsff） |
| 倍速走平台音频能力 | 宿主已落地 / 设备未验 | 见第 5 节；mock 明确 `can_sync_av=0` |
| AC-3（已开 mpegts/mov 到完整压缩帧、合法无 CSD、1536 样点、统一 player 与 Android `audio/ac3` 查询-配置-提交、宿主 mock 时钟） | 宿主已落地 / 设备未验 | 复用公共 `ms_audio_*`（不是每编码平行复制）。合法无 CSD（配置在帧头），不把容器 dac3 或 AAC ASC 当下发 CSD。已测：MPEG-TS cfg=0 au=32 pts=32 inspect=32，MP4 同计数；截断/畸形/旧 ABI；未选中轨过滤与切轨时钟重建；倍速/seek flush；拒绝软件 codec 与系统 AAC 例外扩用。AC-3 切片当时未改 FFmpeg 清单（既有 ac3 parser）；后续 MP3/Opus 切片已重建宿主与双 ABI 前缀，并把 AC-3 编入 23:30 CST 内部 APK，不得再把 22:48 AV1 APK 当作当前实现。`ms_codec_query(AC3)` 仍为 `NONE`。E-AC-3 未做。格式白名单仍不含 raw `.ac3`。无硬件须诚实 `UNSUPPORTED`；mock 不是设备出声。fixture 编码不等于产品软解 |
| MP3（MPEG-TS `mpegaudio` 分帧与已开 MP4，Layer III，合法无 CSD，统一 player 与 Android `audio/mpeg`） | 宿主已落地 / 设备未验 | 追加 `--enable-parser=mpegaudio` 并重建宿主/双 ABI 前缀。已测：MPEG-TS cfg=0 au=40 pts=40 inspect=40 aligned=40 layer=40（一帧一 AU，`frame_bytes==data_size`），MP4 同计数；PTS 递进；seek 后仍分帧；cancel/截断有界；未选中过滤、切轨清时钟、1152@44.1k、倍速/seek；拒绝软件 codec 与系统 AAC 例外扩用；拒绝捏造 ASC；旧 ABI。gcc 14.2 `-O2 -Werror=array-bounds` 已用显式层映射与有界查表收口（PROGRESS §42），默认 `build/release` 未覆盖。`ms_codec_query(MP3)` 仍为 `NONE`。无硬件须诚实 `UNSUPPORTED`；mock 不是设备出声。fixture 编码不等于产品软解 |
| MP2（MPEG-TS `mpegaudio` 分帧与已开 MP4，Layer II，合法无 CSD，Android `audio/mpeg-L2`） | 宿主已落地 / 设备未验 | 与 MP3 共用 parser 与 `ms_audio_*`，层不可混淆。查表边界与 MP3 同一修复。已测：MPEG-TS cfg=0 au=42 pts=42 inspect=42 aligned=42 layer=42，MP4 同路径；1152@48k；切轨/seek/旧 ABI；拒绝软件 codec。`ms_codec_query(MP2)` 仍为 `NONE`。无硬件须诚实 `UNSUPPORTED`。fixture 编码不等于产品软解 |
| Opus（已开 mov/MP4 与最小 Ogg demux；真实 OpusHead/dOps；packet framing；pre-skip / seek preroll / 尾部 discard padding 分项） | 宿主已落地 / 设备未验 | 解析/demux 清单与许可限制同前（`CONFIG_OPUS_DECODER=0`、`CONFIG_DECODERS=0`）。matroska/webm 已开；WebM+Opus 宿主已测，不宣传 VP9/Vorbis。已消费 `AV_PKT_DATA_SKIP_SAMPLES`（起始/尾部）并加法下发到 `ms_event.skip_samples_*` / `ms_hw_audio_submit_au`，不借用 dts。三者不得混用：**(1) 流开始 pre-skip / codec delay**（OpusHead/dOps，默认 312；Ogg 第一包 SKIP start，FFmpeg 常把首包 PTS 置为负，由会话 origin 吸收，相对 ev.pts 常为 0）；**(2) 中途 seek 的解码预热**（RFC 7845 默认 80 ms = 3840 @ 48 kHz；`codecpar->seek_preroll`；FFmpeg 中途 seek **不**再注入 312 的 SKIP start，本层 `note_seek(>0)` 武装 warmup）；**(3) 负 PTS / 平台 codec delay 已在时间戳域记账，不得再平移扬声器时钟**。seek(0)/切轨重新 configure 可再扣 pre-skip；flush 单独不恢复已消耗的 pre-skip；中途 seek 扣 3840 而不是再扣 312。Android：只设官方 csd-0（OpusHead）与 csd-1/csd-2（原生序 uint64 纳秒），有 csd-1 时**不再**设 `encoder-delay`；`platform_trims_delay=1`，本层不对同一段 PCM 再扣 start/warmup，且不以压缩 AU 样点数当作平台已修剪后的输出长度（避免双扣）；仍在解码后、写入 AudioTrack 之前按真实输出样点/PTS 扣包尾 padding。公共 `ms_hw_pcm_trim`（有界 meta 32、hold 8192，尾部保留/EOS 排空/flush 丢弃）由 Android 与宿主 mock 真正复用，不按一块输出 pop 一条输入标记。已知 1:1 FIFO 在 1 AU→多块 PCM、多 AU→一块 PCM 上会错扣，已用递增样点反例关闭。默认宿主 mock `platform_trims_delay=0`，在 mock PCM 计数上应用全部修剪；亦可经调试钩子打开 `platform_trims_delay=1`，由 mock 丢掉 leading pre-skip/warmup 以模拟平台已修剪，再走同一修剪机：expected 为压缩样点减去平台 leading，不双扣 start，缺 PTS 或时间戳相同时仍扣 skip_end（不再把 AU 边界只绑在互异 PTS 上）。**都不是听感通过**。已测（受控 fixture，独立按 TOC/SKIP 计量）：1s MP4/Ogg audible=48000（51×960−312−648），首包 skip_start=312、末包 end=648，signed 首 PTS 为负且不双扣；50ms Ogg/MP4 audible=2400（end=168）；25ms Ogg audible=1201（end=407，相对 lavfi 1200 偏 1，按实际 side data，不编造补偿）；25ms MP4 本树 FFmpeg SKIP end=1、audible=1607，**不作为 1200 合同**（仅在无 side data 且 duration&lt;TOC 时用 duration 补偿末包，覆盖 1s/50ms MP4）。MP4 中途 seek 首包 skip_start=0（不在 priming 区）。Ogg 中途 seek 受页粒度限制，不得宣称任意点精确。0/多包 start skip、溢出 skip 不绕回、背压/部分写、切轨、旧 ABI 加法兼容已覆盖。提交/排队压缩样点不得冒充扬声器时钟（Android 仍用 playback head）。`ms_codec_query(OPUS)` 仍为 `NONE`。无硬件须诚实 `UNSUPPORTED`；mock 不是设备出声。fixture 编码不等于产品软解。不是全部 Opus 码流 |

## 4. 字幕

| 能力 | 状态 | 说明 |
| --- | --- | --- |
| SRT / WebVTT / tx3g 文本解析、时间线、JNI 叠加 | 宿主已落地 / 设备未验 | 有界解析；测试 APK 有 TextView，未上屏验收 |
| ASS / SSA 文本：有界解析与布局模型 | 宿主已落地 / 设备未验 | 复用 `ms_sub` 统一时间轴/轨道/seek/倍速，加法 ABI。外挂整文件与容器内嵌事件（header 在 extradata/CONFIG，Dialogue 或 Matroska 压缩包）进入同一 cue。已测：Format/Style/Dialogue、转义/换行、颜色/字号/粗斜体/对齐/边距、`\pos`、重叠 layer。Android TextView 只应用去标签文本与已解析样式，不再把覆盖标签当正文。未支持特效（`\move`/`\t`/`\fad`/karaoke/绘图/clip/Effect 栏等）写入 `cap_flags` 并标 `DEGRADED`。**不是**完整 ASS 渲染（无字体栅格、无动画/描边合成）。2026-11-22 00:59 CST 起，受控 MKV 内嵌 ASS/SSA 事件已能经 matroska demux 走同一 `ms_sub` 路径（去标签文本与布局模型）；**不得**把先前「事件解析已测」写成当时已能 demux MKV，也不得把本路径称为完整 ASS 渲染。JNI/Canvas 仅为编译+逻辑，**不是视觉验收** |
| 完整 ASS 渲染（可选隔离 `libmsass`） | 宿主已落地（有范围）/ 设备未验 | 许可与体积核实后采用钉选 libass 0.17.3（ISC）+ FreeType 2.13.3（选 FTL，不选 GPLv2）+ HarfBuzz 10.2.0（Old MIT）+ FriBidi 1.0.16（LGPL-2.1+，仅隔离模块，处理同可选 libmsff）。不链入 `libmsmedia`/`libmsff`，不改变未选定的顶层许可证，产品 FFmpeg 仍 `CONFIG_LIBASS=0`/`CONFIG_DECODERS=0`。轻量去标签路径保留。完整渲染按显式能力：喂原始 header/events（不经截断去标签），按媒体时间栅格到内部 RGBA，所有权在 renderer，seek/flush/切轨清理，附件字体默认不加载。字体只用宿主已授权系统字体与 Android `/system/fonts`，不下载、不打包不明字体。宿主 `test_ass_render` 在多时间点对可追溯 fixture 取真实像素（文字栅格/描边红字白边/移动质心/淡入 alpha/karaoke 左右色/clip 外无墨/分层上白盖红/中文+Latin），不是只断言非空。**不承诺 100% 全部 ASS**（复杂绘图、部分 3D/某些附件字体/脚本特效等未声称）。Android 测试 APK 在可选 `libmsass.so` 存在时 dlopen 并走既有 ImageView RGBA 叠加，**不读回视频 Surface**；宿主栅格不是设备视觉验收。`ENABLE_ASS_RENDER=0` 可关闭。体积须把可选模块与默认核心分开报告 |
| DVB 位图字幕（ETSI EN 300 743 段、完整 cue、叠加对齐） | 宿主已落地 / 设备未验 | 复用 `ms_sub` 统一时间轴/轨道/seek/flush/倍速，加法 ABI（`MS_CODEC_SUB_DVB`、`MS_SUB_FORMAT_DVB`、cue 位图字段），旧 `struct_size` 停在 spans 前不写新字段。完整 cue：起止时间、位置、可选裁剪/画布、有界 RGBA8888 所有权与释放。自写有状态组装（完整 PES、跨多 TS 包 PES、多 PES 及段中部分片；续 PES 的 `0x20 0x00` 前缀剥除），**未开** `dvbsub` 解码器。缺省 CLUT 仅作 DVB 回退。已测：16×16 手工 fixture 经 libmsff demux 到 RGBA（尺寸/位置/像素/时间），多 TS 包单 PES、按头部/载荷断点拆成多 PES，缺中片/截断/seek/flush 有界不串帧不泄漏；**不以 4×4 单包代替 16×16 多包需求**。超限 OVERFLOW 且保留已有条目，不把旧画面当新显示集。Android ImageView 叠加，**不把 RGBA 当文字**。JNI/编译不是真机视觉验收 |
| PGS 位图字幕（Blu-ray，完整 cue、叠加对齐） | 宿主已落地 / 设备未验 | 同上统一 API。加法 `MS_CODEC_SUB_PGS`。支持 raw type+len 与 `.sup`（`PG`+PTS/DTS）。Epoch start 清对象。缺 PDS 不臆造颜色。ODS first/continuation/last 先收齐有界 RLE 再解码；一段一个 PES 或段中部分片走同一 acc。已测：16×16 经 libmsff demux 到 RGBA（位置/画布/像素/时间），每段一 PES、ODS 两段/三段续包、原始字节流头部/载荷断点拆 PES、seek/flush。未开 `pgssub` 解码器。设备叠加未验 |
| DVD VobSub（idx+sub / `MSVB` 组合块） | 宿主已落地（有范围）/ 设备未验 | 体积允许已补。须 idx `palette:`，不臆造调色板；裸内嵌 SPU/`MS_CODEC_SUB_VOBSUB` 无 idx 返回 `UNSUPPORTED`。已测：手工 8×2 SPU、AUTO `MSVB`、无调色板拒绝。未开 `dvdsub` 解码器。未声称任意损坏 idx 或全部 DVD 菜单/高亮 |
| 通用位图族 / XSUB / DVB 图文 | 承诺尚未兑现 | 仍可枚举为 `MS_CODEC_SUB_BITMAP`；选择播放保持 `UNSUPPORTED`。不得因 DVB/PGS 已做就写成全部位图完成 |
| P5 AI 字幕（可选插件框架） | 宿主框架已落地 / 真实供应商未接、设备未验 | 可选隔离 `libmsai`，默认禁用，不链入 `libmsmedia`/`libmsff`，默认快启动路径无模型、无网络、无供应商地址。已解码 PCM 在输出修剪后经有界异步 tap 送插件（不阻塞解码/AudioTrack/UI，无额外软解）。公共可验证转单声/线性重采样。partial/final/翻译归并到 `ms_sub` 生成轨 `MS_TRACK_ID_AI_SUB`，不自动选中、不覆盖原字幕。seek/flush/切音轨按 epoch 丢弃迟到结果。禁用/未配置不采集、不传输、无工作线程。源音频/文本不写日志。内置仅受控本地假供应商（明确模拟，不是真实识别）。官方 `test_ai_sub` 走真实音频修剪链路（内容/时间戳、修订/final/翻译、乱序/迟到/取消/错误/超长/队列上限/epoch、pause/倍速/禁用不抓）。不得把手工塞 cue 或 mock 写成真实 AI 识别完成。JNI/测试 APK 有配置启停入口，状态只允许写 simulated/disabled，不得伪报已用某商业 AI。供应商尚未指定，实接与凭据/真机验收仍待用户后续决定 |

## 5. 倍速、暂停与相对/绝对 seek（本轮宿主已落地）

| 能力 | 状态 | 说明 |
| --- | --- | --- |
| 诚实速率能力（`ms_hw_rate_caps`） | 宿主已落地 | 宿主 mock：记账范围 1/8–8×，`can_sync_av=0`，`mock_is_not_device=1`。Android API 23+：`PlaybackParams` 保音调，宣称 1/4–2×。API 21–22：`setPlaybackRate` 变调，约 1/2–2×。超范围或平台拒绝 → `UNSUPPORTED`，**不得只改视频时钟却称为音画同步倍速**。视频-only 且非 mock：非 1× 拒绝 |
| pause / resume 与时钟连续性 | 宿主已落地 | 暂停冻结音频时钟与呈现；改速率不清零时钟；seek/flush 保留速率；切音轨后重套用 |
| 绝对 / 相对 seek（快进快退） | 宿主已落地 | 相对 seek 检查溢出、负值钳 0；VOD 按时长钳位；HLS EVENT/滑动窗口按 `note_seek_range` 的 start/live_edge 钳位。真正不可定位源 `seekable=0` 为 `NOT_SEEKABLE`；负绝对时间为 `INVALID_ARG`。seek 后复用既有 flush、音频 `note_seek`/trim、清空在途字幕、等关键帧、保留 CSD、`timeline_epoch++`，保留用户音轨/字幕/视频变体选择 |
| `ms_ff_seek` / duration / is_seekable / range | 宿主已落地 | `av_seek_frame` 关键帧向前；不启用解码器；时间轴相对会话 origin。加法 `ms_ff_get_seek_range` / `ms_ff_seek_live_edge`（`struct_size`）。HLS EVENT/滑动为可定位，`duration_90k=-1`；VOD 仍公布时长。直播边缘 = 窗口末端减一个 target duration |
| JNI / 测试 Activity | 代码已接线 / 设备未验 | 绝对/相对/回到直播边缘均异步入队，不阻塞 UI；相对定位按窗口而非 VOD 时长钳位；PlayPath 在链入 libmsff 时走 `file:`；Pause/Resume/0.5x/1x/1.5x/2x/−10s/+10s/Live |
| 倒放 / 负速率 | 产品刻意边界 | `UNSUPPORTED`（或 `INVALID_ARG` 当 den/num ≤ 0） |
| 默认构建音频 time-stretch 外挂 | 产品刻意边界（当前） | 未实现；不把「仅视频时钟」当同步倍速 |
| HLS EVENT/滑动窗口定位（时移边界） | 宿主已落地 / 设备未验 | 复用官方 `hls_read_seek`，不平行重写清单。加法查询 start/end/live_edge/safety；绝对/相对定位；`seek_live_edge` 留一个目标时长的安全距离，不跳未完成末片。窗口滑动后过期目标钳位到新 start 并返回 OK。非零 MEDIA-SEQUENCE 与会话 origin 同轴；EVENT 追加（含 DISCONTINUITY）起点保持、终点可增长。不得把窗口长度写成 VOD `duration`。UDP/RTP/RTMP/RTSP 与 DASH 直播（FFmpeg 9.1 `dash_read_seek` 对 `is_live` 返回 `ENOSYS`）诚实 `NOT_SEEKABLE`。不另建 DVR、不无限本地缓存。最小补丁 `patches/ffmpeg-9.1-hls-live-window.patch` 公布窗口元数据，幂等应用，不关字幕 ctx、不改 finished duration 语义。`CONFIG_DECODERS=0`。宿主证据：本地动态 HTTP 真实 demux EVENT/滑动（AU/CSD/PTS），不是出画 |

## 6. 平台与发布

| 能力 | 状态 | 说明 |
| --- | --- | --- |
| Bot 宿主 gcc 14.2 ASan/UBSan 自测 | 宿主已落地 | 非正式验收；含 `test_abi_old_header`（1838 旧头动态链当前 .so，ASan 金丝雀） |
| 公共 C ABI（`MSMEDIA_1.0`，版本仍为 1） | 宿主已落地（本轮修复） | 已交付 1838 的 `ms_event` 无 `struct_size`，不得靠尾部追加宣称兼容。本轮恢复 21 字段 / 本机 LP64 176 字节布局；PCM 修剪改走加法 `ms_event_trim` + `ms_session_event_trim` / `ms_ff_event_trim`（须在 release 前查询）与 `ms_hw_player_note_event_trim`（不改 `feed` 签名）。未删旧符号、未抬版本。带 `struct_size` 的 stats/opts/audio_au 等经旧 size 金丝雀；`ms_sub_cue` / `list_tracks` 为 1838 之后加法 API，当前头数组步进已测。未发现中间插字段或跨库所有权回归。 |
| Android 播放生命周期（Surface 丢失重建 / 音频焦点） | 宿主已落地 / 设备未验 | `set_surface(NULL)` 只拆视频、保留音频；重建不重开 Intent 会话；`notify_audio_focus` 与用户暂停分离（CAN_DUCK 按暂停，无音量闪避）。宿主 `test_surface_loss_keeps_audio_and_focus` 与 `test_csd_change_and_seek_while_unbound_or_paused` 已过。不是真机前后台/焦点/出画验收 |
| 非正式 Android 双 ABI 测试 APK | 构建已出 / 设备未验 | 未安装、未启动共享 AVD |
| Linux VAAPI 码流 profile 映射与 DRM 探测 | 源码+宿主编译/测试已落地；设备出画未做 | `docs/HW_LINUX.md`。本机无 `/dev/dri`，query/configure 诚实非硬解；slice 提交未实现，`is_hardware` 恒 0。不是出画验收 |
| Windows Media Foundation / D3D11 | 仅有设计约定 | 本机无 mingw / Windows SDK，本轮未写未编译代码 |
| iOS VideoToolbox | 仅有设计约定 | 本机无 Apple SDK，本轮未写未编译代码，不伪造签名或测试证明 |
| el7 / 四平台正式构建与设备硬解验收 | 承诺尚未兑现 | 无 docker/podman/可用 el7 镜像；adb 空，用户电脑未连接。`/dev/kvm` 节点存在但当前用户无权限，且不得启动共享 AVD |
| 阶段公开交付包 | 本轮不做 | 1639–1838 历史归档保持不动；用户要最终统一交付 |

下一可执行事项（不因本表自动扩权）：Linux VAAPI 码流映射与 DRM 探测已于 2026-11-22 05:17 CST 官方矩阵通过（`docs/HW_LINUX.md`），**不是出画或四平台验收**。有 GPU 的 Linux 机器上打开设备并完成 slice 提交/呈现仍缺外部条件。P5 可选插件框架与受控假供应商已于 2026-11-22 04:50 CST 官方矩阵通过，**不是真实识别完成，供应商实接未验**（不能自选供应商/上传私有音频）。完整 ASS 宿主栅格已落地，**设备叠加与时间对齐仍未验**。真机出画/出声/时移仍未验（禁止动共享 AVD）。承诺尚未兑现且不得把 `UNSUPPORTED` 写成完成：VAAPI/VideoToolbox/MF 设备硬解、XSUB/图文等其余位图、DASH 直播时移、el7。VP9 为既有欠账、本轮未扩为新承诺。不得把 VP8/Vorbis 或未测的 mk3d/mka/mks 写成已验收。
