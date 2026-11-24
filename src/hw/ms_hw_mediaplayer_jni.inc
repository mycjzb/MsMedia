/*
 * com.mycjzb.media.player.MediaPlayer 的原生通道（只在单 so，MS_SINGLE_SO 时由 ms_hw_player_jni.c 末尾包含）。
 * 复用同一 player_jni 会话/worker：路线 B（MS_HW_HAVE_FF）网络与文件走 ms_ff；
 * 路线 A 只有本地文件（ms_file_source + ms_session）。不支持的输入诚实返回错误码，不假装成功。
 * 对 Java 只暴露毫秒；90 kHz 只在这里换算。
 * by: Long <mycjzb@gmail.com>
 */
#include "../netcore/ms_netcore.h"

/* 与 Java MediaPlayer 约定的错误码（系统 MEDIA_ERROR_* extra 值）。 */
#define MP_ERR_UNSUPPORTED (-1010)
#define MP_ERR_IO (-1004)

JNIEXPORT jlong JNICALL
Java_com_mycjzb_media_player_MediaPlayer_nativeSetup(JNIEnv *env, jclass cls)
{
    (void)cls;
    (void)ms_nc_jni_init(env);
    return Java_com_mycjzb_media_player_MsHwPlayerActivity_nativeCreate(env, NULL);
}

JNIEXPORT void JNICALL
Java_com_mycjzb_media_player_MediaPlayer_nativeRelease(JNIEnv *env, jclass cls, jlong h)
{
    (void)cls;
    Java_com_mycjzb_media_player_MsHwPlayerActivity_nativeDestroy(env, NULL, h);
}

JNIEXPORT void JNICALL
Java_com_mycjzb_media_player_MediaPlayer_nativeSetSurface(JNIEnv *env, jclass cls, jlong h, jobject surface)
{
    jstring r;
    (void)cls;
    r = Java_com_mycjzb_media_player_MsHwPlayerActivity_nativeSetSurface(env, NULL, h, surface);
    if (r) (*env)->DeleteLocalRef(env, r);
}

JNIEXPORT jboolean JNICALL
Java_com_mycjzb_media_player_MediaPlayer_nativeHasFf(JNIEnv *env, jclass cls)
{
    (void)env;
    (void)cls;
#ifdef MS_HW_HAVE_FF
    return JNI_TRUE;
#else
    return JNI_FALSE;
#endif
}

/* 排队打开：以暂停态开始（prepare 语义），worker 打开并识别后 mp_opened=1。
 * 返回 0=已排队；MP_ERR_UNSUPPORTED=本变体不支持；MP_ERR_IO=其它失败。 */
JNIEXPORT jint JNICALL
Java_com_mycjzb_media_player_MediaPlayer_nativePrepare(JNIEnv *env, jclass cls, jlong h, jstring jloc,
                                           jboolean isUrl, jint timeoutMs)
{
    player_jni *pj = pj_from(h);
    const char *loc;
    int q;
    (void)cls;
    if (pj == NULL || pj->p == NULL || jloc == NULL) {
        return MP_ERR_IO;
    }
    loc = (*env)->GetStringUTFChars(env, jloc, NULL);
    if (loc == NULL || loc[0] == 0) {
        if (loc) (*env)->ReleaseStringUTFChars(env, jloc, loc);
        return MP_ERR_IO;
    }
#ifndef MS_HW_HAVE_FF
    /* 路线 A：纯自研网络栈（ms_netsrc）支持 http/https/HLS/DASH/udp/rtp/rtmp(s)/rtsp；其它方案诚实不支持 */
    if (isUrl && (!ms_netsrc_url_supported(loc) || strncmp(loc, "file://", 7) == 0)) {
        (*env)->ReleaseStringUTFChars(env, jloc, loc);
        return MP_ERR_UNSUPPORTED;
    }
#endif
    atomic_store(&pj->mp_opened, 0);
    pthread_mutex_lock(&pj->op);
    (void)ms_hw_player_pause(pj->p);
    pthread_mutex_unlock(&pj->op);
#ifdef MS_HW_HAVE_FF
    if (isUrl) {
        q = queue_session(pj, 1, loc, NULL, timeoutMs > 0 ? (int)timeoutMs : 15000);
    } else {
        char fileurl[1024];
        if (snprintf(fileurl, sizeof(fileurl), "file:%s", loc) >= (int)sizeof(fileurl)) {
            (*env)->ReleaseStringUTFChars(env, jloc, loc);
            return MP_ERR_IO;
        }
        q = queue_session(pj, 1, fileurl, NULL, timeoutMs > 0 ? (int)timeoutMs : 15000);
    }
#else
    q = queue_session(pj, isUrl ? 1 : 2, loc, NULL, timeoutMs > 0 ? (int)timeoutMs : 15000);
#endif
    (*env)->ReleaseStringUTFChars(env, jloc, loc);
    return q == 0 ? 0 : MP_ERR_IO;
}

