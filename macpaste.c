// Public Domain License 2016
//
// Simulate right-handed unix/linux X11 middle-mouse-click copy and paste.
//
// References:
// http://stackoverflow.com/questions/3134901/mouse-tracking-daemon
// http://stackoverflow.com/questions/2379867/simulating-key-press-events-in-mac-os-x#2380280
//
// Compile with:
// gcc -O2 -Wall -Wextra -framework ApplicationServices -o macpaste macpaste.c
//
// Start with:
// ./macpaste
//
// Terminate with Ctrl+C
//
// Optional click-through (-t): a left click on a background window's content is
// re-posted after the app is activated, so the first click also acts on the
// content (links, buttons) instead of only focusing the window. -x "App" disables
// this per app. Off by default.

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <time.h>
#include <search.h>
#include <libproc.h>

#include <ApplicationServices/ApplicationServices.h>
#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>

#define kVK_ANSI_C 0x08
#define kVK_ANSI_V 0x09
#define kVK_Command 0x37
#define kVK_Control 0x3B

#define DOUBLE_CLICK_MILLIS 500
#define DOUBLE_CLICK_DISTANCE_PX 10 // two clicks farther apart than this aren't a double-click
#define DRAG_THRESHOLD_PX 5
// Time for the focus click to land before the paste keystroke follows it. This
// used to be 1 ms, which was never actually exercised: the click was posted to
// the annotated tap and no application ever received it. Now that it is
// delivered, the target has to process it and move its caret first, which is a
// round trip to another process -- paste ahead of that and the text lands where
// the caret used to be. Blocking the tap callback this long is fine; it costs
// one middle click and stays far below the tap's own disable-by-timeout.
#define PASTE_DELAY_NS 15000000LL // 15 ms
#define MODIFIER_DELAY_NS 1000000LL // 1 ms, lets the modifier register before the key
#define MAX_WINDOW_NAME_SIZE 400
#define CLICK_THROUGH_DELAY_SECONDS 0.1 // first check for app activation
// Activation latency varies with how busy the target app is, so poll for it
// instead of assuming it has completed by a fixed deadline: a single check at
// CLICK_THROUGH_DELAY_SECONDS drops the re-post whenever activation runs long.
#define CLICK_THROUGH_POLL_SECONDS 0.025
#define CLICK_THROUGH_MAX_ATTEMPTS 16 // ~0.5s total before giving up
// Accessibility calls are synchronous IPC into the target app and several run
// inside the event tap callback, where blocking stalls the whole input stream.
// Bound them below the tap's own disable-by-timeout so an unresponsive app costs
// one failed lookup instead of a frozen cursor. Measured cold worst case for
// AXUIElementCopyElementAtPosition on this machine was ~52ms, so keep healthy
// margin over that; too tight a value silently disables window matching.
#define AX_MESSAGING_TIMEOUT_SECONDS 0.25f
// Stamped on every mouse event we post. The session tap is upstream of our own
// tap, so these come straight back to mouseCallback; unmarked, a re-posted click
// would look like a fresh user click and start a second click-through, or land
// close enough in time to count as a double-click and fire a spurious copy.
#define MACPASTE_SYNTHETIC 0x6D6370737465LL // "mcpste"

static bool gIsDragging = false;
static long long gPrevClickTime = 0;
static long long gCurClickTime = 0;
static CGPoint gPrevClickPoint;
static CGPoint gCurClickPoint;
static CGPoint gDragStartPoint;

// Keyboard posts go to the annotated tap, which routes them to the focused app.
static CGEventTapLocation gTapA = kCGAnnotatedSessionEventTap;
// Mouse posts must not. Measured on this machine: a click posted to the
// annotated tap is never delivered to any application, frontmost or not, while
// the same click posted to the session tap always is. Everything synthetic we
// aim at a window therefore goes here.
static CGEventTapLocation gTapMouse = kCGSessionEventTap;
static CFMachPortRef gEventTap;
static AXUIElementRef gSystemWide;
static CGEventFlags gCommandKey = kCGEventFlagMaskCommand;
static bool gVerbose = false;
static bool gClickThrough = false;
static bool gClickThroughExclusions = false;
static bool gClickThroughPending = false;
static CGPoint gClickThroughPoint;
static int gClickThroughClickState = 1;
static CGEventFlags gClickThroughFlags = 0;
static pid_t gClickThroughPid = -1;

struct lookup {
    bool skipWindow;
    bool noFocus;
    bool noClickThrough;
};

