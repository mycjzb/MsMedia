/*
 * 链接 libmsmedia.so 的公共 ABI 冒烟：验证动态导出可解析，并走一条与静态库相同的
 * 经典 FLV 出帧短路径。即便在 debug 目录中，本程序也不加 ASan/UBSan；
 * sanitizer 覆盖以链 .a 的 test_core 为准，不能把未加 sanitizer 的 .so 说成已覆盖。
 * 非正式验收，不链 FFmpeg。
 * by: Long <mycjzb@gmail.com>
 */
#include "msmedia/msmedia.h"

#include <stdio.h>
#include <string.h>
#include <stdint.h>

/* 与 test_core 相同的最短合法 avcC + 长度前缀样本，仅作 ABI 路径冒烟。 */
static const uint8_t k_avcc[] = {
    0x01, 0x64, 0x00, 0x1e, 0xff,
    0xe1, 0x00, 0x0b,
    0x67, 0x64, 0x00, 0x1e, 0xac, 0xc8, 0x40, 0x78, 0x9a, 0x80, 0x00,
    0x01, 0x00, 0x04,
    0x68, 0xee, 0x3c, 0x80
};

static const uint8_t k_nal[] = {
    0x00, 0x00, 0x00, 0x08,
    0x65, 0x88, 0x84, 0x00, 0x2a, 0x00, 0x00, 0x01
};

static void wr_be24(uint8_t *p, unsigned v)
{
    p[0] = (uint8_t)((v >> 16) & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)(v & 0xFFu);
}

static void wr_be32(uint8_t *p, unsigned v)
{
    p[0] = (uint8_t)((v >> 24) & 0xFFu);
    p[1] = (uint8_t)((v >> 16) & 0xFFu);
    p[2] = (uint8_t)((v >> 8) & 0xFFu);
    p[3] = (uint8_t)(v & 0xFFu);
}

static int add_tag(uint8_t *out, size_t cap, size_t *n, unsigned typ,
                   unsigned dts, const uint8_t *body, size_t bn)
{
    size_t need = *n + 11u + bn + 4u;
    if (need > cap) {
        return -1;
    }
    out[*n] = (uint8_t)typ;
    wr_be24(out + *n + 1, (unsigned)bn);
    wr_be24(out + *n + 4, dts & 0xFFFFFFu);
    out[*n + 7] = (uint8_t)((dts >> 24) & 0xFFu);
    wr_be24(out + *n + 8, 0);
    memcpy(out + *n + 11, body, bn);
    wr_be32(out + *n + 11 + bn, (unsigned)(11u + bn));
    *n = need;
    return 0;
}

