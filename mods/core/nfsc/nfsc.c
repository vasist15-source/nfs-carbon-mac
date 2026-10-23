/* nfsc.c - Need for Speed: Carbon's widescreen view (core.nfsc).
 *
 * A port of what ThirteenAG's NFSCarbon.WidescreenFix (MIT; see NOTICE) does
 * to the PC v1.4 NFSC.exe, which is the exact build this port translates
 * (its sub_712AC0 is our 00712ac0). The fix patches code bytes; a recompiled
 * game has no code bytes to patch, so the same changes are made here:
 *
 * RESOLUTION. 00712ac0 (stdcall, int *w, int *h) turns the "racing
 * resolution" option into one of six 4:3/5:4 sizes, and every caller that
 * sizes the device asks it. It is replaced: the option walks a list of the
 * screen's own sizes and the common 4:3, 16:10, 16:9 and 21:9 ones; the
 * default is the screen divided by the whole number nearest 1080 rows.
 *
 * FIELD OF VIEW. The projection setup 0071b780 (thiscall, arg0 = the view;
 * its id at [arg0 + 8]) builds an aspect term from two integers at
 * 0071b858/0071b864 and three constants at 0071b8da (1.0), 0071b8ec (0.5)
 * and 0071b923 (1.0). game.toml redirects those five reads to slots in the
 * zero padding after .rsrc; a hook on entry fills them for the view about to
 * be set up, with the fix's values (Hor+: a wider screen sees more to the
 * sides). With widescreen off, or without this mod, the slots hold the
 * game's own values (data_seeds), so the game is unchanged.
 *
 * HUD (the fix's FixHUD). The HUD's horizontal scale and centre are .rdata
 * and .data floats the recompiled code reads at run time, so they are
 * written once the screen is known; six HUD set-ups that store 320.0 read a
 * slot instead, the game's own widescreen HUD layout is forced on, and the
 * offset its minimap moves by (120.0) is a slot (game.toml). When that layout
 * switches on, FEngHud::SetWideScreenMode (005d5280) has queued its package
 * message; the left and right HUD groups are then moved out by the same
 * offset with the game's FindObject/GetCenter/SetCenter, as the fix does.
 * Not ported yet: the rear-view mirror / picture-in-picture placement, FE
 * menu scaling, and loading-screen pillarboxes.
 *
 * CONTROLLER. The console layout, analog triggers and the controller's own
 * button names on the Controls screen: see pad.h. Console prompts in the
 * menus with the controller's icons: see fe_pad.h. Looking around with the
 * right stick: see camera.h.
 */
#include "pop_mod_api.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

POP_MOD_DECLARE_ABI();

#define GET_RACING_RESOLUTION 0x00712ac0u /* stdcall (int *w, int *h) */
#define INC_SCREEN_RES 0x004a51f0u        /* DALOptions::IncScreenRes */
#define RES_LABEL_HASH 0x004a3830u        /* DALLangHash::GetLocg_RacingResolution */
#define LOCALIZED_WIDE 0x00588f20u        /* GetLocalizedWideString */
#define SETUP_PROJECTION 0x0071b780u      /* thiscall (view *) */
#define GAME_ASPECT_NUM 0x00ab0accu       /* the game's own integers it divides */
#define GAME_ASPECT_DEN 0x00ab0ac8u

/* Slots the redirected reads use (game.toml [translate] operand_redirects). */
#define SLOT_ASPECT_NUM 0x00c2b400u /* int, FILD at 0071b858 */
#define SLOT_ASPECT_DEN 0x00c2b404u /* int, FIDIV at 0071b864 */
#define SLOT_FOV_H 0x00c2b408u      /* float, FDIVR at 0071b8da (1.0) */
#define SLOT_FOV_MUL 0x00c2b40cu    /* float, FMUL at 0071b8ec (0.5) */
#define SLOT_FOV_V 0x00c2b410u      /* float, FDIVR at 0071b923 (1.0) */
#define SLOT_HUD_OFFSET 0x00c2b414u /* float, the minimap's widescreen offset (120.0) */
#define SLOT_HUD_CENTRE 0x00c2b418u /* float, six HUD set-ups' 320.0 */
#define SLOT_CURSOR_WIDTH 0x00c2b41cu /* float, the menu cursor's 640.0 (FMUL at 00731665) */
#define SLOT_PIP_X 0x00c2b420u        /* float, the picture-in-picture's x (PUSH at 00750df4) */

