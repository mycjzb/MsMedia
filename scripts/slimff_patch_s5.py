#!/usr/bin/env python3
# 阶段 5（路线 B 零功能损失瘦身第 2 项）：只修改 slimff 源码副本 build/single-analysis/ffmpeg-9.1-slimff-src。
# 所有改动都包在 MS_SLIM_S5 宏守卫里：只有 TRIM=s5 时 configure --extra-cflags 带 -DMS_SLIM_S5=1 才生效，
# 未定义时与原 s4 源码逐字等价（同一副本仍可重建 s4 前缀作对照）。脚本幂等，可重复执行。
#   1) h264_parser：只用到 H264DSPContext.startcode_find_candidate（无汇编时恒为 C 版），直接调用
#      ff_startcode_find_candidate_c，不再为此拉入整个 h264dsp.o（H.264 视频软解本就未开）。
#   2) exif.c：EXIF 仅用于 HEIF 图片项/AVI 元数据/图片解码器帧侧数据，直播/点播音视频播放不需要；
#      解析接口改为“无 EXIF 条目”（返回 0、空 IFD），写/设置返回 ENOSYS；方向矩阵换算保留原实现。
#   3) pixdesc.c：像素格式描述表压缩为常用 YUV/NV12/P010/RGB/GRAY/PAL8 + 全部硬件格式；
#      未保留格式 av_pix_fmt_desc_get 返回 NULL（文档允许），名字查询返回 NULL/NONE。
#   4) codec_desc.c：只保留常见视频/音频（全部 PCM）与全部字幕/数据/附件类 codec descriptor；
#      其余 avcodec_descriptor_get 安全返回 NULL（avcodec_get_name 回退到 "unknown_codec"）。
#   5) AVOption 帮助文本：第 2 个字段 help 包成 MS_OPT_HELP("...")，S5 下为 NULL；
#      .name 键名/偏移/类型/默认值/unit 一律不动，av_opt_set / AVDictionary 选项照常生效。
# by: Long <mycjzb@gmail.com>
import os, re, sys

SRC = sys.argv[1]
TAG = "/* msmedia s5 */"

def rd(p):
    with open(os.path.join(SRC, p), encoding="utf-8", errors="surrogateescape") as f:
        return f.read()

def wr(p, s):
    with open(os.path.join(SRC, p), "w", encoding="utf-8", errors="surrogateescape") as f:
        f.write(s)

def once(s, a, b, what):
    if s.count(a) != 1:
        sys.exit("slimff_patch_s5: anchor not unique for %s" % what)
    return s.replace(a, b)

# ---------- 1) h264_parser ----------
p = "libavcodec/h264_parser.c"
s = rd(p)
if TAG not in s:
    s = once(s, "            i += p->h264dsp.startcode_find_candidate(buf + i, next_avc - i);",
             "#if MS_SLIM_S5 && !defined(MS_S5_SKIP_H264DSP) " + TAG + "\n"
             "            /* 无汇编构建里 h264dsp 的该函数指针恒为 C 版，直接调用，免拉 h264dsp.o。by: Long <mycjzb@gmail.com> */\n"
             "            i += ff_startcode_find_candidate_c(buf + i, next_avc - i);\n"
             "#else\n"
             "            i += p->h264dsp.startcode_find_candidate(buf + i, next_avc - i);\n"
             "#endif", "h264 candidate")
    s = once(s, "    ff_h264dsp_init(&p->h264dsp, 8, 1);",
             "#if !MS_SLIM_S5 || defined(MS_S5_SKIP_H264DSP) " + TAG + "\n    ff_h264dsp_init(&p->h264dsp, 8, 1);\n#endif", "h264dsp init")
    wr(p, s)

