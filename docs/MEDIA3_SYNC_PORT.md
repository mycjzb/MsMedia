# Media3 音画同步逐函数对照表

日期：2026-11-25（Asia/Shanghai）。by: Long <mycjzb@gmail.com>

对照源码：`androidx/media` **main@101e97c**（与 2026-11-24 核对同一提交）。
原始版权 The Android Open Source Project，许可 **Apache License 2.0**。
本仓库顶层许可为 LGPL-2.1，与 Apache-2.0 组合的兼容性风险见 `docs/THIRD_PARTY.md`（不改 `LICENSE`）。

本轮目标：按 Media3 的算法、常量、分支顺序和状态变量**逐函数移植**，C/Java 函数与字段尽量保留 Media3 原名。上一轮（a8bb38f）的自创简化全部撤回并按原版补齐。

符号约定：`C.TIME_UNSET = Long.MIN_VALUE+1`，`C.INDEX_UNSET = -1`，`C.MICROS_PER_SECOND = 1_000_000`，`C.NANOS_PER_SECOND = 1_000_000_000`。

---

## 1. 音频钟

| Media3 类/方法 | 常量/要点 | 本仓库对应 | 照搬程度 |
| --- | --- | --- | --- |
| `AudioTimestampPoller` 全部状态 `STATE_INITIALIZING/TIMESTAMP/TIMESTAMP_ADVANCING/NO_TIMESTAMP/ERROR` | `FAST_POLL 10ms`，`SLOW_POLL 10s`，`ERROR_POLL 500ms`，`INITIALIZING_DURATION 500ms`，`WAIT_FOR_ADVANCE 2s`，`MAX_POSITION_DRIFT_ADVANCING 1ms`，`MAX_AUDIO_TIMESTAMP_OFFSET 5s` | `src/hw/ms_media3_audio_clock.c`：`AudioTimestampPoller_*`（结构字段同名） | **完全照搬**状态机、轮询间隔、前进判定、合理性检查、回绕计数（`AudioTimestampWrapper`） |
| `AudioTimestampPoller.maybePollTimestamp` | 未到间隔且非 force 则返回；先 `maybeUpdateTimestamp` 再按 state 切换 | 同上 | **完全照搬** |
| `AudioTimestampPoller.getTimestampPositionUs` / `computeTimestampPositionUs` | `timestampPosition + getMediaDurationForPlayoutDuration(now - tsSystem, speed)` | 同上 | **完全照搬** |
| `AudioTimestampPoller.hasAdvancingTimestamp` / `isWaitingForAdvancingTimestamp` / `reset` / `expectTimestampFramePositionReset` | | 同上 | **完全照搬** |
| `AudioTrackPositionTracker.getCurrentPositionUs` | 优先 advancing timestamp，否则 `getPlaybackHeadPositionEstimateUs`；不超过 `writtenFrames`；播放中按 ±10%、漂移 <1s 平滑 | `AudioTrackPositionTracker_getCurrentPositionUs` | **完全照搬** |
| `AudioTrackPositionTracker.maybeSampleSyncParams` | 30ms 采 head 偏移，最多 10 个样本平均；采完后更新 latency 并 poll timestamp | 同上 | **完全照搬** |
| `AudioTrackPositionTracker.maybeUpdateLatency` | 反射 `AudioTrack.getLatency()`×1000 − `bufferSizeUs`，间隔 500ms（源码 `MIN_LATENCY_SAMPLE_INTERVAL_US = 50_0000`），>10s 置 0 | 同上；JNI 取隐藏方法，失败永久关闭 | **完全照搬**（获取方式从反射改为 JNI `GetMethodID`，语义相同） |
| `getPlaybackHeadPositionEstimateUs` | 无样本时用 raw head；有样本时 `getMediaDurationForPlayoutDuration(systemTimeUs + smoothedOffset, speed) - latencyUs` | 同上 | **完全照搬** |
| `getPlaybackHeadPosition` / `updateRawPlaybackHeadPosition` | 32 位回绕、`expectRawPlaybackHeadReset`、API≤29 头卡 0 的 Bluetooth workaround（200ms `isStalled`） | 同上 | **完全照搬** |
| `start` / `pause` / `reset` / `handleEndOfStream` / `getSimulatedPlaybackHeadPositionAfterStop` / `setAudioTrackPlaybackSpeed` | stop 后按墙钟模拟位置 | 同上 | **完全照搬** |
| `DefaultAudioSink.getCurrentPositionUs` | 未初始化或 `startMediaTimeUsNeedsInit` 返回 `TIME_UNSET`；否则 `min(trackPos, writtenDur)` 再 `applySkipping(applyMediaPositionParameters(...))` | `DefaultAudioSink_getCurrentPositionUs` | **完全照搬**主路径 |
| `DefaultAudioSink.applyMediaPositionParameters` | checkpoint 队列；空队列时用 processor 链的实际媒体时长并记 drift | 同上 | **算法照搬**；`AudioProcessorChain.getMediaDuration` **无法照搬**：MsMedia 没有 Sonic/变速音频处理器，空队列时用 `getMediaDurationForPlayoutDuration(playout, speed)`（恒速且无处理器时与原版等价） |
| `DefaultAudioSink.applySkipping` / `maybeRecordSkippedSilence` | 跳过静音累加到媒体位置 | 提供空实现（checkpoint 队列始终为空） | **无法照搬原因**：没有 `SilenceSkippingAudioProcessor`，产品未实现跳过静音；函数保留，恒为恒等 |
| `startMediaTimeUs` 在首包 `handleBuffer` 中初始化/不连续校正（>200ms） | 首包 PTS 作为媒体起点 | `DefaultAudioSink_handleBufferTimestamps` | **完全照搬**时间轴部分（不移植音频处理管线本身） |
| `DefaultMediaClock.syncAndGetPositionUs` / `shouldUseStandaloneClock` | 无 renderer 钟、已结束、未 ready、在读 ahead 时用独立钟；切换时禁止时间回跳 | `DefaultMediaClock_*` | **完全照搬**；「audio 未 ready」映射为 `getCurrentPositionUs==TIME_UNSET` 或音频会话不存在 |
| `StandaloneMediaClock.start/stop/resetPosition/getPositionUs` | 已启动则按 `elapsedRealtime` 与速度前进 | `StandaloneMediaClock_*` | **完全照搬** |
| `Util.getMediaDurationForPlayoutDuration` / `getPlayoutDurationForMediaDuration` | `media = playout * speed`，`playout = media / speed` | `ms_media3_getMediaDurationForPlayoutDuration` 等 | **完全照搬** |

