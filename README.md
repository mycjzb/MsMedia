# MsMedia

Android 播放器 SDK：一套 `com.mycjzb.media.player.MediaPlayer` API，可切换 **自研引擎**（`shw:` / `libmsplayer.so`）与 **系统播放器**（`sys:`）。

**客户请先读：[docs/SDK接入指南.md](docs/SDK接入指南.md)**（接入、引擎切换、多音轨/字幕、限制）。

- 最低 Android 4.3（API 18）；ABI：armeabi-v7a、arm64-v8a、x86、x86_64
- 自研 so 运行时只依赖系统库和 `libz.so`（静态裁剪 OpenSSL + FFmpeg）

仓库：<https://github.com/mycjzb/MsMedia>

## 支持范围

| 类别 | 内容 |
| --- | --- |
| 协议 | file、http、https、HLS（含 AES-128、EXT-X-MAP、BYTERANGE、直播）、DASH、rtmp、rtmps、rtsp、udp、rtp |
| 封装 | MPEG-TS、MP4/MOV/fMP4、FLV、MKV/WebM、AVI、MPEG-PS、裸 AAC/AC-3/E-AC-3/DTS/MP3/FLAC/WAV |
| 视频 | H.264、H.265、MPEG-2、AVS+ 等，交给 MediaCodec 硬解（能解什么取决于设备）|
| 音频软解 | AAC、AAC-LATM、AC-3、E-AC-3、MP2、DTS、G.711 A-law、G.711 μ-law；其余格式交给系统解码器 |
| 音频透传 | AC-3 / E-AC-3 / DTS 直通功放，设备不支持时自动回落软解 |
| 字幕 | 内核可解 SRT、WebVTT、ASS/SSA、mov_text、PGS、DVB；文字字幕通过 `OnTimedTextListener` 回调，支持外挂 SRT/VTT/ASS |
| 其他 | 不看扩展名，按内容探测；切片/文件前面套了 PNG/JPEG 头的会自动剥掉 |

## 接入（摘要）

完整说明见 [docs/SDK接入指南.md](docs/SDK接入指南.md)。

1. 把 `libmsplayer.so` 按 ABI 放进 `src/main/jniLibs/<abi>/`。
2. 把下面几个 Java 文件拷进工程，包名不要改（JNI 按包名查找）：

```
android/hw/java/com/mycjzb/media/player/MediaPlayer.java
android/hw/java/com/mycjzb/media/player/PlayEngine.java
android/hw/java/com/mycjzb/media/player/SysPlayerBackend.java
android/hw/java/com/mycjzb/media/player/LiveDetect.java
android/hw/java/com/mycjzb/media/player/AspectMode.java
android/hw/java/com/mycjzb/media/player/MsAspectLayout.java
android/hw/java/com/mycjzb/media/player/TimedText.java
android/hw/java/com/mycjzb/media/player/MsNativeLoader.java
android/hw/java/com/mycjzb/media/player/MsHttpClient.java
```

3. 混淆规则：把 `android/hw/consumer-rules.pro` 加到 `proguardFiles`，或者直接：

```
-keep class com.mycjzb.media.player.** { *; }
-keepclasseswithmembernames class * { native <methods>; }
```

4. 需要网络播放的话，记得在 Manifest 里加 `android.permission.INTERNET`。

## 用法

### 引擎切换

```java
import com.mycjzb.media.player.MediaPlayer;
import com.mycjzb.media.player.PlayEngine;

MediaPlayer.setDefaultEngine(PlayEngine.SHW);    // 全局默认自研
MediaPlayer.setDefaultEngine(PlayEngine.SYSTEM); // 或默认系统

mp.setDataSource("shw:http://cdn/a.m3u8"); // 本条强制自研
mp.setDataSource("sys:http://cdn/b.ts");   // 本条强制系统
```

### 播放

和系统 MediaPlayer 一样，只是 import 换成 `com.mycjzb.media.player.MediaPlayer`：

