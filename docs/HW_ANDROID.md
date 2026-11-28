# Android H264 硬解路径（可选 libmshw）

by: Long <mycjzb@gmail.com>

日期：2026-11-21 12:36 CST（Asia/Shanghai）。本文件记录本里程碑的设计关联、实现范围与验证边界。不以交叉编译或宿主 mock 声称真实硬解已跑通。

## 1. 与现有 C ABI / 架构的关联

- 核心 `libmsmedia` 的 `ms_hw_query` **保持桩**：`is_hardware=0`、`verified=0`、`backend="none"`。`test_core` / `ms_so_abi_smoke` 的非硬解断言不变。核心 `.so` 仍只 NEEDED `libc`。
- 新能力在可选 `libmshw`（类似 `libmsnet` / `libmsff`），公开头 `include/msmedia/ms_hw.h`，符号版本 `MSMEDIA_HW_1.0`。`MSMEDIA_ABI_VERSION` 仍为 1；只增加独立库，不改核心 ABI 布局。
- 本阶段只做 **H264 视频硬解会话**（CSD、时间戳、Surface 输出、有界输入、EOS/flush/seek、格式变化、错误与生命周期）。非 H264 返回 `MS_ERR_UNSUPPORTED`，**不会**静默软解。后续编码/其它 codec 可在同一 C 接口上扩展，本构建不实现。
- 不把硬解塞进 `libmsff`，也不在核心引入 MediaCodec/FFmpeg 解码器。`CONFIG_DECODERS=0` 维持。
- 日志走 `ms_hw_decoder_set_log` 回调；不得打印私密 URL。

## 2. 复用与刻意不抽象

复用已有分配器合同、`ms_status`、`ms_codec` / `ms_sample_format`、有界队列思想与「引用交付 + 计量」原则。不为「统一四平台硬解」先做一层虚接口：Android 与宿主 mock 在 `ms_hw.c` 状态机下分支，平台细节分别在 `ms_hw_android.c` / `ms_hw_jni.c` / `ms_hw_mock.c`。

## 3. Media3 经验范围（不是 Media3 等价实现）

按公开 Media3 / Android MediaCodec 实践对齐，**不宣称 libmsplayer 等同 Media3**。
Android **演示播放**用与超级直播共用的 `bs-p2p/third_party/media3-src`（同级仓库，`MS_MEDIA3_SRC` 可覆盖；1.4.1 **源码模块**直编，见 `android/m3demo`）；**禁止**用 Maven `androidx.media3:…` AAR 冒充。C/JNI 硬解路径仍是自研，不链 Media3。

本步实际采用：

- 先 `MediaCodecList` 枚举，再 `createByCodecName`（或 NDK `AMediaCodec_createCodecByName`）。**不以 `createDecoderByType` 成功当作硬解。**
- API ≥ 29 优先信任 `isHardwareAccelerated()` / `isSoftwareOnly()`，并跳过 alias。
- API < 29（或标志不可用）使用与公开 Media3 `MediaCodecUtil` **同类**的名称启发式：`omx.google.`、`c2.android.`、`c2.google.`、`omx.ffmpeg.`、`omx.k3.ffmpeg.`、`omx.avcodec.`、`omx.qcom/qti.video.decoder.hevcswvdec`、`c2.qti.hevc.decoder.sw`、`omx.sec.`+`.sw.` 视为软件；其余 `omx.` / `c2.` 视为硬件；**未知命名空间视为软件**，不得报硬解。
- 默认 `allow_software=0`，拒绝软件组件返回 `MS_ERR_UNSUPPORTED`，`is_hardware` 仍为 0。
- Surface 输出，`releaseOutputBuffer(..., render=true)`，不把输出像素读回 CPU。
- CSD 拆成 `csd-0`/`csd-1`；时间基默认微秒，与 MediaCodec pts 一致。

本地曾查阅 `/workspace/apk-audit/decompiled/sources/androidx/media3`，该树未找到可直接引用的 `MediaCodecUtil` 实现；启发式按公开文档与已知前缀编写，不复制受版权保护的大段源码。

NDK `AMediaCodecStore` / `AMediaCodecInfo` 为 API 36 引入，本路径面向 API 21，**不依赖**它们做选择。

## 4. 实现分层

| 路径 | 作用 | 本里程碑状态 |
| --- | --- | --- |
| 宿主 mock | 状态机、引用释放、反压、flush、EOS、计量；`is_hardware` 诚实 | 已编译并在宿主跑通（不是设备硬解） |
| API 21+ AMediaCodec + JNI 列表 | 选择 + Surface I/O | NDK r29 armeabi-v7a 已交叉编译；**未在设备/模拟器运行** |
| API 16–20 JNI MediaCodec 薄桥 | 无反射，缓存 `jclass`/`jmethodID`；`getInputBuffers` / `INFO_OUTPUT_BUFFERS_CHANGED` / `BUFFER_FLAG_SYNC_FRAME` | 源码完整；用 **同一 NDK r29 的 API 21 工具链** 编译为 `libmshw-legacy.a`。**NDK r29 没有 API 16 工具链**，不能产出可在 API 16 设备加载的官方 target |
| 验证 APK `MsHwSmoke` | 加载 `libmshw`，`nativeProbe` 打印选择结果 | 已打包签名；**未安装、未运行** |

无 JVM 时（例如交叉编译出的 `mshw_probe` 在设备上若不以 APK 加载）：查询必须说明「MediaCodecList 需要 JVM」，`is_hardware=0`，不得报硬解。

## 5. 内存、所有权与计量

- 提交持有输入引用，直到写入平台 input（计 `platform_copy_*`）或 mock 消费后调用 `release_fn`。不新增整帧中间缓冲。
- 有界硬上限：在途输入默认 8 / 绝对 32；在途压缩字节默认 1 MiB / 绝对 8 MiB；事件默认 8 / 绝对 32。满则 `MS_LIMIT`（反压）。
- 统计：`ref_hold_*`、`platform_copy_*`（平台压缩输入必要 copy，分列）、`staging_copy_*`（本库额外中间拷贝，目标 0）、`pin_peak_bytes`。宿主 mock 的 `platform_copy_*` 与 `staging_copy_*` 均为 0。
- CSD 只在 ≤ 8 KiB 的固定缓冲中拆 SPS/PPS。

## 6. 已完成的可验证结果（2026-11-21 12:36 CST）

### 6.1 宿主 mock（已运行）

- 入口：`env -u CC -u CXX -u LD PATH=/usr/bin:/bin ./build.sh debug` 与 `./build.sh test-size`。
- `build/debug/test_hw_backend` 在 ASan/UBSan 下通过（分类、默认查询非硬解、拒绝软解、无 Surface / 非 H264、生命周期、引用持有至 drive、pts、反压、flush/destroy 释放、EOS、坏 avcC）。
- `./build.sh test-size`（无 sanitizer）通过：42 组 `test_core`、`test_net`、`test_hw_backend`、`test_ff_backend`（TLS 1.2/1.3 矩阵、HTTP/HLS/DASH、RTSP TCP/UDP、RTP 帧合同、RTMP 拉流、负时间戳、TIMEOUT）。体积优化产物未改：宿主 strip `libmsff.so.1.0.0` 仍为 **5,767,128** 字节，`libmsmedia.so.1.0.0` **125,112**，`libmsnet.so.1.0.0` **31,240**。既有 `build/release` 基线 `libmsff.so` **7,986,520** 未覆盖。
- 宿主 `build/release-size/libmshw.so.1.0.0` strip **18,448** 字节（unstripped 21,384）。这是 mock，NEEDED 仅 libc 一类，不是设备硬解库。

### 6.2 交叉编译（已编译，未运行硬解）

非正式 NDK r29（r29.0.14206865）API 21 armeabi-v7a，入口 `./build.sh android-hw` → `scripts/android_hw_build.sh`。不是四平台/旧 Android 正式验收。

| 产物 | 大小（字节） | 说明 |
| --- | ---: | --- |
| `build/android-hw/lib/armeabi-v7a/libmshw.so` | 21,368 | strip；NEEDED：libmediandk / libandroid / liblog / libc / libdl |
| `build/android-hw/libmshw.unstripped.so` | 28,280 | 对照 |
| `build/android-hw/libmshw-legacy.a` | 48,238 | `-DMS_HW_LEGACY_JNI` 编译校验 |
| `build/android-hw/mshw_probe` | 9,536 | 无 JVM 探测可执行文件；不得单独声称硬解 |
| `build/android-hw/MsHwSmoke.apk` | 33,243 | minSdk 21，含上述 so；已 apksigner 校验，未安装 |

动态导出含全部 `ms_hw_*` 公共符号以及 `JNI_OnLoad` / `Java_com_mycjzb_media_player_MsHwSmokeActivity_nativeProbe`（`MSMEDIA_HW_1.0`）。

## 7. 阻塞与未验证项（不得写成已通过）

- **没有已连接的 Android 设备。** 查询时刻 `adb devices` 为空。未对用户电脑做任何操作，未安装收费服务。
- 箱体存在共享 AVD（`CJZB_API34`、`CJZB_ARM64`）与 KVM。为避免改写已有 AVD 状态，**本轮未启动模拟器**，也未 `adb install` / 运行 APK。
- 因此以下均未做：**真实硬件组件上的解码**、Surface 出画、设备上的 CSD/时间戳/EOS/flush/格式变化、`platform_copy_*` 实机计量、模拟器上拒绝 `c2.android`/`OMX.google` 的运行观察（该拒绝是策略的预期，仍需设备或模拟器跑过才能记录）。
- 典型模拟器往往只有软件 AVC；即使以后跑起来，正确策略应拒绝而非把软解报成硬解。
- API 16–20：源码与 API 21 工具链编译校验不等于旧系统 ABI 验收；NDK r29 不能以 API 16 为 target。
- API 21–28 的名称启发式未经真机对照 `isHardwareAccelerated`。API 29+ 标志路径未经真机。
- 未测 HEVC/音频/字幕/编码、未测受保护内容（Crypto）、未测动态改 Surface。
- 本 APK 探测不提交码流，即使将来安装成功，也只验证选择/查询（以及可选的无码流生命周期），**不是一帧硬解播放**。

## 8. 如何继续（需新授权才扩展）

在已连接设备或可安全使用的临时 AVD 上安装 `build/android-hw/MsHwSmoke.apk`，记录 `nativeProbe` 原文（组件名、`is_hardware`、`verified`、拒绝原因）。若有硬件 AVC，再以受控测试码流验证 submit/drive/Surface，并单独记录。不要把本文件第 6 节当作设备验收。

## 9. 播放编排与可运行 APK（2026-11-21 16:26 CST）

本段是在第 6 节硬解会话之上的编排层，不是设备硬解或出画验收。

### 9.1 编排 API

- 源：`include/msmedia/ms_hw.h` 追加 `ms_hw_player_*`（仍为 `MSMEDIA_HW_1.0`，ABI 版本 1），实现 `src/hw/ms_hw_player.c`。
- 职责：公共 `ms_event`（CODEC_CONFIG / AU / EOS / DISCONTINUITY）→ `ms_hw_decoder` → Surface。包含 CSD 缓存、事件时基转到微秒、start/flush/disc 后的关键帧门闩、`MS_LIMIT` 反压、取消、EOS、flush 新 epoch、`release_decoder` 后再次 `set_surface`、引用持有与无 `release_fn` 时的 staging copy 分列。
- 分层计时（`CLOCK_MONOTONIC` 相对 `t_start_us`，未发生为 -1）：

  | 字段 | 含义 | 是否出画 |
  | --- | --- | --- |
  | `t_first_packet_us` | 首个进入播放器的事件 | 否 |
  | `t_first_decodable_au_us` | 通过门闩的首个可提交视频 AU | 否 |
  | `t_first_submit_us` | 首次 `ms_hw_decoder_submit` 成功 | 否 |
  | `t_first_output_us` | 首次 `MS_HW_EVENT_OUTPUT_RENDERED`（平台交 Surface / mock 等同 `releaseOutputBuffer(render)`） | **不是实际显示**（`output_is_not_display` 恒为 1） |
  | `t_first_display_us` | 仅 `ms_hw_player_note_display` | 仅当应用提供上屏钩子；编解码不得调用 |

- 宿主 mock 的 `is_hardware` 在播放器统计里强制为 0，即使 decoder mock 模拟“选到硬件组件”。
- 宿主入口：`build/debug/test_hw_player`（ASan）。状态机用例：门闩、CSD、preroll、反压、copy/引用分列、flush、切换释放、取消、EOS。可选 `MS_HW_PLAYER_TS` / `FLV` / `UDP` / `RTP` 走 `ms_ff` 拉流再喂播放器（仍是 mock 硬解，不是出画）。本轮实测（2026-11-21 16:23 CST）：`MS_HW_PLAYER_TS=file:build/dev-samples/dash-ff/short.ts` 与 `MS_HW_PLAYER_FLV=file:build/dev-samples/dash-ff/short.flv` 均提交 25 个视频 AU；display=0。UDP/RTP 入口已预留环境变量；受控环回示例（不作为本轮播放器实测通过）：

```
# TS / FLV（本轮已跑，exit 0，mock 非出画）
MS_HW_PLAYER_TS=file:build/dev-samples/dash-ff/short.ts \
MS_HW_PLAYER_FLV=file:build/dev-samples/dash-ff/short.flv \
  ./build/debug/test_hw_player

# UDP MPEG-TS / RTP（需另起本机发送，且本机已有官方 FFmpeg 发送端）
# ffmpeg -re -i build/dev-samples/dash-ff/short.ts -c copy -f mpegts "udp://127.0.0.1:1234?pkt_size=1316"
# MS_HW_PLAYER_UDP='udp://127.0.0.1:1234'
# RTP 入口变量为 MS_HW_PLAYER_RTP，样本目录见 build/dev-samples/dash-ff/rtp-h264。
```

`test_ff_backend` 已单独覆盖受控 UDP MPEG-TS 与 RTP 帧合同，不能把那组结果写成 `test_hw_player` 已拉流。


### 9.2 可运行 APK（已交叉编译，未安装）

入口仍是 `./build.sh android-hw`。主界面改为 `MsHwPlayerActivity`（Surface + `nativeCreate` / `setSurface` / `injectDemo` / `drive` / `nativeStats`）。`MsHwSmokeActivity` 仍在同一 APK 内但不再是 LAUNCHER。演示 AU 不是真实节目；真机 MediaCodec 可能对这组占位 avcC 报错。APK 未挂钩 Choreographer，`display` 保持未置位。

| 产物 | 大小（字节） | 说明 |
| --- | ---: | --- |
| `build/android-hw/lib/armeabi-v7a/libmsmedia.so` | 126,832 | strip；仅 libc/libm/libdl；供会话解复用与时间换算 |
| `build/android-hw/libmsmedia.unstripped.so` | 147,088 | 对照 |
| `build/android-hw/lib/armeabi-v7a/libmshw.so` | 33,128 | strip；含编排与 JNI；NEEDED：libmsmedia / libmediandk / libandroid / liblog / libc / libdl |
| `build/android-hw/libmshw.unstripped.so` | 44,544 | 对照 |
| `build/android-hw/libmshw-legacy.a` | 61,840 | 含 `ms_hw_player.c` 的 API21 工具链编译校验 |
| `build/android-hw/mshw_probe` | 9,536 | 仍无 JVM，不得声称硬解 |
| `build/android-hw/MsHwPlayer.apk` | 180,774 | minSdk 21，含上述两个 so 与 Player/Smoke Activity；已 apksigner 校验；**未安装、未在设备或模拟器运行** |

APK 主界面可 `nativeInjectDemo` 或通过 Intent `path` / `file://` 走 `ms_session` 解本地 TS/FLV（`nativePlayPath`）。**未打入 libmsff**，设备上不能走官方 FFmpeg HTTP/HLS/RTSP。演示 AU 不是真实节目；真机 MediaCodec 可能对占位 avcC 报错。未挂钩 Choreographer，`first_display` 保持未置位。为编 armeabi-v7a 的 `libmsmedia`，`ms_util.c` 在无 `__int128` 时走便携乘除（宿主仍用 `__int128`）。

### 9.3 仍未完成

- `adb devices` 仍为空；未启动共享 AVD（`CJZB_API34`、`CJZB_ARM64`），未安装 APK，未做临时隔离 AVD。
- 未测真实 Surface 出画、未测首帧 <1s、不能把本段计时写成实际显示。
- 软件模拟器不得报硬解通过。
- APK 内本地文件路径已实现但未在设备上跑过；上屏钩子需应用调用 `note_display`；非 H.264、libmsff 未进 APK。

## 10. 网络 APK 与 libmsff 集成（2026-11-21 16:39 CST）

本段把已有优化 v7a FFmpeg/OpenSSL 前缀**重链当前适配器**后打进可运行播放 APK，复用 `ms_ff` + `ms_hw_player`，不另写一套拉流。不是设备硬解或出画验收。历史体积对照物 `build/android-v7a/size/libmsff.so.1.0.0`（4,235,076）未覆盖。

### 10.1 已支持（本机可验证）

- APK 含 `libmsff.so`（strip **4,241,096** 字节，NEEDED 仅 libz/libm/libdl/libc，静态合入钉选 OpenSSL 3.5.9 + FFmpeg 9.1 + libxml2）、`libmsmedia.so`（126,832）、`libmshw.so`（37,968，NEEDED 含 libmsff/libmsmedia，RUNPATH `$ORIGIN`）。
- Java 异步 URL 输入、取消、Flush、Surface 创建/销毁（销毁时 `release_decoder`）、网络超时与错误回传 UI；**不把 URL 写入日志或状态文本**。
- TLS：`verify`/`tls_verify` 保持 1；FFmpeg OpenSSL 后端在 verify=1 时做证书链与 `SSL_set1_host` / IP SAN 校验。无可用信任锚时 `ms_ff_open` 对 https/tls/rtsps/rtmps 返回 `MS_ERR_UNSUPPORTED`，**不关验证、不发起握手**。Android 通过导出 `AndroidCAStore` 为 PEM 再 `ms_ff_set_ca_file`；导出失败则明确「配置缺失」。新增 `ms_ff_set_ca_path`、`ms_ff_set_allow_system_trust`、`ms_ff_trust_ready`、`ms_ff_url_needs_tls_trust`。
- 宿主 `test_hw_player` 受控环回：file TS/FLV 各提交 25 个视频 AU；UDP MPEG-TS 与 RTP/SDP（ffmpeg `-re -stream_loop` 本机发送）完整喂入播放器，测到 first_packet / first_decodable_AU / first_submit / first_output，**display=0**（mock，无 `note_display`）。
- 回归：`test_ts_scan`；`test_ff_backend` 全矩阵（含 HLS PNG 内部分片 `ops=1 last_bytes=457 kind=png`、TLS 1.2/1.3、hostname mismatch、取消、无 CA 拒绝 https）。未把 SKIP 写通过。

