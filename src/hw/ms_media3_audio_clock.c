/*
 * 移植 androidx.media3.exoplayer.audio.AudioTimestampPoller /
 * AudioTrackPositionTracker / DefaultAudioSink.getCurrentPositionUs 与
 * applyMediaPositionParameters，以及 DefaultMediaClock / StandaloneMediaClock。
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
#include <time.h>

static int64_t real_nanoTime(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (int64_t)ts.tv_sec * 1000000000ll + (int64_t)ts.tv_nsec;
}

static int64_t real_elapsedRealtimeMs(void)
{
    return real_nanoTime() / 1000000ll;
}

static ms_media3_clock_fns g_clock = {real_nanoTime, real_elapsedRealtimeMs};

void ms_media3_set_test_clock(const ms_media3_clock_fns *fns)
{
    if (fns && fns->nanoTime && fns->elapsedRealtimeMs) {
        g_clock = *fns;
    } else {
        g_clock.nanoTime = real_nanoTime;
        g_clock.elapsedRealtimeMs = real_elapsedRealtimeMs;
    }
}

int64_t ms_media3_nanoTime(void)
{
    return g_clock.nanoTime();
}

int64_t ms_media3_elapsedRealtimeMs(void)
{
    return g_clock.elapsedRealtimeMs();
}

int64_t ms_media3_elapsedRealtimeUs(void)
{
    return ms_media3_elapsedRealtimeMs() * 1000ll;
}

int64_t ms_media3_msToUs(int64_t ms)
{
    return ms * 1000ll;
}

int64_t ms_media3_usToMs(int64_t us)
{
    return us / 1000ll;
}

/* Util.getMediaDurationForPlayoutDuration */
int64_t ms_media3_getMediaDurationForPlayoutDuration(int64_t playoutDurationUs, float speed)
{
    if (speed == 1.0f) {
        return playoutDurationUs;
    }
    return (int64_t)((double)playoutDurationUs * (double)speed);
}

/* Util.getPlayoutDurationForMediaDuration */
int64_t ms_media3_getPlayoutDurationForMediaDuration(int64_t mediaDurationUs, float speed)
{
    if (speed == 1.0f) {
        return mediaDurationUs;
    }
    return (int64_t)((double)mediaDurationUs / (double)speed);
}

int64_t ms_media3_sampleCountToDurationUs(int64_t sampleCount, int sampleRate)
{
    if (sampleRate <= 0) {
        return 0;
    }
    return (sampleCount * MS_C_MICROS_PER_SECOND) / sampleRate;
}

int64_t ms_media3_durationUsToSampleCount(int64_t durationUs, int sampleRate)
{
    return (durationUs * (int64_t)sampleRate) / MS_C_MICROS_PER_SECOND;
}

int64_t ms_media3_constrainValue(int64_t v, int64_t lo, int64_t hi)
{
    if (v < lo) {
        return lo;
    }
    if (v > hi) {
        return hi;
    }
    return v;
}

static int64_t i64abs(int64_t v)
{
    return v < 0 ? -v : v;
}

/* ===================== AudioTimestampWrapper =====================
 * AudioTimestampPoller.AudioTimestampWrapper */

static int AudioTimestampWrapper_maybeUpdateTimestamp(AudioTimestampWrapper *w)
{
    int64_t rawPositionFrames = 0, nanoTime = 0;
    int updated;
    if (!w->track.getTimestamp) {
        return 0;
    }
    updated = w->track.getTimestamp(w->track.ctx, &rawPositionFrames, &nanoTime);
    if (updated) {
        if (w->lastTimestampRawPositionFrames > rawPositionFrames) {
            if (w->expectTimestampFramePositionReset) {
                w->accumulatedRawTimestampFramePosition += w->lastTimestampRawPositionFrames;
                w->expectTimestampFramePositionReset = 0;
            } else {
                w->rawTimestampFramePositionWrapCount++;
            }
        }
        w->lastTimestampRawPositionFrames = rawPositionFrames;
        w->lastTimestampPositionFrames = rawPositionFrames + w->accumulatedRawTimestampFramePosition +
                                         (w->rawTimestampFramePositionWrapCount << 32);
        w->timestampSystemTimeUs = nanoTime / 1000ll;
        w->hasTimestamp = 1;
    }
    return updated;
}

/* ===================== AudioTimestampPoller ===================== */

static void AudioTimestampPoller_updateState(AudioTimestampPoller *p, int state)
{
    p->state = state;
    switch (state) {
    case STATE_INITIALIZING:
        p->lastTimestampSampleTimeUs = 0;
        p->initialTimestampPositionFrames = MS_C_INDEX_UNSET;
        p->initialTimestampSystemTimeUs = MS_C_TIME_UNSET;
        p->initializeSystemTimeUs = ms_media3_nanoTime() / 1000ll;
        p->sampleIntervalUs = FAST_POLL_INTERVAL_US;
        break;
    case STATE_TIMESTAMP:
        p->sampleIntervalUs = FAST_POLL_INTERVAL_US;
        break;
    case STATE_TIMESTAMP_ADVANCING:
    case STATE_NO_TIMESTAMP:
        p->sampleIntervalUs = SLOW_POLL_INTERVAL_US;
        break;
    case STATE_ERROR:
        p->sampleIntervalUs = ERROR_POLL_INTERVAL_US;
        break;
    default:
        break;
    }
}

void AudioTimestampPoller_reset(AudioTimestampPoller *p)
{
    AudioTimestampPoller_updateState(p, STATE_INITIALIZING);
}

