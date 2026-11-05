#include "imports.h"
#include "gdi32_internal.h"
#include "user32_internal.h"
#include "kernel32_internal.h"
#include "seh.h"
#include "profile.h"
#include "mods_seam.h"

// Defined in kernel32.cpp with the scheduler.
void sched_checkpoint();
#include "memory.h"
#include "../platform/ab_phase.h"

#include <map>
#include <mutex>
#include <set>
#include <cctype>
#include <vector>
#include <string>
#include <algorithm>

namespace {

struct Tramp {
    std::string dll;
    std::string name;
    std::string desc; // "DLL!name", stable storage for imports_describe
    uint8_t argc = 0;
    void (*fn)(X86 *) = nullptr;
    uint32_t calls = 0;
    // Entered without a scheduling checkpoint (see atomic_import).
    bool atomic = false;
    // Safe to call from a free-running guest thread beside the baton holder (free_import).
    bool free_ok = false;
    // A DirectSound method: run under g_dsound_m, so a free-running mixer and a baton thread never meet inside one.
    bool dsound = false;
};

// The imports a free-running guest thread (RECOMP_FREE_THREADS, kernel32_internal.h) may call without taking the
// baton: what NFS Carbon's sound mixer calls in steady play (measured 2026-11-06), each checked to keep its state
// under a host lock or in host atomics. Anything else takes the baton first.
bool free_import(const std::string &dll, const std::string &name) {
    static const char *const kernel[] = {"EnterCriticalSection", "LeaveCriticalSection", "TryEnterCriticalSection",
                                         "Sleep", "QueryPerformanceCounter", "QueryPerformanceFrequency",
                                         "InterlockedIncrement", "InterlockedDecrement", "InterlockedExchange",
                                         "InterlockedExchangeAdd", "InterlockedCompareExchange", "ReleaseSemaphore"};
    std::string d = dll;
    for (char &ch : d)
        ch = (char)tolower((unsigned char)ch);
    if (d == "kernel32.dll") {
        for (const char *k : kernel)
            if (name == k)
                return true;
        return false;
    }
    if (d == "winmm.dll")
        return name == "timeGetTime";
    if (d == "dsound.dll")
        return name == "IDirectSoundBuffer::GetCurrentPosition" || name == "IDirectSoundBuffer::Lock" ||
               name == "IDirectSoundBuffer::Unlock";
    return false;
}
std::recursive_mutex g_dsound_m;

// Imports whose callers have just set up state the call reads back, with no
// import in between: a thread switch at their entry checkpoint would let
// another thread replace it. Delphi's VCL sets its creation-control global and
// calls CreateWindowEx at once; the window procedure reads it on the first
// message.
bool atomic_import(const std::string &key) {
    return key == "user32.dll!CreateWindowExA" || key == "user32.dll!CreateWindowExW";
}

// Deliberately leaked: an atexit hook (RECOMP_IMPORT_STATS) reads these after
// static destructors would otherwise have run.
std::vector<Tramp> &tramps() {
    // Reserved to the most there can be, so the vector never moves: a free-running guest thread reads an entry
    // while the baton holder may be adding one.
    static auto *v = [] {
        auto *t = new std::vector<Tramp>();
        t->reserve(TRAMP_MAX);
        return t;
    }();
    return *v;
}
std::map<std::string, uint32_t> &index() {
    static auto *m = new std::map<std::string, uint32_t>();
    return *m;
}
std::map<std::string, ImportShim> &registry() {
    static auto *m = new std::map<std::string, ImportShim>();
    return *m;
}
std::set<std::string> &registered_dlls() {
    static auto *s = new std::set<std::string>();
    return *s;
}

struct DataImport {
    uint32_t size = 0;
    uint32_t addr = 0;
    std::string desc;
};
std::map<std::string, DataImport> &data_imports() {
    static auto *m = new std::map<std::string, DataImport>();
    return *m;
}

std::string lower_dll(const char *dll) {
    std::string k(dll ? dll : "");
    std::transform(k.begin(), k.end(), k.begin(),
                   [](unsigned char ch) { return (char)tolower(ch); });
    return k;
}

std::string key_of(const char *dll, const char *name) {
    std::string k = lower_dll(dll);
    k += '!';
    k += name ? name : "";
    return k;
}

// x86 stdcall exports may carry their stack byte count in _name@bytes.
// Only use this ABI evidence when no explicit signature was registered;
// fastcall (@name@bytes), C++ mangling and malformed suffixes remain unknown.
uint8_t decorated_stdcall_args(const char *name) {
    if (!name || name[0] != '_')
        return ARGC_UNKNOWN;
    const char *suffix = strrchr(name, '@');
    if (!suffix || suffix <= name + 1 || !suffix[1])
        return ARGC_UNKNOWN;
    unsigned bytes = 0;
    for (const char *p = suffix + 1; *p; ++p) {
        if (*p < '0' || *p > '9')
            return ARGC_UNKNOWN;
        bytes = bytes * 10 + unsigned(*p - '0');
        if (bytes > 4u * (ARGC_UNKNOWN - 1))
            return ARGC_UNKNOWN;
    }
    return bytes % 4 ? ARGC_UNKNOWN : uint8_t(bytes / 4);
}

} // namespace

