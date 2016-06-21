/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set sw=2 ts=8 et tw=80 : */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "AndroidAPZ.h"

#include "AsyncPanZoomController.h"
#include "GeneratedJNIWrappers.h"
#include "gfxPrefs.h"
#include "OverscrollHandoffState.h"
#include "OverScroller.h"

#define ANDROID_APZ_LOG(...)
// #define ANDROID_APZ_LOG(...) printf_stderr("ANDROID_APZ: " __VA_ARGS__)

namespace mozilla {
namespace layers {

AndroidSpecificState::AndroidSpecificState() {
  widget::sdk::OverScroller::LocalRef scroller;
  if (widget::sdk::OverScroller::New(widget::GeckoAppShell::GetApplicationContext(), &scroller) != NS_OK) {
    ANDROID_APZ_LOG("%p Failed to create Android OverScroller\n", this);
    return;
  }
  mOverScroller = scroller;
}

const float BOUNDS_EPSILON = 1.0f;

// This function is used to convert the scroll offset from a float to an integer
// suitable for using with the Android OverScroller Class.
// The Android OverScroller class (unfortunately) operates in integers instead of floats.
// When casting a float value such as 1.5 to an integer, the value is converted to 1.
// If this value represents the max scroll offset, the OverScroller class will never scroll
// to the end of the page as it will always be 0.5 pixels short. To work around this issue,
// the min and max scroll extents are floor/ceil to convert them to the nearest integer
// just outside of the actual scroll extents. This means, the starting
// scroll offset must be converted the same way so that if the frame has already been
// scrolled 1.5 pixels, it won't be snapped back when converted to an integer. This integer
// rounding error was one of several causes of Bug 1276463.
static int32_t
ClampStart(float aOrigin, float aMin, float aMax)
{
  if (aOrigin <= aMin) {
    return (int32_t)floor(aMin);
  } else if (aOrigin >= aMax) {
    return (int32_t)ceil(aMax);
  }
  return (int32_t)aOrigin;
}

AndroidFlingAnimation::AndroidFlingAnimation(AsyncPanZoomController& aApzc,
                                             PlatformSpecificStateBase* aPlatformSpecificState,
                                             const RefPtr<const OverscrollHandoffChain>& aOverscrollHandoffChain,
                                             bool aFlingIsHandoff,
                                             const RefPtr<const AsyncPanZoomController>& aScrolledApzc)
  : mApzc(aApzc)
  , mOverscrollHandoffChain(aOverscrollHandoffChain)
  , mScrolledApzc(aScrolledApzc)
  , mSentBounceX(false)
  , mSentBounceY(false)
{
  MOZ_ASSERT(mOverscrollHandoffChain);
  MOZ_ASSERT(aPlatformSpecificState->AsAndroidSpecificState());
  mOverScroller = aPlatformSpecificState->AsAndroidSpecificState()->mOverScroller;
  MOZ_ASSERT(mOverScroller);

  // Drop any velocity on axes where we don't have room to scroll anyways
  // (in this APZC, or an APZC further in the handoff chain).
  // This ensures that we don't take the 'overscroll' path in Sample()
  // on account of one axis which can't scroll having a velocity.
  if (!mOverscrollHandoffChain->CanScrollInDirection(&mApzc, Layer::HORIZONTAL)) {
    ReentrantMonitorAutoEnter lock(mApzc.mMonitor);
    mApzc.mX.SetVelocity(0);
  }
  if (!mOverscrollHandoffChain->CanScrollInDirection(&mApzc, Layer::VERTICAL)) {
    ReentrantMonitorAutoEnter lock(mApzc.mMonitor);
    mApzc.mY.SetVelocity(0);
  }

  ParentLayerPoint velocity = mApzc.GetVelocityVector();
  mPreviousVelocity = velocity;

  float scrollRangeStartX = mApzc.mX.GetPageStart().value;
  float scrollRangeEndX = mApzc.mX.GetScrollRangeEnd().value;
  float scrollRangeStartY = mApzc.mY.GetPageStart().value;
  float scrollRangeEndY = mApzc.mY.GetScrollRangeEnd().value;
  mStartOffset.x = mPreviousOffset.x = mApzc.mX.GetOrigin().value;
  mStartOffset.y = mPreviousOffset.y = mApzc.mY.GetOrigin().value;
  float length = velocity.Length();
  if (length > 0.0f) {
    mFlingDirection = velocity / length;
  }

  int32_t originX = ClampStart(mStartOffset.x, scrollRangeStartX, scrollRangeEndX);
  int32_t originY = ClampStart(mStartOffset.y, scrollRangeStartY, scrollRangeEndY);
  mOverScroller->Fling(originX, originY,
                       // Android needs the velocity in pixels per second and it is in pixels per ms.
                       (int32_t)(velocity.x * 1000.0f), (int32_t)(velocity.y * 1000.0f),
                       (int32_t)floor(scrollRangeStartX), (int32_t)ceil(scrollRangeEndX),
                       (int32_t)floor(scrollRangeStartY), (int32_t)ceil(scrollRangeEndY),
                       0, 0);
}

/**
 * Advances a fling by an interpolated amount based on the Android OverScroller.
 * This should be called whenever sampling the content transform for this
 * frame. Returns true if the fling animation should be advanced by one frame,
 * or false if there is no fling or the fling has ended.
 */
bool
AndroidFlingAnimation::DoSample(FrameMetrics& aFrameMetrics,
                                const TimeDuration& aDelta)
{
  bool shouldContinueFling = true;

  mOverScroller->ComputeScrollOffset(&shouldContinueFling);
  // OverScroller::GetCurrVelocity will sometimes return NaN. So need to externally
  // calculate current velocity and not rely on what the OverScroller calculates.
  // mOverScroller->GetCurrVelocity(&speed);
  int32_t currentX = 0;
  int32_t currentY = 0;
  mOverScroller->GetCurrX(&currentX);
  mOverScroller->GetCurrY(&currentY);
  ParentLayerPoint offset((float)currentX, (float)currentY);

  bool hitBoundX = CheckBounds(mApzc.mX, offset.x, mFlingDirection.x, &(offset.x));
  bool hitBoundY = CheckBounds(mApzc.mY, offset.y, mFlingDirection.y, &(offset.y));

  ParentLayerPoint velocity = mPreviousVelocity;

  // Sometimes the OverScroller fails to update the offset for a frame.
  // If the frame can still scroll we just use the velocity from the previous
  // frame. However, if the frame can no longer scroll in the direction
  // of the fling, then end the animation.
  if (offset != mPreviousOffset) {
    if (aDelta.ToMilliseconds() > 0) {
      velocity = (offset - mPreviousOffset) / (float)aDelta.ToMilliseconds();
      mPreviousVelocity = velocity;
    }
  } else if (hitBoundX || hitBoundY) {
    // We have reached the end of the scroll in one of the directions being scrolled and the offset has not
    // changed so end animation.
    shouldContinueFling = false;
  }

  float speed = velocity.Length();

  // gfxPrefs::APZFlingStoppedThreshold is only used in tests.
  if (!shouldContinueFling || (speed < gfxPrefs::APZFlingStoppedThreshold())) {
    if (shouldContinueFling) {
      // The OverScroller thinks it should continue but the speed is below
      // the stopping threshold so abort the animation.
      mOverScroller->AbortAnimation();
    }
    // This animation is going to end. If DeferHandleFlingOverscroll
    // has not been called and there is still some velocity left,
    // call it so that fling hand off may occur if applicable.
    if (!mSentBounceX && !mSentBounceY && (speed > 0.0f)) {
      DeferHandleFlingOverscroll(velocity);
    }
    return false;
  }

  mPreviousOffset = offset;

  mApzc.SetVelocityVector(velocity);
  aFrameMetrics.SetScrollOffset(offset / aFrameMetrics.GetZoom());

  // If we hit a bounds while flinging, send the velocity so that the bounce
  // animation can play.
  if (hitBoundX || hitBoundY) {
    ParentLayerPoint bounceVelocity = velocity;

    if (!mSentBounceX && hitBoundX && fabsf(offset.x - mStartOffset.x) > BOUNDS_EPSILON) {
      mSentBounceX = true;
    } else {
      bounceVelocity.x = 0.0f;
    }

    if (!mSentBounceY && hitBoundY && fabsf(offset.y - mStartOffset.y) > BOUNDS_EPSILON) {
      mSentBounceY = true;
    } else {
      bounceVelocity.y = 0.0f;
    }
    if (!IsZero(bounceVelocity)) {
      DeferHandleFlingOverscroll(bounceVelocity);
    }
  }

  return true;
}

void
AndroidFlingAnimation::DeferHandleFlingOverscroll(ParentLayerPoint& aVelocity)
{
  mDeferredTasks.AppendElement(
      NewRunnableMethod<ParentLayerPoint,
                        RefPtr<const OverscrollHandoffChain>,
                        RefPtr<const AsyncPanZoomController>>(&mApzc,
                                                              &AsyncPanZoomController::HandleFlingOverscroll,
                                                              aVelocity,
                                                              mOverscrollHandoffChain,
                                                              mScrolledApzc));

}

bool
AndroidFlingAnimation::CheckBounds(Axis& aAxis, float aValue, float aDirection, float* aClamped)
{
  if ((aDirection < 0.0f) && (aValue <= aAxis.GetPageStart().value)) {
    if (aClamped) {
      *aClamped = aAxis.GetPageStart().value;
    }
    return true;
  } else if ((aDirection > 0.0f) && (aValue >= aAxis.GetScrollRangeEnd().value)) {
    if (aClamped) {
      *aClamped = aAxis.GetScrollRangeEnd().value;
    }
    return true;
  }
  return false;
}

} // namespace layers
} // namespace mozilla