void AudioTimestampPoller_init(AudioTimestampPoller *p, AudioTrackOps track, int sampleRate)
{
    memset(p, 0, sizeof(*p));
    p->audioTimestamp.track = track;
    p->sampleRate = sampleRate;
    AudioTimestampPoller_reset(p);
}

static int64_t AudioTimestampPoller_computeTimestampPositionUs4(AudioTimestampPoller *p,
                                                                int64_t timestampPositionFrames,
                                                                int64_t timestampSystemTimeUs,
                                                                int64_t systemTimeUs,
                                                                float audioTrackPlaybackSpeed)
{
    int64_t timestampPositionUs =
        ms_media3_sampleCountToDurationUs(timestampPositionFrames, p->sampleRate);
    int64_t elapsedSinceTimestampUs = systemTimeUs - timestampSystemTimeUs;
    elapsedSinceTimestampUs = ms_media3_getMediaDurationForPlayoutDuration(
        elapsedSinceTimestampUs, audioTrackPlaybackSpeed);
    return timestampPositionUs + elapsedSinceTimestampUs;
}

static int64_t AudioTimestampPoller_computeTimestampPositionUs(AudioTimestampPoller *p,
                                                               int64_t systemTimeUs,
                                                               float audioTrackPlaybackSpeed)
{
    return AudioTimestampPoller_computeTimestampPositionUs4(
        p, p->audioTimestamp.lastTimestampPositionFrames, p->audioTimestamp.timestampSystemTimeUs,
        systemTimeUs, audioTrackPlaybackSpeed);
}

static int AudioTimestampPoller_isTimestampAdvancingFromInitialTimestamp(
    AudioTimestampPoller *p, int64_t systemTimeUs, float audioTrackPlaybackSpeed)
{
    int64_t positionEstimateUsingInitialTimestampUs;
    int64_t positionEstimateUsingCurrentTimestampUs;
    int64_t positionDriftUs;
    if (p->audioTimestamp.lastTimestampPositionFrames <= p->initialTimestampPositionFrames) {
        return 0;
    }
    positionEstimateUsingInitialTimestampUs = AudioTimestampPoller_computeTimestampPositionUs4(
        p, p->initialTimestampPositionFrames, p->initialTimestampSystemTimeUs, systemTimeUs,
        audioTrackPlaybackSpeed);
    positionEstimateUsingCurrentTimestampUs =
        AudioTimestampPoller_computeTimestampPositionUs(p, systemTimeUs, audioTrackPlaybackSpeed);
    positionDriftUs = i64abs(positionEstimateUsingCurrentTimestampUs -
                             positionEstimateUsingInitialTimestampUs);
    return positionDriftUs < MAX_POSITION_DRIFT_ADVANCING_TIMESTAMP_US;
}

static void AudioTimestampPoller_checkTimestampIsPlausibleAndUpdateErrorState(
    AudioTimestampPoller *p, int64_t systemTimeUs, float audioTrackPlaybackSpeed,
    int64_t playbackHeadPositionEstimateUs)
{
    int64_t timestampSystemTimeUs = p->audioTimestamp.timestampSystemTimeUs;
    int64_t timestampPositionUs =
        AudioTimestampPoller_computeTimestampPositionUs(p, systemTimeUs, audioTrackPlaybackSpeed);
    if (i64abs(timestampSystemTimeUs - systemTimeUs) > MAX_AUDIO_TIMESTAMP_OFFSET_US) {
        AudioTimestampPoller_updateState(p, STATE_ERROR);
    } else if (i64abs(timestampPositionUs - playbackHeadPositionEstimateUs) >
               MAX_AUDIO_TIMESTAMP_OFFSET_US) {
        AudioTimestampPoller_updateState(p, STATE_ERROR);
    } else if (p->state == STATE_ERROR) {
        AudioTimestampPoller_reset(p);
    }
}

void AudioTimestampPoller_maybePollTimestamp(AudioTimestampPoller *p, int64_t systemTimeUs,
                                             float audioTrackPlaybackSpeed,
                                             int64_t playbackHeadPositionEstimateUs,
                                             int forceUpdate)
{
    int updatedTimestamp;
    if (!forceUpdate && (systemTimeUs - p->lastTimestampSampleTimeUs) < p->sampleIntervalUs) {
        return;
    }
    p->lastTimestampSampleTimeUs = systemTimeUs;
    updatedTimestamp = AudioTimestampWrapper_maybeUpdateTimestamp(&p->audioTimestamp);
    if (updatedTimestamp) {
        AudioTimestampPoller_checkTimestampIsPlausibleAndUpdateErrorState(
            p, systemTimeUs, audioTrackPlaybackSpeed, playbackHeadPositionEstimateUs);
    }
    switch (p->state) {
    case STATE_INITIALIZING:
        if (updatedTimestamp) {
            if (p->audioTimestamp.timestampSystemTimeUs >= p->initializeSystemTimeUs) {
                p->initialTimestampPositionFrames = p->audioTimestamp.lastTimestampPositionFrames;
                p->initialTimestampSystemTimeUs = p->audioTimestamp.timestampSystemTimeUs;
                AudioTimestampPoller_updateState(p, STATE_TIMESTAMP);
            }
        } else if (systemTimeUs - p->initializeSystemTimeUs > INITIALIZING_DURATION_US) {
            AudioTimestampPoller_updateState(p, STATE_NO_TIMESTAMP);
        }
        break;
    case STATE_TIMESTAMP:
        if (updatedTimestamp) {
            if (AudioTimestampPoller_isTimestampAdvancingFromInitialTimestamp(
                    p, systemTimeUs, audioTrackPlaybackSpeed)) {
                AudioTimestampPoller_updateState(p, STATE_TIMESTAMP_ADVANCING);
            } else if (systemTimeUs - p->initializeSystemTimeUs > WAIT_FOR_ADVANCE_DURATION_US) {
                AudioTimestampPoller_updateState(p, STATE_NO_TIMESTAMP);
            } else {
                p->initialTimestampPositionFrames = p->audioTimestamp.lastTimestampPositionFrames;
                p->initialTimestampSystemTimeUs = p->audioTimestamp.timestampSystemTimeUs;
            }
        } else {
            AudioTimestampPoller_reset(p);
        }
        break;
    case STATE_TIMESTAMP_ADVANCING:
        if (!updatedTimestamp) {
            AudioTimestampPoller_reset(p);
        }
        break;
    case STATE_NO_TIMESTAMP:
        if (updatedTimestamp) {
            AudioTimestampPoller_reset(p);
        }
        break;
    case STATE_ERROR:
    default:
        break;
    }
}