### 10.2 渲染证据（研究结论，已实现分列）

- `releaseOutputBuffer(..., render=true)` / `MS_HW_EVENT_OUTPUT_RENDERED` 只表示交 Surface，`output_is_not_display` 恒为 1。
- NDK `AMediaCodec_setOnFrameRenderedCallback` **自 Android T / API 33**（API 21 工具链用 `dlsym`，符号不存在则跳过）。官方说明：仅供参考、可能延迟/批处理，有的帧已渲染也可能无回调。本库写入 `t_first_codec_rendered_us`，`codec_rendered_is_not_display` 恒为 1，**不得写入 `t_first_display_us`**。
- Java `MediaCodec.OnFrameRenderedListener` 自 API 23，但本路径 codec 在 native 创建，未挂该监听。
- **单次 Choreographer 回调不是视频已显示的证据**，本 APK 不把它接到 `note_display`，也不用手填 display 制造 <1 秒达标。
- `t_first_display_us` 仅应用显式调用 `ms_hw_player_note_display`；`display_evidence` 为 `none` 或 `app_note_display`。

### 10.3 只读 AVD 条件（未启动）

查询时刻（2026-11-21 16:39 CST）：`adb devices` 为空。存在共享 AVD `CJZB_API34`、`CJZB_ARM64`，**未启动、未安装**。本用户不在 kvm 组，`/dev/kvm` 为 `root:pulse-access`，本进程无硬件加速模拟器条件。已有 android-34 x86_64/arm64 系统镜像与磁盘余量，但软件模拟器即使可另建临时 AVD，也**不得报硬解通过**。因此设备硬解、真实 Surface 出画、首帧 <1s 仍为缺口。

### 10.4 产物与命令

```
env -u CC -u CXX -u LD PATH=/usr/bin:/bin ./build.sh debug
env -u CC -u CXX -u LD PATH=/usr/bin:/bin ./build.sh android-hw
MS_FF_DASH_DIR=build/dev-samples/dash-ff ./build/debug/test_hw_player
OPENSSL_CONF=build/openssl-3.5.9/prefix/ssl/openssl.cnf \
  MS_FF_CA=build/debug/net-ca MS_FF_TMP=build/debug/ff-tmp \
  MS_FF_DASH_DIR=build/dev-samples/dash-ff ./build/debug/test_ff_backend
```

`MsHwPlayer.apk`（2026-11-21 16:39 CST）**4,424,302** 字节，已 apksigner 校验，SHA-256 `d4589467bbad1fbea0727d8c3142deacf968433e6c8247bfd473ad35382c93b4`。未安装。`usesCleartextTraffic` 仅方便受控 HTTP，不关闭 HTTPS 校验。

## 11. 帧守恒与 JNI 生命周期审查（2026-11-21 16:53 CST）

本轮限定代码审查与宿主受控验证，不重试 KVM/模拟器/用户电脑。mock 输出不是出画。

### 11.1 UDP 25/15、RTP 25/13 的去向

`short.ts`/`short.flv` 只有 index 0 一个关键帧。file 从头提交 25/25。先前直播用例先启动 ffmpeg 再 `listen=1` 打开，接收端从 GOP 中途的第 k 帧开始：窗口 25 内 `drop_nokey=25-k`、`submitted=k`、`preroll=0`。本轮同条件下实测 UDP k=16（16+9=25）、RTP k=13（13+12=25），守恒成立，**属于合法非关键帧门闩，不是 preroll，也不是本跑的 LIMIT 静默丢帧**。先 listen 再延迟发送的 UDP-from-start 抓住唯一 IDR，提交 25/25、`drop_nokey=0`，反证不应靠提交不可解帧凑 25。

已知 GOP 回归：人造 GOP=5；以及 `ffmpeg -stream_loop 1 -c copy` 的 twice `short.ts` 跳过 10 帧，期望 `drop_nokey=15 submitted=25 in=40`。

### 11.2 已修缺陷

- `video_au_in` 只在消费（preroll / nokey / 成功 submit）后增加；LIMIT 记 `video_au_backpressure` 且所有权留在调用方。
- 泵和 JNI 对 LIMIT/AGAIN 重试，未消费不得当成功释放；禁止为凑数提交非关键帧。
- `ms_hw_decoder_drive`：EOS 后幂等；IDLE/CONFIGURED（未 start）为空操作，避免拉流循环提前 drive 把 `last_error` 写成 STATE。
- JNI：`nativeOpenUrl` 先取消 I/O 再排队 URL，不在 UI 线程 join；工作线程循环取 pending；`op` 锁串行化 Surface/flush/feed/drive；`nativeCancel` 不 join；`nativeDestroy` join 后清空 `p`/`win`。宿主 `test_op_lock_flush_surface_feed` 用同一协议并发 mock。

### 11.3 本轮计时（宿主 mock，2026-11-21 16:52 CST）

相对 CLOCK_MONOTONIC 毫秒。`first_output` 是 mock OUTPUT_RENDERED，**不是显示**。`codec_rendered=-0.001` 表示未置（宿主无 OnFrameRendered）。`display=0`。

| 路径 | in/submitted/drop_nokey | packet | decodable | submit | output(not paint) |
| --- | --- | --- | --- | --- | --- |
| file TS | 25/25/0 | 0.031 | 0.369 | 0.379 | 0.390 |
| file FLV | 25/25/0 | 0.049 | 0.101 | 0.112 | 0.121 |
| live UDP 中途入 GOP | 25/16/9 | 0.048 | 0.284 | 0.297 | 0.308 |
| live RTP 中途入 GOP | 25/13/12 | 0.113 | 0.254 | 0.274 | 0.282 |
| live UDP-from-start | 25/25/0 | 0.056 | 0.124 | 0.142 | 0.156 |

复现：`env -u CC -u CXX -u LD PATH=/usr/bin:/bin ./build.sh debug`，再设 `MS_FF_DASH_DIR` / `MS_HW_PLAYER_TS` / `MS_HW_PLAYER_FLV` 跑 `build/debug/test_hw_player`。`MS_HW_PLAYER_UDP=1` 不再被当成 URL。

### 11.4 剩余依赖（可独立项已完）

真机/AVD 硬解与出画、APK 设备联调、CA 真机观察、RTMPS、四平台/el7、`nativePlayPath` 的 UI 读文件阻塞。C 播放器 API 本身仍非线程安全。不扩大范围。

### 11.5 本轮产物（2026-11-21 16:54 CST）

工作副本 `build/android-hw/MsHwPlayer.apk` **4,424,302** 字节，已签名，SHA-256 `d606e5bab1b78139c05e9c28a9a8257ae50f5ee81c5e86ad36389dbf4f2434ca`（与 16:39 归档哈希不同）。`libmshw.so` **39,720**（含本轮 JNI/播放器修复）。归档：`/workspace/MsMedia-deliver-20261121-1654/`。历史 `build/android-v7a/size/libmsff.so.1.0.0`（4,235,076）与 `MsMedia-deliver-20261121-1639/` 未覆盖。未安装。

## 12. 请求端到端计时与 PlayPath 异步（2026-11-21 17:03 CST）

### 12.1 计时边界（纠正 §11.3 旧表的误导）

§11.3 表中约 0.03–0.4ms 的数字是 **管线局部** 计时：当时测试往往在 `ms_ff_open`/探流之后才创建播放器，`t_start` 已错过建连、探流和套接字中已缓冲的等待。这些数字**不是**用户点播到首包/首可解 AU 的端到端延迟，不能用来声称直播低延迟。

现约定：

- `t_start` / `t_first_*`：create 或最近一次 flush 的管线局部原点。flush 会重置这组值。
- `ms_hw_player_mark_request`：必须在 open/probe/读文件/拉流之前调用。`t_req_first_*` 相对该原点，包含建连、探流与等待关键帧。flush 不改请求原点与已记录的 `t_req_*`。
- `first_output` 仍不是出画；`t_first_display_*` 仅 `note_display`。本轮 display=0。

### 12.2 含注入等待的实测（2026-11-21 17:02 CST，宿主 mock，非出画）

受控 AU（不靠预缓冲或 `-re` 回放声称低延迟）：

- mark 前等待 150ms，喂 CSD+非关键帧后再等 300ms 喂 IDR：`req_packet=0.014ms`，`req_decodable=300.154ms`，`req_submit=300.192ms`，`stage_local decodable=450.270ms`。断言要求请求端到端不小于注入的 IDR 等待。

file（mark 后 open 本地文件，含打开/探流，仍非直播）：

| 路径 | 守恒 | stage_local packet/decodable/submit | request_e2e packet/decodable/submit |
| --- | --- | --- | --- |
| file TS | 25/25/0 | 2.609 / 2.973 / 2.986 ms | 2.561 / 2.925 / 2.938 ms |
| file FLV | 25/25/0 | 1.874 / 1.929 / 1.942 ms | 1.846 / 1.901 / 1.914 ms |

直播（在 open 前 mark；`request_e2e` 含 listen/open/probe。UDP/RTP 的 `io_timeout` 分别为 4s/5s，open 可能接近该上限，**不得把下表读成只有注入的 250ms**）：

| 路径 | 守恒 | request_e2e packet / decodable / submit |
| --- | --- | --- |
| live UDP 中途入 GOP | 25/16/9 | 4047.930 / 4048.212 / 4048.227 ms |
| live RTP 中途入 GOP | 25/13/12 | 4015.495 / 4015.652 / 4015.665 ms |
| live UDP-from-start（注入延迟发送 250ms，断言 ≥200ms） | 25/25/0 | 5060.811 / 5060.968 / 5060.988 ms |

### 12.3 JNI

`nativePlayPath` 与 `nativeOpenUrl` 共用同一 `queue_session` / 工作线程 / 取消；UI 不再同步读整文件或 join 上一会话。工作线程在 flush 后、open 或读文件前调用 `mark_request`。Demo/Drive/Flush/Cancel/set_surface 不在 UI 线程做整文件读取或网络 open。仍非真机生命周期验收。`nativeDestroy` 仍会 join 工作线程以释放资源。

### 12.4 剩余依赖

真机/AVD 硬解与出画、首帧 <1 秒、APK 设备联调、RTMPS、el7/四平台。不扩大范围。

### 12.5 本轮产物（2026-11-21 17:04 CST）

工作副本 `build/android-hw/MsHwPlayer.apk` **4,428,398** 字节，已签名，SHA-256 `c6f5bb326ee1a3acccf8205b02e9b4c6a2dc1eacb1d6465ee73d84cb6fd03045`。`libmshw.so` **40,936**。归档：`/workspace/MsMedia-deliver-20261121-1704/`。不覆盖 `MsMedia-deliver-20261121-1639/`、`MsMedia-deliver-20261121-1654/` 与历史 `build/android-v7a/size/libmsff.so.1.0.0`（4,235,076）。未安装。

## 13. 有界快速起播与 4~5 秒探流拆分（2026-11-21 17:14 CST）

### 13.1 真正瓶颈

17:02 的 `request_e2e` 把 UDP/RTP 记到约 4~5 秒，不是「只差设备出画」。本轮在同条件受控流上拆分后，来源是 `avformat_find_stream_info` 在官方默认 `analyzeduration`（5,000,000 µs）下等待音频 codecpar 完整（无软解时 AAC 常报 unspecified sample rate），再被测试的 `io_timeout_ms`（4s/5s）打断。`read_timeout`/`io_timeout` **不是**正常起播预算，只应约束无响应 I/O。

同一次回归（2026-11-21 17:13 CST，宿主 mock，`display=0`，按时 `-re` 发送，不预读缓存、不超速、不剪掉等待）：

| 路径 | 探流 | 守恒 in/sub/drop_nokey | open_input | find_stream_info | request_e2e packet / decodable / submit |
| --- | --- | --- | --- | --- | --- |
| live UDP 中途入 GOP | DEFAULT | 25/16/9 | 236.360 ms | **3815.432 ms** | 4051.920 / 4052.205 / 4052.227 ms |
| live UDP 中途入 GOP | FAST_START | 19/9/10 | 278.213 ms | **585.341 ms** | 863.617 / 863.839 / 863.851 ms |
| live RTP 中途入 GOP | FAST_START | 25/13/12 | 0.604 ms | 532.424 ms | 533.162 / 533.308 / 533.325 ms |
| live UDP-from-start（注入延迟发送 250ms，packet ≥200ms） | FAST_START | 25/25/0 | 306.385 ms | 99.950 ms | **408.245 / 408.316 / 408.332 ms** |

对照：17:02 在官方默认探流下 UDP-from-start 为 5060.811 / 5060.968 / 5060.988 ms（同一配方，含 250ms 注入与探流）。FAST 本跑 submit 约 408ms，其中 open_input 约 306ms 已包含延迟发送，find_info 约 100ms，不得把 408ms 说成「只有 250ms」，也不得声称 1 秒出画已验。

FAST 中途入 GOP 的 19/9/10 是在同一循环发送端上先跑完 DEFAULT 后再开第二路，窗口更靠后，守恒 `19=9+10` 仍成立，是合法关键帧门闩，不是静默丢帧。

本机 HTTP 控制服务（文件一次发送，**不是** `-re` 直播时基；VOD 速率 TCP）：

| 路径 | 守恒 | open_input / find_info | request_e2e submit |
| --- | --- | --- | --- |
| HTTP TS FAST | 25/25/0 | 1.434 / 0.581 ms | 2.267 ms |
| HTTP FLV FAST | 25/25/0 | 1.644 / 0.547 ms | 2.427 ms |

file FAST：TS request_e2e 2.343 / 2.702 / 2.716 ms（25/25/0）；FLV 1.681 / 1.733 / 1.746 ms（25/25/0）。受控 AU 注入 150ms+300ms IDR 仍为 `req_decodable=300.177ms`（含真实等待）。

### 13.2 Media3 对照与有界策略（公开资料，非拷贝实现）

Media3 `TsExtractor.read()` 每次只消费有限输入，并不使用 FFmpeg 式 5 秒 `analyzeduration`；H264Reader 可从带内 SPS/PPS 得到 format。历史上 `ProgressiveMediaPeriod` 会等已声明的每条轨都有 format 才结束 prepare（PMT 列了轨但迟迟无包时是已知问题）。本轮**不**把 `probesize`/`analyzeduration` 改成 0，**不**关闭音轨。采用有界快速起播：配置与首个可解关键帧齐即可安全送硬解；慢/缺轨、PMT 前 AU、配置变化、音画同步列为后续完善与明确 fallback。

公共 ABI 仍为 `MSMEDIA_1.0` / `MSMEDIA_ABI_VERSION=1`（仅加法）。库默认 `MS_FF_PROBE_DEFAULT`（官方探测，稳健）；`MS_FF_PROBE_FAST_START` 为分离的低延迟直播选项。FAST 有界值：`analyzeduration=400000` µs、`probesize=524288`、`max_probe_packets=48`、独立探测 deadline **500ms**（不是 0）。`open_input` 仍用完整 `io_timeout`，以便延迟发送不被竞态丢掉。探测失败或缺音频参数仍允许读包（原行为）。JNI 播放路径与 `test_hw_player` 的直播/HTTP/file 入口使用 FAST；`test_ff_backend` 保持 DEFAULT 且全矩阵通过。

### 13.3 请求标记、CA 与销毁

- `ms_hw_player_mark_request` 在 Java 点击进入 `queue_session` 时调用（排队与旧会话取消计入用户等待）。
- `ms_hw_player_mark_worker_start` 在工作线程 `begin_session`（flush 之后）调用，区分 enqueue 与 worker start。
- `ms_hw_player_note_open_phases` 记录 `dur_open_input` / `dur_find_stream_info` 及相对请求的完成时刻。
- AndroidCAStore：已有 PEM 缓存直接复用；首次导出在同文件顶层 `MsHwCaExportTask` 后台进行（无内部类/lambda），完成后再回 UI 打开 URL。导出失败不得关校验，也不无限重试。
- `nativeDestroy`（17:16）：先取消 I/O 并 `exit_req`，对 `th_finished` 做最多 2 秒 `pthread_cond_timedwait`，然后仍 `pthread_join`。**该无界 join 已被 §14 取代**：超时后改为 detach + 后台回收，不再在 UI 上无限等待，也不超时后直接 free。

### 13.4 严格边界

宿主 mock，无 display。不声称 1 秒出画已验。内网高速流 <1 秒是目标；本轮证明探流开销在 FAST 下可从约 4~5 秒降到百毫秒级，加上诚实的发送/IDR 等待后 from-start submit 约 408ms，**仍不是设备上屏**。不扩大其它平台或编码。

### 13.5 本轮产物（2026-11-21 17:16 CST）

工作副本 `build/android-hw/MsHwPlayer.apk` **4,432,494** 字节，已签名，SHA-256 `0cef84c68e95fcb39c479abbdf19814ff67308634c1f2c0eb604cf4377fcd547`。`libmshw.so` **42,428**，打入的 `libmsff.so` **4,241,992**（NEEDED 无系统 libssl）。归档：`/workspace/MsMedia-deliver-20261121-1716/`。不覆盖 `MsMedia-deliver-20261121-1639/`、`1654/`、`1704/`，不覆盖 `build/release` 与历史 `build/android-v7a/size/libmsff.so.1.0.0`（4,235,076）。HEAD 仍 `1092bec`。未安装。

## 14. 快速起播稳定性边界（2026-11-21 17:40 CST）

本轮在 t90u 原授权内补齐 FAST 起播后的安全边界，不扩大其它平台或编码。宿主 ASan/UBSan 已回归；**真机/出画仍未验**。不得把仅视频 H264 写成完整有声播放器。

### 14.1 已实现（宿主已测，非正式验收）

