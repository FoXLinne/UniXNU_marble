// SPDX-License-Identifier: BSD-2-Clause-Patent
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#import <dlfcn.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
extern NSDictionary *marble_prepare_system_content_volume(void);

@interface NSObject (MarbleWebKitRegistration)
+ (id)defaultWorkspace;
- (BOOL)registerPlugin:(NSURL *)url;
- (NSString *)bundleIdentifier;
- (NSURL *)bundleURL;
- (BOOL)enumerateCryptexContentWithBlock:(void (^)(id record))block error:(NSError **)error;
- (BOOL)registerBuiltinStandaloneExtension:(id)record personaUniqueStrings:(id)personas operationUUID:(NSUUID *)operation requestContext:(id)context saveObserver:(id)observer registrationError:(NSError **)error;
- (id)initWithURL:(NSURL *)url flags:(uint32_t)flags error:(NSError **)error;
- (BOOL)isSecuredSystemContent;
+ (id)bundleForURL:(NSURL *)url error:(NSError **)error;
- (id)initWithAppExtensionBundle:(id)bundle inBundleIdentifier:(NSString *)owner dataContainers:(id)containers associatedPersonaUniqueStrings:(id)personas;
- (id)bundleRecordWithError:(NSError **)error;
- (BOOL)codeSigningInfoNotAuthoritative;
+ (id)entryForBundle:(id)bundle inContainer:(id)container forPersonas:(id)personas withError:(NSError **)error;
- (id)initWithBundleIdentifier:(NSString *)identifier error:(NSError **)error;
- (id)initWithBundleIdentifier:(NSString *)identifier requireValid:(BOOL)valid platform:(uint32_t)platform error:(NSError **)error;
- (NSURL *)URL;
- (id)extensionPointRecord;
- (NSString *)identifier;
- (uint32_t)platform;
- (BOOL)eligibleForRedaction;
- (uint8_t)_baseFlags;
+ (id)redactedAppexRecordWithUUID:(id)uuid node:(id)node bundleIdentifier:(NSString *)identifier platform:(uint32_t)platform error:(NSError **)error;
@end

static id extension_record(Class cls, NSString *identifier, NSError **error)
{
    // The short24B119 initializer sets requireValid=NO (0x186cf4d24).
    return [[cls alloc] initWithBundleIdentifier:identifier requireValid:YES platform:0 error:error];
}

static NSDictionary *record_info(id record)
{
    if (!record) return @{};
    @try {
        NSURL *url = [record URL];
        NSString *point = [[record extensionPointRecord] identifier];
        return @{@"path": url.path ?: @"", @"extension_point": point ?: @"", @"platform": @([record platform]), @"base_flags": @([record _baseFlags]), @"eligible_for_redaction": @([record eligibleForRedaction])};
    } @catch (NSException *exception) {
        return @{@"exception": exception.reason ?: exception.name};
    }
}
static BOOL record_matches(NSDictionary *info, NSArray *bundle)
{
    NSString *suffix = [NSString stringWithFormat:@"/%@.appex", bundle[0]];
    return [info[@"path"] hasSuffix:suffix] && [info[@"extension_point"] isEqual:bundle[2]];
}

