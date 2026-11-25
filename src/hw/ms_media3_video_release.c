/*
 * 移植 androidx.media3.exoplayer.video.FixedFrameRateEstimator /
 * VideoFrameReleaseHelper（adjustReleaseTime、findClosestVsyncAndUpdateHysteresis）/
 * VideoFrameReleaseControl.getFrameReleaseActionInternal 全分支，以及
 * MediaCodecVideoRenderer 的 shouldDropOutputBuffer / shouldDropBuffersToKeyframe /
 * shouldForceRenderOutputBuffer。
 * 对照 androidx/media main@101e97c，见 docs/MEDIA3_SYNC_PORT.md。
 *
 * 原始版权：Copyright (C) The Android Open Source Project
 * 原始许可：Apache License 2.0
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * 移植注释与接线：by: Long <mycjzb@gmail.com>
 */
#define _DEFAULT_SOURCE
#include "ms_media3.h"

#include <stdlib.h>
#include <string.h>

static int64_t i64abs(int64_t v)
{
    return v < 0 ? -v : v;
}

/* ===================== FixedFrameRateEstimator.Matcher ===================== */

static int Matcher_getRecentFrameOutlierIndex(int64_t frameCount)
{
    return (int)(frameCount % CONSECUTIVE_MATCHING_FRAME_DURATIONS_FOR_SYNC);
}

static void Matcher_reset(FixedFrameRateMatcher *m)
{
    memset(m, 0, sizeof(*m));
}

static int Matcher_isSynced(const FixedFrameRateMatcher *m)
{
    return m->frameCount > CONSECUTIVE_MATCHING_FRAME_DURATIONS_FOR_SYNC &&
           m->recentFrameOutlierCount == 0;
}

static int Matcher_isLastFrameOutlier(const FixedFrameRateMatcher *m)
{
    if (m->frameCount == 0) {
        return 0;
    }
    return m->recentFrameOutlierFlags[Matcher_getRecentFrameOutlierIndex(m->frameCount - 1)];
}

static int64_t Matcher_getFrameDurationNs(const FixedFrameRateMatcher *m)
{
    return m->matchingFrameCount == 0 ? 0
                                      : (m->matchingFrameDurationSumNs / m->matchingFrameCount);
}

static void Matcher_onNextFrame(FixedFrameRateMatcher *m, int64_t framePresentationTimeNs)
{
    if (m->frameCount == 0) {
        m->firstFramePresentationTimeNs = framePresentationTimeNs;
    } else if (m->frameCount == 1) {
        m->firstFrameDurationNs = framePresentationTimeNs - m->firstFramePresentationTimeNs;
        m->matchingFrameDurationSumNs = m->firstFrameDurationNs;
        m->matchingFrameCount = 1;
    } else {
        int64_t lastFrameDurationNs = framePresentationTimeNs - m->lastFramePresentationTimeNs;
        int recentFrameOutlierIndex = Matcher_getRecentFrameOutlierIndex(m->frameCount);
        if (i64abs(lastFrameDurationNs - m->firstFrameDurationNs) <= MAX_MATCHING_FRAME_DIFFERENCE_NS) {
            m->matchingFrameCount++;
            m->matchingFrameDurationSumNs += lastFrameDurationNs;
            if (m->recentFrameOutlierFlags[recentFrameOutlierIndex]) {
                m->recentFrameOutlierFlags[recentFrameOutlierIndex] = 0;
                m->recentFrameOutlierCount--;
            }
        } else if (!m->recentFrameOutlierFlags[recentFrameOutlierIndex]) {
            m->recentFrameOutlierFlags[recentFrameOutlierIndex] = 1;
            m->recentFrameOutlierCount++;
        }
    }
    m->frameCount++;
    m->lastFramePresentationTimeNs = framePresentationTimeNs;
}

void FixedFrameRateEstimator_init(FixedFrameRateEstimator *e)
{
    memset(e, 0, sizeof(*e));
    e->lastFramePresentationTimeNs = MS_C_TIME_UNSET;
    e->formatFrameRate = FORMAT_NO_VALUE;
    e->frameRateEstimate = FORMAT_NO_VALUE;
}

