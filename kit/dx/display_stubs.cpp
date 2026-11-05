// display_stubs.cpp - a definition for every display interface Task 1
// declares, so the tasks that come after it link before it is implemented.
//
// EVERY ONE OF THESE SAYS "NOT IMPLEMENTED", and says it the way its own
// signature can: a status returns the failure, a pointer returns null, a count
// returns zero. None of them pretends to succeed, because a stub that answered
// plausibly would let a caller be written against behaviour that does not
// exist yet and look correct until the day it is replaced.
//
// A later task deletes the stubs it implements from this file. When the file
// is empty the interface is done.
#include "host_api.h"
#include "../runtime/display_seam.h"

#include <stddef.h>
#include <string.h>

extern "C" {

// Everything else this file once held - palettes, revisions, frames and draws
// - is implemented in ddraw.cpp as of DISP-T2, and was deleted from here when
// it was. That is how this file is meant to shrink: a task takes a stub away
// by implementing it, and when the file holds nothing but this comment the
// interface is done.

// The presenter's refresh delay: a build without a presenter has no display
// to wait for, so a Present with a sync interval returns at once.
__attribute__((weak)) double host_present_refresh_delay(int) {
    return 0.0;
}
// No presenter: the D3D9 frame limit keeps its own clock.
__attribute__((weak)) int host_present_slot_clock(double *, double *) {
    return 0;
}
__attribute__((weak)) uint64_t host_present_unique_completed() {
    return 0;
}
__attribute__((weak)) uint64_t host_present_drops() {
    return 0;
}
__attribute__((weak)) uint64_t host_present_repeats() {
    return 0;
}

// Without a host GPU the Direct3D 11 shim rasterizes everything itself.
__attribute__((weak)) int host_gpu2d_available(void) {
    return 0;
}
__attribute__((weak)) void host_gpu2d_texture(uint32_t, int, int, const uint8_t *, int, int, int,
                                              int) {}
__attribute__((weak)) void host_gpu2d_forget(uint32_t) {}
__attribute__((weak)) void host_gpu2d_reset(void) {}
__attribute__((weak)) void host_gpu2d_clear(uint32_t, int, int, const float *) {}
__attribute__((weak)) int host_gpu2d_draw(uint32_t, int, int, uint32_t,
                                          const struct HostGpu2DQuad *) {
    return 0;
}
__attribute__((weak)) uint32_t host_gpu2d_generation(void) {
    return 0;
}
__attribute__((weak)) int host_gpu2d_readback(uint32_t, int, int, uint8_t *) {
    return 0;
}
__attribute__((weak)) void host_display_present_gpu2d(uint32_t, int, int) {}

} // extern "C"
