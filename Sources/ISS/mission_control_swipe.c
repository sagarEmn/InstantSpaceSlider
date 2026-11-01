// Up/down swipes on macOS 26: replaced by a slide so Mission Control opens and
// closes fast. Not active on macOS 27.2, where macOS handles these itself.

#include "iss_internal.h"


// Mission Control override state (vertical swipes and the Mission Control key)
bool missionControlOverrideEnabled = false;
bool verticalTracking = false;
static bool verticalFired = false;

// Real vertical swipes start with a small wobble (often the wrong sign), so
// wait for this much progress before deciding the direction
static const double kVerticalDirectionThreshold = 0.03;

// The trackpad streams vertical and horizontal swipes side by side and cancels
// the loser on release, so a sideways swipe carries vertical drift. Only act
// before release on a clearly vertical swipe with no horizontal one running.
static const double kVerticalEarlyCommit = 0.15;

// Replace a real vertical (Mission Control / App Exposé) swipe with a fast slide
// in the same direction, so the Dock has no spring tail left to play.
CGEventRef vertical_swipe_override(CGEventRef event) {
    CGSGesturePhase phase =
        (CGSGesturePhase)CGEventGetIntegerValueField(event, kCGEventGesturePhase);

    switch (phase) {
    case kCGSGesturePhaseBegan:
        verticalTracking = true;
        verticalFired = false;
        return NULL;

    case kCGSGesturePhaseChanged:
    case kCGSGesturePhaseEnded: {
        if (!verticalTracking) return event;
        if (!verticalFired) {
            double value = CGEventGetDoubleValueField(event, kCGEventGestureSwipeProgress);
            if (phase == kCGSGesturePhaseEnded && fabs(value) < kVerticalDirectionThreshold) {
                value = CGEventGetDoubleValueField(event, kCGEventGestureSwipeVelocityY);
            }
            // Ended means the vertical swipe won; before that, only a clear one counts
            const bool decided = phase == kCGSGesturePhaseEnded
                ? value != 0.0
                : !horizontalActive && fabs(value) >= kVerticalEarlyCommit;
            if (decided) {
                verticalFired = true;
                // Negative (swipe up) opens Mission Control
                const double sign = value < 0.0 ? kMissionControlOpenSign : -kMissionControlOpenSign;
                missionControlOpenedByUs = sign == kMissionControlOpenSign;
                os_log(click_log(), "swipe %{public}s Mission Control", missionControlOpenedByUs ? "opens" : "closes");
                iss_enqueue_slide(kCGGestureMotionVertical, sign, slideDurationMs);
            }
        }
        if (phase == kCGSGesturePhaseEnded) {
            verticalTracking = false;
            verticalFired = false;
        }
        return NULL;
    }

    case kCGSGesturePhaseCancelled:
        verticalTracking = false;
        verticalFired = false;
        return NULL;

    default:
        return verticalTracking ? NULL : event;
    }
}

void iss_set_mission_control_override(bool enabled) {
    missionControlOverrideEnabled = enabled;
    if (!enabled) {
        verticalTracking = false;
        verticalFired = false;
    }
}
