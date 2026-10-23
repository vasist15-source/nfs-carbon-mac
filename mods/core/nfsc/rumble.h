/* rumble.h - controller rumble from Carbon's own force-feedback values.
 *
 * Ported from the NFS Most Wanted Mac port's impact_feedback.h (mods/core/nfsmw), whose
 * description below applies with Carbon's addresses: the force-feedback wrapper is
 * [0x00a98954]; the per-frame driving update (0069xxxx) calls 00734cd0 (impacts: adds
 * those within 150 ms in wrapper+0x4f48, then builds a 150000 us effect) and 00735270
 * (road surface: device, waveform 0..4, magnitude, period clamped to 150 ms), the same
 * functions as MW's 006dc800 / 006dcda0. A gamepad has no force axes, so the game never
 * creates the effects; these hooks read the values it computes and drive the motors
 * through the kit's rumble_pulse. MW's race-state check (0x925e90 == 6) is not used:
 * the road term lapses 100 ms after the game stops calling (menus, air, pause) and an
 * impact lasts 150 ms.
 */
#define FFB_WRAPPER_PTR 0x00a98954u
#define FFB_IMPACT 0x00734cd0u
#define FFB_ROAD 0x00735270u
#include <math.h>
#include <stddef.h>
/* The MW port's notes, kept for how the bridge works. Every address and "Test" number in
 * them is the MW PC 1.3 exe's or its test runs', not Carbon's: MW's 006dc800 / 006dcda0
 * are Carbon's 00734cd0 / 00735270 above, and the surface table was read from MW.
 *
 * Controller rumble from the PC 1.3 force-feedback update.
 *
 * The per-frame FFB update (around 006432e0, wrapper at [009203c4], device 0 = player 1)
 * builds DirectInput effects for a wheel. A gamepad has no force axes, so those effects
 * are never created; two of them are bridged to the pad's motors, using only the values
 * the game computes itself:
 *
 * - Bump/impact: 006dc800 accumulates nearby impacts in wrapper+4f48 and builds a 150000 us
 *   effect. Bridged as a 150 ms pulse (unchanged since Test45).
 * - Road surface: 006dcda0(device, waveform, magnitude, period_ms) is called once per frame
 *   for the device. Its caller averages the surfaces under the tyres: magnitude = average
 *   surface amplitude byte (+0xfa) x speed ratio x 9000/255, period = average surface word
 *   (+0xf8) in ms, waveform = the surface's attribute (hash 46226745).
 *   006dcda0 clamps the period to 150 ms and passes period x 1000 (imul 3e8 at 006dcebf and
 *   006dcf22) as the DirectInput period in microseconds. Waveforms 0..4 select the GUIDs at
 *   008c045c/046c/044c/043c/042c = sine, square, triangle, sawtooth up, sawtooth down; any
 *   other value selects no GUID, so no effect exists. A changed waveform recreates the
 *   effect (006d9460); otherwise only its parameters are updated (006d9610).
 *
 *   ADAPTATION (not console behaviour): the signed periodic wheel force is drawn as a
 *   non-negative motor level, waveform level (0..1) x magnitude/10000 x `surface_rumble` %,
 *   on both motors. The phase runs continuously on the monotonic clock across period and
 *   magnitude updates and restarts at 0 when the waveform changes (the game's effect
 *   restart) or after the calls lapse. SAMPLING APPROXIMATION: the level is sent at the game's
 *   60 Hz update (measured: one command per guest frame), so a period shorter than three
 *   updates (50 ms) cannot be drawn without aliasing, worse in 40 fps stretches; such periods
 *   (grass, hay, golf, sand: 40 ms) run at the waveform's mean level (0.5), a steady buzz.
 *   This is a sampling rule, not a measurement of the motors.
 *   No effect is invented where the game computes none: a stopped car, the air, pause and
 *   menus give magnitude 0 or no call.
 *
 *   The PC surface data (Test57, read from the running game) gives the effect to textured
 *   surfaces only (period ms / intensity 0..255; the +0xf8 word / +0xfa byte): cobble and
 *   rooftile 100/120, dirt and mud 80/130, gravel 60/110, stone 90/70, grass/hay/golf 40/80,
 *   sand 40/60, wood 120/100, railroad and metal 160/100; asphalt, concrete and wet pavement
 *   have none.
 *
 * Output: one mod-owned pulse carries max(current impact, latest surface sample). The surface
 * term is recomputed every tick from the latest call, never held from an earlier sample; the
 * host replaces this owner's previous pulse (Vpad::rumble_pulse) and max-mixes only across
 * owners. Pulses last 60 ms, so a stalled guest stops the motors from the host clock.
 * Waveform/actuator equivalence to a console is not claimed; strengths are settings. */