```java
import com.mycjzb.media.player.MediaPlayer;

MediaPlayer mp = new MediaPlayer();
mp.setDataSource("https://example.com/live/index.m3u8");
mp.setSurface(surfaceView.getHolder().getSurface());
mp.setOnPreparedListener(p -> p.start());
mp.setOnErrorListener((p, what, extra) -> { Log.e("player", what + "/" + extra); return true; });
mp.prepareAsync();

// 退出时
mp.release();
```

`seekTo`、`pause`、`getCurrentPosition`、`getDuration`、`setLooping`、各种 Listener 用法都不变。

### 直播 / 点播与画面比例

```java
mp.setLiveHint(null);           // 自动；或 Boolean.TRUE/FALSE 强制
boolean live = mp.isLive();     // m3u8 无 ENDLIST / mpd 直播窗 → 直播；有结尾/静态 → 点播可 seek

mp.setAspectMode(AspectMode.AUTO); // auto|16:9|4:3|orig|fill
// 布局量算：MsAspectLayout 包 Surface，OnVideoSizeChanged → setVideoSize
```

详见 [docs/SDK接入指南.md](docs/SDK接入指南.md) §5–§6。

### 多音轨和字幕（自研 SHW）

和系统接口一样。`TimedText` 用本库的 `com.mycjzb.media.player.TimedText`（系统那个类没法由第三方创建）。

```java
import com.mycjzb.media.player.MediaPlayer;
import com.mycjzb.media.player.MediaPlayer.TrackInfo;
import com.mycjzb.media.player.TimedText;

mp.setDataSource(url);
// 外挂字幕可以在 prepare 前加，prepare 完成后生效
mp.addTimedTextSource("/sdcard/movie.srt", MediaPlayer.MEDIA_MIMETYPE_TEXT_SUBRIP);
mp.setOnTimedTextListener((p, text) -> {
    // 主线程回调；text 为 null 表示当前没有字幕，清掉显示
    subtitleView.setText(text == null ? "" : text.getText());
});
mp.setOnPreparedListener(p -> {
    TrackInfo[] tracks = p.getTrackInfo();
    for (int i = 0; i < tracks.length; i++) {
        if (tracks[i].getTrackType() == TrackInfo.MEDIA_TRACK_TYPE_AUDIO
                && "chi".equals(tracks[i].getLanguage())) {
            p.selectTrack(i);              // 切到中文音轨
        }
        if (tracks[i].getTrackType() == TrackInfo.MEDIA_TRACK_TYPE_TIMEDTEXT) {
            p.selectTrack(i);              // 打开字幕（默认不开）
        }
    }
    p.start();
});
mp.prepareAsync();

int audio = mp.getSelectedTrack(TrackInfo.MEDIA_TRACK_TYPE_AUDIO);
mp.deselectTrack(mp.getSelectedTrack(TrackInfo.MEDIA_TRACK_TYPE_TIMEDTEXT)); // 关字幕
```

说明：

- 下标就是 `getTrackInfo()` 数组的下标。文字字幕（SRT/WebVTT/tx3g/ASS）报 `TIMEDTEXT`，位图字幕（PGS/DVB/VobSub）报 `SUBTITLE`。
- 视频轨不能选，音轨不能取消，都会抛 `IllegalArgumentException`；prepare 之前调用抛 `IllegalStateException`，和系统一致。
- 外挂字幕支持 `MEDIA_MIMETYPE_TEXT_SUBRIP`，另外可以用 `MEDIA_MIMETYPE_TEXT_VTT`、`MEDIA_MIMETYPE_TEXT_SSA`。
- `TimedText` 额外有 `getStartTimeMs()`、`getEndTimeMs()`、`getAlignment()`。

### HLS 缓冲（参照 Media3 的 DefaultLoadControl）

HLS 会在后台线程提前下载后面的切片。不设置就用默认值，需要调的话在 `prepare` 之前：