void imports_register(const ImportShim *shims, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        const ImportShim &s = shims[i];
        if (s.dll && *s.dll)
            registered_dlls().insert(lower_dll(s.dll));
        std::string k = key_of(s.dll, s.name);
        auto it = registry().find(k);
        if (it != registry().end() && it->second.fn && !s.fn)
            continue;
        registry()[k] = s;
        // If a trampoline already exists, upgrade it in place.
        auto ti = index().find(k);
        if (ti != index().end()) {
            Tramp &t = tramps()[ti->second];
            if (s.fn)
                t.fn = s.fn;
            t.argc = s.argc_stdcall;
        }
    }
}

bool imports_has_dll(const char *dll_lower) {
    return registered_dlls().count(lower_dll(dll_lower)) != 0;
}

uint32_t imports_alloc_trampoline(const char *dll, const char *name, void (*fn)(X86 *),
                                  uint8_t argc_stdcall) {
    std::string k = key_of(dll, name);
    auto it = index().find(k);
    if (it != index().end()) {
        Tramp &t = tramps()[it->second];
        if (fn) {
            t.fn = fn;
            t.argc = argc_stdcall;
        }
        return TRAMP_BASE + TRAMP_STRIDE * it->second;
    }

    Tramp t;
    t.atomic = atomic_import(k);
    t.dll = dll ? dll : "?";
    t.name = name ? name : "?";
    t.desc = t.dll + "!" + t.name;
    t.fn = fn;
    t.argc = argc_stdcall;

    auto ri = registry().find(k);
    if (ri != registry().end()) {
        if (!t.fn)
            t.fn = ri->second.fn;
        if (!fn)
            t.argc = ri->second.argc_stdcall;
    }
    if (t.argc == ARGC_UNKNOWN)
        t.argc = decorated_stdcall_args(name);
    t.free_ok = free_import(t.dll, t.name);
    {
        std::string d = t.dll;
        for (char &ch : d)
            ch = (char)tolower((unsigned char)ch);
        t.dsound = d == "dsound.dll";
    }

    uint32_t idx = (uint32_t)tramps().size();
    if (idx >= TRAMP_MAX) {
        LOGW("imports: trampoline space exhausted (%u entries) for %s", idx, t.desc.c_str());
        return 0;
    }
    tramps().push_back(t);
    index()[k] = idx;
    return TRAMP_BASE + TRAMP_STRIDE * idx;
}

// The data imports D3DPopTB.exe has. weanetr.dll exports three GUIDs (16 bytes
// each) and options_to_parity_table (an int array; 4 KB of zeroed storage is
// what the plan specifies). Registering more is how a later task adds its own.
void imports_register_data(const char *dll, const char *name, uint32_t size) {
    std::string k = key_of(dll, name);
    DataImport &d = data_imports()[k];
    if (d.size < size)
        d.size = size;
    if (d.desc.empty())
        d.desc = std::string(dll) + "!" + name;
}

static void register_default_data_imports() {
    static bool done = false;
    if (done)
        return;
    done = true;
    imports_register_data("weanetr.dll", "?BFAID_INet@@3U_GUID@@A", 16);
    imports_register_data("weanetr.dll", "?BFAID_MODEM@@3U_GUID@@A", 16);
    imports_register_data("weanetr.dll", "?BFSPGUID_MODEM@@3U_GUID@@A", 16);
    imports_register_data("weanetr.dll", "?options_to_parity_table@@3PAHA", 4096);
}

