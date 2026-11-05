// The highest frame rate of the screen a window is on (from NFSMW 3.0.3, NFSU1 run 8). CGDisplayModeGetRefreshRate
// reports 0 for an adaptive (ProMotion) panel and the presenter asked only the main display, so a 120 Hz laptop panel
// - or a TV set to 120 Hz while another display is main - paced at 60. NSScreen.maximumFramesPerSecond (macOS 12)
// answers for the window's own screen. Main thread only, as the host's window state already is.
#import <AppKit/AppKit.h>

extern "C" double os_window_max_fps(void *ns_window) {
    NSWindow *window = (__bridge NSWindow *)ns_window;
    NSScreen *screen = window ? window.screen : NSScreen.mainScreen;
    if (!screen)
        return 0;
    if (@available(macOS 12.0, *))
        return (double)screen.maximumFramesPerSecond;
    return 0;
}