/* 状态快照（Java 主线程轮询）：
 * out[0]=opened out[1]=done_gen out[2]=pos_ms out[3]=dur_ms(-1 未知) out[4]=w out[5]=h
 * out[6]=paused out[7]=seekable(-1/0/1) out[8]=have_first_output out[9]=busy(拿不到锁)
 * out[10]=last_error(ms_status) out[11]=active(worker 正跑会话) out[12]=pending(已排队未开始) */
JNIEXPORT void JNICALL
Java_com_mycjzb_media_player_MediaPlayer_nativeQuery(JNIEnv *env, jclass cls, jlong h, jlongArray jout)
{
    player_jni *pj = pj_from(h);
    jlong v[13];
    (void)cls;
    memset(v, 0, sizeof(v));
    v[3] = -1;
    v[7] = -1;
    if (pj == NULL || pj->p == NULL || jout == NULL || (*env)->GetArrayLength(env, jout) < 13) {
        return;
    }
    v[0] = atomic_load(&pj->mp_opened);
    v[1] = atomic_load(&pj->mp_done);
    v[11] = atomic_load(&pj->mp_active);
    pthread_mutex_lock(&pj->mu);
    v[12] = pj->has_pending;
    pthread_mutex_unlock(&pj->mu);
    if (lock_op_ms(pj, 15)) {
        ms_hw_player_stats st;
        int w = 0, hh = 0;
        int64_t pos = ms_hw_player_position_90k(pj->p);
        int64_t dur = ms_hw_player_duration_90k(pj->p);
        memset(&st, 0, sizeof(st));
        st.struct_size = sizeof(st);
        (void)ms_hw_player_get_stats(pj->p, &st);
        (void)ms_hw_player_video_size(pj->p, &w, &hh);
        v[2] = pos > 0 ? pos / 90 : 0;
        v[3] = dur > 0 ? dur / 90 : -1;
        v[4] = w;
        v[5] = hh;
        v[6] = ms_hw_player_is_paused(pj->p);
        v[7] = ms_hw_player_is_seekable(pj->p);
        v[8] = st.have_first_output;
        v[10] = st.last_error;
        pthread_mutex_unlock(&pj->op);
    } else {
        v[9] = 1;
    }
    (*env)->SetLongArrayRegion(env, jout, 0, 13, v);
}

JNIEXPORT void JNICALL
Java_com_mycjzb_media_player_MediaPlayer_nativeStart(JNIEnv *env, jclass cls, jlong h)
{
    jstring r;
    (void)cls;
    r = Java_com_mycjzb_media_player_MsHwPlayerActivity_nativeResume(env, NULL, h);
    if (r) (*env)->DeleteLocalRef(env, r);
}

JNIEXPORT void JNICALL
Java_com_mycjzb_media_player_MediaPlayer_nativePause(JNIEnv *env, jclass cls, jlong h)
{
    jstring r;
    (void)cls;
    r = Java_com_mycjzb_media_player_MsHwPlayerActivity_nativePause(env, NULL, h);
    if (r) (*env)->DeleteLocalRef(env, r);
}