/* The rear-view mirror and picture-in-picture quads (built by 00750b30 (cdecl, quad *, x, y,
 * w, h): 4 vertices 0x18 bytes apart, x at +0) are laid out for 4:3; the fix's MirrorFix
 * narrows their x by (4/3) / aspect so they keep their shape on a wide screen. */
#define BUILD_SCREEN_QUAD 0x00750b30u

/* HUD floats the code reads in place (the fix's ProtectedGameRefs). */
#define HUD_SCALE_X 0x009e8f8cu
#define HUD_WORLD_MAP_X 0x00a604acu
#define HUD_FE_X 0x009c778cu
#define HUD_RACE_ROUTE_X 0x009d5f3cu

/* FEngHud::SetWideScreenMode and the FE helpers it is moved with. */
#define SET_WIDESCREEN_MODE 0x005d5280u /* thiscall (FEngHud *) */
#define FE_FIND_OBJECT 0x005a0250u      /* cdecl (const char *package, uint32 hash) */
#define FE_GET_CENTER 0x00597900u       /* cdecl (object, float *x, float *y) */
#define FE_SET_CENTER 0x00597a70u       /* cdecl (object, float x, float y) */
#define HUD_LEFT_GROUP 0x1603009Eu
#define HUD_RIGHT_GROUP 0x5D0101F1u

static const PopModApi *g_api;
static uint32_t g_hooks[25];
static int g_widescreen = 1;
static int g_scaling = 1;
static uint32_t g_w = 1280, g_h = 720;
static float g_aspect = 16.0f / 9.0f;
static float g_hud_offset = 0.0f;
static uint32_t g_scratch; /* 8 guest bytes for GetCenter */

static uint32_t float_bits(float f) {
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}
static float bits_float(uint32_t u) {
    float f;
    memcpy(&f, &u, 4);
    return f;
}
static void put_float(uint32_t addr, float f) {
    g_api->guest_write_u32(g_api, addr, float_bits(f));
}
static uint32_t get_u32(uint32_t addr) {
    uint32_t v = 0;
    g_api->guest_read_u32(g_api, addr, &v);
    return v;
}
static int64_t setting(const char *key, int64_t fallback) {
    int64_t v = fallback;
    g_api->settings_get(g_api, key, &v);
    return v;
}
static void log_line(const char *text) {
    g_api->log(g_api, text);
}

#include "pad.h" /* controller layout, analog triggers, button names */
#include "fe_pad.h" /* console button prompts in the menus */
#include "hd_ui.h" /* HD fonts and cursor laid out at their original size */
#include "intro.h" /* skip the start-up movies */
#include "movie.h" /* movies wait for their audio (the fix's MoviePlaybackFix) */
#include "online.h" /* hide Quick Match / Custom Match */
#include "rumble.h" /* controller rumble from the game's force-feedback values */
#include "cheats.h" /* infinite nitrous and speedbreaker */
#include "camera.h" /* look around with the right stick */

