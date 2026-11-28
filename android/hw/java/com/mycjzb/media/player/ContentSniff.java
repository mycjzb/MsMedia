package com.mycjzb.media.player;

/**
 * 只看正文字节头选封装；不看 URL 扩展名、不看 Content-Type。
 * MORE=还要字节；HLS/DASH/JS/PROG=可定。
 */
final class ContentSniff {
    static final int MORE = 0;
    static final int HLS = 1;
    static final int DASH = 2;
    static final int PROG = 3;
    /** JS 规则脚本（含 main），对齐 js-rules；确定后读全再 eval。 */
    static final int JS = 4;

    private ContentSniff() {}

    static int sniff(byte[] b, int n, boolean eof) {
        if (b == null || n <= 0) {
            return eof ? PROG : MORE;
        }
        int i = 0;
        if (n >= 3 && (b[0] & 0xff) == 0xef && (b[1] & 0xff) == 0xbb && (b[2] & 0xff) == 0xbf) {
            i = 3;
        }
        while (i < n && isWs(b[i])) {
            i++;
        }
        int left = n - i;
        if (left <= 0) {
            return eof ? PROG : MORE;
        }
        /* 加密/压缩 JS 容器（MSJS / gzip）优先于媒体魔数 */
        if (isWrappedJs(b, i, n)) {
            return JS;
        }
        /* 二进制封装：勿误判 JS */
        if (isBinaryMedia(b, i, n)) {
            return PROG;
        }
        if (startsWith(b, i, n, "#EXTM3U")) {
            return HLS;
        }
        if (!eof && prefixOf(b, i, n, "#EXTM3U")) {
            return MORE;
        }
        if (b[i] == '<') {
            if (indexOf(b, i, n, "<MPD") >= 0 || indexOf(b, i, n, "<mpd") >= 0) {
                return DASH;
            }
            if (!eof && n < 2048) {
                return MORE;
            }
            return PROG;
        }
        if (looksJs(b, i, n)) {
            return JS;
        }
        /* 文本且尚未凑齐 function main：多读一点再定 */
        if (!eof && n < 8192 && looksText(b, i, n) && indexOf(b, i, n, "function") >= 0) {
            return MORE;
        }
        return PROG;
    }

    private static boolean looksJs(byte[] b, int off, int n) {
        return indexOf(b, off, n, "function main") >= 0
                || indexOf(b, off, n, "function\tmain") >= 0
                || indexOf(b, off, n, "async function main") >= 0;
    }

    /**
     * 正文已是 JS 规则（明文 main / MSJS / gzip）。供 PrefixStrip 禁止剥头；
     * 伪装 URL（如 *.m3u8）也靠固定头 {@code MSJS} 认，不依赖扩展名。
     */
    static boolean isJsPayload(byte[] b, int n) {
        if (b == null || n <= 0) {
            return false;
        }
        int i = 0;
        if (n >= 3 && (b[0] & 0xff) == 0xef && (b[1] & 0xff) == 0xbb && (b[2] & 0xff) == 0xbf) {
            i = 3;
        }
        while (i < n && isWs(b[i])) {
            i++;
        }
        if (i >= n) {
            return false;
        }
        return isWrappedJs(b, i, n) || looksJs(b, i, n);
    }

    /** MSJS 密文或裸 gzip（解压后须含 function main，由 so unwrap 校验）。 */
    private static boolean isWrappedJs(byte[] b, int off, int n) {
        if (n - off >= 2 && (b[off] & 0xff) == 0x1f && (b[off + 1] & 0xff) == 0x8b) {
            return true;
        }
        return n - off >= 4 && b[off] == 'M' && b[off + 1] == 'S' && b[off + 2] == 'J'
                && b[off + 3] == 'S';
    }

    private static boolean isBinaryMedia(byte[] b, int off, int n) {
        if (n - off >= 8) {
            /* MP4/ISO ftyp */
            if (b[off + 4] == 'f' && b[off + 5] == 't' && b[off + 6] == 'y' && b[off + 7] == 'p') {
                return true;
            }
            /* PNG / JPEG / ID3 */
            if ((b[off] & 0xff) == 0x89 && b[off + 1] == 'P') {
                return true;
            }
            if ((b[off] & 0xff) == 0xff && (b[off + 1] & 0xff) == 0xd8) {
                return true;
            }
            if (b[off] == 'I' && b[off + 1] == 'D' && b[off + 2] == '3') {
                return true;
            }
        }
        /* TS：连续 0x47 */
        if (n - off >= 188 && (b[off] & 0xff) == 0x47) {
            return true;
        }
        return false;
    }

    private static boolean looksText(byte[] b, int off, int n) {
        int bad = 0;
        int m = Math.min(n, off + 512);
        for (int i = off; i < m; i++) {
            int c = b[i] & 0xff;
            if (c == 0) {
                return false;
            }
            if (c < 9 || (c > 13 && c < 32 && c != 27)) {
                bad++;
            }
        }
        return bad * 10 < (m - off);
    }

    private static boolean isWs(byte c) {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n';
    }

    private static boolean startsWith(byte[] b, int off, int n, String s) {
        int len = s.length();
        if (n - off < len) {
            return false;
        }
        for (int k = 0; k < len; k++) {
            if (b[off + k] != (byte) s.charAt(k)) {
                return false;
            }
        }
        return true;
    }

    private static boolean prefixOf(byte[] b, int off, int n, String s) {
        int left = n - off;
        if (left <= 0 || left >= s.length()) {
            return false;
        }
        for (int k = 0; k < left; k++) {
            if (b[off + k] != (byte) s.charAt(k)) {
                return false;
            }
        }
        return true;
    }

    private static int indexOf(byte[] b, int off, int n, String s) {
        int len = s.length();
        outer:
        for (int i = off; i + len <= n; i++) {
            for (int k = 0; k < len; k++) {
                if (b[i + k] != (byte) s.charAt(k)) {
                    continue outer;
                }
            }
            return i;
        }
        return -1;
    }
}
