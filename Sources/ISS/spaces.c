// Where we are: reads the desktops macOS knows about, keeps predictions of where
// queued switches will land, and turns a switch request into a dock swipe.

#include "iss_internal.h"

#include <assert.h>

// Gesture speed state
double gestureSpeed = 2000.0;

ISSSwitchCallback switchCallback = NULL;

// Predictions dictionary: DisplayID (CFStringRef) -> Index (CFNumberRef)
static CFMutableDictionaryRef predictionsDict = NULL;

// While our own gestures are still being animated, CGS reports an
// intermediate space and the Dock fires space-change notifications for each
// hop. Trusting either during a burst makes us overshoot past the last space,
// which on macOS 27 wedges the Dock (black screen) until it times out. Keep
// predictions authoritative until the Dock has been quiet for this long.
static const CFTimeInterval kPredictionTTL = 1.0;
CFAbsoluteTime lastGestureTime = 0;

static bool gestures_in_flight(void) {
    return CFAbsoluteTimeGetCurrent() - lastGestureTime < kPredictionTTL;
}

bool get_prediction(const char *displayID, unsigned int *outIndex) {
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

void set_prediction(const char *displayID, unsigned int index) {
    if (!displayID || !predictionsDict) {
        return;
    }
    
    CFStringRef key = CFStringCreateWithCString(NULL, displayID, kCFStringEncodingUTF8);
    CFNumberRef val = CFNumberCreate(NULL, kCFNumberIntType, &index);
    CFDictionarySetValue(predictionsDict, key, val);
    CFRelease(key);
    CFRelease(val);
}

void iss_create_predictions(void) {
    if (!predictionsDict) {
        predictionsDict = CFDictionaryCreateMutable(NULL, 0, &kCFCopyStringDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    }
}

void iss_free_predictions(void) {
    if (predictionsDict) {
        CFRelease(predictionsDict);
        predictionsDict = NULL;
    }
}

static bool extract_space_info_from_display(CFDictionaryRef displayDict,
                                            CGSSpaceID activeSpace,
                                            bool hasActiveSpace,
                                            ISSSpaceInfo *outInfo);
static bool load_space_info_for_display(ISSSpaceInfo *info, bool useCursorDisplay);
static bool iss_switch_with_info(const ISSSpaceInfo *info, ISSDirection direction);

ISSDirection iss_direction_from_gesture_value(double value) {
    // Map the intercepted gesture value to the app's logical direction.
    return value > 0.0 ? ISSDirectionRight : ISSDirectionLeft;
}

// Perform a swipe-override switch: get space info, compute target, switch,
// and notify the handler with the target index.
void swipe_override_switch(ISSDirection dir) {
    // On macOS 27 only the whole gesture landing at once works with fingers down
    const double savedDurationMs = slideDurationMs;
    if (iss_requires_event_augmentation()) slideDurationMs = 0.0;

    ISSSpaceInfo info;
    if (!iss_get_space_info(&info)) {
        iss_perform_switch_gesture(dir, gestureSpeed);
    } else {
        unsigned int predicted;
        unsigned int current = get_prediction(info.displayID, &predicted) ? predicted : info.currentIndex;
        unsigned int target = dir == ISSDirectionLeft ? current - 1 : current + 1;

        if (iss_switch_with_info(&info, dir)) {
            set_prediction(info.displayID, target);
            if (switchCallback) { switchCallback(target); }
        }
    }

    slideDurationMs = savedDurationMs;
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

bool iss_should_block_switch(const ISSSpaceInfo *info, ISSDirection direction) {
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

bool iss_perform_switch_gesture(ISSDirection direction, double velocity) {
    lastGestureTime = CFAbsoluteTimeGetCurrent();

    if (slideDurationMs > 0.0) {
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

void iss_set_gesture_speed(double speed) {
    gestureSpeed = speed;
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
