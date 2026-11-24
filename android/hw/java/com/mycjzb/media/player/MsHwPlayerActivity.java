/*
 * 可运行播放 Activity：Surface + 编排 + 异步 URL（ms_ff）。
 * 交 Surface / releaseOutputBuffer / OnFrameRendered / 单次 Choreographer
 * 都不是实际显示。不以本界面声称设备硬解或首帧出画已验收。
 * 不把私密 URL 写入日志。
 * by: Long <mycjzb@gmail.com>
 */
package com.mycjzb.media.player;

import android.app.Activity;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.media.AudioFocusRequest;
import android.media.AudioManager;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.Base64;
import android.util.Log;
import android.graphics.Bitmap;
import android.graphics.Typeface;
import android.widget.ImageView;
import android.text.SpannableString;
import android.text.Spanned;
import android.text.style.ForegroundColorSpan;
import android.text.style.StyleSpan;
import android.content.SharedPreferences;
import android.graphics.Canvas;
import android.view.GestureDetector;
import android.view.MotionEvent;
import android.view.Display;
import android.view.Gravity;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.widget.Button;
import android.widget.EditText;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.File;
import java.io.FileOutputStream;
import java.security.KeyStore;
import java.security.cert.X509Certificate;
import java.util.Enumeration;

public class MsHwPlayerActivity extends Activity implements SurfaceHolder.Callback, View.OnClickListener, Runnable {
    private static boolean sAssSoLoaded;
    private static final String TAG = "MsHwPlayer";
    private TextView text;
    private TextView cueView;
    private TextView[] cueViews;
    private ImageView[] bmpViews;
    private ImageView assView;
    /** sub_off 后立即隐藏叠加；选中字幕轨后清掉。 */
    private volatile boolean subsHidden;
    private FrameLayout stage;
    private View gestureLayer;
    private EditText subBox;
    private EditText urlBox;
    private long handle;
    private final Handler ui = new Handler(Looper.getMainLooper());
    private boolean polling;
    private boolean ffLoaded;
    private boolean caExporting;
    String pendingUrlAfterCa;
    /* 只从 Intent 自动开播一次，Surface 重建不得重开 URL/路径。 */
    private boolean intentAutoStarted;
    /* 仅 Pause/Resume 按钮，前后台焦点恢复不得清掉。 */
    private boolean userPaused;
    private boolean hasAudioFocus;
    private AudioManager audioMgr;
    private AudioManager.OnAudioFocusChangeListener focusListener;
    private AudioFocusRequest focusRequest;
    static final String DEBUG_CMD_ACTION = "com.mycjzb.media.player.DEBUG_CMD";
    private BroadcastReceiver debugReceiver;
    private GestureDetector gestures;
    /* 0=铺满 stage，1=16:9 信箱，2=居中裁切（放大）。 */
    private int aspectMode;
    private String lastMediaKey;
    private static final String PREF_RESUME = "ms_hw_resume";

    static {
        try {
            System.loadLibrary("msmedia");
        } catch (UnsatisfiedLinkError e) {
            Log.w(TAG, "libmsmedia optional load failed");
        }
        try {
            System.loadLibrary("msff");
        } catch (UnsatisfiedLinkError e) {
            Log.w(TAG, "libmsff not loaded");
        }
        try {
            System.loadLibrary("msass");
            sAssSoLoaded = true;
        } catch (UnsatisfiedLinkError e) {
            sAssSoLoaded = false;
            Log.w(TAG, "libmsass optional complete-ASS renderer not packaged");
        }
        try {
            System.loadLibrary("msai");
        } catch (UnsatisfiedLinkError e) {
            Log.w(TAG, "libmsai optional AI-subtitle module not packaged");
        }
        System.loadLibrary("mshw");
    }

