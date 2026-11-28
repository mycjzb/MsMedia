/* m1 stream — see ysp_tea.h */
#include "ysp_tea.h"
#include "../guard/guard.h"

#include <cstdlib>
#include <cstring>
#include <new>
#include <vector>

namespace {

constexpr uint32_t kDelta = 0x9e3779b9u;
constexpr int kDefaultGoLog2 = 4;
constexpr int kTsPacket = 188;

static inline uint32_t rd_u32le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static inline void wr_u32le(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v);
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

/* 密钥字预先读出，避免每块重复读 key。 */
static void d8k(uint8_t blk[8], uint32_t k0, uint32_t k1, uint32_t k2,
                                uint32_t k3) {
    uint32_t v0 = rd_u32le(blk);
    uint32_t v1 = rd_u32le(blk + 4);
    uint32_t sum = (kDelta << 4);
    for (int r = 0; r < 16; r++) {
        v1 -= (((v0 << 4) + k2) ^ (v0 + sum) ^ ((v0 >> 5) + k3));
        v0 -= (((v1 << 4) + k0) ^ (v1 + sum) ^ ((v1 >> 5) + k1));
        sum -= kDelta;
    }
    wr_u32le(blk, v0);
    wr_u32le(blk + 4, v1);
}

static bool has_epb(const uint8_t *data, size_t len) {
    for (size_t i = 0; i + 2 < len; i++) {
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 3) return true;
    }
    return false;
}

/* 写入 dst（需 >= len），返回 RBSP 长度。无 EPB 时 memcpy。
 * strict（H5E wasm）：03 后须还有字节且 <=3 才剥，NAL 末尾的 00 00 03 保留。 */
static size_t remove_epb_into(uint8_t *dst, const uint8_t *data, size_t len,
                              bool strict = false) {
    if (!has_epb(data, len)) {
        if (dst != data) memcpy(dst, data, len);
        return len;
    }
    size_t o = 0;
    for (size_t i = 0; i < len; i++) {
        if (i + 2 < len && data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 3 &&
            (!strict || (i + 3 < len && data[i + 3] <= 3))) {
            dst[o++] = 0;
            dst[o++] = 0;
            i += 2;
        } else {
            dst[o++] = data[i];
        }
    }
    return o;
}

static std::vector<uint8_t> remove_epb(const uint8_t *data, size_t len) {
    std::vector<uint8_t> out(len);
    out.resize(remove_epb_into(out.data(), data, len));
    return out;
}

/* 仅 tea/live 模式写回同槽时可能需要；go 模式对齐 UrlPorxy 不调用。 */
static std::vector<uint8_t> add_epb(const uint8_t *rbsp, size_t len) {
    std::vector<uint8_t> out;
    out.reserve(len + len / 48 + 8);
    int zeros = 0;
    for (size_t i = 0; i < len; i++) {
        uint8_t b = rbsp[i];
        if (zeros == 2 && b <= 3) {
            out.push_back(0x03);
            zeros = 0;
        }
        out.push_back(b);
        if (b == 0)
            zeros++;
        else
            zeros = 0;
    }
    return out;
}

struct BitReader {
    const uint8_t *data;
    size_t len;
    size_t bit_pos;

    BitReader(const uint8_t *d, size_t n, size_t start_bit = 0)
        : data(d), len(n), bit_pos(start_bit) {}

    explicit BitReader(const std::vector<uint8_t> &v, size_t start_bit = 0)
        : data(v.data()), len(v.size()), bit_pos(start_bit) {}

    int bit() {
        if (bit_pos >= len * 8) return -1;
        int v = (data[bit_pos >> 3] >> (7 - (bit_pos & 7))) & 1;
        bit_pos++;
        return v;
    }

    int bits(int count) {
        int v = 0;
        for (int i = 0; i < count; i++) {
            int b = bit();
            if (b < 0) return -1;
            v = (v << 1) | b;
        }
        return v;
    }

    int ue() {
        int zeros = 0;
        for (;;) {
            int b = bit();
            if (b < 0) return -1;
            if (b == 1) break;
            zeros++;
            if (zeros > 31) return -1;
        }
        if (zeros == 0) return 0;
        int rest = bits(zeros);
        if (rest < 0) return -1;
        return (1 << zeros) - 1 + rest;
    }
};

