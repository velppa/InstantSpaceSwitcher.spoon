#include "ISS.h"

#include <ApplicationServices/ApplicationServices.h>
#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CGEventTypes.h>
#include <float.h>
#include <mach/mach_time.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const CGEventField kCGSEventTypeField = (CGEventField)55;
static const CGEventField kCGEventGestureHIDType = (CGEventField)110;
static const CGEventField kCGEventGestureScrollY = (CGEventField)119;
static const CGEventField kCGEventGestureSwipeMotion = (CGEventField)123;
static const CGEventField kCGEventGestureSwipeProgress = (CGEventField)124;
static const CGEventField kCGEventGestureSwipeVelocityX = (CGEventField)129;
static const CGEventField kCGEventGestureSwipeVelocityY = (CGEventField)130;
static const CGEventField kCGEventGesturePhase = (CGEventField)132;
static const CGEventField kCGEventScrollGestureFlagBits = (CGEventField)135;
static const CGEventField kCGEventGestureZoomDeltaX = (CGEventField)139;

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
};

typedef int32_t CGSConnectionID;
typedef uint64_t CGSSpaceID;

extern CFArrayRef CGSCopyManagedDisplaySpaces(CGSConnectionID connection, CFStringRef display) __attribute__((weak_import));
extern CFStringRef CGSCopyActiveMenuBarDisplayIdentifier(CGSConnectionID connection) __attribute__((weak_import));
extern CGSConnectionID CGSMainConnectionID(void) __attribute__((weak_import));
extern CGSSpaceID CGSGetActiveSpace(CGSConnectionID connection) __attribute__((weak_import));

// SkyLight transaction API — survives macOS 27 (Golden Gate), which blocks
// synthetic dock-swipe gestures. Same mechanism newer yabai uses.
extern CFTypeRef SLSTransactionCreate(CGSConnectionID connection) __attribute__((weak_import));
extern void SLSTransactionSetManagedDisplayCurrentSpace(CFTypeRef transaction, CFStringRef display, CGSSpaceID space) __attribute__((weak_import));
extern CFErrorRef SLSTransactionCommit(CFTypeRef transaction, int synchronous) __attribute__((weak_import));
// Promotes the active/front process to match the space we just switched to.
// Without it, the SLS switch leaves the front process stale and the menu bar
// composites multiple spaces' menus. Returns a CGError (0 = success).
extern int SLSEnsureSpaceSwitchToActiveProcess(CGSConnectionID connection) __attribute__((weak_import));

static bool extract_space_info_from_display(CFDictionaryRef displayDict,
                                            CGSSpaceID activeSpace,
                                            bool hasActiveSpace,
                                            ISSSpaceInfo *outInfo);
static bool load_space_info_for_display(ISSSpaceInfo *info, bool useCursorDisplay);
static bool iss_post_switch_gesture(ISSDirection direction);
static bool iss_should_block_switch(const ISSSpaceInfo *info, ISSDirection direction);
static bool iss_post_switch_gesture_aug(ISSDirection direction);

// --- macOS 27 (Golden Gate) augmented dock-swipe gesture ---
//
// On macOS 27 the gesture data the system actually reads no longer comes from
// the public CGEvent fields; it lives in a hidden IOHID queue-element struct
// embedded in the *serialized* CGEvent as field 4205. Synthetic gestures built
// only with CGEventSetIntegerValueField are silently dropped. To switch:
// build the dock-control event, serialize with CGEventCreateData, append the
// IOHID blob as field 4205, rebuild with CGEventCreateFromData, then post.
// Technique credit: mgbowen/FasterSwiper (macos27 branch).
//
// Floats in the hidden struct are signed 16.16 fixed-point. Direction is
// inverted vs the legacy path: increasing the space index needs *negative*
// progress/velocity (determined empirically on 27.0).

static const CGEventField kCGEventGestureSwipePositionX = (CGEventField)125;
static const CGEventField kCGEventGestureSwipePositionY = (CGEventField)126;

static const uint32_t kISSHIDEventTypeVelocity = 9;
static const uint32_t kISSHIDEventTypeFluidTouch = 23;
static const uint16_t kISSHIDGestureFlavorDockPrimary = 3;

typedef int32_t ISSFixed1616;

