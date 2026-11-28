/*
 * Media3（androidx/media main@101e97c）音画同步移植的公共常量、结构与 API。
 * 逐函数对照见 docs/MEDIA3_SYNC_PORT.md。
 *
 * 原始版权：Copyright (C) The Android Open Source Project
 * 原始许可：Apache License 2.0
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * 移植注释与接线：by: Long <mycjzb@gmail.com>
 */
#ifndef MSMEDIA_MS_MEDIA3_H
#define MSMEDIA_MS_MEDIA3_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MS_C_TIME_UNSET (INT64_MIN + 1)
#define MS_C_TIME_END_OF_SOURCE (INT64_MIN)
#define MS_C_INDEX_UNSET (-1)
#define MS_C_MICROS_PER_SECOND 1000000ll
#define MS_C_NANOS_PER_SECOND 1000000000ll
#define MS_C_MILLIS_PER_SECOND 1000ll

/* androidx.media3.common.C.FirstFrameState */
#define MS_C_FIRST_FRAME_NOT_RENDERED_ONLY_ALLOWED_IF_STARTED 0
#define MS_C_FIRST_FRAME_NOT_RENDERED 1
#define MS_C_FIRST_FRAME_NOT_RENDERED_AFTER_STREAM_CHANGE 2
#define MS_C_FIRST_FRAME_RENDERED 3

/* VideoFrameReleaseControl.FrameReleaseAction */
#define FRAME_RELEASE_IMMEDIATELY 0
#define FRAME_RELEASE_SCHEDULED 1
#define FRAME_RELEASE_DROP 2
#define FRAME_RELEASE_SKIP 3
#define FRAME_RELEASE_IGNORE 4
#define FRAME_RELEASE_TRY_AGAIN_LATER 5

/* MediaCodecVideoRenderer */
#define MIN_EARLY_US_LATE_THRESHOLD (-30000ll)
#define MIN_EARLY_US_VERY_LATE_THRESHOLD (-500000ll)
#define DEFAULT_EARLY_SCHEDULING_THRESHOLD_US 50000ll
#define DEFAULT_LATE_THRESHOLD_TO_DROP_DECODER_INPUT_US 15000ll

/* VideoFrameReleaseHelper */
#define VSYNC_SAMPLE_UPDATE_PERIOD_MS 500ll
#define MAX_ALLOWED_ADJUSTMENT_NS 20000000ll
#define VSYNC_OFFSET_PERCENTAGE 80ll

/* AudioTimestampPoller */
#define STATE_INITIALIZING 0
#define STATE_TIMESTAMP 1
#define STATE_TIMESTAMP_ADVANCING 2
#define STATE_NO_TIMESTAMP 3
#define STATE_ERROR 4
#define FAST_POLL_INTERVAL_US 10000
#define SLOW_POLL_INTERVAL_US 10000000
#define ERROR_POLL_INTERVAL_US 500000
#define INITIALIZING_DURATION_US 500000
#define MAX_POSITION_DRIFT_ADVANCING_TIMESTAMP_US 1000ll
#define WAIT_FOR_ADVANCE_DURATION_US 2000000
#define MAX_AUDIO_TIMESTAMP_OFFSET_US (5 * MS_C_MICROS_PER_SECOND)

/* AudioTrackPositionTracker */
#define MAX_LATENCY_US (10 * MS_C_MICROS_PER_SECOND)
#define MAX_POSITION_DRIFT_FOR_SMOOTHING_US MS_C_MICROS_PER_SECOND
#define MAX_POSITION_SMOOTHING_SPEED_CHANGE_PERCENT 10
#define RAW_PLAYBACK_HEAD_POSITION_UPDATE_INTERVAL_MS 5
#define FORCE_RESET_WORKAROUND_TIMEOUT_MS 200
#define MIN_RAW_PLAYBACK_HEAD_POSITION_WRAP_DISTANCE (1ll << 31)
#define MAX_PLAYHEAD_OFFSET_COUNT 10
#define MIN_PLAYHEAD_OFFSET_SAMPLE_INTERVAL_US 30000
#define MIN_LATENCY_SAMPLE_INTERVAL_US 500000 /* 源码写作 50_0000 */

/* FixedFrameRateEstimator */
#define CONSECUTIVE_MATCHING_FRAME_DURATIONS_FOR_SYNC 15
#define MAX_MATCHING_FRAME_DIFFERENCE_NS 1000000ll
#define MINIMUM_MATCHING_FRAME_DURATION_FOR_HIGH_CONFIDENCE_NS 5000000000ll
#define MINIMUM_MEDIA_FRAME_RATE_CHANGE_FOR_UPDATE_HIGH_CONFIDENCE 0.1f
#define MINIMUM_MEDIA_FRAME_RATE_CHANGE_FOR_UPDATE_LOW_CONFIDENCE 1.0f
#define FORMAT_NO_VALUE (-1.0f)

