/*
 * 选轨 / 字幕接口自测（演示 App 的 auto=tracks 模式，只打 logcat，标签 MsSpeed，前缀 MSTRACK）。
 * 覆盖：非法状态抛 IllegalStateException、getTrackInfo、getSelectedTrack、切音轨、
 * 选带内文字字幕收 onTimedText、addTimedTextSource（prepare 前添加）选外挂字幕、deselect 收 null、
 * 越界/视频/取消音轨抛 IllegalArgumentException、多轮 release 不崩溃。
 * 用法：adb shell am start -n com.mycjzb.media.single.slimff/com.mycjzb.media.player.demo.MsSpeedDemoActivity \
 *        --es auto tracks --es urls /sdcard/.../tracks.mkv --es sub /sdcard/.../ext.srt --ei repeat 3
 * 不是真机出声/上屏验收。by: Long <mycjzb@gmail.com>
 */
package com.mycjzb.media.player.demo;

import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.view.SurfaceHolder;

import com.mycjzb.media.player.MediaPlayer;
import com.mycjzb.media.player.TimedText;

final class MsTrackSelfTest {
    private static final String TAG = "MsSpeed";
    private final Handler main = new Handler(Looper.getMainLooper());
    private final SurfaceHolder holder;
    private final String url;
    private final String sub;
    private final int rounds;
    private int round;
    private int pass;
    private int fail;
    private MediaPlayer mp;
    /* 本轮收到的字幕回调 */
    private int ttText;
    private int ttNull;
    private String ttLast;
    private boolean sawInband;
    private boolean sawExternal;

    MsTrackSelfTest(SurfaceHolder holder, String url, String sub, int rounds) {
        this.holder = holder;
        this.url = url;
        this.sub = sub;
        this.rounds = Math.max(1, rounds);
    }

    void start() {
        nextRound();
    }

    private void ck(String name, boolean ok) {
        if (ok) pass++; else fail++;
        Log.i(TAG, "MSTRACK " + (ok ? "PASS " : "FAIL ") + "r" + round + " " + name);
    }

    private interface Body { void run() throws Exception; }

    private boolean throwsIse(Body b) {
        try { b.run(); return false; } catch (IllegalStateException e) { return true; } catch (Exception e) { return false; }
    }

    private boolean throwsIae(Body b) {
        try { b.run(); return false; } catch (IllegalArgumentException e) { return true; } catch (Exception e) { return false; }
    }

    private void later(int ms, Runnable r) {
        main.postDelayed(r, ms);
    }

    private void nextRound() {
        if (round >= rounds) {
            Log.i(TAG, "MSTRACK_DONE rounds=" + rounds + " pass=" + pass + " fail=" + fail);
            return;
        }
        round++;
        ttText = 0;
        ttNull = 0;
        ttLast = null;
        sawInband = false;
        sawExternal = false;
        mp = new MediaPlayer();
        final MediaPlayer m = mp;
        ck("idle getTrackInfo ISE", throwsIse(new Body() { public void run() { m.getTrackInfo(); } }));
        ck("idle selectTrack ISE", throwsIse(new Body() { public void run() { m.selectTrack(0); } }));
        ck("idle addTimedTextSource ISE", throwsIse(new Body() { public void run() throws Exception {
            m.addTimedTextSource("/nonexistent.srt", MediaPlayer.MEDIA_MIMETYPE_TEXT_SUBRIP); } }));
        try {
            m.setDisplay(holder);
            m.setDataSource(url);
        } catch (Exception e) {
            ck("setDataSource " + e, false);
            finishRound();
            return;
        }
        ck("initialized getSelectedTrack ISE", throwsIse(new Body() { public void run() { m.getSelectedTrack(MediaPlayer.TrackInfo.MEDIA_TRACK_TYPE_AUDIO); } }));
        ck("bad mime IAE", throwsIae(new Body() { public void run() throws Exception {
            m.addTimedTextSource("/x.txt", "text/plain"); } }));
        if (sub != null) {
            try {
                m.addTimedTextSource(sub, MediaPlayer.MEDIA_MIMETYPE_TEXT_SUBRIP); /* prepare 前添加 */
                ck("addTimedTextSource before prepare", true);
            } catch (Exception e) {
                ck("addTimedTextSource before prepare " + e, false);
            }
        }
        m.setOnTimedTextListener(new MediaPlayer.OnTimedTextListener() {
            public void onTimedText(MediaPlayer p, TimedText t) {
                ck("onTimedText on main thread", Looper.myLooper() == Looper.getMainLooper());
                if (t == null) {
                    ttNull++;
                    Log.i(TAG, "MSTT null");
                    return;
                }
                ttText++;
                ttLast = t.getText();
                if (ttLast != null && ttLast.startsWith("IN")) sawInband = true;
                if (ttLast != null && ttLast.startsWith("EXT")) sawExternal = true;
                Log.i(TAG, "MSTT " + t + " bounds=" + t.getBounds());
            }
        });
        m.setOnErrorListener(new MediaPlayer.OnErrorListener() {
            public boolean onError(MediaPlayer p, int what, int extra) {
                ck("no onError (" + what + "," + extra + ")", false);
                return true;
            }
        });
        m.setOnPreparedListener(new MediaPlayer.OnPreparedListener() {
            public void onPrepared(MediaPlayer p) {
                p.start();
                later(1500, new Runnable() { public void run() { stepTracks(); } });
            }
        });
        m.prepareAsync();
    }

