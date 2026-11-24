# 未验证与明确不支持清单

by: Long <mycjzb@gmail.com>

日期：2026-11-22 16:23 CST（Asia/Shanghai）。表头仍保留 2026-11-21 起的宿主证据。本文件随本阶段源码交付包提供，**不是项目完成声明**。宿主编译与 mock 测试不等于 el7 / 四平台 / 旧 Android / 真机验收。

## 本阶段已在 Bot 宿主验证（仍非正式发版）

- 前缀扫描；FFmpeg file/HTTP PNG+TS；HLS 内部分片剥离（ops=1, 457 字节, kind=png）。
- `test_ff_backend` 在既有本地 CA/样本下全矩阵（TLS 1.2/1.3、hostname mismatch、取消、DASH、UDP/RTP 帧合同、RTSP TCP/UDP、RTMP，以及 18:36 CST 起的受控 RTMPS）。ASan/UBSan，exit 0。
- 无信任锚时 https 打开返回 `UNSUPPORTED`（不关验证）。
- `test_core`、`test_ts_scan`、`test_hw_backend`、`test_hw_player`（ASan/UBSan）通过。未把 SKIP 写通过。
- 帧守恒合同：对已消费视频 AU，`video_au_in == submitted + dropped_nokey + dropped_preroll`。`video_au_backpressure` 计 LIMIT 次数，事件未消费，不计入 `in`。禁止为凑齐计数提交不可解非关键帧。
- `short.ts` / `short.flv`：25 个视频 AU，Constrained Baseline，25 fps，**仅 index 0 为关键帧**，随后 24 个非关键帧。file 从头播放提交 25/25，`drop_nokey=0`。
- 已知 GOP=5 人造流：从头 30/30；中途跳过 2 帧后 `in=28 submitted=25 drop_nokey=3`。`short.ts` 两遍拼接后跳过前 10 帧：`in=40 submitted=25 drop_nokey=15`（先丢第一遍剩余 15 个非关键帧，再在第二遍 IDR 提交 25）。
- 直播环回本轮实测（2026-11-21 16:52 CST，CLOCK_MONOTONIC 相对毫秒，**output 不是出画**，`display=0`，`codec_rendered` 未置）：
  - 先启动发送再打开（中途入 GOP，合法门闩，不是静默丢帧，本跑 `preroll=0`、`backpressure=0`）：UDP `in=25 submitted=16 drop_nokey=9`，first_packet=0.048ms、decodable=0.284ms、submit=0.297ms、output_not_display=0.308ms。RTP `in=25 submitted=13 drop_nokey=12`，first_packet=0.113ms、decodable=0.254ms、submit=0.274ms、output_not_display=0.282ms。
  - 先 listen 再延迟启动发送（对准 `short.ts` 唯一 IDR）：UDP-from-start `in=25 submitted=25 drop_nokey=0`，first_packet=0.056ms、decodable=0.124ms、submit=0.142ms、output_not_display=0.156ms。证明此前 25/15、25/13 缺口来自中途入 GOP，而不是必须提交不可解帧才能凑 25。