/* stop/reset：取消当前会话（worker 保留，可再次 prepare）。 */
JNIEXPORT void JNICALL
Java_com_mycjzb_media_player_MediaPlayer_nativeStop(JNIEnv *env, jclass cls, jlong h)
{
    player_jni *pj = pj_from(h);
    (void)cls;
    if (pj == NULL) return;
    (void)env;
    request_stop_io(pj); /* 只停 I/O 与会话，不把 player 置为永久 canceled，便于再次 prepare */
    atomic_store(&pj->mp_opened, 0);
}

/* 毫秒 -> 90 kHz；返回 0=已排队，MP_ERR_UNSUPPORTED=不可 seek。 */
JNIEXPORT jint JNICALL
Java_com_mycjzb_media_player_MediaPlayer_nativeSeekTo(JNIEnv *env, jclass cls, jlong h, jlong msec)
{
    player_jni *pj = pj_from(h);
    int seekable;
    jstring r;
    (void)cls;
    if (pj == NULL || pj->p == NULL) return MP_ERR_IO;
    if (msec < 0) msec = 0;
    if (lock_op_ms(pj, 50)) {
        seekable = ms_hw_player_is_seekable(pj->p);
        pthread_mutex_unlock(&pj->op);
        if (seekable == 0) return MP_ERR_UNSUPPORTED;
    }
    r = Java_com_mycjzb_media_player_MsHwPlayerActivity_nativeSeek(env, NULL, h, (jlong)msec * 90);
    if (r) (*env)->DeleteLocalRef(env, r);
    return 0;
}

JNIEXPORT void JNICALL
Java_com_mycjzb_media_player_MediaPlayer_nativeSetVolume(JNIEnv *env, jclass cls, jlong h, jfloat l, jfloat rr)
{
    player_jni *pj = pj_from(h);
    (void)env;
    (void)cls;
    if (pj == NULL || pj->p == NULL) return;
    if (lock_op_ms(pj, 50)) {
        (void)ms_hw_player_set_volume(pj->p, l, rr);
        pthread_mutex_unlock(&pj->op);
    }
}

JNIEXPORT void JNICALL
Java_com_mycjzb_media_player_MediaPlayer_nativeSetLooping(JNIEnv *env, jclass cls, jlong h, jboolean on)
{
    player_jni *pj = pj_from(h);
    (void)env;
    (void)cls;
    if (pj) atomic_store(&pj->mp_looping, on ? 1 : 0);
}

/* 速率：PlaybackParams.speed 换成有理数（千分之一精度）；返回 ms_status。 */
JNIEXPORT jint JNICALL
Java_com_mycjzb_media_player_MediaPlayer_nativeSetSpeed(JNIEnv *env, jclass cls, jlong h, jfloat speed)
{
    player_jni *pj = pj_from(h);
    int st = MS_ERR_INVALID_ARG;
    (void)env;
    (void)cls;
    if (pj == NULL || pj->p == NULL || !(speed > 0.f) || speed > 8.f) return MS_ERR_INVALID_ARG;
    if (lock_op_ms(pj, 50)) {
        st = ms_hw_player_set_rate(pj->p, (int)(speed * 1000.f + 0.5f), 1000);
        pthread_mutex_unlock(&pj->op);
    }
    return st;
}

/* 起播耗时拆分（微秒，均相对 prepare 入队 mark_request；未发生为 -1）：
 * [0]worker 开始 [1]open_input 完成（含建连）[2]find_stream_info 完成 [3]首包 [4]首个可解码 AU
 * [5]首次送解 [6]首帧输出到 Surface [7]OnFrameRendered [8]open_input 耗时 [9]find_stream_info 耗时
 * 输出到 Surface 不等于屏幕显示；模拟器数值不代表真机。 */
