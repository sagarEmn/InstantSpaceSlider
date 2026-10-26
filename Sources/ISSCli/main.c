#include "../ISS/include/ISS.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <CoreFoundation/CoreFoundation.h>
#include <string.h>

// Defaults domain of the app bundle, so the CLI follows the settings slider
#define ISS_APP_BUNDLE_ID "com.sagarEmn.InstantSpaceSlider"

static void print_usage(const char *progName) {
    fprintf(stderr, "Usage: %s [--duration <ms>] [left|right|index <n>]\n", progName);
}

static double load_saved_slide_duration(void) {
    double duration = ISS_DEFAULT_SLIDE_DURATION_MS;
    CFPropertyListRef value = CFPreferencesCopyAppValue(CFSTR(ISS_SLIDE_DURATION_KEY),
                                                        CFSTR(ISS_APP_BUNDLE_ID));
    if (value) {
        if (CFGetTypeID(value) == CFNumberGetTypeID()) {
            double saved = 0.0;
            if (CFNumberGetValue((CFNumberRef)value, kCFNumberDoubleType, &saved) && saved >= 0.0) {
                duration = saved;
            }
        }
        CFRelease(value);
    }
    return duration;
}

int main(int argc, char **argv) {
    if (!iss_init()) {
        fprintf(stderr, "Failed to initialize ISS (event tap). Check accessibility and input monitoring permissions.\n");
        return 1;
    }

    double duration = load_saved_slide_duration();
    if (argc > 2 && (!strcmp(argv[1], "--duration") || !strcmp(argv[1], "-d"))) {
        char *endPtr = NULL;
        duration = strtod(argv[2], &endPtr);
        if (endPtr == argv[2] || duration < 0.0) {
            fprintf(stderr, "Duration must be a number of milliseconds (0 for instant).\n");
            iss_destroy();
            return 1;
        }
        // Drop the flag but keep the program name in argv[0]
        argv[2] = argv[0];
        argc -= 2;
        argv += 2;
    }
    iss_set_slide_duration(duration);

    ISSDirection direction = ISSDirectionLeft;
    bool useIndex = false;
    unsigned int targetIndex = 0;

    if (argc > 1) {
        if (!strcmp(argv[1], "right") || !strcmp(argv[1], "r") || !strcmp(argv[1], "1")) {
            direction = ISSDirectionRight;
        } else if (!strcmp(argv[1], "left") || !strcmp(argv[1], "l") || !strcmp(argv[1], "0")) {
            direction = ISSDirectionLeft;
        } else if (!strcmp(argv[1], "index") || !strcmp(argv[1], "i")) {
            if (argc < 3) {
                print_usage(argv[0]);
                iss_destroy();
                return 1;
            }
            char *endPtr = NULL;
            long parsed = strtol(argv[2], &endPtr, 10);
            if (endPtr == argv[2] || parsed < 1) {
                fprintf(stderr, "Index must be a positive integer.\n");
                iss_destroy();
                return 1;
            }
            useIndex = true;
            targetIndex = (unsigned int)(parsed - 1); // convert to zero-based
        } else {
            print_usage(argv[0]);
            iss_destroy();
            return 1;
        }
    }

    bool success = false;
    if (useIndex) {
        success = iss_switch_to_index(targetIndex);
    } else {
        success = iss_switch(direction);
    }

    if (!success) {
        fprintf(stderr, "Switch request failed. Check space bounds or permissions.\n");
        iss_destroy();
        return 1;
    }

    // Slides are posted in the background; keep the run loop (and event tap) alive until they finish
    while (iss_is_animating()) {
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.05, false);
    }
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.3, false);
    iss_destroy();
    return 0;
}
