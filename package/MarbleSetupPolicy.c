// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <sys/param.h>
#include <sys/proc.h>
#include <sys/sysproto.h>
#include <sys/fcntl.h>
#include <sys/systm.h>
#include <sys/vnode.h>
#include <pexpert/pexpert.h>

int native_marble_setup_policy(proc_t, user_addr_t);
volatile int marble_setup_policy_status = -1;

static int exists(const char *path)
{
    vnode_t node = NULLVP;
    int error = vnode_lookup(path, 0, &node, vfs_context_current());
    if (!error) vnode_put(node);
    return error;
}

static int move(proc_t process, user_addr_t scratch, const char *from, const char *to)
{
    int error = copyout(from, scratch, strlen(from) + 1);
    if (!error) error = copyout(to, scratch + 0x180, strlen(to) + 1);
    if (error) return error;
    struct rename_args request = {.from = scratch, .to = scratch + 0x180};
    int32_t result = 0;
    return rename(process, &request, &result);
}

static int writePreference(proc_t process, user_addr_t scratch, const char *name, const char *body)
{
    char path[256], temporary[288], backup[288];
    snprintf(path, sizeof(path), "/private/var/mobile/Library/Preferences/%s.plist", name);
    snprintf(temporary, sizeof(temporary), "%s.marble-tmp", path);
    snprintf(backup, sizeof(backup), "%s.marble-before-skip", path);
    int error = copyout(temporary, scratch, strlen(temporary) + 1);
    if (error) return error;
    struct open_args request = {.path = scratch,
        .flags = O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, .mode = 0600};
    int32_t result = 0;
    error = open(process, &request, &result);
    if (error) return error;
    int fd = result;
    const char *parts[] = {"<?xml version=\"1.0\"?><plist version=\"1.0\"><dict>", body, "</dict></plist>\n"};
    for (unsigned i = 0; i < 3 && !error; ++i) {
        size_t length = strlen(parts[i]);
        error = copyout(parts[i], scratch + 0x400, length);
        if (!error) {
            struct write_args write_request = {.fd = fd, .cbuf = scratch + 0x400, .nbyte = length};
            user_ssize_t written = 0;
            error = write(process, &write_request, &written);
            if (!error && written != (user_ssize_t)length) error = EIO;
        }
    }
    struct fchown_args owner = {.fd = fd, .uid = 501, .gid = 501};
    if (!error) error = fchown(process, &owner, &result);
    struct fchmod_args mode = {.fd = fd, .mode = 0600};
    if (!error) error = fchmod(process, &mode, &result);
    struct fsync_args sync = {.fd = fd};
    if (!error) error = fsync(process, &sync, &result);
    struct close_args close_request = {.fd = fd};
    int close_error = sys_close(process, &close_request, &result);
    if (!error) error = close_error;
    if (error) return error;
    int backup_error = exists(backup);
    if (backup_error && backup_error != ENOENT) return backup_error;
    int path_error = exists(path);
    if (path_error && path_error != ENOENT) return path_error;
    if (backup_error == ENOENT && !path_error) {
        error = move(process, scratch, path, backup);
        if (error) return error;
    }
    return move(process, scratch, temporary, path);
}

static int refreshDisplayCache(proc_t process, user_addr_t scratch)
{
    unsigned scale = 0;
    if (!PE_parse_boot_argn("marble-display-scale", &scale, sizeof(scale)) || !scale) return 0;
    unsigned identity = 0;
    PE_parse_boot_argn("marble-ui-identity", &identity, sizeof(identity));
    static const char *paths[] = {
        "/private/var/containers/Shared/SystemGroup/systemgroup.com.apple.mobilegestaltcache/Library/Caches/com.apple.MobileGestalt.plist",
        "/private/var/mobile/Library/Preferences/com.apple.MobileGestalt.plist",
        "/private/var/mobile/Library/Preferences/com.apple.mobilegestalt.plist"
    };
    for (unsigned i = 0; i < sizeof(paths) / sizeof(paths[0]); ++i) {
        char backup[256];
        snprintf(backup, sizeof(backup), "%s.marble-before-ui-%u-scale%u", paths[i], identity, scale);
        int error = exists(paths[i]);
        if (error == ENOENT) continue;
        if (!error) error = move(process, scratch, paths[i], backup);
        printf("MarbleDisplay: scale=%u cache=%s status=%d\n", scale, paths[i], error);
        if (error) return error;
    }
    return 0;
}

static int retireSystemAppRequest(proc_t process, user_addr_t scratch)
{
    const char *path = "/private/var/preferences/com.apple.demo.SADForceInstall.plist";
    const char *retired = "/private/var/preferences/com.apple.demo.SADForceInstall.plist.marble-union-retired";
    const char *backup = "/private/var/preferences/com.apple.demo.SADForceInstall.plist.marble-before-app-restore";
    int error = 0;
    if (exists(retired) == ENOENT && !exists(path)) error = move(process, scratch, path, retired);
    if (!error && exists(path) == ENOENT && !exists(backup)) error = move(process, scratch, backup, path);
    return error;
}

int native_marble_setup_policy(proc_t process, user_addr_t scratch)
{
    int app_error = retireSystemAppRequest(process, scratch);
    if (app_error) return app_error;
    int display_error = refreshDisplayCache(process, scratch);
    if (display_error) return display_error;
    unsigned enabled = 0;
    if (!PE_parse_boot_argn("marble-skip-setup", &enabled, sizeof(enabled)) || enabled != 1) return 0;
    const char *marker = "/private/var/db/marble-setup-complete";
    int error = exists(marker);
    if (!error) { marble_setup_policy_status = 0; return 0; }
    if (error != ENOENT) return error;
    static const struct { const char *name, *body; } preferences[] = {
        {"com.apple.purplebuddy", "<key>SetupDone</key><true/><key>SetupFinishedAllSteps</key><true/>"
            "<key>SetupVersion</key><integer>11</integer><key>daemonizeNeedsToRun</key><false/>"},
        {"com.apple.purplebuddy.notbackedup", "<key>ShouldRunDiagnosticsMiniBuddy</key><false/>"
            "<key>ShouldRunHSA2MiniBuddy</key><false/><key>ShouldRunKeychainSyncMiniBuddy</key><false/>"
            "<key>ShouldRunMesaMiniBuddy</key><false/><key>ShouldRunPaymentMiniBuddy</key><false/>"
            "<key>ShouldRunStandardMiniBuddy</key><false/><key>SilentICDPUpgrade</key><false/>"},
        {"com.apple.managedconfiguration.notbackedup",
            "<key>LockdownActivationIndicatesCloudConfigurationAvailable</key><false/>"}
    };
    for (unsigned i = 0; i < sizeof(preferences) / sizeof(preferences[0]); ++i) {
        error = writePreference(process, scratch, preferences[i].name, preferences[i].body);
        printf("MarbleSetup: %s status=%d\n", preferences[i].name, error);
        if (error) { marble_setup_policy_status = error; return error; }
    }
    error = copyout(marker, scratch, strlen(marker) + 1);
    if (!error) {
        struct open_args request = {.path = scratch, .flags = O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, .mode = 0600};
        int32_t result = 0;
        error = open(process, &request, &result);
        if (!error) {
            int fd = result;
            struct fsync_args sync = {.fd = fd};
            error = fsync(process, &sync, &result);
            struct close_args close_request = {.fd = fd};
            int close_error = sys_close(process, &close_request, &result);
            if (!error) error = close_error;
        }
    }
    marble_setup_policy_status = error;
    return error;
}