static void FixedFrameRateEstimator_updateReportedFrameRate(FixedFrameRateEstimator *e)
{
    int synced = Matcher_isSynced(&e->currentMatcher);
    float candidateFrameRate = synced ? (float)((double)MS_C_NANOS_PER_SECOND /
                                                (double)Matcher_getFrameDurationNs(&e->currentMatcher))
                                      : e->formatFrameRate;
    int shouldUpdate;
    if (candidateFrameRate == e->frameRateEstimate) {
        return;
    }
    if (candidateFrameRate != FORMAT_NO_VALUE && e->frameRateEstimate != FORMAT_NO_VALUE) {
        int candidateIsHighConfidence =
            synced && e->currentMatcher.matchingFrameDurationSumNs >=
                          MINIMUM_MATCHING_FRAME_DURATION_FOR_HIGH_CONFIDENCE_NS;
        float minimumChangeForUpdate =
            candidateIsHighConfidence ? MINIMUM_MEDIA_FRAME_RATE_CHANGE_FOR_UPDATE_HIGH_CONFIDENCE
                                      : MINIMUM_MEDIA_FRAME_RATE_CHANGE_FOR_UPDATE_LOW_CONFIDENCE;
        shouldUpdate = (candidateFrameRate > e->frameRateEstimate
                            ? candidateFrameRate - e->frameRateEstimate
                            : e->frameRateEstimate - candidateFrameRate) >= minimumChangeForUpdate;
    } else if (candidateFrameRate != FORMAT_NO_VALUE) {
        shouldUpdate = 1;
    } else {
        shouldUpdate = e->framesWithoutSyncCount >=
                       (2 * CONSECUTIVE_MATCHING_FRAME_DURATIONS_FOR_SYNC);
    }
    if (shouldUpdate) {
        e->frameRateEstimate = candidateFrameRate;
    }
}

void FixedFrameRateEstimator_onFormatChanged(FixedFrameRateEstimator *e, float formatFrameRate)
{
    e->formatFrameRate = formatFrameRate;
    Matcher_reset(&e->currentMatcher);
    Matcher_reset(&e->candidateMatcher);
    e->candidateMatcherActive = 0;
    e->lastFramePresentationTimeNs = MS_C_TIME_UNSET;
    e->framesWithoutSyncCount = 0;
    FixedFrameRateEstimator_updateReportedFrameRate(e);
}

void FixedFrameRateEstimator_onNextFrame(FixedFrameRateEstimator *e, int64_t framePresentationTimeNs)
{
    if (framePresentationTimeNs == e->lastFramePresentationTimeNs) {
        return;
    }
    e->frameIndex++;
    Matcher_onNextFrame(&e->currentMatcher, framePresentationTimeNs);
    if (Matcher_isSynced(&e->currentMatcher)) {
        e->candidateMatcherActive = 0;
    } else if (e->lastFramePresentationTimeNs != MS_C_TIME_UNSET) {
        if (!e->candidateMatcherActive || Matcher_isLastFrameOutlier(&e->candidateMatcher)) {
            Matcher_reset(&e->candidateMatcher);
            Matcher_onNextFrame(&e->candidateMatcher, e->lastFramePresentationTimeNs);
        }
        e->candidateMatcherActive = 1;
        Matcher_onNextFrame(&e->candidateMatcher, framePresentationTimeNs);
    }
    if (e->candidateMatcherActive && Matcher_isSynced(&e->candidateMatcher)) {
        FixedFrameRateMatcher tmp = e->currentMatcher;
        e->currentMatcher = e->candidateMatcher;
        e->candidateMatcher = tmp;
        e->candidateMatcherActive = 0;
    }
    e->lastFramePresentationTimeNs = framePresentationTimeNs;
    e->framesWithoutSyncCount =
        Matcher_isSynced(&e->currentMatcher) ? 0 : e->framesWithoutSyncCount + 1;
    FixedFrameRateEstimator_updateReportedFrameRate(e);
}

int64_t FixedFrameRateEstimator_getFrameDurationNs(const FixedFrameRateEstimator *e)
{
    return Matcher_isSynced(&e->currentMatcher) ? Matcher_getFrameDurationNs(&e->currentMatcher)
                                                : MS_C_TIME_UNSET;
}

int64_t FixedFrameRateEstimator_getFrameIndex(const FixedFrameRateEstimator *e)
{
    return e->frameIndex;
}

float FixedFrameRateEstimator_getFrameRateEstimate(const FixedFrameRateEstimator *e)
{
    return e->frameRateEstimate;
}

/* ===================== VideoFrameReleaseHelper ===================== */

