// Clicks on Mission Control's Spaces bar on macOS 26. Not active on macOS 27.2.
// The click is held while Accessibility finds what's under it, off the event tap
// because the Dock can be slow to answer, then replaced by a fast close and switch.

#include "iss_internal.h"

#include <float.h>

// Clicking a space in Mission Control's Spaces bar: the Dock's zoom out is
// slow, so the click is held, looked up off the tap, and replaced by our fast
// close and slide. Only checked after our override opened Mission Control.
bool missionControlOpenedByUs = false;
static bool clickHeld = false;          // a mouse down is being looked up
static bool clickResolved = false;      // the lookup has finished for the held click
static bool clickUnconfirmed = false;   // the window check didn't see Mission Control
static long clickedSpaceIndex = -1;     // its result: Spaces bar position, or -1
static AXUIElementRef clickedThumbnail = NULL; // and the thumbnail's button
static CGEventRef heldMouseDown = NULL;
static CGEventRef heldMouseUp = NULL;
static unsigned int clickGeneration = 0;
static dispatch_queue_t clickQueue = NULL;
// Marks clicks we replay so the tap lets them through
static const int64_t kReplayedClickMarker = 0x1555C11C;
// A click that moved farther than this is a drag; leave it to the Dock
static const double kClickSlop = 6.0;

// How many levels above the hit element to look for the thumbnail's button
static const int kThumbnailButtonDepth = 3;

/// Appends "role 'title'" of `element` to `trail`, for the log
static void append_element_to_trail(AXUIElementRef element, char *trail, size_t size) {
    char role[48] = "?";
    char title[48] = "";
    CFTypeRef value = NULL;
    if (AXUIElementCopyAttributeValue(element, kAXRoleAttribute, &value) == kAXErrorSuccess && value) {
        if (CFGetTypeID(value) == CFStringGetTypeID()) CFStringGetCString(value, role, sizeof(role), kCFStringEncodingUTF8);
        CFRelease(value);
    }
    if (AXUIElementCopyAttributeValue(element, kAXTitleAttribute, &value) == kAXErrorSuccess && value) {
        if (CFGetTypeID(value) == CFStringGetTypeID()) CFStringGetCString(value, title, sizeof(title), kCFStringEncodingUTF8);
        CFRelease(value);
    }
    const size_t used = strlen(trail);
    snprintf(trail + used, size - used, "%s%s '%s'", used ? " < " : "", role, title);
}

static const char *const kOnBarReason = "on the bar, away from any thumbnail";
static const char *const kCloseButtonReason = "the close button on a thumbnail's corner";
// The close button sits centred on a thumbnail's top-left corner
static const double kCloseButtonRadius = 18.0;

// How far beside a thumbnail a click on the bar still counts as that thumbnail
static const double kThumbnailGapTolerance = 16.0;

/** @brief Returns the child of `list` under `where`, or the nearest one across, retained.
 *
 * Clicks in the gaps of the Spaces bar, or while it grows from labels into
 * thumbnails, hit the list itself; the Dock still opens the nearest space.
 */
static AXUIElementRef spaces_bar_child_near(AXUIElementRef list, CGPoint where) {
    CFArrayRef children = NULL;
    if (AXUIElementCopyAttributeValue(list, kAXChildrenAttribute, (CFTypeRef *)&children) != kAXErrorSuccess) return NULL;
    AXUIElementRef nearest = NULL;
    double nearestDistance = DBL_MAX;
    for (CFIndex i = 0; i < CFArrayGetCount(children); i++) {
        AXUIElementRef child = (AXUIElementRef)CFArrayGetValueAtIndex(children, i);
        CFTypeRef positionValue = NULL;
        CFTypeRef sizeValue = NULL;
        CGPoint position = CGPointZero;
        CGSize size = CGSizeZero;
        bool hasFrame = AXUIElementCopyAttributeValue(child, kAXPositionAttribute, &positionValue) == kAXErrorSuccess
            && AXUIElementCopyAttributeValue(child, kAXSizeAttribute, &sizeValue) == kAXErrorSuccess
            && AXValueGetValue(positionValue, kAXValueTypeCGPoint, &position)
            && AXValueGetValue(sizeValue, kAXValueTypeCGSize, &size);
        if (positionValue) CFRelease(positionValue);
        if (sizeValue) CFRelease(sizeValue);
        if (!hasFrame) continue;
        // Distance across only: the bar is one row
        const double distance = where.x < position.x ? position.x - where.x
            : where.x > position.x + size.width ? where.x - (position.x + size.width) : 0.0;
        if (distance < nearestDistance) {
            nearestDistance = distance;
            nearest = child;
        }
    }
    // Far from every thumbnail (the add-desktop button, say) isn't a thumbnail click
    if (nearestDistance > kThumbnailGapTolerance) nearest = NULL;
    if (nearest) CFRetain(nearest);
    CFRelease(children);
    return nearest;
}

