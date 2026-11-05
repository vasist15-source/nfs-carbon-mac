// Test71 (TEST ONLY): an A/B of CPU-side changes inside one run, so both sides see the same
// traffic and scene mix (separate runs of the busy city vary by more than the changes measured).
// RECOMP_AB_PERIOD=<seconds>: during odd periods of the uptime clock (the clock the frame timings'
// sealed_s column uses) the switches that ask recomp_ab::old(which) take their earlier behaviour; during
// even periods, the current one. The phase is set once per presented frame on the main thread.
// Unset (every normal run): always the current behaviour; a switch costs one relaxed load.
#pragma once

#include "os.h"

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace recomp_ab {
inline std::atomic<int> old_phase{0};
inline uint64_t period_ns() {
    static const uint64_t p = [] {
        const char *e = recomp_env("AB_PERIOD");
        long s = e ? strtol(e, nullptr, 10) : 0;
        return s > 0 ? (uint64_t)s * 1000000000ull : 0ull;
    }();
    return p;
}
inline void tick() {
    // Test75: RECOMP_AB_FORCE_OLD=1 keeps the earlier behaviour for the whole run (image comparisons).
    static const bool force_old = [] {
        const char *e = recomp_env("AB_FORCE_OLD");
        return e && !strcmp(e, "1");
    }();
    if (force_old)
        old_phase.store(1, std::memory_order_relaxed);
    else if (const uint64_t p = period_ns())
        old_phase.store((int)((os_monotonic_ns() / p) & 1), std::memory_order_relaxed);
}
// Which switches take part: RECOMP_AB_SWITCHES=stride,sampler,imports,view,preshader,audio,free (default: all).
enum : uint32_t {
    STRIDE = 1u,
    SAMPLER = 2u,
    IMPORTS = 4u,
    VIEW = 8u,
    PRESHADER = 16u,
    AUDIO = 32u,
    CONSTS = 64u, // Test75: incremental effect constant binding (d3dx9.cpp bind_constants)
    MIRRORS = 128u, // Test75: per-draw lookups (d3d9.cpp resource mirrors, decoded programs)
    AABB = 256u,    // Test75: native bounding-box routines (native/aabb.cpp)
    FREE = 512u     // free-running guest threads (RECOMP_FREE_THREADS): the old half keeps them on the baton
};
inline uint32_t switches() {
    static const uint32_t m = [] {
        const char *e = recomp_env("AB_SWITCHES");
        if (!e || !*e)
            return ~0u;
        uint32_t bits = 0;
        if (strstr(e, "stride"))
            bits |= STRIDE;
        if (strstr(e, "sampler"))
            bits |= SAMPLER;
        if (strstr(e, "imports"))
            bits |= IMPORTS;
        if (strstr(e, "view"))
            bits |= VIEW;
        if (strstr(e, "preshader"))
            bits |= PRESHADER;
        if (strstr(e, "audio")) // Test73: the native sound-mixer loops (native/audio_mix.cpp)
            bits |= AUDIO;
        if (strstr(e, "consts"))
            bits |= CONSTS;
        if (strstr(e, "mirrors"))
            bits |= MIRRORS;
        if (strstr(e, "aabb"))
            bits |= AABB;
        if (strstr(e, "free"))
            bits |= FREE;
        return bits;
    }();
    return m;
}
inline bool old(uint32_t which) {
    return old_phase.load(std::memory_order_relaxed) != 0 && (switches() & which) != 0;
}
} // namespace recomp_ab