static void skip_scaling_list(BitReader &br, int size) {
    int lastScale = 8, nextScale = 8;
    for (int i = 0; i < size; i++) {
        if (nextScale != 0) {
            int codeNum = br.ue();
            if (codeNum < 0) return;
            int deltaScale = (codeNum & 1) ? (codeNum + 1) / 2 : -(codeNum / 2);
            nextScale = (lastScale + deltaScale + 256) & 0xff;
        }
        lastScale = nextScale == 0 ? lastScale : nextScale;
    }
}

static int p_sps(const uint8_t *nal, size_t nal_len) {
    auto rbsp = remove_epb(nal, nal_len);
    if (rbsp.size() < 5 || (rbsp[0] & 0x1f) != 7) return -1;
    BitReader br(rbsp, 8);
    int profileIdc = br.bits(8);
    if (profileIdc < 0) return -1;
    if (br.bits(8) < 0 || br.bits(8) < 0) return -1;
    if (br.ue() < 0) return -1;
    static const int hi[] = {44, 83, 86, 100, 110, 118, 122, 128, 134, 135, 138, 139, 244};
    bool high = false;
    for (int p : hi)
        if (p == profileIdc) {
            high = true;
            break;
        }
    if (high) {
        int chroma = br.ue();
        if (chroma < 0) return -1;
        if (chroma == 3 && br.bit() < 0) return -1;
        if (br.ue() < 0 || br.ue() < 0 || br.bit() < 0) return -1;
        int seqScaling = br.bit();
        if (seqScaling < 0) return -1;
        if (seqScaling) {
            int count = chroma == 3 ? 12 : 8;
            for (int i = 0; i < count; i++) {
                int f = br.bit();
                if (f < 0) return -1;
                if (f) skip_scaling_list(br, i < 6 ? 16 : 64);
            }
        }
    }
    int v = br.ue();
    if (v < 0) return -1;
    v += 4;
    return (v >= 4 && v <= 16) ? v : -1;
}

/* 顺带带出 slice_type（2/7=I）。开放 GOP 的周期 I 帧仍是 nal_type 1，官方 so 给它 stride 160。 */
static int p_fn(const uint8_t *rbsp, size_t rbsp_len, int log2,
                                      int *slice_type = nullptr) {
    BitReader br(rbsp, rbsp_len, 8);
    if (br.ue() < 0) return -1;
    int st = br.ue();
    if (st < 0 || br.ue() < 0) return -1;
    if (slice_type) *slice_type = st;
    return br.bits(log2);
}

static bool g_idr_ok(int s) {
    static const int ok[] = {256, 224, 192, 160, 288, 128, 320, 96};
    for (int v : ok)
        if (v == s) return true;
    return false;
}

static size_t find_nal_start(const uint8_t *data, size_t pos, size_t total) {
    while (pos + 2 < total) {
        if (data[pos + 2] == 0) {
            if (pos + 3 < total && data[pos + 1] == 0 && data[pos + 3] == 1) return pos + 1;
            pos += 2;
        } else if (data[pos + 2] == 1) {
            if (data[pos] == 0 && data[pos + 1] == 0) return pos;
            pos += 3;
        } else {
            pos += 3;
        }
    }
    return total;
}

struct OutBuf {
    std::vector<uint8_t> data;
    void append(const uint8_t *p, size_t n) {
        if (n) data.insert(data.end(), p, p + n);
    }
    void append_pkt(const uint8_t pkt[kTsPacket]) { append(pkt, kTsPacket); }
};

} // namespace

struct QueuedPkt {
    uint8_t data[kTsPacket];
    bool ready = true;
};

struct m1 {
    int mode = MS_M1_MODE_1;
    int idr_stride = 0;
    int p_stride = 0;
    int log2_max_frame_num = kDefaultGoLog2;
    int eflg = -1;

    std::vector<uint8_t> sync;
    std::vector<QueuedPkt> q;
    std::vector<size_t> pes_qidx;
    std::vector<int> payload_off;
    std::vector<uint8_t> pes;
    int ts_count = 0;
    int video_pid = -1;

