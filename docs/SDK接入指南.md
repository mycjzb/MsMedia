# MsMedia SDK 接入指南

by: Long <mycjzb@gmail.com>

## 1. 产品定位

| 项 | 说明 |
| --- | --- |
| 入口类 | `com.mycjzb.media.player.MediaPlayer` |
| 对齐 | 系统 `android.media.MediaPlayer` 常用签名 |
| 自研 so | `libmsplayer.so`（按 ABI 放入 `jniLibs`） |
| HTTP 工具 | 同包 `MsHttpClient`（与播放独立） |

默认引擎为 Media3（SHW）。so 提供 Http / FfIo / FfAdec / MsProbe / QuickJS 等 JNI。

## 2. 引擎切换

### 2.1 全局默认

```java
MediaPlayer.setDefaultEngine(PlayEngine.SHW);    // Media3（默认）
MediaPlayer.setDefaultEngine(PlayEngine.SYSTEM); // 系统播放器
int cur = MediaPlayer.getDefaultEngine();
```

只影响之后的 `setDataSource`。

### 2.2 URL 前缀

| 前缀 | 引擎 |
| --- | --- |
| `shw:` | Media3 |
| `sys:` | 系统 MediaPlayer |

大小写不敏感；前缀优先于全局默认，不进入真实请求。

```java
mp.setDataSource("shw:http://cdn.example.com/live/index.m3u8");
mp.setDataSource("sys:http://cdn.example.com/a.ts");
mp.setDataSource("http://cdn.example.com/b.m3u8"); // 用默认引擎
int eng = mp.getEngine();

String u = PlayEngine.with(PlayEngine.SYSTEM, "http://x/a.ts"); // sys:http://x/a.ts
String raw = PlayEngine.strip(u);
```

## 3. 接入步骤

1. `libmsplayer.so` → `src/main/jniLibs/<abi>/`
2. 拷贝 Java（**包名勿改**）：

```
com/mycjzb/media/player/MediaPlayer.java
com/mycjzb/media/player/PlayEngine.java
com/mycjzb/media/player/SysPlayerBackend.java
com/mycjzb/media/player/LiveDetect.java
com/mycjzb/media/player/AspectMode.java
com/mycjzb/media/player/MsAspectLayout.java
com/mycjzb/media/player/TimedText.java
com/mycjzb/media/player/MsNativeLoader.java
com/mycjzb/media/player/MsHttpClient.java
```
以及 Media3 / `msplayer` 工程约定依赖。

3. 混淆：

```
-keep class com.mycjzb.media.player.** { *; }
-keep class com.tvbus.engine.** { *; }
-keepclasseswithmembernames class * { native <methods>; }
```

4. Manifest：`INTERNET`

5. **`android:extractNativeLibs` 必须为 `true`**（否则 `sop://` 不可用）：

```gradle
android {
    packagingOptions {          // AGP 8+ 也可写 packaging { }
        jniLibs { useLegacyPackaging true }
    }
}
```

`libtvcore.so`（TVBus/SOPCast 引擎）由本 SDK 的 AAR 自带，安装时需解压到
`nativeLibraryDir` —— SDK 内部要在它的 `JNI_OnLoad` 之前 patch 掉签名校验，
只能按**文件路径** `dlopen`，不能直接从 APK 内 mmap。

未开启时不会崩，但 `sop://` / `tvbus://` 会报 `MEDIA_ERROR_IO`，日志：

```
W/MsTvb: libtvcore.so missing at /data/app/.../lib/arm/libtvcore.so
         — host must set extractNativeLibs/useLegacyPackaging=true
```

> 宿主 `jniLibs` **不要**再放一份 `libtvcore.so`，否则与 AAR 同名冲突。

## 4. 播放

```java
MediaPlayer mp = new MediaPlayer();
mp.setDataSource("https://example.com/live/index.m3u8");
mp.setSurface(surfaceView.getHolder().getSurface());
mp.setOnPreparedListener(p -> p.start());
mp.setOnErrorListener((p, what, extra) -> true);
mp.prepareAsync();
mp.release();
```

`setDataSource(url, headers)` 可带自定义 HTTP 请求头（SHW / 系统均支持）。

常用：`start` / `pause` / `stop` / `reset` / `seekTo` / `getCurrentPosition` / `getDuration` / `setVolume` / `setLooping` 及标准 Listener。

### 4.1 `sop://` / `tvbus://`（TVBus / SOPCast P2P）

与 rtmp 用法**完全一致**，当成普通地址用即可：

```java
mp.setDataSource("sop://broker.example.com:80/1234");
mp.setSurface(holder.getSurface());
mp.prepareAsync();     // 内部：起 P2P 引擎 → 等本地 http → 交给 Media3
mp.release();          // 内部：停引擎
```

- `tvbus://<base58>` 可直传，SDK 内部先解成 `sop://`。
- 起播在后台线程解析，`prepareAsync()` 不阻塞调用方。
- **换台 / reset / release 自动停引擎**，无需业务侧调用任何 stop。
- 解析失败、引擎不可用、so 缺失 → `MEDIA_ERROR_IO`；日志 tag `MsTvb`。
- 本地回环地址端口从 7892 起找空位。
- 优先 `hls`（`index.m3u8`），等不到才退 mpegts。

