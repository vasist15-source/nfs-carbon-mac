// perf_glass_mac.mm - the performance overlay on a native glass panel (perf_glass.h).
//
// VIEWS. A pass-through container over the SDL window's Metal view (hitTest nil, as the settings
// menu's in menu_glass_mac.mm) holds a glass panel in the top-right corner: Liquid Glass
// (NSGlassEffectView, macOS 26 and later) or the HUD material before it. On it: the frame rate in
// large semibold digits, coloured by how close it is to the limit, three rows of label/value pairs in
// the system font with monospaced digits, and in graph mode the last frame intervals as two Core
// Animation shape layers (on time / late) under a dashed budget line. The container is placed right
// above the Metal view and below the settings menu's, which is made earlier on the same tick.
// The presenter publishes numbers about four times a second; the views belong to the main thread.
#import <AppKit/AppKit.h>
#import <QuartzCore/QuartzCore.h>

#include <SDL3/SDL.h>

#include "../perf_glass.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <mutex>

#if __has_include(<AppKit/NSGlassEffectView.h>)
#define POP_HAVE_GLASS_VIEW 1
#else
#define POP_HAVE_GLASS_VIEW 0
#endif

@interface PopPerfGlassContainer : NSView
@end
@implementation PopPerfGlassContainer
- (NSView *)hitTest:(NSPoint)point {
    (void)point;
    return nil; // the game's view under it takes every event
}
- (BOOL)isFlipped {
    return YES;
}
@end

@interface PopPerfGlassContent : NSView
@end
@implementation PopPerfGlassContent
- (BOOL)isFlipped {
    return YES;
}
@end

