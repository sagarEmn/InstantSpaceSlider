#include "include/ISS.h"
#include "event_serialize.h"

#include <ApplicationServices/ApplicationServices.h>
#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CGEventTypes.h>
#include <assert.h>
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <float.h>
#include <mach/mach_time.h>
#include <math.h>
#include <os/log.h> // TEMP debug
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const CGEventField kCGSEventTypeField = (CGEventField)55;
static const CGEventField kCGEventGestureHIDType = (CGEventField)110;
static const CGEventField kCGEventGestureSwipeMotion = (CGEventField)123;
static const CGEventField kCGEventGestureSwipeProgress = (CGEventField)124;
static const CGEventField kCGEventGestureSwipePositionX = (CGEventField)125;
static const CGEventField kCGEventGestureSwipeVelocityX = (CGEventField)129;
static const CGEventField kCGEventGestureSwipeVelocityY = (CGEventField)130;
static const CGEventField kCGEventGesturePhase = (CGEventField)132;
static const CGEventField kCGEventGesturePhaseAlias = (CGEventField)134;
static const CGEventField kCGEventGestureZoomDeltaY = (CGEventField)138;
static const CGEventField kCGEventSourceUnixProcessIDAlias = (CGEventField)169;

// See IOHIDEventType enum in IOHIDFamily
static const uint32_t kIOHIDEventTypeDockSwipe = 23;

typedef uint32_t CGSEventType;
enum {
    kCGSEventScrollWheel = 22,
    kCGSEventZoom = 28,
    kCGSEventGesture = 29,
    kCGSEventDockControl = 30,
    kCGSEventFluidTouchGesture = 31,
};

typedef CF_ENUM(uint8_t, CGSGesturePhase) {
    kCGSGesturePhaseNone = 0,
    kCGSGesturePhaseBegan = 1,
    kCGSGesturePhaseChanged = 2,
    kCGSGesturePhaseEnded = 4,
    kCGSGesturePhaseCancelled = 8,
    kCGSGesturePhaseMayBegin = 128,
};

// Limited subset of motion constants observed in synthetic Dock swipe traces.
typedef CF_ENUM(uint16_t, CGGestureMotion) {
    kCGGestureMotionHorizontal = 1,
    kCGGestureMotionVertical = 2,
};

typedef int32_t CGSConnectionID;
typedef uint64_t CGSSpaceID;

extern CFArrayRef CGSCopyManagedDisplaySpaces(CGSConnectionID connection, CFStringRef display) __attribute__((weak_import));
extern CFStringRef CGSCopyActiveMenuBarDisplayIdentifier(CGSConnectionID connection) __attribute__((weak_import));
extern CGSConnectionID CGSMainConnectionID(void) __attribute__((weak_import));
extern CGSSpaceID CGSGetActiveSpace(CGSConnectionID connection) __attribute__((weak_import));

static CFMachPortRef globalTap = NULL;
static CFRunLoopSourceRef globalSource = NULL;

// Overlay detection state
static bool overlayDetectionEnabled = false;

// Swipe override state
static bool swipeOverrideEnabled = false;
static bool swipeTracking = false;
static bool swipeFired = false;

// Follow-fingers swipe mode: the Dock tracks the real swipe, we only finish it
static bool followFingersEnabled = false;
static bool followTracking = false;
static double followLastProgress = 0.0;
static double followLastDirection = 0.0;
// Swipes inside Mission Control browse its view rather than switch spaces
static bool followInMissionControl = false;
static bool mission_control_on_screen(void);

// Progress changes smaller than this are finger jitter, not a change of direction
static const double kFollowDirectionJitter = 0.002;
// A release at least this fast says which way the fingers were last moving
static const double kFollowReleaseVelocity = 0.5;

// Mission Control override state (vertical swipes and the Mission Control key)
static bool missionControlOverrideEnabled = false;
static bool verticalTracking = false;
static bool verticalFired = false;

// Key code the Mission Control (F3) key reports on Apple keyboards
static const int64_t kMissionControlKeyCode = 160;

// Synthetic progress sign that opens Mission Control. The trackpad reports a
// swipe up as negative progress, but the Dock reads our posted sign the other way.
static const double kMissionControlOpenSign = 1.0;

// Real vertical swipes start with a small wobble (often the wrong sign), so
// wait for this much progress before deciding the direction
static const double kVerticalDirectionThreshold = 0.03;

// The trackpad streams vertical and horizontal swipes side by side and cancels
// the loser on release, so a sideways swipe carries vertical drift. Only act
// before release on a clearly vertical swipe with no horizontal one running.
static const double kVerticalEarlyCommit = 0.15;
static bool horizontalActive = false;

// Clicking a space in Mission Control's Spaces bar: the Dock's zoom out is
// slow, so the click is held, looked up off the tap, and replaced by our fast
// close and slide. Only checked after our override opened Mission Control.
static bool missionControlOpenedByUs = false;
static bool clickHeld = false;          // a mouse down is being looked up
static bool clickResolved = false;      // the lookup has finished for the held click
static long clickedSpaceIndex = -1;     // its result: Spaces bar position, or -1
static CGEventRef heldMouseDown = NULL;
static CGEventRef heldMouseUp = NULL;
static unsigned int clickGeneration = 0;
static dispatch_queue_t clickQueue = NULL;
// Marks clicks we replay so the tap lets them through
static const int64_t kReplayedClickMarker = 0x1555C11C;
// A click that moved farther than this is a drag; leave it to the Dock
static const double kClickSlop = 6.0;
static CGEventRef mission_control_click(CGEventType type, CGEventRef event);
static os_log_t dbg(void) { static os_log_t l; if (!l) l = os_log_create("com.sagarEmn.InstantSpaceSlider", "dbg"); return l; } // TEMP debug

// Our macOS 27 swipes re-enter the tap with sourcePid == 0, so count them out instead
static unsigned int syntheticEventsToPassThrough = 0;

// Gesture speed state
static double gestureSpeed = 2000.0;

// Slide state: switches longer than 0ms post their progress from a serial queue
static double slideDurationMs = ISS_DEFAULT_SLIDE_DURATION_MS;
static dispatch_queue_t slideQueue = NULL;
static atomic_uint pendingSlides = 0;
// Release at instant velocity regardless of the speed setting; a slow release brings back the spring tail
static const double kSlideReleaseVelocity = 2000.0;