#pragma pack(push, 1)
typedef struct {
    uint64_t timestamp;
    uint64_t sender_id;
    uint32_t options;
    uint32_t attribute_length;
    uint32_t event_count;
} ISSHIDQueueElement;
typedef struct {
    uint32_t size;
    uint32_t type;
    uint32_t options;
    uint8_t depth;
    uint8_t reserved[3];
} ISSHIDEventBase;
typedef struct {
    ISSHIDEventBase base;
    ISSFixed1616 position_x, position_y, position_z;
    uint32_t swipe_mask;
    uint16_t gesture_motion, gesture_flavor;
    ISSFixed1616 swipe_progress;
} ISSHIDFluidTouch;
typedef struct {
    ISSHIDEventBase base;
    ISSFixed1616 velocity_x, velocity_y, velocity_z;
} ISSHIDVelocity;
#pragma pack(pop)

static ISSFixed1616 iss_to_fixed1616(double value) {
    ISSFixed1616 fixed = (ISSFixed1616)(value * 65536.0);
    if (fixed == 0 && value != 0.0) {
        fixed = value > 0 ? 1 : -1;  // avoid truncating tiny magnitudes to 0
    }
    return fixed;
}

static void iss_put_be16(uint8_t **cursor, uint16_t value) {
    (*cursor)[0] = (uint8_t)(value >> 8);
    (*cursor)[1] = (uint8_t)(value & 0xFF);
    *cursor += 2;
}

static bool iss_post_augmented_gesture(int phase, double progress, double velocityX) {
    CGEventRef ev = CGEventCreate(NULL);
    if (!ev) {
        return false;
    }
    CGEventSetIntegerValueField(ev, kCGSEventTypeField, kCGSEventDockControl);
    CGEventSetIntegerValueField(ev, kCGEventGestureHIDType, kIOHIDEventTypeDockSwipe);
    CGEventSetIntegerValueField(ev, kCGEventGesturePhase, phase);
    CGEventSetIntegerValueField(ev, kCGEventGestureSwipeMotion, kCGGestureMotionHorizontal);
    CGEventSetDoubleValueField(ev, kCGEventGestureSwipeProgress, progress);
    CGEventSetDoubleValueField(ev, kCGEventGestureSwipeVelocityX, velocityX);
    CGEventSetDoubleValueField(ev, kCGEventGestureSwipeVelocityY, 0);
    CGEventSetDoubleValueField(ev, kCGEventGestureSwipePositionX, 0);
    CGEventSetDoubleValueField(ev, kCGEventGestureSwipePositionY, 0);

    // Build the hidden IOHID queue element: header + fluid-touch (+ velocity).
    uint8_t blob[sizeof(ISSHIDQueueElement) + sizeof(ISSHIDFluidTouch) + sizeof(ISSHIDVelocity)];
    uint8_t *write = blob + sizeof(ISSHIDQueueElement);
    bool hasVelocity = (velocityX != 0.0) || (phase == (int)kCGSGesturePhaseEnded);

    ISSHIDFluidTouch fluid;
    memset(&fluid, 0, sizeof fluid);
    fluid.base.size = sizeof(ISSHIDFluidTouch);
    fluid.base.type = kISSHIDEventTypeFluidTouch;
    fluid.base.options = (uint32_t)((phase & 0xFF) << 24);
    fluid.gesture_motion = (uint16_t)kCGGestureMotionHorizontal;
    fluid.gesture_flavor = kISSHIDGestureFlavorDockPrimary;
    fluid.swipe_progress = iss_to_fixed1616(progress);
    memcpy(write, &fluid, sizeof fluid);
    write += sizeof fluid;

    if (hasVelocity) {
        ISSHIDVelocity vel;
        memset(&vel, 0, sizeof vel);
        vel.base.size = sizeof(ISSHIDVelocity);
        vel.base.type = kISSHIDEventTypeVelocity;
        vel.base.depth = 1;
        vel.velocity_x = iss_to_fixed1616(velocityX);
        memcpy(write, &vel, sizeof vel);
        write += sizeof vel;
    }

    ISSHIDQueueElement header;
    memset(&header, 0, sizeof header);
    header.timestamp = mach_absolute_time();
    header.event_count = hasVelocity ? 2 : 1;
    memcpy(blob, &header, sizeof header);
    size_t blobLen = (size_t)(write - blob);

    CFDataRef data = CGEventCreateData(NULL, ev);
    CFRelease(ev);
    if (!data) {
        return false;
    }
    const uint8_t *src = CFDataGetBytePtr(data);
    CFIndex srcLen = CFDataGetLength(data);

    // Append field 4205 (binary blob): [be16 byteLen][be16 tag<<14|field][bytes].
    // tag 0b00 with size > 1 = binary blob; the deserializer is order-agnostic
    // so appending to the end is sufficient (no full re-serialize needed).
    size_t newLen = (size_t)srcLen + 4 + blobLen;
    uint8_t *buf = malloc(newLen);
    if (!buf) {
        CFRelease(data);
        return false;
    }
    memcpy(buf, src, srcLen);
    uint8_t *cursor = buf + srcLen;
    iss_put_be16(&cursor, (uint16_t)blobLen);
    iss_put_be16(&cursor, (uint16_t)4205);
    memcpy(cursor, blob, blobLen);
    CFRelease(data);

    CFDataRef newData = CFDataCreate(NULL, buf, newLen);
    free(buf);
    if (!newData) {
        return false;
    }
    CGEventRef augmented = CGEventCreateFromData(NULL, newData);
    CFRelease(newData);
    if (!augmented) {
        return false;
    }
    CGEventPost(kCGSessionEventTap, augmented);
    CFRelease(augmented);
    return true;
}