/* ---- the resolution list --------------------------------------------------
 * The host scales the frame smoothly to the window, aspect kept (game.toml
 * [settings] scaling = "smooth"): a size of the screen's own shape fills it,
 * another shape is shown with bars, and the view and HUD follow the chosen
 * size's aspect. The list: the full-screen size / 1, 2, 3, 4 (at least 480
 * rows; these fill the screen pixel for pixel) and every common size no larger
 * than the screen, smallest first; the default is the screen's size nearest
 * 1080 rows. The option's index (00a65388) walks
 * this list; the menu's labels come from here; the registry keeps the choice
 * packed as 0x80000000 | w << 16 | h (as the fix saves it), so a different
 * list on another Mac still finds it. The host knows the screen only once its
 * window is up, after pop_mod_init, so the list is built when the game first
 * asks for its resolution (device creation). */
#define RES_INDEX 0x00a65388u        /* the game's "racing resolution" option */
#define TABLE_RES_SUPPORTED 0x00c2b500u /* u32[RES_MAX], redirected readers */
#define TABLE_RES_REFRESH 0x00c2b5a0u   /* u32[RES_MAX], 0 = the default rate */
#define SLOT_RES_SAVE 0x00c2b640u       /* what the registry write stores */
#define RES_MAX 40
typedef struct {
    uint32_t w, h, hash;
    char label[32];
} ResEntry;
static ResEntry g_res[RES_MAX];
/* Common sizes: 4:3, 16:10, 16:9, 21:9. */
static const uint16_t kCommon[][2] = {
    {640, 480},   {800, 600},   {1024, 768},  {1280, 960},  {1600, 1200}, {1280, 800},
    {1440, 900},  {1680, 1050}, {1920, 1200}, {2560, 1600}, {1280, 720},  {1366, 768},
    {1600, 900},  {1920, 1080}, {2560, 1440}, {3200, 1800}, {3840, 2160}, {2560, 1080},
    {3440, 1440},
};
static int g_res_count = 0, g_res_default = 0, g_res_ready = 0;
static int g_shape_known = 0;
static uint32_t g_applied_w, g_applied_h;

/* The engine's string hash (bStringHash), for the labels' keys. */
static uint32_t string_hash(const char *t) {
    uint32_t h = 0xffffffffu;
    while (*t)
        h = h * 33u + (uint8_t)*t++;
    return h;
}

static void screen_shape(void) {
    if (g_shape_known)
        return;
    uint32_t sw = 0, sh = 0;
    if (!g_api->screen_size || g_api->screen_size(g_api, &sw, &sh) != POP_OK || !sw || !sh)
        return;
    g_aspect = (float)sw / (float)sh;
    uint32_t best = (uint32_t)lround((double)sh / 1080.0);
    if (best < 1)
        best = 1;
    uint32_t cand[RES_MAX][3]; /* w, h, k (0 = a common size) */
    int n = 0;
    for (uint32_t k = 1; k <= 4; ++k)
        if (k == 1 || sh / k >= 480) {
            cand[n][0] = sw / k;
            cand[n][1] = sh / k;
            cand[n++][2] = k;
        }
    for (size_t c = 0; c < sizeof kCommon / sizeof kCommon[0] && n < RES_MAX; ++c) {
        const uint32_t w = kCommon[c][0], h = kCommon[c][1];
        int dup = 0;
        for (int j = 0; j < n; ++j)
            dup |= cand[j][0] == w && cand[j][1] == h;
        if (!dup && w <= sw && h <= sh) {
            cand[n][0] = w;
            cand[n][1] = h;
            cand[n++][2] = 0;
        }
    }
    /* Smallest first (by width, then height). */
    for (int i = 1; i < n; ++i)
        for (int j = i; j > 0 && (cand[j][0] < cand[j - 1][0] ||
                                  (cand[j][0] == cand[j - 1][0] && cand[j][1] < cand[j - 1][1]));
             --j)
            for (int f = 0; f < 3; ++f) {
                const uint32_t t = cand[j][f];
                cand[j][f] = cand[j - 1][f];
                cand[j - 1][f] = t;
            }
    g_res_count = 0;
    for (int i = 0; i < n; ++i) {
        ResEntry *e = &g_res[g_res_count];
        char key[48];
        e->w = cand[i][0];
        e->h = cand[i][1];
        snprintf(e->label, sizeof e->label, cand[i][2] == 1 ? "%ux%u (native)" : "%ux%u", e->w, e->h);
        snprintf(key, sizeof key, "NFSC_NATIVE_RES_%uX%u", e->w, e->h);
        e->hash = string_hash(key);
        if (cand[i][2] == best)
            g_res_default = g_res_count;
        ++g_res_count;
    }
    for (uint32_t i = 0; i < RES_MAX; ++i) {
        g_api->guest_write_u32(g_api, TABLE_RES_SUPPORTED + 4u * i, (int)i < g_res_count);
        g_api->guest_write_u32(g_api, TABLE_RES_REFRESH + 4u * i, 0);
    }
    g_shape_known = 1;
    char line[1024];
    n = snprintf(line, sizeof line, "core.nfsc: screen %ux%u, resolutions", sw, sh);
    for (int i = 0; i < g_res_count && n < (int)sizeof line; ++i)
        n += snprintf(line + n, sizeof line - n, " %ux%u%s", g_res[i].w, g_res[i].h, i == g_res_default ? "*" : "");
    log_line(line);
}

