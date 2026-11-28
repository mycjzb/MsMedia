package com.mycjzb.media.msm3;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.DialogInterface;
import android.graphics.Color;
import android.net.TrafficStats;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.Process;
import android.os.SystemClock;
import android.util.Log;
import android.view.Gravity;
import android.view.KeyEvent;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.ViewGroup;
import android.widget.FrameLayout;
import android.widget.TextView;

import com.mycjzb.media.player.MediaPlayer;
import com.mycjzb.media.player.PlayEngine;
import com.mycjzb.media.player.SubtitleBitmapOverlay;
import com.mycjzb.media.player.TimedText;

import java.io.BufferedReader;
import java.io.InputStreamReader;
import java.util.ArrayList;
import java.util.Locale;

/**
 * 演示 APK：只通过 SDK {@link MediaPlayer} 播控（{@code shw:} = Media3 内核）。
 * 频道只读包内 {@code assets/channels.txt}；换台抄超级直播（频道键/方向键），上下反转。
 * 左上角：多音轨/字幕角标；菜单键弹窗选轨（对齐超级直播习惯）。
 */
public final class PlayActivity extends Activity implements SurfaceHolder.Callback {
    private static final String TAG = "MsM3";
    private static final int FIRST_TIMEOUT_MS = 20000;
    private static final int HUD_MS = 500;
    /** 对齐超级直播 {@code Cfg.zapInvert}：demo 固定反转上下换台。 */
    private static final boolean ZAP_INVERT = true;

    private final Handler main = new Handler(Looper.getMainLooper());
    private SurfaceView surface;
    private SurfaceHolder holder;
    private TextView trackBadge;
    private TextView overlay;
    private TextView subText;
    private SubtitleBitmapOverlay subBmp;
    private AlertDialog trackDialog;
    private String[] urls = new String[0];
    private String[] names = new String[0];
    private int index;
    private int session;
    private long tCreate, tSetDs, tReady, tFirst;
    private String lastErr = "";
    private MediaPlayer player;

    private long netLastRx = -1;
    private long netLastAt;
    private long netBps;
    private final int[] bars = new int[5];
    private String cueLine = "";

