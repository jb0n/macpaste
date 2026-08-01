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

#define DOUBLE_CLICK_MILLIS 500
#define DRAG_THRESHOLD_PX 5
#define PASTE_DELAY_NS 1000000LL // 1 ms, allows click events time to position cursor
#define MAX_WINDOW_NAME_SIZE 400

static char gIsDragging = 0;
static long long gPrevClickTime = 0;
static long long gCurClickTime = 0;
static CGPoint gDragStartPoint;

static CGEventTapLocation gTapA = kCGAnnotatedSessionEventTap;
static CFMachPortRef gEventTap;
static AXUIElementRef gSystemWide;
static CGEventFlags gCommandKey = kCGEventFlagCommand;
static bool gVerbose = false;

struct lookup {
    bool skipWindow;
    bool noFocus;
};

long long now() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    long long milliseconds = ts.tv_sec * 1000LL + ts.tv_nsec / 1000000; // calculate milliseconds
    return milliseconds;
}

static char *lowerDup(const char *s) {
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

static bool copyBasename(const char *path, char *buf, size_t buf_len) {
    const char *base = strrchr(path, '/');
    base = (base != NULL) ? base + 1 : path;
    snprintf(buf, buf_len, "%s", base);
    return true;
}

static bool findBundlePath(const char *execPath, char *out, size_t out_len) {
    char dir[PATH_MAX];
    snprintf(dir, sizeof(dir), "%s", execPath);
    char *slash = strrchr(dir, '/');
    while (slash != NULL && slash != dir) {
        *slash = '\0';
        size_t len = strlen(dir);
        if (len >= 4 && strcmp(dir + len - 4, ".app") == 0) {
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
        CFStringRef display = CFBundleCopyDisplayName(bundle);
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

static bool windowNameAt(CGPoint *mouse, char *buf, size_t buf_len) {
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
    char path[PROC_PIDPATHINFO_MAXSIZE];
    int len = proc_pidpath(pid, path, sizeof(path));
    if (len <= 0) {
        return false;
    }
    return displayNameForExecutable(path, buf, buf_len);
}

static struct lookup *windowLookup(CGPoint *mouse) {
    char name[MAX_WINDOW_NAME_SIZE];
    if (!windowNameAt(mouse, name, sizeof(name))) {
        if (gVerbose) {
            printf("no window\n");
        }
        return NULL;
    }
    char *key = lowerDup(name);
    if (NULL == key) {
        return NULL;
    }
    ENTRY e;
    e.key = key;
    ENTRY *ep = hsearch(e, FIND);
    struct lookup *le = (ep != NULL) ? (struct lookup *)ep->data : NULL;
    if (gVerbose) {
        printf("%s: skipWindow %d noFocus %d\n", name,
               le ? le->skipWindow : 0, le ? le->noFocus : 0);
    }
    return le;
}

static bool isSkipWindow(CGPoint *mouse) {
    struct lookup *le = windowLookup(mouse);
    return (le != NULL) && le->skipWindow;
}

static bool isNoFocusWindow(CGPoint *mouse) {
    struct lookup *le = windowLookup(mouse);
    return (le != NULL) && le->noFocus;
}

static void postKeyDownUp(CGKeyCode keycode, CGEventFlags flags) {
    CGEventRef kbdEventDown = CGEventCreateKeyboardEvent(NULL, keycode, 1);
    CGEventRef kbdEventUp   = CGEventCreateKeyboardEvent(NULL, keycode, 0);
    CGEventSetFlags(kbdEventDown, flags);
    CGEventSetFlags(kbdEventUp, flags);
    CGEventPost(gTapA, kbdEventDown);
    CGEventPost(gTapA, kbdEventUp);
    CFRelease(kbdEventDown);
    CFRelease(kbdEventUp);
}

static void paste(CGEventRef event) {
    // Mouse click to focus and position insertion cursor. Posted at the annotated
    // level (downstream of our own session-level tap) so the posted click never
    // re-enters this callback and triggers a spurious copy via isDoubleClick().
    CGPoint mouseLocation = CGEventGetLocation(event);
    if (!isNoFocusWindow(&mouseLocation)) {
        CGEventRef mouseClickDown = CGEventCreateMouseEvent(NULL, kCGEventLeftMouseDown,
                                                            mouseLocation,
                                                            kCGMouseButtonLeft);
        CGEventRef mouseClickUp = CGEventCreateMouseEvent(NULL, kCGEventLeftMouseUp,
                                                          mouseLocation,
                                                          kCGMouseButtonLeft);
        CGEventPost(gTapA, mouseClickDown);
        CGEventPost(gTapA, mouseClickUp);
        CFRelease(mouseClickDown);
        CFRelease(mouseClickUp);
    }

    if (isSkipWindow(&mouseLocation)) {
        return;
    }

    // Allow click events time to position cursor before pasting.
    struct timespec delay;
    delay.tv_sec = 0;
    delay.tv_nsec = PASTE_DELAY_NS;
    nanosleep(&delay, NULL);

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

static void recordClickTime() {
    gPrevClickTime = gCurClickTime;
    gCurClickTime = now();
}

static char isDoubleClickSpeed() {
    return (gCurClickTime - gPrevClickTime) < DOUBLE_CLICK_MILLIS;
}

static char isDoubleClick() {
    return isDoubleClickSpeed();
}

static CGEventRef mouseCallback (
    CGEventTapProxy proxy,
    CGEventType type,
    CGEventRef event,
    void * refcon
) {
    (void)proxy;
    (void)refcon;
    switch (type) {
    case kCGEventOtherMouseDown:
        if (CGEventGetIntegerValueField(event, kCGMouseEventButtonNumber) == 2) {
            paste(event);
        }
        break;

    case kCGEventLeftMouseDown:
        recordClickTime();
        gDragStartPoint = CGEventGetLocation(event);
        break;

    case kCGEventLeftMouseUp:
        if (isDoubleClick() || gIsDragging) {
            copy(event);
        }
        gIsDragging = 0;
        break;

    case kCGEventLeftMouseDragged:
        if (!gIsDragging) {
            CGPoint p = CGEventGetLocation(event);
            if (p.x - gDragStartPoint.x > DRAG_THRESHOLD_PX ||
                gDragStartPoint.x - p.x > DRAG_THRESHOLD_PX ||
                p.y - gDragStartPoint.y > DRAG_THRESHOLD_PX ||
                gDragStartPoint.y - p.y > DRAG_THRESHOLD_PX) {
                gIsDragging = 1;
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

    // Pass on the event, we must not modify it anyway, we are a listener
    return event;
}

static bool addLookupEntry(const char *name, bool skipWindow, bool noFocus) {
    char *key = lowerDup(name);
    if (NULL == key) {
        printf("Couldn't allocate lookup key\n");
        return false;
    }
    ENTRY e;
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
    }
    return true;
}

int main (int argc, char **argv) {
    CGEventMask emask;
    CFMachPortRef myEventTap;
    CFRunLoopSourceRef eventTapRLSrc;

    if (argc > 1) {
        if (0 == hcreate(argc + 10)) {
            printf("Couldn't create hash table\n");
            return -1;
        }
        int opt;
        while ((opt = getopt(argc, argv, "vcn:s:")) != -1) {
            switch (opt) {
            case 'v':
                gVerbose = true;
                break;
            case 'c':
                gCommandKey = kCGEventFlagControl;
                printf("Using ctrl instead of cmd\n");
                break;
            case 'n':
                printf("Won't focus window '%s'\n", optarg);
                if (!addLookupEntry(optarg, false, true)) {
                    return -1;
                }
                break;
            case 's':
                printf("Will skip window '%s'\n", optarg);
                if (!addLookupEntry(optarg, true, false)) {
                    return -1;
                }
                break;
            default:
                break;
            }
        }
    }

    gSystemWide = AXUIElementCreateSystemWide();
    if (NULL == gSystemWide) {
        fprintf(stderr, "Failed to create system-wide AX element\n");
        return 1;
    }

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
                     kCGEventTapOptionListenOnly, // We only listen, we don't modify
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

    // Add the source to the current RunLoop
    CFRunLoopAddSource(
        CFRunLoopGetCurrent(),
        eventTapRLSrc,
        kCFRunLoopDefaultMode
    );

    // Keep the RunLoop running forever
    CFRunLoopRun();

    // Not reached (RunLoop above never stops running)
    return 0;
}