# ---------- 2) exif ----------
p = "libavcodec/exif.c"
s = rd(p)
if TAG not in s:
    s = once(s, '#include "tiff_common.h"\n', '#include "tiff_common.h"\n\n#if !MS_SLIM_S5 || defined(MS_S5_SKIP_EXIF) ' + TAG + "\n", "exif inc")
    s = once(s, "static const int rotation_lut[2][4] = {",
             "#else /* MS_SLIM_S5：EXIF 解析/写入摘除，接口按“无 EXIF 条目”工作。by: Long <mycjzb@gmail.com> */\n"
             "const char *av_exif_get_tag_name(uint16_t id) { (void)id; return NULL; }\n"
             "int32_t av_exif_get_tag_id(const char *name) { (void)name; return -1; }\n"
             "void av_exif_free(AVExifMetadata *ifd) { if (ifd) { av_freep(&ifd->entries); ifd->count = 0; ifd->size = 0; } }\n"
             "int av_exif_write(void *logctx, const AVExifMetadata *ifd, AVBufferRef **buffer, enum AVExifHeaderMode header_mode)\n"
             "{ (void)logctx; (void)ifd; (void)header_mode; if (buffer) *buffer = NULL; return AVERROR(ENOSYS); }\n"
             "int av_exif_parse_buffer(void *logctx, const uint8_t *buf, size_t size, AVExifMetadata *ifd, enum AVExifHeaderMode header_mode)\n"
             "{ (void)logctx; (void)buf; (void)size; (void)header_mode; if (ifd) memset(ifd, 0, sizeof(*ifd)); return 0; }\n"
             "int av_exif_ifd_to_dict(void *logctx, const AVExifMetadata *ifd, AVDictionary **metadata)\n"
             "{ (void)logctx; (void)ifd; (void)metadata; return 0; }\n"
             "int av_exif_get_entry(void *logctx, AVExifMetadata *ifd, uint16_t id, int flags, AVExifEntry **value)\n"
             "{ (void)logctx; (void)ifd; (void)id; (void)flags; if (value) *value = NULL; return 0; }\n"
             "int av_exif_set_entry(void *logctx, AVExifMetadata *ifd, uint16_t id, enum AVTiffDataType type,\n"
             "                      uint32_t count, const uint8_t *ifd_lead, uint32_t ifd_offset, const void *value)\n"
             "{ (void)logctx; (void)ifd; (void)id; (void)type; (void)count; (void)ifd_lead; (void)ifd_offset; (void)value; return AVERROR(ENOSYS); }\n"
             "int av_exif_remove_entry(void *logctx, AVExifMetadata *ifd, uint16_t id, int flags)\n"
             "{ (void)logctx; (void)ifd; (void)id; (void)flags; return 0; }\n"
             "AVExifMetadata *av_exif_clone_ifd(const AVExifMetadata *ifd) { (void)ifd; return av_mallocz(sizeof(AVExifMetadata)); }\n"
             "#endif\n\n"
             "static const int rotation_lut[2][4] = {", "exif mid")
    s = once(s, "int ff_exif_sanitize_ifd(void *logctx, const AVFrame *frame, AVExifMetadata *ifd)\n{",
             "#if !MS_SLIM_S5 || defined(MS_S5_SKIP_EXIF) " + TAG + "\nint ff_exif_sanitize_ifd(void *logctx, const AVFrame *frame, AVExifMetadata *ifd)\n{", "exif sanitize")
    s = s.rstrip("\n") + "\n#else /* MS_SLIM_S5 by: Long <mycjzb@gmail.com> */\n" \
        "int ff_exif_sanitize_ifd(void *logctx, const AVFrame *frame, AVExifMetadata *ifd)\n" \
        "{ (void)logctx; (void)frame; (void)ifd; return 0; }\n" \
        "int ff_exif_get_buffer(void *logctx, const AVFrame *frame, AVBufferRef **buffer_ptr, enum AVExifHeaderMode header_mode)\n" \
        "{ (void)logctx; (void)frame; (void)header_mode; if (buffer_ptr) *buffer_ptr = NULL; return 0; }\n#endif\n"
    wr(p, s)

# ---------- helpers: 平衡括号扫描（跳过字符串/字符/注释） ----------
def match_brace(s, i):
    """s[i] == '{'，返回匹配 '}' 的下标。"""
    depth, n = 0, len(s)
    while i < n:
        c = s[i]
        if c == '"' or c == "'":
            q = c; i += 1
            while i < n and s[i] != q:
                i += 2 if s[i] == "\\" else 1
        elif s.startswith("/*", i):
            i = s.index("*/", i + 2) + 1
        elif s.startswith("//", i):
            i = s.index("\n", i)
        elif c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                return i
        i += 1
    raise ValueError("unbalanced")

