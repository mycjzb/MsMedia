package com.mycjzb.media.player;

/**
 * 画面比例（对齐超级直播 {@code Cfg.aspectRatio} 字符串）。
 * 与系统 {@link MediaPlayer#setVideoScalingMode(int)} 并存：本类给布局量尺寸用。
 */
public final class AspectMode {
    /** 自动适应（按片源比例 fit）。 */
    public static final String AUTO = "auto";
    public static final String R16_9 = "16:9";
    public static final String R4_3 = "4:3";
    /** 原始比例（同 auto，语义留给 UI 文案）。 */
    public static final String ORIG = "orig";
    /** 铺满（可裁切）。 */
    public static final String FILL = "fill";

    private AspectMode() {}

    /** 非法 → {@link #AUTO}。 */
    public static String normalize(String ratio) {
        if (R16_9.equals(ratio) || R4_3.equals(ratio) || ORIG.equals(ratio)
                || AUTO.equals(ratio) || FILL.equals(ratio)) {
            return ratio;
        }
        return AUTO;
    }

    /** 内部量算代号（对齐 IRenderView AR_*）。 */
    static int toMeasureCode(String ratio) {
        String r = normalize(ratio);
        if (R16_9.equals(r)) return 4;
        if (R4_3.equals(r)) return 5;
        if (FILL.equals(r)) return 1; /* ASPECT_FILL */
        if (ORIG.equals(r) || AUTO.equals(r)) return 0; /* ASPECT_FIT */
        return 0;
    }
}
