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
//
// Homebrew installs are started at login by `brew services`. A copy installed
// from the DMG registers the launch agent in its own bundle instead and hands
// over to it; `DockPin --unregister` removes that agent again.
//
// Every install logs to ~/Library/Logs/DockPin.log.

#include <CoreGraphics/CoreGraphics.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <signal.h>
#include <stdarg.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MAX_DISPLAYS 16
#define BAND 4.0           // points from the edge where events are swallowed
#define PREFS_RECHECK 2.0  // seconds between Dock orientation re-checks
#define MOVE_ATTEMPTS 3    // tries to bring the Dock back per display change
#define DISPLAY_POLL 3.0   // seconds between display layout checks
#define LOG_LIMIT (512 * 1024)  // bytes, then the log moves to DockPin.old.log
#define AGENT_LABEL "com.indapublic.dockpin"  // Contents/Library/LaunchAgents/<label>.plist

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
static const char    *EDGE_NAME[] = { "bottom", "left", "right" };

// --- Log ---------------------------------------------------------------------

// Every install writes to the same file, so a bug report can always carry
// it. Started from a terminal, DockPin keeps writing to the terminal.
static void open_log(const char *home) {
    char path[PATH_MAX], old[PATH_MAX];
    struct stat st;
    if (!home || isatty(STDERR_FILENO)) return;
    snprintf(path, sizeof path, "%s/Library/Logs/DockPin.log", home);
    snprintf(old, sizeof old, "%s/Library/Logs/DockPin.old.log", home);
    if (stat(path, &st) == 0 && st.st_size > LOG_LIMIT) rename(path, old);
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) return;
    dup2(fd, STDERR_FILENO);
    close(fd);
}

static void say(const char *format, ...) {
    char stamp[24];
    time_t now = time(NULL);
    strftime(stamp, sizeof stamp, "%Y-%m-%d %H:%M:%S", localtime(&now));
    fprintf(stderr, "%s ", stamp);
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    fputc('\n', stderr);
}

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

// Returns whether the layout differs from the one loaded before.
static bool load_displays(void) {
    CGDirectDisplayID ids[MAX_DISPLAYS], mainID = CGMainDisplayID();
    uint32_t n = 0;
    CGRect oldAll[MAX_DISPLAYS], oldMain = mainRect;
    uint32_t oldN = nAll;
    memcpy(oldAll, all, sizeof all);
    if (CGGetActiveDisplayList(MAX_DISPLAYS, ids, &n) != kCGErrorSuccess) n = 0;
    mainRect = CGDisplayBounds(mainID);
    nAll = nOthers = 0;
    for (uint32_t i = 0; i < n; i++) {
        CGRect r = CGDisplayBounds(ids[i]);
        all[nAll++] = r;
        // Mirrored displays share the main display's bounds: nothing to guard.
        if (ids[i] != mainID && !CGRectEqualToRect(r, mainRect)) others[nOthers++] = r;
    }
    return nAll != oldN || !CGRectEqualToRect(mainRect, oldMain)
        || memcmp(all, oldAll, nAll * sizeof all[0]) != 0;
}

// "main 1440x900 at 0,0; 2560x1440 at -592,-1440"
static const char *describe_displays(void) {
    static char text[512];
    size_t used = 0;
    text[0] = 0;
    for (uint32_t i = 0; i < nAll && used < sizeof text; i++) {
        CGRect r = all[i];
        used += snprintf(text + used, sizeof text - used, "%s%s%.0fx%.0f at %.0f,%.0f",
                         i ? "; " : "", CGRectEqualToRect(r, mainRect) ? "main " : "",
                         r.size.width, r.size.height, r.origin.x, r.origin.y);
    }
    return text;
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
        say("moving the Dock to the main display");
        push_main_edge();
        usleep(1500000);
    }
    return 0;
}

// --- Resident process --------------------------------------------------------

static void spawn_returner(CFRunLoopTimerRef t, void *ctx) {
    // Asks about our own child only: after a few days its bare PID may well
    // belong to an unrelated process.
    if (returner > 0 && waitpid(returner, NULL, WNOHANG) == 0) return;  // still running
    char *args[] = { selfPath, "--return", NULL };
    if (posix_spawn(&returner, selfPath, NULL, NULL, args, NULL) != 0) returner = 0;
}

static void schedule_return(double delay) {
    CFRunLoopTimerSetNextFireDate(returnTimer, CFAbsoluteTimeGetCurrent() + delay);
}