# ---------- 3) pixdesc ----------
PIX_KEEP = """YUV420P YUYV422 UYVY422 RGB24 BGR24 YUV422P YUV444P GRAY8 PAL8 YUVJ420P YUVJ422P YUVJ444P
NV12 NV21 NV16 NV24 NV42 ARGB RGBA ABGR BGRA 0RGB RGB0 0BGR BGR0 RGB565BE RGB565LE RGB555BE RGB555LE
GRAY10BE GRAY10LE GRAY16BE GRAY16LE YUVA420P YUV420P10BE YUV420P10LE YUV422P10BE YUV422P10LE
YUV444P10BE YUV444P10LE YUV420P12BE YUV420P12LE YUV422P12BE YUV422P12LE YUV444P12BE YUV444P12LE
YUV420P16BE YUV420P16LE P010BE P010LE P012BE P012LE P016BE P016LE P210BE P210LE P410BE P410LE
RGB48BE RGB48LE RGBA64BE RGBA64LE X2RGB10LE X2RGB10BE X2BGR10LE X2BGR10BE
VAAPI VDPAU VIDEOTOOLBOX MEDIACODEC CUDA QSV D3D11 D3D11VA_VLD D3D12 DXVA2_VLD DRM_PRIME OPENCL VULKAN
MMAL AMF_SURFACE OHCODEC""".split()
p = "libavutil/pixdesc.c"
s = rd(p)
if TAG not in s:
    head = "static const AVPixFmtDescriptor av_pix_fmt_descriptors[AV_PIX_FMT_NB] = {"
    i0 = s.index(head)
    ob = i0 + len(head) - 1
    cb = match_brace(s, ob)
    body = s[ob + 1:cb]
    # 逐条拆分 "[AV_PIX_FMT_X] = { ... }," 与其间的 #define（BAYER*_DESC_COMMON）
    kept, defines, j = [], [], 0
    ent = re.compile(r"\[AV_PIX_FMT_([A-Z0-9_]+)\]\s*=\s*\{")
    while True:
        m = ent.search(body, j)
        # 区间内的预处理行原样保留到压缩表
        seg = body[j:m.start() if m else len(body)]
        for dm in re.finditer(r"^#define[^\n]*(?:\\\n[^\n]*)*", seg, re.M):
            defines.append(dm.group(0))
        if not m:
            break
        e_ob = m.end() - 1
        e_cb = match_brace(body, e_ob)
        name = m.group(1)
        if name in PIX_KEEP:
            kept.append((name, body[e_ob:e_cb + 1]))
        j = e_cb + 1
    if len(kept) > 250:
        sys.exit("slimff_patch_s5: too many pixdesc kept")
    comp = ["#if MS_SLIM_S5 && !defined(MS_S5_SKIP_PIXDESC) " + TAG,
            "/* 压缩像素格式表：只保留常用软件格式 + 全部硬件格式，索引表映射枚举→压缩下标+1（0 表示未保留）。",
            " * by: Long <mycjzb@gmail.com> */"]
    # 区间内 #define（BAYER*_DESC_COMMON）只服务 Bayer 条目；保留表不含 Bayer，若含则报错避免静默错表。
    for name, txt in kept:
        if "DESC_COMMON" in txt:
            sys.exit("slimff_patch_s5: kept pixdesc entry uses a local macro: " + name)
    comp.append("static const AVPixFmtDescriptor ms_pd_tbl[] = {")
    for name, txt in kept:
        comp.append("    /* AV_PIX_FMT_%s */ %s," % (name, txt))
    comp.append("};")
    comp.append("static const uint8_t ms_pd_idx[AV_PIX_FMT_NB] = {")
    for k, (name, _) in enumerate(kept):
        comp.append("    [AV_PIX_FMT_%s] = %d," % (name, k + 1))
    comp.append("};")
    comp.append("#define MS_PD_GET(f) (((unsigned)(f) < AV_PIX_FMT_NB && ms_pd_idx[(f)]) ? &ms_pd_tbl[ms_pd_idx[(f)] - 1] : NULL)")
    comp.append("#define av_pix_fmt_descriptors ms_pd_tbl")
    comp.append("#else")
    s = s[:i0] + "\n".join(comp) + "\n" + s[i0:cb + 2] + "\n#endif\n" + s[cb + 2:]
    # 直接下标访问的函数：S5 下改走 MS_PD_GET
    s = once(s, """    for (pix_fmt = 0; pix_fmt < AV_PIX_FMT_NB; pix_fmt++)
        if (av_pix_fmt_descriptors[pix_fmt].name &&""",
             """#if MS_SLIM_S5 && !defined(MS_S5_SKIP_PIXDESC) """ + TAG + """
    for (pix_fmt = 0; pix_fmt < AV_PIX_FMT_NB; pix_fmt++) {
        const AVPixFmtDescriptor *d = MS_PD_GET(pix_fmt);
        if (d && d->name && (!strcmp(d->name, name) || av_match_name(name, d->alias)))
            return pix_fmt;
    }
    return AV_PIX_FMT_NONE;
#endif
    for (pix_fmt = 0; pix_fmt < AV_PIX_FMT_NB; pix_fmt++)
        if (av_pix_fmt_descriptors[pix_fmt].name &&""", "pd internal")
    s = once(s, """    return (unsigned)pix_fmt < AV_PIX_FMT_NB ?
        av_pix_fmt_descriptors[pix_fmt].name : NULL;""",
             """#if MS_SLIM_S5 && !defined(MS_S5_SKIP_PIXDESC) """ + TAG + """
    const AVPixFmtDescriptor *d = MS_PD_GET(pix_fmt);
    return d ? d->name : NULL;
#else
    return (unsigned)pix_fmt < AV_PIX_FMT_NB ?
        av_pix_fmt_descriptors[pix_fmt].name : NULL;
#endif""", "pd name")
    s = once(s, """        const AVPixFmtDescriptor *pixdesc = &av_pix_fmt_descriptors[pix_fmt];
        snprintf(buf, buf_size,""",
             """#if MS_SLIM_S5 && !defined(MS_S5_SKIP_PIXDESC) """ + TAG + """
        const AVPixFmtDescriptor *pixdesc = MS_PD_GET(pix_fmt);
        if (!pixdesc) {
            snprintf(buf, buf_size, "%-11s %7d %10d", "unknown", 0, 0);
            return buf;
        }
#else
        const AVPixFmtDescriptor *pixdesc = &av_pix_fmt_descriptors[pix_fmt];
#endif
        snprintf(buf, buf_size,""", "pd string")
    s = once(s, """    if (pix_fmt < 0 || pix_fmt >= AV_PIX_FMT_NB)
        return NULL;
    return &av_pix_fmt_descriptors[pix_fmt];""",
             """#if MS_SLIM_S5 && !defined(MS_S5_SKIP_PIXDESC) """ + TAG + """
    return MS_PD_GET(pix_fmt);
#else
    if (pix_fmt < 0 || pix_fmt >= AV_PIX_FMT_NB)
        return NULL;
    return &av_pix_fmt_descriptors[pix_fmt];
#endif""", "pd get")
    s = once(s, """    if (desc < av_pix_fmt_descriptors ||
        desc >= av_pix_fmt_descriptors + FF_ARRAY_ELEMS(av_pix_fmt_descriptors))
        return AV_PIX_FMT_NONE;

    return desc - av_pix_fmt_descriptors;""",
             """    if (desc < av_pix_fmt_descriptors ||
        desc >= av_pix_fmt_descriptors + FF_ARRAY_ELEMS(av_pix_fmt_descriptors))
        return AV_PIX_FMT_NONE;
#if MS_SLIM_S5 && !defined(MS_S5_SKIP_PIXDESC) """ + TAG + """
    for (int f = 0; f < AV_PIX_FMT_NB; f++)
        if (MS_PD_GET(f) == desc)
            return f;
    return AV_PIX_FMT_NONE;
#else
    return desc - av_pix_fmt_descriptors;
#endif""", "pd id")
    wr(p, s)

