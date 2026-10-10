// DockPin — keeps the Dock on the main display.
//
// The Dock jumps to another display when the pointer pushes against that
// display's Dock edge. DockPin swallows mouse-move events inside a thin band
// along that edge of every non-main display, so the Dock never sees the push.
// The cursor itself still moves freely; only the events are dropped.
//
// At startup and after display changes it also brings the Dock back to the
// main display if it is elsewhere. That work runs in a short-lived child
// (`DockPin --return`) so the resident process stays as small as possible.

#include <CoreGraphics/CoreGraphics.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define MAX_DISPLAYS 16
#define BAND 4.0           // points from the edge where events are swallowed
#define PREFS_RECHECK 2.0  // seconds between Dock orientation re-checks
#define MOVE_ATTEMPTS 3    // tries to bring the Dock back per display change

enum { EDGE_BOTTOM, EDGE_LEFT, EDGE_RIGHT };

static CGRect         mainRect;
static CGRect         others[MAX_DISPLAYS];  // non-main displays
static uint32_t       nOthers;
static CGRect         all[MAX_DISPLAYS];
static uint32_t       nAll;
static int            edge = EDGE_BOTTOM;
static CFAbsoluteTime prefsReadAt;
static struct timespec prefsMtime;
static char           prefsPath[1024];
static char           selfPath[1024];
static pid_t          returner;
static CFRunLoopTimerRef returnTimer;
static CFMachPortRef  tap;

// Reads the Dock plist directly: a CFPreferences client would keep a cfprefsd
// connection and a cached copy of the whole domain alive in this process.
static void load_orientation(void) {
    struct stat st;
    prefsReadAt = CFAbsoluteTimeGetCurrent();
    if (stat(prefsPath, &st) != 0) { edge = EDGE_BOTTOM; return; }
    if (st.st_mtimespec.tv_sec == prefsMtime.tv_sec && st.st_mtimespec.tv_nsec == prefsMtime.tv_nsec) return;
    prefsMtime = st.st_mtimespec;
    edge = EDGE_BOTTOM;

    int fd = open(prefsPath, O_RDONLY);
    if (fd < 0) return;
    UInt8 *buf = malloc(st.st_size);
    ssize_t n = buf ? read(fd, buf, st.st_size) : -1;
    close(fd);
    if (n > 0) {
        CFDataRef data = CFDataCreateWithBytesNoCopy(NULL, buf, n, kCFAllocatorNull);
        CFPropertyListRef plist = CFPropertyListCreateWithData(NULL, data, kCFPropertyListImmutable, NULL, NULL);
        if (plist && CFGetTypeID(plist) == CFDictionaryGetTypeID()) {
            CFTypeRef v = CFDictionaryGetValue(plist, CFSTR("orientation"));
            if (v && CFEqual(v, CFSTR("left")))       edge = EDGE_LEFT;
            else if (v && CFEqual(v, CFSTR("right"))) edge = EDGE_RIGHT;
        }
        if (plist) CFRelease(plist);
        CFRelease(data);
    }
    free(buf);
}

static void load_displays(void) {
    CGDirectDisplayID ids[MAX_DISPLAYS], mainID = CGMainDisplayID();
    uint32_t n = 0;
    if (CGGetActiveDisplayList(MAX_DISPLAYS, ids, &n) != kCGErrorSuccess) n = 0;
    mainRect = CGDisplayBounds(mainID);
    nAll = nOthers = 0;
    for (uint32_t i = 0; i < n; i++) {
        CGRect r = CGDisplayBounds(ids[i]);
        all[nAll++] = r;
        // Mirrored displays share the main display's bounds: nothing to guard.
        if (ids[i] != mainID && !CGRectEqualToRect(r, mainRect)) others[nOthers++] = r;
    }
}

// --- Bringing the Dock back (child process) ---------------------------------

// The Dock owns one window at the Dock window level, on the display it lives on.
static bool dock_off_main(void) {
    CFArrayRef list = CGWindowListCopyWindowInfo(kCGWindowListOptionOnScreenOnly, kCGNullWindowID);
    if (!list) return false;
    bool off = false;
    int dockLevel = CGWindowLevelForKey(kCGDockWindowLevelKey);
    for (CFIndex i = 0, n = CFArrayGetCount(list); i < n; i++) {
        CFDictionaryRef w = CFArrayGetValueAtIndex(list, i);
        CFStringRef owner = CFDictionaryGetValue(w, kCGWindowOwnerName);
        CFNumberRef layerNum = CFDictionaryGetValue(w, kCGWindowLayer);
        CFDictionaryRef boundsDict = CFDictionaryGetValue(w, kCGWindowBounds);
        int layer;
        CGRect b;
        if (!owner || !layerNum || !boundsDict || !CFEqual(owner, CFSTR("Dock"))) continue;
        if (!CFNumberGetValue(layerNum, kCFNumberIntType, &layer) || layer != dockLevel) continue;
        if (!CGRectMakeWithDictionaryRepresentation(boundsDict, &b)) continue;
        off = !CGRectContainsPoint(mainRect, CGPointMake(CGRectGetMidX(b), CGRectGetMidY(b)));
        break;
    }
    CFRelease(list);
    return off;
}

