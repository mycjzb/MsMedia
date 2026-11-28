package com.mycjzb.media.player;

import android.content.Context;
import android.util.AttributeSet;
import android.view.View;
import android.widget.FrameLayout;

/**
 * 轻量比例容器（量算抄超级直播 {@code MeasureHelper}）。
 * 用法：包住 SurfaceView；{@link #setAspectMode} + {@link #setVideoSize}（可在
 * {@code OnVideoSizeChangedListener} 里调）。
 */
public class MsAspectLayout extends FrameLayout {
    private static final int AR_FILL = 1;
    private static final int AR_16_9 = 4;
    private static final int AR_4_3 = 5;

    private String mode = AspectMode.AUTO;
    private int videoW;
    private int videoH;
    private int sarNum;
    private int sarDen;
    private int rotation;

    public MsAspectLayout(Context context) {
        super(context);
    }

    public MsAspectLayout(Context context, AttributeSet attrs) {
        super(context, attrs);
    }

    public MsAspectLayout(Context context, AttributeSet attrs, int defStyleAttr) {
        super(context, attrs, defStyleAttr);
    }

    public void setAspectMode(String ratio) {
        mode = AspectMode.normalize(ratio);
        requestLayout();
    }

    public String getAspectMode() {
        return mode;
    }

    public void setVideoSize(int width, int height) {
        if (width == videoW && height == videoH) return;
        videoW = width;
        videoH = height;
        requestLayout();
    }

    public void setVideoSampleAspectRatio(int num, int den) {
        sarNum = num;
        sarDen = den;
        requestLayout();
    }

    public void setVideoRotation(int degree) {
        rotation = degree;
        requestLayout();
    }

    @Override
    protected void onMeasure(int widthMeasureSpec, int heightMeasureSpec) {
        int ar = AspectMode.toMeasureCode(mode);
        if (rotation == 90 || rotation == 270) {
            int t = widthMeasureSpec;
            widthMeasureSpec = heightMeasureSpec;
            heightMeasureSpec = t;
        }
        int widthSpecSize = MeasureSpec.getSize(widthMeasureSpec);
        int heightSpecSize = MeasureSpec.getSize(heightMeasureSpec);
        int width = widthSpecSize;
        int height = heightSpecSize;

        if (videoW <= 0 || videoH <= 0 || widthSpecSize <= 0 || heightSpecSize <= 0) {
            /* 无片源尺寸时撑满，保证 Surface 能建出来 */
        } else {
            float specAr = (float) widthSpecSize / (float) heightSpecSize;
            float displayAr = aspectValue(ar);
            if (displayAr < 0.01f) displayAr = specAr;
            boolean fill = ar == AR_FILL;
            boolean wider = displayAr > specAr;
            if (fill) wider = !wider;
            if (wider) {
                width = widthSpecSize;
                height = (int) (width / displayAr);
            } else {
                height = heightSpecSize;
                width = (int) (height * displayAr);
            }
        }

        int childW = MeasureSpec.makeMeasureSpec(width, MeasureSpec.EXACTLY);
        int childH = MeasureSpec.makeMeasureSpec(height, MeasureSpec.EXACTLY);
        for (int i = 0; i < getChildCount(); i++) {
            View c = getChildAt(i);
            if (c.getVisibility() != GONE) {
                c.measure(childW, childH);
            }
        }
        setMeasuredDimension(widthSpecSize, heightSpecSize);
        /* 子 View 居中：在 onLayout */
        layoutCenterW = width;
        layoutCenterH = height;
    }

    private int layoutCenterW;
    private int layoutCenterH;

    @Override
    protected void onLayout(boolean changed, int left, int top, int right, int bottom) {
        int pw = right - left;
        int ph = bottom - top;
        int cw = layoutCenterW > 0 ? layoutCenterW : pw;
        int ch = layoutCenterH > 0 ? layoutCenterH : ph;
        int cl = (pw - cw) / 2;
        int ct = (ph - ch) / 2;
        for (int i = 0; i < getChildCount(); i++) {
            View c = getChildAt(i);
            if (c.getVisibility() != GONE) {
                c.layout(cl, ct, cl + cw, ct + ch);
            }
        }
    }

    private float aspectValue(int ar) {
        if (ar == AR_16_9) {
            return (rotation == 90 || rotation == 270) ? 9f / 16f : 16f / 9f;
        }
        if (ar == AR_4_3) {
            return (rotation == 90 || rotation == 270) ? 3f / 4f : 4f / 3f;
        }
        float r = (float) videoW / (float) videoH;
        if (sarNum > 0 && sarDen > 0) {
            r = r * sarNum / sarDen;
        }
        return r;
    }
}