    /* 复用，避免每 NAL new vector */
    std::vector<uint8_t> scratch;
    std::vector<uint8_t> chunks;

    int s_rbsp(const uint8_t *rbsp, size_t rbsp_len, int nal_type) const {
        if (mode == MS_M1_MODE_1) {
            if (nal_type == 5) {
                /* 手动覆盖优先；否则完整算法：160+32*(rbsp[5]&3) → 160|192|224|256 */
                if (idr_stride > 0)
                    return g_idr_ok(idr_stride) ? idr_stride : 256;
                if (rbsp_len < 6) return 256;
                return 160 + 32 * (rbsp[5] & 3);
            }
            if (p_stride == 224 || p_stride == 256) return p_stride;
            int log2 = log2_max_frame_num > 0 ? log2_max_frame_num : kDefaultGoLog2;
            int st = -1;
            int fn = p_fn(rbsp, rbsp_len, log2, &st);
            /* 非 IDR 的 I 帧（开放 GOP，约 4~5 分钟一个）：so 用 160。当 P 帧按 frame_num
             * 取 224/256 会整帧解坏，后续 P/B 参考它，一直花到下一个 IDR。 */
            if (st == 2 || st == 7) return 160;
            return (fn >= 0 && (fn & (1 << (log2 - 1))) != 0) ? 256 : 224;
        }
        return 80;
    }

    /* 1=央视频：EBSP<=128 明文直通；否则 strict 剥 EPB 后按 GO 公式解，不 add_epb。
     * 2=H5E：对齐 NativeWasmTv decrypt_nal——EBSP<=128 明文直通；否则 strict 剥 EPB，
     *   RBSP 下标 key@16 / stride80 classic TEA，输出 RBSP（不补回 03）。 */
    void d_ni(std::vector<uint8_t> &dst, const uint8_t *unit, size_t unit_len,
                          int nal_type) {
        if (mode == MS_M1_MODE_2) {
            if (unit_len <= 128) {
                dst.assign(unit, unit + unit_len);
                return;
            }
            scratch.resize(unit_len);
            size_t ulen = remove_epb_into(scratch.data(), unit, unit_len, true);
            uint32_t hk0 = rd_u32le(scratch.data() + 16);
            uint32_t hk1 = rd_u32le(scratch.data() + 20);
            uint32_t hk2 = rd_u32le(scratch.data() + 24);
            uint32_t hk3 = rd_u32le(scratch.data() + 28);
            for (size_t j = 0; 32 + j * 80 + 8 <= ulen; j++)
                d8k(scratch.data() + 32 + j * 80, hk0, hk1, hk2, hk3);
            dst.assign(scratch.data(), scratch.data() + ulen);
            return;
        }

        scratch.resize(unit_len);
        if (mode == MS_M1_MODE_1) {
            if (unit_len <= 128) {
                dst.assign(unit, unit + unit_len);
                return;
            }
            size_t ulen = remove_epb_into(scratch.data(), unit, unit_len, true);
            d_u(scratch.data(), ulen, nal_type);
            dst.assign(scratch.data(), scratch.data() + ulen);
            return;
        }

        size_t ulen = remove_epb_into(scratch.data(), unit, unit_len);
        if (ulen < 32) {
            dst.assign(unit, unit + unit_len);
            return;
        }

        int key_off = 16;
        if (key_off + 16 > (int)ulen) {
            dst.assign(unit, unit + unit_len);
            return;
        }
        uint32_t k0 = rd_u32le(scratch.data() + key_off);
        uint32_t k1 = rd_u32le(scratch.data() + key_off + 4);
        uint32_t k2 = rd_u32le(scratch.data() + key_off + 8);
        uint32_t k3 = rd_u32le(scratch.data() + key_off + 12);
        int stride = s_rbsp(scratch.data(), ulen, nal_type);

        int iterations = (int)((ulen - 32) / (size_t)stride);
        for (int i = 0; i < iterations; i++) {
            size_t off = 32 + (size_t)i * (size_t)stride;
            d8k(scratch.data() + off, k0, k1, k2, k3);
        }
        dst = add_epb(scratch.data(), ulen);
    }

