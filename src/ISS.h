#ifndef ISS_h
#define ISS_h

#include <stdbool.h>

/** @brief The direction to switch spaces towards */
typedef enum {
    ISSDirectionLeft = 0,
    ISSDirectionRight = 1
} ISSDirection;

/**
 * @brief Describes the current space state for the active display.
 */
typedef struct {
    unsigned int currentIndex; /**< Zero-based index of the active space */
    unsigned int spaceCount;   /**< Total number of user-visible spaces */
} ISSSpaceInfo;

/**
 * @brief Retrieves the current space info for the active menu-bar display.
 * @param info Output pointer that receives the info struct.
 * @return true on success, false if unavailable (e.g. API failure)
 */
bool iss_get_menubar_space_info(ISSSpaceInfo *info);

/**
 * @brief Attempts to switch directly to the provided space index.
 * @param targetIndex Zero-based index for the desired space.
 * @return true if the request succeeded (already on target or switches posted)
 */
bool iss_switch_to_index(unsigned int targetIndex);

/**
 * @brief Switches directly to the provided space index using a SkyLight
 *        transaction (SLSTransactionSetManagedDisplayCurrentSpace).
 *        No Dock gesture, no animation. Works on macOS 27 (Golden Gate),
 *        where synthetic dock-swipe gestures are blocked.
 * @param targetIndex Zero-based index for the desired space.
 * @return true if the switch was committed (or already on target)
 * @note On macOS 27 this desyncs WindowServer's space state (corrupts the
 *       menu bar). Prefer iss_switch_to_index_aug there.
 */
bool iss_switch_to_index_instant(unsigned int targetIndex);

/**
 * @brief Switches to the provided space index by posting augmented synthetic
 *        dock-swipe gestures — the only Dock-driven method that still works on
 *        macOS 27 (Golden Gate), which ignores gesture data set via the public
 *        CGEvent field API and reads it from a hidden IOHID struct embedded in
 *        the serialized event instead. Instant, Dock-driven (clean menu bar,
 *        empty spaces switch), no WindowServer desync.
 * @param targetIndex Zero-based index for the desired space.
 * @return true on success (or already on target)
 */
bool iss_switch_to_index_aug(unsigned int targetIndex);

#endif /* ISS_h */