static void VideoFrameReleaseHelper_resetAdjustment(VideoFrameReleaseHelper *h)
{
    h->lastAdjustedFrameIndex = MS_C_INDEX_UNSET;
    h->pendingLastAdjustedFrameIndex = MS_C_INDEX_UNSET;
}

void VideoFrameReleaseHelper_init(VideoFrameReleaseHelper *h)
{
    memset(h, 0, sizeof(*h));
    h->lastAdjustedFrameIndex = MS_C_INDEX_UNSET;
    h->pendingLastAdjustedFrameIndex = MS_C_INDEX_UNSET;
    h->sampledVsyncTimeNs = MS_C_TIME_UNSET;
    h->vsyncDurationNs = MS_C_TIME_UNSET;
    h->playbackSpeed = 1.0f;
}

void VideoFrameReleaseHelper_onPlaybackSpeed(VideoFrameReleaseHelper *h, float speed)
{
    h->playbackSpeed = speed > 0.0f ? speed : 1.0f;
    VideoFrameReleaseHelper_resetAdjustment(h);
}

void VideoFrameReleaseHelper_onPositionReset(VideoFrameReleaseHelper *h)
{
    VideoFrameReleaseHelper_resetAdjustment(h);
}

void VideoFrameReleaseHelper_setVsyncData(VideoFrameReleaseHelper *h, int64_t vsyncSampleTimeNs,
                                          int64_t vsyncDurationNs)
{
    h->sampledVsyncTimeNs = vsyncSampleTimeNs;
    h->vsyncDurationNs = vsyncDurationNs;
}

int64_t VideoFrameReleaseHelper_getVsyncDurationNs(const VideoFrameReleaseHelper *h)
{
    return h->vsyncDurationNs;
}

static int VideoFrameReleaseHelper_adjustmentAllowed(int64_t unadjustedReleaseTimeNs,
                                                     int64_t adjustedReleaseTimeNs)
{
    return i64abs(unadjustedReleaseTimeNs - adjustedReleaseTimeNs) <= MAX_ALLOWED_ADJUSTMENT_NS;
}

/* VideoFrameReleaseHelper.findClosestVsyncAndUpdateHysteresis（含 60fps@90Hz 滞回） */
static int64_t findClosestVsyncAndUpdateHysteresis(VideoFrameReleaseHelper *h, int64_t releaseTimeNs,
                                                   int64_t sampledVsyncTimeNs, int64_t vsyncDurationNs)
{
    int64_t vsyncCount = (releaseTimeNs - sampledVsyncTimeNs) / vsyncDurationNs;
    int64_t snappedTimeNs = sampledVsyncTimeNs + (vsyncDurationNs * vsyncCount);
    int64_t snappedBeforeNs, snappedAfterNs;
    int64_t snappedAfterDiffNs, snappedBeforeDiffNs, snappedDiffsDiffNs;
    int shouldEvaluateHysteresis;
    if (releaseTimeNs <= snappedTimeNs) {
        snappedBeforeNs = snappedTimeNs - vsyncDurationNs;
        snappedAfterNs = snappedTimeNs;
    } else {
        snappedBeforeNs = snappedTimeNs;
        snappedAfterNs = snappedTimeNs + vsyncDurationNs;
    }
    snappedAfterDiffNs = snappedAfterNs - releaseTimeNs;
    snappedBeforeDiffNs = releaseTimeNs - snappedBeforeNs;
    snappedDiffsDiffNs = i64abs(snappedAfterDiffNs - snappedBeforeDiffNs);
    shouldEvaluateHysteresis = snappedDiffsDiffNs < vsyncDurationNs / 2;
    if (shouldEvaluateHysteresis) {
        int64_t hysteresisRangeNs = vsyncDurationNs / 4;
        int isInHysteresisRange = snappedDiffsDiffNs < hysteresisRangeNs;
        if (isInHysteresisRange) {
            if (h->lastVsyncHysteresisOffsetNs != 0) {
                h->pendingVsyncHysteresisOffsetNs = h->lastVsyncHysteresisOffsetNs;
            } else {
                h->pendingVsyncHysteresisOffsetNs =
                    snappedAfterDiffNs < snappedBeforeDiffNs ? -hysteresisRangeNs : hysteresisRangeNs;
            }
        } else {
            h->pendingVsyncHysteresisOffsetNs = 0;
        }
    } else {
        h->pendingVsyncHysteresisOffsetNs = h->lastVsyncHysteresisOffsetNs;
    }
    return snappedAfterDiffNs + h->pendingVsyncHysteresisOffsetNs < snappedBeforeDiffNs
               ? snappedAfterNs
               : snappedBeforeNs;
}