    /* plen==0 = 视频 unbounded（CCTV-3 部分 CDN 全线这么写），EOF 按已收齐；只认 plen>0 且
     * 长度不够为真截断。若把 plen==0 当截断，片尾 PES 会整段留密文，每片固定一帧马赛克。 */
    bool pes_length_complete() const {
        if (pes.size() < 6) return false;
        if (!(pes[0] == 0 && pes[1] == 0 && pes[2] == 1)) return false;
        unsigned plen = ((unsigned)pes[4] << 8) | (unsigned)pes[5];
        if (plen == 0) return true;
        return pes.size() >= 6u + plen;
    }

    /* leftover TEA：整段扫描。H5E：对齐 wasm——截断 PES 整段不解；完整 NAL 走 RBSP classic+drop EPB，重写 PES。 */
    std::vector<uint8_t> parse_pes(const uint8_t *pes_in, size_t pes_len, bool pes_complete) {
        if (mode == MS_M1_MODE_1) {
            r_go(pes_in, pes_len);
            return chunks;
        }

        if (mode == MS_M1_MODE_2) {
            /* wasm decrypt_pes: packet_length 未收齐的分片尾 PES 整段保持密文。 */
            if (!pes_complete && pes_len >= 6 && pes_in[0] == 0 && pes_in[1] == 0 &&
                pes_in[2] == 1) {
                unsigned plen = ((unsigned)pes_in[4] << 8) | (unsigned)pes_in[5];
                if (plen > 0 && pes_len < 6u + plen)
                    return std::vector<uint8_t>(pes_in, pes_in + pes_len);
            }
            return r_h5(pes_in, pes_len, pes_complete);
        }

        std::vector<uint8_t> pes(pes_in, pes_in + pes_len);
        size_t begin = 0;
        size_t total = pes.size();
        std::vector<uint8_t> dec;
        while (begin + 4 < total) {
            int sc = 0;
            if (pes[begin] == 0 && pes[begin + 1] == 0 && pes[begin + 2] == 1)
                sc = 3;
            else if (pes[begin] == 0 && pes[begin + 1] == 0 && pes[begin + 2] == 0 &&
                     pes[begin + 3] == 1)
                sc = 4;
            else {
                begin++;
                continue;
            }
            size_t hdr = begin + (size_t)sc;
            if (hdr >= total) break;
            int nal_type = pes[hdr] & 0x1f;
            size_t end = find_nal_start(pes.data(), hdr + 1, total);
            if (nal_type == 1 || nal_type == 5 || nal_type == 25) {
                d_ni(dec, pes.data() + hdr, end - hdr, nal_type);
                size_t copy = dec.size() < (end - hdr) ? dec.size() : (end - hdr);
                memcpy(pes.data() + hdr, dec.data(), copy);
            }
            begin = end;
        }
        return pes;
    }