static long long now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    long long milliseconds = ts.tv_sec * 1000LL + ts.tv_nsec / 1000000; // calculate milliseconds
    return milliseconds;
}

static char *asciiLowerDup(const char *s) {
    size_t len = strlen(s);
    char *out = malloc(len + 1);
    if (NULL == out) {
        return NULL;
    }
    for (size_t i = 0; i < len; i++) {
        char c = s[i];
        out[i] = (c >= 'A' && c <= 'Z') ? (char)(c + 'a' - 'A') : c;
    }
    out[len] = '\0';
    return out;
}

// Case-fold for hash lookups. App display names are UTF-8, so fold via CFString
// (locale-independent) rather than ASCII-only, otherwise -s/-n/-x silently stay
// case-sensitive for any non-ASCII character in a name.
static char *lowerDup(const char *s) {
    CFStringRef str = CFStringCreateWithCString(kCFAllocatorDefault, s, kCFStringEncodingUTF8);
    if (NULL == str) {
        return asciiLowerDup(s); // not valid UTF-8; fold what we can
    }
    CFMutableStringRef folded = CFStringCreateMutableCopy(kCFAllocatorDefault, 0, str);
    CFRelease(str);
    if (NULL == folded) {
        return NULL;
    }
    CFStringLowercase(folded, NULL);
    CFIndex bufLen = CFStringGetMaximumSizeForEncoding(CFStringGetLength(folded),
                                                       kCFStringEncodingUTF8) + 1;
    char *out = malloc((size_t)bufLen);
    if (NULL == out) {
        CFRelease(folded);
        return NULL;
    }
    if (!CFStringGetCString(folded, out, bufLen, kCFStringEncodingUTF8)) {
        free(out);
        CFRelease(folded);
        return NULL;
    }
    CFRelease(folded);
    return out;
}

static bool copyBasename(const char *path, char *buf, size_t buf_len) {
    const char *base = strrchr(path, '/');
    base = (base != NULL) ? base + 1 : path;
    int n = snprintf(buf, buf_len, "%s", base);
    return n >= 0 && (size_t)n < buf_len; // a truncated name would never match anyway
}

static bool findBundlePath(const char *execPath, char *out, size_t out_len) {
    char dir[PROC_PIDPATHINFO_MAXSIZE]; // proc_pidpath() paths can exceed PATH_MAX
    if (strlen(execPath) >= sizeof(dir)) {
        return false;
    }
    snprintf(dir, sizeof(dir), "%s", execPath);
    char *slash = strrchr(dir, '/');
    while (slash != NULL && slash != dir) {
        *slash = '\0';
        size_t len = strlen(dir);
        if (len >= 4 && strcmp(dir + len - 4, ".app") == 0) {
            if (len >= out_len) {
                return false; // rather than silently truncate to the wrong bundle
            }
            snprintf(out, out_len, "%s", dir);
            return true;
        }
        slash = strrchr(dir, '/');
    }
    return false;
}

static bool displayNameForExecutable(const char *execPath, char *buf, size_t buf_len) {
    bool ok = false;
    CFURLRef url = NULL;
    CFBundleRef bundle = NULL;

    char bundlePath[PATH_MAX];
    if (findBundlePath(execPath, bundlePath, sizeof(bundlePath))) {
        url = CFURLCreateFromFileSystemRepresentation(kCFAllocatorDefault,
                                                      (const UInt8 *)bundlePath,
                                                      strlen(bundlePath), true);
    }
    if (url != NULL) {
        bundle = CFBundleCreate(kCFAllocatorDefault, url);
        CFRelease(url);
    }
    if (bundle != NULL) {
        CFStringRef display = NULL;
        CFTypeRef displayVal = CFBundleGetValueForInfoDictionaryKey(bundle, CFSTR("CFBundleDisplayName"));
        if (displayVal != NULL && CFGetTypeID(displayVal) == CFStringGetTypeID()) {
            display = (CFStringRef)CFRetain(displayVal);
        }
        if (display == NULL) {
            CFTypeRef nameVal = CFBundleGetValueForInfoDictionaryKey(bundle, kCFBundleNameKey);
            if (nameVal != NULL && CFGetTypeID(nameVal) == CFStringGetTypeID()) {
                display = (CFStringRef)CFRetain(nameVal);
            }
        }
        if (display != NULL) {
            ok = CFStringGetCString(display, buf, buf_len, kCFStringEncodingUTF8);
            CFRelease(display);
        }
        CFRelease(bundle);
    }
    if (!ok) {
        return copyBasename(execPath, buf, buf_len);
    }
    return true;
}

