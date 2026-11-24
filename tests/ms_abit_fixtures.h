/*
 * 可追溯音频帧夹具：AC-3 / MP2 / MP3 / Opus。
 * 头字段按规范打包；载荷可填零。编码工具样本不等于产品软解。
 * by: Long <mycjzb@gmail.com>
 */
#ifndef MSMEDIA_MS_ABIT_FIXTURES_H
#define MSMEDIA_MS_ABIT_FIXTURES_H

#include <stddef.h>
#include <stdint.h>

/* ATSC A/52：48 kHz、frmsizecod=0（32 kbps）、stereo、无 LFE；帧长 128 字节。 */
static const uint8_t k_ac3_48k_stereo[128] = {
    0x0B, 0x77, 0x00, 0x00, 0x00, 0x40, 0x40
};

/* 44.1 kHz、frmsizecod=0；帧长 138 字节（69 words）。 */
static const uint8_t k_ac3_441k_stereo[138] = {
    0x0B, 0x77, 0x00, 0x00, 0x40, 0x40, 0x40
};

static const uint8_t k_ac3_bad_sync[8] = { 0x0B, 0x76, 0x00, 0x00, 0x00, 0x40, 0x40, 0x00 };

/* MPEG-1 Layer III：128 kbps、44.1 kHz、stereo、无 padding；帧长 417。 */
static const uint8_t k_mp3_441_128[417] = { 0xFF, 0xFB, 0x90, 0x00 };

/* MPEG-1 Layer II：192 kbps、48 kHz、stereo、无 padding；帧长 576。 */
static const uint8_t k_mp2_48k_192[576] = { 0xFF, 0xFD, 0xA4, 0x00 };

/* 把 MP2 头误当作 MP3（层不匹配）。 */
static const uint8_t k_mp2_hdr_only[4] = { 0xFF, 0xFD, 0xA4, 0x00 };

/* 组 ISO MPEG 音频 4 字节头。ver/layer 为规范字段，不是 Layer I/II/III 数字。 */
static inline void ms_abit_pack_mpega(uint8_t d[4], unsigned ver, unsigned layer,
                               unsigned bridx, unsigned sridx, unsigned pad,
                               unsigned mode)
{
    d[0] = 0xFF;
    d[1] = (uint8_t)(0xE0u | ((ver & 3u) << 3) | ((layer & 3u) << 1) | 1u);
    d[2] = (uint8_t)(((bridx & 0xFu) << 4) | ((sridx & 3u) << 2) | ((pad & 1u) << 1));
    d[3] = (uint8_t)((mode & 3u) << 6);
}

/* 保留 version=1、MPEG-1 Layer III 码率/采样率位。 */
static const uint8_t k_mp3_reserved_ver[4] = { 0xFF, 0xEB, 0x90, 0x00 };
/* MPEG-2.5（version=0）Layer III；本切片不声称。 */
static const uint8_t k_mp3_mpeg25[4] = { 0xFF, 0xE3, 0x90, 0x00 };
/* layer=0 保留。 */
static const uint8_t k_mpega_reserved_layer[4] = { 0xFF, 0xF9, 0x90, 0x00 };
/* 自由格式 bridx=0。 */
static const uint8_t k_mp3_bridx0[4] = { 0xFF, 0xFB, 0x00, 0x00 };
/* 坏码率索引 15。 */
static const uint8_t k_mp3_bridx15[4] = { 0xFF, 0xFB, 0xF0, 0x00 };
/* 保留采样率索引 3。 */
static const uint8_t k_mp3_sridx3[4] = { 0xFF, 0xFB, 0x9C, 0x00 };
/* 同步字残缺（不足 4 字节）。 */
static const uint8_t k_mp3_short_hdr[3] = { 0xFF, 0xFB, 0x90 };
/* MPEG-1 Layer I 头；产品只分发 MP2/MP3。 */
static const uint8_t k_mp1_hdr[4] = { 0xFF, 0xFF, 0x90, 0x00 };

/* MPEG-2 Layer III：64 kbps、24 kHz、stereo、无 padding；576 样点，帧长 192。 */
static const uint8_t k_mp3_mpeg2_24k_64[192] = { 0xFF, 0xF3, 0x84, 0x00 };
/* MPEG-1 Layer III：128 kbps、44.1 kHz、有 padding；帧长 418。 */
static const uint8_t k_mp3_441_128_pad[418] = { 0xFF, 0xFB, 0x92, 0x00 };
/* MPEG-1 Layer III：128 kbps、44.1 kHz、mono；帧长 417。 */
static const uint8_t k_mp3_441_128_mono[417] = { 0xFF, 0xFB, 0x90, 0xC0 };
/* 合法极限：MPEG-1 Layer III 320 kbps、32 kHz；帧长 1440。 */
static const uint8_t k_mp3_32k_320[1440] = { 0xFF, 0xFB, 0xE8, 0x00 };
/* 合法下限：MPEG-1 Layer III 32 kbps、44.1 kHz；帧长 104。 */
static const uint8_t k_mp3_441_32[104] = { 0xFF, 0xFB, 0x10, 0x00 };

/* RFC 7845 OpusHead：version=1, 2ch, pre-skip=312, input rate 48000, gain 0, mapping 0。 */
static const uint8_t k_opus_head[19] = {
    0x4F, 0x70, 0x75, 0x73, 0x48, 0x65, 0x61, 0x64,
    0x01, 0x02, 0x38, 0x01, 0x80, 0xBB, 0x00, 0x00,
    0x00, 0x00, 0x00
};

/* 对应的 MP4 dOps（Version=0，无魔术），由真实字段装箱为 OpusHead。 */
static const uint8_t k_opus_dops[11] = {
    0x00, 0x02, 0x38, 0x01, 0x80, 0xBB, 0x00, 0x00, 0x00, 0x00, 0x00
};

/* 20 ms stereo hybrid SWB（config 13），单帧；载荷可为空。 */
static const uint8_t k_opus_toc20[1] = { 0x6C };

#endif
