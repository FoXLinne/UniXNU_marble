// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <CoreFoundation/CoreFoundation.h>
#include <sys/types.h>
extern int proc_pidpath(int, void *, uint32_t);
#define PROC_PIDPATHINFO_MAXSIZE 4096
#include <sys/stat.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>

/* Standalone launch daemons have no application bundle directory. Preserve
 * CoreFoundation's result when present; recover the current executable URL
 * from the kernel's executable vnode when bundle discovery returns NULL.
 * The fallback is restricted to driverkitd and its exact executable path.
 */
static CFURLRef marble_copy_executable_url(CFBundleRef bundle)
{
    CFURLRef result = CFBundleCopyExecutableURL(bundle);
    if (result || !bundle) return result;
    CFURLRef directory = CFBundleCopyBundleURL(bundle);
    UInt8 directory_path[4096];
    Boolean own_directory = directory && CFURLGetFileSystemRepresentation(directory, true,
        directory_path, sizeof(directory_path)) &&
        (!strcmp((const char *)directory_path, "/usr/libexec") ||
         !strcmp((const char *)directory_path, "/usr/libexec/driverkitd"));
    if (directory) CFRelease(directory);
    if (!own_directory) return NULL;
    char path[PROC_PIDPATHINFO_MAXSIZE];
    int length = proc_pidpath(getpid(), path, sizeof(path));
    if (length <= 0 || strcmp(path, "/usr/libexec/driverkitd")) return NULL;
    struct stat attributes;
    if (stat(path, &attributes) || !S_ISREG(attributes.st_mode)) return NULL;
    result = CFURLCreateFromFileSystemRepresentation(kCFAllocatorDefault,
        (const UInt8 *)path, (CFIndex)strlen(path), false);
    if (result) fprintf(stderr, "MarbleExecutablePath: driverkitd executable URL %s\n", path);
    return result;
}

__attribute__((used, section("__DATA,__interpose")))
static const struct { const void *replacement; const void *replacee; } marble_path_interpose = {
    (const void *)marble_copy_executable_url, (const void *)CFBundleCopyExecutableURL
};