JNIEXPORT void JNICALL
Java_com_mycjzb_media_player_MediaPlayer_nativeStartupStats(JNIEnv *env, jclass cls, jlong h, jlongArray jout)
{
    player_jni *pj = pj_from(h);
    jlong v[10];
    int i;
    (void)cls;
    for (i = 0; i < 10; i++) v[i] = -1;
    if (pj == NULL || pj->p == NULL || jout == NULL || (*env)->GetArrayLength(env, jout) < 10) {
        return;
    }
    if (lock_op_ms(pj, 50)) {
        ms_hw_player_stats st;
        memset(&st, 0, sizeof(st));
        st.struct_size = sizeof(st);
        if (ms_hw_player_get_stats(pj->p, &st) == MS_OK && st.t_request_us >= 0) {
            v[0] = st.have_worker_start ? st.t_req_worker_start_us : -1;
            v[1] = st.have_open_phases ? st.t_req_open_input_done_us : -1;
            v[2] = st.have_open_phases ? st.t_req_find_stream_info_done_us : -1;
            v[3] = st.have_req_first_packet ? st.t_req_first_packet_us : -1;
            v[4] = st.have_req_first_decodable_au ? st.t_req_first_decodable_au_us : -1;
            v[5] = st.have_req_first_submit ? st.t_req_first_submit_us : -1;
            v[6] = st.have_req_first_output ? st.t_req_first_output_us : -1;
            v[7] = st.have_req_first_codec_rendered ? st.t_req_first_codec_rendered_us : -1;
            v[8] = st.have_open_phases ? st.dur_open_input_us : -1;
            v[9] = st.have_open_phases ? st.dur_find_stream_info_us : -1;
        }
        pthread_mutex_unlock(&pj->op);
    }
    (*env)->SetLongArrayRegion(env, jout, 0, 10, v);
}

/* 阶段 4 音频直通（对齐 Media3）：caps 为 Java 探测的 MS_PT_CAP_* 位（含开关位）。
 * 下一次音频配置（prepare）生效；设备不支持或打开失败时 C 层自动回退软解。by: Long <mycjzb@gmail.com> */
/* 内部接口（ms_hw_internal.h 同名声明；此处避免引入整个内部头）。 */
void ms_hw_player_set_passthrough_caps(ms_hw_player *p, unsigned caps);
int ms_hw_player_passthrough_route(ms_hw_player *p, unsigned *fallbacks);

JNIEXPORT void JNICALL
Java_com_mycjzb_media_player_MediaPlayer_nativeSetAudioPassthrough(JNIEnv *env, jclass cls, jlong h, jint caps)
{
    player_jni *pj = pj_from(h);
    (void)env;
    (void)cls;
    if (pj == NULL || pj->p == NULL) {
        return;
    }
    pthread_mutex_lock(&pj->op);
    ms_hw_player_set_passthrough_caps(pj->p, (unsigned)caps);
    pthread_mutex_unlock(&pj->op);
}

/* 当前音频路由：0 软解/系统解码，1 原始帧直通，2 IEC61937 直通。 */
JNIEXPORT jint JNICALL
Java_com_mycjzb_media_player_MediaPlayer_nativeGetAudioPassthroughRoute(JNIEnv *env, jclass cls, jlong h)
{
    player_jni *pj = pj_from(h);
    int r = 0;
    (void)env;
    (void)cls;
    if (pj == NULL || pj->p == NULL) {
        return 0;
    }
    pthread_mutex_lock(&pj->op);
    r = ms_hw_player_passthrough_route(pj->p, NULL);
    pthread_mutex_unlock(&pj->op);
    return (jint)r;
}

/* 阶段 6：Media3 风格缓冲水位（路线 B 精简 FFmpeg HLS 独立预下载）。下一次 prepare 生效。
 * Java 层已按 Media3 DefaultLoadControl 规则校验参数；maxBufferMs=0 表示关闭预下载。by: Long <mycjzb@gmail.com> */
JNIEXPORT void JNICALL
Java_com_mycjzb_media_player_MediaPlayer_nativeSetBufferConfig(JNIEnv *env, jclass cls, jlong h, jint minMs, jint maxMs,
                                                   jint playMs, jint rebufMs, jlong maxBytes)
{
    player_jni *pj = pj_from(h);
    (void)env;
    (void)cls;
    if (pj == NULL) {
        return;
    }
    pthread_mutex_lock(&pj->mu);
    pj->buf_set = 1;
    pj->buf_min_ms = minMs;
    pj->buf_max_ms = maxMs;
    pj->buf_play_ms = playMs;
    pj->buf_rebuf_ms = rebufMs;
    pj->buf_max_bytes = (long long)maxBytes;
    pthread_mutex_unlock(&pj->mu);
}

