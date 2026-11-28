package com.mycjzb.media.player.tvbus;

import java.io.ByteArrayOutputStream;
import java.math.BigInteger;
import java.nio.charset.StandardCharsets;
import java.util.Arrays;

import javax.crypto.Cipher;
import javax.crypto.spec.SecretKeySpec;

/**
 * {@code tvbus://<base58>} → {@code sop://<broker>:<port>/<channel>}。
 *
 * <p>对齐 bs-tvbus {@code internal/tvbusurl}（{@code CTVBusImpl::parseUrl}）：
 * base58 → 若 {@code (n-4)%8≠0} 首字节是标志位需去掉 → 依次删下标 2,3,6,8 四字节作密钥
 * → 余下 8 字节块 DES-ECB 解密，明文须以 {@code sop://} 开头。
 */
final class TvbUrl {

    private static final String ALPHABET =
            "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";
    private static final int[] KEY_POS = {2, 3, 6, 8};
    private static final int[] SLOT_POS = {6, 4, 2, 0};

    private TvbUrl() {
    }

    /** 失败返回 null。 */
    static String decode(String addr) {
        try {
            if (addr == null) {
                return null;
            }
            String s = addr.trim();
            if (s.length() >= 8 && s.regionMatches(true, 0, "tvbus://", 0, 8)) {
                s = s.substring(8);
            }
            byte[] b = b58dec(s);
            if (b == null || b.length <= 7) {
                return null;
            }
            if ((b.length - 4) % 8 != 0) {
                b = Arrays.copyOfRange(b, 1, b.length);
            }
            byte[] kb = new byte[4];
            for (int i = 0; i < 4; i++) {
                int p = KEY_POS[i];
                if (p >= b.length) {
                    return null;
                }
                kb[i] = b[p];
                byte[] nb = new byte[b.length - 1];
                System.arraycopy(b, 0, nb, 0, p);
                System.arraycopy(b, p + 1, nb, p, b.length - p - 1);
                b = nb;
            }
            Cipher c = Cipher.getInstance("DES/ECB/NoPadding");
            c.init(Cipher.DECRYPT_MODE, new SecretKeySpec(keyBytes(kb), "DES"));
            ByteArrayOutputStream out = new ByteArrayOutputStream();
            byte[] blk = new byte[8];
            for (int i = 0; i + 8 <= b.length; i += 8) {
                c.doFinal(b, i, 8, blk, 0);
                int z = 0;
                while (z < 8 && blk[z] != 0) {
                    z++;
                }
                out.write(blk, 0, z);
            }
            String sop = new String(out.toByteArray(), StandardCharsets.US_ASCII);
            return sop.startsWith("sop://") ? sop : null;
        } catch (Throwable t) {
            return null;
        }
    }

    private static byte[] keyBytes(byte[] kb) {
        byte[] k = new byte[8];
        for (int i = 0; i < 4; i++) {
            String h = String.format("%02x", kb[i] & 0xff);
            k[SLOT_POS[i]] = (byte) h.charAt(1);
            k[SLOT_POS[i] + 1] = (byte) h.charAt(0);
        }
        return k;
    }

    private static byte[] b58dec(String s) {
        BigInteger n = BigInteger.ZERO;
        BigInteger base = BigInteger.valueOf(58);
        for (int i = 0; i < s.length(); i++) {
            int idx = ALPHABET.indexOf(s.charAt(i));
            if (idx < 0) {
                return null;
            }
            n = n.multiply(base).add(BigInteger.valueOf(idx));
        }
        int zeros = 0;
        while (zeros < s.length() && s.charAt(zeros) == '1') {
            zeros++;
        }
        byte[] nb;
        if (n.signum() == 0) {
            nb = new byte[0];
        } else {
            nb = n.toByteArray();
            if (nb.length > 1 && nb[0] == 0) {
                nb = Arrays.copyOfRange(nb, 1, nb.length);
            }
        }
        byte[] out = new byte[zeros + nb.length];
        System.arraycopy(nb, 0, out, zeros, nb.length);
        return out;
    }
}
