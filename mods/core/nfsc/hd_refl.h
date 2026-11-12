/* hd_refl.h - HD reflections (included by nfsc.c). Setting hd_reflections (Graphics, restart, default on).
 *
 * Aero_'s NFSC HD Reflections (github.com/AeroWidescreen/NFSHDReflections, SOURCE/NFSC, GPL-3; credited
 * in NOTICE) was read for its addresses and what each part changes; the code here is our own. That mod
 * is a Windows .asi that rewrites the exe's code bytes, which a recompiled game cannot load, so the same
 * changes are made with slots the patched instructions read (game.toml, defaults in data_seeds = the
 * game's own values) and hooks that fill them. With the setting off every slot holds the game's value.
 *
 * SIZES (the mod's AutoRes, from the game's own resolution [0x00ab0ac8] x [0x00ab0acc] instead of the
 * desktop's; never smaller than the game's sizes). Filled before each render-target creator runs:
 *   road reflection 0071a950: width = slot 0x00c2b434 (the widescreen fix's 320), height = slot
 *     0x00c2b800 (MOV EDX at 0071a9f7, 240); its view size (MOV imm 320 / 240 into [ECX+0x14] /
 *     [ECX+0x18] at 0071bdee.. / 0071be25.. in 0071bca0) is read from the same two slots;
 *   car reflection cube map 0070de00: size = (AND EAX, slot 0x00c2b804 at 0070de38) + 16, 0 when the
 *     reflection update rate [0x00a65340] is 0 (so 16); the game's 0xf0 -> rows - 16;
 *   rear-view mirror 0070dab0: 1 << floor(log2(w / 3)) x 1 << floor(log2(h / 3)) (MOV EDX,1 / SHL at
 *     0070db03 / 0070db61 -> slots 0x00c2b808 / 0x00c2b80c, computed here) -> w / 2 x h / 6;
 *   picture-in-picture (0070dc20 reads .data 0x00a63b00 / 04, 256) -> h / 2;
 *   road reflection blur 007130e0 (pow2 of w / 2 x h / 2; the mod NOPs both SARs): its reads of the
 *     game's size (007130fc / 007130e4) come from slots 0x00c2b82c / 0x00c2b830 = 2w x 2h.
 *
 * DETAIL (the mod's ImproveReflectionLOD, while driving, [0x00a99bbc] == 6): 0079fb5a TEST CH,0x30 ->
 * TEST [slot 0x00c2b820],ECX with 0x1000; 0079aedb OR EDX,0x8002180 (the cube faces' render flags) ->
 * OR EDX,[slot 0x00c2b824] = 0x8002; the mirror (view 3) gets the main view's flags: 0079aeaa LEA
 * ESI,[EDI-1] (switch index = view id - 1) -> MOVZX ESI,byte [EDI + 0x00c2b8ff], a table of the
 * identity (0xff = no case) in which entry 2 becomes 0.
 *
 * SKY (RestoreSkybox) in the sky draw 007afb70 (view [esp+4], id at +8): views above 3 (the road
 * reflection 4 and the cube faces 0x16..0x1b) take the main view's path (CMP EAX,3 at 007afca1 ->
 * slot 0x00c2b810 = INT_MAX) with the mod's scale 0.25 instead of [0x00a732ac] (0.7; three FLDs ->
 * slot 0x00c2b814, set per view by a hook); the road reflection's sky 0.06 / 300.0 (0x009ef050 at
 * 007afc39, 0x009ef05c at 007afcf0 -> slots 0x00c2b818 / 0x00c2b81c) -> 2.0 / 1.0.
 *
 * SHAPE (AlignmentFix + AspectRatioFix): the cube faces (views 0x16..0x1b) are projected square with
 * the game's own field-of-view terms (setup_projection in nfsc.c asks hd_refl_cube_view); shader
 * constant 0x37, [0x00a6c228] = 0.4 while driving (0073e1b0) -> slot 0x00c2b828 = 0.0.
 *
 * Also always with the setting on: the mirror on by default for new profiles / Restore Defaults (004bd50f
 * stores 0 -> slot 0x00c2b888), and the QRACE cube map's faces (00572c20 returns its name 0x009d083c;
 * the mod then swaps the face names XNEG/XPOS <-> ZNEG/ZPOS in .rdata 0x009e87e4.. for good; here only
 * while that cube map is the one asked for).
 *
 * THE MOD'S OTHER OPTIONS (Mods > Visual; each reads its setting while the game runs). hd_reflections is
 * the master switch: with it off every option is the game's own (at once; sizes after a restart).
 * reflection_details (RestoreDetails 1/2): after the road reflection's draw (call to 0072c9b0 returning
 *   to 0072e193) the cars are drawn into it too ([0x00a6523c] = 3, 007cca20(view 0x00b4b110, 1),
 *   0072c9b0), and after its flares (007507d0 returning to 0072e1e5) their lights in colour (007d5dc0(view,
 *   2, 0), 0072c9b0); in that view the headlight beams (007be170 from 007cbdd3) are left out of the pass
 *   marked 2 and the flares (0074d330 from 007cc641) out of the others ([ebp+0xc8] = the view, byte
 *   [ebp+0xcc] = the pass). Level 2: after the mirror (call to 00749e80 returning to 0072e453), its
 *   particles (if byte [0x00a6536c] > 0: 0075aa10 thiscall 0x00b4be70 (view 0x00b4b090), 0072c9b0), and
 *   0075aa10's own call of 00754bf0 (return 0075ab3a) only for the main view (id 1).
 * reflection_contrast (ReflectionContrast 1 Xbox 360 / 2 PS3): while a cube face is drawn (0072e460,
 *   view in [esp+4]) the world shader (type [EDI+4] = 1) is the game's shader 3 ([0x00b1f66c]): 00729440..
 *   0072945a get a fallback override table (slot 0x00c2b834 -> 9 dwords, -1 = keep) for meshes without
 *   their own; the traffic signs' shader [0x00b42fc8] gets detail 2 (+0x1774); shader constant 0x8b
 *   (two floats, 0x00a6c204 / 0x00a6c214) is lowered for cube faces ([[0x00ab08f4]+4] >= 0x16) at its three
 *   calls of 00713b30 (returns 00748aa2: y - 1.6 / PS3 0.5; 007497eb: y - 0.95, menus x + 2.5, y - 1.0;
 *   007498b7, menus only: y - 1.75); brake flares in the cube faces 180.0 (0074d330: CMP EAX,1 at 0074d9c2
 *   -> slot 0x00c2b858 = that view's id, size slot 0x00c2b85c).
 * traffic_sign_fix: material [[0x00a6b910]+0x74] = 8.0; their draw distance 22500 (0x009ee7f0 at 0079b255)
 *   -> 45000 (slot 0x00c2b884).
 * real_fe_reflections: the menus run the driving paths: [0x00a99bbc] read from slot 0x00c2b870 (= 6) at
 *   0072ecd0, 0072e4de (redirects) and 0073e197 (patch), set before 0072eba0; cube far clip [0x00a63b14]
 *   (100.0, read at 00721a90) -> 160000.0; detail as while driving; brightness x 0.15 without contrast.
 * env_map (ForceEnvironmentMap): 0070f800's index argument from 00748ae9 (return 00748aee).
 * true_flare_size: 007507d0's CMP EAX,0x1b (cube faces, 00750a42) / CMP EAX,3 (mirror, 00750a38) -> slots
 *   0x00c2b878 / 0x00c2b874, so the jumps that enlarge their flares are never taken.
 * flare_rotation off (DisableFlareRotation): [0x00a6c094] 240.0 -> 0, and 0074de9a's 1.0 (slot
 *   0x00c2b860) is -1.0 in the cube faces.
 * realistic_chrome: chrome material [[0x00a6b910]-0xb90/-0xbf0/-0xc00] = 6.0 / 0.125 / 0.25 with shader
 *   detail High ([0x00a63e60] == 3), reflection updates on ([0x00a65340]) and car reflections ([0x00a63e77]).
 * reflection_blur (ReflectionBlurStrength): DownScale4x4 0073cc10's two FDIVR 1.0 (0073cc29/3d -> slot
 *   0x00c2b864) when its caller was called from the road reflection (0072e22e), TwoPassBlur's (0073c97c ->
 *   0x00c2b868).
 * car_reflection_brightness: the six FMUL 0.5 in 0071e030 (slot 0x00c2b880).
 * visual_treatment_race / _menu: 0071d700's LUT [0x00b43068] -> slot 0x00c2b86c = FILTER0..3
 *   ([0x00b43068 + 4k]), set before each call.
 * pause_blur: the four blur getters 0073eca0/b0 (+1.0) and 0073ecc0/d0 (+10.0) while paused
 *   ([0x00a8ad18]) and not in photo mode ([0x00a590a0] != 0x1f41).
 * Not ported: CopFlareScale (its size depends on which flare a loop is drawing, which no slot can tell).
 */
