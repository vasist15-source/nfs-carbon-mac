/* camera.h - look around the car with the controller's right stick (core.nfsc).
 *
 * The idea is ThirteenAG's free look (NFSCarbon.WidescreenFix Camera.ixx, MIT;
 * see NOTICE); the feel is a modern racing game's: the stick's direction is
 * where you look (up = ahead, right = right, down = behind) and on release the
 * camera glides back behind the car.
 *
 * WHERE. CubicCameraMover::Update (004921b0, thiscall, float dt at [ebp + 8];
 * the mover in ESI throughout) builds the view each frame with
 * eCreateLookAtMatrix (0071b430, cdecl: bMatrix4 *out, bVector3 *eye,
 * bVector3 *target, bVector3 *up; world Z is up) at 00492e5b, after the game
 * has placed and smoothed the eye and target. A hook on that one call turns
 * the view before the matrix is made; the game's own camera collision and
 * shake (00483440 / 0048d110 / 00482eb0) still run on the result. The mover's
 * view type is [mover + 0xac] (00486500 maps it 1:1: 0 bumper, 1 hood, 2..4
 * the chase views) and its look-back flag [mover + 0xb1]. Chase views orbit
 * the eye around the target, at the same distance and height; bumper and hood
 * turn the target around the eye (a head turn). Look-back resets the turn.
 *
 * STICK. The right stick is DirectInput's Z (X axis) and Rz (Y axis), range
 * 0..65535, in the device state pad.h reads the triggers from. The game's data
 * format puts X Y Z at +0 / +4 / +8 and Rx Ry Rz at +0x20c / +0x210 / +0x214
 * (as its binding reader 0067a340 reads them; +0x208 is a button). An axis that a driving action is bound to in Options > Controls
 * (e.g. shifting put back on the stick) is left to that action. */

#define CAMERA_LOOK_AT 0x0071b430u     /* eCreateLookAtMatrix */
#define CAMERA_LOOK_AT_RET 0x00492e60u /* after the call in CubicCameraMover::Update */
#define CAMERA_VIEW_TYPE 0xacu         /* in the mover */
#define CAMERA_LOOK_BACK 0xb1u
#define PAD_STATE_RSX 0x008u           /* lZ */
#define PAD_STATE_RSY 0x214u           /* lRz, + = down */
#define CAMERA_DEAD 0.30f              /* stick travel before it counts */
#define CAMERA_FOLLOW_S 0.07f          /* time constants: towards the stick */
#define CAMERA_RETURN_S 0.15f          /* and back behind the car */
#define CAMERA_PI 3.14159265f

static float g_cam_yaw; /* radians, + = anticlockwise seen from above */
static int g_cam_logged; /* bit 0: the hook ran, bit 1: the stick was pushed */

static float cam_wrap(float a) {
    while (a > CAMERA_PI)
        a -= 2.0f * CAMERA_PI;
    while (a < -CAMERA_PI)
        a += 2.0f * CAMERA_PI;
    return a;
}

/* Is port p's stick axis (kind -/+, control 2) bound to a driving or HUD action
 * (rows 0..28 but 23, the menus' FE_ACTION_B2)? */
static int cam_axis_taken(unsigned p, uint32_t kind_neg) {
    for (unsigned r = 0; r <= 28; ++r) {
        if (r == 23)
            continue;
        for (unsigned s = 0; s < 2; ++s) {
            uint32_t at = pad_triple(r, 2u * p + 2u + s);
            if (!at)
                continue;
            uint32_t kind = get_u32(at);
            if ((kind == kind_neg || kind == kind_neg + 1) && get_u32(at + 8) == 2)
                return 1;
        }
    }
    return 0;
}