- file 入口（同一次回归）：TS first_packet=0.031ms、decodable=0.369ms、submit=0.379ms、output_not_display=0.390ms，`in=25 submitted=25`。FLV 0.049 / 0.101 / 0.112 / 0.121ms，同样 25/25。以上都是宿主 mock submit，**不是设备出画**。
- 已修缺陷（宿主验证）：LIMIT 不再提前计入 `in`；未消费事件不得当作已提交而释放；EOS 后再 drive 幂等，不再把 `last_error` 写成 STATE；未 start 前的 drive 为空操作，不再污染 `last_error`。JNI：`nativeOpenUrl` 不再在 UI 线程 join 上一会话；URL 排队 + 取消进行中的 I/O；`op` 锁串行化 set_surface/flush/feed/drive/cancel（cancel 的 I/O 侧可不拿锁）；LIMIT/AGAIN 未消费不释放；`nativeDestroy` 后清空指针。宿主用同一锁协议做 flush/surface/feed 并发 mock（`test_op_lock_flush_surface_feed`）。**这些不是设备生命周期验收。**
- 2026-11-21 17:03 CST 计时语义修正：`t_first_*` 现明确为相对 `t_start`（create/最近 flush）的**管线局部**计时，不得称为直播端到端延迟。新增 `ms_hw_player_mark_request`（须在 open/probe/读文件之前），以及 `t_req_first_*` 请求端到端计时，包含建连、探流与等待关键帧。display 仍无证据，保持未发生。先前公布的约 0.28ms 级数字是播放器已创建且 open/probe 之后的局部计时，**不含完整等待**。
- 受控注入（宿主 mock，非出画）：mark 前等待 150ms + 非关键帧后等待 300ms 再喂 IDR，实测 `req_packet=0.014ms`、`req_decodable=300.154ms`、`req_submit=300.192ms`，`stage_local decodable=450.270ms`（含 mark 前等待）。flush 保留请求原点、重置局部计时。
- 先 listen、mark 后再延迟 250ms 启动发送的 UDP-from-start：守恒 25/25/0；`request_e2e packet=5060.811ms`、`decodable=5060.968ms`（≥ 注入的 250ms；亦含 listen/open/probe，不得把该值说成只有 250ms，更不得用预缓冲或极速回放声称直播低延迟）。display=0。
- 中途入 GOP 直播在 **mark+open 之前** 计时后：UDP `request_e2e packet=4047.930ms decodable=4048.212ms`（in=25/16/9）；RTP `4015.495 / 4015.652ms`（25/13/12）。此前约 0.28ms 是 open 之后才建播放器的局部数字。
- `nativePlayPath` 已与 `nativeOpenUrl` 共用同一异步队列/取消/工作线程，不再在 UI 线程同步读整文件；同一时刻只有一个泵。Demo/Drive/Flush/Cancel/Surface 不 join、不等待网络 open。`mark_request` 在 Java 点击/`queue_session` 入队时调用；工作线程 `begin_session` 调用 `mark_worker_start`。排队与旧会话取消计入请求端到端。
- 2026-11-21 17:13 CST 拆分表明 4~5 秒主要来自 DEFAULT `find_stream_info`（UDP 中途入 GOP：open_input 236.360ms，find_info **3815.432ms**，request_e2e submit 4052.227ms）。FAST_START 同条件：find_info **585.341ms**，submit 863.851ms（该路在 DEFAULT 之后入流，守恒 19/9/10，合法门闩）。RTP-FAST 中途：find_info 532.424ms，submit 533.325ms（25/13/12）。UDP-FAST from-start（注入 250ms）：open_input 306.385ms，find_info 99.950ms，request_e2e **408.245 / 408.316 / 408.332ms**（packet ≥200ms，含注入等待；对照 17:02 默认探流 5060.811ms）。本机 HTTP TS/FLV FAST（VOD 速率，非 `-re`）submit 2.267 / 2.427ms，25/25/0。display=0，**不是 1 秒出画验收**。
- 库默认仍为 `MS_FF_PROBE_DEFAULT`；FAST 有界值非 0、不关音轨（400ms / 512KiB / 48 packets / 500ms 探测 deadline）。`test_ff_backend` 保持 DEFAULT 且全矩阵通过。AndroidCAStore 首次导出改到后台线程并复用 PEM 缓存。
- 2026-11-21 17:40 CST 稳定性边界（宿主 ASan/UBSan，非正式验收，**非出画**）：
  - FAST deadline 软 interrupt（EXIT/ETIMEDOUT/EINTR）可恢复并继续读，置 `probe_incomplete=1`；上下文缺失或探测失败且硬错误则返回可诊断错误并保留计时，不当成 OK。探测已识别流后 pb 出现 EIO（短 RTMP 发布结束）保持打开。UDP-FAST mid-GOP：25/17/8，find_info=585.068ms，submit=771.741ms，`probe_incomplete=1`。DEFAULT mid-GOP：25/15/10，find_info=3757.631ms，submit=4020.069ms。
  - 视频 CSD 变化按 epoch 重新 configure 并等关键帧；相同 CSD 不升 epoch；configure 失败为可诊断错误。FAST 文件仍发音频 TRACK（`video_au=25`）。**音频输出与 A/V 同步未实现**，不得宣称完整有声播放器。公共事件仍无 sample_rate/width/height。中途 AU 内 SPS 若无 `CODEC_CONFIG` 在当时仍未做（已在 18:19 CST 由统一播放器入口补齐，见上条）。
  - 探测期取消：`CANCELED` 101.433ms。不可达 UDP listen：`TIMEOUT` 805.140ms，有界不挂死。真实 2s GOP mid-join：request_e2e submit=2791.105ms（≥1000ms；`-re` 发送，不是人工喂 AU）。
  - `nativeDestroy`：取消 I/O 后最多等 2 秒；完成则 join 释放，否则 detach，由工作线程自释。禁止超时后 free，禁止 UI 无界 join。若 I/O 无法中断，放弃的线程可能活到进程退出（已记录，未真机验证）。