/* VideoFrameReleaseHelper.adjustReleaseTime */
int64_t VideoFrameReleaseHelper_adjustReleaseTime(VideoFrameReleaseHelper *h, int64_t releaseTimeNs,
                                                  int64_t presentationTimeUs, int64_t frameDurationNs,
                                                  int64_t frameIndex)
{
    int64_t adjustedReleaseTimeNs = releaseTimeNs;
    if (presentationTimeUs != h->pendingLastPresentationTimeUs) {
        h->lastAdjustedFrameIndex = h->pendingLastAdjustedFrameIndex;
        h->lastAdjustedReleaseTimeNs = h->pendingLastAdjustedReleaseTimeNs;
        h->lastAdjustedPresentationTimeUs = h->pendingLastPresentationTimeUs;
        h->lastVsyncHysteresisOffsetNs = h->pendingVsyncHysteresisOffsetNs;
    }
    if (h->lastAdjustedFrameIndex != MS_C_INDEX_UNSET) {
        int64_t elapsedReleaseTimeSinceLastFrameNs;
        int64_t candidateAdjustedReleaseTimeNs;
        if (frameDurationNs != MS_C_TIME_UNSET) {
            elapsedReleaseTimeSinceLastFrameNs = (int64_t)((double)(frameDurationNs *
                                                                    (frameIndex - h->lastAdjustedFrameIndex)) /
                                                           (double)h->playbackSpeed);
        } else {
            elapsedReleaseTimeSinceLastFrameNs = (int64_t)((double)(presentationTimeUs -
                                                                    h->lastAdjustedPresentationTimeUs) *
                                                           1000.0 / (double)h->playbackSpeed);
        }
        candidateAdjustedReleaseTimeNs =
            h->lastAdjustedReleaseTimeNs + elapsedReleaseTimeSinceLastFrameNs;
        if (VideoFrameReleaseHelper_adjustmentAllowed(releaseTimeNs, candidateAdjustedReleaseTimeNs)) {
            adjustedReleaseTimeNs = candidateAdjustedReleaseTimeNs;
        } else {
            VideoFrameReleaseHelper_resetAdjustment(h);
        }
    }
    h->pendingLastAdjustedFrameIndex = frameIndex;
    h->pendingLastAdjustedReleaseTimeNs = adjustedReleaseTimeNs;
    h->pendingLastPresentationTimeUs = presentationTimeUs;

    if (h->sampledVsyncTimeNs == MS_C_TIME_UNSET || h->vsyncDurationNs == MS_C_TIME_UNSET ||
        h->vsyncDurationNs <= 0) {
        return adjustedReleaseTimeNs;
    }
    {
        int64_t snappedTimeNs = findClosestVsyncAndUpdateHysteresis(
            h, adjustedReleaseTimeNs, h->sampledVsyncTimeNs, h->vsyncDurationNs);
        return snappedTimeNs - (h->vsyncDurationNs * VSYNC_OFFSET_PERCENTAGE) / 100;
    }
}

/* ===================== MediaCodecVideoRenderer FrameTimingEvaluator ===================== */

int shouldDropOutputBuffer(int64_t earlyUs, int64_t elapsedRealtimeUs, int isLastBuffer)
{
    (void)elapsedRealtimeUs;
    return earlyUs < MIN_EARLY_US_LATE_THRESHOLD && !isLastBuffer;
}

int shouldDropBuffersToKeyframe(int64_t earlyUs, int64_t elapsedRealtimeUs, int isLastBuffer)
{
    (void)elapsedRealtimeUs;
    return earlyUs < MIN_EARLY_US_VERY_LATE_THRESHOLD && !isLastBuffer;
}

int shouldForceRenderOutputBuffer(int64_t earlyUs, int64_t elapsedSinceLastRenderUs)
{
    return earlyUs < MIN_EARLY_US_LATE_THRESHOLD && elapsedSinceLastRenderUs > 100000;
}

/* ===================== VideoFrameReleaseControl ===================== */

static void VideoFrameReleaseControl_resetReleasedFrameState(VideoFrameReleaseControl *c)
{
    c->lastFrameReleaseTimeNs = MS_C_TIME_UNSET;
    c->prevFrameReleaseTimeNs = MS_C_TIME_UNSET;
    c->lastFramePresentationTimeUs = MS_C_TIME_UNSET;
    c->prevFramePresentationTimeUs = MS_C_TIME_UNSET;
}