namespace {

// ---- what the presenter published ---------------------------------------------------------------
std::mutex g_mu;
FramePacingSnapshot g_snap;
int g_mode = 0, g_limit = 0;
uint64_t g_serial = 0;
double g_published = -1;
std::atomic<bool> g_active{false};

// ---- the views (main thread) --------------------------------------------------------------------
constexpr CGFloat kWidth = 236, kPad = 14, kInset = 12, kGraphH = 44;
bool g_gave_up = false;
int g_tries = 0;
uint64_t g_shown_serial = 0;
int g_shown_mode = -1;
NSView *g_metal = nil;
PopPerfGlassContainer *g_container = nil;
NSView *g_glass = nil;
PopPerfGlassContent *g_content = nil;
NSTextField *g_fps = nil, *g_fps_unit = nil, *g_cap = nil;
NSTextField *g_keys[6] = {}, *g_values[6] = {};
CAShapeLayer *g_ok = nil, *g_late = nil, *g_budget = nil;
NSRect g_last_frame = NSZeroRect;

NSView *find_metal_view(NSView *v) {
    if ([v.layer isKindOfClass:[CAMetalLayer class]])
        return v;
    for (NSView *sub in v.subviews)
        if (NSView *found = find_metal_view(sub))
            return found;
    return nil;
}

NSView *make_glass() {
#if POP_HAVE_GLASS_VIEW
    if (@available(macOS 26.0, *)) {
        NSGlassEffectView *g = [[NSGlassEffectView alloc] initWithFrame:NSZeroRect];
        g.style = NSGlassEffectViewStyleRegular;
        g.tintColor = [NSColor colorWithWhite:0.0 alpha:0.30]; // white text stays readable over snow
        g.cornerRadius = 18;
        return g;
    }
#endif
    NSVisualEffectView *v = [[NSVisualEffectView alloc] initWithFrame:NSZeroRect];
    v.material = NSVisualEffectMaterialHUDWindow;
    v.blendingMode = NSVisualEffectBlendingModeWithinWindow;
    v.state = NSVisualEffectStateActive;
    v.wantsLayer = YES;
    v.layer.cornerRadius = 18;
    v.layer.cornerCurve = kCACornerCurveContinuous;
    v.layer.masksToBounds = YES;
    return v;
}

NSTextField *label(NSFont *font, NSColor *color, NSTextAlignment align) {
    NSTextField *t = [NSTextField labelWithString:@""];
    t.font = font;
    t.textColor = color;
    t.alignment = align;
    t.lineBreakMode = NSLineBreakByClipping;
    [g_content addSubview:t];
    return t;
}

CAShapeLayer *shape(NSColor *fill, NSColor *stroke) {
    CAShapeLayer *s = [CAShapeLayer layer];
    s.fillColor = fill ? fill.CGColor : nil;
    s.strokeColor = stroke ? stroke.CGColor : nil;
    s.contentsScale = g_metal.window ? g_metal.window.backingScaleFactor : 2;
    [g_content.layer addSublayer:s];
    return s;
}

void install(SDL_Window *window) {
    if (++g_tries > 600) { // no Metal view after many turns: keep the Metal overlay
        g_gave_up = true;
        return;
    }
    NSWindow *nsw = (__bridge NSWindow *)SDL_GetPointerProperty(SDL_GetWindowProperties(window),
                                                                SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, nullptr);
    NSView *metal = nsw ? find_metal_view(nsw.contentView) : nil;
    if (!metal || !metal.superview)
        return;
    g_metal = metal;
    g_container = [[PopPerfGlassContainer alloc] initWithFrame:metal.frame];
    g_container.wantsLayer = YES;
    g_container.hidden = YES;
    g_container.autoresizingMask = metal.autoresizingMask;
    g_container.appearance = [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];
    g_glass = make_glass();
    g_content = [[PopPerfGlassContent alloc] initWithFrame:NSZeroRect];
    g_content.wantsLayer = YES;
#if POP_HAVE_GLASS_VIEW
    if (@available(macOS 26.0, *)) {
        if ([g_glass isKindOfClass:[NSGlassEffectView class]])
            ((NSGlassEffectView *)g_glass).contentView = g_content; // content sits on the glass, vibrant
        else
            [g_glass addSubview:g_content];
    } else
#endif
        [g_glass addSubview:g_content];
    [g_container addSubview:g_glass];

    g_fps = label([NSFont monospacedDigitSystemFontOfSize:30 weight:NSFontWeightSemibold], NSColor.labelColor,
                  NSTextAlignmentLeft);
    g_fps_unit = label([NSFont systemFontOfSize:13 weight:NSFontWeightMedium], NSColor.secondaryLabelColor,
                       NSTextAlignmentLeft);
    g_fps_unit.stringValue = @"FPS";
    g_cap = label([NSFont monospacedDigitSystemFontOfSize:11 weight:NSFontWeightMedium], NSColor.secondaryLabelColor,
                  NSTextAlignmentRight);
    for (int i = 0; i < 6; ++i) {
        g_keys[i] = label([NSFont systemFontOfSize:11 weight:NSFontWeightRegular], NSColor.secondaryLabelColor,
                          NSTextAlignmentLeft);
        g_values[i] = label([NSFont monospacedDigitSystemFontOfSize:12 weight:NSFontWeightMedium], NSColor.labelColor,
                            NSTextAlignmentRight);
    }
    NSString *keys[6] = {@"Frame", @"p95", @"GPU", @"Age", @"Repeats", @"Drops"};
    for (int i = 0; i < 6; ++i)
        g_keys[i].stringValue = keys[i];
    g_budget = shape(nil, [NSColor colorWithWhite:1 alpha:0.35]);
    g_budget.lineWidth = 1;
    g_budget.lineDashPattern = @[ @3, @3 ];
    g_ok = shape([NSColor.systemGreenColor colorWithAlphaComponent:0.9], nil);
    g_late = shape(NSColor.systemRedColor, nil);

    // Right above the Metal view: under the settings menu's container, made earlier on this tick.
    [metal.superview addSubview:g_container positioned:NSWindowAbove relativeTo:metal];
    g_active = true;
    bool glass26 = false;
#if POP_HAVE_GLASS_VIEW
    if (@available(macOS 26.0, *))
        glass26 = true;
#endif
    fprintf(stderr, "[host] performance overlay on native %s\n", glass26 ? "Liquid Glass" : "HUD material");
}

NSString *fmt(const char *f, double v) {
    char buf[32];
    snprintf(buf, sizeof buf, f, v);
    return [NSString stringWithUTF8String:buf];
}

// Lay the panel out for `mode` and fill it from `s`.
void fill(const FramePacingSnapshot &s, int mode, int limit) {
    const CGFloat inner = kWidth - 2 * kPad;
    CGFloat y = kPad - 4;
    g_fps.frame = NSMakeRect(kPad, y, 70, 36);
    g_fps_unit.frame = NSMakeRect(kPad + 58, y + 15, 40, 16);
    g_cap.frame = NSMakeRect(kPad + inner - 110, y + 16, 110, 15);
    y += 40;
    const CGFloat col = (inner - 16) / 2;
    for (int r = 0; r < 3; ++r, y += 18)
        for (int c = 0; c < 2; ++c) {
            const CGFloat x = kPad + c * (col + 16);
            g_keys[r * 2 + c].frame = NSMakeRect(x, y + 1, col, 15);
            g_values[r * 2 + c].frame = NSMakeRect(x, y, col, 16);
        }
    const double target = limit ? limit : 60;
    const double budget = 1000.0 / target;
    // The frame rate, coloured as macOS colours a meter: green at the target, orange a little under.
    g_fps.stringValue = fmt("%.0f", s.new_fps);
    g_fps.textColor = s.new_fps >= target - 1.5    ? NSColor.systemGreenColor
                      : s.new_fps >= target * 0.85 ? NSColor.systemOrangeColor
                                                   : NSColor.systemRedColor;
    g_cap.stringValue = limit ? fmt("Limit %.0f", limit) : @"Game timing";
    g_values[0].stringValue = fmt("%.1f ms", s.median_ms);
    g_values[1].stringValue = fmt("%.1f ms", s.p95_ms);
    g_values[2].stringValue = fmt("%.1f ms", s.gpu_ms);
    g_values[3].stringValue = fmt("%.1f ms", s.age_ms);
    g_values[4].stringValue = fmt("%.0f%%", s.repeat_percent);
    g_values[5].stringValue = fmt("%.0f", double(s.drops));
    g_values[0].textColor = s.p95_ms > budget * 1.2 ? NSColor.systemOrangeColor : NSColor.labelColor;
    g_values[1].textColor = g_values[0].textColor;
    g_values[4].textColor = s.repeat_percent >= 1 ? NSColor.systemOrangeColor : NSColor.labelColor;
    g_values[5].textColor = s.drops ? NSColor.systemOrangeColor : NSColor.labelColor;

    const bool graph = mode == 2;
    g_ok.hidden = g_late.hidden = g_budget.hidden = !graph;
    if (graph) {
        y += 6;
        // Fixed 0..50 ms scale, newest on the right; a red bar is a missed budget (as the Metal overlay).
        const CGFloat base = y + kGraphH, per = kGraphH / 50.0;
        CGMutablePathRef ok = CGPathCreateMutable(), late = CGPathCreateMutable(), line = CGPathCreateMutable();
        const size_t n = s.intervals_ms.size();
        const CGFloat step = n ? inner / CGFloat(std::max<size_t>(n, 120)) : 0;
        const CGFloat x0 = kPad + inner - step * n;
        for (size_t i = 0; i < n; ++i) {
            const double ms = s.intervals_ms[i];
            const CGFloat h = std::max<CGFloat>(1, CGFloat(std::min(50.0, ms)) * per);
            const CGRect bar = CGRectMake(x0 + step * i, base - h, std::max<CGFloat>(1, step - 0.6), h);
            CGPathAddRoundedRect(ms > budget * 1.2 ? late : ok, nullptr, bar, std::min<CGFloat>(0.8, bar.size.width / 2),
                                 std::min<CGFloat>(0.8, h / 2));
        }
        const CGFloat by = std::round(base - std::min(50.0, budget) * per) + 0.5;
        CGPathMoveToPoint(line, nullptr, kPad, by);
        CGPathAddLineToPoint(line, nullptr, kPad + inner, by);
        [CATransaction begin];
        [CATransaction setDisableActions:YES];
        g_ok.path = ok;
        g_late.path = late;
        g_budget.path = line;
        [CATransaction commit];
        CGPathRelease(ok);
        CGPathRelease(late);
        CGPathRelease(line);
        y = base;
    }
    const CGFloat height = y + kPad - (graph ? 2 : 6);
    // Top-right of the game's view, clear of the notch strip.
    const NSRect bounds = g_metal.bounds;
    const CGFloat top = g_metal.safeAreaInsets.top;
    const NSRect panel = NSMakeRect(bounds.size.width - kWidth - kInset, top + kInset, kWidth, height);
    if (!NSEqualRects(g_container.frame, g_metal.frame))
        g_container.frame = g_metal.frame;
    if (!NSEqualRects(panel, g_last_frame)) {
        g_glass.frame = panel;
        g_content.frame = NSMakeRect(0, 0, kWidth, height);
        g_last_frame = panel;
    }
}

} // namespace

