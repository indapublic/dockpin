// Checks which mouse events DockPin swallows, on a fixed display layout,
// and that the Dock orientation is read from its plist.
#define main dockpin_main
#include "../dockpin.c"
#undef main

static int fails;

static void expect(double x, double y, const char *want) {
    CGEventRef e = CGEventCreateMouseEvent(NULL, kCGEventMouseMoved, CGPointMake(x, y), 0);
    const char *got = on_event(NULL, kCGEventMouseMoved, e, NULL) ? "pass" : "drop";
    if (strcmp(got, want)) { fails++; printf("FAIL edge=%d (%.1f, %.1f): got %s\n", edge, x, y, got); }
    CFRelease(e);
}

static void expect_orientation(const char *value, int want) {
    char tmpl[] = "/tmp/dockpin-test-XXXXXX";
    int fd = mkstemp(tmpl);
    dprintf(fd, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<plist version=\"1.0\"><dict>"
                "<key>orientation</key><string>%s</string></dict></plist>\n", value);
    close(fd);
    snprintf(prefsPath, sizeof prefsPath, "%s", tmpl);
    prefsMtime.tv_sec = 0;
    load_orientation();
    if (edge != want) { fails++; printf("FAIL orientation %s: got %d\n", value, edge); }
    unlink(tmpl);
}

int main(void) {
    // External display above the built-in one and shifted left, so part of its
    // bottom edge borders the main display and the rest is a free screen edge.
    mainRect = CGRectMake(0, 0, 1440, 900);
    all[0] = mainRect;
    all[1] = others[0] = CGRectMake(-592, -1440, 2560, 1440);
    nAll = 2;
    nOthers = 1;
    prefsReadAt = CFAbsoluteTimeGetCurrent() + 1e6;  // keep `edge` as set below

    edge = EDGE_BOTTOM;
    expect(-300, -1, "drop");    // free bottom edge, left of the main display
    expect(1700, -0.5, "drop");  // free bottom edge, right of the main display
    expect(1700, -3.9, "drop");
    expect(1700, -4.1, "pass");  // just above the band
    expect(500, -1, "pass");     // edge shared with the main display: crossing
    expect(500, 899, "pass");    // main display is never blocked
    expect(-591, -500, "pass");  // left edge does not matter for a bottom Dock
    expect(1000, -700, "pass");

    edge = EDGE_LEFT;
    expect(-591, -500, "drop");
    expect(-300, -1, "pass");

    edge = EDGE_RIGHT;
    expect(1967.5, -500, "drop");
    expect(1700, -1, "pass");

    expect_orientation("left", EDGE_LEFT);
    expect_orientation("right", EDGE_RIGHT);
    expect_orientation("bottom", EDGE_BOTTOM);

    // A live PID that is not our child (it was reused) must not block the
    // next returner. /usr/bin/true stands in for DockPin itself.
    snprintf(selfPath, sizeof selfPath, "/usr/bin/true");
    returner = getppid();
    spawn_returner(NULL, NULL);
    if (returner == getppid() || returner <= 0) { fails++; printf("FAIL returner blocked by a foreign PID\n"); }

    // The fake layout above differs from the real one; loading the real one
    // twice reports a change only the first time.
    if (!load_displays()) { fails++; printf("FAIL layout change not reported\n"); }
    if (load_displays()) { fails++; printf("FAIL unchanged layout reported as changed\n"); }

    printf(fails ? "%d failure(s)\n" : "all tests passed\n", fails);
    return fails != 0;
}
