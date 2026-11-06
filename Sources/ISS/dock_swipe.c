// Sending swipes to the Dock: the macOS 26 and 27.2 event formats, and the paced
// slides that replace the Dock's slow spring finish. Slides run on slideQueue so
// the main run loop, and with it our event tap, stays free.

#include "iss_internal.h"
#include <mach/mach_time.h>


// Slide state: switches longer than 0ms post their progress from a serial queue
double slideDurationMs = ISS_DEFAULT_SLIDE_DURATION_MS;
dispatch_queue_t slideQueue = NULL;
atomic_uint pendingSlides = 0;

// Real swipes' fast finishes queued or running; a new swipe cuts them short
atomic_uint finishActive = 0;
atomic_bool finishInterrupted = false;

/** @brief Minimal signed swipe progress for every gesture phase.
 *
 * A small nonzero value minimizes visible travel while retaining direction;
 * velocity still commits the gesture.
 */
double iss_swipe_progress_for_phase(CGSGesturePhase phase, ISSDirection direction) {
    (void)phase;
    const bool isRight = (direction == ISSDirectionRight);
    const double magnitude = 0.000016;
    return isRight ? magnitude : -magnitude;
}

// The fling on the ended phase is what commits the switch on macOS 27. The payload
// field is 16.16 fixed point (max ~32767), so the speed setting isn't reused here.
static const double kModernFlingVelocity = 9999.0;

// Cached natural scrolling preference; -1 means re-read it
static int naturalScrolling = -1;

// Posted by System Settings when the natural scrolling checkbox is toggled
static void natural_scrolling_changed(CFNotificationCenterRef center, void *observer,
                                      CFNotificationName name, const void *object,
                                      CFDictionaryRef userInfo) {
    (void)center;
    (void)observer;
    (void)name;
    (void)object;
    (void)userInfo;
    CFPreferencesAppSynchronize(kCFPreferencesAnyApplication);
    naturalScrolling = -1;
}

void iss_start_watching_natural_scrolling(void) {
    naturalScrolling = -1;
    CFNotificationCenterAddObserver(CFNotificationCenterGetDistributedCenter(),
                                    &naturalScrolling, natural_scrolling_changed,
                                    CFSTR("SwipeScrollDirectionDidChangeNotification"),
                                    NULL, CFNotificationSuspensionBehaviorDeliverImmediately);
}

void iss_stop_watching_natural_scrolling(void) {
    CFNotificationCenterRemoveObserver(CFNotificationCenterGetDistributedCenter(),
                                       &naturalScrolling,
                                       CFSTR("SwipeScrollDirectionDidChangeNotification"),
                                       NULL);
}

// On macOS 27 right is negative, and the Dock reads the posted sign through the
// natural scrolling preference, so it flips again with natural scrolling off.
static double iss_modern_swipe_sign(ISSDirection direction) {
    if (naturalScrolling < 0) {
        Boolean valid = false;
        Boolean natural = CFPreferencesGetAppBooleanValue(CFSTR("com.apple.swipescrolldirection"),
                                                          kCFPreferencesAnyApplication, &valid);
        naturalScrolling = (!valid || natural) ? 1 : 0;
    }
    const double sign = (direction == ISSDirectionRight) ? -1.0 : 1.0;
    return naturalScrolling ? sign : -sign;
}

bool iss_post_tagged_dock_swipe(CGEventRef event) {
    // The Dock ignores a DockControl event without a companion gesture event
    CGEventRef companion = CGEventCreate(NULL);
    if (!companion) {
        return false;
    }
    CGEventSetIntegerValueField(companion, kCGSEventTypeField, kCGSEventGesture);

    CGEventSetIntegerValueField(event, kCGEventSourceUserData, kSyntheticEventTag);
    CGEventSetIntegerValueField(companion, kCGEventSourceUserData, kSyntheticEventTag);
    CGEventPost(kCGSessionEventTap, event);
    CGEventPost(kCGSessionEventTap, companion);
    CFRelease(companion);
    return true;
}

/** @brief Posts a macOS 27 dock swipe. `progress` and `velocity` use the trackpad's signs
 * (positive moves right); they're flipped here to the sign the Dock reads from a posted event.
 * Progress and velocity must point the same way for the release to commit.
 */