static bool windowInfoAt(CGPoint *mouse, char *buf, size_t buf_len, pid_t *pid_out) {
    AXUIElementRef el = NULL;
    AXError err = AXUIElementCopyElementAtPosition(gSystemWide,
                                                   (float)mouse->x, (float)mouse->y, &el);
    if (err != kAXErrorSuccess || el == NULL) {
        return false;
    }
    pid_t pid = 0;
    err = AXUIElementGetPid(el, &pid);
    CFRelease(el);
    if (err != kAXErrorSuccess || pid <= 0) {
        return false;
    }
    if (pid_out != NULL) {
        *pid_out = pid;
    }
    if (buf == NULL) {
        return true;
    }
    char path[PROC_PIDPATHINFO_MAXSIZE];
    int len = proc_pidpath(pid, path, sizeof(path));
    if (len <= 0) {
        return false;
    }
    return displayNameForExecutable(path, buf, buf_len);
}

static bool windowNameAt(CGPoint *mouse, char *buf, size_t buf_len) {
    return windowInfoAt(mouse, buf, buf_len, NULL);
}

static struct lookup *lookupByName(const char *name) {
    char *key = lowerDup(name);
    if (NULL == key) {
        return NULL;
    }
    ENTRY e = {0}; // hsearch() takes ENTRY by value; leave no field uninitialized
    e.key = key;
    ENTRY *ep = hsearch(e, FIND);
    free(key);
    struct lookup *le = (ep != NULL) ? (struct lookup *)ep->data : NULL;
    if (gVerbose) {
        printf("%s: skipWindow %d noFocus %d noClickThrough %d\n", name,
               le ? le->skipWindow : 0, le ? le->noFocus : 0, le ? le->noClickThrough : 0);
    }
    return le;
}

static struct lookup *windowLookup(CGPoint *mouse) {
    char name[MAX_WINDOW_NAME_SIZE];
    if (!windowNameAt(mouse, name, sizeof(name))) {
        if (gVerbose) {
            printf("no window\n");
        }
        return NULL;
    }
    return lookupByName(name);
}

static bool isSkipWindow(CGPoint *mouse) {
    struct lookup *le = windowLookup(mouse);
    return (le != NULL) && le->skipWindow;
}

static bool isNoFocusWindow(CGPoint *mouse) {
    struct lookup *le = windowLookup(mouse);
    return (le != NULL) && le->noFocus;
}

static bool isDockPid(pid_t pid) {
    char path[PROC_PIDPATHINFO_MAXSIZE];
    if (proc_pidpath(pid, path, sizeof(path)) <= 0) {
        return false;
    }
    const char *base = strrchr(path, '/');
    base = (base != NULL) ? base + 1 : path;
    return strcmp(base, "Dock") == 0;
}

static void activateApp(pid_t pid) {
    AXUIElementRef app = AXUIElementCreateApplication(pid);
    if (NULL == app) {
        return;
    }
    AXUIElementSetAttributeValue(app, kAXFrontmostAttribute, kCFBooleanTrue);
    CFRelease(app);
}

static CGEventRef createSyntheticClick(CGEventType type, CGPoint point,
                                       int clickState, CGEventFlags flags) {
    CGEventRef event = CGEventCreateMouseEvent(NULL, type, point, kCGMouseButtonLeft);
    if (NULL == event) {
        return NULL;
    }
    CGEventSetIntegerValueField(event, kCGMouseEventClickState, clickState);
    CGEventSetFlags(event, flags);
    CGEventSetIntegerValueField(event, kCGEventSourceUserData, MACPASTE_SYNTHETIC);
    return event;
}

// A lone down, for a swallowed click that turned out to be the start of a drag:
// the hardware up that ends the drag supplies the other half.
static void postMouseDown(CGPoint point, int clickState, CGEventFlags flags) {
    CGEventRef mouseClickDown = createSyntheticClick(kCGEventLeftMouseDown, point,
                                                     clickState, flags);
    if (NULL == mouseClickDown) {
        return;
    }
    CGEventPost(gTapMouse, mouseClickDown);
    CFRelease(mouseClickDown);
}

