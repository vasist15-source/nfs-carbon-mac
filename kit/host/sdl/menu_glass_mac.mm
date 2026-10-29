// menu_glass_mac.mm - the tabbed settings menu on a native glass panel (menu_glass.h).
//
// VIEWS. A container over the SDL window's Metal view, the same size, holds three views: a dim
// over the game's rectangle, the glass at the menu's panel (NSGlassEffectView on macOS 26 and
// later, the HUD material before it) and the page itself as a layer's contents. The container
// answers no hit test, so every click and scroll still reaches SDL's view and the menu's own
// pointer handling (input_gate.cpp, by fractions of the page), as with the compositor's page.
// Views belong to the main thread: the presenter only publishes pixels (host_menu_glass_publish)
// and the main thread's tick (pump/idle_wait in main.cpp) moves them into the views.
//
// TEXT. The page's text is drawn by Core Text in the system font (San Francisco), handed to the
// menu drawer as its text backend (mods_menu_set_text_backend), on the thread that draws the menu.
#import <AppKit/AppKit.h>
#import <CoreText/CoreText.h>
#import <QuartzCore/QuartzCore.h>

#include <SDL3/SDL.h>

#include "../compositor.h"
#include "../menu_glass.h"
#include "../../runtime/mods_seam.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <utility>
#include <vector>

@interface PopMenuGlassContainer : NSView
@end
@implementation PopMenuGlassContainer
- (NSView *)hitTest:(NSPoint)point {
    (void)point;
    return nil; // the game's view under it takes every event
}
- (BOOL)isFlipped {
    return YES;
}
@end