    private int find(MediaPlayer.TrackInfo[] t, int type, int skip) {
        for (int i = 0; i < t.length; i++) {
            if (t[i].getTrackType() == type && i != skip) return i;
        }
        return -1;
    }

    private int findName(MediaPlayer.TrackInfo[] t, String name) {
        for (int i = 0; i < t.length; i++) {
            if (name.equals(t[i].getName())) return i;
        }
        return -1;
    }

    private void stepTracks() {
        final MediaPlayer m = mp;
        final MediaPlayer.TrackInfo[] t = m.getTrackInfo();
        for (int i = 0; i < t.length; i++) {
            Log.i(TAG, "MSTRACK track[" + i + "] " + t[i] + " name=" + t[i].getName() + " fmt=" + t[i].getFormat());
        }
        int nA = 0, nT = 0;
        for (MediaPlayer.TrackInfo x : t) {
            if (x.getTrackType() == MediaPlayer.TrackInfo.MEDIA_TRACK_TYPE_AUDIO) nA++;
            if (x.getTrackType() == MediaPlayer.TrackInfo.MEDIA_TRACK_TYPE_TIMEDTEXT) nT++;
        }
        ck("tracks >=1 video", find(t, MediaPlayer.TrackInfo.MEDIA_TRACK_TYPE_VIDEO, -1) >= 0);
        ck("tracks 2 audio (got " + nA + ")", nA >= 2);
        ck("tracks timedtext >= " + (sub != null ? 2 : 1) + " (got " + nT + ")", nT >= (sub != null ? 2 : 1));
        final int a0 = m.getSelectedTrack(MediaPlayer.TrackInfo.MEDIA_TRACK_TYPE_AUDIO);
        ck("audio selected by default (" + a0 + ")", a0 >= 0);
        ck("no timedtext selected by default", m.getSelectedTrack(MediaPlayer.TrackInfo.MEDIA_TRACK_TYPE_TIMEDTEXT) == -1);
        ck("select out of range IAE", throwsIae(new Body() { public void run() { m.selectTrack(t.length + 5); } }));
        final int v = find(t, MediaPlayer.TrackInfo.MEDIA_TRACK_TYPE_VIDEO, -1);
        if (v >= 0) ck("select video IAE", throwsIae(new Body() { public void run() { m.selectTrack(v); } }));
        if (a0 >= 0) ck("deselect audio IAE", throwsIae(new Body() { public void run() { m.deselectTrack(a0); } }));
        final int a1 = find(t, MediaPlayer.TrackInfo.MEDIA_TRACK_TYPE_AUDIO, a0);
        if (a1 >= 0) {
            try {
                m.selectTrack(a1);
            } catch (Exception e) {
                ck("select audio " + e, false);
            }
        }
        later(1200, new Runnable() { public void run() {
            int now = m.getSelectedTrack(MediaPlayer.TrackInfo.MEDIA_TRACK_TYPE_AUDIO);
            ck("audio switched " + a0 + "->" + now, a1 >= 0 && now == a1);
            ck("still playing after audio switch", m.isPlaying());
            MediaPlayer.TrackInfo[] t2 = m.getTrackInfo();
            int in = -1;
            for (int i = 0; i < t2.length; i++) {
                if (t2[i].getTrackType() == MediaPlayer.TrackInfo.MEDIA_TRACK_TYPE_TIMEDTEXT && !"external".equals(t2[i].getName())) { in = i; break; }
            }
            if (in >= 0) {
                m.selectTrack(in);
                ck("timedtext selected " + in, m.getSelectedTrack(MediaPlayer.TrackInfo.MEDIA_TRACK_TYPE_TIMEDTEXT) == in);
            } else {
                ck("inband timedtext present", false);
            }
            later(5000, new Runnable() { public void run() { stepExternal(); } });
        } });
    }

    private void stepExternal() {
        final MediaPlayer m = mp;
        Log.i(TAG, "MSTRACK pos=" + m.getCurrentPosition() + "ms playing=" + m.isPlaying());
        ck("inband onTimedText received (n=" + ttText + " last=" + ttLast + ")", sawInband);
        MediaPlayer.TrackInfo[] t = m.getTrackInfo();
        int ext = findName(t, "external");
        if (sub != null) {
            ck("external track listed", ext >= 0);
            if (ext >= 0) m.selectTrack(ext);
            later(5000, new Runnable() { public void run() { stepDeselect(); } });
        } else {
            stepDeselect();
        }
    }

    private void stepDeselect() {
        final MediaPlayer m = mp;
        if (sub != null) ck("external onTimedText received (last=" + ttLast + ")", sawExternal);
        final int sel = m.getSelectedTrack(MediaPlayer.TrackInfo.MEDIA_TRACK_TYPE_TIMEDTEXT);
        final int nullsBefore = ttNull;
        if (sel >= 0) m.deselectTrack(sel);
        ck("timedtext deselected", m.getSelectedTrack(MediaPlayer.TrackInfo.MEDIA_TRACK_TYPE_TIMEDTEXT) == -1);
        later(1500, new Runnable() { public void run() {
            ck("null after deselect (shown=" + (ttLast != null) + ")", ttNull > nullsBefore || ttText == 0);
            finishRound();
        } });
    }

    private void finishRound() {
        try {
            if (mp != null) mp.release();
        } catch (Throwable e) {
            ck("release " + e, false);
        }
        mp = null;
        later(500, new Runnable() { public void run() { nextRound(); } });
    }
}