| 项 | 行为 | 不是什么 |
| --- | --- | --- |
| FAST deadline 之后继续读 | 探测短 deadline 触发时，FFmpeg 常把 `pb->error` 写成 `AVERROR_EXIT` 并置 `eof_reached`。清掉这类**软 interrupt**（EXIT/ETIMEDOUT/EINTR）后保持打开，置 `probe_incomplete=1`，并清除探测用的 `timed_out`，后续 `av_read_frame` 可继续。本轮 UDP-FAST mid-GOP：`probe_incomplete=1`，find_info=585.068ms，仍读到视频 AU。 | 不是「全部轨/参数已识别」。不是把部分失败叫满成功。 |
| 上下文不可用时的失败 | `ic`/`pb` 已不在（且非 `AVFMT_NOFILE`），或探测失败且留下不可恢复的硬 I/O 错误：关闭输入，返回可诊断状态（TIMEOUT/CANCELED/映射错误/`UNKNOWN`），**保留** open/find 计时，不把失败写成 OK，也不静默丢上下文。 | 不是无限等待。不是超时后假装成功。 |
| 探测已识别流、随后 pb 硬错误 | 若 `find_stream_info` 已成功且 `nb_streams>0`（例如短 RTMP 发布结束留下 `EIO`），保持打开，由后续读包报告 I/O；不把已缓冲的包当成 open 失败。`test_ff_backend` RTMP：format=rtmp，events=36，h264_au=11，aac_au=20。 | 不是忽略硬错误；只是不把已完成的探测改写成 open 失败。 |
| 缺音轨 / 延迟参数 | FAST **不关音轨**。文件路径仍发出音频 TRACK（本轮 `video_au=25`）。`nb_streams` 增长会补发 TRACK；extradata/带内 SPS+PPS 签名变化会再发 `CODEC_CONFIG`；已发送轨上的 codec 变化发 `TRACK_UPDATE`（仅信号）。 | **音频输出与 A/V 同步仍未实现**。公共 `ms_event`/`ms_track_info` 未加 sample_rate/width/height（ABI 未扩大）。`TRACK_UPDATE` 不是完整 FORMAT 刷新。 |
| 视频 CSD 变化 | 已配置且 CSD 字节变化：关解码器、`epoch++`、等关键帧、再 `accept_csd`/`ensure_decoder`。相同 CSD 不升 epoch。configure 失败返回可诊断错误，不继续喂旧解码器。宿主 `test_csd_change_reconfigure_epoch` 通过；同用例覆盖 Surface 释放后重设与重入取消。 | 中途 AU 里出现 SPS/PPS 但从未形成 `CODEC_CONFIG` 事件：仍为后续工作，不称为已完成。 |
| 探测期取消 / 不可达 | UDP listen 80ms 取消：`CANCELED` 约 **101.433ms**（不是伪装 TIMEOUT）。无发送、`io_timeout=800ms`：`TIMEOUT` 约 **805.140ms**，`probe_incomplete=0`，有界，不挂死。 | 不是设备上的取消验收。 |
| 真实长 GOP >1 秒 | 用系统 ffmpeg 生成 25fps、`keyint=50`（2s GOP）、`repeat-headers=1` 的 6s TS，`-re -stream_loop` 发送，中途加入，FAST 打开，耐心泵直到首个可解帧。本轮 `request_e2e decodable=2791.087ms submit=2791.105ms`（断言 ≥1000ms）。不是只人工喂 AU 的假计时。 | mock，display=0，不是出画，也不是「长 GOP 也能 1 秒起播」。 |
| DEFAULT 稳健模式 | `test_ff_backend` 仍走官方默认探测，全矩阵通过（含 TLS、HLS/DASH、UDP/RTP 合同、RTSP TCP/UDP、RTMP）。库默认仍为 `MS_FF_PROBE_DEFAULT`。 | 不是把 FAST 设成库默认。 |
| `nativeDestroy` 回收 | 先取消 I/O 并丢弃 pending（`exit_req` 不再处理已排队会话）。UI 对完成信号最多等 2 秒：已结束则 join 并释放自有对象；未结束则 `abandoned=1`、`pthread_detach`，**不 free 仍在跑的上下文**，由工作线程退出时自释。禁止超时后直接 free（UAF），禁止 UI 无界 join，也不在 UI 侧堆积已放弃却被提前释放的对象。宿主 `test_reap_join_and_abandon` 覆盖快速 join 与放弃路径（ASan）。 | 若底层 I/O 无法中断，放弃的线程可能存活到进程退出。这是已写明的限制，**未在真机验证**。不是线程池重构。 |

本轮同条件直播数字（2026-11-21 17:39 CST，CLOCK_MONOTONIC，宿主 mock，**display=0**）：

| 条件 | 守恒 in/sub/drop_nokey | open_input | find_info | request_e2e submit |
| --- | --- | --- | --- | --- |
| UDP-DEFAULT mid-GOP | 25/15/10 | 261.938ms | 3757.631ms | 4020.069ms |
| UDP-FAST mid-GOP（`probe_incomplete=1`） | 25/17/8 | 186.305ms | 585.068ms | 771.741ms |
| RTP-FAST mid-GOP（`probe_incomplete=1`） | 25/13/12 | 0.652ms | 536.620ms | 537.601ms |
| UDP-FAST from-start（注入 250ms） | 25/25/0 | 311.174ms | 99.555ms | 412.977ms |
| 长 GOP 2s mid-join FAST | 1/1/0（停在首个 submit） | 232.955ms | 519.471ms | 2791.105ms |
| HTTP-TS / HTTP-FLV FAST（本机 VOD 速率，非 `-re`） | 25/25/0 | 1.277 / 1.715ms | 0.736 / 0.475ms | 2.304 / 2.473ms |

### 14.2 明确未做 / 不得称为完成

- **音频输出、音画同步未实现。** 发出音频 TRACK 或拉到 AAC 包，不等于可播放声音。不得以仅视频 H264 宣称完整有声播放器。
- 公共事件没有 sample_rate/width/height 字段（故意未扩 ABI）；迟到的音频参数不能通过现有 TRACK 事件完整写出 FORMAT。
- 中途 AU 内带内 SPS 若从未形成 `CODEC_CONFIG`：未做。
- 1 秒出画、设备硬解、APK 真机/AVD 安装运行、RTMPS、el7/四平台：未验。
- 不可中断 I/O 上放弃的工作线程可能直到进程退出才结束：已记录，不是已解决。
- 其它 fallback TODO 不得写成完成。

### 14.3 回归

`./build.sh debug`（gcc 14.2.0，ASan/UBSan 已启用）。`ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 OPENSSL_CONF=/dev/null`，`MS_FF_CA=build/debug/net-ca`，`MS_FF_TMP=build/debug/ff-tmp`，`MS_FF_DASH_DIR=build/dev-samples/dash-ff`。`test_core`、`test_ts_scan`、`test_hw_backend`、`test_hw_player`、`test_ff_backend` 均 exit 0，无 sanitizer 诊断。未把 SKIP 写通过。无 `stdbuf`。真机范围明确未验。未 commit/push，未改 TLS 校验，未 UPX，未覆盖 1639/1654/1704/1716 与 `build/release`、历史 v7a size-opt `libmsff.so`。

### 14.4 本轮产物（2026-11-21 17:42 CST）

工作副本 `build/android-hw/MsHwPlayer.apk` **4,432,494** 字节，已签名，SHA-256 `352ae32552c3af3908b42a3fe7e39ade9d20d4adcec032edfcb72bc705c93eef`。`libmshw.so` **43,320**，打入的 `libmsff.so` **4,244,104**（NEEDED 无系统 libssl；由既有 size-opt v7a 前缀重链适配器，未覆盖历史 `build/android-v7a/size/libmsff.so.1.0.0` 的 4,235,076）。归档：`/workspace/MsMedia-deliver-20261121-1742/`。不覆盖 `MsMedia-deliver-20261121-1639/`、`1654/`、`1704/`、`1716/`，不覆盖 `build/release`。HEAD 仍 `1092bec`。未安装。非正式、不能当作设备验收。

## 15. AAC 音频输出与 A/V 同步（2026-11-21 18:02 CST）

本轮在 t90u「完整的要做」原授权内补齐 Android / H264+AAC 范围内的**可选音频后端、拒绝语义与宿主同步调度**，不扩大其它平台或编码，不引入 FFmpeg 软解，不放开软件 codec，不把系统软解报成硬件。宿主 ASan/UBSan 已回归；**真机出声与设备同步仍未验**。不得把宿主 mock 时钟或仅视频 H264 写成完整有声播放器。

### 15.1 支持 / 拒绝语义

| 项 | 行为 | 不是什么 |
| --- | --- | --- |
| 配置解析 | 公共加法接口 `ms_aac_asc_parse` / `ms_aac_asc_info` 复用既有 LC-AAC ASC 子集（AOT=2，sfi 0..12，声道 1–7）。只解析配置，不解压缩载荷。 | 不是解码器，不是 HE-AAC/SBR/PS。 |
| 默认硬解 only | `allow_software` 默认 0。`OMX.google.` / `c2.android.` / `c2.google.` 等仍为 `MS_HW_KIND_SOFTWARE`。系统仅有软件 AAC 时：`MS_ERR_UNSUPPORTED`，诊断写入 `audio_detail` / `audio_unsupported=1`，`audio_is_hardware=0`，`audio_system_decode_is_not_hardware=1`。视频继续（无音频时钟则立即呈现）。 | 不是悄悄改用软件 AAC。不是伪称有声。统计不得把 mock 或系统软解写成 `is_hardware`。 |
| 宿主 mock | `MS_HW_MOCK_DEFAULT` 查询非硬件，configure 拒绝假装输出。`MS_HW_MOCK_AS_SW` 且不允许软件：UNSUPPORTED。`MS_HW_MOCK_AS_HW` 可跑生命周期，但 `is_hardware` 仍为 0。按 1024 采样/帧记账 PCM 时长，不解码 AAC 载荷。`add_write_quota` 模拟 AudioTrack 部分写入（设备路径忽略）。 | 不是设备扬声器，不是硬件 AAC。 |
| Android 路径 | 兼容层为 MediaCodec（mime `audio/mp4a-latm`）+ AudioTrack JNI。无真正硬件 AAC 时允许明确拒绝。时钟拟用 `AudioTrack.getPlaybackHeadPosition()`，不用 write 字节。 | **未在真机或 AVD 运行**。Android 上「系统解码」不一定是硬件；不得把该路径默认写成硬件解码。 |
| 播放器编排 | 默认 `enable_audio=1`（旧 `struct_size` 也默认开）。先 drive 音频，再视频解码，再按时钟呈现。快速首帧立即记 `first_output`；之后 `pts > clock+80ms` 保持，`pts < clock-200ms` 记 late 并呈现，待呈现队列满则丢最旧并计数。无有效音频时钟则 fallback 立即呈现。尚无 ASC 的 AAC AU 被消费并丢弃，避免卡住音视频复用泵；CONFIG 随后到达再开音频。HE-AAC 等非 LC 配置诊断拒绝，视频可继续。不 CPU 回读 Surface。 | 宿主调度不是设备 A/V 同步验收。display 仍仅 `note_display`，无证据则未发生。 |
| 时钟 | 累计**已播放**采样：`clock_us += 1024*1e6/rate`（按播放当时的采样率）。flush 清零。不得用已排队字节当时钟。 | 不是 AudioTrack 缓冲估计，不是 PTS 猜测。2026-11-21 18:19 CST 复核修正见 §16：设备路径按无符号 32 位播放头（含回绕）；部分 write 为 PCM 字节 leftover；flush 先 pause 再归零。宿主 mock 仍按帧记账，不是设备时钟。 |
| 放弃工作线程上限 | 进程内 `g_abandoned_live`，帽 `MS_HW_ABANDONED_WORKER_CAP=2`。`ms_hw_abandoned_worker_count` / `_cap` / `ms_hw_can_spawn_worker`。`nativeCreate` / `ensure_worker` 在满帽时拒绝新建（返回 0 / -1）并给出诊断。detach 仍不能单独解决增长。宿主 `test_abandoned_worker_cap` 覆盖满帽拒绝与回收后再允许。 | 若底层 I/O 无法中断，放弃的线程仍可能活到进程退出。未真机验证。不是线程池。 |

### 15.2 宿主实测层次（2026-11-21 17:56–17:59 CST，CLOCK_MONOTONIC，mock，display=0）

- `test_hw_backend`：ASC 44.1k/48k 解析；拒绝 DEFAULT/软件 AAC；生命周期含配置、PCM 格式变化、有界背压、部分 write、EOS、flush、取消；时钟在排队时为 0，drive 后为已播放帧时长（1024/44100 s）。
- `test_hw_player`：音频配置后时钟来自已播放帧；快速首帧；early 保持；late 计数；HE-AAC 拒绝且视频仍有 `have_first_output`；放弃线程帽=2 拒绝新建后可回收。既有视频守恒、CSD/epoch、FAST 文件/直播与真实 2s GOP mid-join 仍通过：本轮长 GOP `request_e2e decodable=2782.662ms submit=2782.678ms`（≥1000ms）。UDP-FAST mid-GOP 25/17/8，submit=713.762ms，`probe_incomplete=1`。display=0。
- `test_ff_backend` DEFAULT 全矩阵通过（RTMP events=36，h264_au=11，aac_au=20）。`test_core`、`test_ts_scan` 通过。无 sanitizer 诊断。未把 SKIP 写通过。

### 15.3 明确未做 / 不得称为完成

- **真机 AAC 输出与设备 A/V 同步未验证。** 本阶段没有实机或共享 AVD 出声证据。许多 Android 设备只有系统软件 AAC，按既定硬解 only 将被明确拒绝；接口与诊断已就绪，**不得自行放开政策**，也不得因此伪称有声。
- 宿主 mock 按帧记账不等于扬声器已播放，也不等于 MediaCodec 已输出 PCM。
- 公共 `ms_event`/`ms_track_info` 仍无 sample_rate/width/height（故意未扩 ABI）。
- 中途 AU 内 SPS/PPS 若从未形成 `CODEC_CONFIG`：18:02 时仍为后续工作；已在 2026-11-21 18:19 CST 的 §16 由统一播放器入口补齐（宿主 mock，非设备硬解）。
- 1 秒出画、设备硬解、APK 真机/AVD 安装运行、RTMPS、el7/四平台：未验。
- 不可中断 I/O 上放弃的工作线程可能直到进程退出才结束：已记录上限与拒绝语义，不是已在设备上解决。

### 15.4 回归与产物（2026-11-21 18:02 CST）

`./build.sh debug`（gcc 14.2.0，ASan/UBSan）。`OPENSSL_CONF=/dev/null`，`MS_FF_CA=build/debug/net-ca`，`MS_FF_TMP=build/debug/ff-tmp`，`MS_FF_DASH_DIR=build/dev-samples/dash-ff`。`test_core`、`test_ts_scan`、`test_hw_backend`、`test_hw_player`、`test_ff_backend` 均曾在本阶段 exit 0。非正式 `./build.sh android-hw`（NDK r29 API 21 v7a）由既有 size-opt v7a 前缀重链适配器。工作副本 `build/android-hw/MsHwPlayer.apk` **4,448,878** 字节，已签名，SHA-256 `d3448c333a0658e9d928ef46f9d7094ff44156d97659f05c23fecc3a24e9d06a`。`libmshw.so` **57,132**，打入的 `libmsff.so` **4,244,104**（NEEDED 无系统 libssl）。归档：`/workspace/MsMedia-deliver-20261121-1802/`。不覆盖 `MsMedia-deliver-20261121-1639/`、`1654/`、`1704/`、`1716/`、`1742/`，不覆盖 `build/release` 与历史 `build/android-v7a/size/libmsff.so.1.0.0`（4,235,076）。HEAD 仍 `1092bec`。未安装。非正式、不能当作设备出声或出画验收。

## 16. 中途 AU 内 SPS/PPS（无独立 CODEC_CONFIG）与音频时钟复核（2026-11-21 18:19 CST）

本轮在 t90u「完整的要做」原授权内补齐报告一直保留的缺口：AU 内出现 SPS/PPS 变化，但上游没有独立 `CODEC_CONFIG`。不扩大其它平台或编码，不追加公共 `sample_rate`/`width` 字段，不在 `libmsff` 再做一条提取路径（`libmsff.so` 不得 NEEDED `libmsmedia`）。统一由 `ms_hw_player` 入口安全处理。宿主 ASan/UBSan 已回归；**真机硬解、出画、出声仍未验**。不得把宿主 mock 写成设备出画或完整有声播放器。

### 16.1 带内参数集：支持 / 拒绝 / 等待

| 项 | 行为 | 不是什么 |
| --- | --- | --- |
| 公共解析 | 加法接口 `ms_h264_ps_parse` / `ms_h264_ps_equal` / `ms_h264_ps_pack`。有界：单 NAL ≤ `MS_H264_PS_NAL_MAX`（2048），同类型槽 ≤ `MS_H264_PS_SLOT_MAX`（4）。复用既有 Annex B 起始码、AVCC 长度前缀样本、avcC 配置盒，以及已有 `parse_sps`/`parse_pps`。`is_csd=1` 为配置盒或纯参数集 CSD；`0` 为 AU（AVCC=长度前缀，`nal_len_size=0` 视为 4）。 | 不是第二套 NAL 扫描器，不是 HEVC。无法解析的 type 7/8 忽略，不应用。 |
| 配对规则 | 只把**同一缓冲**里的 SPS 与引用该 `sps_id` 的 PPS 配成对（多枚时从后向前选能配上的最后一枚 PPS）。新 SPS 丢弃不匹配的 pending PPS，**禁止新 SPS + 旧已应用 PPS**。仅 PPS 更新可与**已应用且 id 匹配**的 SPS 配对。 | 不是跨 AU 胡乱拼接。 |
| 不完整 / 畸形 / 超限 | 新参数集尚未成对：消费并丢弃该 AU（`video_au_dropped_nokey++`，不返回 `AGAIN` 以免卡住复用泵），等待最多 `MS_HW_CSD_PENDING_AU_MAX`（16）个后续 AU；超限则 `video_csd_pending_abandoned++` 并放弃 pending，不应用半配置。AVCC 长度畸形：不应用配置，已配置则仍可尝试提交本 AU。单个 PS NAL 超 2048：`MS_ERR_OVERFLOW`。 | 不是无限等待，不是静默用旧 PPS 凑对。 |
| 配置变化 | 字节级不同的成对 CSD：关解码器、`epoch++`、等新配置可解关键帧（可含携带新成对参数集的同一 IDR）。相同 NAL 只计 `video_csd_inband_same`，不升 epoch、不重建。官方 `CODEC_CONFIG` 仍按原始 CSD 字节 `memcmp` 决定是否换 epoch（占位 avcC 不因 parse 失败误伤）。flush 只清 pending，保留已应用 CSD。 | 不是每次 IDR 的 `repeat-headers` 都重建解码器。 |
| 路径 | 合成 Annex B / AVCC 样本，以及受控真实 TS / FLV（跳过独立 CONFIG）均覆盖初始化与中途变化。背压 `LIMIT` 后重试不得丢失已见到的配置。 | 宿主 mock 提交，不是设备硬解或出画。未在 `libmsff` 复制提取器。 |