# ---------- 4) codec_desc ----------
CD_KEEP = set("""MPEG1VIDEO MPEG2VIDEO H261 H263 H263P H263I MPEG4 MSMPEG4V1 MSMPEG4V2 MSMPEG4V3 WMV1 WMV2 WMV3 VC1
H264 HEVC VVC AV1 VP8 VP9 VP6 VP6F VP6A FLV1 CAVS AVS2 AVS3 MJPEG RAWVIDEO THEORA PRORES DNXHD
PNG BMP GIF WEBP
MP1 MP2 MP3 MP3ADU MP3ON4 AAC AAC_LATM AC3 EAC3 AC4 DTS TRUEHD MLP FLAC ALAC OPUS VORBIS SPEEX
AMR_NB AMR_WB GSM GSM_MS G723_1 G729 QCELP ILBC NELLYMOSER WMAV1 WMAV2 WMAPRO DOLBY_E S302M SBC
MPEGH_3D_AUDIO TTA WAVPACK APE
ADPCM_IMA_QT ADPCM_IMA_WAV ADPCM_MS ADPCM_G722 ADPCM_G726 ADPCM_G726LE ADPCM_SWF ADPCM_YAMAHA""".split())
p = "libavcodec/codec_desc.c"
s = rd(p)
if TAG not in s:
    head = "static const AVCodecDescriptor codec_descriptors[] = {"
    i0 = s.index(head)
    ob = i0 + len(head) - 1
    cb = match_brace(s, ob)
    body = s[ob + 1:cb]
    out, j, removed, keptn = [], 0, 0, 0
    while True:
        k = body.find("{", j)
        if k < 0:
            out.append(body[j:]); break
        e_cb = match_brace(body, k)
        txt = body[k:e_cb + 1]
        m_id = re.search(r"\.id\s*=\s*AV_CODEC_ID_([A-Z0-9_]+)", txt)
        m_ty = re.search(r"\.type\s*=\s*(AVMEDIA_TYPE_[A-Z]+)", txt)
        # 取到逗号为止
        end = e_cb + 1
        if end < len(body) and body[end] == ",":
            end += 1
        out.append(body[j:k])
        cid = m_id.group(1) if m_id else ""
        ty = m_ty.group(1) if m_ty else ""
        keep = (ty in ("AVMEDIA_TYPE_SUBTITLE", "AVMEDIA_TYPE_DATA", "AVMEDIA_TYPE_ATTACHMENT")
                or cid.startswith("PCM_") or cid in CD_KEEP or not m_id)
        if keep:
            out.append(body[k:end]); keptn += 1
        else:
            out.append("\n#if !MS_SLIM_S5 || defined(MS_S5_SKIP_CODECDESC)\n    " + body[k:end] + "\n#endif\n"); removed += 1
        j = end
    s = s[:ob + 1] + "\n" + TAG + "\n/* MS_SLIM_S5：只保留常见视频/音频(全部 PCM)/全部字幕/数据/附件描述符（保留 %d 条，摘除 %d 条）。by: Long <mycjzb@gmail.com> */" % (keptn, removed) + "".join(out) + s[cb:]
    wr(p, s)
    print("codec_desc: kept=%d removed=%d" % (keptn, removed))