- 2026-11-21 18:02 CST AAC 输出/同步（宿主 mock，非正式验收，**非设备出声**）：
  - 可选 `ms_hw_audio` + 公共 `ms_aac_asc_parse`。默认拒绝软件 AAC；仅系统软解时诊断 `UNSUPPORTED`，`audio_is_hardware=0`，`audio_system_decode_is_not_hardware=1`，视频 fallback。mock 即使用 `MS_HW_MOCK_AS_HW` 跑生命周期，`is_hardware` 仍为 0。
  - 时钟来自已播放采样，不用排队字节。视频：快速首帧；early +80ms 保持；late -200ms 计数并呈现；无音频时钟立即呈现。无 ASC 的 AAC AU 丢弃，不卡住复用泵。
  - 放弃工作线程帽=2；满帽时 `nativeCreate`/`ensure_worker` 拒绝新建。`test_abandoned_worker_cap` 通过。detach 不能单独保证不增长。
  - 本轮长 GOP mid-join request_e2e submit=2782.678ms；UDP-FAST mid-GOP 25/17/8 submit=713.762ms；`test_ff_backend` DEFAULT 全过（RTMP events=36）。display=0。
- 2026-11-21 18:19 CST 中途 AU 内 SPS/PPS（无独立 `CODEC_CONFIG`）与音频时钟复核（宿主 mock，非正式验收，**非设备硬解/出画/出声**）：
  - 公共有界 `ms_h264_ps_parse`/`equal`/`pack`；统一在 `ms_hw_player` 处理带内变化。禁止新 SPS+旧 PPS 拼接；不成对则有界等待 16 个 AU 后放弃 pending；相同 CSD 不升 epoch。官方 CONFIG 仍按原始字节比较。未在 `libmsff` 复制路径。
  - 合成 Annex B/AVCC：初始化、中途变化、相同 CSD、有界放弃、背压不丢配置。真实受控拼接 TS（跳过 CONFIG）：`in=48 submitted=48 inband=2 same=0 epoch=2`；同流 FLV：`in=50 submitted=50 inband=2 same=0 epoch=2`。
  - 音频：设备时钟按 `getPlaybackHeadPosition` 无符号 32 位并处理回绕；`write` 单位为 PCM 字节，部分写入进 leftover；flush 先 pause 再归零。无硬件 AAC 拒绝语义不变。**未在真机观察播放头或实听。**
  - 本轮 FAST 长 GOP mid-join request_e2e submit=2807.005ms；UDP-FAST mid-GOP 25/17/8 submit=713.118ms；UDP-DEFAULT mid-GOP 25/16/9 submit=4050.261ms；`test_ff_backend` DEFAULT 全过（RTMP events=36）。display=0。
- 2026-11-21 18:36 CST 系统 AAC 有界例外、非正式 arm64-v8a 与受控 RTMPS（宿主 mock，非正式验收，**非设备硬解/出画/出声**）：
  - 明确枚举 `ms_hw_aac_policy` 与 `ms_hw_player_opts.allow_system_aac`。库默认仍为硬解 only（0）。测试 APK 显式 `allow_system_aac=1` 且 `allow_software=0`，界面/状态写明「系统 AAC，非硬件」。`audio_is_hardware` 仅真正硬件为 1；系统软解保留 `audio_system_decode_is_not_hardware=1`。视频软解、非系统第三方软解、HE-AAC、FFmpeg 软解（`CONFIG_DECODERS=0`）仍拒绝。不是把 `allow_software` 变成全面软解。
  - 非正式 v8a size-opt 前缀重链：打入 APK 的 `libmsff.so` v7a 仍为 4,244,104、v8a 为 5,250,888；双 ABI APK 9,921,347 字节，SHA-256 `0963764b63ad651f216873fe6fee7ea9b9a59a540bee53fd14ca65c0dd853ce9`。未覆盖历史 `build/android-v7a/size`。NEEDED 无系统 libssl/libcrypto，无 libmsmedia。
  - 受控 RTMPS（本机 TLS 转发 + `ffmpeg -listen` + 既有本地 CA，校验与主机名检查保持开）：无 CA 为 `UNSUPPORTED`；成功拉流 events=36（与明文 RTMP 同量）；主机名不匹配为 `UNKNOWN`（非 OK）。RTMPT 与公网/设备 RTMPS 未测。缺依赖会记缺口，不把 SKIP 写通过。本轮依赖齐全。
  - 本轮 FAST 长 GOP mid-join request_e2e submit=2794.318ms；UDP-FAST mid-GOP 25/17/8 submit=723.643ms；UDP-DEFAULT mid-GOP 25/16/9 submit=4047.610ms；`test_system_aac_policy` 通过。display=0。