#define PLAYSTATE_STOPPED 1
#define PLAYSTATE_PAUSED 2
#define PLAYSTATE_PLAYING 3

/* ---------- 可注入时钟（生产=CLOCK_MONOTONIC；单测可替换） ---------- */
typedef struct ms_media3_clock_fns {
    int64_t (*nanoTime)(void);
    int64_t (*elapsedRealtimeMs)(void);
} ms_media3_clock_fns;

void ms_media3_set_test_clock(const ms_media3_clock_fns *fns); /* NULL=恢复真实钟 */
int64_t ms_media3_nanoTime(void);
int64_t ms_media3_elapsedRealtimeMs(void);
int64_t ms_media3_elapsedRealtimeUs(void);

int64_t ms_media3_getMediaDurationForPlayoutDuration(int64_t playoutDurationUs, float speed);
int64_t ms_media3_getPlayoutDurationForMediaDuration(int64_t mediaDurationUs, float speed);
int64_t ms_media3_sampleCountToDurationUs(int64_t sampleCount, int sampleRate);
int64_t ms_media3_durationUsToSampleCount(int64_t durationUs, int sampleRate);
int64_t ms_media3_msToUs(int64_t ms);
int64_t ms_media3_usToMs(int64_t us);
int64_t ms_media3_constrainValue(int64_t v, int64_t lo, int64_t hi);

/* AudioTrack 访问（Android 接真实 Track；宿主单测注入假 Track）。
 * 对应 Media3 直接调用的 AudioTrack 方法，不是算法分叉。 */
typedef struct AudioTrackOps {
    void *ctx;
    int (*getPlayState)(void *ctx);
    uint32_t (*getPlaybackHeadPosition)(void *ctx); /* 原始 32 位 */
    int (*getSampleRate)(void *ctx);
    /* 1=成功并填写 framePosition/nanoTime；0=getTimestamp 返回 false */
    int (*getTimestamp)(void *ctx, int64_t *framePosition, int64_t *nanoTime);
    /* 0=成功（毫秒）；-1=方法不存在或失败（对应 Media3 反射失败后不再尝试） */
    int (*getLatencyMs)(void *ctx, int *latency_ms);
} AudioTrackOps;

/* ---------- AudioTimestampWrapper / AudioTimestampPoller ---------- */
typedef struct AudioTimestampWrapper {
    AudioTrackOps track;
    int64_t rawTimestampFramePositionWrapCount;
    int64_t lastTimestampRawPositionFrames;
    int64_t lastTimestampPositionFrames;
    int expectTimestampFramePositionReset;
    int64_t accumulatedRawTimestampFramePosition;
    int64_t timestampSystemTimeUs;
    int hasTimestamp;
} AudioTimestampWrapper;

typedef struct AudioTimestampPoller {
    AudioTimestampWrapper audioTimestamp;
    int sampleRate;
    int state;
    int64_t initializeSystemTimeUs;
    int64_t sampleIntervalUs;
    int64_t lastTimestampSampleTimeUs;
    int64_t initialTimestampPositionFrames;
    int64_t initialTimestampSystemTimeUs;
} AudioTimestampPoller;

void AudioTimestampPoller_init(AudioTimestampPoller *p, AudioTrackOps track, int sampleRate);
void AudioTimestampPoller_maybePollTimestamp(AudioTimestampPoller *p, int64_t systemTimeUs,
                                             float audioTrackPlaybackSpeed,
                                             int64_t playbackHeadPositionEstimateUs,
                                             int forceUpdate);
int AudioTimestampPoller_hasAdvancingTimestamp(const AudioTimestampPoller *p);
int AudioTimestampPoller_isWaitingForAdvancingTimestamp(const AudioTimestampPoller *p);
void AudioTimestampPoller_reset(AudioTimestampPoller *p);
int64_t AudioTimestampPoller_getTimestampPositionUs(AudioTimestampPoller *p, int64_t systemTimeUs,
                                                    float audioTrackPlaybackSpeed);
void AudioTimestampPoller_expectTimestampFramePositionReset(AudioTimestampPoller *p);

