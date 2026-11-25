/*
 * 移植 androidx.media3.exoplayer.video.VideoFrameReleaseHelper.VSyncSampler /
 * VSyncSamplerBase（Choreographer + Display.getRefreshRate）。
 * 对照 androidx/media main@101e97c，见 docs/MEDIA3_SYNC_PORT.md。
 *
 * 原始版权：Copyright (C) The Android Open Source Project
 * 原始许可：Apache License 2.0
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * 移植注释与接线：by: Long <mycjzb@gmail.com>
 */
package com.mycjzb.media.player.media3;

import android.content.Context;
import android.hardware.display.DisplayManager;
import android.os.Build;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.Looper;
import android.util.Log;
import android.view.Choreographer;
import android.view.Display;
import android.view.Surface;

/**
 * 进程级 VSYNC 采样器。必须在带 Looper 的线程上创建 Choreographer（与 Media3 在 playback 线程上
 * 创建相同）。本类用独立 HandlerThread，避免 C 解码线程没有 Looper。
 * API 33+ 的 FrameTimeline 路径放在 {@link VSyncSamplerV33}，避免 API 18 加载时 VerifyError。
 */
public final class VSyncSampler {
    private static final String TAG = "MsVSync";
    /** VideoFrameReleaseHelper.VSYNC_SAMPLE_UPDATE_PERIOD_MS */
    public static final long VSYNC_SAMPLE_UPDATE_PERIOD_MS = 500;
    /** C.TIME_UNSET */
    public static final long TIME_UNSET = Long.MIN_VALUE + 1;
    public static final long NANOS_PER_SECOND = 1_000_000_000L;

    private static final Object LOCK = new Object();
    private static int refs;
    private static HandlerThread thread;
    private static Handler handler;
    private static volatile long sampledVsyncTimeNs = TIME_UNSET;
    private static volatile long vsyncDurationNs = TIME_UNSET;
    private static BaseSampler sampler;

    private VSyncSampler() {}

    public static void start() {
        synchronized (LOCK) {
            refs++;
            if (refs != 1) {
                return;
            }
            thread = new HandlerThread("MsVSync");
            thread.start();
            handler = new Handler(thread.getLooper());
            handler.post(new Runnable() {
                @Override public void run() {
                    startOnLooper();
                }
            });
        }
    }

    public static void stop() {
        synchronized (LOCK) {
            if (refs == 0) {
                return;
            }
            refs--;
            if (refs != 0) {
                return;
            }
            if (handler != null) {
                handler.post(new Runnable() {
                    @Override public void run() {
                        stopOnLooper();
                    }
                });
            }
            /* 线程在 unregister 后再退出，由 stopOnLooper 调用 quit。 */
        }
    }

    public static long sampledVsyncTimeNs() {
        return sampledVsyncTimeNs;
    }

    public static long vsyncDurationNs() {
        return vsyncDurationNs;
    }

    /** VideoFrameReleaseHelper.Api30.setSurfaceFrameRate */
    public static void setSurfaceFrameRate(Surface surface, float frameRate) {
        if (Build.VERSION.SDK_INT < 30 || surface == null || !surface.isValid()) {
            return;
        }
        Api30.setSurfaceFrameRate(surface, frameRate);
    }

    static void updateSample(long vsyncTimeNs, long durationNs) {
        sampledVsyncTimeNs = vsyncTimeNs;
        if (durationNs != 0) {
            vsyncDurationNs = durationNs;
        }
    }

    private static void startOnLooper() {
        Context ctx = currentApplication();
        DisplayManager dm = ctx == null ? null
                : (DisplayManager) ctx.getSystemService(Context.DISPLAY_SERVICE);
        Choreographer choreographer;
        try {
            choreographer = Choreographer.getInstance();
        } catch (RuntimeException e) {
            /* 与 Media3 相同：平台异常则禁用采样（Internal: b/213926330） */
            Log.w(TAG, "Vsync sampling disabled due to platform error", e);
            return;
        }
        if (Build.VERSION.SDK_INT >= 33) {
            sampler = VSyncSamplerV33.create(choreographer, dm, handler);
        } else {
            sampler = new VSyncSamplerBase(choreographer, dm);
        }
        if (sampler != null) {
            sampler.register();
        }
    }

