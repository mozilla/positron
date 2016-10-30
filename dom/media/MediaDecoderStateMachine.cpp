/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim:set ts=2 sw=2 sts=2 et cindent: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifdef XP_WIN
// Include Windows headers required for enabling high precision timers.
#include "windows.h"
#include "mmsystem.h"
#endif

#include <algorithm>
#include <stdint.h>

#include "gfx2DGlue.h"

#include "mediasink/AudioSinkWrapper.h"
#include "mediasink/DecodedAudioDataSink.h"
#include "mediasink/DecodedStream.h"
#include "mediasink/OutputStreamManager.h"
#include "mediasink/VideoSink.h"
#include "mozilla/DebugOnly.h"
#include "mozilla/Logging.h"
#include "mozilla/mozalloc.h"
#include "mozilla/MathAlgorithms.h"
#include "mozilla/Preferences.h"
#include "mozilla/SharedThreadPool.h"
#include "mozilla/TaskQueue.h"

#include "nsComponentManagerUtils.h"
#include "nsContentUtils.h"
#include "nsIEventTarget.h"
#include "nsITimer.h"
#include "nsPrintfCString.h"
#include "nsTArray.h"
#include "nsDeque.h"
#include "prenv.h"

#include "AccurateSeekTask.h"
#include "AudioSegment.h"
#include "DOMMediaStream.h"
#include "ImageContainer.h"
#include "MediaDecoder.h"
#include "MediaDecoderReader.h"
#include "MediaDecoderReaderWrapper.h"
#include "MediaDecoderStateMachine.h"
#include "MediaShutdownManager.h"
#include "MediaPrefs.h"
#include "MediaTimer.h"
#include "NextFrameSeekTask.h"
#include "TimeUnits.h"
#include "VideoSegment.h"
#include "VideoUtils.h"
#include "gfxPrefs.h"

namespace mozilla {

using namespace mozilla::dom;
using namespace mozilla::layers;
using namespace mozilla::media;

#define NS_DispatchToMainThread(...) CompileError_UseAbstractThreadDispatchInstead

// avoid redefined macro in unified build
#undef FMT
#undef DECODER_LOG
#undef VERBOSE_LOG
#undef SAMPLE_LOG
#undef DECODER_WARN
#undef DUMP_LOG
#undef SFMT
#undef SLOG
#undef SWARN

#define FMT(x, ...) "Decoder=%p " x, mDecoderID, ##__VA_ARGS__
#define DECODER_LOG(...) MOZ_LOG(gMediaDecoderLog, LogLevel::Debug,   (FMT(__VA_ARGS__)))
#define VERBOSE_LOG(...) MOZ_LOG(gMediaDecoderLog, LogLevel::Verbose, (FMT(__VA_ARGS__)))
#define SAMPLE_LOG(...)  MOZ_LOG(gMediaSampleLog,  LogLevel::Debug,   (FMT(__VA_ARGS__)))
#define DECODER_WARN(...) NS_WARNING(nsPrintfCString(FMT(__VA_ARGS__)).get())
#define DUMP_LOG(...) NS_DebugBreak(NS_DEBUG_WARNING, nsPrintfCString(FMT(__VA_ARGS__)).get(), nullptr, nullptr, -1)

// Used by StateObject and its sub-classes
#define SFMT(x, ...) "Decoder=%p state=%s " x, mMaster->mDecoderID, ToStateStr(GetState()), ##__VA_ARGS__
#define SLOG(...) MOZ_LOG(gMediaDecoderLog, LogLevel::Debug, (SFMT(__VA_ARGS__)))
#define SWARN(...) NS_WARNING(nsPrintfCString(SFMT(__VA_ARGS__)).get())

// Certain constants get stored as member variables and then adjusted by various
// scale factors on a per-decoder basis. We want to make sure to avoid using these
// constants directly, so we put them in a namespace.
namespace detail {

// If audio queue has less than this many usecs of decoded audio, we won't risk
// trying to decode the video, we'll skip decoding video up to the next
// keyframe. We may increase this value for an individual decoder if we
// encounter video frames which take a long time to decode.
static const uint32_t LOW_AUDIO_USECS = 300000;

// If more than this many usecs of decoded audio is queued, we'll hold off
// decoding more audio. If we increase the low audio threshold (see
// LOW_AUDIO_USECS above) we'll also increase this value to ensure it's not
// less than the low audio threshold.
static const int64_t AMPLE_AUDIO_USECS = 2000000;

} // namespace detail

// If we have fewer than LOW_VIDEO_FRAMES decoded frames, and
// we're not "prerolling video", we'll skip the video up to the next keyframe
// which is at or after the current playback position.
static const uint32_t LOW_VIDEO_FRAMES = 2;

// Threshold in usecs that used to check if we are low on decoded video.
// If the last video frame's end time |mDecodedVideoEndTime| is more than
// |LOW_VIDEO_THRESHOLD_USECS*mPlaybackRate| after the current clock in
// Advanceframe(), the video decode is lagging, and we skip to next keyframe.
static const int32_t LOW_VIDEO_THRESHOLD_USECS = 60000;

// Arbitrary "frame duration" when playing only audio.
static const int AUDIO_DURATION_USECS = 40000;

// If we increase our "low audio threshold" (see LOW_AUDIO_USECS above), we
// use this as a factor in all our calculations. Increasing this will cause
// us to be more likely to increase our low audio threshold, and to
// increase it by more.
static const int THRESHOLD_FACTOR = 2;

namespace detail {

// If we have less than this much undecoded data available, we'll consider
// ourselves to be running low on undecoded data. We determine how much
// undecoded data we have remaining using the reader's GetBuffered()
// implementation.
static const int64_t LOW_DATA_THRESHOLD_USECS = 5000000;

// LOW_DATA_THRESHOLD_USECS needs to be greater than AMPLE_AUDIO_USECS, otherwise
// the skip-to-keyframe logic can activate when we're running low on data.
static_assert(LOW_DATA_THRESHOLD_USECS > AMPLE_AUDIO_USECS,
              "LOW_DATA_THRESHOLD_USECS is too small");

} // namespace detail

// Amount of excess usecs of data to add in to the "should we buffer" calculation.
static const uint32_t EXHAUSTED_DATA_MARGIN_USECS = 100000;

static int64_t DurationToUsecs(TimeDuration aDuration) {
  return static_cast<int64_t>(aDuration.ToSeconds() * USECS_PER_S);
}

static const uint32_t MIN_VIDEO_QUEUE_SIZE = 3;
static const uint32_t MAX_VIDEO_QUEUE_SIZE = 10;
#ifdef MOZ_APPLEMEDIA
static const uint32_t HW_VIDEO_QUEUE_SIZE = 10;
#else
static const uint32_t HW_VIDEO_QUEUE_SIZE = 3;
#endif
static const uint32_t VIDEO_QUEUE_SEND_TO_COMPOSITOR_SIZE = 9999;

static uint32_t sVideoQueueDefaultSize = MAX_VIDEO_QUEUE_SIZE;
static uint32_t sVideoQueueHWAccelSize = HW_VIDEO_QUEUE_SIZE;
static uint32_t sVideoQueueSendToCompositorSize = VIDEO_QUEUE_SEND_TO_COMPOSITOR_SIZE;

static void InitVideoQueuePrefs() {
  MOZ_ASSERT(NS_IsMainThread());
  static bool sPrefInit = false;
  if (!sPrefInit) {
    sPrefInit = true;
    sVideoQueueDefaultSize = Preferences::GetUint(
      "media.video-queue.default-size", MAX_VIDEO_QUEUE_SIZE);
    sVideoQueueHWAccelSize = Preferences::GetUint(
      "media.video-queue.hw-accel-size", HW_VIDEO_QUEUE_SIZE);
    sVideoQueueSendToCompositorSize = Preferences::GetUint(
      "media.video-queue.send-to-compositor-size", VIDEO_QUEUE_SEND_TO_COMPOSITOR_SIZE);
  }
}

// Delay, in milliseconds, that tabs needs to be in background before video
// decoding is suspended.
static TimeDuration
SuspendBackgroundVideoDelay()
{
  return TimeDuration::FromMilliseconds(
    MediaPrefs::MDSMSuspendBackgroundVideoDelay());
}

class MediaDecoderStateMachine::StateObject
{
public:
  virtual ~StateObject() {}
  virtual void Enter() {}; // Entry action.
  virtual void Exit() {};  // Exit action.
  virtual void Step() {}   // Perform a 'cycle' of this state object.
  virtual State GetState() const = 0;

  // Event handlers for various events.
  // Return true if the event is handled by this state object.
  virtual bool HandleDormant(bool aDormant)
  {
    if (!aDormant) {
      return true;
    }
    mMaster->mQueuedSeek.mTarget =
      SeekTarget(mMaster->mCurrentPosition,
                 SeekTarget::Accurate,
                 MediaDecoderEventVisibility::Suppressed);
    // SeekJob asserts |mTarget.IsValid() == !mPromise.IsEmpty()| so we
    // need to create the promise even it is not used at all.
    RefPtr<MediaDecoder::SeekPromise> unused =
      mMaster->mQueuedSeek.mPromise.Ensure(__func__);
    SetState(DECODER_STATE_DORMANT);
    return true;
  }

  virtual bool HandleCDMProxyReady() { return false; }

protected:
  using Master = MediaDecoderStateMachine;
  explicit StateObject(Master* aPtr) : mMaster(aPtr) {}
  TaskQueue* OwnerThread() const { return mMaster->mTaskQueue; }
  MediaResource* Resource() const { return mMaster->mResource; }
  MediaDecoderReaderWrapper* Reader() const { return mMaster->mReader; }

  // Note this function will delete the current state object.
  // Don't access members to avoid UAF after this call.
  void SetState(State aState) { mMaster->SetState(aState); }

