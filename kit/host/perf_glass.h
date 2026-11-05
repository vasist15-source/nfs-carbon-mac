// perf_glass.h - the performance overlay as native macOS views (the macOS desktop host).
//
// On macOS the counters and the frame-time graph are AppKit views over the game's Metal view: a
// Liquid Glass panel (NSGlassEffectView, macOS 26 and later; the HUD material before it) in the
// top-right corner, the numbers in the system font with monospaced digits, the graph as Core
// Animation shape layers. The presenter only hands over numbers (host_perf_glass_publish, about
// four times a second); the main thread's tick moves them into the views. When the views are in
// charge the presenter skips its Metal overlay (performance_overlay.cpp). Every other host, a capture
// run (its marker lines stay on the Metal overlay) and RECOMP_PERF_GLASS=0 keep the Metal overlay:
// the weak defaults in performance_overlay.cpp do nothing.
#pragma once
#include "frame_pacing.h"

// Presenter thread, every presented frame. `mode` 0 off, 1 counters, 2 counters and graph; `limit`
// the frame limit (0 = the game's own). True when the native views draw the overlay, so the caller
// must not draw it as well.
bool host_perf_glass_publish(int mode, const FramePacingSnapshot &s, int limit, double now);
// The main thread's turn (pump/idle_wait in sdl/main.cpp): makes the views on the first call with a
// window, then shows the latest numbers.
void host_perf_glass_tick(void *sdl_window);
