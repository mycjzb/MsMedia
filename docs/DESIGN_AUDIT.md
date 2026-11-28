# MsMedia 设计审计（阶段 6，路线 B 主力单 so）

日期：2026-11-24（Asia/Shanghai）。by: Long <mycjzb@gmail.com>

对照对象：`docs/DESIGN.md`、`FEATURE_MATRIX.md`、`COMPATIBILITY.md`、`FFMPEG_BACKEND.md`、`HW_ANDROID.md`、
`ROADMAP.md`、`SECURITY.md`、`THIRD_PARTY.md`、`PROGRESS.md`、`include/msmedia/msmedia.h`、`android/hw/java/com/mycjzb/media/player/`。
审计产物：`build/android-*/single-slimff-adec-s6/oz-flto/libmsplayer.so`（精简 FFmpeg 9.1 s6 + 裁剪 OpenSSL 3.5.9 静态 + Media3 风格 HLS 预下载）。

分三类：

- **A 已实现并验证**：有宿主 ASan/UBSan/LSan 或模拟器（API 18 / API 34）证据；
- **B 缺失 / 仅桩 / 边界缺陷**：标明本轮是否已修；
- **C 需真机**：模拟器无法证明，列入真机清单。

## 1. 总表

| 设计点 | 类 | 现状与证据 |
| --- | --- | --- |
| 单 `libmsplayer.so`，NEEDED 只有系统库 + libz | A | `scripts/single_s6_verify.sh` 4 ABI 全过：NEEDED = libandroid/libc/libdl/liblog/libm/libz，无 libssl/libcrypto，无 RELR（API 18 可加载） |
| `MediaPlayer` 1:1：setDataSource/prepare(Async)/start/pause/stop/reset/release/seekTo/位置时长/音量/循环/倍速/各 Listener | A | Java 层同名同语义；模拟器生命周期（seek/暂停/恢复/释放）回归通过 |
| `MediaPlayer` 1:1：`getTrackInfo`/`selectTrack`/`deselectTrack`/`addTimedTextSource`/`OnTimedTextListener`/`OnSubtitleDataListener` | **已修（2026-11-24 阶段 7）** | 已加 `getTrackInfo`/`selectTrack`/`deselectTrack`/`getSelectedTrack`/`addTimedTextSource`/`OnTimedTextListener` 与 `TrackInfo`/`TimedText`，复用内核 `pj_select_track`/`pj_cues_now_locked`/`pj_load_ext_sub`（与演示 Activity 同一路径）。仍未做：`OnSubtitleDataListener`、位图字幕回调、多个外挂文件、`TrackInfo` Parcelable |
| `MediaPlayer` 1:1：`setNextMediaPlayer`/`getAudioSessionId`/`attachAuxEffect`/DRM/`MediaDataSource` | B（刻意不做） | 盒子直播点播场景不用；调用方需注意 |
| 单连接探测（一次连接内完成探测与播放，不二次拉取） | A | `ms_ff` 自定义 io_open + FFmpeg 探测复用同一 AVIO；HLS Keep-Alive 统计 `conn_open=1 conn_reuse=4`（5 片） |
| 不看扩展名 + 伪装头剥离（PNG/JPEG 头包 TS/MP4/FLV/MKV，HLS 切片伪装 .png/.jpg） | A | 宿主 `hls_png_seg`/`hls_fmp4_jpg`/`png_mkv`/`png_ts` 通过；API 18/34 原生单测 png_mkv、hls_png 通过；预下载路径同样经过剥离（预下载只缓存原始字节，剥离仍在 demux 前统一做） |
| 8 个音频软解：AAC、AAC-LATM、AC-3、E-AC-3、MP2、DTS、G.711A、G.711μ | A | `config_components` s6 == s5，verify 逐项断言 8 个且只有 8 个 |
| 字幕：SRT/WebVTT/ASS/mov_text/PGS/DVB | A（内核）/ B（Java 未开放） | demuxer/codec 名在 so 内；宿主 `test_ff_backend`、`single_dvb_e2e` 已有；Java 接口见上 |
| 海思 AVS+ | C | cavsvideo parser 在；出画依赖海思 OMX，需真机 |
| AC-3/E-AC-3/DTS 透传与回落 | A（逻辑）/ C（出声） | `nativeSetAudioPassthrough`，不支持时回落软解；真实 HDMI 功放需真机 |
| `MsHttpClient`：严格 TLS、DNS 覆盖/回调、SOCKS5（含认证/远端解析） | A | 宿主 netcore 测试；s6 新增：自签 HTTPS 被拦截（`open=-8`），带 CA 时 200，CA 对但主机名不符仍拦截；API 18 上严格校验访问 `https://www.google.com/` 返回 200（系统 CA 目录） |
| 播放拉流 TLS 默认忽略证书、可切严格 | A | 宿主与 API 18/34：自签 HTTPS HLS、自签 HTTPS + AES-128 可播；`setPlaybackTlsVerify(true)` 时拦截 |
| 音画同步对齐 Media3（音频主钟、getTimestamp、定时出帧 + VSYNC、迟到丢帧） | **已改（2026-11-24 阶段 8）** / C（口型） | 见第 7 节。宿主 `build.sh test` 与 s6 校验通过；API 34 模拟器（TCG、`-no-audio`）证实取钟/延迟/VSYNC 采样/强制释放/丢到关键帧路径在跑，但解码只有约 0.5–2 fps，追不上实时，定时释放（`scheduled`）路径未被触发；真机口型未验 |
| HLS AES-128（显式 IV / 序号 IV） | A | 宿主 + API 18/34：`aes`、`aes_seq`、弱网 AES、seek AES、切台压力 AES 均通过 |
| HLS SAMPLE-AES / DRM | B（刻意不做） | 遇到即停止预下载规划，交回 FFmpeg 原生路径（报不支持） |
| Media3 风格 HLS 缓冲（4 水位 + 字节上限、滞回、首片边下边播、重缓冲门槛、重试、取消） | A | 见第 3 节 |
| API 34 上 slimff HLS 偶发 -1004 | A（已修） | 根因与修复见第 4 节；模拟器 5 轮 HLS 0 次 -1004 |
| API 18–34 兼容 | A（模拟器） | API 18（x86）与 API 34（x86_64）模拟器；arm 真机需 C |
| 并发与零泄漏 | A | 宿主 LSan：30 轮切台、15 轮弱网 AES 切台、取消、seek 全部零泄漏；ASan 抓到的预下载线程 UAF 已修（第 3 节） |
| 硬解出画（MediaCodec）、4K/HDR、老盒子 OMX 兼容 | C | 模拟器只有软件 codec |
| 低内存盒子长时间播放（数小时） | C | 需真机 |