namespace {

// ---- text: the system font through Core Text ------------------------------------------------
std::mutex g_font_mu;
std::map<std::pair<int, int>, CTFontRef> g_fonts;

void *text_font(int px, int weight) {
    std::lock_guard lock(g_font_mu);
    const auto key = std::make_pair(px, weight);
    auto it = g_fonts.find(key);
    if (it != g_fonts.end())
        return (void *)it->second;
    CTFontRef base = CTFontCreateUIFontForLanguage(kCTFontUIFontSystem, px, nullptr);
    if (!base)
        return nullptr;
    CTFontRef font = base;
    if (weight >= 600) {
        // Semibold, as macOS draws its emphasised labels (NSFontWeightSemibold is 0.3).
        NSDictionary *attrs = @{(id)kCTFontTraitsAttribute : @{(id)kCTFontWeightTrait : @(weight >= 700 ? 0.4 : 0.3)}};
        CTFontDescriptorRef d = CTFontDescriptorCreateWithAttributes((__bridge CFDictionaryRef)attrs);
        CTFontRef heavier = CTFontCreateCopyWithAttributes(base, px, nullptr, d);
        CFRelease(d);
        if (heavier) {
            CFRelease(base);
            font = heavier;
        }
    }
    g_fonts[key] = font;
    return (void *)font;
}

void text_metrics(void *font, int *ascent, int *height) {
    CTFontRef f = (CTFontRef)font;
    const double a = CTFontGetAscent(f), d = CTFontGetDescent(f);
    *ascent = int(std::ceil(a));
    *height = int(std::ceil(a + d));
}

CTLineRef make_line(CTFontRef f, const char *utf8) {
    CFStringRef str = CFStringCreateWithCString(nullptr, utf8, kCFStringEncodingUTF8);
    if (!str)
        return nullptr;
    NSDictionary *attrs = @{(id)kCTFontAttributeName : (__bridge id)f, (id)kCTForegroundColorFromContextAttributeName : @YES};
    CFAttributedStringRef as = CFAttributedStringCreate(nullptr, str, (__bridge CFDictionaryRef)attrs);
    CFRelease(str);
    if (!as)
        return nullptr;
    CTLineRef line = CTLineCreateWithAttributedString(as);
    CFRelease(as);
    return line;
}

int text_measure(void *font, const char *utf8) {
    CTLineRef line = make_line((CTFontRef)font, utf8);
    if (!line)
        return 0;
    const double w = CTLineGetTypographicBounds(line, nullptr, nullptr, nullptr);
    CFRelease(line);
    return int(std::lround(w));
}

bool text_render(void *font, const char *utf8, uint8_t **coverage, int *w, int *h, int *x, int *y) {
    CTLineRef line = make_line((CTFontRef)font, utf8);
    if (!line)
        return false;
    const CGRect b = CTLineGetBoundsWithOptions(line, kCTLineBoundsUseGlyphPathBounds);
    const int pad = 2;
    const int x0 = int(std::floor(b.origin.x)) - pad, x1 = int(std::ceil(b.origin.x + b.size.width)) + pad;
    const int y0 = int(std::floor(b.origin.y)) - pad, y1 = int(std::ceil(b.origin.y + b.size.height)) + pad;
    const int bw = x1 - x0, bh = y1 - y0;
    if (b.size.width <= 0 || bw <= 0 || bh <= 0 || bw > 8192 || bh > 1024) {
        CFRelease(line);
        return false;
    }
    uint8_t *data = (uint8_t *)std::calloc(size_t(bw) * size_t(bh), 1);
    CGContextRef ctx = data ? CGBitmapContextCreate(data, bw, bh, 8, bw, nullptr, kCGImageAlphaOnly) : nullptr;
    if (!ctx) {
        std::free(data);
        CFRelease(line);
        return false;
    }
    CGContextSetShouldSmoothFonts(ctx, false);
    CGContextSetShouldAntialias(ctx, true);
    CGContextSetRGBFillColor(ctx, 0, 0, 0, 1);
    CGContextSetTextPosition(ctx, -x0, -y0); // the baseline, bottom-up
    CTLineDraw(line, ctx);
    CGContextRelease(ctx);
    CFRelease(line);
    // Row 0 of the bitmap is its top, y1 above the baseline.
    *coverage = data;
    *w = bw, *h = bh, *x = x0, *y = -y1;
    return true;
}

const MenuTextBackend kText = {text_font, text_metrics, text_measure, text_render};

// ---- what the presenter published -----------------------------------------------------------
std::mutex g_mu;
std::vector<uint8_t> g_pixels; // premultiplied RGBA
int g_w = 0, g_h = 0;
uint64_t g_serial = 0;
bool g_showing = false;
std::atomic<bool> g_active{false};

// ---- the views (main thread) ----------------------------------------------------------------
bool g_gave_up = false;
int g_tries = 0;
NSView *g_metal = nil;
PopMenuGlassContainer *g_container = nil;
NSView *g_dim = nil, *g_glass = nil, *g_page = nil;
uint64_t g_shown_serial = 0;
NSRect g_last_page = NSZeroRect, g_last_glass = NSZeroRect;

NSView *find_metal_view(NSView *v) {
    if ([v.layer isKindOfClass:[CAMetalLayer class]])
        return v;
    for (NSView *sub in v.subviews)
        if (NSView *found = find_metal_view(sub))
            return found;
    return nil;
}

NSView *make_glass() {
    if (@available(macOS 26.0, *)) {
        NSGlassEffectView *g = [[NSGlassEffectView alloc] initWithFrame:NSZeroRect];
        g.style = NSGlassEffectViewStyleRegular;
        // A dark tint keeps the page's white text readable over a bright scene.
        g.tintColor = [NSColor colorWithWhite:0.0 alpha:0.34];
        return g;
    }
    NSVisualEffectView *v = [[NSVisualEffectView alloc] initWithFrame:NSZeroRect];
    v.material = NSVisualEffectMaterialHUDWindow;
    v.blendingMode = NSVisualEffectBlendingModeWithinWindow;
    v.state = NSVisualEffectStateActive;
    v.appearance = [NSAppearance appearanceNamed:NSAppearanceNameVibrantDark];
    v.wantsLayer = YES;
    v.layer.cornerCurve = kCACornerCurveContinuous;
    v.layer.masksToBounds = YES;
    return v;
}

void set_radius(NSView *glass, CGFloat r) {
    if (@available(macOS 26.0, *)) {
        if ([glass isKindOfClass:[NSGlassEffectView class]]) {
            ((NSGlassEffectView *)glass).cornerRadius = r;
            return;
        }
    }
    glass.layer.cornerRadius = r;
}

// The system's accent colour (System Settings > Appearance), for the page's accent.
void accent(uint8_t *r, uint8_t *g, uint8_t *b) {
    NSColor *c = [[NSColor controlAccentColor] colorUsingColorSpace:[NSColorSpace sRGBColorSpace]];
    *r = 10, *g = 132, *b = 255; // macOS blue
    if (!c)
        return;
    *r = uint8_t(std::lround(std::clamp(c.redComponent, 0.0, 1.0) * 255));
    *g = uint8_t(std::lround(std::clamp(c.greenComponent, 0.0, 1.0) * 255));
    *b = uint8_t(std::lround(std::clamp(c.blueComponent, 0.0, 1.0) * 255));
}

void install(SDL_Window *window) {
    if (++g_tries > 600) { // no Metal view after many turns: keep the compositor's page
        g_gave_up = true;
        return;
    }
    NSWindow *nsw = (__bridge NSWindow *)SDL_GetPointerProperty(SDL_GetWindowProperties(window),
                                                                SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, nullptr);
    NSView *metal = nsw ? find_metal_view(nsw.contentView) : nil;
    if (!metal || !metal.superview)
        return;
    g_metal = metal;
    g_container = [[PopMenuGlassContainer alloc] initWithFrame:metal.frame];
    g_container.wantsLayer = YES;
    g_container.hidden = YES;
    g_container.autoresizingMask = metal.autoresizingMask;
    g_dim = [[NSView alloc] initWithFrame:NSZeroRect];
    g_dim.wantsLayer = YES;
    g_dim.layer.backgroundColor = [NSColor colorWithWhite:0.0 alpha:0.28].CGColor;
    g_glass = make_glass();
    g_page = [[NSView alloc] initWithFrame:NSZeroRect];
    g_page.wantsLayer = YES;
    g_page.layer.contentsGravity = kCAGravityResize;
    g_page.layer.minificationFilter = kCAFilterTrilinear;
    [g_container addSubview:g_dim];
    [g_container addSubview:g_glass];
    [g_container addSubview:g_page];
    [metal.superview addSubview:g_container positioned:NSWindowAbove relativeTo:metal];
    uint8_t r, g, b;
    accent(&r, &g, &b);
    mods_menu_set_text_backend(&kText);
    mods_menu_set_style(1, r, g, b);
    g_active = true;
    bool glass26 = false;
    if (@available(macOS 26.0, *))
        glass26 = true;
    fprintf(stderr, "[host] settings menu on native %s\n", glass26 ? "Liquid Glass" : "HUD material");
}

void hide() {
    if (g_container && !g_container.hidden)
        g_container.hidden = YES;
}

CGImageRef make_image(const std::vector<uint8_t> &px, int w, int h) {
    CFDataRef data = CFDataCreate(nullptr, px.data(), CFIndex(px.size()));
    if (!data)
        return nullptr;
    CGDataProviderRef provider = CGDataProviderCreateWithCFData(data);
    CFRelease(data);
    CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGImageRef img = CGImageCreate(size_t(w), size_t(h), 8, 32, size_t(w) * 4, space,
                                   CGBitmapInfo(kCGBitmapByteOrderDefault) | CGBitmapInfo(kCGImageAlphaPremultipliedLast), provider, nullptr,
                                   false, kCGRenderingIntentDefault);
    CGColorSpaceRelease(space);
    CGDataProviderRelease(provider);
    return img;
}

} // namespace