static uint32_t packed(int i) {
    return 0x80000000u | g_res[i].w << 16 | g_res[i].h;
}
/* The option as an index into g_res: a packed size the registry loaded is
 * looked up once (the original game's 0..5 means "the default"). */
static int res_index(void) {
    if (!g_res_count)
        return -1;
    uint32_t v = get_u32(RES_INDEX);
    if (!g_res_ready) {
        int found = g_res_default;
        if (v & 0x80000000u)
            for (int i = 0; i < g_res_count; ++i)
                if (packed(i) == v)
                    found = i;
        v = (uint32_t)found;
        g_api->guest_write_u32(g_api, RES_INDEX, v);
        g_res_ready = 1;
    }
    if (v >= (uint32_t)g_res_count)
        v = (uint32_t)g_res_default;
    g_api->guest_write_u32(g_api, SLOT_RES_SAVE, packed((int)v));
    return (int)v;
}

/* The fix's onResChange for the HUD, for this game size. */
static void apply_hud(void) {
    const float scale = (1.0f / (float)g_w * ((float)g_h / 480.0f)) * 2.0f;
    const float centre = 1.0f / scale; /* 640 / (640 * scale): 320 at 4:3 */
    put_float(HUD_SCALE_X, scale);
    put_float(HUD_WORLD_MAP_X, centre);
    put_float(HUD_FE_X, centre);
    put_float(HUD_RACE_ROUTE_X, centre - 320.0f + 384.0f);
    put_float(SLOT_HUD_CENTRE, centre);
    put_float(SLOT_CURSOR_WIDTH, 2.0f * centre); /* the menu spans 0 .. 2 * centre */
    g_hud_offset = 240.0f * g_aspect - 320.0f; /* -CalculateWidescreenOffset(w, h, 640, 480) */
    put_float(SLOT_HUD_OFFSET, g_hud_offset);
    put_float(SLOT_PIP_X, ((320.0f + g_hud_offset) / 320.0f - 1.042f) / -2.0f); /* fPiPUCAPPosition */
    /* The fix's Rendering.ixx: rain droplets (0.75/aspect * 4/3 = 1/aspect wide, at its
     * RainDropletsScale 0.5; game.toml patches 00722cb0 to read the slots) and the arrest
     * blur's per-pixel step, read in place at 007231a2. */
    put_float(0x00c2b428u, 0.5f / g_aspect);
    put_float(0x00c2b42cu, 0.5f);
    put_float(0x009d0584u, (1.0f / 640.0f) * ((4.0f / 3.0f) / g_aspect));
    char line[128];
    snprintf(line, sizeof line, "core.nfsc: HUD scale %.6f, centre %.1f, side offset %.1f", scale, centre,
             g_hud_offset);
    log_line(line);
}

