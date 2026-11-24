# libmsplayer.so 的 JNI 入口按类名/方法名绑定，混淆后会找不到。业务 App 开 R8/ProGuard 时加上这几行。
# by: Long <mycjzb@gmail.com>

# 所有 native 方法（Java_com_mycjzb_media_player_* 按名字匹配）
-keepclasseswithmembernames class com.mycjzb.media.player.** {
    native <methods>;
}

# 原生层 FindClass / GetMethodID 反向调用的类与方法
-keep class com.mycjzb.media.player.MsSystemTls { *; }
-keep class com.mycjzb.media.player.MsHttpClient {
    static *** dnsResolveFromNative(...);
}

# 公开 API（对外 SDK，保留类名和公开成员，便于业务直接调用）
-keep public class com.mycjzb.media.player.MediaPlayer { public *; }
-keep public class com.mycjzb.media.player.MediaPlayer$* { public *; }
-keep public class com.mycjzb.media.player.TimedText { public *; }
-keep public class com.mycjzb.media.player.MsHttpClient { public *; }
-keep public class com.mycjzb.media.player.MsHttpClient$* { public *; }