#define REFL_ROAD_CREATE 0x0071a950u
#define REFL_CUBE_CREATE 0x0070de00u
#define REFL_MIRROR_CREATE 0x0070dab0u
#define REFL_BLUR_CREATE 0x007130e0u
#define REFL_SKY_DRAW 0x007afb70u
#define REFL_GAME_W 0x00ab0ac8u
#define REFL_GAME_H 0x00ab0accu
#define REFL_PIP_W 0x00a63b00u
#define REFL_PIP_H 0x00a63b04u
#define REFL_SKY_SCALE_GAME 0x00a732acu
#define REFL_GAME_STATE 0x00a99bbcu

#define SLOT_ROAD_W 0x00c2b434u /* shared with the widescreen fix's water reflections */
#define SLOT_ROAD_H 0x00c2b800u
#define SLOT_CUBE_AND 0x00c2b804u
#define SLOT_MIRROR_W 0x00c2b808u
#define SLOT_MIRROR_H 0x00c2b80cu
#define SLOT_SKY_CMP 0x00c2b810u
#define SLOT_SKY_SCALE 0x00c2b814u
#define SLOT_SKY_ROAD_SCALE 0x00c2b818u
#define SLOT_SKY_ROAD_TURN 0x00c2b81cu
#define SLOT_LOD_MASK 0x00c2b820u
#define SLOT_LOD_FLAGS 0x00c2b824u
#define SLOT_PARAM_37 0x00c2b828u
#define SLOT_BLUR_W 0x00c2b82cu
#define SLOT_BLUR_H 0x00c2b830u
#define SLOT_LOD_VIEWS 0x00c2b900u /* byte per view id - 1; id 0 reads 0x00c2b8ff */
#define SLOT_SHADER_TABLE 0x00c2b834u
#define SLOT_P8B_A 0x00c2b840u /* two floats each */
#define SLOT_P8B_B 0x00c2b848u
#define SLOT_P8B_C 0x00c2b850u
#define SLOT_BRAKE_ID 0x00c2b858u
#define SLOT_BRAKE_SIZE 0x00c2b85cu
#define SLOT_FLARE_TURN 0x00c2b860u
#define SLOT_DS_STRENGTH 0x00c2b864u
#define SLOT_TP_STRENGTH 0x00c2b868u
#define SLOT_LUT 0x00c2b86cu
#define SLOT_STATE 0x00c2b870u
#define SLOT_TF_MIRROR 0x00c2b874u
#define SLOT_TF_CUBE 0x00c2b878u
#define SLOT_CUBE_FAR 0x00c2b87cu
#define SLOT_CAR_BRIGHT 0x00c2b880u
#define SLOT_SIGN_DIST 0x00c2b884u
#define SLOT_MIRROR_DEFAULT 0x00c2b888u
#define TABLE_SHADERS_NONE 0x00c2b980u /* 9 x -1 */
#define TABLE_SHADERS_CUBE 0x00c2b9c0u /* type 1 -> shader 3 */