## 5. 直播 / 点播

同连接读字节头判定 HLS/DASH/Progressive（不信扩展名、不信 Content-Type）。直播/点播以打开后的 seekable / 时长 / Timeline 为准。

```java
mp.setLiveHint(Boolean.TRUE); // null=自动
if (mp.isVod()) { /* 可 seek */ }
```

SHW 多码率 HLS/DASH 默认选设备能解的最高码率。

## 6. 画面比例

`auto` / `16:9` / `4:3` / `orig` / `fill`。

```java
mp.setAspectMode(AspectMode.R16_9);
MsAspectLayout box = new MsAspectLayout(context);
box.addView(surfaceView);
box.setAspectMode(AspectMode.AUTO);
mp.setOnVideoSizeChangedListener((p, w, h) -> box.setVideoSize(w, h));
```

## 7. 多音轨 / 字幕 / 频谱（SHW）

系统档下列接口为空；完整能力用 SHW。

```java
mp.addTimedTextSource("/sdcard/movie.srt", MediaPlayer.MEDIA_MIMETYPE_TEXT_SUBRIP);
mp.setOnTimedTextListener((p, text) -> {
    subtitleView.setText(text == null ? "" : text.getText());
});
mp.setOnPreparedListener(p -> {
    MediaPlayer.TrackInfo[] tracks = p.getTrackInfo();
    for (int i = 0; i < tracks.length; i++) {
        if (tracks[i].getTrackType() == MediaPlayer.TrackInfo.MEDIA_TRACK_TYPE_AUDIO
                && "chi".equals(tracks[i].getLanguage())) {
            p.selectTrack(i);
        }
        if (tracks[i].getTrackType() == MediaPlayer.TrackInfo.MEDIA_TRACK_TYPE_TIMEDTEXT) {
            p.selectTrack(i);
        }
    }
    p.start();
});
int[] bars = new int[5];
mp.getAudioBars(bars); // 音频透传时为 0
```

- 文字轨：`TIMEDTEXT`；位图（DVB/PGS）：`SUBTITLE`（`TimedText.getBitmap()`，可用 `SubtitleBitmapOverlay`）
- 字幕默认关，需 `selectTrack`
- 外挂 MIME：`MEDIA_MIMETYPE_TEXT_SUBRIP` / `_VTT` / `_SSA`
- 另支持：`setBufferConfig` / `setPlaybackTlsVerify` / `setAudioPassthroughEnabled`
- 视频轨不可选；音轨不可 deselect
- 纯音频 Progressive 可播

## 8. 缓冲 / 透传 / TLS（SHW）

```java
mp.setBufferConfig(15000, 30000, 1500, 3000, 16L * 1024 * 1024);
mp.setAudioPassthroughEnabled(true);
mp.setPlaybackTlsVerify(false); // 默认 false：播放忽略证书；MsHttpClient 仍严格
```

## 9. 支持范围（SHW）

| 类别 | 内容 |
| --- | --- |
| 协议 | file、http、https、HLS（AES-128/MAP/BYTERANGE/直播）、DASH、rtmp、rtmps、rtsp、udp、rtp、**sop/tvbus** |
| 封装 | TS、MP4/MOV/fMP4、FLV、MKV/WebM、AVI、MPEG-PS、裸 AAC/AC-3/E-AC-3/DTS/MP3/FLAC/WAV |
| 视频 | H.264/H.265/MPEG-2/AVS+ 等 → MediaCodec 硬解 |
| 音频 | 软解 AAC/LATM/AC-3/E-AC-3/MP2/MP3/DTS/DRA/G.711/Opus/FLAC/PCM/Vorbis/ALAC；透传 AC-3/E-AC-3/DTS（可关）。无 WMA |
| 字幕 | SRT/WebVTT/ASS/mov_text/PGS/DVB；外挂 SRT/VTT/ASS |
| 其它 | 按内容探测；PNG/JPEG 伪装头自动剥 |

系统档能力取决于设备，上表不保证。

## 10. 已知限制

- 最低 API 18
- TLS 1.2/1.3 + ECDHE(P-256/P-384) + AES-GCM/CBC
- 不支持 HLS SAMPLE-AES / DRM
- 系统档：无选轨；缓冲/透传/TLS/外挂字幕无效或忽略
- JS 二次拉流嵌套 HTTP 默认忽略证书（入口受 `setPlaybackTlsVerify` 控制）
- 无 iOS / Windows

## 11. 正式包签名（必改）

压壳 so（`PACK_SO=1`）启用 soft guard：比对 App 证书 DER 的 SHA-256 与 so 内嵌指纹。不一致则受保护能力失败（不杀进程）。

上柜须钉客户 **release** 证书，勿用仓库 debug 指纹。

DIYP 测包：**不改** App 签名（现 release→debug.keystore）时，so 保持钉 debug；勿改成公司 `ms-tv` 除非 App 也换签。