/* ---------- AudioTrackPositionTracker ---------- */
typedef struct AudioTrackPositionTracker {
    AudioTrackOps audioTrack;
    int outputSampleRate;
    int isOutputPcm;
    int64_t bufferSizeUs;
    long long playheadOffsets[MAX_PLAYHEAD_OFFSET_COUNT];
    int nextPlayheadOffsetIndex;
    int playheadOffsetCount;
    int64_t smoothedPlayheadOffsetUs;
    int64_t lastPlayheadSampleTimeUs;
    int64_t lastLatencySampleTimeUs;
    int64_t latencyUs;
    int getLatencyMethodDead;
    AudioTimestampPoller audioTimestampPoller;
    int64_t rawPlaybackHeadPosition;
    int64_t rawPlaybackHeadWrapCount;
    int64_t sumRawPlaybackHeadPosition;
    int expectRawPlaybackHeadReset;
    int64_t lastRawPlaybackHeadPositionSampleTimeMs;
    int64_t stopTimestampUs;
    int64_t stopPlaybackHeadPosition;
    int64_t endPlaybackHeadPosition;
    int64_t forceResetWorkaroundTimeMs;
    int64_t lastSystemTimeUs;
    int64_t lastPositionUs;
    float audioTrackPlaybackSpeed;
    int64_t onPositionAdvancingFromPositionUs;
    int hasBeenStarted;
} AudioTrackPositionTracker;

void AudioTrackPositionTracker_init(AudioTrackPositionTracker *t, AudioTrackOps track,
                                    int outputEncodingIsPcm, int outputPcmFrameSize,
                                    int bufferSize);
void AudioTrackPositionTracker_setAudioTrackPlaybackSpeed(AudioTrackPositionTracker *t, float speed);
int64_t AudioTrackPositionTracker_getCurrentPositionUs(AudioTrackPositionTracker *t,
                                                       int64_t writtenFrames);
void AudioTrackPositionTracker_start(AudioTrackPositionTracker *t);
int AudioTrackPositionTracker_isPlaying(AudioTrackPositionTracker *t);
int AudioTrackPositionTracker_isStalled(AudioTrackPositionTracker *t, int64_t writtenFrames);
void AudioTrackPositionTracker_handleEndOfStream(AudioTrackPositionTracker *t, int64_t writtenFrames);
void AudioTrackPositionTracker_pause(AudioTrackPositionTracker *t);
void AudioTrackPositionTracker_expectRawPlaybackHeadReset(AudioTrackPositionTracker *t);
void AudioTrackPositionTracker_reset(AudioTrackPositionTracker *t);

/* ---------- DefaultAudioSink 位置换算（无 AudioProcessorChain） ---------- */
#define MEDIA_POSITION_CHECKPOINT_CAP 8

typedef struct MediaPositionParameters {
    float speed;
    int64_t mediaTimeUs;
    int64_t audioOutputPositionUs;
    int64_t mediaPositionDriftUs;
    int64_t appliedSkippedSilenceDurationUs;
} MediaPositionParameters;

typedef struct DefaultAudioSink {
    int startMediaTimeUsNeedsInit;
    int startMediaTimeUsNeedsSync;
    int64_t startMediaTimeUs;
    int initialized;
    int64_t writtenFrames;
    int outputSampleRate;
    MediaPositionParameters mediaPositionParameters;
    MediaPositionParameters mediaPositionParametersCheckpoints[MEDIA_POSITION_CHECKPOINT_CAP];
    int checkpointCount;
    AudioTrackPositionTracker tracker;
    int tracker_inited;
} DefaultAudioSink;

void DefaultAudioSink_init(DefaultAudioSink *s);
void DefaultAudioSink_configure(DefaultAudioSink *s, AudioTrackOps track, int sampleRate,
                                int isPcm, int pcmFrameSize, int bufferSize);
void DefaultAudioSink_play(DefaultAudioSink *s);
void DefaultAudioSink_pause(DefaultAudioSink *s);
void DefaultAudioSink_flush(DefaultAudioSink *s);
void DefaultAudioSink_handleEndOfStream(DefaultAudioSink *s);
void DefaultAudioSink_addWrittenFrames(DefaultAudioSink *s, int64_t frames);
/* 对应 handleBuffer 里对 startMediaTimeUs 的初始化与 200ms 不连续校正 */
void DefaultAudioSink_handleBufferTimestamps(DefaultAudioSink *s, int64_t presentationTimeUs);
void DefaultAudioSink_setPlaybackSpeed(DefaultAudioSink *s, float speed);
int64_t DefaultAudioSink_applyMediaPositionParameters(DefaultAudioSink *s, int64_t positionUs);
int64_t DefaultAudioSink_applySkipping(DefaultAudioSink *s, int64_t mediaPositionUs,
                                       int64_t audioOutputPositionUs);
