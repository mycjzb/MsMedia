package com.mycjzb.media.m3demo;

import android.app.Activity;
import android.graphics.Color;
import android.net.Uri;
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

import androidx.media3.common.PlaybackException;
import androidx.media3.common.Player;
import androidx.media3.exoplayer.DefaultLoadControl;
import androidx.media3.exoplayer.DefaultRenderersFactory;
import androidx.media3.exoplayer.ExoPlayer;
import androidx.media3.exoplayer.trackselection.DefaultTrackSelector;

import java.io.BufferedReader;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.util.ArrayList;

/**
 * Media3 源码直编演示：默认 ExoPlayer；左右切 System。
 * 上/下换台。首帧 = STATE_READY 且有首帧渲染回调 / 或 isPlaying 后 VIDEO 尺寸。
 */
public final class M3SpeedActivity extends Activity implements SurfaceHolder.Callback {
    private static final String TAG = "M3Speed";
    private static final int FIRST_FRAME_TIMEOUT_MS = 15000;

    private final Handler main = new Handler(Looper.getMainLooper());
    private SurfaceView surface;
    private SurfaceHolder holder;
    private TextView overlay;
    private String[] urls = new String[0];
    private String[] names = new String[0];
    private int index;
    private boolean useMedia3 = true;
    private int session;
    private long tCreate, tSetDs, tPrepared, tFirst;
    private String lastErr = "";
    private ExoPlayer exo;
    private android.media.MediaPlayer sys;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        FrameLayout root = new FrameLayout(this);
        surface = new SurfaceView(this);
        root.addView(surface, new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        overlay = new TextView(this);
        overlay.setTextColor(Color.WHITE);
        overlay.setBackgroundColor(0x99000000);
        overlay.setTextSize(15);
        overlay.setPadding(12, 8, 12, 8);
        root.addView(overlay, new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT,
                Gravity.TOP | Gravity.START));
        LinearLayout bar = new LinearLayout(this);
        bar.setOrientation(LinearLayout.HORIZONTAL);
        bar.addView(btn("上一个", new View.OnClickListener() {
            public void onClick(View v) { step(-1); }
        }));
        bar.addView(btn("下一个", new View.OnClickListener() {
            public void onClick(View v) { step(1); }
        }));
        bar.addView(btn("切换播放器", new View.OnClickListener() {
            public void onClick(View v) { toggle(); }
        }));
        root.addView(bar, new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT,
                Gravity.BOTTOM | Gravity.CENTER_HORIZONTAL));
        setContentView(root);
        surface.getHolder().addCallback(this);
        loadChannels();
        String u = getIntent() != null ? getIntent().getStringExtra("urls") : null;
        if (u != null && u.length() > 0) {
            urls = u.split("\\|");
            names = new String[0];
        }
        String p = getIntent() != null ? getIntent().getStringExtra("player") : null;
        if (p != null) {
            useMedia3 = !"sys".equals(p);
        }
        refresh();
    }

    private Button btn(String t, View.OnClickListener l) {
        Button b = new Button(this);
        b.setText(t);
        b.setOnClickListener(l);
        b.setFocusable(false);
        return b;
    }

    private void loadChannels() {
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
                n.add(line.substring(0, c).trim());
                u.add(line.substring(c + 1).trim());
            }
            br.close();
            if (!u.isEmpty()) {
                urls = u.toArray(new String[u.size()]);
                names = n.toArray(new String[n.size()]);
            }
        } catch (Throwable t) {
            Log.w(TAG, "channels: " + t.getMessage());
        } finally {
            if (in != null) try { in.close(); } catch (Throwable ignored) {}
        }
    }

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
                    toggle();
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
        startPlay();
    }

    private void toggle() {
        useMedia3 = !useMedia3;
        startPlay();
    }

    private void releaseAll() {
        if (exo != null) {
            try { exo.release(); } catch (Throwable ignored) {}
            exo = null;
        }
        if (sys != null) {
            try { sys.release(); } catch (Throwable ignored) {}
            sys = null;
        }
    }

    private void startPlay() {
        releaseAll();
        final int my = ++session;
        tSetDs = tPrepared = tFirst = 0;
        lastErr = "";
        if (holder == null || urls.length == 0) {
            refresh();
            return;
        }
        tCreate = SystemClock.elapsedRealtime();
        final String url = urls[index];
        if (useMedia3) {
            startMedia3(my, url);
        } else {
            startSys(my, url);
        }
        refresh();
    }

    private void startMedia3(final int my, String url) {
        DefaultLoadControl load = new DefaultLoadControl.Builder()
                .setBufferDurationsMs(500, 5000, 250, 500)
                .build();
        DefaultRenderersFactory rf = new DefaultRenderersFactory(this);
        rf.setExtensionRendererMode(DefaultRenderersFactory.EXTENSION_RENDERER_MODE_ON)
                .setEnableDecoderFallback(true);
        exo = new ExoPlayer.Builder(this)
                .setRenderersFactory(rf)
                .setTrackSelector(new DefaultTrackSelector(this))
                .setLoadControl(load)
                .build();
        exo.setVideoSurfaceHolder(holder);
        tSetDs = SystemClock.elapsedRealtime();
        exo.setMediaSource(M3MediaSources.create(url));
        exo.addListener(new Player.Listener() {
            @Override
            public void onPlaybackStateChanged(int state) {
                if (my != session) return;
                if (state == Player.STATE_READY && tPrepared == 0) {
                    tPrepared = SystemClock.elapsedRealtime();
                    refresh();
                }
            }

            @Override
            public void onRenderedFirstFrame() {
                if (my != session || tFirst != 0) return;
                tFirst = SystemClock.elapsedRealtime();
                refresh();
                record("OK");
            }

            @Override
            public void onPlayerError(PlaybackException error) {
                if (my != session) return;
                lastErr = "error " + error.errorCode + " " + error.getErrorCodeName();
                refresh();
                record(lastErr.replace(' ', '_'));
            }
        });
        exo.setPlayWhenReady(true);
        exo.prepare();
        main.postDelayed(new Runnable() {
            public void run() {
                if (my != session || tFirst != 0) return;
                lastErr = "first frame timeout " + FIRST_FRAME_TIMEOUT_MS + "ms";
                refresh();
                record(lastErr.replace(' ', '_'));
            }
        }, FIRST_FRAME_TIMEOUT_MS);
    }

    private void startSys(final int my, String url) {
        sys = new android.media.MediaPlayer();
        sys.setDisplay(holder);
        try {
            sys.setDataSource(this, Uri.parse(url));
            tSetDs = SystemClock.elapsedRealtime();
        } catch (Throwable t) {
            lastErr = "setDataSource " + t.getClass().getSimpleName();
            record(lastErr.replace(' ', '_'));
            refresh();
            return;
        }
        sys.setOnPreparedListener(new android.media.MediaPlayer.OnPreparedListener() {
            public void onPrepared(android.media.MediaPlayer mp) {
                if (my != session) return;
                tPrepared = SystemClock.elapsedRealtime();
                mp.start();
                refresh();
            }
        });
        sys.setOnInfoListener(new android.media.MediaPlayer.OnInfoListener() {
            public boolean onInfo(android.media.MediaPlayer mp, int what, int extra) {
                if (what == android.media.MediaPlayer.MEDIA_INFO_VIDEO_RENDERING_START
                        && my == session && tFirst == 0) {
                    tFirst = SystemClock.elapsedRealtime();
                    refresh();
                    record("OK");
                }
                return false;
            }
        });
        sys.setOnErrorListener(new android.media.MediaPlayer.OnErrorListener() {
            public boolean onError(android.media.MediaPlayer mp, int what, int extra) {
                if (my != session) return true;
                lastErr = "error what=" + what + " extra=" + extra;
                refresh();
                record(lastErr.replace(' ', '_'));
                return true;
            }
        });
        sys.prepareAsync();
        main.postDelayed(new Runnable() {
            public void run() {
                if (my != session || tFirst != 0) return;
                lastErr = "first frame timeout " + FIRST_FRAME_TIMEOUT_MS + "ms";
                refresh();
                record(lastErr.replace(' ', '_'));
            }
        }, FIRST_FRAME_TIMEOUT_MS);
    }

    private void record(String status) {
        String line = String.format(java.util.Locale.US,
                "MSBENCH player=%s idx=%d/%d url=%s status=%s create_to_setds_ms=%d to_prepared_ms=%d to_first_frame_ms=%d",
                useMedia3 ? "Media3-src" : "System", index + 1, urls.length,
                redact(urls[index]), status,
                tSetDs > 0 ? tSetDs - tCreate : -1,
                tPrepared > 0 ? tPrepared - tCreate : -1,
                tFirst > 0 ? tFirst - tCreate : -1);
        Log.i(TAG, line);
    }

    static String redact(String u) {
        int q = u.indexOf('?');
        if (q >= 0) u = u.substring(0, q);
        int s = u.indexOf("://");
        String name = u.substring(u.lastIndexOf('/') + 1);
        if (s < 0) return name;
        int h = u.indexOf('/', s + 3);
        String host = h < 0 ? u.substring(s + 3) : u.substring(s + 3, h);
        return u.substring(0, s + 3) + host + "/…/" + name;
    }

    private void refresh() {
        StringBuilder sb = new StringBuilder();
        sb.append("播放器: ").append(useMedia3 ? "Media3(源码)" : "System")
                .append("    频道: ").append(index + 1).append('/').append(urls.length).append('\n');
        if (names.length > index) sb.append(names[index]).append('\n');
        if (urls.length > 0) sb.append(redact(urls[index])).append('\n');
        sb.append("create→setDataSource: ").append(tSetDs > 0 ? (tSetDs - tCreate) + " ms" : "-").append('\n');
        sb.append("→OnPrepared/READY: ").append(tPrepared > 0 ? (tPrepared - tCreate) + " ms" : "-").append('\n');
        sb.append("→首帧: ").append(tFirst > 0 ? (tFirst - tCreate) + " ms" : "-");
        if (lastErr.length() > 0) sb.append('\n').append(lastErr);
        sb.append("\n上/下:换台  左/右:切 Media3源码↔System");
        overlay.setText(sb.toString());
    }

    @Override
    public void surfaceCreated(SurfaceHolder h) {
        holder = h;
        startPlay();
    }

    @Override
    public void surfaceChanged(SurfaceHolder h, int format, int w, int ht) {}

    @Override
    public void surfaceDestroyed(SurfaceHolder h) {
        holder = null;
        releaseAll();
    }

    @Override
    protected void onDestroy() {
        releaseAll();
        super.onDestroy();
    }
}