- 2026-11-21 20:10 CST P4 音轨/文本字幕切片（宿主事件路径，非正式验收，**非设备出声/上屏字幕**）：
  - 公共加法 ABI（`MSMEDIA_ABI_VERSION` / `MSMEDIA_HW_ABI` 仍为 1）：`ms_sub_*` 有界解析与时间轴；`ms_session_*` / `ms_ff_*` / `ms_hw_player_*` 的 list/select/external/cues_at。未知 id 为 `INVALID_ARG`（不再是占位 `UNSUPPORTED`）。
  - 文本：外挂 SRT/WebVTT；tx3g/mov_text 纯文本；位图/ASS 可枚举、打开播放 `UNSUPPORTED`。未开 FFmpeg 解码器。同容器切轨不重开 Source；HLS 备选 rendition 仍 `UNSUPPORTED`。
  - 宿主测试：`test_p4_tracks_and_subs`（边界/重叠/关闭/flush/畸形/上限/旧 `struct_size`、双音轨 TS 切换 AU/PTS/epoch）；`test_p4_audio_switch_and_subs`（切轨前后配置/时钟、未选中丢弃、无音轨回退、字幕）；`test_p4_ff_tracks`（系统 ffmpeg 生成双 AAC MP4：`a0=1 a1=2`，切换后仅新轨 AU）。连带既有 ASan/UBSan 矩阵 exit 0。不得把这些写成扬声器或画面验收。
  - 测试 APK 已接入 JNI 与叠加 TextView / Tracks、Next Audio、Next Sub、Load SRT、Sub Off。内部工作副本 `build/android-hw/MsHwPlayer.apk` 9,990,979 字节（双 ABI，已签名未安装），SHA-256 `24f5a101bacd27e836dc7e122f88eecff7b94b18932581a6bb7e01b2787eeea3`。因静态编入字幕解析，打入的 v7a/v8a `libmsff.so` 现为 4,257,688 / 5,261,344。本轮**不**另存公开日期归档。不覆盖 1639–1838、`build/release` 与历史 v7a size-opt（4,235,076）。
- 非正式 NDK r29 API 21 armeabi-v7a 交叉编译并签名新的 `MsHwPlayer.apk`（2026-11-21 16:54 CST，4,424,302 字节，SHA-256 `d606e5bab1b78139c05e9c28a9a8257ae50f5ee81c5e86ad36389dbf4f2434ca`；`libmshw.so` 39,720，`libmsff.so` 4,241,096，NEEDED 无系统 libssl）。另存 `/workspace/MsMedia-deliver-20261121-1654/`，**不覆盖** `MsMedia-deliver-20261121-1639/`（旧包 SHA-256 `d4589467bbad1fbea0727d8c3142deacf968433e6c8247bfd473ad35382c93b4`）。**未安装、未运行。**
- 设备或 AVD 上的多音轨切换实听、字幕叠加是否真正画在画面上、外挂字幕文件选择器在真机上的行为：未验证。宿主 cues_at / TextView 赋值不是出画验收。
- 2026-11-21 20:25 CST P4 倍速/相对 seek（宿主 ASan/UBSan，非正式验收，**不是设备音画同步倍速或出画**）：
  - `test_p4_rate_and_seek`：能力诚实（mock `can_sync_av=0`，`mock_is_not_device=1`）、超范围/`0`/负速率拒绝、pause 冻结时钟、改速不清零、flush/seek 保留速率、相对 seek 负值/时长/溢出钳位、直播 `NOT_SEEKABLE`、seek 后等关键帧并清在途字幕、切音轨后速率仍在、旧 `struct_size` 兼容。
  - `test_p4_ff_seek`：可定位 `file:` MP4 的 duration/`is_seekable`、`av_seek_frame` 后仍出 AU/关键帧/CSD，解码器仍关闭。
  - 非正式双 ABI 测试 APK 已重建（未安装）。完整矩阵见 `docs/FEATURE_MATRIX.md`。

- 2026-11-21 21:49 CST P4 ASS/SSA 文本解析与布局模型（宿主 ASan/UBSan，非正式验收，**不是完整 ASS 渲染，不是设备出画/视觉验收**）：
  - `test_ass_ssa_parse`：外挂整文件 AUTO 识别、Style/Dialogue、颜色/字号/粗斜体/对齐/边距、`\N`、重叠 layer 排序、`\pos`+PlayRes、内嵌压缩事件、`clear_cues` 保留头、SSA 对齐映射、`[Fonts]` 忽略且不执行、畸形时间跳过、超长字段有界、非法 UTF-8 可控、`\move`/Effect/karaoke 的 `cap_flags`、旧 `struct_size` 前缀兼容。不去标签就宣称完整渲染。
  - `test_p4_ass_inband`：CODEC_CONFIG 头 + 压缩事件进入播放器 cue，ASS 可选中，位图仍 `UNSUPPORTED`，AUTO 外挂 ASS，seek 后头保留。连带 SRT/音轨/倍速/seek/HLS/HEVC 回归。
  - 非正式双 ABI 测试 APK 已重建（未安装）。JNI/TextView 仅为编译+逻辑接入。完整矩阵见 `docs/FEATURE_MATRIX.md`。