int AudioTimestampPoller_hasAdvancingTimestamp(const AudioTimestampPoller *p)
{
    return p->state == STATE_TIMESTAMP_ADVANCING;
}

int AudioTimestampPoller_isWaitingForAdvancingTimestamp(const AudioTimestampPoller *p)
{
    return p->state == STATE_INITIALIZING || p->state == STATE_TIMESTAMP;
}

int64_t AudioTimestampPoller_getTimestampPositionUs(AudioTimestampPoller *p, int64_t systemTimeUs,
                                                    float audioTrackPlaybackSpeed)
{
    return AudioTimestampPoller_computeTimestampPositionUs(p, systemTimeUs, audioTrackPlaybackSpeed);
}

void AudioTimestampPoller_expectTimestampFramePositionReset(AudioTimestampPoller *p)
{
    p->audioTimestamp.expectTimestampFramePositionReset = 1;
}

/* ===================== AudioTrackPositionTracker ===================== */

static void AudioTrackPositionTracker_resetSyncParams(AudioTrackPositionTracker *t)
{
    t->smoothedPlayheadOffsetUs = 0;
    t->playheadOffsetCount = 0;
    t->nextPlayheadOffsetIndex = 0;
    t->lastPlayheadSampleTimeUs = 0;
    t->lastPositionUs = MS_C_TIME_UNSET;
    t->lastSystemTimeUs = MS_C_TIME_UNSET;
}

static void AudioTrackPositionTracker_updateRawPlaybackHeadPosition(AudioTrackPositionTracker *t,
                                                                    int64_t currentTimeMs)
{
    int state;
    int64_t rawPlaybackHeadPosition;
    if (!t->audioTrack.getPlayState || !t->audioTrack.getPlaybackHeadPosition) {
        return;
    }
    state = t->audioTrack.getPlayState(t->audioTrack.ctx);
    if (state == PLAYSTATE_STOPPED || (!t->hasBeenStarted && state == PLAYSTATE_PAUSED)) {
        return;
    }
    rawPlaybackHeadPosition = (int64_t)(t->audioTrack.getPlaybackHeadPosition(t->audioTrack.ctx) &
                                        0xFFFFFFFFu);
    /* SDK_INT <= 29 的头卡 0 workaround：由调用方通过 ops 无法直接给 SDK，默认启用（真机 API 30+
     * 头卡 0 极少见；若 head 合法前进会立刻清 forceResetWorkaroundTimeMs）。 */
    if (rawPlaybackHeadPosition == 0 && t->rawPlaybackHeadPosition > 0 &&
        state == PLAYSTATE_PLAYING) {
        if (t->forceResetWorkaroundTimeMs == MS_C_TIME_UNSET) {
            t->forceResetWorkaroundTimeMs = currentTimeMs;
        }
        return;
    }
    t->forceResetWorkaroundTimeMs = MS_C_TIME_UNSET;

    if (t->rawPlaybackHeadPosition > rawPlaybackHeadPosition) {
        if (t->expectRawPlaybackHeadReset) {
            t->sumRawPlaybackHeadPosition += t->rawPlaybackHeadPosition;
            t->expectRawPlaybackHeadReset = 0;
        } else if (t->rawPlaybackHeadPosition - rawPlaybackHeadPosition >=
                   MIN_RAW_PLAYBACK_HEAD_POSITION_WRAP_DISTANCE) {
            t->rawPlaybackHeadWrapCount++;
        } else {
            AudioTrackPositionTracker_resetSyncParams(t);
            AudioTimestampPoller_reset(&t->audioTimestampPoller);
        }
    }
    t->rawPlaybackHeadPosition = rawPlaybackHeadPosition;
}

static int64_t AudioTrackPositionTracker_getSimulatedPlaybackHeadPositionAfterStop(
    AudioTrackPositionTracker *t)
{
    int64_t elapsedTimeSinceStopUs, mediaTimeSinceStopUs, framesSinceStop;
    if (t->audioTrack.getPlayState &&
        t->audioTrack.getPlayState(t->audioTrack.ctx) == PLAYSTATE_PAUSED) {
        return t->stopPlaybackHeadPosition;
    }
    elapsedTimeSinceStopUs =
        ms_media3_msToUs(ms_media3_elapsedRealtimeMs()) - t->stopTimestampUs;
    mediaTimeSinceStopUs = ms_media3_getMediaDurationForPlayoutDuration(
        elapsedTimeSinceStopUs, t->audioTrackPlaybackSpeed);
    framesSinceStop =
        ms_media3_durationUsToSampleCount(mediaTimeSinceStopUs, t->outputSampleRate);
    return t->stopPlaybackHeadPosition + framesSinceStop;
}