static bool iss_post_modern_dock_swipe(CGSGesturePhase phase, CGGestureMotion motion, double progress,
                                       double velocity) {
    const double sign = iss_modern_swipe_sign(ISSDirectionRight);

    CGEventRef ev = CGEventCreate(NULL);
    if (!ev) {
        return false;
    }
    CGEventSetIntegerValueField(ev, kCGSEventTypeField, kCGSEventDockControl);
    CGEventSetIntegerValueField(ev, kCGEventGestureHIDType, kIOHIDEventTypeDockSwipe);
    CGEventSetIntegerValueField(ev, kCGEventGesturePhase, phase);
    CGEventSetIntegerValueField(ev, kCGEventGestureSwipeMotion, motion);
    CGEventSetDoubleValueField(ev, kCGEventGestureSwipeProgress, sign * progress);
    // Fields a real trackpad swipe carries (from FasterSwiper's traces).
    CGEventSetIntegerValueField(ev, kCGEventGesturePhaseAlias, phase);
    CGEventSetDoubleValueField(ev, kCGEventGestureZoomDeltaY, 3.0);
    CGEventSetDoubleValueField(ev, kCGEventSourceUnixProcessIDAlias, (double)mach_absolute_time());
    CGEventSetDoubleValueField(ev, kCGEventGestureSwipePositionX, 0.1);
    // Match FasterSwiper: only the Ended event carries velocity.
    if (phase == kCGSGesturePhaseEnded) {
        CGEventSetDoubleValueField(ev, kCGEventGestureSwipeVelocityX, sign * velocity);
        CGEventSetDoubleValueField(ev, kCGEventGestureSwipeVelocityY, sign * velocity);
    }

    CGEventRef augmented = iss_augment_dock_swipe_event(ev);
    CFRelease(ev);
    if (!augmented) {
        return false;
    }

    const bool posted = iss_post_tagged_dock_swipe(augmented);
    CFRelease(augmented);
    return posted;
}

static bool iss_post_legacy_dock_swipe(CGSGesturePhase phase, CGGestureMotion motion, double sign,
                                       double velocity, double progressMagnitude) {
    const double progress = sign * progressMagnitude;

    // Velocity of gesture based on speed setting
    const double vel = sign * velocity;

    CGEventRef ev = CGEventCreate(NULL);
    if (!ev) {
        return false;
    }
    CGEventSetIntegerValueField(ev, kCGSEventTypeField, kCGSEventDockControl);
    CGEventSetIntegerValueField(ev, kCGEventGestureHIDType, kIOHIDEventTypeDockSwipe);
    CGEventSetIntegerValueField(ev, kCGEventGesturePhase, phase);
    CGEventSetDoubleValueField(ev, kCGEventGestureSwipeProgress, progress);
    CGEventSetIntegerValueField(ev, kCGEventGestureSwipeMotion, motion);
    CGEventSetDoubleValueField(ev, kCGEventGestureSwipeVelocityX, vel);
    CGEventSetDoubleValueField(ev, kCGEventGestureSwipeVelocityY, vel);
    CGEventPost(kCGSessionEventTap, ev);
    CFRelease(ev);
    return true;
}

bool iss_post_dock_swipe(CGSGesturePhase phase, ISSDirection direction, double velocity) {
    const double progress = iss_swipe_progress_for_phase(phase, direction);
    if (iss_requires_event_augmentation()) {
        // Near-zero progress (the smallest non-zero 16.16 value) leaves nothing to animate.
        // Full progress switches too, but the previous space flashes before it lands.
        const double sign = direction == ISSDirectionRight ? 1.0 : -1.0;
        return iss_post_modern_dock_swipe(phase, kCGGestureMotionHorizontal, sign * kMinimalProgress,
                                          sign * kModernFlingVelocity);
    }
    return iss_post_legacy_dock_swipe(phase, kCGGestureMotionHorizontal, progress > 0.0 ? 1.0 : -1.0,
                                      velocity, fabs(progress));
}

// Roughly one frame on a 120Hz display
static const useconds_t kSlideFrameMicroseconds = 8333;