上轮自创的「200ms 头不动就用墙钟兜底当主钟」**已删除**，不再在 preroll 期间把钟跑到十几秒。AVD `-no-audio` 时 AudioTrack 头停在 0：按 Media3 会返回接近 0 的位置或 `TIME_UNSET`，时钟改由 `StandaloneMediaClock` 在 **start() 之后**前进（这是 DefaultMediaClock 在 renderer 未提供有效钟时的原版行为，不是另起一套兜底公式）。

---

## 2. 视频出帧

| Media3 类/方法 | 常量/要点 | 本仓库对应 | 照搬程度 |
| --- | --- | --- | --- |
| `MediaCodecVideoRenderer.processOutputBuffer` | 先 `frameRateEstimator.onNextFrame`，再 `getFrameReleaseAction`，再按 action 分支 render/drop/skip/ignore/try later | `processOutputBuffer`（`ms_media3_video_release.c`）+ 平台层执行 release | **完全照搬**分支顺序（不移植 VideoSink/扩展编码器等本工程没有的路径） |
| `VideoFrameReleaseControl.getFrameReleaseAction` / `getFrameReleaseActionInternal` | 见第 3 节全分支 | `VideoFrameReleaseControl_getFrameReleaseAction` | **完全照搬** |
| `calculateEarlyTimeUs` | `(pts - positionUs) / speed`；已 started 再扣 `elapsedRealtime()-elapsedRealtimeUs` | 同上 | **完全照搬**。`positionUs`/`elapsedRealtimeUs` 在 `ms_hw_player_drive` **循环开头**各拍一次，不再在出帧函数里现取钟 |
| `shouldForceRelease` + `FIRST_FRAME_*` | 0=仅 started 才允许；1=`NOT_RENDERED` 立即；2=等 `position>=streamStart`；3=已出过首帧后走 `shouldForceReleaseFrame` | 同上，状态机按原版 | **完全照搬**。首帧立即出 / Surface 重绑后出当前帧 / seek 后出当前帧，分别对应 `NOT_RENDERED` 与 `setOutputSurface`/`reset` 把状态降回 `NOT_RENDERED` |
| `MediaCodecVideoRenderer.shouldDropOutputBuffer` | `earlyUs < -30_000 && !isLast` | `shouldDropOutputBuffer` | **完全照搬** |
| `shouldDropBuffersToKeyframe` | `earlyUs < -500_000 && !isLast` | 同上 | **完全照搬** |
| `shouldForceRenderOutputBuffer` | `earlyUs < -30_000 && elapsedSinceLastRenderUs > 100_000` | 同上 | **完全照搬** |
| `shouldIgnoreFrame` | `shouldDropBuffersToKeyframe && maybeDropBuffersToKeyframe` | 同上 | **完全照搬** |
| `maybeDropBuffersToKeyframe` | **`skipSource(positionUs)==0` 则返回 false，不 flush**；否则跳过源、flush 解码器（含当前输出）、记 `droppedToKeyframeCount` | `maybeDropBuffersToKeyframe`：丢掉尚未送入 MediaCodec、PTS < positionUs 的待输入 AU；0 个则不 flush | **条件完全照搬**。`skipSource` 只能看到尚未入解码器的输入队列，无法跳过已在 MediaCodec 内部的缓冲（与 Media3 的 SampleQueue 范围相同：只跳源，入机内的靠 flush） |
| `VideoFrameReleaseHelper.adjustReleaseTime` | 按上一帧调整时刻 + 帧间隔（优先 `frameDurationNs`，否则 PTS 差）推算，偏差 ≤20ms 才采用；再 VSYNC 吸附 | `VideoFrameReleaseHelper_adjustReleaseTime` | **完全照搬** |
| `findClosestVsyncAndUpdateHysteresis` | 60fps@90Hz 等「两档接近」时用 1/4 周期滞回，避免来回跳 vsync | 同上 | **完全照搬**（上轮未移植，本轮补齐） |
| `FixedFrameRateEstimator` 及其内部 `Matcher` | 连续 15 个匹配（差 ≤1ms）、5s 高置信、高低置信更新门限 0.1/1 fps、失同步 30 帧后清除 | `FixedFrameRateEstimator_*` | **完全照搬** |
| `VSyncSampler` / `VSyncSamplerBase` / `VSyncSamplerV33` | 独立线程 Choreographer，每 500ms 采一次；周期来自 `Display.getRefreshRate()`（API 33 用 `FrameTimeline` 差） | **Java** `com.mycjzb.media.player.media3.VSyncSampler`（贴近原版，C 只读采样结果） | **完全照搬**采样方式与刷新率来源。上轮用相邻回调帧差估周期，已删除 |
| `VideoFrameReleaseHelper.updateSurfacePlaybackFrameRate` / `Api30.setSurfaceFrameRate` | API 30+ `Surface.setFrameRate` | Java `VideoFrameReleaseHelper` 在 `onStarted`/`onSurfaceChanged`/`setSurfaceMediaFrameRate` 中调用 | **完全照搬**（有 Java Surface 时） |
| `renderOutputBufferV21` | `codec.releaseOutputBuffer(index, releaseTimeNs)` | NDK `AMediaCodec_releaseOutputBufferAtTime` / JNI `releaseOutputBuffer(IJ)V`（API 21+） | **完全照搬** |
| ExoPlayer **release-v2@dd430f7** `renderOutputBuffer`（API&lt;21） | `earlyUs&lt;30ms` 时若 `>11ms` 则 `Thread.sleep((earlyUs-10000)/1000)` 再 `release(index,true)` | `renderOutputBuffer_legacy` | **按 ExoPlayer 2.x 照搬**。Media3 main 已是 minSdk 21，没有这条路径。**无法用 main 照搬的原因**：本工程仍支持 API 18–20，只能回退到仍支持该 API 的 ExoPlayer 2.x 实现。睡眠最多约 20ms；音频写出已在独立写线程，不阻塞 AudioTrack |