宿主实测（2026-11-21 18:16 CST，CLOCK_MONOTONIC，mock，display=0）：

- `test_h264_ps_parse_unit`：Annex B 与 avcC 往返；AVCC 长度前缀样本成对；同缓冲 SPS id=1 与引用 sps 0 的 PPS 不得 `pair_complete`；仅 SPS 不完整；短 AVCC 样本 `MS_ERR_MALFORMED`。
- `test_inband_csd_without_config`：无独立 CONFIG 的 Annex B 初始化与中途变化；相同 CSD 不升 epoch；仅新 SPS 后跟无参数集非关键 AU，有界放弃且 epoch 不变；AVCC 样本初始化+变化；`max_pending_in=1` 时 LIMIT 再 drive/重试，配置不丢失。
- 真实受控：两段 `repeat-headers=1` 的 320×240 与 640×360 TS 直接拼接，泵入时故意跳过 `CODEC_CONFIG`：TS `in=48 submitted=48 inband=2 same=0 epoch=2`（epoch 初值 1，一次真实变化到 2）；同流 `-c copy` 转 FLV：`in=50 submitted=50 inband=2 same=0 epoch=2`。守恒成立。
- 既有矩阵仍过：FAST 长 GOP mid-join `request_e2e decodable=2806.990ms submit=2807.005ms`（≥1000ms）；UDP-FAST mid-GOP 25/17/8 submit=713.118ms；UDP-DEFAULT mid-GOP 25/16/9 submit=4050.261ms；RTP-FAST mid-GOP 25/13/12 submit=534.613ms。`test_ff_backend` DEFAULT 全过（RTMP events=36）。display=0。

### 16.2 音频时钟复核（修明确 bug，不扩大范围）

- **设备时钟**必须来自 `AudioTrack.getPlaybackHeadPosition()`，按无符号 32 位计数（JNI `jint` 需零扩展）。`raw` 相对上次减小则 `head_wraps++`，已播放帧为 `(head_wraps<<32)+raw`。不得把宿主 mock 按 1024 采样/帧的推算当作 Android 时钟。
- `AudioTrack.write` 的返回值是 **PCM 字节**（16-bit × 声道 × 采样），不是压缩 AU 字节。部分写入的剩余拷入有界 leftover（64 KiB），下一轮 drive 续写；不得在 `releaseOutputBuffer` 之后丢掉未写完的 PCM。
- `ms_hw_audio_add_write_quota` 的单位同样是 PCM 字节，且**仅宿主 mock 有效**；设备路径忽略。
- `flush`：清空 leftover，复位 wrap/base；Android 上先 `pause`，再 `AudioTrack.flush()`，再 `play`，使播放头归零；并 `AMediaCodec_flush`。
- 无硬件 AAC 的拒绝语义不变：系统仅软解 → `MS_ERR_UNSUPPORTED`，`audio_is_hardware=0`，`audio_system_decode_is_not_hardware=1`，视频 fallback。不放开硬解 only，不把系统软解写成硬件。
- 以上为代码审查与宿主编译；**未在真机观察播放头回绕或 flush 归零，也没有实听证据**。

### 16.3 明确未做 / 不得称为完成

- **真机/AVD 硬件解码、Surface 出画、1 秒首帧、音频实听：未验证。** 未尝试共享 AVD（`CJZB_API34`、`CJZB_ARM64`）。
- 许多 Android 设备可能只有系统软件 AAC，按政策将明确拒绝并保持无声视频。宿主 mock 不是扬声器。
- 在 16 个 AU 的有界等待内始终形不成配套 SPS+PPS：放弃 pending，不半配置；这是规定行为，不是未修的静默拼装。
- 公共 `ms_event`/`ms_track_info` 仍无 `sample_rate`/`width`（本轮故意不加，以免扩大 ABI）。
- RTMPS、el7/四平台正式验收、APK 真机/AVD 安装运行：未验。
- 不可中断 I/O 上放弃的工作线程仍可能活到进程退出。

### 16.4 回归与产物（2026-11-21 18:19 CST）

`./build.sh debug`（gcc 14.2.0，ASan/UBSan）。`OPENSSL_CONF=/dev/null`，`MS_FF_CA=build/debug/net-ca`，`MS_FF_TMP=build/debug/ff-tmp`，`MS_FF_DASH_DIR=build/dev-samples/dash-ff`。`test_core`、`test_ts_scan`、`test_hw_backend`、`test_hw_player`、`test_ff_backend`、`test_net` 均 exit 0，无 sanitizer 诊断。未把 SKIP 写通过。无 `stdbuf`。非正式 `./build.sh android-hw`（NDK r29 API 21 v7a）由既有 size-opt v7a 前缀重链适配器。工作副本 `build/android-hw/MsHwPlayer.apk` **4,452,974** 字节，已签名，SHA-256 `ad9efc68e69e3913698b45586c8646202bdc8c523227e33f141746727e6c8a09`。`libmshw.so` **60,024**，打入的 `libmsff.so` **4,244,104**（NEEDED 无系统 libssl；未覆盖历史 `build/android-v7a/size/libmsff.so.1.0.0` 的 4,235,076）。归档：`/workspace/MsMedia-deliver-20261121-1819/`。不覆盖 `MsMedia-deliver-20261121-1639/`、`1654/`、`1704/`、`1716/`、`1742/`、`1802/`，不覆盖 `build/release`。HEAD 仍 `1092bec`。未安装。非正式、不能当作设备硬解、出画或出声验收。

## 17. 系统 AAC 有界例外、非正式 arm64-v8a 与受控 RTMPS（2026-11-21 18:36 CST）

本轮在「完整播放需要声音」且应对齐 Media3 常用路径的前提下，增加**仅针对 AAC** 的系统 MediaCodec（常为软件分类）+ AudioTrack 有界例外。视频硬解 only 不变，继续拒绝 `OMX.google` / `c2.android` 等视频软解，且仍禁止 FFmpeg 软解（`CONFIG_DECODERS=0`）。必须用明确选项/枚举，不能把 `allow_software` 悄悄变成全面软解。统计必须诚实。另做非正式 `arm64-v8a` 交叉编译（复用已有 size-opt 前缀），以及在既有本地 CA 上的受控 RTMPS 宿主验证。宿主 ASan/UBSan 已回归；**这不是放开视频软解，也不是真机已出声或已出画。**

### 17.1 策略语义

| 项 | 行为 | 不是什么 |
| --- | --- | --- |
| 库默认 | `ms_hw_aac_policy` 的 `MS_HW_AAC_POLICY_HARDWARE_ONLY=0`。`ms_hw_player_opts_default` 将 `allow_system_aac=0`；较小的旧 `struct_size` 经前缀 memcpy 后仍落在该默认。`ms_hw_audio_set_aac_policy` 须在 configure 之前，configure 之后为 `MS_ERR_STATE`。 | 不是默认放开系统软解。不是改 ABI 版本（仍为 1）。 |
| 显式允许 | `allow_system_aac=1`，或 `ms_hw_audio_set_aac_policy(..., MS_HW_AAC_POLICY_ALLOW_SYSTEM)`。只对 AAC 选用系统 MediaCodec。系统软件名仅 `omx.google.` / `c2.android.` / `c2.google.`（`ms_hw_is_system_software_codec_name`）。成功时状态/详情为「系统 AAC，非硬件」，`audio_is_hardware=0`，`audio_system_decode_is_not_hardware=1`。 | 不是 `allow_software` 开关。不是把系统软解写成硬件。 |
| 仍拒绝 | 视频软解（即使 `allow_system_aac=1` 且 `allow_software=0`）；非系统第三方软解（如 `omx.ffmpeg.`，宿主 `MS_HW_MOCK_AS_FOREIGN_SW`）；`MS_HW_MOCK_DEFAULT` 即便允许系统 AAC 也不假装输出；HE-AAC 仍 `UNSUPPORTED`；FFmpeg 视频/音频软解仍关闭。 | 不是全面软件解码。 |
| 测试 APK | `nativeCreate` 显式设 `allow_system_aac=1`、`decoder.allow_software=0`。界面与 `format_stats` 写明「系统 AAC，非硬件」，并注明这不是设备扬声器已出声的声明。意图是典型无硬件 AAC 的设备上**应能**走系统解码出声。 | 不是已在真机实听。库默认策略未被 APK 改写。 |
| ABI 加法 | 新字段只追加在 `ms_hw_player_opts` 末尾（`allow_system_aac`、`audio_mock_mode`，后者 -1 表示跟随 `decoder.mock_mode`）。**不**扩大嵌在中间的 `ms_hw_create_opts`，以免旧 `struct_size` 错位。 | 不是把政策塞进 `allow_software`。未加公共 `sample_rate`/`width`。 |

`audio_is_hardware` 仅在真正硬件解码时为 1。系统软解必须保留 `audio_system_decode_is_not_hardware=1`，并有可诊断状态。宿主 mock 覆盖：默认拒绝、显式允许系统 AAC 的生命周期/背压/flush/时钟、仍拒绝非系统/非 AAC 软解、无音频时视频 fallback。mock 的 `output_is_not_display` 仍为 1。

### 17.2 非正式 arm64-v8a 与双 ABI APK

复用已有 size-opt 流程与前缀：`scripts/android_v8a_size.sh` 产物在 `build/android-v8a/`（`ffmpeg-9.1-size` / `openssl-3.5.9-size` / `libxml2-2.13.8`）。`scripts/android_hw_build.sh` 从这些前缀重链当前适配器，不重编 OpenSSL/FFmpeg，**不覆盖** `build/android-v7a/size`、`build/android-v8a/size` 或 `build/release`。测试 APK 同时打入 `armeabi-v7a` 与 `arm64-v8a`。NDK r29 API 21，非正式，不是四平台验收。

本轮实测（2026-11-21 18:35 CST，Asia/Shanghai）：

| 产物 | 去符号体积（字节） | NEEDED / 说明 |
| --- | ---: | --- |
| 历史 v7a size-opt `build/android-v7a/size/libmsff.so.1.0.0` | 4,235,076 | 保持不动（mtime 2026-11-21 12:17:30 +0800，SHA-256 `7a75709520958a1f2ac5c679fad2e4cd3f899c1d10f507d0120cb011ff8f880b`） |
| 打入 APK 的 v7a `libmsff.so` | 4,244,104 | 与既有 in-APK 基线一致；`libz`/`libm`/`libdl`/`libc`；SONAME `libmsff.so`；无系统 libssl/libcrypto，无 libmsmedia |
| 打入 APK 的 v8a `libmsff.so` | 5,250,888 | 同 NEEDED/SONAME 约束；unstripped 12,602,048。相对 v7a 4,244,104 的差额来自 arm64 码与数据布局，不是又编了一遍依赖 |
| v7a / v8a `libmsmedia.so` | 130,036 / 132,344 | NEEDED `libc`/`libm`/`libdl` |
| v7a / v8a `libmshw.so` | 62,112 / 71,480 | NEEDED `libmsff`/`libmsmedia`/`libmediandk`/`libandroid`/`liblog`/`libc`/`libdl` |
| `MsHwPlayer.apk`（双 ABI，已签名） | 9,921,347 | SHA-256 `0963764b63ad651f216873fe6fee7ea9b9a59a540bee53fd14ca65c0dd853ce9`。未安装。 |

v8a 大于 v7a 是预期的 ABI 差异，不得把该对照写成回归或优化失败。

### 17.3 受控 RTMPS（宿主结果层次）

未关闭 TLS 校验，也未关闭主机名校验。测试专用 `tests/rtmps_tls_forward.py` 在本机 TLS 监听，把明文转到 `ffmpeg -listen` RTMP；**不是产品 RTMPS 服务器**。证书用既有 `build/debug/net-ca`（`server.crt` SAN 含 DNS:localhost 与 IP:127.0.0.1；`wrong.crt` 为同一 CA，CN/SAN=wrong.example）。客户端 `ms_ff_set_ca_file` + `ms_ff_set_allow_system_trust(f,0)`。`ms_ff_url_needs_tls_trust("rtmps://...")` 为 1。

本轮 `test_ff_backend`（2026-11-21 18:35 CST，ASan/UBSan）结果层次：

- **编入**：是（FFmpeg rtmps + 已静态钉选的 OpenSSL 3.5.9）。不得仅因编入或 HTTPS 已过而声称 RTMPS 已验收。
- **宿主受控闭环**：是（本轮）。无 CA：`open=UNSUPPORTED`（校验保持开）。成功拉流：`rtmps://localhost` + 本地 CA，`format=rtmp events=36 h264_au=11 aac_au=20 cfg_v=1 cfg_a=1`，与同样本明文 RTMP 计数一致。主机名不匹配：`open=UNKNOWN`（非 `MS_OK`；错误码映射为 UNKNOWN，不是把失败写成通过）。
- **缺依赖**：若缺 ffmpeg、`short.flv`、本地 CA 或转发脚本，测试打印缺口且**不记通过**。本轮上述依赖齐全，不是 SKIP。
- **未测**：RTMPT、公网/非本机 RTMPS、设备上的 RTMPS、SRTP、AAC-over-RTSP。

### 17.4 明确未做 / 不得称为完成

- **真机/AVD 硬件解码、Surface 出画、1 秒首帧、扬声器实听：未验证。** 未尝试共享 AVD（`CJZB_API34`、`CJZB_ARM64`），未碰用户电脑。不得把宿主 mock、软件模拟器或「系统 AAC」状态行写成已出画或已出声。
- 库默认在仅有系统软件 AAC 的设备上仍拒绝音频并视频 fallback；只有测试 APK（或调用方显式 `allow_system_aac=1`）才会走系统 AAC 路径，而该路径尚未在设备上听到声音。
- el7 / 四平台正式验收、APK 真机/AVD 安装运行：未验。
- 不可中断 I/O 上放弃的工作线程仍可能活到进程退出。

### 17.5 回归与产物（2026-11-21 18:36 CST）

`./build.sh debug`（gcc 14.2.0，ASan/UBSan）。`OPENSSL_CONF=/dev/null`，`MS_FF_CA=build/debug/net-ca`，`MS_FF_TMP=build/debug/ff-tmp`，`MS_FF_DASH_DIR=build/dev-samples/dash-ff`。`test_core`、`test_ts_scan`、`test_hw_backend`、`test_hw_player`（含 `test_system_aac_policy`、中途 CSD TS/FLV、FAST 长 GOP mid-join `request_e2e submit=2794.318ms`、UDP-FAST mid-GOP 25/17/8 submit=723.643ms、UDP-DEFAULT mid-GOP 25/16/9 submit=4047.610ms）、`test_ff_backend`（含受控 RTMPS）、`test_net build/debug/net-ca` 均 exit 0，无 sanitizer 诊断。未把 SKIP 写通过。无 `stdbuf`。非正式 `./build.sh android-hw`（NDK r29 API 21，v7a+v8a）由既有 size-opt 前缀重链适配器。工作副本 `build/android-hw/MsHwPlayer.apk` **9,921,347** 字节，已签名，SHA-256 `0963764b63ad651f216873fe6fee7ea9b9a59a540bee53fd14ca65c0dd853ce9`。归档目录见打包时刻的 `/workspace/MsMedia-deliver-20261121-1838/`。不覆盖 `MsMedia-deliver-20261121-1639/`、`1654/`、`1704/`、`1716/`、`1742/`、`1802/`、`1819/`，不覆盖 `build/release` 与历史 `build/android-v7a/size/libmsff.so.1.0.0`。HEAD 仍 `1092bec`。未安装。非正式、不能当作设备硬解、出画或出声验收。

## 18. P4 音轨选择与文本字幕显示层（2026-11-21 20:10 CST）

本切片把同容器多音轨枚举/选择/播放中切换，以及文本字幕枚举/选择/关闭与有界 cue 时间轴接到既有 `ms_hw_player`、Android JNI 和测试 Activity。视频硬解 only 与「系统 AAC，非硬件」策略不变。`CONFIG_DECODERS=0` 不变。宿主 mock 与 TextView 赋值**不是**设备出声或上屏字幕验收。

### 18.1 播放器与 JNI

- 加法 API：`ms_hw_player_list_tracks` / `select_track` / `set_external_subtitle` / `subtitles_at` / `last_media_90k`。统计尾部增加 `selection_epoch`、`audio_track_id`、`sub_track_id`、`audio_switch_count` 等；旧 `struct_size` 仍可用。
- 切音轨：`reset_audio_session`（flush+destroy，丢弃 ASC），等待新轨 `CODEC_CONFIG` 后 `ensure_audio` 重建；未选中 AU 计入 `audio_au_dropped_unselected`。关闭全部音轨走 `sync_fallback_no_audio`。
- 字幕：选中的内嵌文本进入有界 inband doc；flush / release_decoder / 关闭字幕会 `ms_sub_doc_clear`。外挂默认不自动选中。位图/ASS 选择播放返回 `UNSUPPORTED`。
- JNI（`op` 锁）：`nativeListTracks`、`nativeSelectTrack`（先源 `ms_ff_select_track` 再播放器）、`nativeCurrentCue`（按最近媒体 90 kHz 查询）、`nativeLoadSubtitle`（字节，不记录路径）。Activity：Surface 上叠加 `cueView`，以及 Tracks / Next Audio / Next Sub / Load SRT / Sub Off。轮询刷新 cue。不把 URL 或字幕路径写入日志。

### 18.2 内部测试 APK 体积（非正式，未归档）

由既有 size-opt 前缀重链，不覆盖历史 v7a size-opt。本轮因 `libmsff` 隐藏编入同一份 `ms_sub.c`，打入 APK 的 so 比 1838 略大。

| 产物 | 去符号体积（字节） | 说明 |
| --- | ---: | --- |
| 历史 v7a size-opt `build/android-v7a/size/libmsff.so.1.0.0` | 4,235,076 | 保持不动 |
| 打入 APK 的 v7a / v8a `libmsff.so` | 4,257,688 / 5,261,344 | NEEDED 仅 libz/libm/libdl/libc；无系统 libssl，无 libmsmedia |
| v7a / v8a `libmsmedia.so` | 140,928 / 144,664 | NEEDED libc/libm/libdl |
| v7a / v8a `libmshw.so` | 69,868 / 80,528 | 含 JNI 与播放器切轨/字幕 |
| 内部 `build/android-hw/MsHwPlayer.apk` | 9,990,979 | SHA-256 `24f5a101bacd27e836dc7e122f88eecff7b94b18932581a6bb7e01b2787eeea3`。已签名，**未安装**。不是新的公开交付包。 |

