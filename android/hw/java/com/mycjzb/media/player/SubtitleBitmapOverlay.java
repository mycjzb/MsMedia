package com.mycjzb.media.player;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Rect;
import android.util.AttributeSet;
import android.view.View;

/**
 * DVB/PGS 位图字幕层：抄超级直播 {@code SubtitleBitmapOverlay}
 * （Canvas + filterBitmap，画布坐标缩到 View）。
 */
public final class SubtitleBitmapOverlay extends View {
    private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Rect dest = new Rect();
    private Bitmap bmp;

    public SubtitleBitmapOverlay(Context context) {
        super(context);
        init();
    }

    public SubtitleBitmapOverlay(Context context, AttributeSet attrs) {
        super(context, attrs);
        init();
    }

    private void init() {
        paint.setFilterBitmap(true);
        setWillNotDraw(false);
    }

    public void clearCue() {
        bmp = null;
        dest.setEmpty();
        setVisibility(INVISIBLE);
        invalidate();
    }

    /** @param bounds 在 canvasW×canvasH 坐标系；null/非法则清屏 */
    public void showCue(Bitmap bitmap, Rect bounds, int canvasW, int canvasH) {
        if (bitmap == null || bitmap.isRecycled()
                || bounds == null || bounds.width() <= 0 || bounds.height() <= 0
                || canvasW <= 0 || canvasH <= 0) {
            clearCue();
            return;
        }
        bmp = bitmap;
        int vw = getWidth();
        int vh = getHeight();
        if (vw <= 0 || vh <= 0) {
            final Bitmap ref = bitmap;
            final Rect bc = new Rect(bounds);
            final int cw = canvasW;
            final int ch = canvasH;
            setVisibility(VISIBLE);
            post(new Runnable() {
                @Override
                public void run() {
                    if (!ref.isRecycled()) {
                        showCue(ref, bc, cw, ch);
                    }
                }
            });
            return;
        }
        float scale = Math.min((float) vw / canvasW, (float) vh / canvasH);
        float dx = (vw - canvasW * scale) * 0.5f;
        float dy = (vh - canvasH * scale) * 0.5f;
        int left = Math.round(dx + bounds.left * scale);
        int top = Math.round(dy + bounds.top * scale);
        int right = Math.round(dx + bounds.right * scale);
        int bottom = Math.round(dy + bounds.bottom * scale);
        if (right <= left || bottom <= top) {
            clearCue();
            return;
        }
        dest.set(left, top, right, bottom);
        setVisibility(VISIBLE);
        invalidate();
    }

    /** 便捷：直接吃 {@link TimedText} 位图。 */
    public void showTimedText(TimedText t) {
        if (t == null || !t.hasBitmap()) {
            clearCue();
            return;
        }
        showCue(t.getBitmap(), t.getBounds(), t.getCanvasWidth(), t.getCanvasHeight());
    }

    @Override
    protected void onDraw(Canvas canvas) {
        if (bmp == null || bmp.isRecycled() || dest.isEmpty()) {
            return;
        }
        canvas.drawBitmap(bmp, null, dest, paint);
    }
}