#define REFL_STATE_FN 0x0072eba0u      /* draws the frame's reflections */
#define REFL_CUBE_FACE 0x0072e460u     /* cdecl (view, ...) */
#define REFL_DRAW_PASS 0x0072c9b0u
#define REFL_ROAD_DRAWN_RET 0x0072e193u
#define REFL_CARS_DRAW 0x007cca20u     /* cdecl (view, pass) */
#define REFL_FLARES 0x007507d0u
#define REFL_FLARES_RET 0x0072e1e5u
#define REFL_LIGHTS_DRAW 0x007d5dc0u   /* cdecl (view, pass, 0) */
#define REFL_HEADLIGHTS 0x007be170u    /* thiscall, RET 0xc */
#define REFL_HEADLIGHTS_RET 0x007cbdd8u
#define REFL_CAR_FLARES 0x0074d330u    /* cdecl (view, ...) */
#define REFL_CAR_FLARES_RET 0x007cc646u
#define REFL_MIRROR_DRAWN 0x00749e80u
#define REFL_MIRROR_DRAWN_RET 0x0072e453u
#define REFL_PARTICLES 0x0075aa10u     /* thiscall (view), RET 4 */
#define REFL_PARTICLES_MAIN 0x00754bf0u
#define REFL_PARTICLES_MAIN_RET 0x0075ab3au
#define REFL_SET_VECTOR 0x00713b30u    /* thiscall (id, float *) */
#define REFL_ENV_TEXTURE 0x0070f800u   /* cdecl (index) */
#define REFL_ENV_TEXTURE_RET 0x00748aeeu
#define REFL_DOWNSCALE 0x0073cc10u
#define REFL_LUT_FN 0x0071d700u
#define REFL_CUBE_NAME 0x00572c20u
#define REFL_ROAD_VIEW 0x00b4b110u
#define REFL_MIRROR_VIEW 0x00b4b090u
#define REFL_FACE_NAMES 0x009e87e4u    /* ZNEG ZPOS YNEG YPOS XNEG XPOS, 8 bytes apart */

static int g_hd_refl;
static int g_hd_refl_logged;
static int g_hd_refl_driving = -1;

