/*
 * 单 libmsplayer.so 调试 Demo：多地址切台测起播速度，系统播放器 / MsMedia 播放器一键切换。
 * 遥控器：上/下（含频道+/-）切换上一个/下一个地址并立即重新起播；左/右切换播放器。
 * 屏幕按钮同样可用（手机触屏）。左上角浮层显示播放器、频道序号与起播分段耗时：
 *   create→setDataSource、→OnPrepared、→首帧（OnInfo RENDERING_START），单位毫秒。
 * 自动化（adb am start 传 extra，见 scripts/android_single_apk.sh 说明）：
 *   auto=bench 逐地址×播放器测速并走完整生命周期；http=... 跑一次 MsHttpClient 请求。
 *   auto=tracks 选轨/字幕接口自测（urls 取第一个，sub=外挂 SRT 路径，repeat=轮数），见 MsTrackSelfTest。
 * 日志/浮层不打印完整 URL（只显示 scheme://host/…/文件名），避免泄露私有地址。
 * by: Long <mycjzb@gmail.com>
 */
package com.mycjzb.media.player.demo;

import android.app.Activity;
import android.content.Intent;
import android.graphics.Color;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.util.Log;
import android.view.Gravity;
import android.view.KeyEvent;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.TextView;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

public class MsSpeedDemoActivity extends Activity implements SurfaceHolder.Callback {

    /* 默认列表：优先 assets/channels.txt（DIY 名,url / #genre#）；没有再用下面兜底。
     * 遥控器上下切台；adb --es urls 'a|b' 仍可临时覆盖。 */
    static final String[] TEST_URLS = {
            "h264_aac.ts",
            "h264_aac.mp4",
            "h264_aac.flv",
            "http://10.0.2.2:8080/h264_aac.ts",
            "http://10.0.2.2:8080/h264_aac.mp4",
    };

    private static final String TAG = "MsSpeed";
    private static final int FIRST_FRAME_TIMEOUT_MS = 15000;

    /* ---------------- 播放器适配：系统 / MsMedia 同一套调用 ---------------- */
    interface Listener {
        void onPrepared();
        void onFirstFrame();
        void onError(int what, int extra);
        void onCompletion();
        void onSeekComplete();
    }

    interface Player {
        String kind();
        void setDisplay(SurfaceHolder h);
        void setDataSource(String loc) throws Exception;
        void prepareAsync();
        void start();
        void pause();
        void seekTo(int ms);
        int getDuration();
        int getCurrentPosition();
        boolean isPlaying();
        void release();
        long[] nativeTimingsUs();
    }

    static final class SysPlayer implements Player {
        final android.media.MediaPlayer mp = new android.media.MediaPlayer();

        SysPlayer(final Listener l) {
            mp.setOnPreparedListener(new android.media.MediaPlayer.OnPreparedListener() {
                public void onPrepared(android.media.MediaPlayer m) { l.onPrepared(); }
            });
            mp.setOnInfoListener(new android.media.MediaPlayer.OnInfoListener() {
                public boolean onInfo(android.media.MediaPlayer m, int what, int extra) {
                    if (what == android.media.MediaPlayer.MEDIA_INFO_VIDEO_RENDERING_START) l.onFirstFrame();
                    return false;
                }
            });
            mp.setOnErrorListener(new android.media.MediaPlayer.OnErrorListener() {
                public boolean onError(android.media.MediaPlayer m, int what, int extra) {
                    l.onError(what, extra);
                    return true;
                }
            });
            mp.setOnCompletionListener(new android.media.MediaPlayer.OnCompletionListener() {
                public void onCompletion(android.media.MediaPlayer m) { l.onCompletion(); }
            });
            mp.setOnSeekCompleteListener(new android.media.MediaPlayer.OnSeekCompleteListener() {
                public void onSeekComplete(android.media.MediaPlayer m) { l.onSeekComplete(); }
            });
        }
        public String kind() { return "System"; }
        public void setDisplay(SurfaceHolder h) { mp.setDisplay(h); }
        public void setDataSource(String loc) throws Exception { mp.setDataSource(loc); }
        public void prepareAsync() { mp.prepareAsync(); }
        public void start() { mp.start(); }
        public void pause() { mp.pause(); }
        public void seekTo(int ms) { mp.seekTo(ms); }
        public int getDuration() { return mp.getDuration(); }
        public int getCurrentPosition() { return mp.getCurrentPosition(); }
        public boolean isPlaying() { return mp.isPlaying(); }
        public void release() { mp.release(); }
        public long[] nativeTimingsUs() { return null; }
    }