## 2. 本轮修掉的缺陷

1. **-1004（API 34 偶发）**：主输入打开对瞬时网络错误加退避重试（见第 4 节）。
2. **预下载线程 use-after-free**（ASan 发现）：下载线程在 `avformat_close_input` 后仍读 `ic->protocol_whitelist`。现在创建时拷贝白/黑名单与中断回调，线程不再持有 `AVFormatContext`。
3. **严格 TLS 失败会被当成网络抖动重试 6 s**：`tls_android.c` 把证书/主机名失败改报 `EPROTO`（不在瞬时错误表），严格模式拦截从 6.5 s 降到约 1 s。
4. **IP 直连的严格校验**：OpenSSL `SSL_set1_host` 对 IP 走 SAN IP 匹配；测试证书改为 SAN=IP:127.0.0.1，并用 localhost 验证主机名不符被拦截。

## 3. Media3 风格 HLS（实现要点）

- 接口：C `ms_ff_set_buffer_config` / `ms_ff_get_buffer_stats` / `ms_ff_set_tls_verify`；JNI `nativeSetBufferConfig` / `nativeSetTlsVerify`；Java `setBufferConfig(min, max, play, rebuf, bytes)`，保留 `setPreloadConfig`（未调 `setBufferConfig` 时映射过去）。
- 默认值：15000 / 30000 / 1500 / 3000 ms，16 MiB（上限 256 MiB）。`maxBufferMs=0` 关闭预下载。
- 结构：每个 playlist 一条下载线程，有界单元队列（≤256 单元）；hls.c（`MS_SLIM_S6` 补丁）在打开当前切片时下发「从当前片起」的计划，`io_open` 命中则从内存读，未下完边下边读。
- 支持 EXT-X-MAP、BYTERANGE（连续区间与 hls.c 同样合并为一个请求）、AES-128（复用 libavutil `av_aes` 流式 CBC，去 PKCS#7）。
- 滞回：达到 max 时长或字节暂停，跌破 min 恢复；队首单元总是允许下载（因此字节上限可能被在途的一片超出，测试中 300 KB 上限峰值约 489 KB）。
- 重试：每片 3 次，50/100/200 ms，断点续传；4xx 不重试。清单刷新失败按 min(200ms<<n, max(target/2,100ms)) 退避 3 次；切片打开失败 25 ms 起指数退避。直播刷新间隔沿用 hls.c（≈ targetDuration，失败时退避到 0.5×target）。
- 取消：宿主测得 6–15 ms，API 18 33 ms；销毁时 join 全部线程。
- Keep-Alive：同线程复用连接（`ff_http_do_new_request2`）。