static uint32_t refl_pow2_floor(uint32_t v) {
    uint32_t p = 1;
    while (v >= 2 * p && p < 0x40000000u)
        p *= 2;
    return p;
}
static uint32_t refl_at_least(uint32_t v, uint32_t floor) {
    return v > floor ? v : floor;
}

/* Is this view one of the car reflection's six cube faces? Asked by setup_projection. */
static int hd_refl_cube_view(uint32_t id) {
    return g_hd_refl && id >= 0x16 && id <= 0x1b;
}

static void refl_fill(void) {
    const uint32_t w = get_u32(REFL_GAME_W), h = get_u32(REFL_GAME_H);
    if (w < 64 || h < 64 || w > 16384 || h > 16384)
        return;
    const uint32_t mirror_w = refl_pow2_floor(w / 3), mirror_h = refl_pow2_floor(h / 3);
    const int on = g_hd_refl;
    g_api->guest_write_u32(g_api, SLOT_ROAD_W, on ? refl_at_least(w, 320) : 320);
    g_api->guest_write_u32(g_api, SLOT_ROAD_H, on ? refl_at_least(h, 240) : 240);
    g_api->guest_write_u32(g_api, SLOT_CUBE_AND, on ? refl_at_least(h, 256) - 16 : 0xf0);
    g_api->guest_write_u32(g_api, SLOT_MIRROR_W, on ? refl_at_least(w / 2, mirror_w) : mirror_w);
    g_api->guest_write_u32(g_api, SLOT_MIRROR_H, on ? refl_at_least(h / 6, mirror_h) : mirror_h);
    g_api->guest_write_u32(g_api, REFL_PIP_W, on ? refl_at_least(h / 2, 256) : 256);
    g_api->guest_write_u32(g_api, REFL_PIP_H, on ? refl_at_least(h / 2, 256) : 256);
    g_api->guest_write_u32(g_api, SLOT_BLUR_W, on ? 2 * w : w);
    g_api->guest_write_u32(g_api, SLOT_BLUR_H, on ? 2 * h : h);
    if (on && !g_hd_refl_logged) {
        g_hd_refl_logged = 1;
        char line[160];
        snprintf(line, sizeof line,
                 "core.nfsc: HD reflections for %ux%u: road %ux%u, car %u, mirror %ux%u, picture-in-picture %u", w, h,
                 refl_at_least(w, 320), refl_at_least(h, 240), refl_at_least(h, 256), refl_at_least(w / 2, mirror_w),
                 refl_at_least(h / 6, mirror_h), refl_at_least(h / 2, 256));
        log_line(line);
    }
}

static void refl_before_create(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)api, (void)cpu, (void)inv, (void)user;
    refl_fill();
}

/* Before the sky draw: the mod's scale for the reflections' views (above 3), the game's otherwise. */
static void refl_sky(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)api, (void)inv, (void)user;
    const uint32_t view = get_u32(cpu->esp + 4);
    const uint32_t id = view ? get_u32(view + 8) : 0;
    g_api->guest_write_u32(g_api, SLOT_SKY_SCALE, id > 3 ? float_bits(0.25f) : get_u32(REFL_SKY_SCALE_GAME));
}

/* The mod's other options, read each frame. */
static struct {
    int details, contrast, signs, fe, env, true_flares, turn, chrome, blur, bright, lut_race, lut_menu, pause_blur;
} g_rx;
static uint32_t g_rx_sign_orig, g_rx_sign_obj;
static int g_rx_fe_setting, g_rx_chrome_setting; /* restart settings, read at start */
static int g_rx_qrace;

static uint32_t refl_current_view_id(void) {
    const uint32_t v = get_u32(0x00ab08f4u);
    return v ? get_u32(v + 4) : 0;
}
static uint32_t refl_arg_view_id(const pop_cpu_v1 *cpu) {
    const uint32_t v = get_u32(cpu->esp + 4);
    return v ? get_u32(v + 8) : 0;
}
static int refl_cube_id(uint32_t id) {
    return id >= 0x16 && id <= 0x1b;
}
static int refl_driving(void) {
    return get_u32(REFL_GAME_STATE) == 6;
}

