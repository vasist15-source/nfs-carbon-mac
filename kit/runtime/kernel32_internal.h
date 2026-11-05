// Internal ANSI/W implementation seam. Names are UTF-8; addresses stay guest values.
#pragma once
#include "imports.h"
#include <string>

void kernel32_wide_register();
void create_file_named(X86 *c, const std::string &name);
void get_file_attributes_named(X86 *c, const std::string &name);
void set_file_attributes_named(X86 *c, const std::string &name);
void create_directory_named(X86 *c, const std::string &name);
void remove_directory_named(X86 *c, const std::string &name);
void delete_file_named(X86 *c, const std::string &name);
void copy_file_named(X86 *c, const std::string &source, const std::string &dest);
void find_first_named(X86 *c, const std::string &pattern, bool wide);
void find_next(X86 *c, bool wide);
std::string full_path_named(const std::string &name);
void volume_information_named(X86 *c, const std::string &root, bool wide);
void disk_free_space(X86 *c);
void drive_type_named(X86 *c, const std::string &root);
void logical_drive_strings(X86 *c, bool wide);
void get_file_attributes_ex_named(X86 *c, const std::string &name);

void create_event_named(X86 *c, const std::string &name);
void create_mutex_named(X86 *c, const std::string &name);
void create_mapping_named(X86 *c, const std::string &name);
void open_mutex_named(X86 *c, const std::string &name);

void kernel32_wide_reset();

void get_command_line(X86 *c);
void startup_info(X86 *c);
void wait_multiple_objects(X86 *c);
void kernel32_wide_reset_command_line();

// Free-running guest threads (RECOMP_FREE_THREADS, 2026-11-06). Normally one guest thread runs at a time (the
// baton). A thread whose SetThreadName name is listed in RECOMP_FREE_THREADS (comma separated; NFS Carbon's sound
// mixer "RWAudioCore Dac") runs on its own host core beside the baton holder while it only calls the imports that
// are safe for that (imports.cpp free_ok: critical sections, Sleep, the clocks, Interlocked*, ReleaseSemaphore,
// DirectSound buffer position/Lock/Unlock). Any other import takes the baton first, as before.
extern __thread bool t_guest_free;          // this thread runs without the baton right now
extern __thread bool t_guest_free_eligible; // this thread is listed in RECOMP_FREE_THREADS
bool sched_free_wanted();                   // eligible, not shutting down, not the A/B's old half
void sched_free_enter();                    // baton holder -> free running (hands the baton on)
void sched_free_leave();                    // free running -> takes the baton again (waits for it)