---

## 3. `getFrameReleaseActionInternal` 全分支（与原版同一顺序）

1. `onlyProbe==false` 且已 started 且 `initialPositionUs==UNSET` → 记下 `initialPositionUs = positionUs`
2. `earlyUs = calculateEarlyTimeUs(...)`
3. `isDecodeOnlyFrame && !isLastFrame` → `FRAME_RELEASE_SKIP`
4. 无 Surface 且需要 Surface：`shouldIgnoreFrame(..., treatDropped=true)` → `IGNORE`；已 started 且 `earlyUs < 30_000` → `SKIP`；否则记 `frameReadyWithoutSurface`，`TRY_AGAIN_LATER`
5. `shouldForceRelease(...)` → `IMMEDIATELY`（并 `updateReleasedFrameState`）
6. 未 started，或 `initialPositionUs==UNSET`，或 `positionUs==initialPositionUs`（时间戳尚未前进；可用 `disableAdvancingTimestampChecks` 跳过）→ `TRY_AGAIN_LATER`
7. `unadjustedReleaseTimeNs = nanoTime + earlyUs*1000`；`releaseTimeNs = adjustReleaseTime(...)`；重算 `earlyUs`
8. joining 期间 `treatDropAsSkip`
9. `shouldIgnoreFrame`（即很晚且 `maybeDropBuffersToKeyframe` 成功）→ `IGNORE`
10. `shouldDropFrame`（`earlyUs < -30ms`）→ `DROP`（joining 时改 `SKIP`）
11. `earlyUs > earlySchedulingThresholdUs`（默认 50ms）→ `TRY_AGAIN_LATER`
12. `skipBuffersWithIdenticalReleaseTime` 且释放时刻与上一帧相同 → `DROP` 或 `SKIP`
13. 否则 `FRAME_RELEASE_SCHEDULED`