/* RestoreDetails: cars and their lights in the road reflection. */
static void rx_road_cars(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)cpu, (void)inv, (void)user;
    if (g_rx.details < 1)
        return;
    const uint32_t pass = get_u32(0x00a6523cu);
    const uint32_t args[2] = {REFL_ROAD_VIEW, 1};
    uint32_t eax;
    api->guest_write_u32(api, 0x00a6523cu, 3);
    api->guest_call(api, REFL_CARS_DRAW, 0, args, 2, &eax);
    api->guest_call(api, REFL_DRAW_PASS, 0, NULL, 0, &eax);
    api->guest_write_u32(api, 0x00a6523cu, pass);
}
static void rx_road_lights(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)cpu, (void)inv, (void)user;
    if (g_rx.details < 1)
        return;
    const uint32_t args[3] = {REFL_ROAD_VIEW, 2, 0};
    uint32_t eax;
    api->guest_call(api, REFL_LIGHTS_DRAW, 0, args, 3, &eax);
    api->guest_call(api, REFL_DRAW_PASS, 0, NULL, 0, &eax);
}
static int rx_road_pass(const pop_cpu_v1 *cpu, int *lights_pass) {
    uint8_t pass = 0;
    if (g_rx.details < 1 || get_u32(cpu->ebp + 0xc8) != REFL_ROAD_VIEW)
        return 0;
    g_api->guest_read_u8(g_api, cpu->ebp + 0xcc, &pass);
    *lights_pass = pass == 2;
    return 1;
}
static void rx_headlights(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv, (void)user;
    int lights;
    if (rx_road_pass(cpu, &lights) && lights) {
        api->hook_return(api, cpu, cpu->eax, 0xc);
        return;
    }
    api->call_original(api, cpu->target, cpu);
}
static void rx_car_flares(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv, (void)user;
    int lights;
    if (rx_road_pass(cpu, &lights) && !lights) {
        api->hook_return(api, cpu, cpu->eax, 0);
        return;
    }
    api->call_original(api, cpu->target, cpu);
}
static void rx_mirror_particles(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)cpu, (void)inv, (void)user;
    uint8_t on = 0;
    if (g_rx.details < 2)
        return;
    api->guest_read_u8(api, 0x00a6536cu, &on);
    if ((int8_t)on <= 0)
        return;
    const uint32_t args[1] = {REFL_MIRROR_VIEW};
    uint32_t eax;
    api->guest_call(api, REFL_PARTICLES, 0x00b4be70u, args, 1, &eax);
    api->guest_call(api, REFL_DRAW_PASS, 0, NULL, 0, &eax);
}
static void rx_particles_main(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv, (void)user;
    if (g_rx.details >= 2 && refl_arg_view_id(cpu) != 1) {
        api->hook_return(api, cpu, cpu->eax, 0);
        return;
    }
    api->call_original(api, cpu->target, cpu);
}

/* ReflectionContrast: the world shader in the cube faces, shader constant 0x8b, brake flares. */
static void rx_cube_enter(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)cpu, (void)inv, (void)user;
    api->guest_write_u32(api, SLOT_SHADER_TABLE, g_rx.contrast ? TABLE_SHADERS_CUBE : TABLE_SHADERS_NONE);
}
static void rx_cube_leave(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)cpu, (void)inv, (void)user;
    api->guest_write_u32(api, SLOT_SHADER_TABLE, TABLE_SHADERS_NONE);
}
static void rx_vector(pop_cpu_v1 *cpu, uint32_t scratch, float add, float sub) {
    const uint32_t src = get_u32(cpu->esp + 8);
    if (!src)
        return;
    put_float(scratch, bits_float(get_u32(src)) + add);
    put_float(scratch + 4, bits_float(get_u32(src + 4)) - sub);
    g_api->guest_write_u32(g_api, cpu->esp + 8, scratch);
}
static void rx_p8b_a(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)api, (void)inv, (void)user;
    if (g_rx.contrast && refl_current_view_id() >= 0x16)
        rx_vector(cpu, SLOT_P8B_A, 0.0f, g_rx.contrast > 1 ? 0.5f : 1.6f);
}
static void rx_p8b_b(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)api, (void)inv, (void)user;
    if (!g_rx.contrast || refl_current_view_id() < 0x16)
        return;
    if (refl_driving())
        rx_vector(cpu, SLOT_P8B_B, 0.0f, 0.95f);
    else
        rx_vector(cpu, SLOT_P8B_B, 2.5f, 1.0f);
}
static void rx_p8b_c(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)api, (void)inv, (void)user;
    if (g_rx.contrast && !refl_driving() && refl_current_view_id() >= 0x16)
        rx_vector(cpu, SLOT_P8B_C, 0.0f, 1.75f);
}
/* Before 0074d330 (a car's flares, for one view): brake flare size and flare turn for that view. */
static void rx_flares_view(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv, (void)user;
    const uint32_t id = refl_arg_view_id(cpu);
    const int cube = refl_cube_id(id);
    api->guest_write_u32(api, SLOT_BRAKE_ID, g_rx.contrast && cube ? id : 1);
    put_float(SLOT_BRAKE_SIZE, g_rx.contrast && cube ? 180.0f : 360.0f);
    put_float(SLOT_FLARE_TURN, !g_rx.turn && cube ? -1.0f : 1.0f);
}