# ---------- 5) AVOption help ----------
p = "libavutil/opt.h"
s = rd(p)
if "MS_OPT_HELP" not in s:
    s = once(s, "typedef struct AVOption {",
             "/* msmedia s5：AVOption 帮助文本仅供命令行 -h 展示；MS_SLIM_S5 下置 NULL，键名不动。by: Long <mycjzb@gmail.com> */\n"
             "#ifndef MS_OPT_HELP\n#if defined(MS_SLIM_S5) && MS_SLIM_S5 && !defined(MS_S5_SKIP_OPTHELP)\n#define MS_OPT_HELP(x) NULL\n#else\n#define MS_OPT_HELP(x) x\n#endif\n#endif\n\n"
             "typedef struct AVOption {", "opt.h")
    wr(p, s)
STR = r'"(?:[^"\\\n]|\\.)*"'
ent = re.compile(r"(\{\s*" + STR + r"\s*,\s*)((?:" + STR + r"\s*)+?)(\s*,)")
arr = re.compile(r"AVOption\s+\w+\s*\[\s*\]\s*=\s*\{")
total = 0
for d in ("libavutil", "libavcodec", "libavformat"):
    for fn in sorted(os.listdir(os.path.join(SRC, d))):
        if not (fn.endswith(".c") or fn.endswith(".h")):
            continue
        rel = d + "/" + fn
        s = rd(rel)
        if "AVOption" not in s:
            continue
        pos, out, n = 0, [], 0
        for m in arr.finditer(s):
            if m.start() < pos:
                continue
            ob = m.end() - 1
            try:
                cb = match_brace(s, ob)
            except Exception:
                continue
            blk = s[ob:cb]
            def sub(mm):
                return mm.group(1) + "MS_OPT_HELP(" + mm.group(2).rstrip() + ")" + mm.group(3)
            nb, c = ent.subn(sub, blk)
            n += c
            out.append(s[pos:ob]); out.append(nb); pos = cb
        if n:
            out.append(s[pos:])
            wr(rel, "".join(out))
            total += n
# options_table.h 也是 AVOption 数组（在 .h 内以数组形式出现），已被上面循环覆盖。
print("avoption help wrapped: %d" % total)
print("slimff_patch_s5: ok")