int64_t DefaultAudioSink_getCurrentPositionUs(DefaultAudioSink *s, int sourceEnded);

/* ---------- StandaloneMediaClock / DefaultMediaClock ---------- */
typedef struct StandaloneMediaClock {
    int started;
    int64_t baseUs;
    int64_t baseElapsedMs;
    float speed;
} StandaloneMediaClock;

void StandaloneMediaClock_init(StandaloneMediaClock *c);
void StandaloneMediaClock_start(StandaloneMediaClock *c);
void StandaloneMediaClock_stop(StandaloneMediaClock *c);
void StandaloneMediaClock_resetPosition(StandaloneMediaClock *c, int64_t positionUs);
int64_t StandaloneMediaClock_getPositionUs(StandaloneMediaClock *c);
void StandaloneMediaClock_setPlaybackParameters(StandaloneMediaClock *c, float speed);

typedef struct DefaultMediaClock {
    StandaloneMediaClock standaloneClock;
    int standaloneClockIsStarted;
    int isUsingStandaloneClock;
    int hasRendererClock;
    DefaultAudioSink *audioSink; /* NULL=无音频 renderer 钟 */
} DefaultMediaClock;

void DefaultMediaClock_init(DefaultMediaClock *c);
void DefaultMediaClock_start(DefaultMediaClock *c);
void DefaultMediaClock_stop(DefaultMediaClock *c);
void DefaultMediaClock_resetPosition(DefaultMediaClock *c, int64_t positionUs);
void DefaultMediaClock_setAudioSink(DefaultMediaClock *c, DefaultAudioSink *sink);
void DefaultMediaClock_setPlaybackParameters(DefaultMediaClock *c, float speed);
int64_t DefaultMediaClock_syncAndGetPositionUs(DefaultMediaClock *c, int isReadingAhead);
int DefaultMediaClock_isUsingStandaloneClock(const DefaultMediaClock *c);

/* ---------- FixedFrameRateEstimator ---------- */
typedef struct FixedFrameRateMatcher {
    int64_t firstFramePresentationTimeNs;
    int64_t firstFrameDurationNs;
    int64_t lastFramePresentationTimeNs;
    int64_t frameCount;
    int64_t matchingFrameCount;
    int64_t matchingFrameDurationSumNs;
    int recentFrameOutlierFlags[CONSECUTIVE_MATCHING_FRAME_DURATIONS_FOR_SYNC];
    int recentFrameOutlierCount;
} FixedFrameRateMatcher;

typedef struct FixedFrameRateEstimator {
    FixedFrameRateMatcher currentMatcher;
    FixedFrameRateMatcher candidateMatcher;
    int candidateMatcherActive;
    int64_t lastFramePresentationTimeNs;
    int framesWithoutSyncCount;
    float formatFrameRate;
    float frameRateEstimate;
    int64_t frameIndex;
} FixedFrameRateEstimator;

void FixedFrameRateEstimator_init(FixedFrameRateEstimator *e);
void FixedFrameRateEstimator_onFormatChanged(FixedFrameRateEstimator *e, float formatFrameRate);
void FixedFrameRateEstimator_onNextFrame(FixedFrameRateEstimator *e, int64_t framePresentationTimeNs);
int64_t FixedFrameRateEstimator_getFrameDurationNs(const FixedFrameRateEstimator *e);
int64_t FixedFrameRateEstimator_getFrameIndex(const FixedFrameRateEstimator *e);
float FixedFrameRateEstimator_getFrameRateEstimate(const FixedFrameRateEstimator *e);

/* ---------- VideoFrameReleaseHelper（adjustReleaseTime + 滞回；VSYNC 采样在 Java） ---------- */
typedef struct VideoFrameReleaseHelper {
    int64_t lastAdjustedFrameIndex;
    int64_t lastAdjustedReleaseTimeNs;
    int64_t lastAdjustedPresentationTimeUs;
    int64_t lastVsyncHysteresisOffsetNs;
    int64_t pendingLastAdjustedFrameIndex;
    int64_t pendingLastAdjustedReleaseTimeNs;
    int64_t pendingLastPresentationTimeUs;
    int64_t pendingVsyncHysteresisOffsetNs;
    int64_t sampledVsyncTimeNs;
    int64_t vsyncDurationNs;
    float playbackSpeed;
} VideoFrameReleaseHelper;

