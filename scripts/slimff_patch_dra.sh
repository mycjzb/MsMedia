#!/usr/bin/env bash
# 把国标 DRA 解码器叠进 slimff 源码副本（对齐超级直播）。幂等。
# by: Long <mycjzb@gmail.com>
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="${1:-$ROOT/build/single-analysis/ffmpeg-9.1-slimff-src}"
DRA="$ROOT/third_party/dra"
[[ -f "$SRC/configure" ]] || { echo "error: missing $SRC/configure" >&2; exit 1; }
[[ -f "$DRA/libdradec.c" ]] || { echo "error: missing $DRA/libdradec.c" >&2; exit 1; }

cp -f "$DRA/dradec.c" "$DRA/dradec.h" "$DRA/libdradec.c" "$SRC/libavcodec/"
cat > "$SRC/libavcodec/dra_parser.c" <<'EOF'
#include "parser_internal.h"

static int dra_parse(AVCodecParserContext *s, AVCodecContext *avctx,
                     const uint8_t **poutbuf, int *poutbuf_size,
                     const uint8_t *buf, int buf_size)
{
    (void)s;
    (void)avctx;
    *poutbuf = buf;
    *poutbuf_size = buf_size;
    return buf_size;
}

const FFCodecParser ff_dra_parser = {
    PARSER_CODEC_LIST(AV_CODEC_ID_DRA),
    .parse = dra_parse,
};
EOF

python3 - "$SRC" <<'PY'
import sys
from pathlib import Path
src = Path(sys.argv[1])

def need(path, old, new, label):
    p = src / path
    t = p.read_text(encoding="utf-8", errors="replace")
    if new.strip() in t or (label.startswith("id") and "AV_CODEC_ID_DRA" in t and path.endswith("codec_id.h")):
        # codec_id：已有 DRA 则跳过
        if path.endswith("codec_id.h") and "AV_CODEC_ID_DRA" in t:
            return
        if not path.endswith("codec_id.h") and new in t:
            return
    if new in t:
        return
    if old not in t:
        sys.stderr.write("error: %s marker missing\n" % label)
        sys.exit(1)
    p.write_text(t.replace(old, new, 1), encoding="utf-8")

# codec_id：插在 APPLE_APAC 后；若已有 DRA 则跳过
p = src / "libavcodec/codec_id.h"
t = p.read_text(encoding="utf-8", errors="replace")
if "AV_CODEC_ID_DRA" not in t:
    old = "    AV_CODEC_ID_APPLE_APAC,\n"
    new = "    AV_CODEC_ID_APPLE_APAC,\n    AV_CODEC_ID_DRA,\n"
    if old not in t:
        sys.stderr.write("error: codec_id marker missing\n")
        sys.exit(1)
    p.write_text(t.replace(old, new, 1), encoding="utf-8")

# codec_desc：插在 subtitle codecs 段之前（避开 s5 裁掉的 APAC 块）
need("libavcodec/codec_desc.c",
     "\n    /* subtitle codecs */\n",
     '''
    {
        .id        = AV_CODEC_ID_DRA,
        .type      = AVMEDIA_TYPE_AUDIO,
        .name      = "dra",
        .long_name = NULL_IF_CONFIG_SMALL("DRA (Digital Rise Audio)"),
        .props     = AV_CODEC_PROP_INTRA_ONLY | AV_CODEC_PROP_LOSSY,
    },

    /* subtitle codecs */
''',
     "codec_desc")

need("libavcodec/allcodecs.c",
     "extern const FFCodec ff_opus_decoder;\n",
     "extern const FFCodec ff_opus_decoder;\nextern const FFCodec ff_dra_decoder;\n",
     "allcodecs")
need("libavcodec/parsers.c",
     "extern const FFCodecParser ff_dvbsub_parser;\n",
     "extern const FFCodecParser ff_dvbsub_parser;\nextern const FFCodecParser ff_dra_parser;\n",
     "parsers")
need("libavcodec/Makefile",
     "OBJS-$(CONFIG_ON2AVC_DECODER)          += on2avc.o on2avcdata.o\n",
     "OBJS-$(CONFIG_ON2AVC_DECODER)          += on2avc.o on2avcdata.o\n"
     "OBJS-$(CONFIG_DRA_DECODER)             += libdradec.o dradec.o\n"
     "$(SUBDIR)dradec.o: CFLAGS += -Wno-declaration-after-statement -Wno-unused-function\n",
     "Makefile decoder")
need("libavcodec/Makefile",
     "OBJS-$(CONFIG_DVBSUB_PARSER)           += dvbsub_parser.o\n",
     "OBJS-$(CONFIG_DVBSUB_PARSER)           += dvbsub_parser.o\n"
     "OBJS-$(CONFIG_DRA_PARSER)              += dra_parser.o\n",
     "Makefile parser")
need("libavformat/mpegts.c",
     "    { STREAM_TYPE_VIDEO_AVS3,     AVMEDIA_TYPE_VIDEO, AV_CODEC_ID_AVS3       },\n",
     "    { STREAM_TYPE_VIDEO_AVS3,     AVMEDIA_TYPE_VIDEO, AV_CODEC_ID_AVS3       },\n"
     "    { 0xda,                       AVMEDIA_TYPE_AUDIO, AV_CODEC_ID_DRA        },\n",
     "mpegts type")
need("libavformat/mpegts.c",
     "    { MKTAG('O', 'p', 'u', 's'), AVMEDIA_TYPE_AUDIO, AV_CODEC_ID_OPUS  },\n",
     "    { MKTAG('O', 'p', 'u', 's'), AVMEDIA_TYPE_AUDIO, AV_CODEC_ID_OPUS  },\n"
     "    { MKTAG('D', 'R', 'A', '1'), AVMEDIA_TYPE_AUDIO, AV_CODEC_ID_DRA   },\n"
     "    { MKTAG('d', 'r', 'a', '1'), AVMEDIA_TYPE_AUDIO, AV_CODEC_ID_DRA   },\n",
     "mpegts fourcc")
need("libavformat/mpegts.c",
     """                    pes->pes_header_size += 5;
                    p += 5;
                    buf_size -= 5;
                }
""",
     """                    pes->pes_header_size += 5;
                    p += 5;
                    buf_size -= 5;
                }
                if (pes->st && pes->st->codecpar->codec_id == AV_CODEC_ID_NONE
                    && buf_size >= 2 && AV_RB16(p) == 0x7FFF) {
                    pes->st->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
                    pes->st->codecpar->codec_id = AV_CODEC_ID_DRA;
                }
""",
     "mpegts sync")
need("libavformat/isom_tags.c",
     "    { AV_CODEC_ID_OPUS,            MKTAG('O', 'p', 'u', 's') }, /* mp4ra.org */\n",
     "    { AV_CODEC_ID_OPUS,            MKTAG('O', 'p', 'u', 's') }, /* mp4ra.org */\n"
     "    { AV_CODEC_ID_DRA,             MKTAG('D', 'R', 'A', '1') },\n"
     "    { AV_CODEC_ID_DRA,             MKTAG('d', 'r', 'a', '1') },\n",
     "isom")
sys.stderr.write("ok: DRA overlay -> %s\n" % src)
PY
touch "$SRC/.msmedia-dra-patched"
echo "ok: DRA -> $SRC"
