/*
 * 开发用会话检查器：经公共 ABI 喂任意碎片，统计轨道/config/AU 与 PTS/IDR/hash。
 * 可用于 MPEG-TS、渐进 MP4/MOV、fMP4 与经典 FLV（公共会话 ABI）。
 * 不链入 FFmpeg，也不做软解。仅供交叉验证，不是产品 CLI。
 * by: Long <mycjzb@gmail.com>
 *
 * 用法：
 *   ms_ts_inspect [--chunk N] [--max-retries N] [--source] [--max-lookback N] [file|-]
 * 默认按 188 字节块推入；N=0 表示整文件一次推入。
 * --source：对普通文件用开发用文件源 + drive（可跳过尾 mdat），不可用于 stdin。
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
            "usage: ms_ts_inspect [--chunk N] [--max-retries N] [--source] [--max-lookback N] [file|-]\n");
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


typedef struct inspect_acc {
    int n_track, n_cfg, n_v, n_a, n_key, n_disc, n_other;
    int au_idx;
} inspect_acc;

static void inspect_on_event(inspect_acc *a, const ms_event *ev)
{
    if (ev->type == MS_EVENT_FORMAT) {
        if (ev->format == MS_FORMAT_MPEG_TS) {
            printf("format=mpeg_ts packet_size=%u\n",
                   (unsigned)ev->ts_packet_size);
        } else {
            printf("format=%s\n", ms_format_name(ev->format));
        }
    } else if (ev->type == MS_EVENT_TRACK || ev->type == MS_EVENT_TRACK_UPDATE) {
        a->n_track++;
        printf("track id=%u kind=%s codec=%s level=%s stream_type=0x%02x lang=%s sample_fmt=%s\n",
               ev->track.id,
               kind_name(ev->track.kind),
               ms_codec_name(ev->track.codec),
               ms_recog_level_name(ev->track.level),
               ev->track.stream_type,
               ev->track.language ? ev->track.language : "-",
               ms_sample_format_name(ev->sample_format));
    } else if (ev->type == MS_EVENT_CODEC_CONFIG) {
        char hx[65];
        a->n_cfg++;
        ms_sha256_hex(ev->data, ev->data_size, hx);
        printf("config track=%u size=%zu sha256=%s sample_fmt=%s tb=%d/%d\n",
               ev->track.id, ev->data_size, hx,
               ms_sample_format_name(ev->sample_format),
               ev->time_base_num, ev->time_base_den);
    } else if (ev->type == MS_EVENT_AU) {
        char hx[65];
        char pay[65];
        int hdr;
        const char *knd = kind_name(ev->track.kind);
        ms_sha256_hex(ev->data, ev->data_size, hx);
        pay[0] = 0;
        hdr = -1;
        if (ev->track.codec == MS_CODEC_AAC) {
            hdr = adts_hdr_len(ev->data, ev->data_size);
            if (hdr > 0 && ev->data_size >= (size_t)hdr) {
                ms_sha256_hex(ev->data + hdr, ev->data_size - (size_t)hdr, pay);
            }
            a->n_a++;
        } else if (ev->track.codec == MS_CODEC_H264) {
            a->n_v++;
        }
        if (ev->is_keyframe) {
            a->n_key++;
        }
        printf("au idx=%d track=%u kind=%s codec=%s pts=%s%" PRIu64
               " dts=%s%" PRIu64 " key=%d disc=%d fmtchg=%d size=%zu sha256=%s",
               a->au_idx, ev->track.id, knd, ms_codec_name(ev->track.codec),
               ev->has_pts ? "" : "na:", ev->has_pts ? ev->pts : 0,
               ev->has_dts ? "" : "na:", ev->has_dts ? ev->dts : 0,
               ev->is_keyframe, ev->discontinuity, ev->format_changed,
               ev->data_size, hx);
        if (pay[0] != 0) {
            printf(" adts_hdr=%d payload_sha256=%s", hdr, pay);
        }
        printf(" sample_fmt=%s tb=%d/%d preroll=%d\n",
               ms_sample_format_name(ev->sample_format),
               ev->time_base_num, ev->time_base_den, ev->preroll);
        a->au_idx++;
    } else if (ev->type == MS_EVENT_DISCONTINUITY) {
        a->n_disc++;
        printf("discontinuity\n");
    } else if (ev->type == MS_EVENT_EOS) {
        printf("eos\n");
    } else if (ev->type == MS_EVENT_UNSUPPORTED) {
        a->n_other++;
        printf("unsupported format=%s\n", ms_format_name(ev->format));
    } else {
        a->n_other++;
        printf("event type=%d\n", (int)ev->type);
    }
}

static int run(FILE *fp, size_t chunk, int max_retries, size_t max_lookback)
{
    ms_session *s = NULL;
    ms_session_limits lim;
    ms_status st;
    uint8_t *buf = NULL;
    size_t cap = 0;
    size_t nread;
    int eof = 0;
    inspect_acc acc;
    int failed = 0;
    const char *fail_name = "ok";

    memset(&acc, 0, sizeof(acc));
    ms_session_limits_default(&lim);
    if (max_lookback > 0) {
        lim.max_lookback_bytes = max_lookback;
    }
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

                inspect_on_event(&acc, &ev);
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
                inspect_on_event(&acc, &ev);
                (void)ms_session_release(s, ev.release_id);
            }
            break;
        }
    }

drain_end:
    printf("summary tracks=%d configs=%d video_au=%d audio_au=%d video_or_audio_key=%d disc=%d other=%d status=%s\n",
           acc.n_track, acc.n_cfg, acc.n_v, acc.n_a, acc.n_key, acc.n_disc, acc.n_other,
           failed ? fail_name : "ok");
    {
        ms_session_stats sst;
        memset(&sst, 0, sizeof(sst));
        sst.struct_size = sizeof(sst);
        if (ms_session_get_stats(s, &sst) == MS_OK) {
            printf("stats bytes_in=%" PRIu64
                   " copy_in=%" PRIu64 "/%" PRIu64
                   " copy_asm=%" PRIu64 "/%" PRIu64
                   " copy_out=%" PRIu64 "/%" PRIu64
                   " move_out=%" PRIu64 "/%" PRIu64
                   " baseline_out=%" PRIu64 "\n",
                   sst.bytes_in,
                   sst.copy_in_ops, sst.copy_in_bytes,
                   sst.copy_asm_ops, sst.copy_asm_bytes,
                   sst.copy_out_ops, sst.copy_out_bytes,
                   sst.move_out_ops, sst.move_out_bytes,
                   sst.baseline_out_bytes);
            printf("startup format=%d@%" PRIu64
                   " track=%d@%" PRIu64
                   " config=%d@%" PRIu64
                   " key_au=%d@%" PRIu64
                   " video_key=%d@%" PRIu64
                   " (parse-ready, not first on-screen frame; no net/hw stats)\n",
                   sst.has_format, sst.bytes_at_format,
                   sst.has_track, sst.bytes_at_track,
                   sst.has_config, sst.bytes_at_config,
                   sst.has_key_au, sst.bytes_at_key_au,
                   sst.has_video_key_au, sst.bytes_at_video_key_au);
        }
    }
    free(buf);
    ms_session_destroy(s);
    return failed ? 1 : 0;
}

static void inspect_print_stats(const ms_session *s)
{
    ms_session_stats sst;
    memset(&sst, 0, sizeof(sst));
    sst.struct_size = sizeof(sst);
    if (ms_session_get_stats(s, &sst) != MS_OK) {
        return;
    }
    printf("stats bytes_in=%" PRIu64
           " copy_in=%" PRIu64 "/%" PRIu64
           " copy_asm=%" PRIu64 "/%" PRIu64
           " copy_out=%" PRIu64 "/%" PRIu64
           " move_out=%" PRIu64 "/%" PRIu64
           " baseline_out=%" PRIu64 "\n",
           sst.bytes_in,
           sst.copy_in_ops, sst.copy_in_bytes,
           sst.copy_asm_ops, sst.copy_asm_bytes,
           sst.copy_out_ops, sst.copy_out_bytes,
           sst.move_out_ops, sst.move_out_bytes,
           sst.baseline_out_bytes);
    printf("startup format=%d@%" PRIu64
           " track=%d@%" PRIu64
           " config=%d@%" PRIu64
           " key_au=%d@%" PRIu64
           " video_key=%d@%" PRIu64
           " (parse-ready, not first on-screen frame; no net/hw stats)\n",
           sst.has_format, sst.bytes_at_format,
           sst.has_track, sst.bytes_at_track,
           sst.has_config, sst.bytes_at_config,
           sst.has_key_au, sst.bytes_at_key_au,
           sst.has_video_key_au, sst.bytes_at_video_key_au);
    if (sst.has_seek_land) {
        printf("seek request=%" PRId64 " land=%" PRId64 "\n",
               sst.seek_request_90k, sst.seek_land_90k);
    }
}

static int run_source(const char *path, size_t max_lookback, int max_steps)
{
    ms_session *s = NULL;
    ms_session_limits lim;
    ms_file_source fs;
    ms_source src;
    ms_status st;
    inspect_acc acc;
    int failed = 0;
    const char *fail_name = "ok";
    int i;
    int stall = 0;

    memset(&acc, 0, sizeof(acc));
    memset(&fs, 0, sizeof(fs));
    ms_session_limits_default(&lim);
    if (max_lookback > 0) {
        lim.max_lookback_bytes = max_lookback;
    }
    st = ms_session_create(NULL, &lim, &s);
    if (st != MS_OK) {
        fprintf(stderr, "create failed: %s\n", ms_status_name(st));
        return 1;
    }
    st = ms_file_source_open(&fs, path);
    if (st != MS_OK) {
        fprintf(stderr, "file source open failed: %s\n", ms_status_name(st));
        ms_session_destroy(s);
        return 1;
    }
    ms_file_source_bind(&fs, &src);
    st = ms_session_set_source(s, &src);
    if (st != MS_OK) {
        fprintf(stderr, "set_source failed: %s\n", ms_status_name(st));
        ms_file_source_close(&fs);
        ms_session_destroy(s);
        return 1;
    }
    printf("chunk=source max_lookback=%zu\n", max_lookback);

    for (i = 0; i < max_steps && !failed; i++) {
        ms_event ev;
        st = ms_session_pull(s, &ev);
        if (st == MS_OK) {
            stall = 0;
            inspect_on_event(&acc, &ev);
            if (ms_session_release(s, ev.release_id) != MS_OK) {
                failed = 1;
                fail_name = "release";
            }
            continue;
        }
        if (st == MS_EOS) {
            break;
        }
        if (st != MS_AGAIN) {
            failed = 1;
            fail_name = ms_status_name(st);
            break;
        }
        st = ms_session_drive(s);
        if (st == MS_OK || st == MS_LIMIT || st == MS_EOS) {
            stall = 0;
            continue;
        }
        if (st == MS_AGAIN) {
            stall++;
            if (stall > 64) {
                failed = 1;
                fail_name = "again-stall";
            }
            continue;
        }
        failed = 1;
        fail_name = ms_status_name(st);
        break;
    }
    if (!failed && i >= max_steps) {
        failed = 1;
        fail_name = "drive-bound";
    }
    printf("summary tracks=%d configs=%d video_au=%d audio_au=%d video_or_audio_key=%d disc=%d other=%d status=%s\n",
           acc.n_track, acc.n_cfg, acc.n_v, acc.n_a, acc.n_key, acc.n_disc, acc.n_other,
           failed ? fail_name : "ok");
    inspect_print_stats(s);
    ms_file_source_close(&fs);
    ms_session_destroy(s);
    return failed ? 1 : 0;
}


int main(int argc, char **argv)
{
    const char *path = "-";
    size_t chunk = 188;
    int max_retries = 256;
    size_t max_lookback = 0;
    int use_source = 0;
    FILE *fp;
    int i;
    int rc;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--chunk") == 0 && i + 1 < argc) {
            chunk = (size_t)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--max-retries") == 0 && i + 1 < argc) {
            max_retries = (int)strtol(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--max-lookback") == 0 && i + 1 < argc) {
            max_lookback = (size_t)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--source") == 0) {
            use_source = 1;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            die_usage();
        } else if (argv[i][0] == '-' && argv[i][1] != 0) {
            die_usage();
        } else {
            path = argv[i];
        }
    }

    if (use_source) {
        if (strcmp(path, "-") == 0) {
            fprintf(stderr, "--source requires a regular file path\n");
            return 2;
        }
        return run_source(path, max_lookback, 8 * 1024 * 1024);
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
    rc = run(fp, chunk, max_retries, max_lookback);
    if (fp != stdin) {
        fclose(fp);
    }
    return rc;
}