static uint32_t g_quad;
static void quad_enter(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)api;
    (void)inv;
    (void)user;
    g_quad = get_u32(cpu->esp + 4);
}
static void quad_leave(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)api;
    (void)cpu;
    (void)inv;
    (void)user;
    if (!g_quad)
        return;
    const float narrow = (4.0f / 3.0f) / g_aspect;
    for (uint32_t v = 0; v < 4; ++v)
        put_float(g_quad + 0x18u * v, bits_float(get_u32(g_quad + 0x18u * v)) * narrow);
    g_quad = 0;
}

/* Each group's centre as the game laid it out, so moving it twice never adds up. */
static struct {
    uint32_t object;
    float x, y;
} g_group[2];

static uint32_t call(uint32_t fn, const uint32_t *args, uint32_t n) {
    uint32_t eax = 0;
    g_api->guest_call(g_api, fn, 0, args, n, &eax);
    return eax;
}

static void move_group(uint32_t package, uint32_t hash, int slot, float dx) {
    const uint32_t find[2] = {package, hash};
    const uint32_t object = call(FE_FIND_OBJECT, find, 2);
    if (!object || !g_scratch)
        return;
    if (g_group[slot].object != object) {
        const uint32_t get[3] = {object, g_scratch, g_scratch + 4};
        call(FE_GET_CENTER, get, 3);
        g_group[slot].object = object;
        g_group[slot].x = bits_float(get_u32(g_scratch));
        g_group[slot].y = bits_float(get_u32(g_scratch + 4));
    }
    const uint32_t set[3] = {object, float_bits(g_group[slot].x + dx), float_bits(g_group[slot].y)};
    call(FE_SET_CENTER, set, 3);
}

/* FEngHud::SetWideScreenMode: remember the FEngHud and its widescreen byte on
 * entry (ECX is not kept across the call); when the byte has turned on, the
 * layout message is queued and the side groups are moved out. */
static uint32_t g_hud_self;
static uint8_t g_wide_before;
static void widescreen_mode_enter(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv;
    (void)user;
    g_hud_self = cpu->ecx;
    g_wide_before = 0;
    if (g_hud_self)
        api->guest_read_u8(api, g_hud_self + 0x341, &g_wide_before);
}
static void widescreen_mode_leave(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)cpu;
    (void)inv;
    (void)user;
    if (!g_hud_self || !g_widescreen)
        return;
    uint8_t wide = 0;
    api->guest_read_u8(api, g_hud_self + 0x341, &wide);
    if (!wide || g_wide_before)
        return; /* only when the widescreen layout has just been switched on */
    const uint32_t package = get_u32(g_hud_self + 0x2c);
    if (!package)
        return;
    move_group(package, HUD_LEFT_GROUP, 0, -g_hud_offset);
    move_group(package, HUD_RIGHT_GROUP, 1, g_hud_offset);
}

static void get_racing_resolution(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv,
                                  void *user) {
    (void)inv;
    (void)user;
    screen_shape();
    const int i = res_index();
    if (i >= 0) {
        g_w = g_res[i].w;
        g_h = g_res[i].h;
        g_aspect = (float)g_w / (float)g_h; /* the view and HUD follow the chosen shape */
    }
    if (g_w != g_applied_w || g_h != g_applied_h) {
        apply_hud();
        g_applied_w = g_w;
        g_applied_h = g_h;
        char line[96];
        snprintf(line, sizeof line, "core.nfsc: game resolution %ux%u", g_w, g_h);
        log_line(line);
    }
    const uint32_t pw = get_u32(cpu->esp + 4), ph = get_u32(cpu->esp + 8);
    if (pw)
        api->guest_write_u32(api, pw, g_w);
    if (ph)
        api->guest_write_u32(api, ph, g_h);
    api->hook_return(api, cpu, cpu->eax, 8); /* stdcall: two arguments */
}

