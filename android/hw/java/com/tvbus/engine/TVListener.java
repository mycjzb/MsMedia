package com.tvbus.engine;

/**
 * {@code libtvcore.so} 回调接口（native 通过 {@code RegisterNatives} 反射回调）。
 *
 * <p>包名与类名固定为 {@code com.tvbus.engine.TVListener}：MsMedia 的
 * {@code ms_tvb.c} 在 {@code RegisterNatives} 时按此全名查找，改包名会失败。
 */
public interface TVListener {
    void onInited(String result);

    void onStart(String result);

    void onPrepared(String result);

    void onInfo(String result);

    void onStop(String result);

    void onQuit(String result);
}