static ISSSwitchCallback switchCallback = NULL;

// Predictions dictionary: DisplayID (CFStringRef) -> Index (CFNumberRef)
static CFMutableDictionaryRef predictionsDict = NULL;

// While our own gestures are still being animated, CGS reports an
// intermediate space and the Dock fires space-change notifications for each
// hop. Trusting either during a burst makes us overshoot past the last space,
// which on macOS 27 wedges the Dock (black screen) until it times out. Keep
// predictions authoritative until the Dock has been quiet for this long.
static const CFTimeInterval kPredictionTTL = 1.0;
static CFAbsoluteTime lastGestureTime = 0;

static bool gestures_in_flight(void) {
    return CFAbsoluteTimeGetCurrent() - lastGestureTime < kPredictionTTL;
}

static bool get_prediction(const char *displayID, unsigned int *outIndex) {
    if (!displayID || !predictionsDict) {
        return false;
    }
    if (!gestures_in_flight()) {
        CFDictionaryRemoveAllValues(predictionsDict);
        return false;
    }
    
    CFStringRef key = CFStringCreateWithCString(NULL, displayID, kCFStringEncodingUTF8);
    const void *value = CFDictionaryGetValue(predictionsDict, key);
    CFRelease(key);

    if (value) {
        CFNumberGetValue((CFNumberRef)value, kCFNumberIntType, outIndex);
        return true;
    }
    return false;
}

static void set_prediction(const char *displayID, unsigned int index) {
    if (!displayID || !predictionsDict) {
        return;
    }
    
    CFStringRef key = CFStringCreateWithCString(NULL, displayID, kCFStringEncodingUTF8);
    CFNumberRef val = CFNumberCreate(NULL, kCFNumberIntType, &index);
    CFDictionarySetValue(predictionsDict, key, val);
    CFRelease(key);
    CFRelease(val);
}

static bool extract_space_info_from_display(CFDictionaryRef displayDict,
                                            CGSSpaceID activeSpace,
                                            bool hasActiveSpace,
                                            ISSSpaceInfo *outInfo);
static bool load_space_info_for_display(ISSSpaceInfo *info, bool useCursorDisplay);
static bool iss_perform_switch_gesture(ISSDirection direction, double velocity);
static bool iss_switch_with_info(const ISSSpaceInfo *info, ISSDirection direction);
static bool iss_should_block_switch(const ISSSpaceInfo *info, ISSDirection direction);

static ISSDirection iss_direction_from_gesture_value(double value) {
    // Map the intercepted gesture value to the app's logical direction.
    return value > 0.0 ? ISSDirectionRight : ISSDirectionLeft;
}

// Perform a swipe-override switch: get space info, compute target, switch,
// and notify the handler with the target index.
static void swipe_override_switch(ISSDirection dir) {
    ISSSpaceInfo info;
    if (!iss_get_space_info(&info)) {
        iss_perform_switch_gesture(dir, gestureSpeed);
        return;
    }

    unsigned int predicted;
    unsigned int current = get_prediction(info.displayID, &predicted) ? predicted : info.currentIndex;
    unsigned int target = dir == ISSDirectionLeft ? current - 1 : current + 1;

    if (iss_switch_with_info(&info, dir)) {
        set_prediction(info.displayID, target);
        if (switchCallback) { switchCallback(target); }
    }
}

static bool iss_enqueue_slide(CGGestureMotion motion, double sign, double durationMs);
static void iss_finish_followed_swipe(double progress, double lastDirection);

// Let a real horizontal swipe through so the space follows the fingers, then
// replace its release with a fast finish toward the last direction of movement.
// Release speed for swipes inside Mission Control (its own setting; see iss_set_mission_control_swipe_speed)
static double missionControlSwipeSpeed = ISS_DEFAULT_MISSION_CONTROL_SWIPE_SPEED;

static CGEventRef follow_fingers_swipe(CGEventRef event) {
    CGSGesturePhase phase =
        (CGSGesturePhase)CGEventGetIntegerValueField(event, kCGEventGesturePhase);
    double progress = CGEventGetDoubleValueField(event, kCGEventGestureSwipeProgress);

    switch (phase) {
    case kCGSGesturePhaseBegan:
        followTracking = !iss_is_expose_active();
        followLastProgress = progress;
        followLastDirection = progress > 0.0 ? 1.0 : (progress < 0.0 ? -1.0 : 0.0);
        return event;

    case kCGSGesturePhaseChanged:
        if (followTracking && fabs(progress - followLastProgress) >= kFollowDirectionJitter) {
            followLastDirection = progress > followLastProgress ? 1.0 : -1.0;
            followLastProgress = progress;
        }
        return event;

    case kCGSGesturePhaseEnded: {
        if (!followTracking) return event;
        followTracking = false;
        double velocity = CGEventGetDoubleValueField(event, kCGEventGestureSwipeVelocityX);
        double direction = fabs(velocity) >= kFollowReleaseVelocity ? (velocity > 0.0 ? 1.0 : -1.0)
                                                                    : followLastDirection;
        if (followInMissionControl) {
            // Synthetic progress confuses Mission Control's view, so let the real release
            // through and just make it a quick flick in the last direction of movement
            if (direction != 0.0) {
                const double releaseVelocity = direction * missionControlSwipeSpeed;
                CGEventSetDoubleValueField(event, kCGEventGestureSwipeVelocityX, releaseVelocity);
                CGEventSetDoubleValueField(event, kCGEventGestureSwipeVelocityY, releaseVelocity);
            }
            return event;
        }
        iss_finish_followed_swipe(progress, direction);
        return NULL;
    }

    case kCGSGesturePhaseCancelled:
        followTracking = false;
        return event;

    default:
        return event;
    }
}