    private native long nativeCreate();
    private native String nativeSetForceDecoder(long h, String name);
    private native String nativeSetSurface(long h, Surface surface);
    private native String nativeInjectDemo(long h);
    private native String nativePlayPath(long h, String path);
    private native String nativeOpenUrl(long h, String url, String caFile, int timeoutMs);
    private native boolean nativeHasFf();
    private native String nativeDrive(long h);
    private native String nativeStats(long h);
    private native void nativeFlush(long h);
    private native void nativeCancel(long h);
    private native void nativeDestroy(long h);
    private native String nativeListTracks(long h);
    private native String nativeSelectTrack(long h, int id, int on);
    private native String nativeClearSubtitles(long h);
    private native String nativeCurrentCue(long h);
    private native int nativeFillCues(long h, int[] meta, String[] texts, int[] spans);
    private native int nativeFillBitmap(long h, int index, int[] geom, byte[] rgba);
    private native String nativeLoadSubtitle(long h, byte[] data, int fmt);
    private native String nativePause(long h);
    private native String nativeResume(long h);
    private native String nativeNotifyAudioFocus(long h, int ev);
    private native String nativeSetRate(long h, int num, int den);
    private native String nativeRateCaps(long h);
    private native String nativeSeek(long h, long time90k);
    private native String nativeSeekRelative(long h, long delta90k);
    private native String nativeSeekLiveEdge(long h);
    private native String nativePosition(long h);
    private native int nativeAssAvailable(long h);
    private native String nativeAssSetCanvas(long h, int canvasW, int canvasH);
    private native int nativeFillAssFrame(long h, int[] geom, byte[] rgba);
    private native String nativeAiStatus(long h);
    private native String nativeAiConfigure(long h, int vendor, String srcLang, String tgtLang);
    private native String nativeAiSetEnabled(long h, int on);

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        handle = nativeCreate();
        try {
            String force = getIntent() != null
                    ? getIntent().getStringExtra("force_decoder") : null;
            if (force != null && force.length() > 0) {
                /* 测试覆盖：不改变默认硬解优先；未传 extra 则行为不变。 */
                nativeSetForceDecoder(handle, force);
            }
        } catch (Exception e) {
            /* 忽略测试 extra 读取失败，保持默认路径 */
        }
        audioMgr = (AudioManager) getSystemService(Context.AUDIO_SERVICE);
        focusListener = new MsHwAudioFocusListener(this);
        try {
            ffLoaded = nativeHasFf();
        } catch (UnsatisfiedLinkError e) {
            ffLoaded = false;
        }
        text = new TextView(this);
        text.setTextSize(12f);
        text.setPadding(16, 16, 16, 16);
        text.setText("waiting surface. OUTPUT_RENDERED / OnFrameRendered / Choreographer "
                + "are NOT on-screen paint. Test APK allows system AAC (系统 AAC，非硬件); "
                + "video remains hardware-only. Not a device-sound claim. ff=" + ffLoaded);
        urlBox = new EditText(this);
        urlBox.setHint("URL (file/http/https/udp/rtp/rtsp/rtmp); not logged");
        urlBox.setSingleLine(true);
        String intentUrl = urlFromIntent();
        if (intentUrl != null) {
            urlBox.setText(intentUrl);
        }
        SurfaceView sv = new SurfaceView(this);
        sv.getHolder().addCallback(this);
        stage = new FrameLayout(this);
        stage.addView(sv, new FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT,
                FrameLayout.LayoutParams.MATCH_PARENT));
        cueViews = new TextView[8];
        for (int i = 0; i < cueViews.length; i++) {
            TextView tv = new TextView(this);
            tv.setTextSize(18f);
            tv.setPadding(16, 8, 16, 16);
            tv.setTextColor(0xFFFFFFFF);
            tv.setShadowLayer(3f, 1f, 1f, 0xFF000000);
            tv.setGravity(Gravity.CENTER_HORIZONTAL | Gravity.BOTTOM);
            tv.setVisibility(View.GONE);
            stage.addView(tv, new FrameLayout.LayoutParams(
                    FrameLayout.LayoutParams.MATCH_PARENT,
                    FrameLayout.LayoutParams.MATCH_PARENT));
            cueViews[i] = tv;
        }
        cueView = cueViews[0];
        bmpViews = new ImageView[8];
        for (int i = 0; i < bmpViews.length; i++) {
            ImageView iv = new ImageView(this);
            iv.setVisibility(View.GONE);
            iv.setScaleType(ImageView.ScaleType.FIT_XY);
            stage.addView(iv, new FrameLayout.LayoutParams(0, 0));
            bmpViews[i] = iv;
        }
        assView = new ImageView(this);
        assView.setVisibility(View.GONE);
        assView.setScaleType(ImageView.ScaleType.FIT_XY);
        stage.addView(assView, new FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT,
                FrameLayout.LayoutParams.MATCH_PARENT));
        subBox = new EditText(this);
        subBox.setHint("optional local .srt/.vtt/.ass/.ssa (path not logged)");
        subBox.setSingleLine(true);
        LinearLayout row1 = new LinearLayout(this);
        row1.setOrientation(LinearLayout.HORIZONTAL);
        LinearLayout row2 = new LinearLayout(this);
        row2.setOrientation(LinearLayout.HORIZONTAL);
        Button open = new Button(this);
        open.setText("Open");
        open.setId(1);
        open.setOnClickListener(this);
        Button cancel = new Button(this);
        cancel.setText("Cancel");
        cancel.setId(2);
        cancel.setOnClickListener(this);
        Button demo = new Button(this);
        demo.setText("Demo AU");
        demo.setId(3);
        demo.setOnClickListener(this);
        Button drive = new Button(this);
        drive.setText("Drive");
        drive.setId(4);
        drive.setOnClickListener(this);
        Button flush = new Button(this);
        flush.setText("Flush");
        flush.setId(5);
        flush.setOnClickListener(this);
        Button stats = new Button(this);
        stats.setText("Stats");
        stats.setId(6);
        stats.setOnClickListener(this);
        row1.addView(open);
        row1.addView(cancel);
        row1.addView(demo);
        row2.addView(drive);
        row2.addView(flush);
        row2.addView(stats);
        LinearLayout row3 = new LinearLayout(this);
        row3.setOrientation(LinearLayout.HORIZONTAL);
        Button tracks = new Button(this);
        tracks.setText("Tracks");
        tracks.setId(7);
        tracks.setOnClickListener(this);
        Button nextA = new Button(this);
        nextA.setText("Next Audio");
        nextA.setId(8);
        nextA.setOnClickListener(this);
        Button sub = new Button(this);
        sub.setText("Next Sub");
        sub.setId(9);
        sub.setOnClickListener(this);
        Button loadSub = new Button(this);
        loadSub.setText("Load Sub");
        loadSub.setId(10);
        loadSub.setOnClickListener(this);
        Button subOff = new Button(this);
        subOff.setText("Sub Off");
        subOff.setId(11);
        subOff.setOnClickListener(this);
        row3.addView(tracks);
        row3.addView(nextA);
        row3.addView(sub);
        LinearLayout row4 = new LinearLayout(this);
        row4.setOrientation(LinearLayout.HORIZONTAL);
        row4.addView(loadSub);
        row4.addView(subOff);
        LinearLayout row5 = new LinearLayout(this);
        row5.setOrientation(LinearLayout.HORIZONTAL);
        Button pause = new Button(this);
        pause.setText("Pause");
        pause.setId(12);
        pause.setOnClickListener(this);
        Button resume = new Button(this);
        resume.setText("Resume");
        resume.setId(13);
        resume.setOnClickListener(this);
        Button caps = new Button(this);
        caps.setText("RateCaps");
        caps.setId(14);
        caps.setOnClickListener(this);
        Button r05 = new Button(this);
        r05.setText("0.5x");
        r05.setId(15);
        r05.setOnClickListener(this);
        Button r10 = new Button(this);
        r10.setText("1x");
        r10.setId(16);
        r10.setOnClickListener(this);
        Button r15 = new Button(this);
        r15.setText("1.5x");
        r15.setId(17);
        r15.setOnClickListener(this);
        Button r20 = new Button(this);
        r20.setText("2x");
        r20.setId(18);
        r20.setOnClickListener(this);
        row5.addView(pause);
        row5.addView(resume);
        row5.addView(caps);
        row5.addView(r05);
        row5.addView(r10);
        row5.addView(r15);
        row5.addView(r20);
        LinearLayout row6 = new LinearLayout(this);
        row6.setOrientation(LinearLayout.HORIZONTAL);
        Button rw = new Button(this);
        rw.setText("-10s");
        rw.setId(19);
        rw.setOnClickListener(this);
        Button ffw = new Button(this);
        ffw.setText("+10s");
        ffw.setId(20);
        ffw.setOnClickListener(this);
        Button pos = new Button(this);
        pos.setText("Pos");
        pos.setId(21);
        pos.setOnClickListener(this);
        Button live = new Button(this);
        live.setText("Live");
        live.setId(22);
        live.setOnClickListener(this);
        Button ai = new Button(this);
        ai.setText("AI sim (not real)");
        ai.setId(23);
        ai.setOnClickListener(this);
        row6.addView(rw);
        row6.addView(ffw);
        row6.addView(live);
        row6.addView(pos);
        row6.addView(ai);
        LinearLayout row7 = new LinearLayout(this);
        row7.setOrientation(LinearLayout.HORIZONTAL);
        Button aspect = new Button(this);
        aspect.setText("Aspect");
        aspect.setId(24);
        aspect.setOnClickListener(this);
        Button shot = new Button(this);
        shot.setText("Shot");
        shot.setId(25);
        shot.setOnClickListener(this);
        Button savePos = new Button(this);
        savePos.setText("SavePos");
        savePos.setId(26);
        savePos.setOnClickListener(this);
        Button loadPos = new Button(this);
        loadPos.setText("LoadPos");
        loadPos.setId(27);
        loadPos.setOnClickListener(this);
        row7.addView(aspect);
        row7.addView(shot);
        row7.addView(savePos);
        row7.addView(loadPos);
        ScrollView scroll = new ScrollView(this);
        scroll.addView(text);
        LinearLayout layout = new LinearLayout(this);
        layout.setOrientation(LinearLayout.VERTICAL);
        gestureLayer = new View(this);
        /* 盖在 Surface / 字幕上，避免子 View 抢走滑动和双击。 */
        stage.addView(gestureLayer, new FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT,
                FrameLayout.LayoutParams.MATCH_PARENT));
        layout.addView(stage, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, 320));
        layout.addView(urlBox);
        layout.addView(subBox);
        layout.addView(row1);
        layout.addView(row2);
        layout.addView(row3);
        layout.addView(row4);
        layout.addView(row5);
        layout.addView(row6);
        layout.addView(row7);
        layout.addView(scroll, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, 0, 1f));
        setContentView(layout);
        registerDebugCmdReceiver();
        installDemoGestures();
    }

    private void registerDebugCmdReceiver() {
        if (debugReceiver != null) {
            return;
        }
        debugReceiver = new MsHwDebugCmdReceiver(this);
        IntentFilter f = new IntentFilter(DEBUG_CMD_ACTION);
        if (Build.VERSION.SDK_INT >= 33) {
            registerReceiver(debugReceiver, f, Context.RECEIVER_EXPORTED);
        } else {
            registerReceiver(debugReceiver, f);
        }
    }

    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        if (intent != null) {
            setIntent(intent);
            applyDebugCmd(intent);
        }
    }

    /** adb: am broadcast -a com.mycjzb.media.player.DEBUG_CMD --es cmd stats|pause|resume|seek|seek_rel|next_audio|next_sub|sub_off|pos|rate|load_sub|lyric|aspect|shot|savepos|loadpos
     *  extras: time_ms / time_90k / delta_ms / delta_90k / path / num / den。不记录 URL。 */
    void applyDebugCmd(Intent in) {
        if (in == null) {
            return;
        }
        String cmd = in.getStringExtra("cmd");
        if (cmd == null || cmd.length() == 0) {
            return;
        }
        cmd = cmd.trim().toLowerCase();
        Log.i(TAG, "debug_cmd=" + cmd);
        if (handle == 0) {
            append("debug_cmd ignored: no player handle");
            return;
        }
        if ("stats".equals(cmd)) {
            append(nativeStats(handle));
        } else if ("pause".equals(cmd)) {
            userPaused = true;
            append(nativePause(handle));
        } else if ("resume".equals(cmd)) {
            userPaused = false;
            if (ensureAudioFocus()) {
                append(nativeResume(handle));
            } else {
                append("resume deferred: no audio focus");
            }
        } else if ("seek".equals(cmd)) {
            long t90 = in.getLongExtra("time_90k", -1L);
            if (t90 < 0) {
                long ms = in.getLongExtra("time_ms", -1L);
                if (ms >= 0) {
                    t90 = ms * 90L;
                }
            }
            if (t90 < 0) {
                append("debug_cmd seek needs time_ms or time_90k");
            } else {
                append(nativeSeek(handle, t90));
                append(nativePosition(handle));
                append(nativeStats(handle));
            }
        } else if ("seek_rel".equals(cmd) || "seekrel".equals(cmd)) {
            long d90 = in.getLongExtra("delta_90k", 0L);
            if (!in.hasExtra("delta_90k")) {
                long ms = in.getLongExtra("delta_ms", 10000L);
                d90 = ms * 90L;
            }
            append(nativeSeekRelative(handle, d90));
            append(nativePosition(handle));
            append(nativeStats(handle));
        } else if ("next_audio".equals(cmd) || "nextaudio".equals(cmd)) {
            cycleKind(1);
            append(nativeStats(handle));
        } else if ("next_sub".equals(cmd) || "nextsub".equals(cmd)) {
            cycleKind(2);
            append(nativeStats(handle));
        } else if ("sub_off".equals(cmd) || "suboff".equals(cmd)) {
            hideSubtitleOverlay();
            append(nativeClearSubtitles(handle));
            hideSubtitleOverlay();
            append(nativeClearSubtitles(handle));
            disableSelectedSubs();
            if (cueView != null) {
                cueView.setText("");
            }
            append(nativeStats(handle));
        } else if ("pos".equals(cmd) || "position".equals(cmd)) {
            append(nativePosition(handle));
        } else if ("rate".equals(cmd)) {
            /* adb --ei num / --ei den；也接受 --el。 */
            int num = in.getIntExtra("num", 0);
            int den = in.getIntExtra("den", 0);
            if (num <= 0) {
                num = (int) in.getLongExtra("num", 1L);
            }
            if (den <= 0) {
                den = (int) in.getLongExtra("den", 1L);
            }
            if (num <= 0) {
                num = 1;
            }
            if (den <= 0) {
                den = 1;
            }
            append(nativeSetRate(handle, num, den));
            append(nativeStats(handle));
        } else if ("load_sub".equals(cmd) || "loadsub".equals(cmd)
                || "lyric".equals(cmd) || "lyrics".equals(cmd)) {
            String p = in.getStringExtra("path");
            if (p != null && p.length() > 0 && subBox != null) {
                subBox.setText(p);
            }
            loadLocalSubtitle();
        } else if ("aspect".equals(cmd)) {
            cycleAspect();
        } else if ("shot".equals(cmd) || "screenshot".equals(cmd)) {
            takeScreenshot();
        } else if ("savepos".equals(cmd) || "save_pos".equals(cmd)) {
            saveBreakpoint(true);
        } else if ("loadpos".equals(cmd) || "load_pos".equals(cmd)) {
            restoreBreakpoint();
        } else {
            append("debug_cmd unknown: " + cmd);
        }
    }

    private void markSessionStarted() {
        intentAutoStarted = true;
        if (!userPaused) {
            ensureAudioFocus();
        }
    }

    private void startUrl(String raw) {
        if (raw == null) {
            raw = "";
        }
        raw = raw.trim();
        if (raw.isEmpty()) {
            append("empty url");
            return;
        }
        markSessionStarted();
        rememberMediaKey(raw);
        if (isFileOnly(raw)) {
            String path = raw.startsWith("file:") ? Uri.parse(raw).getPath() : raw;
            append(nativePlayPath(handle, path));
            startPoll();
            return;
        }
        if (!ffLoaded) {
            append("UNSUPPORTED: libmsff not loaded");
            return;
        }
        String ca = "";
        if (needsTls(raw)) {
            ca = cachedCasPath();
            if (ca == null) {
                if (caExporting) {
                    append("system CA export already running");
                    return;
                }
                pendingUrlAfterCa = raw;
                caExporting = true;
                append("exporting system CA off UI (verification stays required)");
                new MsHwCaExportTask(this).start();
                return;
            }
        }
        append(nativeOpenUrl(handle, raw, ca, openTimeoutMs()));
        startPoll();
    }

    /** 默认 15000。调试可通过 --ei timeout_ms 覆盖，不改默认起播合同。 */
    private int openTimeoutMs() {
        Intent in = getIntent();
        int ms = 15000;
        if (in != null && in.hasExtra("timeout_ms")) {
            int v = in.getIntExtra("timeout_ms", 15000);
            if (v >= 1000 && v <= 120000) {
                ms = v;
            }
        }
        return ms;
    }

    private static boolean isFileOnly(String u) {
        String s = u.toLowerCase();
        return s.startsWith("file:") || s.startsWith("/");
    }

    private static boolean needsTls(String u) {
        String s = u.toLowerCase();
        return s.startsWith("https:") || s.startsWith("tls:")
                || s.startsWith("rtsps:") || s.startsWith("rtmps:");
    }

    File casCacheFile() {
        return new File(getCacheDir(), "ms_system_cas.pem");
    }

    String cachedCasPath() {
        File out = casCacheFile();
        if (out.isFile() && out.length() > 0) {
            return out.getAbsolutePath();
        }
        return null;
    }

    /** 后台线程导出。已有缓存则跳过。失败不得据此关闭校验。 */
    void exportCasToCacheOffUi() {
        File out = casCacheFile();
        if (out.isFile() && out.length() > 0) {
            return;
        }
        try {
            KeyStore ks = KeyStore.getInstance("AndroidCAStore");
            ks.load(null);
            int n = 0;
            FileOutputStream fos = new FileOutputStream(out);
            try {
                Enumeration<String> aliases = ks.aliases();
                byte[] nl = "\n".getBytes("US-ASCII");
                while (aliases.hasMoreElements()) {
                    String alias = aliases.nextElement();
                    if (!ks.isCertificateEntry(alias)) {
                        continue;
                    }
                    java.security.cert.Certificate c = ks.getCertificate(alias);
                    if (!(c instanceof X509Certificate)) {
                        continue;
                    }
                    byte[] der = c.getEncoded();
                    String b64 = Base64.encodeToString(der, Base64.DEFAULT);
                    fos.write("-----BEGIN CERTIFICATE-----\n".getBytes("US-ASCII"));
                    fos.write(b64.getBytes("US-ASCII"));
                    fos.write("-----END CERTIFICATE-----".getBytes("US-ASCII"));
                    fos.write(nl);
                    n++;
                }
            } finally {
                fos.close();
            }
            if (n <= 0 || out.length() <= 0) {
                //noinspection ResultOfMethodCallIgnored
                out.delete();
                return;
            }
            Log.i(TAG, "exported system CA count to cache PEM (path not logged as URL)");
        } catch (Exception e) {
            Log.w(TAG, "system CA export failed");
        }
    }

    void postAfterCasExport() {
        caExporting = false;
        ui.post(this);
    }

    @Override
    public void onClick(View v) {
        int id = v.getId();
        if (id == 1) {
            startUrl(urlBox.getText().toString());
        } else if (id == 2) {
            stopPoll();
            nativeCancel(handle);
            append(nativeStats(handle));
        } else if (id == 3) {
            append(nativeInjectDemo(handle));
        } else if (id == 4) {
            append(nativeDrive(handle));
        } else if (id == 5) {
            nativeFlush(handle);
            append(nativeStats(handle));
        } else if (id == 6) {
            append(nativeStats(handle));
        } else if (id == 7) {
            append(nativeListTracks(handle));
        } else if (id == 8) {
            cycleKind(1);
        } else if (id == 9) {
            cycleKind(2);
        } else if (id == 10) {
            loadLocalSubtitle();
        } else if (id == 11) {
            disableSelectedSubs();
            if (cueView != null) {
                cueView.setText("");
            }
        } else if (id == 12) {
            userPaused = true;
            append(nativePause(handle));
        } else if (id == 13) {
            userPaused = false;
            if (ensureAudioFocus()) {
                append(nativeResume(handle));
            } else {
                append("resume deferred: no audio focus");
            }
        } else if (id == 14) {
            append(nativeRateCaps(handle));
        } else if (id == 15) {
            append(nativeSetRate(handle, 1, 2));
        } else if (id == 16) {
            append(nativeSetRate(handle, 1, 1));
        } else if (id == 17) {
            append(nativeSetRate(handle, 3, 2));
        } else if (id == 18) {
            append(nativeSetRate(handle, 2, 1));
        } else if (id == 19) {
            append(nativeSeekRelative(handle, -10L * 90000L));
        } else if (id == 20) {
            append(nativeSeekRelative(handle, 10L * 90000L));
        } else if (id == 21) {
            append(nativePosition(handle));
        } else if (id == 22) {
            append(nativeSeekLiveEdge(handle));
        } else if (id == 23) {
            append(nativeAiStatus(handle));
            append(nativeAiConfigure(handle, 1, "", "en"));
            append(nativeAiSetEnabled(handle, 1));
            append(nativeSelectTrack(handle, 0xFFFF0002, 1));
            append("AI captions use simulated local fake vendor only; not real recognition.");
        } else if (id == 24) {
            cycleAspect();
        } else if (id == 25) {
            takeScreenshot();
        } else if (id == 26) {
            saveBreakpoint(true);
        } else if (id == 27) {
            restoreBreakpoint();
        }
    }

    /** kind: 1=audio 2=sub. 解析 list 文本循环选择，失败则只展示列表。 */
    private void cycleKind(int kindWanted) {
        String list = nativeListTracks(handle);
        append(list);
        if (list == null) {
            return;
        }
        String[] parts = list.split("\\|");
        java.util.ArrayList<Integer> ids = new java.util.ArrayList<Integer>();
        int current = -1;
        for (String part : parts) {
            boolean kindOk = (kindWanted == 1 && part.contains("kind=audio"))
                    || (kindWanted == 2 && part.contains("kind=sub"));
            if (!kindOk) {
                continue;
            }
            int idv = parseTrackId(part);
            if (idv == 0) {
                continue;
            }
            ids.add(Integer.valueOf(idv));
            if (part.contains("sel=1")) {
                current = ids.size() - 1;
            }
        }
        if (ids.isEmpty()) {
            append("no tracks of requested kind (host/UI only; not device acceptance)");
            return;
        }
        int next = (current + 1) % ids.size();
        if (kindWanted == 2 && current >= 0 && next == 0) {
            /* 循环一圈后关闭字幕 */
            hideSubtitleOverlay();
            append(nativeClearSubtitles(handle));
            int curId = ids.get(current).intValue();
            append(nativeSelectTrack(handle, curId, 0));
            return;
        }
        int nid = ids.get(next).intValue();
        if (kindWanted == 2) {
            subsHidden = false;
        }
        append(nativeSelectTrack(handle, nid, 1));
    }

    private void disableSelectedSubs() {
        String list = nativeListTracks(handle);
        if (list == null || list.length() == 0) {
            return;
        }
        String[] parts = list.split("\\|");
        for (String part : parts) {
            if (!part.contains("kind=sub") || !part.contains("sel=1")) {
                continue;
            }
            int idv = parseTrackId(part);
            if (idv != 0) {
                append(nativeSelectTrack(handle, idv, 0));
            }
        }
    }

    private static String NULL_SAFE(String s) {
        return s == null ? "" : s;
    }

    private static int parseTrackId(String part) {
        int at = part.indexOf("id=");
        if (at < 0) {
            return 0;
        }
        int i = at + 3;
        int v = 0;
        while (i < part.length()) {
            char c = part.charAt(i);
            if (c < '0' || c > '9') {
                break;
            }
            v = v * 10 + (c - '0');
            i++;
        }
        return v;
    }

    private void loadLocalSubtitle() {
        String path = subBox != null ? subBox.getText().toString().trim() : "";
        if (path.length() == 0) {
            append("empty subtitle path");
            return;
        }
        try {
            java.io.File f = new java.io.File(path);
            if (!f.isFile() || f.length() <= 0 || f.length() > 262144) {
                append("subtitle file missing or over 256KiB cap");
                return;
            }
            java.io.FileInputStream in = new java.io.FileInputStream(f);
            byte[] data = new byte[(int) f.length()];
            int n = in.read(data);
            in.close();
            if (n <= 0) {
                append("subtitle read empty");
                return;
            }
            if (n < data.length) {
                byte[] slim = new byte[n];
                System.arraycopy(data, 0, slim, 0, n);
                data = slim;
            }
            int fmt = 0;
            String low = path.toLowerCase();
            if (low.endsWith(".vtt") || low.endsWith(".webvtt")) {
                fmt = 2;
            } else if (low.endsWith(".ass")) {
                fmt = 3;
            } else if (low.endsWith(".ssa")) {
                fmt = 4;
            } else if (low.endsWith(".srt")) {
                fmt = 1;
            } else if (low.endsWith(".lrc")) {
                /* 歌词转 SRT 复用外挂字幕通道，不新增 ABI 字段。 */
                byte[] srt = lrcToSrt(data);
                if (srt == null || srt.length == 0) {
                    append("lrc convert empty");
                    return;
                }
                data = srt;
                fmt = 1;
            }
            subsHidden = false;
            append(nativeLoadSubtitle(handle, data, fmt));
            if (low.endsWith(".lrc")) {
                append("lyrics loaded as external SRT (overlay only; not device paint acceptance)");
            }
        } catch (Exception e) {
            append("subtitle load failed");
        }
    }

    private void startPoll() {
        stopPoll();
        polling = true;
        ui.postDelayed(this, 200);
    }

    private void stopPoll() {
        polling = false;
        ui.removeCallbacks(this);
    }

    @Override
    public void run() {
        if (pendingUrlAfterCa != null && !caExporting) {
            String u = pendingUrlAfterCa;
            pendingUrlAfterCa = null;
            if (needsTls(u) && cachedCasPath() == null) {
                append("UNSUPPORTED: TLS trust anchors missing; verification stays required");
                return;
            }
            startUrl(u);
            return;
        }
        if (!polling || handle == 0) {
            return;
        }
        /* worker 持续 drive。UI 只短超时取 stats，避免在 configure/decode
         * 持锁时把主线程卡住（ANR 后系统会冻结进程，命令再也送不进）。 */
        String st = nativeStats(handle);
        if (st == null || !st.startsWith("busy")) {
            append(st);
            applyCuesFromNative();
        }
        ui.postDelayed(this, 200);
    }


    /* 完整 ASS RGBA 叠加到 ImageView。逻辑接入，不是设备视觉验收，也不读回视频 Surface。 */
    private void hideSubtitleOverlay() {
        subsHidden = true;
        if (assView != null) {
            assView.setVisibility(View.GONE);
            assView.setImageBitmap(null);
        }
        if (cueView != null) {
            cueView.setText("");
        }
    }

    private boolean applyAssOverlay() {
        if (assView == null || handle == 0) {
            return false;
        }
        if (subsHidden) {
            assView.setVisibility(View.GONE);
            assView.setImageBitmap(null);
            return false;
        }
        int avail;
        try {
            avail = nativeAssAvailable(handle);
        } catch (UnsatisfiedLinkError e) {
            assView.setVisibility(View.GONE);
            return false;
        }
        if (avail <= 0) {
            assView.setVisibility(View.GONE);
            return false;
        }
        if (avail < 2) {
            assView.setVisibility(View.GONE);
            assView.setImageBitmap(null);
            return false;
        }
        if (stage != null && stage.getWidth() > 16 && stage.getHeight() > 16) {
            try {
                nativeAssSetCanvas(handle, stage.getWidth(), stage.getHeight());
            } catch (UnsatisfiedLinkError e) {
                /* keep previous canvas */
            }
        }
        int[] geom = new int[12];
        int need;
        try {
            need = nativeFillAssFrame(handle, geom, null);
        } catch (UnsatisfiedLinkError e) {
            assView.setVisibility(View.GONE);
            return false;
        }
        int cw = geom[2];
        int ch = geom[3];
        if (need <= 0 || cw <= 0 || ch <= 0 || (long)cw * (long)ch * 4L > need) {
            assView.setVisibility(View.GONE);
            assView.setImageBitmap(null);
            return true; /* script present but empty at this time */
        }
        byte[] rgba = new byte[need];
        int got = nativeFillAssFrame(handle, geom, rgba);
        if (got <= 0) {
            assView.setVisibility(View.GONE);
            return true;
        }
        int[] pixels = new int[cw * ch];
        for (int p = 0; p < cw * ch; p++) {
            int o = p * 4;
            int r = rgba[o] & 0xFF;
            int g = rgba[o + 1] & 0xFF;
            int b = rgba[o + 2] & 0xFF;
            int a = rgba[o + 3] & 0xFF;
            pixels[p] = (a << 24) | (r << 16) | (g << 8) | b;
        }
        Bitmap bm = Bitmap.createBitmap(cw, ch, Bitmap.Config.ARGB_8888);
        bm.setPixels(pixels, 0, cw, 0, 0, cw, ch);
        assView.setImageBitmap(bm);
        assView.setVisibility(View.VISIBLE);
        FrameLayout.LayoutParams lp = (FrameLayout.LayoutParams) assView.getLayoutParams();
        if (lp == null) {
            lp = new FrameLayout.LayoutParams(
                    FrameLayout.LayoutParams.MATCH_PARENT,
                    FrameLayout.LayoutParams.MATCH_PARENT);
        }
        lp.gravity = Gravity.TOP | Gravity.LEFT;
        lp.width = FrameLayout.LayoutParams.MATCH_PARENT;
        lp.height = FrameLayout.LayoutParams.MATCH_PARENT;
        lp.leftMargin = 0;
        lp.topMargin = 0;
        assView.setLayoutParams(lp);
        return true;
    }

    /* 把 RGBA cue 放到 ImageView。逻辑接入，不是设备出画/视觉验收。 */
    private void applyBitmapCue(int index, int posx, int posy, int playx, int playy) {
        if (bmpViews == null || index < 0 || index >= bmpViews.length || bmpViews[index] == null) {
            return;
        }
        int[] geom = new int[12];
        int need = 0;
        try {
            need = nativeFillBitmap(handle, index, geom, null);
        } catch (UnsatisfiedLinkError e) {
            return;
        }
        if (need <= 0) {
            return;
        }
        byte[] rgba = new byte[need];
        int got = nativeFillBitmap(handle, index, geom, rgba);
        if (got <= 0) {
            return;
        }
        int w = geom[2];
        int h = geom[3];
        if (w <= 0 || h <= 0 || (long)w * (long)h * 4L > rgba.length) {
            return;
        }
        int[] pixels = new int[w * h];
        for (int p = 0; p < w * h; p++) {
            int o = p * 4;
            int r = rgba[o] & 0xFF;
            int g = rgba[o + 1] & 0xFF;
            int b = rgba[o + 2] & 0xFF;
            int a = rgba[o + 3] & 0xFF;
            pixels[p] = (a << 24) | (r << 16) | (g << 8) | b;
        }
        Bitmap bm = Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888);
        bm.setPixels(pixels, 0, w, 0, 0, w, h);
        ImageView iv = bmpViews[index];
        iv.setImageBitmap(bm);
        iv.setVisibility(View.VISIBLE);
        FrameLayout.LayoutParams lp = (FrameLayout.LayoutParams) iv.getLayoutParams();
        if (lp == null) {
            lp = new FrameLayout.LayoutParams(w, h);
        }
        int px = geom[0];
        int py = geom[1];
        int cw = geom[8] > 0 ? geom[8] : playx;
        int ch = geom[9] > 0 ? geom[9] : playy;
        if (cw > 0 && ch > 0 && stage != null && stage.getWidth() > 0 && stage.getHeight() > 0) {
            lp.width = Math.max(1, w * stage.getWidth() / cw);
            lp.height = Math.max(1, h * stage.getHeight() / ch);
            lp.gravity = Gravity.TOP | Gravity.LEFT;
            lp.leftMargin = px * stage.getWidth() / cw;
            lp.topMargin = py * stage.getHeight() / ch;
        } else {
            lp.width = w;
            lp.height = h;
            lp.gravity = Gravity.BOTTOM | Gravity.CENTER_HORIZONTAL;
            lp.leftMargin = 0;
            lp.topMargin = 0;
        }
        iv.setLayoutParams(lp);
    }

    /* 只把解析后的样式接到 TextView，不把 ASS 覆盖标签当正文。
     * 这是逻辑接入，不是设备出画/视觉验收。 */
    private void applyCuesFromNative() {
        if (cueViews == null || handle == 0) {
            return;
        }
        int[] meta = new int[8 * 16];
        String[] texts = new String[8];
        int[] spans = new int[8 * 8 * 8];
        int n = 0;
        try {
            n = nativeFillCues(handle, meta, texts, spans);
        } catch (UnsatisfiedLinkError e) {
            String cue = nativeCurrentCue(handle);
            if (cueViews[0] != null) {
                cueViews[0].setVisibility(View.VISIBLE);
                cueViews[0].setText(cue != null ? cue : "");
            }
            return;
        }
        if (n < 0) {
            n = 0;
        }
        if (n > cueViews.length) {
            n = cueViews.length;
        }
        if (bmpViews != null) {
            for (int i = 0; i < bmpViews.length; i++) {
                if (bmpViews[i] != null) {
                    bmpViews[i].setVisibility(View.GONE);
                    bmpViews[i].setImageBitmap(null);
                }
            }
        }
        boolean assActive = applyAssOverlay();
        for (int i = 0; i < cueViews.length; i++) {
            TextView tv = cueViews[i];
            if (tv == null) {
                continue;
            }
            if (i >= n || texts[i] == null) {
                tv.setText("");
                tv.setVisibility(View.GONE);
                continue;
            }
            int flags0 = meta[i * 16 + 9];
            if ((flags0 & (1 << 9)) != 0) {
                /* 位图：不把二进制当文字，走 ImageView。不是真机视觉验收。 */
                tv.setText("");
                tv.setVisibility(View.GONE);
                applyBitmapCue(i, meta[i * 16 + 10], meta[i * 16 + 11],
                               meta[i * 16 + 12], meta[i * 16 + 13]);
                continue;
            }
            if (assActive) {
                /* 完整栅格已覆盖文字 ASS，不再用去标签 TextView。 */
                tv.setText("");
                tv.setVisibility(View.GONE);
                continue;
            }
            int base = i * 16;
            int layer = meta[base];
            int an = meta[base + 1];
            int fs = meta[base + 2];
            int color = meta[base + 3];
            int bold = meta[base + 4];
            int italic = meta[base + 5];
            int ml = meta[base + 6];
            int mr = meta[base + 7];
            int mv = meta[base + 8];
            int flags = meta[base + 9];
            int posx = meta[base + 10];
            int posy = meta[base + 11];
            int playx = meta[base + 12];
            int playy = meta[base + 13];
            int spanCount = meta[base + 15];
            tv.setVisibility(View.VISIBLE);
            tv.setPadding(Math.max(8, ml), Math.max(8, mv), Math.max(8, mr), Math.max(8, mv));
            if ((flags & 1) != 0 && color != 0) {
                tv.setTextColor(color);
            } else {
                tv.setTextColor(0xFFFFFFFF);
            }
            if ((flags & 2) != 0 && fs > 0) {
                float sp = (float) fs;
                if (playy > 0 && stage != null && stage.getHeight() > 0) {
                    sp = (float) fs * (float) stage.getHeight() / (float) playy;
                    /* 逻辑缩放，不是视觉验收。 */
                    if (sp < 8f) {
                        sp = 8f;
                    }
                    if (sp > 96f) {
                        sp = 96f;
                    }
                }
                tv.setTextSize(sp);
            } else {
                tv.setTextSize(18f);
            }
            int style = Typeface.NORMAL;
            if (bold != 0 && italic != 0) {
                style = Typeface.BOLD_ITALIC;
            } else if (bold != 0) {
                style = Typeface.BOLD;
            } else if (italic != 0) {
                style = Typeface.ITALIC;
            }
            tv.setTypeface(Typeface.DEFAULT, style);
            FrameLayout.LayoutParams lp = (FrameLayout.LayoutParams) tv.getLayoutParams();
            if (lp == null) {
                lp = new FrameLayout.LayoutParams(
                        FrameLayout.LayoutParams.WRAP_CONTENT,
                        FrameLayout.LayoutParams.WRAP_CONTENT);
            }
            if ((flags & (1 << 6)) != 0 && playx > 0 && playy > 0 && stage != null
                    && stage.getWidth() > 0 && stage.getHeight() > 0) {
                lp.width = FrameLayout.LayoutParams.WRAP_CONTENT;
                lp.height = FrameLayout.LayoutParams.WRAP_CONTENT;
                lp.gravity = Gravity.TOP | Gravity.LEFT;
                lp.leftMargin = posx * stage.getWidth() / playx;
                lp.topMargin = posy * stage.getHeight() / playy;
            } else {
                lp.width = FrameLayout.LayoutParams.MATCH_PARENT;
                lp.height = FrameLayout.LayoutParams.MATCH_PARENT;
                lp.leftMargin = 0;
                lp.topMargin = 0;
                lp.gravity = anToGravity(an);
            }
            tv.setLayoutParams(lp);
            /* View.setElevation 是 API 21；API 18–20 无阴影叠层，文本/位图字幕仍按 z 序 addView。 */
            if (Build.VERSION.SDK_INT >= 21) {
                tv.setElevation((float) layer);
            }
            SpannableString ss = new SpannableString(texts[i]);
            if (spanCount > 0) {
                for (int s = 0; s + 7 < spans.length; s += 8) {
                    if (spans[s + 7] != i) {
                        continue;
                    }
                    int start = spans[s];
                    int end = spans[s + 1];
                    int sflags = spans[s + 2];
                    if (start < 0 || end > ss.length() || start >= end) {
                        continue;
                    }
                    int st = Typeface.NORMAL;
                    if ((sflags & 4) != 0 && spans[s + 5] != 0) {
                        st |= Typeface.BOLD;
                    }
                    if ((sflags & 8) != 0 && spans[s + 6] != 0) {
                        st |= Typeface.ITALIC;
                    }
                    if (st != Typeface.NORMAL) {
                        ss.setSpan(new StyleSpan(st), start, end, Spanned.SPAN_EXCLUSIVE_EXCLUSIVE);
                    }
                    if ((sflags & 1) != 0) {
                        ss.setSpan(new ForegroundColorSpan(spans[s + 3]), start, end,
                                Spanned.SPAN_EXCLUSIVE_EXCLUSIVE);
                    }
                }
            }
            tv.setText(ss);
        }
    }

    private static int anToGravity(int an) {
        switch (an) {
            case 1:
                return Gravity.BOTTOM | Gravity.LEFT;
            case 3:
                return Gravity.BOTTOM | Gravity.RIGHT;
            case 4:
                return Gravity.CENTER_VERTICAL | Gravity.LEFT;
            case 5:
                return Gravity.CENTER;
            case 6:
                return Gravity.CENTER_VERTICAL | Gravity.RIGHT;
            case 7:
                return Gravity.TOP | Gravity.LEFT;
            case 8:
                return Gravity.TOP | Gravity.CENTER_HORIZONTAL;
            case 9:
                return Gravity.TOP | Gravity.RIGHT;
            case 2:
            default:
                return Gravity.BOTTOM | Gravity.CENTER_HORIZONTAL;
        }
    }


    /** 显示器 HDR 能力，仅供对照。模拟器不是真实 HDR 显示，不得据此宣称效果已验。 */
    private String displayHdrCapsLine() {
        if (Build.VERSION.SDK_INT < 24) {
            return "display_hdr=unknown (API<24; AVD is not a real HDR display; not a display-effect claim)";
        }
        try {
            Display d = getWindowManager().getDefaultDisplay();
            Display.HdrCapabilities cap = d.getHdrCapabilities();
            if (cap == null) {
                return "display_hdr=none (no caps; AVD is not a real HDR display; not a display-effect claim)";
            }
            int[] types = cap.getSupportedHdrTypes();
            StringBuilder sb = new StringBuilder("display_hdr=");
            if (types == null || types.length == 0) {
                sb.append("none");
            } else {
                for (int i = 0; i < types.length; i++) {
                    if (i > 0) {
                        sb.append(',');
                    }
                    sb.append(hdrTypeName(types[i]));
                }
            }
            sb.append(" maxLuma=").append(cap.getDesiredMaxLuminance());
            sb.append(" (AVD/emulator is not a real HDR display; not a display-effect claim)");
            return sb.toString();
        } catch (Throwable t) {
            return "display_hdr=unavailable (not a display-effect claim)";
        }
    }

    private static String hdrTypeName(int t) {
        switch (t) {
            case Display.HdrCapabilities.HDR_TYPE_DOLBY_VISION:
                return "dolby_vision";
            case Display.HdrCapabilities.HDR_TYPE_HDR10:
                return "hdr10";
            case Display.HdrCapabilities.HDR_TYPE_HLG:
                return "hlg";
            case Display.HdrCapabilities.HDR_TYPE_HDR10_PLUS:
                return "hdr10plus";
            default:
                return "type" + t;
        }
    }

    private void append(String s) {
        if (s == null) {
            s = "";
        }
        if (s.contains("hdr=") || s.contains("write_frames=") || s.contains("video_au_submitted")) {
            String cap = displayHdrCapsLine();
            if (s.indexOf("display_hdr=") < 0) {
                s = s + "\n" + cap;
            }
        }
        text.setText(s);
        /* 不把输入框 URL 打进日志；native 文本也不含 URL。 */
        Log.i(TAG, s);
    }

    @Override
    public void surfaceCreated(SurfaceHolder holder) {
        Surface s = holder.getSurface();
        append(nativeSetSurface(handle, s));
        /* 只在尚未自动开播时从 Intent 启动；重建 Surface 只重绑，不重开会话。 */
        if (intentAutoStarted) {
            startPoll();
            return;
        }
        String path = filePathFromIntent();
        if (path != null && path.length() > 0) {
            markSessionStarted();
            rememberMediaKey(path);
            append(nativePlayPath(handle, path));
            startPoll();
        } else {
            String u = urlFromIntent();
            if (u != null && u.length() > 0 && !isFileOnly(u)) {
                startUrl(u);
            }
        }
    }

    private String filePathFromIntent() {
        Intent in = getIntent();
        if (in == null) {
            return null;
        }
        if (Intent.ACTION_VIEW.equals(in.getAction()) && in.getData() != null) {
            Uri u = in.getData();
            if ("file".equals(u.getScheme())) {
                return u.getPath();
            }
        }
        return in.getStringExtra("path");
    }

    private String urlFromIntent() {
        Intent in = getIntent();
        if (in == null) {
            return null;
        }
        if (in.getStringExtra("url") != null) {
            return in.getStringExtra("url");
        }
        if (Intent.ACTION_VIEW.equals(in.getAction()) && in.getData() != null) {
            Uri u = in.getData();
            String sch = u.getScheme();
            if (sch != null && !"file".equals(sch) && !"content".equals(sch)) {
                return u.toString();
            }
        }
        return null;
    }

    @Override
    public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
    }

    @Override
    public void surfaceDestroyed(SurfaceHolder holder) {
        if (handle != 0) {
            nativeSetSurface(handle, null);
        }
    }

    @Override
    protected void onPause() {
        saveBreakpoint(false);
        /* 放弃焦点会走到 LOSS，从而暂停音频；不调用 nativePause，以免当成用户暂停。 */
        abandonAudioFocus();
        super.onPause();
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (intentAutoStarted && !userPaused) {
            /* 回前台始终重新申请焦点；即使系统未授予，也通知 GAIN 以恢复出画
             *（非用户暂停）。否则 focus_paused 会一直挡住 pump。 */
            ensureAudioFocus();
            if (handle != 0) {
                nativeNotifyAudioFocus(handle, 1); /* MS_HW_AUDIO_FOCUS_GAIN */
            }
        }
    }

    @Override
    protected void onDestroy() {
        stopPoll();
        if (debugReceiver != null) {
            try {
                unregisterReceiver(debugReceiver);
            } catch (Exception e) {
                /* already unregistered */
            }
            debugReceiver = null;
        }
        saveBreakpoint(false);
        abandonAudioFocus();
        if (handle != 0) {
            nativeCancel(handle);
            nativeDestroy(handle);
            handle = 0;
        }
        super.onDestroy();
    }

    private boolean ensureAudioFocus() {
        if (audioMgr == null || hasAudioFocus) {
            return hasAudioFocus || audioMgr == null;
        }
        int r;
        if (Build.VERSION.SDK_INT >= 26) {
            if (focusRequest == null) {
                focusRequest = new AudioFocusRequest.Builder(AudioManager.AUDIOFOCUS_GAIN)
                        .setOnAudioFocusChangeListener(focusListener, ui)
                        .setWillPauseWhenDucked(true)
                        .build();
            }
            r = audioMgr.requestAudioFocus(focusRequest);
        } else {
            r = audioMgr.requestAudioFocus(focusListener, AudioManager.STREAM_MUSIC,
                    AudioManager.AUDIOFOCUS_GAIN);
        }
        hasAudioFocus = (r == AudioManager.AUDIOFOCUS_REQUEST_GRANTED);
        if (hasAudioFocus && handle != 0) {
            nativeNotifyAudioFocus(handle, 1);
        }
        return hasAudioFocus;
    }

    private void abandonAudioFocus() {
        if (audioMgr == null) {
            return;
        }
        if (Build.VERSION.SDK_INT >= 26) {
            if (focusRequest != null) {
                audioMgr.abandonAudioFocusRequest(focusRequest);
            }
        } else {
            audioMgr.abandonAudioFocus(focusListener);
        }
        hasAudioFocus = false;
        if (handle != 0) {
            nativeNotifyAudioFocus(handle, 2); /* LOSS */
        }
    }

    void onSystemAudioFocus(int change) {
        if (handle == 0) {
            return;
        }
        int ev;
        if (change == AudioManager.AUDIOFOCUS_GAIN) {
            hasAudioFocus = true;
            ev = 1;
        } else if (change == AudioManager.AUDIOFOCUS_LOSS) {
            hasAudioFocus = false;
            ev = 2;
        } else if (change == AudioManager.AUDIOFOCUS_LOSS_TRANSIENT) {
            hasAudioFocus = false;
            ev = 3;
        } else if (change == AudioManager.AUDIOFOCUS_LOSS_TRANSIENT_CAN_DUCK) {
            hasAudioFocus = false;
            ev = 4;
        } else {
            return;
        }
        append(nativeNotifyAudioFocus(handle, ev));
    }
    void onDemoDoubleTap() {
        if (handle == 0) {
            return;
        }
        if (userPaused) {
            userPaused = false;
            if (ensureAudioFocus()) {
                append(nativeResume(handle));
            } else {
                append("resume deferred: no audio focus");
            }
        } else {
            userPaused = true;
            append(nativePause(handle));
        }
    }

    boolean onDemoFling(MotionEvent e1, MotionEvent e2) {
        if (handle == 0 || e1 == null || e2 == null) {
            return false;
        }
        float dx = e2.getX() - e1.getX();
        float dy = e2.getY() - e1.getY();
        if (Math.abs(dx) < 80 || Math.abs(dx) < Math.abs(dy) * 1.5f) {
            return false;
        }
        long delta = dx > 0 ? 10L * 90000L : -10L * 90000L;
        append(nativeSeekRelative(handle, delta));
        append(nativePosition(handle));
        return true;
    }

    boolean forwardDemoTouch(MotionEvent event) {
        return gestures != null && gestures.onTouchEvent(event);
    }

    private void installDemoGestures() {
        gestures = new GestureDetector(this, new MsHwDemoGestureListener(this));
        View target = gestureLayer != null ? gestureLayer : stage;
        if (target != null) {
            target.setOnTouchListener(new MsHwDemoTouchListener(this));
        }
    }

    private void cycleAspect() {
        aspectMode = (aspectMode + 1) % 3;
        applyAspect();
    }

    private void applyAspect() {
        if (stage == null) {
            return;
        }
        int n = stage.getChildCount();
        View surface = n > 0 ? stage.getChildAt(0) : null;
        if (surface == null) {
            return;
        }
        int stageW = stage.getWidth() > 0 ? stage.getWidth() : stage.getMeasuredWidth();
        int stageH = stage.getHeight() > 0 ? stage.getHeight() : 320;
        if (stageW <= 0) {
            stageW = 480;
        }
        FrameLayout.LayoutParams lp;
        if (aspectMode == 1) {
            /* 16:9 信箱 */
            int w = stageW;
            int h = w * 9 / 16;
            if (h > stageH && stageH > 0) {
                h = stageH;
                w = h * 16 / 9;
            }
            lp = new FrameLayout.LayoutParams(w, h, Gravity.CENTER);
            append("aspect=16:9 letterbox " + w + "x" + h);
        } else if (aspectMode == 2) {
            /* 居中裁切：短边铺满 */
            int w = stageW;
            int h = w * 9 / 16;
            if (h < stageH && stageH > 0) {
                h = stageH;
                w = h * 16 / 9;
            }
            lp = new FrameLayout.LayoutParams(w, h, Gravity.CENTER);
            append("aspect=crop " + w + "x" + h);
        } else {
            lp = new FrameLayout.LayoutParams(
                    FrameLayout.LayoutParams.MATCH_PARENT,
                    FrameLayout.LayoutParams.MATCH_PARENT);
            append("aspect=stretch");
        }
        surface.setLayoutParams(lp);
        stage.requestLayout();
    }

    /**
     * API 24+ 尝试 PixelCopy（可能仍拿不到 Surface 像素）。
     * API 18–23 只能抓 View 层级，SurfaceView 视频区通常是黑的。
     * 截图前暂时遮住 URL 栏，避免把地址写进图片。
     */
    private void takeScreenshot() {
        final View root = getWindow() != null ? getWindow().getDecorView() : null;
        if (root == null) {
            append("screenshot: no decor view");
            return;
        }
        final CharSequence oldUrl = urlBox != null ? urlBox.getText() : null;
        if (urlBox != null) {
            urlBox.setText("(url hidden)");
        }
        File out = new File(getFilesDir(), "screenshot-demo.png");
        boolean ok = false;
        try {
            ok = shotDrawingCache(root, out);
            if (ok) {
                append("screenshot saved " + out.getName() + " " + out.length()
                        + "B (URL bar redacted; Surface may be black on API 18)");
            } else {
                append("screenshot failed");
            }
        } finally {
            if (urlBox != null && oldUrl != null) {
                urlBox.setText(oldUrl);
            }
        }
    }

    private boolean shotDrawingCache(View root, File out) {
        int w = root.getWidth();
        int h = root.getHeight();
        if (w <= 0 || h <= 0) {
            return false;
        }
        Bitmap bmp = Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888);
        Canvas c = new Canvas(bmp);
        root.draw(c);
        return writePng(bmp, out);
    }

    private boolean shotPixelCopy(View root, File out) {
        /* PixelCopy 需要 Surface 与异步回调；演示先走 View 绘制，避免在 UI 线程阻塞等待。 */
        return shotDrawingCache(root, out);
    }

    private boolean writePng(Bitmap bmp, File out) {
        FileOutputStream fos = null;
        try {
            fos = new FileOutputStream(out);
            boolean ok = bmp.compress(Bitmap.CompressFormat.PNG, 100, fos);
            fos.close();
            fos = null;
            return ok && out.length() > 0;
        } catch (Exception e) {
            return false;
        } finally {
            if (fos != null) {
                try {
                    fos.close();
                } catch (Exception ignored) {
                }
            }
        }
    }

    private void rememberMediaKey(String raw) {
        lastMediaKey = mediaKeyOf(raw);
    }

    private static String mediaKeyOf(String raw) {
        if (raw == null) {
            return "";
        }
        String s = raw.trim();
        int h = s.hashCode();
        return "m" + Integer.toHexString(h);
    }

    private void saveBreakpoint(boolean fromUser) {
        if (handle == 0 || lastMediaKey == null || lastMediaKey.length() == 0) {
            if (fromUser) {
                append("savepos: no media key");
            }
            return;
        }
        String pos = nativePosition(handle);
        long t90 = parsePos90k(pos);
        if (t90 < 0) {
            if (fromUser) {
                append("savepos: cannot parse position");
            }
            return;
        }
        if (t90 <= 90L * 1000L) {
            if (fromUser) {
                append("savepos skipped (near start)");
            }
            return;
        }
        SharedPreferences sp = getSharedPreferences(PREF_RESUME, MODE_PRIVATE);
        sp.edit().putLong(lastMediaKey, t90).commit();
        append("savepos key=" + lastMediaKey + " t=" + (t90 / 90L) + "ms (no URL logged)");
    }

    private void restoreBreakpoint() {
        if (handle == 0 || lastMediaKey == null || lastMediaKey.length() == 0) {
            append("loadpos: no media key");
            return;
        }
        SharedPreferences sp = getSharedPreferences(PREF_RESUME, MODE_PRIVATE);
        long t90 = sp.getLong(lastMediaKey, -1L);
        if (t90 <= 0) {
            append("loadpos: no saved point for " + lastMediaKey);
            return;
        }
        append(nativeSeek(handle, t90));
        append(nativePosition(handle));
        append("loadpos key=" + lastMediaKey + " t=" + (t90 / 90L) + "ms");
    }

    private static long parsePos90k(String pos) {
        if (pos == null) {
            return -1;
        }
        int at = pos.indexOf("pos_90k=");
        if (at < 0) {
            at = pos.indexOf("time_90k=");
        }
        if (at >= 0) {
            int i = pos.indexOf('=', at) + 1;
            long v = 0;
            int n = 0;
            while (i < pos.length()) {
                char c = pos.charAt(i);
                if (c < '0' || c > '9') {
                    break;
                }
                v = v * 10 + (c - '0');
                n++;
                i++;
            }
            return n > 0 ? v : -1;
        }
        /* nativePosition 输出 pos=12.376s，不是 pos_90k=。 */
        at = pos.indexOf("pos=");
        if (at < 0) {
            return -1;
        }
        int i = at + 4;
        int start = i;
        while (i < pos.length()) {
            char c = pos.charAt(i);
            if ((c >= '0' && c <= '9') || c == '.' || c == '-') {
                i++;
            } else {
                break;
            }
        }
        if (i <= start) {
            return -1;
        }
        try {
            double sec = Double.parseDouble(pos.substring(start, i));
            if (sec < 0) {
                return -1;
            }
            return (long) (sec * 90000.0 + 0.5);
        } catch (NumberFormatException e) {
            return -1;
        }
    }

    /** 简易 LRC → SRT。支持 [mm:ss.xx] 与多时间戳一行。 */
    private static byte[] lrcToSrt(byte[] raw) {
        if (raw == null || raw.length == 0) {
            return null;
        }
        String text;
        try {
            text = new String(raw, "UTF-8");
        } catch (Exception e) {
            return null;
        }
        java.util.ArrayList<long[]> times = new java.util.ArrayList<long[]>();
        java.util.ArrayList<String> lines = new java.util.ArrayList<String>();
        String[] rows = text.replace("\r\n", "\n").replace('\r', '\n').split("\n");
        for (int r = 0; r < rows.length; r++) {
            String row = rows[r].trim();
            if (row.length() == 0 || row.charAt(0) != '[') {
                continue;
            }
            int tpos = 0;
            java.util.ArrayList<Long> stamps = new java.util.ArrayList<Long>();
            while (tpos < row.length() && row.charAt(tpos) == '[') {
                int end = row.indexOf(']', tpos);
                if (end < 0) {
                    break;
                }
                String tag = row.substring(tpos + 1, end);
                long ms = parseLrcTag(tag);
                tpos = end + 1;
                if (ms >= 0) {
                    stamps.add(Long.valueOf(ms));
                } else {
                    stamps.clear();
                    break;
                }
            }
            String body = tpos <= row.length() ? row.substring(tpos).trim() : "";
            if (stamps.isEmpty() || body.length() == 0) {
                continue;
            }
            for (int k = 0; k < stamps.size(); k++) {
                long ms = stamps.get(k).longValue();
                times.add(new long[] { ms, 0 });
                lines.add(body);
            }
        }
        if (lines.isEmpty()) {
            return null;
        }
        /* 结束时间用下一句起点，缺省 +2s */
        for (int i = 0; i < times.size(); i++) {
            long start = times.get(i)[0];
            long end = start + 2000;
            if (i + 1 < times.size() && times.get(i + 1)[0] > start) {
                end = times.get(i + 1)[0];
            }
            times.get(i)[1] = end;
        }
        StringBuilder sb = new StringBuilder();
        for (int i = 0; i < lines.size(); i++) {
            sb.append(i + 1).append("\n");
            sb.append(srtTime(times.get(i)[0])).append(" --> ").append(srtTime(times.get(i)[1])).append("\n");
            sb.append(lines.get(i)).append("\n\n");
        }
        try {
            return sb.toString().getBytes("UTF-8");
        } catch (Exception e) {
            return null;
        }
    }

    private static long parseLrcTag(String tag) {
        if (tag == null) {
            return -1;
        }
        int colon = tag.indexOf(':');
        if (colon <= 0) {
            return -1;
        }
        try {
            int mm = Integer.parseInt(tag.substring(0, colon).trim());
            String rest = tag.substring(colon + 1).trim().replace(',', '.');
            float sec = Float.parseFloat(rest);
            if (mm < 0 || sec < 0) {
                return -1;
            }
            return mm * 60000L + (long) (sec * 1000.0f);
        } catch (Exception e) {
            return -1;
        }
    }

    private static String srtTime(long ms) {
        if (ms < 0) {
            ms = 0;
        }
        long h = ms / 3600000L;
        long m = (ms % 3600000L) / 60000L;
        long s = (ms % 60000L) / 1000L;
        long cs = ms % 1000L;
        return String.format("%02d:%02d:%02d,%03d", Long.valueOf(h), Long.valueOf(m),
                Long.valueOf(s), Long.valueOf(cs));
    }
}