### 18.3 明确未做

真机/AVD 上的切轨实听、字幕是否真正画在画面上、系统 AAC 出声、1 秒首帧：未验证。未启动共享 AVD，未碰用户电脑。P4 其余编码、HLS 备选 rendition、位图/ASS 渲染与 P5/四平台仍待后续块。倍速/seek 的代码路径见 §19，仍非设备验收。

## 19. P4 倍速与相对/绝对 seek（2026-11-21 20:25 CST）

加法 ABI（`MSMEDIA_HW_ABI` 仍为 1，旧 `struct_size` 截断拷贝可用）：`ms_hw_player` / `ms_hw_audio` 的 `set_rate`、`get_rate_caps`、`pause`/`resume`、`seek`/`seek_relative`、`note_timeline`、position/duration/seekable。`ms_ff_seek` 使用现有 demux seek，不启用 FFmpeg 解码。

能力必须诚实：宿主 mock 可记账但 `can_sync_av=0` 且 `mock_is_not_device=1`；Android API 23+ 走 `PlaybackParams`（保音调，宣称 1/4–2×）；更旧 API 仅变调 `setPlaybackRate`（约至 2×）。超范围或套用失败返回 `UNSUPPORTED`，不得只改视频时钟称为音画同步倍速。负速率不支持。JNI 相对/绝对 seek 入队到泵循环，不在 UI 线程执行；PlayPath 在链入 `libmsff` 时使用 `file:` 以便可定位。测试 Activity 增加 Pause/Resume/RateCaps/0.5x/1x/1.5x/2x/−10s/+10s/Pos。

内部测试 APK（本轮重建，不是新的公开交付目录）：

| 产物 | 大小（字节） | 备注 |
| --- | --- | --- |
| 打入 APK 的 v7a / v8a `libmsff.so` | 4,258,944 / 5,262,872 | NEEDED 仅 libz/libm/libdl/libc；无系统 libssl，无 libmsmedia |
| v7a / v8a `libmsmedia.so` | 140,928 / 144,664 | NEEDED libc/libm/libdl |
| v7a / v8a `libmshw.so` | 80,296 / 91,792 | 含倍速/seek JNI |
| 内部 `build/android-hw/MsHwPlayer.apk` | 10,011,459 | SHA-256 `0cd7957223593c3a2e5ae9e45fd8585d590f8e202ffbbba2f8d07656dfb80f39`。已签名，**未安装**。 |

历史 1639–1838 与 `build/android-v7a/size/libmsff.so.1.0.0`（4,235,076，mtime 2026-11-21 12:17:30 +0800）未覆盖。不是设备听感或出画验收。

## 20. P4 HEVC 配置与提交路径（2026-11-21 21:10 CST）

宿主已贯通：公共 `ms_hevc_*`、FFmpeg MP4/TS 规范 AU/CSD、中途 VPS/SPS/PPS、seek 关键帧门控、mock 配置/提交。Android NDK `AMediaCodec` 与 API 16–20 JNI 均按 `video/hevc` 选择与 configure（csd-0 为带起始码的 VPS+SPS+PPS，csd-1 空）；无匹配硬件时诚实 `UNSUPPORTED` / `kind=NONE`，不伪装。系统 AAC 例外不扩展到其它音频。`CONFIG_DECODERS=0`。

非正式 `./build.sh android-hw`（由重建后的 size-opt FFmpeg 前缀重链，不覆盖 `build/android-v7a/size` 历史产物）：

| 产物 | 大小（字节） | 备注 |
| --- | --- | --- |
| 打入 APK 的 v7a / v8a `libmsff.so` | 4,326,832 / 5,326,712 | 含 HEVC parser/BSF；NEEDED 仅 libz/libm/libdl/libc；无系统 libssl，无 libmsmedia |
| v7a / v8a `libmsmedia.so` | 146,420 / 150,520 | 含 `ms_hevc.c` |
| v7a / v8a `libmshw.so` | 84,280 / 95,856 | 含 HEVC CSD 拆分与 MIME 选择 |
| 内部 `build/android-hw/MsHwPlayer.apk` | 10,163,011 | SHA-256 `908db00b6f8ef4caaff9545ffdf1aa1415c72bf6f3053cfc8592dfb055692ecb`。已签名，**未安装**。 |

历史 `build/android-v7a/size/libmsff.so.1.0.0` 仍为 4,235,076，SHA-256 `7a75709520958a1f2ac5c679fad2e4cd3f899c1d10f507d0120cb011ff8f880b`。不是设备硬解或出画验收。

## 21. P4 HLS 备选 AUDIO rendition（2026-11-21 21:29 CST）

`libmsff` 复用官方 HLS rendition：master `EXT-X-MEDIA` AUDIO 外链可枚举、默认选择、播放中切换/关闭/再启。JNI 选择异步入队，网络在 worker 的 `ms_ff_pull`，不在 UI 线程做 I/O。播放器在新轨提交后才重置音频会话并重配 CSD，视频 epoch 不因切音轨增加。系统 AAC 例外不扩展。自写 `ms_hls` 的 master 仍为拒绝。

非正式 `./build.sh android-hw`（重链适配器，未改 FFmpeg size-opt 前缀，不覆盖历史 `build/android-v7a/size`）：

| 产物 | 大小（字节） | 备注 |
| --- | --- | --- |
| 打入 APK 的 v7a / v8a `libmsff.so` | 4,328,516 / 5,328,272 | NEEDED 仅 libz/libm/libdl/libc |
| v7a / v8a `libmsmedia.so` | 146,420 / 150,520 | 与上一切片相同量级 |
| v7a / v8a `libmshw.so` | 85,020 / 96,728 | 含异步 select |
| 内部 `build/android-hw/MsHwPlayer.apk` | 10,163,011 | SHA-256 `f8fb3f7485e5ba10cc83a6eacc8522c206bb5a51570401945876f41f4a7dd2df`。已签名，**未安装**。 |

不是设备出声或切轨听感验收。

## 22. P4 ASS/SSA 文本布局模型（2026-11-21 21:49 CST）

本切片把外挂 ASS/SSA 与容器内嵌事件接入既有 `ms_sub` / `ms_hw_player` 时间轴，并在测试 Activity 用多层 TextView 应用已解析的一般样式（颜色、字号、粗斜体、对齐、边距、位置、layer）。覆盖标签不再作为正文显示。这是**编译 + 逻辑接入**，不是设备出画或视觉验收，也不是完整 ASS 渲染（无字体引擎、无动画/绘图合成）。位图字幕仍 `UNSUPPORTED`。`CONFIG_DECODERS=0`。未引入 libass，未改顶层许可证。

非正式 `./build.sh android-hw`（重链适配器，未改 FFmpeg size-opt 前缀，不覆盖历史 `build/android-v7a/size`）：

| 产物 | 大小（字节） | 备注 |
| --- | --- | --- |
| 打入 APK 的 v7a / v8a `libmsff.so` | 4,346,244 / 5,344,800 | NEEDED 仅 libz/libm/libdl/libc |
| v7a / v8a `libmsmedia.so` | 164,760 / 167,808 | 含 ASS 有界解析 |
| v7a / v8a `libmshw.so` | 85,740 / 97,496 | 含 `nativeFillCues` |
| 内部 `build/android-hw/MsHwPlayer.apk` | 10,236,739 | SHA-256 `497196215dedd97e027fe060ba4dc652c2c6f790319ea0285c4d13ea78425531`。已签名，**未安装**。 |

历史 `build/android-v7a/size/libmsff.so.1.0.0` 仍为 4,235,076，SHA-256 `7a75709520958a1f2ac5c679fad2e4cd3f899c1d10f507d0120cb011ff8f880b`。不是设备字幕上屏验收。

## 23. P4 MPEG-2 Video 与 MPEG-4 Part 2（2026-11-21 22:16 CST）

本切片把 MPEG-2 Video 与 MPEG-4 Part 2 接到既有统一播放器与 Android 硬解查询/配置/提交路径，而不是只加枚举或 MIME。公共加法接口为 `ms_video_*`（配置解析、相等/签名、AU 检查、关键帧判断），H.264/HEVC 仍走既有 `ms_h264_*`/`ms_hevc_*`。产品 FFmpeg 仍 `CONFIG_DECODERS=0`、`CONFIG_GPL=0`、`CONFIG_NONFREE=0`，视频不得软解回退。无硬件时查询/配置须诚实 `UNSUPPORTED`。宿主 mock 不是设备硬解或出画。测试用 `/usr/bin/ffmpeg` 的 `mpeg2video`/`mpeg4` **编码器**生成受控样本，不等于产品允许解码。自写 TS 对 `0x02`/`0x10` 仍为容器标识。H.263 本轮未接线。

非正式 `./build.sh android-hw`（已按需重建 Android 两 ABI size-opt FFmpeg 前缀并重链适配器；**不覆盖**历史 `build/android-v7a/size`）：

| 产物 | 大小（字节） | 备注 |
| --- | --- | --- |
| 打入 APK 的 v7a / v8a `libmsff.so` | 4,398,172 / 5,397,696 | 含 mpegvideo/mpeg4video parser；NEEDED 仅 libz/libm/libdl/libc；无系统 libssl，无 libmsmedia |
| v7a / v8a `libmsmedia.so` | 169,520 / 172,608 | 含 `ms_vbit` |
| v7a / v8a `libmshw.so` | 87,764 / 99,608 | 含 MPEG-2/4 MIME 与 CSD 提交 |
| 内部 `build/android-hw/MsHwPlayer.apk` | 10,359,619 | SHA-256 `ad5490c050e361339b3143a694a6f01e629007e3617d27b7aa97bdc4a198931c`。已签名，**未安装**。约 2026-11-21 22:16:27 CST |

历史 `build/android-v7a/size/libmsff.so.1.0.0` 仍为 4,235,076，SHA-256 `7a75709520958a1f2ac5c679fad2e4cd3f899c1d10f507d0120cb011ff8f880b`。不是设备硬解或出画验收。

## 24. P4 H.263（2026-11-21 22:34 CST）

本切片把 ITU-T H.263 接到既有 `ms_video_*` 与统一播放器/Android 硬解查询-配置-提交路径，而不是只加枚举或 MIME。配置可能只在图象头：解析 PSC/PTYPE 得到标准尺寸与 I/P，**不把图象头或未知 extradata 捏成 MediaCodec csd-0**。`ms_hw_decoder_configure` / player 在 `complete` 且宽高已知时允许空 CSD。MIME 仍为 `video/3gpp`。

已覆盖：基线短视频头 I/P，标准尺寸 SQCIF 128×96、QCIF 176×144、CIF 352×288、4CIF、16CIF；libmsff 对已开 mov/3GP（`s263`）输出完整 AU、PTS、关键帧；seek 后首视频 AU 为关键帧；截断 3GP 有界结束；player 手工 QCIF→CIF 中途尺寸变化升 epoch；mock 拒绝软件 codec 与捏造 CSD。宿主 fixture 用 `/usr/bin/ffmpeg` 的 `h263` **编码器**生成 3GP，不等于产品允许解码。该编码器的 mp4 复用器无 H.263 tag，故不用 MP4 作本切片容器证据。基线 3GP 实测 PTS==DTS（cfg=0 au=20 key=2 pts=20 inband_cfg=20），是诚实现象。

未覆盖：PLUSPTYPE/自定义尺寸、Sorenson Spark（`FLV1`）、自写 TS/AVI 组帧（`ms_codec_query(H263)` 仍为 `NONE`）。产品未追加 avi demuxer。无硬件须诚实 `UNSUPPORTED`。宿主 mock 与内部 APK **不是**设备硬解或出画。

产品 FFmpeg 只追加 `--enable-parser=h263`，`CONFIG_H263_PARSER=1`，`CONFIG_H263_DECODER=0`，`CONFIG_DECODERS=0`，`CONFIG_GPL=0`，`CONFIG_NONFREE=0`。符号表有 `ff_h263_parser`、无 `ff_h263_decoder`。

非正式 `./build.sh android-hw`（已按需重建宿主与 Android 两 ABI size-opt FFmpeg 前缀并重链适配器；**不覆盖**历史 `build/android-v7a/size`）：

| 产物 | 大小（字节） | 备注 |
| --- | --- | --- |
| 打入 APK 的 v7a / v8a `libmsff.so` | 4,399,244 / 5,398,864 | 含 h263 parser；NEEDED 仅 libz/libm/libdl/libc；无系统 libssl，无 libmsmedia |
| v7a / v8a `libmsmedia.so` | 170,132 / 173,296 | 含 H.263 PSC 解析 |
| v7a / v8a `libmshw.so` | 88,168 / 100,064 | 含空 CSD 配置与 `video/3gpp` |
| 内部 `build/android-hw/MsHwPlayer.apk` | 10,363,715 | SHA-256 `2615604a6e537fb61c256484d8912a767647fc0ba609d09d90e00f86c26ee7f1`。已签名，**未安装**。约 2026-11-21 22:34:22 CST（Asia/Shanghai） |

历史 `build/android-v7a/size/libmsff.so.1.0.0` 仍为 4,235,076，SHA-256 `7a75709520958a1f2ac5c679fad2e4cd3f899c1d10f507d0120cb011ff8f880b`。不是设备硬解或出画验收。

## 25. P4 AV1 可验证切片（2026-11-21 22:48 CST，Asia/Shanghai）

本切片把 AV1 接到既有 `ms_video_*` 与统一播放器/Android 硬解查询-配置-提交路径，而不是只加 MIME。处理合法 OBU 与 av1C、sequence header 宽高、temporal/frame unit 边界、KEY/SWITCH 关键帧，以及中途 sequence header 变化。合法 av1C 原样作为 MediaCodec csd-0；仅有帧内 SH 时 **complete 可在 csd_size=0 成立**，不把 SH 包装成假 av1C。MIME 为 `video/av01`。

已覆盖（宿主）：av1C 16 字节受控样本 64×64/32×32 解析；无 CSD 的 SH+FRAME AU；libmsff 对已开 mov/MP4（`av01`）输出完整 AU、PTS、关键帧（实测 cfg=6 au=20 key=5 pts=20 inspect=20，av1c 累计 96 字节），seek 后首视频 AU 为关键帧，截断有界结束；player 手工 64×64→32×32 中途 SH 升 epoch；mock 拒绝软件 codec。宿主 fixture 用 `/usr/bin/ffmpeg` 的 **libaom-av1 编码器**生成 MP4，不等于产品允许解码，也未引入 dav1d。

明确限制，不广称完成全部合法 AV1：带 `decoder_model` 或 `initial_display_delay` 的 sequence header 本切片保守拒绝以免错读宽高；`show_existing_frame` 不当作新的随机接入关键帧；未开 matroska；自写路径 `ms_codec_query(AV1)` 仍为 `NONE`；空间分层、操作点选择、跨 packet 的裸 OBU 重组等未声称。无硬件须诚实 `UNSUPPORTED`。宿主 mock 与内部 APK **不是**设备硬解或出画。

产品 FFmpeg 追加 `--enable-parser=av1`（此前已有 h263），`CONFIG_AV1_PARSER=1`，`CONFIG_AV1_DECODER=0`，`CONFIG_LIBDAV1D=0`，`CONFIG_DECODERS=0`，`CONFIG_GPL=0`，`CONFIG_NONFREE=0`。符号表有 `ff_av1_parser`、无 `ff_av1_decoder`。

非正式 `./build.sh android-hw`（已按需重建宿主与 Android 两 ABI size-opt FFmpeg 前缀并重链适配器；**不覆盖**历史 `build/android-v7a/size`）：

| 产物 | 大小（字节） | 备注 |
| --- | --- | --- |
| 打入 APK 的 v7a / v8a `libmsff.so` | 4,485,720 / 5,480,048 | 含 h263+av1 parser；NEEDED 仅 libz/libm/libdl/libc；无系统 libssl，无 libmsmedia |
| v7a / v8a `libmsmedia.so` | 172,072 / 175,040 | 含 AV1 OBU/av1C 解析 |
| v7a / v8a `libmshw.so` | 88,540 / 100,296 | 含 av1C 与合法无 CSD 配置、`video/av01` |
| 内部 `build/android-hw/MsHwPlayer.apk` | 10,535,747 | SHA-256 `a1191524964acef5972cdc1e8458c57618edaa6c7ebe0fe96c7266faa8a2e049`。已签名，**未安装**。约 2026-11-21 22:48:47 CST（Asia/Shanghai） |

历史 `build/android-v7a/size/libmsff.so.1.0.0` 仍为 4,235,076，SHA-256 `7a75709520958a1f2ac5c679fad2e4cd3f899c1d10f507d0120cb011ff8f880b`。不是设备硬解或出画验收。

## 26. P4 AC-3 可验证切片（2026-11-21 23:11 CST，Asia/Shanghai）

本切片把 AC-3 接到公共 `ms_audio_*` 与统一播放器/Android 音频查询-配置-提交/AudioTrack 时钟，而不是每编码平行复制。合法无 CSD（帧头含 fscod/frmsizecod/acmod，每帧 1536 样点），不把 dac3 或 AAC ASC 当作 MediaCodec csd-0。MIME 为 `audio/ac3`。系统 AAC 例外不扩到 AC-3。

已覆盖（宿主）：48 kHz/44.1 kHz stereo 帧长；libmsff 对已开 mpegts 与 mov/MP4 输出完整 AU 与 PTS（实测各 cfg=0 au=32 pts=32 inspect=32），seek 后仍有 AU，截断有界；player 带内配置、未选中过滤、切轨清时钟、倍速/seek；mock 拒绝软件 codec 与捏造 ASC；旧 `ms_hw_audio_config` ABI 不加读尾部字段。未追加 FFmpeg parser/decoder（既有 ac3 parser）。`CONFIG_DECODERS=0`。自写 `ms_codec_query(AC3)` 仍为 `NONE`。E-AC-3 未做。fixture 用宿主 ffmpeg 的 AC-3 编码器，不等于产品软解。

无硬件须诚实 `UNSUPPORTED`。宿主 mock **不是**设备出声。本切片当时未再跑非正式 `./build.sh android-hw`。后续 MP3/MP2/Opus 切片已于 23:30 CST 重建双 ABI 前缀并打出新内部 APK（见 §27），AC-3 已编入该包；不得再把 §25 的 AV1 APK 当作当前实现。历史 size 产物未覆盖。