/** @brief Returns the Spaces bar position of the button at `where`, or -1.
 *
 * In Mission Control the Dock exposes AXGroup "Spaces Bar" > AXList > one
 * AXButton per space, in order. Runs on clickQueue: the Dock can be slow to
 * answer while Mission Control animates, and the tap must never wait on it.
 */
static long spaces_bar_index_at(CGPoint where, const char **reason, AXError *error, AXUIElementRef *outButton,
                                char *trail, size_t trailSize) {
    *reason = "no system-wide element";
    *error = kAXErrorSuccess;
    *outButton = NULL;
    trail[0] = '\0';
    AXUIElementRef systemWide = AXUIElementCreateSystemWide();
    if (!systemWide) return -1;
    AXUIElementSetMessagingTimeout(systemWide, 0.25f);

    long index = -1;
    AXUIElementRef button = NULL;
    AXUIElementRef list = NULL;
    AXUIElementRef group = NULL;
    CFTypeRef value = NULL;
    *reason = "no element at the pointer";
    if ((*error = AXUIElementCopyElementAtPosition(systemWide, (float)where.x, (float)where.y, &button)) != kAXErrorSuccess) {
        button = NULL;
        goto done;
    }
    // A hit on the bar itself (between thumbnails): take the nearest thumbnail
    if (AXUIElementCopyAttributeValue(button, kAXRoleAttribute, &value) == kAXErrorSuccess) {
        const bool isList = CFEqual(value, kAXListRole);
        CFRelease(value);
        if (isList) {
            append_element_to_trail(button, trail, trailSize);
            AXUIElementRef child = spaces_bar_child_near(button, where);
            if (!child) {
                *reason = kOnBarReason;
                goto done;
            }
            CFRelease(button);
            button = child;
        }
    }

    // The hit can land on a part inside the thumbnail; walk up to its button
    bool isButton = false;
    for (int depth = 0; depth <= kThumbnailButtonDepth; depth++) {
        append_element_to_trail(button, trail, trailSize);
        *reason = "no role";
        if ((*error = AXUIElementCopyAttributeValue(button, kAXRoleAttribute, &value)) != kAXErrorSuccess) goto done;
        isButton = CFEqual(value, kAXButtonRole);
        CFRelease(value);
        if (isButton || depth == kThumbnailButtonDepth) break;
        AXUIElementRef parent = NULL;
        if (AXUIElementCopyAttributeValue(button, kAXParentAttribute, (CFTypeRef *)&parent) != kAXErrorSuccess) break;
        CFRelease(button);
        button = parent;
    }
    *reason = "not a button";
    if (!isButton) goto done;

    *reason = "no parent list";
    if ((*error = AXUIElementCopyAttributeValue(button, kAXParentAttribute, (CFTypeRef *)&list)) != kAXErrorSuccess) {
        list = NULL;
        goto done;
    }
    *reason = "no parent group";
    if ((*error = AXUIElementCopyAttributeValue(list, kAXParentAttribute, (CFTypeRef *)&group)) != kAXErrorSuccess) {
        group = NULL;
        goto done;
    }
    *reason = "no group title";
    if ((*error = AXUIElementCopyAttributeValue(group, kAXTitleAttribute, &value)) != kAXErrorSuccess) goto done;
    bool isSpacesBar = CFGetTypeID(value) == CFStringGetTypeID() && CFEqual(value, CFSTR("Spaces Bar"));
    CFRelease(value);
    *reason = "not in the Spaces bar";
    if (!isSpacesBar) goto done;

    *reason = "no list children";
    CFArrayRef children = NULL;
    if ((*error = AXUIElementCopyAttributeValue(list, kAXChildrenAttribute, (CFTypeRef *)&children)) != kAXErrorSuccess) goto done;
    *reason = "button not in its list";
    for (CFIndex i = 0; i < CFArrayGetCount(children); i++) {
        if (CFEqual(CFArrayGetValueAtIndex(children, i), button)) {
            index = (long)i;
            *reason = "desktop thumbnail";
            break;
        }
    }
    CFRelease(children);
    if (index >= 0) {
        // Where in the thumbnail the click landed, and what's inside it, for the log
        CFTypeRef positionValue = NULL;
        CGPoint origin = CGPointZero;
        if (AXUIElementCopyAttributeValue(button, kAXPositionAttribute, &positionValue) == kAXErrorSuccess) {
            AXValueGetValue(positionValue, kAXValueTypeCGPoint, &origin);
            CFRelease(positionValue);
        }
        CFIndex childCount = 0;
        AXUIElementGetAttributeValueCount(button, kAXChildrenAttribute, &childCount);
        const size_t used = strlen(trail);
        snprintf(trail + used, trailSize - used, " @ %.0f,%.0f in thumbnail, %ld children",
                 where.x - origin.x, where.y - origin.y, (long)childCount);
        // The close (x) button on the corner isn't its own element; leave it to the Dock
        if (hypot(where.x - origin.x, where.y - origin.y) <= kCloseButtonRadius) {
            index = -1;
            *reason = kCloseButtonReason;
        }
    }

done:
    if (group) CFRelease(group);
    if (list) CFRelease(list);
    if (button && index >= 0) {
        *outButton = button;
        button = NULL;
    }
    if (button) CFRelease(button);
    CFRelease(systemWide);
    return index;
}