/** 同文件顶层类（非内部类，便于 d8）。后台导出 AndroidCAStore，不在 UI 线程做大量读证书。 */
final class MsHwCaExportTask extends Thread {
    private final MsHwPlayerActivity act;
    MsHwCaExportTask(MsHwPlayerActivity act) {
        this.act = act;
    }
    public void run() {
        try {
            act.exportCasToCacheOffUi();
        } finally {
            act.postAfterCasExport();
        }
    }
}

/** 同文件顶层类（非内部类，便于 d8）。系统音频焦点回调。 */
final class MsHwAudioFocusListener implements AudioManager.OnAudioFocusChangeListener {
    private final MsHwPlayerActivity act;
    MsHwAudioFocusListener(MsHwPlayerActivity act) {
        this.act = act;
    }
    @Override
    public void onAudioFocusChange(int change) {
        act.onSystemAudioFocus(change);
    }
}

/** 同文件顶层类（非内部类，便于 d8）。adb 调试广播，Activity 在前台时有效。 */
final class MsHwDebugCmdReceiver extends BroadcastReceiver {
    private final MsHwPlayerActivity act;
    MsHwDebugCmdReceiver(MsHwPlayerActivity act) {
        this.act = act;
    }
    @Override
    public void onReceive(Context context, Intent intent) {
        if (act != null) {
            act.applyDebugCmd(intent);
        }
    }

}

/** 同文件顶层类（非内部类，便于 d8）。双击暂停/继续，左右滑 seek ±10s。 */
final class MsHwDemoGestureListener extends GestureDetector.SimpleOnGestureListener {
    private final MsHwPlayerActivity act;
    MsHwDemoGestureListener(MsHwPlayerActivity act) {
        this.act = act;
    }
    @Override
    public boolean onDown(MotionEvent e) {
        return true;
    }
    @Override
    public boolean onDoubleTap(MotionEvent e) {
        act.onDemoDoubleTap();
        return true;
    }
    @Override
    public boolean onFling(MotionEvent e1, MotionEvent e2, float vx, float vy) {
        return act.onDemoFling(e1, e2);
    }
}

/** 同文件顶层类（非内部类，便于 d8）。把触摸交给手势检测。 */
final class MsHwDemoTouchListener implements View.OnTouchListener {
    private final MsHwPlayerActivity act;
    MsHwDemoTouchListener(MsHwPlayerActivity act) {
        this.act = act;
    }
    @Override
    public boolean onTouch(View v, MotionEvent event) {
        return act.forwardDemoTouch(event);
    }
}
