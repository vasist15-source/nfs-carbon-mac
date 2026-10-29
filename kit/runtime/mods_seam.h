// mods_seam.h - the one boundary the runtime, the DirectX shims and the hosts
// use to reach the mod foundation.
//
// Every name here is also declared in mods/mods_internal.h with the
// identical signature and identical (C) linkage; the mods module defines them
// strongly and mods_seam.cpp defines them weakly, so a build without the mods
// module - the runtime tests, the parity fixture with mods disabled - links
// and behaves exactly as it did before the foundation existed.
//
// mods/tests/seam_contract_test.cpp includes both headers, so a
// signature that drifts is a compile error rather than two symbols.
#pragma once
#include <stddef.h>
#include <stdint.h>

struct PopTextureReplacement;

extern "C" {

// Presentation transition notification. No-op in hosts without a presenter.
void mods_present_level_end(void);
// Test86: a declared setting's current value by "mod_id/key" (the presenter's HDR output reads
// core.nfsmw/hdr_output); `fallback` when no such setting, or without the mods module.
int64_t mods_setting_value(const char *mod_id_slash_key, int64_t fallback);
// Thread-safe application Settings command, drained by the guest frame loop.
void mods_options_request(void);

// Lifecycle, called by the host.
bool mods_load_all(void);
// Shutdown is two steps, because the teardown cannot run while a guest worker
// is still going. mods_shutdown_request revokes every mod at once, so nothing
// a surviving worker does reaches a plugin; mods_shutdown_complete runs the
// teardown - pop_mod_exit, reclamation, unload - the first time quiescence
// holds and reports true from then on. A host asks, lets its workers finish,
// and polls until it is true. mods_shutdown is the single-call form, correct
// only when the caller has already stopped them.
void mods_shutdown_request(void);
bool mods_shutdown_complete(void);
void mods_shutdown(void);
void mods_host_set_main_thread(void);
// How many mods the loader has a record of. Zero means the mods directory was
// empty or absent, which a host needs to know: with nothing loaded there is no
// settings page to show, and registering its keyboard would eat F10 and every
// navigation key in a run that is meant to behave as if the foundation were
// not there.
uint32_t mods_record_count(void);
// Writes the run record: the mod set, the build identity and the input
// metadata for this run. EVERY run records, including one with mods disabled
// or an empty directory, which records an explicit empty mod set - a run whose
// metadata is simply absent cannot be told from one that was never made. The
// loader writes it on its own shutdown; a host that never reached the loader
// writes it itself.
bool mods_write_run_record(const char *path);
// Applies registry mutations queued from a non-guest thread. Called from the
// scheduler's checkpoint and from the host's tick; cheap when nothing is
// queued.
void mods_registry_pump(void);
// Signal-safe. A preformatted description of the mod callback running on this
// thread, or "". Never allocates, never formats, safe from a fault handler.
const char *mods_active_callback_desc(void);
// A guest longjmp or a thread exit jumped over mod invocations on this thread.
void mods_hooks_unwind_to_esp(uint32_t esp);
// Called by mods_hooks_unwind_to_esp when a guest longjmp abandoned hook
// frames, so a capture window opened inside one of them is closed rather than
// left with the arena inaccessible and a fault handler installed. The capture
// harness checks this thread's surviving depth against its owning frame.
void mods_capture_unwound(void);
// How deep this thread is in mod hook invocations. Zero means no mod callback
// is on this stack, which is the scheduler's condition for doing anything that
// a mod callback must not see happen underneath it - dispatching input, above
// all. Weakly zero without the module, which is the truth there.
uint32_t mods_hook_depth(void);

// Input, consulted by a host BEFORE the guest sees anything. True = consumed.
bool mods_input_key(uint8_t dik, uint8_t vk, bool down);
bool mods_input_button(int button, bool down, int32_t x, int32_t y);
bool mods_input_motion(int32_t x, int32_t y, int32_t dx, int32_t dy);
bool mods_input_wheel(int32_t dz);
void mods_input_release_all(void);

// The settings page. A host that can draw the page registers its keyboard
// once, before the first frame it draws: the loader does not, because a host
// that never presents has no page to drive. Idempotent - it replaces the
// runtime's own key handler rather than adding a second one.
void mods_page_init(void);
// Whether the page is showing. A host releases the pointer while it is, so the
// user can reach the page's own settings with a real cursor.
bool mods_page_visible(void);
// Drawn over a presented frame by the host.
void mods_page_draw(void *pixels, int w, int h, int bpp, int pitch, const uint32_t *palette);
// The tabbed settings menu (game.toml [settings] page = "tabs"), drawn as straight-alpha RGBA
// at w x h. 0: the menu is not open. 1: unchanged since *generation, nothing drawn. 2: redrawn;
// *pixels is w*h*4 bytes, valid until the next call, and *generation names it. One caller at a
// time: the thread sealing a frame.
int mods_menu_render(int w, int h, uint64_t *generation, const uint8_t **pixels);
// The pointer over that menu, as fractions (u, v) of the page the presenter last drew. button -1
// is motion, else 0/1/2 with down; wheel is in WM_MOUSEWHEEL units. 1 = consumed (the menu is
// open), 0 = the menu is not open.
int mods_menu_pointer(float u, float v, int button, int down, int wheel);
// A host's own text drawing for that menu (the macOS host: the system font through Core Text);
// without one, or for a size it has no font for, the menu keeps the kit's Open Sans. Called on
// the thread that draws the menu. render gives the whole string's coverage (malloc'd, w*h bytes,
// the caller frees it) with its top-left at (x, y) from the pen on the baseline.
struct MenuTextBackend {
    void *(*font)(int px, int weight);
    void (*metrics)(void *font, int *ascent, int *height);
    int (*measure)(void *font, const char *utf8);
    bool (*render)(void *font, const char *utf8, uint8_t **coverage, int *w, int *h, int *x, int *y);
};
void mods_menu_set_text_backend(const struct MenuTextBackend *backend);
// The menu's look. glass 0: the kit's own (the screen dimmed, a dark panel). glass 1: the host
// puts a glass panel behind the page, so the page leaves the panel clear and draws only what is on
// it, with (r, g, b) as the accent.
void mods_menu_set_style(int glass, uint8_t r, uint8_t g, uint8_t b);
// The last drawn page's panel as fractions of the page, and its corner radius as a fraction of the
// page's height. 0 before a page was drawn.
int mods_menu_panel(float *x0, float *y0, float *x1, float *y1, float *radius);

// Texture override, consulted by the DirectX shim at upload.
int mods_texture_override(uint64_t hash64, int32_t w, int32_t h, int32_t format,
                          uint8_t **out_rgba8, uint32_t *out_bytes);

int mods_texture_override_ex(uint64_t hash64, int32_t w, int32_t h, int32_t format,
                             PopTextureReplacement *out);

} // extern "C"