static int64_t AudioTrackPositionTracker_getPlaybackHeadPosition(AudioTrackPositionTracker *t)
{
    int64_t currentTimeMs;
    if (t->stopTimestampUs != MS_C_TIME_UNSET) {
        int64_t simulated = AudioTrackPositionTracker_getSimulatedPlaybackHeadPositionAfterStop(t);
        return simulated < t->endPlaybackHeadPosition ? simulated : t->endPlaybackHeadPosition;
    }
    currentTimeMs = ms_media3_elapsedRealtimeMs();
    if (currentTimeMs - t->lastRawPlaybackHeadPositionSampleTimeMs >=
        RAW_PLAYBACK_HEAD_POSITION_UPDATE_INTERVAL_MS) {
        AudioTrackPositionTracker_updateRawPlaybackHeadPosition(t, currentTimeMs);
        t->lastRawPlaybackHeadPositionSampleTimeMs = currentTimeMs;
    }
    return t->rawPlaybackHeadPosition + t->sumRawPlaybackHeadPosition +
           (t->rawPlaybackHeadWrapCount << 32);
}

static int64_t AudioTrackPositionTracker_getPlaybackHeadPositionUs(AudioTrackPositionTracker *t)
{
    return ms_media3_sampleCountToDurationUs(AudioTrackPositionTracker_getPlaybackHeadPosition(t),
                                             t->outputSampleRate);
}

static int AudioTrackPositionTracker_maybeUpdateLatency(AudioTrackPositionTracker *t,
                                                        int64_t systemTimeUs)
{
    int64_t previousLatencyUs = t->latencyUs;
    int latency_ms = 0;
    if (!t->isOutputPcm || t->getLatencyMethodDead ||
        systemTimeUs - t->lastLatencySampleTimeUs < MIN_LATENCY_SAMPLE_INTERVAL_US) {
        return 0;
    }
    t->lastLatencySampleTimeUs = systemTimeUs;
    if (!t->audioTrack.getLatencyMs ||
        t->audioTrack.getLatencyMs(t->audioTrack.ctx, &latency_ms) != 0) {
        t->getLatencyMethodDead = 1;
        return previousLatencyUs != t->latencyUs;
    }
    t->latencyUs = (int64_t)latency_ms * 1000ll - t->bufferSizeUs;
    if (t->latencyUs < 0) {
        t->latencyUs = 0;
    }
    if (t->latencyUs > MAX_LATENCY_US) {
        t->latencyUs = 0;
    }
    return previousLatencyUs != t->latencyUs;
}

static int64_t AudioTrackPositionTracker_getPlaybackHeadPositionEstimateUs(
    AudioTrackPositionTracker *t, int64_t systemTimeUs)
{
    int64_t positionUs;
    if (t->playheadOffsetCount == 0) {
        positionUs = (t->stopTimestampUs != MS_C_TIME_UNSET)
                         ? ms_media3_sampleCountToDurationUs(
                               AudioTrackPositionTracker_getSimulatedPlaybackHeadPositionAfterStop(t),
                               t->outputSampleRate)
                         : AudioTrackPositionTracker_getPlaybackHeadPositionUs(t);
    } else {
        positionUs = ms_media3_getMediaDurationForPlayoutDuration(
            systemTimeUs + t->smoothedPlayheadOffsetUs, t->audioTrackPlaybackSpeed);
    }
    if (positionUs < t->latencyUs) {
        positionUs = 0;
    } else {
        positionUs -= t->latencyUs;
    }
    if (t->stopTimestampUs != MS_C_TIME_UNSET) {
        int64_t endUs =
            ms_media3_sampleCountToDurationUs(t->endPlaybackHeadPosition, t->outputSampleRate);
        if (positionUs > endUs) {
            positionUs = endUs;
        }
    }
    return positionUs;
}

static void AudioTrackPositionTracker_maybeSampleSyncParams(AudioTrackPositionTracker *t)
{
    int64_t systemTimeUs = ms_media3_nanoTime() / 1000ll;
    int latencyUpdated;
    int i;
    if (systemTimeUs - t->lastPlayheadSampleTimeUs >= MIN_PLAYHEAD_OFFSET_SAMPLE_INTERVAL_US) {
        int64_t playbackPositionUs = AudioTrackPositionTracker_getPlaybackHeadPositionUs(t);
        if (playbackPositionUs == 0) {
            return; /* AudioTrack 尚未出声 */
        }
        t->playheadOffsets[t->nextPlayheadOffsetIndex] =
            ms_media3_getPlayoutDurationForMediaDuration(playbackPositionUs,
                                                         t->audioTrackPlaybackSpeed) -
            systemTimeUs;
        t->nextPlayheadOffsetIndex = (t->nextPlayheadOffsetIndex + 1) % MAX_PLAYHEAD_OFFSET_COUNT;
        if (t->playheadOffsetCount < MAX_PLAYHEAD_OFFSET_COUNT) {
            t->playheadOffsetCount++;
        }
        t->lastPlayheadSampleTimeUs = systemTimeUs;
        t->smoothedPlayheadOffsetUs = 0;
        for (i = 0; i < t->playheadOffsetCount; i++) {
            t->smoothedPlayheadOffsetUs += t->playheadOffsets[i] / t->playheadOffsetCount;
        }
    }
    latencyUpdated = AudioTrackPositionTracker_maybeUpdateLatency(t, systemTimeUs);
    AudioTimestampPoller_maybePollTimestamp(
        &t->audioTimestampPoller, systemTimeUs, t->audioTrackPlaybackSpeed,
        AudioTrackPositionTracker_getPlaybackHeadPositionEstimateUs(t, systemTimeUs),
        latencyUpdated);
}