static void push_main_edge(void) {
    CGRect r = mainRect;
    CGPoint from, to;
    switch (edge) {
    case EDGE_LEFT:
        from = CGPointMake(CGRectGetMinX(r) + 50, CGRectGetMidY(r));
        to   = CGPointMake(CGRectGetMinX(r), CGRectGetMidY(r));
        break;
    case EDGE_RIGHT:
        from = CGPointMake(CGRectGetMaxX(r) - 50, CGRectGetMidY(r));
        to   = CGPointMake(CGRectGetMaxX(r) - 1, CGRectGetMidY(r));
        break;
    default:
        from = CGPointMake(CGRectGetMidX(r), CGRectGetMaxY(r) - 50);
        to   = CGPointMake(CGRectGetMidX(r), CGRectGetMaxY(r) - 1);
        break;
    }

    CGEventRef now = CGEventCreate(NULL);
    CGPoint back = CGEventGetLocation(now);
    CFRelease(now);

    CGEventSourceRef src = CGEventSourceCreate(kCGEventSourceStateHIDSystemState);
    // Approach the edge, then keep pressing against it: that is what moves the Dock.
    for (int i = 0; i < 16; i++) {
        double t = i < 8 ? i / 7.0 : 1.0;
        CGPoint p = CGPointMake(from.x + (to.x - from.x) * t, from.y + (to.y - from.y) * t);
        CGWarpMouseCursorPosition(p);
        CGEventRef e = CGEventCreateMouseEvent(src, kCGEventMouseMoved, p, kCGMouseButtonLeft);
        CGEventPost(kCGHIDEventTap, e);
        CFRelease(e);
        usleep(i < 8 ? 15000 : 25000);
    }
    if (src) CFRelease(src);
    CGWarpMouseCursorPosition(back);
}

static int return_dock(void) {
    load_orientation();
    load_displays();
    for (int i = 0; i < MOVE_ATTEMPTS && nOthers > 0 && dock_off_main(); i++) {
        fprintf(stderr, "DockPin: moving the Dock to the main display\n");
        push_main_edge();
        usleep(1500000);
    }
    return 0;
}

// --- Resident process --------------------------------------------------------

static void spawn_returner(CFRunLoopTimerRef t, void *ctx) {
    if (returner > 0 && kill(returner, 0) == 0) return;  // previous one still running
    char *args[] = { selfPath, "--return", NULL };
    if (posix_spawn(&returner, selfPath, NULL, NULL, args, NULL) != 0) returner = 0;
}

static void schedule_return(double delay) {
    CFRunLoopTimerSetNextFireDate(returnTimer, CFAbsoluteTimeGetCurrent() + delay);
}

static void on_reconfig(CGDirectDisplayID d, CGDisplayChangeSummaryFlags flags, void *ctx) {
    if (flags & kCGDisplayBeginConfigurationFlag) return;
    load_displays();
    schedule_return(2.0);  // let the Dock settle after the change first
}

static bool on_any_display(CGPoint p) {
    for (uint32_t i = 0; i < nAll; i++)
        if (CGRectContainsPoint(all[i], p)) return true;
    return false;
}

static CGEventRef on_event(CGEventTapProxy proxy, CGEventType type, CGEventRef ev, void *ctx) {
    if (type == kCGEventTapDisabledByTimeout || type == kCGEventTapDisabledByUserInput) {
        CGEventTapEnable(tap, true);
        return ev;
    }

    CGPoint p = CGEventGetLocation(ev);
    if (CGRectContainsPoint(mainRect, p)) return ev;  // hot path

    for (uint32_t i = 0; i < nOthers; i++) {
        CGRect r = others[i];
        if (!CGRectContainsPoint(r, p)) continue;

        double minX = CGRectGetMinX(r), maxX = CGRectGetMaxX(r), maxY = CGRectGetMaxY(r);
        bool nearBottom = p.y >= maxY - BAND, nearLeft = p.x < minX + BAND, nearRight = p.x >= maxX - BAND;
        if (!nearBottom && !nearLeft && !nearRight) return ev;

        // Pick up Dock position changes lazily, only while the pointer is near an edge.
        if (CFAbsoluteTimeGetCurrent() - prefsReadAt > PREFS_RECHECK) load_orientation();

        CGPoint beyond;
        if (edge == EDGE_BOTTOM && nearBottom)     beyond = CGPointMake(p.x, maxY);
        else if (edge == EDGE_LEFT && nearLeft)    beyond = CGPointMake(minX - 0.5, p.y);
        else if (edge == EDGE_RIGHT && nearRight)  beyond = CGPointMake(maxX, p.y);
        else return ev;

        // Edge shared with another display: the cursor just crosses over there.
        return on_any_display(beyond) ? ev : NULL;
    }
    return ev;
}