// Post both halves or neither: a down with no up leaves the app in a tracking
// loop that reads every later mouse move as a drag, selecting text as it goes.
static void postClick(CGPoint point, int clickState, CGEventFlags flags) {
    CGEventRef mouseClickDown = createSyntheticClick(kCGEventLeftMouseDown, point,
                                                     clickState, flags);
    CGEventRef mouseClickUp = createSyntheticClick(kCGEventLeftMouseUp, point,
                                                   clickState, flags);
    if (mouseClickDown != NULL && mouseClickUp != NULL) {
        CGEventPost(gTapMouse, mouseClickDown);
        CGEventPost(gTapMouse, mouseClickUp);
    }
    if (mouseClickDown != NULL) {
        CFRelease(mouseClickDown);
    }
    if (mouseClickUp != NULL) {
        CFRelease(mouseClickUp);
    }
}

struct clickThroughInfo {
    CGPoint point;
    int clickState;
    CGEventFlags flags;
    pid_t pid;
    int attemptsLeft;
};

// Ask an app directly whether it is frontmost, reading back the same attribute
// activateApp() sets. This replaces querying kAXFocusedApplicationAttribute on
// the system-wide element, which fails with kAXErrorCannotComplete whenever the
// current frontmost app declines to answer it (VMware Fusion, for one) and so
// silently disabled click-through for as long as such an app was in front.
//
// Returns false if the app does not answer at all, which is NOT the same as
// answering "not frontmost": callers must never swallow a click for an app whose
// state they cannot read, or the click would be lost entirely.
static bool readAppFrontmost(pid_t pid, bool *frontmost) {
    AXUIElementRef app = AXUIElementCreateApplication(pid);
    if (NULL == app) {
        return false;
    }
    bool ok = false;
    CFTypeRef val = NULL;
    if (AXUIElementCopyAttributeValue(app, kAXFrontmostAttribute, &val) == kAXErrorSuccess &&
        val != NULL) {
        if (CFGetTypeID(val) == CFBooleanGetTypeID()) {
            *frontmost = CFBooleanGetValue((CFBooleanRef)val);
            ok = true;
        }
        CFRelease(val);
    }
    CFRelease(app);
    return ok;
}

static void endClickThrough(CFRunLoopTimerRef timer, struct clickThroughInfo *ci) {
    free(ci);
    CFRunLoopTimerInvalidate(timer);
    CFRelease(timer);
}

// Re-post the swallowed click once the target app is actually frontmost, and
// only if it still owns whatever is under the click point. Without the ownership
// check a dialog that appeared while we waited, or a window that moved or closed,
// would receive a click the user never aimed at it. This runs on the run loop
// rather than in the tap callback, so these AX calls cannot stall input.
static void clickThroughTimerCallback(CFRunLoopTimerRef timer, void *info) {
    struct clickThroughInfo *ci = (struct clickThroughInfo *)info;

    bool frontmost = false;
    if (!readAppFrontmost(ci->pid, &frontmost) || !frontmost) {
        if (--ci->attemptsLeft > 0) {
            return; // activation still in flight; the repeating timer retries
        }
        // Out of attempts. The down was swallowed on the way in, so dropping the
        // re-post now would lose the click outright; send it regardless. A click
        // on a window that never came forward is just the ordinary
        // activate-on-first-click behaviour, so this is safe to fall through to.
        if (gVerbose) {
            printf("click-through: pid %d never became frontmost, posting anyway\n", ci->pid);
        }
    }

    pid_t under = -1;
    if (windowInfoAt(&ci->point, NULL, 0, &under) && under == ci->pid) {
        postClick(ci->point, ci->clickState, ci->flags);
    } else if (gVerbose) {
        printf("click-through: pid %d no longer owns the click point (now %d), "
               "dropping re-post\n", ci->pid, under);
    }
    endClickThrough(timer, ci);
}

static bool scheduleClickThrough(CGPoint point, int clickState, CGEventFlags flags, pid_t pid) {
    struct clickThroughInfo *ci = malloc(sizeof(*ci));
    if (NULL == ci) {
        return false;
    }
    ci->point = point;
    ci->clickState = clickState;
    ci->flags = flags;
    ci->pid = pid;
    ci->attemptsLeft = CLICK_THROUGH_MAX_ATTEMPTS;
    CFRunLoopTimerContext context;
    context.version = 0;
    context.info = ci;
    context.retain = NULL;
    context.release = NULL;
    context.copyDescription = NULL;
    // Repeating: the callback polls for activation and stops the timer itself,
    // either after re-posting the click or once it runs out of attempts.
    CFRunLoopTimerRef timer = CFRunLoopTimerCreate(
        kCFAllocatorDefault,
        CFAbsoluteTimeGetCurrent() + CLICK_THROUGH_DELAY_SECONDS,
        CLICK_THROUGH_POLL_SECONDS, 0, 0,
        clickThroughTimerCallback,
        &context);
    if (NULL == timer) {
        free(ci);
        return false;
    }
    CFRunLoopAddTimer(CFRunLoopGetCurrent(), timer, kCFRunLoopDefaultMode);
    return true;
}