// CGDisplayRegisterReconfigurationCallback never fires in a process without
// AppKit, and AppKit would more than double the footprint. Reading the layout
// takes about 50 us, so DockPin simply looks every few seconds.
static void check_displays(CFRunLoopTimerRef t, void *ctx) {
    if (!load_displays()) return;
    load_orientation();
    say("displays changed: %s; Dock at the %s", describe_displays(), EDGE_NAME[edge]);
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

// --- Starting at login without Homebrew --------------------------------------

enum { AGENT_NOT_REGISTERED, AGENT_ENABLED, AGENT_REQUIRES_APPROVAL, AGENT_NOT_FOUND };

// The SMAppService for the launch agent in this bundle, or nil before macOS 13.
// ServiceManagement is loaded on demand, so the resident process never maps it.
static id login_agent(void) {
    if (!dlopen("/System/Library/Frameworks/ServiceManagement.framework/ServiceManagement", RTLD_LAZY))
        return nil;
    Class cls = objc_getClass("SMAppService");
    if (!cls) return nil;
    return ((id (*)(Class, SEL, id))objc_msgSend)(
        cls, sel_registerName("agentServiceWithPlistName:"), (id)CFSTR(AGENT_LABEL ".plist"));
}

static bool agent_call(id agent, const char *selector) {
    return ((BOOL (*)(id, SEL, id *))objc_msgSend)(agent, sel_registerName(selector), NULL);
}

// For a copy the user opened by hand: makes sure the launch agent runs
// DockPin, now and at every login. Returns false when this process should do
// the work itself: before macOS 13, or after the user turned DockPin off in
// Login Items, which a manual start must not override.
static bool hand_over_to_agent(void) {
    id agent = login_agent();
    if (!agent) return false;
    long status = ((long (*)(id, SEL))objc_msgSend)(agent, sel_registerName("status"));
    if (status == AGENT_ENABLED) return true;
    if (status == AGENT_REQUIRES_APPROVAL) return false;
    if (!agent_call(agent, "registerAndReturnError:")) return false;
    say("registered the login agent");
    return true;
}

// `DockPin --unregister`: stops DockPin and removes it from Login Items.
static int unregister_agent(void) {
    id agent = login_agent();
    return agent && agent_call(agent, "unregisterAndReturnError:") ? 0 : 1;
}

// launchd names the job of an app opened from Finder (or `open`)
// application.<bundle id>.<n>; the agent runs as AGENT_LABEL, brew services
// as sh.brew.dockpin, and a build started from a terminal inherits its name.
static bool opened_from_finder(void) {
    const char *job = getenv("XPC_SERVICE_NAME"), *prefix = "application." AGENT_LABEL ".";
    return job && strncmp(job, prefix, strlen(prefix)) == 0;
}

static bool installed_by_homebrew(void) {
    char real[PATH_MAX];
    return realpath(selfPath, real) && strstr(real, "/Cellar/");
}

static void say_hello(void) {
    char version[32] = "dev", macos[32] = "?";
    size_t size = sizeof macos;
    CFTypeRef v = CFBundleGetValueForInfoDictionaryKey(CFBundleGetMainBundle(), CFSTR("CFBundleShortVersionString"));
    if (v && CFGetTypeID(v) == CFStringGetTypeID()) CFStringGetCString(v, version, sizeof version, kCFStringEncodingUTF8);
    sysctlbyname("kern.osproductversion", macos, &size, NULL, 0);
    say("DockPin %s on macOS %s (pid %d)", version, macos, getpid());
}

int main(int argc, char **argv) {
    const char *home = getenv("HOME");
    snprintf(prefsPath, sizeof prefsPath, "%s/Library/Preferences/com.apple.dock.plist", home ? home : "");
    uint32_t size = sizeof selfPath;
    if (_NSGetExecutablePath(selfPath, &size) != 0) selfPath[0] = 0;

    if (argc > 1 && strcmp(argv[1], "--return") == 0) return return_dock();
    if (argc > 1 && strcmp(argv[1], "--unregister") == 0) return unregister_agent();

    open_log(home);
    say_hello();
    if (opened_from_finder() && !installed_by_homebrew() && hand_over_to_agent()) return 0;

    // AXIsProcessTrusted() can stay stale for the life of the process after the
    // user grants access, so the tap itself is the permission check.
    bool prompted = false;
    while (!(tap = create_tap())) {
        if (!prompted) {
            say("waiting for Accessibility permission");
            request_access();
            prompted = true;
        }
        sleep(3);
    }
    if (prompted && selfPath[0]) execv(selfPath, argv);  // start clean, without the prompt code loaded

    signal(SIGCHLD, SIG_IGN);  // children are reaped automatically
    load_orientation();
    load_displays();

    CFRunLoopSourceRef src = CFMachPortCreateRunLoopSource(NULL, tap, 0);
    CFRunLoopAddSource(CFRunLoopGetMain(), src, kCFRunLoopCommonModes);
    CFRelease(src);  // the run loop keeps it
    CGEventTapEnable(tap, true);

    returnTimer = CFRunLoopTimerCreate(NULL, CFAbsoluteTimeGetCurrent() + 1.0, 1e9, 0, 0, spawn_returner, NULL);
    CFRunLoopAddTimer(CFRunLoopGetMain(), returnTimer, kCFRunLoopCommonModes);

    CFRunLoopTimerRef displayTimer = CFRunLoopTimerCreate(NULL, CFAbsoluteTimeGetCurrent() + DISPLAY_POLL,
                                                          DISPLAY_POLL, 0, 0, check_displays, NULL);
    CFRunLoopTimerSetTolerance(displayTimer, 1.0);  // lets macOS batch the wakeups
    CFRunLoopAddTimer(CFRunLoopGetMain(), displayTimer, kCFRunLoopCommonModes);

    say("active: %s; Dock at the %s", describe_displays(), EDGE_NAME[edge]);
    CFRunLoopRun();
    return 0;
}
