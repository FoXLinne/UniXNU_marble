// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <sys/param.h>
#include <sys/proc.h>
#include <sys/vnode_internal.h>
#include <sys/codesign.h>
#include <sys/systm.h>
#include <pexpert/pexpert.h>

extern void IOLog(const char *, ...);

/* 24B119 (iOS 27.2) UI failure paths and cellular idle state. Every offset and
 * byte string below was re-derived from the 24B119 SystemOS cryptex
 * dyld_shared_cache_arm64e (UUID 555BFDAA-D930-31AC-A915-D472C273C527); see
 * profiles/runtime/ios27-24B119/fixups.json for per-site image, symbol and
 * subcache qualification. No 22H384 offset is reused: identical instruction
 * encodings appear in different subcache files at different offsets. The
 * validated-page fixup path checks every original instruction before applying
 * these entries. */
const char *native_marble_cellular_text_fixup(unsigned index);
const char *native_marble_cellular_text_fixup(unsigned index)
{
    static const char *const entries[] = {
        /* 24B119 PowerlogCore stores persistent monotonic dates beyond
         * INT32_MAX. Keep bucketNSDate, bucketTimeStampForDate and their
         * callers in signed 64-bit seconds. The 32-bit conversions saturate
         * hourly AppRunTime/UsageTime buckets at 2038 while UI queries use
         * doubles. These page fixups preserve the existing date origin and
         * records. */
        "dyld_shared_cache_arm64e.36,27660fc,1800781e9c3657950803621e0039691e1b00781e,1800789e9c3657950803629e0039691e1b00789e",
        "dyld_shared_cache_arm64e.36,276625c,b5021b0b,b5021b8b",
        "dyld_shared_cache_arm64e.36,2766300,6003621e7fa05295ee355795f80300aa20b347f9a002621e,6003629e7fa05295ee355795f80300aa20b347f9a002629e",
        "dyld_shared_cache_arm64e.36,27663e4,a802621e,a802629e",
        "dyld_shared_cache_arm64e.36,276647c,6003621e,6003629e",
        "dyld_shared_cache_arm64e.36,2766f9c,0800781e080dd31a087d131b0001621e,0800789e080dd39a087d139b0001629e",
        "dyld_shared_cache_arm64e.36,276542c,ea02621e0803621e,ea02629e0803629e",
        "dyld_shared_cache_arm64e.36,2768160,0900621e,0900629e",
        /* LCSCaptureApplicationMonitor metadata changes: its own bundle ID is
         * already inserted. The else-branch retains the optional container
         * dictionary's allKeys and hands it to _updateTCCMonitorForBundle-
         * Identifiers:. When the container is absent the getter yields nil;
         * skip the matched retain/update/release window, borrowing the +0
         * value and landing on the same continuation as the tbnz skip. */
        "dyld_shared_cache_arm64e.59,76ae0e4,c7be1894f80300aae00316aae20318aac75b1894f6be1894,c00000b4f80300aae00316aae20318aac75b18941f2003d5",
        /* TUCallCenter currentCallCount / currentCalls. */
        "dyld_shared_cache_arm64e.07,6999300,7f2303d5f44fbea9fd7b01a9,7f2303d5000080d2ff0f5fd6",
        "dyld_shared_cache_arm64e.07,69c7bc0,7f2303d5f44fbea9fd7b01a9,7f2303d5000080d2ff0f5fd6",
        /* TUCallServicesInterface currentCalls / initial state / refresh. */
        "dyld_shared_cache_arm64e.07,698d7d4,7f2303d5f44fbea9fd7b01a9,7f2303d5000080d2ff0f5fd6",
        "dyld_shared_cache_arm64e.07,6993734,7f2303d5ffc300d1,7f2303d5ff0f5fd6",
        "dyld_shared_cache_arm64e.07,69b062c,7f2303d5ff4302d1,7f2303d5ff0f5fd6",
        /* SBTelephonyManager inCall / inCallUsingSpeakerOrReceiver. */
        "dyld_shared_cache_arm64e.36,6da0468,7f2303d5f657bda9f44f01a9,7f2303d5000080d2ff0f5fd6",
        "dyld_shared_cache_arm64e.36,6d9c590,7f2303d5f657bda9f44f01a9,7f2303d5000080d2ff0f5fd6",
    };
    const unsigned count = sizeof(entries) / sizeof(entries[0]);
    if (index < count) return entries[index];
    unsigned audio = 0, haptics = 0;
    PE_parse_boot_argn("marble-audio", &audio, sizeof(audio));
    PE_parse_boot_argn("marble-haptics", &haptics, sizeof(haptics));
    /* 24B119 carries one private 24 MHz double per audio module. CoreAudio's
     * copy feeds AudioGetHostClockFrequency and every [x*,#0x2f8] host-time
     * reader; AudioDSPGraph::Graph::configure turns its copy into DSP queue
     * periods; AURemoteIO::PerformIO and getHALThreadPriority derive IO frames
     * and the real-time thread period from theirs. All three become the
     * board's 19.2 MHz counter so HAL timestamps and DSP queue periods share
     * the same clock. Calendar time is independent. */
    if (audio >= 2 || haptics >= 2) {
        static const char *const clock_entries[] = {
            "dyld_shared_cache_arm64e.38,415e2f8,0000000060e37641,00000000804f7241",
            "dyld_shared_cache_arm64e.28,70f02e8,0000000060e37641,00000000804f7241",
            "dyld_shared_cache_arm64e.36,67d3fe8,0000000060e37641,00000000804f7241",
        };
        unsigned slot = index - count;
        if (slot < sizeof(clock_entries) / sizeof(clock_entries[0])) return clock_entries[slot];
    }
    return NULL;
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
    if (!strcmp(path, "/usr/libexec/driverkitd") ||
        !strcmp(path, "/usr/libexec/audiomxd")) return true;
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

static bool launch_services_executable(const char *path)
{
    return !strcmp(path, "/usr/libexec/lsd");
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
    if (path_error || (!phone_ui_executable(path) && !appsync_executable(path) && !launch_services_executable(path))) return error;
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
    if (launch_services_executable(path))
        return "DYLD_INSERT_LIBRARIES=/System/Library/Extensions/NativeAdrenoMetal.bundle/MarbleSystemContent.dylib";
    if (!strcmp(path, "/usr/libexec/audiomxd"))
        return "DYLD_INSERT_LIBRARIES=/System/Library/Extensions/NativeAdrenoMetal.bundle/MarbleAudioRetry.dylib";
    if (!strcmp(path, "/usr/libexec/driverkitd"))
        return "DYLD_INSERT_LIBRARIES=/System/Library/Extensions/NativeAdrenoMetal.bundle/MarbleExecutablePath.dylib";
    bool layout = phone_ui_executable(path), appsync = appsync_executable(path);
    if (!layout && !appsync) return NULL;
    IOLog("MarbleUI environment path=%s\n", path);
    if (appsync) {
        return layout ? "DYLD_INSERT_LIBRARIES=/System/Library/Extensions/NativeAdrenoMetal.bundle/MarblePhoneUI.dylib:/usr/lib/UniXNUAppSync.dylib" :
            "DYLD_INSERT_LIBRARIES=/usr/lib/UniXNUAppSync.dylib";
    }
    return "DYLD_INSERT_LIBRARIES=/System/Library/Extensions/NativeAdrenoMetal.bundle/MarblePhoneUI.dylib";
}