// Returns true if we are taking over this click: the caller must then swallow
// the down, because both halves are re-posted together once the app is
// frontmost. Swallowing one half and passing the other is what leaves an app
// selecting text under a button the user already released.
static bool maybeStartClickThrough(CGEventRef event) {
    CGPoint point = CGEventGetLocation(event);
    char name[MAX_WINDOW_NAME_SIZE];
    pid_t pid = -1;
    if (!windowInfoAt(&point, name, sizeof(name), &pid)) {
        return false;
    }
    if (isDockPid(pid)) {
        return false;
    }
    bool frontmost = false;
    if (!readAppFrontmost(pid, &frontmost)) {
        // Can't tell, so don't swallow the up: passing the click through beats
        // holding one back for an app that may never report itself frontmost.
        if (gVerbose) {
            printf("click-through: can't read frontmost state of %s, passing click through\n",
                   name);
        }
        return false;
    }
    if (frontmost) {
        if (gVerbose) {
            printf("click-through: %s already frontmost\n", name);
        }
        return false;
    }
    struct lookup *le = lookupByName(name);
    if (le != NULL && le->noClickThrough) {
        if (gVerbose) {
            printf("click-through: %s excluded\n", name);
        }
        return false;
    }
    int clickState = (int)CGEventGetIntegerValueField(event, kCGMouseEventClickState);
    if (clickState < 1) {
        clickState = 1;
    }
    if (gVerbose) {
        printf("click-through: activating pid %d (%s)\n", pid, name);
    }
    activateApp(pid);
    gClickThroughPoint = point;
    gClickThroughClickState = clickState;
    gClickThroughFlags = CGEventGetFlags(event);
    gClickThroughPid = pid;
    gClickThroughPending = true;
    return true;
}

static void nsleep(long long nanos) {
    struct timespec delay;
    delay.tv_sec = 0;
    delay.tv_nsec = nanos;
    nanosleep(&delay, NULL);
}

// The virtual keycode of the physical modifier key that produces `flags`, so we
// can press and release it as hardware would.
static CGKeyCode modifierKeyCode(CGEventFlags flags) {
    return (flags & kCGEventFlagMaskControl) ? kVK_Control : kVK_Command;
}

// Press or release the modifier key itself. Given a modifier keycode,
// CGEventCreateKeyboardEvent() returns a flagsChanged event already carrying the
// modifier mask plus the device-dependent left/right bit that real hardware sets
// (0x...108 for left command down, cleared on release), so take those flags as
// given and only add `extra`. Returns the posted flags so the key event between
// the two can carry the same state.
static CGEventFlags postModifier(CGKeyCode keycode, bool down, CGEventFlags extra) {
    CGEventRef event = CGEventCreateKeyboardEvent(NULL, keycode, down);
    if (NULL == event) {
        return extra;
    }
    CGEventFlags flags = CGEventGetFlags(event) | extra;
    CGEventSetFlags(event, flags);
    CGEventPost(gTapA, event);
    CFRelease(event);
    return flags;
}