void VideoFrameReleaseHelper_init(VideoFrameReleaseHelper *h);
void VideoFrameReleaseHelper_onPlaybackSpeed(VideoFrameReleaseHelper *h, float speed);
void VideoFrameReleaseHelper_onPositionReset(VideoFrameReleaseHelper *h);
void VideoFrameReleaseHelper_setVsyncData(VideoFrameReleaseHelper *h, int64_t vsyncSampleTimeNs,
                                          int64_t vsyncDurationNs);
int64_t VideoFrameReleaseHelper_adjustReleaseTime(VideoFrameReleaseHelper *h, int64_t releaseTimeNs,
                                                  int64_t presentationTimeUs, int64_t frameDurationNs,
                                                  int64_t frameIndex);
int64_t VideoFrameReleaseHelper_getVsyncDurationNs(const VideoFrameReleaseHelper *h);

/* ---------- VideoFrameReleaseControl ---------- */
typedef struct FrameReleaseInfo {
    int64_t earlyUs;
    int64_t releaseTimeNs;
} FrameReleaseInfo;

typedef struct VideoFrameReleaseControl {
    int64_t earlySchedulingThresholdUs;
    int started;
    int firstFrameState;
    int64_t initialPositionUs;
    int64_t lastReleaseRealtimeUs;
    int64_t joiningDeadlineMs;
    int joiningRenderNextFrameImmediately;
    float playbackSpeed;
    int hasOutputSurface;
    int hasOutputBufferRenderer;
    int frameReadyWithoutSurface;
    int disableAdvancingTimestampChecks;
    int requiresOutputSurface;
    int skipBuffersWithIdenticalReleaseTime;
    int64_t allowedJoiningTimeMs;
    int64_t lastFrameReleaseTimeNs;
    int64_t prevFrameReleaseTimeNs;
    int64_t lastFramePresentationTimeUs;
    int64_t prevFramePresentationTimeUs;
    VideoFrameReleaseHelper frameReleaseHelper;
    FixedFrameRateEstimator frameRateEstimator;
} VideoFrameReleaseControl;

void VideoFrameReleaseControl_init(VideoFrameReleaseControl *c);
void VideoFrameReleaseControl_onStarted(VideoFrameReleaseControl *c);
void VideoFrameReleaseControl_onStopped(VideoFrameReleaseControl *c);
void VideoFrameReleaseControl_setOutputSurface(VideoFrameReleaseControl *c, int hasSurface);
void VideoFrameReleaseControl_allowReleaseFirstFrameBeforeStarted(VideoFrameReleaseControl *c);
void VideoFrameReleaseControl_onStreamChanged(VideoFrameReleaseControl *c, int releaseFirstFrameMode);
void VideoFrameReleaseControl_reset(VideoFrameReleaseControl *c);
void VideoFrameReleaseControl_setPlaybackSpeed(VideoFrameReleaseControl *c, float speed);
void VideoFrameReleaseControl_setVsyncData(VideoFrameReleaseControl *c, int64_t t, int64_t d);
int VideoFrameReleaseControl_onFrameReleasedIsFirstFrame(VideoFrameReleaseControl *c);
/* maybeDropBuffersToKeyframe：对应 MediaCodecVideoRenderer.maybeDropBuffersToKeyframe。
 * 返回 1 表示 skipSource 跳过了数据并已 flush；0 表示没有可跳的源，不得 flush。
 * shouldIgnoreFrame = shouldDropBuffersToKeyframe && maybeDrop。 */
typedef int (*maybeDropBuffersToKeyframe_fn)(void *ctx, int64_t positionUs,
                                             int treatDroppedBuffersAsSkipped);
int VideoFrameReleaseControl_getFrameReleaseAction(
    VideoFrameReleaseControl *c,
    int64_t presentationTimeUs, int64_t positionUs, int64_t elapsedRealtimeUs,
    int64_t outputStreamStartPositionUs, int isDecodeOnlyFrame, int isLastFrame,
    int64_t frameDurationNs, int64_t frameIndex, FrameReleaseInfo *frameReleaseInfo,
    maybeDropBuffersToKeyframe_fn maybeDrop, void *maybeDropCtx);

/* MediaCodecVideoRenderer.FrameTimingEvaluator */
int shouldDropOutputBuffer(int64_t earlyUs, int64_t elapsedRealtimeUs, int isLastBuffer);
int shouldDropBuffersToKeyframe(int64_t earlyUs, int64_t elapsedRealtimeUs, int isLastBuffer);
int shouldForceRenderOutputBuffer(int64_t earlyUs, int64_t elapsedSinceLastRenderUs);

#ifdef __cplusplus
}
#endif
#endif