void AudioTrackPositionTracker_init(AudioTrackPositionTracker *t, AudioTrackOps track,
                                    int outputEncodingIsPcm, int outputPcmFrameSize, int bufferSize)
{
    memset(t, 0, sizeof(*t));
    t->audioTrack = track;
    t->audioTrackPlaybackSpeed = 1.0f;
    t->lastSystemTimeUs = MS_C_TIME_UNSET;
    t->lastPositionUs = MS_C_TIME_UNSET;
    t->outputSampleRate = track.getSampleRate ? track.getSampleRate(track.ctx) : 0;
    t->isOutputPcm = outputEncodingIsPcm;
    t->bufferSizeUs = outputEncodingIsPcm && outputPcmFrameSize > 0 && t->outputSampleRate > 0
                          ? ms_media3_sampleCountToDurationUs(bufferSize / outputPcmFrameSize,
                                                              t->outputSampleRate)
                          : MS_C_TIME_UNSET;
    AudioTimestampPoller_init(&t->audioTimestampPoller, track, t->outputSampleRate);
    t->stopTimestampUs = MS_C_TIME_UNSET;
    t->forceResetWorkaroundTimeMs = MS_C_TIME_UNSET;
    t->onPositionAdvancingFromPositionUs = MS_C_TIME_UNSET;
}

void AudioTrackPositionTracker_setAudioTrackPlaybackSpeed(AudioTrackPositionTracker *t, float speed)
{
    t->audioTrackPlaybackSpeed = speed;
}

int64_t AudioTrackPositionTracker_getCurrentPositionUs(AudioTrackPositionTracker *t,
                                                       int64_t writtenFrames)
{
    int64_t systemTimeUs, positionUs, maxPositionUs;
    int useGetTimestampMode;
    int audioTrackPlayState;
    if (t->audioTrack.getPlayState &&
        t->audioTrack.getPlayState(t->audioTrack.ctx) == PLAYSTATE_PLAYING) {
        AudioTrackPositionTracker_maybeSampleSyncParams(t);
    }
    systemTimeUs = ms_media3_nanoTime() / 1000ll;
    useGetTimestampMode = AudioTimestampPoller_hasAdvancingTimestamp(&t->audioTimestampPoller);
    positionUs = useGetTimestampMode
                     ? AudioTimestampPoller_getTimestampPositionUs(
                           &t->audioTimestampPoller, systemTimeUs, t->audioTrackPlaybackSpeed)
                     : AudioTrackPositionTracker_getPlaybackHeadPositionEstimateUs(t, systemTimeUs);
    maxPositionUs = ms_media3_sampleCountToDurationUs(writtenFrames, t->outputSampleRate);
    if (positionUs >= maxPositionUs) {
        AudioTrackPositionTracker_resetSyncParams(t);
        AudioTimestampPoller_reset(&t->audioTimestampPoller);
        return maxPositionUs;
    }
    audioTrackPlayState =
        t->audioTrack.getPlayState ? t->audioTrack.getPlayState(t->audioTrack.ctx) : PLAYSTATE_STOPPED;
    if (audioTrackPlayState == PLAYSTATE_PLAYING) {
        if (t->lastSystemTimeUs != MS_C_TIME_UNSET) {
            int64_t elapsedSystemTimeUs = systemTimeUs - t->lastSystemTimeUs;
            int64_t positionDiffUs = positionUs - t->lastPositionUs;
            int64_t expectedPositionDiffUs = ms_media3_getMediaDurationForPlayoutDuration(
                elapsedSystemTimeUs, t->audioTrackPlaybackSpeed);
            int64_t expectedPositionUs = t->lastPositionUs + expectedPositionDiffUs;
            int64_t positionDriftUs = i64abs(expectedPositionUs - positionUs);
            if (positionDiffUs != 0 && positionDriftUs < MAX_POSITION_DRIFT_FOR_SMOOTHING_US) {
                int64_t maxAllowedDriftUs =
                    expectedPositionDiffUs * MAX_POSITION_SMOOTHING_SPEED_CHANGE_PERCENT / 100;
                positionUs = ms_media3_constrainValue(positionUs, expectedPositionUs - maxAllowedDriftUs,
                                                      expectedPositionUs + maxAllowedDriftUs);
            }
        }
        t->lastSystemTimeUs = systemTimeUs;
        t->lastPositionUs = positionUs;
    }
    return positionUs;
}

void AudioTrackPositionTracker_start(AudioTrackPositionTracker *t)
{
    if (t->stopTimestampUs != MS_C_TIME_UNSET) {
        t->stopTimestampUs = ms_media3_msToUs(ms_media3_elapsedRealtimeMs());
    }
    t->onPositionAdvancingFromPositionUs = AudioTrackPositionTracker_getPlaybackHeadPositionUs(t);
    t->hasBeenStarted = 1;
    AudioTimestampPoller_reset(&t->audioTimestampPoller);
}