## 4. -1004 根因与修复

证据 `build/single-analysis/s4-emu/api34-slimff.full.log`：打开 m3u8 时 TCP connect 1 s 内被拒 3 次（测试 HTTP 服务器 backlog=5，前一个系统播放器刚用并发连接占满队列，模拟器 NAT 回 RST），FFmpeg 放弃 → `avformat_open_input -111` → `MEDIA_ERROR_IO(-1004)`。
修复：`ms_ff.c open_input_with_retry()` 对 ECONNREFUSED/RESET/ETIMEDOUT/EIO/5xx 等瞬时错误再退避 300/700/1500 ms 重试，可被取消和总超时打断；4xx、格式错误、证书失败立即失败。加上 hls.c 清单/切片重试与预下载重试。

## 5. OpenSSL 与 FFmpeg 算法复用

- HLS AES-128 解密用 FFmpeg 自带 `av_aes`（预下载器和 FFmpeg crypto 协议共用一份），没有引入第二份 AES。
- TLS 必须用 OpenSSL 自己的 provider 实现（EVP 接口、GCM、ECDSA/RSA、HKDF）；FFmpeg 的 `av_aes`/`av_sha`/`av_md5` 无法插进 OpenSSL。两者重叠只有 AES/SHA/MD5 几 KB 级，不值得改 OpenSSL 源码去复用。

## 6. 真机清单（C 类）

1. 海思/晶晨/全志盒子 API 18–34：TS/MP4/HLS/MKV 硬解出画，4K HEVC、10-bit HDR。
2. AVS+ 频道（海思）出画。
3. HDMI/光纤功放：AC-3/E-AC-3/DTS 透传出声；拔掉功放后回落软解。
4. 真实 HTTPS 直播源：默认忽略证书可播；`setPlaybackTlsVerify(true)` 对正规证书可播、对自签拦截。
5. `MsHttpClient` 访问业务 HTTPS 接口（老盒子 Android 4.4 系统 CA 较旧，注意 Let's Encrypt ISRG Root X1）。
6. 弱网（限速/丢包）下 HLS 卡顿次数与恢复时间，对比系统播放器。
7. 低内存盒子（512 MB/1 GB）连续播放 4 小时，观察 RSS。
8. 频繁换台（遥控器连按）200 次，无 ANR、无崩溃、无 fd 泄漏。

## 7. 音画同步对齐 Media3（阶段 8，2026-11-24）

核对源码：`androidx/media` main@`101e97c`（当日 `git ls-remote`），文件
`AudioTrackPositionTracker.java`、`AudioTimestampPoller.java`、`VideoFrameReleaseControl.java`、
`VideoFrameReleaseHelper.java`、`MediaCodecVideoRenderer.java`；API < 21 参照 ExoPlayer `release-v2`@`dd430f7`
的 `MediaCodecVideoRenderer`。

