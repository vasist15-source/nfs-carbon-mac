// launch.c - the packaged app's executable (Contents/MacOS/Launch).
//
// Finder starts this; it sets up the places a Mac game keeps its things and
// then runs the real game binary beside it (CarbonRecomp):
//
//   ~/Library/Application Support/NFS Carbon/            profile (saves, settings)
//       Documents/NFS Carbon/<profile>/                  the game's own save files
//       registry.json                                    the game's options
//       mod-settings.json                                the port's options
//       Logs/<date>-<time>.log, Logs/latest.log          one log per launch (10 kept)
//       ShaderWarm/                                      Metal shaders to precompile
//
// Defaults come from Contents/Resources/Defaults the first time only, so a
// player's settings are never overwritten by an update. The game itself is
// Contents/Resources/Game/NFSC.exe, built from the player's own copy.
//
// Any RECOMP_* variable already set in the environment wins, so Terminal
// runs can still override the profile, the log level and so on.
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define PROFILE_NAME "NFS Carbon"
#define GAME_BINARY "CarbonRecomp"
#define LOGS_KEPT 10

static void mkdirs(const char *path) {
    char buf[PATH_MAX];
    snprintf(buf, sizeof buf, "%s", path);
    for (char *p = buf + 1; *p; ++p)
        if (*p == '/') {
            *p = 0;
            mkdir(buf, 0755);
            *p = '/';
        }
    mkdir(buf, 0755);
}

static int exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

static void copy_if_missing(const char *src, const char *dst) {
    if (exists(dst) || !exists(src))
        return;
    FILE *in = fopen(src, "rb"), *out = in ? fopen(dst, "wb") : NULL;
    if (in && out) {
        char buf[65536];
        size_t n;
        while ((n = fread(buf, 1, sizeof buf, in)) > 0)
            fwrite(buf, 1, n, out);
    } else {
        fprintf(stderr, "launch: could not copy %s to %s\n", src, dst);
    }
    if (out)
        fclose(out);
    if (in)
        fclose(in);
}

static void set_default(const char *name, const char *value) {
    if (!getenv(name))
        setenv(name, value, 1);
}

static int by_name(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

// Keep the newest LOGS_KEPT dated logs (their names sort by time).
static void prune_logs(const char *dir) {
    DIR *d = opendir(dir);
    if (!d)
        return;
    char *names[1024];
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) && n < 1024) {
        size_t len = strlen(e->d_name);
        if (len > 4 && !strcmp(e->d_name + len - 4, ".log") && e->d_name[0] >= '0' && e->d_name[0] <= '9')
            names[n++] = strdup(e->d_name);
    }
    closedir(d);
    qsort(names, n, sizeof *names, by_name);
    for (int i = 0; i < n; ++i) {
        if (i < n - LOGS_KEPT) {
            char p[PATH_MAX];
            snprintf(p, sizeof p, "%s/%s", dir, names[i]);
            unlink(p);
        }
        free(names[i]);
    }
}

// A whole-number value from mod-settings.json ("key": n), or `fallback`.
static long mod_setting(const char *profile, const char *key, long fallback) {
    char path[PATH_MAX], want[128], buf[65536];
    snprintf(path, sizeof path, "%s/mod-settings.json", profile);
    FILE *f = fopen(path, "rb");
    if (!f)
        return fallback;
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    snprintf(want, sizeof want, "\"%s\":", key);
    const char *at = strstr(buf, want);
    return at ? strtol(at + strlen(want), NULL, 10) : fallback;
}

