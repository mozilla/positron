/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_KeyframeEffect_h
#define mozilla_dom_KeyframeEffect_h

#include "nsChangeHint.h"
#include "nsCSSProperty.h"
#include "nsCSSValue.h"
#include "nsCycleCollectionParticipant.h"
#include "nsTArray.h"
#include "nsWrapperCache.h"
#include "mozilla/AnimationPerformanceWarning.h"
#include "mozilla/AnimationTarget.h"
#include "mozilla/Attributes.h"
#include "mozilla/ComputedTiming.h"
#include "mozilla/ComputedTimingFunction.h"
#include "mozilla/EffectCompositor.h"
#include "mozilla/KeyframeEffectParams.h"
#include "mozilla/LayerAnimationInfo.h" // LayerAnimations::kRecords
#include "mozilla/Maybe.h"
#include "mozilla/StickyTimeDuration.h"
#include "mozilla/StyleAnimationValue.h"
#include "mozilla/TimeStamp.h"
#include "mozilla/TimingParams.h"
#include "mozilla/dom/AnimationEffectReadOnly.h"
#include "mozilla/dom/AnimationEffectTimingReadOnly.h"
#include "mozilla/dom/Element.h"
#include "mozilla/dom/Nullable.h"

struct JSContext;
class nsCSSPropertySet;
class nsIContent;
class nsIDocument;
class nsIFrame;
class nsIPresShell;
class nsPresContext;

namespace mozilla {

class AnimValuesStyleRule;
enum class CSSPseudoElementType : uint8_t;

namespace dom {
class ElementOrCSSPseudoElement;
class OwningElementOrCSSPseudoElement;
class UnrestrictedDoubleOrKeyframeAnimationOptions;
class UnrestrictedDoubleOrKeyframeEffectOptions;
enum class IterationCompositeOperation : uint32_t;
enum class CompositeOperation : uint32_t;
struct AnimationPropertyDetails;
}

/**
 * A property-value pair specified on a keyframe.
 */
struct PropertyValuePair
{
  nsCSSProperty mProperty;
  // The specified value for the property. For shorthand properties or invalid
  // property values, we store the specified property value as a token stream
  // (string).
  nsCSSValue    mValue;

  bool operator==(const PropertyValuePair& aOther) const {
    return mProperty == aOther.mProperty &&
           mValue == aOther.mValue;
  }
};

/**
 * A single keyframe.
 *
 * This is the canonical form in which keyframe effects are stored and
 * corresponds closely to the type of objects returned via the getKeyframes()
 * API.
 *
 * Before computing an output animation value, however, we flatten these frames
 * down to a series of per-property value arrays where we also resolve any
 * overlapping shorthands/longhands, convert specified CSS values to computed
 * values, etc.
 *
 * When the target element or style context changes, however, we rebuild these
 * per-property arrays from the original list of keyframes objects. As a result,
 * these objects represent the master definition of the effect's values.
 */
struct Keyframe
{
  Keyframe() = default;
  Keyframe(const Keyframe& aOther) = default;
  Keyframe(Keyframe&& aOther)
  {
    *this = Move(aOther);
  }

  Keyframe& operator=(const Keyframe& aOther) = default;
  Keyframe& operator=(Keyframe&& aOther)
  {
    mOffset         = aOther.mOffset;
    mComputedOffset = aOther.mComputedOffset;
    mTimingFunction = Move(aOther.mTimingFunction);
    mPropertyValues = Move(aOther.mPropertyValues);
    return *this;
  }

  Maybe<double>                 mOffset;
  static constexpr double kComputedOffsetNotSet = -1.0;
  double                        mComputedOffset = kComputedOffsetNotSet;
  Maybe<ComputedTimingFunction> mTimingFunction; // Nothing() here means
                                                 // "linear"
  nsTArray<PropertyValuePair>   mPropertyValues;
};

struct AnimationPropertySegment
{
  float mFromKey, mToKey;
  StyleAnimationValue mFromValue, mToValue;
  Maybe<ComputedTimingFunction> mTimingFunction;

  bool operator==(const AnimationPropertySegment& aOther) const {
    return mFromKey == aOther.mFromKey &&
           mToKey == aOther.mToKey &&
           mFromValue == aOther.mFromValue &&
           mToValue == aOther.mToValue &&
           mTimingFunction == aOther.mTimingFunction;
  }
  bool operator!=(const AnimationPropertySegment& aOther) const {
    return !(*this == aOther);
  }
};

struct AnimationProperty
{
  nsCSSProperty mProperty = eCSSProperty_UNKNOWN;