/* 阶段 6：播放拉流 TLS 校验（默认 false=忽略证书链与主机名；true=严格）。MsHttpClient 不受影响，永远严格。 */
JNIEXPORT void JNICALL
Java_com_mycjzb_media_player_MediaPlayer_nativeSetTlsVerify(JNIEnv *env, jclass cls, jlong h, jboolean strict)
{
    player_jni *pj = pj_from(h);
    (void)env;
    (void)cls;
    if (pj == NULL) {
        return;
    }
    pthread_mutex_lock(&pj->mu);
    pj->tls_verify = strict ? 1 : 0;
    pthread_mutex_unlock(&pj->mu);
}

/* 预加载配置：路线 A 纯自研网络栈的提前缓存毫秒数与内存上限；0 = 默认（10000 ms / 16 MiB，绝对上限 128 MiB）。
 * 阶段 6 起路线 B 也兼容：未调 setBufferConfig 时映射为 Media3 水位（见 apply_ff_buffer_tls）。下一次 prepare 生效。 */
JNIEXPORT void JNICALL
Java_com_mycjzb_media_player_MediaPlayer_nativeSetPreload(JNIEnv *env, jclass cls, jlong h, jint aheadMs, jlong maxBytes)
{
    player_jni *pj = pj_from(h);
    (void)env;
    (void)cls;
    if (pj == NULL) {
        return;
    }
    pthread_mutex_lock(&pj->mu);
    pj->ns_ahead_ms = aheadMs > 0 ? (int)aheadMs : 0;
    pj->ns_max_bytes = maxBytes > 0 ? (long)maxBytes : 0;
    pthread_mutex_unlock(&pj->mu);
}

/* ======================================================================
 * 选轨与字幕（对齐 android.media.MediaPlayer 的 getTrackInfo / selectTrack /
 * deselectTrack / getSelectedTrack / OnTimedTextListener / addTimedTextSource）。
 * 复用 ms_hw_player_jni.c 的公共实现 pj_select_track / pj_cues_now_locked / pj_load_ext_sub，
 * 与演示 Activity 走同一条内核路径，不另起平行逻辑。by: Long <mycjzb@gmail.com>
 * ====================================================================== */

/* 与 Java MediaPlayer.TrackInfo.MEDIA_TRACK_TYPE_* 一致。 */
#define MP_TRACK_UNKNOWN 0
#define MP_TRACK_VIDEO 1
#define MP_TRACK_AUDIO 2
#define MP_TRACK_TIMEDTEXT 3
#define MP_TRACK_SUBTITLE 4

#define MP_TRACK_MAX 32
#define MP_TRACK_META 8 /* 每轨 int：id type kind selected default forced codec 保留 */
#define MP_TRACK_STRS 3 /* 每轨字符串：language mime name */
#define MP_CUE_META 12

/* 文字字幕 → TIMEDTEXT（走 OnTimedTextListener）；位图字幕 → SUBTITLE（无文字回调）。 */
static int mp_track_type(const ms_track_info *t)
{
    if (t->kind == MS_TRACK_VIDEO) {
        return MP_TRACK_VIDEO;
    }
    if (t->kind == MS_TRACK_AUDIO) {
        return MP_TRACK_AUDIO;
    }
    if (t->kind != MS_TRACK_SUBTITLE) {
        return MP_TRACK_UNKNOWN;
    }
    switch (t->codec) {
    case MS_CODEC_SUB_TEXT:
    case MS_CODEC_SUB_SRT:
    case MS_CODEC_SUB_WEBVTT:
    case MS_CODEC_SUB_TX3G:
    case MS_CODEC_SUB_ASS:
        return MP_TRACK_TIMEDTEXT;
    default:
        return MP_TRACK_SUBTITLE;
    }
}