static void replay_click(CGEventRef event) {
    CGEventSetIntegerValueField(event, kCGEventSourceUserData, kReplayedClickMarker);
    CGEventPost(kCGHIDEventTap, event);
}

void release_held_clicks(bool replay) {
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
    if (clickedThumbnail) {
        CFRelease(clickedThumbnail);
        clickedThumbnail = NULL;
    }
    clickHeld = false;
    clickResolved = false;
    clickedSpaceIndex = -1;
}

static void switch_to_clicked_space(void *context) {
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

typedef struct {
    long index;
    AXUIElementRef thumbnail;
} ISSClickedSpace;

/// Whether the clicked thumbnail's button still exists, i.e. Mission Control is still up
static bool thumbnail_on_screen(AXUIElementRef thumbnail) {
    if (!thumbnail) return false;
    AXUIElementSetMessagingTimeout(thumbnail, 0.05f);
    CFTypeRef value = NULL;
    const AXError error = AXUIElementCopyAttributeValue(thumbnail, kAXRoleAttribute, &value);
    if (value) CFRelease(value);
    return error == kAXErrorSuccess;
}

static void after_close_slide(void *context) {
    ISSClickedSpace *clicked = context;
    // A slide posted while Mission Control is still on screen moves its view
    // instead (and jumps to the last space), so wait until it's really gone.
    // The window check alone can miss Mission Control; the clicked thumbnail
    // disappearing confirms it.
    int polls = 0;
    for (;;) {
        // Check both every time: each can miss what the other sees
        const bool windowsUp = mission_control_on_screen();
        const bool thumbnailUp = thumbnail_on_screen(clicked->thumbnail);
        if (!windowsUp && !thumbnailUp) break;
        if (++polls >= kCloseWaitPolls) {
            os_log(click_log(), "Mission Control still on screen after the close (windows %{public}d, thumbnail %{public}d); not switching",
                   windowsUp, thumbnailUp);
            break;
        }
        usleep(kCloseWaitPollMicroseconds);
    }
    if (clicked->thumbnail) CFRelease(clicked->thumbnail);
    if (polls < kCloseWaitPolls) {
        os_log(click_log(), "Mission Control closed after %{public}d polls; switching", polls);
        // Back on the main queue, where the space predictions live
        dispatch_async_f(dispatch_get_main_queue(), (void *)(uintptr_t)clicked->index, switch_to_clicked_space);
    }
    free(clicked);
}

/// Closes Mission Control instantly, then cuts to the clicked space
static void open_clicked_space(long index, AXUIElementRef thumbnail) {
    ISSClickedSpace *clicked = malloc(sizeof(*clicked));
    if (!clicked) {
        if (thumbnail) CFRelease(thumbnail);
        return;
    }
    clicked->index = index;
    clicked->thumbnail = thumbnail;
    missionControlOpenedByUs = false;
    // Instant: Mission Control closes onto the space it shows, not the clicked
    // one, so any close animation shows the wrong space first
    iss_enqueue_slide(kCGGestureMotionVertical, -kMissionControlOpenSign, 0.0);
    // Queued behind the close, so the switch starts from the space it landed on
    dispatch_async_f(slideQueue, clicked, after_close_slide);
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
        os_log(click_log(), "NATIVE: click moved (a drag)");
        release_held_clicks(true);
        return;
    }
    const long index = clickedSpaceIndex;
    AXUIElementRef thumbnail = clickedThumbnail;
    clickedThumbnail = NULL;
    os_log(click_log(), "FAST: opening space %{public}ld", index);
    release_held_clicks(false);
    open_clicked_space(index, thumbnail);
}