  // Does this property win in the CSS Cascade?
  //
  // For CSS transitions, this is true as long as a CSS animation on the
  // same property and element is not running, in which case we set this
  // to false so that the animation (lower in the cascade) can win.  We
  // then use this to decide whether to apply the style both in the CSS
  // cascade and for OMTA.
  //
  // For CSS Animations, which are overridden by !important rules in the
  // cascade, we actually determine this from the CSS cascade
  // computations, and then use it for OMTA.
  //
  // **NOTE**: This member is not included when comparing AnimationProperty
  // objects for equality.
  bool mWinsInCascade = false;

  // If true, the propery is currently being animated on the compositor.
  //
  // Note that when the owning Animation requests a non-throttled restyle, in
  // between calling RequestRestyle on its EffectCompositor and when the
  // restyle is performed, this member may temporarily become false even if
  // the animation remains on the layer after the restyle.
  //
  // **NOTE**: This member is not included when comparing AnimationProperty
  // objects for equality.
  bool mIsRunningOnCompositor = false;

  Maybe<AnimationPerformanceWarning> mPerformanceWarning;

  InfallibleTArray<AnimationPropertySegment> mSegments;

  // NOTE: This operator does *not* compare the mWinsInCascade member *or* the
  // mIsRunningOnCompositor member.
  // This is because AnimationProperty objects are compared when recreating
  // CSS animations to determine if mutation observer change records need to
  // be created or not. However, at the point when these objects are compared
  // neither the mWinsInCascade nor the mIsRunningOnCompositor will have been
  // set on the new objects so we ignore these members to avoid generating
  // spurious change records.
  bool operator==(const AnimationProperty& aOther) const {
    return mProperty == aOther.mProperty &&
           mSegments == aOther.mSegments;
  }
  bool operator!=(const AnimationProperty& aOther) const {
    return !(*this == aOther);
  }
};

struct ElementPropertyTransition;

namespace dom {

class Animation;

class KeyframeEffectReadOnly : public AnimationEffectReadOnly
{
public:
  KeyframeEffectReadOnly(nsIDocument* aDocument,
                         const Maybe<OwningAnimationTarget>& aTarget,
                         const TimingParams& aTiming,
                         const KeyframeEffectParams& aOptions);

  NS_DECL_ISUPPORTS_INHERITED
  NS_DECL_CYCLE_COLLECTION_SCRIPT_HOLDER_CLASS_INHERITED(KeyframeEffectReadOnly,
                                                        AnimationEffectReadOnly)

  virtual JSObject* WrapObject(JSContext* aCx,
                               JS::Handle<JSObject*> aGivenProto) override;

  virtual ElementPropertyTransition* AsTransition() { return nullptr; }
  virtual const ElementPropertyTransition* AsTransition() const
  {
    return nullptr;
  }

  // KeyframeEffectReadOnly interface
  static already_AddRefed<KeyframeEffectReadOnly>
  Constructor(const GlobalObject& aGlobal,
              const Nullable<ElementOrCSSPseudoElement>& aTarget,
              JS::Handle<JSObject*> aKeyframes,
              const UnrestrictedDoubleOrKeyframeEffectOptions& aOptions,
              ErrorResult& aRv);

  void GetTarget(Nullable<OwningElementOrCSSPseudoElement>& aRv) const;
  Maybe<NonOwningAnimationTarget> GetTarget() const
  {
    Maybe<NonOwningAnimationTarget> result;
    if (mTarget) {
      result.emplace(*mTarget);
    }
    return result;
  }
  void GetKeyframes(JSContext*& aCx,
                    nsTArray<JSObject*>& aResult,
                    ErrorResult& aRv);
  void GetProperties(nsTArray<AnimationPropertyDetails>& aProperties,
                     ErrorResult& aRv) const;

  IterationCompositeOperation IterationComposite() const;
  CompositeOperation Composite() const;
  void GetSpacing(nsString& aRetVal) const
  {
    mEffectOptions.GetSpacingAsString(aRetVal);
  }

  already_AddRefed<AnimationEffectTimingReadOnly> Timing() const override;

  const TimingParams& SpecifiedTiming() const
  {
    return mTiming->AsTimingParams();
  }
  void SetSpecifiedTiming(const TimingParams& aTiming);
  void NotifyAnimationTimingUpdated();

  Nullable<TimeDuration> GetLocalTime() const;

