/*
 * 硬解探测 Activity：加载 libmshw，列出选择结果。
 * 不以本界面出现宣称真实硬解已跑通。
 * by: Long <mycjzb@gmail.com>
 */
package com.msmedia;

import android.app.Activity;
import android.os.Bundle;
import android.util.Log;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.widget.TextView;

public class MsHwSmokeActivity extends Activity implements SurfaceHolder.Callback {
    private static final String TAG = "MsHw";
    private TextView text;

    static {
        System.loadLibrary("mshw");
    }

    private native String nativeProbe(Surface surface);

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        text = new TextView(this);
        text.setTextSize(14f);
        text.setPadding(24, 24, 24, 24);
        SurfaceView sv = new SurfaceView(this);
        sv.getHolder().addCallback(this);
        android.widget.LinearLayout layout = new android.widget.LinearLayout(this);
        layout.setOrientation(android.widget.LinearLayout.VERTICAL);
        layout.addView(sv, new android.widget.LinearLayout.LayoutParams(
                android.widget.LinearLayout.LayoutParams.MATCH_PARENT, 400));
        layout.addView(text);
        setContentView(layout);
        try {
            text.setText(nativeProbe(null));
        } catch (Throwable t) {
            text.setText("nativeProbe failed: " + t.getClass().getSimpleName());
            Log.e(TAG, "probe", t);
        }
    }

    @Override
    public void surfaceCreated(SurfaceHolder holder) {
        Surface s = holder.getSurface();
        try {
            String r = nativeProbe(s);
            text.setText(r);
            Log.i(TAG, r);
        } catch (Throwable t) {
            text.setText("surface probe failed: " + t.getClass().getSimpleName());
            Log.e(TAG, "surface probe", t);
        }
    }

    @Override
    public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
    }

    @Override
    public void surfaceDestroyed(SurfaceHolder holder) {
    }
}
