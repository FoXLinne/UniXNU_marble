// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <sys/param.h>
#include <sys/proc.h>
#include <sys/vnode_internal.h>
#include <sys/codesign.h>
#include <sys/systm.h>
#include <pexpert/pexpert.h>

extern void IOLog(const char *, ...);

/* 22H384 UI failure paths and cellular idle state. The validated-page fixup
 * path checks every original instruction before applying these entries. */
const char *native_marble_cellular_text_fixup(unsigned index);
const char *native_marble_cellular_text_fixup(unsigned index)
{
    static const char *const entries[] = {
        /* LCSCaptureApplicationMonitor metadata changes: its own bundle ID is
         * already inserted. Skip an absent optional container ID. Borrow the
         * nonnull getter's +0 value until the set retains it, replacing its
         * matched retain/release pair while preserving the notification flow. */
        "dyld_shared_cache_arm64e.58,3a2da08,86e8cd94f80300aae00315aae20318aa3a4a0094cde8cd94,c00000b4f80300aae00315aae20318aa3a4a00941f2003d5",
        /* TUCallCenter currentCallCount / currentCalls. */
        "dyld_shared_cache_arm64e.09,4753e60,7f2303d5f44fbea9fd7b01a9,7f2303d5000080d2ff0f5fd6",
        "dyld_shared_cache_arm64e.09,475c384,7f2303d5f44fbea9fd7b01a9,7f2303d5000080d2ff0f5fd6",
        /* TUCallServicesInterface currentCalls / initial state / refresh. */
        "dyld_shared_cache_arm64e.09,4753f60,7f2303d5f44fbea9fd7b01a9,7f2303d5000080d2ff0f5fd6",
        "dyld_shared_cache_arm64e.09,4753fb0,7f2303d5ffc300d1,7f2303d5ff0f5fd6",
        "dyld_shared_cache_arm64e.09,4764c74,7f2303d5ff4302d1,7f2303d5ff0f5fd6",
        /* SBTelephonyManager inCall / inCallUsingSpeakerOrReceiver. */
        "dyld_shared_cache_arm64e.29,1167f0,7f2303d5f657bda9f44f01a9,7f2303d5000080d2ff0f5fd6",
        "dyld_shared_cache_arm64e.29,1191f0,7f2303d5f657bda9f44f01a9,7f2303d5000080d2ff0f5fd6",
    };
    return index < sizeof(entries) / sizeof(entries[0]) ? entries[index] : NULL;
}

int native_marble_ui_dyld_policy(proc_t, int, user_addr_t, int);

static bool appsync_executable(const char *path)
{
    unsigned enabled = 0;
    PE_parse_boot_argn("appsync", &enabled, sizeof(enabled));
    return enabled == 1 &&
        (!strcmp(path, "/usr/libexec/installd") ||
         !strcmp(path, "/private/var/ngpu/appsync/libexec/installd") ||
         !strcmp(path, "/System/Library/CoreServices/SpringBoard.app/SpringBoard") ||
         !strcmp(path, "/System/Library/PrivateFrameworks/MobileInstallation.framework/XPCServices/com.apple.MobileInstallationHelperService.xpc/com.apple.MobileInstallationHelperService"));
}

static bool phone_ui_executable(const char *path)
{
    // backboardd creates the display modes before loading the Metal addon.
    // Load the same geometry adapter before its first FrontBoard configuration.
    if (!strcmp(path, "/usr/libexec/backboardd")) return true;
    if (!strncmp(path, "/Applications/", sizeof("/Applications/") - 1) ||
        !strncmp(path, "/private/var/staged_system_apps/", sizeof("/private/var/staged_system_apps/") - 1) ||
        !strncmp(path, "/System/Library/CoreServices/", sizeof("/System/Library/CoreServices/") - 1) ||
        !strncmp(path, "/System/Library/Frameworks/", sizeof("/System/Library/Frameworks/") - 1) ||
        !strncmp(path, "/System/Library/PrivateFrameworks/", sizeof("/System/Library/PrivateFrameworks/") - 1) ||
        !strncmp(path, "/System/Library/ExtensionKit/Extensions/", sizeof("/System/Library/ExtensionKit/Extensions/") - 1)) {
        static const char *bundles[] = {".app/", ".appex/", ".xpc/"};
        for (unsigned i = 0; i < sizeof(bundles) / sizeof(bundles[0]); ++i) {
            const char *bundle = path;
            while ((bundle = strnstr(bundle, bundles[i], MAXPATHLEN))) {
                const char *executable = bundle + strlen(bundles[i]);
                if (executable[0] && !strchr(executable, '/')) return true;
                bundle = executable;
            }
        }
    }
    return !strcmp(path, "/Applications/Spotlight.app/Spotlight") ||
           !strcmp(path, "/Applications/InputUI.app/InputUI") ||
           !strcmp(path, "/Applications/Preferences.app/Preferences") ||
           !strcmp(path, "/System/Library/CoreServices/SpringBoard.app/SpringBoard") ||
           !strcmp(path, "/Applications/PosterBoard.app/PosterBoard") ||
           !strcmp(path, "/Applications/Photos.app/Photos") ||
           !strcmp(path, "/private/var/staged_system_apps/Photos.app/Photos") ||
           !strcmp(path, "/Applications/MobileSlideShow.app/MobileSlideShow");
}

int native_marble_ui_dyld_policy(proc_t process, int call, user_addr_t argument, int error)
{
    if (error || call != 0x5a || !csproc_get_platform_binary(process)) return error;
    vnode_t executable = proc_getexecutablevnode(process);
    if (!executable) return error;
    char path[MAXPATHLEN];
    int length = sizeof(path);
    int path_error = vn_getpath(executable, path, &length);
    vnode_put(executable);
    if (path_error || (!phone_ui_executable(path) && !appsync_executable(path))) return error;
    struct {uint64_t input; user_addr_t output;} request;
    uint64_t flags = 0;
    if (copyin(argument, &request, sizeof(request)) || copyin(request.output, &flags, sizeof(flags))) return error;
    uint64_t previous = flags;
    // Apple dyld's ALLOW_PATH_VARS permits the board's signed layout library in DYLD_INSERT_LIBRARIES.
    // The original AMFI result and code-signature/library validation remain in force.
    flags |= 1u << 1;
    error = copyout(&flags, request.output, sizeof(flags));
    IOLog("MarbleUI dyld path=%s flags=%llx->%llx error=%d\n", path, previous, flags, error);
    return error;
}

const char *native_marble_ui_environment(vnode_t executable);
const char *native_marble_ui_environment(vnode_t executable)
{
    char path[MAXPATHLEN];
    int length = sizeof(path);
    if (!executable || vn_getpath(executable, path, &length)) return NULL;
    bool layout = phone_ui_executable(path), appsync = appsync_executable(path);
    if (!layout && !appsync) return NULL;
    IOLog("MarbleUI environment path=%s\n", path);
    if (appsync) {
        return layout ? "DYLD_INSERT_LIBRARIES=/System/Library/Extensions/NativeAdrenoMetal.bundle/MarblePhoneUI.dylib:/usr/lib/UniXNUAppSync.dylib" :
            "DYLD_INSERT_LIBRARIES=/usr/lib/UniXNUAppSync.dylib";
    }
    return "DYLD_INSERT_LIBRARIES=/System/Library/Extensions/NativeAdrenoMetal.bundle/MarblePhoneUI.dylib";
}