    private static void stopOnLooper() {
        if (sampler != null) {
            sampler.unregister();
            sampler = null;
        }
        sampledVsyncTimeNs = TIME_UNSET;
        vsyncDurationNs = TIME_UNSET;
        Looper looper = Looper.myLooper();
        if (looper != null) {
            looper.quit();
        }
        synchronized (LOCK) {
            thread = null;
            handler = null;
        }
    }

    static Context currentApplication() {
        try {
            Class<?> cl = Class.forName("android.app.ActivityThread");
            Object app = cl.getMethod("currentApplication").invoke(null);
            if (app instanceof Context) {
                return ((Context) app).getApplicationContext();
            }
        } catch (Throwable ignored) {
            /* 没有 Application 时只采不到 Display 刷新率，Choreographer 仍可用 */
        }
        return null;
    }

    /** Media3 VSyncSampler.getVsyncDurationNsFromDefaultDisplay */
    static long getVsyncDurationNsFromDefaultDisplay(DisplayManager displayManager) {
        if (displayManager == null) {
            return NANOS_PER_SECOND / 60L;
        }
        Display defaultDisplay = displayManager.getDisplay(Display.DEFAULT_DISPLAY);
        if (defaultDisplay != null) {
            double refresh = defaultDisplay.getRefreshRate();
            if (refresh > 0) {
                return (long) (NANOS_PER_SECOND / refresh);
            }
        }
        Log.w(TAG, "Unable to query display refresh rate, fallback 60Hz");
        return NANOS_PER_SECOND / 60L;
    }

    interface BaseSampler {
        void register();
        void unregister();
    }

    /** Media3 VideoFrameReleaseHelper.VSyncSamplerBase */
    private static final class VSyncSamplerBase implements BaseSampler, Choreographer.FrameCallback,
            DisplayManager.DisplayListener {
        final Choreographer choreographer;
        final DisplayManager displayManager;

        VSyncSamplerBase(Choreographer choreographer, DisplayManager displayManager) {
            this.choreographer = choreographer;
            this.displayManager = displayManager;
        }

        @Override
        public void register() {
            if (displayManager != null) {
                displayManager.registerDisplayListener(this, new Handler(Looper.myLooper()));
            }
            choreographer.removeFrameCallback(this);
            choreographer.postFrameCallback(this);
            updateSample(sampledVsyncTimeNs, getVsyncDurationNsFromDefaultDisplay(displayManager));
        }

        @Override
        public void unregister() {
            if (displayManager != null) {
                displayManager.unregisterDisplayListener(this);
            }
            choreographer.removeFrameCallback(this);
        }

        @Override
        public void doFrame(long vsyncTimeNs) {
            updateSample(vsyncTimeNs, vsyncDurationNs);
            choreographer.removeFrameCallback(this);
            choreographer.postFrameCallbackDelayed(this, VSYNC_SAMPLE_UPDATE_PERIOD_MS);
        }

        @Override
        public void onDisplayAdded(int displayId) {}

        @Override
        public void onDisplayRemoved(int displayId) {}

        @Override
        public void onDisplayChanged(int displayId) {
            if (displayId == Display.DEFAULT_DISPLAY) {
                choreographer.removeFrameCallback(this);
                choreographer.postFrameCallback(this);
                updateSample(sampledVsyncTimeNs, getVsyncDurationNsFromDefaultDisplay(displayManager));
            }
        }
    }

    private static final class Api30 {
        static void setSurfaceFrameRate(Surface surface, float frameRate) {
            int compatibility = frameRate == 0
                    ? Surface.FRAME_RATE_COMPATIBILITY_DEFAULT
                    : Surface.FRAME_RATE_COMPATIBILITY_FIXED_SOURCE;
            try {
                surface.setFrameRate(frameRate, compatibility);
            } catch (IllegalStateException e) {
                Log.e(TAG, "Failed to call Surface.setFrameRate", e);
            }
        }
    }
}
