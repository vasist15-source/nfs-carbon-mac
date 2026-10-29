#include "page_overlay.h"
#include "menu_glass.h"
#include "../runtime/mods_seam.h"

#include <vector>

namespace {
bool g_enabled = false;
}

void host_page_set_enabled(bool enabled) {
    g_enabled = enabled;
}
bool host_page_enabled() {
    return g_enabled;
}

const void *host_page_overlay(const void *pixels, int w, int h, int bpp, int pitch,
                              const uint32_t *palette) {
    // With mods off this is not merely a frame nobody draws on: registering
    // the page's keyboard would consume F10 and, once open, every navigation
    // key, so a run with mods disabled would not deliver the input a run
    // without the foundation delivered. Nothing here happens at all.
    if (!g_enabled)
        return pixels;

    // The page's keyboard is registered here, on the first frame a host draws,
    // and nowhere else. The loader used to do it and no longer does: a host
    // that never presents has no page to drive, and every host that CAN draw
    // one reaches this function. First frame, not load time, because the
    // loader has certainly run by then - it runs before the entry point.
    static bool page_ready = false;
    if (!page_ready) {
        page_ready = true;
        mods_page_init();
    }

    if (!pixels || w <= 0 || h <= 0)
        return pixels;
    int stride = pitch > 0 ? pitch : w * (bpp / 8);
    if (stride <= 0)
        return pixels;

    // One buffer per thread. A presenter is called from the guest thread that
    // presented, and two of them presenting at once must not share storage.
    static thread_local std::vector<uint8_t> copy;
    size_t bytes = (size_t)stride * (size_t)h;
    const uint8_t *src = (const uint8_t *)pixels;
    copy.assign(src, src + bytes);
    // A no-op when the page is hidden, which is the usual case; the copy is
    // taken anyway so there is one path and not two, and so the "we never
    // write through the caller's pointer" rule holds without a condition.
    mods_page_draw(copy.data(), w, h, bpp, stride, palette);
    return copy.data();
}

bool host_page_rgba(std::vector<uint8_t> *rgba) {
    if (!rgba)
        return false;
    rgba->clear();
    if (!g_enabled || !mods_page_visible())
        return false;
    std::vector<uint16_t> pixels(640 * 480, 0);
    mods_page_draw(pixels.data(), 640, 480, 16, 640 * 2, nullptr);
    rgba->resize(640 * 480 * 4);
    for (size_t i = 0; i < pixels.size(); ++i) {
        uint16_t c = pixels[i];
        (*rgba)[4 * i] = uint8_t(((c >> 11) & 31) * 255 / 31);
        (*rgba)[4 * i + 1] = uint8_t(((c >> 5) & 63) * 255 / 63);
        (*rgba)[4 * i + 2] = uint8_t((c & 31) * 255 / 31);
        (*rgba)[4 * i + 3] = c ? 255 : 0;
    }
    return true;
}

int host_page_menu(int w, int h, uint64_t *generation, const uint8_t **pixels) {
    if (!g_enabled)
        return 0;
    return mods_menu_render(w, h, generation, pixels);
}

// The macOS host defines these (sdl/menu_glass_mac.mm); every other host keeps the compositor's page.
__attribute__((weak)) bool host_menu_glass_active(void) {
    return false;
}
__attribute__((weak)) void host_menu_glass_publish(int, int, const uint8_t *) {}
__attribute__((weak)) void host_menu_glass_hide(void) {}
__attribute__((weak)) void host_menu_glass_tick(void *) {}