// Bracket the keystroke with real modifier press/release events instead of only
// stamping the flags onto the key event. Native Cocoa apps read the flags field
// and so accept the bare key event, but VM and remote-desktop clients (VMware
// Fusion, VirtualBox, RDP/VNC) track modifier state from flagsChanged and ignore
// that field, so without this the guest receives a plain "c"/"v" keypress and
// types the letter instead of copying or pasting.
static void postKeyDownUp(CGKeyCode keycode, CGEventFlags flags) {
    CGEventRef kbdEventDown = CGEventCreateKeyboardEvent(NULL, keycode, 1);
    CGEventRef kbdEventUp   = CGEventCreateKeyboardEvent(NULL, keycode, 0);
    if (NULL == kbdEventDown || NULL == kbdEventUp) {
        if (kbdEventDown != NULL) {
            CFRelease(kbdEventDown);
        }
        if (kbdEventUp != NULL) {
            CFRelease(kbdEventUp);
        }
        return;
    }
    // Whatever the user is physically holding right now. It is deliberately kept
    // out of the keystroke itself: shift+drag to extend a selection and alt+drag
    // for column selection are ordinary gestures, and folding those in would send
    // cmd+shift+c or cmd+alt+c, which mean something else entirely. It goes only
    // on the release, so we hand their real modifier state back afterwards
    // instead of leaving the system believing they let go.
    CGEventFlags held = CGEventSourceFlagsState(kCGEventSourceStateCombinedSessionState) &
                        (kCGEventFlagMaskCommand | kCGEventFlagMaskControl |
                         kCGEventFlagMaskAlternate | kCGEventFlagMaskShift);
    CGKeyCode modKey = modifierKeyCode(flags);

    CGEventFlags down = postModifier(modKey, true, 0);
    nsleep(MODIFIER_DELAY_NS);
    CGEventSetFlags(kbdEventDown, down);
    CGEventSetFlags(kbdEventUp, down);
    CGEventPost(gTapA, kbdEventDown);
    CGEventPost(gTapA, kbdEventUp);
    nsleep(MODIFIER_DELAY_NS);
    postModifier(modKey, false, held & ~flags);

    CFRelease(kbdEventDown);
    CFRelease(kbdEventUp);
}

static void paste(CGEventRef event) {
    // Mouse click to focus and position the insertion cursor. It carries the
    // synthetic marker, so coming back through our own tap costs nothing.
    CGPoint mouseLocation = CGEventGetLocation(event);
    if (!isNoFocusWindow(&mouseLocation)) {
        postClick(mouseLocation, 1, 0);
    }

    if (isSkipWindow(&mouseLocation)) {
        return;
    }

    // Allow click events time to position cursor before pasting.
    nsleep(PASTE_DELAY_NS);

    // Paste.
    postKeyDownUp(kVK_ANSI_V, gCommandKey);
}

static void copy(CGEventRef event) {
    CGPoint mouseLocation = CGEventGetLocation(event);
    if (isSkipWindow(&mouseLocation)) {
        return;
    }
    postKeyDownUp(kVK_ANSI_C, gCommandKey);
}

static void recordClick(CGPoint point) {
    gPrevClickTime = gCurClickTime;
    gCurClickTime = now();
    gPrevClickPoint = gCurClickPoint;
    gCurClickPoint = point;
}

static bool isDoubleClickSpeed(void) {
    return (gCurClickTime - gPrevClickTime) < DOUBLE_CLICK_MILLIS;
}

static bool isDoubleClickDistance(void) {
    double dx = gCurClickPoint.x - gPrevClickPoint.x;
    double dy = gCurClickPoint.y - gPrevClickPoint.y;
    return (dx * dx + dy * dy) <=
           (double)DOUBLE_CLICK_DISTANCE_PX * DOUBLE_CLICK_DISTANCE_PX;
}

// Both tests matter: time alone treats two unrelated clicks at opposite corners
// of the screen as a double-click and fires a copy, overwriting the clipboard.
static bool isDoubleClick(void) {
    return isDoubleClickSpeed() && isDoubleClickDistance();
}

