#ifndef EVENT_SERIALIZE_H
#define EVENT_SERIALIZE_H

#include <ApplicationServices/ApplicationServices.h>
#include <stdbool.h>

/**
 * @brief Augments a synthetic dock-swipe CGEvent with the raw IOHID payload
 * that macOS 27 requires to recognize synthetic trackpad swipe gestures.
 *
 * The returned event is retained and the caller is responsible for releasing it.
 *
 * @param event A synthetic dock-swipe CGEvent with the public gesture fields set.
 * @return A retained, augmented CGEventRef, or NULL on failure.
 */
CGEventRef iss_augment_dock_swipe_event(CGEventRef event);

/**
 * @brief Copies a dock-swipe event with both its fields and its embedded IOHID
 * payload set. The Dock reads the payload, and setting fields alone doesn't change it.
 *
 * The returned event is retained and the caller is responsible for releasing it.
 *
 * @param event A real or augmented dock-swipe event.
 * @param phase The gesture phase to set.
 * @param progress Progress, using the sign of the event's fields.
 * @param velocity Release velocity, using the sign of the event's fields.
 * @return A retained event, or NULL if the event has no payload or the copy failed.
 */
CGEventRef iss_edit_dock_swipe_event(CGEventRef event, uint8_t phase, double progress, double velocity);

/**
 * @brief Returns true when the running OS is macOS 27 or later.
 *
 * On these versions, synthetic dock-swipe events must carry the raw IOHID
 * payload created by iss_augment_dock_swipe_event() in order to be honored.
 *
 * For testing, set the environment variable ISS_FORCE_EVENT_AUGMENTATION=1
 * to enable augmentation on any version, or =0 to disable it.
 */
bool iss_requires_event_augmentation(void);

#endif /* EVENT_SERIALIZE_H */