- 2026-11-22 00:44 CST P4 位图字幕 DVB/PGS/VobSub（宿主 ASan/UBSan，非正式验收，**不是真机视觉/叠加验收**）：
  - `test_sub_bitmap_parse` / `test_p4_bitmap_inband` / `test_ff_bitmap_subs`：完整 RGBA cue、时间轴、seek/flush/关闭清理、资源上限、旧 ABI；DVB/PGS 可选中；通用 `SUB_BITMAP` 仍 `UNSUPPORTED`；不把二进制当文字。VobSub 须 idx 调色板。
  - 非正式双 ABI APK 已重建（未安装）：10,724,163 字节，SHA-256 `360b18b8889bdbcb741823fbdf03ad1c7dce094d55a59d219ee694087cb9eaf2`。不得把 00:10 CST AVS+ 包当作当前实现。完整矩阵见 `docs/FEATURE_MATRIX.md`。

## 未验证（不得写成已通过）

- 真实 Android 设备或隔离临时 AVD 上的硬件解码。
- Surface 实际出画、用户可见首帧、首帧 <1 秒。不得把 mock `first_output` 写成出画。
- `releaseOutputBuffer`、`OnFrameRendered`（API 33+）、Choreographer 均不得当作已上屏。
- 软件模拟器上的解码（即使可建临时 AVD，也不得报硬解）。
- APK 内 HTTP(S)/UDP/RTP/RTSP 在设备上的联调；AndroidCAStore 导出与 hostname 校验的真机观察。
- API 16–20 路径的旧系统 ABI 验收（仅 API 21 工具链编译校验）。
- API 21–28 名称启发式与 API 29+ 硬件标志的真机对照。
- Surface 销毁、后台取消、快速开关 URL、flush/reconfigure、JNI 释放后引用：宿主已补 CSD/epoch 重配、Surface 释放后重设、重入取消、不可达/取消探测、destroy 放弃回收（ASan/UBSan）。2026-11-22 15:57 CST 起，`set_surface(NULL)` 只拆视频解码器并保留音频会话（不再走会关掉 AudioTrack 的 `release_decoder`）；Surface 重建不再从 Intent 重开 URL/路径；非关键帧在无表面且等待关键帧时丢弃以免堵住泵；新增 `ms_hw_player_notify_audio_focus`（GAIN 不清除用户暂停；LOSS/TRANSIENT/CAN_DUCK 均按暂停，不做音量闪避）。宿主 `test_surface_loss_keeps_audio_and_focus` 已过（ASan/UBSan）。2026-11-22 16:12 CST 起，另有 `test_csd_change_and_seek_while_unbound_or_paused`：无表面时仍收下新视频 CSD，重建后按已存 CSD 重配并等关键帧、音频保持；焦点丢失期间 seek 保持暂停并重置音频时钟，GAIN 才恢复，GAIN 不清除用户暂停。**仍未在真机或 AVD 上跑，不是前后台/焦点/出声验收**。
- HTTPS 点播前 AndroidCAStore 导出已改为复用缓存或后台线程（宿主/代码审查，**未在真机观察**）。
- 本用户无 `/dev/kvm` 权限，共享 AVD（`CJZB_API34`、`CJZB_ARM64`）按指示不再尝试启动。
- MediaCodec 真实输出形态（1 个压缩 AU 拆成多块 PCM、多 AU 合成一块、空/延迟/EOS 输出）、平台自动 delay/preroll 是否与本层 skip_end 对齐（不得双扣）、AudioTrack 部分 write 与 leftover、以及 playback head 时钟在真机或隔离 AVD 上的行为。宿主已补：`platform_trims_delay=1` 下按「压缩样点−平台 leading」设置 expected，AU 按样点计数结束，缺 PTS 或时间戳相同时第二包 skip_end 仍生效；部分写中途 flush 丢 leftover；暂停不排空 leftover、恢复从断点续写。未知帧长仍可能 expected=0，那时才回退 PTS/EOS。共享 `ms_hw_pcm_trim` 与这些宿主递增样点反例**不是**设备出声验收。
- 设备 AudioTrack `PlaybackParams` / 旧 `setPlaybackRate` 的真实听感、可接受范围与失败路径；pause/resume 与倍速下的出声连续性；相对/绝对 seek 后的真实关键帧起播与字幕重同步。宿主 mock 与编译不是该验收。
- 真机或隔离 AVD 上的 HLS EVENT/滑动窗口定位、回到直播边缘（安全距离）、窗口滑动后过期目标钳位、暂停/倍速下的时移听感与出画。宿主本地动态 HTTP 真实 demux 与播放器窗口钳位不是该验收。DASH 直播时移未实现。
- 位图字幕（DVB/PGS/VobSub）在真机或 AVD 上的叠加位置、裁剪、透明度与时间对齐。ImageView 接线与宿主 cue 不是视觉验收。
- 完整 ASS 栅格在真机或隔离 AVD 上的文字、描边、变换、移动、淡入淡出、karaoke、clip、分层与多语言叠加及时间对齐。宿主 `test_ass_render` 像素证据与 APK ImageView 接线不是视觉验收。不承诺全部 ASS 特效。
- Linux VAAPI / V4L2 在真实 DRM 设备上的打开、profile 查询、slice 提交与出画。本宿主 `test_hw_vaapi` 只证明码流参数映射，以及无 `/dev/dri` 时诚实返回非硬解；`decode_implemented` 本构建为 0。不得把探测或 `UNSUPPORTED` 写成硬解完成。
- Windows 硬件 Media Foundation / D3D11、iOS VideoToolbox 的编译与设备验收。本机当前无 mingw / Apple SDK，未实现、未编译，不伪造 Mac/Windows 验收。可行性（未安装、未开新任务）：Debian trixie 有开源 `gcc-mingw-w64-x86-64` 14.2.0-17+27 与 `mingw-w64-x86-64-dev`；本机 `sudo -n` 可用。若后续授权，可在 Bot 本机装最小开源 MinGW 作核心 C ABI 交叉编译，不操作用户电脑、不拉 Apple SDK。MF/D3D11 头是否齐全需安装后再清点。
- Linux VAAPI 真实 slice 提交与呈现。缺 GPU / `/dev/dri` 不等于不能写 fail-closed 的真实 submit，但尚未编写；不得用未验证伪实现对账。`is_hardware` 在提交闭环前保持 0。
- 正式 CentOS 7 容器发版。本机无 docker/podman、无所需镜像，宿主编译不能替代 el7。
- 真实 AI 字幕供应商实接、凭据与上云授权、真机转写/翻译效果与时延。宿主 `test_ai_sub` 只证明受控假供应商经修剪后 PCM tap 的框架闭环，**不是真实识别验收**。不得把模拟 `SIM-` 文本或测试按钮写成已用某商业 AI。
- MKV / WebM（Opus）在真机或 AVD 上的打开、切轨、seek、内嵌字幕叠加与出画出声。宿主 matroska demux 不是设备验收。

