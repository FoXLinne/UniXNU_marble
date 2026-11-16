// SPDX-License-Identifier: BSD-2-Clause-Patent
#import <Foundation/Foundation.h>
#import <OSLog/OSLog.h>
#import <objc/message.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <dispatch/dispatch.h>
#include <stdbool.h>
#include <limits.h>
#include "ActivityStreamSPI.h"

static int marble_copy_activity_log(void **, size_t *);

// A requested, bounded read of the system log. No logging configuration changes.
int marble_copy_unified_log(void **bytes, size_t *length)
{
    if (!bytes || !length) return EINVAL;
    *bytes = NULL; *length = 0;
    @autoreleasepool {
        NSMutableString *text = [NSMutableString string];
        int status = 0;
        @try {
            NSError *error = nil;
            OSLogStore *store = nil;
            Class cls = NSClassFromString(@"OSLogStore");
            SEL local = NSSelectorFromString(@"localStoreAndReturnError:");
            if ([cls respondsToSelector:local])
                store = ((id (*)(id, SEL, NSError **))objc_msgSend)(cls, local, &error);
            if (!store) store = [OSLogStore storeWithScope:(OSLogStoreScope)0 error:&error];
            if (!store) return marble_copy_activity_log(bytes, length);
            else {
                NSArray *processes = @[@"Alook", @"runningboardd", @"wifid", @"audiomxd",
                    @"SpringBoard", @"backboardd", @"trustd", @"lsd", @"pkd", @"nehelper", @"marble-debug"];
                NSPredicate *filter = [NSPredicate predicateWithFormat:
                    @"process IN %@ OR process BEGINSWITH %@ OR subsystem BEGINSWITH %@",
                    processes, @"com.apple.WebKit", @"com.apple.WebKit"];
                OSLogEnumerator *entries = [store entriesEnumeratorWithOptions:OSLogEnumeratorReverse
                    position:nil predicate:filter error:&error];
                if (!entries) {
                    [text appendFormat:@"System log enumeration: %@\n", error ?: @"unavailable"];
                    status = EIO;
                } else {
                    NSDate *latest = nil;
                    unsigned count = 0;
                    for (OSLogEntry *entry in entries) {
                        if (![entry isKindOfClass:OSLogEntryLog.class]) continue;
                        OSLogEntryLog *line = (OSLogEntryLog *)entry;
                        if (!latest) latest = line.date;
                        if ([latest timeIntervalSinceDate:line.date] > 900) break;
                        [text appendFormat:@"%@ %@[%d] %@/%@: %@\n", line.date, line.process,
                            line.processIdentifier, line.subsystem, line.category, line.composedMessage];
                        if (++count >= 10000 || text.length >= (2u << 20)) break;
                    }
                    [text appendFormat:@"\nEntries: %u; newest first; window at most 900 seconds\n", count];
                }
            }
        } @catch (NSException *exception) {
            [text appendFormat:@"System log exception: %@: %@\n", exception.name, exception.reason];
            status = EIO;
        }
        NSData *data = [text dataUsingEncoding:NSUTF8StringEncoding];
        size_t size = MIN(data.length, (size_t)(4u << 20));
        void *copy = malloc(size + 1);
        if (!copy) return ENOMEM;
        memcpy(copy, data.bytes, size); ((char *)copy)[size] = 0;
        *bytes = copy; *length = size;
        return status;
    }
}

/* iOS excludes OSLogStoreSystem. The system diagnostic stream is the
 * LoggingSupport interface used by Apple's debugserver. It is opened for a
 * requested bounded read, and cancelled when that read completes. */
