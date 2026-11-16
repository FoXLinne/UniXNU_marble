// SPDX-License-Identifier: BSD-2-Clause-Patent
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#import <dlfcn.h>
#import <dispatch/dispatch.h>
#include <sys/attr.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <uuid/uuid.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>

@interface NSObject (MarbleSystemVolume)
+ (id)rootVolumeNode;
+ (id)prebootVolumeNode;
- (id)initWithURL:(NSURL *)url flags:(uint32_t)flags error:(NSError **)error;
- (BOOL)isSecuredSystemContent;
@end

static id systemNode, cryptexNode;
static id boardSystemNode(id cls, SEL selector) { (void)cls; (void)selector; return systemNode; }
static id boardCryptexNode(id cls, SEL selector) { (void)cls; (void)selector; return cryptexNode; }

static NSDictionary *volumeIdentity(const char *path)
{
    struct statfs fs;
    struct attrlist attributes = { .bitmapcount = ATTR_BIT_MAP_COUNT, .volattr = ATTR_VOL_INFO | ATTR_VOL_UUID };
    struct { uint32_t length; uuid_t uuid; } __attribute__((packed, aligned(4))) result = {0};
    if (statfs(path, &fs)) return @{@"error": @(errno)};
    NSMutableDictionary *identity = [NSMutableDictionary dictionaryWithDictionary:
        @{@"readonly": @((fs.f_flags & MNT_RDONLY) != 0), @"filesystem": @(fs.f_fstypename),
          @"source": @(fs.f_mntfromname), @"device": @((uint32_t)fs.f_fsid.val[0])}];
    if (getattrlist(path, &attributes, &result, sizeof(result), 0) || result.length < sizeof(result)) {
        identity[@"error"] = @(errno ?: ENOTSUP); return identity;
    }
    uuid_string_t uuid; uuid_unparse_upper(result.uuid, uuid);
    identity[@"uuid"] = @(uuid); return identity;
}

NSDictionary *marble_prepare_system_content_volume(void)
{
    static NSDictionary *report;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        NSMutableDictionary *state = [NSMutableDictionary dictionary];
        report = state;
        state[@"ready"] = @NO;
        NSDictionary *version = [NSDictionary dictionaryWithContentsOfFile:@"/System/Library/CoreServices/SystemVersion.plist"];
        if (![version[@"ProductBuildVersion"] isEqual:@"24B119"]) { state[@"error"] = @"OS build differs"; return; }
        NSDictionary *identity = volumeIdentity("/System");
        state[@"system_volume"] = identity;
        // This UUID is the immutable SystemOS input selected and authenticated
        // by MarbleSystemOS.c. Data has a different volume UUID. Both native
        // signature validation and LaunchServices redaction checks stay intact.
        if (![identity[@"uuid"] isEqual:@"9F1E9F09-B2A0-4844-98CB-E1025B99C4CE"] ||
            ![identity[@"readonly"] boolValue] || ![identity[@"filesystem"] isEqual:@"apfs"]) {
            state[@"error"] = @"Read-only authenticated SystemOS volume differs"; return;
        }
        NSDictionary *aliasIdentity = volumeIdentity("/private/preboot/Cryptexes/OS/System");
        state[@"cryptex_volume"] = aliasIdentity;
        BOOL sameVolume = [aliasIdentity[@"uuid"] isEqual:identity[@"uuid"]];
        if (!sameVolume && [aliasIdentity[@"filesystem"] isEqual:@"bindfs"] &&
            [aliasIdentity[@"source"] isEqual:@"/System"]) {
            struct stat original, alias;
            const char *suffix = "/Library/ExtensionKit/Extensions/GPUExtension.appex/com.apple.WebKit.GPU";
            NSString *originalPath = [@"/System" stringByAppendingString:@(suffix)];
            NSString *aliasPath = [@"/private/preboot/Cryptexes/OS/System" stringByAppendingString:@(suffix)];
            sameVolume = !stat(originalPath.fileSystemRepresentation, &original) &&
                !stat(aliasPath.fileSystemRepresentation, &alias) && S_ISREG(original.st_mode) &&
                original.st_ino == alias.st_ino && original.st_size == alias.st_size &&
                original.st_uid == 0 && alias.st_uid == 0;
            state[@"verified_bind_alias"] = @(sameVolume);
        }
        if (![aliasIdentity[@"readonly"] boolValue] || !sameVolume) {
            state[@"error"] = @"Cryptex view differs from SystemOS"; return;
        }
        if (!dlopen("/System/Library/Frameworks/CoreServices.framework/CoreServices", RTLD_NOW | RTLD_LOCAL)) {
            state[@"error"] = @"CoreServices unavailable"; return;
        }
        Class nodeClass = NSClassFromString(@"FSNode");
        Method root = class_getClassMethod(nodeClass, @selector(rootVolumeNode));
        Method preboot = class_getClassMethod(nodeClass, @selector(prebootVolumeNode));
        if (!root || !preboot || method_getNumberOfArguments(root) != 2 || method_getNumberOfArguments(preboot) != 2) {
            state[@"error"] = @"24B119 volume-node ABI differs"; return;
        }
        char rootType[8], prebootType[8];
        method_getReturnType(root, rootType, sizeof(rootType));
        method_getReturnType(preboot, prebootType, sizeof(prebootType));
        if (strcmp(rootType, "@") || strcmp(prebootType, "@")) { state[@"error"] = @"Volume-node return ABI differs"; return; }
        NSError *error = nil;
        systemNode = [[nodeClass alloc] initWithURL:[NSURL fileURLWithPath:@"/System" isDirectory:YES] flags:0 error:&error];
        cryptexNode = [[nodeClass alloc] initWithURL:[NSURL fileURLWithPath:@"/private/preboot/Cryptexes/OS/System" isDirectory:YES] flags:0 error:&error];
        if (!systemNode || !cryptexNode) { state[@"error"] = error.description ?: @"System volume nodes unavailable"; return; }
        // Native 24B119 FSNode caches two secured device numbers on first use.
        // The board has a bootstrap root and a Data-backed preboot directory,
        // with SystemOS mounted above them. Seed that native cache with its
        // authenticated read-only SystemOS views, then restore the class API.
        // The native isSecuredSystemContent implementation is never replaced.
        @synchronized (nodeClass) {
            IMP originalRoot = method_setImplementation(root, (IMP)boardSystemNode);
            IMP originalPreboot = method_setImplementation(preboot, (IMP)boardCryptexNode);
            @try { state[@"system_classified"] = @([systemNode isSecuredSystemContent]); }
            @finally {
                method_setImplementation(root, originalRoot);
                method_setImplementation(preboot, originalPreboot);
            }
            state[@"cryptex_classified"] = @([cryptexNode isSecuredSystemContent]);
        }
        state[@"ready"] = @([state[@"system_classified"] boolValue] && [state[@"cryptex_classified"] boolValue]);
        if (![state[@"ready"] boolValue]) state[@"error"] = @"Native secured-volume cache was initialized before board publication";
    });
    return report;
}

__attribute__((constructor)) static void prepareLaunchServicesVolume(void)
{
    if (strcmp(getprogname(), "lsd")) return;
    @autoreleasepool {
        @try {
            NSLog(@"MarbleSystemContent %@", marble_prepare_system_content_volume());
        } @catch (NSException *exception) {
            NSLog(@"MarbleSystemContent initialization: %@", exception);
        }
    }
}