/* codec → MediaFormat KEY_MIME（尽量用 Android 已有的 MIME 名）。 */
static const char *mp_codec_mime(ms_codec c)
{
    switch (c) {
    case MS_CODEC_H264: return "video/avc";
    case MS_CODEC_HEVC: return "video/hevc";
    case MS_CODEC_H263: return "video/3gpp";
    case MS_CODEC_MPEG4_PART2: return "video/mp4v-es";
    case MS_CODEC_MPEG2_VIDEO: return "video/mpeg2";
    case MS_CODEC_AV1: return "video/av01";
    case MS_CODEC_AVS_PLUS:
    case MS_CODEC_AVS_FAMILY: return "video/x-avs"; /* AOSP 无 AVS MIME，私有名 */
    case MS_CODEC_AAC:
    case MS_CODEC_AAC_LATM: return "audio/mp4a-latm";
    case MS_CODEC_AC3: return "audio/ac3";
    case MS_CODEC_EAC3: return "audio/eac3";
    case MS_CODEC_MPEG_AUDIO:
    case MS_CODEC_MP3: return "audio/mpeg";
    case MS_CODEC_MP2: return "audio/mpeg-L2";
    case MS_CODEC_OPUS: return "audio/opus";
    case MS_CODEC_G711_ALAW: return "audio/g711-alaw";
    case MS_CODEC_G711_ULAW: return "audio/g711-mlaw";
    case MS_CODEC_FLAC: return "audio/flac";
    case MS_CODEC_PCM: return "audio/raw";
    case MS_CODEC_DTS: return "audio/vnd.dts";
    case MS_CODEC_SUB_SRT: return "application/x-subrip";
    case MS_CODEC_SUB_WEBVTT: return "text/vtt";
    case MS_CODEC_SUB_TX3G: return "text/3gpp-tt";
    case MS_CODEC_SUB_ASS: return "text/x-ssa";
    case MS_CODEC_SUB_TEXT: return "text/plain";
    case MS_CODEC_SUB_DVB: return "application/dvbsubs";
    case MS_CODEC_SUB_PGS: return "application/pgs";
    case MS_CODEC_SUB_VOBSUB: return "application/x-vobsub";
    case MS_CODEC_SUB_BITMAP: return "application/x-bitmap-subtitle";
    default: return "application/octet-stream";
    }
}

/* 容器里的语言/名字不保证是合法 UTF-8；NewStringUTF 遇到非法序列在 CheckJNI 下会直接 abort。
 * 这里只放行 1–3 字节合法序列，其余字节换成 '?'（4 字节 emoji 在 modified UTF-8 里也不合法）。 */
static jstring mp_safe_jstring(JNIEnv *env, const char *s)
{
    char tmp[128];
    size_t i = 0, o = 0;
    if (s == NULL || s[0] == 0) {
        return NULL;
    }
    while (s[i] != 0 && o + 4 < sizeof(tmp)) {
        unsigned char c = (unsigned char)s[i];
        size_t len = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : 0;
        size_t k;
        int ok = len > 0 && !(len == 1 && c == 0);
        if (len == 2 && c < 0xC2) {
            ok = 0; /* 超长编码 */
        }
        for (k = 1; ok && k < len; k++) {
            if (((unsigned char)s[i + k] & 0xC0) != 0x80) {
                ok = 0;
            }
        }
        if (ok && len == 3) {
            unsigned cp = ((c & 0x0Fu) << 12) | (((unsigned char)s[i + 1] & 0x3Fu) << 6) |
                          ((unsigned char)s[i + 2] & 0x3Fu);
            if (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF)) {
                ok = 0; /* 超长编码 / 代理区 */
            }
        }
        if (!ok) {
            tmp[o++] = '?';
            i++;
            continue;
        }
        for (k = 0; k < len; k++) {
            tmp[o++] = s[i + k];
        }
        i += len;
    }
    tmp[o] = 0;
    return (*env)->NewStringUTF(env, tmp);
}

/* 轨道列表：meta 每轨 MP_TRACK_META 个 int，strs 每轨 MP_TRACK_STRS 个字符串。
 * 返回轨数（只填数组放得下的部分）；-1 = 播放器忙（拿不到锁），Java 用上次结果。 */