uint32_t imports_alloc_data(const char *dll, const char *name) {
    register_default_data_imports();
    auto it = data_imports().find(key_of(dll, name));
    if (it == data_imports().end())
        return 0;
    DataImport &d = it->second;
    // mem_init() invalidates the heap, so a stale address is re-allocated.
    if (!d.addr || !heap_owns(d.addr)) {
        d.addr = heap_alloc(d.size, true);
        if (!d.addr)
            LOGW("imports: cannot allocate %u bytes for data import %s", d.size, d.desc.c_str());
        else
            LOGV("data import %s -> %u zeroed bytes at %08x", d.desc.c_str(), d.size, d.addr);
    }
    return d.addr;
}

uint32_t imports_data_address(const char *dll, const char *name) {
    auto it = data_imports().find(key_of(dll, name));
    return it == data_imports().end() ? 0 : it->second.addr;
}

uint32_t imports_data_count() {
    return (uint32_t)data_imports().size();
}

uint32_t imports_trampoline_for(const char *dll, const char *name) {
    auto it = index().find(key_of(dll, name));
    if (it == index().end())
        return 0;
    return TRAMP_BASE + TRAMP_STRIDE * it->second;
}

uint32_t imports_resolve(const char *dll, const char *name) {
    uint32_t a = imports_trampoline_for(dll, name);
    if (a)
        return a;
    auto ri = registry().find(key_of(dll, name));
    if (ri == registry().end())
        return 0;
    return imports_alloc_trampoline(ri->second.dll, ri->second.name, ri->second.fn,
                                    ri->second.argc_stdcall);
}

bool imports_serves_module(const char *dll) {
    if (!dll || !*dll)
        return false;
    // key_of applies the same DLL normalization as registration/resolution.
    // Including '!' makes the prefix match the complete module name only.
    std::string prefix = key_of(dll, nullptr);
    auto it = registry().lower_bound(prefix);
    return it != registry().end() && it->first.compare(0, prefix.size(), prefix) == 0;
}

const char *imports_describe(uint32_t target) {
    if (!imports_is_trampoline(target))
        return nullptr;
    uint32_t idx = (target - TRAMP_BASE) / TRAMP_STRIDE;
    if (idx >= tramps().size())
        return nullptr;
    return tramps()[idx].desc.c_str();
}

uint8_t imports_argc(uint32_t trampoline) {
    if (!imports_is_trampoline(trampoline))
        return 0;
    uint32_t idx = (trampoline - TRAMP_BASE) / TRAMP_STRIDE;
    return idx < tramps().size() ? tramps()[idx].argc : 0;
}

uint32_t imports_count() {
    return (uint32_t)tramps().size();
}

uint32_t imports_call_count(const char *dll, const char *name) {
    auto it = index().find(key_of(dll, name));
    if (it == index().end())
        return 0;
    return tramps()[it->second].calls;
}

void imports_dump_stats(FILE *out) {
    std::vector<const Tramp *> v;
    for (const Tramp &t : tramps())
        if (t.calls)
            v.push_back(&t);
    std::sort(v.begin(), v.end(),
              [](const Tramp *a, const Tramp *b) { return a->calls > b->calls; });
    fprintf(out, "import calls (%zu of %zu entries used):\n", v.size(), tramps().size());
    for (const Tramp *t : v)
        fprintf(out, "  %8u  %-40s %s\n", t->calls, t->desc.c_str(), t->fn ? "" : "(logging only)");
}

void imports_coverage(uint32_t *implemented, uint32_t *logging_only, uint32_t *unknown_argc) {
    uint32_t impl = 0, log_only = 0, unknown = 0;
    for (const Tramp &t : tramps()) {
        if (t.fn)
            ++impl;
        else
            ++log_only;
        if (t.argc == ARGC_UNKNOWN)
            ++unknown;
    }
    if (implemented)
        *implemented = impl;
    if (logging_only)
        *logging_only = log_only;
    if (unknown_argc)
        *unknown_argc = unknown;
}