| 项 | Media3 值（源码常量） | 本实现 |
| --- | --- | --- |
| 迟到丢帧 | `MIN_EARLY_US_LATE_THRESHOLD = -30_000` | earlyUs < -30 ms 且非末帧：`releaseOutputBuffer(index, false)`，计 `video_au_dropped_late` |
| 严重落后 | `MIN_EARLY_US_VERY_LATE_THRESHOLD = -500_000` | flush 解码器并等下一关键帧，计 `video_drop_to_keyframe`（Media3 还要求源里有可跳过的数据，此处简化为总是执行） |
| 超前暂缓 | `DEFAULT_EARLY_SCHEDULING_THRESHOLD_US = 50_000` | earlyUs > 50 ms：输出缓冲留在解码器（最多 1 个），下轮再判，计 `video_au_held`（每缓冲计一次） |
| 强制释放 | `shouldForceRenderOutputBuffer`：迟到且距上次释放 > 100_000 us | 同；计 `video_au_late` |
| 定时出帧 | `releaseOutputBuffer(index, releaseTimeNs)` | API 21+ `AMediaCodec_releaseOutputBufferAtTime`（dlsym）；计 `video_release_scheduled` |
| 帧节奏平滑 | `MAX_ALLOWED_ADJUSTMENT_NS = 20_000_000` | 同（用 PTS 差，无帧率估计器） |
| VSYNC 吸附 | 最近 vsync − `VSYNC_OFFSET_PERCENTAGE = 80`% 周期；`VSYNC_SAMPLE_UPDATE_PERIOD_MS = 500` | 同；采样用常驻 ALooper 线程 + `AChoreographer`（API 24+，32 位需 API 29），周期由相邻两帧差量得；未移植 60fps@90Hz 滞回 |
| 时间戳轮询 | `FAST 10 ms / SLOW 10 s / ERROR 500 ms`，`INITIALIZING 500 ms`，`WAIT_FOR_ADVANCE 2 s`，前进判定漂移 < 1 ms，合理性 ±5 s | 同，状态机逐条对应；API 18 不调用 `getTimestamp` |
| head 平滑 | 10 个样本 / 30 ms，漂移 < 1 s 时限 ±10% 速度变化 | 同 |
| 输出延迟 | 反射 `getLatency()`×1000 − 缓冲时长，每 500 ms，> 10 s 视为无效 | JNI 取隐藏方法 `getLatency`，失败永久关闭；仅 PCM；时间戳模式不扣 |
| API < 21 | ExoPlayer 2.x：earlyUs < 30 ms 时，若 > 11 ms 先 sleep 到约 10 ms 再 render | 不在持锁线程 sleep：earlyUs > 11 ms 返回“稍后”，到点 `releaseOutputBuffer(index, true)` |

保留的既有行为：首帧不等音频立即出（Media3 `FIRST_FRAME_NOT_RENDERED` 亦强制释放）；暂停时只在首帧 / Surface 重绑 / seek 后出当前帧，其余握住；
无音频钟（无音轨、不支持、关声、尚未出声）直接出帧；AudioTrack 头 200 ms 不动仍走单调钟兜底（`audio_clock_source=4`）。
mock / VAAPI（宿主）不走回调，`drain_vpend` 的阈值同步改为 50 / 30 ms。2 s 无输出冲洗在解码器握着“太早”帧时不触发。

可观测：`ms_hw_player_stats` 新增 `sync_in_decoder`、`audio_clock_source`、`audio_latency_us`、`vsync_period_ns`、
`video_release_scheduled`、`video_drop_to_keyframe`、`video_last_early_us`（结构体尾部加法，ABI 不变）；
Android 上每 5 s 一行 logcat `MsSync`（不含 URL）。

未验证（C 类）：真机口型（耳听/高速摄像）、`getTimestamp` 在真机上进入 TIMESTAMP_ADVANCING 的比例、`getLatency` 在 API 28+ 隐藏接口限制下的可用性（API 34 模拟器可取到，约 90 ms）、
HDMI 直通（IEC61937）下的时钟、高刷新率（90/120 Hz）吸附效果、API 18–20 真机的“稍后再 render”节奏。


2026-11-25：音画同步改为按 androidx/media main@101e97c 逐函数移植（见 `docs/MEDIA3_SYNC_PORT.md`）。
仍未在真机验证口型同步、TIMESTAMP_ADVANCING 占比、HDMI 直通钟、高刷滞回实机效果。
模拟器 -no-audio 下音频头几乎不走，scheduled（提前 ≤50ms 定时释放）窗口很难命中；held 与 VSYNC 采样已在 API34 触发。