## 27. P4 MP3/MP2/Opus 容器 AU 边界与双 ABI 构建（2026-11-21 23:31 CST，Asia/Shanghai）

本切片补完 MPEG-TS MP3/MP2 多帧 PES 的 AU 边界（最小追加 `mpegaudio` parser）以及 Opus 的 mov/MP4 与最小 Ogg 路径，并接到统一 `ms_audio_*` / player 时钟与 Android MIME（`audio/mpeg`、`audio/mpeg-L2`、`audio/opus`）。系统 AAC 例外不扩用。无硬件须诚实 `UNSUPPORTED`，不得软件 fallback。旧 `struct_size` 不加读。

已覆盖（宿主）：

- MP3：MPEG-TS cfg=0 au=40 pts=40 inspect=40 aligned=40 layer=40（一帧一 AU），MP4 同计数；Layer III；seek 后仍分帧；cancel/截断有界；1152@44.1 kHz 时钟；切轨/倍速/seek flush。
- MP2：MPEG-TS cfg=0 au=42 pts=42 inspect=42 aligned=42 layer=42；Layer II；1152@48 kHz。
- Opus：MP4 与 Ogg 均为 cfg=2 au=51 pts=51 inspect=51 aligned=51，真实 OpusHead/dOps（pre-skip=312）；TOC 20 ms=960 样点；preroll 只扣一次，seek/切轨重置后再扣一次，不重复扣；提交/排队时钟不是扬声器时钟；错误元数据/超限/cancel/截断有界。
- **未做**：尾部 discard padding（Ogg 末页修剪/Skip Samples）；E-AC-3；matroska；raw `.ac3` 白名单。不是全部 Opus 码流。

产品 FFmpeg 追加 `--enable-parser=mpegaudio,opus` 与 `--enable-demuxer=ogg`。`CONFIG_MPEGAUDIO_PARSER=1`、`CONFIG_OPUS_PARSER=1`、`CONFIG_OGG_DEMUXER=1`，`CONFIG_OPUS_DECODER=0`、`CONFIG_MP3_DECODER=0`、`CONFIG_DECODERS=0`、`CONFIG_GPL=0`、`CONFIG_NONFREE=0`。Android unstripped `libmsff` 有 `ff_mpegaudio_parser`、`ff_opus_parser`、`ff_ogg_demuxer`，无对应 decoder。ogg 许可为 oggdec MIT 头 + oggparse* LGPL；启用 ogg 会编入 oggparse 辅助，但 `CONFIG_VORBIS_DECODER=0`、`CONFIG_FLAC_DECODER=0`。format 白名单加入 `ogg`。既有意外打开的 asf/rm demuxer 仍不宣传为功能。

`ANDROID_SIZE_FFMPEG_ONLY=1 FFMPEG_REBUILD=1` 重建双 ABI 前缀后，非正式 `./build.sh android-hw` 于 23:30:26 CST exit 0（非正式，不是四平台/el7/真机验收）。未启动共享 AVD，未安装 APK。

| 产物 | 大小（字节） | 备注 |
| --- | --- | --- |
| 打入 APK 的 v7a / v8a `libmsff.so` | 4,530,824 / 5,526,120 | 含 mpegaudio/opus parser 与 ogg demux；NEEDED 仅 libz/libm/libdl/libc；无系统 libssl，无 libmsmedia |
| v7a / v8a `libmsmedia.so` | 176,380 / 179,752 | 含 AC-3/MP3/MP2/Opus `ms_audio_*` |
| v7a / v8a `libmshw.so` | 90,940 / 102,968 | 含 `audio/ac3`、`audio/mpeg`、`audio/mpeg-L2`、`audio/opus` 与 preroll |
| 内部 `build/android-hw/MsHwPlayer.apk` | 10,642,243 | SHA-256 `b643061c37e828a1ee77311707460297747a4ce16e4f6aaca17081f1a1f1fd68`。已签名，**未安装**。约 2026-11-21 23:30:25 CST（Asia/Shanghai）。替代 22:48 的 AV1 APK（10,535,747 / `a1191524…`），不得把旧包当作当前实现 |

历史 `build/android-v7a/size/libmsff.so.1.0.0` 仍为 4,235,076，SHA-256 `7a75709520958a1f2ac5c679fad2e4cd3f899c1d10f507d0120cb011ff8f880b`。不是设备硬解或出声验收。

## 28. Opus 尾部 discard padding 与 pre-skip / seek preroll 分项（2026-11-21 23:51 CST，Asia/Shanghai）

本切片纠正 §27 中「preroll 只扣一次，seek/切轨重置再扣一次」的表述：那是把流开始 pre-skip、中途 seek preroll、负 PTS/平台 codec delay 混成一件。合同以 RFC 7845、本树 FFmpeg 9.1 与 Android MediaCodec 公开表为准，详见 FEATURE_MATRIX。

Android 路径：csd-0 = OpusHead；csd-1/csd-2 = 原生字节序 uint64 纳秒（pre-skip / seek pre-roll）；有 csd-1 时不设 `encoder-delay`。`platform_trims_delay=1` 后本层不再对 start/warmup 做第二次 PCM 修剪，但在 MediaCodec 输出之后、`AudioTrack.write` 之前按包尾 SKIP 修剪（有界 FIFO，leftover 未排空不取新输出）。时钟仍用 playback head，不是压缩提交计数。无设备，本切片**未**做实听，不得把宿主 mock 写成听感通过。

非正式 `./build.sh android-hw` 于 23:51:51 CST exit 0。未启动共享 AVD，未安装 APK。本切片**未**重建 FFmpeg 前缀（仍 `CONFIG_DECODERS=0`、`CONFIG_OPUS_PARSER=1`、`CONFIG_OGG_DEMUXER=1`、`CONFIG_OPUS_DECODER=0`）。

| 产物 | 字节 | 说明 |
| --- | ---: | --- |
| 打入 APK 的 v7a / v8a `libmsff.so` | 4,531,528 / 5,526,712 | NEEDED 仅 libz/libm/libdl/libc；无系统 libssl，无 libmsmedia；unstripped 含 mpegaudio/opus parser 与 ogg demux，无对应 decoder |
| v7a / v8a `libmsmedia.so` | 176,396 / 179,800 | 加法字段 `ms_event.skip_samples_*`、`ms_audio_cfg.seek_preroll` |
| v7a / v8a `libmshw.so` | 92,180 / 104,184 | 导出 `ms_hw_audio_submit_au` / `ms_hw_audio_note_seek`；csd-1/csd-2 与 PCM 尾部修剪 |
| 内部 `build/android-hw/MsHwPlayer.apk` | 10,642,243 | SHA-256 `ab34373d88babcf388fa31459cac4e51fccc3ccfa93ab88346b32548c7f36c15`。已签名，**未安装**。约 2026-11-21 23:51:51 CST（Asia/Shanghai）。替代 23:30 CST 包（同体积、哈希 `b643061c…`） |

## 29. P4 AVS+（广电广播类 / cavs）统一查询-配置-提交（2026-11-22 00:10 CST，Asia/Shanghai）

本切片把已开 MPEG-TS 上的 AVS+ 广播类接到既有 `ms_video_*`、player 与 Android NDK/JNI 路径。不是 AVS2/AVS3，不是软件 `cavs` 解码。只认 GY/T 257.2 广播类 profile `0x48`；`0x20` 与仅有 `stream_type 0x42` 不得标 PLUS。CSD 为真实 `00 00 01 B0` 序列头，不捏造。

Android 公开契约：AOSP `MediaFormat` 与官方 Supported media formats 无 AVS/CAVS MIME。`ms_hw_video_mime(MS_CODEC_AVS_PLUS)` 返回 NULL；pick/configure 在创建 MediaCodec 或 format 之前返回 `UNSUPPORTED`，并说明无 AOSP MIME，禁止回落到 `video/avc`。不得把 OpenHarmony `"video/cavs"` 或某 SoC（如 RK 的 `cavs_rkmpp`）写成普遍存在的 MediaCodec type。**平台设备常无 AVS+ 硬件**；无硬件须诚实拒绝，不能软解回退。宿主 mock（`mock.avsplus.hw`）可配置/提交，**不是设备硬解或出画**。不得把无硬件的 `UNSUPPORTED` 写成设备完成。

本箱无合法 AVS+ 编码器、无 `/ms/build/ffmpeg`；测试用手工打包 fixture 与 `ms_synth` TS，来源可追。fixture 编码（此处为手工打包）不等于产品允许软解。

非正式 `./build.sh android-hw` 于 00:10:37 CST exit 0。已按 `ANDROID_SIZE_FFMPEG_ONLY=1 FFMPEG_REBUILD=1` 重建双 ABI 前缀（`CONFIG_CAVSVIDEO_PARSER=1`、`CONFIG_CAVS_DECODER=0`、`CONFIG_DECODERS=0`），未覆盖历史 size 产物。未启动共享 AVD，未安装 APK。

| 产物 | 字节 | 说明 |
| --- | ---: | --- |
| 打入 APK 的 v7a / v8a `libmsff.so` | 4,534,008 / 5,529,320 | NEEDED 仅 libz/libm/libdl/libc；无系统 libssl，无 libmsmedia；unstripped 含 `ff_cavsvideo_parser`，无 `ff_cavs_decoder` |
| v7a / v8a `libmsmedia.so` | 177,660 / 181,000 | 加法 `MS_SAMPLE_FMT_CAVS`；广播类序列头解析 |
| v7a / v8a `libmshw.so` | 92,388 / 104,424 | MIME 为 NULL 时诚实 `UNSUPPORTED`；mock 名称 `mock.avsplus.hw` / `mock.avsplus.sw` |
| 内部 `build/android-hw/MsHwPlayer.apk` | 10,650,435 | SHA-256 `67ebe4370d1fb7f85aa793a7755210c98d37c18439a8831e93b31460b598b441`。已签名，**未安装**。约 2026-11-22 00:10:37 CST（Asia/Shanghai）。替代 23:51 CST Opus 收口包（10,642,243，`ab34373d…`） |

## 30. P4 位图字幕（DVB / PGS / VobSub）解析与叠加接线（2026-11-22 00:44 CST，Asia/Shanghai）

本切片把常见位图字幕接到既有 `ms_sub` 时间轴、player flush/seek/关闭清理与 Android 叠加，而不是枚举后选择播放返回 `UNSUPPORTED` 就算完成。加法 ABI，`MSMEDIA_ABI_VERSION` 仍为 1。未引入 libass/FreeType；未开任何字幕软件解码器（`CONFIG_DECODERS=0` 维持）。完整 ASS 仍是欠账。通用 `MS_CODEC_SUB_BITMAP`（XSUB/图文）仍不可播放。

Android：`nativeFillBitmap` 提供几何与 RGBA 字节，Activity 用 ImageView + `Bitmap.Config.ARGB_8888` 显示；`nativeFillCues`/`nativeCurrentCue` 不把位图字节拷进字符串。这是逻辑接线，**不是真机视觉验收**。不执行脚本。图幅上限 1920×1080，单文档 RGBA 预算 4MiB，跨 PES 组装 64KiB。畸形/截断/超大图幅/缺调色板可控。本轮无需追加 FFmpeg demuxer/parser（mpegts 已能交出 PES）。

非正式 `./build.sh android-hw` 于 00:44:03 CST exit 0。未重建 FFmpeg 前缀，未覆盖历史 size 产物。未启动共享 AVD，未安装 APK。不得把 00:10 CST AVS+ 包当作本切片实现。

| 产物 | 字节 | 说明 |
| --- | ---: | --- |
| 打入 APK 的 v7a / v8a `libmsff.so` | 4,550,712 / 5,545,656 | NEEDED 仅 libz/libm/libdl/libc；无系统 libssl，无 libmsmedia。体积增来自静态编入的位图解析，不是新软解 |
| v7a / v8a `libmsmedia.so` | 193,624 / 196,936 | NEEDED 仅 libc/libm/libdl |
| v7a / v8a `libmshw.so` | 92,988 / 104,952 | NEEDED 含 libmsff/libmsmedia/libmediandk/libandroid/liblog/libc/libdl |
| 内部 `build/android-hw/MsHwPlayer.apk` | 10,724,163 | SHA-256 `360b18b8889bdbcb741823fbdf03ad1c7dce094d55a59d219ee694087cb9eaf2`。已签名，**未安装**。约 2026-11-22 00:44:03 CST（Asia/Shanghai）。替代 00:10 CST AVS+ 包（10,650,435，`67ebe437…`） |

## 31. 最小追加 matroska demuxer 与 MKV 内嵌已支持轨（2026-11-22 00:59 CST，Asia/Shanghai）

本切片只按需打开官方 matroska demuxer 及相关必要辅助对象，使受控 MKV 内嵌已支持编码与字幕走既有 `ms_ff` / `ms_sub` 路径。不把先前「ASS 事件解析已测」写成当时已能 demux MKV。未引入 libass/FreeType，完整 ASS 渲染仍为欠账。未开任何 decoder（`CONFIG_DECODERS=0`），未开 matroska muxer 与 bzlib。加法 `MS_FORMAT_MATROSKA` / `MS_FORMAT_WEBM`，ABI 仍为 1。WebM 与 matroska 为同一 demuxer；本轮只用已支持 Opus 验证绑定，**不宣传 VP9/VP8/Vorbis**。

已真实重建宿主与 Android 双 ABI 前缀（`FFMPEG_REBUILD=1`，`ANDROID_SIZE_FFMPEG_ONLY=1`），未覆盖历史 `build/android-v7a/size/libmsff.so.1.0.0`。官方 `env -u CC -u CXX -u LD PATH=/usr/bin:/bin ./build.sh test`（gcc 14.2.0 ASan/UBSan）于 **2026-11-22 00:58:38 CST** exit 0。非正式 `./build.sh android-hw` 于 **2026-11-22 00:59:30 CST** exit 0。未启动共享 AVD，未安装 APK。不得把 00:44 CST 位图包当作本切片实现。

| 产物 | 字节 | 说明 |
| --- | ---: | --- |
| 打入 APK 的 v7a / v8a `libmsff.so` | 4,596,124 / 5,592,808 | NEEDED 仅 libz/libm/libdl/libc；无系统 libssl，无 libmsmedia。相对 00:44 位图包 +45,412 / +47,152，来自静态编入的 matroska demux，不是新软解 |
| v7a / v8a `libmsmedia.so` | 193,664 / 196,952 | 加法格式枚举名称；NEEDED 仅 libc/libm/libdl |
| v7a / v8a `libmshw.so` | 92,988 / 104,952 | 本切片未改硬解 ABI |
| 内部 `build/android-hw/MsHwPlayer.apk` | 10,814,275 | SHA-256 `f5967a755217017568277be2a9782690a714351426bce534de8f90844467d997`。已签名，**未安装**。约 2026-11-22 00:59:30 CST（Asia/Shanghai）。替代 00:44 CST 位图包（10,724,163，`360b18b888…`） |

## 32. HLS 字幕 rendition（独立 WebVTT 外链；2026-11-22 01:30 CST，Asia/Shanghai）

本切片对标已落地的 HLS 备选 AUDIO：复用官方 FFmpeg HLS 的 AVProgram/AVStream/rendition 与既有 `ms_sub` / pending-commit / JNI 入队模型，不另写清单调度。只追加 `--enable-demuxer=webvtt` 与白名单 `webvtt`。`CONFIG_DECODERS=0`、`CONFIG_WEBVTT_DECODER=0`、`CONFIG_LIBASS=0`。官方 lavf 对 `TYPE=SUBTITLES` 外链强制用 webvtt 打开分片 URL；本轮宿主证据是独立 WebVTT playlist，**不是** MPEG-TS 封装或主 TS 带内 WebVTT，也不把清单一侧的 ASS/SRT 外链写成已验收。打开后对含字幕流的 HLS 回绕到 0，以免 `find_stream_info` 把短 VTT 读尽。最小本地补丁见 `patches/ffmpeg-9.1-hls-sub-rendition-switch.patch`（切轨不丢掉本段已开始的 cue，关闭再启时重置残留上下文）。

已真实重建宿主与 Android 双 ABI 前缀（`FFMPEG_REBUILD=1`，`ANDROID_SIZE_FFMPEG_ONLY=1`），未覆盖历史 `build/android-v7a/size/libmsff.so.1.0.0`。官方 `env -u CC -u CXX -u LD PATH=/usr/bin:/bin ./build.sh test`（gcc 14.2.0 ASan/UBSan）于 **2026-11-22 01:30:26 CST** exit 0。非正式 `./build.sh android-hw` 于 **2026-11-22 01:30:49 CST** exit 0。未启动共享 AVD，未安装 APK。不得把 00:59 CST 的 matroska 包当作本切片实现。

| 产物 | 字节 | 说明 |
| --- | ---: | --- |
| 打入 APK 的 v7a / v8a `libmsff.so` | 4,602,116 / 5,599,240 | NEEDED 仅 libz/libm/libdl/libc；无系统 libssl，无 libmsmedia。相对 00:59 matroska 包 +5,992 / +6,432，来自静态编入的 webvtt demuxer 与最小 HLS 字幕切轨补丁，不是新软解 |
| v7a / v8a `libmsmedia.so` | 193,664 / 196,952 | 本切片未改核心 ABI 尺寸；NEEDED 仅 libc/libm/libdl |
| v7a / v8a `libmshw.so` | 92,988 / 104,952 | 本切片未改硬解 ABI |
| 内部 `build/android-hw/MsHwPlayer.apk` | 10,826,563 | SHA-256 `d4e1940b469dddc4549634c0a649963d5dc05724d6f4f34e39a490fb7c83a942`。已签名，**未安装**。约 2026-11-22 01:30:49 CST（Asia/Shanghai）。替代 00:59 CST matroska 包（10,814,275，`f5967a755217017568277be2a9782690a714351426bce534de8f90844467d997`） |

本切片不是设备硬解、出画或字幕上屏验收。HLS 视频变体见 §33。

## 33. HLS 视频变体手动选择与切换（2026-11-22 01:55 CST，Asia/Shanghai）

只做既定的手动变体选择，**不承诺自动 ABR 策略**。复用官方 FFmpeg HLS 的 AVProgram/AVStream/playlist 与已落地的音/字幕 pending-commit，不另写 master 调度。`ms_ff_select_track` 对视频只武装 pending；等到新轨关键帧才提交、重发 CSD、丢弃旧视频槽、抬 `selection_epoch` 不抬 `timeline_epoch`。不允许关闭最后一条视频。跨 codec 返回 `UNSUPPORTED`，不是切换完成。关联外链音/字幕组保持用户选择；muxed 在旧变体 TS 内的音轨在提交时回退到新变体的 muxed 音轨。失败/EOF 撤回 pending，不拆会话、不把新轨标已选；JNI 仍只入队。枚举名称只填已知元数据（打开时常见仅 `variant_bitrate`；宽高以切换后真实 SPS 为准）。

