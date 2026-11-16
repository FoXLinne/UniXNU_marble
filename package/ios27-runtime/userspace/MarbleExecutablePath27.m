// SPDX-License-Identifier: BSD-2-Clause-Patent
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
extern int proc_pidpath(int, void *, uint32_t);

static NSString *(*marble_original_executable_path)(id, SEL);

/* 24B119 driverkitd Config calls [NSBundle.mainBundle executablePath]. The
 * standalone daemon has no application bundle directory. Preserve a valid
 * Foundation result; recover a missing path from the kernel executable vnode.
 */
static NSString *marble_executable_path(NSBundle *bundle, SEL selector)
{
    NSString *result = marble_original_executable_path(bundle, selector);
    if (result || bundle != [NSBundle mainBundle]) return result;
    char path[4096];
    int length = proc_pidpath(getpid(), path, sizeof(path));
    if (length <= 0 || strcmp(path, "/usr/libexec/driverkitd")) return nil;
    struct stat attributes;
    if (stat(path, &attributes) || !S_ISREG(attributes.st_mode)) return nil;
    fprintf(stderr, "MarbleExecutablePath: driverkitd executable path %s\n", path);
    return [NSString stringWithUTF8String:path];
}

__attribute__((constructor)) static void marble_install_executable_path(void)
{
    Method method = class_getInstanceMethod([NSBundle class], @selector(executablePath));
    if (!method) return;
    marble_original_executable_path = (void *)method_getImplementation(method);
    method_setImplementation(method, (IMP)marble_executable_path);
}
