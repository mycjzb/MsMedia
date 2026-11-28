/*
 * 与系统 android.media.TimedText 对齐的字幕：getText() / getBounds()；
 * MsMedia 扩展：时间轴、ASS 对齐、DVB/PGS 位图（抄超级直播 MsTimedText）。
 * by: Long <mycjzb@gmail.com>
 */
package com.mycjzb.media.player;

import android.graphics.Bitmap;
import android.graphics.Rect;

public final class TimedText {
    private final String text;
    private final Rect bounds;
    private final int startMs;
    private final int endMs;
    private final int alignment;
    private final Bitmap bitmap;
    private final int canvasW;
    private final int canvasH;

    TimedText(String text, Rect bounds, int startMs, int endMs, int alignment) {
        this(text, bounds, startMs, endMs, alignment, null, 0, 0);
    }

    TimedText(String text, Rect bounds, int startMs, int endMs, int alignment,
              Bitmap bitmap, int canvasW, int canvasH) {
        this.text = text;
        this.bounds = bounds;
        this.startMs = startMs;
        this.endMs = endMs;
        this.alignment = alignment;
        this.bitmap = bitmap;
        this.canvasW = canvasW;
        this.canvasH = canvasH;
    }

    /** Media3 Cue.bitmap → 位图字幕（画布默认 1280×720，对齐 ScreenFit）。 */
    static TimedText fromBitmap(Bitmap bmp, Rect bounds, int canvasW, int canvasH) {
        if (bmp == null || bmp.isRecycled()) {
            return null;
        }
        int cw = canvasW > 0 ? canvasW : 1280;
        int ch = canvasH > 0 ? canvasH : 720;
        return new TimedText(null, bounds, -1, -1, 0, bmp, cw, ch);
    }

    /** 字幕文字（已去掉 ASS/HTML 样式标签，多行用 '\n' 分隔）；位图轨为 null。 */
    public String getText() {
        return text;
    }

    /**
     * 字幕位置。文字：ASS \pos 等才有值。位图：1280×720（或 {@link #getCanvasWidth}）坐标系下的目标框。
     */
    public Rect getBounds() {
        return bounds == null ? null : new Rect(bounds);
    }

    /** MsMedia 扩展：本条字幕开始时间（毫秒），未知为 -1。 */
    public int getStartTimeMs() {
        return startMs;
    }

    /** MsMedia 扩展：本条字幕结束时间（毫秒），未知为 -1。 */
    public int getEndTimeMs() {
        return endMs;
    }

    /** MsMedia 扩展：ASS 数字键盘对齐 1–9；0 = 未指定。 */
    public int getAlignment() {
        return alignment;
    }

    /** MsMedia 扩展：DVB/PGS 位图；无则 null。 */
    public Bitmap getBitmap() {
        return bitmap;
    }

    /** 位图画布宽；无位图为 0。 */
    public int getCanvasWidth() {
        return canvasW;
    }

    /** 位图画布高；无位图为 0。 */
    public int getCanvasHeight() {
        return canvasH;
    }

    public boolean hasBitmap() {
        return bitmap != null && !bitmap.isRecycled();
    }

    @Override
    public String toString() {
        if (hasBitmap()) {
            return "TimedText{bitmap " + canvasW + "x" + canvasH + "}";
        }
        return "TimedText{" + startMs + "-" + endMs + "ms, "
                + (text == null ? "null" : "\"" + text + "\"") + "}";
    }
}
