#ifndef ISS_INTERNAL_H
#define ISS_INTERNAL_H

// Shared by the engine's own files. The app and CLI only see include/ISS.h.

#include "include/ISS.h"
#include "event_serialize.h"

#include <ApplicationServices/ApplicationServices.h>
#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CGEventTypes.h>
#include <dispatch/dispatch.h>
#include <math.h>
#include <os/log.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// MARK: - Private event fields and types

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

// Tags our macOS 27 swipes so the tap can tell them from real trackpad events
static const int64_t kSyntheticEventTag = 0x1551;

// Release at instant velocity regardless of the speed setting; a slow release brings back the spring tail
static const double kSlideReleaseVelocity = 2000.0;

// Smallest non-zero progress; keeps the direction without visible travel
static const double kMinimalProgress = 0.000016;

// Synthetic progress sign that opens Mission Control. The trackpad reports a
// swipe up as negative progress, but the Dock reads our posted sign the other way.
static const double kMissionControlOpenSign = 1.0;

// MARK: - event_tap.c

/// Mission Control log; watch it with
/// log stream --predicate 'subsystem == "com.sagarEmn.InstantSpaceSlider"'
os_log_t click_log(void);
/// Left/right swipe log: each step of a swipe and its fast finish, to find where one got stuck
os_log_t swipe_log(void);

// MARK: - spaces.c

extern double gestureSpeed;
extern ISSSwitchCallback switchCallback;
extern CFAbsoluteTime lastGestureTime;

void iss_create_predictions(void);
void iss_free_predictions(void);
bool get_prediction(const char *displayID, unsigned int *outIndex);
void set_prediction(const char *displayID, unsigned int index);
bool iss_should_block_switch(const ISSSpaceInfo *info, ISSDirection direction);
ISSDirection iss_direction_from_gesture_value(double value);
bool iss_perform_switch_gesture(ISSDirection direction, double velocity);
void swipe_override_switch(ISSDirection dir);

// MARK: - dock_swipe.c

typedef struct {
    CGGestureMotion motion;
    bool postBegan;   // false when continuing a real gesture the Dock is already tracking
    double from;      // signed progress the slide starts at
    double to;        // signed progress the slide ends at (0 = back where it started)
    double velocity;  // signed release velocity
    double durationMs;
    CGEventRef frameEvent;    // macOS 27 finish of a real swipe: the Changed each frame is edited from
    CGEventRef releaseEvent;  // and the held real release, which closes the gesture
} ISSSlideRequest;

extern double slideDurationMs;
extern dispatch_queue_t slideQueue;
extern atomic_uint pendingSlides;
extern atomic_uint finishActive;
extern atomic_bool finishInterrupted;

bool iss_post_tagged_dock_swipe(CGEventRef event);
bool iss_post_dock_swipe(CGSGesturePhase phase, ISSDirection direction, double velocity);
bool iss_enqueue_slide_request(ISSSlideRequest request);
bool iss_enqueue_slide(CGGestureMotion motion, double sign, double durationMs);
void iss_interrupt_finish(void);
void iss_wait_for_slides(void);
void iss_start_watching_natural_scrolling(void);
void iss_stop_watching_natural_scrolling(void);

// MARK: - swipe_horizontal.c

extern bool swipeOverrideEnabled;
extern bool swipeTracking;
extern bool followFingersEnabled;
extern bool followInMissionControl;
extern bool horizontalActive;

CGEventRef instant_swipe(CGEventRef event);
CGEventRef follow_fingers_swipe(CGEventRef event);
void iss_free_follow_state(void);

// MARK: - mission_control_swipe.c

extern bool missionControlOverrideEnabled;
extern bool verticalTracking;

CGEventRef vertical_swipe_override(CGEventRef event);

// MARK: - mission_control_click.c

extern bool missionControlOpenedByUs;

CGEventRef mission_control_click(CGEventType type, CGEventRef event);
void release_held_clicks(bool replay);

// MARK: - overlay_detection.c

bool mission_control_on_screen(void);

#endif /* ISS_INTERNAL_H */
