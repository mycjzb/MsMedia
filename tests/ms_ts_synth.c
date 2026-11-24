/* by: Long <mycjzb@gmail.com> */

#include "ms_ts_synth.h"
#include "ms_crc32.h"

#include <stdlib.h>
#include <string.h>

int ms_synth_init(ms_synth_buf *b)
{
    if (b == NULL) {
        return -1;
    }
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
    return 0;
}

void ms_synth_free(ms_synth_buf *b)
{
    if (b == NULL) {
        return;
    }
    free(b->data);
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

static int synth_grow(ms_synth_buf *b, size_t extra)
{
    size_t need = b->len + extra;
    uint8_t *n;
    size_t cap;
    if (need < b->len) {
        return -1;
    }
    if (need <= b->cap) {
        return 0;
    }
    cap = (b->cap == 0) ? 1024u : b->cap;
    while (cap < need) {
        if (cap > ((size_t)-1) / 2u) {
            cap = need;
            break;
        }
        cap *= 2u;
    }
    n = (uint8_t *)realloc(b->data, cap);
    if (n == NULL) {
        return -1;
    }
    b->data = n;
    b->cap = cap;
    return 0;
}

static uint8_t g_cc[0x2000];
static int g_cc_inited;

static uint8_t next_cc(uint16_t pid)
{
    uint8_t c;
    if (!g_cc_inited) {
        memset(g_cc, 0, sizeof(g_cc));
        g_cc_inited = 1;
    }
    if (pid >= 0x2000) {
        pid = 0x1FFF;
    }
    c = g_cc[pid] & 0x0F;
    g_cc[pid] = (uint8_t)((c + 1) & 0x0F);
    return c;
}

void ms_synth_reset_cc(void)
{
    memset(g_cc, 0, sizeof(g_cc));
    g_cc_inited = 1;
}

static int append_unit(ms_synth_buf *b, const uint8_t ts188[188], uint16_t packet_size)
{
    if (packet_size != 188 && packet_size != 192) {
        return -1;
    }
    if (synth_grow(b, packet_size) != 0) {
        return -1;
    }
    if (packet_size == 192) {
        memset(b->data + b->len, 0, 4);
        memcpy(b->data + b->len + 4, ts188, 188);
    } else {
        memcpy(b->data + b->len, ts188, 188);
    }
    b->len += packet_size;
    return 0;
}

static int write_psi_packets(ms_synth_buf *b, uint16_t pid,
                             const uint8_t *section, size_t slen,
                             uint16_t packet_size)
{
    size_t off = 0;
    int first = 1;
    while (first || off < slen) {
        uint8_t pkt[188];
        size_t room;
        size_t chunk;
        uint8_t cc = next_cc(pid);
        memset(pkt, 0xFF, sizeof(pkt));
        pkt[0] = 0x47;
        pkt[1] = (uint8_t)((first ? 0x40 : 0x00) | ((pid >> 8) & 0x1F));
        pkt[2] = (uint8_t)(pid & 0xFF);
        pkt[3] = (uint8_t)(0x10 | (cc & 0x0F));
        if (first) {
            pkt[4] = 0x00;
            room = 183;
            chunk = (slen - off < room) ? (slen - off) : room;
            memcpy(pkt + 5, section + off, chunk);
            off += chunk;
            first = 0;
        } else {
            room = 184;
            chunk = (slen - off < room) ? (slen - off) : room;
            memcpy(pkt + 4, section + off, chunk);
            off += chunk;
        }
        if (append_unit(b, pkt, packet_size) != 0) {
            return -1;
        }
        if (off >= slen) {
            break;
        }
    }
    return 0;
}

static void finish_section(uint8_t *sec, size_t *io_len)
{
    size_t body = *io_len; /* 已写入但还没有 length 和 CRC，从字节 3 起 */
    uint16_t slen;
    uint32_t crc;
    /* 布局：table_id, 2 length, payload..., crc */
    slen = (uint16_t)(body - 3 + 4);
    sec[1] = (uint8_t)(0xB0 | ((slen >> 8) & 0x0F));
    sec[2] = (uint8_t)(slen & 0xFF);
    crc = ms_crc32_mpeg2(sec, body);
    sec[body] = (uint8_t)(crc >> 24);
    sec[body + 1] = (uint8_t)(crc >> 16);
    sec[body + 2] = (uint8_t)(crc >> 8);
    sec[body + 3] = (uint8_t)crc;
    *io_len = body + 4;
}

int ms_synth_pat_pmt(ms_synth_buf *b,
                     uint16_t program_number,
                     uint16_t pmt_pid,
                     const ms_synth_stream *streams,
                     int nstreams,
                     int null_packets,
                     uint16_t packet_size)
{
    return ms_synth_pat_pmt_version(b, program_number, pmt_pid, streams,
                                    nstreams, null_packets, packet_size, 0);
}

int ms_synth_pat_pmt_version(ms_synth_buf *b,
                             uint16_t program_number,
                             uint16_t pmt_pid,
                             const ms_synth_stream *streams,
                             int nstreams,
                             int null_packets,
                             uint16_t packet_size,
                             unsigned version)
{
    return ms_synth_pat_pmt_info(b, program_number, pmt_pid, streams, nstreams,
                                 null_packets, packet_size, version, NULL, 0);
}

int ms_synth_pat_pmt_info(ms_synth_buf *b,
                          uint16_t program_number,
                          uint16_t pmt_pid,
                          const ms_synth_stream *streams,
                          int nstreams,
                          int null_packets,
                          uint16_t packet_size,
                          unsigned version,
                          const uint8_t *program_info,
                          uint16_t program_info_size)
{
    uint8_t pat[32];
    uint8_t pmt[256];
    size_t plen;
    int i;

    if (b == NULL || (nstreams > 0 && streams == NULL) || nstreams < 0) {
        return -1;
    }
    if (nstreams > MS_SYNTH_MAX) {
        return -1;
    }

    memset(pat, 0, sizeof(pat));
    pat[0] = 0x00;
    /* length 稍后填 */
    pat[3] = 0x00;
    pat[4] = 0x01; /* transport_stream_id */
    pat[5] = (uint8_t)(0xC1 | ((version & 0x1F) << 1)); /* reserved+ver+current */
    pat[6] = 0x00;
    pat[7] = 0x00;
    pat[8] = (uint8_t)(program_number >> 8);
    pat[9] = (uint8_t)program_number;
    pat[10] = (uint8_t)(0xE0 | ((pmt_pid >> 8) & 0x1F));
    pat[11] = (uint8_t)pmt_pid;
    plen = 12;
    finish_section(pat, &plen);
    if (write_psi_packets(b, 0, pat, plen, packet_size) != 0) {
        return -1;
    }

    memset(pmt, 0, sizeof(pmt));
    pmt[0] = 0x02;
    pmt[3] = (uint8_t)(program_number >> 8);
    pmt[4] = (uint8_t)program_number;
    pmt[5] = (uint8_t)(0xC1 | ((version & 0x1F) << 1));
    pmt[6] = 0x00;
    pmt[7] = 0x00;
    /* PCR_PID = 第一个流 PID，或 0x1FFF */
    {
        uint16_t pcr = (nstreams > 0) ? streams[0].pid : 0x1FFF;
        pmt[8] = (uint8_t)(0xE0 | ((pcr >> 8) & 0x1F));
        pmt[9] = (uint8_t)pcr;
    }
    if (program_info != NULL && program_info_size > 0) {
        if (12u + (unsigned)program_info_size + 16u >= sizeof(pmt)) {
            return -1;
        }
        pmt[10] = (uint8_t)(0xF0 | ((program_info_size >> 8) & 0x0F));
        pmt[11] = (uint8_t)program_info_size;
        memcpy(pmt + 12, program_info, program_info_size);
        plen = 12u + (size_t)program_info_size;
    } else {
        pmt[10] = 0xF0;
        pmt[11] = 0x00;
        plen = 12;
    }
    (void)program_info;
    for (i = 0; i < nstreams; i++) {
        uint16_t es_len = 0;
        if (plen + 5 + 8 + 16 >= sizeof(pmt)) {
            return -1;
        }
        pmt[plen++] = streams[i].stream_type;
        pmt[plen++] = (uint8_t)(0xE0 | ((streams[i].pid >> 8) & 0x1F));
        pmt[plen++] = (uint8_t)streams[i].pid;
        if (streams[i].language[0] != 0) {
            es_len = 6;
        }
        if (streams[i].es_descriptor != NULL && streams[i].es_descriptor_size > 0) {
            es_len = (uint16_t)(es_len + streams[i].es_descriptor_size);
        }
        pmt[plen++] = (uint8_t)(0xF0 | ((es_len >> 8) & 0x0F));
        pmt[plen++] = (uint8_t)es_len;
        if (streams[i].language[0] != 0) {
            pmt[plen++] = 0x0A;
            pmt[plen++] = 0x04;
            pmt[plen++] = (uint8_t)streams[i].language[0];
            pmt[plen++] = (uint8_t)streams[i].language[1];
            pmt[plen++] = (uint8_t)streams[i].language[2];
            pmt[plen++] = 0x00;
        }
        if (streams[i].es_descriptor != NULL && streams[i].es_descriptor_size > 0) {
            if (plen + streams[i].es_descriptor_size >= sizeof(pmt)) {
                return -1;
            }
            memcpy(pmt + plen, streams[i].es_descriptor, streams[i].es_descriptor_size);
            plen += streams[i].es_descriptor_size;
        }
    }
    finish_section(pmt, &plen);
    if (write_psi_packets(b, pmt_pid, pmt, plen, packet_size) != 0) {
        return -1;
    }

    for (i = 0; i < null_packets; i++) {
        uint8_t pkt[188];
        uint8_t cc = next_cc(0x1FFF);
        memset(pkt, 0xFF, sizeof(pkt));
        pkt[0] = 0x47;
        pkt[1] = 0x1F;
        pkt[2] = 0xFF;
        pkt[3] = (uint8_t)(0x10 | (cc & 0x0F));
        if (append_unit(b, pkt, packet_size) != 0) {
            return -1;
        }
    }
    return 0;
}

static void encode_pts(uint8_t *out, unsigned prefix, uint64_t pts)
{
    pts &= 0x1FFFFFFFFULL;
    out[0] = (uint8_t)((prefix << 4) | (unsigned)(((pts >> 30) & 0x07) << 1) | 0x01);
    out[1] = (uint8_t)((pts >> 22) & 0xFF);
    out[2] = (uint8_t)((((pts >> 15) & 0x7F) << 1) | 0x01);
    out[3] = (uint8_t)((pts >> 7) & 0xFF);
    out[4] = (uint8_t)(((pts & 0x7F) << 1) | 0x01);
}

static int synth_pes_write(ms_synth_buf *b,
                           uint16_t pid,
                           uint8_t stream_id,
                           int has_pts,
                           uint64_t pts,
                           const uint8_t *payload,
                           size_t payload_len,
                           uint16_t packet_size,
                           int unbounded)
{
    uint8_t header[32];
    size_t hlen = 6;
    size_t total;
    size_t off = 0;
    int first = 1;
    unsigned pes_len;

    if (b == NULL || (payload_len > 0 && payload == NULL)) {
        return -1;
    }
    header[0] = 0x00;
    header[1] = 0x00;
    header[2] = 0x01;
    header[3] = stream_id;
    if (has_pts) {
        header[6] = 0x80;
        header[7] = 0x80;
        header[8] = 0x05;
        encode_pts(header + 9, 0x2, pts);
        hlen = 14;
        pes_len = (unsigned)((hlen - 6) + payload_len);
    } else {
        header[6] = 0x80;
        header[7] = 0x00;
        header[8] = 0x00;
        hlen = 9;
        pes_len = (unsigned)((hlen - 6) + payload_len);
    }
    if (pes_len > 0xFFFF) {
        return -1;
    }
    if (unbounded) {
        header[4] = 0;
        header[5] = 0;
    } else {
        header[4] = (uint8_t)(pes_len >> 8);
        header[5] = (uint8_t)pes_len;
    }
    total = hlen + payload_len;

    while (off < total) {
        uint8_t pkt[188];
        size_t room;
        size_t chunk;
        size_t stuff = 0;
        size_t pay_off;
        uint8_t cc = next_cc(pid);
        const uint8_t *src;
        memset(pkt, 0xFF, sizeof(pkt));
        pkt[0] = 0x47;
        pkt[1] = (uint8_t)((first ? 0x40 : 0x00) | ((pid >> 8) & 0x1F));
        pkt[2] = (uint8_t)(pid & 0xFF);
        room = 184;
        chunk = ((total - off) < room) ? (total - off) : room;
        /* 最后一包若不满 184，必须用 adaptation stuffing，不能把 0xFF 当 PES 载荷。 */
        if (chunk < room) {
            stuff = room - chunk;
        }
        if (stuff > 0) {
            pkt[3] = (uint8_t)(0x30 | (cc & 0x0F));
            pkt[4] = (uint8_t)(stuff - 1u);
            if (stuff >= 2u) {
                pkt[5] = 0x00; /* 无 PCR/flags，其余已是 0xFF */
            }
            pay_off = 4u + stuff;
        } else {
            pkt[3] = (uint8_t)(0x10 | (cc & 0x0F));
            pay_off = 4;
        }
        if (off < hlen) {
            size_t from_h = hlen - off;
            if (from_h > chunk) {
                from_h = chunk;
            }
            memcpy(pkt + pay_off, header + off, from_h);
            if (from_h < chunk && payload != NULL) {
                memcpy(pkt + pay_off + from_h, payload, chunk - from_h);
            }
        } else {
            src = payload + (off - hlen);
            memcpy(pkt + pay_off, src, chunk);
        }
        if (append_unit(b, pkt, packet_size) != 0) {
            return -1;
        }
        off += chunk;
        first = 0;
    }
    return 0;
}

int ms_synth_pes(ms_synth_buf *b,
                 uint16_t pid,
                 uint8_t stream_id,
                 int has_pts,
                 uint64_t pts,
                 const uint8_t *payload,
                 size_t payload_len,
                 uint16_t packet_size)
{
    return synth_pes_write(b, pid, stream_id, has_pts, pts, payload,
                           payload_len, packet_size, 0);
}

int ms_synth_pes_unbounded(ms_synth_buf *b,
                           uint16_t pid,
                           uint8_t stream_id,
                           int has_pts,
                           uint64_t pts,
                           const uint8_t *payload,
                           size_t payload_len,
                           uint16_t packet_size)
{
    return synth_pes_write(b, pid, stream_id, has_pts, pts, payload,
                           payload_len, packet_size, 1);
}

int ms_synth_prepend(ms_synth_buf *b, const uint8_t *pfx, size_t pfx_len)
{
    if (b == NULL) {
        return -1;
    }
    if (pfx_len == 0) {
        return 0;
    }
    if (pfx == NULL) {
        return -1;
    }
    if (synth_grow(b, pfx_len) != 0) {
        return -1;
    }
    memmove(b->data + pfx_len, b->data, b->len);
    memcpy(b->data, pfx, pfx_len);
    b->len += pfx_len;
    return 0;
}

void ms_synth_xor(ms_synth_buf *b, size_t off, uint8_t mask)
{
    if (b == NULL || b->data == NULL || off >= b->len) {
        return;
    }
    b->data[off] ^= mask;
}

int ms_synth_adts_frame(uint8_t *out, size_t cap, size_t *out_len,
                        int profile, int sfi, int channels,
                        const uint8_t *raw, size_t raw_len)
{
    size_t flen;
    if (out_len == NULL) {
        return -1;
    }
    *out_len = 0;
    if (raw_len > 0 && raw == NULL) {
        return -1;
    }
    if (profile < 0 || profile > 3 || sfi < 0 || sfi > 12 ||
        channels <= 0 || channels > 7) {
        return -1;
    }
    if (raw_len > ((size_t)-1) - 7u) {
        return -1;
    }
    flen = 7u + raw_len;
    if (flen > 0x1FFF || flen > cap || out == NULL) {
        return -1;
    }
    /* MPEG-4 ID=0, layer=0, protection_absent=1 */
    out[0] = 0xFF;
    out[1] = 0xF1;
    out[2] = (uint8_t)(((profile & 0x3) << 6) | ((sfi & 0xF) << 2) | ((channels >> 2) & 0x1));
    out[3] = (uint8_t)(((channels & 0x3) << 6) | ((flen >> 11) & 0x3));
    out[4] = (uint8_t)((flen >> 3) & 0xFF);
    out[5] = (uint8_t)(((flen & 0x7) << 5) | 0x1F);
    out[6] = 0xFC;
    if (raw_len > 0) {
        memcpy(out + 7, raw, raw_len);
    }
    *out_len = flen;
    return 0;
}

typedef struct {
    uint8_t *p;
    size_t cap;
    size_t bit;
} synth_bw;

static int bw_put(synth_bw *b, unsigned bits, unsigned v)
{
    unsigned i;
    if (b == NULL || bits == 0 || bits > 32) {
        return -1;
    }
    for (i = 0; i < bits; i++) {
        unsigned bit = (v >> (bits - 1u - i)) & 1u;
        size_t pos = b->bit;
        size_t by = pos / 8u;
        if (by >= b->cap) {
            return -1;
        }
        if ((pos % 8u) == 0) {
            b->p[by] = 0;
        }
        if (bit) {
            b->p[by] = (uint8_t)(b->p[by] | (uint8_t)(0x80u >> (pos % 8u)));
        }
        b->bit++;
    }
    return 0;
}

static int bw_ue(synth_bw *b, unsigned v)
{
    unsigned tmp = v + 1u;
    unsigned lz = 0;
    unsigned x = tmp;
    while (x > 1u) {
        x >>= 1;
        lz++;
    }
    if (lz > 0 && bw_put(b, lz, 0) != 0) {
        return -1;
    }
    if (bw_put(b, 1, 1) != 0) {
        return -1;
    }
    if (lz > 0 && bw_put(b, lz, v + 1u - (1u << lz)) != 0) {
        return -1;
    }
    return 0;
}

static int bw_se(synth_bw *b, int v)
{
    unsigned k;
    if (v > 0) {
        k = (unsigned)v * 2u - 1u;
    } else {
        k = (unsigned)(-v) * 2u;
    }
    return bw_ue(b, k);
}

static int epb_copy(uint8_t *out, size_t cap, size_t *n,
                    const uint8_t *rbsp, size_t rbsp_len)
{
    size_t i;
    size_t z = 0;
    if (n == NULL) {
        return -1;
    }
    for (i = 0; i < rbsp_len; i++) {
        if (*n >= cap) {
            return -1;
        }
        if (z >= 2 && rbsp[i] <= 3u) {
            out[(*n)++] = 0x03;
            if (*n >= cap) {
                return -1;
            }
            z = 0;
        }
        out[(*n)++] = rbsp[i];
        if (rbsp[i] == 0) {
            z++;
        } else {
            z = 0;
        }
    }
    return 0;
}

int ms_synth_annexb_nal(uint8_t *out, size_t cap, size_t *out_len,
                        int sc_len, uint8_t nal_hdr,
                        const uint8_t *rbsp, size_t rbsp_len)
{
    size_t n = 0;
    if (out_len == NULL) {
        return -1;
    }
    *out_len = 0;
    if (sc_len != 3 && sc_len != 4) {
        return -1;
    }
    if (rbsp_len > 0 && rbsp == NULL) {
        return -1;
    }
    if (out == NULL || cap < (size_t)sc_len + 1u) {
        return -1;
    }
    if (sc_len == 4) {
        out[n++] = 0;
    }
    out[n++] = 0;
    out[n++] = 0;
    out[n++] = 1;
    out[n++] = nal_hdr;
    if (epb_copy(out, cap, &n, rbsp, rbsp_len) != 0) {
        return -1;
    }
    *out_len = n;
    return 0;
}

int ms_synth_h264_sps(uint8_t *out, size_t cap, size_t *out_len,
                      int sc_len, int width, int height, int frame_mbs_only)
{
    uint8_t bits[64];
    uint8_t rbsp[80];
    synth_bw bw;
    size_t nbytes;
    unsigned wmbs, hmbs;

    if (width < 16 || height < 16 || (width % 16) != 0 || (height % 16) != 0) {
        return -1;
    }
    wmbs = (unsigned)(width / 16) - 1u;
    if (frame_mbs_only) {
        hmbs = (unsigned)(height / 16) - 1u;
    } else {
        /* 场模式 map units 为帧高的一半。 */
        if ((height / 16) < 2 || ((height / 16) % 2) != 0) {
            return -1;
        }
        hmbs = (unsigned)(height / 16) / 2u - 1u;
    }
    memset(bits, 0, sizeof(bits));
    memset(&bw, 0, sizeof(bw));
    bw.p = bits;
    bw.cap = sizeof(bits);
    if (bw_put(&bw, 8, 66) != 0 || /* profile Baseline */
        bw_put(&bw, 8, 0) != 0 ||
        bw_put(&bw, 8, 30) != 0 || /* level 3.0 */
        bw_ue(&bw, 0) != 0 ||      /* sps_id */
        bw_ue(&bw, 0) != 0 ||      /* log2_max_frame_num_minus4 */
        bw_ue(&bw, 2) != 0 ||      /* poc_type 2，避免再写 POC 字段 */
        bw_ue(&bw, 1) != 0 ||      /* max_num_ref_frames */
        bw_put(&bw, 1, 0) != 0 ||  /* gaps */
        bw_ue(&bw, wmbs) != 0 ||
        bw_ue(&bw, hmbs) != 0 ||
        bw_put(&bw, 1, frame_mbs_only ? 1u : 0u) != 0) {
        return -1;
    }
    if (!frame_mbs_only) {
        if (bw_put(&bw, 1, 0) != 0) { /* mb_adaptive_frame_field_flag = 0 (PAFF) */
            return -1;
        }
    }
    if (bw_put(&bw, 1, 1) != 0 || /* direct_8x8 */
        bw_put(&bw, 1, 0) != 0 || /* cropping */
        bw_put(&bw, 1, 0) != 0 || /* vui */
        bw_put(&bw, 1, 1) != 0) { /* rbsp_trailing_bit */
        return -1;
    }
    nbytes = (bw.bit + 7u) / 8u;
    memcpy(rbsp, bits, nbytes);
    return ms_synth_annexb_nal(out, cap, out_len, sc_len, 0x67, rbsp, nbytes);
}

int ms_synth_h264_pps(uint8_t *out, size_t cap, size_t *out_len, int sc_len)
{
    uint8_t bits[32];
    synth_bw bw;
    size_t nbytes;

    memset(bits, 0, sizeof(bits));
    memset(&bw, 0, sizeof(bw));
    bw.p = bits;
    bw.cap = sizeof(bits);
    if (bw_ue(&bw, 0) != 0 || bw_ue(&bw, 0) != 0 ||
        bw_put(&bw, 1, 0) != 0 || bw_put(&bw, 1, 0) != 0 ||
        bw_ue(&bw, 0) != 0 || bw_ue(&bw, 0) != 0 || bw_ue(&bw, 0) != 0 ||
        bw_put(&bw, 2, 0) != 0 ||
        bw_se(&bw, 0) != 0 || bw_se(&bw, 0) != 0 || bw_se(&bw, 0) != 0 ||
        bw_put(&bw, 1, 0) != 0 || bw_put(&bw, 1, 0) != 0 ||
        bw_put(&bw, 1, 0) != 0 || bw_put(&bw, 1, 1) != 0) {
        return -1;
    }
    nbytes = (bw.bit + 7u) / 8u;
    return ms_synth_annexb_nal(out, cap, out_len, sc_len, 0x68, bits, nbytes);
}

int ms_synth_h264_aud(uint8_t *out, size_t cap, size_t *out_len, int sc_len)
{
    uint8_t rbsp[1];
    /* primary_pic_type=0，加上 rbsp_trailing → 0x10 */
    rbsp[0] = 0x10;
    return ms_synth_annexb_nal(out, cap, out_len, sc_len, 0x09, rbsp, 1);
}

int ms_synth_h264_slice(uint8_t *out, size_t cap, size_t *out_len,
                        int sc_len, int idr, const uint8_t *raw, size_t raw_len)
{
    uint8_t hdr = idr ? (uint8_t)0x65 : (uint8_t)0x41;
    uint8_t tmp[16];
    const uint8_t *rbsp = raw;
    size_t n = raw_len;
    if (raw_len == 0) {
        /* 最小条带头占位：first_mb=0, slice_type=2(I) 或 7(I), pps=0, 再填一点。 */
        tmp[0] = 0x88;
        tmp[1] = 0x80;
        rbsp = tmp;
        n = 2;
    }
    return ms_synth_annexb_nal(out, cap, out_len, sc_len, hdr, rbsp, n);
}

int ms_synth_h264_sps_epb(uint8_t *out, size_t cap, size_t *out_len, int sc_len)
{
    uint8_t bits[64];
    synth_bw bw;
    size_t nbytes;

    /* 对齐后写入 ue(65535) 使 RBSP 出现连续三个 0x00，从而插入 00 00 03。 */
    memset(bits, 0, sizeof(bits));
    memset(&bw, 0, sizeof(bw));
    bw.p = bits;
    bw.cap = sizeof(bits);
    if (bw_put(&bw, 8, 66) != 0 ||
        bw_put(&bw, 8, 0) != 0 ||
        bw_put(&bw, 8, 30) != 0 ||
        bw_ue(&bw, 0) != 0 ||
        bw_ue(&bw, 0) != 0 ||
        bw_ue(&bw, 0) != 0 ||
        bw_ue(&bw, 0) != 0 ||
        bw_ue(&bw, 1) != 0 ||
        bw_put(&bw, 1, 0) != 0 ||
        bw_ue(&bw, 0xFFFFFFu) != 0 ||
        bw_ue(&bw, 0) != 0 ||
        bw_put(&bw, 1, 1) != 0 ||
        bw_put(&bw, 1, 1) != 0 ||
        bw_put(&bw, 1, 0) != 0 ||
        bw_put(&bw, 1, 0) != 0 ||
        bw_put(&bw, 1, 1) != 0) {
        return -1;
    }
    nbytes = (bw.bit + 7u) / 8u;
    return ms_synth_annexb_nal(out, cap, out_len, sc_len, 0x67, bits, nbytes);
}

int ms_synth_h264_sps_ex(uint8_t *out, size_t cap, size_t *out_len,
                         int sc_len, int width, int height, int frame_mbs_only,
                         unsigned sps_id, unsigned poc_type,
                         unsigned log2_max_frame_num_minus4,
                         unsigned log2_max_poc_lsb_minus4,
                         int delta_always_zero)
{
    uint8_t bits[128];
    synth_bw bw;
    size_t nbytes;
    unsigned wmbs, hmbs;

    if (width < 16 || height < 16 || (width % 16) != 0 || (height % 16) != 0) {
        return -1;
    }
    if (sps_id > 31u || poc_type > 2u ||
        log2_max_frame_num_minus4 > 12u || log2_max_poc_lsb_minus4 > 12u) {
        return -1;
    }
    wmbs = (unsigned)(width / 16) - 1u;
    if (frame_mbs_only) {
        hmbs = (unsigned)(height / 16) - 1u;
    } else {
        if ((height / 16) < 2 || ((height / 16) % 2) != 0) {
            return -1;
        }
        hmbs = (unsigned)(height / 16) / 2u - 1u;
    }
    memset(bits, 0, sizeof(bits));
    memset(&bw, 0, sizeof(bw));
    bw.p = bits;
    bw.cap = sizeof(bits);
    if (bw_put(&bw, 8, 66) != 0 ||
        bw_put(&bw, 8, 0) != 0 ||
        bw_put(&bw, 8, 30) != 0 ||
        bw_ue(&bw, sps_id) != 0 ||
        bw_ue(&bw, log2_max_frame_num_minus4) != 0 ||
        bw_ue(&bw, poc_type) != 0) {
        return -1;
    }
    if (poc_type == 0) {
        if (bw_ue(&bw, log2_max_poc_lsb_minus4) != 0) {
            return -1;
        }
    } else if (poc_type == 1) {
        if (bw_put(&bw, 1, delta_always_zero ? 1u : 0u) != 0 ||
            bw_se(&bw, 0) != 0 ||
            bw_se(&bw, 0) != 0 ||
            bw_ue(&bw, 0) != 0) {
            return -1;
        }
    }
    if (bw_ue(&bw, 1) != 0 ||
        bw_put(&bw, 1, 0) != 0 ||
        bw_ue(&bw, wmbs) != 0 ||
        bw_ue(&bw, hmbs) != 0 ||
        bw_put(&bw, 1, frame_mbs_only ? 1u : 0u) != 0) {
        return -1;
    }
    if (!frame_mbs_only) {
        if (bw_put(&bw, 1, 0) != 0) {
            return -1;
        }
    }
    if (bw_put(&bw, 1, 1) != 0 ||
        bw_put(&bw, 1, 0) != 0 ||
        bw_put(&bw, 1, 0) != 0 ||
        bw_put(&bw, 1, 1) != 0) {
        return -1;
    }
    nbytes = (bw.bit + 7u) / 8u;
    return ms_synth_annexb_nal(out, cap, out_len, sc_len, 0x67, bits, nbytes);
}

int ms_synth_h264_pps_ex(uint8_t *out, size_t cap, size_t *out_len, int sc_len,
                         unsigned pps_id, unsigned sps_id,
                         int bottom_field, unsigned slice_groups_minus1)
{
    uint8_t bits[64];
    synth_bw bw;
    size_t nbytes;

    if (pps_id > 255u || sps_id > 31u) {
        return -1;
    }
    memset(bits, 0, sizeof(bits));
    memset(&bw, 0, sizeof(bw));
    bw.p = bits;
    bw.cap = sizeof(bits);
    if (bw_ue(&bw, pps_id) != 0 || bw_ue(&bw, sps_id) != 0 ||
        bw_put(&bw, 1, 0) != 0 ||
        bw_put(&bw, 1, bottom_field ? 1u : 0u) != 0 ||
        bw_ue(&bw, slice_groups_minus1) != 0) {
        return -1;
    }
    if (slice_groups_minus1 != 0) {
        /* FMO 样本到此为止即可，组帧侧在 num_slice_groups_minus1 处拒绝。 */
        if (bw_put(&bw, 1, 1) != 0) {
            return -1;
        }
        nbytes = (bw.bit + 7u) / 8u;
        return ms_synth_annexb_nal(out, cap, out_len, sc_len, 0x68, bits, nbytes);
    }
    if (bw_ue(&bw, 0) != 0 || bw_ue(&bw, 0) != 0 ||
        bw_put(&bw, 2, 0) != 0 ||
        bw_se(&bw, 0) != 0 || bw_se(&bw, 0) != 0 || bw_se(&bw, 0) != 0 ||
        bw_put(&bw, 1, 0) != 0 || bw_put(&bw, 1, 0) != 0 ||
        bw_put(&bw, 1, 0) != 0 || bw_put(&bw, 1, 1) != 0) {
        return -1;
    }
    nbytes = (bw.bit + 7u) / 8u;
    return ms_synth_annexb_nal(out, cap, out_len, sc_len, 0x68, bits, nbytes);
}

int ms_synth_h264_slice_hdr(uint8_t *out, size_t cap, size_t *out_len,
                            const ms_synth_h264_slice_opts *opt)
{
    uint8_t bits[64];
    synth_bw bw;
    size_t nbytes;
    int sc_len;
    int idr;
    int ref;
    unsigned fn_bits, poc_bits;
    uint8_t hdr;

    if (opt == NULL) {
        return -1;
    }
    sc_len = opt->sc_len;
    idr = opt->idr;
    ref = opt->nal_ref_idc;
    if (ref < 0) {
        ref = idr ? 3 : 2;
    }
    if (ref > 3) {
        return -1;
    }
    if (opt->log2_max_frame_num_minus4 > 12u ||
        opt->log2_max_poc_lsb_minus4 > 12u ||
        opt->poc_type > 2u) {
        return -1;
    }
    memset(bits, 0, sizeof(bits));
    memset(&bw, 0, sizeof(bw));
    bw.p = bits;
    bw.cap = sizeof(bits);
    fn_bits = opt->log2_max_frame_num_minus4 + 4u;
    if (bw_ue(&bw, opt->first_mb) != 0 ||
        bw_ue(&bw, opt->slice_type) != 0 ||
        bw_ue(&bw, opt->pps_id) != 0 ||
        bw_put(&bw, fn_bits, opt->frame_num) != 0) {
        return -1;
    }
    if (idr) {
        if (bw_ue(&bw, opt->idr_pic_id) != 0) {
            return -1;
        }
    }
    if (opt->poc_type == 0) {
        poc_bits = opt->log2_max_poc_lsb_minus4 + 4u;
        if (bw_put(&bw, poc_bits, opt->poc_lsb) != 0) {
            return -1;
        }
        if (opt->write_delta_bottom) {
            if (bw_se(&bw, opt->delta_bottom) != 0) {
                return -1;
            }
        }
    } else if (opt->poc_type == 1) {
        if (opt->write_delta0) {
            if (bw_se(&bw, opt->delta0) != 0) {
                return -1;
            }
        }
        if (opt->write_delta1) {
            if (bw_se(&bw, opt->delta1) != 0) {
                return -1;
            }
        }
    }
    if (bw_put(&bw, 1, 1) != 0) { /* trailing */
        return -1;
    }
    nbytes = (bw.bit + 7u) / 8u;
    hdr = (uint8_t)(((unsigned)ref << 5) | (idr ? 5u : 1u));
    return ms_synth_annexb_nal(out, cap, out_len, sc_len, hdr, bits, nbytes);
}

void ms_synth_set_next_cc(uint16_t pid, unsigned cc)
{
    if (!g_cc_inited) {
        memset(g_cc, 0, sizeof(g_cc));
        g_cc_inited = 1;
    }
    if (pid >= 0x2000) {
        pid = 0x1FFF;
    }
    g_cc[pid] = (uint8_t)(cc & 0x0F);
}

unsigned ms_synth_peek_cc(uint16_t pid)
{
    if (!g_cc_inited) {
        return 0;
    }
    if (pid >= 0x2000) {
        pid = 0x1FFF;
    }
    return (unsigned)(g_cc[pid] & 0x0F);
}

int ms_synth_append_188(ms_synth_buf *b, const uint8_t pkt[188], uint16_t packet_size)
{
    return append_unit(b, pkt, packet_size);
}

int ms_synth_delete_unit(ms_synth_buf *b, size_t index, uint16_t packet_size)
{
    size_t off;
    size_t n;
    if (b == NULL || packet_size == 0 || (b->len % packet_size) != 0) {
        return -1;
    }
    n = b->len / packet_size;
    if (index >= n) {
        return -1;
    }
    off = index * (size_t)packet_size;
    memmove(b->data + off, b->data + off + packet_size,
            b->len - off - packet_size);
    b->len -= packet_size;
    return 0;
}

int ms_synth_insert_unit(ms_synth_buf *b, size_t index, const uint8_t pkt[188],
                         uint16_t packet_size)
{
    size_t off;
    size_t n;
    if (b == NULL || pkt == NULL || packet_size == 0 ||
        (b->len % packet_size) != 0) {
        return -1;
    }
    n = b->len / packet_size;
    if (index > n) {
        return -1;
    }
    if (synth_grow(b, packet_size) != 0) {
        return -1;
    }
    off = index * (size_t)packet_size;
    memmove(b->data + off + packet_size, b->data + off, b->len - off);
    if (packet_size == 192) {
        memset(b->data + off, 0, 4);
        memcpy(b->data + off + 4, pkt, 188);
    } else {
        memcpy(b->data + off, pkt, 188);
    }
    b->len += packet_size;
    return 0;
}

void ms_synth_pkt_set_cc(uint8_t *pkt, unsigned cc)
{
    if (pkt == NULL) {
        return;
    }
    pkt[3] = (uint8_t)((pkt[3] & 0xF0) | (cc & 0x0F));
}

void ms_synth_pkt_set_tei(uint8_t *pkt, int tei)
{
    if (pkt == NULL) {
        return;
    }
    if (tei) {
        pkt[1] |= 0x80;
    } else {
        pkt[1] = (uint8_t)(pkt[1] & 0x7F);
    }
}

void ms_synth_pkt_set_scrambling(uint8_t *pkt, unsigned scr)
{
    if (pkt == NULL) {
        return;
    }
    pkt[3] = (uint8_t)((pkt[3] & 0x3F) | ((scr & 0x3u) << 6));
}

int ms_synth_pkt_add_disc_indicator(uint8_t *pkt)
{
    unsigned adapt;
    if (pkt == NULL) {
        return -1;
    }
    adapt = (pkt[3] >> 4) & 0x3u;
    if (adapt == 1) {
        memmove(pkt + 6, pkt + 4, 182);
        pkt[3] = (uint8_t)((pkt[3] & 0x0F) | 0x30);
        pkt[4] = 1;
        pkt[5] = 0x80;
        return 0;
    }
    if ((adapt == 2 || adapt == 3) && pkt[4] > 0) {
        pkt[5] |= 0x80;
        return 0;
    }
    return -1;
}

int ms_synth_adapt_only(ms_synth_buf *b, uint16_t pid, unsigned cc, int disc_ind,
                        uint16_t packet_size)
{
    uint8_t pkt[188];
    memset(pkt, 0xFF, sizeof(pkt));
    pkt[0] = 0x47;
    pkt[1] = (uint8_t)((pid >> 8) & 0x1F);
    pkt[2] = (uint8_t)(pid & 0xFF);
    pkt[3] = (uint8_t)(0x20 | (cc & 0x0F));
    pkt[4] = 183;
    pkt[5] = disc_ind ? 0x80 : 0x00;
    return append_unit(b, pkt, packet_size);
}
