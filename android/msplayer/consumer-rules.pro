# :msplayer release 自裁 + 客户 App R8 共用。HLS/DASH/Ffmpeg 反射入口必须留。
# JNI：方法名绑定，禁止混淆 native
-keepclasseswithmembernames class com.mycjzb.media.player.** {
    native <methods>;
}

# native → Java 回调
-keep class com.mycjzb.media.player.MsHttpClient {
    static *** dnsResolveFromNative(...);
}

# 对外 SDK API
-keep public class com.mycjzb.media.player.MediaPlayer { public *; }
-keep public class com.mycjzb.media.player.MediaPlayer$* { public *; }
-keep public class com.mycjzb.media.player.TimedText { public *; }
-keep public class com.mycjzb.media.player.MsHttpClient { public *; }
-keep public class com.mycjzb.media.player.MsHttpClient$* { public *; }
-keep public class com.mycjzb.media.player.PlayEngine { public *; }
-keep public class com.mycjzb.media.player.AspectMode { public *; }
-keep public class com.mycjzb.media.player.MsAspectLayout { public *; }
-keep public class com.mycjzb.media.player.n.a { public *; }
-keep public class com.mycjzb.media.player.n.b { public *; }
# TVBus 加载入口：宿主用 Class.forName("com.mycjzb.media.player.MsTvbus") 反射调用，
# 方法非 native 且 AAR 内部无调用者，不给 keep 会被 R8 整个裁掉 → 反射失败。
-keep class com.mycjzb.media.player.MsTvbus { *; }
# libtvcore 的 JNI 由 ms_tvb.c 按全名 RegisterNatives，类名/方法名禁止混淆。
-keep class com.tvbus.engine.TVCore { *; }
-keep class com.tvbus.engine.TVListener { *; }
-keep class com.mycjzb.media.player.tvbus.** { *; }
-dontwarn com.tvbus.engine.**

# DefaultRenderersFactory 反射加载 FFmpeg 音频扩展
-keep class androidx.media3.decoder.ffmpeg.FfmpegAudioRenderer { *; }
-keepclassmembers class androidx.media3.decoder.ffmpeg.FfmpegAudioRenderer {
  <init>(android.os.Handler, androidx.media3.exoplayer.audio.AudioRendererEventListener, androidx.media3.exoplayer.audio.AudioSink);
}

# DefaultMediaSourceFactory 反射（HLS/DASH 能力保留，禁止裁模块）
-keepclasseswithmembers class androidx.media3.exoplayer.dash.DashMediaSource$Factory {
  <init>(androidx.media3.datasource.DataSource$Factory);
}
-keepclasseswithmembers class androidx.media3.exoplayer.hls.HlsMediaSource$Factory {
  <init>(androidx.media3.datasource.DataSource$Factory);
}
-keepclasseswithmembers class androidx.media3.exoplayer.hls.HlsMediaSource$Factory {
  <init>(androidx.media3.exoplayer.hls.HlsDataSourceFactory);
}

# 共享 media3（bs-p2p/third_party/media3-src）按类名反射加载的 ApiXX 兼容类；与超级直播 proguard 同一组
-keep class androidx.media3.common.AudioAttributes { *; }
-keep class androidx.media3.common.AudioAttributesApi* { *; }
-keep class androidx.media3.common.util.UtilApi* { *; }
-keep class androidx.media3.common.util.NetworkTypeObserverApi* { *; }
-keep class androidx.media3.exoplayer.audio.DefaultAudioSinkApi* { *; }
-keep class androidx.media3.exoplayer.audio.DefaultAudioOffloadSupportProviderApi* { *; }
-keep class androidx.media3.exoplayer.audio.AudioCapabilitiesApi* { *; }
-keep class androidx.media3.exoplayer.audio.AudioCapabilitiesReceiverApi* { *; }
-keep class androidx.media3.exoplayer.AudioFocusManagerApi* { *; }
-keep class androidx.media3.exoplayer.ExoPlayerImplAudioDeviceApi* { *; }
-keep class androidx.media3.exoplayer.mediacodec.MediaCodecRendererApi* { *; }
-keep class androidx.media3.exoplayer.mediacodec.SynchronousMediaCodecAdapterApi* { *; }
-keep class androidx.media3.exoplayer.video.MediaCodecVideoRendererApi* { *; }
-keep class androidx.media3.exoplayer.trackselection.SpatializerWrapperV32 { *; }

# media3 common / Guava（原 libraries/common/proguard-rules.txt）
-dontwarn org.checkerframework.**
-dontwarn kotlin.annotations.jvm.**
-dontwarn javax.annotation.**
-dontwarn java.lang.ClassValue
-dontwarn java.lang.SafeVarargs
-dontwarn javax.lang.model.element.Modifier
-dontwarn sun.misc.Unsafe
-dontwarn com.google.errorprone.annotations.**
-dontwarn com.google.j2objc.annotations.**
-dontwarn org.codehaus.mojo.animal_sniffer.IgnoreJRERequirement
-keepclassmembernames class com.google.common.base.Function { *; }