static CGEventRef mouseCallback (
    CGEventTapProxy proxy,
    CGEventType type,
    CGEventRef event,
    void * refcon
) {
    (void)proxy;
    (void)refcon;

    // Our own posted clicks, echoed back because the session tap sits upstream
    // of this one. They have already been through this logic once; re-entering
    // it would start a nested click-through or fake a double-click.
    if (CGEventGetIntegerValueField(event, kCGEventSourceUserData) == MACPASTE_SYNTHETIC) {
        return event;
    }

    switch (type) {
    case kCGEventOtherMouseDown:
        if (CGEventGetIntegerValueField(event, kCGMouseEventButtonNumber) == 2) {
            paste(event);
        }
        break;

    case kCGEventLeftMouseDown:
        gDragStartPoint = CGEventGetLocation(event);
        recordClick(gDragStartPoint);
        if (gClickThrough) {
            gClickThroughPending = false; // stale pending from a lost up; start fresh
            if (maybeStartClickThrough(event)) {
                return NULL; // re-posted with its up once the app is frontmost
            }
        }
        break;

    case kCGEventLeftMouseUp:
        // Still pending means the click stayed a click: a drag would have
        // released the swallowed down already, below.
        if (gClickThrough && gClickThroughPending) {
            gClickThroughPending = false;
            if (gVerbose) {
                printf("click-through: swallowing click, re-posting\n");
            }
            if (!scheduleClickThrough(gClickThroughPoint, gClickThroughClickState,
                                      gClickThroughFlags, gClickThroughPid)) {
                // No re-post is coming, and the down is already swallowed, so
                // deliver the click now rather than lose it. It goes to a window
                // that isn't frontmost yet, which is just the ordinary
                // activate-on-first-click behaviour.
                if (gVerbose) {
                    printf("click-through: couldn't schedule re-post, posting click now\n");
                }
                postClick(gClickThroughPoint, gClickThroughClickState, gClickThroughFlags);
            }
            // Either way this up's down is gone, so it must not go through alone.
            gIsDragging = false;
            return NULL;
        }
        if (isDoubleClick() || gIsDragging) {
            copy(event);
        }
        gIsDragging = false;
        break;

    case kCGEventLeftMouseDragged:
        if (!gIsDragging) {
            CGPoint p = CGEventGetLocation(event);
            if (p.x - gDragStartPoint.x > DRAG_THRESHOLD_PX ||
                gDragStartPoint.x - p.x > DRAG_THRESHOLD_PX ||
                p.y - gDragStartPoint.y > DRAG_THRESHOLD_PX ||
                gDragStartPoint.y - p.y > DRAG_THRESHOLD_PX) {
                gIsDragging = true;
                if (gClickThrough && gClickThroughPending) {
                    // The user is dragging, not clicking, so there is nothing to
                    // defer: hand the app the down we swallowed, at the point it
                    // was pressed, and let the rest of the drag and its up run
                    // normally. Waiting for activation here would eat the drag.
                    gClickThroughPending = false;
                    if (gVerbose) {
                        printf("click-through: click became a drag, releasing the down\n");
                    }
                    postMouseDown(gClickThroughPoint, gClickThroughClickState,
                                  gClickThroughFlags);
                }
            }
        }
        break;

    case kCGEventTapDisabledByTimeout:
    case kCGEventTapDisabledByUserInput:
        if (gVerbose) {
            printf("event tap disabled, re-enabling\n");
        }
        CGEventTapEnable(gEventTap, true);
        break;

    default:
        break;
    }

    // Pass on the event. The only events swallowed are both halves of a
    // click-through click, re-posted together (see scheduleClickThrough) once
    // the app is frontmost.
    return event;
}

static bool addLookupEntry(const char *name, bool skipWindow, bool noFocus, bool noClickThrough) {
    char *key = lowerDup(name);
    if (NULL == key) {
        printf("Couldn't allocate lookup key\n");
        return false;
    }
    ENTRY e = {0}; // hsearch() takes ENTRY by value; leave no field uninitialized
    e.key = key;
    ENTRY *ep = hsearch(e, FIND);
    if (NULL == ep) {
        struct lookup *le = malloc(sizeof(*le));
        if (NULL == le) {
            printf("Couldn't allocate lookup entry\n");
            free(key);
            return false;
        }
        le->skipWindow = skipWindow;
        le->noFocus = noFocus;
        le->noClickThrough = noClickThrough;
        e.data = le;
        ep = hsearch(e, ENTER);
        if (NULL == ep) {
            printf("Failed to insert lookup entry for '%s'\n", name);
            free(key);
            free(le);
            return false;
        }
    } else {
        struct lookup *le = ep->data;
        le->skipWindow |= skipWindow;
        le->noFocus |= noFocus;
        le->noClickThrough |= noClickThrough;
        free(key); // only ENTER hands the key to the table (hdestroy() frees those)
    }
    return true;
}

static void usage(const char *prog) {
    fprintf(stderr,
            "Usage: %s [-v] [-c] [-t] [-n name] [-s name] [-x name]\n"
            "  -v         verbose logging\n"
            "  -c         use ctrl instead of cmd for the synthesized copy/paste\n"
            "  -t         enable click-through: the first click on a background\n"
            "             window is re-posted after its app is activated\n"
            "  -n name    don't focus (click) windows of this app before pasting\n"
            "  -s name    skip this app entirely (no copy, no paste)\n"
            "  -x name    exclude this app from click-through (requires -t)\n"
            "  -h         show this help\n"
            "Names match the app's display name, case-insensitively.\n"
            "Terminate with Ctrl+C.\n",
            prog);
}