static void VideoFrameReleaseControl_lowerFirstFrameState(VideoFrameReleaseControl *c, int state)
{
    if (c->firstFrameState > state) {
        c->firstFrameState = state;
    }
}

void VideoFrameReleaseControl_init(VideoFrameReleaseControl *c)
{
    memset(c, 0, sizeof(*c));
    VideoFrameReleaseHelper_init(&c->frameReleaseHelper);
    FixedFrameRateEstimator_init(&c->frameRateEstimator);
    c->firstFrameState = MS_C_FIRST_FRAME_NOT_RENDERED_ONLY_ALLOWED_IF_STARTED;
    c->initialPositionUs = MS_C_TIME_UNSET;
    c->joiningDeadlineMs = MS_C_TIME_UNSET;
    c->playbackSpeed = 1.0f;
    c->requiresOutputSurface = 1;
    c->earlySchedulingThresholdUs = DEFAULT_EARLY_SCHEDULING_THRESHOLD_US;
    c->skipBuffersWithIdenticalReleaseTime = 1;
    VideoFrameReleaseControl_resetReleasedFrameState(c);
}

void VideoFrameReleaseControl_onStarted(VideoFrameReleaseControl *c)
{
    c->started = 1;
}

void VideoFrameReleaseControl_onStopped(VideoFrameReleaseControl *c)
{
    c->started = 0;
    c->joiningDeadlineMs = MS_C_TIME_UNSET;
}

void VideoFrameReleaseControl_setOutputSurface(VideoFrameReleaseControl *c, int hasSurface)
{
    c->hasOutputSurface = hasSurface;
    c->frameReadyWithoutSurface = 0;
    VideoFrameReleaseControl_lowerFirstFrameState(c, MS_C_FIRST_FRAME_NOT_RENDERED);
}

void VideoFrameReleaseControl_allowReleaseFirstFrameBeforeStarted(VideoFrameReleaseControl *c)
{
    if (c->firstFrameState == MS_C_FIRST_FRAME_NOT_RENDERED_ONLY_ALLOWED_IF_STARTED) {
        c->firstFrameState = MS_C_FIRST_FRAME_NOT_RENDERED;
    }
}

/* releaseFirstFrameMode: 0=IMMEDIATELY, 1=WHEN_STARTED, 2=WHEN_PREVIOUS_STREAM_PROCESSED */
void VideoFrameReleaseControl_onStreamChanged(VideoFrameReleaseControl *c, int releaseFirstFrameMode)
{
    switch (releaseFirstFrameMode) {
    case 0:
        c->firstFrameState = MS_C_FIRST_FRAME_NOT_RENDERED;
        break;
    case 1:
        c->firstFrameState = MS_C_FIRST_FRAME_NOT_RENDERED_ONLY_ALLOWED_IF_STARTED;
        break;
    default:
        VideoFrameReleaseControl_lowerFirstFrameState(
            c, MS_C_FIRST_FRAME_NOT_RENDERED_AFTER_STREAM_CHANGE);
        break;
    }
}

void VideoFrameReleaseControl_reset(VideoFrameReleaseControl *c)
{
    VideoFrameReleaseHelper_onPositionReset(&c->frameReleaseHelper);
    c->initialPositionUs = MS_C_TIME_UNSET;
    VideoFrameReleaseControl_lowerFirstFrameState(c, MS_C_FIRST_FRAME_NOT_RENDERED);
    c->joiningDeadlineMs = MS_C_TIME_UNSET;
    c->frameReadyWithoutSurface = 0;
    VideoFrameReleaseControl_resetReleasedFrameState(c);
}

void VideoFrameReleaseControl_setPlaybackSpeed(VideoFrameReleaseControl *c, float speed)
{
    if (speed <= 0.0f || speed == c->playbackSpeed) {
        if (speed > 0.0f) {
            return;
        }
        speed = 1.0f;
    }
    c->playbackSpeed = speed;
    VideoFrameReleaseHelper_onPlaybackSpeed(&c->frameReleaseHelper, speed);
}

void VideoFrameReleaseControl_setVsyncData(VideoFrameReleaseControl *c, int64_t t, int64_t d)
{
    VideoFrameReleaseHelper_setVsyncData(&c->frameReleaseHelper, t, d);
}