/* ForceEnvironmentMap. */
static void rx_env_map(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv, (void)user;
    if (g_rx.env > 0)
        api->guest_write_u32(api, cpu->esp + 4, (uint32_t)(g_rx.env > 5 ? 4 : g_rx.env - 1));
}

/* ReflectionBlurStrength: DownScale4x4 for the road reflection (its caller's return address). */
static float rx_blur_strength(void) {
    static const float k[] = {1.0f, 0.5f, 0.0f, 2.0f};
    return k[g_rx.blur >= 0 && g_rx.blur <= 3 ? g_rx.blur : 0];
}
static void rx_downscale(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)api, (void)inv, (void)user;
    put_float(SLOT_DS_STRENGTH, get_u32(cpu->ebp + 4) == 0x0072e22eu ? rx_blur_strength() : 1.0f);
}

/* ReplaceVisualTreatmentIG / FE. */
static void rx_lut(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)cpu, (void)inv, (void)user;
    int k = refl_driving() ? g_rx.lut_race : g_rx.lut_menu;
    k = k < 0 ? 0 : k > 3 ? 3 : k;
    api->guest_write_u32(api, SLOT_LUT, get_u32(0x00b43068u + 4u * (uint32_t)k));
}

/* PauseBlur: the four blur amounts, more while the game is paused. */
static void rx_pause_blur(const PopModApi *api, pop_cpu_v1 *cpu, float add) {
    api->call_original(api, cpu->target, cpu);
    if (g_rx.pause_blur && get_u32(0x00a8ad18u) && get_u32(0x00a590a0u) != 0x1f41)
        cpu->st[cpu->fpu_top & 7u] += add; /* ST0 */
}
static void rx_pause_blur_1(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv, (void)user;
    rx_pause_blur(api, cpu, 1.0f);
}
static void rx_pause_blur_10(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv, (void)user;
    rx_pause_blur(api, cpu, 10.0f);
}

/* FECubemapFix: the QRACE cube map's X and Z faces swapped, only while it is the one asked for. */
static void rx_cube_name(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv, (void)user;
    const int qrace = g_hd_refl && cpu->eax == 0x009d083cu;
    if (qrace == g_rx_qrace)
        return;
    g_rx_qrace = qrace;
    api->guest_write_u32(api, REFL_FACE_NAMES + 0x00, qrace ? 0x47454e58u : 0x47454e5au); /* ZNEG slot: XNEG */
    api->guest_write_u32(api, REFL_FACE_NAMES + 0x08, qrace ? 0x534f5058u : 0x534f505au); /* ZPOS slot: XPOS */
    api->guest_write_u32(api, REFL_FACE_NAMES + 0x20, qrace ? 0x47454e5au : 0x47454e58u); /* XNEG slot: ZNEG */
    api->guest_write_u32(api, REFL_FACE_NAMES + 0x28, qrace ? 0x534f505au : 0x534f5058u); /* XPOS slot: ZPOS */
}

/* RealFrontEndReflections: the state the reflection code compares with 6. */
static void rx_state(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)cpu, (void)inv, (void)user;
    api->guest_write_u32(api, SLOT_STATE, g_rx.fe ? 6u : get_u32(REFL_GAME_STATE));
}

