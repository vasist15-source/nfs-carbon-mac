/* pad.h - controller layout, analog triggers and button names (core.nfsc).
 *
 * The kit hands the game one DirectInput gamepad, buttons numbered by
 * position (game.toml [controls.native]): 0 west, 1 south, 2 east, 3 north,
 * 4 L1, 5 R1, 6 L2, 7 R2, 8 select, 9 start, 10 L3, 11 R3, 12 home. Axes:
 * X Y Z = left stick X/Y, right stick X; Rx Ry = L2 R2; Rz = right stick Y.
 * Carbon has no layout for such a pad, so it uses its generic PC one (gas on
 * the west button, nothing on the right stick).
 *
 * BINDINGS. 72 rows at 0xa98700, {name, triples *}; triples 0/1 are the
 * keyboard, 2p+2 / 2p+3 controller port p's primary/secondary, each
 * {kind, port, control}. Kinds: 1 key, 2 button, 3 POV (0 up 1 right 2 down
 * 3 left), 4/5 translation axis -/+, 6/7 rotation axis -/+. 00689550
 * (thiscall, int port, RET 4) writes rows 0..46 of a port with the game's
 * defaults for that device: at device creation (00694c50), Restore Defaults on
 * the Controls screen (005c8960) and a full reset (004b01f0).
 *
 * The console layout (XtendedInput's Carbon defaults, by position) is the
 * DEFAULT, not a rule: the hook remembers what the game's own defaults were
 * and writes the layout over them. Each frame, a port whose rows are exactly
 * the game's defaults (a fresh profile, Restore Defaults, or a profile saved
 * before this layout existed) gets the layout; any remap made on Options ->
 * Controls makes the rows differ, so they are left alone and saved as usual.
 *
 * TRIGGERS. The game reads Rx/Ry as centred axes, so a trigger would count
 * only in the second half of its travel. 00692240 (the per-frame update,
 * values at [this + 0x20], one float per row) is wrapped: afterwards every
 * row bound to a trigger axis gets the trigger's full travel, as a pedal.
 *
 * NAMES. 00679ad0 (cdecl: char *out, int device, int row, char primary, int
 * port) names a binding on the Controls screen; for the controller it showed
 * DirectInput's "Button 1" / "Z Rotation". It now shows the controller's own
 * names: Xbox, PlayStation (DualShock/DualSense) or Nintendo Switch Pro,
 * chosen by the controller_buttons setting or, on Auto, from SDL (linked
 * into the host binary, symbols exported) asked which pad is connected. */
#include <dlfcn.h>

#define PAD_TABLE 0x00a98704u        /* rows, stride 8: triples pointer */
#define PAD_DEVICES 0x00a986d0u      /* device per port */
#define PAD_PORT_COUNT 0x00a98948u
#define PAD_SET_DEFAULTS 0x00689550u
#define PAD_UPDATE 0x00692240u
#define PAD_NAME 0x00679ad0u
#define PAD_DEFAULT_ROWS 47u
#define PAD_ROWS 72u
#define PAD_MAX_PORTS 4u
#define PAD_STATE_RX 0x20cu          /* in the device's state: L2, 0..65535 */

static uint32_t g_pad_generic[PAD_MAX_PORTS][PAD_DEFAULT_ROWS][2][3];
static int g_pad_generic_valid[PAD_MAX_PORTS];
static uint32_t g_pad_seen[PAD_MAX_PORTS][PAD_DEFAULT_ROWS][2][3];
static int g_pad_seen_valid[PAD_MAX_PORTS], g_pad_reports;

static uint32_t pad_triple(unsigned row, unsigned slot) {
    uint32_t rows = get_u32(PAD_TABLE + row * 8u);
    return rows ? rows + slot * 12u : 0;
}
static void pad_set(unsigned port, unsigned row, unsigned secondary, uint32_t kind, uint32_t control) {
    uint32_t at = pad_triple(row, 2u * port + 2u + secondary);
    if (!at)
        return;
    g_api->guest_write_u32(g_api, at, kind);
    g_api->guest_write_u32(g_api, at + 4, kind ? port : 0);
    g_api->guest_write_u32(g_api, at + 8, kind ? control : 0);
}