#include <time.h>
static int g_impact_requested;
static uint32_t g_impact_level;   /* 0..65535 before gain, while active */
static uint64_t g_impact_until_ms;
static struct {
    uint32_t magnitude, period, waveform;
    uint64_t seen_ms;
    float phase;          /* 0..1 of the running periodic effect */
    uint64_t phase_ms;    /* when phase was last advanced */
    uint32_t phase_wave;  /* waveform the phase belongs to */
    int phase_live;
} g_road;
static uint64_t g_feedback_sent_ms;
static uint16_t g_feedback_sent_level;        /* low (left) motor */
static uint16_t g_feedback_sent_high;         /* high (right) motor */

static uint64_t feedback_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}
/* Monotonic ms clock; native/tests/rumble_feedback_tests.cpp substitutes a fixture clock. */
static uint64_t (*g_feedback_clock)(void) = feedback_now_ms;
static int impact_api_available(const PopModApi *api) {
    return api->size >= offsetof(PopModApi, rumble_pulse) + sizeof api->rumble_pulse && api->rumble_pulse;
}
static void impact_feedback_stop(const PopModApi *api) {
    if (g_impact_requested && impact_api_available(api)) api->rumble_pulse(api, 0, 0, 0);
    g_impact_requested = 0;
    g_impact_until_ms = 0;
    g_road.seen_ms = 0;
    g_road.phase_live = 0;
    g_feedback_sent_level = 0;
    g_feedback_sent_high = 0;
}
/* Level (0..1) of the game's periodic waveform index at `phase` (0..1). */
static float road_wave(uint32_t waveform, float phase) {
    switch (waveform) {
    case 0: return 0.5f + 0.5f * sinf(6.2831853f * phase);        /* sine */
    case 1: return phase < 0.5f ? 1.0f : 0.0f;                    /* square */
    case 2: return 1.0f - fabsf(2.0f * phase - 1.0f);             /* triangle */
    case 3: return phase;                                         /* sawtooth up */
    case 4: return 1.0f - phase;                                  /* sawtooth down */
    default: return 0.0f;                     /* no effect GUID: the game creates none */
    }
}
/* Advances the running effect's phase to `now`; returns its level (0..1). */
static float road_level_at(uint64_t now) {
    if (!g_road.phase_live || g_road.phase_wave != g_road.waveform) {
        g_road.phase = 0.0f;
        g_road.phase_wave = g_road.waveform;
        g_road.phase_live = 1;
    } else if (g_road.period && now > g_road.phase_ms) {
        g_road.phase += (float)(now - g_road.phase_ms) / (float)g_road.period;
        g_road.phase -= floorf(g_road.phase);
    }
    g_road.phase_ms = now;
    if (g_road.waveform > 4) return 0.0f;
    if (g_road.period < 50) return 0.5f;   /* under three 60 Hz updates: mean level */
    return road_wave(g_road.waveform, g_road.phase);
}
static void impact_feedback_tick(const PopModApi *api) {
    uint32_t wrapper = get_u32(FFB_WRAPPER_PTR);
    if (!impact_api_available(api) || !wrapper ||
        get_u32(wrapper + 0x4460) || get_u32(wrapper + 0x2220)) {
        impact_feedback_stop(api);
        return;
    }
    const uint64_t now = g_feedback_clock();
    int surface_gain = (int)setting("surface_rumble", 100);
    if (surface_gain > 200) surface_gain = 200;
    if (surface_gain < 0) surface_gain = 0;
    uint16_t low, high;
    {
        float level = 0.0f;
        if (surface_gain > 0 && g_road.seen_ms && now - g_road.seen_ms < 100) {
            /* The effect keeps running (and its phase advancing) at magnitude 0. */
            const float wave = road_level_at(now);
            float m = (float)(g_road.magnitude > 10000 ? 10000 : g_road.magnitude) / 10000.0f;
            level = m * wave * (float)surface_gain / 100.0f;
        } else {
            g_road.phase_live = 0;   /* calls lapsed: the next effect starts afresh */
        }
        if (level > 1.0f) level = 1.0f;
        low = high = (uint16_t)(level * 65535.0f + 0.5f);
    }
    if (now < g_impact_until_ms) {   /* Development 50 collision pulse, both motors */
        const uint16_t impact = (uint16_t)(g_impact_level > 65535u ? 65535u : g_impact_level);
        if (impact > low) low = impact;
        if (impact > high) high = impact;
    }
    /* `rumble_floor` (default 1%): a motor level under the cut-off is not played, so a fading effect does not
     * leave a modern controller's motors buzzing at a barely perceptible level. 0 plays every level. */
    {
        int64_t floor_pct = setting("rumble_floor", 1);
        if (floor_pct < 0) floor_pct = 0;
        if (floor_pct > 10) floor_pct = 10;
        const uint16_t floor = (uint16_t)(floor_pct * 65535 / 100);
        if (low < floor) low = 0;
        if (high < floor) high = 0;
    }
    /* At most one command per display frame; a 60 ms pulse is renewed while it lasts. */
    if (low == g_feedback_sent_level && high == g_feedback_sent_high && now - g_feedback_sent_ms < 40) return;
    if (low || high) {
        if (api->rumble_pulse(api, low, high, 60) == POP_OK) g_impact_requested = 1;
    } else if (g_impact_requested) {
        api->rumble_pulse(api, 0, 0, 0);
        g_impact_requested = 0;
    }
    if (setting("input_trace", 0) &&
        ((low >> 12) != (g_feedback_sent_level >> 12) || (high >> 12) != (g_feedback_sent_high >> 12) ||
         /* Test156: also when a motor reaches or leaves zero, so the end of every rumble is in the log (a level
            under 4096, ~6%, otherwise never shows; test report: rumble lingering after a vehicle impact). */
         (low == 0) != (g_feedback_sent_level == 0) || (high == 0) != (g_feedback_sent_high == 0))) {
        char line[320];
        {
            snprintf(line, sizeof line, "core.nfsc: rumble motor=%u (surface wave=%u period=%u mag=%u)", low,
                     g_road.waveform, g_road.period, g_road.magnitude);
        }
        api->log(api, line);
    }
    g_feedback_sent_level = low;
    g_feedback_sent_high = high;
    g_feedback_sent_ms = now;
}
static void impact_feedback(const PopModApi *api, pop_cpu_v1 *cpu,
                            PopHookInvocation *inv, void *user) {
    (void)inv; (void)user;
    uint32_t wrapper = cpu->ecx, device = get_u32(cpu->esp + 4);
    api->call_original(api, cpu->target, cpu);
    int gain = setting("impact_rumble", 100);
    (void)device; /* Carbon passes the player's input slot; single player, so any */
    if (!gain || !impact_api_available(api) ||
        wrapper != get_u32(FFB_WRAPPER_PTR) ||
        get_u32(wrapper + 0x4460) || get_u32(wrapper + 0x2220)) return;
    int32_t magnitude = (int32_t)get_u32(wrapper + 0x4f48);
    if (magnitude <= 0) return;
    if (magnitude > 10000) magnitude = 10000;
    uint32_t strength = (uint32_t)((uint64_t)magnitude * 65535 * (unsigned)gain / 1000000);
    if (strength > 65535) strength = 65535;
    g_impact_level = strength;
    g_impact_until_ms = g_feedback_clock() + 150;
    g_feedback_sent_ms = 0; /* the next tick sends at once */
    if (setting("input_trace", 0)) {
        char line[144];
        snprintf(line, sizeof line, "core.nfsc: impact magnitude=%d motor=%u duration=150ms",
                 magnitude, strength);
        api->log(api, line);
    }
}
/* BEFORE 00735270 (thiscall wrapper; device, waveform, magnitude, period). */
static void road_feedback(const PopModApi *api, pop_cpu_v1 *cpu,
                          PopHookInvocation *inv, void *user) {
    (void)inv; (void)user;
    if (cpu->ecx != get_u32(FFB_WRAPPER_PTR)) return; /* any input slot (single player) */
    const int32_t magnitude = (int32_t)get_u32(cpu->esp + 12);
    const uint32_t waveform = get_u32(cpu->esp + 8), period = get_u32(cpu->esp + 16);
    g_road.magnitude = magnitude > 0 ? (uint32_t)magnitude : 0;
    g_road.waveform = waveform;
    g_road.period = period > 150 ? 150 : period;
    const uint64_t now = g_feedback_clock();
    if (!g_road.seen_ms || now - g_road.seen_ms >= 100)
        g_road.phase_live = 0;   /* calls resume after a lapse (air, pause, a stall): restart */
    g_road.seen_ms = now;
    if (setting("input_trace", 0)) {
        static uint32_t last_wave = 0xffffffffu, last_period, last_bucket = 0xffffffffu;
        const uint32_t bucket = g_road.magnitude / 500;
        if (waveform != last_wave || period != last_period || bucket != last_bucket) {
            char line[144];
            snprintf(line, sizeof line, "core.nfsc: road waveform=%u magnitude=%d period=%ums",
                     waveform, magnitude, period);
            api->log(api, line);
            last_wave = waveform; last_period = period; last_bucket = bucket;
        }
    }
}

static void rumble_install(void) {
    if (install(FFB_IMPACT, impact_feedback, POP_HOOK_REPLACE, 21) != POP_OK)
        log_line("core.nfsc: crash rumble unavailable (hook 00734cd0)");
    if (install(FFB_ROAD, road_feedback, POP_HOOK_BEFORE, 22) != POP_OK)
        log_line("core.nfsc: road-surface rumble unavailable (hook 00735270)");
}