static CFMachPortRef create_tap(void) {
    CGEventMask mask = CGEventMaskBit(kCGEventMouseMoved)
                     | CGEventMaskBit(kCGEventLeftMouseDragged)
                     | CGEventMaskBit(kCGEventRightMouseDragged)
                     | CGEventMaskBit(kCGEventOtherMouseDragged);
    return CGEventTapCreate(kCGSessionEventTap, kCGHeadInsertEventTap,
                            kCGEventTapOptionDefault, mask, on_event, NULL);
}

static void run(char *const argv[]) {
    pid_t pid;
    if (posix_spawn(&pid, argv[0], NULL, NULL, argv, NULL) == 0) waitpid(pid, NULL, 0);
}

// Shows the system Accessibility prompt. Loaded on demand so that the
// ApplicationServices umbrella never sits in memory on normal runs.
static void request_access(void) {
    void *as = dlopen("/System/Library/Frameworks/ApplicationServices.framework/ApplicationServices", RTLD_LAZY);
    Boolean (*ask)(CFDictionaryRef) = as ? dlsym(as, "AXIsProcessTrustedWithOptions") : NULL;
    CFStringRef *promptKey = as ? dlsym(as, "kAXTrustedCheckOptionPrompt") : NULL;
    if (!ask || !promptKey) return;
    if (ask(NULL)) return;  // already allowed, the tap failed for another reason

    // An upgrade changes the ad-hoc signature: the old entry still looks enabled
    // in System Settings but no longer matches. Drop it so the prompt starts clean.
    // tccutil looks the app up through Launch Services, which does not know about
    // a bundle that was never opened from Finder, so register it first.
    CFBundleRef bundle = CFBundleGetMainBundle();
    CFStringRef bundleID = CFBundleGetIdentifier(bundle);
    CFURLRef bundleURL = CFBundleCopyBundleURL(bundle);
    char id[256], path[1024];
    if (bundleID && bundleURL
        && CFStringGetCString(bundleID, id, sizeof id, kCFStringEncodingUTF8)
        && CFURLGetFileSystemRepresentation(bundleURL, true, (UInt8 *)path, sizeof path)) {
        run((char *[]){ "/System/Library/Frameworks/CoreServices.framework/Frameworks/"
                        "LaunchServices.framework/Support/lsregister", "-f", path, NULL });
        run((char *[]){ "/usr/bin/tccutil", "reset", "Accessibility", id, NULL });
    }
    if (bundleURL) CFRelease(bundleURL);

    const void *keys[] = { *promptKey }, *vals[] = { kCFBooleanTrue };
    CFDictionaryRef opts = CFDictionaryCreate(NULL, keys, vals, 1,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    ask(opts);
    CFRelease(opts);
}

int main(int argc, char **argv) {
    const char *home = getenv("HOME");
    snprintf(prefsPath, sizeof prefsPath, "%s/Library/Preferences/com.apple.dock.plist", home ? home : "");
    uint32_t size = sizeof selfPath;
    if (_NSGetExecutablePath(selfPath, &size) != 0) selfPath[0] = 0;

    if (argc > 1 && strcmp(argv[1], "--return") == 0) return return_dock();

    // AXIsProcessTrusted() can stay stale for the life of the process after the
    // user grants access, so the tap itself is the permission check.
    bool prompted = false;
    while (!(tap = create_tap())) {
        if (!prompted) {
            fprintf(stderr, "DockPin: waiting for Accessibility permission\n");
            request_access();
            prompted = true;
        }
        sleep(3);
    }
    if (prompted && selfPath[0]) execv(selfPath, argv);  // start clean, without the prompt code loaded

    signal(SIGCHLD, SIG_IGN);  // children are reaped automatically
    load_orientation();
    load_displays();
    CGDisplayRegisterReconfigurationCallback(on_reconfig, NULL);

    CFRunLoopSourceRef src = CFMachPortCreateRunLoopSource(NULL, tap, 0);
    CFRunLoopAddSource(CFRunLoopGetMain(), src, kCFRunLoopCommonModes);
    CFRelease(src);  // the run loop keeps it
    CGEventTapEnable(tap, true);

    returnTimer = CFRunLoopTimerCreate(NULL, CFAbsoluteTimeGetCurrent() + 1.0, 1e9, 0, 0, spawn_returner, NULL);
    CFRunLoopAddTimer(CFRunLoopGetMain(), returnTimer, kCFRunLoopCommonModes);

    fprintf(stderr, "DockPin: active, %u other display(s)\n", nOthers);
    CFRunLoopRun();
    return 0;
}