/* XtendedInput's Carbon defaults (Xbox names; the same places on any pad). */
static void pad_layout(unsigned port) {
    static const struct { uint8_t row, kind, control, kind2, control2; } rows[] = {
        {0, 7, 1, 0, 0},  /* gas: RT (pedal) */
        {1, 7, 0, 0, 0},  /* brake / reverse: LT (pedal) */
        {2, 4, 0, 0, 0},  {3, 5, 0, 0, 0}, /* steer: left stick */
        {4, 2, 1, 0, 0},  /* handbrake: A */
        {5, 2, 0, 0, 0},  /* speedbreaker: X */
        {6, 2, 2, 0, 0},  /* nitrous: B */
        {7, 2, 4, 0, 0},  /* shift down: LB (the right stick is the camera, camera.h) */
        {8, 2, 5, 0, 0},  /* shift up: RB */
        {18, 2, 8, 0, 0}, /* reset car: Back */
        {19, 2, 3, 0, 0}, /* crew: Y */
        {20, 3, 0, 0, 0}, /* engage event: d-pad up */
        {21, 3, 3, 0, 0}, {22, 3, 1, 0, 0}, /* HUD pad left/right: d-pad */
        {23, 2, 10, 0, 0}, /* FE button 2: LS click */
        {24, 3, 2, 0, 0}, /* camera: d-pad down */
        {25, 2, 11, 0, 0}, /* look back: RS click */
        {28, 2, 9, 0, 0}, /* pause: Start */
        {29, 3, 3, 4, 0}, {30, 3, 1, 5, 0}, {31, 3, 0, 4, 1}, {32, 3, 2, 5, 1}, /* menus: d-pad, left stick */
        {33, 4, 2, 0, 0}, {34, 5, 2, 0, 0}, {35, 6, 2, 0, 0}, {36, 7, 2, 0, 0}, /* menus: right stick */
        {37, 2, 1, 0, 0}, /* accept: A */
        {38, 2, 2, 0, 0}, /* back: B */
        {39, 2, 9, 0, 0}, /* start */
        {40, 2, 6, 0, 0}, {41, 2, 7, 0, 0}, /* menu triggers */
        {42, 2, 5, 0, 0}, /* FE button 0: RB */
        {43, 2, 4, 0, 0}, /* FE button 1: LB */
        {44, 2, 11, 0, 0}, /* FE button 3: RS click */
        {45, 2, 0, 0, 0}, /* FE button 4: X */
        {46, 2, 3, 0, 0}, /* FE button 5: Y */
        {47, 2, 1, 0, 0}, /* skip cutscene: A */
        {48, 2, 10, 0, 0}, /* next song: LS click */
    };
    for (unsigned r = 0; r < PAD_DEFAULT_ROWS; ++r) {
        pad_set(port, r, 0, 0, 0);
        pad_set(port, r, 1, 0, 0);
    }
    for (unsigned i = 0; i < sizeof rows / sizeof rows[0]; ++i) {
        pad_set(port, rows[i].row, 0, rows[i].kind, rows[i].control);
        pad_set(port, rows[i].row, 1, rows[i].kind2, rows[i].control2);
    }
    g_pad_seen_valid[port] = 0;
}

/* The console layout before the right-stick camera (v0.5): shifting on the
 * right stick, camera RB, look back LB, next song RS click. A port still
 * exactly so (the old layout saved in a profile) gets those rows moved; a
 * remap of any of rows 7, 8, 24, 25 (primary slot) makes the port theirs. */
static const struct { uint8_t row, old_kind, old_control, kind, control; } kPadMoved[] = {
    {7, 7, 2, 2, 4}, {8, 6, 2, 2, 5}, {24, 2, 5, 3, 2}, {25, 2, 4, 2, 11}, {48, 2, 11, 2, 10},
};
static int pad_move_old_layout(unsigned port) {
    for (unsigned i = 0; i < 4; ++i) { /* row 48 alone does not decide */
        uint32_t at = pad_triple(kPadMoved[i].row, 2u * port + 2u);
        if (!at || get_u32(at) != kPadMoved[i].old_kind || get_u32(at + 8) != kPadMoved[i].old_control)
            return 0;
    }
    for (unsigned i = 0; i < sizeof kPadMoved / sizeof kPadMoved[0]; ++i) {
        uint32_t at = pad_triple(kPadMoved[i].row, 2u * port + 2u);
        if (at && get_u32(at) == kPadMoved[i].old_kind && get_u32(at + 8) == kPadMoved[i].old_control)
            pad_set(port, kPadMoved[i].row, 0, kPadMoved[i].kind, kPadMoved[i].control);
    }
    g_pad_seen_valid[port] = 0;
    return 1;
}