static int marble_copy_activity_log(void **bytes, size_t *length)
{
    void *library = dlopen("/System/Library/PrivateFrameworks/LoggingSupport.framework/LoggingSupport", RTLD_LAZY | RTLD_LOCAL);
    os_activity_stream_for_pid_t create = library ? (os_activity_stream_for_pid_t)dlsym(library, "os_activity_stream_for_pid") : NULL;
    os_activity_stream_resume_t resume = library ? (os_activity_stream_resume_t)dlsym(library, "os_activity_stream_resume") : NULL;
    os_activity_stream_cancel_t cancel = library ? (os_activity_stream_cancel_t)dlsym(library, "os_activity_stream_cancel") : NULL;
    os_activity_stream_set_event_handler_t events = library ? (os_activity_stream_set_event_handler_t)dlsym(library, "os_activity_stream_set_event_handler") : NULL;
    os_log_copy_formatted_message_t format = library ? (os_log_copy_formatted_message_t)dlsym(library, "os_log_copy_formatted_message") : NULL;
    NSMutableString *text = [NSMutableString stringWithString:@"Transport: iOS diagnostic activity stream; historical and live; 12-second bound\n"];
    __block unsigned count = 0;
    __block int failure = 0;
    __block bool accepting = true;
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    os_activity_stream_t stream = NULL;
    if (create && resume && cancel && format && events) {
        stream = create(-1, OS_ACTIVITY_STREAM_HISTORICAL | OS_ACTIVITY_STREAM_INFO | OS_ACTIVITY_STREAM_NO_SENSITIVE,
            ^bool(os_activity_stream_entry_t entry, int error) {
                @autoreleasepool {
                    @synchronized (text) {
                        if (!accepting) return false;
                        if (error) { failure = error; dispatch_semaphore_signal(done); return false; }
                        if (!entry || entry->type != OS_ACTIVITY_STREAM_TYPE_LOG_MESSAGE || !entry->proc_imagepath) return true;
                        const char *name = strrchr(entry->proc_imagepath, '/'); name = name ? name + 1 : entry->proc_imagepath;
                        if (strcmp(name, "Alook") && strncmp(name, "com.apple.WebKit", 16) &&
                            strcmp(name, "wifid") && strcmp(name, "runningboardd") && strcmp(name, "audiomxd") &&
                            strcmp(name, "SpringBoard") && strcmp(name, "backboardd") && strcmp(name, "lsd") &&
                            strcmp(name, "trustd") && strcmp(name, "nehelper") && strcmp(name, "pkd") && strcmp(name, "marble-debug")) return true;
                        char *message = format(entry->log_message);
                        if (message) { uint64_t timestamp; memcpy(&timestamp, entry->log_message + 8, sizeof(timestamp)); [text appendFormat:@"%llu %s[%d]: %s\n", (unsigned long long)timestamp, name, entry->pid, message]; free(message); count++; }
                        if (count >= 10000 || text.length >= (2u << 20)) { accepting = false; dispatch_semaphore_signal(done); return false; }
                    }
                    return true;
                }
            });
        if (stream) {
            events(stream, ^(os_activity_stream_t source, os_activity_stream_event_t event) {
                (void)source;
                if (event == OS_ACTIVITY_STREAM_EVENT_FAILED || event == OS_ACTIVITY_STREAM_EVENT_STOPPED) {
                    @synchronized (text) { if (event == OS_ACTIVITY_STREAM_EVENT_FAILED) failure = EIO; }
                    dispatch_semaphore_signal(done);
                }
            });
            resume(stream);
            dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 12LL * NSEC_PER_SEC));
            @synchronized (text) { accepting = false; }
            cancel(stream);
        } else failure = errno ?: EIO;
    } else failure = ENOSYS;
    NSData *data;
    @synchronized (text) {
        [text appendFormat:@"\nEntries: %u; stream_error: %d; create=%d resume=%d cancel=%d format=%d events=%d\n", count, failure, create != NULL, resume != NULL, cancel != NULL, format != NULL, events != NULL];
        data = [text dataUsingEncoding:NSUTF8StringEncoding];
    }
    size_t size = MIN(data.length, (size_t)(4u << 20));
    void *copy = malloc(size + 1);
    if (!copy) return ENOMEM;
    memcpy(copy, data.bytes, size); ((char *)copy)[size] = 0;
    *bytes = copy; *length = size;
    // Keep the image mapped while cancellation retires its callbacks.
    return failure;
}

/* Boot network record: wifid, configd and IP configuration messages from agent
 * start for 900 seconds, kept in memory and returned by GET /network-boot.log. */