static void iss_post_slide_event(CGSGesturePhase phase, const ISSSlideRequest *request, double progress) {
    if (iss_requires_event_augmentation()) {
        // The Dock treats any Ended as a commit; only a cancel stays on the current space
        const bool staying = phase == kCGSGesturePhaseEnded && request->to == 0.0;
        const CGSGesturePhase sent = staying ? kCGSGesturePhaseCancelled : phase;
        const double velocity = (staying || phase != kCGSGesturePhaseEnded) ? 0.0 : copysign(kModernFlingVelocity, request->to);
        if (!request->frameEvent) {
            iss_post_modern_dock_swipe(sent, request->motion, progress, velocity);
            return;
        }
        CGEventRef base = phase == kCGSGesturePhaseEnded ? request->releaseEvent : request->frameEvent;
        CGEventRef edited = iss_edit_dock_swipe_event(base, sent, progress, velocity);
        // A release has to reach the Dock even if the edit failed, or the gesture never closes
        if (!edited && phase == kCGSGesturePhaseEnded) {
            os_log(swipe_log(), "couldn't edit the held release; sending it unedited");
        }
        if (edited || phase == kCGSGesturePhaseEnded) iss_post_tagged_dock_swipe(edited ? edited : base);
        if (edited) CFRelease(edited);
    } else {
        iss_post_legacy_dock_swipe(phase, request->motion, 1.0, request->velocity, progress);
    }
}

/** @brief Drags the swipe from `from` to `to` over the request's duration, then releases.
 *
 * Releasing a barely-moved swipe at low velocity leaves the Dock to finish with
 * its spring, whose slow tail delays focus. Arriving at full progress before the
 * high-velocity release leaves nothing to settle, the same end state as the
 * instant switch. Runs on slideQueue so the main run loop (and our event tap)
 * stays free while the frames are posted.
 */
static void iss_run_slide(void *context) {
    ISSSlideRequest *request = context;
    const unsigned int frames = (unsigned int)fmax(1.0, request->durationMs * 1000.0 / kSlideFrameMicroseconds);
    // Progress is passed pre-signed, so the poster's sign is always +1
    const double direction = request->to != 0.0 ? request->to : request->from;

    if (request->postBegan) {
        iss_post_slide_event(kCGSGesturePhaseBegan, request, copysign(kMinimalProgress, direction));
    }
    unsigned int posted = 0;
    for (unsigned int i = 1; i <= frames && !atomic_load(&finishInterrupted); i++) {
        usleep(kSlideFrameMicroseconds);
        const double progress = request->from + (request->to - request->from) * i / frames;
        iss_post_slide_event(kCGSGesturePhaseChanged, request,
                             progress != 0.0 ? progress : copysign(kMinimalProgress, direction));
        posted = i;
    }
    iss_post_slide_event(kCGSGesturePhaseEnded, request,
                         request->to != 0.0 ? request->to : copysign(kMinimalProgress, direction));
    if (request->frameEvent) {
        os_log(swipe_log(), "finish done: %{public}u of %{public}u frames%{public}s, release sent", posted, frames,
               posted < frames ? " (cut short by a new swipe)" : "");
        CFRelease(request->frameEvent);
        CFRelease(request->releaseEvent);
        atomic_fetch_sub(&finishActive, 1);
    }

    free(request);
    atomic_fetch_sub(&pendingSlides, 1);
}

bool iss_enqueue_slide_request(ISSSlideRequest request) {
    ISSSlideRequest *copy = malloc(sizeof(*copy));
    if (!copy) {
        return false;
    }
    *copy = request;

    if (!slideQueue) {
        slideQueue = dispatch_queue_create("iss.slide", DISPATCH_QUEUE_SERIAL);
    }
    atomic_fetch_add(&pendingSlides, 1);
    dispatch_async_f(slideQueue, copy, iss_run_slide);
    return true;
}

void iss_interrupt_finish(void) {
    atomic_store(&finishInterrupted, atomic_load(&finishActive) > 0);
}

bool iss_enqueue_slide(CGGestureMotion motion, double sign, double durationMs) {
    return iss_enqueue_slide_request((ISSSlideRequest){
        .motion = motion,
        .postBegan = true,
        .from = 0.0,
        .to = sign,
        .velocity = sign * kSlideReleaseVelocity,
        .durationMs = durationMs,
    });
}

static void iss_slide_queue_drained(void *context) {
    (void)context;
}

void iss_wait_for_slides(void) {
    if (slideQueue) dispatch_sync_f(slideQueue, NULL, iss_slide_queue_drained);
}

void iss_set_slide_duration(double milliseconds) {
    slideDurationMs = milliseconds > 0.0 ? milliseconds : 0.0;
}

bool iss_is_animating(void) {
    return atomic_load(&pendingSlides) > 0;
}
