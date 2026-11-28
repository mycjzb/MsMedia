# libmsplayer.so 的 JNI 入口按类名/方法名绑定，混淆后会找不到。业务 App 开 R8/ProGuard 时加上这几行。
# by: Long <mycjzb@gmail.com>
# 权威副本：android/msplayer/consumer-rules.pro（SDK 模块已合并 Media3 反射 keep）

-keepclasseswithmembernames class com.mycjzb.media.player.** {
    native <methods>;
}

-keep class com.mycjzb.media.player.MsHttpClient {
    static *** dnsResolveFromNative(...);
}

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

# 原生层 FindClass 调用的 VSYNC 采样器（不得混淆）
-keep class com.mycjzb.media.player.media3.VSyncSampler { *; }
-keep class com.mycjzb.media.player.media3.VSyncSamplerV33 { *; }