int VideoFrameReleaseControl_onFrameReleasedIsFirstFrame(VideoFrameReleaseControl *c)
{
    int firstFrame = c->firstFrameState != MS_C_FIRST_FRAME_RENDERED;
    c->firstFrameState = MS_C_FIRST_FRAME_RENDERED;
    return firstFrame;
}

static void VideoFrameReleaseControl_updateReleasedFrameState(VideoFrameReleaseControl *c,
                                                              int64_t releaseTimeNs,
                                                              int64_t presentationTimeUs)
{
    c->prevFrameReleaseTimeNs = c->lastFrameReleaseTimeNs;
    c->lastFrameReleaseTimeNs = releaseTimeNs;
    c->prevFramePresentationTimeUs = c->lastFramePresentationTimeUs;
    c->lastFramePresentationTimeUs = presentationTimeUs;
    c->lastReleaseRealtimeUs = ms_media3_elapsedRealtimeUs();
}

/* VideoFrameReleaseControl.calculateEarlyTimeUs */
static int64_t calculateEarlyTimeUs(VideoFrameReleaseControl *c, int64_t positionUs,
                                    int64_t elapsedRealtimeUs, int64_t framePresentationTimeUs)
{
    int64_t earlyUs =
        (int64_t)((double)(framePresentationTimeUs - positionUs) / (double)c->playbackSpeed);
    if (c->started) {
        earlyUs -= ms_media3_elapsedRealtimeUs() - elapsedRealtimeUs;
    }
    return earlyUs;
}

/* VideoFrameReleaseControl.shouldForceRelease */
static int shouldForceRelease(VideoFrameReleaseControl *c, int64_t positionUs, int64_t earlyUs,
                              int64_t outputStreamStartPositionUs)
{
    if (c->joiningDeadlineMs != MS_C_TIME_UNSET && !c->joiningRenderNextFrameImmediately) {
        return 0;
    }
    switch (c->firstFrameState) {
    case MS_C_FIRST_FRAME_NOT_RENDERED_ONLY_ALLOWED_IF_STARTED:
        return c->started;
    case MS_C_FIRST_FRAME_NOT_RENDERED:
        return 1;
    case MS_C_FIRST_FRAME_NOT_RENDERED_AFTER_STREAM_CHANGE:
        return positionUs >= outputStreamStartPositionUs;
    case MS_C_FIRST_FRAME_RENDERED: {
        int64_t elapsedTimeSinceLastReleaseUs =
            ms_media3_elapsedRealtimeUs() - c->lastReleaseRealtimeUs;
        return c->started &&
               (c->disableAdvancingTimestampChecks ||
                (c->initialPositionUs != MS_C_TIME_UNSET && c->initialPositionUs != positionUs)) &&
               shouldForceRenderOutputBuffer(earlyUs, elapsedTimeSinceLastReleaseUs);
    }
    default:
        return 0;
    }
}

static int treatSameReleaseTimeAsDropped(void)
{
    return 0; /* Media3 默认按 skip，除非实验开关；本工程不启用该实验项 */
}