    /* H5E：PES 头保留，VCL NAL 解完可能变短，必须重写 PES（不能原位 memcpy 留尾）。 */
    std::vector<uint8_t> r_h5(const uint8_t *pes_in, size_t pes_len,
                                         bool pes_complete) {
        chunks.clear();
        chunks.reserve(pes_len + 64);
        size_t begin = 0, cursor = 0, total = pes_len;
        size_t es0 = 0;
        if (total >= 9 && pes_in[0] == 0 && pes_in[1] == 0 && pes_in[2] == 1)
            es0 = 9 + (size_t)pes_in[8];
        if (es0 > total) es0 = 0;
        if (es0 > 0) {
            chunks.insert(chunks.end(), pes_in, pes_in + es0);
            begin = cursor = es0;
        }
        std::vector<uint8_t> dec;
        while (begin + 4 < total) {
            int sc = 0;
            if (pes_in[begin] == 0 && pes_in[begin + 1] == 0 && pes_in[begin + 2] == 1)
                sc = 3;
            else if (pes_in[begin] == 0 && pes_in[begin + 1] == 0 && pes_in[begin + 2] == 0 &&
                     pes_in[begin + 3] == 1)
                sc = 4;
            else {
                begin++;
                continue;
            }
            if (begin > cursor)
                chunks.insert(chunks.end(), pes_in + cursor, pes_in + begin);
            size_t hdr = begin + (size_t)sc;
            if (hdr >= total) break;
            int nal_type = pes_in[hdr] & 0x1f;
            size_t end = find_nal_start(pes_in, hdr + 1, total);
            bool nal_complete = (end < total) || pes_complete;
            chunks.insert(chunks.end(), pes_in + begin, pes_in + hdr);
            if ((nal_type == 1 || nal_type == 5) && nal_complete) {
                d_ni(dec, pes_in + hdr, end - hdr, nal_type);
                chunks.insert(chunks.end(), dec.begin(), dec.end());
            } else if (nal_complete) {
                chunks.insert(chunks.end(), pes_in + hdr, pes_in + end);
            } else {
                /* 分片截断的末尾半截 NAL：保持密文（对齐此前 H5E 行为）。 */
                chunks.insert(chunks.end(), pes_in + hdr, pes_in + end);
            }
            begin = end;
            cursor = end;
        }
        if (cursor < total)
            chunks.insert(chunks.end(), pes_in + cursor, pes_in + total);
        /* 更新 PES packet_length（变短时），对齐 wasm write_pes_payload。 */
        if (chunks.size() >= 6 && chunks[0] == 0 && chunks[1] == 0 && chunks[2] == 1) {
            unsigned old_plen = ((unsigned)chunks[4] << 8) | (unsigned)chunks[5];
            if (old_plen > 0 && chunks.size() >= 6) {
                size_t new_plen = chunks.size() - 6;
                if (new_plen <= 0xffff) {
                    chunks[4] = (uint8_t)((new_plen >> 8) & 0xff);
                    chunks[5] = (uint8_t)(new_plen & 0xff);
                }
            }
        }
        return chunks;
    }

    void r_go(const uint8_t *pes_in, size_t pes_len) {
        chunks.clear();
        chunks.reserve(pes_len + 64);
        size_t begin = 0, cursor = 0, total = pes_len;
        std::vector<uint8_t> dec;
        while (begin + 4 < total) {
            int sc = 0;
            if (pes_in[begin] == 0 && pes_in[begin + 1] == 0 && pes_in[begin + 2] == 1)
                sc = 3;
            else if (pes_in[begin] == 0 && pes_in[begin + 1] == 0 && pes_in[begin + 2] == 0 &&
                     pes_in[begin + 3] == 1)
                sc = 4;
            else {
                begin++;
                continue;
            }
            if (begin > cursor) chunks.insert(chunks.end(), pes_in + cursor, pes_in + begin);
            size_t hdr = begin + (size_t)sc;
            int nal_type = pes_in[hdr] & 0x1f;
            size_t end = find_nal_start(pes_in, hdr + 1, total);
            chunks.insert(chunks.end(), pes_in + begin, pes_in + hdr);

            if (nal_type == 7 && end > hdr + 2) {
                /* 对齐 UrlPorxy：直接清低 2 bit，不 remove/add EPB */
                size_t nlen = end - hdr;
                scratch.assign(pes_in + hdr, pes_in + end);
                if (scratch.size() > 2) scratch[2] = (uint8_t)(scratch[2] & ~0x3);
                int log2 = p_sps(scratch.data(), scratch.size());
                if (log2 > 0) log2_max_frame_num = log2;
                chunks.insert(chunks.end(), scratch.begin(), scratch.end());
                (void)nlen;
            } else if (nal_type > 0 && nal_type <= 5) {
                d_ni(dec, pes_in + hdr, end - hdr, nal_type);
                chunks.insert(chunks.end(), dec.begin(), dec.end());
            } else {
                chunks.insert(chunks.end(), pes_in + hdr, pes_in + end);
            }
            begin = end;
            cursor = end;
        }
        if (cursor < total) chunks.insert(chunks.end(), pes_in + cursor, pes_in + total);
    }

    void emit_ready(OutBuf &out) {
        size_t drop = 0;
        while (drop < q.size() && q[drop].ready) {
            out.append_pkt(q[drop].data);
            drop++;
        }
        if (drop == 0) return;
        q.erase(q.begin(), q.begin() + (ptrdiff_t)drop);
    }