void imports_dump_coverage(FILE *out) {
    std::map<std::string, std::pair<uint32_t, uint32_t>> per_dll; // implemented, logging-only
    std::vector<const Tramp *> log_only, unknown;
    for (const Tramp &t : tramps()) {
        auto &e = per_dll[t.dll];
        if (t.fn)
            ++e.first;
        else {
            ++e.second;
            log_only.push_back(&t);
        }
        if (t.argc == ARGC_UNKNOWN)
            unknown.push_back(&t);
    }
    fprintf(out, "import coverage by DLL (implemented / logging-only):\n");
    for (const auto &kv : per_dll)
        fprintf(out, "  %-16s %3u / %3u\n", kv.first.c_str(), kv.second.first, kv.second.second);
    fprintf(out, "logging-only imports (%zu):\n", log_only.size());
    for (const Tramp *t : log_only)
        fprintf(out, "  %s\n", t->desc.c_str());
    fprintf(out, "imports with an unknown stdcall argument count (%zu):\n", unknown.size());
    for (const Tramp *t : unknown)
        fprintf(out, "  %s\n", t->desc.c_str());
    fprintf(out, "data imports backed by guest storage (%zu):\n", data_imports().size());
    for (const auto &kv : data_imports())
        fprintf(out, "  %-44s %5u bytes at %08x\n", kv.second.desc.c_str(), kv.second.size,
                kv.second.addr);
}

void imports_dump_report(FILE *out) {
    std::vector<const Tramp *> called, not_reached, log_only;
    for (const Tramp &t : tramps()) {
        if (!t.fn)
            log_only.push_back(&t);
        else if (t.calls)
            called.push_back(&t);
        else
            not_reached.push_back(&t);
    }
    auto by_name = [](const Tramp *a, const Tramp *b) { return a->desc < b->desc; };
    std::sort(called.begin(), called.end(), by_name);
    std::sort(not_reached.begin(), not_reached.end(), by_name);
    std::sort(log_only.begin(), log_only.end(), by_name);

    fprintf(out, "implemented and called (%zu):\n", called.size());
    for (const Tramp *t : called)
        fprintf(out, "  %-44s %u calls\n", t->desc.c_str(), t->calls);
    fprintf(out, "implemented but not reached (%zu):\n", not_reached.size());
    for (const Tramp *t : not_reached)
        fprintf(out, "  %s\n", t->desc.c_str());
    fprintf(out, "logging-only (%zu), of which called: ", log_only.size());
    uint32_t hit = 0;
    for (const Tramp *t : log_only)
        if (t->calls)
            ++hit;
    fprintf(out, "%u\n", hit);
    for (const Tramp *t : log_only)
        fprintf(out, "  %-44s %s\n", t->desc.c_str(), t->calls ? "CALLED" : "");
}

namespace {
ImportReturnObserver g_return_observer = nullptr;
}

void imports_set_return_observer(ImportReturnObserver fn) {
    g_return_observer = fn;
}

namespace {
ImportCallObserver g_call_observer = nullptr;
}

void imports_set_call_observer(ImportCallObserver fn) {
    g_call_observer = fn;
}

ImportReturnObserver imports_set_return_observer_get(void) {
    return g_return_observer;
}

ImportCallObserver imports_set_call_observer_get(void) {
    return g_call_observer;
}

// Dispatch a guest import trampoline at a scheduler checkpoint.
// Copy dispatch metadata before calling a shim, then restore EIP/ESP according to its calling convention.
namespace {
uint64_t g_import_calls = 0;
} // namespace
uint64_t imports_call_count() {
    return g_import_calls;
}
void imports_call_leaves_surfaces() {
    if (!t_guest_free)
        --g_import_calls;
}