## 明确未做 / 不支持（本阶段不扩大）

- 非 H.264 视频、编码、HEVC、DRM/Crypto、RTMPT、SRTP、AAC-over-RTSP、完整摄像机 RTP 档案。受控本机 RTMPS（TLS 转发 + 本地 CA）已在宿主验证，校验保持开；**公网/设备 RTMPS 与 RTMPT 仍未测**。AAC 输出与 A/V 同步、带内 SPS/PPS、以及系统 AAC 有界例外：**接口与宿主 mock 已做，真机硬解/出画/出声/设备同步未验**。P4 文本字幕与同容器音轨切换、以及倍速/相对 seek：**宿主路径已做，设备实听/上屏/真实倍速未验**。HEVC：宿主解析/AU/CSD/mock 提交与 Android 配置路径已接线，**真机硬解与出画未验**；自写 TS HEVC AU 装配未做。HLS 备选 AUDIO 外链：宿主经 libmsff 已测（本地 HTTP master、切换/CSD/seek），**设备出声与真机切换未验**；自写 `ms_hls` master 仍拒绝；FFmpeg 在 header 阶段对部分失效 rendition 的行为以实测为准；底层在首次读包之后可能仍拉取已 discard 的旧 playlist，本层保证停止输出。DVB/PGS/VobSub（idx+sub）位图解析与叠加接线已在宿主落地、**真机视觉未验**；通用位图族/XSUB/图文仍未做。ASS/SSA 轻量去标签/布局模型仍在，不得称为完整渲染。可选隔离 `libmsass` 的宿主多时间点像素证据已于 2026-11-22 04:16 CST 官方矩阵通过，**不是设备视觉验收，也不承诺 100% 全部 ASS**。Android 测试 APK 已打入可选 `libmsass.so` 并接线 ImageView，**未安装、未启动共享 AVD，叠加与时间对齐未验**。2026-11-22 00:59 CST 起 matroska demux 已在宿主落地（MKV 内嵌已支持编码/字幕，WebM 仅 Opus），**设备未验**；不得把先前「ASS 事件解析已测」写成当时已能 demux MKV，也不得宣传未测的 VP9/VP8/Vorbis。MPEG-2 Video / MPEG-4 Part 2：宿主解析/AU/CSD/PTS-DTS（含 B 帧）、seek 门控、中途配置、mock 配置提交与 Android 查询-配置-提交路径已接线，**真机硬解与出画未验**；自写 TS 组帧未做。H.263：宿主解析（PSC 宽高、合法无 CSD）、3GP AU/PTS/关键帧、seek 门控、player 中途尺寸、mock 空 CSD 配置提交与 Android `video/3gpp` 路径已接线，**真机硬解与出画未验**；PLUSPTYPE/自定义尺寸未覆盖；自写组帧未做。AV1：宿主 OBU/av1C/帧内 SH 解析、MP4 AU/PTS/关键帧、seek 门控、player 中途 SH、合法无 CSD 与 av1C 配置提交、Android `video/av01` 路径已接线，**真机硬解与出画未验**；不是全部合法码流（见 FEATURE_MATRIX）；自写组帧未做。AC-3：宿主已落地（合法无 CSD、TS/MP4 AU/PTS、切轨时钟、`audio/ac3` 路径），**真机硬解与出声未验**；自写 `ms_codec_query(AC3)` 仍为 `NONE`；E-AC-3 未做；已编入 23:30 CST 内部双 ABI APK。MP3/MP2：宿主已落地（MPEG-TS `mpegaudio` 按帧 AU 与已开 MP4、层区分、切轨/seek 时钟、`audio/mpeg` / `audio/mpeg-L2`），**真机硬解与出声未验**。Opus：宿主已落地（mov/MP4 与最小 Ogg、真实 Head/dOps、`AV_PKT_DATA_SKIP_SAMPLES` 起始/尾部、pre-skip 与 80ms seek preroll 分项、尾部 discard padding 在解码后 PCM 上修剪），**真机硬解、csd-1/csd-2 自动 trim、AudioTrack 出声与听感均未验**；宿主 mock 不得写成听感通过；不是全部 Opus 码流；25ms 级 MP4 末包 SKIP/duration 以本树 FFmpeg 实测为限；Ogg 中途 seek 受页粒度限制。AVS+（广电广播类 / FFmpeg `cavs`，不是 AVS2/AVS3）：宿主已落地（手工打包 GY/T 257.2 序列头与 MPEG-TS AU/CSD/PTS/关键帧、seek 门控、player 中途配置、mock 配置提交；Android 无 AOSP MIME 时查询-配置诚实 `UNSUPPORTED`），**真机硬解与出画未验**；本箱无合法 AVS+ 编码器与 `/ms/build/ffmpeg`，fixture 不是工具编码也不是产品解码；不用 AVS2 或软件 cavs 充数；自写 `ms_codec_query(AVS_PLUS)` 仍为 `NONE`。AOSP 官方格式表无 AVS MIME，不得把 `"video/cavs"` 写成普遍 MediaCodec type；平台设备常无 AVS+ 硬件，无硬件不得软解回退。可选隔离 `libmsai` 的宿主框架与受控假供应商已于 2026-11-22 04:50:14 CST 官方矩阵通过，**不是真实 AI 识别完成，供应商实接未验**。当前内部工作副本 `build/android-hw/MsHwPlayer.apk` 14,378,081 字节，SHA-256 `c1b4cbcdfa4e24dfe6a7f92e54099d6b8af9c2f3292144b9a7c839ff5b6fb03c`（约 2026-11-22 04:51 CST，已签名未安装，含可选 `libmsai.so` 与 `libmsass.so`）；不得把 04:19 CST ASS 包（14,316,499，`6a74fe6e53eaa5383ebbfaced5be1ee77284c7f6034943c89c1752bc46d703bf`）或 03:55 CST 窗口定位包（10,859,331，`0029b791802bf5250a13c0e70e1b9bc42ea681fd3ad8e99a8ccb37abf8c8da19`）当作本实现；替代 03:25 CST trim 包（10,842,947，`eef92bd2b9afa5b9176a0148c0976470f7ec256437fb2053b28f680131b3ab42`），也不得把 03:03 CST ABI 包（10,834,755，`5ed9079bbd8d66275a1c77a4ebecd6ca9c95f399b991e1937220ae195af3986e`），也不得把 02:41 CST 时间轴 APK（同体积 10,834,755，`e587cb15d8f3a52abb065541e7d7abac1b10cb67d6bbb66cb255f3b7308e2c1c`），也不得把 02:14 CST 的位图组包 APK（同体积，`67d2846d04ac3bdf34e4a8e7592ac77f282ca6a28ae9cc97438f9c04802dccd6`），也不得把 01:55 CST HLS 视频变体包（10,830,659，`92ee83fad6c54d32cd29527774f1bb8d253c4aeac7d4003931aae86eb9872df5`）、01:30 CST HLS 字幕包或 00:59 CST matroska 包当作当前实现。2026-11-22 02:14 CST 起 PGS 续包与 DVB 超单 TS 包 PES 的 lavf 组包鲁棒性已在宿主闭环（16×16+ 真实多包/多 PES/拆分 segment 经 libmsff 到 RGBA），**真机叠加仍未验**；不以 4×4 单包或只喂完整 AU 代替该证据。2026-11-22 01:30 CST 起 HLS 字幕 rendition（独立 WebVTT 外链）已在宿主落地，**真机枚举/切换/叠加出画未验**；不是 MPEG-TS 封装或主 TS 带内 WebVTT，也不把清单一侧的 ASS/SRT 外链写成已验收。2026-11-22 02:38 CST 起，用户 seek 落入已开始未结束 cue、以及直播短 VTT 片头 cue / 滑动窗口去重，已在宿主用受控 HTTP VOD 与无 ENDLIST 动态 live 经真实 lavf demux 闭环，**真机仍未验**。2026-11-22 03:51 CST 起 HLS EVENT/滑动窗口可在源公布范围内定位（`ms_ff_get_seek_range` / live_edge 安全距离 / 过期钳位），**仍不无限缓存历史，也不是 DVR**；DASH 直播因 lavf 对 `is_live` 拒绝定位而保持 `NOT_SEEKABLE`。宿主只证明 AU/CSD/PTS，**不是出画**。2026-11-22 01:55 CST 起 HLS 视频变体手动选择（同 codec 两档分辨率/带宽）已在宿主落地，**真机切换与出画未验**；不承诺自动 ABR；跨 codec 变体本切片明确 `UNSUPPORTED`，不得写成全部切换完成。VP9（既有欠账，本轮未扩承诺）仍未做。返回 `UNSUPPORTED` 不等于这些项已完成（见 `docs/FEATURE_MATRIX.md`）。库默认仍拒绝软件 AAC；测试 APK 显式允许系统 AAC（状态写明「系统 AAC，非硬件」），这不是视频软解，也不是真机已出声。不得把 `allow_software` 当成全面软解开关，也不得把系统软解写成 `audio_is_hardware=1`。发出音频 TRACK、mock 时钟或字幕 cue 不等于扬声器已播放或画面已绘制字幕。
- 关闭 TLS 校验、跳过主机名校验、https→http 降级。
- 把 FFmpeg 软解当作产品解码（`CONFIG_DECODERS=0` 维持）。
- 四平台正式包、CentOS 7 容器验收、源码加壳（UPX）、更换顶层 license 或把 TLS 库改成 mbedTLS。
- 性能/RSS 承诺、正式发版。
- 在核心 `ms_hw_player` 内做全量线程安全（C API 仍要求调用方串行化；JNI 层提供 `op` 锁）。