bool host_perf_glass_publish(int mode, const FramePacingSnapshot &s, int limit, double now) {
    if (!g_active.load(std::memory_order_acquire))
        return false;
    std::lock_guard lock(g_mu);
    if (mode != g_mode || limit != g_limit || now - g_published >= 0.25 || now < g_published) {
        if (mode)
            g_snap = s;
        g_mode = mode;
        g_limit = limit;
        g_published = now;
        ++g_serial;
    }
    return true;
}

void host_perf_glass_tick(void *sdl_window) {
    if (!g_active.load(std::memory_order_relaxed)) {
        static const bool off = [] {
            const char *v = std::getenv("RECOMP_PERF_GLASS");
            return v && *v == '0';
        }();
        if (off || g_gave_up || !sdl_window)
            return;
        @autoreleasepool {
            install(static_cast<SDL_Window *>(sdl_window));
        }
        return;
    }
    FramePacingSnapshot s;
    int mode, limit;
    uint64_t serial;
    {
        std::lock_guard lock(g_mu);
        serial = g_serial;
        mode = g_mode;
        limit = g_limit;
        if (serial == g_shown_serial)
            return;
        if (mode)
            s = g_snap;
    }
    g_shown_serial = serial;
    @autoreleasepool {
        if (!mode) {
            if (!g_container.hidden)
                g_container.hidden = YES;
            g_shown_mode = 0;
            return;
        }
        if (!g_metal.window || g_container.superview != g_metal.superview) {
            NSWindow *nsw = (__bridge NSWindow *)SDL_GetPointerProperty(
                SDL_GetWindowProperties(static_cast<SDL_Window *>(sdl_window)), SDL_PROP_WINDOW_COCOA_WINDOW_POINTER,
                nullptr);
            NSView *metal = nsw ? find_metal_view(nsw.contentView) : nil;
            if (!metal || !metal.superview)
                return;
            g_metal = metal;
            [g_container removeFromSuperview];
            [metal.superview addSubview:g_container positioned:NSWindowAbove relativeTo:metal];
        }
        [CATransaction begin];
        [CATransaction setDisableActions:YES];
        fill(s, mode, limit);
        if (g_container.hidden)
            g_container.hidden = NO;
        g_shown_mode = mode;
        [CATransaction commit];
    }
}
