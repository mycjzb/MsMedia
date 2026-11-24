/*
 * 与系统 android.media.TimedText 对齐的字幕文本：getText() / getBounds()。
 * 系统类的构造函数不公开，无法由第三方播放器创建，所以 MsMedia 自带一份同名类；
 * 业务把 import android.media.TimedText 换成 com.mycjzb.media.player.TimedText 即可。
 * 额外提供 getStartTimeMs / getEndTimeMs / getAlignment（MsMedia 扩展）。
 * by: Long <mycjzb@gmail.com>
 */
package com.mycjzb.media.player;

import android.graphics.Rect;

public final class TimedText {
    private final String text;
    private final Rect bounds;
    private final int startMs;
    private final int endMs;
    private final int alignment;

    TimedText(String text, Rect bounds, int startMs, int endMs, int alignment) {
        this.text = text;
        this.bounds = bounds;
        this.startMs = startMs;
        this.endMs = endMs;
        this.alignment = alignment;
    }

    /** 字幕文字（已去掉 ASS/HTML 样式标签，多行用 '\n' 分隔）。 */
    public String getText() {
        return text;
    }

    /**
     * 字幕位置。只有字幕自己带坐标（如 ASS 的 \pos）时才有值：返回宽高为 0、落在锚点上的 Rect，
     * 坐标系是字幕脚本的 PlayRes 画布，锚点含义看 {@link #getAlignment()}。
     * 没有坐标时返回 null，由业务按底部居中摆放（与系统对 SRT 的处理一致）。
     */
    public Rect getBounds() {
        return bounds == null ? null : new Rect(bounds);
    }

    /** MsMedia 扩展：本条字幕开始时间（毫秒，媒体时间轴），未知为 -1。 */
    public int getStartTimeMs() {
        return startMs;
    }

    /** MsMedia 扩展：本条字幕结束时间（毫秒），未知为 -1。 */
    public int getEndTimeMs() {
        return endMs;
    }

    /** MsMedia 扩展：ASS 数字键盘对齐 1–9（2 = 底部居中）；0 = 字幕未指定。 */
    public int getAlignment() {
        return alignment;
    }

    @Override
    public String toString() {
        return "TimedText{" + startMs + "-" + endMs + "ms, " + (text == null ? "null" : "\"" + text + "\"") + "}";
    }
}