    void s_pes(const std::vector<uint8_t> &out_pes) {
        size_t k = 0;
        for (size_t i = 0; i < pes_qidx.size(); i++) {
            size_t qi = pes_qidx[i];
            if (qi >= q.size()) continue;
            uint8_t *pkt = q[qi].data;
            int start = payload_off[i];
            size_t avail = (size_t)(kTsPacket - start);
            size_t remain = (k < out_pes.size()) ? (out_pes.size() - k) : 0;
            if (remain >= avail) {
                memcpy(pkt + start, out_pes.data() + k, avail);
                k += avail;
            } else if (remain > 0) {
                /* 对齐 wasm：载荷变短时改 adaptation，避免后半包 0xff 被当成 PES。 */
                size_t adaptation_length = 183 - remain;
                pkt[3] = (uint8_t)((pkt[3] & 0xcf) | 0x30);
                pkt[4] = (uint8_t)adaptation_length;
                if (adaptation_length > 0) {
                    pkt[5] = 0;
                    if (adaptation_length > 1)
                        memset(pkt + 6, 0xff, adaptation_length - 1);
                }
                memcpy(pkt + kTsPacket - remain, out_pes.data() + k, remain);
                k += remain;
            } else {
                pkt[3] = (uint8_t)((pkt[3] & 0xcf) | 0x30);
                pkt[4] = 183;
                pkt[5] = 0;
                memset(pkt + 6, 0xff, 182);
            }
            q[qi].ready = true;
        }
    }

    /* 央视频：无 EPB 时长度不变，在 PES 里原位解，不再整帧拷进 dec/chunks。 */
    void d_u(uint8_t *unit, size_t ulen, int nal_type) {
        if (ulen < 32) return;
        int key_off = nal_type == 5 ? 5 : 1;
        if (key_off + 16 > (int)ulen) return;
        uint32_t k0 = rd_u32le(unit + key_off);
        uint32_t k1 = rd_u32le(unit + key_off + 4);
        uint32_t k2 = rd_u32le(unit + key_off + 8);
        uint32_t k3 = rd_u32le(unit + key_off + 12);
        int stride = s_rbsp(unit, ulen, nal_type);
        for (size_t off = 32; off + 8 <= ulen && ulen - off - 8 > 8; off += (size_t)stride)
            d8k(unit + off, k0, k1, k2, k3);
    }

    bool g_epb(const uint8_t *pes_in, size_t total) const {
        size_t begin = 0;
        while (begin + 4 < total) {
            int sc = 0;
            if (pes_in[begin] == 0 && pes_in[begin + 1] == 0 && pes_in[begin + 2] == 1)
                sc = 3;
            else if (pes_in[begin] == 0 && pes_in[begin + 1] == 0 && pes_in[begin + 2] == 0 &&
                     pes_in[begin + 3] == 1)
                sc = 4;
            else {
                begin++;
                continue;
            }
            size_t hdr = begin + (size_t)sc;
            if (hdr >= total) break;
            int nal_type = pes_in[hdr] & 0x1f;
            size_t end = find_nal_start(pes_in, hdr + 1, total);
            if (nal_type > 0 && nal_type <= 5 && has_epb(pes_in + hdr, end - hdr))
                return true;
            begin = end;
        }
        return false;
    }

    void d_gi(uint8_t *pes_in, size_t total) {
        size_t begin = 0;
        while (begin + 4 < total) {
            int sc = 0;
            if (pes_in[begin] == 0 && pes_in[begin + 1] == 0 && pes_in[begin + 2] == 1)
                sc = 3;
            else if (pes_in[begin] == 0 && pes_in[begin + 1] == 0 && pes_in[begin + 2] == 0 &&
                     pes_in[begin + 3] == 1)
                sc = 4;
            else {
                begin++;
                continue;
            }
            size_t hdr = begin + (size_t)sc;
            if (hdr >= total) break;
            int nal_type = pes_in[hdr] & 0x1f;
            size_t end = find_nal_start(pes_in, hdr + 1, total);
            if (nal_type == 7 && end > hdr + 2) {
                pes_in[hdr + 2] = (uint8_t)(pes_in[hdr + 2] & ~0x3);
                int log2 = p_sps(pes_in + hdr, end - hdr);
                if (log2 > 0) log2_max_frame_num = log2;
            } else if (nal_type > 0 && nal_type <= 5 && end - hdr > 128) {
                d_u(pes_in + hdr, end - hdr, nal_type);
            }
            begin = end;
        }
    }

