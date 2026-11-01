// Left/right swipes. Two modes:
//   Instant: the real swipe is swallowed and replaced by one switch.
//   Follow fingers: the real swipe passes through so the Dock tracks the fingers,
//   and its release is replaced by a fast finish toward the last direction of
//   movement. Inside Mission Control the same finish moves its view instead.

#include "iss_internal.h"


// Swipe override state
bool swipeOverrideEnabled = false;
bool swipeTracking = false;
static bool swipeFired = false;
// Whether a horizontal swipe is running; vertical swipes stand back while it is
bool horizontalActive = false;

// Follow-fingers swipe mode: the Dock tracks the real swipe, we only finish it
bool followFingersEnabled = false;
static bool followTracking = false;
static double followLastProgress = 0.0;
static double followLastDirection = 0.0;
// The latest real Changed; on macOS 27 the finish's frames are edited copies of it
static CGEventRef followFrameEvent = NULL;
// Swipes inside Mission Control browse its view rather than switch spaces
bool followInMissionControl = false;

// Progress changes smaller than this are finger jitter, not a change of direction
static const double kFollowDirectionJitter = 0.002;
// A release at least this fast says which way the fingers were last moving
static const double kFollowReleaseVelocity = 0.5;
// How far a swipe with no desktop on that side can move, however far the fingers go
static const double kEdgeOvershoot = 0.30;
static bool followBlockedLeft = false;
static bool followBlockedRight = false;

// Release speed for swipes inside Mission Control on macOS 26 (see iss_set_mission_control_swipe_speed)
static double missionControlSwipeSpeed = ISS_DEFAULT_MISSION_CONTROL_SWIPE_SPEED;

static void iss_finish_followed_swipe(double progress, double lastDirection, CGEventRef frameEvent,
                                      CGEventRef releaseEvent);

/** @brief Replaces a real release with a fast finish, or returns it if it can't.
 *
 * On macOS 27 the Dock reads a swipe from the event's embedded payload, and only a
 * release that says so in there closes the gesture, so the finish is made of edited
 * copies of the real events rather than synthetic ones.
 */
static CGEventRef finish_real_swipe(CGEventRef event, double progress, double direction) {
    if (!iss_requires_event_augmentation()) {
        iss_finish_followed_swipe(progress, direction, NULL, NULL);
        return NULL;
    }
    CGEventRef releaseEvent = followFrameEvent ? CGEventCreateCopy(event) : NULL;
    if (!releaseEvent) return event;
    CGEventRef frameEvent = followFrameEvent;
    followFrameEvent = NULL;
    iss_finish_followed_swipe(progress, direction, frameEvent, releaseEvent);
    return NULL;
}

static void update_follow_edges(void) {
    ISSSpaceInfo info;
    const bool hasInfo = iss_requires_event_augmentation() && iss_get_space_info(&info);
    followBlockedLeft = hasInfo && iss_should_block_switch(&info, ISSDirectionLeft);
    followBlockedRight = hasInfo && iss_should_block_switch(&info, ISSDirectionRight);
}

/// Where the view should be: toward a side with no desktop, the fingers meet resistance
static double progress_within_edges(double progress) {
    if ((progress > 0.0 && !followBlockedRight) || (progress < 0.0 && !followBlockedLeft)) return progress;
    return copysign(kEdgeOvershoot * tanh(fabs(progress) / kEdgeOvershoot), progress);
}

CGEventRef follow_fingers_swipe(CGEventRef event) {
    CGSGesturePhase phase =
        (CGSGesturePhase)CGEventGetIntegerValueField(event, kCGEventGesturePhase);
    double progress = CGEventGetDoubleValueField(event, kCGEventGestureSwipeProgress);
    const double shownProgress = progress_within_edges(progress);

    switch (phase) {
    case kCGSGesturePhaseBegan:
        followTracking = !iss_is_expose_active();
        followLastProgress = progress;
        followLastDirection = progress > 0.0 ? 1.0 : (progress < 0.0 ? -1.0 : 0.0);
        iss_interrupt_finish();
        if (followFrameEvent) {
            CFRelease(followFrameEvent);
            followFrameEvent = NULL;
        }
        update_follow_edges();
        return event;

    case kCGSGesturePhaseChanged:
        if (followTracking && fabs(progress - followLastProgress) >= kFollowDirectionJitter) {
            followLastDirection = progress > followLastProgress ? 1.0 : -1.0;
            followLastProgress = progress;
        }
        if (followTracking && iss_requires_event_augmentation()) {
            if (followFrameEvent) CFRelease(followFrameEvent);
            followFrameEvent = CGEventCreateCopy(event);
            if (shownProgress != progress) {
                CGEventRef resisted = iss_edit_dock_swipe_event(event, kCGSGesturePhaseChanged, shownProgress, 0.0);
                if (resisted) return resisted;
            }
        }
        return event;

    case kCGSGesturePhaseEnded: {
        if (!followTracking) return event;
        followTracking = false;
        double velocity = CGEventGetDoubleValueField(event, kCGEventGestureSwipeVelocityX);
        double direction = fabs(velocity) >= kFollowReleaseVelocity ? (velocity > 0.0 ? 1.0 : -1.0)
                                                                    : followLastDirection;
        if (followInMissionControl && !iss_requires_event_augmentation()) {
            // Synthetic progress confuses Mission Control's view, so let the real release
            // through and just make it a quick flick in the last direction of movement
            if (direction != 0.0) {
                const double releaseVelocity = direction * missionControlSwipeSpeed;
                CGEventSetDoubleValueField(event, kCGEventGestureSwipeVelocityX, releaseVelocity);
                CGEventSetDoubleValueField(event, kCGEventGestureSwipeVelocityY, releaseVelocity);
            }
            return event;
        }
        return finish_real_swipe(event, shownProgress, direction);
    }

    case kCGSGesturePhaseCancelled:
        if (!followTracking) return event;
        followTracking = false;
        // A real cancel goes back where it started, fast like any other finish
        return iss_requires_event_augmentation() ? finish_real_swipe(event, shownProgress, 0.0) : event;

    default:
        return event;
    }
}