/* Each frame (pad.h): settings, and the values that follow the game's state. */
static void hd_refl_tick(void) {
    /* The master switch (hd_reflections): off, every option below is the game's own at once (the
     * sizes, detail and sky follow it at the next start). */
    const int on = setting("hd_reflections", 1) != 0;
    g_rx.details = on ? (int)setting("reflection_details", 1) : 0;
    g_rx.contrast = on ? (int)setting("reflection_contrast", 1) : 0;
    g_rx.signs = on && setting("traffic_sign_fix", 1) != 0;
    g_rx.env = on ? (int)setting("env_map", 0) : 0;
    g_rx.true_flares = on ? (int)setting("true_flare_size", 0) : 0;
    g_rx.turn = !on || setting("flare_rotation", 1) != 0;
    g_rx.blur = on ? (int)setting("reflection_blur", 0) : 0;
    g_rx.bright = on ? (int)setting("car_reflection_brightness", 0) : 0;
    g_rx.lut_race = on ? (int)setting("visual_treatment_race", 0) : 0;
    g_rx.lut_menu = on ? (int)setting("visual_treatment_menu", 0) : 0;
    g_rx.pause_blur = on && setting("pause_blur", 0) != 0;
    g_rx.fe = on && g_rx_fe_setting;
    g_rx.chrome = on && g_rx_chrome_setting;
    const int driving = refl_driving();

    /* reflection detail (and the menus' with real reflections there) */
    if (g_hd_refl) {
        const int more = driving || g_rx.fe;
        if (more != g_hd_refl_driving) {
            g_hd_refl_driving = more;
            g_api->guest_write_u32(g_api, SLOT_LOD_MASK, more ? 0x1000u : 0x3000u);
            g_api->guest_write_u32(g_api, SLOT_LOD_FLAGS, more ? 0x8002u : 0x8002180u);
        }
    }
    put_float(SLOT_CUBE_FAR, g_rx.fe && !driving ? 160000.0f : bits_float(get_u32(0x00a63b14u)));
    g_api->guest_write_u32(g_api, SLOT_TF_MIRROR, g_rx.true_flares >= 2 ? 0xffffffffu : 3u);
    g_api->guest_write_u32(g_api, SLOT_TF_CUBE, g_rx.true_flares >= 1 ? 0x15u : 0x1bu);
    put_float(0x00a6c094u, g_rx.turn ? 240.0f : 0.0f);
    put_float(SLOT_TP_STRENGTH, rx_blur_strength());
    {
        static const float k[] = {1.0f, 0.5f, 0.75f, 1.25f, 1.5f, 2.0f};
        const float b = k[g_rx.bright >= 0 && g_rx.bright <= 5 ? g_rx.bright : 0];
        put_float(SLOT_CAR_BRIGHT, (g_rx.fe && !driving && !g_rx.contrast ? 0.15f : 0.5f) * b);
    }

    /* traffic signs: material brightness (the game's value back when turned off) and distance */
    const uint32_t mats = get_u32(0x00a6b910u);
    if (mats) {
        if (mats != g_rx_sign_obj) {
            g_rx_sign_obj = mats;
            g_rx_sign_orig = get_u32(mats + 0x74);
        }
        g_api->guest_write_u32(g_api, mats + 0x74, g_rx.signs ? 0x41000000u : g_rx_sign_orig);
        if (g_rx_chrome_setting) { /* 1.0 = the mod's "not realistic" values, also when turned off */
            uint8_t detail = 0, rate = 0, cars = 0;
            g_api->guest_read_u8(g_api, 0x00a63e60u, &detail);
            g_api->guest_read_u8(g_api, 0x00a65340u, &rate);
            g_api->guest_read_u8(g_api, 0x00a63e77u, &cars);
            const int real = g_rx.chrome && detail == 3 && (int8_t)rate > 0 && (int8_t)cars > 0;
            put_float(mats - 0xb90, real ? 6.0f : 1.0f);
            put_float(mats - 0xbf0, real ? 0.125f : 1.0f);
            put_float(mats - 0xc00, real ? 0.25f : 1.0f);
        }
    }
    put_float(SLOT_SIGN_DIST, g_rx.signs ? 45000.0f : 22500.0f);
    /* the traffic signs' shader detail while the contrast change is on */
    const uint32_t sign_shader = get_u32(0x00b42fc8u);
    if (sign_shader && g_rx.contrast)
        g_api->guest_write_u32(g_api, sign_shader + 0x1774, 2);
}