/* The game's PC layout, untouched: gas, brake, handbrake, Speedbreaker and
 * nitrous (rows 0 1 4 5 6, both slots) all as the game's defaults put them.
 * A profile saved before this layout existed has them so; a player's remap of
 * any one of them makes the port theirs. (Comparing every row was too strict:
 * a profile load does not give back every row exactly as the defaults wrote
 * them, run 2026-10-10 13:28.) */
static int pad_rows_are_generic(unsigned port) {
    static const unsigned rows[] = {0, 1, 4, 5, 6};
    for (unsigned i = 0; i < sizeof rows / sizeof rows[0]; ++i)
        for (unsigned s = 0; s < 2; ++s) {
            uint32_t at = pad_triple(rows[i], 2u * port + 2u + s);
            if (!at)
                return 0;
            /* kind and control; the port field is the game's bookkeeping */
            if (get_u32(at) != g_pad_generic[port][rows[i]][s][0] ||
                get_u32(at + 8) != g_pad_generic[port][rows[i]][s][2])
                return 0;
        }
    return 1;
}

/* 00689550: the game's defaults for one port; remembered, then the layout. */
static void pad_defaults_hook(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv;
    (void)user;
    uint32_t port = get_u32(cpu->esp + 4);
    api->call_original(api, cpu->target, cpu);
    if (port >= PAD_MAX_PORTS || !get_u32(PAD_DEVICES + port * 4u))
        return;
    for (unsigned r = 0; r < PAD_DEFAULT_ROWS; ++r)
        for (unsigned s = 0; s < 2; ++s) {
            uint32_t at = pad_triple(r, 2u * port + 2u + s);
            for (unsigned k = 0; k < 3; ++k)
                g_pad_generic[port][r][s][k] = at ? get_u32(at + 4u * k) : 0;
        }
    g_pad_generic_valid[port] = 1;
    if (setting("console_layout", 1)) {
        pad_layout(port);
        char line[96];
        snprintf(line, sizeof line, "core.nfsc: controller %u set to the console layout", port);
        log_line(line);
    }
}

static uint32_t pad_ports(void) {
    uint32_t n = get_u32(PAD_PORT_COUNT);
    return n < PAD_MAX_PORTS ? n : PAD_MAX_PORTS;
}

/* Diagnostic: when a port's rows change without this file writing them (a
 * profile load, the Controls screen), log how they differ from the game's
 * defaults, a few times per run. */
static void pad_snapshot(unsigned port, uint32_t out[PAD_DEFAULT_ROWS][2][3]) {
    for (unsigned r = 0; r < PAD_DEFAULT_ROWS; ++r)
        for (unsigned s = 0; s < 2; ++s) {
            uint32_t at = pad_triple(r, 2u * port + 2u + s);
            for (unsigned k = 0; k < 3; ++k)
                out[r][s][k] = at ? get_u32(at + 4u * k) : 0;
        }
}
static void pad_watch(unsigned port) {
    uint32_t now[PAD_DEFAULT_ROWS][2][3];
    pad_snapshot(port, now);
    if (g_pad_seen_valid[port] && memcmp(now, g_pad_seen[port], sizeof now) && g_pad_reports < 6) {
        ++g_pad_reports;
        char line[160];
        snprintf(line, sizeof line, "core.nfsc: controller %u rows changed by the game; differences from its defaults:", port);
        log_line(line);
        for (unsigned r = 0; r < PAD_DEFAULT_ROWS; ++r)
            for (unsigned s = 0; s < 2; ++s)
                if (memcmp(now[r][s], g_pad_generic[port][r][s], 12)) {
                    snprintf(line, sizeof line, "core.nfsc:   row %u slot %u now {%u,%u,%u} default {%u,%u,%u}", r, s,
                             now[r][s][0], now[r][s][1], now[r][s][2], g_pad_generic[port][r][s][0],
                             g_pad_generic[port][r][s][1], g_pad_generic[port][r][s][2]);
                    log_line(line);
                }
    }
    memcpy(g_pad_seen[port], now, sizeof now);
    g_pad_seen_valid[port] = 1;
}