    /* 央视频加密流 SPS reserved_zero_2bits 为 1/2（对齐 so），其余视为明文；无 SPS 返回 -1。 */
    static int s_mk(const uint8_t *p, size_t n) {
        for (size_t i = 0; i + 5 < n; i++)
            if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1 && (p[i + 3] & 0x1f) == 7) {
                int bits = p[i + 5] & 3;
                return bits == 1 || bits == 2;
            }
        return -1;
    }

    /* from_pusi：下一包 PUSI，PES 完整。否则仅 packet_length 收齐才算完整（H5E 分片 EOF 常截断）。 */
    void flush_pes(OutBuf &out, bool from_pusi) {
        if (!pes.empty() && !pes_qidx.empty()) {
            bool complete = from_pusi || pes_length_complete();
            if (mode == MS_M1_MODE_1 && eflg < 0)
                eflg = s_mk(pes.data(), pes.size());
            if (mode == MS_M1_MODE_1 && eflg == 0) {
                s_pes(pes);
            } else if (mode == MS_M1_MODE_1 && !g_epb(pes.data(), pes.size())) {
                d_gi(pes.data(), pes.size());
                s_pes(pes);
            } else if (mode == MS_M1_MODE_1) {
                r_go(pes.data(), pes.size());
                s_pes(chunks);
            } else {
                auto out_pes = parse_pes(pes.data(), pes.size(), complete);
                s_pes(out_pes);
            }
        }
        pes_qidx.clear();
        payload_off.clear();
        pes.clear();
        ts_count = 0;
        emit_ready(out);
    }

    static bool pes_video_start(const uint8_t *p, int n) {
        return n >= 4 && p[0] == 0 && p[1] == 0 && p[2] == 1 && p[3] >= 0xE0 && p[3] <= 0xEF;
    }

    void pass_packet(const uint8_t pkt[kTsPacket], OutBuf &out) {
        QueuedPkt qp;
        memcpy(qp.data, pkt, kTsPacket);
        qp.ready = true;
        q.push_back(qp);
        emit_ready(out);
    }

    void on_packet(const uint8_t pkt[kTsPacket], OutBuf &out) {
        int pid = ((pkt[1] & 0x1f) << 8) | pkt[2];
        int pusi = (pkt[1] & 0x40) >> 6;
        int afc = (pkt[3] & 0x30) >> 4;
        int payload_index = -1;
        if (afc == 1) {
            payload_index = 4;
        } else if (afc == 3) {
            payload_index = 5 + pkt[4];
            if (payload_index > kTsPacket) payload_index = -1;
        }
        if (video_pid < 0 && pusi == 1 && payload_index >= 0 &&
            pes_video_start(pkt + payload_index, kTsPacket - payload_index))
            video_pid = pid;
        if (pid != video_pid) {
            pass_packet(pkt, out);
            return;
        }
        if (pusi == 1 && ts_count > 0) flush_pes(out, true);
        if (payload_index < 0 || payload_index >= kTsPacket) {
            pass_packet(pkt, out);
            return;
        }

        QueuedPkt qp;
        memcpy(qp.data, pkt, kTsPacket);
        qp.ready = false;
        if (pusi == 1) {
            pes_qidx.clear();
            payload_off.clear();
            pes.clear();
            pes_qidx.push_back(q.size());
            q.push_back(qp);
            payload_off.push_back(payload_index);
            pes.assign(pkt + payload_index, pkt + kTsPacket);
            ts_count = 1;
        } else {
            pes_qidx.push_back(q.size());
            q.push_back(qp);
            payload_off.push_back(payload_index);
            pes.insert(pes.end(), pkt + payload_index, pkt + kTsPacket);
            ts_count++;
        }
    }

    int feed_bytes(const uint8_t *in, size_t in_len, OutBuf &out) {
        if (in && in_len) sync.insert(sync.end(), in, in + in_len);

        size_t i = 0;
        while (i + kTsPacket <= sync.size()) {
            if (sync[i] != 0x47) {
                size_t j = i + 1;
                while (j < sync.size() && sync[j] != 0x47) j++;
                if (j >= sync.size()) {
                    sync.erase(sync.begin(),
                               sync.begin() + (sync.size() > 187 ? sync.size() - 187 : 0));
                    return 0;
                }
                i = j;
                continue;
            }
            on_packet(sync.data() + i, out);
            i += kTsPacket;
        }
        if (i > 0) sync.erase(sync.begin(), sync.begin() + (ptrdiff_t)i);
        return 0;
    }
};