static void rx_install(void) {
    g_rx_fe_setting = g_hd_refl && setting("real_fe_reflections", 0) != 0;
    g_rx_chrome_setting = g_hd_refl && setting("realistic_chrome", 0) != 0;
    hd_refl_tick();
    struct {
        uint32_t addr, ret;
        PopHookFn fn;
        int32_t mode;
        int slot;
        const char *what;
    } const hooks[] = {
        {REFL_DRAW_PASS, REFL_ROAD_DRAWN_RET, rx_road_cars, POP_HOOK_AFTER, 35, "cars in the road reflection"},
        {REFL_FLARES, REFL_FLARES_RET, rx_road_lights, POP_HOOK_AFTER, 36, "lights in the road reflection"},
        {REFL_HEADLIGHTS, REFL_HEADLIGHTS_RET, rx_headlights, POP_HOOK_REPLACE, 37, "road reflection headlights"},
        {REFL_CAR_FLARES, REFL_CAR_FLARES_RET, rx_car_flares, POP_HOOK_REPLACE, 38, "road reflection flares"},
        {REFL_MIRROR_DRAWN, REFL_MIRROR_DRAWN_RET, rx_mirror_particles, POP_HOOK_AFTER, 39, "mirror particles"},
        {REFL_PARTICLES_MAIN, REFL_PARTICLES_MAIN_RET, rx_particles_main, POP_HOOK_REPLACE, 40, "mirror particles"},
        {REFL_SET_VECTOR, 0x00748aa2u, rx_p8b_a, POP_HOOK_BEFORE, 41, "reflection contrast"},
        {REFL_SET_VECTOR, 0x007497ebu, rx_p8b_b, POP_HOOK_BEFORE, 42, "reflection contrast"},
        {REFL_SET_VECTOR, 0x007498b7u, rx_p8b_c, POP_HOOK_BEFORE, 43, "reflection contrast"},
        {REFL_ENV_TEXTURE, REFL_ENV_TEXTURE_RET, rx_env_map, POP_HOOK_BEFORE, 44, "one environment map"},
        {REFL_CUBE_FACE, 0, rx_cube_enter, POP_HOOK_BEFORE, 45, "reflection contrast"},
        {REFL_CUBE_FACE, 0, rx_cube_leave, POP_HOOK_AFTER, 46, "reflection contrast"},
        {REFL_CAR_FLARES, 0, rx_flares_view, POP_HOOK_BEFORE, 47, "flare sizes"},
        {REFL_DOWNSCALE, 0, rx_downscale, POP_HOOK_BEFORE, 48, "road reflection blur"},
        {REFL_LUT_FN, 0, rx_lut, POP_HOOK_BEFORE, 49, "visual treatment"},
        {0x0073eca0u, 0, rx_pause_blur_1, POP_HOOK_REPLACE, 50, "pause blur"},
        {0x0073ecb0u, 0, rx_pause_blur_1, POP_HOOK_REPLACE, 51, "pause blur"},
        {0x0073ecc0u, 0, rx_pause_blur_10, POP_HOOK_REPLACE, 52, "pause blur"},
        {0x0073ecd0u, 0, rx_pause_blur_10, POP_HOOK_REPLACE, 53, "pause blur"},
        {REFL_CUBE_NAME, 0, rx_cube_name, POP_HOOK_AFTER, 54, "QRACE cube map"},
        {REFL_STATE_FN, 0, rx_state, POP_HOOK_BEFORE, 55, "real reflections in menus"},
    };
    int failed = 0;
    for (unsigned i = 0; i < sizeof hooks / sizeof hooks[0]; ++i) {
        PopModStatus st = hooks[i].ret
                              ? g_api->hook_install_at_callsite(g_api, hooks[i].addr, hooks[i].ret, hooks[i].fn,
                                                                hooks[i].mode, 0, &g_hooks[hooks[i].slot])
                              : install(hooks[i].addr, hooks[i].fn, hooks[i].mode, hooks[i].slot);
        if (st != POP_OK) {
            char line[128];
            snprintf(line, sizeof line, "core.nfsc: %s unavailable (hook %08x, %d)", hooks[i].what, hooks[i].addr,
                     (int)st);
            log_line(line);
            failed = 1;
        }
    }
    char line[200];
    snprintf(line, sizeof line,
             "core.nfsc: reflection options: details %d, contrast %d, signs %d, menus %d, env map %d, flares %d/%d, "
             "chrome %d%s",
             g_rx.details, g_rx.contrast, g_rx.signs, g_rx.fe, g_rx.env, g_rx.true_flares, g_rx.turn, g_rx.chrome,
             failed ? " (some unavailable)" : "");
    log_line(line);
}

static void hd_refl_install(void) {
    g_hd_refl = setting("hd_reflections", 1) != 0;
    /* Always: with the setting off these fill the slots with the game's own sizes. */
    if (install(REFL_ROAD_CREATE, refl_before_create, POP_HOOK_BEFORE, 29) != POP_OK ||
        install(REFL_CUBE_CREATE, refl_before_create, POP_HOOK_BEFORE, 30) != POP_OK ||
        install(REFL_MIRROR_CREATE, refl_before_create, POP_HOOK_BEFORE, 31) != POP_OK ||
        install(REFL_BLUR_CREATE, refl_before_create, POP_HOOK_BEFORE, 32) != POP_OK) {
        log_line("core.nfsc: reflection sizes unavailable; HD reflections off");
        g_hd_refl = 0;
    }
    rx_install();
    if (!g_hd_refl) {
        log_line("core.nfsc: HD reflections off");
        return;
    }
    g_api->guest_write_u8(g_api, SLOT_LOD_VIEWS + 2, 0); /* the mirror (view 3) as the main view (1) */
    put_float(SLOT_SKY_ROAD_SCALE, 2.0f);
    put_float(SLOT_SKY_ROAD_TURN, 1.0f);
    put_float(SLOT_PARAM_37, 0.0f);
    g_api->guest_write_u32(g_api, SLOT_MIRROR_DEFAULT, 1); /* the mirror on in new profiles */
    /* Without the per-view scale the reflections' sky would be drawn at the main view's size. */
    if (install(REFL_SKY_DRAW, refl_sky, POP_HOOK_BEFORE, 34) == POP_OK)
        g_api->guest_write_u32(g_api, SLOT_SKY_CMP, 0x7fffffffu);
    log_line("core.nfsc: HD reflections on");
}
