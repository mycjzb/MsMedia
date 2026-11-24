# com.msmedia.MediaPlayer 封装设计（阶段 1）

by: Long <mycjzb@gmail.com>

日期：2026-11-24 09:49 Asia/Shanghai。本文件只描述 API 与加载方式，**不是**设备出画 / 出声验收，也不改现有多 so Demo（`MsHwPlayerActivity`）和官方 `scripts/android_hw_build.sh`。

## 1. 目标

业务侧把

```java
import android.media.MediaPlayer;
```

换成

```java
import com.msmedia.MediaPlayer;
```

即可在系统播放器与自研播放器之间切换：类名、常用方法签名、参数、常量值、监听器与系统 `android.media.MediaPlayer` 对齐，不另起私有参数名（例如对外不用 `setRate(int num, int den)`，而用 `setPlaybackParams(PlaybackParams)`）。

底层**只** `System.loadLibrary("msplayer")`，对应统一交付文件 `libmsplayer.so`。自研线产物在 `build/android-v7a/single-native/libmsplayer.so`（及 v8a 对称目录），精简 FFmpeg 线预留 `build/android-v7a/single-slimff/libmsplayer.so`。Java 封装共用，不按路线分两套类。

建议包名 / 类名：**`com.msmedia.MediaPlayer`**（与系统类同名、不同包）。不放在 `android.media` 下，以免与框架类冲突。

## 2. 现有 Demo / JNI 与系统 API 的差距

现有 `MsHwPlayerActivity` 是演示 Activity，不是可替换的播放器类。加载与控制模型如下。

| 项 | 系统 `android.media.MediaPlayer` | 现有 Demo / JNI（`MsHwPlayerActivity` + `ms_hw_player_jni.c`） |
| --- | --- | --- |
| 加载 so | 无（框架内） | 依次 try-load `msmedia` / `msff` / `msass` / `msai`，最后必须 `mshw` |
| 入口类 | `MediaPlayer` 构造或 `create()` | Activity，`nativeCreate()` 返回不透明 `long` 句柄 |
| 设置源 | `setDataSource` 多重大载后再 `prepare` / `prepareAsync` | `nativePlayPath` / `nativeOpenUrl`：入队后工作线程立刻打开并推 AU，没有 prepare 阶段 |
| 开始 / 暂停 / 停止 | `start` / `pause` / `stop` / `reset` / `release` | `nativePause` / `nativeResume`；无独立 `stop`；`nativeCancel` + `nativeDestroy` 近似 release；无 `reset` 合同 |
| 时间单位 | `int` 毫秒（`seekTo(int)`；API 26 另有 `seekTo(long, int mode)`） | `nativeSeek(long time90k)`、`nativeSeekRelative`；`nativePosition` 返回**字符串**（`pos=12.376s` 或 `pos_90k=`） |
| 时长 / 是否在播 | `getDuration()` / `isPlaying()` | 夹在 stats / position 字符串里的 `dur_90k`，无布尔 `isPlaying` |
| 音量 / 循环 | `setVolume(float,float)` / `setLooping` / `isLooping` | 无对应 JNI |
| 画面 | `setDisplay(SurfaceHolder)` / `setSurface(Surface)` / `setVideoScalingMode` / `getVideoWidth` / `getVideoHeight` | 仅 `nativeSetSurface`；比例在 Java 里用私有 `aspectMode` 改 layout，不是系统 scaling 常量；无宽高 getter |
| 速率 | `setPlaybackParams(PlaybackParams)` / `getPlaybackParams()`（API 23） | `nativeSetRate(num, den)` 私有有理数接口 |
| 监听器 | `OnPrepared` / `OnCompletion` / `OnError` / `OnInfo` / `OnSeekComplete` / `OnBufferingUpdate` / `OnVideoSizeChanged` | 无；Activity 轮询 `nativeStats` / `nativeDrive`，错误写在字符串里 |
| Uri / FD | `Context+Uri`、`FileDescriptor(+offset,length)`、headers | 只有 path 字符串与 URL 字符串（另加 ca 文件与超时，属私有扩展） |
| 字幕 / 选轨 / AI | 系统有 `addTimedTextSource` 等（能力不同） | 私有 `nativeLoadSubtitle` / `nativeSelectTrack` / ASS / AI，**不**放进阶段 1 的标准类对外合同（避免出现系统没有的方法名）；后续可用兼容方法或独立 `MsExtras` 挂上，以免「换 import 就能编过」被额外方法绑死 |

