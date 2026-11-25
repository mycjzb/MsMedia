/*
 * 移植 androidx.media3.exoplayer.video.VideoFrameReleaseHelper.VSyncSamplerV33。
 * 单独成类，避免 API 18 校验 Choreographer.VsyncCallback 时 VerifyError。
 * 对照 androidx/media main@101e97c。
 *
 * 原始版权：Copyright (C) The Android Open Source Project
 * 原始许可：Apache License 2.0
 * 移植注释：by: Long <mycjzb@gmail.com>
 */
package com.mycjzb.media.player.media3;

import android.hardware.display.DisplayManager;
import android.os.Handler;
import android.view.Choreographer;
import android.view.Display;

final class VSyncSamplerV33 implements VSyncSampler.BaseSampler, Choreographer.VsyncCallback,
        DisplayManager.DisplayListener {
    private final Choreographer choreographer;
    private final DisplayManager displayManager;
    private final Handler handler;

    static VSyncSampler.BaseSampler create(Choreographer choreographer, DisplayManager dm,
                                          Handler handler) {
        return new VSyncSamplerV33(choreographer, dm, handler);
    }

    private VSyncSamplerV33(Choreographer choreographer, DisplayManager displayManager,
                            Handler handler) {
        this.choreographer = choreographer;
        this.displayManager = displayManager;
        this.handler = handler;
    }

    @Override
    public void register() {
        if (displayManager != null) {
            displayManager.registerDisplayListener(this, handler);
        }
        choreographer.removeVsyncCallback(this);
        choreographer.postVsyncCallback(this);
    }

    @Override
    public void unregister() {
        if (displayManager != null) {
            displayManager.unregisterDisplayListener(this);
        }
        handler.removeCallbacksAndMessages(null);
        choreographer.removeVsyncCallback(this);
    }

    @Override
    public void onVsync(Choreographer.FrameData data) {
        long t = data.getFrameTimeNanos();
        long d = VSyncSampler.TIME_UNSET;
        Choreographer.FrameTimeline[] timelines = data.getFrameTimelines();
        if (timelines != null && timelines.length >= 2) {
            long dur = timelines[1].getExpectedPresentationTimeNanos()
                    - timelines[0].getExpectedPresentationTimeNanos();
            d = dur == 0 ? VSyncSampler.TIME_UNSET : dur;
        }
        VSyncSampler.updateSample(t, d);
        handler.removeCallbacksAndMessages(null);
        final VSyncSamplerV33 self = this;
        handler.postDelayed(new Runnable() {
            @Override public void run() {
                choreographer.postVsyncCallback(self);
            }
        }, VSyncSampler.VSYNC_SAMPLE_UPDATE_PERIOD_MS);
    }

    @Override
    public void onDisplayAdded(int displayId) {}

    @Override
    public void onDisplayRemoved(int displayId) {}

    @Override
    public void onDisplayChanged(int displayId) {
        if (displayId == Display.DEFAULT_DISPLAY) {
            handler.removeCallbacksAndMessages(null);
            choreographer.removeVsyncCallback(this);
            choreographer.postVsyncCallback(this);
        }
    }
}