```java
mp.setBufferConfig(
        15000,              // minBufferMs：缓冲低于它就继续下载
        30000,              // maxBufferMs：缓冲到这么多就暂停下载；传 0 关闭预下载
        1500,               // bufferForPlaybackMs：起播前至少缓冲多少
        3000,               // bufferForPlaybackAfterRebufferMs：卡顿后恢复前至少缓冲多少
        16L * 1024 * 1024); // maxBufferBytes：内存上限，最大 256 MiB
```

上面就是默认值。内存小的盒子可以把 `maxBufferBytes` 调到 4–8 MiB。
切片下载失败会重试 3 次（间隔 50/100/200 ms，从断点续传），4xx 不重试。

### 杜比 / DTS 透传

```java
mp.setAudioPassthroughEnabled(true);   // 在 prepare 之前调用
int caps = MediaPlayer.getAudioPassthroughCaps(context); // 当前输出设备支持哪些格式
```

设备或功放不支持时自动用软解，不会没声音。

### HTTPS 证书

- **播放拉流**（Media3 `JniHttpDataSource` / FFmpeg `rtmps`·`tls`）默认不校验证书（自签、IP HTTPS、过期根证书都能播）。
- **MsHttpClient** 默认严格校验（系统 CA + 主机名），给业务 API 用。播放专用可：

```java
new MsHttpClient.Builder().tlsInsecure(true).build();
```

### MsHttpClient

```java
MsHttpClient client = new MsHttpClient.Builder()
        .connectTimeout(10000)
        .readTimeout(15000)
        .build();

MsHttpClient.Request req = new MsHttpClient.RequestBuilder()
        .url("https://api.example.com/channels")
        .header("Authorization", "Bearer xxx")
        .get()
        .build();

try (MsHttpClient.Response r = client.newCall(req).execute()) {
    if (r.isSuccessful()) {
        String body = r.string();
    }
}
```

DNS 和 SOCKS5 是全局设置，播放器和 MsHttpClient 都生效（`configure` 会抛 `IOException`）：

```java
MsHttpClient.configure(new MsHttpClient.NetConfig()
        .hostOverride("cdn.example.com", "1.2.3.4")
        .socks5("10.0.0.2", 1080));
```

## 从源码编译

需要 Linux x86_64、与超级直播相同的 NDK **21.4.7075529**（默认 `$ANDROID_SDK/ndk/21.4.7075529`，见 `scripts/android_env.sh`）、Python 3、perl、gpg、curl。

常用入口与超级直播一样用 `-va`：

```bash
./scripts/build.sh                 # slimff 生产 so，双 ABI，默认 UPX
./scripts/build.sh -va v7a         # 仅 armeabi-v7a
./scripts/build.sh -va v8a         # 仅 arm64-v8a
./scripts/build.sh -va both        # 双 ABI（默认）
./scripts/build.sh -va all         # 依次打 v7a、v8a、both
./scripts/build.sh pure -va v7a    # 纯自研
./scripts/build.sh --pack 0        # 不压 so
./scripts/build.sh --publish       # 编完同步到 MsPlayer 仓库目录
./scripts/build.sh --deps          # 强制重编 OpenSSL/FFmpeg/msxml 前缀
```

底层分步（首次或缺前缀时 `build.sh` 也会自动编 deps）：