结论：现有 JNI 能覆盖播放/暂停/seek/设 Surface/速率的**能力**，但**不是**系统 MediaPlayer 的生命周期与类型合同。阶段 2 应在 `libmsplayer.so` 内为 `Java_com_msmedia_MediaPlayer_*` 增加一层薄 JNI（或把现有 player JNI 改成可被该类复用的内部函数），把 90 kHz 与毫秒互转关在 so 内；**不得**为迁就封装去改稳定 C ABI v1（`sizeof(ms_event)==176`）。

## 3. 阶段 1 已落地的骨架

路径：`android/hw/java/com/msmedia/MediaPlayer.java`

- 已按系统合同声明常用公开 API（见第 4 节）与全部点名的 listener / 常量（数值与 AOSP 一致）。
- 内部实现系统文档中的状态机：IDLE → INITIALIZED → PREPARING → PREPARED → STARTED / PAUSED / STOPPED / PLAYBACK_COMPLETED，以及 ERROR、release 后的 END。非法调用抛 `IllegalStateException`。
- `setDataSource` 各重载在 Java 侧完成 Uri（`file` / `content` / `android.resource`）到 path 或 `FileDescriptor` 的展开；http(s) 等只保存字符串，不写日志。
- `loadLibrary("msplayer")` 失败时 `prepare` 诚实抛 `IOException`，`prepareAsync` 走 `OnErrorListener`（`MEDIA_ERROR_UNSUPPORTED`），不假装已缓冲或已出画。
- 用 `android-34` / `--release 11` 单独编译到 `build/single-analysis/mediaplayer-classes/`，**未**加入 `scripts/android_hw_build.sh`，因此不会改变现行多 so APK。
- 现有 `MsHwPlayerActivity` 仍加载 `mshw` 等多 so，互不干扰。

尚未接线（阶段 2）：`prepare` 真正打开 `libmsplayer` 会话、`start` 推 AU、`seekTo` 落到原生、`getDuration` / 视频宽高 / 音量 / 循环 / `OnPrepared` 成功回调、AudioAttributes 与 wakelock。骨架里 `start`/`pause` 等在 PREPARED 之后只改状态，是为了把状态机先固定；在原生接通前不要把这些调用写成已播放。

## 4. 对齐清单（阶段 2 接线顺序）

必须对齐（签名与语义）：

- `setDataSource(String)` / `(String, Map)` / `(FileDescriptor)` / `(FileDescriptor, long, long)` / `(Context, Uri)` / `(Context, Uri, Map)` / `(Context, Uri, Map, Map)`
- `prepare` / `prepareAsync`
- `start` / `pause` / `stop` / `reset` / `release`
- `seekTo(int)` / `seekTo(long, int mode)`（`SEEK_PREVIOUS_SYNC` 等）
- `getCurrentPosition` / `getDuration` / `isPlaying`
- `setVolume` / `setLooping` / `isLooping`
- `setDisplay` / `setSurface` / `setVideoScalingMode` / `getVideoWidth` / `getVideoHeight`
- 七个标准 listener
- `setPlaybackParams` / `getPlaybackParams`
- 工厂 `create(Context, Uri)` / `(Context, Uri, SurfaceHolder)` / `(Context, int resid)`
- 常量：`MEDIA_ERROR_*`、`MEDIA_INFO_*`、`VIDEO_SCALING_MODE_*`

建议随后对齐、但不挡「换 import」：`setAudioStreamType`、`setAudioAttributes`、`setScreenOnWhilePlaying`、`getTrackInfo` / `selectTrack` / `deselectTrack`。`MediaDataSource`（API 23）、`setDataSource(AssetFileDescriptor)`（API 24）可按需补重载。

明确不在标准类上暴露的私有能力：强制解码器名、CA 文件路径、live-edge seek、ASS 画布、AI 字幕供应商。需要时另开 `com.msmedia.MediaPlayerExtras` 或包内可见方法，避免污染系统合同。

## 5. 加载方式

单 so APK（后续）：

```java
static { System.loadLibrary("msplayer"); } // 已在 MediaPlayer 构造路径里完成
```

Java 只依赖这一个文件名。自研线与精简 FFmpeg 线替换 so 即可，不必改 Java。可选 `libmsass.so` / `libmsai.so` 仍由 native 侧 `dlopen`，缺失则降级，不进入 `NEEDED`，也不由 Java 强行 `loadLibrary`。

现有多 so Demo 保持原加载顺序，直到单独做单 so 演示 APK 时再切到本类。