int main(int argc, char **argv) {
    (void)argc;
    char self[PATH_MAX], real[PATH_MAX];
    uint32_t size = sizeof self;
    if (_NSGetExecutablePath(self, &size) != 0 || !realpath(self, real)) {
        fprintf(stderr, "launch: cannot find my own path\n");
        return 1;
    }
    char macos[PATH_MAX], resources[PATH_MAX], game[PATH_MAX], binary[PATH_MAX];
    snprintf(macos, sizeof macos, "%s", dirname(real));
    snprintf(resources, sizeof resources, "%s/../Resources", macos);
    if (realpath(resources, real))
        snprintf(resources, sizeof resources, "%s", real);
    snprintf(game, sizeof game, "%s/Game", resources);
    snprintf(binary, sizeof binary, "%s/" GAME_BINARY, macos);

    const char *home = getenv("HOME");
    if (!home || !*home) {
        fprintf(stderr, "launch: HOME is not set\n");
        return 1;
    }
    char profile[PATH_MAX], logs[PATH_MAX], path[PATH_MAX], def[PATH_MAX];
    const char *env_profile = getenv("RECOMP_PROFILE_DIR");
    if (env_profile && *env_profile)
        snprintf(profile, sizeof profile, "%s", env_profile);
    else
        snprintf(profile, sizeof profile, "%s/Library/Application Support/" PROFILE_NAME, home);
    snprintf(logs, sizeof logs, "%s/Logs", profile);
    mkdirs(logs);

    static const char *const defaults[] = {"registry.json", "mod-settings.json"};
    for (size_t i = 0; i < sizeof defaults / sizeof *defaults; ++i) {
        snprintf(def, sizeof def, "%s/Defaults/%s", resources, defaults[i]);
        snprintf(path, sizeof path, "%s/%s", profile, defaults[i]);
        copy_if_missing(def, path);
    }

    set_default("RECOMP_PROFILE_DIR", profile);
    snprintf(path, sizeof path, "%s/registry.json", profile);
    set_default("RECOMP_REGISTRY", path);
    snprintf(path, sizeof path, "%s/NFSC.exe", game);
    set_default("RECOMP_EXE", path);
    set_default("RECOMP_LOG", "1");
    // Render resolution (core.nfsc/render_scale): 0 Screen follows the screen (the
    // renderer's default: 3D drawn at the Retina size whatever the game resolution),
    // 1 draws at 1.5x the game resolution, 2 (the default) at the game resolution.
    // The renderer reads the scale once, at start-up.
    switch (mod_setting(profile, "core.nfsc/render_scale", 2)) {
    case 1:
        set_default("RECOMP_D3D9_SCALE", "1.5");
        break;
    case 2:
        set_default("RECOMP_D3D9_SCALE", "1");
        break;
    default:
        break;
    }
    // The game's sound mixer on its own core (core.nfsc/mixer_own_core, default on): the runtime lets the guest
    // thread that names itself "RWAudioCore Dac" run beside the main thread instead of taking turns with it.
    if (mod_setting(profile, "core.nfsc/mixer_own_core", 1))
        set_default("RECOMP_FREE_THREADS", "RWAudioCore Dac");
    // Metal shaders this Mac has compiled before, recompiled in the background
    // at start-up: a cold shader cache stalled the first frames after the EA
    // splash on a first launch (2026-10-10).
    snprintf(path, sizeof path, "%s/ShaderWarm", profile);
    mkdirs(path);
    set_default("RECOMP_D3D9_SHADER_WARM_DIR", path);

    // One log per launch, plus Logs/latest.log pointing at it.
    char stamp[32], log[PATH_MAX], latest[PATH_MAX];
    time_t now = time(NULL);
    strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", localtime(&now));
    snprintf(log, sizeof log, "%s/%s.log", logs, stamp);
    int fd = open(log, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        dup2(fd, STDOUT_FILENO);
        dup2(fd, STDERR_FILENO);
        close(fd);
        snprintf(latest, sizeof latest, "%s/latest.log", logs);
        unlink(latest);
        symlink(log, latest);
    }
    prune_logs(logs);

    if (chdir(game) != 0) {
        fprintf(stderr, "launch: cannot enter %s: %s\n", game, strerror(errno));
        return 1;
    }
    argv[0] = binary;
    execv(binary, argv);
    fprintf(stderr, "launch: cannot run %s: %s\n", binary, strerror(errno));
    return 1;
}