bool imports_dispatch(X86 *c, uint32_t target) {
    if (!imports_is_trampoline(target))
        return false;
    // A bounded scheduling checkpoint. Cooperative threads must not depend on
    // the guest happening to call a blocking API: the frame limiter spins on
    // GetTickCount and would otherwise starve the service threads for as long
    // as it liked, expired waits and all. Every call into the runtime is a
    // checkpoint, rate limited to one yield per millisecond per thread, and it
    // runs before the shim so the guest state is a consistent call boundary -
    // except at an atomic import, and inside a sched_atomic_enter stretch.
    uint32_t idx = (target - TRAMP_BASE) / TRAMP_STRIDE;
    // A thread RECOMP_FREE_THREADS lists runs without the baton while it calls only free_ok imports; any other
    // import (or shutdown, or the A/B's old half) takes the baton back first, so it runs exactly as before.
    if (RECOMP_UNLIKELY(t_guest_free_eligible)) {
        const bool free_ok = idx < tramps().size() && tramps()[idx].free_ok && sched_free_wanted();
        if (free_ok && !t_guest_free)
            sched_free_enter();
        else if (!free_ok && t_guest_free)
            sched_free_leave();
    }
    if (!t_guest_free && (idx >= tramps().size() || !tramps()[idx].atomic))
        sched_checkpoint();
    if (idx >= tramps().size()) {
        // No shim to say how many arguments to drop, but the return address
        // must still be consumed or the guest stack is displaced from here on.
        LOGW("imports: call to unallocated trampoline %08x (ESP=%08x, return=%08x)", target,
             c->r[R_ESP], rd32(c->r[R_ESP]));
        c->eip = rd32(c->r[R_ESP]);
        c->r[R_ESP] += 4;
        set_eax(c, 0);
        return true;
    }
    // The shim may allocate trampolines (GetProcAddress, a COM shim, a guest
    // callback), which reallocates this vector, so nothing may hold a
    // reference to the element across the call.
    void (*fn)(X86 *) = tramps()[idx].fn;
    uint8_t argc = tramps()[idx].argc;
    // A guest exception can longjmp across fn(c). Keep no C++ owner live
    // across that call, and do not retain a pointer into the movable vector:
    // the label is read from the table again after the shim returns (the
    // entry at idx never changes; only the vector may move).
    // Test71: every graphics call passes here (tens of thousands per frame),
    // so the per-call work outside the shim is kept to what is used. The log
    // level is read once (memory.cpp caches it at first use and it never
    // changes); below 2, LOGV and the SEH chain check do nothing.
    static const bool trace = log_level() >= 2;
    // Fetched at each use: a shim or an observer may allocate a trampoline and move the table.
    auto label = [idx] { return tramps()[idx].desc.c_str(); };
    if (!t_guest_free) { // plain counters: only the baton holder counts
        ++tramps()[idx].calls;
        ++g_import_calls;
    }

    uint32_t ret_addr = rd32(c->r[R_ESP]);
    if (trace) {
        log_msg(2, "-> %s (esp=%08x ret=%08x)", label(), c->r[R_ESP], ret_addr);
        recomp_seh_validate_chain(c, "import enter", label());
    }
    // TEST ONLY: the earlier phase of an in-run A/B (RECOMP_AB_PERIOD, platform/ab_phase.h)
    // repeats the per-call work this replaced - the label copy, the level reads and the
    // chain-check calls - so the two can be compared on the same scene.
    char ab_desc[512];
    const bool ab_old = recomp_ab::old(recomp_ab::IMPORTS);
    if (RECOMP_UNLIKELY(ab_old)) {
        const size_t n = std::min(tramps()[idx].desc.size(), sizeof ab_desc - 1);
        memcpy(ab_desc, tramps()[idx].desc.data(), n);
        ab_desc[n] = '\0';
        LOGV("-> %s (esp=%08x ret=%08x)", ab_desc, c->r[R_ESP], ret_addr);
        recomp_seh_validate_chain(c, "import enter", ab_desc);
    }

    // The arguments as they are NOW, before the shim runs: a stdcall shim pops
    // them, so after the call they are gone and this is the only point they
    // can be read. Only meaningful for a known stdcall count; a cdecl or
    // unknown shim reports none rather than inventing them.
    bool skip = false;
    if (g_call_observer) {
        uint32_t args[8];
        uint32_t n = (argc == ARGC_CDECL || argc == ARGC_UNKNOWN) ? 0u : (uint32_t)argc;
        if (n > 8)
            n = 8;
        for (uint32_t i = 0; i < n; ++i)
            args[i] = rd32(c->r[R_ESP] + 4 + 4 * i);
        uint32_t result = 0;
        if (!g_call_observer(label(), args, n, &result)) {
            set_eax(c, result);
            skip = true;
        }
    }

    if (skip) {
        // The observer refused the call and has set the result itself.
    } else if (fn) {
        if (RECOMP_UNLIKELY(tramps()[idx].dsound)) {
            g_dsound_m.lock();
            fn(c);
            g_dsound_m.unlock();
        } else {
            fn(c);
        }
    } else {
        log_once(label(), "unimplemented import %s: returning 0", label());
        set_eax(c, 0);
    }
    // Read after the shim ran, so the pointer is into the table as it is now.
    if (g_return_observer)
        g_return_observer(label(), c->r[R_EAX]);
    if (trace)
        log_msg(2, "<- %s (eax=%08x)", label(), c->r[R_EAX]);
    if (RECOMP_UNLIKELY(ab_old))
        LOGV("<- %s (eax=%08x)", ab_desc, c->r[R_EAX]);

    uint32_t pop = 4;
    if (argc == ARGC_CDECL) {
        pop = 4;
    } else if (argc == ARGC_UNKNOWN) {
        pop = 4;
        char key[512 + 6];
        snprintf(key, sizeof key, "%s#argc", label());
        log_once(key,
                 "%s has an unknown stdcall argument count: not adjusting ESP, "
                 "the guest stack will drift if it is really stdcall",
                 label());
    } else {
        pop = 4 + 4u * argc;
    }
    c->r[R_ESP] += pop;
    c->eip = ret_addr;
    if (trace)
        recomp_seh_validate_chain(c, "import leave", label());
    if (RECOMP_UNLIKELY(ab_old))
        recomp_seh_validate_chain(c, "import leave", ab_desc);
    return true;
}