int main (int argc, char **argv) {
    CGEventMask emask;
    CFMachPortRef myEventTap;
    CFRunLoopSourceRef eventTapRLSrc;

    // Line-buffer stdout: it is block-buffered when piped, so -v output would
    // otherwise not appear until the buffer fills (this runs until Ctrl+C).
    setvbuf(stdout, NULL, _IOLBF, 0);

    // Always create the lookup table: windowLookup() runs on every paste/copy
    // even with no args, and hsearch() on an uncreated table is a crash.
    if (0 == hcreate((size_t)argc + 10)) {
        fprintf(stderr, "Couldn't create hash table\n");
        return 1;
    }

    int opt;
    while ((opt = getopt(argc, argv, "hvctn:s:x:")) != -1) {
        switch (opt) {
        case 'v':
            gVerbose = true;
            break;
        case 'c':
            gCommandKey = kCGEventFlagMaskControl;
            printf("Using ctrl instead of cmd\n");
            break;
        case 't':
            gClickThrough = true;
            printf("Click-through enabled: first click on a background window also clicks through\n");
            break;
        case 'n':
            printf("Won't focus window '%s'\n", optarg);
            if (!addLookupEntry(optarg, false, true, false)) {
                return 1;
            }
            break;
        case 's':
            printf("Will skip window '%s'\n", optarg);
            if (!addLookupEntry(optarg, true, false, false)) {
                return 1;
            }
            break;
        case 'x':
            printf("Won't click through window '%s'\n", optarg);
            if (!addLookupEntry(optarg, false, false, true)) {
                return 1;
            }
            gClickThroughExclusions = true;
            break;
        case 'h':
            usage(argv[0]);
            return 0;
        default:
            // Unknown option or missing argument: don't run on a half-applied config.
            usage(argv[0]);
            return 1;
        }
    }
    if (!gClickThrough && gClickThroughExclusions) {
        fprintf(stderr, "Warning: -x given but click-through is off (-t); "
                        "exclusions have no effect\n");
    }

    gSystemWide = AXUIElementCreateSystemWide();
    if (NULL == gSystemWide) {
        fprintf(stderr, "Failed to create system-wide AX element\n");
        return 1;
    }
    AXUIElementSetMessagingTimeout(gSystemWide, AX_MESSAGING_TIMEOUT_SECONDS);

    printf("Quit from command-line foreground with Ctrl+C\n");

    // We want "other" mouse button click-release, such as middle or exotic.
    // kCGEventTapDisabledByTimeout/ByUserInput are 0xFFFFFFFE/0xFFFFFFFF, whose
    // CGEventMaskBit shifts would overflow the 32-bit mask; the kernel selects
    // those events via the mod-32 bits 30/31, so set them explicitly.
    emask = CGEventMaskBit(kCGEventOtherMouseDown) |
            CGEventMaskBit(kCGEventLeftMouseDown) |
            CGEventMaskBit(kCGEventLeftMouseUp) |
            CGEventMaskBit(kCGEventLeftMouseDragged) |
            ((CGEventMask)1 << (kCGEventTapDisabledByTimeout & 31)) |
            ((CGEventMask)1 << (kCGEventTapDisabledByUserInput & 31));

    // Create the Tap
    myEventTap = CGEventTapCreate(
                     kCGSessionEventTap,          // Catch all events for current user session
                     kCGTailAppendEventTap,       // Append to end of EventTap list
                     kCGEventTapOptionDefault,    // We modify the stream: swallow click-through ups
                     emask,
                     & mouseCallback,
                     NULL                         // Tap handle is stored in gEventTap below;
                                                  // the callback needs it for re-enabling
                 );
    if (NULL == myEventTap) {
        fprintf(stderr, "Failed to create event tap: grant Accessibility permission "
                        "to %s in System Settings > Privacy & Security > Accessibility\n",
                argv[0]);
        return 1;
    }
    gEventTap = myEventTap;

    // Create a RunLoop Source for it
    eventTapRLSrc = CFMachPortCreateRunLoopSource(
                        kCFAllocatorDefault,
                        myEventTap,
                        0
                    );
    if (NULL == eventTapRLSrc) {
        fprintf(stderr, "Failed to create run loop source for event tap\n");
        return 1;
    }

    // Add the source to the current RunLoop
    CFRunLoopAddSource(
        CFRunLoopGetCurrent(),
        eventTapRLSrc,
        kCFRunLoopDefaultMode
    );
    CFRelease(eventTapRLSrc); // the run loop retains it; gEventTap keeps the tap alive

    // Keep the RunLoop running forever
    CFRunLoopRun();

    // Not reached (RunLoop above never stops running)
    return 0;
}