/** @brief Finishes a real horizontal swipe the user has just released.
 *
 * The Dock tracked the fingers up to `progress`; slide the rest of the way
 * toward the last direction of movement (a full switch, or back to the current
 * space) at the configured speed, then release, so there's no spring tail.
 */
static void iss_finish_followed_swipe(double progress, double lastDirection, CGEventRef frameEvent,
                                      CGEventRef releaseEvent) {
    double target = 0.0;
    const bool movingAway = lastDirection != 0.0 && (progress == 0.0 || (lastDirection > 0.0) == (progress > 0.0));
    if (movingAway) {
        // Moving away from the current space (or from rest) commits to the space on that side
        ISSDirection dir = iss_direction_from_gesture_value(lastDirection);
        ISSSpaceInfo info;
        const bool hasInfo = iss_get_space_info(&info);
        // Past the first/last space there's nothing to switch to; snap back instead
        if (!hasInfo || !iss_should_block_switch(&info, dir)) {
            target = lastDirection > 0.0 ? 1.0 : -1.0;
            // Browsing Mission Control moves its view, not our predictions
            if (hasInfo && !followInMissionControl) {
                unsigned int predicted;
                unsigned int current = get_prediction(info.displayID, &predicted) ? predicted : info.currentIndex;
                unsigned int newIndex = dir == ISSDirectionLeft ? current - 1 : current + 1;
                set_prediction(info.displayID, newIndex);
                if (switchCallback) { switchCallback(newIndex); }
            }
        }
    }

    // A full slide's duration covers a full space; scale by the distance left
    const double durationMs = slideDurationMs * fabs(target - progress);
    if (!followInMissionControl) {
        lastGestureTime = CFAbsoluteTimeGetCurrent() + durationMs / 1000.0;
    }
    if (frameEvent) {
        atomic_store(&finishInterrupted, false);
        atomic_fetch_add(&finishActive, 1);
    }
    const bool queued = iss_enqueue_slide_request((ISSSlideRequest){
        .motion = kCGGestureMotionHorizontal,
        .postBegan = false,
        .from = progress,
        .to = target,
        .velocity = target * kSlideReleaseVelocity,
        .durationMs = durationMs,
        .frameEvent = frameEvent,
        .releaseEvent = releaseEvent,
    });
    if (!queued && frameEvent) {
        // Out of memory: the held release still has to close the gesture
        iss_post_tagged_dock_swipe(releaseEvent);
        CFRelease(frameEvent);
        CFRelease(releaseEvent);
        atomic_fetch_sub(&finishActive, 1);
    }
}

void iss_free_follow_state(void) {
    if (followFrameEvent) {
        CFRelease(followFrameEvent);
        followFrameEvent = NULL;
    }
}

// Replace a real swipe with one instant switch, fired on its first movement.
CGEventRef instant_swipe(CGEventRef event) {
    CGSGesturePhase phase =
        (CGSGesturePhase)CGEventGetIntegerValueField(event, kCGEventGesturePhase);

    switch (phase) {
    case kCGSGesturePhaseBegan:
        // Leave swipes inside Mission Control / App Exposé to the Dock
        if (iss_is_expose_active() || iss_is_mission_control_active()) return event;
        swipeTracking = true;
        swipeFired = false;
        return NULL;

    case kCGSGesturePhaseChanged: {
        if (!swipeTracking) return event;
        if (!swipeFired) {
            double progress =
                CGEventGetDoubleValueField(event, kCGEventGestureSwipeProgress);
            if (progress != 0.0) {
                ISSDirection dir = iss_direction_from_gesture_value(progress);
                swipeFired = true;
                swipe_override_switch(dir);
            }
        }
        return NULL;
    }

    case kCGSGesturePhaseEnded: {
        if (!swipeTracking) return event;
        if (!swipeFired) {
            double velocity =
                CGEventGetDoubleValueField(event, kCGEventGestureSwipeVelocityX);
            if (velocity != 0.0) {
                ISSDirection dir = iss_direction_from_gesture_value(velocity);
                swipeFired = true;
                swipe_override_switch(dir);
            }
        }
        swipeTracking = false;
        swipeFired = false;
        if (iss_requires_event_augmentation()) {
            // On macOS 27 the Dock needs the real ended event to close its gesture
            // state; zero the motion so it doesn't switch a second time
            CGEventSetDoubleValueField(event, kCGEventGestureSwipeProgress, 0.0);
            CGEventSetDoubleValueField(event, kCGEventGestureSwipeVelocityX, 0.0);
            CGEventSetDoubleValueField(event, kCGEventGestureSwipeVelocityY, 0.0);
            return event;
        }
        return NULL;
    }

    case kCGSGesturePhaseCancelled:
        swipeTracking = false;
        swipeFired = false;
        return NULL;

    default:
        return swipeTracking ? NULL : event;
    }
}

void iss_set_swipe_override(bool enabled) {
    swipeOverrideEnabled = enabled;
    if (!enabled) {
        swipeTracking = false;
        swipeFired = false;
    }
}

void iss_set_swipe_follow_fingers(bool enabled) {
    followFingersEnabled = enabled;
    followTracking = false;
}

void iss_set_mission_control_swipe_speed(double velocity) {
    missionControlSwipeSpeed = velocity > 0.0 ? velocity : ISS_DEFAULT_MISSION_CONTROL_SWIPE_SPEED;
}