bool host_menu_glass_active(void) {
    return g_active.load(std::memory_order_acquire);
}

void host_menu_glass_publish(int w, int h, const uint8_t *rgba) {
    if (w <= 0 || h <= 0 || !rgba)
        return;
    std::vector<uint8_t> px(size_t(w) * size_t(h) * 4);
    for (size_t i = 0; i < px.size(); i += 4) { // premultiplied, as Core Animation keeps it
        const unsigned a = rgba[i + 3];
        px[i] = uint8_t((rgba[i] * a + 127) / 255);
        px[i + 1] = uint8_t((rgba[i + 1] * a + 127) / 255);
        px[i + 2] = uint8_t((rgba[i + 2] * a + 127) / 255);
        px[i + 3] = uint8_t(a);
    }
    std::lock_guard lock(g_mu);
    g_pixels.swap(px);
    g_w = w, g_h = h;
    ++g_serial;
    g_showing = true;
}

void host_menu_glass_hide(void) {
    std::lock_guard lock(g_mu);
    g_showing = false;
}

void host_menu_glass_tick(void *sdl_window) {
    if (!g_active.load(std::memory_order_relaxed)) {
        static const bool off = [] {
            const char *v = std::getenv("RECOMP_MENU_GLASS");
            return v && *v == '0';
        }();
        if (off || g_gave_up || !sdl_window)
            return;
        @autoreleasepool {
            install(static_cast<SDL_Window *>(sdl_window));
        }
        if (!g_active.load(std::memory_order_relaxed))
            return;
    }
    bool showing;
    uint64_t serial;
    std::vector<uint8_t> px;
    int w = 0, h = 0;
    {
        std::lock_guard lock(g_mu);
        showing = g_showing;
        serial = g_serial;
        if (showing && serial != g_shown_serial) {
            px = g_pixels;
            w = g_w, h = g_h;
        }
    }
    if (!showing) {
        hide();
        return;
    }
    // Where the compositor placed the page, in drawable pixels, and the panel on it.
    int rx = 0, ry = 0, rw = 0, rh = 0;
    float x0 = 0, y0 = 0, x1 = 1, y1 = 1, radius = 0;
    if (!compositor_settings_page_drawn(&rx, &ry, &rw, &rh) || !mods_menu_panel(&x0, &y0, &x1, &y1, &radius))
        return; // not placed yet: the next turn
    @autoreleasepool {
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
        const CGSize drawable = ((CAMetalLayer *)g_metal.layer).drawableSize;
        const NSRect bounds = g_metal.bounds;
        if (drawable.width <= 0 || bounds.size.width <= 0)
            return;
        const double k = bounds.size.width / drawable.width; // points per drawable pixel
        const NSRect page = NSMakeRect(rx * k, ry * k, rw * k, rh * k);
        const NSRect glass = NSMakeRect(page.origin.x + x0 * page.size.width, page.origin.y + y0 * page.size.height,
                                        (x1 - x0) * page.size.width, (y1 - y0) * page.size.height);
        [CATransaction begin];
        [CATransaction setDisableActions:YES];
        if (!NSEqualRects(g_container.frame, g_metal.frame))
            g_container.frame = g_metal.frame;
        if (!NSEqualRects(page, g_last_page)) {
            g_dim.frame = page;
            g_page.frame = page;
            g_last_page = page;
        }
        if (!NSEqualRects(glass, g_last_glass)) {
            g_glass.frame = glass;
            set_radius(g_glass, radius * page.size.height);
            g_last_glass = glass;
        }
        if (!px.empty()) {
            if (CGImageRef img = make_image(px, w, h)) {
                g_page.layer.contents = (__bridge id)img;
                CGImageRelease(img);
            }
            g_shown_serial = serial;
        }
        if (g_container.hidden && g_shown_serial)
            g_container.hidden = NO;
        [CATransaction commit];
    }
}