    static final class MsPlayer implements Player {
        final com.mycjzb.media.player.MediaPlayer mp = new com.mycjzb.media.player.MediaPlayer();

        MsPlayer(final Listener l) {
            /* 阶段 4 新 API：预加载（10 s / 16 MiB）与音频直通开关（intent --ei pt 1）。 */
            mp.setPreloadConfig(10000, 16L << 20);
            mp.setAudioPassthroughEnabled(sPassthrough);
            mp.setOnPreparedListener(new com.mycjzb.media.player.MediaPlayer.OnPreparedListener() {
                public void onPrepared(com.mycjzb.media.player.MediaPlayer m) {
                    l.onPrepared();
                    Log.i(TAG, "MSPT enabled=" + m.isAudioPassthroughEnabled() + " route=" + m.getAudioPassthroughRoute());
                }
            });
            mp.setOnInfoListener(new com.mycjzb.media.player.MediaPlayer.OnInfoListener() {
                public boolean onInfo(com.mycjzb.media.player.MediaPlayer m, int what, int extra) {
                    if (what == com.mycjzb.media.player.MediaPlayer.MEDIA_INFO_VIDEO_RENDERING_START) l.onFirstFrame();
                    return false;
                }
            });
            mp.setOnErrorListener(new com.mycjzb.media.player.MediaPlayer.OnErrorListener() {
                public boolean onError(com.mycjzb.media.player.MediaPlayer m, int what, int extra) {
                    l.onError(what, extra);
                    return true;
                }
            });
            mp.setOnCompletionListener(new com.mycjzb.media.player.MediaPlayer.OnCompletionListener() {
                public void onCompletion(com.mycjzb.media.player.MediaPlayer m) { l.onCompletion(); }
            });
            mp.setOnSeekCompleteListener(new com.mycjzb.media.player.MediaPlayer.OnSeekCompleteListener() {
                public void onSeekComplete(com.mycjzb.media.player.MediaPlayer m) { l.onSeekComplete(); }
            });
        }
        public String kind() { return com.mycjzb.media.player.MediaPlayer.isFfmpegRoute() ? "MsMedia(B)" : "MsMedia(A)"; }
        public void setDisplay(SurfaceHolder h) { mp.setDisplay(h); }
        public void setDataSource(String loc) throws Exception { mp.setDataSource(loc); }
        public void prepareAsync() { mp.prepareAsync(); }
        public void start() { mp.start(); }
        public void pause() { mp.pause(); }
        public void seekTo(int ms) { mp.seekTo(ms); }
        public int getDuration() { return mp.getDuration(); }
        public int getCurrentPosition() { return mp.getCurrentPosition(); }
        public boolean isPlaying() { return mp.isPlaying(); }
        public void release() { mp.release(); }
        public long[] nativeTimingsUs() { return mp.getStartupTimingsUs(); }
    }

    /* ---------------- 界面与状态 ---------------- */
    private final Handler main = new Handler(Looper.getMainLooper());
    private SurfaceView surface;
    private SurfaceHolder holder;
    private TextView overlay;
    private String[] urls = TEST_URLS;
    private String[] names = new String[0];
    static volatile boolean sPassthrough;
    private int index = 0;
    private boolean useMs = true;
    private Player player;
    private int session; /* 每次起播自增，丢弃旧播放器的迟到回调 */
    private long tCreate, tSetDs, tPrepared, tFirst;
    private String lastErr = "";
    private final List<String> history = new ArrayList<String>();