/* The right stick of whichever controller pushes it furthest, -1..1. */
static void cam_stick(const PopModApi *api, float *x, float *y) {
    float best = 0.0f;
    *x = *y = 0.0f;
    for (unsigned p = 0; p < pad_ports(); ++p) {
        uint32_t device = get_u32(PAD_DEVICES + p * 4u), state = 0;
        if (!device)
            continue;
        uint32_t getter = get_u32(get_u32(device) + 4);
        if (!getter || api->guest_call(api, getter, device, NULL, 0, &state) != POP_OK || !state)
            continue;
        uint32_t rx = get_u32(state + PAD_STATE_RSX), ry = get_u32(state + PAD_STATE_RSY);
        if (rx > 65535u || ry > 65535u)
            continue;
        float sx = cam_axis_taken(p, 4) ? 0.0f : ((float)rx - 32768.0f) / 32767.0f;
        float sy = cam_axis_taken(p, 6) ? 0.0f : ((float)ry - 32768.0f) / 32767.0f;
        if (sx * sx + sy * sy > best) {
            best = sx * sx + sy * sy;
            *x = sx;
            *y = sy;
        }
    }
}

static void cam_rotate(uint32_t v, uint32_t centre, float c, float s) {
    float cx = centre ? bits_float(get_u32(centre)) : 0.0f, cy = centre ? bits_float(get_u32(centre + 4)) : 0.0f;
    float dx = bits_float(get_u32(v)) - cx, dy = bits_float(get_u32(v + 4)) - cy;
    put_float(v, cx + dx * c - dy * s);
    put_float(v + 4, cy + dx * s + dy * c);
}

/* 0071b430, called from 00492e5b only: turn the view before it is built. */
static void camera_look_at(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv;
    (void)user;
    const uint32_t mover = cpu->esi;
    const uint32_t eye = get_u32(cpu->esp + 8), target = get_u32(cpu->esp + 12), up = get_u32(cpu->esp + 16);
    uint8_t look_back = 0;
    api->guest_read_u8(api, mover + CAMERA_LOOK_BACK, &look_back);
    const uint32_t type = get_u32(mover + CAMERA_VIEW_TYPE);
    char line[160];
    if (!(g_cam_logged & 1)) {
        g_cam_logged |= 1;
        snprintf(line, sizeof line, "core.nfsc: right-stick camera running (view type %u, frame %.4f s)", type,
                 bits_float(get_u32(cpu->ebp + 8)));
        log_line(line);
    }
    if (!setting("right_stick_camera", 1) || look_back || type > 4 || !eye || !target) {
        g_cam_yaw = 0.0f;
        return;
    }
    float dt = bits_float(get_u32(cpu->ebp + 8));
    if (!(dt > 0.0f && dt < 0.25f))
        dt = 1.0f / 60.0f;
    float x, y;
    cam_stick(api, &x, &y);
    if (x * x + y * y > CAMERA_DEAD * CAMERA_DEAD) {
        if (!(g_cam_logged & 2)) {
            g_cam_logged |= 2;
            snprintf(line, sizeof line, "core.nfsc: right stick first pushed: x %.2f y %.2f, view type %u", x, y, type);
            log_line(line);
        }
        /* Looking right turns the view clockwise: a negative angle. */
        float want = -atan2f(x, -y);
        float step = cam_wrap(want - g_cam_yaw); /* the short way round */
        g_cam_yaw = cam_wrap(g_cam_yaw + step * (1.0f - expf(-dt / CAMERA_FOLLOW_S)));
    } else if (g_cam_yaw != 0.0f) {
        g_cam_yaw -= g_cam_yaw * (1.0f - expf(-dt / CAMERA_RETURN_S));
        if (fabsf(g_cam_yaw) < 0.002f)
            g_cam_yaw = 0.0f;
    }
    if (g_cam_yaw == 0.0f)
        return;
    const float c = cosf(g_cam_yaw), s = sinf(g_cam_yaw);
    if (type >= 2)
        cam_rotate(eye, target, c, s);  /* orbit the car */
    else
        cam_rotate(target, eye, c, s);  /* turn the head */
    if (up)
        cam_rotate(up, 0, c, s);        /* the camera's lean turns with it */
}

static void camera_install(void) {
    if (!g_api->hook_install_at_callsite ||
        g_api->hook_install_at_callsite(g_api, CAMERA_LOOK_AT, CAMERA_LOOK_AT_RET, camera_look_at, POP_HOOK_BEFORE,
                                        0, &g_hooks[24]) != POP_OK)
        log_line("core.nfsc: right-stick camera unavailable");
}