int marble_register_webkit_extensions(void)
{
    @autoreleasepool {
        NSMutableDictionary *report = [NSMutableDictionary dictionaryWithDictionary:
            @{@"build": @"24B119", @"uid": @(getuid()), @"started": NSDate.date.description,
              @"operation": @"validate and register stock WebKit ExtensionKit bundles", @"source": @MARBLE_DEBUG_SOURCE}];
        NSMutableArray *results = [NSMutableArray array];
        report[@"extensions"] = results;
        BOOL complete = YES;
        @try {
            NSError *error = nil;
            NSDictionary *version = [NSDictionary dictionaryWithContentsOfURL:
                [NSURL fileURLWithPath:@"/System/Library/CoreServices/SystemVersion.plist"] error:&error];
            if (![version[@"ProductBuildVersion"] isEqual:@"24B119"])
                @throw [NSException exceptionWithName:@"MarbleOSBuild" reason:version.description ?: error.description userInfo:nil];
            NSDictionary *systemContent = marble_prepare_system_content_volume();
            report[@"system_content_volume"] = systemContent;
            if (![systemContent[@"ready"] boolValue])
                @throw [NSException exceptionWithName:@"MarbleSystemContent" reason:systemContent.description userInfo:nil];
            if (!dlopen("/System/Library/Frameworks/CoreServices.framework/CoreServices", RTLD_NOW | RTLD_LOCAL))
                @throw [NSException exceptionWithName:@"MarbleCoreServices" reason:@"CoreServices load failed" userInfo:nil];
            Class workspaceClass = NSClassFromString(@"LSApplicationWorkspace");
            Class recordClass = NSClassFromString(@"LSApplicationExtensionRecord");
            if (![workspaceClass respondsToSelector:@selector(defaultWorkspace)] ||
                ![recordClass instancesRespondToSelector:@selector(initWithBundleIdentifier:requireValid:platform:error:)])
                @throw [NSException exceptionWithName:@"MarbleLaunchServices" reason:@"24B119 registration API unavailable" userInfo:nil];
            id workspace = [workspaceClass defaultWorkspace];
            if (![workspace respondsToSelector:@selector(registerBuiltinStandaloneExtension:personaUniqueStrings:operationUUID:requestContext:saveObserver:registrationError:)])
                @throw [NSException exceptionWithName:@"MarbleLaunchServices" reason:@"24B119 builtin standalone extension registrar unavailable" userInfo:nil];
            if (!dlopen("/System/Library/PrivateFrameworks/InstalledContentLibrary.framework/InstalledContentLibrary", RTLD_NOW | RTLD_LOCAL))
                @throw [NSException exceptionWithName:@"MarbleInstalledContent" reason:@"InstalledContentLibrary load failed" userInfo:nil];
            id installed = [NSClassFromString(@"ICLWorkspace") defaultWorkspace];
            NSMutableDictionary *stockRecords = [NSMutableDictionary dictionary];
            __block BOOL inventoryQueried = NO;
            report[@"cryptex_inventory_queried"] = @NO;
            void (^loadInventory)(void) = ^{
                if (inventoryQueried) return;
                inventoryQueried = YES;
                report[@"cryptex_inventory_queried"] = @YES;
                NSError *inventoryError = nil;
                BOOL inventoryOK = [installed enumerateCryptexContentWithBlock:^(id record) {
                    if (![record respondsToSelector:@selector(bundleIdentifier)] || ![record respondsToSelector:@selector(bundleURL)]) return;
                    NSString *identifier = [record bundleIdentifier];
                    if ([identifier hasPrefix:@"com.apple.WebKit."]) {
                        @synchronized (stockRecords) { stockRecords[identifier] = record; }
                    }
                } error:&inventoryError];
                report[@"cryptex_inventory_success"] = @(inventoryOK);
                report[@"cryptex_inventory_error"] = inventoryError.description ?: @"none";
                report[@"webkit_inventory_identifiers"] = stockRecords.allKeys;
            };
            NSArray *bundles = @[
                @[@"GPUExtension", @"com.apple.WebKit.GPU", @"com.apple.web-browser-engine.rendering"],
                @[@"NetworkingExtension", @"com.apple.WebKit.Networking", @"com.apple.web-browser-engine.networking"],
                @[@"WebContentExtension", @"com.apple.WebKit.WebContent", @"com.apple.web-browser-engine.webcontent"],
                @[@"WebContentEnhancedSecurityExtension", @"com.apple.WebKit.WebContent.EnhancedSecurity", @"com.apple.web-browser-engine.webcontent"],
                @[@"WebContentCaptivePortalExtension", @"com.apple.WebKit.WebContent.CaptivePortal", @"com.apple.web-browser-engine.webcontent"]];
            for (NSArray *bundle in bundles) {
                NSString *path = [NSString stringWithFormat:@"/System/Library/ExtensionKit/Extensions/%@.appex", bundle[0]];
                NSURL *url = [NSURL fileURLWithPath:path isDirectory:YES];
                NSMutableDictionary *item = [NSMutableDictionary dictionaryWithDictionary:@{@"identifier": bundle[1], @"path": path}];
                [results addObject:item];
                NSError *queryError = nil;
                id lax = [[recordClass alloc] initWithBundleIdentifier:bundle[1] error:NULL];
                item[@"lax_record_found"] = @(lax != nil);
                item[@"before_record"] = record_info(lax);
                id before = extension_record(recordClass, bundle[1], &queryError);
                item[@"before_valid"] = @(before != nil);
                NSError *remoteError = nil;
                id remoteBefore = [recordClass redactedAppexRecordWithUUID:nil node:nil bundleIdentifier:bundle[1] platform:0 error:&remoteError];
                item[@"before_remote_visible"] = @(remoteBefore != nil);
                item[@"before_remote_error"] = remoteError.description ?: @"none";
                if (before && record_matches(record_info(before), bundle) && [before eligibleForRedaction] && remoteBefore) {
                    item[@"after_registered"] = @YES;
                    continue;
                }
                item[@"before_error"] = queryError.description ?: @"record path or extension point differs";
                if (!before && queryError && !([queryError.domain isEqual:NSOSStatusErrorDomain] && queryError.code == -10814)) {
                    item[@"after_registered"] = @NO; item[@"error"] = @"Lookup failed outside missing-extension status"; complete = NO; continue;
                }
                NSDictionary *info = [NSDictionary dictionaryWithContentsOfURL:[url URLByAppendingPathComponent:@"Info.plist"] error:&error];
                NSString *executable = info[@"CFBundleExecutable"];
                struct stat attributes;
                if (![info[@"CFBundleIdentifier"] isEqual:bundle[1]] ||
                    ![info[@"EXAppExtensionAttributes"][@"EXExtensionPointIdentifier"] isEqual:bundle[2]] ||
                    ![executable isKindOfClass:NSString.class] || ![executable isEqual:bundle[1]] ||
                    stat([[path stringByAppendingPathComponent:executable] fileSystemRepresentation], &attributes) || !S_ISREG(attributes.st_mode)) {
                    item[@"error"] = @"Stock 24B119 extension identity or executable differs";
                    item[@"after_registered"] = @NO; complete = NO; continue;
                }
                loadInventory();
                id stock = stockRecords[bundle[1]];
                if (!stock) {
                    // Bootstrapped Cryptex inventory can be empty. Use the same
                    // 24B119 installation gatherer to read the stock bundle and
                    // its native signing information, without manufacturing an
                    // ICL record or changing the LaunchServices query policy.
                    NSError *gatherError = nil;
                    // The board publishes Cryptex through a bind mount. Read
                    // the verified original system view so native child-path
                    // validation uses the same namespace as its executable.
                    NSURL *sourceURL = url;
                    item[@"gather_source_url"] = sourceURL.path;
                    char resolved[PATH_MAX];
                    if (realpath(sourceURL.fileSystemRepresentation, resolved)) item[@"bundle_realpath"] = @(resolved);
                    else item[@"bundle_realpath_errno"] = @(errno);
                    NSURL *executableURL = [sourceURL URLByAppendingPathComponent:executable];
                    if (realpath(executableURL.fileSystemRepresentation, resolved)) item[@"executable_realpath"] = @(resolved);
                    else item[@"executable_realpath_errno"] = @(errno);
                    id executableBundle = [NSClassFromString(@"MIExtensionKitBundle") bundleForURL:sourceURL error:&gatherError];
                    item[@"native_bundle_class"] = executableBundle ? NSStringFromClass([executableBundle class]) : @"missing";
                    if (executableBundle) {
                        // Native installation discovery creates or reuses the
                        // extension's own persona data container before it
                        // gathers the signed ICL record (24B1190x1ab95b9b8).
                        stock = [NSClassFromString(@"MILaunchServicesDatabaseGatherer")
                            entryForBundle:executableBundle inContainer:nil forPersonas:nil withError:&gatherError];
                    }
                    item[@"record_source"] = @"native MIInstalledInfoGatherer stock filesystem discovery";
                    item[@"gather_error"] = gatherError.description ?: @"none";
                    item[@"gathered_identifier"] = [stock bundleIdentifier] ?: @"missing";
                    item[@"signing_info_authoritative"] = @(stock && ![stock codeSigningInfoNotAuthoritative]);
                    if (stock && ([stock codeSigningInfoNotAuthoritative] || ![[stock bundleIdentifier] isEqual:bundle[1]])) {
                        item[@"error"] = @"Native installation gathering lacks authoritative stock signing identity";
                        item[@"after_registered"] = @NO; complete = NO; continue;
                    }
                }
                NSURL *stockURL = [stock bundleURL];
                NSString *stockSuffix = [NSString stringWithFormat:@"/%@.appex", bundle[0]];
                item[@"installed_content_url"] = stockURL.path ?: @"missing";
                if (!stock || ![stockURL.path hasSuffix:stockSuffix]) {
                    item[@"error"] = @"Authoritative installed cryptex record missing or mismatched";
                    item[@"after_registered"] = @NO; complete = NO; continue;
                }
                NSError *nodeError = nil;
                id node = [[NSClassFromString(@"FSNode") alloc] initWithURL:stockURL flags:0 error:&nodeError];
                item[@"secured_system_content"] = @(node && [node isSecuredSystemContent]);
                NSError *registrationError = nil;
                // Native 24B119 builtin registrar validates FSNode secured-system
                // content and consumes authoritative ICL records, retaining
                // their signing and persona metadata (0x186d81170).
                BOOL registered = [workspace registerBuiltinStandaloneExtension:stock personaUniqueStrings:nil
                    operationUUID:NSUUID.UUID requestContext:nil saveObserver:nil registrationError:&registrationError];
                item[@"register_return"] = @(registered);
                item[@"registration_error"] = registrationError.description ?: @"none";
                id after = nil;
                for (unsigned attempt = 0; attempt < 3; attempt++) {
                    if (attempt) sleep(1);
                    queryError = nil;
                    after = extension_record(recordClass, bundle[1], &queryError);
                    if (after && record_matches(record_info(after), bundle) && [after eligibleForRedaction]) break;
                }
                NSDictionary *afterInfo = record_info(after);
                item[@"after_record"] = afterInfo;
                NSError *remoteAfterError = nil;
                id remoteAfter = [recordClass redactedAppexRecordWithUUID:nil node:nil bundleIdentifier:bundle[1] platform:0 error:&remoteAfterError];
                item[@"after_remote_visible"] = @(remoteAfter != nil);
                item[@"after_remote_error"] = remoteAfterError.description ?: @"none";
                item[@"after_registered"] = @(after && record_matches(afterInfo, bundle) && [after eligibleForRedaction] && remoteAfter);
                if (![item[@"after_registered"] boolValue]) { item[@"after_error"] = queryError.description ?: @"missing or mismatched record"; complete = NO; }
                NSLog(@"MarbleWebKitRegistration %@", item);
            }
        } @catch (NSException *exception) {
            report[@"exception"] = [NSString stringWithFormat:@"%@: %@", exception.name, exception.reason];
            complete = NO;
        }
        report[@"complete"] = @(complete); report[@"finished"] = NSDate.date.description;
        NSData *data = [NSJSONSerialization dataWithJSONObject:report options:NSJSONWritingPrettyPrinted error:NULL];
        NSString *directory = @"/var/mobile/Library/Logs";
        [NSFileManager.defaultManager createDirectoryAtPath:directory withIntermediateDirectories:YES attributes:nil error:NULL];
        NSString *destination = [directory stringByAppendingPathComponent:@"UniXNU-webkit-registration.json"];
        if (![data writeToFile:destination options:NSDataWritingAtomic error:NULL]) return EIO;
        chmod(destination.fileSystemRepresentation, 0600);
        NSLog(@"MarbleWebKitRegistration complete=%d report=%@", complete, destination);
        return complete ? 0 : EIO;
    }
}
