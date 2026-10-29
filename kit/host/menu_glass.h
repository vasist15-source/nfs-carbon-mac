// menu_glass.h - the tabbed settings menu on a native glass panel (the macOS host).
//
// On macOS the menu is shown in AppKit views over the game's Metal view: Liquid Glass
// (NSGlassEffectView, macOS 26 and later) or the HUD material before it behind the panel, and the
// page the kit draws in its glass look (settings_menu_draw.cpp, mods_menu_set_style) on top. The
// compositor then leaves the page out of the frame but still works out where it is, which is where
// the pointer is mapped (CompositorInput::settings_page_native, compositor_settings_page_drawn).
// Every other host, and a Mac where the views cannot be made (RECOMP_MENU_GLASS=0 turns them off),
// keeps the compositor's page: the defaults in page_overlay.cpp do nothing.
#pragma once
#include <stdint.h>

// True once the views exist: the presenter then hands pages here instead of drawing them.
bool host_menu_glass_active(void);
// A newly drawn page (w*h straight-alpha RGBA, copied), on the thread that seals frames.
void host_menu_glass_publish(int w, int h, const uint8_t *rgba);
// The menu closed.
void host_menu_glass_hide(void);
// The main thread's turn: makes the views on the first call with a window, then shows what was
// published where the compositor placed the page.
void host_menu_glass_tick(void *sdl_window);
