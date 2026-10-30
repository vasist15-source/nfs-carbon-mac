/* extras.h - options from the NFSC Extra Options mod (included by nfsc.c).
 *
 * The ExOpts Team's NFSC Extra Options (v3.0.1 source, GPL-3; credited in NOTICE) was read for
 * its addresses and what each option changes; the code here is our own. It patches the exe's
 * bytes; here a value the game keeps in its data is written while it runs, and a number the
 * game keeps in its code is read from a slot (game.toml instruction_patches, defaults in
 * data_seeds), so every option is a live setting on the F10 Mods page.
 *
 * DATA. 0x00a9e6c0 the game's unlock-all switch; 0x00a631e8 Collector's Edition (1 in the exe;
 * forced only while the setting is on); 0x00a712ac game speed (1.0); 0x00b74d20 rain and
 * 0x00b74d64 fog (0); 0x00a797d0 rain amount (1.0); 0x00a71270 Speedbreaker recharge (25 s);
 * 0x00a590a4 cop radar range (300); 0x00a607d0 / d1 cops on the minimap (1, 1). The neon
 * barriers are found by name at track load ("BARRIER_SPLINE_..." in .rdata); a changed letter
 * means none is found.
 *
 * SLOTS (0x00c2b700..). New career cash (5000 / 15000 with Collector's Edition, 004c4cc4 /
 * 004c4cd4); Quick Race laps (most 8, 004ac7c0) and opponents (7, or 6 with a crew member,
 * 004ac840); traffic density for Low / Medium / High (3 / 10 / 50, pushed at 007a9e47 /
 * 007a9e43 / 007a9e3f); busted-screen face pixelation (1, 0066f800 / 0066f87b); the player's
 * headlights glare relit each update (mask 7, 006c63a5) and their beams (1.0, 007cbe93/9b).
 *
 * CALLS (on the game's thread, from the per-frame input update in pad.h): Game_SetWorldHeat
 * 0065c550 (cdecl float), Game_SetCopsEnabled 006513e0 (cdecl bool), Game_ForceAIControl
 * 0065c2c0 / Game_ClearAIControl 0065c330 (cdecl int), PVehicle glare on/off 006c0ba0 /
 * 006c0bc0 and IsGlareOn 006d8080 (thiscall vehicle, id: 7 headlights, 0x7000 cop lights) on
 * the player's vehicle [0x00a9f168]. The game state [0x00a99bbc] is 6 while driving.
 *
 * HEAT. The pursuit update clamps the heat between the event's minimum and maximum
 * ([esi+0xf4] / [esi+0xf8]) with 004010e0 (cdecl float clamp(value, lo, hi)) called at 00449a91;
 * a callsite hook replaces lo / hi there, which is what Extra Options' override does. */

#define EX_UNLOCK_ALL 0x00a9e6c0u
#define EX_COLLECTORS 0x00a631e8u
#define EX_GAME_SPEED 0x00a712acu
#define EX_RAIN 0x00b74d20u
#define EX_FOG 0x00b74d64u
#define EX_RAIN_AMOUNT 0x00a797d0u
#define EX_SB_RECHARGE 0x00a71270u
#define EX_RADAR_RANGE 0x00a590a4u
#define EX_MAP_COPS 0x00a607d0u /* +0 cops not chasing you, +1 cops chasing you */
#define EX_GAME_STATE 0x00a99bbcu
#define EX_PLAYER_VEHICLE 0x00a9f168u
#define EX_SLOT_CASH 0x00c2b700u
#define EX_SLOT_CASH_CE 0x00c2b704u
#define EX_SLOT_LAPS 0x00c2b708u
#define EX_SLOT_OPPONENTS 0x00c2b70cu
#define EX_SLOT_OPPONENTS_CREW 0x00c2b710u
#define EX_SLOT_TRAFFIC 0x00c2b714u /* Low, Medium, High */
#define EX_SLOT_PIXELATE 0x00c2b720u
#define EX_SLOT_HEADLIGHTS 0x00c2b724u
#define EX_SLOT_BEAMS 0x00c2b728u
#define EX_SET_WORLD_HEAT 0x0065c550u
#define EX_SET_COPS 0x006513e0u
#define EX_FORCE_AI 0x0065c2c0u
#define EX_CLEAR_AI 0x0065c330u
#define EX_GLARE_ON 0x006c0ba0u
#define EX_GLARE_OFF 0x006c0bc0u
#define EX_IS_GLARE_ON 0x006d8080u
#define EX_CLAMP 0x004010e0u
#define EX_HEAT_CLAMP_RET 0x00449a96u
#define EX_GLARE_HEADLIGHTS 7u
#define EX_GLARE_COP_LIGHTS 0x7000u