int main(void)
{
    uint8_t flv[512];
    uint8_t vcfg[5 + sizeof(k_avcc)];
    uint8_t vnal[5 + sizeof(k_nal)];
    size_t n = 0;
    ms_session *s = NULL;
    ms_session_limits lim;
    ms_session_limits abs;
    ms_epochs ep;
    ms_recog_level lvl;
    int is_hw = 0, hw_ver = 0;
    const char *hw_be = NULL, *hw_detail = NULL;
    ms_status st;
    size_t consumed = 0;
    int nfmt = 0, ntrk = 0, ncfg = 0, nau = 0, neos = 0;
    int i;

    printf("ms_so_abi_smoke: linked against libmsmedia.so; "
           "this binary is built WITHOUT ASan/UBSan\n");
    printf("abi_version=%d status_ok=%s codec_h264=%s recog=%s "
           "format_flv=%s sample_avcc=%s\n",
           (int)ms_abi_version(),
           ms_status_name(MS_OK),
           ms_codec_name(MS_CODEC_H264),
           ms_recog_level_name(MS_RECOG_CONTAINER_ID),
           ms_format_name(MS_FORMAT_FLV),
           ms_sample_format_name(MS_SAMPLE_FMT_AVCC));

    if (ms_format_name(MS_FORMAT_FLV) == NULL ||
        strcmp(ms_format_name(MS_FORMAT_FLV), "flv") != 0) {
        fprintf(stderr, "format name is not flv\n");
        return 1;
    }

    ms_session_limits_default(&lim);
    ms_session_limits_absolute(&abs);
    (void)ms_codec_query(MS_CODEC_H264, &lvl);
    (void)ms_hw_query(MS_CODEC_H264, &is_hw, &hw_ver, &hw_be, &hw_detail);

    memset(flv, 0, sizeof(flv));
    flv[0] = (uint8_t)'F';
    flv[1] = (uint8_t)'L';
    flv[2] = (uint8_t)'V';
    flv[3] = 1;
    flv[4] = 0x01;
    wr_be32(flv + 5, 9);
    n = 9;
    wr_be32(flv + n, 0);
    n += 4;
    vcfg[0] = (uint8_t)((1u << 4) | 7u);
    vcfg[1] = 0;
    wr_be24(vcfg + 2, 0);
    memcpy(vcfg + 5, k_avcc, sizeof(k_avcc));
    if (add_tag(flv, sizeof(flv), &n, 9, 0, vcfg, sizeof(vcfg)) != 0) {
        return 1;
    }
    vnal[0] = (uint8_t)((1u << 4) | 7u);
    vnal[1] = 1;
    wr_be24(vnal + 2, 0);
    memcpy(vnal + 5, k_nal, sizeof(k_nal));
    if (add_tag(flv, sizeof(flv), &n, 9, 40, vnal, sizeof(vnal)) != 0) {
        return 1;
    }

    st = ms_session_create(NULL, &lim, &s);
    if (st != MS_OK || s == NULL) {
        fprintf(stderr, "create failed: %s\n", ms_status_name(st));
        return 1;
    }
    st = ms_session_push(s, flv, n, &consumed);
    if (st != MS_OK || consumed != n) {
        fprintf(stderr, "push failed: %s consumed=%zu/%zu\n",
                ms_status_name(st), consumed, n);
        ms_session_destroy(s);
        return 1;
    }
    st = ms_session_end_of_input(s);
    if (st != MS_OK) {
        fprintf(stderr, "eos failed: %s\n", ms_status_name(st));
        ms_session_destroy(s);
        return 1;
    }
    for (i = 0; i < 16; i++) {
        ms_event ev;
        st = ms_session_pull(s, &ev);
        if (st == MS_AGAIN || st == MS_EOS) {
            break;
        }
        if (st != MS_OK) {
            fprintf(stderr, "pull failed: %s\n", ms_status_name(st));
            ms_session_destroy(s);
            return 1;
        }
        if (ev.type == MS_EVENT_FORMAT && ev.format == MS_FORMAT_FLV) {
            nfmt++;
        } else if (ev.type == MS_EVENT_TRACK) {
            ntrk++;
        } else if (ev.type == MS_EVENT_CODEC_CONFIG) {
            ncfg++;
        } else if (ev.type == MS_EVENT_AU) {
            nau++;
            if (ev.pts != 40ull || ev.dts != 40ull || ev.data_size != sizeof(k_nal)) {
                fprintf(stderr, "au contract mismatch pts=%llu dts=%llu size=%zu\n",
                        (unsigned long long)ev.pts, (unsigned long long)ev.dts,
                        ev.data_size);
                ms_session_destroy(s);
                return 1;
            }
        } else if (ev.type == MS_EVENT_EOS) {
            neos++;
        }
        if (ms_session_release(s, ev.release_id) != MS_OK) {
            fprintf(stderr, "release failed\n");
            ms_session_destroy(s);
            return 1;
        }
    }
    if (nfmt != 1 || ntrk != 1 || ncfg != 1 || nau != 1) {
        fprintf(stderr, "counts fmt=%d trk=%d cfg=%d au=%d eos=%d\n",
                nfmt, ntrk, ncfg, nau, neos);
        ms_session_destroy(s);
        return 1;
    }
    if (ms_session_epochs(s, &ep) != MS_OK) {
        ms_session_destroy(s);
        return 1;
    }
    {
        ms_session_stats sst;
        memset(&sst, 0, sizeof(sst));
        sst.struct_size = sizeof(sst);
        if (ms_session_get_stats(s, &sst) != MS_OK ||
            !sst.has_format || !sst.has_track || !sst.has_config ||
            !sst.has_key_au || sst.move_out_ops != 1 || sst.copy_out_ops < 1) {
            fprintf(stderr, "get_stats failed or unexpected "
                    "fmt=%d trk=%d cfg=%d key=%d move=%llu copy_out=%llu\n",
                    sst.has_format, sst.has_track, sst.has_config, sst.has_key_au,
                    (unsigned long long)sst.move_out_ops,
                    (unsigned long long)sst.copy_out_ops);
            ms_session_destroy(s);
            return 1;
        }
        printf("stats move_out_ops=%llu copy_out_ops=%llu baseline_out=%llu "
               "bytes_in=%llu (parse-ready, not first frame)\n",
               (unsigned long long)sst.move_out_ops,
               (unsigned long long)sst.copy_out_ops,
               (unsigned long long)sst.baseline_out_bytes,
               (unsigned long long)sst.bytes_in);
    }
    /* 其余 ABI 入口：只确认动态符号可解析，不要求本路径成功实现。 */
    {
        ms_mem_source mem;
        ms_source src;
        ms_file_source fs;
        uint8_t one = 0;
        ms_mem_source_init(&mem, &one, 1);
        ms_mem_source_bind(&mem, &src);
        (void)ms_session_set_source(s, &src);
        (void)ms_session_drive(s);
        (void)ms_file_source_open(&fs, "");
        ms_file_source_close(&fs);
    }
    {
        ms_hls *hh = NULL;
        ms_hls_limits hl;
        ms_hls_mem_fetch mf;
        ms_hls_file_fetch ff;
        ms_hls_fetch fe;
        char uri[80];
        ms_hls_limits_default(&hl);
        (void)ms_hls_create(NULL, &hl, &hh);
        (void)ms_hls_resolve_uri("mem://h/a.m3u8", "s.ts", uri, sizeof(uri));
        ms_hls_mem_fetch_init(&mf, NULL, 0);
        ms_hls_mem_fetch_bind(&mf, &fe);
        if (hh != NULL) {
            (void)ms_hls_bind(hh, s, &fe);
            (void)ms_hls_open(hh, "mem://h/a.m3u8");
            (void)ms_hls_drive(hh);
            (void)ms_hls_cancel(hh);
            ms_hls_destroy(hh);
        }
        ms_hls_file_fetch_init(&ff);
        ms_hls_file_fetch_bind(&ff, &fe);
        (void)ms_session_signal_discontinuity(s);
    }
    (void)ms_session_flush(s);
    (void)ms_session_reset(s);
    (void)ms_session_seek(s, 0);
    (void)ms_session_set_rate(s, 1, 1);
    (void)ms_session_select_track(s, 1, 1);
    (void)ms_session_cancel(s);
    ms_session_destroy(s);
    printf("flv path: format+track+config+au ok (1 each); public ABI symbols resolved\n");
    return 0;
}