static bool iss_post_switch_gesture_aug(ISSDirection direction) {
    bool increase = (direction == ISSDirectionRight);
    double progress = increase ? -1.0 : 1.0;
    double velocity = increase ? -400.0 : 400.0;
    if (!iss_post_augmented_gesture((int)kCGSGesturePhaseBegan, 0.0, 0.0)) {
        return false;
    }
    usleep(3000);
    return iss_post_augmented_gesture((int)kCGSGesturePhaseEnded, progress, velocity);
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

    if (direction == ISSDirectionLeft) {
        return info->currentIndex == 0;
    }

    return info->currentIndex + 1 >= info->spaceCount;
}

static bool iss_post_switch_gesture(ISSDirection direction) {
    const bool isRight = (direction == ISSDirectionRight);

    // ScrollGestureFlagBits seem to mark direction (anything non-zero)
    int32_t scrollGestureFlagDirection = isRight ? 1 : 0;

    // Corresponds to distance, or something along those lines
    const double swipeProgress = isRight ? 2.0 : -2.0;

    // self-explanatory
    const double swipeVelocity = isRight ? 400.0 : -400.0;

    //
    // -- Begin gesture --
    //
    CGEventRef evA = CGEventCreate(NULL);
    if (!evA) {
        return false;
    }
    CGEventSetIntegerValueField(evA, kCGSEventTypeField, kCGSEventGesture);

    CGEventRef evB = CGEventCreate(NULL);
    if (!evB) {
        CFRelease(evA);
        return false;
    }
    CGEventSetIntegerValueField(evB, kCGSEventTypeField, kCGSEventDockControl);
    CGEventSetIntegerValueField(evB, kCGEventGestureHIDType, kIOHIDEventTypeDockSwipe);
    CGEventSetIntegerValueField(evB, kCGEventGesturePhase, kCGSGesturePhaseBegan);
    CGEventSetIntegerValueField(evB, kCGEventScrollGestureFlagBits, scrollGestureFlagDirection);
    CGEventSetIntegerValueField(evB, kCGEventGestureSwipeMotion, kCGGestureMotionHorizontal);
    CGEventSetDoubleValueField(evB, kCGEventGestureScrollY, 0);
    // Cannot explain this
    CGEventSetDoubleValueField(evB, kCGEventGestureZoomDeltaX, FLT_TRUE_MIN);

    CGEventPost(kCGSessionEventTap, evB);
    CGEventPost(kCGSessionEventTap, evA);
    CFRelease(evA);
    CFRelease(evB);

    //
    // -- End gesture --
    //
    evA = CGEventCreate(NULL);
    if (!evA) {
        return false;
    }
    CGEventSetIntegerValueField(evA, kCGSEventTypeField, kCGSEventGesture);

    evB = CGEventCreate(NULL);
    if (!evB) {
        CFRelease(evA);
        return false;
    }
    CGEventSetIntegerValueField(evB, kCGSEventTypeField, kCGSEventDockControl);
    CGEventSetIntegerValueField(evB, kCGEventGestureHIDType, kIOHIDEventTypeDockSwipe);
    CGEventSetIntegerValueField(evB, kCGEventGesturePhase, kCGSGesturePhaseEnded);
    CGEventSetDoubleValueField(evB, kCGEventGestureSwipeProgress, swipeProgress);
    CGEventSetIntegerValueField(evB, kCGEventScrollGestureFlagBits, scrollGestureFlagDirection);
    CGEventSetIntegerValueField(evB, kCGEventGestureSwipeMotion, kCGGestureMotionHorizontal);
    CGEventSetDoubleValueField(evB, kCGEventGestureScrollY, 0);
    CGEventSetDoubleValueField(evB, kCGEventGestureSwipeVelocityX, swipeVelocity);
    CGEventSetDoubleValueField(evB, kCGEventGestureSwipeVelocityY, 0);
    // Cannot explain this
    CGEventSetDoubleValueField(evB, kCGEventGestureZoomDeltaX, FLT_TRUE_MIN);

    CGEventPost(kCGSessionEventTap, evB);
    CGEventPost(kCGSessionEventTap, evA);
    CFRelease(evA);
    CFRelease(evB);

    return true;
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

bool iss_switch_to_index_instant(unsigned int targetIndex) {
    if (!cgs_symbols_available() ||
        (&SLSTransactionCreate == NULL) ||
        (&SLSTransactionSetManagedDisplayCurrentSpace == NULL) ||
        (&SLSTransactionCommit == NULL)) {
        fprintf(stderr, "ISS: SLSTransaction symbols missing\n");
        return false;
    }

    CGSConnectionID connection = CGSMainConnectionID();
    if (connection == 0) {
        return false;
    }

    // Identify the display under the cursor (same policy as iss_get_space_info)
    CFStringRef cursorIdent = NULL;
    CGEventRef tempEvent = CGEventCreate(NULL);
    if (tempEvent) {
        CGPoint cursorLocation = CGEventGetLocation(tempEvent);
        CFRelease(tempEvent);
        CGDirectDisplayID cursorDisplay = 0;
        uint32_t cursorDisplayCount = 0;
        if (CGGetDisplaysWithPoint(cursorLocation, 1, &cursorDisplay, &cursorDisplayCount) == kCGErrorSuccess && cursorDisplayCount > 0) {
            CFUUIDRef displayUUID = CGDisplayCreateUUIDFromDisplayID(cursorDisplay);
            if (displayUUID) {
                cursorIdent = CFUUIDCreateString(NULL, displayUUID);
                CFRelease(displayUUID);
            }
        }
    }

    CFArrayRef displays = CGSCopyManagedDisplaySpaces(connection, NULL);
    if (!displays) {
        if (cursorIdent) CFRelease(cursorIdent);
        return false;
    }

    // Pick the cursor display dict, falling back to the first one
    CFDictionaryRef targetDisplay = NULL;
    CFDictionaryRef fallbackDisplay = NULL;
    const CFIndex displayCount = CFArrayGetCount(displays);
    for (CFIndex i = 0; i < displayCount; i++) {
        const void *displayValue = CFArrayGetValueAtIndex(displays, i);
        if (!displayValue || CFGetTypeID(displayValue) != CFDictionaryGetTypeID()) {
            continue;
        }
        CFDictionaryRef displayDict = (CFDictionaryRef)displayValue;
        if (!fallbackDisplay) {
            fallbackDisplay = displayDict;
        }
        CFStringRef identifier = (CFStringRef)CFDictionaryGetValue(displayDict, CFSTR("Display Identifier"));
        if (cursorIdent && identifier && CFGetTypeID(identifier) == CFStringGetTypeID() && CFEqual(identifier, cursorIdent)) {
            targetDisplay = displayDict;
            break;
        }
    }
    if (!targetDisplay) {
        targetDisplay = fallbackDisplay;
    }
    if (cursorIdent) {
        CFRelease(cursorIdent);
        cursorIdent = NULL;
    }
    if (!targetDisplay) {
        CFRelease(displays);
        return false;
    }

    CFStringRef displayIdent = (CFStringRef)CFDictionaryGetValue(targetDisplay, CFSTR("Display Identifier"));
    const void *spacesValue = CFDictionaryGetValue(targetDisplay, CFSTR("Spaces"));
    if (!displayIdent || !spacesValue || CFGetTypeID(spacesValue) != CFArrayGetTypeID()) {
        CFRelease(displays);
        return false;
    }

    // Collect space IDs in order
    CFArrayRef spaces = (CFArrayRef)spacesValue;
    const CFIndex spaceCount = CFArrayGetCount(spaces);
    CGSSpaceID ids[64];
    unsigned int total = 0;
    for (CFIndex i = 0; i < spaceCount && total < 64; i++) {
        const void *spaceValue = CFArrayGetValueAtIndex(spaces, i);
        if (!spaceValue || CFGetTypeID(spaceValue) != CFDictionaryGetTypeID()) {
            continue;
        }
        CFNumberRef idNumber = (CFNumberRef)CFDictionaryGetValue((CFDictionaryRef)spaceValue, CFSTR("id64"));
        if (!idNumber || CFGetTypeID(idNumber) != CFNumberGetTypeID()) {
            continue;
        }
        CGSSpaceID candidate = 0;
        if (CFNumberGetValue(idNumber, kCFNumberSInt64Type, &candidate)) {
            ids[total++] = candidate;
        }
    }

    if (total == 0 || targetIndex >= total) {
        CFRelease(displays);
        return false;
    }

    CGSSpaceID targetSpace = ids[targetIndex];
    if (&CGSGetActiveSpace != NULL && CGSGetActiveSpace(connection) == targetSpace) {
        CFRelease(displays);
        return true;
    }

    CFTypeRef txn = SLSTransactionCreate(connection);
    if (!txn) {
        CFRelease(displays);
        return false;
    }
    SLSTransactionSetManagedDisplayCurrentSpace(txn, displayIdent, targetSpace);
    // Return value is unreliable (non-NULL even on success) — verify via
    // CGSGetActiveSpace instead.
    SLSTransactionCommit(txn, 1);
    CFRelease(txn);

    // Fix the menu-bar mangle: promote the front process to the new space so
    // the menu bar repaints for the right app instead of compositing stale menus.
    if (&SLSEnsureSpaceSwitchToActiveProcess != NULL) {
        SLSEnsureSpaceSwitchToActiveProcess(connection);
    }

    bool switched = (&CGSGetActiveSpace != NULL) && (CGSGetActiveSpace(connection) == targetSpace);
    CFRelease(displays);
    return switched;
}

bool iss_switch_to_index(unsigned int targetIndex) {
    ISSSpaceInfo info;
    if (!iss_get_space_info(&info)) {
        return false;
    }

    if (info.spaceCount == 0) {
        return false;
    }

    bool outOfBounds = targetIndex >= info.spaceCount;
    if (outOfBounds) {
        targetIndex = info.spaceCount - 1;
    }

    if (info.currentIndex == targetIndex) {
        return !outOfBounds;
    }

    ISSDirection direction = info.currentIndex < targetIndex ? ISSDirectionRight : ISSDirectionLeft;
    unsigned int steps = direction == ISSDirectionRight ? (targetIndex - info.currentIndex) : (info.currentIndex - targetIndex);

    for (unsigned int i = 0; i < steps; i++) {
        if (!iss_post_switch_gesture(direction)) {
            return false;
        }
    }

    return !outOfBounds;
}

bool iss_switch_to_index_aug(unsigned int targetIndex) {
    ISSSpaceInfo info;
    if (!iss_get_space_info(&info)) {
        return false;
    }
    if (info.spaceCount == 0) {
        return false;
    }

    bool outOfBounds = targetIndex >= info.spaceCount;
    if (outOfBounds) {
        targetIndex = info.spaceCount - 1;
    }

    // Each gesture moves exactly one space, so a far jump steps through the
    // spaces in between (inherent to the Dock-driven path; teleport via the SLS
    // transaction mangles the menu bar — dead end, see notes). The Dock pipelines
    // gestures posted in quick succession WITHOUT waiting for each to commit, so
    // a fast fixed-delay BURST — post exactly N gestures, ~2 ms apart, no waiting
    // — covers the distance in ~5 ms/space (~50 ms for 2↔10, ~5× faster than
    // waiting for each commit). Measured 6/6 exact in BOTH directions at 1–6 ms
    // spacing, so no feedback correction is needed: the burst posts exactly the
    // step count and never overshoots. (An earlier feedback top-up was REMOVED —
    // it raced the gesture commit and double-posted, causing the bounce/flash and
    // off-by-one landings, especially leftward near the low edge.)
    ISSDirection direction = info.currentIndex < targetIndex ? ISSDirectionRight : ISSDirectionLeft;
    unsigned int steps = direction == ISSDirectionRight
        ? (targetIndex - info.currentIndex)
        : (info.currentIndex - targetIndex);
    const unsigned int BURST_STEP_US = 2000;   // inter-gesture delay in the burst
    for (unsigned int i = 0; i < steps; i++) {
        if (!iss_post_switch_gesture_aug(direction)) {
            return false;
        }
        if (i + 1 < steps) {
            usleep(BURST_STEP_US);
        }
    }
    return !outOfBounds;
}