  // This function takes as input the timing parameters of an animation and
  // returns the computed timing at the specified local time.
  //
  // The local time may be null in which case only static parameters such as the
  // active duration are calculated. All other members of the returned object
  // are given a null/initial value.
  //
  // This function returns a null mProgress member of the return value
  // if the animation should not be run
  // (because it is not currently active and is not filling at this time).
  static ComputedTiming
  GetComputedTimingAt(const Nullable<TimeDuration>& aLocalTime,
                      const TimingParams& aTiming);

  // Shortcut for that gets the computed timing using the current local time as
  // calculated from the timeline time.
  ComputedTiming
  GetComputedTiming(const TimingParams* aTiming = nullptr) const
  {
    return GetComputedTimingAt(GetLocalTime(),
                               aTiming ? *aTiming : SpecifiedTiming());
  }

  void
  GetComputedTimingAsDict(ComputedTimingProperties& aRetVal) const override;

  bool IsInPlay() const;
  bool IsCurrent() const;
  bool IsInEffect() const;

  void SetAnimation(Animation* aAnimation);
  Animation* GetAnimation() const { return mAnimation; }

  void SetKeyframes(JSContext* aContext, JS::Handle<JSObject*> aKeyframes,
                    ErrorResult& aRv);
  void SetKeyframes(nsTArray<Keyframe>&& aKeyframes,
                    nsStyleContext* aStyleContext);
  const AnimationProperty*
  GetAnimationOfProperty(nsCSSProperty aProperty) const;
  bool HasAnimationOfProperty(nsCSSProperty aProperty) const {
    return GetAnimationOfProperty(aProperty) != nullptr;
  }
  const InfallibleTArray<AnimationProperty>& Properties() const {
    return mProperties;
  }
  InfallibleTArray<AnimationProperty>& Properties() {
    return mProperties;
  }

  // Update |mProperties| by recalculating from |mKeyframes| using
  // |aStyleContext| to resolve specified values.
  void UpdateProperties(nsStyleContext* aStyleContext);

  // Updates |aStyleRule| with the animation values produced by this
  // AnimationEffect for the current time except any properties already
  // contained in |aSetProperties|.
  // Any updated properties are added to |aSetProperties|.
  void ComposeStyle(RefPtr<AnimValuesStyleRule>& aStyleRule,
                    nsCSSPropertySet& aSetProperties);
  // Returns true if at least one property is being animated on compositor.
  bool IsRunningOnCompositor() const;
  void SetIsRunningOnCompositor(nsCSSProperty aProperty, bool aIsRunning);
  void ResetIsRunningOnCompositor();

  // Returns true if this effect, applied to |aFrame|, contains properties
  // that mean we shouldn't run transform compositor animations on this element.
  //
  // For example, if we have an animation of geometric properties like 'left'
  // and 'top' on an element, we force all 'transform' animations running at
  // the same time on the same element to run on the main thread.
  //
  // When returning true, |aPerformanceWarning| stores the reason why
  // we shouldn't run the transform animations.
  bool ShouldBlockAsyncTransformAnimations(
    const nsIFrame* aFrame,
    AnimationPerformanceWarning::Type& aPerformanceWarning) const;

  nsIDocument* GetRenderedDocument() const;
  nsPresContext* GetPresContext() const;
  nsIPresShell* GetPresShell() const;

  // Associates a warning with the animated property on the specified frame
  // indicating why, for example, the property could not be animated on the
  // compositor. |aParams| and |aParamsLength| are optional parameters which
  // will be used to generate a localized message for devtools.
  void SetPerformanceWarning(
    nsCSSProperty aProperty,
    const AnimationPerformanceWarning& aWarning);

  // Cumulative change hint on each segment for each property.
  // This is used for deciding the animation is paint-only.
  void CalculateCumulativeChangeHint(nsStyleContext* aStyleContext);

  // Returns true if all of animation properties' change hints
  // can ignore painting if the animation is not visible.
  // See nsChangeHint_Hints_CanIgnoreIfNotVisible in nsChangeHint.h
  // in detail which change hint can be ignored.
  bool CanIgnoreIfNotVisible() const;

protected:
  KeyframeEffectReadOnly(nsIDocument* aDocument,
                         const Maybe<OwningAnimationTarget>& aTarget,
                         AnimationEffectTimingReadOnly* aTiming,
                         const KeyframeEffectParams& aOptions);

  virtual ~KeyframeEffectReadOnly();

  template<class KeyframeEffectType, class OptionsType>
  static already_AddRefed<KeyframeEffectType>
  ConstructKeyframeEffect(const GlobalObject& aGlobal,
                          const Nullable<ElementOrCSSPseudoElement>& aTarget,
                          JS::Handle<JSObject*> aKeyframes,
                          const OptionsType& aOptions,
                          ErrorResult& aRv);