/* DALOptions::IncScreenRes (004a51f0, stdcall char (int step)): the options
 * screen's left/right on the resolution row, over this list. */
static void inc_screen_res(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv;
    (void)user;
    screen_shape();
    int i = res_index();
    if (i >= 0) {
        const int32_t step = (int32_t)get_u32(cpu->esp + 4);
        i = ((i + (step < 0 ? -1 : 1)) % g_res_count + g_res_count) % g_res_count;
        api->guest_write_u32(api, RES_INDEX, (uint32_t)i);
        api->guest_write_u32(api, SLOT_RES_SAVE, packed(i));
    }
    api->hook_return(api, cpu, (cpu->eax & 0xffffff00u) | 1u, 4);
}

/* The label hash for list entry i (004a3830, stdcall char (uint32 *hash, int i)). */
static void res_label_hash(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv;
    (void)user;
    screen_shape();
    const uint32_t out = get_u32(cpu->esp + 4);
    const uint32_t i = get_u32(cpu->esp + 8);
    uint32_t ok = 0;
    if (out && i < (uint32_t)g_res_count) {
        api->guest_write_u32(api, out, g_res[i].hash);
        ok = 1;
    }
    api->hook_return(api, cpu, (cpu->eax & 0xffffff00u) | ok, 8);
}

/* The localised text for a hash (00588f20, cdecl char (wchar_t *out, int size,
 * uint32 hash)): this list's labels, everything else the game's own. */
static void localized_wide(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv;
    (void)user;
    const uint32_t out = get_u32(cpu->esp + 4), size = get_u32(cpu->esp + 8), hash = get_u32(cpu->esp + 12);
    for (int i = 0; i < g_res_count && out && size; ++i) {
        if (g_res[i].hash != hash)
            continue;
        const char *t = g_res[i].label;
        size_t n = strlen(t);
        if (n + 1 > size)
            n = size - 1;
        void *p = NULL;
        if (api->guest_ptr(api, out, (uint32_t)(2 * (n + 1)), &p) != POP_OK)
            break;
        uint8_t *b = (uint8_t *)p;
        for (size_t k = 0; k < n; ++k) {
            b[2 * k] = (uint8_t)t[k];
            b[2 * k + 1] = 0;
        }
        b[2 * n] = b[2 * n + 1] = 0;
        api->hook_return(api, cpu, (cpu->eax & 0xffffff00u) | 1u, 0); /* cdecl */
        return;
    }
    api->call_original(api, cpu->target, cpu);
}

/* The original values: what the unmodified game reads at these sites. */
static void fov_original(void) {
    g_api->guest_write_u32(g_api, SLOT_ASPECT_NUM, get_u32(GAME_ASPECT_NUM));
    g_api->guest_write_u32(g_api, SLOT_ASPECT_DEN, get_u32(GAME_ASPECT_DEN));
    put_float(SLOT_FOV_H, 1.0f);
    put_float(SLOT_FOV_MUL, 0.5f);
    put_float(SLOT_FOV_V, 1.0f);
}

static void setup_projection(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv,
                             void *user) {
    (void)api;
    (void)inv;
    (void)user;
    if (!g_widescreen) {
        fov_original();
        return;
    }
    const uint32_t view = get_u32(cpu->esp + 4);
    const uint32_t id = view ? get_u32(view + 8) : 0;
    /* The aspect term: 1.0, or 0.925 for the rear-view mirror (view 3),
     * written as the integer ratio the game divides. */
    g_api->guest_write_u32(g_api, SLOT_ASPECT_NUM, id == 3 ? 37u : 1u);
    g_api->guest_write_u32(g_api, SLOT_ASPECT_DEN, id == 3 ? 40u : 1u);
    float hor = 1.0f / (g_aspect / (4.0f / 3.0f));
    float vscale = 1.205f;
    if (g_scaling) {
        hor /= 1.034482718f;
        vscale = g_scaling == 2 ? 1.28f : 1.225f;
    }
    if (id == 1 || id == 4) { /* the main view and its reflections */
        put_float(SLOT_FOV_H, hor);
        put_float(SLOT_FOV_MUL, 0.434665f);
        put_float(SLOT_FOV_V, vscale);
    } else {
        put_float(SLOT_FOV_H, 1.0f);
        put_float(SLOT_FOV_MUL, 0.375f);
        put_float(SLOT_FOV_V, 1.0f);
    }
}