int AudioTrackPositionTracker_isPlaying(AudioTrackPositionTracker *t)
{
    return t->audioTrack.getPlayState &&
           t->audioTrack.getPlayState(t->audioTrack.ctx) == PLAYSTATE_PLAYING;
}

int AudioTrackPositionTracker_isStalled(AudioTrackPositionTracker *t, int64_t writtenFrames)
{
    return t->forceResetWorkaroundTimeMs != MS_C_TIME_UNSET && writtenFrames > 0 &&
           ms_media3_elapsedRealtimeMs() - t->forceResetWorkaroundTimeMs >=
               FORCE_RESET_WORKAROUND_TIMEOUT_MS;
}

void AudioTrackPositionTracker_handleEndOfStream(AudioTrackPositionTracker *t, int64_t writtenFrames)
{
    t->stopPlaybackHeadPosition = AudioTrackPositionTracker_getPlaybackHeadPosition(t);
    t->stopTimestampUs = ms_media3_msToUs(ms_media3_elapsedRealtimeMs());
    t->endPlaybackHeadPosition = writtenFrames;
}

void AudioTrackPositionTracker_pause(AudioTrackPositionTracker *t)
{
    AudioTrackPositionTracker_resetSyncParams(t);
    if (t->stopTimestampUs == MS_C_TIME_UNSET) {
        AudioTimestampPoller_reset(&t->audioTimestampPoller);
    }
    t->stopPlaybackHeadPosition = AudioTrackPositionTracker_getPlaybackHeadPosition(t);
}

void AudioTrackPositionTracker_expectRawPlaybackHeadReset(AudioTrackPositionTracker *t)
{
    t->expectRawPlaybackHeadReset = 1;
    AudioTimestampPoller_expectTimestampFramePositionReset(&t->audioTimestampPoller);
}

void AudioTrackPositionTracker_reset(AudioTrackPositionTracker *t)
{
    AudioTrackOps track = t->audioTrack;
    int rate = t->outputSampleRate;
    int isPcm = t->isOutputPcm;
    int64_t buf = t->bufferSizeUs;
    int dead = t->getLatencyMethodDead;
    AudioTrackPositionTracker_resetSyncParams(t);
    AudioTimestampPoller_init(&t->audioTimestampPoller, track, rate);
    t->rawPlaybackHeadPosition = 0;
    t->rawPlaybackHeadWrapCount = 0;
    t->expectRawPlaybackHeadReset = 0;
    t->sumRawPlaybackHeadPosition = 0;
    t->stopTimestampUs = MS_C_TIME_UNSET;
    t->forceResetWorkaroundTimeMs = MS_C_TIME_UNSET;
    t->lastLatencySampleTimeUs = 0;
    t->latencyUs = 0;
    t->audioTrackPlaybackSpeed = 1.0f;
    t->onPositionAdvancingFromPositionUs = MS_C_TIME_UNSET;
    t->hasBeenStarted = 0;
    t->audioTrack = track;
    t->outputSampleRate = rate;
    t->isOutputPcm = isPcm;
    t->bufferSizeUs = buf;
    t->getLatencyMethodDead = dead;
}

/* ===================== DefaultAudioSink 位置 ===================== */

void DefaultAudioSink_init(DefaultAudioSink *s)
{
    memset(s, 0, sizeof(*s));
    s->startMediaTimeUsNeedsInit = 1;
    s->mediaPositionParameters.speed = 1.0f;
}

void DefaultAudioSink_configure(DefaultAudioSink *s, AudioTrackOps track, int sampleRate,
                                int isPcm, int pcmFrameSize, int bufferSize)
{
    AudioTrackPositionTracker_init(&s->tracker, track, isPcm, pcmFrameSize, bufferSize);
    s->tracker_inited = 1;
    s->initialized = 1;
    s->outputSampleRate = sampleRate;
    s->startMediaTimeUsNeedsInit = 1;
}

void DefaultAudioSink_play(DefaultAudioSink *s)
{
    if (s->tracker_inited) {
        AudioTrackPositionTracker_start(&s->tracker);
    }
}

void DefaultAudioSink_pause(DefaultAudioSink *s)
{
    if (s->tracker_inited) {
        AudioTrackPositionTracker_pause(&s->tracker);
    }
}

void DefaultAudioSink_flush(DefaultAudioSink *s)
{
    s->writtenFrames = 0;
    s->mediaPositionParameters.speed = s->mediaPositionParameters.speed > 0
                                           ? s->mediaPositionParameters.speed
                                           : 1.0f;
    s->mediaPositionParameters.mediaTimeUs = 0;
    s->mediaPositionParameters.audioOutputPositionUs = 0;
    s->mediaPositionParameters.mediaPositionDriftUs = 0;
    s->mediaPositionParameters.appliedSkippedSilenceDurationUs = 0;
    s->startMediaTimeUs = 0;
    s->startMediaTimeUsNeedsInit = 1;
    s->startMediaTimeUsNeedsSync = 0;
    s->checkpointCount = 0;
    if (s->tracker_inited) {
        AudioTrackPositionTracker_reset(&s->tracker);
    }
}

void DefaultAudioSink_handleEndOfStream(DefaultAudioSink *s)
{
    if (s->tracker_inited) {
        AudioTrackPositionTracker_handleEndOfStream(&s->tracker, s->writtenFrames);
    }
}

void DefaultAudioSink_addWrittenFrames(DefaultAudioSink *s, int64_t frames)
{
    if (frames > 0) {
        s->writtenFrames += frames;
    }
}