JNIEXPORT jint JNICALL
Java_com_mycjzb_media_player_MediaPlayer_nativeGetTracks(JNIEnv *env, jclass cls, jlong h,
                                             jintArray jmeta, jobjectArray jstrs)
{
    player_jni *pj = pj_from(h);
    ms_track_info ti[MP_TRACK_MAX];
    jint meta[MP_TRACK_MAX * MP_TRACK_META];
    size_t n = 0, i, cap;
    (void)cls;
    if (pj == NULL || pj->p == NULL || jmeta == NULL || jstrs == NULL) {
        return 0;
    }
    cap = (size_t)(*env)->GetArrayLength(env, jmeta) / MP_TRACK_META;
    if ((size_t)(*env)->GetArrayLength(env, jstrs) / MP_TRACK_STRS < cap) {
        cap = (size_t)(*env)->GetArrayLength(env, jstrs) / MP_TRACK_STRS;
    }
    if (cap > MP_TRACK_MAX) {
        cap = MP_TRACK_MAX;
    }
    memset(ti, 0, sizeof(ti));
    memset(meta, 0, sizeof(meta));
    if (!lock_op_ms(pj, 300)) {
        return -1;
    }
    if (ms_hw_player_list_tracks(pj->p, ti, MP_TRACK_MAX, &n) != MS_OK) {
        n = 0;
    }
#ifdef MS_HW_HAVE_FF
    /* prepare 完成时 worker 可能还没把轨道事件交给播放器；此时用源（ms_ff）的轨道表，
     * 两边轨 id 相同，保证 onPrepared 里 getTrackInfo 就能拿到全部轨道（与系统一致）。 */
    if (n == 0 && pj->ff != NULL) {
        memset(ti, 0, sizeof(ti));
        if (ms_ff_list_tracks(pj->ff, ti, MP_TRACK_MAX, &n) != MS_OK) {
            n = 0;
        }
    }
#endif
    if (n > MP_TRACK_MAX) {
        n = MP_TRACK_MAX;
    }
    for (i = 0; i < n && i < cap; i++) {
        jint *m = meta + i * MP_TRACK_META;
        jstring js;
        m[0] = (jint)ti[i].id;
        m[1] = mp_track_type(&ti[i]);
        m[2] = (jint)ti[i].kind;
        m[3] = ti[i].selected ? 1 : 0;
        m[4] = ti[i].is_default ? 1 : 0;
        m[5] = ti[i].is_forced ? 1 : 0;
        m[6] = (jint)ti[i].codec;
        /* 字符串指针归会话所有，必须在持锁期间拷贝成 Java 对象。 */
        js = mp_safe_jstring(env, ti[i].language);
        (*env)->SetObjectArrayElement(env, jstrs, (jsize)(i * MP_TRACK_STRS + 0), js);
        if (js) (*env)->DeleteLocalRef(env, js);
        js = (*env)->NewStringUTF(env, mp_codec_mime(ti[i].codec));
        (*env)->SetObjectArrayElement(env, jstrs, (jsize)(i * MP_TRACK_STRS + 1), js);
        if (js) (*env)->DeleteLocalRef(env, js);
        js = mp_safe_jstring(env, ti[i].name);
        (*env)->SetObjectArrayElement(env, jstrs, (jsize)(i * MP_TRACK_STRS + 2), js);
        if (js) (*env)->DeleteLocalRef(env, js);
    }
    pthread_mutex_unlock(&pj->op);
    if (i > 0) {
        (*env)->SetIntArrayRegion(env, jmeta, 0, (jsize)(i * MP_TRACK_META), meta);
    }
    return (jint)n;
}

/* 选/取消一条轨（id 为内核轨 id）。返回 ms_status；0 = 已生效或已交给 worker。
 * 调用线程最多等 200 ms 拿锁，拿不到就整条交给 worker，不阻塞 UI。 */
JNIEXPORT jint JNICALL
Java_com_mycjzb_media_player_MediaPlayer_nativeSelectTrack(JNIEnv *env, jclass cls, jlong h, jint id, jboolean on)
{
    (void)env;
    (void)cls;
    return (jint)pj_select_track(pj_from(h), (uint32_t)id, on ? 1 : 0, 200, NULL, NULL, 0);
}