// Replace a real vertical (Mission Control / App Exposé) swipe with a fast slide
// in the same direction, so the Dock has no spring tail left to play.
static CGEventRef vertical_swipe_override(CGEventRef event) {
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

static CGEventRef eventTapCallback(CGEventTapProxy proxy, CGEventType type,
                                   CGEventRef event, void *refcon) {
    (void)proxy;
    (void)refcon;

    // Re-enable if the system disabled our tap for being too slow
    if (type == kCGEventTapDisabledByTimeout || type == kCGEventTapDisabledByUserInput) {
        if (globalTap) CGEventTapEnable(globalTap, true);
        // Anything we posted while disabled never reaches us, so stop waiting for it
        syntheticEventsToPassThrough = 0;
        return event;
    }

    CGSEventType eventType =
        (CGSEventType)CGEventGetIntegerValueField(event, kCGSEventTypeField);

    // Our own events come back within milliseconds; a count left over after the
    // burst would let real trackpad swipes slip past the override
    if (syntheticEventsToPassThrough > 0 && !gestures_in_flight()) {
        syntheticEventsToPassThrough = 0;
    }

    if (syntheticEventsToPassThrough > 0 &&
        (eventType == kCGSEventDockControl || eventType == kCGSEventGesture)) {
        syntheticEventsToPassThrough--;
        return event;
    }

    // Mission Control override relies on the legacy (macOS 13-26) event format
    const bool missionControlOverride = missionControlOverrideEnabled && !iss_requires_event_augmentation();
    if (!swipeOverrideEnabled && !missionControlOverride) return event;

    if (type == kCGEventKeyDown || type == kCGEventKeyUp) {
        if (!missionControlOverride ||
            CGEventGetIntegerValueField(event, kCGKeyboardEventKeycode) != kMissionControlKeyCode) {
            return event;
        }
        if (type == kCGEventKeyDown && !CGEventGetIntegerValueField(event, kCGKeyboardEventAutorepeat)) {
            // Without a real swipe to copy, close if Mission Control looks open, otherwise open
            const double sign = mission_control_on_screen() ? -kMissionControlOpenSign : kMissionControlOpenSign;
            missionControlOpenedByUs = sign == kMissionControlOpenSign;
            iss_enqueue_slide(kCGGestureMotionVertical, sign, slideDurationMs);
        }
        return NULL;
    }

    if (type == kCGEventLeftMouseDown || type == kCGEventLeftMouseUp) {
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
        if (!iss_requires_event_augmentation()) {
            if (CGEventGetIntegerValueField(event, kCGEventGesturePhase) == kCGSGesturePhaseBegan) {
                followInMissionControl = mission_control_on_screen();
                os_log(dbg(), "h-swipe began inMC=%{public}d", followInMissionControl); // TEMP debug
            }
            // Inside Mission Control the view follows the fingers in either mode;
            // finish it on release like follow mode so a small swipe is snappy
            if (followFingersEnabled || followInMissionControl) {
                return follow_fingers_swipe(event);
            }
        }

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

    // Suppress companion gesture events during active swipe tracking
    if (eventType == kCGSEventGesture && (swipeTracking || verticalTracking)) {
        return NULL;
    }

    return event;
}

static bool cgs_symbols_available(void) {
    return (&CGSMainConnectionID != NULL) &&
           (&CGSGetActiveSpace != NULL) &&
           (&CGSCopyManagedDisplaySpaces != NULL);
}

static bool extract_space_info_from_display(CFDictionaryRef displayDict,
                                            CGSSpaceID activeSpace,
                                            bool hasActiveSpace,
                                            ISSSpaceInfo *outInfo) {
    if (!displayDict || !outInfo) {
        return false;
    }

    memset(outInfo->displayID, 0, sizeof(outInfo->displayID));
    CFStringRef identifier = (CFStringRef)CFDictionaryGetValue(displayDict, CFSTR("Display Identifier"));
    if (identifier && CFGetTypeID(identifier) == CFStringGetTypeID()) {
        CFStringGetCString(identifier, outInfo->displayID, sizeof(outInfo->displayID), kCFStringEncodingUTF8);
    }

    const void *spacesValue = CFDictionaryGetValue(displayDict, CFSTR("Spaces"));
    if (!spacesValue || CFGetTypeID(spacesValue) != CFArrayGetTypeID()) {
        return false;
    }

    // Try to get current space from display dict (more accurate per-display)
    CGSSpaceID displayActiveSpace = 0;
    const void *currentSpaceValue = CFDictionaryGetValue(displayDict, CFSTR("Current Space"));
    if (currentSpaceValue && CFGetTypeID(currentSpaceValue) == CFDictionaryGetTypeID()) {
        CFDictionaryRef currentSpaceDict = (CFDictionaryRef)currentSpaceValue;
        CFNumberRef currentSpaceID = (CFNumberRef)CFDictionaryGetValue(currentSpaceDict, CFSTR("id64"));
        if (currentSpaceID && CFGetTypeID(currentSpaceID) == CFNumberGetTypeID()) {
            CFNumberGetValue(currentSpaceID, kCFNumberSInt64Type, &displayActiveSpace);
        }
    }
    
    // Use display-specific active space if available, otherwise use global
    CGSSpaceID targetActiveSpace = displayActiveSpace != 0 ? displayActiveSpace : activeSpace;
    bool hasTargetActiveSpace = displayActiveSpace != 0 || hasActiveSpace;

    CFArrayRef spaces = (CFArrayRef)spacesValue;
    const CFIndex spaceCount = CFArrayGetCount(spaces);

    unsigned int totalSpaces = 0;
    unsigned int activeIndex = 0;
    bool foundActive = false;

    for (CFIndex i = 0; i < spaceCount; i++) {
        const void *spaceValue = CFArrayGetValueAtIndex(spaces, i);
        if (!spaceValue || CFGetTypeID(spaceValue) != CFDictionaryGetTypeID()) {
            continue;
        }

        CFDictionaryRef spaceDict = (CFDictionaryRef)spaceValue;
        CFNumberRef idNumber = (CFNumberRef)CFDictionaryGetValue(spaceDict, CFSTR("id64"));
        if (!idNumber || CFGetTypeID(idNumber) != CFNumberGetTypeID()) {
            continue;
        }

        CGSSpaceID candidate = 0;
        if (CFNumberGetValue(idNumber, kCFNumberSInt64Type, &candidate)) {
            if (!foundActive && hasTargetActiveSpace && candidate == targetActiveSpace) {
                activeIndex = totalSpaces;
                foundActive = true;
            }
            totalSpaces++;
        }
    }

    if (totalSpaces == 0 || (hasTargetActiveSpace && !foundActive)) {
        return false;
    }

    outInfo->spaceCount = totalSpaces;
    outInfo->currentIndex = foundActive ? activeIndex : 0;
    return true;
}

static bool load_space_info_for_display(ISSSpaceInfo *info, bool useCursorDisplay) {
    if (!cgs_symbols_available()) {
        fprintf(stderr, "ISS: required CGS symbols missing\n");
        return false;
    }

    CGSConnectionID connection = CGSMainConnectionID();
    if (connection == 0) {
        fprintf(stderr, "ISS: CGSMainConnectionID returned 0\n");
        return false;
    }

    CGSSpaceID activeSpace = 0;
    bool hasActiveSpace = false;
    if (&CGSGetActiveSpace != NULL) {
        activeSpace = CGSGetActiveSpace(connection);
        if (activeSpace != 0) {
            hasActiveSpace = true;
        } else {
            fprintf(stderr, "ISS: CGSGetActiveSpace returned 0\n");
            return false;
        }
    }

    // Get display identifier based on mode
    CFStringRef activeDisplayIdentifier = NULL;
    
    if (useCursorDisplay) {
        // Get display where cursor is located
        CGEventRef tempEvent = CGEventCreate(NULL);
        if (tempEvent) {
            CGPoint cursorLocation = CGEventGetLocation(tempEvent);
            CFRelease(tempEvent);

            CGDirectDisplayID cursorDisplay = 0;
            uint32_t cursorDisplayCount = 0;

            if (CGGetDisplaysWithPoint(cursorLocation, 1, &cursorDisplay, &cursorDisplayCount) == kCGErrorSuccess && cursorDisplayCount > 0) {
                CFUUIDRef displayUUID = CGDisplayCreateUUIDFromDisplayID(cursorDisplay);
                if (displayUUID) {
                    activeDisplayIdentifier = CFUUIDCreateString(NULL, displayUUID);
                    CFRelease(displayUUID);
                }
            }
        }
    } else {
        // Get menubar display
        if (&CGSCopyActiveMenuBarDisplayIdentifier != NULL) {
            activeDisplayIdentifier = CGSCopyActiveMenuBarDisplayIdentifier(connection);
        }
    }

    CFArrayRef displays = CGSCopyManagedDisplaySpaces(connection, activeDisplayIdentifier);
    if (!displays && activeDisplayIdentifier) {
        displays = CGSCopyManagedDisplaySpaces(connection, NULL);
    }
    if (!displays) {
        if (activeDisplayIdentifier) {
            CFRelease(activeDisplayIdentifier);
        }
        return false;
    }

    const CFIndex displayCount = CFArrayGetCount(displays);
    CFDictionaryRef targetDisplay = NULL;
    CFDictionaryRef fallbackDisplay = NULL;

    for (CFIndex i = 0; i < displayCount; i++) {
        const void *displayValue = CFArrayGetValueAtIndex(displays, i);
        if (!displayValue || CFGetTypeID(displayValue) != CFDictionaryGetTypeID()) {
            continue;
        }

        CFDictionaryRef displayDict = (CFDictionaryRef)displayValue;

        if (!fallbackDisplay) {
            fallbackDisplay = displayDict;
        }

        if (!activeDisplayIdentifier || targetDisplay) {
            continue;
        }

        CFStringRef identifier = (CFStringRef)CFDictionaryGetValue(displayDict, CFSTR("Display Identifier"));
        if (identifier && CFGetTypeID(identifier) == CFStringGetTypeID() && CFEqual(identifier, activeDisplayIdentifier)) {
            targetDisplay = displayDict;
        }
    }

    if (!targetDisplay) {
        targetDisplay = fallbackDisplay;
    }

    bool success = false;
    if (targetDisplay) {
        success = extract_space_info_from_display(targetDisplay, activeSpace, hasActiveSpace, info);
    }

    if (activeDisplayIdentifier) {
        CFRelease(activeDisplayIdentifier);
    }
    CFRelease(displays);

    return success;
}

static bool iss_should_block_switch(const ISSSpaceInfo *info, ISSDirection direction) {
    if (!info) {
        return false;
    }
    if (info->spaceCount == 0) {
        return true;
    }

    unsigned int predicted;
    unsigned int current = get_prediction(info->displayID, &predicted) ? predicted : info->currentIndex;

    if (direction == ISSDirectionLeft) {
        return current == 0;
    }

    return current + 1 >= info->spaceCount;
}

bool iss_can_move(ISSSpaceInfo info, ISSDirection direction) {
    return !iss_should_block_switch(&info, direction);
}

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

static bool iss_post_modern_dock_swipe(CGSGesturePhase phase, ISSDirection direction) {
    const double sign = iss_modern_swipe_sign(direction);

    CGEventRef ev = CGEventCreate(NULL);
    if (!ev) {
        return false;
    }
    CGEventSetIntegerValueField(ev, kCGSEventTypeField, kCGSEventDockControl);
    CGEventSetIntegerValueField(ev, kCGEventGestureHIDType, kIOHIDEventTypeDockSwipe);
    CGEventSetIntegerValueField(ev, kCGEventGesturePhase, phase);
    CGEventSetIntegerValueField(ev, kCGEventGestureSwipeMotion, kCGGestureMotionHorizontal);
    // Near-zero progress (the smallest non-zero 16.16 value) leaves nothing to animate.
    // Full progress switches too, but the previous space flashes before it lands.
    CGEventSetDoubleValueField(ev, kCGEventGestureSwipeProgress, sign * 0.000016);
    // Fields a real trackpad swipe carries (from FasterSwiper's traces).
    CGEventSetIntegerValueField(ev, kCGEventGesturePhaseAlias, phase);
    CGEventSetDoubleValueField(ev, kCGEventGestureZoomDeltaY, 3.0);
    CGEventSetDoubleValueField(ev, kCGEventSourceUnixProcessIDAlias, (double)mach_absolute_time());
    CGEventSetDoubleValueField(ev, kCGEventGestureSwipePositionX, 0.1);
    // Match FasterSwiper: only the Ended event carries velocity.
    if (phase == kCGSGesturePhaseEnded) {
        CGEventSetDoubleValueField(ev, kCGEventGestureSwipeVelocityX, sign * kModernFlingVelocity);
    }

    CGEventRef augmented = iss_augment_dock_swipe_event(ev);
    CFRelease(ev);
    if (!augmented) {
        return false;
    }

    // The Dock ignores a DockControl event without a companion gesture event
    CGEventRef companion = CGEventCreate(NULL);
    if (!companion) {
        CFRelease(augmented);
        return false;
    }
    CGEventSetIntegerValueField(companion, kCGSEventTypeField, kCGSEventGesture);

    if (globalTap) {
        syntheticEventsToPassThrough += 2;
    }
    CGEventPost(kCGSessionEventTap, augmented);
    CGEventPost(kCGSessionEventTap, companion);
    CFRelease(augmented);
    CFRelease(companion);
    return true;
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

static bool iss_post_dock_swipe(CGSGesturePhase phase, ISSDirection direction, double velocity) {
    if (iss_requires_event_augmentation()) {
        return iss_post_modern_dock_swipe(phase, direction);
    }
    const double progress = iss_swipe_progress_for_phase(phase, direction);
    return iss_post_legacy_dock_swipe(phase, kCGGestureMotionHorizontal, progress > 0.0 ? 1.0 : -1.0,
                                      velocity, fabs(progress));
}

// Roughly one frame on a 120Hz display
static const useconds_t kSlideFrameMicroseconds = 8333;

// Smallest non-zero progress; keeps the direction without visible travel
static const double kMinimalProgress = 0.000016;

typedef struct {
    CGGestureMotion motion;
    bool postBegan;   // false when continuing a real gesture the Dock is already tracking
    double from;      // signed progress the slide starts at
    double to;        // signed progress the slide ends at (0 = back where it started)
    double velocity;  // signed release velocity
    double durationMs;
} ISSSlideRequest;

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
        iss_post_legacy_dock_swipe(kCGSGesturePhaseBegan, request->motion, 1.0, request->velocity,
                                   copysign(kMinimalProgress, direction));
    }
    for (unsigned int i = 1; i <= frames; i++) {
        usleep(kSlideFrameMicroseconds);
        const double progress = request->from + (request->to - request->from) * i / frames;
        iss_post_legacy_dock_swipe(kCGSGesturePhaseChanged, request->motion, 1.0, request->velocity,
                                   progress != 0.0 ? progress : copysign(kMinimalProgress, direction));
    }
    iss_post_legacy_dock_swipe(kCGSGesturePhaseEnded, request->motion, 1.0, request->velocity,
                               request->to != 0.0 ? request->to : copysign(kMinimalProgress, direction));

    free(request);
    atomic_fetch_sub(&pendingSlides, 1);
}

static bool iss_enqueue_slide_request(ISSSlideRequest request) {
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

static bool iss_enqueue_slide(CGGestureMotion motion, double sign, double durationMs) {
    return iss_enqueue_slide_request((ISSSlideRequest){
        .motion = motion,
        .postBegan = true,
        .from = 0.0,
        .to = sign,
        .velocity = sign * kSlideReleaseVelocity,
        .durationMs = durationMs,
    });
}

/** @brief Finishes a real horizontal swipe the user has just released.
 *
 * The Dock tracked the fingers up to `progress`; slide the rest of the way
 * toward the last direction of movement (a full switch, or back to the current
 * space) at the configured speed, then release, so there's no spring tail.
 */
static void iss_finish_followed_swipe(double progress, double lastDirection) {
    double target = 0.0;
    const bool movingAway = lastDirection != 0.0 && (progress == 0.0 || (lastDirection > 0.0) == (progress > 0.0));
    if (movingAway && followInMissionControl) {
        // Browsing Mission Control doesn't switch spaces; the Dock stops at the ends itself
        target = lastDirection > 0.0 ? 1.0 : -1.0;
    } else if (movingAway) {
        // Moving away from the current space (or from rest) commits to the space on that side
        ISSDirection dir = iss_direction_from_gesture_value(lastDirection);
        ISSSpaceInfo info;
        const bool hasInfo = iss_get_space_info(&info);
        // Past the first/last space there's nothing to switch to; snap back instead
        if (!hasInfo || !iss_should_block_switch(&info, dir)) {
            target = lastDirection > 0.0 ? 1.0 : -1.0;
            if (hasInfo) {
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
    iss_enqueue_slide_request((ISSSlideRequest){
        .motion = kCGGestureMotionHorizontal,
        .postBegan = false,
        .from = progress,
        .to = target,
        .velocity = target * kSlideReleaseVelocity,
        .durationMs = durationMs,
    });
}

/** @brief Returns the Spaces bar position of the button at `where`, or -1.
 *
 * In Mission Control the Dock exposes AXGroup "Spaces Bar" > AXList > one
 * AXButton per space, in order. Runs on clickQueue: the Dock can be slow to
 * answer while Mission Control animates, and the tap must never wait on it.
 */
static long spaces_bar_index_at(CGPoint where) {
    AXUIElementRef systemWide = AXUIElementCreateSystemWide();
    if (!systemWide) return -1;
    AXUIElementSetMessagingTimeout(systemWide, 0.25f);

    long index = -1;
    AXUIElementRef button = NULL;
    AXUIElementRef list = NULL;
    AXUIElementRef group = NULL;
    CFTypeRef value = NULL;
    if (AXUIElementCopyElementAtPosition(systemWide, (float)where.x, (float)where.y, &button) != kAXErrorSuccess) {
        button = NULL;
        goto done;
    }
    if (AXUIElementCopyAttributeValue(button, kAXRoleAttribute, &value) != kAXErrorSuccess) goto done;
    bool isButton = CFEqual(value, kAXButtonRole);
    CFRelease(value);
    if (!isButton) goto done;

    if (AXUIElementCopyAttributeValue(button, kAXParentAttribute, (CFTypeRef *)&list) != kAXErrorSuccess) {
        list = NULL;
        goto done;
    }
    if (AXUIElementCopyAttributeValue(list, kAXParentAttribute, (CFTypeRef *)&group) != kAXErrorSuccess) {
        group = NULL;
        goto done;
    }
    if (AXUIElementCopyAttributeValue(group, kAXTitleAttribute, &value) != kAXErrorSuccess) goto done;
    bool isSpacesBar = CFGetTypeID(value) == CFStringGetTypeID() && CFEqual(value, CFSTR("Spaces Bar"));
    CFRelease(value);
    if (!isSpacesBar) goto done;

    CFArrayRef children = NULL;
    if (AXUIElementCopyAttributeValue(list, kAXChildrenAttribute, (CFTypeRef *)&children) != kAXErrorSuccess) goto done;
    for (CFIndex i = 0; i < CFArrayGetCount(children); i++) {
        if (CFEqual(CFArrayGetValueAtIndex(children, i), button)) {
            index = (long)i;
            break;
        }
    }
    CFRelease(children);

done:
    if (group) CFRelease(group);
    if (list) CFRelease(list);
    if (button) CFRelease(button);
    CFRelease(systemWide);
    return index;
}

static void replay_click(CGEventRef event) {
    CGEventSetIntegerValueField(event, kCGEventSourceUserData, kReplayedClickMarker);
    CGEventPost(kCGHIDEventTap, event);
}

static void release_held_clicks(bool replay) {
    if (heldMouseDown) {
        if (replay) replay_click(heldMouseDown);
        CFRelease(heldMouseDown);
        heldMouseDown = NULL;
    }
    if (heldMouseUp) {
        if (replay) replay_click(heldMouseUp);
        CFRelease(heldMouseUp);
        heldMouseUp = NULL;
    }
    clickHeld = false;
    clickResolved = false;
    clickedSpaceIndex = -1;
}

static void switch_to_clicked_space(void *context) {
    ISSSpaceInfo info; // TEMP debug
    if (iss_get_space_info(&info)) os_log(dbg(), "click: switching %{public}u -> %{public}lu", info.currentIndex, (unsigned long)(uintptr_t)context); // TEMP debug
    // Cut straight to the clicked space; a second slide right after the close
    // reads as two separate moves
    const double savedDurationMs = slideDurationMs;
    slideDurationMs = 0.0;
    iss_switch_to_index((unsigned int)(uintptr_t)context);
    slideDurationMs = savedDurationMs;
}

// How long to wait for Mission Control to finish closing before switching
static const int kCloseWaitPolls = 60;
static const useconds_t kCloseWaitPollMicroseconds = 10000;

static void after_close_slide(void *context) {
    // A slide posted while Mission Control is still on screen moves its view
    // instead (and wraps to the first space), so wait until it's really gone
    int polls = 0;
    while (mission_control_on_screen()) {
        if (++polls >= kCloseWaitPolls) {
            os_log(dbg(), "click: MC still open after wait, not switching"); // TEMP debug
            return;
        }
        usleep(kCloseWaitPollMicroseconds);
    }
    os_log(dbg(), "click: MC closed after %{public}d polls", polls); // TEMP debug
    // Back on the main queue, where the space predictions live
    dispatch_async_f(dispatch_get_main_queue(), context, switch_to_clicked_space);
}

// Control-Left/Right arrow: macOS's "Move left/right a space" shortcuts
static const CGKeyCode kLeftArrowKeyCode = 123;
static const CGKeyCode kRightArrowKeyCode = 124;
// How long to wait for Mission Control's selection to reach the clicked space
static const int kSelectionWaitPolls = 40;

typedef struct {
    unsigned int target;
    unsigned int steps;
    CGKeyCode key;
} ISSSelectionMove;

static void post_space_shortcut(CGKeyCode key) {
    for (int down = 1; down >= 0; down--) {
        CGEventRef event = CGEventCreateKeyboardEvent(NULL, key, down);
        if (!event) return;
        CGEventSetFlags(event, kCGEventFlagMaskControl | kCGEventFlagMaskSecondaryFn | kCGEventFlagMaskNumericPad);
        CGEventPost(kCGHIDEventTap, event);
        CFRelease(event);
    }
}

/** @brief Moves Mission Control's selection to the clicked space before it closes.
 *
 * Mission Control closes onto the selected space, so closing first shows the
 * old space for a moment before the switch. While a full-screen space is
 * selected the main area keeps showing the desktop, so moving the selection
 * only moves the highlight in the Spaces bar. Runs on slideQueue, ahead of
 * the close.
 */
static void move_mission_control_selection(void *context) {
    ISSSelectionMove *move = context;
    for (unsigned int i = 0; i < move->steps; i++) {
        post_space_shortcut(move->key);
    }
    ISSSpaceInfo info;
    int polls = 0;
    while (polls++ < kSelectionWaitPolls) {
        if (iss_get_space_info(&info) && info.currentIndex == move->target) break;
        usleep(kCloseWaitPollMicroseconds);
    }
    os_log(dbg(), "click: selection moved=%{public}d after %{public}d polls", info.currentIndex == move->target, polls); // TEMP debug
    free(move);
}

/// Closes Mission Control onto the clicked space
static void open_clicked_space(long index) {
    missionControlOpenedByUs = false;
    ISSSpaceInfo info;
    if (iss_get_space_info(&info) && info.currentIndex != (unsigned int)index) {
        ISSSelectionMove *move = malloc(sizeof(*move));
        if (move) {
            const bool right = (unsigned int)index > info.currentIndex;
            move->target = (unsigned int)index;
            move->steps = right ? (unsigned int)index - info.currentIndex : info.currentIndex - (unsigned int)index;
            move->key = right ? kRightArrowKeyCode : kLeftArrowKeyCode;
            dispatch_async_f(slideQueue, move, move_mission_control_selection);
        }
    }
    // Instant: Mission Control closes onto the space it shows, not the clicked
    // one, so any close animation shows the wrong space first
    iss_enqueue_slide(kCGGestureMotionVertical, -kMissionControlOpenSign, 0.0);
    // Queued behind the close, so the switch starts from the space it landed on
    dispatch_async_f(slideQueue, (void *)(uintptr_t)index, after_close_slide);
}

static bool click_moved(void) {
    if (!heldMouseDown || !heldMouseUp) return false;
    CGPoint down = CGEventGetLocation(heldMouseDown);
    CGPoint up = CGEventGetLocation(heldMouseUp);
    return fabs(up.x - down.x) > kClickSlop || fabs(up.y - down.y) > kClickSlop;
}

/// Settles the held click once both its lookup and its mouse up are in
static void settle_held_click(void) {
    if (!clickResolved) return;
    if (clickedSpaceIndex < 0) {
        // Not a space: hand the click to the Dock as it was
        release_held_clicks(true);
        return;
    }
    if (!heldMouseUp) return;
    if (click_moved()) {
        release_held_clicks(true);
        return;
    }
    const long index = clickedSpaceIndex;
    release_held_clicks(false);
    open_clicked_space(index);
}

static void click_lookup_finished(void *context) {
    const uintptr_t packed = (uintptr_t)context;
    // Low bits: generation, so a stale lookup can't settle a newer click
    if ((unsigned int)(packed & 0xFFFF) != (clickGeneration & 0xFFFF) || !clickHeld) return;
    clickResolved = true;
    clickedSpaceIndex = (long)(packed >> 16) - 1;
    os_log(dbg(), "click: lookup index=%{public}ld up=%{public}d", clickedSpaceIndex, heldMouseUp != NULL); // TEMP debug
    settle_held_click();
}

typedef struct {
    CGPoint where;
    unsigned int generation;
} ISSClickLookup;

static void run_click_lookup(void *context) {
    ISSClickLookup *lookup = context;
    const long index = spaces_bar_index_at(lookup->where);
    const uintptr_t packed = ((uintptr_t)(index + 1) << 16) | (lookup->generation & 0xFFFF);
    free(lookup);
    dispatch_async_f(dispatch_get_main_queue(), (void *)packed, click_lookup_finished);
}

static CGEventRef mission_control_click(CGEventType type, CGEventRef event) {
    if (CGEventGetIntegerValueField(event, kCGEventSourceUserData) == kReplayedClickMarker) return event;

    if (type == kCGEventLeftMouseUp) {
        if (!clickHeld) return event;
        heldMouseUp = CGEventCreateCopy(event);
        if (!heldMouseUp) {
            release_held_clicks(true);
            return event;
        }
        settle_held_click();
        return NULL;
    }

    // A click arriving while one is still held: give up on the old one
    if (clickHeld) release_held_clicks(true);
    if (!missionControlOpenedByUs) return event;
    if (!mission_control_on_screen()) {
        missionControlOpenedByUs = false;
        return event;
    }

    ISSClickLookup *lookup = malloc(sizeof(*lookup));
    heldMouseDown = CGEventCreateCopy(event);
    if (!lookup || !heldMouseDown) {
        free(lookup);
        if (heldMouseDown) CFRelease(heldMouseDown);
        heldMouseDown = NULL;
        return event;
    }
    clickHeld = true;
    clickResolved = false;
    clickGeneration++;
    lookup->where = CGEventGetLocation(event);
    lookup->generation = clickGeneration;
    if (!clickQueue) {
        clickQueue = dispatch_queue_create("iss.click", DISPATCH_QUEUE_SERIAL);
    }
    if (!slideQueue) {
        slideQueue = dispatch_queue_create("iss.slide", DISPATCH_QUEUE_SERIAL);
    }
    dispatch_async_f(clickQueue, lookup, run_click_lookup);
    return NULL;
}

static bool iss_perform_switch_gesture(ISSDirection direction, double velocity) {
    lastGestureTime = CFAbsoluteTimeGetCurrent();

    if (slideDurationMs > 0.0 && !iss_requires_event_augmentation()) {
        // Multi-space jumps split the duration so the whole trip takes about one slide
        const double durationMs = slideDurationMs * gestureSpeed / velocity;
        // Keep predictions authoritative until the queued slides have finished
        lastGestureTime += (double)(atomic_load(&pendingSlides) + 1) * durationMs / 1000.0;
        return iss_enqueue_slide(kCGGestureMotionHorizontal, direction == ISSDirectionRight ? 1.0 : -1.0,
                                 durationMs);
    }

    // Send three gesture events--began, changed, and ended
    // If we only send two then mission control doesn't work.
    return iss_post_dock_swipe(kCGSGesturePhaseBegan,   direction, velocity)
        && iss_post_dock_swipe(kCGSGesturePhaseChanged, direction, velocity)
        && iss_post_dock_swipe(kCGSGesturePhaseEnded,   direction, velocity);
}

/** @brief Walks a CGWindowListCopyWindowInfo result
 *
 * Used for trying to determine if Exposé or Mission Control is active.
 *
 * @param windowList The window list to scan
 * @param outLayer18Count The count of layer-18 windows
 * @param outLayer20Count The count of layer-20 windows
 */
static void scan_dock_window_list(CFArrayRef windowList,
                                  int *outLayer18Count,
                                  int *outLayer20Count) {
    *outLayer18Count = 0;
    *outLayer20Count = 0;
    CFIndex count = CFArrayGetCount(windowList);
    for (CFIndex i = 0; i < count; i++) {
        CFDictionaryRef info = (CFDictionaryRef)CFArrayGetValueAtIndex(windowList, i);
        CFStringRef owner = (CFStringRef)CFDictionaryGetValue(info, CFSTR("kCGWindowOwnerName"));
        if (!owner || !CFEqual(owner, CFSTR("Dock"))) continue;
        int layer = 0;
        CFNumberRef layerNum = (CFNumberRef)CFDictionaryGetValue(info, CFSTR("kCGWindowLayer"));
        if (layerNum) {
            CFNumberGetValue(layerNum, kCFNumberIntType, &layer);
        }
        if (layer == 18) {
            (*outLayer18Count)++;
            continue;
        }
        if (layer == 20) {
            (*outLayer20Count)++;
        }
    }
}

// Testable helpers
bool iss_is_expose_detected_in_window_list(CFArrayRef windowList) {
    int layer18Count = 0;
    int layer20Count = 0;
    scan_dock_window_list(windowList, &layer18Count, &layer20Count);
    // App Exposé: layer-18 present, at least one layer-20, AND count(layer=20) <= count(layer=18)
    return layer18Count > 0 && layer20Count > 0 && layer20Count <= layer18Count;
}

bool iss_is_mission_control_detected_in_window_list(CFArrayRef windowList) {
    int layer18Count = 0;
    int layer20Count = 0;
    scan_dock_window_list(windowList, &layer18Count, &layer20Count);
    // Mission Control: layer-18 present AND count(layer=20) > count(layer=18)
    return layer18Count > 0 && layer20Count > layer18Count;
}

/// Returns true when App Exposé is active (1-2 layer-20 windows)
/// This heuristic is empirical and may not work in all cases.
bool iss_is_expose_active(void) {
    if (!overlayDetectionEnabled) return false;
    CFArrayRef windowList = CGWindowListCopyWindowInfo(
        kCGWindowListOptionOnScreenOnly, kCGNullWindowID);
    if (!windowList) return false;
    bool result = iss_is_expose_detected_in_window_list(windowList);
    CFRelease(windowList);
    return result;
}

/// Mission Control check for the swipe overrides, which need it even when
/// the "detection" setting (used for the OSD) is off
static bool mission_control_on_screen(void) {
    CFArrayRef windowList = CGWindowListCopyWindowInfo(
        kCGWindowListOptionOnScreenOnly, kCGNullWindowID);
    if (!windowList) return false;
    bool result = iss_is_mission_control_detected_in_window_list(windowList);
    CFRelease(windowList);
    return result;
}

/// Returns true when Mission Control is active (3+ layer-20 windows)
/// This heuristic is empirical and may not work in all cases.
bool iss_is_mission_control_active(void) {
    if (!overlayDetectionEnabled) return false;
    CFArrayRef windowList = CGWindowListCopyWindowInfo(
        kCGWindowListOptionOnScreenOnly, kCGNullWindowID);
    if (!windowList) return false;
    bool result = iss_is_mission_control_detected_in_window_list(windowList);
    CFRelease(windowList);
    return result;
}

void iss_set_overlay_detection_enabled(bool enabled) {
    overlayDetectionEnabled = enabled;
}

bool iss_init(void) {
    if (globalTap) {
        return true;
    }

    if (!predictionsDict) {
        predictionsDict = CFDictionaryCreateMutable(NULL, 0, &kCFCopyStringDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    }

    CGEventMask mask = CGEventMaskBit(kCGEventKeyDown) | CGEventMaskBit(kCGEventKeyUp)
        | CGEventMaskBit(kCGEventLeftMouseDown) | CGEventMaskBit(kCGEventLeftMouseUp)
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

    naturalScrolling = -1;
    CFNotificationCenterAddObserver(CFNotificationCenterGetDistributedCenter(),
                                    &naturalScrolling, natural_scrolling_changed,
                                    CFSTR("SwipeScrollDirectionDidChangeNotification"),
                                    NULL, CFNotificationSuspensionBehaviorDeliverImmediately);

    return true;
}

void iss_destroy(void) {
    syntheticEventsToPassThrough = 0;
    release_held_clicks(false);
    if (predictionsDict) {
        CFRelease(predictionsDict);
        predictionsDict = NULL;
    }
    if (globalTap) {
        CFNotificationCenterRemoveObserver(CFNotificationCenterGetDistributedCenter(),
                                           &naturalScrolling,
                                           CFSTR("SwipeScrollDirectionDidChangeNotification"),
                                           NULL);
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

bool iss_get_space_info(ISSSpaceInfo *info) {
    if (!info) {
        return false;
    }

    memset(info, 0, sizeof(*info));
    return load_space_info_for_display(info, true);
}

bool iss_get_menubar_space_info(ISSSpaceInfo *info) {
    if (!info) {
        return false;
    }

    memset(info, 0, sizeof(*info));
    return load_space_info_for_display(info, false);
}

static bool iss_switch_with_info(const ISSSpaceInfo *info, ISSDirection direction) {
    if (iss_should_block_switch(info, direction)) {
        return false;
    }
    if (!iss_perform_switch_gesture(direction, gestureSpeed)) {
        return false;
    }

    return true;
}

bool iss_switch(ISSDirection direction) {
    ISSSpaceInfo info;
    if (iss_get_space_info(&info)) {
        unsigned int predicted;
        unsigned int current = get_prediction(info.displayID, &predicted) ? predicted : info.currentIndex;
        unsigned int target = direction == ISSDirectionLeft ? current - 1 : current + 1;

        if (!iss_switch_with_info(&info, direction)) {
            return false;
        }
        set_prediction(info.displayID, target);
        if (switchCallback) { switchCallback(target); }
        return true;
    }

    return iss_perform_switch_gesture(direction, gestureSpeed);
}

bool iss_switch_to_index(unsigned int targetIndex) {
    ISSSpaceInfo info;
    if (!iss_get_space_info(&info)) {
        return false;
    }

    assert(info.spaceCount > 0);

    bool outOfBounds = targetIndex >= info.spaceCount;
    if (outOfBounds) {
        targetIndex = info.spaceCount - 1;
    }

    unsigned int predicted;
    unsigned int currentIndex = get_prediction(info.displayID, &predicted) ? predicted : info.currentIndex;

    if (currentIndex == targetIndex) {
        return !outOfBounds;
    }

    ISSDirection direction = currentIndex < targetIndex ? ISSDirectionRight : ISSDirectionLeft;
    unsigned int steps = direction == ISSDirectionRight ? (targetIndex - currentIndex) : (currentIndex - targetIndex);

    // Multiply velocity by number of steps for faster multi-space switching
    double velocity = gestureSpeed * steps;

    for (unsigned int i = 0; i < steps; i++) {
        if (!iss_perform_switch_gesture(direction, velocity)) {
            return false;
        }
    }

    set_prediction(info.displayID, targetIndex);
    if (switchCallback) { switchCallback(targetIndex); }
    return !outOfBounds;
}

void iss_set_swipe_override(bool enabled) {
    swipeOverrideEnabled = enabled;
    if (!enabled) {
        swipeTracking = false;
        swipeFired = false;
    }
}

void iss_set_gesture_speed(double speed) {
    gestureSpeed = speed;
}

void iss_set_swipe_follow_fingers(bool enabled) {
    followFingersEnabled = enabled;
    followTracking = false;
}

void iss_set_mission_control_override(bool enabled) {
    missionControlOverrideEnabled = enabled;
    if (!enabled) {
        verticalTracking = false;
        verticalFired = false;
    }
}

void iss_set_mission_control_swipe_speed(double velocity) {
    missionControlSwipeSpeed = velocity > 0.0 ? velocity : ISS_DEFAULT_MISSION_CONTROL_SWIPE_SPEED;
}

void iss_set_slide_duration(double milliseconds) {
    slideDurationMs = milliseconds > 0.0 ? milliseconds : 0.0;
}

bool iss_is_animating(void) {
    return atomic_load(&pendingSlides) > 0;
}

void iss_reset_predictions(void) {
    // Space-change notifications fire for every intermediate hop of our own
    // gestures; ignore them until the burst has settled.
    if (gestures_in_flight()) return;
    if (predictionsDict) {
        CFDictionaryRemoveAllValues(predictionsDict);
    }
}

void iss_set_switch_callback(ISSSwitchCallback callback) {
    switchCallback = callback;
}