void DefaultAudioSink_handleBufferTimestamps(DefaultAudioSink *s, int64_t presentationTimeUs)
{
    int64_t expectedPresentationTimeUs;
    if (s->startMediaTimeUsNeedsInit) {
        s->startMediaTimeUs = presentationTimeUs > 0 ? presentationTimeUs : 0;
        s->startMediaTimeUsNeedsSync = 0;
        s->startMediaTimeUsNeedsInit = 0;
        /* applyAudioProcessorPlaybackParametersAndSkipSilence(presentationTimeUs) */
        if (s->checkpointCount < MEDIA_POSITION_CHECKPOINT_CAP) {
            MediaPositionParameters *cp = &s->mediaPositionParametersCheckpoints[s->checkpointCount++];
            cp->speed = s->mediaPositionParameters.speed;
            cp->mediaTimeUs = s->startMediaTimeUs;
            cp->audioOutputPositionUs =
                ms_media3_sampleCountToDurationUs(s->writtenFrames, s->outputSampleRate);
            cp->mediaPositionDriftUs = 0;
            cp->appliedSkippedSilenceDurationUs = 0;
        }
        return;
    }
    expectedPresentationTimeUs =
        s->startMediaTimeUs +
        ms_media3_sampleCountToDurationUs(s->writtenFrames, s->outputSampleRate);
    if (!s->startMediaTimeUsNeedsSync &&
        i64abs(expectedPresentationTimeUs - presentationTimeUs) > 200000) {
        s->startMediaTimeUsNeedsSync = 1;
    }
    if (s->startMediaTimeUsNeedsSync) {
        s->startMediaTimeUs += presentationTimeUs - expectedPresentationTimeUs;
        s->startMediaTimeUsNeedsSync = 0;
        if (s->checkpointCount < MEDIA_POSITION_CHECKPOINT_CAP) {
            MediaPositionParameters *cp = &s->mediaPositionParametersCheckpoints[s->checkpointCount++];
            cp->speed = s->mediaPositionParameters.speed;
            cp->mediaTimeUs = presentationTimeUs > 0 ? presentationTimeUs : 0;
            cp->audioOutputPositionUs =
                ms_media3_sampleCountToDurationUs(s->writtenFrames, s->outputSampleRate);
            cp->mediaPositionDriftUs = 0;
            cp->appliedSkippedSilenceDurationUs = 0;
        }
    }
}

void DefaultAudioSink_setPlaybackSpeed(DefaultAudioSink *s, float speed)
{
    if (speed <= 0.0f) {
        speed = 1.0f;
    }
    s->mediaPositionParameters.speed = speed;
    if (s->tracker_inited) {
        AudioTrackPositionTracker_setAudioTrackPlaybackSpeed(&s->tracker, speed);
    }
}

/* DefaultAudioSink.applyMediaPositionParameters
 * 无 AudioProcessorChain 时，actualMediaDuration = estimated（恒速无处理器与原版等价）。 */
int64_t DefaultAudioSink_applyMediaPositionParameters(DefaultAudioSink *s, int64_t positionUs)
{
    int64_t playoutDurationSinceLastCheckpointUs;
    int64_t estimatedMediaDurationSinceLastCheckpointUs;
    while (s->checkpointCount > 0 &&
           positionUs >= s->mediaPositionParametersCheckpoints[0].audioOutputPositionUs) {
        s->mediaPositionParameters = s->mediaPositionParametersCheckpoints[0];
        memmove(&s->mediaPositionParametersCheckpoints[0],
                &s->mediaPositionParametersCheckpoints[1],
                (size_t)(s->checkpointCount - 1) * sizeof(s->mediaPositionParametersCheckpoints[0]));
        s->checkpointCount--;
    }
    playoutDurationSinceLastCheckpointUs =
        positionUs - s->mediaPositionParameters.audioOutputPositionUs;
    estimatedMediaDurationSinceLastCheckpointUs = ms_media3_getMediaDurationForPlayoutDuration(
        playoutDurationSinceLastCheckpointUs, s->mediaPositionParameters.speed);
    if (s->checkpointCount == 0) {
        /* 无 processor 链：actual == estimated，drift 为 0 */
        s->mediaPositionParameters.mediaPositionDriftUs = 0;
        return s->mediaPositionParameters.mediaTimeUs + estimatedMediaDurationSinceLastCheckpointUs;
    }
    return s->mediaPositionParameters.mediaTimeUs + estimatedMediaDurationSinceLastCheckpointUs +
           s->mediaPositionParameters.mediaPositionDriftUs;
}

/* DefaultAudioSink.applySkipping：无静音跳过处理器，恒等。 */
int64_t DefaultAudioSink_applySkipping(DefaultAudioSink *s, int64_t mediaPositionUs,
                                       int64_t audioOutputPositionUs)
{
    (void)audioOutputPositionUs;
    return mediaPositionUs + s->mediaPositionParameters.appliedSkippedSilenceDurationUs;
}

int64_t DefaultAudioSink_getCurrentPositionUs(DefaultAudioSink *s, int sourceEnded)
{
    int64_t positionUs, writtenUs;
    (void)sourceEnded;
    if (!s->initialized || s->startMediaTimeUsNeedsInit) {
        return MS_C_TIME_UNSET;
    }
    positionUs = AudioTrackPositionTracker_getCurrentPositionUs(&s->tracker, s->writtenFrames);
    writtenUs = ms_media3_sampleCountToDurationUs(s->writtenFrames, s->outputSampleRate);
    if (positionUs > writtenUs) {
        positionUs = writtenUs;
    }
    return DefaultAudioSink_applySkipping(
        s, DefaultAudioSink_applyMediaPositionParameters(s, positionUs), positionUs);
}