---

## 4. 上轮简化对照（必须按原版补齐的点）

| 上轮简化 | 原版 | 本轮 |
| --- | --- | --- |
| 落后 &lt; -500ms **总是** flush 并等关键帧 | 仅当 `skipSource` 真正跳过了源数据才 flush | 已按原版 |
| 刷新周期用相邻 Choreographer 回调时间差 | `Display.getRefreshRate()`（API 33 用 FrameTimeline） | Java `VSyncSampler` 已按原版 |
| 未移植 60fps@90Hz 滞回 | `findClosestVsyncAndUpdateHysteresis` | 已按原版 |
| API&lt;21 改成“稍后重询”、不 sleep | ExoPlayer 2.x：&lt;30ms 内 sleep 到提前 10ms 再 render | 已按 2.x 照搬（main 无此路径，原因见上表） |
| 32 位 API 24–28 / 全部 API 21–23 不采 VSYNC | Java Choreographer 自 API 16 可用，Display 刷新率自 API 17 可用 | 改为 Java 采样，API 18 即可采 |
| 出帧函数里现取音频钟，不扣循环耗时 | 循环开头拍 `positionUs`+`elapsedRealtimeUs`，`calculateEarlyTimeUs` 扣除 | 已按原版 |
| 自创 200ms 墙钟兜底当主钟（preroll 就会把钟跑飞） | 未初始化返回 `TIME_UNSET`，由 `StandaloneMediaClock` 在 start 后前进 | 已删除自创兜底 |
| 函数/变量名自拟（`at_*`、`sync_*`） | Media3 原名 | 本轮用 Media3 原名 |

---

## 5. 无法照搬或只部分照搬（及原因）

1. **`AudioProcessorChain` / Sonic 变速 / 跳过静音**：MsMedia 没有这条 PCM 处理链。`applyMediaPositionParameters` 在无 checkpoint 时用速度公式；`applySkipping` 恒等。恒速、不跳静音时与原版数值相同。
2. **`VideoSink` / `ExtensionCodecAdapter` / 隧道模式 / AV1 扩展解码器**：产品没有这些路径，`processOutputBuffer` 只保留 Surface + MediaCodec 主路径。
3. **`skipSource` 的 SampleQueue**：Media3 跳的是渲染器尚未送入解码器的样本队列；本工程等价物是播放器/解码器的待输入槽。已进入 MediaCodec 的缓冲两边都不能逐个跳过，只能 flush。
4. **Media3 main 的 API&lt;21 出帧**：main 最低 API 21。API 18–20 使用 ExoPlayer 2.x 的 sleep+render 路径。
5. **`onPositionAdvancing` 回调 / 多渲染器时钟互斥异常**：没有 ExoPlayer 的监听器体系，推进回调为空操作；本播放器只有一个音频钟来源。
6. **`Clock` 可注入**：生产路径用 `CLOCK_MONOTONIC`（与 `elapsedRealtime`/`nanoTime` 同为单调钟）。宿主单测通过 `ms_media3_set_test_clock` 注入，便于在没有设备时覆盖 SCHEDULED/VSYNC/丢帧分支。

---

## 6. 文件清单

| 文件 | 内容 |
| --- | --- |
| `src/hw/ms_media3.h` | 常量、结构、对外 API |
| `src/hw/ms_media3_audio_clock.c` | 音频钟全部 |
| `src/hw/ms_media3_video_release.c` | 视频出帧、帧率估计、释放辅助（含滞回） |
| `android/hw/java/.../media3/VSyncSampler.java` | Choreographer + Display 刷新率 |
| `android/hw/java/.../media3/VideoFrameReleaseHelper.java` | Surface.setFrameRate 与采样器生命周期（adjustReleaseTime 在 C，便于宿主单测） |
| `tests/test_media3_sync.c` | 宿主对照：阈值、滞回、maybeDropBuffersToKeyframe 的 0 跳过不 flush、SCHEDULED 计算 |

接入点：`ms_hw_audio.c`（Track 生命周期对接 PositionTracker / DefaultAudioSink）、`ms_hw_player.c`（DefaultMediaClock + 循环开头拍钟 + processOutputBuffer）、`ms_hw_android.c` / `ms_hw_jni.c`（按 action 调用 release/drop/atTime）。
