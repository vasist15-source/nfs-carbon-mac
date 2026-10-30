#pragma once

#include "../dx/host_api.h"
#include "gpu/gpu.h"
#include <vector>

struct UiElement;
struct UiFrame;

enum CompositorMode { COMP_ENHANCED, COMP_CLASSIC };
struct Anchor {
    int8_t h, v;
}; // -1 left/top, 0 centre, 1 right/bottom
struct SceneMapping {
    float scale_x, scale_y, offset_x, offset_y;
    int domain_w;
};
struct LayoutRect {
    int32_t x = 0, y = 0, w = 0, h = 0;
};
struct LayoutElement {
    uint64_t id = 0;
    uint32_t last_seq = 0;
    LayoutRect guest, drawable;
    bool is_cursor = false;
};
// No frame, UiFrame, texture or pixel pointers cross the presenter/baton seam.
struct LayoutSnapshot {
    uint64_t frame_id = 0;
    std::vector<LayoutElement> elements;
    SceneMapping scene{};
    int drawable_w = 0, drawable_h = 0, guest_w = 0, guest_h = 0, ui_scale = 1;
    HostScreenClass cls = HOST_SCREEN_MENU;
    int scale_override = 0;
    bool legacy = false, classic = false, narrow = false;
};
struct CompositorInput;
LayoutSnapshot compositor_layout_snapshot(const CompositorInput *in);
// Rebuild geometry only, without frame storage, GPU work or presenter waits.
LayoutSnapshot compositor_resize_layout(const LayoutSnapshot &layout, int w, int h);
// Host pointer in drawable pixels. Cursors keep UI-scaled size and no anchor.
void compositor_set_pointer_position(bool valid, int32_t x, int32_t y);

struct CompositorInput {
    HostScreenClass cls;
    const UiFrame *ui;
    gpu::Texture world;
    // Amendment 5: guest UI-space texture, possibly already at UI scale,
    // with premultiplied alpha (rendered by blending onto transparent black).
    // Scene-mapped overlays have already been rendered into world.
    gpu::Texture overlay;
    int guest_w, guest_h;
    int drawable_w, drawable_h;
    int scale_override;
    SceneMapping scene;
    bool legacy;
    gpu::Texture legacy_frame;
    gpu::Texture settings_page; // immutable host-rendered page, alpha outside panel
    // Its size: 640x480 for the classic page (shown at the UI scale); anything else is the tabbed
    // menu, drawn at the game rectangle's size and fitted to the drawable.
    int settings_page_w = 640, settings_page_h = 480;
    // The tabbed menu is on screen in the host's own views (menu_glass.h): no texture to draw, but
    // the page is still placed, so the pointer finds it.
    bool settings_page_native = false;
    bool classic = false; // guest-resolution layered scene, aspect-preserving whole-frame mapping
    // Wide view off: the scene was projected at the guest's aspect, so on a
    // wider drawable it is boxed at that aspect with its HUD and input.
    bool narrow = false;
    // Test86 HDR output: used only when the target is RGBA16F. hdr_scene: this frame's alpha marks the
    // pre-HUD scene, so its highlights expand toward hdr_headroom above hdr_knee (linear); hdr_linear:
    // the legacy frame is an already composed linear image (a re-fit), drawn as it is.
    bool hdr_scene = false, hdr_linear = false;
    float hdr_headroom = 1.0f, hdr_knee = 0.55f;
};

// Encode only: never commits, waits or presents. The caller supplies an open
// command buffer, shader-readable inputs and a distinct render target of
// drawable_w x drawable_h. World is the already mapped scene target; it is
// scaled to the drawable without a second mapping. Textures the compositor
// allocates for UI elements are destroyed when `cb` completes.
void compositor_compose(gpu::Device *device, const CompositorInput *in, gpu::Texture out,
                        gpu::CommandBuffer cb);
int compositor_ui_scale(int drawable_h, int guest_h, int override_);
// Where the settings page was last drawn, in drawable pixels (pointer hit tests on the page).
bool compositor_settings_page_drawn(int *x, int *y, int *w, int *h);
// MetalFX upscaling of the frame's main picture (gpu::Device::spatial_upscale) when it is drawn
// larger than it was rendered; the presenter sets it from the game's setting. Off by default.
void compositor_set_upscaling(bool on);
Anchor compositor_default_anchor(const UiElement *e, int guest_w, int guest_h);
// Overrides preserve the guest margin to the chosen edge (or centre offset).
void compositor_set_anchor(uint64_t element_id, Anchor a);
void compositor_clear_anchor(uint64_t id);
// Returns the unclipped rectangle, also used for input mapping. Menu/FMV and
// legacy ignore anchors. Cursors use scene-mapped position and UI-scaled size.
bool compositor_element_rect_on_drawable(const CompositorInput *in, uint64_t id, int *x, int *y,
                                         int *w, int *h);
// Returns total count; writes at most max ids in ascending last_seq order.
uint32_t compositor_element_ids(const UiFrame *ui, uint64_t *out, uint32_t max);

// One instance per presenter. Amendment 16 needs had_draws, deliberately kept
// outside the binding CompositorInput interface. The handles here are leases'
// keys only: the presenter pins the frame/target lease against pool reuse and
// retires that lease after outstanding GPU work completes (T8).
struct CompositorSceneHistory {
    gpu::Texture world;
    gpu::Texture overlay;
    uint64_t scene_reused = 0;
    int guest_w = 0, guest_h = 0, domain_w = 0;
    bool classic = false;
};
// Called once per sealed frame, not once per display-link repeat. Returns true
// only when old textures were substituted. A class/legacy transition forgets
// the old scene, as does a guest resolution/domain change; the lifetime counter
// remains available to the presenter.
bool compositor_resolve_scene(CompositorSceneHistory *history, CompositorInput *in, bool had_draws);