### 11.1 何时验

| 编法 | 验签 |
| --- | --- |
| 未压壳调试 so | 不验 |
| `PACK_SO=1` / `--pack 1` | 验 |

### 11.2 改指纹

文件：`src/guard/apk_sign.c` → `EXPECT_SIGN_OBF[32]`（证书 SHA-256 每字节 XOR `0x5A`）。`MS_APK_SIGN_XOR` 勿单独改。

```bash
keytool -exportcert -keystore /path/to/release.jks -alias YOUR_ALIAS \
  -storepass '***' -file /tmp/app.cer

python3 - <<'PY'
from pathlib import Path
import hashlib
der = Path('/tmp/app.cer').read_bytes()
dig = hashlib.sha256(der).digest()
obf = bytes(b ^ 0x5A for b in dig)
print('static const unsigned char EXPECT_SIGN_OBF[32] = {')
for i in range(0, 32, 8):
    print('    ' + ', '.join(f'0x{obf[j]:02X}' for j in range(i, i + 8)) + ',')
print('};')
print('plain_sha256=', dig.hex())
PY
```

替换数组后：

```bash
bash scripts/build.sh --pack 1 -va both
```

so 与 APK 须同一 release 证书。

### 11.3 排障

| 现象 | 原因 |
| --- | --- |
| `apk mismatch` / TEA 失败 | 指纹与安装包证书不一致 |
| 仅压壳包失败 | 未压壳不验；重钉后再 pack |
| 多渠道马甲 | 每套签名各钉 so，或统一 release 签名 |

### 11.4 其它

- 勿改 SDK 包名 `com.mycjzb.media.player`（JNI / obf 依赖）
- YSP：URL 带 `?ysp-tea=1` 或 `2`；压壳包须指纹正确

## 12. JS 规则脚本

约定见 [JS 解析规则](https://www.aliiptv.com/js-rules)。

| 项 | 说明 |
| --- | --- |
| 触发 | `http(s)` / 本地文件；正文含 `function main`，或 gzip（`1f8b`），或 **MSJS** |
| 执行 | QuickJS；`main(item)`（`item.url` / `item.pageUrl`） |
| 之后 | 返回可播地址后再做字节头探测；最多 2 层 JS |
| API | 全局 `ms.*`；兼 `ku9`/`cjs`（参数兼容超级直播脚本） |

### 12.1 `main()` 返回

| 形态 | 写法 |
| --- | --- |
| A 地址 | `{ url \| playUrl, referer?, userAgent?/ua?, headers? }` 或纯 URL |
| B 正文 | `{ m3u8 }` 或 `{ body\|content, contentType?/mime? }`，可选 `url` + headers；正文 ≤ 8MiB |

```js
return {
  m3u8: "#EXTM3U\n#EXTINF:4,\nhttps://cdn/a.ts\n",
  url: "https://cdn/index.m3u8",
  referer: "https://site/"
};
```

### 12.2 `stream` + `stream_call`

```js
return {
  url: "https://cdn/index.m3u8",
  stream: true,
  streamArgs: { key: "…", blocks: 18 }
};

function stream_call(url, args) {
  var raw = ms.get(url, { Referer: args.referer || "" });
  return raw; // 字节；单次 ≤ 32MiB
}
```

分片 open 时调用 `stream_call`；失败回落普通 HTTP。样例：`samples/js/gxtv_live.js`。

### 12.3 `ms.*`

| 类 | 函数 |
| --- | --- |
| 网络 | `get` / `http_get` / `download`、`post`、`request`、`getQuery`、`log` |
| 缓存 | `getCache` / `setCache(key, value, ttlMs?)` |
| 摘要 | `md5`、`sha1`、`sha256`、`hmac_sha256` |
| 编解码 | `b64_*`、`hex_*`、`urlencode` / `urldecode`、`random_bytes` |
| 时间 | `time`、`now_ms` |
| 对称 | `aes_*`、`xxtea_*`、`cipher(...)` |
| 非对称 | `rsa_public_encrypt`、`rsa_sign`、`rsa_verify` |

蛇形与驼峰双名。样例目录：`samples/js/`。

### 12.4 明文 / gzip / MSJS

| 形态 | 识别 |
| --- | --- |
| 明文 | 含 `function main` |
| gzip | 头 `1f 8b` |
| MSJS | 头 `MSJS`，ver=1，AES-256-GCM |

明文与下发文件均 ≤ 512KiB。

```bash
python3 scripts/pack_msjs.py -i samples/js/yspweb.js -o build/yspweb.msjs
python3 scripts/pack_msjs.py --gzip-only -i samples/js/yspweb.js -o build/yspweb.js.gz
```

### 12.5 编 so 前

```bash
bash scripts/fetch_quickjs.sh
```

## 13. License

MsMedia：LGPL-2.1。FFmpeg 9.1：LGPL-2.1-or-later。OpenSSL 3.5.9：Apache-2.0。QuickJS：MIT。见 [THIRD_PARTY.md](THIRD_PARTY.md)。