static PopModStatus install(uint32_t addr, PopHookFn fn, int32_t mode, int slot) {
    PopModStatus s = g_api->hook_install(g_api, addr, fn, mode, 0, &g_hooks[slot]);
    if (s != POP_OK) {
        char line[96];
        snprintf(line, sizeof line, "core.nfsc: hook %08x failed (%d)", addr, (int)s);
        log_line(line);
    }
    return s;
}

PopModStatus pop_mod_init(const PopModApi *api) {
    g_api = api;
    g_widescreen = setting("widescreen", 1) != 0;
    g_scaling = (int)setting("fov_scaling", 1);
    if (g_scaling < 0 || g_scaling > 2)
        g_scaling = 1;
    fov_original();
    pad_install();
    fe_pad_install(api);
    hd_ui_install();
    intro_install();
    movie_install();
    online_install();
    rumble_install();
    cheats_install();
    camera_install();
    PopModStatus s = install(SETUP_PROJECTION, setup_projection, POP_HOOK_BEFORE, 0);
    if (s != POP_OK)
        return s;
    if (g_widescreen) {
        if ((s = install(GET_RACING_RESOLUTION, get_racing_resolution, POP_HOOK_REPLACE, 1)) != POP_OK)
            return s;
        /* The options menu's resolution row; widescreen works without it. */
        if (install(INC_SCREEN_RES, inc_screen_res, POP_HOOK_REPLACE, 4) != POP_OK ||
            install(RES_LABEL_HASH, res_label_hash, POP_HOOK_REPLACE, 5) != POP_OK ||
            install(LOCALIZED_WIDE, localized_wide, POP_HOOK_REPLACE, 6) != POP_OK)
            log_line("core.nfsc: the resolution menu is unavailable; widescreen still applies");
        if (api->guest_alloc(api, 8, &g_scratch) != POP_OK)
            g_scratch = 0;
        /* The mirror and picture-in-picture shapes; widescreen works without them. */
        if (install(BUILD_SCREEN_QUAD, quad_enter, POP_HOOK_BEFORE, 13) != POP_OK ||
            install(BUILD_SCREEN_QUAD, quad_leave, POP_HOOK_AFTER, 15) != POP_OK)
            log_line("core.nfsc: the mirror is not narrowed for the wide screen");
        if ((s = install(SET_WIDESCREEN_MODE, widescreen_mode_enter, POP_HOOK_BEFORE, 2)) != POP_OK ||
            (s = install(SET_WIDESCREEN_MODE, widescreen_mode_leave, POP_HOOK_AFTER, 3)) != POP_OK)
            return s;
    }
    char line[128];
    snprintf(line, sizeof line, "core.nfsc: widescreen %s, view scaling %d%s", g_widescreen ? "on" : "off",
             g_scaling, g_shape_known ? "" : " (screen size not known yet; asked again at device creation)");
    log_line(line);
    return POP_OK;
}

PopModStatus pop_mod_exit(void) {
    impact_feedback_stop(g_api);
    for (unsigned i = 0; i < sizeof g_hooks / sizeof g_hooks[0]; ++i)
        if (g_hooks[i]) {
            g_api->hook_remove(g_api, g_hooks[i]);
            g_hooks[i] = 0;
        }
    return POP_OK;
}