typedef struct {
    CGPoint where;
    unsigned int generation;
    // Filled in by the lookup
    long index;
    const char *reason;
    AXError error;
    double elapsedMs;
    AXUIElementRef thumbnail;
    char trail[256];  // the elements under the pointer, for the log
} ISSClickLookup;

static void click_lookup_finished(void *context) {
    ISSClickLookup *lookup = context;
    os_log(click_log(), "lookup: %{public}s (index %{public}ld, AX error %{public}d) in %{public}.0f ms; hit %{public}s",
           lookup->reason, lookup->index, (int)lookup->error, lookup->elapsedMs, lookup->trail);
    // A stale lookup can't settle a newer click
    if (lookup->generation != clickGeneration || !clickHeld) {
        os_log(click_log(), "lookup arrived after its click was given up");
        if (lookup->thumbnail) CFRelease(lookup->thumbnail);
        free(lookup);
        return;
    }
    clickResolved = true;
    clickedSpaceIndex = lookup->index;
    clickedThumbnail = lookup->thumbnail;
    if (clickedSpaceIndex < 0) {
        os_log(click_log(), "NATIVE: %{public}s; hit %{public}s", lookup->reason, lookup->trail);
        // A click outside the Spaces bar (a window, empty space) closes Mission
        // Control, as does anything when neither check saw it open
        const bool onSpacesBar = strstr(lookup->trail, "'Spaces Bar'") || lookup->reason == kOnBarReason
            || lookup->reason == kCloseButtonReason;
        if (clickUnconfirmed || !onSpacesBar) missionControlOpenedByUs = false;
    }
    free(lookup);
    settle_held_click();
}

static void run_click_lookup(void *context) {
    ISSClickLookup *lookup = context;
    const CFAbsoluteTime start = CFAbsoluteTimeGetCurrent();
    lookup->index = spaces_bar_index_at(lookup->where, &lookup->reason, &lookup->error, &lookup->thumbnail,
                                                lookup->trail, sizeof(lookup->trail));
    lookup->elapsedMs = (CFAbsoluteTimeGetCurrent() - start) * 1000.0;
    dispatch_async_f(dispatch_get_main_queue(), lookup, click_lookup_finished);
}

CGEventRef mission_control_click(CGEventType type, CGEventRef event) {
    if (CGEventGetIntegerValueField(event, kCGEventSourceUserData) == kReplayedClickMarker) return event;

    if (type == kCGEventLeftMouseDragged) {
        if (!clickHeld || !heldMouseDown) return event;
        const CGPoint down = CGEventGetLocation(heldMouseDown);
        const CGPoint now = CGEventGetLocation(event);
        if (fabs(now.x - down.x) <= kClickSlop && fabs(now.y - down.y) <= kClickSlop) return NULL;
        // A drag (reordering spaces, moving windows): hand the press to the Dock
        // now, then this drag after it, so the Dock sees the whole gesture
        os_log(click_log(), "NATIVE: drag");
        release_held_clicks(true);
        CGEventRef drag = CGEventCreateCopy(event);
        if (!drag) return event;
        replay_click(drag);
        CFRelease(drag);
        return NULL;
    }

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
    if (clickHeld) {
        os_log(click_log(), "NATIVE: new click before the last one was looked up");
        release_held_clicks(true);
    }
    if (!missionControlOpenedByUs) return event;
    // The window check can miss a freshly opened Mission Control; we opened
    // it, so look up anyway (only a real Spaces bar button counts)
    clickUnconfirmed = !mission_control_on_screen();
    os_log(click_log(), "click after opening Mission Control, looking up%{public}s",
           clickUnconfirmed ? " (window check missed it)" : "");

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