    private final Runnable hudTick = new Runnable() {
        @Override
        public void run() {
            sampleNet();
            if (player != null) {
                try {
                    player.getAudioBars(bars);
                } catch (Throwable ignored) {
                }
            }
            refresh();
            main.postDelayed(this, HUD_MS);
        }
    };

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        MediaPlayer.setDefaultEngine(PlayEngine.SHW);
        FrameLayout root = new FrameLayout(this);
        surface = new SurfaceView(this);
        root.addView(surface, new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        trackBadge = new TextView(this);
        trackBadge.setTextColor(Color.WHITE);
        trackBadge.setBackgroundColor(0xCC1565C0);
        trackBadge.setTextSize(16);
        trackBadge.setPadding(14, 10, 14, 10);
        trackBadge.setVisibility(View.GONE);
        root.addView(trackBadge, new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT,
                Gravity.TOP | Gravity.START));
        subBmp = new SubtitleBitmapOverlay(this);
        root.addView(subBmp, new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        subText = new TextView(this);
        subText.setTextColor(Color.WHITE);
        subText.setBackgroundColor(0x66000000);
        subText.setTextSize(18);
        subText.setGravity(Gravity.CENTER_HORIZONTAL);
        subText.setPadding(24, 8, 24, 12);
        subText.setVisibility(View.GONE);
        FrameLayout.LayoutParams stp = new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT,
                Gravity.BOTTOM | Gravity.CENTER_HORIZONTAL);
        stp.setMargins(48, 0, 48, 48);
        root.addView(subText, stp);
        overlay = new TextView(this);
        overlay.setTextColor(Color.WHITE);
        overlay.setBackgroundColor(0x99000000);
        overlay.setTextSize(14);
        overlay.setPadding(12, 8, 12, 8);
        FrameLayout.LayoutParams olp = new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT,
                Gravity.TOP | Gravity.END);
        olp.setMargins(0, 8, 8, 0);
        root.addView(overlay, olp);
        setContentView(root);
        surface.getHolder().addCallback(this);
        loadChannels();
        if (getIntent() != null) {
            String u = getIntent().getStringExtra("urls");
            if (u != null && u.length() > 0) {
                urls = u.split("\\|");
                names = new String[0];
            }
        }
        refresh();
        main.post(hudTick);
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
        releasePlayer();
    }

    @Override
    protected void onDestroy() {
        main.removeCallbacks(hudTick);
        dismissTrackDialog();
        releasePlayer();
        super.onDestroy();
    }

    @Override
    public boolean onKeyDown(int keyCode, KeyEvent event) {
        /* 对齐超级直播 PlayerActivity：频道键与方向键默认方向相反，再经 zapInvert */
        switch (keyCode) {
            case KeyEvent.KEYCODE_CHANNEL_UP:
                step(zapDelta(1));
                return true;
            case KeyEvent.KEYCODE_CHANNEL_DOWN:
                step(zapDelta(-1));
                return true;
            case KeyEvent.KEYCODE_DPAD_UP:
                step(zapDelta(-1));
                return true;
            case KeyEvent.KEYCODE_DPAD_DOWN:
                step(zapDelta(1));
                return true;
            case KeyEvent.KEYCODE_MENU:
            case KeyEvent.KEYCODE_INFO:
                showTrackRootMenu();
                return true;
            case KeyEvent.KEYCODE_PROG_RED:
            case KeyEvent.KEYCODE_MEDIA_AUDIO_TRACK:
                nextAudio();
                return true;
            case KeyEvent.KEYCODE_PROG_GREEN:
                nextSubtitle();
                return true;
            default:
                return super.onKeyDown(keyCode, event);
        }
    }

    private void dismissTrackDialog() {
        if (trackDialog != null) {
            try {
                trackDialog.dismiss();
            } catch (Throwable ignored) {
            }
            trackDialog = null;
        }
    }

    /** 菜单 / INFO：先选音轨或字幕，再弹列表（对齐超级直播）。 */
    private void showTrackRootMenu() {
        if (player == null) {
            return;
        }
        dismissTrackDialog();
        int nA = countTracks(MediaPlayer.TrackInfo.MEDIA_TRACK_TYPE_AUDIO);
        int nS = countTracks(MediaPlayer.TrackInfo.MEDIA_TRACK_TYPE_TIMEDTEXT);
        final ArrayList<String> items = new ArrayList<String>();
        final ArrayList<Integer> actions = new ArrayList<Integer>();
        if (nA > 0) {
            items.add("音轨…（" + audioOrdinal() + "/" + nA + "）");
            actions.add(0);
        }
        if (nS > 0) {
            items.add("字幕…（" + subOrdinalLabel() + "/" + nS + "）");
            actions.add(1);
        }
        if (items.isEmpty()) {
            return;
        }
        AlertDialog.Builder b = new AlertDialog.Builder(this);
        b.setTitle("音轨 / 字幕");
        b.setItems(items.toArray(new CharSequence[items.size()]), new DialogInterface.OnClickListener() {
            @Override
            public void onClick(DialogInterface dialog, int which) {
                if (which < 0 || which >= actions.size()) {
                    return;
                }
                if (actions.get(which) == 0) {
                    showAudioPicker();
                } else {
                    showSubtitlePicker();
                }
            }
        });
        b.setNegativeButton("取消", null);
        trackDialog = b.create();
        trackDialog.show();
    }

    private void showAudioPicker() {
        if (player == null) {
            return;
        }
        dismissTrackDialog();
        final MediaPlayer.TrackInfo[] all = safeTracks();
        final ArrayList<Integer> idx = new ArrayList<Integer>();
        final ArrayList<String> labels = new ArrayList<String>();
        int checked = -1;
        int ord = 0;
        int sel = -1;
        try {
            sel = player.getSelectedTrack(MediaPlayer.TrackInfo.MEDIA_TRACK_TYPE_AUDIO);
        } catch (Throwable ignored) {
        }
        for (int i = 0; i < all.length; i++) {
            if (all[i].getTrackType() != MediaPlayer.TrackInfo.MEDIA_TRACK_TYPE_AUDIO) {
                continue;
            }
            ord++;
            idx.add(i);
            labels.add(trackLabel(false, ord, all[i]));
            if (i == sel) {
                checked = labels.size() - 1;
            }
        }
        if (labels.isEmpty()) {
            return;
        }
        AlertDialog.Builder b = new AlertDialog.Builder(this);
        b.setTitle("音轨选择");
        b.setSingleChoiceItems(labels.toArray(new CharSequence[labels.size()]), checked,
                new DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(DialogInterface dialog, int which) {
                        try {
                            player.selectTrack(idx.get(which));
                        } catch (Throwable ignored) {
                        }
                        dialog.dismiss();
                        refresh();
                    }
                });
        b.setNegativeButton("取消", null);
        trackDialog = b.create();
        trackDialog.show();
    }

    private void showSubtitlePicker() {
        if (player == null) {
            return;
        }
        dismissTrackDialog();
        final MediaPlayer.TrackInfo[] all = safeTracks();
        final ArrayList<Integer> idx = new ArrayList<Integer>();
        final ArrayList<String> labels = new ArrayList<String>();
        labels.add("关闭字幕");
        idx.add(-1);
        int checked = 0;
        int ord = 0;
        int sel = -1;
        try {
            sel = player.getSelectedTrack(MediaPlayer.TrackInfo.MEDIA_TRACK_TYPE_TIMEDTEXT);
        } catch (Throwable ignored) {
        }
        for (int i = 0; i < all.length; i++) {
            if (all[i].getTrackType() != MediaPlayer.TrackInfo.MEDIA_TRACK_TYPE_TIMEDTEXT
                    && all[i].getTrackType() != MediaPlayer.TrackInfo.MEDIA_TRACK_TYPE_SUBTITLE) {
                continue;
            }
            ord++;
            idx.add(i);
            labels.add(trackLabel(true, ord, all[i]));
            if (i == sel) {
                checked = labels.size() - 1;
            }
        }
        if (ord == 0) {
            return;
        }
        AlertDialog.Builder b = new AlertDialog.Builder(this);
        b.setTitle("字幕选择");
        b.setSingleChoiceItems(labels.toArray(new CharSequence[labels.size()]), checked,
                new DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(DialogInterface dialog, int which) {
                        try {
                            int flat = idx.get(which);
                            if (flat < 0) {
                                int cur = player.getSelectedTrack(
                                        MediaPlayer.TrackInfo.MEDIA_TRACK_TYPE_TIMEDTEXT);
                                if (cur < 0) {
                                    cur = player.getSelectedTrack(
                                            MediaPlayer.TrackInfo.MEDIA_TRACK_TYPE_SUBTITLE);
                                }
                                if (cur >= 0) {
                                    player.deselectTrack(cur);
                                }
                            } else {
                                player.selectTrack(flat);
                            }
                        } catch (Throwable ignored) {
                        }
                        dialog.dismiss();
                        refresh();
                    }
                });
        b.setNegativeButton("取消", null);
        trackDialog = b.create();
        trackDialog.show();
    }

    private static String trackLabel(boolean sub, int ordinal, MediaPlayer.TrackInfo t) {
        String head = sub ? ("字幕" + ordinal) : ("音轨" + ordinal);
        String lang = t.getLanguage();
        if (lang != null && lang.length() > 0 && !"und".equalsIgnoreCase(lang)) {
            head = head + "（" + lang + "）";
        } else if (t.getName() != null && t.getName().length() > 0) {
            head = head + "（" + t.getName() + "）";
        }
        return head;
    }

    /** 红键 / 音轨键：环形切下一条 AUDIO。 */
    private void nextAudio() {
        cycleTrack(MediaPlayer.TrackInfo.MEDIA_TRACK_TYPE_AUDIO);
    }

    /** 绿键：环形切字幕；切完一圈关闭。 */
    private void nextSubtitle() {
        if (player == null) {
            return;
        }
        try {
            MediaPlayer.TrackInfo[] t = player.getTrackInfo();
            int cur = player.getSelectedTrack(MediaPlayer.TrackInfo.MEDIA_TRACK_TYPE_TIMEDTEXT);
            if (cur < 0) {
                cur = player.getSelectedTrack(MediaPlayer.TrackInfo.MEDIA_TRACK_TYPE_SUBTITLE);
            }
            int first = -1;
            int next = -1;
            for (int i = 0; i < t.length; i++) {
                int ty = t[i].getTrackType();
                if (ty != MediaPlayer.TrackInfo.MEDIA_TRACK_TYPE_TIMEDTEXT
                        && ty != MediaPlayer.TrackInfo.MEDIA_TRACK_TYPE_SUBTITLE) {
                    continue;
                }
                if (first < 0) {
                    first = i;
                }
                if (i > cur) {
                    next = i;
                    break;
                }
            }
            if (cur >= 0 && next < 0) {
                player.deselectTrack(cur); /* 一圈后关字幕 */
            } else if (next >= 0) {
                player.selectTrack(next);
            } else if (first >= 0) {
                player.selectTrack(first);
            }
        } catch (Throwable ignored) {
        }
        refresh();
    }

    private void cycleTrack(int type) {
        if (player == null) {
            return;
        }
        try {
            MediaPlayer.TrackInfo[] t = player.getTrackInfo();
            int cur = player.getSelectedTrack(type);
            int first = -1;
            int next = -1;
            for (int i = 0; i < t.length; i++) {
                if (t[i].getTrackType() != type) {
                    continue;
                }
                if (first < 0) {
                    first = i;
                }
                if (i > cur) {
                    next = i;
                    break;
                }
            }
            if (next < 0) {
                next = first;
            }
            if (next >= 0) {
                player.selectTrack(next);
            }
        } catch (Throwable ignored) {
        }
        refresh();
    }

    private MediaPlayer.TrackInfo[] safeTracks() {
        try {
            return player != null ? player.getTrackInfo() : new MediaPlayer.TrackInfo[0];
        } catch (Throwable t) {
            return new MediaPlayer.TrackInfo[0];
        }
    }

    private int countTracks(int type) {
        MediaPlayer.TrackInfo[] t = safeTracks();
        int n = 0;
        for (int i = 0; i < t.length; i++) {
            int ty = t[i].getTrackType();
            if (ty == type) {
                n++;
            } else if (type == MediaPlayer.TrackInfo.MEDIA_TRACK_TYPE_TIMEDTEXT
                    && ty == MediaPlayer.TrackInfo.MEDIA_TRACK_TYPE_SUBTITLE) {
                n++;
            }
        }
        return n;
    }

    /** 当前音轨在同类中的 1-based 序号；无选中为 0。 */
    private int audioOrdinal() {
        return ordinalOf(MediaPlayer.TrackInfo.MEDIA_TRACK_TYPE_AUDIO);
    }

    private String subOrdinalLabel() {
        int o = ordinalOf(MediaPlayer.TrackInfo.MEDIA_TRACK_TYPE_TIMEDTEXT);
        if (o <= 0) {
            o = ordinalOf(MediaPlayer.TrackInfo.MEDIA_TRACK_TYPE_SUBTITLE);
        }
        return o > 0 ? String.valueOf(o) : "-";
    }

    private int ordinalOf(int type) {
        if (player == null) {
            return 0;
        }
        try {
            int sel = player.getSelectedTrack(type);
            if (sel < 0) {
                return 0;
            }
            MediaPlayer.TrackInfo[] t = player.getTrackInfo();
            int ord = 0;
            for (int i = 0; i < t.length; i++) {
                int ty = t[i].getTrackType();
                boolean match = ty == type
                        || (type == MediaPlayer.TrackInfo.MEDIA_TRACK_TYPE_TIMEDTEXT
                        && ty == MediaPlayer.TrackInfo.MEDIA_TRACK_TYPE_SUBTITLE);
                if (!match) {
                    continue;
                }
                ord++;
                if (i == sel) {
                    return ord;
                }
            }
        } catch (Throwable ignored) {
        }
        return 0;
    }

    /** 左上角：有多音轨（>1）或有字幕轨时显示「音轨 1/3  字幕 1/2」。 */
    private void updateTrackBadge() {
        if (trackBadge == null) {
            return;
        }
        int nA = countTracks(MediaPlayer.TrackInfo.MEDIA_TRACK_TYPE_AUDIO);
        int nS = countTracks(MediaPlayer.TrackInfo.MEDIA_TRACK_TYPE_TIMEDTEXT);
        if (nA <= 1 && nS <= 0) {
            trackBadge.setVisibility(View.GONE);
            trackBadge.setText("");
            return;
        }
        StringBuilder sb = new StringBuilder();
        if (nA > 1) {
            int o = audioOrdinal();
            sb.append("音轨 ").append(o > 0 ? o : "-").append('/').append(nA);
        }
        if (nS > 0) {
            if (sb.length() > 0) {
                sb.append("   ");
            }
            sb.append("字幕 ").append(subOrdinalLabel()).append('/').append(nS);
        }
        trackBadge.setText(sb.toString());
        trackBadge.setVisibility(View.VISIBLE);
    }

    private static int zapDelta(int sign) {
        return ZAP_INVERT ? -sign : sign;
    }

    private void step(int d) {
        if (urls.length == 0 || d == 0) {
            return;
        }
        index = (index + d + urls.length) % urls.length;
        startPlay();
    }

    private void releasePlayer() {
        if (player != null) {
            try {
                player.release();
            } catch (Throwable ignored) {
            }
            player = null;
        }
    }

    private void applySubtitleCue(TimedText text) {
        if (text != null && text.hasBitmap()) {
            cueLine = "[bitmap]";
            if (subText != null) {
                subText.setVisibility(View.GONE);
                subText.setText("");
            }
            if (subBmp != null) {
                subBmp.showTimedText(text);
            }
        } else {
            if (subBmp != null) {
                subBmp.clearCue();
            }
            String s = text != null && text.getText() != null ? text.getText() : "";
            cueLine = s;
            if (subText != null) {
                if (s.length() > 0) {
                    subText.setText(s);
                    subText.setVisibility(View.VISIBLE);
                } else {
                    subText.setText("");
                    subText.setVisibility(View.GONE);
                }
            }
        }
        refresh();
    }

    private void clearSubtitleUi() {
        cueLine = "";
        if (subBmp != null) {
            subBmp.clearCue();
        }
        if (subText != null) {
            subText.setText("");
            subText.setVisibility(View.GONE);
        }
    }

    private void startPlay() {
        dismissTrackDialog();
        releasePlayer();
        clearSubtitleUi();
        final int my = ++session;
        tSetDs = tReady = tFirst = 0;
        lastErr = "";
        netLastRx = -1;
        netBps = 0;
        if (holder == null || urls.length == 0) {
            refresh();
            return;
        }
        tCreate = SystemClock.elapsedRealtime();
        player = new MediaPlayer();
        player.setDisplay(holder);
        player.setOnTimedTextListener(new MediaPlayer.OnTimedTextListener() {
            @Override
            public void onTimedText(MediaPlayer mp, TimedText text) {
                if (my != session) {
                    return;
                }
                applySubtitleCue(text);
            }
        });
        player.setOnPreparedListener(new MediaPlayer.OnPreparedListener() {
            @Override
            public void onPrepared(MediaPlayer mp) {
                if (my != session) {
                    return;
                }
                tReady = SystemClock.elapsedRealtime();
                refresh();
                try {
                    mp.start();
                } catch (Throwable t) {
                    lastErr = "start " + t.getClass().getSimpleName();
                    refresh();
                }
            }
        });
        player.setOnVideoSizeChangedListener(new MediaPlayer.OnVideoSizeChangedListener() {
            @Override
            public void onVideoSizeChanged(MediaPlayer mp, int width, int height) {
                if (my == session) {
                    refresh();
                }
            }
        });
        player.setOnInfoListener(new MediaPlayer.OnInfoListener() {
            @Override
            public boolean onInfo(MediaPlayer mp, int what, int extra) {
                if (my != session) {
                    return false;
                }
                if (what == MediaPlayer.MEDIA_INFO_VIDEO_RENDERING_START && tFirst == 0) {
                    tFirst = SystemClock.elapsedRealtime();
                    refresh();
                    record("OK");
                }
                return false;
            }
        });
        player.setOnErrorListener(new MediaPlayer.OnErrorListener() {
            @Override
            public boolean onError(MediaPlayer mp, int what, int extra) {
                if (my != session) {
                    return true;
                }
                lastErr = "err " + what + "/" + extra;
                refresh();
                record(lastErr.replace(' ', '_'));
                return true;
            }
        });
        tSetDs = SystemClock.elapsedRealtime();
        try {
            String url = urls[index];
            player.setDataSource(this, Uri.parse(url));
            player.prepareAsync();
        } catch (Throwable t) {
            lastErr = "source " + t.getClass().getSimpleName();
            record(lastErr.replace(' ', '_'));
            refresh();
            return;
        }
        refresh();
        main.postDelayed(new Runnable() {
            @Override
            public void run() {
                if (my != session || tFirst != 0 || lastErr.length() > 0) {
                    return;
                }
                lastErr = "timeout " + FIRST_TIMEOUT_MS + "ms";
                refresh();
                record("timeout");
            }
        }, FIRST_TIMEOUT_MS);
    }

    private void sampleNet() {
        long now = SystemClock.elapsedRealtime();
        long rx = TrafficStats.getUidRxBytes(Process.myUid());
        if (rx == TrafficStats.UNSUPPORTED) {
            netBps = -1;
            return;
        }
        if (netLastRx >= 0 && now > netLastAt) {
            long dt = now - netLastAt;
            long db = rx - netLastRx;
            if (dt > 0 && db >= 0) {
                netBps = db * 1000L / dt;
            }
        }
        netLastRx = rx;
        netLastAt = now;
    }

    private void refresh() {
        String name = (names.length == urls.length && index < names.length)
                ? names[index] : ("#" + (index + 1));
        String url = index < urls.length ? urls[index] : "";
        StringBuilder sb = new StringBuilder();
        sb.append("SDK MediaPlayer / ").append(PlayEngine.name(PlayEngine.SHW)).append('\n');
        sb.append(index + 1).append('/').append(urls.length).append('\n');
        sb.append(name).append('\n');
        sb.append(url).append('\n');
        if (tSetDs > 0) {
            sb.append(String.format(Locale.US, "setDS +%d ms\n", tSetDs - tCreate));
        }
        if (tReady > 0) {
            sb.append(String.format(Locale.US, "prepared +%d ms\n", tReady - tCreate));
        }
        if (tFirst > 0) {
            sb.append(String.format(Locale.US, "firstFrame +%d ms\n", tFirst - tCreate));
        }
        if (lastErr.length() > 0) {
            sb.append(lastErr).append('\n');
        }
        if (player != null) {
            try {
                int w = player.getVideoWidth();
                int h = player.getVideoHeight();
                float fps = player.getVideoFrameRate();
                if (w > 0 && h > 0) {
                    sb.append(w).append('x').append(h);
                    if (fps > 0f) {
                        sb.append(String.format(Locale.US, " @ %.2ffps", fps));
                    }
                    sb.append('\n');
                }
                String vm = shortCodec(player.getVideoMime());
                String am = shortCodec(player.getAudioMime());
                int vb = player.getVideoBitrate();
                int ab = player.getAudioBitrate();
                if (vm != null || am != null) {
                    sb.append("v:").append(vm != null ? vm : "-");
                    if (vb > 0) {
                        sb.append(' ').append(fmtRate(vb));
                    }
                    sb.append("  a:").append(am != null ? am : "-");
                    if (ab > 0) {
                        sb.append(' ').append(fmtRate(ab));
                    }
                    sb.append('\n');
                }
                sb.append(player.isLive() ? "live" : "vod");
                sb.append(" dur=").append(player.getDuration());
                sb.append('\n');
                MediaPlayer.TrackInfo[] tracks = player.getTrackInfo();
                int nA = 0;
                for (int i = 0; i < tracks.length; i++) {
                    if (tracks[i].getTrackType() == MediaPlayer.TrackInfo.MEDIA_TRACK_TYPE_AUDIO) {
                        nA++;
                    }
                }
                if (nA > 0) {
                    int sel = player.getSelectedTrack(MediaPlayer.TrackInfo.MEDIA_TRACK_TYPE_AUDIO);
                    sb.append("audio ").append(nA).append("tr sel=").append(sel).append('\n');
                }
                sb.append("bars ");
                for (int i = 0; i < bars.length; i++) {
                    if (i > 0) {
                        sb.append(' ');
                    }
                    sb.append(bars[i]);
                }
                sb.append('\n');
                if (cueLine.length() > 0) {
                    sb.append("sub ").append(cueLine).append('\n');
                }
            } catch (Throwable ignored) {
            }
        }
        if (netBps > 0) {
            sb.append("net ").append(fmtBytesPerSec(netBps)).append('\n');
        } else if (netBps < 0) {
            sb.append("net n/a\n");
        }
        overlay.setText(sb.toString());
        updateTrackBadge();
    }

    private static String shortCodec(String mime) {
        if (mime == null || mime.length() == 0) {
            return null;
        }
        String m = mime.toLowerCase(Locale.US);
        if (m.contains("avc") || m.equals("video/h264")) {
            return "h264";
        }
        if (m.contains("hevc") || m.contains("h265")) {
            return "hevc";
        }
        if (m.contains("av01") || m.contains("av1")) {
            return "av1";
        }
        if (m.contains("vp9")) {
            return "vp9";
        }
        if (m.contains("vp8")) {
            return "vp8";
        }
        if (m.contains("mp4a") || m.contains("aac")) {
            return "aac";
        }
        if (m.contains("ac3") || m.contains("eac3")) {
            return m.contains("eac3") ? "eac3" : "ac3";
        }
        /* Media3：audio/mpeg-L2 = mp2；audio/mpeg = mp3 */
        if (m.contains("mpeg-l2") || m.contains("mpeg_l2") || m.endsWith("mp2")) {
            return "mp2";
        }
        if (m.equals("audio/mpeg") || m.contains("mpeg-l3") || m.contains("mp3")) {
            return "mp3";
        }
        if (m.contains("mpeg") && m.contains("audio")) {
            return "mpa";
        }
        if (m.contains("opus")) {
            return "opus";
        }
        int slash = m.lastIndexOf('/');
        return slash >= 0 && slash + 1 < m.length() ? m.substring(slash + 1) : m;
    }

    private static String fmtRate(int bps) {
        if (bps >= 1_000_000) {
            return String.format(Locale.US, "%.1fMbps", bps / 1_000_000.0);
        }
        if (bps >= 1000) {
            return String.format(Locale.US, "%.0fkbps", bps / 1000.0);
        }
        return bps + "bps";
    }

    private static String fmtBytesPerSec(long bps) {
        if (bps >= 1_000_000) {
            return String.format(Locale.US, "%.2f MB/s", bps / 1_000_000.0);
        }
        if (bps >= 1000) {
            return String.format(Locale.US, "%.1f KB/s", bps / 1000.0);
        }
        return bps + " B/s";
    }

    private void record(String tag) {
        Log.i(TAG, tag + " idx=" + index
                + " ready=" + (tReady > 0 ? (tReady - tCreate) : -1)
                + " first=" + (tFirst > 0 ? (tFirst - tCreate) : -1));
    }

    private void loadChannels() {
        ArrayList<String> n = new ArrayList<String>();
        ArrayList<String> u = new ArrayList<String>();
        try {
            BufferedReader br = new BufferedReader(
                    new InputStreamReader(getAssets().open("channels.txt"), "UTF-8"));
            String line;
            while ((line = br.readLine()) != null) {
                line = line.trim();
                if (line.length() == 0 || line.startsWith("#")) {
                    continue;
                }
                int c = line.indexOf(',');
                if (c <= 0) {
                    continue;
                }
                n.add(line.substring(0, c).trim());
                u.add(line.substring(c + 1).trim());
            }
            br.close();
        } catch (Throwable t) {
            Log.w(TAG, "channels", t);
        }
        names = n.toArray(new String[0]);
        urls = u.toArray(new String[0]);
    }
}
