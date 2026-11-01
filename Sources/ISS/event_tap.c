// The event tap: every keyboard, mouse and gesture event of interest passes
// through eventTapCallback, which decides who handles it. Returning the event
// lets it continue to the Dock; returning NULL swallows it.

#include "iss_internal.h"

// Key code the Mission Control (F3) key reports on Apple keyboards
static const int64_t kMissionControlKeyCode = 160;
static const int64_t kEscapeKeyCode = 53;

static CFMachPortRef globalTap = NULL;
static CFRunLoopSourceRef globalSource = NULL;

os_log_t click_log(void) {
    static os_log_t log = NULL;
    if (!log) log = os_log_create("com.sagarEmn.InstantSpaceSlider", "mission-control-click");
    return log;
}

static CGEventRef eventTapCallback(CGEventTapProxy proxy, CGEventType type,
                                   CGEventRef event, void *refcon) {
    (void)proxy;
    (void)refcon;

    // Re-enable if the system disabled our tap for being too slow
    if (type == kCGEventTapDisabledByTimeout || type == kCGEventTapDisabledByUserInput) {
        if (globalTap) CGEventTapEnable(globalTap, true);
        return event;
    }

    CGSEventType eventType =
        (CGSEventType)CGEventGetIntegerValueField(event, kCGSEventTypeField);

    if (CGEventGetIntegerValueField(event, kCGEventSourceUserData) == kSyntheticEventTag) {
        return event;
    }

    // Mission Control override relies on the legacy (macOS 13-26) event format
    const bool missionControlOverride = missionControlOverrideEnabled && !iss_requires_event_augmentation();
    if (!swipeOverrideEnabled && !missionControlOverride) return event;

    if (type == kCGEventKeyDown || type == kCGEventKeyUp) {
        // Escape closes Mission Control without us
        if (type == kCGEventKeyDown && missionControlOpenedByUs &&
            CGEventGetIntegerValueField(event, kCGKeyboardEventKeycode) == kEscapeKeyCode) {
            os_log(click_log(), "Escape closes Mission Control");
            missionControlOpenedByUs = false;
        }
        if (!missionControlOverride ||
            CGEventGetIntegerValueField(event, kCGKeyboardEventKeycode) != kMissionControlKeyCode) {
            return event;
        }
        if (type == kCGEventKeyDown && !CGEventGetIntegerValueField(event, kCGKeyboardEventAutorepeat)) {
            // Without a real swipe to copy, close if Mission Control looks open, otherwise open
            const double sign = mission_control_on_screen() ? -kMissionControlOpenSign : kMissionControlOpenSign;
            missionControlOpenedByUs = sign == kMissionControlOpenSign;
            os_log(click_log(), "F3 %{public}s Mission Control", missionControlOpenedByUs ? "opens" : "closes");
            iss_enqueue_slide(kCGGestureMotionVertical, sign, slideDurationMs);
        }
        return NULL;
    }

    if (type == kCGEventLeftMouseDown || type == kCGEventLeftMouseUp || type == kCGEventLeftMouseDragged) {
        return missionControlOverride ? mission_control_click(type, event) : event;
    }

    // Pass through synthetic events (non-HID source). Real gesture events
    // from the trackpad have sourcePid == 0 (HID kernel).
    if (eventType == kCGSEventDockControl || eventType == kCGSEventGesture) {
        pid_t sourcePid = (pid_t)CGEventGetIntegerValueField(event, kCGEventSourceUnixProcessID);
        if (sourcePid != 0) return event;
    }

    if (eventType == kCGSEventDockControl) {
        uint32_t hidType =
            (uint32_t)CGEventGetIntegerValueField(event, kCGEventGestureHIDType);
        if (hidType != kIOHIDEventTypeDockSwipe) return event;

        uint16_t motion =
            (uint16_t)CGEventGetIntegerValueField(event, kCGEventGestureSwipeMotion);
        if (motion == kCGGestureMotionHorizontal) {
            CGSGesturePhase horizontalPhase =
                (CGSGesturePhase)CGEventGetIntegerValueField(event, kCGEventGesturePhase);
            if (horizontalPhase == kCGSGesturePhaseBegan) horizontalActive = true;
            if (horizontalPhase == kCGSGesturePhaseEnded || horizontalPhase == kCGSGesturePhaseCancelled) {
                horizontalActive = false;
            }
        }
        if (motion == kCGGestureMotionVertical && missionControlOverride) {
            return vertical_swipe_override(event);
        }
        if (motion != kCGGestureMotionHorizontal || !swipeOverrideEnabled) return event;
        if (CGEventGetIntegerValueField(event, kCGEventGesturePhase) == kCGSGesturePhaseBegan) {
            // The window check can miss Mission Control; trust our own open too
            followInMissionControl = missionControlOpenedByUs || mission_control_on_screen();
            os_log(click_log(), "sideways swipe %{public}s Mission Control", followInMissionControl ? "inside" : "outside");
        }
        // Inside Mission Control the view follows the fingers in either mode;
        // finish it on release like follow mode so a small swipe is snappy
        if (followFingersEnabled || followInMissionControl) {
            return follow_fingers_swipe(event);
        }

        return instant_swipe(event);
    }

    // Suppress companion gesture events during active swipe tracking. Real ones carry
    // their own copy of the swipe, which would compete with a finish in progress.
    if (eventType == kCGSEventGesture && (swipeTracking || verticalTracking || atomic_load(&finishActive) > 0)) {
        return NULL;
    }

    return event;
}

bool iss_init(void) {
    if (globalTap) {
        return true;
    }

    iss_create_predictions();

    CGEventMask mask = CGEventMaskBit(kCGEventKeyDown) | CGEventMaskBit(kCGEventKeyUp)
        | CGEventMaskBit(kCGEventLeftMouseDown) | CGEventMaskBit(kCGEventLeftMouseUp)
        | CGEventMaskBit(kCGEventLeftMouseDragged)
        | (1ULL << kCGSEventGesture) | (1ULL << kCGSEventDockControl);
    globalTap = CGEventTapCreate(
        kCGSessionEventTap,
        kCGHeadInsertEventTap,
        kCGEventTapOptionDefault,
        mask,
        eventTapCallback,
        NULL
    );

    if (!globalTap) {
        return false;
    }

    globalSource = CFMachPortCreateRunLoopSource(NULL, globalTap, 0);
    if (!globalSource) {
        CFRelease(globalTap);
        globalTap = NULL;
        return false;
    }
    CFRunLoopAddSource(CFRunLoopGetMain(), globalSource, kCFRunLoopCommonModes);
    CGEventTapEnable(globalTap, true);

    iss_start_watching_natural_scrolling();

    return true;
}

void iss_destroy(void) {
    // Never quit with the Dock waiting on a release we're holding
    iss_wait_for_slides();
    iss_free_follow_state();
    release_held_clicks(false);
    iss_free_predictions();
    if (globalTap) {
        iss_stop_watching_natural_scrolling();
        CGEventTapEnable(globalTap, false);
        if (globalSource) {
            CFRunLoopRemoveSource(CFRunLoopGetMain(), globalSource, kCFRunLoopCommonModes);
            CFRelease(globalSource);
            globalSource = NULL;
        }
        CFRelease(globalTap);
        globalTap = NULL;
    }
}