    /* 自动化 */
    private boolean auto;
    private String[] autoPlayers = {"sys", "ms"};
    private int autoRepeat = 1;
    private boolean autoLifecycle = true;
    private int autoStep;
    private final List<int[]> autoPlan = new ArrayList<int[]>(); /* {urlIndex, isMs} */
    private int lifecyclePhase;
    private boolean trackTest;  /* auto=tracks */
    private String trackSub;

    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);
        FrameLayout root = new FrameLayout(this);
        root.setBackgroundColor(Color.BLACK);
        surface = new SurfaceView(this);
        root.addView(surface, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.MATCH_PARENT));
        overlay = new TextView(this);
        overlay.setTextColor(Color.WHITE);
        overlay.setBackgroundColor(0x99000000);
        overlay.setTextSize(15);
        overlay.setPadding(12, 8, 12, 8);
        FrameLayout.LayoutParams olp = new FrameLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT,
                ViewGroup.LayoutParams.WRAP_CONTENT, Gravity.TOP | Gravity.START);
        root.addView(overlay, olp);
        LinearLayout bar = new LinearLayout(this);
        bar.setOrientation(LinearLayout.HORIZONTAL);
        bar.addView(button("上一个", new View.OnClickListener() { public void onClick(View v) { step(-1); } }));
        bar.addView(button("下一个", new View.OnClickListener() { public void onClick(View v) { step(1); } }));
        bar.addView(button("切换播放器", new View.OnClickListener() { public void onClick(View v) { togglePlayer(); } }));
        root.addView(bar, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT,
                ViewGroup.LayoutParams.WRAP_CONTENT, Gravity.BOTTOM | Gravity.CENTER_HORIZONTAL));
        setContentView(root);
        surface.getHolder().addCallback(this);
        loadChannelsAsset();
        parseIntent(getIntent());
        refreshOverlay();
    }

    /** 读 assets/channels.txt：跳过空行与 #genre#；行格式 台名,url */
    private void loadChannelsAsset() {
        InputStream in = null;
        try {
            in = getAssets().open("channels.txt");
            BufferedReader br = new BufferedReader(new InputStreamReader(in, "UTF-8"));
            ArrayList<String> u = new ArrayList<String>();
            ArrayList<String> n = new ArrayList<String>();
            String line;
            while ((line = br.readLine()) != null) {
                line = line.trim();
                if (line.length() == 0 || line.contains("#genre#")) continue;
                int c = line.indexOf(',');
                if (c <= 0 || c + 1 >= line.length()) continue;
                String name = line.substring(0, c).trim();
                String url = line.substring(c + 1).trim();
                if (url.length() == 0) continue;
                n.add(name);
                u.add(url);
            }
            br.close();
            if (u.size() > 0) {
                urls = u.toArray(new String[u.size()]);
                names = n.toArray(new String[n.size()]);
                Log.i(TAG, "channels asset loaded n=" + urls.length);
            }
        } catch (Throwable t) {
            Log.w(TAG, "channels asset skip: " + t.getMessage());
        } finally {
            if (in != null) try { in.close(); } catch (Throwable ignored) {}
        }
    }

    private Button button(String text, View.OnClickListener l) {
        Button bt = new Button(this);
        bt.setText(text);
        bt.setOnClickListener(l);
        bt.setFocusable(false); /* 方向键留给切台/切播放器 */
        return bt;
    }

    private void parseIntent(Intent it) {
        if (it == null) return;
        String u = it.getStringExtra("urls");
        if (u != null && u.length() > 0) {
            urls = u.split("\\|");
            names = new String[0];
        }
        String p = it.getStringExtra("player");
        if (p != null) useMs = !"sys".equals(p);
        String a = it.getStringExtra("auto");
        auto = "bench".equals(a);
        trackTest = "tracks".equals(a);
        trackSub = it.getStringExtra("sub");
        String ps = it.getStringExtra("players");
        if (ps != null && ps.length() > 0) autoPlayers = ps.split(",");
        autoRepeat = Math.max(1, it.getIntExtra("repeat", 1));
        autoLifecycle = it.getIntExtra("lifecycle", 1) != 0;
        sPassthrough = it.getIntExtra("pt", 0) != 0;
        try {
            /* 探测在所有 API 上都必须安全：API 18 返回 0，不得 NoSuchMethodError/VerifyError。 */
            int caps = com.mycjzb.media.player.MediaPlayer.getAudioPassthroughCaps(this);
            Log.i(TAG, "MSAPI sdk=" + android.os.Build.VERSION.SDK_INT + " pt_caps=0x" + Integer.toHexString(caps)
                    + " pt=" + sPassthrough);
        } catch (Throwable t) {
            Log.e(TAG, "MSAPI FAIL " + t);
        }
        if (auto) {
            autoPlan.clear();
            for (int r = 0; r < autoRepeat; r++)
                for (int i = 0; i < urls.length; i++)
                    for (String pl : autoPlayers)
                        autoPlan.add(new int[]{i, "sys".equals(pl) ? 0 : 1});
            autoStep = 0;
        }
        final String http = it.getStringExtra("http");
        if (http != null && http.length() > 0) {
            new Thread(new Runnable() { public void run() { runHttpTests(http.split("\\|")); } }, "ms-http-test").start();
        }
    }

    /* ---------------- 遥控器 ---------------- */
    @Override
    public boolean dispatchKeyEvent(KeyEvent e) {
        if (e.getAction() == KeyEvent.ACTION_DOWN) {
            switch (e.getKeyCode()) {
                case KeyEvent.KEYCODE_DPAD_UP:
                case KeyEvent.KEYCODE_CHANNEL_UP:
                    step(-1);
                    return true;
                case KeyEvent.KEYCODE_DPAD_DOWN:
                case KeyEvent.KEYCODE_CHANNEL_DOWN:
                    step(1);
                    return true;
                case KeyEvent.KEYCODE_DPAD_LEFT:
                case KeyEvent.KEYCODE_DPAD_RIGHT:
                    togglePlayer();
                    return true;
                default:
                    break;
            }
        }
        return super.dispatchKeyEvent(e);
    }

    private void step(int d) {
        if (urls.length == 0) return;
        index = ((index + d) % urls.length + urls.length) % urls.length;
        startPlayback();
    }

    private void togglePlayer() {
        useMs = !useMs;
        startPlayback();
    }

    /* ---------------- 起播与测速 ---------------- */
    private String resolve(String u) {
        if (u.contains("://") || u.startsWith("/")) return u;
        File base = getExternalFilesDir(null);
        return new File(base, u).getAbsolutePath();
    }

    /** 浮层/日志只显示 scheme://host/…/文件名，不暴露完整私有地址与参数。 */
    static String redact(String u) {
        int q = u.indexOf('?');
        if (q >= 0) u = u.substring(0, q);
        int s = u.indexOf("://");
        String name = u.substring(u.lastIndexOf('/') + 1);
        if (s < 0) return name;
        int h = u.indexOf('/', s + 3);
        String host = h < 0 ? u.substring(s + 3) : u.substring(s + 3, h);
        int at = host.lastIndexOf('@');
        if (at >= 0) host = host.substring(at + 1);
        return u.substring(0, s + 3) + host + "/…/" + name;
    }

    private void releasePlayer() {
        if (player != null) {
            try {
                player.release();
            } catch (Throwable t) {
                Log.w(TAG, "release: " + t);
            }
            player = null;
        }
    }

    private void startPlayback() {
        releasePlayer();
        final int my = ++session;
        tSetDs = tPrepared = tFirst = 0;
        lastErr = "";
        lifecyclePhase = 0;
        if (holder == null) {
            refreshOverlay();
            return;
        }
        Listener l = new Listener() {
            public void onPrepared() {
                if (my != session) return;
                tPrepared = SystemClock.elapsedRealtime();
                player.start();
                refreshOverlay();
                main.postDelayed(new Runnable() { public void run() { firstFrameTimeout(my); } }, FIRST_FRAME_TIMEOUT_MS);
            }
            public void onFirstFrame() {
                if (my != session || tFirst != 0) return;
                tFirst = SystemClock.elapsedRealtime();
                refreshOverlay();
                record("OK");
                if (auto) main.postDelayed(new Runnable() { public void run() { lifecycle(my); } }, 300);
            }
            public void onError(int what, int extra) {
                if (my != session) return;
                lastErr = "error what=" + what + " extra=" + extra;
                refreshOverlay();
                record(lastErr);
                if (auto) autoNext();
            }
            public void onCompletion() {
                if (my != session) return;
                Log.i(TAG, "completion");
            }
            public void onSeekComplete() {
                if (my != session) return;
                Log.i(TAG, "seekComplete pos=" + safePos());
                if (auto && lifecyclePhase == 1) {
                    main.postDelayed(new Runnable() { public void run() { lifecycle(my); } }, 500);
                }
            }
        };
        tCreate = SystemClock.elapsedRealtime();
        try {
            player = useMs ? new MsPlayer(l) : new SysPlayer(l);
            player.setDisplay(holder);
            player.setDataSource(resolve(urls[index]));
            tSetDs = SystemClock.elapsedRealtime();
            player.prepareAsync();
        } catch (Throwable t) {
            lastErr = "setDataSource/prepare: " + t.getClass().getSimpleName();
            record(lastErr);
            if (auto) autoNext();
        }
        refreshOverlay();
    }

    private int safePos() {
        try {
            return player != null ? player.getCurrentPosition() : -1;
        } catch (Throwable t) {
            return -1;
        }
    }

    private void firstFrameTimeout(int my) {
        if (my != session || tFirst != 0) return;
        lastErr = "first frame timeout " + FIRST_FRAME_TIMEOUT_MS + "ms";
        refreshOverlay();
        record(lastErr);
        if (auto) autoNext();
    }

    /* 完整生命周期：seekTo(中点) → pause → start → release（自动化时） */
    private void lifecycle(final int my) {
        if (my != session || player == null) return;
        try {
            if (!autoLifecycle) {
                autoNext();
                return;
            }
            if (lifecyclePhase == 0) {
                lifecyclePhase = 1;
                int d = player.getDuration();
                Log.i(TAG, "LIFE seekTo " + (d > 0 ? d / 2 : 0) + " dur=" + d);
                player.seekTo(d > 0 ? d / 2 : 0);
                main.postDelayed(new Runnable() { public void run() {
                    if (my == session && lifecyclePhase == 1) lifecycle(my); } }, 4000);
            } else if (lifecyclePhase == 1) {
                lifecyclePhase = 2;
                player.pause();
                Log.i(TAG, "LIFE pause playing=" + player.isPlaying() + " pos=" + safePos());
                main.postDelayed(new Runnable() { public void run() { lifecycle(my); } }, 500);
            } else if (lifecyclePhase == 2) {
                lifecyclePhase = 3;
                player.start();
                Log.i(TAG, "LIFE start playing=" + player.isPlaying());
                main.postDelayed(new Runnable() { public void run() { lifecycle(my); } }, 800);
            } else {
                Log.i(TAG, "LIFE release pos=" + safePos());
                Log.i(TAG, "MSLIFE OK player=" + player.kind() + " url=" + redact(urls[index]));
                autoNext();
            }
        } catch (Throwable t) {
            Log.e(TAG, "MSLIFE FAIL " + t);
            autoNext();
        }
    }

    private void autoNext() {
        if (!auto) return;
        final int my = session;
        main.postDelayed(new Runnable() {
            public void run() {
                if (my != session) return;
                if (autoStep >= autoPlan.size()) {
                    releasePlayer();
                    Log.i(TAG, "MSBENCH_DONE n=" + autoPlan.size());
                    auto = false;
                    return;
                }
                int[] s = autoPlan.get(autoStep++);
                index = s[0];
                useMs = s[1] != 0;
                startPlayback();
            }
        }, 400);
    }

    private void record(String status) {
        String line = String.format(java.util.Locale.US,
                "MSBENCH player=%s idx=%d/%d url=%s status=%s create_to_setds_ms=%d to_prepared_ms=%d to_first_frame_ms=%d native_us=%s",
                player != null ? player.kind() : (useMs ? "MsMedia" : "System"), index + 1, urls.length,
                redact(urls[index]), status.replace(' ', '_'),
                tSetDs > 0 ? tSetDs - tCreate : -1, tPrepared > 0 ? tPrepared - tCreate : -1,
                tFirst > 0 ? tFirst - tCreate : -1,
                player != null && player.nativeTimingsUs() != null ? Arrays.toString(player.nativeTimingsUs()).replace(" ", "") : "-");
        Log.i(TAG, line);
        history.add(line);
        try {
            File f = new File(getExternalFilesDir(null), "bench.txt");
            FileOutputStream o = new FileOutputStream(f, true);
            o.write((line + "\n").getBytes("UTF-8"));
            o.close();
        } catch (Throwable ignored) {
        }
    }

    private void refreshOverlay() {
        StringBuilder sb = new StringBuilder();
        sb.append("播放器: ").append(useMs ? (com.mycjzb.media.player.MediaPlayer.isFfmpegRoute() ? "MsMedia(路线B)" : "MsMedia(路线A)") : "System")
          .append("    频道: ").append(index + 1).append('/').append(urls.length).append('\n');
        if (names.length > index) sb.append(names[index]).append('\n');
        sb.append(redact(urls.length > 0 ? urls[index] : "-")).append('\n');
        sb.append("create→setDataSource: ").append(tSetDs > 0 ? (tSetDs - tCreate) + " ms" : "-").append('\n');
        sb.append("→OnPrepared: ").append(tPrepared > 0 ? (tPrepared - tCreate) + " ms" : "-").append('\n');
        sb.append("→首帧: ").append(tFirst > 0 ? (tFirst - tCreate) + " ms" : "-");
        if (lastErr.length() > 0) sb.append('\n').append(lastErr);
        sb.append("\n上/下:换台  左/右:切播放器");
        overlay.setText(sb.toString());
    }

    /* ---------------- 公共网络类实测 ---------------- */
    private void runHttpTests(String[] list) {
        com.mycjzb.media.player.MsNativeLoader.ensure();
        com.mycjzb.media.player.MsHttpClient c = new com.mycjzb.media.player.MsHttpClient.Builder()
                .connectTimeout(10000).readTimeout(15000).build();
        for (String u : list) {
            long t0 = SystemClock.elapsedRealtime();
            String res;
            try {
                com.mycjzb.media.player.MsHttpClient.Response r = c.newCall(
                        new com.mycjzb.media.player.MsHttpClient.RequestBuilder().url(u).get().build()).execute();
                long n = 0;
                InputStream in = r.body();
                byte[] buf = new byte[16384];
                int k;
                while ((k = in.read(buf)) > 0) n += k;
                r.close();
                res = "code=" + r.code() + " bytes=" + n;
            } catch (Throwable t) {
                res = "error=" + t.getClass().getSimpleName() + ":" + String.valueOf(t.getMessage()).replace(' ', '_');
            }
            Log.i(TAG, "MSHTTP url=" + redact(u) + " " + res + " ms=" + (SystemClock.elapsedRealtime() - t0));
        }
        Log.i(TAG, "MSHTTP_DONE");
    }

    /* ---------------- Surface ---------------- */
    @Override
    public void surfaceCreated(SurfaceHolder h) {
        holder = h;
        if (trackTest) {
            trackTest = false; /* 只跑一次 */
            new MsTrackSelfTest(h, resolve(urls[0]), trackSub == null ? null : resolve(trackSub), autoRepeat).start();
            return;
        }
        if (auto) {
            autoNext();
        } else {
            startPlayback();
        }
    }

    @Override
    public void surfaceChanged(SurfaceHolder h, int f, int w, int hh) {
    }

    @Override
    public void surfaceDestroyed(SurfaceHolder h) {
        session++;
        releasePlayer();
        holder = null;
    }

    @Override
    protected void onDestroy() {
        session++;
        releasePlayer();
        super.onDestroy();
    }
}
