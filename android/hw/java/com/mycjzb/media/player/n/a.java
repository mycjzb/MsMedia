package com.mycjzb.media.player.n;

import android.net.Uri;
import android.text.TextUtils;

import com.mycjzb.media.player.MsNativeLoader;

/**
 * 短名 JNI 壳：o/c/f/u = open/close/feed/flush；m1/m2；q/s/r。
 * 线协议 query 名运行时拼出，避免 dex 连续明文。
 */
public final class a {
    public static final int m1 = 1;
    public static final int m2 = 2;

    private static final boolean ok;
    private static String qk;

    static {
        boolean v = false;
        try {
            v = MsNativeLoader.ensure();
        } catch (Throwable ignored) {
        }
        ok = v;
    }

    private a() {}

    /** wire：ysp-tea（字节拼，值不变） */
    private static String k() {
        if (qk == null) {
            qk = new String(new byte[] {0x79, 0x73, 0x70, 0x2d, 0x74, 0x65, 0x61});
        }
        return qk;
    }

    public static boolean r() {
        return ok;
    }

    public static int q(Uri u) {
        if (u == null) {
            return 0;
        }
        return qv(u.getQueryParameter(k()));
    }

    public static int q(String url) {
        if (TextUtils.isEmpty(url)) {
            return 0;
        }
        return q(Uri.parse(url));
    }

    private static int qv(String v) {
        if (v == null || v.length() == 0) {
            return 0;
        }
        try {
            int m = Integer.parseInt(v.trim());
            return (m == m1 || m == m2) ? m : 0;
        } catch (NumberFormatException e) {
            return 0;
        }
    }

    public static Uri s(Uri u) {
        if (u == null) {
            return null;
        }
        String key = k();
        if (u.getQueryParameter(key) == null) {
            return u;
        }
        Uri.Builder b = u.buildUpon().clearQuery();
        String enc = u.getEncodedQuery();
        if (enc == null || enc.length() == 0) {
            return b.build();
        }
        StringBuilder nb = new StringBuilder();
        for (String part : enc.split("&")) {
            if (part.length() == 0) {
                continue;
            }
            int eq = part.indexOf('=');
            String pk = eq >= 0 ? part.substring(0, eq) : part;
            if (key.equalsIgnoreCase(Uri.decode(pk))) {
                continue;
            }
            if (nb.length() > 0) {
                nb.append('&');
            }
            nb.append(part);
        }
        if (nb.length() > 0) {
            b.encodedQuery(nb.toString());
        }
        return b.build();
    }

    public static native long o(int mode);

    public static native void c(long h);

    public static native byte[] f(long h, byte[] in, int off, int len);

    public static native byte[] u(long h);
}