## 交付包范围

源码包不含：`build/`、`third_party/cache/`、`.git/`、密钥与本地 CA、私密流、APK 与其它构建产物。版本钉选仍在 `third_party/*.pin`。HEAD 在打包时若仍为本地工作区，请以包内快照为准，不是已推送的远程提交。先前 `MsMedia-deliver-20261121-1639/`、`1654/`、`1704/`、`1716/`、`1742/`、`1802/`、`1819/` 及对应源码包保持不动。本轮（P4 音轨/字幕与 20:25 CST 倍速/seek 切片）**不另存**新的公开日期交付目录；内部测试 APK 可重建。不覆盖上述归档，也不覆盖 `build/release` 与历史 `build/android-v7a/size/libmsff.so.1.0.0`。用户最终要统一交付，非最终阶段保持内部结果。1716 归档（2026-11-21 17:16 CST）仍为 APK 4,432,494 字节，SHA-256 `0cef84c68e95fcb39c479abbdf19814ff67308634c1f2c0eb604cf4377fcd547`。1742 归档：`/workspace/MsMedia-deliver-20261121-1742/`，APK 4,432,494 字节（2026-11-21 17:42 CST），SHA-256 `352ae32552c3af3908b42a3fe7e39ade9d20d4adcec032edfcb72bc705c93eef`；`libmshw.so` 43,320，`libmsff.so` 4,244,104。1802 归档：`/workspace/MsMedia-deliver-20261121-1802/`，APK 4,448,878 字节（2026-11-21 18:02 CST），SHA-256 `d3448c333a0658e9d928ef46f9d7094ff44156d97659f05c23fecc3a24e9d06a`；`libmshw.so` 57,132，`libmsff.so` 4,244,104。1819 归档：`/workspace/MsMedia-deliver-20261121-1819/`，APK 4,452,974 字节（2026-11-21 18:19 CST），SHA-256 `ad9efc68e69e3913698b45586c8646202bdc8c523227e33f141746727e6c8a09`；`libmshw.so` 60,024，v7a `libmsff.so` 4,244,104。本轮归档：`/workspace/MsMedia-deliver-20261121-1838/`，APK 9,921,347 字节（双 ABI，2026-11-21 18:36 CST），SHA-256 `0963764b63ad651f216873fe6fee7ea9b9a59a540bee53fd14ca65c0dd853ce9`；v7a `libmshw.so` 62,112，v8a `libmshw.so` 71,480；v7a `libmsff.so` 4,244,104，v8a `libmsff.so` 5,250,888。未安装。不覆盖 1639/1654/1704/1716/1742/1802/1819。