static const uint32_t kBarrierNames[] = {0x009d85c8u, 0x009d85dcu, 0x009d85f0u, 0x009d8604u,
                                         0x009d8618u, 0x009d862cu, 0x009d8b34u};
static uint8_t g_barrier_letter[sizeof kBarrierNames / sizeof kBarrierNames[0]];

/* What was last applied, so a value is written (or a call made) only when it changes; -1 = not
 * yet. The state-dependent ones are applied again whenever driving starts. */
static int g_ex_driving;
enum {
    EX_UNLOCK, EX_SPEED, EX_RAIN_ON, EX_FOG_ON, EX_RAIN_AMT, EX_SB, EX_RADAR, EX_MAP, EX_CASH,
    EX_LAPS, EX_OPP, EX_TRAFFIC, EX_BUSTED, EX_BARRIERS, EX_HEADLIGHTS, EX_HEAT, EX_COUNT
};
static int64_t g_ex_last[EX_COUNT];

static int64_t ex_choice(const char *key, const int64_t *values, int n, int64_t fallback) {
    const int64_t c = setting(key, 0);
    return c >= 0 && c < n ? values[c] : fallback;
}
/* True once per change of `now` (and on the first look). */
static int ex_changed(int which, int64_t now) {
    if (g_ex_last[which] == now)
        return 0;
    g_ex_last[which] = now;
    return 1;
}
static void ex_write_u8(uint32_t addr, uint8_t v) {
    g_api->guest_write_u8(g_api, addr, v);
}
static uint32_t ex_call(const PopModApi *api, uint32_t fn, uint32_t ecx, uint32_t arg) {
    uint32_t eax = 0;
    if (api->guest_call(api, fn, ecx, &arg, 1, &eax) != POP_OK) {
        static int told;
        if (!told++) {
            char line[96];
            snprintf(line, sizeof line, "core.nfsc: extra options could not call %08x", fn);
            log_line(line);
        }
    }
    return eax;
}
static void ex_glare(const PopModApi *api, uint32_t id, int on) {
    const uint32_t vehicle = get_u32(EX_PLAYER_VEHICLE);
    if (!vehicle)
        return;
    const int lit = (ex_call(api, EX_IS_GLARE_ON, vehicle, id) & 0xffu) != 0;
    if (on && !lit)
        ex_call(api, EX_GLARE_ON, vehicle, id);
    else if (!on && lit)
        ex_call(api, EX_GLARE_OFF, vehicle, id);
}

/* The heat the force_heat setting holds (0 = off), and the bounds heat_min / heat_max set
 * (0 = the event's own). */
static int64_t ex_forced_heat(void) {
    return setting("force_heat", 0);
}

/* 004010e0 called from the pursuit update: the event's heat bounds, or ours. */
static void ex_heat_clamp(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv;
    (void)user;
    const int64_t forced = ex_forced_heat();
    const int64_t lo = forced ? forced : setting("heat_min", 0), hi = forced ? forced : setting("heat_max", 0);
    if (lo > 0)
        api->guest_write_u32(api, cpu->esp + 8, float_bits((float)lo));
    if (hi > 0)
        api->guest_write_u32(api, cpu->esp + 12, float_bits((float)(hi < lo ? lo : hi)));
}