/* 00692240: the per-frame input update (thiscall). */
static void pad_keyboard_crew(void);
static void impact_feedback_tick(const PopModApi *api);
static void extras_tick(const PopModApi *api);
static void pad_update_hook(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv;
    (void)user;
    uint32_t self = cpu->ecx;
    int layout = (int)setting("console_layout", 1);
    pad_keyboard_crew();
    impact_feedback_tick(api); /* rumble.h: one motor update per frame */
    extras_tick(api);          /* extras.h: Extra Options' settings */
    for (unsigned p = 0; p < pad_ports(); ++p)
        if (g_pad_generic_valid[p])
            pad_watch(p);
    if (layout)
        for (unsigned p = 0; p < pad_ports(); ++p) {
            if (g_pad_generic_valid[p] && pad_rows_are_generic(p)) {
                pad_layout(p);
                char line[96];
                snprintf(line, sizeof line, "core.nfsc: controller %u had the PC defaults; console layout applied", p);
                log_line(line);
            } else if (g_pad_generic_valid[p] && pad_move_old_layout(p)) {
                char line[128];
                snprintf(line, sizeof line,
                         "core.nfsc: controller %u had the old console layout; shifting moved to LB/RB, camera to d-pad down, look back to RS click", p);
                log_line(line);
            }
        }
    api->call_original(api, cpu->target, cpu);
    uint32_t values = get_u32(self + 0x20);
    if (get_u32(self + 0x24) != 0 || !values)
        return;
    for (unsigned p = 0; p < pad_ports(); ++p) {
        uint32_t device = get_u32(PAD_DEVICES + p * 4u), state = 0;
        if (!device)
            continue;
        uint32_t getter = get_u32(get_u32(device) + 4);
        if (!getter || api->guest_call(api, getter, device, NULL, 0, &state) != POP_OK || !state)
            continue;
        float trig[2] = {fminf(get_u32(state + PAD_STATE_RX) / 65535.0f, 1.0f),
                         fminf(get_u32(state + PAD_STATE_RX + 4) / 65535.0f, 1.0f)};
        for (unsigned r = 0; r < PAD_ROWS; ++r)
            for (unsigned s = 0; s < 2; ++s) {
                uint32_t at = pad_triple(r, 2u * p + 2u + s);
                if (!at || get_u32(at) != 7)
                    continue;
                uint32_t control = get_u32(at + 8);
                if (control > 1)
                    continue;
                uint32_t vr = values + r * 4u;
                if (trig[control] > bits_float(get_u32(vr)))
                    put_float(vr, trig[control]);
            }
    }
}

/* Which names: 0 Xbox, 1 PlayStation, 2 Nintendo. Setting 0 = ask SDL. */
static int pad_family(void) {
    int64_t chosen = setting("controller_buttons", 0);
    if (chosen >= 1 && chosen <= 3)
        return (int)chosen - 1;
    typedef uint32_t *(*GetPads)(int *);
    typedef int (*TypeFor)(uint32_t);
    typedef void (*Free)(void *);
    static GetPads get_pads;
    static TypeFor type_for;
    static Free sdl_free;
    static int looked;
    if (!looked) {
        looked = 1;
        get_pads = (GetPads)dlsym(RTLD_DEFAULT, "SDL_GetGamepads");
        type_for = (TypeFor)dlsym(RTLD_DEFAULT, "SDL_GetGamepadTypeForID");
        sdl_free = (Free)dlsym(RTLD_DEFAULT, "SDL_free");
    }
    int family = 0, count = 0;
    uint32_t *ids = get_pads ? get_pads(&count) : NULL;
    if (ids && count > 0 && type_for) {
        int t = type_for(ids[0]); /* SDL_GamepadType */
        if (t >= 4 && t <= 6)
            family = 1;           /* PS3, PS4, PS5 */
        else if (t >= 7 && t <= 11)
            family = 2;           /* Switch Pro, Joy-Cons, GameCube */
    }
    if (ids && sdl_free)
        sdl_free(ids);
    return family;
}

static const char *pad_button_name(int family, uint32_t b) {
    static const char *const names[3][13] = {
        {"X", "A", "B", "Y", "LB", "RB", "LT", "RT", "View", "Menu", "LS Click", "RS Click", "Xbox"},
        {"Square", "Cross", "Circle", "Triangle", "L1", "R1", "L2", "R2", "Create", "Options", "L3", "R3", "PS"},
        {"Y", "B", "A", "X", "L", "R", "ZL", "ZR", "Minus", "Plus", "L Stick Click", "R Stick Click", "Home"},
    };
    return b < 13 ? names[family][b] : NULL;
}