/* ===================== StandaloneMediaClock ===================== */

void StandaloneMediaClock_init(StandaloneMediaClock *c)
{
    memset(c, 0, sizeof(*c));
    c->speed = 1.0f;
}

void StandaloneMediaClock_start(StandaloneMediaClock *c)
{
    if (!c->started) {
        c->baseElapsedMs = ms_media3_elapsedRealtimeMs();
        c->started = 1;
    }
}

void StandaloneMediaClock_stop(StandaloneMediaClock *c)
{
    if (c->started) {
        StandaloneMediaClock_resetPosition(c, StandaloneMediaClock_getPositionUs(c));
        c->started = 0;
    }
}

void StandaloneMediaClock_resetPosition(StandaloneMediaClock *c, int64_t positionUs)
{
    c->baseUs = positionUs;
    if (c->started) {
        c->baseElapsedMs = ms_media3_elapsedRealtimeMs();
    }
}

int64_t StandaloneMediaClock_getPositionUs(StandaloneMediaClock *c)
{
    int64_t positionUs = c->baseUs;
    if (c->started) {
        int64_t elapsedSinceBaseMs = ms_media3_elapsedRealtimeMs() - c->baseElapsedMs;
        if (c->speed == 1.0f) {
            positionUs += ms_media3_msToUs(elapsedSinceBaseMs);
        } else {
            positionUs +=
                ms_media3_getMediaDurationForPlayoutDuration(ms_media3_msToUs(elapsedSinceBaseMs),
                                                             c->speed);
        }
    }
    return positionUs;
}

void StandaloneMediaClock_setPlaybackParameters(StandaloneMediaClock *c, float speed)
{
    if (c->started) {
        StandaloneMediaClock_resetPosition(c, StandaloneMediaClock_getPositionUs(c));
    }
    c->speed = speed > 0.0f ? speed : 1.0f;
}

/* ===================== DefaultMediaClock ===================== */

void DefaultMediaClock_init(DefaultMediaClock *c)
{
    memset(c, 0, sizeof(*c));
    StandaloneMediaClock_init(&c->standaloneClock);
    c->isUsingStandaloneClock = 1;
}

void DefaultMediaClock_start(DefaultMediaClock *c)
{
    c->standaloneClockIsStarted = 1;
    StandaloneMediaClock_start(&c->standaloneClock);
}

void DefaultMediaClock_stop(DefaultMediaClock *c)
{
    c->standaloneClockIsStarted = 0;
    StandaloneMediaClock_stop(&c->standaloneClock);
}

void DefaultMediaClock_resetPosition(DefaultMediaClock *c, int64_t positionUs)
{
    StandaloneMediaClock_resetPosition(&c->standaloneClock, positionUs);
}

void DefaultMediaClock_setAudioSink(DefaultMediaClock *c, DefaultAudioSink *sink)
{
    c->audioSink = sink;
    c->hasRendererClock = (sink != NULL);
}

void DefaultMediaClock_setPlaybackParameters(DefaultMediaClock *c, float speed)
{
    if (c->audioSink) {
        DefaultAudioSink_setPlaybackSpeed(c->audioSink, speed);
    }
    StandaloneMediaClock_setPlaybackParameters(&c->standaloneClock, speed);
}

static int DefaultMediaClock_shouldUseStandaloneClock(DefaultMediaClock *c)
{
    int64_t pos;
    if (!c->hasRendererClock || c->audioSink == NULL) {
        return 1;
    }
    pos = DefaultAudioSink_getCurrentPositionUs(c->audioSink, 0);
    /* renderer 未 ready：位置尚未初始化 */
    return pos == MS_C_TIME_UNSET;
}

static int64_t DefaultMediaClock_getPositionUs_raw(DefaultMediaClock *c)
{
    return c->isUsingStandaloneClock ? StandaloneMediaClock_getPositionUs(&c->standaloneClock)
                                     : DefaultAudioSink_getCurrentPositionUs(c->audioSink, 0);
}

int64_t DefaultMediaClock_syncAndGetPositionUs(DefaultMediaClock *c, int isReadingAhead)
{
    (void)isReadingAhead;
    if (DefaultMediaClock_shouldUseStandaloneClock(c)) {
        c->isUsingStandaloneClock = 1;
        if (c->standaloneClockIsStarted) {
            StandaloneMediaClock_start(&c->standaloneClock);
        }
    } else {
        int64_t rendererClockPositionUs = DefaultAudioSink_getCurrentPositionUs(c->audioSink, 0);
        if (c->isUsingStandaloneClock) {
            if (rendererClockPositionUs < StandaloneMediaClock_getPositionUs(&c->standaloneClock)) {
                StandaloneMediaClock_stop(&c->standaloneClock);
                return DefaultMediaClock_getPositionUs_raw(c);
            }
            c->isUsingStandaloneClock = 0;
            if (c->standaloneClockIsStarted) {
                StandaloneMediaClock_start(&c->standaloneClock);
            }
        }
        StandaloneMediaClock_resetPosition(&c->standaloneClock, rendererClockPositionUs);
    }
    return DefaultMediaClock_getPositionUs_raw(c);
}

int DefaultMediaClock_isUsingStandaloneClock(const DefaultMediaClock *c)
{
    return c->isUsingStandaloneClock;
}