尝试把字幕补丁的「关闭残留 ctx」扩展到视频 playlist 会在 `hls_read_packet` 里对空 `pls->ctx` 调用 `av_read_frame` 崩溃，已撤回；字幕补丁与音视频 catch-up 保持上一切片行为。Android 前缀未因本切片重编（libavformat 产品代码相对 01:30 未改），只重链 `ms_ff` 适配器。未覆盖历史 `build/android-v7a/size/libmsff.so.1.0.0`（4,235,076，SHA-256 `7a75709520958a1f2ac5c679fad2e4cd3f899c1d10f507d0120cb011ff8f880b`，mtime 2026-11-21 12:17 CST）。

官方 `env -u CC -u CXX -u LD PATH=/usr/bin:/bin ./build.sh test`（gcc 14.2.0 ASan/UBSan）于 **2026-11-22 01:55:24 CST** exit 0（含 `test_ff_hls_video_variants` 受控本地 HTTP 两档 master 的 AU/CSD/关键帧/PTS 来回切，以及 `test_ff_hls_video_variant_player` 暂停/倍速/seek）。非正式 `./build.sh android-hw` 于 **2026-11-22 01:55:40 CST** exit 0。未启动共享 AVD，未安装 APK。不得把 01:30 CST 的 HLS 字幕包当作本切片实现。

| 产物 | 字节 | 说明 |
| --- | ---: | --- |
| 打入 APK 的 v7a / v8a `libmsff.so` | 4,604,292 / 5,600,920 | NEEDED 仅 libz/libm/libdl/libc；无系统 libssl，无 libmsmedia。相对 01:30 字幕包 +2,176 / +1,680，来自适配器 pending-video 路径，不是新软解、也不是前缀重编 |
| v7a / v8a `libmsmedia.so` | 193,664 / 196,952 | 本切片未改核心 ABI 尺寸；NEEDED 仅 libc/libm/libdl |
| v7a / v8a `libmshw.so` | 93,132 / 105,112 | JNI 同步已提交视频轨；NEEDED 仍含 libmsff/libmsmedia/libmediandk/libandroid/liblog/libc/libdl |
| 内部 `build/android-hw/MsHwPlayer.apk` | 10,830,659 | SHA-256 `92ee83fad6c54d32cd29527774f1bb8d253c4aeac7d4003931aae86eb9872df5`。已签名，**未安装**。约 2026-11-22 01:55 CST（Asia/Shanghai）。替代 01:30 CST HLS 字幕包（10,826,563，`d4e1940b469dddc4549634c0a649963d5dc05724d6f4f34e39a490fb7c83a942`） |

本切片不是设备硬解或出画验收。上一轮限制中，PGS/DVB lavf 组包见 §34；仍保留：直播不可 seek 时短 VTT 探测耗尽可能缺片头 cue；用户 seek 不补发 start 早于 seek 点的 cue。

## 34. PGS 续包 / DVB 超单 TS 包 PES 组包鲁棒性（2026-11-22 02:14 CST，Asia/Shanghai）

先修已有字幕真实缺口，不开新功能。复用有界 `acc` 与统一时间轴（扩展 ODS RLE 缓冲与跨 PES 时间戳），保留旧 ABI、RGBA 生命周期与 4MiB/64KiB 预算。超限诚实 `OVERFLOW`，不把旧画面当新显示集。FFmpeg mpegts 契约：一个 AVPacket 对应一个 PES；DVB 一个 PES 可跨多个 TS 包并带 `0x20 0x00` 前缀；PGS 常见一段一个 PES。END 单独成包时必须并入 acc。PGS ODS 按 first/continuation/last 收齐再解码。DVB 续 PES 剥除前缀；只有以 page composition 开头且含结束段的包才当作独立显示集，避免丢掉未完成页状态。seek/切轨/`clear_cues` 清空 acc 与对象，不串帧、不泄漏。

证据必须经 libmsff demux 到最终 RGBA，独立校验尺寸/位置/像素/开始时间；分片点覆盖段头与载荷断点，不只手工把完整 AU 喂 parser。`test_ff_bitmap_subs` 的 lavf DVB 路径改为 16×16 多 TS 包 PES（不再用 4×4 单包代替）；`test_ff_bitmap_reassembly` 覆盖 DVB 多 PES 断点、缺中片、截断、seek/flush，以及 PGS 每段一 PES、ODS 两段/三段续包与原始字节流拆分。测试夹具 `ms_synth_pes` 在末包不满 184 字节时改用 adaptation stuffing，避免把 `0xFF` 填进 PES 载荷（不等于产品软解）。本轮**未**改 FFmpeg 补丁，也未修 HLS seek/短 VTT（明确留作下一项，不堆未测改动）。Android 前缀未重编（`libavformat.a` 仍 14,423,582），只重链适配器与编入 libmsff 的 `ms_sub`。未覆盖历史 `build/android-v7a/size/libmsff.so.1.0.0`（4,235,076，SHA-256 `7a75709520958a1f2ac5c679fad2e4cd3f899c1d10f507d0120cb011ff8f880b`，mtime 2026-11-21 12:17 CST）。

官方 `env -u CC -u CXX -u LD PATH=/usr/bin:/bin ./build.sh test`（gcc 14.2.0 ASan/UBSan）于 **2026-11-22 02:14:27 CST** exit 0（含 HLS 变体/字幕/音轨、MKV、ASS、Opus 及其它编码回归，以及上述位图组包用例）。非正式 `./build.sh android-hw` 于 **2026-11-22 02:14:54 CST** exit 0。未启动共享 AVD，未安装 APK。不得把 01:55 CST 的视频变体包当作本组包实现。编译/mock 不是设备视觉验收。

| 产物 | 字节 | 说明 |
| --- | ---: | --- |
| 打入 APK 的 v7a / v8a `libmsff.so` | 4,605,892 / 5,602,312 | NEEDED 仅 libz/libm/libdl/libc；无系统 libssl，无 libmsmedia。相对 01:55 变体包 +1,600 / +1,392，来自静态编入的 `ms_sub` 组包路径，不是新软解、也不是前缀重编 |
| v7a / v8a `libmsmedia.so` | 195,216 / 198,376 | 组包状态与 ODS 缓冲；NEEDED 仅 libc/libm/libdl |
| v7a / v8a `libmshw.so` | 93,132 / 105,112 | 本切片未改 JNI 轨道选择；NEEDED 仍含 libmsff/libmsmedia/libmediandk/libandroid/liblog/libc/libdl |
| 内部 `build/android-hw/MsHwPlayer.apk` | 10,834,755 | SHA-256 `67d2846d04ac3bdf34e4a8e7592ac77f282ca6a28ae9cc97438f9c04802dccd6`。已签名，**未安装**。约 2026-11-22 02:14:57 CST（Asia/Shanghai）。替代 01:55 CST 视频变体包（10,830,659，`92ee83fad6c54d32cd29527774f1bb8d253c4aeac7d4003931aae86eb9872df5`） |

本切片不是设备硬解、出画或字幕上屏验收。HLS 字幕时间轴两项缺口见 §35（宿主已闭环，设备未验）。任何字幕补丁不得破坏视频 playlist ctx（已有崩溃经验须回归）。

## 35. HLS 字幕时间轴：seek 覆盖进行中 cue 与直播短 VTT 片头（2026-11-22 02:38 CST，Asia/Shanghai）

只修已授权统一交付内两项已知缺口，不改音视频 seek/catch-up，不把关闭残留 ctx 扩到视频/音频 playlist（空 ctx 会在 `hls_read_packet` 崩溃，已有撤回经验）。也不把直播改成可 seek，不无限缓存历史。

最小补丁 `patches/ffmpeg-9.1-hls-sub-timeline.patch`（与既有切轨补丁一并幂等应用）：字幕包在 cue 终点（与 dts 同一时基，开区间）仍大于 `seek_timestamp` 时保留，已过期 cue 丢弃；直播字幕 playlist 首次启用从当前滑动窗口 `start_seq_no` 重开。`webvttdec.c` 解析 `X-TIMESTAMP-MAP=LOCAL…,MPEGTS…`（字段顺序不限），把映射结果换算为默认 1/1000，避免与 HLS header stub 建流时基冲突。`ms_sub_doc_add_cue` 对相同 track/起止/文本去重，有界 `MS_SUB_CUE_MAX`。旧 ABI 不变。

证据经本地受控 HTTP 真实 lavf demux，不是手工喂 cue、不只查返回值：`test_ff_hls_sub_timeline` 覆盖 seek(2s) 显示 0.5–4s 长 cue、4s 开区间不显示、跨 segment 相同 cue 去重、切轨后旧 cue 消失、非零 `MEDIA-SEQUENCE` 与 `X-TIMESTAMP-MAP` 对齐到约 2.5–6s；无 ENDLIST 动态 live 在探测后仍有片头 cue，重复拉清单/滑动窗口不重复累计，cancel/失效外链/限额不拖垮主音视频。直播 `ms_ff_is_seekable=0`。fixture 编码不等于产品软解。

因 FFmpeg 源码/补丁变更，已真实重建宿主与双 ABI 前缀后再非正式 `android-hw`。未覆盖历史 `build/android-v7a/size/libmsff.so.1.0.0`（4,235,076，SHA-256 `7a75709520958a1f2ac5c679fad2e4cd3f899c1d10f507d0120cb011ff8f880b`，mtime 2026-11-21 12:17 CST）。宿主 `libavformat.a` 14,429,942（此前位图切片为 14,423,582），`libavcodec.a` 仍 15,177,444。`CONFIG_DECODERS=0`，TLS 校验未关，AAC 系统例外未扩展。

官方 `env -u CC -u CXX -u LD PATH=/usr/bin:/bin ./build.sh test`（gcc 14.2.0 ASan/UBSan）于 **2026-11-22 02:38:05 CST** exit 0（含本时间轴用例及 HLS 变体/字幕/音轨、MKV、位图、ASS、Opus 及其它编码回归）。非正式 `./build.sh android-hw` 于 **2026-11-22 02:41:13 CST** exit 0。未启动共享 AVD，未安装 APK。不得把 02:14 CST 的位图组包 APK（同体积但哈希不同）当作本切片实现。编译/mock 不是设备视觉验收。

| 产物 | 字节 | 说明 |
| --- | ---: | --- |
| 打入 APK 的 v7a / v8a `libmsff.so` | 4,607,236 / 5,603,336 | NEEDED 仅 libz/libm/libdl/libc；无系统 libssl，无 libmsmedia。相对 02:14 位图包 +1,344 / +1,024，来自重编 size 前缀中的 HLS/WebVTT 补丁与适配器，不是新软解 |
| v7a / v8a `libmsmedia.so` | 195,760 / 198,840 | cue 去重；NEEDED 仅 libc/libm/libdl |
| v7a / v8a `libmshw.so` | 93,132 / 105,112 | 本切片未改 JNI 轨道选择；NEEDED 仍含 libmsff/libmsmedia/libmediandk/libandroid/liblog/libc/libdl |
| 内部 `build/android-hw/MsHwPlayer.apk` | 10,834,755 | SHA-256 `e587cb15d8f3a52abb065541e7d7abac1b10cb67d6bbb66cb255f3b7308e2c1c`。已签名，**未安装**。约 2026-11-22 02:41:15 CST（Asia/Shanghai）。替代 02:14 CST 位图组包（同体积 10,834,755，`67d2846d04ac3bdf34e4a8e7592ac77f282ca6a28ae9cc97438f9c04802dccd6`） |

本切片不是设备硬解、出画或字幕上屏验收。仍未做：真机叠加与时间对齐；完整 ASS 渲染；MPEG-TS 封装或主 TS 带内 WebVTT；HLS 清单一侧的 ASS/SRT 外链。

## 36. 累积正确性与稳定 C ABI（2026-11-22 03:03 CST，Asia/Shanghai）

不扩功能。已交付 1838 头的 `ms_event` 无 `struct_size`，不得尾部追加后仍称兼容。本轮恢复 1838 布局（本机 LP64 176 字节），PCM 修剪改走加法 `ms_event_trim` / `ms_hw_player_note_event_trim`，`MSMEDIA_ABI_VERSION` 仍为 1，不删除旧符号。`fill_cue` 按调用方 `struct_size` 闸门写入；`clear_cues` 释放位图 acc。未改 FFmpeg 源码或前缀。`test_abi_old_header` 用真实 1838 公共头编译后动态链接当前 .so（ASan 金丝雀）。未启动共享 AVD，未安装 APK。未覆盖历史 `build/android-v7a/size/libmsff.so.1.0.0`（4,235,076，SHA-256 `7a75709520958a1f2ac5c679fad2e4cd3f899c1d10f507d0120cb011ff8f880b`，mtime 2026-11-21 12:17 CST）。

官方 `env -u CC -u CXX -u LD PATH=/usr/bin:/bin ./build.sh test`（gcc 14.2.0 ASan/UBSan，含新旧头动态兼容专项）于 **2026-11-22 03:03:28 CST** exit 0。非正式 `./build.sh android-hw` 于 **2026-11-22 03:03:40 CST** exit 0（仅重链）。不得把 02:41 CST 时间轴 APK（同体积但哈希不同）当作本切片实现。编译/mock 不是设备验收。

| 产物 | 字节 | 说明 |
| --- | ---: | --- |
| 打入 APK 的 v7a / v8a `libmsff.so` | 4,607,364 / 5,603,736 | NEEDED 仅 libz/libm/libdl/libc；无系统 libssl，无 libmsmedia。相对 02:41 时间轴包略增，来自加法 `ms_ff_event_trim`，不是新软解、也不是前缀重编 |
| v7a / v8a `libmsmedia.so` | 196,192 / 199,352 | `ms_event_trim` / `ms_session_event_trim`；NEEDED 仅 libc/libm/libdl |
| v7a / v8a `libmshw.so` | 93,996 / 105,784 | `ms_hw_player_note_event_trim` 与 JNI 在 feed 前 note；NEEDED 仍含 libmsff/libmsmedia/libmediandk/libandroid/liblog/libc/libdl |
| 内部 `build/android-hw/MsHwPlayer.apk` | 10,834,755 | SHA-256 `5ed9079bbd8d66275a1c77a4ebecd6ca9c95f399b991e1937220ae195af3986e`。已签名，**未安装**。约 2026-11-22 03:03:42 CST（Asia/Shanghai）。替代 02:41 CST 时间轴包（同体积，`e587cb15d8f3a52abb065541e7d7abac1b10cb67d6bbb66cb255f3b7308e2c1c`） |

本切片不是设备硬解、出画或出声验收。

## 37. PCM trim carry：关闭 1:1 输入/输出 FIFO 假设（2026-11-22 03:25 CST，Asia/Shanghai）

收口 §36 记下的 Android `tq_*` 按输出块 pop 一条输入标记的问题。抽出可宿主测试的 `ms_hw_pcm_trim`（`src/hw/ms_hw_audio_trim.c`），Android `ms_hw_audio_android_drive` 与宿主 mock drive **调用同一状态机**，不是 mock 另写一套绕过产品算法。按真实输出样点消耗 skip/end；流级 pre-skip / 3840 warmup 计入当前 AU 的 expected，以免把下一包接到未完成 AU。`platform_trims_delay=1` 时 skip_start 与 prefix 不再扣，expected 置 0，只扣 skip_end。格式/bpf 变化在 hold 或当前 AU 未结束时诚实 `MS_ERR_STATE`；输出 PTS 早于已开始的当前 AU、或 PCM 无对应元数据，亦为 `STATE`。meta 满为 `LIMIT`，未知 expected 且 skip_end 超过 hold 为 `OVERFLOW`。flush/seek/cancel 丢弃 hold，不把应丢掉的尾部 padding 先写出。Android 时钟仍用 AudioTrack playback head，不以已提交压缩样点当扬声器时钟。1838 `ms_event` 布局与 sidecar 接口未改。

反例（修复前 1:1 会错）：960 样点 AU 拆成 400+560、start 312 时，1:1 会把 end 或 start 只打在第一块上；两包合成 1920 时会把第二包 end 误用到整块或丢掉。修复后写出 648（312..959）与 960（312..1271）。另覆盖空输出不 pop meta、延迟输出、EOS 排空、部分 AudioTrack 写（配额 100 再补足）、3840 warmup、多包 start、overflow、flush 取消。用递增样点核对总数与边界。不是设备出声。

官方 `env -u CC -u CXX -u LD PATH=/usr/bin:/bin ./build.sh test`（gcc 14.2.0 ASan/UBSan，含 `test_hw_audio_trim_unit` / `test_hw_audio_trim_via_mock`、`test_ff_opus` sidecar、`test_abi_old_header`）于 **2026-11-22 03:25:12 CST** exit 0。未改 FFmpeg 源码，未重建前缀。非正式 `./build.sh android-hw` 于 **2026-11-22 03:25:43 CST** exit 0（仅重链）。未启动共享 AVD，未安装 APK。未覆盖历史 `build/android-v7a/size/libmsff.so.1.0.0`（4,235,076，SHA-256 `7a75709520958a1f2ac5c679fad2e4cd3f899c1d10f507d0120cb011ff8f880b`，mtime 仍 2026-11-21 12:17:30 CST）。不得把宿主模型写成真机通过。

| 产物 | 字节 | 说明 |
| --- | ---: | --- |
| 打入 APK 的 v7a / v8a `libmsff.so` | 4,607,364 / 5,603,736 | 与 §36 相同；NEEDED 仅 libz/libm/libdl/libc；无系统 libssl，无 libmsmedia。本轮未改 FFmpeg 前缀 |
| v7a / v8a `libmsmedia.so` | 196,192 / 199,352 | 与 §36 相同；NEEDED 仅 libc/libm/libdl |
| v7a / v8a `libmshw.so` | 98,116 / 109,968 | 纳入共享 `ms_hw_pcm_trim`；相对 §36 的 93,996 / 105,784 增加。NEEDED 仍含 libmsff/libmsmedia/libmediandk/libandroid/liblog/libc/libdl |
| 内部 `build/android-hw/MsHwPlayer.apk` | 10,842,947 | SHA-256 `eef92bd2b9afa5b9176a0148c0976470f7ec256437fb2053b28f680131b3ab42`。已签名，**未安装**。约 2026-11-22 03:25:46 CST（Asia/Shanghai）。替代 03:03 CST ABI 包（10,834,755，`5ed9079bbd8d66275a1c77a4ebecd6ca9c95f399b991e1937220ae195af3986e`） |