static const char *pad_name(int family, uint32_t kind, uint32_t control) {
    static const char *const pov[] = {"D-Pad Up", "D-Pad Right", "D-Pad Down", "D-Pad Left"};
    static const char *const sticks[2][3] = {{"Left Stick Left", "Left Stick Up", "Right Stick Left"},
                                             {"Left Stick Right", "Left Stick Down", "Right Stick Right"}};
    switch (kind) {
    case 2: return pad_button_name(family, control);
    case 3: return control < 4 ? pov[control] : NULL;
    case 4: case 5: return control < 3 ? sticks[kind - 4][control] : NULL;
    case 6: case 7:
        if (control == 0) return pad_button_name(family, 6);
        if (control == 1) return pad_button_name(family, 7);
        if (control == 2) return kind == 6 ? "Right Stick Up" : "Right Stick Down";
        return NULL;
    }
    return NULL;
}

/* 00679ad0: a binding's name on the Controls screen. */
static void pad_name_hook(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv;
    (void)user;
    uint32_t out = get_u32(cpu->esp + 4), device = get_u32(cpu->esp + 8), row = get_u32(cpu->esp + 12);
    uint32_t primary = get_u32(cpu->esp + 16) & 0xffu, port = get_u32(cpu->esp + 20);
    api->call_original(api, cpu->target, cpu);
    if (device == 1 || !out || row >= PAD_ROWS || port >= PAD_MAX_PORTS)
        return;
    uint32_t at = pad_triple(row, 2u * port + (primary ? 2u : 3u));
    const char *name = at ? pad_name(pad_family(), get_u32(at), get_u32(at + 8)) : NULL;
    char *dst = NULL;
    if (name && api->guest_ptr(api, out, (uint32_t)strlen(name) + 1, (void **)&dst) == POP_OK && dst)
        memcpy(dst, name, strlen(name) + 1);
}

static PopModStatus install(uint32_t addr, PopHookFn fn, int32_t mode, int slot);

/* KEYBOARD CREW KEY. The PC default for Activate Crew Member (row 19,
 * GAME_ACTION_CREWDEFENSIVE) is Right Ctrl alone, which Mac keyboards lack. Its
 * secondary key becomes Right Option (DIK_RMENU 0xB8; the game names it Right
 * Alt): in the defaults template (0xa61c88, 52-byte rows {name, triple x4}; a
 * keyboard triple is {1, DIK, 0}), so Restore Defaults and new profiles get it,
 * and in a loaded profile whose crew row is still exactly the PC default. */
#define PAD_TEMPLATE 0x00a61c88u
#define CREW_ROW 19u
#define DIK_RCONTROL 0x9du
#define DIK_RMENU 0xb8u
static void pad_keyboard_crew(void) {
    const uint32_t t = PAD_TEMPLATE + CREW_ROW * 52u + 4u; /* slot 0, then slot 1 at +12 */
    if (get_u32(t) == 1u && get_u32(t + 4) == DIK_RCONTROL && get_u32(t + 12) == 0u) {
        g_api->guest_write_u32(g_api, t + 12, 1u);
        g_api->guest_write_u32(g_api, t + 16, DIK_RMENU);
        g_api->guest_write_u32(g_api, t + 20, 0u);
    }
    const uint32_t primary = pad_triple(CREW_ROW, 0), secondary = pad_triple(CREW_ROW, 1);
    if (primary && secondary && get_u32(primary) == 1u && get_u32(primary + 4) == DIK_RCONTROL &&
        get_u32(secondary) == 0u) {
        g_api->guest_write_u32(g_api, secondary, 1u);
        g_api->guest_write_u32(g_api, secondary + 4, DIK_RMENU);
        g_api->guest_write_u32(g_api, secondary + 8, 0u);
        log_line("core.nfsc: Activate Crew Member also on Right Option (Mac keyboards have no Right Ctrl)");
    }
}

static void pad_install(void) {
    if (install(PAD_SET_DEFAULTS, pad_defaults_hook, POP_HOOK_REPLACE, 7) != POP_OK ||
        install(PAD_UPDATE, pad_update_hook, POP_HOOK_REPLACE, 8) != POP_OK)
        log_line("core.nfsc: controller layout unavailable");
    if (install(PAD_NAME, pad_name_hook, POP_HOOK_REPLACE, 9) != POP_OK)
        log_line("core.nfsc: controller button names unavailable");
}
