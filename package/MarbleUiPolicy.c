// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <sys/param.h>
#include <sys/proc_internal.h>
#include <sys/vnode_internal.h>
#include <sys/codesign.h>
#include <sys/systm.h>

extern void IOLog(const char *, ...);
int native_marble_ui_dyld_policy(proc_t, int, user_addr_t, int);

static bool phone_ui_executable(const char *path)
{
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
    if (error || call != 0x5a || !csproc_get_platform_binary(process) || !process->p_textvp) return error;
    char path[MAXPATHLEN];
    int length = sizeof(path);
    if (vn_getpath(process->p_textvp, path, &length) ||
        !phone_ui_executable(path)) return error;
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
    if (!executable || vn_getpath(executable, path, &length) ||
        !phone_ui_executable(path)) return NULL;
    IOLog("MarbleUI environment path=%s\n", path);
    return "DYLD_INSERT_LIBRARIES=/System/Library/Extensions/NativeAdrenoMetal.bundle/MarblePhoneUI.dylib";
}