extern "C" {

m1 *ms_m1_create(int mode) {
    ms_guard_ensure();
    if (!ms_guard_ok()) return nullptr;
    m1 *s = new (std::nothrow) m1();
    if (!s) return nullptr;
    if (mode == MS_M1_MODE_1 || mode == MS_M1_MODE_2
        || mode == MS_M1_MODE_0)
        s->mode = mode;
    else
        s->mode = MS_M1_MODE_1;
    s->q.reserve(512);
    s->pes.reserve(256 * 1024);
    s->scratch.reserve(256 * 1024);
    s->chunks.reserve(256 * 1024);
    return s;
}

void ms_m1_destroy(m1 *s) { delete s; }

void ms_m1_set_strides(m1 *s, int idr_stride, int p_stride) {
    if (!s) return;
    s->idr_stride = idr_stride;
    s->p_stride = p_stride;
}

static int finish_out(OutBuf &ob, uint8_t **out, size_t *out_len) {
    if (!out || !out_len) return -1;
    *out = nullptr;
    *out_len = 0;
    if (ob.data.empty()) return 0;
    uint8_t *p = (uint8_t *)malloc(ob.data.size());
    if (!p) return -1;
    memcpy(p, ob.data.data(), ob.data.size());
    *out = p;
    *out_len = ob.data.size();
    return 0;
}

int ms_m1_feed(m1 *s, const uint8_t *in, size_t in_len, uint8_t **out,
                    size_t *out_len) {
    if (!s) return -1;
    OutBuf ob;
    ob.data.reserve(in_len > 0 ? in_len : 4096);
    if (s->feed_bytes(in, in_len, ob) != 0) return -1;
    return finish_out(ob, out, out_len);
}

int ms_m1_flush(m1 *s, uint8_t **out, size_t *out_len) {
    if (!s) return -1;
    OutBuf ob;
    s->feed_bytes(nullptr, 0, ob);
    s->flush_pes(ob, false);
    if (!s->sync.empty()) {
        ob.append(s->sync.data(), s->sync.size());
        s->sync.clear();
    }
    return finish_out(ob, out, out_len);
}

int ms_m1_seg(int mode, const uint8_t *in, size_t in_len,
                           uint8_t **out, size_t *out_len) {
    m1 *s;
    uint8_t *a = nullptr;
    uint8_t *b = nullptr;
    size_t al = 0, bl = 0;
    if (!out || !out_len) return -1;
    *out = nullptr;
    *out_len = 0;
    if (!in || in_len == 0) return 0;
    s = ms_m1_create(mode);
    if (!s) return -1;
    if (ms_m1_feed(s, in, in_len, &a, &al) < 0) {
        ms_m1_destroy(s);
        return -1;
    }
    if (ms_m1_flush(s, &b, &bl) < 0) {
        free(a);
        ms_m1_destroy(s);
        return -1;
    }
    ms_m1_destroy(s);
    if (al + bl == 0) {
        free(a);
        free(b);
        return 0;
    }
    uint8_t *p = (uint8_t *)malloc(al + bl);
    if (!p) {
        free(a);
        free(b);
        return -1;
    }
    if (al) memcpy(p, a, al);
    if (bl) memcpy(p + al, b, bl);
    free(a);
    free(b);
    *out = p;
    *out_len = al + bl;
    return 0;
}

} // extern "C"