static NSMutableData *bootNetworkLog;
static os_activity_stream_t bootNetworkStream;
static dispatch_queue_t bootNetworkQueue;

void marble_start_boot_network_log(void)
{
    void *library = dlopen("/System/Library/PrivateFrameworks/LoggingSupport.framework/LoggingSupport", RTLD_LAZY | RTLD_LOCAL);
    os_activity_stream_for_pid_t create = library ? (os_activity_stream_for_pid_t)dlsym(library, "os_activity_stream_for_pid") : NULL;
    os_activity_stream_resume_t resume = library ? (os_activity_stream_resume_t)dlsym(library, "os_activity_stream_resume") : NULL;
    os_activity_stream_cancel_t cancel = library ? (os_activity_stream_cancel_t)dlsym(library, "os_activity_stream_cancel") : NULL;
    os_log_copy_formatted_message_t format = library ? (os_log_copy_formatted_message_t)dlsym(library, "os_log_copy_formatted_message") : NULL;
    bootNetworkLog = [NSMutableData dataWithCapacity:1u << 20];
    bootNetworkQueue = dispatch_queue_create("org.unixnu.marble.network-boot-log", DISPATCH_QUEUE_SERIAL);
    if (!create || !resume || !cancel || !format) {
        [bootNetworkLog appendData:[@"Boot network log: LoggingSupport interface unavailable\n" dataUsingEncoding:NSUTF8StringEncoding]];
        return;
    }
    const uint64_t began = clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW);
    bootNetworkStream = create(-1, OS_ACTIVITY_STREAM_HISTORICAL | OS_ACTIVITY_STREAM_INFO | OS_ACTIVITY_STREAM_DEBUG | OS_ACTIVITY_STREAM_NO_SENSITIVE,
        ^bool(os_activity_stream_entry_t entry, int error) {
            if (error || !entry || entry->type != OS_ACTIVITY_STREAM_TYPE_LOG_MESSAGE || !entry->proc_imagepath) return !error;
            const char *name = strrchr(entry->proc_imagepath, '/'); name = name ? name + 1 : entry->proc_imagepath;
            if (strcmp(name, "wifid") && strcmp(name, "configd") && strcmp(name, "IPConfiguration") &&
                strcmp(name, "symptomsd") && strcmp(name, "nehelper") && strcmp(name, "SpringBoard")) return true;
            char *message = format(entry->log_message);
            if (!message) return true;
            if (!strcmp(name, "SpringBoard") && !strcasestr(message, "wifi") && !strcasestr(message, "wi-fi")) { free(message); return true; }
            uint64_t timestamp; memcpy(&timestamp, entry->log_message + 8, sizeof(timestamp));
            NSString *line = [NSString stringWithFormat:@"%llu %s[%d]: %s\n", (unsigned long long)timestamp, name, entry->pid, message];
            free(message);
            NSData *bytes = [line dataUsingEncoding:NSUTF8StringEncoding];
            bool more = clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW) - began < 900ull * NSEC_PER_SEC;
            @synchronized (bootNetworkLog) {
                if (bootNetworkLog.length + bytes.length <= (12u << 20)) [bootNetworkLog appendData:bytes];
                else more = false;
            }
            if (!more) dispatch_async(bootNetworkQueue, ^{ if (bootNetworkStream) { cancel(bootNetworkStream); bootNetworkStream = NULL; } });
            return more;
        });
    if (bootNetworkStream) resume(bootNetworkStream);
    else [bootNetworkLog appendData:[@"Boot network log: stream creation failed\n" dataUsingEncoding:NSUTF8StringEncoding]];
}

int marble_copy_boot_network_log(void **bytes, size_t *length)
{
    if (!bytes || !length) return EINVAL;
    *bytes = NULL; *length = 0;
    if (!bootNetworkLog) return ENOENT;
    @synchronized (bootNetworkLog) {
        size_t size = bootNetworkLog.length;
        void *copy = malloc(size + 1);
        if (!copy) return ENOMEM;
        memcpy(copy, bootNetworkLog.bytes, size); ((char *)copy)[size] = 0;
        *bytes = copy; *length = size;
    }
    return 0;
}