/* VideoFrameReleaseControl.getFrameReleaseActionInternal — 分支顺序与原版一致 */
static int getFrameReleaseActionInternal(
    VideoFrameReleaseControl *c, int64_t presentationTimeUs, int64_t positionUs,
    int64_t elapsedRealtimeUs, int64_t outputStreamStartPositionUs, int isDecodeOnlyFrame,
    int isLastFrame, int64_t frameDurationNs, int64_t frameIndex, int onlyProbe,
    FrameReleaseInfo *frameReleaseInfo, maybeDropBuffersToKeyframe_fn maybeDrop, void *maybeDropCtx)
{
    int hasOutput;
    int64_t systemTimeNs, unadjustedReleaseTimeNs;
    int treatDropAsSkip;
    frameReleaseInfo->earlyUs = 0;
    frameReleaseInfo->releaseTimeNs = MS_C_TIME_UNSET;

    if (!onlyProbe && c->started && c->initialPositionUs == MS_C_TIME_UNSET) {
        c->initialPositionUs = positionUs;
    }

    frameReleaseInfo->earlyUs =
        calculateEarlyTimeUs(c, positionUs, elapsedRealtimeUs, presentationTimeUs);

    if (isDecodeOnlyFrame && !isLastFrame) {
        return FRAME_RELEASE_SKIP;
    }
    hasOutput = c->hasOutputSurface || c->hasOutputBufferRenderer;
    if (!hasOutput && c->requiresOutputSurface) {
        if (!onlyProbe && shouldDropBuffersToKeyframe(frameReleaseInfo->earlyUs, elapsedRealtimeUs,
                                                      isLastFrame) &&
            maybeDrop && maybeDrop(maybeDropCtx, positionUs, /* treatDroppedAsSkipped= */ 1)) {
            return FRAME_RELEASE_IGNORE;
        }
        if (c->started && frameReleaseInfo->earlyUs < 30000) {
            return FRAME_RELEASE_SKIP;
        }
        if (!onlyProbe) {
            c->frameReadyWithoutSurface = 1;
        }
        return FRAME_RELEASE_TRY_AGAIN_LATER;
    }
    if (!onlyProbe && !c->requiresOutputSurface) {
        c->frameReadyWithoutSurface = 1;
    }
    if (shouldForceRelease(c, positionUs, frameReleaseInfo->earlyUs, outputStreamStartPositionUs)) {
        if (!onlyProbe) {
            VideoFrameReleaseControl_updateReleasedFrameState(c, ms_media3_nanoTime(),
                                                              presentationTimeUs);
        }
        return FRAME_RELEASE_IMMEDIATELY;
    }
    if (!c->started || c->initialPositionUs == MS_C_TIME_UNSET ||
        positionUs == c->initialPositionUs) {
        return FRAME_RELEASE_TRY_AGAIN_LATER;
    }

    systemTimeNs = ms_media3_nanoTime();
    unadjustedReleaseTimeNs = systemTimeNs + (frameReleaseInfo->earlyUs * 1000ll);
    frameReleaseInfo->releaseTimeNs = VideoFrameReleaseHelper_adjustReleaseTime(
        &c->frameReleaseHelper, unadjustedReleaseTimeNs, presentationTimeUs, frameDurationNs,
        frameIndex);
    frameReleaseInfo->earlyUs = (frameReleaseInfo->releaseTimeNs - systemTimeNs) / 1000ll;

    treatDropAsSkip =
        c->joiningDeadlineMs != MS_C_TIME_UNSET && !c->joiningRenderNextFrameImmediately;
    if (!onlyProbe && shouldDropBuffersToKeyframe(frameReleaseInfo->earlyUs, elapsedRealtimeUs,
                                                  isLastFrame) &&
        maybeDrop && maybeDrop(maybeDropCtx, positionUs, treatDropAsSkip)) {
        return FRAME_RELEASE_IGNORE;
    }
    if (shouldDropOutputBuffer(frameReleaseInfo->earlyUs, elapsedRealtimeUs, isLastFrame)) {
        return treatDropAsSkip ? FRAME_RELEASE_SKIP : FRAME_RELEASE_DROP;
    }
    if (frameReleaseInfo->earlyUs > c->earlySchedulingThresholdUs) {
        return FRAME_RELEASE_TRY_AGAIN_LATER;
    }
    if (c->skipBuffersWithIdenticalReleaseTime &&
        frameReleaseInfo->releaseTimeNs == c->lastFrameReleaseTimeNs) {
        return treatSameReleaseTimeAsDropped() ? FRAME_RELEASE_DROP : FRAME_RELEASE_SKIP;
    }
    if (!onlyProbe) {
        VideoFrameReleaseControl_updateReleasedFrameState(c, frameReleaseInfo->releaseTimeNs,
                                                          presentationTimeUs);
    }
    return FRAME_RELEASE_SCHEDULED;
}

int VideoFrameReleaseControl_getFrameReleaseAction(
    VideoFrameReleaseControl *c, int64_t presentationTimeUs, int64_t positionUs,
    int64_t elapsedRealtimeUs, int64_t outputStreamStartPositionUs, int isDecodeOnlyFrame,
    int isLastFrame, int64_t frameDurationNs, int64_t frameIndex, FrameReleaseInfo *info,
    maybeDropBuffersToKeyframe_fn maybeDrop, void *maybeDropCtx)
{
    return getFrameReleaseActionInternal(c, presentationTimeUs, positionUs, elapsedRealtimeUs,
                                         outputStreamStartPositionUs, isDecodeOnlyFrame, isLastFrame,
                                         frameDurationNs, frameIndex, 0, info, maybeDrop, maybeDropCtx);
}