/* 当前时刻的文字字幕：多条同时显示的 cue 用 '\n' 拼接，返回 UTF-8 字节（Java 侧 new String）。
 * 用字节数组而不是 NewStringUTF，避免字幕里的 emoji / 非法序列触发 CheckJNI abort。
 * meta：[0]start_ms [1]end_ms（-1 未知）[2]track_id [3]style_flags [4]pos_x [5]pos_y
 *       [6]play_res_x [7]play_res_y [8]alignment [9]文字 cue 数（-1 = 播放器忙，保持上次）
 * 无文字 cue 返回 NULL 且 meta[9]=0。 */
JNIEXPORT jbyteArray JNICALL
Java_com_mycjzb_media_player_MediaPlayer_nativeGetCue(JNIEnv *env, jclass cls, jlong h, jintArray jmeta)
{
    player_jni *pj = pj_from(h);
    ms_sub_cue cues[MS_JNI_CUE_MAX];
    char text[MS_SUB_CUE_TEXT_MAX * 2 + 8];
    jint meta[MP_CUE_META];
    size_t n, i, len = 0;
    int ntext = 0;
    jbyteArray out = NULL;
    (void)cls;
    memset(meta, 0, sizeof(meta));
    meta[0] = -1;
    meta[1] = -1;
    if (pj == NULL || pj->p == NULL || jmeta == NULL ||
        (*env)->GetArrayLength(env, jmeta) < MP_CUE_META) {
        return NULL;
    }
    /* 主线程轮询调用：最多等 10 ms，忙就告诉 Java 保持上次字幕。 */
    if (!lock_op_ms(pj, 10)) {
        meta[9] = -1;
        (*env)->SetIntArrayRegion(env, jmeta, 0, MP_CUE_META, meta);
        return NULL;
    }
    n = pj_cues_now_locked(pj, cues, MS_JNI_CUE_MAX);
    for (i = 0; i < n; i++) {
        const ms_sub_cue *c = &cues[i];
        size_t cpy;
        if (!pj_cue_is_text(c)) {
            continue;
        }
        if (ntext == 0) {
            meta[0] = c->has_start ? (jint)(c->start_90k / 90u) : -1;
            meta[1] = c->has_end ? (jint)(c->end_90k / 90u) : -1;
            meta[2] = (jint)c->track_id;
            meta[3] = (jint)c->style_flags;
            meta[4] = c->pos_x;
            meta[5] = c->pos_y;
            meta[6] = c->play_res_x;
            meta[7] = c->play_res_y;
            meta[8] = c->alignment;
        } else if (len + 1 < sizeof(text)) {
            text[len++] = '\n';
        }
        cpy = c->text_size;
        if (cpy > sizeof(text) - 1 - len) {
            cpy = sizeof(text) - 1 - len;
        }
        memcpy(text + len, c->text, cpy);
        len += cpy;
        ntext++;
    }
    pthread_mutex_unlock(&pj->op);
    meta[9] = ntext;
    (*env)->SetIntArrayRegion(env, jmeta, 0, MP_CUE_META, meta);
    if (ntext == 0) {
        return NULL;
    }
    out = (*env)->NewByteArray(env, (jsize)len);
    if (out != NULL && len > 0) {
        (*env)->SetByteArrayRegion(env, out, 0, (jsize)len, (const jbyte *)text);
    }
    return out;
}

/* 外挂字幕（addTimedTextSource）：data 为整个字幕文件字节，fmt 为 ms_sub_format；
 * data 为空 = 清除。成功后出现 id=MS_TRACK_ID_EXTERNAL_SUB 的字幕轨，默认不选中（与系统一致）。
 * 返回 ms_status。 */
JNIEXPORT jint JNICALL
Java_com_mycjzb_media_player_MediaPlayer_nativeAddTimedText(JNIEnv *env, jclass cls, jlong h, jbyteArray jdata, jint fmt)
{
    (void)cls;
    return (jint)pj_load_ext_sub(env, pj_from(h), jdata, (int)fmt, NULL, NULL);
}
