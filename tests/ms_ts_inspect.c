/*
 * 开发用 TS 检查器：经公共 ABI 喂任意碎片，统计轨道/config/AU 与 PTS/IDR/hash。
 * 不链入 FFmpeg，也不做软解。仅供交叉验证，不是产品 CLI。
 * by: Long <mycjzb@gmail.com>
 *
 * 用法：
 *   ms_ts_inspect [--chunk N] [--max-retries N] [file|-]
 * 默认按 188 字节块推入；N=0 表示整文件一次推入。
 */

#include "msmedia/msmedia.h"
#include "ms_sha256.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>

static void die_usage(void)
{
    fprintf(stderr,
            "usage: ms_ts_inspect [--chunk N] [--max-retries N] [file|-]\n");
    exit(2);
}

static int adts_hdr_len(const uint8_t *d, size_t n)
{
    if (n < 7 || d[0] != 0xFF || (d[1] & 0xF0) != 0xF0) {
        return -1;
    }
    return ((d[1] & 0x01) != 0) ? 7 : 9;
}

static const char *kind_name(ms_track_kind k)
{
    if (k == MS_TRACK_VIDEO) {
        return "video";
    }
    if (k == MS_TRACK_AUDIO) {
        return "audio";
    }
    return "other";
}

static int run(FILE *fp, size_t chunk, int max_retries)
{
    ms_session *s = NULL;
    ms_session_limits lim;
    ms_status st;
    uint8_t *buf = NULL;
    size_t cap = 0;
    size_t nread;
    int eof = 0;
    int n_track = 0, n_cfg = 0, n_v = 0, n_a = 0, n_key = 0, n_disc = 0, n_other = 0;
    int au_idx = 0;
    int failed = 0;
    const char *fail_name = "ok";

    ms_session_limits_default(&lim);
    st = ms_session_create(NULL, &lim, &s);
    if (st != MS_OK) {
        fprintf(stderr, "create failed: %s\n", ms_status_name(st));
        return 1;
    }

    cap = (chunk == 0) ? (64u * 1024u) : chunk;
    if (cap < 1) {
        cap = 1;
    }
    buf = (uint8_t *)malloc(cap);
    if (buf == NULL) {
        ms_session_destroy(s);
        return 1;
    }

    printf("chunk=%zu\n", chunk);

    for (;;) {
        size_t off = 0;
        int retries = 0;

        if (!eof) {
            size_t want = cap;
            nread = fread(buf, 1, want, fp);
            if (nread == 0) {
                if (ferror(fp)) {
                    fprintf(stderr, "read error\n");
                    failed = 1;
                    fail_name = "read";
                    break;
                }
                eof = 1;
                st = ms_session_end_of_input(s);
                if (st != MS_OK && st != MS_LIMIT) {
                    failed = 1;
                    fail_name = ms_status_name(st);
                    break;
                }
            }
        } else {
            nread = 0;
        }

        while (off < nread || (eof && nread == 0 && retries == 0)) {
            size_t cons = 0;
            if (off < nread) {
                st = ms_session_push(s, buf + off, nread - off, &cons);
                if (st == MS_OK || st == MS_LIMIT) {
                    off += cons;
                } else if (st == MS_AGAIN) {
                    /* 需要先取事件或更多输入 */
                } else {
                    failed = 1;
                    fail_name = ms_status_name(st);
                    goto drain_end;
                }
            }

            for (;;) {
                ms_event ev;
                st = ms_session_pull(s, &ev);
                if (st == MS_AGAIN) {
                    break;
                }
                if (st == MS_EOS) {
                    eof = 1;
                    goto drain_end;
                }
                if (st != MS_OK) {
                    failed = 1;
                    fail_name = ms_status_name(st);
                    goto drain_end;
                }

                if (ev.type == MS_EVENT_FORMAT) {
                    printf("format=mpeg_ts packet_size=%u\n",
                           (unsigned)ev.ts_packet_size);
                } else if (ev.type == MS_EVENT_TRACK ||
                           ev.type == MS_EVENT_TRACK_UPDATE) {
                    n_track++;
                    printf("track id=%u kind=%s codec=%s level=%s stream_type=0x%02x lang=%s\n",
                           ev.track.id,
                           kind_name(ev.track.kind),
                           ms_codec_name(ev.track.codec),
                           ms_recog_level_name(ev.track.level),
                           ev.track.stream_type,
                           ev.track.language ? ev.track.language : "-");
                } else if (ev.type == MS_EVENT_CODEC_CONFIG) {
                    char hx[65];
                    n_cfg++;
                    ms_sha256_hex(ev.data, ev.data_size, hx);
                    printf("config track=%u size=%zu sha256=%s\n",
                           ev.track.id, ev.data_size, hx);
                } else if (ev.type == MS_EVENT_AU) {
                    char hx[65];
                    char pay[65];
                    int hdr;
                    const char *knd = kind_name(ev.track.kind);
                    ms_sha256_hex(ev.data, ev.data_size, hx);
                    pay[0] = 0;
                    hdr = -1;
                    if (ev.track.codec == MS_CODEC_AAC) {
                        hdr = adts_hdr_len(ev.data, ev.data_size);
                        if (hdr > 0 && ev.data_size >= (size_t)hdr) {
                            ms_sha256_hex(ev.data + hdr, ev.data_size - (size_t)hdr, pay);
                        }
                        n_a++;
                    } else if (ev.track.codec == MS_CODEC_H264) {
                        n_v++;
                    }
                    if (ev.is_keyframe) {
                        n_key++;
                    }
                    printf("au idx=%d track=%u kind=%s codec=%s pts=%s%" PRIu64
                           " dts=%s%" PRIu64 " key=%d disc=%d fmtchg=%d size=%zu sha256=%s",
                           au_idx, ev.track.id, knd, ms_codec_name(ev.track.codec),
                           ev.has_pts ? "" : "na:", ev.has_pts ? ev.pts : 0,
                           ev.has_dts ? "" : "na:", ev.has_dts ? ev.dts : 0,
                           ev.is_keyframe, ev.discontinuity, ev.format_changed,
                           ev.data_size, hx);
                    if (pay[0] != 0) {
                        printf(" adts_hdr=%d payload_sha256=%s", hdr, pay);
                    }
                    printf("\n");
                    au_idx++;
                } else if (ev.type == MS_EVENT_DISCONTINUITY) {
                    n_disc++;
                    printf("discontinuity\n");
                } else if (ev.type == MS_EVENT_EOS) {
                    printf("eos\n");
                } else {
                    n_other++;
                    printf("event type=%d\n", (int)ev.type);
                }
                st = ms_session_release(s, ev.release_id);
                if (st != MS_OK) {
                    failed = 1;
                    fail_name = ms_status_name(st);
                    goto drain_end;
                }
            }

            if (eof && nread == 0) {
                break;
            }
            if (off >= nread) {
                break;
            }
            if (st == MS_LIMIT || (nread > off && retries > 0)) {
                retries++;
                if (retries > max_retries) {
                    failed = 1;
                    fail_name = "limit-retries";
                    goto drain_end;
                }
                continue;
            }
            if (off == 0 && nread > 0 && retries > 0) {
                retries++;
                if (retries > max_retries) {
                    failed = 1;
                    fail_name = "stuck";
                    goto drain_end;
                }
            }
        }

        if (eof) {
            /* 再拉一次直到 EOS/AGAIN */
            int empty = 0;
            while (!empty) {
                ms_event ev;
                st = ms_session_pull(s, &ev);
                if (st == MS_AGAIN || st == MS_EOS) {
                    empty = 1;
                    break;
                }
                if (st != MS_OK) {
                    failed = 1;
                    fail_name = ms_status_name(st);
                    break;
                }
                if (ev.type == MS_EVENT_AU) {
                    char hx[65];
                    char pay[65];
                    int hdr = -1;
                    ms_sha256_hex(ev.data, ev.data_size, hx);
                    pay[0] = 0;
                    if (ev.track.codec == MS_CODEC_AAC) {
                        hdr = adts_hdr_len(ev.data, ev.data_size);
                        if (hdr > 0 && ev.data_size >= (size_t)hdr) {
                            ms_sha256_hex(ev.data + hdr, ev.data_size - (size_t)hdr, pay);
                        }
                        n_a++;
                    } else if (ev.track.codec == MS_CODEC_H264) {
                        n_v++;
                    }
                    if (ev.is_keyframe) {
                        n_key++;
                    }
                    printf("au idx=%d track=%u kind=%s codec=%s pts=%s%" PRIu64
                           " dts=%s%" PRIu64 " key=%d disc=%d fmtchg=%d size=%zu sha256=%s",
                           au_idx, ev.track.id, kind_name(ev.track.kind),
                           ms_codec_name(ev.track.codec),
                           ev.has_pts ? "" : "na:", ev.has_pts ? ev.pts : 0,
                           ev.has_dts ? "" : "na:", ev.has_dts ? ev.dts : 0,
                           ev.is_keyframe, ev.discontinuity, ev.format_changed,
                           ev.data_size, hx);
                    if (pay[0] != 0) {
                        printf(" adts_hdr=%d payload_sha256=%s", hdr, pay);
                    }
                    printf("\n");
                    au_idx++;
                } else if (ev.type == MS_EVENT_CODEC_CONFIG) {
                    char hx[65];
                    n_cfg++;
                    ms_sha256_hex(ev.data, ev.data_size, hx);
                    printf("config track=%u size=%zu sha256=%s\n",
                           ev.track.id, ev.data_size, hx);
                } else if (ev.type == MS_EVENT_DISCONTINUITY) {
                    n_disc++;
                    printf("discontinuity\n");
                } else if (ev.type == MS_EVENT_EOS) {
                    printf("eos\n");
                } else if (ev.type == MS_EVENT_TRACK || ev.type == MS_EVENT_TRACK_UPDATE) {
                    n_track++;
                    printf("track id=%u kind=%s codec=%s level=%s stream_type=0x%02x lang=%s\n",
                           ev.track.id,
                           kind_name(ev.track.kind),
                           ms_codec_name(ev.track.codec),
                           ms_recog_level_name(ev.track.level),
                           ev.track.stream_type,
                           ev.track.language ? ev.track.language : "-");
                }
                (void)ms_session_release(s, ev.release_id);
            }
            break;
        }
    }

drain_end:
    printf("summary tracks=%d configs=%d video_au=%d audio_au=%d video_or_audio_key=%d disc=%d other=%d status=%s\n",
           n_track, n_cfg, n_v, n_a, n_key, n_disc, n_other,
           failed ? fail_name : "ok");
    free(buf);
    ms_session_destroy(s);
    return failed ? 1 : 0;
}

int main(int argc, char **argv)
{
    const char *path = "-";
    size_t chunk = 188;
    int max_retries = 256;
    FILE *fp;
    int i;
    int rc;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--chunk") == 0 && i + 1 < argc) {
            chunk = (size_t)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--max-retries") == 0 && i + 1 < argc) {
            max_retries = (int)strtol(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            die_usage();
        } else if (argv[i][0] == '-' && argv[i][1] != 0) {
            die_usage();
        } else {
            path = argv[i];
        }
    }

    if (strcmp(path, "-") == 0) {
        fp = stdin;
    } else {
        fp = fopen(path, "rb");
        if (fp == NULL) {
            perror(path);
            return 1;
        }
    }
    rc = run(fp, chunk, max_retries);
    if (fp != stdin) {
        fclose(fp);
    }
    return rc;
}