  void ResetWinsInCascade();

  // This effect is registered with its target element so long as:
  //
  // (a) It has a target element, and
  // (b) It is "relevant" (i.e. yet to finish but not idle, or finished but
  //     filling forwards)
  //
  // As a result, we need to make sure this gets called whenever anything
  // changes with regards to this effects's timing including changes to the
  // owning Animation's timing.
  void UpdateTargetRegistration();

  // Remove the current effect target from its EffectSet.
  void UnregisterTarget();

  void RequestRestyle(EffectCompositor::RestyleType aRestyleType);

  // Update the associated frame state bits so that, if necessary, a stacking
  // context will be created and the effect sent to the compositor.  We
  // typically need to do this when the properties referenced by the keyframe
  // have changed, or when the target frame might have changed.
  void MaybeUpdateFrameForCompositor();

  // Looks up the style context associated with the target element, if any.
  // We need to be careful to *not* call this when we are updating the style
  // context. That's because calling GetStyleContextForElement when we are in
  // the process of building a style context may trigger various forms of
  // infinite recursion.
  already_AddRefed<nsStyleContext>
  GetTargetStyleContext();

  Maybe<OwningAnimationTarget> mTarget;
  RefPtr<Animation> mAnimation;

  RefPtr<AnimationEffectTimingReadOnly> mTiming;
  KeyframeEffectParams mEffectOptions;

  // The specified keyframes.
  nsTArray<Keyframe>          mKeyframes;

  // A set of per-property value arrays, derived from |mKeyframes|.
  nsTArray<AnimationProperty> mProperties;

  // The computed progress last time we composed the style rule. This is
  // used to detect when the progress is not changing (e.g. due to a step
  // timing function) so we can avoid unnecessary style updates.
  Nullable<double> mProgressOnLastCompose;

  // We need to track when we go to or from being "in effect" since
  // we need to re-evaluate the cascade of animations when that changes.
  bool mInEffectOnLastAnimationTimingUpdate;

private:
  nsChangeHint mCumulativeChangeHint;

  nsIFrame* GetAnimationFrame() const;

  bool CanThrottle() const;
  bool CanThrottleTransformChanges(nsIFrame& aFrame) const;

  // Returns true unless Gecko limitations prevent performing transform
  // animations for |aFrame|. When returning true, the reason for the
  // limitation is stored in |aOutPerformanceWarning|.
  static bool CanAnimateTransformOnCompositor(
    const nsIFrame* aFrame,
    AnimationPerformanceWarning::Type& aPerformanceWarning);
  static bool IsGeometricProperty(const nsCSSProperty aProperty);

  static const TimeDuration OverflowRegionRefreshInterval();
};

class KeyframeEffect : public KeyframeEffectReadOnly
{
public:
  KeyframeEffect(nsIDocument* aDocument,
                 const Maybe<OwningAnimationTarget>& aTarget,
                 const TimingParams& aTiming,
                 const KeyframeEffectParams& aOptions);

  JSObject* WrapObject(JSContext* aCx,
                       JS::Handle<JSObject*> aGivenProto) override;

  static already_AddRefed<KeyframeEffect>
  Constructor(const GlobalObject& aGlobal,
              const Nullable<ElementOrCSSPseudoElement>& aTarget,
              JS::Handle<JSObject*> aKeyframes,
              const UnrestrictedDoubleOrKeyframeEffectOptions& aOptions,
              ErrorResult& aRv);

  // Variant of Constructor that accepts a KeyframeAnimationOptions object
  // for use with for Animatable.animate.
  // Not exposed to content.
  static already_AddRefed<KeyframeEffect>
  Constructor(const GlobalObject& aGlobal,
              const Nullable<ElementOrCSSPseudoElement>& aTarget,
              JS::Handle<JSObject*> aKeyframes,
              const UnrestrictedDoubleOrKeyframeAnimationOptions& aOptions,
              ErrorResult& aRv);

  void NotifySpecifiedTimingUpdated();

  // This method calls GetTargetStyleContext which is not safe to use when
  // we are in the middle of updating style. If we need to use this when
  // updating style, we should pass the nsStyleContext into this method and use
  // that to update the properties rather than calling
  // GetStyleContextForElement.
  void SetTarget(const Nullable<ElementOrCSSPseudoElement>& aTarget);

protected:
  ~KeyframeEffect() override;
};

} // namespace dom
} // namespace mozilla

#endif // mozilla_dom_KeyframeEffect_h
