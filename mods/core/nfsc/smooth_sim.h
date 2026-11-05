/* smooth_sim.h - even physics steps: one step on every on-time frame (included by nfsc.c).
 * Setting smooth_sim (Graphics, live, default on).
 *
 * The game's physics runs in fixed 1/60 s steps. The main loop 006b7930 passes each frame's time [0x00a99a5c]
 * to the sim service 00761150, which adds it to an accumulator [0x00b4d7ec] and runs floor(acc / step) steps
 * (at most 8), keeping the remainder. The frame time the game measures wobbles by about +-1.5 ms around
 * 16.67 ms (its own clock, quantized to 1/3600 s), even when the display shows every frame on time. Whenever
 * the remainder sits within that wobble of 0 or of a whole step, frames flip between 0 steps (the world stands
 * still for a refresh) and 2 steps (it jumps): 10-40 such pairs per 10 s in a 2026-11-06 session at a steady
 * 60.00 fps, which is the "60 fps but not smooth" feel.
 *
 * Fix: before each call, the remainder is pulled a little towards half a step (a tenth of the distance, at
 * most 0.25 ms per frame), so the wobble never reaches an edge and each on-time frame runs exactly one step.
 * A frame that is really late (two refreshes) still runs two steps. Game time never drifts from real time by
 * more than half a step (8.3 ms); the pull only moves the phase of the steps, not their length. */
#define SIM_SERVICE 0x00761150u
#define SIM_ACTIVE 0x00b4cfe0u
#define SIM_ACC 0x00b4d7ecu
#define SIM_STEP (1.0f / 60.0f)

/* Called with the frame time the sim service is about to add. */
static void smooth_sim_before(float dt) {
    if (!setting("smooth_sim", 1) || !get_u32(SIM_ACTIVE) || !(dt > 0.0f) || dt > 2.5f * SIM_STEP)
        return;
    const float r = bits_float(get_u32(SIM_ACC));
    if (!(r >= 0.0f) || r >= SIM_STEP)
        return;
    float pull = 0.1f * (0.5f * SIM_STEP - r);
    const float most = 0.00025f;
    pull = pull > most ? most : pull < -most ? -most : pull;
    put_float(SIM_ACC, r + pull);
}

static void smooth_sim(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv, (void)user;
    smooth_sim_before(bits_float(get_u32(cpu->esp + 4)));
    api->call_original(api, cpu->target, cpu);
}

static void smooth_sim_install(void) {
    if (install(SIM_SERVICE, smooth_sim, POP_HOOK_REPLACE, 28) != POP_OK)
        log_line("core.nfsc: even physics steps unavailable (hook 00761150)");
}