// ---------------------------------------------------------------------------
// Calling guest code from a shim.
// ---------------------------------------------------------------------------
namespace {
struct Callback {
    X86 *cpu;
    jmp_buf env;
    uint32_t saved_esp, saved_eip, return_sp, profile_depth;
};
struct Callbacks {
    std::vector<Callback *> stack;
    ~Callbacks() {
        for (Callback *call : stack)
            delete call;
    }
};
thread_local Callbacks callbacks;
} // namespace

uint32_t recomp_callback_depth(void) {
    return (uint32_t)callbacks.stack.size();
}

void recomp_callback_truncate(uint32_t depth) {
    while (callbacks.stack.size() > depth) {
        delete callbacks.stack.back();
        callbacks.stack.pop_back();
    }
}

void recomp_callback_reset(X86 *c) {
    for (size_t i = callbacks.stack.size(); i-- > 0;)
        if (!c || callbacks.stack[i]->cpu == c) {
            delete callbacks.stack[i];
            callbacks.stack.erase(callbacks.stack.begin() + i);
        }
}

void recomp_callback_return(X86 *c) {
    if (callbacks.stack.empty())
        return;
    Callback *call = callbacks.stack.back();
    // Every translated return here is deeper on the host stack than this
    // live guest_call. Match the guest return slot as well: a completing SEH
    // landing may RET to an older callback's identical sentinel, in which
    // case intercept must finish that function through its SEH checkpoint.
    if (call->cpu != c || c->r[R_ESP] < call->return_sp + 4 || c->r[R_ESP] > call->saved_esp)
        return;
    mods_hooks_unwind_to_esp(call->return_sp);
    recomp_profile_truncate(call->profile_depth);
    longjmp(call->env, 1);
}

uint32_t guest_call(X86 *c, uint32_t fn, const uint32_t *args, int nargs) {
    Callback *call = new Callback{};
    call->cpu = c;
    call->saved_esp = c->r[R_ESP];
    call->saved_eip = c->eip;
    call->profile_depth = recomp_profile_depth();
    uint32_t esp = call->saved_esp;
    for (int i = nargs - 1; i >= 0; --i) {
        esp -= 4;
        wr32(esp, args[i]);
    }
    esp -= 4;
    wr32(esp, GUEST_RETURN_SENTINEL);
    c->r[R_ESP] = esp;
    call->return_sp = esp;
    callbacks.stack.push_back(call);
    if (!setjmp(call->env))
        recomp_run(c, fn);
    recomp_seh_callback_leave(c, recomp_callback_depth());
    uint32_t eax = c->r[R_EAX];
    c->r[R_ESP] = call->saved_esp;
    c->eip = call->saved_eip;
    callbacks.stack.pop_back();
    delete call;
    return eax;
}

// ---------------------------------------------------------------------------
// Shim tables owned by this runtime.
// ---------------------------------------------------------------------------
void imports_init() {
    static bool done = false;
    if (done)
        return;
    done = true;
    imports_register(g_kernel32_shims, g_kernel32_shim_count);
    kernel32_wide_register();
    oleaut32_register();
    misc_dlls_register();
    comctl32_register();
    media_foundation_register();
    imports_register(g_user32_shims, g_user32_shim_count);
    user32_wide_register();
    user32_vcl_register();
    imports_register(g_misc_shims, g_misc_shim_count);
    imports_register(g_gdi32_shims, g_gdi32_shim_count);
    extern void gdi_model_register();
    gdi_model_register();
    extern void gdi_bitmaps_register();
    gdi_bitmaps_register();
    gdi::register_draw();
    gdi::register_text();
    extern void msimg32_register();
    msimg32_register();
}