```bash
# 下载并校验 FFmpeg 9.1 / OpenSSL 3.5.9 官方源码（只需一次，放在 third_party/cache，不入库）
scripts/openssl_build.sh && scripts/ffmpeg9_build.sh

# OpenSSL（TLS 1.2/1.3 客户端最小集）
ABIS="armeabi-v7a arm64-v8a x86 x86_64" scripts/openssl_slim_build.sh

# 裁剪版 FFmpeg
TRIM=s6 ADEC=1 ADEC_TAG=s6 FF_OPT=oz-flto XML_FLAVOR=ms WITH_XML=1 \
  ABIS="armeabi-v7a arm64-v8a x86 x86_64" scripts/android_slimff_ffmpeg.sh

# libmsplayer.so（默认 PACK_SO=1：超级直播同款 UPX 压 so；PACK_SO=0 跳过）
MODE=slimff ADEC=1 ADEC_TAG=s6 OPT=oz-flto XML_FLAVOR=ms WITH_XML=1 TLS=openssl HLS_PF=1 \
  ABIS="armeabi-v7a arm64-v8a x86 x86_64" scripts/android_single_build.sh
# 输出：build/android-<abi>/single-slimff-adec-s6/oz-flto/libmsplayer.so
#       另留 libmsplayer.so.unpacked（压壳前）

# 检查 NEEDED / 导出符号 / 组件
scripts/single_s6_verify.sh

# 演示 APK
VARIANT=slimff SINGLE_SUB=single-slimff-adec-s6/oz-flto APK_OUT=$PWD/build/single-apk-s6 scripts/android_single_apk.sh

# 宿主测试
./build.sh test
tests/single_s6_hls_test.sh      # HLS 预下载 / AES-128 / HTTPS，ASan+UBSan+LSan
```

### 目录

```
android/hw/java/   Java 层（MediaPlayer、MsHttpClient）
android/single/    演示 App
include/msmedia/   C 头文件
src/hw/            MediaCodec / AudioTrack / JNI
src/ff/            FFmpeg 适配、HLS 预下载、tcp/tls 协议替换
src/netcore/       HTTP、DNS、SOCKS5、TLS（OpenSSL）
src/core src/codec src/container src/sub   自研解析、字幕
scripts/           构建脚本、FFmpeg/OpenSSL 裁剪补丁
tests/             宿主测试和测试服务
docs/              设计、审计、进度记录
```

## 体积

`-Oz` + Full LTO，已 strip，没有用 UPX：

| ABI | libmsplayer.so | zip 压缩后 |
| --- | ---: | ---: |
| armeabi-v7a | 2.66 MB | 1.52 MB |
| arm64-v8a | 3.00 MB | 1.53 MB |
| x86 | 3.06 MB | 1.59 MB |
| x86_64 | 3.25 MB | 1.56 MB |

其中 OpenSSL 约占 1.0–1.6 MB（未压缩）。

## 已知限制

- 硬解出画、HDMI 透传、AVS+ 只在模拟器和部分盒子上验证过，上线前请在目标设备上跑一遍 `docs/DESIGN_AUDIT.md` 里的真机清单。
- TLS 只保留 TLS 1.2/1.3 和 ECDHE（P-256/P-384）+ AES-GCM/CBC，不支持 ChaCha20、X25519、DHE 和 TLS 1.0/1.1。
- HLS 的 SAMPLE-AES 和 DRM 不支持；`maxBufferBytes` 可能被正在下载的那一片多占一点。
- 包名从 `com.msmedia` 改成了 `com.mycjzb.media.player`（`MsHttpClient` 也从 `com.msmedia.net` 移到这里），老代码改一下 import。
- 字幕：同一时间只能挂一个外挂字幕文件，再加会替换上一个；位图字幕可以选，但不通过 `OnTimedTextListener` 回调图片；没有 `OnSubtitleDataListener`；`TrackInfo` 没实现 `Parcelable`。
- 切音轨要等新音轨的数据读到才真正换过去（不会断音），`getSelectedTrack` 会立刻返回新选的轨。
- 不支持 iOS / Windows。

## License

MsMedia 代码按 LGPL-2.1 发布，全文见 [LICENSE](LICENSE)。
静态链接的 FFmpeg 9.1 是 LGPL-2.1-or-later（没有开启 GPL/nonfree），OpenSSL 3.5.9 是 Apache-2.0。分发 `libmsplayer.so` 时要附上这两者的版权声明并提供对应源码，详见 [docs/THIRD_PARTY.md](docs/THIRD_PARTY.md)。
