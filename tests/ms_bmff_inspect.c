/*
 * 开发用 ISO BMFF 箱体检查器。只链公共/内部解析 API，不链 FFmpeg。
 * 非正式验收入口。本步不组帧、不可播放。
 * by: Long <mycjzb@gmail.com>
 */

#include "msmedia/msmedia.h"
#include "ms_internal.h"
#include "ms_bmff.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct rec {
    int n;
    ms_format fmt;
    ms_status last;
} rec;

static ms_status on_box(void *ctx, const ms_bmff_box *box)
{
    char typ[5], par[5];
    (void)ctx;
    ms_bmff_type_str(box->type, typ);
    ms_bmff_type_str(box->parent_type, par);
    printf("box off=%" PRIu64 " size=%" PRIu64 " typ=%s parent=%s depth=%d hdr=%u container=%d uuid=%d\n",
           box->offset, box->size, typ, box->parent_type ? par : "-",
           box->depth, box->header_size, box->is_container, box->has_uuid);
    return MS_OK;
}

static ms_status on_fmt(void *ctx, ms_format format,
                        uint32_t major_brand, uint32_t minor_version)
{
    rec *r = (rec *)ctx;
    char b[5];
    const char *name = "unknown";
    r->fmt = format;
    ms_bmff_type_str(major_brand, b);
    if (format == MS_FORMAT_MP4) {
        name = "mp4";
    } else if (format == MS_FORMAT_FMP4) {
        name = "fmp4";
    } else if (format == MS_FORMAT_MOV) {
        name = "mov";
    }
    printf("format=%s major=%s minor=%u\n", name, b, major_brand ? (unsigned)minor_version : 0);
    return MS_OK;
}

static void die_usage(void)
{
    fprintf(stderr, "usage: ms_bmff_inspect [--chunk N] [file|-]\n");
    exit(2);
}

int main(int argc, char **argv)
{
    const char *path = "-";
    size_t chunk = 4096;
    FILE *fp;
    uint8_t *buf;
    ms_bmff_parser p;
    ms_bmff_callbacks cb;
    rec r;
    ms_status st = MS_OK;
    int i, eof = 0;
    char major[5];

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--chunk") == 0 && i + 1 < argc) {
            chunk = (size_t)strtoul(argv[++i], NULL, 10);
            if (chunk == 0) {
                chunk = 1;
            }
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
            fprintf(stderr, "open failed\n");
            return 1;
        }
    }
    buf = (uint8_t *)malloc(chunk);
    if (buf == NULL) {
        fprintf(stderr, "oom\n");
        return 1;
    }
    memset(&r, 0, sizeof(r));
    memset(&cb, 0, sizeof(cb));
    cb.ctx = &r;
    cb.on_box = on_box;
    cb.on_format = on_fmt;
    ms_bmff_parser_init(&p, NULL, &cb);
    printf("chunk=%zu\n", chunk);
    while (!eof) {
        size_t n = fread(buf, 1, chunk, fp);
        size_t used = 0;
        if (n < chunk) {
            eof = 1;
        }
        st = ms_bmff_parser_push(&p, buf, n, &used, eof);
        if (used < n && st == MS_OK) {
            /* 不应把未消费的合法后缀丢掉。 */
            fprintf(stderr, "partial consume used=%zu n=%zu\n", used, n);
            st = MS_ERR_STATE;
        }
        if (st != MS_OK) {
            break;
        }
    }
    if (fp != stdin) {
        fclose(fp);
    }
    free(buf);
    ms_bmff_type_str(ms_bmff_parser_major_brand(&p), major);
    printf("summary boxes=%d max_depth=%d moov=%d mdat=%d moof=%d mvex=%d skipped=%" PRIu64
           " truncated_mdat=%d major=%s format=%d status=%s\n",
           ms_bmff_parser_box_count(&p),
           ms_bmff_parser_max_depth(&p),
           ms_bmff_parser_saw_moov(&p),
           ms_bmff_parser_saw_mdat(&p),
           ms_bmff_parser_saw_moof(&p),
           ms_bmff_parser_saw_mvex(&p),
           ms_bmff_parser_bytes_skipped(&p),
           ms_bmff_parser_truncated_mdat(&p),
           major,
           (int)ms_bmff_parser_format(&p),
           ms_status_name(st));
    ms_bmff_parser_destroy(&p);
    return (st == MS_OK) ? 0 : 1;
}