/* Once a frame, from pad.h's input update. */
static void extras_tick(const PopModApi *api) {
    static int first = 1;
    if (first) {
        first = 0;
        for (unsigned i = 0; i < sizeof g_ex_last / sizeof g_ex_last[0]; ++i)
            g_ex_last[i] = -1;
        for (unsigned i = 0; i < sizeof kBarrierNames / sizeof kBarrierNames[0]; ++i)
            api->guest_read_u8(api, kBarrierNames[i], &g_barrier_letter[i]);
        uint8_t ce = 0;
        api->guest_read_u8(api, EX_COLLECTORS, &ce);
        char line[96];
        snprintf(line, sizeof line, "core.nfsc: Collector's Edition flag is %u before extras", ce);
        log_line(line);
    }
    /* Unlocks. */
    const int64_t unlock = setting("unlock_all", 0) != 0;
    if (ex_changed(EX_UNLOCK, unlock))
        ex_write_u8(EX_UNLOCK_ALL, (uint8_t)unlock);
    if (setting("collectors_edition", 1)) {
        uint8_t ce = 0;
        api->guest_read_u8(api, EX_COLLECTORS, &ce);
        if (ce != 1) {
            ex_write_u8(EX_COLLECTORS, 1);
            static int told;
            if (!told++)
                log_line("core.nfsc: Collector's Edition switched on (the game had it off)");
        }
    }
    static const int64_t cash[] = {5000, 10000, 25000, 50000, 100000, 250000, 1000000};
    const int64_t c = ex_choice("starting_cash", cash, 7, 5000);
    if (ex_changed(EX_CASH, c)) {
        api->guest_write_u32(api, EX_SLOT_CASH, (uint32_t)c);
        api->guest_write_u32(api, EX_SLOT_CASH_CE, (uint32_t)c + 10000u);
    }
    /* Races. */
    static const int64_t laps[] = {8, 10, 15, 20, 30, 50, 99};
    const int64_t l = ex_choice("qr_max_laps", laps, 7, 8);
    if (ex_changed(EX_LAPS, l))
        api->guest_write_u32(api, EX_SLOT_LAPS, (uint32_t)l);
    static const int64_t opponents[] = {7, 9, 11, 13, 15, 19};
    const int64_t o = ex_choice("qr_max_opponents", opponents, 6, 7);
    if (ex_changed(EX_OPP, o)) {
        api->guest_write_u32(api, EX_SLOT_OPPONENTS, (uint32_t)o);
        api->guest_write_u32(api, EX_SLOT_OPPONENTS_CREW, (uint32_t)o - 1u);
    }
    /* Traffic: Low / Medium / High as the game has them (3, 10, 50), or more of each. */
    static const uint32_t traffic[][3] = {{3, 10, 50}, {6, 20, 75}, {10, 35, 100}, {25, 60, 100}};
    const int64_t t = setting("traffic_amount", 0);
    if (ex_changed(EX_TRAFFIC, t) && t >= 0 && t < 4)
        for (unsigned i = 0; i < 3; ++i)
            api->guest_write_u32(api, EX_SLOT_TRAFFIC + 4u * i, traffic[t][i]);
    static const int64_t recharge[] = {25, 15, 10, 5};
    const int64_t sb = ex_choice("sb_recharge", recharge, 4, 25);
    if (ex_changed(EX_SB, sb))
        put_float(EX_SB_RECHARGE, (float)sb);
    static const int64_t radar[] = {300, 500, 1000};
    const int64_t r = ex_choice("radar_range", radar, 3, 300);
    if (ex_changed(EX_RADAR, r))
        put_float(EX_RADAR_RANGE, (float)r);
    const int64_t map = setting("map_cops", 1) != 0;
    if (ex_changed(EX_MAP, map)) {
        ex_write_u8(EX_MAP_COPS, (uint8_t)map);
        ex_write_u8(EX_MAP_COPS + 1, (uint8_t)map);
    }
    /* World. */
    static const int64_t speed_pct[] = {100, 25, 50, 75, 125, 150, 200};
    const int64_t sp = ex_choice("game_speed", speed_pct, 7, 100);
    if (ex_changed(EX_SPEED, sp))
        put_float(EX_GAME_SPEED, (float)sp / 100.0f);
    const int64_t rain = setting("always_rain", 0) != 0, fog = setting("fog", 0) != 0;
    if (ex_changed(EX_RAIN_ON, rain))
        ex_write_u8(EX_RAIN, (uint8_t)rain);
    if (ex_changed(EX_FOG_ON, fog))
        ex_write_u8(EX_FOG, (uint8_t)fog);
    static const int64_t amount[] = {1, 2, 3};
    const int64_t ra = ex_choice("rain_amount", amount, 3, 1);
    if (ex_changed(EX_RAIN_AMT, ra))
        put_float(EX_RAIN_AMOUNT, (float)ra);
    const int64_t no_barriers = setting("no_neon_barriers", 0) != 0;
    if (ex_changed(EX_BARRIERS, no_barriers))
        for (unsigned i = 0; i < sizeof kBarrierNames / sizeof kBarrierNames[0]; ++i)
            ex_write_u8(kBarrierNames[i], no_barriers ? (uint8_t)'Y' : g_barrier_letter[i]);
    const int64_t uncensored = setting("uncensored_busted", 0) != 0;
    if (ex_changed(EX_BUSTED, uncensored))
        api->guest_write_u32(api, EX_SLOT_PIXELATE, uncensored ? 0u : 1u);
    /* Driving: the calls need a car, so they wait for the game to be driving, and are made
     * again each time driving starts. */
    const int driving = get_u32(EX_GAME_STATE) == 6 && get_u32(EX_PLAYER_VEHICLE) != 0;
    if (driving && !g_ex_driving)
        g_ex_last[EX_HEAT] = -1;
    g_ex_driving = driving;
    const int64_t headlights = setting("headlights", 1) != 0;
    if (ex_changed(EX_HEADLIGHTS, headlights + 2 * driving)) {
        api->guest_write_u32(api, EX_SLOT_HEADLIGHTS, headlights ? EX_GLARE_HEADLIGHTS : 0u);
        put_float(EX_SLOT_BEAMS, headlights ? 1.0f : 0.0f);
        if (driving)
            ex_glare(api, EX_GLARE_HEADLIGHTS, (int)headlights);
    }
    /* What these calls changed; each event starts with the game's own again. Nothing is called
     * while a switch is at the game's default and was not changed. */
    static int cops_off, cop_lights_on, ai_on;
    if (!driving) {
        cops_off = cop_lights_on = ai_on = 0;
        return;
    }
    const int64_t heat = ex_forced_heat();
    if (ex_changed(EX_HEAT, heat) && heat > 0)
        ex_call(api, EX_SET_WORLD_HEAT, 0, float_bits((float)heat));
    const int cops = setting("cops", 1) != 0;
    if (!cops && !cops_off) {
        ex_call(api, EX_SET_COPS, 0, 0);
        cops_off = 1;
    } else if (cops && cops_off) {
        ex_call(api, EX_SET_COPS, 0, 1);
        cops_off = 0;
    }
    const int cop_lights = setting("cop_lights", 0) != 0;
    if (cop_lights != cop_lights_on) {
        ex_glare(api, EX_GLARE_COP_LIGHTS, cop_lights);
        cop_lights_on = cop_lights;
    }
    const int autodrive = setting("auto_drive", 0) != 0;
    if (autodrive != ai_on) {
        ex_call(api, autodrive ? EX_FORCE_AI : EX_CLEAR_AI, 0, 1);
        ai_on = autodrive;
    }
}

static void extras_install(void) {
    if (!g_api->hook_install_at_callsite ||
        g_api->hook_install_at_callsite(g_api, EX_CLAMP, EX_HEAT_CLAMP_RET, ex_heat_clamp, POP_HOOK_BEFORE, 0,
                                        &g_hooks[26]) != POP_OK)
        log_line("core.nfsc: heat level limits unavailable");
}