本切片不是设备硬解、出画或出声验收。MediaCodec 真实拆合/空输出/延迟、平台自动 delay 是否与本层 skip_end 对齐、AudioTrack 部分写与 playback head，仍须设备核实。


## 38. HLS EVENT/滑动窗口定位（2026-11-22 03:55 CST，Asia/Shanghai）

本切片在官方 FFmpeg 9.1 HLS demuxer 上公布 EVENT/滑动窗口元数据，并在 `libmsff` / `libmshw` / JNI 暴露加法 range 与 live_edge API。不另建 DVR，不无限本地缓存。DASH 直播保持 `NOT_SEEKABLE`。JNI `nativeSeekLiveEdge` 与相对 seek 均只入队，不阻塞 UI。seek 后仍走既有 flush / 音频 trim / 字幕清空 / 关键帧门控。宿主只证明 AU/CSD/PTS，本 APK **未安装、未启动共享 AVD，不是设备出画或出声验收**。

已用 `ANDROID_SIZE_FFMPEG_ONLY=1 FFMPEG_REBUILD=1` 真实重建 v7a/v8a size-opt 前缀（含 `msmedia_refresh_hls_window`），再 `./build.sh android-hw` 重链。**未覆盖**冻结 v7a size 产物 `build/android-v7a/size/libmsff.so.1.0.0`（4,235,076 字节，SHA-256 `7a75709520958a1f2ac5c679fad2e4cd3f899c1d10f507d0120cb011ff8f880b`，mtime 2026-11-21 12:17:30 CST）。

| 产物 | 字节 | 说明 |
| --- | ---: | --- |
| 打入 APK 的 v7a / v8a `libmsff.so` | 4,610,244 / 5,606,384 | 纳入 live-window 前缀；相对 §37 的 4,607,364 / 5,603,736 增加。NEEDED 仅 libz/libm/libdl/libc；无系统 libssl，无 libmsmedia |
| v7a / v8a `libmsmedia.so` | 196,192 / 199,352 | 与 §37 相同；NEEDED 仅 libc/libm/libdl |
| v7a / v8a `libmshw.so` | 100,440 / 111,960 | 纳入 seek range / live_edge；相对 §37 的 98,116 / 109,968 增加。NEEDED 仍含 libmsff/libmsmedia/libmediandk/libandroid/liblog/libc/libdl |
| 内部 `build/android-hw/MsHwPlayer.apk` | 10,859,331 | SHA-256 `0029b791802bf5250a13c0e70e1b9bc42ea681fd3ad8e99a8ccb37abf8c8da19`。已签名，**未安装**。约 2026-11-22 03:55:16 CST（Asia/Shanghai）。替代 03:25 CST trim 包（10,842,947，`eef92bd2b9afa5b9176a0148c0976470f7ec256437fb2053b28f680131b3ab42`） |

本切片不是设备硬解、出画、出声或真机时移验收。

## 39. 可选完整 ASS 栅格接入（2026-11-22 04:19 CST，Asia/Shanghai）

`libmshw` 通过 `dlopen("libmsass.so")` 选择完整渲染，不 NEEDED 该模块；缺失时保持轻量去标签 TextView。JNI `nativeFillAssFrame` 复用既有 ImageView RGBA 叠加，画布可按舞台尺寸设置，不读回视频 Surface。测试 APK 在 `System.loadLibrary("msass")` 成功后启用该路径。逻辑接线，**不是设备视觉验收**。未启动共享 AVD，未安装 APK。

打入 APK 的可选 so：v7a 1,478,856 / v8a 1,966,168（NEEDED 仅 libz/libm/libdl/libc）。默认核心 `libmsmedia`/`libmsff` 体积与 §38 相同；`libmshw` 因 bind 略增（105,592 / 118,072）。内部 APK 14,316,499 字节，SHA-256 `6a74fe6e53eaa5383ebbfaced5be1ee77284c7f6034943c89c1752bc46d703bf`。字体目录为 `/system/fonts`，不打包字体，附件字体默认关闭。

## 40. 可选 AI 字幕插件接入（2026-11-22 04:51 CST，Asia/Shanghai）

`libmshw` 通过 `dlopen("libmsai.so")` 接入可选插件，不 NEEDED 该模块；缺失或未配置时 AI 保持禁用，无采集、无额外线程。修剪后可听 PCM 在 Android MediaCodec 输出路径与宿主相同的 `ms_hw_pcm_trim` 之后 tap，再 `plat_write_pcm`，不是二次软解。JNI `nativeAiStatus` / `nativeAiConfigure` / `nativeAiSetEnabled` 只报告 disabled 或 simulated-local-fake；测试按钮文案为「AI sim (not real)」，默认不启用，不得伪报已用某商业 AI。生成轨 `0xFFFF0002` 须用户选中才进入 `subtitles_at`，不覆盖原字幕选择。

打入 APK 的可选 so：`libmsai` v7a 20,000 / v8a 20,608（NEEDED 仅 libmsmedia/libc/libdl，无模型）；`libmsass` 体积与 §39 相同。默认核心 `libmsmedia` 196,448 / 199,608，`libmsff` 与 §39 相同，`libmshw` 112,600 / 125,464（仍不 NEEDED libmsai）。内部 APK 14,378,081 字节，SHA-256 `c1b4cbcdfa4e24dfe6a7f92e54099d6b8af9c2f3292144b9a7c839ff5b6fb03c`。已签名，**未安装、未启动共享 AVD，不是设备出声或真实识别验收**。

## 41. Linux VAAPI 映射/探测与 Android 重链（2026-11-22 05:18 CST，Asia/Shanghai）

公共头增加 `ms_hw_vaapi_*`（加法，`MSMEDIA_HW_ABI` 仍为 1）。Android `libmshw` 编入同一 `ms_hw_vaapi.c`：解析符号可用，探测对该平台返回未构建路径，**不**走 VAAPI，仍以 MediaCodec 为准。本轮非正式双 ABI 重链见 `docs/PROGRESS.md` §41 与 `docs/HW_LINUX.md`。未启动共享 AVD，未安装 APK，不是设备硬解验收。

## 42. 播放生命周期：Surface 丢失重建与音频焦点（2026-11-22 15:57 CST，Asia/Shanghai）

本切片收口测试 APK 上已发现的生命周期缺陷，宿主 mock 可验证，**不是**真机前后台、出画或出声验收。`adb` 仍空，未启动共享 AVD，未安装 APK。

根因与修复：

- `surfaceCreated` 曾在每次表面创建时自动 `nativePlayPath` / `startUrl`，旋转或从后台回到前台会重开会话。现仅从 Intent 自动开播一次；之后的 `surfaceCreated` 只 `set_surface` 重绑。
- JNI 在表面置空时曾调用 `ms_hw_player_release_decoder`，该函数会同时关闭音频会话。现只走 `set_surface(NULL)`：拆视频解码器、清待呈现队列、重设关键帧门闩，并作废旧表面的 `note_display` 证据；保留 CSD、音频、请求端到端计时与会话 epoch。同指针 `set_surface` 不拆。
- 无表面且正在等待关键帧时，非关键视频 AU 记入 `dropped_nokey` 并消费，避免泵被 `AGAIN` 堵住；关键帧仍 `AGAIN` 留给重绑后提交。
- 新增加法 API `ms_hw_player_notify_audio_focus` / `ms_hw_player_is_user_paused`（`MSMEDIA_HW_ABI` 仍为 1）。用户 `pause`/`resume` 与焦点暂停分开。Activity：`onPause` 放弃焦点（通知 LOSS，不调用会置位用户暂停的 `nativePause`）；`onResume` 在未用户暂停时请求焦点并 GAIN；Pause/Resume 按钮只改用户暂停。`TRANSIENT_CAN_DUCK` 按暂停处理，不做音量闪避。焦点监听为同文件顶层类（避免匿名内部类导致 d8 失败）。
- 首帧合同未改：`first_output` 仍不是出画；`display` 仍仅 `note_display`。表面丢失会清掉旧表面的 display 证据，不得把 codec 输出写成出画。

宿主：`./build.sh test`（ASan/UBSan）于 15:56 CST 退出 0，含 `test_surface_loss_keeps_audio_and_focus`、`test_ff_matroska` 与 1838（`sizeof(ms_event)=176`，`abi=1`）。非正式双 ABI 重链（未重编 FFmpeg 前缀，未覆盖历史 `build/android-v7a/size`）：

| 产物 | 字节 | 说明 |
| --- | ---: | --- |
| v7a / v8a `libmshw.so` | 120,708 / 133,984 | NEEDED libmsff、libmsmedia、libmediandk、libandroid、liblog、libdl、libc；不 NEEDED libmsass/libmsai |
| 打入 APK 的 v7a / v8a `libmsff.so` | 4,610,564 / 5,606,704 | 既有 size-opt 前缀重链；NEEDED 仅 libz、libm、libdl、libc，无 libssl |
| 内部 `build/android-hw/MsHwPlayer.apk` | 14,394,465 | SHA-256 `1c64286eda65443a8fff1f6021f99535c42599d6795f9e7a859f1a242ac8fe04`。mtime 2026-11-22 15:57:42 CST。已由 `./build.sh android-hw` 签名，**未安装**。不是交付包。 |
| `libmshw-legacy.a` | 172,634 | 仍仅 API 21 工具链编译校验 API 16–20 视频 JNI；不打进 APK |

冻结对照未改：`build/release/libmsmedia.so.1.0.0` 166,168；历史 `build/android-v7a/size/libmsff.so.1.0.0` 4,235,076，SHA-256 `7a75709520958a1f2ac5c679fad2e4cd3f899c1d10f507d0120cb011ff8f880b`。

## 43. 宿主补齐 Android 修剪旗标、leftover 与交错生命周期（2026-11-22 16:12 CST，Asia/Shanghai）

选题：上一轮 Surface/焦点切片不重复做，也不写成真机已验收。本轮核对后发现：宿主默认 `platform_trims_delay=0`，Android 产品路径（旗标为 1、expected=0、只扣 skip_end）此前没有会话级宿主证明；既有 `via_partial_and_flush` 是先写完 leftover 再 flush，并未断言「写到一半就 flush / 暂停」。表面丢失期间换 CSD、以及焦点丢失期间 seek，也未在宿主播放器路径覆盖。真机 MediaCodec 契约本身仍只能等设备，不在本轮空转。

实现（保持复用，小步；JNI/Java/APK 未改，未重链 android-hw，未重建 FFmpeg 前缀）：

- 宿主调试钩子 `ms_hw_audio_debug_set_platform_trims_delay`：打开后把未消耗的 start_fallback/warmup 交给 mock 的 `platform_lead_left`，decode 输出先丢掉这部分 leading（模拟平台已按 csd-1/csd-2 修剪），再走同一 `ms_hw_pcm_trim`。设备路径返回 `UNSUPPORTED`。
- 产品修剪合同未改算法：`platform_trims_delay=1` 时本层 skip_start/prefix 为 0、`expected_samples=0`，skip_end 仍扣。未知 expected 时 **靠 PTS 切换 AU** 才能把后一包的 skip_end 用上；测试助手 `sub_toc` 在带 skip 的 `submit_au` 路径此前未填写 `pts_us`，已补上（否则两包会粘在第一包 `skip_end=0` 上，出现 1608 而非 960）。
- 新增宿主用例：部分写中途 flush 丢 leftover 并释放 AU；暂停不排空 leftover、恢复按递增样点续写；平台旗标下 312+648 不双扣且可听仍为 960（312..1271）；seek 后平台扣 3840、本层不再扣，可听 960（3840..4799）；单元级 expected=0 + PTS 切 AU；播放器 `test_csd_change_and_seek_while_unbound_or_paused`。

官方 `env -u CC -u CXX -u LD PATH=/usr/bin:/bin ./build.sh test` 于 **2026-11-22 16:12 CST** 退出 0，含上一轮生命周期用例、本轮新用例、`test_ff_matroska` 与 1838（`sizeof(ms_event)=176`，`abi=1`）。宿主 mock **不是**真机出画/出声或 1 秒首帧验收。冻结历史制品未覆盖。

## 44. platform_trims 的 AU 边界不再只靠互异 PTS（2026-11-22 16:21 CST，Asia/Shanghai）

上一轮暴露：`platform_trims_delay=1` 把 `expected_samples` 置 0，修剪机只在 PTS 切到下一 AU 或 EOS 时结束当前包。核查结论：**产品路径并不总是提供可用且互异的 pts_us**，不是「只是测试助手写错」：

- `ms_ff` 的 `fill_packet_ts` 仅在 `pkt->pts != AV_NOPTS_VALUE` 时置 `has_pts`。
- `ms_session` 原样复制帧的 `has_pts`；MPEG-TS PES 可以没有 PTS。
- 播放器 `au_to_us` 在 `!has_pts` 时返回成功且 `has=0`，`submit_au` 写成 `pts_us=0`；`ms_hw_audio_au` 原先没有 `has_pts`，`push_meta_for_slot` 还把缺 PTS 伪造成 `has_pts=1, pts=0`。
- Android `queueInputBuffer` 把负/缺时间戳夹成 0，`trim_feed` 一律 `has_pts=1` 配 `presentationTimeUs`。两包都是 0 时 `maybe_pts_switch` 不会切 AU，后一包 `skip_end` 不生效（反例可听 1608 而非 960）。时间戳相同同样失败。反向乱序仍为 `MS_ERR_STATE`（保持）。seek 后整包被 leading 吞掉时，旧逻辑还会留下 `expected=0` 且 `seen=0` 的粘连 meta。

修复（公共音频路径，不是把责任推给调用方）：

- 旗标打开时，`expected = 压缩样点数 − 本包已交给平台的 leading`，AU 按样点计数结束，本层不再二次扣 start。整包都被 leading 去掉则不入队 meta。
- `ms_hw_audio_au` 加法字段 `has_pts`（`MSMEDIA_HW_ABI` 仍为 1）。旧 `struct_size` 不含该字段时仍视为已提供 `pts_us`；新路径由播放器按事件诚实下发。`ms_hw_audio_submit` 仍视为有 PTS。
- 帧长未知时仍可能 `expected=0`，那时才回退 PTS/EOS；合同写在实现注释与本小节，不是「调用方必须保证互异 PTS」。

宿主反例：`via_platform_trims_no_or_same_pts`（缺 PTS、两包 pts=0、seek 后第五包无 PTS 且 `skip_end=648` 可听 312）。既有互异 PTS、312+648、3840 warmup、leftover/暂停用例一并回归。官方 `./build.sh test` 于 **2026-11-22 16:21 CST** 退出 0，含 trim、生命周期、matroska 与 1838。JNI/Java/APK 源文件未改，按指示未重链 android-hw；非正式 APK 仍为上一轮生命周期产物，**尚未包含本修复**。FFmpeg 前缀未重建。不是真机出声验收。

## 45. 非正式双 ABI APK 重链纳入缺 PTS skip_end 修复（2026-11-22 16:23 CST，Asia/Shanghai）

授权：用现有 `./build.sh android-hw` 把当前宿主已修的 `libmsmedia` / `libmshw` 重链进内部非正式 APK。JNI/Java 源文件本轮仍未改。FFmpeg 前缀未重建（v7a/v8a size-opt `libavformat.a` mtime 仍为 2026-11-22 03:53:14 / 03:54:55 CST），只从既有前缀重链 `libmsff.so`。未覆盖冻结产物，未安装，未启动共享 AVD，不是真机出画/出声或 1 秒首帧验收。

**不得把上一轮 APK 当作本修复实现。** 上一轮非正式包（生命周期切片，不含缺 PTS/`skip_end` 修复）：`build/android-hw/MsHwPlayer.apk` 14,394,465 字节，SHA-256 `1c64286eda65443a8fff1f6021f99535c42599d6795f9e7a859f1a242ac8fe04`，mtime 2026-11-22 15:57:42 CST；当时 v7a/v8a `libmshw.so` 为 120,708 / 133,984。

本轮重链后同一路径被脚本更新为新包（字节数碰巧仍为 14,394,465，哈希不同）：

| 产物 | 字节 | 说明 |
| --- | ---: | --- |
| 内部 `build/android-hw/MsHwPlayer.apk` | 14,394,465 | SHA-256 `d7c2149e9e650c07d9885b9057bf4d5de5fa5df2c331350f128cb3266ec16d67`。mtime 2026-11-22 16:22:50 CST（Asia/Shanghai）。`apksigner verify --min-sdk-version 21` 通过。**未安装**，不是交付包。 |
| 包内 v7a / v8a `libmshw.so` | 120,868 / 134,048 | NEEDED：libmsff、libmsmedia、libmediandk、libandroid、liblog、libdl、libc；不 NEEDED libmsass/libmsai |
| 包内 v7a / v8a `libmsmedia.so` | 196,528 / 199,656 | NEEDED：libc、libm、libdl |
| 包内 v7a / v8a `libmsff.so` | 4,610,564 / 5,606,704 | 既有 size-opt 前缀重链；NEEDED 仅 libz、libm、libdl、libc，无 libssl |
| 包内可选 v7a / v8a `libmsass.so` | 1,478,856 / 1,966,168 | 完整 ASS 模块，非默认核心；NEEDED libz、libm、libdl、libc |
| 包内可选 v7a / v8a `libmsai.so` | 20,000 / 20,608 | 假供应商框架，非真实 ASR；NEEDED libmsmedia、libc、libdl |
| `libmshw-legacy.a` | 173,100 | 仍仅 API 21 工具链编译校验 API 16–20 视频 JNI；不打进 APK |

冻结对照未改：`build/release/libmsmedia.so.1.0.0` 166,168，SHA-256 `49a555154fba7ff0f9558c11050f35682633636dc2c0ab70a36cccf015bb36f5`，mtime 2026-11-21 11:57:55 CST；历史 `build/android-v7a/size/libmsff.so.1.0.0` 4,235,076，SHA-256 `7a75709520958a1f2ac5c679fad2e4cd3f899c1d10f507d0120cb011ff8f880b`，mtime 2026-11-21 12:17:30 CST。

重链后复查：Android 硬解播放在本机还能证明的实质缺口，已没有可在不重复 Surface/焦点、leftover/delay、缺 PTS 修剪的前提下再修的一块。剩余项都需要真机、隔离 AVD 或其它外部条件，本轮未再堆无增量 mock 用例，也未再跑官方 `./build.sh test`（本轮未改产品源码）。