  // Take a raw pointer in order not to change the life cycle of MDSM.
  // It is guaranteed to be valid by MDSM.
  Master* mMaster;
};

class MediaDecoderStateMachine::DecodeMetadataState
  : public MediaDecoderStateMachine::StateObject
{
public:
  explicit DecodeMetadataState(Master* aPtr) : StateObject(aPtr) {}

  void Enter() override
  {
    MOZ_ASSERT(!mMetadataRequest.Exists());
    SLOG("Dispatching AsyncReadMetadata");

    // Set mode to METADATA since we are about to read metadata.
    Resource()->SetReadMode(MediaCacheStream::MODE_METADATA);

    // We disconnect mMetadataRequest in Exit() so it is fine to capture
    // a raw pointer here.
    mMetadataRequest.Begin(Reader()->ReadMetadata()
      ->Then(OwnerThread(), __func__,
        [this] (MetadataHolder* aMetadata) {
          OnMetadataRead(aMetadata);
        },
        [this] (const MediaResult& aError) {
          OnMetadataNotRead(aError);
        }));
  }

  void Exit() override
  {
    mMetadataRequest.DisconnectIfExists();
  }

  State GetState() const override
  {
    return DECODER_STATE_DECODING_METADATA;
  }

  bool HandleDormant(bool aDormant) override
  {
    mPendingDormant = aDormant;
    return true;
  }

private:
  void OnMetadataRead(MetadataHolder* aMetadata)
  {
    mMetadataRequest.Complete();

    if (mPendingDormant) {
      // No need to store mQueuedSeek because we are at position 0.
      SetState(DECODER_STATE_DORMANT);
      return;
    }

    // Set mode to PLAYBACK after reading metadata.
    Resource()->SetReadMode(MediaCacheStream::MODE_PLAYBACK);

    mMaster->mInfo = aMetadata->mInfo;
    mMaster->mMetadataTags = aMetadata->mTags.forget();

    if (mMaster->mInfo.mMetadataDuration.isSome()) {
      mMaster->RecomputeDuration();
    } else if (mMaster->mInfo.mUnadjustedMetadataEndTime.isSome()) {
      RefPtr<Master> master = mMaster;
      Reader()->AwaitStartTime()->Then(OwnerThread(), __func__,
        [master] () {
          NS_ENSURE_TRUE_VOID(!master->IsShutdown());
          TimeUnit unadjusted = master->mInfo.mUnadjustedMetadataEndTime.ref();
          TimeUnit adjustment = master->mReader->StartTime();
          master->mInfo.mMetadataDuration.emplace(unadjusted - adjustment);
          master->RecomputeDuration();
        }, [master, this] () {
          SWARN("Adjusting metadata end time failed");
        }
      );
    }

    if (mMaster->HasVideo()) {
      SLOG("Video decode isAsync=%d HWAccel=%d videoQueueSize=%d",
           Reader()->IsAsync(),
           Reader()->VideoIsHardwareAccelerated(),
           mMaster->GetAmpleVideoFrames());
    }

    // In general, we wait until we know the duration before notifying the decoder.
    // However, we notify  unconditionally in this case without waiting for the start
    // time, since the caller might be waiting on metadataloaded to be fired before
    // feeding in the CDM, which we need to decode the first frame (and
    // thus get the metadata). We could fix this if we could compute the start
    // time by demuxing without necessaring decoding.
    bool waitingForCDM = mMaster->mInfo.IsEncrypted() && !mMaster->mCDMProxy;

    mMaster->mNotifyMetadataBeforeFirstFrame =
      mMaster->mDuration.Ref().isSome() || waitingForCDM;

    if (mMaster->mNotifyMetadataBeforeFirstFrame) {
      mMaster->EnqueueLoadedMetadataEvent();
    }

    if (waitingForCDM) {
      // Metadata parsing was successful but we're still waiting for CDM caps
      // to become available so that we can build the correct decryptor/decoder.
      SetState(DECODER_STATE_WAIT_FOR_CDM);
      return;
    }

    SetState(DECODER_STATE_DECODING_FIRSTFRAME);
  }

  void OnMetadataNotRead(const MediaResult& aError)
  {
    mMetadataRequest.Complete();
    SWARN("Decode metadata failed, shutting down decoder");
    mMaster->DecodeError(aError);
  }

  MozPromiseRequestHolder<MediaDecoderReader::MetadataPromise> mMetadataRequest;

  // True if we need to enter dormant state after reading metadata. Note that
  // we can't enter dormant state until reading metadata is done for some
  // limitations of the reader.
  bool mPendingDormant = false;
};

class MediaDecoderStateMachine::WaitForCDMState
  : public MediaDecoderStateMachine::StateObject
{
public:
  explicit WaitForCDMState(Master* aPtr) : StateObject(aPtr) {}

  State GetState() const override
  {
    return DECODER_STATE_WAIT_FOR_CDM;
  }

  bool HandleCDMProxyReady() override
  {
    SetState(DECODER_STATE_DECODING_FIRSTFRAME);
    return true;
  }
};

class MediaDecoderStateMachine::DormantState
  : public MediaDecoderStateMachine::StateObject
{
public:
  explicit DormantState(Master* aPtr) : StateObject(aPtr) {}

  void Enter() override
  {
    mMaster->DiscardSeekTaskIfExist();
    if (mMaster->IsPlaying()) {
      mMaster->StopPlayback();
    }
    mMaster->Reset();
    mMaster->mReader->ReleaseResources();
  }

  State GetState() const override
  {
    return DECODER_STATE_DORMANT;
  }

  bool HandleDormant(bool aDormant) override
  {
    if (!aDormant) {
      // Exit dormant state.
      SetState(DECODER_STATE_DECODING_METADATA);
    }
    return true;
  }
};

class MediaDecoderStateMachine::DecodingFirstFrameState
  : public MediaDecoderStateMachine::StateObject
{
public:
  explicit DecodingFirstFrameState(Master* aPtr) : StateObject(aPtr) {}

  void Enter() override
  {
    mMaster->DecodeFirstFrame();
  }

  State GetState() const override
  {
    return DECODER_STATE_DECODING_FIRSTFRAME;
  }
};

class MediaDecoderStateMachine::DecodingState
  : public MediaDecoderStateMachine::StateObject
{
public:
  explicit DecodingState(Master* aPtr) : StateObject(aPtr) {}

  void Enter() override
  {
    MOZ_ASSERT(mMaster->mSentFirstFrameLoadedEvent);
    // Pending seek should've been handled by DECODING_FIRSTFRAME before
    // transitioning to DECODING.
    MOZ_ASSERT(!mMaster->mQueuedSeek.Exists());

    if (mMaster->CheckIfDecodeComplete()) {
      SetState(DECODER_STATE_COMPLETED);
      return;
    }

    mDecodeStartTime = TimeStamp::Now();

    // Reset other state to pristine values before starting decode.
    mMaster->mIsAudioPrerolling = !mMaster->DonePrerollingAudio() &&
                                  !Reader()->IsWaitingAudioData();
    mMaster->mIsVideoPrerolling = !mMaster->DonePrerollingVideo() &&
                                  !Reader()->IsWaitingVideoData();

    // Ensure that we've got tasks enqueued to decode data if we need to.
    mMaster->DispatchDecodeTasksIfNeeded();

    mMaster->ScheduleStateMachine();
  }

  void Exit() override
  {
    if (!mDecodeStartTime.IsNull()) {
      TimeDuration decodeDuration = TimeStamp::Now() - mDecodeStartTime;
      SLOG("Exiting DECODING, decoded for %.3lfs", decodeDuration.ToSeconds());
    }
  }

  void Step() override
  {
    if (mMaster->mPlayState != MediaDecoder::PLAY_STATE_PLAYING &&
        mMaster->IsPlaying()) {
      // We're playing, but the element/decoder is in paused state. Stop
      // playing!
      mMaster->StopPlayback();
    }

    // Start playback if necessary so that the clock can be properly queried.
    mMaster->MaybeStartPlayback();

    mMaster->UpdatePlaybackPositionPeriodically();

    MOZ_ASSERT(!mMaster->IsPlaying() ||
               mMaster->IsStateMachineScheduled(),
               "Must have timer scheduled");

    mMaster->MaybeStartBuffering();
  }

  State GetState() const override
  {
    return DECODER_STATE_DECODING;
  }

private:
  // Time at which we started decoding.
  TimeStamp mDecodeStartTime;
};

class MediaDecoderStateMachine::SeekingState
  : public MediaDecoderStateMachine::StateObject
{
public:
  explicit SeekingState(Master* aPtr) : StateObject(aPtr) {}

  State GetState() const override
  {
    return DECODER_STATE_SEEKING;
  }

  bool HandleDormant(bool aDormant) override
  {
    if (!aDormant) {
      return true;
    }
    MOZ_ASSERT(!mMaster->mQueuedSeek.Exists());
    MOZ_ASSERT(mMaster->mCurrentSeek.Exists());
    // Because both audio and video decoders are going to be reset in this
    // method later, we treat a VideoOnly seek task as a normal Accurate
    // seek task so that while it is resumed, both audio and video playback
    // are handled.
    if (mMaster->mCurrentSeek.mTarget.IsVideoOnly()) {
      mMaster->mCurrentSeek.mTarget.SetType(SeekTarget::Accurate);
      mMaster->mCurrentSeek.mTarget.SetVideoOnly(false);
    }
    mMaster->mQueuedSeek = Move(mMaster->mCurrentSeek);
    SetState(DECODER_STATE_DORMANT);
    return true;
  }
};

class MediaDecoderStateMachine::BufferingState
  : public MediaDecoderStateMachine::StateObject
{
public:
  explicit BufferingState(Master* aPtr) : StateObject(aPtr) {}

  void Enter() override
  {
    if (mMaster->IsPlaying()) {
      mMaster->StopPlayback();
    }

    mBufferingStart = TimeStamp::Now();

    MediaStatistics stats = mMaster->GetStatistics();
    SLOG("Playback rate: %.1lfKB/s%s download rate: %.1lfKB/s%s",
         stats.mPlaybackRate/1024, stats.mPlaybackRateReliable ? "" : " (unreliable)",
         stats.mDownloadRate/1024, stats.mDownloadRateReliable ? "" : " (unreliable)");

    mMaster->ScheduleStateMachineIn(USECS_PER_S);
  }

  void Step() override
  {
    TimeStamp now = TimeStamp::Now();
    MOZ_ASSERT(!mBufferingStart.IsNull(), "Must know buffering start time.");

    // With buffering heuristics we will remain in the buffering state if
    // we've not decoded enough data to begin playback, or if we've not
    // downloaded a reasonable amount of data inside our buffering time.
    if (Reader()->UseBufferingHeuristics()) {
      TimeDuration elapsed = now - mBufferingStart;
      bool isLiveStream = Resource()->IsLiveStream();
      if ((isLiveStream || !mMaster->CanPlayThrough()) &&
          elapsed < TimeDuration::FromSeconds(mMaster->mBufferingWait * mMaster->mPlaybackRate) &&
          mMaster->HasLowBufferedData(mMaster->mBufferingWait * USECS_PER_S) &&
          Resource()->IsExpectingMoreData()) {
        SLOG("Buffering: wait %ds, timeout in %.3lfs",
             mMaster->mBufferingWait, mMaster->mBufferingWait - elapsed.ToSeconds());
        mMaster->ScheduleStateMachineIn(USECS_PER_S);
        return;
      }
    } else if (mMaster->OutOfDecodedAudio() || mMaster->OutOfDecodedVideo()) {
      MOZ_ASSERT(Reader()->IsWaitForDataSupported(),
                 "Don't yet have a strategy for non-heuristic + non-WaitForData");
      mMaster->DispatchDecodeTasksIfNeeded();
      MOZ_ASSERT(mMaster->mMinimizePreroll ||
                 !mMaster->OutOfDecodedAudio() ||
                 Reader()->IsRequestingAudioData() ||
                 Reader()->IsWaitingAudioData());
      MOZ_ASSERT(mMaster->mMinimizePreroll ||
                 !mMaster->OutOfDecodedVideo() ||
                 Reader()->IsRequestingVideoData() ||
                 Reader()->IsWaitingVideoData());
      SLOG("In buffering mode, waiting to be notified: outOfAudio: %d, "
           "mAudioStatus: %s, outOfVideo: %d, mVideoStatus: %s",
           mMaster->OutOfDecodedAudio(), mMaster->AudioRequestStatus(),
           mMaster->OutOfDecodedVideo(), mMaster->VideoRequestStatus());
      return;
    }

    SLOG("Buffered for %.3lfs", (now - mBufferingStart).ToSeconds());
    SetState(DECODER_STATE_DECODING);
  }

  State GetState() const override
  {
    return DECODER_STATE_BUFFERING;
  }

private:
  TimeStamp mBufferingStart;
};

class MediaDecoderStateMachine::CompletedState
  : public MediaDecoderStateMachine::StateObject
{
public:
  explicit CompletedState(Master* aPtr) : StateObject(aPtr) {}

  void Enter() override
  {
    mMaster->ScheduleStateMachine();
  }

  void Exit() override
  {
    mSentPlaybackEndedEvent = false;
  }

  void Step() override
  {
    if (mMaster->mPlayState != MediaDecoder::PLAY_STATE_PLAYING &&
        mMaster->IsPlaying()) {
      mMaster->StopPlayback();
    }

    // Play the remaining media. We want to run AdvanceFrame() at least
    // once to ensure the current playback position is advanced to the
    // end of the media, and so that we update the readyState.
    if ((mMaster->HasVideo() && !mMaster->mVideoCompleted) ||
        (mMaster->HasAudio() && !mMaster->mAudioCompleted)) {
      // Start playback if necessary to play the remaining media.
      mMaster->MaybeStartPlayback();
      mMaster->UpdatePlaybackPositionPeriodically();
      MOZ_ASSERT(!mMaster->IsPlaying() ||
                 mMaster->IsStateMachineScheduled(),
                 "Must have timer scheduled");
      return;
    }

    // StopPlayback in order to reset the IsPlaying() state so audio
    // is restarted correctly.
    mMaster->StopPlayback();

    if (mMaster->mPlayState == MediaDecoder::PLAY_STATE_PLAYING &&
        !mSentPlaybackEndedEvent) {
      int64_t clockTime = std::max(mMaster->AudioEndTime(), mMaster->VideoEndTime());
      clockTime = std::max(int64_t(0), std::max(clockTime, mMaster->Duration().ToMicroseconds()));
      mMaster->UpdatePlaybackPosition(clockTime);

      // Ensure readyState is updated before firing the 'ended' event.
      mMaster->UpdateNextFrameStatus();

      mMaster->mOnPlaybackEvent.Notify(MediaEventType::PlaybackEnded);

      mSentPlaybackEndedEvent = true;

      // MediaSink::GetEndTime() must be called before stopping playback.
      mMaster->StopMediaSink();
    }
  }

  State GetState() const override
  {
    return DECODER_STATE_COMPLETED;
  }

private:
  bool mSentPlaybackEndedEvent = false;
};

class MediaDecoderStateMachine::ShutdownState
  : public MediaDecoderStateMachine::StateObject
{
public:
  explicit ShutdownState(Master* aPtr) : StateObject(aPtr) {}

  void Enter() override
  {
    mMaster->mIsShutdown = true;
  }

  void Exit() override
  {
    MOZ_DIAGNOSTIC_ASSERT(false, "Shouldn't escape the SHUTDOWN state.");
  }

  State GetState() const override
  {
    return DECODER_STATE_SHUTDOWN;
  }

  bool HandleDormant(bool aDormant) override
  {
    return true;
  }
};

#define INIT_WATCHABLE(name, val) \
  name(val, "MediaDecoderStateMachine::" #name)
#define INIT_MIRROR(name, val) \
  name(mTaskQueue, val, "MediaDecoderStateMachine::" #name " (Mirror)")
#define INIT_CANONICAL(name, val) \
  name(mTaskQueue, val, "MediaDecoderStateMachine::" #name " (Canonical)")

MediaDecoderStateMachine::MediaDecoderStateMachine(MediaDecoder* aDecoder,
                                                   MediaDecoderReader* aReader) :
  mDecoderID(aDecoder),
  mFrameStats(&aDecoder->GetFrameStatistics()),
  mVideoFrameContainer(aDecoder->GetVideoFrameContainer()),
  mAudioChannel(aDecoder->GetAudioChannel()),
  mTaskQueue(new TaskQueue(GetMediaThreadPool(MediaThreadType::PLAYBACK),
                           /* aSupportsTailDispatch = */ true)),
  mWatchManager(this, mTaskQueue),
  mDispatchedStateMachine(false),
  mDelayedScheduler(mTaskQueue),
  INIT_WATCHABLE(mState, DECODER_STATE_DECODING_METADATA),
  mStateObj(new DecodeMetadataState(this)),
  mCurrentFrameID(0),
  INIT_WATCHABLE(mObservedDuration, TimeUnit()),
  mFragmentEndTime(-1),
  mReader(new MediaDecoderReaderWrapper(mTaskQueue, aReader)),
  mDecodedAudioEndTime(0),
  mDecodedVideoEndTime(0),
  mPlaybackRate(1.0),
  mLowAudioThresholdUsecs(detail::LOW_AUDIO_USECS),
  mAmpleAudioThresholdUsecs(detail::AMPLE_AUDIO_USECS),
  mIsAudioPrerolling(false),
  mIsVideoPrerolling(false),
  mAudioCaptured(false),
  INIT_WATCHABLE(mAudioCompleted, false),
  INIT_WATCHABLE(mVideoCompleted, false),
  mNotifyMetadataBeforeFirstFrame(false),
  mMinimizePreroll(false),
  mDecodeThreadWaiting(false),
  mSentLoadedMetadataEvent(false),
  mSentFirstFrameLoadedEvent(false),
  mVideoDecodeSuspended(false),
  mVideoDecodeSuspendTimer(mTaskQueue),
  mOutputStreamManager(new OutputStreamManager()),
  mResource(aDecoder->GetResource()),
  mAudioOffloading(false),
  INIT_MIRROR(mBuffered, TimeIntervals()),
  INIT_MIRROR(mIsReaderSuspended, true),
  INIT_MIRROR(mEstimatedDuration, NullableTimeUnit()),
  INIT_MIRROR(mExplicitDuration, Maybe<double>()),
  INIT_MIRROR(mPlayState, MediaDecoder::PLAY_STATE_LOADING),
  INIT_MIRROR(mNextPlayState, MediaDecoder::PLAY_STATE_PAUSED),
  INIT_MIRROR(mVolume, 1.0),
  INIT_MIRROR(mPreservesPitch, true),
  INIT_MIRROR(mSameOriginMedia, false),
  INIT_MIRROR(mMediaPrincipalHandle, PRINCIPAL_HANDLE_NONE),
  INIT_MIRROR(mPlaybackBytesPerSecond, 0.0),
  INIT_MIRROR(mPlaybackRateReliable, true),
  INIT_MIRROR(mDecoderPosition, 0),
  INIT_MIRROR(mMediaSeekable, true),
  INIT_MIRROR(mMediaSeekableOnlyInBufferedRanges, false),
  INIT_MIRROR(mIsVisible, true),
  INIT_CANONICAL(mDuration, NullableTimeUnit()),
  INIT_CANONICAL(mIsShutdown, false),
  INIT_CANONICAL(mNextFrameStatus, MediaDecoderOwner::NEXT_FRAME_UNINITIALIZED),
  INIT_CANONICAL(mCurrentPosition, 0),
  INIT_CANONICAL(mPlaybackOffset, 0),
  INIT_CANONICAL(mIsAudioDataAudible, false)
{
  MOZ_COUNT_CTOR(MediaDecoderStateMachine);
  NS_ASSERTION(NS_IsMainThread(), "Should be on main thread.");

  InitVideoQueuePrefs();

  mBufferingWait = 15;
  mLowDataThresholdUsecs = detail::LOW_DATA_THRESHOLD_USECS;

#ifdef XP_WIN
  // Ensure high precision timers are enabled on Windows, otherwise the state
  // machine isn't woken up at reliable intervals to set the next frame,
  // and we drop frames while painting. Note that multiple calls to this
  // function per-process is OK, provided each call is matched by a corresponding
  // timeEndPeriod() call.
  timeBeginPeriod(1);
#endif
}

#undef INIT_WATCHABLE
#undef INIT_MIRROR
#undef INIT_CANONICAL

MediaDecoderStateMachine::~MediaDecoderStateMachine()
{
  MOZ_ASSERT(NS_IsMainThread(), "Should be on main thread.");
  MOZ_COUNT_DTOR(MediaDecoderStateMachine);

#ifdef XP_WIN
  timeEndPeriod(1);
#endif
}

void
MediaDecoderStateMachine::InitializationTask(MediaDecoder* aDecoder)
{
  MOZ_ASSERT(OnTaskQueue());

  // Connect mirrors.
  mBuffered.Connect(mReader->CanonicalBuffered());
  mIsReaderSuspended.Connect(mReader->CanonicalIsSuspended());
  mEstimatedDuration.Connect(aDecoder->CanonicalEstimatedDuration());
  mExplicitDuration.Connect(aDecoder->CanonicalExplicitDuration());
  mPlayState.Connect(aDecoder->CanonicalPlayState());
  mNextPlayState.Connect(aDecoder->CanonicalNextPlayState());
  mVolume.Connect(aDecoder->CanonicalVolume());
  mPreservesPitch.Connect(aDecoder->CanonicalPreservesPitch());
  mSameOriginMedia.Connect(aDecoder->CanonicalSameOriginMedia());
  mMediaPrincipalHandle.Connect(aDecoder->CanonicalMediaPrincipalHandle());
  mPlaybackBytesPerSecond.Connect(aDecoder->CanonicalPlaybackBytesPerSecond());
  mPlaybackRateReliable.Connect(aDecoder->CanonicalPlaybackRateReliable());
  mDecoderPosition.Connect(aDecoder->CanonicalDecoderPosition());
  mMediaSeekable.Connect(aDecoder->CanonicalMediaSeekable());
  mMediaSeekableOnlyInBufferedRanges.Connect(aDecoder->CanonicalMediaSeekableOnlyInBufferedRanges());

  // Initialize watchers.
  mWatchManager.Watch(mBuffered, &MediaDecoderStateMachine::BufferedRangeUpdated);
  mWatchManager.Watch(mIsReaderSuspended, &MediaDecoderStateMachine::ReaderSuspendedChanged);
  mWatchManager.Watch(mState, &MediaDecoderStateMachine::UpdateNextFrameStatus);
  mWatchManager.Watch(mAudioCompleted, &MediaDecoderStateMachine::UpdateNextFrameStatus);
  mWatchManager.Watch(mVideoCompleted, &MediaDecoderStateMachine::UpdateNextFrameStatus);
  mWatchManager.Watch(mVolume, &MediaDecoderStateMachine::VolumeChanged);
  mWatchManager.Watch(mPreservesPitch, &MediaDecoderStateMachine::PreservesPitchChanged);
  mWatchManager.Watch(mEstimatedDuration, &MediaDecoderStateMachine::RecomputeDuration);
  mWatchManager.Watch(mExplicitDuration, &MediaDecoderStateMachine::RecomputeDuration);
  mWatchManager.Watch(mObservedDuration, &MediaDecoderStateMachine::RecomputeDuration);
  mWatchManager.Watch(mPlayState, &MediaDecoderStateMachine::PlayStateChanged);

  if (MediaPrefs::MDSMSuspendBackgroundVideoEnabled()) {
    mIsVisible.Connect(aDecoder->CanonicalIsVisible());
    mWatchManager.Watch(mIsVisible, &MediaDecoderStateMachine::VisibilityChanged);
  }

  // Configure MediaDecoderReaderWrapper.
  SetMediaDecoderReaderWrapperCallback();
}

void
MediaDecoderStateMachine::AudioAudibleChanged(bool aAudible)
{
  mIsAudioDataAudible = aAudible;
}

media::MediaSink*
MediaDecoderStateMachine::CreateAudioSink()
{
  RefPtr<MediaDecoderStateMachine> self = this;
  auto audioSinkCreator = [self] () {
    MOZ_ASSERT(self->OnTaskQueue());
    DecodedAudioDataSink* audioSink = new DecodedAudioDataSink(
      self->mTaskQueue, self->mAudioQueue, self->GetMediaTime(),
      self->mInfo.mAudio, self->mAudioChannel);

    self->mAudibleListener = audioSink->AudibleEvent().Connect(
      self->mTaskQueue, self.get(), &MediaDecoderStateMachine::AudioAudibleChanged);
    return audioSink;
  };
  return new AudioSinkWrapper(mTaskQueue, audioSinkCreator);
}

already_AddRefed<media::MediaSink>
MediaDecoderStateMachine::CreateMediaSink(bool aAudioCaptured)
{
  RefPtr<media::MediaSink> audioSink = aAudioCaptured
    ? new DecodedStream(mTaskQueue, mAudioQueue, mVideoQueue,
                        mOutputStreamManager, mSameOriginMedia.Ref(),
                        mMediaPrincipalHandle.Ref())
    : CreateAudioSink();

  RefPtr<media::MediaSink> mediaSink =
    new VideoSink(mTaskQueue, audioSink, mVideoQueue,
                  mVideoFrameContainer, *mFrameStats,
                  sVideoQueueSendToCompositorSize);
  return mediaSink.forget();
}

bool MediaDecoderStateMachine::HasFutureAudio()
{
  MOZ_ASSERT(OnTaskQueue());
  NS_ASSERTION(HasAudio(), "Should only call HasFutureAudio() when we have audio");
  // We've got audio ready to play if:
  // 1. We've not completed playback of audio, and
  // 2. we either have more than the threshold of decoded audio available, or
  //    we've completely decoded all audio (but not finished playing it yet
  //    as per 1).
  return !mAudioCompleted &&
         (GetDecodedAudioDuration() >
            mLowAudioThresholdUsecs * mPlaybackRate ||
          AudioQueue().IsFinished());
}

bool MediaDecoderStateMachine::HaveNextFrameData()
{
  MOZ_ASSERT(OnTaskQueue());
  return (!HasAudio() || HasFutureAudio()) &&
         (!HasVideo() || VideoQueue().GetSize() > 1);
}

int64_t
MediaDecoderStateMachine::GetDecodedAudioDuration()
{
  MOZ_ASSERT(OnTaskQueue());
  if (mMediaSink->IsStarted()) {
    // mDecodedAudioEndTime might be smaller than GetClock() when there is
    // overlap between 2 adjacent audio samples or when we are playing
    // a chained ogg file.
    return std::max<int64_t>(mDecodedAudioEndTime - GetClock(), 0);
  }
  // MediaSink not started. All audio samples are in the queue.
  return AudioQueue().Duration();
}

bool MediaDecoderStateMachine::HaveEnoughDecodedAudio()
{
  MOZ_ASSERT(OnTaskQueue());

  int64_t ampleAudioUSecs = mAmpleAudioThresholdUsecs * mPlaybackRate;
  if (AudioQueue().GetSize() == 0 ||
      GetDecodedAudioDuration() < ampleAudioUSecs) {
    return false;
  }

  // MDSM will ensure buffering level is high enough for playback speed at 1x
  // at which the DecodedStream is playing.
  return true;
}

bool MediaDecoderStateMachine::HaveEnoughDecodedVideo()
{
  MOZ_ASSERT(OnTaskQueue());

  if (VideoQueue().GetSize() == 0) {
    return false;
  }

  if (VideoQueue().GetSize() - 1 < GetAmpleVideoFrames() * mPlaybackRate) {
    return false;
  }

  return true;
}

bool
MediaDecoderStateMachine::NeedToDecodeVideo()
{
  MOZ_ASSERT(OnTaskQueue());
  SAMPLE_LOG("NeedToDecodeVideo() isDec=%d minPrl=%d enufVid=%d",
             IsVideoDecoding(), mMinimizePreroll, HaveEnoughDecodedVideo());
  return IsVideoDecoding() &&
         mState != DECODER_STATE_SEEKING &&
         ((!mSentFirstFrameLoadedEvent && VideoQueue().GetSize() == 0) ||
          (!mMinimizePreroll && !HaveEnoughDecodedVideo()));
}

bool
MediaDecoderStateMachine::NeedToSkipToNextKeyframe()
{
  MOZ_ASSERT(OnTaskQueue());
  // Don't skip when we're still decoding first frames.
  if (!mSentFirstFrameLoadedEvent) {
    return false;
  }
  MOZ_ASSERT(mState == DECODER_STATE_DECODING ||
             mState == DECODER_STATE_BUFFERING ||
             mState == DECODER_STATE_SEEKING);

  // Since GetClock() can only be called after starting MediaSink, we return
  // false quickly if it is not started because we won't fall behind playback
  // when not consuming media data.
  if (!mMediaSink->IsStarted()) {
    return false;
  }

  // We are in seeking or buffering states, don't skip frame.
  if (!IsVideoDecoding() || mState == DECODER_STATE_BUFFERING ||
      mState == DECODER_STATE_SEEKING) {
    return false;
  }

  // Don't skip frame for video-only decoded stream because the clock time of
  // the stream relies on the video frame.
  if (mAudioCaptured && !HasAudio()) {
    return false;
  }

  // We'll skip the video decode to the next keyframe if we're low on
  // audio, or if we're low on video, provided we're not running low on
  // data to decode. If we're running low on downloaded data to decode,
  // we won't start keyframe skipping, as we'll be pausing playback to buffer
  // soon anyway and we'll want to be able to display frames immediately
  // after buffering finishes. We ignore the low audio calculations for
  // readers that are async, as since their audio decode runs on a different
  // task queue it should never run low and skipping won't help their decode.
  bool isLowOnDecodedAudio = !mReader->IsAsync() &&
                             !mIsAudioPrerolling && IsAudioDecoding() &&
                             (GetDecodedAudioDuration() <
                              mLowAudioThresholdUsecs * mPlaybackRate);
  bool isLowOnDecodedVideo = !mIsVideoPrerolling &&
                             ((GetClock() - mDecodedVideoEndTime) * mPlaybackRate >
                              LOW_VIDEO_THRESHOLD_USECS);
  bool lowBuffered = HasLowBufferedData();

  if ((isLowOnDecodedAudio || isLowOnDecodedVideo) && !lowBuffered) {
    DECODER_LOG("Skipping video decode to the next keyframe lowAudio=%d lowVideo=%d lowUndecoded=%d async=%d",
                isLowOnDecodedAudio, isLowOnDecodedVideo, lowBuffered, mReader->IsAsync());
    return true;
  }

  return false;
}

bool
MediaDecoderStateMachine::NeedToDecodeAudio()
{
  MOZ_ASSERT(OnTaskQueue());
  SAMPLE_LOG("NeedToDecodeAudio() isDec=%d minPrl=%d enufAud=%d",
             IsAudioDecoding(), mMinimizePreroll, HaveEnoughDecodedAudio());

  return IsAudioDecoding() &&
         mState != DECODER_STATE_SEEKING &&
         ((!mSentFirstFrameLoadedEvent && AudioQueue().GetSize() == 0) ||
          (!mMinimizePreroll && !HaveEnoughDecodedAudio()));
}

void
MediaDecoderStateMachine::OnAudioDecoded(MediaData* aAudioSample)
{
  MOZ_ASSERT(OnTaskQueue());
  MOZ_ASSERT(mState != DECODER_STATE_SEEKING);

  RefPtr<MediaData> audio(aAudioSample);
  MOZ_ASSERT(audio);

  // audio->GetEndTime() is not always mono-increasing in chained ogg.
  mDecodedAudioEndTime = std::max(audio->GetEndTime(), mDecodedAudioEndTime);

  SAMPLE_LOG("OnAudioDecoded [%lld,%lld]", audio->mTime, audio->GetEndTime());

  switch (mState) {
    case DECODER_STATE_BUFFERING: {
      // If we're buffering, this may be the sample we need to stop buffering.
      // Save it and schedule the state machine.
      Push(audio, MediaData::AUDIO_DATA);
      ScheduleStateMachine();
      return;
    }

    case DECODER_STATE_DECODING_FIRSTFRAME: {
      Push(audio, MediaData::AUDIO_DATA);
      MaybeFinishDecodeFirstFrame();
      return;
    }

    case DECODER_STATE_DECODING: {
      Push(audio, MediaData::AUDIO_DATA);
      if (mIsAudioPrerolling && DonePrerollingAudio()) {
        StopPrerollingAudio();
      }
      return;
    }

    default: {
      // Ignore other cases.
      return;
    }
  }
}

void
MediaDecoderStateMachine::Push(MediaData* aSample, MediaData::Type aSampleType)
{
  MOZ_ASSERT(OnTaskQueue());
  MOZ_ASSERT(aSample);

  if (aSample->mType == MediaData::AUDIO_DATA) {
    // TODO: Send aSample to MSG and recalculate readystate before pushing,
    // otherwise AdvanceFrame may pop the sample before we have a chance
    // to reach playing.
    AudioQueue().Push(aSample);
  } else if (aSample->mType == MediaData::VIDEO_DATA) {
    // TODO: Send aSample to MSG and recalculate readystate before pushing,
    // otherwise AdvanceFrame may pop the sample before we have a chance
    // to reach playing.
    aSample->As<VideoData>()->mFrameID = ++mCurrentFrameID;
    VideoQueue().Push(aSample);
  } else {
    // TODO: Handle MediaRawData, determine which queue should be pushed.
  }
  UpdateNextFrameStatus();
  DispatchDecodeTasksIfNeeded();
}

void
MediaDecoderStateMachine::OnAudioPopped(const RefPtr<MediaData>& aSample)
{
  MOZ_ASSERT(OnTaskQueue());

  mPlaybackOffset = std::max(mPlaybackOffset.Ref(), aSample->mOffset);
  UpdateNextFrameStatus();
  DispatchAudioDecodeTaskIfNeeded();
}

void
MediaDecoderStateMachine::OnVideoPopped(const RefPtr<MediaData>& aSample)
{
  MOZ_ASSERT(OnTaskQueue());
  mPlaybackOffset = std::max(mPlaybackOffset.Ref(), aSample->mOffset);
  UpdateNextFrameStatus();
  DispatchVideoDecodeTaskIfNeeded();
}

void
MediaDecoderStateMachine::OnNotDecoded(MediaData::Type aType,
                                       const MediaResult& aError)
{
  MOZ_ASSERT(OnTaskQueue());
  MOZ_ASSERT(mState != DECODER_STATE_SEEKING);

  SAMPLE_LOG("OnNotDecoded (aType=%u, aError=%u)", aType, aError.Code());
  bool isAudio = aType == MediaData::AUDIO_DATA;
  MOZ_ASSERT_IF(!isAudio, aType == MediaData::VIDEO_DATA);

  if (IsShutdown()) {
    // Already shutdown;
    return;
  }

  // If the decoder is waiting for data, we tell it to call us back when the
  // data arrives.
  if (aError == NS_ERROR_DOM_MEDIA_WAITING_FOR_DATA) {
    MOZ_ASSERT(mReader->IsWaitForDataSupported(),
               "Readers that send WAITING_FOR_DATA need to implement WaitForData");
    mReader->WaitForData(aType);

    // We are out of data to decode and will enter buffering mode soon.
    // We want to play the frames we have already decoded, so we stop pre-rolling
    // and ensure that loadeddata is fired as required.
    if (isAudio) {
      StopPrerollingAudio();
    } else {
      StopPrerollingVideo();
    }
    return;
  }

  if (aError == NS_ERROR_DOM_MEDIA_CANCELED) {
    if (isAudio) {
      EnsureAudioDecodeTaskQueued();
    } else {
      EnsureVideoDecodeTaskQueued();
    }
    return;
  }

  // If this is a decode error, delegate to the generic error path.
  if (aError != NS_ERROR_DOM_MEDIA_END_OF_STREAM) {
    DecodeError(aError);
    return;
  }

  // This is an EOS. Finish off the queue, and then handle things based on our
  // state.
  if (isAudio) {
    AudioQueue().Finish();
    StopPrerollingAudio();
  } else {
    VideoQueue().Finish();
    StopPrerollingVideo();
  }
  switch (mState) {
    case DECODER_STATE_DECODING_FIRSTFRAME:
      MaybeFinishDecodeFirstFrame();
      return;
    case DECODER_STATE_BUFFERING:
    case DECODER_STATE_DECODING: {
      if (CheckIfDecodeComplete()) {
        SetState(DECODER_STATE_COMPLETED);
        return;
      }
      // Schedule next cycle to see if we can leave buffering state.
      if (mState == DECODER_STATE_BUFFERING) {
        ScheduleStateMachine();
      }
      return;
    }
    default: {
      return;
    }
  }
}

void
MediaDecoderStateMachine::MaybeFinishDecodeFirstFrame()
{
  MOZ_ASSERT(OnTaskQueue());
  MOZ_ASSERT(!mSentFirstFrameLoadedEvent);

  if ((IsAudioDecoding() && AudioQueue().GetSize() == 0) ||
      (IsVideoDecoding() && VideoQueue().GetSize() == 0)) {
    return;
  }

  FinishDecodeFirstFrame();

  if (mQueuedSeek.Exists()) {
    InitiateSeek(Move(mQueuedSeek));
  } else {
    SetState(DECODER_STATE_DECODING);
  }
}

void
MediaDecoderStateMachine::OnVideoDecoded(MediaData* aVideoSample,
                                         TimeStamp aDecodeStartTime)
{
  MOZ_ASSERT(OnTaskQueue());
  MOZ_ASSERT(mState != DECODER_STATE_SEEKING);

  RefPtr<MediaData> video(aVideoSample);
  MOZ_ASSERT(video);

  // Handle abnormal or negative timestamps.
  mDecodedVideoEndTime = std::max(mDecodedVideoEndTime, video->GetEndTime());

  SAMPLE_LOG("OnVideoDecoded [%lld,%lld]", video->mTime, video->GetEndTime());

  switch (mState) {
    case DECODER_STATE_BUFFERING: {
      // If we're buffering, this may be the sample we need to stop buffering.
      // Save it and schedule the state machine.
      Push(video, MediaData::VIDEO_DATA);
      ScheduleStateMachine();
      return;
    }

    case DECODER_STATE_DECODING_FIRSTFRAME: {
      Push(video, MediaData::VIDEO_DATA);
      MaybeFinishDecodeFirstFrame();
      return;
    }

    case DECODER_STATE_DECODING: {
      Push(video, MediaData::VIDEO_DATA);
      if (mIsVideoPrerolling && DonePrerollingVideo()) {
        StopPrerollingVideo();
      }

      // For non async readers, if the requested video sample was slow to
      // arrive, increase the amount of audio we buffer to ensure that we
      // don't run out of audio. This is unnecessary for async readers,
      // since they decode audio and video on different threads so they
      // are unlikely to run out of decoded audio.
      if (mReader->IsAsync()) {
        return;
      }
      TimeDuration decodeTime = TimeStamp::Now() - aDecodeStartTime;
      if (THRESHOLD_FACTOR * DurationToUsecs(decodeTime) > mLowAudioThresholdUsecs &&
          !HasLowBufferedData())
      {
        mLowAudioThresholdUsecs =
          std::min(THRESHOLD_FACTOR * DurationToUsecs(decodeTime), mAmpleAudioThresholdUsecs);
        mAmpleAudioThresholdUsecs = std::max(THRESHOLD_FACTOR * mLowAudioThresholdUsecs,
                                              mAmpleAudioThresholdUsecs);
        DECODER_LOG("Slow video decode, set mLowAudioThresholdUsecs=%lld mAmpleAudioThresholdUsecs=%lld",
                    mLowAudioThresholdUsecs, mAmpleAudioThresholdUsecs);
      }
      return;
    }
    default: {
      // Ignore other cases.
      return;
    }
  }
}

bool
MediaDecoderStateMachine::IsAudioDecoding()
{
  MOZ_ASSERT(OnTaskQueue());
  return HasAudio() && !AudioQueue().IsFinished();
}

bool
MediaDecoderStateMachine::IsVideoDecoding()
{
  MOZ_ASSERT(OnTaskQueue());
  return HasVideo() && !VideoQueue().IsFinished();
}

bool
MediaDecoderStateMachine::CheckIfDecodeComplete()
{
  MOZ_ASSERT(OnTaskQueue());
  // DecodeComplete is possible only after decoding first frames.
  MOZ_ASSERT(mSentFirstFrameLoadedEvent);
  MOZ_ASSERT(mState == DECODER_STATE_DECODING ||
             mState == DECODER_STATE_BUFFERING);
  return !IsVideoDecoding() && !IsAudioDecoding();
}

bool MediaDecoderStateMachine::IsPlaying() const
{
  MOZ_ASSERT(OnTaskQueue());
  return mMediaSink->IsPlaying();
}

nsresult MediaDecoderStateMachine::Init(MediaDecoder* aDecoder)
{
  MOZ_ASSERT(NS_IsMainThread());

  // Dispatch initialization that needs to happen on that task queue.
  nsCOMPtr<nsIRunnable> r = NewRunnableMethod<RefPtr<MediaDecoder>>(
    this, &MediaDecoderStateMachine::InitializationTask, aDecoder);
  mTaskQueue->Dispatch(r.forget());

  mAudioQueueListener = AudioQueue().PopEvent().Connect(
    mTaskQueue, this, &MediaDecoderStateMachine::OnAudioPopped);
  mVideoQueueListener = VideoQueue().PopEvent().Connect(
    mTaskQueue, this, &MediaDecoderStateMachine::OnVideoPopped);

  mMetadataManager.Connect(mReader->TimedMetadataEvent(), OwnerThread());

  mMediaSink = CreateMediaSink(mAudioCaptured);

  mCDMProxyPromise.Begin(aDecoder->RequestCDMProxy()->Then(
    OwnerThread(), __func__, this,
    &MediaDecoderStateMachine::OnCDMProxyReady,
    &MediaDecoderStateMachine::OnCDMProxyNotReady));

  nsresult rv = mReader->Init();
  NS_ENSURE_SUCCESS(rv, rv);

  RefPtr<MediaDecoderStateMachine> self = this;
  OwnerThread()->Dispatch(NS_NewRunnableFunction([self] () {
    self->mStateObj->Enter();
  }));

  return NS_OK;
}

void
MediaDecoderStateMachine::SetMediaDecoderReaderWrapperCallback()
{
  MOZ_ASSERT(OnTaskQueue());

  mAudioCallback = mReader->AudioCallback().Connect(
    mTaskQueue, [this] (AudioCallbackData aData) {
    if (aData.is<MediaData*>()) {
      OnAudioDecoded(aData.as<MediaData*>());
    } else {
      OnNotDecoded(MediaData::AUDIO_DATA, aData.as<MediaResult>());
    }
  });

  mVideoCallback = mReader->VideoCallback().Connect(
    mTaskQueue, [this] (VideoCallbackData aData) {
    typedef Tuple<MediaData*, TimeStamp> Type;
    if (aData.is<Type>()) {
      auto&& v = aData.as<Type>();
      OnVideoDecoded(Get<0>(v), Get<1>(v));
    } else {
      OnNotDecoded(MediaData::VIDEO_DATA, aData.as<MediaResult>());
    }
  });

  mAudioWaitCallback = mReader->AudioWaitCallback().Connect(
    mTaskQueue, [this] (WaitCallbackData aData) {
    if (aData.is<MediaData::Type>()) {
      EnsureAudioDecodeTaskQueued();
    }
  });

  mVideoWaitCallback = mReader->VideoWaitCallback().Connect(
    mTaskQueue, [this] (WaitCallbackData aData) {
    if (aData.is<MediaData::Type>()) {
      EnsureVideoDecodeTaskQueued();
    }
  });
}

void
MediaDecoderStateMachine::CancelMediaDecoderReaderWrapperCallback()
{
  MOZ_ASSERT(OnTaskQueue());
  mAudioCallback.Disconnect();
  mVideoCallback.Disconnect();
  mAudioWaitCallback.Disconnect();
  mVideoWaitCallback.Disconnect();
}

void MediaDecoderStateMachine::StopPlayback()
{
  MOZ_ASSERT(OnTaskQueue());
  DECODER_LOG("StopPlayback()");

  mOnPlaybackEvent.Notify(MediaEventType::PlaybackStopped);

  if (IsPlaying()) {
    mMediaSink->SetPlaying(false);
    MOZ_ASSERT(!IsPlaying());
  }

  DispatchDecodeTasksIfNeeded();
}

void MediaDecoderStateMachine::MaybeStartPlayback()
{
  MOZ_ASSERT(OnTaskQueue());
  // Should try to start playback only after decoding first frames.
  MOZ_ASSERT(mSentFirstFrameLoadedEvent);
  MOZ_ASSERT(mState == DECODER_STATE_DECODING ||
             mState == DECODER_STATE_COMPLETED);

  if (IsPlaying()) {
    // Logging this case is really spammy - don't do it.
    return;
  }

  bool playStatePermits = mPlayState == MediaDecoder::PLAY_STATE_PLAYING;
  if (!playStatePermits || mIsAudioPrerolling ||
      mIsVideoPrerolling || mAudioOffloading) {
    DECODER_LOG("Not starting playback [playStatePermits: %d, "
                "mIsAudioPrerolling: %d, mIsVideoPrerolling: %d, "
                "mAudioOffloading: %d]",
                (int)playStatePermits, (int)mIsAudioPrerolling,
                (int)mIsVideoPrerolling, (int)mAudioOffloading);
    return;
  }

  DECODER_LOG("MaybeStartPlayback() starting playback");
  mOnPlaybackEvent.Notify(MediaEventType::PlaybackStarted);
  StartMediaSink();

  if (!IsPlaying()) {
    mMediaSink->SetPlaying(true);
    MOZ_ASSERT(IsPlaying());
  }

  DispatchDecodeTasksIfNeeded();
}

void
MediaDecoderStateMachine::MaybeStartBuffering()
{
  MOZ_ASSERT(OnTaskQueue());
  // Buffering makes senses only after decoding first frames.
  MOZ_ASSERT(mSentFirstFrameLoadedEvent);
  MOZ_ASSERT(mState == DECODER_STATE_DECODING);

  // Don't enter buffering when MediaDecoder is not playing.
  if (mPlayState != MediaDecoder::PLAY_STATE_PLAYING) {
    return;
  }

  // Don't enter buffering while prerolling so that the decoder has a chance to
  // enqueue some decoded data before we give up and start buffering.
  if (!IsPlaying()) {
    return;
  }

  // No more data to download. No need to enter buffering.
  if (!mResource->IsExpectingMoreData()) {
    return;
  }

  bool shouldBuffer;
  if (mReader->UseBufferingHeuristics()) {
    shouldBuffer = HasLowDecodedData() && HasLowBufferedData();
  } else {
    MOZ_ASSERT(mReader->IsWaitForDataSupported());
    shouldBuffer = (OutOfDecodedAudio() && mReader->IsWaitingAudioData()) ||
                   (OutOfDecodedVideo() && mReader->IsWaitingVideoData());
  }
  if (shouldBuffer) {
    SetState(DECODER_STATE_BUFFERING);
  }
}

void MediaDecoderStateMachine::UpdatePlaybackPositionInternal(int64_t aTime)
{
  MOZ_ASSERT(OnTaskQueue());
  SAMPLE_LOG("UpdatePlaybackPositionInternal(%lld)", aTime);

  mCurrentPosition = aTime;
  NS_ASSERTION(mCurrentPosition >= 0, "CurrentTime should be positive!");
  mObservedDuration = std::max(mObservedDuration.Ref(),
                               TimeUnit::FromMicroseconds(mCurrentPosition.Ref()));
}

void MediaDecoderStateMachine::UpdatePlaybackPosition(int64_t aTime)
{
  MOZ_ASSERT(OnTaskQueue());
  UpdatePlaybackPositionInternal(aTime);

  bool fragmentEnded = mFragmentEndTime >= 0 && GetMediaTime() >= mFragmentEndTime;
  mMetadataManager.DispatchMetadataIfNeeded(TimeUnit::FromMicroseconds(aTime));

  if (fragmentEnded) {
    StopPlayback();
  }
}

/* static */ const char*
MediaDecoderStateMachine::ToStateStr(State aState)
{
  switch (aState) {
    case DECODER_STATE_DECODING_METADATA:   return "DECODING_METADATA";
    case DECODER_STATE_WAIT_FOR_CDM:        return "WAIT_FOR_CDM";
    case DECODER_STATE_DORMANT:             return "DORMANT";
    case DECODER_STATE_DECODING_FIRSTFRAME: return "DECODING_FIRSTFRAME";
    case DECODER_STATE_DECODING:            return "DECODING";
    case DECODER_STATE_SEEKING:             return "SEEKING";
    case DECODER_STATE_BUFFERING:           return "BUFFERING";
    case DECODER_STATE_COMPLETED:           return "COMPLETED";
    case DECODER_STATE_SHUTDOWN:            return "SHUTDOWN";
    default: MOZ_ASSERT_UNREACHABLE("Invalid state.");
  }
  return "UNKNOWN";
}

const char*
MediaDecoderStateMachine::ToStateStr()
{
  MOZ_ASSERT(OnTaskQueue());
  return ToStateStr(mState);
}

void
MediaDecoderStateMachine::SetState(State aState)
{
  MOZ_ASSERT(OnTaskQueue());
  if (mState == aState) {
    return;
  }

  DECODER_LOG("MDSM state: %s -> %s", ToStateStr(), ToStateStr(aState));

  MOZ_ASSERT(mState == mStateObj->GetState());
  mStateObj->Exit();
  mState = aState;

  switch (mState) {
    case DECODER_STATE_DECODING_METADATA:
      mStateObj = MakeUnique<DecodeMetadataState>(this);
      break;
    case DECODER_STATE_WAIT_FOR_CDM:
      mStateObj = MakeUnique<WaitForCDMState>(this);
      break;
    case DECODER_STATE_DORMANT:
      mStateObj = MakeUnique<DormantState>(this);
      break;
    case DECODER_STATE_DECODING_FIRSTFRAME:
      mStateObj = MakeUnique<DecodingFirstFrameState>(this);
      break;
    case DECODER_STATE_DECODING:
      mStateObj = MakeUnique<DecodingState>(this);
      break;
    case DECODER_STATE_SEEKING:
      mStateObj = MakeUnique<SeekingState>(this);
      break;
    case DECODER_STATE_BUFFERING:
      mStateObj = MakeUnique<BufferingState>(this);
      break;
    case DECODER_STATE_COMPLETED:
      mStateObj = MakeUnique<CompletedState>(this);
      break;
    case DECODER_STATE_SHUTDOWN:
      mStateObj = MakeUnique<ShutdownState>(this);
      break;
    default:
      MOZ_ASSERT_UNREACHABLE("Invalid state.");
      break;
  }

  MOZ_ASSERT(mState == mStateObj->GetState());
  mStateObj->Enter();
}

void MediaDecoderStateMachine::VolumeChanged()
{
  MOZ_ASSERT(OnTaskQueue());
  mMediaSink->SetVolume(mVolume);
}

void MediaDecoderStateMachine::RecomputeDuration()
{
  MOZ_ASSERT(OnTaskQueue());

  TimeUnit duration;
  if (mExplicitDuration.Ref().isSome()) {
    double d = mExplicitDuration.Ref().ref();
    if (IsNaN(d)) {
      // We have an explicit duration (which means that we shouldn't look at
      // any other duration sources), but the duration isn't ready yet.
      return;
    }
    // We don't fire duration changed for this case because it should have
    // already been fired on the main thread when the explicit duration was set.
    duration = TimeUnit::FromSeconds(d);
  } else if (mEstimatedDuration.Ref().isSome()) {
    duration = mEstimatedDuration.Ref().ref();
  } else if (mInfo.mMetadataDuration.isSome()) {
    duration = mInfo.mMetadataDuration.ref();
  } else {
    return;
  }

  // Only adjust the duration when an explicit duration isn't set (MSE).
  // The duration is always exactly known with MSE and there's no need to adjust
  // it based on what may have been seen in the past; in particular as this data
  // may no longer exist such as when the mediasource duration was reduced.
  if (mExplicitDuration.Ref().isNothing() &&
      duration < mObservedDuration.Ref()) {
    duration = mObservedDuration;
  }

  MOZ_ASSERT(duration.ToMicroseconds() >= 0);
  mDuration = Some(duration);
}

void
MediaDecoderStateMachine::DispatchSetDormant(bool aDormant)
{
  nsCOMPtr<nsIRunnable> r = NewRunnableMethod<bool>(
    this, &MediaDecoderStateMachine::SetDormant, aDormant);
  OwnerThread()->Dispatch(r.forget());
}

void
MediaDecoderStateMachine::SetDormant(bool aDormant)
{
  MOZ_ASSERT(OnTaskQueue());
  mStateObj->HandleDormant(aDormant);
}

RefPtr<ShutdownPromise>
MediaDecoderStateMachine::Shutdown()
{
  MOZ_ASSERT(OnTaskQueue());

  SetState(DECODER_STATE_SHUTDOWN);

  mDelayedScheduler.Reset();

  mBufferedUpdateRequest.DisconnectIfExists();

  mQueuedSeek.RejectIfExists(__func__);

  DiscardSeekTaskIfExist();

  // Shutdown happens will decode timer is active, we need to disconnect and
  // dispose of the timer.
  mVideoDecodeSuspendTimer.Reset();

  mCDMProxyPromise.DisconnectIfExists();

  if (IsPlaying()) {
    StopPlayback();
  }

  // To break the cycle-reference between MediaDecoderReaderWrapper and MDSM.
  CancelMediaDecoderReaderWrapperCallback();

  Reset();

  mMediaSink->Shutdown();

  // Prevent dangling pointers by disconnecting the listeners.
  mAudioQueueListener.Disconnect();
  mVideoQueueListener.Disconnect();
  mMetadataManager.Disconnect();

  // Disconnect canonicals and mirrors before shutting down our task queue.
  mBuffered.DisconnectIfConnected();
  mIsReaderSuspended.DisconnectIfConnected();
  mEstimatedDuration.DisconnectIfConnected();
  mExplicitDuration.DisconnectIfConnected();
  mPlayState.DisconnectIfConnected();
  mNextPlayState.DisconnectIfConnected();
  mVolume.DisconnectIfConnected();
  mPreservesPitch.DisconnectIfConnected();
  mSameOriginMedia.DisconnectIfConnected();
  mMediaPrincipalHandle.DisconnectIfConnected();
  mPlaybackBytesPerSecond.DisconnectIfConnected();
  mPlaybackRateReliable.DisconnectIfConnected();
  mDecoderPosition.DisconnectIfConnected();
  mMediaSeekable.DisconnectIfConnected();
  mMediaSeekableOnlyInBufferedRanges.DisconnectIfConnected();
  mIsVisible.DisconnectIfConnected();

  mDuration.DisconnectAll();
  mIsShutdown.DisconnectAll();
  mNextFrameStatus.DisconnectAll();
  mCurrentPosition.DisconnectAll();
  mPlaybackOffset.DisconnectAll();
  mIsAudioDataAudible.DisconnectAll();

  // Shut down the watch manager to stop further notifications.
  mWatchManager.Shutdown();

  DECODER_LOG("Shutdown started");

  // Put a task in the decode queue to shutdown the reader.
  // the queue to spin down.
  return mReader->Shutdown()
    ->Then(OwnerThread(), __func__, this,
           &MediaDecoderStateMachine::FinishShutdown,
           &MediaDecoderStateMachine::FinishShutdown)
    ->CompletionPromise();
}

void
MediaDecoderStateMachine::DecodeFirstFrame()
{
  MOZ_ASSERT(OnTaskQueue());
  MOZ_ASSERT(mState == DECODER_STATE_DECODING_FIRSTFRAME);

  // Handle pending seek.
  if (mQueuedSeek.Exists() &&
      (mSentFirstFrameLoadedEvent || mReader->ForceZeroStartTime())) {
    InitiateSeek(Move(mQueuedSeek));
    return;
  }

  // Transition to DECODING if we've decoded first frames.
  if (mSentFirstFrameLoadedEvent) {
    SetState(DECODER_STATE_DECODING);
    return;
  }

  // Dispatch tasks to decode first frames.
  DispatchDecodeTasksIfNeeded();
}

void MediaDecoderStateMachine::PlayStateChanged()
{
  MOZ_ASSERT(OnTaskQueue());

  if (mPlayState != MediaDecoder::PLAY_STATE_PLAYING) {
    mVideoDecodeSuspendTimer.Reset();
    return;
  }

  // Once we start playing, we don't want to minimize our prerolling, as we
  // assume the user is likely to want to keep playing in future. This needs to
  // happen before we invoke StartDecoding().
  if (mMinimizePreroll) {
    mMinimizePreroll = false;
    DispatchDecodeTasksIfNeeded();
  }

  // Some state transitions still happen synchronously on the main thread. So
  // if the main thread invokes Play() and then Seek(), the seek will initiate
  // synchronously on the main thread, and the asynchronous PlayInternal task
  // will arrive when it's no longer valid. The proper thing to do is to move
  // all state transitions to the state machine task queue, but for now we just
  // make sure that none of the possible main-thread state transitions (Seek(),
  // SetDormant(), and Shutdown()) have not occurred.
  if (mState != DECODER_STATE_DECODING &&
      mState != DECODER_STATE_DECODING_FIRSTFRAME &&
      mState != DECODER_STATE_BUFFERING &&
      mState != DECODER_STATE_COMPLETED)
  {
    DECODER_LOG("Unexpected state - Bailing out of PlayInternal()");
    return;
  }

  ScheduleStateMachine();
}

static void
ReportRecoveryTelemetry(const TimeStamp& aRecoveryStart,
                        const MediaInfo& aMediaInfo,
                        bool aIsHardwareAccelerated)
{
  MOZ_ASSERT(NS_IsMainThread());
  if (!aMediaInfo.HasVideo()) {
    return;
  }

  // Keyed by audio+video or video alone, hardware acceleration,
  // and by a resolution range.
  nsCString key(aMediaInfo.HasAudio() ? "AV" : "V");
  key.AppendASCII(aIsHardwareAccelerated ? "(hw)," : ",");
  static const struct { int32_t mH; const char* mRes; } sResolutions[] = {
    {  240, "0-240" },
    {  480, "241-480" },
    {  720, "481-720" },
    { 1080, "721-1080" },
    { 2160, "1081-2160" }
  };
  const char* resolution = "2161+";
  int32_t height = aMediaInfo.mVideo.mImage.height;
  for (const auto& res : sResolutions) {
    if (height <= res.mH) {
      resolution = res.mRes;
      break;
    }
  }
  key.AppendASCII(resolution);

  TimeDuration duration = TimeStamp::Now() - aRecoveryStart;
  double duration_ms = duration.ToMilliseconds();
  Telemetry::Accumulate(Telemetry::VIDEO_SUSPEND_RECOVERY_TIME_MS,
                        key,
                        uint32_t(duration_ms + 0.5));
  Telemetry::Accumulate(Telemetry::VIDEO_SUSPEND_RECOVERY_TIME_MS,
                        NS_LITERAL_CSTRING("All"),
                        uint32_t(duration_ms + 0.5));
}

void MediaDecoderStateMachine::VisibilityChanged()
{
  MOZ_ASSERT(OnTaskQueue());
  DECODER_LOG("VisibilityChanged: mIsVisible=%d, "
              "mVideoDecodeSuspended=%c, mIsReaderSuspended=%d",
              mIsVisible.Ref(), mVideoDecodeSuspended ? 'T' : 'F', mIsReaderSuspended.Ref());

  if (!HasVideo()) {
    return;
  }

  // If not playing then there's nothing to do.
  if (mPlayState != MediaDecoder::PLAY_STATE_PLAYING) {
    return;
  }

  // Start timer to trigger suspended decoding state when going invisible.
  if (!mIsVisible) {
    TimeStamp target = TimeStamp::Now() + SuspendBackgroundVideoDelay();

    RefPtr<MediaDecoderStateMachine> self = this;
    mVideoDecodeSuspendTimer.Ensure(target,
                                    [=]() { self->OnSuspendTimerResolved(); },
                                    [=]() { self->OnSuspendTimerRejected(); });
    return;
  }

  // Resuming from suspended decoding

  // If suspend timer exists, destroy it.
  mVideoDecodeSuspendTimer.Reset();

  if (mVideoDecodeSuspended) {
    mVideoDecodeSuspended = false;
    mOnPlaybackEvent.Notify(MediaEventType::ExitVideoSuspend);
    mReader->SetVideoBlankDecode(false);

    if (mIsReaderSuspended) {
      return;
    }

    // If an existing seek is in flight don't bother creating a new
    // one to catch up.
    if (mSeekTask || mQueuedSeek.Exists()) {
      return;
    }

    // Start counting recovery time from right now.
    TimeStamp start = TimeStamp::Now();
    // Local reference to mInfo, so that it will be copied in the lambda below.
    MediaInfo& info = mInfo;
    bool hw = mReader->VideoIsHardwareAccelerated();

    // Start video-only seek to the current time.
    SeekJob seekJob;

    const SeekTarget::Type type = HasAudio()
                                  ? SeekTarget::Type::Accurate
                                  : SeekTarget::Type::PrevSyncPoint;

    seekJob.mTarget = SeekTarget(GetMediaTime(),
                                 type,
                                 MediaDecoderEventVisibility::Suppressed,
                                 true /* aVideoOnly */);

    InitiateSeek(Move(seekJob))
      ->Then(AbstractThread::MainThread(), __func__,
             [start, info, hw](){ ReportRecoveryTelemetry(start, info, hw); },
             [](){});
  }
}

void MediaDecoderStateMachine::BufferedRangeUpdated()
{
  MOZ_ASSERT(OnTaskQueue());

  // While playing an unseekable stream of unknown duration, mObservedDuration
  // is updated (in AdvanceFrame()) as we play. But if data is being downloaded
  // faster than played, mObserved won't reflect the end of playable data
  // since we haven't played the frame at the end of buffered data. So update
  // mObservedDuration here as new data is downloaded to prevent such a lag.
  if (!mBuffered.Ref().IsInvalid()) {
    bool exists;
    media::TimeUnit end{mBuffered.Ref().GetEnd(&exists)};
    if (exists) {
      mObservedDuration = std::max(mObservedDuration.Ref(), end);
    }
  }
}

void MediaDecoderStateMachine::ReaderSuspendedChanged()
{
  MOZ_ASSERT(OnTaskQueue());
  DECODER_LOG("ReaderSuspendedChanged: %d", mIsReaderSuspended.Ref());
  SetDormant(mIsReaderSuspended);
}

RefPtr<MediaDecoder::SeekPromise>
MediaDecoderStateMachine::Seek(SeekTarget aTarget)
{
  MOZ_ASSERT(OnTaskQueue());

  if (IsShutdown()) {
    return MediaDecoder::SeekPromise::CreateAndReject(/* aIgnored = */ true, __func__);
  }

  // We need to be able to seek in some way
  if (!mMediaSeekable && !mMediaSeekableOnlyInBufferedRanges) {
    DECODER_WARN("Seek() function should not be called on a non-seekable state machine");
    return MediaDecoder::SeekPromise::CreateAndReject(/* aIgnored = */ true, __func__);
  }

  if (aTarget.IsNextFrame() && !HasVideo()) {
    DECODER_WARN("Ignore a NextFrameSeekTask on a media file without video track.");
    return MediaDecoder::SeekPromise::CreateAndReject(/* aIgnored = */ true, __func__);
  }

  MOZ_ASSERT(mDuration.Ref().isSome(), "We should have got duration already");

  // Can't seek until the start time is known.
  bool hasStartTime = mSentFirstFrameLoadedEvent || mReader->ForceZeroStartTime();
  // Can't seek when state is WAIT_FOR_CDM or DORMANT.
  bool stateAllowed = mState >= DECODER_STATE_DECODING_FIRSTFRAME;

  if (!stateAllowed || !hasStartTime) {
    DECODER_LOG("Seek() Not Enough Data to continue at this stage, queuing seek");
    mQueuedSeek.RejectIfExists(__func__);
    mQueuedSeek.mTarget = aTarget;
    return mQueuedSeek.mPromise.Ensure(__func__);
  }
  mQueuedSeek.RejectIfExists(__func__);

  DECODER_LOG("Changed state to SEEKING (to %lld)", aTarget.GetTime().ToMicroseconds());

  SeekJob seekJob;
  seekJob.mTarget = aTarget;
  return InitiateSeek(Move(seekJob));
}

RefPtr<MediaDecoder::SeekPromise>
MediaDecoderStateMachine::InvokeSeek(SeekTarget aTarget)
{
  return InvokeAsync(OwnerThread(), this, __func__,
                     &MediaDecoderStateMachine::Seek, aTarget);
}

void MediaDecoderStateMachine::StopMediaSink()
{
  MOZ_ASSERT(OnTaskQueue());
  if (mMediaSink->IsStarted()) {
    DECODER_LOG("Stop MediaSink");
    mAudibleListener.DisconnectIfExists();

    mMediaSink->Stop();
    mMediaSinkAudioPromise.DisconnectIfExists();
    mMediaSinkVideoPromise.DisconnectIfExists();
  }
}

void
MediaDecoderStateMachine::DispatchDecodeTasksIfNeeded()
{
  MOZ_ASSERT(OnTaskQueue());

  if (mState != DECODER_STATE_DECODING &&
      mState != DECODER_STATE_DECODING_FIRSTFRAME &&
      mState != DECODER_STATE_BUFFERING &&
      mState != DECODER_STATE_SEEKING) {
    return;
  }

  // NeedToDecodeAudio() can go from false to true while we hold the
  // monitor, but it can't go from true to false. This can happen because
  // NeedToDecodeAudio() takes into account the amount of decoded audio
  // that's been written to the AudioStream but not played yet. So if we
  // were calling NeedToDecodeAudio() twice and we thread-context switch
  // between the calls, audio can play, which can affect the return value
  // of NeedToDecodeAudio() giving inconsistent results. So we cache the
  // value returned by NeedToDecodeAudio(), and make decisions
  // based on the cached value. If NeedToDecodeAudio() has
  // returned false, and then subsequently returns true and we're not
  // playing, it will probably be OK since we don't need to consume data
  // anyway.

  const bool needToDecodeAudio = NeedToDecodeAudio();
  const bool needToDecodeVideo = NeedToDecodeVideo();

  // If we're in completed state, we should not need to decode anything else.
  MOZ_ASSERT(mState != DECODER_STATE_COMPLETED ||
             (!needToDecodeAudio && !needToDecodeVideo));

  bool needIdle = !IsLogicallyPlaying() &&
                  mState != DECODER_STATE_SEEKING &&
                  !needToDecodeAudio &&
                  !needToDecodeVideo &&
                  !IsPlaying();

  SAMPLE_LOG("DispatchDecodeTasksIfNeeded needAudio=%d audioStatus=%s needVideo=%d videoStatus=%s needIdle=%d",
             needToDecodeAudio, AudioRequestStatus(),
             needToDecodeVideo, VideoRequestStatus(),
             needIdle);

  if (needToDecodeAudio) {
    EnsureAudioDecodeTaskQueued();
  }
  if (needToDecodeVideo) {
    EnsureVideoDecodeTaskQueued();
  }

  if (needIdle) {
    DECODER_LOG("Dispatching SetIdle() audioQueue=%lld videoQueue=%lld",
                GetDecodedAudioDuration(),
                VideoQueue().Duration());
    mReader->SetIdle();
  }
}

RefPtr<MediaDecoder::SeekPromise>
MediaDecoderStateMachine::InitiateSeek(SeekJob aSeekJob)
{
  MOZ_ASSERT(OnTaskQueue());

  SetState(DECODER_STATE_SEEKING);

  // Discard the existing seek task.
  DiscardSeekTaskIfExist();

  mSeekTaskRequest.DisconnectIfExists();

  // SeekTask will register its callbacks to MediaDecoderReaderWrapper.
  CancelMediaDecoderReaderWrapperCallback();

  // Create a new SeekTask instance for the incoming seek task.
  if (aSeekJob.mTarget.IsAccurate() ||
      aSeekJob.mTarget.IsFast()) {
    mSeekTask = new AccurateSeekTask(mDecoderID, OwnerThread(),
                                     mReader.get(), aSeekJob.mTarget,
                                     mInfo, Duration(), GetMediaTime());
  } else if (aSeekJob.mTarget.IsNextFrame()) {
    mSeekTask = new NextFrameSeekTask(mDecoderID, OwnerThread(), mReader.get(),
                                      aSeekJob.mTarget, mInfo, Duration(),
                                      GetMediaTime(), AudioQueue(), VideoQueue());
  } else {
    MOZ_DIAGNOSTIC_ASSERT(false, "Cannot handle this seek task.");
  }

  // Don't stop playback for a video-only seek since audio is playing.
  if (!aSeekJob.mTarget.IsVideoOnly()) {
    StopPlayback();
  }

  // aSeekJob.mTarget.mTime might be different from
  // mSeekTask->GetSeekTarget().mTime because the seek task might clamp the seek
  // target to [0, duration]. We want to update the playback position to the
  // clamped value.
  UpdatePlaybackPositionInternal(mSeekTask->GetSeekTarget().GetTime().ToMicroseconds());

  if (aSeekJob.mTarget.mEventVisibility == MediaDecoderEventVisibility::Observable) {
    mOnPlaybackEvent.Notify(MediaEventType::SeekStarted);
  }

  // Reset our state machine and decoding pipeline before seeking.
  if (mSeekTask->NeedToResetMDSM()) {
    if (aSeekJob.mTarget.IsVideoOnly()) {
      Reset(TrackInfo::kVideoTrack);
    } else {
      Reset();
    }
  }

  // Do the seek.
  mSeekTaskRequest.Begin(mSeekTask->Seek(Duration())
    ->Then(OwnerThread(), __func__, this,
           &MediaDecoderStateMachine::OnSeekTaskResolved,
           &MediaDecoderStateMachine::OnSeekTaskRejected));

  MOZ_ASSERT(!mQueuedSeek.Exists());
  MOZ_ASSERT(!mCurrentSeek.Exists());
  mCurrentSeek = Move(aSeekJob);
  return mCurrentSeek.mPromise.Ensure(__func__);
}

void
MediaDecoderStateMachine::OnSeekTaskResolved(SeekTaskResolveValue aValue)
{
  MOZ_ASSERT(OnTaskQueue());
  MOZ_ASSERT(mState == DECODER_STATE_SEEKING);

  mSeekTaskRequest.Complete();

  if (aValue.mSeekedAudioData) {
    Push(aValue.mSeekedAudioData.get(), MediaData::AUDIO_DATA);
    mDecodedAudioEndTime =
      std::max(aValue.mSeekedAudioData->GetEndTime(), mDecodedAudioEndTime);
  }

  if (aValue.mSeekedVideoData) {
    Push(aValue.mSeekedVideoData.get(), MediaData::VIDEO_DATA);
    mDecodedVideoEndTime =
      std::max(aValue.mSeekedVideoData->GetEndTime(), mDecodedVideoEndTime);
  }

  if (aValue.mIsAudioQueueFinished) {
    AudioQueue().Finish();
    StopPrerollingAudio();
  }

  if (aValue.mIsVideoQueueFinished) {
    VideoQueue().Finish();
    StopPrerollingVideo();
  }

  SeekCompleted();
}

void
MediaDecoderStateMachine::OnSeekTaskRejected(SeekTaskRejectValue aValue)
{
  MOZ_ASSERT(OnTaskQueue());
  MOZ_ASSERT(mState == DECODER_STATE_SEEKING);

  mSeekTaskRequest.Complete();

  if (aValue.mIsAudioQueueFinished) {
    AudioQueue().Finish();
    StopPrerollingAudio();
  }

  if (aValue.mIsVideoQueueFinished) {
    VideoQueue().Finish();
    StopPrerollingVideo();
  }

  DecodeError(aValue.mError);

  DiscardSeekTaskIfExist();
}

void
MediaDecoderStateMachine::DiscardSeekTaskIfExist()
{
  if (mSeekTask) {
    mCurrentSeek.RejectIfExists(__func__);
    mSeekTask->Discard();
    mSeekTask = nullptr;

    // Reset the MediaDecoderReaderWrapper's callbask.
    SetMediaDecoderReaderWrapperCallback();
  }
}

void
MediaDecoderStateMachine::DispatchAudioDecodeTaskIfNeeded()
{
  MOZ_ASSERT(OnTaskQueue());
  if (!IsShutdown() && NeedToDecodeAudio()) {
    EnsureAudioDecodeTaskQueued();
  }
}

void
MediaDecoderStateMachine::EnsureAudioDecodeTaskQueued()
{
  MOZ_ASSERT(OnTaskQueue());
  MOZ_ASSERT(mState != DECODER_STATE_SEEKING);

  SAMPLE_LOG("EnsureAudioDecodeTaskQueued isDecoding=%d status=%s",
              IsAudioDecoding(), AudioRequestStatus());

  if (mState != DECODER_STATE_DECODING &&
      mState != DECODER_STATE_DECODING_FIRSTFRAME &&
      mState != DECODER_STATE_BUFFERING) {
    return;
  }

  if (!IsAudioDecoding() ||
      mReader->IsRequestingAudioData() ||
      mReader->IsWaitingAudioData()) {
    return;
  }

  RequestAudioData();
}

void
MediaDecoderStateMachine::RequestAudioData()
{
  MOZ_ASSERT(OnTaskQueue());
  MOZ_ASSERT(mState != DECODER_STATE_SEEKING);

  SAMPLE_LOG("Queueing audio task - queued=%i, decoder-queued=%o",
             AudioQueue().GetSize(), mReader->SizeOfAudioQueueInFrames());

  mReader->RequestAudioData();
}

void
MediaDecoderStateMachine::DispatchVideoDecodeTaskIfNeeded()
{
  MOZ_ASSERT(OnTaskQueue());
  if (!IsShutdown() && NeedToDecodeVideo()) {
    EnsureVideoDecodeTaskQueued();
  }
}

void
MediaDecoderStateMachine::EnsureVideoDecodeTaskQueued()
{
  MOZ_ASSERT(OnTaskQueue());
  MOZ_ASSERT(mState != DECODER_STATE_SEEKING);

  SAMPLE_LOG("EnsureVideoDecodeTaskQueued isDecoding=%d status=%s",
             IsVideoDecoding(), VideoRequestStatus());

  if (mState != DECODER_STATE_DECODING &&
      mState != DECODER_STATE_DECODING_FIRSTFRAME &&
      mState != DECODER_STATE_BUFFERING) {
    return;
  }

  if (!IsVideoDecoding() ||
      mReader->IsRequestingVideoData() ||
      mReader->IsWaitingVideoData()) {
    return;
  }

  RequestVideoData();
}

void
MediaDecoderStateMachine::RequestVideoData()
{
  MOZ_ASSERT(OnTaskQueue());
  MOZ_ASSERT(mState != DECODER_STATE_SEEKING);

  bool skipToNextKeyFrame = NeedToSkipToNextKeyframe();

  media::TimeUnit currentTime = media::TimeUnit::FromMicroseconds(GetMediaTime());

  SAMPLE_LOG("Queueing video task - queued=%i, decoder-queued=%o, skip=%i, time=%lld",
             VideoQueue().GetSize(), mReader->SizeOfVideoQueueInFrames(), skipToNextKeyFrame,
             currentTime.ToMicroseconds());

  // MediaDecoderReaderWrapper::RequestVideoData() records the decoding start
  // time and sent it back to MDSM::OnVideoDecoded() so that if the decoding is
  // slow, we can increase our low audio threshold to reduce the chance of an
  // audio underrun while we're waiting for a video decode to complete.
  mReader->RequestVideoData(skipToNextKeyFrame, currentTime);
}

void
MediaDecoderStateMachine::StartMediaSink()
{
  MOZ_ASSERT(OnTaskQueue());
  if (!mMediaSink->IsStarted()) {
    mAudioCompleted = false;
    mMediaSink->Start(GetMediaTime(), mInfo);

    auto videoPromise = mMediaSink->OnEnded(TrackInfo::kVideoTrack);
    auto audioPromise = mMediaSink->OnEnded(TrackInfo::kAudioTrack);

    if (audioPromise) {
      mMediaSinkAudioPromise.Begin(audioPromise->Then(
        OwnerThread(), __func__, this,
        &MediaDecoderStateMachine::OnMediaSinkAudioComplete,
        &MediaDecoderStateMachine::OnMediaSinkAudioError));
    }
    if (videoPromise) {
      mMediaSinkVideoPromise.Begin(videoPromise->Then(
        OwnerThread(), __func__, this,
        &MediaDecoderStateMachine::OnMediaSinkVideoComplete,
        &MediaDecoderStateMachine::OnMediaSinkVideoError));
    }
  }
}

bool
MediaDecoderStateMachine::HasLowDecodedAudio()
{
  MOZ_ASSERT(OnTaskQueue());
  return IsAudioDecoding() &&
         GetDecodedAudioDuration() < EXHAUSTED_DATA_MARGIN_USECS * mPlaybackRate;
}

bool
MediaDecoderStateMachine::HasLowDecodedVideo()
{
  MOZ_ASSERT(OnTaskQueue());
  return IsVideoDecoding() &&
         VideoQueue().GetSize() < LOW_VIDEO_FRAMES * mPlaybackRate;
}

bool
MediaDecoderStateMachine::HasLowDecodedData()
{
  MOZ_ASSERT(OnTaskQueue());
  MOZ_ASSERT(mReader->UseBufferingHeuristics());
  return HasLowDecodedAudio() || HasLowDecodedVideo();
}

bool MediaDecoderStateMachine::OutOfDecodedAudio()
{
    MOZ_ASSERT(OnTaskQueue());
    return IsAudioDecoding() && !AudioQueue().IsFinished() &&
           AudioQueue().GetSize() == 0 &&
           !mMediaSink->HasUnplayedFrames(TrackInfo::kAudioTrack);
}

bool MediaDecoderStateMachine::HasLowBufferedData()
{
  MOZ_ASSERT(OnTaskQueue());
  return HasLowBufferedData(mLowDataThresholdUsecs);
}

bool MediaDecoderStateMachine::HasLowBufferedData(int64_t aUsecs)
{
  MOZ_ASSERT(OnTaskQueue());
  MOZ_ASSERT(mState >= DECODER_STATE_DECODING,
             "Must have loaded first frame for mBuffered to be valid");

  // If we don't have a duration, mBuffered is probably not going to have
  // a useful buffered range. Return false here so that we don't get stuck in
  // buffering mode for live streams.
  if (Duration().IsInfinite()) {
    return false;
  }

  if (mBuffered.Ref().IsInvalid()) {
    return false;
  }

  // We are never low in decoded data when we don't have audio/video or have
  // decoded all audio/video samples.
  int64_t endOfDecodedVideoData =
    (HasVideo() && !VideoQueue().IsFinished())
      ? mDecodedVideoEndTime
      : INT64_MAX;
  int64_t endOfDecodedAudioData =
    (HasAudio() && !AudioQueue().IsFinished())
      ? mDecodedAudioEndTime
      : INT64_MAX;

  int64_t endOfDecodedData = std::min(endOfDecodedVideoData, endOfDecodedAudioData);
  if (Duration().ToMicroseconds() < endOfDecodedData) {
    // Our duration is not up to date. No point buffering.
    return false;
  }

  if (endOfDecodedData == INT64_MAX) {
    // Have decoded all samples. No point buffering.
    return false;
  }

  int64_t start = endOfDecodedData;
  int64_t end = std::min(GetMediaTime() + aUsecs, Duration().ToMicroseconds());
  if (start >= end) {
    // Duration of decoded samples is greater than our threshold.
    return false;
  }
  media::TimeInterval interval(media::TimeUnit::FromMicroseconds(start),
                               media::TimeUnit::FromMicroseconds(end));
  return !mBuffered.Ref().Contains(interval);
}

void
MediaDecoderStateMachine::DecodeError(const MediaResult& aError)
{
  MOZ_ASSERT(OnTaskQueue());
  MOZ_ASSERT(!IsShutdown());
  DECODER_WARN("Decode error");
  // Notify the decode error and MediaDecoder will shut down MDSM.
  mOnPlaybackErrorEvent.Notify(aError);
}

void
MediaDecoderStateMachine::EnqueueLoadedMetadataEvent()
{
  MOZ_ASSERT(OnTaskQueue());
  MediaDecoderEventVisibility visibility =
    mSentLoadedMetadataEvent ? MediaDecoderEventVisibility::Suppressed
                             : MediaDecoderEventVisibility::Observable;
  mMetadataLoadedEvent.Notify(nsAutoPtr<MediaInfo>(new MediaInfo(mInfo)),
                              Move(mMetadataTags),
                              visibility);
  mSentLoadedMetadataEvent = true;
}

void
MediaDecoderStateMachine::EnqueueFirstFrameLoadedEvent()
{
  MOZ_ASSERT(OnTaskQueue());
  // Track value of mSentFirstFrameLoadedEvent from before updating it
  bool firstFrameBeenLoaded = mSentFirstFrameLoadedEvent;
  mSentFirstFrameLoadedEvent = true;
  RefPtr<MediaDecoderStateMachine> self = this;
  mBufferedUpdateRequest.Begin(
    mReader->UpdateBufferedWithPromise()
    ->Then(OwnerThread(),
    __func__,
    // Resolve
    [self, firstFrameBeenLoaded]() {
      self->mBufferedUpdateRequest.Complete();
      MediaDecoderEventVisibility visibility =
        firstFrameBeenLoaded ? MediaDecoderEventVisibility::Suppressed
                             : MediaDecoderEventVisibility::Observable;
      self->mFirstFrameLoadedEvent.Notify(
        nsAutoPtr<MediaInfo>(new MediaInfo(self->mInfo)), visibility);
    },
    // Reject
    []() { MOZ_CRASH("Should not reach"); }));
}

void
MediaDecoderStateMachine::FinishDecodeFirstFrame()
{
  MOZ_ASSERT(OnTaskQueue());
  MOZ_ASSERT(!mSentFirstFrameLoadedEvent);
  DECODER_LOG("FinishDecodeFirstFrame");

  mMediaSink->Redraw(mInfo.mVideo);

  // If we don't know the duration by this point, we assume infinity, per spec.
  if (mDuration.Ref().isNothing()) {
    mDuration = Some(TimeUnit::FromInfinity());
  }

  DECODER_LOG("Media duration %lld, "
              "transportSeekable=%d, mediaSeekable=%d",
              Duration().ToMicroseconds(), mResource->IsTransportSeekable(), mMediaSeekable.Ref());

  // Get potentially updated metadata
  mReader->ReadUpdatedMetadata(&mInfo);

  if (!mNotifyMetadataBeforeFirstFrame) {
    // If we didn't have duration and/or start time before, we should now.
    EnqueueLoadedMetadataEvent();
  }

  EnqueueFirstFrameLoadedEvent();
}

void
MediaDecoderStateMachine::SeekCompleted()
{
  MOZ_ASSERT(OnTaskQueue());
  MOZ_ASSERT(mState == DECODER_STATE_SEEKING);

  int64_t seekTime = mSeekTask->GetSeekTarget().GetTime().ToMicroseconds();
  int64_t newCurrentTime = seekTime;

  // Setup timestamp state.
  RefPtr<MediaData> video = VideoQueue().PeekFront();
  if (seekTime == Duration().ToMicroseconds()) {
    newCurrentTime = seekTime;
  } else if (HasAudio()) {
    RefPtr<MediaData> audio = AudioQueue().PeekFront();
    // Though we adjust the newCurrentTime in audio-based, and supplemented
    // by video. For better UX, should NOT bind the slide position to
    // the first audio data timestamp directly.
    // While seeking to a position where there's only either audio or video, or
    // seeking to a position lies before audio or video, we need to check if
    // seekTime is bounded in suitable duration. See Bug 1112438.
    int64_t audioStart = audio ? audio->mTime : seekTime;
    // We only pin the seek time to the video start time if the video frame
    // contains the seek time.
    if (video && video->mTime <= seekTime && video->GetEndTime() > seekTime) {
      newCurrentTime = std::min(audioStart, video->mTime);
    } else {
      newCurrentTime = audioStart;
    }
  } else {
    newCurrentTime = video ? video->mTime : seekTime;
  }

  // Change state to DECODING or COMPLETED now.
  bool isLiveStream = mResource->IsLiveStream();
  State nextState;
  if (newCurrentTime == Duration().ToMicroseconds() && !isLiveStream) {
    // Seeked to end of media, move to COMPLETED state. Note we don't do
    // this when playing a live stream, since the end of media will advance
    // once we download more data!
    DECODER_LOG("Changed state from SEEKING (to %lld) to COMPLETED", seekTime);
    // Explicitly set our state so we don't decode further, and so
    // we report playback ended to the media element.
    nextState = DECODER_STATE_COMPLETED;
  } else {
    DECODER_LOG("Changed state from SEEKING (to %lld) to DECODING", seekTime);
    nextState = DECODER_STATE_DECODING;
  }

  // We want to resolve the seek request prior finishing the first frame
  // to ensure that the seeked event is fired prior loadeded.
  mCurrentSeek.Resolve(nextState == DECODER_STATE_COMPLETED, __func__);

  // Discard and nullify the seek task.
  // Reset the MediaDecoderReaderWrapper's callbask.
  DiscardSeekTaskIfExist();

  // NOTE: Discarding the mSeekTask must be done before here. The following code
  // might ask the MediaDecoderReaderWrapper to request media data, however, the
  // SeekTask::Discard() will ask MediaDecoderReaderWrapper to discard media
  // data requests.

  // Notify FirstFrameLoaded now if we haven't since we've decoded some data
  // for readyState to transition to HAVE_CURRENT_DATA and fire 'loadeddata'.
  if (!mSentFirstFrameLoadedEvent) {
    // Only MSE can start seeking before finishing decoding first frames.
    MOZ_ASSERT(mReader->ForceZeroStartTime());
    FinishDecodeFirstFrame();
  }

  // Ensure timestamps are up to date.
  UpdatePlaybackPositionInternal(newCurrentTime);

  // Try to decode another frame to detect if we're at the end...
  DECODER_LOG("Seek completed, mCurrentPosition=%lld", mCurrentPosition.Ref());

  if (video) {
    mMediaSink->Redraw(mInfo.mVideo);
    mOnPlaybackEvent.Notify(MediaEventType::Invalidate);
  }

  SetState(nextState);
  MOZ_ASSERT(IsStateMachineScheduled());
}

RefPtr<ShutdownPromise>
MediaDecoderStateMachine::BeginShutdown()
{
  return InvokeAsync(OwnerThread(), this, __func__,
                     &MediaDecoderStateMachine::Shutdown);
}

RefPtr<ShutdownPromise>
MediaDecoderStateMachine::FinishShutdown()
{
  MOZ_ASSERT(OnTaskQueue());
  MOZ_ASSERT(mState == DECODER_STATE_SHUTDOWN,
             "How did we escape from the shutdown state?");
  DECODER_LOG("Shutting down state machine task queue");
  return OwnerThread()->BeginShutdown();
}

void
MediaDecoderStateMachine::RunStateMachine()
{
  MOZ_ASSERT(OnTaskQueue());

  mDelayedScheduler.Reset(); // Must happen on state machine task queue.
  mDispatchedStateMachine = false;
  mStateObj->Step();
}

void
MediaDecoderStateMachine::Reset(TrackSet aTracks)
{
  MOZ_ASSERT(OnTaskQueue());
  DECODER_LOG("MediaDecoderStateMachine::Reset");

  // We should be resetting because we're seeking, shutting down, or entering
  // dormant state. We could also be in the process of going dormant, and have
  // just switched to exiting dormant before we finished entering dormant,
  // hence the DECODING_NONE case below.
  MOZ_ASSERT(IsShutdown() ||
             mState == DECODER_STATE_SEEKING ||
             mState == DECODER_STATE_DORMANT);

  // Assert that aTracks specifies to reset the video track because we
  // don't currently support resetting just the audio track.
  MOZ_ASSERT(aTracks.contains(TrackInfo::kVideoTrack));

  if (aTracks.contains(TrackInfo::kAudioTrack) &&
      aTracks.contains(TrackInfo::kVideoTrack)) {
    // Stop the audio thread. Otherwise, MediaSink might be accessing AudioQueue
    // outside of the decoder monitor while we are clearing the queue and causes
    // crash for no samples to be popped.
    StopMediaSink();
  }

  if (aTracks.contains(TrackInfo::kVideoTrack)) {
    mDecodedVideoEndTime = 0;
    mVideoCompleted = false;
    VideoQueue().Reset();
  }

  if (aTracks.contains(TrackInfo::kAudioTrack)) {
    mDecodedAudioEndTime = 0;
    mAudioCompleted = false;
    AudioQueue().Reset();
  }

  mSeekTaskRequest.DisconnectIfExists();

  mPlaybackOffset = 0;

  mReader->ResetDecode(aTracks);
}

int64_t
MediaDecoderStateMachine::GetClock(TimeStamp* aTimeStamp) const
{
  MOZ_ASSERT(OnTaskQueue());
  int64_t clockTime = mMediaSink->GetPosition(aTimeStamp);
  NS_ASSERTION(GetMediaTime() <= clockTime, "Clock should go forwards.");
  return clockTime;
}

void
MediaDecoderStateMachine::UpdatePlaybackPositionPeriodically()
{
  MOZ_ASSERT(OnTaskQueue());

  if (!IsPlaying()) {
    return;
  }

  // Cap the current time to the larger of the audio and video end time.
  // This ensures that if we're running off the system clock, we don't
  // advance the clock to after the media end time.
  if (VideoEndTime() != -1 || AudioEndTime() != -1) {

    const int64_t clockTime = GetClock();
    // Skip frames up to the frame at the playback position, and figure out
    // the time remaining until it's time to display the next frame and drop
    // the current frame.
    NS_ASSERTION(clockTime >= 0, "Should have positive clock time.");

    // These will be non -1 if we've displayed a video frame, or played an audio frame.
    int64_t t = std::min(clockTime, std::max(VideoEndTime(), AudioEndTime()));
    // FIXME: Bug 1091422 - chained ogg files hit this assertion.
    //MOZ_ASSERT(t >= GetMediaTime());
    if (t > GetMediaTime()) {
      UpdatePlaybackPosition(t);
    }
  }
  // Note we have to update playback position before releasing the monitor.
  // Otherwise, MediaDecoder::AddOutputStream could kick in when we are outside
  // the monitor and get a staled value from GetCurrentTimeUs() which hits the
  // assertion in GetClock().

  int64_t delay = std::max<int64_t>(1, AUDIO_DURATION_USECS / mPlaybackRate);
  ScheduleStateMachineIn(delay);
}

void MediaDecoderStateMachine::UpdateNextFrameStatus()
{
  MOZ_ASSERT(OnTaskQueue());

  MediaDecoderOwner::NextFrameStatus status;
  const char* statusString;

  switch (mState.Ref()) {
    case DECODER_STATE_BUFFERING:
      status = MediaDecoderOwner::NEXT_FRAME_UNAVAILABLE_BUFFERING;
      statusString = "NEXT_FRAME_UNAVAILABLE_BUFFERING";
      break;
    case DECODER_STATE_SEEKING:
      status = MediaDecoderOwner::NEXT_FRAME_UNAVAILABLE_SEEKING;
      statusString = "NEXT_FRAME_UNAVAILABLE_SEEKING";
      break;
    default:
      bool b = HaveNextFrameData();
      status = b ? MediaDecoderOwner::NEXT_FRAME_AVAILABLE :
                   MediaDecoderOwner::NEXT_FRAME_UNAVAILABLE;
      statusString = b ? "NEXT_FRAME_AVAILABLE" : "NEXT_FRAME_UNAVAILABLE";
      break;
  }

  if (status != mNextFrameStatus) {
    DECODER_LOG("Changed mNextFrameStatus to %s", statusString);
    if(status == MediaDecoderOwner::NEXT_FRAME_UNAVAILABLE_BUFFERING ||
       status == MediaDecoderOwner::NEXT_FRAME_UNAVAILABLE) {
      // Ensure currentTime is up to date prior updating mNextFrameStatus so that
      // the MediaDecoderOwner fire events at correct currentTime.
      UpdatePlaybackPositionPeriodically();
    }
  }

  mNextFrameStatus = status;
}

bool
MediaDecoderStateMachine::CanPlayThrough()
{
  MOZ_ASSERT(OnTaskQueue());
  return GetStatistics().CanPlayThrough();
}

MediaStatistics
MediaDecoderStateMachine::GetStatistics()
{
  MOZ_ASSERT(OnTaskQueue());
  MediaStatistics result;
  result.mDownloadRate = mResource->GetDownloadRate(&result.mDownloadRateReliable);
  result.mDownloadPosition = mResource->GetCachedDataEnd(mDecoderPosition);
  result.mTotalBytes = mResource->GetLength();
  result.mPlaybackRate = mPlaybackBytesPerSecond;
  result.mPlaybackRateReliable = mPlaybackRateReliable;
  result.mDecoderPosition = mDecoderPosition;
  result.mPlaybackPosition = mPlaybackOffset;
  return result;
}

void
MediaDecoderStateMachine::ScheduleStateMachine()
{
  MOZ_ASSERT(OnTaskQueue());
  if (mDispatchedStateMachine) {
    return;
  }
  mDispatchedStateMachine = true;

  OwnerThread()->Dispatch(NewRunnableMethod(this, &MediaDecoderStateMachine::RunStateMachine));
}

void
MediaDecoderStateMachine::ScheduleStateMachineIn(int64_t aMicroseconds)
{
  MOZ_ASSERT(OnTaskQueue());          // mDelayedScheduler.Ensure() may Disconnect()
                                      // the promise, which must happen on the state
                                      // machine task queue.
  MOZ_ASSERT(aMicroseconds > 0);
  if (mDispatchedStateMachine) {
    return;
  }

  TimeStamp now = TimeStamp::Now();
  TimeStamp target = now + TimeDuration::FromMicroseconds(aMicroseconds);

  // It is OK to capture 'this' without causing UAF because the callback
  // always happens before shutdown.
  mDelayedScheduler.Ensure(target, [this] () {
    mDelayedScheduler.CompleteRequest();
    RunStateMachine();
  }, [] () {
    MOZ_DIAGNOSTIC_ASSERT(false);
  });
}

bool MediaDecoderStateMachine::OnTaskQueue() const
{
  return OwnerThread()->IsCurrentThreadIn();
}

bool MediaDecoderStateMachine::IsStateMachineScheduled() const
{
  MOZ_ASSERT(OnTaskQueue());
  return mDispatchedStateMachine || mDelayedScheduler.IsScheduled();
}

void
MediaDecoderStateMachine::SetPlaybackRate(double aPlaybackRate)
{
  MOZ_ASSERT(OnTaskQueue());
  MOZ_ASSERT(aPlaybackRate != 0, "Should be handled by MediaDecoder::Pause()");

  mPlaybackRate = aPlaybackRate;
  mMediaSink->SetPlaybackRate(mPlaybackRate);

  if (mIsAudioPrerolling && DonePrerollingAudio()) {
    StopPrerollingAudio();
  }
  if (mIsVideoPrerolling && DonePrerollingVideo()) {
    StopPrerollingVideo();
  }

  ScheduleStateMachine();
}

void MediaDecoderStateMachine::PreservesPitchChanged()
{
  MOZ_ASSERT(OnTaskQueue());
  mMediaSink->SetPreservesPitch(mPreservesPitch);
}

bool
MediaDecoderStateMachine::IsShutdown() const
{
  MOZ_ASSERT(OnTaskQueue());
  return mIsShutdown;
}

int64_t
MediaDecoderStateMachine::AudioEndTime() const
{
  MOZ_ASSERT(OnTaskQueue());
  if (mMediaSink->IsStarted()) {
    return mMediaSink->GetEndTime(TrackInfo::kAudioTrack);
  }
  MOZ_ASSERT(!HasAudio());
  return -1;
}

int64_t
MediaDecoderStateMachine::VideoEndTime() const
{
  MOZ_ASSERT(OnTaskQueue());
  if (mMediaSink->IsStarted()) {
    return mMediaSink->GetEndTime(TrackInfo::kVideoTrack);
  }
  return -1;
}

void
MediaDecoderStateMachine::OnMediaSinkVideoComplete()
{
  MOZ_ASSERT(OnTaskQueue());
  MOZ_ASSERT(mInfo.HasVideo());
  VERBOSE_LOG("[%s]", __func__);

  mMediaSinkVideoPromise.Complete();
  mVideoCompleted = true;
  ScheduleStateMachine();
}

void
MediaDecoderStateMachine::OnMediaSinkVideoError()
{
  MOZ_ASSERT(OnTaskQueue());
  MOZ_ASSERT(mInfo.HasVideo());
  VERBOSE_LOG("[%s]", __func__);

  mMediaSinkVideoPromise.Complete();
  mVideoCompleted = true;
  if (HasAudio()) {
    return;
  }
  DecodeError(MediaResult(NS_ERROR_DOM_MEDIA_MEDIASINK_ERR, __func__));
}

void MediaDecoderStateMachine::OnMediaSinkAudioComplete()
{
  MOZ_ASSERT(OnTaskQueue());
  MOZ_ASSERT(mInfo.HasAudio());
  VERBOSE_LOG("[%s]", __func__);

  mMediaSinkAudioPromise.Complete();
  mAudioCompleted = true;
  // To notify PlaybackEnded as soon as possible.
  ScheduleStateMachine();

  // Report OK to Decoder Doctor (to know if issue may have been resolved).
  mOnDecoderDoctorEvent.Notify(
    DecoderDoctorEvent{DecoderDoctorEvent::eAudioSinkStartup, NS_OK});
}

void MediaDecoderStateMachine::OnMediaSinkAudioError(nsresult aResult)
{
  MOZ_ASSERT(OnTaskQueue());
  MOZ_ASSERT(mInfo.HasAudio());
  VERBOSE_LOG("[%s]", __func__);

  mMediaSinkAudioPromise.Complete();
  mAudioCompleted = true;

  // Result should never be NS_OK in this *error* handler. Report to Dec-Doc.
  MOZ_ASSERT(NS_FAILED(aResult));
  mOnDecoderDoctorEvent.Notify(
    DecoderDoctorEvent{DecoderDoctorEvent::eAudioSinkStartup, aResult});

  // Make the best effort to continue playback when there is video.
  if (HasVideo()) {
    return;
  }

  // Otherwise notify media decoder/element about this error for it makes
  // no sense to play an audio-only file without sound output.
  DecodeError(MediaResult(NS_ERROR_DOM_MEDIA_MEDIASINK_ERR, __func__));
}

void
MediaDecoderStateMachine::OnCDMProxyReady(RefPtr<CDMProxy> aProxy)
{
  MOZ_ASSERT(OnTaskQueue());
  mCDMProxyPromise.Complete();
  mCDMProxy = aProxy;
  mReader->SetCDMProxy(aProxy);
  mStateObj->HandleCDMProxyReady();
}

void
MediaDecoderStateMachine::OnCDMProxyNotReady()
{
  MOZ_ASSERT(OnTaskQueue());
  mCDMProxyPromise.Complete();
}

void
MediaDecoderStateMachine::SetAudioCaptured(bool aCaptured)
{
  MOZ_ASSERT(OnTaskQueue());

  if (aCaptured == mAudioCaptured) {
    return;
  }

  // Rest these flags so they are consistent with the status of the sink.
  // TODO: Move these flags into MediaSink to improve cohesion so we don't need
  // to reset these flags when switching MediaSinks.
  mAudioCompleted = false;
  mVideoCompleted = false;

  // Backup current playback parameters.
  MediaSink::PlaybackParams params = mMediaSink->GetPlaybackParams();

  // Stop and shut down the existing sink.
  StopMediaSink();
  mMediaSink->Shutdown();

  // Create a new sink according to whether audio is captured.
  mMediaSink = CreateMediaSink(aCaptured);

  // Restore playback parameters.
  mMediaSink->SetPlaybackParams(params);

  // We don't need to call StartMediaSink() here because IsPlaying() is now
  // always in sync with the playing state of MediaSink. It will be started in
  // MaybeStartPlayback() in the next cycle if necessary.

  mAudioCaptured = aCaptured;
  ScheduleStateMachine();

  // Don't buffer as much when audio is captured because we don't need to worry
  // about high latency audio devices.
  mAmpleAudioThresholdUsecs = mAudioCaptured ?
                              detail::AMPLE_AUDIO_USECS / 2 :
                              detail::AMPLE_AUDIO_USECS;
  if (mIsAudioPrerolling && DonePrerollingAudio()) {
    StopPrerollingAudio();
  }
}

uint32_t MediaDecoderStateMachine::GetAmpleVideoFrames() const
{
  MOZ_ASSERT(OnTaskQueue());
  return (mReader->IsAsync() && mReader->VideoIsHardwareAccelerated())
    ? std::max<uint32_t>(sVideoQueueHWAccelSize, MIN_VIDEO_QUEUE_SIZE)
    : std::max<uint32_t>(sVideoQueueDefaultSize, MIN_VIDEO_QUEUE_SIZE);
}

void
MediaDecoderStateMachine::DumpDebugInfo()
{
  MOZ_ASSERT(NS_IsMainThread());

  // It is fine to capture a raw pointer here because MediaDecoder only call
  // this function before shutdown begins.
  nsCOMPtr<nsIRunnable> r = NS_NewRunnableFunction([this] () {
    mMediaSink->DumpDebugInfo();
    DUMP_LOG(
      "GetMediaTime=%lld GetClock=%lld mMediaSink=%p "
      "mState=%s mPlayState=%d mSentFirstFrameLoadedEvent=%d IsPlaying=%d "
      "mAudioStatus=%s mVideoStatus=%s mDecodedAudioEndTime=%lld mDecodedVideoEndTime=%lld "
      "mIsAudioPrerolling=%d mIsVideoPrerolling=%d "
      "mAudioCompleted=%d mVideoCompleted=%d",
      GetMediaTime(), mMediaSink->IsStarted() ? GetClock() : -1, mMediaSink.get(),
      ToStateStr(), mPlayState.Ref(), mSentFirstFrameLoadedEvent, IsPlaying(),
      AudioRequestStatus(), VideoRequestStatus(), mDecodedAudioEndTime, mDecodedVideoEndTime,
      mIsAudioPrerolling, mIsVideoPrerolling, mAudioCompleted.Ref(), mVideoCompleted.Ref());
  });

  OwnerThread()->DispatchStateChange(r.forget());
}

void MediaDecoderStateMachine::AddOutputStream(ProcessedMediaStream* aStream,
                                               bool aFinishWhenEnded)
{
  MOZ_ASSERT(NS_IsMainThread());
  DECODER_LOG("AddOutputStream aStream=%p!", aStream);
  mOutputStreamManager->Add(aStream, aFinishWhenEnded);
  nsCOMPtr<nsIRunnable> r = NewRunnableMethod<bool>(
    this, &MediaDecoderStateMachine::SetAudioCaptured, true);
  OwnerThread()->Dispatch(r.forget());
}

void MediaDecoderStateMachine::RemoveOutputStream(MediaStream* aStream)
{
  MOZ_ASSERT(NS_IsMainThread());
  DECODER_LOG("RemoveOutputStream=%p!", aStream);
  mOutputStreamManager->Remove(aStream);
  if (mOutputStreamManager->IsEmpty()) {
    nsCOMPtr<nsIRunnable> r = NewRunnableMethod<bool>(
      this, &MediaDecoderStateMachine::SetAudioCaptured, false);
    OwnerThread()->Dispatch(r.forget());
  }
}

size_t
MediaDecoderStateMachine::SizeOfVideoQueue() const
{
  return mReader->SizeOfVideoQueueInBytes();
}

size_t
MediaDecoderStateMachine::SizeOfAudioQueue() const
{
  return mReader->SizeOfAudioQueueInBytes();
}

AbstractCanonical<media::TimeIntervals>*
MediaDecoderStateMachine::CanonicalBuffered() const
{
  return mReader->CanonicalBuffered();
}

MediaEventSource<void>&
MediaDecoderStateMachine::OnMediaNotSeekable() const
{
  return mReader->OnMediaNotSeekable();
}

const char*
MediaDecoderStateMachine::AudioRequestStatus() const
{
  MOZ_ASSERT(OnTaskQueue());
  if (mReader->IsRequestingAudioData()) {
    MOZ_DIAGNOSTIC_ASSERT(!mReader->IsWaitingAudioData());
    return "pending";
  } else if (mReader->IsWaitingAudioData()) {
    return "waiting";
  }
  return "idle";
}

const char*
MediaDecoderStateMachine::VideoRequestStatus() const
{
  MOZ_ASSERT(OnTaskQueue());
  if (mReader->IsRequestingVideoData()) {
    MOZ_DIAGNOSTIC_ASSERT(!mReader->IsWaitingVideoData());
    return "pending";
  } else if (mReader->IsWaitingVideoData()) {
    return "waiting";
  }
  return "idle";
}

void
MediaDecoderStateMachine::OnSuspendTimerResolved()
{
  DECODER_LOG("OnSuspendTimerResolved");
  mVideoDecodeSuspendTimer.CompleteRequest();
  mVideoDecodeSuspended = true;
  mOnPlaybackEvent.Notify(MediaEventType::EnterVideoSuspend);
  mReader->SetVideoBlankDecode(true);
}

void
MediaDecoderStateMachine::OnSuspendTimerRejected()
{
  DECODER_LOG("OnSuspendTimerRejected");
  MOZ_ASSERT(OnTaskQueue());
  MOZ_ASSERT(!mVideoDecodeSuspended);
  mVideoDecodeSuspendTimer.CompleteRequest();
}

} // namespace mozilla

#undef NS_DispatchToMainThread
